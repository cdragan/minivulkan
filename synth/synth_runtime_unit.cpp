// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

static Synth::MidiEvent make_test_midi_event(Synth::EvType type, uint32_t channel, uint32_t note, uint32_t data)
{
    Synth::MidiEvent event;
    memset(static_cast<void*>(&event), 0, sizeof(event));

    event.event     = type;
    event.channel   = static_cast<uint8_t>(channel);
    event.note      = static_cast<uint8_t>(note);
    event.note_data = static_cast<uint8_t>(data);

    return event;
}

static bool submit_test_midi_event(Synth::EvType type, uint32_t channel, uint32_t note, uint32_t data)
{
    return Synth::submit_external_midi_event(make_test_midi_event(type, channel, note, data));
}

static void submit_test_device_midi_event(Synth::EvType type, uint32_t channel, uint32_t note, uint32_t data)
{
    Synth::submit_device_midi_event(make_test_midi_event(type, channel, note, data));
}

static bool external_note_held(uint32_t channel, uint32_t note)
{
    uint32_t held[4];
    Synth::get_external_held_notes(channel, held);

    return ((held[note >> 5] >> (note & 31u)) & 1u) != 0;
}

static bool external_channel_clear(uint32_t channel)
{
    uint32_t held[4];
    Synth::get_external_held_notes(channel, held);

    return held[0] == 0 && held[1] == 0 && held[2] == 0 && held[3] == 0;
}

static void external_held_notes_regressions()
{
    using Synth::EvType;

    // note_on sets the bit only on its own channel.
    TEST(submit_test_midi_event(EvType::note_on, 2, 60, 100));
    TEST(external_note_held(2, 60));
    TEST(! external_note_held(3, 60));
    TEST(external_channel_clear(3));

    // Multiple notes coexist, also across word boundaries.
    TEST(submit_test_midi_event(EvType::note_on, 2, 64, 90));
    TEST(submit_test_midi_event(EvType::note_on, 2, 127, 90));
    TEST(submit_test_midi_event(EvType::note_on, 2, 0, 90));
    TEST(external_note_held(2, 60));
    TEST(external_note_held(2, 64));
    TEST(external_note_held(2, 127));
    TEST(external_note_held(2, 0));

    // Channels are independent.
    TEST(submit_test_midi_event(EvType::note_on, 5, 60, 100));
    TEST(submit_test_midi_event(EvType::note_off, 5, 60, 0));
    TEST(! external_note_held(5, 60));
    TEST(external_note_held(2, 60));

    // note_off clears only its own note.
    TEST(submit_test_midi_event(EvType::note_off, 2, 60, 64));
    TEST(! external_note_held(2, 60));
    TEST(external_note_held(2, 64));

    // note_on with velocity 0 clears the note.
    TEST(submit_test_midi_event(EvType::note_on, 2, 64, 0));
    TEST(! external_note_held(2, 64));

    // Out-of-range channel and note are ignored.
    submit_test_midi_event(EvType::note_on, Synth::max_channels, 60, 100);
    submit_test_midi_event(EvType::note_on, 2, 200, 100);
    TEST(! external_note_held(2, 60));
    TEST(! external_note_held(2, 200 & 127));

    // A controller event whose controller byte aliases note 60 does not set
    // or clear a bit.  Note 60 held on channel 4 must stay held.
    TEST(submit_test_midi_event(EvType::note_on, 4, 60, 100));
    submit_test_midi_event(EvType::controller, 2, 60, 100);
    submit_test_midi_event(EvType::controller, 4, 60, 0);
    TEST(! external_note_held(2, 60));
    TEST(external_note_held(4, 60));
    TEST(submit_test_midi_event(EvType::note_off, 4, 60, 0));

    // An invalid channel query writes zeros.
    TEST(external_channel_clear(Synth::max_channels));

    // Clear the remaining notes, then check that every channel is clear.
    TEST(submit_test_midi_event(EvType::note_off, 2, 127, 0));
    TEST(submit_test_midi_event(EvType::note_off, 2, 0, 0));
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        TEST(external_channel_clear(channel));
    }

    // Drain the ring.  The stub above swallows the events.
    Synth::pump_live_midi();
}

// Device-entry routing (set_midi_route_target + submit_device_midi_event):
// with no route set the event passes through with its encoded channel, with a
// route set the channel is rewritten before submission.  A route target
// beyond the last channel must not become a channel of its own.
static void device_midi_route_regressions()
{
    using Synth::EvType;

    // Default route (-1) passes the encoded channel through unchanged.
    submit_test_device_midi_event(EvType::note_on, 2, 70, 100);
    TEST(external_note_held(2, 70));
    TEST(! external_note_held(7, 70));
    TEST(external_channel_clear(7));

    // Routing rewrites the channel: a note_on encoded on channel 2 is held
    // on channel 7 only.  Release the passthrough note first so the assert
    // below checks that the routed event no longer touches channel 2.
    submit_test_device_midi_event(EvType::note_off, 2, 70, 0);
    Synth::set_midi_route_target(7);
    submit_test_device_midi_event(EvType::note_on, 2, 70, 100);
    TEST(external_note_held(7, 70));
    TEST(! external_note_held(2, 70));

    // The matching note_off, routed the same way, clears the held bit.
    submit_test_device_midi_event(EvType::note_off, 2, 70, 0);
    TEST(! external_note_held(7, 70));

    // A controller event under routing does not mark notes held.  Note 71 is
    // held on channel 7 through the routed wrapper; a controller on channel
    // 2 whose controller byte aliases note 71 must leave channel 7's bit set
    // and channel 2's bit clear.
    submit_test_device_midi_event(EvType::note_on, 2, 71, 100);
    TEST(external_note_held(7, 71));
    submit_test_device_midi_event(EvType::controller, 2, 71, 100);
    submit_test_device_midi_event(EvType::controller, 2, 71, 0);
    TEST(external_note_held(7, 71));
    TEST(! external_note_held(2, 71));
    submit_test_device_midi_event(EvType::note_off, 2, 71, 0);
    TEST(! external_note_held(7, 71));

    // An out-of-range route target is not applied as a channel, so the note
    // keeps its encoded channel.
    Synth::set_midi_route_target(Synth::max_channels);
    submit_test_device_midi_event(EvType::note_on, 2, 72, 100);
    TEST(external_note_held(2, 72));
    submit_test_device_midi_event(EvType::note_off, 2, 72, 0);
    TEST(! external_note_held(2, 72));

    // Disabling routing restores the passthrough.
    Synth::set_midi_route_target(-1);
    submit_test_device_midi_event(EvType::note_on, 2, 73, 100);
    TEST(external_note_held(2, 73));
    TEST(! external_note_held(7, 73));
    submit_test_device_midi_event(EvType::note_off, 2, 73, 0);

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        TEST(external_channel_clear(channel));
    }

    // Drain the ring.  The stub above swallows the events.
    Synth::pump_live_midi();
}

// Envelope Quick Release regressions: the pure jump-target helpers and their
// state contract, the release-timing sequence mirroring the engine's per-step
// order, and the flag's JSON round-trips plus the envelope-node toggle slot.
// realtime_synth.cpp is not linked into this unit, so the timing test mirrors
// the engine step order (release, then advance, then the termination check
// with the entered-already-parked count). This checks the modeled timing
// policy, not execution of the runtime fade or voice-freeing paths.
static void check_quick_release_regressions()
{
    // The jump location.  The curve is piecewise linear, so the target is a location on
    // a segment of the release slope.  The scan starts at the segment which begins at the
    // sustain end point and compares effective values (min_max_delta may be negative or
    // zero).  Segments with both values above the current one are skipped; a segment with
    // both values below it stops at the segment's first point; otherwise the location is
    // interpolated by the value ratio; the final point is the fallback.  Targets which are
    // not strictly ahead are refused.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 5;
        env.sustain_first_point       = 2;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0x4000 }; // note-off anchor: quarter level
        env.points[1]                 = { 2, 0xC000 };
        env.points[2]                 = { 4, 0x8000 }; // sustain end: above the anchor
        env.points[3]                 = { 6, 0x3000 }; // below the anchor
        env.points[4]                 = { 8, 0 };      // final point

        // The segment from the sustain end to point 3 spans the anchor, and the
        // interpolation lands on point 3.  The reversed seed makes the cleared-direction
        // assertion real.
        Synth::EnvelopeState       state  = { 0, 0, 1 };
        const Synth::EnvelopeState target = Synth::quick_release_target(env, state);
        TEST(target.point == 3 && target.tick == 6 && target.reverse == 0);
        TEST(Synth::apply_quick_release(env, &state));
        TEST(state.point == 3 && state.tick == env.points[3].position);
        TEST(state.reverse == 0); // forward after the jump

        // The target helper is pure: a seeded reverse flag survives it.
        Synth::EnvelopeState       pure_state  = { 0, 0, 1 };
        const Synth::EnvelopeState pure_target = Synth::quick_release_target(env, pure_state);
        TEST(pure_target.point == 3 && pure_target.tick == 6);
        TEST(pure_state.point == 0 && pure_state.tick == 0 && pure_state.reverse == 1);

        // Fallback: every segment after the sustain end sits above the current value, so
        // the loop ends without stopping and the target is the final point.
        Synth::EnvelopeDescriptor rising      = env;
        rising.points[3]                      = { 6, 0xE000 }; // both later segments above the anchor
        rising.points[4]                      = { 8, 0x8000 };
        Synth::EnvelopeState       low_state  = { 0, 0 };
        const Synth::EnvelopeState low_target = Synth::quick_release_target(rising, low_state);
        TEST(low_target.point == 4 && low_target.tick == rising.points[4].position);
        TEST(Synth::apply_quick_release(rising, &low_state));
        TEST(low_state.point == 4 && low_state.tick == rising.points[4].position);

        // A target on the current position is not strictly ahead: apply
        // refuses and leaves the state, including the reverse flag, intact.
        Synth::EnvelopeState       parked_state  = { 6, 3, 1 };
        const Synth::EnvelopeState parked_target = Synth::quick_release_target(env, parked_state);
        TEST(parked_target.point == 3 && parked_target.tick == 6);
        TEST(! Synth::apply_quick_release(env, &parked_state));
        TEST(parked_state.point == 3 && parked_state.tick == 6 && parked_state.reverse == 1);
    }

    // A both-below segment stops at its first point, and a target behind the current
    // position is refused.  The tail is not monotonic: it falls below the sustain level
    // and then rises above it.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 5;
        env.sustain_first_point       = 1;
        env.sustain_last_point        = 1;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65536.0f;
        env.points[0]                 = { 0, 0 };
        env.points[1]                 = { 10, 0x2000 }; // sustain end
        env.points[2]                 = { 20, 0x1000 };
        env.points[3]                 = { 30, 0xC000 };
        env.points[4]                 = { 40, 0 };

        // Current value 0.75 at point 3: the segment (point 1, point 2) lies entirely below
        // it, so the target is that segment's first point, which is behind the position.
        Synth::EnvelopeState       high_state  = { 30, 3, 0 };
        const Synth::EnvelopeState high_target = Synth::quick_release_target(env, high_state);
        TEST(high_target.point == 1 && high_target.tick == 10);
        TEST(! Synth::apply_quick_release(env, &high_state));
        TEST(high_state.point == 3 && high_state.tick == 30);

        // From the attack the same segment is ahead, and the jump lands on its first point.
        Synth::EnvelopeDescriptor peak         = env;
        peak.points[1]                         = { 10, 0xC000 };
        peak.points[2]                         = { 20, 0x4000 };
        peak.points[3]                         = { 30, 0x2000 };
        peak.sustain_first_point               = 2;
        peak.sustain_last_point                = 2;
        Synth::EnvelopeState       peak_state  = { 10, 1, 0 };
        const Synth::EnvelopeState peak_target = Synth::quick_release_target(peak, peak_state);
        TEST(peak_target.point == 2 && peak_target.tick == 20);
        TEST(Synth::apply_quick_release(peak, &peak_state));
        TEST(peak_state.point == 2 && peak_state.tick == 20);
    }

    // The interpolated location, with exact binary values.  The attack reaches 0.25 at
    // tick 5; the release slope from tick 10 (0.5) to tick 30 (0) reaches 0.25 at tick 20.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 3;
        env.sustain_first_point       = 1;
        env.sustain_last_point        = 1;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65536.0f;
        env.points[0]                 = { 0, 0 };
        env.points[1]                 = { 10, 0x8000 };
        env.points[2]                 = { 30, 0 };

        Synth::EnvelopeState state = { 5, 0, 0 };
        TEST(approx(Synth::eval_envelope_at(env, state), 0.25f, 1.0e-6f));
        const Synth::EnvelopeState target = Synth::quick_release_target(env, state);
        TEST(target.point == 1 && target.tick == 20);
        TEST(Synth::apply_quick_release(env, &state));
        TEST(state.point == 1 && state.tick == 20);
        // The curve is continuous across the jump: the jump keeps the level.
        TEST(approx(Synth::eval_envelope_at(env, state), 0.25f, 1.0e-6f));

        // Segments which both lie above the current value are skipped before the stop.  The
        // current 0.125 on the attack lies below the first two release segments (both above
        // it); it is spanned by the last one (0.5 -> 0 over ticks 30..40) at ratio 0.75,
        // tick 37.5, which rounds to tick 38.
        Synth::EnvelopeDescriptor skip  = {};
        skip.num_points                 = 5;
        skip.sustain_first_point        = 1;
        skip.sustain_last_point         = 1;
        skip.min_value                  = 0.0f;
        skip.min_max_delta              = 1.0f / 65536.0f;
        skip.points[0]                  = { 0, 0 };
        skip.points[1]                  = { 10, 0x4000 };
        skip.points[2]                  = { 20, 0xC000 };
        skip.points[3]                  = { 30, 0x8000 };
        skip.points[4]                  = { 40, 0 };
        Synth::EnvelopeState skip_state = { 5, 0, 0 };
        TEST(approx(Synth::eval_envelope_at(skip, skip_state), 0.125f, 1.0e-6f));
        const Synth::EnvelopeState skip_target = Synth::quick_release_target(skip, skip_state);
        TEST(skip_target.point == 3 && skip_target.tick == 38);
        TEST(Synth::apply_quick_release(skip, &skip_state));
        TEST(skip_state.point == 3 && skip_state.tick == 38);
        TEST(approx(Synth::eval_envelope_at(skip, skip_state), 0.1f, 1.0e-3f));
    }

    // Inverted range: min_max_delta may be negative, so the comparison must follow
    // effective values.  Effective values of the points are 0.5, 0.75, 0.25, 0.875.  The
    // sustain-end segment (0.25 -> 0.875) spans the current 0.5 at ratio 0.4 of its 2
    // ticks, which is tick 4.8 (rounded 5).  A raw comparison would see the opposite
    // order.
    {
        Synth::EnvelopeDescriptor inverted         = {};
        inverted.num_points                        = 4;
        inverted.sustain_first_point               = 2;
        inverted.sustain_last_point                = 2;
        inverted.min_value                         = 1.0f;
        inverted.min_max_delta                     = -1.0f / 65535.0f;
        inverted.points[0]                         = { 0, 0x8000 }; // current effective 0.5
        inverted.points[1]                         = { 2, 0x4000 }; // effective 0.75
        inverted.points[2]                         = { 4, 0xC000 }; // effective 0.25
        inverted.points[3]                         = { 6, 0x2000 }; // effective 0.875
        Synth::EnvelopeState       inverted_state  = { 0, 0 };
        const Synth::EnvelopeState inverted_target = Synth::quick_release_target(inverted, inverted_state);
        TEST(inverted_target.point == 2 && inverted_target.tick == 5);
        TEST(Synth::apply_quick_release(inverted, &inverted_state));
        TEST(inverted_state.point == 2 && inverted_state.tick == 5);
    }

    // Flat range: a zero delta makes every effective value equal, so the first segment
    // from the sustain end spans the current value at ratio zero: its first point.
    {
        Synth::EnvelopeDescriptor flat         = {};
        flat.num_points                        = 4;
        flat.sustain_first_point               = 2;
        flat.sustain_last_point                = 2;
        flat.min_value                         = 0.5f;
        flat.min_max_delta                     = 0.0f;
        flat.points[0]                         = { 0, 0x1234 }; // raw values are irrelevant at delta 0
        flat.points[1]                         = { 2, 0x1234 };
        flat.points[2]                         = { 4, 0x1234 };
        flat.points[3]                         = { 6, 0x1234 };
        Synth::EnvelopeState       flat_state  = { 0, 0 };
        const Synth::EnvelopeState flat_target = Synth::quick_release_target(flat, flat_state);
        TEST(flat_target.point == 2 && flat_target.tick == 4);
        TEST(Synth::apply_quick_release(flat, &flat_state));
        TEST(flat_state.point == 2 && flat_state.tick == 4);
    }

    // The sustain end on the final point (no sustain span, or a sustain index out of
    // range): no segment begins there, so the target is the final point.
    {
        Synth::EnvelopeDescriptor env     = {};
        env.num_points                    = 3;
        env.sustain_first_point           = 2;
        env.sustain_last_point            = 2;
        env.min_value                     = 0.0f;
        env.min_max_delta                 = 1.0f / 65536.0f;
        env.points[0]                     = { 0, 0 };
        env.points[1]                     = { 4, 0xFFFF };
        env.points[2]                     = { 8, 0 };
        Synth::EnvelopeState       state  = { 2, 0, 0 };
        const Synth::EnvelopeState target = Synth::quick_release_target(env, state);
        TEST(target.point == 2 && target.tick == 8);

        env.sustain_last_point             = 200; // unvalidated decode: clamps to the final point
        const Synth::EnvelopeState clamped = Synth::quick_release_target(env, state);
        TEST(clamped.point == 2 && clamped.tick == 8);

        Synth::EnvelopeDescriptor  empty        = {};
        const Synth::EnvelopeState empty_target = Synth::quick_release_target(empty, Synth::EnvelopeState{});
        TEST(empty_target.point == 0 && empty_target.tick == 0);
    }

    // The jump's state contract.  After a non-final jump the state
    // parks exactly on the target point, so the next released eval returns
    // the target's value and advances forward; after a final-point jump the
    // released eval stays safely parked (the tick = target position
    // convention must satisfy eval_envelope's parked asserts).
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 5;
        env.sustain_first_point       = 2;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0x4000 };
        env.points[1]                 = { 2, 0xC000 };
        env.points[2]                 = { 4, 0x8000 };
        env.points[3]                 = { 6, 0x3000 };
        env.points[4]                 = { 8, 0 };

        Synth::EnvelopeState state = { 0, 0 };
        TEST(Synth::apply_quick_release(env, &state));
        const float target_value = Synth::eval_envelope(env, &state, false);
        TEST(approx(target_value, 0x3000 / 65535.0f, 0.0001f));
        TEST(state.point == 3 && state.tick == 7); // one tick toward the final point

        Synth::EnvelopeDescriptor rising = env;
        rising.points[3]                 = { 6, 0xE000 };
        rising.points[4]                 = { 8, 0x1000 };
        Synth::EnvelopeState end_state   = { 0, 0 };
        TEST(Synth::apply_quick_release(rising, &end_state));
        const float end_value = Synth::eval_envelope(rising, &end_state, false);
        TEST(approx(end_value, 0x1000 / 65535.0f, 0.0001f));
        TEST(end_state.point == 4 && end_state.tick == 8);
        const float parked_value = Synth::eval_envelope(rising, &end_state, false);
        TEST(approx(parked_value, 0x1000 / 65535.0f, 0.0001f));
        TEST(end_state.point == 4 && end_state.tick == 8);
    }

    // The release timing through the real call order.  The engine per
    // control step, for a released envelope parameter: record whether the
    // state entered the step already parked on the final point, advance the
    // envelope once, bump the entered-parked count when released (reset
    // otherwise), then run the termination check (advance_parameters then
    // update_modulation's volume_termination_due order).
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 4;
        env.sustain_first_point       = 3;
        env.sustain_last_point        = 3; // sustain end is the final point
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };
        env.points[1]                 = { 2, 0x4000 };
        env.points[2]                 = { 4, 0x8000 };
        env.points[3]                 = { 6, 0 };
        const uint32_t final_point    = env.num_points - 1;

        Synth::EnvelopeState state        = { 0, 0 };
        uint16_t             parked_ticks = 0; // mirrors Parameter::last_point_ticks
        auto                 engine_step  = [&](bool held) {
            const bool entry_parked = state.point == final_point && state.tick == env.points[final_point].position;
            Synth::eval_envelope(env, &state, held);
            if (! held) {
                parked_ticks = entry_parked ? static_cast<uint16_t>(parked_ticks + 1) : 0;
            }
            else {
                parked_ticks = 0;
            }
            return parked_ticks >= 2;
        };

        // Quick release off, note-off mid-attack: the envelope plays
        // through; the steps before and including the arrival at the final
        // point are not due, and termination comes after the standard two
        // entered-parked ticks.
        for (uint32_t step = 0; step < 4; step++) {
            TEST(! engine_step(true));
        }
        TEST(state.point == 2 && state.tick == 4);
        TEST(! engine_step(false)); // still approaching the final point: not due
        TEST(state.point == 2 && state.tick == 5);
        TEST(! engine_step(false)); // arrival during release: count 0, not due
        TEST(state.point == 3 && state.tick == 6);
        TEST(! engine_step(false)); // one parked tick: count 1, not due
        TEST(state.point == 3 && state.tick == 6);
        TEST(engine_step(false)); // two parked ticks: count 2, due

        // Release beginning parked: the first released step entered already
        // parked (count 1), the second completes (count 2).
        parked_ticks = 0;
        state        = { 0, 0 };
        for (uint32_t step = 0; step < 16; step++) {
            engine_step(true);
            if (state.point == final_point && state.tick == env.points[final_point].position) {
                break;
            }
        }
        TEST(state.point == final_point && state.tick == env.points[final_point].position);
        TEST(! engine_step(false)); // first released step: count 1, not due
        TEST(engine_step(false));   // second released step: count 2, due

        // Quick release off, ping-pong span reversing at note-off: the
        // released run continues forward past the span end without bouncing
        // or flipping direction.
        {
            Synth::EnvelopeDescriptor bounce      = {};
            bounce.num_points                     = 4;
            bounce.sustain_first_point            = 1;
            bounce.sustain_last_point             = 2;
            bounce.min_value                      = 0.0f;
            bounce.min_max_delta                  = 1.0f / 65535.0f;
            bounce.points[0]                      = { 0, 0 };
            bounce.points[1]                      = { 2, 0x4000 };
            bounce.points[2]                      = { 4, 0x8000 };
            bounce.points[3]                      = { 6, 0 };
            bounce.flags                          = Synth::envelope_ping_pong;
            Synth::EnvelopeState bounce_state     = { 0, 0 };
            auto                 walk_to_span_end = [&bounce](Synth::EnvelopeState* walk_state) {
                while (! (walk_state->point == 2 && walk_state->tick == bounce.points[2].position)) {
                    Synth::eval_envelope(bounce, walk_state, true);
                }
            };
            walk_to_span_end(&bounce_state);
            Synth::eval_envelope(bounce, &bounce_state,
                                 true); // reverses off the span end
            TEST(bounce_state.reverse == 1);
            TEST(Synth::eval_envelope(bounce, &bounce_state, false) != 0.0f); // forward into the span end
            TEST(bounce_state.point == 2 && bounce_state.tick == 4);          // no bounce back into the span
            TEST(Synth::eval_envelope(bounce, &bounce_state, false) != 0.0f);
            TEST(bounce_state.point == 2 && bounce_state.tick == 5); // one tick per step toward the end
            TEST(Synth::eval_envelope(bounce, &bounce_state, false) != 0.0f);
            TEST(bounce_state.point == 3 && bounce_state.tick == 6);
        }
    }

    // The quick_release flag survives a bank round-trip, and a document
    // without the key decodes with the flag off (a zero flag emits no key).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source     = *source_storage;
        Synth::Instrument            instrument = {};
        build_zone_fixture(&source, &instrument);
        source.bank.envelopes.entries[1].flags = Synth::envelope_ping_pong | Synth::envelope_quick_release;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&payload)[Synth::library_payload_max] = payload_storage->elements;
        const uint32_t length                       = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(length > 0);
        TEST(strstr(payload, "\"quick_release\":1") != nullptr);
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(Synth::decode_editor_bank_json(payload, length, &loaded));
        TEST((loaded.bank.envelopes.entries[1].flags & Synth::envelope_quick_release) != 0);
        TEST((loaded.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) != 0);

        source.bank.envelopes.entries[1].flags = Synth::envelope_ping_pong;
        const uint32_t legacy_length           = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(legacy_length > 0);
        TEST(strstr(payload, "\"quick_release\"") == nullptr);
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(Synth::decode_editor_bank_json(payload, legacy_length, &loaded));
        TEST(loaded.bank.envelopes.entries[1].flags == Synth::envelope_ping_pong);
    }

    // The instrument-document format carries the flag the same way.
    {
        Synth::Instrument         instr;
        Synth::EnvelopeDescriptor envs[Synth::instrument_max_envelopes];
        Synth::LFODescriptor      lfos[Synth::instrument_max_lfos];
        uint32_t                  env_count = 0;
        uint32_t                  lfo_count = 0;

        static const char flagged_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                          "\"volume\":{\"base\":1,\"envelope\":{"
                                          "\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,"
                                          "\"level\":1}],"
                                          "\"sustain_start\":1,\"sustain_end\":1,\"ping_pong\":1,\"quick_"
                                          "release\":1}}}]}";
        TEST(Synth::decode_instrument_json(flagged_doc,
                                           sizeof(flagged_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(env_count == 1);
        TEST((envs[0].flags & Synth::envelope_quick_release) != 0);
        TEST((envs[0].flags & Synth::envelope_ping_pong) != 0);

        // Round-trip through the encoder keeps both bits.
        const std::unique_ptr<Synth::InstrumentBank> desc_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       desc_bank         = *desc_bank_storage;
        memset(static_cast<void*>(&desc_bank), 0, sizeof(desc_bank));
        desc_bank.envelopes.num_allocated = env_count;
        for (uint32_t i = 0; i < env_count; i++) {
            desc_bank.envelopes.entries[i] = envs[i];
        }
        char           doc_text[8192] = {}; // static: 8 KB is too much stack
        const uint32_t doc_len        = Synth::encode_instrument_json(doc_text, sizeof(doc_text), &instr, &desc_bank);
        TEST(doc_len > 0);
        TEST(strstr(doc_text, "\"quick_release\":1") != nullptr);
        TEST(Synth::decode_instrument_json(doc_text, doc_len, &instr, envs, &env_count, lfos, &lfo_count));
        TEST((envs[0].flags & Synth::envelope_quick_release) != 0);
        TEST((envs[0].flags & Synth::envelope_ping_pong) != 0);

        // An envelope without either flag key decodes with flags 0.
        static const char unflagged_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                            "\"volume\":{\"base\":1,\"envelope\":{"
                                            "\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,"
                                            "\"level\":1}],"
                                            "\"sustain_start\":1,\"sustain_end\":1}}}]}";
        TEST(Synth::decode_instrument_json(unflagged_doc,
                                           sizeof(unflagged_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(env_count == 1);
        TEST(envs[0].flags == 0);
    }

    // The Quick Release toggle slot on the envelope node flips the
    // flag bit in the bank descriptor, mirroring the Ping-Pong toggle, and
    // leaves the ping-pong bit alone.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        source.bank.envelopes.entries[1].sustain_first_point = 1;
        source.bank.envelopes.entries[1].sustain_last_point  = 2;
        source.bank.envelopes.entries[1].flags = Synth::envelope_ping_pong | Synth::envelope_quick_release;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        const int32_t env_instance = find_detached_by_desc(mapping, 1, 2);
        TEST(env_instance >= 0);
        const uint32_t env_node = mapping.detached[env_instance].node_idx;
        TEST(env_node != Sculptor::pool_no_slot);
        uint32_t quick_release_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, env_node, "Quick Release", &quick_release_slot));
        TEST(graph.node(env_node).slots.entries[quick_release_slot].value.toggle == true);

        Sculptor::PropertyValue toggle_edit = {};
        toggle_edit.toggle                  = false;
        graph.set_slot_value(env_node, quick_release_slot, toggle_edit);
        Sculptor::GraphChange toggle_change = {};
        toggle_change.kind                  = Sculptor::ChangeKind::value_changed;
        toggle_change.node_idx              = env_node;
        toggle_change.slot_idx              = quick_release_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_quick_release) == 0);
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) != 0); // sibling bit survives

        toggle_edit.toggle = true;
        graph.set_slot_value(env_node, quick_release_slot, toggle_edit);
        toggle_change.node_idx = env_node;
        toggle_change.slot_idx = quick_release_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_quick_release) != 0);

        // Toggling Ping-Pong leaves the quick-release bit alone too.
        uint32_t ping_pong_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, env_node, "Ping-Pong", &ping_pong_slot));
        toggle_edit.toggle     = false;
        toggle_change.slot_idx = ping_pong_slot;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_quick_release) != 0);
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) == 0);
        toggle_edit.toggle = true;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_quick_release) != 0);
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) != 0);
    }
}

