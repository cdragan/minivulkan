// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_bank.h"
#include "sculptor_effect_graph.h"
#include "sculptor_graph.h"
#include "sculptor_osc_graph.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

namespace {

static_assert(sizeof(Synth::InstrumentBank) <= 450'000);
static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);

} // namespace

const char Synth::default_channel_names[max_channels][max_name_len] = {
    "Channel 01", "Channel 02", "Channel 03", "Channel 04", "Channel 05", "Channel 06", "Channel 07", "Channel 08",
    "Channel 09", "Drum Track", "Channel 11", "Channel 12", "Channel 13", "Channel 14", "Channel 15", "Channel 16",
};

// Factory default state: the first-run bank the editor builds for a fresh project.

bool Synth::init_default_channel(InstrumentBank* bank, uint32_t channel)
{
    // A bare instrument: one sine layer in blend mode with neutral shared
    // routing.  Only the instruments pool needs capacity: the default
    // references no descriptors and no channel effect chain.
    if (channel >= max_channels || bank->instruments.num_allocated + 1 > max_instruments) {
        return false;
    }
    const uint32_t     instr                          = bank->instruments.allocate();
    Synth::Instrument& instrument                     = bank->instruments.entries[instr];
    instrument                                        = {};
    instrument.layer_count                            = 1;
    instrument.layers[0].osc_type[0]                  = Synth::WaveType::sine_wave;
    instrument.layers[0].osc_type[1]                  = Synth::WaveType::no_wave;
    instrument.layers[0].osc_mode                     = Synth::osc_mode_blend;
    instrument.routing[Synth::mod_volume].base_value  = 1.0f;
    instrument.routing[Synth::mod_panning].base_value = 0.5f;
    bank->channel_zones[channel][0]                   = { 1, static_cast<uint8_t>(instr) };
    for (uint32_t slot = 1; slot < max_instr_per_channel; slot++) {
        bank->channel_zones[channel][slot] = {};
    }
    bank->channel_chains[channel] = {};
    return true;
}

void Synth::init_default_bank(InstrumentBank* bank)
{
    memset(bank, 0, sizeof(*bank));
    bank->drum_track_channel = 9;

    // A fresh bank is bare: the default instrument only, no master LFOs and
    // no effect chains, so the instrument's own envelope is what you hear.
    if (! init_default_channel(bank, 0)) {
        return; // A fresh bank always has room for the default channel.
    }
    bank->channel_enabled[0] = 1;
}

void Synth::get_zone_name(const Synth::InstrumentEditorBank* editor_bank,
                          uint32_t                           channel,
                          uint32_t                           zone_entry,
                          char*                              out,
                          uint32_t                           out_size)
{
    const InstrumentBank* const bank       = &editor_bank->bank;
    const uint8_t               instrument = bank->channel_zones[channel][zone_entry].instrument;
    const char* const           name       = editor_bank->instrument_names[instrument];

    if (name[0]) {
        snprintf(out, out_size, "%s", name);
        return;
    }

    snprintf(out, out_size, "Zone %u", zone_entry);
}

template <typename PoolT> static bool pool_is_compact(const PoolT& pool, uint32_t capacity)
{
    for (uint32_t i = 0; i < capacity; i++) {
        if (pool.is_occupied(i)) {
            if (i >= pool.num_allocated) {
                return false;
            }
        }
        else if (i < pool.num_allocated) {
            return false;
        }
    }

    return true;
}

static bool validate_envelope(const Synth::EnvelopeDescriptor& env)
{
    if (env.num_points < 1 || env.num_points > Synth::max_envelope_points) {
        return false;
    }

    if (env.sustain_first_point > env.sustain_last_point || env.sustain_last_point >= env.num_points) {
        return false;
    }

    if (! std::isfinite(env.min_value) || ! std::isfinite(env.min_max_delta)) {
        return false;
    }

    // A placed point must advance on its predecessor; a leading run of
    // position 0 is the descriptor's default (unplaced) layout and stays valid.
    uint16_t last_position = 0;
    bool     any_placed    = false;
    for (uint32_t i = 0; i < env.num_points; i++) {
        const uint16_t position = env.points[i].position;
        if (position == 0 && ! any_placed)
            continue;
        if (any_placed && position <= last_position) {
            return false;
        }
        last_position = position;
        any_placed    = true;
    }
    return true;
}

