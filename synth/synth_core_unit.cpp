// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

// The keyboard tooltip note name is floor(midi / 12) - 2 with sharps only.
// MIDI 21..23 must render with the signed octave -1.  The 88-note keyboard
// spans 21..108.
static void check_note_name_formats_midi_notes()
{
    char name[8] = {};

    Sculptor::note_name(0, name, sizeof(name));
    TEST(strcmp(name, "C-2") == 0);

    Sculptor::note_name(21, name, sizeof(name));
    TEST(strcmp(name, "A-1") == 0);

    Sculptor::note_name(23, name, sizeof(name));
    TEST(strcmp(name, "B-1") == 0);

    Sculptor::note_name(24, name, sizeof(name));
    TEST(strcmp(name, "C0") == 0);

    Sculptor::note_name(51, name, sizeof(name));
    TEST(strcmp(name, "D#2") == 0);

    Sculptor::note_name(60, name, sizeof(name));
    TEST(strcmp(name, "C3") == 0);

    Sculptor::note_name(108, name, sizeof(name));
    TEST(strcmp(name, "C7") == 0);
}

// R1/R3 reference: the normalized LFO formula (eval_lfo_normalized in
// synth_parameters.cpp, without the pulse shaper).  The pulse_mix identity and
// mix-shape tests compare against it so the unshaped behavior stays pinned
// bit-exactly; the phase formula and the sine/saw switch replicate the
// production code one to one.
static float lfo_reference_normalized(const Synth::WaveType wave,
                                      const uint8_t         duty8,
                                      const uint32_t        tick,
                                      const uint32_t        period_ms)
{
    const float phase =
        static_cast<float>(tick) * static_cast<float>(256u * 1000u) / static_cast<float>(period_ms * 44100u);
    if (wave == Synth::WaveType::sine_wave) {
        return (vmath::sincos(phase * vmath::two_pi).sin + 1.0f) * 0.5f;
    }
    const float frac_phase = phase - static_cast<float>(static_cast<int>(phase));
    const float duty       = static_cast<float>(duty8) / 255.0f;
    if (frac_phase <= duty) {
        return (duty > 0.0f) ? frac_phase / duty : 0.0f;
    }
    const float fall = 1.0f - duty;
    return (fall > 0.0f) ? (1.0f - frac_phase) / fall : 0.0f;
}

