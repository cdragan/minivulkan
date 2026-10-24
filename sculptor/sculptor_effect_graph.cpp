// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_effect_graph.h"
#include "../synth/synth_effect_expansion.h"
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
using Sculptor::lfo_start_names;
using Sculptor::lfo_sync_names;
using Sculptor::lfo_wave_names;
using Sculptor::list_slot;
using Sculptor::make_slot;
using Sculptor::output_slot;
using Sculptor::real_slot;

using Sculptor::PropertyType;
using Sculptor::Slot;
using Sculptor::SlotKind;

namespace {
// The seven real effect types' node rows, in shader param order.  Slider
// bounds are editing aids only: the shaders clamp delay samples, chorus
// depth and FIR cutoffs themselves, so out-of-range values stay audible and
// safe.  FIR: the editor writes a zero cutoff when a lowpass slider
// reaches its 22050 Hz maximum or a highpass slider reaches its 20 Hz
// minimum, so the sliders can reach the off state.  A zero FIR cutoff
// (JSON only) disables that edge too.
static const Sculptor::EffectTypeInfo effect_type_table[Synth::num_effect_types] = {
    { "None", { {} }, 0 },
    { "Distortion",
      { { "Drive", 1.0f, 100.0f, true }, { "Mix", 0.0f, 1.0f, false }, { "Output Gain", 0.0f, 4.0f, false }, {}, {} },
      3 },
    { "Delay",
      { { "Samples", 256.0f, 44100.0f, true },
        { "Feedback", 0.0f, 0.95f, false },
        { "Mix", 0.0f, 1.0f, false },
        {},
        {} },
      3 },
    { "Chorus",
      { { "Rate Hz", 0.1f, 10.0f, true }, { "Depth", 0.0f, 64.0f, false }, { "Mix", 0.0f, 1.0f, false }, {}, {} },
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
    { "FIR",
      { { "Lowpass Hz", 20.0f, 22050.0f, true },
        { "Highpass Hz", 20.0f, 22050.0f, true },
        { "FIR Slope", 1.0f, 100.0f, false },
        {},
        {} },
      3 },
    { "Ring Modulator", { { "Rate Hz", 0.1f, 10000.0f, true }, { "Mix", 0.0f, 1.0f, false }, {}, {}, {} }, 2 },
};

// Neutral, audible defaults: a freshly added effect must change the sound
// predictably and stay DSP-safe (delay feedback below unity, compressor
// ratio at or above one, FIR highpass disabled).
static const float effect_default_params[Synth::num_effect_types][Synth::max_effect_param_floats] = {
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },      // none
    { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f },      // distortion
    { 4410.0f, 0.35f, 0.35f, 0.0f, 0.0f }, // delay: 100 ms
    { 0.6f, 6.0f, 0.5f, 0.0f, 0.0f },      // chorus
    { 0.84f, 0.5f, 0.3f, 0.0f, 0.0f },     // reverb: freeverb room
    { 0.5f, 4.0f, 0.0f, 0.999f, 1.0f },    // compressor
    { 8000.0f, 0.0f, 100.0f, 0.0f, 0.0f }, // fir: full 1025-tap window slope
    { 100.0f, 1.0f, 0.0f, 0.0f, 0.0f },    // ring_mod: 100 Hz, full mix
};
} // anonymous namespace

static void effect_layout_name(const Synth::EffectChainBinding& chain,
                               uint32_t                         owner,
                               uint32_t                         kind,
                               uint32_t                         index,
                               char*                            name,
                               uint32_t                         capacity);

static bool fx_slot_occupied(const Synth::EffectChainBinding& chain, uint32_t slot);
static void fx_delete_effect_routing(Synth::EffectChainBinding*    chain,
                                     Sculptor::EffectChainRouting* routing,
                                     uint32_t                      slot);
static bool fx_splice_effect_run(Synth::EffectChainBinding*    chain,
                                 Sculptor::EffectChainRouting* routing,
                                 uint32_t                      moved,
                                 uint32_t                      run_length,
                                 int32_t                       after_slot,
                                 Sculptor::EffectGraphMapping* mapping        = nullptr,
                                 uint32_t*                     after_slot_new = nullptr);

static Slot enabled_slot(bool enabled)
{
    // The Enabled row is a checkbox that doubles as the enable wire's input:
    // a connected row greys out and prints "wired" instead of editing.
    Slot slot        = Sculptor::toggle_slot("Enabled", enabled);
    slot.connectable = true;
    return slot;
}

static Slot wave_slot(Synth::WaveType wave)
{
    Slot slot             = make_slot("Waveform", SlotKind::property, PropertyType::list);
    slot.num_list_options = 2;
    snprintf(slot.list_options[0], sizeof(slot.list_options[0]), "%s", lfo_wave_names[0]);
    snprintf(slot.list_options[1], sizeof(slot.list_options[1]), "%s", lfo_wave_names[1]);
    slot.value.list_index = wave == Synth::WaveType::sine_wave ? 0 : 1;
    return slot;
}

static const Synth::EffectChainBinding& chain_at(const Synth::InstrumentBank& bank, uint32_t chain)
{
    return chain < Synth::max_channels ? bank.channel_chains[chain] : bank.master_chain;
}

// Chain slots the projection turns into nodes (skipping none-type slots).
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

void Sculptor::fx_cc_pair_label(uint32_t cc_number, char* name, size_t capacity)
{
    snprintf(name, capacity, "CC %u (+%u)", cc_number, cc_number + Synth::cc_lsb_base - Synth::cc_msb_base);
}

// Compiles one parameter's MIDI source rows back into the binding's packed
// input list: the wired rows in row order, each with the source resolved
// from the routing node's per-source dot and Op/Amount read from the row.
// Unwired rows drop out, so the list never carries a none source and never
// Resolves a MIDI node output dot to the source it carries: 1..fx_num_input_sources
// for the standard slots, or ModSource::cc with *cc_index naming the assignable CC
// for the added-CC dots.  *source = 0 (ModSource::none) names an unknown dot.
static void fx_source_of_dot(const Sculptor::EffectGraphMapping& mapping,
                             uint32_t                            slot_idx,
                             uint32_t*                           source,
                             uint32_t*                           cc_index)
{
    *source = 0;
    for (uint32_t s = 0; s < Sculptor::fx_num_input_sources; ++s) {
        if (mapping.midi_source_slots[s] == slot_idx) {
            *source = s + 1;
            return;
        }
    }

    // Added-CC dots sit after the standard slots: a hit names the assignable
    // CC the dot carries.
    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        if (mapping.midi_cc_slots[i] == slot_idx) {
            *source   = static_cast<uint32_t>(Synth::ModSource::cc);
            *cc_index = i;
            return;
        }
    }
}

// The 0-based assignable-CC slot index a CC number maps to (-1 when the number
// is outside CC 16..31, the only CCs an effect can wire).
static int32_t fx_assignable_cc_index(uint32_t cc_number)
{
    if (! Synth::cc_is_assignable(cc_number)) {
        return -1;
    }
    return static_cast<int32_t>(cc_number - Synth::cc_msb_base);
}

// The MIDI node output dot a wired source routes from: the standard slot for
// the fixed sources, the added-CC dot for an assignable CC.  The standard
// slots are always projected alongside the MIDI node; only added-CC dots can
// be absent, and a CC without its registry dot stays unwired - decode does
// not synthesize registry bits - so the endpoint's slot comes back
// pool_no_slot.
static Sculptor::EndPoint fx_input_endpoint(const Sculptor::EffectGraphMapping& mapping,
                                            uint32_t                            source,
                                            uint32_t                            cc_number)
{
    if (source >= 1 && source <= Sculptor::fx_num_input_sources) {
        return Sculptor::EndPoint{ mapping.midi_node, mapping.midi_source_slots[source - 1] };
    }
    if (source == static_cast<uint32_t>(Synth::ModSource::cc)) {
        const int32_t cc_index = fx_assignable_cc_index(cc_number);
        if (cc_index >= 0) {
            return Sculptor::EndPoint{ mapping.midi_node, mapping.midi_cc_slots[cc_index] };
        }
    }
    return Sculptor::EndPoint{ mapping.midi_node, Sculptor::pool_no_slot };
}

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
    // A CC input refuses the edit while its dot does not exist: a loaded
    // M1-era bank carries wired CC rows with an empty registry, and those
    // rows stay inert until the user adds the CC.  A dotted CC row is fully
    // editable.  The pre-check runs before any mutation, so a refusal
    // leaves the caller's binding byte-identical.
    for (uint32_t input = 0; input < binding->num_inputs; ++input) {
        if (binding->inputs[input].source != Synth::ModSource::cc) {
            continue;
        }
        const int32_t cc_index = fx_assignable_cc_index(binding->inputs[input].cc_number);
        if (cc_index < 0 || mapping.midi_cc_slots[cc_index] == Sculptor::pool_no_slot) {
            return false;
        }
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
        uint32_t source   = 0;
        uint32_t cc_index = 0;
        fx_source_of_dot(mapping, output.slot_idx, &source, &cc_index);
        if (source == 0) {
            return false;
        }
        const Sculptor::Slot& op_slot = node.slots.entries[fx_param_src_dot(param, input) + 1];
        if (op_slot.property_type != Sculptor::PropertyType::list || op_slot.value.list_index >= 2) {
            return false;
        }
        Synth::ModInput& mod_input = binding->inputs[num_inputs++];
        mod_input.source           = static_cast<Synth::ModSource>(source);
        mod_input.cc_number        = source == static_cast<uint32_t>(Synth::ModSource::cc)
                                         ? static_cast<uint8_t>(Synth::cc_msb_base + cc_index)
                                         : 0; // a reused slot must not carry stale CC state
        mod_input.op               = op_slot.value.list_index == 1 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
        mod_input.scale            = node.slots.entries[fx_param_src_dot(param, input) + 2].value.real;
    }
    binding->num_inputs = static_cast<uint16_t>(num_inputs);
    return true;
}

