// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument.h"

#include <cassert>
#include <stdio.h>
#include <string.h>

uint8_t Synth::route_instrument(const Zone* zones, uint32_t num_zones, uint8_t note)
{
    // Stored starts are first-note + 1, so compare against note + 1; a zero start ends the
    // table.  An all-zero (empty) table resolves to instrument 0, like a cleared channel.
    const uint8_t start     = static_cast<uint8_t>(note + 1);
    uint32_t      instr_idx = 0;
    uint32_t      i;

    for (i = 0; i < num_zones; i++) {
        if (! zones[i].start_note || start < zones[i].start_note) {
            break;
        }
        instr_idx = i;
    }

    return zones[instr_idx].instrument;
}

bool Synth::remap_envelopes(InstrumentBank* bank, const EnvelopeDescriptor* src, uint32_t num, uint16_t* out_ids)
{
    for (uint32_t i = 0; i < num; i++) {
        out_ids[i] = 0;
    }

    if (bank->envelopes.num_allocated + num > max_envelopes) {
        return false;
    }

    for (uint32_t i = 0; i < num; i++) {
        const uint32_t slot           = bank->envelopes.allocate();
        bank->envelopes.entries[slot] = src[i];
        out_ids[i]                    = static_cast<uint16_t>(slot + 1);
    }

    return true;
}

bool Synth::remap_lfos(InstrumentBank* bank, const LFODescriptor* src, uint32_t num, uint16_t* out_ids)
{
    for (uint32_t i = 0; i < num; i++) {
        out_ids[i] = 0;
    }

    if (bank->lfos.num_allocated + num > max_lfos) {
        return false;
    }

    for (uint32_t i = 0; i < num; i++) {
        const uint32_t slot      = bank->lfos.allocate();
        bank->lfos.entries[slot] = src[i];
        out_ids[i]               = static_cast<uint16_t>(slot + 1);
    }

    return true;
}

void Synth::remap_instrument(const Instrument& src, const uint16_t* env_ids, const uint16_t* lfo_ids, Instrument* dst)
{
    *dst = src;

    for (uint32_t layer = 0; layer < dst->layer_count; layer++) {
        for (uint32_t target = 0; target < num_mod_targets; target++) {

            LayerGen& gen = dst->layers[layer].gen[target];

            if (gen.envelope_desc_id) {
                gen.envelope_desc_id = env_ids[gen.envelope_desc_id - 1];
            }

            if (gen.lfo_desc_id) {
                gen.lfo_desc_id = lfo_ids[gen.lfo_desc_id - 1];
            }
        }
    }
}

void Synth::remap_effect_chain(const EffectChainBinding& src, const uint16_t* lfo_ids, EffectChainBinding* dst)
{
    *dst = src;

    for (uint32_t effect = 0; effect < dst->num_effects; effect++) {
        // Only the effect's live parameter slots carry validated references; the
        // engine and the validators ignore the tail, so remap it to none rather
        // than trust a stale value an in-memory bank may hold there.
        const uint32_t num_params = get_effect_param_floats(dst->effects[effect].type);

        for (uint32_t param = 0; param < num_params; param++) {

            EffectParamBinding& binding = dst->effects[effect].bindings[param];

            if (binding.lfo_desc_id) {
                binding.lfo_desc_id = lfo_ids[binding.lfo_desc_id - 1];
            }
        }

        memset(dst->effects[effect].bindings + num_params,
               0,
               sizeof(EffectParamBinding) * (max_effect_param_floats - num_params));
    }
}
