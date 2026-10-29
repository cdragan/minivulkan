// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

// Builds a small valid bank: one envelope, one LFO, one instrument, one zone.
static void build_small_bank_fixture(Synth::InstrumentBank& bank)
{
    memset(static_cast<void*>(&bank), 0, sizeof(bank));
    Synth::init_default_bank(&bank);
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
    bank.channel_enabled[1]  = 1;
}

// Full-wiring fixture for the maximum-population projection check: every
// (layer, target) cell binds its own envelope and LFO descriptors plus two
// routed inputs, so the projection hits the exact node, instance and
// connection budgets.  Deliberately separate from
// build_osc_graph_max_fixture, whose partial wiring (four targets, layer-0-
// only inputs, one aliased LFO) other tests depend on.
static void build_fir_slope_max_fixture(Synth::InstrumentBank* bank,
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
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        Synth::Oscillator& osc = instrument->layers[layer];
        osc.osc_type[0]        = static_cast<Synth::WaveType>(1 + (layer % 4));
        osc.osc_type[1]        = static_cast<Synth::WaveType>(1 + ((layer + 2) % 4));
        osc.osc_mode           = static_cast<Synth::OscMode>(layer % 3);
        osc.mod_ratio          = 1.0f + 0.25f * static_cast<float>(layer);
        // The max population projects every band block, so the slot,
        // instance and connection budgets stay at their maxima.
        osc.fir_band_count = 4;
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            const uint32_t env_slot = bank->envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            env_ids[layer][t]              = static_cast<uint16_t>(env_slot + 1);
            Synth::EnvelopeDescriptor& env = bank->envelopes.entries[env_slot];
            env.num_points                 = 2;
            env.sustain_first_point        = 0;
            env.sustain_last_point         = 1;
            env.min_value                  = -1.0f;
            env.min_max_delta              = 2.0f;
            env.points[0]                  = { 0, 0x4000 };
            env.points[1]                  = { 300, 0x8000 };

            const uint32_t lfo_slot = bank->lfos.allocate();
            TEST(lfo_slot != pool_no_slot);
            lfo_ids[layer][t]         = static_cast<uint16_t>(lfo_slot + 1);
            Synth::LFODescriptor& lfo = bank->lfos.entries[lfo_slot];
            lfo.wave                  = Synth::WaveType::sine_wave;
            lfo.duty                  = 0x40;
            lfo.period_ms             = static_cast<uint16_t>(100 + 37 * lfo_slot);
            lfo.min_value             = -1.0f;
            lfo.min_max_delta         = 2.0f;

            Synth::TargetBinding& gen = osc.targets[t];
            gen.envelope_desc_id      = env_ids[layer][t];
            gen.lfo_desc_id           = lfo_ids[layer][t];
            gen.lfo_op                = ((layer + t) % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
            gen.lfo_depth             = 0.25f * static_cast<float>(layer + 1);
            gen.lfo_depth_source      = static_cast<Synth::ModSource>(1 + ((layer + t) % 6));
            gen.lfo_rate_source       = static_cast<Synth::ModSource>(1 + ((layer + 2 * t + 1) % 6));
            gen.lfo_rate_scale_ms     = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(t);
            gen.base_value            = -1.0f + 0.25f * static_cast<float>(t);
            gen.num_inputs            = 2;
            gen.inputs[0].source      = Synth::ModSource::pitch_bend;
            gen.inputs[0].op          = Synth::SourceOp::add;
            gen.inputs[0].scale       = 0.5f * static_cast<float>(t);
            gen.inputs[1].source      = Synth::ModSource::mod_wheel;
            gen.inputs[1].op          = Synth::SourceOp::multiply;
            gen.inputs[1].scale       = -0.25f * static_cast<float>(t) - 0.5f;
        }
        // The compile syncs each layer's mod_ratio from its fm_ratio base;
        // the fixture mirrors that so the projection reads consistent rows.
        instrument->layers[layer].targets[Synth::mod_fm_ratio].base_value = instrument->layers[layer].mod_ratio;
    }
}

// The FIR Slope row is a connectable target row with a 12-slot
// block at 206..217, no existing row shifts, the disabled
// predicate covers the widened range exactly on filter-off layers, and the
// maximum-population projection hits the exact node/instance/connection
// budgets.
static void osc_fir_slope_projection_regressions()
{
    // Three-layer fixture: layer 0 band-pass with a spread base, a routed
    // slope base and a detached envelope; layer 1 filter-off with the scalar
    // slope; layer 2 band-pass with a routed slope input.
    const std::unique_ptr<Synth::InstrumentBank> model_bank_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       model_bank         = *model_bank_storage;
    Synth::Instrument                            instrument         = {};
    const std::unique_ptr<Sculptor::Graph>       graph_storage      = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                             graph              = *graph_storage;
    Sculptor::OscGraphMapping                    mapping            = {};
    Sculptor::GraphChange                        discarded[Sculptor::max_pending_changes] = {};
    memset(static_cast<void*>(&model_bank), 0, sizeof(model_bank));
    instrument = {};
    TEST(model_bank.envelopes.allocate() == 0);
    Synth::EnvelopeDescriptor& env = model_bank.envelopes.entries[0];
    env.num_points                 = 2;
    env.sustain_first_point        = 0;
    env.sustain_last_point         = 1;
    env.min_value                  = -0.5f;
    env.min_max_delta              = 1.5f;
    env.points[0]                  = { 0, 0x4000 };
    env.points[1]                  = { 200, 0x8000 };

    instrument.layer_count = 3;
    set_default_dispersion_sections(instrument);
    instrument.layers[0].osc_type[0]                                    = Synth::WaveType::sine_wave;
    instrument.layers[0].osc_type[1]                                    = Synth::WaveType::pulse_wave;
    instrument.layers[0].osc_mode                                       = Synth::osc_mode_blend;
    instrument.layers[0].mod_ratio                                      = 2.0f;
    instrument.layers[0].fir_slope_percent                              = 37;
    instrument.layers[0].fir_type                                       = Synth::fir_bandpass;
    instrument.layers[0].targets[Synth::mod_fir_spread].base_value      = 12.0f;
    instrument.layers[0].targets[Synth::mod_fir_slope].base_value       = 64.0f;
    instrument.layers[0].targets[Synth::mod_pitch].envelope_desc_id     = 1; // projects the detached env node
    instrument.layers[1].osc_type[0]                                    = Synth::WaveType::sine_wave;
    instrument.layers[1].osc_type[1]                                    = Synth::WaveType::pulse_wave;
    instrument.layers[1].osc_mode                                       = Synth::osc_mode_blend;
    instrument.layers[1].mod_ratio                                      = 2.0f;
    instrument.layers[1].fir_slope_percent                              = 37;
    instrument.layers[1].fir_type                                       = Synth::fir_off;
    instrument.layers[2].osc_type[0]                                    = Synth::WaveType::sine_wave;
    instrument.layers[2].osc_type[1]                                    = Synth::WaveType::pulse_wave;
    instrument.layers[2].osc_mode                                       = Synth::osc_mode_blend;
    instrument.layers[2].mod_ratio                                      = 2.0f;
    instrument.layers[2].fir_type                                       = Synth::fir_bandpass;
    instrument.layers[2].targets[Synth::mod_fir_spread].base_value      = 3.0f;
    instrument.layers[2].targets[Synth::mod_fir_slope].num_inputs       = 1;
    instrument.layers[2].targets[Synth::mod_fir_slope].inputs[0].source = Synth::ModSource::velocity;
    instrument.layers[2].targets[Synth::mod_fir_slope].inputs[0].op     = Synth::SourceOp::add;
    instrument.layers[2].targets[Synth::mod_fir_slope].inputs[0].scale  = 1.0f;
    for (uint32_t layer = 0; layer < instrument.layer_count; ++layer) {
        instrument.layers[layer].targets[Synth::mod_fm_ratio].base_value = instrument.layers[layer].mod_ratio;
    }

    TEST(Sculptor::project_instrument_to_graph(instrument, model_bank, &graph, &mapping));
    (void)graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    const Sculptor::Node& osc = graph.node(mapping.osc_nodes[0]);

    // The blocks are appended: 35 fixed rows plus Filter Type and one
    // band block per existing band (the fixture keeps one), pinned against
    // the literal 205.
    const uint32_t expected_slots =
        Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride * instrument.layers[0].fir_band_count;
    TEST(osc.slots.num_allocated == expected_slots);
    TEST(expected_slots == 205);

    // Slot 192 is the connectable FIR Slope row.
    TEST(Sculptor::osc_target_row_slot[Synth::mod_fir_slope] == 192);
    TEST(Sculptor::osc_target_row(Synth::mod_fir_slope) == 192);
    {
        const Sculptor::Slot& row = osc.slots.entries[192];
        TEST(row.connectable);
        TEST(row.drop_menu);
        TEST(strncmp(row.name, "FIR Slope", sizeof(row.name)) == 0);
        TEST(row.property_type == Sculptor::PropertyType::integer);
    }
    // The block follows the row: the env dot at 193 and the source dots at
    // 199/202, all twelve slots allocated.
    TEST(Sculptor::osc_target_env_dot(Synth::mod_fir_slope) == 193);
    TEST(Sculptor::osc_target_src_dot(Synth::mod_fir_slope, 0) == 199);
    TEST(Sculptor::osc_target_src_dot(Synth::mod_fir_slope, 1) == 202);
    {
        const Sculptor::Slot& env_dot = osc.slots.entries[193];
        TEST(env_dot.kind == Sculptor::SlotKind::input);
        TEST(strncmp(env_dot.name, "Envelope", sizeof(env_dot.name)) == 0);
        const Sculptor::Slot& src_a = osc.slots.entries[199];
        TEST(src_a.kind == Sculptor::SlotKind::input);
        TEST(strncmp(src_a.name, "Source A Op", sizeof(src_a.name)) == 0);
        const Sculptor::Slot& src_b = osc.slots.entries[202];
        TEST(src_b.kind == Sculptor::SlotKind::input);
        TEST(strncmp(src_b.name, "Source B Op", sizeof(src_b.name)) == 0);
    }
    // No existing row shifted.
    TEST(Sculptor::osc_target_row_slot[Synth::mod_volume] == 109);
    TEST(Sculptor::osc_target_row_slot[Synth::mod_fir_spread] == 166);
    // Literal budget pins: a future drift cannot pass silently.
    TEST(Sculptor::max_nodes == 416);
    TEST(Sculptor::max_node_slots == 370);

    // Disabled state matches the filter-off predicate: the fir-off layer
    // grays the whole 166..204 range; the band-pass layers keep every slot
    // of the range active.
    for (uint32_t s = 166; s <= 204; s++) {
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(mapping.osc_nodes[1]), s));
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, s));
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(mapping.osc_nodes[2]), s));
    }

    // Display: a routed base wins over the scalar; a wired base of zero
    // clamps to 1 at the display boundary (the integer percent domain); a
    // zeroed binding shows the scalar, even with the filter off.
    TEST(osc.slots.entries[192].value.integer == 64);
    TEST(graph.node(mapping.osc_nodes[2]).slots.entries[192].value.integer == 1);
    TEST(graph.node(mapping.osc_nodes[1]).slots.entries[192].value.integer == 37);

    // An envelope wired into the slope block's env dot compiles onto the
    // slope binding; the wired row is the binding's base, so the routed
    // base survives compilation and a wired base of zero clamps to 1.
    const int32_t env_inst = find_detached_by_desc(mapping, 1, 1);
    TEST(env_inst >= 0);
    if (env_inst >= 0) {
        const Sculptor::EndPoint env_out       = { mapping.detached[env_inst].node_idx, mapping.env_output_slot };
        const Sculptor::EndPoint slope_env_dot = { mapping.osc_nodes[0],
                                                   Sculptor::osc_target_env_dot(Synth::mod_fir_slope) };
        TEST(Sculptor::osc_graph_validate(&mapping, graph, env_out, slope_env_dot));
        TEST(graph.add_connection(env_out, slope_env_dot) != Sculptor::pool_no_slot);
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].targets[Synth::mod_fir_slope].envelope_desc_id == 1);
        TEST(compiled.layers[0].targets[Synth::mod_fir_slope].base_value == 64.0f);
        TEST(compiled.layers[2].targets[Synth::mod_fir_slope].base_value == 1.0f);
    }

    // Maximum population: seven layers with every target block fully wired
    // hit the exact node, instance and connection budgets (equality, so an
    // off-by-one in any budget assert fails here first).
    {
        const std::unique_ptr<Synth::InstrumentBank> max_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       max_bank         = *max_bank_storage;
        Synth::Instrument                            max_instrument   = {};
        uint16_t                                     env_ids[Synth::max_layers][Synth::num_mod_targets] = {};
        uint16_t                                     lfo_ids[Synth::max_layers][Synth::num_mod_targets] = {};
        const std::unique_ptr<Sculptor::Graph>       max_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                             max_graph         = *max_graph_storage;
        Sculptor::OscGraphMapping                    max_mapping       = {};
        build_fir_slope_max_fixture(&max_bank, &max_instrument, env_ids, lfo_ids);
        TEST(Sculptor::project_instrument_to_graph(max_instrument, max_bank, &max_graph, &max_mapping));
        check_osc_graph_mapping(max_graph,
                                max_mapping,
                                Synth::max_layers,
                                Synth::max_layers * Synth::num_mod_targets,
                                Synth::max_layers * Synth::num_mod_targets,
                                4);
        TEST(max_mapping.detached_count == 2 * Synth::max_layers * Synth::num_mod_targets);
        TEST(max_mapping.detached_count == 406);
        TEST(max_mapping.detached_count <= Sculptor::max_osc_instances);
        TEST(max_graph.connection_count() == 1141);
        TEST(max_graph.connection_count() <= Sculptor::max_connections);
    }
}

// FIR Slope helpers: the effective-span law, the full-span byte identity
// against the old fixed-window spelling, and the narrow-span zero tail.
static void check_fir_slope_helpers()
{
    // Law table: the odd span grows exponentially from 11 taps at 1 percent to the
    // full 1025-tap kernel at 100 (11 * (1025 / 11) ^ ((slope - 1) / 99)).
    {
        static const struct {
            float    percent;
            uint32_t span;
        } law[] = {
            { 1.0f, 11 },
            { 10.0f, 17 },
            { 25.0f, 33 },
            { 50.0f, 103 },
            { 75.0f, 325 },
            { 99.0f, 979 },
            { 100.0f, Synth::num_fir_taps },
        };

        for (const auto& row : law) {
            TEST(Synth::fir_effective_span(row.percent) == row.span);
        }
    }

    // Monotonic non-decreasing over the integer sweep, every result odd.
    {
        uint32_t previous = 0;
        for (uint32_t percent = 1; percent <= 100; percent++) {
            const uint32_t span = Synth::fir_effective_span(static_cast<float>(percent));
            TEST(span >= previous);
            TEST((span & 1u) == 1u);
            previous = span;
        }
        TEST(previous == Synth::num_fir_taps);
        // A fractional grid stays monotonic and odd.
        previous = 0;
        for (uint32_t step = 1; step < 200; step++) {
            const uint32_t span = Synth::fir_effective_span(static_cast<float>(step) * 0.5f);
            TEST(span >= previous);
            TEST((span & 1u) == 1u);
            previous = span;
        }
    }

    // Out-of-range clamps: 0, negatives and NaN read as the 1 percent
    // minimum; values above 100 (including +inf) read as the full span.
    {
        TEST(Synth::fir_effective_span(0.0f) == 11);
        TEST(Synth::fir_effective_span(-3.0f) == 11);
        TEST(Synth::fir_effective_span(NAN) == 11);
        TEST(Synth::fir_effective_span(101.0f) == Synth::num_fir_taps);
        TEST(Synth::fir_effective_span(INFINITY) == Synth::num_fir_taps);
    }

    // Byte-identity mirror: at the full span the span-parameterized window
    // reproduces the old fixed (num_taps - 1) spelling bit for bit, with
    // the same float association order.
    {
        constexpr uint32_t taps              = Synth::num_fir_taps;
        constexpr uint32_t center            = taps / 2;
        constexpr float    pi                = 3.141592653589793f;
        float              old_windows[taps] = {};
        float              new_windows[taps] = {};
        for (uint32_t tap = 0; tap < taps; tap++) {
            const float x         = static_cast<float>(tap) - static_cast<float>(center);
            old_windows[tap]      = 0.42f -
                                    0.5f * cosf((2.0f * pi * static_cast<float>(tap)) / static_cast<float>(taps - 1)) +
                                    0.08f * cosf((4.0f * pi * static_cast<float>(tap)) / static_cast<float>(taps - 1));
            const float half_span = static_cast<float>(taps) * 0.5f - 0.5f;
            const float wpos      = x + half_span;
            const float span_m1   = static_cast<float>(taps - 1);
            const bool  in_span   = wpos >= 0.0f && wpos <= span_m1;
            new_windows[tap] =
                in_span ? 0.42f - 0.5f * cosf((2.0f * pi * wpos) / span_m1) + 0.08f * cosf((4.0f * pi * wpos) / span_m1)
                        : 0.0f;
        }
        TEST(memcmp(old_windows, new_windows, sizeof(old_windows)) == 0);
    }

    // Narrow-span mirror at 103 taps (the span of FIR Slope 50 under the exponential law): taps outside the span are
    // exactly zero, in-span taps match an independent direct Blackman evaluation of a standalone 103-tap window, and
    // the in-span window sum stays positive.  The sum is only a window-sum sanity check; it says nothing about the
    // shader's normalization denominator.
    {
        constexpr uint32_t taps = Synth::num_fir_taps;
        constexpr uint32_t span = 103;
        // The kernel centers at (span - 1) / 2, so the direct window
        // starts at index center - span / 2 == 0.
        constexpr uint32_t center                       = (span - 1) / 2;
        constexpr float    pi                           = 3.141592653589793f;
        const float        half_span                    = static_cast<float>(span) * 0.5f - 0.5f;
        const float        span_m1                      = static_cast<float>(span - 1);
        float              windows[Synth::num_fir_taps] = {};
        float              window_sum                   = 0.0f;
        for (uint32_t tap = 0; tap < taps; tap++) {
            const float x       = static_cast<float>(tap) - static_cast<float>(center);
            const float wpos    = x + half_span;
            const bool  in_span = wpos >= 0.0f && wpos <= span_m1;
            const float window =
                in_span ? 0.42f - 0.5f * cosf((2.0f * pi * wpos) / span_m1) + 0.08f * cosf((4.0f * pi * wpos) / span_m1)
                        : 0.0f;
            windows[tap] = window;
            TEST(in_span || window == 0.0f);
            if (in_span) {
                TEST(window != 0.0f);
                window_sum += window;
            }
        }
        // Independent evaluation: the canonical Blackman over a standalone
        // 103-tap window, compared tap for tap against the span-derived form.
        for (uint32_t k = 0; k < span; k++) {
            const float direct = 0.42f - 0.5f * cosf((2.0f * pi * static_cast<float>(k)) / span_m1) +
                                 0.08f * cosf((4.0f * pi * static_cast<float>(k)) / span_m1);
            TEST(windows[center - span / 2 + k] == direct);
        }
        TEST(window_sum > 0.0f);
    }
}

// The FIR slope binding-presence rule, the shared seed
// value, and the propagation-level proof that the note-on seed lives in the
// plain descriptor base (a value-only seed would be clobbered by the first
// propagate_parameters step).
static void osc_fir_slope_seed_regressions()
{
    // Zeroed binding: no base, no inputs, no ids -> the scalar carries.
    {
        Synth::TargetBinding zeroed = {};
        TEST(! Synth::osc_fir_slope_binding_present(zeroed));
        TEST(Synth::osc_fir_slope_seed_value(zeroed, 37) == 37.0f);
    }
    // Present binding with a zero base: the routed base wins, never the
    // scalar; the display and compile boundaries clamp it into 1..100.
    {
        Synth::TargetBinding routed_zero = {};
        routed_zero.lfo_desc_id          = 1;
        TEST(Synth::osc_fir_slope_binding_present(routed_zero));
        TEST(Synth::osc_fir_slope_seed_value(routed_zero, 37) == 0.0f);
    }
    // A routed base overrides the scalar.
    {
        Synth::TargetBinding based = {};
        based.base_value           = 50.0f;
        TEST(Synth::osc_fir_slope_binding_present(based));
        TEST(Synth::osc_fir_slope_seed_value(based, 100) == 50.0f);
    }
    // An out-of-domain routed base is invalid state (both JSON decoders
    // refuse it); the helper passes it through untouched and the runtime
    // span-side clamp is fir_effective_span's job (already tested).
    {
        Synth::TargetBinding negative = {};
        negative.base_value           = -3.0f;
        TEST(Synth::osc_fir_slope_seed_value(negative, 37) == -3.0f);
    }
    // Each presence carrier alone flips the predicate; depth/op/scale
    // fields without a bound id or input do not.
    {
        Synth::TargetBinding carrier = {};
        carrier.base_value           = 1.0f;
        TEST(Synth::osc_fir_slope_binding_present(carrier));
        carrier            = {};
        carrier.num_inputs = 1;
        TEST(Synth::osc_fir_slope_binding_present(carrier));
        carrier                  = {};
        carrier.envelope_desc_id = 1;
        TEST(Synth::osc_fir_slope_binding_present(carrier));
        carrier             = {};
        carrier.lfo_desc_id = 1;
        TEST(Synth::osc_fir_slope_binding_present(carrier));
        carrier                   = {};
        carrier.lfo_depth         = 0.5f;
        carrier.lfo_op            = Synth::SourceOp::multiply;
        carrier.lfo_rate_scale_ms = 10.0f;
        TEST(! Synth::osc_fir_slope_binding_present(carrier));
        TEST(Synth::osc_fir_slope_seed_value(carrier, 42) == 42.0f);
    }
    // The seed enters the plain descriptor base (base_value +
    // base_extra at the configure_plain call), so every propagate step
    // restarts from it and the legacy scalar survives indefinitely.
    {
        Synth::ParamDescriptor descs[3]  = {}; // 0 sentinel, 1 slope dest
        Synth::Parameter       params[3] = {};
        Synth::TargetBinding   zeroed    = {};
        const float            extra     = Synth::osc_fir_slope_binding_present(zeroed) ? 0.0f : 37.0f;
        TEST(extra == 37.0f);
        Synth::configure_plain(&descs[1], zeroed.base_value + extra, 0, 0.0f, 0, Synth::SourceOp::add, nullptr, 0);
        for (uint32_t step = 0; step < 3; step++) {
            Synth::propagate_parameters(params, descs, 3);
            TEST(approx(params[1].value, 37.0f, 1.0e-6f));
        }
        // Routed-zero companion: a present binding configures with zero
        // extra, so the routed base 0 stays 0 and the scalar is never
        // inherited.
        Synth::TargetBinding routed = {};
        routed.num_inputs           = 1;
        routed.inputs[0].source     = Synth::ModSource::velocity;
        routed.inputs[0].op         = Synth::SourceOp::add;
        routed.inputs[0].scale      = 1.0f;
        TEST(Synth::osc_fir_slope_binding_present(routed));
        TEST(Synth::osc_fir_slope_seed_value(routed, 37) == 0.0f);
        const Synth::SourceParam velocity_input = { 2, 1.0f, Synth::SourceOp::add };
        Synth::configure_plain(&descs[1],
                               routed.base_value + 0.0f,
                               0,
                               0.0f,
                               0,
                               Synth::SourceOp::add,
                               &velocity_input,
                               1);
        params[2].value = 0.0f; // the routed input's onset value
        for (uint32_t step = 0; step < 3; step++) {
            Synth::propagate_parameters(params, descs, 3);
            TEST(approx(params[1].value, 0.0f, 1.0e-6f));
        }
    }
}

// The pure FIR coefficient cache decision.  Entries are 18-word
// records (taps_offs, band_count, then four words per band); a
// prev_entry_count of 0 means "no valid snapshot".
static void fir_coeff_cache_action_regressions()
{
    static uint32_t prev[Synth::fir_coeff_entry_words * 2] = {};
    static uint32_t cur[Synth::fir_coeff_entry_words * 2]  = {};
    prev[0]                                                = 1000;
    prev[1]                                                = 220;
    prev[2]                                                = 880;
    prev[3]                                                = 513;
    prev[4]                                                = 2000;
    prev[5]                                                = 330;
    prev[6]                                                = 990;
    prev[7]                                                = 257;
    memcpy(cur, prev, sizeof(cur));

    // Unchanged consecutive snapshots skip the dispatch.
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 2, false) == Synth::fir_coeff_skip);
    // A changed cutoff word dispatches.
    cur[2] = 900;
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 2, false) == Synth::fir_coeff_dispatch);
    memcpy(cur, prev, sizeof(cur));
    // A changed span word dispatches.
    cur[3] = 103;
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 2, false) == Synth::fir_coeff_dispatch);
    memcpy(cur, prev, sizeof(cur));
    // A changed taps offset (active-set membership) dispatches.
    cur[4] = 2100;
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 2, false) == Synth::fir_coeff_dispatch);
    memcpy(cur, prev, sizeof(cur));
    // A count change in either direction dispatches.
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 1, false) == Synth::fir_coeff_dispatch);
    TEST(Synth::fir_coeff_cache_action(prev, 1, cur, 2, false) == Synth::fir_coeff_dispatch);
    // Zero active filters invalidate the snapshot; no dispatch happens at
    // the zero-active early return.
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 0, false) == Synth::fir_coeff_invalidate);
    // The zero-to-any edge: an invalid snapshot dispatches.
    TEST(Synth::fir_coeff_cache_action(prev, 0, cur, 2, false) == Synth::fir_coeff_dispatch);
    // Effect-reset invalidation: cleared taps dispatch even when the words
    // are identical.
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 2, true) == Synth::fir_coeff_dispatch);
    // Cleared taps with no active filters still only invalidate.
    TEST(Synth::fir_coeff_cache_action(prev, 2, cur, 0, true) == Synth::fir_coeff_invalidate);

    // The same decision over the production record layout: 18-word
    // coefficient structs (taps_offs, band_count, then four words per band:
    // highpass, lowpass, window_span, gain), compared bytewise (the runtime
    // passes FIRCoeff arrays, never a reinterpreted word array).
    struct CoeffRecord {
        uint32_t taps_offs;
        uint32_t band_count;

        struct Band {
            uint32_t highpass_cutoff_freq;
            uint32_t lowpass_cutoff_freq;
            uint32_t window_span;
            float    gain;
        };

        Band bands[4];
    };

    static_assert(sizeof(CoeffRecord) == Synth::fir_coeff_entry_words * sizeof(uint32_t));
    const CoeffRecord::Band base_band = { 220, 880, 513, 0 };
    CoeffRecord             rec_prev  = { 1000, 2, { { base_band }, { base_band }, { base_band }, { base_band } } };
    CoeffRecord             rec_cur   = rec_prev;
    TEST(Synth::fir_coeff_cache_action(&rec_prev, 1, &rec_cur, 1, false) == Synth::fir_coeff_skip);
    rec_cur.bands[0].window_span = 103;
    TEST(Synth::fir_coeff_cache_action(&rec_prev, 1, &rec_cur, 1, false) == Synth::fir_coeff_dispatch);
    rec_cur.bands[1]  = rec_prev.bands[1];
    rec_cur.bands[0]  = rec_prev.bands[0];
    rec_cur.taps_offs = 2100;
    TEST(Synth::fir_coeff_cache_action(&rec_prev, 1, &rec_cur, 1, false) == Synth::fir_coeff_dispatch);
}

// Host mirror of the FIR coefficient shader's binary32 arithmetic for one
// cutoff pair and span: the signed windowed kernel, the denominator terms and
// the stored coefficients.  The response sums are then reduced with the
// shader's summation shape and re-evaluated independently in double precision.
struct FirCoeffMirror {
    float coefficients[Synth::num_fir_taps];
    float norm_terms[Synth::num_fir_taps]; // norm_value * window, the coeff_sum path
    float cos_terms[Synth::num_fir_taps];
    float sin_terms[Synth::num_fir_taps];
    float old_denominator;  // coeff_sum: the unchanged normalization sum
    float band_denominator; // stored denominator: center magnitude for band-pass, coeff_sum otherwise
    float center_freq;
};

// Emulates the shader's summation shape: each invocation accumulates its
// strided taps in tap order, subgroupAdd sums each subgroup's lanes in lane
// order, and the subgroup-0 stage sums the partials in subgroup order.
static float fir_test_subgroup_sum(const float* terms, uint32_t work_group, uint32_t subgroup)
{
    constexpr uint32_t taps          = Synth::num_fir_taps;
    const uint32_t     num_subgroups = work_group / subgroup;
    float              total         = 0.0f;
    for (uint32_t s = 0; s < num_subgroups; s++) {
        float sub = 0.0f;
        for (uint32_t lane = s * subgroup; lane < (s + 1) * subgroup; lane++) {
            float lane_sum = 0.0f;
            for (uint32_t tap = lane; tap < taps; tap += work_group) {
                lane_sum += terms[tap];
            }
            sub += lane_sum;
        }
        total += sub;
    }
    return total;
}

// Mirror of the coefficient shader's band-pass denominator selection: a
// degenerate complex magnitude falls back to the coeff_sum path, and a
// degenerate fallback leaves the coefficients unscaled.
static float fir_test_bandpass_denominator(float cos_sum, float sin_sum, float fallback)
{
    const float q           = cos_sum * cos_sum + sin_sum * sin_sum;
    const float m           = sqrtf(q);
    float       denominator = m;
    if (m == 0.0f || std::isnan(cos_sum) || std::isnan(sin_sum) || std::isinf(cos_sum) || std::isinf(sin_sum) ||
        std::isnan(q) || std::isinf(q) || std::isnan(m)) {
        denominator = fallback;
    }
    if (denominator == 0.0f || std::isnan(denominator) || std::isinf(denominator)) {
        denominator = 1.0f;
    }
    return denominator;
}

