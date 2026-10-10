// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"

#include <stdint.h>

namespace Synth {

constexpr uint32_t cc_msb_base        = 16; // assignable MSB controllers: CC 16..31
constexpr uint32_t cc_lsb_base        = 48; // paired LSB controllers:      CC 48..63
constexpr uint32_t num_assignable_ccs = 16;

// Raw 14-bit accumulators for one MIDI channel (CC 16-31 MSB, CC 48-63 LSB).
// lsb[] entries start at the 0xFF sentinel ("no LSB seen yet"); valid LSB
// values are 0..127, so 0xFF never collides with real data.
struct ChannelCcState {
    uint8_t msb[num_assignable_ccs];
    uint8_t lsb[num_assignable_ccs]; // 0xFF = not yet seen
};

// True for the assignable controllers (CC 16..31 and their LSB pairs 48..63).
bool is_assignable_cc(uint32_t controller);

// 0..15 slot index of an assignable controller (call only when is_assignable_cc).
uint32_t assignable_cc_slot(uint32_t controller);

// value = (MSB*128 + effective_LSB) / 16383, where effective_LSB is the LSB
// argument unless it is the 0xFF sentinel, in which case a copy of MSB is
// used.  MSB-only (sentinel) == MSB/127 exactly (16383 = 127*129);
// MSB=127 with LSB=127 == exactly 1.0; no clamping anywhere.
float cc_14bit_value(uint32_t msb, uint32_t lsb);

// Applies one controller event to the channel state and returns the slot's normalized
// value.  Non-assignable controllers leave the state untouched.  An LSB event
// replaces the sentinel with the received value (including zero).
float apply_channel_cc(ChannelCcState* state, uint32_t controller, uint8_t value);

} // namespace Synth
