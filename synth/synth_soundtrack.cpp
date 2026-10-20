// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_soundtrack.h"
#include "synth_config.h"

#include <assert.h>

namespace Synth {

// Single runtime tempo: MIDI ticks per quarter is fixed at 192, and the player scales every
// delta by samples_per_midi_tick.  Tempo events riding the soundtrack stream and the editor
// tempo box both go through set_runtime_tempo_bpm, the only writer of the pair.
constexpr uint16_t default_runtime_tempo_bpm      = 120;
constexpr uint32_t default_midi_ticks_per_quarter = 192;

uint16_t runtime_tempo_bpm = default_runtime_tempo_bpm;
uint32_t samples_per_midi_tick =
    (rt_sampling_rate * 60u) / (default_runtime_tempo_bpm * default_midi_ticks_per_quarter);

void set_runtime_tempo_bpm(uint16_t bpm)
{
    // A zero tempo would divide by zero in the tick rate; the previous value stays.
    assert(bpm != 0);
    if (! bpm) {
        return;
    }

    runtime_tempo_bpm = bpm;
    // Extremely fast tempos truncate the tick rate to 0, which would collapse
    // all event timing to sample 0; clamp to at least 1 sample per tick.
    const uint32_t tick_rate = (rt_sampling_rate * 60u) /
                               (static_cast<uint32_t>(bpm) * default_midi_ticks_per_quarter);
    samples_per_midi_tick = tick_rate ? tick_rate : 1;
}

uint16_t get_runtime_tempo_bpm()
{
    return runtime_tempo_bpm;
}

uint32_t soundtrack_note_release_sample(uint32_t note_start, uint32_t stored_duration,
                                        uint32_t samples_per_tick)
{
    if (stored_duration == soundtrack_duration_unbounded) {
        return 0;
    }

    return note_start + (stored_duration - 1) * samples_per_tick;
}

} // namespace Synth
