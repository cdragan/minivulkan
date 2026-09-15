// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_parameters.h"
#include "synth_effects.h"
#include "synth_effect_expansion.h"
#include "synth_instrument.h"
#include "../sculptor/sculptor_instr_bank.h"
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

namespace {

constexpr uint32_t fake_region_base = 8192;

struct FakeWriter {
    uint32_t next_node;
    uint32_t dest_count;
    uint32_t lfo_count;
    uint16_t last_dest_node;
    uint16_t last_dest_lfo_node;
};

uint32_t fake_alloc_node(const void* ctx)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    return writer.next_node++;
}

uint16_t fake_resolve_source(const void*, Synth::ModSource, uint32_t)
{
    return 77; // arbitrary concrete node id
}

void fake_configure_dest(const void* ctx, uint32_t node, uint16_t lfo_node, float,
        Synth::SourceOp, const Synth::SourceParam*, uint32_t)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.dest_count++;
    writer.last_dest_node = static_cast<uint16_t>(node);
    writer.last_dest_lfo_node = lfo_node;
}

void fake_configure_lfo(const void* ctx, uint32_t, uint16_t, Synth::SourceOp, float, uint16_t, uint16_t, float)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.lfo_count++;
}

const Synth::EffectNodeWriter fake_writer_binding(FakeWriter& writer)
{
    const Synth::EffectNodeWriter result = {
        &writer, fake_alloc_node, fake_resolve_source, fake_configure_dest, fake_configure_lfo
    };
    return result;
}

