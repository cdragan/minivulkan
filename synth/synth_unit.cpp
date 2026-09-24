// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/rng.h"
#include "../sculptor/sculptor_bank_json.h"
#include "../sculptor/sculptor_graph.h"
#include "../sculptor/sculptor_instr_bank.h"
#include "../sculptor/sculptor_instr_library.h"
#include "../sculptor/sculptor_osc_graph.h"
#include "midi_file.h"
#include "synth_effect_expansion.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_parameters.h"
#include "synth_serialize.h"
#include "synth_soundtrack.h"
#include <cmath>
#include <float.h>
#include <stdio.h>
#include <string.h>

#define TEST(test)                         \
    if (! (test)) {                        \
        failed(#test, __FILE__, __LINE__); \
    }

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

// Checks via TEST that a projection filled the mapping consistently: every
// expected node exists and is distinct, connector slots are pinned, and an
// envelope/LFO node exists exactly when its descriptor id is nonzero.
static void check_osc_graph_mapping(const Sculptor::Graph&           graph,
                                    const Sculptor::OscGraphMapping& mapping,
                                    uint32_t                         layer_count,
                                    uint32_t                         env_bindings,
                                    uint32_t                         lfo_bindings)
{
    bool     seen[Sculptor::max_nodes] = {};
    uint32_t count                     = 0;
    for (uint32_t idx = 0; idx < Sculptor::num_osc_graph_inputs; idx++) {
        TEST(mapping.input_nodes[idx] != Sculptor::pool_no_slot);
        TEST(osc_graph_mark_node(seen, mapping.input_nodes[idx], &count));
    }
    TEST(mapping.input_output_slot != Sculptor::pool_no_slot);
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
            // 21 connectors (output + 5 targets x [2 direct, env, LFO]) plus at
            // least the 14 oscillator properties.
            TEST(graph.node(mapping.osc_nodes[layer]).slots.num_allocated >= 35);
        }
        else {
            TEST(mapping.osc_nodes[layer] == Sculptor::pool_no_slot);
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            if (layer < layer_count) {
                TEST((mapping.env_nodes[layer][t] != Sculptor::pool_no_slot) == (mapping.env_desc_ids[layer][t] != 0));
                TEST((mapping.lfo_nodes[layer][t] != Sculptor::pool_no_slot) == (mapping.lfo_desc_ids[layer][t] != 0));
                if (mapping.env_desc_ids[layer][t] != 0) {
                    TEST(osc_graph_mark_node(seen, mapping.env_nodes[layer][t], &count));
                }
                if (mapping.lfo_desc_ids[layer][t] != 0) {
                    TEST(osc_graph_mark_node(seen, mapping.lfo_nodes[layer][t], &count));
                }
            }
            else {
                TEST(mapping.env_nodes[layer][t] == Sculptor::pool_no_slot);
                TEST(mapping.lfo_nodes[layer][t] == Sculptor::pool_no_slot);
                TEST(mapping.env_desc_ids[layer][t] == 0);
                TEST(mapping.lfo_desc_ids[layer][t] == 0);
            }
        }
    }
    uint32_t projected_targets = 0;
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        if (osc_graph_target(static_cast<Synth::ModTarget>(t))) {
            projected_targets++;
            TEST(mapping.osc_direct_input_slot[t][0] != Sculptor::pool_no_slot);
            TEST(mapping.osc_direct_input_slot[t][1] != Sculptor::pool_no_slot);
            TEST(mapping.osc_env_input_slot[t] != Sculptor::pool_no_slot);
            TEST(mapping.osc_lfo_input_slot[t] != Sculptor::pool_no_slot);
        }
    }
    TEST(projected_targets == 5);
    TEST(count == 7 + layer_count + env_bindings + lfo_bindings); // 6 inputs + output + oscillators + bindings
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
// Writes the shared envelope id and the LFO id (both 1-based).
static void build_osc_graph_small_fixture(Synth::InstrumentBank* bank,
                                          Synth::Instrument*     instrument,
                                          uint16_t*              shared_env_id,
                                          uint16_t*              lfo_id)
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

    osc.gen[Synth::mod_volume].envelope_desc_id = *shared_env_id;
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

