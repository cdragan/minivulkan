// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

static int exit_code = 0;
// The clipboard layout tests measure a document's real parser token count with the
// vendored parser the codec itself uses, so the measurement cannot drift from the
// parser implementation. The parser builds warning-clean under -Wall only; its
// integer-conversion noise is silenced the same way the codec's include does.
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4245)
#endif
#define JSMN_STATIC
#include "../thirdparty/jsmn/jsmn.h"
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

// The legacy raw-seam blocks start from a plain bank: give the projection
// the serial routing the v1 migration would install, so its derived wires
// match the old implicit-serial expectations.
void implicit_serial_routing(Synth::InstrumentEditorBank& bank, uint32_t owner)
{
    Synth::EffectChainBinding& chain =
        owner < Synth::max_channels ? bank.bank.channel_chains[owner] : bank.bank.master_chain;
    Sculptor::EffectChainRouting& routing = bank.effect_routing[owner];
    routing                               = {};
    int32_t last                          = -1;
    for (uint32_t slot = 0; slot < chain.num_effects && slot < Synth::max_chain_effects; ++slot) {
        if (chain.effects[slot].type != Synth::EffectType::none) {
            last = static_cast<int32_t>(slot);
        }
    }
    for (int32_t slot = 0; slot <= last; ++slot) {
        routing.out[slot] = Sculptor::effect_out_next;
    }
    if (last >= 0) {
        routing.out[last] = Sculptor::effect_out_output;
        routing.in        = Sculptor::effect_in_first_slot;
    }
}

bool project_legacy_effects(const Synth::InstrumentBank&  bank,
                            uint32_t                      owner,
                            Sculptor::Graph*              graph,
                            Sculptor::EffectGraphMapping* mapping,
                            const bool*                   pins)
{
    const std::unique_ptr<Synth::InstrumentEditorBank> authored_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& authored = *authored_storage;
    memset(static_cast<void*>(&authored), 0, sizeof(authored));
    authored.bank = bank;
    implicit_serial_routing(authored, owner);
    return Sculptor::project_effect_chain_to_graph(authored, owner, graph, mapping, pins);
}

void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n", file, line, test);
}

bool approx(float left, float right, float eps)
{
    float diff = left - right;
    if (diff < 0.0f) {
        diff = -diff;
    }
    return diff <= eps;
}

// True for the four targets these fixtures wire modulators and MIDI
// inputs onto (volume, pitch, panning, FIR spread). All twelve targets
// are uniformly modulate-able; the remaining eight (FM ratio, pad tone
// A/B, duty A/B, osc mix, FM depth and FIR slope) keep base-only
// bindings here purely to bound fixture size.
bool osc_graph_target(Synth::ModTarget target)
{
    return target == Synth::mod_volume || target == Synth::mod_pitch || target == Synth::mod_panning ||
           target == Synth::mod_fir_spread;
}

static bool osc_graph_mark_node(bool* seen, uint32_t node_idx, uint32_t* count)
{
    if (node_idx >= Sculptor::max_nodes || seen[node_idx]) {
        return false;
    }
    seen[node_idx] = true;
    (*count)++;
    return true;
}

// The connection terminating at an input endpoint, or pool_no_slot.
uint32_t find_osc_graph_connection(const Sculptor::Graph& graph, const Sculptor::EndPoint& input)
{
    for (uint32_t i = 0; i < Sculptor::max_connections; ++i) {
        if (! graph.connection_occupied(i)) {
            continue;
        }
        const Sculptor::Connection& c = graph.get_connection(i);
        if (c.input.node_idx == input.node_idx && c.input.slot_idx == input.slot_idx) {
            return i;
        }
    }
    return Sculptor::pool_no_slot;
}

// The band index (0..3) of a band-parameter target, -1 otherwise: the
// test-side mirror of the production band_target mapping (band 0 keeps
// the legacy spread/slope targets 7 and 11).
int32_t osc_target_band_of_target(const uint32_t target)
{
    // Targets 26/27 are string rows, never bands.
    if (target >= 14u && target <= 25u) {
        return static_cast<int32_t>((target - 14u) / 4u) + 1;
    }
    if (target == 7u || target == 11u || target == 12u || target == 13u) {
        return 0;
    }
    return -1;
}

// Checks via TEST that a projection filled the mapping consistently: every
// fixed node exists and is distinct, each occupied layer's oscillator has
// exactly 15 rows plus its target blocks, and the instance registry
// carries exactly env_instances + lfo_instances instances. The distinct
// node total is 3 + layer_count + env_instances + lfo_instances (the
// inputs node + the instrument parameters node + sum + oscillators +
// generator instances).
void check_osc_graph_mapping(const Sculptor::Graph&           graph,
                             const Sculptor::OscGraphMapping& mapping,
                             uint32_t                         layer_count,
                             uint32_t                         env_instances,
                             uint32_t                         lfo_instances,
                             uint32_t                         band_count)
{
    bool     seen[Sculptor::max_nodes] = {};
    uint32_t count                     = 0;
    TEST(mapping.input_node != Sculptor::pool_no_slot);
    TEST(osc_graph_mark_node(seen, mapping.input_node, &count));
    TEST(mapping.instrument_node != Sculptor::pool_no_slot);
    TEST(osc_graph_mark_node(seen, mapping.instrument_node, &count));
    for (uint32_t idx = 0; idx < Sculptor::num_osc_graph_inputs; idx++) {
        TEST(mapping.input_source_slots[idx] != Sculptor::pool_no_slot);
    }
    TEST(mapping.output_node != Sculptor::pool_no_slot);
    TEST(osc_graph_mark_node(seen, mapping.output_node, &count));
    TEST(mapping.osc_output_slot != Sculptor::pool_no_slot);
    TEST(mapping.env_output_slot != Sculptor::pool_no_slot);
    TEST(mapping.lfo_output_slot != Sculptor::pool_no_slot);
    TEST(mapping.lfo_depth_input_slot != Sculptor::pool_no_slot);
    TEST(mapping.lfo_rate_input_slot != Sculptor::pool_no_slot);
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        if (layer < layer_count) {
            TEST(mapping.osc_nodes[layer] != Sculptor::pool_no_slot);
            TEST(osc_graph_mark_node(seen, mapping.osc_nodes[layer], &count));
            // 35 fixed rows plus Filter Type, then one uniform band block
            // per existing band: the "Band N" header label, its [-]/[+]
            // click slots, and four parameter rows each followed by a
            // 12-slot block.
            const uint32_t osc_node = mapping.osc_nodes[layer];
            TEST(graph.node(osc_node).slots.num_allocated ==
                 Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride * band_count);
            for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                // The family dots stay connectable whether or not they carry
                // a wire; the base row never hides.  Bands beyond the count
                // project no rows at all.
                if (osc_target_band_of_target(t) >= 0 &&
                    static_cast<uint32_t>(osc_target_band_of_target(t)) >= band_count) {
                    continue;
                }
                TEST(Sculptor::osc_target_env_dot(t) < graph.node(osc_node).slots.num_allocated);
                TEST(Sculptor::osc_target_lfo_dot(t) < graph.node(osc_node).slots.num_allocated);
                for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                    TEST(Sculptor::osc_target_src_dot(t, i) < graph.node(osc_node).slots.num_allocated);
                }
            }
        }
        else {
            TEST(mapping.osc_nodes[layer] == Sculptor::pool_no_slot);
        }
    }
    TEST(mapping.detached_count == env_instances + lfo_instances);
    for (uint32_t i = 0; i < mapping.detached_count; i++) {
        TEST(mapping.detached[i].kind == 1 || mapping.detached[i].kind == 2);
        TEST(mapping.detached[i].desc_id != 0);
        if (mapping.detached[i].node_idx == Sculptor::pool_no_slot) {
            continue; // unprojected surplus record
        }
        TEST(osc_graph_mark_node(seen, mapping.detached[i].node_idx, &count));
    }
    TEST(count == 3 + layer_count + env_instances + lfo_instances);
}

