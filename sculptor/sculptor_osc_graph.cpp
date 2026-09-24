// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_osc_graph.h"

#include <stdio.h>
#include <string.h>

namespace {

// The five targets the runtime can modulate per oscillator; duty A/B, osc
// mix and FM index are per-oscillator constants carried as base values only.
bool is_projected_target(Synth::ModTarget target)
{
    return target <= Synth::mod_panning || target >= Synth::mod_lowpass_cutoff;
}

uint32_t projected_index(Synth::ModTarget target)
{
    return target <= Synth::mod_panning ? static_cast<uint32_t>(target) : static_cast<uint32_t>(target) - 4u;
}

// The projection arithmetic relies on this exact ModTarget ordering.
static_assert(static_cast<uint32_t>(Synth::mod_panning) == 2);
static_assert(static_cast<uint32_t>(Synth::mod_lowpass_cutoff) == 7);
static_assert(static_cast<uint32_t>(Synth::mod_highpass_cutoff) == 8);
static_assert(Synth::num_mod_targets == 9);
static_assert(Synth::max_mod_inputs == 2);

const char* const projected_target_names[5]                    = { "volume", "pitch", "pan", "lowpass", "highpass" };
const char* const target_names[Synth::num_mod_targets]         = { "volume",  "pitch",    "panning", "duty0",   "duty1",
                                                                   "osc mix", "fm index", "lowpass", "highpass" };
const char* const source_names[Sculptor::num_osc_graph_inputs] = { "Pitch bend", "Mod wheel",  "Channel press.",
                                                                   "Velocity",   "Aftertouch", "Pressure" };
const char* const wave_names[5]                                = { "off", "sine", "saw", "pulse", "noise" };
const char* const osc_mode_names[3]                            = { "blend", "fm", "hard sync" };
const char* const op_names[2]                                  = { "add", "multiply" };

// Oscillator node slot layout.  The order is fixed: project fills slots in
// exactly this order and compile reads properties back by index, so the two
// stay consistent by construction.
//   0                  output
//   1..20              per projected target, four slots: two direct
//                      inputs, envelope input, LFO input (volume 1-4,
//                      pitch 5-8, pan 9-12, lowpass 13-16, highpass 17-20)
//   21..25             waveform a/b, mode, mod ratio, pitch offset
//   26..34             base value per ModTarget (all nine targets)
//   35..69             per projected target: op/scale pair per input index,
//                      then LFO op, LFO depth, rate scale
constexpr uint32_t osc_waveform_a_prop   = 21;
constexpr uint32_t osc_waveform_b_prop   = 22;
constexpr uint32_t osc_mode_prop         = 23;
constexpr uint32_t osc_mod_ratio_prop    = 24;
constexpr uint32_t osc_pitch_offset_prop = 25;
constexpr uint32_t osc_first_base_prop   = 26;
constexpr uint32_t osc_first_target_prop = 35; // first per-target property group
constexpr uint32_t osc_slot_count        = 70;

// Per projected target: op/scale pair per input index, then lfo op,
// lfo depth, rate scale - seven slots per target.
uint32_t osc_op_prop(Synth::ModTarget target, uint32_t input)
{
    return osc_first_target_prop + projected_index(target) * 7 + input * 2;
}

// Envelope node slot layout: 0 output, then descriptor-content properties.

// LFO node slot layout: 0 output, 1 depth source, 2 rate source, then
// descriptor-content properties.
constexpr uint32_t lfo_depth_input = 1;
constexpr uint32_t lfo_rate_input  = 2;

// Output node slot layout: one input per layer, then the skew properties.
constexpr uint32_t output_note_skew_prop  = Synth::max_layers;
constexpr uint32_t output_layer_skew_prop = Synth::max_layers + 1;

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

// The per-binding LFO generator fields live on the target (oscillator) node,
// one property per projected target, because they must survive even when the
// target has no LFO node (the LayerGen keeps them without a descriptor).
uint32_t osc_lfo_op_prop(Synth::ModTarget target)
{
    return osc_first_target_prop + projected_index(target) * 7 + 4;
}

uint32_t osc_lfo_depth_prop(Synth::ModTarget target)
{
    return osc_first_target_prop + projected_index(target) * 7 + 5;
}

uint32_t osc_rate_scale_prop(Synth::ModTarget target)
{
    return osc_first_target_prop + projected_index(target) * 7 + 6;
}

enum NodeRole {
    role_none,
    role_input,
    role_osc,
    role_env,
    role_lfo,
    role_output
};

NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        if (mapping.input_nodes[i] == node_idx) {
            return role_input;
        }
    }
    if (mapping.output_node == node_idx) {
        return role_output;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == node_idx) {
            return role_osc;
        }
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            if (mapping.env_nodes[layer][t] == node_idx) {
                return role_env;
            }
            if (mapping.lfo_nodes[layer][t] == node_idx) {
                return role_lfo;
            }
        }
    }
    return role_none;
}