// Host mirror of the coefficient shader for one integer cutoff pair and span.
static void fir_test_mirror(uint32_t        lowpass_hz,
                            uint32_t        highpass_hz,
                            uint32_t        span,
                            uint32_t        work_group,
                            uint32_t        subgroup,
                            FirCoeffMirror* out)
{
    constexpr uint32_t taps = Synth::num_fir_taps;
    // The mirrored shader centers the kernel at (window_span - 1) / 2, so the
    // non-zero support lands in tap indices [0, span - 1].
    const uint32_t  center = (span - 1) / 2;
    constexpr float pi     = 3.141592653589793f;
    const float     fs     = static_cast<float>(Synth::rt_sampling_rate);

    const float lowpass_norm  = static_cast<float>(lowpass_hz) / fs;
    const float highpass_norm = static_cast<float>(highpass_hz) / fs;
    out->center_freq          = (static_cast<float>(highpass_hz) + static_cast<float>(lowpass_hz)) * 0.5f;
    const float center_w      = 2.0f * pi * out->center_freq / fs;
    const float half_span     = static_cast<float>(span) * 0.5f - 0.5f;
    const float span_m1       = static_cast<float>(span - 1u);

    for (uint32_t tap = 0; tap < taps; tap++) {
        const float x = static_cast<float>(tap) - static_cast<float>(center);

        float value = (x == 0.0f && lowpass_hz == 0 && highpass_hz == 0) ? 1.0f : 0.0f;
        if (lowpass_hz > 0) {
            value = (x == 0.0f) ? 2.0f * lowpass_norm : sinf(2.0f * pi * lowpass_norm * x) / (pi * x);
        }

        float norm_value = value;

        if (highpass_hz > 0) {
            const float hp_value = (x == 0.0f) ? 2.0f * highpass_norm : sinf(2.0f * pi * highpass_norm * x) / (pi * x);
            value -= hp_value;
            if (highpass_hz < lowpass_hz) {
                value      = -value;
                norm_value = hp_value;
            }
            else if (x == 0.0f) {
                value += 1.0f;
                if (lowpass_hz == 0) {
                    norm_value += 1.0f;
                }
            }
        }

        const float wpos    = x + half_span;
        const bool  in_span = wpos >= 0.0f && wpos <= span_m1;
        const float window =
            in_span ? 0.42f - 0.5f * cosf((2.0f * pi * wpos) / span_m1) + 0.08f * cosf((4.0f * pi * wpos) / span_m1)
                    : 0.0f;
        value *= window;

        out->coefficients[tap] = value;
        out->norm_terms[tap]   = norm_value * window;
        if (in_span && highpass_hz > 0 && highpass_hz < lowpass_hz) {
            out->cos_terms[tap] = value * cosf(center_w * x);
            out->sin_terms[tap] = value * sinf(center_w * x);
        }
        else {
            out->cos_terms[tap] = 0.0f;
            out->sin_terms[tap] = 0.0f;
        }
    }

    out->old_denominator  = fir_test_subgroup_sum(out->norm_terms, work_group, subgroup);
    const float cos_sum   = fir_test_subgroup_sum(out->cos_terms, work_group, subgroup);
    const float sin_sum   = fir_test_subgroup_sum(out->sin_terms, work_group, subgroup);
    out->band_denominator = fir_test_bandpass_denominator(cos_sum, sin_sum, out->old_denominator);
}

// Independent double precision response magnitude of a binary32 kernel, with
// causal tap coordinates (phase taken from the tap index itself): the causal
// kernel differs from the centered one by a unit-magnitude phase factor only,
// so this validates the centered measurement's magnitude against the causal
// convolution the runtime applies.
static double fir_test_response(const float* coefficients, double center_freq)
{
    constexpr uint32_t taps = Synth::num_fir_taps;
    const double       w    = 2.0 * 3.141592653589793 * center_freq / static_cast<double>(Synth::rt_sampling_rate);
    double             real = 0.0;
    double             imag = 0.0;
    for (uint32_t tap = 0; tap < taps; tap++) {
        const double c = static_cast<double>(coefficients[tap]);
        if (c == 0.0) {
            continue;
        }
        const double phase = w * static_cast<double>(tap);
        real += c * cos(phase);
        imag -= c * sin(phase);
    }
    return sqrt(real * real + imag * imag);
}

// Stores the shader's per-tap binary32 division by the selected denominator
// (the band-center magnitude for band-pass, the unchanged coeff_sum path
// otherwise) and evaluates the stored outputs' response magnitude.
static double fir_test_stored_response(const FirCoeffMirror& mirror, double freq, float* stored)
{
    constexpr uint32_t taps = Synth::num_fir_taps;
    for (uint32_t tap = 0; tap < taps; tap++) {
        stored[tap] = mirror.coefficients[tap] / mirror.band_denominator;
        TEST(std::isfinite(stored[tap]));
    }
    return fir_test_response(stored, freq);
}

// Normalized center-response tolerance.  Near-Nyquist one-Hz pairs lose more
// precision to binary32 sinc arguments (about 1600 radians at the span edges),
// so they get the looser bound.
static double fir_test_unity_tolerance(uint32_t lowpass_hz, uint32_t highpass_hz)
{
    const double center = 0.5 * (static_cast<double>(lowpass_hz) + static_cast<double>(highpass_hz));
    const bool   near_nyquist_narrow =
        (0.5 * static_cast<double>(Synth::rt_sampling_rate) - center) < 100.0 && (lowpass_hz - highpass_hz) <= 2;
    return near_nyquist_narrow ? 1e-3 : 1e-4;
}

// Kernel-shape assertions over the coefficients fir_test_mirror already computed: with the kernel
// centered at (window_span - 1) / 2, the non-zero support, zero tail, and symmetry hold per span.
static void fir_test_check_span_shape(const FirCoeffMirror& mirror, uint32_t span)
{
    constexpr uint32_t taps        = Synth::num_fir_taps;
    const uint32_t     span_center = (span - 1) / 2;

    for (uint32_t tap = 0; tap < taps; tap++) {
        TEST(std::isfinite(mirror.coefficients[tap]));
        if (tap >= span) {
            TEST(mirror.coefficients[tap] == 0.0f);
        }
    }

    for (uint32_t d = 0; d <= span_center; d++) {
        const float left  = mirror.coefficients[span_center - d];
        const float right = mirror.coefficients[span_center + d];
        // Window-tap symmetry survives binary32 argument rounding only
        // within a last-ulp slack; a wrong center breaks it by orders of
        // magnitude.
        TEST(fabsf(left - right) <= 2e-6f * fmaxf(1.0f, fmaxf(fabsf(left), fabsf(right))));
    }
}

// Host cutoff quantizer mirror used to measure the stored FIR response.
// These tests exercise the mirror, not the production helper.
static uint32_t fir_test_quantize_cutoff(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value <= 1.0f) {
        return 1;
    }
    if (value >= static_cast<float>(Synth::max_cutoff_hz)) {
        return Synth::max_cutoff_hz;
    }
    return static_cast<uint32_t>(value + 0.5f);
}

// Band-pass gain: the coefficient shader normalizes the kernel's actual complex
// response magnitude to 1 at the arithmetic band center for every span,
// verified here by a binary32 host mirror with subgroup-shaped summation and
// an independent double precision response evaluation.
static void check_fir_bandpass_gain()
{
    constexpr uint32_t                    taps                        = Synth::num_fir_taps;
    const std::unique_ptr<FirCoeffMirror> mirror_storage              = std::make_unique<FirCoeffMirror>();
    FirCoeffMirror&                       mirror                      = *mirror_storage;
    float                                 stored[Synth::num_fir_taps] = {};

    // Spot pairs from the measured host response table: the pre-change center
    // gains are pinned to show the regression fails on the old denominator,
    // and the normalized response is unity in both subgroup shapes.  The old
    // gain references are double precision values; the binary32 mirror may
    // deviate from them by up to about a thousandth relative near Nyquist, so
    // the pins use a 2e-3 relative tolerance.
    constexpr float pi = 3.141592653589793f;
    {
        static const struct {
            uint32_t lowpass_hz;
            uint32_t highpass_hz;
            uint32_t span;
            double   old_gain;
        } spots[] = {
            { 880, 220, 11, 2.95233438 },         { 880, 220, taps, 1.00023440 },
            { 441, 440, 11, 0.00225391524 },      { 441, 440, taps, 0.00975174565 },
            { 22049, 22048, 11, 0.000190476168 }, { 22049, 22048, taps, 0.0194805887 },
            { 2, 1, taps, 0.998850774 },          { 466, 415, 11, 0.121851303 },
            { 466, 415, taps, 0.469485529 },
        };

        // Reduction shapes the production shader can actually consume: its
        // second stage runs only in subgroup 0, whose lanes load one partial
        // each, so the subgroup count must not exceed the subgroup size.
        uint32_t shapes[][2] = { { 256, 32 }, { 256, 16 } };
        for (const auto& shape : shapes) {
            TEST(shape[0] / shape[1] <= shape[1]);
            for (const auto& spot : spots) {
                fir_test_mirror(spot.lowpass_hz, spot.highpass_hz, spot.span, shape[0], shape[1], &mirror);
                // Taps outside the span are exactly zero, the support lies
                // in [0, span - 1], and every stored coefficient is finite.
                fir_test_check_span_shape(mirror, spot.span);
                const double magnitude =
                    fir_test_response(mirror.coefficients, static_cast<double>(mirror.center_freq));
                TEST(std::isfinite(magnitude));
                TEST(fabs(magnitude / static_cast<double>(mirror.old_denominator) / spot.old_gain - 1.0) <= 2e-3);
                const double stored_magnitude =
                    fir_test_stored_response(mirror, static_cast<double>(mirror.center_freq), stored);
                TEST(std::isfinite(stored_magnitude));
                TEST(fabs(stored_magnitude - 1.0) <= fir_test_unity_tolerance(spot.lowpass_hz, spot.highpass_hz));
            }
        }
    }

    // Direct effect pairs: unity at the band center for every odd span the
    // shader can produce and for fractional effect slopes.  The oscillator
    // grid below covers the slope-to-span mapping.
    {
        static const struct {
            uint32_t lowpass_hz;
            uint32_t highpass_hz;
        } pairs[] = { { 880, 220 }, { 441, 440 }, { 22049, 22048 }, { 2, 1 }, { 466, 415 } };

        float fractional_slopes[] = { 1.5f, 37.25f };
        for (const auto& pair : pairs) {
            for (uint32_t span = 11; span <= taps; span += 2) {
                fir_test_mirror(pair.lowpass_hz, pair.highpass_hz, span, 256, 32, &mirror);
                fir_test_check_span_shape(mirror, span);
                const double magnitude =
                    fir_test_stored_response(mirror, static_cast<double>(mirror.center_freq), stored);
                TEST(fabs(magnitude - 1.0) <= fir_test_unity_tolerance(pair.lowpass_hz, pair.highpass_hz));
            }
            for (const float slope : fractional_slopes) {
                const uint32_t span = Synth::fir_effective_span(slope);
                fir_test_mirror(pair.lowpass_hz, pair.highpass_hz, span, 256, 32, &mirror);
                fir_test_check_span_shape(mirror, span);
                const double magnitude =
                    fir_test_stored_response(mirror, static_cast<double>(mirror.center_freq), stored);
                TEST(fabs(magnitude - 1.0) <= fir_test_unity_tolerance(pair.lowpass_hz, pair.highpass_hz));
            }
        }
    }

    // The sub-1-Hz floor transports a low edge as pure lowpass; the unchanged
    // coeff_sum normalization keeps the DC gain at unity there.
    {
        float lowpass_edge  = 0.0f;
        float highpass_edge = 0.0f;
        TEST(Synth::oscillator_fir_cutoffs(-36.0f, 12.0f, 1, Synth::fir_bandpass, &lowpass_edge, &highpass_edge));
        TEST(highpass_edge == 0.0f);
        const uint32_t lowpass = fir_test_quantize_cutoff(lowpass_edge);
        TEST(lowpass > 0);
        // Low-pass DC gain at unity for every odd span the shader can
        // produce, with the kernel shape asserted per span.
        for (uint32_t span = 11; span <= taps; span += 2) {
            fir_test_mirror(lowpass, 0, span, 256, 32, &mirror);
            fir_test_check_span_shape(mirror, span);
            const double dc = fir_test_stored_response(mirror, 0.0, stored);
            TEST(fabs(dc - 1.0) <= 1e-4);
        }
    }

    // Oscillator grid through the real edge composition, quantization, guard
    // and span helpers: unity at the band center for every valid band-pass
    // case; floor-killed pure lowpass cases classify into the DC check.
    {
        float    pitches[] = { 0.0f, 36.0f, 69.0f, 96.0f, 127.0f };
        float    spreads[] = { 1e-9f, 0.1f, 1.0f, 12.0f, 36.0f };
        uint32_t mults[]   = { 1, 3 };
        for (const float pitch : pitches) {
            for (const float spread : spreads) {
                for (const uint32_t mult : mults) {
                    float lowpass_edge  = 0.0f;
                    float highpass_edge = 0.0f;
                    if (! Synth::oscillator_fir_cutoffs(pitch,
                                                        spread,
                                                        mult,
                                                        Synth::fir_bandpass,
                                                        &lowpass_edge,
                                                        &highpass_edge)) {
                        continue; // effective spread <= 0: passthrough, no dispatch
                    }
                    uint32_t lowpass  = fir_test_quantize_cutoff(lowpass_edge);
                    uint32_t highpass = fir_test_quantize_cutoff(highpass_edge); // 0 when the floor killed the edge
                    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
                    for (uint32_t slope = 1; slope <= 100; slope++) {
                        const uint32_t span = Synth::fir_effective_span(static_cast<float>(slope));
                        if (highpass == 0) {
                            fir_test_mirror(lowpass, 0, span, 256, 32, &mirror);
                            fir_test_check_span_shape(mirror, span);
                            const double dc = fir_test_stored_response(mirror, 0.0, stored);
                            TEST(fabs(dc - 1.0) <= 1e-4);
                            continue;
                        }
                        TEST(highpass < lowpass);
                        fir_test_mirror(lowpass, highpass, span, 256, 32, &mirror);
                        fir_test_check_span_shape(mirror, span);
                        const double magnitude =
                            fir_test_stored_response(mirror, static_cast<double>(mirror.center_freq), stored);
                        TEST(fabs(magnitude - 1.0) <= fir_test_unity_tolerance(lowpass, highpass));
                    }
                }
            }
        }
    }

    // Unchanged branches: pure lowpass keeps the coefficient-DC-sum
    // normalization (unity DC gain) and pure highpass keeps the center-window
    // denominator, not an exact Nyquist gain.  Passthrough, equal-edge impulse
    // and band-stop cases fail the pinned band-pass predicate, so they stay on
    // the old paths by construction.
    {
        for (uint32_t span = 11; span <= taps; span += 2) {
            fir_test_mirror(880, 0, span, 256, 32, &mirror);
            fir_test_check_span_shape(mirror, span);
            const double dc = fir_test_stored_response(mirror, 0.0, stored);
            TEST(fabs(dc - 1.0) <= 1e-4);

            fir_test_mirror(0, 880, span, 256, 32, &mirror);
            fir_test_check_span_shape(mirror, span);
            const float half_span     = static_cast<float>(span) * 0.5f - 0.5f;
            const float span_m1       = static_cast<float>(span - 1);
            const float center_window = 0.42f - 0.5f * cosf((2.0f * pi * half_span) / span_m1) +
                                        0.08f * cosf((4.0f * pi * half_span) / span_m1);
            TEST(mirror.old_denominator == center_window);
            TEST(fabs(static_cast<double>(mirror.old_denominator) - 1.0) <= 1e-4);
        }
    }

    // Degenerate denominator guards, mirrored from the shader's selection:
    // zero sums, NaN, infinity and squared-magnitude underflow fall back to
    // the coeff_sum path (sign preserved); a degenerate fallback becomes 1;
    // a small but valid magnitude is used without an epsilon floor.
    {
        TEST(fir_test_bandpass_denominator(0.0f, 0.0f, 0.5f) == 0.5f);
        TEST(fir_test_bandpass_denominator(0.0f, 0.0f, -0.5f) == -0.5f);
        TEST(fir_test_bandpass_denominator(NAN, 0.0f, 0.5f) == 0.5f);
        TEST(fir_test_bandpass_denominator(0.0f, INFINITY, 0.5f) == 0.5f);
        TEST(fir_test_bandpass_denominator(1e-25f, 0.0f, 0.25f) == 0.25f);
        const float small_magnitude = fir_test_bandpass_denominator(1e-10f, 0.0f, 0.25f);
        TEST(small_magnitude > 0.0f && small_magnitude < 1e-9f);
        TEST(fir_test_bandpass_denominator(0.0f, 0.0f, 0.0f) == 1.0f);
        TEST(fir_test_bandpass_denominator(0.0f, 0.0f, NAN) == 1.0f);
        TEST(fir_test_bandpass_denominator(0.0f, 0.0f, INFINITY) == 1.0f);
        TEST(fir_test_bandpass_denominator(NAN, NAN, NAN) == 1.0f);
    }
}

// Host mirror of the from-silence volume ramp in synth_chan_combine.comp.glsl: the 32-sample attack when
// the previous volume gain is zero, the full-step interpolation and 5th-order fade-out fallbacks, and
// continuity across step boundaries.
static void check_chan_combine_volume_ramp()
{
    constexpr uint32_t work_group_size           = 256; // rt_step_samples (spec constant id 0)
    constexpr uint32_t volume_adjustment_samples = 32;  // spec constant id 3
    const float        target                    = 0.8f;

    // GLSL mix in the spec's spelling, exact at a = 0 and a = 1.
    const auto mixf = [](float x, float y, float a) { return x * (1.0f - a) + y * a; };
    // The full-step variable.
    const auto full_step = [](uint32_t x) { return static_cast<float>(x) / static_cast<float>(work_group_size - 1u); };
    // The 5th-order fade-out.
    const auto fade_out = [](float step) { return step * step * step * (step * (step * 6.0f - 15.0f) + 10.0f); };
    // The from-silence branch: the 32-sample attack, then hold.
    const auto attack_fade = [](uint32_t x) {
        return x < volume_adjustment_samples
                   ? (static_cast<float>(x) + 1.0f) / static_cast<float>(volume_adjustment_samples)
                   : 1.0f;
    };

    // From silence (note-on): sample 0 plays 1/32 of the target (the
    // historical step = (x + 1) / 32 formula), the target is reached
    // exactly at sample 31 and held through sample 255.
    {
        const float old_volume = 0.0f;
        for (uint32_t x = 0; x < work_group_size; x++) {
            const float multiplier = mixf(old_volume, target, attack_fade(x));
            if (x == 0) {
                TEST(multiplier == target * (1.0f / static_cast<float>(volume_adjustment_samples)));
            }
            else if (x < volume_adjustment_samples - 1) {
                TEST(multiplier < target);
            }
            else {
                TEST(multiplier == target);
            }
            TEST(multiplier > 0.0f && multiplier <= target);
        }
    }

    // Zero to zero (silence restarted): the else branch multiplies a
    // zero-valued interpolation, so the whole step stays at exact zero.
    for (uint32_t x = 0; x < work_group_size; x++) {
        TEST(mixf(0.0f, 0.0f, fade_out(full_step(x))) == 0.0f);
    }

    // Mid-note nonzero -> nonzero: the full-step interpolation.
    // Endpoints are exact; the interior stays strictly between them.
    {
        const float old_volume = 0.5f;
        for (uint32_t x = 0; x < work_group_size; x++) {
            const float multiplier = mixf(old_volume, target, full_step(x));
            if (x == 0) {
                TEST(multiplier == old_volume);
            }
            else if (x == work_group_size - 1) {
                TEST(multiplier == target);
            }
            else {
                TEST(multiplier > old_volume && multiplier < target);
            }
        }
    }

    // Fade-out to zero: the 5th-order polynomial, non-increasing
    // across the step, exact endpoints.
    {
        float previous = 2.0f;
        for (uint32_t x = 0; x < work_group_size; x++) {
            const float multiplier = mixf(target, 0.0f, fade_out(full_step(x)));
            if (x == 0) {
                TEST(multiplier == target);
            }
            else if (x == work_group_size - 1) {
                TEST(multiplier == 0.0f);
            }
            TEST(multiplier <= previous);
            previous = multiplier;
        }
    }

    // Consecutive steps: the attack step ends at the target; the next step
    // holds the target when the volume is unchanged; a mid-note change
    // starts exactly at the previous step's end value; the change ends at
    // its own target and the following held step stays there.
    {
        TEST(mixf(0.0f, target, attack_fade(work_group_size - 1)) == target);
        // A degenerate mix(v, v, a) may wobble by a last ulp in binary32,
        // so the held steps assert continuity within an ulp-scale slack.
        for (uint32_t x = 0; x < work_group_size; x++) {
            TEST(fabsf(mixf(target, target, full_step(x)) - target) <= 1e-6f); // held note
        }
        const float next_target = 0.4f;
        TEST(mixf(target, next_target, full_step(0)) == target);
        TEST(mixf(target, next_target, full_step(work_group_size - 1)) == next_target);
        for (uint32_t x = 0; x < work_group_size; x++) {
            TEST(fabsf(mixf(next_target, next_target, full_step(x)) - next_target) <= 1e-6f);
        }
    }
}

template <typename OscT, typename = void> struct m2_has_band_count : std::false_type {};

template <typename GraphT, typename = void> struct m2_has_band_seam : std::false_type {};

template <typename OscT>
struct m2_has_band_count<OscT, std::void_t<decltype(std::declval<OscT&>().fir_band_count)>> : std::true_type {};

constexpr bool m2_band_state_present = m2_has_band_count<Synth::Oscillator>::value;

template <typename GraphT>
struct m2_has_band_seam<GraphT,
                        std::void_t<decltype(osc_band_slot_hidden(std::declval<const GraphT&>(),
                                                                  std::declval<uint32_t>(),
                                                                  std::declval<uint32_t>()))>> : std::true_type {};

constexpr bool m2_band_seam_present = m2_has_band_seam<Sculptor::Graph>::value;

constexpr bool m2_band_rows_present = Synth::num_mod_targets == 29;

// The band parameter target indices: band 0 reuses the legacy spread (7) and
// slope (11) targets; the 14 appended targets follow at 12..25 in the order
// band 0 offset/gain, then per band 1..3 offset/spread/gain/slope.
constexpr uint32_t m2_band0_targets[4] = { 12, 7, 13, 11 };

constexpr uint32_t m2_band_targets[4][4] = {
    { 12, 7, 13, 11 },
    { 14, 15, 16, 17 },
    { 18, 19, 20, 21 },
    { 22, 23, 24, 25 },
};

// One bandpass layer with the legacy spread/slope state and, when the band
// state exists, a band count plus per-band bases and bindings: bands 1..3
// carry distinct values and the offset row a full binding, and rich_band0
// binds band 0's offset and gain rows, so the copy, remove and serialization
// paths move non-default state.
template <bool M2, typename OscT>
static void build_m2_bandpass_fixture(Synth::InstrumentEditorBank* bank,
                                      OscT&                        osc,
                                      const bool                   seed_higher_bands,
                                      const bool                   rich_band0)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    TEST(bank->bank.envelopes.allocate() == 0);
    TEST(bank->bank.lfos.allocate() == 0);
    Synth::EnvelopeDescriptor& volume_env = bank->bank.envelopes.entries[0];
    volume_env.num_points                 = 2;
    volume_env.sustain_first_point        = 0;
    volume_env.sustain_last_point         = 1;
    volume_env.min_value                  = 0.0f;
    volume_env.min_max_delta              = 1.0f;
    volume_env.points[0]                  = { 0, 0x4000 };
    volume_env.points[1]                  = { 300, 0xC000 };
    TEST(bank->bank.envelopes.allocate() == 1);
    Synth::EnvelopeDescriptor& shared_env           = bank->bank.envelopes.entries[1];
    shared_env                                      = volume_env;
    shared_env.min_value                            = -1.0f;
    shared_env.min_max_delta                        = 2.0f;
    Synth::LFODescriptor& lfo                       = bank->bank.lfos.entries[0];
    lfo.wave                                        = Synth::WaveType::sine_wave;
    lfo.period_ms                                   = 250;
    lfo.min_value                                   = -1.0f;
    lfo.min_max_delta                               = 2.0f;
    osc.osc_type[0]                                 = Synth::WaveType::sine_wave;
    osc.osc_type[1]                                 = Synth::WaveType::pulse_wave;
    osc.osc_mode                                    = Synth::osc_mode_blend;
    osc.fir_type                                    = Synth::fir_bandpass;
    osc.fir_slope_percent                           = 50;
    osc.targets[Synth::mod_volume].base_value       = 0.8f;
    osc.targets[Synth::mod_volume].envelope_desc_id = 1;
    osc.targets[Synth::mod_volume].lfo_desc_id      = 1;
    osc.targets[Synth::mod_volume].lfo_op           = Synth::SourceOp::add;
    osc.targets[Synth::mod_volume].lfo_depth        = 0.5f;
    osc.targets[Synth::mod_pitch].envelope_desc_id  = 2;
    osc.targets[Synth::mod_fir_spread].base_value   = 3.0f;
    if constexpr (M2) {
        osc.fir_band_count                          = seed_higher_bands ? 4 : 1;
        osc.targets[m2_band0_targets[0]].base_value = 5.0f;
        osc.targets[m2_band0_targets[2]].base_value = 0.5f;
        if (rich_band0) {
            Synth::TargetBinding& offset = osc.targets[m2_band0_targets[0]];
            offset.envelope_desc_id      = 2;
            offset.lfo_desc_id           = 1;
            offset.lfo_op                = Synth::SourceOp::add;
            offset.lfo_depth             = 0.5f;
            offset.lfo_depth_source      = Synth::ModSource::velocity;
            offset.lfo_rate_source       = Synth::ModSource::mod_wheel;
            offset.lfo_rate_scale_ms     = 25.0f;
            offset.num_inputs            = 2;
            offset.inputs[0]             = { Synth::ModSource::velocity, Synth::SourceOp::add, 0, 0.5f };
            offset.inputs[1]             = { Synth::ModSource::mod_wheel, Synth::SourceOp::multiply, 0, 0.25f };
            Synth::TargetBinding& gain   = osc.targets[m2_band0_targets[2]];
            gain.lfo_desc_id             = 1;
            gain.lfo_op                  = Synth::SourceOp::multiply;
            gain.lfo_depth               = 0.25f;
        }
        if (seed_higher_bands) {
            for (uint32_t band = 1; band < 4; band++) {
                const float seed                                 = static_cast<float>(band);
                osc.targets[m2_band_targets[band][0]].base_value = 5.0f + 2.0f * seed;
                osc.targets[m2_band_targets[band][1]].base_value = 3.0f + seed;
                osc.targets[m2_band_targets[band][2]].base_value = 0.9f - 0.1f * seed;
                osc.targets[m2_band_targets[band][3]].base_value = 100.0f - 10.0f * seed;
                Synth::TargetBinding& gen                        = osc.targets[m2_band_targets[band][0]];
                gen.envelope_desc_id                             = 2;
                gen.lfo_desc_id                                  = 1;
                gen.lfo_op                                       = Synth::SourceOp::multiply;
                gen.lfo_depth                                    = 0.25f * seed;
                gen.lfo_depth_source                             = Synth::ModSource::velocity;
                gen.lfo_rate_source                              = Synth::ModSource::mod_wheel;
                gen.lfo_rate_scale_ms                            = 10.0f * seed;
                gen.num_inputs                                   = 2;
                gen.inputs[0] = { Synth::ModSource::velocity, Synth::SourceOp::add, 0, 0.5f * seed };
                gen.inputs[1] = { Synth::ModSource::mod_wheel, Synth::SourceOp::multiply, 0, 0.125f * seed };
            }
        }
    }
}

// Projects the bandpass fixture as channel 0 zone 0 and drains the initial
// projection changes; false skips the caller's block.
template <bool M2>
static bool begin_m2_band_graph(Synth::InstrumentEditorBank* bank,
                                Sculptor::Graph*             graph,
                                Sculptor::OscGraphMapping*   mapping,
                                const bool                   seed_higher_bands,
                                const bool                   rich_band0)
{
    Synth::Instrument instrument = {};
    build_m2_bandpass_fixture<M2>(bank, instrument.layers[0], seed_higher_bands, rich_band0);
    instrument.layer_count = 1;
    set_default_dispersion_sections(instrument);
    bank->bank.channel_enabled[0] = 1;
    TEST(bank->bank.instruments.allocate() == 0);
    bank->bank.instruments.entries[0]         = instrument;
    bank->bank.channel_zones[0][0].start_note = 1;
    bank->bank.channel_zones[0][0].instrument = 0;
    if (! Sculptor::project_editor_to_graph(*bank, graph, mapping)) {
        TEST(false);
        return false;
    }
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes] = {};
    (void)graph->take_changes(discarded, Sculptor::max_pending_changes);
    return true;
}

// The occurrence-th slot with `name` (0-based), or pool_no_slot.
static uint32_t find_nth_osc_slot(const Sculptor::Node& node, const char* name, const uint32_t occurrence)
{
    uint32_t seen = 0;
    for (uint32_t idx = 0; idx < node.slots.num_allocated; idx++) {
        if (strncmp(node.slots.entries[idx].name, name, sizeof(node.slots.entries[0].name)) == 0) {
            if (seen == occurrence) {
                return idx;
            }
            seen++;
        }
    }
    return Sculptor::pool_no_slot;
}