// The note-off rule (Synth::envelope_note_off), tested on the real envelope code.
// With Quick Release off, only a backward ping-pong direction changes (it turns
// forward).  With Quick Release on, the envelope also jumps to its release target from
// any position.
static void check_envelope_note_off_regressions()
{
    auto held_entry = [](const Synth::EnvelopeDescriptor& env, const Synth::EnvelopeState& state, bool releasing) {
        return ! releasing && state.point >= env.sustain_first_point && state.point <= env.sustain_last_point;
    };

    // C3.1: Quick Release off, three points without a sustain span (0 -> 1 over four
    // ticks -> 0 over four ticks).  A quick tap rides the full attack and the full fall.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 3;
        env.sustain_first_point       = 2; // the decoder default is the final point
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };
        env.points[1]                 = { 4, 0xFFFF };
        env.points[2]                 = { 8, 0 };

        // The tap lands mid-attack (a), after the peak (c), and at the very start.
        const Synth::EnvelopeState taps[3] = { { 2, 0, 0 }, { 6, 1, 0 }, { 0, 0, 0 } };
        for (uint32_t tap_idx = 0; tap_idx < 3; tap_idx++) {
            Synth::EnvelopeState state = { 0, 0, 0 };
            float                peak  = 0.0f;
            // Held steps up to the tap position.  The decay never starts, because the
            // state stays before the sustain span.
            while (state.point != taps[tap_idx].point || state.tick != taps[tap_idx].tick) {
                TEST(! held_entry(env, state, false));
                const float value = Synth::eval_envelope(env, &state, true);
                if (value > peak) {
                    peak = value;
                }
            }
            const Synth::EnvelopeState before = state;
            TEST(! Synth::envelope_note_off(env, &state)); // flags 0: no jump
            TEST(state.point == before.point && state.tick == before.tick && state.reverse == 0);

            // Released steps: one tick at most per step, never a held entry, and the
            // value reaches 1 and then 0 at the parked final point.
            uint32_t parked_steps = 0;
            uint32_t steps        = 0;
            while (parked_steps < 3 && steps < 64) {
                TEST(! held_entry(env, state, true));
                const uint32_t previous_tick  = state.tick;
                const bool     entered_parked = state.point == 2 && state.tick == env.points[2].position;
                const float    value          = Synth::eval_envelope(env, &state, false);
                if (value > peak) {
                    peak = value;
                }
                TEST(state.tick - previous_tick <= 1);
                // T3.1d: the termination count of the production rule (two entered-parked
                // steps) starts only after the envelope parks on the final point.
                parked_steps = entered_parked ? parked_steps + 1 : 0;
                TEST(parked_steps == 0 || (state.point == 2 && state.tick == 8));
                steps++;
            }
            TEST(approx(peak, 1.0f, 0.0001f)); // the full attack was played
            TEST(state.point == 2 && state.tick == 8);
            TEST(approx(Synth::eval_envelope_at(env, state), 0.0f, 0.0001f)); // the full fall was played
            TEST(parked_steps >= 2);                                          // termination fires only after the park
        }
    }

    // T3.1e: Quick Release off never moves the position for any envelope shape and any
    // note-off position.
    {
        Synth::EnvelopeDescriptor env           = {};
        env.num_points                          = 5;
        env.sustain_first_point                 = 2;
        env.sustain_last_point                  = 2;
        env.min_value                           = 0.0f;
        env.min_max_delta                       = 1.0f / 65535.0f;
        env.points[0]                           = { 0, 0x4000 };
        env.points[1]                           = { 2, 0xC000 };
        env.points[2]                           = { 4, 0x8000 };
        env.points[3]                           = { 6, 0x3000 };
        env.points[4]                           = { 8, 0 };
        const Synth::EnvelopeState positions[6] = { { 0, 0, 1 }, { 1, 0, 1 }, { 3, 1, 1 },
                                                    { 4, 2, 1 }, { 5, 2, 1 }, { 7, 3, 1 } };
        for (const Synth::EnvelopeState& position : positions) {
            Synth::EnvelopeState state = position;
            TEST(! Synth::envelope_note_off(env, &state));
            TEST(state.point == position.point && state.tick == position.tick);
            TEST(state.reverse == 0);
        }
    }

    // C3.2: a ping-pong span traversed backwards turns forward at note-off.  The flip is
    // unconditional on Quick Release, and the position is unchanged.
    for (uint32_t quick_release = 0; quick_release < 2; quick_release++) {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 4;
        env.sustain_first_point       = 1;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };
        env.points[1]                 = { 2, 0x4000 };
        env.points[2]                 = { 4, 0x8000 };
        env.points[3]                 = { 6, 0 };
        env.flags = Synth::envelope_ping_pong |
                    (quick_release ? static_cast<uint8_t>(Synth::envelope_quick_release) : static_cast<uint8_t>(0));

        Synth::EnvelopeState state = { 0, 0, 0 };
        while (! (state.point == 2 && state.tick == env.points[2].position)) {
            Synth::eval_envelope(env, &state, true);
        }
        Synth::eval_envelope(env, &state, true); // reverses off the span end
        TEST(state.reverse == 1);
        TEST(state.point == 1 && state.tick == 3);

        const bool moved = Synth::envelope_note_off(env, &state);
        TEST(state.reverse == 0);
        if (quick_release) {
            // The current value 0.375 lies on the release segment (0.5 -> 0 over ticks
            // 4..6) at ratio 0.25, which rounds to tick 5.
            TEST(moved);
            TEST(state.point == 2 && state.tick == 5);
        }
        else {
            TEST(! moved);
            TEST(state.point == 1 && state.tick == 3); // position unchanged
            // The released run goes forward one tick per step and never bounces back.
            uint32_t previous_position = static_cast<uint32_t>(state.tick);
            while (! (state.point == 3 && state.tick == 6)) {
                Synth::eval_envelope(env, &state, false);
                TEST(state.reverse == 0);
                TEST(static_cast<uint32_t>(state.tick) == previous_position + 1);
                previous_position = state.tick;
            }
        }
        // Either way the released run ends on the parked final point.
        while (! (state.point == 3 && state.tick == 6)) {
            Synth::eval_envelope(env, &state, false);
        }
        TEST(approx(Synth::eval_envelope(env, &state, false), 0.0f, 0.0001f));

        // A forward state at note-off stays forward.
        Synth::EnvelopeState forward = { 1, 1, 0 };
        Synth::envelope_note_off(env, &forward);
        TEST(forward.reverse == 0);
    }

    // The five-point fixture of the jump-target tests, with Quick Release on.
    Synth::EnvelopeDescriptor quick = {};
    quick.num_points                = 5;
    quick.sustain_first_point       = 2;
    quick.sustain_last_point        = 2;
    quick.min_value                 = 0.0f;
    quick.min_max_delta             = 1.0f / 65535.0f;
    quick.flags                     = Synth::envelope_quick_release;
    quick.points[0]                 = { 0, 0x4000 };
    quick.points[1]                 = { 2, 0xC000 };
    quick.points[2]                 = { 4, 0x8000 };
    quick.points[3]                 = { 6, 0x3000 };
    quick.points[4]                 = { 8, 0 };

    // T4.4: a refused jump still keeps the cleared direction.  The state is already
    // parked on the target point, so apply_quick_release refuses.
    {
        Synth::EnvelopeState parked = { 6, 3, 1 };
        TEST(! Synth::apply_quick_release(quick, &parked));
        TEST(parked.reverse == 1);
        TEST(! Synth::envelope_note_off(quick, &parked));
        TEST(parked.point == 3 && parked.tick == 6);
        TEST(parked.reverse == 0);
    }

    // T4.5: a stale reverse flag on an envelope without ping-pong is cleared.
    {
        Synth::EnvelopeDescriptor plain = quick;
        plain.flags                     = 0;
        Synth::EnvelopeState stale      = { 3, 1, 1 };
        TEST(! Synth::envelope_note_off(plain, &stale));
        TEST(stale.point == 1 && stale.tick == 3 && stale.reverse == 0);
    }

    // C3.3: Quick Release on jumps from any position to a location on the slope after the
    // last sustain point.  Expected locations (fixture values 0x4000, 0xC000, 0x8000,
    // 0x3000, 0 at ticks 0, 2, 4, 6, 8; the scan starts at the segment from point 2):
    // - start of the attack, level 0x4000: the segment 0x8000 -> 0x3000 spans it at ratio 0.8,
    //   tick 5.6 rounds to 6, which is point 3
    // - tick 1, level 0x8000: equals the span end value, the location is point 2
    // - tick 3, level 0xA000: the segment is entirely below, the stop is its first point
    // - on the span end: the target is the current position, so no move
    // - tick 5 of the segment, level 0x5800: ratio 0.5, tick 5, the current location
    // - tick 7 of the tail, level 0x1800: the first segment is skipped (both above), the
    //   second spans it at ratio 0.5, tick 7, the current location
    {
        struct JumpCase {
            Synth::EnvelopeState start;
            uint32_t             point;
            uint32_t             tick;
            bool                 moves;
        };

        // EnvelopeState is { tick, point, reverse }.
        const JumpCase cases[] = {
            { { 0, 0, 0 }, 3, 6, true },  { { 1, 0, 0 }, 2, 4, true },  { { 3, 1, 0 }, 2, 4, true },
            { { 4, 2, 0 }, 2, 4, false }, { { 5, 2, 0 }, 2, 5, false }, { { 7, 3, 0 }, 3, 7, false },
            { { 3, 1, 1 }, 2, 4, true }, // backward ping-pong position
        };
        for (const JumpCase& jump_case : cases) {
            const Synth::EnvelopeState target = Synth::quick_release_target(quick, jump_case.start);
            TEST(target.point == jump_case.point && target.tick == jump_case.tick);

            Synth::EnvelopeState state        = jump_case.start;
            const float          level_before = Synth::eval_envelope_at(quick, state);
            const bool           moved        = Synth::envelope_note_off(quick, &state);
            TEST(state.reverse == 0);
            TEST(moved == jump_case.moves);
            if (jump_case.moves) {
                TEST(state.point == jump_case.point && state.tick == jump_case.tick);
                // The jump never raises the level.
                TEST(Synth::eval_envelope_at(quick, state) <= level_before + 0.0001f);
                // One released eval returns the level of the location and advances forward.
                const float expected_value = Synth::eval_envelope_at(quick, state);
                TEST(approx(Synth::eval_envelope(quick, &state, false), expected_value, 0.0001f));
            }
            else {
                TEST(state.point == jump_case.start.point && state.tick == jump_case.start.tick);
            }

            // T6.5: the same position with the flag clear never jumps.
            Synth::EnvelopeDescriptor no_jump = quick;
            no_jump.flags                     = 0;
            Synth::EnvelopeState untouched    = jump_case.start;
            TEST(! Synth::envelope_note_off(no_jump, &untouched));
            TEST(untouched.point == jump_case.start.point && untouched.tick == jump_case.start.tick);
            TEST(untouched.reverse == 0);
        }
    }

    // T6.4: the final point is the fallback location when every segment after the sustain
    // end lies above the current value (mid-attack); late in the tail the location is
    // behind the position, so nothing moves.
    {
        Synth::EnvelopeDescriptor rising = quick;
        rising.points[0]                 = { 0, 0x1000 };
        rising.points[2]                 = { 4, 0x8000 };
        rising.points[3]                 = { 6, 0xE000 };
        rising.points[4]                 = { 8, 0x8000 };

        Synth::EnvelopeState state = { 1, 0, 0 };
        TEST(Synth::envelope_note_off(rising, &state));
        TEST(state.point == 4 && state.tick == 8 && state.reverse == 0);

        Synth::EnvelopeState late = { 7, 3, 0 };
        TEST(! Synth::envelope_note_off(rising, &late));
        TEST(late.point == 3 && late.tick == 7 && late.reverse == 0);
    }

    // T6.6: the inverted and flat ranges pick the same target as apply_quick_release.
    {
        Synth::EnvelopeDescriptor inverted  = {};
        inverted.num_points                 = 4;
        inverted.sustain_first_point        = 2;
        inverted.sustain_last_point         = 2;
        inverted.min_value                  = 1.0f;
        inverted.min_max_delta              = -1.0f / 65535.0f;
        inverted.flags                      = Synth::envelope_quick_release;
        inverted.points[0]                  = { 0, 0x8000 };
        inverted.points[1]                  = { 2, 0x4000 };
        inverted.points[2]                  = { 4, 0xC000 };
        inverted.points[3]                  = { 6, 0x2000 };
        Synth::EnvelopeState note_off_state = { 0, 0, 0 };
        Synth::EnvelopeState apply_state    = { 0, 0, 0 };
        TEST(Synth::envelope_note_off(inverted, &note_off_state));
        TEST(Synth::apply_quick_release(inverted, &apply_state));
        TEST(note_off_state.point == 2 && note_off_state.tick == 5);
        TEST(note_off_state.point == apply_state.point && note_off_state.tick == apply_state.tick);

        Synth::EnvelopeDescriptor flat     = inverted;
        flat.min_value                     = 0.5f;
        flat.min_max_delta                 = 0.0f;
        Synth::EnvelopeState flat_note_off = { 0, 0, 0 };
        Synth::EnvelopeState flat_apply    = { 0, 0, 0 };
        TEST(Synth::envelope_note_off(flat, &flat_note_off));
        TEST(Synth::apply_quick_release(flat, &flat_apply));
        TEST(flat_note_off.point == 2 && flat_note_off.tick == 4);
        TEST(flat_note_off.point == flat_apply.point && flat_note_off.tick == flat_apply.tick);
    }
}

