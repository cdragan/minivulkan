// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "realtime_synth.h"
#include "../core/vmath.h"
#include <math.h>

bool init_real_time_synth_os();

bool init_real_time_synth()
{
    if ( ! init_real_time_synth_os())
        return false;

    // TODO

    return true;
}

void render_audio_buffer(uint32_t sampling_rate,
                         uint32_t num_frames,
                         float*   left_channel,
                         float*   right_channel)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        static float phase = 0;
        static const float frequency = 440.0f;

        left_channel[i]  = sinf(phase);
        right_channel[i] = sinf(phase + vmath::pi);
        phase += 2.0f * vmath::pi * frequency / sampling_rate;
        if (phase > 2.0f * vmath::pi)
            phase -= 2.0f * vmath::pi;
    }
}
