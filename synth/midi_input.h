// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

namespace Synth {

// Pushes one decoded MIDI event into the live-input circular buffer from the OS MIDI thread.
void submit_external_midi_event(const MidiEvent& event);

} // namespace Synth
