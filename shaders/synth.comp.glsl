// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#version 460 core

#extension GL_EXT_shader_explicit_arithmetic_types_int8:  require
#extension GL_EXT_shader_explicit_arithmetic_types_int16: require

// Sample synthesizer, which synthesizes instrument sounds.
//
// * Each sample generated by this synth consists of multiple components
// * Each component has the following parameters:
//   - Integer frequency multiplier, 1+, because a sound can have base frequency
//     and then additional frequencies
//   - Wave type (sine, triangle, sawtooth, square, noise)
//   - Delay (also counts as phase offset)
//   - ADSR amplitude multiplier (0..1) envelope
//   - ADSR frequency offset envelope
//   - amplitude LFO
//   - frequency offset LFO
// * Each ADSR envelope is:
//   - init value (0 for amplitude)
//   - max value
//   - sustain value
//   - end value (0 for amplitude)
//   - attack duration
//   - decay duration
//   - release duration

const uint wave_sine     = 0;
const uint wave_triangle = 1;
const uint wave_sawtooth = 2;
const uint wave_square   = 3;
const uint wave_noise    = 4;

struct Component {
    uint8_t  freq_mult;     // Base frequency multiplier
    uint8_t  wave_type;     // See wave_* constants
    uint16_t delay_us;      // Delay after which this component starts playing
    uint8_t  amplitude_lfo;
    uint8_t  freq_lfo;
    uint8_t  amplitude_env;
    uint8_t  freq_env;
};

struct LFO {
    uint8_t  wave_type;     // See wave_* constants
    uint8_t  dummy1;
    uint16_t period_ms;     // 0 means that LFO is disabled
    uint16_t peak_delta;
    uint16_t dummy2;
};

struct ADSR {
    uint16_t init_value;
    uint16_t max_value;
    uint16_t sustain_value;
    uint16_t end_value;
    uint16_t attack_ms;
    uint16_t decay_ms;
    uint16_t dummy;
    uint16_t release_ms;
};

const uint  mix_freq       = 44100;
const uint  max_components = 32;
const uint  max_lfos       = 4;
const float two_pi         = 6.283185307179586;

layout(set = 0, binding = 0) uniform ubo_data {
    LFO       lfo[max_lfos];
    ADSR      envelope[max_lfos];
    Component comps[max_components];
    uint      num_comps;
} ubo;

layout(push_constant) uniform push_constants {
    uint num_samples;   // Number of samples to generate in the output sound
    uint base_freq;     // Frequency of the note being played
} push;

layout(set = 0, binding = 1) buffer output_data { float out_sound[]; };

layout(local_size_x = 1024) in;

uint random(uint index)
{
    // Use offset (index to the sample) as seed for a trivial LCG
    uint state = (index << 1) | 1;

    // Loop a few times through the LCG
    for (uint i = 0; i < 4; i++)
        state = (state * 0x8088405) + 1;

    // Use xorshift+ RNG from the above seed generated with LCG
    uint rand = (state * 0x8088405) + 1;
    rand ^= rand << 23;
    rand ^= rand >> 18;
    rand ^= state ^ (state >> 5);
    rand = (state + rand) & 0xFFFF;

    return rand;
}

float wave(uint wave_type, uint offs, uint period)
{
    float value;

    // Sine wave
    if (wave_type == wave_sine) {
        value = sin(offs * two_pi / period);
    }
    // Noise
    else if (wave_type == wave_noise) {
        value = float(random(offs) & 0xFFFF) / 32768.0 - 1;
    }
    else {
        offs %= period;
        const uint half_period = period / 2;

        // Square wave
        if (wave_type == wave_square) {
            value = (offs < half_period) ? -1 : 1;

        // Triangle or sawtooth wave
        } else {
            value = (float(offs) / float(half_period)) - 1;

            // Convert sawtooth to triangle wave
            if (wave_type == wave_triangle) {
                value = (abs(value) * 2) - 1;
            }
        }
    }

    return value;
}

int lfo(LFO lfo, uint offs)
{
    if (lfo.period_ms == 0)
        return 0;

    const uint lfo_period = lfo.period_ms * mix_freq / 1000;

    return int(wave(lfo.wave_type, offs, lfo_period) * lfo.peak_delta);
}

uint16_t mix16(uint16_t start_value, uint16_t end_value, uint offs, uint end_offs)
{
    const int range = int(end_value) - int(start_value);
    const int delta = int(offs) * range / int(end_offs);
    return uint16_t(int(start_value) + delta);
}

uint envelope(ADSR envelope, uint offs, uint duration_ms)
{
    const uint attack_end_offs = envelope.attack_ms * mix_freq / 1000;
    if (offs < attack_end_offs)
        return mix16(envelope.init_value, envelope.max_value, offs, attack_end_offs);

    offs -= attack_end_offs;

    const uint decay_end_offs = envelope.decay_ms * mix_freq / 1000;
    if (offs < decay_end_offs)
        return mix16(envelope.max_value, envelope.sustain_value, offs, decay_end_offs);

    offs -= decay_end_offs;

    const uint att_dec_rel_ms   = envelope.attack_ms + envelope.decay_ms + envelope.release_ms;
    const uint sustain_ms       = duration_ms - att_dec_rel_ms;
    const uint sustain_end_offs = sustain_ms * mix_freq / 1000;
    if (offs < sustain_end_offs)
        return envelope.sustain_value;

    offs -= sustain_end_offs;

    const uint release_end_offs = envelope.release_ms * mix_freq / 1000;
    if (offs < release_end_offs)
        return mix16(envelope.sustain_value, envelope.end_value, offs, release_end_offs);

    return envelope.end_value;
}

void generate_sample(uint out_offs)
{
    float value = 0;

    int lfo_values[max_lfos];
    for (uint i = 0; i < max_lfos; i++) {
        lfo_values[i] = lfo(ubo.lfo[i], out_offs);
    }

    const uint duration_ms = push.num_samples * 1000 / mix_freq;

    for (uint i = 0; i < ubo.num_comps; i++) {
        Component comp = ubo.comps[i];

        const uint start_offs = comp.delay_us * mix_freq / 1000000;
        if (out_offs < start_offs)
            continue;

        const uint delay_ms = (comp.delay_us > 0) ? ((comp.delay_us - 1) / 1000 + 1) : 0;
        const uint offs     = out_offs - start_offs;

        int freq_delta = 0;
        if (comp.freq_lfo > 0)
            freq_delta = lfo_values[comp.freq_lfo - 1];

        if (comp.freq_env > 0) {
            const uint env_value = envelope(ubo.envelope[comp.freq_env - 1], offs,
                                            duration_ms - delay_ms);
            freq_delta += int(env_value) - 32768;
        }

        const uint period = mix_freq / ((push.base_freq + freq_delta) * comp.freq_mult);

        float comp_value = wave(comp.wave_type, offs, period);

        if (comp.amplitude_lfo > 0)
            comp_value *= float(lfo_values[comp.amplitude_lfo - 1]) / 65535.0;

        if (comp.amplitude_env > 0) {
            const uint env_value = envelope(ubo.envelope[comp.amplitude_env - 1], offs,
                                            duration_ms - delay_ms);
            comp_value *= float(env_value) / 65536.0;
        }

        value += comp_value;
    }

    out_sound[out_offs] = value;
}

void main()
{
    const uint delta = gl_WorkGroupSize.x * gl_NumWorkGroups.x;

    for (uint out_offs = gl_GlobalInvocationID.x; out_offs < push.num_samples; out_offs += delta) {
        generate_sample(out_offs);
    }
}