Synth::EffectSlotBinding* enabled_effect(Synth::InstrumentBank& bank, uint32_t channel, uint32_t slot,
        Synth::EffectType type)
{
    Synth::EffectChainBinding& chain = (channel < Synth::max_channels)
                                     ? bank.channel_chains[channel]
                                     : bank.master_chain;
    chain.num_effects = static_cast<uint8_t>(slot + 1);
    chain.effects[slot].type = type;
    chain.effects[slot].enabled = true;
    return &chain.effects[slot];
}

} // namespace

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

        Synth::LFODescriptor test_lfo = { };
        test_lfo.wave = Synth::WaveType::sine_wave;
        test_lfo.period_ms = 50;
        Synth::configure_lfo(&descs[lfo_id], test_lfo, Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        const Synth::SourceParam inputs[1] = { { input_id, 1.0f, Synth::SourceOp::multiply } };
        Synth::configure_plain(&descs[dest_id], 0.1f, 0, lfo_id, Synth::SourceOp::add, inputs, 1);

        // configure_lfo made the LFO node; configure_plain made the dest a plain node.
        TEST(descs[lfo_id].kind == Synth::ParamKind::lfo);
        TEST(descs[lfo_id].lfo.lfo.wave == Synth::WaveType::sine_wave);
        TEST(approx(descs[lfo_id].lfo.lfo.period_ms, 50.0f, 0.001f)); // captured copy
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

    // Keyboard split routing: route_instrument maps a note to an instrument via an ordered
    // table; stored starts are first-note + 1 (0 = unused slot), so an all-zero table resolves to instrument 0.
    {
        const uint32_t count = Synth::max_instr_per_channel;

        Synth::Zone empty[Synth::max_instr_per_channel] = { };
        TEST(Synth::route_instrument(empty, count, 0)   == 0);
        TEST(Synth::route_instrument(empty, count, 60)  == 0);
        TEST(Synth::route_instrument(empty, count, 127) == 0);

        // Notes 0..59 -> instrument 0, notes 60.. -> instrument 1.  Slot 0 stores 1
        // (zone 0 starts at note 0); 0 is the end-of-table sentinel.
        Synth::Zone split[Synth::max_instr_per_channel] = { };
        split[0] = { 1,  0 };
        split[1] = { 61, 1 };;
        TEST(Synth::route_instrument(split, count, 0)   == 0);
        TEST(Synth::route_instrument(split, count, 59)  == 0);
        TEST(Synth::route_instrument(split, count, 60)  == 1);
        TEST(Synth::route_instrument(split, count, 127) == 1);

        Synth::Zone three[Synth::max_instr_per_channel] = { };
        three[0] = { 1,  2 };
        three[1] = { 49, 4 };;
        three[2] = { 73, 3 };;
        TEST(Synth::route_instrument(three, count, 47)  == 2);
        TEST(Synth::route_instrument(three, count, 48)  == 4);
        TEST(Synth::route_instrument(three, count, 71)  == 4);
        TEST(Synth::route_instrument(three, count, 72)  == 3);

        // A full table with no sentinel still resolves the top range.
        Synth::Zone full[Synth::max_instr_per_channel] = { };
        for (uint32_t idx = 0; idx < count; idx++) {
            full[idx] = { static_cast<uint8_t>(idx + 1), static_cast<uint8_t>(idx) };
        }
        TEST(Synth::route_instrument(full, count, 127) == count - 1);
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
        bank.channel_zones[0][0] = { 1, 4 };       // note >= 1 -> instrument slot 4 (survives)
        bank.channel_zones[0][1] = { 65, 3 };      // references deleted instrument slot 3

        uint32_t instr_map[Synth::max_instruments];
        bank.instruments.defragment(instr_map);
        TEST(instr_map[4] == 2);
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

        // Valid descriptor content: the decoder now fully validates banks.
        bank.envelopes.entries[env].num_points = 2;
        bank.envelopes.entries[env].points[1].position = 100;
        bank.lfos.entries[lfo].wave = Synth::WaveType::sine_wave;
        bank.lfos.entries[lfo].period_ms = 50;

        bank.instruments.entries[instr].layer_count = 2;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id =
            static_cast<uint16_t>(env + 1);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id =
            static_cast<uint16_t>(lfo + 1);
        bank.channel_zones[0][0] = { 1, static_cast<uint8_t>(instr) };
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
        bank.channel_zones[2][0] = { 65, static_cast<uint8_t>(instr) };

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
        b.instruments.allocate();
        b.instruments.entries[0].layer_count = 1;
        b.channel_zones[1][0].start_note = k;
        b.channel_zones[1][0].instrument = 0;
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

    // ---- Instrument bank: dense-pool validation, serialize, publish queue ----

    // Builds a small valid bank: one instrument, one envelope, one LFO, one zone.
    auto make_valid_bank = [](Synth::InstrumentBank& bank) {
        memset(&bank, 0, sizeof(bank));
        const uint32_t env = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points = 2;
        bank.envelopes.entries[env].points[1].position = 100;
        const uint32_t lfo = bank.lfos.allocate();
        bank.lfos.entries[lfo].wave = Synth::WaveType::sine_wave;
        bank.lfos.entries[lfo].period_ms = 50;
        const uint32_t instr = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 1;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(env + 1);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id = static_cast<uint16_t>(lfo + 1);
        bank.channel_zones[0][0] = { 1, static_cast<uint8_t>(instr) };
        bank.channel_enabled[0] = 1;
        bank.channel_enabled[1] = 1; // zone-rule negatives exercise channel 1
    };

    // A valid bank passes validation.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Default recipe: init_default_bank builds the first-run bank; every structural
    // invariant the editor and runtime rely on holds out of the box.
    {
        static Synth::InstrumentBank bank;
        Synth::init_default_bank(&bank);
        TEST(Synth::validate_instrument_bank(&bank));
        TEST(bank.channel_enabled[0] == 1);
        bool others_disabled = true;
        for (uint32_t c = 1; c < Synth::max_channels; c++) {
            others_disabled = others_disabled && bank.channel_enabled[c] == 0;
        }
        TEST(others_disabled);
        TEST(memcmp(bank.channel_names[0], "Channel 01", 11) == 0);
        TEST(memcmp(bank.channel_names[9], "Drum Track", 11) == 0);
        TEST(memcmp(bank.channel_names[15], "Channel 16", 11) == 0);
        TEST(bank.drum_track_channel == 9);
        TEST(bank.channel_zones[0][0].start_note == 1);
        TEST(bank.channel_zones[0][0].instrument == 0);
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.envelopes.num_allocated == 1);
        TEST(bank.lfos.num_allocated == 3); // vibrato, tremolo, master FIR sweep
        TEST(bank.channel_chains[0].num_effects == 1);
        TEST(bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(bank.channel_chains[0].effects[0].enabled);
        TEST(bank.master_chain.num_effects == 3);
        // The default instrument wires its descriptors through the local-id remap.
        const Synth::Instrument& instr = bank.instruments.entries[0];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(instr.layers[0].gen[Synth::mod_pitch].lfo_desc_id == 1);
        TEST(instr.layers[0].gen[Synth::mod_volume].lfo_desc_id == 2);
        TEST(bank.master_chain.effects[2].bindings[0].lfo_desc_id == 3);
    }

    // init_default_channel into a populated bank appends the recipe into the free slots and
    // remaps its descriptor references past the existing content.
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        TEST(bank.envelopes.allocate() == 0);
        bank.envelopes.entries[0].num_points = 2;
        bank.envelopes.entries[0].points[1].position = 100;
        TEST(bank.lfos.allocate() == 0);
        bank.lfos.entries[0].wave = Synth::WaveType::sine_wave;
        bank.lfos.entries[0].period_ms = 50;
        TEST(bank.instruments.allocate() == 0);
        bank.instruments.entries[0].layer_count = 1;
        TEST(Synth::init_default_channel(&bank, 5));
        TEST(bank.instruments.num_allocated == 2);
        TEST(bank.envelopes.num_allocated == 2);
        TEST(bank.lfos.num_allocated == 3);
        TEST(bank.channel_zones[5][0].start_note == 1);
        TEST(bank.channel_zones[5][0].instrument == 1);
        const Synth::Instrument& instr = bank.instruments.entries[1];
        TEST(instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 2);
        TEST(instr.layers[0].gen[Synth::mod_pitch].lfo_desc_id == 2);
        TEST(instr.layers[0].gen[Synth::mod_volume].lfo_desc_id == 3);
        TEST(bank.channel_chains[5].num_effects == 1);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // init_default_channel fails cleanly when a pool lacks space, leaving the bank
    // byte-for-byte untouched.
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        bank.instruments.entries[0].layer_count = 1;
        TEST(bank.instruments.allocate() == 0);
        for (uint32_t i = 0; i < Synth::max_lfos; i++) {
            TEST(bank.lfos.allocate() == i);
        }
        static Synth::InstrumentBank before;
        memcpy(&before, &bank, sizeof(bank));
        TEST( ! Synth::init_default_channel(&bank, 2));
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // reclaim_unused_slots removes instruments no enabled channel's zone table references and
    // descriptors no surviving instrument or effect chain references, compacting the pools and
    // remapping zones, names, and references in one pass.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank); // instrument 0 (env 1, lfo 1) on ch0; ch0 and ch1 enabled

        TEST(bank.instruments.allocate() == 1);
        bank.instruments.entries[1].layer_count = 1;
        bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id = 1; // shares env 1
        TEST(bank.instruments.allocate() == 2);
        bank.instruments.entries[2].layer_count = 1;
        TEST(bank.envelopes.allocate() == 1);
        bank.envelopes.entries[1].num_points = 2;
        bank.envelopes.entries[1].points[1].position = 200;
        bank.instruments.entries[2].layers[0].gen[Synth::mod_volume].envelope_desc_id = 2; // env 2 roots nothing but instr 2
        bank.channel_zones[1][0] = { 1, 1 }; // ch1 -> instrument 1

        memcpy(bank.instrument_names[0], "KeepA", 6);
        memcpy(bank.instrument_names[1], "KeepB", 6);
        memcpy(bank.instrument_names[2], "Drop", 5);
        TEST(Synth::validate_instrument_bank(&bank));

        Synth::reclaim_unused_slots(&bank);

        // The orphaned instrument is gone; survivors keep their relative order, names included.
        TEST(bank.instruments.num_allocated == 2);
        TEST(bank.channel_zones[0][0].instrument == 0);
        TEST(bank.channel_zones[1][0].instrument == 1);
        TEST(memcmp(bank.instrument_names[0], "KeepA", 6) == 0);
        TEST(memcmp(bank.instrument_names[1], "KeepB", 6) == 0);
        // Env 2 (referenced only by the removed instrument) is reclaimed; env 1 keeps id 1.
        TEST(bank.envelopes.num_allocated == 1);
        TEST(bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Effect chains root LFOs: with every channel disabled the chain's LFO survives while the
    // instrument's LFO is reclaimed, and the chain's reference is remapped to the compacted id.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank); // lfo 1 referenced by instrument 0
        TEST(bank.lfos.allocate() == 1);
        bank.lfos.entries[1].wave = Synth::WaveType::sawtooth_wave;
        bank.lfos.entries[1].period_ms = 90;
        bank.master_chain.num_effects = 1;
        bank.master_chain.effects[0].type = Synth::EffectType::distortion;
        bank.master_chain.effects[0].enabled = true;
        bank.master_chain.effects[0].bindings[0].base_value = 1.0f;
        bank.master_chain.effects[0].bindings[0].lfo_desc_id = 2;
        TEST(Synth::validate_instrument_bank(&bank));

        bank.channel_enabled[0] = 0;
        bank.channel_enabled[1] = 0;
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.instruments.num_allocated == 0);
        TEST(bank.lfos.num_allocated == 1);
        TEST(bank.master_chain.effects[0].bindings[0].lfo_desc_id == 1); // remapped 2 -> 1
        TEST(bank.lfos.entries[0].wave == Synth::WaveType::sawtooth_wave);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // A clean bank reclaims nothing: the bank comes back byte-for-byte identical.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        static Synth::InstrumentBank before;
        memcpy(&before, &bank, sizeof(bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // Disabled channels may hold stale zone bytes; reclaim clears references to removed
    // instruments and remaps the rest without touching the (skipped) zone structure.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank); // instr 0 on ch0
        TEST(bank.instruments.allocate() == 1);
        bank.instruments.entries[1].layer_count = 1;
        bank.channel_zones[1][0] = { 1, 1 }; // ch1 -> instr 1
        TEST(bank.instruments.allocate() == 2);
        bank.instruments.entries[2].layer_count = 1; // orphaned
        bank.channel_enabled[0] = 0; // instr 0 loses its last live reference
        bank.channel_zones[2][0] = { 5, 7 };  // ch2 disabled: dangling reference
        bank.channel_zones[2][1] = { 9, 0 };  // reference to the instrument about to be removed
        bank.channel_zones[2][2] = { 13, 1 }; // reference to a survivor (compacts 1 -> 0)
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.envelopes.num_allocated == 0); // env 1 rooted instr 0 only
        TEST(bank.channel_zones[2][0].instrument == 0);
        TEST(bank.channel_zones[2][1].instrument == 0);
        TEST(bank.channel_zones[2][2].instrument == 0);
        TEST(bank.channel_zones[1][0].instrument == 0);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Validation negatives: each mutation makes exactly the targeted rule reject the bank.
    auto expect_invalid = [](Synth::InstrumentBank& bank) {
        TEST( ! Synth::validate_instrument_bank(&bank));
    };
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.num_allocated = Synth::max_instruments + 1; // over capacity
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.num_allocated = 2; // count mismatch: only 1 occupied
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.free(0); // hole below num_allocated: count/occupied mismatch
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 11, 0 };
        bank.channel_zones[1][2] = { 6, 0 }; // later zone starts before the earlier one
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 0, 0 };
        bank.channel_zones[1][2] = { 20, 0 }; // garbage after the terminator
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 200 }; // slot 0 instrument unoccupied (dangling)
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 20, 1 }; // reachable entry references an unoccupied instrument
        expect_invalid(bank);
    }
    // Slot 0 must start at note 0 (stored start 1); any other first start is invalid.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 5, 0 };
        expect_invalid(bank);
    }
    // Disabled channels keep arbitrary zone bytes: garbage zones pass while disabled and
    // fail once the channel is enabled.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[3][0] = { 9, 7 };
        bank.channel_zones[3][1] = { 2, 80 }; // out of order
        bank.channel_enabled[3] = 0;
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_enabled[3] = 1;
        expect_invalid(bank);
    }
    // channel_enabled bytes must be 0/1.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_enabled[2] = 2;
        expect_invalid(bank);
    }
    // Unterminated name fields are rejected (imported files are untrusted byte images).
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        memset(bank.instrument_names[0], 0xAB, Synth::max_name_len);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        memset(bank.channel_names[5], 0xAB, Synth::max_name_len);
        expect_invalid(bank);
    }
    // A full zone table with no terminator is valid (route_instrument semantics unchanged).
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        for (uint32_t e = 0; e < Synth::max_instr_per_channel; e++) {
            bank.channel_zones[1][e] = { static_cast<uint8_t>(e + 1), 0 };
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id = 5; // dangling
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].lfo_desc_id = 9; // dangling
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = Synth::max_envelope_points + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points = 3;
        env.points[2].position = env.points[1].position; // duplicate position
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points = 3;
        env.points[2].position = 50; // decreasing position
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.sustain_first_point = 2; // sustain_first > sustain_last
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].sustain_last_point = Synth::max_envelope_points;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].period_ms = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::no_wave; // not runtime-supported
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::pulse_wave;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::noise_wave;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[0] = static_cast<Synth::WaveType>(99);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[1] = static_cast<Synth::WaveType>(200);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_mode = static_cast<Synth::OscMode>(3);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layer_count = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layer_count = Synth::max_layers + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[0].lfo_depth_source = static_cast<Synth::ModSource>(77);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[0].lfo_op = static_cast<Synth::SourceOp>(9);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].routing[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].routing[0].num_inputs = 1;
        bank.instruments.entries[0].routing[0].inputs[0].op = static_cast<Synth::SourceOp>(7);
        expect_invalid(bank);
    }

    // ---- Effect chains in the bank (schema, validation, codec) ----

    TEST(Synth::get_effect_state_bytes(Synth::EffectType::delay) == 353024); // 88201 floats, 256-aligned

    // A chain with one enabled delay and an LFO-driven param passes validation.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain = bank.channel_chains[0];
        chain.num_effects = 1;
        chain.effects[0].type = Synth::EffectType::delay;
        chain.effects[0].enabled = true;
        chain.effects[0].bindings[0].base_value = 250.0f;
        chain.effects[0].bindings[0].lfo_desc_id = 1; // make_valid_bank allocated one LFO
        chain.effects[0].bindings[0].lfo_op = Synth::SourceOp::add;
        chain.effects[0].bindings[0].lfo_depth = 100.0f;
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::num_types; // invalid type
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = Synth::max_chain_effects + 1; // over capacity
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9; // dangling LFO ref
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = 1;
        bank.master_chain.effects[0].type = Synth::EffectType::delay;
        bank.master_chain.effects[0].bindings[0].num_inputs = 1; // master chain: no MIDI inputs
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = 1;
        bank.master_chain.effects[0].type = Synth::EffectType::delay;
        bank.master_chain.effects[0].bindings[0].lfo_desc_id = 1;
        bank.master_chain.effects[0].bindings[0].lfo_depth_source = Synth::ModSource::pitch_bend; // master: LFO-only
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::velocity; // per-note role
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].base_value = NAN;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale = INFINITY;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects; // 4 delays: exactly the state budget
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects;
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        bank.channel_chains[0].num_effects = 1; // 5th delay: over the whole-bank state budget
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true;
        expect_invalid(bank);
    }
    {
        // Exactly 32 modulated effect params pass; one more is rejected.
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 32; channel++) {
            Synth::EffectChainBinding& chain = bank.channel_chains[channel];
            for (uint32_t slot = 0; slot < Synth::max_chain_effects && num_modulated < 32; slot++) {
                chain.num_effects = static_cast<uint8_t>(slot + 1);
                chain.effects[slot].type = Synth::EffectType::distortion; // 2 params, no state
                for (uint32_t param = 0; param < 2 && num_modulated < 32; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 32);
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_chains[15].num_effects = Synth::max_chain_effects;
        bank.channel_chains[15].effects[3].type = Synth::EffectType::distortion;
        bank.channel_chains[15].effects[3].bindings[1].lfo_desc_id = 1; // 33rd modulated param
        expect_invalid(bank);
    }

    // Effect chain data round-trips through the codec byte-exactly.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = 2;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true;
        bank.channel_chains[0].effects[0].bindings[0].base_value = 300.0f;
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth = 50.0f;
        bank.channel_chains[0].effects[1].type = Synth::EffectType::chorus;
        bank.channel_chains[0].effects[1].bindings[1].num_inputs = 1;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].source = Synth::ModSource::mod_wheel;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].op = Synth::SourceOp::multiply;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].scale = 0.5f;
        bank.master_chain.num_effects = 1;
        bank.master_chain.effects[0].type = Synth::EffectType::reverb;
        bank.master_chain.effects[0].enabled = true;
        bank.master_chain.effects[0].bindings[0].base_value = 0.25f;

        static uint8_t blob[sizeof(Synth::InstrumentBank) + 64];
        const uint32_t n = Synth::encode_instrument_bank(&bank, blob, sizeof(blob));
        TEST(n > 0);
        static Synth::InstrumentBank restored;
        memset(&restored, 0x5A, sizeof(restored));
        TEST(Synth::decode_instrument_bank(blob, n, &restored));
        TEST(memcmp(&restored, &bank, sizeof(bank)) == 0);
    }

    // ---- Effect expansion: preflight/commit transaction over a fake node writer ----

    // A stateful chain expands onto pool nodes with state at the region base, one clear range.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].base_value = 300.0f;
        delay->bindings[0].lfo_desc_id = 1;
        delay->bindings[0].lfo_depth = 50.0f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.slots[0][0].allocated_for == Synth::EffectType::delay);
        TEST(plan.num_nodes == 2); // dest + LFO leaf
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].params[0] == 300.0f);
        TEST(chains[0].effects[0].state_offs == fake_region_base);
        TEST(chains[0].effects[0].src_param_id[0] == 50); // dest node allocated first
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        TEST(writer.last_dest_lfo_node == 51); // LFO leaf allocated second

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        TEST(Synth::take_effect_clear_ranges().count == 0); // consumed once
    }
    // An unchanged slot keeps its state offset and emits no clear; a type change re-allocates.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        // Same effect: offset preserved, nothing to clear, no new consumption.
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST( ! plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(Synth::take_effect_clear_ranges().count == 0);

        // Type change: fresh allocation at the bump position, cleared before first use.
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);
        TEST(plan.slots[0][0].state_offs == fake_region_base + delay_bytes);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == delay_bytes + Synth::get_effect_state_bytes(Synth::EffectType::chorus));
    }
    // Over-budget and over-pool preflights fail without touching committed state.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;

        // Master: 4 delays - exactly the worst chain, fits the budget.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();
        const uint32_t committed_consumed = plan.consumed_bytes;

        // A 5th delay anywhere exceeds the budget: preflight fails, nothing mutates.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed); // plan untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // no clears recorded

        // The committed configuration (5th delay dropped again) still re-preflights
        // cleanly via the preservation path.
        bank.channel_chains[0].num_effects = 0;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed);

        // 33 modulated params exceed the modulation pool: rejected wherever they sit.
        Synth::init_effect_state_region(fake_region_base);
        static Synth::InstrumentBank pool_bank;
        make_valid_bank(pool_bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 33; channel++) {
            Synth::EffectChainBinding& chain = pool_bank.channel_chains[channel];
            chain.num_effects = 4;
            for (uint32_t slot = 0; slot < 4 && num_modulated < 33; slot++) {
                chain.effects[slot].type = Synth::EffectType::distortion; // 2 params, no state
                for (uint32_t param = 0; param < 2 && num_modulated < 33; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 33);
        TEST( ! Synth::preflight_effect_expansion(pool_bank, &plan, &error));

        // An input-only binding (no LFO) costs one node, not two.
        Synth::init_effect_state_region(fake_region_base);
        static Synth::InstrumentBank input_bank;
        make_valid_bank(input_bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(input_bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs = 1;
        delay->bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        TEST(Synth::preflight_effect_expansion(input_bank, &plan, &error));
        TEST(plan.num_nodes == 1);
    }
    // A shrinking chain leaves no stale instances behind.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 0, 1, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 2);

        bank.channel_chains[0].num_effects = 1;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[1].type == Synth::EffectType::none);
        TEST( ! chains[0].effects[1].enabled);
        TEST(chains[0].effects[1].state_offs == 0);
        TEST(chains[0].effects[1].src_param_id[0] == 0);
    }

    // Stateless effects (distortion) claim no state, no offset and no clear range.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const distortion = enabled_effect(bank, 0, 0, Synth::EffectType::distortion);
        distortion->bindings[0].base_value = 5.0f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == 0);
        TEST( ! plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == 0);

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].state_offs == 0);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // A preflight that fails validation leaves the caller's plan byte-identical. The commit
    // is only reachable after a passing preflight (set_current_bank returns early on
    // failure), so a rejected bank can never reach the writer or the chains.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs = Synth::max_mod_inputs + 1; // over the input bound

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        plan.consumed_bytes = 0xABCD;
        const char* error = nullptr;
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 0xABCD); // untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // nothing recorded either
    }
    // Each commit-safety rule the writer relies on is checked by the preflight.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        const char* error = nullptr;
        static Synth::EffectExpansionPlan plan;

        Synth::init_effect_state_region(fake_region_base);

        // Dangling LFO descriptor.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9;
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Non-finite LFO depth.
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth = NAN;
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Per-note MIDI source on a channel effect.
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth = 1.0f;
        bank.channel_chains[0].effects[0].bindings[1].num_inputs = 1;
        bank.channel_chains[0].effects[0].bindings[1].inputs[0].source = Synth::ModSource::velocity;
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Any MIDI source on the master chain (here: via the LFO depth source).
        Synth::EffectSlotBinding* const reverb = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        reverb->bindings[0].lfo_desc_id = 1;
        reverb->bindings[0].lfo_depth_source = Synth::ModSource::mod_wheel;
        TEST( ! Synth::preflight_effect_expansion(bank, &plan, &error));
    }
    // Clears accumulate across publishes drained in one step; the take hands out all of them.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        enabled_effect(bank, 1, 0, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const uint32_t chorus_bytes = Synth::get_effect_state_bytes(Synth::EffectType::chorus);
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);

        // Re-type both slots: the drained second publish allocates fresh state and clears it.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 1, 0, Synth::EffectType::delay);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 4); // two commits x two stateful slots, none consumed in between
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == chorus_bytes);
        TEST(clears.ranges[2].offset == fake_region_base + 2 * chorus_bytes); // second commit, fresh
        TEST(clears.ranges[2].bytes == delay_bytes);
        TEST(clears.ranges[3].offset == fake_region_base + 2 * chorus_bytes + delay_bytes);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // Channel 15 and the master chain expand like any other chain.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const ch15 = enabled_effect(bank, Synth::max_channels - 1, 0, Synth::EffectType::delay);
        ch15->bindings[0].base_value = 100.0f;
        Synth::EffectSlotBinding* const rev = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        rev->bindings[0].base_value = 0.7f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[Synth::max_channels - 1][0].state_offs == fake_region_base);
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);
        TEST(plan.slots[Synth::max_channels][0].state_offs == fake_region_base + delay_bytes);

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[Synth::max_channels - 1].effects[0].params[0] == 100.0f);
        TEST(master.effects[0].type == Synth::EffectType::reverb);
        TEST(master.effects[0].params[0] == 0.7f);
        TEST(master.effects[0].state_offs == fake_region_base + delay_bytes);
    }
    // The clear ring holds three full commits: the init commit's ranges stay pending until
    // the first render step, where the drain can apply both queued banks.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const chorus = enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        chorus->bindings[0].base_value = 1.5f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char* error = nullptr;
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter writer = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        for (uint32_t republish = 0; republish < 2; republish++) {
            // Each drained publish re-types the slot, so it allocates and clears fresh state.
            enabled_effect(bank, 0, 0, (republish % 2) ? Synth::EffectType::chorus
                    : Synth::EffectType::delay);
            TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        }

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 3); // init commit + two drained publishes, none consumed in between
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }


    // Target-never-mutated: malformed images (each breaking a different validated invariant)
    // are rejected by decode and leave the destination byte-identical.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        static uint8_t image[Synth::instrument_bank_image_size];
        TEST(Synth::encode_instrument_bank(&bank, image, sizeof(image)) == Synth::instrument_bank_image_size);

        // Byte offsets of the interesting fields inside the encoded image (header + bank copy).
        const uint8_t* const bank_base = reinterpret_cast<const uint8_t*>(&bank);
        auto image_offset = [bank_base](const void* field) {
            return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(field) - bank_base)
                + Synth::instrument_bank_header_size;
        };
        const uint32_t env_points_off = image_offset(&bank.envelopes.entries[0].num_points);
        const uint32_t env_pos1_off = image_offset(&bank.envelopes.entries[0].points[1].position);
        const uint32_t env_sustain_first_off = image_offset(&bank.envelopes.entries[0].sustain_first_point);
        const uint32_t env_sustain_last_off = image_offset(&bank.envelopes.entries[0].sustain_last_point);
        const uint32_t lfo_period_off = image_offset(&bank.lfos.entries[0].period_ms);
        const uint32_t lfo_wave_off = image_offset(&bank.lfos.entries[0].wave);
        const uint32_t instr_layer_count_off = image_offset(&bank.instruments.entries[0].layer_count);
        const uint32_t osc_type0_off = image_offset(&bank.instruments.entries[0].layers[0].osc_type[0]);
        const uint32_t osc_mode_off = image_offset(&bank.instruments.entries[0].layers[0].osc_mode);
        const uint32_t gen_env_id_off = image_offset(&bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id);
        const uint32_t gen_lfo_id_off = image_offset(&bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].lfo_desc_id);
        const uint32_t num_inputs_off = image_offset(&bank.instruments.entries[0].routing[0].num_inputs);
        const uint32_t lfo_depth_source_off = image_offset(&bank.instruments.entries[0].layers[0].gen[0].lfo_depth_source);
        const uint32_t instr_count_off = image_offset(&bank.instruments.num_allocated);

        auto decode_must_reject = [&](const uint8_t* bad) {
            static Synth::InstrumentBank untouched;
            memset(&untouched, 0x5A, sizeof(untouched));
            static Synth::InstrumentBank reference;
            memset(&reference, 0x5A, sizeof(reference));
            TEST( ! Synth::decode_instrument_bank(bad, sizeof(bad), &untouched));
            TEST(memcmp(&untouched, &reference, sizeof(untouched)) == 0);
        };

        static uint8_t bad[Synth::instrument_bank_image_size];
        memcpy(bad, image, sizeof(bad)); bad[env_points_off] = 0; decode_must_reject(bad); // 0 envelope points
        memcpy(bad, image, sizeof(bad)); bad[env_points_off] = Synth::max_envelope_points + 1; decode_must_reject(bad); // 9 points
        memcpy(bad, image, sizeof(bad)); bad[env_pos1_off] = 0; decode_must_reject(bad); // duplicate positions
        memcpy(bad, image, sizeof(bad)); bad[env_sustain_first_off] = 2; bad[env_sustain_last_off] = 0; decode_must_reject(bad); // bad sustain
        memcpy(bad, image, sizeof(bad)); bad[lfo_period_off] = 0; bad[lfo_period_off + 1] = 0; decode_must_reject(bad); // zero LFO period
        memcpy(bad, image, sizeof(bad)); bad[lfo_wave_off] = static_cast<uint8_t>(Synth::WaveType::pulse_wave); decode_must_reject(bad); // bad LFO wave
        memcpy(bad, image, sizeof(bad)); bad[osc_type0_off] = 99; decode_must_reject(bad); // bad osc_type
        memcpy(bad, image, sizeof(bad)); bad[osc_mode_off] = 3; decode_must_reject(bad); // bad osc_mode
        memcpy(bad, image, sizeof(bad)); bad[gen_env_id_off] = 5; decode_must_reject(bad); // dangling envelope ref
        memcpy(bad, image, sizeof(bad)); bad[gen_lfo_id_off] = 9; decode_must_reject(bad); // dangling LFO ref
        memcpy(bad, image, sizeof(bad)); bad[num_inputs_off] = Synth::max_mod_inputs + 1; decode_must_reject(bad); // too many inputs
        memcpy(bad, image, sizeof(bad)); bad[lfo_depth_source_off] = 77; decode_must_reject(bad); // bad ModSource
        memcpy(bad, image, sizeof(bad)); bad[instr_count_off] = 0xFF; decode_must_reject(bad); // num_allocated over capacity
        memcpy(bad, image, sizeof(bad)); bad[instr_layer_count_off] = 0; decode_must_reject(bad); // layer_count 0
    }

    // Queue mechanics: room condition, wraparound, ordering, two-phase consume.
    // Packets are whole banks; instruments.entries[0].layer_count marks each one.
    {
        static Synth::BankUpdateQueue queue;
        memset(&queue, 0, sizeof(queue));

        Synth::InstrumentBank bank = { };
        bank.instruments.allocate();

        // Fill the queue (capacity 2).
        for (uint32_t k = 0; k < Synth::bank_queue_capacity; k++) {
            bank.instruments.entries[0].layer_count = k + 1;
            TEST(Synth::push_bank_update(&queue, bank));
        }
        // Third push fails at tail=2, head=0.
        bank.instruments.entries[0].layer_count = 99;
        TEST( ! Synth::push_bank_update(&queue, bank));

        // Consume one: the third push now succeeds (wraparound: slot 0 reused).
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 1);
        Synth::consume_bank_update(&queue);
        TEST(Synth::push_bank_update(&queue, bank));

        // Drain in order: 2, then 99.
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 2);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 99);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue) == nullptr);
    }
    // get_zone_name: named instrument and the "Zone X" fallback.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        memcpy(bank.instrument_names[0], "Lead", 5);
        char name[Synth::max_name_len];
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Lead") == 0);

        bank.instrument_names[0][0] = 0;
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Zone 0") == 0);

        bank.channel_zones[0][1] = { 50, 0 };
        Synth::get_zone_name(&bank, 0, 1, name, sizeof(name));
        TEST(strcmp(name, "Zone 1") == 0);
    }

    // Capture semantics: a configured generator node holds its descriptor BY VALUE; mutating
    // (or replacing) the bank afterwards cannot change the node's sound.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);

        Synth::ParamDescriptor env_node = { };
        env_node.kind = Synth::ParamKind::envelope;
        env_node.envelope = bank.envelopes.entries[0];
        bank.envelopes.entries[0].num_points = 7; // edit the bank entry
        bank.envelopes.entries[0].points[1].position = 12345;
        TEST(env_node.envelope.num_points == 2); // node kept its own copy
        TEST(env_node.envelope.points[1].position == 100);

        Synth::ParamDescriptor lfo_node = { };
        Synth::configure_lfo(&lfo_node, bank.lfos.entries[0], Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        bank.lfos.entries[0].period_ms = 9999;
        TEST(lfo_node.lfo.lfo.period_ms == 50); // node kept its own copy
    }

    // Full-bank publish flow: two complete banks queued (coalesced upstream), drained in
    // order; the runtime bank ends at the newest and the queue is empty.
    {
        static Synth::InstrumentBank banks[2];
        static Synth::InstrumentBank runtime;
        static Synth::BankUpdateQueue queue;
        memset(&queue, 0, sizeof(queue));
        make_valid_bank(banks[0]);
        make_valid_bank(banks[1]);
        banks[0].instruments.entries[0].routing[Synth::mod_volume].base_value = 0.25f;
        banks[1].instruments.entries[0].routing[Synth::mod_volume].base_value = 0.75f;

        TEST(Synth::validate_instrument_bank(&banks[0]));
        TEST(Synth::validate_instrument_bank(&banks[1]));
        TEST(Synth::push_bank_update(&queue, banks[0]));
        TEST(Synth::push_bank_update(&queue, banks[1]));

        // Audio-side drain: copy each peeked bank, then consume (as the editor app's audio-step hook does).
        while (const Synth::InstrumentBank* packet = Synth::peek_bank_update(&queue)) {
            runtime = *packet;
            Synth::consume_bank_update(&queue);
        }
        TEST(approx(runtime.instruments.entries[0].routing[Synth::mod_volume].base_value, 0.75f, 0.001f));
        TEST(runtime.instruments.num_allocated == 1);
    }


    // ---- Zone table helpers: lookup, boundary moves, splits ----
    {
        static Synth::InstrumentBank bank;
        Synth::init_default_bank(&bank);
        Synth::Zone* zones = bank.channel_zones[0];

        // Default channel: one zone covering 0..127, instrument 1.
        TEST(Synth::zone_entry_at(zones, 0) == 0);
        TEST(Synth::zone_entry_at(zones, 64) == 0);
        TEST(Synth::zone_entry_at(zones, 127) == 0);

        // Split at 60: zone 0 keeps 0..59, new zone 1 covers 60..127 with a cloned
        // instrument (same bytes, name copied).  The instrument byte is the pool slot
        // directly, so the default zone holds slot 0.
        TEST(Synth::zone_split_new(zones, 0, 60, &bank));
        TEST(zones[0].start_note == 1 && zones[0].instrument == 0);
        TEST(zones[1].start_note == 61 && zones[1].instrument == 1);
        TEST(zones[2].start_note == 0);
        TEST(bank.instruments.num_allocated == 2);
        TEST(memcmp(bank.instruments.entries, bank.instruments.entries + 1, sizeof(Synth::Instrument)) == 0);
        TEST(strcmp(bank.instrument_names[0], bank.instrument_names[1]) == 0);
        TEST(Synth::zone_entry_at(zones, 59) == 0);
        TEST(Synth::zone_entry_at(zones, 60) == 1);

        // Renaming the clone does not touch the original.
        strcpy(bank.instrument_names[1], "Clone");
        TEST(strcmp(bank.instrument_names[0], "Clone") != 0);

        // Splitting at a zone's first note replaces its instrument in place (no new
        // zone slot needed).
        TEST(Synth::zone_split_new(zones, 1, 60, &bank));
        TEST(zones[1].start_note == 61 && zones[1].instrument == 2);
        TEST(bank.instruments.num_allocated == 3);
        TEST(Synth::zone_entry_at(zones, 59) == 0);
        TEST(Synth::zone_entry_at(zones, 60) == 1);

        // join_next: note 50 joins zone 1, which starts at 50.
        TEST(Synth::zone_join_next(zones, 0, 50));
        TEST(zones[1].start_note == 51);
        TEST(Synth::zone_entry_at(zones, 50) == 1);
        TEST(Synth::zone_entry_at(zones, 49) == 0);

        // join_previous: note 50 joins zone 0, which pushes zone 1's start to 51.
        TEST(Synth::zone_join_previous(zones, 1, 50));
        TEST(zones[1].start_note == 52);
        TEST(Synth::zone_entry_at(zones, 50) == 0);
        TEST(Synth::zone_entry_at(zones, 51) == 1);

        // Moving a zone's last note into the previous zone removes the zone.
        TEST(Synth::zone_join_previous(zones, 1, 127));
        TEST(zones[0].start_note == 1 && zones[0].instrument == 0);
        TEST(zones[1].start_note == 0);
        TEST(Synth::zone_entry_at(zones, 127) == 0);

        // Three-zone table: [0..20] [21..70] [71..127].
        Synth::Zone* z = bank.channel_zones[1];
        z[0] = Synth::Zone{ 1, 0 };
        z[1] = Synth::Zone{ 22, 1 };
        z[2] = Synth::Zone{ 72, 2 };
        z[3] = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_entry_at(z, 0) == 0);
        TEST(Synth::zone_entry_at(z, 20) == 0);
        TEST(Synth::zone_entry_at(z, 21) == 1);
        TEST(Synth::zone_entry_at(z, 70) == 1);
        TEST(Synth::zone_entry_at(z, 71) == 2);
        TEST(Synth::zone_entry_at(z, 127) == 2);

        // Clicking an existing boundary: 21 is zone 1's first note; "add to previous
        // zone" moves it into zone 0, and zone 1 starts at 22.
        TEST(Synth::zone_join_previous(z, 1, 21));
        TEST(z[0].start_note == 1 && z[1].start_note == 23);
        TEST(Synth::zone_entry_at(z, 21) == 0);
        TEST(Synth::zone_entry_at(z, 22) == 1);

        // 70 is zone 1's last note; "add to next zone" moves it into zone 2.
        TEST(Synth::zone_join_next(z, 1, 70));
        TEST(z[2].start_note == 71);
        TEST(Synth::zone_entry_at(z, 69) == 1);
        TEST(Synth::zone_entry_at(z, 70) == 2);

        // A one-note zone is dropped when its only note moves to the previous zone,
        // and the later zones shift down.
        Synth::Zone* w = bank.channel_zones[2];
        w[0] = Synth::Zone{ 1, 0 };
        w[1] = Synth::Zone{ 51, 1 };
        w[2] = Synth::Zone{ 52, 2 };
        w[3] = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_previous(w, 1, 50));
        TEST(w[0].start_note == 1 && w[0].instrument == 0);
        TEST(w[1].start_note == 52 && w[1].instrument == 2);
        TEST(w[2].start_note == 0);
        TEST(Synth::zone_entry_at(w, 50) == 0);
        TEST(Synth::zone_entry_at(w, 51) == 1);

        // ... and symmetrically, moving a zone's first note into the next zone drops
        // the emptied zone; the next zone shifts down and absorbs the moved note.
        // Here note 0 leaves zone 0 ([0..49]), so zone 1 moves to slot 0 and starts
        // at 0.
        Synth::Zone* v = bank.channel_zones[3];
        v[0] = Synth::Zone{ 1, 0 };
        v[1] = Synth::Zone{ 51, 1 };
        v[2] = Synth::Zone{ 52, 2 };
        v[3] = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_next(v, 0, 0));
        TEST(v[0].start_note == 1 && v[0].instrument == 1);
        TEST(v[1].start_note == 52 && v[1].instrument == 2);
        TEST(v[2].start_note == 0);
        TEST(Synth::zone_entry_at(v, 0) == 0);
        TEST(Synth::zone_entry_at(v, 50) == 0);

        // Splitting at note 1 leaves a one-note first zone [0].
        Synth::Zone* n1 = bank.channel_zones[4];
        n1[0] = Synth::Zone{ 1, 0 };
        n1[1] = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_split_new(n1, 0, 1, &bank));
        TEST(n1[0].start_note == 1 && n1[1].start_note == 2);
        TEST(Synth::zone_entry_at(n1, 0) == 0);
        TEST(Synth::zone_entry_at(n1, 1) == 1);

        // A one-note-only first zone [0]: moving its note to the next zone drops it and
        // the next zone takes over the whole keyboard.
        Synth::Zone* s0 = bank.channel_zones[5];
        s0[0] = Synth::Zone{ 1, 0 };
        s0[1] = Synth::Zone{ 2, 1 };
        s0[2] = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_next(s0, 0, 0));
        TEST(s0[0].start_note == 1 && s0[0].instrument == 1);
        TEST(s0[1].start_note == 0);
        TEST(Synth::zone_entry_at(s0, 0) == 0);
        TEST(Synth::zone_entry_at(s0, 127) == 0);

        // A full 16-zone table cannot split in the middle; splitting at a zone's first
        // note needs no new entry and stays allowed.
        static Synth::InstrumentBank bank_full_table;
        Synth::init_default_bank(&bank_full_table);
        Synth::Zone* f = bank_full_table.channel_zones[0];
        for (uint32_t e = 0; e < Synth::max_instr_per_channel; e++) {
            f[e] = Synth::Zone{ static_cast<uint8_t>(e * 8 + 1),
                                static_cast<uint8_t>(bank_full_table.instruments.allocate()) };
        }
        TEST(bank_full_table.instruments.num_allocated == 17); // 1 default + 16 zone instruments
        TEST( ! Synth::zone_split_new(f, 0, 4, &bank_full_table));
        TEST(f[1].start_note == 9); // table untouched on refusal
        const uint32_t pool_before = bank_full_table.instruments.num_allocated;
        TEST(Synth::zone_split_new(f, 0, 0, &bank_full_table));
        TEST(bank_full_table.instruments.num_allocated == pool_before + 1);

        // A full instrument pool refuses the clone with the bank untouched.
        static Synth::InstrumentBank bank_full_pool;
        Synth::init_default_bank(&bank_full_pool);
        while (bank_full_pool.instruments.allocate() != pool_no_slot)
            ;
        TEST( ! Synth::zone_split_new(bank_full_pool.channel_zones[0], 0, 60, &bank_full_pool));
        TEST(bank_full_pool.channel_zones[0][0].start_note == 1);
        TEST(bank_full_pool.channel_zones[0][0].instrument == 0);

        // Repeated split/reclaim cycles keep the instrument pool bounded: each split
        // at the zone's first note clones the instrument and leaves the old one
        // unreferenced, so reclaim returns it to the pool.
        static Synth::InstrumentBank bank_cycles;
        Synth::init_default_bank(&bank_cycles);
        const uint32_t pool_start = bank_cycles.instruments.num_allocated;
        for (uint32_t cycle = 0; cycle < 50; cycle++) {
            TEST(Synth::zone_split_new(bank_cycles.channel_zones[0], 0, 0, &bank_cycles));
            Synth::reclaim_unused_slots(&bank_cycles);
            TEST(bank_cycles.instruments.num_allocated <= pool_start + 1);
        }
    }
    return exit_code;
}
