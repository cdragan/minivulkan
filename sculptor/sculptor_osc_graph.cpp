// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_osc_graph.h"
#include "sculptor_instr_envelope_edit.h"

#include <cassert>
#include <cmath>
#include <stdio.h>
#include <string.h>

namespace {

// The editor-facing helpers exported from sculptor_osc_graph.h are used
// unqualified in this file's local sections.
using Sculptor::connection_into;
using Sculptor::find_param;
using Sculptor::param_src_input;
using Sculptor::param_target_names;

// The five targets the runtime can modulate per oscillator; duty A/B, osc
// mix and FM index are per-oscillator constants carried as base values only
// (shared predicate lives in sculptor_instr_bank.h next to the canonical
// record numbering).
bool is_projected_target(Synth::ModTarget target)
{
    return Synth::graph_target_projected(target);
}

uint32_t projected_index(Synth::ModTarget target)
{
    return target <= Synth::mod_panning ? static_cast<uint32_t>(target) : static_cast<uint32_t>(target) - 4u;
}

// The projection arithmetic relies on this exact ModTarget ordering.
static_assert(static_cast<uint32_t>(Synth::mod_panning) == 2);
static_assert(static_cast<uint32_t>(Synth::mod_duty0) == 3);
static_assert(static_cast<uint32_t>(Synth::mod_fm_index) == 6);
static_assert(static_cast<uint32_t>(Synth::mod_lowpass_cutoff) == 7);
static_assert(Synth::num_mod_targets == 9);
static_assert(Synth::max_mod_inputs == 2);

uint32_t projected_target_of_index(uint32_t projected)
{
    return static_cast<uint32_t>(Synth::graph_projected_target(projected));
}

const char* const target_names[Synth::num_mod_targets] = { "volume",       "pitch",    "panning", "Duty A",  "Duty B",
                                                           "Waveform Mix", "FM Depth", "lowpass", "highpass" };
const char* const source_names[Sculptor::num_osc_graph_inputs] = { "Pitch bend", "Mod wheel",  "Channel press.",
                                                                   "Velocity",   "Aftertouch", "Pressure (max)" };
const char* const wave_names[5]                                = { "off", "sine", "saw", "pulse", "noise" };
const char* const osc_mode_names[3]                            = { "blend", "fm", "hard sync" };
const char* const op_names[2]                                  = { "+", "x" };

// Oscillator node slot layout.  The order is fixed: project fills slots in
// exactly this order and compile reads properties back by index, so the two
// stay consistent by construction.
//   0        output
//   1        waveform a (list)
//   2        duty a (shared constant, [0, 1])
//   3        waveform b (list)
//   4        duty b (shared constant, [0, 1])
//   5        mix mode (list)
//   6        waveform mix (shared constant, [0, 1])
//   7        fm depth (shared constant)
//   8        fm ratio (per oscillator)
//   9..11    connectable dynamic values: volume, pan, pitch (shared routing)
//   12       pitch offset (per oscillator)
//   13..14   connectable dynamic values: lowpass, highpass (shared routing)
constexpr uint32_t osc_waveform_a_prop   = 1;
constexpr uint32_t osc_waveform_b_prop   = 3;
constexpr uint32_t osc_mode_prop         = 5;
constexpr uint32_t osc_fm_ratio_prop     = 8;
constexpr uint32_t osc_pitch_offset_prop = 12;
constexpr uint32_t osc_slot_count        = 15;

// Per-target presentation views: the single source of truth for value
// bounds, bank-unit scaling and slider curve.  Oscillator rows and
// parameter base values show display units; the bank stores scaled
// units (radians for fm depth, Hz for the cutoffs).
constexpr Sculptor::OscTargetView osc_target_views[Synth::num_mod_targets] = {
    { 0.0f, 1.0f, 1.0f, false },       // volume
    { -24.0f, 24.0f, 1.0f, false },    // pitch (semitones)
    { 0.0f, 1.0f, 1.0f, false },       // pan (0.5 = center)
    { 0.0f, 1.0f, 1.0f, false },       // duty a
    { 0.0f, 1.0f, 1.0f, false },       // duty b
    { 0.0f, 1.0f, 1.0f, false },       // waveform mix
    { 0.0f, 1.0f, 6.2831853f, false }, // fm depth (1.0 == 2*pi radians)
    { 20.0f, 20000.0f, 1.0f, true },   // lowpass (Hz)
    { 20.0f, 20000.0f, 1.0f, true },   // highpass (Hz)
};

// The value row of each ModTarget on an oscillator node.  The rows are not
// contiguous, so the mapping is a table indexed by ModTarget.
constexpr uint32_t osc_target_slots[Synth::num_mod_targets] = {
    9,  // volume
    11, // pitch
    10, // pan
    2,  // duty a
    4,  // duty b
    6,  // waveform mix
    7,  // fm depth
    13, // lowpass
    14, // highpass
};

uint32_t osc_target_prop(Synth::ModTarget target)
{
    return osc_target_slots[static_cast<uint32_t>(target)];
}

// The projected index (0..4) of a connectable dynamic value row, or -1 when
// the row is a shared constant or not a target value row at all.
int32_t osc_projected_row_index(uint32_t slot_idx)
{
    for (uint32_t proj = 0; proj < 5; ++proj) {
        if (osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(proj))) == slot_idx) {
            return static_cast<int32_t>(proj);
        }
    }
    return -1;
}

bool osc_row_is_dynamic(uint32_t slot_idx)
{
    return osc_projected_row_index(slot_idx) >= 0;
}

// Shared constant rows sync across every oscillator's inline view; fm ratio
// and pitch offset are per oscillator and do not.
bool osc_row_is_constant(uint32_t slot_idx)
{
    return slot_idx == osc_target_prop(Synth::mod_duty0) || slot_idx == osc_target_prop(Synth::mod_duty1) ||
           slot_idx == osc_target_prop(Synth::mod_osc_mix) || slot_idx == osc_target_prop(Synth::mod_fm_index);
}

// Parameter node slot layout: thirteen slots forming six renderer rows -
// output, value, envelope input, the LFO row (input + op + amount + rate
// scale, one shared renderer line) and the two source rows (input + op +
// scale each, one shared renderer line per source).
// 0        output
// 1        value (shared routing base value)
// 2        envelope input
// 3..6     lfo row: input, op, amount (lfo_depth), rate scale
// 7..9     source 0 row: input, op, scale
// 10..12   source 1 row: input, op, scale
constexpr uint32_t param_value_prop      = 1;
constexpr uint32_t param_env_input       = 2;
constexpr uint32_t param_lfo_input       = 3;
constexpr uint32_t param_lfo_op_prop     = 4;
constexpr uint32_t param_lfo_depth_prop  = 5;
constexpr uint32_t param_rate_scale_prop = 6;

constexpr uint32_t param_src_op_prop(uint32_t i)
{
    return 8u + 3u * i;
}

constexpr uint32_t param_src_scale_prop(uint32_t i)
{
    return 9u + 3u * i;
}

// Row groups: the LFO and source rows pack the connector and its inline
// widgets onto one renderer line.
constexpr uint8_t param_lfo_row_group  = 1;
constexpr uint8_t param_src0_row_group = 2;

// Envelope node slot layout: 0 output, then descriptor-content properties.

// LFO node slot layout: 0 output, 1 depth source, 2 rate source, then
// descriptor-content properties.
constexpr uint32_t lfo_depth_input = 1;
constexpr uint32_t lfo_rate_input  = 2;

// Output node slot layout: one input per layer, then the skew properties.
constexpr uint32_t input_note_detune_prop = Sculptor::num_osc_graph_inputs;
constexpr uint32_t input_osc_detune_prop  = Sculptor::num_osc_graph_inputs + 1;

Sculptor::Slot make_slot(const char*            name,
                         Sculptor::SlotKind     kind,
                         Sculptor::PropertyType type = Sculptor::PropertyType::unused)
{
    Sculptor::Slot slot = {};
    snprintf(slot.name, sizeof(slot.name), "%s", name);
    slot.kind          = kind;
    slot.connectable   = false;
    slot.property_type = type;
    return slot;
}

Sculptor::Slot output_slot(const char* name)
{
    return make_slot(name, Sculptor::SlotKind::output);
}

Sculptor::Slot input_slot(const char* name)
{
    return make_slot(name, Sculptor::SlotKind::input);
}

Sculptor::Slot real_slot(const char* name, float value)
{
    Sculptor::Slot slot = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::real);
    slot.value.real     = value;
    return slot;
}

// Bounded real: the renderer draws a slider clamped to the range.
Sculptor::Slot bounded_real_slot(const char* name, float value, float min_value, float max_value)
{
    Sculptor::Slot slot = real_slot(name, value);
    slot.real_min       = min_value;
    slot.real_max       = max_value;
    slot.real_bounded   = true;
    return slot;
}

Sculptor::Slot int_slot(const char* name, int32_t value)
{
    Sculptor::Slot slot = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::integer);
    slot.value.integer  = value;
    return slot;
}

Sculptor::Slot list_slot(const char* name, const char* const* options, uint32_t num_options, uint32_t index)
{
    Sculptor::Slot slot   = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::list);
    slot.num_list_options = static_cast<uint8_t>(num_options);
    for (uint32_t i = 0; i < num_options; ++i) {
        snprintf(slot.list_options[i], sizeof(slot.list_options[i]), "%s", options[i]);
    }
    slot.value.list_index = static_cast<uint8_t>(index);
    return slot;
}

// Shared constant value row: display units from the target view
// (fm depth normalized to 0..1), bounded, optionally logarithmic.
Sculptor::Slot osc_view_real_slot(const char* name, float bank_value, Synth::ModTarget target)
{
    const Sculptor::OscTargetView view = osc_target_views[static_cast<uint32_t>(target)];
    Sculptor::Slot slot   = bounded_real_slot(name, bank_value / view.bank_scale, view.min_value, view.max_value);
    slot.real_logarithmic = view.logarithmic;
    return slot;
}

// Connectable value row: edits the shared routing base value while
// unconnected; a parameter wire greys it (presentational) and attaches
// the gen binding instead.
Sculptor::Slot osc_view_connectable_slot(const char* name, float bank_value, Synth::ModTarget target)
{
    Sculptor::Slot slot = osc_view_real_slot(name, bank_value, target);
    slot.connectable    = true;
    return slot;
}

enum NodeRole {
    role_none,
    role_input,
    role_osc,
    role_env,
    role_lfo,
    role_output,
    role_param
};

NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    if (mapping.input_node == node_idx) {
        return role_input;
    }
    if (mapping.output_node == node_idx) {
        return role_output;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == node_idx) {
            return role_osc;
        }
    }
    for (uint32_t i = 0; i < mapping.param_count; ++i) {
        if (mapping.params[i].node_idx == node_idx) {
            return role_param;
        }
    }
    // Generator instances resolve through the registry, so they validate on
    // connect and their content edits route into the descriptor-edit path.
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].node_idx == node_idx) {
            return mapping.detached[i].kind == 1 ? role_env : role_lfo;
        }
    }
    return role_none;
}

int32_t find_instance(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    if (node_idx == Sculptor::pool_no_slot) {
        return -1;
    }
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].node_idx == node_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

enum OutputKind {
    out_none,
    out_input,
    out_osc,
    out_env,
    out_lfo,
    out_param
};

OutputKind classify_output(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_input:
            return out_input;
        case role_osc:
            return point.slot_idx == mapping.osc_output_slot ? out_osc : out_none;
        case role_env:
            return point.slot_idx == mapping.env_output_slot ? out_env : out_none;
        case role_lfo:
            return point.slot_idx == mapping.lfo_output_slot ? out_lfo : out_none;
        case role_param:
            return point.slot_idx == mapping.param_output_slot ? out_param : out_none;
        default:
            return out_none;
    }
}

enum InputKind {
    in_none,
    in_target, // oscillator connectable value row (projected index via osc_projected_row_index)
    in_penv,
    in_plfo,
    in_psrc,
    in_depth,
    in_rate,
    in_sum
};

InputKind classify_input(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_osc:
            if (osc_row_is_dynamic(point.slot_idx)) {
                return in_target;
            }
            return in_none;
        case role_param:
            if (point.slot_idx == param_env_input) {
                return in_penv;
            }
            if (point.slot_idx == param_lfo_input) {
                return in_plfo;
            }
            if (point.slot_idx == param_src_input(0) || point.slot_idx == param_src_input(1)) {
                return in_psrc;
            }
            return in_none;
        case role_lfo:
            if (point.slot_idx == mapping.lfo_depth_input_slot) {
                return in_depth;
            }
            if (point.slot_idx == mapping.lfo_rate_input_slot) {
                return in_rate;
            }
            return in_none;
        case role_output:
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (point.slot_idx == mapping.output_layer_input_slot[layer]) {
                    return in_sum;
                }
            }
            return in_none;
        default:
            return in_none;
    }
}

// Resolves the MIDI source feeding a connector through the inputs node's
// per-source slots; false when the source is not a projected MIDI input.
bool edge_source(const Sculptor::Graph&           graph,
                 const Sculptor::OscGraphMapping& mapping,
                 uint32_t                         connection_idx,
                 Synth::ModSource*                source)
{
    const uint32_t node_idx = graph.get_connection(connection_idx).output.node_idx;
    if (node_idx != mapping.input_node) {
        return false;
    }
    const uint32_t slot_idx = graph.get_connection(connection_idx).output.slot_idx;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        if (mapping.input_source_slots[i] == slot_idx) {
            *source = static_cast<Synth::ModSource>(i + 1);
            return true;
        }
    }
    return false;
}

bool read_list_prop(const Sculptor::Slot& slot, uint32_t* index)
{
    if (slot.property_type != Sculptor::PropertyType::list || slot.value.list_index >= slot.num_list_options) {
        return false;
    }
    *index = slot.value.list_index;
    return true;
}

void clear_mapping(Sculptor::OscGraphMapping* mapping)
{
    mapping->source_bank = nullptr;
    mapping->input_node  = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        mapping->input_source_slots[i] = Sculptor::pool_no_slot;
    }
    mapping->output_node = Sculptor::pool_no_slot;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        mapping->output_layer_input_slot[layer] = Sculptor::pool_no_slot;
        mapping->osc_nodes[layer]               = Sculptor::pool_no_slot;
    }
    mapping->osc_output_slot = Sculptor::pool_no_slot;
    // Uniform node layouts; populated even when no such node exists so the
    // mapping always describes the full projection layout.
    mapping->env_output_slot      = 0;
    mapping->lfo_output_slot      = 0;
    mapping->lfo_depth_input_slot = lfo_depth_input;
    mapping->lfo_rate_input_slot  = lfo_rate_input;
    mapping->param_output_slot    = 0;
    mapping->channel              = 0;
    mapping->zone                 = 0;
    mapping->param_count          = 0;
    for (uint32_t i = 0; i < Sculptor::max_detached_nodes; ++i) {
        mapping->detached[i].node_idx     = Sculptor::pool_no_slot;
        mapping->detached[i].kind         = 0;
        mapping->detached[i].desc_id      = 0;
        mapping->detached[i].uid          = 0;
        mapping->detached[i].depth_source = 0;
        mapping->detached[i].rate_source  = 0;
    }
    mapping->detached_count = 0;
}