static bool valid_mod_source(uint32_t source)
{
    return source <= static_cast<uint32_t>(Synth::ModSource::pressure_combine);
}

static bool valid_source_op(uint32_t op)
{
    return op <= static_cast<uint32_t>(Synth::SourceOp::multiply);
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an
// effect's context (the runtime applies the same restriction at expansion).
static bool is_channel_effect_source(uint32_t source)
{
    return source == static_cast<uint32_t>(Synth::ModSource::none) ||
           source == static_cast<uint32_t>(Synth::ModSource::pitch_bend) ||
           source == static_cast<uint32_t>(Synth::ModSource::mod_wheel) ||
           source == static_cast<uint32_t>(Synth::ModSource::channel_pressure);
}

// One effect param's binding.  The master chain admits no MIDI-driven source at all
// (it has no channel inputs, so MIDI modulation there would be meaningless).
static bool validate_effect_param_binding(const Synth::EffectParamBinding& binding, bool is_master, uint32_t num_lfos)
{
    if (! std::isfinite(binding.base_value) || ! std::isfinite(binding.lfo_depth) ||
        ! std::isfinite(binding.lfo_rate_scale)) {
        return false;
    }

    if (! valid_source_op(static_cast<uint32_t>(binding.lfo_op))) {
        return false;
    }

    // With dense pools, a 1-based descriptor id refers to an occupied entry iff id <= num_allocated.
    if (binding.lfo_desc_id > num_lfos) {
        return false;
    }

    if (is_master) {
        if (binding.lfo_depth_source != Synth::ModSource::none || binding.lfo_rate_source != Synth::ModSource::none ||
            binding.num_inputs) {
            return false;
        }
    }
    else {
        if (! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_depth_source)) ||
            ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_rate_source))) {
            return false;
        }

        if (binding.num_inputs > Synth::max_mod_inputs) {
            return false;
        }

        for (uint32_t input = 0; input < binding.num_inputs; input++) {
            const Synth::ModInput& mod_input = binding.inputs[input];
            if (! is_channel_effect_source(static_cast<uint32_t>(mod_input.source)) ||
                ! valid_source_op(static_cast<uint32_t>(mod_input.op)) || ! std::isfinite(mod_input.scale)) {
                return false;
            }
        }
    }

    return true;
}

// One chain: slot types, finite params, legal bindings.  Also accumulates the whole-bank
// totals the caller checks against the modulation-pool and effect-state budgets.
static bool validate_effect_chain(const Synth::EffectChainBinding& chain,
                                  bool                             is_master,
                                  uint32_t                         num_lfos,
                                  uint32_t*                        num_modulated,
                                  uint32_t*                        state_bytes)
{
    if (chain.num_effects > Synth::max_chain_effects) {
        return false;
    }

    for (uint32_t slot = 0; slot < chain.num_effects; slot++) {

        const Synth::EffectSlotBinding& effect = chain.effects[slot];

        if (static_cast<uint32_t>(effect.type) >= Synth::num_effect_types) {
            return false;
        }

        const uint32_t num_params = get_effect_param_floats(effect.type);

        for (uint32_t param = 0; param < num_params; param++) {

            const Synth::EffectParamBinding& binding = effect.bindings[param];

            if (! validate_effect_param_binding(binding, is_master, num_lfos)) {
                return false;
            }

            if (binding.lfo_desc_id || binding.num_inputs) {
                (*num_modulated)++;
            }
        }

        if (effect.enabled && effect.type != Synth::EffectType::none) {
            *state_bytes += get_effect_state_bytes(effect.type);
        }
    }

    return true;
}