// Registry index of the detached instance of `kind` (1 envelope, 2 LFO) bound
// to descriptor `desc_id`, or -1.
static int32_t find_m2_detached_desc(const Sculptor::OscGraphMapping& mapping,
                                     const uint32_t                   kind,
                                     const uint32_t                   desc_id)
{
    for (uint32_t i = 0; i < mapping.detached_count; i++) {
        if (mapping.detached[i].kind == kind && mapping.detached[i].desc_id == desc_id &&
            mapping.detached[i].node_idx != Sculptor::pool_no_slot) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Wires the shared envelope and LFO instances plus both MIDI source rows onto
// `target`'s block dots through the editor change path.
static bool wire_m2_target(Synth::InstrumentEditorBank* bank,
                           Sculptor::Graph*             graph,
                           Sculptor::OscGraphMapping*   mapping,
                           const uint32_t               osc_node,
                           const uint32_t               target)
{
    const int32_t env_inst = find_m2_detached_desc(*mapping, 1, 2);
    const int32_t lfo_inst = find_m2_detached_desc(*mapping, 2, 1);
    TEST(env_inst >= 0 && lfo_inst >= 0);
    if (env_inst < 0 || lfo_inst < 0) {
        return false;
    }
    const Sculptor::EndPoint env_out      = { mapping->detached[env_inst].node_idx, mapping->env_output_slot };
    const Sculptor::EndPoint lfo_out      = { mapping->detached[lfo_inst].node_idx, mapping->lfo_output_slot };
    const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
    const uint32_t           wheel_idx    = static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1;
    const Sculptor::EndPoint velocity_out = { mapping->input_node, mapping->input_source_slots[velocity_idx] };
    const Sculptor::EndPoint wheel_out    = { mapping->input_node, mapping->input_source_slots[wheel_idx] };
    const Sculptor::EndPoint dots[4]      = {
        { osc_node, Sculptor::osc_target_env_dot(target) },
        { osc_node, Sculptor::osc_target_lfo_dot(target) },
        { osc_node, Sculptor::osc_target_src_dot(target, 0) },
        { osc_node, Sculptor::osc_target_src_dot(target, 1) },
    };
    const Sculptor::EndPoint outs[4] = { env_out, lfo_out, velocity_out, wheel_out };
    for (uint32_t i = 0; i < 4; i++) {
        if (find_osc_graph_connection(*graph, dots[i]) != Sculptor::pool_no_slot) {
            continue;
        }
        if (graph->add_connection(outs[i], dots[i]) == Sculptor::pool_no_slot) {
            TEST(false);
            return false;
        }
        Sculptor::GraphChange changes[Sculptor::max_pending_changes] = {};
        const uint32_t        count = graph->take_changes(changes, Sculptor::max_pending_changes);
        TEST(count >= 1);
        bool applied = count >= 1;
        for (uint32_t c = 0; c < count; c++) {
            if (changes[c].kind != Sculptor::ChangeKind::connection_added) {
                continue;
            }
            applied = Sculptor::apply_osc_graph_change(bank, graph, mapping, changes[c]) && applied;
        }
        TEST(applied);
        if (! applied) {
            return false;
        }
    }
    return true;
}

// Mirrors Sculptor::SynthEditor::drain_osc_graph's apply loop for one
// queued batch: applies the events in order and stops after a band [-]/[+]
// structural event, discarding the remaining queued events (the graph is
// stale until the re-projection rebuilds it from the model). Returns the
// number of discarded events.
static uint32_t m2_drain_osc_batch(Synth::InstrumentEditorBank* bank,
                                   Sculptor::Graph*             graph,
                                   Sculptor::OscGraphMapping*   mapping,
                                   const Sculptor::GraphChange* changes,
                                   const uint32_t               count)
{
    bool     ok      = true;
    uint32_t applied = 0;
    for (uint32_t i = 0; i < count && ok; ++i) {
        ok = Sculptor::apply_osc_graph_change(bank, graph, mapping, changes[i]);
        if (ok) {
            applied = i + 1;
        }
        if (ok && changes[i].kind == Sculptor::ChangeKind::value_changed &&
            Sculptor::is_band_button_slot(changes[i].slot_idx)) {
            break;
        }
    }
    return count - applied;
}

// The band header labels, indexed by band (the test-side copy of the
// production header labels).
static const char* const band_header_names_m2[4] = { "Band 1", "Band 2", "Band 3", "Band 4" };

// Band 1's header row (label plus its [-]/[+] click slots) nests one
// level under the Filter Type row (like the legacy Cutoff row), every
// existing band projects one uniform 55-slot block with generic
// parameter labels, and only existing bands project.
// Low-pass keeps the cutoff and FIR Slope labels.
static void m2_band_layout_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    TEST(Synth::instrument_graph_layout_capacity == 415);
    TEST(Synth::fir_coeff_entry_words == 18);

    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, false)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const Sculptor::Node& osc = graph.node(mapping.osc_nodes[0]);
    // One band block: 150 + 55 slots.
    TEST(osc.slots.num_allocated == Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride);
    TEST(find_nth_osc_slot(osc, "Band 1", 0) == Sculptor::osc_band_header_slot(0));
    TEST(find_nth_osc_slot(osc, "Filter Type", 0) == 149);
    const uint32_t        header_slot = Sculptor::osc_band_header_slot(0);
    const Sculptor::Slot& header      = osc.slots.entries[header_slot];
    TEST(strncmp(header.name, "Band 1", sizeof(header.name)) == 0);
    TEST(header.property_type == Sculptor::PropertyType::unused);
    TEST(! header.connectable);
    TEST(header.child_row && header.child_level == 1);
    TEST(header.row_group == Sculptor::osc_band_row_group);
    const Sculptor::Slot& minus = osc.slots.entries[header_slot + 1];
    const Sculptor::Slot& plus  = osc.slots.entries[header_slot + 2];
    TEST(minus.property_type == Sculptor::PropertyType::toggle && plus.property_type == Sculptor::PropertyType::toggle);
    TEST(minus.row_group == header.row_group && plus.row_group == header.row_group);
    TEST(! minus.child_row && minus.child_level == 0);
    TEST(! plus.child_row && plus.child_level == 0);

    // Four bands: band 0's rows sit at 153/166/179/192 and
    // bands 1..3 shift by 3 slots per earlier band.
    static const struct {
        uint32_t    slot;
        const char* name;
        uint32_t    target;
    } band_rows[16] = {
        { 153, "Offset", 12 }, { 166, "Spread", 7 },  { 179, "Gain", 13 }, { 192, "FIR Slope", 11 },
        { 208, "Offset", 14 }, { 221, "Spread", 15 }, { 234, "Gain", 16 }, { 247, "FIR Slope", 17 },
        { 263, "Offset", 18 }, { 276, "Spread", 19 }, { 289, "Gain", 20 }, { 302, "FIR Slope", 21 },
        { 318, "Offset", 22 }, { 331, "Spread", 23 }, { 344, "Gain", 24 }, { 357, "FIR Slope", 25 },
    };

    bank.bank.instruments.entries[0].layers[0].fir_band_count = 4;
    if (reproject_m2_band_graph(&bank, &graph, &mapping)) {
        const Sculptor::Node& four = graph.node(mapping.osc_nodes[0]);
        TEST(four.slots.num_allocated == Sculptor::osc_band_first_slot + 4u * Sculptor::osc_band_block_stride);
        for (uint32_t b = 0; b < 4; b++) {
            const Sculptor::Slot& band_header = four.slots.entries[Sculptor::osc_band_header_slot(b)];
            TEST(strncmp(band_header.name, band_header_names_m2[b], sizeof(band_header.name)) == 0);
            TEST(band_header.child_row && band_header.child_level == 1);
        }
        for (const auto& row : band_rows) {
            TEST(strncmp(four.slots.entries[row.slot].name, row.name, sizeof(four.slots.entries[0].name)) == 0);
            TEST(Sculptor::osc_target_row(row.target) == row.slot);
            TEST(Sculptor::osc_target_block_base(row.target) == row.slot + 1);
            TEST(strncmp(four.slots.entries[row.slot + 1].name, "Envelope", sizeof(four.slots.entries[0].name)) == 0);
            TEST(four.slots.entries[row.slot].child_row && four.slots.entries[row.slot].child_level == 2);
        }
        bank.bank.instruments.entries[0].layers[0].fir_band_count = 1;
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // Low-pass keeps the cutoff and legacy FIR Slope labels at their slots.
    bank.bank.instruments.entries[0].layers[0].fir_type = Synth::fir_lowpass;
    if (reproject_m2_band_graph(&bank, &graph, &mapping)) {
        const Sculptor::Node& lowpass_osc = graph.node(mapping.osc_nodes[0]);
        const uint32_t        cutoff_slot = find_nth_osc_slot(lowpass_osc, "Cutoff (Hz)", 0);
        const uint32_t        slope_slot  = find_nth_osc_slot(lowpass_osc, "FIR Slope", 0);
        TEST(cutoff_slot == 166);
        TEST(slope_slot == 192);
        // The legacy single-edge rows keep the fix-6 legacy depths: the
        // Cutoff and FIR Slope rows stay one level under Filter Type.
        TEST(lowpass_osc.slots.entries[cutoff_slot].child_row &&
             lowpass_osc.slots.entries[cutoff_slot].child_level == 1);
        TEST(lowpass_osc.slots.entries[slope_slot].child_row && lowpass_osc.slots.entries[slope_slot].child_level == 1);
        // The whole band UI hides for the single-edge filter types (the
        // legacy cutoff/slope carve-out lives in the composed seam).
        for (uint32_t slot = Sculptor::osc_band_first_slot; slot < lowpass_osc.slots.num_allocated; slot++) {
            TEST(Sculptor::osc_band_slot_hidden(graph, mapping.osc_nodes[0], slot));
        }
        bank.bank.instruments.entries[0].layers[0].fir_type = Synth::fir_bandpass;
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // Under bandpass every existing band is always visible: nothing in
    // the band region hides, wired or not (the selection concept is gone).
    bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].envelope_desc_id = 2;
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    for (uint32_t slot = Sculptor::osc_band_first_slot; slot < osc.slots.num_allocated; slot++) {
        TEST(! Sculptor::osc_band_slot_hidden(graph, mapping.osc_nodes[0], slot));
    }
}

// C2.1: every existing band is always visible under bandpass (headers,
// buttons, rows, blocks - no selection concept); the single-edge filter
// types hide the whole band region, whose legacy carve-out lives in the
// composed seam.
template <bool Seam, typename GraphT>
static void m2_band_visibility_body(GraphT& graph, const uint32_t osc_node, const uint32_t filter_slot)
{
    if constexpr (Seam) {
        const auto     hidden = [&](const uint32_t slot) { return osc_band_slot_hidden(graph, osc_node, slot); };
        const uint32_t first  = Sculptor::osc_band_first_slot;
        const uint32_t last   = Sculptor::osc_band_header_slot(3) + Sculptor::osc_band_block_stride;
        for (uint32_t slot = first; slot < last; slot++) {
            TEST(! hidden(slot));
        }
        // Off, lowpass and highpass hide every band slot.
        for (uint32_t type = 0; type < Synth::num_filter_types; type++) {
            if (type == static_cast<uint32_t>(Synth::fir_bandpass)) {
                continue;
            }
            Sculptor::PropertyValue select_type = {};
            select_type.list_index              = static_cast<uint8_t>(type);
            graph.set_slot_value(osc_node, filter_slot, select_type);
            for (uint32_t slot = first; slot < last; slot++) {
                TEST(hidden(slot));
            }
        }
        // Back to bandpass for the caller.
        Sculptor::PropertyValue bandpass = {};
        bandpass.list_index              = static_cast<uint8_t>(Synth::fir_bandpass);
        graph.set_slot_value(osc_node, filter_slot, bandpass);
    }
    else {
        TEST(false);
    }
}

static void m2_band_visibility_regressions()
{
    TEST(m2_band_rows_present && m2_band_seam_present);
    if (! (m2_band_rows_present && m2_band_seam_present)) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, true, false)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const Sculptor::Node& osc         = graph.node(mapping.osc_nodes[0]);
    const uint32_t        header_slot = find_nth_osc_slot(osc, "Band 1", 0);
    const uint32_t        filter_slot = find_nth_osc_slot(osc, "Filter Type", 0);
    TEST(header_slot == Sculptor::osc_band_header_slot(0) && filter_slot == 149);
    if (header_slot != Sculptor::osc_band_header_slot(0) || filter_slot != 149) {
        return;
    }
    m2_band_visibility_body<m2_band_seam_present>(graph, mapping.osc_nodes[0], filter_slot);
}

// [-] on band N removes band N (later bands shift up and the
// vacated tail zeroes); [+] on band N inserts a copy of band N after it
// (values and full modulation bindings). Counts stay within 1..4 and a
// removed band leaks nowhere into the exported document.
// A MIDI source wired into a band row keeps its model binding when the
// filter leaves bandpass (re-selecting bandpass restores the wire), but
// the render state goes dead: the band row and its source dot hide and
// the source endpoint stays connected - exactly the state the renderer
// must gray at the wire and at the source.
static void m2_band_source_gray_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, true, false)) {
        return;
    }
    const uint32_t osc_node      = mapping.osc_nodes[0];
    const uint32_t dot           = Sculptor::osc_target_src_dot(14, 0); // band 1 offset's Source A
    const uint32_t velocity_slot = mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1];
    TEST(find_osc_graph_connection(graph, { osc_node, dot }) != Sculptor::pool_no_slot);
    const uint32_t filter_slot = find_nth_osc_slot(graph.node(osc_node), "Filter Type", 0);
    TEST(filter_slot == 149);
    if (filter_slot != 149) {
        return;
    }
    // Filter to off through the apply path, then rebuild from the model.
    Sculptor::PropertyValue off = {};
    off.list_index              = static_cast<uint8_t>(Synth::fir_off);
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, filter_slot, off));
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    TEST(find_osc_graph_connection(graph, { osc_node, dot }) != Sculptor::pool_no_slot);
    TEST(Sculptor::osc_band_slot_hidden(graph, osc_node, dot));
    TEST(Sculptor::osc_node_slot_hidden(graph, osc_node, dot));
    TEST(graph.slot_is_connected(mapping.input_node, velocity_slot));
    // Back to bandpass: the wire restores and the rows un-hide.
    Sculptor::PropertyValue bandpass = {};
    bandpass.list_index              = static_cast<uint8_t>(Synth::fir_bandpass);
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, filter_slot, bandpass));
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    TEST(find_osc_graph_connection(graph, { osc_node, dot }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::osc_band_slot_hidden(graph, osc_node, dot));
}

static void m2_band_add_remove_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, true)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const uint32_t osc_node  = mapping.osc_nodes[0];
    const uint32_t header0   = Sculptor::osc_band_header_slot(0);
    const uint32_t plus_slot = header0 + 2;
    const auto     click     = [&](const uint32_t slot) {
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        return apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, slot, press);
    };
    const auto layer       = [&]() -> Synth::Oscillator& { return bank.bank.instruments.entries[0].layers[0]; };
    const auto bands_match = [&](const uint32_t added, const uint32_t previous) {
        bool matched = true;
        for (uint32_t p = 0; p < 4; p++) {
            const uint32_t added_target    = m2_band_targets[added][p];
            const uint32_t previous_target = m2_band_targets[previous][p];
            matched                        = memcmp(&layer().targets[added_target],
                                                    &layer().targets[previous_target],
                                                    sizeof(Synth::TargetBinding)) == 0 &&
                                             matched;
        }
        return matched;
    };
    const auto projected_count = [&](const uint32_t expect) {
        TEST(graph.node(osc_node).slots.num_allocated ==
             Sculptor::osc_band_first_slot + expect * Sculptor::osc_band_block_stride);
    };

    // Appends: [+] on the last band copies it wholesale.  Band 0's
    // unrouted slope binding keeps the zeroed sentinel while the layer
    // scalar carries its value, so the first add's slope copy carries the
    // displayed scalar (value-copy semantics), not band 0's bytes;
    // every other binding byte-equals the copied band's.
    TEST(click(plus_slot));
    TEST(layer().fir_band_count == 2);
    for (uint32_t p = 0; p < 4; p++) {
        const Synth::TargetBinding& added    = layer().targets[m2_band_targets[1][p]];
        const Synth::TargetBinding& previous = layer().targets[m2_band_targets[0][p]];
        if (p == 3) {
            TEST(added.base_value == 50.0f);   // the fixture's fir_slope_percent scalar
            TEST(previous.base_value == 0.0f); // the unrouted sentinel
        }
        else {
            TEST(memcmp(&added, &previous, sizeof(Synth::TargetBinding)) == 0);
        }
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    projected_count(2);
    TEST(find_nth_osc_slot(graph.node(osc_node), "Band 2", 0) == Sculptor::osc_band_header_slot(1));

    TEST(click(Sculptor::osc_band_header_slot(1) + 2));
    TEST(layer().fir_band_count == 3);
    TEST(bands_match(2, 1));
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping)); // the next click needs the rebuilt slots
    TEST(click(Sculptor::osc_band_header_slot(2) + 2));
    TEST(layer().fir_band_count == 4);
    TEST(bands_match(3, 2));
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    projected_count(4);

    // The fifth add is a no-op.
    TEST(click(plus_slot));
    TEST(layer().fir_band_count == 4);

    // Band 3 carries distinct state with a full binding tuple.
    for (uint32_t p = 0; p < 4; p++) {
        Synth::TargetBinding& gen = layer().targets[m2_band_targets[3][p]];
        gen                       = {};
        gen.base_value            = p == 2 ? 0.2f + 0.2f * static_cast<float>(p)
                                           : 1.0f + static_cast<float>(p); // gain stays within its 0.01..1 view
        gen.envelope_desc_id      = 2;
        gen.lfo_desc_id           = 1;
        gen.num_inputs            = 1;
        gen.inputs[0]             = { Synth::ModSource::aftertouch, Synth::SourceOp::add, 0, 0.75f };
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // [-] on band 2 removes band 2: band 3 shifts up into band 2 (bindings
    // and wires follow) and the last block zeroes.
    Synth::TargetBinding old_b1[4];
    Synth::TargetBinding old_b2[4];
    Synth::TargetBinding old_b3[4];
    for (uint32_t p = 0; p < 4; p++) {
        old_b1[p] = layer().targets[m2_band_targets[1][p]];
        old_b2[p] = layer().targets[m2_band_targets[2][p]];
        old_b3[p] = layer().targets[m2_band_targets[3][p]];
    }
    TEST(click(Sculptor::osc_band_header_slot(1) + 1));
    TEST(layer().fir_band_count == 3);
    for (uint32_t p = 0; p < 4; p++) {
        TEST(memcmp(&layer().targets[m2_band_targets[1][p]], &old_b2[p], sizeof(Synth::TargetBinding)) == 0);
        TEST(memcmp(&layer().targets[m2_band_targets[2][p]], &old_b3[p], sizeof(Synth::TargetBinding)) == 0);
    }
    const Synth::TargetBinding zero = {};
    for (uint32_t p = 0; p < 4; p++) {
        TEST(memcmp(&layer().targets[m2_band_targets[3][p]], &zero, sizeof(zero)) == 0);
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    projected_count(3);
    // The shifted band's family wires re-appear on the new block.
    TEST(find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_env_dot(m2_band_targets[2][0]) }) !=
         Sculptor::pool_no_slot);
    TEST(find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_src_dot(m2_band_targets[2][0], 0) }) !=
         Sculptor::pool_no_slot);

    // [+] on band 1 inserts a copy of band 1 after it; the old bands 1
    // and 2 shift up to 2 and 3.
    TEST(click(plus_slot));
    TEST(layer().fir_band_count == 4);
    for (uint32_t p = 0; p < 4; p++) {
        TEST(memcmp(&layer().targets[m2_band_targets[1][p]], &old_b1[p], sizeof(Synth::TargetBinding)) == 0);
        TEST(memcmp(&layer().targets[m2_band_targets[2][p]], &old_b2[p], sizeof(Synth::TargetBinding)) == 0);
        TEST(memcmp(&layer().targets[m2_band_targets[3][p]], &old_b3[p], sizeof(Synth::TargetBinding)) == 0);
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    projected_count(4);

    // Removing band 2 shifts band 3 up into it and zeroes the vacated
    // tail; the exported document carries no removed-band keys.
    TEST(click(Sculptor::osc_band_header_slot(2) + 1));
    TEST(layer().fir_band_count == 3);
    for (uint32_t p = 0; p < 4; p++) {
        TEST(memcmp(&layer().targets[m2_band_targets[3][p]], &zero, sizeof(zero)) == 0);
    }
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    TEST(export_clipboard_fixture(bank, text, sizeof(text)));
    TEST(strstr(text, "fir_band3_") == nullptr);
}

// Removing band 0 shifts bands 1..3 up into 0..2: the stored bindings move
// wholesale, the re-projected wires follow the shifted blocks with their
// source identity intact, rows without bindings stay unwired, and the
// vacated band 3 block stops projecting.
static void m2_band_remove_first_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, true, true)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const uint32_t osc_node = mapping.osc_nodes[0];
    const auto     layer    = [&]() -> Synth::Oscillator& { return bank.bank.instruments.entries[0].layers[0]; };

    // Snapshot band 1's bindings, then remove band 0.
    Synth::TargetBinding old_b1[4];
    for (uint32_t p = 0; p < 4; p++) {
        old_b1[p] = layer().targets[m2_band_targets[1][p]];
    }
    Sculptor::PropertyValue press = {};
    press.toggle                  = true;
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, Sculptor::osc_band_header_slot(0) + 1, press));
    TEST(layer().fir_band_count == 3);
    for (uint32_t p = 0; p < 4; p++) {
        TEST(memcmp(&layer().targets[m2_band_targets[0][p]], &old_b1[p], sizeof(Synth::TargetBinding)) == 0);
    }
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    TEST(graph.node(osc_node).slots.num_allocated ==
         Sculptor::osc_band_first_slot + 3 * Sculptor::osc_band_block_stride);

    // The shifted wires keep their source identity: the shared envelope,
    // the shared LFO and the two MIDI sources that fed old band 1's offset
    // row now feed band 0's offset row.
    const int32_t env_inst = find_m2_detached_desc(mapping, 1, 2);
    const int32_t lfo_inst = find_m2_detached_desc(mapping, 2, 1);
    TEST(env_inst >= 0 && lfo_inst >= 0);
    if (env_inst < 0 || lfo_inst < 0) {
        return;
    }
    const uint32_t offset0   = m2_band_targets[0][0];
    const uint32_t env_wire  = find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_env_dot(offset0) });
    const uint32_t lfo_wire  = find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_lfo_dot(offset0) });
    const uint32_t src0_wire = find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_src_dot(offset0, 0) });
    const uint32_t src1_wire = find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_src_dot(offset0, 1) });
    TEST(env_wire != Sculptor::pool_no_slot && lfo_wire != Sculptor::pool_no_slot);
    TEST(src0_wire != Sculptor::pool_no_slot && src1_wire != Sculptor::pool_no_slot);
    if (env_wire != Sculptor::pool_no_slot && lfo_wire != Sculptor::pool_no_slot &&
        src0_wire != Sculptor::pool_no_slot && src1_wire != Sculptor::pool_no_slot) {
        // The wire's source must be a detached instance of the binding's
        // descriptors; multiple instances can share a descriptor id, so
        // the identity is checked through the wire's own source node.
        const int32_t env_of_wire = find_instance_by_node(mapping, graph.get_connection(env_wire).output.node_idx);
        const int32_t lfo_of_wire = find_instance_by_node(mapping, graph.get_connection(lfo_wire).output.node_idx);
        TEST(env_of_wire >= 0 && mapping.detached[env_of_wire].kind == 1 && mapping.detached[env_of_wire].desc_id == 2);
        TEST(lfo_of_wire >= 0 && mapping.detached[lfo_of_wire].kind == 2 && mapping.detached[lfo_of_wire].desc_id == 1);
        TEST(graph.get_connection(env_wire).output.slot_idx == mapping.env_output_slot);
        TEST(graph.get_connection(lfo_wire).output.slot_idx == mapping.lfo_output_slot);
        TEST(graph.get_connection(src0_wire).output.node_idx == mapping.input_node);
        TEST(graph.get_connection(src0_wire).output.slot_idx ==
             mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1]);
        TEST(graph.get_connection(src1_wire).output.node_idx == mapping.input_node);
        TEST(graph.get_connection(src1_wire).output.slot_idx ==
             mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1]);
    }

    // Family and parameter coverage on the shifted blocks: only the offset
    // rows of the surviving bands carry bindings (all four families), the
    // other parameter rows show no wires, and the vacated band 3 block
    // leaves no obsolete endpoints behind.
    const auto wires_into = [&](const uint32_t target) {
        const Sculptor::EndPoint dots[4] = {
            { osc_node, Sculptor::osc_target_env_dot(target) },
            { osc_node, Sculptor::osc_target_lfo_dot(target) },
            { osc_node, Sculptor::osc_target_src_dot(target, 0) },
            { osc_node, Sculptor::osc_target_src_dot(target, 1) },
        };
        uint32_t hits = 0;
        for (const Sculptor::EndPoint& dot : dots) {
            hits += find_osc_graph_connection(graph, dot) != Sculptor::pool_no_slot ? 1u : 0u;
        }
        return hits;
    };
    for (uint32_t band = 0; band < 4; band++) {
        for (uint32_t p = 0; p < 4; p++) {
            TEST(wires_into(m2_band_targets[band][p]) == (band < 3 && p == 0 ? 4u : 0u));
        }
    }
}

// The donation transition: add a band, give it distinct state through the
// real edit path, remove it, and re-add it in one session. The removed
// band's bindings are zeroed on removal, so the round-trip compile with
// the previous-binding donation must not resurrect them, and the re-added
// band is a fresh copy of the surviving band rather than the removed state.
static void m2_band_donation_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, true)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const uint32_t osc_node = mapping.osc_nodes[0];
    const auto     layer    = [&]() -> Synth::Oscillator& { return bank.bank.instruments.entries[0].layers[0]; };
    const uint32_t b1_gain  = m2_band_targets[1][2];

    Sculptor::PropertyValue press = {};
    press.toggle                  = true;
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, Sculptor::osc_band_header_slot(0) + 2, press));
    TEST(layer().fir_band_count == 2);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // Give the added band a distinct gain through the real edit path.
    Sculptor::PropertyValue gain_edit = {};
    gain_edit.real                    = 0.7f;
    TEST(apply_m2_slot_edit(&bank,
                            &graph,
                            &mapping,
                            osc_node,
                            Sculptor::osc_target_row(Synth::mod_fir_band1_gain),
                            gain_edit));
    TEST(layer().targets[b1_gain].base_value == 0.7f);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // Remove the added band: its bindings zero.
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, Sculptor::osc_band_header_slot(1) + 1, press));
    TEST(layer().fir_band_count == 1);
    const Synth::TargetBinding zero = {};
    TEST(memcmp(&layer().targets[b1_gain], &zero, sizeof(zero)) == 0);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    // The round-trip compile donates the previous bindings of bands beyond
    // the projected count: the zeroed tail must stay zeroed.
    Synth::Instrument compiled = {};
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled, nullptr, &bank.bank.instruments.entries[0]));
    TEST(memcmp(&compiled, &bank.bank.instruments.entries[0], sizeof(Synth::Instrument)) == 0);

    // Re-add in the same session: a fresh copy of band 0, and the removed
    // gain (0.7) must not leak back through the donation.
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, Sculptor::osc_band_header_slot(0) + 2, press));
    TEST(layer().fir_band_count == 2);
    TEST(layer().targets[b1_gain].base_value == 0.5f);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    Synth::Instrument recompiled = {};
    TEST(
        Sculptor::compile_graph_to_instrument(graph, mapping, &recompiled, nullptr, &bank.bank.instruments.entries[0]));
    TEST(memcmp(&recompiled, &bank.bank.instruments.entries[0], sizeof(Synth::Instrument)) == 0);
}

// The queued-batch structural boundary: a band [-]/[+] click and a later
// ordinary value event queued in one drain batch. The band event rewrites
// the stored oscillator and leaves the graph stale, so the drain stops at
// it and discards the remaining events (their GUI values snap back on the
// re-projection); the count change survives.
static void m2_band_batch_boundary_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, true)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const uint32_t osc_node = mapping.osc_nodes[0];
    const auto     layer    = [&]() -> Synth::Oscillator& { return bank.bank.instruments.entries[0].layers[0]; };
    const uint32_t b0_gain  = m2_band0_targets[2];

    // Grow to two bands so the batch carries [+] on band 0 followed by a
    // band 0 Gain edit.
    Sculptor::PropertyValue press = {};
    press.toggle                  = true;
    TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, Sculptor::osc_band_header_slot(0) + 2, press));
    TEST(layer().fir_band_count == 2);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));

    Sculptor::PropertyValue gain_edit = {};
    gain_edit.real                    = 0.6f;
    graph.set_slot_value(osc_node, Sculptor::osc_band_header_slot(0) + 2, press);
    graph.set_slot_value(osc_node, Sculptor::osc_target_row(Synth::mod_fir_band0_gain), gain_edit);
    Sculptor::GraphChange batch[2] = {};
    batch[0].kind                  = Sculptor::ChangeKind::value_changed;
    batch[0].node_idx              = osc_node;
    batch[0].slot_idx              = Sculptor::osc_band_header_slot(0) + 2;
    batch[1]                       = batch[0];
    batch[1].slot_idx              = Sculptor::osc_target_row(Synth::mod_fir_band0_gain);
    TEST(m2_drain_osc_batch(&bank, &graph, &mapping, batch, 2) == 1);
    // The insertion survives and the discarded Gain edit did not apply.
    TEST(layer().fir_band_count == 3);
    TEST(layer().targets[b0_gain].base_value == 0.5f);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    TEST(graph.node(osc_node).slots.num_allocated ==
         Sculptor::osc_band_first_slot + 3 * Sculptor::osc_band_block_stride);
    TEST(layer().targets[m2_band_targets[1][2]].base_value == 0.5f);

    // The same boundary for removal: [-] on band 0 then a band 0 Gain edit.
    graph.set_slot_value(osc_node, Sculptor::osc_band_header_slot(0) + 1, press);
    graph.set_slot_value(osc_node, Sculptor::osc_target_row(Synth::mod_fir_band0_gain), gain_edit);
    batch[0].slot_idx = Sculptor::osc_band_header_slot(0) + 1;
    batch[1].slot_idx = Sculptor::osc_target_row(Synth::mod_fir_band0_gain);
    TEST(m2_drain_osc_batch(&bank, &graph, &mapping, batch, 2) == 1);
    TEST(layer().fir_band_count == 2);
    // Band 0 now holds the old band 1 (the copy): the discarded edit did
    // not apply against the stale graph.
    TEST(layer().targets[b0_gain].base_value == 0.5f);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    TEST(graph.node(osc_node).slots.num_allocated ==
         Sculptor::osc_band_first_slot + 2 * Sculptor::osc_band_block_stride);
}

