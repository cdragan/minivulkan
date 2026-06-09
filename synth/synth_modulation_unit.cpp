// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_modulation.h"
#include <stdio.h>

#define TEST(test) if ( ! (test)) { failed(#test, __FILE__, __LINE__); }

static int exit_code = 0;

static void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n", file, line, test);
}

static bool approx(float left, float right, float eps)
{
    float diff = left - right;
    if (diff < 0.0f) {
        diff = -diff;
    }
    return diff <= eps;
}

int main()
{
    // A4 = 440 Hz
    TEST(approx(Synth::note_to_frequency(69, 0.0f, 1), 440.0f, 0.01f));
    // One octave up = 880 Hz
    TEST(approx(Synth::note_to_frequency(81, 0.0f, 1), 880.0f, 0.02f));
    // One semitone up via pitch offset ~ 466.16 Hz
    TEST(approx(Synth::note_to_frequency(69, 1.0f, 1), 466.16f, 0.05f));
    // freq_mult doubles the frequency
    TEST(approx(Synth::note_to_frequency(69, 0.0f, 2), 880.0f, 0.02f));

    // sine LFO: contribution stays within [min, min+delta] and is periodic
    const Synth::LFODescriptor sine_lfo = { Synth::sine_wave, 0, 1000, -1.0f, 2.0f }; // 1s period, range [-1,1]
    float min_seen =  1e9f;
    float max_seen = -1e9f;
    for (uint32_t tick = 0; tick < 1000; tick++) {
        const float lfo_value = Synth::eval_lfo(sine_lfo, tick, 256, 44100);
        TEST(lfo_value >= -1.0001f && lfo_value <= 1.0001f);
        if (lfo_value < min_seen) {
            min_seen = lfo_value;
        }
        if (lfo_value > max_seen) {
            max_seen = lfo_value;
        }
    }
    TEST(approx(min_seen, -1.0f, 0.05f));
    TEST(approx(max_seen,  1.0f, 0.05f));
    // periodicity: with these params one period is exactly 4 ticks, so values
    // repeat every 4 ticks
    {
        const Synth::LFODescriptor periodic_lfo = { Synth::sine_wave, 0, 1024, -1.0f, 2.0f };
        const uint32_t ticks_per_period = 4;
        TEST(approx(Synth::eval_lfo(periodic_lfo, 0, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, ticks_per_period, 256, 1000), 0.001f));
        TEST(approx(Synth::eval_lfo(periodic_lfo, 1, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, 1 + ticks_per_period, 256, 1000), 0.001f));
    }

    // sawtooth (triangle, duty=0x7F) LFO stays within [0,1] and reaches both ends
    {
        const Synth::LFODescriptor saw_lfo = { Synth::sawtooth_wave, 0x7F, 1000, 0.0f, 1.0f };
        float saw_min =  1e9f;
        float saw_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float saw_value = Synth::eval_lfo(saw_lfo, tick, 256, 44100);
            TEST(saw_value >= -0.0001f && saw_value <= 1.0001f);
            if (saw_value < saw_min) {
                saw_min = saw_value;
            }
            if (saw_value > saw_max) {
                saw_max = saw_value;
            }
        }
        TEST(approx(saw_min, 0.0f, 0.05f));
        TEST(approx(saw_max, 1.0f, 0.05f));
    }

    // 4-point ADSR-like envelope: rise to peak (tick2), decay to mid (tick4,
    // the sustain point), release to zero (tick6).  value 0xFFFF maps to 1.0.
    {
        struct FourPointEnvelope {
            Synth::EnvelopeDescriptor desc;
            Synth::EnvelopeDescriptor::Point extra_points[3];
        };
        FourPointEnvelope env = { };
        env.desc.num_points          = 4;
        env.desc.sustain_first_point = 2;
        env.desc.sustain_last_point  = 2;
        env.desc.min_value           = 0.0f;
        env.desc.min_max_delta       = 1.0f / 65535.0f;
        env.desc.points[0] = { 0, 0 };       // start at min
        env.desc.points[1] = { 2, 0xFFFF };  // attack peak
        env.desc.points[2] = { 4, 0x8000 };  // decay to ~mid (sustain)
        env.desc.points[3] = { 6, 0 };       // release to min

        // Sustained: run 20 ticks holding sustain.  Should reach ~1.0 peak,
        // then settle and HOLD at the sustain value (~0.5).
        Synth::EnvelopeState state = { 0, 0 };
        float first_value = Synth::eval_envelope(env.desc, &state, true);
        TEST(approx(first_value, 0.0f, 0.01f));
        float peak_value = first_value;
        float last_value = first_value;
        for (uint32_t tick = 1; tick < 20; tick++) {
            last_value = Synth::eval_envelope(env.desc, &state, true);
            if (last_value > peak_value) {
                peak_value = last_value;
            }
        }
        TEST(approx(peak_value, 1.0f, 0.02f));        // attack peak reached
        TEST(approx(last_value, 0x8000 / 65535.0f, 0.01f)); // held at sustain value

        // Release: stop sustaining, run more ticks; value must reach ~0.
        float release_value = last_value;
        for (uint32_t tick = 0; tick < 10; tick++) {
            release_value = Synth::eval_envelope(env.desc, &state, false);
        }
        TEST(approx(release_value, 0.0f, 0.01f));
    }

    // pitch bend -> semitones: centered 14-bit value scaled by the bend range.
    // center (0) -> no bend; full deflection -> +/- range; half -> ~half range.
    TEST(approx(Synth::pitch_bend_to_semitones(0, 2.0f), 0.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 2.0f), 2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(-8192, 2.0f), -2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(4096, 2.0f), 1.0f, 0.001f));
    // range scales linearly
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 12.0f), 12.0f, 0.01f));

    // eval_parameter sums a parameter's enabled sources (base + envelope + LFO +
    // MIDI input) and advances the running state one tick per call.  Null sources
    // and a zero MIDI value contribute nothing.
    {
        // base only
        Synth::ParameterState base_state = { { 0, 0 }, 0 };
        TEST(approx(Synth::eval_parameter(0.5f, nullptr, true, nullptr, 0.0f, &base_state, 256, 44100), 0.5f, 0.001f));

        // MIDI input is added (e.g. pitch bend in semitones), independent of scope
        Synth::ParameterState midi_state = { { 0, 0 }, 0 };
        TEST(approx(Synth::eval_parameter(0.0f, nullptr, true, nullptr, 1.75f, &midi_state, 256, 44100), 1.75f, 0.001f));

        // base + MIDI
        Synth::ParameterState base_midi_state = { { 0, 0 }, 0 };
        TEST(approx(Synth::eval_parameter(0.25f, nullptr, true, nullptr, -0.5f, &base_midi_state, 256, 44100), -0.25f, 0.001f));

        // base + LFO matches base + eval_lfo, and the LFO tick advances each call
        {
            const Synth::LFODescriptor lfo = { Synth::sine_wave, 0, 1000, -1.0f, 2.0f };
            Synth::ParameterState lfo_state = { { 0, 0 }, 0 };
            const float expected_tick0 = 0.1f + Synth::eval_lfo(lfo, 0, 256, 44100);
            TEST(approx(Synth::eval_parameter(0.1f, nullptr, true, &lfo, 0.0f, &lfo_state, 256, 44100), expected_tick0, 0.001f));
            TEST(lfo_state.lfo_tick == 1);
            const float expected_tick1 = 0.1f + Synth::eval_lfo(lfo, 1, 256, 44100);
            TEST(approx(Synth::eval_parameter(0.1f, nullptr, true, &lfo, 0.0f, &lfo_state, 256, 44100), expected_tick1, 0.001f));
            TEST(lfo_state.lfo_tick == 2);
        }

        // base + envelope + LFO + MIDI all sum together; state advances once
        {
            struct FourPointEnvelope {
                Synth::EnvelopeDescriptor desc;
                Synth::EnvelopeDescriptor::Point extra_points[3];
            };
            FourPointEnvelope env = { };
            env.desc.num_points          = 4;
            env.desc.sustain_first_point = 2;
            env.desc.sustain_last_point  = 2;
            env.desc.min_value           = 0.0f;
            env.desc.min_max_delta       = 1.0f / 65535.0f;
            env.desc.points[0] = { 0, 0 };
            env.desc.points[1] = { 2, 0xFFFF };
            env.desc.points[2] = { 4, 0x8000 };
            env.desc.points[3] = { 6, 0 };

            const Synth::LFODescriptor lfo = { Synth::sine_wave, 0, 1000, -1.0f, 2.0f };

            // Reference contributions computed independently
            Synth::EnvelopeState env_ref = { 0, 0 };
            const float env_contrib = Synth::eval_envelope(env.desc, &env_ref, true);
            const float lfo_contrib = Synth::eval_lfo(lfo, 0, 256, 44100);

            Synth::ParameterState combined = { { 0, 0 }, 0 };
            const float expected = 0.2f + env_contrib + lfo_contrib + 0.3f;
            TEST(approx(Synth::eval_parameter(0.2f, &env.desc, true, &lfo, 0.3f, &combined, 256, 44100), expected, 0.001f));
            TEST(combined.lfo_tick == 1);
            TEST(combined.envelope.tick == env_ref.tick);
        }
    }

    TEST(Synth::effect_param_floats(Synth::effect_distortion) == 2);
    TEST(Synth::effect_param_floats(Synth::effect_delay)      == 3);
    TEST(Synth::effect_param_floats(Synth::effect_chorus)     == 3);
    TEST(Synth::effect_param_floats(Synth::effect_reverb)     == 3);
    TEST(Synth::effect_param_floats(Synth::effect_compressor) == 5);
    TEST(Synth::effect_param_floats(Synth::effect_fir)        == 2);
    TEST(Synth::effect_state_floats(Synth::effect_distortion) == 0);
    TEST(Synth::effect_state_floats(Synth::effect_delay)      == 88201);
    TEST(Synth::effect_state_floats(Synth::effect_chorus)     == 4412);
    TEST(Synth::effect_state_floats(Synth::effect_reverb)     == 25191);
    TEST(Synth::effect_state_floats(Synth::effect_compressor) == 1);
    TEST(Synth::effect_state_floats(Synth::effect_fir)        == 3074);
    TEST(Synth::effect_param_floats(Synth::effect_none)       == 0);
    TEST(Synth::effect_state_floats(Synth::effect_none)       == 0);

    return exit_code;
}
