// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_osc_graph.h"
#include "../core/d_printf.h"
#include "sculptor_bank_json.h"
#include "sculptor_effect_graph.h"
#include "sculptor_instr_envelope_edit.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <string.h>

using Sculptor::bounded_real_slot;
using Sculptor::input_slot;
using Sculptor::int_slot;
using Sculptor::lfo_wave_names;
using Sculptor::list_slot;
using Sculptor::make_slot;
using Sculptor::mod_source_names;
using Sculptor::output_slot;
using Sculptor::real_slot;
using Sculptor::source_op_names;

using Sculptor::connection_into;
using Sculptor::Graph;
using Sculptor::osc_slot_count;
using Sculptor::osc_target_block_base;
using Sculptor::osc_target_block_field;
using Sculptor::osc_target_block_stride;
using Sculptor::osc_target_env_depth;
using Sculptor::osc_target_env_dot;
using Sculptor::osc_target_lfo_dot;
using Sculptor::osc_target_row_slot;
using Sculptor::osc_target_src_dot;

enum NodeRole {
    role_none,
    role_input,
    role_osc,
    role_env,
    role_lfo,
    role_output
};

enum OutputKind {
    out_none,
    out_input,
    out_osc,
    out_env,
    out_lfo
};

enum InputKind {
    in_none,
    in_depth,
    in_rate,
    in_sum
};

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

using Sculptor::find_record;

struct PortableNode {
    Synth::InstrumentGraphLayout locator;
    uint8_t                      index;
    uint8_t                      served[Synth::num_mod_targets];
};

static_assert(sizeof(PortableNode) == (sizeof(Synth::InstrumentGraphLayout) + 1 + Synth::num_mod_targets + 3) / 4 * 4);

namespace {
PortableNode                        clipboard_source_nodes[Synth::instrument_graph_layout_capacity];
PortableNode                        clipboard_decoded_nodes[Synth::instrument_graph_layout_capacity];
Synth::InstrumentGraphLayout        clipboard_portable_records[Synth::instrument_graph_layout_capacity];
Synth::GraphNodeLayout              clipboard_mapped_records[Synth::instrument_graph_layout_capacity];
const Synth::InstrumentGraphLayout* clipboard_saved_layout[Synth::instrument_graph_layout_capacity];

Synth::InstrumentEditorBank clipboard_candidate;
Synth::InstrumentBank       clipboard_model_bank;
Synth::GraphNodeLayout      clipboard_candidate_records[Synth::instrument_graph_layout_capacity];
uint16_t                    clipboard_envelope_ids[Synth::instrument_max_envelopes];
uint16_t                    clipboard_lfo_ids[Synth::instrument_max_lfos];
char                        clipboard_destination_name[Synth::max_name_len];

// The projection arithmetic relies on this exact ModTarget ordering.
static_assert(static_cast<uint32_t>(Synth::mod_panning) == 2);
static_assert(static_cast<uint32_t>(Synth::mod_duty0) == 3);
static_assert(static_cast<uint32_t>(Synth::mod_fm_index) == 6);
static_assert(Synth::num_mod_targets == 8);
static_assert(Synth::max_mod_inputs == 2);

const char* const target_names[Synth::num_mod_targets]       = { "volume", "pitch",        "panning",  "Duty A",
                                                                 "Duty B", "Waveform Mix", "FM Depth", "Band spread" };
const char* const wave_names[5]                              = { "off", "sine", "saw", "pulse", "noise" };
const char* const osc_mode_names[3]                          = { "blend", "fm", "hard sync" };
const char* const filter_type_names[Synth::num_filter_types] = { "off", "bandpass", "lowpass", "highpass" };

// Dropdown order puts "off" first.  The stored enum order is fixed: zero must

// The FIR spread row's label, indexed by the stored FilterType value.

const char* const fir_spread_names[Synth::num_filter_types] = { "Filter Off",
                                                                "Band Spread",
                                                                "Cutoff (Hz)",
                                                                "Cutoff (Hz)" };

// Oscillator node slot layout. The order is fixed: project fills slots in
// exactly this order and compile reads properties back by index, so the two
// stay consistent by construction.
// 0 output
// 1 waveform a (list)
// 2 duty a (per-layer value, [0, 1])
// 3..14 duty a's 12-slot modulation block
// 15 waveform b (list)
// 16 duty b (per-layer value, [0, 1])
// 17..28 duty b's 12-slot modulation block
// 29 mix mode (list)
// 30 waveform mix (per-layer value, [0, 1])
// 31..42 waveform mix's 12-slot modulation block
// 43 fm depth (per-layer value)
// 44..55 fm depth's 12-slot modulation block
// 56 fm ratio (per oscillator)
// 57 volume (per-layer base value)
// 58..69 volume's 12-slot modulation block
// 70 pan (per-layer base value)
// 71..82 pan's 12-slot modulation block
// 83 pitch (per-layer base value)
// 84..95 pitch's 12-slot modulation block
// 96 pitch offset (per oscillator)
// 97 octave offset (per oscillator, [-12, +12] whole octaves)
// 98 filter type (per oscillator, list)
// 99 fir pitch spread (per-layer base value; band-pass: [0, 36] semitones, low-pass/high-pass: cutoff in Hz)
// 100..111 fir pitch spread's 12-slot modulation block
// 112 fir slope (per oscillator, [1, 100] percent)
// Every target value row (2, 16, 30, 43, 57, 70, 83, 99) is
// connectable: all eight targets are uniformly modulate-able, so each
// row's 12-slot block follows its value row (see the block helpers in
// sculptor_osc_graph.h); block field 0 is the envelope dot, so it
// renders on the line under the row the block attaches to, and field 1
// is the envelope depth's nested row.
constexpr uint32_t osc_waveform_a_prop    = 1;
constexpr uint32_t osc_waveform_b_prop    = 15;
constexpr uint32_t osc_mode_prop          = 29;
constexpr uint32_t osc_fm_ratio_prop      = 56;
constexpr uint32_t osc_pitch_offset_prop  = 96;
constexpr uint32_t osc_octave_offset_prop = 97;
constexpr uint32_t osc_fir_slope_prop     = 112;
constexpr uint32_t osc_fir_type_prop      = 98;
// Per-target presentation views: the single source of truth for value
// bounds, bank-unit scaling and slider curve.  Oscillator rows and
// parameter base values show display units; the bank stores scaled
// units (radians for fm depth).
constexpr Sculptor::OscTargetView osc_target_views[Synth::num_mod_targets] = {
    { 0.0f, 1.0f, 1.0f, false },       // volume
    { -24.0f, 24.0f, 1.0f, false },    // pitch (semitones)
    { 0.0f, 1.0f, 1.0f, false },       // pan (0.5 = center)
    { 0.0f, 1.0f, 1.0f, false },       // duty a
    { 0.0f, 1.0f, 1.0f, false },       // duty b
    { 0.0f, 1.0f, 1.0f, false },       // waveform mix
    { 0.0f, 1.0f, 6.2831853f, false }, // fm depth (1.0 == 2*pi radians)
    { 0.0f, 36.0f, 1.0f, false },      // fir pitch spread (semitones)
};

// Low-pass and high-pass cutoff slider.  The editor writes a zero cutoff (edge off)
// when a low-pass slider reaches the maximum or a high-pass slider reaches the
// minimum, like the effect FIR sliders.  A zero cutoff stays valid in the JSON.
constexpr float                   osc_fir_cutoff_min_hz = 20.0f;
constexpr float                   osc_fir_cutoff_max_hz = 22050.0f;
constexpr Sculptor::OscTargetView osc_fir_cutoff_view   = { osc_fir_cutoff_min_hz, osc_fir_cutoff_max_hz, 1.0f, true };

// Filter off keeps the old value of either unit, so it accepts both ranges.
constexpr Sculptor::OscTargetView osc_fir_off_view = { 0.0f, osc_fir_cutoff_max_hz, 1.0f, false };

// Envelope node slot layout: 0 output, then descriptor-content properties.

// LFO node slot layout: 0 output, 1 depth source, 2 rate source, then
// descriptor-content properties.
constexpr uint32_t lfo_depth_input = 1;
constexpr uint32_t lfo_rate_input  = 2;

// Output node slot layout: one input per layer.

// Canonical kind-0 numbering is shared with the JSON codec; it lives in
// sculptor_instr_bank.h.  Only the fixed nodes (inputs, sum, oscillators)
// carry canonical indices; generator instances are record-keyed.
constexpr uint32_t canonical_first_osc  = Synth::graph_canonical_first_osc;
constexpr uint32_t canonical_first_env  = Synth::graph_canonical_first_env;
constexpr uint32_t canonical_node_count = Synth::graph_canonical_node_count;
} // namespace

// Oscillator-node rows whose widgets only make sense for the current
// waveform/mode selection.  Slot indices follow the projection layout
// contract in sculptor_osc_graph.cpp (waveform b, mix mode and fm ratio,
// plus the duty b, waveform mix and fm depth rows, need waveform b; the
// cutoff row, its block and the fir slope need a filter type other than off).
bool Sculptor::oscillator_slot_waveform_mode_disabled(const Sculptor::Node& node, uint32_t slot_idx)
{
    if (node.slots.num_allocated < osc_slot_count) {
        return false;
    }
    const uint32_t wave_b = node.slots.entries[osc_waveform_b_prop].value.list_index;
    const uint32_t mode   = node.slots.entries[osc_mode_prop].value.list_index;
    const uint32_t duty1  = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_duty1)];
    const uint32_t mix    = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_osc_mix)];
    const uint32_t fm     = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_fm_index)];
    if (wave_b == 0 && (slot_idx == duty1 || slot_idx == osc_mode_prop || slot_idx == mix || slot_idx == fm ||
                        slot_idx == osc_fm_ratio_prop)) {
        return true; // duty b, mix mode, waveform mix and both fm rows need waveform b
    }
    if (slot_idx == fm && mode != 1) {
        return true; // fm depth only drives the fm mix mode
    }
    if (slot_idx == osc_fm_ratio_prop && mode == 0) {
        return true; // fm ratio drives fm and hard sync, not blend
    }
    const uint32_t fir_type = node.slots.entries[osc_fir_type_prop].value.list_index;
    if (fir_type == static_cast<uint32_t>(Synth::fir_off)) {
        const uint32_t spread = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_fir_spread)];
        if ((slot_idx >= spread && slot_idx < osc_fir_slope_prop) || slot_idx == osc_fir_slope_prop) {
            return true; // the cutoff row, its block and the slope are unused with the filter off
        }
    }
    return false;
}

// Parameter target index space: the ModTarget enumeration (0..8).  Every
// target has an oscillator value row, so there is no
// projected-index mapping of a five-target subset.

static uint32_t osc_target_prop(Synth::ModTarget target)
{
    return osc_target_row_slot[static_cast<uint32_t>(target)];
}

// The ModTarget (0..8) of a target value row, or -1 when the row is not a
// target value row at all.
static int32_t osc_row_target_index(uint32_t slot_idx)
{
    for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
        if (osc_target_prop(static_cast<Synth::ModTarget>(target)) == slot_idx) {
            return static_cast<int32_t>(target);
        }
    }
    return -1;
}

static bool osc_row_is_target(uint32_t slot_idx)
{
    return osc_row_target_index(slot_idx) >= 0;
}

static Sculptor::OscTargetView target_view_for(Synth::ModTarget target, Synth::FilterType fir_type)
{
    if (target == Synth::mod_fir_spread) {
        if (fir_type == Synth::fir_lowpass || fir_type == Synth::fir_highpass) {
            return osc_fir_cutoff_view;
        }
        if (fir_type == Synth::fir_off) {
            return osc_fir_off_view;
        }
    }
    return osc_target_views[static_cast<uint32_t>(target)];
}

// Spread row value in slider units: a zero low-pass cutoff shows at the maximum,
// a zero high-pass cutoff at the minimum.
static float fir_spread_display_value(const Synth::Oscillator& osc)
{
    const float base = osc.targets[Synth::mod_fir_spread].base_value;
    if (base == 0.0f && osc.fir_type == Synth::fir_lowpass) {
        return osc_fir_cutoff_max_hz;
    }
    if (base == 0.0f && osc.fir_type == Synth::fir_highpass) {
        return osc_fir_cutoff_min_hz;
    }
    return base;
}

// Inverse of fir_spread_display_value, with the slider range applied.  A type change keeps the number,
// so the range clamp also converts the old unit.  Filter off keeps the value.
static float fir_spread_bank_value(Synth::FilterType fir_type, float value)
{
    if (fir_type == Synth::fir_lowpass) {
        if (! (value > 0.0f) || value >= osc_fir_cutoff_max_hz) {
            return 0.0f;
        }
        return value < osc_fir_cutoff_min_hz ? osc_fir_cutoff_min_hz : value;
    }
    if (fir_type == Synth::fir_highpass) {
        if (! (value > osc_fir_cutoff_min_hz)) {
            return 0.0f;
        }
        return value > osc_fir_cutoff_max_hz ? osc_fir_cutoff_max_hz : value;
    }
    if (fir_type == Synth::fir_bandpass) {
        return value > 36.0f ? 36.0f : value;
    }
    return value;
}

static Sculptor::Slot osc_view_display_slot(const char*       name,
                                            float             display_value,
                                            Synth::ModTarget  target,
                                            Synth::FilterType fir_type = Synth::fir_bandpass)
{
    const Sculptor::OscTargetView view = target_view_for(target, fir_type);
    Sculptor::Slot                slot = bounded_real_slot(name, display_value, view.min_value, view.max_value);
    slot.real_logarithmic              = view.logarithmic;
    return slot;
}

// Connectable target value row: the layer's own base value, editable
// regardless of wiring; direct modulator wires drive the modulators on
// top (the block's envelope/LFO/Source dots), never the base.
static Sculptor::Slot osc_view_connectable_slot(const char*       name,
                                                float             display_value,
                                                Synth::ModTarget  target,
                                                Synth::FilterType fir_type = Synth::fir_bandpass)
{
    Sculptor::Slot slot = osc_view_display_slot(name, display_value, target, fir_type);
    slot.connectable    = true;
    // The drop-menu anchor: exactly the nine target value rows take
    // creation menus when a drag from the row's dot releases over empty
    // canvas.
    slot.drop_menu = true;
    return slot;
}

// Connection families a target block's rows carry.
enum TargetFamily {
    family_env,
    family_lfo,
    family_src
};