// Every band header carries [-]/[+] push buttons that stay visible while
// their band exists and gray at the count bounds: all [-] disable at one
// band, all [+] at four, both editable in between.
static void m2_band_button_gray_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, false)) {
        return;
    }
    const uint32_t osc_node = mapping.osc_nodes[0];
    const auto     click    = [&](const uint32_t slot) {
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        return apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, slot, press);
    };
    const auto gray = [&](const bool plus_disabled, const bool minus_disabled, const uint32_t count) {
        for (uint32_t band = 0; band < count; band++) {
            const uint32_t header = Sculptor::osc_band_header_slot(band);
            TEST(graph.slot_edit_disabled(osc_node, header + 2) == plus_disabled);
            TEST(graph.slot_edit_disabled(osc_node, header + 1) == minus_disabled);
            TEST(! Sculptor::osc_band_slot_hidden(graph, osc_node, header + 1));
            TEST(! Sculptor::osc_band_slot_hidden(graph, osc_node, header + 2));
        }
    };
    // One band: every [-] grays, [+] stays live.
    gray(false, true, 1);
    // Three appends reach the four-band cap: every [+] grays there.
    for (uint32_t count = 2; count <= 4; count++) {
        TEST(click(Sculptor::osc_band_header_slot(count - 2) + 2));
        TEST(bank.bank.instruments.entries[0].layers[0].fir_band_count == count);
        // The edit-disabled flags restamp on the structural re-projection.
        TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
        gray(count == 4, false, count);
    }
    // One remove re-enables [+] and keeps [-] live.
    TEST(click(Sculptor::osc_band_header_slot(3) + 1));
    TEST(bank.bank.instruments.entries[0].layers[0].fir_band_count == 3);
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    gray(false, false, 3);
}

// Every band parameter of every band carries the standard four modulator
// rows, the row bounds follow the per-parameter ranges (offset -36..36,
// spread 0..36, gain 0.01..1, slope 1..100), and the compiled bindings
// carry base and wiring per band.
static void m2_per_band_modulation_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank          = *bank_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::OscGraphMapping                          mapping       = {};
    if (! begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, true, false)) {
        return;
    }
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    if (mapping.osc_nodes[0] == Sculptor::pool_no_slot) {
        return;
    }
    const uint32_t     osc_node    = mapping.osc_nodes[0];
    static const float bases[4][4] = {
        { 1.0f, 6.0f, 0.9f, 40.0f },
        { 4.0f, 9.0f, 0.7f, 50.0f },
        { 7.0f, 12.0f, 0.5f, 60.0f },
        { 10.0f, 15.0f, 0.3f, 70.0f },
    };

    static const struct {
        float min_value;
        float max_value;
    } bounds[4] = {
        { -36.0f, 36.0f },
        { 0.0f, 36.0f },
        { 0.01f, 1.0f },
        { 1.0f, 100.0f },
    };

    for (uint32_t band = 0; band < 4; band++) {
        for (uint32_t p = 0; p < 4; p++) {
            const uint32_t                target = m2_band_targets[band][p];
            const Sculptor::OscTargetView view   = Sculptor::osc_target_view(static_cast<Synth::ModTarget>(target));
            TEST(view.min_value == bounds[p].min_value && view.max_value == bounds[p].max_value);
            const uint32_t row = Sculptor::osc_target_row(target);
            TEST(graph.node(osc_node).slots.entries[row].property_type ==
                 (p == 3 ? Sculptor::PropertyType::integer : Sculptor::PropertyType::real));
            Sculptor::PropertyValue base = {};
            if (p == 3) {
                base.integer = static_cast<int32_t>(bases[band][p]);
            }
            else {
                base.real = bases[band][p];
            }
            TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, row, base));
            TEST(wire_m2_target(&bank, &graph, &mapping, osc_node, target));
            TEST(Sculptor::refresh_osc_graph_compilation(&bank, graph, mapping, 0, 0));
            const Synth::TargetBinding& gen = bank.bank.instruments.entries[0].layers[0].targets[target];
            TEST(gen.base_value == bases[band][p]);
            TEST(gen.envelope_desc_id == 2);
            TEST(gen.lfo_desc_id == 1);
            TEST(gen.num_inputs == 2);
            TEST(gen.inputs[0].source == Synth::ModSource::velocity);
            TEST(gen.inputs[1].source == Synth::ModSource::mod_wheel);
        }
    }

    // The wiring survives the reprojection for every band.
    TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
    for (uint32_t band = 0; band < 4; band++) {
        const uint32_t              target = m2_band_targets[band][3];
        const Synth::TargetBinding& gen    = bank.bank.instruments.entries[0].layers[0].targets[target];
        TEST(gen.envelope_desc_id == 2 && gen.lfo_desc_id == 1 && gen.num_inputs == 2);
        TEST(gen.base_value == bases[band][3]);
    }
}

// Complex response of a binary32 kernel at freq (causal tap phase), the
// complex counterpart of fir_test_response.
static void fir_test_complex_response(const float* coefficients, const double freq, double* real, double* imag)
{
    constexpr uint32_t taps = Synth::num_fir_taps;
    const double       w    = 2.0 * 3.141592653589793 * freq / static_cast<double>(Synth::rt_sampling_rate);
    double             re   = 0.0;
    double             im   = 0.0;
    for (uint32_t tap = 0; tap < taps; tap++) {
        const double c = static_cast<double>(coefficients[tap]);
        if (c == 0.0) {
            continue;
        }
        const double phase = w * static_cast<double>(tap);
        re += c * cos(phase);
        im -= c * sin(phase);
    }
    *real = re;
    *imag = im;
}

// C2.3: the multi-band kernel accumulation.  Each band's kernel is the
// single-band mirror scaled by the band gain and normalized by the band's own
// denominator; the staged taps hold the complex sum, so overlapping bands
// with unequal slopes interfere instead of adding in magnitude.
static void m2_fir_kernel_mirror_regressions()
{
    constexpr uint32_t                    taps              = Synth::num_fir_taps;
    const std::unique_ptr<FirCoeffMirror> band_storage      = std::make_unique<FirCoeffMirror>();
    FirCoeffMirror&                       band              = *band_storage;
    const std::unique_ptr<FirCoeffMirror> other_storage     = std::make_unique<FirCoeffMirror>();
    FirCoeffMirror&                       other             = *other_storage;
    float                                 stored[taps]      = {};
    float                                 band_stored[taps] = {};

    // Per-band staging math mirror: the edges come from the production edge
    // composition (center pitch + offset, +/- spread, the 1 Hz highpass floor
    // and the shared guard) so a single band at gain 1 reduces to the
    // single-band kernel.
    const auto band_edges = [](const float    offset_semitones,
                               const float    spread,
                               const uint32_t mult,
                               uint32_t*      lowpass,
                               uint32_t*      highpass) {
        float lowpass_edge  = 0.0f;
        float highpass_edge = 0.0f;
        TEST(Synth::oscillator_fir_cutoffs(69.0f + offset_semitones,
                                           spread,
                                           mult,
                                           Synth::fir_bandpass,
                                           &lowpass_edge,
                                           &highpass_edge));
        *lowpass  = fir_test_quantize_cutoff(lowpass_edge);
        *highpass = fir_test_quantize_cutoff(highpass_edge);
        Synth::oscillator_fir_band_guard(lowpass, highpass);
        return Synth::pitch_to_frequency(69.0f + offset_semitones, mult);
    };

    // Single-band equivalence: one band at offset 0 with gain 1 keeps the
    // single-band stored response across spans.
    for (const uint32_t span : { 11u, 513u, Synth::num_fir_taps }) {
        uint32_t lowpass  = 0;
        uint32_t highpass = 0;
        (void)band_edges(0.0f, 6.0f, 1, &lowpass, &highpass);
        fir_test_mirror(lowpass, highpass, span, 256, 32, &band);
        const double magnitude = fir_test_stored_response(band, static_cast<double>(band.center_freq), stored);
        TEST(fabs(magnitude - 1.0) <= fir_test_unity_tolerance(lowpass, highpass));
    }

    // Three separated bands peak at each band's center frequency, scaled by
    // that band's gain.
    {
        struct BandSpec {
            float offset;
            float spread;
            float slope;
            float gain;
        };

        static const BandSpec specs[3] = {
            { -36.0f, 6.0f, 100.0f, 1.0f },
            { 0.0f, 6.0f, 100.0f, 0.5f },
            { 36.0f, 6.0f, 100.0f, 0.25f },
        };
        static float band_kernels[3][taps];
        uint32_t     band_index = 0;
        double       centers[3] = {};
        memset(static_cast<void*>(stored), 0, sizeof(stored));
        for (const BandSpec& spec : specs) {
            uint32_t lowpass    = 0;
            uint32_t highpass   = 0;
            centers[band_index] = static_cast<double>(band_edges(spec.offset, spec.spread, 1, &lowpass, &highpass));
            const uint32_t span = Synth::fir_effective_span(spec.slope);
            fir_test_mirror(lowpass, highpass, span, 256, 32, &band);
            fir_test_check_span_shape(band, span);
            const double own = fir_test_stored_response(band, static_cast<double>(band.center_freq), band_stored);
            TEST(fabs(own - 1.0) <= fir_test_unity_tolerance(lowpass, highpass));
            memcpy(band_kernels[band_index], band_stored, sizeof(band_kernels[0]));
            for (uint32_t tap = 0; tap < taps; tap++) {
                stored[tap] = stored[tap] + band_stored[tap] * spec.gain;
            }
            band_index++;
        }
        for (uint32_t probe = 0; probe < 3; probe++) {
            double expected_re = 0.0;
            double expected_im = 0.0;
            for (uint32_t j = 0; j < 3; j++) {
                double re = 0.0;
                double im = 0.0;
                fir_test_complex_response(band_kernels[j], centers[probe], &re, &im);
                expected_re += re * static_cast<double>(specs[j].gain);
                expected_im += im * static_cast<double>(specs[j].gain);
            }
            const double expected = sqrt(expected_re * expected_re + expected_im * expected_im);
            const double measured = fir_test_response(stored, centers[probe]);
            TEST(fabs(measured - expected) <= 1e-3 * expected + 1e-3);
        }
    }

    // Overlapping bands with unequal slopes sum as complex responses: the
    // staged kernel equals the complex sum of the per-band kernels, and the
    // magnitude does not add.
    {
        uint32_t       lowpass0  = 0;
        uint32_t       highpass0 = 0;
        const double   center0   = static_cast<double>(band_edges(0.0f, 6.0f, 1, &lowpass0, &highpass0));
        uint32_t       lowpass1  = 0;
        uint32_t       highpass1 = 0;
        const double   center1   = static_cast<double>(band_edges(2.0f, 6.0f, 1, &lowpass1, &highpass1));
        const uint32_t span0     = Synth::fir_effective_span(100.0f);
        const uint32_t span1     = Synth::fir_effective_span(1.0f);
        fir_test_mirror(lowpass0, highpass0, span0, 256, 32, &band);
        fir_test_check_span_shape(band, span0);
        fir_test_stored_response(band, center0, band_stored);
        float kernel0[taps] = {};
        memcpy(kernel0, band_stored, sizeof(kernel0));
        fir_test_mirror(lowpass1, highpass1, span1, 256, 32, &other);
        fir_test_check_span_shape(other, span1);
        fir_test_stored_response(other, center1, band_stored);
        for (uint32_t tap = 0; tap < taps; tap++) {
            stored[tap] = kernel0[tap] + band_stored[tap];
            TEST(std::isfinite(stored[tap]));
        }
        const double probe_freqs[3] = { 500.0, 550.0, 600.0 };
        for (const double freq : probe_freqs) {
            double re0 = 0.0;
            double im0 = 0.0;
            double re1 = 0.0;
            double im1 = 0.0;
            fir_test_complex_response(kernel0, freq, &re0, &im0);
            fir_test_complex_response(band_stored, freq, &re1, &im1);
            const double expected_re        = re0 + re1;
            const double expected_im        = im0 + im1;
            const double expected_magnitude = sqrt(expected_re * expected_re + expected_im * expected_im);
            const double measured           = fir_test_response(stored, freq);
            TEST(fabs(measured - expected_magnitude) <= 1e-3 * expected_magnitude + 1e-6);
            const double magnitude_sum = sqrt(re0 * re0 + im0 * im0) + sqrt(re1 * re1 + im1 * im1);
            TEST(fabs(magnitude_sum - expected_magnitude) > 1e-3);
        }
    }
}

// ---- FIR effect multi-band staging, expansion, JSON and remap ----
static void fir_effect_bands_regressions()
{
    // Staging: the legacy single-band form (and the zeroed legacy count) stages
    // unconditionally with the gain forced to 1.0, so legacy banks keep their
    // bit-exact coefficient entries.
    {
        Synth::FirEffectCoeffEntry entry                                          = {};
        const float                band_params[Synth::num_fir_effect_band_params] = {};
        TEST(Synth::stage_fir_effect_coeff_entry(1234, 1, 8000.0f, 0.0f, 100.0f, band_params, &entry));
        TEST(entry.taps_offs == 1234);
        TEST(entry.band_count == 1);
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(8000.0f));
        TEST(entry.bands[0].highpass_cutoff_freq == 0);
        TEST(entry.bands[0].window_span == Synth::fir_effective_span(100.0f));
        TEST(entry.bands[0].gain == 1.0f); // the legacy gain stays forced
        TEST(Synth::stage_fir_effect_coeff_entry(0, 0, 8000.0f, 0.0f, 100.0f, band_params, &entry));
        TEST(entry.band_count == 1);
        TEST(entry.bands[0].gain == 1.0f);
    }
    // Multi-band staging: bands compact in activation order, gains clamp to
    // [0, 1] with nonpositive-or-NaN staging a zero-gain band, and an
    // all-inactive configuration drops the entry entirely.
    {
        Synth::FirEffectCoeffEntry entry                                          = {};
        float                      band_params[Synth::num_fir_effect_band_params] = {};
        band_params[0] = 2.0f; // band 0 gain clamps down to 1
        // Band 1: lowpass 4000 Hz, highpass 200 Hz, gain 0.5, slope 50.
        band_params[1] = 4000.0f;
        band_params[2] = 200.0f;
        band_params[3] = 0.5f;
        band_params[4] = 50.0f;
        // Band 2: lowpass 0 -> the highpass-only form is inactive and skips.
        // Band 3: lowpass 3000 Hz, highpass 1000 Hz, negative gain, slope 33.
        band_params[9]  = 3000.0f;
        band_params[10] = 1000.0f;
        band_params[11] = -1.0f;
        band_params[12] = 33.0f;
        TEST(Synth::stage_fir_effect_coeff_entry(9, 4, 8000.0f, 0.0f, 100.0f, band_params, &entry));
        TEST(entry.band_count == 3); // the inactive band compacts out
        TEST(entry.bands[0].gain == 1.0f);
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(8000.0f));
        TEST(entry.bands[1].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(4000.0f));
        TEST(entry.bands[1].highpass_cutoff_freq == Synth::cutoff_hz_to_int(200.0f));
        TEST(entry.bands[1].gain == 0.5f);
        TEST(entry.bands[1].window_span == Synth::fir_effective_span(50.0f));
        TEST(entry.bands[2].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(3000.0f));
        TEST(entry.bands[2].highpass_cutoff_freq == Synth::cutoff_hz_to_int(1000.0f));
        TEST(entry.bands[2].gain == 0.0f); // the negative gain stages a zero-gain band
        TEST(entry.bands[2].window_span == Synth::fir_effective_span(33.0f));
        // A NaN gain reads as a zero gain, mirroring the oscillator's band loop.
        band_params[11] = 0.0f;
        band_params[11] = band_params[11] / band_params[11];
        TEST(Synth::stage_fir_effect_coeff_entry(9, 4, 8000.0f, 0.0f, 100.0f, band_params, &entry));
        TEST(entry.bands[2].gain == 0.0f);
        // Every band inactive: the entry drops so the taps keep their last kernel.
        const float quiet_params[Synth::num_fir_effect_band_params] = {};
        TEST(! Synth::stage_fir_effect_coeff_entry(9, 4, 0.0f, 0.0f, 100.0f, quiet_params, &entry));
        TEST(! Synth::stage_fir_effect_coeff_entry(9, 3, 0.0f, 0.0f, 100.0f, quiet_params, &entry));
    }
    // Builds a small valid bank: one envelope, one LFO, one instrument, one zone.
    auto build_bank_fixture = [](Synth::InstrumentBank& bank) { build_small_bank_fixture(bank); };
    // Expansion: active band params seed the instance's band values and commit
    // one node per modulated band parameter, exactly like scalar params.
    {
        Synth::InstrumentBank bank = {};
        build_bank_fixture(bank);
        Synth::EffectSlotBinding* const fir = enabled_effect(bank, 0, 0, Synth::EffectType::fir);
        fir->bindings[0].base_value         = 8000.0f;
        fir->bindings[1].base_value         = 0.0f;
        fir->bindings[2].base_value         = 100.0f;
        fir->fir_bands.band_count           = 3;
        fir->fir_bands.bands[0].base_value  = 0.5f; // band 0's gain
        Synth::EffectParamBinding* band1    = fir->fir_bands.bands + 1;
        band1[0].base_value                 = 4000.0f;
        band1[1].base_value                 = 200.0f;
        band1[2].base_value                 = 0.8f;
        band1[2].lfo_desc_id                = 1;
        band1[2].lfo_depth                  = 0.5f;
        band1[3].base_value                 = 50.0f;
        Synth::EffectParamBinding* band2    = fir->fir_bands.bands + 5;
        band2[0].base_value                 = 3000.0f;
        band2[1].base_value                 = 1000.0f;
        band2[2].base_value                 = 1.0f;
        band2[3].base_value                 = 100.0f;
        TEST(Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        // Nine active band params, one modulated: dest node + LFO leaf.
        TEST(plan.num_nodes == 2);
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        const Synth::EffectInstance& instance = chains[0].effects[0];
        TEST(instance.fir_bands.band_count == 3);
        TEST(instance.fir_bands.params[0] == 0.5f);
        TEST(instance.fir_bands.params[1] == 4000.0f);
        TEST(instance.fir_bands.params[2] == 200.0f);
        TEST(instance.fir_bands.params[3] == 0.8f);
        TEST(instance.fir_bands.params[4] == 50.0f);
        TEST(instance.fir_bands.params[5] == 3000.0f);
        TEST(instance.fir_bands.params[6] == 1000.0f);
        TEST(instance.fir_bands.params[7] == 1.0f);
        TEST(instance.fir_bands.params[8] == 100.0f);
        TEST(instance.fir_bands.src_param_id[3] == 50); // the modulated band's dest node
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        TEST(writer.last_dest_lfo_node == 51);
        for (uint32_t band_param = 0; band_param < Synth::num_fir_effect_band_params; band_param++) {
            if (band_param != 3) {
                TEST(instance.fir_bands.src_param_id[band_param] == 0); // unwired bands resolve nothing
            }
        }
        (void)Synth::take_effect_clear_ranges();
    }
    // Refusals: a count beyond the array bound rejects at validation, and the
    // preflight refuses the oversized count.
    {
        Synth::InstrumentBank bank = {};
        build_bank_fixture(bank);
        Synth::EffectSlotBinding* const fir = enabled_effect(bank, 0, 0, Synth::EffectType::fir);
        fir->fir_bands.band_count           = Synth::max_fir_effect_bands + 1;
        TEST(! Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(error && strstr(error, "too many bands") != nullptr);
        // A dormant band binding is ignored like the params tail (remap and
        // canonicalize zero it), so wiring one stays valid in memory.
        fir->fir_bands.band_count           = 3;
        fir->fir_bands.bands[9].lfo_desc_id = 1; // band 3's lowpass is dormant at count 3
        TEST(Synth::validate_instrument_bank(&bank));
    }
    // Bank JSON: multi-band FIR slots carry the band count and per-band
    // bindings; the legacy single-band form encodes exactly as before.
    {
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        Synth::EffectSlotBinding* const fir = enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        fir->bindings[2].base_value         = 100.0f; // a valid legacy slope keeps the round trip byte-stable
        fir->fir_bands.band_count           = 3;
        fir->fir_bands.bands[0].base_value  = 0.5f;
        Synth::EffectParamBinding* band1    = fir->fir_bands.bands + 1;
        band1[0].base_value                 = 4000.0f;
        band1[1].base_value                 = 200.0f;
        band1[2].base_value                 = 0.0f; // an explicit zero gain survives the round trip
        band1[2].lfo_desc_id                = 1;
        band1[3].base_value                 = 50.0f;
        Synth::EffectParamBinding* band2    = fir->fir_bands.bands + 5;
        band2[0].base_value                 = 3000.0f;
        band2[1].base_value                 = 1000.0f;
        band2[3].base_value                 = 100.0f;
        const uint32_t length               = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        TEST(strstr(text, "\"fir_bands\":3") != nullptr);
        TEST(strstr(text, "\"fir_band1\"") != nullptr);
        TEST(strstr(text, "\"fir_band2\"") != nullptr);
        TEST(strstr(text, "\"fir_band0_gain\"") != nullptr);
        TEST(strstr(text, "\"fir_band3\"") == nullptr); // the dormant band stays out
        TEST(Synth::decode_editor_bank_json(text, length, &loaded));
        TEST(memcmp(loaded.bank.channel_chains[0].effects[0].fir_bands.bands,
                    fir->fir_bands.bands,
                    sizeof(fir->fir_bands.bands)) == 0);
        TEST(loaded.bank.channel_chains[0].effects[0].fir_bands.band_count == 3);
        TEST(Synth::validate_instrument_bank(&loaded.bank));
        // Re-encoding the decoded bank reproduces the same bytes.
        const uint32_t reencoded =
            Synth::encode_editor_bank_json(&loaded, text + length + 1, sizeof(text) - length - 1);
        TEST(reencoded == length);
        TEST(memcmp(text, text + length + 1, length) == 0);
    }
    // The legacy single-band form keeps its zeroed band block through the JSON
    // round trip, so byte-comparable bank snapshots survive.
    {
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        const uint32_t length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        TEST(strstr(text, "fir_bands") == nullptr); // band count 1 encodes nothing
        TEST(Synth::decode_editor_bank_json(text, length, &loaded));
        const uint8_t zeroed[sizeof(loaded.bank.channel_chains[0].effects[0].fir_bands)] = {};
        TEST(memcmp(&loaded.bank.channel_chains[0].effects[0].fir_bands, zeroed, sizeof(zeroed)) == 0);
    }
    // Band keys refuse on non-FIR slots and an oversized count refuses.
    {
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::delay);
        const uint32_t length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        const char* anchor = strstr(text, "\"type\":\"delay\"");
        TEST(anchor != nullptr);
        const uint32_t anchor_at     = static_cast<uint32_t>(anchor - text);
        const char*    injection     = ",\"fir_bands\":2";
        const uint32_t injection_len = static_cast<uint32_t>(strlen(injection));
        TEST(anchor_at + injection_len + (length - anchor_at) + 1 <= sizeof(text));
        memmove(text + anchor_at + injection_len, text + anchor_at, length - anchor_at + 1);
        memcpy(text + anchor_at, injection, injection_len);
        TEST(! Synth::decode_editor_bank_json(text, length + injection_len, &loaded));
    }
    {
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        const uint32_t length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        const char* anchor = strstr(text, "\"type\":\"fir\"");
        TEST(anchor != nullptr);
        const uint32_t anchor_at     = static_cast<uint32_t>(anchor - text);
        const char*    injection     = ",\"fir_bands\":5";
        const uint32_t injection_len = static_cast<uint32_t>(strlen(injection));
        TEST(anchor_at + injection_len + (length - anchor_at) + 1 <= sizeof(text));
        memmove(text + anchor_at + injection_len, text + anchor_at, length - anchor_at + 1);
        memcpy(text + anchor_at, injection, injection_len);
        TEST(! Synth::decode_editor_bank_json(text, length + injection_len, &loaded));
    }
    // Cloning remaps active band LFO references through the same lookup and
    // zeroes dormant bands rather than trusting stale bytes.
    {
        Synth::EffectChainBinding src                 = {};
        src.num_effects                               = 1;
        src.effects[0].type                           = Synth::EffectType::fir;
        src.effects[0].fir_bands.band_count           = 2;
        src.effects[0].fir_bands.bands[1].lfo_desc_id = 1; // band 1's lowpass
        src.effects[0].fir_bands.bands[2].lfo_desc_id = 2; // band 1's highpass
        src.effects[0].fir_bands.bands[5].lfo_desc_id = 3; // dormant band 2's lowpass
        const uint16_t            lfo_ids[]           = { 10, 11, 12 };
        Synth::EffectChainBinding dst                 = {};
        Synth::remap_effect_chain(src, lfo_ids, &dst);
        TEST(dst.effects[0].fir_bands.bands[1].lfo_desc_id == 10);
        TEST(dst.effects[0].fir_bands.bands[2].lfo_desc_id == 11);
        TEST(dst.effects[0].fir_bands.bands[5].lfo_desc_id == 0);
        TEST(dst.effects[0].fir_bands.bands[9].lfo_desc_id == 0);
        TEST(dst.effects[0].fir_bands.band_count == 2);
        // The legacy single-band form zeroes every band binding, including
        // band 0's gain (the staging ignores it at count 1).
        src.effects[0].fir_bands.band_count           = 1;
        src.effects[0].fir_bands.bands[0].lfo_desc_id = 2;
        Synth::remap_effect_chain(src, lfo_ids, &dst);
        TEST(dst.effects[0].fir_bands.bands[0].lfo_desc_id == 0);
    }
}