enum OutputKind {
    out_none,
    out_input,
    out_osc,
    out_env,
    out_lfo
};

OutputKind classify_output(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_input:
            return point.slot_idx == mapping.input_output_slot ? out_input : out_none;
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

enum InputKind {
    in_none,
    in_direct,
    in_env,
    in_lfo,
    in_depth,
    in_rate,
    in_sum
};

InputKind classify_input(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_osc:
            for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                if (! is_projected_target(static_cast<Synth::ModTarget>(t))) {
                    continue;
                }
                for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                    if (point.slot_idx == mapping.osc_direct_input_slot[t][i]) {
                        return in_direct;
                    }
                }
                if (point.slot_idx == mapping.osc_env_input_slot[t]) {
                    return in_env;
                }
                if (point.slot_idx == mapping.osc_lfo_input_slot[t]) {
                    return in_lfo;
                }
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

// The live connection terminating at an input connector, or pool_no_slot.
uint32_t connection_into(const Sculptor::Graph& graph, uint32_t node_idx, uint32_t slot_idx)
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

// Resolves the MIDI source feeding a connector through the input node table;
// false when the source node is not a projected MIDI input node.
bool edge_source(const Sculptor::Graph&           graph,
                 const Sculptor::OscGraphMapping& mapping,
                 uint32_t                         connection_idx,
                 Synth::ModSource*                source)
{
    const uint32_t node_idx = graph.get_connection(connection_idx).output.node_idx;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        if (mapping.input_nodes[i] == node_idx) {
            *source = static_cast<Synth::ModSource>(i + 1);
            return true;
        }
    }
    return false;
}

// Resolves a binding node's descriptor id through the mapping; false when
// the node is not a projected binding node of the expected kind.
bool binding_desc_id(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx, bool env, uint16_t* desc_id)
{
    if (node_idx == Sculptor::pool_no_slot) {
        return false;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            if (env && mapping.env_nodes[layer][t] == node_idx) {
                *desc_id = mapping.env_desc_ids[layer][t];
                return true;
            }
            if (! env && mapping.lfo_nodes[layer][t] == node_idx) {
                *desc_id = mapping.lfo_desc_ids[layer][t];
                return true;
            }
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
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        mapping->input_nodes[i] = Sculptor::pool_no_slot;
    }
    mapping->input_output_slot = Sculptor::pool_no_slot;
    mapping->output_node       = Sculptor::pool_no_slot;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        mapping->output_layer_input_slot[layer] = Sculptor::pool_no_slot;
        mapping->osc_nodes[layer]               = Sculptor::pool_no_slot;
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            mapping->env_nodes[layer][t]    = Sculptor::pool_no_slot;
            mapping->lfo_nodes[layer][t]    = Sculptor::pool_no_slot;
            mapping->env_desc_ids[layer][t] = 0;
            mapping->lfo_desc_ids[layer][t] = 0;
        }
    }
    mapping->osc_output_slot = Sculptor::pool_no_slot;
    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            mapping->osc_direct_input_slot[t][i] = Sculptor::pool_no_slot;
        }
        mapping->osc_env_input_slot[t] = Sculptor::pool_no_slot;
        mapping->osc_lfo_input_slot[t] = Sculptor::pool_no_slot;
    }
    // Uniform env/lfo node slot layout; populated even when no binding node
    // exists so the mapping always describes the full projection layout.
    mapping->env_output_slot      = 0;
    mapping->lfo_output_slot      = 0;
    mapping->lfo_depth_input_slot = lfo_depth_input;
    mapping->lfo_rate_input_slot  = lfo_rate_input;
}

