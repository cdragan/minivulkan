// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

// Zeroes every layer at or above layer_count: the canonical image decode
// produces, and the only image the per-layer JSON format can round-trip
// (the per-layer format stores live layers only).
static void zero_dormant_layers(Synth::Instrument& instrument)
{
    if (instrument.layer_count < Synth::max_layers) {
        memset(static_cast<void*>(&instrument.layers[instrument.layer_count]),
               0,
               (Synth::max_layers - instrument.layer_count) * sizeof(instrument.layers[0]));
    }
}

// Round-trip uniformity: an instrument whose live layers all carry the same
// routing (as the editor's fanned-out fixtures do) decodes with that routing
// intact on every live layer; decode itself no longer fans anything out.
static void check_routing_fan_out(const Synth::Instrument& instrument)
{
    for (uint32_t layer = 1; layer < instrument.layer_count; layer++) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            TEST(instrument.layers[layer].targets[t].base_value == instrument.layers[0].targets[t].base_value);
            TEST(instrument.layers[layer].targets[t].num_inputs == instrument.layers[0].targets[t].num_inputs);
            TEST(memcmp(&instrument.layers[layer].targets[t].inputs[0],
                        &instrument.layers[0].targets[t].inputs[0],
                        sizeof(Synth::ModInput) * Synth::max_mod_inputs) == 0);
        }
    }
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

// Captures a whole library file in bounded 64 KB chunks into a heap buffer and
// returns its exact size.  Refusal tests compare this snapshot with the file
// after the refused call, so any change anywhere in the file is caught.
static uint64_t snapshot_library_file(const char* path, std::unique_ptr<uint8_t[]>& snapshot)
{
    FILE* const file = fopen(path, "rb");
    TEST(file != nullptr);
    if (! file) {
        return 0;
    }
    TEST(fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    TEST(size > 0);
    TEST(fseek(file, 0, SEEK_SET) == 0);
    snapshot            = std::make_unique<uint8_t[]>(static_cast<uint64_t>(size));
    uint64_t read_total = 0;
    while (read_total < static_cast<uint64_t>(size)) {
        const uint32_t chunk = static_cast<uint64_t>(size) - read_total < 64 * 1024
                                   ? static_cast<uint32_t>(static_cast<uint64_t>(size) - read_total)
                                   : 64 * 1024;
        TEST(fread(snapshot.get() + read_total, 1, chunk, file) == chunk);
        read_total += chunk;
    }
    TEST(fgetc(file) == EOF); // the read stopped exactly at the end of the file
    TEST(! ferror(file));
    TEST(fclose(file) == 0);
    return static_cast<uint64_t>(size);
}

// Compares a file with a snapshot byte-for-byte in bounded 64 KB chunks; the
// exact final size must match too.
static bool library_file_matches_snapshot(const char*                       path,
                                          const std::unique_ptr<uint8_t[]>& snapshot,
                                          uint64_t                          snapshot_size)
{
    const std::unique_ptr<uint8_t[]> chunk = std::make_unique<uint8_t[]>(64 * 1024);
    FILE* const                      file  = fopen(path, "rb");
    TEST(file != nullptr);
    if (! file) {
        return false;
    }

    uint64_t compared = 0;
    while (true) {
        const size_t got = fread(chunk.get(), 1, 64 * 1024, file);
        if (compared + got > snapshot_size || (got && memcmp(snapshot.get() + compared, chunk.get(), got) != 0)) {
            TEST(fclose(file) == 0);
            return false;
        }
        compared += got;
        if (got < 64 * 1024) {
            break;
        }
    }

    TEST(feof(file) && ! ferror(file));
    TEST(fclose(file) == 0);
    return compared == snapshot_size;
}

// Model/predicate evidence combines waveform/mode availability with caller-owned gates.
static bool fm_widget_enabled(const Sculptor::Graph& graph, uint32_t node_index)
{
    const uint32_t        fm_depth = Sculptor::osc_target_row(Synth::mod_fm_index);
    const Sculptor::Node& node     = graph.node(node_index);
    return ! Sculptor::oscillator_slot_waveform_mode_disabled(node, fm_depth) &&
           ! graph.slot_edit_disabled(node_index, fm_depth) && ! graph.slot_is_connected(node_index, fm_depth) &&
           ! node.ghost;
}

static void check_named_fm_projection(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone)
{
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    const bool projected = Sculptor::project_editor_to_graph(bank, &graph, &mapping, channel, zone);
    TEST(projected);
    if (! projected) {
        return;
    }
    const uint32_t           slot       = bank.bank.channel_zones[channel][zone].instrument;
    const Synth::Instrument& instrument = bank.bank.instruments.entries[slot];
    TEST(instrument.layer_count == 3);
    TEST(approx(instrument.layers[0].targets[Synth::mod_fm_index].base_value, 0.625f * 6.2831853f, 0.00001f));
    TEST(instrument.layers[0].targets[Synth::mod_volume].num_inputs == 1);
    TEST(instrument.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
    TEST(instrument.layers[0].targets[Synth::mod_volume].inputs[0].op == Synth::SourceOp::add);
    TEST(instrument.layers[0].targets[Synth::mod_volume].inputs[0].scale == 2.0f);
    for (uint32_t layer = 0; layer < instrument.layer_count; ++layer) {
        const uint32_t        node_index = mapping.osc_nodes[layer];
        const Sculptor::Node& node       = graph.node(node_index);
        TEST(instrument.layers[layer].osc_type[1] == Synth::WaveType::sine_wave);
        TEST(instrument.layers[layer].osc_mode == Synth::osc_mode_fm);
        TEST(instrument.layers[layer].mod_ratio == 2);
        const Synth::TargetBinding& generator = instrument.layers[layer].targets[Synth::mod_volume];
        TEST(generator.envelope_desc_id != 0 && generator.lfo_desc_id != 0);
        TEST(generator.lfo_op == Synth::SourceOp::add);
        TEST(generator.lfo_depth == 0.5f);
        TEST(generator.lfo_depth_source == Synth::ModSource::velocity);
        TEST(generator.lfo_rate_source == Synth::ModSource::mod_wheel);
        TEST(generator.lfo_rate_scale_ms == 20);
        const Synth::EnvelopeDescriptor& envelope = bank.bank.envelopes.entries[generator.envelope_desc_id - 1];
        TEST(envelope.num_points == 2);
        TEST(approx(envelope.min_value + envelope.points[0].value * envelope.min_max_delta,
                    (layer == 1 ? -0.5f : -1.0f) + 0x2000 * 2.0f,
                    0.01f));
        TEST(bank.bank.lfos.entries[generator.lfo_desc_id - 1].period_ms == 300);
        TEST(fm_widget_enabled(graph, node_index));
        TEST(approx(node.slots.entries[Sculptor::osc_target_row(Synth::mod_fm_index)].value.real, 0.625f, 0.000001f));
        TEST(graph.slot_is_connected(node_index, Sculptor::osc_target_lfo_dot(Synth::mod_volume)));
    }
    const Sculptor::Node& oscillator = graph.node(mapping.osc_nodes[0]);
    TEST(oscillator.position.x == 77 && oscillator.position.y == 88);
    TEST(oscillator.content_width_override == 321 && oscillator.content_height_override == 222);
    for (uint32_t canonical = 0; canonical < 2; ++canonical) {
        const int32_t saved = Sculptor::find_record(bank, channel, zone, 0, canonical, 0);
        TEST(saved >= 0);
        if (saved >= 0) {
            const Synth::GraphNodeLayout& record = bank.graph_layout[saved];
            TEST(record.x == 500.0f + 100.0f * static_cast<float>(canonical));
            TEST(record.y == -400.0f - 50.0f * static_cast<float>(canonical));
            TEST(record.width_override == 350.0f + 10.0f * static_cast<float>(canonical));
            TEST(record.height_override == 250.0f + 20.0f * static_cast<float>(canonical));
        }
    }
    const Sculptor::Node& input = graph.node(mapping.input_node);
    TEST(input.position.x == 500 && input.position.y == -400);
    TEST(input.content_width_override == 350 && input.content_height_override == 250);
    const Sculptor::Node& sum = graph.node(mapping.output_node);
    TEST(sum.position.x == 600 && sum.position.y == -450);
    TEST(sum.content_width_override == 360 && sum.content_height_override == 270);
    const uint32_t          wave_b_slot   = 42; // waveform b, layout contract
    const uint32_t          mode_slot     = 69; // mix mode, layout contract
    const uint32_t          fm_depth_slot = Sculptor::osc_target_row(Synth::mod_fm_index);
    const uint32_t          fm_ratio_slot = 96; // fm ratio, layout contract
    Sculptor::PropertyValue wave          = {};
    wave.list_index                       = 0;
    graph.set_slot_value(mapping.osc_nodes[0], wave_b_slot, wave);
    TEST(! fm_widget_enabled(graph, mapping.osc_nodes[0]));
    wave.list_index = 1;
    graph.set_slot_value(mapping.osc_nodes[0], wave_b_slot, wave);
    wave.list_index = 0;
    graph.set_slot_value(mapping.osc_nodes[0], mode_slot, wave);
    TEST(! fm_widget_enabled(graph, mapping.osc_nodes[0]));
    const uint32_t node_index = mapping.osc_nodes[0];
    for (uint32_t mode = 0; mode <= static_cast<uint32_t>(Synth::osc_mode_ring_mod); ++mode) {
        wave.list_index = static_cast<uint8_t>(mode);
        graph.set_slot_value(node_index, mode_slot, wave);
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), fm_depth_slot) == (mode != 1));
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), fm_ratio_slot) == (mode == 0));
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index),
                                                              Sculptor::osc_target_row(Synth::mod_osc_mix)) ==
             (mode == 1 || mode == 2));
        wave.list_index = 0;
        graph.set_slot_value(node_index, wave_b_slot, wave);
        // With waveform b off, exactly the duty b / mix mode / waveform mix /
        // fm depth / fm ratio rows plus both pad tone families and the spread
        // family are gated, across all 136 slots.  Waveform a is saw in this
        // fixture, so the duty a family stays gated-off.
        const uint32_t wave_a_list  = graph.node(node_index).slots.entries[1].value.list_index;
        const bool     tone_a_gated = wave_a_list != static_cast<uint32_t>(Synth::WaveType::pad_wave) - 1;
        const uint32_t tone0        = Sculptor::osc_target_row(Synth::mod_pad_tilt0);
        const uint32_t tone1        = Sculptor::osc_target_row(Synth::mod_pad_tilt1);
        for (uint32_t row = 0; row < graph.node(node_index).slots.num_allocated; ++row) {
            const bool gated =
                (row >= Sculptor::osc_string_extras_first_slot && row <= Sculptor::osc_brightness_slot) ||
                row == Sculptor::osc_target_row(Synth::mod_duty1) || row == mode_slot ||
                row == Sculptor::osc_target_row(Synth::mod_osc_mix) || row == fm_depth_slot || row == fm_ratio_slot ||
                (tone_a_gated && row >= tone0 && row <= tone0 + Sculptor::osc_target_block_stride) ||
                (row >= tone1 && row <= tone1 + Sculptor::osc_target_block_stride) ||
                (row >= Sculptor::osc_target_row(Synth::mod_fir_spread) &&
                 row <= Sculptor::osc_target_row(Synth::mod_fir_slope) + Sculptor::osc_target_block_stride);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
        wave.list_index = 1;
        graph.set_slot_value(node_index, wave_b_slot, wave);
    }
    wave.list_index = 1;
    graph.set_slot_value(node_index, mode_slot, wave);
    // Saw wave a keeps duty a live; sine wave b gates the duty b family.
    // Mode 1 keeps fm depth and fm ratio live; both tone families and the
    // spread family stay gated.
    {
        const uint32_t duty1_row  = Sculptor::osc_target_row(Synth::mod_duty1);
        const uint32_t tone0      = Sculptor::osc_target_row(Synth::mod_pad_tilt0);
        const uint32_t tone1      = Sculptor::osc_target_row(Synth::mod_pad_tilt1);
        const uint32_t mix_row    = Sculptor::osc_target_row(Synth::mod_osc_mix);
        const uint32_t spread_row = Sculptor::osc_target_row(Synth::mod_fir_spread);
        for (uint32_t row = 0; row < graph.node(node_index).slots.num_allocated; ++row) {
            const bool gated =
                (row >= Sculptor::osc_string_extras_first_slot && row <= Sculptor::osc_brightness_slot) ||
                (row >= duty1_row && row <= duty1_row + Sculptor::osc_target_block_stride) ||
                (row >= tone0 && row <= tone0 + Sculptor::osc_target_block_stride) ||
                (row >= tone1 && row <= tone1 + Sculptor::osc_target_block_stride) || row == mix_row ||
                (row >= spread_row &&
                 row <= Sculptor::osc_target_row(Synth::mod_fir_slope) + Sculptor::osc_target_block_stride);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
    }
    // Positive direction of the tone gates: wave a at the pad list entry
    // (the wave a list starts at sine, so pad is one below the stored wave)
    // re-enables tone a while wave b stays off.
    wave.list_index = 4;
    graph.set_slot_value(node_index, 1, wave);
    wave.list_index = 0;
    graph.set_slot_value(node_index, wave_b_slot, wave);
    {
        const uint32_t tone_b_row = Sculptor::osc_target_row(Synth::mod_pad_tilt1);
        const uint32_t spread_row = Sculptor::osc_target_row(Synth::mod_fir_spread);
        for (uint32_t row = 0; row < graph.node(node_index).slots.num_allocated; ++row) {
            const bool gated =
                (row >= Sculptor::osc_string_extras_first_slot && row <= Sculptor::osc_brightness_slot) ||
                (row >= tone_b_row && row <= tone_b_row + Sculptor::osc_target_block_stride) ||
                row == Sculptor::osc_target_row(Synth::mod_duty1) || row == mode_slot ||
                row == Sculptor::osc_target_row(Synth::mod_osc_mix) || row == fm_depth_slot || row == fm_ratio_slot ||
                (row >= spread_row &&
                 row <= Sculptor::osc_target_row(Synth::mod_fir_slope) + Sculptor::osc_target_block_stride);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
    }
    // Wave b at pad (5) enables tone b and un-gates the whole duty b family.
    // The spread family stays gated: the fixture keeps the fir_off default.
    // The mix row stays gated because the fixture mode is FM.
    wave.list_index = 5;
    graph.set_slot_value(node_index, wave_b_slot, wave);
    {
        const uint32_t spread_row = Sculptor::osc_target_row(Synth::mod_fir_spread);
        for (uint32_t row = 0; row < graph.node(node_index).slots.num_allocated; ++row) {
            const bool gated =
                (row >= Sculptor::osc_string_extras_first_slot && row <= Sculptor::osc_brightness_slot) ||
                (row >= spread_row &&
                 row <= Sculptor::osc_target_row(Synth::mod_fir_slope) + Sculptor::osc_target_block_stride) ||
                row == Sculptor::osc_target_row(Synth::mod_osc_mix);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
    }
    // Noise wave a gates the duty a family; pad wave b keeps duty b live.
    wave.list_index = 3;
    graph.set_slot_value(node_index, 1, wave);
    {
        const uint32_t duty0_row  = Sculptor::osc_target_row(Synth::mod_duty0);
        const uint32_t tone0      = Sculptor::osc_target_row(Synth::mod_pad_tilt0);
        const uint32_t mix_row    = Sculptor::osc_target_row(Synth::mod_osc_mix);
        const uint32_t spread_row = Sculptor::osc_target_row(Synth::mod_fir_spread);
        for (uint32_t row = 0; row < graph.node(node_index).slots.num_allocated; ++row) {
            const bool gated =
                (row >= Sculptor::osc_string_extras_first_slot && row <= Sculptor::osc_brightness_slot) ||
                (row >= duty0_row && row <= duty0_row + Sculptor::osc_target_block_stride) ||
                (row >= tone0 && row <= tone0 + Sculptor::osc_target_block_stride) || row == mix_row ||
                (row >= spread_row &&
                 row <= Sculptor::osc_target_row(Synth::mod_fir_slope) + Sculptor::osc_target_block_stride);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
    }
    // String wave a (list entry 5) reuses the duty a / pad tone a rows as String Dispersion /
    // Sustain Damping, so both families stay live (Release Damping and Pluck Brightness follow).
    wave.list_index = 5;
    graph.set_slot_value(node_index, 1, wave);
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index),
                                                            Sculptor::osc_target_row(Synth::mod_duty0)));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index),
                                                            Sculptor::osc_target_row(Synth::mod_duty0) +
                                                                Sculptor::osc_target_block_stride));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index),
                                                            Sculptor::osc_target_row(Synth::mod_pad_tilt0)));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), Sculptor::osc_brightness_slot));
    // Restore the contract state for the following widget tests.
    wave.list_index = 0;
    graph.set_slot_value(node_index, 1, wave);
    wave.list_index = 1;
    TEST(fm_widget_enabled(graph, node_index));
    graph.set_slot_edit_disabled(node_index, fm_depth_slot, true);
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), fm_depth_slot));
    TEST(! fm_widget_enabled(graph, node_index));
    graph.set_slot_edit_disabled(node_index, fm_depth_slot, false);
    TEST(fm_widget_enabled(graph, node_index));
    graph.set_ghost(node_index, true);
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), fm_depth_slot));
    TEST(! fm_widget_enabled(graph, node_index));
    graph.set_ghost(node_index, false);
    TEST(fm_widget_enabled(graph, node_index));
    // FM depth stays unwired in this fixture; a connected volume row uses the separate connection gate.
    TEST(! graph.slot_is_connected(node_index, fm_depth_slot));
    TEST(graph.slot_is_connected(node_index, Sculptor::osc_target_lfo_dot(Synth::mod_volume)));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index),
                                                            Sculptor::osc_target_lfo_dot(Synth::mod_volume)));
    TEST(fm_widget_enabled(graph, node_index));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(mapping.input_node), fm_depth_slot));
}

