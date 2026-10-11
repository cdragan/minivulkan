// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_effect_expansion.h"

#include "synth_midi_cc.h"

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

namespace {

// Effect-state region inside the device data buffer, carved once at init. Slot offsets
// are absolute device byte offsets (region base + relative placement).
uint32_t state_region_base = 0;

// The plan currently committed; a re-expansion preserves its per-slot state offsets.
Synth::EffectExpansionPlan current_plan;
bool                       has_current_plan = false;

// Only the final plan dispatches. Retained unrendered ranges carry needs_clear;
// abandoned ranges need no fill, and final-plan ranges never overlap.
constexpr uint32_t      max_pending_clears = (Synth::max_channels + 1) * Synth::max_chain_effects;
Synth::EffectClearRange pending_clears[max_pending_clears];
uint32_t                num_pending_clears = 0;

} // namespace

// Chain bindings in a fixed index scheme: [0, max_channels) are the channel chains,
// [max_channels] is the master chain.
static const Synth::EffectChainBinding* bank_chains(const Synth::InstrumentBank& bank, uint32_t chain_idx)
{
    return (chain_idx < Synth::max_channels) ? &bank.channel_chains[chain_idx] : &bank.master_chain;
}

static bool valid_source_op(uint32_t op)
{
    return op <= static_cast<uint32_t>(Synth::SourceOp::multiply);
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an
// effect's context (the editor-side validator applies the same rule at publish).
static bool is_channel_effect_source(uint32_t source)
{
    return source <= static_cast<uint32_t>(Synth::ModSource::channel_pressure);
}

// Effect param inputs admit the assignable CC sources; the CC number must name
// CC 16..31, and non-CC inputs must carry no stale CC number.  The source and
// CC half of is_valid_effect_mod_input.
bool Synth::is_valid_effect_input(const ModInput& input)
{
    if (input.source == ModSource::cc) {
        return input.cc_number >= cc_msb_base && input.cc_number < cc_msb_base + num_assignable_ccs;
    }
    return is_channel_effect_source(static_cast<uint32_t>(input.source)) && input.cc_number == 0;
}

static bool finite_value(float value)
{
    return value - value == 0.0f;
}

// Full acceptance for an effect ModInput, param input or enable routing alike.
// Shared with the editor bank validator so load-time and expansion-time
// acceptance cannot diverge.
bool Synth::is_valid_effect_mod_input(const ModInput& input)
{
    return is_valid_effect_input(input) && valid_source_op(static_cast<uint32_t>(input.op)) &&
           finite_value(input.scale);
}

// One effect param's binding. Mirrors every rule the commit relies on, so a passing
// preflight guarantees the commit cannot index out of bounds or configure garbage:
// finite floats, valid ops, LFO descriptors within the bank pool, bounded inputs, and
// channel-wide MIDI sources only. Master-chain bindings follow the same source rules
// as channel chains; the commit resolves their sources from channel index 0.
static bool validate_effect_param_binding(const Synth::EffectParamBinding& binding, uint32_t num_lfos)
{
    if (! finite_value(binding.base_value) || ! finite_value(binding.lfo_depth) ||
        ! finite_value(binding.lfo_rate_scale)) {
        return false;
    }
    if (! valid_source_op(static_cast<uint32_t>(binding.lfo_op))) {
        return false;
    }
    // With dense pools, a 1-based descriptor id refers to an occupied entry iff id <= num_allocated.
    if (binding.lfo_desc_id > num_lfos) {
        return false;
    }
    if (! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_depth_source)) ||
        ! is_channel_effect_source(static_cast<uint32_t>(binding.lfo_rate_source))) {
        return false;
    }
    if (binding.num_inputs > Synth::max_mod_inputs) {
        return false;
    }
    for (uint32_t input = 0; input < binding.num_inputs; input++) {
        const Synth::ModInput& mod_input = binding.inputs[input];
        if (! is_valid_effect_mod_input(mod_input)) {
            return false;
        }
    }
    return true;
}

bool Synth::effect_live_enabled(uint16_t enable_param_id, float resolved_leaf_value, bool saved_enabled)
{
    if (! enable_param_id) {
        return saved_enabled;
    }
    // The raw 14-bit midpoint: MSB 63 tops out at 8191, MSB 64 starts at 8192.
    constexpr float enable_threshold = 8192.0f / 16383.0f;
    return resolved_leaf_value >= enable_threshold;
}

bool Synth::effect_needs_reset(bool previous_live_enabled, bool live_enabled)
{
    return ! previous_live_enabled && live_enabled;
}

void Synth::init_effect_state_region(uint32_t region_base_offset)
{
    state_region_base  = region_base_offset;
    has_current_plan   = false;
    current_plan       = EffectExpansionPlan();
    num_pending_clears = 0;
}