static void check_core_inline_0()
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
    const Synth::LFODescriptor sine_lfo = { Synth::WaveType::sine_wave,
                                            0,
                                            1000,
                                            -1.0f,
                                            2.0f }; // 1s period, range [-1,1]
    float                      min_seen = 1e9f;
    float                      max_seen = -1e9f;
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
    TEST(approx(max_seen, 1.0f, 0.05f));
    // periodicity: with these params one period is exactly 4 ticks, so values
    // repeat every 4 ticks
    {
        const Synth::LFODescriptor periodic_lfo     = { Synth::WaveType::sine_wave, 0, 1024, -1.0f, 2.0f };
        const uint32_t             ticks_per_period = 4;
        TEST(approx(Synth::eval_lfo(periodic_lfo, 0, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, ticks_per_period, 256, 1000),
                    0.001f));
        TEST(approx(Synth::eval_lfo(periodic_lfo, 1, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, 1 + ticks_per_period, 256, 1000),
                    0.001f));
    }

    // sawtooth (triangle, duty=0x7F) LFO stays within [0,1] and reaches both ends
    {
        const Synth::LFODescriptor saw_lfo = { Synth::WaveType::sawtooth_wave, 0x7F, 1000, 0.0f, 1.0f };
        float                      saw_min = 1e9f;
        float                      saw_max = -1e9f;
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
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 4;
        env.sustain_first_point       = 2;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };      // start at min
        env.points[1]                 = { 2, 0xFFFF }; // attack peak
        env.points[2]                 = { 4, 0x8000 }; // decay to ~mid (sustain)
        env.points[3]                 = { 6, 0 };      // release to min

        // Sustained: run 20 ticks holding sustain.  Should reach ~1.0 peak,
        // then settle and HOLD at the sustain value (~0.5).
        Synth::EnvelopeState state       = { 0, 0 };
        float                first_value = Synth::eval_envelope(env, &state, true);
        TEST(approx(first_value, 0.0f, 0.01f));
        float peak_value = first_value;
        float last_value = first_value;
        for (uint32_t tick = 1; tick < 20; tick++) {
            last_value = Synth::eval_envelope(env, &state, true);
            if (last_value > peak_value) {
                peak_value = last_value;
            }
        }
        TEST(approx(peak_value, 1.0f, 0.02f));              // attack peak reached
        TEST(approx(last_value, 0x8000 / 65535.0f, 0.01f)); // held at sustain value

        // Release: stop sustaining, run more ticks; value must reach ~0.
        float release_value = last_value;
        for (uint32_t tick = 0; tick < 10; tick++) {
            release_value = Synth::eval_envelope(env, &state, false);
        }
        TEST(approx(release_value, 0.0f, 0.01f));

        // eval_envelope_at peeks at a state without advancing it: same value the advancing
        // eval returns at that state, and the state is left untouched.
        Synth::EnvelopeState peek_state = { 1, 0 }; // mid-attack between points 0 and 1
        const float          peeked     = Synth::eval_envelope_at(env, peek_state);
        TEST(peek_state.tick == 1 && peek_state.point == 0);
        TEST(approx(peeked, 0.5f, 0.01f));
        const float advanced = Synth::eval_envelope(env, &peek_state, true);
        TEST(approx(advanced, peeked, 0.0f));
        TEST(peek_state.tick == 2 && peek_state.point == 1);
    }

    // Piano-type sustain envelope semantics.  A single-point span parks
    // forever; a multi-point span loops back to the span start with
    // ping-pong off; ping-pong reverses direction at both span ends; the
    // decay helpers cross the cutoff exactly at T; and the termination
    // predicate gates on the decay and the dest level.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 4;
        env.sustain_first_point       = 1;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };      // start at min
        env.points[1]                 = { 2, 0x4000 }; // span start: quarter level
        env.points[2]                 = { 4, 0x8000 }; // span end: half level
        env.points[3]                 = { 6, 0 };      // release to min

        // Walks a sustained envelope until its state parks on the given span
        // point, so the recorded sequences below are independent of the
        // approach phase.
        auto walk_to_span_point = [&env](Synth::EnvelopeState* state, uint32_t point) {
            for (;;) {
                Synth::eval_envelope(env, state, true);
                if (state->point == point && state->tick == env.points[point].position) {
                    return;
                }
            }
        };

        // A single-point span at a non-final position holds its value
        // indefinitely while sustained.
        {
            Synth::EnvelopeDescriptor single = env;
            single.sustain_first_point       = 1;
            single.sustain_last_point        = 1;
            Synth::EnvelopeState state       = { 0, 0 };
            walk_to_span_point(&state, 1);
            float value = Synth::eval_envelope(single, &state, true);
            TEST(approx(value, 0x4000 / 65535.0f, 0.01f));
            TEST(state.point == 1 && state.tick == single.points[1].position);
            for (uint32_t tick = 0; tick < 100; tick++) {
                value = Synth::eval_envelope(single, &state, true);
            }
            TEST(approx(value, 0x4000 / 65535.0f, 0.01f));
            TEST(state.point == 1 && state.tick == single.points[1].position);
        }

        // With ping-pong off, the multi-point span loops back to the
        // span start, and the sampled value sequence is periodic.
        {
            Synth::EnvelopeState state = { 0, 0 };
            walk_to_span_point(&state, 2);
            uint32_t ticks[6];
            float    values[6];
            for (uint32_t i = 0; i < 6; i++) {
                ticks[i]  = state.tick;
                values[i] = Synth::eval_envelope(env, &state, true);
            }
            // The loop-back jump advances one tick in the same eval, so the
            // sampled span-end and interior ticks alternate and the value
            // sequence is periodic with period 2.
            TEST(ticks[0] == 4 && ticks[1] == 3 && ticks[2] == 4);
            TEST(ticks[3] == 3 && ticks[4] == 4 && ticks[5] == 3);
            TEST(values[0] == values[2] && values[1] == values[3]);
            TEST(values[2] == values[4] && values[3] == values[5]);
        }

        // With ping-pong on, the span plays forward then backward,
        // reversing continuously at both span ends, indefinitely.
        {
            Synth::EnvelopeDescriptor bounce = env;
            bounce.flags                     = Synth::envelope_ping_pong;
            Synth::EnvelopeState state       = { 0, 0 };
            walk_to_span_point(&state, 2);
            uint32_t ticks[8];
            float    values[8];
            for (uint32_t i = 0; i < 8; i++) {
                ticks[i]  = state.tick;
                values[i] = Synth::eval_envelope(bounce, &state, true);
            }
            TEST(ticks[0] == 4 && ticks[1] == 3 && ticks[2] == 2 && ticks[3] == 3);
            TEST(ticks[4] == 4 && ticks[5] == 3 && ticks[6] == 2 && ticks[7] == 3);
            TEST(values[0] == values[4] && values[1] == values[5]);
            TEST(values[2] == values[6] && values[3] == values[7]);
            // A single-point span ignores the ping-pong flag and parks.
            Synth::EnvelopeDescriptor single  = env;
            single.flags                      = Synth::envelope_ping_pong;
            single.sustain_first_point        = 1;
            single.sustain_last_point         = 1;
            Synth::EnvelopeState single_state = { 0, 0 };
            walk_to_span_point(&single_state, 1);
            float single_value = Synth::eval_envelope(single, &single_state, true);
            for (uint32_t tick = 0; tick < 50; tick++) {
                single_value = Synth::eval_envelope(single, &single_state, true);
            }
            TEST(approx(single_value, 0x4000 / 65535.0f, 0.01f));
            TEST(single_state.point == 1 && single_state.tick == single.points[1].position);
        }

        // The per-tick multiplier lands the factor on the cutoff after
        // exactly T worth of control steps, regardless of the release length.
        {
            TEST(Synth::sustain_decay_cutoff == 1.0f / 1024.0f);
            TEST(Synth::sustain_decay_per_tick(0) == 1.0f); // no decay
            TEST(Synth::sustain_decay_ticks(0.0f) == 0);    // legacy gate
            TEST(Synth::sustain_decay_ticks(-1.0f) == 0);   // never decays
            TEST(Synth::sustain_decay_ticks(1.0e-9f) == 1); // any positive release decays at least one step
            // One control step is 256/44100 s, so 1 s rounds to 172 steps.
            const uint32_t ticks_1s = Synth::sustain_decay_ticks(1.0f);
            TEST(ticks_1s == 172);
            const uint32_t ticks_15s = Synth::sustain_decay_ticks(15.0f);
            TEST(ticks_15s == 2584);
            const float mult_1s  = Synth::sustain_decay_per_tick(ticks_1s);
            const float mult_15s = Synth::sustain_decay_per_tick(ticks_15s);
            TEST(mult_1s < mult_15s && mult_15s < 1.0f); // longer release: slower decay
            float factor_1s = 1.0f;
            for (uint32_t i = 0; i < ticks_1s; i++) {
                factor_1s *= mult_1s;
            }
            TEST(approx(factor_1s, 1.0f / 1024.0f, 0.0002f));
            float factor_15s = 1.0f;
            for (uint32_t i = 0; i < ticks_15s; i++) {
                factor_15s *= mult_15s;
            }
            TEST(approx(factor_15s, 1.0f / 1024.0f, 0.0002f));
            // An out-of-range release saturates instead of trapping on the
            // float-to-unsigned conversion.
            TEST(Synth::sustain_decay_ticks(1.0e30f) == UINT32_MAX);
        }

        // The termination gate predicate.  A decaying oscillator whose
        // dest level is still audible owns its termination (the completion
        // fade would cut a still-decaying note); a silent dest falls back to
        // the existing completion rule, and a non-decaying oscillator never
        // skips it.
        {
            TEST(Synth::sustain_decay_owns_termination(true, 0.5f));
            TEST(! Synth::sustain_decay_owns_termination(true, 0.0f));
            TEST(! Synth::sustain_decay_owns_termination(false, 0.5f));
            TEST(! Synth::sustain_decay_owns_termination(false, 0.0f));
        }

        // Instrument release without a volume envelope: decay-decision
        // tests over a mirror of the engine's decision policy.
        // realtime_synth.cpp is not linked into this unit, so these tests
        // exercise only the modeled decision policy. These are NOT proof of
        // the runtime fade or free behavior itself: the real fade block and
        // voice freeing run in realtime_synth.cpp and are covered by the
        // manual audio smoke.  The held-cutoff free path is also
        // un-mirrored: volume_termination_due is release-only, so a held
        // voice reaching the cutoff frees through the cutoff disjunct in
        // the free condition, which this release-only model does not exercise.
        {
            struct MirrorOsc {
                bool     decaying;
                uint32_t decay_tick;
                float    decay_factor;
                float    decay_mult;
            };

            const uint32_t total_ticks = Synth::sustain_decay_ticks(1.0f); // 172 steps to the cutoff
            const float    mult        = Synth::sustain_decay_per_tick(total_ticks);

            // One control step of the engine's no-envelope volume path: the
            // decay starts on the first held step and advances every step
            // once started, held or released.
            auto engine_step = [](MirrorOsc* osc, bool held, uint32_t release_ticks) {
                if (release_ticks && ! osc->decaying) {
                    if (held) {
                        osc->decaying   = true;
                        osc->decay_tick = 0;
                    }
                }
                else if (release_ticks && osc->decaying) {
                    ++osc->decay_tick;
                    osc->decay_factor *= osc->decay_mult;
                }
            };
            // Mirrors volume_termination_due's gate order: the decay gate
            // precedes the no-envelope early return, so a mid-decay note-off
            // keeps decaying while the dest level is audible.
            auto termination_due = [](const MirrorOsc& osc, float dest_value, uint32_t release_ticks) {
                if (release_ticks && osc.decaying) {
                    if (osc.decay_tick >= release_ticks) {
                        return true;
                    }
                    if (Synth::sustain_decay_owns_termination(osc.decaying, dest_value)) {
                        return false;
                    }
                }
                return true; // no envelope: the early return owns the rest
            };

            // (a) Hold a note with no envelope and release > 0: the volume
            // decays while held and the voice reaches the cutoff at ~T,
            // freeing without a key event.
            {
                MirrorOsc osc             = { false, 0, 1.0f, mult };
                float     previous_volume = 0.8f;
                for (uint32_t step = 0; step <= total_ticks; ++step) {
                    engine_step(&osc, true, total_ticks);
                    const float volume = 0.8f * osc.decay_factor;
                    if (osc.decaying && step > 0) {
                        TEST(volume < previous_volume); // decays while held
                    }
                    previous_volume = volume;
                }
                TEST(osc.decaying && osc.decay_tick == total_ticks);
                TEST(termination_due(osc, 0.8f, total_ticks)); // frees at the cutoff
            }

            // (b) Note-off mid-decay: the decay runs through to the cutoff
            // with no jump, then the real fade block takes over.
            {
                MirrorOsc osc = { false, 0, 1.0f, mult };
                for (uint32_t step = 0; step < 50; ++step) {
                    engine_step(&osc, true, total_ticks);
                }
                TEST(osc.decaying && osc.decay_tick == 49);
                float previous_volume = 0.8f * osc.decay_factor;
                bool  smooth          = true;
                for (uint32_t step = 50; step < total_ticks; ++step) {
                    engine_step(&osc, false, total_ticks); // released: decay continues
                    const float volume = 0.8f * osc.decay_factor;
                    if (volume >= previous_volume) { // still decaying smoothly, no jump
                        smooth = false;
                    }
                    previous_volume = volume;
                }
                TEST(smooth);
                TEST(osc.decay_tick == total_ticks - 1);
                TEST(! termination_due(osc, 0.8f, total_ticks)); // owns termination mid-decay
                engine_step(&osc, false, total_ticks);
                TEST(osc.decay_tick == total_ticks);
                TEST(termination_due(osc, 0.8f, total_ticks));
            }

            // (c) Release 0: legacy immediate fade on note-off; no decay ever
            // starts.
            {
                MirrorOsc osc = { false, 0, 1.0f, mult };
                for (uint32_t step = 0; step < 10; ++step) {
                    engine_step(&osc, true, 0);
                }
                TEST(! osc.decaying && osc.decay_tick == 0);
                TEST(termination_due(osc, 0.8f, 0)); // fade on the first released block
            }

            // (d) Dest 0: the decay gate falls through and the voice frees
            // on release like the legacy rule.
            {
                MirrorOsc osc = { false, 0, 1.0f, mult };
                for (uint32_t step = 0; step < 10; ++step) {
                    engine_step(&osc, true, total_ticks);
                }
                TEST(osc.decaying && osc.decay_tick == 9);
                TEST(termination_due(osc, 0.0f, total_ticks));
            }
        }
    }

    // pitch bend -> semitones: centered 14-bit value scaled by the bend range.
    // center (0) -> no bend; full deflection -> +/- range; half -> ~half range.
    TEST(approx(Synth::pitch_bend_to_semitones(0, 2.0f), 0.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 2.0f), 2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(-8192, 2.0f), -2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(4096, 2.0f), 1.0f, 0.001f));
    // range scales linearly
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 12.0f), 12.0f, 0.01f));

    TEST(Synth::get_effect_param_floats(Synth::EffectType::distortion) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::delay) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::chorus) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::reverb) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::compressor) == 5);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::fir) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::ring_mod) == 2);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::distortion) == 2);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::delay) == 88201);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::chorus) == 4412);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::reverb) == 25191);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::compressor) == 1);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::fir) == 3074);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::ring_mod) == 1);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::none) == 0);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::none) == 0);

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

        TEST(Synth::get_ringbuf_contig_tail(0, capacity) == capacity);                // offset 0: whole buffer
        TEST(Synth::get_ringbuf_contig_tail(1000, capacity) == 24);                   // mid-buffer: 1024 - 1000
        TEST(Synth::get_ringbuf_contig_tail(2048, capacity) == capacity);             // exact multiple wraps to 0
        TEST(Synth::get_ringbuf_contig_tail(capacity + 1, capacity) == capacity - 1); // offset 1 after a wrap
    }

    // propagate_parameters: one-step-delay vs zero-lag external input.  Chain X(external) -> B -> A.
    // An external source is read at its current value (zero lag); a plain->plain hop lags exactly
    // one step.  Parameter 0 is the reserved sentinel.
    {
        Synth::ParamDescriptor descs[4] = {};
        // descs[0] (sentinel) and descs[3] (X, externally driven) stay kind external.
        descs[2].kind              = Synth::ParamKind::plain; // B reads X additively
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0]  = { 3, 1.0f, Synth::SourceOp::add };
        descs[1].kind              = Synth::ParamKind::plain; // A reads B additively
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::add };

        Synth::Parameter params[4] = {};
        params[3].value            = 5.0f; // drive the external input

        Synth::propagate_parameters(params, descs, 4); // step 1
        TEST(approx(params[2].value, 5.0f, 0.001f));   // B picked up the input with zero lag
        TEST(approx(params[1].value, 0.0f, 0.001f));   // A still sees B's previous (0): one-step delay

        Synth::propagate_parameters(params, descs, 4); // step 2
        TEST(approx(params[2].value, 5.0f, 0.001f));
        TEST(approx(params[1].value, 5.0f, 0.001f)); // A now sees B, one step later
    }

    // propagate_parameters: a feedback cycle (A <-> B, |amount| < 1) stays finite and
    // bounded; the one-step delay makes it a well-defined iteration, never a deadlock or NaN.
    {
        Synth::ParamDescriptor descs[3] = {};
        // descs[0] (sentinel) stays kind external.
        descs[1].kind              = Synth::ParamKind::plain; // A = 1 + 0.5 * B.prev
        descs[1].plain.base_value  = 1.0f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 0.5f, Synth::SourceOp::add };
        descs[2].kind              = Synth::ParamKind::plain; // B = 1 + 0.5 * A.prev
        descs[2].plain.base_value  = 1.0f;
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0]  = { 1, 0.5f, Synth::SourceOp::add };

        Synth::Parameter params[3] = {};
        for (uint32_t step = 0; step < 1000; step++) {
            Synth::propagate_parameters(params, descs, 3);
        }
        TEST(params[1].value == params[1].value); // not NaN
        TEST(params[2].value == params[2].value);
        TEST(params[1].value < 100.0f && params[1].value > -100.0f); // bounded (converges to 2)
        TEST(approx(params[1].value, 2.0f, 0.01f));
    }

    // propagate_parameters: envelope-kind params are exempt from the prev_value
    // snapshot.  The envelope step writes the pair (value at the step's starting
    // tick, prev_value one tick ahead) itself, so the destination reads the
    // end-of-step value without the snapshot clobbering it.
    {
        Synth::ParamDescriptor descs[3] = {};
        descs[1].kind                   = Synth::ParamKind::plain; // dest = 1.0 + env.prev
        descs[1].plain.base_value       = 1.0f;
        descs[1].plain.num_sources      = 1;
        descs[1].plain.sources[0]       = { 2, 1.0f, Synth::SourceOp::add };
        descs[2].kind                   = Synth::ParamKind::envelope;

        Synth::Parameter params[3] = {};
        params[2].value            = 1.0f; // envelope value at the step's starting tick
        params[2].prev_value       = 2.0f; // envelope value one tick ahead (peek)

        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[1].value, 3.0f, 0.001f));      // dest folded the end-of-step value
        TEST(approx(params[2].prev_value, 2.0f, 0.001f)); // the pair survived the snapshot
    }

    // propagate_parameters: a multiply source scales the base (e.g. velocity into volume).
    {
        Synth::ParamDescriptor descs[3] = {};
        // descs[0] (sentinel) and descs[2] (velocity input) stay kind external.
        descs[1].kind              = Synth::ParamKind::plain; // volume base, scaled by velocity
        descs[1].plain.base_value  = 0.8f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::multiply };

        Synth::Parameter params[3] = {};
        params[2].value            = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[1].value, 0.4f, 0.001f)); // 0.8 * 0.5 (input read with zero lag)
    }

    // propagate_parameters: sources fold left-to-right over the running accumulator, so a
    // multiply applies to base plus prior adds, not to base alone.  This is the production
    // volume path: (base + envelope) * tremolo * velocity.
    {
        Synth::ParamDescriptor descs[5] = {};
        // descs[0] sentinel, descs[2] envelope, descs[3] tremolo, descs[4] velocity stay kind
        // external; their values are set directly below.
        descs[1].kind              = Synth::ParamKind::plain;
        descs[1].plain.num_sources = 3;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::add };
        descs[1].plain.sources[1]  = { 3, 1.0f, Synth::SourceOp::multiply };
        descs[1].plain.sources[2]  = { 4, 1.0f, Synth::SourceOp::multiply };

        Synth::Parameter params[5] = {};
        params[2].value            = 2.0f; // envelope 2.0
        params[3].value            = 0.5f; // tremolo gain 0.5
        params[4].value            = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 5);
        TEST(approx(params[1].value, 0.5f, 0.001f)); // (0 + 2.0) * 0.5 * 0.5, not 0 + 2.0 + ...
    }

    // configure_plain: effect-param shape -- base folded with an LFO leaf (add) and a
    // channel input leaf (multiply).  This is the contract the effect-param expander relies on:
    // a modulated effect param's value is propagate_parameters' result for the assembled dest.
    // The LFO leaf value is set directly here (the host pre-pass that fills it is not under test).
    {
        constexpr uint16_t dest_id  = 1;
        constexpr uint16_t lfo_id   = 2;
        constexpr uint16_t input_id = 3;

        Synth::ParamDescriptor descs[4] = {};
        // descs[0] (sentinel) and descs[input_id] (channel input, e.g. mod wheel) stay kind external.

        Synth::LFODescriptor test_lfo = {};
        test_lfo.wave                 = Synth::WaveType::sine_wave;
        test_lfo.period_ms            = 50;
        Synth::configure_lfo(&descs[lfo_id], test_lfo, Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        const Synth::SourceParam inputs[1] = { { input_id, 1.0f, Synth::SourceOp::multiply } };
        Synth::configure_plain(&descs[dest_id], 0.1f, 0, 1.0f, lfo_id, Synth::SourceOp::add, inputs, 1);

        // configure_lfo made the LFO node; configure_plain made the dest a plain node.
        TEST(descs[lfo_id].kind == Synth::ParamKind::lfo);
        TEST(descs[lfo_id].lfo.lfo.wave == Synth::WaveType::sine_wave);
        TEST(approx(descs[lfo_id].lfo.lfo.period_ms, 50.0f, 0.001f)); // captured copy
        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2);

        Synth::Parameter params[4] = {};
        params[lfo_id].value       = 0.2f; // pretend the LFO produced 0.2
        params[input_id].value     = 0.5f; // channel input 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 0.15f, 0.001f)); // (0.1 + 0.2) * 0.5
    }

    // configure_plain: no LFO, an envelope source plus a multiply input -- the voice volume
    // shape (base + envelope) * velocity.  lfo_desc_id 0 means no LFO leaf and no LFO source.
    {
        constexpr uint16_t dest_id = 1;
        constexpr uint16_t env_id  = 2;
        constexpr uint16_t vel_id  = 3;

        Synth::ParamDescriptor descs[4] = {};
        // descs[0] sentinel, descs[env_id] envelope value, descs[vel_id] velocity: kind external,
        // their values set directly below.

        const Synth::SourceParam inputs[1] = { { vel_id, 1.0f, Synth::SourceOp::multiply } };
        Synth::configure_plain(&descs[dest_id], 0.0f, env_id, 1.0f, 0, Synth::SourceOp::add, inputs, 1);

        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2);         // envelope source + velocity source, no LFO source
        TEST(descs[dest_id].plain.sources[0].scale == 1.0f); // the legacy unscaled envelope
        // A non-1 scale rides on the envelope source itself (the per-binding
        // envelope depth); the other sources keep their own scales.
        Synth::configure_plain(&descs[dest_id], 0.0f, env_id, 0.5f, 0, Synth::SourceOp::add, inputs, 1);
        TEST(descs[dest_id].plain.sources[0].scale == 0.5f);
        TEST(descs[dest_id].plain.sources[0].op == Synth::SourceOp::add);
        TEST(descs[dest_id].plain.sources[1].scale == 1.0f);
        // Restore the legacy unscaled descriptor the propagation below asserts on.
        Synth::configure_plain(&descs[dest_id], 0.0f, env_id, 1.0f, 0, Synth::SourceOp::add, inputs, 1);

        Synth::Parameter params[4] = {};
        params[env_id].value       = 2.0f; // envelope 2.0
        params[vel_id].value       = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 1.0f, 0.001f)); // (0 + 2.0) * 0.5
    }

    // eval_lfo_mod: add op swings bipolar within [-depth, depth] and reaches both ends;
    // depth 0 is the neutral contribution 0.  4 ticks = one period here.
    {
        const Synth::LFODescriptor lfo     = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                depth   = 0.5f;
        float                      add_min = 1e9f;
        float                      add_max = -1e9f;
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
        TEST(approx(add_max, depth, 0.02f));
        // depth 0 -> neutral 0 at every tick
        TEST(approx(Synth::eval_lfo_mod(lfo, 7, 256, 1024, 1000, 0.0f, Synth::SourceOp::add), 0.0f, 0.001f));
    }

    // eval_lfo_mod: multiply op is an attenuation factor within [1-depth, 1];
    // depth 0 is the neutral factor 1.
    {
        const Synth::LFODescriptor lfo     = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                depth   = 0.5f;
        float                      mul_min = 1e9f;
        float                      mul_max = -1e9f;
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
        const Synth::LFODescriptor lfo  = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                slow = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1024, 0.5f, Synth::SourceOp::add);
        const float                fast = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 512, 0.5f, Synth::SourceOp::add);
        TEST(! approx(slow, fast, 0.05f));
        // period_ms 0 falls back to the descriptor's own period_ms.
        const float defaulted     = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 0, 0.5f, Synth::SourceOp::add);
        const float explicit_same = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1000, 0.5f, Synth::SourceOp::add);
        TEST(approx(defaulted, explicit_same, 0.001f));
    }

    // Lfo_beat_div_period_ms resolves a beat division to a period in ms at
    // the runtime tempo.  Div 0 returns 0 (the descriptor's absolute period
    // applies); divs 1..5 are 1, 1/2, 1/4, 1/8, 1/16 note, which at the 120 BPM
    // default resolve to 2000, 1000, 500, 250, 125 ms (exact).
    {
        TEST(Synth::lfo_beat_div_period_ms(0) == 0.0f);
        TEST(Synth::lfo_beat_div_period_ms(1) == 2000.0f);
        TEST(Synth::lfo_beat_div_period_ms(2) == 1000.0f);
        TEST(Synth::lfo_beat_div_period_ms(3) == 500.0f);
        TEST(Synth::lfo_beat_div_period_ms(4) == 250.0f);
        TEST(Synth::lfo_beat_div_period_ms(5) == 125.0f);
    }

    // The asset-less default is 120 BPM with the historical tick rate 114
    // (44100 * 60 / (120 * 192), integer division).  R8/R9: the setter
    // recomputes the tick rate, the beat_div periods scale with the tempo, and
    // one quarter (192 ticks) spans 21888 samples at 120 and 29376 at 90, so
    // deltas scheduled after a tempo change scale with the new rate.  The test
    // resets the tempo to 120 at the end (state hygiene for later tests).
    {
        TEST(Synth::get_runtime_tempo_bpm() == 120);
        TEST(Synth::samples_per_midi_tick == 114);
        TEST(Synth::samples_per_midi_tick * 192u == 21888u);

        Synth::set_runtime_tempo_bpm(90);
        TEST(Synth::get_runtime_tempo_bpm() == 90);
        TEST(Synth::samples_per_midi_tick == 153);
        TEST(Synth::samples_per_midi_tick * 192u == 29376u);
        // Div 3 is a quarter note: 60000 / 90 = 666.67 ms; div 1 is 4 beats.
        TEST(approx(Synth::lfo_beat_div_period_ms(3), 666.667f, 1.0f));
        TEST(approx(Synth::lfo_beat_div_period_ms(1), 2666.667f, 1.0f));

        // A beat_div 3 LFO with no rate source evaluates with the resolved
        // period (rounded to the nearest ms), matching eval_lfo_mod driven
        // with that period.  A quarter period in, the sine sits at its peak.
        Synth::LFODescriptor synced_lfo             = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        synced_lfo.beat_div                         = 3;
        Synth::ParamDescriptor::LFOParam synced_gen = {};
        synced_gen.lfo                              = synced_lfo;
        synced_gen.op                               = Synth::SourceOp::add;
        synced_gen.depth                            = 0.5f;
        const uint32_t resolved_ms                  = static_cast<uint32_t>(Synth::lfo_beat_div_period_ms(3) + 0.5f);
        for (uint32_t tick = 0; tick < 16; tick++) {
            TEST(Synth::eval_sourced_lfo(synced_gen, 0.0f, 0.0f, tick) == Synth::eval_lfo_mod(synced_lfo,
                                                                                              tick,
                                                                                              Synth::rt_step_samples,
                                                                                              Synth::rt_sampling_rate,
                                                                                              resolved_ms,
                                                                                              0.5f,
                                                                                              Synth::SourceOp::add));
        }
        const float quarter_in = Synth::eval_sourced_lfo(synced_gen, 0.0f, 0.0f, 29);
        // The sine sits at its peak; the add-modulated peak value with depth
        // 0.5 is exactly the depth (the wave contribution 2w - 1 reaches 1).
        TEST(approx(quarter_in, 0.5f, 0.01f));

        Synth::set_runtime_tempo_bpm(120);
        TEST(Synth::get_runtime_tempo_bpm() == 120);
        TEST(Synth::samples_per_midi_tick == 114);
        TEST(Synth::samples_per_midi_tick * 192u == 21888u);
    }

    // R8/T1: a tempo change does not reset the LFO phase.  The cycle position
    // is recomputed from the absolute tick, so after the change the wave shows
    // the value at the same absolute tick under the new period; it does not
    // restart at phase 0.  The test resets the tempo to 120 at the end.
    {
        Synth::ParamDescriptor::LFOParam gen = {};
        gen.lfo                              = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        gen.lfo.beat_div                     = 3; // 500 ms at the 120 BPM default
        gen.op                               = Synth::SourceOp::add;
        gen.depth                            = 1.0f;
        const uint32_t tick                  = 200;
        const float    before                = Synth::eval_sourced_lfo(gen, 0.0f, 0.0f, tick);
        Synth::set_runtime_tempo_bpm(90);
        const float    after       = Synth::eval_sourced_lfo(gen, 0.0f, 0.0f, tick + 1);
        const uint32_t resolved_ms = static_cast<uint32_t>(Synth::lfo_beat_div_period_ms(3) + 0.5f);
        TEST(after == Synth::eval_lfo_mod(gen.lfo,
                                          tick + 1,
                                          Synth::rt_step_samples,
                                          Synth::rt_sampling_rate,
                                          resolved_ms,
                                          1.0f,
                                          Synth::SourceOp::add));
        // Tick 201 sits deep in the cycle under the new period; a phase reset
        // would read the mid value 0.5 instead.
        TEST(! approx(after, 0.5f, 0.1f));
        TEST(! approx(before, after, 0.05f));
        Synth::set_runtime_tempo_bpm(120);
        TEST(Synth::get_runtime_tempo_bpm() == 120);
        TEST(Synth::samples_per_midi_tick == 114);
    }

    // R9/R11: a mid-song FF 51 tempo event is captured.  With emit_ticks true
    // the parser emits it between the voice events at its tick (channel 0,
    // bpm 90); with emit_ticks false it is consumed for the conversion and not
    // emitted, and the later deltas scale with the new tempo.  The soundtrack
    // codec round-trips the tempo event (code, tick, value) exactly.
    {
        static const uint8_t midi[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x60, // division 96
            0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x13,                                     // track length 19
            0x00, 0x90, 0x3C, 0x64,                   // delta 0, note_on ch0 note 60 vel 100
            0x60, 0xFF, 0x51, 0x03, 0x0A, 0x2C, 0x2B, // delta 96, tempo 666667 us/quarter (90 BPM)
            0x60, 0x80, 0x3C, 0x40,                   // delta 96, note_off ch0 note 60 vel 64
            0x00, 0xFF, 0x2F, 0x00,                   // end of track
        };
        Synth::MidiEvent events[8];
        const uint32_t   count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100, true);
        TEST(count == 3);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::tempo);
        TEST(events[1].channel == 0);
        TEST(events[1].time == 96); // absolute tick
        TEST(events[1].tempo_bpm == 90);
        TEST(events[2].event == Synth::EvType::note_off);
        TEST(events[2].time == 192);

        // Sample mode: no tempo event, but the conversion consumes it, so the
        // note_off lands at 96 ticks at 120 BPM plus 96 at 90 BPM (div 96).
        Synth::MidiEvent sampled[8];
        TEST(Synth::parse_midi_file(midi, sizeof(midi), sampled, 8, 44100) == 2);
        TEST(sampled[0].event == Synth::EvType::note_on && sampled[0].time == 0);
        TEST(sampled[1].event == Synth::EvType::note_off && sampled[1].time == 51450);

        // Codec round-trip: encode stores the tempo event like any stored
        // event; decode emits it with the same code, tick and value.
        Synth::MidiEvent stored[3] = {};
        stored[0].time             = 0;
        stored[0].event            = Synth::EvType::note_on;
        stored[0].channel          = 0;
        stored[0].note             = 60;
        stored[0].note_data        = 100;
        stored[1].time             = 96;
        stored[1].event            = Synth::EvType::tempo;
        stored[1].channel          = 0;
        stored[1].tempo_bpm        = 90;
        stored[2].time             = 192;
        stored[2].event            = Synth::EvType::note_off;
        stored[2].channel          = 0;
        stored[2].note             = 60;
        stored[2].note_data        = 64;
        uint8_t           dest[64];
        Synth::Soundtrack soundtrack = {};
        TEST(Synth::encode_soundtrack(stored, 3, dest, sizeof(dest), &soundtrack) > 0);
        Synth::MidiEvent decoded[8]    = {};
        const uint32_t   decoded_count = Synth::decode_soundtrack(soundtrack, decoded, 8);
        TEST(decoded_count == 3);
        uint32_t tempo_index = decoded_count;
        for (uint32_t i = 0; i < decoded_count; i++) {
            if (decoded[i].event == Synth::EvType::tempo) {
                tempo_index = i;
            }
        }
        TEST(tempo_index < decoded_count);
        TEST(decoded[tempo_index].time == 96);
        TEST(decoded[tempo_index].channel == 0);
        TEST(decoded[tempo_index].tempo_bpm == 90);
        for (uint32_t i = 0; i < decoded_count; i++) {
            if (i == tempo_index) {
                continue;
            }
            if (decoded[i].event == Synth::EvType::note_on) {
                TEST(decoded[i].time == 0 && decoded[i].note == 60);
            }
            else {
                TEST(decoded[i].event == Synth::EvType::note_off && decoded[i].time == 192 && decoded[i].note == 60);
            }
        }

        // A tempo-only stream still encodes (the event counts toward
        // used_channels) and decodes to the single tempo event.
        Synth::MidiEvent tempo_only[1] = {};
        tempo_only[0].time             = 5;
        tempo_only[0].event            = Synth::EvType::tempo;
        tempo_only[0].channel          = 0;
        tempo_only[0].tempo_bpm        = 120;
        Synth::Soundtrack tempo_track  = {};
        TEST(Synth::encode_soundtrack(tempo_only, 1, dest, sizeof(dest), &tempo_track) > 0);
        Synth::MidiEvent tempo_decoded[4] = {};
        TEST(Synth::decode_soundtrack(tempo_track, tempo_decoded, 4) == 1);
        TEST(tempo_decoded[0].event == Synth::EvType::tempo);
        TEST(tempo_decoded[0].time == 5 && tempo_decoded[0].tempo_bpm == 120);
    }

    // Pulse_mix 0 is a bit-exact identity.  eval_lfo (min 0, delta 1) must
    // equal the normalized formula reference with exact equality for sine over
    // a full period and for sawtooth duties 0, 0x40, 0x7F, 0x80, 0xFF over
    // sampled phases.  The saw descriptors pin start_point 1 (lowest,
    // offset 0): the default middle start offsets the saw by duty/2, which
    // would break the identity for every duty above 0.  The sine needs no
    // pinning: its middle offset is 0.
    {
        Synth::LFODescriptor identity_sine_lfo = { Synth::WaveType::sine_wave, 0, 997, 0.0f, 1.0f };
        identity_sine_lfo.pulse_mix            = 0;
        for (uint32_t tick = 0; tick < 996; tick++) {
            TEST(Synth::eval_lfo(identity_sine_lfo, tick, 256, 44100) ==
                 lfo_reference_normalized(Synth::WaveType::sine_wave, 0, tick, 997));
        }
        const uint8_t duties[5] = { 0, 0x40, 0x7F, 0x80, 0xFF };
        for (uint32_t duty_index = 0; duty_index < 5; duty_index++) {
            Synth::LFODescriptor saw_lfo = { Synth::WaveType::sawtooth_wave, duties[duty_index], 997, 0.0f, 1.0f };
            saw_lfo.pulse_mix            = 0;
            saw_lfo.start_point          = 1; // lowest: offset 0
            for (uint32_t tick = 0; tick < 996; tick++) {
                TEST(Synth::eval_lfo(saw_lfo, tick, 256, 44100) ==
                     lfo_reference_normalized(Synth::WaveType::sawtooth_wave, duties[duty_index], tick, 997));
            }
        }
    }

    // Pulse_mix 255 is the step.  Saw duty 0x80 (128/255, slightly above
    // 0.5), start_point 1 (lowest, offset 0), step 256 / rate 1024 / period
    // 1000: one tick is a quarter phase.  Phases 0, 1/4, 1/2, 3/4 give outputs
    // 0, 0, 1, 1; the exact w = 0.5 crossings sit slightly above the sampled
    // phases (at 64/255 on the rise, 383/510 on the fall), so the 1/4 sample
    // reads just below mid and the 3/4 sample reads just above mid while still
    // descending.  A sub-crossing phase (1/8 -> w 0.25) reads 0.  A sine sweep
    // over full periods produces only 0 or 1.
    {
        Synth::LFODescriptor saw_lfo = { Synth::WaveType::sawtooth_wave, 0x80, 1000, 0.0f, 1.0f };
        saw_lfo.pulse_mix            = 255;
        saw_lfo.start_point          = 1;
        TEST(Synth::eval_lfo(saw_lfo, 0, 256, 1024) == 0.0f);
        TEST(Synth::eval_lfo(saw_lfo, 1, 256, 1024) == 0.0f);
        TEST(Synth::eval_lfo(saw_lfo, 2, 256, 1024) == 1.0f);
        TEST(Synth::eval_lfo(saw_lfo, 3, 256, 1024) == 1.0f);
        // Phase 1/8: w = (1/8) / (128/255) = 0.249..., below the crossing.
        Synth::LFODescriptor slow_saw = saw_lfo;
        slow_saw.period_ms            = 2000;
        TEST(Synth::eval_lfo(slow_saw, 1, 256, 1024) == 0.0f);
        // Inclusive-rule pin: sine at phase 0 has w = (sin(0) + 1) * 0.5 = 0.5
        // exactly, and the step reads the max there (w >= 0.5 rule).  Sine
        // phase 1/2 is deliberately not a second pin: sincos(pi) is not exactly
        // zero in floating point, so w there is 0.5 only up to rounding and
        // reads min.
        Synth::LFODescriptor step_sine_lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        step_sine_lfo.pulse_mix            = 255;
        TEST(Synth::eval_lfo(step_sine_lfo, 0, 256, 1024) == 1.0f);
        for (uint32_t tick = 0; tick < 512; tick++) {
            const float value = Synth::eval_lfo(step_sine_lfo, tick, 256, 44100);
            TEST(value == 0.0f || value == 1.0f);
        }
    }

    // Intermediate mixes stay within [0, 1]; the output is a non-decreasing
    // function of the unshaped wave w; the exact-min and exact-max flat regions
    // grow with the mix; and w = 0.5 maps to 0.5 for every mix below 255.
    {
        const uint8_t   mixes[4] = { 1, 64, 128, 254 };
        static float    w_values[512];
        static float    out_values[512];
        static uint32_t order[512];
        uint32_t        zero_counts[4] = {};
        uint32_t        one_counts[4]  = {};
        for (uint32_t mix_index = 0; mix_index < 4; mix_index++) {
            Synth::LFODescriptor lfo = { Synth::WaveType::sine_wave, 0, 997, 0.0f, 1.0f };
            lfo.pulse_mix            = mixes[mix_index];
            for (uint32_t tick = 0; tick < 512; tick++) {
                w_values[tick]   = lfo_reference_normalized(Synth::WaveType::sine_wave, 0, tick, 997);
                out_values[tick] = Synth::eval_lfo(lfo, tick, 256, 44100);
                TEST(out_values[tick] >= 0.0f && out_values[tick] <= 1.0f);
                if (out_values[tick] == 0.0f) {
                    zero_counts[mix_index]++;
                }
                if (out_values[tick] == 1.0f) {
                    one_counts[mix_index]++;
                }
                order[tick] = tick;
            }
            // Sort the tick indices by w (insertion sort) and require the
            // shaped output to be non-decreasing in w.
            for (uint32_t i = 1; i < 512; i++) {
                const uint32_t current = order[i];
                uint32_t       j       = i;
                while (j > 0 && w_values[order[j - 1]] > w_values[current]) {
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = current;
            }
            for (uint32_t i = 0; i + 1 < 512; i++) {
                TEST(out_values[order[i]] <= out_values[order[i + 1]] + 1.0e-6f);
            }
            // Sine at phase 0 has w = 0.5 exactly; every mix below 255 keeps
            // the mid value there.
            TEST(approx(Synth::eval_lfo(lfo, 0, 256, 1024), 0.5f, 1.0e-4f));
        }
        for (uint32_t mix_index = 1; mix_index < 4; mix_index++) {
            TEST(zero_counts[mix_index] >= zero_counts[mix_index - 1]);
            TEST(one_counts[mix_index] >= one_counts[mix_index - 1]);
        }
        TEST(zero_counts[3] > zero_counts[0]);
        TEST(one_counts[3] > one_counts[0]);
    }

    // R2b: start points.  Both waves start at the middle value, rising; the
    // start point is a constant phase offset of one cycle.  At phase 0:
    // middle = 0.5 for both waves, sine lowest = 0.0 (cycle position 3/4),
    // sine highest = 1.0 (1/4), saw lowest = 0.0 (position 0), saw highest =
    // 1.0 at position duty (duty 0x80).  Composition with the morph: a start
    // point with pulse_mix 255 keeps the step shape and shifts the transition
    // to the offset phase.
    {
        Synth::LFODescriptor start_sine_lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        Synth::LFODescriptor saw_lfo        = { Synth::WaveType::sawtooth_wave, 0x80, 1000, 0.0f, 1.0f };
        start_sine_lfo.start_point          = 0;
        saw_lfo.start_point                 = 0;
        TEST(Synth::eval_lfo(start_sine_lfo, 0, 256, 1024) == 0.5f);
        TEST(Synth::eval_lfo(saw_lfo, 0, 256, 1024) == 0.5f);
        start_sine_lfo.start_point = 1;
        saw_lfo.start_point        = 1;
        TEST(approx(Synth::eval_lfo(start_sine_lfo, 0, 256, 1024), 0.0f, 0.001f));
        TEST(Synth::eval_lfo(saw_lfo, 0, 256, 1024) == 0.0f);
        start_sine_lfo.start_point = 2;
        saw_lfo.start_point        = 2;
        TEST(approx(Synth::eval_lfo(start_sine_lfo, 0, 256, 1024), 1.0f, 0.001f));
        TEST(Synth::eval_lfo(saw_lfo, 0, 256, 1024) == 1.0f);
        // Lowest start plus full mix: phase 0 sits at cycle position 3/4
        // (min); a quarter period in the position wraps to 0, where
        // w = 0.5 exactly and the step reads the max.
        start_sine_lfo.start_point = 1;
        start_sine_lfo.pulse_mix   = 255;
        TEST(Synth::eval_lfo(start_sine_lfo, 0, 256, 1024) == 0.0f);
        TEST(Synth::eval_lfo(start_sine_lfo, 1, 256, 1024) == 1.0f);
    }

    // The rate source offset and the >= 1 ms clamp compose with the
    // resolved beat_div base.  Beat_div 3 at 120 BPM resolves to 500 ms.
    {
        Synth::ParamDescriptor::LFOParam gen = {};
        gen.lfo                              = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        gen.lfo.beat_div                     = 3;
        gen.op                               = Synth::SourceOp::add;
        gen.depth                            = 0.5f;
        gen.rate_param_id                    = 7;
        gen.rate_scale_ms                    = 100.0f;
        // Rate source 1 offsets the resolved base: 500 + 100 = 600 ms.
        TEST(Synth::eval_sourced_lfo(gen, 0.0f, 1.0f, 3) == Synth::eval_lfo_mod(gen.lfo,
                                                                                3,
                                                                                Synth::rt_step_samples,
                                                                                Synth::rt_sampling_rate,
                                                                                600,
                                                                                0.5f,
                                                                                Synth::SourceOp::add));
        // Scale -1000 drives the offset to -500 ms; the clamp keeps 1 ms.
        gen.rate_scale_ms = -1000.0f;
        TEST(Synth::eval_sourced_lfo(gen, 0.0f, 1.0f, 3) == Synth::eval_lfo_mod(gen.lfo,
                                                                                3,
                                                                                Synth::rt_step_samples,
                                                                                Synth::rt_sampling_rate,
                                                                                1,
                                                                                0.5f,
                                                                                Synth::SourceOp::add));
        // With no rate source the resolved division period wins over the
        // descriptor's own absolute period_ms.
        gen.rate_param_id = 0;
        gen.lfo.period_ms = 12345;
        TEST(Synth::eval_sourced_lfo(gen, 0.0f, 0.0f, 3) == Synth::eval_lfo_mod(gen.lfo,
                                                                                3,
                                                                                Synth::rt_step_samples,
                                                                                Synth::rt_sampling_rate,
                                                                                500,
                                                                                0.5f,
                                                                                Synth::SourceOp::add));
    }

    // Depth 0 is the neutral contribution: add contributes exactly 0 and
    // multiply exactly 1 (the pulse shaper leaves these op semantics unchanged).
    {
        const Synth::LFODescriptor lfo = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        TEST(Synth::eval_lfo_mod(lfo, 3, 256, 1024, 1000, 0.0f, Synth::SourceOp::add) == 0.0f);
        TEST(Synth::eval_lfo_mod(lfo, 3, 256, 1024, 1000, 0.0f, Synth::SourceOp::multiply) == 1.0f);
        TEST(Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1000, 0.0f, Synth::SourceOp::add) == 0.0f);
        TEST(Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1000, 0.0f, Synth::SourceOp::multiply) == 1.0f);
    }

    // T2 (E6/E7): the LFO descriptor's pulse_mix, beat_div, and start_point
    // survive the bank JSON roundtrip under their planned key names, are
    // omitted at the defaults (key_uint_nonzero style), and old documents
    // without the keys decode with the zero defaults.  The library document
    // format (frequency_hz dialect) carries the same three keys through its
    // own encode and decode sites; the effects document shares enc_lfo and
    // decode_lfo with the bank format, so the bank roundtrip covers it.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> t2_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& t2_bank = *t2_bank_storage;
        Synth::init_default_bank(&t2_bank.bank);
        Sculptor::set_identity_osc_sources(&t2_bank, 0, 0);
        const uint32_t t2_lfo_slot = t2_bank.bank.lfos.allocate();
        TEST(t2_lfo_slot != pool_no_slot);
        Synth::LFODescriptor& t2_lfo = t2_bank.bank.lfos.entries[t2_lfo_slot];
        t2_lfo.wave                  = Synth::WaveType::sine_wave;
        t2_lfo.period_ms             = 300;
        t2_lfo.min_value             = -1.0f;
        t2_lfo.min_max_delta         = 2.0f;
        t2_lfo.pulse_mix             = 200;
        t2_lfo.beat_div              = 3;
        t2_lfo.start_point           = 2;
        TEST(Synth::validate_instrument_bank(&t2_bank.bank));
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> t2_payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&t2_payload)[Synth::library_payload_max] = t2_payload_storage->elements;
        const uint32_t t2_len = Synth::encode_editor_bank_json(&t2_bank, t2_payload, sizeof(t2_payload));
        TEST(t2_len > 0);
        // E6 roundtrip: the nonzero fields encode verbatim and survive a decode
        // into a zeroed bank.
        TEST(strstr(t2_payload, "\"pulse_mix\":200") != nullptr);
        TEST(strstr(t2_payload, "\"beat_div\":3") != nullptr);
        TEST(strstr(t2_payload, "\"start_point\":2") != nullptr);
        const std::unique_ptr<Synth::InstrumentEditorBank> t2_decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& t2_decoded = *t2_decoded_storage;
        memset(static_cast<void*>(&t2_decoded), 0, sizeof(t2_decoded));
        TEST(Synth::decode_editor_bank_json(t2_payload, t2_len, &t2_decoded));
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].pulse_mix == 200);
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].beat_div == 3);
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].start_point == 2);
        TEST(Synth::validate_instrument_bank(&t2_decoded.bank));
        // E6 omission: the defaults 0/0/0 leave the document free of all three
        // keys, and that document decodes back to the defaults.
        t2_lfo.pulse_mix              = 0;
        t2_lfo.beat_div               = 0;
        t2_lfo.start_point            = 0;
        const uint32_t t2_default_len = Synth::encode_editor_bank_json(&t2_bank, t2_payload, sizeof(t2_payload));
        TEST(t2_default_len > 0);
        TEST(strstr(t2_payload, "\"pulse_mix\"") == nullptr);
        TEST(strstr(t2_payload, "\"beat_div\"") == nullptr);
        TEST(strstr(t2_payload, "\"start_point\"") == nullptr);
        memset(static_cast<void*>(&t2_decoded), 0, sizeof(t2_decoded));
        TEST(Synth::decode_editor_bank_json(t2_payload, t2_default_len, &t2_decoded));
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].pulse_mix == 0);
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].beat_div == 0);
        TEST(t2_decoded.bank.lfos.entries[t2_lfo_slot].start_point == 0);
        // E7 old bank: an LFO object without the three keys (the hand-written
        // document stands in for a pre-M2 bank) decodes with the defaults.
        {
            static const char t2_old_doc[] =
                "{\"instrument_editor_bank\":{\"lfos\":[{\"wave\":\"sine\",\"period_ms\":100}]}}";
            const std::unique_ptr<Synth::InstrumentEditorBank> t2_old_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& t2_old = *t2_old_storage;
            memset(static_cast<void*>(&t2_old), 0, sizeof(t2_old));
            TEST(Synth::decode_editor_bank_json(t2_old_doc, static_cast<uint32_t>(sizeof(t2_old_doc) - 1), &t2_old));
            TEST(t2_old.bank.lfos.entries[0].pulse_mix == 0);
            TEST(t2_old.bank.lfos.entries[0].beat_div == 0);
            TEST(t2_old.bank.lfos.entries[0].start_point == 0);
        }
        // T2 item 4 (out-of-range): splicing beat_div 6 and start_point 7 into
        // an LFO object still decodes, each field reads 0, and the bank passes
        // validate_instrument_bank (garbage-tolerant decode, section 17.2).
        {
            static const char t2_oor_doc[] =
                "{\"instrument_editor_bank\":{\"lfos\":[{\"wave\":\"sine\",\"period_ms\":100}]}}";
            char           t2_oor_spliced[256];
            const uint32_t t2_oor_base_len = static_cast<uint32_t>(sizeof(t2_oor_doc) - 1);
            const uint32_t t2_beat_len     = splice_single_occurrence(t2_oor_doc,
                                                                      t2_oor_base_len,
                                                                      "\"period_ms\":100",
                                                                      "\"period_ms\":100,\"beat_div\":6",
                                                                      t2_oor_spliced,
                                                                      sizeof(t2_oor_spliced));
            TEST(t2_beat_len > 0);
            const std::unique_ptr<Synth::InstrumentEditorBank> t2_beat_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& t2_beat = *t2_beat_storage;
            memset(static_cast<void*>(&t2_beat), 0, sizeof(t2_beat));
            TEST(Synth::decode_editor_bank_json(t2_oor_spliced, t2_beat_len, &t2_beat));
            TEST(t2_beat.bank.lfos.entries[0].beat_div == 0);
            TEST(t2_beat.bank.lfos.entries[0].start_point == 0);
            TEST(Synth::validate_instrument_bank(&t2_beat.bank));
            const uint32_t t2_start_len = splice_single_occurrence(t2_oor_doc,
                                                                   t2_oor_base_len,
                                                                   "\"period_ms\":100",
                                                                   "\"period_ms\":100,\"start_point\":7",
                                                                   t2_oor_spliced,
                                                                   sizeof(t2_oor_spliced));
            TEST(t2_start_len > 0);
            const std::unique_ptr<Synth::InstrumentEditorBank> t2_start_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& t2_start = *t2_start_storage;
            memset(static_cast<void*>(&t2_start), 0, sizeof(t2_start));
            TEST(Synth::decode_editor_bank_json(t2_oor_spliced, t2_start_len, &t2_start));
            TEST(t2_start.bank.lfos.entries[0].beat_div == 0);
            TEST(t2_start.bank.lfos.entries[0].start_point == 0);
            TEST(Synth::validate_instrument_bank(&t2_start.bank));
        }
        // T2c: the library document format carries the three keys too.  The
        // decode refuses unknown fields today, so a keyed document must decode
        // and land the values in the instrument's LFO table; the absence form
        // decodes to the defaults on old documents.
        {
            static const char t2_doc_keys[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                              "\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":4,\"duty\":0.25,"
                                              "\"pulse_mix\":200,\"beat_div\":3,\"start_point\":2,\"depth\":0.5}}}]}";
            static const char t2_doc_plain[] =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                "\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":4,\"duty\":0.25,"
                "\"depth\":0.5}}}]}";
            const std::unique_ptr<Synth::Instrument> t2_instr_storage = std::make_unique<Synth::Instrument>();
            Synth::Instrument&                       t2_instr         = *t2_instr_storage;
            Synth::EnvelopeDescriptor                t2_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor                     t2_doc_lfos[Synth::instrument_max_lfos]  = {};
            uint32_t                                 t2_env_count = Synth::instrument_max_envelopes;
            uint32_t                                 t2_lfo_count = Synth::instrument_max_lfos;
            TEST(Synth::decode_instrument_json(t2_doc_keys,
                                               static_cast<uint32_t>(sizeof(t2_doc_keys) - 1),
                                               &t2_instr,
                                               t2_envs,
                                               &t2_env_count,
                                               t2_doc_lfos,
                                               &t2_lfo_count));
            TEST(t2_lfo_count == 1);
            TEST(t2_doc_lfos[0].pulse_mix == 200);
            TEST(t2_doc_lfos[0].beat_div == 3);
            TEST(t2_doc_lfos[0].start_point == 2);
            // The keyed descriptor re-encodes with the three keys: stage the
            // decoded LFO in the descriptor bank the encoder resolves ids
            // against.  Staged before the plain-document decode below, whose
            // descriptor reset zeroes the slot.
            const std::unique_ptr<Synth::InstrumentBank> t2_rt_bank_storage = std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank&                       t2_rt_bank         = *t2_rt_bank_storage;
            memset(static_cast<void*>(&t2_rt_bank), 0, sizeof(t2_rt_bank));
            t2_rt_bank.lfos.entries[0]    = t2_doc_lfos[0];
            t2_rt_bank.lfos.num_allocated = 1;
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> t2_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&t2_text)[clipboard_capacity] = t2_text_storage->elements;
            TEST(Synth::encode_instrument_json(t2_text, sizeof(t2_text), &t2_instr, &t2_rt_bank) > 0);
            TEST(strstr(t2_text, "\"pulse_mix\":200") != nullptr);
            TEST(strstr(t2_text, "\"beat_div\":3") != nullptr);
            TEST(strstr(t2_text, "\"start_point\":2") != nullptr);
            TEST(Synth::decode_instrument_json(t2_doc_plain,
                                               static_cast<uint32_t>(sizeof(t2_doc_plain) - 1),
                                               &t2_instr,
                                               t2_envs,
                                               &t2_env_count,
                                               t2_doc_lfos,
                                               &t2_lfo_count));
            TEST(t2_doc_lfos[0].pulse_mix == 0);
            TEST(t2_doc_lfos[0].beat_div == 0);
            TEST(t2_doc_lfos[0].start_point == 0);
        }
    }

    // random_pitch_skew: a nonzero amount stays within [-amount, amount] and spans most of it;
    // amount 0 is exactly 0 (deterministic, generator unadvanced).
    {
        RNG skew_rng;
        skew_rng.init(0x5eed1234u);
        TEST(Synth::random_pitch_skew(&skew_rng, 0.0f) == 0.0f);

        const float amount   = 0.25f;
        float       skew_min = 1e9f;
        float       skew_max = -1e9f;
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
        TEST(skew_max > amount * 0.9f);
    }

    // Keyboard split routing: route_instrument maps a note to an instrument via an ordered
    // table; stored starts are first-note + 1 (0 = unused slot), so an all-zero table resolves to instrument 0.
    {
        const uint32_t count = Synth::max_instr_per_channel;

        Synth::Zone empty[Synth::max_instr_per_channel] = {};
        TEST(Synth::route_instrument(empty, count, 0) == 0);
        TEST(Synth::route_instrument(empty, count, 60) == 0);
        TEST(Synth::route_instrument(empty, count, 127) == 0);

        // Notes 0..59 -> instrument 0, notes 60.. -> instrument 1.  Slot 0 stores 1
        // (zone 0 starts at note 0); 0 is the end-of-table sentinel.
        Synth::Zone split[Synth::max_instr_per_channel] = {};
        split[0]                                        = { 1, 0 };
        split[1]                                        = { 61, 1 };
        ;
        TEST(Synth::route_instrument(split, count, 0) == 0);
        TEST(Synth::route_instrument(split, count, 59) == 0);
        TEST(Synth::route_instrument(split, count, 60) == 1);
        TEST(Synth::route_instrument(split, count, 127) == 1);

        Synth::Zone three[Synth::max_instr_per_channel] = {};
        three[0]                                        = { 1, 2 };
        three[1]                                        = { 49, 4 };
        ;
        three[2] = { 73, 3 };
        ;
        TEST(Synth::route_instrument(three, count, 47) == 2);
        TEST(Synth::route_instrument(three, count, 48) == 4);
        TEST(Synth::route_instrument(three, count, 71) == 4);
        TEST(Synth::route_instrument(three, count, 72) == 3);

        // A full table with no sentinel still resolves the top range.
        Synth::Zone full[Synth::max_instr_per_channel] = {};
        for (uint32_t idx = 0; idx < count; idx++) {
            full[idx] = { static_cast<uint8_t>(idx + 1), static_cast<uint8_t>(idx) };
        }
        TEST(Synth::route_instrument(full, count, 127) == count - 1);
    }

    // Per-layer routing (data model): each layer owns its bindings for a target, so two
    // layers of one instrument carry different base values and different routed inputs for
    // the same target and read back distinctly.  The runtime reads
    // instrument.layers[layer_idx].targets[target] via one per-target lookup; the dynamic
    // two-layer behavior is covered by the manual audio check because realtime_synth.cpp is
    // not linked into the test suites.
    {
        Synth::Instrument instrument = {};
        instrument.layer_count       = 2;
        set_default_dispersion_sections(instrument);
        instrument.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
        instrument.layers[1].osc_type[0] = Synth::WaveType::sawtooth_wave;

        Synth::TargetBinding& low  = instrument.layers[0].targets[Synth::mod_volume];
        low.base_value             = 0.25f;
        low.num_inputs             = 1;
        low.inputs[0]              = { Synth::ModSource::velocity, Synth::SourceOp::add, 0, 0.5f };
        Synth::TargetBinding& high = instrument.layers[1].targets[Synth::mod_volume];
        high.base_value            = 0.75f;
        high.num_inputs            = 2;
        high.inputs[0]             = { Synth::ModSource::mod_wheel, Synth::SourceOp::multiply, 0, -0.5f };
        high.inputs[1]             = { Synth::ModSource::pitch_bend, Synth::SourceOp::add, 0, 2.0f };

        // Both layers' bindings for the same target read back distinctly.
        TEST(instrument.layers[0].targets[Synth::mod_volume].base_value == 0.25f);
        TEST(instrument.layers[1].targets[Synth::mod_volume].base_value == 0.75f);
        TEST(instrument.layers[0].targets[Synth::mod_volume].num_inputs == 1);
        TEST(instrument.layers[1].targets[Synth::mod_volume].num_inputs == 2);
        TEST(instrument.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
        TEST(instrument.layers[1].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(instrument.layers[1].targets[Synth::mod_volume].inputs[1].source == Synth::ModSource::pitch_bend);
        TEST(memcmp(&instrument.layers[0].targets[Synth::mod_volume],
                    &instrument.layers[1].targets[Synth::mod_volume],
                    sizeof(Synth::TargetBinding)) != 0);

        // The validator accepts divergent per-layer routing.
        const std::unique_ptr<Synth::InstrumentBank> probe_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       probe_bank         = *probe_bank_storage;
        memset(static_cast<void*>(&probe_bank), 0, sizeof(probe_bank));
        const uint32_t slot = probe_bank.instruments.allocate();
        TEST(slot != pool_no_slot);
        probe_bank.instruments.entries[slot] = instrument;
        probe_bank.channel_enabled[0]        = 1;
        probe_bank.channel_zones[0][0]       = { 1, static_cast<uint8_t>(slot) };
        TEST(Synth::validate_instrument_bank(&probe_bank));
    }

    // --- Phase 1 data foundation: generic pools, container, defragment + remap ---

    // alloc-distinct: three allocations from an empty pool give distinct, in-range slots.
    {
        Pool<Synth::EnvelopeDescriptor, Synth::max_envelopes> env_pool = {};
        const uint32_t                                        slot0    = env_pool.allocate();
        const uint32_t                                        slot1    = env_pool.allocate();
        const uint32_t                                        slot2    = env_pool.allocate();
        TEST(slot0 != pool_no_slot && slot1 != pool_no_slot && slot2 != pool_no_slot);
        TEST(slot0 != slot1 && slot1 != slot2 && slot0 != slot2);
        TEST(slot0 < Synth::max_envelopes && slot1 < Synth::max_envelopes && slot2 < Synth::max_envelopes);
        TEST(env_pool.num_allocated == 3);
    }

    // reuse-after-free: freeing the middle slot lets the next allocate reuse it.
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = {};
        const uint32_t                              first    = lfo_pool.allocate();
        const uint32_t                              middle   = lfo_pool.allocate();
        const uint32_t                              last     = lfo_pool.allocate();
        TEST(first != pool_no_slot && last != pool_no_slot);
        lfo_pool.free(middle);
        TEST(lfo_pool.num_allocated == 2);
        const uint32_t reused = lfo_pool.allocate();
        TEST(reused == middle);
        TEST(lfo_pool.num_allocated == 3);
    }

    // full-pool: allocate up to capacity, then allocate fails with pool_no_slot (no OOB).
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = {};
        for (uint32_t idx = 0; idx < Synth::max_lfos; idx++) {
            TEST(lfo_pool.allocate() != pool_no_slot);
        }
        TEST(lfo_pool.allocate() == pool_no_slot);
        TEST(lfo_pool.num_allocated == Synth::max_lfos);
    }

    // defrag-remap (envelopes): a fragmented pool [used,free,used,free,used] compacts to the
    // front preserving order, and an instrument's 1-based envelope reference is rewritten.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.envelopes.allocate() == idx);
        }
        bank.envelopes.free(1);
        bank.envelopes.free(3);

        // Tag slot 4's data so we can find where it lands, and reference it from a layer
        // (desc_id is 1-based: slot 4 -> id 5).
        bank.envelopes.entries[4].num_points = 42;
        const uint32_t instr                 = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layer_count = 1;
        set_default_dispersion_sections(bank.instruments.entries[instr]);
        bank.instruments.entries[instr].layers[0].targets[Synth::mod_volume].envelope_desc_id = 5;

        uint32_t env_map[Synth::max_envelopes];
        bank.envelopes.defragment(env_map);
        TEST(bank.envelopes.num_allocated == 3);
        TEST(env_map[0] == 0); // old 0,2,4 -> new 0,1,2 in order
        TEST(env_map[2] == 1);
        TEST(env_map[4] == 2);
        TEST(env_map[1] == pool_no_slot);
        TEST(env_map[3] == pool_no_slot);
        TEST(bank.envelopes.entries[2].num_points == 42); // slot 4's data moved to slot 2
    }

    // defrag-remap (LFOs): same mechanism as envelopes -- a layer's 1-based lfo_desc_id is
    // rewritten to the referenced LFO entry's new slot.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.lfos.allocate() == idx);
        }
        bank.lfos.free(1);
        bank.lfos.free(3);
        bank.lfos.entries[4].period_ms = 333;

        const uint32_t instr = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layers[0].targets[Synth::mod_pitch].lfo_desc_id = 5; // slot 4 -> id 5

        uint32_t lfo_map[Synth::max_lfos];
        bank.lfos.defragment(lfo_map);
        TEST(lfo_map[4] == 2);
        TEST(bank.lfos.entries[2].period_ms == 333);
    }

    // defrag-remap (instruments): an instrument pool compacts and a channel split-table entry
    // that references an instrument (0-based) is rewritten to the new slot.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.instruments.allocate() == idx);
        }
        bank.instruments.free(1);
        bank.instruments.free(3);
        bank.channel_zones[0][0] = { 1, 4 };  // note >= 1 -> instrument slot 4 (survives)
        bank.channel_zones[0][1] = { 65, 3 }; // references deleted instrument slot 3

        uint32_t instr_map[Synth::max_instruments];
        bank.instruments.defragment(instr_map);
        TEST(instr_map[4] == 2);
    }

    // snapshot-roundtrip: a byte copy of the container, then a byte restore after mutation,
    // reproduces identical state -- the property the undo stack and save/load rely on.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        const uint32_t                               instr        = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count               = 3;
        set_default_dispersion_sections(bank.instruments.entries[instr]);
        const uint32_t env                     = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points = 7;

        const std::unique_ptr<Synth::InstrumentBank> snapshot_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       snapshot         = *snapshot_storage;
        memcpy(&snapshot, &bank, sizeof(bank));

        bank.instruments.entries[instr].layer_count = 99;
        set_default_dispersion_sections(bank.instruments.entries[instr]);
        bank.instruments.free(instr);

        memcpy(&bank, &snapshot, sizeof(bank));
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.instruments.entries[instr].layer_count == 3);
        TEST(bank.envelopes.entries[env].num_points == 7);
        TEST(memcmp(&bank, &snapshot, sizeof(bank)) == 0);
    }

    // MIDI file: format 0, single track, division 96.  A tempo meta (500000 us/quarter =
    // 120 BPM), a note_on at delta 0, and a note_off one quarter (delta 96) later.  At 44100 Hz
    // a quarter note is 0.5 s = 22050 samples, so the note_off lands at sample 22050.
    {
        static const uint8_t midi[] = {
            // MThd
            0x4D,
            0x54,
            0x68,
            0x64, // "MThd"
            0x00,
            0x00,
            0x00,
            0x06, // header length 6
            0x00,
            0x00, // format 0
            0x00,
            0x01, // ntrks 1
            0x00,
            0x60, // division 96
            // MTrk
            0x4D,
            0x54,
            0x72,
            0x6B, // "MTrk"
            0x00,
            0x00,
            0x00,
            0x13, // track length 19
            0x00,
            0xFF,
            0x51,
            0x03,
            0x07,
            0xA1,
            0x20, // delta 0, tempo 500000 us/quarter
            0x00,
            0x90,
            0x3C,
            0x64, // delta 0, note_on ch0 note 60 vel 100
            0x60,
            0x80,
            0x3C,
            0x40, // delta 96, note_off ch0 note 60 vel 64
            0x00,
            0xFF,
            0x2F,
            0x00, // delta 0, end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t   count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
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
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x60, 0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x0B, // track length 11
            0x00, 0x90, 0x3C, 0x64,                                           // delta 0, note_on ch0 note 60 vel 100
            0x30, 0x3E, 0x64,       // delta 48, running status: note_on note 62 vel 100
            0x00, 0xFF, 0x2F, 0x00, // end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t   count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
        TEST(count == 2);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].note == 60);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::note_on);
        TEST(events[1].note == 62);
        TEST(events[1].time == 11025); // delta 48 ticks = half a quarter = 11025 samples
    }

    // MIDI file: malformed / truncated inputs return 0 without crashing (checked under ASAN).
    {
        Synth::MidiEvent events[8];
        // Empty buffer.
        TEST(Synth::parse_midi_file(nullptr, 0, events, 8, 44100) == 0);
        // Bad magic.
        uint8_t bad_magic[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        TEST(Synth::parse_midi_file(bad_magic, sizeof(bad_magic), events, 8, 44100) == 0);
        // Truncated valid header (track length claims more bytes than present).
        static const uint8_t truncated[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x60, 0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x40, // claims 64 bytes but track is cut off
            0x00, 0x90, 0x3C,                                                 // incomplete note_on
        };
        TEST(Synth::parse_midi_file(truncated, sizeof(truncated), events, 8, 44100) == 0);
        // SMPTE division (high bit set) is rejected.
        static const uint8_t smpte[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01,
            0xE8, 0x04, // negative (SMPTE) division
            0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x04, 0x00, 0xFF, 0x2F, 0x00,
        };
        TEST(Synth::parse_midi_file(smpte, sizeof(smpte), events, 8, 44100) == 0);
    }

    // A file whose sample time exceeds the 32-bit field is rejected (no UB on the cast).  An
    // extreme tempo (0xFFFFFF us/quarter) at division 1 makes one tick ~740k samples, so a
    // 6000-tick delta overruns UINT32_MAX.
    {
        const uint8_t overflow_midi[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, // format 0
            0x00, 0x01,                                                 // one track
            0x00, 0x01,                                                 // division: 1 tick per quarter
            0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x10,             // track length 16
            0x00, 0xFF, 0x51, 0x03, 0xFF, 0xFF, 0xFF,                   // tempo 0xFFFFFF us/quarter
            0xAE, 0x70, 0x90, 0x3C, 0x64,                               // delta 6000, note_on ch0 note 60 vel 100
            0x00, 0xFF, 0x2F, 0x00,                                     // end of track
        };
        Synth::MidiEvent overflow_events[8] = {};
        TEST(Synth::parse_midi_file(overflow_midi, sizeof(overflow_midi), overflow_events, 8, 44100) == 0);
    }

    // soundtrack codec round-trips a tick-domain, time-ordered stream.  encode pairs each
    // note_on with its later note_off into a note_on + duration; decode reconstructs the note_off
    // at start + duration.  Times are MIDI ticks.  note_off velocity is not stored (the duration
    // model drops it), so note_offs here carry note_data 0, matching the reconstructed value.
    {
        Synth::MidiEvent events[7] = {};

        events[0].time      = 0;
        events[0].event     = Synth::EvType::note_on;
        events[0].channel   = 0;
        events[0].note      = 60;
        events[0].note_data = 100;

        events[1].time            = 0;
        events[1].event           = Synth::EvType::controller;
        events[1].channel         = 2;
        events[1].controller      = 7;
        events[1].controller_data = 120;

        events[2].time      = 50;
        events[2].event     = Synth::EvType::aftertouch;
        events[2].channel   = 0;
        events[2].note      = 60;
        events[2].note_data = 40;

        events[3].time       = 100;
        events[3].event      = Synth::EvType::pitch_bend;
        events[3].channel    = 0;
        events[3].pitch_bend = -2048;

        events[4].time      = 100;
        events[4].event     = Synth::EvType::note_on;
        events[4].channel   = 2;
        events[4].note      = 67;
        events[4].note_data = 90;

        events[5].time      = 200;
        events[5].event     = Synth::EvType::note_off;
        events[5].channel   = 0;
        events[5].note      = 60;
        events[5].note_data = 0;

        events[6].time      = 300;
        events[6].event     = Synth::EvType::note_off;
        events[6].channel   = 2;
        events[6].note      = 67;
        events[6].note_data = 0;

        const uint32_t    event_count = 7;
        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = {};
        const uint32_t    written    = Synth::encode_soundtrack(events, event_count, dest, sizeof(dest), &soundtrack);
        TEST(written > 0);

        Synth::MidiEvent decoded[7] = {};
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 7) == event_count);
        TEST(memcmp(events, decoded, event_count * sizeof(Synth::MidiEvent)) == 0);

        // A buffer too small to hold the planes fails cleanly.
        Synth::Soundtrack scratch = {};
        TEST(Synth::encode_soundtrack(events, event_count, dest, 4, &scratch) == 0);

        // An out-of-range channel is rejected.
        Synth::MidiEvent bad_channel = events[0];
        bad_channel.channel          = Synth::max_channels;
        TEST(Synth::encode_soundtrack(&bad_channel, 1, dest, sizeof(dest), &scratch) == 0);
    }

    // an unmatched note_on (no note_off) encodes as an unbounded note and decodes back to a
    // single note_on with no reconstructed note_off.
    {
        Synth::MidiEvent events[1] = {};
        events[0].time             = 0;
        events[0].event            = Synth::EvType::note_on;
        events[0].channel          = 0;
        events[0].note             = 48;
        events[0].note_data        = 80;

        uint8_t           dest[64];
        Synth::Soundtrack soundtrack = {};
        TEST(Synth::encode_soundtrack(events, 1, dest, sizeof(dest), &soundtrack) > 0);

        Synth::MidiEvent decoded[4] = {};
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 4) == 1);
        TEST(decoded[0].event == Synth::EvType::note_on);
        TEST(decoded[0].note == 48 && decoded[0].note_data == 80);
    }

    // D3 (retrigger + multi-byte VLQ): an overlapping same-note retrigger closes the earlier note
    // at the new note_on's time, and large tick deltas/durations (> 0x3FFF) exercise multi-byte VLQ.
    {
        Synth::MidiEvent events[3] = {};
        events[0].time             = 0;
        events[0].event            = Synth::EvType::note_on;
        events[0].channel          = 0;
        events[0].note             = 64;
        events[0].note_data        = 100;
        events[1].time             = 20000;
        events[1].event            = Synth::EvType::note_on; // retrigger; delta > 0x3FFF
        events[1].channel          = 0;
        events[1].note             = 64;
        events[1].note_data        = 110;
        events[2].time             = 60000;
        events[2].event            = Synth::EvType::note_off; // duration > 0x3FFF
        events[2].channel          = 0;
        events[2].note             = 64;
        events[2].note_data        = 0;

        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = {};
        TEST(Synth::encode_soundtrack(events, 3, dest, sizeof(dest), &soundtrack) > 0);

        // Decoded: note_on@0, note_off@20000 (earlier note closed at retrigger), note_on@20000,
        // note_off@60000.  Sort is by (time, channel); same-time events keep input order.
        Synth::MidiEvent decoded[8]    = {};
        const uint32_t   decoded_count = Synth::decode_soundtrack(soundtrack, decoded, 8);
        TEST(decoded_count == 4);

        uint32_t note_offs     = 0;
        uint32_t off_at_20000  = 0;
        uint32_t off_at_60000  = 0;
        int      off_idx_20000 = -1;
        int      on_idx_20000  = -1;
        for (uint32_t i = 0; i < decoded_count; i++) {
            if (decoded[i].event == Synth::EvType::note_off) {
                note_offs++;
                if (decoded[i].time == 20000) {
                    off_at_20000++;
                    off_idx_20000 = (int)i;
                }
                if (decoded[i].time == 60000) {
                    off_at_60000++;
                }
                TEST(decoded[i].note == 64);
            }
            else if (decoded[i].event == Synth::EvType::note_on && decoded[i].time == 20000) {
                on_idx_20000 = (int)i;
            }
        }
        TEST(note_offs == 2);
        TEST(off_at_20000 == 1); // earlier note closed at the retrigger
        TEST(off_at_60000 ==
             1); // second note closed by its real note_off
                 // The reconstructed note_off must precede the retriggered note_on at the same tick, so the
                 // player closes the old voice before allocating the new one (else the retrigger drops it).
        TEST(off_idx_20000 >= 0 && on_idx_20000 >= 0 && off_idx_20000 < on_idx_20000);
    }

    // a note_on's stored duration resolves to the absolute sample at which the player
    // auto-releases its voice (0 = none).  Stored value v means a real duration of v - 1 ticks.
    {
        TEST(Synth::soundtrack_note_release_sample(1000, 0, 50) == 0);     // unbounded -> none
        TEST(Synth::soundtrack_note_release_sample(1000, 11, 50) == 1500); // 10 ticks * 50 + 1000
        TEST(Synth::soundtrack_note_release_sample(1000, 1, 50) == 1000);  // 0-tick note releases at start
        TEST(Synth::soundtrack_note_release_sample(0, 1, 50) == 0);        // degenerate: 0-tick at 0 -> none
    }

    // ---- Instrument bank: dense-pool validation, serialize, publish queue ----

    // Builds a small valid bank: one instrument, one envelope, one LFO, one zone.
    // Builds a valid bank whose owner chain's first slot is an enabled delay with one input row
    // driven by source/cc_number (owner >= max_channels selects the master chain).

    // A valid bank passes validation.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Default bank: init_default_bank builds the first-run bank; every
    // structural invariant the editor and runtime rely on holds out of the box.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        TEST(Synth::validate_instrument_bank(&bank));
        TEST(bank.channel_enabled[0] == 1);
        bool others_disabled = true;
        for (uint32_t c = 1; c < Synth::max_channels; c++) {
            others_disabled = others_disabled && bank.channel_enabled[c] == 0;
        }
        TEST(others_disabled);
        char def_name[Synth::max_name_len];
        Synth::get_default_channel_name(0, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Channel 01", 11) == 0);
        Synth::get_default_channel_name(9, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Drum Track", 11) == 0);
        Synth::get_default_channel_name(15, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Channel 16", 11) == 0);
        TEST(bank.channel_zones[0][0].start_note == 1);
        TEST(bank.channel_zones[0][0].instrument == 0);
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.envelopes.num_allocated == 0);       // bare default: no descriptors
        TEST(bank.lfos.num_allocated == 0);            // bare default: no master LFOs
        TEST(bank.channel_chains[0].num_effects == 0); // bare default: no chain
        TEST(bank.master_chain.num_effects == 0);      // bare default: no master effects
        // The bare default instrument: one sine layer with neutral per-layer
        // routing.
        const Synth::Instrument& instr = bank.instruments.entries[0];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].osc_type[0] == Synth::WaveType::sine_wave);
        TEST(instr.layers[0].osc_type[1] == Synth::WaveType::no_wave);
        TEST(instr.layers[0].osc_mode == Synth::osc_mode_blend);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            TEST(instr.layers[0].targets[t].envelope_desc_id == 0);
            TEST(instr.layers[0].targets[t].lfo_desc_id == 0);
            TEST(instr.layers[0].targets[t].num_inputs == 0);
        }
        TEST(instr.layers[0].targets[Synth::mod_volume].base_value == 1.0f);
        TEST(instr.layers[0].targets[Synth::mod_panning].base_value == 0.5f);
    }
    // FIR Slope factory regression: every value-initialized construction
    // path seeds the 100 default, so a factory-fresh bank projects,
    // validates and compiles without an out-of-range slot refusal.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        TEST(bank.instruments.num_allocated == 1);
        for (uint32_t layer = 0; layer < bank.instruments.entries[0].layer_count; layer++) {
            TEST(bank.instruments.entries[0].layers[layer].fir_slope_percent == 100);
        }
        Synth::Oscillator fresh_osc = {};
        TEST(fresh_osc.fir_slope_percent == 100);
        const std::unique_ptr<Synth::InstrumentEditorBank> editor_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& editor_bank = *editor_bank_storage;
        Synth::init_default_bank(&editor_bank.bank);
        Sculptor::set_identity_osc_sources(&editor_bank, 0, 0);
        TEST(Synth::validate_instrument_bank(&editor_bank.bank));
        const std::unique_ptr<Sculptor::Graph> factory_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       factory_graph         = *factory_graph_storage;
        Sculptor::OscGraphMapping              factory_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(editor_bank, &factory_graph, &factory_mapping));
        const std::unique_ptr<Synth::InstrumentBank> factory_playback_storage =
            std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank& factory_playback = *factory_playback_storage;
        TEST(Sculptor::compile_editor_playback_bank(editor_bank, &factory_playback));
        TEST(factory_playback.instruments.entries[factory_playback.channel_zones[0][0].instrument]
                 .layers[0]
                 .fir_slope_percent == 100);
        const std::unique_ptr<Synth::InstrumentEditorBank> channel_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& channel_bank = *channel_bank_storage;
        Synth::init_default_bank(&channel_bank.bank);
        Sculptor::set_identity_osc_sources(&channel_bank, 0, 0);
        TEST(Synth::init_default_channel(&channel_bank.bank, 3));
        const uint32_t fresh_slot = channel_bank.bank.channel_zones[3][0].instrument;
        TEST(channel_bank.bank.instruments.entries[fresh_slot].layers[0].fir_slope_percent == 100);
    }
    // FIR Slope removal reset regression: deleting an oscillator node through
    // the linked graph path compacts the layers, the surviving layer keeps
    // its own slope, and the vacated tail slot resets to the 100 default,
    // which is what a re-added layer starts from when it is not inherited.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> rm_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           rm_bank          = *rm_bank_storage;
        const std::unique_ptr<Sculptor::Graph> rm_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       rm_graph         = *rm_graph_storage;
        Sculptor::OscGraphMapping              rm_mapping       = {};
        Synth::init_default_bank(&rm_bank.bank);
        Sculptor::set_identity_osc_sources(&rm_bank, 0, 0);
        Synth::Instrument& rm_entry = rm_bank.bank.instruments.entries[rm_bank.bank.channel_zones[0][0].instrument];
        rm_entry.layers[1]          = rm_entry.layers[0];
        rm_entry.layers[0].fir_slope_percent = 63; // the survivor keeps its own slope
        rm_entry.layers[1].fir_slope_percent = 37;
        rm_entry.layer_count                 = 2;
        set_default_dispersion_sections(rm_entry);
        TEST(Synth::validate_instrument_bank(&rm_bank.bank));
        TEST(Sculptor::project_editor_to_graph(rm_bank, &rm_graph, &rm_mapping));
        TEST(rm_mapping.osc_nodes[1] != Sculptor::pool_no_slot);
        Sculptor::GraphChange rm_change = {};
        rm_change.kind                  = Sculptor::ChangeKind::node_deleted;
        rm_change.node_idx              = rm_mapping.osc_nodes[1];
        TEST(Sculptor::apply_osc_graph_change(&rm_bank, &rm_graph, &rm_mapping, rm_change));
        TEST(rm_entry.layer_count == 1);
        TEST(rm_entry.layers[0].fir_slope_percent == 63);
        TEST(rm_entry.layers[Synth::max_layers - 1].fir_slope_percent == 100);
        TEST(rm_mapping.osc_nodes[1] == Sculptor::pool_no_slot);
    }

    // init_default_channel into a populated bank appends a bare instrument and
    // allocates no descriptors and no channel effect chain.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        TEST(bank.envelopes.allocate() == 0);
        bank.envelopes.entries[0].num_points         = 2;
        bank.envelopes.entries[0].points[1].position = 100;
        TEST(bank.lfos.allocate() == 0);
        bank.lfos.entries[0].wave      = Synth::WaveType::sine_wave;
        bank.lfos.entries[0].period_ms = 50;
        TEST(bank.instruments.allocate() == 0);
        bank.instruments.entries[0].layer_count = 1;
        set_default_dispersion_sections(bank.instruments.entries[0]);
        TEST(Synth::init_default_channel(&bank, 5));
        TEST(bank.instruments.num_allocated == 2);
        TEST(bank.envelopes.num_allocated == 1);
        TEST(bank.lfos.num_allocated == 1);
        TEST(bank.channel_zones[5][0].start_note == 1);
        TEST(bank.channel_zones[5][0].instrument == 1);
        const Synth::Instrument& instr = bank.instruments.entries[1];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].osc_type[0] == Synth::WaveType::sine_wave);
        TEST(instr.layers[0].osc_type[1] == Synth::WaveType::no_wave);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            TEST(instr.layers[0].targets[t].envelope_desc_id == 0);
            TEST(instr.layers[0].targets[t].lfo_desc_id == 0);
            TEST(instr.layers[0].targets[t].num_inputs == 0);
        }
        TEST(instr.layers[0].targets[Synth::mod_volume].base_value == 1.0f);
        TEST(instr.layers[0].targets[Synth::mod_panning].base_value == 0.5f);
        TEST(bank.channel_chains[5].num_effects == 0);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // init_default_channel fails cleanly when the instruments pool lacks
    // space, leaving the bank byte-for-byte untouched.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        bank.instruments.entries[0].layer_count = 1;
        set_default_dispersion_sections(bank.instruments.entries[0]);
        for (uint32_t i = 0; i < Synth::max_instruments; i++) {
            TEST(bank.instruments.allocate() == i);
        }
        const std::unique_ptr<Synth::InstrumentBank> before_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       before         = *before_storage;
        memcpy(&before, &bank, sizeof(bank));
        TEST(! Synth::init_default_channel(&bank, 2));
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // reclaim_unused_slots removes instruments no enabled channel's zone table references and
    // descriptors no surviving instrument or effect chain references, compacting the pools and
    // remapping zones, names, and references in one pass.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank); // instrument 0 (env 1, lfo 1) on ch0; ch0 and ch1 enabled

        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count = 1;
        set_default_dispersion_sections(bank.bank.instruments.entries[1]);
        bank.bank.instruments.entries[1].layers[0].targets[Synth::mod_volume].envelope_desc_id = 1; // shares env 1
        TEST(bank.bank.instruments.allocate() == 2);
        bank.bank.instruments.entries[2].layer_count = 1;
        set_default_dispersion_sections(bank.bank.instruments.entries[2]);
        TEST(bank.bank.envelopes.allocate() == 1);
        bank.bank.envelopes.entries[1].num_points         = 2;
        bank.bank.envelopes.entries[1].points[1].position = 200;
        bank.bank.instruments.entries[2].layers[0].targets[Synth::mod_volume].envelope_desc_id =
            2;                                    // env 2 roots nothing but instr 2
        bank.bank.channel_zones[1][0] = { 1, 1 }; // ch1 -> instrument 1

        memcpy(bank.instrument_names[0], "KeepA", 6);
        memcpy(bank.instrument_names[1], "KeepB", 6);
        memcpy(bank.instrument_names[2], "Drop", 5);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        Synth::reclaim_unused_slots(&bank);

        // The orphaned instrument is gone; survivors keep their relative order, names included.
        TEST(bank.bank.instruments.num_allocated == 2);
        TEST(bank.bank.channel_zones[0][0].instrument == 0);
        TEST(bank.bank.channel_zones[1][0].instrument == 1);
        TEST(memcmp(bank.instrument_names[0], "KeepA", 6) == 0);
        TEST(memcmp(bank.instrument_names[1], "KeepB", 6) == 0);
        // Env 2 (referenced only by the removed instrument) is reclaimed; env 1 keeps id 1.
        TEST(bank.bank.envelopes.num_allocated == 1);
        TEST(bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
        TEST(bank.bank.instruments.entries[1].layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // Effect chains root LFOs: with every channel disabled the chain's LFO survives while the
    // instrument's LFO is reclaimed, and the chain's reference is remapped to the compacted id.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank); // lfo 1 referenced by instrument 0
        TEST(bank.bank.lfos.allocate() == 1);
        bank.bank.lfos.entries[1].wave                            = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[1].period_ms                       = 90;
        bank.bank.master_chain.num_effects                        = 1;
        bank.bank.master_chain.effects[0].type                    = Synth::EffectType::distortion;
        bank.bank.master_chain.effects[0].enabled                 = true;
        bank.bank.master_chain.effects[0].bindings[0].base_value  = 1.0f;
        bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id = 2;
        TEST(Synth::validate_instrument_bank(&bank.bank));

        bank.bank.channel_enabled[0] = 0;
        bank.bank.channel_enabled[1] = 0;
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.bank.instruments.num_allocated == 0);
        TEST(bank.bank.lfos.num_allocated == 1);
        TEST(bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id == 1); // remapped 2 -> 1
        TEST(bank.bank.lfos.entries[0].wave == Synth::WaveType::sawtooth_wave);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // A clean bank reclaims nothing: the bank comes back byte-for-byte identical.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        const std::unique_ptr<Synth::InstrumentEditorBank> before_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& before = *before_storage;
        memcpy(&before, &bank, sizeof(bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // Disabled channels may hold stale zone bytes; reclaim clears references to removed
    // instruments and remaps the rest without touching the (skipped) zone structure.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank); // instr 0 on ch0
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count = 1;
        set_default_dispersion_sections(bank.bank.instruments.entries[1]);
        bank.bank.channel_zones[1][0] = { 1, 1 }; // ch1 -> instr 1
        TEST(bank.bank.instruments.allocate() == 2);
        bank.bank.instruments.entries[2].layer_count = 1; // orphaned
        set_default_dispersion_sections(bank.bank.instruments.entries[2]);
        bank.bank.channel_enabled[0]  = 0;         // instr 0 loses its last live reference
        bank.bank.channel_zones[2][0] = { 5, 7 };  // ch2 disabled: dangling reference
        bank.bank.channel_zones[2][1] = { 9, 0 };  // reference to the instrument about to be removed
        bank.bank.channel_zones[2][2] = { 13, 1 }; // reference to a survivor (compacts 1 -> 0)
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.bank.instruments.num_allocated == 1);
        TEST(bank.bank.envelopes.num_allocated == 0); // env 1 rooted instr 0 only
        TEST(bank.bank.channel_zones[2][0].instrument == 0);
        TEST(bank.bank.channel_zones[2][1].instrument == 0);
        TEST(bank.bank.channel_zones[2][2].instrument == 0);
        TEST(bank.bank.channel_zones[1][0].instrument == 0);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // Validation negatives: each mutation makes exactly the targeted rule reject the bank.
    auto expect_invalid = [](Synth::InstrumentBank& bank) { TEST(! Synth::validate_instrument_bank(&bank)); };
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.num_allocated = Synth::max_instruments + 1; // over capacity
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.num_allocated = 2; // count mismatch: only 1 occupied
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.free(0); // hole below num_allocated: count/occupied mismatch
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 11, 0 };
        bank.channel_zones[1][2] = { 6, 0 }; // later zone starts before the earlier one
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 0, 0 };
        bank.channel_zones[1][2] = { 20, 0 }; // garbage after the terminator
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 200 }; // slot 0 instrument unoccupied (dangling)
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 20, 1 }; // reachable entry references an unoccupied instrument
        expect_invalid(bank);
    }
    // Slot 0 must start at note 0 (stored start 1); any other first start is invalid.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 5, 0 };
        expect_invalid(bank);
    }
    // Disabled channels keep arbitrary zone bytes: garbage zones pass while disabled and
    // fail once the channel is enabled.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_zones[3][0] = { 9, 7 };
        bank.channel_zones[3][1] = { 2, 80 }; // out of order
        bank.channel_enabled[3]  = 0;
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_enabled[3] = 1;
        expect_invalid(bank);
    }
    // channel_enabled bytes must be 0/1.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_enabled[2] = 2;
        expect_invalid(bank);
    }
    // A full zone table with no terminator is valid (route_instrument semantics unchanged).
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        for (uint32_t e = 0; e < Synth::max_instr_per_channel; e++) {
            bank.channel_zones[1][e] = { static_cast<uint8_t>(e + 1), 0 };
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_desc_id = 5; // dangling
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].lfo_desc_id = 9; // dangling
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = 0;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = Synth::max_envelope_points + 1;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points                 = 3;
        env.points[2].position         = env.points[1].position; // duplicate position
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points                 = 3;
        env.points[2].position         = 50; // decreasing position
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.sustain_first_point        = 2; // sustain_first > sustain_last
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.envelopes.entries[0].sustain_last_point = Synth::max_envelope_points;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.lfos.entries[0].period_ms = 0;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::no_wave; // not runtime-supported
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::pulse_wave;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::noise_wave;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[0] = static_cast<Synth::WaveType>(99);
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[1] = static_cast<Synth::WaveType>(200);
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_mode = static_cast<Synth::OscMode>(4);
        expect_invalid(bank);
    }
    // layer_count 0 is valid: an all-red zone compiles to zero
    // connected layers, and its missing bits disable the channel, so the
    // runtime never renders it.  Only a count above the layer maximum
    // still refuses.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layer_count = Synth::max_layers + 1;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[0].lfo_depth_source = static_cast<Synth::ModSource>(77);
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[0].lfo_op = static_cast<Synth::SourceOp>(9);
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].targets[0].num_inputs   = 1;
        bank.instruments.entries[0].layers[0].targets[0].inputs[0].op = static_cast<Synth::SourceOp>(7);
        expect_invalid(bank);
    }

    // ---- Effect chains in the bank (schema, validation, codec) ----

    TEST(Synth::get_effect_state_bytes(Synth::EffectType::delay) == 353024); // 88201 floats, 256-aligned

    // A chain with one enabled delay and an LFO-driven param passes validation.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain         = bank.channel_chains[0];
        chain.num_effects                        = 1;
        chain.effects[0].type                    = Synth::EffectType::delay;
        chain.effects[0].enabled                 = true;
        chain.effects[0].bindings[0].base_value  = 250.0f;
        chain.effects[0].bindings[0].lfo_desc_id = 1; // make_valid_bank allocated one LFO
        chain.effects[0].bindings[0].lfo_op      = Synth::SourceOp::add;
        chain.effects[0].bindings[0].lfo_depth   = 100.0f;
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects     = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::num_types; // invalid type
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = Synth::max_chain_effects + 1; // over capacity
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                        = 1;
        bank.channel_chains[0].effects[0].type                    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9; // dangling LFO ref
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.master_chain.num_effects                       = 1;
        bank.master_chain.effects[0].type                   = Synth::EffectType::delay;
        bank.master_chain.effects[0].bindings[0].num_inputs = 1;
        bank.master_chain.effects[0].bindings[0].inputs[0].source =
            Synth::ModSource::velocity; // per-note role: refused on the master chain as on channels
        expect_invalid(bank);
    }

    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                             = 1;
        bank.channel_chains[0].effects[0].type                         = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::velocity; // per-note role
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].base_value = NAN;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                             = 1;
        bank.channel_chains[0].effects[0].type                         = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale  = INFINITY;
        expect_invalid(bank);
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects; // 4 delays: exactly the state budget
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type    = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects;
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type    = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        bank.channel_chains[0].num_effects        = 1; // 5th delay: over the whole-bank state budget
        bank.channel_chains[0].effects[0].type    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true;
        expect_invalid(bank);
    }
    {
        // Exactly 32 modulated effect params pass; one more is rejected.
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 32; channel++) {
            Synth::EffectChainBinding& chain = bank.channel_chains[channel];
            for (uint32_t slot = 0; slot < Synth::max_chain_effects && num_modulated < 32; slot++) {
                chain.num_effects        = static_cast<uint8_t>(slot + 1);
                chain.effects[slot].type = Synth::EffectType::distortion; // 3 params; disabled, so it claims no state
                for (uint32_t param = 0; param < 2 && num_modulated < 32; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 32);
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_chains[15].num_effects                        = Synth::max_chain_effects;
        bank.channel_chains[15].effects[3].type                    = Synth::EffectType::distortion;
        bank.channel_chains[15].effects[3].bindings[1].lfo_desc_id = 1; // 33rd modulated param
        expect_invalid(bank);
    }

    // ---- Effect expansion: preflight/commit transaction over a fake node writer ----
}

