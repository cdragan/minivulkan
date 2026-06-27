// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_parameters.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_serialize.h"
#include "midi_file.h"
#include "synth_soundtrack.h"
#include "../core/rng.h"
#include <stdio.h>
#include <string.h>

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
        Synth::EnvelopeDescriptor env = { };
        env.num_points          = 4;
        env.sustain_first_point = 2;
        env.sustain_last_point  = 2;
        env.min_value           = 0.0f;
        env.min_max_delta       = 1.0f / 65535.0f;
        env.points[0] = { 0, 0 };       // start at min
        env.points[1] = { 2, 0xFFFF };  // attack peak
        env.points[2] = { 4, 0x8000 };  // decay to ~mid (sustain)
        env.points[3] = { 6, 0 };       // release to min

        // Sustained: run 20 ticks holding sustain.  Should reach ~1.0 peak,
        // then settle and HOLD at the sustain value (~0.5).
        Synth::EnvelopeState state = { 0, 0 };
        float first_value = Synth::eval_envelope(env, &state, true);
        TEST(approx(first_value, 0.0f, 0.01f));
        float peak_value = first_value;
        float last_value = first_value;
        for (uint32_t tick = 1; tick < 20; tick++) {
            last_value = Synth::eval_envelope(env, &state, true);
            if (last_value > peak_value) {
                peak_value = last_value;
            }
        }
        TEST(approx(peak_value, 1.0f, 0.02f));        // attack peak reached
        TEST(approx(last_value, 0x8000 / 65535.0f, 0.01f)); // held at sustain value

        // Release: stop sustaining, run more ticks; value must reach ~0.
        float release_value = last_value;
        for (uint32_t tick = 0; tick < 10; tick++) {
            release_value = Synth::eval_envelope(env, &state, false);
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

    TEST(Synth::get_effect_param_floats(Synth::EffectType::distortion) == 2);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::delay)      == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::chorus)     == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::reverb)     == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::compressor) == 5);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::fir)        == 2);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::distortion) == 0);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::delay)      == 88201);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::chorus)     == 4412);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::reverb)     == 25191);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::compressor) == 1);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::fir)        == 3074);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::none)       == 0);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::none)       == 0);

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
        descs[2].plain.sources[0] = { 3, 1.0f, Synth::SourceOp::add };
        descs[1].kind = Synth::ParamKind::plain;            // A reads B additively
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0] = { 2, 1.0f, Synth::SourceOp::add };

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
        descs[1].plain.sources[0] = { 2, 0.5f, Synth::SourceOp::add };
        descs[2].kind = Synth::ParamKind::plain;            // B = 1 + 0.5 * A.prev
        descs[2].plain.base_value = 1.0f;
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0] = { 1, 0.5f, Synth::SourceOp::add };

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
        descs[1].plain.sources[0] = { 2, 1.0f, Synth::SourceOp::multiply };

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
        descs[1].plain.sources[0] = { 2, 1.0f, Synth::SourceOp::add      };
        descs[1].plain.sources[1] = { 3, 1.0f, Synth::SourceOp::multiply };
        descs[1].plain.sources[2] = { 4, 1.0f, Synth::SourceOp::multiply };

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
        const Synth::SourceParam inputs[1] = { { input_id, 1.0f, Synth::SourceOp::multiply } };
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

        const Synth::SourceParam inputs[1] = { { vel_id, 1.0f, Synth::SourceOp::multiply } };
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

    // --- Phase 1 data foundation: generic pools, container, defragment + remap ---

    // alloc-distinct: three allocations from an empty pool give distinct, in-range slots.
    {
        Pool<Synth::EnvelopeDescriptor, Synth::max_envelopes> env_pool = { };
        const uint32_t slot0 = env_pool.allocate();
        const uint32_t slot1 = env_pool.allocate();
        const uint32_t slot2 = env_pool.allocate();
        TEST(slot0 != pool_no_slot && slot1 != pool_no_slot && slot2 != pool_no_slot);
        TEST(slot0 != slot1 && slot1 != slot2 && slot0 != slot2);
        TEST(slot0 < Synth::max_envelopes && slot1 < Synth::max_envelopes && slot2 < Synth::max_envelopes);
        TEST(env_pool.num_allocated == 3);
    }

    // reuse-after-free: freeing the middle slot lets the next allocate reuse it.
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = { };
        const uint32_t first  = lfo_pool.allocate();
        const uint32_t middle = lfo_pool.allocate();
        const uint32_t last   = lfo_pool.allocate();
        TEST(first != pool_no_slot && last != pool_no_slot);
        lfo_pool.free(middle);
        TEST(lfo_pool.num_allocated == 2);
        const uint32_t reused = lfo_pool.allocate();
        TEST(reused == middle);
        TEST(lfo_pool.num_allocated == 3);
    }

    // full-pool: allocate up to capacity, then allocate fails with pool_no_slot (no OOB).
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = { };
        for (uint32_t idx = 0; idx < Synth::max_lfos; idx++) {
            TEST(lfo_pool.allocate() != pool_no_slot);
        }
        TEST(lfo_pool.allocate() == pool_no_slot);
        TEST(lfo_pool.num_allocated == Synth::max_lfos);
    }

    // defrag-remap (envelopes): a fragmented pool [used,free,used,free,used] compacts to the
    // front preserving order, and an instrument's 1-based envelope reference is rewritten.
    {
        Synth::InstrumentBank bank = { };
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.envelopes.allocate() == idx);
        }
        bank.envelopes.free(1);
        bank.envelopes.free(3);

        // Tag slot 4's data so we can find where it lands, and reference it from a layer
        // (desc_id is 1-based: slot 4 -> id 5).
        bank.envelopes.entries[4].num_points = 42;
        const uint32_t instr = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layer_count = 1;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id = 5;

        uint32_t env_map[Synth::max_envelopes];
        bank.envelopes.defragment(env_map);
        TEST(bank.envelopes.num_allocated == 3);
        TEST(env_map[0] == 0);                      // old 0,2,4 -> new 0,1,2 in order
        TEST(env_map[2] == 1);
        TEST(env_map[4] == 2);
        TEST(env_map[1] == pool_no_slot);
        TEST(env_map[3] == pool_no_slot);
        TEST(bank.envelopes.entries[2].num_points == 42); // slot 4's data moved to slot 2

        Synth::remap_envelope_refs(&bank, env_map);
        TEST(bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id == 3);
    }

    // defrag-remap (LFOs): same mechanism as envelopes -- a layer's 1-based lfo_desc_id is
    // rewritten to the referenced LFO entry's new slot.
    {
        Synth::InstrumentBank bank = { };
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.lfos.allocate() == idx);
        }
        bank.lfos.free(1);
        bank.lfos.free(3);
        bank.lfos.entries[4].period_ms = 333;

        const uint32_t instr = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id = 5; // slot 4 -> id 5

        uint32_t lfo_map[Synth::max_lfos];
        bank.lfos.defragment(lfo_map);
        TEST(lfo_map[4] == 2);
        TEST(bank.lfos.entries[2].period_ms == 333);

        Synth::remap_lfo_refs(&bank, lfo_map);
        TEST(bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id == 3);
    }

    // defrag-remap (instruments): an instrument pool compacts and a channel split-table entry
    // that references an instrument (0-based) is rewritten to the new slot.
    {
        Synth::InstrumentBank bank = { };
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.instruments.allocate() == idx);
        }
        bank.instruments.free(1);
        bank.instruments.free(3);
        bank.channel_routes[0][0] = { 1, 4 };       // note >= 1 -> instrument slot 4 (survives)
        bank.channel_routes[0][1] = { 64, 3 };      // references deleted instrument slot 3

        uint32_t instr_map[Synth::max_instruments];
        bank.instruments.defragment(instr_map);
        TEST(instr_map[4] == 2);

        Synth::remap_instrument_refs(&bank, instr_map);
        TEST(bank.channel_routes[0][0].instrument == 2);
        TEST(bank.channel_routes[0][1].instrument == 0); // dangling ref falls back to instrument 0
    }

    // snapshot-roundtrip: a byte copy of the container, then a byte restore after mutation,
    // reproduces identical state -- the property the undo stack and save/load rely on.
    {
        Synth::InstrumentBank bank = { };
        const uint32_t instr = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.drum_track_channel = 9;
        const uint32_t env = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points = 7;

        Synth::InstrumentBank snapshot;
        memcpy(&snapshot, &bank, sizeof(bank));

        bank.instruments.entries[instr].layer_count = 99;
        bank.instruments.free(instr);
        bank.drum_track_channel = 0;

        memcpy(&bank, &snapshot, sizeof(bank));
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.instruments.entries[instr].layer_count == 3);
        TEST(bank.drum_track_channel == 9);
        TEST(bank.envelopes.entries[env].num_points == 7);
        TEST(memcmp(&bank, &snapshot, sizeof(bank)) == 0);
    }

    // instrument bank codec round-trips losslessly, with cross-references intact
    {
        Synth::InstrumentBank bank = { };
        const uint32_t env   = bank.envelopes.allocate();
        const uint32_t lfo   = bank.lfos.allocate();
        const uint32_t instr = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 2;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id =
            static_cast<uint16_t>(env + 1);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id =
            static_cast<uint16_t>(lfo + 1);
        bank.channel_routes[0][0] = { 0, static_cast<uint8_t>(instr) };
        bank.drum_track_channel   = 9;
        memcpy(bank.instrument_names[instr], "Lead", 5);

        uint8_t image[Synth::instrument_bank_image_size];
        const uint32_t written = Synth::encode_instrument_bank(&bank, image, sizeof(image));
        TEST(written == Synth::instrument_bank_image_size);

        Synth::InstrumentBank restored = { };
        TEST(Synth::decode_instrument_bank(image, written, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);

        // A buffer too small to hold the image fails cleanly, writing nothing.
        TEST(Synth::encode_instrument_bank(&bank, image, 4) == 0);

        // A corrupt marker is rejected.
        uint8_t bad[Synth::instrument_bank_image_size];
        memcpy(bad, image, sizeof(bad));
        bad[0] = static_cast<uint8_t>(bad[0] ^ 0xFFu);
        TEST( ! Synth::decode_instrument_bank(bad, written, &restored));

        // A mismatched version is rejected (version is the two bytes after the 4-byte marker).
        memcpy(bad, image, sizeof(bad));
        bad[4] = static_cast<uint8_t>(bad[4] ^ 0xFFu);
        TEST( ! Synth::decode_instrument_bank(bad, written, &restored));

        // A mismatched payload size is rejected (the four bytes after the version).
        memcpy(bad, image, sizeof(bad));
        bad[6] = static_cast<uint8_t>(bad[6] ^ 0xFFu);
        TEST( ! Synth::decode_instrument_bank(bad, written, &restored));

        // A truncated image (header only) is rejected.
        TEST( ! Synth::decode_instrument_bank(image, Synth::instrument_bank_header_size, &restored));
    }

    // instrument bank persists to and loads from a real file
    {
        Synth::InstrumentBank bank = { };
        bank.drum_track_channel  = 7;
        const uint32_t instr     = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.channel_routes[2][1] = { 64, static_cast<uint8_t>(instr) };

        const char* const path = "synth_bank_roundtrip.tmp";
        TEST(Synth::save_instrument_bank(path, &bank));

        Synth::InstrumentBank restored = { };
        TEST(Synth::load_instrument_bank(path, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);
        remove(path);

        // Loading a nonexistent file fails cleanly.
        TEST( ! Synth::load_instrument_bank("synth_bank_does_not_exist.tmp", &restored));
    }

    // MIDI file: format 0, single track, division 96.  A tempo meta (500000 us/quarter =
    // 120 BPM), a note_on at delta 0, and a note_off one quarter (delta 96) later.  At 44100 Hz
    // a quarter note is 0.5 s = 22050 samples, so the note_off lands at sample 22050.
    {
        static const uint8_t midi[] = {
            // MThd
            0x4D, 0x54, 0x68, 0x64,             // "MThd"
            0x00, 0x00, 0x00, 0x06,             // header length 6
            0x00, 0x00,                         // format 0
            0x00, 0x01,                         // ntrks 1
            0x00, 0x60,                         // division 96
            // MTrk
            0x4D, 0x54, 0x72, 0x6B,             // "MTrk"
            0x00, 0x00, 0x00, 0x13,             // track length 19
            0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, // delta 0, tempo 500000 us/quarter
            0x00, 0x90, 0x3C, 0x64,             // delta 0, note_on ch0 note 60 vel 100
            0x60, 0x80, 0x3C, 0x40,             // delta 96, note_off ch0 note 60 vel 64
            0x00, 0xFF, 0x2F, 0x00,             // delta 0, end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
        TEST(count == 2);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].channel == 0);
        TEST(events[0].note == 60);
        TEST(events[0].note_data == 100);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::note_off);
        TEST(events[1].channel == 0);
        TEST(events[1].note == 60);
        TEST(events[1].time == 22050);
    }

    // MIDI file: running status -- a second note_on reuses the prior 0x90 status byte (no
    // status byte, just data).  Default tempo (no FF51) is 120 BPM, division 96.
    {
        static const uint8_t midi[] = {
            0x4D, 0x54, 0x68, 0x64,
            0x00, 0x00, 0x00, 0x06,
            0x00, 0x00,
            0x00, 0x01,
            0x00, 0x60,
            0x4D, 0x54, 0x72, 0x6B,
            0x00, 0x00, 0x00, 0x0B,             // track length 11
            0x00, 0x90, 0x3C, 0x64,             // delta 0, note_on ch0 note 60 vel 100
            0x30, 0x3E, 0x64,                   // delta 48, running status: note_on note 62 vel 100
            0x00, 0xFF, 0x2F, 0x00,             // end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
        TEST(count == 2);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].note == 60);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::note_on);
        TEST(events[1].note == 62);
        TEST(events[1].time == 11025);          // delta 48 ticks = half a quarter = 11025 samples
    }

    // MIDI file: malformed / truncated inputs return 0 without crashing (checked under ASAN).
    {
        Synth::MidiEvent events[8];
        // Empty buffer.
        TEST(Synth::parse_midi_file(nullptr, 0, events, 8, 44100) == 0);
        // Bad magic.
        static const uint8_t bad_magic[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        TEST(Synth::parse_midi_file(bad_magic, sizeof(bad_magic), events, 8, 44100) == 0);
        // Truncated valid header (track length claims more bytes than present).
        static const uint8_t truncated[] = {
            0x4D, 0x54, 0x68, 0x64,
            0x00, 0x00, 0x00, 0x06,
            0x00, 0x00,
            0x00, 0x01,
            0x00, 0x60,
            0x4D, 0x54, 0x72, 0x6B,
            0x00, 0x00, 0x00, 0x40,             // claims 64 bytes but track is cut off
            0x00, 0x90, 0x3C,                   // incomplete note_on
        };
        TEST(Synth::parse_midi_file(truncated, sizeof(truncated), events, 8, 44100) == 0);
        // SMPTE division (high bit set) is rejected.
        static const uint8_t smpte[] = {
            0x4D, 0x54, 0x68, 0x64,
            0x00, 0x00, 0x00, 0x06,
            0x00, 0x00,
            0x00, 0x01,
            0xE8, 0x04,                         // negative (SMPTE) division
            0x4D, 0x54, 0x72, 0x6B,
            0x00, 0x00, 0x00, 0x04,
            0x00, 0xFF, 0x2F, 0x00,
        };
        TEST(Synth::parse_midi_file(smpte, sizeof(smpte), events, 8, 44100) == 0);
    }

    // A file whose sample time exceeds the 32-bit field is rejected (no UB on the cast).  An
    // extreme tempo (0xFFFFFF us/quarter) at division 1 makes one tick ~740k samples, so a
    // 6000-tick delta overruns UINT32_MAX.
    {
        const uint8_t overflow_midi[] = {
            0x4D, 0x54, 0x68, 0x64,
            0x00, 0x00, 0x00, 0x06,
            0x00, 0x00,                         // format 0
            0x00, 0x01,                         // one track
            0x00, 0x01,                         // division: 1 tick per quarter
            0x4D, 0x54, 0x72, 0x6B,
            0x00, 0x00, 0x00, 0x10,             // track length 16
            0x00, 0xFF, 0x51, 0x03, 0xFF, 0xFF, 0xFF, // tempo 0xFFFFFF us/quarter
            0xAE, 0x70, 0x90, 0x3C, 0x64,       // delta 6000, note_on ch0 note 60 vel 100
            0x00, 0xFF, 0x2F, 0x00,             // end of track
        };
        Synth::MidiEvent overflow_events[8] = { };
        TEST(Synth::parse_midi_file(overflow_midi, sizeof(overflow_midi), overflow_events, 8, 44100) == 0);
    }

    // soundtrack codec round-trips a tick-domain, time-ordered stream.  encode pairs each
    // note_on with its later note_off into a note_on + duration; decode reconstructs the note_off
    // at start + duration.  Times are MIDI ticks.  note_off velocity is not stored (the duration
    // model drops it), so note_offs here carry note_data 0, matching the reconstructed value.
    {
        Synth::MidiEvent events[7] = { };

        events[0].time = 0;   events[0].event = Synth::EvType::note_on;
        events[0].channel = 0; events[0].note = 60; events[0].note_data = 100;

        events[1].time = 0;   events[1].event = Synth::EvType::controller;
        events[1].channel = 2; events[1].controller = 7; events[1].controller_data = 120;

        events[2].time = 50;  events[2].event = Synth::EvType::aftertouch;
        events[2].channel = 0; events[2].note = 60; events[2].note_data = 40;

        events[3].time = 100; events[3].event = Synth::EvType::pitch_bend;
        events[3].channel = 0; events[3].pitch_bend = -2048;

        events[4].time = 100; events[4].event = Synth::EvType::note_on;
        events[4].channel = 2; events[4].note = 67; events[4].note_data = 90;

        events[5].time = 200; events[5].event = Synth::EvType::note_off;
        events[5].channel = 0; events[5].note = 60; events[5].note_data = 0;

        events[6].time = 300; events[6].event = Synth::EvType::note_off;
        events[6].channel = 2; events[6].note = 67; events[6].note_data = 0;

        const uint32_t event_count = 7;
        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = { };
        const uint32_t written = Synth::encode_soundtrack(events, event_count, dest, sizeof(dest),
                                                          &soundtrack);
        TEST(written > 0);

        Synth::MidiEvent decoded[7] = { };
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 7) == event_count);
        TEST(memcmp(events, decoded, event_count * sizeof(Synth::MidiEvent)) == 0);

        // A buffer too small to hold the planes fails cleanly.
        Synth::Soundtrack scratch = { };
        TEST(Synth::encode_soundtrack(events, event_count, dest, 4, &scratch) == 0);

        // An out-of-range channel is rejected.
        Synth::MidiEvent bad_channel = events[0];
        bad_channel.channel = Synth::max_channels;
        TEST(Synth::encode_soundtrack(&bad_channel, 1, dest, sizeof(dest), &scratch) == 0);
    }

    // an unmatched note_on (no note_off) encodes as an unbounded note and decodes back to a
    // single note_on with no reconstructed note_off.
    {
        Synth::MidiEvent events[1] = { };
        events[0].time = 0; events[0].event = Synth::EvType::note_on;
        events[0].channel = 0; events[0].note = 48; events[0].note_data = 80;

        uint8_t           dest[64];
        Synth::Soundtrack soundtrack = { };
        TEST(Synth::encode_soundtrack(events, 1, dest, sizeof(dest), &soundtrack) > 0);

        Synth::MidiEvent decoded[4] = { };
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 4) == 1);
        TEST(decoded[0].event == Synth::EvType::note_on);
        TEST(decoded[0].note == 48 && decoded[0].note_data == 80);
    }

    // D3 (retrigger + multi-byte VLQ): an overlapping same-note retrigger closes the earlier note
    // at the new note_on's time, and large tick deltas/durations (> 0x3FFF) exercise multi-byte VLQ.
    {
        Synth::MidiEvent events[3] = { };
        events[0].time = 0;     events[0].event = Synth::EvType::note_on;
        events[0].channel = 0;  events[0].note = 64; events[0].note_data = 100;
        events[1].time = 20000; events[1].event = Synth::EvType::note_on; // retrigger; delta > 0x3FFF
        events[1].channel = 0;  events[1].note = 64; events[1].note_data = 110;
        events[2].time = 60000; events[2].event = Synth::EvType::note_off; // duration > 0x3FFF
        events[2].channel = 0;  events[2].note = 64; events[2].note_data = 0;

        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = { };
        TEST(Synth::encode_soundtrack(events, 3, dest, sizeof(dest), &soundtrack) > 0);

        // Decoded: note_on@0, note_off@20000 (earlier note closed at retrigger), note_on@20000,
        // note_off@60000.  Sort is by (time, channel); same-time events keep input order.
        Synth::MidiEvent decoded[8] = { };
        const uint32_t decoded_count = Synth::decode_soundtrack(soundtrack, decoded, 8);
        TEST(decoded_count == 4);

        uint32_t note_offs = 0;
        uint32_t off_at_20000 = 0;
        uint32_t off_at_60000 = 0;
        int off_idx_20000 = -1;
        int on_idx_20000  = -1;
        for (uint32_t i = 0; i < decoded_count; i++) {
            if (decoded[i].event == Synth::EvType::note_off) {
                note_offs++;
                if (decoded[i].time == 20000) { off_at_20000++; off_idx_20000 = (int)i; }
                if (decoded[i].time == 60000) { off_at_60000++; }
                TEST(decoded[i].note == 64);
            }
            else if (decoded[i].event == Synth::EvType::note_on && decoded[i].time == 20000) {
                on_idx_20000 = (int)i;
            }
        }
        TEST(note_offs == 2);
        TEST(off_at_20000 == 1); // earlier note closed at the retrigger
        TEST(off_at_60000 == 1); // second note closed by its real note_off
        // The reconstructed note_off must precede the retriggered note_on at the same tick, so the
        // player closes the old voice before allocating the new one (else the retrigger drops it).
        TEST(off_idx_20000 >= 0 && on_idx_20000 >= 0 && off_idx_20000 < on_idx_20000);
    }

    // a note_on's stored duration resolves to the absolute sample at which the player
    // auto-releases its voice (0 = none).  Stored value v means a real duration of v - 1 ticks.
    {
        TEST(Synth::soundtrack_note_release_sample(1000, 0, 50)  == 0);    // unbounded -> none
        TEST(Synth::soundtrack_note_release_sample(1000, 11, 50) == 1500); // 10 ticks * 50 + 1000
        TEST(Synth::soundtrack_note_release_sample(1000, 1, 50)  == 1000); // 0-tick note releases at start
        TEST(Synth::soundtrack_note_release_sample(0, 1, 50)     == 0);    // degenerate: 0-tick at 0 -> none
    }

    // ---- instrument bank persistence (codec) ----

    // Stamp several distinct fields so a memcmp discriminates more than one byte.
    auto stamp_bank = [](Synth::InstrumentBank& b, uint8_t k) {
        b.drum_track_channel              = k;
        b.channel_routes[1][0].start_note = k;
        b.channel_routes[1][0].instrument = static_cast<uint8_t>(k + 1u);
        b.instrument_names[0][0]          = static_cast<char>('A' + (k & 7u));
        b.channel_names[0][0]             = static_cast<char>('z' - (k & 7u));
    };

    // SYIB encode/decode round-trips the whole bank exactly (full-struct memcmp).  Bad input
    // fails cleanly without touching the destination (full-struct check, not one field).
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        stamp_bank(bank, 4);

        static uint8_t blob[sizeof(Synth::InstrumentBank) + 64];
        const uint32_t n = Synth::encode_instrument_bank(&bank, blob, sizeof(blob));
        TEST(n > 0);

        static Synth::InstrumentBank restored;
        memset(&restored, 0x5A, sizeof(restored));
        TEST(Synth::decode_instrument_bank(blob, n, &restored));
        TEST(memcmp(&restored, &bank, sizeof(bank)) == 0);

        // Bad input: truncated / corrupt marker -> false, destination byte-for-byte untouched.
        static Synth::InstrumentBank untouched;
        memset(&untouched, 0x33, sizeof(untouched));
        static Synth::InstrumentBank untouched_ref;
        memset(&untouched_ref, 0x33, sizeof(untouched_ref));
        TEST( ! Synth::decode_instrument_bank(blob, 3, &untouched));        // truncated
        TEST(memcmp(&untouched, &untouched_ref, sizeof(untouched)) == 0);
        blob[0] ^= 0xFFu;                                                   // corrupt marker
        TEST( ! Synth::decode_instrument_bank(blob, n, &untouched));
        TEST(memcmp(&untouched, &untouched_ref, sizeof(untouched)) == 0);
    }

    return exit_code;
}
