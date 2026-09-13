// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_bank.h"

#include <cmath>
#include <stdio.h>

#include "../synth/synth_serialize.h"

namespace Synth {

void get_zone_name(const InstrumentBank* bank, uint32_t channel, uint32_t zone_entry, char* out, uint32_t out_size)
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
bool pool_is_dense(const PoolT& pool, uint32_t capacity)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        if (pool.is_occupied(i)) {
            count++;
        }
    }

    if (count != pool.num_allocated) {
        return false;
    }
    for (uint32_t i = 0; i < pool.num_allocated; i++) {
        if ( ! pool.is_occupied(i)) {
            return false;
        }
    }
    return true;
}

bool validate_envelope(const EnvelopeDescriptor& env)
{
    if (env.num_points < 1 || env.num_points > max_envelope_points) {
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

bool validate_lfo(const LFODescriptor& lfo)
{
    if (lfo.period_ms == 0) {
        return false;
    }
    // eval_lfo_normalized only implements sine and sawtooth; anything else asserts there.
    if (lfo.wave != WaveType::sine_wave && lfo.wave != WaveType::sawtooth_wave) {
        return false;
    }
    return true;
}

bool valid_mod_source(uint32_t source)
{
    return source <= static_cast<uint32_t>(ModSource::pressure_combine);
}

bool valid_source_op(uint32_t op)
{
    return op <= static_cast<uint32_t>(SourceOp::multiply);
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an
// effect's context (the runtime applies the same restriction at expansion).
bool is_channel_effect_source(uint32_t source)
{
    return source == static_cast<uint32_t>(ModSource::none)
        || source == static_cast<uint32_t>(ModSource::pitch_bend)
        || source == static_cast<uint32_t>(ModSource::mod_wheel)
        || source == static_cast<uint32_t>(ModSource::channel_pressure);
}

// One effect param's binding. The master chain admits no MIDI-driven source at all
// (it has no channel inputs, so MIDI modulation there would be meaningless).
bool validate_effect_param_binding(const EffectParamBinding& binding, bool is_master, uint32_t num_lfos)
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
        if (binding.lfo_depth_source != ModSource::none || binding.lfo_rate_source != ModSource::none ||
            binding.num_inputs) {
            return false;
        }
    }
    else {
        if ( ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_depth_source)) ||
             ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_rate_source))) {
            return false;
        }
        if (binding.num_inputs > max_mod_inputs) {
            return false;
        }
        for (uint32_t input = 0; input < binding.num_inputs; input++) {
            const ModInput& mod_input = binding.inputs[input];
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
bool validate_effect_chain(const EffectChainBinding& chain, bool is_master, uint32_t num_lfos,
                           uint32_t* num_modulated, uint32_t* state_bytes)
{
    if (chain.num_effects > max_chain_effects) {
        return false;
    }
    for (uint32_t slot = 0; slot < chain.num_effects; slot++) {
        const EffectSlotBinding& effect = chain.effects[slot];
        if (static_cast<uint32_t>(effect.type) >= num_effect_types) {
            return false;
        }
        const uint32_t num_params = get_effect_param_floats(effect.type);
        for (uint32_t param = 0; param < num_params; param++) {
            const EffectParamBinding& binding = effect.bindings[param];
            if ( ! validate_effect_param_binding(binding, is_master, num_lfos)) {
                return false;
            }
            if (binding.lfo_desc_id || binding.num_inputs) {
                (*num_modulated)++;
            }
        }
        if (effect.enabled && effect.type != EffectType::none) {
            *state_bytes += get_effect_state_bytes(effect.type);
        }
    }
    return true;
}

bool validate_instrument(const Instrument& instrument)
{
    if (instrument.layer_count < 1 || instrument.layer_count > max_layers) {
        return false;
    }
    for (uint32_t layer = 0; layer < instrument.layer_count; layer++) {
        const Oscillator& osc = instrument.layers[layer];
        if (osc.osc_mode >= 3) {
            return false;
        }
        for (uint32_t w = 0; w < 2; w++) {
            if (static_cast<uint32_t>(osc.osc_type[w]) > static_cast<uint32_t>(WaveType::noise_wave)) {
                return false;
            }
        }
        for (uint32_t target = 0; target < num_mod_targets; target++) {
            const LayerGen& gen = osc.gen[target];
            if ( ! valid_source_op(static_cast<uint32_t>(gen.lfo_op))) {
                return false;
            }
            if ( ! valid_mod_source(static_cast<uint32_t>(gen.lfo_depth_source)) ||
                ! valid_mod_source(static_cast<uint32_t>(gen.lfo_rate_source))) {
                return false;
            }
        }
    }
    for (uint32_t target = 0; target < num_mod_targets; target++) {
        const InputRouting& routing = instrument.routing[target];
        if (routing.num_inputs > max_mod_inputs) {
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

} // namespace

bool validate_instrument_bank(const InstrumentBank* bank)
{
    // Pool metadata bounds first (num_allocated drives runtime indexing), then density.
    if (bank->instruments.num_allocated > max_instruments ||
        bank->envelopes.num_allocated > max_envelopes ||
        bank->lfos.num_allocated > max_lfos ||
        bank->parameters.num_allocated > max_parameters) {
        return false;
    }
    if ( ! pool_is_dense(bank->instruments, max_instruments) ||
        ! pool_is_dense(bank->envelopes, max_envelopes) ||
        ! pool_is_dense(bank->lfos, max_lfos) ||
        ! pool_is_dense(bank->parameters, max_parameters)) {
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
        const Instrument& instrument = bank->instruments.entries[instr];
        for (uint32_t layer = 0; layer < instrument.layer_count; layer++) {
            for (uint32_t target = 0; target < num_mod_targets; target++) {
                const LayerGen& gen = instrument.layers[layer].gen[target];
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

    // Zones: nonzero starts strictly ascending; once a zero appears all later starts are zero;
    // a full table with no terminator is valid (select_instrument semantics). Reachability:
    // slot 0 is always reachable (its instrument is the note-0 answer), entries after the first
    // zero are unreachable - their instrument bytes are not validated as references.
    for (uint32_t channel = 0; channel < max_channels; channel++) {
        const Zone* const zones = bank->channel_zones[channel];

        // With dense pools, an id refers to an occupied entry iff id <= num_allocated (1-based
        // desc ids) / id < num_allocated (0-based instrument indices).
        if (zones[0].instrument >= bank->instruments.num_allocated) {
            return false;
        }

        uint32_t last_start = 0;
        bool terminated = false;
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

    // Effect chains: slot types, finite params, legal sources. The whole bank's modulated
    // effect params must fit the modulation pool; its enabled-effect state must fit the
    // device state budget.
    uint32_t num_modulated = 0;
    uint32_t state_bytes = 0;
    for (uint32_t channel = 0; channel < max_channels; channel++) {
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


const InstrumentBank* peek_bank_update(BankUpdateQueue* queue)
{
    const uint32_t head = queue->head.load(std::memory_order_relaxed);
    const uint32_t tail = queue->tail.load(std::memory_order_acquire);

    if (head == tail) {
        return nullptr;
    }
    return &queue->packets[head % bank_queue_capacity];
}

void consume_bank_update(BankUpdateQueue* queue)
{
    const uint32_t head = queue->head.load(std::memory_order_relaxed);
    queue->head.store(head + 1, std::memory_order_release);
}

bool push_bank_update(BankUpdateQueue* queue, const InstrumentBank& bank)
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

// Static-memory accounting. Budgets are upper bounds on the measured struct sizes; any
// later resident (bigger pools, more snapshots) must re-check the aggregate below.
static_assert(sizeof(InstrumentBank) <= 450'000);            // runtime + editor + scratch + rollback + pending
static_assert(sizeof(BankUpdateQueue) <= 2 * sizeof(InstrumentBank) + 32);
static_assert(7 * sizeof(InstrumentBank)
              + 10 * sizeof(InstrumentBank)
              + instrument_bank_header_size
              + 4 * 1638400u
              <= 16 * 1024 * 1024);

} // namespace Synth
