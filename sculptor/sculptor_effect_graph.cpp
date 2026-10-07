// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_effect_graph.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {

using Sculptor::bounded_real_slot;
using Sculptor::connection_into;
using Sculptor::fx_param_src_dot;
using Sculptor::input_slot;
using Sculptor::int_slot;
using Sculptor::list_slot;
using Sculptor::make_slot;
using Sculptor::output_slot;
using Sculptor::real_slot;

using Sculptor::PropertyType;
using Sculptor::Slot;
using Sculptor::SlotKind;

// Effect LFO waves the runtime can evaluate (eval_lfo_mod); pulse and noise
// would trip the runtime's palette assert, and bank validation rejects them.
const char* const fx_lfo_wave_names[2] = { "Sine", "Sawtooth" };

// The six real effect types' node rows, in shader param order.  Slider
// bounds are editing aids only: the shaders clamp delay samples, chorus
// depth and FIR cutoffs themselves, so out-of-range values stay audible and
// safe.  A zero FIR cutoff disables that edge.
const Sculptor::EffectTypeInfo effect_type_table[Synth::num_effect_types] = {
    { "None", { {} }, 0 },
    { "Distortion", { { "Drive", 0.0f, 16.0f, true }, { "Mix", 0.0f, 1.0f, false }, {}, {}, {} }, 2 },
    { "Delay",
      { { "Samples", 256.0f, 44100.0f, true },
        { "Feedback", 0.0f, 0.95f, false },
        { "Mix", 0.0f, 1.0f, false },
        {},
        {} },
      3 },
    { "Chorus",
      { { "Rate Hz", 0.01f, 10.0f, true }, { "Depth", 0.0f, 64.0f, false }, { "Mix", 0.0f, 1.0f, false }, {}, {} },
      3 },
    { "Reverb",
      { { "Room", 0.0f, 1.0f, false }, { "Damping", 0.0f, 1.0f, false }, { "Wet", 0.0f, 1.0f, false }, {}, {} },
      3 },
    { "Compressor",
      { { "Threshold", 0.001f, 1.0f, true },
        { "Ratio", 1.0f, 20.0f, true },
        { "Attack", 0.0f, 1.0f, false },
        { "Release", 0.0f, 1.0f, false },
        { "Makeup", 0.0f, 4.0f, false } },
      5 },
    { "FIR", { { "Lowpass Hz", 20.0f, 22050.0f, true }, { "Highpass Hz", 20.0f, 22050.0f, true }, {}, {}, {} }, 2 },
};

// Neutral, audible defaults: a freshly added effect must change the sound
// predictably and stay DSP-safe (delay feedback below unity, compressor
// ratio at or above one, FIR highpass disabled).
const float effect_default_params[Synth::num_effect_types][Synth::max_effect_param_floats] = {
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },      // none
    { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f },      // distortion
    { 4410.0f, 0.35f, 0.35f, 0.0f, 0.0f }, // delay: 100 ms
    { 0.6f, 6.0f, 0.5f, 0.0f, 0.0f },      // chorus
    { 0.84f, 0.5f, 0.3f, 0.0f, 0.0f },     // reverb: freeverb room
    { 0.5f, 4.0f, 0.0f, 0.999f, 1.0f },    // compressor
    { 8000.0f, 0.0f, 0.0f, 0.0f, 0.0f },   // fir
};

Slot enabled_slot(bool enabled)
{
    Slot slot             = make_slot("Enabled", SlotKind::property, PropertyType::list);
    slot.num_list_options = 2;
    snprintf(slot.list_options[0], sizeof(slot.list_options[0]), "On");
    snprintf(slot.list_options[1], sizeof(slot.list_options[1]), "Off");
    slot.value.list_index = enabled ? 0 : 1;
    return slot;
}

Slot wave_slot(Synth::WaveType wave)
{
    Slot slot             = make_slot("Waveform", SlotKind::property, PropertyType::list);
    slot.num_list_options = 2;
    snprintf(slot.list_options[0], sizeof(slot.list_options[0]), "%s", fx_lfo_wave_names[0]);
    snprintf(slot.list_options[1], sizeof(slot.list_options[1]), "%s", fx_lfo_wave_names[1]);
    slot.value.list_index = wave == Synth::WaveType::sine_wave ? 0 : 1;
    return slot;
}

const Synth::EffectChainBinding& chain_at(const Synth::InstrumentBank& bank, uint32_t chain)
{
    return chain < Synth::max_channels ? bank.channel_chains[chain] : bank.master_chain;
}

// Chain slots the projection turns into nodes (skipping none-type slots).
// X position of projected node `i`; shared by effect, LFO and output nodes
// so every lane keeps the same no-overlap stride.
float fx_node_x(uint32_t i)
{
    constexpr float first_node_x = 320.0f;
    constexpr float node_step    = 560.0f;
    return first_node_x + node_step * static_cast<float>(i);
}