void project_osc_node(Sculptor::Graph*           graph,
                      uint32_t                   layer,
                      const Synth::Instrument&   instrument,
                      Sculptor::OscGraphMapping* mapping)
{
    char name[32];
    snprintf(name, sizeof(name), "Oscillator %u", layer + 1);
    const uint32_t node = graph->create_node(name, vmath::vec2(512.0f, 256.0f * static_cast<float>(layer)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    mapping->osc_nodes[layer]    = node;
    const Synth::Oscillator& osc = instrument.layers[layer];

    mapping->osc_output_slot = graph->add_slot(node, output_slot("Output"));

    // Waveform A cannot be off: the list starts at sine and compile adds
    // one back onto the wave type.  Old banks storing off clamp to sine.
    graph->add_slot(
        node,
        list_slot("Waveform A",
                  wave_names + 1,
                  4,
                  osc.osc_type[0] == Synth::WaveType::no_wave ? 0 : static_cast<uint32_t>(osc.osc_type[0]) - 1));
    graph->add_slot(
        node,
        osc_view_real_slot(target_names[3], instrument.routing[Synth::mod_duty0].base_value, Synth::mod_duty0));
    graph->add_slot(node, list_slot("Waveform B", wave_names, 5, static_cast<uint32_t>(osc.osc_type[1])));
    graph->add_slot(
        node,
        osc_view_real_slot(target_names[4], instrument.routing[Synth::mod_duty1].base_value, Synth::mod_duty1));
    graph->add_slot(node, list_slot("Mix Mode", osc_mode_names, 3, static_cast<uint32_t>(osc.osc_mode)));
    graph->add_slot(
        node,
        osc_view_real_slot(target_names[5], instrument.routing[Synth::mod_osc_mix].base_value, Synth::mod_osc_mix));
    graph->add_slot(
        node,
        osc_view_real_slot(target_names[6], instrument.routing[Synth::mod_fm_index].base_value, Synth::mod_fm_index));
    graph->add_slot(node, bounded_real_slot("FM Ratio", osc.mod_ratio, 0.125f, 8.0f));
    graph->add_slot(
        node,
        osc_view_connectable_slot("Volume", instrument.routing[Synth::mod_volume].base_value, Synth::mod_volume));
    graph->add_slot(
        node,
        osc_view_connectable_slot("Pan", instrument.routing[Synth::mod_panning].base_value, Synth::mod_panning));
    graph->add_slot(
        node,
        osc_view_connectable_slot("Pitch", instrument.routing[Synth::mod_pitch].base_value, Synth::mod_pitch));
    graph->add_slot(node, bounded_real_slot("Pitch Offset", osc.pitch_offset, -12.0f, 12.0f));
    graph->add_slot(node,
                    osc_view_connectable_slot("Lowpass",
                                              instrument.routing[Synth::mod_lowpass_cutoff].base_value,
                                              Synth::mod_lowpass_cutoff));
    graph->add_slot(node,
                    osc_view_connectable_slot("Highpass",
                                              instrument.routing[Synth::mod_highpass_cutoff].base_value,
                                              Synth::mod_highpass_cutoff));
}

// Node construction shared by the derived projection and record-driven
// creation, so the slot layouts cannot drift.  Slot 0 is always the output;
// env content follows, then for LFOs the depth/rate inputs.
uint32_t create_env_node(Sculptor::Graph*                 graph,
                         const char*                      name,
                         vmath::vec2                      position,
                         const Synth::EnvelopeDescriptor& env)
{
    const uint32_t node = graph->create_node(name, position);
    if (node == Sculptor::pool_no_slot) {
        return Sculptor::pool_no_slot;
    }
    // Records carry arbitrary (unsnapped) editor positions; the exact
    // position overrides create_node's grid snap.  Envelope nodes are wider
    // so the curve widget has room to work.
    graph->set_node_layout(node, position, Sculptor::envelope_node_content_width, 0.0f);
    graph->add_slot(node, output_slot("Value"));
    graph->add_slot(node, real_slot("Value (min)", env.min_value));
    graph->add_slot(node, real_slot("Value (max)", env.min_value + 65535.0f * env.min_max_delta));
    // Point and sustain editing lives in the curve state widget; property
    // slots for those fields would be a second stale UI over the same data.
    return node;
}

uint32_t create_lfo_node(Sculptor::Graph*            graph,
                         const char*                 name,
                         vmath::vec2                 position,
                         const Synth::LFODescriptor& lfo)
{
    const uint32_t node = graph->create_node(name, position);
    if (node == Sculptor::pool_no_slot) {
        return Sculptor::pool_no_slot;
    }
    // Records carry arbitrary (unsnapped) editor positions; the exact
    // position overrides create_node's grid snap.
    graph->set_node_layout(node, position, 0.0f, 0.0f);
    graph->add_slot(node, output_slot("Value"));
    graph->add_slot(node, input_slot("Depth"));
    graph->add_slot(node, input_slot("Rate"));
    graph->add_slot(node, list_slot("Waveform", wave_names, 5, static_cast<uint32_t>(lfo.wave)));
    graph->add_slot(node, bounded_real_slot("Duty", static_cast<float>(lfo.duty) / 255.0f, 0.0f, 1.0f));
    graph->add_slot(node, int_slot("Period (ms)", lfo.period_ms));
    return node;
}

uint32_t create_param_node(Sculptor::Graph*           graph,
                           const char*                name,
                           vmath::vec2                position,
                           Synth::ModTarget           target,
                           float                      base_value,
                           uint32_t                   lfo_op_index,
                           float                      lfo_depth_init,
                           float                      lfo_rate_scale_init,
                           const Synth::InputRouting& routing)
{
    const uint32_t node = graph->create_node(name, position);
    if (node == Sculptor::pool_no_slot) {
        return Sculptor::pool_no_slot;
    }
    // A parameter owns its stored title (the record name), so the title
    // editor stays available even though the graph disables node-state
    // edits graph-wide.
    graph->set_node_renamable(node, true);
    graph->set_node_layout(node, position, 0.0f, 0.0f);
    graph->add_slot(node, output_slot("Value"));
    graph->add_slot(node, osc_view_real_slot("Base Value", base_value, target));
    graph->add_slot(node, input_slot("Envelope"));
    // The LFO row: connector plus inline widgets on one renderer line.
    Sculptor::Slot lfo_in = input_slot("LFO");
    lfo_in.row_group      = param_lfo_row_group;
    graph->add_slot(node, lfo_in);
    Sculptor::Slot lfo_op = list_slot("Op", op_names, 2, lfo_op_index);
    lfo_op.row_group      = param_lfo_row_group;
    graph->add_slot(node, lfo_op);
    Sculptor::Slot lfo_depth = real_slot("Depth", lfo_depth_init);
    lfo_depth.row_group      = param_lfo_row_group;
    graph->add_slot(node, lfo_depth);
    Sculptor::Slot rate_scale = real_slot("Rate x", lfo_rate_scale_init);
    rate_scale.row_group      = param_lfo_row_group;
    graph->add_slot(node, rate_scale);
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        Sculptor::Slot src_in = input_slot(i == 0 ? "Source A" : "Source B");
        src_in.row_group      = static_cast<uint8_t>(param_src0_row_group + i);
        graph->add_slot(node, src_in);
        Sculptor::Slot src_op = list_slot("Op", op_names, 2, static_cast<uint32_t>(routing.inputs[i].op));
        src_op.row_group      = static_cast<uint8_t>(param_src0_row_group + i);
        graph->add_slot(node, src_op);
        Sculptor::Slot src_scale = real_slot("Amount", routing.inputs[i].scale);
        src_scale.row_group      = static_cast<uint8_t>(param_src0_row_group + i);
        graph->add_slot(node, src_scale);
    }
    return node;
}

// Duplicates of one target are disambiguated by enumeration order over the
// whole registry (derived parameters first, record surplus after).
void name_params(Sculptor::Graph* graph, const Sculptor::OscGraphMapping& mapping)
{
    uint32_t ordinals[5] = {};
    for (uint32_t i = 0; i < mapping.param_count; ++i) {
        const Sculptor::ParamEntry& param = mapping.params[i];
        if (param.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(param.node_idx)) {
            continue;
        }
        char name[32];
        if (++ordinals[param.target] > 1) {
            snprintf(name, sizeof(name), "%s %u", param_target_names[param.target], ordinals[param.target]);
        }
        else {
            graph->rename_node(param.node_idx, param_target_names[param.target]);
            continue;
        }
        graph->rename_node(param.node_idx, name);
    }
}

// The per-cell binding tuple: cells of one target merge into one parameter
// iff all fields match. lfo_depth_source/lfo_rate_source are per-cell bank
// fields, so they must take part: two cells sharing one LFO descriptor but
// differing in depth source stay separate parameters.
struct ParamGroup {
    uint8_t  target; // projected index 0..4
    uint16_t env_desc_id;
    uint16_t lfo_desc_id;
    uint8_t  lfo_op;
    float    lfo_depth;
    float    lfo_rate_scale_ms;
    uint8_t  depth_source;
    uint8_t  rate_source;
    uint8_t  served; // bitset of merged cells' layers
};

bool same_tuple(const ParamGroup& a, const Synth::LayerGen& gen)
{
    return a.env_desc_id == gen.envelope_desc_id && a.lfo_desc_id == gen.lfo_desc_id &&
           a.lfo_op == static_cast<uint8_t>(gen.lfo_op) && a.lfo_depth == gen.lfo_depth &&
           a.lfo_rate_scale_ms == gen.lfo_rate_scale_ms &&
           a.depth_source == static_cast<uint8_t>(gen.lfo_depth_source) &&
           a.rate_source == static_cast<uint8_t>(gen.lfo_rate_source);
}

// Deterministic derivation: targets in ModTarget order, cells in layer
// order, groups in first-cell order.  A cell needs a parameter when its gen
// carries a binding; a target whose routing carries MIDI inputs keeps a
// zero-tuple carrier only when no bound cell gives it a parameter already
// (any parameter of the target carries the shared routing views, so a
// synthesized carrier next to a bound one would be a phantom node).  This
// is load-only materialization: compile zeroes the inputs of a target
// whose last serving parameter died, so editor edits cannot produce the
// inputs-without-bound-cell state the carrier exists to cover.
uint32_t derive_param_groups(const Synth::Instrument& instrument, ParamGroup* groups)
{
    uint32_t       count = 0;
    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    for (uint32_t proj = 0; proj < 5; ++proj) {
        const Synth::ModTarget     target  = static_cast<Synth::ModTarget>(projected_target_of_index(proj));
        const Synth::InputRouting& routing = instrument.routing[target];
        const uint32_t             num_inputs =
            routing.num_inputs <= Synth::max_mod_inputs ? routing.num_inputs : Synth::max_mod_inputs;
        uint32_t target_groups = 0;
        for (uint32_t layer = 0; layer < layer_count; ++layer) {
            const Synth::LayerGen& gen = instrument.layers[layer].gen[target];
            if (gen.envelope_desc_id == 0 && gen.lfo_desc_id == 0) {
                continue;
            }
            uint32_t found = count;
            for (uint32_t g = count - target_groups; g < count; ++g) {
                if (same_tuple(groups[g], gen)) {
                    found = g;
                    break;
                }
            }
            if (found == count) {
                groups[count].target            = static_cast<uint8_t>(proj);
                groups[count].env_desc_id       = gen.envelope_desc_id;
                groups[count].lfo_desc_id       = gen.lfo_desc_id;
                groups[count].lfo_op            = static_cast<uint8_t>(gen.lfo_op);
                groups[count].lfo_depth         = gen.lfo_depth;
                groups[count].lfo_rate_scale_ms = gen.lfo_rate_scale_ms;
                groups[count].depth_source      = static_cast<uint8_t>(gen.lfo_depth_source);
                groups[count].rate_source       = static_cast<uint8_t>(gen.lfo_rate_source);
                groups[count].served            = 0;
                ++count;
                ++target_groups;
            }
            groups[found].served |= static_cast<uint8_t>(1u << layer);
        }
        if (num_inputs > 0 && target_groups == 0) {
            // Pure MIDI carrier: no bound cell, but the shared routing views
            // (base value and source rows) must stay reachable for compile.
            groups[count].target            = static_cast<uint8_t>(proj);
            groups[count].env_desc_id       = 0;
            groups[count].lfo_desc_id       = 0;
            groups[count].lfo_op            = 0;
            groups[count].lfo_depth         = 0.0f;
            groups[count].lfo_rate_scale_ms = 0.0f;
            groups[count].depth_source      = 0;
            groups[count].rate_source       = 0;
            groups[count].served            = static_cast<uint8_t>((1u << layer_count) - 1u);
            ++count;
        }
    }
    return count;
}

// Generator instance identity: (kind, descriptor, LFO depth/rate sources).
// The sources take part because they ride the LFO node's depth/rate input
// edges - two parameters sharing one LFO descriptor but differing in depth
// source each need their own instance.
struct InstanceKey {
    uint8_t kind;
    uint8_t desc_id;
    uint8_t depth_source;
    uint8_t rate_source;
};

// Appends a derivation key unless an identical key is already listed;
// the keys array lists each distinct instance once.
void append_instance_key(InstanceKey* keys, uint32_t& count, const InstanceKey& key)
{
    uint32_t k = 0;
    while (k < count && (keys[k].kind != key.kind || keys[k].desc_id != key.desc_id ||
                         keys[k].depth_source != key.depth_source || keys[k].rate_source != key.rate_source)) {
        ++k;
    }
    if (k == count) {
        keys[count++] = key;
    }
}

uint32_t derive_instance_keys(const ParamGroup* groups, uint32_t group_count, InstanceKey* keys)
{
    uint32_t count = 0;
    for (uint32_t g = 0; g < group_count; ++g) {
        if (groups[g].env_desc_id != 0) {
            const InstanceKey key = { 1, static_cast<uint8_t>(groups[g].env_desc_id), 0, 0 };
            append_instance_key(keys, count, key);
        }
        if (groups[g].lfo_desc_id != 0) {
            const InstanceKey key = { 2,
                                      static_cast<uint8_t>(groups[g].lfo_desc_id),
                                      groups[g].depth_source,
                                      groups[g].rate_source };
            append_instance_key(keys, count, key);
        }
    }
    return count;
}

// Installs the renderer's visual roles after a projection rebuild:
// parameter nodes tint with the parameter colors and every shared routing
// row (value row + source op/scale rows) carries the shared-row marker.
// clear() wipes the roles at projection start, so stale roles cannot
// survive a rebuild.
void install_visual_roles(Sculptor::Graph* graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t p = 0; p < mapping.param_count; ++p) {
        const Sculptor::ParamEntry& param = mapping.params[p];
        if (param.node_idx == Sculptor::pool_no_slot) {
            continue;
        }
        graph->set_node_visual_role(param.node_idx, Sculptor::node_role_parameter);
        graph->set_slot_visual_role(param.node_idx, param_value_prop, Sculptor::slot_role_shared_row);
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            graph->set_slot_visual_role(param.node_idx, param_src_op_prop(i), Sculptor::slot_role_shared_row);
            graph->set_slot_visual_role(param.node_idx, param_src_scale_prop(i), Sculptor::slot_role_shared_row);
        }
    }
    // Every oscillator value row driven by a ModTarget shows the
    // instrument-wide routing entry: duty a/b, waveform mix, fm depth and
    // the five connectable rows are shared across layers; the remaining
    // rows (waveforms, mix mode, fm ratio, pitch offset) stay per oscillator.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == Sculptor::pool_no_slot) {
            continue;
        }
        for (uint32_t target_i = 0; target_i < Synth::num_mod_targets; ++target_i) {
            graph->set_slot_visual_role(mapping.osc_nodes[layer],
                                        osc_target_prop(static_cast<Synth::ModTarget>(target_i)),
                                        Sculptor::slot_role_shared_row);
        }
    }
}

// Wires a parameter node's source rows to the shared routing inputs (later
// user edits mirror eventlessly across the target's parameters).  Skips the
// none source and the one-past-the-end sentinel slot.
void wire_param_sources(Sculptor::Graph*                 graph,
                        const Sculptor::OscGraphMapping& mapping,
                        uint32_t                         param_node_idx,
                        const Synth::InputRouting&       routing)
{
    const uint32_t num_inputs =
        routing.num_inputs <= Synth::max_mod_inputs ? routing.num_inputs : Synth::max_mod_inputs;
    for (uint32_t i = 0; i < num_inputs; ++i) {
        const uint32_t source = static_cast<uint32_t>(routing.inputs[i].source);
        if (source == 0 || source > Sculptor::num_osc_graph_inputs) {
            continue;
        }
        graph->add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_source_slots[source - 1] },
                              Sculptor::EndPoint{ param_node_idx, param_src_input(i) });
    }
}

} // namespace

// Editor-facing helpers exported from sculptor_osc_graph.h.  Defined at
// file scope with explicit prefixes; the file-local sections above and below
// reach them through using-declarations.

bool Sculptor::env_volume_shape_ok(const Synth::EnvelopeDescriptor& env)
{
    // The span must be nonnegative too: a negative delta would evaluate the
    // interior points below the zero floor.
    return env.min_value == 0.0f && env.min_max_delta >= 0.0f && env.points[0].value == 0 &&
           env.points[env.num_points - 1].value == 0;
}

// Forces the volume-envelope shape: minimum 0 with the effective top preserved,
// and the first and the last point at 0.  Returns false when the effective top
// is negative, which cannot be preserved under the nonnegative volume range.
static bool env_convert_to_volume_shape(Synth::EnvelopeDescriptor& env)
{
    const float max_value = env.min_value + 65535.0f * env.min_max_delta;
    if (max_value < 0.0f) {
        return false;
    }
    env.min_value                        = 0.0f;
    env.min_max_delta                    = max_value / 65535.0f;
    env.points[0].value                  = 0;
    env.points[env.num_points - 1].value = 0;
    return true;
}

void Sculptor::env_target_usage(const Graph&           graph,
                                const OscGraphMapping& mapping,
                                uint16_t               desc_id,
                                bool*                  volume_used,
                                bool*                  other_used,
                                int32_t                exclude_param_idx,
                                uint32_t               exclude_connection)
{
    *volume_used = false;
    *other_used  = false;
    for (uint32_t p = 0; p < mapping.param_count; ++p) {
        if (static_cast<int32_t>(p) == exclude_param_idx) {
            continue;
        }
        const ParamEntry& param = mapping.params[p];
        if (param.node_idx == pool_no_slot || ! graph.node_occupied(param.node_idx)) {
            continue;
        }
        const uint32_t env_conn = connection_into(graph, param.node_idx, param_env_input);
        if (env_conn == pool_no_slot || env_conn == exclude_connection) {
            continue;
        }
        const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
        if (inst < 0 || mapping.detached[inst].kind != 1 || mapping.detached[inst].desc_id != desc_id) {
            continue;
        }
        if (param.target == projected_index(Synth::mod_volume)) {
            *volume_used = true;
        }
        else {
            *other_used = true;
        }
    }
    // Descriptor ids live in a bank-wide pool, so users in other instruments
    // and zones count too; the projection records the bank it read.  The
    // projected instrument is skipped: the graph scan above is authoritative
    // for it (it reflects pending edits and knows which parameter serves
    // which generator, which the committed bank alone cannot attribute).
    if (mapping.source_bank != nullptr) {
        uint32_t self_instrument = Synth::max_instruments;
        if (mapping.channel < Synth::max_channels && mapping.zone < Synth::max_instr_per_channel) {
            const Synth::Zone& zone_entry = mapping.source_bank->channel_zones[mapping.channel][mapping.zone];
            // Only a valid zone entry identifies the projected instrument; a
            // bare bank (no zone table) has none to skip.
            if (zone_entry.start_note != 0 && zone_entry.instrument < mapping.source_bank->instruments.num_allocated) {
                self_instrument = zone_entry.instrument;
            }
        }
        for (uint32_t instr_idx = 0; instr_idx < mapping.source_bank->instruments.num_allocated; ++instr_idx) {
            if (instr_idx == self_instrument) {
                continue;
            }
            const Synth::Instrument& instrument = mapping.source_bank->instruments.entries[instr_idx];
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                    if (instrument.layers[layer].gen[t].envelope_desc_id != desc_id) {
                        continue;
                    }
                    if (static_cast<Synth::ModTarget>(t) == Synth::mod_volume) {
                        *volume_used = true;
                    }
                    else {
                        *other_used = true;
                    }
                }
            }
        }
    }
}

// The five parameter target display names, shared by the node menus, the
// immediate rename feedback and the projection's derived titles.
const char* const Sculptor::param_target_names[5] = { "Volume", "Pitch", "Panning", "Lowpass", "Highpass" };

Sculptor::OscTargetView Sculptor::osc_target_view(Synth::ModTarget target)
{
    return osc_target_views[static_cast<uint32_t>(target)];
}

// Slot index on an oscillator node holding the value row of projected
// parameter target `projected_index` (0..4); rows are not contiguous.
uint32_t Sculptor::osc_target_row(uint32_t projected_index)
{
    return osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(projected_index)));
}