// Transactional single-binding rebuild: the rebuild writes into a local
// copy and the commit happens only on success, so a refusal leaves the
// destination binding byte-identical.
static bool rebuild_param_binding(const Sculptor::Graph&              graph,
                                  const Sculptor::EffectGraphMapping& mapping,
                                  uint32_t                            node_idx,
                                  uint32_t                            param,
                                  Synth::EffectParamBinding*          binding)
{
    Synth::EffectParamBinding rebuilt = *binding;

    if (! rebuild_param_inputs(graph, mapping, node_idx, param, &rebuilt)) {
        return false;
    }

    *binding = rebuilt;
    return true;
}

// Compiles the wire (if any) into an effect node's Enabled row
// (Sculptor::fx_enabled_row) into the slot's enable routing.  The row carries
// no Op/Amount controls, so the wired form is the neutral combine: add,
// scale 1.  The wire's source must be an added-CC dot (user ruling
// 2026-10-11): the standard channel-wide dots (bend, mod wheel, channel
// pressure) refuse here too, mirroring the editor connection validator, so a
// hand-authored graph cannot author a non-CC routing.  A CC routing refuses
// the edit while its dot does not exist, the M1 precedent that keeps a loaded
// routing inert until the user adds the CC; the pre-checks run before any
// mutation, so a refusal leaves the caller's routing byte-identical.
static bool rebuild_enable_input(const Sculptor::Graph&              graph,
                                 const Sculptor::EffectGraphMapping& mapping,
                                 uint32_t                            node_idx,
                                 const Synth::ModInput&              current,
                                 Synth::ModInput*                    out)
{
    if (current.source == Synth::ModSource::cc) {
        const int32_t cc_index = fx_assignable_cc_index(current.cc_number);
        if (cc_index < 0 || mapping.midi_cc_slots[cc_index] == Sculptor::pool_no_slot) {
            return false;
        }
    }

    Synth::ModInput rebuilt = {};

    const uint32_t conn = connection_into(graph, node_idx, Sculptor::fx_enabled_row);
    if (conn != Sculptor::pool_no_slot) {
        const Sculptor::EndPoint output = graph.get_connection(conn).output;
        if (output.node_idx != mapping.midi_node) {
            return false;
        }
        uint32_t source   = 0;
        uint32_t cc_index = 0;
        fx_source_of_dot(mapping, output.slot_idx, &source, &cc_index);

        // CC-only: a standard channel-wide dot or an unknown dot refuses.
        if (source != static_cast<uint32_t>(Synth::ModSource::cc)) {
            return false;
        }
        rebuilt.source    = Synth::ModSource::cc;
        rebuilt.cc_number = static_cast<uint8_t>(Synth::cc_msb_base + cc_index);
        if (! Synth::is_valid_effect_input(rebuilt)) {
            return false;
        }
        rebuilt.op    = Synth::SourceOp::add;
        rebuilt.scale = 1.0f;
    }

    *out = rebuilt;
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

bool Sculptor::fx_chain_uses_cc(const Synth::EffectChainBinding& chain, uint32_t cc_number)
{
    const int32_t cc_index = fx_assignable_cc_index(cc_number);
    if (cc_index < 0) {
        return false;
    }
    return (fx_chain_wired_cc_mask(chain) >> cc_index) & 1u;
}

// The wired-CC registry bit a routing input contributes: 0 when the input
// is not an assignable CC.
static uint16_t fx_cc_mask_bit(const Synth::ModInput& input)
{
    const int32_t cc_index = fx_assignable_cc_index(input.cc_number);
    if (input.source != Synth::ModSource::cc || cc_index < 0) {
        return 0;
    }
    return static_cast<uint16_t>(1u << cc_index);
}

uint16_t Sculptor::fx_chain_wired_cc_mask(const Synth::EffectChainBinding& chain)
{
    uint16_t mask = 0;
    for (uint32_t slot = 0; slot < chain.num_effects; ++slot) {
        // The enable routing joins the registry: a CC wired to the Enabled
        // row refuses removal like a wired param source.
        mask                      = static_cast<uint16_t>(mask | fx_cc_mask_bit(chain.effects[slot].enable_input));
        const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
        for (uint32_t param = 0; param < num_params; ++param) {
            const Synth::EffectParamBinding& binding = chain.effects[slot].bindings[param];
            for (uint32_t input = 0; input < binding.num_inputs; ++input) {
                mask = static_cast<uint16_t>(mask | fx_cc_mask_bit(binding.inputs[input]));
            }
        }
    }
    return mask;
}

uint16_t Sculptor::instrument_cc_mask(const Synth::Instrument& instrument)
{
    uint16_t mask = 0;

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            const Synth::TargetBinding& binding = instrument.layers[layer].targets[target];
            const uint32_t              num_inputs =
                binding.num_inputs <= Synth::max_mod_inputs ? binding.num_inputs : Synth::max_mod_inputs;

            for (uint32_t input = 0; input < num_inputs; ++input) {
                const Synth::ModInput& in = binding.inputs[input];
                if (in.source != Synth::ModSource::cc) {
                    continue;
                }
                if (! Synth::cc_is_assignable(in.cc_number)) {
                    continue;
                }
                mask = static_cast<uint16_t>(mask | (1u << (in.cc_number - Synth::cc_msb_base)));
            }
        }
    }
    return mask;
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

void Sculptor::fx_adjacent_projected_slots(const Synth::EffectChainBinding& chain,
                                           uint32_t                         slot,
                                           int32_t*                         previous,
                                           int32_t*                         next)
{
    *previous = -1;
    *next     = -1;
    uint32_t       slots[Synth::max_chain_effects];
    const uint32_t count    = projected_slots(chain, slots);
    uint32_t       position = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (slots[i] == slot) {
            position = i;
            break;
        }
    }
    if (position == count) {
        return;
    }
    if (position > 0) {
        *previous = static_cast<int32_t>(slots[position - 1]);
    }
    if (position + 1 < count) {
        *next = static_cast<int32_t>(slots[position + 1]);
    }
}

void Sculptor::fx_seed_effect_lfo_binding(Synth::EffectParamBinding* binding, uint32_t desc_id)
{
    // One descriptor-assignment and depth-seed policy serves the drop,
    // drag and canvas pathways, so the fresh-binding default can only
    // change in one place.
    binding->lfo_desc_id = static_cast<uint16_t>(desc_id);
    if (binding->lfo_depth == 0.0f) {
        binding->lfo_depth = 0.5f; // a depth-0 fresh binding would read as broken
    }
}

bool Sculptor::fx_param_is_collapsed(const Graph& graph, const uint32_t node_idx, const uint32_t param)
{
    return ! graph.slot_is_connected(node_idx, fx_param_row(param)) &&
           ! graph.slot_is_connected(node_idx, fx_param_lfo_dot(param)) &&
           ! graph.slot_is_connected(node_idx, fx_param_src_dot(param, 0)) &&
           ! graph.slot_is_connected(node_idx, fx_param_src_dot(param, 1));
}

bool Sculptor::fx_param_field_hidden(const Graph&   graph,
                                     const uint32_t node_idx,
                                     const uint32_t param,
                                     const uint32_t field)
{
    if (field == 0) {
        return false;
    }

    if (field <= 3) {
        return ! graph.slot_is_connected(node_idx, fx_param_lfo_dot(param));
    }

    if (field <= 6) {
        return ! graph.slot_is_connected(node_idx, fx_param_src_dot(param, 0));
    }

    return ! graph.slot_is_connected(node_idx, fx_param_src_dot(param, 1));
}

uint32_t Sculptor::fx_param_free_input(const Graph&   graph,
                                       const uint32_t node_idx,
                                       const uint32_t param,
                                       const bool     lfo_source)
{
    if (lfo_source) {
        return graph.slot_is_connected(node_idx, fx_param_lfo_dot(param)) ? pool_no_slot : fx_param_lfo_dot(param);
    }
    for (uint32_t input = 0; input < Synth::max_mod_inputs; ++input) {
        const uint32_t dot = fx_param_src_dot(param, input);
        if (! graph.slot_is_connected(node_idx, dot)) {
            return dot;
        }
    }
    return pool_no_slot;
}

