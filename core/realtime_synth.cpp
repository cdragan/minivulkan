// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "realtime_synth.h"
#include "../core/d_printf.h"
#include "../core/minivulkan.h"
#include "../core/mstdc.h"
#include "../core/resource.h"
#include "../core/vmath.h"
#include <math.h>

namespace {
    Buffer host_audio_output_buf;

    CommandBuffers<1> audio_cmd_buf;

    constexpr uint32_t rt_step_samples = 256;
}

bool init_real_time_synth_os();

bool init_real_time_synth()
{
    if (compute_family_index == no_queue_family) {
        d_printf("No async compute queue available for synth\n");
        return false;
    }

    if ( ! init_real_time_synth_os())
        return false;

    // TODO - use project-dependent audio length
    constexpr uint32_t seconds = 1;
    constexpr uint32_t sample_size = sizeof(float);
    constexpr VkDeviceSize buf_size = mstd::align_up(rt_sampling_rate * 2U * sample_size * seconds, rt_step_samples);

    if ( ! host_audio_output_buf.allocate(Usage::host_only,
                                          buf_size,
                                          VK_FORMAT_UNDEFINED,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          { "host audio buffer" }))
        return false;

    if ( ! allocate_command_buffers_once(&audio_cmd_buf, 1, compute_family_index))
        return false;

    return true;
}

enum WaveType : uint32_t {
    // Wave disabled - for WaveState that can only be wave_type[1]
    no_wave,
    // Normal sine wave, duty is ignored
    sine_wave,
    // Sawtooth wave
    //
    // |\  duty=0    /\ duty=0.5           duty=1  /|
    // | \          /  \ (triangle wave)          / |
    // |  \             \  /                     /  |
    // |   \             \/                     /   |
    sawtooth_wave,
    // Pulse wave
    //
    // | duty=0   +--+ duty=0.5        duty=1 |
    // |          |  | (square wave)          |
    // |             |  |                     |
    // +---          +--+              -------+
    pulse_wave,
    // White noise
    noise_wave
};

using ParamIndex = uint32_t;

struct WaveState {
    WaveType   wave_type[2];
    ParamIndex duty[2];     // Duty for sawtooth or pulse
    ParamIndex wave_mix;    // Amount of mixing between 2 waves
    ParamIndex amplitude;   // Wave amplitude
    ParamIndex pitch_adj;   // Pitch adjustment
    float      pitch_base;  // Base pitch from note
    float      phase;       // Current phase
};

enum ParamType : uint32_t {
    const_param,            // Constant, fixed value
    adsr_param,             // Envelope
    lfo_param               // Low frequency oscillator
};

struct ParamState {
    ParamIndex param;       // Index of global parameter description/config
    float      value;       // Current value (e.g. LFO)
    float      phase;       // Current phase step for ADSR/LFO
};

struct Parameter {
    ParamType param_type;
    union {
        float        const_value;
        struct {
            uint32_t duration_ms[4]; // Attack, decay, sustain, release
            float    value[4];       // Init, peak, sustain, end
        } adsr;
        struct {
            WaveType wave_type;
            float    duty;
            uint32_t freq_hz;
            float    value[2];  // Min, max
        } lfo;
    } param;
};

// TODO filter

// Template boilerplate to support different output audio buffer types,
// for example float non-interleaved or int16_t interleaved.

template<typename T, bool interleaved>
struct StereoPtr {
    T* left;
    T* right;

    StereoPtr<T, interleaved>& operator+=(size_t offset) {
        left  += offset;
        right += offset;
        return *this;
    }

    static StereoPtr<T, interleaved> from_buffer(Buffer& buffer) {
        T* const ptr = buffer.get_ptr<T>();
        const size_t size = buffer.size();
        return { ptr, ptr + (size / (2 * sizeof(T))) };
    }
};

template<typename T>
static StereoPtr<T, false> operator+(StereoPtr<T, false> ptr, size_t offset)
{
    return { ptr.left + offset, ptr.right + offset };
}

template<typename T>
struct StereoPtr<T, true> {
    T* data;