void project_osc_node(Sculptor::Graph*           graph,
                      uint32_t                   layer,
                      const Synth::Instrument&   instrument,
                      Sculptor::OscGraphMapping* mapping)
{
    char name[32];
    snprintf(name, sizeof(name), "Osc %u", layer + 1);
    const uint32_t node = graph->create_node(name, vmath::vec2(512.0f, 256.0f * static_cast<float>(layer)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    mapping->osc_nodes[layer]    = node;
    const Synth::Oscillator& osc = instrument.layers[layer];

    mapping->osc_output_slot = graph->add_slot(node, output_slot("out"));

    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
        if (! is_projected_target(target)) {
            continue;
        }
        const uint32_t proj = projected_index(target);
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            snprintf(name, sizeof(name), "%s %u", projected_target_names[proj], i);
            mapping->osc_direct_input_slot[t][i] = graph->add_slot(node, input_slot(name));
        }
        snprintf(name, sizeof(name), "env %s", projected_target_names[proj]);
        mapping->osc_env_input_slot[t] = graph->add_slot(node, input_slot(name));
        snprintf(name, sizeof(name), "lfo %s", projected_target_names[proj]);
        mapping->osc_lfo_input_slot[t] = graph->add_slot(node, input_slot(name));
    }

    graph->add_slot(node, list_slot("waveform a", wave_names, 5, static_cast<uint32_t>(osc.osc_type[0])));
    graph->add_slot(node, list_slot("waveform b", wave_names, 5, static_cast<uint32_t>(osc.osc_type[1])));
    graph->add_slot(node, list_slot("mode", osc_mode_names, 3, static_cast<uint32_t>(osc.osc_mode)));
    graph->add_slot(node, real_slot("mod ratio", osc.mod_ratio));
    graph->add_slot(node, real_slot("pitch offset", osc.pitch_offset));

    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        snprintf(name, sizeof(name), "base %s", target_names[t]);
        graph->add_slot(node, real_slot(name, instrument.routing[t].base_value));
    }

    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
        if (! is_projected_target(target)) {
            continue;
        }
        const uint32_t proj = projected_index(target);
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            snprintf(name, sizeof(name), "op %s %u", projected_target_names[proj], i);
            graph->add_slot(node,
                            list_slot(name, op_names, 2, static_cast<uint32_t>(instrument.routing[t].inputs[i].op)));
            snprintf(name, sizeof(name), "scale %s %u", projected_target_names[proj], i);
            graph->add_slot(node, real_slot(name, instrument.routing[t].inputs[i].scale));
        }
        snprintf(name, sizeof(name), "lfo op %s", projected_target_names[proj]);
        graph->add_slot(node, list_slot(name, op_names, 2, static_cast<uint32_t>(osc.gen[t].lfo_op)));
        snprintf(name, sizeof(name), "lfo depth %s", projected_target_names[proj]);
        graph->add_slot(node, real_slot(name, osc.gen[t].lfo_depth));
        snprintf(name, sizeof(name), "rate scale %s", projected_target_names[proj]);
        graph->add_slot(node, real_slot(name, osc.gen[t].lfo_rate_scale_ms));
    }
}