static bool validate_instrument(const Synth::Instrument& instrument)
{
    if (instrument.layer_count < 1 || instrument.layer_count > Synth::max_layers) {
        return false;
    }

    for (uint32_t layer = 0; layer < instrument.layer_count; layer++) {

        const Synth::Oscillator& osc = instrument.layers[layer];

        if (osc.osc_mode >= 3) {
            return false;
        }

        for (uint32_t w = 0; w < 2; w++) {
            if (static_cast<uint32_t>(osc.osc_type[w]) > static_cast<uint32_t>(Synth::WaveType::noise_wave)) {
                return false;
            }
        }

        for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

            const Synth::LayerGen& gen = osc.gen[target];

            if (! valid_source_op(static_cast<uint32_t>(gen.lfo_op))) {
                return false;
            }

            if (! valid_mod_source(static_cast<uint32_t>(gen.lfo_depth_source)) ||
                ! valid_mod_source(static_cast<uint32_t>(gen.lfo_rate_source))) {
                return false;
            }
        }
    }

    for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

        const Synth::InputRouting& routing = instrument.routing[target];

        if (routing.num_inputs > Synth::max_mod_inputs) {
            return false;
        }

        for (uint32_t input = 0; input < routing.num_inputs; input++) {
            if (! valid_mod_source(static_cast<uint32_t>(routing.inputs[input].source)) ||
                ! valid_source_op(static_cast<uint32_t>(routing.inputs[input].op))) {
                return false;
            }
        }
    }

    return true;
}

void Synth::get_default_channel_name(uint32_t channel, char* out, uint32_t out_size)
{
    if (channel >= max_channels || out_size == 0) {
        return;
    }

    snprintf(out, out_size, "%s", default_channel_names[channel]);
}

uint32_t Synth::zone_entry_at(const Zone* zones, uint32_t note)
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

bool Synth::zone_join_previous(Zone* zones, uint32_t i, uint32_t note)
{
    if (i >= max_instr_per_channel || i == 0 || note >= 128 || zone_entry_at(zones, note) != i)
        return false;

    const bool     has_next = i + 1 < max_instr_per_channel && zones[i + 1].start_note != 0;
    const uint32_t zone_end = has_next ? zones[i + 1].start_note - 2 : 127;
    if (note >= zone_end) {
        // Note was entry i's last note: it now covers nothing, so drop the entry and
        // shift the later zones down.  The loop stops at the first free slot, which
        // is where the terminator belongs.
        uint32_t slot = i;
        while (slot + 1 < max_instr_per_channel && zones[slot + 1].start_note != 0) {
            zones[slot] = zones[slot + 1];
            slot++;
        }
        zones[slot] = Zone{ 0, 0 };
    }
    else
        zones[i].start_note = static_cast<uint8_t>(note + 2);

    return true;
}

bool Synth::zone_join_next(Zone* zones, uint32_t i, uint32_t note)
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
        while (slot + 1 < max_instr_per_channel && zones[slot + 1].start_note != 0) {
            zones[slot] = zones[slot + 1];
            slot++;
        }
        zones[slot]         = Zone{ 0, 0 };
        zones[i].start_note = new_start;
    }
    else
        zones[i + 1].start_note = new_start;

    return true;
}