uint32_t projected_slots(const Synth::EffectChainBinding& chain, uint32_t* out_slots)
{
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < chain.num_effects && slot < Synth::max_chain_effects; ++slot) {
        if (chain.effects[slot].type != Synth::EffectType::none) {
            out_slots[count++] = slot;
        }
    }
    return count;
}

// Compiles one parameter's MIDI source rows back into the binding's packed
// input list: the wired rows in row order, each with the source resolved
// from the routing node's per-source dot and Op/Amount read from the row.
// Unwired rows drop out, so the list never carries a none source and never
// desyncs from the wires.
bool rebuild_param_inputs(const Sculptor::Graph&              graph,
                          const Sculptor::EffectGraphMapping& mapping,
                          uint32_t                            node_idx,
                          uint32_t                            param,
                          Synth::EffectParamBinding*          binding)
{
    const Sculptor::Node& node = graph.node(node_idx);
    if (node.slots.num_allocated <= fx_param_src_dot(param, 1) + 2) {
        return false;
    }
    uint32_t num_inputs = 0;
    for (uint32_t input = 0; input < Synth::max_mod_inputs; ++input) {
        const uint32_t conn = connection_into(graph, node_idx, fx_param_src_dot(param, input));
        if (conn == Sculptor::pool_no_slot) {
            continue;
        }
        const Sculptor::EndPoint output = graph.get_connection(conn).output;
        if (output.node_idx != mapping.midi_node) {
            return false;
        }
        uint32_t source = 0;
        for (uint32_t s = 0; s < Sculptor::fx_num_input_sources; ++s) {
            if (mapping.midi_source_slots[s] == output.slot_idx) {
                source = s + 1;
                break;
            }
        }
        if (source == 0) {
            return false;
        }
        const Sculptor::Slot& op_slot = node.slots.entries[fx_param_src_dot(param, input) + 1];
        if (op_slot.property_type != Sculptor::PropertyType::list || op_slot.value.list_index >= 2) {
            return false;
        }
        Synth::ModInput& mod_input = binding->inputs[num_inputs++];
        mod_input.source           = static_cast<Synth::ModSource>(source);
        mod_input.op               = op_slot.value.list_index == 1 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
        mod_input.scale            = node.slots.entries[fx_param_src_dot(param, input) + 2].value.real;
    }
    binding->num_inputs = static_cast<uint16_t>(num_inputs);
    return true;
}

} // anonymous namespace

// One LFO node per descriptor the chain's meaningful bindings reference or
// the caller pinned; shared by the projection and the node-count preflight
// so the two can never disagree on which descriptors cost a node.
static bool fx_lfo_is_projected(const Synth::EffectChainBinding& chain, const bool* pinned_lfos, uint32_t desc)
{
    if (pinned_lfos != nullptr && pinned_lfos[desc - 1]) {
        return true;
    }
    return Sculptor::effect_chain_uses_lfo(chain, desc);
}

bool Sculptor::parse_effect_lfo_title(const char (&name)[32], uint16_t* descriptor_id)
{
    if (! memchr(name, 0, sizeof(name)) || strncmp(name, "LFO ", 4) != 0 || ! name[4])
        return false;
    uint32_t id = 0;
    for (uint32_t digit = 4; name[digit]; ++digit) {
        if (name[digit] < '0' || name[digit] > '9' || id > Synth::max_lfos)
            return false;
        id = id * 10 + static_cast<uint32_t>(name[digit] - '0');
    }
    char expected[32];
    snprintf(expected, sizeof(expected), "LFO %u", id);
    if (strcmp(expected, name) != 0)
        return false;
    *descriptor_id = id && id <= Synth::max_lfos ? static_cast<uint16_t>(id) : 0;
    return true;
}

bool Sculptor::translate_effect_lfo_title(char (&name)[32], const uint16_t* lfo_ids, uint16_t* mapped_id)
{
    uint16_t id = 0;
    if (! parse_effect_lfo_title(name, &id))
        return false;
    *mapped_id = id ? lfo_ids[id - 1] : 0;
    if (*mapped_id)
        snprintf(name, sizeof(name), "LFO %u", *mapped_id);
    return true;
}

bool Sculptor::effect_chain_uses_lfo(const Synth::EffectChainBinding& chain, uint32_t desc)
{
    for (uint32_t slot = 0; slot < chain.num_effects; ++slot) {
        const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
        for (uint32_t param = 0; param < num_params; ++param) {
            if (chain.effects[slot].bindings[param].lfo_desc_id == desc) {
                return true;
            }
        }
    }
    return false;
}