// Wave-2 band-editor regressions, restored to the pre-M4 contract: no
// current effect type projects the FIR band region, so a fir node renders
// exactly its legacy rows (Lowpass Hz, Highpass Hz, FIR Slope, Enabled)
// for every stored band count and every highpass edge. The band machinery
// stays dormant behind the fx_band_region_projected gate and stays covered
// at the model and seam level, ready for the future Multi-band FIR effect
// node to re-adopt.
static void fir_effect_band_editor_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate     = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> work_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       work          = *work_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping       = {};

    // Builds the model side: one FIR effect on channel 0 with the authored
    // Highpass Hz edge and band count, band 1 seeded with distinct values
    // for copy/remove coverage, plus one LFO descriptor the band
    // modulation references.
    const auto build = [&](const uint32_t band_count, const float highpass_hz) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const uint32_t lfo = source.bank.lfos.allocate();
        {
            Synth::LFODescriptor& descriptor = source.bank.lfos.entries[lfo];
            descriptor.wave                  = Synth::WaveType::sine_wave;
            descriptor.period_ms             = 300;
            descriptor.min_value             = -1.0f;
            descriptor.min_max_delta         = 2.0f;
        }
        auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::fir);
        chain.effects[0].bindings[0].base_value = 8000.0f; // lowpass
        chain.effects[0].bindings[1].base_value = highpass_hz;
        chain.effects[0].bindings[2].base_value = 100.0f; // slope
        chain.effects[0].fir_bands.band_count   = static_cast<uint8_t>(band_count);
        Synth::EffectParamBinding* band1        = chain.effects[0].fir_bands.bands + 1;
        band1[0].base_value                     = 4000.0f;
        band1[1].base_value                     = 200.0f;
        band1[2].base_value                     = 0.5f;
        band1[3].base_value                     = 50.0f;
    };
    const uint32_t lfo_desc  = 1;
    const auto     reproject = [&]() {
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        Sculptor::GraphChange drained[Sculptor::max_pending_changes];
        graph.take_changes(drained, Sculptor::max_pending_changes);
    };
    const auto node          = [&]() { return mapping.effect_nodes[0]; };
    const auto chain_effects = [&]() -> Synth::EffectSlotBinding& {
        return Sculptor::fx_graph_chain(&source.bank, 0).effects[0];
    };
    // The direct-change seam: stages (Graph::set_slot_value is
    // deliberately eventless), then applies the matching value_changed
    // event through the production apply function.
    const auto fx_edit = [&](const uint32_t slot_idx, const Sculptor::PropertyValue value) {
        graph.set_slot_value(node(), slot_idx, value);
        Sculptor::GraphChange change = {};
        change.kind                  = Sculptor::ChangeKind::value_changed;
        change.node_idx              = node();
        change.slot_idx              = slot_idx;
        candidate                    = source;
        const bool applied           = Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change);
        TEST(applied);
        if (applied) {
            source = candidate;
        }
        return applied;
    };
    // The apply-layer mirror of the production drain's structural-event
    // boundary (drain_fx_graph's band-click scan): the batch is truncated
    // after the first FIR band-button event and the rest is discarded.
    const auto drain_fx_batch = [&](const Sculptor::GraphChange* changes, const uint32_t count, const bool boundary) {
        bool           band_click = false;
        const uint32_t apply_count =
            boundary ? Sculptor::fx_band_click_boundary(changes, count, mapping, source.bank, &band_click) : count;
        (void)band_click;
        work = source;
        const bool ok =
            Sculptor::apply_effect_graph_batch(&source, &graph, &mapping, changes, apply_count, nullptr, &work);
        TEST(ok);
        if (ok) {
            source = work;
        }
        return count - apply_count;
    };

    // Legacy-only projection, pure lowpass: exactly the 33 legacy slots.
    build(1, 0.0f);
    reproject();
    TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);

    // Legacy-only projection under bandpass-authored state too: the
    // fx_band_region_projected gate never opens for any current effect
    // type, whatever the stored band count.
    build(1, 200.0f);
    chain_effects().fir_bands.bands[0].base_value = 0.7f;
    reproject();
    TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
    build(4, 200.0f);
    reproject();
    TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);

    // The legacy rows sit in place, in order, with the legacy renderer
    // line ids: three parameter blocks (row groups 2/3/4, source rows
    // 8..13) then the Enabled row.
    {
        const Sculptor::Node& fir            = graph.node(node());
        uint32_t              legacy_lowpass = 0;
        TEST(find_graph_slot(graph, node(), "Lowpass Hz", &legacy_lowpass));
        TEST(legacy_lowpass == Sculptor::fx_param_row(0));
        uint32_t legacy_highpass = 0;
        TEST(find_graph_slot(graph, node(), "Highpass Hz", &legacy_highpass));
        TEST(legacy_highpass == Sculptor::fx_param_row(1));
        uint32_t legacy_slope = 0;
        TEST(find_graph_slot(graph, node(), "FIR Slope", &legacy_slope));
        TEST(legacy_slope == Sculptor::fx_param_row(2));
        const Sculptor::Slot& slope = fir.slots.entries[Sculptor::fx_param_row(2)];
        TEST(slope.property_type == Sculptor::PropertyType::integer);
        TEST(slope.int_bounded && slope.real_min == 1.0f && slope.real_max == 100.0f);
        TEST(slope.value.integer == 100);
        const Sculptor::Slot& enabled = fir.slots.entries[Sculptor::fx_enabled_row];
        TEST(enabled.property_type == Sculptor::PropertyType::toggle);
    }

    // The display filter answers legacy for fir nodes: the base rows
    // always render and their unconnected modulator rows hide, exactly
    // like the pre-M4 legacy parameter rows.
    {
        const Synth::EffectSlotBinding& effect = chain_effects();
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(0)) == false);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(0) + 1) == true);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(0) + 7) == true);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(1)) == false);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(1) + 1) == true);
    }

    // The drop-menu and wiring validator decode nothing band-shaped on a
    // fir node: every projected slot decodes to a legacy row (fir_row 0..2
    // into bindings[]) or to no binding at all (the Enabled row).
    {
        const Synth::EffectSlotBinding& effect = chain_effects();
        for (uint32_t slot_idx = 0; slot_idx < Sculptor::fx_band_first_slot; ++slot_idx) {
            Synth::EffectParamBinding* binding  = nullptr;
            uint32_t                   row_base = 0;
            uint32_t                   field    = 0;
            uint32_t                   fir_row  = 0;
            if (! Sculptor::fx_decode_row_slot(chain_effects(), slot_idx, &binding, &row_base, &field, &fir_row)) {
                continue; // the Enabled row and the audio endpoints
            }
            TEST(fir_row <= 3);
            TEST(binding >= &effect.bindings[0] && binding <= &effect.bindings[2]);
            TEST(row_base < Sculptor::fx_band_first_slot);
        }
    }

    // Dormant band machinery, seam-level: [+] copies values and modulation
    // bindings into the new band even though no band row projects.
    build(1, 200.0f);
    {
        Synth::EffectParamBinding& gain0 = chain_effects().fir_bands.bands[0];
        gain0.base_value                 = 0.7f;
        gain0.lfo_desc_id                = lfo_desc;
        gain0.lfo_depth                  = 0.5f;
        gain0.num_inputs                 = 1;
        gain0.inputs[0]                  = { Synth::ModSource::mod_wheel, Synth::SourceOp::add, 0, 0.25f };
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        // The band-0 gain's LFO reference still projects the LFO node (the
        // dormant registry rule the Multi-band FIR node will reuse).
        TEST(mapping.lfo_nodes[lfo_desc - 1] != Sculptor::pool_no_slot);
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_first_slot + 2, press));
        TEST(chain_effects().fir_bands.band_count == 2);
        Synth::EffectParamBinding* bands = chain_effects().fir_bands.bands;
        TEST(bands[1].base_value == 8000.0f);
        TEST(bands[2].base_value == 200.0f);
        TEST(bands[3].base_value == 0.7f);
        TEST(bands[3].lfo_desc_id == lfo_desc && bands[3].lfo_depth == 0.5f);
        TEST(bands[3].num_inputs == 1 && bands[3].inputs[0].scale == 0.25f);
        TEST(bands[4].base_value == 100.0f);
        TEST(bands[0].base_value == 0.7f); // the copied band keeps its bytes
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        // [-] on band 0 removes it: the old band 1 becomes band 0 (its
        // bytes land in the legacy rows) and the tail block zeroes.
        Synth::EffectParamBinding old_band1[4];
        memcpy(old_band1, bands + 1, sizeof(old_band1));
        TEST(fx_edit(Sculptor::fx_band_first_slot + 1, press));
        TEST(chain_effects().fir_bands.band_count == 1);
        TEST(chain_effects().bindings[0].base_value == old_band1[0].base_value);
        TEST(chain_effects().bindings[1].base_value == old_band1[1].base_value);
        TEST(chain_effects().bindings[2].base_value == old_band1[3].base_value);
        TEST(memcmp(static_cast<const void*>(&chain_effects().fir_bands.bands[0]),
                    static_cast<const void*>(&old_band1[2]),
                    sizeof(Synth::EffectParamBinding)) == 0);
        const Synth::EffectParamBinding zero = {};
        TEST(memcmp(static_cast<const void*>(&chain_effects().fir_bands.bands[1]),
                    static_cast<const void*>(&zero),
                    sizeof(Synth::EffectParamBinding)) == 0);
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
    }

    // [-] between bands shifts later bands up and zeroes the tail.
    build(4, 200.0f);
    {
        Synth::EffectParamBinding* bands = chain_effects().fir_bands.bands;
        bands[5].base_value              = 3000.0f; // band 2
        bands[6].base_value              = 1000.0f;
        bands[7].base_value              = 0.4f;
        bands[8].base_value              = 40.0f;
        bands[9].base_value              = 2000.0f; // band 3
        bands[10].base_value             = 1500.0f;
        bands[11].base_value             = 0.3f;
        bands[12].base_value             = 30.0f;
        Synth::EffectParamBinding old_band2[4];
        Synth::EffectParamBinding old_band3[4];
        memcpy(old_band2, bands + 5, sizeof(old_band2));
        memcpy(old_band3, bands + 9, sizeof(old_band3));
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir, 1) + 1, press));
        TEST(chain_effects().fir_bands.band_count == 3);
        for (uint32_t p = 0; p < 4; ++p) {
            TEST(bands[1 + p].base_value == old_band2[p].base_value);
            TEST(bands[5 + p].base_value == old_band3[p].base_value);
        }
        const Synth::EffectParamBinding zero = {};
        for (uint32_t p = 0; p < 4; ++p) {
            TEST(memcmp(static_cast<const void*>(&bands[9 + p]),
                        static_cast<const void*>(&zero),
                        sizeof(Synth::EffectParamBinding)) == 0);
        }
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
    }

    // Count-bound no-ops: the fifth add and the remove at one band.
    build(1, 200.0f);
    {
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_first_slot + 1, press));
        TEST(chain_effects().fir_bands.band_count == 1);
    }
    build(4, 200.0f);
    {
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir, 0) + 2, press));
        TEST(chain_effects().fir_bands.band_count == 4);
    }

    // Mirror: [+] on a zeroed unwired band 0 seeds the copy's gain to
    // 1.0 (an audible zero copy would be a silent surprise).
    build(1, 200.0f);
    {
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_first_slot + 2, press));
        TEST(chain_effects().fir_bands.band_count == 2);
        TEST(chain_effects().fir_bands.bands[3].base_value == 1.0f);
        TEST(chain_effects().fir_bands.bands[0].base_value == 0.0f);
    }

    // Crossing into bandpass through the Highpass row writes no band-0
    // seed anymore: the restored legacy behavior leaves fir_bands
    // untouched, and the projection stays legacy-only.
    build(1, 0.0f);
    {
        Sculptor::PropertyValue highpass = {};
        highpass.real                    = 200.0f;
        TEST(fx_edit(Sculptor::fx_param_row(1), highpass));
        TEST(chain_effects().bindings[1].base_value == 200.0f);
        TEST(chain_effects().fir_bands.bands[0].base_value == 0.0f);
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
    }

    // The dormant boundary scan, pinned directly: with no band rows
    // projected, the batch path never delivers a band click (unallocated
    // slot changes are skipped), so the truncation arithmetic is exercised
    // at the scan itself.
    build(1, 200.0f);
    reproject();
    {
        Sculptor::GraphChange click = {};
        click.kind                  = Sculptor::ChangeKind::value_changed;
        click.node_idx              = node();
        click.slot_idx              = Sculptor::fx_band_first_slot + 2;
        bool band_click             = false;
        TEST(Sculptor::fx_band_click_boundary(&click, 1, mapping, source.bank, &band_click) == 1);
        TEST(band_click);
    }
    build(4, 200.0f);
    reproject();
    {
        Sculptor::GraphChange click = {};
        click.kind                  = Sculptor::ChangeKind::value_changed;
        click.node_idx              = node();
        click.slot_idx              = Sculptor::fx_band_header_slot(Synth::EffectType::fir, 1) + 1;
        bool band_click             = false;
        TEST(Sculptor::fx_band_click_boundary(&click, 1, mapping, source.bank, &band_click) == 1);
        TEST(band_click);
    }

    // Five-parameter effects overlap the band slot range: their edits at
    // band-button arithmetic never route into the band machinery and the
    // drain boundary scan never truncates their batches.
    {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::compressor);
        reproject();
        TEST(Sculptor::is_fx_band_button_slot(Synth::EffectType::fir, Sculptor::fx_param_row(3) + 2));
        const uint32_t op_slot = Sculptor::fx_param_row(3) + 2;
        TEST(graph.node(node()).slots.num_allocated > op_slot);
        Sculptor::PropertyValue op = {};
        op.integer                 = 1; // any op different from the default 0 (none wired)
        TEST(fx_edit(op_slot, op));
        TEST(chain_effects().type == Synth::EffectType::compressor);
        TEST(chain_effects().bindings[3].lfo_op == Synth::SourceOp::multiply);
        TEST(chain_effects().fir_bands.band_count == 0);
        Sculptor::PropertyValue press      = {};
        press.toggle                       = true;
        Sculptor::PropertyValue depth_edit = {};
        depth_edit.real                    = 0.3f;
        graph.set_slot_value(node(), op_slot, press);
        graph.set_slot_value(node(), Sculptor::fx_param_row(3) + 3, depth_edit);
        Sculptor::GraphChange batch[2] = {};
        batch[0].kind                  = Sculptor::ChangeKind::value_changed;
        batch[0].node_idx              = node();
        batch[0].slot_idx              = op_slot;
        batch[1]                       = batch[0];
        batch[1].slot_idx              = Sculptor::fx_param_row(3) + 3;
        TEST(drain_fx_batch(batch, 2, true) == 0);           // no truncation for non-FIR
        TEST(chain_effects().bindings[3].lfo_depth == 0.3f); // both edits applied
    }

    // LFO descriptor removal and the wired-CC registry see the band
    // bindings too: a band-referenced LFO stays projected and its CC
    // inputs count toward the chain mask.
    build(1, 200.0f);
    {
        Synth::EffectParamBinding& gain0 = chain_effects().fir_bands.bands[0];
        gain0.lfo_desc_id                = lfo_desc;
        gain0.num_inputs                 = 1;
        gain0.inputs[0]                  = { Synth::ModSource::cc, Synth::SourceOp::add, 16, 0.5f };
        reproject();
        TEST(mapping.lfo_nodes[lfo_desc - 1] != Sculptor::pool_no_slot);
        TEST((Sculptor::fx_chain_wired_cc_mask(Sculptor::fx_graph_chain(&source.bank, 0)) & 1u) != 0);
    }
}

// ---- Seam gates and state-loss regressions ----
// The plain FIR renders legacy rows only (the band region belongs to
// fir_multiband, covered by fir_multiband_projection_regressions); these
// pins keep the plain-FIR dormant seam: the band-aware decode, the
// out-of-count refusal, the corrupt-count remap clamp and band 0
// single-band round trip.
static void m4_fix3_wave_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate     = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> work_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       work          = *work_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph         = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping       = {};

    // Builds the model side: one FIR effect on channel 0 with the authored
    // Highpass Hz edge and band count, band 1 seeded with distinct values,
    // plus one LFO descriptor the band modulation references.
    const auto build = [&](const uint32_t band_count, const float highpass_hz) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const uint32_t        lfo        = source.bank.lfos.allocate();
        Synth::LFODescriptor& descriptor = source.bank.lfos.entries[lfo];
        descriptor.wave                  = Synth::WaveType::sine_wave;
        descriptor.period_ms             = 300;
        descriptor.min_value             = -1.0f;
        descriptor.min_max_delta         = 2.0f;
        auto& chain                      = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects                = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::fir);
        chain.effects[0].bindings[0].base_value = 8000.0f; // lowpass
        chain.effects[0].bindings[1].base_value = highpass_hz;
        chain.effects[0].bindings[2].base_value = 100.0f; // slope
        chain.effects[0].fir_bands.band_count   = static_cast<uint8_t>(band_count);
        Synth::EffectParamBinding* band1        = chain.effects[0].fir_bands.bands + 1;
        band1[0].base_value                     = 4000.0f;
        band1[1].base_value                     = 200.0f;
        band1[2].base_value                     = 0.5f;
        band1[3].base_value                     = 50.0f;
    };
    const uint32_t lfo_desc  = 1;
    const auto     reproject = [&]() {
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        Sculptor::GraphChange drained[Sculptor::max_pending_changes];
        graph.take_changes(drained, Sculptor::max_pending_changes);
    };
    const auto node          = [&]() { return mapping.effect_nodes[0]; };
    const auto chain_effects = [&]() -> Synth::EffectSlotBinding& {
        return Sculptor::fx_graph_chain(&source.bank, 0).effects[0];
    };

    // The display filter answers legacy rows for a five-parameter effect:
    // its rows share the band slot range, so the band region decode is
    // FIR-only.  In particular the fourth parameter's base row must render
    // and its unconnected modulator rows must hide.
    {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::compressor);
        reproject();
        const Synth::EffectSlotBinding& effect = chain_effects();
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(3)) == false);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(3) + 1) == true);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(3) + 7) == true);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(4)) == false);
        TEST(Sculptor::fx_effect_slot_hidden(effect, graph, node(), Sculptor::fx_param_row(4) + 1) == true);
    }

    // The wiring seams reach nothing band-shaped on a fir node: no band row
    // projects, so a wire aimed at a band row's LFO dot lands outside the
    // projected slot count and the apply refuses it without seeding the
    // dormant band binding.
    {
        build(2, 200.0f);
        chain_effects().fir_bands.bands[1].lfo_desc_id = lfo_desc;
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        const uint32_t lfo_node = mapping.lfo_nodes[lfo_desc - 1];
        TEST(lfo_node != Sculptor::pool_no_slot);
        Sculptor::GraphChange wire      = {};
        wire.kind                       = Sculptor::ChangeKind::connection_added;
        wire.connection_output.node_idx = lfo_node;
        wire.connection_output.slot_idx = 0;
        wire.connection_input.node_idx  = node();
        wire.connection_input.slot_idx  = Sculptor::fx_band_row_slot(Synth::EffectType::fir, 1, 2) + 1;
        candidate                       = source;
        TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, wire));
        TEST(candidate.bank.channel_chains[0].effects[0].fir_bands.bands[3].lfo_desc_id == 0);
    }

    // The same refusal for a MIDI source wire aimed at a band row's inline
    // dot: the projected slot count bounds the decode, so the band binding
    // stays untouched.
    {
        build(2, 200.0f);
        chain_effects().fir_bands.bands[1].lfo_desc_id = lfo_desc;
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        Sculptor::GraphChange wire      = {};
        wire.kind                       = Sculptor::ChangeKind::connection_added;
        wire.connection_output.node_idx = mapping.midi_node;
        wire.connection_output.slot_idx = mapping.midi_source_slots[0];
        wire.connection_input.node_idx  = node();
        wire.connection_input.slot_idx  = Sculptor::fx_band_row_slot(Synth::EffectType::fir, 1, 2);
        candidate                       = source;
        TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, wire));
        const Synth::EffectParamBinding& gain1 = candidate.bank.channel_chains[0].effects[0].fir_bands.bands[3];
        TEST(gain1.num_inputs == 0);
        TEST(gain1.lfo_desc_id == 0);
    }

    // The full-replay sweep replays a five-parameter effect's live rows:
    // the band-region skip is FIR-only, so compressor Release and Makeup
    // values land in the candidate at library-save and paste time.
    {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::compressor);
        reproject();
        Sculptor::PropertyValue release_value = {};
        release_value.real                    = 0.25f;
        graph.set_slot_value(node(), Sculptor::fx_param_row(3), release_value);
        Sculptor::PropertyValue makeup_value = {};
        makeup_value.real                    = 0.75f;
        graph.set_slot_value(node(), Sculptor::fx_param_row(4), makeup_value);
        work = source;
        TEST(Sculptor::apply_effect_graph_batch(&source, &graph, &mapping, nullptr, 0, nullptr, &work));
        TEST(work.bank.channel_chains[0].effects[0].bindings[3].base_value == 0.25f);
        TEST(work.bank.channel_chains[0].effects[0].bindings[4].base_value == 0.75f);
    }

    // Out-of-count band keys are format errors, not defaults: the decoder
    // routes them through the unknown-field path instead of silently
    // dropping them.
    const std::unique_ptr<FixedArray<char, 65536>> text_storage       = std::make_unique<FixedArray<char, 65536>>();
    char (&text)[65536]                                               = text_storage->elements;
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage   = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank           = *bank_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       loaded         = *loaded_storage;
    const auto inject_and_refuse = [&](const char* anchor_text, const char* replacement, const uint32_t band_count) {
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        bank.bank.channel_chains[0].effects[0].fir_bands.band_count = static_cast<uint8_t>(band_count);
        const uint32_t length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        const char* anchor = strstr(text, anchor_text);
        TEST(anchor != nullptr);
        if (! anchor) {
            return;
        }
        const uint32_t anchor_at       = static_cast<uint32_t>(anchor - text);
        const uint32_t anchor_len      = static_cast<uint32_t>(strlen(anchor_text));
        const uint32_t replacement_len = static_cast<uint32_t>(strlen(replacement));
        TEST(anchor_at + replacement_len + (length - anchor_at - anchor_len) + 1 <= sizeof(text));
        memmove(text + anchor_at + replacement_len, text + anchor_at + anchor_len, length - anchor_at - anchor_len + 1);
        memcpy(text + anchor_at, replacement, replacement_len);
        TEST(! Synth::decode_editor_bank_json(text, length + replacement_len - anchor_len, &loaded));
    };
    inject_and_refuse("\"fir_band1\"", "\"fir_band3\":{}\"fir_band1\"", 2);
    inject_and_refuse("\"type\":\"fir\"", "\"type\":\"fir\",\"fir_band1\":{}", 0);
    inject_and_refuse("\"type\":\"fir\"", "\"type\":\"fir\",\"fir_band0_gain\":{}", 0);

    // Band 0's authored Gain survives the single-band round trip: the key
    // encodes whenever the binding is non-default, and the legacy zeroed
    // block still encodes nothing.
    {
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        bank.bank.channel_chains[0].effects[0].fir_bands.bands[0].base_value = 0.7f;
        const uint32_t length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        TEST(strstr(text, "\"fir_band0_gain\"") != nullptr);
        TEST(Synth::decode_editor_bank_json(text, length, &loaded));
        TEST(loaded.bank.channel_chains[0].effects[0].fir_bands.bands[0].base_value == 0.7f);
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
        const uint32_t legacy_length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(legacy_length > 0);
        TEST(strstr(text, "fir_band0_gain") == nullptr); // the legacy zeroed block stays omitted
    }

    // Model-based evidence, not an executable GUI test: the unit
    // target does not link the GUI translation unit.  The wiring seams
    // classify endpoints through the band-aware row decoder, the drop
    // command carries the anchor slot for that decode, and the derived
    // generator title resolves band rows through the same decoder.

    // A corrupt in-memory band count cannot write out of bounds: the
    // remap clamps to the array bound like canonicalize_effect does.  The
    // probe keeps a canary behind the chain so the overflow cannot pass
    // silently.
    {
        struct RemapProbe {
            Synth::EffectChainBinding chain;
            uint64_t                  canary;
        };

        RemapProbe probe                            = {};
        probe.canary                                = 0x5A5A5A5A5A5A5A5Aull;
        probe.chain.num_effects                     = 1;
        probe.chain.effects[0].type                 = Synth::EffectType::fir;
        probe.chain.effects[0].fir_bands.band_count = 255; // in-memory corruption
        const uint16_t lfo_ids[]                    = { 10, 11, 12 };
        Synth::remap_effect_chain(probe.chain, lfo_ids, &probe.chain);
        TEST(probe.canary == 0x5A5A5A5A5A5A5A5Aull);
        TEST(probe.chain.effects[0].fir_bands.bands[12].lfo_desc_id == 0);
    }
}

