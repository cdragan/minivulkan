// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_KHR_shader_subgroup_basic: require
#extension GL_KHR_shader_subgroup_arithmetic: require

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 1) const uint subgroup_size = 32;

layout(constant_id = 2) const uint num_taps = 1025;

struct FIRParams {
    uint taps_offs;             // Offset of FIR filter's taps

    uint highpass_cutoff_freq;  // High pass frequency (if 0, it's a pure low pass)
    uint lowpass_cutoff_freq;   // Low pass frequency (if 0, it's a pure high pass)
    uint window_span;           // Effective odd window span in taps, 11..num_taps (num_taps = full sharpness)
};

layout(binding = 0) writeonly buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer param_buf { FIRParams params[]; };

layout(push_constant) uniform global_param_buf {
    uint sampling_freq;
};

shared float coefficients[num_taps];
shared float norm_sum[work_group_size / subgroup_size];
shared float norm_cos[work_group_size / subgroup_size];
shared float norm_sin[work_group_size / subgroup_size];

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
    const float lowpass_norm_cutoff  = float(param.lowpass_cutoff_freq)  / float(sampling_freq);
    const float highpass_norm_cutoff = float(param.highpass_cutoff_freq) / float(sampling_freq);
    const uint  center               = (num_taps - 1) / 2;
    const bool  passthrough          = param.lowpass_cutoff_freq == 0 && param.highpass_cutoff_freq == 0;
    const bool  bandpass             = param.highpass_cutoff_freq > 0 && param.highpass_cutoff_freq < param.lowpass_cutoff_freq;
    const float center_freq          = (float(param.highpass_cutoff_freq) + float(param.lowpass_cutoff_freq)) * 0.5;
    const float center_w             = 2.0 * pi * center_freq / float(sampling_freq);
    float       coeff_sum            = 0;
    float       cos_sum              = 0;
    float       sin_sum              = 0;

    for (uint tap = gl_LocalInvocationID.x; tap < num_taps; tap += work_group_size) {

        const float x = float(tap) - float(center);

        // Low pass coefficient
        float value = (x == 0.0 && passthrough) ? 1.0 : 0.0;
        if (param.lowpass_cutoff_freq > 0) {
            value = sinc_lowpass(x, lowpass_norm_cutoff);
        }

        float norm_value = value;

        // High pass, band pass or band stop coefficient
        if (param.highpass_cutoff_freq > 0) {
            const float hp_value = sinc_lowpass(x, highpass_norm_cutoff);
            value -= hp_value;

            // Band pass
            if (param.highpass_cutoff_freq < param.lowpass_cutoff_freq) {
                value      = -value;
                norm_value = hp_value;
            }
            // High pass or band stop
            else if (x == 0.0) {
                value += 1.0;

                if (param.lowpass_cutoff_freq == 0) {
                    norm_value += 1.0;
                }
            }
        }

        //const float window = 0.54 - 0.46 * cos((2 * pi * tap) / (num_taps - 1)); // Hamming

        // Blackman over the effective span: taps outside the span are exactly
        // zero, and at the full span (window_span == num_taps) this is the
        // fixed-window expression bit for bit.
        const float half_span = float(param.window_span) * 0.5 - 0.5;  // (window_span - 1) / 2
        const float wpos      = x + half_span;                         // 0..window_span - 1 inside the span
        const float span_m1   = float(param.window_span - 1u);
        const bool  in_span   = wpos >= 0.0 && wpos <= span_m1;
        const float window    = in_span
            ? (0.42 - 0.5 * cos((2 * pi * wpos) / span_m1) + 0.08 * cos((4 * pi * wpos) / span_m1))
            : 0.0;
        value *= window;

        coefficients[tap] = value;

        coeff_sum += norm_value * window;

        if (bandpass && in_span) {
            cos_sum += value * cos(center_w * x);
            sin_sum += value * sin(center_w * x);
        }
    }

    // Normalize coefficients
    coeff_sum = subgroupAdd(coeff_sum);
    if (subgroupElect()) {
        norm_sum[gl_SubgroupID] = coeff_sum;
    }
    if (bandpass) {
        cos_sum = subgroupAdd(cos_sum);
        sin_sum = subgroupAdd(sin_sum);
        if (subgroupElect()) {
            norm_cos[gl_SubgroupID] = cos_sum;
            norm_sin[gl_SubgroupID] = sin_sum;
        }
    }

    barrier();

    if (gl_SubgroupID == 0) {
        coeff_sum = 0;
        if (gl_SubgroupInvocationID < (work_group_size / gl_SubgroupSize)) {
            coeff_sum = norm_sum[gl_SubgroupInvocationID];
        }

        coeff_sum = subgroupAdd(coeff_sum);

        if (bandpass) {
            cos_sum = 0;
            sin_sum = 0;
            if (gl_SubgroupInvocationID < (work_group_size / gl_SubgroupSize)) {
                cos_sum = norm_cos[gl_SubgroupInvocationID];
                sin_sum = norm_sin[gl_SubgroupInvocationID];
            }

            cos_sum = subgroupAdd(cos_sum);
            sin_sum = subgroupAdd(sin_sum);

            if (subgroupElect()) {
                const float q           = cos_sum * cos_sum + sin_sum * sin_sum;
                const float m           = sqrt(q);
                float       denominator = m;
                if (m == 0.0 || isnan(cos_sum) || isnan(sin_sum) || isinf(cos_sum) || isinf(sin_sum) || isnan(q) || isinf(q) || isnan(m)) {
                    denominator = coeff_sum;
                }
                if (denominator == 0.0 || isnan(denominator) || isinf(denominator)) {
                    denominator = 1.0;
                }
                norm_sum[0] = denominator;
            }
        } else if (subgroupElect()) {
            norm_sum[0] = coeff_sum;
        }
    }

    barrier();

    for (uint tap = gl_LocalInvocationID.x; tap < num_taps; tap += work_group_size) {
        data[param.taps_offs + tap] = coefficients[tap] / norm_sum[0];
    }
}
