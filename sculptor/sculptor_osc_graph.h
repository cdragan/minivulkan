// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Projection between a synth instrument and its oscillator editor graph.
// project_instrument_to_graph rebuilds the whole graph from an instrument;
// compile_graph_to_instrument rebuilds the instrument from graph + mapping.
// The projection accepts exactly the instruments it can express: a routing
// input with the none source is refused. For any accepted instrument the
// round trip is synthesis-equivalent, and bit-identical when the instrument
// is in canonical graph-expressible form: every field the graph does not
// express is dormant - never read by the runtime - and compiles to zero.
// Unexpressed fields include, for example, routing entries past the
// connected inputs, routing input and all generator fields on targets the
// graph does not project, and LFO depth/rate sources without a bound LFO.
// Descriptor pool entries are referenced by id, never copied, so aliased
// descriptor ids stay aliased and the pools are never deduplicated or
// duplicated.

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_graph.h"

namespace Sculptor {

// One input node per MIDI source role (every ModSource except none).
constexpr uint32_t num_osc_graph_inputs = static_cast<uint32_t>(Synth::ModSource::pressure_combine);

// Where every projected piece of the model landed in the graph.  Slot and
// node indices are stable across property edits; structural edits re-project
// and refill the whole mapping.
struct OscGraphMapping {
    uint32_t input_nodes[num_osc_graph_inputs]; // indexed by ModSource - 1
    uint32_t input_output_slot;
    uint32_t output_node; // the oscillator sum node
    uint32_t output_layer_input_slot[Synth::max_layers];
    uint32_t osc_nodes[Synth::max_layers];
    uint32_t osc_output_slot;
    // Connector slots exist only on the five targets the runtime modulates;
    // the rest stay pool_no_slot.
    uint32_t osc_direct_input_slot[Synth::num_mod_targets][Synth::max_mod_inputs];
    uint32_t osc_env_input_slot[Synth::num_mod_targets];
    uint32_t osc_lfo_input_slot[Synth::num_mod_targets];
    uint32_t env_nodes[Synth::max_layers][Synth::num_mod_targets];
    uint32_t env_output_slot;
    uint32_t lfo_nodes[Synth::max_layers][Synth::num_mod_targets];
    uint32_t lfo_output_slot;
    uint32_t lfo_depth_input_slot;
    uint32_t lfo_rate_input_slot;
    // 1-based descriptor ids, 0 = unbound; only projected targets bind.
    uint16_t env_desc_ids[Synth::max_layers][Synth::num_mod_targets];
    uint16_t lfo_desc_ids[Synth::max_layers][Synth::num_mod_targets];
};

// Rebuilds the graph from scratch: clears every node and connection, then
// recreates the full projection.  The bank is only read.  Returns false
// without touching the graph, mapping or bank when the instrument is not
// expressible (a projected target's routing input carries the none source);
// the refusal is also reported through the graph's error overlay.
// Deterministic: same instrument and bank produce the same node/slot/
// connection layout.
bool project_instrument_to_graph(const Synth::Instrument&     instrument,
                                 const Synth::InstrumentBank& bank,
                                 Graph*                       graph,
                                 OscGraphMapping*             mapping);

// Rebuilds the instrument from the graph.  Writes every field of *out
// (including zeroing what the graph does not express), so the result is
// directly comparable with the projected model.  Descriptor ids are read
// from the mapping, not the bank.  Returns false on any inconsistency the
// validator should have made impossible; *out is still fully written.
bool compile_graph_to_instrument(const Graph& graph, const OscGraphMapping& mapping, Synth::Instrument* out);

// Grammar validator, installable with Graph::set_validator.  The allowed
// edge set is exactly: envelope -> target envelope input, LFO -> target LFO
// input, MIDI input -> target direct input or LFO depth/rate source, and
// oscillator -> sum node layer input.  Duplicate source -> target pairs are
// first-class and order-significant, so there is no duplicate rule.
bool osc_graph_validate(void* user_data, const Graph& graph, EndPoint output, EndPoint input);

// Writes one descriptor-content property edit through to the bank pool entry
// named by the node's desc id.  Nodes sharing a desc id edit the same entry,
// so aliasing stays consistent and repeated edits are idempotent.  Returns
// false with the bank unmodified when the node is not an envelope or LFO
// binding node, the slot is not a descriptor-content property, or the value
// does not fit the descriptor field.
bool apply_osc_graph_descriptor_edit(const Graph&           graph,
                                     const OscGraphMapping& mapping,
                                     Synth::InstrumentBank* bank,
                                     uint32_t               node_idx,
                                     uint32_t               slot_idx,
                                     PropertyValue          value);

} // namespace Sculptor