// Registry index of the parameter projected onto node_idx, or -1.
int32_t Sculptor::find_param(const OscGraphMapping& mapping, uint32_t node_idx)
{
    if (node_idx == Sculptor::pool_no_slot) {
        return -1;
    }
    for (uint32_t i = 0; i < mapping.param_count; ++i) {
        if (mapping.params[i].node_idx == node_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// True when an earlier-enumerated same-target parameter is still record-less
// (derived): a record gained by the parameter at param_idx would attach to
// that sibling positionally at re-projection and hijack its node.
bool Sculptor::param_has_recordless_predecessor(const OscGraphMapping& mapping, uint32_t param_idx)
{
    const ParamEntry& param = mapping.params[param_idx];
    for (uint32_t q = 0; q < param_idx; ++q) {
        const ParamEntry& other = mapping.params[q];
        if (other.target == param.target && other.uid == 0 && other.node_idx != pool_no_slot) {
            return true;
        }
    }
    return false;
}

// Add Parameter preflight (see the header comment): the registry bound
// mirrors the projection's own refusal so the menu rejects before
// committing, and the record-less check mirrors the rename path.
bool Sculptor::osc_add_parameter_refused(const OscGraphMapping& mapping,
                                         const uint32_t         target_index,
                                         char*                  error,
                                         const uint32_t         error_size)
{
    if (mapping.param_count + 1 > max_param_nodes) {
        snprintf(error, error_size, "Synth: cannot add a parameter: the parameter registry is full");
        return true;
    }
    if (param_target_has_recordless_derived(mapping, target_index)) {
        snprintf(error, error_size, "Synth: cannot add a parameter: rename the target's earlier parameter first");
        return true;
    }
    return false;
}

// True when the target already has a record-less (derived) parameter: a
// record re-keyed into that target attaches to the sibling positionally at
// re-projection and hijacks its node.
bool Sculptor::param_target_has_recordless_derived(const OscGraphMapping& mapping, uint32_t target)
{
    for (uint32_t q = 0; q < mapping.param_count; ++q) {
        const ParamEntry& other = mapping.params[q];
        if (other.target == target && other.uid == 0 && other.node_idx != pool_no_slot) {
            return true;
        }
    }
    return false;
}

// One-based ordinal of the parameter among its target's derived
// parameters in enumeration order (== projection layer order). A kind-3
// record stores this ordinal so re-projection pairs it with the same
// derived group regardless of record uid order.
uint8_t Sculptor::param_group_ordinal(const OscGraphMapping& mapping, uint32_t param_idx)
{
    const ParamEntry& param   = mapping.params[param_idx];
    uint32_t          ordinal = 0;
    for (uint32_t q = 0; q < param_idx; ++q) {
        const ParamEntry& other = mapping.params[q];
        if (other.target == param.target && other.node_idx != pool_no_slot) {
            ++ordinal;
        }
    }
    return static_cast<uint8_t>(ordinal + 1);
}

// Eventless wire-driven retarget, shared by the Change Target menu and the
// apply path's cross-target wire drop (see the header comment). All writes
// are eventless mirrors; the caller owns guards, commit, undo and
// re-projection.
bool Sculptor::retarget_param(Synth::InstrumentEditorBank* bank,
                              Graph&                       graph,
                              OscGraphMapping&             mapping,
                              const uint32_t               channel,
                              const uint32_t               zone,
                              const uint32_t               param_idx,
                              const uint32_t               new_target)
{
    ParamEntry&    param      = mapping.params[param_idx];
    const uint32_t param_node = param.node_idx;
    // The parameter's attached envelope moves with the retarget: a volume
    // envelope may only serve the volume target, and only a volume envelope
    // may serve it.  Retargeting onto volume converts a nonconforming
    // descriptor; the descriptor is shared, so other users are honored.
    const uint32_t env_conn = connection_into(graph, param_node, param_env_input);
    if (env_conn != pool_no_slot) {
        const int32_t env_inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
        if (env_inst >= 0 && mapping.detached[env_inst].kind == 1) {
            const uint16_t desc_id     = mapping.detached[env_inst].desc_id;
            bool           volume_used = false;
            bool           other_used  = false;
            env_target_usage(graph, mapping, desc_id, &volume_used, &other_used, static_cast<int32_t>(param_idx));
            if (new_target == projected_index(Synth::mod_volume)) {
                if (other_used) {
                    return false;
                }
                if (desc_id != 0 && desc_id <= bank->bank.envelopes.num_allocated) {
                    Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[desc_id - 1];
                    if (! env_volume_shape_ok(env) && ! env_convert_to_volume_shape(env)) {
                        return false;
                    }
                }
            }
            else if (volume_used) {
                return false;
            }
        }
    }
    // The source rows are shared routing views: the captured wiring is the
    // parameter's own routing, which moves to the new target only when the
    // destination has no serving parameter to adopt from.
    EndPoint captured_sources[Synth::max_mod_inputs];
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        captured_sources[i] = EndPoint{ pool_no_slot, pool_no_slot };
        const uint32_t conn = connection_into(graph, param_node, param_src_input(i));
        if (conn != pool_no_slot) {
            captured_sources[i] = graph.get_connection(conn).output;
        }
    }
    int32_t record_idx = find_record(*bank, channel, zone, 3, param.target, param.uid);
    if (record_idx >= 0) {
        bank->graph_layout[record_idx].index = static_cast<uint8_t>(new_target);
        // At the new target the record joins that target's uid-order positional
        // attach over unclaimed groups, materializing its own node when no
        // eligible group remains.
        bank->graph_layout[record_idx].param_slot = Synth::graph_record_param_free;
    }
    else {
        // Defensive: a record-backed parameter without a live record gets one,
        // so the re-keyed identity survives re-projection.
        if (! graph_records_have_capacity(*bank, 1)) {
            return false;
        }
        Synth::GraphNodeLayout record                  = {};
        const Node&            node                    = graph.node(param_node);
        record.channel                                 = static_cast<uint8_t>(channel);
        record.zone                                    = static_cast<uint8_t>(zone);
        record.kind                                    = 3;
        record.index                                   = static_cast<uint8_t>(new_target);
        record.param_slot                              = Synth::graph_record_param_free;
        record.x                                       = node.position.x;
        record.y                                       = node.position.y;
        record.width_override                          = node.content_width_override;
        record.height_override                         = node.content_height_override;
        record.uid                                     = allocate_detached_uid(*bank, channel, zone, 3);
        bank->graph_layout[bank->graph_layout_count++] = record;
        param.uid                                      = record.uid;
    }
    // The mapping entry moves first: the compile gates a value wire by the
    // parameter's current target, and the triggering event must compile the
    // destination target's routing into the bank.
    const uint32_t old_target = param.target;
    param.target              = static_cast<uint8_t>(new_target);

    // The base value obeys the destination target's bounds: a parameter
    // moving from pitch to pan cannot keep an out-of-range base.  Eventless
    // write: the caller owns commit/undo.
    const Sculptor::OscTargetView dest = osc_target_views[static_cast<uint32_t>(projected_target_of_index(new_target))];
    if (dest.max_value > dest.min_value) {
        Sculptor::PropertyValue base = graph.node(param.node_idx).slots.entries[param_value_prop].value;
        base.real                    = base.real < dest.min_value   ? dest.min_value
                                       : base.real > dest.max_value ? dest.max_value
                                                                    : base.real;
        graph.set_slot_value(param.node_idx, param_value_prop, base);
    }

    // Retarget wires onto the new target's oscillator rows. The rewrite is
    // eventless through set_connection_input: the apply path runs inside a
    // drained batch, where an eventful write would echo back as a second
    // user edit. Only wires on the old target's rows move; a wire on
    // another target's row or off the oscillator rows entirely (defensive:
    // the validator never creates one) is dropped. A move onto an occupied
    // row frees the wire instead - the row keeps its single edge (the
    // dropped wire in the drop case), so no duplicate accumulates. Wires
    // are snapshotted first because the rewrites free connections while
    // the loop runs.
    uint32_t wires[max_connections];
    uint32_t wire_count = 0;
    for (uint32_t c = 0; c < max_connections; ++c) {
        if (! graph.connection_occupied(c)) {
            continue;
        }
        const Connection& connection = graph.get_connection(c);
        if (connection.output.node_idx == param_node && connection.output.slot_idx == mapping.param_output_slot) {
            wires[wire_count++] = c;
        }
    }
    for (uint32_t w = 0; w < wire_count; ++w) {
        const uint32_t    c          = wires[w];
        const Connection& connection = graph.get_connection(c);
        const uint32_t    input_node = connection.input.node_idx;
        const uint32_t    input_slot = connection.input.slot_idx;
        bool              osc_row    = false;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping.osc_nodes[layer] == input_node) {
                osc_row = true;
                break;
            }
        }
        if (! osc_row) {
            graph.set_connection_input(c, EndPoint{ pool_no_slot, pool_no_slot });
            continue;
        }
        const int32_t slot_target = osc_projected_row_index(input_slot);
        if (slot_target >= 0 && static_cast<uint32_t>(slot_target) == old_target) {
            const uint32_t new_slot =
                osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(new_target)));
            bool row_taken = false;
            for (uint32_t d = 0; d < max_connections && ! row_taken; ++d) {
                if (d == c || ! graph.connection_occupied(d)) {
                    continue;
                }
                const Connection& other = graph.get_connection(d);
                row_taken               = other.input.node_idx == input_node && other.input.slot_idx == new_slot;
            }
            if (row_taken) {
                graph.set_connection_input(c, EndPoint{ pool_no_slot, pool_no_slot });
            }
            else {
                graph.set_connection_input(c, EndPoint{ input_node, new_slot });
            }
        }
        else if (slot_target >= 0 && static_cast<uint32_t>(slot_target) == new_target) {
            // Already on the destination row (the dropped or dragged wire
            // itself): leave it alone.
        }
        else {
            // Anything else - another target's row or a nonsensical slot -
            // cannot belong to this parameter: free it (the validator never
            // creates one, so this only catches bypassing callers).
            graph.set_connection_input(c, EndPoint{ pool_no_slot, pool_no_slot });
        }
    }
    // Source rows are shared routing views and the destination target's
    // routing is authoritative: after the retarget every same-target
    // parameter's source rows must show identical wiring, so whichever
    // parameter compile picks as the routing carrier reads the same sources
    // and the carrier pick is order-independent. The retargeted parameter
    // adopts the first other serving parameter's wiring, or keeps its
    // captured wiring when the destination has none (it becomes the carrier).
    int32_t resident = -1;
    for (uint32_t q = 0; q < mapping.param_count && resident < 0; ++q) {
        if (q == param_idx || mapping.params[q].target != new_target || mapping.params[q].node_idx == pool_no_slot ||
            ! graph.node_occupied(mapping.params[q].node_idx)) {
            continue;
        }
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping.osc_nodes[layer] == pool_no_slot) {
                continue;
            }
            const uint32_t conn =
                connection_into(graph,
                                mapping.osc_nodes[layer],
                                osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(new_target))));
            if (conn != pool_no_slot && graph.get_connection(conn).output.node_idx == mapping.params[q].node_idx) {
                resident = static_cast<int32_t>(q);
                break;
            }
        }
    }
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        EndPoint wiring = EndPoint{ pool_no_slot, pool_no_slot };
        if (resident < 0) {
            wiring = captured_sources[i];
        }
        else {
            const uint32_t conn = connection_into(graph, mapping.params[resident].node_idx, param_src_input(i));
            if (conn != pool_no_slot) {
                wiring = graph.get_connection(conn).output;
            }
        }
        // A no-slot endpoint clears the row's stale wire in place.
        graph.set_slot_input(param_node, param_src_input(i), wiring);
    }
    return true;
}

// The live connection terminating at an input connector, or pool_no_slot.
uint32_t Sculptor::connection_into(const Graph& graph, uint32_t node_idx, uint32_t slot_idx)
{
    for (uint32_t i = 0; i < Sculptor::max_connections; ++i) {
        if (! graph.connection_occupied(i)) {
            continue;
        }
        const Sculptor::Connection& connection = graph.get_connection(i);
        if (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx) {
            return i;
        }
    }
    return Sculptor::pool_no_slot;
}

// Index of the zone's layout record with the given key, or -1.
int32_t Sculptor::find_record(const Synth::InstrumentEditorBank& bank,
                              uint32_t                           channel,
                              uint32_t                           zone,
                              uint32_t                           kind,
                              uint32_t                           index,
                              uint8_t                            uid)
{
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == kind && record.index == index &&
            record.uid == uid) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Copies a parameter's live wiring into a kind-3 record's persistence
// fields: served bits from the parameter's value wires, and the
// envelope/LFO descriptor ids plus the LFO source pair from the bound
// instance nodes (0s when an input is unwired).  The LFO sources resolve
// from the live depth/rate edges into the bound instance, so a mid-session
// rewire is what the record stores and the triple matches the instance at
// re-projection. The record's name persists as the user set it and never
// follows the wires. Returns false when a source edge cannot be resolved.
bool Sculptor::store_param_wiring(const Graph&            graph,
                                  const OscGraphMapping&  mapping,
                                  const ParamEntry&       param,
                                  Synth::GraphNodeLayout* record)
{
    record->served      = param.served;
    record->env_desc_id = 0;
    if (param.env_node != Sculptor::pool_no_slot) {
        const int32_t inst = find_instance(mapping, param.env_node);
        if (inst >= 0 && mapping.detached[inst].kind == 1) {
            record->env_desc_id = static_cast<uint8_t>(mapping.detached[inst].desc_id);
        }
    }
    record->lfo_desc_id      = 0;
    record->lfo_depth_source = 0;
    record->lfo_rate_source  = 0;
    if (param.lfo_node != Sculptor::pool_no_slot) {
        const int32_t inst = find_instance(mapping, param.lfo_node);
        if (inst >= 0 && mapping.detached[inst].kind == 2) {
            record->lfo_desc_id     = static_cast<uint8_t>(mapping.detached[inst].desc_id);
            Synth::ModSource source = Synth::ModSource::none;
            const uint32_t   depth_conn =
                connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_depth_input_slot);
            if (depth_conn != Sculptor::pool_no_slot) {
                if (! edge_source(graph, mapping, depth_conn, &source)) {
                    return false;
                }
                record->lfo_depth_source = static_cast<uint8_t>(source);
            }
            const uint32_t rate_conn =
                connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_rate_input_slot);
            if (rate_conn != Sculptor::pool_no_slot) {
                if (! edge_source(graph, mapping, rate_conn, &source)) {
                    return false;
                }
                record->lfo_rate_source = static_cast<uint8_t>(source);
            }
        }
    }
    return true;
}

bool Sculptor::project_instrument_to_graph(const Synth::Instrument&     instrument,
                                           const Synth::InstrumentBank& bank,
                                           Graph*                       graph,
                                           OscGraphMapping*             mapping)
{
    // The graph cannot express a sourceless routing input: dropping it would
    // change synthesis on commit (the eval multiplies by the zero sentinel),
    // so refuse visibly instead of compacting silently.  Checked before any
    // state is touched; the refusal is reported through the error overlay.
    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        if (! is_projected_target(static_cast<Synth::ModTarget>(t))) {
            continue;
        }
        const uint32_t num_inputs = instrument.routing[t].num_inputs <= Synth::max_mod_inputs
                                        ? instrument.routing[t].num_inputs
                                        : Synth::max_mod_inputs;
        for (uint32_t i = 0; i < num_inputs; ++i) {
            if (instrument.routing[t].inputs[i].source == Synth::ModSource::none) {
                graph->set_error("Cannot project: sourceless modulation input");
                return false;
            }
        }
    }
    clear_mapping(mapping);
    mapping->source_bank = &bank;
    graph->clear();

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    const uint32_t input_node = graph->create_node("Inputs", vmath::vec2(0.0f, 0.0f));
    if (input_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->input_node = input_node;
    // Display order groups performance inputs first; the mapping stays
    // indexed by ModSource, so wire semantics are unaffected.
    constexpr Synth::ModSource input_display_order[Sculptor::num_osc_graph_inputs] = {
        Synth::ModSource::velocity,         Synth::ModSource::aftertouch, Synth::ModSource::channel_pressure,
        Synth::ModSource::pressure_combine, Synth::ModSource::pitch_bend, Synth::ModSource::mod_wheel,
    };
    for (uint32_t i = 0; i < num_osc_graph_inputs; ++i) {
        const uint32_t source                   = static_cast<uint32_t>(input_display_order[i]);
        mapping->input_source_slots[source - 1] = graph->add_slot(input_node, output_slot(source_names[source - 1]));
    }
    // Random pitch offset drawn once per note for every layer, in semitones.
    graph->add_slot(input_node, bounded_real_slot("Random Note Detune", instrument.note_skew_semitones, 0.0f, 1.0f));
    // Independent random pitch offset drawn per layer per note, in semitones.
    graph->add_slot(input_node,
                    bounded_real_slot("Random Oscillator Detune", instrument.layer_skew_semitones, 0.0f, 1.0f));

    const uint32_t output_node = graph->create_node("Oscillator Output", vmath::vec2(1024.0f, 0.0f));
    if (output_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->output_node = output_node;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        char name[32];
        snprintf(name, sizeof(name), "Layer %u", layer + 1);
        mapping->output_layer_input_slot[layer] = graph->add_slot(output_node, input_slot(name));
    }

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        project_osc_node(graph, layer, instrument, mapping);
        if (mapping->osc_nodes[layer] == Sculptor::pool_no_slot) {
            return false;
        }
    }

    // Deterministic derivation, then materialization in derivation order.
    ParamGroup     groups[max_param_nodes];
    const uint32_t group_count = derive_param_groups(instrument, groups);
    InstanceKey    keys[2 * max_param_nodes];
    const uint32_t key_count = derive_instance_keys(groups, group_count, keys);

    uint32_t env_seq = 0;
    uint32_t lfo_seq = 0;
    for (uint32_t k = 0; k < key_count; ++k) {
        const InstanceKey& key           = keys[k];
        const bool         is_env        = key.kind == 1;
        const uint32_t     num_allocated = is_env ? bank.envelopes.num_allocated : bank.lfos.num_allocated;
        // A stale descriptor id projects no node; compile drops the binding
        // (the field is dormant - the runtime gates on valid ids too).
        if (key.desc_id == 0 || key.desc_id > num_allocated) {
            continue;
        }
        char     name[32];
        uint32_t ordinal;
        if (is_env) {
            ordinal = ++env_seq;
            snprintf(name, sizeof(name), "Envelope %u", ordinal);
        }
        else {
            ordinal = ++lfo_seq;
            snprintf(name, sizeof(name), "LFO %u", ordinal);
        }
        const float    y = 128.0f * static_cast<float>(ordinal);
        const uint32_t node =
            is_env ? create_env_node(graph, name, vmath::vec2(1024.0f, y), bank.envelopes.entries[key.desc_id - 1])
                   : create_lfo_node(graph, name, vmath::vec2(1536.0f, y), bank.lfos.entries[key.desc_id - 1]);
        if (node == Sculptor::pool_no_slot) {
            return false;
        }
        DetachedNode& entry = mapping->detached[mapping->detached_count++];
        entry.node_idx      = node;
        entry.kind          = key.kind;
        entry.desc_id       = key.desc_id;
        entry.uid           = 0;
        entry.depth_source  = key.depth_source;
        entry.rate_source   = key.rate_source;
    }

    uint32_t param_ordinal = 0;
    for (uint32_t g = 0; g < group_count; ++g) {
        const ParamGroup&          group   = groups[g];
        const float                y       = 128.0f * static_cast<float>(++param_ordinal);
        const Synth::InputRouting& routing = instrument.routing[projected_target_of_index(group.target)];
        const uint32_t node = create_param_node(graph,
                                                param_target_names[group.target],
                                                vmath::vec2(768.0f, y),
                                                static_cast<Synth::ModTarget>(projected_target_of_index(group.target)),
                                                routing.base_value,
                                                group.lfo_op,
                                                group.lfo_depth,
                                                group.lfo_rate_scale_ms,
                                                routing);
        if (node == Sculptor::pool_no_slot) {
            return false;
        }
        ParamEntry& entry = mapping->params[mapping->param_count++];
        entry.node_idx    = node;
        entry.target      = group.target;
        entry.uid         = 0;
        entry.env_node    = Sculptor::pool_no_slot;
        entry.lfo_node    = Sculptor::pool_no_slot;
        entry.served      = group.served;
    }
    name_params(graph, *mapping);

    // Wire derivation.  Each served cell's oscillator value row connects to
    // its group's parameter; the parameter's envelope/LFO inputs connect to
    // the instances matching its tuple (aliases resolve to the first
    // fully-matching instance in derivation order - records attached later
    // disambiguate).
    for (uint32_t g = 0; g < group_count; ++g) {
        const ParamGroup& group = groups[g];
        ParamEntry&       param = mapping->params[g];
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (! (param.served & (1u << layer))) {
                continue;
            }
            graph->add_connection(
                EndPoint{ param.node_idx, mapping->param_output_slot },
                EndPoint{ mapping->osc_nodes[layer],
                          osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(group.target))) });
        }
        if (group.env_desc_id != 0) {
            // Aliased instances match the same descriptor; the first
            // fully-matching instance in derivation order wins - records
            // attached later disambiguate shared instances.
            uint32_t best = Sculptor::pool_no_slot;
            for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 1 &&
                    mapping->detached[i].desc_id == group.env_desc_id) {
                    best = i;
                }
            }
            if (best != Sculptor::pool_no_slot) {
                graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->env_output_slot },
                                      EndPoint{ param.node_idx, param_env_input });
                param.env_node = mapping->detached[best].node_idx;
            }
        }
        if (group.lfo_desc_id != 0) {
            uint32_t best = Sculptor::pool_no_slot;
            for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 2 &&
                    mapping->detached[i].desc_id == group.lfo_desc_id &&
                    mapping->detached[i].depth_source == group.depth_source &&
                    mapping->detached[i].rate_source == group.rate_source) {
                    best = i;
                }
            }
            if (best != Sculptor::pool_no_slot) {
                graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->lfo_output_slot },
                                      EndPoint{ param.node_idx, param_lfo_input });
                param.lfo_node = mapping->detached[best].node_idx;
            }
        }
    }

    // Source rows are shared views: every parameter of a target draws the
    // same MIDI-source wires, derived from routing.inputs (never read
    // destructively).
    for (uint32_t proj = 0; proj < 5; ++proj) {
        const Synth::ModTarget     target  = static_cast<Synth::ModTarget>(projected_target_of_index(proj));
        const Synth::InputRouting& routing = instrument.routing[target];
        for (uint32_t p = 0; p < mapping->param_count; ++p) {
            if (mapping->params[p].target != proj) {
                continue;
            }
            wire_param_sources(graph, *mapping, mapping->params[p].node_idx, routing);
        }
    }

    // The LFO instances' depth/rate inputs keep their MIDI-source wires.
    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const DetachedNode& entry = mapping->detached[i];
        if (entry.kind != 2) {
            continue;
        }
        if (entry.depth_source != 0) {
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.depth_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_depth_input_slot });
        }
        if (entry.rate_source != 0) {
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.rate_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_rate_input_slot });
        }
    }

    // Structural hard connections: every oscillator feeds its sum input.
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        graph->add_connection(EndPoint{ mapping->osc_nodes[layer], mapping->osc_output_slot },
                              EndPoint{ mapping->output_node, mapping->output_layer_input_slot[layer] });
    }
    return true;
}