// The in-place restart seed (EnvelopeState::old_value).  A restarted envelope continues from
// the level of the previous envelope instead of dropping to its first point.  Zero means no
// seed, so a fresh note keeps the exact legacy evaluation.
static void check_envelope_restart_seed_regressions()
{
    // Three points, no sustain span: 0 -> 1 over four ticks -> 0 over four ticks.
    Synth::EnvelopeDescriptor env = {};
    env.num_points                = 3;
    env.sustain_first_point       = 2;
    env.sustain_last_point        = 2;
    env.min_value                 = 0.0f;
    env.min_max_delta             = 1.0f / 65536.0f;
    env.points[0]                 = { 0, 0 };
    env.points[1]                 = { 4, 0x8000 };
    env.points[2]                 = { 8, 0 };

    // A fresh note (old_value zero) evaluates exactly as the legacy code did.  The expected
    // values and positions are hand-derived literals, not produced by the code under test: the
    // value at tick t is the integer interpolation of the point values (0, 0x8000, 0) scaled
    // by 1/65536, and the state advances one tick per step until it parks on the sustain
    // point.
    {
        const float    expected_value[10] = { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.375f, 0.25f, 0.125f, 0.0f, 0.0f };
        const uint16_t expected_point[10] = { 0, 0, 0, 1, 1, 1, 1, 2, 2, 2 };
        const uint16_t expected_tick[10]  = { 1, 2, 3, 4, 5, 6, 7, 8, 8, 8 };

        Synth::EnvelopeState fresh = {};
        TEST(Synth::eval_envelope_at(env, fresh) == 0.0f);
        for (uint32_t step = 0; step < 10; step++) {
            const float value = Synth::eval_envelope(env, &fresh, true);
            TEST(value == expected_value[step]);
            TEST(fresh.point == expected_point[step]);
            TEST(fresh.tick == expected_tick[step]);
            TEST(fresh.old_value == 0.0f);
        }

        // An envelope which holds on its first point reports the stored value, not a seed.
        Synth::EnvelopeDescriptor hold  = env;
        hold.sustain_first_point        = 0;
        hold.sustain_last_point         = 0;
        hold.points[0]                  = { 0, 0x4000 };
        Synth::EnvelopeState hold_state = {};
        TEST(Synth::eval_envelope_at(hold, hold_state) == 0.25f);
        TEST(Synth::eval_envelope(hold, &hold_state, true) == 0.25f);
        TEST(hold_state.point == 0 && hold_state.tick == 0);
    }

    // A seeded restart reports the carried level at its start and ramps from it to the
    // next point (0.5 at tick 4) through the first segment.
    {
        Synth::EnvelopeState seeded = {};
        seeded.old_value            = 0.25f;
        TEST(approx(Synth::eval_envelope_at(env, seeded), 0.25f, 1.0e-6f));

        const float expected[5] = { 0.25f, 0.3125f, 0.375f, 0.4375f, 0.5f };
        for (uint32_t step = 0; step < 5; step++) {
            const float value = Synth::eval_envelope(env, &seeded, true);
            TEST(approx(value, expected[step], 1.0e-6f));
        }
        // Past the first segment the seed is no longer consulted: the state behaves as without
        // one.  The stored seed itself is left alone.
        TEST(seeded.point == 1 && seeded.tick == 5);
        TEST(seeded.old_value == 0.25f);
        TEST(approx(Synth::eval_envelope_at(env, seeded), 0.5f - 0.5f / 4.0f, 1.0e-3f));
    }

    // The pair the engine step writes: value at the restart level, prev_value one tick
    // ahead.  Both approximate the carried level.
    {
        Synth::EnvelopeState seeded = {};
        seeded.old_value            = 0.25f;
        const float value           = Synth::eval_envelope(env, &seeded, true);
        const float prev_value      = Synth::eval_envelope_at(env, seeded);
        TEST(approx(value, 0.25f, 1.0e-6f));
        TEST(approx(prev_value, 0.3125f, 1.0e-6f));
        TEST(prev_value - value < 0.07f);
    }

    // The dest stays continuous across a takeover: the configure-time onset value and the
    // first post-restart advance both approximate base + scale * carried level.
    {
        Synth::ParamDescriptor descs[3]  = {};
        Synth::Parameter       params[3] = {};
        descs[1].kind                    = Synth::ParamKind::envelope;
        descs[1].envelope                = env;
        params[1].envelope.old_value     = 0.25f;
        Synth::configure_plain(&descs[2], 0.125f, 1, 2.0f, 0, Synth::SourceOp::add, nullptr, 0);

        TEST(approx(Synth::param_onset_value(params, descs, 2), 0.125f + 2.0f * 0.25f, 1.0e-6f));

        // One engine step: the pre-pass advances the envelope, then the dest propagates.
        params[1].value      = Synth::eval_envelope(descs[1].envelope, &params[1].envelope, true);
        params[1].prev_value = Synth::eval_envelope_at(descs[1].envelope, params[1].envelope);
        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[2].value, 0.125f + 2.0f * 0.25f, 0.2f));
        TEST(params[2].value > 0.125f + 2.0f * 0.2f); // far from the first point's zero

        // A fresh note starts at the first point's value.
        Synth::Parameter fresh[3] = {};
        TEST(approx(Synth::param_onset_value(fresh, descs, 2), 0.125f, 1.0e-6f));
    }

    // The carried level applies to every envelope at its start, including one which sustains
    // on its first point.  Release such an envelope into its tail, then restart it in place
    // with the captured level: the restart reports that level, not the stored first point.
    {
        Synth::EnvelopeDescriptor hold = env;
        hold.sustain_first_point       = 0;
        hold.sustain_last_point        = 0;
        hold.points[0]                 = { 0, 0x8000 };
        hold.points[1]                 = { 4, 0x4000 };
        hold.points[2]                 = { 8, 0 };

        Synth::EnvelopeState state = {};
        TEST(Synth::eval_envelope_at(hold, state) == 0.5f);
        // Run the released tail (sustain off) to tick 6 of the second segment.
        for (uint32_t step = 0; step < 6; step++) {
            Synth::eval_envelope(hold, &state, false);
        }
        TEST(state.point == 1 && state.tick == 6);
        const float captured = Synth::eval_envelope_at(hold, state);
        TEST(captured == 0.125f);

        // Takeover: the state restarts at the envelope start with the captured level.
        Synth::EnvelopeState restarted = {};
        restarted.old_value            = captured;
        TEST(Synth::eval_envelope_at(hold, restarted) == captured);
        TEST(Synth::eval_envelope(hold, &restarted, true) == captured);
        TEST(restarted.point == 0 && restarted.tick == 0);
    }

    // A quick-release jump drops the seed.
    {
        Synth::EnvelopeDescriptor quick = env;
        quick.flags                     = Synth::envelope_quick_release;
        Synth::EnvelopeState state      = {};
        state.old_value                 = 0.25f;
        Synth::EnvelopeState jumped     = state;
        jumped.tick                     = 2;
        TEST(Synth::envelope_note_off(quick, &jumped));
        TEST(jumped.old_value == 0.0f);
    }
}

struct NoteOnSimOsc {
    bool    used;
    uint8_t voice_id;
    uint8_t layer_idx;
    uint8_t midi_channel;
    uint8_t note;
    float   volume;
    float   old_volume;
    float   phase;
};

struct NoteOnSimVoice {
    bool    active;
    bool    note_finished;
    uint8_t channel;
    uint8_t note;
    uint8_t osc_count;
    uint8_t osc_ids[4];
};

struct NoteOnSimState {
    NoteOnSimVoice voices[4]; // slot 0 is the sentinel: 3 usable voices
    NoteOnSimOsc   oscs[8];   // slot 0 is the sentinel: 7 usable oscillators
    uint8_t        note_to_voice[16][128];
};

static bool note_on_sim(NoteOnSimState* state, uint32_t channel, uint32_t note, uint32_t layer_count)
{
    const uint32_t existing_voice = state->note_to_voice[channel][note];

    uint32_t voice_idx = 0;
    for (uint32_t idx = 1; idx < 4 && ! voice_idx; idx++) {
        if (! state->voices[idx].active) {
            voice_idx = idx;
        }
    }
    if (! voice_idx) {
        return false;
    }

    NoteOnSimVoice& voice = state->voices[voice_idx];
    voice.active          = true;
    voice.note_finished   = false;
    voice.channel         = static_cast<uint8_t>(channel);
    voice.note            = static_cast<uint8_t>(note);

    uint32_t allocated = 0;
    for (uint32_t idx = 1; idx < 8 && allocated < layer_count; idx++) {
        if (! state->oscs[idx].used) {
            state->oscs[idx].used     = true;
            state->oscs[idx].voice_id = static_cast<uint8_t>(voice_idx);
            voice.osc_ids[allocated]  = static_cast<uint8_t>(idx);
            allocated++;
        }
    }
    if (allocated < layer_count) {
        for (uint32_t layer_idx = 0; layer_idx < allocated; layer_idx++) {
            state->oscs[voice.osc_ids[layer_idx]].used     = false;
            state->oscs[voice.osc_ids[layer_idx]].voice_id = 0;
            voice.osc_ids[layer_idx]                       = 0;
        }
        voice.active    = false;
        voice.osc_count = 0;
        return false;
    }
    voice.osc_count = static_cast<uint8_t>(layer_count);
    for (uint32_t layer_idx = 0; layer_idx < layer_count; layer_idx++) {
        NoteOnSimOsc& osc = state->oscs[voice.osc_ids[layer_idx]];
        osc.layer_idx     = static_cast<uint8_t>(layer_idx);
        osc.midi_channel  = static_cast<uint8_t>(channel);
        osc.note          = static_cast<uint8_t>(note);
        osc.old_volume    = 0.0f;
        osc.phase         = 0.0f;
        osc.volume        = 0.0f;
    }
    if (existing_voice) {
        state->voices[existing_voice].note_finished = true;
    }
    state->note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);
    return true;
}

// Gives every live oscillator distinct nonzero state, so any change shows up.
static void note_on_sim_mark(NoteOnSimState* state)
{
    for (uint32_t idx = 1; idx < 8; idx++) {
        if (state->oscs[idx].used) {
            state->oscs[idx].volume     = 0.1f * static_cast<float>(idx);
            state->oscs[idx].old_volume = 0.05f * static_cast<float>(idx);
            state->oscs[idx].phase      = 0.01f * static_cast<float>(idx + 1);
        }
    }
}

static void check_note_on_exhaustion_regressions()
{
    // T2.1: the voice pool is full.  A new note and a retrigger are both dropped, and
    // the whole state stays byte-identical.
    {
        NoteOnSimState state;
        memset(static_cast<void*>(&state), 0, sizeof(state));
        TEST(note_on_sim(&state, 0, 60, 1));
        TEST(note_on_sim(&state, 0, 62, 1));
        TEST(note_on_sim(&state, 3, 64, 1));
        note_on_sim_mark(&state);

        NoteOnSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &state, sizeof(state));

        TEST(! note_on_sim(&state, 0, 65, 1)); // a different new note
        TEST(memcmp(&state, &snapshot, sizeof(state)) == 0);

        const uint32_t old_voice = state.note_to_voice[0][60];
        TEST(old_voice != 0);
        TEST(! note_on_sim(&state, 0, 60, 1)); // a retrigger of a playing note
        TEST(memcmp(&state, &snapshot, sizeof(state)) == 0);
        TEST(state.note_to_voice[0][60] == old_voice);
        TEST(! state.voices[old_voice].note_finished);
        TEST(state.oscs[state.voices[old_voice].osc_ids[0]].volume > 0.0f);
    }

    // T2.2: the oscillator pool runs out for a three-layer note while a voice is free.
    // The playing voices, their oscillators and the mappings stay byte-identical.
    for (uint32_t retrigger = 0; retrigger < 2; retrigger++) {
        NoteOnSimState state;
        memset(static_cast<void*>(&state), 0, sizeof(state));
        TEST(note_on_sim(&state, 0, 60, 3));
        TEST(note_on_sim(&state, 0, 62, 2)); // five of seven oscillators are in use
        note_on_sim_mark(&state);

        NoteOnSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &state, sizeof(state));

        const uint32_t new_note = retrigger ? 60 : 64;
        TEST(! note_on_sim(&state, 0, new_note, 3));

        TEST(memcmp(&state.voices[1], &snapshot.voices[1], 2 * sizeof(state.voices[1])) == 0);
        TEST(memcmp(state.oscs, snapshot.oscs, sizeof(state.oscs)) == 0);
        TEST(memcmp(state.note_to_voice, snapshot.note_to_voice, sizeof(state.note_to_voice)) == 0);

        // The touched free voice returns to the pool.  The two partial claims are free.
        TEST(! state.voices[3].active && state.voices[3].osc_count == 0);
        for (uint32_t idx = 6; idx < 8; idx++) {
            TEST(! state.oscs[idx].used && state.oscs[idx].voice_id == 0);
        }
        for (uint32_t layer_idx = 0; layer_idx < 3; layer_idx++) {
            TEST(state.voices[3].osc_ids[layer_idx] == 0);
        }

        // The old note is still mapped and sounding.
        const uint32_t old_voice = state.note_to_voice[0][new_note];
        if (retrigger) {
            TEST(old_voice == 1);
            TEST(! state.voices[old_voice].note_finished);
            TEST(state.voices[old_voice].active);
            TEST(state.oscs[state.voices[old_voice].osc_ids[0]].volume > 0.0f);
        }
        else {
            TEST(old_voice == 0);
        }
    }

    // T2.3: on success the retrigger gets a distinct voice.  The old voice is finished
    // only now, the mapping moves to the new voice, and the new oscillators start fresh.
    {
        NoteOnSimState state;
        memset(static_cast<void*>(&state), 0, sizeof(state));
        TEST(note_on_sim(&state, 0, 60, 2));
        note_on_sim_mark(&state);
        const uint32_t old_voice = state.note_to_voice[0][60];
        TEST(old_voice == 1);

        TEST(note_on_sim(&state, 0, 60, 2));
        const uint32_t new_voice = state.note_to_voice[0][60];
        TEST(new_voice != 0 && new_voice != old_voice);
        TEST(state.voices[old_voice].note_finished);
        TEST(state.voices[old_voice].active);
        TEST(! state.voices[new_voice].note_finished);
        for (uint32_t layer_idx = 0; layer_idx < 2; layer_idx++) {
            const NoteOnSimOsc& osc = state.oscs[state.voices[new_voice].osc_ids[layer_idx]];
            TEST(osc.old_volume == 0.0f && osc.phase == 0.0f);
            TEST(osc.voice_id == new_voice);
        }
    }
}

static void check_voice_mode_codec()
{
    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;

    // Absent key: legacy documents decode to multiple, even into a pre-filled instrument.
    static const char defaults_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}]}";
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    decoded.instr.voice_mode = 2;
    TEST(decode_clipboard(&decoded, defaults_doc, max_portable_records));
    TEST(decoded.instr.voice_mode == 0);

    // Clipboard form: each spelled name decodes, and the encoder spells it back.
    static const char* const                     names[3]     = { "multiple", "reuse per-note", "single" };
    const std::unique_ptr<Synth::InstrumentBank> desc_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       desc_bank    = *desc_storage;
    memset(static_cast<void*>(&desc_bank), 0, sizeof(desc_bank));
    static char text[8192];
    for (uint32_t mode = 0; mode < 3; mode++) {
        char doc[256];
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"voices\":\"%s\",\"layers\":[{\"wave_a\":\"sine\"}]}",
                 names[mode]);
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(decode_clipboard(&decoded, doc, max_portable_records));
        TEST(decoded.instr.voice_mode == mode);

        const uint32_t len = Synth::encode_instrument_json(text, sizeof(text), &decoded.instr, &desc_bank);
        TEST(len > 0);
        if (mode == 0) {
            TEST(strstr(text, "\"voices\"") == nullptr); // the default is omitted
        }
        else {
            char expected[64];
            snprintf(expected, sizeof(expected), "\"voices\":\"%s\"", names[mode]);
            TEST(strstr(text, expected) != nullptr);
        }
        const std::unique_ptr<ClipboardDecode> again_decode_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       again                = *again_decode_storage;
        memset(static_cast<void*>(&again), 0, sizeof(again));
        TEST(Synth::decode_instrument_json(text,
                                           len,
                                           &again.instr,
                                           again.envs,
                                           &again.env_count,
                                           again.lfos,
                                           &again.lfo_count));
        TEST(again.instr.voice_mode == mode);
    }

    // Clipboard and validated-bank domains decode the same key.
    static const char single_doc[] =
        "{\"format\":\"synth-instrument-v1\",\"voices\":\"single\",\"layers\":[{\"wave_a\":\"sine\"}]}";
    const Synth::InstrumentJsonDomain domains[2] = { Synth::InstrumentJsonDomain::clipboard,
                                                     Synth::InstrumentJsonDomain::validated_bank };
    for (uint32_t d = 0; d < 2; d++) {
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_instrument_json_for_domain(single_doc,
                                                      sizeof(single_doc) - 1,
                                                      &decoded.instr,
                                                      decoded.envs,
                                                      &decoded.env_count,
                                                      decoded.lfos,
                                                      &decoded.lfo_count,
                                                      decoded.layout,
                                                      max_portable_records,
                                                      &decoded.layout_count,
                                                      domains[d]));
        TEST(decoded.instr.voice_mode == 2);
    }

    // Rejections: an unknown name, a wrong-case name and non-string values.
    static const char* const bad_values[] = { "\"poly\"", "\"Single\"", "1", "true", "null" };
    for (const char* bad : bad_values) {
        char doc[256];
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"voices\":%s,\"layers\":[{\"wave_a\":\"sine\"}]}",
                 bad);
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(! decode_clipboard(&decoded, doc, max_portable_records));
    }

    // An out-of-range stored mode cannot be spelled: the encoder fails.
    Synth::Instrument bad_instr = decoded.instr;
    bad_instr.voice_mode        = 3;
    TEST(Synth::encode_instrument_json(text, sizeof(text), &bad_instr, &desc_bank) == 0);

    // Pool (editor bank) form: round trip is byte-stable.
    const std::unique_ptr<Synth::InstrumentEditorBank> pool_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       pool         = *pool_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> back_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       back         = *back_storage;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> again_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&again_payload)[Synth::library_payload_max] = again_storage->elements;
    build_named_fm_fixture(&pool);
    for (uint32_t mode = 1; mode < 3; mode++) {
        pool.bank.instruments.entries[0].voice_mode = static_cast<uint8_t>(mode);
        const uint32_t len                          = Synth::encode_editor_bank_json(&pool, payload, sizeof(payload));
        TEST(len > 0);
        char expected[64];
        snprintf(expected, sizeof(expected), "\"voices\":\"%s\"", names[mode]);
        TEST(strstr(payload, expected) != nullptr);
        TEST(Synth::decode_editor_bank_json(payload, len, &back));
        TEST(back.bank.instruments.entries[0].voice_mode == mode);
        const uint32_t again_len = Synth::encode_editor_bank_json(&back, again_payload, sizeof(again_payload));
        // The fixture is not canonical, so stability is checked from the decoded bank on.
        TEST(again_len > 0);
        TEST(Synth::decode_editor_bank_json(again_payload, again_len, &back));
        TEST(back.bank.instruments.entries[0].voice_mode == mode);
        TEST(strstr(again_payload, expected) != nullptr);
        // A canonical document re-encodes to the same bytes.
        TEST(Synth::encode_editor_bank_json(&back, payload, sizeof(payload)) == again_len);
        TEST(memcmp(payload, again_payload, again_len) == 0);
    }
    pool.bank.instruments.entries[0].voice_mode = 0;
    uint32_t len                                = Synth::encode_editor_bank_json(&pool, payload, sizeof(payload));
    TEST(len > 0 && strstr(payload, "\"voices\"") == nullptr);
    pool.bank.instruments.entries[0].voice_mode = 3;
    TEST(Synth::encode_editor_bank_json(&pool, payload, sizeof(payload)) == 0);
}

