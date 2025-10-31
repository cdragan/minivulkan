// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

#extension GL_KHR_shader_subgroup_basic: require
#extension GL_KHR_shader_subgroup_arithmetic: require

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 1) const uint num_taps = 1025;

struct FIRParams {
    uint taps_offs;             // Offset of FIR filter's taps

    uint highpass_cutoff_freq;  // High pass frequency (if 0, it's a pure low pass)
    uint lowpass_cutoff_freq;   // Low pass frequency (if 0, it's a pure high pass)
};

layout(binding = 0) writeonly buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer param_buf { FIRParams params[]; };

layout(push_constant) uniform global_param_buf {
    uint sampling_freq;
};

shared float coefficients[num_taps];
shared float norm_sum[work_group_size / 32];

const float pi = 3.141592653589793;

float sinc_lowpass(float x, float norm_cutoff)
{
    float value;

    if (x == 0.0) {
        value = 2.0 * norm_cutoff;
    } else {
        value = sin(2.0 * pi * norm_cutoff * x) / (pi * x);
    }

    return value;
}

void main()
{
    FIRParams param = params[gl_WorkGroupID.x];

    // Calculate sinc function and apply hamming window
    const float lowpass_norm_cutoff  = float(2 * param.lowpass_cutoff_freq)  / float(sampling_freq);
    const float highpass_norm_cutoff = float(2 * param.highpass_cutoff_freq) / float(sampling_freq);
    const uint  center               = (num_taps - 1) / 2;
    float       coeff_sum            = 0;

    for (uint tap = gl_LocalInvocationID.x; tap < num_taps; tap += work_group_size) {

        const float x = float(tap) - float(center);

        // Low pass coefficient
        float value = 0.0;
        if (param.lowpass_cutoff_freq > 0) {
            value = sinc_lowpass(x, lowpass_norm_cutoff);
        }

        // High pass, band pass or band stop coefficient
        if (param.highpass_cutoff_freq > 0) {
            value -= sinc_lowpass(x, highpass_norm_cutoff);

            // Band pass
            if (param.highpass_cutoff_freq < param.lowpass_cutoff_freq) {
                value = -value;
            }
            // High pass or band stop
            else if (x == 0.0) {
                value += 1.0;
            }
        }

        //const float window = 0.54 - 0.46 * cos((2 * pi * tap) / (num_taps - 1)); // Hamming
        const float window = 0.42 - 0.5 * cos((2 * pi * tap) / (num_taps - 1)) + 0.08 * cos((4 * pi * tap) / (num_taps - 1)); // Blackman
        value *= window;

        coefficients[tap] = value;

        coeff_sum += value;
    }

    // Normalize coefficients
    coeff_sum = subgroupAdd(coeff_sum);
    if (subgroupElect()) {
        norm_sum[gl_SubgroupID] = coeff_sum;
    }

    barrier();

    if (gl_SubgroupID == 0) {
        coeff_sum = 0;
        if (gl_SubgroupInvocationID < (work_group_size / gl_SubgroupSize)) {
            coeff_sum = norm_sum[gl_SubgroupInvocationID];
        }

        coeff_sum = subgroupAdd(coeff_sum);

        if (subgroupElect()) {
            norm_sum[0] = coeff_sum;
        }
    }

    barrier();

    for (uint tap = gl_LocalInvocationID.x; tap < num_taps; tap += work_group_size) {
        data[param.taps_offs + tap] = coefficients[tap] / norm_sum[0];
    }
}