const Sculptor::EffectTypeInfo& Sculptor::effect_type_info(Synth::EffectType type)
{
    const uint32_t index = static_cast<uint32_t>(type);
    assert(index < Synth::num_effect_types);
    return effect_type_table[index];
}

float Sculptor::effect_param_default(Synth::EffectType type, uint32_t param)
{
    const uint32_t index = static_cast<uint32_t>(type);
    assert(index < Synth::num_effect_types && param < Synth::max_effect_param_floats);
    return effect_default_params[index][param];
}

Synth::EffectChainBinding& Sculptor::fx_graph_chain(Synth::InstrumentBank* bank, uint32_t chain)
{
    assert(chain <= Synth::max_channels);
    return chain < Synth::max_channels ? bank->channel_chains[chain] : bank->master_chain;
}

int32_t Sculptor::fx_effect_slot_of(const EffectGraphMapping& mapping, uint32_t node_idx)
{
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        if (mapping.effect_nodes[slot] == node_idx) {
            return static_cast<int32_t>(slot);
        }
    }
    return -1;
}

uint32_t Sculptor::fx_lfo_desc_of(const EffectGraphMapping& mapping, uint32_t node_idx)
{
    for (uint32_t desc = 0; desc < Synth::max_lfos; ++desc) {
        if (mapping.lfo_nodes[desc] == node_idx) {
            return desc + 1;
        }
    }
    return 0;
}

// Number of nodes the chain's projection needs: fixed endpoints, one per
// projected effect slot, and one per pinned-or-referenced LFO descriptor.
// Editors preflight node-adding commands against Sculptor::max_nodes with
// this count.
uint32_t Sculptor::fx_projected_node_count(const Synth::InstrumentBank& bank, uint32_t chain, const bool* pinned_lfos)
{
    const Synth::EffectChainBinding& chain_ref = chain_at(bank, chain);
    const bool                       is_master = chain == Synth::max_channels;
    uint32_t                         effect_slots[Synth::max_chain_effects];
    uint32_t                         count = 2 + (is_master ? 0u : 1u) + projected_slots(chain_ref, effect_slots);
    for (uint32_t desc = 1; desc <= bank.lfos.num_allocated; ++desc) {
        if (fx_lfo_is_projected(chain_ref, pinned_lfos, desc)) {
            ++count;
        }
    }
    return count;
}