static void check_editor_instrument_round_trips()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> source_before_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& source_before = *source_before_storage;

    build_named_fm_fixture(&source);
    check_named_fm_projection(source, 0, 0);

    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    source_before                    = source;

    TEST(export_clipboard_fixture(source, text, sizeof(text)));
    TEST(memcmp(&source, &source_before, sizeof(source)) == 0);
    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;
    TEST(decode_clipboard(&decoded, text, max_portable_records));

    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& destination = *destination_storage;
    Synth::Instrument            occupied;
    build_clipboard_zone_fixture(&destination, &occupied);
    destination.bank.channel_enabled[2]             = 1;
    destination.bank.channel_zones[2][0].start_note = 1;
    destination.bank.channel_zones[2][0].instrument = 0;
    strcpy(destination.channel_names[2], "Destination");

    Synth::EffectSlotBinding* master =
        enabled_effect(destination.bank, Synth::max_channels, 0, Synth::EffectType::delay);
    master->bindings[0].lfo_desc_id = 1;
    add_detached_record(&destination,
                        Synth::max_channels,
                        0,
                        4,
                        0,
                        700,
                        701,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    strcpy(destination.graph_layout[0].name, "Delay");
    const Synth::GraphNodeLayout unrelated_effect = destination.graph_layout[0];

    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;
    const bool                   replaced  = Sculptor::replace_zone_instrument_candidate(destination,
                                                                                         2,
                                                                                         0,
                                                                                         decoded.instr,
                                                                                         decoded.envs,
                                                                                         decoded.env_count,
                                                                                         decoded.lfos,
                                                                                         decoded.lfo_count,
                                                                                         decoded.layout,
                                                                                         decoded.layout_count,
                                                                                         nullptr,
                                                                                         &candidate);
    TEST(replaced);

    if (replaced) {
        TEST(memcmp(candidate.effect_routing, destination.effect_routing, sizeof(destination.effect_routing)) == 0);
        check_named_fm_projection(candidate, 2, 0);
        TEST(candidate.bank.channel_zones[2][0].start_note == 1);
        TEST(strcmp(candidate.channel_names[2], "Destination") == 0);
        TEST(strcmp(candidate.instrument_names[candidate.bank.channel_zones[2][0].instrument], "Clipboard zone") == 0);
    }

    // Aliased zones have independent layout but share instrument/descriptor storage.
    const uint32_t second = source.bank.instruments.allocate();
    TEST(second == 1);
    source.bank.instruments.entries[second] = source.bank.instruments.entries[0];
    strcpy(source.instrument_names[second], "Shared descriptors");
    // Capture the fixture record count once: each zone appends the same
    // records, so a per-iteration recount would compound per zone.
    const uint32_t records = source.graph_layout_count;
    for (uint32_t zone = 1; zone < Synth::max_instr_per_channel; ++zone) {
        source.bank.channel_zones[0][zone].start_note = static_cast<uint8_t>(1 + 7 * zone);
        source.bank.channel_zones[0][zone].instrument = static_cast<uint8_t>(zone % 2);
        for (uint32_t r = 0; r < records; ++r) {
            Synth::GraphNodeLayout record = source.graph_layout[r];
            record.zone                   = static_cast<uint8_t>(zone);
            record.x += static_cast<float>(zone);
            source.graph_layout[source.graph_layout_count++] = record;
        }
    }

    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
        Sculptor::set_identity_osc_sources(&source, 0, zone);
    }

    const uint16_t            chain_lfo = add_detached_lfo_descriptor(&source);
    Synth::EffectSlotBinding* effect    = enabled_effect(source.bank, 0, 0, Synth::EffectType::delay);
    effect->bindings[0].lfo_desc_id     = chain_lfo;
    effect->bindings[0].lfo_depth       = 0.125f;
    add_detached_record(&source, 0, 0, 4, 0, 901, 902, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(source.graph_layout[source.graph_layout_count - 1].name, "Delay");
    add_detached_record(&source, 0, 0, 4, 0, 903, 904, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(source.graph_layout[source.graph_layout_count - 1].name, "LFO 2");

    const char* const path = "synth_named_fm_roundtrip.tmp";
    remove(path);
    source_before = source;
    TEST(Synth::save_library_record(path, "Regression", "Named FM", &source, 0) == 0);
    TEST(memcmp(&source, &source_before, sizeof(source)) == 0);

    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(path, &entry, 1) == 1);
    uint16_t output_slot = 0xBEEF;
    TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &output_slot));
    TEST(output_slot != 0 && output_slot != 0xBEEF);
    check_named_fm_projection(destination, 2, 0);
    TEST(destination.bank.instruments.num_allocated == 3);
    TEST(destination.bank.envelopes.num_allocated == 4);
    TEST(destination.bank.lfos.num_allocated == 3);

    for (uint32_t zone = 1; zone < Synth::max_instr_per_channel; ++zone) {
        TEST(destination.bank.channel_zones[2][zone].instrument == output_slot + zone % 2);
        TEST(destination.bank.channel_zones[2][zone].start_note == 1 + 7 * zone);
        uint32_t zone_records = 0;
        for (uint32_t r = 0; r < destination.graph_layout_count; ++r) {
            const Synth::GraphNodeLayout& record = destination.graph_layout[r];
            if (record.channel == 2 && record.zone == zone && record.kind < 4) {
                ++zone_records;
                if (record.kind == 0) {
                    const float base_x = record.index == 2 ? 77.0f : 500.0f + 100.0f * static_cast<float>(record.index);
                    TEST(record.x == base_x + static_cast<float>(zone));
                }
            }
        }
        TEST(zone_records == 4);
    }
    TEST(destination.bank.channel_chains[2].effects[0].bindings[0].lfo_desc_id == 3);
    TEST(count_records_matching(destination, 2, 0, 4, 0) == 2);

    bool remapped_lfo_title = false;
    bool preserved_master   = false;
    for (uint32_t r = 0; r < destination.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = destination.graph_layout[r];
        if (record.channel == 2 && record.kind == 4 && strcmp(record.name, "LFO 3") == 0) {
            remapped_lfo_title = record.x == 903 && record.y == 904;
        }
        if (record.channel == Synth::max_channels) {
            preserved_master = memcmp(&record, &unrelated_effect, sizeof(record)) == 0;
        }
    }
    TEST(remapped_lfo_title);
    TEST(preserved_master);
    TEST(destination.bank.master_chain.effects[0].bindings[0].lfo_desc_id == 1);
    TEST(strcmp(destination.channel_names[2], "Destination") == 0);
    TEST(destination.bank.channel_zones[0][0].instrument == 0);

    destination.bank.channel_enabled[2] = 1;
    Synth::reclaim_unused_slots(&destination);
    check_named_fm_projection(destination, 2, 0);
    TEST(destination.bank.lfos.num_allocated == 3);
    TEST(Sculptor::validate_editor_metadata(destination));

    bool reclaimed_title = false;
    for (uint32_t r = 0; r < destination.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = destination.graph_layout[r];
        if (record.channel == 2 && record.kind == 4 && strcmp(record.name, "LFO 3") == 0) {
            reclaimed_title = true;
        }
    }
    TEST(reclaimed_title);
    remove(path);
}

static void check_new_zone_metadata_clone()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    build_named_fm_fixture(&source);
    source.graph_layout[3].width_override  = 280;
    source.graph_layout[3].height_override = 180;
    const uint32_t records                 = source.graph_layout_count;

    candidate = source;
    TEST(Synth::zone_split_new(candidate.bank.channel_zones[0], 0, 60, &candidate));
    TEST(Sculptor::zone_records_split_copy(&candidate, 0, 0));
    TEST(Sculptor::clone_split_generators(&candidate, 0, 0));

    TEST(candidate.bank.channel_zones[0][0].start_note == 1);
    TEST(candidate.bank.channel_zones[0][1].start_note == 61);
    TEST(strcmp(candidate.instrument_names[candidate.bank.channel_zones[0][1].instrument], "Named FM") == 0);
    TEST(candidate.graph_layout_count == 2 * records);

    // Kind-1/2 records re-point at the clone's fresh descriptor ids; match
    // them by kind and uid, every other record copies verbatim.
    for (uint32_t r = 0; r < records; ++r) {
        const Synth::GraphNodeLayout& original  = source.graph_layout[r];
        const bool                    generator = original.kind == 1 || original.kind == 2;
        int32_t                       cloned    = -1;
        if (generator) {
            for (uint32_t i = 0; i < candidate.graph_layout_count; ++i) {
                const Synth::GraphNodeLayout& record = candidate.graph_layout[i];
                if (record.zone == 1 && record.kind == original.kind && record.uid == original.uid) {
                    cloned = static_cast<int32_t>(i);
                }
            }
        }
        else {
            cloned = Sculptor::find_record(candidate, 0, 1, original.kind, original.index, original.uid);
        }
        TEST(cloned >= 0);
        if (cloned >= 0) {
            Synth::GraphNodeLayout expected = original;
            expected.zone                   = 1;
            if (generator) {
                expected.index = candidate.graph_layout[cloned].index;
            }
            TEST(memcmp(&expected, &candidate.graph_layout[cloned], sizeof(expected)) == 0);
        }
    }

    // The clone's generator descriptors live in fresh pool slots: the ids
    // differ from the source zone's, the bytes match, and one zone's edits
    // cannot reach the other zone's sound.
    const Synth::Instrument& source_instrument =
        source.bank.instruments.entries[source.bank.channel_zones[0][0].instrument];
    const Synth::Instrument& cloned_instrument =
        candidate.bank.instruments.entries[candidate.bank.channel_zones[0][1].instrument];
    for (uint32_t layer = 0; layer < cloned_instrument.layer_count; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            const Synth::TargetBinding& source_binding = source_instrument.layers[layer].targets[target];
            const Synth::TargetBinding& cloned_binding = cloned_instrument.layers[layer].targets[target];
            if (source_binding.envelope_desc_id != 0) {
                TEST(cloned_binding.envelope_desc_id != source_binding.envelope_desc_id);
                TEST(memcmp(&source.bank.envelopes.entries[source_binding.envelope_desc_id - 1],
                            &candidate.bank.envelopes.entries[cloned_binding.envelope_desc_id - 1],
                            sizeof(Synth::EnvelopeDescriptor)) == 0);
            }
            if (source_binding.lfo_desc_id != 0) {
                TEST(cloned_binding.lfo_desc_id != source_binding.lfo_desc_id);
                TEST(memcmp(&source.bank.lfos.entries[source_binding.lfo_desc_id - 1],
                            &candidate.bank.lfos.entries[cloned_binding.lfo_desc_id - 1],
                            sizeof(Synth::LFODescriptor)) == 0);
            }
        }
    }

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(candidate, &graph, &mapping, 0, 1));
    const std::unique_ptr<Sculptor::Graph> original_graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       original_graph         = *original_graph_storage;
    Sculptor::OscGraphMapping              original_mapping       = {};
    TEST(Sculptor::project_editor_to_graph(source, &original_graph, &original_mapping, 0, 0));

    for (uint32_t n = 0; n < Sculptor::max_nodes; ++n) {
        if (! original_graph.node_occupied(n)) {
            continue;
        }
        const Sculptor::Node& original = original_graph.node(n);
        bool                  matched  = false;
        for (uint32_t c = 0; c < Sculptor::max_nodes; ++c) {
            if (! graph.node_occupied(c) || strcmp(original.name, graph.node(c).name) != 0) {
                continue;
            }
            const Sculptor::Node& clone = graph.node(c);
            TEST(clone.position.x == original.position.x && clone.position.y == original.position.y);
            TEST(clone.content_width_override == original.content_width_override);
            TEST(clone.content_height_override == original.content_height_override);
            matched = true;
        }
        TEST(matched);
    }

    TEST(Sculptor::validate_editor_metadata(candidate));

    // Replacing the instrument at its first note does not insert or move metadata.
    candidate = source;
    TEST(Synth::zone_split_new(candidate.bank.channel_zones[0], 0, 0, &candidate));
    TEST(candidate.graph_layout_count == records);
    TEST(memcmp(source.graph_layout, candidate.graph_layout, records * sizeof(source.graph_layout[0])) == 0);

    // Effect records belong to the channel, even when their zone byte is nonzero.
    candidate                     = source;
    Synth::GraphNodeLayout effect = {};
    effect.kind                   = 4;
    effect.zone                   = 2;
    strcpy(effect.name, "Delay");
    candidate.graph_layout[candidate.graph_layout_count++] = effect;
    TEST(Sculptor::zone_records_split_copy(&candidate, 0, 0));
    TEST(candidate.graph_layout_count == 2 * records + 1);
    TEST(memcmp(&candidate.graph_layout[records], &effect, sizeof(effect)) == 0);

    // The metadata helper must refuse before shifting records or missing-sum rows.
    candidate                                                         = source;
    candidate.graph_layout_count                                      = Synth::max_graph_records - 3;
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       before         = *before_storage;
    before                                                            = candidate;
    TEST(! Sculptor::zone_records_split_copy(&candidate, 0, 0));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);

    // The generator clone refuses when a pool lacks room for the copies.
    candidate                              = source;
    candidate.bank.envelopes.num_allocated = Synth::max_envelopes;
    TEST(Synth::zone_split_new(candidate.bank.channel_zones[0], 0, 60, &candidate));
    TEST(Sculptor::zone_records_split_copy(&candidate, 0, 0));
    TEST(! Sculptor::clone_split_generators(&candidate, 0, 0));
}

// Regression: two distinct envelope descriptors with identical content,
// one per layer, must survive the library JSON round trip unmerged.  The
// document groups bindings by encoded descriptor id; content equality
// alone cannot, and a merged descriptor would change later edits.
static void check_identical_envelopes_library_round_trips()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& destination = *destination_storage;

    memset(static_cast<void*>(&source), 0, sizeof(source));
    source.bank.channel_enabled[0] = 1;
    TEST(source.bank.instruments.allocate() == 0);
    source.bank.channel_zones[0][0].start_note = 1;
    source.bank.channel_zones[0][0].instrument = 0;

    const uint32_t first_env = source.bank.envelopes.allocate();
    TEST(first_env != pool_no_slot);
    Synth::EnvelopeDescriptor& env = source.bank.envelopes.entries[first_env];
    env.num_points                 = 2;
    env.sustain_first_point        = 1;
    env.sustain_last_point         = 1;
    env.min_value                  = -1.0f;
    env.min_max_delta              = 2.0f;
    env.points[0].position         = 0;
    env.points[0].value            = 0;
    env.points[1].position         = 100;
    env.points[1].value            = 0xFFFF;
    const uint32_t second_env      = source.bank.envelopes.allocate();
    TEST(second_env != pool_no_slot);
    source.bank.envelopes.entries[second_env] = env;

    Synth::Instrument instrument = {};
    instrument.layer_count       = 2;
    set_default_dispersion_sections(instrument);
    instrument.layers[0].osc_type[0]                                 = Synth::WaveType::sawtooth_wave;
    instrument.layers[1].osc_type[0]                                 = Synth::WaveType::sawtooth_wave;
    instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = 1;
    instrument.layers[1].targets[Synth::mod_volume].envelope_desc_id = 2;
    source.bank.instruments.entries[0]                               = instrument;
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    strcpy(source.instrument_names[0], "Twin Envelopes");
    strcpy(source.channel_names[0], "Source");
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::validate_editor_metadata(source));

    const char* const path = "synth_twin_envelopes_roundtrip.tmp";
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "Twin Envelopes", &source, 0) == 0);
    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(path, &entry, 1) == 1);
    uint16_t slot = 0xBEEF;
    TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &slot));
    TEST(destination.bank.envelopes.num_allocated == 2);
    const Synth::Instrument& loaded = destination.bank.instruments.entries[slot];
    // Each layer keeps its own envelope binding: the two descriptors may
    // hold identical content but must not merge into one shared slot.
    TEST(loaded.layers[0].targets[Synth::mod_volume].envelope_desc_id != 0);
    TEST(loaded.layers[1].targets[Synth::mod_volume].envelope_desc_id != 0);
    TEST(loaded.layers[0].targets[Synth::mod_volume].envelope_desc_id !=
         loaded.layers[1].targets[Synth::mod_volume].envelope_desc_id);
    const Synth::EnvelopeDescriptor& loaded_first =
        destination.bank.envelopes.entries[loaded.layers[0].targets[Synth::mod_volume].envelope_desc_id - 1];
    const Synth::EnvelopeDescriptor& loaded_second =
        destination.bank.envelopes.entries[loaded.layers[1].targets[Synth::mod_volume].envelope_desc_id - 1];
    TEST(memcmp(&loaded_first, &loaded_second, sizeof(loaded_first)) == 0);
    TEST(loaded_first.num_points == 2);
    TEST(loaded_first.sustain_first_point == 1 && loaded_first.sustain_last_point == 1);
    for (uint32_t point = 0; point < 2; ++point) {
        TEST(approx(loaded_first.min_value + loaded_first.points[point].value * loaded_first.min_max_delta,
                    env.min_value + env.points[point].value * env.min_max_delta,
                    0.01f));
    }
    remove(path);
}

// Regression: a channel whose envelope and LFO pool ids no longer coincide
// with the decoded document's positions (fragmented pool allocation, dummies
// allocated first) must still save to the library.  The save path's id maps are
// keyed by source pool id while the decoded layout records index descriptors by
// document position, so both the kind-1 (envelope) and kind-2 (LFO) remaps must
// translate through the document's ids instead of missing and refusing the save.
static void check_fragmented_pool_ids_library_round_trips()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& destination = *destination_storage;

    memset(static_cast<void*>(&source), 0, sizeof(source));
    source.bank.channel_enabled[0] = 1;

    // Push the real descriptors' pool ids high: the channel never references
    // the dummy slots, so the kept envelopes land at ids 4 and 5 and the LFO
    // at id 3 while every document position starts at 1.
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t slot = source.bank.envelopes.allocate();
        TEST(slot != pool_no_slot);
        source.bank.envelopes.entries[slot].num_points = 1;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t slot = source.bank.lfos.allocate();
        TEST(slot != pool_no_slot);
        source.bank.lfos.entries[slot].wave      = Synth::WaveType::sine_wave;
        source.bank.lfos.entries[slot].period_ms = 300;
    }

    Synth::EnvelopeDescriptor env = {};
    env.num_points                = 2;
    env.sustain_first_point       = 1;
    env.sustain_last_point        = 1;
    env.min_value                 = -1.0f;
    env.min_max_delta             = 2.0f;
    env.points[0].position        = 0;
    env.points[0].value           = 0;
    env.points[1].position        = 100;
    env.points[1].value           = 0x8000;

    const uint32_t first_env = source.bank.envelopes.allocate();
    TEST(first_env == 3); // id 4
    source.bank.envelopes.entries[first_env]          = env;
    const Synth::EnvelopeDescriptor first_env_content = source.bank.envelopes.entries[first_env];

    env.points[1].value = 0x4000;

    const uint32_t second_env = source.bank.envelopes.allocate();
    TEST(second_env == 4); // id 5
    source.bank.envelopes.entries[second_env]          = env;
    const Synth::EnvelopeDescriptor second_env_content = source.bank.envelopes.entries[second_env];

    const uint32_t lfo_slot = source.bank.lfos.allocate();
    TEST(lfo_slot == 2); // id 3
    source.bank.lfos.entries[lfo_slot].wave          = Synth::WaveType::sine_wave;
    source.bank.lfos.entries[lfo_slot].period_ms     = 300;
    source.bank.lfos.entries[lfo_slot].min_value     = -1.0f;
    source.bank.lfos.entries[lfo_slot].min_max_delta = 2.0f;

    // Two zones with two instruments: each zone's volume target carries its
    // own envelope plus the shared LFO, and one detached record per generator
    // rides along so the save must remap both kind-1 and kind-2 records.
    Synth::Instrument instrument = {};
    instrument.layer_count       = 1;
    set_default_dispersion_sections(instrument);
    instrument.layers[0].osc_type[0]                                 = Synth::WaveType::sawtooth_wave;
    instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = 4;
    instrument.layers[0].targets[Synth::mod_volume].lfo_desc_id      = 3;

    for (uint32_t zone = 0; zone < 2; ++zone) {

        const uint32_t slot = source.bank.instruments.allocate();
        TEST(slot != pool_no_slot);

        instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(4 + zone);
        source.bank.instruments.entries[slot]                            = instrument;
        source.bank.channel_zones[0][zone].start_note                    = static_cast<uint8_t>(zone ? 60 : 1);
        source.bank.channel_zones[0][zone].instrument                    = static_cast<uint8_t>(slot);
        Sculptor::set_identity_osc_sources(&source, 0, zone);
        strcpy(source.instrument_names[slot], zone ? "Fragmented B" : "Fragmented A");

        add_detached_record(&source,
                            0,
                            zone,
                            1,
                            4 + zone,
                            44.0f + 10.0f * static_cast<float>(zone),
                            55.0f,
                            Synth::ModSource::none,
                            Synth::ModSource::none,
                            1);
        add_detached_record(&source,
                            0,
                            zone,
                            2,
                            3,
                            66.0f + 10.0f * static_cast<float>(zone),
                            77.0f,
                            Synth::ModSource::none,
                            Synth::ModSource::none,
                            1);
    }

    strcpy(source.channel_names[0], "Fragmented");
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::validate_editor_metadata(source));

    const char* const path = "synth_fragmented_pool_roundtrip.tmp";
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "Fragmented", &source, 0) == 0);

    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(path, &entry, 1) == 1);
    uint16_t load_slot = 0xBEEF;
    TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &load_slot));

    // The dummies never travel: the record holds one envelope per zone and
    // the one shared LFO, installed densely in the destination.
    TEST(destination.bank.envelopes.num_allocated == 2);
    TEST(destination.bank.lfos.num_allocated == 1);
    TEST(destination.bank.instruments.num_allocated == 2);

    for (uint32_t zone = 0; zone < 2; ++zone) {

        const uint8_t               instr = destination.bank.channel_zones[2][zone].instrument;
        const Synth::TargetBinding& gen =
            destination.bank.instruments.entries[instr].layers[0].targets[Synth::mod_volume];

        TEST(gen.envelope_desc_id == zone + 1);
        TEST(gen.lfo_desc_id == 1);

        // The detached records survive with positions remapped to the dense
        // destination pool: one kind-1 record at the zone's envelope and one
        // kind-2 record at the shared LFO.
        TEST(count_records_matching(destination, 2, zone, 1, zone + 1) == 1);
        TEST(count_records_matching(destination, 2, zone, 2, 1) == 1);

        const Synth::EnvelopeDescriptor& loaded_env = destination.bank.envelopes.entries[gen.envelope_desc_id - 1];
        const Synth::EnvelopeDescriptor& src_env    = zone ? second_env_content : first_env_content;
        TEST(loaded_env.num_points == 2);
        TEST(loaded_env.sustain_first_point == 1 && loaded_env.sustain_last_point == 1);
        // The second point's level survives the document round trip, and the
        // two zones' envelopes stay distinct instead of merging.
        const float loaded_level = loaded_env.min_value + loaded_env.points[1].value * loaded_env.min_max_delta;
        TEST(approx(loaded_level, src_env.min_value + src_env.points[1].value * src_env.min_max_delta, 0.01f));
        const Synth::EnvelopeDescriptor& other_env = destination.bank.envelopes.entries[2 - gen.envelope_desc_id];
        const float other_level = other_env.min_value + other_env.points[1].value * other_env.min_max_delta;
        TEST(! approx(loaded_level, other_level, 0.01f));
        TEST(destination.bank.lfos.entries[gen.lfo_desc_id - 1].period_ms == 300);
    }

    remove(path);
}