bool Synth::zone_split_new(Zone* zones, uint32_t i, uint32_t note, InstrumentEditorBank* editor_bank)
{
    InstrumentBank* const bank = &editor_bank->bank;
    if (i >= max_instr_per_channel || note >= 128)
        return false;
    if (zone_entry_at(zones, note) != i)
        return false;

    const bool first_note = note + 1 == zones[i].start_note;
    // A split that keeps entry i inserts one entry after it and shifts the rest down;
    // that needs one free slot, so a full 16-zone table refuses it.
    if (! first_note) {
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
    memcpy(editor_bank->instrument_names[dst], editor_bank->instrument_names[src], max_name_len);

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

bool Synth::validate_instrument_bank(const Synth::InstrumentBank* bank, const bool check_lfo_waves)
{
    // Pool metadata bounds first (num_allocated drives runtime indexing), then density.
    if (bank->instruments.num_allocated > max_instruments || bank->envelopes.num_allocated > max_envelopes ||
        bank->lfos.num_allocated > max_lfos || bank->parameters.num_allocated > max_parameters) {
        return false;
    }

    if (! pool_is_compact(bank->instruments, max_instruments) || ! pool_is_compact(bank->envelopes, max_envelopes) ||
        ! pool_is_compact(bank->lfos, max_lfos) || ! pool_is_compact(bank->parameters, max_parameters)) {
        return false;
    }

    // The runtime dispatches note-ons only to enabled channels; 0/1 are the only
    // meaningful bytes.
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        if (bank->channel_enabled[channel] > 1) {
            return false;
        }
    }

    for (uint32_t i = 0; i < bank->envelopes.num_allocated; i++) {
        if (! validate_envelope(bank->envelopes.entries[i])) {
            return false;
        }
    }

    for (uint32_t i = 0; i < bank->lfos.num_allocated; i++) {
        const Synth::LFODescriptor& lfo = bank->lfos.entries[i];
        // The sine/sawtooth palette is a runtime restriction enforced when a bank
        // is published; a file may carry descriptors written before it tightened,
        // so the load path checks only the period every LFO needs to oscillate.
        if (lfo.period_ms == 0 ||
            (check_lfo_waves && lfo.wave != Synth::WaveType::sine_wave && lfo.wave != Synth::WaveType::sawtooth_wave)) {
            return false;
        }
    }

    for (uint32_t instr = 0; instr < bank->instruments.num_allocated; instr++) {
        if (! validate_instrument(bank->instruments.entries[instr])) {
            return false;
        }

        const Synth::Instrument& instrument = bank->instruments.entries[instr];

        for (uint32_t layer = 0; layer < instrument.layer_count; layer++) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

                const Synth::LayerGen& gen = instrument.layers[layer].gen[target];

                if (gen.envelope_desc_id && (gen.envelope_desc_id > bank->envelopes.num_allocated ||
                                             ! bank->envelopes.is_occupied(gen.envelope_desc_id - 1))) {
                    return false;
                }

                if (gen.lfo_desc_id &&
                    (gen.lfo_desc_id > bank->lfos.num_allocated || ! bank->lfos.is_occupied(gen.lfo_desc_id - 1))) {
                    return false;
                }
            }
        }
    }

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {

        if (! bank->channel_enabled[channel]) {
            continue;
        }

        const Zone* const zones = bank->channel_zones[channel];

        // With dense pools, an id refers to an occupied entry iff id <= num_allocated
        // (1-based desc ids) / id < num_allocated (0-based instrument indices).
        if (zones[0].instrument >= bank->instruments.num_allocated) {
            return false;
        }

        uint32_t last_start = 0;
        bool     terminated = false;

        for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {

            const uint8_t start = zones[entry].start_note;

            if (terminated) {
                if (start != 0) {
                    return false;
                }
                continue;
            }

            if (start == 0) {
                terminated = true;
                continue;
            }

            if (entry == 0 && start != 1) {
                return false; // zone 0 always starts at note 0
            }

            if (entry && start <= last_start) {
                return false;
            }

            last_start = start;

            const uint8_t instrument = zones[entry].instrument;
            if (instrument >= bank->instruments.num_allocated) {
                return false;
            }
        }
    }

    uint32_t num_modulated = 0;
    uint32_t state_bytes   = 0;

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        if (! validate_effect_chain(bank->channel_chains[channel],
                                    false,
                                    bank->lfos.num_allocated,
                                    &num_modulated,
                                    &state_bytes)) {
            return false;
        }
    }

    if (! validate_effect_chain(bank->master_chain, true, bank->lfos.num_allocated, &num_modulated, &state_bytes)) {
        return false;
    }

    if (num_modulated > max_effect_mod_params || state_bytes > effect_state_budget) {
        return false;
    }

    return true;
}

// Compacts a pool after the caller freed the unwanted slots: fills new_ids with the new
// 1-based id of each old 1-based id (0 = removed), and moves the parallel name array
// (indexed by old slot, nullptr when the pool has none) into the new slot order.
template <typename T, uint32_t capacity>
static void compact_and_remap_pool(Pool<T, capacity>* pool, char (*names)[Synth::max_name_len], uint16_t* new_ids)
{
    uint32_t old_to_new[capacity];
    pool->defragment(old_to_new);

    if (names) {
        char moved[capacity][Synth::max_name_len] = {};
        for (uint32_t old_slot = 0; old_slot < capacity; old_slot++) {
            const uint32_t new_slot = old_to_new[old_slot];
            if (new_slot != pool_no_slot)
                memcpy(&moved[new_slot], &names[old_slot], sizeof(moved[0]));
        }
        memcpy(names, moved, sizeof(moved));
    }

    for (uint32_t id = 1; id <= capacity; id++) {
        const uint32_t slot = old_to_new[id - 1];
        new_ids[id - 1]     = slot != pool_no_slot ? static_cast<uint16_t>(slot + 1) : 0;
    }
}