void project_env_node(Sculptor::Graph*             graph,
                      uint32_t                     layer,
                      Synth::ModTarget             target,
                      uint32_t                     row,
                      const Synth::Instrument&     instrument,
                      const Synth::InstrumentBank& bank,
                      Sculptor::OscGraphMapping*   mapping)
{
    const Synth::LayerGen& gen = instrument.layers[layer].gen[target];
    if (gen.envelope_desc_id == 0 || gen.envelope_desc_id > bank.envelopes.num_allocated) {
        return;
    }
    const Synth::EnvelopeDescriptor& env = bank.envelopes.entries[gen.envelope_desc_id - 1];

    char name[32];
    snprintf(name, sizeof(name), "Env L%u %s", layer + 1, projected_target_names[projected_index(target)]);
    const uint32_t node = graph->create_node(name, vmath::vec2(1024.0f, 128.0f * static_cast<float>(row)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    mapping->env_nodes[layer][target]    = node;
    mapping->env_desc_ids[layer][target] = gen.envelope_desc_id;
    mapping->env_output_slot             = graph->add_slot(node, output_slot("out"));
    graph->add_slot(node, real_slot("min_value", env.min_value));
    graph->add_slot(node, real_slot("min_max_delta", env.min_max_delta));
    graph->add_slot(node, int_slot("sustain_first", env.sustain_first_point));
    graph->add_slot(node, int_slot("sustain_last", env.sustain_last_point));
    graph->add_slot(node, int_slot("num_points", env.num_points));

    graph->add_connection(Sculptor::EndPoint{ node, mapping->env_output_slot },
                          Sculptor::EndPoint{ mapping->osc_nodes[layer], mapping->osc_env_input_slot[target] });
}

void project_lfo_node(Sculptor::Graph*             graph,
                      uint32_t                     layer,
                      Synth::ModTarget             target,
                      uint32_t                     row,
                      const Synth::Instrument&     instrument,
                      const Synth::InstrumentBank& bank,
                      Sculptor::OscGraphMapping*   mapping)
{
    const Synth::LayerGen& gen = instrument.layers[layer].gen[target];
    if (gen.lfo_desc_id == 0 || gen.lfo_desc_id > bank.lfos.num_allocated) {
        return;
    }
    const Synth::LFODescriptor& lfo = bank.lfos.entries[gen.lfo_desc_id - 1];

    char name[32];
    snprintf(name, sizeof(name), "LFO L%u %s", layer + 1, projected_target_names[projected_index(target)]);
    const uint32_t node = graph->create_node(name, vmath::vec2(1536.0f, 128.0f * static_cast<float>(row)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    mapping->lfo_nodes[layer][target]    = node;
    mapping->lfo_desc_ids[layer][target] = gen.lfo_desc_id;
    mapping->lfo_output_slot             = graph->add_slot(node, output_slot("out"));
    mapping->lfo_depth_input_slot        = graph->add_slot(node, input_slot("depth"));
    mapping->lfo_rate_input_slot         = graph->add_slot(node, input_slot("rate"));
    graph->add_slot(node, list_slot("wave", wave_names, 5, static_cast<uint32_t>(lfo.wave)));
    graph->add_slot(node, int_slot("duty", lfo.duty));
    graph->add_slot(node, int_slot("period_ms", lfo.period_ms));
    graph->add_slot(node, real_slot("min_value", lfo.min_value));
    graph->add_slot(node, real_slot("min_max_delta", lfo.min_max_delta));

    graph->add_connection(Sculptor::EndPoint{ node, mapping->lfo_output_slot },
                          Sculptor::EndPoint{ mapping->osc_nodes[layer], mapping->osc_lfo_input_slot[target] });

    if (gen.lfo_depth_source != Synth::ModSource::none) {
        graph->add_connection(Sculptor::EndPoint{ mapping->input_nodes[static_cast<uint32_t>(gen.lfo_depth_source) - 1],
                                                  mapping->input_output_slot },
                              Sculptor::EndPoint{ node, mapping->lfo_depth_input_slot });
    }
    if (gen.lfo_rate_source != Synth::ModSource::none) {
        graph->add_connection(Sculptor::EndPoint{ mapping->input_nodes[static_cast<uint32_t>(gen.lfo_rate_source) - 1],
                                                  mapping->input_output_slot },
                              Sculptor::EndPoint{ node, mapping->lfo_rate_input_slot });
    }
}

} // namespace

bool Sculptor::project_instrument_to_graph(const Synth::Instrument&     instrument,
                                           const Synth::InstrumentBank& bank,
                                           Sculptor::Graph*             graph,
                                           Sculptor::OscGraphMapping*   mapping)
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
    graph->clear();

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        const uint32_t node = graph->create_node(source_names[i], vmath::vec2(0.0f, 96.0f * static_cast<float>(i)));
        if (node == Sculptor::pool_no_slot) {
            return false;
        }
        mapping->input_nodes[i]    = node;
        mapping->input_output_slot = graph->add_slot(node, output_slot("out"));
    }

    const uint32_t output_node = graph->create_node("Osc Sum", vmath::vec2(2048.0f, 0.0f));
    if (output_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->output_node = output_node;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        char name[32];
        snprintf(name, sizeof(name), "Layer %u", layer + 1);
        mapping->output_layer_input_slot[layer] = graph->add_slot(output_node, input_slot(name));
    }
    graph->add_slot(output_node, real_slot("note skew", instrument.note_skew_semitones));
    graph->add_slot(output_node, real_slot("layer skew", instrument.layer_skew_semitones));

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        project_osc_node(graph, layer, instrument, mapping);
    }

    uint32_t env_row = 0;
    uint32_t lfo_row = 0;
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            if (! is_projected_target(target)) {
                continue;
            }
            if (instrument.layers[layer].gen[t].envelope_desc_id != 0) {
                project_env_node(graph, layer, target, env_row++, instrument, bank, mapping);
            }
            if (instrument.layers[layer].gen[t].lfo_desc_id != 0) {
                project_lfo_node(graph, layer, target, lfo_row++, instrument, bank, mapping);
            }
        }
    }

    // MIDI sources feed the direct inputs; the routing is shared by all
    // layers, so every layer's oscillator draws the same direct edges.
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            if (! is_projected_target(target)) {
                continue;
            }
            const Synth::InputRouting& routing = instrument.routing[t];
            const uint32_t             num_inputs =
                routing.num_inputs <= Synth::max_mod_inputs ? routing.num_inputs : Synth::max_mod_inputs;
            for (uint32_t i = 0; i < num_inputs; ++i) {
                const uint32_t source = static_cast<uint32_t>(routing.inputs[i].source);
                if (source == 0 || source > Sculptor::num_osc_graph_inputs) {
                    continue;
                }
                graph->add_connection(
                    Sculptor::EndPoint{ mapping->input_nodes[source - 1], mapping->input_output_slot },
                    Sculptor::EndPoint{ mapping->osc_nodes[layer], mapping->osc_direct_input_slot[t][i] });
            }
        }
    }

    // Structural hard connections: every oscillator feeds its sum input.
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        graph->add_connection(Sculptor::EndPoint{ mapping->osc_nodes[layer], mapping->osc_output_slot },
                              Sculptor::EndPoint{ mapping->output_node, mapping->output_layer_input_slot[layer] });
    }

    return true;
}