    StereoPtr<T, true>& operator+=(size_t offset) {
        data += offset;
        return *this;
    }

    static StereoPtr<T, true> from_buffer(Buffer& buffer) {
        return { buffer.get_ptr<T>() };
    }
};

template<typename T>
static StereoPtr<T, true> operator+(StereoPtr<T, true> ptr, size_t offset)
{
    return { ptr.data + offset };
}

template<typename T>
static void copy_audio_data(StereoPtr<T, false> dest, StereoPtr<T, false> src, uint32_t num_samples)
{
    mstd::mem_copy(dest.left,  src.left,  num_samples * sizeof(T));
    mstd::mem_copy(dest.right, src.right, num_samples * sizeof(T));
}

template<typename T>
static void copy_audio_data(StereoPtr<T, true> dest, StereoPtr<T, true> src, uint32_t num_samples)
{
    mstd::mem_copy(dest.data, src.data, num_samples * 2 * sizeof(T));
}

template<typename T, bool interleaved>
static void render_audio(uint32_t num_samples)
{
    assert(num_samples % rt_step_samples == 0 && num_samples > 0);

    VkResult res = CHK(vkResetCommandBuffer(audio_cmd_buf, 0));
    if (res != VK_SUCCESS)
        return;

    static VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };

    res = CHK(vkBeginCommandBuffer(audio_cmd_buf, &begin_info));
    if (res != VK_SUCCESS)
        return;

    // TODO 256 samples at a time

    res = CHK(vkEndCommandBuffer(audio_cmd_buf));
    if (res != VK_SUCCESS)
        return;

    static const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0,                  // waitSemaphoreCount
        nullptr,            // pWaitSemaphored
        &dst_stage,         // pWaitDstStageMask
        1,                  // commandBufferCount
        audio_cmd_buf.bufs,
        0,                  // signalSemaphoreCount
        nullptr             // pSignalSemaphores
    };

    res = CHK(vkQueueSubmit(vk_compute_queue, 1, &submit_info, vk_fens[fen_compute]));
    if (res != VK_SUCCESS)
        return;

    if ( ! wait_and_reset_fence(fen_compute))
        return;

    host_audio_output_buf.invalidate();

    const auto buf_ptr = StereoPtr<T, interleaved>::from_buffer(host_audio_output_buf);
    for (uint32_t i = 0; i < num_samples; i++) {
        static float phase = 0;
        static const float frequency = 440.0f;

        buf_ptr.left[i]  = sinf(phase);
        buf_ptr.right[i] = sinf(phase + vmath::pi);

        phase += 2.0f * vmath::pi * frequency / rt_sampling_rate;

        if (phase > 2.0f * vmath::pi)
            phase -= 2.0f * vmath::pi;
    }
}

template<typename T, bool interleaved>
static void render_audio_buffer(StereoPtr<T, interleaved> output_buf, uint32_t num_samples)
{
    static uint32_t consumed_samples;
    static uint32_t remaining_samples;

    const auto rendered_src = StereoPtr<T, interleaved>::from_buffer(host_audio_output_buf);

    if (remaining_samples) {
        const uint32_t to_copy = mstd::min(remaining_samples, num_samples);

        copy_audio_data(output_buf, rendered_src + consumed_samples, to_copy);

        output_buf        += to_copy;
        num_samples       -= to_copy;
        remaining_samples -= to_copy;
        consumed_samples  += to_copy;

        if (remaining_samples)
            return;
    }

    const uint32_t to_render = mstd::align_up(num_samples, rt_step_samples);
    render_audio<T, interleaved>(to_render);

    const uint32_t to_copy = mstd::min(to_render, num_samples);
    copy_audio_data(output_buf, rendered_src, to_copy);

    if (to_render > to_copy) {
        consumed_samples  = to_copy;
        remaining_samples = to_render - to_copy;
    }
}

void render_audio_buffer(uint32_t num_frames,
                         float*   left_channel,
                         float*   right_channel)
{
    if (num_frames) {
        const StereoPtr<float, false> channels = { left_channel, right_channel };
        render_audio_buffer(channels, num_frames);
    }
}