bool Sculptor::compile_graph_to_instrument(const Graph& graph, const OscGraphMapping& mapping, Synth::Instrument* out)
{
    *out = Synth::Instrument{};

    uint32_t layer_count = 0;
    while (layer_count < Synth::max_layers && mapping.osc_nodes[layer_count] != pool_no_slot) {
        ++layer_count;
    }
    for (uint32_t layer = layer_count; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != pool_no_slot) {
            return false; // gap in the oscillator node run
        }
    }
    if (mapping.output_node == pool_no_slot) {
        return false;
    }
    out->layer_count = layer_count;

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        const Node& node = graph.node(mapping.osc_nodes[layer]);
        if (node.slots.num_allocated < osc_slot_count) {
            return false;
        }
        const Slot*        slots = node.slots.entries;
        Synth::Oscillator& osc   = out->layers[layer];

        uint32_t index = 0;
        if (! read_list_prop(slots[osc_waveform_a_prop], &index)) {
            return false;
        }
        osc.osc_type[0] = static_cast<Synth::WaveType>(index + 1);
        if (! read_list_prop(slots[osc_waveform_b_prop], &index)) {
            return false;
        }
        osc.osc_type[1] = static_cast<Synth::WaveType>(index);
        if (! read_list_prop(slots[osc_mode_prop], &index)) {
            return false;
        }
        osc.osc_mode     = static_cast<Synth::OscMode>(index);
        osc.mod_ratio    = slots[osc_fm_ratio_prop].value.real;
        osc.pitch_offset = slots[osc_pitch_offset_prop].value.real;

        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            if (! is_projected_target(target)) {
                continue; // generator fields on unprojected targets compile to zero
            }
            // An oscillator input with no parameter wire is a constant: the
            // whole per-cell binding resets.  A wire binds the parameter's
            // CURRENT inputs - envelope/LFO descriptor ids come from the
            // wired instance nodes, so rewiring rebinds without re-projection.
            Synth::LayerGen gen  = {};
            const uint32_t  conn = connection_into(graph, mapping.osc_nodes[layer], osc_target_prop(target));
            if (conn != pool_no_slot) {
                const int32_t p = find_param(mapping, graph.get_connection(conn).output.node_idx);
                if (p < 0) {
                    return false;
                }
                const ParamEntry& param = mapping.params[p];
                if (param.node_idx == pool_no_slot || ! graph.node_occupied(param.node_idx)) {
                    continue; // the parameter node is gone (deleted in the same drained batch): the cell compiles
                              // unbound
                }

                const uint32_t env_conn = connection_into(graph, param.node_idx, param_env_input);
                if (env_conn != pool_no_slot) {
                    const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
                    if (inst < 0 || mapping.detached[inst].kind != 1) {
                        return false;
                    }
                    gen.envelope_desc_id = mapping.detached[inst].desc_id;
                }
                const uint32_t lfo_conn = connection_into(graph, param.node_idx, param_lfo_input);
                if (lfo_conn != pool_no_slot) {
                    const int32_t inst = find_instance(mapping, graph.get_connection(lfo_conn).output.node_idx);
                    if (inst < 0 || mapping.detached[inst].kind != 2) {
                        return false;
                    }
                    gen.lfo_desc_id = mapping.detached[inst].desc_id;
                    // Depth/rate sources resolve from the edges into the bound
                    // LFO instance; no edge means the source stays none.
                    const uint32_t depth_conn =
                        connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_depth_input_slot);
                    if (depth_conn != pool_no_slot &&
                        ! edge_source(graph, mapping, depth_conn, &gen.lfo_depth_source)) {
                        return false;
                    }
                    const uint32_t rate_conn =
                        connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_rate_input_slot);
                    if (rate_conn != pool_no_slot && ! edge_source(graph, mapping, rate_conn, &gen.lfo_rate_source)) {
                        return false;
                    }
                }
                if (! read_list_prop(graph.node(param.node_idx).slots.entries[param_lfo_op_prop], &index)) {
                    return false;
                }
                gen.lfo_op            = static_cast<Synth::SourceOp>(index);
                gen.lfo_depth         = graph.node(param.node_idx).slots.entries[param_lfo_depth_prop].value.real;
                gen.lfo_rate_scale_ms = graph.node(param.node_idx).slots.entries[param_rate_scale_prop].value.real;
            }
            osc.gen[t] = gen;
        }
    }

    // MIDI routing is shared by all layers: the source wires and op/scale
    // rows come from the target's first serving parameter, and a connected
    // dynamic value row is edited through its parameter (the connected
    // oscillator row is a greyed view), so the base value reads the serving
    // parameter when one exists and the first oscillator's row otherwise.
    if (layer_count > 0) {
        const Node& first = graph.node(mapping.osc_nodes[0]);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            if (first.slots.num_allocated <= osc_target_prop(target)) {
                return false;
            }
            out->routing[t].base_value = first.slots.entries[osc_target_prop(target)].value.real;
            if (target == Synth::mod_fm_index) {
                out->routing[t].base_value *= osc_target_views[static_cast<uint32_t>(Synth::mod_fm_index)].bank_scale;
            }
            if (! is_projected_target(target)) {
                continue;
            }
            int32_t carrier = -1;
            for (uint32_t p = 0; p < mapping.param_count; ++p) {
                // Only a parameter that value-wires the target carries the
                // routing: an inert detached parameter (served == 0) must not
                // keep MIDI inputs alive, or a disconnected value wire would
                // resurrect them on the next re-projection.
                if (mapping.params[p].target == projected_index(target) && mapping.params[p].served != 0 &&
                    mapping.params[p].node_idx != pool_no_slot && graph.node_occupied(mapping.params[p].node_idx)) {
                    carrier = static_cast<int32_t>(p);
                    break;
                }
            }
            if (carrier < 0) {
                // No serving parameter of this target: the MIDI routing has no
                // view left, so its inputs compile to zero (the base value
                // survives on the shared oscillator rows).
                continue;
            }
            const ParamEntry& param    = mapping.params[carrier];
            out->routing[t].base_value = graph.node(param.node_idx).slots.entries[param_value_prop].value.real;
            uint32_t num_inputs        = 0;
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                const uint32_t conn = connection_into(graph, param.node_idx, param_src_input(i));
                if (conn == pool_no_slot) {
                    continue;
                }
                Synth::ModSource source = Synth::ModSource::none;
                if (! edge_source(graph, mapping, conn, &source)) {
                    return false;
                }
                const Node& param_node = graph.node(param.node_idx);
                if (param_node.slots.num_allocated <= param_src_op_prop(i)) {
                    return false;
                }
                uint32_t op_index = 0;
                if (! read_list_prop(param_node.slots.entries[param_src_op_prop(i)], &op_index)) {
                    return false;
                }
                Synth::ModInput& input = out->routing[t].inputs[num_inputs++];
                input.source           = source;
                input.op               = static_cast<Synth::SourceOp>(op_index);
                input.scale            = param_node.slots.entries[param_src_scale_prop(i)].value.real;
            }
            out->routing[t].num_inputs = static_cast<uint16_t>(num_inputs);
        }
    }

    const Node& inputs = graph.node(mapping.input_node);
    if (inputs.slots.num_allocated < num_osc_graph_inputs + 2) {
        return false;
    }
    out->note_skew_semitones  = inputs.slots.entries[input_note_detune_prop].value.real;
    out->layer_skew_semitones = inputs.slots.entries[input_osc_detune_prop].value.real;

    return true;
}

bool Sculptor::osc_graph_validate(void* user_data, Graph& graph, EndPoint output, EndPoint input)
{
    const OscGraphMapping* mapping = static_cast<const OscGraphMapping*>(user_data);
    if (! mapping) {
        return false;
    }

    switch (classify_output(*mapping, output)) {
        case out_param: {
            // A parameter wire into its own target's oscillator row is the
            // ordinary case. A wire onto a DIFFERENT target's row retargets
            // the parameter (the wire decides the target): allowed only for
            // record-backed parameters whose destination has no record-less
            // derived parameter the re-keyed record would positionally
            // hijack.
            const int32_t p = find_param(*mapping, output.node_idx);
            if (p < 0) {
                return false;
            }
            if (classify_input(*mapping, input) != in_target) {
                return false;
            }
            const int32_t target = osc_projected_row_index(input.slot_idx);
            if (target < 0) {
                return false;
            }
            if (static_cast<uint32_t>(target) == mapping->params[p].target) {
                return true;
            }
            if (mapping->params[p].uid == 0 ||
                param_target_has_recordless_derived(*mapping, static_cast<uint32_t>(target))) {
                return false;
            }
            // The attached envelope moves with the retarget and must respect
            // the volume-envelope constraint at its new target.
            const uint32_t env_conn = connection_into(graph, mapping->params[p].node_idx, param_env_input);
            if (env_conn == pool_no_slot) {
                return true;
            }
            const int32_t env_inst = find_instance(*mapping, graph.get_connection(env_conn).output.node_idx);
            if (env_inst < 0 || mapping->detached[env_inst].kind != 1) {
                return true;
            }
            bool volume_used = false;
            bool other_used  = false;
            env_target_usage(graph, *mapping, mapping->detached[env_inst].desc_id, &volume_used, &other_used, p);
            if (static_cast<uint32_t>(target) == projected_index(Synth::mod_volume) && other_used) {
                graph.set_error("Envelope already serves a non-volume target");
                return false;
            }
            if (static_cast<uint32_t>(target) != projected_index(Synth::mod_volume) && volume_used) {
                graph.set_error("Envelope already serves the volume target");
                return false;
            }
            return true;
        }
        case out_env: {
            if (classify_input(*mapping, input) != in_penv) {
                return false;
            }
            // A volume envelope (minimum 0, first and last point 0) serves
            // only the volume target, and only a volume envelope serves the
            // volume target: the descriptor is shared by every target that
            // wires it, and the two roles want different ranges.
            const int32_t p = find_param(*mapping, input.node_idx);
            if (p < 0) {
                return false;
            }
            const int32_t inst = find_instance(*mapping, output.node_idx);
            if (inst < 0 || mapping->detached[inst].kind != 1) {
                return false;
            }
            bool volume_used = false;
            bool other_used  = false;
            env_target_usage(graph,
                             *mapping,
                             mapping->detached[inst].desc_id,
                             &volume_used,
                             &other_used,
                             -1,
                             graph.moving_connection);
            if (mapping->params[p].target == projected_index(Synth::mod_volume) && other_used) {
                graph.set_error("Envelope already serves a non-volume target");
                return false;
            }
            if (mapping->params[p].target != projected_index(Synth::mod_volume) && volume_used) {
                graph.set_error("Envelope already serves the volume target");
                return false;
            }
            return true;
        }
        case out_lfo:
            return classify_input(*mapping, input) == in_plfo;
        case out_input:
            switch (classify_input(*mapping, input)) {
                case in_psrc:
                    // Source rows are order-indexed: the add path refuses
                    // source 1 before source 0 (checked at wire time), but
                    // deleting or retargeting away the source-0 wire while
                    // source-1 stays wired leaves a transient state that
                    // compacts sound-identically into inputs[0]; the next
                    // re-projection restores the row identity.  (Accepted:
                    // cascade-drop was rejected because it would silently
                    // destroy user wiring.)
                    if (input.slot_idx == param_src_input(1) &&
                        connection_into(graph, input.node_idx, param_src_input(0)) == Sculptor::pool_no_slot) {
                        graph.set_error("Wire source 0 first");
                        return false;
                    }
                    return true;
                case in_depth:
                case in_rate:
                    return true;
                default:
                    return false;
            }
        case out_osc: {
            if (classify_input(*mapping, input) != in_sum) {
                return false;
            }
            // The hard connection is layer-matched: an oscillator feeds its own
            // sum input, both resolved through the mapping.
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (mapping->osc_nodes[layer] != output.node_idx) {
                    continue;
                }
                for (uint32_t sum_layer = 0; sum_layer < Synth::max_layers; ++sum_layer) {
                    if (mapping->output_layer_input_slot[sum_layer] == input.slot_idx) {
                        return layer == sum_layer;
                    }
                }
                return false;
            }
            return false;
        }
        default:
            return false;
    }
}

