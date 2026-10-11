// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

// Platform hooks implemented by the Windows audio and MIDI back ends.
namespace Synth {
bool init_synth_os();
void stop_synth_os();
bool init_midi_os();
void stop_midi_os();
} // namespace Synth
