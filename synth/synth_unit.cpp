// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/rng.h"
#include "../sculptor/sculptor_bank_json.h"
#include "../sculptor/sculptor_effect_graph.h"
#include "../sculptor/sculptor_graph.h"
#include "../sculptor/sculptor_instr_bank.h"
#include "../sculptor/sculptor_instr_envelope_edit.h"
#include "../sculptor/sculptor_instr_library.h"
#include "../sculptor/sculptor_osc_graph.h"
#include "midi_file.h"
#include "synth_effect_expansion.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_parameters.h"
#include "synth_serialize.h"
#include "synth_soundtrack.h"
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
#include <cmath>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(test)                         \
    if (! (test)) {                        \
        failed(#test, __FILE__, __LINE__); \
    }

namespace {

// Fixed portable-record bound: the nine canonical nodes plus one envelope, one
// LFO and one parameter group per projected (layer,target) cell.
constexpr uint32_t max_portable_records = Synth::graph_canonical_node_count + 3u * Sculptor::max_param_nodes;
static_assert(max_portable_records == Synth::instrument_graph_layout_capacity);
static_assert(max_portable_records == 114, "9 canonical + 35 envelopes + 35 LFOs + 35 parameter groups");

// Parser token bound the clipboard codec is specified to accept.
constexpr uint32_t clipboard_token_bound = 8192;
constexpr uint32_t fake_region_base      = 8192;

// The editor's clipboard staging capacity. The copy path encodes into
// capacity - 1 and terminates the document itself, so a document of L payload
// bytes needs capacity >= L + 1. This mirrors the editor's staging buffer in
// sculptor_instr_edit.cpp; a larger document must raise that buffer, never
// truncate the document.
constexpr uint32_t clipboard_capacity = 64 * 1024;

// One instrument document with one saved position per node kind, assembled from
// a fixed body and a caller-supplied record list, so a rejection test changes
// only the record under inspection.
const char clipboard_doc_head[] =
    "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
    "\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":2,\"pitch_offset_semitones\":1.5,"
    "\"volume\":{\"base\":0.8,\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.0625},"
    "{\"time_seconds\":0.25,\"level\":0.5625},{\"time_seconds\":0.5,\"level\":1.0625}],\"sustain_start\":1,"
    "\"sustain_end\":2},\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":4,\"duty\":0.25,\"depth\":0.5,"
    "\"depth_source\":\"velocity\",\"rate_source\":\"mod_wheel\",\"rate_scale_ms\":20}},"
    "\"pitch\":{\"base\":0,\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
    "{\"time_seconds\":0.5,\"level\":-0.25}],\"sustain_start\":0,\"sustain_end\":1}},"
    "\"panning\":{\"base\":0.5},\"lowpass_cutoff\":{\"base\":2000},\"highpass_cutoff\":{\"base\":2000}}],";

// The same instrument with a volume envelope byte-identical to the pitch
// envelope's, which the decoder interns into one descriptor and the projection
// therefore serves from one node.
const char clipboard_alias_head[] =
    "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
    "\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":2,\"pitch_offset_semitones\":1.5,"
    "\"volume\":{\"base\":0.8,\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
    "{\"time_seconds\":0.5,\"level\":-0.25}],\"sustain_start\":0,\"sustain_end\":1}},"
    "\"pitch\":{\"base\":0,\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
    "{\"time_seconds\":0.5,\"level\":-0.25}],\"sustain_start\":0,\"sustain_end\":1}},"
    "\"panning\":{\"base\":0.5}}],";

// The five records a copy of the clipboard fixture's zone carries: the sum node
// (canonical numbering is input 0, sum 1, oscillator layer n = 2 + n, so this
// one-layer zone owns no canonical index above 2), the volume envelope, the
// volume LFO with its bound source pair, and the volume and pitch parameters.
const char clipboard_doc_records[] = "{\"kind\":0,\"canonical_index\":1,\"x\":64,\"y\":32,\"width\":0,\"height\":0},"
                                     "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":120.5,\"y\":44.25,\"width\":220,"
                                     "\"height\":120},"
                                     "{\"kind\":2,\"layer\":0,\"target\":0,\"depth_source\":4,\"rate_source\":2,"
                                     "\"x\":12.5,\"y\":-8.5,\"width\":0,\"height\":0},"
                                     "{\"kind\":3,\"target\":0,\"parameter_ordinal\":0,\"x\":60,\"y\":-40,\"width\":0,"
                                     "\"height\":0},"
                                     "{\"kind\":3,\"target\":1,\"parameter_ordinal\":0,\"x\":-60,\"y\":40,\"width\":0,"
                                     "\"height\":0}";

} // namespace

// Every output of one clipboard decode, so a rejected document can be shown to
// have left all of them - both descriptor arrays and all three counts -
// byte-identical.
struct ClipboardDecode {
    Synth::Instrument            instr;
    Synth::EnvelopeDescriptor    envs[Synth::instrument_max_envelopes];
    Synth::LFODescriptor         lfos[Synth::instrument_max_lfos];
    Synth::InstrumentGraphLayout layout[max_portable_records];
    uint32_t                     env_count;
    uint32_t                     lfo_count;
    uint32_t                     layout_count;
};

struct FakeWriter {
    uint32_t next_node;
    uint32_t dest_count;
    uint32_t lfo_count;
    uint16_t last_dest_node;
    uint16_t last_dest_lfo_node;
};

static int exit_code = 0;

static void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n", file, line, test);
}

static bool approx(float left, float right, float eps)
{
    float diff = left - right;
    if (diff < 0.0f) {
        diff = -diff;
    }
    return diff <= eps;
}

// True for the five modulation targets the oscillator graph exposes bindings for
// (the runtime's modulatable set: volume, pitch, panning, lowpass, highpass).  The
// remaining targets (duty A/B, osc mix, FM index) are unmodulated constants that
// carry only base values, so the graph projects no connectors or generators for
// them and their routing stays input-free.
static bool osc_graph_target(Synth::ModTarget target)
{
    return target == Synth::mod_volume || target == Synth::mod_pitch || target == Synth::mod_panning ||
           target == Synth::mod_lowpass_cutoff || target == Synth::mod_highpass_cutoff;
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

static uint32_t find_osc_graph_connection(const Sculptor::Graph& graph, const Sculptor::EndPoint& input);

// Checks via TEST that a projection filled the mapping consistently: every
// fixed node exists and is distinct, each occupied layer's oscillator has
// exactly 15 rows, every parameter entry names a distinct 13-row node, and
// the instance registry carries exactly env_instances + lfo_instances
// instances.  The distinct node total is 2 + layer_count + param_count +
// env_instances + lfo_instances (the inputs node + sum + oscillators +
// parameters + generator instances).
static void check_osc_graph_mapping(const Sculptor::Graph&           graph,
                                    const Sculptor::OscGraphMapping& mapping,
                                    uint32_t                         layer_count,
                                    uint32_t                         param_count,
                                    uint32_t                         env_instances,
                                    uint32_t                         lfo_instances)
{
    bool     seen[Sculptor::max_nodes] = {};
    uint32_t count                     = 0;
    TEST(mapping.input_node != Sculptor::pool_no_slot);
    TEST(osc_graph_mark_node(seen, mapping.input_node, &count));
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
    TEST(mapping.param_output_slot != Sculptor::pool_no_slot);
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        if (layer < layer_count) {
            TEST(mapping.osc_nodes[layer] != Sculptor::pool_no_slot);
            TEST(osc_graph_mark_node(seen, mapping.osc_nodes[layer], &count));
            // 15 rows: output, five connectable target values, four shared
            // constants, three lists and two per-oscillator reals; no input rows.
            TEST(graph.node(mapping.osc_nodes[layer]).slots.num_allocated == 15);
            for (uint32_t s = 0; s < 15; s++) {
                TEST(graph.node(mapping.osc_nodes[layer]).slots.entries[s].kind != Sculptor::SlotKind::input);
            }
        }
        else {
            TEST(mapping.osc_nodes[layer] == Sculptor::pool_no_slot);
        }
    }
    TEST(mapping.param_count == param_count);
    for (uint32_t p = 0; p < mapping.param_count; p++) {
        const Sculptor::ParamEntry& param = mapping.params[p];
        TEST(param.target < 5);
        if (param.node_idx == Sculptor::pool_no_slot) {
            continue; // unprojected surplus record
        }
        TEST(osc_graph_mark_node(seen, param.node_idx, &count));
        TEST(graph.node(param.node_idx).slots.num_allocated == 13);
        // Every wire into a target value row comes from a parameter of that
        // target (the fan-out may cover any subset of layers).
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            const uint32_t conn =
                find_osc_graph_connection(graph, { mapping.osc_nodes[layer], Sculptor::osc_target_row(param.target) });
            if (conn != Sculptor::pool_no_slot) {
                const uint32_t src              = graph.get_connection(conn).output.node_idx;
                bool           from_same_target = false;
                for (uint32_t q = 0; q < mapping.param_count; q++) {
                    from_same_target = from_same_target ||
                                       (mapping.params[q].target == param.target && mapping.params[q].node_idx == src);
                }
                TEST(from_same_target);
                TEST(graph.get_connection(conn).output.slot_idx == mapping.param_output_slot);
            }
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
    TEST(count == 2 + layer_count + param_count + env_instances + lfo_instances);
}

// Finds a slot by name on a node; slots are scanned in creation order.
static bool find_graph_slot(const Sculptor::Graph& graph, uint32_t node_idx, const char* name, uint32_t* slot_idx)
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

// The connection terminating at an input endpoint, or pool_no_slot.
static uint32_t find_osc_graph_connection(const Sculptor::Graph& graph, const Sculptor::EndPoint& input)
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

// Connection matching both endpoints: fx reference wires land on rows that
// also carry property state, so endpoint identity alone is the test key.
static uint32_t find_fx_graph_connection(const Sculptor::Graph&    graph,
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

// Index of the first parameter entry with the given target and served
// bitset, or -1.
static int32_t find_param_by_served(const Sculptor::OscGraphMapping& mapping, uint32_t target, uint8_t served)
{
    for (uint32_t p = 0; p < mapping.param_count; p++) {
        if (mapping.params[p].target == target && mapping.params[p].served == served) {
            return static_cast<int32_t>(p);
        }
    }
    return -1;
}

// Registry index of the instance projected onto `node_idx`, or -1.
static int32_t find_instance_by_node(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    for (uint32_t i = 0; i < mapping.detached_count; i++) {
        if (mapping.detached[i].node_idx == node_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Builds a max-complexity projection fixture: 7 layers, an envelope and an LFO
// bound on every layer for each of the five projected targets, full two-input
// routing with mixed ops and scales (volume repeats velocity across its two
// ordered inputs), nonzero skews, and one LFO descriptor aliased across two
// layers.  Writes the referenced 1-based descriptor ids (0 = unbound), indexed
// [layer][Synth::ModTarget], to env_ids/lfo_ids.
static void build_osc_graph_max_fixture(Synth::InstrumentBank* bank,
                                        Synth::Instrument*     instrument,
                                        uint16_t (*env_ids)[Synth::num_mod_targets],
                                        uint16_t (*lfo_ids)[Synth::num_mod_targets])
{
    *bank       = {};
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
        osc.pitch_offset       = 0.5f * static_cast<float>(layer) - 1.5f;
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            if (! osc_graph_target(static_cast<Synth::ModTarget>(t))) {
                continue; // duty A/B, osc mix and FM index stay unmodulated constants
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

            Synth::LayerGen& gen  = osc.gen[t];
            gen.envelope_desc_id  = env_ids[layer][t];
            gen.lfo_desc_id       = lfo_ids[layer][t];
            gen.lfo_op            = ((layer + t) % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
            gen.lfo_depth         = 0.25f * static_cast<float>(layer + 1);
            gen.lfo_depth_source  = static_cast<Synth::ModSource>(1 + ((layer + t) % 6));
            gen.lfo_rate_source   = static_cast<Synth::ModSource>(1 + ((layer + 2 * t + 1) % 6));
            gen.lfo_rate_scale_ms = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(t);
        }
    }
    TEST(env_count == 35);
    TEST(lfo_count == 34); // 35 bindings minus the one alias

    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        Synth::InputRouting& routing = instrument->routing[t];
        routing.base_value           = -1.0f + 0.25f * static_cast<float>(t);
        if (! osc_graph_target(static_cast<Synth::ModTarget>(t))) {
            continue; // non-projected targets stay pure constants (no routed inputs)
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
    instrument->note_skew_semitones  = 0.3f;
    instrument->layer_skew_semitones = -0.7f;
}

// Builds a minimal projection fixture with an aliased envelope pair: one layer,
// volume bound to an envelope and an LFO, pitch bound to an envelope sharing the
// volume envelope's descriptor id.  The LFO's depth/rate sources stay unset so
// the LFO node's source connectors are free for validator connection attempts.
// Writes the pitch envelope id, the LFO id and the volume envelope id (all 1-based).
static void build_osc_graph_small_fixture(Synth::InstrumentBank* bank,
                                          Synth::Instrument*     instrument,
                                          uint16_t*              shared_env_id,
                                          uint16_t*              lfo_id,
                                          uint16_t*              volume_env_id)
{
    *bank       = {};
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
    // first and last point 0) cannot serve a non-volume target, so volume and
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
    osc.pitch_offset        = 3.5f;

    osc.gen[Synth::mod_volume].envelope_desc_id = *volume_env_id;
    osc.gen[Synth::mod_volume].lfo_desc_id      = *lfo_id;
    osc.gen[Synth::mod_volume].lfo_op           = Synth::SourceOp::add;
    osc.gen[Synth::mod_volume].lfo_depth        = 0.5f;
    osc.gen[Synth::mod_pitch].envelope_desc_id  = *shared_env_id;
    osc.gen[Synth::mod_pitch].lfo_op            = Synth::SourceOp::multiply;
    osc.gen[Synth::mod_pitch].lfo_depth         = 1.0f;

    instrument->routing[Synth::mod_volume].base_value       = 0.8f;
    instrument->routing[Synth::mod_volume].num_inputs       = 1;
    instrument->routing[Synth::mod_volume].inputs[0].source = Synth::ModSource::velocity;
    instrument->routing[Synth::mod_volume].inputs[0].op     = Synth::SourceOp::add;
    instrument->routing[Synth::mod_volume].inputs[0].scale  = 2.0f;
    instrument->routing[Synth::mod_pitch].base_value        = -2.0f;
    instrument->note_skew_semitones                         = 0.1f;
}

static uint32_t fake_alloc_node(const void* ctx)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    return writer.next_node++;
}

static uint16_t fake_resolve_source(const void*, Synth::ModSource, uint32_t)
{
    return 77; // arbitrary concrete node id
}

static void fake_configure_dest(const void* ctx,
                                uint32_t    node,
                                uint16_t    lfo_node,
                                float,
                                Synth::SourceOp,
                                const Synth::SourceParam*,
                                uint32_t)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.dest_count++;
    writer.last_dest_node     = static_cast<uint16_t>(node);
    writer.last_dest_lfo_node = lfo_node;
}

static void fake_configure_lfo(const void* ctx, uint32_t, uint16_t, Synth::SourceOp, float, uint16_t, uint16_t, float)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    writer.lfo_count++;
}

static const Synth::EffectNodeWriter fake_writer_binding(FakeWriter& writer)
{
    const Synth::EffectNodeWriter result = { &writer,
                                             fake_alloc_node,
                                             fake_resolve_source,
                                             fake_configure_dest,
                                             fake_configure_lfo };
    return result;
}

static Synth::EffectSlotBinding* enabled_effect(Synth::InstrumentBank& bank,
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
static void add_detached_record(Synth::InstrumentEditorBank* bank,
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
    record                         = {};
    record.channel                 = static_cast<uint8_t>(channel);
    record.zone                    = static_cast<uint8_t>(zone);
    record.kind                    = static_cast<uint8_t>(kind);
    record.index                   = static_cast<uint8_t>(desc_id);
    record.x                       = x;
    record.y                       = y;
    record.depth_source            = static_cast<uint8_t>(depth_source);
    record.rate_source             = static_cast<uint8_t>(rate_source);
    record.uid                     = uid;
}

// Builds an editor bank whose channel 0 zone 0 holds the small projection
// fixture instrument (bound envelope + LFO on volume, envelope on pitch) with
// a valid zone table; no layout records yet.
static void build_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    *bank                  = {};
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
static uint16_t add_detached_lfo_descriptor(Synth::InstrumentEditorBank* bank)
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
static uint32_t count_records_matching(const Synth::InstrumentEditorBank& bank,
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

// Builds {"editor":{"layouts":[...]}} with `total` detached records of one
// kind, distributed round-robin over `zone_count` zones of channel 0 with
// per-zone-incrementing uids.
static uint32_t build_detached_layouts_json(char*    dest,
                                            uint32_t size,
                                            uint32_t zone_count,
                                            uint32_t total,
                                            uint32_t kind,
                                            uint32_t index,
                                            uint32_t depth_source,
                                            uint32_t rate_source)
{
    uint32_t pos = 0;
    pos += static_cast<uint32_t>(snprintf(dest + pos, size - pos, "{\"layouts\":["));
    for (uint32_t i = 0; i < total; i++) {
        const uint32_t zone = zone_count > 0 ? i % zone_count : 0;
        const uint32_t uid  = (i / zone_count) + 1;
        const int written = snprintf(dest + pos,
                                     size - pos,
                                     "{\"channel\":0,\"zone\":%u,\"kind\":%u,\"index\":%u,\"x\":11.0,\"y\":22.0,"
                                     "\"width\":0.0,\"height\":0.0,\"depth_source\":%u,\"rate_source\":%u,\"uid\":%u},",
                                     zone,
                                     kind,
                                     index,
                                     depth_source,
                                     rate_source,
                                     uid);
        if (written < 0 || pos + static_cast<uint32_t>(written) >= size) {
            TEST(false); // builder buffer too small
            return 0;
        }
        pos += static_cast<uint32_t>(written);
    }
    if (pos > 0 && dest[pos - 1] == ',') {
        pos--; // drop the trailing comma
    }
    {
        const int written = snprintf(dest + pos, size - pos, "]}");
        if (written < 0 || pos + static_cast<uint32_t>(written) >= size) {
            TEST(false);
            return 0;
        }
        pos += static_cast<uint32_t>(written);
    }
    return pos;
}

// Encodes `bank`, splices `"editor":<editor_json>` in as a proper key-value
// pair before the final close brace, and decodes the result into *out.
// `editor_json` is the editor object's inner value, e.g. {"layouts":[...]}.
static bool decode_bank_with_editor_section(const Synth::InstrumentEditorBank& bank,
                                            const char*                        editor_json,
                                            Synth::InstrumentEditorBank*       out)
{
    static char    doc[512 * 1024];
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

// Index of the projected node named `name`, or pool_no_slot.
static uint32_t find_graph_node_by_name(const Sculptor::Graph& graph, const char* name)
{
    for (uint32_t i = 0; i < Sculptor::max_nodes; i++) {
        if (graph.node_occupied(i) && strncmp(graph.node(i).name, name, sizeof(graph.node(i).name)) == 0) {
            return i;
        }
    }
    return Sculptor::pool_no_slot;
}

// Number of projected nodes named `name`.
static uint32_t count_graph_nodes_named(const Sculptor::Graph& graph, const char* name)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < Sculptor::max_nodes; i++) {
        if (graph.node_occupied(i) && strncmp(graph.node(i).name, name, sizeof(graph.node(i).name)) == 0) {
            count++;
        }
    }
    return count;
}

// Appends one kind-3 parameter layout record; index is the dynamic-target
// projection order (0 = volume .. 4 = highpass).
static void add_parameter_record(Synth::InstrumentEditorBank* bank,
                                 uint32_t                     channel,
                                 uint32_t                     zone,
                                 uint32_t                     target_index,
                                 float                        x,
                                 float                        y,
                                 uint8_t                      uid)
{
    TEST(bank->graph_layout_count < Synth::max_graph_records);
    Synth::GraphNodeLayout& record = bank->graph_layout[bank->graph_layout_count++];
    record                         = {};
    record.channel                 = static_cast<uint8_t>(channel);
    record.zone                    = static_cast<uint8_t>(zone);
    record.kind                    = 3;
    record.index                   = static_cast<uint8_t>(target_index);
    record.x                       = x;
    record.y                       = y;
    record.uid                     = uid;
}

// Channel 0 zone 0 parameter-node fixture: three layers; volume is bound on
// layers 0 and 2 with one shared envelope + LFO tuple (depth source velocity,
// rate source mod wheel) and on layer 1 with a second envelope descriptor
// (different tuple, second parameter); pitch is bound on layer 0 with that
// second envelope; layer 1/2 pitch stay unbound constants.  Volume routing
// carries one velocity input.  No layout records.
static void build_parameter_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    *bank                         = {};
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
        instrument->layers[layer].osc_type[0]  = Synth::WaveType::sawtooth_wave;
        instrument->layers[layer].pitch_offset = -2.0f;
    }
    Synth::LayerGen volume_tuple                                 = {};
    volume_tuple.envelope_desc_id                                = 1;
    volume_tuple.lfo_desc_id                                     = 1;
    volume_tuple.lfo_op                                          = Synth::SourceOp::add;
    volume_tuple.lfo_depth                                       = 0.5f;
    volume_tuple.lfo_depth_source                                = Synth::ModSource::velocity;
    volume_tuple.lfo_rate_source                                 = Synth::ModSource::mod_wheel;
    volume_tuple.lfo_rate_scale_ms                               = 20.0f;
    instrument->layers[0].gen[Synth::mod_volume]                 = volume_tuple;
    instrument->layers[2].gen[Synth::mod_volume]                 = volume_tuple;
    Synth::LayerGen second_tuple                                 = volume_tuple;
    second_tuple.envelope_desc_id                                = 2;
    instrument->layers[1].gen[Synth::mod_volume]                 = second_tuple;
    instrument->layers[0].gen[Synth::mod_pitch].envelope_desc_id = 2;

    Synth::InputRouting& volume_routing              = instrument->routing[Synth::mod_volume];
    volume_routing.base_value                        = 0.8f;
    volume_routing.num_inputs                        = 1;
    volume_routing.inputs[0].source                  = Synth::ModSource::velocity;
    volume_routing.inputs[0].op                      = Synth::SourceOp::add;
    volume_routing.inputs[0].scale                   = 2.0f;
    instrument->routing[Synth::mod_pitch].base_value = -2.0f;

    bank->bank.instruments.entries[0] = *instrument;
}

// ---------------------------------------------------------------------------
// Clipboard portable layout records: fixtures, measurement and comparison.
// ---------------------------------------------------------------------------

// Real parser token count of one document under the vendored parser the codec
// uses. Returns -1 on any parse failure, including a document needing more tokens
// than the bound: it reports "cannot be measured within the bound", never a
// partial count.
static int32_t count_clipboard_tokens(const char* text, uint32_t len)
{
    static jsmntok_t tokens[clipboard_token_bound];
    jsmn_parser      parser;
    jsmn_init(&parser);
    const int parsed = jsmn_parse(&parser, text, len, tokens, clipboard_token_bound);
    return parsed < 0 ? -1 : static_cast<int32_t>(parsed);
}

static void fill_clipboard_sentinels(ClipboardDecode* out)
{
    memset(out, 0x5A, sizeof(*out));
    // The count fields sentinel at the destination capacities rather than a
    // bit pattern: a refused decode leaves them as lengths a later call in the
    // same block could consume, and a capacity keeps every such read in bounds.
    out->env_count    = Synth::instrument_max_envelopes;
    out->lfo_count    = Synth::instrument_max_lfos;
    out->layout_count = max_portable_records;
}

static bool clipboard_sentinels_intact(const ClipboardDecode* a, const ClipboardDecode* b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

// Decodes `doc` into every tracked output with `layout_capacity` slots of layout
// output.
static bool decode_clipboard(ClipboardDecode* out, const char* doc, uint32_t layout_capacity)
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

// Index of the first portable record matching a locator, where a field passed as
// 0xFF is a wildcard, or -1 when no record matches.
static int32_t find_portable(const Synth::InstrumentGraphLayout* records,
                             uint32_t                            count,
                             uint32_t                            kind,
                             uint32_t                            layer,
                             uint32_t                            target,
                             uint32_t                            ordinal)
{
    for (uint32_t i = 0; i < count; i++) {
        const Synth::InstrumentGraphLayout& r = records[i];
        if (kind != 0xFFu && r.kind != kind) {
            continue;
        }
        if (layer != 0xFFu && r.layer != layer) {
            continue;
        }
        if (target != 0xFFu && r.target != target) {
            continue;
        }
        if (ordinal != 0xFFu && r.parameter_ordinal != ordinal) {
            continue;
        }
        return static_cast<int32_t>(i);
    }
    return -1;
}

// One zone of one channel: a single layer with a distinct envelope and a distinct
// LFO on volume (depth from velocity, rate from mod wheel) and a second distinct
// envelope on pitch, so the instrument projects canonical, envelope, LFO and
// parameter nodes with pairwise distinct identities. No layout records, no
// effect chain.
static void build_clipboard_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument)
{
    *bank                         = {};
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

    instrument->layer_count                      = 1;
    Synth::Oscillator& osc                       = instrument->layers[0];
    osc.osc_type[0]                              = Synth::WaveType::sine_wave;
    osc.osc_type[1]                              = Synth::WaveType::sawtooth_wave;
    osc.osc_mode                                 = Synth::osc_mode_fm;
    osc.mod_ratio                                = 2.0f;
    osc.pitch_offset                             = 1.5f;
    osc.gen[Synth::mod_volume].envelope_desc_id  = 1;
    osc.gen[Synth::mod_volume].lfo_desc_id       = 1;
    osc.gen[Synth::mod_volume].lfo_op            = Synth::SourceOp::add;
    osc.gen[Synth::mod_volume].lfo_depth         = 0.5f;
    osc.gen[Synth::mod_volume].lfo_depth_source  = Synth::ModSource::velocity;
    osc.gen[Synth::mod_volume].lfo_rate_source   = Synth::ModSource::mod_wheel;
    osc.gen[Synth::mod_volume].lfo_rate_scale_ms = 20.0f;
    osc.gen[Synth::mod_pitch].envelope_desc_id   = 2;

    instrument->routing[Synth::mod_volume].base_value          = 0.8f;
    instrument->routing[Synth::mod_pitch].base_value           = 0.0f;
    instrument->routing[Synth::mod_panning].base_value         = 0.5f;
    instrument->routing[Synth::mod_lowpass_cutoff].base_value  = 2000.0f;
    instrument->routing[Synth::mod_highpass_cutoff].base_value = 2000.0f;
    bank->bank.instruments.entries[0]                          = *instrument;
    strcpy(bank->instrument_names[0], "Clipboard zone");
}

// Validates a hand-built instrument model by installing it, and the descriptor
// pools its generator ids name, in a one-zone bank: the projection whose node
// identities the normalizer keys on only exists for a model the runtime accepts.
static bool validate_standalone_model(const Synth::Instrument&         instrument,
                                      const Synth::EnvelopeDescriptor* envelopes,
                                      uint32_t                         envelope_count,
                                      const Synth::LFODescriptor*      lfos,
                                      uint32_t                         lfo_count)
{
    static Synth::InstrumentBank probe_bank;
    memset(&probe_bank, 0, sizeof(probe_bank));
    for (uint32_t e = 0; e < envelope_count; e++) {
        const uint32_t env_slot = probe_bank.envelopes.allocate();
        TEST(env_slot != pool_no_slot);
        probe_bank.envelopes.entries[env_slot] = envelopes[e];
    }
    for (uint32_t l = 0; l < lfo_count; l++) {
        const uint32_t lfo_slot = probe_bank.lfos.allocate();
        TEST(lfo_slot != pool_no_slot);
        probe_bank.lfos.entries[lfo_slot] = lfos[l];
    }
    const uint32_t instrument_slot = probe_bank.instruments.allocate();
    TEST(instrument_slot == 0);
    probe_bank.instruments.entries[instrument_slot] = instrument;
    probe_bank.channel_enabled[0]                   = 1;
    probe_bank.channel_zones[0][0].start_note       = 1;
    probe_bank.channel_zones[0][0].instrument       = static_cast<uint8_t>(instrument_slot);
    return Synth::validate_instrument_bank(&probe_bank);
}

// Largest clipboard document: seven layers, an eight-point envelope and a
// distinct LFO with a distinct source pair on every projected target (35 distinct
// generator tuples, hence 35 parameter groups), two routing inputs on every
// projected target and base values on the constant targets, plus one portable
// record for every canonical node and for every envelope, LFO and parameter
// group. Returns the record count.
static uint32_t build_clipboard_max_fixture(Synth::InstrumentBank*        bank,
                                            Synth::Instrument*            instrument,
                                            Synth::InstrumentGraphLayout* records)
{
    *bank                   = {};
    *instrument             = {};
    instrument->layer_count = Synth::max_layers;
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        Synth::Oscillator& osc = instrument->layers[layer];
        osc.osc_type[0]        = Synth::WaveType::sine_wave;
        osc.osc_type[1]        = Synth::WaveType::sawtooth_wave;
        osc.osc_mode           = static_cast<Synth::OscMode>(layer % 3);
        osc.mod_ratio          = 1.0f + 0.25f * static_cast<float>(layer);
        osc.pitch_offset       = 0.5f * static_cast<float>(layer) - 1.5f;
        for (uint32_t cell = 0; cell < 5; cell++) {
            const Synth::ModTarget target = Synth::graph_projected_target(cell);

            const uint32_t env_slot = bank->envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            Synth::EnvelopeDescriptor& env = bank->envelopes.entries[env_slot];
            env.num_points                 = Synth::max_envelope_points;
            env.sustain_first_point        = 0;
            env.sustain_last_point         = static_cast<uint8_t>(Synth::max_envelope_points - 1);
            env.min_value                  = -0.5f + 0.03125f * static_cast<float>(env_slot);
            env.min_max_delta              = 1.0f / 16384.0f;
            for (uint32_t p = 0; p < env.num_points; p++) {
                env.points[p].position = static_cast<uint16_t>(100 * p + 1 + env_slot);
                env.points[p].value    = static_cast<uint16_t>(0x1000 + 0x2000 * p + 0x40 * env_slot);
            }

            const uint32_t lfo_slot = bank->lfos.allocate();
            TEST(lfo_slot != pool_no_slot);
            Synth::LFODescriptor& lfo = bank->lfos.entries[lfo_slot];
            lfo.wave          = (lfo_slot & 1) != 0 ? Synth::WaveType::sawtooth_wave : Synth::WaveType::sine_wave;
            lfo.duty          = static_cast<uint8_t>(0x20 + (lfo_slot % 0x60));
            lfo.period_ms     = static_cast<uint16_t>(50 + 7 * lfo_slot);
            lfo.min_value     = -0.5f + 0.0625f * static_cast<float>(lfo_slot);
            lfo.min_max_delta = 0.5f + 0.125f * static_cast<float>(lfo_slot);

            Synth::LayerGen& gen  = osc.gen[target];
            gen.envelope_desc_id  = static_cast<uint16_t>(env_slot + 1);
            gen.lfo_desc_id       = static_cast<uint16_t>(lfo_slot + 1);
            gen.lfo_op            = ((layer + cell) % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
            gen.lfo_depth         = 0.25f * static_cast<float>(layer + 1);
            gen.lfo_depth_source  = static_cast<Synth::ModSource>(1 + ((layer + cell) % 6));
            gen.lfo_rate_source   = static_cast<Synth::ModSource>(1 + ((layer + 2 * cell + 1) % 6));
            gen.lfo_rate_scale_ms = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(cell);
        }
    }
    for (uint32_t cell = 0; cell < 5; cell++) {
        const Synth::ModTarget target  = Synth::graph_projected_target(cell);
        Synth::InputRouting&   routing = instrument->routing[target];
        routing.base_value             = target == Synth::mod_volume    ? 0.75f
                                         : target == Synth::mod_pitch   ? 0.0f
                                         : target == Synth::mod_panning ? 0.5f
                                                                        : 2000.0f;
        routing.num_inputs             = 2;
        routing.inputs[0].source       = Synth::ModSource::velocity;
        routing.inputs[0].op           = Synth::SourceOp::add;
        routing.inputs[0].scale        = 0.5f;
        routing.inputs[1].source       = Synth::ModSource::mod_wheel;
        routing.inputs[1].op           = Synth::SourceOp::multiply;
        routing.inputs[1].scale        = -0.25f;
    }
    instrument->routing[Synth::mod_duty0].base_value    = 0.25f;
    instrument->routing[Synth::mod_duty1].base_value    = 0.5f;
    instrument->routing[Synth::mod_osc_mix].base_value  = 0.75f;
    instrument->routing[Synth::mod_fm_index].base_value = 0.5f * 6.2831853f;
    instrument->note_skew_semitones                     = 0.25f;
    instrument->layer_skew_semitones                    = 0.5f;

    uint32_t count = 0;
    for (uint32_t index = 0; index < Synth::graph_canonical_node_count; index++) {
        Synth::InstrumentGraphLayout& r = records[count++];
        r                               = {};
        r.kind                          = Synth::instrument_graph_layout_canonical;
        r.canonical_index               = static_cast<uint8_t>(index);
        r.x                             = -100.0f + static_cast<float>(index);
        r.y                             = 12.5f * static_cast<float>(index);
        r.width_override                = static_cast<float>(index);
        r.height_override               = 0.0f;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        for (uint32_t cell = 0; cell < 5; cell++) {
            const Synth::ModTarget target = Synth::graph_projected_target(cell);
            const Synth::LayerGen& gen    = instrument->layers[layer].gen[target];
            const uint32_t         seed   = layer * 5u + cell;

            Synth::InstrumentGraphLayout& env = records[count++];
            env                               = {};
            env.kind                          = Synth::instrument_graph_layout_envelope;
            env.layer                         = static_cast<uint8_t>(layer);
            env.target                        = static_cast<uint8_t>(target);
            env.x                             = 1000.0f + static_cast<float>(seed);
            env.y                             = -250.0f + static_cast<float>(seed);
            env.width_override                = 128.0f;
            env.height_override               = 64.0f;

            Synth::InstrumentGraphLayout& lfo = records[count++];
            lfo                               = {};
            lfo.kind                          = Synth::instrument_graph_layout_lfo;
            lfo.layer                         = static_cast<uint8_t>(layer);
            lfo.target                        = static_cast<uint8_t>(target);
            lfo.depth_source                  = static_cast<uint8_t>(gen.lfo_depth_source);
            lfo.rate_source                   = static_cast<uint8_t>(gen.lfo_rate_source);
            lfo.x                             = -750.0f - static_cast<float>(seed);
            lfo.y                             = 500.0f + static_cast<float>(seed);
            lfo.width_override                = 0.0f;
            lfo.height_override               = 0.0f;

            Synth::InstrumentGraphLayout& param = records[count++];
            param                               = {};
            param.kind                          = Synth::instrument_graph_layout_parameter;
            param.target                        = static_cast<uint8_t>(target);
            param.parameter_ordinal             = static_cast<uint8_t>(layer);
            param.x                             = 250.0f * static_cast<float>(layer) + static_cast<float>(cell);
            param.y                             = 3.25f * static_cast<float>(seed);
            param.width_override                = 240.0f;
            param.height_override               = 120.0f;
        }
    }
    TEST(count == max_portable_records);
    return count;
}

// First occurrence of `needle` in [from,to), or nullptr.
static const char* find_in_range(const char* from, const char* to, const char* needle)
{
    const size_t n = strlen(needle);
    for (const char* p = from; p + n <= to; p++) {
        if (strncmp(p, needle, n) == 0) {
            return p;
        }
    }
    return nullptr;
}

// This source check supports a direct negated helper guard with an immediate
// return on failure, optionally preceded by the exact refusal notification.
static const char* skip_source_space(const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static bool zone_paste_body_has_failure_guard(const char* body, const char* end)
{
    const char* const helper  = find_in_range(body, end, "replace_zone_instrument_candidate");
    const char* const undo    = find_in_range(body, end, "undo_group_reset");
    const char* const commit  = find_in_range(body, end, "commit_candidate");
    const char* const publish = find_in_range(body, end, "push_bank_update");
    if (! helper || (! undo && ! commit)) {
        return false;
    }
    const char*       first_state = end;
    const char* const states[]    = { undo, commit, publish };
    for (const char* state : states) {
        if (state && state < first_state) {
            first_state = state;
        }
    }
    const char* guard = body;
    while ((guard = find_in_range(guard, helper, "if")) != nullptr) {
        const char* p = skip_source_space(guard + 2, helper);
        if (p < helper && *p == '(') {
            p = skip_source_space(p + 1, helper);
            if (p < helper && *p == '!') {
                p = skip_source_space(p + 1, helper);
                if (helper - p >= 10 && strncmp(p, "Sculptor::", 10) == 0) {
                    p += 10;
                }
                if (p == helper) {
                    break;
                }
            }
        }
        guard += 2;
    }
    if (! guard || helper >= first_state) {
        return false;
    }
    const char* p = skip_source_space(helper + strlen("replace_zone_instrument_candidate"), first_state);
    if (p >= first_state || *p != '(') {
        return false;
    }
    int depth = 1;
    while (++p < first_state && depth != 0) {
        if (*p == '(') {
            ++depth;
        }
        if (*p == ')') {
            --depth;
        }
    }
    p = skip_source_space(p, first_state);
    if (depth != 0 || p >= first_state || *p != ')') {
        return false;
    }
    p                 = skip_source_space(p + 1, first_state);
    const bool braced = p < first_state && *p == '{';
    if (braced) {
        p = skip_source_space(p + 1, first_state);
    }
    constexpr char refusal_notification[] =
        "Sculptor::notify_error(\"Synth: cannot paste: the instrument replacement was refused\");";
    if (first_state - p >= static_cast<ptrdiff_t>(sizeof(refusal_notification) - 1) &&
        strncmp(p, refusal_notification, sizeof(refusal_notification) - 1) == 0) {
        p = skip_source_space(p + sizeof(refusal_notification) - 1, first_state);
    }
    if (first_state - p < 6 || strncmp(p, "return", 6) != 0) {
        return false;
    }
    p = skip_source_space(p + 6, first_state);
    if (p >= first_state || *p != ';') {
        return false;
    }
    p = skip_source_space(p + 1, first_state);
    return ! braced || (p < first_state && *p == '}');
}

static void check_zone_paste_source_guard_self_tests()
{
    const char* const correct  = "if (!Sculptor::replace_zone_instrument_candidate(a, f(b))) { return; } "
                                 "undo_group_reset(); commit_candidate(); push_bank_update();";
    const char* const unbraced = "if (! replace_zone_instrument_candidate(a)) return; commit_candidate();";
    const char* const inverted = "if (replace_zone_instrument_candidate(a)) return; "
                                 "undo_group_reset(); commit_candidate(); push_bank_update();";
    const char* const notified =
        "if (!Sculptor::replace_zone_instrument_candidate(a)) { "
        "Sculptor::notify_error(\"Synth: cannot paste: the instrument replacement was refused\"); return; } commit_candidate();";
    const char* const unsafe = "if (!Sculptor::replace_zone_instrument_candidate(a)) { commit_candidate(); return; }";
    TEST(zone_paste_body_has_failure_guard(notified, notified + strlen(notified)));
    TEST(! zone_paste_body_has_failure_guard(unsafe, unsafe + strlen(unsafe)));
    TEST(zone_paste_body_has_failure_guard(correct, correct + strlen(correct)));
    TEST(zone_paste_body_has_failure_guard(unbraced, unbraced + strlen(unbraced)));
    TEST(! zone_paste_body_has_failure_guard(inverted, inverted + strlen(inverted)));
}

// Source-inspection evidence, not an executable GUI test: the unit target does
// not link the GUI translation unit. Searches stay inside the command's balanced
// body; braces and parentheses inside literals or comments are unsupported.
static void check_zone_paste_source_ordering()
{
    check_zone_paste_source_guard_self_tests();
    static char src[256 * 1024];
    FILE* const file = fopen("sculptor/sculptor_instr_edit.cpp", "rb");
    TEST(file != nullptr);
    if (! file) {
        return;
    }
    const size_t got = fread(src, 1, sizeof(src) - 1, file);
    fclose(file);
    TEST(got > 0);
    src[got]               = 0;
    const char* const decl = strstr(src, "Sculptor::SynthEditor::do_zone_paste_instrument");
    TEST(decl != nullptr);
    if (! decl) {
        return;
    }
    const char* body_open = decl;
    while (*body_open != 0 && *body_open != '{') {
        body_open++;
    }
    TEST(*body_open == '{');
    if (*body_open != '{') {
        return;
    }
    const char* body_close = nullptr;
    int         depth      = 0;
    for (const char* p = body_open; *p != 0; p++) {
        if (*p == '{') {
            depth++;
        }
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                body_close = p;
                break;
            }
        }
    }
    TEST(body_close != nullptr);
    if (! body_close) {
        return;
    }
    TEST(zone_paste_body_has_failure_guard(body_open, body_close));
}

// Index of the first mapped editor record of `kind` keying `index`, or -1.
static int32_t find_mapped(const Synth::GraphNodeLayout* records, uint32_t count, uint32_t kind, uint32_t index)
{
    for (uint32_t i = 0; i < count; i++) {
        if (records[i].kind == kind && records[i].index == index) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Assembles one instrument document. The document's closing brace is emitted
// here and nowhere else, so a caller-supplied record list can never duplicate
// it, and every assembled fixture is parsed before a test reads it: a baseline
// that is not well-formed JSON cannot silently pass.
static uint32_t build_layout_doc(char* dest, uint32_t size, const char* head, const char* records)
{
    const int written = snprintf(dest, size, "%s\"graph_layout\":[%s]}", head, records);
    TEST(written > 0 && static_cast<uint32_t>(written) < size);
    const int32_t fixture_tokens = written > 0 ? count_clipboard_tokens(dest, static_cast<uint32_t>(written)) : -1;
    TEST(fixture_tokens > 0);
    return written > 0 ? static_cast<uint32_t>(written) : 0;
}

int main()
{
    // Debug builds log skipped unknown JSON fields to stdout; the suite must stay
    // silent unless failing, and failures report on stderr, so stdout is discarded.
#ifdef _WIN32
    freopen("NUL", "w", stdout);
#else
    freopen("/dev/null", "w", stdout);
#endif

    // A4 = 440 Hz
    TEST(approx(Synth::note_to_frequency(69, 0.0f, 1), 440.0f, 0.01f));
    // One octave up = 880 Hz
    TEST(approx(Synth::note_to_frequency(81, 0.0f, 1), 880.0f, 0.02f));
    // One semitone up via pitch offset ~ 466.16 Hz
    TEST(approx(Synth::note_to_frequency(69, 1.0f, 1), 466.16f, 0.05f));
    // freq_mult doubles the frequency
    TEST(approx(Synth::note_to_frequency(69, 0.0f, 2), 880.0f, 0.02f));

    // sine LFO: contribution stays within [min, min+delta] and is periodic
    const Synth::LFODescriptor sine_lfo = { Synth::WaveType::sine_wave,
                                            0,
                                            1000,
                                            -1.0f,
                                            2.0f }; // 1s period, range [-1,1]
    float                      min_seen = 1e9f;
    float                      max_seen = -1e9f;
    for (uint32_t tick = 0; tick < 1000; tick++) {
        const float lfo_value = Synth::eval_lfo(sine_lfo, tick, 256, 44100);
        TEST(lfo_value >= -1.0001f && lfo_value <= 1.0001f);
        if (lfo_value < min_seen) {
            min_seen = lfo_value;
        }
        if (lfo_value > max_seen) {
            max_seen = lfo_value;
        }
    }
    TEST(approx(min_seen, -1.0f, 0.05f));
    TEST(approx(max_seen, 1.0f, 0.05f));
    // periodicity: with these params one period is exactly 4 ticks, so values
    // repeat every 4 ticks
    {
        const Synth::LFODescriptor periodic_lfo     = { Synth::WaveType::sine_wave, 0, 1024, -1.0f, 2.0f };
        const uint32_t             ticks_per_period = 4;
        TEST(approx(Synth::eval_lfo(periodic_lfo, 0, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, ticks_per_period, 256, 1000),
                    0.001f));
        TEST(approx(Synth::eval_lfo(periodic_lfo, 1, 256, 1000),
                    Synth::eval_lfo(periodic_lfo, 1 + ticks_per_period, 256, 1000),
                    0.001f));
    }

    // sawtooth (triangle, duty=0x7F) LFO stays within [0,1] and reaches both ends
    {
        const Synth::LFODescriptor saw_lfo = { Synth::WaveType::sawtooth_wave, 0x7F, 1000, 0.0f, 1.0f };
        float                      saw_min = 1e9f;
        float                      saw_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float saw_value = Synth::eval_lfo(saw_lfo, tick, 256, 44100);
            TEST(saw_value >= -0.0001f && saw_value <= 1.0001f);
            if (saw_value < saw_min) {
                saw_min = saw_value;
            }
            if (saw_value > saw_max) {
                saw_max = saw_value;
            }
        }
        TEST(approx(saw_min, 0.0f, 0.05f));
        TEST(approx(saw_max, 1.0f, 0.05f));
    }

    // 4-point ADSR-like envelope: rise to peak (tick2), decay to mid (tick4,
    // the sustain point), release to zero (tick6).  value 0xFFFF maps to 1.0.
    {
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 4;
        env.sustain_first_point       = 2;
        env.sustain_last_point        = 2;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f / 65535.0f;
        env.points[0]                 = { 0, 0 };      // start at min
        env.points[1]                 = { 2, 0xFFFF }; // attack peak
        env.points[2]                 = { 4, 0x8000 }; // decay to ~mid (sustain)
        env.points[3]                 = { 6, 0 };      // release to min

        // Sustained: run 20 ticks holding sustain.  Should reach ~1.0 peak,
        // then settle and HOLD at the sustain value (~0.5).
        Synth::EnvelopeState state       = { 0, 0 };
        float                first_value = Synth::eval_envelope(env, &state, true);
        TEST(approx(first_value, 0.0f, 0.01f));
        float peak_value = first_value;
        float last_value = first_value;
        for (uint32_t tick = 1; tick < 20; tick++) {
            last_value = Synth::eval_envelope(env, &state, true);
            if (last_value > peak_value) {
                peak_value = last_value;
            }
        }
        TEST(approx(peak_value, 1.0f, 0.02f));              // attack peak reached
        TEST(approx(last_value, 0x8000 / 65535.0f, 0.01f)); // held at sustain value

        // Release: stop sustaining, run more ticks; value must reach ~0.
        float release_value = last_value;
        for (uint32_t tick = 0; tick < 10; tick++) {
            release_value = Synth::eval_envelope(env, &state, false);
        }
        TEST(approx(release_value, 0.0f, 0.01f));

        // eval_envelope_at peeks at a state without advancing it: same value the advancing
        // eval returns at that state, and the state is left untouched.
        Synth::EnvelopeState peek_state = { 1, 0 }; // mid-attack between points 0 and 1
        const float          peeked     = Synth::eval_envelope_at(env, peek_state);
        TEST(peek_state.tick == 1 && peek_state.point == 0);
        TEST(approx(peeked, 0.5f, 0.01f));
        const float advanced = Synth::eval_envelope(env, &peek_state, true);
        TEST(approx(advanced, peeked, 0.0f));
        TEST(peek_state.tick == 2 && peek_state.point == 1);
    }

    // pitch bend -> semitones: centered 14-bit value scaled by the bend range.
    // center (0) -> no bend; full deflection -> +/- range; half -> ~half range.
    TEST(approx(Synth::pitch_bend_to_semitones(0, 2.0f), 0.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 2.0f), 2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(-8192, 2.0f), -2.0f, 0.001f));
    TEST(approx(Synth::pitch_bend_to_semitones(4096, 2.0f), 1.0f, 0.001f));
    // range scales linearly
    TEST(approx(Synth::pitch_bend_to_semitones(8191, 12.0f), 12.0f, 0.01f));

    TEST(Synth::get_effect_param_floats(Synth::EffectType::distortion) == 2);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::delay) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::chorus) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::reverb) == 3);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::compressor) == 5);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::fir) == 2);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::distortion) == 0);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::delay) == 88201);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::chorus) == 4412);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::reverb) == 25191);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::compressor) == 1);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::fir) == 3074);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::none) == 0);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::none) == 0);

    // Ring buffer accounting on free-running frame counters: available, free
    // space, and the contiguous run before the physical buffer wraps.
    {
        const uint32_t capacity = 1024;

        TEST(Synth::get_ringbuf_data_size(0, 0) == 0);
        TEST(Synth::get_ringbuf_avail_space(0, 0, capacity) == capacity);

        TEST(Synth::get_ringbuf_data_size(256, 0) == 256);
        TEST(Synth::get_ringbuf_avail_space(256, 0, capacity) == capacity - 256);

        TEST(Synth::get_ringbuf_data_size(300, 44) == 256);
        TEST(Synth::get_ringbuf_avail_space(300, 44, capacity) == capacity - 256);

        TEST(Synth::get_ringbuf_data_size(capacity, 0) == capacity);
        TEST(Synth::get_ringbuf_avail_space(capacity, 0, capacity) == 0);

        TEST(Synth::get_ringbuf_contig_tail(0, capacity) == capacity);                // offset 0: whole buffer
        TEST(Synth::get_ringbuf_contig_tail(1000, capacity) == 24);                   // mid-buffer: 1024 - 1000
        TEST(Synth::get_ringbuf_contig_tail(2048, capacity) == capacity);             // exact multiple wraps to 0
        TEST(Synth::get_ringbuf_contig_tail(capacity + 1, capacity) == capacity - 1); // offset 1 after a wrap
    }

    // propagate_parameters: one-step-delay vs zero-lag external input.  Chain X(external) -> B -> A.
    // An external source is read at its current value (zero lag); a plain->plain hop lags exactly
    // one step.  Parameter 0 is the reserved sentinel.
    {
        Synth::ParamDescriptor descs[4] = {};
        // descs[0] (sentinel) and descs[3] (X, externally driven) stay kind external.
        descs[2].kind              = Synth::ParamKind::plain; // B reads X additively
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0]  = { 3, 1.0f, Synth::SourceOp::add };
        descs[1].kind              = Synth::ParamKind::plain; // A reads B additively
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::add };

        Synth::Parameter params[4] = {};
        params[3].value            = 5.0f; // drive the external input

        Synth::propagate_parameters(params, descs, 4); // step 1
        TEST(approx(params[2].value, 5.0f, 0.001f));   // B picked up the input with zero lag
        TEST(approx(params[1].value, 0.0f, 0.001f));   // A still sees B's previous (0): one-step delay

        Synth::propagate_parameters(params, descs, 4); // step 2
        TEST(approx(params[2].value, 5.0f, 0.001f));
        TEST(approx(params[1].value, 5.0f, 0.001f)); // A now sees B, one step later
    }

    // propagate_parameters: a feedback cycle (A <-> B, |amount| < 1) stays finite and
    // bounded; the one-step delay makes it a well-defined iteration, never a deadlock or NaN.
    {
        Synth::ParamDescriptor descs[3] = {};
        // descs[0] (sentinel) stays kind external.
        descs[1].kind              = Synth::ParamKind::plain; // A = 1 + 0.5 * B.prev
        descs[1].plain.base_value  = 1.0f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 0.5f, Synth::SourceOp::add };
        descs[2].kind              = Synth::ParamKind::plain; // B = 1 + 0.5 * A.prev
        descs[2].plain.base_value  = 1.0f;
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0]  = { 1, 0.5f, Synth::SourceOp::add };

        Synth::Parameter params[3] = {};
        for (uint32_t step = 0; step < 1000; step++) {
            Synth::propagate_parameters(params, descs, 3);
        }
        TEST(params[1].value == params[1].value); // not NaN
        TEST(params[2].value == params[2].value);
        TEST(params[1].value < 100.0f && params[1].value > -100.0f); // bounded (converges to 2)
        TEST(approx(params[1].value, 2.0f, 0.01f));
    }

    // propagate_parameters: envelope-kind params are exempt from the prev_value
    // snapshot.  The envelope step writes the pair (value at the step's starting
    // tick, prev_value one tick ahead) itself, so the destination reads the
    // end-of-step value without the snapshot clobbering it.
    {
        Synth::ParamDescriptor descs[3] = {};
        descs[1].kind                   = Synth::ParamKind::plain; // dest = 1.0 + env.prev
        descs[1].plain.base_value       = 1.0f;
        descs[1].plain.num_sources      = 1;
        descs[1].plain.sources[0]       = { 2, 1.0f, Synth::SourceOp::add };
        descs[2].kind                   = Synth::ParamKind::envelope;

        Synth::Parameter params[3] = {};
        params[2].value            = 1.0f; // envelope value at the step's starting tick
        params[2].prev_value       = 2.0f; // envelope value one tick ahead (peek)

        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[1].value, 3.0f, 0.001f));      // dest folded the end-of-step value
        TEST(approx(params[2].prev_value, 2.0f, 0.001f)); // the pair survived the snapshot
    }

    // propagate_parameters: a multiply source scales the base (e.g. velocity into volume).
    {
        Synth::ParamDescriptor descs[3] = {};
        // descs[0] (sentinel) and descs[2] (velocity input) stay kind external.
        descs[1].kind              = Synth::ParamKind::plain; // volume base, scaled by velocity
        descs[1].plain.base_value  = 0.8f;
        descs[1].plain.num_sources = 1;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::multiply };

        Synth::Parameter params[3] = {};
        params[2].value            = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 3);
        TEST(approx(params[1].value, 0.4f, 0.001f)); // 0.8 * 0.5 (input read with zero lag)
    }

    // propagate_parameters: sources fold left-to-right over the running accumulator, so a
    // multiply applies to base plus prior adds, not to base alone.  This is the production
    // volume path: (base + envelope) * tremolo * velocity.
    {
        Synth::ParamDescriptor descs[5] = {};
        // descs[0] sentinel, descs[2] envelope, descs[3] tremolo, descs[4] velocity stay kind
        // external; their values are set directly below.
        descs[1].kind              = Synth::ParamKind::plain;
        descs[1].plain.num_sources = 3;
        descs[1].plain.sources[0]  = { 2, 1.0f, Synth::SourceOp::add };
        descs[1].plain.sources[1]  = { 3, 1.0f, Synth::SourceOp::multiply };
        descs[1].plain.sources[2]  = { 4, 1.0f, Synth::SourceOp::multiply };

        Synth::Parameter params[5] = {};
        params[2].value            = 2.0f; // envelope 2.0
        params[3].value            = 0.5f; // tremolo gain 0.5
        params[4].value            = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 5);
        TEST(approx(params[1].value, 0.5f, 0.001f)); // (0 + 2.0) * 0.5 * 0.5, not 0 + 2.0 + ...
    }

    // configure_plain: effect-param shape -- base folded with an LFO leaf (add) and a
    // channel input leaf (multiply).  This is the contract the effect-param expander relies on:
    // a modulated effect param's value is propagate_parameters' result for the assembled dest.
    // The LFO leaf value is set directly here (the host pre-pass that fills it is not under test).
    {
        constexpr uint16_t dest_id  = 1;
        constexpr uint16_t lfo_id   = 2;
        constexpr uint16_t input_id = 3;

        Synth::ParamDescriptor descs[4] = {};
        // descs[0] (sentinel) and descs[input_id] (channel input, e.g. mod wheel) stay kind external.

        Synth::LFODescriptor test_lfo = {};
        test_lfo.wave                 = Synth::WaveType::sine_wave;
        test_lfo.period_ms            = 50;
        Synth::configure_lfo(&descs[lfo_id], test_lfo, Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        const Synth::SourceParam inputs[1] = { { input_id, 1.0f, Synth::SourceOp::multiply } };
        Synth::configure_plain(&descs[dest_id], 0.1f, 0, lfo_id, Synth::SourceOp::add, inputs, 1);

        // configure_lfo made the LFO node; configure_plain made the dest a plain node.
        TEST(descs[lfo_id].kind == Synth::ParamKind::lfo);
        TEST(descs[lfo_id].lfo.lfo.wave == Synth::WaveType::sine_wave);
        TEST(approx(descs[lfo_id].lfo.lfo.period_ms, 50.0f, 0.001f)); // captured copy
        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2);

        Synth::Parameter params[4] = {};
        params[lfo_id].value       = 0.2f; // pretend the LFO produced 0.2
        params[input_id].value     = 0.5f; // channel input 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 0.15f, 0.001f)); // (0.1 + 0.2) * 0.5
    }

    // configure_plain: no LFO, an envelope source plus a multiply input -- the voice volume
    // shape (base + envelope) * velocity.  lfo_desc_id 0 means no LFO leaf and no LFO source.
    {
        constexpr uint16_t dest_id = 1;
        constexpr uint16_t env_id  = 2;
        constexpr uint16_t vel_id  = 3;

        Synth::ParamDescriptor descs[4] = {};
        // descs[0] sentinel, descs[env_id] envelope value, descs[vel_id] velocity: kind external,
        // their values set directly below.

        const Synth::SourceParam inputs[1] = { { vel_id, 1.0f, Synth::SourceOp::multiply } };
        Synth::configure_plain(&descs[dest_id], 0.0f, env_id, 0, Synth::SourceOp::add, inputs, 1);

        TEST(descs[dest_id].kind == Synth::ParamKind::plain);
        TEST(descs[dest_id].plain.num_sources == 2); // envelope source + velocity source, no LFO source

        Synth::Parameter params[4] = {};
        params[env_id].value       = 2.0f; // envelope 2.0
        params[vel_id].value       = 0.5f; // velocity 0.5
        Synth::propagate_parameters(params, descs, 4);
        TEST(approx(params[dest_id].value, 1.0f, 0.001f)); // (0 + 2.0) * 0.5
    }

    // eval_lfo_mod: add op swings bipolar within [-depth, depth] and reaches both ends;
    // depth 0 is the neutral contribution 0.  4 ticks = one period here.
    {
        const Synth::LFODescriptor lfo     = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                depth   = 0.5f;
        float                      add_min = 1e9f;
        float                      add_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float value = Synth::eval_lfo_mod(lfo, tick, 256, 1024, 1000, depth, Synth::SourceOp::add);
            TEST(value >= -depth - 0.001f && value <= depth + 0.001f);
            if (value < add_min) {
                add_min = value;
            }
            if (value > add_max) {
                add_max = value;
            }
        }
        TEST(approx(add_min, -depth, 0.02f));
        TEST(approx(add_max, depth, 0.02f));
        // depth 0 -> neutral 0 at every tick
        TEST(approx(Synth::eval_lfo_mod(lfo, 7, 256, 1024, 1000, 0.0f, Synth::SourceOp::add), 0.0f, 0.001f));
    }

    // eval_lfo_mod: multiply op is an attenuation factor within [1-depth, 1];
    // depth 0 is the neutral factor 1.
    {
        const Synth::LFODescriptor lfo     = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                depth   = 0.5f;
        float                      mul_min = 1e9f;
        float                      mul_max = -1e9f;
        for (uint32_t tick = 0; tick < 1000; tick++) {
            const float factor = Synth::eval_lfo_mod(lfo, tick, 256, 1024, 1000, depth, Synth::SourceOp::multiply);
            TEST(factor >= 1.0f - depth - 0.001f && factor <= 1.0f + 0.001f);
            if (factor < mul_min) {
                mul_min = factor;
            }
            if (factor > mul_max) {
                mul_max = factor;
            }
        }
        TEST(approx(mul_min, 1.0f - depth, 0.02f));
        TEST(approx(mul_max, 1.0f, 0.02f));
        TEST(approx(Synth::eval_lfo_mod(lfo, 3, 256, 1024, 1000, 0.0f, Synth::SourceOp::multiply), 1.0f, 0.001f));
    }

    // eval_lfo_mod: a sourced rate (period_ms override) actually changes the rate.
    // Halving the period doubles the cycles, so a tick that is a quarter-period at
    // the long period becomes a half-period at the short one -> different phase.
    {
        const Synth::LFODescriptor lfo  = { Synth::WaveType::sine_wave, 0, 1000, 0.0f, 1.0f };
        const float                slow = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1024, 0.5f, Synth::SourceOp::add);
        const float                fast = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 512, 0.5f, Synth::SourceOp::add);
        TEST(! approx(slow, fast, 0.05f));
        // period_ms 0 falls back to the descriptor's own period_ms.
        const float defaulted     = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 0, 0.5f, Synth::SourceOp::add);
        const float explicit_same = Synth::eval_lfo_mod(lfo, 1, 256, 1024, 1000, 0.5f, Synth::SourceOp::add);
        TEST(approx(defaulted, explicit_same, 0.001f));
    }

    // random_pitch_skew: a nonzero amount stays within [-amount, amount] and spans most of it;
    // amount 0 is exactly 0 (deterministic, generator unadvanced).
    {
        RNG skew_rng;
        skew_rng.init(0x5eed1234u);
        TEST(Synth::random_pitch_skew(&skew_rng, 0.0f) == 0.0f);

        const float amount   = 0.25f;
        float       skew_min = 1e9f;
        float       skew_max = -1e9f;
        for (uint32_t draw = 0; draw < 100000; draw++) {
            const float skew = Synth::random_pitch_skew(&skew_rng, amount);
            TEST(skew >= -amount && skew <= amount);
            if (skew < skew_min) {
                skew_min = skew;
            }
            if (skew > skew_max) {
                skew_max = skew;
            }
        }
        TEST(skew_min < -amount * 0.9f);
        TEST(skew_max > amount * 0.9f);
    }

    // Keyboard split routing: route_instrument maps a note to an instrument via an ordered
    // table; stored starts are first-note + 1 (0 = unused slot), so an all-zero table resolves to instrument 0.
    {
        const uint32_t count = Synth::max_instr_per_channel;

        Synth::Zone empty[Synth::max_instr_per_channel] = {};
        TEST(Synth::route_instrument(empty, count, 0) == 0);
        TEST(Synth::route_instrument(empty, count, 60) == 0);
        TEST(Synth::route_instrument(empty, count, 127) == 0);

        // Notes 0..59 -> instrument 0, notes 60.. -> instrument 1.  Slot 0 stores 1
        // (zone 0 starts at note 0); 0 is the end-of-table sentinel.
        Synth::Zone split[Synth::max_instr_per_channel] = {};
        split[0]                                        = { 1, 0 };
        split[1]                                        = { 61, 1 };
        ;
        TEST(Synth::route_instrument(split, count, 0) == 0);
        TEST(Synth::route_instrument(split, count, 59) == 0);
        TEST(Synth::route_instrument(split, count, 60) == 1);
        TEST(Synth::route_instrument(split, count, 127) == 1);

        Synth::Zone three[Synth::max_instr_per_channel] = {};
        three[0]                                        = { 1, 2 };
        three[1]                                        = { 49, 4 };
        ;
        three[2] = { 73, 3 };
        ;
        TEST(Synth::route_instrument(three, count, 47) == 2);
        TEST(Synth::route_instrument(three, count, 48) == 4);
        TEST(Synth::route_instrument(three, count, 71) == 4);
        TEST(Synth::route_instrument(three, count, 72) == 3);

        // A full table with no sentinel still resolves the top range.
        Synth::Zone full[Synth::max_instr_per_channel] = {};
        for (uint32_t idx = 0; idx < count; idx++) {
            full[idx] = { static_cast<uint8_t>(idx + 1), static_cast<uint8_t>(idx) };
        }
        TEST(Synth::route_instrument(full, count, 127) == count - 1);
    }

    // --- Phase 1 data foundation: generic pools, container, defragment + remap ---

    // alloc-distinct: three allocations from an empty pool give distinct, in-range slots.
    {
        Pool<Synth::EnvelopeDescriptor, Synth::max_envelopes> env_pool = {};
        const uint32_t                                        slot0    = env_pool.allocate();
        const uint32_t                                        slot1    = env_pool.allocate();
        const uint32_t                                        slot2    = env_pool.allocate();
        TEST(slot0 != pool_no_slot && slot1 != pool_no_slot && slot2 != pool_no_slot);
        TEST(slot0 != slot1 && slot1 != slot2 && slot0 != slot2);
        TEST(slot0 < Synth::max_envelopes && slot1 < Synth::max_envelopes && slot2 < Synth::max_envelopes);
        TEST(env_pool.num_allocated == 3);
    }

    // reuse-after-free: freeing the middle slot lets the next allocate reuse it.
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = {};
        const uint32_t                              first    = lfo_pool.allocate();
        const uint32_t                              middle   = lfo_pool.allocate();
        const uint32_t                              last     = lfo_pool.allocate();
        TEST(first != pool_no_slot && last != pool_no_slot);
        lfo_pool.free(middle);
        TEST(lfo_pool.num_allocated == 2);
        const uint32_t reused = lfo_pool.allocate();
        TEST(reused == middle);
        TEST(lfo_pool.num_allocated == 3);
    }

    // full-pool: allocate up to capacity, then allocate fails with pool_no_slot (no OOB).
    {
        Pool<Synth::LFODescriptor, Synth::max_lfos> lfo_pool = {};
        for (uint32_t idx = 0; idx < Synth::max_lfos; idx++) {
            TEST(lfo_pool.allocate() != pool_no_slot);
        }
        TEST(lfo_pool.allocate() == pool_no_slot);
        TEST(lfo_pool.num_allocated == Synth::max_lfos);
    }

    // defrag-remap (envelopes): a fragmented pool [used,free,used,free,used] compacts to the
    // front preserving order, and an instrument's 1-based envelope reference is rewritten.
    {
        static Synth::InstrumentBank bank = {};
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.envelopes.allocate() == idx);
        }
        bank.envelopes.free(1);
        bank.envelopes.free(3);

        // Tag slot 4's data so we can find where it lands, and reference it from a layer
        // (desc_id is 1-based: slot 4 -> id 5).
        bank.envelopes.entries[4].num_points = 42;
        const uint32_t instr                 = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layer_count                                       = 1;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id = 5;

        uint32_t env_map[Synth::max_envelopes];
        bank.envelopes.defragment(env_map);
        TEST(bank.envelopes.num_allocated == 3);
        TEST(env_map[0] == 0); // old 0,2,4 -> new 0,1,2 in order
        TEST(env_map[2] == 1);
        TEST(env_map[4] == 2);
        TEST(env_map[1] == pool_no_slot);
        TEST(env_map[3] == pool_no_slot);
        TEST(bank.envelopes.entries[2].num_points == 42); // slot 4's data moved to slot 2
    }

    // defrag-remap (LFOs): same mechanism as envelopes -- a layer's 1-based lfo_desc_id is
    // rewritten to the referenced LFO entry's new slot.
    {
        static Synth::InstrumentBank bank = {};
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.lfos.allocate() == idx);
        }
        bank.lfos.free(1);
        bank.lfos.free(3);
        bank.lfos.entries[4].period_ms = 333;

        const uint32_t instr = bank.instruments.allocate();
        TEST(instr != pool_no_slot);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id = 5; // slot 4 -> id 5

        uint32_t lfo_map[Synth::max_lfos];
        bank.lfos.defragment(lfo_map);
        TEST(lfo_map[4] == 2);
        TEST(bank.lfos.entries[2].period_ms == 333);
    }

    // defrag-remap (instruments): an instrument pool compacts and a channel split-table entry
    // that references an instrument (0-based) is rewritten to the new slot.
    {
        static Synth::InstrumentBank bank = {};
        for (uint32_t idx = 0; idx < 5; idx++) {
            TEST(bank.instruments.allocate() == idx);
        }
        bank.instruments.free(1);
        bank.instruments.free(3);
        bank.channel_zones[0][0] = { 1, 4 };  // note >= 1 -> instrument slot 4 (survives)
        bank.channel_zones[0][1] = { 65, 3 }; // references deleted instrument slot 3

        uint32_t instr_map[Synth::max_instruments];
        bank.instruments.defragment(instr_map);
        TEST(instr_map[4] == 2);
    }

    // snapshot-roundtrip: a byte copy of the container, then a byte restore after mutation,
    // reproduces identical state -- the property the undo stack and save/load rely on.
    {
        static Synth::InstrumentBank bank           = {};
        const uint32_t               instr          = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.drum_track_channel                     = 9;
        const uint32_t env                          = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points      = 7;

        static Synth::InstrumentBank snapshot;
        memcpy(&snapshot, &bank, sizeof(bank));

        bank.instruments.entries[instr].layer_count = 99;
        bank.instruments.free(instr);
        bank.drum_track_channel = 0;

        memcpy(&bank, &snapshot, sizeof(bank));
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.instruments.entries[instr].layer_count == 3);
        TEST(bank.drum_track_channel == 9);
        TEST(bank.envelopes.entries[env].num_points == 7);
        TEST(memcmp(&bank, &snapshot, sizeof(bank)) == 0);
    }

    // instrument bank codec round-trips losslessly, with cross-references intact
    {
        static Synth::InstrumentBank bank  = {};
        const uint32_t               env   = bank.envelopes.allocate();
        const uint32_t               lfo   = bank.lfos.allocate();
        const uint32_t               instr = bank.instruments.allocate();

        // Valid descriptor content: the decoder now fully validates banks.
        bank.envelopes.entries[env].num_points         = 2;
        bank.envelopes.entries[env].points[1].position = 100;
        bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
        bank.lfos.entries[lfo].period_ms               = 50;

        bank.instruments.entries[instr].layer_count = 2;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id =
            static_cast<uint16_t>(env + 1);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id = static_cast<uint16_t>(lfo + 1);
        bank.channel_zones[0][0] = { 1, static_cast<uint8_t>(instr) };
        bank.drum_track_channel  = 9;

        uint8_t        image[Synth::instrument_bank_image_size<Synth::InstrumentBank>];
        const uint32_t written = Synth::encode_instrument_bank(&bank, image, sizeof(image));
        TEST(written == Synth::instrument_bank_image_size<Synth::InstrumentBank>);

        static Synth::InstrumentBank restored = {};
        TEST(Synth::decode_instrument_bank(image, written, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);

        // A buffer too small to hold the image fails cleanly, writing nothing.
        TEST(Synth::encode_instrument_bank(&bank, image, 4) == 0);

        // A corrupt marker is rejected.
        static uint8_t bad[Synth::instrument_bank_image_size<Synth::InstrumentBank>];
        memcpy(bad, image, sizeof(bad));
        bad[0] = static_cast<uint8_t>(bad[0] ^ 0xFFu);
        TEST(! Synth::decode_instrument_bank(bad, written, &restored));

        // A mismatched version is rejected (version is the two bytes after the 4-byte marker).
        memcpy(bad, image, sizeof(bad));
        bad[4] = static_cast<uint8_t>(bad[4] ^ 0xFFu);
        TEST(! Synth::decode_instrument_bank(bad, written, &restored));

        // A mismatched payload size is rejected (the four bytes after the version).
        memcpy(bad, image, sizeof(bad));
        bad[6] = static_cast<uint8_t>(bad[6] ^ 0xFFu);
        TEST(! Synth::decode_instrument_bank(bad, written, &restored));

        // A truncated image (header only) is rejected.
        TEST(! Synth::decode_instrument_bank(image, Synth::instrument_bank_header_size, &restored));
    }

    // instrument bank persists to and loads from a real file
    {
        static Synth::InstrumentBank bank           = {};
        bank.drum_track_channel                     = 7;
        const uint32_t instr                        = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.channel_zones[2][0]                    = { 65, static_cast<uint8_t>(instr) };

        const char* const path = "synth_bank_roundtrip.tmp";
        TEST(Synth::save_instrument_bank(path, &bank));

        static Synth::InstrumentBank restored = {};
        TEST(Synth::load_instrument_bank(path, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);
        remove(path);

        // Loading a nonexistent file fails cleanly.
        TEST(! Synth::load_instrument_bank("synth_bank_does_not_exist.tmp", &restored));
    }

    // MIDI file: format 0, single track, division 96.  A tempo meta (500000 us/quarter =
    // 120 BPM), a note_on at delta 0, and a note_off one quarter (delta 96) later.  At 44100 Hz
    // a quarter note is 0.5 s = 22050 samples, so the note_off lands at sample 22050.
    {
        static const uint8_t midi[] = {
            // MThd
            0x4D,
            0x54,
            0x68,
            0x64, // "MThd"
            0x00,
            0x00,
            0x00,
            0x06, // header length 6
            0x00,
            0x00, // format 0
            0x00,
            0x01, // ntrks 1
            0x00,
            0x60, // division 96
            // MTrk
            0x4D,
            0x54,
            0x72,
            0x6B, // "MTrk"
            0x00,
            0x00,
            0x00,
            0x13, // track length 19
            0x00,
            0xFF,
            0x51,
            0x03,
            0x07,
            0xA1,
            0x20, // delta 0, tempo 500000 us/quarter
            0x00,
            0x90,
            0x3C,
            0x64, // delta 0, note_on ch0 note 60 vel 100
            0x60,
            0x80,
            0x3C,
            0x40, // delta 96, note_off ch0 note 60 vel 64
            0x00,
            0xFF,
            0x2F,
            0x00, // delta 0, end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t   count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
        TEST(count == 2);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].channel == 0);
        TEST(events[0].note == 60);
        TEST(events[0].note_data == 100);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::note_off);
        TEST(events[1].channel == 0);
        TEST(events[1].note == 60);
        TEST(events[1].time == 22050);
    }

    // MIDI file: running status -- a second note_on reuses the prior 0x90 status byte (no
    // status byte, just data).  Default tempo (no FF51) is 120 BPM, division 96.
    {
        static const uint8_t midi[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x60, 0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x0B, // track length 11
            0x00, 0x90, 0x3C, 0x64,                                           // delta 0, note_on ch0 note 60 vel 100
            0x30, 0x3E, 0x64,       // delta 48, running status: note_on note 62 vel 100
            0x00, 0xFF, 0x2F, 0x00, // end of track
        };

        Synth::MidiEvent events[8];
        const uint32_t   count = Synth::parse_midi_file(midi, sizeof(midi), events, 8, 44100);
        TEST(count == 2);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].note == 60);
        TEST(events[0].time == 0);
        TEST(events[1].event == Synth::EvType::note_on);
        TEST(events[1].note == 62);
        TEST(events[1].time == 11025); // delta 48 ticks = half a quarter = 11025 samples
    }

    // MIDI file: malformed / truncated inputs return 0 without crashing (checked under ASAN).
    {
        Synth::MidiEvent events[8];
        // Empty buffer.
        TEST(Synth::parse_midi_file(nullptr, 0, events, 8, 44100) == 0);
        // Bad magic.
        static const uint8_t bad_magic[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        TEST(Synth::parse_midi_file(bad_magic, sizeof(bad_magic), events, 8, 44100) == 0);
        // Truncated valid header (track length claims more bytes than present).
        static const uint8_t truncated[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x60, 0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x40, // claims 64 bytes but track is cut off
            0x00, 0x90, 0x3C,                                                 // incomplete note_on
        };
        TEST(Synth::parse_midi_file(truncated, sizeof(truncated), events, 8, 44100) == 0);
        // SMPTE division (high bit set) is rejected.
        static const uint8_t smpte[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01,
            0xE8, 0x04, // negative (SMPTE) division
            0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x04, 0x00, 0xFF, 0x2F, 0x00,
        };
        TEST(Synth::parse_midi_file(smpte, sizeof(smpte), events, 8, 44100) == 0);
    }

    // A file whose sample time exceeds the 32-bit field is rejected (no UB on the cast).  An
    // extreme tempo (0xFFFFFF us/quarter) at division 1 makes one tick ~740k samples, so a
    // 6000-tick delta overruns UINT32_MAX.
    {
        const uint8_t overflow_midi[] = {
            0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, // format 0
            0x00, 0x01,                                                 // one track
            0x00, 0x01,                                                 // division: 1 tick per quarter
            0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0x10,             // track length 16
            0x00, 0xFF, 0x51, 0x03, 0xFF, 0xFF, 0xFF,                   // tempo 0xFFFFFF us/quarter
            0xAE, 0x70, 0x90, 0x3C, 0x64,                               // delta 6000, note_on ch0 note 60 vel 100
            0x00, 0xFF, 0x2F, 0x00,                                     // end of track
        };
        Synth::MidiEvent overflow_events[8] = {};
        TEST(Synth::parse_midi_file(overflow_midi, sizeof(overflow_midi), overflow_events, 8, 44100) == 0);
    }

    // soundtrack codec round-trips a tick-domain, time-ordered stream.  encode pairs each
    // note_on with its later note_off into a note_on + duration; decode reconstructs the note_off
    // at start + duration.  Times are MIDI ticks.  note_off velocity is not stored (the duration
    // model drops it), so note_offs here carry note_data 0, matching the reconstructed value.
    {
        Synth::MidiEvent events[7] = {};

        events[0].time      = 0;
        events[0].event     = Synth::EvType::note_on;
        events[0].channel   = 0;
        events[0].note      = 60;
        events[0].note_data = 100;

        events[1].time            = 0;
        events[1].event           = Synth::EvType::controller;
        events[1].channel         = 2;
        events[1].controller      = 7;
        events[1].controller_data = 120;

        events[2].time      = 50;
        events[2].event     = Synth::EvType::aftertouch;
        events[2].channel   = 0;
        events[2].note      = 60;
        events[2].note_data = 40;

        events[3].time       = 100;
        events[3].event      = Synth::EvType::pitch_bend;
        events[3].channel    = 0;
        events[3].pitch_bend = -2048;

        events[4].time      = 100;
        events[4].event     = Synth::EvType::note_on;
        events[4].channel   = 2;
        events[4].note      = 67;
        events[4].note_data = 90;

        events[5].time      = 200;
        events[5].event     = Synth::EvType::note_off;
        events[5].channel   = 0;
        events[5].note      = 60;
        events[5].note_data = 0;

        events[6].time      = 300;
        events[6].event     = Synth::EvType::note_off;
        events[6].channel   = 2;
        events[6].note      = 67;
        events[6].note_data = 0;

        const uint32_t    event_count = 7;
        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = {};
        const uint32_t    written    = Synth::encode_soundtrack(events, event_count, dest, sizeof(dest), &soundtrack);
        TEST(written > 0);

        Synth::MidiEvent decoded[7] = {};
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 7) == event_count);
        TEST(memcmp(events, decoded, event_count * sizeof(Synth::MidiEvent)) == 0);

        // A buffer too small to hold the planes fails cleanly.
        Synth::Soundtrack scratch = {};
        TEST(Synth::encode_soundtrack(events, event_count, dest, 4, &scratch) == 0);

        // An out-of-range channel is rejected.
        Synth::MidiEvent bad_channel = events[0];
        bad_channel.channel          = Synth::max_channels;
        TEST(Synth::encode_soundtrack(&bad_channel, 1, dest, sizeof(dest), &scratch) == 0);
    }

    // an unmatched note_on (no note_off) encodes as an unbounded note and decodes back to a
    // single note_on with no reconstructed note_off.
    {
        Synth::MidiEvent events[1] = {};
        events[0].time             = 0;
        events[0].event            = Synth::EvType::note_on;
        events[0].channel          = 0;
        events[0].note             = 48;
        events[0].note_data        = 80;

        uint8_t           dest[64];
        Synth::Soundtrack soundtrack = {};
        TEST(Synth::encode_soundtrack(events, 1, dest, sizeof(dest), &soundtrack) > 0);

        Synth::MidiEvent decoded[4] = {};
        TEST(Synth::decode_soundtrack(soundtrack, decoded, 4) == 1);
        TEST(decoded[0].event == Synth::EvType::note_on);
        TEST(decoded[0].note == 48 && decoded[0].note_data == 80);
    }

    // D3 (retrigger + multi-byte VLQ): an overlapping same-note retrigger closes the earlier note
    // at the new note_on's time, and large tick deltas/durations (> 0x3FFF) exercise multi-byte VLQ.
    {
        Synth::MidiEvent events[3] = {};
        events[0].time             = 0;
        events[0].event            = Synth::EvType::note_on;
        events[0].channel          = 0;
        events[0].note             = 64;
        events[0].note_data        = 100;
        events[1].time             = 20000;
        events[1].event            = Synth::EvType::note_on; // retrigger; delta > 0x3FFF
        events[1].channel          = 0;
        events[1].note             = 64;
        events[1].note_data        = 110;
        events[2].time             = 60000;
        events[2].event            = Synth::EvType::note_off; // duration > 0x3FFF
        events[2].channel          = 0;
        events[2].note             = 64;
        events[2].note_data        = 0;

        uint8_t           dest[256];
        Synth::Soundtrack soundtrack = {};
        TEST(Synth::encode_soundtrack(events, 3, dest, sizeof(dest), &soundtrack) > 0);

        // Decoded: note_on@0, note_off@20000 (earlier note closed at retrigger), note_on@20000,
        // note_off@60000.  Sort is by (time, channel); same-time events keep input order.
        Synth::MidiEvent decoded[8]    = {};
        const uint32_t   decoded_count = Synth::decode_soundtrack(soundtrack, decoded, 8);
        TEST(decoded_count == 4);

        uint32_t note_offs     = 0;
        uint32_t off_at_20000  = 0;
        uint32_t off_at_60000  = 0;
        int      off_idx_20000 = -1;
        int      on_idx_20000  = -1;
        for (uint32_t i = 0; i < decoded_count; i++) {
            if (decoded[i].event == Synth::EvType::note_off) {
                note_offs++;
                if (decoded[i].time == 20000) {
                    off_at_20000++;
                    off_idx_20000 = (int)i;
                }
                if (decoded[i].time == 60000) {
                    off_at_60000++;
                }
                TEST(decoded[i].note == 64);
            }
            else if (decoded[i].event == Synth::EvType::note_on && decoded[i].time == 20000) {
                on_idx_20000 = (int)i;
            }
        }
        TEST(note_offs == 2);
        TEST(off_at_20000 == 1); // earlier note closed at the retrigger
        TEST(off_at_60000 ==
             1); // second note closed by its real note_off
                 // The reconstructed note_off must precede the retriggered note_on at the same tick, so the
                 // player closes the old voice before allocating the new one (else the retrigger drops it).
        TEST(off_idx_20000 >= 0 && on_idx_20000 >= 0 && off_idx_20000 < on_idx_20000);
    }

    // a note_on's stored duration resolves to the absolute sample at which the player
    // auto-releases its voice (0 = none).  Stored value v means a real duration of v - 1 ticks.
    {
        TEST(Synth::soundtrack_note_release_sample(1000, 0, 50) == 0);     // unbounded -> none
        TEST(Synth::soundtrack_note_release_sample(1000, 11, 50) == 1500); // 10 ticks * 50 + 1000
        TEST(Synth::soundtrack_note_release_sample(1000, 1, 50) == 1000);  // 0-tick note releases at start
        TEST(Synth::soundtrack_note_release_sample(0, 1, 50) == 0);        // degenerate: 0-tick at 0 -> none
    }

    // ---- instrument bank persistence (codec) ----

    // Stamp several distinct fields so a memcmp discriminates more than one byte.
    auto stamp_bank = [](Synth::InstrumentBank& b, uint8_t k) {
        b.drum_track_channel = k;
        b.instruments.allocate();
        b.instruments.entries[0].layer_count = 1;
        b.channel_zones[1][0].start_note     = k;
        b.channel_zones[1][0].instrument     = 0;
    };

    // SYIB encode/decode round-trips the whole bank exactly (full-struct memcmp).  Bad input
    // fails cleanly without touching the destination (full-struct check, not one field).
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        stamp_bank(bank, 4);

        static uint8_t blob[sizeof(Synth::InstrumentBank) + 64];
        const uint32_t n = Synth::encode_instrument_bank(&bank, blob, sizeof(blob));
        TEST(n > 0);

        static Synth::InstrumentBank restored;
        memset(&restored, 0x5A, sizeof(restored));
        TEST(Synth::decode_instrument_bank(blob, n, &restored));
        TEST(memcmp(&restored, &bank, sizeof(bank)) == 0);

        // Bad input: truncated / corrupt marker -> false, destination byte-for-byte untouched.
        static Synth::InstrumentBank untouched;
        memset(&untouched, 0x33, sizeof(untouched));
        static Synth::InstrumentBank untouched_ref;
        memset(&untouched_ref, 0x33, sizeof(untouched_ref));
        TEST(! Synth::decode_instrument_bank(blob, 3, &untouched)); // truncated
        TEST(memcmp(&untouched, &untouched_ref, sizeof(untouched)) == 0);
        blob[0] ^= 0xFFu; // corrupt marker
        TEST(! Synth::decode_instrument_bank(blob, n, &untouched));
        TEST(memcmp(&untouched, &untouched_ref, sizeof(untouched)) == 0);
    }

    // ---- Instrument bank: dense-pool validation, serialize, publish queue ----

    // Builds a small valid bank: one instrument, one envelope, one LFO, one zone.
    auto make_valid_bank = [](Synth::InstrumentBank& bank) {
        memset(&bank, 0, sizeof(bank));
        const uint32_t env                             = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points         = 2;
        bank.envelopes.entries[env].points[1].position = 100;
        const uint32_t lfo                             = bank.lfos.allocate();
        bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
        bank.lfos.entries[lfo].period_ms               = 50;
        const uint32_t instr                           = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count    = 1;
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_volume].envelope_desc_id =
            static_cast<uint16_t>(env + 1);
        bank.instruments.entries[instr].layers[0].gen[Synth::mod_pitch].lfo_desc_id = static_cast<uint16_t>(lfo + 1);
        bank.channel_zones[0][0] = { 1, static_cast<uint8_t>(instr) };
        bank.channel_enabled[0]  = 1;
        bank.channel_enabled[1]  = 1; // zone-rule negatives exercise channel 1
    };

    // A valid bank passes validation.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Default bank: init_default_bank builds the first-run bank; every
    // structural invariant the editor and runtime rely on holds out of the box.
    {
        static Synth::InstrumentBank bank;
        Synth::init_default_bank(&bank);
        TEST(Synth::validate_instrument_bank(&bank));
        TEST(bank.channel_enabled[0] == 1);
        bool others_disabled = true;
        for (uint32_t c = 1; c < Synth::max_channels; c++) {
            others_disabled = others_disabled && bank.channel_enabled[c] == 0;
        }
        TEST(others_disabled);
        char def_name[Synth::max_name_len];
        Synth::get_default_channel_name(0, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Channel 01", 11) == 0);
        Synth::get_default_channel_name(9, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Drum Track", 11) == 0);
        Synth::get_default_channel_name(15, def_name, sizeof(def_name));
        TEST(memcmp(def_name, "Channel 16", 11) == 0);
        TEST(bank.drum_track_channel == 9);
        TEST(bank.channel_zones[0][0].start_note == 1);
        TEST(bank.channel_zones[0][0].instrument == 0);
        TEST(bank.instruments.num_allocated == 1);
        TEST(bank.envelopes.num_allocated == 0);       // bare default: no descriptors
        TEST(bank.lfos.num_allocated == 0);            // bare default: no master LFOs
        TEST(bank.channel_chains[0].num_effects == 0); // bare default: no chain
        TEST(bank.master_chain.num_effects == 0);      // bare default: no master effects
        // The bare default instrument: one sine layer, neutral shared routing.
        const Synth::Instrument& instr = bank.instruments.entries[0];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].osc_type[0] == Synth::WaveType::sine_wave);
        TEST(instr.layers[0].osc_type[1] == Synth::WaveType::no_wave);
        TEST(instr.layers[0].osc_mode == Synth::osc_mode_blend);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            TEST(instr.layers[0].gen[t].envelope_desc_id == 0);
            TEST(instr.layers[0].gen[t].lfo_desc_id == 0);
            TEST(instr.routing[t].num_inputs == 0);
        }
        TEST(instr.routing[Synth::mod_volume].base_value == 1.0f);
        TEST(instr.routing[Synth::mod_panning].base_value == 0.5f);
    }

    // init_default_channel into a populated bank appends a bare instrument and
    // allocates no descriptors and no channel effect chain.
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        TEST(bank.envelopes.allocate() == 0);
        bank.envelopes.entries[0].num_points         = 2;
        bank.envelopes.entries[0].points[1].position = 100;
        TEST(bank.lfos.allocate() == 0);
        bank.lfos.entries[0].wave      = Synth::WaveType::sine_wave;
        bank.lfos.entries[0].period_ms = 50;
        TEST(bank.instruments.allocate() == 0);
        bank.instruments.entries[0].layer_count = 1;
        TEST(Synth::init_default_channel(&bank, 5));
        TEST(bank.instruments.num_allocated == 2);
        TEST(bank.envelopes.num_allocated == 1);
        TEST(bank.lfos.num_allocated == 1);
        TEST(bank.channel_zones[5][0].start_note == 1);
        TEST(bank.channel_zones[5][0].instrument == 1);
        const Synth::Instrument& instr = bank.instruments.entries[1];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].osc_type[0] == Synth::WaveType::sine_wave);
        TEST(instr.layers[0].osc_type[1] == Synth::WaveType::no_wave);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            TEST(instr.layers[0].gen[t].envelope_desc_id == 0);
            TEST(instr.layers[0].gen[t].lfo_desc_id == 0);
            TEST(instr.routing[t].num_inputs == 0);
        }
        TEST(instr.routing[Synth::mod_volume].base_value == 1.0f);
        TEST(instr.routing[Synth::mod_panning].base_value == 0.5f);
        TEST(bank.channel_chains[5].num_effects == 0);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // init_default_channel fails cleanly when the instruments pool lacks
    // space, leaving the bank byte-for-byte untouched.
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        bank.instruments.entries[0].layer_count = 1;
        for (uint32_t i = 0; i < Synth::max_instruments; i++) {
            TEST(bank.instruments.allocate() == i);
        }
        static Synth::InstrumentBank before;
        memcpy(&before, &bank, sizeof(bank));
        TEST(! Synth::init_default_channel(&bank, 2));
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // reclaim_unused_slots removes instruments no enabled channel's zone table references and
    // descriptors no surviving instrument or effect chain references, compacting the pools and
    // remapping zones, names, and references in one pass.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank); // instrument 0 (env 1, lfo 1) on ch0; ch0 and ch1 enabled

        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count                                       = 1;
        bank.bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id = 1; // shares env 1
        TEST(bank.bank.instruments.allocate() == 2);
        bank.bank.instruments.entries[2].layer_count = 1;
        TEST(bank.bank.envelopes.allocate() == 1);
        bank.bank.envelopes.entries[1].num_points         = 2;
        bank.bank.envelopes.entries[1].points[1].position = 200;
        bank.bank.instruments.entries[2].layers[0].gen[Synth::mod_volume].envelope_desc_id =
            2;                                    // env 2 roots nothing but instr 2
        bank.bank.channel_zones[1][0] = { 1, 1 }; // ch1 -> instrument 1

        memcpy(bank.instrument_names[0], "KeepA", 6);
        memcpy(bank.instrument_names[1], "KeepB", 6);
        memcpy(bank.instrument_names[2], "Drop", 5);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        Synth::reclaim_unused_slots(&bank);

        // The orphaned instrument is gone; survivors keep their relative order, names included.
        TEST(bank.bank.instruments.num_allocated == 2);
        TEST(bank.bank.channel_zones[0][0].instrument == 0);
        TEST(bank.bank.channel_zones[1][0].instrument == 1);
        TEST(memcmp(bank.instrument_names[0], "KeepA", 6) == 0);
        TEST(memcmp(bank.instrument_names[1], "KeepB", 6) == 0);
        // Env 2 (referenced only by the removed instrument) is reclaimed; env 1 keeps id 1.
        TEST(bank.bank.envelopes.num_allocated == 1);
        TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(bank.bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // Effect chains root LFOs: with every channel disabled the chain's LFO survives while the
    // instrument's LFO is reclaimed, and the chain's reference is remapped to the compacted id.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank); // lfo 1 referenced by instrument 0
        TEST(bank.bank.lfos.allocate() == 1);
        bank.bank.lfos.entries[1].wave                            = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[1].period_ms                       = 90;
        bank.bank.master_chain.num_effects                        = 1;
        bank.bank.master_chain.effects[0].type                    = Synth::EffectType::distortion;
        bank.bank.master_chain.effects[0].enabled                 = true;
        bank.bank.master_chain.effects[0].bindings[0].base_value  = 1.0f;
        bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id = 2;
        TEST(Synth::validate_instrument_bank(&bank.bank));

        bank.bank.channel_enabled[0] = 0;
        bank.bank.channel_enabled[1] = 0;
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.bank.instruments.num_allocated == 0);
        TEST(bank.bank.lfos.num_allocated == 1);
        TEST(bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id == 1); // remapped 2 -> 1
        TEST(bank.bank.lfos.entries[0].wave == Synth::WaveType::sawtooth_wave);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // A clean bank reclaims nothing: the bank comes back byte-for-byte identical.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);
        static Synth::InstrumentEditorBank before;
        memcpy(&before, &bank, sizeof(bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(memcmp(&bank, &before, sizeof(bank)) == 0);
    }

    // Disabled channels may hold stale zone bytes; reclaim clears references to removed
    // instruments and remaps the rest without touching the (skipped) zone structure.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank); // instr 0 on ch0
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count = 1;
        bank.bank.channel_zones[1][0]                = { 1, 1 }; // ch1 -> instr 1
        TEST(bank.bank.instruments.allocate() == 2);
        bank.bank.instruments.entries[2].layer_count = 1;         // orphaned
        bank.bank.channel_enabled[0]                 = 0;         // instr 0 loses its last live reference
        bank.bank.channel_zones[2][0]                = { 5, 7 };  // ch2 disabled: dangling reference
        bank.bank.channel_zones[2][1]                = { 9, 0 };  // reference to the instrument about to be removed
        bank.bank.channel_zones[2][2]                = { 13, 1 }; // reference to a survivor (compacts 1 -> 0)
        Synth::reclaim_unused_slots(&bank);

        TEST(bank.bank.instruments.num_allocated == 1);
        TEST(bank.bank.envelopes.num_allocated == 0); // env 1 rooted instr 0 only
        TEST(bank.bank.channel_zones[2][0].instrument == 0);
        TEST(bank.bank.channel_zones[2][1].instrument == 0);
        TEST(bank.bank.channel_zones[2][2].instrument == 0);
        TEST(bank.bank.channel_zones[1][0].instrument == 0);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // Validation negatives: each mutation makes exactly the targeted rule reject the bank.
    auto expect_invalid = [](Synth::InstrumentBank& bank) { TEST(! Synth::validate_instrument_bank(&bank)); };
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.num_allocated = Synth::max_instruments + 1; // over capacity
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.num_allocated = 2; // count mismatch: only 1 occupied
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.free(0); // hole below num_allocated: count/occupied mismatch
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 11, 0 };
        bank.channel_zones[1][2] = { 6, 0 }; // later zone starts before the earlier one
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 0, 0 };
        bank.channel_zones[1][2] = { 20, 0 }; // garbage after the terminator
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 200 }; // slot 0 instrument unoccupied (dangling)
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 1, 0 };
        bank.channel_zones[1][1] = { 20, 1 }; // reachable entry references an unoccupied instrument
        expect_invalid(bank);
    }
    // Slot 0 must start at note 0 (stored start 1); any other first start is invalid.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[1][0] = { 5, 0 };
        expect_invalid(bank);
    }
    // Disabled channels keep arbitrary zone bytes: garbage zones pass while disabled and
    // fail once the channel is enabled.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_zones[3][0] = { 9, 7 };
        bank.channel_zones[3][1] = { 2, 80 }; // out of order
        bank.channel_enabled[3]  = 0;
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_enabled[3] = 1;
        expect_invalid(bank);
    }
    // channel_enabled bytes must be 0/1.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_enabled[2] = 2;
        expect_invalid(bank);
    }
    // A full zone table with no terminator is valid (route_instrument semantics unchanged).
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        for (uint32_t e = 0; e < Synth::max_instr_per_channel; e++) {
            bank.channel_zones[1][e] = { static_cast<uint8_t>(e + 1), 0 };
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id = 5; // dangling
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].lfo_desc_id = 9; // dangling
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].num_points = Synth::max_envelope_points + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points                 = 3;
        env.points[2].position         = env.points[1].position; // duplicate position
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points                 = 3;
        env.points[2].position         = 50; // decreasing position
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.sustain_first_point        = 2; // sustain_first > sustain_last
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.envelopes.entries[0].sustain_last_point = Synth::max_envelope_points;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].period_ms = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::no_wave; // not runtime-supported
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::pulse_wave;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.lfos.entries[0].wave = Synth::WaveType::noise_wave;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[0] = static_cast<Synth::WaveType>(99);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_type[1] = static_cast<Synth::WaveType>(200);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].osc_mode = static_cast<Synth::OscMode>(3);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layer_count = 0;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layer_count = Synth::max_layers + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[0].lfo_depth_source = static_cast<Synth::ModSource>(77);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].layers[0].gen[0].lfo_op = static_cast<Synth::SourceOp>(9);
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].routing[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.instruments.entries[0].routing[0].num_inputs   = 1;
        bank.instruments.entries[0].routing[0].inputs[0].op = static_cast<Synth::SourceOp>(7);
        expect_invalid(bank);
    }

    // ---- Effect chains in the bank (schema, validation, codec) ----

    TEST(Synth::get_effect_state_bytes(Synth::EffectType::delay) == 353024); // 88201 floats, 256-aligned

    // A chain with one enabled delay and an LFO-driven param passes validation.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain         = bank.channel_chains[0];
        chain.num_effects                        = 1;
        chain.effects[0].type                    = Synth::EffectType::delay;
        chain.effects[0].enabled                 = true;
        chain.effects[0].bindings[0].base_value  = 250.0f;
        chain.effects[0].bindings[0].lfo_desc_id = 1; // make_valid_bank allocated one LFO
        chain.effects[0].bindings[0].lfo_op      = Synth::SourceOp::add;
        chain.effects[0].bindings[0].lfo_depth   = 100.0f;
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects     = 1;
        bank.channel_chains[0].effects[0].type = Synth::EffectType::num_types; // invalid type
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects = Synth::max_chain_effects + 1; // over capacity
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                        = 1;
        bank.channel_chains[0].effects[0].type                    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9; // dangling LFO ref
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects                       = 1;
        bank.master_chain.effects[0].type                   = Synth::EffectType::delay;
        bank.master_chain.effects[0].bindings[0].num_inputs = 1; // master chain: no MIDI inputs
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects                             = 1;
        bank.master_chain.effects[0].type                         = Synth::EffectType::delay;
        bank.master_chain.effects[0].bindings[0].lfo_desc_id      = 1;
        bank.master_chain.effects[0].bindings[0].lfo_depth_source = Synth::ModSource::pitch_bend; // master: LFO-only
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                             = 1;
        bank.channel_chains[0].effects[0].type                         = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::velocity; // per-note role
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs = Synth::max_mod_inputs + 1;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].base_value = NAN;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                             = 1;
        bank.channel_chains[0].effects[0].type                         = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].bindings[0].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale  = INFINITY;
        expect_invalid(bank);
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects; // 4 delays: exactly the state budget
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type    = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        TEST(Synth::validate_instrument_bank(&bank));
    }
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.master_chain.num_effects = Synth::max_chain_effects;
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            bank.master_chain.effects[slot].type    = Synth::EffectType::delay;
            bank.master_chain.effects[slot].enabled = true;
        }
        bank.channel_chains[0].num_effects        = 1; // 5th delay: over the whole-bank state budget
        bank.channel_chains[0].effects[0].type    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true;
        expect_invalid(bank);
    }
    {
        // Exactly 32 modulated effect params pass; one more is rejected.
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 32; channel++) {
            Synth::EffectChainBinding& chain = bank.channel_chains[channel];
            for (uint32_t slot = 0; slot < Synth::max_chain_effects && num_modulated < 32; slot++) {
                chain.num_effects        = static_cast<uint8_t>(slot + 1);
                chain.effects[slot].type = Synth::EffectType::distortion; // 2 params, no state
                for (uint32_t param = 0; param < 2 && num_modulated < 32; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 32);
        TEST(Synth::validate_instrument_bank(&bank));
        bank.channel_chains[15].num_effects                        = Synth::max_chain_effects;
        bank.channel_chains[15].effects[3].type                    = Synth::EffectType::distortion;
        bank.channel_chains[15].effects[3].bindings[1].lfo_desc_id = 1; // 33rd modulated param
        expect_invalid(bank);
    }

    // Effect chain data round-trips through the codec byte-exactly.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        bank.channel_chains[0].num_effects                             = 2;
        bank.channel_chains[0].effects[0].type                         = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled                      = true;
        bank.channel_chains[0].effects[0].bindings[0].base_value       = 300.0f;
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id      = 1;
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth        = 50.0f;
        bank.channel_chains[0].effects[1].type                         = Synth::EffectType::chorus;
        bank.channel_chains[0].effects[1].bindings[1].num_inputs       = 1;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].source = Synth::ModSource::mod_wheel;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].op     = Synth::SourceOp::multiply;
        bank.channel_chains[0].effects[1].bindings[1].inputs[0].scale  = 0.5f;
        bank.master_chain.num_effects                                  = 1;
        bank.master_chain.effects[0].type                              = Synth::EffectType::reverb;
        bank.master_chain.effects[0].enabled                           = true;
        bank.master_chain.effects[0].bindings[0].base_value            = 0.25f;

        static uint8_t blob[sizeof(Synth::InstrumentBank) + 64];
        const uint32_t n = Synth::encode_instrument_bank(&bank, blob, sizeof(blob));
        TEST(n > 0);
        static Synth::InstrumentBank restored;
        memset(&restored, 0x5A, sizeof(restored));
        TEST(Synth::decode_instrument_bank(blob, n, &restored));
        TEST(memcmp(&restored, &bank, sizeof(bank)) == 0);
    }

    // ---- Effect expansion: preflight/commit transaction over a fake node writer ----

    // A stateful chain expands onto pool nodes with state at the region base, one clear range.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].base_value         = 300.0f;
        delay->bindings[0].lfo_desc_id        = 1;
        delay->bindings[0].lfo_depth          = 50.0f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.slots[0][0].allocated_for == Synth::EffectType::delay);
        TEST(plan.num_nodes == 2); // dest + LFO leaf
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].params[0] == 300.0f);
        TEST(chains[0].effects[0].state_offs == fake_region_base);
        TEST(chains[0].effects[0].src_param_id[0] == 50); // dest node allocated first
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        TEST(writer.last_dest_lfo_node == 51); // LFO leaf allocated second

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        TEST(Synth::take_effect_clear_ranges().count == 0); // consumed once
    }
    // An unchanged slot keeps its state offset and emits no clear; a type change re-allocates.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        // Same effect: offset preserved, nothing to clear, no new consumption.
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(! plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(Synth::take_effect_clear_ranges().count == 0);

        // Type change: fresh allocation at the bump position, cleared before first use.
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);
        TEST(plan.slots[0][0].state_offs == fake_region_base + delay_bytes);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == delay_bytes + Synth::get_effect_state_bytes(Synth::EffectType::chorus));
    }
    // Over-budget and over-pool preflights fail without touching committed state.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;

        // Master: 4 delays - exactly the worst chain, fits the budget.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();
        const uint32_t committed_consumed = plan.consumed_bytes;

        // A 5th delay anywhere exceeds the budget: preflight fails, nothing mutates.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed);    // plan untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // no clears recorded

        // The committed configuration (5th delay dropped again) still re-preflights
        // cleanly via the preservation path.
        bank.channel_chains[0].num_effects = 0;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed);

        // 33 modulated params exceed the modulation pool: rejected wherever they sit.
        Synth::init_effect_state_region(fake_region_base);
        static Synth::InstrumentBank pool_bank;
        make_valid_bank(pool_bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 33; channel++) {
            Synth::EffectChainBinding& chain = pool_bank.channel_chains[channel];
            chain.num_effects                = 4;
            for (uint32_t slot = 0; slot < 4 && num_modulated < 33; slot++) {
                chain.effects[slot].type = Synth::EffectType::distortion; // 2 params, no state
                for (uint32_t param = 0; param < 2 && num_modulated < 33; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 33);
        TEST(! Synth::preflight_effect_expansion(pool_bank, &plan, &error));

        // An input-only binding (no LFO) costs one node, not two.
        Synth::init_effect_state_region(fake_region_base);
        static Synth::InstrumentBank input_bank;
        make_valid_bank(input_bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(input_bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs         = 1;
        delay->bindings[0].inputs[0].source   = Synth::ModSource::mod_wheel;
        TEST(Synth::preflight_effect_expansion(input_bank, &plan, &error));
        TEST(plan.num_nodes == 1);
    }
    // A shrinking chain leaves no stale instances behind.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 0, 1, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 2);

        bank.channel_chains[0].num_effects = 1;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[1].type == Synth::EffectType::none);
        TEST(! chains[0].effects[1].enabled);
        TEST(chains[0].effects[1].state_offs == 0);
        TEST(chains[0].effects[1].src_param_id[0] == 0);
    }

    // Stateless effects (distortion) claim no state, no offset and no clear range.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const distortion = enabled_effect(bank, 0, 0, Synth::EffectType::distortion);
        distortion->bindings[0].base_value         = 5.0f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == 0);
        TEST(! plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == 0);

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].state_offs == 0);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // A preflight that fails validation leaves the caller's plan byte-identical.  The commit
    // is only reachable after a passing preflight (set_current_bank returns early on
    // failure), so a rejected bank can never reach the writer or the chains.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs         = Synth::max_mod_inputs + 1; // over the input bound

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        plan.consumed_bytes = 0xABCD;
        const char* error   = nullptr;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 0xABCD);                // untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // nothing recorded either
    }
    // Each commit-safety rule the writer relies on is checked by the preflight.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        const char*                       error = nullptr;
        static Synth::EffectExpansionPlan plan;

        Synth::init_effect_state_region(fake_region_base);

        // Dangling LFO descriptor.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Non-finite LFO depth.
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth   = NAN;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Per-note MIDI source on a channel effect.
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth        = 1.0f;
        bank.channel_chains[0].effects[0].bindings[1].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[1].inputs[0].source = Synth::ModSource::velocity;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Any MIDI source on the master chain (here: via the LFO depth source).
        Synth::EffectSlotBinding* const reverb =
            enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        reverb->bindings[0].lfo_desc_id      = 1;
        reverb->bindings[0].lfo_depth_source = Synth::ModSource::mod_wheel;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
    }
    // Clears accumulate across publishes drained in one step; the take hands out all of them.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        enabled_effect(bank, 1, 0, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        static Synth::EffectChain         chains[Synth::max_channels];
        static Synth::EffectChain         master;
        FakeWriter                        writer = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const uint32_t chorus_bytes = Synth::get_effect_state_bytes(Synth::EffectType::chorus);
        const uint32_t delay_bytes  = Synth::get_effect_state_bytes(Synth::EffectType::delay);

        // Re-type both slots: the drained second publish allocates fresh state and clears it.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 1, 0, Synth::EffectType::delay);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 4); // two commits x two stateful slots, none consumed in between
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == chorus_bytes);
        TEST(clears.ranges[2].offset == fake_region_base + 2 * chorus_bytes); // second commit, fresh
        TEST(clears.ranges[2].bytes == delay_bytes);
        TEST(clears.ranges[3].offset == fake_region_base + 2 * chorus_bytes + delay_bytes);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // Channel 15 and the master chain expand like any other chain.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const ch15 =
            enabled_effect(bank, Synth::max_channels - 1, 0, Synth::EffectType::delay);
        ch15->bindings[0].base_value        = 100.0f;
        Synth::EffectSlotBinding* const rev = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        rev->bindings[0].base_value         = 0.7f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[Synth::max_channels - 1][0].state_offs == fake_region_base);
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);
        TEST(plan.slots[Synth::max_channels][0].state_offs == fake_region_base + delay_bytes);

        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[Synth::max_channels - 1].effects[0].params[0] == 100.0f);
        TEST(master.effects[0].type == Synth::EffectType::reverb);
        TEST(master.effects[0].params[0] == 0.7f);
        TEST(master.effects[0].state_offs == fake_region_base + delay_bytes);
    }
    // The clear ring holds three full commits: the init commit's ranges stay pending until
    // the first render step, where the drain can apply both queued banks.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const chorus = enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        chorus->bindings[0].base_value         = 1.5f;

        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        static Synth::EffectChain         chains[Synth::max_channels];
        static Synth::EffectChain         master;
        FakeWriter                        writer = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        for (uint32_t republish = 0; republish < 2; republish++) {
            // Each drained publish re-types the slot, so it allocates and clears fresh state.
            enabled_effect(bank, 0, 0, (republish % 2) ? Synth::EffectType::chorus : Synth::EffectType::delay);
            TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        }

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 3); // init commit + two drained publishes, none consumed in between
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }

    // Target-never-mutated: malformed images (each breaking a different validated invariant)
    // are rejected by decode and leave the destination byte-identical.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        static uint8_t image[Synth::instrument_bank_image_size<Synth::InstrumentBank>];
        TEST(Synth::encode_instrument_bank(&bank, image, sizeof(image)) ==
             Synth::instrument_bank_image_size<Synth::InstrumentBank>);

        // Byte offsets of the interesting fields inside the encoded image (header + bank copy).
        const uint8_t* const bank_base    = reinterpret_cast<const uint8_t*>(&bank);
        auto                 image_offset = [bank_base](const void* field) {
            return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(field) - bank_base) +
                   Synth::instrument_bank_header_size;
        };
        const uint32_t env_points_off        = image_offset(&bank.envelopes.entries[0].num_points);
        const uint32_t env_pos1_off          = image_offset(&bank.envelopes.entries[0].points[1].position);
        const uint32_t env_sustain_first_off = image_offset(&bank.envelopes.entries[0].sustain_first_point);
        const uint32_t env_sustain_last_off  = image_offset(&bank.envelopes.entries[0].sustain_last_point);
        const uint32_t lfo_period_off        = image_offset(&bank.lfos.entries[0].period_ms);
        const uint32_t lfo_wave_off          = image_offset(&bank.lfos.entries[0].wave);
        const uint32_t instr_layer_count_off = image_offset(&bank.instruments.entries[0].layer_count);
        const uint32_t osc_type0_off         = image_offset(&bank.instruments.entries[0].layers[0].osc_type[0]);
        const uint32_t osc_mode_off          = image_offset(&bank.instruments.entries[0].layers[0].osc_mode);
        const uint32_t gen_env_id_off =
            image_offset(&bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id);
        const uint32_t gen_lfo_id_off =
            image_offset(&bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].lfo_desc_id);
        const uint32_t num_inputs_off = image_offset(&bank.instruments.entries[0].routing[0].num_inputs);
        const uint32_t lfo_depth_source_off =
            image_offset(&bank.instruments.entries[0].layers[0].gen[0].lfo_depth_source);
        const uint32_t instr_count_off = image_offset(&bank.instruments.num_allocated);

        auto decode_must_reject = [&](const uint8_t* bad) {
            static Synth::InstrumentBank untouched;
            memset(&untouched, 0x5A, sizeof(untouched));
            static Synth::InstrumentBank reference;
            memset(&reference, 0x5A, sizeof(reference));
            TEST(! Synth::decode_instrument_bank(bad, sizeof(bad), &untouched));
            TEST(memcmp(&untouched, &reference, sizeof(untouched)) == 0);
        };

        static uint8_t bad[Synth::instrument_bank_image_size<Synth::InstrumentBank>];
        memcpy(bad, image, sizeof(bad));
        bad[env_points_off] = 0;
        decode_must_reject(bad); // 0 envelope points
        memcpy(bad, image, sizeof(bad));
        bad[env_points_off] = Synth::max_envelope_points + 1;
        decode_must_reject(bad); // 9 points
        memcpy(bad, image, sizeof(bad));
        bad[env_pos1_off] = 0;
        decode_must_reject(bad); // duplicate positions
        memcpy(bad, image, sizeof(bad));
        bad[env_sustain_first_off] = 2;
        bad[env_sustain_last_off]  = 0;
        decode_must_reject(bad); // bad sustain
        memcpy(bad, image, sizeof(bad));
        bad[lfo_period_off]     = 0;
        bad[lfo_period_off + 1] = 0;
        decode_must_reject(bad); // zero LFO period
        memcpy(bad, image, sizeof(bad));
        bad[lfo_wave_off] = static_cast<uint8_t>(Synth::WaveType::pulse_wave);
        decode_must_reject(bad); // bad LFO wave
        memcpy(bad, image, sizeof(bad));
        bad[osc_type0_off] = 99;
        decode_must_reject(bad); // bad osc_type
        memcpy(bad, image, sizeof(bad));
        bad[osc_mode_off] = 3;
        decode_must_reject(bad); // bad osc_mode
        memcpy(bad, image, sizeof(bad));
        bad[gen_env_id_off] = 5;
        decode_must_reject(bad); // dangling envelope ref
        memcpy(bad, image, sizeof(bad));
        bad[gen_lfo_id_off] = 9;
        decode_must_reject(bad); // dangling LFO ref
        memcpy(bad, image, sizeof(bad));
        bad[num_inputs_off] = Synth::max_mod_inputs + 1;
        decode_must_reject(bad); // too many inputs
        memcpy(bad, image, sizeof(bad));
        bad[lfo_depth_source_off] = 77;
        decode_must_reject(bad); // bad ModSource
        memcpy(bad, image, sizeof(bad));
        bad[instr_count_off] = 0xFF;
        decode_must_reject(bad); // num_allocated over capacity
        memcpy(bad, image, sizeof(bad));
        bad[instr_layer_count_off] = 0;
        decode_must_reject(bad); // layer_count 0
    }

    // Queue mechanics: room condition, wraparound, ordering, two-phase consume.
    // Packets are whole banks; instruments.entries[0].layer_count marks each one.
    {
        static Synth::BankUpdateQueue queue;
        memset(&queue, 0, sizeof(queue));

        static Synth::InstrumentBank bank = {};
        bank.instruments.allocate();

        // Fill the queue (capacity 2).
        for (uint32_t k = 0; k < Synth::bank_queue_capacity; k++) {
            bank.instruments.entries[0].layer_count = k + 1;
            TEST(Synth::push_bank_update(&queue, bank));
        }
        // Third push fails at tail=2, head=0.
        bank.instruments.entries[0].layer_count = 99;
        TEST(! Synth::push_bank_update(&queue, bank));

        // Consume one: the third push now succeeds (wraparound: slot 0 reused).
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 1);
        Synth::consume_bank_update(&queue);
        TEST(Synth::push_bank_update(&queue, bank));

        // Drain in order: 2, then 99.
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 2);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 99);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue) == nullptr);
    }
    // get_zone_name: named instrument and the "Zone X" fallback.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);
        memcpy(bank.instrument_names[0], "Lead", 5);
        char name[Synth::max_name_len];
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Lead") == 0);

        bank.instrument_names[0][0] = 0;
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Zone 0") == 0);

        bank.bank.channel_zones[0][1] = { 50, 0 };
        Synth::get_zone_name(&bank, 0, 1, name, sizeof(name));
        TEST(strcmp(name, "Zone 1") == 0);
    }

    // Capture semantics: a configured generator node holds its descriptor BY VALUE; mutating
    // (or replacing) the bank afterwards cannot change the node's sound.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);

        Synth::ParamDescriptor env_node              = {};
        env_node.kind                                = Synth::ParamKind::envelope;
        env_node.envelope                            = bank.envelopes.entries[0];
        bank.envelopes.entries[0].num_points         = 7; // edit the bank entry
        bank.envelopes.entries[0].points[1].position = 12345;
        TEST(env_node.envelope.num_points == 2); // node kept its own copy
        TEST(env_node.envelope.points[1].position == 100);

        Synth::ParamDescriptor lfo_node = {};
        Synth::configure_lfo(&lfo_node, bank.lfos.entries[0], Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        bank.lfos.entries[0].period_ms = 9999;
        TEST(lfo_node.lfo.lfo.period_ms == 50); // node kept its own copy
    }

    // Full-bank publish flow: two complete banks queued (coalesced upstream), drained in
    // order; the runtime bank ends at the newest and the queue is empty.
    {
        static Synth::InstrumentBank  banks[2];
        static Synth::InstrumentBank  runtime;
        static Synth::BankUpdateQueue queue;
        memset(&queue, 0, sizeof(queue));
        make_valid_bank(banks[0]);
        make_valid_bank(banks[1]);
        banks[0].instruments.entries[0].routing[Synth::mod_volume].base_value = 0.25f;
        banks[1].instruments.entries[0].routing[Synth::mod_volume].base_value = 0.75f;

        TEST(Synth::validate_instrument_bank(&banks[0]));
        TEST(Synth::validate_instrument_bank(&banks[1]));
        TEST(Synth::push_bank_update(&queue, banks[0]));
        TEST(Synth::push_bank_update(&queue, banks[1]));

        // Audio-side drain: copy each peeked bank, then consume (as the editor app's audio-step hook does).
        while (const Synth::InstrumentBank* packet = Synth::peek_bank_update(&queue)) {
            runtime = *packet;
            Synth::consume_bank_update(&queue);
        }
        TEST(approx(runtime.instruments.entries[0].routing[Synth::mod_volume].base_value, 0.75f, 0.001f));
        TEST(runtime.instruments.num_allocated == 1);
    }

    // ---- Zone table helpers: lookup, boundary moves, splits ----
    {
        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        Synth::Zone* zones = bank.bank.channel_zones[0];

        // Default channel: one zone covering 0..127, instrument 1.
        TEST(Synth::zone_entry_at(zones, 0) == 0);
        TEST(Synth::zone_entry_at(zones, 64) == 0);
        TEST(Synth::zone_entry_at(zones, 127) == 0);

        // Split at 60: zone 0 keeps 0..59, new zone 1 covers 60..127 with a cloned
        // instrument (same bytes, name copied).  The instrument byte is the pool slot
        // directly, so the default zone holds slot 0.
        TEST(Synth::zone_split_new(zones, 0, 60, &bank));
        TEST(zones[0].start_note == 1 && zones[0].instrument == 0);
        TEST(zones[1].start_note == 61 && zones[1].instrument == 1);
        TEST(zones[2].start_note == 0);
        TEST(bank.bank.instruments.num_allocated == 2);
        TEST(memcmp(bank.bank.instruments.entries, bank.bank.instruments.entries + 1, sizeof(Synth::Instrument)) == 0);
        TEST(strcmp(bank.instrument_names[0], bank.instrument_names[1]) == 0);
        TEST(Synth::zone_entry_at(zones, 59) == 0);
        TEST(Synth::zone_entry_at(zones, 60) == 1);

        // Renaming the clone does not touch the original.
        strcpy(bank.instrument_names[1], "Clone");
        TEST(strcmp(bank.instrument_names[0], "Clone") != 0);

        // Splitting at a zone's first note replaces its instrument in place (no new
        // zone slot needed).
        TEST(Synth::zone_split_new(zones, 1, 60, &bank));
        TEST(zones[1].start_note == 61 && zones[1].instrument == 2);
        TEST(bank.bank.instruments.num_allocated == 3);
        TEST(Synth::zone_entry_at(zones, 59) == 0);
        TEST(Synth::zone_entry_at(zones, 60) == 1);

        // join_next: note 50 joins zone 1, which starts at 50.
        TEST(Synth::zone_join_next(zones, 0, 50));
        TEST(zones[1].start_note == 51);
        TEST(Synth::zone_entry_at(zones, 50) == 1);
        TEST(Synth::zone_entry_at(zones, 49) == 0);

        // join_previous: note 50 joins zone 0, which pushes zone 1's start to 51.
        TEST(Synth::zone_join_previous(zones, 1, 50));
        TEST(zones[1].start_note == 52);
        TEST(Synth::zone_entry_at(zones, 50) == 0);
        TEST(Synth::zone_entry_at(zones, 51) == 1);

        // Moving a zone's last note into the previous zone removes the zone.
        TEST(Synth::zone_join_previous(zones, 1, 127));
        TEST(zones[0].start_note == 1 && zones[0].instrument == 0);
        TEST(zones[1].start_note == 0);
        TEST(Synth::zone_entry_at(zones, 127) == 0);

        // Three-zone table: [0..20] [21..70] [71..127].
        Synth::Zone* z = bank.bank.channel_zones[1];
        z[0]           = Synth::Zone{ 1, 0 };
        z[1]           = Synth::Zone{ 22, 1 };
        z[2]           = Synth::Zone{ 72, 2 };
        z[3]           = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_entry_at(z, 0) == 0);
        TEST(Synth::zone_entry_at(z, 20) == 0);
        TEST(Synth::zone_entry_at(z, 21) == 1);
        TEST(Synth::zone_entry_at(z, 70) == 1);
        TEST(Synth::zone_entry_at(z, 71) == 2);
        TEST(Synth::zone_entry_at(z, 127) == 2);

        // Clicking an existing boundary: 21 is zone 1's first note; "add to previous
        // zone" moves it into zone 0, and zone 1 starts at 22.
        TEST(Synth::zone_join_previous(z, 1, 21));
        TEST(z[0].start_note == 1 && z[1].start_note == 23);
        TEST(Synth::zone_entry_at(z, 21) == 0);
        TEST(Synth::zone_entry_at(z, 22) == 1);

        // 70 is zone 1's last note; "add to next zone" moves it into zone 2.
        TEST(Synth::zone_join_next(z, 1, 70));
        TEST(z[2].start_note == 71);
        TEST(Synth::zone_entry_at(z, 69) == 1);
        TEST(Synth::zone_entry_at(z, 70) == 2);

        // A one-note zone is dropped when its only note moves to the previous zone,
        // and the later zones shift down.
        Synth::Zone* w = bank.bank.channel_zones[2];
        w[0]           = Synth::Zone{ 1, 0 };
        w[1]           = Synth::Zone{ 51, 1 };
        w[2]           = Synth::Zone{ 52, 2 };
        w[3]           = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_previous(w, 1, 50));
        TEST(w[0].start_note == 1 && w[0].instrument == 0);
        TEST(w[1].start_note == 52 && w[1].instrument == 2);
        TEST(w[2].start_note == 0);
        TEST(Synth::zone_entry_at(w, 50) == 0);
        TEST(Synth::zone_entry_at(w, 51) == 1);

        // ... and symmetrically, moving a zone's first note into the next zone drops
        // the emptied zone; the next zone shifts down and absorbs the moved note.
        // Here note 0 leaves zone 0 ([0..49]), so zone 1 moves to slot 0 and starts
        // at 0.
        Synth::Zone* v = bank.bank.channel_zones[3];
        v[0]           = Synth::Zone{ 1, 0 };
        v[1]           = Synth::Zone{ 51, 1 };
        v[2]           = Synth::Zone{ 52, 2 };
        v[3]           = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_next(v, 0, 0));
        TEST(v[0].start_note == 1 && v[0].instrument == 1);
        TEST(v[1].start_note == 52 && v[1].instrument == 2);
        TEST(v[2].start_note == 0);
        TEST(Synth::zone_entry_at(v, 0) == 0);
        TEST(Synth::zone_entry_at(v, 50) == 0);

        // A full 16-entry table has no terminator; removal must stop at the table
        // edge and never shift entries into an adjacent populated channel's table.
        static Synth::Zone tables[2][Synth::max_instr_per_channel];
        for (uint32_t slot = 0; slot < Synth::max_instr_per_channel; slot++) {
            tables[0][slot] = Synth::Zone{ static_cast<uint8_t>(slot + 1), 0 };
            tables[1][slot] = Synth::Zone{ static_cast<uint8_t>(slot + 1), 1 };
        }
        // Removing the last zone of the full table drops it in place.
        TEST(Synth::zone_join_previous(tables[0], 15, 127));
        TEST(tables[0][15].start_note == 0 && tables[0][15].instrument == 0);
        TEST(tables[0][14].start_note == 15);
        // Removing the first zone of the full table shifts the rest down.
        for (uint32_t slot = 0; slot < Synth::max_instr_per_channel; slot++)
            tables[0][slot] = Synth::Zone{ static_cast<uint8_t>(slot + 1), 0 };
        TEST(Synth::zone_join_next(tables[0], 0, 0));
        TEST(tables[0][0].start_note == 1 && tables[0][0].instrument == 0);
        TEST(tables[0][14].start_note == 16 && tables[0][15].start_note == 0);
        // Removing a middle zone of the full table shifts only the later entries.
        for (uint32_t slot = 0; slot < Synth::max_instr_per_channel; slot++)
            tables[0][slot] = Synth::Zone{ static_cast<uint8_t>(slot + 1), 0 };
        TEST(Synth::zone_join_previous(tables[0], 7, 7));
        TEST(tables[0][7].start_note == 9);
        TEST(tables[0][13].start_note == 15 && tables[0][14].start_note == 16);
        TEST(tables[0][15].start_note == 0);
        // The adjacent populated channel's table is untouched throughout.
        for (uint32_t slot = 0; slot < Synth::max_instr_per_channel; slot++)
            TEST(tables[1][slot].start_note == static_cast<uint8_t>(slot + 1) && tables[1][slot].instrument == 1);

        // Splitting at note 1 leaves a one-note first zone [0].
        Synth::Zone* n1 = bank.bank.channel_zones[4];
        n1[0]           = Synth::Zone{ 1, 0 };
        n1[1]           = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_split_new(n1, 0, 1, &bank));
        TEST(n1[0].start_note == 1 && n1[1].start_note == 2);
        TEST(Synth::zone_entry_at(n1, 0) == 0);
        TEST(Synth::zone_entry_at(n1, 1) == 1);

        // A one-note-only first zone [0]: moving its note to the next zone drops it and
        // the next zone takes over the whole keyboard.
        Synth::Zone* s0 = bank.bank.channel_zones[5];
        s0[0]           = Synth::Zone{ 1, 0 };
        s0[1]           = Synth::Zone{ 2, 1 };
        s0[2]           = Synth::Zone{ 0, 0 };
        TEST(Synth::zone_join_next(s0, 0, 0));
        TEST(s0[0].start_note == 1 && s0[0].instrument == 1);
        TEST(s0[1].start_note == 0);
        TEST(Synth::zone_entry_at(s0, 0) == 0);
        TEST(Synth::zone_entry_at(s0, 127) == 0);

        // A full 16-zone table cannot split in the middle; splitting at a zone's first
        // note needs no new entry and stays allowed.
        static Synth::InstrumentEditorBank bank_full_table;
        Synth::init_default_bank(&bank_full_table.bank);
        Synth::Zone* f = bank_full_table.bank.channel_zones[0];
        for (uint32_t e = 0; e < Synth::max_instr_per_channel; e++) {
            f[e] = Synth::Zone{ static_cast<uint8_t>(e * 8 + 1),
                                static_cast<uint8_t>(bank_full_table.bank.instruments.allocate()) };
        }
        TEST(bank_full_table.bank.instruments.num_allocated == 17); // 1 default + 16 zone instruments
        TEST(! Synth::zone_split_new(f, 0, 4, &bank_full_table));
        TEST(f[1].start_note == 9); // table untouched on refusal
        const uint32_t pool_before = bank_full_table.bank.instruments.num_allocated;
        TEST(Synth::zone_split_new(f, 0, 0, &bank_full_table));
        TEST(bank_full_table.bank.instruments.num_allocated == pool_before + 1);

        // A full instrument pool refuses the clone with the bank untouched.
        static Synth::InstrumentEditorBank bank_full_pool;
        Synth::init_default_bank(&bank_full_pool.bank);
        while (bank_full_pool.bank.instruments.allocate() != pool_no_slot)
            ;
        TEST(! Synth::zone_split_new(bank_full_pool.bank.channel_zones[0], 0, 60, &bank_full_pool));
        TEST(bank_full_pool.bank.channel_zones[0][0].start_note == 1);
        TEST(bank_full_pool.bank.channel_zones[0][0].instrument == 0);

        // Repeated split/reclaim cycles keep the instrument pool bounded: each split
        // at the zone's first note clones the instrument and leaves the old one
        // unreferenced, so reclaim returns it to the pool.
        static Synth::InstrumentEditorBank bank_cycles;
        Synth::init_default_bank(&bank_cycles.bank);
        const uint32_t pool_start = bank_cycles.bank.instruments.num_allocated;
        for (uint32_t cycle = 0; cycle < 50; cycle++) {
            TEST(Synth::zone_split_new(bank_cycles.bank.channel_zones[0], 0, 0, &bank_cycles));
            Synth::reclaim_unused_slots(&bank_cycles);
            TEST(bank_cycles.bank.instruments.num_allocated <= pool_start + 1);
        }
    }
    // ------------------------------------------------------------------
    // Instrument library: records, the validation ladder, and reclaim on load.
    // ------------------------------------------------------------------
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);
        // Instrument 1 references env 2 and lfo 2 while unrelated descriptors exist,
        // so the record's dense renumbering is visible.
        TEST(bank.bank.envelopes.allocate() == 1);
        bank.bank.envelopes.entries[1].num_points         = 2;
        bank.bank.envelopes.entries[1].points[1].position = 100;
        TEST(bank.bank.lfos.allocate() == 1);
        bank.bank.lfos.entries[1].wave      = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[1].period_ms = 80;
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count                                       = 1;
        bank.bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id = 2;
        bank.bank.instruments.entries[1].layers[0].gen[Synth::mod_pitch].lfo_desc_id       = 2;
        memcpy(bank.instrument_names[1], "Saved", 6);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const char* const path = "synth_library_roundtrip.tmp";

        // Roundtrip: the record keeps the channel's whole instrument - every zone
        // of the channel and every instrument the zones reference - renumbered to
        // a dense prefix with the zoning on channel 0.
        bank.bank.channel_zones[1][0] = { 1, 0 };
        bank.bank.channel_zones[1][1] = { 61, 1 };
        TEST(Synth::validate_instrument_bank(&bank.bank));
        TEST(Synth::save_library_record(path, "Pads", "Saved", &bank, 1) == 0);

        Synth::LibraryEntry entries[Synth::library_max_records];
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 1);
        TEST(strcmp(entries[0].category, "Pads") == 0);
        TEST(strcmp(entries[0].name, "Saved") == 0);
        TEST(entries[0].payload_size > 0 && entries[0].payload_size <= Synth::library_payload_max);

        static Synth::InstrumentEditorBank record;
        memset(&record, 0, sizeof(record));
        uint16_t load_slot = 0;
        TEST(Synth::load_library_instrument(path, &entries[0], &record, 0, &load_slot));
        TEST(load_slot == 0);
        TEST(Synth::validate_instrument_bank(&record.bank));
        TEST(record.bank.instruments.num_allocated == 2);
        TEST(record.bank.envelopes.num_allocated == 2);
        TEST(record.bank.lfos.num_allocated == 2);
        // Both zones moved to channel 0; instrument ids stay dense and valid.
        TEST(record.bank.channel_zones[0][0].start_note == 1);
        TEST(record.bank.channel_zones[0][0].instrument == 0);
        TEST(record.bank.channel_zones[0][1].start_note == 61);
        TEST(record.bank.channel_zones[0][1].instrument == 1);
        TEST(record.bank.instruments.entries[1].layers[0].gen[Synth::mod_volume].envelope_desc_id == 2);
        TEST(record.bank.instruments.entries[1].layers[0].gen[Synth::mod_pitch].lfo_desc_id == 2);
        TEST(record.bank.channel_chains[0].num_effects == 0);
        TEST(record.bank.master_chain.num_effects == 0);
        TEST(strcmp(record.instrument_names[1], "Saved") == 0);

        // Save As renames only the record; the payload keeps the source names.
        TEST(Synth::save_library_record(path, "Pads", "Renamed", &bank, 1) == 0);
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 2);
        TEST(strcmp(entries[1].name, "Renamed") == 0);
        memset(&record, 0, sizeof(record));
        TEST(Synth::load_library_instrument(path, &entries[1], &record, 0, &load_slot));
        TEST(strcmp(record.instrument_names[1], "Saved") == 0);
        TEST(strcmp(bank.instrument_names[1], "Saved") == 0);

        remove(path);
    }

    // A whole-channel record carries the channel's effect chain; chain-referenced
    // LFOs survive even when no instrument uses them.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);
        TEST(bank.bank.lfos.allocate() == 1);
        bank.bank.lfos.entries[1].wave                                 = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[1].period_ms                            = 80;
        bank.bank.channel_chains[1].num_effects                        = 2;
        bank.bank.channel_chains[1].effects[0].type                    = Synth::EffectType::delay;
        bank.bank.channel_chains[1].effects[0].enabled                 = true;
        bank.bank.channel_chains[1].effects[0].bindings[0].base_value  = 0.25f;
        bank.bank.channel_chains[1].effects[0].bindings[0].lfo_desc_id = 2;
        // Unused parameter slots have no canonical in-memory value; a stale
        // reference there must not reach the record or crash the save remap.
        bank.bank.channel_chains[1].effects[0].bindings[4].lfo_desc_id = Synth::max_lfos + 1;
        bank.bank.channel_zones[1][0]                                  = { 1, 0 };
        bank.bank.channel_chains[1].effects[1].type                    = Synth::EffectType::distortion;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        const char* const path = "synth_library_chain.tmp";
        TEST(Synth::save_library_record(path, "Fx", "Chained", &bank, 1) == 0);
        Synth::LibraryEntry entries[Synth::library_max_records];
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 1);
        static Synth::InstrumentEditorBank record;
        memset(&record, 0, sizeof(record));
        uint16_t load_slot = 0;
        TEST(Synth::load_library_instrument(path, &entries[0], &record, 0, &load_slot));
        TEST(Synth::validate_instrument_bank(&record.bank));
        TEST(record.bank.channel_chains[0].num_effects == 2);
        TEST(record.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(record.bank.channel_chains[0].effects[0].bindings[0].base_value == 0.25f);
        TEST(record.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == 2);
        TEST(record.bank.channel_chains[0].effects[0].bindings[4].lfo_desc_id == 0);
        TEST(record.bank.lfos.num_allocated == 2);
        TEST(record.bank.lfos.entries[1].period_ms == 80);
        TEST(record.bank.master_chain.num_effects == 0);
        remove(path);
    }

    // Loading replaces the target channel's effect chain; the reclaim afterwards
    // frees the replaced chain's exclusive descriptors with the old instruments.
    {
        static Synth::InstrumentEditorBank src;
        memset(&src, 0, sizeof(src));
        src.bank.instruments.allocate();
        src.bank.instruments.entries[0].layer_count = 1;
        memcpy(src.instrument_names[0], "Chained", 8);
        src.bank.channel_zones[0][0].start_note = 1;
        src.bank.channel_zones[0][0].instrument = 0;
        src.bank.channel_enabled[0]             = 1;
        TEST(src.bank.lfos.allocate() == 0);
        src.bank.lfos.entries[0].wave                                 = Synth::WaveType::sine_wave;
        src.bank.lfos.entries[0].period_ms                            = 80;
        src.bank.channel_chains[0].num_effects                        = 1;
        src.bank.channel_chains[0].effects[0].type                    = Synth::EffectType::chorus;
        src.bank.channel_chains[0].effects[0].bindings[2].lfo_desc_id = 1;
        TEST(Synth::validate_instrument_bank(&src.bank));
        const char* const path = "synth_library_replace.tmp";
        TEST(Synth::save_library_record(path, "Fx", "Chained", &src, 0) == 0);
        Synth::LibraryEntry entries[1];
        TEST(Synth::read_library_index(path, entries, 1) == 1);

        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        bank.bank.channel_enabled[2]                                   = 1;
        bank.bank.channel_zones[2][0].start_note                       = 1;
        bank.bank.channel_zones[2][0].instrument                       = 0;
        const uint32_t default_lfos                                    = bank.bank.lfos.num_allocated;
        const uint32_t old_lfo                                         = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[old_lfo].wave                           = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[old_lfo].period_ms                      = 90;
        bank.bank.channel_chains[2].num_effects                        = 1;
        bank.bank.channel_chains[2].effects[0].type                    = Synth::EffectType::reverb;
        bank.bank.channel_chains[2].effects[0].bindings[0].lfo_desc_id = static_cast<uint16_t>(old_lfo + 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        uint16_t slot = 0;
        TEST(Synth::load_library_instrument(path, &entries[0], &bank, 2, &slot));
        // The record's chain replaced channel 2's chain, LFO remapped into the bank.
        TEST(bank.bank.channel_chains[2].num_effects == 1);
        TEST(bank.bank.channel_chains[2].effects[0].type == Synth::EffectType::chorus);
        TEST(bank.bank.channel_chains[2].effects[0].bindings[2].lfo_desc_id == bank.bank.lfos.num_allocated);
        TEST(bank.bank.lfos.entries[bank.bank.lfos.num_allocated - 1].period_ms == 80);
        Synth::reclaim_unused_slots(&bank);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        // The replaced chain's exclusive LFO is gone; the record's chain LFO stayed.
        TEST(bank.bank.lfos.num_allocated == default_lfos + 1);
        TEST(bank.bank.lfos.entries[default_lfos].period_ms == 80);
        TEST(bank.bank.channel_chains[2].effects[0].bindings[2].lfo_desc_id == default_lfos + 1);
        remove(path);
    }

    // Record shape: the chain is legal only on channel 0, effect types must be in
    // range, LFO references must resolve inside the record's LFO pool, and the
    // zone table must be nonempty, anchored, ordered, and orphan-free.  A valid
    // record loads; each rejection case introduces exactly one invalid condition.
    {
        static Synth::InstrumentEditorBank src;
        make_valid_bank(src.bank);
        src.bank.channel_enabled[0]                                   = 0;
        src.bank.channel_enabled[1]                                   = 0;
        src.bank.channel_chains[0].num_effects                        = 1;
        src.bank.channel_chains[0].effects[0].type                    = Synth::EffectType::delay;
        src.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        constexpr uint32_t bank_json_text_size                        = 1024 * 1024;
        static char        text[bank_json_text_size];
        const char* const  path         = "synth_library_shape.tmp";
        auto               attempt_load = [&](bool expect_load) {
            const uint32_t payload_len = Synth::encode_editor_bank_json(&src, text, bank_json_text_size);
            TEST(payload_len > 0 && payload_len <= Synth::library_payload_max);
            FILE* const file = fopen(path, "wb");
            TEST(file != nullptr);
            const uint32_t header[3] = { 0x42494c49, Synth::library_version, 1 };
            fwrite(header, sizeof(header), 1, file);
            char rec[52] = {};
            memcpy(rec, "Cat", 3);
            memcpy(rec + 24, "Bad", 3);
            memcpy(rec + 48, &payload_len, 4);
            fwrite(rec, sizeof(rec), 1, file);
            fwrite(text, payload_len, 1, file);
            fclose(file);
            Synth::LibraryEntry entries[1];
            TEST(Synth::read_library_index(path, entries, 1) == 1);
            static Synth::InstrumentEditorBank scratch;
            memset(&scratch, 0, sizeof(scratch));
            uint16_t load_slot = 0;
            TEST(Synth::load_library_instrument(path, &entries[0], &scratch, 0, &load_slot) == expect_load);
        };
        attempt_load(true);
        // A chain on a channel other than 0 is not a valid record.
        src.bank.channel_chains[1].num_effects     = 1;
        src.bank.channel_chains[1].effects[0].type = Synth::EffectType::delay;
        attempt_load(false);
        src.bank.channel_chains[1].num_effects = 0;
        // Neither is a non-empty master chain.
        src.bank.master_chain.num_effects     = 1;
        src.bank.master_chain.effects[0].type = Synth::EffectType::delay;
        attempt_load(false);
        src.bank.master_chain.num_effects = 0;
        // Chain LFO references must resolve inside the record's pool.
        src.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 5;
        attempt_load(false);
        src.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        // The zone table must be nonempty and anchored at note 0.
        src.bank.channel_zones[0][0].start_note = 0;
        attempt_load(false);
        src.bank.channel_zones[0][0].start_note = 1;
        // Zone starts must strictly ascend.
        src.bank.channel_zones[0][1] = { 1, 0 };
        attempt_load(false);
        src.bank.channel_zones[0][1] = { 0, 0 };
        // Every record instrument must be referenced by a zone.
        src.bank.instruments.allocate();
        src.bank.instruments.entries[1].layer_count = 1;
        attempt_load(false);
        remove(path);
    }

    // Load appends the record's instrument into a populated bank with remapped ids;
    // repeated load/delete cycles keep the pool bounded through reclaim.
    {
        static Synth::InstrumentEditorBank src;
        memset(&src, 0, sizeof(src));
        src.bank.instruments.allocate();
        src.bank.instruments.entries[0].layer_count = 1;
        memcpy(src.instrument_names[0], "Loaded", 7);
        src.bank.channel_zones[0][0].start_note = 1;
        src.bank.channel_zones[0][0].instrument = 0;
        src.bank.channel_enabled[0]             = 1;
        TEST(Synth::validate_instrument_bank(&src.bank));
        const char* const load_path = "synth_library_load.tmp";
        TEST(Synth::save_library_record(load_path, "Cat", "Inst", &src, 0) == 0);
        Synth::LibraryEntry load_entries[1];
        TEST(Synth::read_library_index(load_path, load_entries, 1) == 1);
        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        const uint32_t pool_start = bank.bank.instruments.num_allocated;
        uint16_t       slot       = 0;
        TEST(Synth::load_library_instrument(load_path, &load_entries[0], &bank, 2, &slot));
        TEST(slot == pool_start);
        TEST(strcmp(bank.instrument_names[slot], "Loaded") == 0);
        TEST(bank.bank.instruments.entries[slot].layer_count == 1);
        TEST(bank.bank.channel_zones[2][0].start_note == 1);
        TEST(bank.bank.channel_zones[2][0].instrument == slot);
        bank.bank.channel_enabled[2] = 1;
        Synth::reclaim_unused_slots(&bank);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        for (uint32_t cycle = 0; cycle < 20; cycle++) {
            TEST(Synth::load_library_instrument(load_path, &load_entries[0], &bank, 2, &slot));
            bank.bank.channel_enabled[2] = 1;
            Synth::reclaim_unused_slots(&bank);
            memset(bank.bank.channel_zones[2], 0, sizeof(bank.bank.channel_zones[2]));
            bank.bank.channel_enabled[2] = 0;
            Synth::reclaim_unused_slots(&bank);
            TEST(Synth::validate_instrument_bank(&bank.bank));
            TEST(bank.bank.instruments.num_allocated == pool_start);
        }
        remove(load_path);
    }

    // A pool without space refuses the load with the bank unmodified.
    {
        static Synth::InstrumentEditorBank src;
        memset(&src, 0, sizeof(src));
        src.bank.envelopes.allocate();
        Synth::EnvelopeDescriptor env = {};
        env.num_points                = 1;
        env.sustain_first_point       = 0;
        env.sustain_last_point        = 0;
        env.min_value                 = 0.0f;
        env.min_max_delta             = 1.0f;
        env.points[0].position        = 0;
        env.points[0].value           = 0xFFFF;
        src.bank.envelopes.entries[0] = env;
        src.bank.instruments.allocate();
        src.bank.instruments.entries[0].layer_count                                       = 1;
        src.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id = 1;
        src.bank.channel_zones[0][0].start_note                                           = 1;
        src.bank.channel_zones[0][0].instrument                                           = 0;
        src.bank.channel_enabled[0]                                                       = 1;
        TEST(Synth::validate_instrument_bank(&src.bank));
        const char* const full_path = "synth_library_full.tmp";
        TEST(Synth::save_library_record(full_path, "Cat", "Inst", &src, 0) == 0);
        Synth::LibraryEntry full_entries[1];
        TEST(Synth::read_library_index(full_path, full_entries, 1) == 1);
        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        while (bank.bank.envelopes.allocate() != pool_no_slot)
            ;
        const uint32_t env_before   = bank.bank.envelopes.num_allocated;
        const uint32_t instr_before = bank.bank.instruments.num_allocated;
        const uint32_t zone_before  = bank.bank.channel_zones[2][0].start_note;
        uint16_t       slot         = 0;
        TEST(! Synth::load_library_instrument(full_path, &full_entries[0], &bank, 2, &slot));
        TEST(bank.bank.envelopes.num_allocated == env_before);
        TEST(bank.bank.instruments.num_allocated == instr_before);
        TEST(bank.bank.channel_zones[2][0].start_note == zone_before);
        remove(full_path);
    }

    // The validation ladder refuses records at the step that first fails: framing,
    // then decode, then record shape.
    {
        static Synth::InstrumentEditorBank fat;
        Synth::init_default_bank(&fat.bank); // a valid bank, but not a valid record shape

        const char* const path = "synth_library_ladder.tmp";
        // The codec's document bound, duplicated because the codec keeps it internal;
        // if the codec's real bound grows past this, the too-large test catches it.
        constexpr uint32_t bank_json_text_size = 1024 * 1024;

        static char text[bank_json_text_size];

        const uint32_t payload_len = Synth::encode_editor_bank_json(&fat, text, bank_json_text_size);
        TEST(payload_len > 0 && payload_len <= Synth::library_payload_max);

        for (uint32_t attempt = 0; attempt < 2; attempt++) {
            // Re-encode every attempt: the corrupting patch below flips a byte of
            // the record text, and the next attempt needs pristine text again
            TEST(Synth::encode_editor_bank_json(&fat, text, bank_json_text_size) == payload_len);

            FILE* const file = fopen(path, "wb");
            TEST(file != nullptr);
            const uint32_t header[3] = { 0x42494c49, Synth::library_version, 1 };
            fwrite(header, sizeof(header), 1, file);

            char rec[52] = {};
            memcpy(rec, "Cat", 3);
            memcpy(rec + 24, "Wide", 4);
            memcpy(rec + 48, &payload_len, 4);
            fwrite(rec, sizeof(rec), 1, file);
            fwrite(text, payload_len, 1, file);
            fclose(file);

            Synth::LibraryEntry entries[4];
            TEST(Synth::read_library_index(path, entries, 4) == 1);

            static Synth::InstrumentEditorBank scratch;
            memset(&scratch, 0, sizeof(scratch));

            uint16_t load_slot = 0;

            if (attempt == 0) {
                // Framing: an index entry whose size no longer matches the record is refused.
                entries[0].payload_size = payload_len - 1;
                TEST(! Synth::load_library_instrument(path, &entries[0], &scratch, 0, &load_slot));

                // Decode: a flipped payload byte breaks the JSON text.
                FILE* const patch = fopen(path, "r+b");
                TEST(fseek(patch, static_cast<long>(entries[0].payload_offset), SEEK_SET) == 0);
                const uint8_t broken = static_cast<uint8_t>(~static_cast<uint8_t>(text[0]));
                fwrite(&broken, 1, 1, patch);
                fclose(patch);

                entries[0].payload_size = payload_len;
                TEST(! Synth::load_library_instrument(path, &entries[0], &scratch, 0, &load_slot));
            }
            else {
                // Shape: a valid full bank (zones, chains, enabled channels) is not
                // a valid record.
                TEST(! Synth::load_library_instrument(path, &entries[0], &scratch, 0, &load_slot));
            }
        }

        remove(path);
    }

    // A corrupt or truncated library is never rebuilt over: the save fails and the
    // original bytes stay untouched.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        const char* const junk_path = "synth_library_corrupt_save.tmp";
        FILE* const       junk      = fopen(junk_path, "wb");
        fwrite("not a library at all", 20, 1, junk);
        fclose(junk);
        TEST(Synth::save_library_record(junk_path, "C", "N", &bank, 0) != 0);
        FILE* const check   = fopen(junk_path, "rb");
        char        buf[20] = {};
        fread(buf, 1, 20, check);
        fclose(check);
        TEST(memcmp(buf, "not a library at all", 20) == 0);
        remove(junk_path);

        const char* const trunc_path = "synth_library_trunc_save.tmp";
        FILE* const       trunc      = fopen(trunc_path, "wb");
        TEST(trunc != nullptr);
        const uint32_t header[3] = { 0x42494c49, Synth::library_version, 2 };
        fwrite(header, sizeof(header), 1, trunc);
        char rec[52] = {};
        memcpy(rec, "C", 1);
        memcpy(rec + 24, "N", 1);
        const uint32_t zero = 0;
        memcpy(rec + 48, &zero, 4);
        fwrite(rec, sizeof(rec), 1, trunc);
        fclose(trunc);
        TEST(Synth::save_library_record(trunc_path, "C", "N2", &bank, 0) != 0);
        remove(trunc_path);
    }

    // A record whose payload exceeds the capacity is indexed but never rebuilt over:
    // the save is refused visibly (oversized status) and the original file bytes are
    // preserved, instead of silently dropping the record from a rebuild.
    {
        const char* const path = "synth_library_oversize.tmp";
        FILE* const       file = fopen(path, "wb");
        TEST(file != nullptr);
        const uint32_t header[3] = { 0x42494c49, Synth::library_version, 1 };
        fwrite(header, sizeof(header), 1, file);

        char rec[52] = {};
        memcpy(rec, "Cat", 3);
        memcpy(rec + 24, "Huge", 4);
        const uint32_t huge = Synth::library_payload_max + 2048;
        memcpy(rec + 48, &huge, 4);
        fwrite(rec, sizeof(rec), 1, file);

        static uint8_t junk[64 * 1024];
        memset(junk, 0x5a, sizeof(junk));
        for (uint32_t written = 0; written < huge; written += static_cast<uint32_t>(sizeof(junk))) {
            const uint32_t chunk = huge - written < static_cast<uint32_t>(sizeof(junk))
                                       ? huge - written
                                       : static_cast<uint32_t>(sizeof(junk));
            fwrite(junk, chunk, 1, file);
        }
        fclose(file);

        Synth::LibraryEntry      entries[8];
        Synth::LibraryScanStatus status = Synth::library_invalid;
        TEST(Synth::read_library_index(path, entries, 8, &status) == 1);
        TEST(status == Synth::library_oversized);
        TEST(entries[0].payload_size == huge);

        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        Synth::LibraryScanStatus save_status = Synth::library_valid;
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, 0, &save_status) != 0);
        TEST(save_status == Synth::library_oversized);
        // An index with more records than the rebuild can carry is refused with
        // the reason reaching the caller.
        {
            save_status                 = Synth::library_valid;
            const char* const many_path = "synth_library_many.tmp";
            FILE* const       many      = fopen(many_path, "wb");
            TEST(many != nullptr);
            const uint32_t many_header[3] = { 0x42494C49, Synth::library_version, Synth::library_max_records + 1 };
            TEST(fwrite(many_header, sizeof(many_header), 1, many) == 1);
            for (uint32_t r = 0; r <= Synth::library_max_records; r++) {
                // The record header layout is internal to the library writer; the test
                // spells it out with the public field sizes.
                struct {
                    char     category[Synth::library_category_len];
                    char     name[Synth::library_name_len];
                    uint32_t payload_size;
                } record = {};

                snprintf(record.category, sizeof(record.category), "Cat");
                snprintf(record.name, sizeof(record.name), "N%03u", r);
                record.payload_size = 2;
                TEST(fwrite(&record, sizeof(record), 1, many) == 1);
                TEST(fwrite("{}", 1, 2, many) == 2);
            }
            fclose(many);
            TEST(Synth::save_library_record(many_path, "Cat", "New", &bank, 0, &save_status) == EINVAL);
            TEST(save_status == Synth::library_invalid);
            TEST(remove(many_path) == 0);
        }
        // A corrupt record (unterminated names) blocks the rebuild: the save is
        // refused so the record's bytes are never silently dropped.
        {
            const char* const corrupt_path = "synth_library_corrupt.tmp";
            FILE* const       corrupt      = fopen(corrupt_path, "wb");
            TEST(corrupt != nullptr);
            const uint32_t corrupt_header[3] = { 0x42494C49, Synth::library_version, 1 };
            TEST(fwrite(corrupt_header, sizeof(corrupt_header), 1, corrupt) == 1);
            char bad_record[Synth::library_category_len + Synth::library_name_len + 4] = {};
            memset(bad_record, 'x', sizeof(bad_record) - 4);
            const uint32_t payload_size = 2;
            memcpy(bad_record + sizeof(bad_record) - 4, &payload_size, 4);
            TEST(fwrite(bad_record, sizeof(bad_record), 1, corrupt) == 1);
            TEST(fwrite("{}", 1, 2, corrupt) == 2);
            fclose(corrupt);
            save_status = Synth::library_valid;
            TEST(Synth::save_library_record(corrupt_path, "Cat", "New", &bank, 0, &save_status) == EINVAL);
            TEST(save_status == Synth::library_invalid);
            remove(corrupt_path);
        }
        // Refused saves report an errno failure, never success.
        save_status = Synth::library_valid;
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, Synth::max_channels, &save_status) == EINVAL);
        TEST(save_status == Synth::library_invalid);
        save_status = Synth::library_valid;
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, 1, &save_status) == EINVAL); // disabled channel
        TEST(save_status == Synth::library_invalid);
        save_status                              = Synth::library_valid;
        bank.bank.channel_zones[0][0].start_note = 0; // channel enabled but empty: no zones to save
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, 0, &save_status) == EINVAL);
        TEST(save_status == Synth::library_invalid);
        make_valid_bank(bank.bank);
        bank.bank.channel_zones[0][0].instrument =
            static_cast<uint8_t>(bank.bank.instruments.num_allocated); // dangling instrument id
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, 0, &save_status) == EINVAL);
        TEST(save_status == Synth::library_invalid);
        make_valid_bank(bank.bank);

        // The oversized record survives byte-for-byte: same size, same header,
        // same payload pattern at every spot-checked offset
        FILE* const re = fopen(path, "rb");
        TEST(re != nullptr);
        TEST(fseek(re, 0, SEEK_END) == 0);
        TEST(ftell(re) == static_cast<long>(12 + 52 + huge));
        for (uint32_t offset = 64; offset < huge; offset += 64 * 1024) {
            TEST(fseek(re, static_cast<long>(12 + 52 + offset), SEEK_SET) == 0);
            uint8_t sample = 0;
            TEST(fread(&sample, 1, 1, re) == 1);
            TEST(sample == 0x5A);
        }
        fclose(re);

        remove(path);
    }

    // Record shape: a stale zone past slot 0 in an otherwise empty bank is refused
    // even though bank validation accepts it (disabled channels skip zone checks).
    {
        const char* const payload =
            "{\"instrument_editor_bank\":{\"instruments\":[{}],\"channels\":"
            "[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0},{\"start\":60,\"instrument\":0}]}]}}";
        const uint32_t payload_len = static_cast<uint32_t>(strlen(payload));

        const char* const path = "synth_library_zoneshape.tmp";
        FILE* const       file = fopen(path, "wb");
        TEST(file != nullptr);
        const uint32_t header[3] = { 0x42494c49, Synth::library_version, 1 };
        fwrite(header, sizeof(header), 1, file);

        char rec[52] = {};
        memcpy(rec, "Cat", 3);
        memcpy(rec + 24, "Ghost", 5);
        memcpy(rec + 48, &payload_len, 4);
        fwrite(rec, sizeof(rec), 1, file);
        fwrite(payload, payload_len, 1, file);
        fclose(file);

        Synth::LibraryEntry entries[4];
        TEST(Synth::read_library_index(path, entries, 4) == 1);

        static Synth::InstrumentEditorBank scratch;
        memset(&scratch, 0, sizeof(scratch));
        uint16_t load_slot = 0;
        TEST(! Synth::load_library_instrument(path, &entries[0], &scratch, 0, &load_slot));

        remove(path);
    }

    // The scanner skips records with unterminated strings and stops at truncated
    // or foreign files; only the first library_max_records records are indexed.
    {
        Synth::LibraryEntry entries[Synth::library_max_records];
        const uint32_t      magic = 0x42494c49;
        const uint32_t      small = 4;

        const char* const path = "synth_library_scan.tmp";
        FILE* const       file = fopen(path, "wb");
        TEST(file != nullptr);
        const uint32_t header[3] = { magic, Synth::library_version, 3 };
        fwrite(header, sizeof(header), 1, file);

        char bad_category[52] = {};
        memset(bad_category, 'x', Synth::library_category_len);
        memcpy(bad_category + 48, &small, 4);
        fwrite(bad_category, sizeof(bad_category), 1, file);
        fwrite("1234", 4, 1, file);

        char bad_name[52] = {};
        memcpy(bad_name, "Cat", 3);
        memset(bad_name + 24, 'y', Synth::library_name_len);
        memcpy(bad_name + 48, &small, 4);
        fwrite(bad_name, sizeof(bad_name), 1, file);
        fwrite("5678", 4, 1, file);

        char good[52] = {};
        memcpy(good, "Cat", 3);
        memcpy(good + 24, "Inst", 4);
        memcpy(good + 48, &small, 4);
        fwrite(good, sizeof(good), 1, file);
        fwrite("9abc", 4, 1, file);
        fclose(file);

        Synth::LibraryScanStatus scan_status = Synth::library_valid;
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records, &scan_status) == 1);
        // The skipped unterminated records make the library corrupt: the scan
        // reports it so a later save refuses instead of silently deleting them.
        TEST(scan_status == Synth::library_invalid);
        TEST(strcmp(entries[0].category, "Cat") == 0 && strcmp(entries[0].name, "Inst") == 0);
        TEST(entries[0].payload_offset == 12 + 2 * (52 + 4) + 52);
        TEST(entries[0].payload_size == 4);
        remove(path);

        const char* const junk_path = "synth_library_junk.tmp";
        FILE* const       junk      = fopen(junk_path, "wb");
        fwrite("not a library at all", 20, 1, junk);
        fclose(junk);
        TEST(Synth::read_library_index(junk_path, entries, Synth::library_max_records) == 0);
        remove(junk_path);

        TEST(Synth::read_library_index("synth_library_missing.tmp", entries, Synth::library_max_records) == 0);

        const char* const trunc_path = "synth_library_trunc.tmp";
        FILE* const       trunc      = fopen(trunc_path, "wb");
        fwrite(header, sizeof(header), 1, trunc); // declares 3 records, then ends
        fclose(trunc);
        TEST(Synth::read_library_index(trunc_path, entries, Synth::library_max_records) == 0);
        remove(trunc_path);
    }

    // Overwriting a record preserves its siblings; the rebuilt file keeps the rest.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);
        const char* const path = "synth_library_overwrite.tmp";
        TEST(Synth::save_library_record(path, "C1", "N1", &bank, 0) == 0);
        TEST(Synth::save_library_record(path, "C2", "N2", &bank, 0) == 0);

        Synth::LibraryEntry entries[8];
        TEST(Synth::read_library_index(path, entries, 8) == 2);
        TEST(Synth::save_library_record(path, "C1", "N1", &bank, 0) == 0);
        TEST(Synth::read_library_index(path, entries, 8) == 2);
        TEST(strcmp(entries[0].category, "C2") == 0 && strcmp(entries[0].name, "N2") == 0);
        TEST(strcmp(entries[1].category, "C1") == 0 && strcmp(entries[1].name, "N1") == 0);
        remove(path);
    }

    // A library declaring more records than the index can hold is never rebuilt
    // over: the save refuses instead of silently dropping the tail.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        const char* const path = "synth_library_overcap.tmp";
        FILE* const       file = fopen(path, "wb");
        TEST(file != nullptr);
        const uint32_t header[3] = { 0x42494c49, Synth::library_version, Synth::library_max_records + 1 };
        fwrite(header, sizeof(header), 1, file);
        char           rec[52] = {};
        const uint32_t zero    = 0;
        memcpy(rec + 48, &zero, 4);
        for (uint32_t i = 0; i < Synth::library_max_records + 1; i++) {
            snprintf(rec, Synth::library_category_len, "C%u", i);
            fwrite(rec, sizeof(rec), 1, file);
        }
        fclose(file);

        Synth::LibraryEntry      entries[Synth::library_max_records];
        Synth::LibraryScanStatus status = Synth::library_invalid;
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records, &status) ==
             Synth::library_max_records);
        TEST(status == Synth::library_valid);
        TEST(Synth::save_library_record(path, "C0", "N", &bank, 0) != 0);
        remove(path);
    }

    // The editor bank JSON codec round-trips a populated bank byte-for-byte.
    {
        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        for (uint32_t channel = 0; channel < Synth::max_channels; channel++)
            Synth::get_default_channel_name(channel, bank.channel_names[channel], Synth::max_name_len);
        memcpy(bank.instrument_names[0], "Recipe", 7);

        TEST(Synth::validate_instrument_bank(&bank.bank));

        static char    doc[128 * 1024];
        const uint32_t len = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0 && len < sizeof(doc));
        TEST(doc[0] == '{');

        static Synth::InstrumentEditorBank restored;
        TEST(Synth::decode_editor_bank_json(doc, len, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);

        // A file round-trip through the reader/writer is equally faithful
        static Synth::InstrumentEditorBank from_file;
        const char* const                  path = "synth_bank_json.tmp";
        TEST(Synth::save_editor_bank_file(path, &bank) == 0);
        TEST(Synth::load_editor_bank_file(path, &from_file) == Synth::BankFileStatus::ok);
        TEST(memcmp(&bank, &from_file, sizeof(bank)) == 0);
        remove(path);

        // Absent means a fresh project, not a failure
        static Synth::InstrumentEditorBank scratch;
        TEST(Synth::load_editor_bank_file("synth_bank_missing.tmp", &scratch) == Synth::BankFileStatus::absent);
        // A name array without its terminator cannot be read safely: the encoder
        // refuses instead of scanning past the fixed array.
        {
            static Synth::InstrumentEditorBank unterminated;
            make_valid_bank(unterminated.bank);
            unterminated.bank.instruments.num_allocated = 1;
            memset(unterminated.instrument_names[0], 'x', Synth::max_name_len);
            char small_buffer[64 * 1024];
            TEST(Synth::encode_editor_bank_json(&unterminated, small_buffer, sizeof(small_buffer)) == 0);
            memset(unterminated.instrument_names[0], 0, Synth::max_name_len);
            memset(unterminated.channel_names[0], 'y', Synth::max_name_len);
            TEST(Synth::encode_editor_bank_json(&unterminated, small_buffer, sizeof(small_buffer)) == 0);
            // The encoder's %.9g spelling of the finite float extrema round-trips:
            // 3.40282347e+38 sits just past the exact float32 maximum read as a double,
            // yet converts back to the same finite float.
            {
                static Synth::InstrumentEditorBank extreme;
                make_valid_bank(extreme.bank);
                extreme.bank.envelopes.entries[0].min_value     = -FLT_MAX;
                extreme.bank.envelopes.entries[0].min_max_delta = FLT_MAX;
                char           extreme_text[64 * 1024];
                const uint32_t extreme_len =
                    Synth::encode_editor_bank_json(&extreme, extreme_text, sizeof(extreme_text));
                TEST(extreme_len != 0);
                static Synth::InstrumentEditorBank extreme_back;
                TEST(Synth::decode_editor_bank_json(extreme_text, extreme_len, &extreme_back));
                TEST(extreme_back.bank.envelopes.entries[0].min_value == -FLT_MAX);
                TEST(extreme_back.bank.envelopes.entries[0].min_max_delta == FLT_MAX);
                // A spelling beyond the finite float range is refused, not clamped.  The
                // replacement keeps the original token's length, so the document stays
                // grammar-valid and the non-finite binary32 conversion is what rejects it.
                memcpy(strstr(extreme_text, "\"min_max_delta\":") + strlen("\"min_max_delta\":"), "1.00000000e+39", 14);
                TEST(! Synth::decode_editor_bank_json(extreme_text, extreme_len, &extreme_back));
                // The direct memory decoder enforces the same document bound as the
                // file reader; an oversized input is refused and leaves the bank
                // untouched.
                static char oversized[1024 * 1024 + 16];
                memset(oversized, ' ', sizeof(oversized));
                oversized[0] = '{';
                oversized[1] = '}';
                static Synth::InstrumentEditorBank untouched;
                memset(&untouched, 0x3C, sizeof(untouched));
                TEST(! Synth::decode_editor_bank_json(oversized, sizeof(oversized), &untouched));
                TEST(static_cast<unsigned char>(untouched.channel_names[0][0]) == 0x3C);
                // A Unicode escape decodes to the same bytes as its literal UTF-8
                // spelling, so both forms of a name are interchangeable and the
                // re-encoded document stays valid JSON.
                {
                    static Synth::InstrumentEditorBank from_literal;
                    static Synth::InstrumentEditorBank from_escape;
                    const char* const                  literal_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\xc3\xa9\"]}}";
                    const char* const escape_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\\u00e9\"]}}";
                    TEST(Synth::decode_editor_bank_json(literal_doc,
                                                        static_cast<uint32_t>(strlen(literal_doc)),
                                                        &from_literal));
                    TEST(Synth::decode_editor_bank_json(escape_doc,
                                                        static_cast<uint32_t>(strlen(escape_doc)),
                                                        &from_escape));
                    TEST(memcmp(from_literal.channel_names[1], from_escape.channel_names[1], Synth::max_name_len) == 0);
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][0]) == 0xC3);
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][1]) == 0xA9);
                    // A surrogate pair decodes to its code point's UTF-8 spelling.
                    const char* const pair_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\\uD83D\\uDE00\"]}}";
                    TEST(Synth::decode_editor_bank_json(pair_doc,
                                                        static_cast<uint32_t>(strlen(pair_doc)),
                                                        &from_escape));
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][0]) == 0xF0);
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][1]) == 0x9F);
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][2]) == 0x98);
                    TEST(static_cast<unsigned char>(from_escape.channel_names[1][3]) == 0x80);
                    TEST(from_escape.channel_names[1][4] == 0);
                    // Malformed literal UTF-8 in a name is refused: it would come back out
                    // as an unencodable document.
                    const char* const malformed_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\xc3\x28\"]}}";
                    TEST(! Synth::decode_editor_bank_json(malformed_doc,
                                                          static_cast<uint32_t>(strlen(malformed_doc)),
                                                          &from_escape));
                    // The value-string grammar matches the key grammar: a lone low
                    // surrogate, a high surrogate not paired with a low one, and a
                    // truncated literal sequence are all refused, so nothing decodes
                    // into text the encoder could not re-emit as valid JSON.
                    const char* const lone_low_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\\uDC00\"]}}";
                    TEST(! Synth::decode_editor_bank_json(lone_low_doc,
                                                          static_cast<uint32_t>(strlen(lone_low_doc)),
                                                          &from_escape));
                    const char* const unpaired_high_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\\uD800xx\\uDC00\"]}}";
                    TEST(! Synth::decode_editor_bank_json(unpaired_high_doc,
                                                          static_cast<uint32_t>(strlen(unpaired_high_doc)),
                                                          &from_escape));
                    const char* const short_literal_doc =
                        "{\"instrument_editor_bank\":{\"channel_names\":[\"Channel 01\",\"\xe1\x80\x41\"]}}";
                    TEST(! Synth::decode_editor_bank_json(short_literal_doc,
                                                          static_cast<uint32_t>(strlen(short_literal_doc)),
                                                          &from_escape));
                    // Enum spellings dispatch on DECODED text, so an escape-per-character
                    // spelling of a wave name loads identically to its literal form.
                    const char* const enum_doc =
                        "{\"instrument_editor_bank\":{\"lfos\":[{\"wave\":\"\\u0073\\u0069\\u006E\\u0065\",\"period_ms\":100}]}}";
                    TEST(Synth::decode_editor_bank_json(enum_doc,
                                                        static_cast<uint32_t>(strlen(enum_doc)),
                                                        &from_escape));
                    TEST(static_cast<uint32_t>(from_escape.bank.lfos.entries[0].wave) == 1);
                }
            }
        }
        // A bank file that is not JSON (the retired binary SYIB layout, for
        // instance) is an invalid file; the destination stays untouched.
        {
            const char* const binary_path = "synth_bank_binary.tmp";
            FILE* const       binary      = fopen(binary_path, "wb");
            TEST(binary != nullptr);
            fwrite("SYIB\4\0\0\0stale-bytes", 1, 20, binary);
            fclose(binary);
            static Synth::InstrumentEditorBank binary_bank;
            memset(&binary_bank, 0x3C, sizeof(binary_bank));
            TEST(Synth::load_editor_bank_file(binary_path, &binary_bank) == Synth::BankFileStatus::invalid);
            const uint8_t* const sentinel  = reinterpret_cast<const uint8_t*>(&binary_bank);
            bool                 untouched = true;
            for (size_t k = 0; k < sizeof(binary_bank); k++)
                untouched = untouched && sentinel[k] == 0x3C;
            TEST(untouched);
            remove(binary_path);
        }

        // Defaults fill: an empty bank document keeps the factory defaults and
        // still validates
        static Synth::InstrumentEditorBank defaults;
        TEST(Synth::decode_editor_bank_json("{\"instrument_editor_bank\":{}}",
                                            static_cast<uint32_t>(strlen("{\"instrument_editor_bank\":{}}")),
                                            &defaults));
        TEST(defaults.bank.drum_track_channel == 9);
        TEST(defaults.bank.instruments.num_allocated == 0);
        TEST(strcmp(defaults.channel_names[0], "Channel 01") == 0);
        TEST(Synth::validate_instrument_bank(&defaults.bank));
    }

    // Unknown fields are skipped and logged in debug builds; the decode still
    // succeeds.
    {
        static Synth::InstrumentEditorBank bank;

        const char* const doc = "{\"bogus\":1,\"instrument_editor_bank\":{\"future_thing\":[3]},\"toplevel\":true}";
        TEST(Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &bank));
        TEST(Synth::validate_instrument_bank(&bank.bank)); // the default bank

        // A record payload with unknown fields loads cleanly
        static Synth::InstrumentEditorBank src;
        make_valid_bank(src.bank);
        memcpy(src.instrument_names[0], "Rec", 4);
        const char* const lib_path = "synth_library_report.tmp";
        TEST(Synth::save_library_record(lib_path, "C", "N", &src, 0) == 0);

        Synth::LibraryEntry entries[4];
        TEST(Synth::read_library_index(lib_path, entries, 4) == 1);
        static Synth::InstrumentEditorBank dst;
        uint16_t                           slot = 0;
        TEST(Synth::load_library_instrument(lib_path, &entries[0], &dst, 1, &slot));
        remove(lib_path);
    }

    // Malformed documents are rejected; a rejected decode never commits: the
    // destination keeps its bytes on syntax, schema and semantic failures alike.
    {
        static Synth::InstrumentEditorBank bank;

        const char* const bad[] = { "{",
                                    "{\"instrument_editor_bank\":}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":9,}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":\"x\"}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":01}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":1e999}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":9}} trailing",
                                    ("{\"instrument_editor_bank\":{\"envelopes\":[{\"num_points\":3,\"points\":"
                                     "[{\"position\":0},{\"position\":1}]}]}}"),
                                    "[1,2,3]",
                                    "garbage" };

        for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            // The destination holds a recognizable pattern; every failure keeps it
            memset(&bank, 0x5A, sizeof(bank));
            TEST(! Synth::decode_editor_bank_json(bad[i], static_cast<uint32_t>(strlen(bad[i])), &bank));
            const bool unchanged =
                static_cast<const uint8_t*>(static_cast<const void*>(&bank))[0] == 0x5A &&
                static_cast<const uint8_t*>(static_cast<const void*>(&bank))[sizeof(bank) - 1] == 0x5A;
            TEST(unchanged);
        }

        // A semantically invalid document (zero-period LFO) is rejected by the
        // validation stage, and the destination is still untouched
        memset(&bank, 0x5A, sizeof(bank));
        const char* const semantic = "{\"instrument_editor_bank\":{\"lfos\":[{}]}}";
        TEST(! Synth::decode_editor_bank_json(semantic, static_cast<uint32_t>(strlen(semantic)), &bank));
        TEST(static_cast<const uint8_t*>(static_cast<const void*>(&bank))[0] == 0x5A);
    }

    // Strict grammar: separators, string content, containment and duplicate keys
    // are enforced document-wide - including inside unknown subtrees the schema
    // decoder never visits.
    {
        static Synth::InstrumentEditorBank bank;

        const char* const bad[] = {
            ",{}",                                                            // separator before the root object
            "{\"future\":[1 2]}",                                             // missing element comma
            "{\"future\":[1,,2]}",                                            // empty element
            "{\"instrument_editor_bank\":{\"channels\":[{},{ 1}]}}",          // non-string key, recognized subtree
            "{\"future\":\"a\nb\"}",                                          // raw control character (literal newline)
            "{\"instrument_editor_bank\":{\"instrument_names\":[\"a\nb\"]}}", // same, recognized
            "{\"future\":\"a\\q\"}",                                          // invalid escape
            "{\"future\":\"a\\u12\"}",                                        // truncated unicode escape
            "{\"future\":\"a",                                                // unterminated string
            "{\"instrument_editor_bank\":{},\"instrument_editor_bank\":{}}",  // duplicate root key
            "{\"instrument_editor_bank\":{\"drum_track_channel\":9,\"drum_track_channel\":9}}", // dup
            "{\"future\":{\"a\":1,\"a\":2}}",                                 // duplicate key in an unknown object
            "{\"future\":{\"a\":1,\"\\u0061\":2}}",                           // escaped duplicate key
            "{\"instrument_editor_bank\":{\"drum_track_channel\":9 \"x\":1}}" // missing pair comma
        };

        for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            TEST(! Synth::decode_editor_bank_json(bad[i], static_cast<uint32_t>(strlen(bad[i])), &bank));
        }

        // A future section with unique, well-formed fields stays accepted and
        // reported as unknown
        const char* const future = "{\"instrument_editor_bank\":{},\"layout\":{\"nodes\":[{\"x\":1,\"y\":2}]}}";
        TEST(Synth::decode_editor_bank_json(future, static_cast<uint32_t>(strlen(future)), &bank));
        // Escaped key spellings dispatch by DECODED text: a recognized name
        // written with escapes is that field, and an unknown escaped name
        // is logged by its decoded form
        const char* const escaped =
            "{\"\\u0069nstrument_editor_bank\":{\"\\u0064rum_track_channel\":7},\"\\u0066uture\":1}";
        TEST(Synth::decode_editor_bank_json(escaped, static_cast<uint32_t>(strlen(escaped)), &bank));
        TEST(bank.bank.drum_track_channel == 7);
    }

    // Key uniqueness holds at any length and across escape spellings: comparison
    // walks decoded symbol streams with no whole-key buffer, so two over-long
    // spellings of one name still collide, and hex case alone cannot tell two
    // escapes of one code point apart.  A long but unique key stays legal - it is
    // logged by its decoded prefix, never rejected for its length.
    {
        static Synth::InstrumentEditorBank bank;

        static char doc[2048];

        // 256 literal 'a's vs "\u0061" + 255 'a's: both decode to 256 'a's
        uint32_t len = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\"future\":{\""));
        memset(doc + len, 'a', 256);
        len += 256;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":1,\"\\u0061"));
        memset(doc + len, 'a', 255);
        len += 255;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":2}}"));
        TEST(len < sizeof(doc));
        TEST(! Synth::decode_editor_bank_json(doc, len, &bank));

        // Two escapes of one non-byte code point, differing only in hex case
        const char* const hexcase = "{\"future\":{\"\\u010a\":1,\"\\u010A\":2}}";
        TEST(! Synth::decode_editor_bank_json(hexcase, static_cast<uint32_t>(strlen(hexcase)), &bank));

        // One long unknown name, escaped at its head: accepted, and the report
        // shows the DECODED prefix - never the raw escape text
        len = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\"\\u0066uture\":1,\""));
        memset(doc + len, 'x', 300);
        len += 300;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":2}"));
        TEST(len < sizeof(doc));
        TEST(Synth::decode_editor_bank_json(doc, len, &bank));
    }

    // Keys compare by DECODED CODE POINTS: literal UTF-8 and \u escapes of one
    // name are the same key, surrogate pairs decode to their combined code
    // point, and malformed key spellings (lone surrogates, malformed UTF-8) are
    // rejected by the grammar pass.  Unknown non-ASCII names report their
    // decoded UTF-8 prefix, truncated cleanly at the report slot boundary.
    {
        static Synth::InstrumentEditorBank bank;

        // Literal UTF-8 vs its escape: one name, rejected as a duplicate
        const char* const literal_dup = "{\"future\":{\"\xC4\x8A\":1,\"\\u010a\":2}}";
        TEST(! Synth::decode_editor_bank_json(literal_dup, static_cast<uint32_t>(strlen(literal_dup)), &bank));

        // An escaped surrogate pair vs the literal emoji: one name, rejected
        const char* const emoji_dup = "{\"future\":{\"\xF0\x9F\x98\x80\":1,\"\\uD83D\\uDE00\":2}}";
        TEST(! Synth::decode_editor_bank_json(emoji_dup, static_cast<uint32_t>(strlen(emoji_dup)), &bank));

        // Lone surrogates and a high surrogate without its low one are malformed
        const char* const bad_surrogates[] = { "{\"future\":{\"\\uD83D\":1}}",
                                               "{\"future\":{\"\\uDE00\":1}}",
                                               "{\"future\":{\"\\uD83D\\u0041\":1}}" };
        for (uint32_t i = 0; i < sizeof(bad_surrogates) / sizeof(bad_surrogates[0]); i++) {
            TEST(! Synth::decode_editor_bank_json(bad_surrogates[i],
                                                  static_cast<uint32_t>(strlen(bad_surrogates[i])),
                                                  &bank));
        }

        // Malformed literal UTF-8 in a key is rejected
        const char* const bad_utf8[] = {
            "{\"future\":{\"\xC4\":1}}",         // a lead without its continuation
            "{\"future\":{\"\x8A\":1}}",         // a continuation without its lead
            "{\"future\":{\"\xE0\x80\x80\":1}}", // an overlong spelling of zero
            "{\"future\":{\"\xED\xA0\x80\":1}}"  // a literal surrogate
        };
        for (uint32_t i = 0; i < sizeof(bad_utf8) / sizeof(bad_utf8[0]); i++) {
            TEST(! Synth::decode_editor_bank_json(bad_utf8[i], static_cast<uint32_t>(strlen(bad_utf8[i])), &bank));
        }

        // A unique non-ASCII key is accepted, logged as decoded UTF-8,
        // identically for the literal and the escaped spelling
        const char* const non_ascii[] = { "{\"\xC4\x8A\":1}", "{\"\\u010a\":1}" };
        for (uint32_t i = 0; i < sizeof(non_ascii) / sizeof(non_ascii[0]); i++) {
            TEST(Synth::decode_editor_bank_json(non_ascii[i], static_cast<uint32_t>(strlen(non_ascii[i])), &bank));
        }

        // An escaped surrogate pair is logged as its combined code point's UTF-8
        const char* const emoji_key = "{\"\\uD83D\\uDE00\":1}";
        TEST(Synth::decode_editor_bank_json(emoji_key, static_cast<uint32_t>(strlen(emoji_key)), &bank));
        // One long escaped name: the debug log shows the decoded UTF-8 prefix, cut
        // where the next code point no longer fits the slot - 23 two-byte code
        // points fill 46 of the 48 bytes, then the terminator
        static char doc[1024];
        uint32_t    len = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\""));
        for (uint32_t i = 0; i < 100; i++)
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\\u010a"));
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":1}"));
        TEST(len < sizeof(doc));
        TEST(Synth::decode_editor_bank_json(doc, len, &bank));
    }

    // Pool capacity: a document repeating pool arrays or the root bank object
    // exhausts a pool and is rejected - never written past the pool's entries.
    {
        static Synth::InstrumentEditorBank bank;

        // A full instruments pool followed by a repeated array overflows without
        // the allocation checks
        static char doc[256 * 1024];
        uint32_t    len = 0;
        len += static_cast<uint32_t>(
            snprintf(doc + len, sizeof(doc) - len, "{\"instrument_editor_bank\":{\"instruments\":["));
        for (uint32_t i = 0; i < Synth::max_instruments; i++) {
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, i ? "," : ""));
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{}"));
        }
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "],\"instruments\":[{}]}}"));
        TEST(len < sizeof(doc));

        TEST(! Synth::decode_editor_bank_json(doc, len, &bank)); // pool exhaustion

        // Repeated root bank objects also fill the pool cumulatively
        static char doc2[256 * 1024];
        uint32_t    len2 = 0;
        len2 += static_cast<uint32_t>(snprintf(doc2 + len2, sizeof(doc2) - len2, "{"));
        for (uint32_t i = 0; i < Synth::max_instruments + 1; i++) {
            len2 += static_cast<uint32_t>(snprintf(doc2 + len2, sizeof(doc2) - len2, i ? "," : ""));
            len2 += static_cast<uint32_t>(
                snprintf(doc2 + len2, sizeof(doc2) - len2, "\"instrument_editor_bank\":{\"instruments\":[{}]}"));
        }
        len2 += static_cast<uint32_t>(snprintf(doc2 + len2, sizeof(doc2) - len2, "}"));
        TEST(len2 < sizeof(doc2));
        TEST(! Synth::decode_editor_bank_json(doc2, len2, &bank));
    }

    // Byte-fidelity contract: decode is lossy BY DESIGN on first load.  An
    // in-memory bank carrying stale bytes (deleted entries, unused tails) or a
    // negative zero may differ from its restored image, which the load
    // canonicalizes.  What the format guarantees is POST-LOAD IDEMPOTENCE: a
    // loaded bank is canonical, so save(load(save(bank))) == save(bank) and a
    // second full save/load cycle reproduces the same bytes.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        // Stale pool bytes: unreferenced descriptors are reclaimed and compacted,
        // but the freed tail slots keep their old bytes
        const uint32_t extra_env                                  = bank.bank.envelopes.allocate();
        bank.bank.envelopes.entries[extra_env].num_points         = 2;
        bank.bank.envelopes.entries[extra_env].points[1].position = 50;
        const uint32_t extra_lfo                                  = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[extra_lfo].wave                    = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[extra_lfo].period_ms               = 33;
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.envelopes.num_allocated == 1);
        TEST(bank.bank.lfos.num_allocated == 1);

        // Stale bytes inside live entries: a layer beyond layer_count, a routing
        // input beyond num_inputs, envelope points beyond num_points, a name tail
        // after shortening, and a negative zero
        bank.bank.instruments.entries[0].layers[2].pitch_offset                      = 3.5f;
        bank.bank.instruments.entries[0].routing[Synth::mod_panning].num_inputs      = 1;
        bank.bank.instruments.entries[0].routing[Synth::mod_panning].inputs[1].scale = 7.0f;
        bank.bank.envelopes.entries[0].points[7].position                            = 999;
        bank.bank.envelopes.entries[0].min_value                                     = -0.0f;
        memcpy(bank.instrument_names[0], "0123456789ABCDEF", 17);
        memcpy(bank.instrument_names[0], "Rec", 4);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        static char    text1[128 * 1024];
        static char    text2[128 * 1024];
        const uint32_t len1 = Synth::encode_editor_bank_json(&bank, text1, sizeof(text1));
        TEST(len1 > 0 && len1 < sizeof(text1));

        static Synth::InstrumentEditorBank round1;
        TEST(Synth::decode_editor_bank_json(text1, len1, &round1));

        // The canonical form: negative zero normalized, stale bytes gone
        TEST(! std::signbit(round1.bank.envelopes.entries[0].min_value));
        TEST(round1.bank.envelopes.entries[1].num_points == 0);
        TEST(round1.bank.envelopes.entries[1].points[1].position == 0);
        TEST(round1.bank.lfos.entries[1].period_ms == 0);
        TEST(round1.bank.instruments.entries[0].layers[2].pitch_offset == 0.0f);
        TEST(round1.bank.instruments.entries[0].routing[Synth::mod_panning].inputs[1].scale == 0.0f);
        TEST(round1.bank.envelopes.entries[0].points[7].position == 0);
        TEST(round1.instrument_names[0][3] == 0);
        TEST(round1.instrument_names[0][10] == 0);
        TEST(Synth::validate_instrument_bank(&round1.bank));

        const uint32_t len2 = Synth::encode_editor_bank_json(&round1, text2, sizeof(text2));
        TEST(len2 > 0 && len2 < sizeof(text2));
        TEST(len1 == len2);
        TEST(memcmp(text1, text2, len1) == 0); // save(load(save(bank))) == save(bank)

        static Synth::InstrumentEditorBank round2;
        TEST(Synth::decode_editor_bank_json(text2, len2, &round2));
        TEST(memcmp(&round1, &round2, sizeof(round1)) == 0);
    }

    // Reload guarantee: a valid bank whose document exceeds the bounded token pool
    // is refused by the save instead of writing an unloadable file.
    {
        static Synth::InstrumentEditorBank bank;
        memset(&bank, 0, sizeof(bank));

        // Dense instruments: every layer binds every target to both descriptors,
        // so the encoded document needs far more than the 8192-token pool
        const uint32_t env                                  = bank.bank.envelopes.allocate();
        bank.bank.envelopes.entries[env].num_points         = 2;
        bank.bank.envelopes.entries[env].points[1].position = 100;
        const uint32_t lfo                                  = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[lfo].period_ms               = 50;

        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t     slot  = bank.bank.instruments.allocate();
            Synth::Instrument& instr = bank.bank.instruments.entries[slot];
            instr.layer_count        = Synth::max_layers;
            for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
                Synth::Oscillator& osc = instr.layers[layer];
                osc.osc_type[0]        = Synth::WaveType::sine_wave;
                osc.osc_mode           = Synth::osc_mode_fm;
                osc.mod_ratio          = 1.0f;
                for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
                    Synth::LayerGen& gen  = osc.gen[target];
                    gen.envelope_desc_id  = static_cast<uint16_t>(env + 1);
                    gen.lfo_desc_id       = static_cast<uint16_t>(lfo + 1);
                    gen.lfo_depth         = 0.5f;
                    gen.lfo_depth_source  = Synth::ModSource::velocity;
                    gen.lfo_rate_source   = Synth::ModSource::mod_wheel;
                    gen.lfo_rate_scale_ms = 1.0f;
                }
            }
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
                Synth::InputRouting& routing = instr.routing[target];
                routing.base_value           = 0.25f;
                routing.num_inputs           = Synth::max_mod_inputs;
                for (uint32_t in = 0; in < Synth::max_mod_inputs; in++) {
                    routing.inputs[in].source = Synth::ModSource::velocity;
                    routing.inputs[in].op     = Synth::SourceOp::multiply;
                    routing.inputs[in].scale  = 0.5f;
                }
            }
        }
        bank.bank.channel_zones[0][0] = { 1, 0 };
        bank.bank.channel_enabled[0]  = 1;
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const char* const path = "synth_bank_toomanytokens.tmp";
        TEST(Synth::save_editor_bank_file(path, &bank) != 0); // visibly refused
        FILE* const check = fopen(path, "rb");
        TEST(check == nullptr); // nothing was written
        if (check)
            fclose(check);
        remove(path);
    }

    // The encoder never trusts count metadata: bank validation checks only
    // parameter-pool metadata, not standalone contents, so a validator-accepted
    // bank can carry a malformed parameter.  Encoding it must fail cleanly (no
    // read past the arrays, no file written) instead of trusting the count.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        const uint32_t param = bank.bank.parameters.allocate();
        TEST(param != pool_no_slot);
        bank.bank.parameters.entries[param].kind              = Synth::ParamKind::plain;
        bank.bank.parameters.entries[param].plain.base_value  = 1.0f;
        bank.bank.parameters.entries[param].plain.num_sources = 65535; // far past sources[4]

        TEST(Synth::validate_instrument_bank(&bank.bank)); // metadata valid, contents unchecked

        static char text[64 * 1024];
        TEST(Synth::encode_editor_bank_json(&bank, text, sizeof(text)) == 0);

        const char* const bad_path = "synth_bank_badparam.tmp";
        TEST(Synth::save_editor_bank_file(bad_path, &bank) != 0);
        FILE* const bad_check = fopen(bad_path, "rb");
        TEST(bad_check == nullptr); // nothing was written
        if (bad_check)
            fclose(bad_check);
        remove(bad_path);
    }

    // A document beyond the token pool and a bank file beyond the parse buffer are
    // both visibly rejected through their bounded staging.
    {
        static char doc[128 * 1024];
        uint32_t    len = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\"instrument_editor_bank\":{"));
        for (uint32_t i = 0; i < 5000; i++)
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\"k%u\":1,", i));
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\"x\":1}}"));
        TEST(len < sizeof(doc));

        static Synth::InstrumentEditorBank bank;
        TEST(! Synth::decode_editor_bank_json(doc, len, &bank)); // token pool exhaustion

        const char* const big_path = "synth_bank_toobig.tmp";
        FILE* const       big      = fopen(big_path, "wb");
        TEST(big != nullptr);
        static uint8_t chunk[64 * 1024];
        // The codec's document bound, duplicated because the codec keeps it internal.
        constexpr uint32_t bank_json_text_size = 1024 * 1024;
        memset(chunk, 0x61, sizeof(chunk));
        for (uint32_t written = 0; written <= bank_json_text_size; written += sizeof(chunk))
            fwrite(chunk, sizeof(chunk), 1, big);
        fclose(big);
        // The loader refuses documents past the parse buffer and leaves the
        // destination untouched.
        static Synth::InstrumentEditorBank toobig_bank;
        memset(&toobig_bank, 0x5A, sizeof(toobig_bank));
        TEST(Synth::load_editor_bank_file(big_path, &toobig_bank) == Synth::BankFileStatus::too_large);
        const uint8_t* const sentinel  = reinterpret_cast<const uint8_t*>(&toobig_bank);
        bool                 untouched = true;
        for (size_t k = 0; k < sizeof(toobig_bank); k++)
            untouched = untouched && sentinel[k] == 0x5A;
        TEST(untouched);
        remove(big_path);
    }

    // A library container from another version is an invalid file: rejected
    // like any other malformed input, and never rebuilt over.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank);

        const char* const path = "synth_library_v1.tmp";
        FILE* const       file = fopen(path, "wb");
        TEST(file != nullptr);
        const uint32_t header[3] = { 0x42494c49, 1, 1 }; // the old version
        fwrite(header, sizeof(header), 1, file);

        char rec[52] = {};
        memcpy(rec, "Cat", 3);
        memcpy(rec + 24, "Old", 3);
        const uint32_t four = 4;
        memcpy(rec + 48, &four, 4);
        fwrite(rec, sizeof(rec), 1, file);
        fwrite("data", 4, 1, file);
        fclose(file);

        Synth::LibraryEntry      entries[8];
        Synth::LibraryScanStatus status = Synth::library_invalid;
        TEST(Synth::read_library_index(path, entries, 8, &status) == 0);
        TEST(status == Synth::library_invalid);

        Synth::LibraryScanStatus save_status = Synth::library_valid;
        TEST(Synth::save_library_record(path, "Cat", "New", &bank, 0, &save_status) != 0);
        TEST(save_status == Synth::library_invalid); // the reason reaches the caller

        FILE* const check              = fopen(path, "rb");
        uint8_t     bytes[12 + 52 + 4] = {};
        TEST(fread(bytes, 1, sizeof(bytes), check) == sizeof(bytes));
        fclose(check);
        TEST(bytes[4] == 1); // the original v1 file is untouched
        TEST(bytes[12] == 'C' && bytes[13] == 'a' && bytes[14] == 't');
        TEST(bytes[36] == 'O' && bytes[37] == 'l' && bytes[38] == 'd');
        remove(path);
    }

    // ---- Oscillator-graph projection (sculptor_osc_graph) ----

    // Max-complexity round-trip: a fully populated instrument projects to the
    // full graph (7 fixed nodes + 7 oscillators + 35 parameters + 35 envelope
    // and 35 LFO instances = 119 nodes, 252 edges) and compiles back
    // bit-identically, with the descriptor pools untouched (no dedup or
    // duplication) and the aliasing pattern preserved.
    {
        static Synth::InstrumentBank bank;
        static Synth::InstrumentBank bank_image;
        static Synth::Instrument     instrument;
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        bank_image = bank;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 35, 35, 35);
        TEST(graph.connection_count() == 252);

        // Re-projection onto a populated graph resets it deterministically.
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 35, 35, 35);
        TEST(graph.connection_count() == 252);

        static Synth::Instrument compiled;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);

        // The aliased LFO descriptor serves two parameters through two distinct
        // instances (their depth/rate sources differ); both compiled cells
        // carry the shared desc id.
        const int32_t pitch_cell  = find_param_by_served(mapping, 1, static_cast<uint8_t>(1u << 1));
        const int32_t volume_cell = find_param_by_served(mapping, 0, static_cast<uint8_t>(1u << 4));
        TEST(pitch_cell >= 0 && volume_cell >= 0);
        if (pitch_cell >= 0 && volume_cell >= 0) {
            TEST(mapping.params[pitch_cell].lfo_node != mapping.params[volume_cell].lfo_node);
            const int32_t lfo_a = find_instance_by_node(mapping, mapping.params[pitch_cell].lfo_node);
            const int32_t lfo_b = find_instance_by_node(mapping, mapping.params[volume_cell].lfo_node);
            TEST(lfo_a >= 0 && lfo_b >= 0);
            if (lfo_a >= 0 && lfo_b >= 0) {
                TEST(mapping.detached[lfo_a].desc_id == lfo_ids[1][Synth::mod_pitch]);
                TEST(mapping.detached[lfo_b].desc_id == lfo_ids[1][Synth::mod_pitch]);
            }
        }
        TEST(compiled.layers[1].gen[Synth::mod_pitch].lfo_desc_id == lfo_ids[1][Synth::mod_pitch]);
        TEST(compiled.layers[4].gen[Synth::mod_volume].lfo_desc_id == lfo_ids[1][Synth::mod_pitch]);
    }

    // Minimal round-trip: one unbound oscillator layer projects to the fixed
    // nodes plus one oscillator (8 nodes, one hard connection) and compiles
    // back bit-identically.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument = {};
        instrument.layer_count                  = 1;
        instrument.layers[0].osc_type[0]        = Synth::WaveType::sine_wave;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, 1, 0, 0, 0);
        TEST(graph.connection_count() == 1);
        static Synth::Instrument compiled;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        // Waveform A has no off option: the list starts at sine and compile
        // adds one back onto the wave type.
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].num_list_options == 4);
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].value.list_index == 0);
        Sculptor::PropertyValue saw;
        saw.list_index = 1;
        graph.set_slot_value(mapping.osc_nodes[0], 1, saw); // "saw"
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].value.list_index == 1);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].osc_type[0] == Synth::WaveType::sawtooth_wave);
    }

    // Grammar validator: the allowed edge set is exactly parameter -> its own
    // target's oscillator value row, envelope -> parameter envelope input,
    // LFO -> parameter LFO input, MIDI input -> parameter source input or LFO
    // depth/rate source, and the layer-matched oscillator -> sum connections.
    // A repeated allowed pair stays allowed: duplicate inputs are first-class
    // and order-significant, so there is no duplicate-source rule.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        uint16_t                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, 1, 2, 2, 1);
        TEST(graph.connection_count() == 7); // 2 osc wires + 2 env + 1 lfo + 1 source + 1 sum

        const Sculptor::EndPoint volume_out   = { mapping.params[0].node_idx, mapping.param_output_slot };
        const Sculptor::EndPoint pitch_out    = { mapping.params[1].node_idx, mapping.param_output_slot };
        const Sculptor::EndPoint osc_out      = { mapping.osc_nodes[0], mapping.osc_output_slot };
        const Sculptor::EndPoint env_out      = { mapping.params[0].env_node, mapping.env_output_slot };
        const Sculptor::EndPoint lfo_out      = { mapping.params[0].lfo_node, mapping.lfo_output_slot };
        const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
        const Sculptor::EndPoint input_out    = { mapping.input_node, mapping.input_source_slots[velocity_idx] };
        const Sculptor::EndPoint volume_row   = { mapping.osc_nodes[0], 9 };
        const Sculptor::EndPoint pitch_row    = { mapping.osc_nodes[0], 11 };
        const Sculptor::EndPoint env_in       = { mapping.params[1].node_idx, 2 };
        const Sculptor::EndPoint lfo_in       = { mapping.params[0].node_idx, 3 };
        const Sculptor::EndPoint src_in       = { mapping.params[0].node_idx, 7 };
        const Sculptor::EndPoint depth_in     = { mapping.params[0].lfo_node, mapping.lfo_depth_input_slot };
        const Sculptor::EndPoint rate_in      = { mapping.params[0].lfo_node, mapping.lfo_rate_input_slot };
        const Sculptor::EndPoint sum_in       = { mapping.output_node, mapping.output_layer_input_slot[0] };
        TEST(Sculptor::osc_graph_validate(&mapping, graph, volume_out, volume_row));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, pitch_out, pitch_row));
        // The volume envelope (desc 2) already serves the volume target, so it
        // cannot also serve pitch; the pitch envelope (desc 1) can.
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, env_in));
        const Sculptor::EndPoint pitch_env_out = { mapping.params[1].env_node, mapping.env_output_slot };
        TEST(Sculptor::osc_graph_validate(&mapping, graph, pitch_env_out, env_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, lfo_out, lfo_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, input_out, src_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, input_out, depth_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, input_out, rate_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, osc_out, sum_in));
        // The same source -> same target pair validates twice.
        TEST(Sculptor::osc_graph_validate(&mapping, graph, input_out, src_in));
        TEST(Sculptor::osc_graph_validate(&mapping, graph, input_out, src_in));
        // Everything outside the allowed set is refused. These two
        // parameters are derived (uid == 0), so even a cross-target drop
        // that would retarget a record-backed parameter is refused for them.
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, volume_out, pitch_row)); // derived, cross-target
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, pitch_out, volume_row)); // derived, cross-target
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, lfo_out, env_in));       // lfo into an envelope input
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, lfo_in));       // envelope into an LFO input
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, src_in));       // envelope into a source input
        TEST(! Sculptor::osc_graph_validate(&mapping,
                                            graph,
                                            input_out,
                                            volume_row));                         // sources never reach oscillator rows
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, input_out, sum_in)); // inputs never reach the sum directly
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, sum_in));
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, osc_out, env_in)); // oscillators feed only the sum
        // Installed as the widget validator: a grammar refusal reports through
        // the error overlay, a grammatically valid pair connects.
        graph.set_validator(Sculptor::osc_graph_validate, &mapping);
        TEST(! graph.attempt_connection(env_out, lfo_in));
        TEST(graph.has_error());
        graph.dismiss_error();
        TEST(graph.attempt_connection(input_out, depth_in));
    }

    // Descriptor-content property edits write through to the bank pool entry
    // named by the node's desc id; instances sharing a desc id edit the same
    // entry, so aliasing stays consistent and repeated edits are idempotent.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        uint16_t                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        // A second LFO instance on the same descriptor id (its depth source
        // differs, so the registry keeps two nodes).
        instrument.layers[0].gen[Synth::mod_pitch].lfo_desc_id      = lfo_id;
        instrument.layers[0].gen[Synth::mod_pitch].lfo_depth_source = Synth::ModSource::aftertouch;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, 1, 2, 2, 2);
        const uint32_t env_node = mapping.params[0].env_node;
        const uint32_t lfo_a    = mapping.params[0].lfo_node;
        const uint32_t lfo_b    = mapping.params[1].lfo_node;
        TEST(env_node != Sculptor::pool_no_slot && lfo_a != Sculptor::pool_no_slot && lfo_b != Sculptor::pool_no_slot);
        TEST(lfo_a != lfo_b);
        const int32_t inst_a = find_instance_by_node(mapping, lfo_a);
        const int32_t inst_b = find_instance_by_node(mapping, lfo_b);
        TEST(inst_a >= 0 && inst_b >= 0);
        if (inst_a >= 0 && inst_b >= 0) {
            TEST(mapping.detached[inst_a].desc_id == lfo_id);
            TEST(mapping.detached[inst_b].desc_id == lfo_id);
        }
        // Edits through either aliased node hit the same pool entry.
        uint32_t slot = 0;
        TEST(find_graph_slot(graph, lfo_a, "Period (ms)", &slot));
        Sculptor::PropertyValue edited = {};
        edited.integer                 = 400;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, lfo_a, slot, edited));
        TEST(bank.lfos.entries[lfo_id - 1].period_ms == 400);
        TEST(find_graph_slot(graph, lfo_b, "Period (ms)", &slot));
        edited.integer = 450;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, lfo_b, slot, edited));
        TEST(bank.lfos.entries[lfo_id - 1].period_ms == 450);
        // Repeating the edit through the first node is idempotent.
        const Synth::LFODescriptor saved_lfo = bank.lfos.entries[lfo_id - 1];
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, lfo_a, slot, edited));
        TEST(memcmp(&saved_lfo, &bank.lfos.entries[lfo_id - 1], sizeof(Synth::LFODescriptor)) == 0);
        // Envelope content through the pitch envelope instance (the volume
        // envelope pins its minimum to 0 while it serves the volume target).
        // "Value (min)" pins the effective top: moving the floor re-derives
        // the span so the max stays where it was.
        const uint32_t pitch_env_node = mapping.params[1].env_node;
        TEST(find_graph_slot(graph, pitch_env_node, "Value (min)", &slot));
        const float old_max = bank.envelopes.entries[shared_env_id - 1].min_value +
                              65535.0f * bank.envelopes.entries[shared_env_id - 1].min_max_delta;
        edited.real         = -0.75f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, pitch_env_node, slot, edited));
        TEST(bank.envelopes.entries[shared_env_id - 1].min_value == -0.75f);
        TEST(approx(bank.envelopes.entries[shared_env_id - 1].min_value +
                        65535.0f * bank.envelopes.entries[shared_env_id - 1].min_max_delta,
                    old_max,
                    1.0e-5f));
        // "Value (max)" is the effective value at full scale: the descriptor
        // stores the span (max - min) / 65535, so a full-scale point lands
        // exactly on the max instead of multiplying it by 65535.
        TEST(find_graph_slot(graph, pitch_env_node, "Value (max)", &slot));
        edited.real = 1.0f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, pitch_env_node, slot, edited));
        TEST(approx(bank.envelopes.entries[shared_env_id - 1].min_max_delta, 1.75f / 65535.0f, 1.0e-9f));
        TEST(approx(bank.envelopes.entries[shared_env_id - 1].min_value +
                        65535.0f * bank.envelopes.entries[shared_env_id - 1].min_max_delta,
                    1.0f,
                    1.0e-6f));
        // The volume envelope keeps its minimum: a nonzero floor is refused,
        // and 0 (already the value) applies as a no-op.
        TEST(find_graph_slot(graph, env_node, "Value (min)", &slot));
        edited.real = -0.75f;
        TEST(! Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot, edited));
        edited.real = 0.0f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot, edited));
        // Non-descriptor slots refuse.
        edited.integer = 1;
        TEST(! Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, mapping.osc_nodes[0], 0, edited));
    }

    // Wiring an envelope into the volume target forces the volume-envelope
    // shape on the shared descriptor: minimum 0 with the effective top
    // preserved, and the first and the last point at 0.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        Sculptor::GraphChange discard[8] = {};
        graph.take_changes(discard, 8);
        // Unwire the pitch envelope (desc 1, non-conforming: min -0.5, first
        // point 0x4000) so the test starts from a free descriptor.
        const uint32_t pitch_env_conn = find_osc_graph_connection(graph, { mapping.params[1].node_idx, 2 });
        TEST(pitch_env_conn != Sculptor::pool_no_slot);
        graph.delete_connection(pitch_env_conn);
        graph.take_changes(discard, 8);
        TEST(graph.add_connection(Sculptor::EndPoint{ mapping.params[1].env_node, mapping.env_output_slot },
                                  Sculptor::EndPoint{ mapping.params[0].node_idx, 2 }));
        Sculptor::GraphChange drained[8] = {};
        const uint32_t        n          = graph.take_changes(drained, 8);
        TEST(n == 1);
        TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, drained[0]));
        const Synth::EnvelopeDescriptor& converted = bank.bank.envelopes.entries[0];
        TEST(converted.min_value == 0.0f);
        // The effective top (min + 65535 * delta) is preserved through the conversion.
        TEST(approx(converted.min_value + 65535.0f * converted.min_max_delta, -0.5f + 65535.0f * 1.5f, 1.0e-2f));
        TEST(converted.points[0].value == 0 && converted.points[converted.num_points - 1].value == 0);
    }

    // A volume envelope keeps its effective top at or above its minimum: a
    // negative top is refused, a nonnegative one applies.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        uint16_t                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        uint32_t                slot_idx = 0;
        Sculptor::PropertyValue edited   = {};
        const uint32_t          env_node = mapping.params[0].env_node;
        TEST(find_graph_slot(graph, env_node, "Value (max)", &slot_idx));
        edited.real = -1.0f;
        TEST(! Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot_idx, edited));
        edited.real = 0.5f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot_idx, edited));
        TEST(approx(bank.envelopes.entries[volume_env_id - 1].min_value +
                        65535.0f * bank.envelopes.entries[volume_env_id - 1].min_max_delta,
                    0.5f,
                    1.0e-5f));
    }

    // Retargeting moves the attached envelope with the parameter: moving a
    // nonconforming envelope onto the volume target converts it, and moving a
    // volume envelope off volume is allowed.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        // The pitch parameter carries the nonconforming desc 1; retargeting it
        // onto the volume row converts the descriptor.
        TEST(Sculptor::retarget_param(&bank,
                                      graph,
                                      mapping,
                                      0,
                                      0,
                                      1,
                                      static_cast<uint32_t>(Synth::mod_volume))); // projected volume row
        const Synth::EnvelopeDescriptor& converted = bank.bank.envelopes.entries[0];
        TEST(converted.min_value == 0.0f);
        TEST(approx(converted.min_value + 65535.0f * converted.min_max_delta, -0.5f + 65535.0f * 1.5f, 1.0e-2f));
        TEST(converted.points[0].value == 0 && converted.points[converted.num_points - 1].value == 0);
        // Moving the volume parameter (desc 2) onto the pitch row is allowed;
        // the descriptor keeps its shape.
        TEST(Sculptor::retarget_param(&bank,
                                      graph,
                                      mapping,
                                      0,
                                      0,
                                      0,
                                      static_cast<uint32_t>(Synth::mod_pitch))); // projected pitch row
    }

    // Descriptor ids are bank-wide: a descriptor wired to volume in one
    // instrument conflicts with a non-volume use in another, visible through
    // the same usage scan.
    {
        Synth::InstrumentBank bank = {};
        TEST(bank.envelopes.allocate() == 0);
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[0];
        env.num_points                 = 2;
        env.min_value                  = 0.0f;
        env.min_max_delta              = 1.0f / 65535.0f;
        env.points[0]                  = { 0, 0 };
        env.points[1]                  = { 100, 0xFFFF };
        TEST(bank.instruments.allocate() == 0);
        TEST(bank.instruments.allocate() == 1);
        bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id = 1;
        bank.instruments.entries[1].layers[0].gen[Synth::mod_pitch].envelope_desc_id  = 1;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(bank.instruments.entries[1], bank, &graph, &mapping));
        bool volume_used = false;
        bool other_used  = false;
        Sculptor::env_target_usage(graph, mapping, 1, &volume_used, &other_used);
        TEST(volume_used && other_used);
        // Wiring the shared descriptor into instrument 1's volume row is refused.
        const Sculptor::EndPoint env_out      = { mapping.params[1].env_node, mapping.env_output_slot };
        const Sculptor::EndPoint volume_input = { mapping.params[0].node_idx, 2 };
        TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, volume_input));
    }
    // An envelope whose output is unwired cannot affect the sound: its
    // curve editor goes read-only (the widget checks the output wire).
    // Row-level: a parameter row's knobs are inert exactly when that row's
    // input is unconnected.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        const uint32_t env_slot = bank.bank.envelopes.allocate();
        TEST(env_slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank.bank.envelopes.entries[env_slot];
        env.num_points                 = 2;
        env.min_value                  = -1.0f;
        env.min_max_delta              = 2.0f / 65535.0f;
        env.points[0]                  = { 0, 0 };
        env.points[1]                  = { 100, 0xFFFF };
        add_detached_record(&bank,
                            0,
                            0,
                            1,
                            env_slot + 1,
                            50.0f,
                            60.0f,
                            Synth::ModSource::none,
                            Synth::ModSource::none,
                            8);
        const uint16_t lfo_desc = add_detached_lfo_descriptor(&bank);
        add_detached_record(&bank, 0, 0, 2, lfo_desc, 70.0f, 80.0f, Synth::ModSource::none, Synth::ModSource::none, 9);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 5); // two derived envs + derived LFO + two surplus
        // Checks projected envelope wiring only: the widget derives its
        // read-only decision from the envelope's output wire, but the
        // ImGui-side application of that decision is not unit-testable.
        TEST(graph.slot_is_connected(mapping.detached[0].node_idx, mapping.env_output_slot));
        TEST(graph.slot_is_connected(mapping.detached[1].node_idx, mapping.env_output_slot));
        TEST(! graph.slot_is_connected(mapping.detached[3].node_idx, mapping.env_output_slot));
        // Row level: a parameter row's knobs are inert exactly when that
        // row's input is unconnected.  Param slot layout: 4..6 LFO row
        // knobs (op, depth, rate scale), 8..9 source 0 (op, scale),
        // 11..12 source 1.
        uint32_t volume_param = pool_no_slot;
        uint32_t pitch_param  = pool_no_slot;
        for (uint32_t p = 0; p < mapping.param_count; ++p) {
            if (mapping.params[p].target == static_cast<uint32_t>(Synth::mod_volume)) {
                volume_param = mapping.params[p].node_idx;
            }
            if (mapping.params[p].target == static_cast<uint32_t>(Synth::mod_pitch)) {
                pitch_param = mapping.params[p].node_idx;
            }
        }
        TEST(volume_param != pool_no_slot && pitch_param != pool_no_slot);
        // The fixture binds the LFO to volume and routes velocity into
        // volume's first source row; every other row input is unconnected.
        for (uint32_t slot_idx = 4; slot_idx <= 6; ++slot_idx) {
            TEST(! graph.slot_edit_disabled(volume_param, slot_idx));
            TEST(graph.slot_edit_disabled(pitch_param, slot_idx));
        }
        TEST(! graph.slot_edit_disabled(volume_param, 8) && ! graph.slot_edit_disabled(volume_param, 9));
        TEST(graph.slot_edit_disabled(pitch_param, 8) && graph.slot_edit_disabled(pitch_param, 9));
        TEST(graph.slot_edit_disabled(volume_param, 11) && graph.slot_edit_disabled(volume_param, 12));
        TEST(graph.slot_edit_disabled(pitch_param, 11) && graph.slot_edit_disabled(pitch_param, 12));
    }
    // Detaching the volume LFO leaves the parameter's LFO row unconnected:
    // its knobs grey out at the next projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        Synth::Oscillator& layer                                        = bank.bank.instruments.entries[0].layers[0];
        layer.gen[static_cast<uint32_t>(Synth::mod_volume)].lfo_desc_id = 0;
        layer.gen[static_cast<uint32_t>(Synth::mod_volume)].lfo_depth_source = Synth::ModSource::none;
        layer.gen[static_cast<uint32_t>(Synth::mod_volume)].lfo_rate_source  = Synth::ModSource::none;
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            1,
                            111.0f,
                            222.0f,
                            Synth::ModSource::velocity,
                            Synth::ModSource::mod_wheel,
                            1);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        uint32_t volume_param = pool_no_slot;
        for (uint32_t p = 0; p < mapping.param_count; ++p) {
            if (mapping.params[p].target == static_cast<uint32_t>(Synth::mod_volume)) {
                volume_param = mapping.params[p].node_idx;
            }
        }
        TEST(volume_param != pool_no_slot);
        for (uint32_t slot_idx = 4; slot_idx <= 6; ++slot_idx) {
            TEST(graph.slot_edit_disabled(volume_param, slot_idx));
        }
        TEST(! graph.slot_edit_disabled(volume_param, 8) && ! graph.slot_edit_disabled(volume_param, 9));
    }

    // Projection refuses instruments it cannot express: a routing input with
    // the none source would change synthesis on commit if silently dropped
    // (the eval multiplies by the zero sentinel), so the refusal is visible
    // and leaves the graph, mapping and bank untouched.
    {
        static Synth::InstrumentBank bank;
        static Synth::InstrumentBank bank_image;
        static Synth::Instrument     good          = {};
        good.layer_count                           = 1;
        good.routing[Synth::mod_volume].num_inputs = 1;
        good.routing[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity, Synth::SourceOp::multiply, 0.5f };

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(good, bank, &graph, &mapping));
        TEST(mapping.output_node != Sculptor::pool_no_slot);

        static Synth::Instrument bad                    = good;
        bad.routing[Synth::mod_volume].inputs[0].source = Synth::ModSource::none;
        bank_image                                      = bank;
        const uint32_t connections_before               = graph.connection_count();
        const uint32_t output_before                    = mapping.output_node;

        TEST(! Sculptor::project_instrument_to_graph(bad, bank, &graph, &mapping));
        TEST(graph.has_error());
        TEST(graph.connection_count() == connections_before);
        TEST(mapping.output_node == output_before);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);
    }

    // Compile resolves generator bindings from the live edges into each
    // parameter, not from projection-time state: deleting an edge unbinds the
    // wired cells, retargeting an edge moves the binding, and a bound LFO with
    // no depth edge yields lfo_depth_source none.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        uint16_t                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        static Synth::Instrument         compiled;

        // (a) Deleting the volume envelope edge unbinds volume only.
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        static Synth::Instrument expected                          = instrument;
        expected.layers[0].gen[Synth::mod_volume].envelope_desc_id = 0;
        const uint32_t env_conn = find_osc_graph_connection(graph, { mapping.params[0].node_idx, 2 });
        TEST(env_conn != Sculptor::pool_no_slot);
        graph.delete_connection(env_conn);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);

        // (b) Retargeting the volume envelope to pitch moves the binding:
        // pitch bound, volume unbound, bit-identical to a model built that way.
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        expected                                                   = instrument;
        expected.layers[0].gen[Synth::mod_volume].envelope_desc_id = 0;
        const uint32_t pitch_env_conn = find_osc_graph_connection(graph, { mapping.params[1].node_idx, 2 });
        TEST(pitch_env_conn != Sculptor::pool_no_slot);
        graph.delete_connection(pitch_env_conn);
        const uint32_t volume_env_conn = find_osc_graph_connection(graph, { mapping.params[0].node_idx, 2 });
        TEST(volume_env_conn != Sculptor::pool_no_slot);
        TEST(graph.move_connection_end(volume_env_conn, false, { mapping.params[1].node_idx, 2 }));
        expected.layers[0].gen[Synth::mod_pitch].envelope_desc_id = volume_env_id;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);

        // (c) A bound LFO with no depth edge yields lfo_depth_source none.
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::channel_pressure;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        expected                                                   = instrument;
        expected.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::none;
        const uint32_t depth_conn =
            find_osc_graph_connection(graph, { mapping.params[0].lfo_node, mapping.lfo_depth_input_slot });
        TEST(depth_conn != Sculptor::pool_no_slot);
        graph.delete_connection(depth_conn);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);
    }

    // Detached lifecycle: the editor projection places free-standing generator
    // instances from the bank's kind-1/2 records, content edits write through
    // to the shared descriptor pool entry, stored LFO sources re-route on
    // re-projection, and re-binding moves the record back onto the derived
    // instance instead of duplicating it.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        const uint16_t         lfo_desc    = instrument.layers[0].gen[Synth::mod_volume].lfo_desc_id;
        const Synth::ModSource saved_depth = instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source;
        const Synth::ModSource saved_rate  = instrument.layers[0].gen[Synth::mod_volume].lfo_rate_source;
        // Detach the volume LFO: clear the binding, store a detached record
        // with its own position, uid and LFO sources.
        instrument.layers[0].gen[Synth::mod_volume].lfo_desc_id      = 0;
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::none;
        instrument.layers[0].gen[Synth::mod_volume].lfo_rate_source  = Synth::ModSource::none;
        bank.bank.instruments.entries[0]                             = instrument;
        const uint16_t desc_id                                       = add_detached_lfo_descriptor(&bank);
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            desc_id,
                            111.0f,
                            222.0f,
                            Synth::ModSource::velocity,
                            Synth::ModSource::mod_wheel,
                            1);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 3); // two derived envelopes + surplus LFO
        TEST(mapping.detached[2].kind == 2);
        TEST(mapping.detached[2].desc_id == desc_id);
        TEST(mapping.detached[2].uid == 1);
        TEST(static_cast<Synth::ModSource>(mapping.detached[2].depth_source) == Synth::ModSource::velocity);
        TEST(static_cast<Synth::ModSource>(mapping.detached[2].rate_source) == Synth::ModSource::mod_wheel);
        const uint32_t node = mapping.detached[2].node_idx;
        TEST(node != Sculptor::pool_no_slot);
        TEST(graph.node(node).position.x == 111.0f && graph.node(node).position.y == 222.0f);
        uint32_t slot = 0;
        TEST(find_graph_slot(graph, node, "Period (ms)", &slot));
        TEST(graph.node(node).slots.entries[slot].value.integer == 300);
        // Depth/rate source edges follow the record's stored sources; there is
        // no parameter wire because the node is free-standing.
        const uint32_t velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
        const uint32_t wheel_idx    = static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1;
        const uint32_t depth_conn   = find_osc_graph_connection(graph, { node, mapping.lfo_depth_input_slot });
        TEST(depth_conn != Sculptor::pool_no_slot);
        if (depth_conn != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(depth_conn).output.slot_idx == mapping.input_source_slots[velocity_idx]);
        }
        const uint32_t rate_conn = find_osc_graph_connection(graph, { node, mapping.lfo_rate_input_slot });
        TEST(rate_conn != Sculptor::pool_no_slot);
        if (rate_conn != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(rate_conn).output.slot_idx == mapping.input_source_slots[wheel_idx]);
        }
        TEST(find_osc_graph_connection(graph, { mapping.params[0].node_idx, 3 }) == Sculptor::pool_no_slot);
        TEST(find_osc_graph_connection(graph, { mapping.params[1].node_idx, 3 }) == Sculptor::pool_no_slot);
        // Content edits write through to the shared pool entry.
        Sculptor::PropertyValue edited = {};
        edited.integer                 = 400;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank.bank, node, slot, edited));
        TEST(bank.bank.lfos.entries[desc_id - 1].period_ms == 400);
        // Editing the record's stored sources re-routes on re-projection.
        bank.graph_layout[0].depth_source = static_cast<uint8_t>(Synth::ModSource::aftertouch);
        bank.graph_layout[0].rate_source  = static_cast<uint8_t>(Synth::ModSource::none);
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        const uint32_t rerouted = mapping.detached[2].node_idx;
        TEST(rerouted != Sculptor::pool_no_slot);
        const uint32_t touch_idx = static_cast<uint32_t>(Synth::ModSource::aftertouch) - 1;
        const uint32_t depth2    = find_osc_graph_connection(graph, { rerouted, mapping.lfo_depth_input_slot });
        TEST(depth2 != Sculptor::pool_no_slot);
        if (depth2 != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(depth2).output.slot_idx == mapping.input_source_slots[touch_idx]);
        }
        TEST(find_osc_graph_connection(graph, { rerouted, mapping.lfo_rate_input_slot }) == Sculptor::pool_no_slot);
        // Re-attach: restore the binding; the record is consumed and the
        // derived instance carries the LFO again.
        instrument.layers[0].gen[Synth::mod_volume].lfo_desc_id      = lfo_desc;
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = saved_depth;
        instrument.layers[0].gen[Synth::mod_volume].lfo_rate_source  = saved_rate;
        bank.bank.instruments.entries[0]                             = instrument;
        bank.graph_layout_count                                      = 0;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 3);
        TEST(mapping.params[0].lfo_node != Sculptor::pool_no_slot);
        const int32_t lfo_inst = find_instance_by_node(mapping, mapping.params[0].lfo_node);
        TEST(lfo_inst >= 0 && mapping.detached[lfo_inst].desc_id == lfo_desc);
        TEST(find_osc_graph_connection(graph, { mapping.params[0].node_idx, 3 }) != Sculptor::pool_no_slot);
        // The fixture's LFO binding carries no depth/rate sources, so the
        // re-attached instance has no source edges.
        TEST(find_osc_graph_connection(graph, { mapping.params[0].lfo_node, mapping.lfo_depth_input_slot }) ==
             Sculptor::pool_no_slot);
        // Disconnect again: the record returns with a fresh uid and position.
        instrument.layers[0].gen[Synth::mod_volume].lfo_desc_id      = 0;
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::none;
        instrument.layers[0].gen[Synth::mod_volume].lfo_rate_source  = Synth::ModSource::none;
        bank.bank.instruments.entries[0]                             = instrument;
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            desc_id,
                            111.0f,
                            222.0f,
                            Synth::ModSource::velocity,
                            Synth::ModSource::mod_wheel,
                            2);
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 3);
        TEST(mapping.detached[2].uid == 2);
        TEST(graph.node(mapping.detached[2].node_idx).position.x == 111.0f);
        // Delete: dropping the record frees the descriptor at reclamation.
        bank.graph_layout_count = 0;
        Synth::reclaim_unused_slots(&bank);
        TEST(! bank.bank.lfos.is_occupied(desc_id - 1));
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 2);
    }

    // Aliased detached instances: two detached LFOs share one descriptor id but
    // keep their own positions and sources; the pair survives save/load and
    // re-projection, a content edit through either node hits the shared entry,
    // and re-binding one leaves the other free-standing.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        const uint16_t desc_id = add_detached_lfo_descriptor(&bank);
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            desc_id,
                            10.0f,
                            20.0f,
                            Synth::ModSource::velocity,
                            Synth::ModSource::mod_wheel,
                            1);
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            desc_id,
                            30.0f,
                            40.0f,
                            Synth::ModSource::aftertouch,
                            Synth::ModSource::none,
                            2);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 5); // two derived envs + derived LFO + two surplus LFOs
        const uint32_t node_a = mapping.detached[3].node_idx;
        const uint32_t node_b = mapping.detached[4].node_idx;
        TEST(node_a != Sculptor::pool_no_slot && node_b != Sculptor::pool_no_slot && node_a != node_b);
        TEST(graph.node(node_a).position.x == 10.0f && graph.node(node_a).position.y == 20.0f);
        TEST(graph.node(node_b).position.x == 30.0f && graph.node(node_b).position.y == 40.0f);
        // A content edit through either node hits the shared entry.
        uint32_t slot = 0;
        TEST(find_graph_slot(graph, node_a, "Period (ms)", &slot));
        Sculptor::PropertyValue edited = {};
        edited.integer                 = 450;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank.bank, node_a, slot, edited));
        TEST(bank.bank.lfos.entries[desc_id - 1].period_ms == 450);
        // The pair survives save/load byte-identically.
        static char                        editor_json[256 * 1024];
        static Synth::InstrumentEditorBank restored;
        const uint32_t written = Synth::encode_editor_bank_json(&bank, editor_json, sizeof(editor_json));
        TEST(written > 0);
        TEST(Synth::decode_editor_bank_json(editor_json, written, &restored));
        TEST(memcmp(bank.graph_layout, restored.graph_layout, sizeof(bank.graph_layout)) == 0);
        TEST(memcmp(bank.graph_missing_sum, restored.graph_missing_sum, sizeof(bank.graph_missing_sum)) == 0);
        TEST(Sculptor::project_editor_to_graph(restored, &graph, &mapping));
        TEST(mapping.detached_count == 5); // two derived envs + derived LFO + two surplus LFOs
        const uint32_t depth_a =
            find_osc_graph_connection(graph, { mapping.detached[3].node_idx, mapping.lfo_depth_input_slot });
        TEST(depth_a != Sculptor::pool_no_slot);
        if (depth_a != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(depth_a).output.slot_idx ==
                 mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1]);
        }
        const uint32_t depth_b =
            find_osc_graph_connection(graph, { mapping.detached[4].node_idx, mapping.lfo_depth_input_slot });
        TEST(depth_b != Sculptor::pool_no_slot);
        if (depth_b != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(depth_b).output.slot_idx ==
                 mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::aftertouch) - 1]);
        }
        TEST(find_osc_graph_connection(graph, { mapping.detached[4].node_idx, mapping.lfo_rate_input_slot }) ==
             Sculptor::pool_no_slot);
        // Reconnect one: the gen binding matching record A's sources consumes
        // that record; record B stays free-standing.
        instrument.layers[0].gen[Synth::mod_volume].lfo_desc_id      = desc_id;
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::velocity;
        instrument.layers[0].gen[Synth::mod_volume].lfo_rate_source  = Synth::ModSource::mod_wheel;
        restored.bank.instruments.entries[0]                         = instrument;
        restored.graph_layout[0]    = restored.graph_layout[1]; // drop record A, keep B
        restored.graph_layout_count = 1;
        TEST(Sculptor::project_editor_to_graph(restored, &graph, &mapping));
        TEST(mapping.detached_count == 4);
        TEST(mapping.detached[1].uid == 0); // derived instance for the binding
        TEST(mapping.detached[3].uid == 2); // record B still free-standing
        const uint32_t vol_lfo_conn = find_osc_graph_connection(graph, { mapping.params[0].node_idx, 3 });
        TEST(vol_lfo_conn != Sculptor::pool_no_slot);
        if (vol_lfo_conn != Sculptor::pool_no_slot) {
            TEST(graph.get_connection(vol_lfo_conn).output.node_idx == mapping.params[0].lfo_node);
        }
    }

    // Reclamation remaps kind-1/2 records' descriptor ids together
    // with the pool compaction, so detached nodes keep editing their content.
    {
        static Synth::InstrumentEditorBank bank;
        bank                         = {};
        bank.bank.channel_enabled[0] = 1;
        bank.bank.instruments.allocate();
        Synth::Instrument& instrument    = bank.bank.instruments.entries[0];
        instrument.layer_count           = 1;
        instrument.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
        // Envelope id 1 is bound to the instrument; id 2 is referenced only by
        // a detached record.
        const uint32_t env_a = bank.bank.envelopes.allocate();
        const uint32_t env_b = bank.bank.envelopes.allocate();
        TEST(env_a == 0 && env_b == 1);
        bank.bank.envelopes.entries[env_a].num_points                = 2;
        bank.bank.envelopes.entries[env_a].min_value                 = -0.5f;
        bank.bank.envelopes.entries[env_a].min_max_delta             = 1.0f;
        bank.bank.envelopes.entries[env_b]                           = bank.bank.envelopes.entries[env_a];
        bank.bank.envelopes.entries[env_b].min_value                 = -0.75f;
        instrument.layers[0].gen[Synth::mod_volume].envelope_desc_id = 1;
        bank.bank.channel_zones[0][0].start_note                     = 1;
        bank.bank.channel_zones[0][0].instrument                     = 0;
        add_detached_record(&bank, 0, 0, 1, 2, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        // Deleting the bound envelope leaves id 2 referenced only by the
        // detached record; reclamation keeps it (kept-id) and compacts it
        // into id 1, and the record's index must follow.
        bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id = 0;
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.envelopes.is_occupied(0));
        TEST(bank.bank.envelopes.entries[0].min_value == -0.75f);
        TEST(bank.graph_layout_count == 1);
        TEST(bank.graph_layout[0].kind == 1);
        TEST(bank.graph_layout[0].index == 1);
    }

    // Deleting a bound generator node clears every binding it served in the
    // same drained batch: the delete survives a re-projection instead of
    // resurrecting the node.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        Sculptor::GraphChange discard[8] = {};
        graph.take_changes(discard, 8);
        const uint32_t env_node = mapping.params[0].env_node;
        TEST(env_node != Sculptor::pool_no_slot);
        graph.delete_node(env_node);
        Sculptor::GraphChange drained[8] = {};
        const uint32_t        n          = graph.take_changes(drained, 8);
        TEST(n == 2); // one envelope wire + the node itself
        for (uint32_t i = 0; i < n; i++) {
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, drained[i]));
        }
        TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 0);
        TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].envelope_desc_id == 1);
        TEST(bank.bank.envelopes.is_occupied(0)); // the descriptor itself is untouched
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.params[0].env_node == Sculptor::pool_no_slot);
        TEST(mapping.params[1].env_node != Sculptor::pool_no_slot);
        TEST(mapping.detached_count == 2); // the pitch envelope + the LFO instance remain
        TEST(count_graph_nodes_named(graph, "Envelope 2") == 0);
    }

    // Disconnecting a bound generator keeps its single record: the detached
    // instance reuses it instead of appending a duplicate record key, and
    // reconnecting re-attaches to the same record.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        const Synth::Instrument original = instrument;
        add_detached_record(&bank, 0, 0, 1, 2, 100.0f, 40.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        Sculptor::GraphChange discard[8] = {};
        graph.take_changes(discard, 8);
        const uint32_t env_node = mapping.params[0].env_node; // the volume envelope owns desc 2
        TEST(env_node != Sculptor::pool_no_slot);
        TEST(graph.node(env_node).position.x == 100.0f); // the record attached
        // Disconnect both served parameters.
        const uint32_t conn_a = find_osc_graph_connection(graph, { mapping.params[0].node_idx, 2 });
        const uint32_t conn_b = find_osc_graph_connection(graph, { mapping.params[1].node_idx, 2 });
        TEST(conn_a != Sculptor::pool_no_slot && conn_b != Sculptor::pool_no_slot);
        graph.delete_connection(conn_a);
        graph.delete_connection(conn_b);
        Sculptor::GraphChange drained[8] = {};
        const uint32_t        n          = graph.take_changes(drained, 8);
        TEST(n == 2);
        for (uint32_t i = 0; i < n; i++) {
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, drained[i]));
        }
        // The record survives exactly once; no duplicate key was appended.
        TEST(count_records_matching(bank, 0, 0, 1, 2) == 1);
        TEST(Sculptor::validate_editor_metadata(bank));
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(count_records_matching(bank, 0, 0, 1, 2) == 1);
        TEST(mapping.detached_count == 3);
        uint32_t recorded = 0;
        for (uint32_t i = 0; i < mapping.detached_count; ++i) {
            if (mapping.detached[i].kind == 1 && mapping.detached[i].uid == 1) {
                recorded = mapping.detached[i].node_idx;
            }
        }
        TEST(recorded != 0);
        TEST(graph.node(recorded).position.x == 100.0f);
        // Reconnect: the binding re-attaches to the same record.
        bank.bank.instruments.entries[0] = original;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(count_records_matching(bank, 0, 0, 1, 2) == 1);
        recorded = 0;
        for (uint32_t i = 0; i < mapping.detached_count; ++i) {
            if (mapping.detached[i].kind == 1 && mapping.detached[i].uid == 1) {
                recorded = mapping.detached[i].node_idx;
            }
        }
        TEST(recorded != 0 && recorded == mapping.params[0].env_node);
    }

    // Free-standing instances resolve through the registry in the widget
    // validator, so connecting a detached LFO into a parameter's LFO input is
    // accepted while mismatches stay refused.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        const uint16_t lfo_desc = add_detached_lfo_descriptor(&bank);
        add_detached_record(&bank, 0, 0, 2, lfo_desc, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.detached_count == 4); // two derived envs + derived LFO + surplus LFO
        graph.set_validator(Sculptor::osc_graph_validate, &mapping);
        const Sculptor::EndPoint lfo_out = { mapping.detached[3].node_idx, mapping.lfo_output_slot };
        // The volume parameter's LFO input is bound in this fixture; the pitch
        // parameter's is free.
        TEST(graph.attempt_connection(lfo_out, { mapping.params[1].node_idx, 3 }));
        TEST(! graph.attempt_connection(lfo_out, { mapping.params[1].node_idx, 2 }));
    }

    // Applying the drained connection events maintains the zone's
    // missing-sum mask (osc->sum deleted sets the layer's bit, added clears it).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        const uint32_t sum_conn =
            find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[0] });
        TEST(sum_conn != Sculptor::pool_no_slot);

        static Sculptor::GraphChange change = {};
        change.kind                         = Sculptor::ChangeKind::connection_deleted;
        change.connection_idx               = sum_conn;
        // The event carries its endpoint pair: the pool slot may already be
        // freed or reused when the change is applied.
        change.connection_output = graph.get_connection(sum_conn).output;
        change.connection_input  = graph.get_connection(sum_conn).input;
        const bool applied       = Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change);
        TEST(applied);
        if (applied) {
            TEST((bank.graph_missing_sum[0][0] & static_cast<uint8_t>(1u << 0)) != 0);
            // Both unconnected endpoints of the broken edge carry the red mark.
            TEST(graph.slot_missing(mapping.output_node, mapping.output_layer_input_slot[0]));
            TEST(graph.slot_missing(mapping.osc_nodes[0], mapping.osc_output_slot));
        }
        change.kind          = Sculptor::ChangeKind::connection_added;
        const bool applied_2 = Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change);
        TEST(applied_2);
        if (applied_2) {
            TEST((bank.graph_missing_sum[0][0] & static_cast<uint8_t>(1u << 0)) == 0);
            TEST(! graph.slot_missing(mapping.osc_nodes[0], mapping.osc_output_slot));
        }
    }

    // The real delete path: deleting the sum edge through the widget frees the
    // pool slot before the event is drained, so the drained deletion must set
    // the layer's missing-sum bit from the event's endpoint pair alone.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        Sculptor::GraphChange discard[8] = {};
        (void)graph.take_changes(discard, 8);
        const uint32_t sum_conn =
            find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[0] });
        TEST(sum_conn != Sculptor::pool_no_slot);

        graph.delete_connection(sum_conn);
        Sculptor::GraphChange drained[8] = {};
        const uint32_t        drained_n  = graph.take_changes(drained, 8);
        TEST(drained_n == 1);
        TEST(drained[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(drained[0].connection_input.node_idx == mapping.output_node);
        TEST(drained[0].connection_input.slot_idx == mapping.output_layer_input_slot[0]);
        const bool applied = Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, drained[0]);
        TEST(applied);
        if (applied) {
            TEST((bank.graph_missing_sum[0][0] & static_cast<uint8_t>(1u << 0)) != 0);
        }
    }

    // Re-projection reproduces the broken state: the masked layer's sum
    // edge stays deleted and its input slot carries the missing (red) flag.
    {
        static Synth::InstrumentBank       model_bank;
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        uint16_t                           env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                           lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        bank.bank                    = model_bank;
        bank.bank.channel_enabled[0] = 1;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;

        // Capture the unmasked edge count first; the masked re-projection
        // must reproduce it minus exactly the deleted sum edge.
        static Sculptor::Graph           unmasked_graph;
        static Sculptor::OscGraphMapping unmasked_mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &unmasked_graph, &unmasked_mapping));
        const uint32_t full_edges = unmasked_graph.connection_count();
        TEST(full_edges > 0);

        bank.graph_missing_sum[0][0] = static_cast<uint8_t>(1u << 2);

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        const bool                       projected = Sculptor::project_editor_to_graph(bank, &graph, &mapping);
        TEST(projected);
        if (projected) {
            TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[2] }) ==
                 Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[0] }) !=
                 Sculptor::pool_no_slot);
            TEST(graph.connection_count() == full_edges - 1);
            TEST(graph.slot_missing(mapping.output_node, mapping.output_layer_input_slot[2]));
            TEST(! graph.slot_missing(mapping.output_node, mapping.output_layer_input_slot[0]));
        }
    }

    // Layer compaction shifts missing-sum bits together with layers.
    {
        static Synth::InstrumentEditorBank bank;
        bank                         = {};
        bank.graph_missing_sum[3][1] = static_cast<uint8_t>((1u << 1) | (1u << 3) | (1u << 5));
        Sculptor::compact_missing_sum_bits(&bank, 3, 1, 1);
        TEST(bank.graph_missing_sum[3][1] == static_cast<uint8_t>((1u << 2) | (1u << 4)));
    }

    // A missing-sum bit for a layer the zone instrument does not have marks a
    // sum input that does not exist; validation refuses it.  A bit within the
    // layer count stays valid.
    {
        static Synth::InstrumentEditorBank bank;
        bank                                = {};
        bank.bank.channel_enabled[0]        = 1;
        static Synth::Instrument instrument = {};
        instrument.layer_count              = 2;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        bank.graph_missing_sum[0][0]             = static_cast<uint8_t>(1u << 1);
        TEST(Sculptor::validate_editor_metadata(bank));
        bank.graph_missing_sum[0][0] = static_cast<uint8_t>(1u << 2);
        TEST(! Sculptor::validate_editor_metadata(bank));
        bank.graph_missing_sum[0][0] = 0;
        TEST(Sculptor::validate_editor_metadata(bank));
        // A full-layer instrument is not exempt: the highest in-range bit
        // stays valid and a stray bit beyond the last layer invalidates.
        bank.bank.instruments.entries[0].layer_count = Synth::max_layers;
        bank.graph_missing_sum[0][0]                 = static_cast<uint8_t>(1u << (Synth::max_layers - 1));
        TEST(Sculptor::validate_editor_metadata(bank));
        bank.graph_missing_sum[0][0] = static_cast<uint8_t>(1u << Synth::max_layers);
        TEST(! Sculptor::validate_editor_metadata(bank));
        bank.graph_missing_sum[0][0] = 0;
        TEST(Sculptor::validate_editor_metadata(bank));
    }

    // Publish masks the channel disabled while any zone has a missing-sum
    // bit; the stored channel_enabled value stays 1 and the channel re-enables
    // once every zone is complete again.
    {
        static Synth::InstrumentEditorBank bank;
        bank                                 = {};
        bank.bank.channel_enabled[0]         = 1;
        bank.bank.channel_enabled[1]         = 1;
        bank.graph_missing_sum[0][0]         = 1;
        uint8_t enabled[Synth::max_channels] = {};
        Sculptor::compute_publish_channel_enabled(bank, enabled);
        TEST(enabled[0] == 0);
        TEST(enabled[1] == 1);
        TEST(bank.bank.channel_enabled[0] == 1); // stored value untouched
        bank.graph_missing_sum[0][0] = 0;
        Sculptor::compute_publish_channel_enabled(bank, enabled);
        TEST(enabled[0] == 1);
    }

    // Zone-table metadata rules - split copies kind-0 records and the mask
    // row (never kind-1/2/3) after shifting later zones; drop and reset remove.
    {
        static Synth::InstrumentEditorBank bank;
        bank = {};
        add_detached_record(&bank, 0, 0, 0, 5, 1.0f, 2.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        add_detached_record(&bank, 0, 0, 2, 9, 3.0f, 4.0f, Synth::ModSource::velocity, Synth::ModSource::none, 1);
        add_detached_record(&bank, 0, 1, 0, 6, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        add_detached_record(&bank, 0, 2, 0, 7, 7.0f, 8.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        bank.graph_missing_sum[0][0] = 1;
        bank.graph_missing_sum[0][1] = 2;
        bank.graph_missing_sum[0][2] = 4;

        Sculptor::zone_records_split_copy(&bank, 0, 0);
        TEST(bank.graph_layout_count == 5);
        TEST(count_records_matching(bank, 0, 0, 0, 5) == 1);
        TEST(count_records_matching(bank, 0, 0, 2, 9) == 1); // detached stays in zone 0
        TEST(count_records_matching(bank, 0, 1, 0, 5) == 1); // copied kind-0 record
        TEST(count_records_matching(bank, 0, 1, 2, 9) == 0); // ... but not the detached one
        TEST(count_records_matching(bank, 0, 2, 0, 6) == 1); // shifted
        TEST(count_records_matching(bank, 0, 3, 0, 7) == 1);
        TEST(bank.graph_missing_sum[0][1] == 1); // copied mask row
        TEST(bank.graph_missing_sum[0][2] == 2); // shifted rows
        TEST(bank.graph_missing_sum[0][3] == 4);

        Sculptor::zone_records_drop_zone(&bank, 0, 1);
        TEST(bank.graph_layout_count == 4);
        TEST(count_records_matching(bank, 0, 1, 0, 5) == 0); // dropped
        TEST(count_records_matching(bank, 0, 1, 0, 6) == 1); // shifted down
        TEST(count_records_matching(bank, 0, 2, 0, 7) == 1);
        TEST(bank.graph_missing_sum[0][1] == 2);
        TEST(bank.graph_missing_sum[0][2] == 4);
        TEST(bank.graph_missing_sum[0][3] == 0);

        // A kind-4 record is a channel-wide effect layout, not zone state:
        // dropping a zone must neither delete nor renumber it.
        Synth::GraphNodeLayout& fx_record = bank.graph_layout[bank.graph_layout_count++];
        fx_record                         = {};
        fx_record.channel                 = 0;
        fx_record.zone                    = 0;
        fx_record.kind                    = 4;
        snprintf(fx_record.name, sizeof(fx_record.name), "Delay");
        fx_record.x = 320.0f;
        Sculptor::zone_records_drop_zone(&bank, 0, 0);
        TEST(bank.graph_layout_count == 3); // two shifted zone records + the fx record
        TEST(count_records_matching(bank, 0, 0, 0, 6) == 1);
        TEST(count_records_matching(bank, 0, 1, 0, 7) == 1);
        TEST(bank.graph_layout[2].kind == 4 && strcmp(bank.graph_layout[2].name, "Delay") == 0);
        TEST(bank.graph_layout[2].zone == 0);

        add_detached_record(&bank, 0, 2, 2, 3, 9.0f, 9.0f, Synth::ModSource::none, Synth::ModSource::none, 9);
        bank.graph_missing_sum[1][0] = 0x80;
        Sculptor::channel_records_reset(&bank, 0);
        TEST(bank.graph_layout_count == 0);
        TEST(bank.graph_missing_sum[0][0] == 0 && bank.graph_missing_sum[0][2] == 0);
        TEST(bank.graph_missing_sum[1][0] == 0x80); // other channels untouched
    }

    // Preflight arithmetic: the record-list capacity and detached-count
    // accounting the record-creating operations preflight against.
    {
        static Synth::InstrumentEditorBank bank;
        bank = {};
        TEST(Sculptor::graph_records_have_capacity(bank, 1));
        bank.graph_layout_count = Synth::max_graph_records;
        TEST(! Sculptor::graph_records_have_capacity(bank, 1));
        TEST(Sculptor::graph_records_have_capacity(bank, 0));
        bank.graph_layout_count = 0;
        for (uint32_t i = 0; i < 3; i++) {
            add_detached_record(&bank,
                                2,
                                0,
                                2,
                                1,
                                0.0f,
                                0.0f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                static_cast<uint8_t>(i + 1));
        }
        TEST(Sculptor::count_detached_records(bank, 2, 0) == 3);
        TEST(Sculptor::count_detached_records(bank, 2, 1) == 0);
    }

    // Node-capacity arithmetic: the implied projected node count the
    // Add Oscillator preflight and the JSON decode's overfull check use.
    {
        static Synth::InstrumentBank       model_bank;
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        uint16_t                           env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                           lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        bank.bank                    = model_bank;
        bank.bank.channel_enabled[0] = 1;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        TEST(Sculptor::count_projected_nodes(bank, 0, 0) == 114);

        static Synth::InstrumentEditorBank minimal;
        static Synth::Instrument           minimal_instrument = {};
        build_zone_fixture(&minimal, &minimal_instrument);
        TEST(Sculptor::count_projected_nodes(minimal, 0, 0) == 8);
    }

    // Undo group tags coalesce consecutive edits of one editable field.
    {
        Sculptor::UndoGroupState     state = {};
        const Sculptor::UndoGroupTag tag_a = { 1, 7, 0 };
        const Sculptor::UndoGroupTag tag_b = { 1, 8, 0 };
        TEST(Sculptor::undo_group_needs_snapshot(&state, tag_a)); // first edit snapshots
        TEST(! Sculptor::undo_group_needs_snapshot(&state, tag_a));
        TEST(Sculptor::undo_group_needs_snapshot(&state, tag_b)); // another field snapshots
        Sculptor::undo_group_reset(&state);
        TEST(Sculptor::undo_group_needs_snapshot(&state, tag_b)); // reset forces a snapshot
    }

    // The editor state round-trips through the bank JSON codec.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_zone_fixture(&bank, &instrument);
        add_detached_record(&bank, 0, 0, 0, 0, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        add_detached_record(&bank, 0, 0, 1, 1, 7.0f, 8.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
        const uint16_t lfo_desc = add_detached_lfo_descriptor(&bank);
        add_detached_record(&bank,
                            0,
                            0,
                            2,
                            lfo_desc,
                            9.0f,
                            10.0f,
                            Synth::ModSource::velocity,
                            Synth::ModSource::mod_wheel,
                            2);
        bank.graph_missing_sum[0][0] = 0x1;
        // A second zone with a two-layer instrument so the round trip covers
        // two nonzero mask rows; both bits stay within that layer count.
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1]             = Synth::Instrument{};
        bank.bank.instruments.entries[1].layer_count = 2;
        bank.bank.channel_zones[1][0].start_note     = 1;
        bank.bank.channel_zones[1][0].instrument     = 1;
        bank.graph_missing_sum[1][0]                 = 0x3;

        static char    doc[256 * 1024];
        const uint32_t len = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0 && len < sizeof(doc));
        static Synth::InstrumentEditorBank restored;
        TEST(Synth::decode_editor_bank_json(doc, len, &restored));
        TEST(restored.graph_layout_count == 3);
        TEST(memcmp(restored.graph_layout, bank.graph_layout, sizeof(bank.graph_layout)) == 0);
        TEST(memcmp(restored.graph_missing_sum, bank.graph_missing_sum, sizeof(bank.graph_missing_sum)) == 0);
    }

    // The decode refuses malformed editor metadata (records with
    // out-of-range bytes, over-cap counts, overfull implied projections) and
    // drops duplicate record keys.
    {
        static Synth::InstrumentEditorBank bank;
        Synth::init_default_bank(&bank.bank);
        // The detached-layout bounds tests need one descriptor in the pool:
        // ids at the allocated bound decode, one past it refuse.
        TEST(bank.bank.lfos.allocate() == 0);
        bank.bank.lfos.entries[0].wave      = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[0].period_ms = 50;
        static Synth::InstrumentEditorBank restored;
        static char                        editor_json[256 * 1024];

        // Kind byte past the four record kinds.
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 4, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));

        // A kind-0 record is keyed by canonical index alone: a nonzero uid
        // would name the same bound node twice.
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 0, 5, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));

        // Kind-0 index past the legacy canonical generator range (new files
        // key kind 0 by the fixed nodes only; old files decode for migration).
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 0, 84, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));

        // Detached descriptor id outside the pool's allocated range (the
        // test bank's LFO pool holds 1 descriptor).
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 2, 4, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 2, 2, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 2, 1, 0, 0);
        TEST(decode_bank_with_editor_section(bank, editor_json, &restored));

        // Source bytes past pressure_combine would index past the input nodes.
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 2, 1, 7, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));

        // Over-cap counts: 121 detached records in one zone (per-zone cap 120)
        // and 1320 records over 11 zones (global cap 1280).
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 121, 2, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));
        build_detached_layouts_json(editor_json, sizeof(editor_json), 11, 1320, 2, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));

        // Duplicate record keys drop the later record.
        {
            static char two_records[1024];
            snprintf(
                two_records,
                sizeof(two_records),
                "{\"layouts\":["
                "{\"channel\":0,\"zone\":0,\"kind\":2,\"index\":1,\"x\":1.0,\"y\":2.0,\"width\":0.0,\"height\":0.0,\"depth_source\":0,\"rate_source\":0,\"uid\":7},"
                "{\"channel\":0,\"zone\":0,\"kind\":2,\"index\":1,\"x\":3.0,\"y\":4.0,\"width\":0.0,\"height\":0.0,\"depth_source\":0,\"rate_source\":0,\"uid\":7}]}");
            const bool decoded = decode_bank_with_editor_section(bank, two_records, &restored);
            TEST(decoded);
            if (decoded) {
                TEST(restored.graph_layout_count == 1);
                TEST(restored.graph_layout[0].x == 1.0f);
            }
        }

        // Overfull implied node projection: the max fixture implies 114 nodes;
        // 15 detached records push it past 128, 14 land exactly on 128.
        static Synth::InstrumentBank       model_bank;
        static Synth::InstrumentEditorBank max_bank;
        static Synth::Instrument           instrument = {};
        uint16_t                           env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                           lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        max_bank.bank                    = model_bank;
        max_bank.bank.channel_enabled[0] = 1;
        TEST(max_bank.bank.instruments.allocate() == 0);
        max_bank.bank.instruments.entries[0]         = instrument;
        max_bank.bank.channel_zones[0][0].start_note = 1;
        max_bank.bank.channel_zones[0][0].instrument = 0;
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 15, 2, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(max_bank, editor_json, &restored));
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 14, 2, 1, 0, 0);
        TEST(decode_bank_with_editor_section(max_bank, editor_json, &restored));
    }

    // Shared-routing sync fans one shared slot's value out to every view of
    // the same routing field eventlessly; per-layer and per-binding slots are
    // refused.
    {
        static Synth::InstrumentBank bank;
        static Synth::Instrument     instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        uint16_t                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        instrument.layer_count            = 2;
        instrument.layers[1]              = instrument.layers[0];
        instrument.layers[1].pitch_offset = 3.75f; // per-layer contrast
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        uint32_t pitch_offset_slot = 0;
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "Pitch Offset", &pitch_offset_slot));
        uint32_t volume_slot = 0;
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "Volume", &volume_slot));
        Sculptor::PropertyValue edited = {};
        edited.real                    = 3.0f;
        TEST(! Sculptor::sync_osc_graph_shared_slot(&graph, mapping, mapping.osc_nodes[0], pitch_offset_slot, edited));
        TEST(graph.node(mapping.osc_nodes[1]).slots.entries[pitch_offset_slot].value.real == 3.75f);
        const bool synced =
            Sculptor::sync_osc_graph_shared_slot(&graph, mapping, mapping.osc_nodes[0], volume_slot, edited);
        TEST(synced);
        if (synced) {
            TEST(graph.node(mapping.osc_nodes[0]).slots.entries[volume_slot].value.real == 3.0f);
            TEST(graph.node(mapping.osc_nodes[1]).slots.entries[volume_slot].value.real == 3.0f);
            // Every view of the same routing field follows: the parameter's
            // value row carries the same base value.
            TEST(graph.node(mapping.params[0].node_idx).slots.entries[1].value.real == 3.0f);
            TEST(graph.node(mapping.params[1].node_idx).slots.entries[1].value.real == -2.0f); // other target untouched
            static Sculptor::GraphChange changes[4] = {};
            TEST(graph.take_changes(changes, 4) == 0); // eventless: no echo
        }
        // Syncing from a parameter view reaches the oscillator rows too.
        edited.real = 4.0f;
        const bool param_synced =
            Sculptor::sync_osc_graph_shared_slot(&graph, mapping, mapping.params[0].node_idx, 1, edited);
        TEST(param_synced);
        if (param_synced) {
            TEST(graph.node(mapping.osc_nodes[0]).slots.entries[volume_slot].value.real == 4.0f);
        }
        // Source op/scale rows are shared per target; the amount and rate-scale
        // rows are per binding and refused.
        edited.real = 1.5f;
        TEST(! Sculptor::sync_osc_graph_shared_slot(&graph, mapping, mapping.params[0].node_idx, 5, edited));
        const bool scale_synced =
            Sculptor::sync_osc_graph_shared_slot(&graph, mapping, mapping.params[0].node_idx, 9, edited);
        TEST(scale_synced);
        if (scale_synced) {
            TEST(graph.node(mapping.params[0].node_idx).slots.entries[9].value.real == 1.5f);
            TEST(graph.node(mapping.params[1].node_idx).slots.entries[9].value.real == 0.0f); // other target untouched
        }
    }

    // Parameter nodes: free-standing per-target modulation recipes wired to
    // oscillator target inputs.  Oscillators shrink to 15 rows with connectable
    // dynamic values and no direct generator connectors; a parameter wire is
    // the only thing that attaches gen bindings, and every view of a shared
    // routing field compiles to the same value.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            const bool projected = Sculptor::project_editor_to_graph(bank, &graph, &mapping);
            TEST(projected);
            if (! projected) {
                break;
            }
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8); // drain-and-discard re-arms the ring

            // Oscillators: exactly 15 rows, no input-side slots, no "base "
            // labels, and the five dynamic rows are connectable values.
            for (uint32_t layer = 0; layer < 3; layer++) {
                const Sculptor::Node& osc = graph.node(mapping.osc_nodes[layer]);
                TEST(osc.slots.num_allocated == 15);
                for (uint32_t s = 0; s < 15; s++) {
                    TEST(osc.slots.entries[s].kind != Sculptor::SlotKind::input);
                    TEST(strncmp(osc.slots.entries[s].name, "base ", 5) != 0);
                }
                for (uint32_t t = 0; t < 5; t++) {
                    TEST(osc.slots.entries[Sculptor::osc_target_row(t)].connectable);
                }
            }

            // One parameter per distinct binding tuple: "Volume" serves
            // layers 0 and 2, "Volume 2" serves layer 1, "Pitch" serves
            // layer 0. Parameters carry the six-row recipe (13 slots with
            // the row compaction).
            const uint32_t volume_param  = find_graph_node_by_name(graph, "Volume");
            const uint32_t second_volume = find_graph_node_by_name(graph, "Volume 2");
            const uint32_t pitch_param   = find_graph_node_by_name(graph, "Pitch");
            TEST(volume_param != Sculptor::pool_no_slot);
            TEST(second_volume != Sculptor::pool_no_slot);
            TEST(pitch_param != Sculptor::pool_no_slot);
            if (volume_param == Sculptor::pool_no_slot || second_volume == Sculptor::pool_no_slot ||
                pitch_param == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.node(volume_param).slots.num_allocated == 13);
            TEST(graph.node(second_volume).slots.num_allocated == 13);
            TEST(graph.node(pitch_param).slots.num_allocated == 13);

            // Each parameter fans out to exactly its served cells' target
            // inputs (osc row 1 = volume, row 2 = pitch).
            const uint32_t to_layer_0 = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 });
            const uint32_t to_layer_1 = find_osc_graph_connection(graph, { mapping.osc_nodes[1], 9 });
            const uint32_t to_layer_2 = find_osc_graph_connection(graph, { mapping.osc_nodes[2], 9 });
            TEST(to_layer_0 != Sculptor::pool_no_slot);
            TEST(to_layer_1 != Sculptor::pool_no_slot);
            TEST(to_layer_2 != Sculptor::pool_no_slot);
            if (to_layer_0 != Sculptor::pool_no_slot && to_layer_1 != Sculptor::pool_no_slot &&
                to_layer_2 != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(to_layer_0).output.node_idx == volume_param);
                TEST(graph.get_connection(to_layer_1).output.node_idx == second_volume);
                TEST(graph.get_connection(to_layer_2).output.node_idx == volume_param);
            }
            const uint32_t to_pitch = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 11 });
            TEST(to_pitch != Sculptor::pool_no_slot);
            if (to_pitch != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(to_pitch).output.node_idx == pitch_param);
            }
            TEST(find_osc_graph_connection(graph, { mapping.osc_nodes[1], 11 }) == Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, { mapping.osc_nodes[2], 11 }) == Sculptor::pool_no_slot);

            // Compile round-trips: served cells keep their binding tuple,
            // unwired inputs are constants, and instrument-wide routing
            // survives verbatim.
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
            TEST(compiled.layers[1].gen[Synth::mod_pitch].envelope_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_pitch].lfo_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_pitch].lfo_depth_source == Synth::ModSource::none);
            TEST(compiled.routing[Synth::mod_volume].base_value == 0.8f);
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 1);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].scale == 2.0f);

            // Shared routing edits via any view produce identical compiled
            // routing: a parameter value row and a (greyed) oscillator inline
            // are views of the same base_value storage.
            Sculptor::PropertyValue edited = {};
            edited.real                    = 0.25f;
            graph.set_slot_value(volume_param, 1, edited);
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.routing[Synth::mod_volume].base_value == 0.25f);
            graph.set_slot_value(mapping.osc_nodes[1], 9, edited);
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.routing[Synth::mod_volume].base_value == 0.25f);
            TEST(compiled.routing[Synth::mod_pitch].base_value == -2.0f); // other target untouched
            static Sculptor::GraphChange drained[4] = {};
            TEST(graph.take_changes(drained, 4) == 0); // eventless views
        } while (false);
    }

    // Grouping determinism: the same bank state projects the same parameter
    // node set and order on every projection, and the merge rule honors the
    // full binding tuple - two cells sharing one LFO descriptor but differing
    // in lfo_depth_source stay two parameters and compile with their own
    // per-cell source fields.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph_a;
        static Sculptor::Graph           graph_b;
        static Sculptor::OscGraphMapping mapping_a;
        static Sculptor::OscGraphMapping mapping_b;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph_a, &mapping_a));
            TEST(Sculptor::project_editor_to_graph(bank, &graph_b, &mapping_b));
            uint32_t occupied = 0;
            for (uint32_t i = 0; i < Sculptor::max_nodes; i++) {
                TEST(graph_a.node_occupied(i) == graph_b.node_occupied(i));
                if (graph_a.node_occupied(i)) {
                    occupied++;
                    TEST(strncmp(graph_a.node(i).name, graph_b.node(i).name, sizeof(graph_a.node(i).name)) == 0);
                }
            }
            TEST(occupied > 0);
            TEST(find_graph_node_by_name(graph_a, "Volume") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph_a, "Volume 2") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph_a, "Pitch") != Sculptor::pool_no_slot);

            // Same LFO descriptor, different depth source: no merge.
            instrument.layers[2].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::aftertouch;
            bank.bank.instruments.entries[0]                             = instrument;
            static Sculptor::Graph           graph_c;
            static Sculptor::OscGraphMapping mapping_c;
            TEST(Sculptor::project_editor_to_graph(bank, &graph_c, &mapping_c));
            TEST(count_graph_nodes_named(graph_c, "Volume") == 1);
            TEST(count_graph_nodes_named(graph_c, "Volume 2") == 1);
            TEST(count_graph_nodes_named(graph_c, "Volume 3") == 1);
            const uint32_t first = find_graph_node_by_name(graph_c, "Volume");
            const uint32_t third = find_graph_node_by_name(graph_c, "Volume 3");
            TEST(first != Sculptor::pool_no_slot && third != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot || third == Sculptor::pool_no_slot) {
                break;
            }
            TEST(find_osc_graph_connection(graph_c, { mapping_c.osc_nodes[0], 9 }) != Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph_c, { mapping_c.osc_nodes[2], 9 }) != Sculptor::pool_no_slot);
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph_c, mapping_c, &compiled));
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
            TEST(compiled.layers[1].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::aftertouch);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_desc_id == 1); // same descriptor
            TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        } while (false);
    }

    // Old banks migrate purely by re-derivation: a bank whose zone state is
    // only gen/routing (no parameter records) projects parameter nodes,
    // compiles byte-identically, and keeps routing.inputs verbatim.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        TEST(bank.graph_layout_count == 0); // everything derives from gen/routing
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(find_graph_node_by_name(graph, "Volume") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume 2") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Pitch") != Sculptor::pool_no_slot);
            if (find_graph_node_by_name(graph, "Volume") == Sculptor::pool_no_slot) {
                break;
            }
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 1);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].scale == 2.0f);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].op == Synth::SourceOp::add);
        } while (false);
    }

    // Source-edge mirroring: wiring a MIDI source into one same-target
    // parameter's source input defines the shared routing entry for all of
    // them; the sibling mirror is applied eventlessly at drain time, and the
    // compiled routing is identical whichever node was touched.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs = 0;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        static Synth::Instrument         compiled_a;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t first  = find_graph_node_by_name(graph, "Volume");
            const uint32_t second = find_graph_node_by_name(graph, "Volume 2");
            TEST(first != Sculptor::pool_no_slot && second != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot || second == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
            const Sculptor::EndPoint velocity_out = { mapping.input_node, mapping.input_source_slots[velocity_idx] };

            // Wire source 0 on the first parameter through the real event path.
            TEST(graph.add_connection(velocity_out, Sculptor::EndPoint{ first, 7 }) != Sculptor::pool_no_slot);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));

            // The sibling mirrored eventlessly: same source, no echo.
            const uint32_t mirror = find_osc_graph_connection(graph, { second, 7 });
            TEST(mirror != Sculptor::pool_no_slot);
            if (mirror != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(mirror).output.slot_idx == mapping.input_source_slots[velocity_idx]);
            }
            static Sculptor::GraphChange echo[4] = {};
            TEST(graph.take_changes(echo, 4) == 0);
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled_a));
            TEST(compiled_a.routing[Synth::mod_volume].num_inputs == 1);
            TEST(compiled_a.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(compiled_a.routing[Synth::mod_pitch].num_inputs == 0); // other target untouched

            // Wiring the sibling instead compiles to the same routing.
            build_parameter_fixture(&bank, &instrument);
            bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs = 0;
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t second_2 = find_graph_node_by_name(graph, "Volume 2");
            TEST(second_2 != Sculptor::pool_no_slot);
            if (second_2 == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.add_connection(velocity_out, Sculptor::EndPoint{ second_2, 7 }) != Sculptor::pool_no_slot);
            const uint32_t count_2 = graph.take_changes(changes, 8);
            TEST(count_2 == 1);
            if (count_2 != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            static Synth::Instrument compiled_b;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled_b));
            TEST(memcmp(&compiled_a, &compiled_b, sizeof(Synth::Instrument)) == 0);
        } while (false);
    }

    // LFO depth/rate sources: the bound LFO node keeps its MIDI-source
    // depth/rate wires and compile lands them in gen[l][t] for every served
    // cell; unwiring one served cell resets that cell's whole binding and
    // leaves the sibling untouched.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t volume_param = find_graph_node_by_name(graph, "Volume");
            TEST(volume_param != Sculptor::pool_no_slot);
            if (volume_param == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t lfo_conn = find_osc_graph_connection(graph, { volume_param, 3 });
            TEST(lfo_conn != Sculptor::pool_no_slot);
            if (lfo_conn == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t lfo_node = graph.get_connection(lfo_conn).output.node_idx;
            TEST(find_osc_graph_connection(graph, { lfo_node, mapping.lfo_depth_input_slot }) !=
                 Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, { lfo_node, mapping.lfo_rate_input_slot }) != Sculptor::pool_no_slot);

            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_rate_source == Synth::ModSource::mod_wheel);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_rate_source == Synth::ModSource::mod_wheel);

            const uint32_t wire = find_osc_graph_connection(graph, { mapping.osc_nodes[2], 9 });
            TEST(wire != Sculptor::pool_no_slot);
            if (wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(wire);
            static Sculptor::GraphChange changes[4] = {};
            const uint32_t               count      = graph.take_changes(changes, 4);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.layers[2].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::none);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_rate_source == Synth::ModSource::none);
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
        } while (false);
    }

    // Validator refusals: a parameter output only drives its own target's
    // oscillator rows, envelope and LFO inputs are not interchangeable, only
    // MIDI sources enter source inputs, and MIDI sources wire nowhere but
    // source rows and LFO depth/rate inputs.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t volume_param = find_graph_node_by_name(graph, "Volume");
            TEST(volume_param != Sculptor::pool_no_slot);
            if (volume_param == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t env_conn = find_osc_graph_connection(graph, { volume_param, 2 });
            const uint32_t lfo_conn = find_osc_graph_connection(graph, { volume_param, 3 });
            TEST(env_conn != Sculptor::pool_no_slot && lfo_conn != Sculptor::pool_no_slot);
            if (env_conn == Sculptor::pool_no_slot || lfo_conn == Sculptor::pool_no_slot) {
                break;
            }
            const Sculptor::EndPoint env_out      = graph.get_connection(env_conn).output;
            const Sculptor::EndPoint lfo_out      = graph.get_connection(lfo_conn).output;
            const Sculptor::EndPoint param_out    = { volume_param, 0 };
            const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
            const Sculptor::EndPoint velocity_out = { mapping.input_node, mapping.input_source_slots[velocity_idx] };

            TEST(! Sculptor::osc_graph_validate(&mapping, graph, param_out, { mapping.osc_nodes[0], 11 }));
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, lfo_out, { volume_param, 2 }));
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, { volume_param, 3 }));
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, env_out, { volume_param, 7 }));
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, velocity_out, { mapping.osc_nodes[0], 9 }));
        } while (false);
    }

    // Source-order refusal: wiring source 1 while source 0 is unwired on
    // the same parameter is refused with a specific message.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs = 0;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t first = find_graph_node_by_name(graph, "Volume");
            TEST(first != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot) {
                break;
            }
            TEST(strcmp(graph.node(first).name, "Volume") == 0);
            const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
            const Sculptor::EndPoint velocity_out = { mapping.input_node, mapping.input_source_slots[velocity_idx] };
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, velocity_out, Sculptor::EndPoint{ first, 10 }));
            TEST(graph.has_error());
            TEST(strcmp(graph.error_text(), "Wire source 0 first") == 0);
        } while (false);
    }

    // Deleting a parameter clears the gen bindings of the cells it served,
    // leaves sibling parameters and the instrument-wide routing untouched,
    // and removes its kind-3 record so it cannot resurrect on re-projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
        add_parameter_record(&bank, 0, 0, 0, 30.0f, 40.0f, 2);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t first  = find_graph_node_by_name(graph, "Volume");
            const uint32_t second = find_graph_node_by_name(graph, "Volume 2");
            TEST(first != Sculptor::pool_no_slot && second != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot || second == Sculptor::pool_no_slot) {
                break;
            }
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 2);

            graph.delete_node(first);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }

            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(count_graph_nodes_named(graph, "Volume") == 1);   // the sibling, renamed by order
            TEST(count_graph_nodes_named(graph, "Volume 2") == 0); // no resurrection
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 1);   // only the sibling's record
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.layers[0].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(compiled.layers[2].gen[Synth::mod_volume].lfo_desc_id == 0);
            TEST(compiled.layers[1].gen[Synth::mod_volume].envelope_desc_id == 2); // sibling untouched
            TEST(compiled.layers[1].gen[Synth::mod_volume].lfo_desc_id == 1);
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 1); // routing.inputs untouched
            TEST(compiled.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
        } while (false);
    }

    // Deleting the ONLY parameter of a target with MIDI inputs kills the
    // routing with it: the drained batch's store compiles zero inputs once no
    // serving parameter exists, and re-projection derives no carrier.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t sibling = find_graph_node_by_name(graph, "Volume 2");
            const uint32_t sole    = find_graph_node_by_name(graph, "Volume");
            TEST(sibling != Sculptor::pool_no_slot && sole != Sculptor::pool_no_slot);
            if (sibling == Sculptor::pool_no_slot || sole == Sculptor::pool_no_slot) {
                break;
            }
            TEST(strcmp(graph.node(sibling).name, "Volume 2") == 0);
            TEST(strcmp(graph.node(sole).name, "Volume") == 0);
            // Retire the sibling so "Volume" is the target's only parameter.
            graph.delete_node(sibling);
            static Sculptor::GraphChange changes[8] = {};
            uint32_t                     count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 1);
            // Deleting the last serving parameter zeroes the routing inputs.
            graph.delete_node(sole);
            count = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            // Re-projection derives no parameter for the target: no bound
            // cells and no MIDI inputs are left.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Volume") == Sculptor::pool_no_slot);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 0);
        } while (false);
    }
    // Deleting a derived parameter of a target with MIDI routing inputs
    // zeroes the routing and the gen bindings of every cell it served.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::InputRouting& pitch_routing = bank.bank.instruments.entries[0].routing[Synth::mod_pitch];
        pitch_routing.num_inputs           = 1;
        pitch_routing.inputs[0].source     = Synth::ModSource::velocity;
        pitch_routing.inputs[0].op         = Synth::SourceOp::add;
        pitch_routing.inputs[0].scale      = 1.0f;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t pitch = find_graph_node_by_name(graph, "Pitch");
            TEST(pitch != Sculptor::pool_no_slot);
            if (pitch == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_node(pitch);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_pitch].num_inputs == 0);
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                TEST(bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_pitch].envelope_desc_id == 0);
                TEST(bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_pitch].lfo_desc_id == 0);
            }
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Pitch") == Sculptor::pool_no_slot);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_pitch].num_inputs == 0);
        } while (false);
    }
    // Disconnecting the last serving parameter's VALUE wires detaches it into
    // a record-backed inert node: the routing inputs compile to zero, the gen
    // bindings clear, and re-projection re-materializes the parameter with no
    // wires at all (no value wires, no source wires).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t sibling = find_graph_node_by_name(graph, "Volume 2");
            const uint32_t sole    = find_graph_node_by_name(graph, "Volume");
            TEST(sibling != Sculptor::pool_no_slot && sole != Sculptor::pool_no_slot);
            if (sibling == Sculptor::pool_no_slot || sole == Sculptor::pool_no_slot) {
                break;
            }
            // Retire the sibling so "Volume" is the last serving parameter.
            graph.delete_node(sibling);
            static Sculptor::GraphChange changes[8] = {};
            uint32_t                     count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            // Disconnect both value wires (oscillator volume rows 1 <- "Volume").
            uint32_t value_wires = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx == sole && connection.output.slot_idx == 0) {
                    graph.delete_connection(c);
                    ++value_wires;
                }
            }
            TEST(value_wires == 2);
            count = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                TEST(bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume].envelope_desc_id == 0);
                TEST(bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume].lfo_desc_id == 0);
            }
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 1); // detached record
            // Re-projection: the record materializes an inert record-backed
            // parameter with no wires at all.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t inert = find_graph_node_by_name(graph, "Volume");
            TEST(inert != Sculptor::pool_no_slot);
            if (inert == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t outgoing    = 0;
            uint32_t value_rows  = 0;
            uint32_t source_rows = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx == inert) {
                    ++outgoing;
                }
                if (connection.input.node_idx == inert &&
                    (connection.input.slot_idx == 7 || connection.input.slot_idx == 10)) {
                    ++source_rows;
                }
                for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                    if (connection.input.node_idx == mapping.osc_nodes[layer] && connection.input.slot_idx == 1) {
                        ++value_rows;
                    }
                }
            }
            TEST(outgoing == 0);
            TEST(value_rows == 0);
            TEST(source_rows == 0);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
        } while (false);
    }
    // Disconnecting a MIDI SOURCE wire decrements the routing inputs and the
    // wire stays gone across re-projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t sole = find_graph_node_by_name(graph, "Volume");
            TEST(sole != Sculptor::pool_no_slot);
            if (sole == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t source_wire = find_osc_graph_connection(graph, Sculptor::EndPoint{ sole, 7 });
            TEST(source_wire != Sculptor::pool_no_slot);
            if (source_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(source_wire);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t reprojected = find_graph_node_by_name(graph, "Volume");
            TEST(reprojected != Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, Sculptor::EndPoint{ reprojected, 7 }) == Sculptor::pool_no_slot);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
        } while (false);
    }
    // Renaming a record-backed parameter (the model sequence behind the
    // node-menu Rename): the kind-3 record re-keys to the new target and the
    // value wires retarget onto the new rows, so the drained batch compiles
    // the source inputs into the NEW target's routing and clears the old
    // target's; re-projection rebuilds the node under its new name.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 5); // kind-3 volume record
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            // The record attaches to the first derived volume parameter.
            const uint32_t renamed = find_graph_node_by_name(graph, "Volume");
            TEST(renamed != Sculptor::pool_no_slot);
            if (renamed == Sculptor::pool_no_slot) {
                break;
            }
            int32_t entry = -1;
            for (uint32_t p = 0; p < mapping.param_count; ++p) {
                if (mapping.params[p].node_idx == renamed) {
                    entry = static_cast<int32_t>(p);
                }
            }
            TEST(entry >= 0 && mapping.params[entry].uid == 5);
            if (entry < 0 || mapping.params[entry].uid != 5) {
                break;
            }
            // Retire the sibling so the rename carries the whole volume routing.
            const uint32_t sibling = find_graph_node_by_name(graph, "Volume 2");
            TEST(sibling != Sculptor::pool_no_slot);
            if (sibling == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_node(sibling);
            static Sculptor::GraphChange changes[8] = {};
            uint32_t                     count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            // Re-key the record volume -> panning and move the mapping entry.
            int32_t record_idx = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.channel == 0 && record.zone == 0 && record.kind == 3 && record.index == 0 &&
                    record.uid == 5) {
                    record_idx = static_cast<int32_t>(r);
                    break;
                }
            }
            TEST(record_idx >= 0);
            if (record_idx < 0) {
                break;
            }
            bank.graph_layout[record_idx].index = 2;
            mapping.params[entry].target        = 2;
            // Retarget the wires onto the panning row.
            uint32_t moved_wires = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != renamed || connection.output.slot_idx != 0) {
                    continue;
                }
                TEST(graph.move_connection_end(
                    c,
                    false,
                    Sculptor::EndPoint{ connection.input.node_idx, Sculptor::osc_target_row(2) }));
                ++moved_wires;
            }
            TEST(moved_wires == 2);
            count = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_panning].num_inputs == 1);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_panning].inputs[0].source ==
                 Synth::ModSource::velocity);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_panning].envelope_desc_id == 1);
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 0);
            // Re-projection rebuilds the node under its new name with the
            // source wire attached to the new target's routing.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t panning = find_graph_node_by_name(graph, "Panning");
            TEST(panning != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume") == Sculptor::pool_no_slot);
            if (panning != Sculptor::pool_no_slot) {
                TEST(find_osc_graph_connection(graph, Sculptor::EndPoint{ panning, 7 }) != Sculptor::pool_no_slot);
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 0);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_panning].num_inputs == 1);
        } while (false);
    }
    // Renaming a record-backed parameter into a LOWER-indexed target that
    // already has a serving parameter with different sources: the
    // destination's routing is authoritative, so the renamed parameter's
    // source rows adopt the resident's wiring and the compiled routing keeps
    // the resident's sources; the old target's routing dies (it has no
    // serving parameter left).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::InputRouting& pitch_routing = instrument.routing[Synth::mod_pitch];
        pitch_routing.num_inputs           = 1;
        pitch_routing.inputs[0].source     = Synth::ModSource::mod_wheel;
        pitch_routing.inputs[0].op         = Synth::SourceOp::add;
        pitch_routing.inputs[0].scale      = 1.0f;
        bank.bank.instruments.entries[0]   = instrument;
        add_parameter_record(&bank, 0, 0, 1, 30.0f, 40.0f, 6); // kind-3 pitch record
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t volume_node = find_graph_node_by_name(graph, "Volume");
            const uint32_t pitch_node  = find_graph_node_by_name(graph, "Pitch");
            TEST(volume_node != Sculptor::pool_no_slot && pitch_node != Sculptor::pool_no_slot);
            if (volume_node == Sculptor::pool_no_slot || pitch_node == Sculptor::pool_no_slot) {
                break;
            }
            int32_t entry = -1;
            for (uint32_t p = 0; p < mapping.param_count; ++p) {
                if (mapping.params[p].node_idx == pitch_node) {
                    entry = static_cast<int32_t>(p);
                }
            }
            TEST(entry >= 0 && mapping.params[entry].uid == 6);
            if (entry < 0 || mapping.params[entry].uid != 6) {
                break;
            }
            // The renamed parameter's own routing differs from the resident's.
            const uint32_t wheel_idx = static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1;
            const uint32_t own_wire  = find_osc_graph_connection(graph, Sculptor::EndPoint{ pitch_node, 7 });
            TEST(own_wire != Sculptor::pool_no_slot);
            if (own_wire == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.get_connection(own_wire).output.slot_idx == mapping.input_source_slots[wheel_idx]);
            // Rename Pitch -> volume: re-key the record, flip the mapping
            // target, then retarget the value wire.  The volume row of layer 0
            // is wired by the resident, so the retarget is refused and the
            // wire is deleted (the refused edit is the visible loss).
            int32_t record_idx = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.channel == 0 && record.zone == 0 && record.kind == 3 && record.index == 1 &&
                    record.uid == 6) {
                    record_idx = static_cast<int32_t>(r);
                    break;
                }
            }
            TEST(record_idx >= 0);
            if (record_idx < 0) {
                break;
            }
            bank.graph_layout[record_idx].index = 0;
            mapping.params[entry].target        = 0;
            const uint32_t value_wire           = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 11 });
            TEST(value_wire != Sculptor::pool_no_slot);
            if (value_wire == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.get_connection(value_wire).output.node_idx == pitch_node);
            TEST(! graph.move_connection_end(value_wire, false, Sculptor::EndPoint{ mapping.osc_nodes[0], 9 }));
            graph.delete_connection(value_wire);
            // The eventless source-row adoption: the resident (Volume, the
            // first other serving parameter of the new target) carries the
            // destination's routing, so the renamed parameter's rows mirror
            // it (its own mod_wheel wiring is dropped).
            const uint32_t resident_wire = find_osc_graph_connection(graph, Sculptor::EndPoint{ volume_node, 7 });
            TEST(resident_wire != Sculptor::pool_no_slot);
            if (resident_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.set_slot_input(pitch_node, 7, graph.get_connection(resident_wire).output);
            graph.set_slot_input(pitch_node, 10, Sculptor::EndPoint{ Sculptor::pool_no_slot, Sculptor::pool_no_slot });
            static Sculptor::GraphChange changes[8] = {};
            uint32_t                     count      = graph.take_changes(changes, 8);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            // The compiled routing keeps the resident's sources and the old
            // target's routing died with its last serving parameter.
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 1);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].inputs[0].source ==
                 Synth::ModSource::velocity);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_pitch].num_inputs == 0);
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].envelope_desc_id == 0);
            // The renamed parameter's source rows now mirror the resident's
            // wiring (identical views, order-independent carrier pick).
            const uint32_t velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
            const uint32_t adopted      = find_osc_graph_connection(graph, Sculptor::EndPoint{ pitch_node, 7 });
            TEST(adopted != Sculptor::pool_no_slot);
            if (adopted != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(adopted).output.slot_idx == mapping.input_source_slots[velocity_idx]);
            }
            TEST(find_osc_graph_connection(graph, Sculptor::EndPoint{ pitch_node, 10 }) == Sculptor::pool_no_slot);
            // Re-projection: the old target is gone (no Pitch node, its
            // routing dead), and the re-keyed record attaches to the new
            // target's first derived parameter, whose views show the same
            // resident routing.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Pitch") == Sculptor::pool_no_slot);
            const uint32_t attached = find_graph_node_by_name(graph, "Volume");
            TEST(attached != Sculptor::pool_no_slot);
            if (attached != Sculptor::pool_no_slot) {
                const uint32_t attached_wire = find_osc_graph_connection(graph, Sculptor::EndPoint{ attached, 7 });
                TEST(attached_wire != Sculptor::pool_no_slot);
                if (attached_wire != Sculptor::pool_no_slot) {
                    TEST(graph.get_connection(attached_wire).output.slot_idx ==
                         mapping.input_source_slots[velocity_idx]);
                }
            }
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 1);
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_pitch].num_inputs == 0);
        } while (false);
    }
    // A delete_selected batch holding a non-last oscillator plus another
    // state-bearing node must not refuse: remove_osc_layer compacts the
    // mapping's oscillator run like the instrument's layers, so the later
    // events in the batch recompile against a gapless run instead of
    // failing the compile and resurrecting the deleted nodes.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        instrument.layers[1].osc_type[0] = Synth::WaveType::sine_wave;
        instrument.layers[2].osc_type[0] = Synth::WaveType::pulse_wave;
        bank.bank.instruments.entries[0] = instrument;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t osc_a   = mapping.osc_nodes[0];
            const uint32_t osc_b   = mapping.osc_nodes[1];
            const uint32_t osc_c   = mapping.osc_nodes[2];
            const uint32_t volume2 = find_graph_node_by_name(graph, "Volume 2");
            TEST(osc_a != Sculptor::pool_no_slot && osc_b != Sculptor::pool_no_slot &&
                 osc_c != Sculptor::pool_no_slot && volume2 != Sculptor::pool_no_slot);
            if (osc_a == Sculptor::pool_no_slot || volume2 == Sculptor::pool_no_slot) {
                break;
            }
            // One gesture: both deletions drain as a single batch.
            graph.delete_node(osc_a);
            graph.delete_node(volume2);
            static Sculptor::GraphChange changes[16] = {};
            const uint32_t               count       = graph.take_changes(changes, 16);
            TEST(count > 0);
            if (count == 0) {
                break;
            }
            bool ok = true;
            for (uint32_t i = 0; i < count; i++) {
                ok = Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]) && ok;
            }
            TEST(ok); // no refusal: the compacted run has no gap to reject
            if (! ok) {
                break;
            }
            // Layers compacted, the mapping run matches, no stale missing-sum
            // bit flags the survivor's intact edge.
            TEST(bank.bank.instruments.entries[0].layer_count == 2);
            TEST(bank.bank.instruments.entries[0].layers[0].osc_type[0] == Synth::WaveType::sine_wave);
            TEST(bank.bank.instruments.entries[0].layers[1].osc_type[0] == Synth::WaveType::pulse_wave);
            TEST(mapping.osc_nodes[0] == osc_b && mapping.osc_nodes[1] == osc_c);
            TEST(mapping.osc_nodes[2] == Sculptor::pool_no_slot);
            TEST(bank.graph_missing_sum[0][0] == 0);
            TEST(find_graph_node_by_name(graph, "Volume 2") == Sculptor::pool_no_slot);
            // Re-projection keeps the deletions: nothing resurrects.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(bank.bank.instruments.entries[0].layer_count == 2);
            TEST(count_graph_nodes_named(graph, "Volume") == 1);
            TEST(count_graph_nodes_named(graph, "Volume 2") == 0);
            TEST(mapping.osc_nodes[2] == Sculptor::pool_no_slot);
        } while (false);
    }
    // Two oscillators selected in one gesture: both layer removals apply in
    // one drained batch, and a wire that dies with the second oscillator must
    // not mark the survivor's intact sum edge missing.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        instrument.layers[1].osc_type[0] = Synth::WaveType::sine_wave;
        instrument.layers[2].osc_type[0] = Synth::WaveType::pulse_wave;
        bank.bank.instruments.entries[0] = instrument;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t osc_a = mapping.osc_nodes[0];
            const uint32_t osc_b = mapping.osc_nodes[1];
            const uint32_t osc_c = mapping.osc_nodes[2];
            TEST(osc_a != Sculptor::pool_no_slot && osc_b != Sculptor::pool_no_slot && osc_c != Sculptor::pool_no_slot);
            if (osc_c == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_node(osc_a);
            graph.delete_node(osc_b);
            static Sculptor::GraphChange changes[16] = {};
            const uint32_t               count       = graph.take_changes(changes, 16);
            TEST(count > 0);
            if (count == 0) {
                break;
            }
            bool ok = true;
            for (uint32_t i = 0; i < count; i++) {
                ok = Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]) && ok;
            }
            TEST(ok);
            if (! ok) {
                break;
            }
            TEST(bank.bank.instruments.entries[0].layer_count == 1);
            TEST(bank.bank.instruments.entries[0].layers[0].osc_type[0] == Synth::WaveType::pulse_wave);
            TEST(mapping.osc_nodes[0] == osc_c);
            TEST(mapping.osc_nodes[1] == Sculptor::pool_no_slot);
            TEST(mapping.osc_nodes[2] == Sculptor::pool_no_slot);
            TEST(bank.graph_missing_sum[0][0] == 0); // the survivor's sum edge is intact
            TEST(bank.bank.instruments.entries[0].routing[Synth::mod_volume].num_inputs == 1);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(bank.bank.instruments.entries[0].layer_count == 1);
            // Volume 2 lost its oscillator edge with b and detaches to a
            // kind-3 record. Its stored tuple (envelope 2) does not match
            // the surviving group's tuple, so the record keeps its own
            // identity and materializes a free-standing inert node instead
            // of renaming the survivor.
            TEST(count_graph_nodes_named(graph, "Volume") == 1);
            TEST(count_graph_nodes_named(graph, "Volume 2") == 1);
            TEST(mapping.osc_nodes[1] == Sculptor::pool_no_slot);
        } while (false);
    }

    // Retargeting a parameter wire from parameter A to parameter B rebinds
    // the served cells to B's binding tuple.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t second = find_graph_node_by_name(graph, "Volume 2");
            TEST(second != Sculptor::pool_no_slot);
            if (second == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t wire = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 });
            TEST(wire != Sculptor::pool_no_slot);
            if (wire == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.move_connection_end(wire, true, Sculptor::EndPoint{ second, 0 }));
            static Sculptor::GraphChange changes[4] = {};
            const uint32_t               count      = graph.take_changes(changes, 4);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            const uint32_t retargeted = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 });
            TEST(retargeted != Sculptor::pool_no_slot);
            if (retargeted != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(retargeted).output.node_idx == second);
            }
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.layers[0].gen[Synth::mod_volume].envelope_desc_id == 2); // B's tuple
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_desc_id == 1);
            TEST(compiled.layers[0].gen[Synth::mod_volume].lfo_depth_source == Synth::ModSource::velocity);
            TEST(compiled.layers[2].gen[Synth::mod_volume].envelope_desc_id == 1); // A keeps its cell
        } while (false);
    }

    // Retargeting a shared source wire OFF a parameter source row unmirrors
    // the siblings: the compiled routing loses the input exactly once and no
    // stale mirror keeps it alive.  Retargeting back INTO a source row
    // mirrors to the siblings again.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t first  = find_graph_node_by_name(graph, "Volume");
            const uint32_t second = find_graph_node_by_name(graph, "Volume 2");
            TEST(first != Sculptor::pool_no_slot && second != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot || second == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t src_wire = find_osc_graph_connection(graph, { first, 7 });
            const uint32_t lfo_conn = find_osc_graph_connection(graph, { first, 3 });
            TEST(src_wire != Sculptor::pool_no_slot && lfo_conn != Sculptor::pool_no_slot);
            if (src_wire == Sculptor::pool_no_slot || lfo_conn == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t lfo_node = graph.get_connection(lfo_conn).output.node_idx;
            const uint32_t velocity_slot =
                mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1];
            TEST(graph.get_connection(src_wire).output.slot_idx == velocity_slot);
            const uint32_t depth_slot = mapping.lfo_depth_input_slot;

            // Free the fixture's LFO depth input, then retarget the source-0
            // wire onto it.
            const uint32_t depth_wire = find_osc_graph_connection(graph, { lfo_node, depth_slot });
            TEST(depth_wire != Sculptor::pool_no_slot);
            if (depth_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(depth_wire);
            static Sculptor::GraphChange changes[8] = {};
            uint32_t                     count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }

            TEST(graph.move_connection_end(src_wire, false, Sculptor::EndPoint{ lfo_node, depth_slot }));
            count = graph.take_changes(changes, 8);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            TEST(find_osc_graph_connection(graph, { first, 7 }) == Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, { second, 7 }) == Sculptor::pool_no_slot); // no stale mirror
            const uint32_t moved = find_osc_graph_connection(graph, { lfo_node, depth_slot });
            TEST(moved != Sculptor::pool_no_slot);
            if (moved == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.get_connection(moved).output.slot_idx == velocity_slot);
            static Synth::Instrument compiled;
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 0);
            TEST(compiled.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::none);

            // Retargeting back INTO a source row mirrors to the siblings.
            TEST(graph.move_connection_end(moved, false, Sculptor::EndPoint{ second, 7 }));
            count = graph.take_changes(changes, 8);
            TEST(count == 1);
            if (count != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            TEST(find_osc_graph_connection(graph, { first, 7 }) != Sculptor::pool_no_slot);
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 1);
        } while (false);
    }

    // Kind-3 parameter records round-trip through the editor JSON section
    // with stable uids and dedup on the record key; old files without kind-3
    // records still load; stale kind-0 records over unbound cells are dropped
    // at migration while bound ones are re-keyed to their descriptor.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static char                        editor_json[1024];
        static Synth::InstrumentEditorBank decoded;
        do {
            snprintf(
                editor_json,
                sizeof(editor_json),
                "{\"layouts\":["
                "{\"channel\":0,\"zone\":0,\"kind\":3,\"index\":0,\"x\":10.0,\"y\":20.0,\"width\":0.0,\"height\":0.0,\"uid\":1},"
                "{\"channel\":0,\"zone\":0,\"kind\":3,\"index\":0,\"x\":30.0,\"y\":40.0,\"width\":0.0,\"height\":0.0,\"uid\":2},"
                "{\"channel\":0,\"zone\":0,\"kind\":3,\"index\":1,\"x\":50.0,\"y\":60.0,\"width\":0.0,\"height\":0.0,\"uid\":1}]}");
            const bool decoded_ok = decode_bank_with_editor_section(bank, editor_json, &decoded);
            TEST(decoded_ok); // kind 3 is a valid record kind
            if (! decoded_ok) {
                break;
            }
            TEST(decoded.graph_layout_count == 3);
            TEST(decoded.graph_layout[0].kind == 3 && decoded.graph_layout[0].index == 0);
            TEST(decoded.graph_layout[0].uid == 1);
            TEST(decoded.graph_layout[0].x == 10.0f && decoded.graph_layout[0].y == 20.0f);
            TEST(decoded.graph_layout[1].uid == 2);
            TEST(decoded.graph_layout[2].index == 1);

            // uid stability across a re-encode.
            static char    doc[512 * 1024];
            const uint32_t len = Synth::encode_editor_bank_json(&decoded, doc, sizeof(doc));
            TEST(len > 0);
            static Synth::InstrumentEditorBank again;
            TEST(Synth::decode_editor_bank_json(doc, len, &again));
            TEST(again.graph_layout_count == decoded.graph_layout_count);
            TEST(memcmp(decoded.graph_layout,
                        again.graph_layout,
                        decoded.graph_layout_count * sizeof(Synth::GraphNodeLayout)) == 0);

            // Duplicate record keys dedup: the later record is dropped.
            snprintf(
                editor_json,
                sizeof(editor_json),
                "{\"layouts\":["
                "{\"channel\":0,\"zone\":0,\"kind\":3,\"index\":0,\"x\":1.0,\"y\":2.0,\"width\":0.0,\"height\":0.0,\"uid\":5},"
                "{\"channel\":0,\"zone\":0,\"kind\":3,\"index\":0,\"x\":9.0,\"y\":9.0,\"width\":0.0,\"height\":0.0,\"uid\":5}]}");
            TEST(decode_bank_with_editor_section(bank, editor_json, &decoded));
            TEST(decoded.graph_layout_count == 1);
            if (decoded.graph_layout_count == 1) {
                TEST(decoded.graph_layout[0].x == 1.0f); // first record kept
            }

            // Old files: a detached-envelope record still loads unchanged.
            build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 1, 1, 1, 0, 0);
            TEST(decode_bank_with_editor_section(bank, editor_json, &decoded));

        } while (false);
    }

    // Add Envelope's default descriptor (do_osc_add_generator's construction
    // - the function is GUI-only, so the test replicates it) satisfies bank
    // validation and sustains at the full-scale peak: the delta is in
    // per-65535 point units and the attack spans about 30 ms (positions are
    // control ticks, 256/44100 s each).
    {
        static Synth::InstrumentEditorBank bank;
        bank                = {};
        const uint32_t slot = bank.bank.envelopes.allocate();
        TEST(slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank.bank.envelopes.entries[slot];
        env                            = Synth::EnvelopeDescriptor{};
        env.num_points                 = 3;
        env.sustain_first_point        = 1;
        env.sustain_last_point         = 1;
        env.min_value                  = -1.0f;
        env.min_max_delta              = 2.0f / 65535.0f;
        env.points[0].position         = 0;
        env.points[0].value            = 0x0000;
        env.points[1].position         = 5;
        env.points[1].value            = 0xFFFF;
        env.points[2].position         = 91;
        env.points[2].value            = 0x8000;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::EnvelopeState state = { 0, 0 };
        float                peak  = env.min_value;
        for (int i = 0; i < 8; i++) {
            peak = Synth::eval_envelope(env, &state, true);
        }
        TEST(approx(peak, 1.0f, 1e-4f));
        // The sustain loop holds the peak: further evaluation does not move
        // past the final point.
        TEST(approx(Synth::eval_envelope(env, &state, true), 1.0f, 1e-4f));
        // Note-off: the release tail descends from the peak back to neutral
        // (0.0 on this envelope's -1..1 range) and the envelope stays there,
        // so the voice can free on silence.
        float released = 1.0f;
        for (int i = 0; i < 96; i++) {
            released = Synth::eval_envelope(env, &state, false);
        }
        TEST(approx(released, 0.0f, 1e-4f));
        TEST(approx(Synth::eval_envelope(env, &state, false), 0.0f, 1e-4f));
    }

    // Partial-wiring persistence: a kind-3 record with served bits plus
    // envelope/LFO references against zero generator cells materializes
    // the parameter with all its wires and its stored name - wiring the
    // live instrument tuple does not express, so the record alone carries
    // it across re-projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        do {
            bank                         = {};
            instrument                   = {};
            bank.bank.channel_enabled[0] = 1;
            TEST(bank.bank.instruments.allocate() == 0);
            bank.bank.channel_zones[0][0].start_note = 1;
            bank.bank.channel_zones[0][0].instrument = 0;
            const uint32_t env_slot                  = bank.bank.envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            Synth::EnvelopeDescriptor& env = bank.bank.envelopes.entries[env_slot];
            env.num_points                 = 2;
            env.sustain_first_point        = 0;
            env.sustain_last_point         = 1;
            env.min_value                  = -1.0f;
            env.min_max_delta              = 2.0f;
            env.points[0].position         = 0;
            env.points[0].value            = 0x2000;
            env.points[1].position         = 100;
            env.points[1].value            = 0x4000;
            const uint32_t lfo_slot        = bank.bank.lfos.allocate();
            TEST(lfo_slot != pool_no_slot);
            Synth::LFODescriptor& lfo = bank.bank.lfos.entries[lfo_slot];
            lfo.wave                  = Synth::WaveType::sine_wave;
            lfo.period_ms             = 300;
            lfo.min_value             = -1.0f;
            lfo.min_max_delta         = 2.0f;

            instrument.layer_count                           = 2;
            instrument.layers[0].osc_type[0]                 = Synth::WaveType::sine_wave;
            instrument.layers[1].osc_type[0]                 = Synth::WaveType::sine_wave;
            instrument.routing[Synth::mod_volume].base_value = 1.0f;
            bank.bank.instruments.entries[0]                 = instrument;

            add_detached_record(&bank, 0, 0, 1, 1, 10.0f, 20.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
            add_detached_record(&bank, 0, 0, 2, 1, 30.0f, 40.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
            add_parameter_record(&bank, 0, 0, 0, 50.0f, 60.0f, 1);
            Synth::GraphNodeLayout& param_record = bank.graph_layout[bank.graph_layout_count - 1];
            param_record.served                  = 0x3;
            param_record.env_desc_id             = 1;
            param_record.lfo_desc_id             = 1;
            snprintf(param_record.name, sizeof(param_record.name), "My Param");

            static Sculptor::Graph           graph;
            static Sculptor::OscGraphMapping mapping;
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            TEST(mapping.param_count == 1);
            const uint32_t param_node = mapping.params[0].node_idx;
            TEST(find_graph_node_by_name(graph, "My Param") == param_node);
            TEST(graph.node(param_node).renamable);             // parameter nodes own their stored title
            TEST(! graph.node(mapping.osc_nodes[0]).renamable); // oscillator titles derive
            TEST(mapping.params[0].served == 0x3);
            for (uint32_t layer = 0; layer < 2; layer++) {
                const uint32_t wire = find_osc_graph_connection(graph, { mapping.osc_nodes[layer], 9 });
                TEST(wire != Sculptor::pool_no_slot);
                if (wire != Sculptor::pool_no_slot) {
                    TEST(graph.get_connection(wire).output.node_idx == param_node);
                }
            }
            const uint32_t env_wire = find_osc_graph_connection(graph, { param_node, 2 });
            TEST(env_wire != Sculptor::pool_no_slot);
            if (env_wire != Sculptor::pool_no_slot) {
                TEST(mapping.params[0].env_node == graph.get_connection(env_wire).output.node_idx);
            }
            const uint32_t lfo_wire = find_osc_graph_connection(graph, { param_node, 3 });
            TEST(lfo_wire != Sculptor::pool_no_slot);
            if (lfo_wire != Sculptor::pool_no_slot) {
                TEST(mapping.params[0].lfo_node == graph.get_connection(lfo_wire).output.node_idx);
            }
        } while (false);
    }

    // The evaporation scenario end-to-end: connecting a record-backed
    // parameter (zero generator tuple) to an oscillator row and re-projecting
    // keeps the wire - the value wire through the apply path, the envelope
    // input wire likewise; both are editor state the generator tuple cannot
    // express, so only the record persists them.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        do {
            bank                         = {};
            instrument                   = {};
            bank.bank.channel_enabled[0] = 1;
            TEST(bank.bank.instruments.allocate() == 0);
            bank.bank.channel_zones[0][0].start_note = 1;
            bank.bank.channel_zones[0][0].instrument = 0;
            const uint32_t env_slot                  = bank.bank.envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            Synth::EnvelopeDescriptor& env                   = bank.bank.envelopes.entries[env_slot];
            env.num_points                                   = 2;
            env.sustain_first_point                          = 0;
            env.sustain_last_point                           = 1;
            env.min_value                                    = -1.0f;
            env.min_max_delta                                = 2.0f;
            env.points[0].position                           = 0;
            env.points[0].value                              = 0x2000;
            env.points[1].position                           = 100;
            env.points[1].value                              = 0x4000;
            instrument.layer_count                           = 1;
            instrument.layers[0].osc_type[0]                 = Synth::WaveType::sine_wave;
            instrument.routing[Synth::mod_volume].base_value = 1.0f;
            bank.bank.instruments.entries[0]                 = instrument;

            add_detached_record(&bank, 0, 0, 1, 1, 10.0f, 20.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
            add_parameter_record(&bank, 0, 0, 0, 50.0f, 60.0f, 1);

            static Sculptor::Graph           graph;
            static Sculptor::OscGraphMapping mapping;
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            TEST(mapping.param_count == 1);
            const uint32_t param_node = mapping.params[0].node_idx;
            TEST(find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 }) == Sculptor::pool_no_slot);
            // Connect the parameter's value output to the layer-0 volume row.
            TEST(graph.add_connection(Sculptor::EndPoint{ param_node, mapping.param_output_slot },
                                      Sculptor::EndPoint{ mapping.osc_nodes[0], 9 }) != Sculptor::pool_no_slot);
            // Wire the envelope instance into the parameter's envelope input.
            const uint32_t env_node = mapping.detached[0].node_idx;
            TEST(mapping.detached[0].kind == 1);
            TEST(graph.add_connection(Sculptor::EndPoint{ env_node, mapping.env_output_slot },
                                      Sculptor::EndPoint{ param_node, 2 }) != Sculptor::pool_no_slot);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count == 2);
            if (count != 2) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.graph_layout[bank.graph_layout_count - 1].served == 0x1);
            TEST(bank.graph_layout[bank.graph_layout_count - 1].env_desc_id == 1);
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
            // Re-projection keeps both wires.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(mapping.param_count >= 1);
            const uint32_t reprojected = find_graph_node_by_name(graph, "Volume");
            TEST(reprojected != Sculptor::pool_no_slot);
            if (reprojected != Sculptor::pool_no_slot) {
                TEST(find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 }) != Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(graph, { reprojected, 2 }) != Sculptor::pool_no_slot);
            }
        } while (false);
    }

    // Write-side sync: the record fields follow the live wires.  Disconnecting
    // a value wire clears the served bit; deleting an oscillator layer shifts
    // the bits like the missing-sum bits and the kind-0 oscillator records.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 5); // attaches to the first derived volume parameter
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            int32_t record_idx = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.kind == 3 && record.index == 0 && record.uid == 5) {
                    record_idx = static_cast<int32_t>(r);
                }
            }
            TEST(record_idx >= 0);
            if (record_idx < 0) {
                break;
            }
            // The attached parameter serves layers 0 and 2 (the shared tuple).
            uint32_t vol_node = Sculptor::pool_no_slot;
            for (uint32_t p = 0; p < mapping.param_count; p++) {
                if (mapping.params[p].uid == 5) {
                    vol_node = mapping.params[p].node_idx;
                }
            }
            TEST(vol_node != Sculptor::pool_no_slot);
            if (vol_node == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t wire = find_osc_graph_connection(graph, { mapping.osc_nodes[0], 9 });
            TEST(wire != Sculptor::pool_no_slot);
            if (wire == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.get_connection(wire).output.node_idx == vol_node);
            graph.delete_connection(wire);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count >= 1);
            if (count == 0) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.graph_layout[record_idx].served == 0x4); // layer-0 bit gone, layer-2 bit kept
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(bank.bank.instruments.entries[0].layers[2].gen[Synth::mod_volume].envelope_desc_id == 1);

            // Layer deletion compacts the served bits: deleting layer 1 moves
            // the layer-2 bit down to layer 1 (and shifts the kind-0 record).
            graph.delete_node(mapping.osc_nodes[1]);
            const uint32_t count_2 = graph.take_changes(changes, 8);
            TEST(count_2 >= 1);
            if (count_2 == 0) {
                break;
            }
            for (uint32_t i = 0; i < count_2; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.graph_layout[record_idx].served == 0x2); // layer-2 bit compacted into layer 1
            TEST(bank.bank.instruments.entries[0].layer_count == 2);
            TEST(bank.bank.instruments.entries[0].layers[1].gen[Synth::mod_volume].envelope_desc_id == 1);
        } while (false);
    }

    // Naming: Add Parameter's record names the node "Parameter %u" (uid);
    // a free-text rename through the name_changed apply path writes the
    // record, creates one for a derived parameter, survives re-projection,
    // refuses an empty name and trims to the record's 31 characters.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        const uint8_t added_uid = Sculptor::allocate_detached_uid(bank, 0, 0, 3);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, added_uid);
        snprintf(bank.graph_layout[bank.graph_layout_count - 1].name,
                 sizeof(bank.graph_layout[0].name),
                 "Parameter %u",
                 added_uid);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Parameter 1") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume") == Sculptor::pool_no_slot); // the record attached to it

            // Free rename of the OTHER (derived, record-less) volume parameter.
            const uint32_t vol2 = find_graph_node_by_name(graph, "Volume 2");
            TEST(vol2 != Sculptor::pool_no_slot);
            if (vol2 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol2, "Filter Mix");
            (void)graph.take_changes(discard, 8); // the widget's own event; the batch below carries the rename
            static Sculptor::GraphChange change = {};
            change.kind                         = Sculptor::ChangeKind::name_changed;
            change.node_idx                     = vol2;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 2); // the moved record plus the rename's new record
            int32_t renamed_record = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.kind == 3 && record.index == 0 && strcmp(record.name, "Filter Mix") == 0) {
                    renamed_record = static_cast<int32_t>(r);
                }
            }
            TEST(renamed_record >= 0);
            if (renamed_record < 0) {
                break;
            }
            TEST(bank.graph_layout[renamed_record].uid != 0);
            TEST(bank.graph_layout[renamed_record].served == 0x2); // seeded from the live wiring (layer 1)
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Filter Mix") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume 2") == Sculptor::pool_no_slot);

            // An empty rename refuses: the old name survives.
            const uint32_t renamed = find_graph_node_by_name(graph, "Filter Mix");
            TEST(renamed != Sculptor::pool_no_slot);
            if (renamed == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(renamed, "");
            (void)graph.take_changes(discard, 8);
            change.node_idx = renamed;
            TEST(! Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Filter Mix") != Sculptor::pool_no_slot);

            // A name longer than the record stores trims to 31 characters.
            const uint32_t trim_node = find_graph_node_by_name(graph, "Filter Mix");
            if (trim_node != Sculptor::pool_no_slot) {
                graph.rename_node(trim_node, "0123456789012345678901234567890123456789");
                (void)graph.take_changes(discard, 8);
                change.node_idx = trim_node;
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
                int32_t trimmed = -1;
                for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                    const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                    if (record.kind == 3 && record.index == 0 && strncmp(record.name, "0123456789", 10) == 0) {
                        trimmed = static_cast<int32_t>(r);
                    }
                }
                TEST(trimmed >= 0);
                if (trimmed >= 0) {
                    TEST(strlen(bank.graph_layout[trimmed].name) == 31);
                }
            }
        } while (false);
    }

    // Derived-sibling rename hijack: a kind-3 record gained by a derived
    // parameter would attach positionally to the earlier-enumerated
    // record-less same-target sibling at re-projection.  Renaming the second
    // sibling is refused; renaming the first sibling then allows the second.
    // No record ever lands on the wrong parameter.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            // params[0] and params[1] are the two derived volume parameters
            // (different tuples); params[2] is pitch.
            TEST(mapping.params[0].uid == 0 && mapping.params[1].uid == 0);
            TEST(Sculptor::param_has_recordless_predecessor(mapping, 1));
            TEST(! Sculptor::param_has_recordless_predecessor(mapping, 0));
            // The destination-target variant of the guard: volume and pitch carry
            // derived parameters, panning and highpass do not.
            TEST(Sculptor::param_target_has_recordless_derived(mapping, 0));
            TEST(Sculptor::param_target_has_recordless_derived(mapping, 1));
            TEST(! Sculptor::param_target_has_recordless_derived(mapping, 2));
            TEST(! Sculptor::param_target_has_recordless_derived(mapping, 4));

            // Renaming the SECOND volume parameter is refused: no record may
            // be created.
            const uint32_t vol2 = find_graph_node_by_name(graph, "Volume 2");
            TEST(vol2 != Sculptor::pool_no_slot);
            if (vol2 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol2, "Second Name");
            (void)graph.take_changes(discard, 8); // the widget's own event; the batch below carries the rename
            static Sculptor::GraphChange change = {};
            change.kind                         = Sculptor::ChangeKind::name_changed;
            change.node_idx                     = vol2;
            TEST(! Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 0);
            // The refusal recovery re-derives the old titles: the first
            // sibling keeps its state, no record hijacked it.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(mapping.params[0].uid == 0 && mapping.params[1].uid == 0);
            TEST(find_graph_node_by_name(graph, "Volume") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume 2") != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Second Name") == Sculptor::pool_no_slot);

            // Renaming the FIRST sibling succeeds and its record seeds from
            // its own wiring (layers 0 and 2).
            const uint32_t vol1 = find_graph_node_by_name(graph, "Volume");
            TEST(vol1 != Sculptor::pool_no_slot);
            if (vol1 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol1, "First Name");
            (void)graph.take_changes(discard, 8);
            change.node_idx = vol1;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 1);
            int32_t first_record = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.kind == 3 && strcmp(record.name, "First Name") == 0) {
                    first_record = static_cast<int32_t>(r);
                }
            }
            TEST(first_record >= 0);
            if (first_record < 0) {
                break;
            }
            TEST(bank.graph_layout[first_record].served == 0x5);
            TEST(! Sculptor::param_has_recordless_predecessor(mapping, 1));

            // Now the SECOND sibling's rename succeeds; re-projection keeps
            // both records on their own parameters.
            const uint32_t vol2b = find_graph_node_by_name(graph, "Volume 2");
            TEST(vol2b != Sculptor::pool_no_slot);
            if (vol2b == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol2b, "Second Name");
            (void)graph.take_changes(discard, 8);
            change.node_idx = vol2b;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            TEST(count_records_matching(bank, 0, 0, 3, 0) == 2);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t first_node  = find_graph_node_by_name(graph, "First Name");
            const uint32_t second_node = find_graph_node_by_name(graph, "Second Name");
            TEST(first_node != Sculptor::pool_no_slot && second_node != Sculptor::pool_no_slot);
            TEST(find_graph_node_by_name(graph, "Volume") == Sculptor::pool_no_slot);
            // No hijack: the first parameter kept the layer-0/2 wiring, the
            // second its layer-1 wiring.
            int32_t named_first  = -1;
            int32_t named_second = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.kind == 3 && strcmp(record.name, "First Name") == 0) {
                    named_first = static_cast<int32_t>(r);
                }
                if (record.kind == 3 && strcmp(record.name, "Second Name") == 0) {
                    named_second = static_cast<int32_t>(r);
                }
            }
            TEST(named_first >= 0 && named_second >= 0);
            if (named_first >= 0 && named_second >= 0) {
                TEST(bank.graph_layout[named_first].served == 0x5);
                TEST(bank.graph_layout[named_second].served == 0x2);
                TEST(! Sculptor::param_target_has_recordless_derived(mapping, 0));
            }
        } while (false);
    }

    // LFO depth rewire: a record-backed parameter's LFO binding survives a
    // mid-session rewire of the bound instance's depth input.  The record
    // triple resolves from the live depth/rate edges, so the rewire is what
    // the record stores and the parameter's LFO wire re-attaches at
    // re-projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            // Give the first volume parameter a record via a free-text
            // rename; the record seeds its LFO triple from the live wiring
            // (desc 1, depth velocity, rate mod wheel).
            const uint32_t vol1 = find_graph_node_by_name(graph, "Volume");
            TEST(vol1 != Sculptor::pool_no_slot);
            if (vol1 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol1, "Wired");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange change = {};
            change.kind                         = Sculptor::ChangeKind::name_changed;
            change.node_idx                     = vol1;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            int32_t record_idx = -1;
            for (uint32_t r = 0; r < bank.graph_layout_count; r++) {
                const Synth::GraphNodeLayout& record = bank.graph_layout[r];
                if (record.kind == 3 && strcmp(record.name, "Wired") == 0) {
                    record_idx = static_cast<int32_t>(r);
                }
            }
            TEST(record_idx >= 0);
            if (record_idx < 0) {
                break;
            }
            TEST(bank.graph_layout[record_idx].lfo_desc_id == 1);
            TEST(static_cast<Synth::ModSource>(bank.graph_layout[record_idx].lfo_depth_source) ==
                 Synth::ModSource::velocity);

            // Rewire the bound LFO instance's depth input to pitch bend.
            const uint32_t lfo_node = mapping.params[0].lfo_node;
            TEST(lfo_node != Sculptor::pool_no_slot);
            const uint32_t depth_conn = find_osc_graph_connection(graph, { lfo_node, mapping.lfo_depth_input_slot });
            TEST(depth_conn != Sculptor::pool_no_slot);
            if (depth_conn == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.get_connection(depth_conn).output.slot_idx ==
                 mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1]);
            graph.delete_connection(depth_conn);
            const uint32_t bend_idx = static_cast<uint32_t>(Synth::ModSource::pitch_bend) - 1;
            TEST(graph.add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_source_slots[bend_idx] },
                                      Sculptor::EndPoint{ lfo_node, mapping.lfo_depth_input_slot }) !=
                 Sculptor::pool_no_slot);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               count      = graph.take_changes(changes, 8);
            TEST(count == 2);
            if (count != 2) {
                break;
            }
            for (uint32_t i = 0; i < count; i++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i]));
            }
            TEST(bank.graph_layout[record_idx].lfo_desc_id == 1);
            TEST(static_cast<Synth::ModSource>(bank.graph_layout[record_idx].lfo_depth_source) ==
                 Synth::ModSource::pitch_bend);
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].lfo_depth_source ==
                 Synth::ModSource::pitch_bend);

            // Re-projection re-attaches the parameter's LFO wire: the record
            // triple matches the rewired instance.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t wired_node = find_graph_node_by_name(graph, "Wired");
            TEST(wired_node != Sculptor::pool_no_slot);
            if (wired_node == Sculptor::pool_no_slot) {
                break;
            }
            TEST(mapping.params[0].lfo_node != Sculptor::pool_no_slot);
            const uint32_t lfo_wire = find_osc_graph_connection(graph, { wired_node, 3 });
            TEST(lfo_wire != Sculptor::pool_no_slot);
            if (lfo_wire != Sculptor::pool_no_slot) {
                TEST(graph.get_connection(lfo_wire).output.node_idx == mapping.params[0].lfo_node);
            }
        } while (false);
    }

    // Wire-driven retarget: dropping (or dragging) a record-backed
    // parameter's value wire onto a different target's oscillator row
    // retargets the parameter. The record re-keys, the parameter's other
    // value wires shift to the new target's rows, the node keeps its
    // user-chosen name, and the compile routes the new target while the old
    // target keeps its remaining serving parameters.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            // Give the volume parameter a record via a free-text rename; the
            // record seeds from its live wiring (volume rows of layers 0/2).
            const uint32_t vol1 = find_graph_node_by_name(graph, "Volume");
            TEST(vol1 != Sculptor::pool_no_slot);
            if (vol1 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(vol1, "Parameter A");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange change = {};
            change.kind                         = Sculptor::ChangeKind::name_changed;
            change.node_idx                     = vol1;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));
            const int32_t p = Sculptor::find_param(mapping, vol1);
            TEST(p >= 0 && mapping.params[p].uid != 0 && mapping.params[p].target == 0);
            if (p < 0 || mapping.params[p].uid == 0) {
                break;
            }
            // The pitch row is refused (its derived parameter is record-less and
            // would be hijacked); the empty panning row is accepted.
            const Sculptor::EndPoint param_out   = { vol1, mapping.param_output_slot };
            const Sculptor::EndPoint pitch_row   = { mapping.osc_nodes[0], 11 };
            const Sculptor::EndPoint panning_row = { mapping.osc_nodes[0], 10 };
            TEST(! Sculptor::osc_graph_validate(&mapping, graph, param_out, pitch_row));
            TEST(Sculptor::osc_graph_validate(&mapping, graph, param_out, panning_row));
            TEST(graph.add_connection(param_out, panning_row) != Sculptor::pool_no_slot);
            static Sculptor::GraphChange changes[8] = {};
            const uint32_t               num        = graph.take_changes(changes, 8);
            TEST(num == 1);
            if (num != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[0]));
            // The record and the mapping entry carry the new target and the node
            // kept its user-chosen name.
            TEST(mapping.params[p].target == 2);
            const int32_t record_idx = Sculptor::find_record(bank, 0, 0, 3, 2, mapping.params[p].uid);
            TEST(record_idx >= 0 && bank.graph_layout[record_idx].index == 2);
            TEST(strcmp(graph.node(vol1).name, "Parameter A") == 0);
            // The parameter's wires all sit on panning rows now: the layer-0
            // volume wire was freed (the dropped wire occupies that row) and
            // the layer-2 wire shifted, so each touched row carries exactly
            // one edge from this parameter.
            uint32_t panning_wires = 0;
            uint32_t volume_wires  = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != vol1 || connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[0] && connection.input.slot_idx == 10) {
                    panning_wires++;
                }
                if (connection.input.node_idx == mapping.osc_nodes[0] && connection.input.slot_idx == 9) {
                    volume_wires++;
                }
            }
            TEST(panning_wires == 1 && volume_wires == 0);
            // The parameter served layers 0 and 2: the layer-2 rows shifted too.
            uint32_t panning_wires_2 = 0;
            uint32_t volume_wires_2  = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != vol1 || connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[2] && connection.input.slot_idx == 10) {
                    panning_wires_2++;
                }
                if (connection.input.node_idx == mapping.osc_nodes[0] && connection.input.slot_idx == 9) {
                    volume_wires_2++;
                }
            }
            TEST(panning_wires_2 == 1 && volume_wires_2 == 0);
            // The compile routes the parameter's source wiring into the new
            // target; the old target keeps its remaining serving parameter.
            const Synth::Instrument& compiled = bank.bank.instruments.entries[0];
            TEST(compiled.routing[Synth::mod_panning].num_inputs == 1);
            TEST(compiled.routing[Synth::mod_panning].inputs[0].source == Synth::ModSource::velocity);
            TEST(compiled.routing[Synth::mod_volume].num_inputs == 1);
            // Re-projection is stable: the record drives the same rows.
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            TEST(find_graph_node_by_name(graph, "Parameter A") != Sculptor::pool_no_slot);
            // Re-projection rebuilt the mapping: re-find the parameter by node.
            const int32_t p2 = Sculptor::find_param(mapping, find_graph_node_by_name(graph, "Parameter A"));
            TEST(p2 >= 0 && mapping.params[p2].target == 2 && mapping.params[p2].uid != 0);
            // Dragging the wire's input end to the lowpass row retargets again.
            const uint32_t wire = Sculptor::connection_into(graph, mapping.osc_nodes[0], 10);
            TEST(wire != Sculptor::pool_no_slot);
            if (wire == Sculptor::pool_no_slot) {
                break;
            }
            TEST(graph.move_connection_end(wire, false, Sculptor::EndPoint{ mapping.osc_nodes[0], 13 }));
            static Sculptor::GraphChange moves[8] = {};
            const uint32_t               moved    = graph.take_changes(moves, 8);
            TEST(moved == 1);
            if (moved != 1) {
                break;
            }
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, moves[0]));
            TEST(mapping.params[p2].target == 3);
            const int32_t record_idx2 = Sculptor::find_record(bank, 0, 0, 3, 3, mapping.params[p2].uid);
            TEST(record_idx2 >= 0);
            // A cross-target drop bypassing the validator is refused by the apply
            // path's defensive guard, and re-projection clears the stray wire.
            TEST(graph.add_connection(param_out, pitch_row) != Sculptor::pool_no_slot);
            static Sculptor::GraphChange strays[8] = {};
            const uint32_t               strayed   = graph.take_changes(strays, 8);
            TEST(strayed == 1);
            if (strayed != 1) {
                break;
            }
            TEST(! Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, strays[0]));
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const int32_t p3 = Sculptor::find_param(mapping, find_graph_node_by_name(graph, "Parameter A"));
            TEST(p3 >= 0 && mapping.params[p3].target == 3 && mapping.params[p3].uid != 0);
            const uint32_t param_a = mapping.params[p3].node_idx;
            TEST(param_a != Sculptor::pool_no_slot);
            uint32_t pitch_wires = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx == param_a && connection.output.slot_idx == mapping.param_output_slot &&
                    connection.input.node_idx == mapping.osc_nodes[0] && connection.input.slot_idx == 11) {
                    pitch_wires++;
                }
            }
            TEST(pitch_wires == 0);
        } while (false);
    }

    // Record pairing survives uid order: a record whose uid is lower than
    // its sibling's must not steal the sibling's group. The records below
    // are the state after renaming "Volume 2" first and "Volume" second
    // (uid 1 carries ordinal 2, uid 2 carries ordinal 1); the explicit
    // ordinals pair each record with its own group, while the legacy
    // uid-order positional attach would transpose them.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        // uid 1 renamed second: it belongs to the second-derived group
        // ("Volume 2", serving layer 1). uid 2 renamed first: the first
        // derived group ("Volume", serving layers 0 and 2).
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
        bank.graph_layout[bank.graph_layout_count - 1].param_slot = 2;
        snprintf(bank.graph_layout[bank.graph_layout_count - 1].name,
                 sizeof(bank.graph_layout[bank.graph_layout_count - 1].name),
                 "Alpha");
        add_parameter_record(&bank, 0, 0, 0, 30.0f, 40.0f, 2);
        bank.graph_layout[bank.graph_layout_count - 1].param_slot = 1;
        snprintf(bank.graph_layout[bank.graph_layout_count - 1].name,
                 sizeof(bank.graph_layout[bank.graph_layout_count - 1].name),
                 "Beta");
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t alpha = find_graph_node_by_name(graph, "Alpha");
            const uint32_t beta  = find_graph_node_by_name(graph, "Beta");
            TEST(alpha != Sculptor::pool_no_slot && beta != Sculptor::pool_no_slot);
            if (alpha == Sculptor::pool_no_slot || beta == Sculptor::pool_no_slot) {
                break;
            }
            // Alpha is layer 1's parameter: one value wire into layer 1's row,
            // none into layer 0's. Beta serves layers 0 and 2.
            uint32_t alpha_layer1 = 0;
            uint32_t alpha_layer0 = 0;
            uint32_t beta_layer1  = 0;
            uint32_t beta_layer0  = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[1] && connection.input.slot_idx == 9) {
                    if (connection.output.node_idx == alpha) {
                        alpha_layer1++;
                    }
                    if (connection.output.node_idx == beta) {
                        beta_layer1++;
                    }
                }
                if (connection.input.node_idx == mapping.osc_nodes[0] && connection.input.slot_idx == 9) {
                    if (connection.output.node_idx == alpha) {
                        alpha_layer0++;
                    }
                    if (connection.output.node_idx == beta) {
                        beta_layer0++;
                    }
                }
            }
            TEST(alpha_layer1 == 1 && alpha_layer0 == 0);
            TEST(beta_layer1 == 0 && beta_layer0 == 1);
        } while (false);
    }

    // Rename, add-parameter, rename interleave: the added parameter's
    // record sits between the two renamed ones in record-array order.
    // The explicit ordinals keep each rename on its own group and the
    // added record materializes free-standing instead of hijacking a
    // group. (uid order here matches layer order; the out-of-uid-order
    // test above carries the transposition proof.)
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t first = find_graph_node_by_name(graph, "Volume");
            TEST(first != Sculptor::pool_no_slot);
            if (first == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(first, "One");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = first;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            // Add Parameter's record: free-standing, named. The renames took
            // uids 1 and 2, so this one takes 3 and sits between them in the
            // record array.
            add_parameter_record(&bank, 0, 0, 0, 50.0f, 60.0f, 3);
            Synth::GraphNodeLayout& added = bank.graph_layout[bank.graph_layout_count - 1];
            added.param_slot              = Synth::graph_record_param_free;
            snprintf(added.name, sizeof(added.name), "Parameter 1");
            const uint32_t second = find_graph_node_by_name(graph, "Volume 2");
            TEST(second != Sculptor::pool_no_slot);
            if (second == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(second, "Two");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename2 = {};
            rename2.kind                         = Sculptor::ChangeKind::name_changed;
            rename2.node_idx                     = second;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename2));
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t one        = find_graph_node_by_name(graph, "One");
            const uint32_t two        = find_graph_node_by_name(graph, "Two");
            const uint32_t added_node = find_graph_node_by_name(graph, "Parameter 1");
            TEST(one != Sculptor::pool_no_slot && two != Sculptor::pool_no_slot &&
                 added_node != Sculptor::pool_no_slot);
            if (one == Sculptor::pool_no_slot || two == Sculptor::pool_no_slot ||
                added_node == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t one_layer0        = 0;
            uint32_t two_layer1        = 0;
            uint32_t added_value_wires = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == added_node) {
                    added_value_wires++;
                }
                if (connection.output.node_idx == one && connection.input.node_idx == mapping.osc_nodes[0] &&
                    connection.input.slot_idx == 9) {
                    one_layer0++;
                }
                if (connection.output.node_idx == two && connection.input.node_idx == mapping.osc_nodes[1] &&
                    connection.input.slot_idx == 9) {
                    two_layer1++;
                }
            }
            TEST(one_layer0 == 1 && two_layer1 == 1);
            TEST(added_value_wires == 0);
        } while (false);
    }

    // Stale-ordinal consistency: a record whose ordinal names no existing
    // group disables the explicit pass for that target, so every record of
    // the target takes the uid-order positional fallback (the retired
    // semantics that handle renumbered positions correctly). Here the
    // surviving group's record (uid 1, ordinal 2 - its group outlived the
    // first group and now sits alone) must win the single group via the
    // uid-order fallback; the stale record (uid 2, ordinal 1, whose group
    // died) materializes free-standing. Without the guard, uid 2 would
    // bind the group by position and uid 1 would duplicate the node - the
    // reverse pairing.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        // The projection reads the bank's instrument entry, not the local
        // fixture copy: zero the gen cells there.
        bank.bank.instruments.entries[0].layers[1].gen[Synth::mod_volume] = Synth::LayerGen{};
        bank.bank.instruments.entries[0].layers[2].gen[Synth::mod_volume] = Synth::LayerGen{};
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
        bank.graph_layout[bank.graph_layout_count - 1].param_slot = 2;
        snprintf(bank.graph_layout[bank.graph_layout_count - 1].name,
                 sizeof(bank.graph_layout[bank.graph_layout_count - 1].name),
                 "Live");
        add_parameter_record(&bank, 0, 0, 0, 30.0f, 40.0f, 2);
        bank.graph_layout[bank.graph_layout_count - 1].param_slot = 1;
        snprintf(bank.graph_layout[bank.graph_layout_count - 1].name,
                 sizeof(bank.graph_layout[bank.graph_layout_count - 1].name),
                 "Stale");
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {

            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t stale = find_graph_node_by_name(graph, "Stale");
            const uint32_t live  = find_graph_node_by_name(graph, "Live");
            TEST(stale != Sculptor::pool_no_slot && live != Sculptor::pool_no_slot);
            if (stale == Sculptor::pool_no_slot || live == Sculptor::pool_no_slot) {
                break;
            }
            // Live owns the single volume group: one value wire into layer 0's
            // row. Stale is the surplus materialization: no value wires.
            uint32_t live_wires  = 0;
            uint32_t stale_wires = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == live && connection.input.node_idx == mapping.osc_nodes[0] &&
                    connection.input.slot_idx == 9) {
                    live_wires++;
                }
                if (connection.output.node_idx == stale &&
                    (connection.input.slot_idx == 9 || connection.input.slot_idx == 10 ||
                     connection.input.slot_idx == 11 || connection.input.slot_idx == 13 ||
                     connection.input.slot_idx == 14) &&
                    connection.input.node_idx == mapping.osc_nodes[0]) {
                    stale_wires++;
                }
            }
            TEST(live_wires == 1);
            TEST(stale_wires == 0);
        } while (false);
    }

    // Group-death ordinal truthfulness: a record stamped while its parameter
    // owned a derived group must stay truthful when a sibling group dies.
    // Three volume groups A (layers 0/2), B (layer 1), C (layer 3); renaming
    // A then B stamps their records with ordinals 1 and 2. Deleting A's
    // value wires kills A's group: A's own record slot becomes free-standing
    // and the post-reconcile re-stamp moves B's ordinal to 1, so
    // re-projection binds B's record to B's own group - not to C's.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::Instrument& banked               = bank.bank.instruments.entries[0];
        banked.layer_count                      = 4;
        Synth::LayerGen third_tuple             = banked.layers[0].gen[Synth::mod_volume];
        third_tuple.lfo_depth                   = 0.25f;
        banked.layers[3].gen[Synth::mod_volume] = third_tuple;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            // The rename-order guard forces renaming A before B; both records
            // carry truthful ordinals (1 and 2) while both groups live.
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            TEST(a_param >= 0 && mapping.params[a_param].uid != 0);
            if (a_param < 0 || mapping.params[a_param].uid == 0) {
                break;
            }
            const uint32_t ay_uid    = mapping.params[a_param].uid;
            const int32_t  ay_record = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(ay_uid));
            TEST(ay_record >= 0);
            if (ay_record < 0) {
                break;
            }
            const uint32_t b_node = find_graph_node_by_name(graph, "Volume 2");
            TEST(b_node != Sculptor::pool_no_slot);
            if (b_node == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(b_node, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b_node;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t b_param = Sculptor::find_param(mapping, b_node);
            TEST(b_param >= 0 && mapping.params[b_param].uid != 0);
            if (b_param < 0 || mapping.params[b_param].uid == 0) {
                break;
            }
            const uint32_t bee_uid    = mapping.params[b_param].uid;
            const int32_t  bee_record = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(bee_uid));
            TEST(bee_record >= 0 && bank.graph_layout[bee_record].param_slot == 2);
            if (bee_record < 0 || bank.graph_layout[bee_record].param_slot != 2) {
                break;
            }
            // Delete both of Ay's value wires (it serves layers 0 and 2); the
            // second deletion kills A's group. The graph is mutated directly (the
            // way the editor does it) and the drained deletion events sync the
            // model.
            const uint32_t a_node = find_graph_node_by_name(graph, "Ay");
            TEST(a_node != Sculptor::pool_no_slot);
            if (a_node == Sculptor::pool_no_slot) {
                break;
            }
            for (uint32_t layer = 0; layer < 3; layer++) {
                const uint32_t wire = Sculptor::connection_into(graph, mapping.osc_nodes[layer], 9);
                if (wire == Sculptor::pool_no_slot || graph.get_connection(wire).output.node_idx != a_node) {
                    continue;
                }
                graph.delete_connection(wire);
                static Sculptor::GraphChange deletion[8] = {};
                const uint32_t               moved       = graph.take_changes(deletion, 8);
                for (uint32_t e = 0; e < moved; e++) {
                    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, deletion[e]));
                }
            }
            TEST(bank.graph_layout[bee_record].param_slot == 1);
            TEST(bank.graph_layout[ay_record].param_slot == Synth::graph_record_param_free);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            TEST(bee != Sculptor::pool_no_slot);
            if (bee == Sculptor::pool_no_slot) {
                break;
            }
            // Bee owns its own group: one value wire into layer 1's row, none
            // into layer 3's (C's row).
            uint32_t bee_layer1 = 0;
            uint32_t bee_layer3 = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != bee || connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[1] && connection.input.slot_idx == 9) {
                    bee_layer1++;
                }
                if (connection.input.node_idx == mapping.osc_nodes[3] && connection.input.slot_idx == 9) {
                    bee_layer3++;
                }
            }
            TEST(bee_layer1 == 1 && bee_layer3 == 0);
        } while (false);
    }

    // Node-deletion truthfulness: deleting a parameter node drops its wires
    // and its record before any event drains, so no reconcile runs while the
    // node still exists. apply_node_deleted reconciles after removing the
    // record: the survivors' ordinals are re-stamped (Bee 2 -> 1) and the
    // re-projection binds Bee to its own group, not to C's.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::Instrument& banked               = bank.bank.instruments.entries[0];
        banked.layer_count                      = 4;
        Synth::LayerGen third_tuple             = banked.layers[0].gen[Synth::mod_volume];
        third_tuple.lfo_depth                   = 0.25f;
        banked.layers[3].gen[Synth::mod_volume] = third_tuple;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            const uint32_t b_node = find_graph_node_by_name(graph, "Volume 2");
            TEST(b_node != Sculptor::pool_no_slot);
            if (b_node == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(b_node, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b_node;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t b_param = Sculptor::find_param(mapping, b_node);
            TEST(b_param >= 0 && mapping.params[b_param].uid != 0);
            if (b_param < 0 || mapping.params[b_param].uid == 0) {
                break;
            }
            const uint32_t bee_uid    = mapping.params[b_param].uid;
            const int32_t  bee_record = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(bee_uid));
            TEST(bee_record >= 0 && bank.graph_layout[bee_record].param_slot == 2);
            if (bee_record < 0 || bank.graph_layout[bee_record].param_slot != 2) {
                break;
            }
            const uint32_t a_node = find_graph_node_by_name(graph, "Ay");
            TEST(a_node != Sculptor::pool_no_slot);
            if (a_node == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_node(a_node);
            static Sculptor::GraphChange batch[16] = {};
            const uint32_t               moved     = graph.take_changes(batch, 16);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(bank.graph_layout[bee_record].param_slot == 1);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            TEST(bee != Sculptor::pool_no_slot);
            if (bee == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t bee_layer1 = 0;
            uint32_t bee_layer3 = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != bee || connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[1] && connection.input.slot_idx == 9) {
                    bee_layer1++;
                }
                if (connection.input.node_idx == mapping.osc_nodes[3] && connection.input.slot_idx == 9) {
                    bee_layer3++;
                }
            }
            TEST(bee_layer1 == 1 && bee_layer3 == 0);
        } while (false);
    }

    // Multi-death composition: two same-target groups dying in one batch
    // must leave the survivors' ordinals truthful with no per-death
    // bookkeeping order. Four volume groups A (layers 0/2), B (1), C (3),
    // D (4); renaming A, B, C stamps ordinals 1, 2, 3. Deleting A's and B's
    // wires kills both groups; C's ordinal must end at 1 (counting deaths
    // against the un-renumbered mapping would leave C at 2) and C's record
    // must bind C's own row at re-projection.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::Instrument& banked               = bank.bank.instruments.entries[0];
        banked.layer_count                      = 5;
        Synth::LayerGen third_tuple             = banked.layers[0].gen[Synth::mod_volume];
        third_tuple.lfo_depth                   = 0.25f;
        banked.layers[3].gen[Synth::mod_volume] = third_tuple;
        Synth::LayerGen fourth_tuple            = third_tuple;
        fourth_tuple.lfo_depth                  = 0.125f;
        banked.layers[4].gen[Synth::mod_volume] = fourth_tuple;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename = {};
            rename.kind                         = Sculptor::ChangeKind::name_changed;
            const char*    old_names[3]         = { "Volume", "Volume 2", "Volume 3" };
            const char*    new_names[3]         = { "Ay", "Bee", "Sea" };
            const uint32_t nodes[3]             = {
                find_graph_node_by_name(graph, old_names[0]),
                find_graph_node_by_name(graph, old_names[1]),
                find_graph_node_by_name(graph, old_names[2]),
            };
            for (uint32_t i = 0; i < 3; i++) {
                TEST(nodes[i] != Sculptor::pool_no_slot);
                if (nodes[i] == Sculptor::pool_no_slot) {
                    break;
                }
                graph.rename_node(nodes[i], new_names[i]);
                (void)graph.take_changes(discard, 8);
                rename.node_idx = nodes[i];
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename));
            }
            const uint32_t c_node = find_graph_node_by_name(graph, "Sea");
            TEST(c_node != Sculptor::pool_no_slot);
            if (c_node == Sculptor::pool_no_slot) {
                break;
            }
            const int32_t c_param = Sculptor::find_param(mapping, c_node);
            TEST(c_param >= 0 && mapping.params[c_param].uid != 0);
            if (c_param < 0 || mapping.params[c_param].uid == 0) {
                break;
            }
            const uint32_t sea_uid    = mapping.params[c_param].uid;
            const int32_t  sea_record = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(sea_uid));
            TEST(sea_record >= 0 && bank.graph_layout[sea_record].param_slot == 3);
            if (sea_record < 0 || bank.graph_layout[sea_record].param_slot != 3) {
                break;
            }
            const uint32_t a_node = find_graph_node_by_name(graph, "Ay");
            const uint32_t b_node = find_graph_node_by_name(graph, "Bee");
            TEST(a_node != Sculptor::pool_no_slot && b_node != Sculptor::pool_no_slot);
            if (a_node == Sculptor::pool_no_slot || b_node == Sculptor::pool_no_slot) {
                break;
            }
            // Delete A's value wires (layers 0 and 2) and B's (layer 1) in one
            // graph rewrite, then drain the whole batch once: both group deaths
            // compose inside a single apply pass.
            for (uint32_t layer = 0; layer < 5; layer++) {
                const uint32_t wire = Sculptor::connection_into(graph, mapping.osc_nodes[layer], 9);
                if (wire == Sculptor::pool_no_slot) {
                    continue;
                }
                const uint32_t out = graph.get_connection(wire).output.node_idx;
                if (out != a_node && out != b_node) {
                    continue;
                }
                graph.delete_connection(wire);
            }
            static Sculptor::GraphChange batch[16] = {};
            const uint32_t               moved     = graph.take_changes(batch, 16);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(bank.graph_layout[sea_record].param_slot == 1);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t sea = find_graph_node_by_name(graph, "Sea");
            TEST(sea != Sculptor::pool_no_slot);
            if (sea == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t sea_layer3 = 0;
            uint32_t sea_layer4 = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx != sea || connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.input.node_idx == mapping.osc_nodes[3] && connection.input.slot_idx == 9) {
                    sea_layer3++;
                }
                if (connection.input.node_idx == mapping.osc_nodes[4] && connection.input.slot_idx == 9) {
                    sea_layer4++;
                }
            }
            TEST(sea_layer3 == 1 && sea_layer4 == 0);
        } while (false);
    }

    // Reordering without death: moving A's layer-0 value wire to B changes
    // the derived enumeration order (B now serves the lower layer first)
    // while both groups stay alive. The post-reconcile re-stamp updates both
    // records (Bee 2 -> 1, Ay 1 -> 2) so re-projection pairs each record
    // with its own group.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            const uint32_t b_node = find_graph_node_by_name(graph, "Volume 2");
            TEST(b_node != Sculptor::pool_no_slot);
            if (b_node == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(b_node, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b_node;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            const int32_t b_param = Sculptor::find_param(mapping, b_node);
            TEST(a_param >= 0 && b_param >= 0);
            if (a_param < 0 || b_param < 0) {
                break;
            }
            TEST(mapping.params[a_param].uid != 0 && mapping.params[b_param].uid != 0);
            if (mapping.params[a_param].uid == 0 || mapping.params[b_param].uid == 0) {
                break;
            }
            const uint32_t ay_uid     = mapping.params[a_param].uid;
            const uint32_t bee_uid    = mapping.params[b_param].uid;
            const int32_t  ay_record  = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(ay_uid));
            const int32_t  bee_record = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(bee_uid));
            TEST(ay_record >= 0 && bee_record >= 0);
            if (ay_record < 0 || bee_record < 0) {
                break;
            }
            TEST(bank.graph_layout[ay_record].param_slot == 1 && bank.graph_layout[bee_record].param_slot == 2);
            if (bank.graph_layout[ay_record].param_slot != 1 || bank.graph_layout[bee_record].param_slot != 2) {
                break;
            }
            // Move A's layer-0 wire to B: drop it, then wire B's output into
            // the same row input (the editor's own mutation + drained events).
            const uint32_t a_wire = Sculptor::connection_into(graph, mapping.osc_nodes[0], 9);
            TEST(a_wire != Sculptor::pool_no_slot && graph.get_connection(a_wire).output.node_idx == a0);
            if (a_wire == Sculptor::pool_no_slot || graph.get_connection(a_wire).output.node_idx != a0) {
                break;
            }
            graph.delete_connection(a_wire);
            const uint32_t moved_wire = graph.add_connection(Sculptor::EndPoint{ b_node, mapping.param_output_slot },
                                                             Sculptor::EndPoint{ mapping.osc_nodes[0], 9 });
            TEST(moved_wire != Sculptor::pool_no_slot);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(bank.graph_layout[bee_record].param_slot == 1 && bank.graph_layout[ay_record].param_slot == 2);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            (void)graph.take_changes(discard, 8);
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            const uint32_t ay  = find_graph_node_by_name(graph, "Ay");
            TEST(bee != Sculptor::pool_no_slot && ay != Sculptor::pool_no_slot);
            if (bee == Sculptor::pool_no_slot || ay == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t bee_rows = 0;
            uint32_t ay_rows  = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.input.slot_idx != 9) {
                    continue;
                }
                if (connection.output.node_idx == bee && connection.output.slot_idx == mapping.param_output_slot) {
                    bee_rows++;
                }
                if (connection.output.node_idx == ay && connection.output.slot_idx == mapping.param_output_slot) {
                    ay_rows++;
                }
            }
            TEST(bee_rows == 2 && ay_rows == 1);
        } while (false);
    }

    // Deflation truthfulness: a record-less derived group occupies a group
    // at re-projection, so the re-stamp must count it even though no record
    // pairs with it. Renaming only A and then moving A's layer-0 wire to the
    // record-less B makes B the first derived group; A's ordinal must become
    // 2. Skipping record-less parameters would stamp A as 1 and the explicit
    // pass would bind A's record to B's group (name theft on re-projection).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            const uint32_t b_node = find_graph_node_by_name(graph, "Volume 2");
            TEST(b_node != Sculptor::pool_no_slot);
            if (b_node == Sculptor::pool_no_slot) {
                break;
            }
            TEST(Sculptor::find_param(mapping, b_node) >= 0 &&
                 mapping.params[Sculptor::find_param(mapping, b_node)].uid == 0);
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            TEST(a_param >= 0 && mapping.params[a_param].uid != 0);
            if (a_param < 0 || mapping.params[a_param].uid == 0) {
                break;
            }
            const int32_t ay_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_param].uid));
            TEST(ay_record >= 0 && bank.graph_layout[ay_record].param_slot == 1);
            if (ay_record < 0) {
                break;
            }
            // Move A's layer-0 wire to B in one graph rewrite.
            const uint32_t a_wire = Sculptor::connection_into(graph, mapping.osc_nodes[0], 9);
            TEST(a_wire != Sculptor::pool_no_slot && graph.get_connection(a_wire).output.node_idx == a0);
            if (a_wire == Sculptor::pool_no_slot || graph.get_connection(a_wire).output.node_idx != a0) {
                break;
            }
            graph.delete_connection(a_wire);
            const uint32_t moved_wire = graph.add_connection(Sculptor::EndPoint{ b_node, mapping.param_output_slot },
                                                             Sculptor::EndPoint{ mapping.osc_nodes[0], 9 });
            TEST(moved_wire != Sculptor::pool_no_slot);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(bank.graph_layout[ay_record].param_slot == 2);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t ay = find_graph_node_by_name(graph, "Ay");
            const uint32_t vl = find_graph_node_by_name(graph, "Volume");
            TEST(ay != Sculptor::pool_no_slot && vl != Sculptor::pool_no_slot);
            if (ay == Sculptor::pool_no_slot || vl == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t ay_rows   = 0;
            uint32_t vl_layers = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == ay) {
                    ay_rows++;
                }
                if (connection.output.node_idx == vl && (connection.input.node_idx == mapping.osc_nodes[0] ||
                                                         connection.input.node_idx == mapping.osc_nodes[1])) {
                    vl_layers++;
                }
            }
            TEST(ay_rows == 1 && vl_layers == 2);
        } while (false);
    }

    // Dormant parameter truthfulness: disconnecting a renamed parameter's
    // envelope and LFO leaves its value wires live but compiles them to zero
    // bindings, so the parameter leaves the derived order. Its explicit
    // ordinal must demote to free-standing, and the tuple-class rules must
    // keep the dormant record from claiming the sibling's live group at
    // re-projection (which would resurrect modulation the user removed).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            TEST(a_param >= 0 && mapping.params[a_param].uid != 0);
            if (a_param < 0 || mapping.params[a_param].uid == 0) {
                break;
            }
            const int32_t ay_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_param].uid));
            TEST(ay_record >= 0 && bank.graph_layout[ay_record].param_slot == 1);
            if (ay_record < 0) {
                break;
            }
            // Disconnect A's envelope and LFO in one graph rewrite.
            const uint32_t env_wire = Sculptor::connection_into(graph, a0, 2);
            const uint32_t lfo_wire = Sculptor::connection_into(graph, a0, 3);
            TEST(env_wire != Sculptor::pool_no_slot && lfo_wire != Sculptor::pool_no_slot);
            if (env_wire == Sculptor::pool_no_slot || lfo_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(env_wire);
            graph.delete_connection(lfo_wire);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(bank.graph_layout[ay_record].param_slot == Synth::graph_record_param_free);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t ay = find_graph_node_by_name(graph, "Ay");
            const uint32_t vl = find_graph_node_by_name(graph, "Volume");
            TEST(ay != Sculptor::pool_no_slot && vl != Sculptor::pool_no_slot);
            if (ay == Sculptor::pool_no_slot || vl == Sculptor::pool_no_slot) {
                break;
            }
            // The sibling keeps exactly its own wiring; the dormant record
            // must not have injected its layer-0/2 wires into the sibling.
            uint32_t vl_layer0 = 0;
            uint32_t ay_rows   = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == vl && connection.input.node_idx == mapping.osc_nodes[0]) {
                    vl_layer0++;
                }
                if (connection.output.node_idx == ay) {
                    ay_rows++;
                }
            }
            TEST(vl_layer0 == 0 && ay_rows == 2);
            // The served cells compile to zero bindings: no modulation left.
            const Synth::Instrument& banked = bank.bank.instruments.entries[0];
            TEST(banked.layers[0].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(banked.layers[0].gen[Synth::mod_volume].lfo_desc_id == 0);
            TEST(banked.layers[2].gen[Synth::mod_volume].envelope_desc_id == 0);
            TEST(banked.layers[2].gen[Synth::mod_volume].lfo_desc_id == 0);
            TEST(banked.layers[1].gen[Synth::mod_volume].envelope_desc_id == 2);
        } while (false);
    }

    // Tuple merge: rewiring B's envelope onto A's envelope instance and
    // matching B's LFO row turns B's tuple identical to A's, so the two
    // parameters merge into one derived group at the next projection. The
    // merge-aware re-stamp keeps A's ordinal and demotes B's to
    // free-standing; re-projection must keep both identities (B's record
    // materializes its own surplus node instead of stealing the group).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            const uint32_t b0 = find_graph_node_by_name(graph, "Volume 2");
            TEST(a0 != Sculptor::pool_no_slot && b0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot || b0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            graph.rename_node(b0, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            const int32_t b_param = Sculptor::find_param(mapping, b0);
            TEST(a_param >= 0 && b_param >= 0);
            if (a_param < 0 || b_param < 0) {
                break;
            }
            TEST(mapping.params[a_param].uid != 0 && mapping.params[b_param].uid != 0);
            const int32_t ay_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_param].uid));
            const int32_t bee_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[b_param].uid));
            TEST(ay_record >= 0 && bee_record >= 0);
            if (ay_record < 0 || bee_record < 0) {
                break;
            }
            TEST(bank.graph_layout[ay_record].param_slot == 1 && bank.graph_layout[bee_record].param_slot == 2);
            // Rewire B onto A's envelope and LFO instances and copy A's LFO
            // row values; all validator-legal gestures.
            uint32_t env1 = Sculptor::pool_no_slot;
            uint32_t lfo1 = Sculptor::pool_no_slot;
            for (uint32_t d = 0; d < mapping.detached_count; d++) {
                if (mapping.detached[d].kind == 1 && mapping.detached[d].desc_id == 1) {
                    env1 = mapping.detached[d].node_idx;
                }
                if (mapping.detached[d].kind == 2 && mapping.detached[d].desc_id == 1) {
                    lfo1 = mapping.detached[d].node_idx;
                }
            }
            TEST(env1 != Sculptor::pool_no_slot && lfo1 != Sculptor::pool_no_slot);
            if (env1 == Sculptor::pool_no_slot || lfo1 == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t b_env_wire = Sculptor::connection_into(graph, b0, 2);
            TEST(b_env_wire != Sculptor::pool_no_slot);
            if (b_env_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(b_env_wire);
            TEST(graph.add_connection(Sculptor::EndPoint{ env1, 0 }, Sculptor::EndPoint{ b0, 2 }) !=
                 Sculptor::pool_no_slot);
            TEST(graph.add_connection(Sculptor::EndPoint{ lfo1, 0 }, Sculptor::EndPoint{ b0, 3 }) !=
                 Sculptor::pool_no_slot);
            Sculptor::PropertyValue op_copy = {};
            op_copy.list_index              = graph.node(a0).slots.entries[4].value.list_index;
            graph.set_slot_value(b0, 4, op_copy);
            Sculptor::PropertyValue depth_copy = {};
            depth_copy.real                    = graph.node(a0).slots.entries[5].value.real;
            graph.set_slot_value(b0, 5, depth_copy);
            Sculptor::PropertyValue rate_copy = {};
            rate_copy.real                    = graph.node(a0).slots.entries[6].value.real;
            graph.set_slot_value(b0, 6, rate_copy);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            TEST(Sculptor::refresh_osc_graph_compilation(&bank, graph, mapping, 0, 0));
            TEST(bank.graph_layout[ay_record].param_slot == 1);
            TEST(bank.graph_layout[bee_record].param_slot == Synth::graph_record_param_free);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t ay  = find_graph_node_by_name(graph, "Ay");
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            TEST(ay != Sculptor::pool_no_slot && bee != Sculptor::pool_no_slot);
            if (ay == Sculptor::pool_no_slot || bee == Sculptor::pool_no_slot) {
                break;
            }
            uint32_t ay_rows  = 0;
            uint32_t bee_rows = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == ay) {
                    ay_rows++;
                }
                if (connection.output.node_idx == bee) {
                    bee_rows++;
                }
            }
            // The merged group is one parameter: its owner wires every served
            // row, including the layer the merged-away record used to drive.
            // The merged-away record stays surplus and finds its rows
            // occupied, so it materializes as an inert identity.
            TEST(ay_rows == 3);
            TEST(bee_rows == 0);
            // The merged tuple still compiles: layer 1 carries A's envelope.
            TEST(bank.bank.instruments.entries[0].layers[1].gen[Synth::mod_volume].envelope_desc_id == 1);
        } while (false);
    }

    // Pure-MIDI carrier: disconnecting every generator input from both
    // renamed parameters leaves the target's MIDI routing alive (value
    // wires keep the parameters serving), so the projection derives the
    // single zero-tuple carrier group. The first serving parameter claims
    // it via the uid-order fallback; the dormant sibling materializes its
    // own inert node. No modulation may return, and the routing must stay.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            const uint32_t b0 = find_graph_node_by_name(graph, "Volume 2");
            TEST(a0 != Sculptor::pool_no_slot && b0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot || b0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            graph.rename_node(b0, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const uint32_t a_env = Sculptor::connection_into(graph, a0, 2);
            const uint32_t a_lfo = Sculptor::connection_into(graph, a0, 3);
            const uint32_t b_env = Sculptor::connection_into(graph, b0, 2);
            const uint32_t b_lfo = Sculptor::connection_into(graph, b0, 3);
            TEST(a_env != Sculptor::pool_no_slot && a_lfo != Sculptor::pool_no_slot);
            TEST(b_env != Sculptor::pool_no_slot && b_lfo != Sculptor::pool_no_slot);
            if (a_env == Sculptor::pool_no_slot || a_lfo == Sculptor::pool_no_slot || b_env == Sculptor::pool_no_slot ||
                b_lfo == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(a_env);
            graph.delete_connection(a_lfo);
            graph.delete_connection(b_env);
            graph.delete_connection(b_lfo);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            // The batch's reconciles see the fully-disconnected graph
            // state (graph mutations land before the drained events
            // apply): the first serving parameter takes the carrier with
            // its explicit ordinal kept, and the sibling demotes to
            // free-standing. The carrier claim is therefore explicit,
            // not a positional fallback.
            const int32_t a_carrier = Sculptor::find_param(mapping, a0);
            const int32_t b_carrier = Sculptor::find_param(mapping, b0);
            TEST(a_carrier >= 0 && b_carrier >= 0);
            if (a_carrier < 0 || b_carrier < 0) {
                break;
            }
            TEST(mapping.params[a_carrier].uid != 0 && mapping.params[b_carrier].uid != 0);
            const int32_t ay_carrier =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_carrier].uid));
            const int32_t bee_carrier =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[b_carrier].uid));
            TEST(ay_carrier >= 0 && bee_carrier >= 0);
            if (ay_carrier < 0 || bee_carrier < 0) {
                break;
            }
            TEST(bank.graph_layout[ay_carrier].param_slot == 1);
            TEST(bank.graph_layout[bee_carrier].param_slot == Synth::graph_record_param_free);
            // A demoted (free-standing) record must still claim the
            // carrier through the uid-order positional pass: the tuple
            // rules let a dormant record bind the zero-tuple carrier.
            bank.graph_layout[ay_carrier].param_slot = Synth::graph_record_param_free;
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t ay  = find_graph_node_by_name(graph, "Ay");
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            TEST(ay != Sculptor::pool_no_slot && bee != Sculptor::pool_no_slot);
            if (ay == Sculptor::pool_no_slot || bee == Sculptor::pool_no_slot) {
                break;
            }
            // No modulation anywhere on the volume target, but the MIDI
            // routing survives through the carrier parameter.
            const Synth::Instrument& banked = bank.bank.instruments.entries[0];
            for (uint32_t layer = 0; layer < 3; layer++) {
                TEST(banked.layers[layer].gen[Synth::mod_volume].envelope_desc_id == 0);
                TEST(banked.layers[layer].gen[Synth::mod_volume].lfo_desc_id == 0);
            }
            TEST(banked.routing[Synth::mod_volume].num_inputs == 1);
            uint32_t ay_rows  = 0;
            uint32_t bee_rows = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.slot_idx != mapping.param_output_slot) {
                    continue;
                }
                if (connection.output.node_idx == ay) {
                    ay_rows++;
                }
                if (connection.output.node_idx == bee) {
                    bee_rows++;
                }
            }
            // The carrier is one MIDI-driven parameter: the claiming
            // parameter wires every served row; the dormant record
            // materializes as an inert surplus identity.
            TEST(ay_rows == 3);
            TEST(bee_rows == 0);
        } while (false);
    }

    // Source rows pack the connector and its inline widgets onto one
    // renderer line: the input, op and scale slots of each mod input share
    // a row group, distinct across inputs. Unshared slots would render one
    // line per widget and overflow the parameter node's row budget.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(mapping.param_count >= 1);
            if (mapping.param_count < 1) {
                break;
            }
            const Sculptor::Node& node = graph.node(mapping.params[0].node_idx);
            TEST(node.slots.entries[7].row_group == node.slots.entries[8].row_group);
            TEST(node.slots.entries[8].row_group == node.slots.entries[9].row_group);
            TEST(node.slots.entries[10].row_group == node.slots.entries[11].row_group);
            TEST(node.slots.entries[11].row_group == node.slots.entries[12].row_group);
            TEST(node.slots.entries[7].row_group != node.slots.entries[10].row_group);
            TEST(node.slots.entries[3].row_group == node.slots.entries[4].row_group);
            TEST(node.slots.entries[4].row_group == node.slots.entries[5].row_group);
            TEST(node.slots.entries[5].row_group == node.slots.entries[6].row_group);
        } while (false);
    }

    // A surplus (free-standing) parameter node carries the parameter tint
    // and the shared-row markers like any derived parameter: the visual
    // roles install after the record surplus appends.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                bank.bank.instruments.entries[0].layers[layer].gen[t]  = Synth::LayerGen{};
                bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
            }
        }
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            // With no generator bindings and no MIDI routing the
            // instrument-only projection has zero parameters, so the bare
            // record below materializes a true surplus node (no carrier
            // group forms for it to attach to).
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(mapping.param_count == 0);
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(mapping.param_count == 1);
            if (mapping.param_count != 1) {
                break;
            }
            const uint32_t node = mapping.params[0].node_idx;
            TEST(graph.node_visual_role(node) == Sculptor::node_role_parameter);
            TEST(graph.slot_visual_role(node, 1) == Sculptor::slot_role_shared_row);
            TEST(graph.slot_visual_role(node, 8) == Sculptor::slot_role_shared_row);
            TEST(graph.slot_visual_role(node, 11) == Sculptor::slot_role_shared_row);
            TEST(graph.slot_visual_role(node, 0) != Sculptor::slot_role_shared_row);
        } while (false);
    }

    // Parameter registry bound: the projection materializes one parameter
    // node per surplus record, and max_param_nodes is a designed static
    // budget. With no derived groups, the 35th free-standing parameter
    // still projects; the 36th must fail the projection (the editor's Add
    // Parameter guards this bound before committing).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                bank.bank.instruments.entries[0].layers[layer].gen[t]  = Synth::LayerGen{};
                bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
            }
        }
        for (uint32_t uid = 1; uid <= Sculptor::max_param_nodes; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
        }
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.param_count == Sculptor::max_param_nodes);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(Sculptor::max_param_nodes + 1));
        TEST(! Sculptor::project_editor_to_graph(bank, &graph, &mapping));
    }

    // JSON compatibility: the kind-3 persistence fields round-trip through the
    // bank file, and a record written before the fields existed decodes to
    // the zero state (no name override, no wires).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 3);
        Synth::GraphNodeLayout& record = bank.graph_layout[bank.graph_layout_count - 1];
        record.served                  = 0x5;
        record.env_desc_id             = 1;
        record.lfo_desc_id             = 1;
        record.lfo_depth_source        = static_cast<uint8_t>(Synth::ModSource::velocity);
        record.lfo_rate_source         = static_cast<uint8_t>(Synth::ModSource::mod_wheel);
        record.param_slot              = 2;
        snprintf(record.name, sizeof(record.name), "Persisted");
        static char                        doc[512 * 1024];
        static Synth::InstrumentEditorBank decoded;
        const uint32_t                     written = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(written > 0);
        TEST(Synth::decode_editor_bank_json(doc, written, &decoded));
        TEST(memcmp(bank.graph_layout, decoded.graph_layout, sizeof(bank.graph_layout)) == 0);
        TEST(strcmp(decoded.graph_layout[0].name, "Persisted") == 0);
        TEST(decoded.graph_layout[0].param_slot == 2);
        TEST(Sculptor::validate_editor_metadata(decoded));

        // A legacy record (no new keys) loads with the zero state.  The splice
        // helper decodes the base bank's own editor section too, so the
        // legacy record is the spliced one (uid 1; the fixture's record has
        // uid 3).
        static char    legacy[64 * 1024];
        const uint32_t legacy_len = build_detached_layouts_json(legacy, sizeof(legacy), 1, 1, 3, 0, 0, 0);
        TEST(legacy_len > 0);
        static Synth::InstrumentEditorBank legacy_decoded;
        TEST(decode_bank_with_editor_section(bank, legacy, &legacy_decoded));
        int32_t legacy_idx = -1;
        for (uint32_t r = 0; r < legacy_decoded.graph_layout_count; r++) {
            const Synth::GraphNodeLayout& legacy_record = legacy_decoded.graph_layout[r];
            if (legacy_record.kind == 3 && legacy_record.uid == 1) {
                legacy_idx = static_cast<int32_t>(r);
            }
        }
        TEST(legacy_idx >= 0);
        if (legacy_idx >= 0) {
            TEST(legacy_decoded.graph_layout[legacy_idx].served == 0);
            TEST(legacy_decoded.graph_layout[legacy_idx].env_desc_id == 0);
            TEST(legacy_decoded.graph_layout[legacy_idx].lfo_desc_id == 0);
            TEST(legacy_decoded.graph_layout[legacy_idx].param_slot == 0);
            TEST(legacy_decoded.graph_layout[legacy_idx].name[0] == 0);
        }
        TEST(Sculptor::validate_editor_metadata(legacy_decoded));

        // A record carrying persistence fields on the wrong kind refuses.
        Synth::GraphNodeLayout& bad = bank.graph_layout[bank.graph_layout_count++];
        bad                         = {};
        bad.channel                 = 0;
        bad.zone                    = 0;
        bad.kind                    = 1;
        bad.index                   = 1;
        bad.uid                     = 1;
        bad.served                  = 0x1;
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.served     = 0;
        bad.param_slot = 1;
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.param_slot = 0;
        bad.served     = 0;
        bad.name[0]    = 'x';
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.name[0] = 0;
        TEST(Sculptor::validate_editor_metadata(bank));
        // A kind-3 record with out-of-range persistence fields refuses.
        bad.kind   = 3;
        bad.index  = 0;
        bad.served = static_cast<uint8_t>(1u << Synth::max_layers);
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.served      = 0x1;
        bad.env_desc_id = 3; // the fixture allocates two envelope descriptors
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.env_desc_id      = 0;
        bad.lfo_depth_source = 7;
        TEST(! Sculptor::validate_editor_metadata(bank));
        bad.lfo_depth_source = 0;
        TEST(Sculptor::validate_editor_metadata(bank));
    }

    // Kind-4 records (effect-graph node places): keyed by name, valid on
    // every chain including the master, refusing wrong-kind key fields,
    // duplicates and empty names, and round-tripping through the bank file.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        Synth::GraphNodeLayout& fx = bank.graph_layout[bank.graph_layout_count++];
        fx                         = {};
        fx.channel                 = 0;
        fx.zone                    = 0;
        fx.kind                    = 4;
        snprintf(fx.name, sizeof(fx.name), "Delay");
        fx.x = 320.0f;
        TEST(Sculptor::validate_editor_metadata(bank));
        fx.channel = static_cast<uint8_t>(Sculptor::fx_master_chain);
        TEST(Sculptor::validate_editor_metadata(bank));

        static char                        doc[512 * 1024];
        static Synth::InstrumentEditorBank decoded;
        const uint32_t                     written = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(written > 0);
        TEST(Synth::decode_editor_bank_json(doc, written, &decoded));
        TEST(decoded.graph_layout_count == bank.graph_layout_count);
        TEST(strcmp(decoded.graph_layout[decoded.graph_layout_count - 1].name, "Delay") == 0);
        TEST(decoded.graph_layout[decoded.graph_layout_count - 1].kind == 4);
        TEST(Sculptor::validate_editor_metadata(decoded));

        Synth::GraphNodeLayout& duplicate = bank.graph_layout[bank.graph_layout_count++];
        duplicate                         = fx;
        TEST(! Sculptor::validate_editor_metadata(bank)); // same name twice on one chain
        bank.graph_layout_count--;
        fx.index = 1;
        TEST(! Sculptor::validate_editor_metadata(bank));
        fx.index = 0;
        fx.zone  = 1;
        TEST(! Sculptor::validate_editor_metadata(bank));
        fx.zone = 0;
        fx.uid  = 1;
        TEST(! Sculptor::validate_editor_metadata(bank));
        fx.uid     = 0;
        fx.name[0] = 0;
        TEST(! Sculptor::validate_editor_metadata(bank));
        bank.graph_layout_count--; // drop the fx record again
        TEST(Sculptor::validate_editor_metadata(bank));
    }

    // Record validation: kind 3 is accepted and the node-pool check counts
    // parameter nodes.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
        add_parameter_record(&bank, 0, 0, 1, 30.0f, 40.0f, 1);
        TEST(Sculptor::validate_editor_metadata(bank));

        // 7 fixed + 3 oscillators + 3 parameters + 2 envelope + 1 LFO
        // descriptor nodes.
        TEST(Sculptor::count_projected_nodes(bank, 0, 0) == 11);
    }

    // Capacity: the reshaped projection fits the decided slot budget.
    {
        TEST(Sculptor::max_node_slots >= 53); // 5-param effects carrying the full modulation grammar
    }

    // Merge-demoted truthfulness across three groups: when B merges into
    // A, B's demoted record must not claim the unrelated recordless C
    // group at re-projection. Tuple-class matching alone would let it:
    // C is tuple-bearing and unclaimed, and the claim would rename C's
    // node and inject B's LFO wiring into it.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        // A third volume group: layer 2's tuple becomes envelope-1 only.
        bank.bank.instruments.entries[0].layers[2].gen[Synth::mod_volume].lfo_desc_id = 0;
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            const uint32_t b0 = find_graph_node_by_name(graph, "Volume 2");
            const uint32_t c0 = find_graph_node_by_name(graph, "Volume 3");
            TEST(a0 != Sculptor::pool_no_slot && b0 != Sculptor::pool_no_slot && c0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot || b0 == Sculptor::pool_no_slot || c0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            graph.rename_node(b0, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            const int32_t b_param = Sculptor::find_param(mapping, b0);
            TEST(a_param >= 0 && b_param >= 0);
            if (a_param < 0 || b_param < 0) {
                break;
            }
            const int32_t ay_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_param].uid));
            const int32_t bee_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[b_param].uid));
            TEST(ay_record >= 0 && bee_record >= 0);
            if (ay_record < 0 || bee_record < 0) {
                break;
            }
            // Rewire B onto A's generator instances and copy A's LFO row
            // values; all validator-legal gestures. All mutations land
            // before the drained batch applies, so every reconcile during
            // the drain sees the final state.
            uint32_t env1 = Sculptor::pool_no_slot;
            uint32_t lfo1 = Sculptor::pool_no_slot;
            for (uint32_t d = 0; d < mapping.detached_count; d++) {
                if (mapping.detached[d].kind == 1 && mapping.detached[d].desc_id == 1) {
                    env1 = mapping.detached[d].node_idx;
                }
                if (mapping.detached[d].kind == 2 && mapping.detached[d].desc_id == 1) {
                    lfo1 = mapping.detached[d].node_idx;
                }
            }
            TEST(env1 != Sculptor::pool_no_slot && lfo1 != Sculptor::pool_no_slot);
            if (env1 == Sculptor::pool_no_slot || lfo1 == Sculptor::pool_no_slot) {
                break;
            }
            const uint32_t b_env_wire = Sculptor::connection_into(graph, b0, 2);
            TEST(b_env_wire != Sculptor::pool_no_slot);
            if (b_env_wire == Sculptor::pool_no_slot) {
                break;
            }
            graph.delete_connection(b_env_wire);
            TEST(graph.add_connection(Sculptor::EndPoint{ env1, 0 }, Sculptor::EndPoint{ b0, 2 }) !=
                 Sculptor::pool_no_slot);
            TEST(graph.add_connection(Sculptor::EndPoint{ lfo1, 0 }, Sculptor::EndPoint{ b0, 3 }) !=
                 Sculptor::pool_no_slot);
            Sculptor::PropertyValue op_copy = {};
            op_copy.list_index              = graph.node(a0).slots.entries[4].value.list_index;
            graph.set_slot_value(b0, 4, op_copy);
            Sculptor::PropertyValue depth_copy = {};
            depth_copy.real                    = graph.node(a0).slots.entries[5].value.real;
            graph.set_slot_value(b0, 5, depth_copy);
            Sculptor::PropertyValue rate_copy = {};
            rate_copy.real                    = graph.node(a0).slots.entries[6].value.real;
            graph.set_slot_value(b0, 6, rate_copy);
            static Sculptor::GraphChange batch[8] = {};
            const uint32_t               moved    = graph.take_changes(batch, 8);
            for (uint32_t e = 0; e < moved; e++) {
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[e]));
            }
            // The merge-aware re-stamp keeps A's ordinal and demotes B's.
            TEST(bank.graph_layout[ay_record].param_slot == 1);
            TEST(bank.graph_layout[bee_record].param_slot == Synth::graph_record_param_free);
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            const uint32_t ay  = find_graph_node_by_name(graph, "Ay");
            const uint32_t bee = find_graph_node_by_name(graph, "Bee");
            const uint32_t cc  = find_graph_node_by_name(graph, "Volume 2");
            TEST(ay != Sculptor::pool_no_slot && bee != Sculptor::pool_no_slot && cc != Sculptor::pool_no_slot);
            if (ay == Sculptor::pool_no_slot || bee == Sculptor::pool_no_slot || cc == Sculptor::pool_no_slot) {
                break;
            }
            // B's record keeps its own identity: it materializes an inert
            // surplus node and leaves C's recordless group untouched.
            // Re-resolve the instance node after the re-projection: the
            // rebuild recreates the detached nodes.
            env1 = Sculptor::pool_no_slot;
            for (uint32_t d = 0; d < mapping.detached_count; d++) {
                if (mapping.detached[d].kind == 1 && mapping.detached[d].desc_id == 1) {
                    env1 = mapping.detached[d].node_idx;
                }
            }
            TEST(env1 != Sculptor::pool_no_slot);
            uint32_t bee_rows      = 0;
            uint32_t c_env_wires   = 0;
            uint32_t c_lfo_wires   = 0;
            uint32_t c_env_from_e1 = 0;
            for (uint32_t c = 0; c < Sculptor::max_connections; c++) {
                if (! graph.connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& connection = graph.get_connection(c);
                if (connection.output.node_idx == bee && connection.output.slot_idx == mapping.param_output_slot) {
                    bee_rows++;
                }
                if (connection.input.node_idx == cc && connection.input.slot_idx == 2) {
                    c_env_wires++;
                    if (connection.output.node_idx == env1) {
                        c_env_from_e1++;
                    }
                }
                if (connection.input.node_idx == cc && connection.input.slot_idx == 3) {
                    c_lfo_wires++;
                }
            }
            TEST(bee_rows == 0);
            TEST(c_env_wires == 1 && c_env_from_e1 == 1);
            TEST(c_lfo_wires == 0);
        } while (false);
    }

    // The Add Parameter preflight predicate refuses at the registry bound
    // and while a record-less derived sibling could be hijacked, and stays
    // silent when every derived parameter carries its record.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                bank.bank.instruments.entries[0].layers[layer].gen[t]  = Synth::LayerGen{};
                bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
            }
        }
        for (uint32_t uid = 1; uid <= Sculptor::max_param_nodes; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
        }
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            TEST(mapping.param_count == Sculptor::max_param_nodes);
            char error[128] = {};
            TEST(Sculptor::osc_add_parameter_refused(mapping, 0, error, sizeof(error)));
            TEST(strncmp(error,
                         "Synth: cannot add a parameter: the parameter registry is full",
                         sizeof("Synth: cannot add a parameter: the parameter registry is full")) == 0);
        } while (false);
        // Record-less derived sibling: renaming one of the two fixture
        // parameters leaves the other derived and record-less.
        static Synth::InstrumentEditorBank bank2;
        static Synth::Instrument           instrument2 = {};
        build_parameter_fixture(&bank2, &instrument2);
        static Sculptor::Graph           graph2;
        static Sculptor::OscGraphMapping mapping2;
        do {
            TEST(Sculptor::project_editor_to_graph(bank2, &graph2, &mapping2));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph2.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph2, "Volume");
            TEST(a0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot) {
                break;
            }
            graph2.rename_node(a0, "Ay");
            (void)graph2.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank2, &graph2, &mapping2, rename0));
            char error[128] = {};
            TEST(Sculptor::osc_add_parameter_refused(mapping2, 0, error, sizeof(error)));
            TEST(strncmp(error,
                         "Synth: cannot add a parameter: rename the target's earlier parameter first",
                         sizeof("Synth: cannot add a parameter: rename the target's earlier parameter first")) == 0);
        } while (false);
        // Clean case: both fixture parameters renamed, nothing refuses.
        static Synth::InstrumentEditorBank bank3;
        static Synth::Instrument           instrument3 = {};
        build_parameter_fixture(&bank3, &instrument3);
        static Sculptor::Graph           graph3;
        static Sculptor::OscGraphMapping mapping3;
        do {
            TEST(Sculptor::project_editor_to_graph(bank3, &graph3, &mapping3));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph3.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph3, "Volume");
            const uint32_t b0 = find_graph_node_by_name(graph3, "Volume 2");
            TEST(a0 != Sculptor::pool_no_slot && b0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot || b0 == Sculptor::pool_no_slot) {
                break;
            }
            graph3.rename_node(a0, "Ay");
            (void)graph3.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank3, &graph3, &mapping3, rename0));
            graph3.rename_node(b0, "Bee");
            (void)graph3.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b0;
            TEST(Sculptor::apply_osc_graph_change(&bank3, &graph3, &mapping3, rename1));
            char error[128] = {};
            TEST(! Sculptor::osc_add_parameter_refused(mapping3, 0, error, sizeof(error)));
            TEST(error[0] == '\0');
        } while (false);
    }

    // Eventless LFO-row edits merge groups through the value_changed
    // path: two groups differing only in one LFO row value merge when a
    // value_changed event aligns the value, with no explicit refresh and
    // without any wiring change.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        do {
            TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
            static Sculptor::GraphChange discard[8] = {};
            (void)graph.take_changes(discard, 8);
            const uint32_t a0 = find_graph_node_by_name(graph, "Volume");
            const uint32_t b0 = find_graph_node_by_name(graph, "Volume 2");
            TEST(a0 != Sculptor::pool_no_slot && b0 != Sculptor::pool_no_slot);
            if (a0 == Sculptor::pool_no_slot || b0 == Sculptor::pool_no_slot) {
                break;
            }
            graph.rename_node(a0, "Ay");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename0 = {};
            rename0.kind                         = Sculptor::ChangeKind::name_changed;
            rename0.node_idx                     = a0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename0));
            graph.rename_node(b0, "Bee");
            (void)graph.take_changes(discard, 8);
            static Sculptor::GraphChange rename1 = {};
            rename1.kind                         = Sculptor::ChangeKind::name_changed;
            rename1.node_idx                     = b0;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, rename1));
            const int32_t a_param = Sculptor::find_param(mapping, a0);
            const int32_t b_param = Sculptor::find_param(mapping, b0);
            TEST(a_param >= 0 && b_param >= 0);
            if (a_param < 0 || b_param < 0) {
                break;
            }
            const int32_t ay_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[a_param].uid));
            const int32_t bee_record =
                Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(mapping.params[b_param].uid));
            TEST(ay_record >= 0 && bee_record >= 0);
            if (ay_record < 0 || bee_record < 0) {
                break;
            }
            // Rewire B onto A's generator instances eventlessly (the
            // apply-side mirroring API pushes no events, so no reconcile
            // runs mid-rewrite and B's explicit ordinal survives), then
            // make the depth row the only tuple difference.
            uint32_t env1 = Sculptor::pool_no_slot;
            uint32_t lfo1 = Sculptor::pool_no_slot;
            for (uint32_t d = 0; d < mapping.detached_count; d++) {
                if (mapping.detached[d].kind == 1 && mapping.detached[d].desc_id == 1) {
                    env1 = mapping.detached[d].node_idx;
                }
                if (mapping.detached[d].kind == 2 && mapping.detached[d].desc_id == 1) {
                    lfo1 = mapping.detached[d].node_idx;
                }
            }
            TEST(env1 != Sculptor::pool_no_slot && lfo1 != Sculptor::pool_no_slot);
            if (env1 == Sculptor::pool_no_slot || lfo1 == Sculptor::pool_no_slot) {
                break;
            }
            graph.set_slot_input(b0, 2, Sculptor::EndPoint{ env1, 0 });
            graph.set_slot_input(b0, 3, Sculptor::EndPoint{ lfo1, 0 });
            Sculptor::PropertyValue op_copy = {};
            op_copy.list_index              = graph.node(a0).slots.entries[4].value.list_index;
            graph.set_slot_value(b0, 4, op_copy);
            Sculptor::PropertyValue rate_copy = {};
            rate_copy.real                    = graph.node(a0).slots.entries[6].value.real;
            graph.set_slot_value(b0, 6, rate_copy);
            Sculptor::PropertyValue depth_copy = {};
            depth_copy.real                    = graph.node(a0).slots.entries[5].value.real + 50.0f;
            graph.set_slot_value(b0, 5, depth_copy);
            TEST(Sculptor::refresh_osc_graph_compilation(&bank, graph, mapping, 0, 0));
            TEST(bank.graph_layout[ay_record].param_slot == 1);
            TEST(bank.graph_layout[bee_record].param_slot == 2);
            // The value_changed event aligns the depth row: the groups
            // merge through apply_value_change's reconcile, before any
            // explicit refresh.
            depth_copy.real = graph.node(a0).slots.entries[5].value.real;
            graph.set_slot_value(b0, 5, depth_copy);
            static Sculptor::GraphChange batch[8] = {};
            batch[0].kind                         = Sculptor::ChangeKind::value_changed;
            batch[0].node_idx                     = b0;
            batch[0].slot_idx                     = 5;
            TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, batch[0]));
            TEST(bank.graph_layout[ay_record].param_slot == 1);
            TEST(bank.graph_layout[bee_record].param_slot == Synth::graph_record_param_free);
            // The merged tuple compiles: both layers carry the same LFO
            // depth now.
            TEST(bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume].lfo_depth ==
                 bank.bank.instruments.entries[0].layers[1].gen[Synth::mod_volume].lfo_depth);
        } while (false);
    }

    // The implied parameter count is a validation bound: 35 free-standing
    // parameters validate, 36 do not (the projection would refuse the
    // 36th and leave the zone's oscillator editor unavailable).
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                bank.bank.instruments.entries[0].layers[layer].gen[t]  = Synth::LayerGen{};
                bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
            }
        }
        for (uint32_t uid = 1; uid <= Sculptor::max_param_nodes; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
        }
        TEST(Sculptor::validate_editor_metadata(bank));
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(Sculptor::max_param_nodes + 1));
        TEST(! Sculptor::validate_editor_metadata(bank));
    }

    // The validation parameter bound follows the projection's attachment
    // decisions, not raw record counts: dormant records (persisted value
    // wires, no generator tuple) cannot attach to the fixture's derived
    // groups and every one of them materializes a parameter node. With the
    // two derived groups, 33 dormant records sit exactly at the bound and
    // the 34th crosses it.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        // The fixture's two derived groups (volume env1+lfo1 and pitch
        // env2, both on layer 0), no MIDI routing: dormant records have no
        // carrier to claim.
        for (uint32_t layer = 1; layer < Synth::max_layers; layer++) {
            bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume] = Synth::LayerGen{};
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
        }
        for (uint32_t uid = 1; uid < Sculptor::max_param_nodes - 1; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
            const int32_t rec = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(uid));
            TEST(rec >= 0);
            if (rec < 0) {
                break;
            }
            bank.graph_layout[rec].served = 1;
        }
        TEST(Sculptor::validate_editor_metadata(bank));
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        const int32_t last =
            Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        TEST(last >= 0);
        if (last >= 0) {
            bank.graph_layout[last].served = 1;
        }
        TEST(! Sculptor::validate_editor_metadata(bank));
    }

    // A record whose persisted tuple names a different generator than the
    // derived groups cannot attach either (the persisted-tuple
    // verification), so it also materializes a parameter node at the
    // validation bound.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 1; layer < Synth::max_layers; layer++) {
            bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume] = Synth::LayerGen{};
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
        }
        for (uint32_t uid = 1; uid < Sculptor::max_param_nodes - 1; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
            const int32_t rec = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(uid));
            TEST(rec >= 0);
            if (rec < 0) {
                break;
            }
            bank.graph_layout[rec].env_desc_id = 2;
        }
        TEST(Sculptor::validate_editor_metadata(bank));
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        const int32_t last =
            Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        TEST(last >= 0);
        if (last >= 0) {
            bank.graph_layout[last].env_desc_id = 2;
        }
        TEST(! Sculptor::validate_editor_metadata(bank));
    }

    // An env-only derived group whose gen cell carries dormant LFO source
    // fields must compare against the projection's live tuple, which zeros
    // the sources while no LFO edge exists: a record naming the dormant
    // source cannot attach, so the validation bound must treat it as
    // surplus exactly like the projection does.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume] = Synth::LayerGen{};
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            bank.bank.instruments.entries[0].routing[t].num_inputs = 0;
        }
        // One env-only volume group on layer 0 with a dormant depth source.
        Synth::LayerGen& gen = bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_volume];
        gen.envelope_desc_id = 1;
        gen.lfo_desc_id      = 0;
        gen.lfo_depth_source = Synth::ModSource::velocity;
        gen.lfo_rate_source  = Synth::ModSource::none;
        for (uint32_t uid = 1; uid < Sculptor::max_param_nodes - 1; uid++) {
            add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(uid));
            const int32_t rec = Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(uid));
            TEST(rec >= 0);
            if (rec < 0) {
                break;
            }
            bank.graph_layout[rec].served           = 1;
            bank.graph_layout[rec].env_desc_id      = 1;
            bank.graph_layout[rec].lfo_depth_source = static_cast<uint8_t>(Synth::ModSource::velocity);
        }
        TEST(Sculptor::validate_editor_metadata(bank));
        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        // A bare explicit record can claim the live env-only group even
        // though earlier UID records name its dormant (unwired) source.
        add_parameter_record(&bank, 0, 0, 0, 73.0f, -19.0f, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        Synth::GraphNodeLayout& saved = bank.graph_layout[bank.graph_layout_count - 1];
        saved.param_slot              = 1;
        saved.width_override          = 211.0f;
        saved.height_override         = 97.0f;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        TEST(Sculptor::validate_editor_metadata(bank));
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        TEST(mapping.params[0].uid == saved.uid);
        TEST(graph.node(mapping.params[0].node_idx).position.x == saved.x);
        uint32_t parameter_count = 0;
        Sculptor::count_projected_nodes(bank, 0, 0, &parameter_count);
        TEST(parameter_count == mapping.param_count);
        Synth::InstrumentGraphLayout exported[max_portable_records];
        uint32_t                     exported_count = 0;
        TEST(Sculptor::encode_instrument_graph_layout(bank.bank.instruments.entries[0],
                                                      bank.bank,
                                                      bank,
                                                      0,
                                                      0,
                                                      exported,
                                                      max_portable_records,
                                                      &exported_count));
        uint32_t saved_parameters = 0;
        for (uint32_t i = 0; i < exported_count; ++i) {
            if (exported[i].kind == 3) {
                ++saved_parameters;
                TEST(exported[i].target == Synth::mod_volume && exported[i].parameter_ordinal == 0);
                TEST(exported[i].x == graph.node(mapping.params[0].node_idx).position.x);
                TEST(exported[i].y == graph.node(mapping.params[0].node_idx).position.y);
                TEST(exported[i].width_override == saved.width_override);
                TEST(exported[i].height_override == saved.height_override);
            }
        }
        TEST(saved_parameters == 1);
        --bank.graph_layout_count;
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        const int32_t last =
            Sculptor::find_record(bank, 0, 0, 3, 0, static_cast<uint8_t>(Sculptor::max_param_nodes - 1));
        TEST(last >= 0);
        if (last >= 0) {
            bank.graph_layout[last].served           = 1;
            bank.graph_layout[last].env_desc_id      = 1;
            bank.graph_layout[last].lfo_depth_source = static_cast<uint8_t>(Synth::ModSource::velocity);
        }
        TEST(! Sculptor::validate_editor_metadata(bank));
    }

    // Reclaim roots and remaps kind-3 records' descriptor references: a
    // record may be the only referencer of an envelope, and a stale id
    // after compaction would fail validation forever.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        // Only the records reference envelope 2: clear the gens that use it.
        for (uint32_t layer = 1; layer < Synth::max_layers; layer++) {
            bank.bank.instruments.entries[0].layers[layer].gen[Synth::mod_volume] = Synth::LayerGen{};
        }
        add_parameter_record(&bank, 0, 0, 0, 10.0f, 20.0f, 1);
        const int32_t rec = Sculptor::find_record(bank, 0, 0, 3, 0, 1);
        TEST(rec >= 0);
        if (rec >= 0) {
            bank.graph_layout[rec].env_desc_id = 2;
        }
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.envelopes.num_allocated == 2);
        TEST(rec < 0 || bank.graph_layout[rec].env_desc_id == 2);
        // A third descriptor referenced by nothing is freed, and the
        // record's reference to the last descriptor remaps down.
        const uint32_t third = bank.bank.envelopes.allocate();
        TEST(third != pool_no_slot);
        if (third != pool_no_slot) {
            bank.bank.envelopes.entries[third] = Synth::EnvelopeDescriptor{};
        }
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.envelopes.num_allocated == 2);
        TEST(rec < 0 || bank.graph_layout[rec].env_desc_id == 2);
    }

    // The envelope evaluator holds the start value on unplaced descriptors:
    // adjacent position-0 points never advance, so the interpolation must
    // not divide by a zero duration.
    {
        static Synth::EnvelopeDescriptor env = {};
        env.num_points                       = 2;
        env.sustain_first_point              = 0;
        env.sustain_last_point               = 1;
        env.min_value                        = 0.0f;
        env.min_max_delta                    = 2.0f;
        env.points[0].position               = 0;
        env.points[0].value                  = 0x2000;
        env.points[1].position               = 0;
        env.points[1].value                  = 0x4000;
        static Synth::EnvelopeState state    = {};
        for (uint32_t i = 0; i < 8; i++) {
            const float value = Synth::eval_envelope(env, &state, true);
            TEST(value == 0x2000 * 2.0f);
        }
    }

    // The decoder rejects objects in key position: the grammar admits only
    // comma-separated string keys, at the root and in unknown subtrees.
    {
        static Synth::InstrumentEditorBank bank;
        static Synth::InstrumentEditorBank decoded;
        static Synth::Instrument           instrument = {};
        build_parameter_fixture(&bank, &instrument);
        static char doc[512];
        snprintf(doc, sizeof(doc), "{\"instrument_editor_bank\":{}{\"editor\":{}}}");
        TEST(! Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &decoded));
        snprintf(doc, sizeof(doc), "{\"instrument_editor_bank\":{},\"x\":{\"a\":1}{\"b\":2}}");
        TEST(! Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &decoded));
    }

    // Envelope curve edit constraints: the editing helpers accept only
    // requests that leave the descriptor valid, clamp into the free
    // neighbor range, and leave the output untouched on rejection.
    {
        Synth::EnvelopeDescriptor base = {};
        base.num_points                = 4;
        base.sustain_first_point       = 1;
        base.sustain_last_point        = 2;
        base.min_value                 = -1.0f;
        base.min_max_delta             = 2.0f;
        base.points[0]                 = { 0, 0x2000 };
        base.points[1]                 = { 100, 0x8000 };
        base.points[2]                 = { 200, 0x4000 };
        base.points[3]                 = { 300, 0 };

        TEST(Sculptor::env_positions_strictly_increasing(base));

        // A descriptor with duplicate positions (the hand-built-JSON zero
        // run) is a read-only source: every edit rejects and leaves the
        // output untouched.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.points[2].position        = 100;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 2, &out));
            TEST(! Sculptor::env_set_sustain_first(bad, 3, &out));
            TEST(out.num_points == 4 && out.points[1].position == 100);
            TEST(out.points[2].position == 200 && out.sustain_last_point == 2);
        }
        // The validator's leading run of zero positions is equally read-only
        // for editing, and the sustain setters repair nothing there.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.points[1].position        = 0;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 2, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 2, false, &out));
            TEST(! Sculptor::env_remove_point(bad, 3, &out));
            TEST(! Sculptor::env_set_sustain_last(bad, 3, &out));
            TEST(out.num_points == 4 && out.points[3].position == 300);
        }
        // A point count beyond the array bound rejects before any indexing.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.num_points                = Synth::max_envelope_points + 1;
            TEST(! Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 1, &out));
            TEST(! Sculptor::env_set_sustain_first(bad, 1, &out));
            TEST(out.num_points == 4);
        }
        // Sustain indices outside the point range make move/insert/remove
        // reject, while the sustain setters can repair the descriptor.
        {
            Synth::EnvelopeDescriptor bad = base;
            bad.sustain_first_point       = 2;
            bad.sustain_last_point        = 1;
            TEST(Sculptor::env_positions_strictly_increasing(bad));
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(bad, 1, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(bad, 1, true, &out));
            TEST(! Sculptor::env_remove_point(bad, 2, &out));
            TEST(Sculptor::env_set_sustain_last(bad, 3, &out));
            TEST(out.sustain_first_point == 2 && out.sustain_last_point == 3);
            TEST(out.num_points == 4 && out.points[1].position == 100);
        }

        // Out-of-range point indices reject.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_move_point(base, 4, 50, 0x1000, &out));
            TEST(! Sculptor::env_insert_point(base, 4, true, &out));
            TEST(! Sculptor::env_remove_point(base, 4, &out));
            TEST(! Sculptor::env_set_sustain_last(base, 4, &out));
            TEST(out.num_points == 4);
        }

        // Moves clamp strictly between the neighbors (the first point into
        // [0, next-1], the last into [prev+1, 65535]).
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 1, 150, 0xFFFF, &out));
            TEST(out.points[1].position == 150 && out.points[1].value == 0xFFFF);
            TEST(out.num_points == 4 && out.sustain_first_point == 1 && out.sustain_last_point == 2);
            TEST(out.points[0].position == 0 && out.points[2].position == 200);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 1, 250, 0x8000, &out));
            TEST(out.points[1].position == 199);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 2, 10, 0x8000, &out));
            TEST(out.points[2].position == 101);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 0, 5000, 0x1234, &out));
            TEST(out.points[0].position == 99 && out.points[0].value == 0x1234);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_move_point(base, 3, 0, 0x1234, &out));
            TEST(out.points[3].position == 201);
            TEST(out.points[3].value == 0x1234);
        }

        // Inserts copy the reference value; interior insertions land at the
        // midpoint of the free range, endpoint insertions just past the
        // endpoint.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(base, 3, true, &out));
            TEST(out.num_points == 5);
            TEST(out.points[4].position == 301 && out.points[4].value == 0);
            TEST(out.points[3].position == 300 && out.points[0].position == 0);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 2);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(base, 1, true, &out));
            TEST(out.num_points == 5);
            TEST(out.points[2].position == 150 && out.points[2].value == 0x8000);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 3);
            // In-place insertion copies the reference value, not whatever
            // the shift has already overwritten.
            Synth::EnvelopeDescriptor in_place = base;
            TEST(Sculptor::env_insert_point(in_place, 1, false, &in_place));
            TEST(in_place.num_points == 5);
            TEST(in_place.points[1].position == 50 && in_place.points[1].value == 0x8000);
            TEST(in_place.points[2].position == 100 && in_place.points[2].value == 0x8000);
        }
        // Inserting before the sustain start shifts the whole span right.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[0].position        = 100;
            src.points[1].position        = 200;
            src.points[2].position        = 300;
            src.points[3].position        = 400;
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_insert_point(src, 0, false, &out));
            TEST(out.num_points == 5);
            TEST(out.points[0].position == 99);
            TEST(out.points[1].position == 100);
            TEST(out.sustain_first_point == 2 && out.sustain_last_point == 3);
        }
        // Before the first point at tick 0 has no free tick above it.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(base, 0, false, &out));
            TEST(out.num_points == 4);
        }
        // Adjacent interior points leave no free tick on either side.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[1].position        = 99;
            src.points[2].position        = 100;
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 1, true, &out));
            TEST(! Sculptor::env_insert_point(src, 2, false, &out));
            TEST(out.num_points == 4);
        }
        // A last point at the top of the range leaves no room after it.
        {
            Synth::EnvelopeDescriptor src = base;
            src.points[3].position        = 65535;
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 3, true, &out));
            TEST(out.num_points == 4);
        }
        // Single-point envelopes can only grow forward.
        {
            Synth::EnvelopeDescriptor src = {};
            src.num_points                = 1;
            src.sustain_first_point       = 0;
            src.sustain_last_point        = 0;
            src.points[0]                 = { 0, 0x8000 };
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 0, false, &out));
            TEST(Sculptor::env_insert_point(src, 0, true, &out));
            TEST(out.num_points == 2);
            TEST(out.points[1].position == 1 && out.points[1].value == 0x8000);
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
        }
        // The eight-point cap rejects further insertions.
        {
            Synth::EnvelopeDescriptor src = base;
            src.num_points                = Synth::max_envelope_points;
            for (uint32_t i = 1; i < Synth::max_envelope_points; ++i) {
                src.points[i].position = static_cast<uint16_t>(i * 100);
            }
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_insert_point(src, 3, true, &out));
            TEST(! Sculptor::env_insert_point(src, 3, false, &out));
            TEST(out.num_points == 4);
        }

        // Removals reject point 0 and the last remaining point, shift the
        // tail left, zero the vacated slot, and move or clamp the sustain
        // span.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(! Sculptor::env_remove_point(base, 0, &out));
            TEST(out.num_points == 4);
            Synth::EnvelopeDescriptor one = {};
            one.num_points                = 1;
            one.points[0]                 = { 0, 0x8000 };
            TEST(! Sculptor::env_remove_point(one, 0, &out));
            TEST(out.num_points == 4);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(base, 1, &out));
            TEST(out.num_points == 3);
            TEST(out.points[0].position == 0 && out.points[1].position == 200 && out.points[2].position == 300);
            TEST(out.points[3].position == 0 && out.points[3].value == 0);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 1);
        }
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(base, 3, &out));
            TEST(out.num_points == 3);
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 2);
        }
        // Removing the last point of a two-point envelope clamps the
        // sustain span back onto the survivor.
        {
            Synth::EnvelopeDescriptor src = {};
            src.num_points                = 2;
            src.sustain_first_point       = 1;
            src.sustain_last_point        = 1;
            src.points[0]                 = { 0, 0x2000 };
            src.points[1]                 = { 100, 0x8000 };
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_remove_point(src, 1, &out));
            TEST(out.num_points == 1);
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
        }

        // Sustain bounds move the other bound when they would cross.
        {
            Synth::EnvelopeDescriptor out = base;
            TEST(Sculptor::env_set_sustain_first(base, 0, &out));
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 2);
            TEST(Sculptor::env_set_sustain_first(base, 3, &out));
            TEST(out.sustain_first_point == 3 && out.sustain_last_point == 3);
            TEST(Sculptor::env_set_sustain_last(base, 0, &out));
            TEST(out.sustain_first_point == 0 && out.sustain_last_point == 0);
            TEST(Sculptor::env_set_sustain_last(base, 3, &out));
            TEST(out.sustain_first_point == 1 && out.sustain_last_point == 3);
        }

        // Tick conversions: one tick is 256 samples at 44100 Hz; ms round
        // trip through nearest-tick rounding.
        {
            TEST(approx(Sculptor::envelope_ms_per_tick, 256000.0f / 44100.0f, 0.0f));
            TEST(approx(Sculptor::envelope_ticks_to_ms(172), 172.0f * Sculptor::envelope_ms_per_tick, 0.0f));
            TEST(Sculptor::envelope_ms_to_ticks(0.0f) == 0);
            TEST(Sculptor::envelope_ms_to_ticks(-1.0f) == 0);
            TEST(Sculptor::envelope_ms_to_ticks(Sculptor::envelope_ms_per_tick) == 1);
            TEST(Sculptor::envelope_ms_to_ticks(1000.0f) == 172);
            TEST(Sculptor::envelope_ms_to_ticks(1.0e9f) == 65535);
            const uint16_t ticks = Sculptor::envelope_ms_to_ticks(123.0f);
            TEST(approx(Sculptor::envelope_ticks_to_ms(ticks), 123.0f, 3.0f));
        }
    }

    // A widget-style edit sequence on a real bank stays valid and survives
    // the JSON round trip byte for byte.
    {
        static Synth::InstrumentEditorBank bank;
        make_valid_bank(bank.bank); // instrument 0 (env 1, lfo 1) on ch0

        Synth::EnvelopeDescriptor env = bank.bank.envelopes.entries[0];
        TEST(Sculptor::env_insert_point(env, 1, true, &env));
        TEST(Sculptor::env_move_point(env, 2, 250, 0xC000, &env));
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(Sculptor::env_set_sustain_last(env, 1, &env));
        // Delete down to a single point, then grow again from it.
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(env.num_points == 1);
        TEST(Sculptor::env_insert_point(env, 0, true, &env));
        TEST(env.num_points == 2);
        // Deleting the nonzero final point leaves a valid single-point
        // descriptor.
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(env.num_points == 1 && env.points[0].position == 0);
        bank.bank.envelopes.entries[0] = env;
        TEST(Synth::validate_instrument_bank(&bank.bank));

        static char    doc[128 * 1024];
        const uint32_t len = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0);
        static Synth::InstrumentEditorBank decoded;
        TEST(Synth::decode_editor_bank_json(doc, len, &decoded));
        TEST(decoded.bank.envelopes.num_allocated == bank.bank.envelopes.num_allocated);
        TEST(memcmp(&decoded.bank.envelopes.entries[0], &bank.bank.envelopes.entries[0], sizeof(env)) == 0);
        static char    doc2[128 * 1024];
        const uint32_t len2 = Synth::encode_editor_bank_json(&decoded, doc2, sizeof(doc2));
        TEST(len2 == len && memcmp(doc, doc2, len) == 0);
    }

    // ---- Effects graph projection and edit paths (sculptor_effect_graph) ----

    // Projection mirrors one channel chain; value edits, structural deletes,
    // move/retype and fresh-LFO semantics all hold on a validated bank.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain = bank.channel_chains[0];
        chain.num_effects                = 2;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        chain.effects[0].enabled                 = true;
        chain.effects[0].bindings[1].lfo_desc_id = 1;
        chain.effects[0].bindings[1].lfo_depth   = 0.5f;
        // The same descriptor referenced from another chain: shared content.
        Synth::EffectChainBinding& other = bank.channel_chains[1];
        other.num_effects                = 1;
        Sculptor::fx_init_slot(&other, 0, Synth::EffectType::chorus);
        other.effects[0].bindings[0].lfo_desc_id = 1;
        other.effects[0].bindings[0].lfo_depth   = 0.5f;
        // A MIDI source rides the Mix row (channel chains only).
        chain.effects[0].bindings[1].num_inputs       = 1;
        chain.effects[0].bindings[1].inputs[0].source = Synth::ModSource::mod_wheel;
        chain.effects[0].bindings[1].inputs[0].op     = Synth::SourceOp::multiply;
        chain.effects[0].bindings[1].inputs[0].scale  = 0.5f;
        TEST(Synth::validate_instrument_bank(&bank));

        static Sculptor::Graph              graph;
        static Sculptor::EffectGraphMapping mapping;
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
        TEST(mapping.chain == 0);
        TEST(mapping.effect_nodes[0] != Sculptor::pool_no_slot);
        TEST(mapping.effect_nodes[1] != Sculptor::pool_no_slot);
        TEST(mapping.effect_nodes[2] == Sculptor::pool_no_slot);
        TEST(mapping.lfo_count == 1 && mapping.lfo_nodes[0] != Sculptor::pool_no_slot);
        TEST(mapping.lfo_nodes[1] == Sculptor::pool_no_slot);
        TEST(graph.node(mapping.effect_nodes[0]).slots.num_allocated == 3 + Sculptor::fx_param_stride * 2);
        TEST(graph.node(mapping.lfo_nodes[0]).slots.num_allocated == 4);
        TEST(mapping.midi_node != Sculptor::pool_no_slot); // channel chain: MIDI routes in
        TEST(graph.node(mapping.midi_node).slots.num_allocated == Sculptor::fx_num_input_sources);

        // Serial wires plus one read-only reference wire onto the Mix row.
        TEST(find_fx_graph_connection(graph, { mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[1], 1 }, { mapping.output_node, 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph,
                                      { mapping.lfo_nodes[0], 0 },
                                      { mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(1) }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph,
                                      { mapping.midi_node, mapping.midi_source_slots[1] },
                                      { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) }) !=
             Sculptor::pool_no_slot);

        // Projected values mirror the chain and the descriptor.
        TEST(graph.node(mapping.effect_nodes[0]).slots.entries[2].value.list_index == 0);
        TEST(graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)].value.real ==
             chain.effects[0].bindings[0].base_value);
        TEST(graph.node(mapping.lfo_nodes[0]).slots.entries[3].value.integer == bank.lfos.entries[0].period_ms);

        // Unwired modulation knobs stay inert; the wired rows' knobs are live.
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(0) + 2));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(0) + 3));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 2));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 3));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 1));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 2));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) + 1));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) + 2));

        Sculptor::GraphChange change = {};
        change.kind                  = Sculptor::ChangeKind::value_changed;

        // Enabled row: the two-option list toggles the slot.
        change.node_idx                = mapping.effect_nodes[0];
        change.slot_idx                = 2;
        Sculptor::PropertyValue toggle = {};
        toggle.list_index              = 1;
        graph.set_slot_value(mapping.effect_nodes[0], 2, toggle);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(! chain.effects[0].enabled);
        toggle.list_index = 0;
        graph.set_slot_value(mapping.effect_nodes[0], 2, toggle);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].enabled);

        // Parameter row: the live slot value becomes the base value.
        change.slot_idx             = Sculptor::fx_param_row(1);
        Sculptor::PropertyValue mix = {};
        mix.real                    = 0.75f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, mix);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].base_value == 0.75f);

        // The Op and Depth rows ride the modulation line under the base slider.
        change.slot_idx            = Sculptor::fx_param_row(0) + 2;
        Sculptor::PropertyValue op = {};
        op.list_index              = 1;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, op);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_op == Synth::SourceOp::multiply);
        op.list_index = 0;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, op);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_op == Synth::SourceOp::add);

        change.slot_idx               = Sculptor::fx_param_row(0) + 3;
        Sculptor::PropertyValue depth = {};
        depth.real                    = 1.25f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, depth);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_depth == 1.25f);

        // A MIDI source row edit compiles the row back into the binding's
        // packed input list.
        change.slot_idx                = Sculptor::fx_param_src_dot(1, 0) + 2;
        Sculptor::PropertyValue amount = {};
        amount.real                    = 0.25f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, amount);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].num_inputs == 1);
        TEST(chain.effects[0].bindings[1].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(chain.effects[0].bindings[1].inputs[0].op == Synth::SourceOp::multiply);
        TEST(chain.effects[0].bindings[1].inputs[0].scale == 0.25f);

        // LFO descriptor edits reach every sharing chain at once.
        change.node_idx              = mapping.lfo_nodes[0];
        change.slot_idx              = 1;
        Sculptor::PropertyValue wave = {};
        wave.list_index              = 1;
        graph.set_slot_value(mapping.lfo_nodes[0], 1, wave);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].wave == Synth::WaveType::sawtooth_wave);
        wave.list_index = 0;
        graph.set_slot_value(mapping.lfo_nodes[0], 1, wave);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].wave == Synth::WaveType::sine_wave);

        change.slot_idx              = 2;
        Sculptor::PropertyValue duty = {};
        duty.real                    = 0.25f;
        graph.set_slot_value(mapping.lfo_nodes[0], 2, duty);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].duty == 64);

        change.slot_idx                = 3;
        Sculptor::PropertyValue period = {};
        period.integer                 = 500;
        graph.set_slot_value(mapping.lfo_nodes[0], 3, period);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].period_ms == 500);
        // A zero period would freeze the LFO: rejected, value kept.
        period.integer = 0;
        graph.set_slot_value(mapping.lfo_nodes[0], 3, period);
        TEST(! Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].period_ms == 500);

        // Deleting the LFO node clears only this chain's references.
        change.kind = Sculptor::ChangeKind::node_deleted;
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].lfo_desc_id == 0);
        TEST(other.effects[0].bindings[0].lfo_desc_id == 1);

        // Deleting an effect node removes the slot and shifts later slots down.
        change.node_idx = mapping.effect_nodes[1];
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.num_effects == 1);
        TEST(chain.effects[0].type == Synth::EffectType::distortion);
        TEST(chain.effects[1].type == Synth::EffectType::none);
        TEST(chain.effects[1].bindings[0].lfo_desc_id == 0);

        // Splice: front, after an anchor, out of range refuses.
        chain.num_effects = 2;
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        TEST(Sculptor::fx_splice_effect(&chain, 1, -1));
        TEST(chain.effects[0].type == Synth::EffectType::delay);
        TEST(chain.effects[1].type == Synth::EffectType::distortion);
        TEST(Sculptor::fx_splice_effect(&chain, 0, 1));
        TEST(chain.effects[0].type == Synth::EffectType::distortion);
        TEST(chain.effects[1].type == Synth::EffectType::delay);
        TEST(! Sculptor::fx_splice_effect(&chain, 0, 2));
        TEST(! Sculptor::fx_splice_effect(&chain, 2, 0));

        // Retype: neutral defaults, no dormant modulation of the old type.
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::chorus);
        TEST(chain.effects[0].type == Synth::EffectType::chorus);
        TEST(chain.effects[0].bindings[0].base_value == Sculptor::effect_param_default(Synth::EffectType::chorus, 0));
        TEST(chain.effects[0].bindings[0].lfo_desc_id == 0);

        // New LFO: a fresh descriptor, unbound until a wire lands on a row.
        uint16_t desc_id = 0;
        TEST(Sculptor::allocate_default_lfo(&bank, &desc_id));
        TEST(desc_id == 2);
        TEST(bank.lfos.num_allocated == 2);
        TEST(chain.effects[0].bindings[0].lfo_desc_id == 0);
        TEST(bank.lfos.entries[1].wave == Synth::WaveType::sine_wave);
        TEST(bank.lfos.entries[1].period_ms == 250);

        // The modulation count spans every chain, disabled effects included.
        uint32_t num_modulated = 0;
        uint32_t state_bytes   = 0;
        Sculptor::count_effect_budgets(bank, &num_modulated, &state_bytes);
        TEST(num_modulated == 1); // the other chain's chorus; the fresh LFO is unwired
        TEST(state_bytes == 0);   // fx_init_slot leaves effects disabled

        // Full descriptor pool: refusal leaves the bank unmodified.
        while (bank.lfos.num_allocated < Synth::max_lfos) {
            bank.lfos.allocate();
        }
        uint16_t refused = 0;
        TEST(! Sculptor::allocate_default_lfo(&bank, &refused));
        TEST(refused == 0);
    }

    // Deleting a node through the widget API drops its projected wires
    // first, each drop reporting a connection event with a dead endpoint; the
    // apply path no-ops those byproducts (the freed node carried the wire) and
    // the node event alone removes the slot.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::distortion);
        enabled_effect(bank, 0, 1, Synth::EffectType::delay);
        TEST(Synth::validate_instrument_bank(&bank));

        static Sculptor::Graph              widget_graph;
        static Sculptor::EffectGraphMapping widget_mapping;
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &widget_graph, &widget_mapping));
        // Projection quiets event reporting until the first drain re-arms
        // the ring; drain once first, as the per-frame GUI flow does.
        Sculptor::GraphChange widget_changes[Sculptor::max_pending_changes];
        (void)widget_graph.take_changes(widget_changes, Sculptor::max_pending_changes);
        widget_graph.delete_node(widget_mapping.effect_nodes[1]);

        const uint32_t widget_count     = widget_graph.take_changes(widget_changes, Sculptor::max_pending_changes);
        bool           seen_node_delete = false;
        bool           seen_wire_drop   = false;
        for (uint32_t i = 0; i < widget_count; ++i) {
            seen_node_delete = seen_node_delete || widget_changes[i].kind == Sculptor::ChangeKind::node_deleted;
            seen_wire_drop   = seen_wire_drop || widget_changes[i].kind == Sculptor::ChangeKind::connection_deleted;
        }
        TEST(seen_node_delete);
        TEST(seen_wire_drop);

        bool drain_ok = true;
        for (uint32_t i = 0; i < widget_count && drain_ok; ++i) {
            drain_ok = Sculptor::apply_fx_graph_change(&bank, widget_graph, widget_mapping, widget_changes[i]);
        }
        TEST(drain_ok);
        TEST(bank.channel_chains[0].num_effects == 1);
        TEST(bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Wire gestures: an LFO Value wire onto a parameter dot binds the
    // descriptor, retargeting moves the binding, dragging a serial wire
    // reorders the chain, and a serial-wire disconnect is refused (the
    // wire carries the chain's audio).
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        enabled_effect(bank, 0, 1, Synth::EffectType::distortion);
        enabled_effect(bank, 0, 2, Synth::EffectType::delay);
        TEST(Synth::validate_instrument_bank(&bank));

        // The projection shows descriptor 1 only through the pin: nothing
        // references it until the first wire lands.
        bool pinned[Synth::max_lfos] = {};
        pinned[0]                    = true;

        static Sculptor::Graph              wire_graph;
        static Sculptor::EffectGraphMapping wire_mapping;
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &wire_graph, &wire_mapping, pinned));
        TEST(wire_mapping.lfo_count == 1);
        Sculptor::GraphChange wire_changes[Sculptor::max_pending_changes];
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes); // re-arm the ring

        Synth::EffectChainBinding& wire_chain = bank.channel_chains[0];
        const uint32_t             chorus     = wire_mapping.effect_nodes[0];

        // Bind: LFO Value -> chorus rate row.
        TEST(
            wire_graph.attempt_connection({ wire_mapping.lfo_nodes[0], 0 }, { chorus, Sculptor::fx_param_lfo_dot(0) }));
        const uint32_t bind_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(bind_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_added);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].lfo_desc_id == 1);
        TEST(wire_chain.effects[0].bindings[0].lfo_depth == 0.5f); // depth-0 fresh binding reads as broken
        TEST(Synth::validate_instrument_bank(&bank));

        // Retarget the wire to the depth row: the old binding clears.
        const uint32_t bind_wire = find_fx_graph_connection(wire_graph,
                                                            { wire_mapping.lfo_nodes[0], 0 },
                                                            { chorus, Sculptor::fx_param_lfo_dot(0) });
        TEST(bind_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(bind_wire, false, { chorus, Sculptor::fx_param_lfo_dot(1) }));
        const uint32_t retarget_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(retarget_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].lfo_desc_id == 0);
        TEST(wire_chain.effects[0].bindings[1].lfo_desc_id == 1);

        // Unbind: pulling the wire off the row clears the binding.
        wire_graph.delete_connection(bind_wire);
        const uint32_t unbind_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(unbind_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[1].lfo_desc_id == 0);

        // MIDI route: the routing node's mod-wheel dot onto the chorus rate
        // Source A row packs the row into the binding's input list.
        TEST(wire_graph.attempt_connection({ wire_mapping.midi_node, wire_mapping.midi_source_slots[1] },
                                           { chorus, Sculptor::fx_param_src_dot(0, 0) }));
        const uint32_t midi_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(midi_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_added);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].op == Synth::SourceOp::add);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].scale == 1.0f);
        TEST(Synth::validate_instrument_bank(&bank));

        // A second source on Source B keeps both inputs, packed in row order.
        TEST(wire_graph.attempt_connection({ wire_mapping.midi_node, wire_mapping.midi_source_slots[0] },
                                           { chorus, Sculptor::fx_param_src_dot(0, 1) }));
        const uint32_t midi2_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(midi2_count == 1);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 2);
        TEST(wire_chain.effects[0].bindings[0].inputs[1].source == Synth::ModSource::pitch_bend);
        TEST(Synth::validate_instrument_bank(&bank));

        // Unroute: pulling a source wire drops that row's input; the
        // remaining input compacts to the front of the list.
        const uint32_t midi_wire =
            find_fx_graph_connection(wire_graph,
                                     { wire_mapping.midi_node, wire_mapping.midi_source_slots[1] },
                                     { chorus, Sculptor::fx_param_src_dot(0, 0) });
        TEST(midi_wire != Sculptor::pool_no_slot);
        wire_graph.delete_connection(midi_wire);
        const uint32_t unroute_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(unroute_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::pitch_bend);

        // Cross-parameter retarget: pulling the Source B wire off param 0 and
        // dropping it on param 1's Source A moves the routing.  The row the wire
        // left must forget the source, or the move would become a copy.
        const uint32_t pitch_bend_wire =
            find_fx_graph_connection(wire_graph,
                                     { wire_mapping.midi_node, wire_mapping.midi_source_slots[0] },
                                     { chorus, Sculptor::fx_param_src_dot(0, 1) });
        TEST(pitch_bend_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(pitch_bend_wire, false, { chorus, Sculptor::fx_param_src_dot(1, 0) }));
        const uint32_t reroute_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(reroute_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 0);
        TEST(wire_chain.effects[0].bindings[1].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[1].inputs[0].source == Synth::ModSource::pitch_bend);
        TEST(Synth::validate_instrument_bank(&bank));

        // Reorder: retarget the distortion Out end of the delay's feed wire
        // onto the chorus Out dot, so the delay follows the chorus directly.
        const uint32_t delay_in_wire = find_fx_graph_connection(wire_graph,
                                                                { wire_mapping.effect_nodes[1], 1 },
                                                                { wire_mapping.effect_nodes[2], 0 });
        TEST(delay_in_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(delay_in_wire, true, { wire_mapping.effect_nodes[0], 1 }));
        const uint32_t reorder_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(reorder_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].type == Synth::EffectType::chorus);
        TEST(wire_chain.effects[1].type == Synth::EffectType::delay);
        TEST(wire_chain.effects[2].type == Synth::EffectType::distortion);
        TEST(Synth::validate_instrument_bank(&bank));

        // The editor re-projects after every structural commit: the mapping
        // is the projection's slot-to-node table, so a splice invalidates
        // it.  Re-project before the next gesture, exactly like the drain.
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &wire_graph, &wire_mapping, pinned));
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);

        // To the front: retarget the delay Out end of the distortion's feed
        // wire onto the fixed input node.  The bank order is chorus, delay,
        // distortion, so the distortion node now projects at slot 2.
        const uint32_t distor_in_wire = find_fx_graph_connection(wire_graph,
                                                                 { wire_mapping.effect_nodes[1], 1 },
                                                                 { wire_mapping.effect_nodes[2], 0 });
        TEST(distor_in_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(distor_in_wire, true, { wire_mapping.input_node, 0 }));
        const uint32_t front_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(front_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].type == Synth::EffectType::distortion);

        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &wire_graph, &wire_mapping, pinned));
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);

        // Disconnect: a serial wire refuses to die - it carries the chain's
        // audio, so the apply path rejects the delete and the chain keeps
        // the chorus.  The bank order is distortion, chorus, delay.
        const uint32_t chorus_in_wire = find_fx_graph_connection(wire_graph,
                                                                 { wire_mapping.effect_nodes[0], 1 },
                                                                 { wire_mapping.effect_nodes[1], 0 });
        TEST(chorus_in_wire != Sculptor::pool_no_slot);
        wire_graph.delete_connection(chorus_in_wire);
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(! Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.num_effects == 3);
        TEST(wire_chain.effects[0].type == Synth::EffectType::distortion);
        TEST(wire_chain.effects[1].type == Synth::EffectType::chorus);
        TEST(wire_chain.effects[2].type == Synth::EffectType::delay);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Master chain: LFO modulation is legal, every MIDI-driven source is not.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectChainBinding& master = bank.master_chain;
        master.num_effects                = 1;
        Sculptor::fx_init_slot(&master, 0, Synth::EffectType::reverb);
        master.effects[0].enabled                 = true;
        master.effects[0].bindings[0].lfo_desc_id = 1;
        master.effects[0].bindings[0].lfo_depth   = 0.25f;
        TEST(Synth::validate_instrument_bank(&bank));

        static Sculptor::Graph              graph;
        static Sculptor::EffectGraphMapping mapping;
        TEST(Sculptor::project_effect_chain_to_graph(bank, Sculptor::fx_master_chain, &graph, &mapping));
        TEST(mapping.chain == Sculptor::fx_master_chain);
        TEST(mapping.lfo_count == 1);
        TEST(mapping.midi_node == Sculptor::pool_no_slot); // the master chain has no MIDI routing node

        master.effects[0].bindings[0].lfo_depth_source = Synth::ModSource::pitch_bend;
        expect_invalid(bank);
        master.effects[0].bindings[0].lfo_depth_source = Synth::ModSource::none;
        master.effects[0].bindings[0].num_inputs       = 1;
        master.effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        expect_invalid(bank);

        // The same direct input is channel-wide state on a channel chain.
        // The bank validates as a whole, so the master's rejected binding must go
        // first.
        master.effects[0].bindings[0].num_inputs = 0;

        Synth::EffectChainBinding& channel = bank.channel_chains[0];
        channel.num_effects                = 1;
        Sculptor::fx_init_slot(&channel, 0, Synth::EffectType::reverb);
        channel.effects[0].enabled                      = true;
        channel.effects[0].bindings[0].num_inputs       = 1;
        channel.effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Widest projection: 4 effects, every parameter modulated by a distinct
    // descriptor; 18 nodes and 17 wires stay far under the widget's pools.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain = bank.channel_chains[0];
        chain.num_effects                = 4;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::compressor);
        Sculptor::fx_init_slot(&chain, 3, Synth::EffectType::fir);
        uint16_t       next_desc = 1;
        const uint32_t max_desc  = 2 + 3 + 5 + 2;
        while (bank.lfos.num_allocated < max_desc) {
            const uint32_t slot               = bank.lfos.allocate();
            bank.lfos.entries[slot].wave      = Synth::WaveType::sine_wave;
            bank.lfos.entries[slot].period_ms = 250;
        }
        for (uint32_t slot = 0; slot < 4; ++slot) {
            const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
            for (uint32_t param = 0; param < num_params; ++param) {
                chain.effects[slot].bindings[param].lfo_desc_id = next_desc++;
                chain.effects[slot].bindings[param].lfo_depth   = 0.1f;
            }
        }
        TEST(Synth::validate_instrument_bank(&bank));

        static Sculptor::Graph              graph;
        static Sculptor::EffectGraphMapping mapping;
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
        TEST(mapping.lfo_count == max_desc);

        // The compressor node carries the full modulation grammar: 5 params
        // x 10 slots + In/Out/Enabled = 53, under the widget's per-node cap.
        TEST(graph.node(mapping.effect_nodes[2]).slots.num_allocated == 3 + 5 * Sculptor::fx_param_stride);
        uint32_t node_count = 0;
        for (uint32_t idx = 0; idx < Sculptor::max_nodes; ++idx) {
            if (graph.node_occupied(idx)) {
                ++node_count;
            }
        }
        TEST(node_count == 19); // input, output, MIDI, 4 effects, 12 LFOs

        uint32_t num_modulated = 0;
        uint32_t state_bytes   = 0;
        Sculptor::count_effect_budgets(bank, &num_modulated, &state_bytes);
        TEST(num_modulated == max_desc);
    }

    // Known runtime seam, deliberately deferred: effect-state bump allocation
    // never reclaims, so a disable -> re-enable cycle of the worst chain
    // exhausts the budget and preflight refuses visibly.  This test pins the
    // seam so the editor's visible-failure surfacing keeps matching reality.
    {
        static Synth::InstrumentBank bank;
        make_valid_bank(bank);
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        Synth::init_effect_state_region(fake_region_base);
        static Synth::EffectExpansionPlan plan;
        const char*                       error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        static Synth::EffectChain chains[Synth::max_channels];
        static Synth::EffectChain master;
        FakeWriter                writer = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        // Disabled slots carry no state, but the consumed budget never shrinks.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            bank.master_chain.effects[slot].enabled = false;
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        // Re-enabling the same four delays needs fresh state at the bump
        // position: the cumulative total exceeds the budget and preflight fails.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            bank.master_chain.effects[slot].enabled = true;
        }
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        Synth::init_effect_state_region(fake_region_base);
    }

    // ---------------------------------------------------------------------------
    // Clipboard instrument documents (doc/synth_instrument_schema.json): decode
    // into a standalone instrument with 1-based local descriptor indices, encode
    // back from a bank-resolved instrument.
    // ---------------------------------------------------------------------------
    {
        const char* const doc = "{"
                                "\"format\":\"synth-instrument-v1\","
                                "\"note_skew_semitones\":0.25,"
                                "\"layer_skew_semitones\":0.5,"
                                "\"layers\":["
                                "{"
                                "\"wave_a\":\"sine\","
                                "\"wave_b\":\"sawtooth\","
                                "\"mode\":\"fm\","
                                "\"mod_ratio\":2.0,"
                                "\"pitch_offset_semitones\":-12.0,"
                                "\"volume\":{"
                                "\"base\":0.8,"
                                "\"envelope\":{\"points\":[{\"time_seconds\":0.0,\"level\":0.0},"
                                "{\"time_seconds\":0.25,\"level\":1.0},"
                                "{\"time_seconds\":0.5,\"level\":0.25}],\"sustain_start\":2,\"sustain_end\":2},"
                                "\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":5.0,\"min_level\":0.0,\"max_level\":1.0,"
                                "\"op\":\"add\",\"depth\":0.2},"
                                "\"sources\":[{\"source\":\"velocity\",\"op\":\"add\",\"scale\":0.5},"
                                "{\"source\":\"mod_wheel\",\"op\":\"multiply\",\"scale\":0.25}]"
                                "},"
                                "\"pitch\":{\"base\":1.0},"
                                "\"panning\":{\"base\":0.5},"
                                "\"duty_b\":{\"base\":0.5},"
                                "\"fm_index\":{\"base\":0.75}"
                                "}"
                                "]"
                                "}";

        static Synth::Instrument         instr;
        static Synth::EnvelopeDescriptor envs[Synth::instrument_max_envelopes];
        static Synth::LFODescriptor      lfos[Synth::instrument_max_lfos];
        uint32_t                         env_count = 999;
        uint32_t                         lfo_count = 999;
        TEST(Synth::decode_instrument_json(doc,
                                           static_cast<uint32_t>(strlen(doc)),
                                           &instr,
                                           envs,
                                           &env_count,
                                           lfos,
                                           &lfo_count));
        TEST(instr.layer_count == 1);
        TEST(instr.note_skew_semitones == 0.25f);
        TEST(instr.layer_skew_semitones == 0.5f);
        const Synth::Oscillator& osc = instr.layers[0];
        TEST(osc.osc_type[0] == Synth::WaveType::sine_wave);
        TEST(osc.osc_type[1] == Synth::WaveType::sawtooth_wave);
        TEST(osc.osc_mode == Synth::osc_mode_fm);
        TEST(osc.mod_ratio == 2.0f);
        TEST(osc.pitch_offset == -12.0f);
        // Untouched targets keep neutral defaults.
        TEST(instr.routing[Synth::mod_osc_mix].base_value == 0.0f);
        TEST(instr.routing[Synth::mod_volume].base_value == 0.8f);
        TEST(instr.routing[Synth::mod_volume].num_inputs == 2);
        TEST(instr.routing[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
        TEST(instr.routing[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
        TEST(instr.routing[Synth::mod_volume].inputs[1].scale == 0.25f);
        TEST(instr.routing[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
        TEST(instr.routing[Synth::mod_volume].inputs[0].op == Synth::SourceOp::add);
        TEST(instr.routing[Synth::mod_volume].inputs[0].scale == 0.5f);
        TEST(instr.routing[Synth::mod_pitch].base_value == 1.0f);
        TEST(instr.routing[Synth::mod_panning].base_value == 0.5f);
        TEST(instr.routing[Synth::mod_duty1].base_value == 0.5f);
        // fm_index is 0..1 in the document, 0..2*pi radians in the bank.
        TEST(instr.routing[Synth::mod_fm_index].base_value == 0.75f * 6.2831853f);
        // Envelope: seconds -> ticks, levels 0..1 -> 16-bit values, min normalized to 0.
        TEST(osc.gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(env_count == 1);
        const Synth::EnvelopeDescriptor& env = envs[0];
        TEST(env.num_points == 3);
        TEST(env.sustain_first_point == 2);
        TEST(env.sustain_last_point == 2);
        TEST(env.min_value == 0.0f);
        TEST(env.min_max_delta == 1.0f / 65535.0f); // runtime: min + raw * delta spans levels 0..1
        TEST(env.points[1].position == Sculptor::envelope_ms_to_ticks(250.0f));
        TEST(env.points[1].value == 0xFFFF);
        TEST(env.points[2].value == 16384); // level 0.25
        // LFO: Hz -> period_ms, levels 0..1 -> min/delta.
        TEST(osc.gen[Synth::mod_volume].lfo_desc_id == 1);
        TEST(lfo_count == 1);
        const Synth::LFODescriptor& lfo = lfos[0];
        TEST(lfo.wave == Synth::WaveType::sine_wave);
        TEST(lfo.period_ms == 200);
        TEST(lfo.min_value == 0.0f);
        TEST(osc.gen[Synth::mod_volume].lfo_op == Synth::SourceOp::add);
        TEST(osc.gen[Synth::mod_volume].lfo_depth == 0.2f);

        // Install into a bank through the remap helpers, then encode: the
        // document must decode back to the same instrument content.
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        uint16_t env_ids[Synth::instrument_max_envelopes];
        uint16_t lfo_ids[Synth::instrument_max_lfos];
        TEST(Synth::remap_envelopes(&bank, envs, env_count, env_ids));
        TEST(Synth::remap_lfos(&bank, lfos, lfo_count, lfo_ids));
        Synth::Instrument installed;
        Synth::remap_instrument(instr, env_ids, lfo_ids, &installed);
        const uint32_t slot = bank.instruments.allocate();
        TEST(slot != pool_no_slot);
        bank.instruments.entries[slot]      = installed;
        bank.channel_zones[0][0].start_note = 1;
        bank.channel_zones[0][0].instrument = static_cast<uint8_t>(slot);
        TEST(Synth::validate_instrument_bank(&bank));

        static char    encoded[64 * 1024];
        const uint32_t encoded_len = Synth::encode_instrument_json(encoded, sizeof(encoded), &installed, &bank);
        TEST(encoded_len > 0);
        static Synth::Instrument         decoded;
        static Synth::EnvelopeDescriptor decoded_envs[Synth::instrument_max_envelopes];
        static Synth::LFODescriptor      decoded_lfos[Synth::instrument_max_lfos];
        uint32_t                         decoded_env_count = 999;
        uint32_t                         decoded_lfo_count = 999;
        TEST(Synth::decode_instrument_json(encoded,
                                           encoded_len,
                                           &decoded,
                                           decoded_envs,
                                           &decoded_env_count,
                                           decoded_lfos,
                                           &decoded_lfo_count));
        TEST(decoded.layer_count == installed.layer_count);
        const Synth::Oscillator& decoded_osc = decoded.layers[0];
        TEST(decoded_osc.osc_type[0] == Synth::WaveType::sine_wave);
        TEST(decoded_osc.osc_type[1] == Synth::WaveType::sawtooth_wave);
        TEST(decoded_osc.osc_mode == Synth::osc_mode_fm);
        TEST(decoded_osc.mod_ratio == 2.0f);
        TEST(decoded_osc.pitch_offset == -12.0f);
        TEST(decoded.routing[Synth::mod_volume].base_value == 0.8f);
        TEST(decoded.routing[Synth::mod_volume].num_inputs == 2);
        TEST(decoded.routing[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
        TEST(decoded.routing[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
        TEST(decoded.routing[Synth::mod_volume].inputs[1].scale == 0.25f);
        TEST(decoded.routing[Synth::mod_fm_index].base_value == 0.75f * 6.2831853f);
        TEST(decoded_osc.gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(decoded_env_count == 1);
        TEST(decoded_envs[0].points[1].position == env.points[1].position);
        TEST(decoded_envs[0].points[2].value == 16384);
        TEST(decoded_envs[0].min_value == envs[0].min_value);
        TEST(decoded_envs[0].min_max_delta == envs[0].min_max_delta);
        TEST(decoded_osc.gen[Synth::mod_volume].lfo_desc_id == 1);
        TEST(decoded_lfo_count == 1);
        TEST(decoded_lfos[0].period_ms == 200);
        TEST(decoded_osc.gen[Synth::mod_volume].lfo_depth == 0.2f);

        // fm_index levels are document units 0..1; the bank stores radians, so the
        // envelope's min/delta and the LFO's min/delta scale by 2*pi. The envelope's
        // 16-bit values still span the level range across the delta.
        {
            const char* const fm_doc =
                "{"
                "\"format\":\"synth-instrument-v1\","
                "\"layers\":[{"
                "\"wave_a\":\"sine\","
                "\"mode\":\"fm\","
                "\"fm_index\":{"
                "\"envelope\":{\"points\":[{\"time_seconds\":0.0,\"level\":0.25},{\"time_seconds\":0.1,\"level\":0.75}]},"
                "\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":2.0,\"min_level\":0.25,\"max_level\":0.5,\"depth\":0.1}"
                "}}]}";
            static Synth::Instrument         fm_instr;
            static Synth::EnvelopeDescriptor fm_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      fm_lfos[Synth::instrument_max_lfos];
            uint32_t                         fm_env_count = 999;
            uint32_t                         fm_lfo_count = 999;
            TEST(Synth::decode_instrument_json(fm_doc,
                                               static_cast<uint32_t>(strlen(fm_doc)),
                                               &fm_instr,
                                               fm_envs,
                                               &fm_env_count,
                                               fm_lfos,
                                               &fm_lfo_count));
            TEST(fm_env_count == 1);
            const float fm_scale = 6.2831853f;
            TEST(fm_envs[0].min_value == 0.25f * fm_scale);
            TEST(fm_envs[0].min_max_delta == 0.5f * fm_scale / 65535.0f);
            TEST(fm_envs[0].points[1].value == 0xFFFF); // level 0.75 tops the 0.25..0.75 range
            TEST(fm_lfo_count == 1);
            TEST(fm_lfos[0].min_value == 0.25f * fm_scale);
            TEST(fm_lfos[0].min_max_delta == 0.25f * fm_scale);
            // The encoder must emit document units, or the editor's own copy cannot
            // paste back: install into a bank, encode, decode, compare bank values.
            static Synth::InstrumentEditorBank fm_bank;
            fm_bank = {};
            uint16_t fm_env_ids[Synth::instrument_max_envelopes];
            uint16_t fm_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&fm_bank.bank, fm_envs, fm_env_count, fm_env_ids));
            TEST(Synth::remap_lfos(&fm_bank.bank, fm_lfos, fm_lfo_count, fm_lfo_ids));
            Synth::Instrument fm_installed;
            Synth::remap_instrument(fm_instr, fm_env_ids, fm_lfo_ids, &fm_installed);
            const uint32_t fm_slot = fm_bank.bank.instruments.allocate();
            TEST(fm_slot != pool_no_slot);
            fm_bank.bank.instruments.entries[fm_slot]   = fm_installed;
            fm_bank.bank.channel_enabled[0]             = 1;
            fm_bank.bank.channel_zones[0][0].start_note = 1;
            fm_bank.bank.channel_zones[0][0].instrument = static_cast<uint8_t>(fm_slot);
            TEST(Synth::validate_instrument_bank(&fm_bank.bank));

            static char    fm_text[64 * 1024];
            const uint32_t fm_text_len =
                Synth::encode_instrument_json(fm_text, sizeof(fm_text), &fm_installed, &fm_bank.bank);
            TEST(fm_text_len > 0);
            static Synth::Instrument         fm_back;
            static Synth::EnvelopeDescriptor fm_back_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      fm_back_lfos[Synth::instrument_max_lfos];
            uint32_t                         fm_back_env_count = 0;
            uint32_t                         fm_back_lfo_count = 0;
            TEST(Synth::decode_instrument_json(fm_text,
                                               fm_text_len,
                                               &fm_back,
                                               fm_back_envs,
                                               &fm_back_env_count,
                                               fm_back_lfos,
                                               &fm_back_lfo_count));
            TEST(fm_back_env_count == 1);
            TEST(fm_back_envs[0].min_value == fm_envs[0].min_value);
            TEST(fm_back_envs[0].min_max_delta == fm_envs[0].min_max_delta);
            TEST(fm_back_envs[0].points[1].position == fm_envs[0].points[1].position);
            TEST(fm_back_envs[0].points[1].value == fm_envs[0].points[1].value);
            TEST(fm_back_lfo_count == 1);
            TEST(fm_back_lfos[0].min_value == fm_lfos[0].min_value);
            TEST(fm_back_lfos[0].min_max_delta == fm_lfos[0].min_max_delta);
        }

        // Identical envelope descriptors on separate layers share one pool slot.
        {
            const char* const dup_doc =
                "{"
                "\"format\":\"synth-instrument-v1\","
                "\"layers\":["
                "{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":[{\"time_seconds\":0.1,\"level\":0.5}]}}},"
                "{\"wave_a\":\"pulse\",\"volume\":{\"envelope\":{\"points\":[{\"time_seconds\":0.1,\"level\":0.5}]}}}"
                "]}";
            static Synth::Instrument         dup_instr;
            static Synth::EnvelopeDescriptor dup_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      dup_lfos[Synth::instrument_max_lfos];
            uint32_t                         dup_env_count = 999;
            uint32_t                         dup_lfo_count = 999;
            TEST(Synth::decode_instrument_json(dup_doc,
                                               static_cast<uint32_t>(strlen(dup_doc)),
                                               &dup_instr,
                                               dup_envs,
                                               &dup_env_count,
                                               dup_lfos,
                                               &dup_lfo_count));
            TEST(dup_instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
            TEST(dup_instr.layers[1].gen[Synth::mod_volume].envelope_desc_id == 1);
            TEST(dup_env_count == 1);
        }

        // Pasting over a populated zone drops its stale graph records; channel-wide
        // effect layouts (kind 4), master-chain records, and other zones stay
        // untouched, and the clear owns the zone's mask row.
        {
            static Synth::InstrumentEditorBank clear_bank;
            clear_bank = {};
            add_detached_record(&clear_bank, 0, 0, 1, 1, 0.0f, 0.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            add_parameter_record(&clear_bank, 0, 0, 0, 1.0f, 2.0f, 7);
            clear_bank.graph_layout[1].served = 2; // a parameter serving layer 1
            add_detached_record(&clear_bank, 0, 1, 1, 1, 0.0f, 0.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            add_detached_record(&clear_bank, 1, 0, 1, 1, 0.0f, 0.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            add_detached_record(&clear_bank,
                                0,
                                0,
                                4,
                                1,
                                0.0f,
                                0.0f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                0); // same-channel effect layout
            add_detached_record(&clear_bank,
                                16,
                                0,
                                4,
                                1,
                                0.0f,
                                0.0f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                0);                 // master-chain effect layout
            clear_bank.graph_missing_sum[0][0] = 5; // the mask row belongs to the clear

            Sculptor::zone_records_clear_zone(&clear_bank, 0, 0);

            TEST(clear_bank.graph_layout_count == 4);
            TEST(count_records_matching(clear_bank, 0, 0, 1, 1) == 0);
            TEST(count_records_matching(clear_bank, 0, 0, 3, 0) == 0);
            TEST(count_records_matching(clear_bank, 0, 1, 1, 1) == 1);
            TEST(count_records_matching(clear_bank, 1, 0, 1, 1) == 1);
            TEST(clear_bank.graph_missing_sum[0][0] == 0);
            uint32_t effect_layouts = 0;
            for (uint32_t r = 0; r < clear_bank.graph_layout_count; r++) {
                if (clear_bank.graph_layout[r].kind == 4)
                    ++effect_layouts;
            }
            TEST(effect_layouts == 2);
        }
        // Every rejection leaves the outputs byte-identical.
        const char* const rejected_docs[] = {
            // malformed
            "{\"format\":\"synth-instrument-v1\",\"layers\":[]",
            // unknown key
            "{\"format\":\"synth-instrument-v1\",\"layers\":[],\"bogus\":1}",
            // bad wave
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"flugelhorn\"}]}",
            // bad mode
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"granular\"}]}",
            // out of range
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"pitch_offset_semitones\":13.0}]}",
            // out of range
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":1.5}}]}",
            // > max inputs
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"sources\":["
            "{\"source\":\"velocity\"},{\"source\":\"mod_wheel\"},{\"source\":\"aftertouch\"}]}}]}",
            // unknown target
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"bassoon\":{\"base\":1.0}}]}",
            // negative Hz
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":-5.0}}}]}",
            // period overflow
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":0.001}}}]}",
            // > max points
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":["
            "{\"time_seconds\":0.0},{\"time_seconds\":0.1},{\"time_seconds\":0.2},{\"time_seconds\":0.3},"
            "{\"time_seconds\":0.4},{\"time_seconds\":0.5},{\"time_seconds\":0.6},{\"time_seconds\":0.7},"
            "{\"time_seconds\":0.8}]}}}]}",
            // not ascending
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":["
            "{\"time_seconds\":0.5,\"level\":0.5},{\"time_seconds\":0.1,\"level\":1.0}]}}}]}",
            // wrong format tag
            "{\"format\":\"synth-editor-9\",\"layers\":[]}",
            // no points
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":[]}}}]}",
            // no wave
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"frequency_hz\":5.0}}}]}",
            // no frequency
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"sine\"}}}]}",
            // source entry without "source"
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"sources\":[{\"op\":\"add\",\"scale\":0.5}]}}]}",
            // unknown field inside a source entry
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"sources\":[{\"source\":\"velocity\",\"banana\":1}]}}]}",
            // time beyond the tick range
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":["
            "{\"time_seconds\":1000000.0,\"level\":0.5}]}}}]}",
            // sub-tick collision
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":["
            "{\"time_seconds\":0.1,\"level\":0.5},{\"time_seconds\":0.101,\"level\":1.0}]}}}]}",
            // unsupported LFO wave (pulse)
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"pulse\",\"frequency_hz\":5.0}}}]}",
            // unsupported LFO wave (noise)
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"noise\",\"frequency_hz\":5.0}}}]}",
            // mod_ratio out of range
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":100.0}]}",
            // cutoff base below the slider floor
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"lowpass_cutoff\":{\"base\":10.0}}]}",
            // duplicate
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"wave_a\":\"sawtooth\"}]}",
            // level span overflows the stored delta
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":-3e38},{\"time_seconds\":0.1,\"level\":3e38}]}}}]}",
            // LFO span overflows the stored delta
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":5.0,\"min_level\":-3e38,\"max_level\":3e38,\"depth\":0.5}}}]}",
            // LFO span overflows the bank-unit conversion
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fm_index\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":5.0,\"max_level\":3e38,\"depth\":0.5}}}]}",
        };
        static Synth::Instrument         reference_instr;
        static Synth::EnvelopeDescriptor reference_envs[Synth::instrument_max_envelopes];
        static Synth::LFODescriptor      reference_lfos[Synth::instrument_max_lfos];
        memset(&reference_instr, 0x5A, sizeof(reference_instr));
        memset(reference_envs, 0x5A, sizeof(reference_envs));
        memset(reference_lfos, 0x5A, sizeof(reference_lfos));
        for (uint32_t i = 0; i < sizeof(rejected_docs) / sizeof(rejected_docs[0]); i++) {
            Synth::Instrument sentinel_instr;
            memset(&sentinel_instr, 0x5A, sizeof(sentinel_instr));
            static Synth::EnvelopeDescriptor sentinel_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      sentinel_lfos[Synth::instrument_max_lfos];
            memset(sentinel_envs, 0x5A, sizeof(sentinel_envs));
            memset(sentinel_lfos, 0x5A, sizeof(sentinel_lfos));
            uint32_t sentinel_env_count = 999;
            uint32_t sentinel_lfo_count = 999;
            TEST(! Synth::decode_instrument_json(rejected_docs[i],
                                                 static_cast<uint32_t>(strlen(rejected_docs[i])),
                                                 &sentinel_instr,
                                                 sentinel_envs,
                                                 &sentinel_env_count,
                                                 sentinel_lfos,
                                                 &sentinel_lfo_count));
            const bool untouched = sentinel_env_count == 999 && sentinel_lfo_count == 999 &&
                                   memcmp(&sentinel_instr, &reference_instr, sizeof(sentinel_instr)) == 0 &&
                                   memcmp(sentinel_envs, reference_envs, sizeof(reference_envs)) == 0 &&
                                   memcmp(sentinel_lfos, reference_lfos, sizeof(reference_lfos)) == 0;
            TEST(untouched);
        }

        // A zero ratio is bank-legal in FM/sync, a cutoff base of 0 is the bypass
        // point, and a cutoff envelope level can quantize just below the slider
        // floor; all of them must decode, and a copy must paste back exactly.
        {
            const char* const zero_ratio_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":0.0}]}";
            static Synth::Instrument         zero_instr;
            static Synth::EnvelopeDescriptor zero_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      zero_lfos[Synth::instrument_max_lfos];
            uint32_t                         zero_env_count = 999;
            uint32_t                         zero_lfo_count = 999;
            TEST(Synth::decode_instrument_json(zero_ratio_doc,
                                               static_cast<uint32_t>(strlen(zero_ratio_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.layers[0].osc_mode == Synth::osc_mode_fm);
            TEST(zero_instr.layers[0].mod_ratio == 0.0f);

            // The encoder must keep emitting the zero (restoring the omitted-key
            // default of 1 would change the sound), and an omitted ratio decodes to 1.
            {
                static Synth::InstrumentEditorBank zero_bank;
                zero_bank                = {};
                const uint32_t zero_slot = zero_bank.bank.instruments.allocate();
                TEST(zero_slot != pool_no_slot);
                zero_bank.bank.instruments.entries[zero_slot] = zero_instr;
                static char    zero_text[64 * 1024];
                const uint32_t zero_text_len =
                    Synth::encode_instrument_json(zero_text, sizeof(zero_text), &zero_instr, &zero_bank.bank);
                TEST(zero_text_len > 0);
                static Synth::Instrument         zero_back;
                static Synth::EnvelopeDescriptor zero_back_envs[Synth::instrument_max_envelopes];
                static Synth::LFODescriptor      zero_back_lfos[Synth::instrument_max_lfos];
                uint32_t                         zero_back_env_count = 0;
                uint32_t                         zero_back_lfo_count = 0;
                TEST(Synth::decode_instrument_json(zero_text,
                                                   zero_text_len,
                                                   &zero_back,
                                                   zero_back_envs,
                                                   &zero_back_env_count,
                                                   zero_back_lfos,
                                                   &zero_back_lfo_count));
                TEST(zero_back.layers[0].osc_mode == Synth::osc_mode_fm);
                TEST(zero_back.layers[0].mod_ratio == 0.0f);
            }

            const char* const sync_zero_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"sync\",\"mod_ratio\":0.0}]}";
            TEST(Synth::decode_instrument_json(sync_zero_doc,
                                               static_cast<uint32_t>(strlen(sync_zero_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.layers[0].osc_mode == Synth::osc_mode_hard_sync);
            TEST(zero_instr.layers[0].mod_ratio == 0.0f);

            const char* const omitted_ratio_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\"}]}";
            TEST(Synth::decode_instrument_json(omitted_ratio_doc,
                                               static_cast<uint32_t>(strlen(omitted_ratio_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.layers[0].osc_mode == Synth::osc_mode_fm);
            TEST(zero_instr.layers[0].mod_ratio == 1.0f);

            const char* const cutoff_bypass_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"lowpass_cutoff\":{\"base\":0.0}}]}";
            TEST(Synth::decode_instrument_json(cutoff_bypass_doc,
                                               static_cast<uint32_t>(strlen(cutoff_bypass_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.routing[Synth::mod_lowpass_cutoff].base_value == 0.0f);

            const char* const cutoff_floor_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"lowpass_cutoff\":{\"base\":20.0}}]}";
            TEST(Synth::decode_instrument_json(cutoff_floor_doc,
                                               static_cast<uint32_t>(strlen(cutoff_floor_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.routing[Synth::mod_lowpass_cutoff].base_value == 20.0f);

            // The level-20 point quantizes to raw 655, which encodes to just below
            // 20 Hz and must re-quantize to the same raw value.

            const char* const cutoff_env_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"lowpass_cutoff\":{\"envelope\":{\"points\":[{\"time_seconds\":0.0,\"level\":0.0},{\"time_seconds\":0.1,\"level\":20.0},{\"time_seconds\":0.2,\"level\":2000.0}]}}}]}";
            static Synth::EnvelopeDescriptor cut_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      cut_lfos[Synth::instrument_max_lfos];
            uint32_t                         cut_env_count = 999;
            uint32_t                         cut_lfo_count = 999;
            TEST(Synth::decode_instrument_json(cutoff_env_doc,
                                               static_cast<uint32_t>(strlen(cutoff_env_doc)),
                                               &zero_instr,
                                               cut_envs,
                                               &cut_env_count,
                                               cut_lfos,
                                               &cut_lfo_count));
            TEST(cut_env_count == 1);
            TEST(cut_envs[0].min_value == 0.0f);
            TEST(cut_envs[0].points[1].value == 655); // the floor's own quantization step

            static Synth::InstrumentEditorBank cut_bank;
            cut_bank = {};
            uint16_t cut_env_ids[Synth::instrument_max_envelopes];
            uint16_t cut_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&cut_bank.bank, cut_envs, cut_env_count, cut_env_ids));
            TEST(Synth::remap_lfos(&cut_bank.bank, cut_lfos, cut_lfo_count, cut_lfo_ids));
            Synth::Instrument cut_installed;
            Synth::remap_instrument(zero_instr, cut_env_ids, cut_lfo_ids, &cut_installed);
            const uint32_t cut_slot = cut_bank.bank.instruments.allocate();
            TEST(cut_slot != pool_no_slot);
            cut_bank.bank.instruments.entries[cut_slot]  = cut_installed;
            cut_bank.bank.channel_enabled[0]             = 1;
            cut_bank.bank.channel_zones[0][0].start_note = 1;
            cut_bank.bank.channel_zones[0][0].instrument = static_cast<uint8_t>(cut_slot);
            TEST(Synth::validate_instrument_bank(&cut_bank.bank));
            static char    cut_text[64 * 1024];
            const uint32_t cut_text_len =
                Synth::encode_instrument_json(cut_text, sizeof(cut_text), &cut_installed, &cut_bank.bank);
            TEST(cut_text_len > 0);
            static Synth::Instrument         cut_back;
            static Synth::EnvelopeDescriptor cut_back_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      cut_back_lfos[Synth::instrument_max_lfos];
            uint32_t                         cut_back_env_count = 0;
            uint32_t                         cut_back_lfo_count = 0;
            TEST(Synth::decode_instrument_json(cut_text,
                                               cut_text_len,
                                               &cut_back,
                                               cut_back_envs,
                                               &cut_back_env_count,
                                               cut_back_lfos,
                                               &cut_back_lfo_count));
            TEST(cut_back_env_count == 1);
            TEST(cut_back_envs[0].points[1].value == cut_envs[0].points[1].value);
            TEST(cut_back_envs[0].min_value == cut_envs[0].min_value);
            TEST(cut_back_envs[0].min_max_delta == cut_envs[0].min_max_delta);
        }

        // A zone copied out of the editor by hand: LFO "Value" sweeps default to
        // -1..1 regardless of the target's slider range, and envelope min/max
        // slots are unbounded, so levels must decode and paste back exactly.
        {
            const char* const real_zone_doc =
                "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
                "\"layers\":[{\"wave_a\":\"sine\",\"pitch_offset_semitones\":0,\"volume\":{\"base\":0,"
                "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},{\"time_seconds\":0.0406349227,"
                "\"level\":0.491630435},{\"time_seconds\":0.522448957,\"level\":0}],\"sustain_start\":1,"
                "\"sustain_end\":1},\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":3.33333325,\"duty\":0,"
                "\"min_level\":0,\"max_level\":1,\"depth\":0.5,\"depth_source\":\"aftertouch\","
                "\"rate_scale_ms\":0}},\"pitch\":{\"base\":0,\"envelope\":{\"points\":[{\"time_seconds\":0,"
                "\"level\":0.255542845},{\"time_seconds\":0.0464399122,\"level\":1.52587891e-05},"
                "{\"time_seconds\":0.522448957,\"level\":-0.223620966}],\"sustain_start\":1,\"sustain_end\":1}},"
                "\"panning\":{\"base\":0.5,\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":1.66666663,\"duty\":0,"
                "\"min_level\":-1,\"max_level\":1,\"depth\":0.5,\"depth_source\":\"aftertouch\","
                "\"rate_scale_ms\":0}},\"duty_a\":{\"base\":0.172000006},\"duty_b\":{\"base\":0.156000003},"
                "\"osc_mix\":{\"base\":0},\"fm_index\":{\"base\":0},\"lowpass_cutoff\":{\"base\":20},"
                "\"highpass_cutoff\":{\"base\":20}}]}";
            static Synth::Instrument         real_instr;
            static Synth::EnvelopeDescriptor real_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      real_lfos[Synth::instrument_max_lfos];
            uint32_t                         real_env_count = 999;
            uint32_t                         real_lfo_count = 999;
            TEST(Synth::decode_instrument_json(real_zone_doc,
                                               static_cast<uint32_t>(strlen(real_zone_doc)),
                                               &real_instr,
                                               real_envs,
                                               &real_env_count,
                                               real_lfos,
                                               &real_lfo_count));
            TEST(real_lfo_count == 2);
            TEST(real_lfos[1].min_value == -1.0f); // panning LFO sweep below the slider range
            TEST(real_lfos[1].min_max_delta == 2.0f);

            static Synth::InstrumentEditorBank real_bank;
            real_bank = {};
            uint16_t real_env_ids[Synth::instrument_max_envelopes];
            uint16_t real_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&real_bank.bank, real_envs, real_env_count, real_env_ids));
            TEST(Synth::remap_lfos(&real_bank.bank, real_lfos, real_lfo_count, real_lfo_ids));
            Synth::Instrument real_installed;
            Synth::remap_instrument(real_instr, real_env_ids, real_lfo_ids, &real_installed);
            const uint32_t real_slot = real_bank.bank.instruments.allocate();
            TEST(real_slot != pool_no_slot);
            real_bank.bank.instruments.entries[real_slot] = real_installed;
            real_bank.bank.channel_enabled[0]             = 1;
            real_bank.bank.channel_zones[0][0].start_note = 1;
            real_bank.bank.channel_zones[0][0].instrument = static_cast<uint8_t>(real_slot);
            TEST(Synth::validate_instrument_bank(&real_bank.bank));
            static char    real_text[64 * 1024];
            const uint32_t real_text_len =
                Synth::encode_instrument_json(real_text, sizeof(real_text), &real_installed, &real_bank.bank);
            TEST(real_text_len > 0);
            static Synth::Instrument         real_back;
            static Synth::EnvelopeDescriptor real_back_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      real_back_lfos[Synth::instrument_max_lfos];
            uint32_t                         real_back_env_count = 0;
            uint32_t                         real_back_lfo_count = 0;
            TEST(Synth::decode_instrument_json(real_text,
                                               real_text_len,
                                               &real_back,
                                               real_back_envs,
                                               &real_back_env_count,
                                               real_back_lfos,
                                               &real_back_lfo_count));
            TEST(real_back_lfo_count == 2);
            TEST(real_back_lfos[1].min_value == real_lfos[1].min_value);
            TEST(real_back_lfos[1].min_max_delta == real_lfos[1].min_max_delta);
        }

        // Envelope levels sit outside the base's slider range as well: the
        // editor's min/max slots are unbounded reals, so a volume envelope
        // swinging past full scale must decode and stay bank-legal.
        {
            const char* const swing_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":-1.0},{\"time_seconds\":0.1,\"level\":5.0}]}}}]}";
            static Synth::Instrument         swing_instr;
            static Synth::EnvelopeDescriptor swing_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      swing_lfos[Synth::instrument_max_lfos];
            uint32_t                         swing_env_count = 999;
            uint32_t                         swing_lfo_count = 999;
            TEST(Synth::decode_instrument_json(swing_doc,
                                               static_cast<uint32_t>(strlen(swing_doc)),
                                               &swing_instr,
                                               swing_envs,
                                               &swing_env_count,
                                               swing_lfos,
                                               &swing_lfo_count));
            TEST(swing_env_count == 1);
            TEST(swing_envs[0].min_value == -1.0f);
            TEST(swing_envs[0].min_max_delta == 6.0f / 65535.0f);
            TEST(swing_envs[0].points[0].value == 0);
            TEST(swing_envs[0].points[1].value == 0xFFFF);
            static Synth::InstrumentEditorBank swing_bank;
            swing_bank = {};
            uint16_t swing_env_ids[Synth::instrument_max_envelopes];
            uint16_t swing_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&swing_bank.bank, swing_envs, swing_env_count, swing_env_ids));
            TEST(Synth::remap_lfos(&swing_bank.bank, swing_lfos, swing_lfo_count, swing_lfo_ids));
            Synth::Instrument swing_installed;
            Synth::remap_instrument(swing_instr, swing_env_ids, swing_lfo_ids, &swing_installed);
            const uint32_t swing_slot = swing_bank.bank.instruments.allocate();
            TEST(swing_slot != pool_no_slot);
            swing_bank.bank.instruments.entries[swing_slot] = swing_installed;
            swing_bank.bank.channel_enabled[0]              = 1;
            swing_bank.bank.channel_zones[0][0].start_note  = 1;
            swing_bank.bank.channel_zones[0][0].instrument  = static_cast<uint8_t>(swing_slot);
            TEST(Synth::validate_instrument_bank(&swing_bank.bank));
        }

        // -------------------------------------------------------------------
        // Clipboard portable layout records.
        //
        // Positives that own a bank assert validate_instrument_bank and
        // validate_editor_metadata before the operation under test; hand-built instrument
        // models assert validate_standalone_model; document rejections decode their
        // baseline first, so each refusal isolates the record under inspection.
        // -------------------------------------------------------------------
        {
            // A moved canonical, envelope, LFO and parameter node survives export,
            // encode, decode and mapping, and lands back on the decoded node
            // identity rather than on a numeric descriptor id.
            static Synth::InstrumentEditorBank  ex_bank;
            static Synth::Instrument            ex_instr;
            static Synth::InstrumentGraphLayout ex_portable[max_portable_records];
            static Synth::GraphNodeLayout       ex_mapped[max_portable_records];
            static Synth::GraphNodeLayout       ex_replay_mapped[max_portable_records];
            static char                         ex_text[clipboard_capacity];
            build_clipboard_zone_fixture(&ex_bank, &ex_instr);
            TEST(Synth::validate_instrument_bank(&ex_bank.bank));
            TEST(Sculptor::validate_editor_metadata(ex_bank));

            add_detached_record(&ex_bank, 0, 0, 0, 1, 64.0f, 32.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            add_detached_record(&ex_bank,
                                0,
                                0,
                                1,
                                1,
                                120.5f,
                                44.25f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                1);
            add_detached_record(&ex_bank,
                                0,
                                0,
                                2,
                                1,
                                12.5f,
                                -8.5f,
                                Synth::ModSource::velocity,
                                Synth::ModSource::mod_wheel,
                                1);
            add_parameter_record(&ex_bank, 0, 0, 0, 60.0f, -40.0f, 1);
            add_parameter_record(&ex_bank, 0, 0, 1, -60.0f, 40.0f, 1);
            ex_bank.graph_layout[1].width_override  = 220.0f;
            ex_bank.graph_layout[1].height_override = 120.0f;
            // Portable parameter ordinals are zero-based, internal slots one-based.
            ex_bank.graph_layout[3].param_slot = 1;
            ex_bank.graph_layout[4].param_slot = 1;
            TEST(Sculptor::validate_editor_metadata(ex_bank));

            uint32_t ex_portable_count = max_portable_records;
            TEST(Sculptor::encode_instrument_graph_layout(ex_instr,
                                                          ex_bank.bank,
                                                          ex_bank,
                                                          0,
                                                          0,
                                                          ex_portable,
                                                          max_portable_records,
                                                          &ex_portable_count));
            TEST(ex_portable_count == 5);
            int32_t rec = find_portable(ex_portable,
                                        ex_portable_count,
                                        Synth::instrument_graph_layout_canonical,
                                        0xFFu,
                                        0xFFu,
                                        0xFFu);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].canonical_index == 1);
            if (rec >= 0)
                TEST(ex_portable[rec].x == 64.0f && ex_portable[rec].y == 32.0f);
            rec = find_portable(ex_portable,
                                ex_portable_count,
                                Synth::instrument_graph_layout_envelope,
                                0,
                                Synth::mod_volume,
                                0xFFu);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].x == 120.5f && ex_portable[rec].width_override == 220.0f);
            if (rec >= 0)
                TEST(ex_portable[rec].height_override == 120.0f);
            rec = find_portable(ex_portable,
                                ex_portable_count,
                                Synth::instrument_graph_layout_lfo,
                                0,
                                Synth::mod_volume,
                                0xFFu);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
            if (rec >= 0)
                TEST(ex_portable[rec].rate_source == static_cast<uint8_t>(Synth::ModSource::mod_wheel));
            rec = find_portable(ex_portable,
                                ex_portable_count,
                                Synth::instrument_graph_layout_parameter,
                                0xFFu,
                                Synth::mod_volume,
                                0);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].x == 60.0f);
            rec = find_portable(ex_portable,
                                ex_portable_count,
                                Synth::instrument_graph_layout_parameter,
                                0xFFu,
                                Synth::mod_pitch,
                                0);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].x == -60.0f);

            // Exporter refusals leave the record sink and the count untouched: a
            // destination outside the zone table, and a capacity that cannot hold what
            // this zone exports.
            static Synth::InstrumentGraphLayout ex_refuse[max_portable_records];
            static Synth::InstrumentGraphLayout ex_refuse_sentinel[max_portable_records];
            memset(ex_refuse, 0x5A, sizeof(ex_refuse));
            memset(ex_refuse_sentinel, 0x5A, sizeof(ex_refuse_sentinel));
            constexpr uint32_t ex_refuse_capacity = sizeof(ex_refuse) / sizeof(ex_refuse[0]);
            uint32_t           ex_refuse_count    = ex_refuse_capacity;
            TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                            ex_bank.bank,
                                                            ex_bank,
                                                            Synth::max_channels, // no such channel
                                                            0,
                                                            ex_refuse,
                                                            ex_refuse_capacity,
                                                            &ex_refuse_count));
            TEST(ex_refuse_count == ex_refuse_capacity);
            TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);
            TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                            ex_bank.bank,
                                                            ex_bank,
                                                            0,
                                                            Synth::max_instr_per_channel, // no such zone
                                                            ex_refuse,
                                                            ex_refuse_capacity,
                                                            &ex_refuse_count));
            TEST(ex_refuse_count == ex_refuse_capacity);
            TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);
            // A zone that exports five records cannot fit into zero slots.
            TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                            ex_bank.bank,
                                                            ex_bank,
                                                            0,
                                                            0,
                                                            ex_refuse,
                                                            0,
                                                            &ex_refuse_count));
            TEST(ex_refuse_count == ex_refuse_capacity);
            TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);

            static Synth::InstrumentEditorBank ex_before;
            ex_before             = ex_bank;
            const uint32_t ex_len = Synth::encode_instrument_json(ex_text,
                                                                  sizeof(ex_text),
                                                                  &ex_instr,
                                                                  &ex_bank.bank,
                                                                  ex_portable,
                                                                  ex_portable_count);
            TEST(ex_len > 0);
            TEST(ex_len + 1 <= clipboard_capacity);
            TEST(strstr(ex_text, "\"graph_layout\":[") != nullptr);
            TEST(memcmp(&ex_bank, &ex_before, sizeof(ex_bank)) == 0);

            static ClipboardDecode ex_dec;
            fill_clipboard_sentinels(&ex_dec);
            TEST(decode_clipboard(&ex_dec, ex_text, max_portable_records));
            TEST(ex_dec.layout_count == ex_portable_count);
            TEST(ex_dec.env_count == 2);
            TEST(ex_dec.lfo_count == 1);
            TEST(ex_dec.instr.layer_count == 1);
            TEST(ex_dec.instr.layers[0].osc_mode == Synth::osc_mode_fm);
            TEST(ex_dec.instr.layers[0].mod_ratio == 2.0f);
            TEST(ex_dec.instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
            TEST(ex_dec.instr.layers[0].gen[Synth::mod_volume].lfo_desc_id == 1);
            TEST(ex_dec.instr.layers[0].gen[Synth::mod_pitch].envelope_desc_id == 2);
            TEST(ex_dec.instr.routing[Synth::mod_volume].base_value == 0.8f);
            TEST(ex_dec.envs[0].num_points == 3);
            TEST(ex_dec.envs[1].num_points == 3);
            TEST(ex_dec.lfos[0].period_ms == 250);
            TEST(ex_dec.layout[0].kind == Synth::instrument_graph_layout_canonical);
            TEST(find_portable(ex_dec.layout,
                               ex_dec.layout_count,
                               Synth::instrument_graph_layout_envelope,
                               0,
                               Synth::mod_volume,
                               0xFFu) >= 0);

            uint32_t ex_mapped_count = max_portable_records;
            TEST(Sculptor::map_instrument_graph_layout(ex_dec.instr,
                                                       ex_dec.envs,
                                                       ex_dec.env_count,
                                                       ex_dec.lfos,
                                                       ex_dec.lfo_count,
                                                       ex_dec.layout,
                                                       ex_dec.layout_count,
                                                       ex_mapped,
                                                       max_portable_records,
                                                       &ex_mapped_count));
            TEST(ex_mapped_count == 5);
            int32_t m = find_mapped(ex_mapped, ex_mapped_count, 0, 1);
            TEST(m >= 0);
            if (m >= 0)
                TEST(ex_mapped[m].x == 64.0f && ex_mapped[m].y == 32.0f);
            if (m >= 0)
                TEST(ex_mapped[m].uid == 0);
            m = find_mapped(ex_mapped, ex_mapped_count, 1, 1);
            TEST(m >= 0);
            if (m >= 0)
                TEST(ex_mapped[m].x == 120.5f && ex_mapped[m].width_override == 220.0f);
            if (m >= 0)
                TEST(ex_mapped[m].uid != 0);
            if (m >= 0)
                TEST(ex_mapped[m].depth_source == 0 && ex_mapped[m].rate_source == 0);
            m = find_mapped(ex_mapped, ex_mapped_count, 2, 1);
            TEST(m >= 0);
            if (m >= 0)
                TEST(ex_mapped[m].depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
            if (m >= 0)
                TEST(ex_mapped[m].rate_source == static_cast<uint8_t>(Synth::ModSource::mod_wheel));
            if (m >= 0)
                TEST(ex_mapped[m].x == 12.5f && ex_mapped[m].uid != 0);
            m = find_mapped(ex_mapped, ex_mapped_count, 3, Synth::mod_volume);
            TEST(m >= 0);
            if (m >= 0)
                TEST(ex_mapped[m].param_slot == 1 && ex_mapped[m].x == 60.0f && ex_mapped[m].uid != 0);
            m = find_mapped(ex_mapped, ex_mapped_count, 3, Synth::mod_pitch);
            TEST(m >= 0);
            if (m >= 0)
                TEST(ex_mapped[m].param_slot == 1 && ex_mapped[m].x == -60.0f);

            // Mapper refusals: a decoded locator the decoded model does not project, and
            // a kind that is not clipboard data. Both leave the mapped sink and the count
            // untouched; the same sink accepts the resolved records above, so only the
            // locator differs.
            static Synth::InstrumentGraphLayout mp_records[1];
            static Synth::GraphNodeLayout       mp_sink[4];
            static Synth::GraphNodeLayout       mp_sink_sentinel[4];
            memset(mp_sink, 0x5A, sizeof(mp_sink));
            memset(mp_sink_sentinel, 0x5A, sizeof(mp_sink_sentinel));
            constexpr uint32_t mp_capacity = sizeof(mp_sink) / sizeof(mp_sink[0]);
            uint32_t           mp_count    = mp_capacity;
            const uint32_t     mp_cases    = 2;
            for (uint32_t c = 0; c < mp_cases; c++) {
                memset(mp_records, 0, sizeof(mp_records));
                if (c == 0) {
                    // The decoded instrument has one layer, so layer 1 is not projected.
                    mp_records[0].kind   = Synth::instrument_graph_layout_envelope;
                    mp_records[0].layer  = 1;
                    mp_records[0].target = static_cast<uint8_t>(Synth::mod_volume);
                }
                else {
                    // Effect nodes are never clipboard data.
                    mp_records[0].kind = 4;
                }
                mp_records[0].x = 5.0f;
                mp_count        = mp_capacity;
                TEST(! Sculptor::map_instrument_graph_layout(ex_dec.instr,
                                                             ex_dec.envs,
                                                             ex_dec.env_count,
                                                             ex_dec.lfos,
                                                             ex_dec.lfo_count,
                                                             mp_records,
                                                             1,
                                                             mp_sink,
                                                             mp_capacity,
                                                             &mp_count));
                TEST(mp_count == mp_capacity);
                TEST(memcmp(mp_sink, mp_sink_sentinel, sizeof(mp_sink_sentinel)) == 0);
            }

            // Re-encoding the decoded instrument with the decoded-relative records
            // and mapping the result again reproduces the same editor records: the
            // portable identity is stable across a second copy.
            static Synth::InstrumentBank ex_replay_bank;
            memset(&ex_replay_bank, 0, sizeof(ex_replay_bank));
            uint16_t ex_env_ids[Synth::instrument_max_envelopes];
            uint16_t ex_lfo_ids[Synth::instrument_max_lfos];
            // The copy helpers below index their map outputs with the decoded
            // instrument generator ids, so the replay runs only on a decode that
            // produced the fixture's expected descriptor counts.
            const bool ex_decoded = ex_dec.env_count == 2 && ex_dec.lfo_count == 1 && ex_dec.layout_count == 5;
            TEST(ex_decoded);
            if (ex_decoded) {
                TEST(Synth::remap_envelopes(&ex_replay_bank, ex_dec.envs, ex_dec.env_count, ex_env_ids));
                TEST(Synth::remap_lfos(&ex_replay_bank, ex_dec.lfos, ex_dec.lfo_count, ex_lfo_ids));
                static Synth::Instrument ex_replay_instr;
                Synth::remap_instrument(ex_dec.instr, ex_env_ids, ex_lfo_ids, &ex_replay_instr);
                static char    ex_replay_text[clipboard_capacity];
                const uint32_t ex_replay_len = Synth::encode_instrument_json(ex_replay_text,
                                                                             sizeof(ex_replay_text),
                                                                             &ex_replay_instr,
                                                                             &ex_replay_bank,
                                                                             ex_dec.layout,
                                                                             ex_dec.layout_count);
                TEST(ex_replay_len > 0);
                static ClipboardDecode ex_replay_dec;
                fill_clipboard_sentinels(&ex_replay_dec);
                TEST(decode_clipboard(&ex_replay_dec, ex_replay_text, max_portable_records));
                TEST(ex_replay_dec.layout_count == ex_dec.layout_count);
                TEST(ex_replay_dec.env_count == ex_dec.env_count);
                TEST(ex_replay_dec.lfo_count == ex_dec.lfo_count);
                uint32_t ex_replay_mapped_count = max_portable_records;
                TEST(Sculptor::map_instrument_graph_layout(ex_replay_dec.instr,
                                                           ex_replay_dec.envs,
                                                           ex_replay_dec.env_count,
                                                           ex_replay_dec.lfos,
                                                           ex_replay_dec.lfo_count,
                                                           ex_replay_dec.layout,
                                                           ex_replay_dec.layout_count,
                                                           ex_replay_mapped,
                                                           max_portable_records,
                                                           &ex_replay_mapped_count));
                TEST(ex_replay_mapped_count == ex_mapped_count);
                TEST(memcmp(ex_replay_mapped, ex_mapped, ex_mapped_count * sizeof(ex_mapped[0])) == 0);
            }
        }

        {
            // The reclaim map is capacity-wide and exact: one entry per LFO slot,
            // zeros for freed and never-occupied ids, one-based ids for survivors,
            // the same map the surviving bindings are remapped with.
            static Synth::InstrumentEditorBank rm_bank;
            static Synth::InstrumentEditorBank rm_parallel;
            static uint16_t                    rm_map[Synth::max_lfos];
            make_valid_bank(rm_bank.bank); // instrument 0 owns envelope 1 and LFO 1
            for (uint32_t extra = 0; extra < 2; extra++) {
                const uint32_t added = rm_bank.bank.lfos.allocate();
                TEST(added != pool_no_slot);
                rm_bank.bank.lfos.entries[added].wave      = Synth::WaveType::sine_wave;
                rm_bank.bank.lfos.entries[added].period_ms = static_cast<uint16_t>(90 + 40 * extra);
            }
            rm_bank.bank.master_chain.num_effects                        = 1;
            rm_bank.bank.master_chain.effects[0].type                    = Synth::EffectType::distortion;
            rm_bank.bank.master_chain.effects[0].enabled                 = true;
            rm_bank.bank.master_chain.effects[0].bindings[0].base_value  = 1.0f;
            rm_bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id = 3; // id 2 stays unreferenced
            TEST(Synth::validate_instrument_bank(&rm_bank.bank));
            rm_parallel = rm_bank;

            for (uint32_t i = 0; i < Synth::max_lfos; i++) {
                rm_map[i] = 0xFFFF;
            }
            Synth::reclaim_unused_slots(&rm_bank, rm_map);

            // In this fixture LFO 1 (instrument) and LFO 3 (master chain) survive and
            // compact to 1 and 2; LFO 2 is referenced by nothing; every slot beyond the
            // allocated high water mark is unoccupied and reports zero.
            TEST(rm_map[0] == 1);
            TEST(rm_map[1] == 0);
            TEST(rm_map[2] == 2);
            for (uint32_t i = 3; i < Synth::max_lfos; i++) {
                TEST(rm_map[i] == 0);
            }
            TEST(rm_bank.bank.lfos.num_allocated == 2);
            TEST(rm_bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id == 2);
            TEST(rm_bank.bank.instruments.entries[0].layers[0].gen[Synth::mod_pitch].lfo_desc_id == 1);
            TEST(Synth::validate_instrument_bank(&rm_bank.bank));

            // The one-argument overload is the same transaction without the map.
            Synth::reclaim_unused_slots(&rm_parallel);
            TEST(memcmp(&rm_parallel, &rm_bank, sizeof(rm_bank)) == 0);
        }

        {
            // Descriptor identity, not descriptor id. Two byte-identical envelopes
            // intern into one decoded descriptor, so a record naming either source
            // cell addresses that one node.
            static char       it_text[clipboard_capacity];
            const char* const it_records =
                "{\"kind\":1,\"layer\":0,\"target\":1,\"x\":33.5,\"y\":4.5,\"width\":0,\"height\":0}";
            build_layout_doc(it_text, sizeof(it_text), clipboard_alias_head, it_records);
            static ClipboardDecode it_dec;
            fill_clipboard_sentinels(&it_dec);
            TEST(decode_clipboard(&it_dec, it_text, max_portable_records));
            TEST(it_dec.env_count == 1); // interning really happened
            TEST(it_dec.instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
            TEST(it_dec.instr.layers[0].gen[Synth::mod_pitch].envelope_desc_id == 1);
            TEST(it_dec.layout_count == 1);

            static Synth::GraphNodeLayout it_mapped[max_portable_records];
            uint32_t                      it_mapped_count = max_portable_records;
            TEST(Sculptor::map_instrument_graph_layout(it_dec.instr,
                                                       it_dec.envs,
                                                       it_dec.env_count,
                                                       it_dec.lfos,
                                                       it_dec.lfo_count,
                                                       it_dec.layout,
                                                       it_dec.layout_count,
                                                       it_mapped,
                                                       max_portable_records,
                                                       &it_mapped_count));
            TEST(it_mapped_count == 1);
            TEST(it_mapped[0].kind == 1 && it_mapped[0].index == 1);
            TEST(it_mapped[0].x == 33.5f && it_mapped[0].uid != 0);

            // A generator on a target the graph does not project has no node to
            // attach to, so its record cannot resolve and the document is refused.
            const char* const dm_head =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"duty_a\":"
                "{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
                "{\"time_seconds\":0.5,\"level\":0.75}],\"sustain_start\":0,\"sustain_end\":1}}}],";
            static char dm_text[clipboard_capacity];
            build_layout_doc(dm_text,
                             sizeof(dm_text),
                             dm_head,
                             "{\"kind\":1,\"layer\":0,\"target\":3,\"x\":10,\"y\":10,\"width\":0,\"height\":0}");
            static ClipboardDecode dm_attempt;
            fill_clipboard_sentinels(&dm_attempt);
            TEST(decode_clipboard(&dm_attempt, dm_text, max_portable_records) == false);
            // The same document without the record decodes: the refusal is the
            // record's, not the document's.
            static ClipboardDecode dm_baseline;
            fill_clipboard_sentinels(&dm_baseline);
            build_layout_doc(dm_text, sizeof(dm_text), dm_head, "");
            TEST(decode_clipboard(&dm_baseline, dm_text, max_portable_records));
            TEST(dm_baseline.env_count == 1);
            static ClipboardDecode dm_sentinel;
            fill_clipboard_sentinels(&dm_sentinel);
            TEST(clipboard_sentinels_intact(&dm_attempt, &dm_sentinel));
        }

        {
            // Copy-time normalization. Source envelopes A, A' (byte-identical to A)
            // and B give three source parameter groups on volume; the decoder interns
            // A and A', so the decoded instrument has two. The saved positions sit on
            // A' (source ordinal 1) and B (source ordinal 2) and must attach to
            // decoded ordinals 0 and 1.
            static Synth::EnvelopeDescriptor    nm_envs[3];
            static Synth::EnvelopeDescriptor    nm_dec_envs[2];
            static Synth::Instrument            nm_src;
            static Synth::Instrument            nm_dec;
            static Synth::InstrumentGraphLayout nm_records[2];
            static Synth::InstrumentGraphLayout nm_out[4];
            static Synth::InstrumentGraphLayout nm_swapped[2];
            static Synth::InstrumentGraphLayout nm_swapped_out[4];

            memset(nm_envs, 0, sizeof(nm_envs));
            for (uint32_t e = 0; e < 3; e++) {
                Synth::EnvelopeDescriptor& source_env = nm_envs[e];
                source_env.num_points                 = 2;
                source_env.sustain_first_point        = 0;
                source_env.sustain_last_point         = 1;
                source_env.points[0].position         = 0;
                source_env.points[0].value            = 0x2000;
                source_env.points[1].position         = 200;
                source_env.points[1].value            = 0x8000;
                source_env.min_value                  = e == 2 ? 0.5f : -0.25f; // B differs from A/A'
                source_env.min_max_delta              = e == 2 ? 1.0f : 0.75f;
            }
            nm_dec_envs[0] = nm_envs[0];
            nm_dec_envs[1] = nm_envs[2];

            nm_src.layer_count = 3;
            nm_dec.layer_count = 3;
            for (uint32_t layer = 0; layer < 3; layer++) {
                nm_src.layers[layer].osc_type[0]                             = Synth::WaveType::sine_wave;
                nm_src.layers[layer].gen[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(layer + 1);
                nm_dec.layers[layer].osc_type[0]                             = Synth::WaveType::sine_wave;
                nm_dec.layers[layer].gen[Synth::mod_volume].envelope_desc_id = layer == 2 ? 2 : 1;
                nm_src.layers[layer].mod_ratio                               = 1.0f;
                nm_dec.layers[layer].mod_ratio                               = 1.0f;
            }
            TEST(validate_standalone_model(nm_src, nm_envs, 3, nullptr, 0));
            TEST(validate_standalone_model(nm_dec, nm_dec_envs, 2, nullptr, 0));

            memset(nm_records, 0, sizeof(nm_records));
            nm_records[0].kind              = Synth::instrument_graph_layout_parameter;
            nm_records[0].target            = static_cast<uint8_t>(Synth::mod_volume);
            nm_records[0].parameter_ordinal = 1; // A'
            nm_records[0].x                 = 111.5f;
            nm_records[0].y                 = 1.5f;
            nm_records[1].kind              = Synth::instrument_graph_layout_parameter;
            nm_records[1].target            = static_cast<uint8_t>(Synth::mod_volume);
            nm_records[1].parameter_ordinal = 2; // B
            nm_records[1].x                 = 222.5f;
            nm_records[1].y                 = 2.5f;

            uint32_t nm_count = 4;
            TEST(Sculptor::normalize_instrument_graph_layout(nm_src,
                                                             nm_envs,
                                                             3,
                                                             nullptr,
                                                             0,
                                                             nm_records,
                                                             2,
                                                             nm_dec,
                                                             nm_dec_envs,
                                                             2,
                                                             nullptr,
                                                             0,
                                                             nm_out,
                                                             sizeof(nm_out) / sizeof(nm_out[0]),
                                                             &nm_count));
            TEST(nm_count == 2);
            int32_t n =
                find_portable(nm_out, nm_count, Synth::instrument_graph_layout_parameter, 0xFFu, Synth::mod_volume, 0);
            TEST(n >= 0);
            if (n >= 0)
                TEST(nm_out[n].x == 111.5f && nm_out[n].y == 1.5f); // source ordinal 1 -> decoded ordinal 0
            n = find_portable(nm_out, nm_count, Synth::instrument_graph_layout_parameter, 0xFFu, Synth::mod_volume, 1);
            TEST(n >= 0);
            if (n >= 0)
                TEST(nm_out[n].x == 222.5f && nm_out[n].y == 2.5f); // source ordinal 2 -> decoded ordinal 1
            TEST(find_portable(nm_out, nm_count, 0xFFu, 0xFFu, 0xFFu, 2) == -1); // no stale third group

            // Input record order does not participate in identity: the reversed
            // input normalizes to byte-identical output.
            nm_swapped[0]             = nm_records[1];
            nm_swapped[1]             = nm_records[0];
            uint32_t nm_swapped_count = 4;
            TEST(Sculptor::normalize_instrument_graph_layout(nm_src,
                                                             nm_envs,
                                                             3,
                                                             nullptr,
                                                             0,
                                                             nm_swapped,
                                                             2,
                                                             nm_dec,
                                                             nm_dec_envs,
                                                             2,
                                                             nullptr,
                                                             0,
                                                             nm_swapped_out,
                                                             sizeof(nm_swapped_out) / sizeof(nm_swapped_out[0]),
                                                             &nm_swapped_count));
            TEST(nm_swapped_count == nm_count);
            TEST(memcmp(nm_swapped_out, nm_out, nm_count * sizeof(nm_out[0])) == 0);

            // Exercise the complete copy pipeline with A/A'/B descriptor interning.
            static Synth::InstrumentEditorBank nm_copy_bank;
            nm_copy_bank = {};
            nm_copy_bank.bank.instruments.allocate();
            nm_copy_bank.bank.instruments.entries[0] = nm_src;
            nm_copy_bank.bank.channel_enabled[0]     = 1;
            nm_copy_bank.bank.channel_zones[0][0]    = { 1, 0 };
            for (uint32_t envelope = 0; envelope < 3; ++envelope) {
                nm_copy_bank.bank.envelopes.allocate();
                nm_copy_bank.bank.envelopes.entries[envelope] = nm_envs[envelope];
            }
            nm_copy_bank.graph_layout_count = 2;
            for (uint32_t record = 0; record < 2; ++record) {
                Synth::GraphNodeLayout& saved = nm_copy_bank.graph_layout[record];
                saved.kind                    = 3;
                saved.uid                     = static_cast<uint8_t>(record + 1);
                saved.param_slot              = static_cast<uint8_t>(record + 2);
                saved.x                       = nm_records[record].x;
                saved.y                       = nm_records[record].y;
            }
            static char                         nm_copy_text[clipboard_capacity];
            static ClipboardDecode              nm_copy_decoded;
            static Synth::InstrumentGraphLayout nm_copy_export[max_portable_records];
            static Synth::InstrumentGraphLayout nm_copy_normalized[max_portable_records];
            uint32_t                            nm_copy_length =
                Synth::encode_instrument_json(nm_copy_text, sizeof(nm_copy_text), &nm_src, &nm_copy_bank.bank);
            TEST(nm_copy_length != 0);
            TEST(decode_clipboard(&nm_copy_decoded, nm_copy_text, max_portable_records));
            TEST(nm_copy_decoded.env_count == 2);
            uint32_t nm_copy_export_count = 0;
            TEST(Sculptor::encode_instrument_graph_layout(nm_src,
                                                          nm_copy_bank.bank,
                                                          nm_copy_bank,
                                                          0,
                                                          0,
                                                          nm_copy_export,
                                                          max_portable_records,
                                                          &nm_copy_export_count));
            uint32_t nm_copy_normalized_count = 0;
            TEST(Sculptor::normalize_instrument_graph_layout(nm_src,
                                                             nm_envs,
                                                             3,
                                                             nullptr,
                                                             0,
                                                             nm_copy_export,
                                                             nm_copy_export_count,
                                                             nm_copy_decoded.instr,
                                                             nm_copy_decoded.envs,
                                                             nm_copy_decoded.env_count,
                                                             nm_copy_decoded.lfos,
                                                             nm_copy_decoded.lfo_count,
                                                             nm_copy_normalized,
                                                             max_portable_records,
                                                             &nm_copy_normalized_count));
            TEST(nm_copy_normalized_count == 2);
            nm_copy_length = Synth::encode_instrument_json(nm_copy_text,
                                                           sizeof(nm_copy_text),
                                                           &nm_src,
                                                           &nm_copy_bank.bank,
                                                           nm_copy_normalized,
                                                           nm_copy_normalized_count);
            TEST(nm_copy_length != 0);
            TEST(decode_clipboard(&nm_copy_decoded, nm_copy_text, max_portable_records));
            TEST(nm_copy_decoded.env_count == 2 && nm_copy_decoded.layout_count == 2);
            TEST(nm_copy_decoded.layout[0].parameter_ordinal == 0 && nm_copy_decoded.layout[0].x == 111.5f);
            TEST(nm_copy_decoded.layout[1].parameter_ordinal == 1 && nm_copy_decoded.layout[1].x == 222.5f);

            // Source-only ordinal 2 does not exist in the decoded two-group model.
            nm_copy_normalized[1].parameter_ordinal = 2;
            TEST(Synth::encode_instrument_json(nm_copy_text,
                                               sizeof(nm_copy_text),
                                               &nm_src,
                                               &nm_copy_bank.bank,
                                               nm_copy_normalized,
                                               nm_copy_normalized_count) == 0);

            // Envelope nodes alias the same way, and the earliest source node
            // carrying a saved layout wins for the collapsed decoded node.
            static Synth::InstrumentGraphLayout nm_env_records[2];
            memset(nm_env_records, 0, sizeof(nm_env_records));
            nm_env_records[0].kind   = Synth::instrument_graph_layout_envelope;
            nm_env_records[0].layer  = 1; // A'
            nm_env_records[0].target = static_cast<uint8_t>(Synth::mod_volume);
            nm_env_records[0].x      = 33.5f;
            nm_env_records[1].kind   = Synth::instrument_graph_layout_envelope;
            nm_env_records[1].layer  = 0; // A
            nm_env_records[1].target = static_cast<uint8_t>(Synth::mod_volume);
            nm_env_records[1].x      = 11.5f;
            static Synth::InstrumentGraphLayout nm_env_out[4];
            uint32_t                            nm_env_count = 4;
            TEST(Sculptor::normalize_instrument_graph_layout(nm_src,
                                                             nm_envs,
                                                             3,
                                                             nullptr,
                                                             0,
                                                             nm_env_records,
                                                             2,
                                                             nm_dec,
                                                             nm_dec_envs,
                                                             2,
                                                             nullptr,
                                                             0,
                                                             nm_env_out,
                                                             sizeof(nm_env_out) / sizeof(nm_env_out[0]),
                                                             &nm_env_count));
            // A and A' are byte-identical, so the decoder interns them and both source
            // records name the one decoded envelope node: exactly one output record,
            // carrying the earliest source projected node's saved position and size.
            TEST(nm_env_count == 1);
            n = find_portable(nm_env_out,
                              nm_env_count,
                              Synth::instrument_graph_layout_envelope,
                              0,
                              Synth::mod_volume,
                              0xFFu);
            TEST(n >= 0);
            if (n >= 0)
                TEST(nm_env_out[n].x == 11.5f); // the earliest source projected node wins
            n = find_portable(nm_env_out,
                              nm_env_count,
                              Synth::instrument_graph_layout_envelope,
                              2,
                              Synth::mod_volume,
                              0xFFu);
            TEST(n == -1); // B had no saved position
            // The collapsed node's other source locator leaves no record of its own.
            TEST(find_portable(nm_env_out,
                               nm_env_count,
                               Synth::instrument_graph_layout_envelope,
                               1,
                               Synth::mod_volume,
                               0xFFu) == -1);
            // With only A' saved, its position is the one that survives, re-anchored
            // to the decoded node's earliest serving cell.
            nm_env_records[1] = nm_env_records[0];
            nm_env_count      = 4;
            TEST(Sculptor::normalize_instrument_graph_layout(nm_src,
                                                             nm_envs,
                                                             3,
                                                             nullptr,
                                                             0,
                                                             nm_env_records,
                                                             1,
                                                             nm_dec,
                                                             nm_dec_envs,
                                                             2,
                                                             nullptr,
                                                             0,
                                                             nm_env_out,
                                                             sizeof(nm_env_out) / sizeof(nm_env_out[0]),
                                                             &nm_env_count));
            TEST(nm_env_count == 1);
            TEST(nm_env_out[0].layer == 0 && nm_env_out[0].x == 33.5f);

            // Two LFO instances that share one descriptor but differ in their bound source
            // pair are two distinct projected nodes, so each keeps its own saved position and
            // its own source pair. Capacity exactly equal to the resolved count succeeds;
            // one slot less refuses with the record sink and the count untouched.
            static Synth::LFODescriptor na_lfos[1];
            memset(na_lfos, 0, sizeof(na_lfos));
            na_lfos[0].wave          = Synth::WaveType::sine_wave;
            na_lfos[0].duty          = 0x40;
            na_lfos[0].period_ms     = 300;
            na_lfos[0].min_value     = -1.0f;
            na_lfos[0].min_max_delta = 2.0f;

            static Synth::Instrument na_src;
            static Synth::Instrument na_dec;
            na_src.layer_count = 2;
            na_dec.layer_count = 2;
            for (uint32_t layer = 0; layer < 2; layer++) {
                na_src.layers[layer].osc_type[0] = Synth::WaveType::sine_wave;
                na_dec.layers[layer].osc_type[0] = Synth::WaveType::sine_wave;
                na_src.layers[layer].mod_ratio   = 1.0f;
                na_dec.layers[layer].mod_ratio   = 1.0f;
                Synth::LayerGen& src_gen         = na_src.layers[layer].gen[Synth::mod_volume];
                Synth::LayerGen& dec_gen         = na_dec.layers[layer].gen[Synth::mod_volume];
                src_gen.lfo_desc_id              = 1;
                dec_gen.lfo_desc_id              = 1;
                src_gen.lfo_op                   = Synth::SourceOp::add;
                dec_gen.lfo_op                   = Synth::SourceOp::add;
                src_gen.lfo_depth                = 0.5f;
                dec_gen.lfo_depth                = 0.5f;
                src_gen.lfo_rate_scale_ms        = 20.0f;
                dec_gen.lfo_rate_scale_ms        = 20.0f;
                src_gen.lfo_depth_source = layer == 0 ? Synth::ModSource::velocity : Synth::ModSource::aftertouch;
                src_gen.lfo_rate_source  = layer == 0 ? Synth::ModSource::mod_wheel : Synth::ModSource::pitch_bend;
                dec_gen.lfo_depth_source = src_gen.lfo_depth_source;
                dec_gen.lfo_rate_source  = src_gen.lfo_rate_source;
            }
            TEST(validate_standalone_model(na_src, nullptr, 0, na_lfos, 1));
            TEST(validate_standalone_model(na_dec, nullptr, 0, na_lfos, 1));

            static Synth::InstrumentGraphLayout na_pair_records[2];
            memset(na_pair_records, 0, sizeof(na_pair_records));
            for (uint32_t i = 0; i < 2; i++) {
                na_pair_records[i].kind   = Synth::instrument_graph_layout_lfo;
                na_pair_records[i].layer  = static_cast<uint8_t>(i);
                na_pair_records[i].target = static_cast<uint8_t>(Synth::mod_volume);
                na_pair_records[i].depth_source =
                    static_cast<uint8_t>(i == 0 ? Synth::ModSource::velocity : Synth::ModSource::aftertouch);
                na_pair_records[i].rate_source =
                    static_cast<uint8_t>(i == 0 ? Synth::ModSource::mod_wheel : Synth::ModSource::pitch_bend);
                na_pair_records[i].x = 7.0f + static_cast<float>(i);
                na_pair_records[i].y = -3.5f - static_cast<float>(i);
            }

            static Synth::InstrumentGraphLayout na_pair_out[4];
            constexpr uint32_t                  na_pair_capacity = sizeof(na_pair_out) / sizeof(na_pair_out[0]);
            uint32_t                            na_pair_count    = na_pair_capacity;
            TEST(Sculptor::normalize_instrument_graph_layout(na_src,
                                                             nullptr,
                                                             0,
                                                             na_lfos,
                                                             1,
                                                             na_pair_records,
                                                             2,
                                                             na_dec,
                                                             nullptr,
                                                             0,
                                                             na_lfos,
                                                             1,
                                                             na_pair_out,
                                                             na_pair_capacity,
                                                             &na_pair_count));
            TEST(na_pair_count == 2);
            n = find_portable(na_pair_out,
                              na_pair_count,
                              Synth::instrument_graph_layout_lfo,
                              0,
                              Synth::mod_volume,
                              0xFFu);
            TEST(n >= 0);
            if (n >= 0) {
                TEST(na_pair_out[n].x == 7.0f && na_pair_out[n].y == -3.5f);
                TEST(na_pair_out[n].depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
                TEST(na_pair_out[n].rate_source == static_cast<uint8_t>(Synth::ModSource::mod_wheel));
            }
            n = find_portable(na_pair_out,
                              na_pair_count,
                              Synth::instrument_graph_layout_lfo,
                              1,
                              Synth::mod_volume,
                              0xFFu);
            TEST(n >= 0);
            if (n >= 0) {
                TEST(na_pair_out[n].x == 8.0f && na_pair_out[n].y == -4.5f);
                TEST(na_pair_out[n].depth_source == static_cast<uint8_t>(Synth::ModSource::aftertouch));
                TEST(na_pair_out[n].rate_source == static_cast<uint8_t>(Synth::ModSource::pitch_bend));
            }

            static Synth::InstrumentGraphLayout na_exact[2];
            constexpr uint32_t                  na_exact_capacity = sizeof(na_exact) / sizeof(na_exact[0]);
            uint32_t                            na_exact_count    = na_exact_capacity;
            TEST(Sculptor::normalize_instrument_graph_layout(na_src,
                                                             nullptr,
                                                             0,
                                                             na_lfos,
                                                             1,
                                                             na_pair_records,
                                                             2,
                                                             na_dec,
                                                             nullptr,
                                                             0,
                                                             na_lfos,
                                                             1,
                                                             na_exact,
                                                             na_exact_capacity,
                                                             &na_exact_count));
            TEST(na_exact_count == 2);
            TEST(memcmp(na_exact, na_pair_out, sizeof(na_exact)) == 0);

            static Synth::InstrumentGraphLayout na_refuse[1];
            static Synth::InstrumentGraphLayout na_refuse_sentinel;
            memset(na_refuse, 0x5A, sizeof(na_refuse));
            memset(&na_refuse_sentinel, 0x5A, sizeof(na_refuse_sentinel));
            uint32_t na_refuse_count = 1;
            TEST(! Sculptor::normalize_instrument_graph_layout(na_src,
                                                               nullptr,
                                                               0,
                                                               na_lfos,
                                                               1,
                                                               na_pair_records,
                                                               2,
                                                               na_dec,
                                                               nullptr,
                                                               0,
                                                               na_lfos,
                                                               1,
                                                               na_refuse,
                                                               sizeof(na_refuse) / sizeof(na_refuse[0]),
                                                               &na_refuse_count));
            TEST(na_refuse_count == 1);
            TEST(memcmp(na_refuse, &na_refuse_sentinel, sizeof(na_refuse_sentinel)) == 0);

            // Normalizer refusals for locators the model cannot resolve: an envelope
            // record naming a decoded cell with no envelope bound, a canonical index past
            // the projection, and a kind that is not clipboard data. Each leaves the output
            // array and count untouched, so a refused normalization cannot leak a partially
            // translated record into the clipboard.
            static Synth::InstrumentGraphLayout nr_out[2];
            static Synth::InstrumentGraphLayout nr_out_sentinel[2];
            memset(nr_out, 0x5A, sizeof(nr_out));
            memset(nr_out_sentinel, 0x5A, sizeof(nr_out_sentinel));
            constexpr uint32_t nr_capacity = sizeof(nr_out) / sizeof(nr_out[0]);
            uint32_t           nr_count    = nr_capacity;
            for (uint32_t c = 0; c < 3; c++) {
                Synth::InstrumentGraphLayout nr_rec = {};
                if (c == 0) {
                    // nm_dec binds envelopes to mod_volume cells only, so this cell has
                    // no generator and nothing to attach a position to.
                    nr_rec.kind   = Synth::instrument_graph_layout_envelope;
                    nr_rec.layer  = 0;
                    nr_rec.target = static_cast<uint8_t>(Synth::mod_panning);
                }
                else if (c == 1) {
                    nr_rec.kind            = Synth::instrument_graph_layout_canonical;
                    nr_rec.canonical_index = 99; // past the projection
                }
                else {
                    nr_rec.kind = 4; // effect nodes are never clipboard data
                }
                nr_rec.x = 5.0f;
                nr_rec.y = 6.0f;
                nr_count = nr_capacity;
                TEST(! Sculptor::normalize_instrument_graph_layout(nm_src,
                                                                   nm_envs,
                                                                   3,
                                                                   nullptr,
                                                                   0,
                                                                   &nr_rec,
                                                                   1,
                                                                   nm_dec,
                                                                   nm_dec_envs,
                                                                   2,
                                                                   nullptr,
                                                                   0,
                                                                   nr_out,
                                                                   nr_capacity,
                                                                   &nr_count));
                TEST(nr_count == nr_capacity);
                TEST(memcmp(nr_out, &nr_out_sentinel, sizeof(nr_out_sentinel)) == 0);
            }
            // An output array smaller than the normalized records refuses the whole
            // normalization instead of truncating it: these two records normalize into two
            // slots when there is room for them.
            static Synth::InstrumentGraphLayout nr_tight[1];
            static Synth::InstrumentGraphLayout nr_tight_sentinel[1];
            memset(nr_tight, 0x5A, sizeof(nr_tight));
            memset(nr_tight_sentinel, 0x5A, sizeof(nr_tight_sentinel));
            uint32_t nr_tight_count = 1;
            TEST(! Sculptor::normalize_instrument_graph_layout(nm_src,
                                                               nm_envs,
                                                               3,
                                                               nullptr,
                                                               0,
                                                               nm_swapped,
                                                               2,
                                                               nm_dec,
                                                               nm_dec_envs,
                                                               2,
                                                               nullptr,
                                                               0,
                                                               nr_tight,
                                                               1,
                                                               &nr_tight_count));
            TEST(nr_tight_count == 1);
            TEST(memcmp(nr_tight, &nr_tight_sentinel, sizeof(nr_tight_sentinel)) == 0);
        }

        {
            // Object property order is not identity: a permuted document decodes to
            // equal outputs, in any key order, at any nesting level.
            static char            pp_text[clipboard_capacity];
            static ClipboardDecode pp_canonical;
            fill_clipboard_sentinels(&pp_canonical);
            const uint32_t pp_len =
                build_layout_doc(pp_text, sizeof(pp_text), clipboard_doc_head, clipboard_doc_records);
            TEST(pp_len > 0);
            TEST(decode_clipboard(&pp_canonical, pp_text, max_portable_records));
            TEST(pp_canonical.layout_count == 5);

            static char            pp_perm_text[clipboard_capacity];
            static ClipboardDecode pp_perm;
            fill_clipboard_sentinels(&pp_perm);
            build_layout_doc(pp_perm_text,
                             sizeof(pp_perm_text),
                             clipboard_doc_head,
                             "{\"height\":120,\"width\":220,\"y\":44.25,\"x\":120.5,\"target\":0,\"layer\":0,"
                             "\"kind\":1},"
                             "{\"y\":-40,\"parameter_ordinal\":0,\"kind\":3,\"x\":60,\"target\":0,\"height\":0,"
                             "\"width\":0},"
                             "{\"kind\":0,\"x\":64,\"canonical_index\":1,\"width\":0,\"y\":32,\"height\":0},"
                             "{\"rate_source\":2,\"height\":0,\"depth_source\":4,\"kind\":2,\"width\":0,"
                             "\"target\":0,\"y\":-8.5,\"layer\":0,\"x\":12.5},"
                             "{\"kind\":3,\"target\":1,\"parameter_ordinal\":0,\"x\":-60,\"y\":40,\"width\":0,"
                             "\"height\":0}");
            TEST(decode_clipboard(&pp_perm, pp_perm_text, max_portable_records));
            TEST(pp_perm.layout_count == pp_canonical.layout_count);
            TEST(pp_perm.env_count == pp_canonical.env_count);
            TEST(pp_perm.lfo_count == pp_canonical.lfo_count);
            TEST(pp_perm.instr.layer_count == pp_canonical.instr.layer_count);
            for (uint32_t i = 0; i < pp_canonical.layout_count; i++) {
                const int32_t match = find_portable(pp_perm.layout,
                                                    pp_perm.layout_count,
                                                    pp_canonical.layout[i].kind,
                                                    pp_canonical.layout[i].layer,
                                                    pp_canonical.layout[i].target,
                                                    pp_canonical.layout[i].parameter_ordinal);
                TEST(match >= 0);
                if (match >= 0) {
                    TEST(memcmp(&pp_perm.layout[match], &pp_canonical.layout[i], sizeof(pp_canonical.layout[i])) == 0);
                }
            }
            for (uint32_t i = 0; i < pp_canonical.env_count; i++) {
                TEST(memcmp(&pp_perm.envs[i], &pp_canonical.envs[i], sizeof(pp_perm.envs[i])) == 0);
            }

            // Reordering the document's top-level keys is equally inert, and a
            // required key whose value is zero is still required.
            const char* const pp_reordered =
                "{\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":2,"
                "\"pitch_offset_semitones\":1.5,\"volume\":{\"base\":0.8,"
                "\"envelope\":{\"sustain_start\":1,\"sustain_end\":2,\"points\":["
                "{\"level\":0.0625,\"time_seconds\":0},{\"level\":0.5625,\"time_seconds\":0.25},"
                "{\"level\":1.0625,\"time_seconds\":0.5}]},"
                "\"lfo\":{\"depth\":0.5,\"rate_source\":\"mod_wheel\",\"wave\":\"sine\",\"duty\":0.25,"
                "\"frequency_hz\":4,\"depth_source\":\"velocity\",\"rate_scale_ms\":20}},"
                "\"pitch\":{\"envelope\":{\"sustain_start\":0,\"sustain_end\":1,\"points\":["
                "{\"level\":0.25,\"time_seconds\":0},{\"level\":-0.25,\"time_seconds\":0.5}]},"
                "\"base\":0},\"panning\":{\"base\":0.5},\"lowpass_cutoff\":{\"base\":2000},"
                "\"highpass_cutoff\":{\"base\":2000}}],"
                "\"layer_skew_semitones\":0,\"note_skew_semitones\":0,\"format\":\"synth-instrument-v1\","
                "\"graph_layout\":[{\"kind\":0,\"canonical_index\":1,\"x\":0,\"y\":0,\"width\":0,"
                "\"height\":0}]}";
            static ClipboardDecode pp_reorder_dec;
            fill_clipboard_sentinels(&pp_reorder_dec);
            TEST(decode_clipboard(&pp_reorder_dec, pp_reordered, max_portable_records));
            TEST(pp_reorder_dec.layout_count == 1);
            TEST(pp_reorder_dec.layout[0].x == 0.0f); // zero-valued required fields decode
            TEST(pp_reorder_dec.env_count == 2);
            TEST(pp_reorder_dec.lfo_count == 1);
            TEST(pp_reorder_dec.instr.routing[Synth::mod_volume].base_value == 0.8f);
        }

        {
            // Every malformed portable record and every locator the model cannot
            // resolve refuses the whole document with all outputs intact. The
            // baseline document decodes, so each refusal is the record's doing.
            static char            ml_text[clipboard_capacity];
            static ClipboardDecode ml_attempt;
            static ClipboardDecode ml_sentinel;
            fill_clipboard_sentinels(&ml_sentinel);
            build_layout_doc(ml_text, sizeof(ml_text), clipboard_doc_head, clipboard_doc_records);
            static ClipboardDecode ml_baseline;
            fill_clipboard_sentinels(&ml_baseline);
            TEST(decode_clipboard(&ml_baseline, ml_text, max_portable_records));
            TEST(ml_baseline.layout_count == 5);

            struct LayoutRejection {
                const char* records;
                const char* dimension;
            };

            const LayoutRejection rejections[] = {
                { "{\"kind\":99,\"canonical_index\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}", "unknown kind" },
                { "{\"kind\":0,\"canonical_index\":1,\"x\":0,\"y\":0,\"width\":0,\"height\":0,\"z\":1}",
                  "unknown key" },
                { "{\"kind\":0,\"canonical_index\":1,\"x\":0,\"y\":0,\"width\":0}", "missing required field" },
                { "{\"kind\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}", "locator key omitted at value zero" },
                { "{\"kind\":0,\"canonical_index\":\"1\",\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "wrong token type" },
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"canonical_index\":0,\"x\":0,\"y\":0,\"width\":0,"
                  "\"height\":0}",
                  "unrelated locator key" },
                { "{\"kind\":1,\"layer\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}", "missing locator field" },
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":0,\"y\":0,\"width\":-1,\"height\":0}", "negative width" },
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1e400,\"y\":0,\"width\":0,\"height\":0}",
                  "overflowed number" },
                { "{\"kind\":1,\"layer\":7,\"target\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "layer out of range" },
                { "{\"kind\":1,\"layer\":0,\"target\":99,\"x\":0,\"y\":0,\"width\":0,\"height\":0}", "unknown target" },
                { "{\"kind\":2,\"layer\":0,\"target\":0,\"depth_source\":7,\"rate_source\":0,\"x\":0,\"y\":0,"
                  "\"width\":0,\"height\":0}",
                  "source out of range" },
                { "{\"kind\":0,\"canonical_index\":9,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "canonical index beyond the bound" },
                { "{\"kind\":3,\"target\":0,\"parameter_ordinal\":35,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "parameter ordinal beyond the bound" },
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":1,\"width\":0,\"height\":0},"
                  "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":2,\"y\":2,\"width\":0,\"height\":0}",
                  "duplicate portable locator" },
                { "{\"kind\":3,\"target\":8,\"parameter_ordinal\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "unresolved locator" },
            };
            for (uint32_t i = 0; i < sizeof(rejections) / sizeof(rejections[0]); i++) {
                static char ml_bad[clipboard_capacity];
                build_layout_doc(ml_bad, sizeof(ml_bad), clipboard_doc_head, rejections[i].records);
                fill_clipboard_sentinels(&ml_attempt);
                if (decode_clipboard(&ml_attempt, ml_bad, max_portable_records)) {
                    failed(rejections[i].dimension, __FILE__, __LINE__);
                }
                TEST(clipboard_sentinels_intact(&ml_attempt, &ml_sentinel));
            }

            // Two distinct locators that resolve to the one interned descriptor
            // address a single node twice: refused. The one-record form of the same
            // document decodes, so the refusal is the second locator's.
            static char ml_alias[clipboard_capacity];
            build_layout_doc(ml_alias,
                             sizeof(ml_alias),
                             clipboard_alias_head,
                             "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":1,\"width\":0,\"height\":0}");
            static ClipboardDecode ml_alias_baseline;
            fill_clipboard_sentinels(&ml_alias_baseline);
            TEST(decode_clipboard(&ml_alias_baseline, ml_alias, max_portable_records));
            TEST(ml_alias_baseline.env_count == 1);
            build_layout_doc(ml_alias,
                             sizeof(ml_alias),
                             clipboard_alias_head,
                             "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":1,\"width\":0,\"height\":0},"
                             "{\"kind\":1,\"layer\":0,\"target\":1,\"x\":2,\"y\":2,\"width\":0,\"height\":0}");
            fill_clipboard_sentinels(&ml_attempt);
            TEST(decode_clipboard(&ml_attempt, ml_alias, max_portable_records) == false);
            TEST(clipboard_sentinels_intact(&ml_attempt, &ml_sentinel));

            // Over the fixed record bound. The bound equals the number of locators the
            // model can express, so any extra record also repeats a locator: what this
            // pins is the refusal and the untouched outputs, not which check ran first.
            {
                static char ml_over[clipboard_capacity];
                uint32_t    ml_over_pos = 0;
                ml_over_pos += static_cast<uint32_t>(snprintf(ml_over + ml_over_pos,
                                                              sizeof(ml_over) - ml_over_pos,
                                                              "%s\"graph_layout\":[",
                                                              clipboard_doc_head));
                for (uint32_t r = 0; r <= max_portable_records; r++) {
                    ml_over_pos += static_cast<uint32_t>(snprintf(ml_over + ml_over_pos,
                                                                  sizeof(ml_over) - ml_over_pos,
                                                                  "{\"kind\":0,\"canonical_index\":%u,\"x\":0,\"y\":0,"
                                                                  "\"width\":0,\"height\":0}%s",
                                                                  r % Synth::graph_canonical_node_count,
                                                                  r == max_portable_records ? "" : ","));
                }
                // Built by hand rather than through build_layout_doc, so it closes the
                // record array and the document object itself.
                ml_over_pos +=
                    static_cast<uint32_t>(snprintf(ml_over + ml_over_pos, sizeof(ml_over) - ml_over_pos, "]}"));
                TEST(ml_over_pos < sizeof(ml_over));
                // Assembled by hand, so it is parsed here before it is rejected for its count.
                TEST(count_clipboard_tokens(ml_over, ml_over_pos) > 0);
                fill_clipboard_sentinels(&ml_attempt);
                TEST(decode_clipboard(&ml_attempt, ml_over, max_portable_records) == false);
                TEST(clipboard_sentinels_intact(&ml_attempt, &ml_sentinel));
            }
        }

        {
            // Codec compatibility in both directions.
            static char    cc_doc[clipboard_capacity];
            const uint32_t cc_len = build_layout_doc(cc_doc, sizeof(cc_doc), clipboard_doc_head, clipboard_doc_records);
            TEST(cc_len > 0);

            // A valid nonempty layout is refused by the legacy decoder rather than
            // silently dropped, with every legacy output left at its sentinel.
            static ClipboardDecode cc_legacy_attempt;
            static ClipboardDecode cc_legacy_sentinel;
            fill_clipboard_sentinels(&cc_legacy_attempt);
            fill_clipboard_sentinels(&cc_legacy_sentinel);
            TEST(! Synth::decode_instrument_json(cc_doc,
                                                 cc_len,
                                                 &cc_legacy_attempt.instr,
                                                 cc_legacy_attempt.envs,
                                                 &cc_legacy_attempt.env_count,
                                                 cc_legacy_attempt.lfos,
                                                 &cc_legacy_attempt.lfo_count));
            TEST(clipboard_sentinels_intact(&cc_legacy_attempt, &cc_legacy_sentinel));

            // An absent or empty field decodes; the new decoder accepts both and
            // reports a zero layout count through a null layout output while the
            // descriptor counts stay actual.
            static char cc_empty[clipboard_capacity];
            build_layout_doc(cc_empty, sizeof(cc_empty), clipboard_doc_head, "");
            static ClipboardDecode cc_empty_dec;
            fill_clipboard_sentinels(&cc_empty_dec);
            TEST(decode_clipboard(&cc_empty_dec, cc_empty, max_portable_records));
            TEST(cc_empty_dec.layout_count == 0);
            TEST(cc_empty_dec.env_count == 2);
            TEST(cc_empty_dec.lfo_count == 1);
            uint32_t                         cc_null_env  = 999;
            uint32_t                         cc_null_lfo  = 999;
            uint32_t                         cc_null_rows = 999;
            static Synth::Instrument         cc_null_instr;
            static Synth::EnvelopeDescriptor cc_null_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      cc_null_lfos[Synth::instrument_max_lfos];
            TEST(Synth::decode_instrument_json(cc_empty,
                                               static_cast<uint32_t>(strlen(cc_empty)),
                                               &cc_null_instr,
                                               cc_null_envs,
                                               &cc_null_env,
                                               cc_null_lfos,
                                               &cc_null_lfo,
                                               nullptr,
                                               0,
                                               &cc_null_rows));
            TEST(cc_null_rows == 0);
            TEST(cc_null_env == 2);
            TEST(cc_null_lfo == 1);

            // The same with a descriptorless instrument.
            const char* const cc_bare = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}],";
            static char       cc_bare_doc[clipboard_capacity];
            build_layout_doc(cc_bare_doc, sizeof(cc_bare_doc), cc_bare, "");
            uint32_t cc_bare_env  = 999;
            uint32_t cc_bare_lfo  = 999;
            uint32_t cc_bare_rows = 999;
            TEST(Synth::decode_instrument_json(cc_bare_doc,
                                               static_cast<uint32_t>(strlen(cc_bare_doc)),
                                               &cc_null_instr,
                                               cc_null_envs,
                                               &cc_bare_env,
                                               cc_null_lfos,
                                               &cc_bare_lfo,
                                               nullptr,
                                               0,
                                               &cc_bare_rows));
            TEST(cc_bare_rows == 0);
            TEST(cc_bare_env == 0);
            TEST(cc_bare_lfo == 0);

            // Exact layout capacity succeeds; one slot short fails with every output
            // untouched, as does a nonempty layout with no output at all.
            static Synth::InstrumentGraphLayout cc_out[max_portable_records];
            static Synth::Instrument            cc_out_instr;
            static Synth::EnvelopeDescriptor    cc_out_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor         cc_out_lfos[Synth::instrument_max_lfos];
            uint32_t                            cc_out_env  = 999;
            uint32_t                            cc_out_lfo  = 999;
            uint32_t                            cc_out_rows = 999;
            TEST(Synth::decode_instrument_json(cc_doc,
                                               cc_len,
                                               &cc_out_instr,
                                               cc_out_envs,
                                               &cc_out_env,
                                               cc_out_lfos,
                                               &cc_out_lfo,
                                               cc_out,
                                               5,
                                               &cc_out_rows));
            TEST(cc_out_rows == 5);
            TEST(cc_out_env == 2);
            TEST(cc_out_lfo == 1);

            static ClipboardDecode cc_short;
            static ClipboardDecode cc_short_sentinel;
            fill_clipboard_sentinels(&cc_short);
            fill_clipboard_sentinels(&cc_short_sentinel);
            TEST(! Synth::decode_instrument_json(cc_doc,
                                                 cc_len,
                                                 &cc_short.instr,
                                                 cc_short.envs,
                                                 &cc_short.env_count,
                                                 cc_short.lfos,
                                                 &cc_short.lfo_count,
                                                 cc_short.layout,
                                                 4,
                                                 &cc_short.layout_count));
            TEST(clipboard_sentinels_intact(&cc_short, &cc_short_sentinel));
            TEST(! Synth::decode_instrument_json(cc_doc,
                                                 cc_len,
                                                 &cc_short.instr,
                                                 cc_short.envs,
                                                 &cc_short.env_count,
                                                 cc_short.lfos,
                                                 &cc_short.lfo_count,
                                                 nullptr,
                                                 0,
                                                 &cc_short.layout_count));
            TEST(clipboard_sentinels_intact(&cc_short, &cc_short_sentinel));
        }

        {
            // The largest clipboard document is measured against the clipboard
            // capacity and the parser token bound, never assumed. Seven layers, an
            // eight-point envelope and a distinct LFO on every projected target, and
            // the full portable record set.
            static Synth::InstrumentBank        mf_bank;
            static Synth::Instrument            mf_instr;
            static Synth::InstrumentGraphLayout mf_records[max_portable_records];
            static Synth::GraphNodeLayout       mf_mapped[max_portable_records];
            static char                         mf_text[clipboard_capacity];
            const uint32_t                      mf_count = build_clipboard_max_fixture(&mf_bank, &mf_instr, mf_records);
            const uint32_t                      mf_slot  = mf_bank.instruments.allocate();
            TEST(mf_slot != pool_no_slot);
            mf_bank.instruments.entries[mf_slot]   = mf_instr;
            mf_bank.channel_enabled[0]             = 1;
            mf_bank.channel_zones[0][0].start_note = 1;
            mf_bank.channel_zones[0][0].instrument = static_cast<uint8_t>(mf_slot);
            TEST(Synth::validate_instrument_bank(&mf_bank));
            TEST(mf_bank.envelopes.num_allocated == 35);
            TEST(mf_bank.lfos.num_allocated == 35);
            TEST(mf_count == max_portable_records);

            const uint32_t mf_len =
                Synth::encode_instrument_json(mf_text, sizeof(mf_text), &mf_instr, &mf_bank, mf_records, mf_count);
            TEST(mf_len > 0);
            // Payload bytes exclude the terminator the caller writes; the required
            // capacity is one byte more.
            const int32_t mf_tokens = count_clipboard_tokens(mf_text, mf_len);
            if (getenv("SYNTH_UNIT_MEASURE")) {
                fprintf(stderr,
                        "clipboard max fixture: payload %u bytes, required capacity %u bytes, %d tokens (bound %u, "
                        "buffer %u)\n",
                        mf_len,
                        mf_len + 1,
                        mf_tokens,
                        clipboard_token_bound,
                        clipboard_capacity);
            }
            TEST(mf_len + 1 <= clipboard_capacity);

            // Capacity one byte short of payload + terminator fails. Destination bytes are
            // unspecified on failure, so only the return value is asserted; the identical
            // call at the exact capacity then succeeds with the measured length, which is
            // what shows the short call is the capacity and not the document.
            static char mf_short_text[clipboard_capacity];
            TEST(Synth::encode_instrument_json(mf_short_text, mf_len, &mf_instr, &mf_bank, mf_records, mf_count) == 0);
            static char mf_exact_text[clipboard_capacity];
            TEST(Synth::encode_instrument_json(mf_exact_text, mf_len + 1, &mf_instr, &mf_bank, mf_records, mf_count) ==
                 mf_len);
            TEST(mf_tokens > 0);
            TEST(static_cast<uint32_t>(mf_tokens) <= clipboard_token_bound);

            static ClipboardDecode mf_dec;
            fill_clipboard_sentinels(&mf_dec);
            TEST(decode_clipboard(&mf_dec, mf_text, max_portable_records));
            TEST(mf_dec.layout_count == max_portable_records);
            TEST(mf_dec.env_count == 35);
            TEST(mf_dec.lfo_count == 35);
            TEST(mf_dec.instr.layer_count == Synth::max_layers);

            uint32_t mf_mapped_count = max_portable_records;
            TEST(Sculptor::map_instrument_graph_layout(mf_dec.instr,
                                                       mf_dec.envs,
                                                       mf_dec.env_count,
                                                       mf_dec.lfos,
                                                       mf_dec.lfo_count,
                                                       mf_dec.layout,
                                                       mf_dec.layout_count,
                                                       mf_mapped,
                                                       max_portable_records,
                                                       &mf_mapped_count));
            TEST(mf_mapped_count == max_portable_records);
            for (uint32_t i = 0; i < max_portable_records; i++) {
                TEST(mf_mapped[i].x == mf_records[i].x);
                TEST(mf_mapped[i].y == mf_records[i].y);
                TEST(mf_mapped[i].width_override == mf_records[i].width_override);
                TEST(mf_mapped[i].height_override == mf_records[i].height_override);
            }

            // One slot short of the record count refuses the whole document.
            static ClipboardDecode mf_short;
            static ClipboardDecode mf_sentinel;
            fill_clipboard_sentinels(&mf_short);
            fill_clipboard_sentinels(&mf_sentinel);
            TEST(decode_clipboard(&mf_short, mf_text, max_portable_records - 1) == false);
            TEST(clipboard_sentinels_intact(&mf_short, &mf_sentinel));

            // The same instrument through the legacy entry point is smaller by the
            // record array alone and carries an empty layout: the layout array is the
            // only addition to the document.
            static char    mf_legacy[clipboard_capacity];
            const uint32_t mf_legacy_len =
                Synth::encode_instrument_json(mf_legacy, sizeof(mf_legacy), &mf_instr, &mf_bank);
            TEST(mf_legacy_len > 0);
            TEST(mf_legacy_len < mf_len);
            TEST(strstr(mf_legacy, "\"graph_layout\":[]") != nullptr);

            // A legal portable LFO record has nine key/value pairs: 19 tokens.
            // This bounds legal layout output, not rejection order for malformed input.
            // The instrument body is measured from this fixture's legacy encoding.
            constexpr uint32_t max_tokens_per_record = 2u * 9u + 1u; // object + 9 keys + 9 values
            const int32_t      mf_body_tokens        = count_clipboard_tokens(mf_legacy, mf_legacy_len);
            TEST(mf_body_tokens > 0);
            TEST(max_portable_records * max_tokens_per_record < clipboard_token_bound);
            TEST(static_cast<uint64_t>(mf_body_tokens) + uint64_t{ max_portable_records } * max_tokens_per_record <
                 uint64_t{ clipboard_token_bound });
            if (getenv("SYNTH_UNIT_MEASURE")) {
                fprintf(stderr,
                        "token arithmetic: instrument body %d tokens + %u records x %u <= bound %u\n",
                        mf_body_tokens,
                        max_portable_records,
                        max_tokens_per_record,
                        clipboard_token_bound);
            }
        }

        {
            // Import with no generator wiring at all: canonical records resolve, and
            // surplus records naming nodes the instrument does not have are refused.
            const char* const      nw_head = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}],";
            static char            nw_text[clipboard_capacity];
            static ClipboardDecode nw_dec;
            fill_clipboard_sentinels(&nw_dec);
            build_layout_doc(nw_text,
                             sizeof(nw_text),
                             nw_head,
                             "{\"kind\":0,\"canonical_index\":0,\"x\":1,\"y\":2,\"width\":0,\"height\":0},"
                             "{\"kind\":0,\"canonical_index\":1,\"x\":3,\"y\":4,\"width\":5,\"height\":6}");
            TEST(decode_clipboard(&nw_dec, nw_text, max_portable_records));
            TEST(nw_dec.layout_count == 2);
            TEST(nw_dec.env_count == 0);
            TEST(nw_dec.lfo_count == 0);
            static Synth::GraphNodeLayout nw_mapped[8];
            uint32_t                      nw_mapped_count = 8;
            TEST(Sculptor::map_instrument_graph_layout(nw_dec.instr,
                                                       nw_dec.envs,
                                                       nw_dec.env_count,
                                                       nw_dec.lfos,
                                                       nw_dec.lfo_count,
                                                       nw_dec.layout,
                                                       nw_dec.layout_count,
                                                       nw_mapped,
                                                       8,
                                                       &nw_mapped_count));
            TEST(nw_mapped_count == 2);
            TEST(find_mapped(nw_mapped, nw_mapped_count, 0, 0) >= 0);
            TEST(find_mapped(nw_mapped, nw_mapped_count, 0, 1) >= 0);
            TEST(nw_mapped[0].name[0] == 0);

            const char* const surplus[] = {
                "{\"kind\":0,\"canonical_index\":3,\"x\":1,\"y\":2,\"width\":0,\"height\":0}",  // oscillator layer 1
                "{\"kind\":0,\"canonical_index\":99,\"x\":1,\"y\":2,\"width\":0,\"height\":0}", // past the bound
                "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":2,\"width\":0,\"height\":0}",
                "{\"kind\":3,\"target\":0,\"parameter_ordinal\":0,\"x\":1,\"y\":2,\"width\":0,\"height\":0}",
            };
            for (uint32_t i = 0; i < sizeof(surplus) / sizeof(surplus[0]); i++) {
                static char nw_bad[clipboard_capacity];
                build_layout_doc(nw_bad, sizeof(nw_bad), nw_head, surplus[i]);
                static ClipboardDecode nw_attempt;
                static ClipboardDecode nw_sentinel;
                fill_clipboard_sentinels(&nw_sentinel);
                fill_clipboard_sentinels(&nw_attempt);
                TEST(decode_clipboard(&nw_attempt, nw_bad, max_portable_records) == false);
                TEST(clipboard_sentinels_intact(&nw_attempt, &nw_sentinel));
            }
        }

        {
            // Candidate replacement replaces only the destination zone's layout and
            // mask row: every other root, name, record, effect layout and channel
            // effect title survives, and surviving effect LFO titles follow the
            // actual descriptor renumbering.
            static Synth::InstrumentEditorBank rp_bank;
            static Synth::Instrument           rp_instr;
            build_clipboard_zone_fixture(&rp_bank, &rp_instr);
            TEST(rp_bank.bank.instruments.allocate() == 1);
            rp_bank.bank.instruments.entries[1]         = rp_instr;
            rp_bank.bank.channel_zones[0][1].start_note = 65;
            rp_bank.bank.channel_zones[0][1].instrument = 1;
            strcpy(rp_bank.instrument_names[1], "Other zone");
            // Effect-chain titles are independent roots: the channel chain's node
            // title and LFO title, and the master chain's LFO title. LFO 2 is
            // referenced by nothing, LFO 3 by the channel chain, so the compaction
            // renumbers "LFO 3" down to "LFO 2" while "LFO 1" stays.
            add_detached_record(&rp_bank, 0, 0, 4, 0, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            strcpy(rp_bank.graph_layout[rp_bank.graph_layout_count - 1].name, "Delay");
            add_detached_record(&rp_bank, 0, 0, 4, 0, 7.0f, 8.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            strcpy(rp_bank.graph_layout[rp_bank.graph_layout_count - 1].name, "LFO 3");
            add_detached_record(&rp_bank, 16, 0, 4, 0, 9.0f, 10.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            strcpy(rp_bank.graph_layout[rp_bank.graph_layout_count - 1].name, "LFO 1");
            // A detached record on the other zone, and an extant-layer mask bit per zone.
            add_detached_record(&rp_bank, 0, 1, 1, 1, 11.0f, 12.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
            const Synth::GraphNodeLayout rp_other_record = rp_bank.graph_layout[rp_bank.graph_layout_count - 1];
            rp_bank.graph_missing_sum[0][0]              = 1;
            rp_bank.graph_missing_sum[0][1]              = 1;
            for (uint32_t extra = 0; extra < 2; extra++) {
                const uint32_t added                       = rp_bank.bank.lfos.allocate();
                rp_bank.bank.lfos.entries[added].wave      = Synth::WaveType::sine_wave;
                rp_bank.bank.lfos.entries[added].period_ms = static_cast<uint16_t>(120 + 30 * extra);
            }
            rp_bank.bank.channel_chains[0].num_effects                        = 1;
            rp_bank.bank.channel_chains[0].effects[0].type                    = Synth::EffectType::delay;
            rp_bank.bank.channel_chains[0].effects[0].enabled                 = true;
            rp_bank.bank.channel_chains[0].effects[0].bindings[0].base_value  = 0.4f;
            rp_bank.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 3;
            rp_bank.bank.channel_chains[0].effects[0].bindings[0].lfo_depth   = 0.1f;
            rp_bank.bank.master_chain.num_effects                             = 1;
            rp_bank.bank.master_chain.effects[0].type                         = Synth::EffectType::distortion;
            rp_bank.bank.master_chain.effects[0].enabled                      = true;
            rp_bank.bank.master_chain.effects[0].bindings[0].base_value       = 1.0f;
            rp_bank.bank.master_chain.effects[0].bindings[0].lfo_desc_id      = 1;
            TEST(Synth::validate_instrument_bank(&rp_bank.bank));
            TEST(Sculptor::validate_editor_metadata(rp_bank));

            static char            rp_doc[clipboard_capacity];
            static ClipboardDecode rp_dec;
            build_layout_doc(rp_doc, sizeof(rp_doc), clipboard_doc_head, clipboard_doc_records);
            fill_clipboard_sentinels(&rp_dec);
            TEST(decode_clipboard(&rp_dec, rp_doc, max_portable_records));

            static Synth::InstrumentEditorBank rp_candidate;
            memset(&rp_candidate, 0x5A, sizeof(rp_candidate));
            static Synth::InstrumentEditorBank rp_source_before;
            rp_source_before       = rp_bank;
            const bool rp_replaced = Sculptor::replace_zone_instrument_candidate(rp_bank,
                                                                                 0,
                                                                                 0,
                                                                                 rp_dec.instr,
                                                                                 rp_dec.envs,
                                                                                 rp_dec.env_count,
                                                                                 rp_dec.lfos,
                                                                                 rp_dec.lfo_count,
                                                                                 rp_dec.layout,
                                                                                 rp_dec.layout_count,
                                                                                 &rp_candidate);
            TEST(rp_replaced);
            // The candidate keeps its sentinel bytes until the transaction succeeds,
            // so its contents are inspected only on a successful paste.
            if (rp_replaced) {
                TEST(memcmp(&rp_bank, &rp_source_before, sizeof(rp_bank)) == 0);
                TEST(Synth::validate_instrument_bank(&rp_candidate.bank));
                TEST(Sculptor::validate_editor_metadata(rp_candidate));

                const uint32_t rp_dest_slot  = rp_candidate.bank.channel_zones[0][0].instrument;
                const uint32_t rp_other_slot = rp_candidate.bank.channel_zones[0][1].instrument;
                TEST(rp_candidate.bank.channel_zones[0][0].start_note == 1);
                TEST(rp_candidate.bank.channel_zones[0][1].start_note == 65);
                TEST(rp_dest_slot != rp_other_slot);
                uint16_t rp_envelope_ids[Synth::instrument_max_envelopes] = {};
                uint16_t rp_lfo_ids[Synth::instrument_max_lfos]           = {};
                rp_envelope_ids[0]                                        = 3;
                rp_envelope_ids[1]                                        = 4;
                rp_lfo_ids[0]                                             = 3;
                Synth::Instrument rp_expected;
                Synth::remap_instrument(rp_dec.instr, rp_envelope_ids, rp_lfo_ids, &rp_expected);
                TEST(memcmp(&rp_candidate.bank.instruments.entries[rp_dest_slot], &rp_expected, sizeof(rp_expected)) ==
                     0);
                TEST(strcmp(rp_candidate.instrument_names[rp_dest_slot], "Clipboard zone") == 0);
                TEST(strcmp(rp_candidate.instrument_names[rp_other_slot], "Other zone") == 0);
                TEST(Sculptor::count_detached_records(rp_candidate, 0, 1) == 1);
                for (uint32_t r = 0; r < rp_candidate.graph_layout_count; r++) {
                    const Synth::GraphNodeLayout& record = rp_candidate.graph_layout[r];
                    if (record.channel == 0 && record.zone == 1) {
                        TEST(memcmp(&record, &rp_other_record, sizeof(record)) == 0);
                    }
                }
                TEST(rp_candidate.graph_missing_sum[0][0] == 0);
                TEST(rp_candidate.graph_missing_sum[0][1] == 1);

                // The destination zone's stale records are gone and it carries exactly
                // the mapped decoded layout (the canonical record aside, which keys a
                // bound node rather than a detached one).
                TEST(Sculptor::count_detached_records(rp_candidate, 0, 0) == rp_dec.layout_count - 1);
                uint32_t rp_pasted  = 0;
                uint32_t rp_effects = 0;
                bool     rp_delay   = false;
                bool     rp_renamed = false;
                bool     rp_master  = false;
                for (uint32_t r = 0; r < rp_candidate.graph_layout_count; r++) {
                    const Synth::GraphNodeLayout& record = rp_candidate.graph_layout[r];
                    if (record.kind == 4) {
                        ++rp_effects;
                        if (record.channel == 0 && strcmp(record.name, "Delay") == 0) {
                            rp_delay = record.x == 5.0f && record.y == 6.0f;
                        }
                        if (record.channel == 0 && strcmp(record.name, "LFO 2") == 0) {
                            rp_renamed = record.x == 7.0f && record.y == 8.0f;
                        }
                        if (record.channel == 16 && strcmp(record.name, "LFO 1") == 0) {
                            rp_master = record.x == 9.0f && record.y == 10.0f;
                        }
                        continue;
                    }
                    if (record.channel == 0 && record.zone == 0) {
                        ++rp_pasted;
                    }
                }
                TEST(rp_pasted == rp_dec.layout_count);
                TEST(rp_effects == 3);
                TEST(rp_delay);
                TEST(rp_renamed);
                TEST(rp_master);
                TEST(rp_candidate.bank.lfos.num_allocated == 3);
                TEST(rp_candidate.bank.instruments.entries[rp_dest_slot].layers[0].gen[Synth::mod_volume].lfo_desc_id ==
                     3);
                TEST(rp_candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == 2);
            }
        }

        {
            // An editor bank whose own metadata is invalid is refused before any
            // candidate is written: the paste must not launder a broken source.
            static Synth::InstrumentEditorBank md_bank;
            static Synth::Instrument           md_instr;
            build_clipboard_zone_fixture(&md_bank, &md_instr);
            add_detached_record(&md_bank, 0, 0, 4, 0, 1.0f, 2.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            // An effect record without a node title cannot key its node.
            TEST(md_bank.graph_layout[md_bank.graph_layout_count - 1].name[0] == 0);
            TEST(! Sculptor::validate_editor_metadata(md_bank));

            static Synth::Instrument md_bare;
            md_bare                       = {};
            md_bare.layer_count           = 1;
            md_bare.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
            md_bare.layers[0].osc_type[1] = Synth::WaveType::sine_wave;
            md_bare.layers[0].mod_ratio   = 1.0f;

            static Synth::InstrumentEditorBank md_candidate;
            static Synth::InstrumentEditorBank md_sentinel;
            memset(&md_candidate, 0x5A, sizeof(md_candidate));
            memset(&md_sentinel, 0x5A, sizeof(md_sentinel));
            TEST(! Sculptor::replace_zone_instrument_candidate(md_bank,
                                                               0,
                                                               0,
                                                               md_bare,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               &md_candidate));
            TEST(memcmp(&md_candidate, &md_sentinel, sizeof(md_sentinel)) == 0);
        }

        {
            // Placeholder selection and refusal sequencing.
            //
            // A descriptorless instrument pasted over the sole zone of a channel is
            // valid with no descriptor at all, and the uniquely rooted destination
            // slot is reused in place.
            static Synth::Instrument pl_bare;
            pl_bare                                        = {};
            pl_bare.layer_count                            = 1;
            pl_bare.layers[0].osc_type[0]                  = Synth::WaveType::sine_wave;
            pl_bare.layers[0].osc_type[1]                  = Synth::WaveType::sine_wave;
            pl_bare.layers[0].osc_mode                     = Synth::osc_mode_blend;
            pl_bare.layers[0].mod_ratio                    = 1.0f;
            pl_bare.routing[Synth::mod_panning].base_value = 0.5f;

            static Synth::InstrumentEditorBank pl_bank;
            static Synth::Instrument           pl_instr;
            build_clipboard_zone_fixture(&pl_bank, &pl_instr);
            TEST(Synth::validate_instrument_bank(&pl_bank.bank));
            TEST(Sculptor::validate_editor_metadata(pl_bank));

            static Synth::InstrumentEditorBank pl_candidate;
            memset(&pl_candidate, 0x5A, sizeof(pl_candidate));
            static Synth::InstrumentEditorBank pl_before;
            pl_before = pl_bank;
            TEST(Sculptor::replace_zone_instrument_candidate(pl_bank,
                                                             0,
                                                             0,
                                                             pl_bare,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             &pl_candidate));
            TEST(memcmp(&pl_bank, &pl_before, sizeof(pl_bank)) == 0);
            TEST(Synth::validate_instrument_bank(&pl_candidate.bank));
            TEST(Sculptor::validate_editor_metadata(pl_candidate));
            TEST(pl_candidate.bank.channel_zones[0][0].instrument == 0);
            TEST(pl_candidate.bank.instruments.num_allocated == 1);
            TEST(pl_candidate.bank.envelopes.num_allocated == 0); // nothing roots them any more
            TEST(pl_candidate.bank.lfos.num_allocated == 0);
            TEST(memcmp(&pl_candidate.bank.instruments.entries[0], &pl_bare, sizeof(pl_bare)) == 0);
            TEST(strcmp(pl_candidate.instrument_names[0], "Clipboard zone") == 0);

            // Destination arguments outside the bank refuse with the candidate
            // untouched.
            static Synth::InstrumentEditorBank pl_sentinel;
            memset(&pl_sentinel, 0x5A, sizeof(pl_sentinel));
            memset(&pl_candidate, 0x5A, sizeof(pl_candidate));
            TEST(! Sculptor::replace_zone_instrument_candidate(pl_bank,
                                                               Synth::max_channels,
                                                               0,
                                                               pl_bare,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               &pl_candidate));
            TEST(memcmp(&pl_candidate, &pl_sentinel, sizeof(pl_sentinel)) == 0);
            memset(&pl_candidate, 0x5A, sizeof(pl_candidate));
            TEST(! Sculptor::replace_zone_instrument_candidate(pl_bank,
                                                               0,
                                                               1, // the channel has one zone entry
                                                               pl_bare,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               &pl_candidate));
            TEST(memcmp(&pl_candidate, &pl_sentinel, sizeof(pl_sentinel)) == 0);
        }

        {
            // A fully allocated instrument pool cannot hand out a placeholder, so a
            // uniquely rooted destination succeeds by reusing its own slot while a
            // shared destination must refuse: the pool is full before reclaim, and an
            // allocated-but-unrooted slot is not free.
            static Synth::InstrumentEditorBank fp_bank;
            fp_bank = {};
            for (uint32_t i = 0; i < Synth::max_instruments; i++) {
                TEST(fp_bank.bank.instruments.allocate() == i);
                fp_bank.bank.instruments.entries[i].layer_count           = 1;
                fp_bank.bank.instruments.entries[i].layers[0].osc_type[0] = Synth::WaveType::sine_wave;
                fp_bank.bank.instruments.entries[i].layers[0].mod_ratio   = 1.0f;
            }
            fp_bank.bank.channel_enabled[0]             = 1;
            fp_bank.bank.channel_zones[0][0].start_note = 1;
            fp_bank.bank.channel_zones[0][0].instrument = 7;
            TEST(Synth::validate_instrument_bank(&fp_bank.bank));

            static Synth::Instrument fp_bare;
            fp_bare                       = {};
            fp_bare.layer_count           = 1;
            fp_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
            fp_bare.layers[0].osc_type[1] = Synth::WaveType::noise_wave;
            fp_bare.layers[0].mod_ratio   = 1.0f;

            static Synth::InstrumentEditorBank fp_candidate;
            memset(&fp_candidate, 0x5A, sizeof(fp_candidate));
            TEST(Sculptor::replace_zone_instrument_candidate(fp_bank,
                                                             0,
                                                             0,
                                                             fp_bare,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             &fp_candidate));
            TEST(Synth::validate_instrument_bank(&fp_candidate.bank));
            TEST(fp_candidate.bank.instruments.num_allocated >= 1);
            TEST(fp_candidate.bank.instruments.entries[fp_candidate.bank.channel_zones[0][0].instrument]
                     .layers[0]
                     .osc_type[0] == Synth::WaveType::noise_wave);

            // Same full pool, shared destination: no free or unallocated slot exists
            // before reclaim, so the transaction refuses even though reclaim would
            // later free unrooted slots.
            static Synth::InstrumentEditorBank fs_bank;
            fs_bank                                     = fp_bank;
            fs_bank.bank.channel_zones[0][1].start_note = 65;
            fs_bank.bank.channel_zones[0][1].instrument = 7;
            TEST(Synth::validate_instrument_bank(&fs_bank.bank));

            static Synth::InstrumentEditorBank fs_candidate;
            static Synth::InstrumentEditorBank fs_sentinel;
            memset(&fs_candidate, 0x5A, sizeof(fs_candidate));
            memset(&fs_sentinel, 0x5A, sizeof(fs_sentinel));
            TEST(! Sculptor::replace_zone_instrument_candidate(fs_bank,
                                                               0,
                                                               0,
                                                               fp_bare,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               &fs_candidate));
            TEST(memcmp(&fs_candidate, &fs_sentinel, sizeof(fs_sentinel)) == 0);
        }

        {
            // A shared destination with a free slot gets its own placeholder, so the
            // other zone keeps playing the instrument it always played.
            static Synth::InstrumentEditorBank sh_bank;
            sh_bank = {};
            TEST(sh_bank.bank.instruments.allocate() == 0);
            TEST(sh_bank.bank.instruments.allocate() == 1);
            sh_bank.bank.instruments.entries[0].layer_count           = 1;
            sh_bank.bank.instruments.entries[0].layers[0].osc_type[0] = Synth::WaveType::sine_wave;
            sh_bank.bank.instruments.entries[0].layers[0].mod_ratio   = 1.0f;
            sh_bank.bank.instruments.entries[1].layer_count           = 1;
            sh_bank.bank.instruments.entries[1].layers[0].osc_type[0] = Synth::WaveType::sawtooth_wave;
            sh_bank.bank.instruments.entries[1].layers[0].mod_ratio   = 1.0f;
            sh_bank.bank.channel_enabled[0]                           = 1;
            sh_bank.bank.channel_zones[0][0].start_note               = 1;
            sh_bank.bank.channel_zones[0][0].instrument               = 0;
            sh_bank.bank.channel_zones[0][1].start_note               = 65;
            sh_bank.bank.channel_zones[0][1].instrument               = 0; // shared
            strcpy(sh_bank.instrument_names[0], "Shared");
            TEST(Synth::validate_instrument_bank(&sh_bank.bank));
            const Synth::Instrument sh_old = sh_bank.bank.instruments.entries[0];

            static Synth::Instrument sh_bare;
            sh_bare                       = {};
            sh_bare.layer_count           = 1;
            sh_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
            sh_bare.layers[0].osc_type[1] = Synth::WaveType::noise_wave;
            sh_bare.layers[0].mod_ratio   = 1.0f;

            static Synth::InstrumentEditorBank sh_candidate;
            memset(&sh_candidate, 0x5A, sizeof(sh_candidate));
            TEST(Sculptor::replace_zone_instrument_candidate(sh_bank,
                                                             0,
                                                             0,
                                                             sh_bare,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             nullptr,
                                                             0,
                                                             &sh_candidate));
            TEST(Synth::validate_instrument_bank(&sh_candidate.bank));
            const uint32_t sh_zone_zero = sh_candidate.bank.channel_zones[0][0].instrument;
            const uint32_t sh_zone_one  = sh_candidate.bank.channel_zones[0][1].instrument;
            TEST(sh_zone_zero != sh_zone_one);
            TEST(memcmp(&sh_candidate.bank.instruments.entries[sh_zone_zero], &sh_bare, sizeof(sh_bare)) == 0);
            TEST(memcmp(&sh_candidate.bank.instruments.entries[sh_zone_one], &sh_old, sizeof(sh_old)) == 0);
            TEST(sh_candidate.bank.instruments.num_allocated == 2);
            TEST(strcmp(sh_candidate.instrument_names[sh_zone_one], "Shared") == 0);

            {
                // A decoded instrument carrying descriptor references the paste does not
                // install is refused: the source bank keeps its bytes and the candidate keeps
                // its sentinel. The same bank accepts a bare instrument with no references, so
                // the refusal is the reference and not the bank.
                static Synth::InstrumentEditorBank vr_bank;
                static Synth::Instrument           vr_instr;
                build_clipboard_zone_fixture(&vr_bank, &vr_instr);

                static Synth::Instrument vr_bare;
                vr_bare                       = {};
                vr_bare.layer_count           = 1;
                vr_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
                vr_bare.layers[0].mod_ratio   = 1.0f;
                TEST(validate_standalone_model(vr_bare, nullptr, 0, nullptr, 0));

                static Synth::InstrumentEditorBank vr_ok;
                memset(&vr_ok, 0x5A, sizeof(vr_ok));
                TEST(Sculptor::replace_zone_instrument_candidate(vr_bank,
                                                                 0,
                                                                 0,
                                                                 vr_bare,
                                                                 nullptr,
                                                                 0,
                                                                 nullptr,
                                                                 0,
                                                                 nullptr,
                                                                 0,
                                                                 &vr_ok));

                static Synth::InstrumentEditorBank vr_candidate;
                static Synth::InstrumentEditorBank vr_sentinel;
                memset(&vr_candidate, 0x5A, sizeof(vr_candidate));
                memset(&vr_sentinel, 0x5A, sizeof(vr_sentinel));
                static Synth::InstrumentEditorBank vr_before;
                vr_before = vr_bank;

                // Envelope id past the descriptor pool, with no descriptors installed.
                static Synth::Instrument vr_bad_env;
                vr_bad_env = vr_bare;
                vr_bad_env.layers[0].gen[Synth::mod_volume].envelope_desc_id =
                    static_cast<uint16_t>(Synth::instrument_max_envelopes + 1);
                TEST(! validate_standalone_model(vr_bad_env, nullptr, 0, nullptr, 0));
                TEST(! Sculptor::replace_zone_instrument_candidate(vr_bank,
                                                                   0,
                                                                   0,
                                                                   vr_bad_env,
                                                                   nullptr,
                                                                   0,
                                                                   nullptr,
                                                                   0,
                                                                   nullptr,
                                                                   0,
                                                                   &vr_candidate));
                TEST(memcmp(&vr_candidate, &vr_sentinel, sizeof(vr_sentinel)) == 0);
                TEST(memcmp(&vr_bank, &vr_before, sizeof(vr_before)) == 0);

                // LFO id past the descriptors that come with the instrument.
                static Synth::LFODescriptor vr_lfo;
                vr_lfo               = {};
                vr_lfo.wave          = Synth::WaveType::sine_wave;
                vr_lfo.period_ms     = 500;
                vr_lfo.min_value     = 0.0f;
                vr_lfo.min_max_delta = 1.0f;
                static Synth::Instrument vr_bad_lfo;
                vr_bad_lfo                                              = vr_bare;
                vr_bad_lfo.layers[0].gen[Synth::mod_volume].lfo_desc_id = 2;
                TEST(! validate_standalone_model(vr_bad_lfo, nullptr, 0, &vr_lfo, 1));
                TEST(! Sculptor::replace_zone_instrument_candidate(vr_bank,
                                                                   0,
                                                                   0,
                                                                   vr_bad_lfo,
                                                                   nullptr,
                                                                   0,
                                                                   &vr_lfo,
                                                                   1,
                                                                   nullptr,
                                                                   0,
                                                                   &vr_candidate));
                TEST(memcmp(&vr_candidate, &vr_sentinel, sizeof(vr_sentinel)) == 0);
                TEST(memcmp(&vr_bank, &vr_before, sizeof(vr_before)) == 0);

                // A layout record naming a generator the decoded instrument does not project.
                static Synth::InstrumentGraphLayout vr_records[1];
                memset(vr_records, 0, sizeof(vr_records));
                vr_records[0].kind   = Synth::instrument_graph_layout_envelope;
                vr_records[0].layer  = 0;
                vr_records[0].target = static_cast<uint8_t>(Synth::mod_panning);
                vr_records[0].x      = 3.0f;
                vr_records[0].y      = 4.0f;
                TEST(! Sculptor::replace_zone_instrument_candidate(vr_bank,
                                                                   0,
                                                                   0,
                                                                   vr_bare,
                                                                   nullptr,
                                                                   0,
                                                                   nullptr,
                                                                   0,
                                                                   vr_records,
                                                                   1,
                                                                   &vr_candidate));
                TEST(memcmp(&vr_candidate, &vr_sentinel, sizeof(vr_sentinel)) == 0);
                TEST(memcmp(&vr_bank, &vr_before, sizeof(vr_before)) == 0);

                // The one refusal this fixture cannot construct: an effect LFO title colliding
                // with a renumbered instrument LFO title after the reclaim's LFO remap. The
                // remap is injective over survivors and effect chains are roots of their own,
                // so no reachable input produces the collision; what does happen is the
                // renumber rekeying the effect's title, pinned by the reclaim-map fixture.
            }
        }

        {
            // A disabled destination channel still classifies roots: it is enabled
            // privately, validated and counted with the reclaim's own rule, and the
            // original enabled flag is restored in the candidate. The replacement is
            // observably different from the shared instrument, and the other zone keeps
            // its own instrument bytes, name, layout records and missing-sum row.
            static Synth::InstrumentEditorBank dc_bank;
            dc_bank = {};
            TEST(dc_bank.bank.instruments.allocate() == 0);
            TEST(dc_bank.bank.instruments.allocate() == 1);
            dc_bank.bank.instruments.entries[0].layer_count                           = 1;
            dc_bank.bank.instruments.entries[0].layers[0].osc_type[0]                 = Synth::WaveType::sawtooth_wave;
            dc_bank.bank.instruments.entries[0].layers[0].mod_ratio                   = 3.0f;
            dc_bank.bank.instruments.entries[0].routing[Synth::mod_volume].base_value = 0.25f;
            dc_bank.bank.instruments.entries[1].layer_count                           = 1;
            dc_bank.bank.instruments.entries[1].layers[0].osc_type[0]                 = Synth::WaveType::pulse_wave;
            dc_bank.bank.instruments.entries[1].layers[0].mod_ratio                   = 1.0f;
            dc_bank.bank.channel_zones[0][0].start_note                               = 1;
            dc_bank.bank.channel_zones[0][0].instrument                               = 0;
            dc_bank.bank.channel_zones[0][1].start_note                               = 65;
            dc_bank.bank.channel_zones[0][1].instrument = 0; // shared with the destination
            strcpy(dc_bank.instrument_names[0], "Shared disabled");
            // The other zone's canonical metadata needs no generator descriptor.
            add_detached_record(&dc_bank,
                                0,
                                1,
                                Synth::instrument_graph_layout_canonical,
                                0,
                                13.5f,
                                14.5f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                0);
            dc_bank.graph_missing_sum[0][1]              = 1;
            const Synth::GraphNodeLayout dc_other_record = dc_bank.graph_layout[dc_bank.graph_layout_count - 1];
            // Validate the complete privately enabled state before disabling it.
            static Synth::InstrumentEditorBank dc_enabled;
            dc_enabled                         = dc_bank;
            dc_enabled.bank.channel_enabled[0] = 1;
            TEST(Synth::validate_instrument_bank(&dc_enabled.bank));
            TEST(Sculptor::validate_editor_metadata(dc_enabled));
            dc_bank.bank.channel_enabled[0] = 0;
            TEST(Synth::validate_instrument_bank(&dc_bank.bank));
            TEST(Sculptor::validate_editor_metadata(dc_bank));
            const Synth::Instrument dc_old           = dc_bank.bank.instruments.entries[0];
            const uint32_t          dc_other_records = Sculptor::count_detached_records(dc_bank, 0, 1);
            const uint32_t          dc_record_count  = dc_bank.graph_layout_count;

            static Synth::Instrument dc_bare;
            dc_bare                                       = {};
            dc_bare.layer_count                           = 1;
            dc_bare.layers[0].osc_type[0]                 = Synth::WaveType::noise_wave;
            dc_bare.layers[0].osc_type[1]                 = Synth::WaveType::noise_wave;
            dc_bare.layers[0].mod_ratio                   = 1.0f;
            dc_bare.routing[Synth::mod_volume].base_value = 0.75f;

            static Synth::InstrumentEditorBank dc_candidate;
            memset(&dc_candidate, 0x5A, sizeof(dc_candidate));
            const bool dc_replaced = Sculptor::replace_zone_instrument_candidate(dc_bank,
                                                                                 0,
                                                                                 0,
                                                                                 dc_bare,
                                                                                 nullptr,
                                                                                 0,
                                                                                 nullptr,
                                                                                 0,
                                                                                 nullptr,
                                                                                 0,
                                                                                 &dc_candidate);
            TEST(dc_replaced);
            // Reading the candidate at all requires the transaction to have succeeded:
            // a refused one leaves it uninitialised.
            if (dc_replaced) {
                TEST(Synth::validate_instrument_bank(&dc_candidate.bank));
                TEST(dc_candidate.bank.channel_enabled[0] == 0); // restored, not left enabled
                const uint32_t dc_zone_zero = dc_candidate.bank.channel_zones[0][0].instrument;
                const uint32_t dc_zone_one  = dc_candidate.bank.channel_zones[0][1].instrument;
                TEST(dc_zone_zero != dc_zone_one);
                TEST(memcmp(&dc_candidate.bank.instruments.entries[dc_zone_zero], &dc_bare, sizeof(dc_bare)) == 0);
                TEST(memcmp(&dc_candidate.bank.instruments.entries[dc_zone_one], &dc_old, sizeof(dc_old)) == 0);
                TEST(strcmp(dc_candidate.instrument_names[dc_zone_one], "Shared disabled") == 0);
                TEST(Sculptor::count_detached_records(dc_candidate, 0, 1) == dc_other_records);
                TEST(dc_candidate.graph_layout_count == dc_record_count);
                uint32_t preserved_other_zone_record_count = 0;
                for (uint32_t r = 0; r < dc_candidate.graph_layout_count; r++) {
                    const Synth::GraphNodeLayout& record = dc_candidate.graph_layout[r];
                    if (record.channel == 0 && record.zone == 1) {
                        ++preserved_other_zone_record_count;
                        TEST(memcmp(&record, &dc_other_record, sizeof(record)) == 0);
                    }
                }
                TEST(preserved_other_zone_record_count == 1);
                TEST(dc_candidate.graph_missing_sum[0][1] == 1);
            }

            // A destination table that is only invalid once the channel is enabled
            // (a zone referencing an instrument the pool does not hold) refuses the
            // whole transaction.
            static Synth::InstrumentEditorBank dt_bank;
            dt_bank                                     = dc_bank;
            dt_bank.bank.channel_zones[0][1].instrument = 200;
            TEST(Synth::validate_instrument_bank(&dt_bank.bank)); // disabled: the table is skipped

            static Synth::InstrumentEditorBank dt_candidate;
            static Synth::InstrumentEditorBank dt_sentinel;
            memset(&dt_candidate, 0x5A, sizeof(dt_candidate));
            memset(&dt_sentinel, 0x5A, sizeof(dt_sentinel));
            TEST(! Sculptor::replace_zone_instrument_candidate(dt_bank,
                                                               0,
                                                               0,
                                                               dc_bare,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               nullptr,
                                                               0,
                                                               &dt_candidate));
            TEST(memcmp(&dt_candidate, &dt_sentinel, sizeof(dt_sentinel)) == 0);
        }

        {
            // This exercises measurement-pool overflow, not production decoder
            // rejection order: a well-formed document exceeds the measurement bound.
            static char ov_doc[clipboard_capacity];
            uint32_t    ov_pos = 0;
            ov_pos += static_cast<uint32_t>(snprintf(ov_doc + ov_pos, sizeof(ov_doc) - ov_pos, "{\"a\":["));
            for (uint32_t t = 0; t < clipboard_token_bound && ov_pos + 4 < sizeof(ov_doc); t++) {
                ov_pos +=
                    static_cast<uint32_t>(snprintf(ov_doc + ov_pos, sizeof(ov_doc) - ov_pos, "%s0", t == 0 ? "" : ","));
            }
            ov_pos += static_cast<uint32_t>(snprintf(ov_doc + ov_pos, sizeof(ov_doc) - ov_pos, "]}"));
            TEST(ov_pos < sizeof(ov_doc));
            TEST(count_clipboard_tokens(ov_doc, ov_pos) == -1);
            // A document that fits the bound still measures, so the -1 above is a bound
            // result and not a truncation artifact of the generated prefix.
            static const char ov_small_doc[] = "{\"a\":[0,1,2]}";
            TEST(count_clipboard_tokens(ov_small_doc, static_cast<uint32_t>(strlen(ov_small_doc))) > 0);
        }

        {
            // Ordering of the GUI paste command against the candidate helper.
            check_zone_paste_source_ordering();
        }

        {
            // Clipboard sizing estimate, measured rather than assumed: the storage
            // one copy/paste transaction holds at once - one document buffer, one parser
            // token pool at the specified bound, the portable and mapped record arrays, one
            // decoded instrument with its descriptor arrays, and the reclaim map. Nothing
            // here is treated as shared with the codec's own scratch: only physically shared
            // buffers are counted once, so this is the clipboard's own footprint and not the
            // editor's resident set.
            const uint64_t clipboard_staging =
                clipboard_capacity + uint64_t{ clipboard_token_bound } * sizeof(jsmntok_t) +
                uint64_t{ max_portable_records } * sizeof(Synth::InstrumentGraphLayout) * 2 +
                uint64_t{ max_portable_records } * sizeof(Synth::GraphNodeLayout) + sizeof(Synth::Instrument) +
                uint64_t{ Synth::instrument_max_envelopes } * sizeof(Synth::EnvelopeDescriptor) +
                uint64_t{ Synth::instrument_max_lfos } * sizeof(Synth::LFODescriptor) +
                uint64_t{ Synth::max_lfos } * sizeof(uint16_t);
            // Complete resident storage is bounded by the owner in sculptor_instr_edit.cpp.
            if (getenv("SYNTH_UNIT_MEASURE")) {
                fprintf(
                    stderr,
                    "clipboard staging %llu B; parser storage %llu B; clipboard text %u B; candidate bank %llu B; "
                    "portable records %llu B; mapped records %llu B; decoded model %llu B; descriptor maps %llu B\n",
                    static_cast<unsigned long long>(clipboard_staging),
                    static_cast<unsigned long long>(1024u * 1024u + uint64_t{ clipboard_token_bound } *
                                                                        (sizeof(jsmntok_t) + sizeof(uint32_t))),
                    clipboard_capacity,
                    static_cast<unsigned long long>(sizeof(Synth::InstrumentEditorBank)),
                    static_cast<unsigned long long>(uint64_t{ max_portable_records } *
                                                    sizeof(Synth::InstrumentGraphLayout)),
                    static_cast<unsigned long long>(uint64_t{ max_portable_records } * sizeof(Synth::GraphNodeLayout)),
                    static_cast<unsigned long long>(
                        sizeof(Synth::Instrument) +
                        uint64_t{ Synth::instrument_max_envelopes } * sizeof(Synth::EnvelopeDescriptor) +
                        uint64_t{ Synth::instrument_max_lfos } * sizeof(Synth::LFODescriptor)),
                    static_cast<unsigned long long>(2u * Synth::max_lfos * sizeof(uint16_t)));
                fprintf(stderr,
                        "runtime instrument bank %llu bytes; editor bank %llu bytes; bank queue %llu bytes\n",
                        static_cast<unsigned long long>(sizeof(Synth::InstrumentBank)),
                        static_cast<unsigned long long>(sizeof(Synth::InstrumentEditorBank)),
                        static_cast<unsigned long long>(sizeof(Synth::BankUpdateQueue)));
            }
            TEST(clipboard_staging < 1024u * 1024u + 64u * 1024u + 16u * 1024u);
        }

        // The default instrument round-trips too.
        static Synth::InstrumentBank default_bank;
        memset(&default_bank, 0, sizeof(default_bank));
        TEST(Synth::init_default_channel(&default_bank, 0));
        const uint32_t default_encoded_len = Synth::encode_instrument_json(encoded,
                                                                           sizeof(encoded),
                                                                           &default_bank.instruments.entries[0],
                                                                           &default_bank);
        TEST(default_encoded_len > 0);
        // New clipboard output includes graph_layout even when no node moved.
        TEST(strstr(encoded, "\"graph_layout\":[]") != nullptr);
        static Synth::Instrument         default_decoded;
        static Synth::EnvelopeDescriptor default_envs[Synth::instrument_max_envelopes];
        static Synth::LFODescriptor      default_lfos[Synth::instrument_max_lfos];
        uint32_t                         default_env_count = 999;
        uint32_t                         default_lfo_count = 999;
        TEST(Synth::decode_instrument_json(encoded,
                                           default_encoded_len,
                                           &default_decoded,
                                           default_envs,
                                           &default_env_count,
                                           default_lfos,
                                           &default_lfo_count));
        TEST(default_decoded.layer_count == 1);

        // Older documents without graph_layout remain accepted, and an empty optional
        // field is accepted by the same decoder.
        const char* const empty_layout_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}],\"graph_layout\":[]}";
        static Synth::Instrument         empty_layout_instr;
        static Synth::EnvelopeDescriptor empty_layout_envs[Synth::instrument_max_envelopes];
        static Synth::LFODescriptor      empty_layout_lfos[Synth::instrument_max_lfos];
        uint32_t                         empty_layout_env_count = 999;
        uint32_t                         empty_layout_lfo_count = 999;
        TEST(Synth::decode_instrument_json(empty_layout_doc,
                                           static_cast<uint32_t>(strlen(empty_layout_doc)),
                                           &empty_layout_instr,
                                           empty_layout_envs,
                                           &empty_layout_env_count,
                                           empty_layout_lfos,
                                           &empty_layout_lfo_count));

        // The shipped example document stays valid: it is what users and LLMs
        // copy from (doc/synth_instrument_example.json).
        {
            static char example[64 * 1024];
            FILE* const example_file = fopen("doc/synth_instrument_example.json", "rb");
            TEST(example_file != nullptr);
            const size_t got = example_file ? fread(example, 1, sizeof(example) - 1, example_file) : 0;
            if (example_file) {
                fclose(example_file);
            }
            TEST(got > 0 && got < sizeof(example) - 1);
            static Synth::Instrument         example_instr;
            static Synth::EnvelopeDescriptor example_envs[Synth::instrument_max_envelopes];
            static Synth::LFODescriptor      example_lfos[Synth::instrument_max_lfos];
            uint32_t                         example_env_count = 0;
            uint32_t                         example_lfo_count = 0;
            if (got > 0) {
                TEST(Synth::decode_instrument_json(example,
                                                   static_cast<uint32_t>(got),
                                                   &example_instr,
                                                   example_envs,
                                                   &example_env_count,
                                                   example_lfos,
                                                   &example_lfo_count));
                TEST(example_instr.layer_count == 2);
                TEST(example_instr.layers[0].osc_mode == Synth::osc_mode_fm);
                TEST(example_env_count == 2);
                TEST(example_lfo_count == 2);
            }
        }
    }

    return exit_code;
}
