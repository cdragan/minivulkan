// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument.h"

namespace Synth {

uint8_t route_instrument(const Zone* zones, uint32_t num_zones, uint8_t note)
{
    uint32_t instr_idx;

    for (instr_idx = 0; instr_idx < num_zones; instr_idx++) {
        const uint32_t start_note = zones[instr_idx].start_note;
        if (note < start_note || ! start_note) {
            if (instr_idx) {
                --instr_idx;
            }
            break;
        }
    }

    if (instr_idx == num_zones) {
        --instr_idx;
    }

    return zones[instr_idx].instrument;
}

} // namespace Synth
