// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"
#include "../synth/realtime_synth.h"
#include <alsa/asoundlib.h>
#include <atomic>
#include <errno.h>
#include <pthread.h>
#include <time.h>

namespace {
static snd_pcm_t*            pcm;
static pthread_t             playback_thread;
static pthread_t             producer_thread;
static bool                  playback_created;
static bool                  producer_created;
static std::atomic<bool>     playback_running;
static std::atomic<bool>     producer_running;
static std::atomic<uint64_t> timestamp_ms;

static constexpr uint32_t block_frames = 512;
static constexpr timespec sleep_time   = { 0, 1'000'000 };
static float              left[block_frames];
static float              right[block_frames];
} // namespace

static void* audio_producer_main(void*)
{
    while (producer_running.load(std::memory_order_relaxed)) {
        if (! Synth::produce_audio_batch<float, false>()) {
            nanosleep(&sleep_time, nullptr);
        }
    }

    return nullptr;
}

static void* audio_playback_main(void*)
{
    uint32_t pending         = 0;
    uint32_t offset          = 0;
    uint64_t accepted_frames = 0;
    uint64_t played_frames   = 0;

    while (playback_running.load(std::memory_order_relaxed)) {
        int result = snd_pcm_wait(pcm, 10);
        if (result == 0 || result == -EINTR || result == -EAGAIN) {
            continue;
        }

        if (result > 0) {
            if (! pending) {
                Synth::consume_audio<float, false>(block_frames, left, right);
                pending = block_frames;
                offset  = 0;
            }

            void*                   channels[2] = { left + offset, right + offset };
            const snd_pcm_sframes_t written     = snd_pcm_writen(pcm, channels, pending);
            if (written >= 0) {
                pending -= static_cast<uint32_t>(written);
                offset += static_cast<uint32_t>(written);
                accepted_frames += static_cast<uint64_t>(written);

                snd_pcm_sframes_t delay = 0;
                result                  = snd_pcm_delay(pcm, &delay);
                if (result >= 0) {
                    const uint64_t queued   = delay > 0 ? static_cast<uint64_t>(delay) : 0;
                    const uint64_t position = accepted_frames > queued ? accepted_frames - queued : 0;
                    if (position > played_frames) {
                        played_frames = position;
                    }

                    timestamp_ms.store(played_frames * 1000 / Synth::rt_sampling_rate, std::memory_order_relaxed);
                    continue;
                }
            }
            else {
                result = static_cast<int>(written);
            }
        }

        if (result == -EINTR || result == -EAGAIN) {
            continue;
        }

        if (result == -EPIPE || result == -ESTRPIPE) {
            result = snd_pcm_prepare(pcm);
            if (result >= 0) {
                // Do not count discarded device frames as played after a reset.
                accepted_frames = played_frames;
                pending         = 0;
                continue;
            }
        }

        d_printf("ALSA audio stream failed: %s\n", snd_strerror(result));
        break;
    }

    return nullptr;
}

bool Synth::init_synth_os()
{
    if (! init_midi_os()) {
        d_printf("Live MIDI input unavailable\n");
    }

    timestamp_ms.store(0, std::memory_order_relaxed);

    int result = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (result < 0) {
        d_printf("Failed to open ALSA audio: %s\n", snd_strerror(result));
        stop_synth_os();
        return false;
    }

    // The default plug layer converts this format to the device format.
    result = snd_pcm_set_params(pcm,
                                SND_PCM_FORMAT_FLOAT_LE,
                                SND_PCM_ACCESS_RW_NONINTERLEAVED,
                                2,
                                rt_sampling_rate,
                                1,
                                30'000);
    if (result < 0) {
        d_printf("Failed to set ALSA audio format: %s\n", snd_strerror(result));
        stop_synth_os();
        return false;
    }

    producer_running.store(true, std::memory_order_relaxed);
    result = pthread_create(&producer_thread, nullptr, audio_producer_main, nullptr);
    if (result != 0) {
        d_printf("Failed to start audio producer thread: %d\n", result);
        stop_synth_os();
        return false;
    }

    producer_created = true;

    // Bound the wait because the producer can fail to fill the ring.
    const AudioRingStatus ring   = get_audio_ring_status();
    const uint32_t        target = ring.lead_frames < ring.max_fill_frames ? ring.lead_frames : ring.max_fill_frames;

    for (uint32_t waited_ms = 0; waited_ms < 2'000; waited_ms++) {
        if (get_audio_ring_status().fill_frames >= target) {
            break;
        }

        nanosleep(&sleep_time, nullptr);
    }

    playback_running.store(true, std::memory_order_relaxed);
    result = pthread_create(&playback_thread, nullptr, audio_playback_main, nullptr);
    if (result != 0) {
        d_printf("Failed to start audio playback thread: %d\n", result);
        stop_synth_os();
        return false;
    }

    playback_created = true;

    return true;
}

void Synth::stop_synth_os()
{
    stop_midi_os();

    playback_running.store(false, std::memory_order_relaxed);
    if (playback_created) {
        pthread_join(playback_thread, nullptr);
        playback_created = false;
    }

    producer_running.store(false, std::memory_order_relaxed);
    if (producer_created) {
        pthread_join(producer_thread, nullptr);
        producer_created = false;
    }

    if (pcm) {
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
        pcm = nullptr;
    }
}

uint64_t Synth::get_current_timestamp_ms()
{
    return timestamp_ms.load(std::memory_order_relaxed);
}