static void check_zero_fm_clipboard_library_round_trips()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       destination = *destination_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> installed_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                                installed       = *installed_storage;
    const std::unique_ptr<ClipboardDecode>                      decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                                            decoded         = *decoded_storage;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    const char* const path           = "synth_zero_fm_roundtrip.tmp";

    for (uint32_t active = 0; active < 2; ++active) {
        for (uint32_t nonzero = 0; nonzero < 2; ++nonzero) {
            build_named_fm_fixture(&source);
            Sculptor::set_identity_osc_sources(&source, 0, 0);
            const float        depth   = nonzero ? 0.625f : 0.0f;
            const float        ratio   = nonzero ? 2.0f : 0.0f;
            Synth::Instrument& fixture = source.bank.instruments.entries[0];
            // Bank-side writes seed every layer (fixture uniformity, no bridge).
            for (uint32_t l = 0; l < fixture.layer_count; ++l) {
                fixture.layers[l].targets[Synth::mod_fm_index].base_value = depth * 6.2831853f;
            }
            for (uint32_t l = 0; l < source.bank.instruments.entries[0].layer_count; ++l) {
                Synth::Oscillator& osc = source.bank.instruments.entries[0].layers[l];
                osc.osc_type[1]        = active ? Synth::WaveType::sine_wave : Synth::WaveType::no_wave;
                osc.osc_mode           = active ? Synth::osc_mode_fm : Synth::osc_mode_blend;
                osc.mod_ratio          = ratio;
                osc.targets[Synth::mod_fm_ratio].base_value = ratio;
            }

            TEST(Synth::validate_instrument_bank(&source.bank));
            TEST(export_clipboard_fixture(source, text, sizeof(text)));
            TEST(decode_clipboard(&decoded, text, max_portable_records));
            check_routing_fan_out(decoded.instr);
            TEST(decoded.instr.layers[0].targets[Synth::mod_fm_index].base_value == depth * 6.2831853f);
            TEST(decoded.instr.layers[0].mod_ratio == ratio);

            destination = source;
            TEST(Sculptor::replace_zone_instrument_candidate(destination,
                                                             0,
                                                             0,
                                                             decoded.instr,
                                                             decoded.envs,
                                                             decoded.env_count,
                                                             decoded.lfos,
                                                             decoded.lfo_count,
                                                             decoded.layout,
                                                             decoded.layout_count,
                                                             nullptr,
                                                             &installed));

            for (uint32_t transport = 0; transport < 2; ++transport) {
                if (transport) {
                    remove(path);
                    TEST(Synth::save_library_record(path, "Regression", "Zero FM", &source, 0) == 0);
                    Synth::LibraryEntry entry;
                    TEST(Synth::read_library_index(path, &entry, 1) == 1);
                    uint16_t slot = 0xBEEF;
                    TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &slot));
                    installed = destination;
                }
                const uint32_t           channel = transport ? 2 : 0;
                const Synth::Instrument& instr =
                    installed.bank.instruments.entries[installed.bank.channel_zones[channel][0].instrument];
                TEST(instr.layers[0].targets[Synth::mod_fm_index].base_value == depth * 6.2831853f);
                const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
                Sculptor::Graph&                       graph         = *graph_storage;
                Sculptor::OscGraphMapping              mapping       = {};
                TEST(Sculptor::project_editor_to_graph(installed, &graph, &mapping, channel, 0));
                for (uint32_t l = 0; l < instr.layer_count; ++l) {
                    TEST(instr.layers[l].mod_ratio == ratio);
                    TEST(instr.layers[l].osc_type[1] ==
                         (active ? Synth::WaveType::sine_wave : Synth::WaveType::no_wave));
                    TEST(instr.layers[l].osc_mode == (active ? Synth::osc_mode_fm : Synth::osc_mode_blend));
                    const Sculptor::Node& node = graph.node(mapping.osc_nodes[l]);
                    TEST(node.slots.entries[Sculptor::osc_target_row(Synth::mod_fm_index)].value.real == depth);
                    TEST(node.slots.entries[96].value.real == ratio); // fm ratio, layout contract
                    TEST(fm_widget_enabled(graph, mapping.osc_nodes[l]) == (active != 0));
                }
            }
        }
    }

    // Legacy omission retains the decoder default; explicit zero is not omission.
    const char* const omitted = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}]}";
    TEST(decode_clipboard(&decoded, omitted, max_portable_records));
    // Zero-default, per layer: layer 0 keeps the document defaults, every later
    // layer decodes to the zeroed binding (no fan-out).
    TEST(decoded.instr.layers[0].targets[Synth::mod_volume].base_value == 1.0f);
    TEST(decoded.instr.layers[0].targets[Synth::mod_volume].num_inputs == 0);
    TEST(decoded.instr.layers[0].targets[Synth::mod_panning].base_value == 0.5f);
    for (uint32_t layer = 1; layer < Synth::max_layers; layer++) {
        TEST(decoded.instr.layers[layer].targets[Synth::mod_volume].base_value == 0.0f);
        TEST(decoded.instr.layers[layer].targets[Synth::mod_volume].num_inputs == 0);
        TEST(decoded.instr.layers[layer].targets[Synth::mod_panning].base_value == 0.0f);
    }
    TEST(decoded.instr.layers[0].mod_ratio == 1);

    const char* const explicit_zero =
        "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mod_ratio\":0,\"fm_index\":{\"base\":0}}]}";
    TEST(decode_clipboard(&decoded, explicit_zero, max_portable_records));
    TEST(decoded.instr.layers[0].mod_ratio == 0);
    TEST(decoded.instr.layers[0].targets[Synth::mod_fm_index].base_value == 0);
    remove(path);
}

static void check_library_metadata_transactions()
{
    Synth::GraphNodeLayout a = {};
    strcpy(a.name, "Delay");
    for (uint32_t kind = 0; kind <= 4; ++kind) {
        a.kind                   = static_cast<uint8_t>(kind);
        Synth::GraphNodeLayout b = a;
        TEST(Sculptor::graph_layout_record_identity_equal(a, b));
        strcpy(b.name, "Other");
        TEST(Sculptor::graph_layout_record_identity_equal(a, b) == (kind != 4));
        b = a;
        ++b.channel;
        TEST(! Sculptor::graph_layout_record_identity_equal(a, b));
        b = a;
        ++b.zone;
        TEST(! Sculptor::graph_layout_record_identity_equal(a, b));
        b = a;
        ++b.kind;
        TEST(! Sculptor::graph_layout_record_identity_equal(a, b));
        b = a;
        ++b.index;
        TEST(! Sculptor::graph_layout_record_identity_equal(a, b));
        b = a;
        ++b.uid;
        TEST(! Sculptor::graph_layout_record_identity_equal(a, b));
        b   = a;
        b.x = 123;
        TEST(Sculptor::graph_layout_record_identity_equal(a, b));
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> identity_bank_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& identity_bank = *identity_bank_storage;
    identity_bank.graph_layout_count           = 2;
    identity_bank.graph_layout[0]              = a;
    identity_bank.graph_layout[1]              = a;
    TEST(! Sculptor::validate_editor_metadata(identity_bank));
    strcpy(identity_bank.graph_layout[1].name, "Other");
    TEST(Sculptor::validate_editor_metadata(identity_bank));
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    build_named_fm_fixture(&source);
    source.bank.channel_enabled[0] = 0;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    uint32_t length                             = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(length > 0);
    const char* const path = "synth_metadata_transaction.tmp";
    write_library_fixture(path, payload, length);
    Synth::LibraryEntry control_entry;
    TEST(Synth::read_library_index(path, &control_entry, 1) == 1);
    const std::unique_ptr<Synth::InstrumentEditorBank> control_destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& control_destination = *control_destination_storage;
    uint16_t                     control_slot        = 0xBEEF;
    TEST(Synth::load_library_instrument(path, &control_entry, &control_destination, 2, &control_slot));
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> before_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&before)[Synth::library_payload_max + 128] = before_storage->elements;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> after_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&after)[Synth::library_payload_max + 128] = after_storage->elements;
    // A duplicate locator with a conflicting position must reach strict
    // library validation before the compatible bank reader discards it.
    char* const layouts = strstr(payload, "\"layouts\":[");
    TEST(layouts != nullptr);
    if (layouts) {
        char* const  insert      = layouts + strlen("\"layouts\":[");
        const char   duplicate[] = "{\"channel\":0,\"zone\":0,\"kind\":1,\"index\":1,\"uid\":1,\"x\":999,\"y\":888},";
        const size_t offset      = static_cast<size_t>(insert - payload);
        memmove(insert + sizeof(duplicate) - 1, insert, length - offset + 1);
        memcpy(insert, duplicate, sizeof(duplicate) - 1);
        length += sizeof(duplicate) - 1;
        const std::unique_ptr<Synth::InstrumentEditorBank> legacy_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& legacy = *legacy_storage;
        TEST(Synth::decode_editor_bank_json(payload, length, &legacy));
        TEST(legacy.graph_layout_count == source.graph_layout_count);
        write_library_fixture(path, payload, length);
        const uint32_t      file_size = read_library_fixture(path, before, sizeof(before));
        Synth::LibraryEntry entry;
        TEST(Synth::read_library_index(path, &entry, 1) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       destination = *destination_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& snapshot = *snapshot_storage;
        Synth::Instrument            occupied;
        build_clipboard_zone_fixture(&destination, &occupied);
        snapshot             = destination;
        uint16_t output_slot = 0xBEEF;
        TEST(! Synth::load_library_instrument(path, &entry, &destination, 2, &output_slot));
        TEST(memcmp(&destination, &snapshot, sizeof(destination)) == 0);
        TEST(output_slot == 0xBEEF);
        TEST(read_library_fixture(path, after, sizeof(after)) == file_size);
        TEST(memcmp(before, after, file_size) == 0);
    }
    // Unknown metadata is tolerated by ordinary bank reads, not library imports.
    const char* const unknown_keys[] = { "\"layouts\":[{", "\"editor\":{" };
    for (const char* key : unknown_keys) {
        length       = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        char* insert = strstr(payload, key);
        TEST(insert != nullptr);
        if (! insert) {
            continue;
        }
        insert += strlen(key);
        const char   unknown[] = "\"future\":1,";
        const size_t offset    = static_cast<size_t>(insert - payload);
        memmove(insert + sizeof(unknown) - 1, insert, length - offset + 1);
        memcpy(insert, unknown, sizeof(unknown) - 1);
        length += sizeof(unknown) - 1;
        const std::unique_ptr<Synth::InstrumentEditorBank> legacy_unknown_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& legacy_unknown = *legacy_unknown_storage;
        TEST(Synth::decode_editor_bank_json(payload, length, &legacy_unknown));
        write_library_fixture(path, payload, length);
        const uint32_t      file_size = read_library_fixture(path, before, sizeof(before));
        Synth::LibraryEntry entry;
        TEST(Synth::read_library_index(path, &entry, 1) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> unknown_destination_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       unknown_destination = *unknown_destination_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> unknown_snapshot_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& unknown_snapshot = *unknown_snapshot_storage;
        unknown_snapshot                              = unknown_destination;
        uint16_t output_slot                          = 0xBEEF;
        TEST(! Synth::load_library_instrument(path, &entry, &unknown_destination, 2, &output_slot));
        TEST(memcmp(&unknown_destination, &unknown_snapshot, sizeof(unknown_destination)) == 0);
        TEST(output_slot == 0xBEEF);
        TEST(read_library_fixture(path, after, sizeof(after)) == file_size);
        TEST(memcmp(before, after, file_size) == 0);
    }
    build_named_fm_fixture(&source);
    // Failed serialization must not overwrite an existing record or its source.
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "FM", &source, 0) == 0);
    const uint32_t file_size = read_library_fixture(path, before, sizeof(before));
    // The poison uses a kind-4 record whose name fills all 32 counted
    // bytes with no terminator: a still-genuine refusal.
    Synth::GraphNodeLayout& poison = source.graph_layout[1];
    poison.kind                    = 4;
    poison.channel                 = static_cast<uint8_t>(Synth::max_channels);
    poison.zone                    = 0;
    poison.index                   = 0;
    poison.uid                     = 0;
    memset(poison.name, 'x', sizeof(poison.name));
    const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& snapshot = *snapshot_storage;
    snapshot                              = source;
    TEST(! export_clipboard_fixture(source, payload, sizeof(payload)));
    TEST(Synth::save_library_record(path, "Regression", "FM", &source, 0) != 0);
    TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
    TEST(read_library_fixture(path, after, sizeof(after)) == file_size);
    TEST(memcmp(before, after, file_size) == 0);
    remove(path);
}

static void check_editor_round_trip_capacity_failures()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> source_before_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source_before = *source_before_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       destination = *destination_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_before_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       destination_before = *destination_before_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_before_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate_before = *candidate_before_storage;

    build_named_fm_fixture(&source);
    source_before = source;

    char tiny[16];
    TEST(! export_clipboard_fixture(source, tiny, sizeof(tiny)));
    TEST(memcmp(&source, &source_before, sizeof(source)) == 0);

    const std::unique_ptr<FixedArray<char, clipboard_capacity>> document_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&document)[clipboard_capacity] = document_storage->elements;
    TEST(export_clipboard_fixture(source, document, sizeof(document)));

    const std::unique_ptr<ClipboardDecode> decoded_storage  = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded          = *decoded_storage;
    const std::unique_ptr<ClipboardDecode> sentinel_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       sentinel         = *sentinel_storage;
    fill_clipboard_sentinels(&decoded);
    fill_clipboard_sentinels(&sentinel);
    TEST(! decode_clipboard(&decoded, document, 1));
    TEST(clipboard_sentinels_intact(&decoded, &sentinel));
    TEST(decode_clipboard(&decoded, document, max_portable_records));

    Synth::Instrument occupied;
    build_clipboard_zone_fixture(&destination, &occupied);

    // Copy-on-write preflights an additional instrument slot before reclaiming
    // the unreferenced last slot; the full pool must refuse without mutation.
    for (uint32_t slot = 1; slot < Synth::max_instruments; ++slot) {
        TEST(destination.bank.instruments.allocate() == slot);
        destination.bank.instruments.entries[slot] = occupied;
    }
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        destination.bank.channel_enabled[channel] = 1;
        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
            Synth::Zone& root = destination.bank.channel_zones[channel][zone];
            root.start_note   = static_cast<uint8_t>(1 + 7 * zone);
            root.instrument =
                static_cast<uint8_t>((channel * Synth::max_instr_per_channel + zone) % Synth::max_instruments);
        }
    }
    destination.bank.channel_zones[Synth::max_channels - 1][Synth::max_instr_per_channel - 1].instrument = 0;

    TEST(Synth::validate_instrument_bank(&destination.bank));
    destination_before = destination;
    memset(static_cast<void*>(&candidate), 0x5A, sizeof(candidate));
    candidate_before = candidate;

    TEST(! Sculptor::replace_zone_instrument_candidate(destination,
                                                       0,
                                                       0,
                                                       decoded.instr,
                                                       decoded.envs,
                                                       decoded.env_count,
                                                       decoded.lfos,
                                                       decoded.lfo_count,
                                                       decoded.layout,
                                                       decoded.layout_count,
                                                       nullptr,
                                                       &candidate));
    TEST(memcmp(&destination, &destination_before, sizeof(destination)) == 0);
    TEST(memcmp(&candidate, &candidate_before, sizeof(candidate)) == 0);

    const char* const path = "synth_roundtrip_capacity.tmp";
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "FM", &source, 0) == 0);
    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(path, &entry, 1) == 1);

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> before_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&before)[Synth::library_payload_max + 128] = before_storage->elements;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> after_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&after)[Synth::library_payload_max + 128] = after_storage->elements;
    const uint32_t bytes                            = read_library_fixture(path, before, sizeof(before));

    uint16_t output_slot = 0xBEEF;
    TEST(! Synth::load_library_instrument(path, &entry, &destination, 0, &output_slot));
    TEST(output_slot == 0xBEEF);
    TEST(memcmp(&destination, &destination_before, sizeof(destination)) == 0);
    TEST(memcmp(&source, &source_before, sizeof(source)) == 0);
    TEST(read_library_fixture(path, after, sizeof(after)) == bytes);
    TEST(memcmp(before, after, bytes) == 0);
    remove(path);
}

// The decode -> project -> compile path must never silently drop a decoded
// modulator: an FM-depth LFO in the bank projects a wire into the layer's
// FM depth row (connectable under AC11) and the binding survives
// store_compiled_instrument.  Before the row was connectable the wire was
// silently discarded and the compile overwrote the LFO with a base-only
// binding.
static void check_decoded_fm_lfo_binding_survives_compile()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Synth::TargetBinding& fm         = instrument.layers[0].targets[Synth::mod_fm_index];
    fm.lfo_desc_id                   = 1;
    fm.lfo_op                        = Synth::SourceOp::add;
    fm.lfo_depth                     = 0.5f;
    bank.bank.instruments.entries[0] = instrument;
    TEST(Synth::validate_instrument_bank(&bank.bank));
    TEST(Sculptor::validate_editor_metadata(bank));
    // Decode: the bank round trips through its own JSON form.
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t length                       = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
    // Project and recompile: the FM depth row is wired and the LFO binding
    // lands back in the compiled instrument.
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(decoded, &graph, &mapping, 0, 0));
    TEST(
        find_osc_graph_connection(graph, { mapping.osc_nodes[0], Sculptor::osc_target_lfo_dot(Synth::mod_fm_index) }) !=
        Sculptor::pool_no_slot);
    TEST(Sculptor::refresh_osc_graph_compilation(&decoded, graph, mapping, 0, 0));
    const Synth::TargetBinding& compiled = decoded.bank.instruments.entries[decoded.bank.channel_zones[0][0].instrument]
                                               .layers[0]
                                               .targets[Synth::mod_fm_index];
    TEST(compiled.lfo_desc_id == 1);
    TEST(compiled.lfo_op == Synth::SourceOp::add);
    TEST(compiled.lfo_depth == 0.5f);
}

// C3/C4 rename fixtures commit through the event ring the way the editor
// drain does: rename_node fills the node title and pushes name_changed,
// and apply_osc_graph_change is the drain's apply step.  The candidate
// commit and its structural re-projection stay outside these checks; the
// re-projections here stand in for the drain's own rebuild.
static void check_renamed_oscillator_persists_through_save_load_and_reprojection()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    const uint32_t osc0 = mapping.osc_nodes[0];
    // Backward-compat baseline: the seeded kind-0 record has no name and
    // the projection shows the default title.
    const int32_t seeded = Sculptor::find_record(bank, 0, 0, 0, Synth::graph_canonical_first_osc, 0);
    TEST(seeded >= 0);
    if (seeded >= 0) {
        TEST(bank.graph_layout[seeded].name[0] == 0);
    }
    TEST(strcmp(graph.node(osc0).name, "Oscillator 1") == 0);

    graph.rename_node(osc0, "Lead Saw");
    Sculptor::GraphChange changed[Sculptor::max_pending_changes] = {};
    TEST(graph.take_changes(changed, Sculptor::max_pending_changes) == 1);
    TEST(changed[0].kind == Sculptor::ChangeKind::name_changed);
    TEST(changed[0].node_idx == osc0);
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changed[0]));

    // The committed rename upserts the zone's kind-0 record.
    const int32_t saved = Sculptor::find_record(bank, 0, 0, 0, Synth::graph_canonical_first_osc, 0);
    TEST(saved >= 0);
    if (saved >= 0) {
        TEST(strcmp(bank.graph_layout[saved].name, "Lead Saw") == 0);
    }

    // Re-projection restores the stored title (the re-projection half).
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(strcmp(graph.node(mapping.osc_nodes[0]).name, "Lead Saw") == 0);
    // The un-renamed oscillator keeps its default title.
    TEST(strcmp(graph.node(mapping.osc_nodes[1]).name, "Oscillator 2") == 0);

    // Save + load round trip (the document half).
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t length                       = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
    TEST(Sculptor::validate_editor_metadata(decoded));
    TEST(Sculptor::project_editor_to_graph(decoded, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(strcmp(graph.node(mapping.osc_nodes[0]).name, "Lead Saw") == 0);
    TEST(strcmp(graph.node(mapping.osc_nodes[1]).name, "Oscillator 2") == 0);
}

// C3/C4 boundary: a name filling all 32 counted bytes of the record field
// stores with no terminator; encode, decode, validation and re-projection
// must handle the full field without reading past byte 32.
static void check_exactly_32_byte_oscillator_name_round_trips()
{
    const char* const full_name = "abcdefghijklmnopqrstuvwxyz012345"; // exactly 32 counted bytes
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    const uint32_t osc0 = mapping.osc_nodes[0];
    graph.rename_node(osc0, full_name);
    Sculptor::GraphChange changed[Sculptor::max_pending_changes] = {};
    TEST(graph.take_changes(changed, Sculptor::max_pending_changes) == 1);
    TEST(changed[0].kind == Sculptor::ChangeKind::name_changed);
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changed[0]));

    // The record field holds all 32 counted bytes; no terminator exists,
    // so only a bounded compare is legal here.
    const int32_t saved = Sculptor::find_record(bank, 0, 0, 0, Synth::graph_canonical_first_osc, 0);
    TEST(saved >= 0);
    if (saved >= 0) {
        TEST(memcmp(bank.graph_layout[saved].name, full_name, 32) == 0);
    }

    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(strcmp(graph.node(mapping.osc_nodes[0]).name, full_name) == 0);

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t length                       = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
    TEST(Sculptor::validate_editor_metadata(decoded));
    TEST(Sculptor::project_editor_to_graph(decoded, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(strcmp(graph.node(mapping.osc_nodes[0]).name, full_name) == 0);
}

// Committing an empty name is a no-op: the apply step skips the
// kind-0 record write, so the structural re-projection restores the
// default title and no name is stored.
static void check_empty_rename_is_a_no_op()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);
    const uint32_t                         records_before = bank.graph_layout_count;
    const std::unique_ptr<Sculptor::Graph> graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph          = *graph_storage;
    Sculptor::OscGraphMapping              mapping        = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    const uint32_t osc0 = mapping.osc_nodes[0];
    graph.rename_node(osc0, "");
    Sculptor::GraphChange changed[Sculptor::max_pending_changes] = {};
    TEST(graph.take_changes(changed, Sculptor::max_pending_changes) == 1);
    TEST(changed[0].kind == Sculptor::ChangeKind::name_changed);
    // The no-op commit still applies cleanly: it writes nothing.
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changed[0]));
    TEST(bank.graph_layout_count == records_before);
    const int32_t saved = Sculptor::find_record(bank, 0, 0, 0, Synth::graph_canonical_first_osc, 0);
    TEST(saved >= 0);
    if (saved >= 0) {
        TEST(bank.graph_layout[saved].name[0] == 0);
    }

    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(strcmp(graph.node(mapping.osc_nodes[0]).name, "Oscillator 1") == 0);
}

