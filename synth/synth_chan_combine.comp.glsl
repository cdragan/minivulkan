// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(local_size_x = 256) in;

// Smooth volume adjustment to avoid glitches
layout(constant_id = 0) const uint volume_adjustment_samples = 32;

struct InputParams {
    uint  in_sound_offs;
    float old_volume;
    float volume;
    float old_panning;
    float panning;
};

layout(set = 0, binding = 0) buffer data_buf { float data[]; };

layout(set = 1, binding = 0, std430) readonly buffer input_param_buf { InputParams input_params[]; };

layout(push_constant) uniform push {
    uint out_sound_offs;
    uint num_inputs;
};

void main()
{
    float left_value  = 0;
    float right_value = 0;

    for (uint i = 0; i < num_inputs; i++) {
        const InputParams param = input_params[i];

        // Read input sound data
        float value = data[param.in_sound_offs + gl_LocalInvocationID.x];

        // Calculate multipliers with smooth adjustment
        float multiplier;
        float panning;
        if (gl_LocalInvocationID.x < volume_adjustment_samples) {
            const float step = float(gl_LocalInvocationID.x + 1) / float(volume_adjustment_samples);
            multiplier = mix(param.old_volume,  param.volume,  step);
            panning    = mix(param.old_panning, param.panning, step);
        }
        else {
            multiplier = param.volume;
            panning    = param.panning;
        }

        // Apply volume
        value *= multiplier;

        // Apply panning
        const float theta = panning * 1.570796326794897;
        left_value  += cos(theta) * value;
        right_value += sin(theta) * value;
    }

    data[out_sound_offs + gl_LocalInvocationID.x * 2]     = left_value;
    data[out_sound_offs + gl_LocalInvocationID.x * 2 + 1] = right_value;
}
