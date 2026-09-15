// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_bank.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

#include "../synth/synth_serialize.h"

static_assert(sizeof(Synth::InstrumentBank) <= 450'000);            // runtime + editor + scratch + rollback + pending
static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);
static_assert(7 * sizeof(Synth::InstrumentBank)
              + 10 * sizeof(Synth::InstrumentBank)
              + Synth::instrument_bank_header_size
              + 4 * 1638400u
              <= 16 * 1024 * 1024);

void Synth::get_zone_name(const Synth::InstrumentBank* bank, uint32_t channel, uint32_t zone_entry, char* out, uint32_t out_size)
{
    const uint8_t instrument = bank->channel_zones[channel][zone_entry].instrument;
    const char* const name = bank->instrument_names[instrument];

    if (name[0]) {
        snprintf(out, out_size, "%s", name);
        return;
    }

    snprintf(out, out_size, "Zone %u", zone_entry);
}

namespace {

template<typename PoolT>
bool pool_is_compact(const PoolT& pool, uint32_t capacity)
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

bool validate_envelope(const Synth::EnvelopeDescriptor& env)
{
    if (env.num_points < 1 || env.num_points > Synth::max_envelope_points) {
        return false;
    }

    if (env.sustain_first_point > env.sustain_last_point || env.sustain_last_point >= env.num_points) {
        return false;
    }

    if ( ! std::isfinite(env.min_value) || ! std::isfinite(env.min_max_delta)) {
        return false;
    }

    for (uint32_t i = 0; i < env.num_points; i++) {
        if (i && env.points[i].position <= env.points[i - 1].position) {
            return false;
        }
    }

    return true;
}

bool validate_lfo(const Synth::LFODescriptor& lfo)
{
    if (lfo.period_ms == 0) {
        return false;
    }

    // The only wave type supported by LFO
    if (lfo.wave != Synth::WaveType::sine_wave && lfo.wave != Synth::WaveType::sawtooth_wave) {
        return false;
    }

    return true;
}

bool valid_mod_source(uint32_t source)
{
    return source <= static_cast<uint32_t>(Synth::ModSource::pressure_combine);
}

bool valid_source_op(uint32_t op)
{
    return op <= static_cast<uint32_t>(Synth::SourceOp::multiply);
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an
// effect's context (the runtime applies the same restriction at expansion).
bool is_channel_effect_source(uint32_t source)
{
    return source == static_cast<uint32_t>(Synth::ModSource::none)
        || source == static_cast<uint32_t>(Synth::ModSource::pitch_bend)
        || source == static_cast<uint32_t>(Synth::ModSource::mod_wheel)
        || source == static_cast<uint32_t>(Synth::ModSource::channel_pressure);
}

// One effect param's binding. The master chain admits no MIDI-driven source at all
// (it has no channel inputs, so MIDI modulation there would be meaningless).
bool validate_effect_param_binding(const Synth::EffectParamBinding& binding, bool is_master, uint32_t num_lfos)
{
    if ( ! std::isfinite(binding.base_value) ||
         ! std::isfinite(binding.lfo_depth) ||
         ! std::isfinite(binding.lfo_rate_scale)) {
        return false;
    }

    if ( ! valid_source_op(static_cast<uint32_t>(binding.lfo_op))) {
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
        if ( ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_depth_source)) ||
             ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_rate_source))) {
            return false;
        }

        if (binding.num_inputs > Synth::max_mod_inputs) {
            return false;
        }

        for (uint32_t input = 0; input < binding.num_inputs; input++) {
            const Synth::ModInput& mod_input = binding.inputs[input];
            if ( ! is_channel_effect_source(static_cast<uint32_t>(mod_input.source)) ||
                 ! valid_source_op(static_cast<uint32_t>(mod_input.op)) ||
                 ! std::isfinite(mod_input.scale)) {
                return false;
            }
        }
    }

    return true;
}

// One chain: slot types, finite params, legal bindings. Also accumulates the whole-bank
// totals the caller checks against the modulation-pool and effect-state budgets.
bool validate_effect_chain(const Synth::EffectChainBinding& chain,
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

            if ( ! validate_effect_param_binding(binding, is_master, num_lfos)) {
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

bool validate_instrument(const Synth::Instrument& instrument)
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

            if ( ! valid_source_op(static_cast<uint32_t>(gen.lfo_op))) {
                return false;
            }

            if ( ! valid_mod_source(static_cast<uint32_t>(gen.lfo_depth_source)) ||
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
            if ( ! valid_mod_source(static_cast<uint32_t>(routing.inputs[input].source)) ||
                ! valid_source_op(static_cast<uint32_t>(routing.inputs[input].op))) {
                return false;
            }
        }
    }

    return true;
}

// Fixed-size name buffers hold raw byte images; an imported file may carry an
// unterminated run, and any reader stopping at NUL would read past the buffer.
bool verify_names(const Synth::InstrumentBank* bank)
{
    for (uint32_t i = 0; i < bank->instruments.num_allocated; i++) {
        if (memchr(bank->instrument_names[i], 0, Synth::max_name_len) == nullptr) {
            return false;
        }
    }

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        if (memchr(bank->channel_names[channel], 0, Synth::max_name_len) == nullptr) {
            return false;
        }
    }

    return true;
}

} // anonymous namespace