void Synth::reclaim_unused_slots(Synth::InstrumentEditorBank* editor_bank)
{
    reclaim_unused_slots(editor_bank, nullptr);
}

void Synth::reclaim_unused_slots(Synth::InstrumentEditorBank* editor_bank, uint16_t* old_to_new_lfo_ids)
{
    InstrumentBank* const bank = &editor_bank->bank;
    // Instruments are rooted only by enabled channels' zone tables: a disabled channel
    // never dispatches note-ons, so its (possibly stale) entries root nothing.
    bool keep_instr[max_instruments] = {};

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {

        if (! bank->channel_enabled[channel])
            continue;

        const Zone* const zones = bank->channel_zones[channel];

        for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {
            // An empty zone slot's instrument byte is meaningless (0 would root slot 0),
            // so stop at the first one.
            if (zones[entry].start_note == 0)
                break;

            if (zones[entry].instrument < bank->instruments.num_allocated)
                keep_instr[zones[entry].instrument] = true;
        }
    }

    // Iterate a captured count: the frees below shrink num_allocated as they run.
    const uint32_t num_instruments = bank->instruments.num_allocated;
    for (uint32_t i = 0; i < num_instruments; i++) {
        if (! keep_instr[i])
            bank->instruments.free(i);
    }

    uint16_t instr_ids[max_instruments];
    compact_and_remap_pool(&bank->instruments, editor_bank->instrument_names, instr_ids);

    // Remap every zone table, disabled channels included: their entries may hold stale
    // bytes, and a reference to a removed instrument must not survive the compaction.
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {
            Zone&          zone   = bank->channel_zones[channel][entry];
            const uint16_t new_id = zone.instrument ? instr_ids[zone.instrument] : 0;
            zone.instrument       = static_cast<uint8_t>(new_id ? new_id - 1 : 0);
        }
    }

    // Descriptors are rooted by any surviving instrument and by any effect chain, enabled
    // or not: chains keep their state and bindings across channel enable/disable.
    bool keep_env[max_envelopes] = {};
    bool keep_lfo[max_lfos]      = {};

    for (uint32_t i = 0; i < bank->instruments.num_allocated; i++) {
        const Synth::Instrument& instr = bank->instruments.entries[i];
        for (uint32_t layer = 0; layer < instr.layer_count; layer++) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

                const Synth::LayerGen& gen = instr.layers[layer].gen[target];

                if (gen.envelope_desc_id && gen.envelope_desc_id <= max_envelopes)
                    keep_env[gen.envelope_desc_id - 1] = true;

                if (gen.lfo_desc_id && gen.lfo_desc_id <= max_lfos)
                    keep_lfo[gen.lfo_desc_id - 1] = true;
            }
        }
    }

    for (uint32_t chain = 0; chain <= Synth::max_channels; chain++) {

        const Synth::EffectChainBinding& binding =
            chain < Synth::max_channels ? bank->channel_chains[chain] : bank->master_chain;

        for (uint32_t slot = 0; slot < binding.num_effects; slot++) {

            const uint32_t num_params = get_effect_param_floats(binding.effects[slot].type);

            for (uint32_t param = 0; param < num_params; param++) {

                const uint16_t lfo_id = binding.effects[slot].bindings[param].lfo_desc_id;

                if (lfo_id && lfo_id <= max_lfos)
                    keep_lfo[lfo_id - 1] = true;
            }
        }
    }

    // Detached-node layout records root their descriptors too: a detached
    // envelope or LFO exists only through its record, and dropping the record's
    // descriptor would leave the editor node editing freed memory.
    for (uint32_t i = 0; i < editor_bank->graph_layout_count; i++) {
        const Synth::GraphNodeLayout& record = editor_bank->graph_layout[i];
        if (record.kind == 1 && record.index && record.index <= max_envelopes)
            keep_env[record.index - 1] = true;
        if (record.kind == 2 && record.index && record.index <= max_lfos)
            keep_lfo[record.index - 1] = true;
    }

    // Parameter records (kind 3) root their referenced descriptors too:
    // a record's wiring persists across gestures that leave no value
    // wires, so the record may be the only referencer an envelope or LFO
    // has.
    for (uint32_t r = 0; r < editor_bank->graph_layout_count; r++) {
        const GraphNodeLayout& record = editor_bank->graph_layout[r];
        if (record.kind != 3) {
            continue;
        }
        if (record.env_desc_id && record.env_desc_id <= max_envelopes)
            keep_env[record.env_desc_id - 1] = true;
        if (record.lfo_desc_id && record.lfo_desc_id <= max_lfos)
            keep_lfo[record.lfo_desc_id - 1] = true;
    }

    const uint32_t num_envelopes = bank->envelopes.num_allocated;
    for (uint32_t i = 0; i < num_envelopes; i++) {
        if (! keep_env[i])
            bank->envelopes.free(i);
    }

    uint16_t env_ids[max_envelopes];
    compact_and_remap_pool(&bank->envelopes, nullptr, env_ids);

    const uint32_t num_lfos = bank->lfos.num_allocated;
    for (uint32_t i = 0; i < num_lfos; i++) {
        if (! keep_lfo[i])
            bank->lfos.free(i);
    }

    uint16_t lfo_ids[max_lfos];
    compact_and_remap_pool(&bank->lfos, nullptr, lfo_ids);
    if (old_to_new_lfo_ids)
        memcpy(old_to_new_lfo_ids, lfo_ids, sizeof(lfo_ids));

    for (uint32_t i = 0; i < bank->instruments.num_allocated; i++) {

        Synth::Instrument& instr = bank->instruments.entries[i];

        for (uint32_t layer = 0; layer < instr.layer_count; layer++) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

                Synth::LayerGen& gen = instr.layers[layer].gen[target];

                if (gen.envelope_desc_id)
                    gen.envelope_desc_id = env_ids[gen.envelope_desc_id - 1];

                if (gen.lfo_desc_id)
                    gen.lfo_desc_id = lfo_ids[gen.lfo_desc_id - 1];
            }
        }
    }

    // Parameter records' descriptor references follow the same remap: a
    // stale id after compaction would either name a different descriptor
    // or exceed num_allocated and fail validation forever.
    for (uint32_t r = 0; r < editor_bank->graph_layout_count; r++) {
        GraphNodeLayout& record = editor_bank->graph_layout[r];
        if (record.kind != 3) {
            continue;
        }
        if (record.env_desc_id)
            record.env_desc_id = static_cast<uint8_t>(env_ids[record.env_desc_id - 1]);
        if (record.lfo_desc_id)
            record.lfo_desc_id = static_cast<uint8_t>(lfo_ids[record.lfo_desc_id - 1]);
    }

    for (uint32_t chain = 0; chain <= Synth::max_channels; chain++) {

        Synth::EffectChainBinding* const binding =
            chain < Synth::max_channels ? &bank->channel_chains[chain] : &bank->master_chain;

        for (uint32_t slot = 0; slot < binding->num_effects; slot++) {

            const uint32_t num_params = get_effect_param_floats(binding->effects[slot].type);

            for (uint32_t param = 0; param < num_params; param++) {

                Synth::EffectParamBinding* const p = &binding->effects[slot].bindings[param];

                if (p->lfo_desc_id)
                    p->lfo_desc_id = lfo_ids[p->lfo_desc_id - 1];
            }
        }
    }
    // Detached records are descriptor-keyed: their index follows the
    // compaction so each node keeps editing the same descriptor content.
    for (uint32_t i = 0; i < editor_bank->graph_layout_count; i++) {
        Synth::GraphNodeLayout& record = editor_bank->graph_layout[i];
        if (record.kind == 1 && record.index && record.index <= max_envelopes)
            record.index = static_cast<uint8_t>(env_ids[record.index - 1]);
        if (record.kind == 2 && record.index && record.index <= max_lfos)
            record.index = static_cast<uint8_t>(lfo_ids[record.index - 1]);
    }
    uint32_t retained = 0;
    for (uint32_t index = 0; index < editor_bank->graph_layout_count; ++index) {
        Synth::GraphNodeLayout record = editor_bank->graph_layout[index];
        uint16_t               new_id = 0;
        if (record.kind == 4 && Sculptor::translate_effect_lfo_title(record.name, lfo_ids, &new_id)) {
            const Synth::EffectChainBinding& chain =
                record.channel < Synth::max_channels ? bank->channel_chains[record.channel] : bank->master_chain;
            if (! new_id || ! Sculptor::effect_chain_uses_lfo(chain, new_id))
                continue;
        }
        editor_bank->graph_layout[retained++] = record;
    }
    editor_bank->graph_layout_count = retained;
}