// Finds a slot by name on a node; slots are scanned in creation order.

bool find_graph_slot(const Sculptor::Graph& graph, uint32_t node_idx, const char* name, uint32_t* slot_idx)
{
    const Sculptor::Node& node = graph.node(node_idx);
    for (uint32_t idx = 0; idx < node.slots.num_allocated; idx++) {
        if (strncmp(node.slots.entries[idx].name, name, sizeof(node.slots.entries[idx].name)) == 0) {
            *slot_idx = idx;
            return true;
        }
    }
    return false;
}

// Connection matching both endpoints: fx reference wires land on rows that
// also carry property state, so endpoint identity alone is the test key.
uint32_t find_fx_graph_connection(const Sculptor::Graph&    graph,
                                  const Sculptor::EndPoint& output,
                                  const Sculptor::EndPoint& input)
{
    for (uint32_t i = 0; i < Sculptor::max_connections; ++i) {
        if (! graph.connection_occupied(i)) {
            continue;
        }
        const Sculptor::Connection& c = graph.get_connection(i);
        if (c.output.node_idx == output.node_idx && c.output.slot_idx == output.slot_idx &&
            c.input.node_idx == input.node_idx && c.input.slot_idx == input.slot_idx) {
            return i;
        }
    }
    return Sculptor::pool_no_slot;
}

// Registry index of the instance projected onto `node_idx`, or -1.
int32_t find_instance_by_node(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    for (uint32_t i = 0; i < mapping.detached_count; i++) {
        if (mapping.detached[i].node_idx == node_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Fixture helper: the fixtures seed per-layer generator variety
// explicitly; this copies only layer 0's base value and MIDI inputs onto
// every live layer, so the MIDI routing is uniform while the generators
// stay per-layer (the per-layer format round-trips uniform instruments
// unchanged; nothing fans anything out at runtime).
void fan_out_routing(Synth::Instrument& instrument)
{
    // Dormant layers stay zero: the memcmp round-trips compare the whole
    // struct, and the compiled image leaves dormant layers untouched.
    for (uint32_t layer = 1; layer < instrument.layer_count; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            instrument.layers[layer].targets[t].base_value = instrument.layers[0].targets[t].base_value;
            instrument.layers[layer].targets[t].num_inputs = instrument.layers[0].targets[t].num_inputs;
            for (uint32_t i = 0; i < Synth::max_mod_inputs; i++) {
                instrument.layers[layer].targets[t].inputs[i] = instrument.layers[0].targets[t].inputs[i];
            }
        }
    }
}

// Zeroed instrument memory carries no dispersion section count; the validator requires an integer 1..6, so
// fixtures that build instruments from raw memory restore the default four.
void set_default_dispersion_sections(Synth::Instrument& instrument)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        instrument.layers[layer].string_dispersion_sections = 4.0f;
    }
}