// Adds target `target`'s 12-slot modulation block directly after the
// target's value row (the caller adds the osc node's slots in row
// order), seeded from the cell binding: field 1 carries the envelope
// depth, fields 3..5 the LFO op/depth/rate scale, fields 7..8 and
// 10..11 the MIDI source op/amount pairs.  Op shares its input row.
// Depth, Rate x, and Amount use separate nested rows.  Inputs past
// num_inputs seed the neutral add/0 grammar the compile reads through
// when a wire lands later.
static void append_target_block(Graph* graph, uint32_t node, Synth::ModTarget target, const Synth::TargetBinding& gen)
{
    const uint32_t t       = static_cast<uint32_t>(target);
    Sculptor::Slot env_dot = input_slot("Envelope");
    env_dot.child_row      = true;
    env_dot.row_group      = 3 * t + 4; // the dot shares its line with the depth
    graph->add_slot(node, env_dot);
    Sculptor::Slot env_depth = bounded_real_slot("", gen.envelope_depth, 0.0f, 1.0f, false);
    env_depth.child_row      = true;
    env_depth.row_group      = 3 * t + 4;
    graph->add_slot(node, env_depth);
    Sculptor::append_lfo_row(graph,
                             node,
                             static_cast<uint8_t>(3 * t + 1),
                             static_cast<uint32_t>(gen.lfo_op),
                             gen.lfo_depth,
                             nullptr,
                             nullptr);
    Sculptor::Slot rate_scale = real_slot("Rate x", gen.lfo_rate_scale_ms);
    rate_scale.child_row      = true;
    rate_scale.child_level    = 2;
    graph->add_slot(node, rate_scale);
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        const Synth::SourceOp op    = i < gen.num_inputs ? gen.inputs[i].op : Synth::SourceOp::add;
        const float           scale = i < gen.num_inputs ? gen.inputs[i].scale : 0.0f;
        Sculptor::append_mod_source_row(graph,
                                        node,
                                        i,
                                        static_cast<uint8_t>(3 * t + 2 + i),
                                        static_cast<uint32_t>(op),
                                        scale);
    }
}

// A wire being moved still occupies its old landing dot while the
// drag is in flight: the moving connection does not count as taken,
// so relocating it inside its own block works.
static bool dot_taken_for_move(const Graph& graph, uint32_t node_idx, uint32_t dot_idx)
{
    const uint32_t conn = connection_into(graph, node_idx, dot_idx);
    return conn != pool_no_slot && conn != graph.moving_connection;
}

// The first free input dot of target `target`'s block for connection
// family `family`: pool_no_slot when the family has no free input.  A
// base-row drop resolves its landing dot here; the source family fills
// Source A before Source B.
static uint32_t osc_target_free_input(const Sculptor::Graph& graph, uint32_t node_idx, uint32_t target, uint32_t family)
{
    if (family == family_env) {
        const uint32_t dot = osc_target_env_dot(target);
        return dot_taken_for_move(graph, node_idx, dot) ? pool_no_slot : dot;
    }
    if (family == family_lfo) {
        const uint32_t dot = osc_target_lfo_dot(target);
        return dot_taken_for_move(graph, node_idx, dot) ? pool_no_slot : dot;
    }
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        const uint32_t dot = osc_target_src_dot(target, i);
        if (! dot_taken_for_move(graph, node_idx, dot)) {
            return dot;
        }
    }
    return pool_no_slot;
}

// Decodes an oscillator-node input endpoint: `target` receives the
// ModTarget index, `family` the row family, `dot` the specific input dot
// (pool_no_slot for the base value row, the standing drop target), and
// `base_row` reports the base row.  Returns false for endpoints that are
// not target-block inputs.
static NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx);

static bool decode_osc_input(const Sculptor::OscGraphMapping& mapping,
                             Sculptor::EndPoint               point,
                             bool*                            base_row,
                             uint32_t*                        target,
                             uint32_t*                        family,
                             uint32_t*                        dot)
{
    if (node_role(mapping, point.node_idx) != role_osc) {
        return false;
    }

    *base_row                = false;
    *dot                     = pool_no_slot;
    const int32_t row_target = osc_row_target_index(point.slot_idx);
    if (row_target >= 0) {
        *base_row = true;
        *target   = static_cast<uint32_t>(row_target);
        *family   = family_src; // decided by the wire's output end
        return true;
    }

    uint32_t block_target = 0;
    uint32_t block_field  = 0;
    if (! osc_target_block_field(point.slot_idx, &block_target, &block_field)) {
        return false;
    }

    *target = block_target;
    if (block_field == 0) {
        *family = family_env;
        *dot    = osc_target_env_dot(*target);
        return true;
    }
    if (block_field == 2) {
        *family = family_lfo;
        *dot    = osc_target_lfo_dot(*target);
        return true;
    }
    if (block_field == 6 || block_field == 9) {
        *family = family_src;
        *dot    = osc_target_src_dot(*target, block_field == 6 ? 0 : 1);
        return true;
    }
    return false;
}

static NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    if (mapping.input_node == node_idx) {
        return role_input;
    }
    if (mapping.instrument_node == node_idx) {
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
    // Generator instances resolve through the registry, so they validate on
    // connect and their content edits route into the descriptor-edit path.
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].node_idx == node_idx) {
            return mapping.detached[i].kind == 1 ? role_env : role_lfo;
        }
    }
    return role_none;
}

static int32_t find_instance(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
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

static OutputKind classify_output(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
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
        default:
            return out_none;
    }
}

static InputKind classify_input(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
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
static bool edge_source(const Sculptor::Graph&           graph,
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

// Resolves an inputs-node output slot to its assignable CC index; -1 when
// the slot is not a projected CC dot.
static int32_t input_cc_index(const Sculptor::OscGraphMapping& mapping, uint32_t slot_idx)
{
    if (slot_idx == Sculptor::pool_no_slot) {
        return -1;
    }

    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        if (mapping.input_cc_slots[i] == slot_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Resolves the assignable CC slot index feeding a connector through the
// inputs node's CC dots; -1 when the connection does not leave a CC dot.
static int32_t edge_cc_index(const Sculptor::Graph&           graph,
                             const Sculptor::OscGraphMapping& mapping,
                             uint32_t                         connection_idx)
{
    const uint32_t node_idx = graph.get_connection(connection_idx).output.node_idx;
    if (node_idx != mapping.input_node) {
        return -1;
    }
    return input_cc_index(mapping, graph.get_connection(connection_idx).output.slot_idx);
}

static bool read_list_prop(const Sculptor::Slot& slot, uint32_t* index)
{
    if (slot.property_type != Sculptor::PropertyType::list || slot.value.list_index >= slot.num_list_options) {
        return false;
    }
    *index = slot.value.list_index;
    return true;
}

static void clear_mapping(Sculptor::OscGraphMapping* mapping)
{
    mapping->source_bank = nullptr;
    mapping->input_node  = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        mapping->input_source_slots[i] = Sculptor::pool_no_slot;
    }
    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        mapping->input_cc_slots[i] = Sculptor::pool_no_slot;
    }
    mapping->output_node = Sculptor::pool_no_slot;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        mapping->output_layer_input_slot[layer] = Sculptor::pool_no_slot;
        mapping->osc_nodes[layer]               = Sculptor::pool_no_slot;
    }
    mapping->osc_output_slot = Sculptor::pool_no_slot;
    mapping->instrument_node = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < 3; ++i) {
        mapping->instrument_slots[i] = Sculptor::pool_no_slot;
    }
    // Uniform node layouts; populated even when no such node exists so the
    // mapping always describes the full projection layout.
    mapping->env_output_slot      = 0;
    mapping->lfo_output_slot      = 0;
    mapping->lfo_depth_input_slot = lfo_depth_input;
    mapping->lfo_rate_input_slot  = lfo_rate_input;
    mapping->channel              = 0;
    mapping->zone                 = 0;
    for (uint32_t i = 0; i < Sculptor::max_osc_instances; ++i) {
        mapping->detached[i].node_idx     = Sculptor::pool_no_slot;
        mapping->detached[i].kind         = 0;
        mapping->detached[i].desc_id      = 0;
        mapping->detached[i].uid          = 0;
        mapping->detached[i].depth_source = 0;
        mapping->detached[i].rate_source  = 0;
    }
    mapping->detached_count = 0;
}

static void project_osc_node(Sculptor::Graph*           graph,
                             uint32_t                   layer,
                             const Synth::Oscillator&   osc,
                             Sculptor::OscGraphMapping* mapping)
{
    char name[32];
    snprintf(name, sizeof(name), "Oscillator %u", layer + 1);
    const uint32_t node = graph->create_node(name, vmath::vec2(512.0f, 256.0f * static_cast<float>(layer)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    mapping->osc_nodes[layer] = node;

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
        osc_view_connectable_slot(target_names[3], osc.targets[Synth::mod_duty0].base_value, Synth::mod_duty0));
    // One 11-slot modulation block per ModTarget, directly after the
    // target's value row, seeded from the layer's own binding
    // (display-unit fields carry the raw values; only the base rows are
    // scaled).
    append_target_block(graph, node, Synth::mod_duty0, osc.targets[Synth::mod_duty0]);
    graph->add_slot(node, list_slot("Waveform B", wave_names, 5, static_cast<uint32_t>(osc.osc_type[1])));
    graph->add_slot(
        node,
        osc_view_connectable_slot(target_names[4], osc.targets[Synth::mod_duty1].base_value, Synth::mod_duty1));
    append_target_block(graph, node, Synth::mod_duty1, osc.targets[Synth::mod_duty1]);
    graph->add_slot(node, list_slot("Mix Mode", osc_mode_names, 3, static_cast<uint32_t>(osc.osc_mode)));
    graph->add_slot(
        node,
        osc_view_connectable_slot(target_names[5], osc.targets[Synth::mod_osc_mix].base_value, Synth::mod_osc_mix));
    append_target_block(graph, node, Synth::mod_osc_mix, osc.targets[Synth::mod_osc_mix]);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_names[6],
                                              osc.targets[Synth::mod_fm_index].base_value /
                                                  osc_target_views[Synth::mod_fm_index].bank_scale,
                                              Synth::mod_fm_index));
    append_target_block(graph, node, Synth::mod_fm_index, osc.targets[Synth::mod_fm_index]);
    graph->add_slot(
        node,
        bounded_real_slot("FM Ratio", osc.mod_ratio, Sculptor::osc_fm_ratio_min, Sculptor::osc_fm_ratio_max));
    graph->add_slot(node,
                    osc_view_connectable_slot("Volume", osc.targets[Synth::mod_volume].base_value, Synth::mod_volume));
    append_target_block(graph, node, Synth::mod_volume, osc.targets[Synth::mod_volume]);
    graph->add_slot(node,
                    osc_view_connectable_slot("Pan", osc.targets[Synth::mod_panning].base_value, Synth::mod_panning));
    append_target_block(graph, node, Synth::mod_panning, osc.targets[Synth::mod_panning]);
    graph->add_slot(node,
                    osc_view_connectable_slot("Pitch", osc.targets[Synth::mod_pitch].base_value, Synth::mod_pitch));
    append_target_block(graph, node, Synth::mod_pitch, osc.targets[Synth::mod_pitch]);
    // A bounded bare input box: the renderer clamps edits to the stored
    // range without rendering a slider (the saved-bank decode rejects
    // out-of-range values, so the GUI must never produce one).  Convention:
    // populated min/max on an unbounded real slot (real_bounded false) means
    // clamp-on-edit; the renderer's guard is min < max.
    Sculptor::Slot pitch_offset_slot = Sculptor::real_slot("Pitch Offset", osc.pitch_offset);
    pitch_offset_slot.real_min       = Sculptor::osc_pitch_offset_min;
    pitch_offset_slot.real_max       = Sculptor::osc_pitch_offset_max;
    graph->add_slot(node, pitch_offset_slot);
    graph->add_slot(node,
                    Sculptor::bounded_int_slot("Octave Offset",
                                               osc.octave_offset,
                                               Sculptor::osc_octave_offset_min,
                                               Sculptor::osc_octave_offset_max));
    graph->add_slot(
        node,
        list_slot("Filter Type", filter_type_names, Synth::num_filter_types, static_cast<uint32_t>(osc.fir_type)));
    graph->add_slot(node,
                    osc_view_connectable_slot(fir_spread_names[osc.fir_type],
                                              fir_spread_display_value(osc),
                                              Synth::mod_fir_spread,
                                              osc.fir_type));
    append_target_block(graph, node, Synth::mod_fir_spread, osc.targets[Synth::mod_fir_spread]);
    graph->add_slot(node, Sculptor::bounded_int_slot("FIR Slope", osc.fir_slope_percent, 1, 100));
}

// Node construction shared by the derived projection and record-driven
// creation, so the slot layouts cannot drift.  Slot 0 is always the output;
// env content follows, then for LFOs the depth/rate inputs.
static uint32_t create_env_node(Sculptor::Graph*                 graph,
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
    graph->add_slot(node, Sculptor::toggle_slot("Ping-Pong", env.ping_pong != 0));
    // Point and sustain editing lives in the curve state widget; property
    // slots for those fields would be a second stale UI over the same data.
    return node;
}

static uint32_t create_lfo_node(Sculptor::Graph*            graph,
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
    graph->add_slot(node, list_slot("Waveform", lfo_wave_names, 2, lfo.wave == Synth::WaveType::sine_wave ? 0 : 1));
    // Duty shapes sawtooth only: sine has no phase to widen.
    const uint32_t duty_idx =
        graph->add_slot(node, bounded_real_slot("Duty", static_cast<float>(lfo.duty) / 255.0f, 0.0f, 1.0f));
    graph->set_slot_edit_disabled(node, duty_idx, lfo.wave == Synth::WaveType::sine_wave);
    graph->add_slot(node, int_slot("Period (ms)", lfo.period_ms));
    return node;
}

// Appends a derivation key unless an identical key is already listed;
// the keys array lists each distinct instance once.
static void append_instance_key(InstanceKey* keys, uint32_t& count, const InstanceKey& key)
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

// Deterministic derivation: one key per distinct (kind, descriptor,
// sources) tuple over the projected rows, in target order then row
// order: the deterministic instance numbering the projection and the
// node-count check share.  A binding's envelope key comes before its
// LFO key.  rows[row] names the oscillator the row projects; the walk
// covers rows 0..run_length - 1.
static uint32_t derive_row_instance_keys(const Synth::Oscillator* const* rows, uint32_t run_length, InstanceKey* keys)
{
    uint32_t count = 0;
    for (uint32_t target_idx = 0; target_idx < Synth::num_mod_targets; ++target_idx) {
        for (uint32_t row = 0; row < run_length; ++row) {
            const Synth::TargetBinding& gen = rows[row]->targets[target_idx];
            if (gen.envelope_desc_id != 0) {
                const InstanceKey key = { 1, static_cast<uint8_t>(gen.envelope_desc_id), 0, 0 };
                append_instance_key(keys, count, key);
            }
            if (gen.lfo_desc_id != 0) {
                const InstanceKey key = { 2,
                                          static_cast<uint8_t>(gen.lfo_desc_id),
                                          static_cast<uint8_t>(gen.lfo_depth_source),
                                          static_cast<uint8_t>(gen.lfo_rate_source) };
                append_instance_key(keys, count, key);
            }
        }
    }
    return count;
}

static uint32_t derive_instance_keys(const Synth::Instrument& instrument, InstanceKey* keys)
{
    const Synth::Oscillator* rows[Synth::max_layers] = {};
    uint32_t                 run_length              = 0;
    const uint32_t           layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    for (; run_length < layer_count; ++run_length) {
        rows[run_length] = &instrument.layers[run_length];
    }
    return derive_row_instance_keys(rows, run_length, keys);
}