namespace {

constexpr uint32_t fake_region_base = 8192;

struct FakeWriter {
    uint32_t next_node;
    uint32_t dest_count;
    uint32_t lfo_count;
    uint16_t last_dest_node;
    uint16_t last_dest_lfo_node;
};

uint32_t fake_alloc_node(const void* ctx)
{
    FakeWriter& writer = *static_cast<FakeWriter*>(const_cast<void*>(ctx));
    return writer.next_node++;
}

uint16_t fake_resolve_source(const void*, Synth::ModSource, uint32_t)
{
    return 77; // arbitrary concrete node id
}

void fake_configure_dest(const void* ctx,
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

void fake_configure_lfo(const void* ctx, uint32_t, uint16_t, Synth::SourceOp, float, uint16_t, uint16_t, float)
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

} // namespace

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
        Synth::InstrumentBank bank = {};
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
        Synth::InstrumentBank bank = {};
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
        Synth::InstrumentBank bank = {};
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
        Synth::InstrumentBank bank                  = {};
        const uint32_t        instr                 = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.drum_track_channel                     = 9;
        const uint32_t env                          = bank.envelopes.allocate();
        bank.envelopes.entries[env].num_points      = 7;

        Synth::InstrumentBank snapshot;
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
        Synth::InstrumentBank bank  = {};
        const uint32_t        env   = bank.envelopes.allocate();
        const uint32_t        lfo   = bank.lfos.allocate();
        const uint32_t        instr = bank.instruments.allocate();

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

        Synth::InstrumentBank restored = {};
        TEST(Synth::decode_instrument_bank(image, written, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);

        // A buffer too small to hold the image fails cleanly, writing nothing.
        TEST(Synth::encode_instrument_bank(&bank, image, 4) == 0);

        // A corrupt marker is rejected.
        uint8_t bad[Synth::instrument_bank_image_size<Synth::InstrumentBank>];
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
        Synth::InstrumentBank bank                  = {};
        bank.drum_track_channel                     = 7;
        const uint32_t instr                        = bank.instruments.allocate();
        bank.instruments.entries[instr].layer_count = 3;
        bank.channel_zones[2][0]                    = { 65, static_cast<uint8_t>(instr) };

        const char* const path = "synth_bank_roundtrip.tmp";
        TEST(Synth::save_instrument_bank(path, &bank));

        Synth::InstrumentBank restored = {};
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

    // Default recipe: init_default_bank builds the first-run bank; every structural
    // invariant the editor and runtime rely on holds out of the box.
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
        TEST(bank.envelopes.num_allocated == 1);
        TEST(bank.lfos.num_allocated == 3); // vibrato, tremolo, master FIR sweep
        TEST(bank.channel_chains[0].num_effects == 1);
        TEST(bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(bank.channel_chains[0].effects[0].enabled);
        TEST(bank.master_chain.num_effects == 3);
        // The default instrument wires its descriptors through the local-id remap.
        const Synth::Instrument& instr = bank.instruments.entries[0];
        TEST(instr.layer_count == 1);
        TEST(instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 1);
        TEST(instr.layers[0].gen[Synth::mod_pitch].lfo_desc_id == 1);
        TEST(instr.layers[0].gen[Synth::mod_volume].lfo_desc_id == 2);
        TEST(bank.master_chain.effects[2].bindings[0].lfo_desc_id == 3);
    }

    // init_default_channel into a populated bank appends the recipe into the free slots and
    // remaps its descriptor references past the existing content.
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
        TEST(bank.envelopes.num_allocated == 2);
        TEST(bank.lfos.num_allocated == 3);
        TEST(bank.channel_zones[5][0].start_note == 1);
        TEST(bank.channel_zones[5][0].instrument == 1);
        const Synth::Instrument& instr = bank.instruments.entries[1];
        TEST(instr.layers[0].gen[Synth::mod_volume].envelope_desc_id == 2);
        TEST(instr.layers[0].gen[Synth::mod_pitch].lfo_desc_id == 2);
        TEST(instr.layers[0].gen[Synth::mod_volume].lfo_desc_id == 3);
        TEST(bank.channel_chains[5].num_effects == 1);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // init_default_channel fails cleanly when a pool lacks space, leaving the bank
    // byte-for-byte untouched.
    {
        static Synth::InstrumentBank bank;
        memset(&bank, 0, sizeof(bank));
        bank.instruments.entries[0].layer_count = 1;
        TEST(bank.instruments.allocate() == 0);
        for (uint32_t i = 0; i < Synth::max_lfos; i++) {
            TEST(bank.lfos.allocate() == i);
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
    // A preflight that fails validation leaves the caller's plan byte-identical. The commit
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

        Synth::InstrumentBank bank = {};
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
                // A spelling beyond the finite float range is refused, not clamped. The
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
        // so the encoded document needs far more than the 4096-token pool
        const uint32_t env                                  = bank.bank.envelopes.allocate();
        bank.bank.envelopes.entries[env].num_points         = 2;
        bank.bank.envelopes.entries[env].points[1].position = 100;
        const uint32_t lfo                                  = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[lfo].period_ms               = 50;

        for (uint32_t i = 0; i < 4; i++) {
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
    // full graph (84 nodes, 210 modulation edges + 7 hard connections) and
    // compiles back bit-identically, with the descriptor pools untouched (no
    // dedup or duplication) and the aliasing pattern preserved.
    {
        static Synth::InstrumentBank bank;
        static Synth::InstrumentBank bank_image;
        Synth::Instrument            instrument;
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        bank_image = bank;

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);

        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 35, 35);
        TEST(graph.connection_count() == Synth::max_layers * 5 * 6 + Synth::max_layers);

        // Re-projection onto a populated graph resets it deterministically.
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 35, 35);
        TEST(graph.connection_count() == Synth::max_layers * 5 * 6 + Synth::max_layers);

        static Synth::Instrument compiled;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);

        // The aliased LFO keeps one shared desc id on both bindings.
        TEST(mapping.lfo_desc_ids[1][Synth::mod_pitch] == lfo_ids[1][Synth::mod_pitch]);
        TEST(mapping.lfo_desc_ids[4][Synth::mod_volume] == lfo_ids[1][Synth::mod_pitch]);
        TEST(compiled.layers[1].gen[Synth::mod_pitch].lfo_desc_id == lfo_ids[1][Synth::mod_pitch]);
        TEST(compiled.layers[4].gen[Synth::mod_volume].lfo_desc_id == lfo_ids[1][Synth::mod_pitch]);
    }