// The derived-title helper joins deduplicated parameter
// row labels with ", " and appends " (<host>)" when every label belongs to
// one host node.  False (no labels) means the caller keeps the default
// "Envelope N" / "LFO N" title.
static void check_derive_generator_title_builds_titles()
{
    char title[64] = {};

    const char* const volume[] = { "Volume" };
    TEST(Sculptor::derive_generator_title(volume, 1, "Oscillator 1", title, sizeof(title)));
    TEST(strcmp(title, "Volume (Oscillator 1)") == 0);

    const char* const pan_pitch[] = { "Pan", "Pitch" };
    TEST(Sculptor::derive_generator_title(pan_pitch, 2, "Oscillator 1", title, sizeof(title)));
    TEST(strcmp(title, "Pan, Pitch (Oscillator 1)") == 0);

    // Connections spanning several host nodes: no parenthetical.
    TEST(Sculptor::derive_generator_title(pan_pitch, 2, nullptr, title, sizeof(title)));
    TEST(strcmp(title, "Pan, Pitch") == 0);

    // Duplicate labels dedup by string compare, preserving first-seen order.
    const char* const pan_pan_pitch[] = { "Pan", "Pan", "Pitch" };
    TEST(Sculptor::derive_generator_title(pan_pan_pitch, 3, nullptr, title, sizeof(title)));
    TEST(strcmp(title, "Pan, Pitch") == 0);

    const char* const volume_volume[] = { "Volume", "Volume" };
    TEST(Sculptor::derive_generator_title(volume_volume, 2, "Oscillator 2", title, sizeof(title)));
    TEST(strcmp(title, "Volume (Oscillator 2)") == 0);

    // No labels: false means the caller keeps the default title, and the
    // output buffer stays untouched (a null label list must not be read).
    memset(static_cast<void*>(title), 'z', sizeof(title));
    TEST(! Sculptor::derive_generator_title(nullptr, 0, "Oscillator 1", title, sizeof(title)));
    TEST(title[0] == 'z');
    TEST(title[sizeof(title) - 1] == 'z');

    // An empty host title is no host: the result carries no parenthetical.
    TEST(Sculptor::derive_generator_title(volume, 1, "", title, sizeof(title)));
    TEST(strcmp(title, "Volume") == 0);

    // A null label entry is refused before any write: the caller keeps the
    // default title and the buffer stays untouched.
    const char* const null_label[] = { nullptr };
    memset(static_cast<void*>(title), 'z', sizeof(title));
    TEST(! Sculptor::derive_generator_title(null_label, 1, nullptr, title, sizeof(title)));
    TEST(title[0] == 'z');
    TEST(title[sizeof(title) - 1] == 'z');

    // A null entry after a valid label is refused too: refusal still
    // happens before any write, so the whole buffer keeps its bytes.
    const char* const valid_then_null[] = { "Pan", nullptr };
    memset(static_cast<void*>(title), 'z', sizeof(title));
    char before_null_entry[sizeof(title)] = {};
    memcpy(before_null_entry, title, sizeof(before_null_entry));
    TEST(! Sculptor::derive_generator_title(valid_then_null, 2, nullptr, title, sizeof(title)));
    TEST(memcmp(title, before_null_entry, sizeof(title)) == 0);

    // Capacity truncation: the title truncates into the buffer and stays
    // NUL-terminated; at 8 bytes the joined labels keep a 7-character
    // prefix and the host parenthetical is dropped entirely.
    char tiny[8] = {};
    TEST(Sculptor::derive_generator_title(pan_pitch, 2, "Oscillator 1", tiny, sizeof(tiny)));
    TEST(strcmp(tiny, "Pan, Pi") == 0);
    TEST(tiny[sizeof(tiny) - 1] == 0);
}

// C4 decode-side kind gate: a kind-4 (effect node) name must terminate
// within its 32-byte field.  Bounded encode tolerates the unterminated
// bytes by design, but decode refuses before the name can reach any
// strcmp identity comparison.
static void check_terminated_kind4_name_still_refused_to_decode()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    // Append one kind-4 effect-node record whose name fills all 32
    // counted bytes with no terminator.
    TEST(bank.graph_layout_count < Synth::max_graph_records);
    Synth::GraphNodeLayout& record = bank.graph_layout[bank.graph_layout_count++];
    record                         = {};
    record.channel                 = static_cast<uint8_t>(Synth::max_channels);
    record.zone                    = 0;
    record.kind                    = 4;
    record.x                       = 10.0f;
    record.y                       = 20.0f;
    memset(record.name, 'x', sizeof(record.name));

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t length                       = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(! Synth::decode_editor_bank_json(payload, length, &decoded));
}

// Only kind-0 (oscillator title) and kind-4 (name-keyed fx node) records
// carry names.  Stale non-empty name bytes on kinds 1/2/5 are ignored: a bank
// carrying a named record of every such kind still decodes.
static void check_validator_ignores_stale_kind125_names()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    add_detached_record(&bank, 0, 0, 1, 2, 12, 13, Synth::ModSource::none, Synth::ModSource::none, 2);
    strcpy(bank.graph_layout[bank.graph_layout_count - 1].name, "Stale envelope");

    const uint16_t lfo = add_detached_lfo_descriptor(&bank);
    add_detached_record(&bank, 0, 0, 2, lfo, 14, 15, Synth::ModSource::none, Synth::ModSource::none, 3);
    strcpy(bank.graph_layout[bank.graph_layout_count - 1].name, "Stale LFO");

    add_detached_record(&bank, 0, 0, 5, 0, 16, 17, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(bank.graph_layout[bank.graph_layout_count - 1].name, "Stale parameters");

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const uint32_t length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));

    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
}

// A2b: "LFO 0" parses as a well-formed effect-LFO title whose zero descriptor
// names no LFO; the record drops as debris instead of refusing the document,
// and the retained records around it keep their order.
static void check_validator_accepts_lfo0_title()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    const uint32_t base = bank.graph_layout_count;
    add_detached_record(&bank, 0, 0, 1, 2, 30, 40, Synth::ModSource::none, Synth::ModSource::none, 2);

    add_detached_record(&bank, 0, 0, 4, 0, 60, 70, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(bank.graph_layout[bank.graph_layout_count - 1].name, "LFO 0");

    add_detached_record(&bank, 0, 0, 1, 2, 80, 90, Synth::ModSource::none, Synth::ModSource::none, 3);

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const uint32_t length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));

    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));

    TEST(decoded.graph_layout_count == base + 2);
    TEST(decoded.graph_layout[base].kind == 1 && decoded.graph_layout[base].uid == 2);
    TEST(decoded.graph_layout[base + 1].kind == 1 && decoded.graph_layout[base + 1].uid == 3);
    for (uint32_t index = 0; index < decoded.graph_layout_count; ++index) {
        TEST(strcmp(decoded.graph_layout[index].name, "LFO 0") != 0);
    }
}

// A5/A9: the depth/rate wiring-source fields are reserved for kinds 1/2;
// kind-4 and kind-5 records never read them, so non-zero residue is ignored
// instead of refusing the document.
static void check_validator_ignores_kind45_reserved_fields()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    add_detached_record(&bank, 0, 0, 4, 0, 60, 70, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(bank.graph_layout[bank.graph_layout_count - 1].name, "Delay");
    bank.graph_layout[bank.graph_layout_count - 1].depth_source = 1;
    bank.graph_layout[bank.graph_layout_count - 1].rate_source  = 2;

    add_detached_record(&bank, 0, 0, 5, 0, 80, 90, Synth::ModSource::none, Synth::ModSource::none, 0);
    bank.graph_layout[bank.graph_layout_count - 1].depth_source = 1;
    bank.graph_layout[bank.graph_layout_count - 1].rate_source  = 2;

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const uint32_t length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));

    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
}

// B2: only kind-2 LFO records read depth_source/rate_source, so the
// decode-stage ModSource range bound applies to kind 2 alone.  The
// counterpart below pins that rule.
static void check_validator_skips_depth_rate_for_unused_kinds()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    // Kind 0 keys a bound node and never reads the source bytes: an
    // out-of-range residue on the third oscillator node decodes.
    add_detached_record(&bank, 0, 0, 0, 3, 20, 21, Synth::ModSource::none, Synth::ModSource::none, 0);
    bank.graph_layout[bank.graph_layout_count - 1].depth_source = 200;
    bank.graph_layout[bank.graph_layout_count - 1].rate_source  = 200;

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const uint32_t length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));

    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));

    // Counterpart: a kind-2 record reads its sources, so out-of-range bytes
    // keep refusing the document.
    const std::unique_ptr<Synth::InstrumentEditorBank> lfo_bank_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& lfo_bank = *lfo_bank_storage;
    build_named_fm_fixture(&lfo_bank);

    const uint16_t lfo = add_detached_lfo_descriptor(&lfo_bank);
    add_detached_record(&lfo_bank, 0, 0, 2, lfo, 22, 23, Synth::ModSource::none, Synth::ModSource::none, 1);
    lfo_bank.graph_layout[lfo_bank.graph_layout_count - 1].depth_source = 200;
    lfo_bank.graph_layout[lfo_bank.graph_layout_count - 1].rate_source  = 200;

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> lfo_payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&lfo_payload)[Synth::library_payload_max] = lfo_payload_storage->elements;

    const uint32_t lfo_length = Synth::encode_editor_bank_json(&lfo_bank, lfo_payload, sizeof(lfo_payload));
    TEST(lfo_length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> lfo_decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& lfo_decoded = *lfo_decoded_storage;
    memset(static_cast<void*>(&lfo_decoded), 0, sizeof(lfo_decoded));

    TEST(! Synth::decode_editor_bank_json(lfo_payload, lfo_length, &lfo_decoded));
}

// A17: a doubled oscillator (the same layer named by two sum-input slots)
// is a compilable configuration.  The bank decodes; the deterministic compile
// keeps a copy of the referenced layer in every wired slot, in slot order;
// the clipboard round trip and the graph compile keep the doubled row.
static void check_validator_accepts_duplicate_osc_in_row()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    build_named_fm_fixture(&bank);

    // The fixture's identity row is {1, 2, 3}; slot 1 doubles slot 0's layer.
    bank.graph_osc_source[0][0][1] = 1;

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const uint32_t length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);

    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));

    const bool decoded_ok = Synth::decode_editor_bank_json(payload, length, &decoded);
    TEST(decoded_ok);
    if (! decoded_ok) {
        return;
    }

    Synth::Instrument playback = {};
    TEST(Sculptor::compile_playback_instrument(decoded.bank.instruments.entries[0],
                                               decoded.graph_osc_source[0][0],
                                               &playback));
    TEST(playback.layer_count == 3);
    TEST(memcmp(&playback.layers[0], &decoded.bank.instruments.entries[0].layers[0], sizeof(Synth::Oscillator)) == 0);
    TEST(memcmp(&playback.layers[1], &decoded.bank.instruments.entries[0].layers[0], sizeof(Synth::Oscillator)) == 0);
    TEST(memcmp(&playback.layers[2], &decoded.bank.instruments.entries[0].layers[2], sizeof(Synth::Oscillator)) == 0);

    // Clipboard round trip: the encode writes the doubled row unchanged and
    // the decode accepts it, so the row survives the round trip.
    const std::unique_ptr<FixedArray<char, 128 * 1024>> clipboard_storage =
        std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&clipboard)[128 * 1024] = clipboard_storage->elements;

    const uint32_t clipboard_len = Sculptor::encode_editor_instrument_json(decoded,
                                                                           0,
                                                                           0,
                                                                           clipboard,
                                                                           sizeof(clipboard),
                                                                           Synth::InstrumentJsonDomain::clipboard);
    TEST(clipboard_len > 0);

    const std::unique_ptr<ClipboardDecode> round_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       round         = *round_storage;
    fill_clipboard_sentinels(&round);
    uint8_t clipboard_row[Synth::max_layers] = {};
    TEST(Synth::decode_instrument_json_for_domain(clipboard,
                                                  clipboard_len,
                                                  &round.instr,
                                                  round.envs,
                                                  &round.env_count,
                                                  round.lfos,
                                                  &round.lfo_count,
                                                  round.layout,
                                                  max_portable_records,
                                                  &round.layout_count,
                                                  Synth::InstrumentJsonDomain::clipboard,
                                                  clipboard_row));

    const uint8_t expected_row[Synth::max_layers] = { 1, 1, 3 };
    TEST(memcmp(clipboard_row, expected_row, sizeof(expected_row)) == 0);

    // Projection and graph compilation keep accepting the doubled row: the
    // derived row matches the stored one.
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(decoded, &graph, &mapping));

    Synth::Instrument compiled                       = {};
    uint8_t           derived_row[Synth::max_layers] = {};
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled, derived_row));
    TEST(memcmp(derived_row, expected_row, sizeof(expected_row)) == 0);
}

// C1/C2: portable layout records never carry a name or a negative size
// override because the encoders never write one.  A direct-API record name
// is accepted but not copied: the mapping and normalization outputs keep the
// name bytes zero.  A negative size override is accepted and copied through,
// and rendering selects the default sizes when an override is not positive.
static void check_validator_accepts_portable_name_and_override()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_clipboard_zone_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0); // portable domains carry the wired (playback) form

    // C2 rides a real document: the record's width and height serialize as
    // "5", and the splices flip each sign to -5 behind the decoder's back.
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc,
                        10,
                        11,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    bank.graph_layout[bank.graph_layout_count - 1].width_override  = 5;
    bank.graph_layout[bank.graph_layout_count - 1].height_override = 5;

    const std::unique_ptr<FixedArray<char, 128 * 1024>> text_storage = std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&text)[128 * 1024]                                         = text_storage->elements;

    const uint32_t len = Sculptor::encode_editor_instrument_json(bank,
                                                                 0,
                                                                 0,
                                                                 text,
                                                                 sizeof(text),
                                                                 Synth::InstrumentJsonDomain::validated_bank);
    TEST(len > 0);

    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;
    fill_clipboard_sentinels(&decoded);
    TEST(Synth::decode_instrument_json_for_domain(text,
                                                  len,
                                                  &decoded.instr,
                                                  decoded.envs,
                                                  &decoded.env_count,
                                                  decoded.lfos,
                                                  &decoded.lfo_count,
                                                  decoded.layout,
                                                  max_portable_records,
                                                  &decoded.layout_count,
                                                  Synth::InstrumentJsonDomain::validated_bank,
                                                  nullptr));
    TEST(decoded.layout_count == 1);

    const std::unique_ptr<FixedArray<char, 128 * 1024>> spliced_storage =
        std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&spliced)[128 * 1024] = spliced_storage->elements;

    const uint32_t spliced_len =
        splice_single_occurrence(text, len, "\"width\":", "\"width\":-", spliced, sizeof(spliced));
    TEST(spliced_len > 0);
    const uint32_t both_len =
        splice_single_occurrence(spliced, spliced_len, "\"height\":", "\"height\":-", text, sizeof(text));
    TEST(both_len > 0);

    fill_clipboard_sentinels(&decoded);
    TEST(Synth::decode_instrument_json_for_domain(text,
                                                  both_len,
                                                  &decoded.instr,
                                                  decoded.envs,
                                                  &decoded.env_count,
                                                  decoded.lfos,
                                                  &decoded.lfo_count,
                                                  decoded.layout,
                                                  max_portable_records,
                                                  &decoded.layout_count,
                                                  Synth::InstrumentJsonDomain::validated_bank,
                                                  nullptr));

    // The negative override bytes survive decode unchanged; rendering picks
    // the default sizes when an override is not positive.
    TEST(decoded.layout_count == 1);
    TEST(decoded.layout[0].width_override == -5.0f && decoded.layout[0].height_override == -5.0f);

    // C1 through the record-structure rule directly: the portable document
    // grammar refuses a name key before record_structure_ok runs, so the
    // stale-name record is exercised in memory against the mapping and
    // normalization walks.
    const std::unique_ptr<Synth::InstrumentEditorBank> named_bank_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& named_bank       = *named_bank_storage;
    Synth::Instrument            named_instrument = {};
    build_clipboard_zone_fixture(&named_bank, &named_instrument);

    Synth::InstrumentGraphLayout named_record = {};
    named_record.kind                         = 0;
    named_record.canonical_index              = Synth::graph_canonical_first_osc;
    named_record.x                            = 1;
    named_record.y                            = 2;
    strcpy(named_record.name, "Stale");

    Synth::GraphNodeLayout mapped[1]    = {};
    uint32_t               mapped_count = 0;

    const bool mapped_ok = Sculptor::map_instrument_graph_layout(named_instrument,
                                                                 named_bank.bank.envelopes.entries,
                                                                 named_bank.bank.envelopes.num_allocated,
                                                                 named_bank.bank.lfos.entries,
                                                                 named_bank.bank.lfos.num_allocated,
                                                                 &named_record,
                                                                 1,
                                                                 mapped,
                                                                 1,
                                                                 &mapped_count);
    TEST(mapped_ok);
    TEST(! mapped_ok || mapped_count == 1);
    TEST(mapped[0].name[0] == 0);

    // Normalization ignores the stale name the same way: the output record's
    // name bytes stay zero.
    Synth::InstrumentGraphLayout old_normalized[1] = {};
    uint32_t                     normalized_count  = 0;

    const bool normalized_ok = Sculptor::normalize_instrument_graph_layout(named_instrument,
                                                                           named_bank.bank.envelopes.entries,
                                                                           named_bank.bank.envelopes.num_allocated,
                                                                           named_bank.bank.lfos.entries,
                                                                           named_bank.bank.lfos.num_allocated,
                                                                           &named_record,
                                                                           1,
                                                                           named_instrument,
                                                                           named_bank.bank.envelopes.entries,
                                                                           named_bank.bank.envelopes.num_allocated,
                                                                           named_bank.bank.lfos.entries,
                                                                           named_bank.bank.lfos.num_allocated,
                                                                           old_normalized,
                                                                           1,
                                                                           &normalized_count);
    TEST(normalized_ok);
    TEST(! normalized_ok || normalized_count == 1);
    TEST(old_normalized[0].name[0] == 0);
}

static void check_unrepresented_layout_refusal()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                                snapshot = *snapshot_storage;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> document_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&document)[clipboard_capacity]                                           = document_storage->elements;
    const char* const                                                         path = "synth_unrepresented_layout.tmp";
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> before_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&before)[Synth::library_payload_max + 128] = before_storage->elements;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 128>> after_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 128>>();
    char (&after)[Synth::library_payload_max + 128] = after_storage->elements;

    for (uint32_t shape = 0; shape < 2; ++shape) {
        build_named_fm_fixture(&source);
        remove(path);
        TEST(Synth::save_library_record(path, "Regression", "FM", &source, 0) == 0);
        const uint32_t bytes = read_library_fixture(path, before, sizeof(before));

        if (shape == 0) {
            // A detached LFO record wired to nothing projects no node.
            // Its descriptor is validly allocated, so the portable export
            // drops the record (logged) instead of refusing: the graph UI
            // cannot repair an invisible node, and the totality check
            // exempts exactly this debris.
            const uint16_t lfo = add_detached_lfo_descriptor(&source);
            add_detached_record(&source, 0, 0, 2, lfo, 991, 992, Synth::ModSource::none, Synth::ModSource::none, 1);
        }
        else if (shape == 1) {
            // An unallocated descriptor id is outside the drop exemption:
            // the metadata validation refuses the record outright, and the
            // export fails with the library file untouched.
            add_detached_record(&source, 0, 0, 2, 250, 991, 992, Synth::ModSource::none, Synth::ModSource::none, 1);
        }

        TEST(Synth::validate_instrument_bank(&source.bank));
        snapshot = source;

        if (shape == 0) {
            TEST(Sculptor::validate_editor_metadata(source));
            TEST(export_clipboard_fixture(source, document, sizeof(document)));
            // The exported portable layout carries no record for the
            // dropped detached node.
            const std::unique_ptr<ClipboardDecode> drop_decode_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       drop_decode         = *drop_decode_storage;
            TEST(decode_clipboard(&drop_decode, document, max_portable_records));
            bool dropped_record_gone = true;
            for (uint32_t i = 0; i < drop_decode.layout_count; i++) {
                dropped_record_gone =
                    dropped_record_gone && ! (drop_decode.layout[i].x == 991.0f && drop_decode.layout[i].y == 992.0f);
            }
            TEST(dropped_record_gone);
        }
        else {
            TEST(! export_clipboard_fixture(source, document, sizeof(document)));
            TEST(Synth::save_library_record(path, "Regression", "FM", &source, 0) != 0);
            TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
            TEST(read_library_fixture(path, after, sizeof(after)) == bytes);
            TEST(memcmp(before, after, bytes) == 0);
        }
        remove(path);
    }
}