// The oscillator fields mirror the note-on table of process_note_on.  The envelope fields
// stand for the node-owned state (EnvelopeState and last_point_ticks), which note-on zeroes
// through configure_target_modulation.  A takeover keeps the running signal state (phase,
// mod_phase, old_volume, old_panning, the FIR history flag and the FIR engagement state) and
// resets the note-dependent state.
// Test-only voice policy model, not execution of realtime_synth.cpp.
struct VoiceSimOsc {
    bool     used;
    bool     terminating;
    bool     decaying;
    bool     clear_fir_hist;
    bool     fir_active;
    uint8_t  voice_id;
    uint8_t  layer_idx;
    uint8_t  source_layer;
    uint8_t  env_reverse;
    uint16_t env_point;
    uint16_t env_tick;
    uint16_t last_point_ticks;
    uint32_t decay_tick;
    uint32_t noise_counter;
    uint32_t note_seed;
    uint32_t fir_taps_offs;
    float    old_volume;
    float    phase;
    float    mod_phase;
    float    old_panning;
    float    decay_mult;
    float    decay_factor;
};

struct VoiceSimVoice {
    bool     active;
    bool     releasing;
    bool     sustained;
    bool     note_finished;
    uint8_t  channel;
    uint8_t  note;
    uint8_t  instrument;
    uint8_t  osc_count;
    uint8_t  osc_ids[4];
    uint32_t release_sample;
    uint32_t release_total_ticks;
};

struct VoiceSimState {
    VoiceSimVoice voices[5]; // slot 0 is the sentinel: 4 usable voices
    VoiceSimOsc   oscs[10];  // slot 0 is the sentinel: 9 usable oscillators
    uint8_t       note_to_voice[4][128];
    float         release_seconds;     // the instrument's Release (s); zero leaves the decay off
    uint32_t      noise_start_counter; // mirrors the production noise_start_counter
};

constexpr uint32_t voice_sim_noise_stride = 0x9e3779b9u;

// Per-layer instrument data the sim stands in for: layer 1 and 2 carry a filter, only layer 1
// has a positive spread.  The base pan differs per layer.
static bool voice_sim_layer_filtered(uint32_t layer)
{
    return layer == 1 || layer == 2;
}

static bool voice_sim_layer_fir_active(uint32_t layer)
{
    return layer == 1;
}

static float voice_sim_layer_pan(uint32_t layer)
{
    return 0.1f * static_cast<float>(layer + 1);
}

static uint32_t voice_sim_fir_offset(uint32_t osc_slot)
{
    return 100 + 4 * osc_slot;
}

// Mirrors voice_restartable_in_place: the voice must still hold every layer, in order.
static bool voice_sim_restartable(const VoiceSimState& s, uint32_t voice_idx, uint32_t layers)
{
    const VoiceSimVoice& voice = s.voices[voice_idx];
    if (voice.osc_count != layers) {
        return false;
    }
    for (uint32_t i = 0; i < voice.osc_count; i++) {
        if (s.oscs[voice.osc_ids[i]].source_layer != i) {
            return false;
        }
    }
    return true;
}

// The allocation order and the in-place restart path of process_note_on.  Returns false when
// the note is dropped, and then the state is left exactly as it was (apart from the free voice
// slot that was touched and returned).
static bool voice_sim_note_on(VoiceSimState* s,
                              uint32_t       mode,
                              uint32_t       instrument,
                              uint32_t       channel,
                              uint32_t       note,
                              uint32_t       layers,
                              uint32_t       release_sample)
{
    const uint32_t existing = s->note_to_voice[channel][note];
    uint32_t       reset    = 0;
    if (mode == 1) {
        if (existing && ! s->voices[existing].note_finished && s->voices[existing].instrument == instrument &&
            voice_sim_restartable(*s, existing, layers)) {
            reset = existing;
        }
    }
    else if (mode == 2) {
        for (uint32_t i = 1; i < 5 && ! reset; i++) {
            if (s->voices[i].active && ! s->voices[i].note_finished && s->voices[i].instrument == instrument &&
                voice_sim_restartable(*s, i, layers)) {
                reset = i;
            }
        }
    }

    uint32_t prev_chan = 0;
    uint32_t prev_note = 0;
    uint32_t voice_idx = reset;
    if (reset) {
        // An in-place restart allocates nothing and cannot fail.
        prev_chan = s->voices[reset].channel;
        prev_note = s->voices[reset].note;
    }
    else {
        for (uint32_t i = 1; i < 5 && ! voice_idx; i++) {
            if (! s->voices[i].active) {
                voice_idx = i;
            }
        }
        if (! voice_idx) {
            return false;
        }
    }

    VoiceSimVoice& voice      = s->voices[voice_idx];
    voice.channel             = static_cast<uint8_t>(channel);
    voice.note                = static_cast<uint8_t>(note);
    voice.instrument          = static_cast<uint8_t>(instrument);
    voice.active              = true;
    voice.releasing           = false;
    voice.note_finished       = false;
    voice.sustained           = false;
    voice.release_sample      = release_sample;
    voice.release_total_ticks = Synth::sustain_decay_ticks(s->release_seconds);

    if (reset) {
        assert(voice.osc_count == layers);
    }
    else {
        uint32_t allocated = 0;
        for (uint32_t i = 1; i < 10 && allocated < layers; i++) {
            if (! s->oscs[i].used) {
                s->oscs[i].used            = true;
                s->oscs[i].voice_id        = static_cast<uint8_t>(voice_idx);
                voice.osc_ids[allocated++] = static_cast<uint8_t>(i);
            }
        }
        if (allocated < layers) {
            for (uint32_t i = 0; i < allocated; i++) {
                memset(static_cast<void*>(&s->oscs[voice.osc_ids[i]]), 0, sizeof(VoiceSimOsc));
                voice.osc_ids[i] = 0;
            }
            voice.active    = false;
            voice.osc_count = 0;
            return false;
        }
        voice.osc_count = static_cast<uint8_t>(layers);
    }

    if (reset && (prev_chan != channel || prev_note != note) && s->note_to_voice[prev_chan][prev_note] == voice_idx) {
        s->note_to_voice[prev_chan][prev_note] = 0;
    }
    if (mode == 2) {
        for (uint32_t i = 1; i < 5; i++) {
            VoiceSimVoice& swept = s->voices[i];
            if (i != voice_idx && swept.active && ! swept.note_finished && swept.instrument == instrument) {
                if (s->note_to_voice[swept.channel][swept.note] == i) {
                    s->note_to_voice[swept.channel][swept.note] = 0;
                }
                swept.note_finished = true;
            }
        }
    }
    if (existing && existing != voice_idx) {
        s->voices[existing].note_finished = true;
    }
    s->note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);

    for (uint32_t layer = 0; layer < layers; layer++) {
        VoiceSimOsc& osc = s->oscs[voice.osc_ids[layer]];
        osc.layer_idx    = static_cast<uint8_t>(layer);
        osc.source_layer = static_cast<uint8_t>(layer);
        if (! reset) {
            osc.phase       = 0.0f;
            osc.mod_phase   = 0.0f;
            osc.old_volume  = 0.0f;
            osc.old_panning = voice_sim_layer_pan(layer);
        }
        osc.terminating  = false;
        osc.decay_mult   = Synth::sustain_decay_per_tick(voice.release_total_ticks);
        osc.decay_factor = 1.0f;
        osc.decay_tick   = 0;
        osc.decaying     = false;
        s->noise_start_counter += voice_sim_noise_stride;
        osc.noise_counter    = s->noise_start_counter;
        osc.note_seed        = s->noise_start_counter;
        osc.env_point        = 0;
        osc.env_tick         = 0;
        osc.env_reverse      = 0;
        osc.last_point_ticks = 0;
        // A restarted oscillator which already runs this filter keeps its history and engagement.
        const bool keeps_filter = reset && (osc.fir_taps_offs != 0) == voice_sim_layer_filtered(layer);
        if (voice_sim_layer_filtered(layer)) {
            osc.fir_taps_offs = voice_sim_fir_offset(voice.osc_ids[layer]);
            if (! keeps_filter) {
                osc.clear_fir_hist = true;
            }
        }
        else {
            osc.fir_taps_offs  = 0;
            osc.clear_fir_hist = false;
        }
        if (! keeps_filter) {
            osc.fir_active = voice_sim_layer_fir_active(layer);
        }
    }
    return true;
}

// Gives every live oscillator and voice distinctive state, so any carried field shows up.  The
// values follow the reset table in the approved test specification.
static void voice_sim_mark(VoiceSimState* s)
{
    for (uint32_t i = 1; i < 10; i++) {
        VoiceSimOsc& osc = s->oscs[i];
        if (osc.used) {
            osc.old_volume       = 0.05f * static_cast<float>(i);
            osc.phase            = 0.77f;
            osc.mod_phase        = 0.66f;
            osc.noise_counter    = 12345;
            osc.note_seed        = 999;
            osc.old_panning      = 0.9f;
            osc.decay_factor     = 0.5f;
            osc.decay_mult       = 0.25f;
            osc.decay_tick       = 40;
            osc.decaying         = true;
            osc.terminating      = true;
            osc.clear_fir_hist   = false;
            osc.fir_active       = true;
            osc.fir_taps_offs    = voice_sim_layer_filtered(osc.source_layer) ? 777u : 0u;
            osc.env_point        = 5;
            osc.env_tick         = 1;
            osc.env_reverse      = 1;
            osc.last_point_ticks = 3;
        }
    }
    for (uint32_t i = 1; i < 5; i++) {
        if (s->voices[i].active) {
            s->voices[i].releasing           = true;
            s->voices[i].sustained           = true;
            s->voices[i].release_sample      = 5000;
            s->voices[i].release_total_ticks = 999;
        }
    }
}

// A dropped note touches a free voice slot and gives it back.  The slot is not part of the
// playing state, so the exhaustion comparisons blank it first.
static void voice_sim_clear_free_voice(VoiceSimState* s, uint32_t voice_idx)
{
    TEST(! s->voices[voice_idx].active);
    memset(static_cast<void*>(&s->voices[voice_idx]), 0, sizeof(VoiceSimVoice));
}

static uint32_t voice_sim_used_oscs(const VoiceSimState& s)
{
    uint32_t count = 0;
    for (uint32_t i = 1; i < 10; i++) {
        count += s.oscs[i].used ? 1 : 0;
    }
    return count;
}

static uint32_t voice_sim_live(const VoiceSimState& s, uint32_t instrument)
{
    uint32_t count = 0;
    for (uint32_t i = 1; i < 5; i++) {
        count += (s.voices[i].active && ! s.voices[i].note_finished && s.voices[i].instrument == instrument) ? 1 : 0;
    }
    return count;
}

static void check_voice_mode_sim()
{
    // Reuse per-note: the same note restarts in place.  The running signal state carries.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 2, 0));
        voice_sim_mark(&s);
        const uint32_t voice = s.note_to_voice[0][60];
        const float    seed0 = s.oscs[s.voices[voice].osc_ids[0]].old_volume;
        const float    seed1 = s.oscs[s.voices[voice].osc_ids[1]].old_volume;
        TEST(seed0 > 0.0f && seed1 > seed0);
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 2, 123));
        TEST(s.note_to_voice[0][60] == voice);
        TEST(voice_sim_live(s, 0) == 1);
        const VoiceSimVoice& v = s.voices[voice];
        TEST(v.active && ! v.releasing && ! v.sustained && ! v.note_finished && v.release_sample == 123);
        TEST(v.osc_count == 2);
        TEST(s.oscs[v.osc_ids[0]].old_volume == seed0 && s.oscs[v.osc_ids[1]].old_volume == seed1);
        for (uint32_t layer = 0; layer < 2; layer++) {
            const VoiceSimOsc& osc = s.oscs[v.osc_ids[layer]];
            TEST(osc.phase == 0.77f && osc.mod_phase == 0.66f);
            TEST(osc.decay_tick == 0 && ! osc.terminating && osc.voice_id == voice);
        }
    }

    // Reuse per-note: a different note takes a new voice, and a different instrument is
    // never reset.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 1, 0));
        voice_sim_mark(&s);
        TEST(voice_sim_note_on(&s, 1, 0, 0, 62, 1, 0));
        TEST(s.note_to_voice[0][60] == 1 && s.note_to_voice[0][62] == 2);
        TEST(voice_sim_live(s, 0) == 2);
        TEST(s.oscs[s.voices[1].osc_ids[0]].phase == 0.77f);
        TEST(voice_sim_note_on(&s, 1, 3, 0, 60, 1, 0)); // key remapped to instrument 3
        TEST(s.note_to_voice[0][60] == 3 && s.voices[1].note_finished);
        TEST(s.oscs[s.voices[3].osc_ids[0]].old_volume == 0.0f);
    }

    // Reuse per-note with the oscillator pool full: the restart needs no oscillator, so it
    // succeeds.  A restart whose layer count grew cannot restart in place; it needs a new
    // voice and new oscillators, finds none, and the old note keeps playing untouched.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        TEST(voice_sim_note_on(&s, 0, 1, 0, 61, 4, 0));
        TEST(voice_sim_note_on(&s, 0, 1, 0, 62, 4, 0)); // 9 of 9 oscillators in use
        voice_sim_mark(&s);
        VoiceSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &s, sizeof(s));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 1, 0)); // same layer count: in place
        TEST(s.note_to_voice[0][60] == 1 && voice_sim_used_oscs(s) == 9);
        TEST(s.oscs[s.voices[1].osc_ids[0]].phase == 0.77f);
        memcpy(static_cast<void*>(&snapshot), &s, sizeof(s));
        TEST(! voice_sim_note_on(&s, 1, 0, 0, 60, 2, 0)); // the layer count grew to two
        voice_sim_clear_free_voice(&s, 4);
        voice_sim_clear_free_voice(&snapshot, 4);
        TEST(memcmp(&s, &snapshot, sizeof(s)) == 0);
    }

    // A voice which lost a layer (the last live entry moved into the gap) cannot restart in
    // place.  The note takes a new voice with fresh state and the old voice fades out.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 3, 0));
        VoiceSimVoice& v = s.voices[1];
        for (uint32_t layer = 0; layer < 3; layer++) {
            s.oscs[v.osc_ids[layer]].old_volume = 0.1f * static_cast<float>(layer + 1);
            s.oscs[v.osc_ids[layer]].phase      = 0.5f;
        }
        const uint8_t gone = v.osc_ids[0];
        memset(static_cast<void*>(&s.oscs[gone]), 0, sizeof(VoiceSimOsc));
        v.osc_ids[0]                   = v.osc_ids[2];
        v.osc_ids[2]                   = 0;
        v.osc_count                    = 2;
        s.oscs[v.osc_ids[0]].layer_idx = 0;
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 3, 0));
        TEST(s.note_to_voice[0][60] == 2 && s.voices[1].note_finished && s.voices[1].active);
        TEST(s.voices[2].osc_count == 3);
        for (uint32_t layer = 0; layer < 3; layer++) {
            TEST(s.oscs[s.voices[2].osc_ids[layer]].old_volume == 0.0f);
            TEST(s.oscs[s.voices[2].osc_ids[layer]].phase == 0.0f);
        }
        TEST(s.voices[1].osc_count == 2 && s.oscs[s.voices[1].osc_ids[0]].old_volume == 0.3f);
    }

    // Single mode: four different notes leave exactly one live voice, on any channel.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        const uint32_t channels[4] = { 0, 1, 0, 2 };
        const uint32_t notes[4]    = { 60, 64, 67, 72 };
        float          carried     = 0.0f;
        for (uint32_t i = 0; i < 4; i++) {
            TEST(voice_sim_note_on(&s, 2, 0, channels[i], notes[i], 1, 0));
            TEST(voice_sim_live(s, 0) == 1);
            TEST(s.note_to_voice[channels[i]][notes[i]] == 1);
            if (i) {
                TEST(s.note_to_voice[channels[i - 1]][notes[i - 1]] == 0);
                TEST(s.oscs[s.voices[1].osc_ids[0]].old_volume == carried);
            }
            s.oscs[s.voices[1].osc_ids[0]].old_volume = 0.25f * static_cast<float>(i + 1);
            carried                                   = 0.25f * static_cast<float>(i + 1);
        }
        // Another instrument is unaffected.
        TEST(voice_sim_note_on(&s, 2, 5, 0, 50, 1, 0));
        TEST(voice_sim_live(s, 0) == 1 && voice_sim_live(s, 5) == 1);
    }

    // Single mode sweep: other live voices of the instrument fade out.  A key which was
    // remapped is not cleared by the sweep.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0)); // voice 1
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0)); // voice 2
        TEST(voice_sim_note_on(&s, 0, 0, 0, 64, 1, 0)); // voice 3
        s.note_to_voice[0][64] = 0;                     // voice 3 lost its key
        s.note_to_voice[0][65] = 3;                     // and some other voice owns a key
        TEST(voice_sim_note_on(&s, 2, 0, 0, 67, 1, 0));
        TEST(s.note_to_voice[0][67] == 1 && s.note_to_voice[0][60] == 0 && s.note_to_voice[0][62] == 0);
        TEST(s.voices[2].note_finished && s.voices[3].note_finished && ! s.voices[1].note_finished);
        TEST(s.note_to_voice[0][65] == 3); // the stale key of voice 3 stays; only equal keys clear
        TEST(voice_sim_live(s, 0) == 1);
        TEST(s.voices[2].active && s.voices[3].active); // swept voices fade, they are not cut
    }

    // Single mode: the takeover note's key held by another live voice of the instrument.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0));
        TEST(voice_sim_note_on(&s, 2, 0, 0, 62, 1, 0)); // same key as voice 2; voice 1 is lowest
        TEST(s.note_to_voice[0][62] == 1 && s.voices[2].note_finished && ! s.voices[1].note_finished);
        TEST(s.note_to_voice[0][60] == 0 && voice_sim_live(s, 0) == 1);
    }

    // Single mode with the oscillator pool full: a takeover with the same layer count needs no
    // oscillator and succeeds.  A takeover which needs a new voice and finds no oscillator is
    // dropped, and it leaves everything, including the sweep targets, alone.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 4, 0));
        TEST(voice_sim_note_on(&s, 0, 1, 0, 64, 4, 0)); // 9 of 9 oscillators in use
        voice_sim_mark(&s);
        TEST(voice_sim_note_on(&s, 2, 0, 0, 70, 1, 0)); // takes voice 1 in place; voice 2 fades
        TEST(s.note_to_voice[0][70] == 1 && s.voices[2].note_finished && voice_sim_used_oscs(s) == 9);
        VoiceSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &s, sizeof(s));
        TEST(! voice_sim_note_on(&s, 2, 0, 0, 71, 5, 0)); // layer count mismatch, no free oscillator
        voice_sim_clear_free_voice(&s, 4);
        voice_sim_clear_free_voice(&snapshot, 4);
        TEST(memcmp(&s, &snapshot, sizeof(s)) == 0);
    }

    // Single mode with the voice pool full and no live voice of the instrument: clean drop.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        for (uint32_t i = 0; i < 4; i++) {
            TEST(voice_sim_note_on(&s, 0, 1, 0, 60 + i, 1, 0));
        }
        voice_sim_mark(&s);
        VoiceSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &s, sizeof(s));
        TEST(! voice_sim_note_on(&s, 2, 0, 0, 80, 1, 0));
        TEST(memcmp(&s, &snapshot, sizeof(s)) == 0);
    }
}