static void check_core_inline_1()
{

    // The envelope evaluator holds the start value on unplaced descriptors:
    // adjacent position-0 points never advance, so the interpolation must
    // not divide by a zero duration.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 2;
        env.sustain_first_point       = 0;
        env.sustain_last_point        = 1;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 2.0f;
        env.points[0].position        = 0;
        env.points[0].value           = 0x2000;
        env.points[1].position        = 0;
        env.points[1].value           = 0x4000;
        Synth::EnvelopeState state    = {};
        for (uint32_t i = 0; i < 8; i++) {
            const float value = Synth::eval_envelope(env, &state, true);
            TEST(value == 0x2000 * 2.0f);
        }
    }
}

static void check_core_inline_2()
{

    // Envelope curve edit constraints: the editing helpers accept only
    // requests that leave the descriptor valid, clamp into the free
    // neighbor range, and leave the output untouched on rejection.
    {
        Synth::EnvelopeDescriptor base = {};
        base.num_points                = 4;
        base.sustain_first_point       = 1;
        base.sustain_last_point        = 2;
        base.min_value                 = -1.0f;
        base.min_max_delta             = 2.0f;
        base.points[0]                 = { 0, 0x2000 };
        base.points[1]                 = { 100, 0x8000 };
        base.points[2]                 = { 200, 0x4000 };
        base.points[3]                 = { 300, 0 };

        TEST(Sculptor::env_positions_strictly_increasing(base));

        // A descriptor with duplicate positions (the hand-built-JSON zero
        // run) is a read-only source: every edit rejects and leaves the
        // output untouched.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.points[2].position        = 100;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 2, &out));
            TEST(! Sculptor::env_set_sustain_first(bad, 3, &out));
            TEST(out.num_points == 4 && out.points[1].position == 100);
            TEST(out.points[2].position == 200 && out.sustain_last_point == 2);
        }
        // The validator's leading run of zero positions is equally read-only
        // for editing, and the sustain setters repair nothing there.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.points[1].position        = 0;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 2, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 2, false, &out));
            TEST(! Sculptor::env_remove_point(bad, 3, &out));
            TEST(! Sculptor::env_set_sustain_last(bad, 3, &out));
            TEST(out.num_points == 4 && out.points[3].position == 300);
        }
        // A point count beyond the array bound rejects before any indexing.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.num_points                = Synth::max_envelope_points + 1;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 1, &out));
            TEST(! Sculptor::env_set_sustain_first(bad, 1, &out));
            TEST(out.num_points == 4);
        }
        // Sustain indices outside the point range make move/insert/remove
        // reject, while the sustain setters can repair the descriptor.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.sustain_first_point       = 2;
            bad.sustain_last_point        = 1;
            TEST(Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 2, &out));
            TEST(Sculptor::env_set_sustain_last(bad, 3, &out));
            TEST(out.sustain_first_point == 2 && out.sustain_last_point == 3);
            TEST(out.num_points == 4 && out.points[1].position == 100);
        }

        // Out-of-range point indices reject.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(base, 4, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(base, 4, true, &out));
            TEST(! Sculptor::env_remove_point(base, 4, &out));
            TEST(! Sculptor::env_set_sustain_last(base, 4, &out));
            TEST(out.num_points == 4);
        }

        // Moves clamp strictly between the neighbors (the first point into
        // [0, next-1], the last into [prev+1, 65535]).
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 1, 150, 0xFFFF, &out));
            TEST(out.points[1].position == 150 && out.points[1].value == 0xFFFF);
            TEST(out.num_points == 4 && out.sustain_first_point == 1 && out.sustain_last_point == 2);
            TEST(out.points[0].position == 0 && out.points[2].position == 200);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 1, 250, 0x8000, &out));
            TEST(out.points[1].position == 199);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 2, 10, 0x8000, &out));
            TEST(out.points[2].position == 101);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 0, 5000, 0x1234, &out));
            TEST(out.points[0].position == 99 && out.points[0].value == 0x1234);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 3, 0, 0x1234, &out));
            TEST(out.points[3].position == 201);
            TEST(out.points[3].value == 0x1234);
        }

        // Inserts copy the reference value; interior insertions land at the
        // midpoint of the free range, endpoint insertions just past the
        // endpoint.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(base, 3, true, &out));
            TEST(out.num_points == 5);
            TEST(out.points[4].position == 301 && out.points[4].value == 0);
            TEST(out.points[3].position == 300 && out.points[0].position == 0);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 2);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(base, 1, true, &out));
            TEST(out.num_points == 5);
            TEST(out.points[2].position == 150 && out.points[2].value == 0x8000);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 3);
            // In-place insertion copies the reference value, not whatever
            // the shift has already overwritten.
            Synth::EnvelopeDescriptor in_place = base;
            TEST(Sculptor::env_insert_point(in_place, 1, false, &in_place));
            TEST(in_place.num_points == 5);
            TEST(in_place.points[1].position == 50 && in_place.points[1].value == 0x8000);
            TEST(in_place.points[2].position == 100 && in_place.points[2].value == 0x8000);
        }
        // Inserting before the sustain start shifts the whole span right.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[0].position        = 100;
            src.points[1].position        = 200;
            src.points[2].position        = 300;
            src.points[3].position        = 400;
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(src, 0, false, &out));
            TEST(out.num_points == 5);
            TEST(out.points[0].position == 99);
            TEST(out.points[1].position == 100);
            TEST(out.sustain_first_point == 2 && out.sustain_last_point == 3);
        }
        // Before the first point at tick 0 has no free tick above it.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(base, 0, false, &out));
            TEST(out.num_points == 4);
        }
        // Adjacent interior points leave no free tick on either side.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[1].position        = 99;
            src.points[2].position        = 100;
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 1, true, &out));
            TEST(! Sculptor::env_insert_point(src, 2, false, &out));
            TEST(out.num_points == 4);
        }
        // A last point at the top of the range leaves no room after it.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[3].position        = 65535;
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 3, true, &out));
            TEST(out.num_points == 4);
        }
        // Single-point envelopes can only grow forward.
        {
            Synth::EnvelopeDescriptor src = {};
            src.num_points                = 1;
            src.sustain_first_point       = 0;
            src.sustain_last_point        = 0;
            src.points[0]                 = { 0, 0x8000 };
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 0, false, &out));
            TEST(Sculptor::env_insert_point(src, 0, true, &out));
            TEST(out.num_points == 2);
            TEST(out.points[1].position == 1 && out.points[1].value == 0x8000);
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
        }
        // The eight-point cap rejects further insertions.
        {
            Synth::EnvelopeDescriptor src = base;
            src.num_points                = Synth::max_envelope_points;
            for (uint32_t i = 1; i < Synth::max_envelope_points; ++i) {
                src.points[i].position = static_cast<uint16_t>(i * 100);
            }
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 3, true, &out));
            TEST(! Sculptor::env_insert_point(src, 3, false, &out));
            TEST(out.num_points == 4);
        }

        // Removals reject point 0 and the last remaining point, shift the
        // tail left, zero the vacated slot, and move or clamp the sustain
        // span.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_remove_point(base, 0, &out));
            TEST(out.num_points == 4);
            Synth::EnvelopeDescriptor one = {};
            one.num_points                = 1;
            one.points[0]                 = { 0, 0x8000 };
            TEST(! Sculptor::env_remove_point(one, 0, &out));
            TEST(out.num_points == 4);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(base, 1, &out));
            TEST(out.num_points == 3);
            TEST(out.points[0].position == 0 && out.points[1].position == 200 && out.points[2].position == 300);
            TEST(out.points[3].position == 0 && out.points[3].value == 0);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 1);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(base, 3, &out));
            TEST(out.num_points == 3);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 2);
        }
        // Removing the last point of a two-point envelope clamps the
        // sustain span back onto the survivor.
        {
            Synth::EnvelopeDescriptor src = {};
            src.num_points                = 2;
            src.sustain_first_point       = 1;
            src.sustain_last_point        = 1;
            src.points[0]                 = { 0, 0x2000 };
            src.points[1]                 = { 100, 0x8000 };
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(src, 1, &out));
            TEST(out.num_points == 1);
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
        }

        // Sustain bounds move the other bound when they would cross.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_set_sustain_first(base, 0, &out));
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 2);
            TEST(Sculptor::env_set_sustain_first(base, 3, &out));
            TEST(out.sustain_first_point == 3 && out.sustain_last_point == 3);
            TEST(Sculptor::env_set_sustain_last(base, 0, &out));
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
            TEST(Sculptor::env_set_sustain_last(base, 3, &out));
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 3);
        }

        // Tick conversions: one tick is 256 samples at 44100 Hz; ms round
        // trip through nearest-tick rounding.
        {
            TEST(approx(Sculptor::envelope_ms_per_tick, 256000.0f / 44100.0f, 0.0f));
            TEST(approx(Sculptor::envelope_ticks_to_ms(172), 172.0f * Sculptor::envelope_ms_per_tick, 0.0f));
            TEST(Sculptor::envelope_ms_to_ticks(0.0f) == 0);
            TEST(Sculptor::envelope_ms_to_ticks(-1.0f) == 0);
            TEST(Sculptor::envelope_ms_to_ticks(Sculptor::envelope_ms_per_tick) == 1);
            TEST(Sculptor::envelope_ms_to_ticks(1000.0f) == 172);
            TEST(Sculptor::envelope_ms_to_ticks(1.0e9f) == 65535);
            const uint16_t ticks = Sculptor::envelope_ms_to_ticks(123.0f);
            TEST(approx(Sculptor::envelope_ticks_to_ms(ticks), 123.0f, 3.0f));
        }
    }
}

int main()
{
    test_silence_stdout();
    check_note_name_formats_midi_notes();
    check_core_inline_0();
    check_core_inline_1();
    check_core_inline_2();
    return test_exit_code();
}