bool Synth::validate_instrument_bank(const Synth::InstrumentBank* bank)
{
    // Pool metadata bounds first (num_allocated drives runtime indexing), then density.
    if (bank->instruments.num_allocated > max_instruments ||
        bank->envelopes.num_allocated > max_envelopes ||
        bank->lfos.num_allocated > max_lfos ||
        bank->parameters.num_allocated > max_parameters) {
        return false;
    }

    if (! pool_is_compact(bank->instruments, max_instruments) ||
        ! pool_is_compact(bank->envelopes, max_envelopes) ||
        ! pool_is_compact(bank->lfos, max_lfos) ||
        ! pool_is_compact(bank->parameters, max_parameters)) {
        return false;
    }

    // The runtime dispatches note-ons only to enabled channels; 0/1 are the only
    // meaningful bytes.
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        if (bank->channel_enabled[channel] > 1) {
            return false;
        }
    }

    if ( ! verify_names(bank)) {
        return false;
    }

    for (uint32_t i = 0; i < bank->envelopes.num_allocated; i++) {
        if ( ! validate_envelope(bank->envelopes.entries[i])) {
            return false;
        }
    }

    for (uint32_t i = 0; i < bank->lfos.num_allocated; i++) {
        if ( ! validate_lfo(bank->lfos.entries[i])) {
            return false;
        }
    }

    for (uint32_t instr = 0; instr < bank->instruments.num_allocated; instr++) {
        if ( ! validate_instrument(bank->instruments.entries[instr])) {
            return false;
        }

        const Synth::Instrument& instrument = bank->instruments.entries[instr];

        for (uint32_t layer = 0; layer < instrument.layer_count; layer++) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {

                const Synth::LayerGen& gen = instrument.layers[layer].gen[target];

                if (gen.envelope_desc_id &&
                    (gen.envelope_desc_id > bank->envelopes.num_allocated ||
                     ! bank->envelopes.is_occupied(gen.envelope_desc_id - 1))) {
                    return false;
                }

                if (gen.lfo_desc_id &&
                    (gen.lfo_desc_id > bank->lfos.num_allocated ||
                     ! bank->lfos.is_occupied(gen.lfo_desc_id - 1))) {
                    return false;
                }
            }
        }
    }

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {

        if ( ! bank->channel_enabled[channel]) {
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
    uint32_t state_bytes  = 0;

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        if ( ! validate_effect_chain(bank->channel_chains[channel], false, bank->lfos.num_allocated,
                                     &num_modulated, &state_bytes)) {
            return false;
        }
    }

    if ( ! validate_effect_chain(bank->master_chain, true, bank->lfos.num_allocated,
                                 &num_modulated, &state_bytes)) {
        return false;
    }

    if (num_modulated > max_effect_mod_params || state_bytes > effect_state_budget) {
        return false;
    }

    return true;
}

namespace {


// Compacts a pool after the caller freed the unwanted slots: fills new_ids with the new
// 1-based id of each old 1-based id (0 = removed), and moves the parallel name array
// (indexed by old slot, nullptr when the pool has none) into the new slot order.
template<typename T, uint32_t capacity>
void compact_and_remap_pool(Pool<T, capacity>* pool, char (*names)[Synth::max_name_len], uint16_t* new_ids)
{
    uint32_t old_to_new[capacity];
    pool->defragment(old_to_new);

    if (names) {
        char moved[capacity][Synth::max_name_len] = { };
        for (uint32_t old_slot = 0; old_slot < capacity; old_slot++) {
            const uint32_t new_slot = old_to_new[old_slot];
            if (new_slot != pool_no_slot)
                memcpy(&moved[new_slot], &names[old_slot], sizeof(moved[0]));
        }
        memcpy(names, moved, sizeof(moved));
    }

    for (uint32_t id = 1; id <= capacity; id++) {
        const uint32_t slot = old_to_new[id - 1];
        new_ids[id - 1] = slot != pool_no_slot ? static_cast<uint16_t>(slot + 1) : 0;
    }
}

} // anonymous namespace

void Synth::reclaim_unused_slots(Synth::InstrumentBank* bank)
{
    // Instruments are rooted only by enabled channels' zone tables: a disabled channel
    // never dispatches note-ons, so its (possibly stale) entries root nothing.
    bool keep_instr[max_instruments] = { };

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {

        if ( ! bank->channel_enabled[channel])
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
        if ( ! keep_instr[i])
            bank->instruments.free(i);
    }

    uint16_t instr_ids[max_instruments];
    compact_and_remap_pool(&bank->instruments, bank->instrument_names, instr_ids);

    // Remap every zone table, disabled channels included: their entries may hold stale
    // bytes, and a reference to a removed instrument must not survive the compaction.
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        for (uint32_t entry = 0; entry < max_instr_per_channel; entry++) {
            Zone& zone = bank->channel_zones[channel][entry];
            const uint16_t new_id = zone.instrument ? instr_ids[zone.instrument] : 0;
            zone.instrument = static_cast<uint8_t>(new_id ? new_id - 1 : 0);
        }
    }

    // Descriptors are rooted by any surviving instrument and by any effect chain, enabled
    // or not: chains keep their state and bindings across channel enable/disable.
    bool keep_env[max_envelopes] = { };
    bool keep_lfo[max_lfos]      = { };

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

    const uint32_t num_envelopes = bank->envelopes.num_allocated;
    for (uint32_t i = 0; i < num_envelopes; i++) {
        if ( ! keep_env[i])
            bank->envelopes.free(i);
    }

    uint16_t env_ids[max_envelopes];
    compact_and_remap_pool(&bank->envelopes, nullptr, env_ids);

    const uint32_t num_lfos = bank->lfos.num_allocated;
    for (uint32_t i = 0; i < num_lfos; i++) {
        if ( ! keep_lfo[i])
            bank->lfos.free(i);
    }

    uint16_t lfo_ids[max_lfos];
    compact_and_remap_pool(&bank->lfos, nullptr, lfo_ids);

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
