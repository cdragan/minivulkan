// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "realtime_synth.h"

// Builds without live MIDI input (the minimal playback build) link this placeholder, so the
// audio loop's per-step pump compiles to nothing.

namespace Synth {

void pump_live_midi()
{
}

} // namespace Synth
