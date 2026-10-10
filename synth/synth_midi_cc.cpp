// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_midi_cc.h"

bool Synth::is_assignable_cc(uint32_t controller)
{
    return (controller >= cc_msb_base && controller < cc_msb_base + num_assignable_ccs) ||
           (controller >= cc_lsb_base && controller < cc_lsb_base + num_assignable_ccs);
}

uint32_t Synth::assignable_cc_slot(uint32_t controller)
{
    return controller < cc_lsb_base ? controller - cc_msb_base : controller - cc_lsb_base;
}

float Synth::cc_14bit_value(uint32_t msb, uint32_t lsb)
{
    const uint32_t effective_lsb = (lsb == 0xFF) ? msb : lsb;
    return static_cast<float>(msb * 128 + effective_lsb) / 16383.0f;
}

float Synth::apply_channel_cc(ChannelCcState* state, uint32_t controller, uint8_t value)
{
    if (! is_assignable_cc(controller)) {
        return 0.0f;
    }
    const uint32_t slot = assignable_cc_slot(controller);
    if (controller < cc_lsb_base) {
        state->msb[slot] = value;
    }
    else {
        state->lsb[slot] = value; // a real LSB (including zero) clears the 0xFF sentinel
    }
    return cc_14bit_value(state->msb[slot], state->lsb[slot]);
}

Synth::MixRole Synth::mix_cc_role(uint32_t controller)
{
    if (controller == 7) {
        return MixRole::volume;
    }
    if (controller == 10) {
        return MixRole::panning;
    }
    return MixRole::none;
}

void Synth::write_external_leaf(float normalized, MixLeaf leaf)
{
    *leaf.value      = normalized;
    *leaf.prev_value = normalized;
}

bool Synth::write_mix_cc(uint32_t controller, uint8_t data, Synth::MixLeaf volume_leaf, Synth::MixLeaf panning_leaf)
{
    const MixRole role = mix_cc_role(controller);
    if (role == MixRole::none) {
        return false;
    }
    const float value = static_cast<float>(data) / 127.0f;
    write_external_leaf(value, (role == MixRole::volume) ? volume_leaf : panning_leaf);
    return true;
}

void Synth::mix_leaves_to_channel(float leaf_volume, float leaf_panning, float* volume, float* panning)
{
    *volume  = leaf_volume;
    *panning = leaf_panning;
}