// Wires one target block's source rows from the cell binding's MIDI
// inputs.  Standard sources resolve through the input node's source
// slots; CC sources through the assignable-CC dots, where an absent dot,
// or a cc_number outside the assignable range, leaves the row unwired -
// decode never synthesizes registry bits.  Skips the none source and the
// one-past-the-end sentinel slot.  Returns false when a wire cannot be
// added: a silently dropped source wire loses the decoded MIDI routing
// at the next compile.
static bool wire_target_sources(Sculptor::Graph*                 graph,
                                const Sculptor::OscGraphMapping& mapping,
                                uint32_t                         osc_node_idx,
                                uint32_t                         target,
                                const Synth::ModInput*           inputs,
                                uint32_t                         num_inputs)
{
    if (num_inputs > Synth::max_mod_inputs) {
        num_inputs = Synth::max_mod_inputs;
    }
    for (uint32_t i = 0; i < num_inputs; ++i) {
        if (inputs[i].source == Synth::ModSource::cc) {
            const uint32_t cc_number = inputs[i].cc_number;
            if (cc_number < Synth::cc_msb_base || cc_number - Synth::cc_msb_base >= Synth::num_assignable_ccs) {
                continue;
            }
            const uint32_t cc_index = cc_number - Synth::cc_msb_base;
            if (mapping.input_cc_slots[cc_index] == Sculptor::pool_no_slot) {
                continue;
            }
            if (graph->add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_cc_slots[cc_index] },
                                      Sculptor::EndPoint{ osc_node_idx, osc_target_src_dot(target, i) }) ==
                Sculptor::pool_no_slot) {
                return false;
            }
            continue;
        }
        const uint32_t source = static_cast<uint32_t>(inputs[i].source);
        if (source == 0 || source > Sculptor::num_osc_graph_inputs) {
            continue;
        }
        if (graph->add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_source_slots[source - 1] },
                                  Sculptor::EndPoint{ osc_node_idx, osc_target_src_dot(target, i) }) ==
            Sculptor::pool_no_slot) {
            return false;
        }
    }
    return true;
}

// A target block's LFO and source-row knobs shape the signal arriving on
// that row's input; with no input connected they change nothing.  Runs
// over every projected oscillator node: the projection calls it after a
// rebuild and the apply path after an eventless relocation.
static void install_edit_disabled_flags(Sculptor::Graph* graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        const uint32_t node = mapping.osc_nodes[layer];
        if (node == Sculptor::pool_no_slot || ! graph->node_occupied(node)) {
            continue;
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const bool env_wired = graph->slot_is_connected(node, osc_target_env_dot(t));
            graph->set_slot_edit_disabled(node, osc_target_env_depth(t), ! env_wired);
            const bool lfo_wired = graph->slot_is_connected(node, osc_target_lfo_dot(t));
            for (uint32_t f = 3; f <= 5; ++f) {
                graph->set_slot_edit_disabled(node, osc_target_block_base(t) + f, ! lfo_wired);
            }
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                const uint32_t dot       = osc_target_src_dot(t, i);
                const bool     src_wired = graph->slot_is_connected(node, dot);
                graph->set_slot_edit_disabled(node, dot + 1, ! src_wired);
                graph->set_slot_edit_disabled(node, dot + 2, ! src_wired);
            }
        }
    }
}

// Editor-facing helpers exported from sculptor_osc_graph.h.  Defined at
// file scope with explicit prefixes; the file-local sections above and below
// reach them through using-declarations.

bool Sculptor::env_volume_shape_ok(const Synth::EnvelopeDescriptor& env)
{
    // The span must be nonnegative too: a negative delta would evaluate the
    // interior points below the zero floor.
    // Tail values remain authored (a sustain release may end above 0), and
    // the first point's value is authored like every other point: the
    // runtime ramps volume from silence at note-on.
    return env.min_value == 0.0f && env.min_max_delta >= 0.0f;
}

// Forces the volume-envelope shape: minimum 0 with the effective top preserved;
// raw point values stay authored.  Returns false when the effective top
// is negative, which cannot be preserved under the nonnegative volume range.
// The drop-commit preset converts a freshly allocated descriptor before the
// commit, so a volume binding never lands in the bipolar default shape the
// apply path would have to convert (or the validator refuse).
bool Sculptor::env_convert_to_volume_shape(Synth::EnvelopeDescriptor& env)
{
    const float max_value = env.min_value + 65535.0f * env.min_max_delta;
    if (max_value < 0.0f) {
        return false;
    }
    env.min_value     = 0.0f;
    env.min_max_delta = max_value / 65535.0f;
    return true;
}

// Per-family modulator-row visibility for the target's block: the family
// rows render only while their family dot carries a wire (the fx
// param_field pattern).  The base row never hides: it is always
// connectable and always renders its value.
bool Sculptor::osc_target_field_hidden(const Graph& graph, uint32_t node_idx, uint32_t target, uint32_t field)
{
    if (field <= 1) {
        return connection_into(graph, node_idx, osc_target_env_dot(target)) == pool_no_slot;
    }
    if (field <= 5) {
        return connection_into(graph, node_idx, osc_target_lfo_dot(target)) == pool_no_slot;
    }
    const uint32_t src = field < 9 ? 0 : 1;
    return connection_into(graph, node_idx, osc_target_src_dot(target, src)) == pool_no_slot;
}

// The gray add dot's row on the Oscillator Output node: one row
// behind the last connected row of the run, or the run's length when
// nothing is connected (every run row renders red).  The visibility
// predicate, the projection's drop flags and the gray-dot color seam
// share this one row.
uint32_t Sculptor::osc_row_gray_slot(uint32_t run_length, uint8_t missing_bits)
{
    for (uint32_t row = run_length; row > 0; --row) {
        if (! Sculptor::osc_row_is_red(missing_bits, row - 1)) {
            return row;
        }
    }
    return run_length;
}

bool Sculptor::osc_layer_slot_hidden(uint32_t slot_idx, const OscGraphMapping& mapping, uint8_t missing_bits)
{
    // The projected run is the number of consecutively projected oscillator
    // nodes; the projection compacts layers, so the first no-slot entry ends
    // the run.
    uint32_t run_length = 0;
    while (run_length < Synth::max_layers && mapping.osc_nodes[run_length] != pool_no_slot) {
        ++run_length;
    }
    // The visible rows extend to the gray slot (one row behind the last
    // connected row, or the run's length when nothing is connected); the
    // trailing shells past it and every row above hide.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.output_layer_input_slot[layer] == slot_idx) {
            return layer > Sculptor::osc_row_gray_slot(run_length, missing_bits);
        }
    }
    return false; // a non-layer slot index, including the no-slot sentinel
}

void Sculptor::env_target_usage(const Graph&           graph,
                                const OscGraphMapping& mapping,
                                uint16_t               desc_id,
                                bool*                  volume_used,
                                bool*                  other_used,
                                uint32_t               exclude_connection)
{
    *volume_used = false;
    *other_used  = false;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        const uint32_t node = mapping.osc_nodes[layer];
        if (node == pool_no_slot || ! graph.node_occupied(node)) {
            continue;
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const uint32_t env_conn = connection_into(graph, node, osc_target_env_dot(t));
            if (env_conn == pool_no_slot || env_conn == exclude_connection) {
                continue;
            }
            const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
            if (inst < 0 || mapping.detached[inst].kind != 1 || mapping.detached[inst].desc_id != desc_id) {
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
    // Descriptor ids live in a bank-wide pool, so users in other instruments
    // and zones count too; the projection records the bank it read.  The
    // projected instrument is skipped: the graph scan above is authoritative
    // for it (it reflects pending edits, which the committed bank alone
    // cannot attribute).
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
                    if (instrument.layers[layer].targets[t].envelope_desc_id != desc_id) {
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

Sculptor::OscTargetView Sculptor::osc_target_view(Synth::ModTarget target, Synth::FilterType fir_type)
{
    return target_view_for(target, fir_type);
}

bool Sculptor::osc_change_restructures(const Sculptor::OscGraphMapping& mapping, const Sculptor::GraphChange& change)
{
    return change.kind == Sculptor::ChangeKind::value_changed && change.slot_idx == osc_fir_type_prop &&
           node_role(mapping, change.node_idx) == role_osc;
}

// Slot index on an oscillator node holding the value row of ModTarget
// `target` (0..8); rows are not contiguous.
uint32_t Sculptor::osc_target_row(uint32_t target)
{
    return osc_target_prop(static_cast<Synth::ModTarget>(target));
}

// Decodes an oscillator-node block slot into its (target, field): false
// when the slot is one of the fixed slots (a property or a target value
// row) or past the last block.
bool Sculptor::osc_target_block_field(uint32_t slot_idx, uint32_t* target, uint32_t* field)
{
    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const uint32_t base = osc_target_block_base(t);
        if (slot_idx >= base && slot_idx < base + osc_target_block_stride) {
            *target = t;
            *field  = slot_idx - base;
            return true;
        }
    }

    return false;
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

bool Sculptor::project_instrument_to_graph(const Synth::Instrument&     instrument,
                                           const Synth::InstrumentBank& bank,
                                           Graph*                       graph,
                                           OscGraphMapping*             mapping,
                                           const uint16_t*              added_cc_mask,
                                           const uint8_t*               missing_sum_mask,
                                           const Synth::Oscillator*     phantom_layers)
{
    // The graph cannot express a sourceless routing input: dropping it would
    // change synthesis on commit (the eval multiplies by the zero sentinel),
    // so refuse visibly instead of compacting silently.  Checked before any
    // state is touched; the refusal is reported through the error overlay.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::TargetBinding& binding = instrument.layers[layer].targets[t];
            const uint32_t              num_inputs =
                binding.num_inputs <= Synth::max_mod_inputs ? binding.num_inputs : Synth::max_mod_inputs;
            for (uint32_t i = 0; i < num_inputs; ++i) {
                if (binding.inputs[i].source == Synth::ModSource::none) {
                    graph->set_error("Cannot project: sourceless modulation input");
                    return false;
                }
            }
        }
    }
    clear_mapping(mapping);
    mapping->source_bank = &bank;
    graph->clear();

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    const uint32_t input_node = graph->create_node("MIDI Inputs", vmath::vec2(0.0f, 0.0f));
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
        const uint32_t source = static_cast<uint32_t>(input_display_order[i]);
        mapping->input_source_slots[source - 1] =
            graph->add_slot(input_node, output_slot(mod_source_names[source - 1]));
    }
    if (added_cc_mask) {
        for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
            if (! (*added_cc_mask & (1u << i))) {
                continue;
            }
            char label[32];
            Sculptor::fx_cc_pair_label(Synth::cc_msb_base + i, label, sizeof(label));
            mapping->input_cc_slots[i] = graph->add_slot(input_node, output_slot(label));
        }
    }
    // The note/layer detune and release rows live on a dedicated node
    // beside MIDI Inputs, so they survive CC-dot changes and stay
    // editable as one group.
    const uint32_t instrument_node = graph->create_node("Instrument Parameters", vmath::vec2(0.0f, 512.0f));
    if (instrument_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->instrument_node = instrument_node;
    // Random pitch offset drawn once per note for every layer, in semitones.
    mapping->instrument_slots[0] =
        graph->add_slot(instrument_node,
                        bounded_real_slot("Random Note Detune", instrument.note_skew_semitones, 0.0f, 1.0f));
    // Independent random pitch offset drawn per layer per note, in semitones.
    mapping->instrument_slots[1] =
        graph->add_slot(instrument_node,
                        bounded_real_slot("Random Oscillator Detune", instrument.layer_skew_semitones, 0.0f, 1.0f));
    mapping->instrument_slots[2] =
        graph->add_slot(instrument_node, bounded_real_slot("Release (s)", instrument.release_seconds, 0.0f, 30.0f));

    // The zone's projected row run: a red-marked row hosts its phantom
    // shell (the resolved entry the caller passes; with no phantom pointer
    // a fresh default shell) and the connected layers compact around the
    // red rows in order - the compiled instrument's i-th layer is the
    // i-th connected row, so the projection and the compile agree through
    // a full round trip.  The run ends at the first row that is neither
    // red-marked nor carrying a remaining connected layer: the free row.
    Synth::Oscillator fresh_shell = {};
    // Waveform A cannot be off, so an empty shell starts on sine.
    fresh_shell.osc_type[0]                          = Synth::WaveType::sine_wave;
    const uint8_t            missing_bits            = missing_sum_mask != nullptr ? *missing_sum_mask : 0;
    const uint32_t           run_length              = Sculptor::osc_row_run_length(layer_count, missing_bits);
    const Synth::Oscillator* rows[Synth::max_layers] = {};
    uint32_t                 connected               = 0;
    for (uint32_t row = 0; row < run_length; ++row) {
        rows[row] = Sculptor::osc_row_is_red(missing_bits, row)
                        ? (phantom_layers != nullptr ? &phantom_layers[row] : &fresh_shell)
                        : &instrument.layers[connected++];
    }
    const uint32_t output_node = graph->create_node("Oscillator Output", vmath::vec2(1024.0f, 0.0f));
    if (output_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->output_node = output_node;
    // The gray add slot (one row behind the last connected row, or the
    // run's length when nothing is connected) anchors the drop flags.
    const uint32_t gray_row = Sculptor::osc_row_gray_slot(run_length, missing_bits);
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        char name[32];
        snprintf(name, sizeof(name), "Layer %u", layer + 1);
        Sculptor::Slot slot = input_slot(name);
        // The drop-menu anchors: the red-marked (missing-sum) rows below
        // the gray slot keep their add dots, and the gray slot itself
        // offers the free dot.  The trailing shells past the gray slot and
        // every row above it offer nothing.
        // Occupied, unmarked rows stay unflagged.
        slot.drop_menu = layer == gray_row || (layer < gray_row && Sculptor::osc_row_is_red(missing_bits, layer));
        mapping->output_layer_input_slot[layer] = graph->add_slot(output_node, slot);
    }

    for (uint32_t row = 0; row < run_length; ++row) {
        project_osc_node(graph, row, *rows[row], mapping);
        if (mapping->osc_nodes[row] == Sculptor::pool_no_slot) {
            return false;
        }
    }

    // Deterministic derivation: one key per distinct (kind, descriptor,
    // sources) tuple over the projected rows (phantom shells included, so
    // a shell's bindings re-derive on re-projection), then the
    // envelope/LFO instance nodes.
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_row_instance_keys(rows, run_length, keys);
    uint32_t       env_seq   = 0;
    uint32_t       lfo_seq   = 0;
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
        if (mapping->detached_count >= max_osc_instances) {
            graph->set_error("Cannot project: too many generator instances");
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

    for (uint32_t row = 0; row < run_length; ++row) {
        const uint32_t osc_node = mapping->osc_nodes[row];
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::TargetBinding& gen = rows[row]->targets[t];
            if (gen.envelope_desc_id != 0) {
                uint32_t best = Sculptor::pool_no_slot;
                for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                    if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 1 &&
                        mapping->detached[i].desc_id == gen.envelope_desc_id) {
                        best = i;
                    }
                }
                if (best != Sculptor::pool_no_slot &&
                    graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->env_output_slot },
                                          EndPoint{ osc_node, osc_target_env_dot(t) }) == Sculptor::pool_no_slot) {
                    // A dropped wire would silently compile the cell base-only
                    // over the bank's stored modulators.
                    return false;
                }
            }
            if (gen.lfo_desc_id != 0) {
                uint32_t best = Sculptor::pool_no_slot;
                for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                    if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 2 &&
                        mapping->detached[i].desc_id == gen.lfo_desc_id &&
                        mapping->detached[i].depth_source == static_cast<uint8_t>(gen.lfo_depth_source) &&
                        mapping->detached[i].rate_source == static_cast<uint8_t>(gen.lfo_rate_source)) {
                        best = i;
                    }
                }
                if (best != Sculptor::pool_no_slot &&
                    graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->lfo_output_slot },
                                          EndPoint{ osc_node, osc_target_lfo_dot(t) }) == Sculptor::pool_no_slot) {
                    return false;
                }
            }
            if (gen.num_inputs != 0 &&
                ! wire_target_sources(graph, *mapping, osc_node, t, gen.inputs, gen.num_inputs)) {
                return false;
            }
        }
    }

    // The LFO instances' depth/rate inputs keep their MIDI-source wires.
    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const DetachedNode& entry = mapping->detached[i];
        if (entry.kind != 2) {
            continue;
        }
        if (entry.depth_source != 0 &&
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.depth_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_depth_input_slot }) ==
                Sculptor::pool_no_slot) {
            return false;
        }
        if (entry.rate_source != 0 &&
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.rate_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_rate_input_slot }) == Sculptor::pool_no_slot) {
            return false;
        }
    }

    // Structural hard connections: every connected oscillator feeds its
    // own sum input row; a red-marked row's phantom shell stays unwired.
    for (uint32_t row = 0; row < run_length; ++row) {
        if (missing_sum_mask != nullptr && (*missing_sum_mask & (1u << row)) != 0) {
            continue;
        }
        if (graph->add_connection(EndPoint{ mapping->osc_nodes[row], mapping->osc_output_slot },
                                  EndPoint{ mapping->output_node, mapping->output_layer_input_slot[row] }) ==
            Sculptor::pool_no_slot) {
            return false;
        }
    }
    return true;
}

