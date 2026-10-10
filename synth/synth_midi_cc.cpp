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
