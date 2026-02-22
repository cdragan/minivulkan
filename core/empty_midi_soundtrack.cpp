// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "realtime_synth.h"

namespace Synth {
    const uint32_t num_channels         = 1;
    const uint8_t  empty[]              = { 0xFFu, 0x7Fu };
    const uint8_t* midi_delta_times[]   = { &empty[0] };
    const uint8_t* midi_events[]        = { nullptr };
    const uint8_t* midi_notes[]         = { nullptr };
    const uint8_t* midi_note_data[]     = { nullptr };
    const uint8_t* midi_ctrl[]          = { nullptr };
    const uint8_t* midi_ctrl_data[]     = { nullptr };
    const uint8_t* midi_pitch_bend_lo[] = { nullptr };
    const uint8_t* midi_pitch_bend_hi[] = { nullptr };

    const InstrumentRouting instr_routing[] = { { } };
}