bool Sculptor::compile_graph_to_instrument(const Sculptor::Graph&           graph,
                                           const Sculptor::OscGraphMapping& mapping,
                                           Synth::Instrument*               out)
{
    *out = Synth::Instrument{};

    uint32_t layer_count = 0;
    while (layer_count < Synth::max_layers && mapping.osc_nodes[layer_count] != Sculptor::pool_no_slot) {
        ++layer_count;
    }
    for (uint32_t layer = layer_count; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != Sculptor::pool_no_slot) {
            return false; // gap in the oscillator node run
        }
    }
    if (mapping.output_node == Sculptor::pool_no_slot) {
        return false;
    }
    out->layer_count = layer_count;

    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        const Sculptor::Node& node = graph.node(mapping.osc_nodes[layer]);
        if (node.slots.num_allocated < osc_slot_count) {
            return false;
        }
        const Sculptor::Slot* slots = node.slots.entries;
        Synth::Oscillator&    osc   = out->layers[layer];

        uint32_t index = 0;
        if (! read_list_prop(slots[osc_waveform_a_prop], &index)) {
            return false;
        }
        osc.osc_type[0] = static_cast<Synth::WaveType>(index);
        if (! read_list_prop(slots[osc_waveform_b_prop], &index)) {
            return false;
        }
        osc.osc_type[1] = static_cast<Synth::WaveType>(index);
        if (! read_list_prop(slots[osc_mode_prop], &index)) {
            return false;
        }
        osc.osc_mode     = static_cast<Synth::OscMode>(index);
        osc.mod_ratio    = slots[osc_mod_ratio_prop].value.real;
        osc.pitch_offset = slots[osc_pitch_offset_prop].value.real;

        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            Synth::LayerGen&       gen    = osc.gen[t];
            if (! is_projected_target(target)) {
                continue;
            }
            // Bindings resolve from the edges entering the target: an edge from a
            // binding node binds that node's descriptor, and no edge means no
            // binding.  The desc id comes from the source node's identity, so
            // aliasing survives edits and re-projection.
            const uint32_t env_conn = connection_into(graph, mapping.osc_nodes[layer], mapping.osc_env_input_slot[t]);
            if (env_conn != Sculptor::pool_no_slot && ! binding_desc_id(mapping,
                                                                        graph.get_connection(env_conn).output.node_idx,
                                                                        true,
                                                                        &gen.envelope_desc_id)) {
                return false;
            }
            const uint32_t lfo_conn = connection_into(graph, mapping.osc_nodes[layer], mapping.osc_lfo_input_slot[t]);
            if (lfo_conn != Sculptor::pool_no_slot) {
                const uint32_t lfo_node = graph.get_connection(lfo_conn).output.node_idx;
                if (! binding_desc_id(mapping, lfo_node, false, &gen.lfo_desc_id)) {
                    return false;
                }
                // Depth/rate sources resolve from the edges into the connected LFO
                // node; no edge means the source stays none.
                const uint32_t depth_conn = connection_into(graph, lfo_node, mapping.lfo_depth_input_slot);
                if (depth_conn != Sculptor::pool_no_slot &&
                    ! edge_source(graph, mapping, depth_conn, &gen.lfo_depth_source)) {
                    return false;
                }
                const uint32_t rate_conn = connection_into(graph, lfo_node, mapping.lfo_rate_input_slot);
                if (rate_conn != Sculptor::pool_no_slot &&
                    ! edge_source(graph, mapping, rate_conn, &gen.lfo_rate_source)) {
                    return false;
                }
            }
            if (! read_list_prop(slots[osc_lfo_op_prop(target)], &index)) {
                return false;
            }
            gen.lfo_op            = static_cast<Synth::SourceOp>(index);
            gen.lfo_depth         = slots[osc_lfo_depth_prop(target)].value.real;
            gen.lfo_rate_scale_ms = slots[osc_rate_scale_prop(target)].value.real;
        }
    }

    // MIDI routing is shared by all layers: base values come from any
    // oscillator node (they all carry the same shared values), the input
    // sources from layer 0's direct-input connectors, and the op/scale from
    // the per-input-index properties on the target node.
    if (layer_count > 0) {
        const Sculptor::Node& first = graph.node(mapping.osc_nodes[0]);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
            out->routing[t].base_value    = first.slots.entries[osc_first_base_prop + t].value.real;
            if (! is_projected_target(target)) {
                continue;
            }
            uint32_t num_inputs = 0;
            uint32_t op_index   = 0;
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                const uint32_t conn = connection_into(graph, mapping.osc_nodes[0], mapping.osc_direct_input_slot[t][i]);
                if (conn == Sculptor::pool_no_slot) {
                    continue;
                }
                Synth::ModSource source = Synth::ModSource::none;
                if (! edge_source(graph, mapping, conn, &source)) {
                    return false;
                }
                Synth::ModInput& input = out->routing[t].inputs[num_inputs++];
                input.source           = source;
                if (! read_list_prop(first.slots.entries[osc_op_prop(target, i)], &op_index)) {
                    return false;
                }
                input.op    = static_cast<Synth::SourceOp>(op_index);
                input.scale = first.slots.entries[osc_op_prop(target, i) + 1].value.real;
            }
            out->routing[t].num_inputs = static_cast<uint16_t>(num_inputs);
        }
    }

    const Sculptor::Node& output = graph.node(mapping.output_node);
    if (output.slots.num_allocated < Synth::max_layers + 2) {
        return false;
    }
    out->note_skew_semitones  = output.slots.entries[output_note_skew_prop].value.real;
    out->layer_skew_semitones = output.slots.entries[output_layer_skew_prop].value.real;

    return true;
}

