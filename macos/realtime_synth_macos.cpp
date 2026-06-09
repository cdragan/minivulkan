// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../synth/realtime_synth.h"
#include "../core/d_printf.h"
#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <pthread.h>
#include <time.h>

// Realtime synth output through the AUHAL (Output AudioUnit) C API.  This avoids
// Objective-C so the same implementation can serve both the GUI build and the
// Objective-C-free nogui build.

static AudioComponentInstance output_unit;
static uint64_t               saved_timestamp_ms;

static pthread_t         audio_producer_thread;
static std::atomic<bool> audio_producer_running;

static void* audio_producer_main(void*)
{
    while (audio_producer_running.load(std::memory_order_relaxed)) {
        if ( ! Synth::produce_audio_batch<float, false>()) {
            constexpr uint32_t        sleep_ns   = 1'000'000;
            constexpr struct timespec sleep_time = { 0, sleep_ns };
            nanosleep(&sleep_time, nullptr);
        }
    }

    return nullptr;
}

static OSStatus render_callback(void*                       in_ref_con,
                                AudioUnitRenderActionFlags* io_action_flags,
                                const AudioTimeStamp*       in_timestamp,
                                UInt32                      in_bus_number,
                                UInt32                      in_number_frames,
                                AudioBufferList*            io_data)
{
    if (io_data->mNumberBuffers != 2)
        return noErr;

    if (in_timestamp->mFlags & kAudioTimeStampSampleTimeValid)
        saved_timestamp_ms = static_cast<uint64_t>(in_timestamp->mSampleTime) * 1000U / Synth::rt_sampling_rate;

    Synth::consume_audio<float, false>(in_number_frames,
                                       static_cast<float*>(io_data->mBuffers[0].mData),
                                       static_cast<float*>(io_data->mBuffers[1].mData));

    return noErr;
}

namespace Synth {

bool init_synth_os()
{
    static const AudioComponentDescription output_desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_DefaultOutput,
        kAudioUnitManufacturer_Apple,
        0,
        0
    };

    AudioComponent component = AudioComponentFindNext(nullptr, &output_desc);
    if ( ! component) {
        d_printf("Failed to find output audio component\n");
        return false;
    }

    if (AudioComponentInstanceNew(component, &output_unit) != noErr) {
        d_printf("Failed to create output audio unit\n");
        return false;
    }

    // Non-interleaved stereo float, matching render_audio_buffer's two channels.
    AudioStreamBasicDescription stream_format = { };
    stream_format.mSampleRate       = rt_sampling_rate;
    stream_format.mFormatID         = kAudioFormatLinearPCM;
    stream_format.mFormatFlags      = kAudioFormatFlagIsFloat
                                    | kAudioFormatFlagIsPacked
                                    | kAudioFormatFlagIsNonInterleaved;
    stream_format.mFramesPerPacket  = 1;
    stream_format.mChannelsPerFrame = 2;
    stream_format.mBitsPerChannel   = 32;
    stream_format.mBytesPerFrame    = sizeof(float);
    stream_format.mBytesPerPacket   = sizeof(float);

    if (AudioUnitSetProperty(output_unit,
                             kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input,
                             0,
                             &stream_format,
                             sizeof(stream_format)) != noErr) {
        d_printf("Failed to set output audio stream format\n");
        return false;
    }

    AURenderCallbackStruct callback = { };
    callback.inputProc       = render_callback;
    callback.inputProcRefCon = nullptr;

    if (AudioUnitSetProperty(output_unit,
                             kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input,
                             0,
                             &callback,
                             sizeof(callback)) != noErr) {
        d_printf("Failed to set output audio render callback\n");
        return false;
    }

    if (AudioUnitInitialize(output_unit) != noErr) {
        d_printf("Failed to initialize output audio unit\n");
        return false;
    }

    // Start the producer before the unit so the ring is prefilled when audio begins.
    audio_producer_running.store(true, std::memory_order_relaxed);
    if (pthread_create(&audio_producer_thread, nullptr, audio_producer_main, nullptr) != 0) {
        audio_producer_running.store(false, std::memory_order_relaxed);
        d_printf("Failed to start audio producer thread\n");
        return false;
    }

    if (AudioOutputUnitStart(output_unit) != noErr) {
        d_printf("Failed to start output audio unit\n");
        return false;
    }

    return true;
}

void stop_synth_os()
{
    if (output_unit) {
        AudioOutputUnitStop(output_unit);
    }

    // Stop the producer after the callbacks, then join before any teardown.
    if (audio_producer_running.exchange(false, std::memory_order_relaxed)) {
        pthread_join(audio_producer_thread, nullptr);
    }
}

uint64_t get_current_timestamp_ms()
{
    return saved_timestamp_ms;
}

} // namespace Synth
