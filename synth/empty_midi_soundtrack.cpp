// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "realtime_synth.h"

namespace Synth {
    const uint32_t num_channels          = 2;
    const uint8_t  empty[]               = { 0xFFu, 0xFFu, 0xFFu, 0x7Fu };
    const uint8_t* midi_delta_times[]    = { &empty[0], &empty[0] };
    const uint8_t* midi_events[]         = { nullptr, nullptr };
    const uint8_t* midi_notes[]          = { nullptr, nullptr };
    const uint8_t* midi_note_data[]      = { nullptr, nullptr };
    const uint8_t* midi_note_durations[] = { nullptr, nullptr };
    const uint8_t* midi_ctrl[]           = { nullptr, nullptr };
    const uint8_t* midi_ctrl_data[]      = { nullptr, nullptr };
    const uint8_t* midi_pitch_bend_lo[]  = { nullptr, nullptr };
    const uint8_t* midi_pitch_bend_hi[]  = { nullptr, nullptr };

    // One routing per MIDI channel; live input can target any channel.
    const InstrumentRouting instr_routing[max_channels] = { };
}