bool Sculptor::osc_graph_validate(void*                  user_data,
                                  const Sculptor::Graph& graph,
                                  Sculptor::EndPoint     output,
                                  Sculptor::EndPoint     input)
{
    (void)graph;
    const Sculptor::OscGraphMapping* mapping = static_cast<const Sculptor::OscGraphMapping*>(user_data);
    if (! mapping) {
        return false;
    }

    switch (classify_output(*mapping, output)) {
        case out_env:
            return classify_input(*mapping, input) == in_env;
        case out_lfo:
            return classify_input(*mapping, input) == in_lfo;
        case out_input:
            switch (classify_input(*mapping, input)) {
                case in_direct:
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

bool Sculptor::apply_osc_graph_descriptor_edit(const Sculptor::Graph&           graph,
                                               const Sculptor::OscGraphMapping& mapping,
                                               Synth::InstrumentBank*           bank,
                                               uint32_t                         node_idx,
                                               uint32_t                         slot_idx,
                                               Sculptor::PropertyValue          value)
{
    uint16_t desc_id = 0;
    bool     is_env  = false;
    for (uint32_t layer = 0; layer < Synth::max_layers && desc_id == 0; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            if (mapping.env_nodes[layer][t] == node_idx) {
                desc_id = mapping.env_desc_ids[layer][t];
                is_env  = true;
                break;
            }
            if (mapping.lfo_nodes[layer][t] == node_idx) {
                desc_id = mapping.lfo_desc_ids[layer][t];
                break;
            }
        }
    }
    if (desc_id == 0) {
        return false;
    }

    const Sculptor::Node& node = graph.node(node_idx);
    if (slot_idx >= node.slots.num_allocated) {
        return false;
    }
    const Sculptor::Slot& slot = node.slots.entries[slot_idx];
    if (slot.kind != Sculptor::SlotKind::property) {
        return false;
    }

    if (is_env) {
        if (desc_id > bank->envelopes.num_allocated) {
            return false;
        }
        Synth::EnvelopeDescriptor& env = bank->envelopes.entries[desc_id - 1];
        if (slot.property_type == Sculptor::PropertyType::real) {
            if (strncmp(slot.name, "min_value", sizeof(slot.name)) == 0) {
                env.min_value = value.real;
                return true;
            }
            if (strncmp(slot.name, "min_max_delta", sizeof(slot.name)) == 0) {
                env.min_max_delta = value.real;
                return true;
            }
            return false;
        }
        if (slot.property_type == Sculptor::PropertyType::integer && value.integer >= 0 && value.integer <= 255) {
            if (strncmp(slot.name, "sustain_first", sizeof(slot.name)) == 0) {
                env.sustain_first_point = static_cast<uint8_t>(value.integer);
                return true;
            }
            if (strncmp(slot.name, "sustain_last", sizeof(slot.name)) == 0) {
                env.sustain_last_point = static_cast<uint8_t>(value.integer);
                return true;
            }
        }
        return false;
    }

    if (desc_id > bank->lfos.num_allocated) {
        return false;
    }
    Synth::LFODescriptor& lfo = bank->lfos.entries[desc_id - 1];
    if (slot.property_type == Sculptor::PropertyType::real) {
        if (strncmp(slot.name, "min_value", sizeof(slot.name)) == 0) {
            lfo.min_value = value.real;
            return true;
        }
        if (strncmp(slot.name, "min_max_delta", sizeof(slot.name)) == 0) {
            lfo.min_max_delta = value.real;
            return true;
        }
        return false;
    }
    if (slot.property_type == Sculptor::PropertyType::list) {
        if (strncmp(slot.name, "wave", sizeof(slot.name)) != 0 || value.list_index >= slot.num_list_options) {
            return false;
        }
        lfo.wave = static_cast<Synth::WaveType>(value.list_index);
        return true;
    }
    if (slot.property_type == Sculptor::PropertyType::integer && value.integer >= 0) {
        if (strncmp(slot.name, "duty", sizeof(slot.name)) == 0 && value.integer <= 255) {
            lfo.duty = static_cast<uint8_t>(value.integer);
            return true;
        }
        if (strncmp(slot.name, "period_ms", sizeof(slot.name)) == 0 && value.integer <= 65535) {
            lfo.period_ms = static_cast<uint16_t>(value.integer);
            return true;
        }
    }
    return false;
}