// Builds a max-complexity projection fixture: 7 layers, an envelope and an LFO
// bound on every layer for each of the five projected targets, full two-input
// routing with mixed ops and scales (volume repeats velocity across its two
// ordered inputs), nonzero skews, and one LFO descriptor aliased across two
// layers.  Writes the referenced 1-based descriptor ids (0), indexed
// [layer][Synth::ModTarget], to env_ids/lfo_ids.
void build_osc_graph_max_fixture(Synth::InstrumentBank* bank,
                                 Synth::Instrument*     instrument,
                                 uint16_t (*env_ids)[Synth::num_mod_targets],
                                 uint16_t (*lfo_ids)[Synth::num_mod_targets])
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument = {};
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            env_ids[layer][t] = 0;
            lfo_ids[layer][t] = 0;
        }
    }

    instrument->layer_count = Synth::max_layers;
    uint32_t env_count      = 0;
    uint32_t lfo_count      = 0;
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        Synth::Oscillator& osc = instrument->layers[layer];
        osc.osc_type[0]        = static_cast<Synth::WaveType>(1 + (layer % 4));
        osc.osc_type[1]        = static_cast<Synth::WaveType>(1 + ((layer + 2) % 4));
        osc.osc_mode           = static_cast<Synth::OscMode>(layer % 3);
        osc.mod_ratio          = 1.0f + 0.25f * static_cast<float>(layer);
        // Layer 2 carries a non-default brightness so the Pluck Brightness row round-trips.
        osc.string_brightness = layer == 2 ? 0.25f : 1.0f;
        // Four bands: every band block projects and round-trips.
        osc.fir_band_count = 4;
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            if (! osc_graph_target(static_cast<Synth::ModTarget>(t))) {
                continue; // duty A/B, osc mix and FM depth keep base-only bindings here
            }
            env_count++;
            const uint32_t env_slot = bank->envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            env_ids[layer][t]              = static_cast<uint16_t>(env_slot + 1);
            Synth::EnvelopeDescriptor& env = bank->envelopes.entries[env_slot];
            env.num_points                 = static_cast<uint8_t>(2 + (env_slot % (Synth::max_envelope_points - 1)));
            env.sustain_first_point        = 0;
            env.sustain_last_point         = static_cast<uint8_t>(env.num_points - 1);
            env.min_value                  = -1.0f + 0.125f * static_cast<float>(env_slot);
            env.min_max_delta              = 1.0f + 0.25f * static_cast<float>(env_slot);
            for (uint32_t p = 0; p < env.num_points; p++) {
                env.points[p].position = static_cast<uint16_t>(100 * p + env_slot);
                env.points[p].value    = static_cast<uint16_t>(0x2000 * (p + 1));
            }

            // One LFO descriptor is deliberately aliased: layer 4's volume LFO
            // reuses layer 1's pitch LFO descriptor (already allocated above).
            if (layer == 4 && t == Synth::mod_volume) {
                lfo_ids[layer][t] = lfo_ids[1][Synth::mod_pitch];
            }
            else {
                lfo_count++;
                const uint32_t lfo_slot = bank->lfos.allocate();
                TEST(lfo_slot != pool_no_slot);
                lfo_ids[layer][t]         = static_cast<uint16_t>(lfo_slot + 1);
                Synth::LFODescriptor& lfo = bank->lfos.entries[lfo_slot];
                lfo.wave                  = static_cast<Synth::WaveType>(1 + (lfo_slot % 4));
                lfo.duty                  = static_cast<uint8_t>(0x20 + (lfo_slot % 0x60));
                lfo.period_ms             = static_cast<uint16_t>(100 + 37 * lfo_slot);
                lfo.min_value             = -0.5f + 0.0625f * static_cast<float>(lfo_slot);
                lfo.min_max_delta         = 0.5f + 0.125f * static_cast<float>(lfo_slot);
            }

            Synth::TargetBinding& gen = osc.targets[t];
            gen.envelope_desc_id      = env_ids[layer][t];
            gen.lfo_desc_id           = lfo_ids[layer][t];
            gen.lfo_op                = ((layer + t) % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
            gen.lfo_depth             = 0.25f * static_cast<float>(layer + 1);
            gen.lfo_depth_source      = static_cast<Synth::ModSource>(1 + ((layer + t) % 6));
            gen.lfo_rate_source       = static_cast<Synth::ModSource>(1 + ((layer + 2 * t + 1) % 6));
            gen.lfo_rate_scale_ms     = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(t);
        }
    }
    TEST(env_count == 28);
    TEST(lfo_count == 27); // 28 bindings minus the one alias

    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        Synth::TargetBinding& routing = instrument->layers[0].targets[t];
        if (t == Synth::mod_fir_slope || t == Synth::mod_fir_band1_slope || t == Synth::mod_fir_band2_slope ||
            t == Synth::mod_fir_band3_slope) {
            // The FIR slope bases are integer percents 1..100; this fixture's
            // formula would produce fractional bases, so those bindings stay
            // zeroed here (the scalar / row carries the value).
            continue;
        }
        if (t == Synth::mod_string_damping || t == Synth::mod_string_release_damping ||
            t == Synth::mod_string_dispersion) {
            continue; // projected only under the string wave, which this fixture never uses
        }
        routing.base_value = -1.0f + 0.25f * static_cast<float>(t);
        if (! osc_graph_target(static_cast<Synth::ModTarget>(t))) {
            continue; // duty A/B, osc mix and FM depth keep no routed inputs here
        }
        routing.num_inputs       = 2;
        const bool same_source   = (t == Synth::mod_volume); // ordered duplicate: velocity twice
        routing.inputs[0].source = same_source ? Synth::ModSource::velocity : Synth::ModSource::pitch_bend;
        routing.inputs[0].op     = Synth::SourceOp::add;
        routing.inputs[0].scale  = 0.5f * static_cast<float>(t);
        routing.inputs[1].source = same_source ? Synth::ModSource::velocity : Synth::ModSource::mod_wheel;
        routing.inputs[1].op     = Synth::SourceOp::multiply;
        routing.inputs[1].scale  = -0.25f * static_cast<float>(t) - 0.5f;
    }
    fan_out_routing(*instrument);
    // The compile syncs each layer's mod_ratio from its fm_ratio base; the
    // fixture mirrors that so project->compile comparisons stay exact.
    for (uint32_t layer = 0; layer < instrument->layer_count; ++layer) {
        instrument->layers[layer].targets[Synth::mod_fm_ratio].base_value = instrument->layers[layer].mod_ratio;
    }
    instrument->note_skew_semitones  = 0.3f;
    instrument->layer_skew_semitones = -0.7f;
}

