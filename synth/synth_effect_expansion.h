// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_parameters.h"

namespace Synth {

// One runtime effect instance: the working copy the render path reads. Base values come
// from the bank's bindings at commit; modulated params are overwritten each step from the
// graph node recorded in src_param_id (0 = unmodulated constant).
struct EffectInstance {
    EffectType type;
    bool       enabled;
    float      params[max_effect_param_floats];
    uint32_t   state_offs; // byte offset into the device data buffer; 0 when stateless
    uint16_t   src_param_id[max_effect_param_floats];
};

struct EffectChain {
    uint32_t       num_effects;
    EffectInstance effects[max_chain_effects];
};

// Preflight result for one effect slot: where its persistent state lives in the device
// buffer (0 = stateless), whether that state is freshly allocated and must be zeroed
// before its first shader use, and which effect the offset was sized for (a re-expansion
// preserves the offset while the slot keeps running the same enabled effect).
struct EffectSlotPlan {
    uint32_t   state_offs;
    bool       needs_clear;
    EffectType allocated_for;
};

// Pure plan for expanding a bank's effect chains: per-slot state placement plus the
// modulation-pool node count the commit will need. Index [max_channels] is the master
// chain.
struct EffectExpansionPlan {
    EffectSlotPlan slots[max_channels + 1][max_chain_effects];
    uint32_t       num_nodes;      // dest + optional LFO leaf per modulated param
    uint32_t       consumed_bytes; // region bytes consumed if this plan commits
};

// One contiguous device byte range to zero before its effect's first shader use.
struct EffectClearRange {
    uint32_t offset;
    uint32_t bytes;
};

struct EffectClearList {
    const EffectClearRange* ranges;
    uint32_t                count;
};

// Points the expansion at the carved effect-state region inside the device data buffer and
// resets all expansion state (plan, consumption, pending clears). The region must be
// aligned to effect_state_alignment; the host asserts the device honors that alignment.
void init_effect_state_region(uint32_t region_base_offset);

// Pure preflight of a candidate bank's effect chains. Computes per-slot state placement
// (preserving a slot's previous offset while it keeps running the same enabled effect),
// counts required modulation nodes, and enforces every rule the commit relies on: chain
// capacity, effect types, binding validity (finite values, ops, LFO descriptors within
// the bank pool, bounded channel-wide MIDI inputs, master chain LFO-only), the state
// budget and the modulated-param limit. On failure returns false with a static error
// string and mutates nothing - the previously committed chains and state remain valid
// and sounding.
bool preflight_effect_expansion(const InstrumentBank& bank,
                                EffectExpansionPlan*  out_plan,
                                const char**          error);

// Host callbacks the commit uses to configure modulation nodes over the runtime's
// parameter arrays. After a passing preflight every callback is guaranteed to succeed.
struct EffectNodeWriter {
    const void* ctx;
    uint32_t (*alloc_node)(const void* ctx); // next free modulation-pool node id
    uint16_t (*resolve_source)(const void* ctx, ModSource source, uint32_t channel);
    void (*configure_dest)(const void* ctx, uint32_t node, uint16_t lfo_node, float base_value,
                           SourceOp lfo_op, const SourceParam* sources, uint32_t num_inputs);
    void (*configure_lfo)(const void* ctx, uint32_t node, uint16_t lfo_desc_id,
                          SourceOp lfo_op, float lfo_depth,
                          uint16_t depth_source, uint16_t rate_source, float rate_scale);
};

// Applies a passing plan: fills the runtime chains from the bank (base values into
// params[], state offsets and source nodes from the plan), records clear ranges for
// freshly allocated state, and makes the plan current (preserving future re-expansions).
// Infallible by construction after a passing preflight.
void commit_effect_expansion(const InstrumentBank& bank,
                             const EffectExpansionPlan& plan,
                             EffectChain* out_channel_chains, // [max_channels]
                             EffectChain* out_master_chain,
                             const EffectNodeWriter& writer);

// Hands the clear ranges recorded since the last take to the render loop, which zeroes
// them before any effect dispatch. Single consumer, same thread as commit.
EffectClearList take_effect_clear_ranges();

} // namespace Synth
