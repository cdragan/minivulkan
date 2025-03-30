// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "realtime_synth.h"

namespace Synth {
    const uint32_t num_channels          = 1;
    uint32_t       channel_samples[]     = { 0 };
    const uint8_t  empty[]               = { 0xFFu, 0x7Fu };
    const uint8_t* delta_times[]         = { &empty[0] };
    const uint8_t* events[]              = { nullptr };
    uint8_t        events_decode_state[] = { 0 };
    const uint8_t* notes[]               = { nullptr };
    const uint8_t* note_data[]           = { nullptr };
    const uint8_t* controller[]          = { nullptr };
    const uint8_t* controller_data[]     = { nullptr };
    const uint8_t* pitch_bend_lo[]       = { nullptr };
    const uint8_t* pitch_bend_hi[]       = { nullptr };
    uint32_t       samples_per_midi_tick = 0;
}