// Builds a minimal projection fixture with an aliased envelope pair: one layer,
// volume bound to an envelope and an LFO, pitch bound to an envelope sharing the
// volume envelope's descriptor id.  The LFO's depth/rate sources stay unset so
// the LFO node's source connectors are free for validator connection attempts.
// Writes the pitch envelope id, the LFO id and the volume envelope id (all 1-based).
void build_osc_graph_small_fixture(Synth::InstrumentBank* bank,
                                   Synth::Instrument*     instrument,
                                   uint16_t*              shared_env_id,
                                   uint16_t*              lfo_id,
                                   uint16_t*              volume_env_id)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument = {};
    TEST(bank->envelopes.allocate() == 0);
    TEST(bank->lfos.allocate() == 0);
    *shared_env_id = 1;
    *lfo_id        = 1;

    Synth::EnvelopeDescriptor& env = bank->envelopes.entries[0];
    env.num_points                 = 3;
    env.sustain_first_point        = 1;
    env.sustain_last_point         = 1;
    env.min_value                  = -0.5f;
    env.min_max_delta              = 1.5f;
    env.points[0]                  = { 0, 0x4000 };
    env.points[1]                  = { 200, 0x8000 };
    env.points[2]                  = { 500, 0xC000 };

    // The volume target carries its own envelope descriptor: descriptors are
    // shared by every target that wires them, and the volume shape (minimum 0,
    // nonnegative range) cannot serve a non-volume target, so volume and
    // pitch can never share one.  This fixture's volume curve deliberately
    // keeps the pre-constraint content (a legacy bank state the runtime must
    // still play).
    TEST(bank->envelopes.allocate() == 1);
    bank->envelopes.entries[1] = env;
    *volume_env_id             = 2;

    Synth::LFODescriptor& lfo = bank->lfos.entries[0];
    lfo.wave                  = Synth::WaveType::sine_wave;
    lfo.duty                  = 0x7F;
    lfo.period_ms             = 250;
    lfo.min_value             = -1.0f;
    lfo.min_max_delta         = 2.0f;

    instrument->layer_count = 1;
    Synth::Oscillator& osc  = instrument->layers[0];
    osc.osc_type[0]         = Synth::WaveType::sine_wave;
    osc.osc_type[1]         = Synth::WaveType::pulse_wave;
    osc.osc_mode            = Synth::osc_mode_blend;
    osc.mod_ratio           = 2.0f;

    osc.targets[Synth::mod_volume].envelope_desc_id = *volume_env_id;
    osc.targets[Synth::mod_volume].lfo_desc_id      = *lfo_id;
    osc.targets[Synth::mod_volume].lfo_op           = Synth::SourceOp::add;
    osc.targets[Synth::mod_volume].lfo_depth        = 0.5f;
    osc.targets[Synth::mod_pitch].envelope_desc_id  = *shared_env_id;
    osc.targets[Synth::mod_pitch].lfo_op            = Synth::SourceOp::multiply;
    osc.targets[Synth::mod_pitch].lfo_depth         = 1.0f;

    instrument->layers[0].targets[Synth::mod_volume].base_value       = 0.8f;
    instrument->layers[0].targets[Synth::mod_volume].num_inputs       = 1;
    instrument->layers[0].targets[Synth::mod_volume].inputs[0].source = Synth::ModSource::velocity;
    instrument->layers[0].targets[Synth::mod_volume].inputs[0].op     = Synth::SourceOp::add;
    instrument->layers[0].targets[Synth::mod_volume].inputs[0].scale  = 2.0f;
    instrument->layers[0].targets[Synth::mod_pitch].base_value        = -2.0f;
    fan_out_routing(*instrument);
    // The compile syncs each layer's mod_ratio from its fm_ratio base; the
    // fixture mirrors that so project->compile comparisons stay exact.
    for (uint32_t layer = 0; layer < instrument->layer_count; ++layer) {
        instrument->layers[layer].targets[Synth::mod_fm_ratio].base_value = instrument->layers[layer].mod_ratio;
    }
    instrument->note_skew_semitones = 0.1f;
}

static uint32_t fake_alloc_node(const void* ctx)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    return writer.next_node++;
}

static uint16_t fake_resolve_source(const void* ctx, Synth::ModSource source, uint8_t cc_number, uint32_t chain)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    assert(writer.num_resolved < 32); // recordings accumulate across every commit in the suite
    writer.resolved[writer.num_resolved++] = { source, cc_number, chain };
    return 77; // arbitrary concrete node id
}

static void fake_configure_dest(const void* ctx,
                                uint32_t    node,
                                uint16_t    lfo_node,
                                float,
                                Synth::SourceOp,
                                const Synth::SourceParam* sources,
                                uint32_t                  num_inputs)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.dest_count++;
    writer.last_dest_node            = static_cast<uint16_t>(node);
    writer.last_dest_lfo_node        = lfo_node;
    writer.last_dest_source_param_id = num_inputs ? sources[0].param_id : 0;
}

static void fake_configure_lfo(const void* ctx, uint32_t, uint16_t, Synth::SourceOp, float, uint16_t, uint16_t, float)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.lfo_count++;
}

const Synth::EffectNodeWriter fake_writer_binding(FakeWriter& writer)
{
    const Synth::EffectNodeWriter result = { &writer,
                                             fake_alloc_node,
                                             fake_resolve_source,
                                             fake_configure_dest,
                                             fake_configure_lfo };
    return result;
}

Synth::EffectSlotBinding* enabled_effect(Synth::InstrumentBank& bank,
                                         uint32_t               channel,
                                         uint32_t               slot,
                                         Synth::EffectType      type)
{
    Synth::EffectChainBinding& chain =
        (channel < Synth::max_channels) ? bank.channel_chains[channel] : bank.master_chain;
    chain.num_effects           = static_cast<uint8_t>(slot + 1);
    chain.effects[slot].type    = type;
    chain.effects[slot].enabled = true;
    return &chain.effects[slot];
}

// ---- Instrument graph editor test helpers ----

// Appends one detached-node layout record to the bank's editor state.
void add_detached_record(Synth::InstrumentEditorBank* bank,
                         uint32_t                     channel,
                         uint32_t                     zone,
                         uint32_t                     kind,
                         uint32_t                     desc_id,
                         float                        x,
                         float                        y,
                         Synth::ModSource             depth_source,
                         Synth::ModSource             rate_source,
                         uint8_t                      uid)
{
    TEST(bank->graph_layout_count < Synth::max_graph_records);
    Synth::GraphNodeLayout& record = bank->graph_layout[bank->graph_layout_count++];
    memset(static_cast<void*>(&record), 0, sizeof(record));
    record.channel      = static_cast<uint8_t>(channel);
    record.zone         = static_cast<uint8_t>(zone);
    record.kind         = static_cast<uint8_t>(kind);
    record.index        = static_cast<uint8_t>(desc_id);
    record.x            = x;
    record.y            = y;
    record.depth_source = static_cast<uint8_t>(depth_source);
    record.rate_source  = static_cast<uint8_t>(rate_source);
    record.uid          = uid;
}

// Builds an editor bank whose channel 0 zone 0 holds the small projection
// fixture instrument (bound envelope + LFO on volume, envelope on pitch) with
// a valid zone table; no layout records yet.
void build_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    uint16_t shared_env_id = 0;
    uint16_t lfo_id        = 0;
    uint16_t volume_env_id = 0;
    build_osc_graph_small_fixture(&bank->bank, instrument, &shared_env_id, &lfo_id, &volume_env_id);
    bank->bank.channel_enabled[0] = 1;
    TEST(bank->bank.instruments.allocate() == 0);
    bank->bank.instruments.entries[0]         = *instrument;
    bank->bank.channel_zones[0][0].start_note = 1;
    bank->bank.channel_zones[0][0].instrument = 0;
}

