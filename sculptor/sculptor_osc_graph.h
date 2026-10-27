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
// connected inputs.  Descriptor pool entries are referenced by id, never copied, so
// aliased descriptor ids stay aliased and the pools are never deduplicated
// or duplicated.

#pragma once

#include "../synth/synth_instrument.h"
#include "../synth/synth_midi_cc.h"
#include "sculptor_bank_json.h"
#include "sculptor_graph.h"
#include "sculptor_instr_bank.h"

namespace Sculptor {

// Only waveform/mode availability; connections, edit locks and ghosts are caller-owned.
bool oscillator_slot_waveform_mode_disabled(const Node& node, uint32_t slot_index);

// The six instrument-supported MIDI source roles (pitch bend, mod wheel, channel
// pressure, velocity, aftertouch and combined pressure; not the whole ModSource
// set) share one input node.
constexpr uint32_t num_osc_graph_inputs = static_cast<uint32_t>(Synth::ModSource::pressure_combine);
// The registry/record source bounds and the canonical node numbering pin the
// same bound: a drift between the supported-source set and the canonical
// numbering must fail the build.
static_assert(num_osc_graph_inputs == Synth::graph_canonical_input_count);

// Free-standing envelope/LFO node instance, registered per (zone, kind,
// descriptor, LFO sources).  Every envelope/LFO node is an instance: a
// wire out of the instance binds it to its target cell, and an unwired
// instance stays free-standing, backed by a kind-1/2 layout record once
// it carries editor state.
struct DetachedNode {
    uint32_t node_idx;     // pool_no_slot while the node is not projected
    uint8_t  kind;         // 1 = envelope, 2 = LFO
    uint8_t  desc_id;      // 1-based descriptor id
    uint8_t  uid;          // per-(zone, kind) instance id; 0 while record-less
    uint8_t  depth_source; // LFO only: ModSource feeding the depth input
    uint8_t  rate_source;  // LFO only: ModSource feeding the rate input
};

constexpr uint32_t max_detached_nodes = Synth::max_detached_per_zone;

// The instance registry spans every detached generator node the graph
// can hold: the node pool minus the always-present fixed nodes (the
// MIDI input node and the sum node). It is sized independently of the
// detached-record cap and of the layer count, so a low-layer projection
// with record-driven surplus instances still fits; the node pool itself
// enforces the layer-dependent budget.
constexpr uint32_t max_osc_instances = max_nodes - (Synth::graph_canonical_input_node_count + 1);
static_assert(2 * Synth::max_layers * Synth::num_mod_targets <= max_osc_instances);

// The twelve per-target modulation blocks live on the oscillator node
// itself: each ModTarget's 12-slot block sits directly after its value
// row slot, so the block's modulator rows render under the row they
// attach to (the fx pattern).  See the block helpers below for the
// field map.  Row visibility is derived live from connection state;
// slots, connections, snapshots and undo bytes never encode it.

// Worst-case bound node population: one input node for six MIDI source
// roles, the instrument-parameters node and sum, one oscillator per
// layer, and per (layer, target) cell one envelope and one LFO instance.
static_assert(Synth::graph_canonical_input_node_count + 2 + Synth::max_layers +
                  2 * Synth::max_layers * Synth::num_mod_targets <=
              max_nodes);
// Worst-case bound edge population: per layer one oscillator->sum edge,
// per cell one envelope, one LFO and two source edges, and per distinct
// LFO instance key up to two source edges into its depth/rate inputs.
static_assert(Synth::max_layers + 6 * Synth::max_layers * Synth::num_mod_targets <= max_connections);

// Oscillator node slot grammar: 37 fixed slots (oscillator properties,
// the FIR slope and filter type properties, and target value rows, order
// pinned by osc_target_row_slot below),
// and every ModTarget owns one 12-slot modulation block directly after
// its value row.  After the target blocks, each existing band projects
// one uniform block: a "Band N" header row whose bare [-]/[+] push-button
// click slots insert a copy after / remove that band (the block constants
// below pin the layout).  Block field 0 is the envelope input dot, field 1 the
// unlabeled envelope depth sharing the dot's renderer line, fields 2..5 the LFO row (dot, op, depth, rate scale),
// fields 6..8 the Source A row and fields 9..11 the Source B row (dot, op, amount each).  Row-group ids are per
// (target, row): the LFO row groups 3*t + 1, the source rows 3*t + 2 and 3*t + 3, the envelope row 3*t + 4; the fixed
// slots own their lines (group 0). Four value rows nest as children of a
// parent row: duty and pad tone under their waveform rows, plus
// Pluck Brightness and Release Damping under waveform a, waveform mix
// / fm depth / fm ratio under mix mode, octave offset under pitch, and the band header rows and the band parameter rows
// under filter type. A nested row's whole modulation block shifts one tree level deeper.
constexpr uint32_t osc_slot_count          = 35;
constexpr uint32_t osc_target_block_stride = 12;
// One uniform block per existing band, directly after Filter Type (slot
// 149; osc_slot_count is the legacy fixed-row additive constant, not the
// fixed slot count): the "Band N" header label row, its [-] and [+] click
// slots, then the band's four parameter rows each followed by a 12-slot
// modulation block. Only the stored band count's blocks are projected, so
// the projected slot count encodes the band count.
constexpr uint32_t osc_band_first_slot = 150;
// Under the string wave the two shared rows are reinterpreted: the duty a row (slot 2 and its
// block) carries String Dispersion and the pad tone a row (slot 15 and its block) carries
// Sustain Damping.  Release Damping owns slot 28 with its block (29..40) and Pluck Brightness, a
// plain layer scalar in [0, 1] (1 = unfiltered burst) without a block, owns slot 41, directly
// before Waveform B.  Both are nested under Waveform A.  The string-only slots 28..41 are
// allocated for every wave and hidden unless wave A is the string wave.
constexpr uint32_t osc_string_extras_first_slot = 28;
constexpr uint32_t osc_brightness_slot          = 41;
constexpr uint32_t osc_band_max_bands           = 4;
constexpr uint32_t osc_band_block_stride        = 3u + 4u * (osc_target_block_stride + 1u);

// First slot of band `band`'s block (0-based): the header label; the
// [-] and [+] click slots follow at +1 and +2.
constexpr uint32_t osc_band_header_slot(uint32_t band)
{
    return osc_band_first_slot + osc_band_block_stride * band;
}

// The band header rows' renderer line ids: a header's label and its two
// click slots share one widget column (the renderer keys on this range).
constexpr uint8_t osc_band_row_group = 105;

// Band rows nest one level under the band header rows, which themselves
// nest one level under the Filter Type row: header at 1, value rows at
// 2, blocks at 3, rate/amount rows at 4.  The Multi-band FIR effect node
// reuses the depths for its own band tree (header at 1 under Enabled).
constexpr uint8_t osc_band_block_level = 3;

// The value row of each ModTarget on an oscillator node.  The rows are
// not contiguous: the duty rows interleave with the oscillator
// properties, and the rows after an earlier target's block shift down
// by that block.  Indexed by ModTarget.
constexpr uint32_t osc_target_row_slot[Synth::num_mod_targets] = {
    109, // volume
    135, // pitch
    122, // pan
    2,   // duty a
    43,  // duty b
    70,  // waveform mix
    83,  // fm depth
    166, // fir pitch spread (band 0 Spread row)
    96,  // fm ratio
    15,  // pad tone a
    56,  // pad tone b
    192, // fir slope
    153, // fir band 0 offset
    179, // fir band 0 gain
    208, // fir band 1 offset
    221, // fir band 1 spread
    234, // fir band 1 gain
    247, // fir band 1 slope
    263, // fir band 2 offset
    276, // fir band 2 spread
    289, // fir band 2 gain
    302, // fir band 2 slope
    318, // fir band 3 offset
    331, // fir band 3 spread
    344, // fir band 3 gain
    357, // fir band 3 slope
    15,  // damping (shares the pad tone a row)
    28,  // release damping
    2,   // dispersion (shares the duty a row)
};

// First slot of target `target`'s modulation block on an oscillator
// node: directly after its value row.
constexpr uint32_t osc_target_block_base(uint32_t target)
{
    return osc_target_row_slot[target] + 1u;
}

// The envelope input dot of target `target`'s block: an envelope wire's
// input endpoint.
constexpr uint32_t osc_target_env_dot(uint32_t target)
{
    return osc_target_block_base(target);
}

// The envelope depth slot of target `target`'s block: the per-binding
// multiplier on the bound envelope's contribution into the target.
constexpr uint32_t osc_target_env_depth(uint32_t target)
{
    return osc_target_block_base(target) + 1u;
}

// The LFO input dot of target `target`'s block: an LFO wire's input
// endpoint.
constexpr uint32_t osc_target_lfo_dot(uint32_t target)
{
    return osc_target_block_base(target) + 2u;
}

// The MIDI source input dot of target `target`'s block, source row
// `input` (0 = Source A, 1 = Source B).
constexpr uint32_t osc_target_src_dot(uint32_t target, uint32_t input)
{
    return osc_target_block_base(target) + 6u + 3u * input;
}

// Last block end fits the slot pool; the twelve row slots are spaced far
// enough apart that no block reaches the next row (smallest base-row
// gap 13, between duty a row 2 and pad tone a row 15; every block
// ends one slot before the next row).  The pinned targets are the
// appended band 3 rows, the current highest rows; extend the pins when a
// later append raises the high-water mark.
static_assert(osc_target_row_slot[Synth::mod_fir_band3_slope] + osc_target_block_stride <= max_node_slots);
static_assert(osc_target_row_slot[Synth::mod_fir_band3_offset] + osc_target_block_stride <= max_node_slots);
static_assert(osc_band_header_slot(osc_band_max_bands - 1u) + osc_band_block_stride <= max_node_slots);
static_assert(osc_target_row_slot[Synth::mod_string_dispersion] == osc_target_row_slot[Synth::mod_duty0]);
static_assert(osc_target_row_slot[Synth::mod_string_damping] == osc_target_row_slot[Synth::mod_pad_tilt0]);
static_assert(osc_target_row_slot[Synth::mod_string_release_damping] == osc_string_extras_first_slot);
static_assert(osc_string_extras_first_slot == osc_target_row_slot[Synth::mod_pad_tilt0] + osc_target_block_stride + 1u);
static_assert(osc_brightness_slot ==
              osc_target_row_slot[Synth::mod_string_release_damping] + osc_target_block_stride + 1u);
static_assert(osc_brightness_slot + 1u < osc_target_row_slot[Synth::mod_duty1]);
static_assert(osc_band_header_slot(osc_band_max_bands - 1u) + osc_band_block_stride == max_node_slots);
static_assert(osc_target_row_slot[Synth::mod_pad_tilt1] + osc_target_block_stride <
              osc_target_row_slot[Synth::mod_osc_mix]);
static_assert(osc_target_row_slot[Synth::mod_fm_ratio] + osc_target_block_stride <
              osc_target_row_slot[Synth::mod_volume]);

// The band region's legacy rows: for the single-edge filter types these
// two slots are the cutoff and FIR Slope rows and keep showing, with
// their modulation blocks, exactly as before the band UI.
constexpr uint32_t osc_filter_cutoff_slot = osc_target_row_slot[Synth::mod_fir_spread];
constexpr uint32_t osc_filter_slope_slot  = osc_target_row_slot[Synth::mod_fir_slope];

// Where every projected piece of the model landed in the graph.  Slot and
// node indices are stable across property edits; structural edits re-project
// and refill the whole mapping.
struct OscGraphMapping {
    uint32_t input_node;                                // one node for all MIDI source roles
    uint32_t input_source_slots[num_osc_graph_inputs];  // indexed by ModSource - 1
    uint32_t input_cc_slots[Synth::num_assignable_ccs]; // per assignable CC dot, pool_no_slot when absent
    uint32_t instrument_node;                           // "Instrument Parameters" node, pool_no_slot when unprojected
    uint32_t instrument_slots[4]; // [0] note skew, [1] layer skew, [2] release, [3] voices; node's property slots
    uint32_t output_node;         // the oscillator sum node
    uint32_t output_layer_input_slot[Synth::max_layers];
    uint32_t osc_nodes[Synth::max_layers];
    uint32_t osc_output_slot;
    // Uniform generator node layout; populated even when no such node
    // exists so the mapping always describes the full layout.
    uint32_t env_output_slot;
    uint32_t lfo_output_slot;
    uint32_t lfo_depth_input_slot;
    uint32_t lfo_rate_input_slot;

