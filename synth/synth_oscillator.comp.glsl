// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 1) const uint num_taps = 1025;

const uint no_wave       = 0;
const uint sine_wave     = 1;
const uint sawtooth_wave = 2;
const uint pulse_wave    = 3;
const uint noise_wave    = 4;

const float two_pi = 6.283185307179586;

struct OscillatorParams {
    uint  out_sound_offs;   // Offset of output sound data
    float phase;            // Initial phase value at first sample to render, 1 is equivalent to wave length
    float phase_step;       // Phase step between samples, derived from oscillator's and sampling frequencies
    uint  osc_type[2];      // Two oscillator types
    float duty[2];          // Duty cycle for sawtooth and pulse oscillator [0..1]
    float osc_mix;          // Mixing between osc_type[0] and osc_type[1] [0..1]

    // Optional filter parameters
    uint  fir_memory_offs;  // Offset of FIR filter's memory
    uint  taps_offs;        // Offset of FIR filter's taps
};

layout(binding = 0) buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer param_buf { OscillatorParams params[]; };

shared float cached_input[num_taps - 1 + work_group_size];

uint pcg_hash(uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float oscillator(uint osc_type, float phase, float duty)
{
    if (osc_type == sine_wave) {
        return sin(two_pi * phase);
    }
    else if (osc_type == sawtooth_wave) {
        phase = fract(phase);

        float value = 2.0 * phase;

        if (duty < 0.0001) {
            value = 2.0 - value;
        }
        else if (duty > 0.999) {
        }
        else if (phase < duty) {
            value /= duty;
        }
        else {
            value = (2.0 - value) / (1.0 - duty);
        }

        return value - 1.0;
    }
    else if (osc_type == pulse_wave) {
        phase = fract(phase);

        return (phase < duty) ? 1.0 : -1.0;
    }
    else { // noise_wave
        const uint uvalue = pcg_hash(uint(floor(phase * 3)));
        return (float(uvalue & 0xFFFFu) / 32767.5) - 1.0;
    }
}

void main()
{
    const OscillatorParams param = params[gl_WorkGroupID.x];

    // Apply first oscillator
    const float phase = param.phase + param.phase_step * gl_LocalInvocationID.x;

    const uint  type1 = param.osc_type[0];
    const float duty1 = param.duty[0];

    float value = oscillator(type1, phase, duty1);

    // Apply second oscillator (optional)
    const uint type2 = param.osc_type[1];

    if (type2 != no_wave) {
        const float duty2   = param.duty[1];
        const float osc_mix = param.osc_mix;

        value = mix(value, oscillator(type2, phase, duty2), osc_mix);
    }

    // Apply FIR filter (optional)
    if (param.taps_offs != 0) {

        // Read previously saved inputs
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            cached_input[tap] = data[param.fir_memory_offs + tap];
        }

        // Store generated sample values
        const uint base_pos = num_taps - 1 + gl_LocalInvocationID.x;
        cached_input[base_pos] = value;

        barrier();

        // Save current inputs for next invocation
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            data[param.fir_memory_offs + tap] = cached_input[work_group_size + tap];
        }

        // Apply FIR filter convolution
        value *= data[param.taps_offs];

        for (uint tap = 1; tap < num_taps; tap++) {
            value += data[param.taps_offs + tap] * cached_input[base_pos - tap];
        }
    }

    // Output generated sample
    data[param.out_sound_offs + gl_LocalInvocationID.x] = value;
}