// Allocates one detached LFO descriptor (sine, 300 ms) in the bank and returns
// its 1-based id.
uint16_t add_detached_lfo_descriptor(Synth::InstrumentEditorBank* bank)
{
    const uint32_t slot = bank->bank.lfos.allocate();
    TEST(slot != pool_no_slot);
    Synth::LFODescriptor& lfo = bank->bank.lfos.entries[slot];
    lfo.wave                  = Synth::WaveType::sine_wave;
    lfo.period_ms             = 300;
    lfo.min_value             = -1.0f;
    lfo.min_max_delta         = 2.0f;
    return static_cast<uint16_t>(slot + 1);
}

// Number of layout records matching one (channel, zone, kind, index) key.
uint32_t count_records_matching(const Synth::InstrumentEditorBank& bank,
                                uint32_t                           channel,
                                uint32_t                           zone,
                                uint32_t                           kind,
                                uint32_t                           index)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < bank.graph_layout_count; i++) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == kind && record.index == index) {
            count++;
        }
    }
    return count;
}

// Encodes `bank`, splices `"editor":<editor_json>` in as a proper key-value
// pair before the final close brace, and decodes the result into *out.
// `editor_json` is the editor object's inner value, e.g. {"layouts":[...]}.
bool decode_bank_with_editor_section(const Synth::InstrumentEditorBank& bank,
                                     const char*                        editor_json,
                                     Synth::InstrumentEditorBank*       out)
{
    const std::unique_ptr<FixedArray<char, 512 * 1024>> doc_storage = std::make_unique<FixedArray<char, 512 * 1024>>();
    char (&doc)[512 * 1024]                                         = doc_storage->elements;
    const uint32_t base_len = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
    TEST(base_len > 0 && base_len < sizeof(doc));
    uint32_t close = base_len;
    while (close > 0 && doc[close - 1] != '}') {
        close--;
    }
    TEST(close > 0);
    const int written = snprintf(doc + close - 1, sizeof(doc) - (close - 1), ",\"editor\":%s}", editor_json);
    TEST(written > 0 && static_cast<uint32_t>(written) < sizeof(doc) - (close - 1));
    return Synth::decode_editor_bank_json(doc, close - 1 + static_cast<uint32_t>(written), out);
}

// Channel 0 zone 0 binding fixture: three layers; volume is bound on
// layers 0 and 2 with one shared envelope + LFO tuple (depth source
// velocity, rate source mod wheel) and on layer 1 with a second envelope
// descriptor (different tuple); pitch is bound on layer 0 with that
// second envelope; layer 1/2 pitch stay unbound constants. Volume
// routing carries one velocity input. No layout records.
void build_binding_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument                   = {};
    bank->bank.channel_enabled[0] = 1;
    TEST(bank->bank.instruments.allocate() == 0);
    bank->bank.channel_zones[0][0].start_note = 1;
    bank->bank.channel_zones[0][0].instrument = 0;

    for (uint32_t e = 0; e < 2; e++) {
        const uint32_t env_slot = bank->bank.envelopes.allocate();
        TEST(env_slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[env_slot];
        env.num_points                 = 2;
        env.sustain_first_point        = 0;
        env.sustain_last_point         = 1;
        env.min_value                  = -1.0f + 0.5f * static_cast<float>(e);
        env.min_max_delta              = 2.0f;
        env.points[0].position         = 0;
        env.points[0].value            = 0x2000;
        env.points[1].position         = 100;
        env.points[1].value            = 0x4000;
    }
    const uint32_t lfo_slot = bank->bank.lfos.allocate();
    TEST(lfo_slot != pool_no_slot);
    Synth::LFODescriptor& lfo = bank->bank.lfos.entries[lfo_slot];
    lfo.wave                  = Synth::WaveType::sine_wave;
    lfo.period_ms             = 300;
    lfo.min_value             = -1.0f;
    lfo.min_max_delta         = 2.0f;

    instrument->layer_count = 3;
    for (uint32_t layer = 0; layer < 3; layer++) {
        instrument->layers[layer].osc_type[0] = Synth::WaveType::sawtooth_wave;
    }
    Synth::TargetBinding volume_tuple                                = {};
    volume_tuple.envelope_desc_id                                    = 1;
    volume_tuple.lfo_desc_id                                         = 1;
    volume_tuple.lfo_op                                              = Synth::SourceOp::add;
    volume_tuple.lfo_depth                                           = 0.5f;
    volume_tuple.lfo_depth_source                                    = Synth::ModSource::velocity;
    volume_tuple.lfo_rate_source                                     = Synth::ModSource::mod_wheel;
    volume_tuple.lfo_rate_scale_ms                                   = 20.0f;
    instrument->layers[0].targets[Synth::mod_volume]                 = volume_tuple;
    instrument->layers[2].targets[Synth::mod_volume]                 = volume_tuple;
    Synth::TargetBinding second_tuple                                = volume_tuple;
    second_tuple.envelope_desc_id                                    = 2;
    instrument->layers[1].targets[Synth::mod_volume]                 = second_tuple;
    instrument->layers[0].targets[Synth::mod_pitch].envelope_desc_id = 2;

    Synth::TargetBinding& volume_routing                       = instrument->layers[0].targets[Synth::mod_volume];
    volume_routing.base_value                                  = 0.8f;
    volume_routing.num_inputs                                  = 1;
    volume_routing.inputs[0].source                            = Synth::ModSource::velocity;
    volume_routing.inputs[0].op                                = Synth::SourceOp::add;
    volume_routing.inputs[0].scale                             = 2.0f;
    instrument->layers[0].targets[Synth::mod_pitch].base_value = -2.0f;
    fan_out_routing(*instrument);

    bank->bank.instruments.entries[0] = *instrument;
}

// ---------------------------------------------------------------------------
// Clipboard portable layout records: fixtures, measurement and comparison.
// ---------------------------------------------------------------------------

// Real parser token count of one document under the vendored parser the codec
// uses. Returns -1 on any parse failure, including a document needing more tokens
// than the bound: it reports "cannot be measured within the bound", never a
// partial count.
int32_t count_clipboard_tokens(const char* text, uint32_t len)
{
    const std::unique_ptr<FixedArray<jsmntok_t, clipboard_token_bound>> tokens_storage =
        std::make_unique<FixedArray<jsmntok_t, clipboard_token_bound>>();
    jsmntok_t(&tokens)[clipboard_token_bound] = tokens_storage->elements;
    jsmn_parser parser;
    jsmn_init(&parser);
    const int parsed = jsmn_parse(&parser, text, len, tokens, clipboard_token_bound);
    return parsed < 0 ? -1 : static_cast<int32_t>(parsed);
}