// Multi-band FIR regressions: the effect type's identity (type tables,
// count/state budgets), the uniform Center/Spread/Gain/Slope band storage
// with staging-side edge derivation, the fx_band_region_projected gate
// opening for the multiband node only, the type-dropdown conversion porting
// both ways, and the bank codec's fir_multiband round trip.  The plain
// FIR's behavior stays byte-identical; fir_effect_bands_regressions and
// fir_effect_band_editor_regressions keep pinning it.
static void fir_multiband_staging_regressions()
{
    // Identity: the enum trails ring_mod, the param/state budgets match the
    // plain FIR's exactly (the multiband reuses the FIR's shader, state
    // carve and three legacy scalar slots).
    TEST(static_cast<uint32_t>(Synth::EffectType::fir_multiband) == Synth::num_effect_types - 1);
    TEST(Synth::get_effect_param_floats(Synth::EffectType::fir_multiband) == 3);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::fir_multiband) ==
         Synth::get_effect_state_floats(Synth::EffectType::fir));
    TEST(Synth::get_effect_state_bytes(Synth::EffectType::fir_multiband) ==
         Synth::get_effect_state_bytes(Synth::EffectType::fir));

    // Menu display order: Multi-band FIR sits immediately after FIR in the
    // effect-type menus; the enum order itself stays pinned by the JSON
    // codec.
    {
        uint32_t fir_pos       = 0;
        uint32_t multiband_pos = 0;
        for (uint32_t entry = 0; entry < Synth::num_effect_types - 1; ++entry) {
            if (Sculptor::fx_type_menu_order[entry] == Synth::EffectType::fir) {
                fir_pos = entry;
            }
            if (Sculptor::fx_type_menu_order[entry] == Synth::EffectType::fir_multiband) {
                multiband_pos = entry;
            }
        }
        TEST(multiband_pos == fir_pos + 1);
    }

    // Coefficient staging derives every band's edges from its uniform
    // center/spread pair: highpass = center - spread/2, lowpass = center +
    // spread/2, clamped to [0, Nyquist] so a band may be half-open.  A band
    // whose spread is nonpositive or whose center is nonpositive is inactive
    // and compacts out; an all-inactive configuration drops the entry
    // entirely.
    {
        Synth::FirEffectCoeffEntry entry                                          = {};
        float                      band_params[Synth::num_fir_effect_band_params] = {};
        // Band 0: center 4000 Hz, spread 3600 Hz -> 2200..5800 Hz, gain 0.7, slope 50.
        band_params[0] = 4000.0f;
        band_params[1] = 3600.0f;
        band_params[2] = 0.7f;
        band_params[3] = 50.0f;
        // Band 1 stays zeroed: the degenerate spread contributes nothing.
        // Band 2: center 1200 Hz, spread 800 Hz -> 800..1600 Hz, gain clamps down from 2.
        band_params[8]  = 1200.0f;
        band_params[9]  = 800.0f;
        band_params[10] = 2.0f;
        band_params[11] = 33.0f;
        TEST(Synth::stage_fir_multiband_coeff_entry(77, 3, band_params, &entry));
        TEST(entry.taps_offs == 77);
        TEST(entry.band_count == 2); // the degenerate band compacts out
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(5800.0f));
        TEST(entry.bands[0].highpass_cutoff_freq == Synth::cutoff_hz_to_int(2200.0f));
        TEST(entry.bands[0].gain == 0.7f);
        TEST(entry.bands[0].window_span == Synth::fir_effective_span(50.0f));
        TEST(entry.bands[1].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(1600.0f));
        TEST(entry.bands[1].highpass_cutoff_freq == Synth::cutoff_hz_to_int(800.0f));
        TEST(entry.bands[1].gain == 1.0f); // the oversized gain clamps down
        TEST(entry.bands[1].window_span == Synth::fir_effective_span(33.0f));
        // The zeroed count reads as the single-band marker.
        TEST(Synth::stage_fir_multiband_coeff_entry(9, 0, band_params, &entry));
        TEST(entry.band_count == 1);
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(5800.0f));
        // Half-open bands are legal staging states: the edges clamp to
        // [0, Nyquist].  A center at exactly spread/2 derives a highpass edge
        // of exactly 0, the plain FIR's pure-low form.
        float degenerate[Synth::num_fir_effect_band_params] = {};
        degenerate[0]                                       = 2000.0f;
        degenerate[1]                                       = 4000.0f; // highpass edge clamps to 0
        TEST(Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
        TEST(entry.band_count == 1);
        TEST(entry.bands[0].highpass_cutoff_freq == 0);
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(4000.0f));
        // A lowpass edge at or above Nyquist stages as a pure high band: the
        // lowpass cutoff writes 0 so no lowpass kernel runs, matching the
        // plain FIR's edge-off semantics exactly.
        degenerate[0] = 11525.0f; // highpass 1000, lowpass edge at Nyquist
        degenerate[1] = 21050.0f;
        TEST(Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
        TEST(entry.band_count == 1);
        TEST(entry.bands[0].highpass_cutoff_freq == Synth::cutoff_hz_to_int(1000.0f));
        TEST(entry.bands[0].lowpass_cutoff_freq == 0);
        // A negative highpass edge clamps to 0 instead of going inactive.
        degenerate[0] = 500.0f; // highpass edge -100 clamps to 0, lowpass 1100
        degenerate[1] = 1200.0f;
        TEST(Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
        TEST(entry.bands[0].highpass_cutoff_freq == 0);
        TEST(entry.bands[0].lowpass_cutoff_freq == Synth::cutoff_hz_to_int(1100.0f));
        // A zero spread and a NaN pair still read as absent.
        const float quiet_params[Synth::num_fir_effect_band_params] = {};
        TEST(! Synth::stage_fir_multiband_coeff_entry(9, 1, quiet_params, &entry));
        degenerate[0] = 2000.0f;
        degenerate[1] = 0.0f;
        TEST(! Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
        degenerate[1] = degenerate[1] / degenerate[1]; // NaN spread
        TEST(! Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
        degenerate[1] = 4000.0f;
        degenerate[0] = 0.0f;
        degenerate[0] = degenerate[0] / degenerate[0]; // NaN center
        TEST(! Synth::stage_fir_multiband_coeff_entry(9, 1, degenerate, &entry));
    }

    // Expansion: the uniform band values seed the instance's band array and
    // commit one node per modulated band parameter, exactly like the plain
    // FIR's band bindings.
    {
        Synth::InstrumentBank bank = {};
        build_small_bank_fixture(bank);
        Synth::EffectSlotBinding* const multiband = enabled_effect(bank, 0, 0, Synth::EffectType::fir_multiband);
        multiband->bindings[0].base_value         = 8000.0f; // the legacy scalars ride along unused
        multiband->bindings[2].base_value         = 100.0f;
        multiband->fir_bands.band_count           = 2;
        multiband->fir_bands.bands[0].base_value  = 4000.0f; // band 0 center
        multiband->fir_bands.bands[1].base_value  = 3600.0f; // band 0 spread
        multiband->fir_bands.bands[2].base_value  = 0.5f;    // band 0 gain
        multiband->fir_bands.bands[2].lfo_desc_id = 1;
        multiband->fir_bands.bands[2].lfo_depth   = 0.5f;
        multiband->fir_bands.bands[3].base_value  = 50.0f;   // band 0 slope
        multiband->fir_bands.bands[4].base_value  = 1200.0f; // band 1 center
        multiband->fir_bands.bands[5].base_value  = 800.0f;  // band 1 spread
        multiband->fir_bands.bands[6].base_value  = 1.0f;
        multiband->fir_bands.bands[7].base_value  = 100.0f;
        TEST(Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        // One modulated band binding: dest node + LFO leaf.
        TEST(plan.num_nodes == 2);
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        const Synth::EffectInstance& instance = chains[0].effects[0];
        TEST(instance.fir_bands.band_count == 2);
        TEST(instance.fir_bands.params[0] == 4000.0f);
        TEST(instance.fir_bands.params[1] == 3600.0f);
        TEST(instance.fir_bands.params[2] == 0.5f);
        TEST(instance.fir_bands.params[3] == 50.0f);
        TEST(instance.fir_bands.params[4] == 1200.0f);
        TEST(instance.fir_bands.params[5] == 800.0f);
        TEST(instance.fir_bands.params[6] == 1.0f);
        TEST(instance.fir_bands.params[7] == 100.0f);
        TEST(instance.fir_bands.src_param_id[2] == 50); // the modulated band's dest node
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        TEST(writer.last_dest_lfo_node == 51);
        for (uint32_t band_param = 0; band_param < Synth::num_fir_effect_band_params; band_param++) {
            if (band_param != 2) {
                TEST(instance.fir_bands.src_param_id[band_param] == 0); // unwired bands resolve nothing
            }
        }
        (void)Synth::take_effect_clear_ranges();
    }
    // The multiband's legacy bindings are unused: modulation stored there
    // validates but counts nothing, builds no node and resolves no source,
    // while the stored scalar still seeds the instance.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        build_small_bank_fixture(bank);
        Synth::EffectSlotBinding* const multiband  = enabled_effect(bank, 0, 0, Synth::EffectType::fir_multiband);
        multiband->bindings[0].base_value          = 8000.0f;
        multiband->bindings[0].lfo_desc_id         = 1;
        multiband->bindings[0].lfo_depth           = 0.5f;
        multiband->bindings[1].num_inputs          = 1;
        multiband->bindings[1].inputs[0].source    = Synth::ModSource::cc;
        multiband->bindings[1].inputs[0].cc_number = 17;
        multiband->bindings[1].inputs[0].op        = Synth::SourceOp::add;
        multiband->bindings[1].inputs[0].scale     = 1.0f;
        TEST(Synth::validate_instrument_bank(&bank));

        uint32_t num_modulated = 99;
        uint32_t state_bytes   = 0;
        Sculptor::count_effect_budgets(bank, &num_modulated, &state_bytes);
        TEST(num_modulated == 0);

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.num_nodes == 0);

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(writer.next_node == 50 && writer.dest_count == 0 && writer.lfo_count == 0);
        TEST(writer.num_resolved == 0);
        TEST(chains[0].effects[0].params[0] == 8000.0f);
        TEST(chains[0].effects[0].src_param_id[0] == 0 && chains[0].effects[0].src_param_id[1] == 0);
        (void)Synth::take_effect_clear_ranges();
    }

    // Refusals: a count beyond the array bound rejects at validation, and the
    // preflight refuses the oversized count.
    {
        Synth::InstrumentBank bank = {};
        build_small_bank_fixture(bank);
        Synth::EffectSlotBinding* const multiband = enabled_effect(bank, 0, 0, Synth::EffectType::fir_multiband);
        multiband->fir_bands.band_count           = Synth::max_fir_effect_bands + 1;
        TEST(! Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(error && strstr(error, "too many bands") != nullptr);
    }
    // Cloning remaps the multiband's active band LFO references through the
    // same lookup and zeroes the dormant tail.
    {
        Synth::EffectChainBinding src                 = {};
        src.num_effects                               = 1;
        src.effects[0].type                           = Synth::EffectType::fir_multiband;
        src.effects[0].fir_bands.band_count           = 2;
        src.effects[0].fir_bands.bands[2].lfo_desc_id = 1; // band 0's gain
        src.effects[0].fir_bands.bands[5].lfo_desc_id = 2; // band 1's spread
        src.effects[0].fir_bands.bands[9].lfo_desc_id = 3; // dormant band 2's center
        const uint16_t            lfo_ids[]           = { 10, 11, 12 };
        Synth::EffectChainBinding dst                 = {};
        Synth::remap_effect_chain(src, lfo_ids, &dst);
        TEST(dst.effects[0].fir_bands.bands[2].lfo_desc_id == 10);
        TEST(dst.effects[0].fir_bands.bands[5].lfo_desc_id == 11);
        TEST(dst.effects[0].fir_bands.bands[9].lfo_desc_id == 0);
        TEST(dst.effects[0].fir_bands.bands[15].lfo_desc_id == 0);
        TEST(dst.effects[0].fir_bands.band_count == 2);
    }

    // Bank JSON: the multiband slot round trips losslessly in both
    // directions; the plain FIR's legacy band keys refuse on the new type
    // and the new object keys refuse on the plain FIR.
    {
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        Synth::EffectSlotBinding* const multiband = enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir_multiband);
        multiband->fir_bands.band_count           = 3;
        multiband->fir_bands.bands[0].base_value  = 4000.0f;
        multiband->fir_bands.bands[1].base_value  = 3600.0f;
        multiband->fir_bands.bands[2].base_value  = 0.5f;
        multiband->fir_bands.bands[2].lfo_desc_id = 1;
        multiband->fir_bands.bands[3].base_value  = 50.0f;
        multiband->fir_bands.bands[6].base_value  = 0.25f;  // band 1's gain; its center/spread stay 0
        multiband->fir_bands.bands[7].base_value  = 33.0f;  // band 1's slope
        multiband->fir_bands.bands[8].base_value  = 250.0f; // band 2's center
        multiband->fir_bands.bands[9].base_value  = 100.0f; // band 2's spread
        multiband->fir_bands.bands[10].base_value = 0.9f;   // band 2's gain
        multiband->fir_bands.bands[11].base_value = 66.0f;  // band 2's slope
        const uint32_t length                     = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(length > 0);
        TEST(strstr(text, "\"type\":\"fir_multiband\"") != nullptr);
        TEST(strstr(text, "\"fir_bands\":3") != nullptr);
        TEST(strstr(text, "\"fir_band0\":{\"center\":{\"base_value\":4000}") != nullptr);
        TEST(strstr(text, "\"fir_band1\":{\"gain\":{\"base_value\":0.25}") != nullptr);
        TEST(strstr(text, "\"fir_band2\":{\"center\":{\"base_value\":250}") != nullptr);
        TEST(strstr(text, "fir_band3") == nullptr); // the dormant band stays out
        TEST(Synth::decode_editor_bank_json(text, length, &loaded));
        const Synth::EffectSlotBinding& decoded = loaded.bank.channel_chains[0].effects[0];
        TEST(decoded.fir_bands.band_count == 3);
        TEST(decoded.fir_bands.bands[0].base_value == 4000.0f);
        TEST(decoded.fir_bands.bands[1].base_value == 3600.0f);
        TEST(decoded.fir_bands.bands[2].base_value == 0.5f);
        TEST(decoded.fir_bands.bands[2].lfo_desc_id == 1);
        TEST(decoded.fir_bands.bands[3].base_value == 50.0f);
        TEST(decoded.fir_bands.bands[4].base_value == 0.0f);
        TEST(decoded.fir_bands.bands[5].base_value == 0.0f);
        TEST(decoded.fir_bands.bands[6].base_value == 0.25f);
        TEST(decoded.fir_bands.bands[7].base_value == 33.0f);
        TEST(decoded.fir_bands.bands[8].base_value == 250.0f);
        TEST(decoded.fir_bands.bands[9].base_value == 100.0f);
        TEST(decoded.fir_bands.bands[10].base_value == 0.9f);
        TEST(decoded.fir_bands.bands[11].base_value == 66.0f);
        TEST(decoded.fir_bands.bands[12].base_value == 0.0f); // band 3 stays dormant-zeroed
        // The decoded bank re-encodes and re-decodes byte-identically: the
        // round trip is idempotent, so no dead-value drift accumulates across
        // save/load cycles.
        const uint32_t reencoded_length = Synth::encode_editor_bank_json(&loaded, text, sizeof(text));
        TEST(reencoded_length == length);
        TEST(Synth::decode_editor_bank_json(text, reencoded_length, &loaded));
        TEST(memcmp(static_cast<const void*>(&loaded.bank.channel_chains[0].effects[0]),
                    static_cast<const void*>(&bank.bank.channel_chains[0].effects[0]),
                    sizeof(Synth::EffectSlotBinding)) == 0);
        // The single-band form (no count key) round trips band 0 alone.
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        Synth::EffectSlotBinding* const single = enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir_multiband);
        single->fir_bands.bands[0].base_value  = 2000.0f;
        single->fir_bands.bands[1].base_value  = 600.0f;
        single->fir_bands.bands[2].base_value  = 1.0f;   // the neutral gain omits its key
        single->fir_bands.bands[3].base_value  = 100.0f; // the default slope omits its key
        const uint32_t single_length           = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(single_length > 0);
        TEST(strstr(text, "\"fir_bands\"") == nullptr);
        TEST(strstr(text, "\"fir_band0\":{\"center\":{\"base_value\":2000},\"spread\":{\"base_value\":600}}") !=
             nullptr);
        TEST(Synth::decode_editor_bank_json(text, single_length, &loaded));
        TEST(loaded.bank.channel_chains[0].effects[0].fir_bands.bands[0].base_value == 2000.0f);
        TEST(loaded.bank.channel_chains[0].effects[0].fir_bands.bands[1].base_value == 600.0f);
        // Cross-type band keys are format errors in the strict effects
        // document dialect (the editor bank loader tolerates unknown keys by
        // design): the plain FIR's legacy keys refuse on a multiband slot,
        // the multiband's object keys refuse on a plain FIR slot, and an
        // out-of-range band count refuses outright.
        const std::unique_ptr<Synth::EffectsDocument> doc_storage         = std::make_unique<Synth::EffectsDocument>();
        Synth::EffectsDocument&                       doc                 = *doc_storage;
        const std::unique_ptr<Synth::EffectsDocument> doc_decoded_storage = std::make_unique<Synth::EffectsDocument>();
        Synth::EffectsDocument&                       doc_decoded         = *doc_decoded_storage;
        const auto splice_and_refuse = [&](Synth::EffectType type, const char* injected_key) {
            effects_clipboard_fixture(&doc, true);
            doc.chain.effects[0].type = type;
            if (type == Synth::EffectType::fir || type == Synth::EffectType::fir_multiband) {
                doc.chain.effects[0].bindings[2].base_value = 100.0f; // either FIR type needs an integer legacy slope
            }
            const uint32_t n = Synth::encode_effects_json(&doc, text, sizeof(text));
            TEST(n > 0);
            const char* type_key = strstr(text, "\"type\":\"");
            TEST(type_key != nullptr);
            const char* value_end = strchr(type_key + 8, '"');
            TEST(value_end != nullptr);
            if (! type_key || ! value_end) {
                return;
            }
            char injection[64];
            snprintf(injection, sizeof(injection), ",\"%s\":{}", injected_key);
            const uint32_t injection_len = static_cast<uint32_t>(strlen(injection));
            const uint32_t insert_at     = static_cast<uint32_t>(value_end - text) + 1;
            TEST(insert_at + injection_len + (n - insert_at) + 1 <= sizeof(text));
            memmove(text + insert_at + injection_len, text + insert_at, n - insert_at + 1);
            memcpy(text + insert_at, injection, injection_len);
            TEST(! Synth::decode_effects_json(text, n + injection_len, &doc_decoded));
        };
        splice_and_refuse(Synth::EffectType::fir_multiband, "fir_band0_gain");
        splice_and_refuse(Synth::EffectType::fir, "fir_band0");
        splice_and_refuse(Synth::EffectType::fir_multiband, "fir_band3");
        // An out-of-range count refuses the editor bank dialect too: the
        // bound lives in the value grammar, not the unknown-field policy.
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        build_small_bank_fixture(bank.bank);
        (void)enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir_multiband);
        const uint32_t count_length = Synth::encode_editor_bank_json(&bank, text, sizeof(text));
        TEST(count_length > 0);
        const char* bands_key = strstr(text, "\"fir_bands\":");
        TEST(bands_key == nullptr); // a fresh single-band slot omits the count key
        TEST(strstr(text, "\"type\":\"fir_multiband\"") != nullptr);
        if (strstr(text, "\"type\":\"fir_multiband\"")) {
            const char*    multiband_type = strstr(text, "\"type\":\"fir_multiband\"");
            const uint32_t insert_at      = static_cast<uint32_t>(multiband_type - text) + 22;
            const char*    injection      = ",\"fir_bands\":5";
            const uint32_t injection_len  = static_cast<uint32_t>(strlen(injection));
            memmove(text + insert_at + injection_len, text + insert_at, count_length - insert_at + 1);
            memcpy(text + insert_at, injection, injection_len);
            TEST(! Synth::decode_editor_bank_json(text, count_length + injection_len, &loaded));

            // The multiband type shares the plain FIR's legacy params shape, so the
            // slope reread covers it too: a fractional legacy slope (well-formed
            // for decode_binding's float grammar) refuses the strict document
            // while an integer one decodes.
            {
                const auto decode_with_multiband_slope = [](const char* third_binding_json) {
                    Synth::EffectsDocument slope_doc = {};
                    effects_clipboard_fixture(&slope_doc, true);
                    slope_doc.chain.effects[0].type                          = Synth::EffectType::fir_multiband;
                    slope_doc.chain.effects[0].fir_bands.bands[0].base_value = 4000.0f;
                    slope_doc.chain.effects[0].fir_bands.bands[1].base_value = 3600.0f;
                    slope_doc.chain.effects[0].fir_bands.bands[2].base_value = 0.5f;
                    slope_doc.chain.effects[0].fir_bands.bands[3].base_value = 66.0f;
                    slope_doc.chain.effects[0].bindings[0]                   = {};
                    slope_doc.chain.effects[0].bindings[1]                   = {};
                    slope_doc.chain.effects[0].bindings[2]                   = {};
                    slope_doc.chain.effects[0].bindings[0].base_value        = 8000.0f;
                    slope_doc.chain.effects[0].bindings[1].base_value        = 200.0f;
                    slope_doc.chain.effects[0].bindings[2].base_value        = 61.0f;
                    const std::unique_ptr<FixedArray<char, 65536>> encoded_storage =
                        std::make_unique<FixedArray<char, 65536>>();
                    char (&encoded)[65536] = encoded_storage->elements;
                    const uint32_t len     = Synth::encode_effects_json(&slope_doc, encoded, sizeof(encoded));
                    TEST(len > 0);
                    const char* const                              obj = "{\"base_value\":61}";
                    const std::unique_ptr<FixedArray<char, 65536>> spliced_storage =
                        std::make_unique<FixedArray<char, 65536>>();
                    char (&spliced)[65536] = spliced_storage->elements;
                    const uint32_t spliced_len =
                        splice_single_occurrence(encoded, len, obj, third_binding_json, spliced, sizeof(spliced));
                    TEST(spliced_len > 0);
                    if (! spliced_len) {
                        return false;
                    }
                    Synth::EffectsDocument out = {};
                    return Synth::decode_effects_json(spliced, spliced_len, &out);
                };
                TEST(decode_with_multiband_slope("{\"base_value\":61}"));     // the splice itself decodes
                TEST(! decode_with_multiband_slope("{\"base_value\":50.5}")); // fractional slope refuses
                TEST(! decode_with_multiband_slope("{\"base_value\":1e2}"));
            }
        }
    }

    // The fx_band_region_projected gate opens for the multiband node only:
    // the projected slot count encodes the uniform band region (3 region-base
    // slots + 43 slots per band), while a plain FIR node keeps its
    // byte-identical 33-slot legacy projection whatever its band state.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           candidate     = *candidate_storage;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};

        // Builds the model side: one multiband FIR effect on channel 0 with
        // the given band count and band 0's uniform bindings seeded.
        const auto build = [&](Synth::EffectType type, const uint32_t band_count) {
            memset(static_cast<void*>(&source), 0, sizeof(source));
            Synth::init_default_bank(&source.bank);
            Sculptor::set_identity_osc_sources(&source, 0, 0);
            const uint32_t lfo = source.bank.lfos.allocate();
            {
                Synth::LFODescriptor& descriptor = source.bank.lfos.entries[lfo];
                descriptor.wave                  = Synth::WaveType::sine_wave;
                descriptor.period_ms             = 300;
                descriptor.min_value             = -1.0f;
                descriptor.min_max_delta         = 2.0f;
            }
            auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
            chain.num_effects = 1;
            Sculptor::fx_init_slot(&chain, 0, type);
            chain.effects[0].bindings[0].base_value = 8000.0f;
            chain.effects[0].bindings[1].base_value = 200.0f;
            chain.effects[0].bindings[2].base_value = 55.0f;
            if (type == Synth::EffectType::fir_multiband) {
                chain.effects[0].fir_bands.band_count          = static_cast<uint8_t>(band_count);
                chain.effects[0].fir_bands.bands[0].base_value = 4000.0f;
                chain.effects[0].fir_bands.bands[1].base_value = 3600.0f;
                chain.effects[0].fir_bands.bands[2].base_value = 0.7f;
                chain.effects[0].fir_bands.bands[3].base_value = 50.0f;
            }
        };
        const auto reproject = [&]() {
            TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
            Sculptor::GraphChange drained[Sculptor::max_pending_changes];
            graph.take_changes(drained, Sculptor::max_pending_changes);
        };
        const auto node          = [&]() { return mapping.effect_nodes[0]; };
        const auto chain_effects = [&]() -> Synth::EffectSlotBinding& {
            return Sculptor::fx_graph_chain(&source.bank, 0).effects[0];
        };
        // The direct-change seam: stages (Graph::set_slot_value is
        // deliberately eventless), then applies the matching value_changed
        // event through the production apply function.
        const auto fx_edit = [&](const uint32_t slot_idx, const Sculptor::PropertyValue value) {
            graph.set_slot_value(node(), slot_idx, value);
            Sculptor::GraphChange change = {};
            change.kind                  = Sculptor::ChangeKind::value_changed;
            change.node_idx              = node();
            change.slot_idx              = slot_idx;
            candidate                    = source;
            const bool applied           = Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change);
            TEST(applied);
            if (applied) {
                source = candidate;
            }
            return applied;
        };

        // The gate stays shut for the plain FIR: exactly the 33 legacy slots
        // under every stored band count.
        build(Synth::EffectType::fir, 1);
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);
        build(Synth::EffectType::fir, 4);
        reproject();
        TEST(graph.node(node()).slots.num_allocated == Sculptor::fx_band_first_slot);

        // The gate opens for the multiband node: one uniform block per band.
        build(Synth::EffectType::fir_multiband, 1);
        reproject();
        TEST(graph.node(node()).slots.num_allocated ==
             Sculptor::fx_band_region_first_slot(Synth::EffectType::fir_multiband) + Sculptor::fx_band_block_stride);
        build(Synth::EffectType::fir_multiband, 4);
        reproject();
        TEST(graph.node(node()).slots.num_allocated ==
             Sculptor::fx_band_region_first_slot(Synth::EffectType::fir_multiband) +
                 Synth::max_fir_effect_bands * Sculptor::fx_band_block_stride);
        // The count byte travels unvalidated: a corrupt count clamps at the
        // region constants like every band walker.
        chain_effects().fir_bands.band_count = 250;
        reproject();
        TEST(graph.node(node()).slots.num_allocated ==
             Sculptor::fx_band_region_first_slot(Synth::EffectType::fir_multiband) +
                 Synth::max_fir_effect_bands * Sculptor::fx_band_block_stride);

        // The band-region click edits work on the multiband node through the
        // same [-]/[+] grammar fir_effect_band_editor_regressions pins on the
        // plain FIR, addressing the uniform band bindings.
        build(Synth::EffectType::fir_multiband, 1);
        reproject();
        Sculptor::PropertyValue press = {};
        press.toggle                  = true;
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir_multiband, 0) + 2, press)); // [+] on band 0
        TEST(chain_effects().fir_bands.band_count == 2);
        const Synth::EffectParamBinding* bands = chain_effects().fir_bands.bands;
        TEST(bands[4].base_value == 4000.0f); // the copy carries band 0's uniform bindings
        TEST(bands[5].base_value == 3600.0f);
        TEST(bands[6].base_value == 0.7f);
        TEST(bands[7].base_value == 50.0f);
        reproject();
        TEST(graph.node(node()).slots.num_allocated ==
             Sculptor::fx_band_region_first_slot(Synth::EffectType::fir_multiband) +
                 2 * Sculptor::fx_band_block_stride);
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir_multiband, 1) + 1, press)); // [-] on band 1
        TEST(chain_effects().fir_bands.band_count == 1);
        TEST(chain_effects().fir_bands.bands[4].base_value == 0.0f); // the vacated block zeroes
        // The multiband's deliberate unwired zero gain is not a legacy
        // marker: a copy out of band 0 keeps the zero gain instead of
        // seeding an audible 1.0 (the plain FIR's copy-out-of-band-0 gain
        // seed is plain-FIR only, matching apply_fx_band_edit).
        chain_effects().fir_bands.bands[2].base_value = 0.0f;
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir_multiband, 0) + 2, press)); // [+] on band 0
        TEST(chain_effects().fir_bands.band_count == 2);
        TEST(chain_effects().fir_bands.bands[6].base_value == 0.0f); // the copy keeps gain 0
        TEST(chain_effects().fir_bands.bands[2].base_value == 0.0f);
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir_multiband, 1) + 1, press)); // [-] on band 1
        TEST(chain_effects().fir_bands.band_count == 1);
        // The count bounds gray the buttons: a press past 4 bands changes
        // nothing.
        chain_effects().fir_bands.band_count = Synth::max_fir_effect_bands;
        TEST(fx_edit(Sculptor::fx_band_header_slot(Synth::EffectType::fir_multiband, Synth::max_fir_effect_bands - 1) +
                         2,
                     press));
        TEST(chain_effects().fir_bands.band_count == Synth::max_fir_effect_bands);
        // The multiband's band rows decode like the plain FIR's: a wire aimed
        // at a band row's LFO dot binds the row's uniform binding.  The bank
        // pre-references the LFO from band 0's slope so the LFO node
        // projects; the wire then binds the Gain row's binding to it.
        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[3].lfo_desc_id = 1;
        reproject();
        const uint32_t lfo_node = mapping.lfo_nodes[0];
        TEST(lfo_node != Sculptor::pool_no_slot);
        Sculptor::GraphChange wire      = {};
        wire.kind                       = Sculptor::ChangeKind::connection_added;
        wire.connection_output.node_idx = lfo_node;
        wire.connection_output.slot_idx = 0;
        wire.connection_input.node_idx  = node();
        wire.connection_input.slot_idx =
            Sculptor::fx_band_row_slot(Synth::EffectType::fir_multiband, 0, 2) + 1; // band 0's Gain row LFO dot
        candidate = source;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, wire));
        TEST(candidate.bank.channel_chains[0].effects[0].fir_bands.bands[2].lfo_desc_id == 1);
        source = candidate;

        // Type-dropdown conversion, both ways, through the production
        // candidate.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value = 8000.0f; // lowpass
        chain_effects().bindings[1].base_value = 200.0f;  // highpass
        chain_effects().bindings[2].base_value = 55.0f;   // slope
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.type == Synth::EffectType::fir_multiband);
            TEST(converted.fir_bands.band_count == 1);
            TEST(converted.fir_bands.bands[0].base_value == (8000.0f + 200.0f) * 0.5f); // center = midpoint
            TEST(converted.fir_bands.bands[1].base_value == 8000.0f - 200.0f);          // spread = width
            TEST(converted.fir_bands.bands[2].base_value == 1.0f);                      // the neutral gain
            TEST(converted.fir_bands.bands[3].base_value == 55.0f);                     // the legacy slope
        }
        // The pure lowpass (the FIR default state) ports as the lowpass-only
        // case: one active half-open band covering 0..lowpass.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value = 8000.0f; // lowpass
        chain_effects().bindings[1].base_value = 0.0f;    // highpass off
        chain_effects().bindings[2].base_value = 55.0f;   // slope
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.fir_bands.band_count == 1);
            TEST(converted.fir_bands.bands[0].base_value == 4000.0f); // center = lowpass/2
            TEST(converted.fir_bands.bands[1].base_value == 8000.0f); // spread = lowpass
            TEST(converted.fir_bands.bands[3].base_value == 55.0f);   // the legacy slope
        }
        // Highpass only: one band covering highpass..Nyquist.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value = 0.0f;
        chain_effects().bindings[1].base_value = 1000.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.fir_bands.band_count == 1);
            TEST(converted.fir_bands.bands[0].base_value == (1000.0f + Synth::rt_sampling_rate / 2.0f) * 0.5f);
            TEST(converted.fir_bands.bands[1].base_value == Synth::rt_sampling_rate / 2.0f - 1000.0f);
        }
        // Out-of-range authored cutoffs bind to the staged range first: the
        // plain FIR stages positive cutoffs clamped to Nyquist - 1, so a
        // highpass-only FIR at or above Nyquist must convert into a band that
        // stages (nonzero highpass, lowpass cutoff 0), not into the zero-spread
        // pair the multiband staging refuses.
        for (uint32_t overshoot = 0; overshoot < 2; ++overshoot) {
            build(Synth::EffectType::fir, 1);
            chain_effects().bindings[0].base_value = 0.0f;
            chain_effects().bindings[1].base_value =
                overshoot == 0 ? Synth::rt_sampling_rate / 2.0f : 3.0f * Synth::rt_sampling_rate / 4.0f;
            TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
            {
                const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
                TEST(converted.fir_bands.band_count == 1);
                TEST(converted.fir_bands.bands[0].base_value ==
                     (Synth::max_cutoff_hz + Synth::rt_sampling_rate / 2.0f) * 0.5f);
                TEST(converted.fir_bands.bands[1].base_value == 1.0f);
                // The converted band stages: nonzero highpass, lowpass cutoff 0.
                float band_params[Synth::num_fir_effect_band_params] = {};
                for (uint32_t kind = 0; kind < 4; ++kind) {
                    band_params[kind] = converted.fir_bands.bands[kind].base_value;
                }
                Synth::FirEffectCoeffEntry entry = {};
                TEST(Synth::stage_fir_multiband_coeff_entry(9, 1, band_params, &entry));
                TEST(entry.bands[0].highpass_cutoff_freq == Synth::max_cutoff_hz);
                TEST(entry.bands[0].lowpass_cutoff_freq == 0);
            }
        }

        // Reject case with an out-of-range highpass: the raw highpass binds to
        // the staged range too, so the high band keeps a stageable 1 Hz spread.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value = 500.0f;
        chain_effects().bindings[1].base_value = 3.0f * Synth::rt_sampling_rate / 4.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.fir_bands.band_count == 2);
            TEST(converted.fir_bands.bands[4].base_value ==
                 (Synth::max_cutoff_hz + Synth::rt_sampling_rate / 2.0f) * 0.5f);
            TEST(converted.fir_bands.bands[5].base_value == 1.0f);
        }

        // Reject (highpass >= lowpass, both set): two half-open bands in
        // ascending order, the low range 0..lowpass and the high range
        // highpass..Nyquist.  The slope ports into every created band, wire
        // included.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value  = 500.0f;  // lowpass
        chain_effects().bindings[1].base_value  = 1000.0f; // highpass
        chain_effects().bindings[2].base_value  = 33.0f;
        chain_effects().bindings[2].lfo_desc_id = 1;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.fir_bands.band_count == 2);
            TEST(converted.fir_bands.bands[0].base_value == 250.0f); // low band center = lowpass/2
            TEST(converted.fir_bands.bands[1].base_value == 500.0f); // low band spread = lowpass
            TEST(converted.fir_bands.bands[2].base_value == 1.0f);   // the neutral gain
            TEST(converted.fir_bands.bands[3].base_value == 33.0f);
            TEST(converted.fir_bands.bands[3].lfo_desc_id == 1);
            TEST(converted.fir_bands.bands[4].base_value == (1000.0f + Synth::rt_sampling_rate / 2.0f) * 0.5f);
            TEST(converted.fir_bands.bands[5].base_value == Synth::rt_sampling_rate / 2.0f - 1000.0f);
            TEST(converted.fir_bands.bands[6].base_value == 1.0f);
            TEST(converted.fir_bands.bands[7].base_value == 33.0f);
        }
        // Passthrough (both edges off): one full-range band that filters
        // nothing.
        build(Synth::EffectType::fir, 1);
        chain_effects().bindings[0].base_value = 0.0f;
        chain_effects().bindings[1].base_value = 0.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir_multiband, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.fir_bands.band_count == 1);
            TEST(converted.fir_bands.bands[0].base_value == Synth::rt_sampling_rate / 4.0f);
            TEST(converted.fir_bands.bands[1].base_value == Synth::rt_sampling_rate / 2.0f);
        }
        // Back conversion ports band 0 only, matching the staging's edge
        // derivation: a full-range band reads as passthrough (both cutoffs 0),
        // a pure low band keeps its lowpass edge with the highpass off, a pure
        // high band keeps its highpass edge with the lowpass off, an ordinary
        // band keeps both edges; bands 1..3 (UI Band 2-4) drop.
        build(Synth::EffectType::fir_multiband, 3);
        chain_effects().fir_bands.bands[0].base_value  = 4000.0f;
        chain_effects().fir_bands.bands[1].base_value  = 3600.0f;
        chain_effects().fir_bands.bands[2].base_value  = 0.7f;
        chain_effects().fir_bands.bands[3].base_value  = 55.0f;
        chain_effects().fir_bands.bands[3].lfo_desc_id = 1;
        chain_effects().fir_bands.bands[4].base_value  = 1200.0f; // band 1's values drop
        chain_effects().fir_bands.bands[5].base_value  = 800.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.type == Synth::EffectType::fir);
            TEST(converted.bindings[0].base_value == 5800.0f); // lowpass = center + spread/2
            TEST(converted.bindings[1].base_value == 2200.0f); // highpass = center - spread/2
            TEST(converted.bindings[2].base_value == 55.0f);   // band 0's slope rides along
            TEST(converted.bindings[2].lfo_desc_id == 1);      // the slope's wire survives
        }
        // The half-open band 0 forms back-convert to the plain FIR's edge-off
        // conventions.
        // Full-range band: both cutoffs 0 (passthrough).
        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[0].base_value = 11025.0f;
        chain_effects().fir_bands.bands[1].base_value = 22050.0f;
        chain_effects().fir_bands.bands[3].base_value = 55.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.bindings[0].base_value == 0.0f); // lowpass off
            TEST(converted.bindings[1].base_value == 0.0f); // highpass off
            TEST(converted.bindings[2].base_value == 55.0f);
        }
        // Pure low band: lowpass = edge, highpass = 0.
        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[0].base_value = 4000.0f;
        chain_effects().fir_bands.bands[1].base_value = 8000.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.bindings[0].base_value == 8000.0f);
            TEST(converted.bindings[1].base_value == 0.0f);
        }
        // Pure high band: highpass = edge, lowpass = 0.
        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[0].base_value = 11525.0f;
        chain_effects().fir_bands.bands[1].base_value = 21050.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        {
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.bindings[0].base_value == 0.0f);
            TEST(converted.bindings[1].base_value == 1000.0f);
        }
        // An inactive band 1 (nonpositive spread or center) stages nothing, so
        // it converts to the passthrough pair instead of a derived band-stop;
        // the slope still rides along.
        const float inactive_bands[3][2] = { { 4000.0f, -2000.0f }, { 4000.0f, 0.0f }, { 0.0f, 3000.0f } };
        for (const auto& inactive : inactive_bands) {
            build(Synth::EffectType::fir_multiband, 1);
            chain_effects().fir_bands.bands[0].base_value = inactive[0];
            chain_effects().fir_bands.bands[1].base_value = inactive[1];
            chain_effects().fir_bands.bands[3].base_value = 55.0f;
            TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
            const Synth::EffectSlotBinding& converted = candidate.bank.channel_chains[0].effects[0];
            TEST(converted.bindings[0].base_value == 0.0f);
            TEST(converted.bindings[1].base_value == 0.0f);
            TEST(converted.bindings[2].base_value == 55.0f);
        }

        // Derived edges normalize to the cutoff range like the forward
        // direction: a band above the range clamps its highpass to
        // max_cutoff_hz (the lowpass at Nyquist stays the pure-high 0), and
        // positive sub-1-Hz edges clamp up to 1 Hz.
        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[0].base_value = 30000.0f;
        chain_effects().fir_bands.bands[1].base_value = 2000.0f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 0.0f);
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[1].base_value ==
             static_cast<float>(Synth::max_cutoff_hz));

        build(Synth::EffectType::fir_multiband, 1);
        chain_effects().fir_bands.bands[0].base_value = 0.2f;
        chain_effects().fir_bands.bands[1].base_value = 0.2f;
        TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::fir, &candidate));
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[1].base_value == 1.0f);
    }
}