bool Sculptor::project_effect_chain_to_graph(const Synth::InstrumentBank& bank,
                                             uint32_t                     chain,
                                             Graph*                       graph,
                                             EffectGraphMapping*          mapping,
                                             const bool*                  pinned_lfos)
{
    assert(chain <= Synth::max_channels);
    *mapping       = EffectGraphMapping{};
    mapping->chain = chain;
    // Zero is a valid node index, so unused entries carry the pool's no-slot
    // sentinel instead of the zero-init.
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        mapping->effect_nodes[slot] = pool_no_slot;
    }
    for (uint32_t desc = 0; desc < Synth::max_lfos; ++desc) {
        mapping->lfo_nodes[desc] = pool_no_slot;
    }

    const bool                       is_master = chain == Synth::max_channels;
    const Synth::EffectChainBinding& chain_ref = chain_at(bank, chain);

    // node_state_edits_enabled is projection state: every projection
    // starts with plain nodes.  The owning pane installs its callbacks
    // after the projection returns, so a pane switch cannot leave one
    // pane's rules on another pane's nodes.
    graph->clear();
    graph->node_state_edits_enabled = false;
    // Fixed endpoints: the channel's combined output (all of the channel's
    // zones) or every channel's output for the master chain.
    mapping->input_node  = graph->create_node(is_master ? "All channels" : "Channel input", vmath::vec2(0.0f, 0.0f));
    mapping->output_node = graph->create_node(is_master ? "Master output" : "Channel output",
                                              vmath::vec2(fx_node_x(Synth::max_chain_effects), 0.0f));
    mapping->midi_node   = pool_no_slot;
    if (mapping->input_node == pool_no_slot || mapping->output_node == pool_no_slot) {
        return false;
    }
    graph->add_slot(mapping->input_node, output_slot("Out"));
    graph->add_slot(mapping->output_node, input_slot("In"));

    // The MIDI routing node, channel chains only: the master chain has no
    // channel to take MIDI from, and bank validation rejects every MIDI
    // source on it.  One output dot per channel-wide source.
    if (! is_master) {
        const uint32_t midi_node = graph->create_node("MIDI", vmath::vec2(0.0f, 320.0f));
        if (midi_node == pool_no_slot) {
            return false;
        }
        mapping->midi_node = midi_node;
        for (uint32_t source = 1; source <= fx_num_input_sources; ++source) {
            mapping->midi_source_slots[source - 1] =
                graph->add_slot(midi_node, output_slot(Sculptor::mod_source_names[source - 1]));
        }
    }

    uint32_t       slots[Synth::max_chain_effects];
    const uint32_t num_projected = projected_slots(chain_ref, slots);

    uint32_t type_ordinal[Synth::num_effect_types] = {};
    for (uint32_t i = 0; i < num_projected; ++i) {
        const uint32_t                  slot   = slots[i];
        const Synth::EffectSlotBinding& effect = chain_ref.effects[slot];
        const EffectTypeInfo&           info   = effect_type_info(effect.type);

        char           name[64];
        const uint32_t type_index = static_cast<uint32_t>(effect.type);
        ++type_ordinal[type_index];
        if (type_ordinal[type_index] > 1) {
            snprintf(name, sizeof(name), "%s %u", info.name, type_ordinal[type_index]);
        }
        else {
            snprintf(name, sizeof(name), "%s", info.name);
        }

        const uint32_t node = graph->create_node(name, vmath::vec2(fx_node_x(i), 0.0f));
        if (node == pool_no_slot) {
            return false;
        }
        mapping->effect_nodes[slot] = node;

        // In and Out share one line: the audio flows left to right through
        // the node, so the dots sit across from each other on the first row.
        Slot in_slot      = input_slot("In");
        Slot out_slot     = output_slot("Out");
        in_slot.row_group = out_slot.row_group = 1;
        graph->add_slot(node, in_slot);
        graph->add_slot(node, out_slot);
        graph->add_slot(node, enabled_slot(effect.enabled));
        for (uint32_t param = 0; param < info.num_params; ++param) {
            const EffectParamInfo&           param_info = info.params[param];
            const Synth::EffectParamBinding& binding    = effect.bindings[param];
            const uint8_t                    row_group  = static_cast<uint8_t>(2 + param);
            // The base value rides its own slider line, like an oscillator
            // parameter node's Base Value row, and stays editable while a
            // wired LFO modulates around it.
            Slot row = bounded_real_slot(param_info.name,
                                         binding.base_value,
                                         param_info.min_value,
                                         param_info.max_value,
                                         param_info.logarithmic);
            graph->add_slot(node, row);
            // The modulation row, the oscillator editor's LFO row grammar:
            // the LFO wire lands on the dot, Op picks how the wave combines
            // with the base, Depth sets the strength.  The knobs stay inert
            // until a wire arrives, so an unmodulated row never reads as
            // broken controls.
            Slot lfo_dot      = input_slot("LFO");
            lfo_dot.row_group = row_group;
            graph->add_slot(node, lfo_dot);
            Slot op =
                list_slot("Op", Sculptor::source_op_names, 2, binding.lfo_op == Synth::SourceOp::multiply ? 1u : 0u);
            op.row_group                  = row_group;
            const uint32_t op_slot_idx    = graph->add_slot(node, op);
            Slot           depth          = real_slot("Depth", binding.lfo_depth);
            depth.row_group               = row_group;
            const uint32_t depth_slot_idx = graph->add_slot(node, depth);
            if (binding.lfo_desc_id == 0) {
                graph->set_slot_edit_disabled(node, op_slot_idx, true);
                graph->set_slot_edit_disabled(node, depth_slot_idx, true);
            }
            // The MIDI source rows, the oscillator editor's Source A/B
            // grammar: a source wire lands on the dot, Op picks how the
            // source's value combines with the base, Amount scales it.
            // Rows past num_inputs show neutral defaults; the knobs stay
            // inert until a wire arrives.
            for (uint32_t input = 0; input < Synth::max_mod_inputs; ++input) {
                const uint8_t src_row_group = static_cast<uint8_t>(8 + 2 * param + input);
                Slot          src_dot       = input_slot(input == 0 ? "Source A" : "Source B");
                src_dot.row_group           = src_row_group;
                graph->add_slot(node, src_dot);
                const Synth::SourceOp src_op =
                    input < binding.num_inputs ? binding.inputs[input].op : Synth::SourceOp::add;
                Slot op_row =
                    list_slot("Op", Sculptor::source_op_names, 2, src_op == Synth::SourceOp::multiply ? 1u : 0u);
                op_row.row_group = src_row_group;
                graph->add_slot(node, op_row);
                const float src_scale  = input < binding.num_inputs ? binding.inputs[input].scale : 1.0f;
                Slot        amount_row = real_slot("Amount", src_scale);
                amount_row.row_group   = src_row_group;
                graph->add_slot(node, amount_row);
            }
        }
    }

    // Serial wires through the projected nodes; with no effects the input
    // feeds the output directly.
    uint32_t wire_from = mapping->input_node;
    uint32_t wire_slot = 0;
    for (uint32_t i = 0; i < num_projected; ++i) {
        const uint32_t slot = slots[i];
        graph->add_connection(EndPoint{ wire_from, wire_slot }, EndPoint{ mapping->effect_nodes[slot], 0 });
        wire_from = mapping->effect_nodes[slot];
        wire_slot = 1; // the effect node's Out dot
    }
    graph->add_connection(EndPoint{ wire_from, wire_slot }, EndPoint{ mapping->output_node, 0 });

    // One LFO node per descriptor the chain's meaningful bindings reference
    // or the caller pinned, deduplicated by id.  Descriptor content edits
    // fan out to every user, including oscillators and other chains.
    for (uint32_t desc = 1; desc <= bank.lfos.num_allocated; ++desc) {
        if (! fx_lfo_is_projected(chain_ref, pinned_lfos, desc)) {
            continue;
        }
        const Synth::LFODescriptor& lfo = bank.lfos.entries[desc - 1];
        char                        name[64];
        snprintf(name, sizeof(name), "LFO %u", desc);
        const vmath::vec2 lfo_pos(fx_node_x(mapping->lfo_count), 320.0f);
        const uint32_t    node = graph->create_node(name, lfo_pos);
        if (node == pool_no_slot) {
            return false;
        }
        mapping->lfo_nodes[desc - 1] = node;
        ++mapping->lfo_count;

        graph->add_slot(node, output_slot("Value"));
        graph->add_slot(node, wave_slot(lfo.wave));
        graph->add_slot(node, bounded_real_slot("Duty", static_cast<float>(lfo.duty) / 255.0f, 0.0f, 1.0f, false));
        graph->add_slot(node, int_slot("Period (ms)", lfo.period_ms));
    }

    // Reference wires: one per meaningful binding that names the descriptor,
    // landing on the effect node's parameter row so the row reads as
    // modulated.  Input endpoints accept a single connection, matching the
    // one-descriptor-per-binding schema.
    for (uint32_t desc = 1; desc <= bank.lfos.num_allocated; ++desc) {
        if (mapping->lfo_nodes[desc - 1] == pool_no_slot) {
            continue;
        }
        for (uint32_t slot = 0; slot < chain_ref.num_effects; ++slot) {
            const uint32_t num_params = Synth::get_effect_param_floats(chain_ref.effects[slot].type);
            for (uint32_t param = 0; param < num_params; ++param) {
                if (chain_ref.effects[slot].bindings[param].lfo_desc_id != desc) {
                    continue;
                }
                graph->add_connection(EndPoint{ mapping->lfo_nodes[desc - 1], 0 },
                                      EndPoint{ mapping->effect_nodes[slot], fx_param_lfo_dot(param) });
            }
        }
    }

    // MIDI source wires: one per binding input row, from the routing node's
    // per-source dot (channel chains only; the master chain has no node).
    if (mapping->midi_node != pool_no_slot) {
        for (uint32_t slot = 0; slot < chain_ref.num_effects; ++slot) {
            const uint32_t num_params = Synth::get_effect_param_floats(chain_ref.effects[slot].type);
            for (uint32_t param = 0; param < num_params; ++param) {
                const Synth::EffectParamBinding& binding = chain_ref.effects[slot].bindings[param];
                for (uint32_t input = 0; input < binding.num_inputs; ++input) {
                    const uint32_t source = static_cast<uint32_t>(binding.inputs[input].source);
                    if (source == 0 || source > fx_num_input_sources) {
                        continue; // validation guarantees this; stale rows stay unwired
                    }
                    graph->add_connection(EndPoint{ mapping->midi_node, mapping->midi_source_slots[source - 1] },
                                          EndPoint{ mapping->effect_nodes[slot], fx_param_src_dot(param, input) });
                }
            }
        }
    }

    // MIDI source rows' knobs go live exactly when a wire lands: the wires
    // above are authoritative, so the flags settle after them.
    for (uint32_t i = 0; i < num_projected; ++i) {
        const uint32_t slot       = slots[i];
        const uint32_t num_params = Synth::get_effect_param_floats(chain_ref.effects[slot].type);
        for (uint32_t param = 0; param < num_params; ++param) {
            for (uint32_t input = 0; input < Synth::max_mod_inputs; ++input) {
                const uint32_t dot_idx = fx_param_src_dot(param, input);
                const bool     wired   = graph->slot_is_connected(mapping->effect_nodes[slot], dot_idx);
                graph->set_slot_edit_disabled(mapping->effect_nodes[slot], dot_idx + 1, ! wired);
                graph->set_slot_edit_disabled(mapping->effect_nodes[slot], dot_idx + 2, ! wired);
            }
        }
    }

    return true;
}