void fill_clipboard_sentinels(ClipboardDecode* out)
{
    memset(static_cast<void*>(out), 0x5A, sizeof(*out));
    // The count fields sentinel at the destination capacities rather than a
    // bit pattern: a refused decode leaves them as lengths a later call in the
    // same block could consume, and a capacity keeps every such read in bounds.
    out->env_count    = Synth::instrument_max_envelopes;
    out->lfo_count    = Synth::instrument_max_lfos;
    out->layout_count = max_portable_records;
}

bool clipboard_sentinels_intact(const ClipboardDecode* a, const ClipboardDecode* b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

// Decodes `doc` into every tracked output with `layout_capacity` slots of layout
// output.
bool decode_clipboard(ClipboardDecode* out, const char* doc, uint32_t layout_capacity)
{
    return Synth::decode_instrument_json(doc,
                                         static_cast<uint32_t>(strlen(doc)),
                                         &out->instr,
                                         out->envs,
                                         &out->env_count,
                                         out->lfos,
                                         &out->lfo_count,
                                         out->layout,
                                         layout_capacity,
                                         &out->layout_count);
}

// One zone of one channel: a single layer with a distinct envelope and a distinct
// LFO on volume (depth from velocity, rate from mod wheel) and a second distinct
// envelope on pitch, so the instrument projects canonical, envelope and LFO
// nodes with pairwise distinct identities. No layout records, no
// effect chain.
void build_clipboard_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument                   = {};
    bank->bank.channel_enabled[0] = 1;
    TEST(bank->bank.instruments.allocate() == 0);
    bank->bank.channel_zones[0][0].start_note = 1;
    bank->bank.channel_zones[0][0].instrument = 0;

    for (uint32_t e = 0; e < 2; e++) {
        const uint32_t slot = bank->bank.envelopes.allocate();
        TEST(slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[slot];
        env.num_points                 = 3;
        env.sustain_first_point        = 1;
        env.sustain_last_point         = 2;
        env.min_value                  = -0.5f + 0.25f * static_cast<float>(e);
        env.min_max_delta              = 1.0f / 32768.0f;
        for (uint32_t p = 0; p < env.num_points; p++) {
            env.points[p].position = static_cast<uint16_t>(100 * p + 1);
            env.points[p].value    = static_cast<uint16_t>(0x2000 + 0x4000 * p + 0x100 * e);
        }
    }
    const uint32_t lfo_slot = bank->bank.lfos.allocate();
    TEST(lfo_slot != pool_no_slot);
    Synth::LFODescriptor& lfo = bank->bank.lfos.entries[lfo_slot];
    lfo.wave                  = Synth::WaveType::sine_wave;
    lfo.duty                  = 0x40;
    lfo.period_ms             = 250;
    lfo.min_value             = -1.0f;
    lfo.min_max_delta         = 2.0f;

    instrument->layer_count                          = 1;
    Synth::Oscillator& osc                           = instrument->layers[0];
    osc.osc_type[0]                                  = Synth::WaveType::sine_wave;
    osc.osc_type[1]                                  = Synth::WaveType::sawtooth_wave;
    osc.osc_mode                                     = Synth::osc_mode_fm;
    osc.mod_ratio                                    = 2.0f;
    osc.targets[Synth::mod_volume].envelope_desc_id  = 1;
    osc.targets[Synth::mod_volume].lfo_desc_id       = 1;
    osc.targets[Synth::mod_volume].lfo_op            = Synth::SourceOp::add;
    osc.targets[Synth::mod_volume].lfo_depth         = 0.5f;
    osc.targets[Synth::mod_volume].lfo_depth_source  = Synth::ModSource::velocity;
    osc.targets[Synth::mod_volume].lfo_rate_source   = Synth::ModSource::mod_wheel;
    osc.targets[Synth::mod_volume].lfo_rate_scale_ms = 20.0f;
    osc.targets[Synth::mod_pitch].envelope_desc_id   = 2;

    instrument->layers[0].targets[Synth::mod_volume].base_value     = 0.8f;
    instrument->layers[0].targets[Synth::mod_pitch].base_value      = 0.0f;
    instrument->layers[0].targets[Synth::mod_panning].base_value    = 0.5f;
    instrument->layers[0].targets[Synth::mod_fir_spread].base_value = 3.0f;
    instrument->layers[0].targets[Synth::mod_fm_ratio].base_value   = 2.0f;
    bank->bank.instruments.entries[0]                               = *instrument;
    strcpy(bank->instrument_names[0], "Clipboard zone");
}

bool export_clipboard_fixture(const Synth::InstrumentEditorBank& bank, char* text, uint32_t capacity)
{
    return Sculptor::encode_editor_instrument_json(bank,
                                                   0,
                                                   0,
                                                   text,
                                                   capacity,
                                                   Synth::InstrumentJsonDomain::clipboard) != 0;
}

void write_library_fixture(const char* path,
                           const char* payload,
                           uint32_t    length,
                           const char* category,
                           const char* name)
{
    FILE* const file = fopen(path, "wb");
    TEST(file != nullptr);
    if (! file) {
        return;
    }
    const uint32_t header[3]  = { 0x42494c49, Synth::library_version, 1 };
    char           record[52] = {};
    TEST(strlen(category) < 24 && strlen(name) < 24);
    snprintf(record, 24, "%s", category);
    snprintf(record + 24, 24, "%s", name);
    memcpy(record + 48, &length, 4);
    TEST(fwrite(header, sizeof(header), 1, file) == 1);
    TEST(fwrite(record, sizeof(record), 1, file) == 1);
    TEST(fwrite(payload, length, 1, file) == 1);
    TEST(fclose(file) == 0);
}

uint32_t read_library_fixture(const char* path, char* bytes, uint32_t capacity)
{
    FILE* const file = fopen(path, "rb");
    TEST(file != nullptr);
    if (! file) {
        return 0;
    }
    const size_t count = fread(bytes, 1, capacity, file);
    TEST(feof(file) && ! ferror(file));
    TEST(fclose(file) == 0);
    return static_cast<uint32_t>(count);
}

void build_named_fm_fixture(Synth::InstrumentEditorBank* bank)
{
    Synth::Instrument instrument;
    build_binding_fixture(bank, &instrument);

    for (uint32_t layer = 0; layer < instrument.layer_count; ++layer) {
        instrument.layers[layer].osc_type[1]                             = Synth::WaveType::sine_wave;
        instrument.layers[layer].osc_mode                                = Synth::osc_mode_fm;
        instrument.layers[layer].mod_ratio                               = 2.0f;
        instrument.layers[layer].targets[Synth::mod_fm_ratio].base_value = instrument.layers[layer].mod_ratio;
    }
    // Bank-side writes seed every layer (fixture uniformity, no bridge).
    for (uint32_t layer = 0; layer < instrument.layer_count; ++layer) {
        instrument.layers[layer].targets[Synth::mod_fm_index].base_value = 0.625f * 6.2831853f;
    }

    bank->bank.instruments.entries[0] = instrument;
    Sculptor::set_identity_osc_sources(bank, 0, 0);
    strcpy(bank->instrument_names[0], "Named FM");
    strcpy(bank->channel_names[0], "Source");

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(*bank, &graph, &mapping));

    add_detached_record(bank, 0, 0, 0, 2, 77, 88, Synth::ModSource::none, Synth::ModSource::none, 0);
    bank->graph_layout[0].width_override  = 321;
    bank->graph_layout[0].height_override = 222;
    add_detached_record(bank, 0, 0, 1, 1, 44, 55, Synth::ModSource::none, Synth::ModSource::none, 1);

    for (uint32_t canonical = 0; canonical < 2; ++canonical) {
        add_detached_record(bank,
                            0,
                            0,
                            0,
                            canonical,
                            500.0f + 100.0f * static_cast<float>(canonical),
                            -400.0f - 50.0f * static_cast<float>(canonical),
                            Synth::ModSource::none,
                            Synth::ModSource::none,
                            0);
        bank->graph_layout[bank->graph_layout_count - 1].width_override =
            350.0f + 10.0f * static_cast<float>(canonical);
        bank->graph_layout[bank->graph_layout_count - 1].height_override =
            250.0f + 20.0f * static_cast<float>(canonical);
    }

    TEST(Synth::validate_instrument_bank(&bank->bank));
    TEST(Sculptor::validate_editor_metadata(*bank));
}