static void check_legacy_library_numeric_domains()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                                destination = *destination_storage;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    const char* const path           = "synth_legacy_numeric.tmp";
    for (uint32_t values = 1; values <= 3; ++values) {
        for (uint32_t metadata = 0; metadata < 2; ++metadata) {
            build_named_fm_fixture(&source);
            if (! metadata) {
                source.graph_layout_count = 0;
            }
            Synth::Oscillator& osc = source.bank.instruments.entries[0].layers[0];
            osc.octave_offset      = values & 1 ? 13 : 0;
            osc.mod_ratio          = values & 2 ? 9.0f : 2;
            TEST(Synth::validate_instrument_bank(&source.bank));
            const uint32_t len =
                Synth::encode_instrument_json(text, sizeof(text), &source.bank.instruments.entries[0], &source.bank);
            TEST(len > 0);
            const std::unique_ptr<ClipboardDecode> attempted_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       attempted         = *attempted_storage;
            const std::unique_ptr<ClipboardDecode> sentinel_storage  = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       sentinel          = *sentinel_storage;
            fill_clipboard_sentinels(&attempted);
            fill_clipboard_sentinels(&sentinel);
            TEST(! decode_clipboard(&attempted, text, max_portable_records));
            TEST(clipboard_sentinels_intact(&attempted, &sentinel));
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> legacy_payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&legacy_payload)[Synth::library_payload_max] = legacy_payload_storage->elements;
            source.bank.channel_enabled[0]                     = 0;
            const uint32_t legacy_length =
                Synth::encode_editor_bank_json(&source, legacy_payload, sizeof(legacy_payload));
            TEST(legacy_length > 0);
            write_library_fixture(path, legacy_payload, legacy_length);
            Synth::LibraryEntry legacy_entry;
            TEST(Synth::read_library_index(path, &legacy_entry, 1) == 1);
            memset(static_cast<void*>(&destination), 0, sizeof(destination));
            uint16_t legacy_slot = 0xBEEF;
            // Out-of-range octave offsets reject the saved-bank load.
            TEST(Synth::load_library_instrument(path, &legacy_entry, &destination, 2, &legacy_slot) == ! (values & 1));
            TEST(legacy_slot == (values & 1 ? 0xBEEF : 0));
            if (! (values & 1)) {
                TEST(destination.bank.instruments.entries[0].layers[0].octave_offset == osc.octave_offset);
                TEST(destination.bank.instruments.entries[0].layers[0].mod_ratio == osc.mod_ratio);
                if (metadata) {
                    TEST(destination.graph_layout_count == source.graph_layout_count);
                }
            }
            source.bank.channel_enabled[0] = 1;
            remove(path);
            // The library save path verifies its own encode-decode round
            // trip against the shared decode validation.
            TEST((Synth::save_library_record(path, "Legacy", "Numeric", &source, 0) == 0) == ! (values & 1));
            if (! (values & 1)) {
                Synth::LibraryEntry entry;
                TEST(Synth::read_library_index(path, &entry, 1) == 1);
                memset(static_cast<void*>(&destination), 0, sizeof(destination));
                uint16_t output_slot = 0xBEEF;
                TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &output_slot));
                TEST(output_slot == 0);
                TEST(destination.bank.instruments.entries[0].layers[0].octave_offset == osc.octave_offset);
                TEST(destination.bank.instruments.entries[0].layers[0].mod_ratio == osc.mod_ratio);
                if (metadata) {
                    TEST(destination.graph_layout_count == source.graph_layout_count);
                }
            }
            remove(path);
        }
    }
}

// Envelope depth serialization: the document dialect carries the depth
// inside the envelope object, the decoded default is 1 (the legacy
// unscaled contribution), explicit values including 0 survive, and the
// saved-bank dialect round-trips a scaled binding.
static void envelope_depth_serialization_regressions()
{
    Synth::Instrument                                           instr                                 = {};
    Synth::EnvelopeDescriptor                                   envs[Synth::instrument_max_envelopes] = {};
    Synth::LFODescriptor                                        lfos[Synth::instrument_max_lfos]      = {};
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    uint32_t env_count               = 999;
    uint32_t lfo_count               = 999;

    const char* const no_depth_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                     "\"panning\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},"
                                     "{\"time_seconds\":0.5,\"level\":1}]}}}]}";
    TEST(Synth::decode_instrument_json(no_depth_doc,
                                       static_cast<uint32_t>(strlen(no_depth_doc)),
                                       &instr,
                                       envs,
                                       &env_count,
                                       lfos,
                                       &lfo_count));
    TEST(env_count == 1);
    TEST(instr.layers[0].targets[Synth::mod_panning].envelope_desc_id == 1);
    TEST(instr.layers[0].targets[Synth::mod_panning].envelope_depth == 1.0f);

    // The scaled shape re-encodes with the key and decodes back.  The encoder
    // resolves descriptor ids against a bank, so stage the decoded envelope in.
    const std::unique_ptr<Synth::InstrumentBank> rt_bank_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       rt_bank         = *rt_bank_storage;
    memset(static_cast<void*>(&rt_bank), 0, sizeof(rt_bank));
    rt_bank.envelopes.entries[0]    = envs[0];
    rt_bank.envelopes.num_allocated = 1;
    TEST(Synth::encode_instrument_json(text, sizeof(text), &instr, &rt_bank) > 0);
    TEST(strstr(text, "\"depth\"") == nullptr); // the 1 default omits the key

    instr.layers[0].targets[Synth::mod_panning].envelope_depth = 0.5f;
    TEST(Synth::encode_instrument_json(text, sizeof(text), &instr, &rt_bank) > 0);
    TEST(strstr(text, "\"depth\":0.5") != nullptr);

    const char* const scaled_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                   "\"panning\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},"
                                   "{\"time_seconds\":0.5,\"level\":1}],\"depth\":0.5}}}]}";
    TEST(Synth::decode_instrument_json(scaled_doc,
                                       static_cast<uint32_t>(strlen(scaled_doc)),
                                       &instr,
                                       envs,
                                       &env_count,
                                       lfos,
                                       &lfo_count));
    TEST(instr.layers[0].targets[Synth::mod_panning].envelope_depth == 0.5f);

    const char* const zero_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                 "\"panning\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0},"
                                 "{\"time_seconds\":0.5,\"level\":1}],\"depth\":0}}}]}";
    TEST(Synth::decode_instrument_json(zero_doc,
                                       static_cast<uint32_t>(strlen(zero_doc)),
                                       &instr,
                                       envs,
                                       &env_count,
                                       lfos,
                                       &lfo_count));
    TEST(instr.layers[0].targets[Synth::mod_panning].envelope_depth == 0.0f);

    // Saved-bank dialect: a scaled binding round-trips through the
    // library payload, the default omits the key.
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    build_named_fm_fixture(&source);
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_depth = 0.25f;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t length                       = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(length > 0);
    TEST(strstr(payload, "\"envelope_depth\":0.25") != nullptr);
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& bank_decoded = *bank_decoded_storage;
    memset(static_cast<void*>(&bank_decoded), 0, sizeof(bank_decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &bank_decoded));
    TEST(bank_decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_depth == 0.25f);
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            Synth::TargetBinding& b = source.bank.instruments.entries[0].layers[layer].targets[t];
            if (b.envelope_desc_id != 0) {
                b.envelope_depth = 1.0f;
            }
        }
    }
    const uint32_t default_length = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(default_length > 0);
    TEST(strstr(payload, "envelope_depth") == nullptr); // every wired binding at 1: the default omits the key
}

// Pad wave codec: "pad" decodes as an oscillator wave in both positions and
// preserves through a full encode-decode round trip, while the legacy LFO
// acceptance set stays closed and every legacy oscillator wave name keeps its
// frozen enum value.
static void check_pad_json_codec()
{
    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;

    // "pad" decodes in the oscillator A position, with the duty_a base.
    {
        static const char pad_a_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\",\"duty_a\":{\"base\":0.5}}]}";
        fill_clipboard_sentinels(&decoded);
        TEST(decode_clipboard(&decoded, pad_a_doc, max_portable_records));
        TEST(decoded.instr.layers[0].osc_type[0] == static_cast<Synth::WaveType>(5));
        TEST(decoded.instr.layers[0].targets[Synth::mod_duty0].base_value == 0.5f);
    }

    // "pad" decodes in the oscillator B position too.
    {
        static const char pad_b_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"wave_b\":\"pad\"}]}";
        fill_clipboard_sentinels(&decoded);
        TEST(decode_clipboard(&decoded, pad_b_doc, max_portable_records));
        TEST(decoded.instr.layers[0].osc_type[1] == static_cast<Synth::WaveType>(5));
    }

    // Full round trip: decode, encode with the shared encoder, decode again;
    // the pad waves in both positions and the duty base must survive both hops.
    {
        static const char pad_rt_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\",\"wave_b\":\"pad\",\"duty_a\":{\"base\":0.5}}]}";
        Synth::Instrument         rt_instr                                 = {};
        Synth::EnvelopeDescriptor rt_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      rt_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  rt_env_count                             = 0;
        uint32_t                  rt_lfo_count                             = 0;
        TEST(Synth::decode_instrument_json(pad_rt_doc,
                                           static_cast<uint32_t>(strlen(pad_rt_doc)),
                                           &rt_instr,
                                           rt_envs,
                                           &rt_env_count,
                                           rt_lfos,
                                           &rt_lfo_count));
        const std::unique_ptr<Synth::InstrumentEditorBank> rt_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& rt_bank = *rt_bank_storage;
        memset(static_cast<void*>(&rt_bank), 0, sizeof(rt_bank));
        const uint32_t rt_slot = rt_bank.bank.instruments.allocate();
        TEST(rt_slot != pool_no_slot);
        rt_bank.bank.instruments.entries[rt_slot] = rt_instr;
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> rt_text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&rt_text)[clipboard_capacity] = rt_text_storage->elements;
        const uint32_t rt_len = Synth::encode_instrument_json(rt_text, sizeof(rt_text), &rt_instr, &rt_bank.bank);
        TEST(rt_len > 0);
        Synth::Instrument         rt_back                                       = {};
        Synth::EnvelopeDescriptor rt_back_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      rt_back_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  rt_back_env_count                             = 0;
        uint32_t                  rt_back_lfo_count                             = 0;
        TEST(Synth::decode_instrument_json(rt_text,
                                           rt_len,
                                           &rt_back,
                                           rt_back_envs,
                                           &rt_back_env_count,
                                           rt_back_lfos,
                                           &rt_back_lfo_count));
        TEST(rt_back.layers[0].osc_type[0] == static_cast<Synth::WaveType>(5));
        TEST(rt_back.layers[0].osc_type[1] == static_cast<Synth::WaveType>(5));
        TEST(rt_back.layers[0].targets[Synth::mod_duty0].base_value == 0.5f);
    }

    // The legacy LFO acceptance set stays closed: the inline layer LFO decode
    // whitelists sine and sawtooth explicitly, so "pad" must keep failing
    // decode.
    {
        static const char pad_lfo_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                          "\"volume\":{\"lfo\":{\"wave\":\"pad\",\"frequency_hz\":4}}}]}";
        const std::unique_ptr<ClipboardDecode> guarded_storage        = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       guarded                = *guarded_storage;
        const std::unique_ptr<ClipboardDecode> guard_sentinel_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       guard_sentinel         = *guard_sentinel_storage;
        fill_clipboard_sentinels(&guarded);
        fill_clipboard_sentinels(&guard_sentinel);
        TEST(! decode_clipboard(&guarded, pad_lfo_doc, max_portable_records));
        TEST(clipboard_sentinels_intact(&guarded, &guard_sentinel));
    }

    // The bank-format decode path shares the same wave table: a standalone
    // "lfos" array must refuse "pad" the same way the inline layer LFO does.
    {
        static const char pad_bank_lfo_doc[] =
            "{\"instrument_editor_bank\":{\"lfos\":[{\"wave\":\"pad\",\"period_ms\":100}]}}";
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_guarded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank_guarded = *bank_guarded_storage;
        memset(static_cast<void*>(&bank_guarded), 0, sizeof(bank_guarded));
        TEST(! Synth::decode_editor_bank_json(pad_bank_lfo_doc,
                                              static_cast<uint32_t>(strlen(pad_bank_lfo_doc)),
                                              &bank_guarded));
    }

    // The five legacy oscillator wave names keep their frozen enum values 0..4.
    {
        static const char* const legacy_names[5]  = { "none", "sine", "sawtooth", "pulse", "noise" };
        const Synth::WaveType    legacy_values[5] = { Synth::WaveType::no_wave,
                                                      Synth::WaveType::sine_wave,
                                                      Synth::WaveType::sawtooth_wave,
                                                      Synth::WaveType::pulse_wave,
                                                      Synth::WaveType::noise_wave };
        char                     legacy_doc[128]  = {};
        for (uint32_t w = 0; w < 5; ++w) {
            const int written = snprintf(legacy_doc,
                                         sizeof(legacy_doc),
                                         "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"%s\"}]}",
                                         legacy_names[w]);
            TEST(written > 0 && static_cast<uint32_t>(written) < sizeof(legacy_doc));
            fill_clipboard_sentinels(&decoded);
            TEST(decode_clipboard(&decoded, legacy_doc, max_portable_records));
            TEST(decoded.instr.layers[0].osc_type[0] == legacy_values[w]);
        }
    }
}

// Pad tilt mod targets: the two tilt target names decode as target blocks,
// a base survives the full encode-decode round trip, every legacy target
// name keeps its frozen enum value, and an unknown target name still fails
// decode.
static void check_pad_tilt_json_codec()
{
    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;

    // Both tilt target names decode in one layer.  The same layer without
    // the tilt keys decodes today, so a tilt-key refusal can only come
    // from the unknown target names, not from the layer shape.
    {
        char control_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\"}]}";
        fill_clipboard_sentinels(&decoded);
        TEST(decode_clipboard(&decoded, control_doc, max_portable_records));
        static const char tilt_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\","
                                       "\"pad_tone_a\":{\"base\":0.5},\"pad_tone_b\":{\"base\":-1}}]}";
        fill_clipboard_sentinels(&decoded);
        TEST(decode_clipboard(&decoded, tilt_doc, max_portable_records));
        const Sculptor::OscTargetView tilt_view0 = Sculptor::osc_target_view(Synth::mod_pad_tilt0);
        const Sculptor::OscTargetView tilt_view1 = Sculptor::osc_target_view(Synth::mod_pad_tilt1);
        TEST(tilt_view0.min_value == -1.0f && tilt_view0.max_value == 1.0f && tilt_view0.bank_scale == 1.0f);
        TEST(tilt_view1.min_value == -1.0f && tilt_view1.max_value == 1.0f && tilt_view1.bank_scale == 1.0f);
        TEST(approx(decoded.instr.layers[0].targets[Synth::mod_pad_tilt0].base_value,
                    0.5f * tilt_view0.bank_scale,
                    1.0e-5f));
        TEST(approx(decoded.instr.layers[0].targets[Synth::mod_pad_tilt1].base_value,
                    -1.0f * tilt_view1.bank_scale,
                    1.0e-5f));
    }

    // Full round trip: decode, encode with the shared encoder, decode again;
    // both hops must succeed.
    {
        static const char         tilt_rt_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\","
                                                  "\"pad_tone_a\":{\"base\":0.5},\"pad_tone_b\":{\"base\":-1}}]}";
        Synth::Instrument         rt_instr      = {};
        Synth::EnvelopeDescriptor rt_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      rt_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  rt_env_count                             = 0;
        uint32_t                  rt_lfo_count                             = 0;
        TEST(Synth::decode_instrument_json(tilt_rt_doc,
                                           static_cast<uint32_t>(strlen(tilt_rt_doc)),
                                           &rt_instr,
                                           rt_envs,
                                           &rt_env_count,
                                           rt_lfos,
                                           &rt_lfo_count));
        const std::unique_ptr<Synth::InstrumentEditorBank> rt_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& rt_bank = *rt_bank_storage;
        memset(static_cast<void*>(&rt_bank), 0, sizeof(rt_bank));
        const uint32_t rt_slot = rt_bank.bank.instruments.allocate();
        TEST(rt_slot != pool_no_slot);
        rt_bank.bank.instruments.entries[rt_slot] = rt_instr;
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> rt_text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&rt_text)[clipboard_capacity] = rt_text_storage->elements;
        const uint32_t rt_len = Synth::encode_instrument_json(rt_text, sizeof(rt_text), &rt_instr, &rt_bank.bank);
        TEST(rt_len > 0);
        Synth::Instrument         rt_back                                       = {};
        Synth::EnvelopeDescriptor rt_back_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      rt_back_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  rt_back_env_count                             = 0;
        uint32_t                  rt_back_lfo_count                             = 0;
        TEST(Synth::decode_instrument_json(rt_text,
                                           rt_len,
                                           &rt_back,
                                           rt_back_envs,
                                           &rt_back_env_count,
                                           rt_back_lfos,
                                           &rt_back_lfo_count));
        TEST(approx(rt_back.layers[0].targets[Synth::mod_pad_tilt0].base_value, 0.5f, 1.0e-5f));
        TEST(approx(rt_back.layers[0].targets[Synth::mod_pad_tilt1].base_value, -1.0f, 1.0e-5f));
    }

    // The eight legacy target names keep their frozen enum values 0..7; the
    // array size is fixed so appending targets later cannot shift these.
    {
        static const char* const legacy_names[8] = { "volume", "pitch",   "panning",  "duty_a",
                                                     "duty_b", "osc_mix", "fm_index", "fir_pitch_spread" };
        char                     legacy_doc[192] = {};
        for (uint32_t t = 0; t < 8; ++t) {
            const int written = snprintf(legacy_doc,
                                         sizeof(legacy_doc),
                                         "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                         "\"%s\":{\"base\":0.25}}]}",
                                         legacy_names[t]);
            TEST(written > 0 && static_cast<uint32_t>(written) < sizeof(legacy_doc));
            fill_clipboard_sentinels(&decoded);
            TEST(decode_clipboard(&decoded, legacy_doc, max_portable_records));
            const Sculptor::OscTargetView view = Sculptor::osc_target_view(static_cast<Synth::ModTarget>(t));
            TEST(approx(decoded.instr.layers[0].targets[static_cast<Synth::ModTarget>(t)].base_value,
                        0.25f * view.bank_scale,
                        1.0e-5f));
        }
    }

    // An unknown target name still fails decode: only the two spelled tilt
    // names may join the table.
    {
        static const char unknown_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\","
                                          "\"pad_tone\":{\"base\":0}}]}";
        const std::unique_ptr<ClipboardDecode> guarded_storage        = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       guarded                = *guarded_storage;
        const std::unique_ptr<ClipboardDecode> guard_sentinel_storage = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       guard_sentinel         = *guard_sentinel_storage;
        fill_clipboard_sentinels(&guarded);
        fill_clipboard_sentinels(&guard_sentinel);
        TEST(! decode_clipboard(&guarded, unknown_doc, max_portable_records));
        TEST(clipboard_sentinels_intact(&guarded, &guard_sentinel));
    }
}