    // The (channel, zone) the projection sourced; apply paths route mask bits
    // and record updates through it.
    uint32_t channel;
    uint32_t zone;

    // Generator-instance registry, rebuilt by the projection. Derived
    // instances come first (one per distinct (kind, descriptor, sources)
    // key, in first-use order); record-driven surplus appends after them
    // in record order.
    DetachedNode detached[max_osc_instances];
    uint32_t     detached_count;
    // The bank the projection reads, for bank-wide descriptor usage scans
    // (descriptor ids are shared across every instrument and zone).  Set by
    // the projection; never dereferenced before the first projection.
    const Synth::InstrumentBank* source_bank;
};

// Rebuilds the graph from scratch: clears every node and connection, then
// recreates the full projection.  The bank is only read.  Returns false
// without touching the graph, mapping or bank when the instrument is not
// expressible (any layer's routing input carries the none source, or the
// graph node pool cannot hold the derived generator instances); the
// refusal is also reported through the graph's error overlay.
// The MIDI Inputs node's slots order as: six standard output dots, then the
// CC dots (when the optional editor mask is passed).  The instrument's
// detune and release rows live on a dedicated "Instrument Parameters" node
// created right after the MIDI Inputs node.
// Deterministic: same instrument and bank produce the same node/slot/
// connection layout.
bool project_instrument_to_graph(const Synth::Instrument&     instrument,
                                 const Synth::InstrumentBank& bank,
                                 Graph*                       graph,
                                 OscGraphMapping*             mapping,
                                 // Optional editor-only input: when non-null,
                                 // one CC output dot per set bit is created
                                 // between the standard dots and the detune
                                 // rows.  Direct callers pass nothing.
                                 const uint16_t* added_cc_mask = nullptr,
                                 // Optional editor-only input: the zone's source row
                                 // (graph_osc_source[channel][zone]): slot s fed by oscillator
                                 // ordinal (entry - 1), 0 = unwired.  Null means identity wiring:
                                 // slot s is fed by oscillator s for s < layer_count.  Direct
                                 // callers pass nothing.
                                 const uint8_t* osc_source = nullptr);

// Exports one zone's saved editor positions as portable records relative to
// the zone's own instrument: every registered record of (channel,zone) that
// resolves to a projected node of `instr` is converted, effect (kind 4)
// records are never clipboard data.  Unrepresented selected records are
// refused.  The clipboard domain encodes the authoring form directly; the
// portable domains translate kind-0 records onto the playback ranks of the
// zone's source row and drop records of unwired ordinals.  Records are
// all-or-nothing and *out_count is the actual count only on success;
// capacity must hold every record or the call fails.
bool encode_instrument_graph_layout(const Synth::Instrument&           instr,
                                    const Synth::InstrumentBank&       bank,
                                    const Synth::InstrumentEditorBank& editor_bank,
                                    uint32_t                           channel,
                                    uint32_t                           zone,
                                    Synth::InstrumentGraphLayout*      out_records,
                                    uint32_t                           capacity,
                                    uint32_t*                          out_count,
                                    Synth::InstrumentJsonDomain        domain);
// Resolves normalized decoded-relative portable records into editor layout
// records for the decoded instrument, assigning the instance uids the projection
// pairs records by. Unknown kinds, unresolved canonical (kind 0) locators,
// duplicate locators, non-finite or negative-sized positions and a capacity
// below the input count are refused with every output untouched.  One narrow
// relaxation: a structurally valid envelope/LFO record (kind 1 or 2) that
// resolves to no node - the removed oscillator cutoff targets - is skipped,
// logged and compacted away, so *out_count is the SURVIVING record count
// (no caller may assume it equals the input count).
bool map_instrument_graph_layout(const Synth::Instrument&            decoded_instr,
                                 const Synth::EnvelopeDescriptor*    decoded_envs,
                                 uint32_t                            decoded_env_count,
                                 const Synth::LFODescriptor*         decoded_lfos,
                                 uint32_t                            decoded_lfo_count,
                                 const Synth::InstrumentGraphLayout* decoded_layout,
                                 uint32_t                            decoded_layout_count,
                                 Synth::GraphNodeLayout*             out_records,
                                 uint32_t                            capacity,
                                 uint32_t*                           out_count);

bool append_instrument_graph_layout(const Synth::Instrument&            instrument,
                                    const Synth::EnvelopeDescriptor*    envelopes,
                                    uint32_t                            envelope_count,
                                    const Synth::LFODescriptor*         lfos,
                                    uint32_t                            lfo_count,
                                    const Synth::InstrumentGraphLayout* layout,
                                    uint32_t                            layout_count,
                                    const uint16_t*                     envelope_ids,
                                    const uint16_t*                     lfo_ids,
                                    uint32_t                            channel,
                                    uint32_t                            zone,
                                    Synth::InstrumentEditorBank*        candidate);

// Source-to-decoded normalization: re-anchors portable layout records exported
// from the source zone onto the identities of the transport-decoded document.
// Node identity follows the unchanged (layer,target) serving cells
// through decoder quantization and descriptor interning; raw descriptor bytes and
// numeric pool ids are never matched across the encode/decode boundary.
//
// Source locators resolve only against source_instr/source_envs/source_lfos, and
// emitted locators only against the decoded model.  Input pointers may be null
// only when their paired count is zero.  Every saved source node must correspond
// to exactly one decoded node; ambiguous collapse or split is refused instead
// of discarding metadata.  Unresolved source locators are refused, except a structurally valid
// kind-1/2 source locator that resolves to no node (a removed cutoff target):
// it is skipped, logged and excluded from the normalization.
// Duplicate decoded-node records from external JSON are refused by the mapper
// and decoder, not by this source-to-decoded normalization.
//
// All-or-nothing: *out_count is the actual count only on success.  Output records
// are decoded-relative and ready for the layout-aware encoder overload and for
// map_instrument_graph_layout.  Editor-internal: clipboard and library transport
// paths and unit tests call it; the synth player runtime never does.
bool normalize_instrument_graph_layout(const Synth::Instrument&            source_instr,
                                       const Synth::EnvelopeDescriptor*    source_envs,
                                       uint32_t                            source_env_count,
                                       const Synth::LFODescriptor*         source_lfos,
                                       uint32_t                            source_lfo_count,
                                       const Synth::InstrumentGraphLayout* source_layout,
                                       uint32_t                            source_layout_count,
                                       const Synth::Instrument&            decoded_instr,
                                       const Synth::EnvelopeDescriptor*    decoded_envs,
                                       uint32_t                            decoded_env_count,
                                       const Synth::LFODescriptor*         decoded_lfos,
                                       uint32_t                            decoded_lfo_count,
                                       Synth::InstrumentGraphLayout*       out_layout,
                                       uint32_t                            capacity,
                                       uint32_t*                           out_count);
// Compile the projected graph into the playback instrument for one zone:
// the projected model is the compile's input, so the output is directly
// comparable with it.  Bindings resolve from the live graph edges, not
// from projection-time state.  Returns false on any inconsistency the
// validator should have made impossible; *out is still fully written.
// osc_source_out, when given, receives the derived per-zone source row.
// previous, when given, donates the bindings of bands beyond the
// projected count: those bands project no rows, so the graph cannot
// express their state (the band edits zero them on removal).
bool compile_graph_to_instrument(const Graph&             graph,
                                 const OscGraphMapping&   mapping,
                                 Synth::Instrument*       out,
                                 uint8_t*                 osc_source_out = nullptr,
                                 const Synth::Instrument* previous       = nullptr);

// Grammar validator, installable with Graph::set_validator.  The allowed
// edge set is exactly: envelope -> a target block's envelope dot (base
// value row drop or direct dot), LFO -> a target block's LFO dot (same
// two landing forms), MIDI input (standard role or added CC dot) -> a
// target block's free Source dot (base row drop takes the first free
// one), and oscillator -> sum node layer input.  A family dot admits one
// wire only, so fresh connections onto an occupied dot refuse; moving an
// existing wire's either end stays valid.  Duplicate source -> target
// pairs are first-class and order-significant, so there is no duplicate
// rule.
bool osc_graph_validate(void* user_data, Graph& graph, EndPoint output, EndPoint input);

// Canonical kind-0 record index of a projected fixed node (input 0, sum 1,
// oscillator layers 2+n), or pool_no_slot when the node is not
// projected or carries no canonical index (generator instances are
// record-keyed, never canonical).
uint32_t osc_graph_canonical_index(const OscGraphMapping& mapping, uint32_t node_idx);

// The zone's projected row-run policy, shared by the projection, the
// node-count walk, the metadata validator and the editor consumers, all
// derived from the zone's source row alone: the run spans slots 0..k
// where k is the highest wired slot, a slot with entry 0 inside the run
// renders red (an unwired hole), and the gray add slot follows the run.
// The run's length: the number of slots up to and including the highest
// wired slot, 0 when nothing is wired.
uint32_t osc_row_run_length(const uint8_t source_row[Synth::max_layers]);
// Whether the slot renders red inside the run: an unwired hole.
bool osc_row_is_red(const uint8_t source_row[Synth::max_layers], uint32_t slot);
// The gray add dot's slot on the Oscillator Output node: the slot after
// the run (the run's length).  The visibility predicate, the projection's
// drop flags and the gray-dot color seam share this one slot.
uint32_t osc_row_gray_slot(const uint8_t source_row[Synth::max_layers]);
uint32_t osc_lowest_free_slot(const uint8_t source_row[Synth::max_layers]);

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
// and the source row decides which oscillator nodes wire into the output.
// The bank is only read.
// Returns false when the zone has no instrument or the instrument is not
// expressible; mid-walk failures can leave the graph partially rebuilt, so
// the caller re-projects on false and must not keep the graph or mapping
// state.
bool project_editor_to_graph(const Synth::InstrumentEditorBank& bank,
                             Graph*                             graph,
                             OscGraphMapping*                   mapping,
                             uint32_t                           channel = 0,
                             uint32_t                           zone    = 0);

// Channel-wide wired-CC scan over the shared per-channel registry: the
// channel's fx chain OR any of its zones' osc routing inputs carries the
// assignable CC.  Removal refusal (both panes) consults it; the master
// chain keeps the fx-chain-only guard because osc has no master chain.
bool channel_uses_cc(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t cc_number);

// Applies one drained Graph change to the editor bank through the mapping
// (per-frame apply): property edits via apply_osc_graph_descriptor_edit,
// oscillator->sum connection_added/deleted with the auto-move wire
// cleanup, base-target-row drops via the eventless family-dot relocation,
// and generator wiring via the detach reconciliation.
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

// Full editor-metadata validation (JSON decode commit and editor candidate
// commit) lives in sculptor_instr_bank.h next to its storage and constants.

// Pure editor-side helpers for the instrument graph editor state.
// No ImGui here: synth_unit links this translation unit.

// Presentation view of a modulation target's value: node rows and
// parameter base values show display units, the bank stores
// bank_scale-scaled units (radians for fm depth).
// max_value == min_value means unbounded.
struct OscTargetView {
    float min_value;
    float max_value;
    float bank_scale;
    bool  logarithmic;
};

// Bank-value bounds shared by the oscillator graph editor's bounded slots and
// the clipboard instrument codec, so the two can never disagree.  The
// values live in the synth header so the runtime clamp shares them.
constexpr float osc_fm_ratio_min     = Synth::osc_mod_ratio_min;
constexpr float osc_fm_ratio_max     = Synth::osc_mod_ratio_max;
constexpr float osc_pitch_offset_min = -12.0f;
constexpr float osc_pitch_offset_max = 12.0f;

// Octave offset bounds, shared by the bounded integer slider, the
// compile-path validation and the JSON validators.
constexpr int osc_octave_offset_min = -12;
constexpr int osc_octave_offset_max = 12;

// The shared presentation view of a modulation target; the single
// source of truth for value bounds, unit scaling and slider curve.
// The FIR spread target changes with the layer's filter type: semitones for band-pass (and off),
// a fixed cutoff in Hz for low-pass and high-pass.  Other targets ignore the type.
OscTargetView osc_target_view(Synth::ModTarget target, Synth::FilterType fir_type = Synth::fir_bandpass);

// The band index (0..3) of a band-parameter target, or -1 when the target is not a band
// parameter.  Band 0 keeps the legacy spread/slope targets, so its four parameters are 12, 7, 13, 11.
// The string damping targets (26, 27) are not FIR bands.
inline int32_t osc_target_band_index(const uint32_t target)
{
    if (target >= 14u && target <= 25u) {
        return static_cast<int32_t>((target - 14u) / 4u) + 1;
    }
    if (target == 7u || target == 11u || target == 12u || target == 13u) {
        return 0;
    }
    return -1;
}

// True when a value change needs a wholesale reprojection.  The filter type changes the spread
// row's label and range.
bool osc_change_restructures(const OscGraphMapping& mapping, const GraphChange& change);
// True when the slot is one of the band header rows' [-]/[+] click slots
// (header slot + 1 and + 2 in every band block).  Such an edit rewrites
// the stored bands and leaves the graph stale until the structural
// re-projection rebuilds it.
bool is_band_button_slot(uint32_t slot_idx);
// Slot index on an oscillator node holding row of ModTarget
// `target` (0..10); rows are not contiguous.
uint32_t osc_target_row(uint32_t target);

// Decodes an oscillator-node block slot into its (target, field):
// false when the slot is one of the fixed slots (a property or a target
// value row) or past the last block.
bool osc_target_block_field(uint32_t slot_idx, uint32_t* target, uint32_t* field);
// Decodes an oscillator-node input endpoint (target, row family and dot) as
// the graph validator and apply paths see it; the shared duty a / pad tone a
// rows and blocks decode to the target the node's wave a projects there
// (String Dispersion / Sustain Damping under the string wave). A node the
// graph no longer holds (a deletion event's freed endpoint) decodes with the
// default Duty A / Pad Tone A meaning. False for non-oscillator or non-target
// endpoints.
bool decode_osc_input(const OscGraphMapping& mapping,
                      const Graph&           graph,
                      EndPoint               point,
                      bool*                  base_row,
                      uint32_t*              target,
                      uint32_t*              family,
                      uint32_t*              dot);
// False for a ModTarget whose row is shared and currently carries another
// target on this oscillator node (duty a / pad tone a give way to String
// Dispersion / Sustain Damping under the string wave; Release Damping exists
// only under the string wave).
bool osc_node_target_row_active(const Node& node, uint32_t target);

// Per-family modulator-row visibility for target `target`'s block on an
// oscillator node: field 0 (the envelope dot) renders iff the envelope
// dot is connected; fields 1..4 render iff the LFO dot is connected;
// fields 5..7 / 8..10 render iff that Source dot is connected.  Derived
// live from connection state at render time, never stored.
// Public seam: the render filter osc_slot_hidden is static in the editor
// TU, so the unit suite needs this callable render-visibility query.
bool osc_target_field_hidden(const Graph& graph, uint32_t node_idx, uint32_t target, uint32_t field);

// Band-derived render visibility: true when the slot is a Band row, a
// band-parameter row/block slot, or a read-only band display row that
// the filter type, the band selection and the wiring hide right now,
// false for every other slot.  Derived live from the projected Filter
// Type and Band slots and the connections, never stored.
bool osc_band_slot_hidden(const Graph& graph, uint32_t node_idx, uint32_t slot_idx);

// The composed oscillator-node render visibility: the band-derived hiding
// with the legacy cutoff and FIR Slope carve-out for the single-edge
// filter types, plus the per-target block field rule on whatever
// remains.  Public seam: the editor's slot-hidden callback delegates
// here, so the composed decision stays unit-testable the way
// osc_band_slot_hidden is.
bool osc_node_slot_hidden(const Graph& graph, uint32_t node_idx, uint32_t slot_idx);

// Progressive layer visibility on the Oscillator Output node: the visible
// rows extend to the gray add slot (the slot after the wired run's
// length); every row above the gray slot hides - a hidden slot renders
// no connector and no dot hit target.  A non-layer slot index - including
// the no-slot sentinel - never hides.
// Pure render-state predicate: all seven slots stay allocated, only the
// filter hides rows.
bool osc_layer_slot_hidden(uint32_t slot_idx, const uint8_t source_row[Synth::max_layers]);
// Volume envelope keeps a nonnegative range (tail and first-point values remain authored).
bool env_volume_shape_ok(const Synth::EnvelopeDescriptor& env);

// Forces the volume-envelope shape on a descriptor: minimum 0 with the
// effective top preserved; raw point values stay authored.
// Returns false when the effective top is negative, which cannot be
// preserved under the nonnegative volume range.  The apply path runs it
// when a wire lands on a volume target; the drop-commit preset runs it
// before the commit.
bool env_convert_to_volume_shape(Synth::EnvelopeDescriptor& env);

// Reports whether the envelope descriptor id serves a volume target, a
// non-volume target, or both.  Descriptor ids are shared by every cell that
// wires them and by every instrument in the bank, so the answer covers all
// users.  exclude_connection omits one connection (a wire move re-lands it)
// from the scan.
void env_target_usage(const Graph&           graph,
                      const OscGraphMapping& mapping,
                      uint16_t               desc_id,
                      bool*                  volume_used,
                      bool*                  other_used,
                      uint32_t               exclude_connection = pool_no_slot);

// Index of the zone's layout record with the given key, or -1.
int32_t find_record(const Synth::InstrumentEditorBank& bank,
                    uint32_t                           channel,
                    uint32_t                           zone,
                    uint32_t                           kind,
                    uint32_t                           index,
                    uint8_t                            uid);

// Recomputes the generator-record reconciliation and the compiled
// instrument after an eventless graph rewrite; the drain does this
// implicitly at the end of every applied batch.
bool refresh_osc_graph_compilation(Synth::InstrumentEditorBank* bank,
                                   Graph&                       graph,
                                   OscGraphMapping&             mapping,
                                   uint32_t                     channel,
                                   uint32_t                     zone);

// One sparse per-zone node layout record lives in sculptor_instr_bank.h
// next to its storage inside InstrumentEditorBank.

// Node population the projection derives for one zone: fixed nodes,
// oscillators, generator instances and record-driven surplus.
// Preflights (record capacity, node pool) count against it.
uint32_t count_projected_nodes(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone);

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

// Updates or appends the layout record keyed by (channel, zone, kind,
// index) with uid 0: the lookup selects bound records whose uid is 0, and
// records with another uid are not selected.  The record's position and
// size overrides follow the node, and a missing record appends zeroed when
// the array has capacity.  Returns the existing-or-appended record, or null
// on capacity refusal.
Synth::GraphNodeLayout* upsert_graph_layout_record(Synth::InstrumentEditorBank* bank,
                                                   uint32_t                     channel,
                                                   uint32_t                     zone,
                                                   uint8_t                      kind,
                                                   uint8_t                      index,
                                                   const Sculptor::Node&        node_ref);
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

// Publishes one zone's playback form: the layers the zone's source row
// wires, in slot order, with the instrument-level fields preserved.
// layer_count 0 is legal output (the caller refuses it where required).
bool compile_playback_instrument(const Synth::Instrument& editor_instr,
                                 const uint8_t            source_row[Synth::max_layers],
                                 Synth::Instrument*       out);
// Identity wiring for one zone: slot s is fed by oscillator s for
// s < layer_count, every later slot is unwired.  Every site that installs
// a playback-form instrument writes identity wiring through this helper.
void set_identity_osc_sources(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Publish-time channel_enabled mask: a channel publishes enabled only when
// no zone's source row is all zero (nothing wired) and no zone's instrument
// has zero layers (a zero-layer instrument never publishes audibly).  The
// bank's stored channel_enabled values are left untouched (they stay 1).
void compute_publish_channel_enabled(const Synth::InstrumentEditorBank& bank, uint8_t out_enabled[Synth::max_channels]);

// Zone-table metadata mutation rules.  Records move together.
// zone_split_new non-first-note case: shifts zones > `zone` by +1, then copies
// zone `zone`'s instrument records into zone `zone` + 1.
// Channel-wide effect records are neither copied nor shifted.  Returns false
// without mutating anything when the copied records would overflow the
// global record list.
bool zone_records_split_copy(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Clones the generator descriptors the split's new zone references.
// zone_split_new copies the instrument struct, so the new zone's target
// bindings still name the source zone's envelope and LFO descriptor ids and
// an edit in either zone would rewrite the other zone's sound.  Every
// referenced descriptor gains a fresh pool slot and the new zone's target
// bindings and kind-1/2 layout records re-point at the copies.  Several
// targets naming one descriptor still share the clone, so intra-zone
// sharing stays intact.  Returns false when a pool lacks capacity; the
// caller refuses the whole split, so no state leaks.
bool clone_split_generators(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Drops zone `zone`'s records and shifts later zones down by one
// (zone_delete, and joins that remove an entry).
void zone_records_drop_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
void zone_records_clear_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone);
// Drops ALL records of every zone of the channel (library load,
// channel init, channel delete).
void channel_records_reset(Synth::InstrumentEditorBank* bank, uint32_t channel);

} // namespace Sculptor
