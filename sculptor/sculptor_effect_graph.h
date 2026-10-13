// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Projection of retained editor effect payloads and audio topology into the
// shared graph widget. Structural edits re-project; no ImGui dependency.

#pragma once

#include "../synth/synth_instrument.h"
#include "../synth/synth_midi_cc.h"
#include "sculptor_graph.h"

#include <stdint.h>

namespace Synth {
struct EffectsDocument;
struct InstrumentEditorBank;
} // namespace Synth

namespace Sculptor {

struct EffectAudioTopology;

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

// Recognizes exact bounded decimal LFO titles; custom spellings return false.
// A recognized title outside the descriptor pool returns descriptor id zero.
bool parse_effect_lfo_title(const char (&name)[32], uint16_t* descriptor_id);

// Translates canonical titles through a max_lfos-entry map; false leaves custom
// titles and mapped_id untouched.  A missing mapping returns true with id zero
// and leaves the title unchanged.
bool translate_effect_lfo_title(char (&name)[32], const uint16_t* lfo_ids, uint16_t* mapped_id);

// Label of one assignable CC pair: the MSB controller number and its paired
// LSB number, matching the assignable pair grammar (CC 16 rides CC 48).
// Writes at most capacity bytes, including the terminator.
void fx_cc_pair_label(uint32_t cc_number, char* name, size_t capacity);

// Whether a meaningful effect parameter binding references this LFO descriptor.
bool effect_chain_uses_lfo(const Synth::EffectChainBinding& chain, uint32_t descriptor_id);

// Whether a meaningful effect parameter binding input names the assignable
// CC `cc_number` (16..31).
bool fx_chain_uses_cc(const Synth::EffectChainBinding& chain, uint32_t cc_number);

// Wired assignable-CC mask of one chain: bit i names CC (16 + i) that a
// meaningful parameter input wires.  Pure; callers own mask mutation.
uint16_t fx_chain_wired_cc_mask(const Synth::EffectChainBinding& chain);

// Wired assignable-CC mask of one instrument's osc routing: bit i names
// CC (16 + i) that a routing input wires.  Library load and zone paste
// OR it into the destination channel's shared registry mask.
uint16_t instrument_cc_mask(const Synth::Instrument& instrument);

// Neutral, audible base value for a newly added or retyped effect slot.
float effect_param_default(Synth::EffectType type, uint32_t param);

// Chain index of the master chain (max_channels doubles as the chain count).
constexpr uint32_t fx_master_chain = Synth::max_channels;

// The chain binding a chain index selects; 0..15 select channels, 16 the
// master chain.  Non-const, for the edit paths.
Synth::EffectChainBinding& fx_graph_chain(Synth::InstrumentBank* bank, uint32_t chain);

// Where the projected nodes of one chain live in the shared graph widget.
// Node indices are valid until the next re-projection of any graph pane.
// The MIDI node carries the three channel-wide MIDI sources plus the
// chain's added-CC dots: effects are not note-triggered, so the per-voice
// sources (velocity and aftertouch) are not routable to them.
constexpr uint32_t fx_num_input_sources = 3;

struct EffectGraphMapping {
    uint32_t input_node;                             // fixed source endpoint
    uint32_t output_node;                            // fixed sink endpoint
    uint32_t effect_nodes[Synth::max_chain_effects]; // pool_no_slot for unused or none slots
    uint32_t lfo_nodes[Synth::max_lfos];             // by descriptor id - 1; pool_no_slot when unreferenced
    uint32_t lfo_count;                              // number of live LFO nodes
    uint32_t midi_node; // MIDI source node; sources resolve from the chain's own channel, master from channel index 0
    uint32_t midi_source_slots[fx_num_input_sources];  // output dot per channel-wide MIDI source
    uint32_t midi_cc_slots[Synth::num_assignable_ccs]; // output dot per added CC; pool_no_slot when absent
    uint32_t chain;                                    // channel index or fx_master_chain
};

// Captures validated serial audio topology; out is unchanged on refusal.
// proposed_connection is a non-mutating optional edge appended after live edges.
// excluded_connection omits a live wire being moved; pool_no_slot excludes none.
bool capture_effect_audio(const Graph&                     graph,
                          const EffectGraphMapping&        mapping,
                          const Synth::EffectChainBinding& chain,
                          EffectAudioTopology*             out,
                          const Connection*                proposed_connection = nullptr,
                          uint32_t                         excluded_connection = pool_no_slot);

// Applies a bounded snapshot without consuming events.  A null capture mask records
// all represented nodes; otherwise only selected or already persisted nodes are saved.
// Outputs are unchanged on failure; source and output may alias.
bool apply_effect_graph_batch(const Synth::InstrumentEditorBank* source,
                              const Graph*                       graph,
                              const EffectGraphMapping*          mapping,
                              const GraphChange*                 changes,
                              uint32_t                           count,
                              const bool*                        capture_nodes,
                              Synth::InstrumentEditorBank*       out);
// Requires graph and mapping; captures the complete pending projection without consumption.
bool stage_effect_graph_edits(const Synth::InstrumentEditorBank* source,
                              const Graph*                       graph,
                              const EffectGraphMapping*          mapping,
                              Synth::InstrumentEditorBank*       out);
// Ordinary bank presentation drops only unmatched generated effect titles.  Missing
// descriptor references and unrecognized titles refuse without modifying the bank.
bool normalize_effect_layout(Synth::InstrumentEditorBank* bank);
bool change_effect_type_candidate(const Synth::InstrumentEditorBank* source,
                                  uint32_t                           owner,
                                  uint32_t                           slot,
                                  Synth::EffectType                  type,
                                  Synth::InstrumentEditorBank*       out);
// Allocates an owner-scoped represented root.  Both outputs are success-only.
bool add_effect_lfo_candidate(const Synth::InstrumentEditorBank* source,
                              uint32_t                           owner,
                              Synth::InstrumentEditorBank*       out,
                              uint16_t*                          out_descriptor,
                              const bool*                        pinned_lfos = nullptr);
// Extends an initialized descriptor keep-set with bindings and owner-scoped roots.
bool collect_effect_lfos(const Synth::InstrumentEditorBank* source, uint32_t chain, bool* keep_lfos);
bool transfer_effect_layout(const Synth::InstrumentEditorBank* source,
                            uint32_t                           source_chain,
                            const uint16_t*                    lfo_ids,
                            uint32_t                           destination_chain,
                            Synth::InstrumentEditorBank*       candidate);
// Null graph/mapping extracts persisted state; pending extraction never consumes events.
bool extract_effect_chain_document(const Synth::InstrumentEditorBank* source,
                                   uint32_t                           chain,
                                   const Graph*                       graph,
                                   const EffectGraphMapping*          mapping,
                                   Synth::EffectsDocument*            out);
// Allocates fresh LFO ids and validates before assigning output.
bool replace_effect_chain_candidate(const Synth::InstrumentEditorBank* source,
                                    uint32_t                           chain,
                                    const Synth::EffectsDocument*      document,
                                    Synth::InstrumentEditorBank*       out_candidate,
                                    const bool*                        pinned_lfos = nullptr);

// Chain slot an effect node projects, or -1 when the node is not an effect.
int32_t fx_effect_slot_of(const EffectGraphMapping& mapping, uint32_t node_idx);

// Descriptor id an LFO node projects, or 0 when the node is not an LFO node.
uint32_t fx_lfo_desc_of(const EffectGraphMapping& mapping, uint32_t node_idx);
// Whether projected param `param` on an effect node carries no modulator:
// no connection targets its base-value row (an inline wire in transit) or
// any of its three modulator input dots.  Derived live from connection
// state, never stored.
bool fx_param_is_collapsed(const Graph& graph, uint32_t node_idx, uint32_t param);
// Per-family modulator-row visibility for projected param `param`: field 0
// (the base-value row) always renders; fields 1..3 (LFO dot/op/depth) render
// iff the param's LFO dot is connected; fields 4..6 / 7..9 (each Source
// dot/op/amount row) render iff that Source dot is connected.  Derived live
// from connection state at render time, never stored; slot arithmetic,
// connections, snapshots and undo bytes are untouched.  Public seam: the
// render filter fx_slot_hidden is static in the editor TU, so the unit
// suite needs this callable render-visibility query.
bool fx_param_field_hidden(const Graph& graph, uint32_t node_idx, uint32_t param, uint32_t field);
// The first free modulator input dot of projected param `param` on an
// effect node: the LFO dot for an LFO source wire, else the first free
// Source A/B dot in row order. pool_no_slot when the wire's family has no
// free input. Pure connection-state query; the inline value-row dot of a
// collapsed parameter resolves its wire through this choice.
uint32_t fx_param_free_input(const Graph& graph, uint32_t node_idx, uint32_t param, bool lfo_source);
// Budget for one additional represented node, including owner roots and legacy pins.
bool fx_can_add_node(const Synth::InstrumentEditorBank* source, uint32_t owner, const bool* pinned_lfos = nullptr);
// Number of nodes the chain's projection needs: fixed endpoints, one per
// projected effect slot, and one per pinned-or-referenced LFO descriptor.
uint32_t fx_projected_node_count(const Synth::InstrumentBank& bank, uint32_t chain, const bool* pinned_lfos);
// Rebuilds every retained effect node and authored audio fragment, plus unique
// bound or pinned LFO nodes and their modulation wires. Slot mapping is authoring
// identity, not playback order. False leaves a partial projection on exhaustion.
bool project_effect_chain_to_graph(const Synth::InstrumentEditorBank& bank,
                                   uint32_t                           chain,
                                   Graph*                             graph,
                                   EffectGraphMapping*                mapping,
                                   const bool*                        pinned_lfos = nullptr);

// Payload-only single-event seam for parameter/modulation edits and legacy raw
// chain helper checks. Editor audio structure is captured transactionally by
// apply_effect_graph_batch; this raw-bank seam cannot represent detached topology.
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
// MIDI source rows (input dot, op list, amount each).
constexpr uint32_t fx_param_stride = 10;

// The Enabled row's slot index on a projected effect node (layout in the
// fx_param_stride comment above).  The row doubles as the enable wire's
// input endpoint.
constexpr uint32_t fx_enabled_row = 2;

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
