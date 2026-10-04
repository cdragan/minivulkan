// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Projection between one effect chain binding (a channel chain or the master
// chain) and the shared graph widget.  The chain is the single source of
// truth: the projection mirrors it, every user edit flows back through
// drained change events or the pane's canvas commands, and structural
// edits re-project.  No ImGui here: synth_unit links this file.

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_graph.h"

#include <stdint.h>

namespace Sculptor {

// One scalar effect parameter's editing metadata, in shader param order.
struct EffectParamInfo {
    char  name[24];
    float min_value;
    float max_value;
    bool  logarithmic;
};

// One effect type's node rows.  Slots of type none project no node at all,
// so every projected effect has at least one parameter row.
struct EffectTypeInfo {
    char            name[24];
    EffectParamInfo params[Synth::max_effect_param_floats];
    uint8_t         num_params;
};

// Display metadata for the six real effect types; the index is the type.
// The bounds are editing aids (sliders clamp on interaction), not DSP
// limits: the shaders already clamp or tolerate out-of-range params.
const EffectTypeInfo& effect_type_info(Synth::EffectType type);

// Neutral, audible base value for a newly added or retyped effect slot.
float effect_param_default(Synth::EffectType type, uint32_t param);

// Chain index of the master chain (max_channels doubles as the chain count).
constexpr uint32_t fx_master_chain = Synth::max_channels;

// The chain binding a chain index selects; 0..15 select channels, 16 the
// master chain.  Non-const, for the edit paths.
Synth::EffectChainBinding& fx_graph_chain(Synth::InstrumentBank* bank, uint32_t chain);

// Where the projected nodes of one chain live in the shared graph widget.
// Node indices are valid until the next re-projection of any graph pane.
// Channel-wide MIDI sources only: effects are not note-triggered, so the
// per-voice sources (velocity and aftertouch) are not routable to them.
constexpr uint32_t fx_num_input_sources = 3;

struct EffectGraphMapping {
    uint32_t input_node;                              // fixed source endpoint
    uint32_t output_node;                             // fixed sink endpoint
    uint32_t effect_nodes[Synth::max_chain_effects];  // pool_no_slot for unused or none slots
    uint32_t lfo_nodes[Synth::max_lfos];              // by descriptor id - 1; pool_no_slot when unreferenced
    uint32_t lfo_count;                               // number of live LFO nodes
    uint32_t midi_node;                               // channel chains only: MIDI source node; pool_no_slot on master
    uint32_t midi_source_slots[fx_num_input_sources]; // output dot per channel-wide MIDI source
    uint32_t chain;                                   // channel index or fx_master_chain
};

// Chain slot an effect node projects, or -1 when the node is not an effect.
int32_t fx_effect_slot_of(const EffectGraphMapping& mapping, uint32_t node_idx);

// Descriptor id an LFO node projects, or 0 when the node is not an LFO node.
uint32_t fx_lfo_desc_of(const EffectGraphMapping& mapping, uint32_t node_idx);
// Number of nodes the chain's projection needs: fixed endpoints, one per
// projected effect slot, and one per pinned-or-referenced LFO descriptor.
uint32_t fx_projected_node_count(const Synth::InstrumentBank& bank, uint32_t chain, const bool* pinned_lfos);
// Rebuilds the graph from the chain: Input -> effect nodes -> Output serial
// wires, plus one LFO node per descriptor the chain's meaningful bindings
// reference (deduplicated by id, shared with every other chain unchanged)
// and per entry the caller pinned (a freshly added, not yet wired LFO must
// stay on the canvas to receive its first wire).  Each LFO node carries one
// reference wire per parameter row it modulates.  Wires are editable: the
// validator installed by the editor restricts what may connect, and the
// apply path turns wire edits into chain reorders and LFO bindings.
// Returns false only on graph capacity exhaustion, leaving the graph
// partially rebuilt (the caller re-projects from a valid bank).
bool project_effect_chain_to_graph(const Synth::InstrumentBank& bank,
                                   uint32_t                     chain,
                                   Graph*                       graph,
                                   EffectGraphMapping*          mapping,
                                   const bool*                  pinned_lfos = nullptr);

// Applies one drained graph change to the bank through the mapping:
// value_changed edits Enabled rows, parameter base values, the LFO op and
// depth rows and LFO descriptor fields (shared descriptors edit every
// user at once); node_deleted removes an effect slot or clears this chain's
// references to a deleted LFO node; connection edits reorder the chain
// (a serial wire into a node places that node right after the wire's
// source), bind or unbind LFO descriptors onto parameter rows, and refuse
// serial-wire disconnects (the chain wire carries audio; deleting an
// effect is the node menu's Delete).  Connection events whose endpoints
// died with a node deleted in the same batch are byproducts and change
// nothing.  The change carries indices only, so the slot's live value is
// read from the graph.  Returns false when the change cannot be
// applied.
bool apply_fx_graph_change(Synth::InstrumentBank*    bank,
                           const Graph&              graph,
                           const EffectGraphMapping& mapping,
                           const GraphChange&        change);

// Removes chain slot `slot`, shifting later slots down and zeroing the freed
// tail slot so no dormant binding survives the reclaim.
void fx_remove_effect(Synth::EffectChainBinding* chain, uint32_t slot);

// Reorders the chain: the moved slot is spliced to the front when
// after_slot is negative, right after after_slot otherwise.  False when a
// slot is out of range; caller guards the no-op (moved == after).
bool fx_splice_effect(Synth::EffectChainBinding* chain, uint32_t moved_slot, int32_t after_slot);

// Re-initializes a slot to a new type with neutral base values and every
// modulation field cleared, so a retyped slot never carries dormant
// references of the old type.
void fx_init_slot(Synth::EffectChainBinding* chain, uint32_t slot, Synth::EffectType type);

// Clears the LFO reference of every meaningful binding in one chain that
// names desc_id; other chains and oscillator uses keep their references.
void fx_clear_lfo_references(Synth::EffectChainBinding* chain, uint16_t desc_id);

// Allocates a fresh LFO descriptor (sine, 250 ms) in the bank's pool; false
// with the bank unmodified when the descriptor pool is full.  Wiring it to
// a parameter row is what binds it (the apply path's connection handler).
bool allocate_default_lfo(Synth::InstrumentBank* bank, uint16_t* out_desc_id);

// Parameter layout on an effect node: In and Out share row group 1
// (input dot left, output dot right), Enabled sits at slot 2, then every
// parameter owns ten consecutive slots: its base-value slider on its own
// line, the LFO modulation row (input dot, op list, depth), then the two
// MIDI source rows (input dot, op list, amount each) - the oscillator
// editor's parameter-node grammar.
constexpr uint32_t fx_param_stride = 10;

// First (base-value slider) slot index of projected param `param`.
constexpr uint32_t fx_param_row(uint32_t param)
{
    return 3u + fx_param_stride * param;
}

// The LFO input dot of projected param `param`: the wire endpoint of an
// LFO binding.
constexpr uint32_t fx_param_lfo_dot(uint32_t param)
{
    return fx_param_row(param) + 1u;
}

// The MIDI source input dot of projected param `param`, source row `input`
// (0 = Source A, 1 = Source B).
constexpr uint32_t fx_param_src_dot(uint32_t param, uint32_t input)
{
    return fx_param_row(param) + 4u + 3u * input;
}

} // namespace Sculptor