bool Sculptor::apply_fx_graph_change(Synth::InstrumentBank*    bank,
                                     const Graph&              graph,
                                     const EffectGraphMapping& mapping,
                                     const GraphChange&        change)
{
    Synth::EffectChainBinding& chain = fx_graph_chain(bank, mapping.chain);
    const int32_t  slot = change.node_idx != pool_no_slot ? fx_effect_slot_of(mapping, change.node_idx) : -1;
    const uint32_t desc = change.node_idx != pool_no_slot ? fx_lfo_desc_of(mapping, change.node_idx) : 0;

    switch (change.kind) {
        case ChangeKind::value_changed: {
            // Events carry indices, not values: the slot's live value is the edited
            // state.  A deleted node is gone from the pool, so only value events
            // read the graph.
            const Node& node      = graph.node(change.node_idx);
            const Slot& live_slot = node.slots.entries[change.slot_idx];
            if (slot >= 0) {
                if (change.slot_idx == 2) {
                    // The two-option Enabled list: 0 = On, 1 = Off.
                    chain.effects[slot].enabled = live_slot.value.list_index == 0;
                    return true;
                }
                if (change.slot_idx < fx_param_row(0)) {
                    return false;
                }
                const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
                const uint32_t param      = (change.slot_idx - fx_param_row(0)) / fx_param_stride;
                const uint32_t field      = (change.slot_idx - fx_param_row(0)) % fx_param_stride;
                if (param >= num_params) {
                    return false;
                }
                Synth::EffectParamBinding& binding = chain.effects[slot].bindings[param];
                switch (field) {
                    case 0:
                        binding.base_value = live_slot.value.real;
                        return true;
                    case 2:
                        binding.lfo_op =
                            live_slot.value.list_index == 1 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
                        return true;
                    case 3:
                        binding.lfo_depth = live_slot.value.real;
                        return true;
                    case 5:
                    case 6:
                    case 8:
                    case 9:
                        // A MIDI source row edit: the wired rows compile
                        // back into the binding's packed input list from the
                        // graph.
                        return rebuild_param_inputs(graph, mapping, change.node_idx, param, &binding);
                    default:
                        // Fields 1, 4 and 7 are input dots: an input slot
                        // never produces value events.
                        return false;
                }
            }
            if (desc != 0) {
                if (desc > bank->lfos.num_allocated) {
                    return false;
                }
                Synth::LFODescriptor& lfo = bank->lfos.entries[desc - 1];
                if (change.slot_idx == 1) {
                    // The two-option wave list keeps the descriptor inside the runtime's
                    // sine/sawtooth palette.
                    lfo.wave =
                        live_slot.value.list_index == 0 ? Synth::WaveType::sine_wave : Synth::WaveType::sawtooth_wave;
                    return true;
                }
                if (change.slot_idx == 2) {
                    if (live_slot.value.real < 0.0f || live_slot.value.real > 1.0f) {
                        return false;
                    }
                    lfo.duty = static_cast<uint8_t>(live_slot.value.real * 255.0f + 0.5f);
                    return true;
                }
                if (change.slot_idx == 3) {
                    if (live_slot.value.integer < 1 || live_slot.value.integer > 65535) {
                        return false;
                    }
                    lfo.period_ms = static_cast<uint16_t>(live_slot.value.integer);
                    return true;
                }
                return false;
            }
            return false;
        }
        case ChangeKind::node_deleted:
            if (slot >= 0) {
                fx_remove_effect(&chain, static_cast<uint32_t>(slot));
                return true;
            }
            if (desc != 0) {
                fx_clear_lfo_references(&chain, static_cast<uint16_t>(desc));
                return true;
            }
            return false;
        case ChangeKind::connection_added:
        case ChangeKind::connection_changed:
        case ChangeKind::connection_deleted: {
            // Wire edits are the editor's reordering and binding gestures.
            // A wire that died with a node deleted earlier in the same
            // batch carries a freed endpoint: it is a byproduct of the node
            // deletion (applied separately) and must not mutate anything.
            if (! graph.node_occupied(change.connection_input.node_idx) ||
                ! graph.node_occupied(change.connection_output.node_idx)) {
                return true;
            }

            const EndPoint in_point  = change.connection_input;
            const EndPoint out_point = change.connection_output;
            const int32_t  in_slot   = fx_effect_slot_of(mapping, in_point.node_idx);
            const int32_t  out_slot  = fx_effect_slot_of(mapping, out_point.node_idx);

            // A parameter dot takes an LFO's Value wire: the wire is the
            // binding.  A retarget clears the dot it left before binding
            // the dot it landed on, so a wire pulled between parameters (or
            // between LFO nodes) moves the binding with it.
            const uint32_t param_field = in_point.slot_idx >= fx_param_row(0)
                                             ? (in_point.slot_idx - fx_param_row(0)) % fx_param_stride
                                             : fx_param_stride;
            if (in_slot >= 0 && param_field == 1) {
                if (in_point.slot_idx >= graph.node(in_point.node_idx).slots.num_allocated) {
                    return false;
                }
                const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[in_slot].type);
                const uint32_t param      = (in_point.slot_idx - fx_param_lfo_dot(0)) / fx_param_stride;
                if (param >= num_params) {
                    return false;
                }
                if (change.kind == ChangeKind::connection_deleted) {
                    chain.effects[in_slot].bindings[param].lfo_desc_id = 0;
                    return true;
                }
                const uint32_t desc_id = fx_lfo_desc_of(mapping, out_point.node_idx);
                if (desc_id == 0 || desc_id > bank->lfos.num_allocated) {
                    return false;
                }
                if (change.kind == ChangeKind::connection_changed) {
                    const EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        const int32_t  prev_slot  = fx_effect_slot_of(mapping, prev.node_idx);
                        const uint32_t prev_field = prev.slot_idx >= fx_param_row(0)
                                                        ? (prev.slot_idx - fx_param_row(0)) % fx_param_stride
                                                        : fx_param_stride;
                        if (prev_slot >= 0 && prev_field == 1) {
                            const uint32_t prev_param = (prev.slot_idx - fx_param_lfo_dot(0)) / fx_param_stride;
                            if (prev_param < Synth::get_effect_param_floats(chain.effects[prev_slot].type)) {
                                chain.effects[prev_slot].bindings[prev_param].lfo_desc_id = 0;
                            }
                        }
                    }
                }
                Synth::EffectParamBinding& binding = chain.effects[in_slot].bindings[param];
                binding.lfo_desc_id                = static_cast<uint16_t>(desc_id);
                if (binding.lfo_depth == 0.0f) {
                    binding.lfo_depth = 0.5f; // a depth-0 fresh binding would read as broken
                }
                return true;
            }

            // A MIDI source row's dot takes the routing node's per-source
            // wire: the wired rows, in row order, compile back into the
            // binding's packed input list.  Connect, retarget and disconnect
            // all rebuild from the graph, so the list can never desync from
            // the wires.
            if (in_slot >= 0 && (param_field == 4 || param_field == 7)) {
                if (in_point.slot_idx >= graph.node(in_point.node_idx).slots.num_allocated) {
                    return false;
                }
                const uint32_t param = (in_point.slot_idx - fx_param_row(0)) / fx_param_stride;
                if (param >= Synth::get_effect_param_floats(chain.effects[in_slot].type)) {
                    return false;
                }
                if (mapping.midi_node == pool_no_slot) {
                    return false; // the master chain admits no MIDI sources
                }
                if (change.kind == ChangeKind::connection_changed) {
                    // A retarget pulled the wire off another parameter's source row;
                    // that row's packed inputs must forget the source or the move
                    // becomes a copy.
                    const EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        const uint32_t prev_field = prev.slot_idx >= fx_param_row(0)
                                                        ? (prev.slot_idx - fx_param_row(0)) % fx_param_stride
                                                        : fx_param_stride;
                        if (prev_field == 4 || prev_field == 7) {
                            const uint32_t prev_param = (prev.slot_idx - fx_param_row(0)) / fx_param_stride;
                            const int32_t  prev_slot  = fx_effect_slot_of(mapping, prev.node_idx);
                            if (prev_slot >= 0 &&
                                prev_param < Synth::get_effect_param_floats(chain.effects[prev_slot].type)) {
                                if (! rebuild_param_inputs(graph,
                                                           mapping,
                                                           prev.node_idx,
                                                           prev_param,
                                                           &chain.effects[prev_slot].bindings[prev_param])) {
                                    return false;
                                }
                            }
                        }
                    }
                }
                return rebuild_param_inputs(graph,
                                            mapping,
                                            in_point.node_idx,
                                            param,
                                            &chain.effects[in_slot].bindings[param]);
            }

            // Serial wires: the wire into a node defines the node's place.
            // A new or retargeted wire places the fed effect right after
            // the source; a wire into the output node places its source at
            // the end.  A serial wire is not deletable: it carries the
            // chain's audio, so refusing the disconnect keeps every
            // effect's configuration safe behind the explicit node delete.
            const bool in_is_output_node = in_point.node_idx == mapping.output_node;
            if (! in_is_output_node && (in_slot < 0 || in_point.slot_idx != 0)) {
                return false;
            }
            if (in_is_output_node && out_slot < 0 && out_point.node_idx != mapping.input_node) {
                return false;
            }
            const bool added = change.kind != ChangeKind::connection_deleted;
            if (in_is_output_node) {
                if (out_slot < 0) {
                    return true; // the empty chain's direct wire: nothing to place
                }
                if (added) {
                    if (static_cast<uint32_t>(out_slot) + 1 == chain.num_effects) {
                        return true; // already last: the re-projection redraws the same wire
                    }
                    return Sculptor::fx_splice_effect(&chain,
                                                      static_cast<uint32_t>(out_slot),
                                                      static_cast<int32_t>(chain.num_effects) - 1);
                }
                return false; // a serial wire refuses to die: delete the effect node instead
            }
            if (added) {
                if (out_slot == in_slot) {
                    return true; // a node can never feed itself
                }
                return Sculptor::fx_splice_effect(&chain, static_cast<uint32_t>(in_slot), out_slot < 0 ? -1 : out_slot);
            }
            return false; // a serial wire refuses to die: delete the effect node instead
        }
        default:
            // Names and colors are projections the widget suppresses.
            return false;
    }
}

