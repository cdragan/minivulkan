// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 2) const uint num_taps = 1025;

const uint no_wave       = 0;
const uint sine_wave     = 1;
const uint sawtooth_wave = 2;
const uint pulse_wave    = 3;
const uint noise_wave    = 4;

const float two_pi = 6.283185307179586;

const uint osc_mode_blend     = 0;  // Mix osc_type[0] and osc_type[1] by osc_mix (default)
const uint osc_mode_fm        = 1;  // osc_type[0] is carrier, osc_type[1] is modulator
const uint osc_mode_hard_sync = 2;  // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave

struct OscillatorParams {
    uint  out_sound_offs;   // Offset of output sound data
    float phase;            // Initial phase value at first sample to render, 1 is equivalent to wave length
    float phase_step;       // Phase step between samples, derived from oscillator's and sampling frequencies
    uint  osc_type[2];      // Two oscillator types
    float duty[2];          // Duty cycle for sawtooth and pulse oscillator [0..1]
    float osc_mix;          // Mixing between osc_type[0] and osc_type[1] [0..1]
    uint  osc_mode;         // osc_mode_blend or osc_mode_fm
    float mod_ratio;        // FM: modulator frequency / carrier frequency.  Hard sync: slave cycles per master cycle
    float fm_index;         // FM modulation depth
    float mod_phase;        // Modulator's initial phase value at first sample to render
    float mod_phase_step;   // Modulator's phase step between samples

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
    const uint  type2 = param.osc_type[1];
    const float duty2 = param.duty[1];

    float value;

    if (param.osc_mode == osc_mode_fm) {
        // Frequency modulation: type1 is the carrier, type2 the modulator.
        // The modulator runs at its own frequency with its own phase.
        const float mod_phase = param.mod_phase + param.mod_phase_step * gl_LocalInvocationID.x;
        const float modulator = oscillator(type2, mod_phase, duty2);

        value = oscillator(type1, phase + param.fm_index * modulator, duty1);
    }
    else if (param.osc_mode == osc_mode_hard_sync) {
        // Hard sync: type1 is the master providing the note frequency, type2 is
        // the slave whose phase resets each time the master phase wraps.  The
        // slave is stateless, derived from the master phase.  mod_ratio is
        // reused here as the sync ratio (slave cycles per master cycle).
        value = oscillator(type2, fract(param.mod_ratio * fract(phase)), duty2);
    }
    else {
        // Blend mode: mix the two oscillators by osc_mix.
        value = oscillator(type1, phase, duty1);

        if (type2 != no_wave) {
            value = mix(value, oscillator(type2, phase, duty2), param.osc_mix);
        }
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