// Multi-band FIR node-projection regressions: the multiband node's tree
// starts at Band 1 right after the Enabled row (no
// legacy parameter rows project; the region base is type-aware, slot 3),
// one uniform "Band N" header per band at the oscillator band tree's
// depths, its four rows labeled Center/Spread/Gain/FIR Slope with the
// bindings' own values (no FIR edge-extreme display rule), and the count
// bounds graying the [-]/[+] click slots.
static void fir_multiband_projection_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           candidate     = *candidate_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::EffectGraphMapping           mapping       = {};

    const Synth::EffectType multiband = Synth::EffectType::fir_multiband;

    // Builds the model side: one multiband FIR effect on channel 0 with the
    // given stored band count and band 0's uniform bindings seeded.
    const auto build = [&](const uint32_t band_count) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        auto& chain       = Sculptor::fx_graph_chain(&source.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, multiband);
        chain.effects[0].fir_bands.band_count          = static_cast<uint8_t>(band_count);
        chain.effects[0].fir_bands.bands[0].base_value = 4000.0f;
        chain.effects[0].fir_bands.bands[1].base_value = 3600.0f;
        chain.effects[0].fir_bands.bands[2].base_value = 0.7f;
        chain.effects[0].fir_bands.bands[3].base_value = 50.0f;
    };
    const auto reproject = [&]() {
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        Sculptor::GraphChange drained[Sculptor::max_pending_changes];
        graph.take_changes(drained, Sculptor::max_pending_changes);
    };
    const auto node          = [&]() { return mapping.effect_nodes[0]; };
    const auto chain_effects = [&]() -> Synth::EffectSlotBinding& {
        return Sculptor::fx_graph_chain(&source.bank, 0).effects[0];
    };

    // Projection slot sets for every stored count: the tree is the three
    // root slots (In/Out/Enabled) plus one uniform 43-slot block per band;
    // the extents pin the multiband region base (slot 3, right after the
    // Enabled row), not the plain FIR's 33.
    for (uint32_t count = 1; count <= Synth::max_fir_effect_bands; ++count) {
        build(count);
        reproject();
        TEST(graph.node(node()).slots.num_allocated ==
             Sculptor::fx_band_region_first_slot(multiband) + count * Sculptor::fx_band_block_stride);
    }

    // Band 1's block: the "Band 1" header line at the oscillator band
    // tree's depths, its [-]/[+] click slots, and the four uniform rows
    // carrying the bindings' own values. No legacy parameter rows project:
    // slot 3 is the band header itself.
    build(2);
    chain_effects().fir_bands.bands[4].base_value = 1200.0f; // band 1's center
    chain_effects().fir_bands.bands[5].base_value = 800.0f;  // band 1's spread
    reproject();
    {
        const Sculptor::Node& effect_node = graph.node(node());
        TEST(strcmp(effect_node.name, "Multi-band FIR") == 0);
        TEST(Sculptor::fx_band_region_first_slot(multiband) == Sculptor::fx_param_row(0));
        const uint32_t header = Sculptor::fx_band_header_slot(multiband, 0);
        TEST(header == Sculptor::fx_param_row(0));
        TEST(strcmp(effect_node.slots.entries[header].name, "Band 1") == 0);
        TEST(effect_node.slots.entries[header].child_row);
        TEST(effect_node.slots.entries[header].child_level == 1);
        TEST(strcmp(effect_node.slots.entries[header + 1].name, "-") == 0);
        TEST(strcmp(effect_node.slots.entries[header + 2].name, "+") == 0);
        static const char* const row_names[4] = { "Center (Hz)", "Spread (Hz)", "Gain", "FIR Slope" };
        for (uint32_t row = 0; row < 4; ++row) {
            const uint32_t row_slot = Sculptor::fx_band_row_slot(multiband, 0, row);
            TEST(strcmp(effect_node.slots.entries[row_slot].name, row_names[row]) == 0);
            TEST(effect_node.slots.entries[row_slot].child_row);
            TEST(effect_node.slots.entries[row_slot].child_level == 2);
            // The row's modulation block sits one level deeper (the
            // oscillator band blocks' depth).
            TEST(effect_node.slots.entries[row_slot + 1].child_level == Sculptor::osc_band_block_level);
        }
        // The rows project the bindings' own values: no edge-extreme rule
        // applies to Center/Spread (the FIR zero-cutoff display rule is
        // legacy-only).
        TEST(effect_node.slots.entries[Sculptor::fx_band_row_slot(multiband, 1, 0)].value.real == 1200.0f);
        TEST(effect_node.slots.entries[Sculptor::fx_band_row_slot(multiband, 1, 1)].value.real == 800.0f);
        TEST(strcmp(effect_node.slots.entries[Sculptor::fx_band_header_slot(multiband, 1)].name, "Band 2") == 0);
    }

    // The count bounds gray the click slots: at one band only [+] works, at
    // the maximum only [-], and both stay live in between.
    build(1);
    reproject();
    TEST(graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 1));
    TEST(! graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 2));
    build(Synth::max_fir_effect_bands);
    reproject();
    TEST(! graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 1));
    TEST(graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 2));
    TEST(! graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 1) + 1));
    TEST(graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 1) + 2));
    build(2);
    reproject();
    TEST(! graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 1));
    TEST(! graph.slot_edit_disabled(node(), Sculptor::fx_band_header_slot(multiband, 0) + 2));

    // A channel-MIDI wire lands on a band row's source dot and binds the
    // row's uniform binding: band 0's Gain row (source A dot at +4) takes
    // the mod wheel. The connection lands in the graph first (production
    // connects through the graph), then the change rebuilds the packed
    // input list from the wires.
    build(1);
    reproject();
    const Sculptor::EndPoint midi_from = {
        mapping.midi_node,
        mapping.midi_source_slots[static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1]
    };
    const Sculptor::EndPoint midi_to = { node(), Sculptor::fx_band_row_slot(multiband, 0, 2) + 4u };
    TEST(graph.add_connection(midi_from, midi_to) != Sculptor::pool_no_slot);
    Sculptor::GraphChange midi_wire = {};
    midi_wire.kind                  = Sculptor::ChangeKind::connection_added;
    midi_wire.connection_output     = midi_from;
    midi_wire.connection_input      = midi_to;
    candidate                       = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, midi_wire));
    TEST(candidate.bank.channel_chains[0].effects[0].fir_bands.bands[2].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].fir_bands.bands[2].inputs[0].source ==
         Synth::ModSource::mod_wheel);

    // Modulation stored in the legacy scalar bindings the multiband type
    // never reads must project no legacy wires: the legacy traversals walk
    // the projected parameter rows (0 for the multiband), so an unused
    // bindings[] entry cannot spill its wires onto the band tree's slots
    // (the old slot arithmetic would land the MIDI wire on Band 1 Center
    // LFO input and the LFO wire on the band header's [-] click slot).
    build(1);
    const uint16_t lfo_id                        = add_detached_lfo_descriptor(&source);
    chain_effects().bindings[0].lfo_desc_id      = lfo_id;
    chain_effects().bindings[0].num_inputs       = 1;
    chain_effects().bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
    reproject();
    TEST(! graph.slot_is_connected(node(), Sculptor::fx_param_src_dot(0, 0)));
    TEST(! graph.slot_is_connected(node(), Sculptor::fx_param_lfo_dot(0)));
    // The band rows stay the only wire targets: nothing reaches the band
    // rows' modulation dots from the unused legacy bindings.
    TEST(! graph.slot_is_connected(node(), Sculptor::fx_band_row_slot(multiband, 0, 2) + 4u));
    TEST(! graph.slot_is_connected(node(), Sculptor::fx_band_row_slot(multiband, 0, 0) + 4u));

    // Reference discovery follows the same bounds: the unused legacy
    // bindings and the inactive band tail hold no LFO and no CC reference
    // for a multiband slot (no orphan LFO node, no CC lock with no visible
    // wire), while the active band bindings still do.
    {
        constexpr uint32_t cc_number = 16;
        const auto         wire_cc   = [&](Synth::EffectParamBinding& binding) {
            binding.num_inputs          = 1;
            binding.inputs[0].source    = Synth::ModSource::cc;
            binding.inputs[0].cc_number = cc_number;
        };
        build(1);
        const uint16_t unused_lfo = add_detached_lfo_descriptor(&source);
        wire_cc(chain_effects().bindings[1]);
        chain_effects().bindings[1].lfo_desc_id = unused_lfo;
        reproject();
        Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&source.bank, 0);
        TEST(! Sculptor::effect_chain_uses_lfo(chain, unused_lfo));
        TEST(! Sculptor::fx_chain_uses_cc(chain, cc_number));
        TEST(mapping.lfo_nodes[unused_lfo - 1] == Sculptor::pool_no_slot);
        // A band beyond the stored count is bounded out as well ...
        chain_effects().bindings[1]                    = Synth::EffectParamBinding{};
        chain_effects().fir_bands.bands[8].lfo_desc_id = unused_lfo;
        wire_cc(chain_effects().fir_bands.bands[9]);
        TEST(! Sculptor::effect_chain_uses_lfo(chain, unused_lfo));
        TEST(! Sculptor::fx_chain_uses_cc(chain, cc_number));
        // ... and the same bindings count once the band is active.
        chain_effects().fir_bands.band_count = 3;
        TEST(Sculptor::effect_chain_uses_lfo(chain, unused_lfo));
        TEST(Sculptor::fx_chain_uses_cc(chain, cc_number));
        // The plain FIR keeps validating all sixteen bindings, dormant included.
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::fir);
        chain_effects().fir_bands.band_count            = 1;
        chain_effects().fir_bands.bands[15].lfo_desc_id = unused_lfo;
        wire_cc(chain_effects().fir_bands.bands[14]);
        TEST(Sculptor::effect_chain_uses_lfo(chain, unused_lfo));
        TEST(Sculptor::fx_chain_uses_cc(chain, cc_number));
    }

    // The band-click boundary resolves the master chain (index max_channels)
    // through the chain accessor: a band click on the master chain's
    // multiband node truncates the batch at the click, drops the trailing
    // value event, and flags a structural reprojection.
    {
        build(2);
        Synth::EffectChainBinding& master = Sculptor::fx_graph_chain(&source.bank, Sculptor::fx_master_chain);
        master                            = Synth::EffectChainBinding{};
        master.num_effects                = 1;
        Sculptor::fx_init_slot(&master, 0, multiband);
        master.effects[0].fir_bands.band_count = 2;
        TEST(Sculptor::project_effect_chain_to_graph(source, Sculptor::fx_master_chain, &graph, &mapping));
        Sculptor::GraphChange batch[2] = {};
        batch[0].kind                  = Sculptor::ChangeKind::value_changed;
        batch[0].node_idx              = mapping.effect_nodes[0];
        batch[0].slot_idx              = Sculptor::fx_band_header_slot(multiband, 0) + 2;
        batch[1]                       = batch[0];
        batch[1].slot_idx              = Sculptor::fx_band_row_slot(multiband, 0, 0);
        bool band_click                = false;
        TEST(Sculptor::fx_band_click_boundary(batch, 2, mapping, source.bank, &band_click) == 1);
        TEST(band_click);
    }
}

// LFO pool reclamation roots and remaps FIR band-row LFO references with the same
// bounds the validator accepts, and ignores a multiband's unused legacy bindings.
static void fir_band_lfo_reclaim_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;

    const auto build = [&](const Synth::EffectType type, const uint32_t band_count) -> Synth::EffectSlotBinding& {
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        Synth::init_default_bank(&bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        auto& chain       = Sculptor::fx_graph_chain(&bank.bank, 0);
        chain.num_effects = 1;
        Sculptor::fx_init_slot(&chain, 0, type);
        chain.effects[0].fir_bands.band_count = static_cast<uint8_t>(band_count);
        return chain.effects[0];
    };
    const auto add_lfo = [&](const uint32_t period_ms) -> uint16_t {
        const uint32_t index                    = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[index].wave      = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[index].period_ms = static_cast<uint16_t>(period_ms);
        return static_cast<uint16_t>(index + 1);
    };

    // A multiband band-row LFO with no layout record survives compaction behind
    // an earlier unused LFO, with its band id remapped.
    {
        Synth::EffectSlotBinding& effect = build(Synth::EffectType::fir_multiband, 2);
        const uint32_t            base   = bank.bank.lfos.num_allocated;
        add_lfo(70);
        effect.fir_bands.bands[5].lfo_desc_id = add_lfo(90);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.lfos.num_allocated == base + 1);
        TEST(effect.fir_bands.bands[5].lfo_desc_id == base + 1);
        TEST(bank.bank.lfos.entries[base].period_ms == 90);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.lfos.num_allocated == base + 1);
        TEST(effect.fir_bands.bands[5].lfo_desc_id == base + 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // An LFO referenced only by a multiband's unused legacy binding is freed and
    // the stale legacy reference clears instead of dangling.
    {
        Synth::EffectSlotBinding& effect = build(Synth::EffectType::fir_multiband, 1);
        const uint32_t            base   = bank.bank.lfos.num_allocated;
        effect.bindings[0].lfo_desc_id   = add_lfo(70);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.lfos.num_allocated == base);
        TEST(effect.bindings[0].lfo_desc_id == 0);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }

    // The plain FIR validates its dormant band bindings, so a dormant reference
    // roots its LFO and follows the compaction.
    {
        Synth::EffectSlotBinding& effect = build(Synth::EffectType::fir, 1);
        const uint32_t            base   = bank.bank.lfos.num_allocated;
        add_lfo(70);
        effect.fir_bands.bands[15].lfo_desc_id = add_lfo(90);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.lfos.num_allocated == base + 1);
        TEST(effect.fir_bands.bands[15].lfo_desc_id == base + 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
    }
}

// Multi-band decode and staging regressions: portable documents, legacy
// defaults, bank round-trips and library installation.

// Assembles the portable multi-band document: one bandpass layer, fir_bands 4,
// and all sixteen band parameters bound (base + envelope + LFO + two MIDI
// sources), with per-band envelope and LFO contents so the decoder interns
// four of each.  Returns the document length, or 0 when it does not fit.
static uint32_t build_m2_multiband_document(char* text, const uint32_t capacity)
{
    const char* const head =
        "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
        "\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"blend\",\"fir_type\":\"bandpass\",\"fir_bands\":4,";

    static const struct {
        const char* offset_key;
        const char* spread_key;
        const char* gain_key;
        const char* slope_key;
    } band_keys[4] = {
        { "fir_band0_offset", "fir_pitch_spread", "fir_band0_gain", "fir_slope" },
        { "fir_band1_offset", "fir_band1_spread", "fir_band1_gain", "fir_band1_slope" },
        { "fir_band2_offset", "fir_band2_spread", "fir_band2_gain", "fir_band2_slope" },
        { "fir_band3_offset", "fir_band3_spread", "fir_band3_gain", "fir_band3_slope" },
    };

    static const struct {
        float   offset;
        float   spread;
        float   gain;
        int32_t slope;
    } band_bases[4] = {
        { -6.0f, 4.0f, 0.9f, 90 },
        { -3.0f, 5.0f, 0.8f, 80 },
        { 3.0f, 6.0f, 0.7f, 70 },
        { 6.0f, 7.0f, 0.6f, 60 },
    };

    uint32_t   len    = 0;
    const auto append = [&](const char* fragment) {
        const size_t fragment_len = strlen(fragment);
        if (len + fragment_len + 1 > capacity) {
            return false;
        }
        memcpy(text + len, fragment, fragment_len);
        len += static_cast<uint32_t>(fragment_len);
        text[len] = 0;
        return true;
    };
    if (! append(head)) {
        return 0;
    }
    for (uint32_t band = 0; band < 4; band++) {
        if (band > 0 && ! append(",")) {
            return 0;
        }
        const char* const keys[4] = { band_keys[band].offset_key,
                                      band_keys[band].spread_key,
                                      band_keys[band].gain_key,
                                      band_keys[band].slope_key };
        for (uint32_t p = 0; p < 4; p++) {
            char base_text[24] = {};
            if (p == 3) {
                snprintf(base_text, sizeof(base_text), "%d", band_bases[band].slope);
            }
            else {
                const float base = p == 0   ? band_bases[band].offset
                                   : p == 1 ? band_bases[band].spread
                                            : band_bases[band].gain;
                snprintf(base_text, sizeof(base_text), "%.1f", static_cast<double>(base));
            }
            char      fragment[640] = {};
            const int written       = snprintf(
                fragment,
                sizeof(fragment),
                "%s\"%s\":{\"base\":%s,\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
                "{\"time_seconds\":0.5,\"level\":%.3f}],\"sustain_start\":0,\"sustain_end\":1,\"ping_pong\":1},"
                "\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":%u,\"duty\":0.25,\"depth\":0.5,\"min_level\":0,\"max_level\":1,"
                "\"depth_source\":\"velocity\",\"rate_source\":\"mod_wheel\",\"rate_scale_ms\":20},"
                "\"sources\":[{\"source\":\"velocity\",\"op\":\"add\",\"scale\":0.5},"
                "{\"source\":\"mod_wheel\",\"op\":\"multiply\",\"scale\":0.25}]}",
                p > 0 ? "," : "",
                keys[p],
                base_text,
                static_cast<double>(0.25f + 0.05f * static_cast<float>(band)),
                4u + band);
            TEST(written > 0 && written < static_cast<int>(sizeof(fragment)));
            if (written <= 0 || written >= static_cast<int>(sizeof(fragment)) || ! append(fragment)) {
                return 0;
            }
        }
    }
    if (! append("}]}")) {
        return 0;
    }
    return len;
}

// Installs a decoded document into a fresh editor bank as channel 0 zone 0 so
// the portable encoder can re-export it.
static void install_m2_decoded_instrument(Synth::InstrumentEditorBank* bank, const ClipboardDecode& decoded)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    bank->bank.channel_enabled[0] = 1;
    TEST(bank->bank.instruments.allocate() == 0);
    bank->bank.instruments.entries[0] = decoded.instr;
    for (uint32_t i = 0; i < decoded.env_count; i++) {
        TEST(bank->bank.envelopes.allocate() == i);
        bank->bank.envelopes.entries[i] = decoded.envs[i];
    }
    for (uint32_t i = 0; i < decoded.lfo_count; i++) {
        TEST(bank->bank.lfos.allocate() == i);
        bank->bank.lfos.entries[i] = decoded.lfos[i];
    }
    bank->bank.channel_zones[0][0].start_note = 1;
    bank->bank.channel_zones[0][0].instrument = 0;
}

// Legacy backfill expectations ride the decoded-object template parameter so
// the band member access defers with the band state.
template <bool M2, typename DecodedT> static void m2_check_legacy_backfill(DecodedT& decoded)
{
    if constexpr (M2) {
        TEST(decoded.instr.layer_count == 2);
        for (uint32_t layer = 0; layer < 2 && layer < decoded.instr.layer_count; layer++) {
            const auto& layer_osc = decoded.instr.layers[layer];
            TEST(layer_osc.fir_band_count == 1);
            const uint32_t offset_target = m2_band0_targets[0];
            const uint32_t gain_target   = m2_band0_targets[2];
            TEST(layer_osc.targets[offset_target].base_value == 0.0f);
            TEST(layer_osc.targets[gain_target].base_value == 1.0f);
        }
    }
    else {
        TEST(false);
    }
}

// C2.4: the portable document codec round-trips multi-band state, backfills
// legacy single-band documents, and keeps the gain-0 and slope conventions.
static void m2_document_json_regressions()
{
    {
        const std::unique_ptr<FixedArray<char, 8 * 1024>> doc_storage = std::make_unique<FixedArray<char, 8 * 1024>>();
        char (&doc)[8 * 1024]                                         = doc_storage->elements;
        const uint32_t doc_len                                        = build_m2_multiband_document(doc, sizeof(doc));
        TEST(doc_len > 0);
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        const bool                             ok              = decode_clipboard(&decoded, doc, max_portable_records);
        TEST(ok);
        if (! ok) {
            return;
        }
        TEST(decoded.instr.layer_count == 1);
        static const float bases[4][4] = {
            { -6.0f, 4.0f, 0.9f, 90.0f },
            { -3.0f, 5.0f, 0.8f, 80.0f },
            { 3.0f, 6.0f, 0.7f, 70.0f },
            { 6.0f, 7.0f, 0.6f, 60.0f },
        };
        for (uint32_t band = 0; band < 4; band++) {
            for (uint32_t p = 0; p < 4; p++) {
                const uint32_t              target = m2_band_targets[band][p];
                const Synth::TargetBinding& gen    = decoded.instr.layers[0].targets[target];
                TEST(gen.base_value == bases[band][p]);
                TEST(gen.envelope_desc_id != 0);
                TEST(gen.lfo_desc_id != 0);
                TEST(gen.num_inputs == 2);
                TEST(gen.inputs[0].source == Synth::ModSource::velocity);
                TEST(gen.inputs[1].source == Synth::ModSource::mod_wheel);
            }
        }
        TEST(decoded.env_count == 4);
        TEST(decoded.lfo_count == 4);
        // The re-exported document decodes back to the same band state and
        // carries the count key.
        const std::unique_ptr<Synth::InstrumentEditorBank> installed_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& installed = *installed_storage;
        install_m2_decoded_instrument(&installed, decoded);
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&text)[clipboard_capacity] = text_storage->elements;
        TEST(export_clipboard_fixture(installed, text, sizeof(text)));
        TEST(strstr(text, "\"fir_bands\":4") != nullptr);
        const std::unique_ptr<ClipboardDecode> redecoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       redecoded         = *redecoded_storage;
        TEST(decode_clipboard(&redecoded, text, max_portable_records));
        TEST(redecoded.instr.layers[0].targets[m2_band_targets[1][0]].base_value == -3.0f);
        TEST(redecoded.instr.layers[0].targets[m2_band_targets[1][2]].envelope_desc_id != 0);
    }

    // Legacy single-band documents backfill one band with Offset 0 and Gain 1
    // on every layer.
    {
        const char* const legacy =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":0.5}},"
            "{\"wave_a\":\"sawtooth\",\"fir_type\":\"bandpass\",\"fir_pitch_spread\":{\"base\":3}}]}";
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        TEST(decode_clipboard(&decoded, legacy, max_portable_records));
        m2_check_legacy_backfill<m2_band_state_present>(decoded);
    }

    // An explicitly saved gain-0 band stays gain 0 through the round trip (the
    // explicit-empty-block convention): decode treats it as an inactive band and
    // the encoder re-emits it instead of reloading it as the 1.0 default.
    {
        const char* const gain0_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
            "\"fir_bands\":2,\"fir_band0_gain\":{}}]}";
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        const bool                             ok = decode_clipboard(&decoded, gain0_doc, max_portable_records);
        TEST(ok);
        if (! ok) {
            return;
        }
        TEST(decoded.instr.layers[0].targets[m2_band0_targets[2]].base_value == 0.0f);
        const std::unique_ptr<Synth::InstrumentEditorBank> installed_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& installed = *installed_storage;
        install_m2_decoded_instrument(&installed, decoded);
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&text)[clipboard_capacity] = text_storage->elements;
        TEST(export_clipboard_fixture(installed, text, sizeof(text)));
        TEST(strstr(text, "\"fir_band0_gain\":{}") != nullptr);
    }

    // A later layer's explicit gain 0 survives too: the gain default is seeded
    // for every layer, and the nonzero default is not lossy for a zero base.
    {
        const char* const later_gain0_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"},"
            "{\"wave_a\":\"sawtooth\",\"fir_type\":\"bandpass\",\"fir_bands\":2,\"fir_band0_gain\":{}}]}";
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        const bool                             ok = decode_clipboard(&decoded, later_gain0_doc, max_portable_records);
        TEST(ok);
        if (! ok) {
            return;
        }
        TEST(decoded.instr.layers[0].targets[m2_band0_targets[2]].base_value == 1.0f);
        TEST(decoded.instr.layers[1].targets[m2_band0_targets[2]].base_value == 0.0f);
        const std::unique_ptr<Synth::InstrumentEditorBank> installed_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& installed = *installed_storage;
        install_m2_decoded_instrument(&installed, decoded);
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&text)[clipboard_capacity] = text_storage->elements;
        TEST(export_clipboard_fixture(installed, text, sizeof(text)));
        TEST(strstr(text, "\"fir_band0_gain\":{}") != nullptr);
    }

    // A present band-slope block with an omitted base decodes the
    // routed-to-zero state; an absent block decodes the 100 default.
    {
        const char* const slope0_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
            "\"fir_bands\":2,\"fir_band1_slope\":{}}]}";
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        const bool                             ok = decode_clipboard(&decoded, slope0_doc, max_portable_records);
        TEST(ok);
        if (! ok) {
            return;
        }
        TEST(decoded.instr.layers[0].targets[m2_band_targets[1][3]].base_value == 0.0f);
    }
    {
        const char* const absent_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
            "\"fir_bands\":2,\"fir_band1_offset\":{\"base\":2}}]}";
        const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       decoded         = *decoded_storage;
        const bool                             ok = decode_clipboard(&decoded, absent_doc, max_portable_records);
        TEST(ok);
        if (! ok) {
            return;
        }
        TEST(decoded.instr.layers[0].targets[m2_band_targets[1][0]].base_value == 2.0f);
        TEST(decoded.instr.layers[0].targets[m2_band_targets[1][1]].base_value == 0.0f);
        TEST(decoded.instr.layers[0].targets[m2_band_targets[1][2]].base_value == 1.0f);
        TEST(decoded.instr.layers[0].targets[m2_band_targets[1][3]].base_value == 100.0f);
    }
}