static void check_bank_inline_0()
{

    // ---- Zone table helpers: lookup, boundary moves, splits ----
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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
        Synth::Zone tables[2][Synth::max_instr_per_channel] = {};
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
        for (uint32_t slot = 0; slot < Synth::max_instr_per_channel; slot++) {
            TEST(tables[1][slot].start_note == static_cast<uint8_t>(slot + 1) && tables[1][slot].instrument == 1);
        }

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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_full_table_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank_full_table = *bank_full_table_storage;
        Synth::init_default_bank(&bank_full_table.bank);
        Sculptor::set_identity_osc_sources(&bank_full_table, 0, 0);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_full_pool_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank_full_pool = *bank_full_pool_storage;
        Synth::init_default_bank(&bank_full_pool.bank);
        Sculptor::set_identity_osc_sources(&bank_full_pool, 0, 0);
        while (bank_full_pool.bank.instruments.allocate() != pool_no_slot) {
            ;
        }
        TEST(! Synth::zone_split_new(bank_full_pool.bank.channel_zones[0], 0, 60, &bank_full_pool));
        TEST(bank_full_pool.bank.channel_zones[0][0].start_note == 1);
        TEST(bank_full_pool.bank.channel_zones[0][0].instrument == 0);

        // Repeated split/reclaim cycles keep the instrument pool bounded: each split
        // at the zone's first note clones the instrument and leaves the old one
        // unreferenced, so reclaim returns it to the pool.
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_cycles_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank_cycles = *bank_cycles_storage;
        Synth::init_default_bank(&bank_cycles.bank);
        Sculptor::set_identity_osc_sources(&bank_cycles, 0, 0);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        // Instrument 1 references env 2 and lfo 2 while unrelated descriptors exist,
        // so the record's dense renumbering is visible.
        TEST(bank.bank.envelopes.allocate() == 1);
        bank.bank.envelopes.entries[1].num_points         = 2;
        bank.bank.envelopes.entries[1].points[1].position = 100;
        TEST(bank.bank.lfos.allocate() == 1);
        bank.bank.lfos.entries[1].wave      = Synth::WaveType::sawtooth_wave;
        bank.bank.lfos.entries[1].period_ms = 80;
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1].layer_count = 1;
        set_default_dispersion_sections(bank.bank.instruments.entries[1]);
        bank.bank.instruments.entries[1].layers[0].fir_slope_percent                           = 100;
        bank.bank.instruments.entries[1].layers[0].targets[Synth::mod_volume].envelope_desc_id = 2;
        bank.bank.instruments.entries[1].layers[0].targets[Synth::mod_pitch].lfo_desc_id       = 2;
        memcpy(bank.instrument_names[1], "Saved", 6);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const char* const path = "synth_library_roundtrip.tmp";

        // Roundtrip: the record keeps the channel's whole instrument - every zone
        // of the channel and every instrument the zones reference - renumbered to
        // a dense prefix with the zoning on channel 0.
        bank.bank.channel_zones[1][0] = { 1, 0 };
        bank.bank.channel_zones[1][1] = { 61, 1 };
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        Sculptor::set_identity_osc_sources(&bank, 1, 0);
        Sculptor::set_identity_osc_sources(&bank, 1, 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));
        TEST(Synth::save_library_record(path, "Pads", "Saved", &bank, 1) == 0);

        Synth::LibraryEntry entries[Synth::library_max_records];
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 1);
        TEST(strcmp(entries[0].category, "Pads") == 0);
        TEST(strcmp(entries[0].name, "Saved") == 0);
        TEST(entries[0].payload_size > 0 && entries[0].payload_size <= Synth::library_payload_max);

        const std::unique_ptr<Synth::InstrumentEditorBank> record_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& record = *record_storage;
        memset(static_cast<void*>(&record), 0, sizeof(record));
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
        TEST(record.bank.instruments.entries[1].layers[0].targets[Synth::mod_volume].envelope_desc_id == 2);
        TEST(record.bank.instruments.entries[1].layers[0].targets[Synth::mod_pitch].lfo_desc_id == 2);
        TEST(record.bank.channel_chains[0].num_effects == 0);
        TEST(record.bank.master_chain.num_effects == 0);
        TEST(strcmp(record.instrument_names[1], "Saved") == 0);

        // Save As renames only the record; the payload keeps the source names.
        TEST(Synth::save_library_record(path, "Pads", "Renamed", &bank, 1) == 0);
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 2);
        TEST(strcmp(entries[1].name, "Renamed") == 0);
        memset(static_cast<void*>(&record), 0, sizeof(record));
        TEST(Synth::load_library_instrument(path, &entries[1], &record, 0, &load_slot));
        TEST(strcmp(record.instrument_names[1], "Saved") == 0);
        TEST(strcmp(bank.instrument_names[1], "Saved") == 0);

        remove(path);
    }

    // A whole-channel record carries the channel's effect chain; chain-referenced
    // LFOs survive even when no instrument uses them.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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
        Sculptor::set_identity_osc_sources(&bank, 1, 0);
        bank.bank.channel_chains[1].effects[1].type = Synth::EffectType::distortion;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        const char* const path = "synth_library_chain.tmp";
        TEST(Synth::save_library_record(path, "Fx", "Chained", &bank, 1) == 0);
        Synth::LibraryEntry entries[Synth::library_max_records];
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> record_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& record = *record_storage;
        memset(static_cast<void*>(&record), 0, sizeof(record));
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
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src = *src_storage;
        memset(static_cast<void*>(&src), 0, sizeof(src));
        src.bank.instruments.allocate();
        src.bank.instruments.entries[0].layer_count = 1;
        set_default_dispersion_sections(src.bank.instruments.entries[0]);
        memcpy(src.instrument_names[0], "Chained", 8);
        src.bank.instruments.entries[0].layers[0].fir_slope_percent          = 100;
        src.bank.instruments.entries[0].layers[0].string_dispersion_sections = 4.0f;
        src.bank.channel_zones[0][0].start_note                              = 1;
        src.bank.channel_zones[0][0].instrument                              = 0;
        Sculptor::set_identity_osc_sources(&src, 0, 0);
        src.bank.channel_enabled[0] = 1;
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

        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src = *src_storage;
        make_valid_bank(src.bank);
        Sculptor::set_identity_osc_sources(&src, 0, 0);
        src.bank.channel_enabled[0]                                                      = 0;
        src.bank.channel_enabled[1]                                                      = 0;
        src.bank.channel_chains[0].num_effects                                           = 1;
        src.bank.channel_chains[0].effects[0].type                                       = Synth::EffectType::delay;
        src.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id                    = 1;
        constexpr uint32_t                                           bank_json_text_size = 1024 * 1024;
        const std::unique_ptr<FixedArray<char, bank_json_text_size>> text_storage =
            std::make_unique<FixedArray<char, bank_json_text_size>>();
        char (&text)[bank_json_text_size] = text_storage->elements;
        const char* const path            = "synth_library_shape.tmp";
        auto              attempt_load    = [&](bool expect_load) {
            const uint32_t payload_len = Synth::encode_editor_bank_json(&src, text, bank_json_text_size);
            TEST(payload_len > 0 && payload_len <= Synth::library_payload_max);
            write_library_fixture(path, text, payload_len, "Cat", "Bad");
            Synth::LibraryEntry entries[1];
            TEST(Synth::read_library_index(path, entries, 1) == 1);
            const std::unique_ptr<Synth::InstrumentEditorBank> scratch_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& scratch = *scratch_storage;
            memset(static_cast<void*>(&scratch), 0, sizeof(scratch));
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
        set_default_dispersion_sections(src.bank.instruments.entries[1]);
        attempt_load(false);
        remove(path);
    }

    // Load appends the record's instrument into a populated bank with remapped ids;
    // repeated load/delete cycles keep the pool bounded through reclaim.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src = *src_storage;
        memset(static_cast<void*>(&src), 0, sizeof(src));
        src.bank.instruments.allocate();
        src.bank.instruments.entries[0].layer_count = 1;
        set_default_dispersion_sections(src.bank.instruments.entries[0]);
        memcpy(src.instrument_names[0], "Loaded", 7);
        src.bank.instruments.entries[0].layers[0].fir_slope_percent          = 100;
        src.bank.instruments.entries[0].layers[0].string_dispersion_sections = 4.0f;
        src.bank.channel_zones[0][0].start_note                              = 1;
        src.bank.channel_zones[0][0].instrument                              = 0;
        Sculptor::set_identity_osc_sources(&src, 0, 0);
        src.bank.channel_enabled[0] = 1;
        TEST(Synth::validate_instrument_bank(&src.bank));
        const char* const load_path = "synth_library_load.tmp";
        TEST(Synth::save_library_record(load_path, "Cat", "Inst", &src, 0) == 0);
        Synth::LibraryEntry load_entries[1];
        TEST(Synth::read_library_index(load_path, load_entries, 1) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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
            memset(static_cast<void*>(bank.bank.channel_zones[2]), 0, sizeof(bank.bank.channel_zones[2]));
            memset(static_cast<void*>(bank.graph_osc_source[2]), 0, sizeof(bank.graph_osc_source[2]));
            bank.bank.channel_enabled[2] = 0;
            Synth::reclaim_unused_slots(&bank);
            TEST(Synth::validate_instrument_bank(&bank.bank));
            TEST(bank.bank.instruments.num_allocated == pool_start);
        }
        remove(load_path);
    }

    // A pool without space refuses the load with the bank unmodified.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src = *src_storage;
        memset(static_cast<void*>(&src), 0, sizeof(src));
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
        src.bank.instruments.entries[0].layer_count = 1;
        set_default_dispersion_sections(src.bank.instruments.entries[0]);
        src.bank.instruments.entries[0].layers[0].fir_slope_percent                           = 100;
        src.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_desc_id = 1;
        src.bank.channel_zones[0][0].start_note                                               = 1;
        src.bank.channel_zones[0][0].instrument                                               = 0;
        Sculptor::set_identity_osc_sources(&src, 0, 0);
        src.bank.channel_enabled[0] = 1;
        TEST(Synth::validate_instrument_bank(&src.bank));
        const char* const full_path = "synth_library_full.tmp";
        TEST(Synth::save_library_record(full_path, "Cat", "Inst", &src, 0) == 0);
        Synth::LibraryEntry full_entries[1];
        TEST(Synth::read_library_index(full_path, full_entries, 1) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        while (bank.bank.envelopes.allocate() != pool_no_slot) {
            ;
        }
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
        const std::unique_ptr<Synth::InstrumentEditorBank> fat_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& fat = *fat_storage;
        Synth::init_default_bank(&fat.bank); // a valid bank, but not a valid record shape

        const char* const path = "synth_library_ladder.tmp";
        // The codec's document bound, duplicated because the codec keeps it internal;
        // if the codec's real bound grows past this, the too-large test catches it.
        constexpr uint32_t bank_json_text_size = 1024 * 1024;

        const std::unique_ptr<FixedArray<char, bank_json_text_size>> text_storage =
            std::make_unique<FixedArray<char, bank_json_text_size>>();
        char (&text)[bank_json_text_size] = text_storage->elements;

        const uint32_t payload_len = Synth::encode_editor_bank_json(&fat, text, bank_json_text_size);
        TEST(payload_len > 0 && payload_len <= Synth::library_payload_max);

        for (uint32_t attempt = 0; attempt < 2; attempt++) {
            // Re-encode every attempt: the corrupting patch below flips a byte of
            // the record text, and the next attempt needs pristine text again
            TEST(Synth::encode_editor_bank_json(&fat, text, bank_json_text_size) == payload_len);

            write_library_fixture(path, text, payload_len, "Cat", "Wide");

            Synth::LibraryEntry entries[4];
            TEST(Synth::read_library_index(path, entries, 4) == 1);

            const std::unique_ptr<Synth::InstrumentEditorBank> scratch_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& scratch = *scratch_storage;
            memset(static_cast<void*>(&scratch), 0, sizeof(scratch));

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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

        const char* const junk_path = "synth_library_corrupt_save.tmp";
        FILE* const       junk      = fopen(junk_path, "wb");
        fwrite("not a library at all", 20, 1, junk);
        fclose(junk);
        TEST(Synth::save_library_record(junk_path, "C", "N", &bank, 0) != 0);
        FILE* const  check      = fopen(junk_path, "rb");
        char         buf[20]    = {};
        const size_t read_count = fread(buf, 1, 20, check);
        (void)read_count;
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

        const std::unique_ptr<FixedArray<uint8_t, 64 * 1024>> junk_storage =
            std::make_unique<FixedArray<uint8_t, 64 * 1024>>();
        uint8_t (&junk)[64 * 1024] = junk_storage->elements;
        memset(static_cast<void*>(junk), 0x5a, sizeof(junk));
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

        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

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
            memset(static_cast<void*>(bad_record), 'x', sizeof(bad_record) - 4);
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
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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

        const std::unique_ptr<Synth::InstrumentEditorBank> scratch_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& scratch = *scratch_storage;
        memset(static_cast<void*>(&scratch), 0, sizeof(scratch));
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
        memset(static_cast<void*>(bad_category), 'x', Synth::library_category_len);
        memcpy(bad_category + 48, &small, 4);
        fwrite(bad_category, sizeof(bad_category), 1, file);
        fwrite("1234", 4, 1, file);

        char bad_name[52] = {};
        memcpy(bad_name, "Cat", 3);
        memset(static_cast<void*>(bad_name + 24), 'y', Synth::library_name_len);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

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

    // Deleting one record keeps every survivor's payload faithful, including
    // records in the empty category.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src  = *src_storage;
        const char* const            path = "synth_library_delete_record.tmp";
        auto                         save_fixture =
            [&](const char* category, const char* name, uint16_t point, Synth::WaveType wave, uint16_t period) {
                make_valid_bank(src.bank);
                Sculptor::set_identity_osc_sources(&src, 0, 0);
                src.bank.envelopes.entries[0].points[1].position = point;
                src.bank.lfos.entries[0].wave                    = wave;
                src.bank.lfos.entries[0].period_ms               = period;
                snprintf(src.instrument_names[0], sizeof(src.instrument_names[0]), "%s", name);
                TEST(Synth::validate_instrument_bank(&src.bank));
                TEST(Synth::save_library_record(path, category, name, &src, 0) == 0);
            };

        save_fixture("Drums", "Kick", 25, Synth::WaveType::sine_wave, 30);
        save_fixture("Drums", "Snare", 50, Synth::WaveType::sawtooth_wave, 60);
        save_fixture("", "Free", 75, Synth::WaveType::sine_wave, 90);

        Synth::LibraryScanStatus delete_status = Synth::library_valid;
        TEST(Synth::delete_library_record(path, "Drums", "Snare", &delete_status) == 0);
        TEST(delete_status == Synth::library_valid);

        Synth::LibraryEntry entries[Synth::library_max_records];
        delete_status = Synth::library_invalid;
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records, &delete_status) == 2);
        TEST(delete_status == Synth::library_valid);
        TEST(strcmp(entries[0].category, "Drums") == 0 && strcmp(entries[0].name, "Kick") == 0);
        TEST(strcmp(entries[1].category, "") == 0 && strcmp(entries[1].name, "Free") == 0);

        const std::unique_ptr<Synth::InstrumentEditorBank> record_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& record    = *record_storage;
        uint16_t                     load_slot = 0;
        memset(static_cast<void*>(&record), 0, sizeof(record));
        TEST(Synth::load_library_instrument(path, &entries[0], &record, 0, &load_slot));
        TEST(load_slot == 0);
        TEST(Synth::validate_instrument_bank(&record.bank));
        TEST(record.bank.instruments.num_allocated == 1);
        TEST(record.bank.envelopes.num_allocated == 1);
        TEST(record.bank.lfos.num_allocated == 1);
        TEST(record.bank.channel_zones[0][0].start_note == 1);
        TEST(record.bank.channel_zones[0][0].instrument == 0);
        TEST(record.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
        TEST(record.bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].lfo_desc_id == 1);
        TEST(record.bank.envelopes.entries[0].points[1].position == 25);
        TEST(record.bank.lfos.entries[0].wave == Synth::WaveType::sine_wave);
        TEST(record.bank.lfos.entries[0].period_ms == 30);
        TEST(strcmp(record.instrument_names[0], "Kick") == 0);
        memset(static_cast<void*>(&record), 0, sizeof(record));
        TEST(Synth::load_library_instrument(path, &entries[1], &record, 0, &load_slot));
        TEST(Synth::validate_instrument_bank(&record.bank));
        TEST(record.bank.envelopes.entries[0].points[1].position == 75);
        TEST(record.bank.lfos.entries[0].wave == Synth::WaveType::sine_wave);
        TEST(record.bank.lfos.entries[0].period_ms == 90);
        TEST(strcmp(record.instrument_names[0], "Free") == 0);

        remove(path);
    }

    // Deleting a category removes exactly its records; the other categories and
    // the empty category survive intact.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src          = *src_storage;
        const char* const            path         = "synth_library_delete_category.tmp";
        auto                         save_fixture = [&](const char* category, const char* name, uint16_t point) {
            make_valid_bank(src.bank);
            Sculptor::set_identity_osc_sources(&src, 0, 0);
            src.bank.envelopes.entries[0].points[1].position = point;
            snprintf(src.instrument_names[0], sizeof(src.instrument_names[0]), "%s", name);
            TEST(Synth::validate_instrument_bank(&src.bank));
            TEST(Synth::save_library_record(path, category, name, &src, 0) == 0);
        };

        save_fixture("Drums", "Kick", 20);
        save_fixture("Drums", "Snare", 40);
        save_fixture("Bass", "Sub", 60);
        save_fixture("", "Free", 80);

        Synth::LibraryScanStatus delete_status = Synth::library_valid;
        TEST(Synth::delete_library_category(path, "Drums", &delete_status) == 0);
        TEST(delete_status == Synth::library_valid);

        Synth::LibraryEntry entries[Synth::library_max_records];
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 2);
        TEST(strcmp(entries[0].category, "Bass") == 0 && strcmp(entries[0].name, "Sub") == 0);
        TEST(strcmp(entries[1].category, "") == 0 && strcmp(entries[1].name, "Free") == 0);

        // The empty category is legal data: deleting it removes only its records.
        TEST(Synth::delete_library_category(path, "", &delete_status) == 0);
        TEST(delete_status == Synth::library_valid);
        TEST(Synth::read_library_index(path, entries, Synth::library_max_records) == 1);
        TEST(strcmp(entries[0].category, "Bass") == 0 && strcmp(entries[0].name, "Sub") == 0);

        const std::unique_ptr<Synth::InstrumentEditorBank> record_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& record    = *record_storage;
        uint16_t                     load_slot = 0;
        memset(static_cast<void*>(&record), 0, sizeof(record));
        TEST(Synth::load_library_instrument(path, &entries[0], &record, 0, &load_slot));
        TEST(Synth::validate_instrument_bank(&record.bank));
        TEST(record.bank.envelopes.entries[0].points[1].position == 60);
        TEST(strcmp(record.instrument_names[0], "Sub") == 0);

        remove(path);
    }

    // A no-match delete is idempotent: success, no rewrite, untouched bytes.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        const char* const path = "synth_library_delete_nomatch.tmp";
        TEST(Synth::save_library_record(path, "C1", "N1", &bank, 0) == 0);

        char           before[16384] = {};
        const uint32_t file_size     = read_library_fixture(path, before, sizeof(before));

        Synth::LibraryScanStatus delete_status = Synth::library_absent;
        TEST(Synth::delete_library_record(path, "Missing", "Nope", &delete_status) == 0);
        TEST(delete_status == Synth::library_valid);
        delete_status = Synth::library_absent;
        TEST(Synth::delete_library_category(path, "Missing", &delete_status) == 0);
        TEST(delete_status == Synth::library_valid);

        char after[16384] = {};
        TEST(read_library_fixture(path, after, sizeof(after)) == file_size);
        TEST(memcmp(before, after, file_size) == 0);
        remove(path);
    }

    // A null record name is refused before anything is opened; the file stays
    // byte-identical (the null sentinel would silently delete a category).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        const char* const path = "synth_library_delete_nullname.tmp";
        TEST(Synth::save_library_record(path, "C1", "N1", &bank, 0) == 0);

        char           before[16384] = {};
        const uint32_t file_size     = read_library_fixture(path, before, sizeof(before));

        Synth::LibraryScanStatus delete_status = Synth::library_valid;
        TEST(Synth::delete_library_record(path, "C1", nullptr, &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);

        char after[16384] = {};
        TEST(read_library_fixture(path, after, sizeof(after)) == file_size);
        TEST(memcmp(before, after, file_size) == 0);
        remove(path);
    }

    // A corrupt, oversized or overcapacity library is never rebuilt by a delete:
    // both wrappers return EINVAL with library_invalid (the scanner keeps its own
    // status), and the original bytes and scan result stay untouched.
    {
        Synth::LibraryScanStatus delete_status = Synth::library_valid;

        const char* const junk_path = "synth_library_delete_corrupt.tmp";
        FILE* const       junk      = fopen(junk_path, "wb");
        fwrite("not a library at all", 20, 1, junk);
        fclose(junk);
        char           junk_before[32] = {};
        const uint32_t junk_size       = read_library_fixture(junk_path, junk_before, sizeof(junk_before));
        TEST(Synth::delete_library_record(junk_path, "C", "N", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        delete_status = Synth::library_valid;
        TEST(Synth::delete_library_category(junk_path, "C", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        char junk_after[32] = {};
        TEST(read_library_fixture(junk_path, junk_after, sizeof(junk_after)) == junk_size);
        TEST(memcmp(junk_before, junk_after, junk_size) == 0);
        remove(junk_path);

        // A record past the payload capacity flags the scan oversized.
        const char* const over_path = "synth_library_delete_oversize.tmp";
        FILE* const       over      = fopen(over_path, "wb");
        TEST(over != nullptr);
        const uint32_t over_header[3] = { 0x42494c49, Synth::library_version, 1 };
        fwrite(over_header, sizeof(over_header), 1, over);
        char rec[52] = {};
        memcpy(rec, "Cat", 3);
        memcpy(rec + 24, "Huge", 4);
        const uint32_t huge = Synth::library_payload_max + 2048;
        memcpy(rec + 48, &huge, 4);
        fwrite(rec, sizeof(rec), 1, over);
        const std::unique_ptr<FixedArray<uint8_t, 64 * 1024>> pattern_storage =
            std::make_unique<FixedArray<uint8_t, 64 * 1024>>();
        uint8_t (&pattern)[64 * 1024] = pattern_storage->elements;
        memset(static_cast<void*>(pattern), 0x5a, sizeof(pattern));
        for (uint32_t written = 0; written < huge; written += static_cast<uint32_t>(sizeof(pattern))) {
            const uint32_t chunk = huge - written < static_cast<uint32_t>(sizeof(pattern))
                                       ? huge - written
                                       : static_cast<uint32_t>(sizeof(pattern));
            fwrite(pattern, chunk, 1, over);
        }
        fclose(over);

        Synth::LibraryScanStatus scan_status = Synth::library_valid;
        Synth::LibraryEntry      over_entries[8];
        TEST(Synth::read_library_index(over_path, over_entries, 8, &scan_status) == 1);
        TEST(scan_status == Synth::library_oversized);
        std::unique_ptr<uint8_t[]> over_snapshot;
        const uint64_t             over_size = snapshot_library_file(over_path, over_snapshot);
        delete_status                        = Synth::library_valid;
        TEST(Synth::delete_library_record(over_path, "Cat", "Huge", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        delete_status = Synth::library_valid;
        TEST(Synth::delete_library_category(over_path, "Cat", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        TEST(library_file_matches_snapshot(over_path, over_snapshot, over_size));
        scan_status = Synth::library_valid;
        TEST(Synth::read_library_index(over_path, over_entries, 8, &scan_status) == 1);
        TEST(scan_status == Synth::library_oversized);
        remove(over_path);

        // More records than the index can carry: the delete refuses like the save.
        const char* const many_path = "synth_library_delete_many.tmp";
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
        // The scan stays valid but reports the truncated index before and after
        // the refused delete.
        Synth::LibraryScanStatus many_status    = Synth::library_valid;
        bool                     many_truncated = false;
        Synth::LibraryEntry      many_entries[Synth::library_max_records];
        TEST(Synth::read_library_index(many_path,
                                       many_entries,
                                       Synth::library_max_records,
                                       &many_status,
                                       &many_truncated) == Synth::library_max_records);
        TEST(many_status == Synth::library_valid && many_truncated);
        std::unique_ptr<uint8_t[]> many_snapshot;
        const uint64_t             many_size = snapshot_library_file(many_path, many_snapshot);
        delete_status                        = Synth::library_valid;
        TEST(Synth::delete_library_record(many_path, "Cat", "N000", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        delete_status = Synth::library_valid;
        TEST(Synth::delete_library_category(many_path, "Cat", &delete_status) == EINVAL);
        TEST(delete_status == Synth::library_invalid);
        TEST(library_file_matches_snapshot(many_path, many_snapshot, many_size));
        many_status    = Synth::library_valid;
        many_truncated = false;
        TEST(Synth::read_library_index(many_path,
                                       many_entries,
                                       Synth::library_max_records,
                                       &many_status,
                                       &many_truncated) == Synth::library_max_records);
        TEST(many_status == Synth::library_valid && many_truncated);
        remove(many_path);
    }

    // An absent library reports absence for both wrappers.
    {
        Synth::LibraryScanStatus delete_status = Synth::library_valid;
        TEST(Synth::delete_library_record("synth_library_missing.tmp", "C", "N", &delete_status) == ENOENT);
        TEST(delete_status == Synth::library_absent);
        delete_status = Synth::library_valid;
        TEST(Synth::delete_library_category("synth_library_missing.tmp", "C", &delete_status) == ENOENT);
        TEST(delete_status == Synth::library_absent);
    }

    // The editor bank JSON codec round-trips a populated bank byte-for-byte.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        // The per-layer format stores live layers only, so normalize the fixture
        // the way decode's canonicalization does.
        zero_dormant_layers(bank.bank.instruments.entries[0]);
        for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
            Synth::get_default_channel_name(channel, bank.channel_names[channel], Synth::max_name_len);
        }
        memcpy(bank.instrument_names[0], "Recipe", 7);

        TEST(Synth::validate_instrument_bank(&bank.bank));

        const std::unique_ptr<FixedArray<char, 128 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 128 * 1024>>();
        char (&doc)[128 * 1024] = doc_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0 && len < sizeof(doc));
        TEST(doc[0] == '{');

        const std::unique_ptr<Synth::InstrumentEditorBank> restored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& restored = *restored_storage;
        TEST(Synth::decode_editor_bank_json(doc, len, &restored));
        TEST(memcmp(&bank, &restored, sizeof(bank)) == 0);

        // A file round-trip through the reader/writer is equally faithful
        const std::unique_ptr<Synth::InstrumentEditorBank> from_file_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& from_file = *from_file_storage;
        const char* const            path      = "synth_bank_json.tmp";
        TEST(Synth::save_editor_bank_file(path, &bank) == 0);
        TEST(Synth::load_editor_bank_file(path, &from_file) == Synth::BankFileStatus::ok);
        TEST(memcmp(&bank, &from_file, sizeof(bank)) == 0);
        remove(path);

        // Per-layer routing (AC4): the bank format stores each layer's targets
        // separately and decode no longer fans out, so divergent layers survive
        // a save/load byte-for-byte.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> routed_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& routed = *routed_storage;
            memset(static_cast<void*>(&routed), 0, sizeof(routed));
            routed.bank.channel_enabled[0] = 1;
            TEST(routed.bank.instruments.allocate() == 0);
            Synth::Instrument& routed_instr = routed.bank.instruments.entries[0];
            routed_instr.layer_count        = 3;
            set_default_dispersion_sections(routed_instr);
            for (uint32_t layer = 0; layer < 3; layer++) {
                routed_instr.layers[layer].osc_type[0]       = Synth::WaveType::sine_wave;
                routed_instr.layers[layer].fir_slope_percent = 100;
            }
            // Layer 0: two ordered volume inputs, negative pitch base.
            routed_instr.layers[0].targets[Synth::mod_volume].base_value = 0.8f;
            routed_instr.layers[0].targets[Synth::mod_volume].num_inputs = 2;
            routed_instr.layers[0].targets[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity,
                                                                             Synth::SourceOp::add,
                                                                             0,
                                                                             0.5f };
            routed_instr.layers[0].targets[Synth::mod_volume].inputs[1]  = { Synth::ModSource::mod_wheel,
                                                                             Synth::SourceOp::multiply,
                                                                             0,
                                                                             -0.25f };
            routed_instr.layers[0].targets[Synth::mod_pitch].base_value  = -2.0f;
            // Layer 1: one volume input, opposite pitch base, duty A base.
            routed_instr.layers[1].targets[Synth::mod_volume].base_value = 0.3f;
            routed_instr.layers[1].targets[Synth::mod_volume].num_inputs = 1;
            routed_instr.layers[1].targets[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity,
                                                                             Synth::SourceOp::add,
                                                                             0,
                                                                             0.2f };
            routed_instr.layers[1].targets[Synth::mod_pitch].base_value  = 1.5f;
            routed_instr.layers[1].targets[Synth::mod_duty0].base_value  = 0.4f;
            // Layer 2 carries no routing at all: omitted layers decode to the
            // zeroed binding.
            routed.bank.channel_zones[0][0] = { 1, 0 };
            TEST(Synth::validate_instrument_bank(&routed.bank));

            const std::unique_ptr<FixedArray<char, 64 * 1024>> routed_doc_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&routed_doc)[64 * 1024] = routed_doc_storage->elements;
            const uint32_t routed_len     = Synth::encode_editor_bank_json(&routed, routed_doc, sizeof(routed_doc));
            TEST(routed_len > 0);
            const std::unique_ptr<Synth::InstrumentEditorBank> routed_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& routed_decoded = *routed_decoded_storage;
            TEST(Synth::decode_editor_bank_json(routed_doc, routed_len, &routed_decoded));
            const Synth::Instrument& dec = routed_decoded.bank.instruments.entries[0];
            TEST(dec.layers[0].targets[Synth::mod_volume].base_value == 0.8f);
            TEST(dec.layers[0].targets[Synth::mod_volume].num_inputs == 2);
            TEST(dec.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(dec.layers[0].targets[Synth::mod_volume].inputs[0].scale == 0.5f);
            TEST(dec.layers[0].targets[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
            TEST(dec.layers[0].targets[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
            TEST(dec.layers[0].targets[Synth::mod_volume].inputs[1].scale == -0.25f);
            TEST(dec.layers[0].targets[Synth::mod_pitch].base_value == -2.0f);
            TEST(dec.layers[1].targets[Synth::mod_volume].base_value == 0.3f);
            TEST(dec.layers[1].targets[Synth::mod_volume].num_inputs == 1);
            TEST(dec.layers[1].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(dec.layers[1].targets[Synth::mod_volume].inputs[0].scale == 0.2f);
            TEST(dec.layers[1].targets[Synth::mod_pitch].base_value == 1.5f);
            TEST(dec.layers[1].targets[Synth::mod_duty0].base_value == 0.4f);
            TEST(dec.layers[1].targets[Synth::mod_pitch].num_inputs == 0);
            TEST(dec.layers[2].targets[Synth::mod_volume].base_value == 0.0f);
            TEST(dec.layers[2].targets[Synth::mod_volume].num_inputs == 0);
            TEST(dec.layers[2].targets[Synth::mod_pitch].base_value == 0.0f);
            TEST(dec.layers[2].targets[Synth::mod_duty0].base_value == 0.0f);
            TEST(dec.layers[3].targets[Synth::mod_volume].base_value == 0.0f);
        }

        // Absent means a fresh project, not a failure
        const std::unique_ptr<Synth::InstrumentEditorBank> scratch_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& scratch = *scratch_storage;
        TEST(Synth::load_editor_bank_file("synth_bank_missing.tmp", &scratch) == Synth::BankFileStatus::absent);
        // A name array without its terminator cannot be read safely: the encoder
        // refuses instead of scanning past the fixed array.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> unterminated_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& unterminated = *unterminated_storage;
            make_valid_bank(unterminated.bank);
            Sculptor::set_identity_osc_sources(&unterminated, 0, 0);
            unterminated.bank.instruments.num_allocated = 1;
            memset(static_cast<void*>(unterminated.instrument_names[0]), 'x', Synth::max_name_len);
            const std::unique_ptr<FixedArray<char, 64 * 1024>> small_buffer_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&small_buffer)[64 * 1024] = small_buffer_storage->elements;
            TEST(Synth::encode_editor_bank_json(&unterminated, small_buffer, sizeof(small_buffer)) == 0);
            memset(static_cast<void*>(unterminated.instrument_names[0]), 0, Synth::max_name_len);
            memset(static_cast<void*>(unterminated.channel_names[0]), 'y', Synth::max_name_len);
            TEST(Synth::encode_editor_bank_json(&unterminated, small_buffer, sizeof(small_buffer)) == 0);
            // The encoder's %.9g spelling of the finite float extrema round-trips:
            // 3.40282347e+38 sits just past the exact float32 maximum read as a double,
            // yet converts back to the same finite float.
            {
                const std::unique_ptr<Synth::InstrumentEditorBank> extreme_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& extreme = *extreme_storage;
                make_valid_bank(extreme.bank);
                Sculptor::set_identity_osc_sources(&extreme, 0, 0);
                extreme.bank.envelopes.entries[0].min_value     = -FLT_MAX;
                extreme.bank.envelopes.entries[0].min_max_delta = FLT_MAX;
                const std::unique_ptr<FixedArray<char, 64 * 1024>> extreme_text_storage =
                    std::make_unique<FixedArray<char, 64 * 1024>>();
                char (&extreme_text)[64 * 1024] = extreme_text_storage->elements;
                const uint32_t extreme_len =
                    Synth::encode_editor_bank_json(&extreme, extreme_text, sizeof(extreme_text));
                TEST(extreme_len != 0);
                const std::unique_ptr<Synth::InstrumentEditorBank> extreme_back_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& extreme_back = *extreme_back_storage;
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
                const std::unique_ptr<FixedArray<char, 1024 * 1024 + 16>> oversized_storage =
                    std::make_unique<FixedArray<char, 1024 * 1024 + 16>>();
                char (&oversized)[1024 * 1024 + 16] = oversized_storage->elements;
                memset(static_cast<void*>(oversized), ' ', sizeof(oversized));
                oversized[0] = '{';
                oversized[1] = '}';
                const std::unique_ptr<Synth::InstrumentEditorBank> untouched_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& untouched = *untouched_storage;
                memset(static_cast<void*>(&untouched), 0x3C, sizeof(untouched));
                TEST(! Synth::decode_editor_bank_json(oversized, sizeof(oversized), &untouched));
                TEST(static_cast<unsigned char>(untouched.channel_names[0][0]) == 0x3C);
                // A Unicode escape decodes to the same bytes as its literal UTF-8
                // spelling, so both forms of a name are interchangeable and the
                // re-encoded document stays valid JSON.
                {
                    const std::unique_ptr<Synth::InstrumentEditorBank> from_literal_storage =
                        std::make_unique<Synth::InstrumentEditorBank>();
                    Synth::InstrumentEditorBank&                       from_literal = *from_literal_storage;
                    const std::unique_ptr<Synth::InstrumentEditorBank> from_escape_storage =
                        std::make_unique<Synth::InstrumentEditorBank>();
                    Synth::InstrumentEditorBank& from_escape = *from_escape_storage;
                    const char* const            literal_doc =
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
            const std::unique_ptr<Synth::InstrumentEditorBank> binary_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& binary_bank = *binary_bank_storage;
            memset(static_cast<void*>(&binary_bank), 0x3C, sizeof(binary_bank));
            TEST(Synth::load_editor_bank_file(binary_path, &binary_bank) == Synth::BankFileStatus::invalid);
            const uint8_t* const sentinel  = reinterpret_cast<const uint8_t*>(&binary_bank);
            bool                 untouched = true;
            for (size_t k = 0; k < sizeof(binary_bank); k++) {
                untouched = untouched && sentinel[k] == 0x3C;
            }
            TEST(untouched);
            remove(binary_path);
        }

        // Defaults fill: an empty bank document keeps the factory defaults and
        // still validates
        const std::unique_ptr<Synth::InstrumentEditorBank> defaults_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& defaults = *defaults_storage;
        TEST(Synth::decode_editor_bank_json("{\"instrument_editor_bank\":{}}",
                                            static_cast<uint32_t>(strlen("{\"instrument_editor_bank\":{}}")),
                                            &defaults));
        TEST(defaults.bank.instruments.num_allocated == 0);
        TEST(strcmp(defaults.channel_names[0], "Channel 01") == 0);
        TEST(Synth::validate_instrument_bank(&defaults.bank));
    }

    // Unknown fields are skipped and logged in debug builds; the decode still
    // succeeds.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

        const char* const doc = "{\"bogus\":1,\"instrument_editor_bank\":{\"future_thing\":[3]},\"toplevel\":true}";
        TEST(Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &bank));
        TEST(Synth::validate_instrument_bank(&bank.bank)); // the default bank

        // A record payload with unknown fields loads cleanly
        const std::unique_ptr<Synth::InstrumentEditorBank> src_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& src = *src_storage;
        make_valid_bank(src.bank);
        Sculptor::set_identity_osc_sources(&src, 0, 0);
        memcpy(src.instrument_names[0], "Rec", 4);
        const char* const lib_path = "synth_library_report.tmp";
        TEST(Synth::save_library_record(lib_path, "C", "N", &src, 0) == 0);

        Synth::LibraryEntry entries[4];
        TEST(Synth::read_library_index(lib_path, entries, 4) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> dst_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& dst  = *dst_storage;
        uint16_t                     slot = 0;
        TEST(Synth::load_library_instrument(lib_path, &entries[0], &dst, 1, &slot));
        remove(lib_path);
    }

    // Malformed documents are rejected; a rejected decode never commits: the
    // destination keeps its bytes on syntax, schema and semantic failures alike.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

        const char* const bad[] = { "{",
                                    "{\"instrument_editor_bank\":}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":9,}}",
                                    "{\"instrument_editor_bank\":{\"envelopes\":[{\"num_points\":\"x\"}]}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":01}}",
                                    "{\"instrument_editor_bank\":{\"envelopes\":[{\"num_points\":1e999}]}}",
                                    "{\"instrument_editor_bank\":{\"drum_track_channel\":9}} trailing",
                                    ("{\"instrument_editor_bank\":{\"envelopes\":[{\"num_points\":3,\"points\":"
                                     "[{\"position\":0},{\"position\":1}]}]}}"),
                                    "[1,2,3]",
                                    "garbage" };

        for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            // The destination holds a recognizable pattern; every failure keeps it
            memset(static_cast<void*>(&bank), 0x5A, sizeof(bank));
            TEST(! Synth::decode_editor_bank_json(bad[i], static_cast<uint32_t>(strlen(bad[i])), &bank));
            const bool unchanged =
                static_cast<const uint8_t*>(static_cast<const void*>(&bank))[0] == 0x5A &&
                static_cast<const uint8_t*>(static_cast<const void*>(&bank))[sizeof(bank) - 1] == 0x5A;
            TEST(unchanged);
        }

        // A semantically invalid document (zero-period LFO) is rejected by the
        // validation stage, and the destination is still untouched
        memset(static_cast<void*>(&bank), 0x5A, sizeof(bank));
        const char* const semantic = "{\"instrument_editor_bank\":{\"lfos\":[{}]}}";
        TEST(! Synth::decode_editor_bank_json(semantic, static_cast<uint32_t>(strlen(semantic)), &bank));
        TEST(static_cast<const uint8_t*>(static_cast<const void*>(&bank))[0] == 0x5A);
    }

    // Strict grammar: separators, string content, containment and duplicate keys
    // are enforced document-wide - including inside unknown subtrees the schema
    // decoder never visits.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

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
            "{\"\\u0069nstrument_editor_bank\":{\"\\u0063hannel_names\":[\"Probe\"]},\"\\u0066uture\":1}";
        TEST(Synth::decode_editor_bank_json(escaped, static_cast<uint32_t>(strlen(escaped)), &bank));
        TEST(strcmp(bank.channel_names[0], "Probe") == 0);
    }

    // Key uniqueness holds at any length and across escape spellings: comparison
    // walks decoded symbol streams with no whole-key buffer, so two over-long
    // spellings of one name still collide, and hex case alone cannot tell two
    // escapes of one code point apart.  A long but unique key stays legal - it is
    // logged by its decoded prefix, never rejected for its length.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

        char doc[2048] = {};

        // 256 literal 'a's vs "\u0061" + 255 'a's: both decode to 256 'a's
        uint32_t len = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\"future\":{\""));
        memset(static_cast<void*>(doc + len), 'a', 256);
        len += 256;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":1,\"\\u0061"));
        memset(static_cast<void*>(doc + len), 'a', 255);
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
        memset(static_cast<void*>(doc + len), 'x', 300);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

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
        char     doc[1024] = {};
        uint32_t len       = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\""));
        for (uint32_t i = 0; i < 100; i++) {
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\\u010a"));
        }
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\":1}"));
        TEST(len < sizeof(doc));
        TEST(Synth::decode_editor_bank_json(doc, len, &bank));
    }

    // Pool capacity: a document repeating pool arrays or the root bank object
    // exhausts a pool and is rejected - never written past the pool's entries.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;

        // A full instruments pool followed by a repeated array overflows without
        // the allocation checks
        const std::unique_ptr<FixedArray<char, 256 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 256 * 1024>>();
        char (&doc)[256 * 1024] = doc_storage->elements;
        uint32_t len            = 0;
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
        const std::unique_ptr<FixedArray<char, 256 * 1024>> doc2_storage =
            std::make_unique<FixedArray<char, 256 * 1024>>();
        char (&doc2)[256 * 1024] = doc2_storage->elements;
        uint32_t len2            = 0;
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

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
        bank.bank.instruments.entries[0].layers[2].octave_offset                               = 5;
        bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_panning].num_inputs      = 1;
        bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_panning].inputs[1].scale = 7.0f;
        bank.bank.envelopes.entries[0].points[7].position                                      = 999;
        bank.bank.envelopes.entries[0].min_value                                               = -0.0f;
        memcpy(bank.instrument_names[0], "0123456789ABCDEF", 17);
        memcpy(bank.instrument_names[0], "Rec", 4);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const std::unique_ptr<FixedArray<char, 128 * 1024>> text1_storage =
            std::make_unique<FixedArray<char, 128 * 1024>>();
        char (&text1)[128 * 1024] = text1_storage->elements;
        const std::unique_ptr<FixedArray<char, 128 * 1024>> text2_storage =
            std::make_unique<FixedArray<char, 128 * 1024>>();
        char (&text2)[128 * 1024] = text2_storage->elements;
        const uint32_t len1       = Synth::encode_editor_bank_json(&bank, text1, sizeof(text1));
        TEST(len1 > 0 && len1 < sizeof(text1));

        const std::unique_ptr<Synth::InstrumentEditorBank> round1_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& round1 = *round1_storage;
        TEST(Synth::decode_editor_bank_json(text1, len1, &round1));

        // The canonical form: negative zero normalized, stale bytes gone
        TEST(! std::signbit(round1.bank.envelopes.entries[0].min_value));
        TEST(round1.bank.envelopes.entries[1].num_points == 0);
        TEST(round1.bank.envelopes.entries[1].points[1].position == 0);
        TEST(round1.bank.lfos.entries[1].period_ms == 0);
        TEST(round1.bank.instruments.entries[0].layers[2].octave_offset == 0);
        TEST(round1.bank.instruments.entries[0].layers[0].targets[Synth::mod_panning].inputs[1].scale == 0.0f);
        TEST(round1.bank.envelopes.entries[0].points[7].position == 0);
        TEST(round1.instrument_names[0][3] == 0);
        TEST(round1.instrument_names[0][10] == 0);
        TEST(Synth::validate_instrument_bank(&round1.bank));

        const uint32_t len2 = Synth::encode_editor_bank_json(&round1, text2, sizeof(text2));
        TEST(len2 > 0 && len2 < sizeof(text2));
        TEST(len1 == len2);
        TEST(memcmp(text1, text2, len1) == 0); // save(load(save(bank))) == save(bank)

        const std::unique_ptr<Synth::InstrumentEditorBank> round2_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& round2 = *round2_storage;
        TEST(Synth::decode_editor_bank_json(text2, len2, &round2));
        TEST(memcmp(&round1, &round2, sizeof(round1)) == 0);
    }

    // Reload guarantee: a valid bank whose document exceeds the bounded token pool
    // is refused by the save instead of writing an unloadable file.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));

        // Dense instruments: every layer binds every target to both descriptors,
        // so the encoded document needs far more than the 16384-token pool
        const uint32_t env                                  = bank.bank.envelopes.allocate();
        bank.bank.envelopes.entries[env].num_points         = 2;
        bank.bank.envelopes.entries[env].points[1].position = 100;
        const uint32_t lfo                                  = bank.bank.lfos.allocate();
        bank.bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[lfo].period_ms               = 50;

        for (uint32_t i = 0; i < 16; i++) {
            const uint32_t     slot  = bank.bank.instruments.allocate();
            Synth::Instrument& instr = bank.bank.instruments.entries[slot];
            instr.layer_count        = Synth::max_layers;
            set_default_dispersion_sections(instr);
            for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
                Synth::Oscillator& osc = instr.layers[layer];
                osc.osc_type[0]        = Synth::WaveType::sine_wave;
                osc.osc_mode           = Synth::osc_mode_fm;
                osc.mod_ratio          = 1.0f;
                for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
                    Synth::TargetBinding& gen = osc.targets[target];
                    gen.envelope_desc_id      = static_cast<uint16_t>(env + 1);
                    gen.lfo_desc_id           = static_cast<uint16_t>(lfo + 1);
                    gen.lfo_depth             = 0.5f;
                    gen.lfo_depth_source      = Synth::ModSource::velocity;
                    gen.lfo_rate_source       = Synth::ModSource::mod_wheel;
                    gen.lfo_rate_scale_ms     = 1.0f;
                }
            }
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
                Synth::TargetBinding& routing = instr.layers[0].targets[target];
                routing.base_value            = 0.25f;
                routing.num_inputs            = Synth::max_mod_inputs;
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
        if (check) {
            fclose(check);
        }
        remove(path);
    }

    // The encoder never trusts count metadata: bank validation checks only
    // parameter-pool metadata, not standalone contents, so a validator-accepted
    // bank can carry a malformed parameter.  Encoding it must fail cleanly (no
    // read past the arrays, no file written) instead of trusting the count.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

        const uint32_t param = bank.bank.parameters.allocate();
        TEST(param != pool_no_slot);
        bank.bank.parameters.entries[param].kind              = Synth::ParamKind::plain;
        bank.bank.parameters.entries[param].plain.base_value  = 1.0f;
        bank.bank.parameters.entries[param].plain.num_sources = 65535; // far past sources[4]

        TEST(Synth::validate_instrument_bank(&bank.bank)); // metadata valid, contents unchecked

        const std::unique_ptr<FixedArray<char, 64 * 1024>> text_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&text)[64 * 1024] = text_storage->elements;
        TEST(Synth::encode_editor_bank_json(&bank, text, sizeof(text)) == 0);

        const char* const bad_path = "synth_bank_badparam.tmp";
        TEST(Synth::save_editor_bank_file(bad_path, &bank) != 0);
        FILE* const bad_check = fopen(bad_path, "rb");
        TEST(bad_check == nullptr); // nothing was written
        if (bad_check) {
            fclose(bad_check);
        }
        remove(bad_path);
    }

    // A document beyond the token pool and a bank file beyond the parse buffer are
    // both visibly rejected through their bounded staging.
    {
        const std::unique_ptr<FixedArray<char, 256 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 256 * 1024>>();
        char (&doc)[256 * 1024] = doc_storage->elements;
        uint32_t len            = 0;
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "{\"instrument_editor_bank\":{"));
        for (uint32_t i = 0; i < 16500; i++) {
            len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\"k%u\":1,", i));
        }
        len += static_cast<uint32_t>(snprintf(doc + len, sizeof(doc) - len, "\"x\":1}}"));
        TEST(len < sizeof(doc));

        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        TEST(! Synth::decode_editor_bank_json(doc, len, &bank)); // token pool exhaustion

        const char* const big_path = "synth_bank_toobig.tmp";
        FILE* const       big      = fopen(big_path, "wb");
        TEST(big != nullptr);
        // Keep the oversized fixture, but write its bytes in one operation.
        constexpr uint32_t                                          oversized_bytes = 1024 * 1024 + 64 * 1024;
        const std::unique_ptr<FixedArray<uint8_t, oversized_bytes>> chunk_storage =
            std::make_unique<FixedArray<uint8_t, oversized_bytes>>();
        uint8_t (&chunk)[oversized_bytes] = chunk_storage->elements;
        memset(static_cast<void*>(chunk), 0x61, sizeof(chunk));
        TEST(fwrite(chunk, sizeof(chunk), 1, big) == 1);
        fclose(big);
        // The loader refuses documents past the parse buffer and leaves the
        // destination untouched.
        const std::unique_ptr<Synth::InstrumentEditorBank> toobig_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& toobig_bank = *toobig_bank_storage;
        memset(static_cast<void*>(&toobig_bank), 0x5A, sizeof(toobig_bank));
        TEST(Synth::load_editor_bank_file(big_path, &toobig_bank) == Synth::BankFileStatus::too_large);
        const std::unique_ptr<Synth::InstrumentEditorBank> sentinel_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        memset(static_cast<void*>(sentinel_storage.get()), 0x5A, sizeof(toobig_bank));
        TEST(memcmp(&toobig_bank, sentinel_storage.get(), sizeof(toobig_bank)) == 0);
        remove(big_path);
    }

    // A library container from another version is an invalid file: rejected
    // like any other malformed input, and never rebuilt over.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

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
}