// Replaces the single occurrence of `needle` in `payload` (length `len`)
// with `replacement`, writing the patched document to `out`.  Returns the
// patched length, or 0 when the needle does not occur exactly once or the
// patched document does not fit.  Used to shape hand-decoded routing
// variants the encoder itself refuses to emit.
uint32_t splice_single_occurrence(const char* payload,
                                  uint32_t    len,
                                  const char* needle,
                                  const char* replacement,
                                  char*       out,
                                  uint32_t    capacity)
{
    const uint32_t    needle_len      = static_cast<uint32_t>(strlen(needle));
    const uint32_t    replacement_len = static_cast<uint32_t>(strlen(replacement));
    const char* const hit             = strstr(payload, needle);
    if (! hit) {
        return 0;
    }
    if (strstr(hit + 1, needle) != nullptr) {
        return 0; // needle is not unique in the document
    }
    const uint32_t head = static_cast<uint32_t>(hit - payload);
    const uint32_t tail = len - head - needle_len;
    if (head + replacement_len + tail + 1 > capacity) {
        return 0;
    }
    memcpy(out, payload, head);
    memcpy(out + head, replacement, replacement_len);
    memcpy(out + head + replacement_len, payload + head + needle_len, tail);
    return head + replacement_len + tail;
}

void effects_clipboard_fixture(Synth::EffectsDocument* doc, bool midi)
{
    memset(static_cast<void*>(doc), 0, sizeof(*doc));
    doc->lfo_count = 3;
    for (uint32_t i = 0; i < doc->lfo_count; ++i) {
        doc->lfos[i] = { Synth::WaveType::sine_wave, 127, 333, -2.0f, 4.0f };
    }
    doc->chain.num_effects          = 4;
    const Synth::EffectType types[] = { Synth::EffectType::delay,
                                        Synth::EffectType::distortion,
                                        Synth::EffectType::delay,
                                        Synth::EffectType::fir };
    for (uint32_t s = 0; s < 4; ++s) {
        Synth::EffectSlotBinding& slot = doc->chain.effects[s];
        slot.type                      = types[s];
        slot.enabled                   = s != 2;
        for (uint32_t p = 0; p < Synth::get_effect_param_floats(slot.type); ++p) {
            Synth::EffectParamBinding& b = slot.bindings[p];
            b.base_value                 = -123.5f + static_cast<float>(s * 10 + p);
            if (slot.type == Synth::EffectType::fir && p == 2) {
                // The FIR slope percent needs a valid integer base; the
                // generic formula's negative fractional value is invalid.
                b.base_value = 100.0f;
            }
            b.lfo_desc_id =
                s == 1 ? 0
                       : static_cast<uint16_t>(s == 3 ? 2 : 1); // slot 1 stays unmodulated: the fixture rides three
                                                                // chains and must respect the modulated-param budget
            b.lfo_op         = Synth::SourceOp::multiply;
            b.lfo_depth      = 0.75f;
            b.lfo_rate_scale = 432.0f;
            if (midi) {
                b.lfo_depth_source = Synth::ModSource::mod_wheel;
                b.lfo_rate_source  = Synth::ModSource::pitch_bend;
                if (s != 1) { // slot 1 stays fully unmodulated: the fixture rides three chains and must respect the
                              // modulated-param budget
                    b.num_inputs       = 2;
                    b.inputs[0].source = Synth::ModSource::channel_pressure;
                    b.inputs[0].op     = Synth::SourceOp::add;
                    b.inputs[1].source = Synth::ModSource::none;
                    b.inputs[1].op     = Synth::SourceOp::multiply;
                }
            }
        }
    }
    const uint8_t  kinds[]   = { Synth::effects_graph_layout_input,  Synth::effects_graph_layout_output,
                                 Synth::effects_graph_layout_effect, Synth::effects_graph_layout_effect,
                                 Synth::effects_graph_layout_effect, Synth::effects_graph_layout_lfo,
                                 Synth::effects_graph_layout_lfo,    Synth::effects_graph_layout_lfo,
                                 Synth::effects_graph_layout_midi };
    const uint16_t indices[] = { 0, 0, 0, 2, 3, 1, 2, 3, 0 };
    doc->graph_layout_count  = midi ? 9 : 8;
    for (uint32_t i = 0; i < doc->graph_layout_count; ++i) {
        Synth::EffectsGraphLayout& layout = doc->graph_layout[i];
        layout.kind                       = kinds[i];
        layout.index                      = indices[i];
        layout.x                          = static_cast<float>(i * 32);
        layout.y                          = i ? -static_cast<float>(i * 16) : 0.0f;
        layout.width_override             = 240.0f + static_cast<float>(i);
        layout.height_override            = 180.0f;
    }
}

