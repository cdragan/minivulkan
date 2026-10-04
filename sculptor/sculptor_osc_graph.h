// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Projection between a synth instrument and its oscillator editor graph.
// project_instrument_to_graph rebuilds the whole graph from an instrument;
// compile_graph_to_instrument rebuilds the instrument from graph + mapping.
// The projection accepts exactly the instruments it can express: a routing
// input with the none source is refused.  For any accepted instrument the
// round trip is synthesis-equivalent, and bit-identical when the instrument
// is in canonical graph-expressible form: every field the graph does not
// express is dormant - never read by the runtime - and compiles to zero.
// Unexpressed fields include, for example, routing entries past the
// connected inputs and all generator fields on targets the graph does not
// project.  Descriptor pool entries are referenced by id, never copied, so
// aliased descriptor ids stay aliased and the pools are never deduplicated
// or duplicated.

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_graph.h"
#include "sculptor_instr_bank.h"

namespace Sculptor {

// One input node per MIDI source role (every ModSource except none).
constexpr uint32_t num_osc_graph_inputs = static_cast<uint32_t>(Synth::ModSource::pressure_combine);
// The registry/record source bounds and the canonical node numbering pin the
// same bound: a drift between the ModSource set and the canonical numbering
// must fail the build.
static_assert(num_osc_graph_inputs == Synth::graph_canonical_input_count);

// Free-standing envelope/LFO node instance, registered per (zone, kind,
// descriptor, LFO sources).  Every envelope/LFO node is an instance:
// parameters bind instances through their envelope/LFO input wires, and an
// instance no parameter references stays free-standing, backed by a
// kind-1/2 layout record once it carries editor state.
struct DetachedNode {
    uint32_t node_idx;     // pool_no_slot while the node is not projected
    uint8_t  kind;         // 1 = envelope, 2 = LFO
    uint8_t  desc_id;      // 1-based descriptor id
    uint8_t  uid;          // per-(zone, kind) instance id; 0 while record-less
    uint8_t  depth_source; // LFO only: ModSource feeding the depth input
    uint8_t  rate_source;  // LFO only: ModSource feeding the rate input
};

constexpr uint32_t max_detached_nodes = Synth::max_detached_per_zone;

// One per-target modulation recipe.  Cells of one target whose full binding
// tuple (envelope desc id, LFO desc id, LFO op/depth/rate scale, LFO
// depth/rate sources) is identical merge into one parameter; the parameter
// fans out to exactly its served cells' oscillator inputs.
constexpr uint32_t max_param_nodes = Synth::max_layers * 5;

// Worst-case bound node population: 6 MIDI inputs + sum, one oscillator per
// layer, and per (layer, target) cell one parameter, envelope and LFO node.
static_assert(7 + Synth::max_layers + 3 * max_param_nodes <= max_nodes);
// Worst-case bound edge population: per layer one oscillator->sum edge, and
// per cell one parameter->oscillator, envelope->parameter and LFO->parameter
// edge plus two source->parameter and two source->LFO depth/rate edges.
static_assert(Synth::max_layers + 7 * max_param_nodes <= max_connections);

// One projected parameter node: identity is its target; duplicates of one
// target are disambiguated by enumeration order ("Volume", "Volume 2").
// `uid` is the kind-3 record uid, 0 while the node carries no record.
struct ParamEntry {
    uint32_t node_idx; // pool_no_slot while the node is not projected
    uint8_t  target;   // projected target index 0..4
    uint8_t  uid;      // kind-3 record uid; 0 while record-less
    uint32_t env_node; // bound envelope instance node, pool_no_slot when none
    uint32_t lfo_node; // bound LFO instance node, pool_no_slot when none
    uint8_t  served;   // bitset of layers whose oscillator input wires here
};

// Where every projected piece of the model landed in the graph.  Slot and
// node indices are stable across property edits; structural edits re-project
// and refill the whole mapping.
struct OscGraphMapping {
    uint32_t input_node;                               // one node for all MIDI source roles
    uint32_t input_source_slots[num_osc_graph_inputs]; // indexed by ModSource - 1
    uint32_t output_node;                              // the oscillator sum node
    uint32_t output_layer_input_slot[Synth::max_layers];
    uint32_t osc_nodes[Synth::max_layers];
    uint32_t osc_output_slot;
    // Uniform generator/parameter node layouts; populated even when no such
    // node exists so the mapping always describes the full layout.
    uint32_t env_output_slot;
    uint32_t lfo_output_slot;
    uint32_t lfo_depth_input_slot;
    uint32_t lfo_rate_input_slot;
    uint32_t param_output_slot;

    // The (channel, zone) the projection sourced; apply paths route mask bits
    // and record updates through it.
    uint32_t channel;
    uint32_t zone;

