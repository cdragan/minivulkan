// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

const uint no_wave       = 0;
const uint sine_wave     = 1;
const uint sawtooth_wave = 2;
const uint pulse_wave    = 3;
const uint noise_wave    = 4;

const float two_pi = 6.283185307179586;

struct WaveParams {
    float phase;            // Initial phase value at first sample to render, 1 is equivalent to wave length
    float phase_step;       // Phase step between samples, derived from wave and sampling frequencies
    uint  wave_type[2];     // Two wave types
    float duty[2];          // Duty cycle for sawtooth and pulse wave [0..1]
    float wave_mix;         // Mixing between wave_type[0] and wave_type[1] [0..1]
};

layout(set = 0, binding = 0) buffer input_params { WaveParams params[]; };

layout(set = 0, binding = 1) buffer output_data { float out_sound[]; };

layout(local_size_x = 256) in;

uint pcg_hash(uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float wave(uint wave_type, float duty)
{
    float phase = params[gl_WorkGroupID.x].phase;

    if (wave_type == sine_wave) {
        return sin(two_pi * phase);
    }
    else if (wave_type == sawtooth_wave) {
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
    else if (wave_type == pulse_wave) {
        phase = fract(phase);

        return (phase < duty) ? 1.0 : -1.0;
    }
    else { // noise_wave
        const uint uvalue = pcg_hash(uint(floor(phase)) + gl_LocalInvocationID.x);
        return (float(uvalue & 0xFFFFu) / 32767.5) - 1.0;
    }
}

void main()
{
    const float phase = params[gl_WorkGroupID.x].phase + params[gl_WorkGroupID.x].phase_step * gl_LocalInvocationID.x;

    const uint  type1 = params[gl_WorkGroupID.x].wave_type[0];
    const float duty1 = params[gl_WorkGroupID.x].duty[0];

    float value = wave(type1, duty1);

    const uint type2 = params[gl_WorkGroupID.x].wave_type[1];

    if (type2 != no_wave) {
        const float duty2    = params[gl_WorkGroupID.x].duty[1];
        const float wave_mix = params[gl_WorkGroupID.x].wave_mix;

        value = mix(value, wave(type2, duty2), wave_mix);
    }

    out_sound[gl_GlobalInvocationID.x] = value;
}