    // Minimal round-trip: one layer, no bindings, projects to the fixed nodes
    // plus one oscillator and compiles back bit-identically.
    {
        static Synth::InstrumentBank bank;
        Synth::Instrument            instrument          = {};
        instrument.layer_count                           = 1;
        instrument.layers[0].osc_type[0]                 = Synth::WaveType::sawtooth_wave;
        instrument.layers[0].pitch_offset                = -7.0f;
        instrument.routing[Synth::mod_volume].base_value = 0.5f;
        instrument.note_skew_semitones                   = 0.2f;

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        check_osc_graph_mapping(graph, mapping, 1, 0, 0);
        TEST(graph.connection_count() == 1); // one hard connection, no modulation edges

        static Synth::Instrument compiled;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
    }

    // Grammar validator: the allowed edge set is exactly Envelope -> target,
    // LFO -> target, input -> target direct input, input -> LFO depth/rate
    // sources, and the structural oscillator -> Output hard connections.  A
    // repeated allowed pair stays allowed: duplicate inputs are first-class
    // and order-significant, so there is no duplicate-source rule.
    {
        static Synth::InstrumentBank bank;
        Synth::Instrument            instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id);

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        check_osc_graph_mapping(graph, mapping, 1, 2, 1);
        TEST(graph.connection_count() == 5); // 1 direct + 2 envelope + 1 LFO + 1 hard

        const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
        const Sculptor::EndPoint input_out    = { mapping.input_nodes[velocity_idx], mapping.input_output_slot };
        const Sculptor::EndPoint osc_out      = { mapping.osc_nodes[0], mapping.osc_output_slot };
        const Sculptor::EndPoint env_out      = { mapping.env_nodes[0][Synth::mod_volume], mapping.env_output_slot };
        const Sculptor::EndPoint lfo_out      = { mapping.lfo_nodes[0][Synth::mod_volume], mapping.lfo_output_slot };
        const Sculptor::EndPoint direct_in    = { mapping.osc_nodes[0],
                                                  mapping.osc_direct_input_slot[Synth::mod_pitch][0] };
        const Sculptor::EndPoint direct_in_1  = { mapping.osc_nodes[0],
                                                  mapping.osc_direct_input_slot[Synth::mod_pitch][1] };
        const Sculptor::EndPoint env_in       = { mapping.osc_nodes[0], mapping.osc_env_input_slot[Synth::mod_pitch] };
        const Sculptor::EndPoint lfo_in       = { mapping.osc_nodes[0], mapping.osc_lfo_input_slot[Synth::mod_pitch] };
        const Sculptor::EndPoint depth_in = { mapping.lfo_nodes[0][Synth::mod_volume], mapping.lfo_depth_input_slot };
        const Sculptor::EndPoint rate_in  = { mapping.lfo_nodes[0][Synth::mod_volume], mapping.lfo_rate_input_slot };
        const Sculptor::EndPoint sum_in   = { mapping.output_node, mapping.output_layer_input_slot[0] };

