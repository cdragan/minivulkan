// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

// Smooth volume adjustment to avoid glitches
layout(constant_id = 2) const uint volume_adjustment_samples = 32;

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

layout(set = 0, binding = 0) buffer data_buf { float data[]; };

layout(set = 1, binding = 0, std430) readonly buffer input_param_buf { InputParams input_params[]; };

layout(set = 1, binding = 1, std430) readonly buffer param_buf { Params params[]; };

void main()
{
    const Params param = params[gl_WorkGroupID.x];

    float left_value  = 0;
    float right_value = 0;

    for (uint i = 0; i < param.num_inputs; i++) {
        const InputParams input_param = input_params[param.input_params_offs + i];

        // Read input sound data
        float value = data[input_param.in_sound_offs + gl_LocalInvocationID.x];

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

        // Apply volume
        value *= multiplier;

        // Apply panning
        const float theta = panning * 1.570796326794897;
        left_value  += cos(theta) * value;
        right_value += sin(theta) * value;
    }

    data[param.out_sound_offs + gl_LocalInvocationID.x * 2]     = left_value;
    data[param.out_sound_offs + gl_LocalInvocationID.x * 2 + 1] = right_value;
}