// Reads one oscillator layer back from its projected node's slots: the
// inverse of project_osc_node's slot layout.  The compile reads one layer
// per connected row, and the phantom capture stores the layer a
// disconnection removes from the compiled instrument.
static bool read_oscillator_layer(const Sculptor::Graph&           graph,
                                  const Sculptor::OscGraphMapping& mapping,
                                  uint32_t                         layer,
                                  Synth::Oscillator*               osc_out)
{
    const Sculptor::Node& node = graph.node(mapping.osc_nodes[layer]);
    if (node.slots.num_allocated < osc_slot_count) {
        return false;
    }

    const Sculptor::Slot* slots = node.slots.entries;
    Synth::Oscillator&    osc   = *osc_out;

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
    osc.osc_mode         = static_cast<Synth::OscMode>(index);
    osc.mod_ratio        = slots[osc_fm_ratio_prop].value.real;
    osc.pitch_offset     = slots[osc_pitch_offset_prop].value.real;
    const int32_t octave = slots[osc_octave_offset_prop].value.integer;
    if (octave < Sculptor::osc_octave_offset_min || octave > Sculptor::osc_octave_offset_max) {
        return false;
    }
    osc.octave_offset   = static_cast<int8_t>(octave);
    const int32_t slope = slots[osc_fir_slope_prop].value.integer;
    if (slope < 1 || slope > 100) {
        return false;
    }
    osc.fir_slope_percent = static_cast<uint8_t>(slope);

    if (! read_list_prop(slots[osc_fir_type_prop], &index)) {
        return false;
    }
    osc.fir_type = static_cast<Synth::FilterType>(index);

    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
        // Each layer owns its cell: the base value always comes from the
        // layer's own row (display units scaled to bank units); the
        // block rows wired to it contribute the modulators only.  A
        // family dot with no wire leaves the family's fields dormant
        // (they compile to zero), so rewiring rebinds without
        // re-projection.
        Synth::TargetBinding gen = {};
        gen.base_value           = slots[osc_target_prop(target)].value.real * osc_target_views[t].bank_scale;
        if (target == Synth::mod_fir_spread) {
            gen.base_value = fir_spread_bank_value(osc.fir_type, gen.base_value);
        }
        // A connection still ending on the base target row is a
        // validator/apply invariant violation: the apply path relocates
        // every committed wire onto its family dot before this runs.
        if (connection_into(graph, mapping.osc_nodes[layer], osc_target_prop(target)) != pool_no_slot) {
            return false;
        }
        const uint32_t env_conn = connection_into(graph, mapping.osc_nodes[layer], osc_target_env_dot(t));
        // The depth slot mirrors the binding whether or not an envelope is
        // wired, so the projection round trip is bit-identical even for the
        // dormant field.
        gen.envelope_depth = slots[osc_target_env_depth(t)].value.real;
        if (env_conn != pool_no_slot) {
            const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
            if (inst < 0 || mapping.detached[inst].kind != 1) {
                return false;
            }
            gen.envelope_desc_id = mapping.detached[inst].desc_id;
        }
        const uint32_t lfo_conn = connection_into(graph, mapping.osc_nodes[layer], osc_target_lfo_dot(t));
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
            if (depth_conn != pool_no_slot && ! edge_source(graph, mapping, depth_conn, &gen.lfo_depth_source)) {
                return false;
            }
            const uint32_t rate_conn =
                connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_rate_input_slot);
            if (rate_conn != pool_no_slot && ! edge_source(graph, mapping, rate_conn, &gen.lfo_rate_source)) {
                return false;
            }
            uint32_t lfo_index = 0;
            if (! read_list_prop(slots[osc_target_lfo_dot(t) + 1], &lfo_index)) {
                return false;
            }
            gen.lfo_op            = static_cast<Synth::SourceOp>(lfo_index);
            gen.lfo_depth         = slots[osc_target_lfo_dot(t) + 2].value.real;
            gen.lfo_rate_scale_ms = slots[osc_target_lfo_dot(t) + 3].value.real;
        }
        uint32_t num_inputs = 0;
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            const uint32_t dot  = osc_target_src_dot(t, i);
            const uint32_t conn = connection_into(graph, mapping.osc_nodes[layer], dot);
            if (conn == pool_no_slot) {
                continue;
            }
            const int32_t    cc_index = edge_cc_index(graph, mapping, conn);
            Synth::ModSource source   = Synth::ModSource::none;
            if (cc_index < 0 && ! edge_source(graph, mapping, conn, &source)) {
                return false;
            }
            if (node.slots.num_allocated <= dot + 2) {
                return false;
            }
            uint32_t op_index = 0;
            if (! read_list_prop(slots[dot + 1], &op_index)) {
                return false;
            }
            Synth::ModInput& input = gen.inputs[num_inputs++];
            if (cc_index >= 0) {
                // A CC wire compiles to the assignable cc source; the
                // validate_instrument 16..31 rule protects the runtime
                // resolve_source slot assert.
                input.source    = Synth::ModSource::cc;
                input.cc_number = static_cast<uint8_t>(Synth::cc_msb_base + static_cast<uint32_t>(cc_index));
            }
            else {
                input.source = source;
            }
            input.op    = static_cast<Synth::SourceOp>(op_index);
            input.scale = slots[dot + 2].value.real;
        }
        gen.num_inputs = static_cast<uint16_t>(num_inputs);
        osc.targets[t] = gen;
    }
    return true;
}

// The phantom store's pool helpers: entries are kept compacted in array
// order (a stable order the JSON codec and the zone operations rely on),
// one per (channel, zone, layer) key.

// Index of the (channel, zone, layer) entry, or -1 when the row has none.
int32_t Sculptor::find_phantom_entry(const Synth::InstrumentEditorBank& bank,
                                     uint32_t                           channel,
                                     uint32_t                           zone,
                                     uint32_t                           layer)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel || layer >= Synth::max_layers) {
        return -1;
    }
    for (uint32_t i = 0; i < bank.phantom_count; ++i) {
        const Synth::PhantomOscillator& entry = bank.phantoms[i];
        if (entry.channel == channel && entry.zone == zone && entry.layer == layer) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Removes the entry at index and closes the gap, preserving order.
static void remove_phantom_entry_at(Synth::InstrumentEditorBank* bank, uint32_t index)
{
    if (index >= bank->phantom_count) {
        return;
    }
    memmove(&bank->phantoms[index],
            &bank->phantoms[index + 1],
            (bank->phantom_count - index - 1) * sizeof(bank->phantoms[0]));
    bank->phantom_count--;
}

// Writes one zone-layer's entry: the entry updates in place when it
// exists, otherwise it appends.  Returns false when appending would
// exceed the store's fixed capacity.
bool Sculptor::set_phantom_entry(Synth::InstrumentEditorBank* bank,
                                 uint32_t                     channel,
                                 uint32_t                     zone,
                                 uint32_t                     layer,
                                 const Synth::Oscillator&     osc)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel || layer >= Synth::max_layers) {
        return false;
    }
    const int32_t existing = Sculptor::find_phantom_entry(*bank, channel, zone, layer);
    if (existing >= 0) {
        bank->phantoms[existing].osc = osc;
        return true;
    }
    if (bank->phantom_count >= Synth::max_phantom_entries) {
        return false;
    }
    Synth::PhantomOscillator& entry = bank->phantoms[bank->phantom_count++];
    entry.osc                       = osc;
    entry.channel                   = static_cast<uint8_t>(channel);
    entry.zone                      = static_cast<uint8_t>(zone);
    entry.layer                     = static_cast<uint8_t>(layer);
    return true;
}

// Drops one zone-layer's entry; a no-op when the row has none.
void Sculptor::clear_phantom_entry(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone, uint32_t layer)
{
    const int32_t existing = Sculptor::find_phantom_entry(*bank, channel, zone, layer);
    if (existing >= 0) {
        remove_phantom_entry_at(bank, static_cast<uint32_t>(existing));
    }
}

// Captures every red-marked row's shell parameters from its node into the
// phantom store: the compiled instrument omits red-marked rows, so the
// store is the only home those parameters have.  The capture at the
// deletion event preserves the disconnected oscillator; later edits and
// wire changes to the shell keep the entry in step, so a re-wired shell
// compiles back to exactly the parameters the node last held.  An entry
// is written only while its row's bit is set, and it is invalidated with
// the bit, so a still-red layer's existing entry is never replaced by
// another oscillator's capture.  Returns false when a row without an
// entry cannot get one (the store is full): the caller refuses the batch
// and re-projection restores the pre-event state.
static bool refresh_phantom_entries(Synth::InstrumentEditorBank*     bank,
                                    const Sculptor::Graph&           graph,
                                    const Sculptor::OscGraphMapping& mapping,
                                    uint32_t                         channel,
                                    uint32_t                         zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return true;
    }
    const uint8_t bits = bank->graph_missing_sum[channel][zone];
    if (bits == 0) {
        return true;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if ((bits & (1u << layer)) == 0 || mapping.osc_nodes[layer] == Sculptor::pool_no_slot ||
            ! graph.node_occupied(mapping.osc_nodes[layer])) {
            continue;
        }

        Synth::Oscillator captured;
        memset(&captured, 0, sizeof(captured));

        if (read_oscillator_layer(graph, mapping, layer, &captured) &&
            ! Sculptor::set_phantom_entry(bank, channel, zone, layer, captured)) {
            return false;
        }
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
            // gap in the oscillator node run
            return false;
        }
    }
    if (mapping.output_node == pool_no_slot) {
        return false;
    }

    // Red-marked rows are omitted from the compiled instrument: the
    // compiled layers compact in order (the first connected layer becomes
    // compiled layer 0 and so on) and the count covers connected layers
    // only, so a disconnected oscillator stops reaching the synth.  The
    // red state is read from the sum side's missing mark, which the
    // connection events and the projection keep in step with the
    // missing-sum bits.  A graph whose every row is red-marked compiles to
    // a silent, valid instrument with layer_count 0.
    uint32_t compiled_count = 0;
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        if (graph.slot_missing(mapping.output_node, mapping.output_layer_input_slot[layer])) {
            continue;
        }
        if (! read_oscillator_layer(graph, mapping, layer, &out->layers[compiled_count])) {
            return false;
        }
        ++compiled_count;
    }
    out->layer_count = compiled_count;

    // Instrument-parameter rows live on the dedicated node; direct
    // project_instrument_to_graph callers (the unit tests) always project
    // it, so a missing node means a hand-built graph: parameters stay at
    // the Instrument{} zero-init instead of a slot read.
    if (mapping.instrument_node != pool_no_slot) {
        const Node& params = graph.node(mapping.instrument_node);
        if (params.slots.num_allocated < 3) {
            return false;
        }
        for (uint32_t i = 0; i < 3; ++i) {
            if (mapping.instrument_slots[i] == pool_no_slot) {
                return false;
            }
        }
        out->note_skew_semitones  = params.slots.entries[mapping.instrument_slots[0]].value.real;
        out->layer_skew_semitones = params.slots.entries[mapping.instrument_slots[1]].value.real;
        out->release_seconds      = params.slots.entries[mapping.instrument_slots[2]].value.real;
    }
    // Invariant: every wire into an oscillator lands on a block family
    // dot.  A connection still ending on a base target row means a
    // headless caller bypassed the relocation, and compiling it would
    // silently drop the wire.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == pool_no_slot) {
            continue;
        }
        for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
            if (! graph.connection_occupied(c)) {
                continue;
            }
            const Sculptor::Connection& conn = graph.get_connection(c);
            if (conn.input.node_idx == mapping.osc_nodes[layer] && osc_row_is_target(conn.input.slot_idx)) {
                return false;
            }
        }
    }

    return true;
}

