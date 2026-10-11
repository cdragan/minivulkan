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

// Channel-mix CCs: CC 7 = volume, CC 10 = panning.  Plain 7-bit, outside the
// assignable MSB (16..31) and LSB (48..63) ranges (cc_msb_base/cc_lsb_base).
enum class MixRole : uint8_t {
    none,
    volume,
    panning
};
MixRole mix_cc_role(uint32_t controller);

// One external leaf: pointers to a parameter slot's value and prev_value
// fields.  Every MIDI-driven source (mix CCs, mod wheel, assignable CCs,
// pressure, bend) binds one.
struct ExternalLeaf {
    float* value;
    float* prev_value;
};

// Shared zero-lag write for one external leaf: sets value and prev_value
// together to the normalized value.  Every MIDI-driven leaf handler uses it.
void write_external_leaf(float normalized, ExternalLeaf leaf);

// Writes a channel-mix controller into the addressed leaf.  Value is
// data/127.0f; value and prev_value are written together (zero lag, same
// pattern as the mod wheel / pressure handlers).  Returns false for
// controllers that are not channel-mix CCs; writes nothing in that case.
bool write_mix_cc(uint32_t controller, uint8_t data, ExternalLeaf volume_leaf, ExternalLeaf panning_leaf);

// Block-boundary handoff at the channel-to-master combine upload: copies the
// current leaf values into the channel mix state.  Never touches the old_*
// fields; the caller's existing swap (old = current, after the shader params
// are emitted) stays the only old/new machinery.
void mix_leaves_to_channel(float leaf_volume, float leaf_panning, float* volume, float* panning);

} // namespace Synth
