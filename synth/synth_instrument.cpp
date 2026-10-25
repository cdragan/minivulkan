// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument.h"

#include <cassert>
#include <stdio.h>
#include <string.h>

// True when the binding carries anything at all: a nonzero base, at least one routed MIDI input,
// or a bound envelope/LFO descriptor.  Depth/rate-scale/op fields without a bound id or input do
// not make a binding present; this matches the JSON decoder's rule that an absent routing key
// leaves the binding zeroed.
bool Synth::osc_fir_slope_binding_present(const TargetBinding& binding)
{
    return binding.base_value != 0.0f || binding.num_inputs != 0 || binding.envelope_desc_id != 0 ||
           binding.lfo_desc_id != 0;
}

// The slope value a layer starts from: the routed base when the binding is present, the
// pre-existing scalar otherwise.  One spelling shared by the note-on seed, the editor display and
// the tests.
float Synth::osc_fir_slope_seed_value(const TargetBinding& binding, const uint8_t fir_slope_percent)
{
    return osc_fir_slope_binding_present(binding) ? binding.base_value : static_cast<float>(fir_slope_percent);
}

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

            TargetBinding& binding = dst->layers[layer].targets[target];

            if (binding.envelope_desc_id) {
                binding.envelope_desc_id = env_ids[binding.envelope_desc_id - 1];
            }

            if (binding.lfo_desc_id) {
                binding.lfo_desc_id = lfo_ids[binding.lfo_desc_id - 1];
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

        // FIR band bindings remap through the same lookup.  Dormant bands (beyond
        // the active count, or all of them on the legacy single-band form) zero
        // like the params tail rather than trust stale bytes.
        EffectSlotBinding& slot = dst->effects[effect];
        // The count byte travels unvalidated between banks, so the clamp
        // precedes every use: an out-of-bound count indexes past
        // bands[max_fir_effect_bands] (ASAN-visible corruption).
        uint32_t band_count = slot.fir_bands.band_count;
        if (band_count > max_fir_effect_bands)
            band_count = max_fir_effect_bands;
        // A fir_multiband slot reads the band array uniformly, so every band of
        // its clamped count is active (a zeroed count is the single-band marker).
        const uint32_t multiband_band_params =
            slot.type == EffectType::fir_multiband ? 4 * (band_count < 1 ? 1 : band_count) : 0;
        const uint32_t active_band_params = multiband_band_params                            ? multiband_band_params
                                            : slot.type == EffectType::fir && band_count > 1 ? 1 + 4 * (band_count - 1)
                                                                                             : 0;
        for (uint32_t band_param = 0; band_param < active_band_params; band_param++) {
            EffectParamBinding& band_binding = slot.fir_bands.bands[band_param];
            if (band_binding.lfo_desc_id) {
                band_binding.lfo_desc_id = lfo_ids[band_binding.lfo_desc_id - 1];
            }
        }
        memset(slot.fir_bands.bands + active_band_params,
               0,
               sizeof(EffectParamBinding) * (num_fir_effect_band_params - active_band_params));
    }
}