        TEST(osc_graph_validate(&mapping, graph, env_out, env_in));
        TEST(osc_graph_validate(&mapping, graph, lfo_out, lfo_in));
        TEST(osc_graph_validate(&mapping, graph, input_out, direct_in));
        TEST(osc_graph_validate(&mapping, graph, input_out, direct_in_1));
        TEST(osc_graph_validate(&mapping, graph, input_out, depth_in));
        TEST(osc_graph_validate(&mapping, graph, input_out, rate_in));
        TEST(osc_graph_validate(&mapping, graph, osc_out, sum_in));

        // The same source -> same target pair validates twice: no duplicate rule.
        TEST(osc_graph_validate(&mapping, graph, input_out, direct_in));
        TEST(osc_graph_validate(&mapping, graph, input_out, direct_in));

        // Everything outside the allowed set is refused.
        TEST(! osc_graph_validate(&mapping, graph, input_out, env_out)); // input -> Envelope
        TEST(! osc_graph_validate(&mapping, graph, env_out, depth_in));  // envelopes have no input side
        TEST(! osc_graph_validate(&mapping, graph, env_out, direct_in)); // envelopes drive only targets
        TEST(! osc_graph_validate(&mapping, graph, lfo_out, depth_in));  // LFOs drive only targets
        TEST(! osc_graph_validate(&mapping, graph, osc_out, env_in));    // oscillators feed only the sum
        TEST(! osc_graph_validate(&mapping, graph, input_out, sum_in));  // inputs never reach the sum directly
        TEST(! osc_graph_validate(&mapping, graph, env_out, sum_in));