const Synth::InstrumentBank* Synth::peek_bank_update(Synth::BankUpdateQueue* queue)
{
    const uint32_t head = queue->head.load(std::memory_order_relaxed);
    const uint32_t tail = queue->tail.load(std::memory_order_acquire);

    if (head == tail) {
        return nullptr;
    }
    return &queue->packets[head % bank_queue_capacity];
}

void Synth::consume_bank_update(Synth::BankUpdateQueue* queue)
{
    const uint32_t head = queue->head.load(std::memory_order_relaxed);
    queue->head.store(head + 1, std::memory_order_release);
}

bool Synth::push_bank_update(Synth::BankUpdateQueue* queue, const Synth::InstrumentBank& bank)
{
    const uint32_t tail = queue->tail.load(std::memory_order_relaxed);
    const uint32_t head = queue->head.load(std::memory_order_acquire);

    if (tail - head >= bank_queue_capacity) {
        return false;
    }

    queue->packets[tail % bank_queue_capacity] = bank;
    queue->tail.store(tail + 1, std::memory_order_release);
    return true;
}

// Editor graph metadata: record accounting and validation shared by the JSON
// codec and the editor's candidate commit.  Lives here (not in
// sculptor_osc_graph.cpp) so both binaries that link the editor bank also
// link its validation.