    // Parameter-node registry, in derivation order (targets in ModTarget
    // order, cells in layer order, groups in first-cell order; detached
    // record surplus appends after).
    ParamEntry params[max_param_nodes];
    uint32_t   param_count;

    // Generator-instance registry, rebuilt by the projection.  Derived
    // instances come first (one per distinct (kind, descriptor, sources)
    // key, in first-use order); record-driven surplus appends in record
    // order.  Entries are ordered by record order.
    DetachedNode detached[max_detached_nodes];
    uint32_t     detached_count;
    // The bank the projection reads, for bank-wide descriptor usage scans
    // (descriptor ids are shared across every instrument and zone).  Set by
    // the projection; never dereferenced before the first projection.
    const Synth::InstrumentBank* source_bank;
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
// directly comparable with the projected model.  Bindings resolve from the
// live graph edges, not from projection-time state.  Returns false on any
// inconsistency the validator should have made impossible; *out is still
// fully written.
bool compile_graph_to_instrument(const Graph& graph, const OscGraphMapping& mapping, Synth::Instrument* out);

// Grammar validator, installable with Graph::set_validator.  The allowed
// edge set is exactly: parameter -> same-target oscillator value row,
// envelope -> parameter envelope input, LFO -> parameter LFO input, MIDI
// input -> parameter source input or LFO depth/rate source, and oscillator
// -> sum node layer input.  Duplicate source -> target pairs are
// first-class and order-significant, so there is no duplicate rule.
bool osc_graph_validate(void* user_data, Graph& graph, EndPoint output, EndPoint input);

// Canonical kind-0 record index of a projected fixed node (inputs 0..5, the
// sum node 6, oscillator layers), or pool_no_slot when the node is not
// projected or carries no canonical index (parameters and generator
// instances are record-keyed, never canonical).
uint32_t osc_graph_canonical_index(const OscGraphMapping& mapping, uint32_t node_idx);

// Writes one descriptor-content property edit through to the bank pool entry
// named by the node's desc id.  Nodes sharing a desc id edit the same entry,
// so aliasing stays consistent and repeated edits are idempotent.  Returns
// false with the bank unmodified when the node is not an envelope or LFO
// instance, the slot is not a descriptor-content property, or the value
// does not fit the descriptor field.
bool apply_osc_graph_descriptor_edit(const Graph&           graph,
                                     const OscGraphMapping& mapping,
                                     Synth::InstrumentBank* bank,
                                     uint32_t               node_idx,
                                     uint32_t               slot_idx,
                                     PropertyValue          value);

// Full projection from the editor bank: rebuilds the graph from the zone's
// instrument plus the bank's editor-side state - kind-1/2 records attach to
// derived generator instances (or materialize surplus free-standing nodes),
// kind-3 records attach to derived parameters by (target, ordinal) or
// materialize detached extra parameters, masked oscillator->sum edges stay
// deleted, and the surviving missing-sum bits re-apply the per-slot missing
// flags.  The bank is only read.  Returns false when the zone has no
// instrument or the instrument is not expressible; mid-walk failures can
// leave the graph partially rebuilt, so the caller re-projects on false and
// must not keep the graph or mapping state.
bool project_editor_to_graph(const Synth::InstrumentEditorBank& bank,
                             Graph*                             graph,
                             OscGraphMapping*                   mapping,
                             uint32_t                           channel = 0,
                             uint32_t                           zone    = 0);

// Applies one drained Graph change to the editor bank through the mapping
// (per-frame apply): property edits via apply_osc_graph_descriptor_edit and
// the shared-routing fan-out, oscillator->sum connection_added/deleted via
// the zone's missing-sum bits, parameter/generator wiring via rebind and
// detach reconciliation, source-edge mirroring across same-target
// parameters, and parameter title renames into the kind-3 record name.
// Returns false when the change cannot be applied (e.g.
// record-list capacity); application up to that point may already have
// mutated the bank, so the caller refuses its whole batch and re-projects
// from the last committed bank.
bool apply_osc_graph_change(Synth::InstrumentEditorBank* bank,
                            Graph*                       graph,
                            OscGraphMapping*             mapping,
                            const GraphChange&           change,
                            uint32_t                     channel = 0,
                            uint32_t                     zone    = 0);

// Shared-routing fan-out: writes `value` into every live view of the field
// addressed by (node role, slot), without change events - the five dynamic
// and four constant oscillator value rows, the value row of every
// same-target parameter, and the source op/scale rows of every same-target
// parameter.  Returns false (writes nothing) for per-layer or per-binding
// slots.
bool sync_osc_graph_shared_slot(Graph*                 graph,
                                const OscGraphMapping& mapping,
                                uint32_t               node_idx,
                                uint32_t               slot_idx,
                                PropertyValue          value);

// Full editor-metadata validation (JSON decode commit and editor candidate
// commit) lives in sculptor_instr_bank.h next to its storage and constants.

// Pure editor-side helpers for the instrument graph editor state.
// No ImGui here: synth_unit links this translation unit.

// The five parameter target display names, shared by the node menus, the
// immediate rename feedback and the projection's derived titles.
extern const char* const param_target_names[5];

// Parameter-node source-row input slots (rows 7..9 and 10..12; see the
// slot layout table in sculptor_osc_graph.cpp).
constexpr uint32_t param_src_input(uint32_t i)
{
    return 7u + 3u * i;
}

// Presentation view of a modulation target's value: node rows and
// parameter base values show display units, the bank stores
// bank_scale-scaled units (radians for fm depth, Hz for cutoffs).
// max_value == min_value means unbounded.
struct OscTargetView {
    float min_value;
    float max_value;
    float bank_scale;
    bool  logarithmic;
};

// The shared presentation view of a modulation target; the single
// source of truth for value bounds, unit scaling and slider curve.
OscTargetView osc_target_view(Synth::ModTarget target);

// Slot index on an oscillator node holding the value row of projected
// parameter target `projected_index` (0..4); rows are not contiguous.
uint32_t osc_target_row(uint32_t projected_index); // Registry index of the parameter projected onto node_idx, or -1.
int32_t  find_param(const OscGraphMapping& mapping, uint32_t node_idx);

// The volume-envelope shape: minimum 0 and the first and the last point at 0,
// so a volume envelope starts and ends in silence.
bool env_volume_shape_ok(const Synth::EnvelopeDescriptor& env);

// Reports whether the envelope descriptor id is wired into a volume target's
// parameter, into a non-volume target, or both.  Descriptor ids are shared by
// every parameter that wires them and by every instrument in the bank, so the
// answer covers all users.  exclude_param_idx / exclude_connection omit one
// parameter (a retarget moves its own usage) or one connection (a wire move
// re-lands it) from the scan.
void env_target_usage(const Graph&           graph,
                      const OscGraphMapping& mapping,
                      uint16_t               desc_id,
                      bool*                  volume_used,
                      bool*                  other_used,
                      int32_t                exclude_param_idx  = -1,
                      uint32_t               exclude_connection = pool_no_slot);

// Index of the zone's layout record with the given key, or -1.
int32_t find_record(const Synth::InstrumentEditorBank& bank,
                    uint32_t                           channel,
                    uint32_t                           zone,
                    uint32_t                           kind,
                    uint32_t                           index,
                    uint8_t                            uid);

// True when an earlier-enumerated same-target parameter is still record-less
// (derived).  A record gained by the parameter at param_idx would attach to
// that sibling positionally at re-projection (records pair with derived
// parameters in enumeration order) and hijack its node, so the gaining
// gesture must be refused.
bool param_has_recordless_predecessor(const OscGraphMapping& mapping, uint32_t param_idx);
// True when the target has a record-less (derived) parameter: a record re-keyed
// into that target would attach to that sibling positionally at re-projection.
bool param_target_has_recordless_derived(const OscGraphMapping& mapping, uint32_t target);
// Add Parameter preflight: true when committing the new free-standing
// parameter would leave the projection unable to load the zone (the
// parameter registry is a designed static budget) or would let the new
// bare record hijack a record-less derived sibling of the target at
// re-projection (the same shape the rename path refuses). error receives
// the user-facing refusal message.
bool osc_add_parameter_refused(const OscGraphMapping& mapping, uint32_t target_index, char* error, uint32_t error_size);
// One-based ordinal of the parameter among its target's derived
// parameters in enumeration order; stored in kind-3 records (param_slot)
// so re-projection pairs them by group, not by uid order.
uint8_t param_group_ordinal(const OscGraphMapping& mapping, uint32_t param_idx);
// Eventless wire-driven retarget, shared by the Change Target menu and the
// apply path's cross-target wire drop: the kind-3 record re-keys to the new
// target, the mapping entry flips, the parameter's value wires shift onto
// the new target's oscillator rows, and the source rows adopt the
// destination's routing or restore the captured wiring.
// Returns false only when a defensive record creation hits capacity.
bool retarget_param(Synth::InstrumentEditorBank* bank,
                    Graph&                       graph,
                    OscGraphMapping&             mapping,
                    uint32_t                     channel,
                    uint32_t                     zone,
                    uint32_t                     param_idx,
                    uint32_t                     new_target);
// Recomputes the binding reconciliation and the compiled instrument after
// an eventless graph rewrite (see retarget_param); the drain does this
// implicitly at the end of every applied batch.
bool refresh_osc_graph_compilation(Synth::InstrumentEditorBank* bank,
                                   Graph&                       graph,
                                   OscGraphMapping&             mapping,
                                   uint32_t                     channel,
                                   uint32_t                     zone);

// Copies a parameter's live wiring into a kind-3 record's persistence
// fields: served bits from the parameter's value wires, and the
// envelope/LFO descriptor ids plus the LFO source pair from the bound
// instance nodes (0s when an input is unwired).  The LFO sources resolve
// from the live depth/rate edges into the bound instance, so a mid-session
// rewire is what the record stores; the record triple then still matches
// the instance at re-projection. The record's name persists as the user
// set it and never follows the wires. Returns false when a source edge
// cannot be resolved.
bool store_param_wiring(const Graph&            graph,
                        const OscGraphMapping&  mapping,
                        const ParamEntry&       param,
                        Synth::GraphNodeLayout* record);

// One sparse per-zone node layout record lives in sculptor_instr_bank.h
// next to its storage inside InstrumentEditorBank.

// Node population the projection derives for one zone: fixed nodes,
// oscillators, parameters, generator instances and record-driven surplus.
// Preflights (record capacity, node pool) count against it.
// parameter_count optionally receives the implied parameter count
// (derived groups plus materialized surplus parameters).
uint32_t count_projected_nodes(const Synth::InstrumentEditorBank& bank,
                               uint32_t                           channel,
                               uint32_t                           zone,
                               uint32_t*                          parameter_count = nullptr);

// A group tag identifies one editable FIELD ({kind, id0, id1}). The editor
// pushes an undo snapshot only when the tag differs from the last commit's
// tag; same-tag edits amend the live state.  The tag is remembered when
// undo_group_needs_snapshot returns true, before the commit's outcome is
// known: on a refused commit the caller must undo_group_reset so the
// refused edit does not coalesce into the next one.
struct UndoGroupTag {
    uint32_t kind;
    uint32_t id0;
    uint32_t id1;
};

struct UndoGroupState {
    UndoGroupTag last;
    bool         has_last;
};

// Returns true when the caller must push an undo snapshot (the tag differs
// from the remembered one) and remembers the tag; false when the edit amends
// the state tagged by the previous commit.  See UndoGroupTag for the reset
// obligation on refused commits.
bool undo_group_needs_snapshot(UndoGroupState* state, UndoGroupTag tag);

// Clears the remembered tag (undo, redo, channel/zone selection change,
// re-projection, load), forcing the next commit to snapshot.
void undo_group_reset(UndoGroupState* state);

// True when `additional` net new records fit in the global record list.
bool graph_records_have_capacity(const Synth::InstrumentEditorBank& bank, uint32_t additional);
// Lowest per-(zone, kind) instance id not in use by a live record; the
// caller enforces the per-zone detached cap, so a free id always exists.
uint8_t allocate_detached_uid(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone, uint32_t kind);

// A generator instance gains its kind-1/2 layout record: the live node
// layout and source wires are captured, so a re-projection restores the
// instance with its move and edits.  Returns false when the record list is
// full or a source wire cannot be resolved.
bool detach_osc_graph_instance(Synth::InstrumentEditorBank* bank,
                               const Graph&                 graph,
                               OscGraphMapping*             mapping,
                               uint32_t                     channel,
                               uint32_t                     zone,
                               uint32_t                     registry_idx);

// Shifts the zone's missing-sum bits together with an oscillator layer
// compaction: bits above `removed_layer` move down by one.
void compact_missing_sum_bits(Synth::InstrumentEditorBank* bank,
                              uint32_t                     channel,
                              uint32_t                     zone,
                              uint32_t                     removed_layer);
// Publish-time channel_enabled mask: a channel publishes enabled only when no
// zone of the channel has a missing-sum bit.  The bank's stored
// channel_enabled values are left untouched (they stay 1).
void compute_publish_channel_enabled(const Synth::InstrumentEditorBank& bank, uint8_t out_enabled[Synth::max_channels]);

// Zone-table metadata mutation rules.  Records and mask rows move together.
// zone_split_new non-first-note case: shifts zones > `zone` by +1, then copies
// zone `zone`'s kind-0 records and mask row into zone `zone` + 1. Kind-1/2/3
// records are NOT copied (generators and parameters are zone-local UI state;
// the split zone's wiring re-derives deterministically).  Returns false
// without mutating anything when the copied records would overflow the
// global record list.
bool zone_records_split_copy(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Drops zone `zone`'s records and mask row and shifts later zones down by one
// (zone_delete, and joins that remove an entry).
void zone_records_drop_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Drops ALL records and mask rows of every zone of the channel (library load,
// channel init, channel delete).
void channel_records_reset(Synth::InstrumentEditorBank* bank, uint32_t channel);

} // namespace Sculptor
