// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_modulation.h"
#include "../core/rng.h"
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
    const Synth::LFODescriptor sine_lfo = { Synth::WaveType::sine_wave, 0, 1000, -1.0f, 2.0f }; // 1s period, range [-1,1]
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
        const Synth::LFODescriptor periodic_lfo = { Synth::WaveType::sine_wave, 0, 1024, -1.0f, 2.0f };
        const uint32_t ticks_per_period = 4;
        TEST(approx(Synth::eval_lfo(periodic_lfo, 0, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, ticks_per_period, 256, 1000), 0.001f));
        TEST(approx(Synth::eval_lfo(periodic_lfo, 1, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, 1 + ticks_per_period, 256, 1000), 0.001f));
    }

    // sawtooth (triangle, duty=0x7F) LFO stays within [0,1] and reaches both ends
    {
        const Synth::LFODescriptor saw_lfo = { Synth::WaveType::sawtooth_wave, 0x7F, 1000, 0.0f, 1.0f };
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

    TEST(Synth::effect_param_floats(Synth::EffectType::distortion) == 2);
    TEST(Synth::effect_param_floats(Synth::EffectType::delay)      == 3);
    TEST(Synth::effect_param_floats(Synth::EffectType::chorus)     == 3);
    TEST(Synth::effect_param_floats(Synth::EffectType::reverb)     == 3);
    TEST(Synth::effect_param_floats(Synth::EffectType::compressor) == 5);
    TEST(Synth::effect_param_floats(Synth::EffectType::fir)        == 2);
    TEST(Synth::effect_state_floats(Synth::EffectType::distortion) == 0);
    TEST(Synth::effect_state_floats(Synth::EffectType::delay)      == 88201);
    TEST(Synth::effect_state_floats(Synth::EffectType::chorus)     == 4412);
    TEST(Synth::effect_state_floats(Synth::EffectType::reverb)     == 25191);
    TEST(Synth::effect_state_floats(Synth::EffectType::compressor) == 1);
    TEST(Synth::effect_state_floats(Synth::EffectType::fir)        == 3074);
    TEST(Synth::effect_param_floats(Synth::EffectType::none)       == 0);
    TEST(Synth::effect_state_floats(Synth::EffectType::none)       == 0);

    // Ring buffer accounting on free-running frame counters: available, free
    // space, and the contiguous run before the physical buffer wraps.
    {
        const uint32_t capacity = 1024;

        TEST(Synth::get_ringbuf_data_size(0, 0) == 0);
        TEST(Synth::get_ringbuf_avail_space(0, 0, capacity) == capacity);

        TEST(Synth::get_ringbuf_data_size(256, 0) == 256);
        TEST(Synth::get_ringbuf_avail_space(256, 0, capacity) == capacity - 256);

        TEST(Synth::get_ringbuf_data_size(300, 44) == 256);
        TEST(Synth::get_ringbuf_avail_space(300, 44, capacity) == capacity - 256);

        TEST(Synth::get_ringbuf_data_size(capacity, 0) == capacity);
        TEST(Synth::get_ringbuf_avail_space(capacity, 0, capacity) == 0);

        TEST(Synth::get_ringbuf_contig_tail(0, capacity) == capacity);          // offset 0: whole buffer
        TEST(Synth::get_ringbuf_contig_tail(1000, capacity) == 24);             // mid-buffer: 1024 - 1000
        TEST(Synth::get_ringbuf_contig_tail(2048, capacity) == capacity);       // exact multiple wraps to 0
        TEST(Synth::get_ringbuf_contig_tail(capacity + 1, capacity) == capacity - 1); // offset 1 after a wrap
    }

    // propagate_parameters: one-step-delay vs zero-lag external input.  Chain X(external) -> B -> A.
    // An external source is read at its current value (zero lag); a plain->plain hop lags exactly
    // one step.  Parameter 0 is the reserved sentinel.
    {
        Synth::ParamDescriptor descs[4] = { };
        // descs[0] (sentinel) and descs[3] (X, externally driven) stay kind external.
        descs[2].kind = Synth::ParamKind::plain;            // B reads X additively
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0] = { 3, Synth::SourceOp::add, 1.0f };
        descs[1].kind = Synth::ParamKind::plain;            // A reads B additively
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0] = { 2, Synth::SourceOp::add, 1.0f };

        Synth::Parameter params[4] = { };
        params[3].value = 5.0f;                       // drive the external input

        Synth::propagate_parameters(params, descs, 4);     // step 1
        TEST(approx(params[2].value, 5.0f, 0.001f));  // B picked up the input with zero lag
        TEST(approx(params[1].value, 0.0f, 0.001f));  // A still sees B's previous (0): one-step delay

        Synth::propagate_parameters(params, descs, 4);     // step 2
        TEST(approx(params[2].value, 5.0f, 0.001f));
        TEST(approx(params[1].value, 5.0f, 0.001f));  // A now sees B, one step later
    }

    // propagate_parameters: a feedback cycle (A <-> B, |amount| < 1) stays finite and
    // bounded; the one-step delay makes it a well-defined iteration, never a deadlock or NaN.
    {
        Synth::ParamDescriptor descs[3] = { };
        // descs[0] (sentinel) stays kind external.
        descs[1].kind = Synth::ParamKind::plain;            // A = 1 + 0.5 * B.prev
        descs[1].plain.base_value = 1.0f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0] = { 2, Synth::SourceOp::add, 0.5f };
        descs[2].kind = Synth::ParamKind::plain;            // B = 1 + 0.5 * A.prev
        descs[2].plain.base_value = 1.0f;
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0] = { 1, Synth::SourceOp::add, 0.5f };

        Synth::Parameter params[3] = { };
        for (uint32_t step = 0; step < 1000; step++) {
            Synth::propagate_parameters(params, descs, 3);
        }
        TEST(params[1].value == params[1].value);     // not NaN
        TEST(params[2].value == params[2].value);
        TEST(params[1].value < 100.0f && params[1].value > -100.0f); // bounded (converges to 2)
        TEST(approx(params[1].value, 2.0f, 0.01f));
    }

    // propagate_parameters: a multiply source scales the base (e.g. velocity into volume).
    {
        Synth::ParamDescriptor descs[3] = { };
        // descs[0] (sentinel) and descs[2] (velocity input) stay kind external.
        descs[1].kind = Synth::ParamKind::plain;            // volume base, scaled by velocity
        descs[1].plain.base_value = 0.8f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0] = { 2, Synth::SourceOp::multiply, 1.0f };

        Synth::Parameter params[3] = { };
        params[2].value = 0.5f;                       // velocity 0.5
        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[1].value, 0.4f, 0.001f));  // 0.8 * 0.5 (input read with zero lag)
    }

    // propagate_parameters: sources fold left-to-right over the running accumulator, so a
    // multiply applies to base plus prior adds, not to base alone.  This is the production
    // volume path: (base + envelope) * tremolo * velocity.
    {
        Synth::ParamDescriptor descs[5] = { };
        // descs[0] sentinel, descs[2] envelope, descs[3] tremolo, descs[4] velocity stay kind
        // external; their values are set directly below.
        descs[1].kind = Synth::ParamKind::plain;
        descs[1].plain.num_sources = 3;
        descs[1].plain.sources[0] = { 2, Synth::SourceOp::add,      1.0f };
        descs[1].plain.sources[1] = { 3, Synth::SourceOp::multiply, 1.0f };
        descs[1].plain.sources[2] = { 4, Synth::SourceOp::multiply, 1.0f };

        Synth::Parameter params[5] = { };
        params[2].value = 2.0f;                       // envelope 2.0
        params[3].value = 0.5f;                       // tremolo gain 0.5
        params[4].value = 0.5f;                       // velocity 0.5
        Synth::propagate_parameters(params, descs, 5);
        TEST(approx(params[1].value, 0.5f, 0.001f));  // (0 + 2.0) * 0.5 * 0.5, not 0 + 2.0 + ...
    }

    // configure_plain: effect-param shape -- base folded with an LFO leaf (add) and a
    // channel input leaf (multiply).  This is the contract the effect-param expander relies on:
    // a modulated effect param's value is propagate_parameters' result for the assembled dest.
    // The LFO leaf value is set directly here (the host pre-pass that fills it is not under test).
    {
        constexpr uint16_t dest_id  = 1;
        constexpr uint16_t lfo_id   = 2;
        constexpr uint16_t input_id = 3;

        Synth::ParamDescriptor descs[4] = { };
        // descs[0] (sentinel) and descs[input_id] (channel input, e.g. mod wheel) stay kind external.

        Synth::configure_lfo(&descs[lfo_id], 1, Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        const Synth::SourceParam inputs[1] = { { input_id, Synth::SourceOp::multiply, 1.0f } };
        Synth::configure_plain(&descs[dest_id], 0.1f, 0, lfo_id, Synth::SourceOp::add, inputs, 1);

        // configure_lfo made the LFO node; configure_plain made the dest a plain node.
        TEST(descs[lfo_id].kind == Synth::ParamKind::lfo);
        TEST(descs[lfo_id].lfo.desc_id == 1);
        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2);

        Synth::Parameter params[4] = { };
        params[lfo_id].value   = 0.2f;                // pretend the LFO produced 0.2
        params[input_id].value = 0.5f;                // channel input 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 0.15f, 0.001f)); // (0.1 + 0.2) * 0.5
    }

    // configure_plain: no LFO, an envelope source plus a multiply input -- the voice volume
    // shape (base + envelope) * velocity.  lfo_desc_id 0 means no LFO leaf and no LFO source.
    {
        constexpr uint16_t dest_id = 1;
        constexpr uint16_t env_id  = 2;
        constexpr uint16_t vel_id  = 3;

        Synth::ParamDescriptor descs[4] = { };
        // descs[0] sentinel, descs[env_id] envelope value, descs[vel_id] velocity: kind external,
        // their values set directly below.

        const Synth::SourceParam inputs[1] = { { vel_id, Synth::SourceOp::multiply, 1.0f } };
        Synth::configure_plain(&descs[dest_id], 0.0f, env_id, 0, Synth::SourceOp::add, inputs, 1);

        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2);  // envelope source + velocity source, no LFO source

        Synth::Parameter params[4] = { };
        params[env_id].value = 2.0f;                  // envelope 2.0
        params[vel_id].value = 0.5f;                  // velocity 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 1.0f, 0.001f)); // (0 + 2.0) * 0.5
    }

    // eval_lfo_mod: add op swings bipolar within [-depth, depth] and reaches both ends;
    // depth 0 is the neutral contribution 0.  4 ticks = one period here.
    {
        const Synth::LFODescriptor lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float depth = 0.5f;
        float add_min =  1e9f;
        float add_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float value = Synth::eval_lfo_mod(lfo, tick, 256, 1024, 1000, depth, Synth::SourceOp::add);
            TEST(value >= -depth - 0.001f && value <= depth + 0.001f);
            if (value < add_min) {
                add_min = value;
            }
            if (value > add_max) {
                add_max = value;
            }
        }
        TEST(approx(add_min, -depth, 0.02f));
        TEST(approx(add_max,  depth, 0.02f));
        // depth 0 -> neutral 0 at every tick
        TEST(approx(Synth::eval_lfo_mod(lfo, 7, 256, 1024, 1000, 0.0f, Synth::SourceOp::add), 0.0f, 0.001f));
    }

    // eval_lfo_mod: multiply op is an attenuation factor within [1-depth, 1];
    // depth 0 is the neutral factor 1.
    {
        const Synth::LFODescriptor lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float depth = 0.5f;
        float mul_min =  1e9f;
        float mul_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float factor = Synth::eval_lfo_mod(lfo, tick, 256, 1024, 1000, depth, Synth::SourceOp::multiply);
            TEST(factor >= 1.0f - depth - 0.001f && factor <= 1.0f + 0.001f);
            if (factor < mul_min) {
                mul_min = factor;
            }
            if (factor > mul_max) {
                mul_max = factor;
            }
        }
        TEST(approx(mul_min, 1.0f - depth, 0.02f));
        TEST(approx(mul_max, 1.0f, 0.02f));
        TEST(approx(Synth::eval_lfo_mod(lfo, 3, 256, 1024, 1000, 0.0f, Synth::SourceOp::multiply), 1.0f, 0.001f));
    }

    // eval_lfo_mod: a sourced rate (period_ms override) actually changes the rate.
    // Halving the period doubles the cycles, so a tick that is a quarter-period at
    // the long period becomes a half-period at the short one -> different phase.
    {
        const Synth::LFODescriptor lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float slow = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1024, 0.5f, Synth::SourceOp::add);
        const float fast = Synth::eval_lfo_mod(lfo, 1, 256, 1024,  512, 0.5f, Synth::SourceOp::add);
        TEST( ! approx(slow, fast, 0.05f));
        // period_ms 0 falls back to the descriptor's own period_ms.
        const float defaulted = Synth::eval_lfo_mod(lfo, 1, 256, 1024,    0, 0.5f, Synth::SourceOp::add);
        const float explicit_same = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1000, 0.5f, Synth::SourceOp::add);
        TEST(approx(defaulted, explicit_same, 0.001f));
    }

    // random_pitch_skew: a nonzero amount stays within [-amount, amount] and spans most of it;
    // amount 0 is exactly 0 (deterministic, generator unadvanced).
    {
        RNG skew_rng;
        skew_rng.init(0x5eed1234u);
        TEST(Synth::random_pitch_skew(&skew_rng, 0.0f) == 0.0f);

        const float amount = 0.25f;
        float skew_min =  1e9f;
        float skew_max = -1e9f;
        for (uint32_t draw = 0; draw < 100000; draw++) {
            const float skew = Synth::random_pitch_skew(&skew_rng, amount);
            TEST(skew >= -amount && skew <= amount);
            if (skew < skew_min) {
                skew_min = skew;
            }
            if (skew > skew_max) {
                skew_max = skew;
            }
        }
        TEST(skew_min < -amount * 0.9f);
        TEST(skew_max >  amount * 0.9f);
    }

    // Keyboard split routing: select_instrument maps a note to an instrument via an ordered
    // table; start_note 0 ends the table, so an all-zero table resolves to instrument 0.
    {
        const uint32_t count = Synth::max_instr_per_channel;

        Synth::NoteRoute empty[Synth::max_instr_per_channel] = { };
        TEST(Synth::select_instrument(empty, count, 0)   == 0);
        TEST(Synth::select_instrument(empty, count, 60)  == 0);
        TEST(Synth::select_instrument(empty, count, 127) == 0);

        // Notes 1..59 -> instrument 0, notes 60.. -> instrument 1.  Entry 0 must start at a
        // non-zero note since 0 is the end-of-table sentinel.
        Synth::NoteRoute split[Synth::max_instr_per_channel] = { };
        split[0] = { 1,  0 };
        split[1] = { 60, 1 };
        TEST(Synth::select_instrument(split, count, 0)   == 0);
        TEST(Synth::select_instrument(split, count, 59)  == 0);
        TEST(Synth::select_instrument(split, count, 60)  == 1);
        TEST(Synth::select_instrument(split, count, 127) == 1);

        Synth::NoteRoute three[Synth::max_instr_per_channel] = { };
        three[0] = { 1,  2 };
        three[1] = { 48, 4 };
        three[2] = { 72, 3 };
        TEST(Synth::select_instrument(three, count, 47)  == 2);
        TEST(Synth::select_instrument(three, count, 48)  == 4);
        TEST(Synth::select_instrument(three, count, 71)  == 4);
        TEST(Synth::select_instrument(three, count, 72)  == 3);

        // A full table with no sentinel still resolves the top range.
        Synth::NoteRoute full[Synth::max_instr_per_channel] = { };
        for (uint32_t idx = 0; idx < count; idx++) {
            full[idx] = { static_cast<uint8_t>(idx + 1), static_cast<uint8_t>(idx) };
        }
        TEST(Synth::select_instrument(full, count, 127) == count - 1);
    }

    return exit_code;
}
