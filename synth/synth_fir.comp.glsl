// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(local_size_x = 256) in;

layout(constant_id = 0) const uint num_taps = 1025;

struct FIRParams {
    uint fir_memory_offs;
    uint taps_offs;
    uint in_sound_offs;
    uint out_sound_offs;
};

layout(set = 0, binding = 0) buffer data_buf { float data[]; };

layout(set = 1, binding = 0, std430) readonly buffer param_buf { FIRParams params[]; };

shared float cached_input[num_taps - 1 + gl_WorkGroupSize.x];

void main()
{
    const FIRParams param = params[gl_WorkGroupID.x];

    // Read previously saved inputs
    for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += gl_WorkGroupSize.x) {
        cached_input[tap] = data[param.fir_memory_offs + tap];
    }

    // Read new inputs
    const uint base_pos = num_taps - 1 + gl_LocalInvocationID.x;
    cached_input[base_pos] = data[param.in_sound_offs + gl_LocalInvocationID.x];

    barrier();

    // Save current inputs for next invocation
    for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += gl_WorkGroupSize.x) {
        data[param.fir_memory_offs + tap] = cached_input[gl_WorkGroupSize.x + tap];
    }

    // Apply FIR filter convolution
    float value = 0;

    for (uint tap = 0; tap < num_taps; tap++) {
        value += data[param.taps_offs + tap] * cached_input[base_pos - tap];
    }

    data[param.out_sound_offs + gl_LocalInvocationID.x] = value;
}