bool Synth::preflight_effect_expansion(const InstrumentBank& bank, EffectExpansionPlan* out_plan, const char** error)
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

            // The enable routing obeys the same source rules as param inputs,
            // so a bank the preflight accepts cannot feed the resolve garbage.
            if (! is_valid_effect_mod_input(binding.enable_input)) {
                *error = "effect chain carries an invalid enable routing";
                return false;
            }

            // Routed-but-saved-off slots reserve state too: the routing can
            // flip them live mid-run, so their state must exist and start
            // cleared.
            const uint32_t state_bytes = (binding.type != EffectType::none &&
                                          (binding.enabled || binding.enable_input.source != ModSource::none))
                                             ? get_effect_state_bytes(binding.type)
                                             : 0;

            const uint32_t num_params = get_effect_param_floats(binding.type);
            for (uint32_t param = 0; param < num_params; param++) {
                const EffectParamBinding& param_binding = binding.bindings[param];
                if (! validate_effect_param_binding(param_binding, bank.lfos.num_allocated)) {
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

void Synth::commit_effect_expansion(const InstrumentBank&      bank,
                                    const EffectExpansionPlan& plan,
                                    EffectChain*               out_channel_chains, // [max_channels]
                                    EffectChain*               out_master_chain,
                                    const EffectNodeWriter&    writer)
{
    num_pending_clears = 0;
    for (uint32_t chain_idx = 0; chain_idx <= max_channels; chain_idx++) {
        // MIDI sources resolve per chain: channel chains from their own channel,
        // the master chain from channel index 0 (steering via channel 1).
        const uint32_t            midi_channel = (chain_idx < max_channels) ? chain_idx : 0;
        const EffectChainBinding& binding      = *bank_chains(bank, chain_idx);
        EffectChain& chain = (chain_idx < max_channels) ? out_channel_chains[chain_idx] : *out_master_chain;

        chain.num_effects = binding.num_effects;
        for (uint32_t slot = 0; slot < max_chain_effects; slot++) {
            EffectInstance& instance = chain.effects[slot];

            // Captured before the overwrite: a retained slot continues from
            // the previous instance's live enable state.
            const EffectInstance previous = instance;

            instance = EffectInstance();
            if (slot >= binding.num_effects) {
                continue;
            }

            const EffectSlotBinding& slot_binding = binding.effects[slot];
            instance.type                         = slot_binding.type;
            instance.state_offs                   = plan.slots[chain_idx][slot].state_offs;

            // The plan retains a slot's state range while it keeps running
            // the same effect type, so a retained slot was live (or routed)
            // continuously across the publish: its new instance continues
            // from the previous live enable state, and a publish never looks
            // like an off->on transition for an effect that was live
            // immediately before it.  A fresh instance seeds from the saved
            // checkbox bit.  Same slot-identity rule the preflight's
            // state_offs retention uses.
            const EffectSlotPlan& prev_slot = current_plan.slots[chain_idx][slot];
            const bool            retained  = has_current_plan && instance.state_offs &&
                                              prev_slot.state_offs == instance.state_offs &&
                                              prev_slot.allocated_for == slot_binding.type;
            instance.enabled                = retained ? previous.enabled : slot_binding.enabled;
            instance.saved_enabled          = slot_binding.enabled;

            // The wired enable routing resolves like a param source (channel
            // chains from their own channel, the master chain from channel
            // index 0) but consumes no modulation node: the runtime reads the
            // resolved leaf directly at the enable control point.
            if (slot_binding.enable_input.source != ModSource::none) {
                assert(is_valid_effect_mod_input(slot_binding.enable_input));
                instance.enable_param_id = writer.resolve_source(writer.ctx,
                                                                 slot_binding.enable_input.source,
                                                                 slot_binding.enable_input.cc_number,
                                                                 midi_channel);
            }

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
                for (uint32_t input = 0; input < param_binding.num_inputs; input++) {
                    assert(is_valid_effect_input(param_binding.inputs[input]));
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
                    // Resolve the sources into locals before the configure call.
                    // The writer records the resolve order, and argument
                    // evaluation order is unspecified in C++, so compilers may
                    // disagree.  The depth source must resolve first.
                    const uint16_t depth_source =
                        writer.resolve_source(writer.ctx, param_binding.lfo_depth_source, 0, midi_channel);
                    const uint16_t rate_source =
                        writer.resolve_source(writer.ctx, param_binding.lfo_rate_source, 0, midi_channel);
                    writer.configure_lfo(writer.ctx,
                                         lfo_node,
                                         param_binding.lfo_desc_id,
                                         param_binding.lfo_op,
                                         param_binding.lfo_depth,
                                         depth_source,
                                         rate_source,
                                         param_binding.lfo_rate_scale);
                }

                SourceParam sources[max_mod_inputs];
                for (uint32_t input = 0; input < param_binding.num_inputs; input++) {
                    const ModInput& mod_input = param_binding.inputs[input];
                    sources[input]            = {
                        writer.resolve_source(writer.ctx, mod_input.source, mod_input.cc_number, midi_channel),
                        mod_input.scale,
                        mod_input.op
                    };
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

Synth::EffectClearList Synth::take_effect_clear_ranges()
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