void Sculptor::fx_remove_effect(Synth::EffectChainBinding* chain, uint32_t slot)
{
    assert(slot < chain->num_effects);
    for (uint32_t i = slot; i + 1 < Synth::max_chain_effects; ++i) {
        chain->effects[i] = chain->effects[i + 1];
    }
    chain->effects[Synth::max_chain_effects - 1] = Synth::EffectSlotBinding{};
    --chain->num_effects;
}

bool Sculptor::fx_splice_effect(Synth::EffectChainBinding* chain, uint32_t moved_slot, int32_t after_slot)
{
    const int32_t num = static_cast<int32_t>(chain->num_effects);
    if (static_cast<int32_t>(moved_slot) >= num || after_slot >= num) {
        return false;
    }
    static Synth::EffectSlotBinding snapshot[Synth::max_chain_effects];
    uint32_t                        order[Synth::max_chain_effects];
    uint32_t                        count = 0;
    for (uint32_t slot = 0; slot < chain->num_effects; ++slot) {
        if (slot != moved_slot) {
            order[count++] = slot;
        }
    }
    uint32_t insert = 0;
    if (after_slot >= 0) {
        while (insert < count && order[insert] != static_cast<uint32_t>(after_slot)) {
            ++insert;
        }
        if (insert == count) {
            return false; // the anchor slot is not in the chain
        }
        ++insert;
    }
    const Synth::EffectSlotBinding moved = chain->effects[moved_slot];
    for (uint32_t i = 0; i < count; ++i) {
        snapshot[i] = chain->effects[order[i]];
    }
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        chain->effects[slot] = Synth::EffectSlotBinding{};
    }
    for (uint32_t i = 0; i < insert; ++i) {
        chain->effects[i] = snapshot[i];
    }
    chain->effects[insert] = moved;
    for (uint32_t i = insert; i < count; ++i) {
        chain->effects[i + 1] = snapshot[i];
    }
    return true;
}

