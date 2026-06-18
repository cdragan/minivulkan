// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument.h"

namespace Synth {

uint8_t select_instrument(const NoteRoute* routes, uint32_t num_routes, uint8_t note)
{
    uint32_t instr_idx;

    for (instr_idx = 0; instr_idx < num_routes; instr_idx++) {
        const uint32_t start_note = routes[instr_idx].start_note;
        if (note < start_note || ! start_note) {
            if (instr_idx) {
                --instr_idx;
            }
            break;
        }
    }

    if (instr_idx == num_routes) {
        --instr_idx;
    }

    return routes[instr_idx].instrument;
}

static uint16_t remap_desc_id(uint16_t desc_id, const uint32_t* old_to_new)
{
    if (desc_id == 0) {
        return 0;
    }

    const uint32_t new_slot = old_to_new[desc_id - 1];
    if (new_slot == pool_no_slot) {
        return 0;
    }

    return static_cast<uint16_t>(new_slot + 1);
}

static void remap_layer_gen_ids(InstrumentBank* bank, const uint32_t* old_to_new, bool lfo_field)
{
    for (uint32_t instr = 0; instr < max_instruments; instr++) {
        if ( ! bank->instruments.is_occupied(instr)) {
            continue;
        }

        Instrument& instrument = bank->instruments.entries[instr];
        for (uint32_t layer = 0; layer < max_layers; layer++) {
            for (uint32_t target = 0; target < num_mod_targets; target++) {
                LayerGen& gen = instrument.layers[layer].gen[target];
                uint16_t* desc_id = lfo_field ? &gen.lfo_desc_id : &gen.envelope_desc_id;
                *desc_id = remap_desc_id(*desc_id, old_to_new);
            }
        }
    }
}

void remap_envelope_refs(InstrumentBank* bank, const uint32_t* old_to_new)
{
    remap_layer_gen_ids(bank, old_to_new, false);
}

void remap_lfo_refs(InstrumentBank* bank, const uint32_t* old_to_new)
{
    remap_layer_gen_ids(bank, old_to_new, true);
}

void remap_instrument_refs(InstrumentBank* bank, const uint32_t* old_to_new)
{
    for (uint32_t channel = 0; channel < max_channels; channel++) {
        for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {

            NoteRoute& route = bank->channel_routes[channel][entry];
            if (route.instrument >= max_instruments) {
                continue;
            }

            // A route to a deleted instrument falls back to instrument 0, never a stale index
            // that would now alias whichever instrument compacted into the freed slot.
            const uint32_t new_slot = old_to_new[route.instrument];
            route.instrument = (new_slot == pool_no_slot) ? 0 : static_cast<uint8_t>(new_slot);
        }
    }
}

} // namespace Synth