static void check_bank_inline_1()
{
    // A zero-layer instrument without missing bits stays a valid editor
    // state, but its channel publishes disabled: the runtime refuses to
    // voice a zero-layer instrument, so publication must never enable it.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        bank.bank.channel_enabled[0] = 1;
        Synth::Instrument instrument = {};
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        TEST(Synth::validate_instrument_bank(&bank.bank, false));
        TEST(Sculptor::validate_editor_metadata(bank));
        uint8_t enabled[Synth::max_channels] = {};
        Sculptor::compute_publish_channel_enabled(bank, enabled);
        TEST(enabled[0] == 0);
        TEST(bank.bank.channel_enabled[0] == 1);
        // The document round-trips too.
        FILE* stream = tmpfile();
        TEST(stream != nullptr);
        if (stream) {
            uint32_t written = 0;
            TEST(Synth::write_editor_bank_json(stream, &bank, sizeof(bank), &written) == 0);
            rewind(stream);
            const std::unique_ptr<Synth::InstrumentEditorBank> restored_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& restored = *restored_storage;
            TEST(Synth::read_editor_bank_json(stream, written, &restored));
            fclose(stream);
            Sculptor::compute_publish_channel_enabled(restored, enabled);
            TEST(enabled[0] == 0);
        }
    }

    // Zone-table metadata rules copy zone records and the source row after
    // shifting later zones; drop and reset remove.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        add_detached_record(&bank, 0, 0, 0, 5, 1.0f, 2.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        add_detached_record(&bank, 0, 0, 2, 9, 3.0f, 4.0f, Synth::ModSource::velocity, Synth::ModSource::none, 1);
        add_detached_record(&bank, 0, 1, 0, 6, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        add_detached_record(&bank, 0, 2, 0, 7, 7.0f, 8.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
        bank.graph_osc_source[0][0][0] = 1;
        bank.graph_osc_source[0][1][0] = 2;
        bank.graph_osc_source[0][2][0] = 3;

        Sculptor::zone_records_split_copy(&bank, 0, 0);
        TEST(bank.graph_layout_count == 6);
        TEST(count_records_matching(bank, 0, 0, 0, 5) == 1);
        TEST(count_records_matching(bank, 0, 0, 2, 9) == 1); // detached stays in zone 0
        TEST(count_records_matching(bank, 0, 1, 0, 5) == 1); // copied kind-0 record
        TEST(count_records_matching(bank, 0, 1, 2, 9) == 1); // copied generator
        TEST(count_records_matching(bank, 0, 2, 0, 6) == 1); // shifted
        TEST(count_records_matching(bank, 0, 3, 0, 7) == 1);
        TEST(bank.graph_osc_source[0][1][0] == 1); // copied source row
        TEST(bank.graph_osc_source[0][2][0] == 2); // shifted rows
        TEST(bank.graph_osc_source[0][3][0] == 3);

        Sculptor::zone_records_drop_zone(&bank, 0, 1);
        TEST(bank.graph_layout_count == 4);
        TEST(count_records_matching(bank, 0, 1, 0, 5) == 0); // dropped
        TEST(count_records_matching(bank, 0, 1, 0, 6) == 1); // shifted down
        TEST(count_records_matching(bank, 0, 2, 0, 7) == 1);
        TEST(bank.graph_osc_source[0][1][0] == 2);
        TEST(bank.graph_osc_source[0][2][0] == 3);
        TEST(bank.graph_osc_source[0][3][0] == 0);

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
        bank.graph_osc_source[1][0][0] = 7;
        Sculptor::channel_records_reset(&bank, 0);
        TEST(bank.graph_layout_count == 0);
        TEST(bank.graph_osc_source[0][0][0] == 0 && bank.graph_osc_source[0][2][0] == 0);
        TEST(bank.graph_osc_source[1][0][0] == 7); // other channels untouched
    }

    // Preflight arithmetic: the record-list capacity and detached-count
    // accounting the record-creating operations preflight against.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
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
        const std::unique_ptr<Synth::InstrumentBank> model_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       model_bank         = *model_bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        bank.bank                    = model_bank;
        bank.bank.channel_enabled[0] = 1;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        TEST(Sculptor::count_projected_nodes(bank, 0, 0) == 66);

        const std::unique_ptr<Synth::InstrumentEditorBank> minimal_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& minimal            = *minimal_storage;
        Synth::Instrument            minimal_instrument = {};
        build_zone_fixture(&minimal, &minimal_instrument);
        TEST(Sculptor::count_projected_nodes(minimal, 0, 0) == 7);
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
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
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
        bank.graph_osc_source[0][0][0] = 1;
        // A second zone with a two-layer instrument so the round trip covers
        // two nonzero source rows; both bits stay within that layer count.
        TEST(bank.bank.instruments.allocate() == 1);
        bank.bank.instruments.entries[1]             = Synth::Instrument{};
        bank.bank.instruments.entries[1].layer_count = 2;
        set_default_dispersion_sections(bank.bank.instruments.entries[1]);
        bank.bank.channel_zones[1][0].start_note = 1;
        bank.bank.channel_zones[1][0].instrument = 1;
        bank.graph_osc_source[1][0][0]           = 1;
        bank.graph_osc_source[1][0][1]           = 2;

        const std::unique_ptr<FixedArray<char, 256 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 256 * 1024>>();
        char (&doc)[256 * 1024] = doc_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0 && len < sizeof(doc));
        const std::unique_ptr<Synth::InstrumentEditorBank> restored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& restored = *restored_storage;
        TEST(Synth::decode_editor_bank_json(doc, len, &restored));
        TEST(restored.graph_layout_count == 3);
        TEST(memcmp(restored.graph_layout, bank.graph_layout, sizeof(bank.graph_layout)) == 0);
        TEST(memcmp(restored.graph_osc_source, bank.graph_osc_source, sizeof(bank.graph_osc_source)) == 0);
    }

    // The decode refuses malformed editor metadata (records with
    // out-of-range bytes, over-cap counts, overfull implied projections) and
    // drops duplicate record keys.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        Synth::init_default_bank(&bank.bank);
        // The detached-layout bounds tests need one descriptor in the pool:
        // ids at the allocated bound decode, one past it refuse.
        TEST(bank.bank.lfos.allocate() == 0);
        bank.bank.lfos.entries[0].wave      = Synth::WaveType::sine_wave;
        bank.bank.lfos.entries[0].period_ms = 50;
        const std::unique_ptr<Synth::InstrumentEditorBank> restored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                        restored = *restored_storage;
        const std::unique_ptr<FixedArray<char, 512 * 1024>> editor_json_storage =
            std::make_unique<FixedArray<char, 512 * 1024>>();
        char (&editor_json)[512 * 1024] = editor_json_storage->elements;

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
        // and 2561 records over 16 channels x 16 zones (global cap 2560).
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 121, 2, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(bank, editor_json, &restored));
        // The global record cap: a single editor-section document cannot reach
        // the 2560/2561 boundary anymore (the raised json_token_count of 32768
        // parses roughly 1550 records), so the cap is pinned directly on the
        // validator the decoder runs.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> cap_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& cap_bank = *cap_bank_storage;
            Synth::init_default_bank(&cap_bank.bank);
            TEST(cap_bank.bank.lfos.allocate() != pool_no_slot);
            const uint32_t over_cap  = Synth::max_graph_records + 1;
            const uint32_t pair_span = (over_cap + 255) / 256;
            for (uint32_t i = 0; i < Synth::max_graph_records; i++) {
                Synth::GraphNodeLayout& record = cap_bank.graph_layout[i];
                record.kind                    = 2;
                record.index                   = 1;
                record.channel                 = static_cast<uint8_t>((i / pair_span) / Synth::max_instr_per_channel);
                record.zone                    = static_cast<uint8_t>((i / pair_span) % Synth::max_instr_per_channel);
                record.uid                     = static_cast<uint8_t>((i % pair_span) + 1);
            }
            cap_bank.graph_layout_count = Synth::max_graph_records;
            TEST(Sculptor::validate_editor_metadata(cap_bank));
            cap_bank.graph_layout_count = over_cap;
            TEST(! Sculptor::validate_editor_metadata(cap_bank));
        }

        // Duplicate record keys drop the later record.
        {
            char two_records[1024] = {};
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

        // Overfull implied node projection: the fully-wired fixture implies 304 nodes;
        // 113 detached records push it past 416, 112 land exactly on 416.
        const std::unique_ptr<Synth::InstrumentBank> model_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       model_bank         = *model_bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> max_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& max_bank   = *max_bank_storage;
        Synth::Instrument            instrument = {};
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        // The max_nodes pool (416) leaves the four-target fixture's 66
        // implied nodes too far below the pool for the 120-record detached cap to
        // reach the per-zone node-pool refusal, so this test alone wires fifteen
        // more targets (FM ratio and the fourteen band targets) with full
        // envelope + LFO bindings.
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            Synth::Oscillator& osc                       = instrument.layers[layer];
            osc.fir_type                                 = Synth::fir_bandpass;
            osc.fir_band_count                           = 4;
            osc.targets[Synth::mod_fir_slope].base_value = 50.0f;
            for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
                const bool extra = target == Synth::mod_fm_ratio ||
                                   (target >= Synth::mod_fir_band0_offset && target <= Synth::mod_fir_band3_slope) ||
                                   target == Synth::mod_string_damping || target == Synth::mod_string_release_damping;
                if (osc_graph_target(static_cast<Synth::ModTarget>(target)) || ! extra) {
                    continue;
                }

                const uint32_t env_slot = model_bank.envelopes.allocate();
                TEST(env_slot != pool_no_slot);
                Synth::EnvelopeDescriptor& env = model_bank.envelopes.entries[env_slot];
                env.num_points                 = 2;
                env.sustain_first_point        = 0;
                env.sustain_last_point         = 1;
                env.min_value                  = -1.0f;
                env.min_max_delta              = 1.0f;
                env.points[0].position         = 100;
                env.points[0].value            = 0x2000;
                env.points[1].position         = 200;
                env.points[1].value            = 0x4000;

                const uint32_t lfo_slot = model_bank.lfos.allocate();
                TEST(lfo_slot != pool_no_slot);
                Synth::LFODescriptor& lfo = model_bank.lfos.entries[lfo_slot];
                lfo.wave                  = Synth::WaveType::sawtooth_wave;
                lfo.duty                  = 0x40;
                lfo.period_ms             = 137;
                lfo.min_value             = -0.5f;
                lfo.min_max_delta         = 0.5f;

                Synth::TargetBinding& gen = osc.targets[target];
                gen.base_value = target == Synth::mod_fir_band0_gain                                            ? 0.25f
                                 : target >= Synth::mod_fir_band1_offset && target <= Synth::mod_fir_band1_gain ? 0.25f
                                 : target >= Synth::mod_fir_band2_offset && target <= Synth::mod_fir_band2_gain ? 0.25f
                                 : target >= Synth::mod_fir_band3_offset && target <= Synth::mod_fir_band3_gain ? 0.25f
                                 : (target == Synth::mod_fir_band1_slope || target == Synth::mod_fir_band2_slope ||
                                    target == Synth::mod_fir_band3_slope)
                                     ? 50.0f
                                     : 0.0f;
                gen.envelope_desc_id  = static_cast<uint16_t>(env_slot + 1);
                gen.lfo_desc_id       = static_cast<uint16_t>(lfo_slot + 1);
                gen.lfo_op            = Synth::SourceOp::add;
                gen.lfo_depth         = 0.25f;
                gen.lfo_depth_source  = static_cast<Synth::ModSource>(1 + ((layer + target) % 6));
                gen.lfo_rate_source   = static_cast<Synth::ModSource>(1 + ((layer + 2 * target + 1) % 6));
                gen.lfo_rate_scale_ms = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(target);
            }
        }
        max_bank.bank                    = model_bank;
        max_bank.bank.channel_enabled[0] = 1;
        TEST(max_bank.bank.instruments.allocate() == 0);
        max_bank.bank.instruments.entries[0]         = instrument;
        max_bank.bank.channel_zones[0][0].start_note = 1;
        max_bank.bank.channel_zones[0][0].instrument = 0;
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 113, 2, 1, 0, 0);
        TEST(! decode_bank_with_editor_section(max_bank, editor_json, &restored));
        build_detached_layouts_json(editor_json, sizeof(editor_json), 1, 112, 2, 1, 0, 0);
        TEST(decode_bank_with_editor_section(max_bank, editor_json, &restored));
        // The accepted boundary fills the node pool exactly: the projection
        // still succeeds at 304 + 112 = 416 nodes.
        const std::unique_ptr<Sculptor::Graph> boundary_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       boundary_graph         = *boundary_graph_storage;
        Sculptor::OscGraphMapping              boundary_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(restored, &boundary_graph, &boundary_mapping));
    }
}

