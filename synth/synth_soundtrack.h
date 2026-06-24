// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

#include <stdint.h>

namespace Synth {

// Compact soundtrack storage: the per-channel structure-of-arrays the realtime player streams
// directly (see midi_delta_times[] and friends in realtime_synth.h).  Times are MIDI ticks; the
// player scales each delta by samples_per_midi_tick at playback, so the stored stream is
// tempo-independent.  Storing ticks (not samples) and splitting each field into its own
// per-channel plane is what compresses smallest under the project compressor.
//
// Note duration model: a note is one note_on carrying the tick count until its note_off, so
// note_off events are never stored.  encode pairs each note_on with the next note_off on the same
// channel and note; decode reconstructs the note_off at start + duration.

// A note_on whose matching note_off is missing gets this stored duration, meaning "held with no
// stored release"; the player leaves such a voice sounding until voice stealing ends it.  A stored
// duration value v > 0 means a real duration of v - 1 ticks, so a genuine 0-tick note stays
// distinct from the unbounded sentinel.
constexpr uint32_t soundtrack_duration_unbounded = 0;

// Delta-time of this value in a channel's delta plane terminates the channel (no more events).
// Deltas are stored as a variable-length quantity; this sentinel is far above any real tick delta,
// so a song of any length encodes without colliding with it.
constexpr uint32_t soundtrack_end_of_channel = 0x0FFFFFFFu;

// Offset applied to a 14-bit MIDI pitch-bend value; MidiEvent stores it centered on zero.
constexpr int soundtrack_pitch_bend_center = 0x2000;

// Per-channel plane pointers into a caller-owned byte buffer.  Mirrors the player's extern arrays
// so the build-time converter can emit these planes verbatim.
struct Soundtrack {
    uint32_t       num_channels;
    const uint8_t* delta_times[max_channels];    // VLQ tick deltas, end_of_channel sentinel terminated
    const uint8_t* events[max_channels];         // 4-bit event codes, two per byte (high nibble first)
    const uint8_t* notes[max_channels];          // note number, one per note_on / aftertouch
    const uint8_t* note_data[max_channels];      // velocity / aftertouch pressure, paired with notes
    const uint8_t* durations[max_channels];      // VLQ tick duration, one per note_on
    const uint8_t* ctrl[max_channels];           // controller number, one per controller event
    const uint8_t* ctrl_data[max_channels];      // controller value, paired with ctrl
    const uint8_t* pitch_bend_lo[max_channels];  // pitch bend LSB (7 bits), one per pitch_bend event
    const uint8_t* pitch_bend_hi[max_channels];  // pitch bend MSB (7 bits), paired with lo
};

// Encodes a tick-domain, time-ordered MidiEvent stream into dest, filling *out with plane pointers
// into dest.  Returns bytes written, or 0 if dest is too small or an event has an out-of-range
// channel.  note_off events are consumed into the matched note_on's duration and not stored.
uint32_t encode_soundtrack(const MidiEvent* events,
                           uint32_t         num_events,
                           uint8_t*         dest,
                           uint32_t         dest_size,
                           Soundtrack*      out);

// Decodes a soundtrack into a canonical (time, then channel) ordered MidiEvent stream, with each
// note_off reconstructed at its note_on time + duration.  Returns the event count, or 0 if the
// stream holds more events than max_events.
uint32_t decode_soundtrack(const Soundtrack& soundtrack, MidiEvent* events, uint32_t max_events);

// Absolute sample at which a duration-model voice should auto-release, or 0 for none.  note_start
// is the note_on's sample time; stored_duration is the encoded value (0 = unbounded, else the real
// duration is stored_duration - 1 ticks).  The realtime player calls this to schedule the release.
// A bounded 0-tick note at sample 0 resolves to 0 and so is treated as "none" (degenerate; real
// music does not place a zero-length note at the very first tick).
uint32_t soundtrack_note_release_sample(uint32_t note_start, uint32_t stored_duration, uint32_t samples_per_tick);

} // namespace Synth
