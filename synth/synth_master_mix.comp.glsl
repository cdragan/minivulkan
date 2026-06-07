// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

// Smooth volume adjustment to avoid glitches
layout(constant_id = 3) const uint volume_adjustment_samples = 32;

struct InputParams {
    uint  in_sound_offs;
    float old_volume;
    float volume;
    float old_panning;
    float panning;
};

struct Params {
    uint out_sound_offs;
    uint input_params_offs;
    uint num_inputs;
};

layout(binding = 0) buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer input_param_buf { InputParams input_params[]; };

layout(binding = 2, std430) readonly buffer param_buf { Params params[]; };

void main()
{
    const Params param = params[gl_WorkGroupID.x];

    float left_value  = 0;
    float right_value = 0;

    for (uint i = 0; i < param.num_inputs; i++) {
        const InputParams input_param = input_params[param.input_params_offs + i];

        // Read interleaved-stereo input channel data
        float left_in  = data[input_param.in_sound_offs + gl_LocalInvocationID.x * 2];
        float right_in = data[input_param.in_sound_offs + gl_LocalInvocationID.x * 2 + 1];

        // Calculate multipliers with smooth adjustment
        float multiplier;
        float panning;
        if (gl_LocalInvocationID.x < volume_adjustment_samples) {
            const float step = float(gl_LocalInvocationID.x + 1) / float(volume_adjustment_samples);
            multiplier = mix(input_param.old_volume,  input_param.volume,  step);
            panning    = mix(input_param.old_panning, input_param.panning, step);
        }
        else {
            multiplier = input_param.volume;
            panning    = input_param.panning;
        }

        // Channel-level pan is a linear balance law that is transparent at
        // center (pan 0.5 -> both gains 1.0) so a single centered channel
        // passes through unchanged.  The per-oscillator stereo balance is
        // already applied by synth_chan_combine, so this only attenuates the
        // opposite side as the channel pans toward an extreme.
        const float left_gain  = min(1.0, 2.0 * (1.0 - panning));
        const float right_gain = min(1.0, 2.0 * panning);

        left_value  += multiplier * left_gain  * left_in;
        right_value += multiplier * right_gain * right_in;
    }

    data[param.out_sound_offs + gl_LocalInvocationID.x * 2]     = left_value;
    data[param.out_sound_offs + gl_LocalInvocationID.x * 2 + 1] = right_value;
}