bool Sculptor::apply_osc_graph_descriptor_edit(const Graph&           graph,
                                               const OscGraphMapping& mapping,
                                               Synth::InstrumentBank* bank,
                                               uint32_t               node_idx,
                                               uint32_t               slot_idx,
                                               PropertyValue          value)
{
    // Generator instances resolve through the registry: their content edits
    // write through to the pool descriptor by id, aliasing included.
    const int32_t inst = find_instance(mapping, node_idx);
    if (inst < 0) {
        return false;
    }
    const uint16_t desc_id = mapping.detached[inst].desc_id;
    const bool     is_env  = mapping.detached[inst].kind == 1;
    if (desc_id == 0) {
        return false;
    }

    const Node& node = graph.node(node_idx);
    if (slot_idx >= node.slots.num_allocated) {
        return false;
    }
    const Slot& slot = node.slots.entries[slot_idx];
    if (slot.kind != SlotKind::property) {
        return false;
    }

    if (is_env) {
        if (desc_id > bank->envelopes.num_allocated) {
            return false;
        }
        Synth::EnvelopeDescriptor& env = bank->envelopes.entries[desc_id - 1];
        if (slot.property_type == PropertyType::real) {
            // The runtime evaluates the curve as min_value + raw_value *
            // min_max_delta with raw_value in 0..65535, so the slots expose
            // the effective range endpoints: the top re-derives the span
            // (max - min) / 65535, and moving the floor pins the top.
            if (strncmp(slot.name, "Value (min)", sizeof(slot.name)) == 0) {
                // A volume envelope keeps its minimum at 0; the descriptor is
                // shared, so the constraint follows its volume wiring.  The
                // refusing caller reports the refused batch.
                bool volume_used = false;
                bool other_used  = false;
                env_target_usage(graph, mapping, desc_id, &volume_used, &other_used);
                if (volume_used && value.real != 0.0f) {
                    return false;
                }
                const float max_value = env.min_value + 65535.0f * env.min_max_delta;
                env.min_value         = value.real;
                env.min_max_delta     = (max_value - env.min_value) / 65535.0f;
                return true;
            }
            if (strncmp(slot.name, "Value (max)", sizeof(slot.name)) == 0) {
                // A volume envelope keeps its effective top at or above its
                // minimum; the descriptor is shared, so the constraint follows
                // its volume wiring.
                bool volume_used = false;
                bool other_used  = false;
                env_target_usage(graph, mapping, desc_id, &volume_used, &other_used);
                if (volume_used && value.real < env.min_value) {
                    return false;
                }
                env.min_max_delta = (value.real - env.min_value) / 65535.0f;
                return true;
            }
            return false;
        }
        return false;
    }

    if (desc_id > bank->lfos.num_allocated) {
        return false;
    }
    Synth::LFODescriptor& lfo = bank->lfos.entries[desc_id - 1];
    if (slot.property_type == PropertyType::real) {
        return false;
    }
    if (slot.property_type == PropertyType::list) {
        if (strncmp(slot.name, "Waveform", sizeof(slot.name)) != 0 || value.list_index >= slot.num_list_options) {
            return false;
        }
        lfo.wave = static_cast<Synth::WaveType>(value.list_index);
        return true;
    }
    if (slot.property_type == PropertyType::integer && value.integer >= 0) {
        if (strncmp(slot.name, "Duty", sizeof(slot.name)) == 0 && value.real >= 0.0f && value.real <= 1.0f) {
            lfo.duty = static_cast<uint8_t>(value.real * 255.0f + 0.5f);
            return true;
        }
        if (strncmp(slot.name, "Period (ms)", sizeof(slot.name)) == 0 && value.integer <= 65535) {
            lfo.period_ms = static_cast<uint16_t>(value.integer);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Editor-side state helpers (sparse layout records, missing-sum masks,
// undo group tags) and the editor projection / change-apply paths.
// ---------------------------------------------------------------------------

namespace {

// The editor-facing helpers exported from sculptor_osc_graph.h are used
// unqualified in this file's local sections.
using Sculptor::connection_into;
using Sculptor::find_param;
using Sculptor::find_record;
using Sculptor::param_src_input;
using Sculptor::param_target_names;

// Canonical kind-0 numbering is shared with the JSON codec; it lives in
// sculptor_instr_bank.h.  Only the fixed nodes (inputs, sum, oscillators)
// carry canonical indices; parameters and generator instances are
// record-keyed.
constexpr uint32_t canonical_first_osc  = Synth::graph_canonical_first_osc;
constexpr uint32_t canonical_first_env  = Synth::graph_canonical_first_env;
constexpr uint32_t canonical_node_count = Synth::graph_canonical_node_count;

// The projected node a kind-0 record keys, or pool_no_slot when the current
// projection has no such node (shorter layer run).
uint32_t node_at_canonical_index(const Sculptor::OscGraphMapping& mapping, uint32_t index)
{
    if (index == 0) {
        return mapping.input_node;
    }
    if (index == 1) {
        return mapping.output_node;
    }
    if (index < canonical_first_env) {
        return mapping.osc_nodes[index - canonical_first_osc];
    }
    return Sculptor::pool_no_slot;
}

void remove_record_at(Synth::InstrumentEditorBank* bank, uint32_t idx)
{
    for (uint32_t i = idx; i + 1 < bank->graph_layout_count; ++i) {
        bank->graph_layout[i] = bank->graph_layout[i + 1];
    }
    bank->graph_layout_count--;
}

void append_record(Synth::InstrumentEditorBank* bank, const Synth::GraphNodeLayout& record)
{
    bank->graph_layout[bank->graph_layout_count++] = record;
}

// Materializes a kind-3 record's persisted partial wiring: value wires to
// the served layers the generator tuple does not derive (a wired zero-tuple
// cell compiles to a zero binding - the wire is editor state, invisible to
// synthesis), and the envelope/LFO instance wires the record references.
// An occupied row or input keeps its live wire (another parameter's, or the
// derived binding's own); a stale descriptor reference leaves the input
// unwired.  Aliased instances resolve to the first fully-matching instance
// in derivation order, the same rule the derivation itself uses.
void wire_record_state(Sculptor::Graph*              graph,
                       Sculptor::OscGraphMapping*    mapping,
                       const Synth::GraphNodeLayout& record,
                       Sculptor::ParamEntry*         param)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (! (record.served & (1u << layer)) || mapping->osc_nodes[layer] == Sculptor::pool_no_slot ||
            ! graph->node_occupied(mapping->osc_nodes[layer])) {
            continue;
        }
        const uint32_t row = osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(param->target)));
        if (connection_into(*graph, mapping->osc_nodes[layer], row) != Sculptor::pool_no_slot) {
            continue;
        }
        graph->add_connection(Sculptor::EndPoint{ param->node_idx, mapping->param_output_slot },
                              Sculptor::EndPoint{ mapping->osc_nodes[layer], row });
    }
    if (record.env_desc_id != 0 &&
        connection_into(*graph, param->node_idx, param_env_input) == Sculptor::pool_no_slot) {
        for (uint32_t i = 0; i < mapping->detached_count; ++i) {
            const Sculptor::DetachedNode& entry = mapping->detached[i];
            if (entry.kind == 1 && entry.desc_id == record.env_desc_id) {
                graph->add_connection(Sculptor::EndPoint{ entry.node_idx, mapping->env_output_slot },
                                      Sculptor::EndPoint{ param->node_idx, param_env_input });
                param->env_node = entry.node_idx;
                break;
            }
        }
    }
    if (record.lfo_desc_id != 0 &&
        connection_into(*graph, param->node_idx, param_lfo_input) == Sculptor::pool_no_slot) {
        for (uint32_t i = 0; i < mapping->detached_count; ++i) {
            const Sculptor::DetachedNode& entry = mapping->detached[i];
            if (entry.kind == 2 && entry.desc_id == record.lfo_desc_id &&
                entry.depth_source == record.lfo_depth_source && entry.rate_source == record.lfo_rate_source) {
                graph->add_connection(Sculptor::EndPoint{ entry.node_idx, mapping->lfo_output_slot },
                                      Sculptor::EndPoint{ param->node_idx, param_lfo_input });
                param->lfo_node = entry.node_idx;
                break;
            }
        }
    }
}

bool detach_param(Synth::InstrumentEditorBank* bank,
                  const Sculptor::Graph&       graph,
                  Sculptor::OscGraphMapping*   mapping,
                  uint32_t                     channel,
                  uint32_t                     zone,
                  uint32_t                     param_idx)
{
    if (bank->graph_layout_count >= Synth::max_graph_records) {
        return false;
    }
    Sculptor::ParamEntry&  param  = mapping->params[param_idx];
    Synth::GraphNodeLayout record = {};
    record.channel                = static_cast<uint8_t>(channel);
    record.zone                   = static_cast<uint8_t>(zone);
    record.kind                   = 3;
    record.index                  = param.target;
    // A parameter detached from its last value wire has no group left:
    // its record is born free-standing and re-claims its own wire-created
    // group (or any unclaimed same-target group) through the positional
    // pass. Rename/move-created records keep a live group, so they stamp
    // the truthful ordinal.
    record.param_slot = param.served != 0 ? param_group_ordinal(*mapping, param_idx) : Synth::graph_record_param_free;
    const Sculptor::Node& node = graph.node(param.node_idx);
    record.x                   = node.position.x;
    record.y                   = node.position.y;
    record.width_override      = node.content_width_override;
    record.height_override     = node.content_height_override;
    if (! Sculptor::store_param_wiring(graph, *mapping, param, &record)) {
        return false;
    }
    record.uid = Sculptor::allocate_detached_uid(*bank, channel, zone, 3);
    append_record(bank, record);
    param.uid = record.uid;
    return true;
}

// Re-derives binding state from the live edges: parameters pick up their
// bound instances and served cells, free-standing state materializes into
// records, and LFO records follow the live depth/rate edges.
// The live binding tuple of a parameter: what compile_graph_to_instrument
// would write into the served cells' gen fields from the current graph
// state. Mirrors the compile's resolution exactly - descriptor ids from
// the wired instance nodes, depth/rate sources from the edges into the
// bound LFO, op/depth/rate scale from the parameter's own slots - so the
// re-stamp can dedupe parameters the way derive_param_groups merges their
// cells. Returns false for the same broken states the compile rejects.
bool live_param_tuple(const Sculptor::Graph&           graph,
                      const Sculptor::OscGraphMapping& mapping,
                      const Sculptor::ParamEntry&      param,
                      ParamGroup*                      tuple)
{
    *tuple                  = ParamGroup{};
    tuple->target           = param.target;
    const uint32_t env_conn = connection_into(graph, param.node_idx, param_env_input);
    if (env_conn != pool_no_slot) {
        const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
        if (inst < 0 || mapping.detached[inst].kind != 1) {
            return false;
        }
        tuple->env_desc_id = mapping.detached[inst].desc_id;
    }
    const uint32_t lfo_conn = connection_into(graph, param.node_idx, param_lfo_input);
    if (lfo_conn != pool_no_slot) {
        const int32_t inst = find_instance(mapping, graph.get_connection(lfo_conn).output.node_idx);
        if (inst < 0 || mapping.detached[inst].kind != 2) {
            return false;
        }
        tuple->lfo_desc_id      = mapping.detached[inst].desc_id;
        Synth::ModSource source = Synth::ModSource::none;
        const uint32_t   depth_conn =
            connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_depth_input_slot);
        if (depth_conn != pool_no_slot) {
            if (! edge_source(graph, mapping, depth_conn, &source)) {
                return false;
            }
            tuple->depth_source = static_cast<uint8_t>(source);
        }
        const uint32_t rate_conn = connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_rate_input_slot);
        if (rate_conn != pool_no_slot) {
            if (! edge_source(graph, mapping, rate_conn, &source)) {
                return false;
            }
            tuple->rate_source = static_cast<uint8_t>(source);
        }
    }
    uint32_t index = 0;
    if (! read_list_prop(graph.node(param.node_idx).slots.entries[param_lfo_op_prop], &index)) {
        return false;
    }
    tuple->lfo_op            = static_cast<uint8_t>(index);
    tuple->lfo_depth         = graph.node(param.node_idx).slots.entries[param_lfo_depth_prop].value.real;
    tuple->lfo_rate_scale_ms = graph.node(param.node_idx).slots.entries[param_rate_scale_prop].value.real;
    return true;
}

bool same_live_tuple(const ParamGroup& a, const ParamGroup& b)
{
    return a.env_desc_id == b.env_desc_id && a.lfo_desc_id == b.lfo_desc_id && a.lfo_op == b.lfo_op &&
           a.lfo_depth == b.lfo_depth && a.lfo_rate_scale_ms == b.lfo_rate_scale_ms &&
           a.depth_source == b.depth_source && a.rate_source == b.rate_source;
}

// A parameter outside the derived order (dormant, or merged into a
// tuple-identical sibling) must not keep an explicit ordinal: it would
// name a group that never derives and trip the wholesale explicit-distrust
// fallback at the next projection.
void demote_explicit_param_ordinal(Synth::InstrumentEditorBank* bank,
                                   uint32_t                     channel,
                                   uint32_t                     zone,
                                   uint32_t                     target,
                                   uint32_t                     uid)
{
    if (uid == 0) {
        return;
    }
    const int32_t record_idx = find_record(*bank, channel, zone, 3, target, static_cast<uint8_t>(uid));
    if (record_idx < 0) {
        return;
    }
    Synth::GraphNodeLayout& record = bank->graph_layout[record_idx];
    if (record.param_slot != 0 && record.param_slot != Synth::graph_record_param_free) {
        record.param_slot = Synth::graph_record_param_free;
    }
}

bool reconcile_bindings(Synth::InstrumentEditorBank* bank,
                        Sculptor::Graph*             graph,
                        Sculptor::OscGraphMapping*   mapping,
                        uint32_t                     channel,
                        uint32_t                     zone)
{
    for (uint32_t p = 0; p < mapping->param_count; ++p) {
        Sculptor::ParamEntry& param = mapping->params[p];
        if (param.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(param.node_idx)) {
            continue;
        }
        param.env_node          = Sculptor::pool_no_slot;
        param.lfo_node          = Sculptor::pool_no_slot;
        const uint32_t env_conn = connection_into(*graph, param.node_idx, param_env_input);
        if (env_conn != Sculptor::pool_no_slot) {
            param.env_node = graph->get_connection(env_conn).output.node_idx;
        }
        const uint32_t lfo_conn = connection_into(*graph, param.node_idx, param_lfo_input);
        if (lfo_conn != Sculptor::pool_no_slot) {
            param.lfo_node = graph->get_connection(lfo_conn).output.node_idx;
        }
        param.served = 0;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping->osc_nodes[layer] == Sculptor::pool_no_slot) {
                continue;
            }
            const uint32_t conn = connection_into(
                *graph,
                mapping->osc_nodes[layer],
                osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(param.target))));
            if (conn != Sculptor::pool_no_slot && graph->get_connection(conn).output.node_idx == param.node_idx) {
                param.served |= static_cast<uint8_t>(1u << layer);
            }
        }
        if (param.served == 0) {
            // The parameter lost its last value wire: its derived group dies
            // with the next compile. The dying record's own explicit slot
            // becomes free-standing; survivor ordinals are corrected by the
            // re-stamp pass below, which mirrors the derived order after
            // every reconcile no matter how the order changed.
            if (param.uid != 0) {
                const int32_t dying = find_record(*bank, channel, zone, 3, param.target, param.uid);
                if (dying >= 0) {
                    Synth::GraphNodeLayout& dying_record = bank->graph_layout[dying];
                    if (dying_record.param_slot != 0 && dying_record.param_slot != Synth::graph_record_param_free) {
                        dying_record.param_slot = Synth::graph_record_param_free;
                    }
                }
            }
            else if (! detach_param(bank, *graph, mapping, channel, zone, p)) {
                return false;
            }
        }
        // Partial-wiring persistence: the record follows the live wires
        // eventlessly (metadata writes never enter the change queue), so a
        // re-projection rebuilds the wiring the compiled instrument cannot
        // express.
        if (param.uid != 0) {
            const int32_t record_idx = find_record(*bank, channel, zone, 3, param.target, param.uid);
            if (record_idx >= 0 &&
                ! Sculptor::store_param_wiring(*graph, *mapping, param, &bank->graph_layout[record_idx])) {
                return false;
            }
        }
    }
    // Ordinal truthfulness: explicit record ordinals mirror the derived
    // group order (groups enumerate in first-served-layer order), so the
    // projection's explicit pass pairs each record with its own group.
    // Re-stamping runs after every re-derivation because graph edits
    // change that order without a matching bookkeeping event. The counted
    // population mirrors derive_param_groups exactly: a
    // parameter occupies a group when its generator tuple is wired, and a
    // target with MIDI routing inputs but no tuple-bearing parameter keeps
    // its single zero-tuple carrier group; tuple-less parameters outside
    // those cases are dormant (their value wires compile to zero bindings)
    // and their explicit ordinals demote to free-standing.
    for (uint32_t proj = 0; proj < 5; ++proj) {
        const Synth::Zone&         zone_entry = bank->bank.channel_zones[channel][zone];
        const Synth::InputRouting& carrier_routing =
            zone_entry.start_note != 0 && zone_entry.instrument < bank->bank.instruments.num_allocated
                ? bank->bank.instruments.entries[zone_entry.instrument].routing[projected_target_of_index(proj)]
                : Synth::InputRouting{};
        const uint32_t routing_inputs =
            carrier_routing.num_inputs <= Synth::max_mod_inputs ? carrier_routing.num_inputs : Synth::max_mod_inputs;
        bool       tuple_param = false;
        ParamGroup seen[Sculptor::max_param_nodes];
        for (uint32_t q = 0; q < mapping->param_count; ++q) {
            const Sculptor::ParamEntry& scanned = mapping->params[q];
            if (scanned.target == proj && scanned.served != 0 && scanned.node_idx != Sculptor::pool_no_slot &&
                graph->node_occupied(scanned.node_idx) &&
                (scanned.env_node != Sculptor::pool_no_slot || scanned.lfo_node != Sculptor::pool_no_slot)) {
                tuple_param = true;
            }
        }
        const bool carrier       = ! tuple_param && routing_inputs > 0;
        bool       carrier_taken = false;
        uint32_t   seen_count    = 0;
        uint32_t   position      = 0;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            for (uint32_t q = 0; q < mapping->param_count; ++q) {
                const Sculptor::ParamEntry& ordered = mapping->params[q];
                if (ordered.target != proj || ordered.served == 0 || ordered.node_idx == Sculptor::pool_no_slot ||
                    ! graph->node_occupied(ordered.node_idx)) {
                    continue;
                }
                uint32_t first = 0;
                while ((ordered.served & (1u << first)) == 0) {
                    ++first;
                }
                if (first != layer) {
                    continue;
                }
                ParamGroup tuple;
                if (! live_param_tuple(*graph, *mapping, ordered, &tuple)) {
                    return false;
                }
                const bool tuple_wired = tuple.env_desc_id != 0 || tuple.lfo_desc_id != 0;
                if (! tuple_wired && (! carrier || carrier_taken)) {
                    // Dormant parameter: value-wired but compiling to a zero
                    // binding and outside the derived order. An explicit
                    // ordinal here would name a group that never derives, so
                    // it demotes to free-standing; the positional pass
                    // restricts tuple-less records to carrier groups, so a
                    // dormant record can only materialize its own inert node.
                    demote_explicit_param_ordinal(bank, channel, zone, proj, ordered.uid);
                    continue;
                }
                if (! tuple_wired) {
                    carrier_taken = true;
                }
                else {
                    // Cells of one target merge into one derived group when
                    // their binding tuples are identical, so a parameter whose
                    // tuple equals an already-counted one shares that group:
                    // it occupies no further position, and its explicit
                    // ordinal would name a group that never derives separately.
                    bool merged = false;
                    for (uint32_t s = 0; s < seen_count; ++s) {
                        if (same_live_tuple(seen[s], tuple)) {
                            merged = true;
                            break;
                        }
                    }
                    if (merged) {
                        demote_explicit_param_ordinal(bank, channel, zone, proj, ordered.uid);
                        continue;
                    }
                    seen[seen_count++] = tuple;
                }
                // A serving parameter occupies a group even without a record.
                ++position;
                const int32_t record_idx =
                    find_record(*bank, channel, zone, 3, proj, static_cast<uint8_t>(ordered.uid));
                if (record_idx < 0) {
                    continue;
                }
                Synth::GraphNodeLayout& record = bank->graph_layout[record_idx];
                if (record.param_slot != 0 && record.param_slot != Synth::graph_record_param_free) {
                    record.param_slot = static_cast<uint8_t>(position);
                }
            }
        }
    }

    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const Sculptor::DetachedNode& entry = mapping->detached[i];
        if (entry.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(entry.node_idx)) {
            continue;
        }
        bool referenced = false;
        for (uint32_t p = 0; p < mapping->param_count && ! referenced; ++p) {
            const Sculptor::ParamEntry& param = mapping->params[p];
            if (param.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(param.node_idx)) {
                continue;
            }
            referenced = param.env_node == entry.node_idx || param.lfo_node == entry.node_idx;
        }
        if (! referenced && entry.uid == 0 &&
            ! Sculptor::detach_osc_graph_instance(bank, *graph, mapping, channel, zone, i)) {
            return false;
        }
        if (entry.kind == 2 && entry.uid != 0) {
            // Free-standing LFO source records follow the live depth/rate
            // edges, so edits while free-standing survive re-projection.
            const int32_t record_idx = find_record(*bank, channel, zone, 2, entry.desc_id, entry.uid);
            if (record_idx < 0) {
                continue;
            }
            Synth::GraphNodeLayout& record     = bank->graph_layout[record_idx];
            const uint32_t          depth_conn = connection_into(*graph, entry.node_idx, mapping->lfo_depth_input_slot);
            const uint32_t          rate_conn  = connection_into(*graph, entry.node_idx, mapping->lfo_rate_input_slot);
            Synth::ModSource        source     = Synth::ModSource::none;
            record.depth_source                = 0;
            record.rate_source                 = 0;
            if (depth_conn != Sculptor::pool_no_slot) {
                if (! edge_source(*graph, *mapping, depth_conn, &source)) {
                    return false;
                }
                record.depth_source = static_cast<uint8_t>(source);
            }
            if (rate_conn != Sculptor::pool_no_slot) {
                if (! edge_source(*graph, *mapping, rate_conn, &source)) {
                    return false;
                }
                record.rate_source = static_cast<uint8_t>(source);
            }
        }
    }
    return true;
}