bool Sculptor::osc_graph_validate(void* user_data, Graph& graph, EndPoint output, EndPoint input)
{
    const OscGraphMapping* mapping = static_cast<const OscGraphMapping*>(user_data);
    if (! mapping) {
        return false;
    }

    switch (classify_output(*mapping, output)) {
        case out_env: {
            // An envelope wire lands on a target block's envelope dot: the
            // base value row resolves the dot in the apply path, a direct
            // dot drop names it here.  One envelope per (layer, target):
            // the destination dot must be free.  A volume envelope
            // (minimum 0, nonnegative span) serves only the volume
            // target, and only a volume envelope serves the volume target:
            // the descriptor is shared by every target that wires it, and
            // the two roles want different ranges.
            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_env) {
                return false;
            }
            const uint32_t env_dot = osc_target_env_dot(target);
            if (dot_taken_for_move(graph, input.node_idx, env_dot)) {
                graph.set_error("That envelope row already carries a wire");
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
                             graph.moving_connection);
            if (target == static_cast<uint32_t>(Synth::mod_volume) && other_used) {
                graph.set_error("Envelope already serves a non-volume target");
                return false;
            }
            if (target != static_cast<uint32_t>(Synth::mod_volume) && volume_used) {
                graph.set_error("Envelope already serves the volume target");
                return false;
            }
            return true;
        }
        case out_lfo: {
            // One LFO per (layer, target): the block has one LFO dot, so a
            // fresh wire needs it free.
            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_lfo) {
                return false;
            }
            const uint32_t lfo_dot = osc_target_lfo_dot(target);
            if (dot_taken_for_move(graph, input.node_idx, lfo_dot)) {
                graph.set_error("That LFO row already carries a wire");
                return false;
            }
            return true;
        }
        case out_input: {
            // A CC dot is a source-role output: the runtime resolves cc for
            // routing inputs only, so LFO depth/rate refuse it
            // (validate_instrument refuses cc there too), and envelope/LFO
            // dots refuse every source-role output.
            const bool cc_dot = input_cc_index(*mapping, output.slot_idx) >= 0;

            // LFO depth/rate inputs accept standard MIDI source outputs
            // (the runtime resolves cc for routing inputs only, so CC
            // dots refuse there; validate_instrument refuses cc too).
            const InputKind input_kind = classify_input(*mapping, input);
            if (input_kind == in_depth || input_kind == in_rate) {
                if (cc_dot) {
                    graph.set_error("CC wires into the Source rows only");
                    return false;
                }
                return true;
            }

            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_src) {
                return false;
            }
            if (family == family_src) {
                // A direct drop names its dot; a base-row drop takes the
                // first free Source dot in the apply path, so one free dot
                // in the block suffices.
                if (dot != pool_no_slot) {
                    if (dot_taken_for_move(graph, input.node_idx, dot)) {
                        graph.set_error("That source row already carries a wire");
                        return false;
                    }
                    return true;
                }
                if (osc_target_free_input(graph, input.node_idx, target, family_src) == pool_no_slot) {
                    graph.set_error("Both source rows carry wires");
                    return false;
                }
                return true;
            }
            graph.set_error(cc_dot ? "CC wires into the Source rows only"
                                   : "MIDI sources wire into the Source rows only");
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
        if (slot.property_type == PropertyType::toggle && strncmp(slot.name, "Ping-Pong", sizeof(slot.name)) == 0) {
            // Toggling a multi-point sustain span off snaps the live span end to
            // the span start immediately (same rule as decode,
            // sculptor_bank_json.cpp).  Events carry no old value, so the
            // previous flag state comes from the descriptor itself.
            if (env.ping_pong && ! value.toggle && env.sustain_last_point > env.sustain_first_point) {
                env.points[env.sustain_last_point].value = env.points[env.sustain_first_point].value;
            }
            env.ping_pong = value.toggle ? 1 : 0;
            return true;
        }
        return false;
    }

    if (desc_id > bank->lfos.num_allocated) {
        return false;
    }
    Synth::LFODescriptor& lfo = bank->lfos.entries[desc_id - 1];
    if (slot.property_type == PropertyType::real) {
        if (strncmp(slot.name, "Duty", sizeof(slot.name)) != 0 || ! (value.real >= 0.0f && value.real <= 1.0f)) {
            return false;
        }

        lfo.duty = static_cast<uint8_t>(value.real * 255.0f + 0.5f);
        return true;
    }
    if (slot.property_type == PropertyType::list) {
        if (strncmp(slot.name, "Waveform", sizeof(slot.name)) != 0 || value.list_index >= slot.num_list_options) {
            return false;
        }
        // The two-option wave list keeps the descriptor inside the runtime's
        // sine/sawtooth palette.
        lfo.wave = value.list_index == 0 ? Synth::WaveType::sine_wave : Synth::WaveType::sawtooth_wave;
        return true;
    }
    if (slot.property_type == PropertyType::integer && value.integer >= 0) {
        if (strncmp(slot.name, "Period (ms)", sizeof(slot.name)) == 0 && value.integer <= 65535) {
            lfo.period_ms = static_cast<uint16_t>(value.integer);
            return true;
        }
    }
    return false;
}

// The projected node a kind-0 record keys, or pool_no_slot when the current
// projection has no such node (shorter layer run).
static uint32_t node_at_canonical_index(const Sculptor::OscGraphMapping& mapping, uint32_t index)
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

static void remove_record_at(Synth::InstrumentEditorBank* bank, uint32_t idx)
{
    for (uint32_t i = idx; i + 1 < bank->graph_layout_count; ++i) {
        bank->graph_layout[i] = bank->graph_layout[i + 1];
    }
    bank->graph_layout_count--;
}

static void append_record(Synth::InstrumentEditorBank* bank, const Synth::GraphNodeLayout& record)
{
    bank->graph_layout[bank->graph_layout_count++] = record;
}

// Generator-record persistence: a record-less, unreferenced generator
// instance gains a layout record (free-standing state survives
// re-projection), and a recorded LFO's depth/rate sources follow the
// live edges so a mid-session rewire is what the record stores.
// Reference status derives from the direct target-block edges: an
// instance wired into any oscillator block dot is referenced.
static bool reconcile_generator_records(Synth::InstrumentEditorBank* bank,
                                        Sculptor::Graph*             graph,
                                        Sculptor::OscGraphMapping*   mapping,
                                        uint32_t                     channel,
                                        uint32_t                     zone)
{
    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const Sculptor::DetachedNode& entry = mapping->detached[i];
        if (entry.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(entry.node_idx)) {
            continue;
        }
        bool referenced = false;
        for (uint32_t layer = 0; layer < Synth::max_layers && ! referenced; ++layer) {
            const uint32_t node = mapping->osc_nodes[layer];
            if (node == Sculptor::pool_no_slot || ! graph->node_occupied(node)) {
                continue;
            }
            for (uint32_t t = 0; t < Synth::num_mod_targets && ! referenced; ++t) {
                const uint32_t env_conn = connection_into(*graph, node, osc_target_env_dot(t));
                const uint32_t lfo_conn = connection_into(*graph, node, osc_target_lfo_dot(t));
                if (env_conn != Sculptor::pool_no_slot &&
                    graph->get_connection(env_conn).output.node_idx == entry.node_idx) {
                    referenced = true;
                }
                if (lfo_conn != Sculptor::pool_no_slot &&
                    graph->get_connection(lfo_conn).output.node_idx == entry.node_idx) {
                    referenced = true;
                }
            }
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
static bool osc_deletion_pending(const Sculptor::Graph& graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != Sculptor::pool_no_slot && ! graph.node_occupied(mapping.osc_nodes[layer])) {
            return true;
        }
    }
    return false;
}

// Rewrites the zone's instrument from the graph (canonical form: everything
// the graph does not express compiles to zero).  MIDI routing inputs survive
// in the cell bindings only while the block's source rows wire them:
// compile zeroes the inputs of a block with unconnected source dots.
static bool store_compiled_instrument(Synth::InstrumentEditorBank*     bank,
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

// Syncs every projected instance of one LFO descriptor: wave, duty, and
// period slots are views of the shared descriptor, and Duty editability
// follows the wave list.  Value edits never reproject, so one edit
// through one node must update the sibling nodes silently.
static void sync_lfo_descriptor_views(Synth::InstrumentEditorBank*     bank,
                                      Sculptor::Graph*                 graph,
                                      const Sculptor::OscGraphMapping& mapping,
                                      uint32_t                         node_idx)
{
    uint32_t desc_id = 0;
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].kind == 2 && mapping.detached[i].node_idx == node_idx) {
            desc_id = mapping.detached[i].desc_id;
        }
    }

    if (desc_id == 0 || desc_id > bank->bank.lfos.num_allocated) {
        return;
    }

    const Synth::LFODescriptor& lfo = bank->bank.lfos.entries[desc_id - 1];
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        const Sculptor::DetachedNode& entry = mapping.detached[i];
        if (entry.kind != 2 || entry.desc_id != desc_id || entry.node_idx == Sculptor::pool_no_slot) {
            continue;
        }

        const Sculptor::Node& node = graph->node(entry.node_idx);
        for (uint32_t slot = 0; slot < node.slots.num_allocated; ++slot) {
            const Sculptor::Slot&   slot_ref = node.slots.entries[slot];
            Sculptor::PropertyValue value    = {};
            if (strncmp(slot_ref.name, "Waveform", sizeof(slot_ref.name)) == 0) {
                value.list_index = lfo.wave == Synth::WaveType::sine_wave ? 0 : 1;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Duty", sizeof(slot_ref.name)) == 0) {
                value.real = static_cast<float>(lfo.duty) / 255.0f;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Period (ms)", sizeof(slot_ref.name)) == 0) {
                value.integer = lfo.period_ms;
                graph->set_slot_value(entry.node_idx, slot, value);
            }
        }

        refresh_lfo_duty_disabled(*graph, entry.node_idx);
    }
}

static bool apply_value_change(Synth::InstrumentEditorBank* bank,
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
        case role_osc:
            // Per-layer values: the edited row is that layer's own base; no sibling
            // view syncs.  A red-marked row is a phantom shell the compile
            // omits, so its edit refreshes the row's phantom store entry
            // (the shell stays editable and its parameters survive
            // re-projection).
            for (uint32_t row = 0; row < Synth::max_layers; ++row) {
                if (mapping->osc_nodes[row] == change.node_idx &&
                    (bank->graph_missing_sum[channel][zone] & (1u << row)) != 0) {
                    if (! refresh_phantom_entries(bank, *graph, *mapping, channel, zone)) {
                        return false;
                    }
                    break;
                }
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        case role_env:
        case role_lfo: {
            const bool applied = apply_osc_graph_descriptor_edit(*graph,
                                                                 *mapping,
                                                                 &bank->bank,
                                                                 change.node_idx,
                                                                 change.slot_idx,
                                                                 slot.value);
            if (applied) {
                sync_lfo_descriptor_views(bank, graph, *mapping, change.node_idx);
            }

            return applied;
        }
        case role_input:
        case role_output:
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        default:
            return true;
    }
}

// Removes one oscillator layer from the zone's instrument (graph node
// delete): the layers compact, the zone's mask bits and the layer's kind-0
// record move with the layers, and the mapping drops the node.  Generator
// records are descriptor-keyed, so no record remapping beyond the
// oscillator range is needed.
static bool remove_osc_layer(Synth::InstrumentEditorBank* bank,
                             Sculptor::Graph*             graph,
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
    const uint8_t      bits       = bank->graph_missing_sum[channel][zone];
    // A red-marked row is a phantom shell: removing it drops the shell and
    // its store entry without touching the compiled layers, which count
    // connected layers only.  A connected row removes its own compiled
    // layer, the one at the number of connected rows above it.
    if ((bits & (1u << layer)) == 0) {
        uint32_t compiled_layer = 0;
        for (uint32_t l = 0; l < layer; ++l) {
            if ((bits & (1u << l)) == 0) {
                ++compiled_layer;
            }
        }
        if (compiled_layer < instrument.layer_count) {
            for (uint32_t l = compiled_layer; l + 1 < Synth::max_layers; ++l) {
                instrument.layers[l] = instrument.layers[l + 1];
            }
            instrument.layers[Synth::max_layers - 1] = Synth::Oscillator{};
            instrument.layer_count--;
        }
    }

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
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;

    // The mapping's oscillator run compacts exactly like the instrument's
    // layers and the missing-sum bits above: osc_nodes[l] names the node of
    // projected row l (connected layers and phantom shells share one run,
    // instrument layers hold connected layers only), so a multi-node delete
    // batch whose later events recompile the instrument never sees a gap in
    // the run.
    for (uint32_t l = layer; l + 1 < Synth::max_layers; ++l) {
        mapping->osc_nodes[l] = mapping->osc_nodes[l + 1];
    }
    mapping->osc_nodes[Synth::max_layers - 1] = Sculptor::pool_no_slot;
    // The compile reads the red state from the sum side's slot_missing
    // marks, so the compaction must realign them with the compacted
    // mapping and bits before the batch's final compile: a mark left at
    // a shifted row would skip a surviving connected layer (and include
    // the shell instead).
    const uint8_t compacted_bits = bank->graph_missing_sum[channel][zone];
    for (uint32_t l = 0; l < Synth::max_layers; ++l) {
        const bool missing = (compacted_bits & (1u << l)) != 0;
        graph->set_slot_missing(mapping->output_node, mapping->output_layer_input_slot[l], missing);
        if (mapping->osc_nodes[l] != Sculptor::pool_no_slot) {
            graph->set_slot_missing(mapping->osc_nodes[l], mapping->osc_output_slot, missing);
        }
    }
    return true;
}

static bool apply_node_deleted(Synth::InstrumentEditorBank* bank,
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

    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping->osc_nodes[layer] == node_idx) {
            // The compaction realigns the mapping run with the instrument's
            // layers, and the recompile from live wires rewrites the zone
            // (cells whose wires disappear with the layer, targets that lose
            // their last serving modulator).  When another oscillator of this
            // batch is still pending deletion, the store defers to that
            // node_deleted's final compile.
            if (! remove_osc_layer(bank, graph, mapping, channel, zone, layer)) {
                return false;
            }
            if (! reconcile_generator_records(bank, graph, mapping, channel, zone)) {
                return false;
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        }
    }
    return true; // fixed nodes are vetoed upstream; nothing else carries state
}