// C2.4: the bank JSON embeds multi-band state for every layer; a single-band
// instrument omits the count key, and the decode side installs the band state
// on top of existing bank content without disturbing it.
static void m2_bank_json_round_trip_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    build_m2_bandpass_fixture<m2_band_state_present>(&source, instrument.layers[0], false, false);
    // The fixture fills layer 0 only; the encoder needs the count set.
    instrument.layer_count = 1;
    set_default_dispersion_sections(instrument);
    source.bank.channel_enabled[0] = 1;
    TEST(source.bank.instruments.allocate() == 0);
    source.bank.instruments.entries[0]         = instrument;
    source.bank.channel_zones[0][0].start_note = 1;
    source.bank.channel_zones[0][0].instrument = 0;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> doc_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&doc)[clipboard_capacity] = doc_storage->elements;
    const uint32_t doc_len          = Synth::encode_editor_bank_json(&source, doc, sizeof(doc));
    TEST(doc_len > 0);
    const char* const anchor = strstr(doc, "\"wave_a\":\"sine\"");
    TEST(anchor != nullptr);
    if (! anchor) {
        return;
    }
    static const char                                           band_keys[] = ",\"fir_bands\":4,"
                                                                              "\"fir_band0_offset\":{\"base\":5},"
                                                                              "\"fir_band0_gain\":{\"base\":0.5},"
                                                                              "\"fir_band1_offset\":{\"base\":7},"
                                                                              "\"fir_band1_spread\":{\"base\":4},"
                                                                              "\"fir_band1_gain\":{\"base\":0.8},"
                                                                              "\"fir_band1_slope\":{\"base\":90},"
                                                                              "\"fir_band2_offset\":{\"base\":9},"
                                                                              "\"fir_band2_spread\":{\"base\":5},"
                                                                              "\"fir_band2_gain\":{\"base\":0.7},"
                                                                              "\"fir_band2_slope\":{\"base\":80},"
                                                                              "\"fir_band3_offset\":{\"base\":11},"
                                                                              "\"fir_band3_spread\":{\"base\":6},"
                                                                              "\"fir_band3_gain\":{\"base\":0.6},"
                                                                              "\"fir_band3_slope\":{\"base\":70}";
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> combined_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&combined)[clipboard_capacity] = combined_storage->elements;
    {
        const size_t insert_at = static_cast<size_t>(anchor + strlen("\"wave_a\":\"sine\"") - doc);
        const size_t keys_len  = sizeof(band_keys) - 1;
        TEST(doc_len + keys_len + 1 <= sizeof(combined));
        if (doc_len + keys_len + 1 > sizeof(combined)) {
            return;
        }
        memcpy(combined, doc, insert_at);
        memcpy(combined + insert_at, band_keys, keys_len);
        memcpy(combined + insert_at + keys_len, doc + insert_at, doc_len - insert_at);
        combined[doc_len + keys_len] = 0;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> target_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       target         = *target_storage;
    target.bank.channel_enabled[0]                                    = 1;
    TEST(Synth::decode_editor_bank_json(combined, doc_len + sizeof(band_keys) - 1, &target));
    static const float band_bases[4][4] = {
        { 5.0f, 3.0f, 0.5f, 50.0f },
        { 7.0f, 4.0f, 0.8f, 90.0f },
        { 9.0f, 5.0f, 0.7f, 80.0f },
        { 11.0f, 6.0f, 0.6f, 70.0f },
    };
    for (uint32_t band = 0; band < 4; band++) {
        for (uint32_t p = 0; p < 4; p++) {
            const uint32_t              target_index = m2_band_targets[band][p];
            const Synth::TargetBinding& gen = target.bank.instruments.entries[0].layers[0].targets[target_index];
            TEST(gen.base_value == band_bases[band][p]);
        }
    }

    // The decode side installs band state over existing bank content, and the
    // re-encoded bank carries the keys again while keeping pre-existing state.
    target.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value = 0.25f;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    TEST(Synth::encode_editor_bank_json(&target, text, sizeof(text)) > 0);
    TEST(strstr(text, "\"fir_bands\":4") != nullptr);
    TEST(strstr(text, "\"fir_band1_slope\"") != nullptr);
    TEST(strstr(text, "\"volume\":{\"base_value\":0.25") != nullptr);

    // A single-band instrument omits the count key: the default keeps the
    // document short.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> single_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& single            = *single_storage;
        Synth::Instrument            single_instrument = {};
        build_m2_bandpass_fixture<m2_band_state_present>(&single, single_instrument.layers[0], false, false);
        // The fixture fills layer 0 only; the encoder needs the count set.
        single_instrument.layer_count = 1;
        set_default_dispersion_sections(single_instrument);
        single.bank.channel_enabled[0] = 1;
        TEST(single.bank.instruments.allocate() == 0);
        single.bank.instruments.entries[0]         = single_instrument;
        single.bank.channel_zones[0][0].start_note = 1;
        single.bank.channel_zones[0][0].instrument = 0;
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> single_doc_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&single_doc)[clipboard_capacity] = single_doc_storage->elements;
        TEST(Synth::encode_editor_bank_json(&single, single_doc, sizeof(single_doc)) > 0);
        TEST(strstr(single_doc, "fir_bands") == nullptr);
    }
}

// C2.4: library records round-trip multi-band state end to end, and zone
// splits carry the per-band generator bindings to the cloned zone.
static void m2_library_zone_regressions()
{
    TEST(Synth::num_mod_targets == 29);
    if (Synth::num_mod_targets != 29) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    build_m2_bandpass_fixture<m2_band_state_present>(&source, instrument.layers[0], true, false);
    // The fixture fills layer 0 only; the encoder needs the count set.
    instrument.layer_count = 1;
    set_default_dispersion_sections(instrument);
    source.bank.channel_enabled[0] = 1;
    TEST(source.bank.instruments.allocate() == 0);
    source.bank.instruments.entries[0]         = instrument;
    source.bank.channel_zones[0][0].start_note = 1;
    source.bank.channel_zones[0][0].instrument = 0;
    // The record stores the playback form: wire layer 0 (identity row),
    // like every other library save fixture.
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    const char* const path = "synth_m2_multiband_roundtrip.tmp";
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "Multi Band", &source, 0) == 0);
    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(path, &entry, 1) == 1);
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& destination = *destination_storage;
    uint16_t                     output_slot = 0;
    TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &output_slot));

    // The loaded record projects all four band blocks, so the count and
    // the band state round-tripped through the bank JSON inside the record.
    {
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(destination, &graph, &mapping, 2, 0));
        TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
        if (mapping.osc_nodes[0] != Sculptor::pool_no_slot) {
            const Sculptor::Node& osc = graph.node(mapping.osc_nodes[0]);
            TEST(osc.slots.num_allocated == Sculptor::osc_band_first_slot + 4u * Sculptor::osc_band_block_stride);
            for (uint32_t band = 0; band < 4; band++) {
                TEST(find_nth_osc_slot(osc, band_header_names_m2[band], 0) == Sculptor::osc_band_header_slot(band));
            }
        }
    }

    // Zone split: the cloned zone carries the band generators with fresh
    // envelope/LFO pool slots and identical contents.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> before_split_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& before_split = *before_split_storage;
        before_split                              = destination;
        TEST(Synth::zone_split_new(destination.bank.channel_zones[2], 0, 60, &destination));
        TEST(Sculptor::zone_records_split_copy(&destination, 2, 0));
        TEST(Sculptor::clone_split_generators(&destination, 2, 0));
        const Synth::Instrument& source_instrument =
            before_split.bank.instruments.entries[before_split.bank.channel_zones[2][0].instrument];
        const Synth::Instrument& cloned_instrument =
            destination.bank.instruments.entries[destination.bank.channel_zones[2][1].instrument];
        for (uint32_t band = 0; band < 4; band++) {
            for (uint32_t p = 0; p < 4; p++) {
                const uint32_t              target         = m2_band_targets[band][p];
                const Synth::TargetBinding& source_binding = source_instrument.layers[0].targets[target];
                const Synth::TargetBinding& cloned_binding = cloned_instrument.layers[0].targets[target];
                if (source_binding.envelope_desc_id != 0) {
                    TEST(cloned_binding.envelope_desc_id != source_binding.envelope_desc_id);
                    TEST(memcmp(&before_split.bank.envelopes.entries[source_binding.envelope_desc_id - 1],
                                &destination.bank.envelopes.entries[cloned_binding.envelope_desc_id - 1],
                                sizeof(Synth::EnvelopeDescriptor)) == 0);
                }
                if (source_binding.lfo_desc_id != 0) {
                    TEST(cloned_binding.lfo_desc_id != source_binding.lfo_desc_id);
                    TEST(memcmp(&before_split.bank.lfos.entries[source_binding.lfo_desc_id - 1],
                                &destination.bank.lfos.entries[cloned_binding.lfo_desc_id - 1],
                                sizeof(Synth::LFODescriptor)) == 0);
                }
            }
        }
    }
    remove(path);
}

// Fix-wave regressions: legacy bank backfill, the bandpass engagement gate,
// the fresh bandpass default, the 255 descriptor-id bound, the slope copy on
// [+] and the always-visible count-bound buttons.
static void m2_fix_wave_regressions()
{
    // A legacy single-band bandpass bank document (no fir_bands, no band
    // keys) decodes with band 0 gain 1.0 and offset 0, and re-saving keeps
    // the audible state; a non-bandpass layer keeps the zeroed gain binding.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        Synth::init_default_bank(&source.bank);
        source.bank.channel_enabled[0]                  = 1;
        Synth::Oscillator& layer                        = source.bank.instruments.entries[0].layers[0];
        layer.fir_type                                  = Synth::fir_bandpass;
        layer.targets[Synth::mod_fir_spread].base_value = 3.0f;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> legacy_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&legacy)[Synth::library_payload_max] = legacy_storage->elements;
        const uint32_t legacy_len                  = Synth::encode_editor_bank_json(&source, legacy, sizeof(legacy));
        TEST(legacy_len > 0);
        TEST(strstr(legacy, "fir_band0_gain") == nullptr); // the legacy document carries no band keys
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        TEST(Synth::decode_editor_bank_json(legacy, legacy_len, &loaded));
        const Synth::Oscillator& decoded = loaded.bank.instruments.entries[0].layers[0];
        TEST(decoded.fir_band_count == 1);
        TEST(decoded.targets[Synth::mod_fir_band0_gain].base_value == 1.0f);
        TEST(decoded.targets[Synth::mod_fir_band0_offset].base_value == 0.0f);
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> text_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&text)[Synth::library_payload_max] = text_storage->elements;
        TEST(Synth::encode_editor_bank_json(&loaded, text, sizeof(text)) > 0);
        TEST(strstr(text, "\"fir_band0_gain\":{\"base_value\":1") != nullptr);
    }
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        Synth::init_default_bank(&source.bank);
        source.bank.channel_enabled[0] = 1;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> legacy_off_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&legacy_off)[Synth::library_payload_max] = legacy_off_storage->elements;
        const uint32_t legacy_off_len = Synth::encode_editor_bank_json(&source, legacy_off, sizeof(legacy_off));
        TEST(legacy_off_len > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        TEST(Synth::decode_editor_bank_json(legacy_off, legacy_off_len, &loaded));
        TEST(loaded.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.0f);
    }
    // Staging mirror of the engagement rules: any band with positive
    // spread stages an entry whose gain word is the production clamp
    // result, so a zero-gain bandpass stages a zeroed kernel (every
    // staged gain word zero: silence) while all spreads zero stay
    // passthrough.
    {
        const uint32_t count      = 4;
        const float    spreads[4] = { 6.0f, 6.0f, 6.0f, 6.0f };
        const float    gains[4]   = { 0.5f, 1.5f, 0.0f, -0.25f };
        bool           any_staged = false;
        for (uint32_t band = 0; band < count; band++) {
            float    lowpass_hz  = 0.0f;
            float    highpass_hz = 0.0f;
            uint32_t span        = 0;
            if (Synth::oscillator_fir_band_cutoffs(69.0f,
                                                   0.0f,
                                                   spreads[band],
                                                   1,
                                                   100.0f,
                                                   &lowpass_hz,
                                                   &highpass_hz,
                                                   &span)) {
                any_staged = true;
                // The staged gain word is the clamp result: above 1 clamps
                // to 1, a nonpositive or NaN gain clamps to 0, so a band
                // whose gain clamps to zero contributes nothing to the
                // accumulation (gain * kernel / norm stays zero).
                float gain = gains[band];
                if (gain > 1.0f) {
                    gain = 1.0f;
                }
                if (! (gain > 0.0f)) {
                    gain = 0.0f;
                }
                const float want[4] = { 0.5f, 1.0f, 0.0f, 0.0f };
                TEST(gain == want[band]);
            }
        }
        TEST(any_staged); // every spread positive: the filter stages
    }
    {
        // All-zero gains with positive spreads: the entry still stages and
        // every staged gain word is zero, so the kernel is the zero kernel.
        const uint32_t count          = 2;
        const float    spreads[2]     = { 6.0f, 6.0f };
        const float    gains[2]       = { 0.0f, 0.0f };
        bool           any_staged     = false;
        bool           all_gains_zero = true;
        for (uint32_t band = 0; band < count; band++) {
            float    lowpass_hz  = 0.0f;
            float    highpass_hz = 0.0f;
            uint32_t span        = 0;
            if (Synth::oscillator_fir_band_cutoffs(69.0f,
                                                   0.0f,
                                                   spreads[band],
                                                   1,
                                                   100.0f,
                                                   &lowpass_hz,
                                                   &highpass_hz,
                                                   &span)) {
                any_staged = true;
                float gain = gains[band];
                if (gain > 1.0f) {
                    gain = 1.0f;
                }
                if (! (gain > 0.0f)) {
                    gain = 0.0f;
                }
                if (gain != 0.0f) {
                    all_gains_zero = false;
                }
            }
        }
        TEST(any_staged);     // positive spreads stage the entry
        TEST(all_gains_zero); // every staged gain word zero: the zeroed kernel
    }
    {
        bool        all_inactive    = true;
        const float zero_spreads[2] = { 0.0f, 0.0f };
        for (uint32_t band = 0; band < 2u; band++) {
            float    lowpass_hz  = 0.0f;
            float    highpass_hz = 0.0f;
            uint32_t span        = 0;
            if (Synth::oscillator_fir_band_cutoffs(69.0f,
                                                   0.0f,
                                                   zero_spreads[band],
                                                   1,
                                                   100.0f,
                                                   &lowpass_hz,
                                                   &highpass_hz,
                                                   &span)) {
                all_inactive = false;
            }
        }
        TEST(all_inactive); // every spread zero: passthrough, no entry
    }
    // The 255 descriptor-id bound: a binding to the last existing envelope
    // descriptor validates, a binding past the pool refuses.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        while (bank.envelopes.num_allocated < 254) {
            const uint32_t             slot = bank.envelopes.allocate();
            Synth::EnvelopeDescriptor& env  = bank.envelopes.entries[slot];
            env.num_points                  = 1;
            env.sustain_first_point         = 0;
            env.sustain_last_point          = 0;
            env.min_value                   = 0.0f;
            env.min_max_delta               = 1.0f;
            env.points[0]                   = { 0, 0x4000 };
        }
        Synth::Instrument& instrument = bank.instruments.entries[bank.channel_zones[0][0].instrument];
        instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = 254;
        TEST(Synth::validate_instrument_bank(&bank));
        instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = 255;
        TEST(! Synth::validate_instrument_bank(&bank));
    }
    // Editor: [+] copies the previous band's displayed slope (37% stays 37%),
    // and switching to bandpass seeds an audible gain default.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           bank          = *bank_storage;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        if (begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, false)) {
            bank.bank.instruments.entries[0].layers[0].fir_slope_percent = 37;
            TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
            const uint32_t          osc_node  = mapping.osc_nodes[0];
            const uint32_t          band_slot = find_nth_osc_slot(graph.node(osc_node), "Band 1", 0);
            const uint32_t          plus_slot = band_slot + 2; // the header's [-] sits at +1, [+] at +2
            Sculptor::PropertyValue press     = {};
            press.toggle                      = true;
            TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, plus_slot, press));
            TEST(bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band1_slope].base_value == 37.0f);
            TEST(bank.bank.instruments.entries[0].layers[0].fir_band_count == 2);
            TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
            // The band 2 block is projected after the insert and both
            // buttons stay live at two bands.
            TEST(find_nth_osc_slot(graph.node(osc_node), "Band 2", 0) == band_slot + Sculptor::osc_band_block_stride);
            TEST(! graph.slot_edit_disabled(osc_node, plus_slot - 1));
            TEST(! graph.slot_edit_disabled(osc_node, plus_slot));
        }
    }
}

static void m2_fix2_wave_regressions()
{
    // The composed render-visibility seam.  Under the single-edge filter
    // types the legacy cutoff and FIR Slope rows keep showing while the
    // band UI hides wholesale; under bandpass they are band 0's rows and
    // every existing band's block shows alongside them.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           bank          = *bank_storage;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        if (begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, false)) {
            const uint32_t osc_node    = mapping.osc_nodes[0];
            const uint32_t filter_slot = find_nth_osc_slot(graph.node(osc_node), "Filter Type", 0);
            const uint32_t band_slot   = find_nth_osc_slot(graph.node(osc_node), "Band 1", 0);
            TEST(filter_slot != Sculptor::pool_no_slot && band_slot != Sculptor::pool_no_slot);
            const auto hidden = [&](const uint32_t slot) {
                return Sculptor::osc_node_slot_hidden(graph, osc_node, slot);
            };
            // Bandpass: band 1's whole block shows, header and buttons included.
            TEST(! hidden(150) && ! hidden(151) && ! hidden(152));
            TEST(! hidden(153) && ! hidden(166) && ! hidden(179) && ! hidden(192));
            // The single-edge filter types keep the legacy cutoff and FIR
            // Slope rows visible while the band UI hides wholesale.
            for (uint32_t type = 0; type < Synth::num_filter_types; type++) {
                if (type == static_cast<uint32_t>(Synth::fir_bandpass)) {
                    continue;
                }
                Sculptor::PropertyValue select_type = {};
                select_type.list_index              = static_cast<uint8_t>(type);
                graph.set_slot_value(osc_node, filter_slot, select_type);
                TEST(! hidden(166) && ! hidden(192));
                TEST(hidden(150) && hidden(151) && hidden(152) && hidden(153) && hidden(179));
            }
        }
    }
    // An explicit zero band gain survives the bank round trip: the
    // encoder emits the empty routing block for a base-0 unwired gain,
    // the decode keeps it at 0 (no absent-default backfill), and a
    // re-encode is byte-stable.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        Synth::init_default_bank(&source.bank);
        source.bank.channel_enabled[0]                        = 1;
        Synth::Oscillator& layer                              = source.bank.instruments.entries[0].layers[0];
        layer.fir_type                                        = Synth::fir_bandpass;
        layer.fir_band_count                                  = 2;
        layer.targets[Synth::mod_fir_spread].base_value       = 3.0f;
        layer.targets[Synth::mod_fir_slope].base_value        = 100.0f;
        layer.targets[Synth::mod_fir_band0_gain].base_value   = 0.0f; // band 0: the inactive band
        layer.targets[Synth::mod_fir_band1_spread].base_value = 6.0f;
        layer.targets[Synth::mod_fir_band1_gain].base_value   = 0.5f;
        layer.targets[Synth::mod_fir_band1_slope].base_value  = 80.0f;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> text_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&text)[Synth::library_payload_max] = text_storage->elements;
        const uint32_t text_len                  = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(text_len > 0);
        TEST(strstr(text, "\"fir_band0_gain\":{}") != nullptr); // the empty block states the gain-0 band
        TEST(strstr(text, "\"fir_band1_gain\":{\"base_value\":0.5") != nullptr);
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        TEST(Synth::decode_editor_bank_json(text, text_len, &loaded));
        TEST(loaded.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.0f);
        TEST(loaded.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band1_gain].base_value == 0.5f);
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> again_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&again)[Synth::library_payload_max] = again_storage->elements;
        const uint32_t again_len                  = Synth::encode_editor_bank_json(&loaded, again, sizeof(again));
        TEST(again_len == text_len);
        TEST(memcmp(text, again, text_len) == 0);
    }
    // The empty-block emission is a bandpass-only policy: a non-bandpass
    // layer with a retained fir_band_count > 1 (legacy or hand-edited)
    // keeps the legacy serialization, so its zeroed unwired band gain
    // stays omitted and the document round trips byte-stable.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        Synth::init_default_bank(&source.bank);
        source.bank.channel_enabled[0]                      = 1;
        Synth::Oscillator& layer                            = source.bank.instruments.entries[0].layers[0];
        layer.fir_type                                      = Synth::fir_lowpass;
        layer.fir_band_count                                = 2;
        layer.targets[Synth::mod_fir_spread].base_value     = 1000.0f;
        layer.targets[Synth::mod_fir_slope].base_value      = 100.0f; // routed, so the decode keeps it
        layer.targets[Synth::mod_fir_band0_gain].base_value = 0.0f;   // zeroed unwired band gain
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> text_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&text)[Synth::library_payload_max] = text_storage->elements;
        const uint32_t text_len                  = Synth::encode_editor_bank_json(&source, text, sizeof(text));
        TEST(text_len > 0);
        TEST(strstr(text, "fir_band0_gain") == nullptr); // no empty gain block for a non-bandpass layer
        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& loaded = *loaded_storage;
        TEST(Synth::decode_editor_bank_json(text, text_len, &loaded));
        TEST(loaded.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.0f);
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> again_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&again)[Synth::library_payload_max] = again_storage->elements;
        const uint32_t again_len                  = Synth::encode_editor_bank_json(&loaded, again, sizeof(again));
        TEST(again_len == text_len);
        TEST(memcmp(text, again, text_len) == 0);
    }
    // The filter-type switch seeds an unwired Band Gain row at 1.0
    // through the real editor apply path, and a fresh oscillator's
    // default band state is one band with offset 0, spread 0 and the
    // seeded gain; re-selecting the already-stored bandpass type must
    // not reset a hand-set unwired gain.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           bank          = *bank_storage;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        if (begin_m2_band_graph<m2_band_state_present>(&bank, &graph, &mapping, false, false)) {
            Synth::Oscillator fresh                    = Synth::Oscillator{};
            fresh.osc_type[0]                          = Synth::WaveType::sine_wave;
            bank.bank.instruments.entries[0].layers[0] = fresh;
            TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
            const uint32_t osc_node    = mapping.osc_nodes[0];
            const uint32_t filter_slot = find_nth_osc_slot(graph.node(osc_node), "Filter Type", 0);
            TEST(filter_slot != Sculptor::pool_no_slot);
            if (filter_slot == Sculptor::pool_no_slot) {
                return;
            }
            TEST(bank.bank.instruments.entries[0].layers[0].fir_band_count == 1);
            TEST(bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.0f);
            Sculptor::PropertyValue select = {};
            select.list_index              = static_cast<uint8_t>(Synth::fir_bandpass);
            TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, filter_slot, select));
            const Synth::Oscillator& switched = bank.bank.instruments.entries[0].layers[0];
            TEST(switched.fir_type == Synth::fir_bandpass);
            TEST(switched.fir_band_count == 1);
            TEST(switched.targets[Synth::mod_fir_band0_gain].base_value == 1.0f); // seeded audible default
            TEST(switched.targets[Synth::mod_fir_band0_offset].base_value == 0.0f);
            TEST(switched.targets[Synth::mod_fir_spread].base_value == 0.0f);
            // A hand-set unwired gain survives re-selecting bandpass.
            Sculptor::PropertyValue hand = {};
            hand.real                    = 0.25f;
            const uint32_t gain_row      = Sculptor::osc_target_row_slot[Synth::mod_fir_band0_gain];
            TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, gain_row, hand));
            TEST(bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.25f);
            TEST(apply_m2_slot_edit(&bank, &graph, &mapping, osc_node, filter_slot, select));
            TEST(bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.25f);
        }
    }
}

// FIR pitch spread helpers: the band-pass edge composition, the sub-1-Hz
// highpass floor transport, the integer bandwidth guard and the
// spread-driven monotonicity contract.
static void check_fir_spread_helpers()
{
    float lowpass_hz  = 0.0f;
    float highpass_hz = 0.0f;
    TEST(Synth::oscillator_fir_cutoffs(69.0f, 12.0f, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(approx(lowpass_hz, Synth::pitch_to_frequency(81.0f, 1), 0.001f));
    TEST(approx(highpass_hz, Synth::pitch_to_frequency(57.0f, 1), 0.001f));
    TEST(approx(lowpass_hz, 880.0f, 0.001f)); // absolute anchor
    TEST(approx(highpass_hz, 220.0f, 0.001f));
    // The frequency multiplier scales both edges.
    TEST(Synth::oscillator_fir_cutoffs(69.0f, 12.0f, 3, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(approx(lowpass_hz, 2640.0f, 0.001f));
    TEST(approx(highpass_hz, 660.0f, 0.001f));
    // Filter-off spellings: zero, negative and NaN spreads are passthrough.
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, 0.0f, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, -0.5f, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, NAN, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    // The sub-1-Hz highpass edge transports as exactly 0 Hz (pure lowpass),
    // while a comfortably-above-floor edge stays a real band-pass edge.
    TEST(Synth::oscillator_fir_cutoffs(-36.0f, 12.0f, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(approx(lowpass_hz, Synth::pitch_to_frequency(-24.0f, 1), 0.001f));
    TEST(approx(lowpass_hz, 2.04f, 0.01f));
    TEST(highpass_hz == 0.0f); // floor-killed: converts to integer 0, not 1
    TEST(Synth::oscillator_fir_cutoffs(69.0f, 12.0f, 1, Synth::fir_bandpass, &lowpass_hz, &highpass_hz));
    TEST(highpass_hz > Synth::oscillator_fir_highpass_floor_hz);
    // Filter types: low-pass and high-pass take the value as a fixed Hz cutoff
    // (no key tracking, no freq_mult), off never filters.
    TEST(Synth::oscillator_fir_cutoffs(69.0f, 440.0f, 3, Synth::fir_lowpass, &lowpass_hz, &highpass_hz));
    TEST(lowpass_hz == 440.0f && highpass_hz == 0.0f);
    TEST(Synth::oscillator_fir_cutoffs(10.0f, 1000.0f, 1, Synth::fir_highpass, &lowpass_hz, &highpass_hz));
    TEST(lowpass_hz == 0.0f && highpass_hz == 1000.0f);
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, 12.0f, 1, Synth::fir_off, &lowpass_hz, &highpass_hz));
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, 0.0f, 1, Synth::fir_lowpass, &lowpass_hz, &highpass_hz));
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, -1.0f, 1, Synth::fir_highpass, &lowpass_hz, &highpass_hz));
    TEST(! Synth::oscillator_fir_cutoffs(69.0f, NAN, 1, Synth::fir_lowpass, &lowpass_hz, &highpass_hz));

    // The integer bandwidth guard keeps a positive spread audible.
    uint32_t lowpass  = 440;
    uint32_t highpass = 440;
    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
    TEST(lowpass == 441 && highpass == 440); // zero bandwidth widens upward
    lowpass  = Synth::max_cutoff_hz;
    highpass = Synth::max_cutoff_hz;
    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
    TEST(lowpass == Synth::max_cutoff_hz && highpass == Synth::max_cutoff_hz - 1); // ceiling widens downward
    lowpass  = 441;
    highpass = 440;
    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
    TEST(lowpass == 441 && highpass == 440); // a real band-pass is untouched
    lowpass  = 440;
    highpass = 0;
    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
    TEST(lowpass == 440 && highpass == 0); // pure lowpass is untouched
    lowpass  = 440;
    highpass = 441;
    Synth::oscillator_fir_band_guard(&lowpass, &highpass);
    TEST(lowpass == 442 && highpass == 441); // defensive inverted-input repair
    // Monotonicity sweep: a true return always yields finite, ordered edges.
    const float    pitches[] = { -36.0f, 0.0f, 69.0f, 127.0f };
    const float    spreads[] = { 0.5f, 3.0f, 12.0f, 36.0f };
    const uint32_t mults[]   = { 1, 2, 3 };
    for (const float pitch : pitches) {
        for (const float spread : spreads) {
            for (const uint32_t mult : mults) {
                float lo = 0.0f;
                float hi = 0.0f;
                if (Synth::oscillator_fir_cutoffs(pitch, spread, mult, Synth::fir_bandpass, &lo, &hi)) {
                    TEST(lo == lo && hi == hi); // not NaN
                    TEST(hi <= lo);
                }
            }
        }
    }
    // F3 mirrors: the note-on reserve predicate and the engage transition,
    // driven by real binding fields and the production transition shape.
    {
        Synth::Instrument mirror_instr = {};
        mirror_instr.layer_count       = 1;
        set_default_dispersion_sections(mirror_instr);
        auto has_filter = [](const Synth::Instrument& instrument, uint32_t layer_idx) {
            const Synth::Oscillator&    layer  = instrument.layers[layer_idx];
            const Synth::TargetBinding& spread = layer.targets[Synth::mod_fir_spread];
            return spread.base_value != 0.0f || spread.envelope_desc_id != 0 || spread.lfo_desc_id != 0 ||
                   spread.num_inputs != 0;
        };
        // Reserved by base value alone.
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].base_value = 3.0f;
        TEST(has_filter(mirror_instr, 0));
        // A zero base with no wiring is passthrough; each wiring term alone
        // reserves the slot at note-on.
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].base_value = 0.0f;
        TEST(! has_filter(mirror_instr, 0));
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].lfo_desc_id = 1;
        TEST(has_filter(mirror_instr, 0));
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].lfo_desc_id = 0;
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].num_inputs  = 1;
        TEST(has_filter(mirror_instr, 0));
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].num_inputs       = 0;
        mirror_instr.layers[0].targets[Synth::mod_fir_spread].envelope_desc_id = 1;
        TEST(has_filter(mirror_instr, 0));
        // The engage transition mirrors the single-source activity
        // predicate: passthrough while no staged band's spread is positive,
        // engaged on the first positive step, and re-engaged after every
        // return to passthrough.  Band 1's spread drives the edge while
        // band 0 stays zero, so a multi-band bandpass re-engages without a
        // band-0 spread transition.
        const bool     fir_taps_offs   = true;
        const uint32_t band_count      = 2;
        float          band_spreads[2] = { 0.0f, 0.0f };
        const auto     band_active     = [&]() {
            for (uint32_t band = 0; band < band_count; band++) {
                if (band_spreads[band] > 0.0f) {
                    return true;
                }
            }
            return false;
        };
        bool        active         = false;
        uint32_t    engage_count   = 0;
        const float band1_values[] = { -1.0f, 0.0f, 0.5f, 0.5f, 0.0f, 2.0f };
        for (const float new_spread : band1_values) {
            const bool prev_active = active;
            band_spreads[1]        = new_spread;
            active                 = band_active();
            if (fir_taps_offs && ! prev_active && active) {
                ++engage_count; // clear_fir_hist: the frozen history holds stale audio
            }
        }
        TEST(engage_count == 2);
        TEST(active);
    }
    // Schema identity: the mod_target enum carries the spread target and
    // no removed cutoff spellings, so a future schema edit cannot
    // silently reintroduce them.
}

int main()
{
    test_silence_stdout();
    check_fir_spread_helpers();
    check_fir_slope_helpers();
    osc_fir_slope_seed_regressions();
    fir_coeff_cache_action_regressions();
    osc_fir_slope_projection_regressions();
    check_fir_bandpass_gain();
    check_chan_combine_volume_ramp();
    m2_band_layout_regressions();
    m2_band_visibility_regressions();
    m2_band_source_gray_regressions();
    m2_band_add_remove_regressions();
    m2_band_remove_first_regressions();
    m2_band_donation_regressions();
    m2_band_batch_boundary_regressions();
    m2_band_button_gray_regressions();
    m2_per_band_modulation_regressions();
    m2_fir_kernel_mirror_regressions();
    fir_effect_bands_regressions();
    fir_effect_band_editor_regressions();
    m4_fix3_wave_regressions();
    fir_multiband_staging_regressions();
    fir_multiband_projection_regressions();
    fir_band_lfo_reclaim_regressions();
    m2_document_json_regressions();
    m2_bank_json_round_trip_regressions();
    m2_library_zone_regressions();
    m2_fix_wave_regressions();
    m2_fix2_wave_regressions();
    return test_exit_code();
}