static void check_bank_inline_2()
{

    // Kind-4 records (effect-graph node places): keyed by name, valid on
    // every chain including the master, refusing wrong-kind key fields,
    // duplicates and empty names, and round-tripping through the bank file.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
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

        bank.bank.master_chain.num_effects = 1;
        Sculptor::fx_init_slot(&bank.bank.master_chain, 0, Synth::EffectType::delay);
        const std::unique_ptr<FixedArray<char, 512 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 512 * 1024>>();
        char (&doc)[512 * 1024] = doc_storage->elements;
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        const uint32_t               written = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
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
}

static void check_bank_inline_3()
{

    // The decoder rejects objects in key position: the grammar admits only
    // comma-separated string keys, at the root and in unknown subtrees.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       bank = *bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded    = *decoded_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        char doc[512] = {};
        snprintf(doc, sizeof(doc), "{\"instrument_editor_bank\":{}{\"editor\":{}}}");
        TEST(! Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &decoded));
        snprintf(doc, sizeof(doc), "{\"instrument_editor_bank\":{},\"x\":{\"a\":1}{\"b\":2}}");
        TEST(! Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), &decoded));
    }
}

int main()
{
    test_silence_stdout();
    envelope_depth_serialization_regressions();
    check_editor_instrument_round_trips();
    check_new_zone_metadata_clone();
    check_zero_fm_clipboard_library_round_trips();
    check_identical_envelopes_library_round_trips();
    check_fragmented_pool_ids_library_round_trips();
    check_library_metadata_transactions();
    check_editor_round_trip_capacity_failures();
    check_unrepresented_layout_refusal();
    check_decoded_fm_lfo_binding_survives_compile();
    check_renamed_oscillator_persists_through_save_load_and_reprojection();
    check_exactly_32_byte_oscillator_name_round_trips();
    check_empty_rename_is_a_no_op();
    check_terminated_kind4_name_still_refused_to_decode();
    check_validator_ignores_stale_kind125_names();
    check_validator_accepts_lfo0_title();
    check_validator_ignores_kind45_reserved_fields();
    check_validator_skips_depth_rate_for_unused_kinds();
    check_validator_accepts_duplicate_osc_in_row();
    check_validator_accepts_portable_name_and_override();
    check_legacy_library_numeric_domains();
    check_pad_json_codec();
    check_pad_tilt_json_codec();
    check_derive_generator_title_builds_titles();
    check_bank_inline_0();
    check_bank_inline_1();
    check_bank_inline_2();
    check_bank_inline_3();
    return test_exit_code();
}
