// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_effect_graph.h"
#include "sculptor_bank_json.h"
#include <cmath>

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
static const char* const fx_lfo_wave_names[2] = { "Sine", "Sawtooth" };

// The six real effect types' node rows, in shader param order.  Slider
// bounds are editing aids only: the shaders clamp delay samples, chorus
// depth and FIR cutoffs themselves, so out-of-range values stay audible and
// safe.  A zero FIR cutoff disables that edge.
static const Sculptor::EffectTypeInfo effect_type_table[Synth::num_effect_types] = {
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
static const float effect_default_params[Synth::num_effect_types][Synth::max_effect_param_floats] = {
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },      // none
    { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f },      // distortion
    { 4410.0f, 0.35f, 0.35f, 0.0f, 0.0f }, // delay: 100 ms
    { 0.6f, 6.0f, 0.5f, 0.0f, 0.0f },      // chorus
    { 0.84f, 0.5f, 0.3f, 0.0f, 0.0f },     // reverb: freeverb room
    { 0.5f, 4.0f, 0.0f, 0.999f, 1.0f },    // compressor
    { 8000.0f, 0.0f, 0.0f, 0.0f, 0.0f },   // fir
};

static void effect_layout_name(const Synth::EffectChainBinding& chain,
                               uint32_t                         owner,
                               uint32_t                         kind,
                               uint32_t                         index,
                               char*                            name,
                               uint32_t                         capacity);

static Slot enabled_slot(bool enabled)
{
    Slot slot             = make_slot("Enabled", SlotKind::property, PropertyType::list);
    slot.num_list_options = 2;
    snprintf(slot.list_options[0], sizeof(slot.list_options[0]), "On");
    snprintf(slot.list_options[1], sizeof(slot.list_options[1]), "Off");
    slot.value.list_index = enabled ? 0 : 1;
    return slot;
}

static Slot wave_slot(Synth::WaveType wave)
{
    Slot slot             = make_slot("Waveform", SlotKind::property, PropertyType::list);
    slot.num_list_options = 2;
    snprintf(slot.list_options[0], sizeof(slot.list_options[0]), "%s", fx_lfo_wave_names[0]);
    snprintf(slot.list_options[1], sizeof(slot.list_options[1]), "%s", fx_lfo_wave_names[1]);
    slot.value.list_index = wave == Synth::WaveType::sine_wave ? 0 : 1;
    return slot;
}

static const Synth::EffectChainBinding& chain_at(const Synth::InstrumentBank& bank, uint32_t chain)
{
    return chain < Synth::max_channels ? bank.channel_chains[chain] : bank.master_chain;
}

// Chain slots the projection turns into nodes (skipping none-type slots).
// X position of projected node `i`; shared by effect, LFO and output nodes
// so every lane keeps the same no-overlap stride.
static float fx_node_x(uint32_t i)
{
    constexpr float first_node_x = 320.0f;
    constexpr float node_step    = 560.0f;
    return first_node_x + node_step * static_cast<float>(i);
}

static vmath::vec2 fx_lfo_position(uint32_t ordinal)
{
    return vmath::vec2(fx_node_x(ordinal), 320.0f);
}

