// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_parameters.h"

namespace Synth {

// Per-band live values for a FIR effect instance, in FirEffectBands index order: index 0
// is band 0's gain, then bands 1..3 each carry lowpass, highpass, gain and slope.
// src_param_id is the resolved modulation node (0 = unmodulated constant).
struct FirEffectBandValues {
    uint8_t  band_count;
    float    params[num_fir_effect_band_params];
    uint16_t src_param_id[num_fir_effect_band_params];
};

// One runtime effect instance: the working copy the render path reads. Base values come
// from the bank's bindings at commit; modulated params are overwritten each step from the
// graph node recorded in src_param_id (0 = unmodulated constant).
struct EffectInstance {
    EffectType type;
    // Live enable state: a wired routing's per-block threshold decision, or
    // the saved checkbox for an unwired instance.  At the enable control
    // point this holds the previous block's decision, which is what the
    // RESET edge compares.
    bool enabled;
    // The slot's saved checkbox bit as committed.  An unwired instance's
    // live decision reads this, so a checkbox toggle crossing a publish
    // stays visible as an off->on transition at the runtime control point.
    bool     saved_enabled;
    float    params[max_effect_param_floats];
    uint32_t state_offs; // byte offset into the device data buffer; 0 when stateless
    uint16_t src_param_id[max_effect_param_floats];
    FirEffectBandValues fir_bands; // FIR effect only; refreshed like params
    // Resolved leaf parameter id of the slot's wired enable routing (0 =
    // unwired, the saved checkbox alone decides).  Filled at commit through
    // the same per-chain channel the param sources resolve from.
    uint16_t enable_param_id;
};

struct EffectChain {
    uint32_t       num_effects;
    EffectInstance effects[max_chain_effects];
};

// Preflight result for one effect slot: where its persistent state lives in the device
// buffer (0 = stateless), whether that state still needs initialization and must be zeroed
// before its first shader use, and which effect the offset was sized for (a re-expansion
// preserves the offset while the slot keeps running the same effect type, enabled or
// enable-routed).
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
    uint32_t       consumed_bytes; // live allocated bytes if this plan commits
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
// resets all expansion state (plan and pending clears). The region must be
// aligned to effect_state_alignment; the host asserts the device honors that alignment.
// Effect param inputs admit the assignable CC sources: the CC number must name
// CC 16..31, and non-CC inputs must carry no stale CC number.  The source and
// CC half of is_valid_effect_mod_input, which adds the op and scale rules.
bool is_valid_effect_input(const ModInput& input);

// Full acceptance rule for an effect ModInput, param input or enable routing
// alike: the source/CC rules of is_valid_effect_input plus an add-or-multiply
// op and a finite scale.  Shared by the runtime preflight, the commit asserts
// and the editor bank validator, so load-time and expansion-time acceptance
// cannot diverge.
bool is_valid_effect_mod_input(const ModInput& input);

void init_effect_state_region(uint32_t region_base_offset);

// Pure preflight of a candidate bank's effect chains. Computes per-slot state placement
// (preserving a slot's previous offset while it keeps running the same enabled effect),
// counts required modulation nodes, and enforces every rule the commit relies on: chain
// capacity, effect types, binding validity (finite values, ops, LFO descriptors within
// the bank pool, bounded channel-wide MIDI inputs - master chains follow the same source
// rules as channel chains, resolved from channel index 0), the state budget and the
// modulated-param limit. New state uses holes outside retained live
// ranges; fragmentation may refuse a bank whose live byte sum fits. On failure returns false with a static error
// string and mutates nothing - the previously committed chains and state remain valid
// and sounding.
bool preflight_effect_expansion(const InstrumentBank& bank, EffectExpansionPlan* out_plan, const char** error);

// Host callbacks the commit uses to configure modulation nodes over the runtime's
// parameter arrays. After a passing preflight every callback is guaranteed to succeed.
struct EffectNodeWriter {
    const void* ctx;
    uint32_t (*alloc_node)(const void* ctx); // next free modulation-pool node id
    // Resolves a channel-wide source to the writer's source node id. "channel" is the
    // MIDI channel the source resolves from: channel chains resolve from their own
    // channel; the master chain resolves from channel index 0.
    uint16_t (*resolve_source)(const void* ctx, ModSource source, uint8_t cc_number, uint32_t channel);
    void (*configure_dest)(const void*        ctx,
                           uint32_t           node,
                           uint16_t           lfo_node,
                           float              base_value,
                           SourceOp           lfo_op,
                           const SourceParam* sources,
                           uint32_t           num_inputs);
    void (*configure_lfo)(const void* ctx,
                          uint32_t    node,
                          uint16_t    lfo_desc_id,
                          SourceOp    lfo_op,
                          float       lfo_depth,
                          uint16_t    depth_source,
                          uint16_t    rate_source,
                          float       rate_scale);
};

// Applies a passing plan: fills the runtime chains from the bank (base values into
// params[], state offsets and source nodes from the plan), records clear ranges for
// new or retained-unrendered state, and makes the plan current. Only the final plan
// dispatches, so abandoned ranges need no clear.  A retained slot's instance
// continues from the previous instance's live enable state, so a publish never
// reads as an off->on transition for an effect that was sounding immediately
// before it; a fresh instance seeds from the saved checkbox bit.
// Infallible by construction after a passing preflight.
void commit_effect_expansion(const InstrumentBank&      bank,
                             const EffectExpansionPlan& plan,
                             EffectChain*               out_channel_chains, // [max_channels]
                             EffectChain*               out_master_chain,
                             const EffectNodeWriter&    writer);

// Live enable decision for one effect instance, evaluated per control block.
// enable_param_id != 0 (wired routing): the saved checkbox is ignored and the
// instance is live exactly when the resolved leaf value reaches the raw 14-bit
// on-threshold: the source's raw 14-bit value >= 8192 of 16383, i.e. MSB >= 64
// (MSB 63 tops out at 8191).  enable_param_id == 0 (unwired): the
// saved checkbox decides, as before.
bool effect_live_enabled(uint16_t enable_param_id, float resolved_leaf_value, bool saved_enabled);

// RESET decision for one control block: the state range is cleared exactly on
// the off->on live-enable transition, so a re-enabled effect starts from
// silence instead of replaying the state its last live run left behind.
bool effect_needs_reset(bool previous_live_enabled, bool live_enabled);

// Hands the latest plan's outstanding clear ranges to the render loop, which zeroes
// them before any effect dispatch, and retires needs_clear. Single consumer, same
// thread as commit; consume the list before another commit.
EffectClearList take_effect_clear_ranges();

} // namespace Synth
