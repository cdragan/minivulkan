// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"
#include "../synth/realtime_synth.h"
#include <alsa/asoundlib.h>
#include <atomic>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
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
static float              out_buf[block_frames * 2];
} // namespace

static void* audio_producer_main(void*)
{
    while (producer_running.load(std::memory_order_relaxed)) {
        if (! Synth::produce_audio_batch<float, true>()) {
            nanosleep(&sleep_time, nullptr);
        }
    }

    return nullptr;
}

static void* audio_playback_main(void*)
{
    uint64_t accepted_frames = 0;
    uint64_t played_frames   = 0;

    while (playback_running.load(std::memory_order_relaxed)) {
        Synth::consume_audio<float, true>(block_frames, out_buf, nullptr);

        const snd_pcm_sframes_t written = snd_pcm_writei(pcm, out_buf, block_frames);

        if (written >= 0) {
            accepted_frames += static_cast<uint64_t>(written);

            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(pcm, &delay) >= 0) {
                const uint64_t queued   = delay > 0 ? static_cast<uint64_t>(delay) : 0;
                const uint64_t position = accepted_frames > queued ? accepted_frames - queued : 0;
                if (position > played_frames) {
                    played_frames = position;
                }
            }

            timestamp_ms.store(played_frames * 1000 / Synth::rt_sampling_rate, std::memory_order_relaxed);
            continue;
        }

        const int result = static_cast<int>(written);
        if (result == -EINTR) {
            continue;
        }

        // Handles -EPIPE (underrun) and -ESTRPIPE (suspend), passes anything else through.
        if (snd_pcm_recover(pcm, result, 0) >= 0) {
            d_printf("ALSA audio: recovered from %s\n", snd_strerror(result));

            // Do not count discarded device frames as played after a reset.
            accepted_frames = played_frames;
            continue;
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

    const char* device = getenv("VK_ALSA_DEVICE");
    if (! device || ! *device) {
        device = "default";
    }

    int result = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (result < 0) {
        d_printf("Failed to open ALSA audio \"%s\": %s\n", device, snd_strerror(result));
        stop_synth_os();
        return false;
    }

    result = snd_pcm_set_params(pcm,
                                SND_PCM_FORMAT_FLOAT_LE,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                2,
                                rt_sampling_rate,
                                1,
                                30'000);
    if (result < 0) {
        d_printf("Failed to set ALSA audio format: %s\n", snd_strerror(result));
        stop_synth_os();
        return false;
    }

    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_current(pcm, hw_params);

    snd_pcm_uframes_t buffer_size = 0;
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);

    snd_pcm_sw_params_t* sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(pcm, sw_params);

    snd_pcm_uframes_t start_threshold = 0;
    snd_pcm_uframes_t avail_min       = 0;
    snd_pcm_sw_params_get_start_threshold(sw_params, &start_threshold);
    snd_pcm_sw_params_get_avail_min(sw_params, &avail_min);

    d_printf("ALSA audio: opened \"%s\", buffer %lu, start_threshold %lu, avail_min %lu\n",
             device,
             buffer_size,
             start_threshold,
             avail_min);

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

    if (get_audio_ring_status().fill_frames < target) {
        d_printf("ALSA audio: ring is not filling, the synth is not rendering audio\n");
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