uint32_t Sculptor::count_detached_records(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone &&
            (record.kind == 1 || record.kind == 2 || record.kind == 3)) {
            ++count;
        }
    }
    return count;
}

bool Sculptor::graph_layout_record_identity_equal(const Synth::GraphNodeLayout& a, const Synth::GraphNodeLayout& b)
{
    return a.channel == b.channel && a.zone == b.zone && a.kind == b.kind && a.index == b.index && a.uid == b.uid &&
           (a.kind != 4 || strcmp(a.name, b.name) == 0);
}

bool Sculptor::validate_editor_metadata(const Synth::InstrumentEditorBank& bank)
{
    if (bank.graph_layout_count > Synth::max_graph_records) {
        return false;
    }
    uint16_t detached_per_zone[Synth::max_channels][Synth::max_instr_per_channel] = {};
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.zone >= Synth::max_instr_per_channel || record.kind > 4 ||
            (record.channel >= Synth::max_channels && ! (record.kind == 4 && record.channel == Synth::max_channels))) {
            return false;
        }
        // Effect-graph records (kind 4) key by node name instead of uid: the
        // fx projection names its nodes deterministically per chain content
        // ("Delay", "LFO 3", "Channel input"), and the master chain (chain
        // index max_channels) keeps its own records.
        if (record.kind == 4) {
            if (record.channel > Synth::max_channels || record.zone != 0 || record.index != 0 || record.uid != 0 ||
                record.depth_source != 0 || record.rate_source != 0 ||
                (record.served | record.env_desc_id | record.lfo_desc_id | record.lfo_depth_source |
                 record.lfo_rate_source | record.param_slot) != 0 ||
                record.name[0] == 0) {
                return false;
            }
        }
        // Kinds 1/2/3 are detached records keyed by uid; a zero uid would
        // alias the derived (record-less) projection state.
        if (record.kind != 0 && record.kind != 4 && record.uid == 0) {
            return false;
        }
        // The partial-wiring persistence fields belong to parameter records
        // alone; anywhere else they would silently change what a record keys.
        if (record.kind != 3 && record.kind != 4 &&
            (record.served != 0 || record.env_desc_id != 0 || record.lfo_desc_id != 0 || record.lfo_depth_source != 0 ||
             record.lfo_rate_source != 0 || record.param_slot != 0 || record.name[0] != 0)) {
            return false;
        }
        if (record.kind == 0) {
            if (record.index >= Synth::graph_canonical_node_count) {
                return false;
            }
            // Bound nodes are keyed by canonical index alone: a nonzero uid
            // would create a second key for the same node.
            if (record.uid != 0) {
                return false;
            }
        }
        else if (record.kind == 3) {
            // A parameter record keys a dynamic target (projection order
            // 0..4).  Its persistence fields name oscillator layers,
            // descriptor pool entries and ModSource values, so a stale
            // reference cannot push a projection read out of bounds.
            if (record.index >= 5 || (record.served >> Synth::max_layers) != 0 ||
                record.env_desc_id > bank.bank.envelopes.num_allocated ||
                record.lfo_desc_id > bank.bank.lfos.num_allocated ||
                record.lfo_depth_source > Synth::graph_canonical_input_count ||
                record.lfo_rate_source > Synth::graph_canonical_input_count) {
                return false;
            }
            if (++detached_per_zone[record.channel][record.zone] > Synth::max_detached_per_zone) {
                return false;
            }
            // Served bits name oscillator layers of the zone's instrument: a
            // bit at or above its actual layer count is stale.
            const Synth::Zone& zone_entry = bank.bank.channel_zones[record.channel][record.zone];
            if (zone_entry.start_note != 0 && zone_entry.instrument < bank.bank.instruments.num_allocated &&
                (record.served >> bank.bank.instruments.entries[zone_entry.instrument].layer_count) != 0) {
                return false;
            }
        }
        else if (record.kind != 4) {
            const uint32_t num_allocated =
                record.kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated) {
                return false;
            }
            if (record.depth_source > Synth::graph_canonical_input_count ||
                record.rate_source > Synth::graph_canonical_input_count) {
                return false;
            }
            if (++detached_per_zone[record.channel][record.zone] > Synth::max_detached_per_zone) {
                return false;
            }
        }
        if (! std::isfinite(record.x) || ! std::isfinite(record.y) || ! std::isfinite(record.width_override) ||
            ! std::isfinite(record.height_override)) {
            return false;
        }
        // Record keys are unique: (channel, zone, kind, index, uid); a kind-4
        // record keys by name (one record per projected fx node).
        for (uint32_t j = 0; j < i; ++j) {
            const Synth::GraphNodeLayout& other = bank.graph_layout[j];
            if (Sculptor::graph_layout_record_identity_equal(other, record)) {
                return false;
            }
        }
    }
    // The implied projected node count per zone must fit the node pool,
    // and the implied parameter count must fit the static parameter
    // registry: the projection refuses beyond it, which would leave the
    // zone's oscillator editor unavailable. count_projected_nodes mirrors
    // the projection's attachment decisions (ordinal trust, tuple
    // verification, positional pairing, dormant surplus), so a bank that
    // passes here is one the projection can always materialize.
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
            uint32_t parameter_count = 0;
            if (Sculptor::count_projected_nodes(bank, channel, zone, &parameter_count) > Sculptor::max_nodes ||
                parameter_count > Sculptor::max_param_nodes) {
                return false;
            }
            // A missing-sum bit must reference an existing oscillator layer:
            // a bit beyond the zone's layer count would mark a sum input
            // that does not exist and disable the channel for nothing.
            uint32_t           layers     = 0;
            const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
            if (zone_entry.start_note != 0 && zone_entry.instrument < bank.bank.instruments.num_allocated) {
                const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];
                layers = instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
            }
            // layers is clamped to max_layers, so the shift covers every
            // layer count including a full one: any bit at or beyond the
            // layer count (a stray high bit) invalidates.
            if ((bank.graph_missing_sum[channel][zone] >> layers) != 0) {
                return false;
            }
        }
    }
    return true;
}

// Whole-bank effect budget accounting, shared by the effects editor's
// preflights (a refused edit with a specific message reads better than the
// commit's generic refusal).  Counts modulated parameters and the static
// enabled-effect state bytes exactly like validate_instrument_bank does,
// including disabled effects and slots.
void Sculptor::count_effect_budgets(const Synth::InstrumentBank& bank, uint32_t* num_modulated, uint32_t* state_bytes)
{
    *num_modulated = 0;
    *state_bytes   = 0;
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        validate_effect_chain(bank.channel_chains[channel], false, bank.lfos.num_allocated, num_modulated, state_bytes);
    }
    validate_effect_chain(bank.master_chain, true, bank.lfos.num_allocated, num_modulated, state_bytes);
}