// Note-off and termination models for the voice sim.  They mirror process_note_off with
// begin_voice_release, and the free branch of update_modulation.
static void voice_sim_note_off(VoiceSimState* s, bool pedal_down, uint32_t channel, uint32_t note)
{
    const uint32_t voice_idx = s->note_to_voice[channel][note];
    if (! voice_idx) {
        return;
    }
    if (pedal_down) {
        s->voices[voice_idx].sustained = true;
    }
    else if (! s->voices[voice_idx].releasing) {
        s->voices[voice_idx].releasing = true;
    }
}

// The voice's last oscillator is gone: a note_finished voice leaves the mapping alone.
static void voice_sim_terminate(VoiceSimState* s, uint32_t voice_idx)
{
    VoiceSimVoice& voice = s->voices[voice_idx];
    for (uint32_t i = 0; i < voice.osc_count; i++) {
        memset(static_cast<void*>(&s->oscs[voice.osc_ids[i]]), 0, sizeof(VoiceSimOsc));
    }
    if (! voice.note_finished) {
        s->note_to_voice[voice.channel][voice.note] = 0;
    }
    voice.osc_count = 0;
    memset(voice.osc_ids, 0, sizeof(voice.osc_ids));
    voice.active = false;
}

// Every field of an in-place restarted voice.  The signal state (phase, mod_phase, old_volume,
// old_panning, the FIR history flag and the FIR engagement state) equals the state before the
// restart, and the oscillator slots did not move.  The note-dependent fields hold their fresh
// note-on values.  Noise_before is the sim's noise_start_counter ahead of the note-on.
static void voice_sim_expect_restart(const VoiceSimState& s,
                                     const VoiceSimState& before,
                                     uint32_t             voice_idx,
                                     uint32_t             layers,
                                     uint32_t             release_sample,
                                     uint32_t             noise_before)
{
    const VoiceSimVoice& voice = s.voices[voice_idx];
    TEST(voice.active && ! voice.releasing && ! voice.sustained && ! voice.note_finished);
    TEST(voice.release_sample == release_sample);
    TEST(voice.release_total_ticks == Synth::sustain_decay_ticks(s.release_seconds));
    TEST(voice.osc_count == layers);
    TEST(memcmp(voice.osc_ids, before.voices[voice_idx].osc_ids, sizeof(voice.osc_ids)) == 0);
    for (uint32_t layer = 0; layer < layers; layer++) {
        const uint32_t     slot = voice.osc_ids[layer];
        const VoiceSimOsc& osc  = s.oscs[slot];
        const VoiceSimOsc& old  = before.oscs[slot];
        TEST(osc.used && osc.voice_id == voice_idx);
        TEST(osc.layer_idx == layer && osc.source_layer == layer);
        // Persisting signal state.
        TEST(osc.phase == old.phase && osc.mod_phase == old.mod_phase);
        TEST(osc.old_volume == old.old_volume && osc.old_panning == old.old_panning);
        TEST(osc.clear_fir_hist == old.clear_fir_hist && osc.fir_active == old.fir_active);
        // Note-dependent state.
        TEST(! osc.terminating && ! osc.decaying);
        TEST(osc.decay_tick == 0 && osc.decay_factor == 1.0f);
        TEST(osc.decay_mult == Synth::sustain_decay_per_tick(voice.release_total_ticks));
        const uint32_t expected_noise = noise_before + voice_sim_noise_stride * (layer + 1);
        TEST(osc.noise_counter == expected_noise && osc.note_seed == expected_noise);
        TEST(osc.env_point == 0 && osc.env_tick == 0 && osc.env_reverse == 0 && osc.last_point_ticks == 0);
        TEST(osc.fir_taps_offs == (voice_sim_layer_filtered(layer) ? voice_sim_fir_offset(slot) : 0u));
    }
}

static void check_voice_mode_reset_sim()
{
    // T11.1: every field of the old note is pre-loaded with a distinctive value.  After the
    // restart, the signal state persists and every note-dependent field equals its fresh
    // note-on value.  Both the same-note reuse and the single-mode takeover of another note
    // take this path.
    for (uint32_t mode = 1; mode < 3; mode++) {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        s.release_seconds = 1.0f;
        TEST(voice_sim_note_on(&s, mode, 0, 0, 60, 3, 0));
        voice_sim_mark(&s);
        const uint32_t voice = s.note_to_voice[0][60];
        for (uint32_t layer = 0; layer < 3; layer++) {
            TEST(s.oscs[s.voices[voice].osc_ids[layer]].old_volume > 0.0f);
        }
        VoiceSimState before;
        memcpy(static_cast<void*>(&before), &s, sizeof(s));
        const uint32_t noise_before = s.noise_start_counter;
        const uint32_t note         = mode == 1 ? 60 : 64;
        TEST(voice_sim_note_on(&s, mode, 0, 0, note, 3, 4321));
        TEST(s.note_to_voice[0][note] == voice);
        TEST(voice_sim_live(s, 0) == 1);
        voice_sim_expect_restart(s, before, voice, 3, 4321, noise_before);
        TEST(s.voices[voice].channel == 0 && s.voices[voice].note == note && s.voices[voice].instrument == 0);
        TEST(voice_sim_used_oscs(s) == 3);
        // The marked values themselves persisted (guards the comparison against a vacuous match).
        for (uint32_t layer = 0; layer < 3; layer++) {
            const VoiceSimOsc& osc = s.oscs[s.voices[voice].osc_ids[layer]];
            TEST(osc.phase == 0.77f && osc.mod_phase == 0.66f && osc.old_panning == 0.9f);
            TEST(osc.fir_active && ! osc.clear_fir_hist);
        }
    }

    // T11.2: the restart keeps the oscillator slots, so the FIR offsets stay where they are and
    // the history is not cleared.  A slot freed ahead of the old note's slots is not taken.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 62, 1, 0)); // voice 1 holds oscillator 1
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 3, 0)); // voice 2 holds oscillators 2..4
        voice_sim_mark(&s);
        voice_sim_terminate(&s, 1);
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 3, 0));
        const VoiceSimVoice& v = s.voices[2];
        TEST(v.osc_ids[0] == 2 && v.osc_ids[1] == 3 && v.osc_ids[2] == 4);
        TEST(! s.oscs[1].used);
        TEST(s.oscs[2].fir_taps_offs == 0 && ! s.oscs[2].clear_fir_hist);
        TEST(s.oscs[3].fir_taps_offs == voice_sim_fir_offset(3) && ! s.oscs[3].clear_fir_hist && s.oscs[3].fir_active);
        TEST(s.oscs[4].fir_taps_offs == voice_sim_fir_offset(4) && ! s.oscs[4].clear_fir_hist && s.oscs[4].fir_active);
    }

    // T9.1: the same note restarts from the attack, the sustain (decay not yet started and
    // running) and the middle of the release.  The real decay functions fix the fresh values.
    for (uint32_t position = 0; position < 4; position++) {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        s.release_seconds = 2.0f;
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 2, 0));
        TEST(voice_sim_note_on(&s, 1, 0, 0, 67, 1, 0)); // an unrelated voice stays alone
        const uint32_t voice = s.note_to_voice[0][60];
        VoiceSimVoice& v     = s.voices[voice];
        for (uint32_t layer = 0; layer < 2; layer++) {
            VoiceSimOsc& osc = s.oscs[v.osc_ids[layer]];
            osc.old_volume   = 0.3f + 0.1f * static_cast<float>(layer);
            osc.phase        = 0.4f;
            osc.mod_phase    = 0.3f;
            osc.env_reverse  = 0;
            if (position == 0) { // attack, held
                osc.env_point = 0;
                osc.env_tick  = 3;
            }
            else if (position == 1) { // sustain, decay not started
                osc.env_point = 2;
                osc.env_tick  = 5;
            }
            else if (position == 2) { // sustain, decay running
                osc.env_point    = 2;
                osc.env_tick     = 5;
                osc.decaying     = true;
                osc.decay_tick   = 12;
                osc.decay_factor = 0.8f;
                osc.decay_mult   = Synth::sustain_decay_per_tick(Synth::sustain_decay_ticks(2.0f));
            }
            else { // released, mid-decay
                v.releasing      = true;
                osc.env_point    = 3;
                osc.env_tick     = 6;
                osc.decaying     = true;
                osc.decay_tick   = 10;
                osc.decay_factor = 0.7f;
                osc.decay_mult   = Synth::sustain_decay_per_tick(Synth::sustain_decay_ticks(2.0f));
            }
        }
        VoiceSimState before;
        memcpy(static_cast<void*>(&before), &s, sizeof(s));
        const uint32_t voices_before = voice_sim_live(s, 0);
        const uint32_t noise_before  = s.noise_start_counter;
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 2, 9));
        TEST(s.note_to_voice[0][60] == voice);
        TEST(voice_sim_live(s, 0) == voices_before);
        TEST(s.voices[voice].osc_count == 2 && voice_sim_used_oscs(s) == 3);
        voice_sim_expect_restart(s, before, voice, 2, 9, noise_before);
        // No dip: the restart's amplitude continues from the running level, never from zero.
        TEST(s.oscs[s.voices[voice].osc_ids[0]].old_volume > 0.0f);
        TEST(s.oscs[s.voices[voice].osc_ids[0]].phase == 0.4f && s.oscs[s.voices[voice].osc_ids[0]].mod_phase == 0.3f);
    }

    // T9.2: play, release, then re-press before the voice ends.  The seed is the decayed level.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        s.release_seconds = 1.0f;
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 1, 0));
        const uint32_t voice = s.note_to_voice[0][60];
        VoiceSimOsc&   osc   = s.oscs[s.voices[voice].osc_ids[0]];
        float          level = 0.5f;
        osc.decaying         = true;
        for (uint32_t tick = 0; tick < 8; tick++) {
            level *= osc.decay_mult;
            osc.decay_factor *= osc.decay_mult;
            osc.decay_tick++;
        }
        osc.old_volume = level;
        voice_sim_note_off(&s, false, 0, 60);
        TEST(s.voices[voice].releasing);
        TEST(level > 0.0f && level < 0.5f);
        TEST(voice_sim_note_on(&s, 1, 0, 0, 60, 1, 0));
        TEST(s.note_to_voice[0][60] == voice);
        const VoiceSimOsc& fresh = s.oscs[s.voices[voice].osc_ids[0]];
        TEST(fresh.old_volume == level && ! s.voices[voice].releasing);
        TEST(fresh.decay_tick == 0 && fresh.decay_factor == 1.0f && ! fresh.decaying);
    }

    // T9.7: the layer count changes across the restart in both directions.  The old voice cannot
    // restart in place, so the note takes a new voice with fresh state and the old voice fades.
    {
        const uint32_t before[2] = { 3, 1 };
        const uint32_t after[2]  = { 1, 3 };
        for (uint32_t i = 0; i < 2; i++) {
            VoiceSimState s;
            memset(static_cast<void*>(&s), 0, sizeof(s));
            TEST(voice_sim_note_on(&s, 1, 0, 0, 60, before[i], 0));
            voice_sim_mark(&s);
            TEST(voice_sim_note_on(&s, 1, 0, 0, 60, after[i], 0));
            TEST(s.note_to_voice[0][60] == 2);
            TEST(s.voices[1].active && s.voices[1].note_finished && s.voices[1].osc_count == before[i]);
            const VoiceSimVoice& v = s.voices[2];
            TEST(v.osc_count == after[i] && voice_sim_used_oscs(s) == before[i] + after[i]);
            for (uint32_t layer = 0; layer < after[i]; layer++) {
                const VoiceSimOsc& osc = s.oscs[v.osc_ids[layer]];
                TEST(osc.used && osc.voice_id == 2);
                TEST(osc.old_volume == 0.0f && osc.phase == 0.0f && osc.mod_phase == 0.0f);
            }
            for (uint32_t layer = after[i]; layer < 4; layer++) {
                TEST(v.osc_ids[layer] == 0);
            }
        }
    }

    // T8.1/T8.2: multiple mode keeps the clean-drop rule.  A retrigger gets a distinct voice
    // with a zero seed, and a full voice pool drops the retrigger without a change.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        voice_sim_mark(&s);
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        const uint32_t new_voice = s.note_to_voice[0][60];
        TEST(new_voice == 2 && s.voices[1].note_finished && ! s.voices[2].note_finished);
        TEST(s.oscs[s.voices[2].osc_ids[0]].old_volume == 0.0f);
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 64, 1, 0));
        voice_sim_mark(&s);
        VoiceSimState snapshot;
        memcpy(static_cast<void*>(&snapshot), &s, sizeof(s));
        TEST(! voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));
        TEST(! voice_sim_note_on(&s, 0, 0, 0, 70, 1, 0));
        TEST(memcmp(&s, &snapshot, sizeof(s)) == 0);
    }

    // T10.2/T10.3: after a single-mode takeover the old key does nothing at note-off, with and
    // without the sustain pedal, and the new key releases (or is held by the pedal) normally.
    for (uint32_t pedal = 0; pedal < 2; pedal++) {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 2, 0, 0, 60, 1, 0));
        TEST(voice_sim_note_on(&s, 2, 0, 0, 64, 1, 0));
        TEST(s.note_to_voice[0][60] == 0 && s.note_to_voice[0][64] == 1);
        voice_sim_note_off(&s, pedal != 0, 0, 60);
        TEST(! s.voices[1].releasing && ! s.voices[1].sustained);
        voice_sim_note_off(&s, pedal != 0, 0, 64);
        TEST(pedal ? (s.voices[1].sustained && ! s.voices[1].releasing)
                   : (s.voices[1].releasing && ! s.voices[1].sustained));
    }

    // T10.7 (second part)/T10.8: a swept voice ends later.  Its termination must not clear a
    // key which another note has taken since.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0)); // voice 1
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0)); // voice 2
        TEST(voice_sim_note_on(&s, 2, 0, 0, 67, 1, 0)); // takes voice 1, sweeps voice 2
        TEST(s.voices[2].note_finished && s.note_to_voice[0][62] == 0);
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0)); // the swept key is played again
        const uint32_t again = s.note_to_voice[0][62];
        TEST(again == 3);
        voice_sim_terminate(&s, 2);
        TEST(s.note_to_voice[0][62] == again && ! s.voices[2].active);
        // The slot of the ended voice is reused for another note on the same key.
        TEST(voice_sim_note_on(&s, 0, 0, 0, 62, 1, 0));
        TEST(s.note_to_voice[0][62] == 2 && s.voices[3].note_finished);
        voice_sim_terminate(&s, 3);
        TEST(s.note_to_voice[0][62] == 2);
        voice_sim_terminate(&s, 2); // the unfinished voice clears its own key
        TEST(s.note_to_voice[0][62] == 0);
    }

    // T10.9: another live voice holds the takeover key.  It fades, and the final mapping is the
    // reset voice.  The other voice may belong to another instrument.
    for (uint32_t other_instrument = 0; other_instrument < 2; other_instrument++) {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 0, 0, 0, 60, 1, 0));                // voice 1, the lowest of the instrument
        TEST(voice_sim_note_on(&s, 0, other_instrument, 0, 62, 1, 0)); // voice 2 owns the key
        TEST(voice_sim_note_on(&s, 2, 0, 0, 62, 1, 0));
        TEST(s.note_to_voice[0][62] == 1);
        TEST(s.voices[2].note_finished && s.voices[2].active);
        TEST(! s.voices[1].note_finished);
        TEST(s.note_to_voice[0][60] == 0);
        // The cross-instrument voice is not an instrument-0 voice, so only the key rule fades it.
        TEST(voice_sim_live(s, 0) == 1);
        TEST(voice_sim_live(s, 1) == 0);
    }

    // T10.13: scope.  Another instrument's voices are left alone, and the same instrument on a
    // second MIDI channel is taken over.  This follows the approved per-instrument ruling.
    {
        VoiceSimState s;
        memset(static_cast<void*>(&s), 0, sizeof(s));
        TEST(voice_sim_note_on(&s, 2, 0, 0, 60, 1, 0)); // instrument X, channel 0
        TEST(voice_sim_note_on(&s, 2, 1, 0, 64, 1, 0)); // instrument Y, channel 0
        TEST(voice_sim_live(s, 0) == 1 && voice_sim_live(s, 1) == 1);
        TEST(s.note_to_voice[0][60] == 1 && s.note_to_voice[0][64] == 2);
        TEST(voice_sim_note_on(&s, 2, 0, 1, 67, 1, 0)); // instrument X, channel 1
        TEST(s.note_to_voice[1][67] == 1 && s.note_to_voice[0][60] == 0);
        TEST(voice_sim_live(s, 0) == 1 && voice_sim_live(s, 1) == 1);
        TEST(s.note_to_voice[0][64] == 2 && ! s.voices[2].note_finished);
        TEST(s.voices[1].channel == 1 && s.voices[1].note == 67);
    }
}

static void make_cc_input_bank(Synth::InstrumentBank& bank, uint32_t owner, Synth::ModSource source, uint8_t cc_number)
{
    make_valid_bank(bank);
    Synth::EffectSlotBinding* const input_slot  = enabled_effect(bank, owner, 0, Synth::EffectType::delay);
    input_slot->bindings[0].num_inputs          = 1;
    input_slot->bindings[0].inputs[0].source    = source;
    input_slot->bindings[0].inputs[0].cc_number = cc_number;
    input_slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
    input_slot->bindings[0].inputs[0].scale     = 0.5f;
}