        // Installed as the widget validator: a grammar refusal reports through
        // the error overlay, a grammatically valid pair connects.
        graph.set_validator(Sculptor::osc_graph_validate, &mapping);
        TEST(! graph.attempt_connection(env_out, depth_in));
        TEST(graph.has_error());
        graph.dismiss_error();
        TEST(graph.attempt_connection(input_out, depth_in));
    }

    // Descriptor-content property edits write through to the bank pool entry
    // named by the node's desc_id; nodes sharing a desc id edit the same entry,
    // so aliasing stays consistent and repeated edits are idempotent.
    {
        static Synth::InstrumentBank bank;
        Synth::Instrument            instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id);

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);

        const uint32_t env_node   = mapping.env_nodes[0][Synth::mod_volume];
        const uint32_t alias_node = mapping.env_nodes[0][Synth::mod_pitch];
        const uint32_t lfo_node   = mapping.lfo_nodes[0][Synth::mod_volume];
        uint32_t       slot       = 0;

        TEST(find_graph_slot(graph, env_node, "min_value", &slot));
        Sculptor::PropertyValue edited = {};
        edited.real                    = 0.125f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot, edited));
        TEST(bank.envelopes.entries[shared_env_id - 1].min_value == 0.125f);

        const Synth::EnvelopeDescriptor before = bank.envelopes.entries[shared_env_id - 1];
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, env_node, slot, edited));
        TEST(memcmp(&before, &bank.envelopes.entries[shared_env_id - 1], sizeof(Synth::EnvelopeDescriptor)) == 0);

        // The aliased sibling node edits the same pool entry.
        TEST(find_graph_slot(graph, alias_node, "min_max_delta", &slot));
        edited.real = 2.5f;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, alias_node, slot, edited));
        TEST(bank.envelopes.entries[shared_env_id - 1].min_max_delta == 2.5f);
        TEST(bank.envelopes.entries[shared_env_id - 1].min_value == 0.125f);

        TEST(find_graph_slot(graph, lfo_node, "period_ms", &slot));
        edited.integer = 400;
        TEST(Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, lfo_node, slot, edited));
        TEST(bank.lfos.entries[lfo_id - 1].period_ms == 400);

        // A slot that is not a descriptor-content property is refused with the
        // bank unmodified.
        const Synth::EnvelopeDescriptor untouched = bank.envelopes.entries[shared_env_id - 1];
        TEST(! Sculptor::apply_osc_graph_descriptor_edit(graph, mapping, &bank, mapping.osc_nodes[0], 0, edited));
        TEST(memcmp(&untouched, &bank.envelopes.entries[shared_env_id - 1], sizeof(Synth::EnvelopeDescriptor)) == 0);
    }

    // Projection refuses instruments it cannot express: a routing input with
    // the none source would change synthesis on commit if silently dropped
    // (the eval multiplies by the zero sentinel), so the refusal is visible
    // and leaves the graph, mapping and bank untouched.
    {
        static Synth::InstrumentBank bank;
        static Synth::InstrumentBank bank_image;
        Synth::Instrument            good          = {};
        good.layer_count                           = 1;
        good.routing[Synth::mod_volume].num_inputs = 1;
        good.routing[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity, Synth::SourceOp::multiply, 0.5f };

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        TEST(Sculptor::project_instrument_to_graph(good, bank, &graph, &mapping));
        TEST(mapping.output_node != Sculptor::pool_no_slot);

        Synth::Instrument bad                           = good;
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

    // Compile resolves envelope/LFO bindings from the graph edges entering
    // each target, not from the projection-time mapping: deleting an edge
    // unbinds the target, retargeting an edge moves the binding, and an LFO
    // node with no depth edge compiles to lfo_depth_source none.
    {
        static Synth::InstrumentBank bank;
        Synth::Instrument            instrument    = {};
        uint16_t                     shared_env_id = 0;
        uint16_t                     lfo_id        = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id);

        static Sculptor::Graph           graph;
        static Sculptor::OscGraphMapping mapping;
        static Synth::Instrument         compiled;

        // (a) Deleting the volume envelope edge unbinds volume only.
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        Synth::Instrument expected                                 = instrument;
        expected.layers[0].gen[Synth::mod_volume].envelope_desc_id = 0;
        const uint32_t env_conn =
            find_osc_graph_connection(graph, { mapping.osc_nodes[0], mapping.osc_env_input_slot[Synth::mod_volume] });
        TEST(env_conn != Sculptor::pool_no_slot);
        graph.delete_connection(env_conn);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);

        // (b) Retargeting the volume envelope to pitch moves the binding: pitch
        // bound, volume unbound, bit-identical to a model built that way.
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        expected                                                   = instrument;
        expected.layers[0].gen[Synth::mod_volume].envelope_desc_id = 0;
        const uint32_t pitch_env_conn =
            find_osc_graph_connection(graph, { mapping.osc_nodes[0], mapping.osc_env_input_slot[Synth::mod_pitch] });
        TEST(pitch_env_conn != Sculptor::pool_no_slot);
        graph.delete_connection(pitch_env_conn);
        const uint32_t volume_env_conn =
            find_osc_graph_connection(graph, { mapping.osc_nodes[0], mapping.osc_env_input_slot[Synth::mod_volume] });
        TEST(volume_env_conn != Sculptor::pool_no_slot);
        TEST(graph.move_connection_end(volume_env_conn,
                                       false,
                                       { mapping.osc_nodes[0], mapping.osc_env_input_slot[Synth::mod_pitch] }));
        expected.layers[0].gen[Synth::mod_pitch].envelope_desc_id = shared_env_id;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);

        // (c) An LFO node with no depth edge yields lfo_depth_source none.
        instrument.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::channel_pressure;
        Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping);
        expected                                                   = instrument;
        expected.layers[0].gen[Synth::mod_volume].lfo_depth_source = Synth::ModSource::none;
        const uint32_t depth_conn =
            find_osc_graph_connection(graph, { mapping.lfo_nodes[0][Synth::mod_volume], mapping.lfo_depth_input_slot });
        TEST(depth_conn != Sculptor::pool_no_slot);
        graph.delete_connection(depth_conn);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&expected, &compiled, sizeof(Synth::Instrument)) == 0);
    }
    return exit_code;
}
