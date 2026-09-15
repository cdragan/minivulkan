// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument.h"

#include <cassert>
#include <string.h>

namespace Synth {

uint8_t route_instrument(const Zone* zones, uint32_t num_zones, uint8_t note)
{
    // Stored starts are first-note + 1, so compare against note + 1; a zero start ends the
    // table.  An all-zero (empty) table resolves to instrument 0, like a cleared channel.
    const uint8_t start = static_cast<uint8_t>(note + 1);
    uint32_t instr_idx = 0;
    uint32_t i;

    for (i = 0; i < num_zones; i++) {
        if ( ! zones[i].start_note || start < zones[i].start_note) {
            break;
        }
        instr_idx = i;
    }

    return zones[instr_idx].instrument;
}

bool remap_envelopes(InstrumentBank* bank, const EnvelopeDescriptor* src, uint32_t num, uint16_t* out_ids)
{
    for (uint32_t i = 0; i < num; i++) {
        out_ids[i] = 0;
    }
    if (bank->envelopes.num_allocated + num > max_envelopes) {
        return false;
    }
    for (uint32_t i = 0; i < num; i++) {
        const uint32_t slot = bank->envelopes.allocate();
        bank->envelopes.entries[slot] = src[i];
        out_ids[i] = static_cast<uint16_t>(slot + 1);
    }
    return true;
}

bool remap_lfos(InstrumentBank* bank, const LFODescriptor* src, uint32_t num, uint16_t* out_ids)
{
    for (uint32_t i = 0; i < num; i++) {
        out_ids[i] = 0;
    }
    if (bank->lfos.num_allocated + num > max_lfos) {
        return false;
    }
    for (uint32_t i = 0; i < num; i++) {
        const uint32_t slot = bank->lfos.allocate();
        bank->lfos.entries[slot] = src[i];
        out_ids[i] = static_cast<uint16_t>(slot + 1);
    }
    return true;
}

void remap_instrument(const Instrument& src, const uint16_t* env_ids, const uint16_t* lfo_ids, Instrument* dst)
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

void remap_effect_chain(const EffectChainBinding& src, const uint16_t* lfo_ids, EffectChainBinding* dst)
{
    *dst = src;
    for (uint32_t effect = 0; effect < dst->num_effects; effect++) {
        for (uint32_t param = 0; param < max_effect_param_floats; param++) {
            EffectParamBinding& binding = dst->effects[effect].bindings[param];
            if (binding.lfo_desc_id) {
                binding.lfo_desc_id = lfo_ids[binding.lfo_desc_id - 1];
            }
        }
    }
}

namespace {

// Default recipe content.  Descriptor ids are 1-based and local to each table:
// recipe_lfos: 1 = vibrato, 2 = tremolo; master_lfos: 1 = FIR cutoff sweep.
// The default channel's instrument, with 1-based descriptor ids local to the recipe tables:
// 1 = volume envelope, 2 = vibrato LFO, 3 = tremolo LFO.
Instrument build_recipe_instrument()
{
    Instrument instr = { };
    instr.layer_count = 1;
    Oscillator& osc = instr.layers[0];
    osc.osc_type[0] = WaveType::sine_wave;
    osc.osc_type[1] = WaveType::no_wave;
    osc.osc_mode = osc_mode_blend;

    // Pitch: wheel-scaled vibrato; the wheel also speeds the sweep up (negative rate_scale
    // shortens the 167 ms period toward ~60 ms at full wheel).
    LayerGen& pitch = osc.gen[mod_pitch];
    pitch.lfo_desc_id = 1;
    pitch.lfo_op = SourceOp::add;
    pitch.lfo_depth = 0.5f;
    pitch.lfo_depth_source = ModSource::mod_wheel;
    pitch.lfo_rate_source = ModSource::mod_wheel;
    pitch.lfo_rate_scale_ms = -107.0f;

    // Volume: the percussive ADSR, attenuated by a pressure-driven tremolo.
    LayerGen& volume = osc.gen[mod_volume];
    volume.envelope_desc_id = 1;
    volume.lfo_desc_id = 2;
    volume.lfo_op = SourceOp::multiply;
    volume.lfo_depth = 1.0f;
    volume.lfo_depth_source = ModSource::pressure_combine;

    instr.routing[mod_pitch].num_inputs = 1;
    instr.routing[mod_pitch].inputs[0] = { ModSource::pitch_bend, SourceOp::add, 1.0f };
    instr.routing[mod_volume].num_inputs = 1;
    instr.routing[mod_volume].inputs[0] = { ModSource::velocity, SourceOp::multiply, 1.0f };
    instr.routing[mod_panning].base_value = 0.5f;
    return instr;
}
const Instrument recipe_instrument = build_recipe_instrument();

const EnvelopeDescriptor recipe_envelope = {
    .num_points = 7,
    .sustain_first_point = 5,
    .sustain_last_point = 5,
    .min_value = 0.0f,
    .min_max_delta = 1.0f / 65535.0f,
    .points = {
        { 0, 0 },          // silent
        { 1, 0xFFFF },     // fast attack (~6 ms)
        { 12, 0xB000 },    // initial fast decay
        { 45, 0x6000 },
        { 120, 0x2000 },
        { 210, 0x0800 },   // long tail to ~3% (sustain)
        { 235, 0 },        // release (~145 ms)
    },
};

const LFODescriptor recipe_lfos[2] = {
    { WaveType::sine_wave, 0, 167, 0.0f, 1.0f }, // vibrato: sine, ~6 Hz
    { WaveType::sine_wave, 0, 200, 0.0f, 1.0f }, // tremolo: sine, ~5 Hz
};

// Demo channel chain: tanh drive 5.0, fully wet.
const EffectChainBinding recipe_channel_chain = {
    .num_effects = 1,
    .effects = {
        {
            .type = EffectType::distortion,
            .enabled = true,
            .bindings = {
                { 5.0f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 1.0f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
            },
        },
    },
};

// Demo master chain: reverb, compressor, then an FIR lowpass whose cutoff a 4 s triangle
// (sawtooth at duty 0x7F) sweeps through a bank binding (base = sweep center, depth = half
// the sweep span).
const LFODescriptor master_lfos[1] = {
    { WaveType::sawtooth_wave, 0x7F, 4000, 0.0f, 1.0f },
};

const EffectChainBinding recipe_master_chain = {
    .num_effects = 3,
    .effects = {
        {
            .type = EffectType::reverb,
            .enabled = true,
            .bindings = {
                { 0.7f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 0.5f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 0.3f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
            },
        },
        {
            .type = EffectType::compressor,
            .enabled = true,
            .bindings = {
                { 0.3f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 4.0f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 0.9f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 0.9995f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 1.5f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
            },
        },
        {
            .type = EffectType::fir,
            .enabled = true,
            .bindings = {
                { 3125.0f, 1, SourceOp::add, 2875.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
                { 0.0f, 0, SourceOp::add, 0.0f, ModSource::none, ModSource::none, 0.0f, 0, { } },
            },
        },
    },
};

const char default_channel_names[max_channels][max_name_len] = {
    "Channel 01", "Channel 02", "Channel 03", "Channel 04", "Channel 05",
    "Channel 06", "Channel 07", "Channel 08", "Channel 09", "Drum Track",
    "Channel 11", "Channel 12", "Channel 13", "Channel 14", "Channel 15",
    "Channel 16",
};

} // namespace

bool init_default_channel(InstrumentBank* bank, uint32_t channel)
{
    if (channel >= max_channels ||
        bank->instruments.num_allocated + 1 > max_instruments ||
        bank->envelopes.num_allocated + 1 > max_envelopes ||
        bank->lfos.num_allocated + 2 > max_lfos) {
        return false;
    }

    uint16_t env_ids[1];
    uint16_t lfo_ids[2];
    if ( ! remap_envelopes(bank, &recipe_envelope, 1, env_ids) ||
         ! remap_lfos(bank, recipe_lfos, 2, lfo_ids)) {
        return false;
    }

    const uint32_t instr = bank->instruments.allocate();
	remap_instrument(recipe_instrument, env_ids, lfo_ids, &bank->instruments.entries[instr]);

    bank->channel_zones[channel][0] = { 1, static_cast<uint8_t>(instr) };
    for (uint32_t slot = 1; slot < max_instr_per_channel; slot++) {
        bank->channel_zones[channel][slot] = { };
    }

    remap_effect_chain(recipe_channel_chain, lfo_ids, &bank->channel_chains[channel]);
    return true;
}

uint32_t zone_entry_at(const Zone* zones, uint32_t note)
{
    uint32_t found = pool_no_slot;

    for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {
        if (zones[entry].start_note == 0)
            break;
        if (note >= zones[entry].start_note - 1)
            found = entry;
        else
            break; // later zones start even higher
    }
    return found;
}

bool zone_join_previous(Zone* zones, uint32_t i, uint32_t note)
{
    if (i >= max_instr_per_channel || i == 0 || note >= 128 || zone_entry_at(zones, note) != i)
        return false;

    const bool has_next = i + 1 < max_instr_per_channel && zones[i + 1].start_note != 0;
    const uint32_t zone_end = has_next ? zones[i + 1].start_note - 2 : 127;
    if (note >= zone_end) {
        // Note was entry i's last note: it now covers nothing, so drop the entry and
        // shift the later zones down.  The loop stops at the first free slot, which
        // is where the terminator belongs.
        uint32_t slot = i;
        while (zones[slot + 1].start_note != 0) {
            zones[slot] = zones[slot + 1];
            slot++;
        }
        zones[slot] = Zone{ 0, 0 };
    }
    else
        zones[i].start_note = static_cast<uint8_t>(note + 2);

    return true;
}

bool zone_join_next(Zone* zones, uint32_t i, uint32_t note)
{
    if (i >= max_instr_per_channel || note >= 128 || i + 1 >= max_instr_per_channel)
        return false;
    if (zones[i + 1].start_note == 0 || zone_entry_at(zones, note) != i)
        return false;

    const uint8_t new_start = static_cast<uint8_t>(note + 1);
    if (new_start <= zones[i].start_note) {
        // Note was entry i's first note: it now covers nothing, so drop the entry and
        // shift the later zones down.  The loop stops at the first free slot, which
        // is where the terminator belongs.  The shifted entry keeps the new start.
        uint32_t slot = i;
        while (zones[slot + 1].start_note != 0) {
            zones[slot] = zones[slot + 1];
            slot++;
        }
        zones[slot] = Zone{ 0, 0 };
        zones[i].start_note = new_start;
    }
    else
        zones[i + 1].start_note = new_start;

    return true;
}

bool zone_split_new(Zone* zones, uint32_t i, uint32_t note, InstrumentBank* bank)
{
    if (i >= max_instr_per_channel || note >= 128)
        return false;
    if (zone_entry_at(zones, note) != i)
        return false;

    const bool first_note = note + 1 == zones[i].start_note;
    // A split that keeps entry i inserts one entry after it and shifts the rest down;
    // that needs one free slot, so a full 16-zone table refuses it.
    if ( ! first_note) {
        uint32_t num_zones = 0;
        while (num_zones < max_instr_per_channel && zones[num_zones].start_note != 0)
            num_zones++;
        if (num_zones >= max_instr_per_channel)
            return false;
    }

    // The zone instrument byte is the pool slot directly (slot 0 is a valid instrument);
    // an occupied slot's instrument is always a live pool slot.
    const uint32_t src = zones[i].instrument;
    const uint32_t dst = bank->instruments.allocate();
    if (dst == pool_no_slot)
        return false;

    bank->instruments.entries[dst] = bank->instruments.entries[src];
    memcpy(bank->instrument_names[dst], bank->instrument_names[src], max_name_len);

    if (first_note)
        zones[i].instrument = static_cast<uint8_t>(dst); // entry i keeps its start
    else {
        for (uint32_t slot = max_instr_per_channel - 1; slot > i + 1; slot--)
            zones[slot] = zones[slot - 1];
        zones[i + 1].start_note = static_cast<uint8_t>(note + 1);
        zones[i + 1].instrument = static_cast<uint8_t>(dst);
    }

    return true;
}

void init_default_bank(InstrumentBank* bank)
{
    memset(bank, 0, sizeof(*bank));
    for (uint32_t channel = 0; channel < max_channels; channel++) {
        memcpy(bank->channel_names[channel], default_channel_names[channel], max_name_len);
    }
    bank->drum_track_channel = 9;

    // A fresh bank always has room; the master chain's LFO remap cannot fail either.
    assert(init_default_channel(bank, 0));
    bank->channel_enabled[0] = 1;

    uint16_t master_lfo_ids[1];
    remap_lfos(bank, master_lfos, 1, master_lfo_ids);
    remap_effect_chain(recipe_master_chain, master_lfo_ids, &bank->master_chain);
}

} // namespace Synth