// The missing-sum bits live in the bank (they outlive the mapping) and
// are file-local: only this TU reads and clears them.
static void set_missing_sum_bit(Synth::InstrumentEditorBank* bank,
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

static bool channel_has_missing_sum(const Synth::InstrumentEditorBank& bank, uint32_t channel)
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

// Relocates one occupied connection off its base target row onto the
// family dot its output end implies (eventless; the fx param_field
// pattern): an envelope/LFO instance output lands on the block's
// envelope/LFO dot, a MIDI or CC output lands on the first free Source
// dot (A before B). Seeds the row's audible defaults: a first LFO wire
// adopts depth 0.5 while the depth still reads 0 (0 silences the LFO
// entirely), a fresh source row gets the neutral add/1.0 seeds.
// *relocated reports whether the wire moved. Returns false when the
// family has no free input dot; the validator pre-checks every
// well-formed edit, so only a headless caller that bypassed it refuses.
static bool relocate_base_row_wire(Sculptor::Graph*                 graph,
                                   const Sculptor::OscGraphMapping* mapping,
                                   uint32_t                         connection_idx,
                                   bool*                            relocated)
{
    *relocated                             = false;
    const Sculptor::Connection& connection = graph->get_connection(connection_idx);
    bool                        base_row   = false;
    uint32_t                    target     = 0;
    uint32_t                    family     = 0;
    uint32_t                    dot        = Sculptor::pool_no_slot;
    if (! decode_osc_input(*mapping, connection.input, &base_row, &target, &family, &dot) || ! base_row) {
        return true;
    }

    uint32_t      wire_family = family_src;
    const int32_t inst        = find_instance(*mapping, connection.output.node_idx);
    if (inst >= 0 && mapping->detached[inst].kind == 1) {
        wire_family = family_env;
    }
    else if (inst >= 0 && mapping->detached[inst].kind == 2) {
        wire_family = family_lfo;
    }
    const uint32_t landing = osc_target_free_input(*graph, connection.input.node_idx, target, wire_family);
    if (landing == Sculptor::pool_no_slot) {
        return false;
    }

    graph->set_connection_input(connection_idx, Sculptor::EndPoint{ connection.input.node_idx, landing });
    if (wire_family == family_env &&
        graph->node(connection.input.node_idx).slots.entries[landing + 1].value.real == 0.0f) {
        // A first envelope wire adopts the legacy unscaled amount: the
        // row's depth defaults to 0, which silences the envelope entirely,
        // and 0 is almost never the intent.
        graph->set_slot_value(connection.input.node_idx, landing + 1, Sculptor::PropertyValue{ .real = 1.0f });
    }
    if (wire_family == family_lfo &&
        graph->node(connection.input.node_idx).slots.entries[landing + 2].value.real == 0.0f) {
        // A first LFO wire adopts an audible depth: the row's depth
        // defaults to 0, which silences the LFO entirely, and 0 is
        // almost never the intent.
        graph->set_slot_value(connection.input.node_idx, landing + 2, Sculptor::PropertyValue{ .real = 0.5f });
    }
    if (wire_family == family_src) {
        // Neutral add/1.0 seeds so a fresh source row is audible immediately (fx parity).
        graph->set_slot_value(connection.input.node_idx, landing + 1, Sculptor::PropertyValue{ .list_index = 0 });
        graph->set_slot_value(connection.input.node_idx, landing + 2, Sculptor::PropertyValue{ .real = 1.0f });
    }
    *relocated = true;
    return true;
}

static bool apply_connection_change(Synth::InstrumentEditorBank* bank,
                                    Sculptor::Graph*             graph,
                                    Sculptor::OscGraphMapping*   mapping,
                                    const Sculptor::GraphChange& change,
                                    uint32_t                     channel,
                                    uint32_t                     zone)
{
    // A wire dropped on a base target row relocates eventlessly onto the
    // family dot its output end implies (relocate_base_row_wire);
    // the guard refusals mirror the validator, so a well-formed edit
    // never refuses here; a headless caller that bypasses the validator
    // gets a refused batch.
    // A deletion event never observes a base-row endpoint: the apply-side
    // bind and the relocation land in the same drain, so by the time a
    // delete drains its wire already sits on the family dot.  A delete
    // event that still records a base-row input is a headless injection.
    if (change.kind == Sculptor::ChangeKind::connection_deleted) {
        bool     deleted_row    = false;
        uint32_t deleted_target = 0;
        uint32_t deleted_family = 0;
        uint32_t deleted_dot    = Sculptor::pool_no_slot;
        if (decode_osc_input(*mapping,
                             change.connection_input,
                             &deleted_row,
                             &deleted_target,
                             &deleted_family,
                             &deleted_dot) &&
            deleted_row) {
            return false;
        }
    }

    bool relocated = false;
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx)) {
        bool event_relocated = false;
        if (! relocate_base_row_wire(graph, mapping, change.connection_idx, &event_relocated)) {
            return false; // validator pre-checked
        }
        relocated = event_relocated;
    }

    // Wiring an envelope into the volume target forces the volume-envelope
    // shape on the shared descriptor: minimum 0 with the top preserved; raw
    // point values, including the first, stay as drawn.  The wire-time
    // validator refuses cross-target sharing, so the conversion never changes
    // what a non-volume user hears.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx)) {
        const Sculptor::Connection& connection = graph->get_connection(change.connection_idx);
        bool                        base_row   = false;
        uint32_t                    target     = 0;
        uint32_t                    family     = 0;
        uint32_t                    dot        = Sculptor::pool_no_slot;
        if (decode_osc_input(*mapping, connection.input, &base_row, &target, &family, &dot) && family == family_env &&
            target == static_cast<uint32_t>(Synth::mod_volume)) {
            const int32_t inst = find_instance(*mapping, connection.output.node_idx);
            if (inst >= 0 && mapping->detached[inst].kind == 1 && mapping->detached[inst].desc_id != 0 &&
                mapping->detached[inst].desc_id <= bank->bank.envelopes.num_allocated) {
                Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[mapping->detached[inst].desc_id - 1];
                if (! Sculptor::env_volume_shape_ok(env)) {
                    // A negative effective top cannot be preserved under the
                    // nonnegative volume range; refuse the wire.
                    if (! Sculptor::env_convert_to_volume_shape(env)) {
                        return false;
                    }
                }
            }
        }
    }

    // A batch may hold further wire-landing events behind this one; their
    // connections still sit on their base rows and would trip the compile's
    // invariant. Relocate every remaining base-row wire now (eventless, the
    // helper above), so an applied batch always leaves a compile-clean
    // graph; a headless row wire that never went through the applier still
    // refuses at compile time.
    for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
        if (! graph->connection_occupied(c) || c == change.connection_idx) {
            continue;
        }
        bool other_relocated = false;
        if (! relocate_base_row_wire(graph, mapping, c, &other_relocated)) {
            return false;
        }
        relocated = relocated || other_relocated;
    }

    if (relocated) {
        // The relocated row's LFO/source fields become live (or dormant,
        // for a moved-away wire): the edit-disabled flags follow the new
        // wiring eventlessly, like the projection's rebuild does.
        install_edit_disabled_flags(graph, *mapping);
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
                if (! missing) {
                    // The layer is connected again: its shell is no longer a
                    // phantom, so the store entry dies with the bit.
                    Sculptor::clear_phantom_entry(bank, channel, zone, layer);
                }
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

    // Phantom capture runs after the missing-bit block, so the row a
    // deletion just disconnected captures the oscillator the node still
    // holds - the compile below omits red-marked rows, and the store is
    // the only home the disconnected layer's parameters keep.  A capture
    // that does not fit the store refuses the batch; the refusal's
    // re-projection restores the wire, so the disconnection simply does
    // not happen until the store has room.
    if (! refresh_phantom_entries(bank, *graph, *mapping, channel, zone)) {
        return false;
    }

    if (! reconcile_generator_records(bank, graph, mapping, channel, zone)) {
        return false;
    }
    return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
}

// The layer a new oscillator inherits its parameters from:  the highest
// enabled layer, that is, the last one whose waveform A is on.  Waveform
// A cannot be switched off in the editor, so every editor layer
// qualifies.  Banks loaded with an off waveform A fall back to the
// fresh defaults instead.
uint32_t Sculptor::osc_inherit_source_layer(const Synth::Instrument& instrument)
{
    for (uint32_t layer = instrument.layer_count; layer > 0; --layer) {
        if (instrument.layers[layer - 1].osc_type[0] != Synth::WaveType::no_wave) {
            return layer - 1;
        }
    }
    return Sculptor::pool_no_slot;
}

uint32_t Sculptor::osc_row_run_length(uint32_t layer_count, uint8_t missing_bits)
{
    uint32_t connected = 0;
    for (uint32_t row = 0; row < Synth::max_layers; ++row) {
        if ((missing_bits & (1u << row)) != 0) {
            continue;
        }
        if (connected >= layer_count) {
            return row;
        }
        ++connected;
    }
    return Synth::max_layers;
}

bool Sculptor::osc_row_is_red(uint8_t missing_bits, uint32_t row)
{
    return row < Synth::max_layers && (missing_bits & (1u << row)) != 0;
}

uint32_t Sculptor::osc_row_connected_below(uint8_t missing_bits, uint32_t row)
{
    uint32_t connected = 0;
    for (uint32_t below = 0; below < row && below < Synth::max_layers; ++below) {
        if ((missing_bits & (1u << below)) == 0) {
            ++connected;
        }
    }
    return connected;
}

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

// A generator instance no wire references becomes free-standing: a
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
            return true; // node names carry no model state in this graph
        default:
            return true; // node/slot additions and colors carry no model state
    }
}

// Eventless graph rewrites (wire-driven retargeting) carry no change
// events, so a caller outside the drain refreshes the generator records
// and the compiled instrument explicitly, the way apply_connection_change
// does after a drained batch.
bool Sculptor::refresh_osc_graph_compilation(Synth::InstrumentEditorBank* bank,
                                             Graph&                       graph,
                                             OscGraphMapping&             mapping,
                                             const uint32_t               channel,
                                             const uint32_t               zone)
{
    if (! reconcile_generator_records(bank, &graph, &mapping, channel, zone)) {
        return false;
    }
    return store_compiled_instrument(bank, graph, mapping, channel, zone);
}

// Fills resolved[] for the zone's red-marked rows: the phantom store's
// entry where one is valid, the fresh default shell where a red-marked row
// has no store entry.  Production keeps every set bit covered (the event
// capture writes an entry when a bit is set and the load path migrates
// banks saved before the store existed), so the fresh default is a
// last-resort shell for direct callers that hand-set bits without one.
static void resolve_phantom_rows(const Synth::InstrumentEditorBank& bank,
                                 uint32_t                           channel,
                                 uint32_t                           zone,
                                 Synth::Oscillator                  resolved[Synth::max_layers])
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }
    const uint8_t bits = bank.graph_missing_sum[channel][zone];
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if ((bits & (1u << layer)) == 0) {
            continue;
        }
        const int32_t entry = Sculptor::find_phantom_entry(bank, channel, zone, layer);
        if (entry >= 0) {
            resolved[layer] = bank.phantoms[entry].osc;
            continue;
        }
        // Reachable only for direct callers that hand-set bits without the
        // store entry validate_editor_metadata requires: the shell
        // projects from the fresh default instead.
        resolved[layer] = Synth::Oscillator{};
        // Waveform A cannot be off, so an empty shell starts on sine.
        resolved[layer].osc_type[0] = Synth::WaveType::sine_wave;
    }
}

uint32_t Sculptor::count_projected_nodes(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone)
{
    // Fixed inputs, the instrument-parameters node and the sum node, then
    // whatever the zone's instrument and the zone's records add: oscillator
    // layers, generator instances and record surplus (a record that
    // attaches to a derived instance adds no node).
    uint32_t count = Synth::graph_canonical_input_node_count + 2;
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return count;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return count;
    }
    const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];

    Synth::Oscillator resolved[Synth::max_layers] = {};
    resolve_phantom_rows(bank, channel, zone, resolved);
    const Synth::Oscillator* rows[Synth::max_layers] = {};
    const uint32_t           layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    const uint8_t  missing_bits = bank.graph_missing_sum[channel][zone];
    const uint32_t run_length   = Sculptor::osc_row_run_length(layer_count, missing_bits);
    uint32_t       connected    = 0;
    for (uint32_t row = 0; row < run_length; ++row) {
        rows[row] = Sculptor::osc_row_is_red(missing_bits, row) ? &resolved[row] : &instrument.layers[connected++];
    }
    count += run_length;
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_row_instance_keys(rows, run_length, keys);
    for (uint32_t k = 0; k < key_count; ++k) {
        const uint32_t num_allocated =
            keys[k].kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
        if (keys[k].desc_id != 0 && keys[k].desc_id <= num_allocated) {
            ++count; // one node per distinct instance key
        }
    }

    // Records: kind-1/2 records either attach to a derived instance or
    // materialize a surplus node.
    bool claimed[2 * Synth::max_layers * Synth::num_mod_targets] = {};
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
    return count;
}

// The waveform pair a newly added oscillator starts with: the zone's rows
// are scanned top to bottom and the last connected row's pair wins; when
// the run has no connected row, the run's last row (a phantom shell)
// supplies it; an empty run keeps the fresh default (sine with B off).
Sculptor::OscInheritedType Sculptor::osc_inherited_type(const Synth::InstrumentEditorBank& bank,
                                                        uint32_t                           channel,
                                                        uint32_t                           zone)
{
    OscInheritedType inherited = {};
    inherited.wave_a           = Synth::WaveType::sine_wave;
    inherited.wave_b           = Synth::WaveType::no_wave;
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return inherited;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return inherited;
    }
    const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];
    const uint8_t            bits       = bank.graph_missing_sum[channel][zone];
    const uint32_t           layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    const uint32_t    run_length                  = Sculptor::osc_row_run_length(layer_count, bits);
    Synth::Oscillator resolved[Synth::max_layers] = {};
    resolve_phantom_rows(bank, channel, zone, resolved);
    uint32_t                 connected      = 0;
    const Synth::Oscillator* last_connected = nullptr;
    for (uint32_t row = 0; row < run_length; ++row) {
        if (Sculptor::osc_row_is_red(bits, row)) {
            continue;
        }
        last_connected = &instrument.layers[connected++];
    }
    if (last_connected != nullptr) {
        inherited.wave_a = last_connected->osc_type[0];
        inherited.wave_b = last_connected->osc_type[1];
        return inherited;
    }
    if (run_length > 0) {
        // No connected row: the run's last row is a phantom shell.
        inherited.wave_a = resolved[run_length - 1].osc_type[0];
        inherited.wave_b = resolved[run_length - 1].osc_type[1];
    }
    return inherited;
}

// The channel-wide removal-refusal scan: the shared per-channel registry
// makes a CC wired by the channel's fx chain OR by any zone's osc routing
// refuse removal in both panes.  The zone scan stops at the first empty
// slot (an empty slot's instrument byte is meaningless, like the library
// load).  Out-of-range channel (master-chain callers keep the fx-chain-only
// guard) and out-of-range CC refuse nothing.
bool Sculptor::channel_uses_cc(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t cc_number)
{
    if (channel >= Synth::max_channels || ! Synth::cc_is_assignable(cc_number)) {
        return false;
    }

    if (Sculptor::fx_chain_uses_cc(bank.bank.channel_chains[channel], cc_number)) {
        return true;
    }

    const uint16_t bit = static_cast<uint16_t>(1u << (cc_number - Synth::cc_msb_base));
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel && bank.bank.channel_zones[channel][zone].start_note;
         ++zone) {
        const uint32_t instr = bank.bank.channel_zones[channel][zone].instrument;
        if (instr >= bank.bank.instruments.num_allocated) {
            continue;
        }
        if (Sculptor::instrument_cc_mask(bank.bank.instruments.entries[instr]) & bit) {
            return true;
        }
    }
    return false;
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
    Synth::Oscillator resolved_phantoms[Synth::max_layers] = {};
    resolve_phantom_rows(bank, channel, zone, resolved_phantoms);
    if (! project_instrument_to_graph(bank.bank.instruments.entries[zone_entry.instrument],
                                      bank.bank,
                                      graph,
                                      mapping,
                                      &bank.added_cc_mask[channel],
                                      &bank.graph_missing_sum[channel][zone],
                                      resolved_phantoms)) {
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

    // Records attach to derived nodes: kind-1/2 records to the first
    // unclaimed instance with a matching key (records visited in array
    // order; attachment consumes one instance per record).  Surplus
    // records materialize free-standing nodes.
    bool claimed[max_osc_instances] = {};
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
        if (record.kind == 5) {
            // The Instrument Parameters node is structural but not canonically
            // numbered: its layout record keys the zone alone and re-applies
            // the saved position and size overrides when it exists.
            if (mapping->instrument_node != pool_no_slot) {
                graph->set_node_layout(mapping->instrument_node,
                                       vmath::vec2(record.x, record.y),
                                       record.width_override,
                                       record.height_override);
            }
            continue;
        }
        if (record.kind == 1 || record.kind == 2) {
            const bool     is_env        = record.kind == 1;
            const uint32_t num_allocated = is_env ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated || record.depth_source > num_osc_graph_inputs ||
                record.rate_source > num_osc_graph_inputs) {
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
            // The registry-full refusal applies to surplus instances only:
            // attaching to an existing instance adds no node.
            if (mapping->detached_count >= max_osc_instances) {
                return false;
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
            if (record.kind == 2 && record.depth_source != 0 &&
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.depth_source - 1] },
                    EndPoint{ node, mapping->lfo_depth_input_slot }) == Sculptor::pool_no_slot) {
                return false;
            }
            if (record.kind == 2 && record.rate_source != 0 &&
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.rate_source - 1] },
                    EndPoint{ node, mapping->lfo_rate_input_slot }) == Sculptor::pool_no_slot) {
                return false;
            }
        }
    }

    install_edit_disabled_flags(graph, *mapping);

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
    // The phantom store rides the same compaction: the removed row's entry
    // is dropped and this zone's entries above it shift down one layer
    // index, so an entry's layer always matches its red-marked row.
    for (uint32_t i = 0; i < bank->phantom_count;) {
        if (bank->phantoms[i].channel == channel && bank->phantoms[i].zone == zone) {
            if (bank->phantoms[i].layer == removed_layer) {
                remove_phantom_entry_at(bank, i);
                continue;
            }
            if (bank->phantoms[i].layer > removed_layer) {
                bank->phantoms[i].layer--;
            }
        }
        ++i;
    }
}