// True while a mapped oscillator node is freed: its layer removal is still
// pending in this drained batch (a node's wire events always drain before
// its own node_deleted), so a compile now would read the freed node's stale
// slots.  The batch's last oscillator node_deleted finds only live nodes and
// produces the final compile.
bool osc_deletion_pending(const Sculptor::Graph& graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != Sculptor::pool_no_slot && ! graph.node_occupied(mapping.osc_nodes[layer])) {
            return true;
        }
    }
    return false;
}

// Rewrites the zone's instrument from the graph (canonical form: everything
// the graph does not express compiles to zero).  MIDI routing inputs exist
// only while some parameter of the target value-wires it: compile zeroes the
// inputs of a target with no serving parameter, and the pure-MIDI-carrier
// materialization therefore only serves banks loaded from file.
bool store_compiled_instrument(Synth::InstrumentEditorBank*     bank,
                               const Sculptor::Graph&           graph,
                               const Sculptor::OscGraphMapping& mapping,
                               uint32_t                         channel,
                               uint32_t                         zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return false;
    }
    if (osc_deletion_pending(graph, mapping)) {
        return true; // the pending node_deleted recompiles after the removal
    }
    Synth::Instrument compiled;
    if (! compile_graph_to_instrument(graph, mapping, &compiled)) {
        return false;
    }
    bank->bank.instruments.entries[zone_entry.instrument] = compiled;
    return true;
}

// Instrument-wide shared fields: the nine per-target value rows on the
// oscillators, the value row of every parameter, and the source op/scale
// rows (shared per target).  Per-layer fields (waveforms, mode, ratio, pitch
// offset) and per-binding LFO rows stay on their own node.
bool is_shared_param_slot(uint32_t slot_idx)
{
    return slot_idx == param_value_prop || slot_idx == param_src_op_prop(0) || slot_idx == param_src_scale_prop(0) ||
           slot_idx == param_src_op_prop(1) || slot_idx == param_src_scale_prop(1);
}

bool apply_value_change(Synth::InstrumentEditorBank* bank,
                        Sculptor::Graph*             graph,
                        Sculptor::OscGraphMapping*   mapping,
                        const Sculptor::GraphChange& change,
                        uint32_t                     channel,
                        uint32_t                     zone)
{
    if (change.node_idx >= Sculptor::max_nodes || ! graph->node_occupied(change.node_idx)) {
        return true;
    }
    const Sculptor::Node& node = graph->node(change.node_idx);
    if (change.slot_idx >= node.slots.num_allocated) {
        return true;
    }
    const Sculptor::Slot& slot = node.slots.entries[change.slot_idx];
    switch (node_role(*mapping, change.node_idx)) {
        case role_osc: {
            // The widget wrote the edited node; sibling views receive the
            // shared value eventlessly so every view stays in step.
            if (osc_row_is_dynamic(change.slot_idx) || osc_row_is_constant(change.slot_idx)) {
                sync_osc_graph_shared_slot(graph, *mapping, change.node_idx, change.slot_idx, slot.value);
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        }
        case role_param: {
            if (is_shared_param_slot(change.slot_idx)) {
                sync_osc_graph_shared_slot(graph, *mapping, change.node_idx, change.slot_idx, slot.value);
            }
            // The LFO row fields take part in the per-cell binding tuple, so
            // editing one can merge or split derived groups; the ordinals
            // re-stamp before the compile, exactly as for wiring changes.
            if (change.slot_idx == param_lfo_op_prop || change.slot_idx == param_lfo_depth_prop ||
                change.slot_idx == param_rate_scale_prop) {
                if (! reconcile_bindings(bank, graph, mapping, channel, zone)) {
                    return false;
                }
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        }
        case role_env:
        case role_lfo:
            return apply_osc_graph_descriptor_edit(*graph,
                                                   *mapping,
                                                   &bank->bank,
                                                   change.node_idx,
                                                   change.slot_idx,
                                                   slot.value);
        case role_input:
        case role_output:
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        default:
            return true;
    }
}

// Removes one oscillator layer from the zone's instrument (graph node
// delete): the layers compact, the zone's mask bits and the layer's kind-0
// record move with the layers, and the mapping drops the node.  Parameter
// records are target-keyed and generator records descriptor-keyed, so no
// record remapping beyond the oscillator range is needed.
bool remove_osc_layer(Synth::InstrumentEditorBank* bank,
                      Sculptor::OscGraphMapping*   mapping,
                      uint32_t                     channel,
                      uint32_t                     zone,
                      uint32_t                     layer)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return false;
    }
    Synth::Instrument& instrument = bank->bank.instruments.entries[zone_entry.instrument];
    if (layer >= instrument.layer_count) {
        return true;
    }
    for (uint32_t l = layer; l + 1 < Synth::max_layers; ++l) {
        instrument.layers[l] = instrument.layers[l + 1];
    }
    instrument.layers[Synth::max_layers - 1] = Synth::Oscillator{};
    instrument.layer_count--;

    Sculptor::compact_missing_sum_bits(bank, channel, zone, layer);

    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout record = bank->graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == 0 &&
            record.index >= canonical_first_osc && record.index < canonical_first_env) {
            const uint32_t layer_of_record = record.index - canonical_first_osc;
            if (layer_of_record == layer) {
                continue;
            }
            if (layer_of_record > layer) {
                record.index = static_cast<uint8_t>(record.index - 1);
            }
        }
        // A parameter record's served bits index the same layers the
        // compaction above renumbers, so they shift with it.
        if (record.channel == channel && record.zone == zone && record.kind == 3 && record.served != 0) {
            uint8_t compacted = 0;
            for (uint32_t l = 0; l < Synth::max_layers; ++l) {
                if (l == layer) {
                    continue;
                }
                if (record.served & (1u << l)) {
                    compacted |= static_cast<uint8_t>(1u << (l > layer ? l - 1 : l));
                }
            }
            record.served = compacted;
        }
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;

    // The mapping's oscillator run compacts exactly like the instrument's
    // layers and the missing-sum bits above: osc_nodes[layer] always names
    // the node of instrument layer `layer`, so a multi-node delete batch whose
    // later events recompile the instrument never sees a gap in the run.
    for (uint32_t l = layer; l + 1 < Synth::max_layers; ++l) {
        mapping->osc_nodes[l] = mapping->osc_nodes[l + 1];
    }
    mapping->osc_nodes[Synth::max_layers - 1] = Sculptor::pool_no_slot;
    return true;
}

bool apply_node_deleted(Synth::InstrumentEditorBank* bank,
                        Sculptor::Graph*             graph,
                        Sculptor::OscGraphMapping*   mapping,
                        const Sculptor::GraphChange& change,
                        uint32_t                     channel,
                        uint32_t                     zone)
{
    const uint32_t node_idx = change.node_idx;

    const int32_t instance = find_instance(*mapping, node_idx);
    if (instance >= 0) {
        const Sculptor::DetachedNode entry = mapping->detached[instance];
        if (entry.uid != 0) {
            const int32_t record_idx = find_record(*bank, channel, zone, entry.kind, entry.desc_id, entry.uid);
            if (record_idx >= 0) {
                remove_record_at(bank, static_cast<uint32_t>(record_idx));
            }
        }
        for (uint32_t i = static_cast<uint32_t>(instance); i + 1 < mapping->detached_count; ++i) {
            mapping->detached[i] = mapping->detached[i + 1];
        }
        mapping->detached_count--;
        return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
    }

    const int32_t param = find_param(*mapping, node_idx);
    if (param >= 0) {
        // Deleting a parameter deletes its kind-3 record (no detached
        // resurrection); the gen bindings of the cells it served were
        // cleared by the dropped parameter wires (their connection events
        // drained before this one). The target's MIDI routing follows the
        // last-serving-parameter rule: store_compiled_instrument zeroes a
        // target's inputs when its last serving parameter dies.
        const Sculptor::ParamEntry entry = mapping->params[param];
        if (entry.uid != 0) {
            const int32_t record_idx = find_record(*bank, channel, zone, 3, entry.target, entry.uid);
            if (record_idx >= 0) {
                remove_record_at(bank, static_cast<uint32_t>(record_idx));
            }
        }
        for (uint32_t i = static_cast<uint32_t>(param); i + 1 < mapping->param_count; ++i) {
            mapping->params[i] = mapping->params[i + 1];
        }
        mapping->param_count--;
        // The deleted parameter's group dies with this compile: reconcile
        // re-derives the survivors (including the ordinal re-stamp) before
        // the store, so the model never rests on stale ordinals.
        if (! reconcile_bindings(bank, graph, mapping, channel, zone)) {
            return false;
        }
        return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
    }

    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping->osc_nodes[layer] == node_idx) {
            // The compaction realigns the mapping run with the instrument's
            // layers, and the recompile from live wires rewrites the zone
            // (bindings of cells whose wires died with the layer, routing of
            // targets that lost their last serving parameter).  When another
            // oscillator of this batch is still pending deletion, the store
            // defers to that node_deleted's final compile.
            if (! remove_osc_layer(bank, mapping, channel, zone, layer)) {
                return false;
            }
            if (! reconcile_bindings(bank, graph, mapping, channel, zone)) {

                return false;
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        }
    }
    return true; // fixed nodes are vetoed upstream; nothing else carries state
}

// The missing-sum bits live in the bank (they outlive the mapping) and
// are file-local: only this TU reads and clears them.
void set_missing_sum_bit(Synth::InstrumentEditorBank* bank,
                         uint32_t                     channel,
                         uint32_t                     zone,
                         uint32_t                     layer,
                         bool                         missing)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel || layer >= Synth::max_layers) {
        return;
    }
    const uint8_t mask = static_cast<uint8_t>(1u << layer);
    if (missing) {
        bank->graph_missing_sum[channel][zone] |= mask;
    }
    else {
        bank->graph_missing_sum[channel][zone] &= static_cast<uint8_t>(~mask);
    }
}

bool channel_has_missing_sum(const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    if (channel >= Synth::max_channels) {
        return false;
    }
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
        if (bank.graph_missing_sum[channel][zone] != 0) {
            return true;
        }
    }
    return false;
}

bool apply_connection_change(Synth::InstrumentEditorBank* bank,
                             Sculptor::Graph*             graph,
                             Sculptor::OscGraphMapping*   mapping,
                             const Sculptor::GraphChange& change,
                             uint32_t                     channel,
                             uint32_t                     zone)
{
    // A parameter value wire landing on a different target's oscillator row
    // retargets the parameter: the wire decides the target. The dropped wire
    // itself keeps its endpoint (it already sits on the destination row);
    // the parameter's other value wires shift across. The guard refusals
    // mirror the validator, so a well-formed edit never refuses here; a
    // headless caller that bypasses the validator gets a refused batch.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx) &&
        graph->get_connection(change.connection_idx).output.slot_idx == mapping->param_output_slot) {
        const Sculptor::Connection& connection = graph->get_connection(change.connection_idx);
        const int32_t               p          = find_param(*mapping, connection.output.node_idx);
        if (p >= 0) {
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (mapping->osc_nodes[layer] != connection.input.node_idx) {
                    continue;
                }
                const int32_t target = osc_projected_row_index(connection.input.slot_idx);
                if (target >= 0 && static_cast<uint32_t>(target) != mapping->params[p].target) {
                    if (mapping->params[p].uid == 0 ||
                        param_target_has_recordless_derived(*mapping, static_cast<uint32_t>(target))) {
                        return false;
                    }
                    if (! retarget_param(bank,
                                         *graph,
                                         *mapping,
                                         channel,
                                         zone,
                                         static_cast<uint32_t>(p),
                                         static_cast<uint32_t>(target))) {
                        return false;
                    }
                }
                break;
            }
        }
    }
    // A first LFO wire adopts an audible depth: the row's depth defaults to
    // 0, which silences the LFO entirely, and 0 is almost never the intent.
    if (change.kind == Sculptor::ChangeKind::connection_added && change.connection_idx < Sculptor::max_connections &&
        graph->connection_occupied(change.connection_idx) && change.connection_input.slot_idx == param_lfo_input) {
        const int32_t p = find_param(*mapping, change.connection_input.node_idx);
        if (p >= 0 && graph->node_occupied(change.connection_input.node_idx)) {
            const uint32_t param_node = mapping->params[p].node_idx;
            if (graph->node(param_node).slots.entries[param_lfo_depth_prop].value.real == 0.0f) {
                graph->set_slot_value(param_node, param_lfo_depth_prop, Sculptor::PropertyValue{ .real = 0.5f });
            }
        }
    }

    // Wiring an envelope into the volume target forces the volume-envelope
    // shape on the shared descriptor: minimum 0 with the top preserved, and
    // the first and the last point at 0, so a note starts and ends in
    // silence.  The wire-time validator refuses cross-target sharing, so
    // the conversion never changes what a non-volume user hears.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx) &&
        graph->get_connection(change.connection_idx).input.slot_idx == param_env_input) {
        const Sculptor::Connection& connection = graph->get_connection(change.connection_idx);
        const int32_t               p          = find_param(*mapping, connection.input.node_idx);
        if (p >= 0 && graph->node_occupied(connection.input.node_idx) &&
            mapping->params[p].target == projected_index(Synth::mod_volume)) {
            const int32_t inst = find_instance(*mapping, connection.output.node_idx);
            if (inst >= 0 && mapping->detached[inst].kind == 1 && mapping->detached[inst].desc_id != 0 &&
                mapping->detached[inst].desc_id <= bank->bank.envelopes.num_allocated) {
                Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[mapping->detached[inst].desc_id - 1];
                if (! Sculptor::env_volume_shape_ok(env)) {
                    // A negative effective top cannot be preserved under the
                    // nonnegative volume range; refuse the wire.
                    if (! env_convert_to_volume_shape(env)) {
                        return false;
                    }
                }
            }
        }
    }

    // The missing-sum bits track the event, not the graph state: by the time
    // a deletion event is drained the pool slot may already be freed or
    // reused, so the endpoint pair rides in the event itself.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_deleted ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && change.connection_input.node_idx == mapping->output_node) {
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            // The event's output end must still be the layer's oscillator: a
            // wire that dies with an oscillator deleted earlier in the same
            // batch refers to a node the compaction already moved out of this
            // layer, and marking the layer now would flag the survivor's
            // intact edge.  The deleted layer's own bit is removed by
            // remove_osc_layer's compaction either way.
            if (change.connection_input.slot_idx == mapping->output_layer_input_slot[layer] &&
                change.connection_output.node_idx == mapping->osc_nodes[layer]) {
                const bool missing = change.kind == Sculptor::ChangeKind::connection_deleted;
                set_missing_sum_bit(bank, channel, zone, layer, missing);
                // Both unconnected endpoints of the broken edge carry the red
                // mark: the sum input and the oscillator output.
                graph->set_slot_missing(mapping->output_node, mapping->output_layer_input_slot[layer], missing);
                if (mapping->osc_nodes[layer] != Sculptor::pool_no_slot) {
                    graph->set_slot_missing(mapping->osc_nodes[layer], mapping->osc_output_slot, missing);
                }
            }
        }
    }

    // A retarget moves the input endpoint in place.  When the OLD input was
    // a same-target parameter source slot, the sibling mirrors must follow
    // the wire off that slot: compile reads the shared routing from whichever
    // parameter the user touched, so a stale mirror would keep routing an
    // input the user moved away.  The slot that now holds the live wire keeps
    // it (a retarget between two source rows of the same target stays shared).
    if (change.kind == Sculptor::ChangeKind::connection_changed && change.connection_idx < Sculptor::max_connections) {
        const int32_t old_p = find_param(*mapping, change.connection_prev_input.node_idx);
        if (old_p >= 0 && graph->node_occupied(change.connection_prev_input.node_idx)) {
            const Sculptor::ParamEntry& old_param = mapping->params[old_p];
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                if (change.connection_prev_input.slot_idx != param_src_input(i)) {
                    continue;
                }
                for (uint32_t q = 0; q < mapping->param_count; ++q) {
                    if (q == static_cast<uint32_t>(old_p) || mapping->params[q].target != old_param.target ||
                        mapping->params[q].node_idx == Sculptor::pool_no_slot ||
                        ! graph->node_occupied(mapping->params[q].node_idx)) {
                        continue;
                    }
                    if (mapping->params[q].node_idx == change.connection_input.node_idx &&
                        param_src_input(i) == change.connection_input.slot_idx) {
                        continue; // this slot holds the retargeted wire now
                    }
                    graph->set_slot_input(mapping->params[q].node_idx,
                                          param_src_input(i),
                                          Sculptor::EndPoint{ Sculptor::pool_no_slot, Sculptor::pool_no_slot });
                }
            }
        }
    }

    // Source-row edits define the shared routing entry, so a user wire into
    // one same-target parameter mirrors eventlessly onto every sibling: the
    // user's wire is the one connection event, the mirrors never enter the
    // change queue (an echo would re-apply the edit as a second user edit).
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_deleted ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections) {
        const int32_t p = find_param(*mapping, change.connection_input.node_idx);
        // A wire that died with its deleted node is bookkeeping, not a user
        if (p >= 0 && graph->node_occupied(change.connection_input.node_idx)) {
            const Sculptor::ParamEntry& param = mapping->params[p];
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                if (change.connection_input.slot_idx != param_src_input(i)) {
                    continue;
                }
                const Sculptor::EndPoint mirror =
                    change.kind == Sculptor::ChangeKind::connection_deleted
                        ? Sculptor::EndPoint{ Sculptor::pool_no_slot, Sculptor::pool_no_slot }
                        : change.connection_output;
                for (uint32_t q = 0; q < mapping->param_count; ++q) {
                    if (q == static_cast<uint32_t>(p) || mapping->params[q].target != param.target ||
                        mapping->params[q].node_idx == Sculptor::pool_no_slot ||
                        ! graph->node_occupied(mapping->params[q].node_idx)) {
                        continue;
                    }
                    graph->set_slot_input(mapping->params[q].node_idx, param_src_input(i), mirror);
                }
            }
        }
    }

    if (! reconcile_bindings(bank, graph, mapping, channel, zone)) {
        return false;
    }
    return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
}

