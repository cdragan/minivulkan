// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_effect_expansion.h"

#include <assert.h>
#include <string.h>

// ponytail: first-fit scans at most 68 live ranges; relocation is needed to
// admit every byte-fitting bank despite pinned-state fragmentation.
static bool place_effect_state(const Synth::EffectExpansionPlan& plan, uint32_t base, uint32_t bytes, uint32_t* offset)
{
    uint32_t cursor = 0;
    while (cursor <= Synth::effect_state_budget - bytes) {
        uint32_t next = cursor;
        for (uint32_t chain = 0; chain <= Synth::max_channels; ++chain) {
            for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
                const Synth::EffectSlotPlan& occupied = plan.slots[chain][slot];
                if (! occupied.state_offs) {
                    continue;
                }
                const uint32_t begin = occupied.state_offs - base;
                const uint32_t end   = begin + Synth::get_effect_state_bytes(occupied.allocated_for);
                if (cursor < end && begin < cursor + bytes && end > next) {
                    next = end;
                }
            }
        }
        if (next == cursor) {
            *offset = base + cursor;
            return true;
        }
        cursor = next;
    }
    return false;
}

namespace Synth {

namespace {

// Effect-state region inside the device data buffer, carved once at init. Slot offsets
// are absolute device byte offsets (region base + relative placement).
uint32_t state_region_base = 0;

// The plan currently committed; a re-expansion preserves its per-slot state offsets.
EffectExpansionPlan current_plan;
bool                has_current_plan = false;

// Only the final plan dispatches. Retained unrendered ranges carry needs_clear;
// abandoned ranges need no fill, and final-plan ranges never overlap.
constexpr uint32_t max_pending_clears = (max_channels + 1) * max_chain_effects;
EffectClearRange   pending_clears[max_pending_clears];
uint32_t           num_pending_clears = 0;

// Chain bindings in a fixed index scheme: [0, max_channels) are the channel chains,
// [max_channels] is the master chain.
const EffectChainBinding* bank_chains(const InstrumentBank& bank, uint32_t chain_idx)
{
    return (chain_idx < max_channels) ? &bank.channel_chains[chain_idx] : &bank.master_chain;
}

bool valid_source_op(uint32_t op)
{
    return op <= static_cast<uint32_t>(SourceOp::multiply);
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an
// effect's context (the editor-side validator applies the same rule at publish).
bool is_channel_effect_source(uint32_t source)
{
    return source <= static_cast<uint32_t>(ModSource::channel_pressure);
}

static bool finite_value(float value)
{
    return value - value == 0.0f;
}

// One effect param's binding. Mirrors every rule the commit relies on, so a passing
// preflight guarantees the commit cannot index out of bounds or configure garbage:
// finite floats, valid ops, LFO descriptors within the bank pool, bounded inputs, and
// channel-wide MIDI sources only (the master chain admits no MIDI-driven source at all,
// direct or via LFO depth/rate: it has no channel inputs).
bool validate_effect_param_binding(const EffectParamBinding& binding, bool is_master, uint32_t num_lfos)
{
    if (! finite_value(binding.base_value) || ! finite_value(binding.lfo_depth) || ! finite_value(binding.lfo_rate_scale)) {
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
        if (binding.lfo_depth_source != ModSource::none || binding.lfo_rate_source != ModSource::none ||
            binding.num_inputs) {
            return false;
        }
    }
    else {
        if (! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_depth_source)) ||
            ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_rate_source))) {
            return false;
        }
        if (binding.num_inputs > max_mod_inputs) {
            return false;
        }
        for (uint32_t input = 0; input < binding.num_inputs; input++) {
            const ModInput& mod_input = binding.inputs[input];
            if (! is_channel_effect_source(static_cast<uint32_t>(mod_input.source)) ||
                ! valid_source_op(static_cast<uint32_t>(mod_input.op)) || ! finite_value(mod_input.scale)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

void init_effect_state_region(uint32_t region_base_offset)
{
    state_region_base  = region_base_offset;
    has_current_plan   = false;
    current_plan       = EffectExpansionPlan();
    num_pending_clears = 0;
}

bool preflight_effect_expansion(const InstrumentBank& bank, EffectExpansionPlan* out_plan, const char** error)
{
    const EffectExpansionPlan* const prev = has_current_plan ? &current_plan : nullptr;

    // Built locally and copied out only on success: on failure the caller's plan (and
    // everything else) is untouched. Value-init: stateless slots must carry zero offsets.
    EffectExpansionPlan plan = {};

    uint32_t num_modulated = 0;

    for (uint32_t chain_idx = 0; chain_idx <= max_channels; chain_idx++) {
        const EffectChainBinding& chain = *bank_chains(bank, chain_idx);

        if (chain.num_effects > max_chain_effects) {
            *error = "effect chain exceeds per-chain capacity";
            return false;
        }

        for (uint32_t slot = 0; slot < chain.num_effects; slot++) {
            const EffectSlotBinding& binding = chain.effects[slot];
            if (static_cast<uint32_t>(binding.type) >= num_effect_types) {
                *error = "effect chain names an invalid effect type";
                return false;
            }

            const uint32_t state_bytes =
                (binding.enabled && binding.type != EffectType::none) ? get_effect_state_bytes(binding.type) : 0;

            const uint32_t num_params = get_effect_param_floats(binding.type);
            for (uint32_t param = 0; param < num_params; param++) {
                const EffectParamBinding& param_binding = binding.bindings[param];
                if (! validate_effect_param_binding(param_binding,
                                                    chain_idx == max_channels,
                                                    bank.lfos.num_allocated)) {
                    *error = "effect chain carries an invalid parameter binding";
                    return false;
                }
                if (param_binding.lfo_desc_id || param_binding.num_inputs) {
                    ++num_modulated;
                    plan.num_nodes += 1 + (param_binding.lfo_desc_id ? 1u : 0u);
                }
            }

            if (! state_bytes) {
                continue; // stateless or disabled: state_offs stays 0, nothing to clear
            }

            if (state_bytes > effect_state_budget - plan.consumed_bytes) {
                *error = "effect state budget exceeded";
                return false;
            }
            plan.consumed_bytes += state_bytes;
            // Reserve all compatible live ranges before placing any new state.
            if (prev) {
                const EffectSlotPlan& prev_slot = prev->slots[chain_idx][slot];
                if (prev_slot.state_offs && prev_slot.allocated_for == binding.type) {
                    plan.slots[chain_idx][slot] = prev_slot;
                    continue;
                }
            }
            plan.slots[chain_idx][slot] = { 0, true, binding.type };
        }
    }

    if (num_modulated > max_effect_mod_params) {
        *error = "too many modulated effect params for the modulation pool";
        return false;
    }

    for (uint32_t chain = 0; chain <= max_channels; ++chain) {
        for (uint32_t slot = 0; slot < max_chain_effects; ++slot) {
            EffectSlotPlan& allocation = plan.slots[chain][slot];
            if (allocation.needs_clear && ! allocation.state_offs &&
                ! place_effect_state(plan,
                                     state_region_base,
                                     get_effect_state_bytes(allocation.allocated_for),
                                     &allocation.state_offs)) {
                *error = "effect state has no contiguous free range";
                return false;
            }
        }
    }
    *out_plan = plan;
    return true;
}

void commit_effect_expansion(const InstrumentBank&      bank,
                             const EffectExpansionPlan& plan,
                             EffectChain*               out_channel_chains, // [max_channels]
                             EffectChain*               out_master_chain,
                             const EffectNodeWriter&    writer)
{
    num_pending_clears = 0;
    for (uint32_t chain_idx = 0; chain_idx <= max_channels; chain_idx++) {
        const EffectChainBinding& binding = *bank_chains(bank, chain_idx);
        EffectChain& chain = (chain_idx < max_channels) ? out_channel_chains[chain_idx] : *out_master_chain;

        chain.num_effects = binding.num_effects;
        for (uint32_t slot = 0; slot < max_chain_effects; slot++) {
            EffectInstance& instance = chain.effects[slot];
            instance                 = EffectInstance();
            if (slot >= binding.num_effects) {
                continue;
            }

            const EffectSlotBinding& slot_binding = binding.effects[slot];
            instance.type                         = slot_binding.type;
            instance.enabled                      = slot_binding.enabled;
            instance.state_offs                   = plan.slots[chain_idx][slot].state_offs;

            const uint32_t state_bytes = get_effect_state_bytes(slot_binding.type);
            const uint32_t num_params  = get_effect_param_floats(slot_binding.type);
            for (uint32_t param = 0; param < num_params; param++) {
                const EffectParamBinding& param_binding = slot_binding.bindings[param];

                // Every value commit consumes is asserted, including for unmodulated params.
                assert(finite_value(param_binding.base_value) && finite_value(param_binding.lfo_depth) &&
                       finite_value(param_binding.lfo_rate_scale));
                assert(param_binding.num_inputs <= max_mod_inputs);

                instance.params[param] = param_binding.base_value;

                if (! param_binding.lfo_desc_id && ! param_binding.num_inputs) {
                    continue;
                }

                assert(param_binding.lfo_op == SourceOp::add || param_binding.lfo_op == SourceOp::multiply);
                assert(param_binding.lfo_desc_id <= bank.lfos.num_allocated);
                assert(chain_idx != max_channels ||
                       (param_binding.num_inputs == 0 && param_binding.lfo_depth_source == ModSource::none &&
                        param_binding.lfo_rate_source == ModSource::none));
                for (uint32_t input = 0; input < param_binding.num_inputs; input++) {
                    assert(is_channel_effect_source(static_cast<uint32_t>(param_binding.inputs[input].source)));
                    assert(param_binding.inputs[input].op == SourceOp::add ||
                           param_binding.inputs[input].op == SourceOp::multiply);
                    assert(finite_value(param_binding.inputs[input].scale));
                }

                const uint32_t dest_node = writer.alloc_node(writer.ctx);
                assert(dest_node); // the preflight counted this node

                uint32_t lfo_node = 0;
                if (param_binding.lfo_desc_id) {
                    lfo_node = writer.alloc_node(writer.ctx);
                    assert(lfo_node);
                    writer.configure_lfo(writer.ctx,
                                         lfo_node,
                                         param_binding.lfo_desc_id,
                                         param_binding.lfo_op,
                                         param_binding.lfo_depth,
                                         writer.resolve_source(writer.ctx, param_binding.lfo_depth_source, chain_idx),
                                         writer.resolve_source(writer.ctx, param_binding.lfo_rate_source, chain_idx),
                                         param_binding.lfo_rate_scale);
                }

                SourceParam sources[max_mod_inputs];
                for (uint32_t input = 0; input < param_binding.num_inputs; input++) {
                    const ModInput& mod_input = param_binding.inputs[input];
                    sources[input]            = { writer.resolve_source(writer.ctx, mod_input.source, chain_idx),
                                                  mod_input.scale,
                                                  mod_input.op };
                }

                writer.configure_dest(writer.ctx,
                                      dest_node,
                                      static_cast<uint16_t>(lfo_node),
                                      param_binding.base_value,
                                      param_binding.lfo_op,
                                      sources,
                                      param_binding.num_inputs);

                instance.src_param_id[param] = static_cast<uint16_t>(dest_node);
            }

            if (plan.slots[chain_idx][slot].needs_clear) {
                assert(state_bytes);
                assert(num_pending_clears < max_pending_clears);
                pending_clears[num_pending_clears++] = { instance.state_offs, state_bytes };
            }
        }
    }

    current_plan     = plan;
    has_current_plan = true;
}

EffectClearList take_effect_clear_ranges()
{
    const EffectClearList list = { pending_clears, num_pending_clears };
    num_pending_clears         = 0;
    for (uint32_t chain = 0; chain <= max_channels; ++chain) {
        for (uint32_t slot = 0; slot < max_chain_effects; ++slot) {
            current_plan.slots[chain][slot].needs_clear = false;
        }
    }
    return list;
}

} // namespace Synth