// Number of nodes the chain's projection needs: fixed endpoints, one per
// projected effect slot, and one per pinned-or-referenced LFO descriptor.
// Editors preflight node-adding commands against Sculptor::max_nodes with
// this count.
uint32_t Sculptor::fx_projected_node_count(const Synth::InstrumentBank& bank, uint32_t chain, const bool* pinned_lfos)
{
    const Synth::EffectChainBinding& chain_ref = chain_at(bank, chain);
    uint32_t                         effect_slots[Synth::max_chain_effects];
    uint32_t                         count = 3 + projected_slots(chain_ref, effect_slots);
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
    if (! validate_effect_routing(chain_at(bank, chain), authored.effect_routing[chain])) {
        return false;
    }
    *mapping       = EffectGraphMapping{};
    mapping->chain = chain;
    // Zero is a valid node index, so unused entries carry the pool's no-slot
    // sentinel instead of the zero-init.
    mapping->midi_node = pool_no_slot;
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        mapping->effect_nodes[slot] = pool_no_slot;
    }
    for (uint32_t desc = 0; desc < Synth::max_lfos; ++desc) {
        mapping->lfo_nodes[desc] = pool_no_slot;
    }
    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        mapping->midi_cc_slots[i] = pool_no_slot;
    }

    const bool                       is_master = chain == Synth::max_channels;
    const Synth::EffectChainBinding& chain_ref = chain_at(bank, chain);
    uint32_t                         slots[Synth::max_chain_effects];
    // Computed before node creation: the output node's lane follows the
    // last projected effect.
    const uint32_t num_projected = projected_slots(chain_ref, slots);

    // node_state_edits_enabled is projection state: every projection
    // starts with plain nodes.  The owning pane installs its callbacks
    // after the projection returns, so a pane switch cannot leave one
    // pane's rules on another pane's nodes.
    graph->clear();
    graph->node_state_edits_enabled = false;
    // Fixed endpoints: the channel's combined output (all of the channel's
    // zones) or every channel's output for the master chain.  Default nodes
    // start on their lane at grid x 0; the arrangement pass after the render
    // measures spreads them 32 px edge to edge.
    mapping->input_node  = graph->create_node(is_master ? "All channels" : "Channel input", vmath::vec2(0.0f, 0.0f));
    mapping->output_node = graph->create_node(is_master ? "Master output" : "Channel output", vmath::vec2(0.0f, 0.0f));
    if (mapping->input_node == pool_no_slot || mapping->output_node == pool_no_slot) {
        return false;
    }
    graph->set_node_auto_lane(mapping->input_node, true);
    graph->set_node_auto_lane(mapping->output_node, true);
    graph->add_slot(mapping->input_node, output_slot("Out"));
    graph->add_slot(mapping->output_node, input_slot("In"));

    // The MIDI routing node: one per chain, master included (the master
    // chain's sources resolve from channel index 0).  One output dot per
    // channel-wide source.
    {
        const uint32_t midi_node = graph->create_node("MIDI Inputs", vmath::vec2(0.0f, 320.0f));
        if (midi_node == pool_no_slot) {
            return false;
        }
        mapping->midi_node = midi_node;
        for (uint32_t source = 1; source <= fx_num_input_sources; ++source) {
            mapping->midi_source_slots[source - 1] =
                graph->add_slot(midi_node, output_slot(Sculptor::mod_source_names[source - 1]));
        }

        // One output dot per added CC of the chain, ascending CC order after
        // the standard dots.  The label names the CC number pair.
        for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
            if (! (authored.added_cc_mask[chain] & (1u << i))) {
                continue;
            }
            char label[32];
            Sculptor::fx_cc_pair_label(Synth::cc_msb_base + i, label, sizeof(label));
            mapping->midi_cc_slots[i] = graph->add_slot(midi_node, output_slot(label));
        }
    }

    for (uint32_t i = 0; i < num_projected; ++i) {
        const uint32_t                  slot   = slots[i];
        const Synth::EffectSlotBinding& effect = chain_ref.effects[slot];
        const EffectTypeInfo&           info   = effect_type_info(effect.type);

        char name[64];
        effect_layout_name(chain_ref, chain, Synth::effects_graph_layout_effect, slot, name, sizeof(name));
        const uint32_t node = graph->create_node(name, vmath::vec2(0.0f, 0.0f));
        if (node == pool_no_slot) {
            return false;
        }
        graph->set_node_auto_lane(node, true);
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

            float display_value = binding.base_value;
            if (effect.type == Synth::EffectType::fir && param <= 1 && binding.base_value == 0.0f) {
                display_value = (param == 0) ? param_info.max_value : param_info.min_value;
            }

            Slot row;
            if (effect.type == Synth::EffectType::fir && param == 2) {
                // The FIR slope percent is an integer 1..100: the bounded integer
                // slider, not a bounded real row.  The editor never writes a zero
                // slope base, so no zero-cutoff display special case applies here.
                int32_t slope = static_cast<int32_t>(binding.base_value);
                if (slope < 1) {
                    slope = 1;
                }
                if (slope > 100) {
                    slope = 100;
                }
                row = Sculptor::bounded_int_slot(param_info.name, slope, 1, 100);
            }
            else {
                row = bounded_real_slot(param_info.name,
                                        display_value,
                                        param_info.min_value,
                                        param_info.max_value,
                                        param_info.logarithmic);
            }

            row.connectable = binding.lfo_desc_id == 0 || binding.num_inputs < Synth::max_mod_inputs;
            // The drop-menu anchor: the always-visible base row opens the
            // Add LFO menu while the parameter carries no wire at all and
            // modulator capacity remains.  Projection state only; the seam
            // re-checks the unconnected half at click time.
            row.drop_menu =
                binding.lfo_desc_id == 0 && binding.num_inputs == 0 && binding.num_inputs < Synth::max_mod_inputs;
            graph->add_slot(node, row);
            // The modulation row, the oscillator editor's LFO row grammar:
            // the LFO wire lands on the dot, Op picks how the wave combines
            // with the base, Depth sets the strength.  The knobs stay inert
            // until a wire arrives, so an unmodulated row never reads as
            // broken controls.
            uint32_t op_slot_idx    = 0;
            uint32_t depth_slot_idx = 0;
            Sculptor::append_lfo_row(graph,
                                     node,
                                     row_group,
                                     binding.lfo_op == Synth::SourceOp::multiply ? 1u : 0u,
                                     binding.lfo_depth,
                                     &op_slot_idx,
                                     &depth_slot_idx);
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
                const Synth::SourceOp src_op =
                    input < binding.num_inputs ? binding.inputs[input].op : Synth::SourceOp::add;
                const float src_scale = input < binding.num_inputs ? binding.inputs[input].scale : 1.0f;
                Sculptor::append_mod_source_row(graph,
                                                node,
                                                input,
                                                static_cast<uint8_t>(8 + 2 * param + input),
                                                src_op == Synth::SourceOp::multiply ? 1u : 0u,
                                                src_scale);
            }
        }
    }

    // Wires derive from the routing flags plus slot order.
    {
        const Sculptor::EffectChainRouting& routing = authored.effect_routing[chain];
        if (routing.in == effect_in_first_slot && mapping->effect_nodes[0] != pool_no_slot) {
            graph->add_connection(EndPoint{ mapping->input_node, 0 }, EndPoint{ mapping->effect_nodes[0], 0 });
        }
        if (routing.in == effect_in_output) {
            graph->add_connection(EndPoint{ mapping->input_node, 0 }, EndPoint{ mapping->output_node, 0 });
        }
        for (uint32_t slot = 0; slot < chain_ref.num_effects && slot < Synth::max_chain_effects; ++slot) {
            if (mapping->effect_nodes[slot] == pool_no_slot ||
                chain_ref.effects[slot].type == Synth::EffectType::none) {
                continue;
            }
            if (routing.out[slot] == effect_out_next && slot + 1 < Synth::max_chain_effects &&
                mapping->effect_nodes[slot + 1] != pool_no_slot) {
                graph->add_connection(EndPoint{ mapping->effect_nodes[slot], 1 },
                                      EndPoint{ mapping->effect_nodes[slot + 1], 0 });
            }
            else if (routing.out[slot] == effect_out_output) {
                graph->add_connection(EndPoint{ mapping->effect_nodes[slot], 1 }, EndPoint{ mapping->output_node, 0 });
            }
        }
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
        // Creation place: the LFO lane y with a placeholder x; the node is
        // auto-lane, so the arrangement pass (same frame, once the renderer
        // has measured widths) or a restored layout record owns its x.  A
        // fresh default node with no record is auto-lane and the
        // arrangement sets its x after the renderer measures it; the MIDI
        // node stays a fixed anchor.
        const uint32_t node = graph->create_node(name, vmath::vec2(0.0f, 320.0f));
        if (node == pool_no_slot) {
            return false;
        }
        graph->set_node_auto_lane(node, true);
        mapping->lfo_nodes[desc - 1] = node;
        ++mapping->lfo_count;

        graph->add_slot(node, output_slot("Value"));
        graph->add_slot(node, wave_slot(lfo.wave));
        // Duty shapes sawtooth only: sine has no phase to widen.
        const uint32_t duty_idx =
            graph->add_slot(node, bounded_real_slot("Duty", static_cast<float>(lfo.duty) / 255.0f, 0.0f, 1.0f, false));
        graph->set_slot_edit_disabled(node, duty_idx, lfo.wave == Synth::WaveType::sine_wave);
        const uint32_t period_idx = graph->add_slot(node, int_slot("Period (ms)", lfo.period_ms));
        graph->set_slot_edit_disabled(node, period_idx, lfo.beat_div != 0);
        // Shared slot order with the oscillator graph's LFO node
        // (sculptor_osc_graph.cpp); the dispatch below is index-based.
        graph->add_slot(node,
                        bounded_real_slot("Pulse", static_cast<float>(lfo.pulse_mix) / 255.0f, 0.0f, 1.0f, false));
        graph->add_slot(node, list_slot("Sync", lfo_sync_names, 6, lfo.beat_div <= 5 ? lfo.beat_div : 0));
        graph->add_slot(node, list_slot("Start", lfo_start_names, 3, lfo.start_point <= 2 ? lfo.start_point : 0));
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
    // per-source dot.
    if (mapping->midi_node != pool_no_slot) {
        for (uint32_t slot = 0; slot < chain_ref.num_effects; ++slot) {
            const uint32_t num_params = Synth::get_effect_param_floats(chain_ref.effects[slot].type);
            for (uint32_t param = 0; param < num_params; ++param) {
                const Synth::EffectParamBinding& binding = chain_ref.effects[slot].bindings[param];
                for (uint32_t input = 0; input < binding.num_inputs; ++input) {
                    const Sculptor::EndPoint from =
                        fx_input_endpoint(*mapping,
                                          static_cast<uint32_t>(binding.inputs[input].source),
                                          binding.inputs[input].cc_number);
                    if (from.slot_idx == pool_no_slot) {
                        continue;
                    }
                    graph->add_connection(from,
                                          EndPoint{ mapping->effect_nodes[slot], fx_param_src_dot(param, input) });
                }
            }
        }

        // The enable routing wires from its source dot onto the Enabled row.
        for (uint32_t slot = 0; slot < chain_ref.num_effects; ++slot) {
            if (mapping->effect_nodes[slot] == pool_no_slot) {
                continue;
            }
            const Synth::ModInput&   enable = chain_ref.effects[slot].enable_input;
            const Sculptor::EndPoint from =
                fx_input_endpoint(*mapping, static_cast<uint32_t>(enable.source), enable.cc_number);
            if (from.slot_idx == pool_no_slot) {
                continue;
            }
            graph->add_connection(from, EndPoint{ mapping->effect_nodes[slot], Sculptor::fx_enabled_row });
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

// The auto-lane layout computed without touching the graph: the arranged x
// of every effects-lane node, the LFO lane's arranged x per LFO ordinal, and
// the x one further LFO would take.  The arrangement pass applies the plan;
// the canvas add path freezes a new LFO's record at next_lfo_x.
struct FxLanePlan {
    uint32_t lane_count; // effects-lane nodes, in walk order
    uint32_t lane_nodes[Synth::max_chain_effects + 2];
    float    lane_x[Synth::max_chain_effects + 2];
    uint32_t effect_count; // effects on the lane (LFO column anchors)
    uint32_t lfo_count;    // projected LFOs, in descriptor order
    uint32_t lfo_nodes[Synth::max_lfos];
    float    lfo_x[Synth::max_lfos];
    float    next_lfo_x; // one further LFO's arranged x
};

// Effects lane (y 0): the input node, the effects by slot order, the
// output node.  Each arranged node sits exactly 32 px (two grid squares)
// right of the right edge of everything before it on the lane, with no
// grid snapping: a node whose measured width is not a grid multiple
// lands between grid lines (the exact 32 px edge-to-edge gaps win over
// grid snapping).  The first lane node keeps the projection's place.  A
// non-auto node (a restored layout record) keeps its place and anchors
// the chain, so a user-moved box is never auto-arranged again.  LFO lane
// (y 320): LFO i takes effect i's arranged x so the two lanes stay
// column-aligned; LFOs past the effect count keep stepping 32 px past
// the previous LFO, and the fixed MIDI node anchors the first.
static bool plan_fx_auto_lane(const Sculptor::Graph&              graph,
                              const Sculptor::EffectGraphMapping& mapping,
                              const float*                        widths,
                              FxLanePlan*                         plan)
{
    constexpr float fx_lane_gap = 2.0f * Sculptor::graph_grid_spacing; // 32 px edge to edge

    if (! widths || ! plan || mapping.input_node == Sculptor::pool_no_slot ||
        mapping.output_node == Sculptor::pool_no_slot) {
        return false;
    }
    *plan                     = FxLanePlan{};
    uint32_t count            = 0;
    plan->lane_nodes[count++] = mapping.input_node;
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        const uint32_t node = mapping.effect_nodes[slot];
        if (node != Sculptor::pool_no_slot && graph.node_occupied(node)) {
            plan->lane_nodes[count++] = node;
        }
    }
    plan->lane_nodes[count++] = mapping.output_node;
    plan->lane_count          = count;
    plan->effect_count        = count - 2;

    bool  have_right = false;
    float right_edge = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t node_idx = plan->lane_nodes[i];
        const float    width    = widths[node_idx];
        if (! graph.node_auto_lane(node_idx)) {
            // A saved layout wins; its right edge anchors the next arranged
            // node, so the walk stays monotone even across a left-moved box.
            const float node_x     = graph.node(node_idx).position.x;
            const float node_right = node_x + width;
            if (! have_right || node_right > right_edge) {
                right_edge = node_right;
            }
            have_right      = true;
            plan->lane_x[i] = node_x;
            continue;
        }
        if (! have_right) {
            // The first lane node keeps the projection's place and anchors
            // the walk; every later node steps exactly 32 px past the right
            // edge of everything before it.
            const float node_x = graph.node(node_idx).position.x;
            right_edge         = node_x + width;
            have_right         = true;
            plan->lane_x[i]    = node_x;
            continue;
        }
        const float x   = right_edge + fx_lane_gap;
        right_edge      = x + width;
        plan->lane_x[i] = x;
    }

    bool  lfo_prev  = false;
    float lfo_right = 0.0f;
    if (mapping.midi_node != Sculptor::pool_no_slot && graph.node_occupied(mapping.midi_node)) {
        lfo_right = graph.node(mapping.midi_node).position.x + widths[mapping.midi_node];
        lfo_prev  = true;
    }
    uint32_t lfo_ord = 0;
    for (uint32_t desc = 0; desc < Synth::max_lfos; ++desc) {
        const uint32_t node_idx = mapping.lfo_nodes[desc];
        if (node_idx == Sculptor::pool_no_slot || ! graph.node_occupied(node_idx)) {
            continue;
        }
        const float width = widths[node_idx];
        if (! graph.node_auto_lane(node_idx)) {
            const float node_x     = graph.node(node_idx).position.x;
            const float node_right = node_x + width;
            if (! lfo_prev || node_right > lfo_right) {
                lfo_right = node_right;
            }
            plan->lfo_nodes[lfo_ord] = node_idx;
            plan->lfo_x[lfo_ord]     = node_x;
            lfo_prev                 = true;
            ++lfo_ord;
            continue;
        }
        const float x            = lfo_ord < plan->effect_count ? plan->lane_x[lfo_ord + 1] : lfo_right + fx_lane_gap;
        plan->lfo_nodes[lfo_ord] = node_idx;
        plan->lfo_x[lfo_ord]     = x;
        lfo_right                = x + width;
        lfo_prev                 = true;
        ++lfo_ord;
    }
    plan->lfo_count  = lfo_ord;
    plan->next_lfo_x = lfo_ord < plan->effect_count ? plan->lane_x[lfo_ord + 1] : lfo_right + fx_lane_gap;
    return true;
}

// Applies the planned default arrangement to the graph's auto-lane nodes.
// `widths` is indexed by node idx so unit tests drive the pass headless
// with synthetic widths; the pane passes the sizes the renderer just
// measured.  Returns true when any node moved.
bool Sculptor::fx_arrange_auto_lane(Graph* graph, const Sculptor::EffectGraphMapping& mapping, const float* widths)
{
    if (! graph) {
        return false;
    }
    FxLanePlan plan;
    if (! plan_fx_auto_lane(*graph, mapping, widths, &plan)) {
        return false;
    }
    bool moved = false;
    for (uint32_t i = 0; i < plan.lane_count; ++i) {
        const uint32_t node_idx = plan.lane_nodes[i];
        const Node&    node     = graph->node(node_idx);
        if (! graph->node_auto_lane(node_idx) || plan.lane_x[i] == node.position.x) {
            continue;
        }
        graph->place_auto_lane_node(node_idx, vmath::vec2(plan.lane_x[i], node.position.y));
        moved = true;
    }
    for (uint32_t i = 0; i < plan.lfo_count; ++i) {
        const uint32_t node_idx = plan.lfo_nodes[i];
        const Node&    node     = graph->node(node_idx);
        if (! graph->node_auto_lane(node_idx) || plan.lfo_x[i] == node.position.x) {
            continue;
        }
        graph->place_auto_lane_node(node_idx, vmath::vec2(plan.lfo_x[i], node.position.y));
        moved = true;
    }
    return moved;
}

// The measured default x one further LFO node would take on the chain's
// LFO lane (the arrangement rule above): the canvas add freezes a new
// LFO's record at that measured add-time place, so the added box lands
// in the measured lane.  Widths contract matches
// fx_arrange_auto_lane; false when the lane cannot be laid out.
bool Sculptor::fx_next_lfo_lane_x(const Graph&              graph,
                                  const EffectGraphMapping& mapping,
                                  const float*              widths,
                                  float*                    out_x)
{
    if (! out_x) {
        return false;
    }
    FxLanePlan plan;
    if (! plan_fx_auto_lane(graph, mapping, widths, &plan)) {
        return false;
    }
    *out_x = plan.next_lfo_x;
    return true;
}

// A retarget that pulled a wire off an effect's Enabled row drops that
// effect's enable routing: the move must not leave a copy behind.
static void fx_drop_enabled_routing(Synth::EffectChainBinding& chain, int32_t prev_slot)
{
    if (prev_slot >= 0) {
        chain.effects[prev_slot].enable_input = Synth::ModInput{};
    }
}

// A retarget pulled the wire off a parameter's source dot: that row's
// packed inputs must forget the source or the move becomes a copy.  When
// the previous endpoint is a source dot of an in-range parameter, rebuilds
// that row from the graph and commits it; a caller that still owes its own
// side a commit must rebuild it into a local copy beforehand, so a refusal
// anywhere leaves the bank unchanged.  Returns false when the release
// rebuild refuses; true otherwise, with *released saying whether a row was
// released.
static bool fx_release_prev_source(const Sculptor::Graph&              graph,
                                   const Sculptor::EffectGraphMapping& mapping,
                                   Synth::EffectChainBinding&          chain,
                                   const Sculptor::EndPoint&           prev,
                                   bool*                               released)
{
    *released = false;

    const uint32_t prev_field = prev.slot_idx >= Sculptor::fx_param_row(0)
                                    ? (prev.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                                    : Sculptor::fx_param_stride;
    if (prev_field != 4 && prev_field != 7) {
        return true;
    }

    const uint32_t prev_param = (prev.slot_idx - Sculptor::fx_param_row(0)) / Sculptor::fx_param_stride;
    const int32_t  prev_slot  = Sculptor::fx_effect_slot_of(mapping, prev.node_idx);
    if (prev_slot < 0 || prev_param >= Synth::get_effect_param_floats(chain.effects[prev_slot].type)) {
        return true;
    }

    Synth::EffectParamBinding prev_binding = chain.effects[prev_slot].bindings[prev_param];
    if (! rebuild_param_inputs(graph, mapping, prev.node_idx, prev_param, &prev_binding)) {
        return false;
    }
    chain.effects[prev_slot].bindings[prev_param] = prev_binding;
    *released                                     = true;
    return true;
}

static bool apply_fx_graph_change_internal(Synth::InstrumentBank*              bank,
                                           const Sculptor::Graph&              graph,
                                           const Sculptor::EffectGraphMapping& mapping,
                                           const Sculptor::GraphChange&        change,
                                           Sculptor::EffectGraphMapping*       reordered_mapping,
                                           Sculptor::EffectChainRouting*       routing)
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
                if (change.slot_idx == Sculptor::fx_enabled_row) {
                    // The Enabled checkbox writes the saved state; a wired
                    // row greys out and emits no value events.
                    chain.effects[slot].enabled = live_slot.value.toggle;
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
                    case 0: {
                        // FIR edges turn off at their slider extremes: at the 20 Hz
                        // minimum the 1025-tap kernel cannot resolve the edge and the
                        // result is silence, and at the 22050 Hz maximum the edge is
                        // transparent. Both extremes write the zero cutoff "off" state,
                        // for lowpass and highpass alike.
                        // The synth keeps the raw Hz values untouched.
                        float base_value = live_slot.value.real;
                        if (chain.effects[slot].type == Synth::EffectType::fir) {
                            if (param == 2) {
                                // The FIR slope percent is an integer 1..100; its slider
                                // extremes are valid values, not an "off" state, so the
                                // zero-writing rule below never fires for it.
                                int32_t slope = live_slot.value.integer;
                                if (slope < 1) {
                                    slope = 1;
                                }
                                if (slope > 100) {
                                    slope = 100;
                                }
                                binding.base_value = static_cast<float>(slope);
                                return true;
                            }
                            const Sculptor::EffectTypeInfo& fir_info =
                                Sculptor::effect_type_info(Synth::EffectType::fir);
                            const Sculptor::EffectParamInfo& fir_param = fir_info.params[param];
                            const bool                       at_extreme =
                                base_value <= fir_param.min_value || base_value >= fir_param.max_value;
                            if (param <= 1 && at_extreme) {
                                base_value = 0.0f;
                            }
                        }
                        binding.base_value = base_value;
                        return true;
                    }
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
                        // back into the binding's packed input list from
                        // the graph.
                        return rebuild_param_binding(graph, mapping, change.node_idx, param, &binding);
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
                    if (! (live_slot.value.real >= 0.0f && live_slot.value.real <= 1.0f)) {
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
                if (change.slot_idx == 4) {
                    if (! (live_slot.value.real >= 0.0f && live_slot.value.real <= 1.0f)) {
                        return false;
                    }
                    lfo.pulse_mix = static_cast<uint8_t>(live_slot.value.real * 255.0f + 0.5f);
                    return true;
                }
                if (change.slot_idx == 5) {
                    if (live_slot.value.list_index >= 6) {
                        return false;
                    }
                    lfo.beat_div = static_cast<uint8_t>(live_slot.value.list_index);
                    return true;
                }
                if (change.slot_idx == 6) {
                    if (live_slot.value.list_index >= 3) {
                        return false;
                    }
                    lfo.start_point = static_cast<uint8_t>(live_slot.value.list_index);
                    return true;
                }
                return false;
            }
            return false;
        }
        case Sculptor::ChangeKind::node_deleted:
            if (slot >= 0) {
                if (! routing) {
                    return false;
                }
                // Delete heal plus flag compaction; the batch shifts the mapping.
                fx_delete_effect_routing(&chain, routing, static_cast<uint32_t>(slot));
                return true;
            }
            if (desc != 0) {
                Sculptor::fx_clear_lfo_references(&chain, static_cast<uint16_t>(desc));
                return true;
            }
            return false;
        case Sculptor::ChangeKind::connection_added:
        case Sculptor::ChangeKind::connection_changed:
        case Sculptor::ChangeKind::connection_deleted: {
            // Wire edits are the editor's reordering and binding gestures.
            // A wire with a freed endpoint is a byproduct of node deletion
            // (applied separately) and must not mutate anything; the batch
            // pre-filter drops those.
            const Sculptor::EndPoint in_point  = change.connection_input;
            const Sculptor::EndPoint out_point = change.connection_output;
            const int32_t            in_slot   = Sculptor::fx_effect_slot_of(mapping, in_point.node_idx);
            const int32_t            out_slot  = Sculptor::fx_effect_slot_of(mapping, out_point.node_idx);

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
                        else if (prev.slot_idx == Sculptor::fx_enabled_row) {
                            // The wire left another effect's Enabled row; its
                            // routing drops with it.
                            fx_drop_enabled_routing(chain, prev_slot);
                        }
                    }
                }
                Synth::EffectParamBinding& binding = chain.effects[in_slot].bindings[param];
                Sculptor::fx_seed_effect_lfo_binding(&binding, desc_id);
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
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    // A retarget pulled the wire off another parameter's source row;
                    // that row's packed inputs must forget the source or the move
                    // becomes a copy.
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        if (prev.slot_idx == Sculptor::fx_enabled_row) {
                            const int32_t prev_slot = Sculptor::fx_effect_slot_of(mapping, prev.node_idx);
                            if (prev_slot >= 0) {
                                Synth::EffectParamBinding dest_binding = chain.effects[in_slot].bindings[param];
                                if (! rebuild_param_inputs(graph, mapping, in_point.node_idx, param, &dest_binding)) {
                                    return false;
                                }
                                // The wire left the Enabled row; its routing
                                // drops with it.
                                fx_drop_enabled_routing(chain, prev_slot);
                                chain.effects[in_slot].bindings[param] = dest_binding;
                                return true;
                            }
                        }
                        else {
                            // This side rebuilds into a local copy before the
                            // release below commits the previous row: both
                            // sides rebuild before either commits, so a
                            // refusal leaves the bank unchanged.
                            Synth::EffectParamBinding dest_binding = chain.effects[in_slot].bindings[param];
                            if (! rebuild_param_inputs(graph, mapping, in_point.node_idx, param, &dest_binding)) {
                                return false;
                            }
                            bool released = false;
                            if (! fx_release_prev_source(graph, mapping, chain, prev, &released)) {
                                return false;
                            }
                            if (released) {
                                chain.effects[in_slot].bindings[param] = dest_binding;
                                return true;
                            }
                        }
                    }
                }
                Synth::EffectParamBinding& dest = chain.effects[in_slot].bindings[param];
                return rebuild_param_binding(graph, mapping, in_point.node_idx, param, &dest);
            }

            // The Enabled row's wire is the enable routing: connect, retarget
            // and disconnect compile it from the graph, so the routing can
            // never desync from the wires.
            if (in_slot >= 0 && in_point.slot_idx == Sculptor::fx_enabled_row) {
                if (in_point.slot_idx >= graph.node(in_point.node_idx).slots.num_allocated) {
                    return false;
                }
                if (mapping.midi_node == Sculptor::pool_no_slot) {
                    return false;
                }
                Synth::ModInput rebuilt = {};
                if (! rebuild_enable_input(graph,
                                           mapping,
                                           in_point.node_idx,
                                           chain.effects[in_slot].enable_input,
                                           &rebuilt)) {
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    // A retarget pulled the wire off another input endpoint;
                    // that endpoint's owned state must forget the wire or the
                    // move becomes a copy.
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        // The routing itself rebuilt into a local copy above,
                        // so a refused release refuses the whole change.
                        bool released = false;
                        if (! fx_release_prev_source(graph, mapping, chain, prev, &released)) {
                            return false;
                        }
                        if (prev.slot_idx == Sculptor::fx_enabled_row) {
                            // The wire moved between Enabled rows.
                            fx_drop_enabled_routing(chain, Sculptor::fx_effect_slot_of(mapping, prev.node_idx));
                        }
                    }
                }
                chain.effects[in_slot].enable_input = rebuilt;
                return true;
            }

            // A collapsed parameter's inline value-row dot (the connectable
            // base-value row): the wire stands in for the row's first free
            // modulator input, so the binding lands there directly - an LFO
            // output fills the LFO input (fresh depth default), a
            // channel-wide MIDI source or added CC appends the first free
            // source row (neutral add/scale).  The dot stays while any
            // modulator family has a free input, so the append cannot split
            // the packed input list.  A disconnect never observes a value-row
            // endpoint (the apply-side bind and the re-projection that
            // re-lands the wire land in the same drain), so deletes refuse.
            if (in_slot >= 0 && param_field == 0) {
                if (in_point.slot_idx >= graph.node(in_point.node_idx).slots.num_allocated) {
                    return false;
                }
                const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[in_slot].type);
                const uint32_t param      = (in_point.slot_idx - Sculptor::fx_param_row(0)) / Sculptor::fx_param_stride;
                if (param >= num_params) {
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_deleted) {
                    return false;
                }
                const uint32_t desc_id  = Sculptor::fx_lfo_desc_of(mapping, out_point.node_idx);
                const bool     lfo_wire = desc_id != 0;
                if (! lfo_wire &&
                    (mapping.midi_node == Sculptor::pool_no_slot || out_point.node_idx != mapping.midi_node)) {
                    return false;
                }
                if (Sculptor::fx_param_free_input(graph, in_point.node_idx, param, lfo_wire) ==
                    Sculptor::pool_no_slot) {
                    return false;
                }
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    // A retarget pulled the wire off another input endpoint;
                    // that endpoint's owned state must forget the wire or
                    // the move becomes a copy.
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    if (graph.node_occupied(prev.node_idx) &&
                        (prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx)) {
                        const uint32_t prev_field =
                            prev.slot_idx >= Sculptor::fx_param_row(0)
                                ? (prev.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                                : Sculptor::fx_param_stride;
                        const int32_t prev_slot = Sculptor::fx_effect_slot_of(mapping, prev.node_idx);
                        if (prev_slot >= 0 && prev_field == 1) {
                            const uint32_t prev_param =
                                (prev.slot_idx - Sculptor::fx_param_lfo_dot(0)) / Sculptor::fx_param_stride;
                            if (prev_param < Synth::get_effect_param_floats(chain.effects[prev_slot].type)) {
                                chain.effects[prev_slot].bindings[prev_param].lfo_desc_id = 0;
                            }
                        }
                        else if (prev.slot_idx == Sculptor::fx_enabled_row) {
                            fx_drop_enabled_routing(chain, prev_slot);
                        }
                        else {
                            bool released = false;
                            if (! fx_release_prev_source(graph, mapping, chain, prev, &released)) {
                                return false;
                            }
                        }
                    }
                }
                Synth::EffectParamBinding& binding = chain.effects[in_slot].bindings[param];
                if (lfo_wire) {
                    if (desc_id > bank->lfos.num_allocated) {
                        return false;
                    }
                    Sculptor::fx_seed_effect_lfo_binding(&binding, desc_id);
                    return true;
                }
                if (binding.num_inputs >= Synth::max_mod_inputs) {
                    return false;
                }
                uint32_t source   = 0;
                uint32_t cc_index = 0;
                fx_source_of_dot(mapping, out_point.slot_idx, &source, &cc_index);
                if (source == 0) {
                    return false;
                }
                Synth::ModInput& added = binding.inputs[binding.num_inputs++];
                added.source           = static_cast<Synth::ModSource>(source);
                added.cc_number        = source == static_cast<uint32_t>(Synth::ModSource::cc)
                                             ? static_cast<uint8_t>(Synth::cc_msb_base + cc_index)
                                             : 0; // a reused slot must not carry stale CC state
                added.op               = Synth::SourceOp::add;
                added.scale            = 1.0f;
                return true;
            }

            // Audio wires: the flag-routing gestures.  The raw
            // public seam carries no routing state, so it refuses them.
            {
                const bool out_input_node = out_point.node_idx == mapping.input_node && out_point.slot_idx == 0;
                const bool out_effect_out = out_slot >= 0 && out_point.slot_idx == 1;
                const bool in_output_node = in_point.node_idx == mapping.output_node && in_point.slot_idx == 0;
                const bool in_effect_in   = in_slot >= 0 && in_point.slot_idx == 0;
                if (! (out_input_node || out_effect_out) || ! (in_output_node || in_effect_in) || ! routing) {
                    return false;
                }

                if (change.kind == Sculptor::ChangeKind::connection_deleted) {
                    // Clear the flag on the wire's earlier endpoint.
                    if (out_input_node) {
                        routing->in = Sculptor::effect_in_disconnected;
                    }
                    else {
                        routing->out[out_slot] = Sculptor::effect_out_disconnected;
                    }
                    Sculptor::fx_sanitize_routing(&chain, routing);
                    return true;
                }

                // A retarget clears the removed wire's earlier endpoint
                // first, so the old wire never survives as a ghost (rewire
                // overwrite).  Exactly one end moved; the unchanged end is
                // the old wire's earlier endpoint when the input end moved,
                // and derivable from the routing when the output end moved.
                if (change.kind == Sculptor::ChangeKind::connection_changed) {
                    const Sculptor::EndPoint prev = change.connection_prev_input;
                    const bool               input_end_moved =
                        prev.node_idx != in_point.node_idx || prev.slot_idx != in_point.slot_idx;
                    if (! input_end_moved) {
                        // The output end moved: whoever fed this input held
                        // the old wire.
                        int32_t old_source = -3; // -3 = none, -2 = input node
                        if (in_output_node) {
                            old_source = Sculptor::fx_output_holder(chain, *routing);
                        }
                        else if (Sculptor::fx_slot_input_fed(chain, *routing, static_cast<uint32_t>(in_slot))) {
                            old_source = in_slot == 0 ? -2 : in_slot - 1;
                        }
                        const int32_t new_source = out_input_node ? -2 : out_slot;
                        if (old_source == new_source) {
                            return true; // the same wire re-landed: nothing moved
                        }
                        if (old_source == -2) {
                            routing->in = Sculptor::effect_in_disconnected;
                        }
                        else if (old_source >= 0) {
                            routing->out[old_source] = Sculptor::effect_out_disconnected;
                        }
                    }
                    else if (out_input_node) {
                        routing->in = Sculptor::effect_in_disconnected;
                    }
                    else {
                        routing->out[out_slot] = Sculptor::effect_out_disconnected;
                    }
                }

                if (in_output_node) {
                    // Only an effect can feed the output; a new
                    // holder demotes the old one.
                    if (out_input_node) {
                        // The channel input node feeds the output node directly:
                        // the bypass wire. The stored effect wiring stays underneath.
                        const int32_t holder = Sculptor::fx_output_holder(chain, *routing);
                        if (holder >= 0) {
                            routing->out[holder] = Sculptor::effect_out_disconnected;
                        }
                        routing->in = Sculptor::effect_in_output;
                        fx_sanitize_routing(&chain, routing);
                        return true;
                    }
                    if (out_slot < 0) {
                        return false;
                    }
                    const int32_t holder = Sculptor::fx_output_holder(chain, *routing);
                    if (holder >= 0 && holder != out_slot) {
                        routing->out[holder] = Sculptor::effect_out_disconnected;
                    }
                    // One output feed: the bypass yields to the effect wire.
                    if (routing->in == Sculptor::effect_in_output) {
                        routing->in = Sculptor::effect_in_disconnected;
                    }
                    routing->out[out_slot] = Sculptor::effect_out_output;
                    Sculptor::fx_sanitize_routing(&chain, routing);
                    return true;
                }

                // A connect lands on an unfed segment start; fed
                // targets refuse like the canvas does.
                if (in_slot < 0 || in_slot == out_slot ||
                    Sculptor::fx_slot_input_fed(chain, *routing, static_cast<uint32_t>(in_slot))) {
                    return false;
                }
                if (out_input_node) {
                    // The target's run moves to the front.
                    const uint32_t run = fx_run_length(chain, *routing, static_cast<uint32_t>(in_slot));
                    if (! fx_splice_effect_run(&chain,
                                               routing,
                                               static_cast<uint32_t>(in_slot),
                                               run,
                                               -1,
                                               reordered_mapping)) {
                        return false;
                    }
                    routing->in = Sculptor::effect_in_first_slot;
                }
                else {
                    // The target's run moves directly after the
                    // source, and a source inside the target's run refuses
                    // (the cycle rotation case).
                    const uint32_t run = fx_run_length(chain, *routing, static_cast<uint32_t>(in_slot));
                    if (out_slot > in_slot && out_slot < in_slot + static_cast<int32_t>(run)) {
                        return false;
                    }
                    uint32_t out_slot_new = static_cast<uint32_t>(out_slot);
                    if (! fx_splice_effect_run(&chain,
                                               routing,
                                               static_cast<uint32_t>(in_slot),
                                               run,
                                               out_slot,
                                               reordered_mapping,
                                               &out_slot_new)) {
                        return false;
                    }
                    routing->out[out_slot_new] = Sculptor::effect_out_next; // the splice may have moved the source
                }
                Sculptor::fx_sanitize_routing(&chain, routing);
                return true;
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
    return apply_fx_graph_change_internal(bank, graph, mapping, change, nullptr, nullptr);
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

void Sculptor::fx_exchange_effect_slots(Synth::EffectChainBinding* chain, uint32_t slot_a, uint32_t slot_b)
{
    const Synth::EffectSlotBinding temporary = chain->effects[slot_a];
    chain->effects[slot_a]                   = chain->effects[slot_b];
    chain->effects[slot_b]                   = temporary;
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
            snprintf(name, capacity, "MIDI Inputs");
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

// Generated effect titles, the legacy "MIDI" key, and parseable LFO
// titles with a zero descriptor can be obsolete presentation.  Nonzero
// descriptor titles remain references and must resolve, even when no
// parameter uses the node.
static bool obsolete_effect_title(const char (&name)[32])
{
    // "MIDI" keyed the MIDI node's layout record before the node was
    // retitled "MIDI Inputs"; old saved records drop instead of refusing
    // the load, and the moved place resets to the projection default.
    if (strcmp(name, "MIDI") == 0) {
        return true;
    }

    // A parseable LFO title with a zero descriptor ("LFO 0" residue)
    // references no live LFO: dropped like other obsolete titles instead
    // of refusing the load.
    uint16_t descriptor = 0;
    if (Sculptor::parse_effect_lfo_title(name, &descriptor) && descriptor == 0) {
        return true;
    }

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
                                                         const char (&name)[32],
                                                         uint8_t*  kind,
                                                         uint16_t* identity)
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

// True when the slot holds a live effect payload.
static bool fx_slot_occupied(const Synth::EffectChainBinding& chain, uint32_t slot)
{
    return slot < chain.num_effects && slot < Synth::max_chain_effects &&
           chain.effects[slot].type != Synth::EffectType::none;
}

bool Sculptor::fx_slot_input_fed(const Synth::EffectChainBinding& chain,
                                 const EffectChainRouting&        routing,
                                 uint32_t                         slot)
{
    if (! fx_slot_occupied(chain, slot)) {
        return false;
    }
    if (slot == 0) {
        return routing.in == effect_in_first_slot;
    }
    return routing.out[slot - 1] == effect_out_next;
}

int32_t Sculptor::fx_output_holder(const Synth::EffectChainBinding& chain, const EffectChainRouting& routing)
{
    for (uint32_t slot = 0; slot < chain.num_effects && slot < Synth::max_chain_effects; ++slot) {
        if (chain.effects[slot].type != Synth::EffectType::none && routing.out[slot] == effect_out_output) {
            return static_cast<int32_t>(slot);
        }
    }
    return -1;
}

bool Sculptor::fx_connector_red(const Synth::EffectChainBinding& chain,
                                const EffectChainRouting&        routing,
                                FxConnector                      kind,
                                int32_t                          slot)
{
    switch (kind) {
        case fx_connector_global_input:
            return routing.in == effect_in_disconnected;
        case fx_connector_global_output:
            return fx_output_holder(chain, routing) < 0 && routing.in != effect_in_output;
        case fx_connector_effect_input:
            return ! fx_slot_input_fed(chain, routing, slot < 0 ? 0 : static_cast<uint32_t>(slot));
        case fx_connector_effect_output: {
            if (slot < 0 || ! fx_slot_occupied(chain, static_cast<uint32_t>(slot))) {
                return true;
            }
            const uint8_t out = routing.out[slot];
            if (out == effect_out_output) {
                return false;
            }
            // A next flag stays unred while its successor slot exists and is
            // occupied; the sanitizer makes the dangling case unreachable.
            return out == effect_out_disconnected || ! fx_slot_occupied(chain, static_cast<uint32_t>(slot) + 1);
        }
    }
    return true;
}

void Sculptor::fx_sanitize_routing(Synth::EffectChainBinding* chain, EffectChainRouting* routing)
{
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        if (routing->out[slot] == effect_out_next && ! fx_slot_occupied(*chain, slot + 1)) {
            routing->out[slot] = effect_out_disconnected;
        }
    }
}

void Sculptor::fx_compact_chain(Synth::EffectChainBinding*    chain,
                                Sculptor::EffectChainRouting* routing,
                                uint32_t                      order[Synth::max_chain_effects])
{
    // order, when given, receives the old -> new slot permutation; removed
    // dormant slots are marked by max_chain_effects.
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        if (order) {
            order[slot] = Synth::max_chain_effects;
        }
    }
    const Synth::EffectChainBinding snapshot = *chain;
    uint8_t                         out_snapshot[Synth::max_chain_effects];
    memcpy(out_snapshot, routing->out, sizeof(out_snapshot));
    uint32_t kept = 0;
    for (uint32_t slot = 0; slot < chain->num_effects && slot < Synth::max_chain_effects; ++slot) {
        if (snapshot.effects[slot].type == Synth::EffectType::none) {
            continue;
        }
        chain->effects[kept] = snapshot.effects[slot];
        routing->out[kept]   = out_snapshot[slot];
        if (order) {
            order[slot] = kept;
        }
        ++kept;
    }
    for (uint32_t slot = kept; slot < Synth::max_chain_effects; ++slot) {
        chain->effects[slot] = {};
        routing->out[slot]   = Sculptor::effect_out_disconnected;
    }
    chain->num_effects = static_cast<uint8_t>(kept);
    if (! kept) {
        routing->in = effect_in_disconnected;
    }
    fx_sanitize_routing(chain, routing);
}