// One enabled delay effect on the chain a CC fixture builds on.  chain is a
// channel index or Sculptor::fx_master_chain.
Synth::EffectChainBinding& add_cc_chain_fixture(Synth::InstrumentEditorBank* bank, uint32_t chain)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    Synth::init_default_bank(&bank->bank);
    Sculptor::set_identity_osc_sources(bank, 0, 0);

    Synth::EffectChainBinding& fixture_chain = Sculptor::fx_graph_chain(&bank->bank, chain);

    fixture_chain.num_effects = 1;
    Sculptor::fx_init_slot(&fixture_chain, 0, Synth::EffectType::delay);
    fixture_chain.effects[0].enabled = true;
    return fixture_chain;
}

// Direct per-connection modulator rows: every oscillator node keeps its
// 18 fixed slots and adds one 12-slot block per ModTarget directly after
// its value row (block_base(t) = osc_target_row_slot[t] + 1); field 0 is
// the env dot, 1 the env depth, 2 the LFO dot, 3 the LFO op, 4 the LFO
// depth, 5 the rate scale, 6 and 9 the source dots, 7/8 and 10/11 the
// op/amount widgets; row groups are 3*t+1..4, the env row 3*t+4 packing
// the dot and its unlabeled depth on one line.  Row visibility is the live predicate
// osc_target_field_hidden(graph, node, target, field).

// Index of the first projected detached generator matching (kind, desc_id),
// or -1.  kind is 1, 2 = LFO; desc_id is the 1-based pool id.
int32_t find_detached_by_desc(const Sculptor::OscGraphMapping& mapping, uint32_t kind, uint32_t desc_id)
{
    for (uint32_t i = 0; i < mapping.detached_count; i++) {
        if (mapping.detached[i].kind == kind && mapping.detached[i].desc_id == desc_id &&
            mapping.detached[i].node_idx != Sculptor::pool_no_slot) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Rebuilds the graph from the bank's current state and drains the projection
// changes; false skips the caller's block.
bool reproject_m2_band_graph(Synth::InstrumentEditorBank* bank,
                             Sculptor::Graph*             graph,
                             Sculptor::OscGraphMapping*   mapping)
{
    if (! Sculptor::project_editor_to_graph(*bank, graph, mapping)) {
        TEST(false);
        return false;
    }
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes] = {};
    (void)graph->take_changes(discarded, Sculptor::max_pending_changes);
    return true;
}

// Stages one oscillator slot edit on the graph, drains it and applies the
// value change to the bank; the click-style band slots ride this path.
bool apply_m2_slot_edit(Synth::InstrumentEditorBank*  bank,
                        Sculptor::Graph*              graph,
                        Sculptor::OscGraphMapping*    mapping,
                        const uint32_t                node_idx,
                        const uint32_t                slot_idx,
                        const Sculptor::PropertyValue value)
{
    graph->set_slot_value(node_idx, slot_idx, value);
    // Graph::set_slot_value is deliberately eventless (the shared-routing
    // fan-out relies on no echo, pinned in sculptor_graph_unit.cpp), so the
    // staged value rides the established direct-change pattern when the
    // setter staged nothing.
    Sculptor::GraphChange changes[Sculptor::max_pending_changes] = {};
    uint32_t              count = graph->take_changes(changes, Sculptor::max_pending_changes);
    if (count == 0) {
        changes[0].kind     = Sculptor::ChangeKind::value_changed;
        changes[0].node_idx = node_idx;
        changes[0].slot_idx = slot_idx;
        count               = 1;
    }
    TEST(count >= 1);
    bool applied = count >= 1;
    for (uint32_t i = 0; i < count; i++) {
        if (changes[i].node_idx != node_idx || changes[i].slot_idx != slot_idx ||
            changes[i].kind != Sculptor::ChangeKind::value_changed) {
            continue;
        }
        applied = Sculptor::apply_osc_graph_change(bank, graph, mapping, changes[i]) && applied;
    }
    TEST(applied);
    return applied;
}

// realtime_synth.cpp is not linked into this unit.  pump_live_midi in
// midi_input.cpp calls apply_midi_event, so this no-op stub swallows the
// drained events.
void Synth::apply_midi_event(const Synth::MidiEvent&) {}

void make_valid_bank(Synth::InstrumentBank& bank)
{
    memset(static_cast<void*>(&bank), 0, sizeof(bank));
    const uint32_t env                             = bank.envelopes.allocate();
    bank.envelopes.entries[env].num_points         = 2;
    bank.envelopes.entries[env].points[1].position = 100;
    const uint32_t lfo                             = bank.lfos.allocate();
    bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
    bank.lfos.entries[lfo].period_ms               = 50;
    const uint32_t instr                           = bank.instruments.allocate();
    bank.instruments.entries[instr].layer_count    = 1;
    set_default_dispersion_sections(bank.instruments.entries[instr]);
    bank.instruments.entries[instr].layers[0].fir_slope_percent          = 100;
    bank.instruments.entries[instr].layers[0].string_dispersion_sections = 4.0f;
    bank.instruments.entries[instr].layers[0].targets[Synth::mod_volume].envelope_desc_id =
        static_cast<uint16_t>(env + 1);
    bank.instruments.entries[instr].layers[0].targets[Synth::mod_pitch].lfo_desc_id = static_cast<uint16_t>(lfo + 1);
    bank.channel_zones[0][0] = { 1, static_cast<uint8_t>(instr) };
    bank.channel_enabled[0]  = 1;
    bank.channel_enabled[1]  = 1; // zone-rule negatives exercise channel 1
}

int test_exit_code()
{
    return exit_code;
}

void test_silence_stdout()
{
#ifdef _WIN32
    const bool silenced = freopen("NUL", "w", stdout) != nullptr;
#else
    const bool silenced = freopen("/dev/null", "w", stdout) != nullptr;
#endif
    (void)silenced;
}