// Free-text parameter rename: the title editor already wrote the live node
// name; the record stores it (a derived parameter gains its record first,
// seeded from its live wiring).  An empty name refuses so the batch
// recovery re-projects the old title back, and the stored form is capped at
// the record's 31 characters (the re-projection after the commit repaints
// the trimmed form).  A derived parameter with an earlier-enumerated
// record-less same-target sibling refuses instead: its record would attach
// to that sibling positionally at re-projection.
bool apply_param_name(Synth::InstrumentEditorBank* bank,
                      Sculptor::Graph*             graph,
                      Sculptor::OscGraphMapping*   mapping,
                      const Sculptor::GraphChange& change,
                      uint32_t                     channel,
                      uint32_t                     zone)
{
    const int32_t p = find_param(*mapping, change.node_idx);
    if (p < 0) {
        return true; // only parameter nodes are renamable in this graph
    }
    Sculptor::ParamEntry& param = mapping->params[p];
    if (graph->node(change.node_idx).name[0] == 0) {
        return false;
    }
    if (param.uid == 0 && Sculptor::param_has_recordless_predecessor(*mapping, static_cast<uint32_t>(p))) {
        // The new record would attach positionally to the earlier
        // record-less same-target sibling at re-projection and hijack its
        // node, so the rename is refused; the batch recovery re-derives the
        // old title.
        graph->set_error("the earlier parameter of this target must be renamed first");
        return false;
    }
    if (param.uid == 0 && ! detach_param(bank, *graph, mapping, channel, zone, static_cast<uint32_t>(p))) {
        return false; // record capacity: the rename cannot persist
    }
    // detach_param stamps the naive enumeration ordinal, which can miscount
    // dormant and merged siblings; the re-stamp replaces it with the derived
    // truth before the change reports success.
    reconcile_bindings(bank, graph, mapping, channel, zone);
    const int32_t record_idx = find_record(*bank, channel, zone, 3, param.target, param.uid);
    if (record_idx < 0) {
        return false;
    }
    snprintf(bank->graph_layout[record_idx].name,
             sizeof(bank->graph_layout[record_idx].name),
             "%s",
             graph->node(change.node_idx).name);
    return true;
}

} // namespace

uint32_t Sculptor::osc_graph_canonical_index(const OscGraphMapping& mapping, uint32_t node_idx)
{
    if (node_idx == pool_no_slot) {
        return pool_no_slot;
    }
    for (uint32_t index = 0; index < canonical_node_count; ++index) {
        if (node_at_canonical_index(mapping, index) == node_idx) {
            return index;
        }
    }
    return pool_no_slot;
}

// A generator instance no parameter references becomes free-standing: a
// kind-1/2 record preserves it across re-projection.  Instances already
// carrying a record keep it.
bool Sculptor::detach_osc_graph_instance(Synth::InstrumentEditorBank* bank,
                                         const Sculptor::Graph&       graph,
                                         Sculptor::OscGraphMapping*   mapping,
                                         uint32_t                     channel,
                                         uint32_t                     zone,
                                         uint32_t                     registry_idx)
{
    if (bank->graph_layout_count >= Synth::max_graph_records) {
        return false;
    }
    const Sculptor::DetachedNode entry  = mapping->detached[registry_idx];
    Synth::GraphNodeLayout       record = {};
    record.channel                      = static_cast<uint8_t>(channel);
    record.zone                         = static_cast<uint8_t>(zone);
    record.kind                         = entry.kind;
    record.index                        = entry.desc_id;
    const Sculptor::Node& node          = graph.node(entry.node_idx);
    record.x                            = node.position.x;
    record.y                            = node.position.y;
    record.width_override               = node.content_width_override;
    record.height_override              = node.content_height_override;
    if (entry.kind == 2) {
        // The sources ride the live depth/rate edges, so edits while
        // free-standing survive re-projection.
        const uint32_t   depth_conn = connection_into(graph, entry.node_idx, mapping->lfo_depth_input_slot);
        const uint32_t   rate_conn  = connection_into(graph, entry.node_idx, mapping->lfo_rate_input_slot);
        Synth::ModSource source     = Synth::ModSource::none;
        if (depth_conn != Sculptor::pool_no_slot) {
            if (! edge_source(graph, *mapping, depth_conn, &source)) {
                return false;
            }
            record.depth_source = static_cast<uint8_t>(source);
        }
        if (rate_conn != Sculptor::pool_no_slot) {
            if (! edge_source(graph, *mapping, rate_conn, &source)) {
                return false;
            }
            record.rate_source = static_cast<uint8_t>(source);
        }
    }
    record.uid = Sculptor::allocate_detached_uid(*bank, channel, zone, entry.kind);
    append_record(bank, record);
    mapping->detached[registry_idx].uid = record.uid;
    return true;
}

// Lowest per-(zone, kind) instance id not in use by a live record.
uint8_t Sculptor::allocate_detached_uid(const Synth::InstrumentEditorBank& bank,
                                        uint32_t                           channel,
                                        uint32_t                           zone,
                                        uint32_t                           kind)
{
    bool used[256] = {};
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == kind) {
            used[record.uid] = true;
        }
    }
    for (uint32_t uid = 1; uid < 256; ++uid) {
        if (! used[uid]) {
            return static_cast<uint8_t>(uid);
        }
    }
    // Unreachable while the caller enforces the per-zone detached cap (120
    // records cannot cover 255 ids).
    assert(false);
    return 0;
}

bool Sculptor::sync_osc_graph_shared_slot(Graph*                 graph,
                                          const OscGraphMapping& mapping,
                                          uint32_t               node_idx,
                                          uint32_t               slot_idx,
                                          PropertyValue          value)
{
    const int32_t p = find_param(mapping, node_idx);
    if (p >= 0) {
        const ParamEntry& param = mapping.params[p];
        if (slot_idx == param_value_prop) {
            for (uint32_t q = 0; q < mapping.param_count; ++q) {
                if (mapping.params[q].target == param.target && mapping.params[q].node_idx != pool_no_slot &&
                    graph->node_occupied(mapping.params[q].node_idx)) {
                    graph->set_slot_value(mapping.params[q].node_idx, param_value_prop, value);
                }
            }
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (mapping.osc_nodes[layer] != pool_no_slot && graph->node_occupied(mapping.osc_nodes[layer])) {
                    graph->set_slot_value(
                        mapping.osc_nodes[layer],
                        osc_target_prop(static_cast<Synth::ModTarget>(projected_target_of_index(param.target))),
                        value);
                }
            }
            return true;
        }
        if (slot_idx == param_src_op_prop(0) || slot_idx == param_src_scale_prop(0) ||
            slot_idx == param_src_op_prop(1) || slot_idx == param_src_scale_prop(1)) {
            for (uint32_t q = 0; q < mapping.param_count; ++q) {
                if (mapping.params[q].target == param.target && mapping.params[q].node_idx != pool_no_slot &&
                    graph->node_occupied(mapping.params[q].node_idx)) {
                    graph->set_slot_value(mapping.params[q].node_idx, slot_idx, value);
                }
            }
            return true;
        }
        return false;
    }

    if (node_role(mapping, node_idx) != role_osc) {
        return false;
    }
    if (osc_row_is_dynamic(slot_idx)) {
        // A dynamic value row edits the shared routing base value: every
        // oscillator's inline view and every same-target parameter's value
        // row is a synced copy.
        const uint32_t proj = static_cast<uint32_t>(osc_projected_row_index(slot_idx));
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping.osc_nodes[layer] != pool_no_slot && graph->node_occupied(mapping.osc_nodes[layer])) {
                graph->set_slot_value(mapping.osc_nodes[layer], slot_idx, value);
            }
        }
        for (uint32_t q = 0; q < mapping.param_count; ++q) {
            if (mapping.params[q].target == proj && mapping.params[q].node_idx != pool_no_slot &&
                graph->node_occupied(mapping.params[q].node_idx)) {
                graph->set_slot_value(mapping.params[q].node_idx, param_value_prop, value);
            }
        }
        return true;
    }
    if (osc_row_is_constant(slot_idx)) {
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping.osc_nodes[layer] != pool_no_slot && graph->node_occupied(mapping.osc_nodes[layer])) {
                graph->set_slot_value(mapping.osc_nodes[layer], slot_idx, value);
            }
        }
        return true;
    }
    return false;
}

bool Sculptor::apply_osc_graph_change(Synth::InstrumentEditorBank* bank,
                                      Graph*                       graph,
                                      OscGraphMapping*             mapping,
                                      const GraphChange&           change,
                                      uint32_t                     channel,
                                      uint32_t                     zone)
{
    switch (change.kind) {
        case ChangeKind::value_changed:
            return apply_value_change(bank, graph, mapping, change, channel, zone);
        case ChangeKind::connection_added:
        case ChangeKind::connection_deleted:
        case ChangeKind::connection_changed:
            return apply_connection_change(bank, graph, mapping, change, channel, zone);
        case ChangeKind::node_deleted:
            return apply_node_deleted(bank, graph, mapping, change, channel, zone);
        case ChangeKind::name_changed:
            return apply_param_name(bank, graph, mapping, change, channel, zone);
        default:
            return true; // node/slot additions and colors carry no model state
    }
}

// Eventless graph rewrites (wire-driven retargeting) carry no change
// events, so a caller outside the drain refreshes the bindings and the
// compiled instrument explicitly, the way apply_connection_change does
// after a drained batch.
bool Sculptor::refresh_osc_graph_compilation(Synth::InstrumentEditorBank* bank,
                                             Graph&                       graph,
                                             OscGraphMapping&             mapping,
                                             const uint32_t               channel,
                                             const uint32_t               zone)
{
    if (! reconcile_bindings(bank, &graph, &mapping, channel, zone)) {
        return false;
    }
    return store_compiled_instrument(bank, graph, mapping, channel, zone);
}

// Mirrors the projection's kind-3 attachment decisions against the
// derived groups alone: wholesale ordinal trust, the explicit pass with
// persisted-tuple verification, and the uid-order positional pass with
// class and tuple eligibility (the record-pairing passes in
// project_editor_to_graph). Returns how many records materialize
// free-standing parameters; a record the projection would attach adds
// none. The comparison uses the groups' tuples because a derived
// parameter's live tuple equals its group's tuple at the point the
// record passes run - except that the live tuple's LFO source fields
// are zero whenever the parameter has no LFO edge, so env-only groups
// compare with zero sources regardless of their raw source values -
// and a carrier group's live tuple is all zero.
uint32_t group_depth_source(const ParamGroup& group)
{
    return group.lfo_desc_id == 0 ? 0 : group.depth_source;
}

uint32_t group_rate_source(const ParamGroup& group)
{
    return group.lfo_desc_id == 0 ? 0 : group.rate_source;
}

uint32_t count_surplus_params(const Synth::InstrumentEditorBank& bank,
                              uint32_t                           channel,
                              uint32_t                           zone,
                              const ParamGroup*                  groups,
                              uint32_t                           group_count)
{
    uint32_t surplus_total = 0;
    for (uint32_t proj = 0; proj < 5; ++proj) {
        // Per-target record scratch, capped like the projection's own
        // list; beyond the cap the projection refuses outright, so
        // counting every record as surplus keeps the bound conservative.
        uint32_t record_idxs[Synth::max_detached_per_zone] = {};
        uint32_t num_records                               = 0;
        bool     over_cap                                  = false;
        for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[r];
            if (record.channel == channel && record.zone == zone && record.kind == 3 && record.index == proj) {
                if (num_records < Synth::max_detached_per_zone) {
                    record_idxs[num_records++] = r;
                }
                else {
                    over_cap = true;
                }
            }
        }
        // Insertion sort by uid: attachment order is uid order.
        for (uint32_t a = 1; a < num_records; ++a) {
            const uint32_t v = record_idxs[a];
            uint32_t       b = a;
            while (b > 0 && bank.graph_layout[record_idxs[b - 1]].uid > bank.graph_layout[v].uid) {
                record_idxs[b] = record_idxs[b - 1];
                --b;
            }
            record_idxs[b] = v;
        }
        // This target's groups in derivation order.
        uint32_t target_groups                        = 0;
        uint32_t group_idx[Sculptor::max_param_nodes] = {};
        for (uint32_t g = 0; g < group_count; ++g) {
            if (groups[g].target == proj) {
                group_idx[target_groups++] = g;
            }
        }
        bool     claimed[Sculptor::max_param_nodes]     = {};
        bool     attached[Synth::max_detached_per_zone] = {};
        uint32_t surplus_here                           = over_cap ? num_records : 0;
        if (! over_cap) {
            // Explicit ordinals are trusted only while every explicit
            // ordinal names an existing group.
            bool trust_explicit = true;
            for (uint32_t ri = 0; ri < num_records; ++ri) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
                if (record.param_slot != 0 && record.param_slot != Synth::graph_record_param_free &&
                    record.param_slot > target_groups) {
                    trust_explicit = false;
                    break;
                }
            }
            for (uint32_t ri = 0; trust_explicit && ri < num_records; ++ri) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
                if (record.param_slot == 0 || record.param_slot == Synth::graph_record_param_free) {
                    continue;
                }
                uint32_t seen = 0;
                for (uint32_t s = 0; s < target_groups; ++s) {
                    ++seen;
                    if (seen != record.param_slot) {
                        continue;
                    }
                    if (! claimed[s]) {
                        const bool bare = record.env_desc_id == 0 && record.lfo_desc_id == 0 && record.served == 0;
                        const ParamGroup& tuple = groups[group_idx[s]];
                        if (bare || (static_cast<uint16_t>(record.env_desc_id) == tuple.env_desc_id &&
                                     static_cast<uint16_t>(record.lfo_desc_id) == tuple.lfo_desc_id &&
                                     record.lfo_depth_source == group_depth_source(tuple) &&
                                     record.lfo_rate_source == group_rate_source(tuple))) {
                            claimed[s]   = true;
                            attached[ri] = true;
                        }
                    }
                    break;
                }
            }
            // Positional fallback, uid order: each unattached record
            // considers the first unclaimed group of the target; class
            // and tuple mismatches leave that candidate to later records
            // and send the record to the surplus count.
            uint32_t s_scan = 0;
            for (uint32_t ri = 0; ri < num_records; ++ri) {
                if (attached[ri]) {
                    continue;
                }
                const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
                while (s_scan < target_groups && claimed[s_scan]) {
                    ++s_scan;
                }
                if (s_scan >= target_groups) {
                    break;
                }
                const bool        record_tuple = record.env_desc_id != 0 || record.lfo_desc_id != 0;
                const ParamGroup& tuple        = groups[group_idx[s_scan]];
                const bool        param_tuple  = tuple.env_desc_id != 0 || tuple.lfo_desc_id != 0;
                const bool        bare         = ! record_tuple && record.served == 0;
                if (record_tuple != param_tuple && ! bare) {
                    continue;
                }
                if (! bare) {
                    if (static_cast<uint16_t>(record.env_desc_id) != tuple.env_desc_id ||
                        static_cast<uint16_t>(record.lfo_desc_id) != tuple.lfo_desc_id ||
                        record.lfo_depth_source != group_depth_source(tuple) ||
                        record.lfo_rate_source != group_rate_source(tuple)) {
                        continue;
                    }
                }
                claimed[s_scan] = true;
                attached[ri]    = true;
                ++s_scan;
            }
            for (uint32_t ri = 0; ri < num_records; ++ri) {
                if (! attached[ri]) {
                    ++surplus_here;
                }
            }
        }
        surplus_total += surplus_here;
    }
    return surplus_total;
}

uint32_t Sculptor::count_projected_nodes(const Synth::InstrumentEditorBank& bank,
                                         uint32_t                           channel,
                                         uint32_t                           zone,
                                         uint32_t*                          parameter_count)
{
    // Fixed inputs + sum node, then whatever the zone's instrument and the
    // zone's records add: parameters, generator instances and record
    // surplus (a record that attaches to a derived node adds no node).
    uint32_t count  = Synth::graph_canonical_input_node_count + 1;
    uint32_t params = 0;
    if (parameter_count) {
        *parameter_count = 0;
    }
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return count;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return count;
    }
    const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];
    count += instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    ParamGroup     groups[Sculptor::max_param_nodes];
    const uint32_t group_count = derive_param_groups(instrument, groups);
    count += group_count;
    params += group_count;

    InstanceKey    keys[2 * Sculptor::max_param_nodes];
    const uint32_t key_count = derive_instance_keys(groups, group_count, keys);
    for (uint32_t k = 0; k < key_count; ++k) {
        const uint32_t num_allocated =
            keys[k].kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
        if (keys[k].desc_id != 0 && keys[k].desc_id <= num_allocated) {
            ++count; // one node per distinct instance key
        }
    }

    // Records: kind-1/2 records either attach to a derived instance or
    // materialize a surplus node; kind-3 records follow the projection's
    // attachment decisions (counted below the loop).
    bool claimed[2 * Sculptor::max_param_nodes] = {};
    for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[r];
        if (record.channel != channel || record.zone != zone) {
            continue;
        }
        if (record.kind == 1 || record.kind == 2) {
            const uint32_t num_allocated =
                record.kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated) {
                continue; // invalid records are validation's business
            }
            bool attached = false;
            for (uint32_t k = 0; k < key_count && ! attached; ++k) {
                if (claimed[k] || keys[k].kind != record.kind || keys[k].desc_id != record.index) {
                    continue;
                }
                if (record.kind == 2 &&
                    (keys[k].depth_source != record.depth_source || keys[k].rate_source != record.rate_source)) {
                    continue;
                }
                claimed[k] = true;
                attached   = true;
            }
            if (! attached) {
                ++count;
            }
        }
    }
    // Kind-3 records: the parameter count mirrors the projection's
    // attachment decisions, so a record the projection would attach adds
    // no node and anything else materializes a free-standing parameter.
    const uint32_t surplus_params = count_surplus_params(bank, channel, zone, groups, group_count);
    count += surplus_params;
    params += surplus_params;
    if (parameter_count) {
        *parameter_count = params;
    }
    return count;
}