// Length of the connected run a slot heads: the slot plus every following
// slot its effect_out_next run reaches.
uint32_t Sculptor::fx_run_length(const Synth::EffectChainBinding& chain,
                                 const EffectChainRouting&        routing,
                                 uint32_t                         slot)
{
    uint32_t length = 1;
    while (slot + length < Synth::max_chain_effects && routing.out[slot + length - 1] == Sculptor::effect_out_next &&
           fx_slot_occupied(chain, slot + length)) {
        ++length;
    }
    return length;
}

// Moves the raw contiguous block [moved, moved + run_length) directly after
// after_slot (-1 = the front), permuting payloads and flags together.  The
// block is a contiguous occupied run, so no gap can appear.
static bool fx_splice_effect_run(Synth::EffectChainBinding*    chain,
                                 Sculptor::EffectChainRouting* routing,
                                 uint32_t                      moved,
                                 uint32_t                      run_length,
                                 int32_t                       after_slot,
                                 Sculptor::EffectGraphMapping* mapping,
                                 uint32_t*                     after_slot_new)
{
    const uint32_t count     = chain->num_effects;
    const uint32_t block_end = moved + run_length;
    if (moved >= count || block_end > count || after_slot >= static_cast<int32_t>(count) ||
        (after_slot >= 0 && static_cast<uint32_t>(after_slot) >= moved &&
         static_cast<uint32_t>(after_slot) < block_end)) {
        return false;
    }
    uint32_t order[Synth::max_chain_effects];
    uint32_t position = 0;
    bool     inserted = false;
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (slot >= moved && slot < block_end) {
            continue;
        }
        if (! inserted && (after_slot < 0 || static_cast<int32_t>(slot) > after_slot)) {
            for (uint32_t i = 0; i < run_length; ++i) {
                order[position++] = moved + i;
            }
            inserted = true;
        }
        order[position++] = slot;
    }
    if (! inserted) {
        for (uint32_t i = 0; i < run_length; ++i) {
            order[position++] = moved + i;
        }
    }

    const Synth::EffectChainBinding snapshot = *chain;
    uint8_t                         out_snapshot[Synth::max_chain_effects];
    memcpy(out_snapshot, routing->out, sizeof(out_snapshot));
    const Sculptor::EffectGraphMapping previous = mapping ? *mapping : Sculptor::EffectGraphMapping{};
    for (uint32_t slot = 0; slot < count; ++slot) {
        chain->effects[slot] = snapshot.effects[order[slot]];
        routing->out[slot]   = out_snapshot[order[slot]];
        if (after_slot >= 0 && order[slot] == static_cast<uint32_t>(after_slot) && after_slot_new) {
            *after_slot_new = slot; // the anchor's own new index, for flag writes
        }
        if (mapping) {
            mapping->effect_nodes[slot] = previous.effect_nodes[order[slot]];
        }
    }
    return true;
}