void Sculptor::compute_publish_channel_enabled(const Synth::InstrumentEditorBank& bank,
                                               uint8_t                            out_enabled[Synth::max_channels])
{
    // The stored channel_enabled values stay untouched; a channel
    // publishes disabled while one of its zones holds a broken oscillator
    // sum, or while a playable zone's instrument has no connected layer
    // (layer_count 0 reaches the runtime's layer_count >= 1 assertion
    // even without a missing bit, so the publication refuses it).
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        bool silent = channel_has_missing_sum(bank, channel);
        if (! silent) {
            for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
                const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
                if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
                    continue;
                }
                if (bank.bank.instruments.entries[zone_entry.instrument].layer_count == 0) {
                    silent = true;
                    break;
                }
            }
        }
        out_enabled[channel] = (bank.bank.channel_enabled[channel] != 0 && ! silent) ? 1 : 0;
    }
}

bool Sculptor::zone_records_split_copy(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone + 1 >= Synth::max_instr_per_channel) {
        return false;
    }
    // Preflight before any shift: the zone's records must fit after the
    // copy, so a refusal leaves every record and mask row untouched.
    uint32_t record_count = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind != 4) {
            ++record_count;
        }
    }
    if (bank->graph_layout_count + record_count > Synth::max_graph_records) {
        return false;
    }
    // The clone's phantom entries must fit the store too, so a refusal
    // leaves every record, mask row and store entry untouched.
    uint32_t                 source_phantom_count = 0;
    Synth::PhantomOscillator source_phantoms[Synth::max_layers];
    for (uint32_t i = 0; i < bank->phantom_count; ++i) {
        if (bank->phantoms[i].channel == channel && bank->phantoms[i].zone == zone) {
            if (source_phantom_count >= Synth::max_layers) {
                return false;
            }
            source_phantoms[source_phantom_count++] = bank->phantoms[i];
        }
    }
    if (bank->phantom_count + source_phantom_count > Synth::max_phantom_entries) {
        return false;
    }
    // Shift later zones first, then copy: records, mask rows and store
    // entries move together.
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.kind != 4 && record.channel == channel && record.zone > zone) {
            record.zone++;
        }
    }
    for (uint32_t z = Synth::max_instr_per_channel - 1; z > zone + 1; --z) {
        bank->graph_missing_sum[channel][z] = bank->graph_missing_sum[channel][z - 1];
    }
    bank->graph_missing_sum[channel][zone + 1] = bank->graph_missing_sum[channel][zone];
    // The phantom store rows move with the bits: later zones shift down,
    // and the cloned zone carries the source zone's entries.
    for (uint32_t i = 0; i < bank->phantom_count; ++i) {
        if (bank->phantoms[i].channel == channel && bank->phantoms[i].zone > zone) {
            bank->phantoms[i].zone++;
        }
    }
    for (uint32_t i = 0; i < source_phantom_count; ++i) {
        Synth::PhantomOscillator copy         = source_phantoms[i];
        copy.zone                             = static_cast<uint8_t>(zone + 1);
        bank->phantoms[bank->phantom_count++] = copy;
    }

    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        // Instrument descriptors remain shared by the cloned content; record
        // identities are zone-scoped.  Effects are channel-wide and do not copy.
        if (record.channel == channel && record.zone == zone && record.kind != 4) {
            Synth::GraphNodeLayout copy                    = record;
            copy.zone                                      = static_cast<uint8_t>(zone + 1);
            bank->graph_layout[bank->graph_layout_count++] = copy;
        }
    }
    return true;
}

// Compacts graph_layout by dropping the zone's records; shared by the
// zone-drop and zone-clear operations so the record policy cannot drift
// between them. Kind-4 records are channel-wide effect layouts, not zone
// state: they are never dropped, and a drop that renumbers zones leaves
// them alone.
static void filter_zone_records(Synth::InstrumentEditorBank* bank,
                                uint32_t                     channel,
                                uint32_t                     zone,
                                bool                         shift_later_zones)
{
    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout record = bank->graph_layout[i];
        if (record.kind != 4 && record.channel == channel) {
            if (record.zone == zone)
                continue;
            if (shift_later_zones && record.zone > zone)
                record.zone--;
        }
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;
}

void Sculptor::zone_records_drop_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }
    filter_zone_records(bank, channel, zone, true);
    for (uint32_t z = zone; z + 1 < Synth::max_instr_per_channel; ++z) {
        bank->graph_missing_sum[channel][z] = bank->graph_missing_sum[channel][z + 1];
    }
    bank->graph_missing_sum[channel][Synth::max_instr_per_channel - 1] = 0;
    // The phantom store rows move with the bits: the dropped zone's
    // entries go and later zones shift down.
    for (uint32_t i = 0; i < bank->phantom_count;) {
        if (bank->phantoms[i].channel != channel) {
            ++i;
            continue;
        }
        if (bank->phantoms[i].zone == zone) {
            remove_phantom_entry_at(bank, i);
            continue;
        }
        if (bank->phantoms[i].zone > zone) {
            bank->phantoms[i].zone--;
        }
        ++i;
    }
}

void Sculptor::zone_records_clear_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }
    filter_zone_records(bank, channel, zone, false);
    bank->graph_missing_sum[channel][zone] = 0;
    // The zone's phantom entries go with its bits.
    for (uint32_t i = 0; i < bank->phantom_count;) {
        if (bank->phantoms[i].channel == channel && bank->phantoms[i].zone == zone) {
            remove_phantom_entry_at(bank, i);
            continue;
        }
        ++i;
    }
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
    // The channel's phantom entries go with its bits.
    for (uint32_t i = 0; i < bank->phantom_count;) {
        if (bank->phantoms[i].channel == channel) {
            remove_phantom_entry_at(bank, i);
            continue;
        }
        ++i;
    }
}

static bool layout_model_valid(const Synth::Instrument&         instrument,
                               const Synth::EnvelopeDescriptor* envelopes,
                               uint32_t                         envelope_count,
                               const Synth::LFODescriptor*      lfos,
                               uint32_t                         lfo_count)
{
    if (envelope_count > Synth::max_envelopes || lfo_count > Synth::max_lfos || (! envelopes && envelope_count) ||
        (! lfos && lfo_count))
        return false;
    clipboard_model_bank = {};
    for (uint32_t index = 0; index < envelope_count; ++index) {
        const uint32_t slot                          = clipboard_model_bank.envelopes.allocate();
        clipboard_model_bank.envelopes.entries[slot] = envelopes[index];
    }
    for (uint32_t index = 0; index < lfo_count; ++index) {
        const uint32_t slot                     = clipboard_model_bank.lfos.allocate();
        clipboard_model_bank.lfos.entries[slot] = lfos[index];
    }
    const uint32_t slot                            = clipboard_model_bank.instruments.allocate();
    clipboard_model_bank.instruments.entries[slot] = instrument;
    clipboard_model_bank.channel_enabled[0]        = 1;
    clipboard_model_bank.channel_zones[0][0]       = { 1, static_cast<uint8_t>(slot) };
    if (! Synth::validate_instrument_bank(&clipboard_model_bank))
        return false;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            const Synth::TargetBinding& binding = instrument.layers[layer].targets[target];
            for (uint32_t input = 0; input < binding.num_inputs; ++input) {
                if (binding.inputs[input].source == Synth::ModSource::none)
                    return false;
            }
        }
    }
    return true;
}

static uint32_t derive_portable_nodes(const Synth::Instrument& instrument, PortableNode* nodes)
{
    uint32_t count = 0;
    for (uint32_t canonical = 0; canonical < 2 + instrument.layer_count; ++canonical) {
        nodes[count]                         = {};
        nodes[count].locator.canonical_index = static_cast<uint8_t>(canonical);
        nodes[count].index                   = static_cast<uint8_t>(canonical);
        ++count;
    }
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_instance_keys(instrument, keys);
    for (uint32_t key = 0; key < key_count; ++key) {
        nodes[count]              = {};
        PortableNode& node        = nodes[count];
        node.locator.kind         = keys[key].kind;
        node.index                = keys[key].desc_id;
        node.locator.depth_source = keys[key].depth_source;
        node.locator.rate_source  = keys[key].rate_source;
        bool first                = true;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                const Synth::TargetBinding& gen = instrument.layers[layer].targets[target];
                const bool serves = node.locator.kind == 1
                                        ? gen.envelope_desc_id == node.index
                                        : gen.lfo_desc_id == node.index &&
                                              static_cast<uint8_t>(gen.lfo_depth_source) == node.locator.depth_source &&
                                              static_cast<uint8_t>(gen.lfo_rate_source) == node.locator.rate_source;
                if (! serves)
                    continue;
                node.served[target] |= static_cast<uint8_t>(1u << layer);
                if (first) {
                    node.locator.layer  = static_cast<uint8_t>(layer);
                    node.locator.target = static_cast<uint8_t>(target);
                    first               = false;
                }
            }
        }
        ++count;
    }
    return count;
}

// Structural validity of a portable layout record: a NUL-terminated EMPTY
// name (the portable encoder deliberately leaves names empty), finite
// position and non-negative size overrides.  A record failing this is
// document corruption; only a structurally valid record that resolves to no
// node may be dropped as removable-target debris.
static bool record_structure_ok(const Synth::InstrumentGraphLayout& record)
{
    return memchr(record.name, 0, sizeof(record.name)) && record.name[0] == 0 && std::isfinite(record.x) &&
           std::isfinite(record.y) && std::isfinite(record.width_override) && std::isfinite(record.height_override) &&
           record.width_override >= 0 && record.height_override >= 0;
}

static uint32_t resolve_portable_node(const Synth::InstrumentGraphLayout& record,
                                      const PortableNode*                 nodes,
                                      uint32_t                            count)
{
    if (! record_structure_ok(record))
        return pool_no_slot;
    for (uint32_t node = 0; node < count; ++node) {
        const PortableNode& candidate = nodes[node];
        if (candidate.locator.kind != record.kind)
            continue;
        if (record.kind == 0 && candidate.locator.canonical_index == record.canonical_index)
            return node;
        if ((record.kind == 1 || record.kind == 2) && record.layer < Synth::max_layers &&
            record.target < Synth::num_mod_targets && (candidate.served[record.target] & (1u << record.layer)) &&
            (record.kind == 1 || (candidate.locator.depth_source == record.depth_source &&
                                  candidate.locator.rate_source == record.rate_source)))
            return node;
    }
    return pool_no_slot;
}

static void copy_portable_position(Synth::InstrumentGraphLayout*       destination,
                                   const Synth::InstrumentGraphLayout& source)
{
    destination->x               = source.x;
    destination->y               = source.y;
    destination->width_override  = source.width_override;
    destination->height_override = source.height_override;
    memcpy(destination->name, source.name, sizeof(destination->name));
}

// Shared per-record drop policy for the layout mapping and normalization
// walks: structurally checks one record, resolves it against the node
// pool, and classifies the outcome.  A structurally valid kind-1/2 record
// that resolves to nothing is droppable removed-target debris, logged
// here; canonical (kind 0) and unknown kinds are self-inconsistency the
// caller must refuse.  The callers own claims, ordering and compaction.
enum PortableRecordVerdict {
    portable_record_invalid,
    portable_record_skipped,
    portable_record_resolved
};

static PortableRecordVerdict classify_portable_record(const Synth::InstrumentGraphLayout& record,
                                                      const PortableNode*                 nodes,
                                                      uint32_t                            node_count,
                                                      uint32_t*                           out_node)
{
    if (! record_structure_ok(record)) {
        return portable_record_invalid; // Document corruption, not removable-target debris.
    }
    const uint32_t node = resolve_portable_node(record, nodes, node_count);
    if (node != pool_no_slot) {
        *out_node = node;
        return portable_record_resolved;
    }
    if (record.kind == 1 || record.kind == 2) {
        d_printf("Ignored unresolvable graph layout record: kind %u layer %u target %u\n",
                 static_cast<unsigned>(record.kind),
                 static_cast<unsigned>(record.layer),
                 static_cast<unsigned>(record.target));
        return portable_record_skipped;
    }
    return portable_record_invalid;
}

