// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

namespace Synth {

// Pushes one decoded MIDI event into the live-input circular buffer from a
// producer thread (OS MIDI thread or the synth editor GUI thread). Returns
// false when the buffer is full and the event was dropped.
bool submit_external_midi_event(const MidiEvent& event);

// Display only.  Copies the held-note bits of one channel (4 words, bit n%32
// of word n/32).  Notes come from submit_external_midi_event: note_on with
// velocity > 0 sets a bit, note_off or note_on with velocity 0 clears it.
// Writes zeros for an invalid channel.
void get_external_held_notes(uint32_t channel, uint32_t* held_mask);

// Returns the total number of MIDI events submitted so far. The GUI compares
// consecutive values to detect new MIDI activity and redraw when it changes.
uint64_t get_external_midi_event_count();

} // namespace Synth
