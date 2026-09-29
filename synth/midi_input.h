// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

namespace Synth {

// Pushes one decoded MIDI event into the live-input circular buffer from a
// producer thread (OS MIDI thread or the synth editor GUI thread). Returns
// false when the buffer is full and the event was dropped.
bool submit_external_midi_event(const MidiEvent& event);

} // namespace Synth