bool Sculptor::map_instrument_graph_layout(const Synth::Instrument&            instrument,
                                           const Synth::EnvelopeDescriptor*    envelopes,
                                           uint32_t                            envelope_count,
                                           const Synth::LFODescriptor*         lfos,
                                           uint32_t                            lfo_count,
                                           const Synth::InstrumentGraphLayout* layout,
                                           uint32_t                            layout_count,
                                           Synth::GraphNodeLayout*             out_records,
                                           uint32_t                            capacity,
                                           uint32_t*                           out_count)
{
    if (! out_count || (! layout && layout_count) || (! out_records && layout_count) ||
        layout_count > Synth::instrument_graph_layout_capacity || capacity < layout_count ||
        ! layout_model_valid(instrument, envelopes, envelope_count, lfos, lfo_count))
        return false;
    PortableNode* const           nodes      = clipboard_decoded_nodes;
    const uint32_t                node_count = derive_portable_nodes(instrument, nodes);
    bool                          claimed[Synth::instrument_graph_layout_capacity] = {};
    Synth::GraphNodeLayout* const records                                          = clipboard_mapped_records;
    memset(records, 0, sizeof(clipboard_mapped_records));
    uint32_t count = 0;
    for (uint32_t index = 0; index < layout_count; ++index) {
        uint32_t                    node    = 0;
        const PortableRecordVerdict verdict = classify_portable_record(layout[index], nodes, node_count, &node);
        if (verdict == portable_record_skipped) {
            continue; // No output record, no claim, no uid.
        }
        if (verdict == portable_record_invalid) {
            return false; // Self-inconsistent record, not removable debris.
        }
        if (claimed[node]) {
            return false; // A duplicate claim keeps the surviving uids unique.
        }
        claimed[node]                  = true;
        Synth::GraphNodeLayout& record = records[count++];
        record.kind                    = layout[index].kind;
        record.index                   = nodes[node].index;
        record.depth_source            = layout[index].kind == 2 ? layout[index].depth_source : 0;
        record.rate_source             = layout[index].kind == 2 ? layout[index].rate_source : 0;
        record.uid                     = record.kind == 0 ? 0 : static_cast<uint8_t>(node + 1);
        record.x                       = layout[index].x;
        record.y                       = layout[index].y;
        record.width_override          = layout[index].width_override;
        record.height_override         = layout[index].height_override;
        memcpy(record.name, layout[index].name, sizeof(record.name));
    }
    if (count)
        memcpy(out_records, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

bool Sculptor::normalize_instrument_graph_layout(const Synth::Instrument&            source_instr,
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
                                                 uint32_t*                           out_count)
{
    if (! out_count || (! source_layout && source_layout_count) ||
        source_layout_count > Synth::instrument_graph_layout_capacity ||
        source_instr.layer_count != decoded_instr.layer_count ||
        ! layout_model_valid(source_instr, source_envs, source_env_count, source_lfos, source_lfo_count) ||
        ! layout_model_valid(decoded_instr, decoded_envs, decoded_env_count, decoded_lfos, decoded_lfo_count))
        return false;
    PortableNode* const                        source_nodes  = clipboard_source_nodes;
    PortableNode* const                        decoded_nodes = clipboard_decoded_nodes;
    const uint32_t                             source_count  = derive_portable_nodes(source_instr, source_nodes);
    const uint32_t                             decoded_count = derive_portable_nodes(decoded_instr, decoded_nodes);
    const Synth::InstrumentGraphLayout** const saved         = clipboard_saved_layout;
    memset(saved, 0, sizeof(clipboard_saved_layout));
    for (uint32_t index = 0; index < source_layout_count; ++index) {
        uint32_t                    node = 0;
        const PortableRecordVerdict verdict =
            classify_portable_record(source_layout[index], source_nodes, source_count, &node);
        if (verdict == portable_record_skipped) {
            continue; // Dropped records never enter saved[], never match a
                      // decoded node, and never fire the tail totality check.
        }
        if (verdict == portable_record_invalid) {
            return false;
        }
        if (saved[node])
            return false; // A duplicate claim keeps the surviving uids unique.
        saved[node] = &source_layout[index];
    }
    Synth::InstrumentGraphLayout* const records = clipboard_portable_records;
    memset(records, 0, sizeof(clipboard_portable_records));
    uint32_t count                                         = 0;
    bool     used[Synth::instrument_graph_layout_capacity] = {};
    for (uint32_t decoded = 0; decoded < decoded_count; ++decoded) {
        for (uint32_t source = 0; source < source_count; ++source) {
            if (! saved[source] || source_nodes[source].locator.kind != decoded_nodes[decoded].locator.kind)
                continue;
            bool corresponds =
                source_nodes[source].locator.kind == 0 && source_nodes[source].index == decoded_nodes[decoded].index;
            for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                if (source_nodes[source].served[target] & decoded_nodes[decoded].served[target])
                    corresponds = true;
            }
            if (! corresponds)
                continue;
            if (used[source])
                return false;
            for (uint32_t other = source + 1; other < source_count; ++other) {
                if (! saved[other] || source_nodes[other].locator.kind != decoded_nodes[decoded].locator.kind)
                    continue;
                for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                    if (source_nodes[other].served[target] & decoded_nodes[decoded].served[target])
                        return false;
                }
            }
            used[source]   = true;
            records[count] = decoded_nodes[decoded].locator;
            copy_portable_position(&records[count++], *saved[source]);
            break;
        }
    }
    for (uint32_t source = 0; source < source_count; ++source) {
        if (saved[source] && ! used[source])
            return false;
    }
    if (capacity < count || (! out_layout && count))
        return false;
    if (count)
        memcpy(out_layout, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

bool Sculptor::encode_instrument_graph_layout(const Synth::Instrument&           instr,
                                              const Synth::InstrumentBank&       bank,
                                              const Synth::InstrumentEditorBank& editor_bank,
                                              uint32_t                           channel,
                                              uint32_t                           zone,
                                              Synth::InstrumentGraphLayout*      out_records,
                                              uint32_t                           capacity,
                                              uint32_t*                          out_count)
{
    if (! out_count || channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel ||
        ! Synth::validate_instrument_bank(&bank) || ! Sculptor::validate_editor_metadata(editor_bank) ||
        ! layout_model_valid(instr,
                             bank.envelopes.entries,
                             bank.envelopes.num_allocated,
                             bank.lfos.entries,
                             bank.lfos.num_allocated)) {
        return false;
    }
    const Synth::Zone& entry = editor_bank.bank.channel_zones[channel][zone];
    if (! entry.start_note || entry.instrument >= editor_bank.bank.instruments.num_allocated ||
        memcmp(&instr, &editor_bank.bank.instruments.entries[entry.instrument], sizeof(instr)) != 0) {
        return false;
    }
    if (editor_bank.graph_missing_sum[channel][zone]) {
        return false; // a broken oscillator sum must be repaired before export
    }
    PortableNode* const                 nodes      = clipboard_decoded_nodes;
    const uint32_t                      node_count = derive_portable_nodes(instr, nodes);
    Synth::InstrumentGraphLayout* const records    = clipboard_portable_records;
    memset(records, 0, sizeof(clipboard_portable_records));
    uint32_t count                                 = 0;
    bool     represented[Synth::max_graph_records] = {};
    for (uint32_t index = 0; index < node_count; ++index) {
        const PortableNode& node  = nodes[index];
        uint32_t            saved = pool_no_slot;
        for (uint32_t record_index = 0; record_index < editor_bank.graph_layout_count; ++record_index) {
            const Synth::GraphNodeLayout& record = editor_bank.graph_layout[record_index];
            if (record.channel != channel || record.zone != zone || record.kind != node.locator.kind ||
                record.index != node.index ||
                (record.kind == 2 &&
                 (record.depth_source != node.locator.depth_source || record.rate_source != node.locator.rate_source)))
                continue;
            saved = record_index;
            break;
        }

        if (saved == pool_no_slot)
            continue;
        const Synth::GraphNodeLayout& record = editor_bank.graph_layout[saved];
        if (represented[saved]) {
            return false;
        }
        represented[saved]             = true;
        records[count]                 = node.locator;
        records[count].x               = record.x;
        records[count].y               = record.y;
        records[count].width_override  = record.width_override;
        records[count].height_override = record.height_override;
        ++count;
    }
    for (uint32_t record = 0; record < editor_bank.graph_layout_count; ++record) {
        const Synth::GraphNodeLayout& saved = editor_bank.graph_layout[record];
        // Kinds 4 and 5 are bank-only records that never enter the portable
        // document: effect-graph nodes and the Instrument Parameters node's
        // position stay out of the clipboard/library format.
        if (saved.channel == channel && saved.zone == zone && saved.kind != 4 && saved.kind != 5 &&
            ! represented[record]) {
            // An unrepresented envelope/LFO record whose descriptor id is
            // allocated (1-based) is eligible for the exemption: it is
            // dropped, logged, instead of failing an export the graph UI
            // cannot repair.  Every other unrepresented record still fails.
            const bool allocated_modulator =
                (saved.kind == 1 && saved.index != 0 && saved.index <= bank.envelopes.num_allocated) ||
                (saved.kind == 2 && saved.index != 0 && saved.index <= bank.lfos.num_allocated);
            if (allocated_modulator) {
                d_printf("Ignored unresolvable graph layout record: kind %u descriptor %u\n",
                         static_cast<unsigned>(saved.kind),
                         static_cast<unsigned>(saved.index));
                continue;
            }
            return false;
        }
    }
    if (capacity < count || (! out_records && count)) {
        return false;
    }
    if (count)
        memcpy(out_records, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

bool Sculptor::replace_zone_instrument_candidate(const Synth::InstrumentEditorBank&  source,
                                                 uint32_t                            channel,
                                                 uint32_t                            zone_entry,
                                                 const Synth::Instrument&            decoded_instrument,
                                                 const Synth::EnvelopeDescriptor*    decoded_envelopes,
                                                 uint32_t                            decoded_envelope_count,
                                                 const Synth::LFODescriptor*         decoded_lfos,
                                                 uint32_t                            decoded_lfo_count,
                                                 const Synth::InstrumentGraphLayout* decoded_layout,
                                                 uint32_t                            decoded_layout_count,
                                                 Synth::InstrumentEditorBank*        out_candidate)
{
    if (! out_candidate || channel >= Synth::max_channels || zone_entry >= Synth::max_instr_per_channel ||
        decoded_envelope_count > Synth::instrument_max_envelopes || decoded_lfo_count > Synth::instrument_max_lfos ||
        ! Synth::validate_instrument_bank(&source.bank) || ! validate_editor_metadata(source)) {
        return false;
    }
    uint32_t mapped_count = 0;
    if (! map_instrument_graph_layout(decoded_instrument,
                                      decoded_envelopes,
                                      decoded_envelope_count,
                                      decoded_lfos,
                                      decoded_lfo_count,
                                      decoded_layout,
                                      decoded_layout_count,
                                      clipboard_candidate_records,
                                      Synth::instrument_graph_layout_capacity,
                                      &mapped_count)) {
        return false;
    }
    clipboard_candidate = source;
    // The pasted instrument's wired osc CC bits ride into the receiving
    // channel's shared registry mask; pre-existing destination bits survive
    // and the OR is idempotent for a repeated paste.
    clipboard_candidate.added_cc_mask[channel] = static_cast<uint16_t>(
        clipboard_candidate.added_cc_mask[channel] | Sculptor::instrument_cc_mask(decoded_instrument));
    Synth::InstrumentBank& bank    = clipboard_candidate.bank;
    const uint8_t          enabled = bank.channel_enabled[channel];
    bank.channel_enabled[channel]  = 1;
    if (! Synth::validate_instrument_bank(&bank))
        return false;
    const Synth::Zone& destination = bank.channel_zones[channel][zone_entry];
    if (! destination.start_note || destination.instrument >= bank.instruments.num_allocated)
        return false;
    const uint32_t original_slot = destination.instrument;
    memcpy(clipboard_destination_name,
           clipboard_candidate.instrument_names[original_slot],
           sizeof(clipboard_destination_name));
    uint32_t roots = 0;
    for (uint32_t root_channel = 0; root_channel < Synth::max_channels; ++root_channel) {
        if (! bank.channel_enabled[root_channel])
            continue;
        for (uint32_t entry = 0;
             entry < Synth::max_instr_per_channel && bank.channel_zones[root_channel][entry].start_note;
             ++entry) {
            if (bank.channel_zones[root_channel][entry].instrument == original_slot)
                ++roots;
        }
    }
    uint32_t placeholder_slot = original_slot;
    if (roots != 1) {
        placeholder_slot = bank.instruments.allocate();
        if (placeholder_slot == pool_no_slot)
            return false;
    }
    Synth::Instrument placeholder = bank.instruments.entries[original_slot];
    for (uint32_t layer = 0; layer < placeholder.layer_count; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            placeholder.layers[layer].targets[target].envelope_desc_id = 0;
            placeholder.layers[layer].targets[target].lfo_desc_id      = 0;
        }
    }
    bank.instruments.entries[placeholder_slot] = placeholder;
    memcpy(clipboard_candidate.instrument_names[placeholder_slot],
           clipboard_destination_name,
           sizeof(clipboard_destination_name));
    bank.channel_zones[channel][zone_entry].instrument = static_cast<uint8_t>(placeholder_slot);
    zone_records_clear_zone(&clipboard_candidate, channel, zone_entry);
    if (! Synth::validate_instrument_bank(&bank) || ! validate_editor_metadata(clipboard_candidate))
        return false;
    Synth::reclaim_unused_slots(&clipboard_candidate);
    const uint32_t installed_slot = bank.channel_zones[channel][zone_entry].instrument;
    if (! Synth::remap_envelopes(&bank, decoded_envelopes, decoded_envelope_count, clipboard_envelope_ids) ||
        ! Synth::remap_lfos(&bank, decoded_lfos, decoded_lfo_count, clipboard_lfo_ids))
        return false;
    Synth::remap_instrument(decoded_instrument,
                            clipboard_envelope_ids,
                            clipboard_lfo_ids,
                            &bank.instruments.entries[installed_slot]);
    memcpy(clipboard_candidate.instrument_names[installed_slot],
           clipboard_destination_name,
           sizeof(clipboard_destination_name));
    if (! append_instrument_graph_layout(decoded_instrument,
                                         decoded_envelopes,
                                         decoded_envelope_count,
                                         decoded_lfos,
                                         decoded_lfo_count,
                                         decoded_layout,
                                         decoded_layout_count,
                                         clipboard_envelope_ids,
                                         clipboard_lfo_ids,
                                         channel,
                                         zone_entry,
                                         &clipboard_candidate))
        return false;
    bank.channel_enabled[channel] = enabled;
    if (! Synth::validate_instrument_bank(&bank) || ! validate_editor_metadata(clipboard_candidate))
        return false;
    *out_candidate = clipboard_candidate;
    return true;
}

bool Sculptor::append_instrument_graph_layout(const Synth::Instrument&            instrument,
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
                                              Synth::InstrumentEditorBank*        candidate)
{
    if (! candidate || ! envelope_ids || ! lfo_ids || channel >= Synth::max_channels ||
        zone >= Synth::max_instr_per_channel || candidate->graph_layout_count > Synth::max_graph_records ||
        layout_count > Synth::max_graph_records - candidate->graph_layout_count)
        return false;
    uint32_t count = 0;
    if (! map_instrument_graph_layout(instrument,
                                      envelopes,
                                      envelope_count,
                                      lfos,
                                      lfo_count,
                                      layout,
                                      layout_count,
                                      clipboard_candidate_records,
                                      Synth::instrument_graph_layout_capacity,
                                      &count))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        Synth::GraphNodeLayout& record = clipboard_candidate_records[index];
        record.channel                 = static_cast<uint8_t>(channel);
        record.zone                    = static_cast<uint8_t>(zone);
        if (record.kind == 1) {
            if (! envelope_ids[record.index - 1] ||
                envelope_ids[record.index - 1] > candidate->bank.envelopes.num_allocated)
                return false;
            record.index = static_cast<uint8_t>(envelope_ids[record.index - 1]);
        }
        if (record.kind == 2) {
            if (! lfo_ids[record.index - 1] || lfo_ids[record.index - 1] > candidate->bank.lfos.num_allocated)
                return false;
            record.index = static_cast<uint8_t>(lfo_ids[record.index - 1]);
        }
    }
    memcpy(candidate->graph_layout + candidate->graph_layout_count,
           clipboard_candidate_records,
           count * sizeof(clipboard_candidate_records[0]));
    candidate->graph_layout_count += count;
    return true;
}
