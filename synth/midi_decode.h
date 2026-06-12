// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

#include <stdint.h>

namespace Synth {

struct MidiDecoder {
    uint8_t status;     // Running status byte (0 = none); channel-voice status only
    uint8_t data[2];    // Data bytes buffered for the message in progress
    uint8_t data_count; // Number of data bytes buffered so far
    bool    in_sysex;   // Inside a SysEx block, swallowing payload until it ends
};

bool midi_decode_byte(MidiDecoder* decoder, uint8_t byte, MidiEvent* out);

} // namespace Synth