static void check_runtime_inline_0()
{
    // ---------------------------------------------------------------------------
    // CC routing tests (T1-T6).  The value normalization formula:
    // value = (MSB*128 + effective_LSB) / 16383, where effective_LSB is the
    // received LSB once one has arrived for the slot and otherwise a copy of
    // the MSB (0xFF sentinel in the LSB array).  The enable threshold (raw
    // >= 8192) is intentionally out of scope here.
    // ---------------------------------------------------------------------------

    // Documented initial channel CC state: MSB registers at zero, every LSB
    // register at the 0xFF "not yet seen" sentinel.
    auto make_cc_state = []() {
        Synth::ChannelCcState state;
        memset(static_cast<void*>(&state), 0, sizeof(state));
        memset(static_cast<void*>(state.lsb), 0xFF, sizeof(state.lsb));
        return state;
    };

    // T1: cc_14bit_value implements the ruled normalization exactly.  MSB-only
    // (sentinel) input equals MSB/127 because (MSB*129)/16383 and MSB/127 are
    // the same real value, so exact float comparison holds.
    {
        TEST(Synth::cc_14bit_value(0, 0xFF) == 0.0f / 127.0f);
        TEST(Synth::cc_14bit_value(1, 0xFF) == 1.0f / 127.0f);
        TEST(Synth::cc_14bit_value(63, 0xFF) == 63.0f / 127.0f);
        TEST(Synth::cc_14bit_value(64, 0xFF) == 64.0f / 127.0f);
        TEST(Synth::cc_14bit_value(126, 0xFF) == 126.0f / 127.0f);
        TEST(Synth::cc_14bit_value(127, 0xFF) == 127.0f / 127.0f);
        TEST(Synth::cc_14bit_value(127, 127) == 1.0f);
        TEST(Synth::cc_14bit_value(127, 0) == 16256.0f / 16383.0f);
        TEST(Synth::cc_14bit_value(64, 0) == 8192.0f / 16383.0f);
        TEST(Synth::cc_14bit_value(1, 1) == 129.0f / 16383.0f);
        TEST(Synth::cc_14bit_value(127, 126) == (127.0f * 128.0f + 126.0f) / 16383.0f);
    }

    // T2: apply_channel_cc accumulates the MSB/LSB pair per slot and channel.
    {
        // MSB-only updates return MSB/127 exactly while the LSB sentinel stands.
        Synth::ChannelCcState state = make_cc_state();
        TEST(Synth::apply_channel_cc(&state, 16, 100) == 100.0f / 127.0f);
        TEST(Synth::apply_channel_cc(&state, 16, 127) == 1.0f);
        // A real LSB arrival (including ZERO) clears the sentinel and recomputes;
        // MSB=127 then LSB=0 moves 1.0 -> 16256/16383 (accepted one-time correction).
        TEST(Synth::apply_channel_cc(&state, 48, 0) == 16256.0f / 16383.0f);
        TEST(Synth::apply_channel_cc(&state, 48, 30) == (127.0f * 128.0f + 30.0f) / 16383.0f);

        // An LSB arriving before any MSB clears the sentinel; MSB is still 0.
        state = make_cc_state();
        TEST(Synth::apply_channel_cc(&state, 48, 50) == 50.0f / 16383.0f);
        TEST(Synth::apply_channel_cc(&state, 48, 0) == 0.0f); // zero LSB also clears
        TEST(Synth::apply_channel_cc(&state, 16, 127) == 16256.0f / 16383.0f);

        // Non-assignable controllers (CC 1 among them) leave the state untouched.
        state = make_cc_state();
        (void)Synth::apply_channel_cc(&state, 16, 100);
        const Synth::ChannelCcState untouched = state;
        uint32_t                    foreign[] = { 1, 15, 32, 33, 47, 64, 119 };
        for (uint32_t i = 0; i < sizeof(foreign) / sizeof(foreign[0]); ++i) {
            (void)Synth::apply_channel_cc(&state, foreign[i], 64);
            TEST(memcmp(&state, &untouched, sizeof(state)) == 0);
        }

        // Pair rule CC n + CC n+32: pairs 16/48, 17/49 ... 31/63 land in slots 0..15.
        state = make_cc_state();
        Synth::apply_channel_cc(&state, 16, 100);
        Synth::apply_channel_cc(&state, 48, 30);
        TEST(Synth::apply_channel_cc(&state, 17, 64) == 64.0f / 127.0f); // slot 1, sentinel
        Synth::apply_channel_cc(&state, 49, 20);
        Synth::apply_channel_cc(&state, 31, 127);
        TEST(Synth::apply_channel_cc(&state, 63, 0) == 16256.0f / 16383.0f); // slot 15
        TEST(state.msb[0] == 100 && state.lsb[0] == 30);                     // adjacent pair left slot 0 unchanged
        TEST(state.msb[1] == 64 && state.lsb[1] == 20);
        TEST(state.msb[15] == 127 && state.lsb[15] == 0);

        // Channels are independent: writing one state leaves another untouched.
        Synth::ChannelCcState other = make_cc_state();
        (void)Synth::apply_channel_cc(&other, 16, 5);
        TEST(other.msb[0] == 5);
        TEST(state.msb[0] == 100 && state.lsb[0] == 30);
    }

    // Sustain pedal: the CC 64 threshold sits between 63 and 64, and the
    // per-channel pedal state is independent.  CC 64 is neither an
    // assignable CC nor a mix CC, so the pedal branch reuses no existing
    // routing.
    {
        Synth::ChannelCcState pedal_state = make_cc_state();
        TEST(! Synth::apply_sustain_pedal(&pedal_state, 63));
        TEST(! pedal_state.sustain_pedal_down);
        TEST(Synth::apply_sustain_pedal(&pedal_state, 64));
        TEST(pedal_state.sustain_pedal_down);
        TEST(Synth::apply_sustain_pedal(&pedal_state, 127));
        TEST(! Synth::apply_sustain_pedal(&pedal_state, 0));
        TEST(! pedal_state.sustain_pedal_down);
        Synth::ChannelCcState other_pedal = make_cc_state();
        TEST(! other_pedal.sustain_pedal_down);
        TEST(Synth::apply_sustain_pedal(&pedal_state, 64));
        TEST(pedal_state.sustain_pedal_down && ! other_pedal.sustain_pedal_down);
        TEST(! Synth::is_assignable_cc(Synth::sustain_pedal_cc));
        TEST(Synth::mix_cc_role(Synth::sustain_pedal_cc) == Synth::MixRole::none);
    }

    // T3: the runtime validator admits assignable CC inputs (16..31) on channel
    // chains and the commit hands source, CC number and chain to the writer.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const slot  = enabled_effect(bank, 3, 0, Synth::EffectType::delay);
        slot->bindings[0].num_inputs          = 2;
        slot->bindings[0].inputs[0].source    = Synth::ModSource::cc;
        slot->bindings[0].inputs[0].cc_number = 16;
        slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
        slot->bindings[0].inputs[0].scale     = 0.5f;
        slot->bindings[0].inputs[1].source    = Synth::ModSource::cc;
        slot->bindings[0].inputs[1].cc_number = 31;
        slot->bindings[0].inputs[1].op        = Synth::SourceOp::multiply;
        slot->bindings[0].inputs[1].scale     = 0.25f;
        static_assert(sizeof(Synth::ModInput) == 8); // INV2: the packed input stays 8 bytes

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(writer.num_resolved == 2);
        TEST(writer.resolved[0].source == Synth::ModSource::cc);
        TEST(writer.resolved[0].cc_number == 16);
        TEST(writer.resolved[0].chain == 3);
        TEST(writer.resolved[1].source == Synth::ModSource::cc);
        TEST(writer.resolved[1].cc_number == 31);
        TEST(writer.resolved[1].chain == 3);
        TEST(writer.dest_count == 1);
        TEST(writer.last_dest_source_param_id == 77); // resolved through the real writer path
        (void)Synth::take_effect_clear_ranges();

        // The same rules reject out-of-range or misplaced CC state.
        auto expect_preflight_reject = [](Synth::InstrumentBank& rejected) {
            Synth::init_effect_state_region(fake_region_base);
            Synth::EffectExpansionPlan reject_plan  = {};
            const char*                reject_error = nullptr;
            TEST(! Synth::preflight_effect_expansion(rejected, &reject_plan, &reject_error));
        };
        const std::unique_ptr<Synth::InstrumentBank> reject_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       reject_bank         = *reject_bank_storage;
        make_cc_input_bank(reject_bank, 3, Synth::ModSource::cc, 15);
        expect_preflight_reject(reject_bank);
        make_cc_input_bank(reject_bank, 3, Synth::ModSource::cc, 32);
        expect_preflight_reject(reject_bank);
        make_cc_input_bank(reject_bank, 3, Synth::ModSource::cc, 0);
        expect_preflight_reject(reject_bank);
        make_cc_input_bank(reject_bank, 3, Synth::ModSource::mod_wheel, 17); // non-CC, stale number
        expect_preflight_reject(reject_bank);

        // cc is not admissible as an LFO depth/rate source (no number to carry).
        make_valid_bank(reject_bank);
        {
            Synth::EffectSlotBinding* const cc_slot = enabled_effect(reject_bank, 3, 0, Synth::EffectType::delay);
            cc_slot->bindings[0].lfo_desc_id        = 1;
            cc_slot->bindings[0].lfo_depth          = 0.1f;
            cc_slot->bindings[0].lfo_depth_source   = Synth::ModSource::cc;
        }
        expect_preflight_reject(reject_bank);
        make_valid_bank(reject_bank);
        {
            Synth::EffectSlotBinding* const cc_slot = enabled_effect(reject_bank, 3, 0, Synth::EffectType::delay);
            cc_slot->bindings[0].lfo_desc_id        = 1;
            cc_slot->bindings[0].lfo_depth          = 0.1f;
            cc_slot->bindings[0].lfo_rate_source    = Synth::ModSource::cc;
        }
        expect_preflight_reject(reject_bank);
    }

    // T4: the bank JSON codec round-trips CC inputs; defaults stay omitted and
    // the decoder refuses invalid cc_number values.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        Synth::EffectChainBinding& chain = source.bank.channel_chains[0];
        chain.num_effects                = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
        chain.effects[0].enabled                         = true;
        chain.effects[0].bindings[0].num_inputs          = 1;
        chain.effects[0].bindings[0].inputs[0].source    = Synth::ModSource::cc;
        chain.effects[0].bindings[0].inputs[0].cc_number = 17;
        chain.effects[0].bindings[0].inputs[0].op        = Synth::SourceOp::add;
        chain.effects[0].bindings[0].inputs[0].scale     = 0.5f;

        const std::unique_ptr<FixedArray<char, 64 * 1024>> text_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&text)[64 * 1024] = text_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(len > 0);
        text[len] = '\0';
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        TEST(Synth::decode_editor_bank_json(text, len, &decoded));
        const Synth::ModInput& round_tripped = decoded.bank.channel_chains[0].effects[0].bindings[0].inputs[0];
        TEST(round_tripped.source == Synth::ModSource::cc);
        TEST(round_tripped.cc_number == 17);
        TEST(round_tripped.op == Synth::SourceOp::add);
        TEST(round_tripped.scale == 0.5f);
        TEST(Synth::validate_instrument_bank(&decoded.bank));

        // A legacy pitch_bend input encodes without any cc_number key.
        const std::unique_ptr<Synth::InstrumentEditorBank> legacy_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& legacy = *legacy_storage;
        memset(static_cast<void*>(&legacy), 0, sizeof(legacy));
        Synth::init_default_bank(&legacy.bank);
        Sculptor::set_identity_osc_sources(&legacy, 0, 0);
        Synth::EffectChainBinding& legacy_chain = legacy.bank.channel_chains[0];
        legacy_chain.num_effects                = 1;
        Sculptor::fx_init_slot(&legacy_chain, 0, Synth::EffectType::delay);
        legacy_chain.effects[0].enabled                      = true;
        legacy_chain.effects[0].bindings[0].num_inputs       = 1;
        legacy_chain.effects[0].bindings[0].inputs[0].source = Synth::ModSource::pitch_bend;
        legacy_chain.effects[0].bindings[0].inputs[0].op     = Synth::SourceOp::multiply;
        legacy_chain.effects[0].bindings[0].inputs[0].scale  = 0.25f;
        const uint32_t legacy_len = Synth::encode_editor_bank_json(&legacy, text, sizeof(text));
        TEST(legacy_len > 0);
        text[legacy_len] = '\0';
        TEST(strstr(text, "cc_number") == nullptr);
        TEST(Synth::decode_editor_bank_json(text, legacy_len, &decoded));
        TEST(decoded.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number == 0);
        TEST(Synth::validate_instrument_bank(&decoded.bank));

        // An out-of-range cc_number: decode implies validation, so the editor
        // bank decoder itself refuses the rogue bank during its internal
        // validate pass; the validator refuses it by any other route too.
        const std::unique_ptr<Synth::InstrumentEditorBank> rogue_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& rogue                                      = *rogue_storage;
        rogue                                                                   = legacy;
        rogue.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source    = Synth::ModSource::cc;
        rogue.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number = 40;
        const uint32_t rogue_len = Synth::encode_editor_bank_json(&rogue, text, sizeof(text));
        TEST(rogue_len > 0);
        TEST(! Synth::decode_editor_bank_json(text, rogue_len, &decoded));
        TEST(! Synth::validate_instrument_bank(&rogue.bank));
        // Malformed cc_number values must fail the decoder itself, never wrap:
        // the decode bound is 0x7F because controller numbers are 7-bit. A
        // widened bound plus uint8_t narrowing would still alias (65552 &
        // 0xFF == 16, 256 & 0xFF == 0), which is what the 65536/65552 rows pin;
        // 281 (281 & 0xFF == 25, an assignable CC) pins the decode bound
        // itself: it is refused by 0x7F but would decode under a widened
        // 0xFFFF bound. A failed decode leaves the output untouched.
        rogue.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number = 16;
        const uint32_t wrap_len = Synth::encode_editor_bank_json(&rogue, text, sizeof(text));
        TEST(wrap_len > 0);
        text[wrap_len]                                  = '\0';
        static const char* const malformed_cc_numbers[] = { "-1", "16.5", "128", "281", "65536", "65552" };
        char                     cc_number_key[]        = "\"cc_number\":16";
        for (uint32_t cc_value = 0; cc_value < sizeof(malformed_cc_numbers) / sizeof(malformed_cc_numbers[0]);
             ++cc_value) {
            const char* const field = strstr(text, cc_number_key);
            TEST(field != nullptr);
            const std::unique_ptr<FixedArray<char, 64 * 1024>> spliced_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&spliced)[64 * 1024] = spliced_storage->elements;
            const uint32_t prefix_len  = static_cast<uint32_t>(field - text);
            memcpy(spliced, text, prefix_len);
            const int written = snprintf(spliced + prefix_len,
                                         sizeof(spliced) - prefix_len,
                                         "\"cc_number\":%s",
                                         malformed_cc_numbers[cc_value]);
            TEST(written > 0);
            const uint32_t tail_len = wrap_len - prefix_len - static_cast<uint32_t>(sizeof(cc_number_key)) + 1;
            memcpy(spliced + prefix_len + written, field + sizeof(cc_number_key) - 1, tail_len);
            const uint32_t spliced_len = prefix_len + static_cast<uint32_t>(written) + tail_len;
            const std::unique_ptr<Synth::InstrumentEditorBank> prior_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& prior = *prior_storage;
            prior                              = decoded;
            TEST(! Synth::decode_editor_bank_json(spliced, spliced_len, &decoded));
            TEST(memcmp(&decoded, &prior, sizeof(decoded)) == 0);
        }
    }

    // Piano-type sustain bank JSON: decode normalization and the
    // release_seconds/ping_pong round trip.
    {
        Synth::Instrument         instr;
        Synth::EnvelopeDescriptor envs[Synth::instrument_max_envelopes];
        Synth::LFODescriptor      lfos[Synth::instrument_max_lfos];
        uint32_t                  env_count = 0;
        uint32_t                  lfo_count = 0;

        // Four-point volume envelope, span [1, 3], unequal endpoints.
        static const char unequal_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1},"
            "{\"time_seconds\":0.2,\"level\":0.5},{\"time_seconds\":0.3,\"level\":0.25}],"
            "\"sustain_start\":1,\"sustain_end\":3}}}]}";

        // With ping-pong off, the decoded multi-point span loads with the
        // span's last point value forced to the first point's value.
        TEST(Synth::decode_instrument_json(unequal_doc,
                                           sizeof(unequal_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(env_count == 1);
        TEST(envs[0].sustain_first_point == 1 && envs[0].sustain_last_point == 3);
        TEST(envs[0].points[3].value == envs[0].points[1].value);
        TEST(envs[0].points[2].value != envs[0].points[1].value); // interior points stay authored

        // Edge case: only sustain_end set at the final point defaults to a
        // single-point span, so nothing is snapped.
        static const char end_only_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1},"
            "{\"time_seconds\":0.2,\"level\":0.5},{\"time_seconds\":0.3,\"level\":0.25}],"
            "\"sustain_end\":3}}}]}";
        TEST(Synth::decode_instrument_json(end_only_doc,
                                           sizeof(end_only_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(envs[0].sustain_first_point == 3 && envs[0].sustain_last_point == 3);
        // The final point keeps its authored level (0.25 -> 0x4000): a snap to
        // the span start (level 0) or to the first point (level 1) would fail.
        TEST(envs[0].points[3].value == 0x4000);
        TEST(envs[0].points[3].value != envs[0].points[0].value);

        // Edge case: only sustain_start set below the final point forms an
        // effective multi-point span, so the snap fires.
        static const char start_only_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1},"
            "{\"time_seconds\":0.2,\"level\":0.5},{\"time_seconds\":0.3,\"level\":0.25}],"
            "\"sustain_start\":1}}}]}";
        TEST(Synth::decode_instrument_json(start_only_doc,
                                           sizeof(start_only_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(envs[0].sustain_first_point == 1 && envs[0].sustain_last_point == 3);
        TEST(envs[0].points[3].value == envs[0].points[1].value);

        // Ping-pong on preserves the authored unequal endpoints.
        static const char ping_pong_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1},"
            "{\"time_seconds\":0.2,\"level\":0.5},{\"time_seconds\":0.3,\"level\":0.25}],"
            "\"sustain_start\":1,\"sustain_end\":3,\"ping_pong\":1}}}]}";
        TEST(Synth::decode_instrument_json(ping_pong_doc,
                                           sizeof(ping_pong_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST((envs[0].flags & Synth::envelope_ping_pong) != 0);
        TEST(envs[0].points[3].value != envs[0].points[1].value);

        // Re-encode keeps the normalized value: decode the unequal document
        // again, then round-trip the normalized envelope through a
        // descriptor bank.
        TEST(Synth::decode_instrument_json(unequal_doc,
                                           sizeof(unequal_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        // Heap storage: an InstrumentBank is far too large for the stack.
        const std::unique_ptr<Synth::InstrumentBank> desc_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       desc_bank         = *desc_bank_storage;
        memset(static_cast<void*>(&desc_bank), 0, sizeof(desc_bank));
        desc_bank.envelopes.num_allocated = env_count;
        for (uint32_t i = 0; i < env_count; i++) {
            desc_bank.envelopes.entries[i] = envs[i];
        }
        char           doc_text[8192] = {}; // static: 8 KB is too much stack
        const uint32_t doc_len        = Synth::encode_instrument_json(doc_text, sizeof(doc_text), &instr, &desc_bank);
        TEST(doc_len > 0);
        TEST(Synth::decode_instrument_json(doc_text, doc_len, &instr, envs, &env_count, lfos, &lfo_count));
        TEST(envs[0].points[3].value == envs[0].points[1].value);

        // The release and ping-pong fields round-trip; absent fields
        // decode as 0; invalid values are refused.
        char defaults_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}]}";
        TEST(Synth::decode_instrument_json(defaults_doc,
                                           sizeof(defaults_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(instr.release_seconds == 0.0f);

        static const char release_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"release_seconds\":12.5,"
            "\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,\"envelope\":{"
            "\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1}],"
            "\"sustain_start\":1,\"sustain_end\":1,\"ping_pong\":1}}}]}";
        TEST(Synth::decode_instrument_json(release_doc,
                                           sizeof(release_doc) - 1,
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(instr.release_seconds == 12.5f);
        TEST(env_count == 1 && (envs[0].flags & Synth::envelope_ping_pong) != 0);
        desc_bank.envelopes.num_allocated = env_count;
        for (uint32_t i = 0; i < env_count; i++) {
            desc_bank.envelopes.entries[i] = envs[i];
        }
        const uint32_t release_len = Synth::encode_instrument_json(doc_text, sizeof(doc_text), &instr, &desc_bank);
        TEST(release_len > 0);
        TEST(Synth::decode_instrument_json(doc_text, release_len, &instr, envs, &env_count, lfos, &lfo_count));
        TEST(instr.release_seconds == 12.5f);
        TEST((envs[0].flags & Synth::envelope_ping_pong) != 0);

        // The decoder refuses a negative, oversized or non-finite release and
        // a ping-pong flag above 1.
        static const char negative_release_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"release_seconds\":-1,\"layers\":[{\"wave_a\":\"sine\"}]}";
        TEST(! Synth::decode_instrument_json(negative_release_doc,
                                             sizeof(negative_release_doc) - 1,
                                             &instr,
                                             envs,
                                             &env_count,
                                             lfos,
                                             &lfo_count));
        static const char oversized_release_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"release_seconds\":3601,\"layers\":[{\"wave_a\":\"sine\"}]}";
        TEST(! Synth::decode_instrument_json(oversized_release_doc,
                                             sizeof(oversized_release_doc) - 1,
                                             &instr,
                                             envs,
                                             &env_count,
                                             lfos,
                                             &lfo_count));
        static const char infinite_release_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"release_seconds\":1e400,\"layers\":[{\"wave_a\":\"sine\"}]}";
        TEST(! Synth::decode_instrument_json(infinite_release_doc,
                                             sizeof(infinite_release_doc) - 1,
                                             &instr,
                                             envs,
                                             &env_count,
                                             lfos,
                                             &lfo_count));
        static const char ping_pong_two_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1}],"
            "\"sustain_start\":1,\"sustain_end\":1,\"ping_pong\":2}}}]}";
        TEST(! Synth::decode_instrument_json(ping_pong_two_doc,
                                             sizeof(ping_pong_two_doc) - 1,
                                             &instr,
                                             envs,
                                             &env_count,
                                             lfos,
                                             &lfo_count));
        static const char quick_release_two_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1,"
            "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.1,\"level\":1}],"
            "\"sustain_start\":1,\"sustain_end\":1,\"quick_release\":2}}}]}";
        TEST(! Synth::decode_instrument_json(quick_release_two_doc,
                                             sizeof(quick_release_two_doc) - 1,
                                             &instr,
                                             envs,
                                             &env_count,
                                             lfos,
                                             &lfo_count));

        // Pool (editor bank) form: the fields round-trip and the absent
        // defaults stay 0.
        const std::unique_ptr<Synth::InstrumentEditorBank> pool_source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& pool_source = *pool_source_storage;
        build_named_fm_fixture(&pool_source);
        pool_source.bank.instruments.entries[0].release_seconds = 3.5f;
        pool_source.bank.envelopes.entries[0].flags             = Synth::envelope_ping_pong;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> pool_payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&pool_payload)[Synth::library_payload_max] = pool_payload_storage->elements;
        uint32_t pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> pool_decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& pool_decoded = *pool_decoded_storage;
        TEST(Synth::decode_editor_bank_json(pool_payload, pool_len, &pool_decoded));
        TEST(pool_decoded.bank.instruments.entries[0].release_seconds == 3.5f);
        TEST((pool_decoded.bank.envelopes.entries[0].flags & Synth::envelope_ping_pong) != 0);

        // Pool form: a multi-point span with unequal endpoints, ping-pong
        // off, decodes normalized.
        Synth::EnvelopeDescriptor& pool_env = pool_source.bank.envelopes.entries[0];
        pool_env.flags                      = 0;
        pool_env.sustain_first_point        = 0;
        pool_env.sustain_last_point         = 1;
        pool_env.points[0].value            = 0x2000;
        pool_env.points[1].value            = 0x4000;
        TEST(pool_env.num_points == 2); // the default fixture envelope carries two points
        pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        TEST(Synth::decode_editor_bank_json(pool_payload, pool_len, &pool_decoded));
        TEST(pool_decoded.bank.envelopes.entries[0].points[1].value ==
             pool_decoded.bank.envelopes.entries[0].points[0].value);

        // Pool form: absent fields decode as 0.
        pool_source.bank.instruments.entries[0].release_seconds = 0.0f;
        pool_env.flags                                          = 0;
        pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        TEST(Synth::decode_editor_bank_json(pool_payload, pool_len, &pool_decoded));
        TEST(pool_decoded.bank.instruments.entries[0].release_seconds == 0.0f);
        TEST((pool_decoded.bank.envelopes.entries[0].flags & Synth::envelope_ping_pong) == 0);

        // Pool form: the decoder refuses an out-of-range release.  Each splice
        // starts from a fresh encode and rewrites only the encoded number, so
        // the payload stays valid JSON and the value rejection itself is what
        // fails the decode.
        pool_source.bank.instruments.entries[0].release_seconds = 3.5f;
        pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        char* number_field = strstr(pool_payload, ":3.5}");
        TEST(number_field != nullptr);
        if (number_field) {
            // "-1" is one byte shorter than "3.5": shift the payload tail left.
            memmove(number_field + 3, number_field + 4, strlen(number_field + 4) + 1);
            memcpy(number_field + 1, "-1", 2);
            TEST(! Synth::decode_editor_bank_json(pool_payload, pool_len - 1, &pool_decoded));
        }
        pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        number_field = strstr(pool_payload, ":3.5}");
        TEST(number_field != nullptr);
        if (number_field) {
            memcpy(number_field + 1, "1e4", 3); // 10000 s exceeds the bound
            TEST(! Synth::decode_editor_bank_json(pool_payload, pool_len, &pool_decoded));
        }

        // A negative zero release decodes into the canonical positive zero:
        // the bounds accept -0.0, so canonicalization owns the sign.
        pool_len = Synth::encode_editor_bank_json(&pool_source, pool_payload, sizeof(pool_payload));
        TEST(pool_len > 0);
        number_field = strstr(pool_payload, ":3.5}");
        TEST(number_field != nullptr);
        if (number_field) {
            memmove(number_field + 3, number_field + 4, strlen(number_field + 4) + 1); // "-0" is one byte shorter
            memcpy(number_field + 1, "-0", 2);
            TEST(Synth::decode_editor_bank_json(pool_payload, pool_len - 1, &pool_decoded));
            TEST(pool_decoded.bank.instruments.entries[0].release_seconds == 0.0f);
            TEST(! std::signbit(pool_decoded.bank.instruments.entries[0].release_seconds));
        }
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> pool_reencoded_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&pool_reencoded)[Synth::library_payload_max] = pool_reencoded_storage->elements;
        const uint32_t reencoded_len =
            Synth::encode_editor_bank_json(&pool_decoded, pool_reencoded, sizeof(pool_reencoded));
        TEST(reencoded_len > 0);
        TEST(strstr(pool_reencoded, "release_seconds") == nullptr); // zero is omitted on encode
    }

    // A standalone-parameter LFO carries full 16-bit depth/rate parameter ids (they
    // are ParamDescriptor ids, not 7-bit controller numbers): ids above 127 decode
    // and round-trip byte-identically. Regression: the decoder once applied the
    // 7-bit cc_number bound here, rejecting old banks whose ids exceeded 127.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const uint32_t param = source.bank.parameters.allocate();
        TEST(param != pool_no_slot);
        Synth::LFODescriptor lfo = {};
        lfo.wave                 = Synth::WaveType::sine_wave;
        lfo.period_ms            = 50;
        Synth::configure_lfo(&source.bank.parameters.entries[param], lfo, Synth::SourceOp::add, 0.5f, 200, 300, 0.0f);
        TEST(Synth::validate_instrument_bank(&source.bank));
        const std::unique_ptr<FixedArray<char, 64 * 1024>> text_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&text)[64 * 1024] = text_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(len > 0);
        text[len] = '\0';
        TEST(strstr(text, "\"depth_param_id\":200") != nullptr);
        TEST(strstr(text, "\"rate_param_id\":300") != nullptr);
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        TEST(Synth::decode_editor_bank_json(text, len, &decoded));
        const Synth::ParamDescriptor& decoded_param = decoded.bank.parameters.entries[param];
        TEST(decoded_param.kind == Synth::ParamKind::lfo);
        TEST(decoded_param.lfo.depth_param_id == 200);
        TEST(decoded_param.lfo.rate_param_id == 300);
        TEST(Synth::validate_instrument_bank(&decoded.bank));
        const std::unique_ptr<FixedArray<char, 64 * 1024>> again_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&again)[64 * 1024]  = again_storage->elements;
        const uint32_t second_len = Synth::encode_editor_bank_json(&decoded, again, sizeof(again));
        TEST(second_len == len);
        TEST(memcmp(text, again, len) == 0);
    }
    // T5: banks saved before the feature load identically and re-encode
    // byte-identically (the cc_number field stays omitted for zero).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        Synth::EffectChainBinding& chain = source.bank.channel_chains[0];
        chain.num_effects                = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
        chain.effects[0].enabled                      = true;
        chain.effects[0].bindings[0].num_inputs       = 1;
        chain.effects[0].bindings[0].inputs[0].source = Synth::ModSource::pitch_bend;
        chain.effects[0].bindings[0].inputs[0].op     = Synth::SourceOp::multiply;
        chain.effects[0].bindings[0].inputs[0].scale  = 0.25f;

        const std::unique_ptr<FixedArray<char, 64 * 1024>> text_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&text)[64 * 1024]  = text_storage->elements;
        const uint32_t first_len = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(first_len > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        TEST(Synth::decode_editor_bank_json(text, first_len, &decoded));
        const Synth::ModInput& input = decoded.bank.channel_chains[0].effects[0].bindings[0].inputs[0];
        TEST(input.source == Synth::ModSource::pitch_bend);
        TEST(input.cc_number == 0);
        TEST(input.op == Synth::SourceOp::multiply);
        TEST(input.scale == 0.25f);
        TEST(Synth::validate_instrument_bank(&decoded.bank));

        const std::unique_ptr<FixedArray<char, 64 * 1024>> again_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&again)[64 * 1024]  = again_storage->elements;
        const uint32_t second_len = Synth::encode_editor_bank_json(&decoded, again, sizeof(again));
        TEST(second_len == first_len);
        TEST(memcmp(text, again, first_len) == 0);
    }

    // T6: the editor bank validator mirrors the runtime rules.
    {
        auto make_cc_bank = [&](Synth::InstrumentBank& target, uint8_t cc_number, bool master) {
            make_valid_bank(target);
            Synth::EffectSlotBinding* const cc_slot =
                enabled_effect(target, master ? Synth::max_channels : 3, 0, Synth::EffectType::delay);
            cc_slot->bindings[0].num_inputs          = 1;
            cc_slot->bindings[0].inputs[0].source    = Synth::ModSource::cc;
            cc_slot->bindings[0].inputs[0].cc_number = cc_number;
            cc_slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
            cc_slot->bindings[0].inputs[0].scale     = 0.5f;
        };
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_cc_bank(bank, 15, false);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_bank(bank, 32, false);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_bank(bank, 16, false);
        TEST(Synth::validate_instrument_bank(&bank));
        make_cc_bank(bank, 31, false);
        TEST(Synth::validate_instrument_bank(&bank));
        make_cc_bank(bank, 16, true); // master chains admit CC inputs
        TEST(Synth::validate_instrument_bank(&bank));

        // cc is not admissible as an LFO depth/rate source (no number to carry).
        make_valid_bank(bank);
        {
            Synth::EffectSlotBinding* const slot = enabled_effect(bank, 3, 0, Synth::EffectType::delay);
            slot->bindings[0].lfo_desc_id        = 1;
            slot->bindings[0].lfo_depth          = 0.1f;
            slot->bindings[0].lfo_depth_source   = Synth::ModSource::cc;
        }
        TEST(! Synth::validate_instrument_bank(&bank));
        make_valid_bank(bank);
        {
            Synth::EffectSlotBinding* const slot = enabled_effect(bank, 3, 0, Synth::EffectType::delay);
            slot->bindings[0].lfo_desc_id        = 1;
            slot->bindings[0].lfo_depth          = 0.1f;
            slot->bindings[0].lfo_rate_source    = Synth::ModSource::cc;
        }
        TEST(! Synth::validate_instrument_bank(&bank));

        // A non-CC input must carry no stale CC number.
        make_valid_bank(bank);
        {
            Synth::EffectSlotBinding* const slot  = enabled_effect(bank, 3, 0, Synth::EffectType::delay);
            slot->bindings[0].num_inputs          = 1;
            slot->bindings[0].inputs[0].source    = Synth::ModSource::mod_wheel;
            slot->bindings[0].inputs[0].cc_number = 17;
            slot->bindings[0].inputs[0].op        = Synth::SourceOp::multiply;
            slot->bindings[0].inputs[0].scale     = 0.25f;
        }
        TEST(! Synth::validate_instrument_bank(&bank));

        // Instrument routing refuses stale CC numbers on non-CC inputs, and the
        // instrument document encoder never emits cc_number for a non-CC input.
        const char* const         stale_doc                                   = "{"
                                                                                "\"format\":\"synth-instrument-v1\","
                                                                                "\"layers\":["
                                                                                "{"
                                                                                "\"wave_a\":\"sine\","
                                                                                "\"volume\":{"
                                                                                "\"base\":0.8,"
                                                                                "\"sources\":[{\"source\":\"velocity\",\"op\":\"add\",\"scale\":0.5,"
                                                                                "\"cc_number\":17}]"
                                                                                "}"
                                                                                "}"
                                                                                "]}";
        Synth::Instrument         stale_instr                                 = {};
        Synth::EnvelopeDescriptor stale_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      stale_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  stale_env_count                             = 0;
        uint32_t                  stale_lfo_count                             = 0;
        // Decode implies validation: the instrument document decoder refuses the
        // stale CC state during its internal validate pass
        // (map_instrument_graph_layout runs validate_instrument_bank), so the
        // decode itself refuses instead of handing back an unvalidated instrument.
        TEST(! Synth::decode_instrument_json(stale_doc,
                                             static_cast<uint32_t>(strlen(stale_doc)),
                                             &stale_instr,
                                             stale_envs,
                                             &stale_env_count,
                                             stale_lfos,
                                             &stale_lfo_count));

        // The validator still refuses the stale state when it arrives by any
        // other route (a hand-authored bank), and the encoder refuses to emit it.
        stale_instr             = {};
        stale_instr.layer_count = 1;
        set_default_dispersion_sections(stale_instr);
        stale_instr.layers[0].targets[Synth::mod_volume].num_inputs          = 1;
        stale_instr.layers[0].targets[Synth::mod_volume].inputs[0].source    = Synth::ModSource::velocity;
        stale_instr.layers[0].targets[Synth::mod_volume].inputs[0].cc_number = 17;
        stale_instr.layers[0].targets[Synth::mod_volume].inputs[0].op        = Synth::SourceOp::add;
        stale_instr.layers[0].targets[Synth::mod_volume].inputs[0].scale     = 0.5f;
        const std::unique_ptr<Synth::InstrumentBank> stale_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       stale_bank         = *stale_bank_storage;
        memset(static_cast<void*>(&stale_bank), 0, sizeof(stale_bank));
        uint16_t stale_env_ids[Synth::instrument_max_envelopes] = {};
        uint16_t stale_lfo_ids[Synth::instrument_max_lfos]      = {};
        TEST(Synth::remap_envelopes(&stale_bank, stale_envs, stale_env_count, stale_env_ids));
        TEST(Synth::remap_lfos(&stale_bank, stale_lfos, stale_lfo_count, stale_lfo_ids));
        Synth::Instrument stale_installed;
        Synth::remap_instrument(stale_instr, stale_env_ids, stale_lfo_ids, &stale_installed);
        const uint32_t stale_slot = stale_bank.instruments.allocate();
        TEST(stale_slot != pool_no_slot);
        stale_bank.instruments.entries[stale_slot] = stale_installed;
        stale_bank.channel_zones[0][0].start_note  = 1;
        stale_bank.channel_zones[0][0].instrument  = static_cast<uint8_t>(stale_slot);
        TEST(! Synth::validate_instrument_bank(&stale_bank));

        // The document encoder refuses (or at least never emits) a stale CC
        // number on a non-CC instrument input.
        char           stale_encoded[16 * 1024] = {};
        const uint32_t stale_len =
            Synth::encode_instrument_json(stale_encoded, sizeof(stale_encoded), &stale_installed, &stale_bank);
        TEST(stale_len == 0 || strstr(stale_encoded, "cc_number") == nullptr);
    }

    // ---------------------------------------------------------------------------
    // Master chain tests: the master chain admits the standard MIDI node
    // sources and CC 16-31, all resolved from channel index 0; the editor
    // validator, projection, adoption and layout machinery follow the same rules.
    // ---------------------------------------------------------------------------

    // T-M2-1: the commit resolves master standard sources from channel index 0
    // (RC4): the recorded chain is 0, not the master chain index.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const slot = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        slot->bindings[0].num_inputs         = 1;
        slot->bindings[0].inputs[0].source   = Synth::ModSource::mod_wheel;
        slot->bindings[0].inputs[0].op       = Synth::SourceOp::add;
        slot->bindings[0].inputs[0].scale    = 0.5f;
        slot->bindings[1].num_inputs         = 2;
        slot->bindings[1].inputs[0].source   = Synth::ModSource::pitch_bend;
        slot->bindings[1].inputs[0].op       = Synth::SourceOp::add;
        slot->bindings[1].inputs[0].scale    = 0.5f;
        slot->bindings[1].inputs[1].source   = Synth::ModSource::channel_pressure;
        slot->bindings[1].inputs[1].op       = Synth::SourceOp::multiply;
        slot->bindings[1].inputs[1].scale    = 0.25f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan     = {};
        const char*                error    = nullptr;
        const bool                 accepted = Synth::preflight_effect_expansion(bank, &plan, &error);
        TEST(accepted);
        if (accepted) {
            Synth::EffectChain chains[Synth::max_channels] = {};
            Synth::EffectChain master                      = {};
            FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
            TEST(writer.num_resolved == 3);
            TEST(writer.resolved[0].source == Synth::ModSource::mod_wheel);
            TEST(writer.resolved[0].chain == 0); // master resolves from channel index 0
            TEST(writer.resolved[1].source == Synth::ModSource::pitch_bend);
            TEST(writer.resolved[1].chain == 0);
            TEST(writer.resolved[2].source == Synth::ModSource::channel_pressure);
            TEST(writer.resolved[2].chain == 0);
            TEST(writer.dest_count == 2);
            TEST(writer.last_dest_source_param_id == 77); // the fake source node id
            TEST(master.effects[0].src_param_id[0] == 1 && master.effects[0].src_param_id[1] == 2);
            (void)Synth::take_effect_clear_ranges();
        }
    }

    // T-M2-2: master CC inputs resolve from channel index 0 while a channel
    // chain in the same bank keeps resolving from its own channel.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const master_slot =
            enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
        master_slot->bindings[0].num_inputs           = 1;
        master_slot->bindings[0].inputs[0].source     = Synth::ModSource::cc;
        master_slot->bindings[0].inputs[0].cc_number  = 16;
        master_slot->bindings[0].inputs[0].op         = Synth::SourceOp::add;
        master_slot->bindings[0].inputs[0].scale      = 0.5f;
        master_slot->bindings[1].num_inputs           = 1;
        master_slot->bindings[1].inputs[0].source     = Synth::ModSource::cc;
        master_slot->bindings[1].inputs[0].cc_number  = 31;
        master_slot->bindings[1].inputs[0].op         = Synth::SourceOp::multiply;
        master_slot->bindings[1].inputs[0].scale      = 0.25f;
        Synth::EffectSlotBinding* const channel_slot  = enabled_effect(bank, 3, 0, Synth::EffectType::delay);
        channel_slot->bindings[0].num_inputs          = 1;
        channel_slot->bindings[0].inputs[0].source    = Synth::ModSource::cc;
        channel_slot->bindings[0].inputs[0].cc_number = 20;
        channel_slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
        channel_slot->bindings[0].inputs[0].scale     = 0.5f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan     = {};
        const char*                error    = nullptr;
        const bool                 accepted = Synth::preflight_effect_expansion(bank, &plan, &error);
        TEST(accepted);
        if (accepted) {
            Synth::EffectChain chains[Synth::max_channels] = {};
            Synth::EffectChain master                      = {};
            FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
            TEST(writer.num_resolved == 3);
            TEST(writer.resolved[0].source == Synth::ModSource::cc); // channel 3 commits first
            TEST(writer.resolved[0].cc_number == 20);
            TEST(writer.resolved[0].chain == 3);
            TEST(writer.resolved[1].source == Synth::ModSource::cc);
            TEST(writer.resolved[1].cc_number == 16);
            TEST(writer.resolved[1].chain == 0); // master resolves from channel index 0
            TEST(writer.resolved[2].cc_number == 31);
            TEST(writer.resolved[2].chain == 0);
            TEST(writer.dest_count == 3); // one dest per modulated param: channel 3's and the master's two
            (void)Synth::take_effect_clear_ranges();
        }
    }

    // T-M2-3: master LFO depth/rate sources follow the channel-chain rules
    // (full relaxation): both validators admit them and the commit resolves
    // them from channel index 0.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const slot = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
        slot->bindings[0].lfo_desc_id        = 1;
        slot->bindings[0].lfo_op             = Synth::SourceOp::add;
        slot->bindings[0].lfo_depth          = 0.25f;
        slot->bindings[0].lfo_depth_source   = Synth::ModSource::mod_wheel;
        slot->bindings[0].lfo_rate_source    = Synth::ModSource::none;
        TEST(Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan     = {};
        const char*                error    = nullptr;
        const bool                 accepted = Synth::preflight_effect_expansion(bank, &plan, &error);
        TEST(accepted);
        if (accepted) {
            Synth::EffectChain chains[Synth::max_channels] = {};
            Synth::EffectChain master                      = {};
            FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
            TEST(writer.num_resolved == 2);
            TEST(writer.resolved[0].source == Synth::ModSource::mod_wheel); // depth
            TEST(writer.resolved[0].chain == 0);
            TEST(writer.resolved[1].source == Synth::ModSource::none); // rate
            TEST(writer.resolved[1].chain == 0);
            TEST(writer.dest_count == 1);
            (void)Synth::take_effect_clear_ranges();
        }
    }

    // T-M2-4: master rejections that survive M2 - per-note roles, out-of-range
    // CC numbers and stale CC numbers mirror the channel-chain rules, pinned in
    // both validators (preflight and the editor bank validator).
    {
        auto make_master_case =
            [&](Synth::InstrumentBank& target, bool as_input, uint8_t cc_number, Synth::ModSource source) {
                if (as_input) {
                    make_cc_input_bank(target, Synth::max_channels, source, cc_number);
                }
                else {
                    make_valid_bank(target);
                    Synth::EffectSlotBinding* const case_slot =
                        enabled_effect(target, Synth::max_channels, 0, Synth::EffectType::delay);
                    case_slot->bindings[0].lfo_desc_id      = 1;
                    case_slot->bindings[0].lfo_depth        = 0.1f;
                    case_slot->bindings[0].lfo_depth_source = source;
                }
            };
        auto expect_master_refused = [&](Synth::InstrumentBank& refused) {
            Synth::init_effect_state_region(fake_region_base);
            Synth::EffectExpansionPlan plan  = {};
            const char*                error = nullptr;
            TEST(! Synth::preflight_effect_expansion(refused, &plan, &error));
            TEST(! Synth::validate_instrument_bank(&refused));
        };
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        static const Synth::ModSource                per_note[]   = { Synth::ModSource::velocity,
                                                                      Synth::ModSource::aftertouch,
                                                                      Synth::ModSource::pressure_combine };
        for (uint32_t i = 0; i < sizeof(per_note) / sizeof(per_note[0]); ++i) {
            make_master_case(bank, true, 0, per_note[i]);
            expect_master_refused(bank);
            make_master_case(bank, false, 0, per_note[i]);
            expect_master_refused(bank);
        }
        uint8_t bad_numbers[] = { 0, 15, 32 };
        for (uint32_t i = 0; i < sizeof(bad_numbers) / sizeof(bad_numbers[0]); ++i) {
            make_master_case(bank, true, bad_numbers[i], Synth::ModSource::cc);
            expect_master_refused(bank);
        }
        make_master_case(bank, true, 17, Synth::ModSource::mod_wheel); // non-CC, stale number
        expect_master_refused(bank);
    }

    // T-M2-5: the editor bank validator mirrors the relaxed master rules:
    // standard and CC inputs are admitted on the master chain; per-note roles
    // and bad CC numbers stay refused, and cc stays inadmissible as an LFO
    // depth/rate source there.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::mod_wheel, 0);
        TEST(Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::cc, 16);
        TEST(Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::cc, 31);
        TEST(Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::velocity, 0);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::aftertouch, 0);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::pressure_combine, 0);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::cc, 15);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::cc, 32);
        TEST(! Synth::validate_instrument_bank(&bank));
        make_cc_input_bank(bank, Synth::max_channels, Synth::ModSource::mod_wheel, 17); // non-CC, stale number
        TEST(! Synth::validate_instrument_bank(&bank));
        // cc is not admissible as a master LFO depth/rate source (no number to carry).
        make_valid_bank(bank);
        {
            Synth::EffectSlotBinding* const slot =
                enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
            slot->bindings[0].lfo_desc_id      = 1;
            slot->bindings[0].lfo_depth        = 0.1f;
            slot->bindings[0].lfo_depth_source = Synth::ModSource::cc;
        }
        TEST(! Synth::validate_instrument_bank(&bank));
        make_valid_bank(bank);
        {
            Synth::EffectSlotBinding* const slot =
                enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
            slot->bindings[0].lfo_desc_id     = 1;
            slot->bindings[0].lfo_depth       = 0.1f;
            slot->bindings[0].lfo_rate_source = Synth::ModSource::cc;
        }
        TEST(! Synth::validate_instrument_bank(&bank));
    }

    // T-M2-6: the master chain projects with the standard MIDI node (ED2
    // editor half): three output dots, the same node count as an identical
    // channel chain, and a graph node named "MIDI Inputs" like on channel chains.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const master =
            enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        master->bindings[0].lfo_desc_id         = 1;
        master->bindings[0].lfo_depth           = 0.25f;
        Synth::EffectSlotBinding* const channel = enabled_effect(bank, 0, 0, Synth::EffectType::reverb);
        channel->bindings[0].lfo_desc_id        = 1;
        channel->bindings[0].lfo_depth          = 0.25f;
        TEST(Synth::validate_instrument_bank(&bank));
        bool pins[Synth::max_lfos] = {};
        pins[0]                    = true;
        TEST(Sculptor::fx_projected_node_count(bank, Sculptor::fx_master_chain, pins) ==
             Sculptor::fx_projected_node_count(bank, 0, pins));
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(project_legacy_effects(bank, Sculptor::fx_master_chain, &graph, &mapping));
        TEST(mapping.midi_node != Sculptor::pool_no_slot);
        if (mapping.midi_node != Sculptor::pool_no_slot) {
            for (uint32_t source = 0; source < Sculptor::fx_num_input_sources; ++source) {
                TEST(mapping.midi_source_slots[source] != Sculptor::pool_no_slot);    // the 3 standard dots
                TEST(strcmp(graph.node(mapping.midi_node).name, "MIDI Inputs") == 0); // master MIDI node identity
            }
        }
    }

    // A hand-authored CC input on master still projects with its row unwired,
    // and a source-row edit on that row refuses atomically.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const slot  = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
        slot->bindings[0].num_inputs          = 1;
        slot->bindings[0].inputs[0].source    = Synth::ModSource::cc;
        slot->bindings[0].inputs[0].cc_number = 16;
        slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
        slot->bindings[0].inputs[0].scale     = 0.5f;
        const std::unique_ptr<Sculptor::Graph> cc_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       cc_graph         = *cc_graph_storage;
        Sculptor::EffectGraphMapping           cc_mapping       = {};
        TEST(project_legacy_effects(bank, Sculptor::fx_master_chain, &cc_graph, &cc_mapping));
        if (cc_mapping.midi_node != Sculptor::pool_no_slot) {
            Sculptor::GraphChange cc_discard[Sculptor::max_pending_changes];
            (void)cc_graph.take_changes(cc_discard, Sculptor::max_pending_changes);
            Sculptor::GraphChange cc_row = {};
            cc_row.kind                  = Sculptor::ChangeKind::value_changed;
            cc_row.node_idx              = cc_mapping.effect_nodes[0];
            cc_row.slot_idx              = Sculptor::fx_param_src_dot(0, 0) + 2; // the cc row's Amount
            auto cc_value                = cc_graph.node(cc_row.node_idx).slots.entries[cc_row.slot_idx].value;
            cc_value.real                = 0.75f;
            cc_graph.set_slot_value(cc_row.node_idx, cc_row.slot_idx, cc_value);
            const std::unique_ptr<Synth::InstrumentBank> candidate_storage = std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank&                       candidate         = *candidate_storage;
            candidate                                                      = bank;
            const Synth::EffectParamBinding before                         = bank.master_chain.effects[0].bindings[0];
            TEST(! Sculptor::apply_fx_graph_change(&candidate, cc_graph, cc_mapping, cc_row));
            TEST(memcmp(&candidate.master_chain.effects[0].bindings[0], &before, sizeof(before)) == 0);
        }
    }

    // T-M2-7: adopting a channel document onto the master chain preserves the
    // routing: inputs, LFO sources and the midi layout record survive,
    // and the adopted master chain round-trips back out.
    {
        Synth::EffectsDocument                             doc       = {};
        Synth::EffectsDocument                             extracted = {};
        Synth::EffectsDocument                             rogue     = {};
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& candidate = *candidate_storage;
        effects_clipboard_fixture(&doc, true);
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t id            = source.bank.lfos.allocate();
            source.bank.lfos.entries[id] = doc.lfos[i];
        }
        Sculptor::fx_graph_chain(&source.bank, 5) = doc.chain; // the document's origin chain
        TEST(Sculptor::replace_effect_chain_candidate(&source, Sculptor::fx_master_chain, &doc, &candidate));
        TEST(Synth::validate_instrument_bank(&candidate.bank));
        const Synth::EffectChainBinding& adopted = candidate.bank.master_chain;
        for (uint32_t s = 0; s < 4; ++s) {
            for (uint32_t p = 0; p < Synth::get_effect_param_floats(adopted.effects[s].type); ++p) {
                const Synth::EffectParamBinding& b   = adopted.effects[s].bindings[p];
                const Synth::EffectParamBinding& old = doc.chain.effects[s].bindings[p];
                TEST(b.num_inputs == old.num_inputs);
                TEST(memcmp(b.inputs, old.inputs, sizeof(b.inputs)) == 0);
                TEST(b.lfo_depth_source == old.lfo_depth_source && b.lfo_rate_source == old.lfo_rate_source);
            }
        }
        TEST(Sculptor::extract_effect_chain_document(&candidate,
                                                     Sculptor::fx_master_chain,
                                                     nullptr,
                                                     nullptr,
                                                     &extracted));
        TEST(extracted.lfo_count == 3);
        TEST(extracted.graph_layout_count == doc.graph_layout_count); // midi record transfers

        // The negative: a per-note source in the document is refused by the
        // document validator before adoption, so master never sees it.
        rogue                                               = doc;
        rogue.chain.effects[0].bindings[0].inputs[0].source = Synth::ModSource::velocity;
        TEST(! Synth::validate_effects_document(&rogue));
        TEST(! Sculptor::replace_effect_chain_candidate(&source, Sculptor::fx_master_chain, &rogue, &candidate));
    }

    // T-M2-8: master wire edits through the graph (ED2): the MIDI node's dots
    // route onto master parameter rows like on any channel chain.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        (void)enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(project_legacy_effects(bank, Sculptor::fx_master_chain, &graph, &mapping));
        TEST(mapping.midi_node != Sculptor::pool_no_slot);
        Sculptor::GraphChange changes[Sculptor::max_pending_changes];
        (void)graph.take_changes(changes, Sculptor::max_pending_changes);
        Synth::EffectChainBinding& chain = bank.master_chain;
        // Connect the mod-wheel dot to param 0's Source A row.
        TEST(graph.attempt_connection({ mapping.midi_node, mapping.midi_source_slots[1] },
                                      { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }));
        const uint32_t count = graph.take_changes(changes, Sculptor::max_pending_changes);
        TEST(count == 1 && changes[0].kind == Sculptor::ChangeKind::connection_added);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, changes[0]));
        TEST(chain.effects[0].bindings[0].num_inputs == 1);
        TEST(chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(chain.effects[0].bindings[0].inputs[0].op == Synth::SourceOp::add);
        TEST(chain.effects[0].bindings[0].inputs[0].scale == 1.0f);
        TEST(Synth::validate_instrument_bank(&bank));
        // Disconnect removes the input again.
        const uint32_t wire = find_fx_graph_connection(graph,
                                                       { mapping.midi_node, mapping.midi_source_slots[1] },
                                                       { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) });
        TEST(wire != Sculptor::pool_no_slot);
        graph.delete_connection(wire);
        const uint32_t dropped = graph.take_changes(changes, Sculptor::max_pending_changes);
        TEST(dropped == 1 && changes[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, changes[0]));
        TEST(chain.effects[0].bindings[0].num_inputs == 0);
    }

    // T-M2-9: legacy master compatibility (DM3 as amended): legacy banks
    // without master routing round-trip byte-identically.  A legacy master
    // pane already at the node limit refuses cleanly because projection
    // includes the MIDI node; a pane one node below the limit keeps loading.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        Synth::EffectChainBinding& chain = source.bank.master_chain;
        chain.num_effects                = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
        chain.effects[0].enabled = true;
        const std::unique_ptr<FixedArray<char, 64 * 1024>> text_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&text)[64 * 1024] = text_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(len > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        TEST(Synth::decode_editor_bank_json(text, len, &decoded));
        TEST(Synth::validate_instrument_bank(&decoded.bank));
        const std::unique_ptr<FixedArray<char, 64 * 1024>> again_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&again)[64 * 1024]  = again_storage->elements;
        const uint32_t second_len = Synth::encode_editor_bank_json(&decoded, again, sizeof(again));
        TEST(second_len == len);
        TEST(memcmp(text, again, len) == 0);
    }
    {
        // The node ceiling (150) sits above every reachable fx projection
        // (3 fixed nodes + 4 effects + 128 pinned LFOs = 135), so a master
        // pane pinning the full LFO pool validates and projects.
        const std::unique_ptr<Synth::InstrumentEditorBank> at_ceiling_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& at_ceiling = *at_ceiling_storage;
        memset(static_cast<void*>(&at_ceiling), 0, sizeof(at_ceiling));
        Synth::init_default_bank(&at_ceiling.bank);
        Sculptor::set_identity_osc_sources(&at_ceiling, 0, 0);
        at_ceiling.bank.master_chain.num_effects = Synth::max_chain_effects;
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            Sculptor::fx_init_slot(&at_ceiling.bank.master_chain, slot, Synth::EffectType::delay);
            at_ceiling.bank.master_chain.effects[slot].enabled = true;
        }
        bool pins[Synth::max_lfos] = {};
        for (uint32_t index = 0; index < Synth::max_lfos; ++index) {
            uint16_t allocated = 0;
            TEST(Sculptor::allocate_default_lfo(&at_ceiling.bank, &allocated));
            Synth::GraphNodeLayout& record = at_ceiling.graph_layout[at_ceiling.graph_layout_count++];
            memset(static_cast<void*>(&record), 0, sizeof(record));
            record.kind    = 4;
            record.channel = static_cast<uint8_t>(Sculptor::fx_master_chain);
            snprintf(record.name, sizeof(record.name), "LFO %u", allocated);
        }
        TEST(Synth::validate_instrument_bank(&at_ceiling.bank));
        TEST(Sculptor::validate_editor_metadata(at_ceiling));
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(Sculptor::project_effect_chain_to_graph(at_ceiling, Sculptor::fx_master_chain, &graph, &mapping, pins));
    }

    // ---------------------------------------------------------------------------
    // Channel mix CCs: CC 7 writes the channel volume leaf and CC 10 the
    // channel panning leaf through the synth_midi_cc seam; the block-boundary
    // handoff feeds the existing old/new swap at the channel-to-master upload.
    // ---------------------------------------------------------------------------

    // T-M3-1: mix CC role mapping: CC 7 -> volume, CC 10 -> panning, others
    // -> none; the assignable-CC predicate does not claim CC 7 or CC 10.
    {
        TEST(Synth::mix_cc_role(7) == Synth::MixRole::volume);
        TEST(Synth::mix_cc_role(10) == Synth::MixRole::panning);
        uint32_t unmapped[] = { 1, 6, 11, 16, 48, 63, 119 };
        for (uint32_t i = 0; i < sizeof(unmapped) / sizeof(unmapped[0]); ++i) {
            TEST(Synth::mix_cc_role(unmapped[i]) == Synth::MixRole::none);
        }
        TEST(! Synth::is_assignable_cc(7));
        TEST(! Synth::is_assignable_cc(10));
    }

    // T-M3-2: mix CC leaf write: the addressed leaf gets data/127.0f in value
    // and prev_value together (zero lag), the other leaf stays untouched, and
    // a non-mix controller returns false without a write.
    {
        float                     volume_value       = -1.0f;
        float                     volume_prev_value  = -1.0f;
        float                     panning_value      = -1.0f;
        float                     panning_prev_value = -1.0f;
        const Synth::ExternalLeaf volume_leaf{ &volume_value, &volume_prev_value };
        const Synth::ExternalLeaf panning_leaf{ &panning_value, &panning_prev_value };

        TEST(Synth::write_mix_cc(7, 0, volume_leaf, panning_leaf));
        TEST(volume_value == 0.0f && volume_prev_value == 0.0f);
        TEST(panning_value == -1.0f && panning_prev_value == -1.0f);

        TEST(Synth::write_mix_cc(10, 127, volume_leaf, panning_leaf));
        TEST(panning_value == 1.0f && panning_prev_value == 1.0f);
        TEST(volume_value == 0.0f && volume_prev_value == 0.0f);

        TEST(Synth::write_mix_cc(7, 64, volume_leaf, panning_leaf));
        TEST(volume_value == 64.0f / 127.0f && volume_prev_value == 64.0f / 127.0f);
        TEST(panning_value == 1.0f && panning_prev_value == 1.0f);

        TEST(! Synth::write_mix_cc(11, 100, volume_leaf, panning_leaf));
        TEST(volume_value == 64.0f / 127.0f && volume_prev_value == 64.0f / 127.0f);
        TEST(panning_value == 1.0f && panning_prev_value == 1.0f);
    }

    // T-M3-3: the test captures upload values before each swap. The first
    // pair differs after a CC write; the next unchanged block emits equal
    // values. The test does not run the runtime upload loop.
    {
        float leaf_volume  = 0.0f;
        float leaf_panning = 0.0f;
        float volume       = 1.0f;
        float panning      = 0.5f;
        float old_volume   = 1.0f;
        float old_panning  = 0.5f;

        // A CC arrived during the block: the handoff moves only the current
        // fields, so the channel stage ramps old -> current across the block.
        leaf_volume  = 0.25f;
        leaf_panning = 0.75f;
        Synth::mix_leaves_to_channel(leaf_volume, leaf_panning, &volume, &panning);
        TEST(volume == 0.25f && panning == 0.75f);
        TEST(old_volume == 1.0f && old_panning == 0.5f);

        const float emitted_old_volume  = old_volume;
        const float emitted_volume      = volume;
        const float emitted_old_panning = old_panning;
        const float emitted_panning     = panning;
        TEST(emitted_old_volume == 1.0f && emitted_volume == 0.25f);
        TEST(emitted_old_panning == 0.5f && emitted_panning == 0.75f);
        old_volume  = volume;
        old_panning = panning;
        TEST(old_volume == 0.25f && old_panning == 0.75f);

        // Next block with no CC arriving: the emitted pair must already be
        // equal, so no ramp gradient exists before the swap.
        Synth::mix_leaves_to_channel(leaf_volume, leaf_panning, &volume, &panning);
        TEST(volume == 0.25f && panning == 0.75f);
        const float second_old_volume  = old_volume;
        const float second_volume      = volume;
        const float second_old_panning = old_panning;
        const float second_panning     = panning;
        TEST(second_old_volume == second_volume && second_old_panning == second_panning);
        old_volume  = volume;
        old_panning = panning;
        TEST(volume == old_volume && panning == old_panning);
    }
}

int main()
{
    test_silence_stdout();
    external_held_notes_regressions();
    device_midi_route_regressions();
    check_quick_release_regressions();
    check_envelope_note_off_regressions();
    check_envelope_restart_seed_regressions();
    check_note_on_exhaustion_regressions();
    check_voice_mode_codec();
    check_voice_mode_sim();
    check_voice_mode_reset_sim();
    check_runtime_inline_0();
    return test_exit_code();
}
