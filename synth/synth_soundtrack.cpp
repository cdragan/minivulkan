// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_soundtrack.h"
#include "synth_config.h"

namespace Synth {
uint32_t soundtrack_note_release_sample(uint32_t note_start, uint32_t stored_duration,
                                        uint32_t samples_per_tick)
{
    if (stored_duration == soundtrack_duration_unbounded) {
        return 0;
    }

    return note_start + (stored_duration - 1) * samples_per_tick;
}

} // namespace Synth