void Sculptor::fx_init_slot(Synth::EffectChainBinding* chain, uint32_t slot, Synth::EffectType type)
{
    Synth::EffectSlotBinding& effect = chain->effects[slot];
    effect                           = Synth::EffectSlotBinding{};
    effect.type                      = type;
    for (uint32_t param = 0; param < Synth::max_effect_param_floats; ++param) {
        effect.bindings[param].base_value = effect_param_default(type, param);
    }
}

void Sculptor::fx_clear_lfo_references(Synth::EffectChainBinding* chain, uint16_t desc_id)
{
    for (uint32_t slot = 0; slot < chain->num_effects; ++slot) {
        const uint32_t num_params = Synth::get_effect_param_floats(chain->effects[slot].type);
        for (uint32_t param = 0; param < num_params; ++param) {
            if (chain->effects[slot].bindings[param].lfo_desc_id == desc_id) {
                chain->effects[slot].bindings[param].lfo_desc_id = 0;
            }
        }
    }
}

// One authority for a fresh LFO descriptor's defaults; the oscillator and
// effect editors both start from this shape.
bool Sculptor::allocate_default_lfo(Synth::InstrumentBank* bank, uint16_t* out_desc_id)
{
    if (bank->lfos.num_allocated >= Synth::max_lfos) {
        return false;
    }
    const uint32_t        desc_slot = bank->lfos.allocate();
    Synth::LFODescriptor& lfo       = bank->lfos.entries[desc_slot];
    lfo                             = Synth::LFODescriptor{};
    lfo.wave                        = Synth::WaveType::sine_wave;
    lfo.period_ms                   = 250;
    lfo.min_value                   = -1.0f;
    lfo.min_max_delta               = 2.0f;
    *out_desc_id                    = static_cast<uint16_t>(desc_slot + 1);
    return true;
}