static uint32_t projected_slots(const Synth::EffectChainBinding& chain, uint32_t* out_slots)
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
static bool rebuild_param_inputs(const Sculptor::Graph&              graph,
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

bool Sculptor::project_effect_chain_to_graph(const Synth::InstrumentEditorBank& authored,
                                             uint32_t                           chain,
                                             Graph*                             graph,
                                             EffectGraphMapping*                mapping,
                                             const bool*                        pinned_lfos)
{
    assert(chain <= Synth::max_channels);
    const Synth::InstrumentBank& bank = authored.bank;
    if (! validate_effect_audio_topology(chain_at(bank, chain), authored.effect_audio[chain])) {
        return false;
    }
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

    for (uint32_t i = 0; i < num_projected; ++i) {
        const uint32_t                  slot   = slots[i];
        const Synth::EffectSlotBinding& effect = chain_ref.effects[slot];
        const EffectTypeInfo&           info   = effect_type_info(effect.type);

        char name[64];
        effect_layout_name(chain_ref, chain, Synth::effects_graph_layout_effect, slot, name, sizeof(name));
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

    if (authored.effect_audio[chain].explicit_edges) {
        for (uint32_t source = 0; source <= Synth::max_chain_effects; ++source) {
            const uint32_t next = authored.effect_audio[chain].next[source];
            if (! next) {
                continue;
            }
            const EndPoint from = source == Synth::max_chain_effects ? EndPoint{ mapping->input_node, 0 }
                                                                     : EndPoint{ mapping->effect_nodes[source], 1 };
            const EndPoint to   = next == Synth::max_chain_effects + 1 ? EndPoint{ mapping->output_node, 0 }
                                                                       : EndPoint{ mapping->effect_nodes[next - 1], 0 };
            if (graph->add_connection(from, to) == pool_no_slot) {
                return false;
            }
        }
    }
    else {
        uint32_t wire_from = mapping->input_node;
        uint32_t wire_slot = 0;
        for (uint32_t i = 0; i < num_projected; ++i) {
            const uint32_t slot = slots[i];
            graph->add_connection(EndPoint{ wire_from, wire_slot }, EndPoint{ mapping->effect_nodes[slot], 0 });
            wire_from = mapping->effect_nodes[slot];
            wire_slot = 1;
        }
        graph->add_connection(EndPoint{ wire_from, wire_slot }, EndPoint{ mapping->output_node, 0 });
    }

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
        const vmath::vec2 lfo_pos = fx_lfo_position(mapping->lfo_count);
        const uint32_t    node    = graph->create_node(name, lfo_pos);
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

static constexpr int32_t fx_not_serial     = -3;
static constexpr int32_t fx_serial_refused = -2;
static constexpr int32_t fx_serial_noop    = -1;

static int32_t fx_serial_placement(const Sculptor::Graph&              graph,
                                   const Sculptor::EffectGraphMapping& mapping,
                                   const Sculptor::GraphChange&        change,
                                   uint32_t                            effect_count,
                                   int32_t*                            after)
{
    if (change.kind == Sculptor::ChangeKind::connection_insert_before) {
        const Sculptor::EndPoint grabbed     = change.connection_prev_input;
        const Sculptor::EndPoint target      = change.connection_input;
        const int32_t            moved       = Sculptor::fx_effect_slot_of(mapping, grabbed.node_idx);
        const int32_t            destination = Sculptor::fx_effect_slot_of(mapping, target.node_idx);
        if (moved < 0 || grabbed.slot_idx != 0 || target.slot_idx != 0 || ! graph.node_occupied(grabbed.node_idx) ||
            ! graph.node_occupied(target.node_idx) || (destination < 0 && target.node_idx != mapping.output_node)) {
            return fx_serial_refused;
        }
        *after = destination < 0 ? static_cast<int32_t>(effect_count) - 1 : destination - 1;
        return moved == destination || *after == moved ? fx_serial_noop : moved;
    }
    if (change.kind != Sculptor::ChangeKind::connection_added &&
        change.kind != Sculptor::ChangeKind::connection_changed &&
        change.kind != Sculptor::ChangeKind::connection_deleted) {
        return fx_not_serial;
    }
    const Sculptor::EndPoint input  = change.connection_input;
    const Sculptor::EndPoint output = change.connection_output;
    if (! graph.node_occupied(input.node_idx) || ! graph.node_occupied(output.node_idx)) {
        return fx_serial_noop;
    }
    const int32_t input_slot  = Sculptor::fx_effect_slot_of(mapping, input.node_idx);
    const int32_t output_slot = Sculptor::fx_effect_slot_of(mapping, output.node_idx);
    if (input.node_idx == mapping.output_node) {
        if (output_slot < 0) {
            return output.node_idx == mapping.input_node ? fx_serial_noop : fx_serial_refused;
        }
        if (change.kind == Sculptor::ChangeKind::connection_deleted) {
            return fx_serial_refused;
        }
        *after = static_cast<int32_t>(effect_count) - 1;
        return output_slot == *after ? fx_serial_noop : output_slot;
    }
    if (input_slot < 0 || input.slot_idx != 0) {
        return fx_not_serial;
    }
    if (change.kind == Sculptor::ChangeKind::connection_deleted) {
        return fx_serial_refused;
    }
    *after = output_slot < 0 ? -1 : output_slot;
    return input_slot == output_slot ? fx_serial_noop : input_slot;
}

static bool splice_effect(Synth::EffectChainBinding*    chain,
                          uint32_t                      moved_slot,
                          int32_t                       after_slot,
                          Sculptor::EffectGraphMapping* mapping);

static bool apply_fx_graph_change_internal(Synth::InstrumentBank*              bank,
                                           const Sculptor::Graph&              graph,
                                           const Sculptor::EffectGraphMapping& mapping,
                                           const Sculptor::GraphChange&        change,
                                           Sculptor::EffectGraphMapping*       reordered_mapping)
{
    Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(bank, mapping.chain);
    const int32_t              slot =
        change.node_idx != Sculptor::pool_no_slot ? Sculptor::fx_effect_slot_of(mapping, change.node_idx) : -1;
    const uint32_t desc =
        change.node_idx != Sculptor::pool_no_slot ? Sculptor::fx_lfo_desc_of(mapping, change.node_idx) : 0;

    switch (change.kind) {
        case Sculptor::ChangeKind::value_changed: {
            // Events carry indices, not values: the slot's live value is the edited
            // state.  A deleted node is gone from the pool, so only value events
            // read the graph.
            const Sculptor::Node& node      = graph.node(change.node_idx);
            const Sculptor::Slot& live_slot = node.slots.entries[change.slot_idx];
            if (slot >= 0) {
                if (change.slot_idx == 2) {
                    // The two-option Enabled list: 0 = On, 1 = Off.
                    chain.effects[slot].enabled = live_slot.value.list_index == 0;
                    return true;
                }
                if (change.slot_idx < Sculptor::fx_param_row(0)) {
                    return false;
                }
                const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
                const uint32_t param      = (change.slot_idx - Sculptor::fx_param_row(0)) / Sculptor::fx_param_stride;
                const uint32_t field      = (change.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride;
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
        case Sculptor::ChangeKind::node_deleted:
            if (slot >= 0) {
                Sculptor::fx_remove_effect(&chain, static_cast<uint32_t>(slot));
                return true;
            }
            if (desc != 0) {
                Sculptor::fx_clear_lfo_references(&chain, static_cast<uint16_t>(desc));
                return true;
            }
            return false;
        case Sculptor::ChangeKind::connection_insert_before:
        case Sculptor::ChangeKind::connection_added:
        case Sculptor::ChangeKind::connection_changed:
        case Sculptor::ChangeKind::connection_deleted: {
            // Wire edits are the editor's reordering and binding gestures.
            // A wire with a freed endpoint is a byproduct of node deletion
            // (applied separately) and must not mutate anything.
            int32_t       after = -1;
            const int32_t moved = fx_serial_placement(graph, mapping, change, chain.num_effects, &after);
            if (moved != fx_not_serial) {
                return moved == fx_serial_noop ||
                       (moved >= 0 && splice_effect(&chain, static_cast<uint32_t>(moved), after, reordered_mapping));
            }
            const Sculptor::EndPoint in_point  = change.connection_input;
            const Sculptor::EndPoint out_point = change.connection_output;
            const int32_t            in_slot   = Sculptor::fx_effect_slot_of(mapping, in_point.node_idx);

            // A parameter dot takes an LFO's Value wire: the wire is the
            // binding.  A retarget clears the dot it left before binding
            // the dot it landed on, so a wire pulled between parameters (or
            // between LFO nodes) moves the binding with it.
            const uint32_t param_field =
                in_point.slot_idx >= Sculptor::fx_param_row(0)
                    ? (in_point.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                    : Sculptor::fx_param_stride;
            if (in_slot >= 0 && param_field == 1) {
                if (in_point.slot_idx >= graph.node(in_point.node_idx).slots.num_allocated) {
                    return false;
                }
                const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[in_slot].type);
                const uint32_t param = (in_point.slot_idx - Sculptor::fx_param_lfo_dot(0)) / Sculptor::fx_param_stride;
                if (param >= num_params) {
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_deleted) {
                    chain.effects[in_slot].bindings[param].lfo_desc_id = 0;
                    return true;
                }
                const uint32_t desc_id = Sculptor::fx_lfo_desc_of(mapping, out_point.node_idx);
                if (desc_id == 0 || desc_id > bank->lfos.num_allocated) {
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        const int32_t  prev_slot = Sculptor::fx_effect_slot_of(mapping, prev.node_idx);
                        const uint32_t prev_field =
                            prev.slot_idx >= Sculptor::fx_param_row(0)
                                ? (prev.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                                : Sculptor::fx_param_stride;
                        if (prev_slot >= 0 && prev_field == 1) {
                            const uint32_t prev_param =
                                (prev.slot_idx - Sculptor::fx_param_lfo_dot(0)) / Sculptor::fx_param_stride;
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
                const uint32_t param = (in_point.slot_idx - Sculptor::fx_param_row(0)) / Sculptor::fx_param_stride;
                if (param >= Synth::get_effect_param_floats(chain.effects[in_slot].type)) {
                    return false;
                }
                if (mapping.midi_node == Sculptor::pool_no_slot) {
                    return false; // the master chain admits no MIDI sources
                }
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    // A retarget pulled the wire off another parameter's source row;
                    // that row's packed inputs must forget the source or the move
                    // becomes a copy.
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        const uint32_t prev_field =
                            prev.slot_idx >= Sculptor::fx_param_row(0)
                                ? (prev.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                                : Sculptor::fx_param_stride;
                        if (prev_field == 4 || prev_field == 7) {
                            const uint32_t prev_param =
                                (prev.slot_idx - Sculptor::fx_param_row(0)) / Sculptor::fx_param_stride;
                            const int32_t prev_slot = Sculptor::fx_effect_slot_of(mapping, prev.node_idx);
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

            return false;
        }
        default:
            // Names and colors are projections the widget suppresses.
            return false;
    }
}

bool Sculptor::apply_fx_graph_change(Synth::InstrumentBank*    bank,
                                     const Graph&              graph,
                                     const EffectGraphMapping& mapping,
                                     const GraphChange&        change)
{
    return apply_fx_graph_change_internal(bank, graph, mapping, change, nullptr);
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

static bool effect_splice_order(uint32_t count, uint32_t moved_slot, int32_t after_slot, uint32_t* order)
{
    if (moved_slot >= count || after_slot >= static_cast<int32_t>(count) || after_slot < -1 ||
        after_slot == static_cast<int32_t>(moved_slot)) {
        return false;
    }
    uint32_t retained = 0;
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (slot != moved_slot) {
            order[retained++] = slot;
        }
    }
    uint32_t insert = 0;
    if (after_slot >= 0) {
        while (order[insert] != static_cast<uint32_t>(after_slot)) {
            ++insert;
        }
        ++insert;
    }
    for (uint32_t slot = retained; slot > insert; --slot) {
        order[slot] = order[slot - 1];
    }
    order[insert] = moved_slot;
    return true;
}

static bool splice_effect(Synth::EffectChainBinding*    chain,
                          uint32_t                      moved_slot,
                          int32_t                       after_slot,
                          Sculptor::EffectGraphMapping* mapping)
{
    uint32_t order[Synth::max_chain_effects];
    if (! effect_splice_order(chain->num_effects, moved_slot, after_slot, order)) {
        return false;
    }
    const Synth::EffectChainBinding snapshot = *chain;
    for (uint32_t slot = 0; slot < chain->num_effects; ++slot) {
        chain->effects[slot] = snapshot.effects[order[slot]];
    }
    if (mapping) {
        const Sculptor::EffectGraphMapping previous = *mapping;
        for (uint32_t slot = 0; slot < chain->num_effects; ++slot) {
            mapping->effect_nodes[slot] = previous.effect_nodes[order[slot]];
        }
    }
    return true;
}

bool Sculptor::fx_splice_effect(Synth::EffectChainBinding* chain, uint32_t moved_slot, int32_t after_slot)
{
    return splice_effect(chain, moved_slot, after_slot, nullptr);
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

static void effect_layout_name(const Synth::EffectChainBinding& chain,
                               uint32_t                         owner,
                               uint32_t                         kind,
                               uint32_t                         index,
                               char*                            name,
                               uint32_t                         capacity)
{
    switch (kind) {
        case Synth::effects_graph_layout_input:
            snprintf(name, capacity, "%s", owner == Sculptor::fx_master_chain ? "All channels" : "Channel input");
            break;
        case Synth::effects_graph_layout_output:
            snprintf(name, capacity, "%s", owner == Sculptor::fx_master_chain ? "Master output" : "Channel output");
            break;
        case Synth::effects_graph_layout_midi:
            snprintf(name, capacity, "MIDI");
            break;
        case Synth::effects_graph_layout_lfo:
            snprintf(name, capacity, "LFO %u", index);
            break;
        case Synth::effects_graph_layout_effect: {
            const Synth::EffectType type    = chain.effects[index].type;
            uint32_t                ordinal = 0;
            for (uint32_t slot = 0; slot <= index; ++slot) {
                if (chain.effects[slot].type == type) {
                    ++ordinal;
                }
            }
            if (ordinal > 1) {
                snprintf(name, capacity, "%s %u", Sculptor::effect_type_info(type).name, ordinal);
            }
            else {
                snprintf(name, capacity, "%s", Sculptor::effect_type_info(type).name);
            }
            break;
        }
    }
}

static bool effect_layout_identity(const Synth::InstrumentBank& bank,
                                   uint32_t                     owner,
                                   const char*                  name,
                                   uint8_t*                     kind,
                                   uint16_t*                    index)
{
    const Synth::EffectChainBinding& chain = chain_at(bank, owner);
    for (uint32_t role = 0; role <= Synth::effects_graph_layout_lfo; ++role) {
        if (role == Synth::effects_graph_layout_midi && owner == Sculptor::fx_master_chain) {
            continue;
        }
        const uint32_t first = role == Synth::effects_graph_layout_lfo ? 1 : 0;
        const uint32_t count = role == Synth::effects_graph_layout_effect ? chain.num_effects
                               : role == Synth::effects_graph_layout_lfo  ? bank.lfos.num_allocated + 1
                                                                          : 1;
        for (uint32_t identity = first; identity < count; ++identity) {
            if (role == Synth::effects_graph_layout_effect && chain.effects[identity].type == Synth::EffectType::none) {
                continue;
            }
            char expected[32];
            effect_layout_name(chain, owner, role, identity, expected, sizeof(expected));
            if (strcmp(name, expected) == 0) {
                *kind  = static_cast<uint8_t>(role);
                *index = static_cast<uint16_t>(identity);
                return true;
            }
        }
    }
    return false;
}

// Only generated effect titles can be obsolete presentation.  Descriptor titles
// remain references and must resolve, even when no parameter uses the node.
static bool obsolete_effect_title(const char* name)
{
    for (uint32_t type = 1; type < Synth::num_effect_types; ++type) {
        const char* title = Sculptor::effect_type_info(static_cast<Synth::EffectType>(type)).name;
        if (strcmp(name, title) == 0) {
            return true;
        }
        for (uint32_t ordinal = 2; ordinal <= Synth::max_chain_effects; ++ordinal) {
            char expected[32];
            snprintf(expected, sizeof(expected), "%s %u", title, ordinal);
            if (strcmp(name, expected) == 0) {
                return true;
            }
        }
    }
    return false;
}

enum class EffectLayoutClassification {
    resolved,
    obsolete,
    invalid
};

static EffectLayoutClassification classify_effect_layout(const Synth::InstrumentBank& bank,
                                                         uint32_t                     owner,
                                                         const char*                  name,
                                                         uint8_t*                     kind,
                                                         uint16_t*                    identity)
{
    if (effect_layout_identity(bank, owner, name, kind, identity)) {
        return EffectLayoutClassification::resolved;
    }
    return obsolete_effect_title(name) ? EffectLayoutClassification::obsolete : EffectLayoutClassification::invalid;
}

bool Sculptor::normalize_effect_layout(Synth::InstrumentEditorBank* bank)
{
    if (! bank || ! Synth::validate_instrument_bank(&bank->bank, false) || ! validate_editor_metadata(*bank)) {
        return false;
    }
    for (uint32_t record_index = 0; record_index < bank->graph_layout_count; ++record_index) {
        const Synth::GraphNodeLayout& record   = bank->graph_layout[record_index];
        uint8_t                       kind     = 0;
        uint16_t                      identity = 0;
        if (record.kind == 4 && classify_effect_layout(bank->bank, record.channel, record.name, &kind, &identity) ==
                                    EffectLayoutClassification::invalid) {
            return false;
        }
    }
    uint32_t retained = 0;
    for (uint32_t record_index = 0; record_index < bank->graph_layout_count; ++record_index) {
        const Synth::GraphNodeLayout& record   = bank->graph_layout[record_index];
        uint8_t                       kind     = 0;
        uint16_t                      identity = 0;
        if (record.kind != 4 || classify_effect_layout(bank->bank, record.channel, record.name, &kind, &identity) ==
                                    EffectLayoutClassification::resolved) {
            bank->graph_layout[retained++] = record;
        }
    }
    bank->graph_layout_count = retained;
    memset(bank->graph_layout + retained, 0, (Synth::max_graph_records - retained) * sizeof(Synth::GraphNodeLayout));
    return true;
}

bool Sculptor::collect_effect_lfos(const Synth::InstrumentEditorBank* source, uint32_t chain, bool* keep_lfos)
{
    if (! source || ! keep_lfos || chain > fx_master_chain || ! Synth::validate_instrument_bank(&source->bank) ||
        ! validate_editor_metadata(*source)) {
        return false;
    }
    bool collected[Synth::max_lfos];
    memcpy(collected, keep_lfos, sizeof(collected));
    const Synth::EffectChainBinding& binding = chain_at(source->bank, chain);
    for (uint32_t identity = 1; identity <= source->bank.lfos.num_allocated; ++identity) {
        if (effect_chain_uses_lfo(binding, identity)) {
            collected[identity - 1] = true;
        }
    }
    for (uint32_t record_index = 0; record_index < source->graph_layout_count; ++record_index) {
        const Synth::GraphNodeLayout& record = source->graph_layout[record_index];
        if (record.kind != 4 || record.channel != chain) {
            continue;
        }
        uint8_t                          kind     = 0;
        uint16_t                         identity = 0;
        const EffectLayoutClassification classification =
            classify_effect_layout(source->bank, chain, record.name, &kind, &identity);
        if (classification == EffectLayoutClassification::obsolete) {
            continue;
        }
        if (classification == EffectLayoutClassification::invalid) {
            return false;
        }
        if (kind == Synth::effects_graph_layout_lfo) {
            collected[identity - 1] = true;
        }
    }
    memcpy(keep_lfos, collected, sizeof(collected));
    return true;
}

bool Sculptor::transfer_effect_layout(const Synth::InstrumentEditorBank* source,
                                      uint32_t                           source_chain,
                                      const uint16_t*                    lfo_ids,
                                      uint32_t                           destination_chain,
                                      Synth::InstrumentEditorBank*       candidate)
{
    if (! source || ! candidate || ! lfo_ids || source_chain > fx_master_chain || destination_chain > fx_master_chain ||
        source->graph_layout_count > Synth::max_graph_records || source->bank.lfos.num_allocated > Synth::max_lfos ||
        chain_at(source->bank, source_chain).num_effects > Synth::max_chain_effects ||
        chain_at(candidate->bank, destination_chain).num_effects > Synth::max_chain_effects) {
        return false;
    }
    Synth::GraphNodeLayout records[Synth::effects_graph_layout_capacity];
    uint32_t               count = 0;
    for (uint32_t record_index = 0; record_index < source->graph_layout_count; ++record_index) {
        Synth::GraphNodeLayout record = source->graph_layout[record_index];
        if (record.kind != 4 || record.channel != source_chain) {
            continue;
        }
        uint8_t  kind     = 0;
        uint16_t identity = 0;
        if (! memchr(record.name, 0, sizeof(record.name))) {
            return false;
        }
        const EffectLayoutClassification classification =
            classify_effect_layout(source->bank, source_chain, record.name, &kind, &identity);
        if (classification == EffectLayoutClassification::obsolete) {
            continue;
        }
        if (classification == EffectLayoutClassification::invalid) {
            return false;
        }
        if (kind == Synth::effects_graph_layout_midi && destination_chain == fx_master_chain) {
            continue;
        }
        if (kind == Synth::effects_graph_layout_lfo) {
            identity = lfo_ids[identity - 1];
            if (! identity || identity > candidate->bank.lfos.num_allocated) {
                return false;
            }
        }
        record.channel                         = static_cast<uint8_t>(destination_chain);
        const Synth::EffectChainBinding& chain = fx_graph_chain(&candidate->bank, destination_chain);
        effect_layout_name(chain, destination_chain, kind, identity, record.name, sizeof(record.name));
        if (count == Synth::effects_graph_layout_capacity) {
            return false;
        }
        records[count++] = record;
    }
    if (candidate->graph_layout_count > Synth::max_graph_records - count) {
        return false;
    }
    memcpy(candidate->graph_layout + candidate->graph_layout_count, records, count * sizeof(records[0]));
    candidate->graph_layout_count += count;
    return true;
}

static Synth::InstrumentEditorBank effects_candidate;
static Synth::EffectsDocument      effects_document;
static Synth::InstrumentEditorBank layout_source;

static bool capture_effect_layout(const Synth::InstrumentEditorBank&  source,
                                  const Sculptor::Graph&              graph,
                                  const Sculptor::EffectGraphMapping& original,
                                  const Sculptor::EffectGraphMapping& current,
                                  const bool*                         capture_nodes,
                                  Synth::InstrumentEditorBank*        candidate)
{
    uint32_t retained = 0;
    for (uint32_t index = 0; index < candidate->graph_layout_count; ++index) {
        const Synth::GraphNodeLayout& record     = candidate->graph_layout[index];
        uint16_t                      descriptor = 0;
        const bool unprojected_root = record.kind == 4 && record.channel == current.chain &&
                                      Sculptor::parse_effect_lfo_title(record.name, &descriptor) && descriptor &&
                                      original.lfo_nodes[descriptor - 1] == Sculptor::pool_no_slot;
        if (record.kind != 4 || record.channel != current.chain || unprojected_root) {
            candidate->graph_layout[retained++] = record;
        }
    }
    candidate->graph_layout_count          = retained;
    const Synth::EffectChainBinding& chain = chain_at(candidate->bank, current.chain);
    for (uint32_t node_index = 0; node_index < Sculptor::max_nodes; ++node_index) {
        if (! graph.node_occupied(node_index)) {
            continue;
        }
        uint32_t       kind         = Synth::effects_graph_layout_input;
        uint32_t       identity     = 0;
        uint32_t       old_identity = 0;
        const int32_t  slot         = Sculptor::fx_effect_slot_of(current, node_index);
        const uint32_t descriptor   = Sculptor::fx_lfo_desc_of(current, node_index);
        if (slot >= 0) {
            kind         = Synth::effects_graph_layout_effect;
            identity     = static_cast<uint32_t>(slot);
            old_identity = static_cast<uint32_t>(Sculptor::fx_effect_slot_of(original, node_index));
        }
        else if (descriptor) {
            kind         = Synth::effects_graph_layout_lfo;
            identity     = descriptor;
            old_identity = descriptor;
        }
        else if (node_index == current.output_node) {
            kind = Synth::effects_graph_layout_output;
        }
        else if (node_index == current.midi_node) {
            kind = Synth::effects_graph_layout_midi;
        }
        else if (node_index != current.input_node) {
            return false;
        }
        char old_title[32];
        effect_layout_name(chain_at(source.bank, original.chain),
                           original.chain,
                           kind,
                           old_identity,
                           old_title,
                           sizeof(old_title));
        const Synth::GraphNodeLayout* saved = nullptr;
        for (uint32_t index = 0; index < source.graph_layout_count; ++index) {
            const Synth::GraphNodeLayout& record = source.graph_layout[index];
            if (record.kind == 4 && record.channel == original.chain && strcmp(record.name, old_title) == 0) {
                saved = &record;
                break;
            }
        }
        if (capture_nodes && ! capture_nodes[node_index] && ! saved) {
            continue;
        }
        if (candidate->graph_layout_count == Synth::max_graph_records) {
            return false;
        }
        Synth::GraphNodeLayout& record = candidate->graph_layout[candidate->graph_layout_count++];
        record                         = saved ? *saved : Synth::GraphNodeLayout{};
        if (! capture_nodes || capture_nodes[node_index]) {
            const Sculptor::Node& node = graph.node(node_index);
            record.x                   = node.position.x;
            record.y                   = node.position.y;
            record.width_override      = node.content_width_override;
            record.height_override     = node.content_height_override;
        }
        record.kind    = 4;
        record.channel = static_cast<uint8_t>(current.chain);
        effect_layout_name(chain, current.chain, kind, identity, record.name, sizeof(record.name));
    }
    memset(candidate->graph_layout + candidate->graph_layout_count,
           0,
           (Synth::max_graph_records - candidate->graph_layout_count) * sizeof(Synth::GraphNodeLayout));
    return true;
}

bool Sculptor::capture_effect_audio(const Graph&                     graph,
                                    const EffectGraphMapping&        mapping,
                                    const Synth::EffectChainBinding& chain,
                                    EffectAudioTopology*             out,
                                    const Connection*                proposed_connection,
                                    uint32_t                         excluded_connection)
{
    if (! out || ! graph.node_occupied(mapping.input_node) || ! graph.node_occupied(mapping.output_node)) {
        return false;
    }
    Sculptor::EffectAudioTopology audio         = {};
    audio.explicit_edges                        = 1;
    bool incoming[Synth::max_chain_effects + 1] = {};
    for (uint32_t index = 0; index <= Sculptor::max_connections; ++index) {
        if (index == max_connections ? ! proposed_connection
                                     : (! graph.connection_occupied(index) || index == excluded_connection)) {
            continue;
        }
        const Connection& edge = index == max_connections ? *proposed_connection : graph.get_connection(index);
        if (! graph.node_occupied(edge.output.node_idx) || ! graph.node_occupied(edge.input.node_idx) ||
            edge.output.slot_idx >= graph.node(edge.output.node_idx).slots.num_allocated ||
            edge.input.slot_idx >= graph.node(edge.input.node_idx).slots.num_allocated) {
            return false;
        }
        const int32_t source_slot = Sculptor::fx_effect_slot_of(mapping, edge.output.node_idx);
        const int32_t target_slot = Sculptor::fx_effect_slot_of(mapping, edge.input.node_idx);
        const bool    audio_source =
            edge.output.node_idx == mapping.input_node || (source_slot >= 0 && edge.output.slot_idx == 1);
        const bool audio_target =
            edge.input.node_idx == mapping.output_node || (target_slot >= 0 && edge.input.slot_idx == 0);
        if (! audio_source && ! audio_target) {
            continue;
        }
        if (! audio_source || ! audio_target || edge.output.slot_idx != (source_slot >= 0 ? 1u : 0u) ||
            edge.input.slot_idx != 0) {
            return false;
        }
        const uint32_t source = source_slot >= 0 ? static_cast<uint32_t>(source_slot) : Synth::max_chain_effects;
        const uint32_t target =
            target_slot >= 0 ? static_cast<uint32_t>(target_slot) + 1 : Synth::max_chain_effects + 1;
        if (audio.next[source] || incoming[target - 1]) {
            return false;
        }
        audio.next[source]   = static_cast<uint8_t>(target);
        incoming[target - 1] = true;
    }
    if (! Sculptor::validate_effect_audio_topology(chain, audio)) {
        return false;
    }
    *out = audio;
    return true;
}

bool Sculptor::apply_effect_graph_batch(const Synth::InstrumentEditorBank* source,
                                        const Graph*                       graph,
                                        const EffectGraphMapping*          mapping,
                                        const GraphChange*                 changes,
                                        uint32_t                           count,
                                        const bool*                        capture_nodes,
                                        Synth::InstrumentEditorBank*       out)
{
    if (! source || ! graph || ! mapping || ! out || (! changes && count) || count > max_pending_changes ||
        mapping->chain > fx_master_chain || ! Synth::validate_instrument_bank(&source->bank) ||
        ! validate_editor_metadata(*source)) {
        return false;
    }
    bool represented[Synth::max_lfos] = {};
    if (! collect_effect_lfos(source, mapping->chain, represented)) {
        return false;
    }
    layout_source              = *source;
    source                     = &layout_source;
    effects_candidate          = *source;
    EffectGraphMapping current = *mapping;
    if (! capture_nodes) {
        for (uint32_t node_index = 0; node_index < max_nodes; ++node_index) {
            if (! graph->node_occupied(node_index)) {
                continue;
            }
            const int32_t  effect_slot = fx_effect_slot_of(current, node_index);
            const uint32_t lfo         = fx_lfo_desc_of(current, node_index);
            const Node&    node        = graph->node(node_index);
            for (uint32_t slot = 0; slot < node.slots.num_allocated; ++slot) {
                if (node.slots.entries[slot].kind != SlotKind::property || (effect_slot < 0 && ! lfo)) {
                    continue;
                }
                if (effect_slot >= 0 && slot >= fx_param_row(0)) {
                    const uint32_t field = (slot - fx_param_row(0)) % fx_param_stride;
                    if (field == 5 || field == 6 || field == 8 || field == 9) {
                        continue;
                    }
                }
                GraphChange change = {};
                change.kind        = ChangeKind::value_changed;
                change.node_idx    = node_index;
                change.slot_idx    = slot;
                if (! apply_fx_graph_change(&effects_candidate.bank, *graph, current, change)) {
                    return false;
                }
            }
        }
    }

    for (uint32_t index = 0; index < count; ++index) {
        const GraphChange& change = changes[index];
        if (change.kind == ChangeKind::node_deleted && fx_effect_slot_of(current, change.node_idx) >= 0) {
            continue;
        }
        if (change.kind == ChangeKind::value_changed &&
            (! graph->node_occupied(change.node_idx) ||
             change.slot_idx >= graph->node(change.node_idx).slots.num_allocated)) {
            continue;
        }
        if (change.kind == ChangeKind::connection_insert_before) {
            return false;
        }
        if (change.kind == ChangeKind::connection_added || change.kind == ChangeKind::connection_deleted ||
            change.kind == ChangeKind::connection_changed) {
            const int32_t effect = fx_effect_slot_of(current, change.connection_input.node_idx);
            if (change.connection_input.node_idx == current.output_node ||
                (effect >= 0 && change.connection_input.slot_idx == 0)) {
                continue;
            }
        }
        if (! apply_fx_graph_change_internal(&effects_candidate.bank, *graph, current, change, &current)) {
            return false;
        }
    }
    // Descending deletion keeps later original mapping indices valid.
    for (uint32_t slot = Synth::max_chain_effects; slot > 0; --slot) {
        for (uint32_t index = 0; index < count; ++index) {
            const GraphChange& change = changes[index];
            if (change.kind != ChangeKind::node_deleted ||
                fx_effect_slot_of(current, change.node_idx) != static_cast<int32_t>(slot - 1)) {
                continue;
            }
            if (! apply_fx_graph_change(&effects_candidate.bank, *graph, current, change)) {
                return false;
            }
            for (uint32_t moved = slot; moved < Synth::max_chain_effects; ++moved) {
                current.effect_nodes[moved - 1] = current.effect_nodes[moved];
            }
            current.effect_nodes[Synth::max_chain_effects - 1] = pool_no_slot;
        }
    }
    // Capture the live final edges after descending payload/mapping compaction.
    Sculptor::EffectAudioTopology audio = {};
    if (! capture_effect_audio(*graph, current, fx_graph_chain(&effects_candidate.bank, mapping->chain), &audio)) {
        return false;
    }
    bool structural_audio = source->effect_audio[mapping->chain].explicit_edges != 0;
    for (uint32_t index = 0; index < count; ++index) {
        const GraphChange& change = changes[index];
        if (change.kind == ChangeKind::node_deleted && fx_effect_slot_of(*mapping, change.node_idx) >= 0) {
            structural_audio = true;
        }
        if ((change.kind == ChangeKind::connection_added || change.kind == ChangeKind::connection_deleted ||
             change.kind == ChangeKind::connection_changed) &&
            (change.connection_input.node_idx == mapping->output_node ||
             (fx_effect_slot_of(*mapping, change.connection_input.node_idx) >= 0 &&
              change.connection_input.slot_idx == 0))) {
            structural_audio = true;
        }
    }
    if (structural_audio) {
        effects_candidate.effect_audio[mapping->chain] = audio;
    }
    // Source metadata is read while the private candidate's chain has its new identities.
    if (! capture_effect_layout(*source, *graph, *mapping, current, capture_nodes, &effects_candidate) ||
        ! Synth::validate_instrument_bank(&effects_candidate.bank) || ! validate_editor_metadata(effects_candidate)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

bool Sculptor::stage_effect_graph_edits(const Synth::InstrumentEditorBank* source,
                                        const Graph*                       graph,
                                        const EffectGraphMapping*          mapping,
                                        Synth::InstrumentEditorBank*       out)
{
    if (! source || ! graph || ! mapping || ! out) {
        return false;
    }
    GraphChange    changes[max_pending_changes];
    bool           overflow = false;
    const uint32_t count    = graph->peek_changes(changes, max_pending_changes, &overflow);
    if (overflow) {
        return false;
    }
    return apply_effect_graph_batch(source, graph, mapping, changes, count, nullptr, out);
}

bool Sculptor::change_effect_type_candidate(const Synth::InstrumentEditorBank* source,
                                            uint32_t                           owner,
                                            uint32_t                           slot,
                                            Synth::EffectType                  type,
                                            Synth::InstrumentEditorBank*       out)
{
    if (! source || ! out || owner > fx_master_chain || ! Synth::validate_instrument_bank(&source->bank) ||
        ! validate_editor_metadata(*source) || slot >= chain_at(source->bank, owner).num_effects ||
        type == Synth::EffectType::none || static_cast<uint32_t>(type) >= Synth::num_effect_types) {
        return false;
    }
    layout_source                      = *source;
    source                             = &layout_source;
    effects_candidate                  = *source;
    Synth::EffectChainBinding& chain   = fx_graph_chain(&effects_candidate.bank, owner);
    const bool                 enabled = chain.effects[slot].enabled;
    fx_init_slot(&chain, slot, type);
    chain.effects[slot].enabled = enabled;
    uint32_t retained           = 0;
    for (uint32_t index = 0; index < source->graph_layout_count; ++index) {
        Synth::GraphNodeLayout record = source->graph_layout[index];
        if (record.kind == 4 && record.channel == owner) {
            uint8_t                          kind     = 0;
            uint16_t                         identity = 0;
            const EffectLayoutClassification classification =
                classify_effect_layout(source->bank, owner, record.name, &kind, &identity);
            if (classification == EffectLayoutClassification::obsolete) {
                continue;
            }
            if (classification == EffectLayoutClassification::invalid) {
                return false;
            }
            effect_layout_name(chain, owner, kind, identity, record.name, sizeof(record.name));
        }
        effects_candidate.graph_layout[retained++] = record;
    }
    effects_candidate.graph_layout_count = retained;
    memset(effects_candidate.graph_layout + retained,
           0,
           (Synth::max_graph_records - retained) * sizeof(Synth::GraphNodeLayout));
    if (! Synth::validate_instrument_bank(&effects_candidate.bank) || ! validate_editor_metadata(effects_candidate)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

static bool effect_node_budget(const Synth::InstrumentEditorBank* source,
                               uint32_t                           owner,
                               const bool*                        pinned_lfos,
                               uint32_t                           additional_nodes,
                               bool*                              represented)
{
    if (! source || owner > Sculptor::fx_master_chain || source->bank.lfos.num_allocated > Synth::max_lfos) {
        return false;
    }
    memset(represented, 0, Synth::max_lfos * sizeof(bool));
    if (pinned_lfos) {
        for (uint32_t identity = source->bank.lfos.num_allocated; identity < Synth::max_lfos; ++identity) {
            if (pinned_lfos[identity]) {
                return false;
            }
        }
        memcpy(represented, pinned_lfos, Synth::max_lfos * sizeof(bool));
    }
    return Sculptor::collect_effect_lfos(source, owner, represented) &&
           Sculptor::fx_projected_node_count(source->bank, owner, represented) <=
               Sculptor::max_nodes - additional_nodes;
}

bool Sculptor::fx_can_add_node(const Synth::InstrumentEditorBank* source, uint32_t owner, const bool* pinned_lfos)
{
    bool represented[Synth::max_lfos];
    return effect_node_budget(source, owner, pinned_lfos, 1, represented);
}

bool Sculptor::add_effect_lfo_candidate(const Synth::InstrumentEditorBank* source,
                                        uint32_t                           owner,
                                        Synth::InstrumentEditorBank*       out,
                                        uint16_t*                          out_descriptor,
                                        const bool*                        pinned_lfos)
{
    if (! source || ! out || ! out_descriptor || owner > fx_master_chain ||
        ! Synth::validate_instrument_bank(&source->bank) || ! validate_editor_metadata(*source) ||
        source->graph_layout_count == Synth::max_graph_records) {
        return false;
    }
    bool represented[Synth::max_lfos];
    if (! effect_node_budget(source, owner, pinned_lfos, 1, represented)) {
        return false;
    }
    effects_candidate   = *source;
    uint16_t descriptor = 0;
    if (! allocate_default_lfo(&effects_candidate.bank, &descriptor)) {
        return false;
    }
    Synth::GraphNodeLayout& record = effects_candidate.graph_layout[effects_candidate.graph_layout_count++];
    record                         = {};
    uint32_t ordinal               = 0;
    for (uint32_t identity = 1; identity < descriptor; ++identity) {
        if (represented[identity - 1]) {
            ++ordinal;
        }
    }
    const vmath::vec2 position = fx_lfo_position(ordinal);
    record.x                   = position.x;
    record.y                   = position.y;
    record.kind                = 4;
    record.channel             = static_cast<uint8_t>(owner);
    effect_layout_name(chain_at(effects_candidate.bank, owner),
                       owner,
                       Synth::effects_graph_layout_lfo,
                       descriptor,
                       record.name,
                       sizeof(record.name));
    if (! Synth::validate_instrument_bank(&effects_candidate.bank) || ! validate_editor_metadata(effects_candidate)) {
        return false;
    }
    *out            = effects_candidate;
    *out_descriptor = descriptor;
    return true;
}

bool Sculptor::extract_effect_chain_document(const Synth::InstrumentEditorBank* source,
                                             uint32_t                           chain,
                                             const Graph*                       graph,
                                             const EffectGraphMapping*          mapping,
                                             Synth::EffectsDocument*            out)
{
    if (! source || ! out || chain > fx_master_chain || (graph == nullptr) != (mapping == nullptr) ||
        (mapping && mapping->chain != chain)) {
        return false;
    }
    if (graph) {
        if (! stage_effect_graph_edits(source, graph, mapping, &effects_candidate)) {
            return false;
        }
    }
    else {
        effects_candidate = *source;
        if (! normalize_effect_layout(&effects_candidate)) {
            return false;
        }
    }
    bool keep_lfos[Synth::max_lfos] = {};
    if (! collect_effect_lfos(&effects_candidate, chain, keep_lfos)) {
        return false;
    }
    if (graph) {
        for (uint32_t identity = 0; identity < source->bank.lfos.num_allocated; ++identity) {
            const uint32_t node = mapping->lfo_nodes[identity];
            if (node != pool_no_slot && graph->node_occupied(node)) {
                keep_lfos[identity] = true;
            }
        }
    }
    Synth::EffectsDocument& document = effects_document;
    memset(&document, 0, sizeof(document));
    uint16_t lfo_ids[Synth::max_lfos] = {};
    for (uint32_t identity = 0; identity < source->bank.lfos.num_allocated; ++identity) {
        if (keep_lfos[identity]) {
            document.lfos[document.lfo_count++] = effects_candidate.bank.lfos.entries[identity];
            lfo_ids[identity]                   = static_cast<uint16_t>(document.lfo_count);
        }
    }
    Synth::remap_effect_chain(fx_graph_chain(&effects_candidate.bank, chain), lfo_ids, &document.chain);
    document.audio = effects_candidate.effect_audio[chain];
    for (uint32_t record_index = 0; record_index < effects_candidate.graph_layout_count; ++record_index) {
        const Synth::GraphNodeLayout& record = effects_candidate.graph_layout[record_index];
        if (record.kind != 4 || record.channel != chain) {
            continue;
        }
        uint8_t  kind     = 0;
        uint16_t identity = 0;
        if (! effect_layout_identity(effects_candidate.bank, chain, record.name, &kind, &identity) ||
            document.graph_layout_count == Synth::effects_graph_layout_capacity) {
            return false;
        }
        if (kind == Synth::effects_graph_layout_lfo) {
            identity = lfo_ids[identity - 1];
        }
        Synth::EffectsGraphLayout& layout = document.graph_layout[document.graph_layout_count++];
        layout.kind                       = kind;
        layout.index                      = identity;
        layout.x                          = record.x;
        layout.y                          = record.y;
        layout.width_override             = record.width_override;
        layout.height_override            = record.height_override;
    }
    if (! Synth::validate_effects_document(&document)) {
        return false;
    }
    *out = document;
    return true;
}

bool Sculptor::replace_effect_chain_candidate(const Synth::InstrumentEditorBank* source,
                                              uint32_t                           chain,
                                              const Synth::EffectsDocument*      document,
                                              Synth::InstrumentEditorBank*       out_candidate,
                                              const bool*                        pinned_lfos)
{
    if (! source || ! out_candidate || chain > fx_master_chain || ! Synth::validate_effects_document(document) ||
        ! Synth::validate_instrument_bank(&source->bank) || ! validate_editor_metadata(*source) ||
        source->bank.lfos.num_allocated > Synth::max_lfos - document->lfo_count) {
        return false;
    }
    effects_candidate                 = *source;
    uint16_t lfo_ids[Synth::max_lfos] = {};
    if (! Synth::remap_lfos(&effects_candidate.bank, document->lfos, document->lfo_count, lfo_ids)) {
        return false;
    }
    Synth::EffectChainBinding& destination = fx_graph_chain(&effects_candidate.bank, chain);
    Synth::remap_effect_chain(document->chain, lfo_ids, &destination);
    effects_candidate.effect_audio[chain] = document->audio;
    if (chain == fx_master_chain) {
        for (uint32_t slot = 0; slot < destination.num_effects; ++slot) {
            for (uint32_t param = 0; param < Synth::get_effect_param_floats(destination.effects[slot].type); ++param) {
                Synth::EffectParamBinding& binding = destination.effects[slot].bindings[param];
                binding.num_inputs                 = 0;
                memset(binding.inputs, 0, sizeof(binding.inputs));
                binding.lfo_depth_source = Synth::ModSource::none;
                binding.lfo_rate_source  = Synth::ModSource::none;
            }
        }
    }
    uint32_t retained = 0;
    for (uint32_t index = 0; index < effects_candidate.graph_layout_count; ++index) {
        const Synth::GraphNodeLayout& record = effects_candidate.graph_layout[index];
        if (record.kind != 4 || record.channel != chain) {
            effects_candidate.graph_layout[retained++] = record;
        }
    }
    effects_candidate.graph_layout_count = retained;
    // A local bank lets the same title/identity transfer serve library and clipboard payloads.
    memset(&layout_source, 0, sizeof(layout_source));
    layout_source.bank.channel_chains[0]  = document->chain;
    layout_source.bank.lfos.num_allocated = document->lfo_count;
    for (uint32_t index = 0; index < document->graph_layout_count; ++index) {
        const Synth::EffectsGraphLayout& layout = document->graph_layout[index];
        Synth::GraphNodeLayout&          record = layout_source.graph_layout[layout_source.graph_layout_count++];
        record.kind                             = 4;
        record.x                                = layout.x;
        record.y                                = layout.y;
        record.width_override                   = layout.width_override;
        record.height_override                  = layout.height_override;
        effect_layout_name(document->chain, 0, layout.kind, layout.index, record.name, sizeof(record.name));
    }
    if (! transfer_effect_layout(&layout_source, 0, lfo_ids, chain, &effects_candidate) ||
        ! Synth::validate_instrument_bank(&effects_candidate.bank) || ! validate_editor_metadata(effects_candidate)) {
        return false;
    }
    memset(effects_candidate.graph_layout + effects_candidate.graph_layout_count,
           0,
           (Synth::max_graph_records - effects_candidate.graph_layout_count) * sizeof(Synth::GraphNodeLayout));
    bool represented[Synth::max_lfos];
    if (! effect_node_budget(&effects_candidate, chain, pinned_lfos, 0, represented)) {
        return false;
    }
    *out_candidate = effects_candidate;
    return true;
}