// Stages the source into the shared candidate; refusals leave the caller's
// out untouched.  Source and out may alias.
static bool fx_begin_candidate(const Synth::InstrumentEditorBank* source, uint32_t chain)
{
    if (! source || chain > Sculptor::fx_master_chain) {
        return false;
    }
    effects_candidate = *source;
    return true;
}

bool Sculptor::fx_connect_input_candidate(const Synth::InstrumentEditorBank* source,
                                          uint32_t                           chain,
                                          uint32_t                           target_slot,
                                          Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    if (! fx_slot_occupied(bind, target_slot) || fx_slot_input_fed(bind, routing, target_slot)) {
        return false;
    }
    const uint32_t run = fx_run_length(bind, routing, target_slot);
    if (! fx_splice_effect_run(&bind, &routing, target_slot, run, -1)) {
        return false;
    }
    routing.in = effect_in_first_slot;
    fx_sanitize_routing(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

bool Sculptor::fx_connect_slots_candidate(const Synth::InstrumentEditorBank* source,
                                          uint32_t                           chain,
                                          uint32_t                           source_slot,
                                          uint32_t                           target_slot,
                                          Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    if (! fx_slot_occupied(bind, source_slot) || ! fx_slot_occupied(bind, target_slot) || source_slot == target_slot ||
        fx_slot_input_fed(bind, routing, target_slot)) {
        return false;
    }
    // Cycle refusal: a source inside the target's run would rotate the run.
    const uint32_t run = fx_run_length(bind, routing, target_slot);
    if (source_slot > target_slot && source_slot < target_slot + run) {
        return false;
    }
    uint32_t source_slot_new = source_slot;
    if (! fx_splice_effect_run(&bind,
                               &routing,
                               target_slot,
                               run,
                               static_cast<int32_t>(source_slot),
                               nullptr,
                               &source_slot_new)) {
        return false;
    }
    routing.out[source_slot_new] = effect_out_next; // the splice may have moved the source
    fx_sanitize_routing(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

bool Sculptor::fx_connect_output_candidate(const Synth::InstrumentEditorBank* source,
                                           uint32_t                           chain,
                                           uint32_t                           source_slot,
                                           Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    if (! fx_slot_occupied(bind, source_slot)) {
        return false;
    }
    const int32_t holder = fx_output_holder(bind, routing);
    if (holder >= 0 && holder != static_cast<int32_t>(source_slot)) {
        routing.out[holder] = effect_out_disconnected;
    }
    // One output feed: the bypass yields to the effect wire.
    if (routing.in == effect_in_output) {
        routing.in = effect_in_disconnected;
    }
    routing.out[source_slot] = effect_out_output;
    fx_sanitize_routing(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

bool Sculptor::fx_disconnect_candidate(const Synth::InstrumentEditorBank* source,
                                       uint32_t                           chain,
                                       FxConnector                        kind,
                                       int32_t                            slot,
                                       Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    switch (kind) {
        case fx_connector_global_input:
            routing.in = effect_in_disconnected;
            break;
        case fx_connector_global_output: {
            const int32_t holder = fx_output_holder(bind, routing);
            if (holder >= 0) {
                routing.out[holder] = effect_out_disconnected;
            }
            break;
        }
        case fx_connector_effect_input:
            if (slot < 0 || ! fx_slot_occupied(bind, static_cast<uint32_t>(slot))) {
                return false;
            }
            if (slot == 0) {
                if (routing.in == effect_in_first_slot) {
                    routing.in = effect_in_disconnected;
                }
            }
            else {
                routing.out[slot - 1] = effect_out_disconnected;
            }
            break;
        case fx_connector_effect_output:
            if (slot < 0 || ! fx_slot_occupied(bind, static_cast<uint32_t>(slot))) {
                return false;
            }
            routing.out[slot] = effect_out_disconnected;
            break;
    }
    fx_sanitize_routing(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

// Removes one slot and repairs the routing: flags shift down with the
// payloads, a doubly connected slot hands its downstream connection to
// its predecessor (the global input keeps feeding slot 0), and an
// emptied chain loses its input wire.  Deleting slot 0 keeps the input
// feed only when the deleted slot fed its successor: a disconnected or
// output-flagged slot 0 must not wire the global input onto the next
// remaining slot, which nothing fed before.  The trailing sanitize also
// repairs the fed-slot-disconnected-output case, whose flag shift would
// otherwise leave a dangling effect_out_next (the canvas batch seam and
// the candidate path both run this one transform).
static void fx_delete_effect_routing(Synth::EffectChainBinding*    chain,
                                     Sculptor::EffectChainRouting* routing,
                                     uint32_t                      slot)
{
    const bool    fed        = Sculptor::fx_slot_input_fed(*chain, *routing, slot);
    const uint8_t downstream = routing->out[slot];
    Sculptor::fx_remove_effect(chain, slot);
    for (uint32_t i = slot; i + 1 < Synth::max_chain_effects; ++i) {
        routing->out[i] = routing->out[i + 1];
    }
    routing->out[Synth::max_chain_effects - 1] = Sculptor::effect_out_disconnected;
    if (fed && downstream != Sculptor::effect_out_disconnected && slot > 0) {
        routing->out[slot - 1] = downstream;
    }
    if (chain->num_effects == 0) {
        routing->in = Sculptor::effect_in_disconnected;
    }
    else if (slot == 0 && downstream != Sculptor::effect_out_next) {
        routing->in = Sculptor::effect_in_disconnected;
    }
    Sculptor::fx_sanitize_routing(chain, routing);
}

bool Sculptor::fx_delete_effect_candidate(const Synth::InstrumentEditorBank* source,
                                          uint32_t                           chain,
                                          uint32_t                           slot,
                                          Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    if (! fx_slot_occupied(bind, slot)) {
        return false;
    }
    fx_delete_effect_routing(&bind, &routing, slot);
    fx_compact_chain(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

bool Sculptor::fx_add_effect_candidate(const Synth::InstrumentEditorBank* source,
                                       uint32_t                           chain,
                                       Synth::EffectType                  type,
                                       Synth::InstrumentEditorBank*       out)
{
    if (! out || ! fx_begin_candidate(source, chain)) {
        return false;
    }
    Synth::EffectChainBinding& bind    = fx_graph_chain(&effects_candidate.bank, chain);
    EffectChainRouting&        routing = effects_candidate.effect_routing[chain];
    if (bind.num_effects >= Synth::max_chain_effects || type == Synth::EffectType::none ||
        static_cast<uint32_t>(type) >= Synth::num_effect_types) {
        return false;
    }
    // Insert immediately after the output holder; with no holder append
    // after the last occupied slot.
    const int32_t  holder = fx_output_holder(bind, routing);
    const uint32_t insert = holder >= 0 ? static_cast<uint32_t>(holder) + 1 : bind.num_effects;
    for (uint32_t slot = bind.num_effects; slot > insert; --slot) {
        bind.effects[slot] = bind.effects[slot - 1];
        routing.out[slot]  = routing.out[slot - 1];
    }
    fx_init_slot(&bind, insert, type);
    bind.effects[insert].enabled = true;
    ++bind.num_effects;
    if (holder >= 0) {
        routing.out[holder] = effect_out_next;
    }
    // The bypass owns the output feed: a new effect stays unwired while
    // the input node feeds the output node directly.
    if (routing.in != effect_in_output) {
        routing.out[insert] = effect_out_output;
        if (bind.num_effects == 1) {
            routing.in = effect_in_first_slot;
        }
    }
    fx_sanitize_routing(&bind, &routing);
    if (! validate_effect_routing(bind, routing)) {
        return false;
    }
    *out = effects_candidate;
    return true;
}

void Sculptor::fx_swap_effect_layout_records(Synth::InstrumentEditorBank* bank,
                                             uint32_t                     chain,
                                             uint32_t                     slot_a,
                                             uint32_t                     slot_b)
{
    if (! bank || chain > Synth::max_channels || slot_a >= Synth::max_chain_effects ||
        slot_b >= Synth::max_chain_effects || slot_a == slot_b) {
        return;
    }
    // Only the named chain's records: another chain may hold occupied slots
    // at the same indices, and its saved layout must never move.
    Synth::EffectChainBinding& bind = fx_graph_chain(&bank->bank, chain);
    if (! fx_slot_occupied(bind, slot_a) || ! fx_slot_occupied(bind, slot_b)) {
        return;
    }
    // Post-exchange titles: slot_a carries slot_b's old payload and vice
    // versa, so exchanging the two records' names exchanges their bodies
    // between the titles and the two boxes physically trade places.  A
    // missing record means the default position: the present record
    // re-keys to the other title.
    char title_a[32];
    char title_b[32];
    effect_layout_name(bind, chain, Synth::effects_graph_layout_effect, slot_a, title_a, sizeof(title_a));
    effect_layout_name(bind, chain, Synth::effects_graph_layout_effect, slot_b, title_b, sizeof(title_b));
    int32_t record_a = -1;
    int32_t record_b = -1;
    for (uint32_t index = 0; index < bank->graph_layout_count; ++index) {
        Synth::GraphNodeLayout& record = bank->graph_layout[index];
        if (record.kind != 4 || record.channel != chain) {
            continue;
        }
        if (strcmp(record.name, title_a) == 0) {
            record_a = static_cast<int32_t>(index);
        }
        if (strcmp(record.name, title_b) == 0) {
            record_b = static_cast<int32_t>(index);
        }
    }
    if (record_a < 0 && record_b < 0) {
        return;
    }
    if (record_a >= 0 && record_b >= 0) {
        char name[32];
        memcpy(name, bank->graph_layout[record_a].name, sizeof(name));
        memcpy(bank->graph_layout[record_a].name, bank->graph_layout[record_b].name, sizeof(name));
        memcpy(bank->graph_layout[record_b].name, name, sizeof(name));
    }
    else if (record_a >= 0) {
        snprintf(bank->graph_layout[record_a].name, sizeof(bank->graph_layout[record_a].name), "%s", title_b);
    }
    else {
        snprintf(bank->graph_layout[record_b].name, sizeof(bank->graph_layout[record_b].name), "%s", title_a);
    }
}

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

// Sets one projected LFO node's Duty and Period editability: duty shapes
// sawtooth only, and the period freezes while a beat division resolves it.
// Plain value edits never reproject the node.
void Sculptor::refresh_lfo_edit_disabled(Sculptor::Graph& graph, uint32_t node)
{
    if (node >= Sculptor::max_nodes || ! graph.node_occupied(node)) {
        return;
    }

    const Sculptor::Node& node_ref   = graph.node(node);
    uint32_t              wave_idx   = Sculptor::pool_no_slot;
    uint32_t              duty_idx   = Sculptor::pool_no_slot;
    uint32_t              sync_idx   = Sculptor::pool_no_slot;
    uint32_t              period_idx = Sculptor::pool_no_slot;
    for (uint32_t slot = 0; slot < node_ref.slots.num_allocated; ++slot) {
        const Sculptor::Slot& slot_ref = node_ref.slots.entries[slot];
        if (strncmp(slot_ref.name, "Waveform", sizeof(slot_ref.name)) == 0) {
            wave_idx = slot;
        }

        if (strncmp(slot_ref.name, "Duty", sizeof(slot_ref.name)) == 0) {
            duty_idx = slot;
        }

        if (strncmp(slot_ref.name, "Sync", sizeof(slot_ref.name)) == 0) {
            sync_idx = slot;
        }

        if (strncmp(slot_ref.name, "Period (ms)", sizeof(slot_ref.name)) == 0) {
            period_idx = slot;
        }
    }

    // Each lock is evaluated independently: a missing slot leaves only that
    // one lock unchanged.
    if (wave_idx != Sculptor::pool_no_slot && duty_idx != Sculptor::pool_no_slot) {
        const bool sine = node_ref.slots.entries[wave_idx].value.list_index == 0;
        graph.set_slot_edit_disabled(node, duty_idx, sine);
    }

    if (sync_idx != Sculptor::pool_no_slot && period_idx != Sculptor::pool_no_slot) {
        const bool synced = node_ref.slots.entries[sync_idx].value.list_index != 0;
        graph.set_slot_edit_disabled(node, period_idx, synced);
    }
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
    // Wire gestures edit the candidate's routing flags directly; the
    // candidate owns the pointer's lifetime for the whole batch.
    Sculptor::EffectChainRouting* routing = &effects_candidate.effect_routing[mapping->chain];

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
                if (! apply_fx_graph_change_internal(&effects_candidate.bank,
                                                     *graph,
                                                     current,
                                                     change,
                                                     nullptr,
                                                     routing)) {
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
        if (change.kind == ChangeKind::connection_added || change.kind == ChangeKind::connection_deleted ||
            change.kind == ChangeKind::connection_changed) {
            // A wire whose endpoint node is gone is a deletion byproduct;
            // the delete branch owns the routing repair.
            if (! graph->node_occupied(change.connection_input.node_idx) ||
                ! graph->node_occupied(change.connection_output.node_idx)) {
                continue;
            }
        }
        if (! apply_fx_graph_change_internal(&effects_candidate.bank, *graph, current, change, &current, routing)) {
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
            if (! apply_fx_graph_change_internal(&effects_candidate.bank, *graph, current, change, nullptr, routing)) {
                return false;
            }
            for (uint32_t moved = slot; moved < Synth::max_chain_effects; ++moved) {
                current.effect_nodes[moved - 1] = current.effect_nodes[moved];
            }
            current.effect_nodes[Synth::max_chain_effects - 1] = pool_no_slot;
        }
    }
    if (! Sculptor::validate_effect_routing(chain_at(effects_candidate.bank, mapping->chain), *routing)) {
        return false;
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
                                        vmath::vec2                        position,
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
    // Measured add-time placement: the caller passes the place the new box
    // lands at (the release point for the drop, the measured lane x for
    // the canvas add); the record freezes it so the unwired descriptor
    // stays represented and persistent, chain-scoped.
    record.x       = position.x;
    record.y       = position.y;
    record.kind    = 4;
    record.channel = static_cast<uint8_t>(owner);
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
    document.routing = effects_candidate.effect_routing[chain];
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
    if (! source || ! out_candidate || chain > fx_master_chain) {
        return false;
    }
    if (! Synth::validate_effects_document(document)) {
        return false;
    }
    if (! Synth::validate_instrument_bank(&source->bank) || ! validate_editor_metadata(*source) ||
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
    // A pasted chain's wired CC inputs adopt their dots into the destination
    // registry.  The union only adds dots, never removes inputs, so the
    // destination's pre-existing additions survive.
    effects_candidate.added_cc_mask[chain] =
        static_cast<uint16_t>(effects_candidate.added_cc_mask[chain] | fx_chain_wired_cc_mask(destination));

    effects_candidate.effect_routing[chain] = document->routing;
    uint32_t retained                       = 0;
    for (uint32_t index = 0; index < effects_candidate.graph_layout_count; ++index) {
        const Synth::GraphNodeLayout& record = effects_candidate.graph_layout[index];
        if (record.kind != 4 || record.channel != chain) {
            effects_candidate.graph_layout[retained++] = record;
        }
    }
    effects_candidate.graph_layout_count = retained;
    // A local bank lets the same title/identity transfer serve library and clipboard payloads.
    memset(static_cast<void*>(&layout_source), 0, sizeof(layout_source));
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
    if (! transfer_effect_layout(&layout_source, 0, lfo_ids, chain, &effects_candidate)) {
        return false;
    }
    if (! Synth::validate_instrument_bank(&effects_candidate.bank)) {
        return false;
    }
    if (! validate_editor_metadata(effects_candidate)) {
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