bool Sculptor::project_editor_to_graph(const Synth::InstrumentEditorBank& bank,
                                       Graph*                             graph,
                                       OscGraphMapping*                   mapping,
                                       uint32_t                           channel,
                                       uint32_t                           zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return false;
    }
    if (! project_instrument_to_graph(bank.bank.instruments.entries[zone_entry.instrument],
                                      bank.bank,
                                      graph,
                                      mapping)) {
        return false;
    }
    mapping->channel = channel;
    mapping->zone    = zone;

    const uint8_t missing = bank.graph_missing_sum[channel][zone];

    // Masked oscillator->sum edges stay deleted across re-projection, and
    // the surviving bits re-apply the red missing marks (which live outside
    // the graph snapshots).
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        const uint32_t slot          = mapping->output_layer_input_slot[layer];
        const bool     layer_missing = (missing & (1u << layer)) != 0;
        if (layer_missing) {
            const uint32_t conn = connection_into(*graph, mapping->output_node, slot);
            if (conn != pool_no_slot) {
                graph->delete_connection(conn);
            }
        }
        // Both unconnected endpoints of the broken edge carry the red mark:
        // the sum input and the oscillator output.
        graph->set_slot_missing(mapping->output_node, slot, layer_missing);
        if (mapping->osc_nodes[layer] != pool_no_slot) {
            graph->set_slot_missing(mapping->osc_nodes[layer], mapping->osc_output_slot, layer_missing);
        }
    }

    const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];

    // Records attach to derived nodes: kind-1/2 records to the first
    // unclaimed instance with a matching key (records visited in array
    // order; attachment consumes one instance per record), kind-3 records
    // to the K-th derived same-target parameter ordered by uid.  Surplus
    // records materialize free-standing nodes.
    bool claimed[max_detached_nodes] = {};
    for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[r];
        if (record.channel != channel || record.zone != zone) {
            continue;
        }
        if (record.kind == 0) {
            const uint32_t node = node_at_canonical_index(*mapping, record.index);
            if (node == pool_no_slot) {
                continue;
            }
            graph->set_node_layout(node,
                                   vmath::vec2(record.x, record.y),
                                   record.width_override,
                                   record.height_override);
            continue;
        }
        if (record.kind == 1 || record.kind == 2) {
            const bool     is_env        = record.kind == 1;
            const uint32_t num_allocated = is_env ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated || record.depth_source > num_osc_graph_inputs ||
                record.rate_source > num_osc_graph_inputs || mapping->detached_count >= max_detached_nodes) {
                return false;
            }
            bool attached = false;
            for (uint32_t i = 0; i < mapping->detached_count && ! attached; ++i) {
                DetachedNode& entry = mapping->detached[i];
                if (claimed[i] || entry.uid != 0 || entry.kind != record.kind || entry.desc_id != record.index) {
                    continue;
                }
                if (record.kind == 2 &&
                    (entry.depth_source != record.depth_source || entry.rate_source != record.rate_source)) {
                    continue;
                }
                claimed[i]         = true;
                attached           = true;
                entry.uid          = record.uid;
                entry.depth_source = record.depth_source;
                entry.rate_source  = record.rate_source;
                graph->set_node_layout(entry.node_idx,
                                       vmath::vec2(record.x, record.y),
                                       record.width_override,
                                       record.height_override);
                char name[32];
                snprintf(name, sizeof(name), is_env ? "Envelope %u" : "LFO %u", record.uid);
                graph->rename_node(entry.node_idx, name);
            }
            if (attached) {
                continue;
            }
            // Surplus record: a free-standing generator instance with its
            // own position, uid and (for LFOs) depth/rate source wires.
            char name[32];
            snprintf(name, sizeof(name), is_env ? "Envelope %u" : "LFO %u", record.uid);
            const uint32_t node = is_env ? create_env_node(graph,
                                                           name,
                                                           vmath::vec2(record.x, record.y),
                                                           bank.bank.envelopes.entries[record.index - 1])
                                         : create_lfo_node(graph,
                                                           name,
                                                           vmath::vec2(record.x, record.y),
                                                           bank.bank.lfos.entries[record.index - 1]);
            if (node == pool_no_slot) {
                return false;
            }
            DetachedNode& entry = mapping->detached[mapping->detached_count++];
            entry.node_idx      = node;
            entry.kind          = record.kind;
            entry.desc_id       = record.index;
            entry.uid           = record.uid;
            entry.depth_source  = record.depth_source;
            entry.rate_source   = record.rate_source;
            if (record.kind == 2 && record.depth_source != 0) {
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.depth_source - 1] },
                    EndPoint{ node, mapping->lfo_depth_input_slot });
            }
            if (record.kind == 2 && record.rate_source != 0) {
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.rate_source - 1] },
                    EndPoint{ node, mapping->lfo_rate_input_slot });
            }
        }
    }

    // Kind-3 records pair with derived parameters in three passes: records
    // carrying explicit ordinals bind to the group their ordinal names
    // (skipped wholesale when an ordinal names no existing group), the rest
    // join a uid-order positional attach over unclaimed groups, and the
    // leftovers materialize detached extra parameters (inert binders; value
    // and source rows stay live shared views).
    for (uint32_t proj = 0; proj < 5; ++proj) {
        // The per-zone record cap bounds this list; a defensive ceiling
        // keeps a malformed bank from overflowing the scratch buffer.
        uint32_t record_idxs[Synth::max_detached_per_zone] = {};
        uint32_t num_records                               = 0;
        for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[r];
            if (record.channel == channel && record.zone == zone && record.kind == 3 && record.index == proj) {
                if (num_records < Synth::max_detached_per_zone) {
                    record_idxs[num_records++] = r;
                }
                else {
                    return false;
                }
            }
        }
        // Insertion sort by uid: attachment order is uid order.
        for (uint32_t a = 1; a < num_records; ++a) {
            const uint32_t v = record_idxs[a];
            uint32_t       b = a;
            while (b > 0 && bank.graph_layout[record_idxs[b - 1]].uid > bank.graph_layout[v].uid) {
                record_idxs[b] = record_idxs[b - 1];
                --b;
            }
            record_idxs[b] = v;
        }

        bool claimed_param[max_param_nodes]                = {};
        bool attached_record[Synth::max_detached_per_zone] = {};
        // Count this target's derived groups (surplus materializes after
        // this block, so every same-target parameter here is a group).
        uint32_t group_count = 0;
        for (uint32_t p = 0; p < mapping->param_count; ++p) {
            if (mapping->params[p].target == proj && mapping->params[p].node_idx != pool_no_slot) {
                ++group_count;
            }
        }
        // Explicit ordinals are trusted only while every explicit ordinal
        // names an existing group. Deletions can renumber positions, letting
        // a surviving group occupy a dead record's ordinal; binding by
        // position then would steal that group from its own record. When
        // any ordinal is out of range the projection distrusts the explicit
        // pass wholesale and every record joins the uid-order positional
        // attach, which handles stale ordinals correctly.
        bool trust_explicit = true;
        for (uint32_t ri = 0; ri < num_records; ++ri) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
            if (record.param_slot != 0 && record.param_slot != Synth::graph_record_param_free &&
                record.param_slot > group_count) {
                trust_explicit = false;
                break;
            }
        }

        for (uint32_t ri = 0; trust_explicit && ri < num_records; ++ri) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
            if (record.param_slot == 0 || record.param_slot == Synth::graph_record_param_free) {
                continue;
            }
            uint32_t seen = 0;
            for (uint32_t p = 0; p < mapping->param_count; ++p) {
                ParamEntry& param = mapping->params[p];
                if (param.target != proj || param.node_idx == pool_no_slot) {
                    continue;
                }
                ++seen;
                if (seen != record.param_slot) {
                    continue;
                }
                if (! claimed_param[p]) {
                    // A record binds a group only when the group carries the
                    // record's stored descriptor ids and LFO sources; a
                    // mismatched record falls through to the positional pass.
                    // A fully bare record (nothing stored) injects nothing,
                    // so its trusted ordinal needs no tuple check.
                    const bool bare_record = record.env_desc_id == 0 && record.lfo_desc_id == 0 && record.served == 0;
                    ParamGroup tuple;
                    if (! bare_record && (! live_param_tuple(*graph, *mapping, param, &tuple) ||
                                          static_cast<uint16_t>(record.env_desc_id) != tuple.env_desc_id ||
                                          static_cast<uint16_t>(record.lfo_desc_id) != tuple.lfo_desc_id ||
                                          record.lfo_depth_source != tuple.depth_source ||
                                          record.lfo_rate_source != tuple.rate_source)) {
                        break;
                    }
                    param.uid = record.uid;
                    // The record's served bits persist wiring the generator
                    // tuple does not express; the gen-derived bits stay set
                    // either way (they reflect the live bindings).
                    param.served = static_cast<uint8_t>(param.served | record.served);
                    graph->set_node_layout(param.node_idx,
                                           vmath::vec2(record.x, record.y),
                                           record.width_override,
                                           record.height_override);
                    wire_record_state(graph, mapping, record, &param);
                    claimed_param[p]    = true;
                    attached_record[ri] = true;
                }
                break;
            }
        }
        // Positional fallback, uid order: records without a trusted explicit
        // ordinal take the first same-target group the earlier passes left
        // unclaimed. Eligibility guards what wire_record_state would inject:
        // a record carrying a stored generator tuple may only claim a
        // tuple-bearing group whose tuple matches the record's persisted
        // descriptor ids and LFO sources, a tuple-less record carrying
        // persisted value wires (a dormant parameter whose envelope and LFO
        // inputs were disconnected) may only claim a tuple-less carrier group
        // (the carrier's live tuple is all zero, so the persisted zero tuple
        // matches it), and a bare record (tuple-less, nothing persisted)
        // claims any unclaimed group because it injects nothing. A mismatched
        // record leaves the candidate to later records and falls through to
        // the surplus pass below.
        uint32_t p_scan = 0;
        for (uint32_t ri = 0; ri < num_records; ++ri) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
            if (attached_record[ri]) {
                continue;
            }
            const bool record_tuple = record.env_desc_id != 0 || record.lfo_desc_id != 0;
            while (p_scan < mapping->param_count &&
                   (mapping->params[p_scan].target != proj || mapping->params[p_scan].node_idx == pool_no_slot ||
                    claimed_param[p_scan])) {
                ++p_scan;
            }
            if (p_scan >= mapping->param_count) {
                break;
            }
            const bool param_tuple =
                mapping->params[p_scan].env_node != pool_no_slot || mapping->params[p_scan].lfo_node != pool_no_slot;
            const bool bare_record = ! record_tuple && record.served == 0;
            if (record_tuple != param_tuple && ! bare_record) {
                continue;
            }
            if (! bare_record) {
                // The same persisted-field comparison the explicit pass
                // performs: op, depth and rate scale are not persisted, so
                // they play no part, and a record whose stored tuple names a
                // different group must not adopt it.
                ParamGroup tuple;
                if (! live_param_tuple(*graph, *mapping, mapping->params[p_scan], &tuple) ||
                    static_cast<uint16_t>(record.env_desc_id) != tuple.env_desc_id ||
                    static_cast<uint16_t>(record.lfo_desc_id) != tuple.lfo_desc_id ||
                    record.lfo_depth_source != tuple.depth_source || record.lfo_rate_source != tuple.rate_source) {
                    continue;
                }
            }
            ParamEntry& param = mapping->params[p_scan];

            param.uid    = record.uid;
            param.served = static_cast<uint8_t>(param.served | record.served);
            graph->set_node_layout(param.node_idx,
                                   vmath::vec2(record.x, record.y),
                                   record.width_override,
                                   record.height_override);
            wire_record_state(graph, mapping, record, &param);
            claimed_param[p_scan] = true;
            attached_record[ri]   = true;
            ++p_scan;
        }
        // Records with no same-target group left materialize a free-standing
        // extra parameter (inert binder; value and source rows stay live
        // shared views).
        for (uint32_t ri = 0; ri < num_records; ++ri) {
            if (attached_record[ri]) {
                continue;
            }
            const Synth::GraphNodeLayout& record = bank.graph_layout[record_idxs[ri]];
            if (mapping->param_count >= max_param_nodes) {
                return false;
            }
            const Synth::InputRouting& routing = instrument.routing[projected_target_of_index(proj)];
            const uint32_t node = create_param_node(graph,
                                                    param_target_names[proj],
                                                    vmath::vec2(record.x, record.y),
                                                    static_cast<Synth::ModTarget>(projected_target_of_index(proj)),
                                                    routing.base_value,
                                                    0u,
                                                    0.0f,
                                                    0.0f,
                                                    routing);
            if (node == pool_no_slot) {
                return false;
            }
            ParamEntry& entry = mapping->params[mapping->param_count++];
            entry.node_idx    = node;
            entry.target      = static_cast<uint8_t>(proj);
            entry.uid         = record.uid;
            entry.env_node    = pool_no_slot;
            entry.lfo_node    = pool_no_slot;
            entry.served      = record.served;
            // Source rows are live shared views on every parameter of the
            // target, detached extras included.
            wire_param_sources(graph, *mapping, node, routing);
            wire_record_state(graph, mapping, record, &entry);
        }
    }

    // Visual roles install after the record surplus appends, so detached
    // extra parameters carry the parameter tint and shared-row markers too.
    install_visual_roles(graph, *mapping);

    // Names assign after the record surplus appends, so duplicates of one
    // target number by the full enumeration order; a stored record name
    // then overrides the derived one.
    name_params(graph, *mapping);
    for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[r];
        if (record.channel != channel || record.zone != zone || record.kind != 3 || record.name[0] == 0) {
            continue;
        }
        for (uint32_t p = 0; p < mapping->param_count; ++p) {
            if (mapping->params[p].target == record.index && mapping->params[p].uid == record.uid &&
                mapping->params[p].node_idx != pool_no_slot) {
                graph->rename_node(mapping->params[p].node_idx, record.name);
            }
        }
    }
    return true;
}

bool Sculptor::undo_group_needs_snapshot(UndoGroupState* state, UndoGroupTag tag)
{
    if (state->has_last && state->last.kind == tag.kind && state->last.id0 == tag.id0 && state->last.id1 == tag.id1) {
        return false;
    }
    state->last     = tag;
    state->has_last = true;
    return true;
}

void Sculptor::undo_group_reset(UndoGroupState* state)
{
    state->has_last = false;
}

bool Sculptor::graph_records_have_capacity(const Synth::InstrumentEditorBank& bank, uint32_t additional)
{
    return bank.graph_layout_count + additional <= Synth::max_graph_records;
}

void Sculptor::compact_missing_sum_bits(Synth::InstrumentEditorBank* bank,
                                        uint32_t                     channel,
                                        uint32_t                     zone,
                                        uint32_t                     removed_layer)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel || removed_layer >= Synth::max_layers) {
        return;
    }
    uint8_t compacted = 0;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (layer == removed_layer) {
            continue;
        }
        if (bank->graph_missing_sum[channel][zone] & (1u << layer)) {
            const uint32_t target = layer > removed_layer ? layer - 1u : layer;
            compacted |= static_cast<uint8_t>(1u << target);
        }
    }
    bank->graph_missing_sum[channel][zone] = compacted;
}

void Sculptor::compute_publish_channel_enabled(const Synth::InstrumentEditorBank& bank,
                                               uint8_t                            out_enabled[Synth::max_channels])
{
    // The stored channel_enabled values stay untouched; a channel publishes
    // disabled only while one of its zones holds a broken oscillator sum.
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        out_enabled[channel] =
            (bank.bank.channel_enabled[channel] != 0 && ! channel_has_missing_sum(bank, channel)) ? 1 : 0;
    }
}

bool Sculptor::zone_records_split_copy(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone + 1 >= Synth::max_instr_per_channel) {
        return false;
    }
    // Preflight before any shift: the zone's kind-0 records must fit after the
    // copy, so a refusal leaves every record and mask row untouched.
    uint32_t kind0_count = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == 0) {
            ++kind0_count;
        }
    }
    if (bank->graph_layout_count + kind0_count > Synth::max_graph_records) {
        return false;
    }
    // Shift later zones first, then copy: records and mask rows move together.
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel == channel && record.zone > zone) {
            record.zone++;
        }
    }
    for (uint32_t z = Synth::max_instr_per_channel - 1; z > zone + 1; --z) {
        bank->graph_missing_sum[channel][z] = bank->graph_missing_sum[channel][z - 1];
    }
    bank->graph_missing_sum[channel][zone + 1] = bank->graph_missing_sum[channel][zone];

    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        // Only bound fixed-node layout records copy; generators and
        // parameters are zone-local UI state and must not cross-link two
        // zones to one descriptor or target.
        if (record.channel == channel && record.zone == zone && record.kind == 0) {
            Synth::GraphNodeLayout copy                    = record;
            copy.zone                                      = static_cast<uint8_t>(zone + 1);
            bank->graph_layout[bank->graph_layout_count++] = copy;
        }
    }
    return true;
}

void Sculptor::zone_records_drop_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }
    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout record = bank->graph_layout[i];
        if (record.channel == channel) {
            if (record.zone == zone) {
                continue;
            }
            if (record.zone > zone) {
                record.zone--;
            }
        }
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;
    for (uint32_t z = zone; z + 1 < Synth::max_instr_per_channel; ++z) {
        bank->graph_missing_sum[channel][z] = bank->graph_missing_sum[channel][z + 1];
    }
    bank->graph_missing_sum[channel][Synth::max_instr_per_channel - 1] = 0;
}

void Sculptor::channel_records_reset(Synth::InstrumentEditorBank* bank, uint32_t channel)
{
    if (channel >= Synth::max_channels) {
        return;
    }
    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        if (bank->graph_layout[i].channel == channel) {
            continue;
        }
        bank->graph_layout[write++] = bank->graph_layout[i];
    }
    bank->graph_layout_count = write;
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
        bank->graph_missing_sum[channel][zone] = 0;
    }
}
