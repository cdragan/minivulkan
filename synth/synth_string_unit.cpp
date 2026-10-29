// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

constexpr float string_fs = 44100.0f;

constexpr uint32_t string_ring_size = 2034;

constexpr uint32_t string_max_sections = 6;

struct StringScalars {
    float    c;
    float    a;
    float    g;
    float    gr;
    uint32_t n;
    bool     silent;
    float    disp_a;
    uint32_t num_sections;
    float    disp_delay;
};

struct StringMirror {
    bool     tagged; // Models shader header words 5-6: set when the first in-range pass inits
    uint32_t tag;
    float    ds[string_max_sections];
    float    ring[string_ring_size];
    uint32_t wr;
    float    t1;
    float    u1;
    float    e;
    float    chain_lo; // Models shader header words 8-9: the chain token split in two integer-valued floats
    float    chain_hi; // lo = float((token & 0xFFFF) + 1), hi = float(token >> 16); both 0 for a clean region
};

// D11/TRAP: a string layer with a non-default brightness and routing on
// targets 26/27 at band count 1 round-trips encode -> decode -> project ->
// compile, and the decoded compile equals the direct compile of the source.
static void check_string_brightness_trap_decode_compile()
{
    if (Synth::num_mod_targets < 28) {
        return;
    }
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Synth::Oscillator& layer = instrument.layers[0];
    layer.osc_type[0]        = Synth::WaveType::string_wave;
    layer.fir_band_count     = 1;
    layer.string_brightness  = 0.25f;
    for (uint32_t t = Synth::mod_string_damping; t <= Synth::mod_string_release_damping; ++t) {
        layer.targets[t].base_value     = 0.5f;
        layer.targets[t].envelope_depth = 1.0f; // the decoded default
        layer.targets[t].lfo_desc_id    = 1;
        layer.targets[t].lfo_op         = Synth::SourceOp::add;
        layer.targets[t].lfo_depth      = 0.25f;
    }
    bank.bank.instruments.entries[0] = instrument;
    TEST(Synth::validate_instrument_bank(&bank.bank));

    // Direct compile of the source.
    const std::unique_ptr<Sculptor::Graph> direct_graph   = std::make_unique<Sculptor::Graph>();
    Sculptor::OscGraphMapping              direct_mapping = {};
    TEST(Sculptor::project_editor_to_graph(bank, direct_graph.get(), &direct_mapping, 0, 0));
    Synth::Instrument direct = {};
    TEST(Sculptor::compile_graph_to_instrument(*direct_graph, direct_mapping, &direct));
    TEST(direct.layers[0].string_brightness == 0.25f);

    // Encode, decode, then compile the decoded bank.
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
    const Synth::Oscillator& dec =
        decoded.bank.instruments.entries[decoded.bank.channel_zones[0][0].instrument].layers[0];
    TEST(dec.string_brightness == 0.25f);
    for (uint32_t t = Synth::mod_string_damping; t <= Synth::mod_string_release_damping; ++t) {
        TEST(dec.targets[t].base_value == 0.5f);
        TEST(dec.targets[t].lfo_desc_id == 1);
        TEST(dec.targets[t].lfo_depth == 0.25f);
    }
    const std::unique_ptr<Sculptor::Graph> decoded_graph   = std::make_unique<Sculptor::Graph>();
    Sculptor::OscGraphMapping              decoded_mapping = {};
    TEST(Sculptor::project_editor_to_graph(decoded, decoded_graph.get(), &decoded_mapping, 0, 0));
    Synth::Instrument recompiled = {};
    TEST(Sculptor::compile_graph_to_instrument(*decoded_graph, decoded_mapping, &recompiled));
    TEST(recompiled.layers[0].string_brightness == 0.25f);
    TEST(memcmp(&recompiled.layers[0], &direct.layers[0], sizeof(Synth::Oscillator)) == 0);
}

static bool string_clip_decode(const char* doc, ClipboardDecode* out)
{
    fill_clipboard_sentinels(out);
    return decode_clipboard(out, doc, max_portable_records);
}

static void check_string_wave_json_codec()
{
    const std::unique_ptr<ClipboardDecode> storage  = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded  = *storage;
    static const char* const               modes[4] = { "blend", "fm", "sync", "ring_mod" };
    char                                   doc[512];

    // Wave A string in all four modes: strict document and lenient bank.
    for (uint32_t m = 0; m < 4; ++m) {
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"wave_b\":\"sine\","
                 "\"mode\":\"%s\"}]}",
                 modes[m]);
        TEST(string_clip_decode(doc, &decoded));
        TEST(decoded.instr.layers[0].osc_type[0] == Synth::WaveType::string_wave);
        TEST(static_cast<uint32_t>(decoded.instr.layers[0].osc_mode) == m);

        snprintf(doc,
                 sizeof(doc),
                 "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
                 "\"wave_b\":\"sine\",\"mode\":\"%s\",\"routing\":{}}]}],"
                 "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}",
                 modes[m]);
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
        TEST(Synth::decode_editor_bank_json(doc, static_cast<uint32_t>(strlen(doc)), bank_storage.get()));
        TEST(bank_storage->bank.instruments.entries[0].layers[0].osc_type[0] == Synth::WaveType::string_wave);
    }

    // Wave B string is refused by the strict decoder and the lenient bank.
    {
        static const char b_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"wave_b\":\"string\"}]}";
        TEST(! string_clip_decode(b_doc, &decoded));
        static const char b_bank[] =
            "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"sine\","
            "\"wave_b\":\"string\",\"routing\":{}}]}],"
            "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
        TEST(! Synth::decode_editor_bank_json(b_bank, static_cast<uint32_t>(strlen(b_bank)), bank_storage.get()));
    }

    // Wherever pad is accepted string is too (wave A), and wherever pad is
    // refused (LFO waves) string is refused as well.
    {
        static const char lfo_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                      "\"volume\":{\"lfo\":{\"wave\":\"string\",\"frequency_hz\":4}}}]}";
        TEST(! string_clip_decode(lfo_doc, &decoded));
        static const char lfo_bank[] =
            "{\"instrument_editor_bank\":{\"lfos\":[{\"wave\":\"string\",\"period_ms\":100}]}}";
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
        TEST(! Synth::decode_editor_bank_json(lfo_bank, static_cast<uint32_t>(strlen(lfo_bank)), bank_storage.get()));
    }

    // Legacy pad documents still decode, with no string keys re-encoded.
    {
        static const char pad_doc[] =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"pad\",\"duty_a\":{\"base\":0.5}}]}";
        TEST(string_clip_decode(pad_doc, &decoded));
        TEST(decoded.instr.layers[0].osc_type[0] == static_cast<Synth::WaveType>(5));
    }

    // Encode round trip of wave A string; unused string keys are omitted.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        Synth::Instrument instr = {};
        instr.layer_count       = 1;
        set_default_dispersion_sections(instr);
        instr.layers[0].osc_type[0]       = Synth::WaveType::string_wave;
        instr.layers[0].fir_slope_percent = 100;
        const uint32_t slot               = bank.bank.instruments.allocate();
        TEST(slot != pool_no_slot);
        bank.bank.instruments.entries[slot] = instr;
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&text)[clipboard_capacity] = text_storage->elements;
        const uint32_t length            = Synth::encode_instrument_json(text, sizeof(text), &instr, &bank.bank);
        TEST(length > 0);
        if (length > 0) {
            TEST(strstr(text, "\"wave_a\":\"string\"") != nullptr);
            TEST(strstr(text, "string_pick") == nullptr);
            TEST(strstr(text, "string_damping") == nullptr);
            TEST(strstr(text, "string_release_damping") == nullptr);
            TEST(string_clip_decode(text, &decoded));
            TEST(decoded.instr.layers[0].osc_type[0] == Synth::WaveType::string_wave);
        }
    }
}

// Builds the smallest valid bank (one instrument, one envelope, one LFO, one
// zone) with `wave_a` / `wave_b` on layer 0.
static void string_make_bank(Synth::InstrumentBank& bank, Synth::WaveType wave_a, Synth::WaveType wave_b)
{
    memset(static_cast<void*>(&bank), 0, sizeof(bank));
    const uint32_t env                             = bank.envelopes.allocate();
    bank.envelopes.entries[env].num_points         = 2;
    bank.envelopes.entries[env].points[1].position = 100;
    const uint32_t lfo                             = bank.lfos.allocate();
    bank.lfos.entries[lfo].wave                    = Synth::WaveType::sine_wave;
    bank.lfos.entries[lfo].period_ms               = 50;
    const uint32_t     instr                       = bank.instruments.allocate();
    Synth::Oscillator& layer                       = bank.instruments.entries[instr].layers[0];
    bank.instruments.entries[instr].layer_count    = 1;
    set_default_dispersion_sections(bank.instruments.entries[instr]);
    layer.fir_slope_percent                           = 100;
    layer.string_dispersion_sections                  = 4.0f;
    layer.osc_type[0]                                 = wave_a;
    layer.osc_type[1]                                 = wave_b;
    layer.targets[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(env + 1);
    layer.targets[Synth::mod_pitch].lfo_desc_id       = static_cast<uint16_t>(lfo + 1);
    bank.channel_zones[0][0]                          = { 1, static_cast<uint8_t>(instr) };
    bank.channel_enabled[0]                           = 1;
}

// Validator: wave A string (6) accepted, wave A 7 refused, wave B string refused.
static void check_string_wave_validator()
{
    const std::unique_ptr<Synth::InstrumentBank> storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       bank    = *storage;

    string_make_bank(bank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
    TEST(Synth::validate_instrument_bank(&bank));
    string_make_bank(bank, Synth::WaveType::string_wave, Synth::WaveType::sine_wave);
    TEST(Synth::validate_instrument_bank(&bank));
    string_make_bank(bank, static_cast<Synth::WaveType>(7), Synth::WaveType::no_wave);
    TEST(! Synth::validate_instrument_bank(&bank));
    string_make_bank(bank, Synth::WaveType::sine_wave, Synth::WaveType::string_wave);
    TEST(! Synth::validate_instrument_bank(&bank));
    // Pluck brightness must lie in [0,1]; the bound edges are accepted.
    static const float brightness_ok[] = { 0.0f, 0.5f, 1.0f };
    for (const float brightness : brightness_ok) {
        string_make_bank(bank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
        bank.instruments.entries[0].layers[0].string_brightness = brightness;
        TEST(Synth::validate_instrument_bank(&bank));
    }
    static const float brightness_bad[] = { -0.1f, 1.1f, NAN };
    for (const float brightness : brightness_bad) {
        string_make_bank(bank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
        bank.instruments.entries[0].layers[0].string_brightness = brightness;
        TEST(! Synth::validate_instrument_bank(&bank));
    }
}

// Legacy pick key, damping and release damping JSON bounds and defaults.  The removed pick key is fully inert:
// any numeric value, in range or not, decodes and is ignored; damping and release damping are 0..1 targets.
static void check_string_param_json_bounds()
{
    const std::unique_ptr<ClipboardDecode> storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded = *storage;
    char                                   doc[256];

    // The removed pick key is accepted whatever its value: in range, out of range or negative.
    static const char* const accepted[] = { "0", "0.5", "1", "-0.1", "1.1", "2.0" };
    for (const char* value : accepted) {
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"string_pick\":%s}]}",
                 value);
        TEST(string_clip_decode(doc, &decoded));
    }

    // A legacy bank that still carries string_pick loads to exactly the same instrument as the document without
    // the key (the value is ignored, in range or not), so it renders identically.
    {
        const std::unique_ptr<ClipboardDecode> with_pick    = std::make_unique<ClipboardDecode>();
        const std::unique_ptr<ClipboardDecode> without_pick = std::make_unique<ClipboardDecode>();
        // The decode target is a plain decode buffer; zero every byte exactly as
        // before. The unsigned char pointer avoids the class-memaccess warning.
        memset(reinterpret_cast<unsigned char*>(without_pick.get()), 0, sizeof(ClipboardDecode));
        TEST(string_clip_decode("{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\","
                                "\"string_brightness\":0.5,\"string_dispersion\":0.25}]}",
                                without_pick.get()));
        static const char* const pick_values[] = { "0.75", "2.0", "-0.1" };
        for (const char* value : pick_values) {
            memset(reinterpret_cast<unsigned char*>(with_pick.get()), 0, sizeof(ClipboardDecode));
            snprintf(doc,
                     sizeof(doc),
                     "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"string_pick\":%s,"
                     "\"string_brightness\":0.5,\"string_dispersion\":0.25}]}",
                     value);
            TEST(string_clip_decode(doc, with_pick.get()));
            TEST(memcmp(&with_pick->instr, &without_pick->instr, sizeof(with_pick->instr)) == 0);
            TEST(with_pick->instr.layers[0].string_brightness == 0.5f);
        }
    }

    // Damping and release damping targets: bases decode in range, refuse out of range.
    static const char* const target_keys[2] = { "string_damping", "string_release_damping" };
    for (uint32_t t = 0; t < 2; ++t) {
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"%s\":{\"base\":0.25}}]}",
                 target_keys[t]);
        TEST(string_clip_decode(doc, &decoded));
        if (Synth::num_mod_targets > 26 + t) {
            const Synth::TargetBinding* const bindings = decoded.instr.layers[0].targets;
            TEST(bindings[26 + t].base_value == 0.25f);
        }
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"%s\":{\"base\":1.5}}]}",
                 target_keys[t]);
        TEST(! string_clip_decode(doc, &decoded));
    }

    // Absent keys default to zero: a pad/string layer with no string keys
    // decodes with zeroed damping and release damping bindings.
    {
        static const char absent_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\"}]}";
        TEST(string_clip_decode(absent_doc, &decoded));
        if (Synth::num_mod_targets > 27) {
            const Synth::TargetBinding* const bindings = decoded.instr.layers[0].targets;
            TEST(bindings[26].base_value == 0.0f && bindings[26].num_inputs == 0);
            TEST(bindings[27].base_value == 0.0f && bindings[27].num_inputs == 0);
        }
        TEST(Synth::num_mod_targets == 29);
    }
}

// Gating: string A (list index 5) grays no mode row and shows the four string rows (String
// Dispersion and Sustain Damping on the shared duty a / pad tone a rows, Release Damping with its
// block, and Pluck Brightness); the duty / pad tone targets have no separate row under string. Every
// other wave A grays the whole Release Damping / Pluck Brightness range 28..41 (and the shared rows per
// their sawtooth / pulse / pad use, with the duty row relabelled Pad Wander under pad); the B list
// offers no string.
static void check_string_wave_gating()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
    const uint32_t node_index = mapping.osc_nodes[0];
    TEST(node_index != Sculptor::pool_no_slot);
    if (node_index == Sculptor::pool_no_slot) {
        return;
    }

    // Lists: A ends with string, B does not offer it.
    {
        const Sculptor::Node& node = graph.node(node_index);
        TEST(node.slots.entries[1].num_list_options == 6);
        TEST(strcmp(node.slots.entries[1].list_options[5], "string") == 0);
        TEST(node.slots.entries[42].num_list_options == 6);
        for (uint32_t i = 0; i < node.slots.entries[42].num_list_options; ++i) {
            TEST(strcmp(node.slots.entries[42].list_options[i], "string") != 0);
        }
    }

    constexpr uint32_t wave_a_slot     = 1;
    constexpr uint32_t wave_b_slot     = 42;
    constexpr uint32_t mode_slot       = 69;
    constexpr uint32_t brightness_slot = Sculptor::osc_brightness_slot;

    Sculptor::PropertyValue value = {};
    value.list_index              = 1; // wave B sine so the mode rows are live
    graph.set_slot_value(node_index, wave_b_slot, value);
    value.list_index = 1; // fm mode
    graph.set_slot_value(node_index, mode_slot, value);

    bool           mode_rows_saw[5] = {};
    const uint32_t mode_rows[5]     = { mode_slot, 82, 95, 69, 42 };
    value.list_index                = 1; // sawtooth: reference for the mode rows
    graph.set_slot_value(node_index, wave_a_slot, value);
    for (uint32_t r = 0; r < 5; ++r) {
        mode_rows_saw[r] = Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), mode_rows[r]);
    }

    for (uint32_t list = 0; list < 6; ++list) {
        value.list_index = static_cast<uint8_t>(list);
        graph.set_slot_value(node_index, wave_a_slot, value);
        const bool is_string = list == 5;
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), brightness_slot) == ! is_string);
        // Rows 2..14 (Duty A, String Dispersion under string) are gated for sine and noise;
        // rows 15..27 (Pad Tone A, Sustain Damping under string) for all but
        // pad and string.
        for (uint32_t row = 2; row <= 27; ++row) {
            const bool gated = row <= 14 ? (list == 0 || list == 3) : (list != 4 && list != 5);
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == gated);
        }
        // Release Damping (28 with its block) and Pluck Brightness (41) exist only under string.
        for (uint32_t row = Sculptor::osc_string_extras_first_slot; row <= Sculptor::osc_brightness_slot; ++row) {
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), row) == ! is_string);
        }
        if (is_string) {
            // String A grays no mode row: the mode rows follow the saw reference.
            for (uint32_t r = 0; r < 5; ++r) {
                TEST(Sculptor::oscillator_slot_waveform_mode_disabled(graph.node(node_index), mode_rows[r]) ==
                     mode_rows_saw[r]);
            }
        }
    }
}

// Row reuse: under the string wave the Duty A / Pad Tone A rows (2 and 15) are
// relabelled String Dispersion / Sustain Damping (targets 28 / 26); Release Damping
// (target 27) owns row 28; all round-trip through compile; other waves keep Duty A / Pad Tone A and drop the string
// bindings from the graph.
static void check_string_row_reuse()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    instrument.layers[0].osc_type[0]                                           = Synth::WaveType::string_wave;
    instrument.layers[0].targets[Synth::mod_string_damping].base_value         = 0.3f;
    instrument.layers[0].targets[Synth::mod_string_dispersion].base_value      = 0.45f;
    instrument.layers[0].targets[Synth::mod_string_release_damping].base_value = 0.6f;
    TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
    const Sculptor::Node& node = graph.node(mapping.osc_nodes[0]);
    TEST(node.slots.num_allocated ==
         Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride * instrument.layers[0].fir_band_count);
    TEST(strcmp(node.slots.entries[2].name, "String Dispersion") == 0 &&
         approx(node.slots.entries[2].value.real, 0.45f, 1.0e-6f));
    TEST(strcmp(node.slots.entries[15].name, "Sustain Damping") == 0 &&
         approx(node.slots.entries[15].value.real, 0.3f, 1.0e-6f));
    TEST(strcmp(node.slots.entries[28].name, "Release Damping") == 0 &&
         approx(node.slots.entries[28].value.real, 0.6f, 1.0e-6f));
    TEST(strcmp(node.slots.entries[41].name, "Pluck Brightness") == 0);
    Synth::Instrument compiled = {};
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    TEST(approx(compiled.layers[0].targets[Synth::mod_string_damping].base_value, 0.3f, 1.0e-6f));
    TEST(approx(compiled.layers[0].targets[Synth::mod_string_release_damping].base_value, 0.6f, 1.0e-6f));
    TEST(approx(compiled.layers[0].targets[Synth::mod_string_dispersion].base_value, 0.45f, 1.0e-6f));
    TEST(compiled.layers[0].targets[Synth::mod_duty0].base_value == 0.0f);

    instrument.layers[0].osc_type[0]                          = Synth::WaveType::sawtooth_wave;
    instrument.layers[0].targets[Synth::mod_duty0].base_value = 0.7f;
    TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
    const Sculptor::Node& saw_node = graph.node(mapping.osc_nodes[0]);
    TEST(strcmp(saw_node.slots.entries[2].name, "Duty A") == 0 &&
         approx(saw_node.slots.entries[2].value.real, 0.7f, 1.0e-6f));
    TEST(strcmp(saw_node.slots.entries[15].name, "Pad Tone A") == 0);
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    TEST(approx(compiled.layers[0].targets[Synth::mod_duty0].base_value, 0.7f, 1.0e-6f));
    TEST(compiled.layers[0].targets[Synth::mod_string_damping].base_value == 0.0f);

    Sculptor::GraphChange change = {};
    change.kind                  = Sculptor::ChangeKind::value_changed;
    change.node_idx              = mapping.osc_nodes[0];
    change.slot_idx              = 1;
    TEST(Sculptor::osc_change_restructures(mapping, change));
    change.slot_idx = 42; // wave b renames the duty b row (Pad Wander under pad)
    TEST(Sculptor::osc_change_restructures(mapping, change));
    change.slot_idx = 69; // mix mode
    TEST(! Sculptor::osc_change_restructures(mapping, change));
}

// Pad Wander: the duty rows relabel under the pad wave (A and B) with a wander-depth tooltip;
// sawtooth and pulse keep Duty A / Duty B; the string wave carries String Dispersion on the
// duty a row.
static void check_pad_wander_labels()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    const uint32_t duty_a = Sculptor::osc_target_row(Synth::mod_duty0);
    const uint32_t duty_b = Sculptor::osc_target_row(Synth::mod_duty1);

    const Synth::WaveType waves[4] = { Synth::WaveType::sawtooth_wave,
                                       Synth::WaveType::pulse_wave,
                                       Synth::WaveType::pad_wave,
                                       Synth::WaveType::string_wave };
    for (const Synth::WaveType wave_a : waves) {
        for (const Synth::WaveType wave_b : waves) {
            if (wave_b == Synth::WaveType::string_wave) {
                continue; // the B list offers no string
            }
            instrument.layers[0].osc_type[0] = wave_a;
            instrument.layers[0].osc_type[1] = wave_b;
            TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
            const Sculptor::Node& node = graph.node(mapping.osc_nodes[0]);

            const Sculptor::Slot& slot_a = node.slots.entries[duty_a];
            if (wave_a == Synth::WaveType::pad_wave) {
                TEST(strcmp(slot_a.name, "Pad Wander") == 0);
                TEST(slot_a.tooltip && strstr(slot_a.tooltip, "wander depth") != nullptr);
            }
            else if (wave_a == Synth::WaveType::string_wave) {
                TEST(strcmp(slot_a.name, "String Dispersion") == 0);
            }
            else {
                TEST(strcmp(slot_a.name, "Duty A") == 0);
                TEST(slot_a.tooltip && strstr(slot_a.tooltip, "wander depth") == nullptr);
            }

            const Sculptor::Slot& slot_b = node.slots.entries[duty_b];
            if (wave_b == Synth::WaveType::pad_wave) {
                TEST(strcmp(slot_b.name, "Pad Wander") == 0);
                TEST(slot_b.tooltip && strstr(slot_b.tooltip, "wander depth") != nullptr);
            }
            else {
                TEST(strcmp(slot_b.name, "Duty B") == 0);
                TEST(slot_b.tooltip && strstr(slot_b.tooltip, "wander depth") == nullptr);
            }
        }
    }
}

// Release Damping (28 with its block) and Pluck Brightness (41) have no row unless wave a is
// the string wave: the render visibility callback hides them, not only grays them.
static void check_string_extras_hidden()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    const Synth::WaveType waves[6] = { Synth::WaveType::sine_wave,  Synth::WaveType::sawtooth_wave,
                                       Synth::WaveType::pulse_wave, Synth::WaveType::noise_wave,
                                       Synth::WaveType::pad_wave,   Synth::WaveType::string_wave };
    for (const Synth::WaveType wave : waves) {
        instrument.layers[0].osc_type[0] = wave;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        const uint32_t node_idx  = mapping.osc_nodes[0];
        const bool     is_string = wave == Synth::WaveType::string_wave;
        for (uint32_t slot = Sculptor::osc_string_extras_first_slot; slot <= Sculptor::osc_brightness_slot; ++slot) {
            const bool target_row = slot == Sculptor::osc_string_extras_first_slot;
            const bool plain_row  = slot == Sculptor::osc_brightness_slot;
            if (! is_string) {
                TEST(Sculptor::osc_node_slot_hidden(graph, node_idx, slot));
            }
            else if (target_row || plain_row) {
                TEST(! Sculptor::osc_node_slot_hidden(graph, node_idx, slot));
            }
        }
        // The rows before the extras and Waveform B after them stay shown (the shared duty a / pad tone a
        // rows are never hidden; Waveform B is not a string row).
        TEST(! Sculptor::osc_node_slot_hidden(graph, node_idx, 2));
        TEST(! Sculptor::osc_node_slot_hidden(graph, node_idx, 15));
        TEST(! Sculptor::osc_node_slot_hidden(graph, node_idx, 42));
    }
}

// Decoder identity pin (decode_osc_input driven directly: no end-to-end path
// observes the decoded ModTarget). A string-wave layer decodes its duty a /
// pad tone a rows and blocks to String Dispersion / Sustain Damping; a
// non-string layer decodes the same slots to Duty A / Pad Tone A. The
// string layer carries wired dispersion routing.
static void check_string_decoder_target_identity()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    instrument.layers[0].osc_type[0]                                            = Synth::WaveType::string_wave;
    instrument.layers[0].targets[Synth::mod_string_dispersion].envelope_desc_id = 1;
    instrument.layers[1].osc_type[0]                                            = Synth::WaveType::sawtooth_wave;
    bank.bank.instruments.entries[0]                                            = instrument;
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));

    const uint32_t string_node = mapping.osc_nodes[0];
    const uint32_t plain_node  = mapping.osc_nodes[1];
    TEST(
        find_osc_graph_connection(graph, { string_node, Sculptor::osc_target_env_dot(Synth::mod_string_dispersion) }) !=
        Sculptor::pool_no_slot);

    const uint32_t shared_targets[2] = { Synth::mod_duty0, Synth::mod_pad_tilt0 };
    const uint32_t string_targets[2] = { Synth::mod_string_dispersion, Synth::mod_string_damping };
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t row   = Sculptor::osc_target_row(shared_targets[i]);
        const uint32_t block = Sculptor::osc_target_env_dot(shared_targets[i]);
        for (uint32_t layer = 0; layer < 2; ++layer) {
            const uint32_t node     = layer == 0 ? string_node : plain_node;
            const uint32_t expected = layer == 0 ? string_targets[i] : shared_targets[i];
            bool           base_row = false;
            uint32_t       target   = 0xFFFFu;
            uint32_t       family   = 0xFFFFu;
            uint32_t       dot      = 0xFFFFu;
            // Base value row.
            TEST(Sculptor::decode_osc_input(mapping, graph, { node, row }, &base_row, &target, &family, &dot));
            TEST(base_row && target == expected && dot == Sculptor::pool_no_slot);
            // Block envelope dot.
            base_row = true;
            TEST(Sculptor::decode_osc_input(mapping, graph, { node, block }, &base_row, &target, &family, &dot));
            TEST(! base_row && target == expected && dot == block);
        }
    }
}

// Deleting an oscillator that has incoming modulation wires queues its
// wire-deletion events against the already-freed node. The deletion decoder
// must not read the freed node (it asserted/crashed) and decodes the freed
// endpoint with the default Duty A semantics; the drained batch applies.
static void check_osc_node_delete_with_wires_decodes()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    instrument.layers[0].osc_type[0]                                            = Synth::WaveType::string_wave;
    instrument.layers[0].targets[Synth::mod_string_dispersion].envelope_desc_id = 1;
    bank.bank.instruments.entries[0]                                            = instrument;
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));

    Sculptor::GraphChange changes[Sculptor::max_pending_changes];
    (void)graph.take_changes(changes, Sculptor::max_pending_changes); // drop the projection traffic

    const uint32_t deleted = mapping.osc_nodes[0];
    graph.delete_node(deleted);
    TEST(! graph.node_occupied(deleted));
    const uint32_t count = graph.take_changes(changes, Sculptor::max_pending_changes);

    uint32_t wire_drops = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (changes[i].kind == Sculptor::ChangeKind::connection_deleted &&
            changes[i].connection_input.node_idx == deleted) {
            ++wire_drops;
            bool     base_row = true;
            uint32_t target   = 0xFFFFu;
            uint32_t family   = 0xFFFFu;
            uint32_t dot      = 0xFFFFu;
            if (changes[i].connection_input.slot_idx == Sculptor::osc_target_env_dot(Synth::mod_duty0)) {
                TEST(Sculptor::decode_osc_input(mapping,
                                                graph,
                                                changes[i].connection_input,
                                                &base_row,
                                                &target,
                                                &family,
                                                &dot));
                TEST(! base_row && target == Synth::mod_duty0); // default semantics, not dispersion
            }
        }
        TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changes[i], 0, 0));
    }
    TEST(wire_drops >= 2); // the dispersion envelope wire and the layer's other incoming wires
}

// Live Waveform A switch (any wave to any wave, shared and string-only rows routed, pad tone
// negative): the committed bank must still save and reload.  Switching to string used to compile
// the Pad Tone row's negative value into Sustain Damping (0..1 in the bank), so the auto-save's
// reload check refused the bank (save returns EINVAL, which the editor reports as
// "Synth: cannot write %s: %s" with strerror(EINVAL)).
static void check_wave_switch_bank_saves()
{
    const char* const path = "synth_wave_switch_bank.tmp";

    const uint32_t wave_a_slot = 1; // waveform a, layout contract

    static const Synth::WaveType  waves[]   = { Synth::WaveType::sine_wave,  Synth::WaveType::sawtooth_wave,
                                                Synth::WaveType::pulse_wave, Synth::WaveType::noise_wave,
                                                Synth::WaveType::pad_wave,   Synth::WaveType::string_wave };
    static const Synth::WaveType  waves_b[] = { Synth::WaveType::no_wave,
                                                Synth::WaveType::sawtooth_wave,
                                                Synth::WaveType::pad_wave };
    static const Synth::ModTarget routed[]  = { Synth::mod_duty0,
                                                Synth::mod_pad_tilt0,
                                                Synth::mod_string_damping,
                                                Synth::mod_string_release_damping,
                                                Synth::mod_string_dispersion };
    // Negative pad_tilt0 bases must survive a switch to string_wave.
    const auto check_switch = [&](Synth::WaveType  start,
                                  Synth::WaveType  end,
                                  Synth::WaveType  wave_b,
                                  Synth::ModTarget target,
                                  uint32_t         base_slot) {
        const float base = base_slot == 0 ? 0.4f : target == Synth::mod_pad_tilt0 ? -0.5f : 0.9f;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);

        instrument.layers[0].osc_type[0] = start;
        instrument.layers[0].osc_type[1] = wave_b;
        Synth::TargetBinding& binding    = instrument.layers[0].targets[target];
        binding.base_value               = base;
        binding.envelope_desc_id         = 1;
        binding.lfo_desc_id              = 1;
        binding.lfo_op                   = Synth::SourceOp::add;
        binding.lfo_depth                = 0.5f;
        binding.num_inputs               = 1;
        binding.inputs[0].source         = Synth::ModSource::velocity;
        binding.inputs[0].op             = Synth::SourceOp::add;
        binding.inputs[0].scale          = 0.5f;
        bank.bank.instruments.entries[0] = instrument;

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
        Sculptor::GraphChange drained[Sculptor::max_pending_changes] = {};
        (void)graph.take_changes(drained, Sculptor::max_pending_changes);

        Sculptor::PropertyValue wave = {};
        wave.list_index              = static_cast<uint8_t>(static_cast<uint32_t>(end) - 1u);
        TEST(apply_m2_slot_edit(&bank, &graph, &mapping, mapping.osc_nodes[0], wave_a_slot, wave));
        TEST(reproject_m2_band_graph(&bank, &graph, &mapping));
        TEST(Synth::save_editor_bank_file(path, &bank) == 0);

        const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        TEST(Synth::load_editor_bank_file(path, loaded_storage.get()) == Synth::BankFileStatus::ok);
        remove(path);
    };
    // Part A: 60 rows ending in string, including every start/target/base combination.
    for (uint32_t start = 0; start < 6; ++start) {
        for (uint32_t target = 0; target < 5; ++target) {
            for (uint32_t base_slot = 0; base_slot < 2; ++base_slot) {
                check_switch(waves[start],
                             Synth::WaveType::string_wave,
                             waves_b[(start + target + base_slot) % 3],
                             routed[target],
                             base_slot);
            }
        }
    }
    // Part B: 30 rows cover all non-string ends; together the parts cover all 36 wave pairs.
    for (uint32_t start = 0; start < 6; ++start) {
        for (uint32_t end = 0; end < 5; ++end) {
            check_switch(waves[start], waves[end], waves_b[(start + end) % 3], routed[(start + end) % 5], start % 2);
        }
    }
}

static void check_string_trap_regressions()
{

    if (Synth::num_mod_targets > 27) {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        Synth::Oscillator& layer = bank.bank.instruments.entries[0].layers[0];
        layer.osc_type[0]        = Synth::WaveType::string_wave;
        layer.fir_band_count     = 1;
        for (uint32_t t = 26; t < 28; ++t) {
            Synth::TargetBinding* const binding = &layer.targets[0] + t;
            binding->base_value                 = 0.5f;
            binding->lfo_desc_id                = 1;
            binding->lfo_op                     = Synth::SourceOp::add;
            binding->lfo_depth                  = 0.25f;
        }
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&payload)[Synth::library_payload_max] = payload_storage->elements;
        const uint32_t length                       = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
        TEST(length > 0);
        TEST(strstr(payload, "\"string_damping\"") != nullptr);
        TEST(strstr(payload, "\"string_release_damping\"") != nullptr);
    }
}

// Pluck brightness: an optional plain layer scalar in [0,1]; absent decodes as 1 (the unfiltered
// burst), out of range or NaN refuses, and it survives encode, decode, graph projection and compile.
static void check_string_brightness()
{
    const std::unique_ptr<ClipboardDecode> storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded = *storage;
    char                                   doc[256];

    // Absent key: default full brightness.
    static const char absent_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\"}]}";
    TEST(string_clip_decode(absent_doc, &decoded));
    TEST(decoded.instr.layers[0].string_brightness == 1.0f);
    TEST(Synth::Oscillator{}.string_brightness == 1.0f);

    static const char* const accepted[]       = { "0", "0.25", "1" };
    static const float       accepted_value[] = { 0.0f, 0.25f, 1.0f };
    for (uint32_t i = 0; i < 3; ++i) {
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"string_brightness\":%s}]}",
                 accepted[i]);
        TEST(string_clip_decode(doc, &decoded));
        TEST(decoded.instr.layers[0].string_brightness == accepted_value[i]);
    }
    static const char* const refused[] = { "-0.1", "1.1", "NaN" };
    for (const char* value : refused) {
        snprintf(doc,
                 sizeof(doc),
                 "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"string_brightness\":%s}]}",
                 value);
        TEST(! string_clip_decode(doc, &decoded));
    }
    // Unknown keys still refuse; the bank decoder takes the key and refuses out of range too.
    static const char bank_ok[] =
        "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
        "\"string_brightness\":0.5,\"routing\":{}}]}],"
        "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
    static const char bank_absent[] =
        "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
        "\"routing\":{}}]}],"
        "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
    static const char bank_bad[] =
        "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
        "\"string_brightness\":1.5,\"routing\":{}}]}],"
        "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
    TEST(Synth::decode_editor_bank_json(bank_ok, static_cast<uint32_t>(strlen(bank_ok)), bank_storage.get()));
    TEST(bank_storage->bank.instruments.entries[0].layers[0].string_brightness == 0.5f);
    memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
    TEST(Synth::decode_editor_bank_json(bank_absent, static_cast<uint32_t>(strlen(bank_absent)), bank_storage.get()));
    TEST(bank_storage->bank.instruments.entries[0].layers[0].string_brightness == 1.0f);
    memset(static_cast<void*>(bank_storage.get()), 0, sizeof(*bank_storage));
    TEST(! Synth::decode_editor_bank_json(bank_bad, static_cast<uint32_t>(strlen(bank_bad)), bank_storage.get()));

    // Validator: bounds accepted, out of range and NaN refused.
    const std::unique_ptr<Synth::InstrumentBank> vstorage    = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       vbank       = *vstorage;
    static const float                           ok_values[] = { 0.0f, 0.5f, 1.0f };
    for (const float value : ok_values) {
        string_make_bank(vbank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
        vbank.instruments.entries[0].layers[0].string_brightness = value;
        TEST(Synth::validate_instrument_bank(&vbank));
    }
    static const float bad_values[] = { -0.1f, 1.1f, NAN };
    for (const float value : bad_values) {
        string_make_bank(vbank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
        vbank.instruments.entries[0].layers[0].string_brightness = value;
        TEST(! Synth::validate_instrument_bank(&vbank));
    }

    // Clipboard encode: the default omits the key, a lowered value round-trips.
    const std::unique_ptr<Synth::InstrumentEditorBank> ebank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       ebank         = *ebank_storage;
    memset(static_cast<void*>(&ebank), 0, sizeof(ebank));
    Synth::Instrument instr = {};
    instr.layer_count       = 1;
    set_default_dispersion_sections(instr);
    instr.layers[0].osc_type[0] = Synth::WaveType::string_wave;
    const uint32_t slot         = ebank.bank.instruments.allocate();
    TEST(slot != pool_no_slot);
    ebank.bank.instruments.entries[slot] = instr;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    uint32_t length                  = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0);
    TEST(strstr(text, "string_brightness") == nullptr);
    TEST(string_clip_decode(text, &decoded));
    TEST(decoded.instr.layers[0].string_brightness == 1.0f);

    instr.layers[0].string_brightness = 0.375f;
    length                            = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0);
    TEST(strstr(text, "\"string_brightness\":0.375") != nullptr);
    TEST(string_clip_decode(text, &decoded));
    TEST(decoded.instr.layers[0].string_brightness == 0.375f);

    // Editor bank round trip, then graph projection and compile keep the value.
    const std::unique_ptr<Synth::InstrumentEditorBank> src_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       src         = *src_storage;
    Synth::Instrument                                  fixture     = {};
    build_binding_fixture(&src, &fixture);
    src.bank.instruments.entries[0].layers[0].osc_type[0]       = Synth::WaveType::string_wave;
    src.bank.instruments.entries[0].layers[0].fir_band_count    = 1;
    src.bank.instruments.entries[0].layers[0].string_brightness = 0.375f;
    TEST(Synth::validate_instrument_bank(&src.bank));
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t payload_length               = Synth::encode_editor_bank_json(&src, payload, sizeof(payload));
    TEST(payload_length > 0);
    TEST(strstr(payload, "\"string_brightness\"") != nullptr);
    const std::unique_ptr<Synth::InstrumentEditorBank> dst_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       dst         = *dst_storage;
    memset(static_cast<void*>(&dst), 0, sizeof(dst));
    TEST(Synth::decode_editor_bank_json(payload, payload_length, &dst));
    const Synth::Oscillator& dec = dst.bank.instruments.entries[dst.bank.channel_zones[0][0].instrument].layers[0];
    TEST(dec.string_brightness == 0.375f);

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(dst, &graph, &mapping, 0, 0));
    const Sculptor::Node& node = graph.node(mapping.osc_nodes[0]);
    TEST(strcmp(node.slots.entries[Sculptor::osc_brightness_slot].name, "Pluck Brightness") == 0);
    TEST(node.slots.entries[Sculptor::osc_brightness_slot].value.real == 0.375f);
    TEST(node.slots.entries[Sculptor::osc_brightness_slot].child_row);
    Synth::Instrument compiled = {};
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    TEST(compiled.layers[0].string_brightness == 0.375f);
}

// Builds a one-layer clipboard document carrying one extra layer key; returns the document in buf.
static const char* string_disp_clip_doc(char (&buf)[320], const char* key, const char* value)
{
    snprintf(buf,
             sizeof(buf),
             "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\",\"%s\":%s}]}",
             key,
             value);
    return buf;
}

// Same for the editor bank document.
static const char* string_disp_bank_doc(char (&buf)[512], const char* key, const char* value)
{
    snprintf(buf,
             sizeof(buf),
             "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
             "\"%s\":%s,\"routing\":{}}]}],"
             "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}",
             key,
             value);
    return buf;
}

// String dispersion and its section count: optional plain layer keys. Absent decodes as 0 and 4; dispersion
// must be a finite number in [0,1] and sections an integer in [1,6]; both survive encode, decode, graph
// projection and compile, and the validator refuses bad values whatever the other field holds.
static void check_string_dispersion_codec()
{
    const std::unique_ptr<ClipboardDecode> storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded = *storage;
    char                                   clip_buf[320];
    char                                   bank_buf[512];

    // Absent keys: defaults 0 and 4.
    static const char absent_doc[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\"}]}";
    TEST(string_clip_decode(absent_doc, &decoded));
    TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.0f);
    TEST(decoded.instr.layers[0].string_dispersion_sections == 4.0f);
    TEST(Synth::Oscillator{}.targets[Synth::mod_string_dispersion].base_value == 0.0f);
    TEST(Synth::Oscillator{}.string_dispersion_sections == 4.0f);

    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    const std::unique_ptr<Synth::InstrumentEditorBank> keep_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       decoded_bank = *bank_storage;
    memset(static_cast<void*>(&decoded_bank), 0, sizeof(decoded_bank));
    static const char bank_absent[] =
        "{\"instrument_editor_bank\":{\"instruments\":[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"string\","
        "\"routing\":{}}]}],\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
    TEST(Synth::decode_editor_bank_json(bank_absent, static_cast<uint32_t>(strlen(bank_absent)), &decoded_bank));
    TEST(decoded_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value == 0.0f);
    TEST(decoded_bank.bank.instruments.entries[0].layers[0].string_dispersion_sections == 4.0f);

    // Accepted values, through both decoders: dispersion bounds 0 and 1, sections bounds 1 and 6.
    static const char* const disp_ok[]       = { "0", "0.25", "1" };
    static const float       disp_ok_value[] = { 0.0f, 0.25f, 1.0f };
    static const char* const sect_ok[]       = { "1", "4", "6" };
    static const float       sect_ok_value[] = { 1.0f, 4.0f, 6.0f };
    for (uint32_t i = 0; i < 3; ++i) {
        TEST(string_clip_decode(string_disp_clip_doc(clip_buf, "string_dispersion", disp_ok[i]), &decoded));
        TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == disp_ok_value[i]);
        TEST(decoded.instr.layers[0].string_dispersion_sections == 4.0f);
        TEST(string_clip_decode(string_disp_clip_doc(clip_buf, "string_dispersion_sections", sect_ok[i]), &decoded));
        TEST(decoded.instr.layers[0].string_dispersion_sections == sect_ok_value[i]);
        TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.0f);
        const char* const bd = string_disp_bank_doc(bank_buf, "string_dispersion", disp_ok[i]);
        TEST(Synth::decode_editor_bank_json(bd, static_cast<uint32_t>(strlen(bd)), &decoded_bank));
        TEST(decoded_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value ==
             disp_ok_value[i]);
        const char* const bs = string_disp_bank_doc(bank_buf, "string_dispersion_sections", sect_ok[i]);
        TEST(Synth::decode_editor_bank_json(bs, static_cast<uint32_t>(strlen(bs)), &decoded_bank));
        TEST(decoded_bank.bank.instruments.entries[0].layers[0].string_dispersion_sections == sect_ok_value[i]);
    }

    // Refused values, through both decoders; each failed bank decode leaves the destination unchanged.
    const char* const bank_good = string_disp_bank_doc(bank_buf, "string_dispersion", "0.5");
    TEST(Synth::decode_editor_bank_json(bank_good, static_cast<uint32_t>(strlen(bank_good)), &decoded_bank));
    memcpy(static_cast<void*>(keep_storage.get()), &decoded_bank, sizeof(decoded_bank));
    static const char* const disp_bad[] = { "-0.1",      "1.1",     "2",    "NaN",  "Infinity",
                                            "-Infinity", "\"0.5\"", "true", "null", "[0.5]" };
    static const char* const sect_bad[] = { "0",   "-1",       "7",     "2.5",  "4.0000001",
                                            "NaN", "Infinity", "\"4\"", "true", "null" };
    for (const char* value : disp_bad) {
        TEST(! string_clip_decode(string_disp_clip_doc(clip_buf, "string_dispersion", value), &decoded));
        const char* const bd = string_disp_bank_doc(bank_buf, "string_dispersion", value);
        TEST(! Synth::decode_editor_bank_json(bd, static_cast<uint32_t>(strlen(bd)), &decoded_bank));
        TEST(memcmp(&decoded_bank, keep_storage.get(), sizeof(decoded_bank)) == 0);
    }
    for (const char* value : sect_bad) {
        TEST(! string_clip_decode(string_disp_clip_doc(clip_buf, "string_dispersion_sections", value), &decoded));
        const char* const bs = string_disp_bank_doc(bank_buf, "string_dispersion_sections", value);
        TEST(! Synth::decode_editor_bank_json(bs, static_cast<uint32_t>(strlen(bs)), &decoded_bank));
        TEST(memcmp(&decoded_bank, keep_storage.get(), sizeof(decoded_bank)) == 0);
    }
    // A refused clipboard decode never reports success after a valid sibling key.
    static const char both_bad[] = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"string\","
                                   "\"string_dispersion\":0.5,\"string_dispersion_sections\":9}]}";
    TEST(! string_clip_decode(both_bad, &decoded));

    // Negative zero spelling decodes as zero; the editor bank decoder canonicalizes it to positive zero.
    TEST(string_clip_decode(string_disp_clip_doc(clip_buf, "string_dispersion", "-0"), &decoded));
    TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.0f);
    const char* const bneg = string_disp_bank_doc(bank_buf, "string_dispersion", "-0.0");
    TEST(Synth::decode_editor_bank_json(bneg, static_cast<uint32_t>(strlen(bneg)), &decoded_bank));
    TEST(! std::signbit(
        decoded_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value));

    // Validator: unconditional contract, independent of the other field.
    const std::unique_ptr<Synth::InstrumentBank> vstorage       = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       vbank          = *vstorage;
    static const float                           bad_sections[] = { 0.0f, -1.0f, 7.0f, 2.5f, NAN, INFINITY };
    static const float                           bad_disp[]     = { -0.1f, 1.1f, 2.0f, NAN, INFINITY };
    static const float                           other_disp[]   = { 0.0f, 0.5f };
    static const float                           other_sect[]   = { 1.0f, 4.0f, 6.0f };
    for (const float other : other_disp) {
        for (const float value : bad_sections) {
            string_make_bank(vbank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
            vbank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = other;
            vbank.instruments.entries[0].layers[0].string_dispersion_sections                       = value;
            TEST(! Synth::validate_instrument_bank(&vbank));
        }
    }
    for (const float other : other_sect) {
        for (const float value : bad_disp) {
            string_make_bank(vbank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
            vbank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = value;
            vbank.instruments.entries[0].layers[0].string_dispersion_sections                       = other;
            TEST(! Synth::validate_instrument_bank(&vbank));
        }
    }
    for (uint32_t i = 0; i < 3; ++i) {
        string_make_bank(vbank, Synth::WaveType::string_wave, Synth::WaveType::no_wave);
        vbank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = disp_ok_value[i];
        vbank.instruments.entries[0].layers[0].string_dispersion_sections                       = sect_ok_value[i];
        TEST(Synth::validate_instrument_bank(&vbank));
    }

    // Clipboard encode: defaults are omitted, valid non-defaults are emitted and round-trip, values the decoder
    // would refuse stay out of the document.
    const std::unique_ptr<Synth::InstrumentEditorBank> ebank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       ebank         = *ebank_storage;
    memset(static_cast<void*>(&ebank), 0, sizeof(ebank));
    Synth::Instrument instr = {};
    instr.layer_count       = 1;
    set_default_dispersion_sections(instr);
    instr.layers[0].osc_type[0] = Synth::WaveType::string_wave;
    const uint32_t slot         = ebank.bank.instruments.allocate();
    TEST(slot != pool_no_slot);
    ebank.bank.instruments.entries[slot] = instr;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    uint32_t length                  = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0);
    TEST(strstr(text, "string_dispersion") == nullptr);
    TEST(string_clip_decode(text, &decoded));
    TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.0f &&
         decoded.instr.layers[0].string_dispersion_sections == 4.0f);

    instr.layers[0].targets[Synth::mod_string_dispersion].base_value = -0.0f;
    length = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0 && strstr(text, "string_dispersion") == nullptr);

    instr.layers[0].targets[Synth::mod_string_dispersion].base_value = 0.375f;
    length = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0);
    TEST(strstr(text, "\"string_dispersion\":0.375") != nullptr);
    TEST(strstr(text, "string_dispersion_sections") == nullptr);
    TEST(string_clip_decode(text, &decoded));
    TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.375f &&
         decoded.instr.layers[0].string_dispersion_sections == 4.0f);

    instr.layers[0].string_dispersion_sections = 2.0f;
    length                                     = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length > 0);
    TEST(strstr(text, "\"string_dispersion_sections\":2") != nullptr);
    TEST(string_clip_decode(text, &decoded));
    TEST(decoded.instr.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.375f &&
         decoded.instr.layers[0].string_dispersion_sections == 2.0f);

    static const float unencodable[] = { 0.0f, 7.0f, 2.5f, NAN };
    for (const float value : unencodable) {
        instr.layers[0].targets[Synth::mod_string_dispersion].base_value = 0.0f;
        instr.layers[0].string_dispersion_sections                       = value;
        length = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
        TEST(length == 0 || strstr(text, "string_dispersion_sections") == nullptr);
    }
    instr.layers[0].string_dispersion_sections                       = 4.0f;
    instr.layers[0].targets[Synth::mod_string_dispersion].base_value = NAN;
    length = Synth::encode_instrument_json(text, sizeof(text), &instr, &ebank.bank);
    TEST(length == 0 || strstr(text, "string_dispersion") == nullptr);

    // Editor bank round trip, then graph projection and compile keep both values.
    const std::unique_ptr<Synth::InstrumentEditorBank> src_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       src         = *src_storage;
    Synth::Instrument                                  fixture     = {};
    build_binding_fixture(&src, &fixture);
    src.bank.instruments.entries[0].layers[0].osc_type[0]    = Synth::WaveType::string_wave;
    src.bank.instruments.entries[0].layers[0].fir_band_count = 1;
    src.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = 0.625f;
    src.bank.instruments.entries[0].layers[0].string_dispersion_sections                       = 3.0f;
    TEST(Synth::validate_instrument_bank(&src.bank));
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t payload_length               = Synth::encode_editor_bank_json(&src, payload, sizeof(payload));
    TEST(payload_length > 0);
    TEST(strstr(payload, "\"string_dispersion\":0.625") != nullptr);
    TEST(strstr(payload, "\"string_dispersion_sections\":3") != nullptr);
    const std::unique_ptr<Synth::InstrumentEditorBank> dst_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       dst         = *dst_storage;
    memset(static_cast<void*>(&dst), 0, sizeof(dst));
    TEST(Synth::decode_editor_bank_json(payload, payload_length, &dst));
    const Synth::Oscillator& dec = dst.bank.instruments.entries[dst.bank.channel_zones[0][0].instrument].layers[0];
    TEST(dec.targets[Synth::mod_string_dispersion].base_value == 0.625f && dec.string_dispersion_sections == 3.0f);

    // Negative zero in an editor bank encodes without the key and decodes as positive zero; defaults omit both keys.
    src.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = -0.0f;
    src.bank.instruments.entries[0].layers[0].string_dispersion_sections                       = 4.0f;
    const uint32_t plain_length = Synth::encode_editor_bank_json(&src, payload, sizeof(payload));
    TEST(plain_length > 0 && strstr(payload, "string_dispersion") == nullptr);
    memset(static_cast<void*>(&dst), 0, sizeof(dst));
    TEST(Synth::decode_editor_bank_json(payload, plain_length, &dst));
    TEST(! std::signbit(dst.bank.instruments.entries[dst.bank.channel_zones[0][0].instrument]
                            .layers[0]
                            .targets[Synth::mod_string_dispersion]
                            .base_value));
    src.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = 0.625f;
    src.bank.instruments.entries[0].layers[0].string_dispersion_sections                       = 3.0f;
    const uint32_t full_length = Synth::encode_editor_bank_json(&src, payload, sizeof(payload));
    memset(static_cast<void*>(&dst), 0, sizeof(dst));
    TEST(Synth::decode_editor_bank_json(payload, full_length, &dst));

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(dst, &graph, &mapping, 0, 0));
    const Sculptor::Node& node = graph.node(mapping.osc_nodes[0]);
    TEST(node.slots.entries[Sculptor::osc_target_row_slot[Synth::mod_string_dispersion]].value.real == 0.625f);
    Synth::Instrument compiled = {};
    TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    TEST(compiled.layers[0].targets[Synth::mod_string_dispersion].base_value == 0.625f);
    TEST(compiled.layers[0].string_dispersion_sections == 4.0f);
}

static void string_wave_m2_regressions()
{
    check_string_wave_json_codec();
    check_string_wave_validator();
    check_string_param_json_bounds();
    check_string_brightness();
    check_string_dispersion_codec();
    check_string_wave_gating();
    check_string_row_reuse();
    check_pad_wander_labels();
    check_string_extras_hidden();
    check_string_decoder_target_identity();
    check_osc_node_delete_with_wires_decodes();
    check_wave_switch_bank_saves();

    check_string_trap_regressions();
}

// ---- String DSP: float32 CPU mirror of the lane-0 serial loop (executable spec).
// Evidence ceiling: pins the arithmetic and design logic, not GPU equivalence.
static uint32_t string_pcg_hash(uint32_t state)
{
    state               = state * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Host-side clamp of the resolved damping destinations (realtime_synth.cpp unit_range_clamp).
static float string_unit_clamp(float value)
{
    return value >= 0.0f ? (value <= 1.0f ? value : 1.0f) : 0.0f;
}

// Per-step scalar block, evaluated once per dispatch in float32.
// Dispersion helpers, same closed forms and float operation order as the shader.
static float string_disp_ratio(float q)
{
    if (q <= 0.3849002f) {
        const float cos_arg = fminf(1.0f, 2.598076f * q);
        return 1.1547005f * cosf(acosf(cos_arg) * 0.33333334f);
    }
    const float cardano = powf(q * 0.5f + sqrtf(q * q * 0.25f - 0.037037037f), 0.33333334f);
    return cardano + 0.33333334f / cardano;
}

static float string_disp_phase_delay(float coeff, float omega)
{
    return (2.0f / omega) * atan2f((1.0f - coeff) * sinf(0.5f * omega), (1.0f + coeff) * cosf(0.5f * omega));
}

// Test-only mutation selector for check_string_mirror_dispersion: 0 = the real mirror, 1 = a_disp sign flip,
// 2 = retune term omitted, 3 = corrupted TDF-II state update, 4 = retired (a shared state word is a no-op with one
// section), 5 = f0 compensation dropped, 6 = divides by the stored section count instead of the constant 1.
static int string_disp_mutation = 0;

// Test-only stand-in for the stored (inert) section count, read only by mutation 6.
static float string_test_stored_sections = 1.0f;

// Dispersion strength, mirroring string_dispersion_strength in synth_oscillator.comp.glsl.
static constexpr float string_dispersion_strength = 0.05863f;

static StringScalars string_mirror_scalars(float f0, float damping, float release_damping, float dispersion = 0.0f)
{
    StringScalars s = {};
    s.silent        = ! (f0 >= 22.0f);
    if (s.silent) {
        return s;
    }
    damping           = string_unit_clamp(damping);
    float len         = string_fs / f0;
    len               = len < 3.0f ? 3.0f : len;
    s.c               = 0.10f + 0.30f * damping;
    const float tau   = s.c / (1.0f - s.c);
    const float t60   = 20.0f * powf(0.25f / 20.0f, damping);
    const float omega = 6.2831853f * f0 / string_fs;
    const float mag   = (1.0f - s.c) / sqrtf(1.0f - 2.0f * s.c * cosf(omega) + s.c * s.c);
    const float g     = powf(10.0f, -3.0f * len / (string_fs * t60)) / mag;
    s.g               = g < 0.9999f ? g : 0.9999f;
    float disp_delay  = 0.0f;
    if (dispersion > 0.0f) {
        const float omega_q = string_disp_mutation == 5 ? 6.2831853f * 220.0f / string_fs : omega;
        // The section count is frozen at one.  Mutation 6 divides by the stored count instead.
        const float section_count = string_disp_mutation == 6 ? string_test_stored_sections : 1.0f;
        const float disp_q = string_dispersion_strength * dispersion / (section_count * (omega_q * omega_q * omega_q));
        const float disp_r = string_disp_ratio(disp_q);
        const float section_a     = (1.0f - disp_r) / (1.0f + disp_r);
        const float coeff         = string_disp_mutation == 1 ? -section_a : section_a;
        const float section_delay = string_disp_phase_delay(coeff, omega);
        if (len - tau - section_delay >= 3.0f) {
            s.disp_a       = coeff;
            s.num_sections = 1;
            disp_delay     = section_delay;
        }
    }
    s.disp_delay      = string_disp_mutation == 2 ? 0.0f : disp_delay;
    const float delay = len - tau - s.disp_delay;
    s.n               = static_cast<uint32_t>(floorf(delay - 1.0f));
    const float d     = delay - static_cast<float>(s.n);
    s.a               = (1.0f - d) / (1.0f + d);
    s.gr              = powf(10.0f, -release_damping * 3.0f * len / (string_fs * 0.05f));
    return s;
}

// Note-on burst: noise, mean pass, then the optional circular one-pole tone stage (brightness < 1; 1 keeps
// the legacy excitation bit-identical).  Built into a scratch array, as the shader's shared burst array does.
// Returns the delay length n (0 when the pitch is below the floor, nothing written).
static uint32_t string_mirror_burst(float*   burst,
                                    float    f0,
                                    float    damping,
                                    uint32_t seed,
                                    float    brightness = 1.0f,
                                    float    dispersion = 0.0f)
{
    const StringScalars s = string_mirror_scalars(f0, damping, 0.0f, dispersion);
    if (s.silent) {
        return 0;
    }
    const uint32_t n = s.n;
    for (uint32_t i = 0; i < n; ++i) {
        burst[i] = static_cast<float>(string_pcg_hash(seed + i) & 0xFFFFu) / 32767.5f - 1.0f;
    }
    float sum = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        sum += burst[k];
    }
    const float mean = sum / static_cast<float>(n);
    for (uint32_t k = 0; k < n; ++k) {
        burst[k] -= mean;
    }
    if (brightness < 1.0f) {
        // Same math as the shader: a zero-state lap gives the tail, which fixes the periodic
        // wrap start state; then the filtered lap, re-centering and energy-matched rescale.
        const float tone_coeff = 0.95f * (1.0f - string_unit_clamp(brightness));
        float       tone_state = 0.0f;
        float       energy_in  = 0.0f;
        for (uint32_t k = 0; k < n; ++k) {
            const float sample_in = burst[k];
            tone_state            = (1.0f - tone_coeff) * sample_in + tone_coeff * tone_state;
            energy_in += sample_in * sample_in;
        }
        tone_state /= 1.0f - powf(tone_coeff, static_cast<float>(n));
        float tone_sum    = 0.0f;
        float tone_energy = 0.0f;
        for (uint32_t k = 0; k < n; ++k) {
            tone_state = (1.0f - tone_coeff) * burst[k] + tone_coeff * tone_state;
            burst[k]   = tone_state;
            tone_sum += tone_state;
            tone_energy += tone_state * tone_state;
        }
        const float tone_mean = tone_sum / static_cast<float>(n);
        const float tone_var  = tone_energy - tone_sum * tone_mean;
        const float tone_gain = tone_var > 0.0f ? sqrtf(energy_in / tone_var) : 1.0f;
        for (uint32_t k = 0; k < n; ++k) {
            burst[k] = (burst[k] - tone_mean) * tone_gain;
        }
    }
    return n;
}

// Replace-style note-on init (the legacy behavior): the burst becomes the ring head and all running state
// restarts.  The chain token words are left alone.
static void string_mirror_init(StringMirror* m,
                               float         f0,
                               float         damping,
                               uint32_t      seed,
                               float         brightness = 1.0f,
                               float         dispersion = 0.0f)
{
    static float   burst[string_ring_size];
    const uint32_t n = string_mirror_burst(burst, f0, damping, seed, brightness, dispersion);
    if (n == 0) {
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        m->ring[i] = burst[i];
    }
    m->wr = n;
    m->t1 = 0.0f;
    m->u1 = 0.0f;
    m->e  = 1.0f;
    for (uint32_t k = 0; k < string_max_sections; ++k) {
        m->ds[k] = 0.0f;
    }
    m->tagged = true;
    m->tag    = seed;
}

// One dispatch of `count` samples (the shader uses 256).
static void string_mirror_step(StringMirror* m,
                               float         f0,
                               float         damping,
                               float         release_damping,
                               bool          release,
                               float*        out,
                               uint32_t      count,
                               float         dispersion = 0.0f)
{
    const StringScalars s = string_mirror_scalars(f0, damping, release_damping, dispersion);
    if (s.silent) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = 0.0f;
        }
        return;
    }
    const float kr     = 1.0f - expf(-1.0f / (string_fs * 0.003f));
    const float target = release ? s.gr : 1.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t rd = m->wr >= s.n ? m->wr - s.n : m->wr + string_ring_size - s.n;
        const float    x  = m->ring[rd];
        out[i]            = x;
        m->e += kr * (target - m->e);
        const float t       = (1.0f - s.c) * x + s.c * m->t1;
        const float u       = s.a * t + m->t1 - s.a * m->u1;
        float       cascade = u;
        for (uint32_t k = 0; k < s.num_sections; ++k) {
            const float section_out = s.disp_a * cascade + m->ds[k];
            m->ds[k] = string_disp_mutation == 3 ? cascade + s.disp_a * section_out : cascade - s.disp_a * section_out;
            cascade  = section_out;
        }
        float v        = cascade * (s.g * m->e);
        v              = fabsf(v) < 8.0f ? v : 0.0f;
        m->ring[m->wr] = v;
        m->t1          = t;
        m->u1          = u;
        m->wr          = m->wr + 1 == string_ring_size ? 0 : m->wr + 1;
    }
}

// Shader-equivalent pass: silent skips everything, the first in-range pass of a seed inits
// (full brightness; the optional tone stage is exercised through string_mirror_init directly).
static void string_mirror_play(StringMirror* m,
                               float         f0,
                               float         damping,
                               float         release_damping,
                               bool          release,
                               uint32_t      seed,
                               float*        out,
                               uint32_t      count)
{
    if (! string_mirror_scalars(f0, damping, release_damping).silent && ! (m->tagged && m->tag == seed)) {
        string_mirror_init(m, f0, damping, seed);
    }
    string_mirror_step(m, f0, damping, release_damping, release, out, count);
}

// Whole 256-sample dispatches; count is a multiple of 256.
static void string_mirror_run(StringMirror* m, float f0, float damping, uint32_t count, float* out)
{
    for (uint32_t i = 0; i < count; i += 256) {
        string_mirror_step(m, f0, damping, 0.0f, false, out + i, 256);
    }
}

// Hann-windowed DFT magnitude of x[start, start+len) at freq (Hz).
static double string_amp_at(const float* x, uint32_t start, uint32_t len, double freq)
{
    constexpr double two_pi = 6.283185307179586;
    double           re     = 0.0;
    double           im     = 0.0;
    for (uint32_t i = 0; i < len; ++i) {
        const double w  = 0.5 - 0.5 * cos(two_pi * i / len);
        const double ph = two_pi * freq * (start + i) / 44100.0;
        re += w * static_cast<double>(x[start + i]) * cos(ph);
        im += w * static_cast<double>(x[start + i]) * sin(ph);
    }
    return sqrt(re * re + im * im);
}

// Fundamental frequency (Hz): golden-section peak of the windowed DFT near f0.
// A decaying line keeps a symmetric peak under a symmetric window, so no bias.
static double string_peak_freq(const float* x, uint32_t start, uint32_t len, double f0, double half_width = 0.4)
{
    double           lo    = f0 - half_width;
    double           hi    = f0 + half_width;
    constexpr double ratio = 0.6180339887498949;
    double           a     = hi - ratio * (hi - lo);
    double           b     = lo + ratio * (hi - lo);
    double           fa    = string_amp_at(x, start, len, a);
    double           fb    = string_amp_at(x, start, len, b);
    for (int it = 0; it < 28; ++it) {
        if (fa < fb) {
            lo = a;
            a  = b;
            fa = fb;
            b  = lo + ratio * (hi - lo);
            fb = string_amp_at(x, start, len, b);
        }
        else {
            hi = b;
            b  = a;
            fb = fa;
            a  = hi - ratio * (hi - lo);
            fa = string_amp_at(x, start, len, a);
        }
    }
    return 0.5 * (lo + hi);
}

// Time to -60 dB (s) from the line amplitude in two equal windows `gap` samples apart.
static double string_measured_t60(const float* x, uint32_t start, uint32_t len, uint32_t gap, double f0)
{
    const double a1 = string_amp_at(x, start, len, f0);
    const double a2 = string_amp_at(x, start + gap, len, f0);
    return 60.0 * (gap / 44100.0) / (20.0 * log10(a1 / a2));
}

// DSP mirror core: these tests exercise only the mirror above, which is the
// executable spec the shader must match (evidence ceiling: CPU float32 logic,
// not GPU equivalence).
static constexpr uint32_t string_test_samples = 4 * 44100 / 256 * 256;

static float string_test_out[string_test_samples];

static StringMirror string_test_mirror;

static void string_test_render(float f0, float damping, uint32_t seed)
{
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, f0, damping, seed);
    string_mirror_run(&string_test_mirror, f0, damping, string_test_samples, string_test_out);
}

// Hand-checkable case: f0 = 4410 Hz gives L = 10, p = 0 gives c = 0.1, so
// tau = 1/9, D = 9.8889, N = 8, d = 1.8889, a = -0.30769 and g is capped at
// 0.9999. Ring 0.5, -0.25, 0.75, -1, 0.25, -0.5, 1, -0.75:
//   i = 0: x = 0.5, t = 0.45, u = a*0.45 = -0.13846154, v = u*0.9999 = -0.13844769
//   i = 1: x = -0.25, t = -0.18, u = a*(-0.18) + 0.45 - a*(-0.13846154) = 0.46278107,
//          v = 0.46273479
// With release (q = 1): Gr = 10^(-30/2205) = 0.96915794, kr = 0.0075300848, so
// e = 0.99976776 then 0.99953726, v = -0.13841554 and 0.46252066.
static void check_string_mirror_recurrence()
{
    const float         ring0[8] = { 0.5f, -0.25f, 0.75f, -1.0f, 0.25f, -0.5f, 1.0f, -0.75f };
    const StringScalars scalars  = string_mirror_scalars(4410.0f, 0.0f, 1.0f);
    TEST(! scalars.silent);
    TEST(scalars.n == 8);
    TEST(approx(scalars.c, 0.1f, 1e-7f));
    TEST(approx(scalars.a, -0.30769231f, 1e-6f));
    TEST(scalars.g == 0.9999f);
    TEST(approx(scalars.gr, 0.96915794f, 1e-6f));

    for (uint32_t release = 0; release < 2; ++release) {
        StringMirror m = {};
        for (uint32_t i = 0; i < 8; ++i) {
            m.ring[i] = ring0[i];
        }
        m.wr = 8;
        m.e  = 1.0f;
        float out[2];
        string_mirror_step(&m, 4410.0f, 0.0f, 1.0f, release != 0, out, 2);
        TEST(out[0] == 0.5f);
        TEST(out[1] == -0.25f);
        TEST(m.wr == 10);
        TEST(approx(m.t1, -0.18f, 1e-6f));
        TEST(approx(m.u1, 0.46278107f, 1e-6f));
        TEST(approx(m.ring[8], release ? -0.13841554f : -0.13844769f, 1e-6f));
        TEST(approx(m.ring[9], release ? 0.46252066f : 0.46273479f, 1e-6f));
        TEST(approx(m.e, release ? 0.99953726f : 1.0f, 1e-6f));
        TEST(m.ring[0] == ring0[0]); // untouched slots keep their value
    }

    // Init: ring slots 0..N-1 sum to ~0, state is reset.
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, 220.0f, 0.5f, 99);
    const uint32_t n   = string_mirror_scalars(220.0f, 0.5f, 0.0f).n;
    double         sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        sum += static_cast<double>(string_test_mirror.ring[i]);
    }
    TEST(fabs(sum / n) < 1e-6);
    TEST(string_test_mirror.wr == n && string_test_mirror.e == 1.0f);
}

// C1: the dominant spectral line sits within one bin of the fundamental.
// 16384-sample Hann window from 1.0 s at p = 0.5 (t60 2.2 s): the damped
// upper harmonics have decayed by then.
static void check_string_mirror_spectral_peak()
{
    constexpr uint32_t len      = 16384;
    constexpr double   bin      = 44100.0 / len;
    const float        freqs[2] = { 82.41f, 329.63f };
    for (uint32_t i = 0; i < 2; ++i) {
        string_test_render(freqs[i], 0.5f, 1234);
        uint32_t best     = 0;
        double   best_amp = 0.0;
        for (uint32_t k = 1; k * bin < 5.0 * static_cast<double>(freqs[i]); ++k) {
            const double amp = string_amp_at(string_test_out, 44100, len, k * bin);
            if (amp > best_amp) {
                best_amp = amp;
                best     = k;
            }
        }
        const double expected = static_cast<double>(freqs[i]) / bin;
        TEST(fabs(best - expected) <= 1.0);
        // C1: refine around the coarse bin (golden section) and hold 10 cents,
        // for the low E as well as the high note.
        const double refined = string_peak_freq(string_test_out, 44100, len, best * bin, bin);
        TEST(fabs(1200.0 * log2(refined / static_cast<double>(freqs[i]))) < 10.0);
    }
}

// C2: the fundamental t60 matches 20 * (0.25/20)^p. Tolerance 2%: the design
// makes the fundamental exact (g divides by |H(f0)|), so the error is only the
// Hann-window amplitude estimate on a decaying line (measured < 0.1%) plus
// float32 rounding. Broadband RMS is not used: harmonics decay faster.
static void check_string_mirror_t60()
{
    const float damping[3] = { 0.25f, 0.5f, 0.75f };
    double      previous   = 1e9;
    for (uint32_t i = 0; i < 3; ++i) {
        string_test_render(110.0f, damping[i], 77);
        const double designed = 20.0 * pow(0.25 / 20.0, damping[i]);
        const double measured = string_measured_t60(string_test_out, 1024, 8192, 11025, 110.0);
        TEST(fabs(measured / designed - 1.0) < 0.02);
        TEST(measured < previous);
        previous = measured;
    }
}

// Allpass fractional tuning: fs/f0 is non-integer for every entry. The golden
// section peak of a 2 s window (symmetric, so decay adds no bias) must be
// within 0.1 cent. Measured worst case is about 0.014 cent (880 Hz).
static void check_string_mirror_tuning()
{
    const float freqs[5] = { 123.47f, 261.63f, 329.63f, 440.0f, 880.0f };
    for (uint32_t i = 0; i < 5; ++i) {
        string_test_render(freqs[i], 0.0f, 4321);
        const double peak = string_peak_freq(string_test_out, 11025, 88200, static_cast<double>(freqs[i]));
        TEST(fabs(1200.0 * log2(peak / static_cast<double>(freqs[i]))) < 0.1);
    }
}

// Loop-gain cap. The cap engages when 10^(-3L/(fs*t60))/|H| exceeds 0.9999,
// which needs a short loop (high note) with a long t60, or a lowpass-heavy
// loop. It never engages on low notes: 82.41 Hz at p = 0 and p = 1 stays
// below it. Where it engages the fundamental decays FASTER than the designed
// t60 (the gain cannot reach the target), at L/fs * 3 / -log10(g*|H|).
static void check_string_mirror_gain_cap()
{
    TEST(string_mirror_scalars(82.41f, 0.0f, 0.0f).g < 0.9999f);
    TEST(string_mirror_scalars(82.41f, 1.0f, 0.0f).g < 0.9999f);

    const float damping[2] = { 0.0f, 1.0f };
    for (uint32_t i = 0; i < 2; ++i) {
        const float f0 = 2000.0f;
        TEST(string_mirror_scalars(f0, damping[i], 0.0f).g == 0.9999f);
        string_test_render(f0, damping[i], 5);
        const double c        = 0.10 + 0.30 * static_cast<double>(damping[i]);
        const double omega    = 6.283185307179586 * static_cast<double>(f0) / 44100.0;
        const double mag      = (1.0 - c) / sqrt(1.0 - 2.0 * c * cos(omega) + c * c);
        const double expected = (44100.0 / static_cast<double>(f0)) / 44100.0 * 3.0 / -log10(0.9999 * mag);
        const double designed = 20.0 * pow(0.25 / 20.0, damping[i]);
        const double measured = string_measured_t60(string_test_out, 0, 2048, 4410, static_cast<double>(f0));
        TEST(fabs(measured / expected - 1.0) < 0.02);
        TEST(measured < designed);
    }
}

// 22 Hz floor. The design maps f0 < 22 (and NaN) to silence, not a clamp (a
// clamp would sound a wrong pitch): output is 0 and the state is untouched.
// 22 Hz is the longest loop (N = 2002 at p = 1) and must stay finite and in bounds.
static void check_string_mirror_floor()
{
    const float silent_freqs[3] = { 21.9f, 0.0f, NAN };
    for (uint32_t i = 0; i < 3; ++i) {
        TEST(string_mirror_scalars(silent_freqs[i], 0.5f, 0.0f).silent);
        StringMirror m = {};
        m.ring[0]      = 1.0f;
        m.wr           = 5;
        float out[256];
        for (uint32_t k = 0; k < 256; ++k) {
            out[k] = 1.0f;
        }
        string_mirror_init(&m, silent_freqs[i], 0.5f, 3);
        string_mirror_step(&m, silent_freqs[i], 0.5f, 0.0f, false, out, 256);
        TEST(m.wr == 5 && m.ring[0] == 1.0f);
        for (uint32_t k = 0; k < 256; ++k) {
            TEST(out[k] == 0.0f);
        }
    }

    const float live_freqs[3] = { 22.0f, 440.0f, 10000.0f };
    for (uint32_t i = 0; i < 3; ++i) {
        const StringScalars s = string_mirror_scalars(live_freqs[i], 1.0f, 0.0f);
        TEST(! s.silent && s.n >= 1 && s.n < string_ring_size - 1);
        string_test_render(live_freqs[i], 1.0f, 8);
        double peak = 0.0;
        for (uint32_t k = 0; k < string_test_samples; ++k) {
            TEST(fabsf(string_test_out[k]) < 8.0f);
            peak = fmax(peak, fabs(string_test_out[k]));
        }
        TEST(peak > 0.0);
    }
    TEST(string_mirror_scalars(22.0f, 1.0f, 0.0f).n == 2002);
}

static void string_mirror_m2_regressions_a()
{
    check_string_mirror_recurrence();
    check_string_mirror_spectral_peak();
    check_string_mirror_t60();
    check_string_mirror_tuning();
    check_string_mirror_gain_cap();
    check_string_mirror_floor();
}

// ---- Release lifetime, pick comb, FM ring read, edge bounds.
// Mirror-level evidence only (CPU float32 logic, not GPU equivalence).

// Render `total_blocks` dispatches into string_test_out.  The note is released
// (e smoother target = Gr) from block `release_block` on.
static void string_release_render(float    f0,
                                  float    damping,
                                  float    release_damping,
                                  uint32_t seed,
                                  uint32_t release_block,
                                  uint32_t total_blocks)
{
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, f0, damping, seed);
    for (uint32_t b = 0; b < total_blocks; ++b) {
        string_mirror_step(&string_test_mirror,
                           f0,
                           damping,
                           release_damping,
                           b >= release_block,
                           string_test_out + b * 256,
                           256);
    }
}

static float string_max_step(const float* x, uint32_t from, uint32_t to)
{
    float worst = 0.0f;
    for (uint32_t i = from + 1; i < to; ++i) {
        worst = fmaxf(worst, fabsf(x[i] - x[i - 1]));
    }
    return worst;
}

// C3a: with q > 0 the release is click-free.  The e smoother (3 ms) moves the
// loop gain continuously, so the largest sample-to-sample step in the 2048
// samples after the release boundary stays within 1% of the largest step in
// the 2048 samples before it (a gain jump would exceed the signal's own
// steps).  Also: q = 1 gives -60 dB per 50 ms once e has converged.
static void check_string_mirror_release_click_free()
{
    constexpr uint32_t release_block = 32;
    constexpr uint32_t boundary      = release_block * 256;

    // 1.0 is T60 = 50 ms; 1.6667 is the curved maximum (T60 = 30 ms), which the shader takes unclamped.
    static const float release_values[] = { 1.0f, 1.6667f };
    for (const float release_damping : release_values) {
        string_release_render(110.0f, 0.5f, release_damping, 99, release_block, 80);
        const float before = string_max_step(string_test_out, boundary - 2048, boundary);
        const float across = string_max_step(string_test_out, boundary - 2048, boundary + 2048);
        TEST(before > 0.0f);
        TEST(across <= before * 1.01f);
    }

    // Fundamental level 25 ms after the release and 50 ms later: -60 dB +- 3 at q = 1.
    string_release_render(110.0f, 0.5f, 1.0f, 99, release_block, 80);
    const uint32_t start   = boundary + 1100;
    const double   a1      = string_amp_at(string_test_out, start, 1024, 110.0);
    const double   a2      = string_amp_at(string_test_out, start + 2205, 1024, 110.0);
    const double   drop_db = 20.0 * log10(a1 / a2);
    TEST(fabs(drop_db - 60.0) < 3.0);
}

// C3b: release damping 0 keeps Gr = 1 (e stays 1), so the note rings to the
// natural t60 of the damping row.
static void check_string_mirror_release_zero_rings()
{
    string_release_render(110.0f, 0.5f, 0.0f, 99, 8, 400);
    const double designed = 20.0 * pow(0.25 / 20.0, 0.5);
    const double measured = string_measured_t60(string_test_out, 4096, 8192, 11025, 110.0);
    TEST(fabs(measured / designed - 1.0) < 0.02);
}

// C3 production lifetime: the volume envelope owns the voice.  In
// volume_termination_due a voice with no volume envelope frees on its first
// released block, so no release damping is ever heard.  With a long volume
// release (2 s linear here) the voice stays alive through the release, and
// the string's own damping (palm mute) is the only thing that shortens the
// note.  Output = string * volume envelope, as the combine stage does.
static double string_lifetime_rms(float release_damping, uint32_t window_from, uint32_t window_len, double* env_at_end)
{
    constexpr uint32_t off_block           = 32;
    constexpr uint32_t boundary            = off_block * 256;
    constexpr double   vol_release_samples = 88200.0;
    string_release_render(110.0f, 0.5f, release_damping, 99, off_block, 128);
    double sum = 0.0;
    for (uint32_t k = window_from; k < window_from + window_len; ++k) {
        const double vol = 1.0 - static_cast<double>(k - boundary) / vol_release_samples;
        const double v   = static_cast<double>(string_test_out[k]) * vol;
        sum += v * v;
    }
    *env_at_end = 1.0 - static_cast<double>(window_from + window_len - boundary) / vol_release_samples;
    return sqrt(sum / window_len);
}

static void check_string_mirror_release_lifetime()
{
    constexpr uint32_t from    = 32 * 256 + 4410;
    double             env_on  = 0.0;
    double             env_off = 0.0;
    const double       rms_on  = string_lifetime_rms(1.0f, from, 4410, &env_on);
    const double       rms_off = string_lifetime_rms(0.0f, from, 4410, &env_off);
    // The voice is still alive (volume release unfinished) in both runs.
    TEST(env_on > 0.8 && env_off > 0.8);
    // Release damping on vs off: the damped string is >40 dB quieter 100 ms in.
    TEST(rms_off > 0.01);
    TEST(rms_on < 0.01 * rms_off);

    // Pedal-up: a held pedal keeps release false; the first block after it
    // flips engages the damping at once (e is pulled toward gr within 6 ms).
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, 110.0f, 0.5f, 99);
    for (uint32_t b = 0; b < 32; ++b) {
        string_mirror_step(&string_test_mirror, 110.0f, 0.5f, 1.0f, false, string_test_out + b * 256, 256);
    }
    TEST(string_test_mirror.e == 1.0f);
    string_mirror_step(&string_test_mirror, 110.0f, 0.5f, 1.0f, true, string_test_out, 256);
    TEST(string_test_mirror.e < 0.5f);
}

// C3c: damping 0 (t60 = 20 s) holds amplitude for seconds: 3 s apart the
// fundamental decays by the designed 10^(-3 * 3 / 20) = 0.355 (5% tolerance
// on the measured t60).
static void check_string_mirror_sustain_holds()
{
    string_test_render(110.0f, 0.0f, 31);
    const double t60 = string_measured_t60(string_test_out, 1024, 8192, 132300, 110.0);
    TEST(fabs(t60 / 20.0 - 1.0) < 0.05);
    const double a1 = string_amp_at(string_test_out, 1024, 8192, 110.0);
    const double a2 = string_amp_at(string_test_out, 1024 + 132300, 8192, 110.0);
    TEST(a2 > 0.3 * a1);
}

static bool string_rings_equal(const float* a, const float* b, uint32_t n)
{
    return memcmp(a, b, n * sizeof(float)) == 0;
}

// Pluck brightness tone stage.  Frozen legacy reference for brightness 1: the pre-tone-stage excitation, the
// seeded noise burst followed by the mean pass.
static void string_legacy_excitation_reference(float* ref, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; ++i) {
        ref[i] = static_cast<float>(string_pcg_hash(seed + i) & 0xFFFFu) / 32767.5f - 1.0f;
    }
    float sum = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        sum += ref[k];
    }
    const float mean = sum / static_cast<float>(n);
    for (uint32_t k = 0; k < n; ++k) {
        ref[k] -= mean;
    }
}

// Independent double-precision tone stage: the exact periodic one-pole
// y[i] = (1 - a) / (1 - a^n) * sum_j a^j x[(i - j) mod n], mean removed, rescaled to the
// input energy. Uses the closed-form circular sum, not the shader's two-pass recurrence.
static void string_tone_reference(float* ref, const float* in, uint32_t n, float brightness)
{
    static double y[string_ring_size];
    const double  a         = static_cast<double>(0.95f * (1.0f - brightness));
    const double  norm      = (1.0 - a) / (1.0 - pow(a, static_cast<double>(n)));
    double        energy_in = 0.0;
    double        sum       = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        double weight = 1.0;
        double acc    = 0.0;
        for (uint32_t j = 0; j < n; ++j) {
            acc += weight * static_cast<double>(in[(i + n - j) % n]);
            weight *= a;
        }
        y[i] = norm * acc;
        sum += y[i];
        energy_in += static_cast<double>(in[i]) * static_cast<double>(in[i]);
    }
    const double mean = sum / static_cast<double>(n);
    double       var  = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        var += (y[i] - mean) * (y[i] - mean);
    }
    const double gain = var > 0.0 ? sqrt(energy_in / var) : 1.0;
    for (uint32_t i = 0; i < n; ++i) {
        ref[i] = static_cast<float>((y[i] - mean) * gain);
    }
}

static double string_ring_energy(const float* ring, uint32_t n)
{
    double energy = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        energy += static_cast<double>(ring[i]) * static_cast<double>(ring[i]);
    }
    return energy;
}

static double string_ring_sum(const float* ring, uint32_t n)
{
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        sum += static_cast<double>(ring[i]);
    }
    return sum;
}

// Circular first-difference energy over energy: about 2 for white noise, lower as the
// high end is attenuated.
static double string_ring_roughness(const float* ring, uint32_t n)
{
    double diff = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const double step = static_cast<double>(ring[i]) - static_cast<double>(ring[(i + n - 1) % n]);
        diff += step * step;
    }
    const double energy = string_ring_energy(ring, n);
    return energy > 0.0 ? diff / energy : 0.0;
}

static void check_string_mirror_brightness()
{
    static StringMirror m;
    static StringMirror base;
    static float        ref[string_ring_size];

    // (a) Brightness 1 (explicit and defaulted) is bit-identical to the frozen legacy excitation.  At damping 0.5:
    // 441 Hz gives N = 98 and 400 Hz gives N = 108 (both even); 420 Hz gives N = 103 (odd).
    const float legacy_freqs[3] = { 441.0f, 400.0f, 420.0f };
    for (const float f0 : legacy_freqs) {
        const uint32_t n = string_mirror_scalars(f0, 0.5f, 0.0f).n;
        TEST(n >= 3);
        TEST((n % 2u) == (f0 == 420.0f ? 1u : 0u));

        memset(&m, 0, sizeof(m));
        string_mirror_init(&m, f0, 0.5f, 55, 1.0f);
        string_legacy_excitation_reference(ref, n, 55);
        TEST(string_rings_equal(m.ring, ref, n));

        memset(&m, 0, sizeof(m));
        string_mirror_init(&m, f0, 0.5f, 55);
        TEST(string_rings_equal(m.ring, ref, n));

        // The tone stage is active just below 1.
        string_mirror_init(&m, f0, 0.5f, 55, 0.99f);
        TEST(! string_rings_equal(m.ring, ref, n));
    }

    // (b) and (c): lowered brightness against the independent periodic one-pole reference, plus
    // energy, mean and wrap-seam invariants. expected_n pins the shortest supported loops
    // (N = 3, 2, 1 at damping 0.5); 0 means unchecked.
    struct ToneCase {
        float    f0;
        uint32_t expected_n;
    };

    static const ToneCase cases[] = {
        { 441.0f, 0 }, { 400.0f, 0 }, { 110.0f, 0 }, { 8820.0f, 3 }, { 11025.0f, 2 }, { 20000.0f, 1 },
    };
    static const float levels[] = { 0.75f, 0.5f, 0.25f, 0.0f };
    for (const ToneCase& c : cases) {
        const uint32_t n = string_mirror_scalars(c.f0, 0.5f, 0.0f).n;
        TEST(c.expected_n == 0 || n == c.expected_n);
        memset(&base, 0, sizeof(base));
        string_mirror_init(&base, c.f0, 0.5f, 71, 1.0f);
        const double energy_in = string_ring_energy(base.ring, n);
        for (const float level : levels) {
            memset(&m, 0, sizeof(m));
            string_mirror_init(&m, c.f0, 0.5f, 71, level);
            for (uint32_t k = 0; k < n; ++k) {
                TEST(std::isfinite(m.ring[k]));
            }

            string_tone_reference(ref, base.ring, n, level);
            for (uint32_t k = 0; k < n; ++k) {
                TEST(approx(m.ring[k], ref[k], 1e-4f));
            }

            // Energy preserved, mean removed.
            TEST(fabs(string_ring_energy(m.ring, n) - energy_in) <= 1e-4 * energy_in + 1e-12);
            TEST(fabs(string_ring_sum(m.ring, n)) / n < 1e-5);

            // Seam-free wrap: the output obeys z[i] - a z[i - 1] = (1 - a) g x[i] at every index,
            // including i = 0 which reads z[n - 1]. A zero-state start breaks it at the seam.
            // The common gain g is fitted by projection and must be positive.
            if (n >= 2 && energy_in > 0.0) {
                const double a   = static_cast<double>(0.95f * (1.0f - level));
                double       dot = 0.0;
                for (uint32_t i = 0; i < n; ++i) {
                    const double r = static_cast<double>(m.ring[i]) - a * static_cast<double>(m.ring[(i + n - 1) % n]);
                    dot += r * static_cast<double>(base.ring[i]);
                }
                const double scale = dot / ((1.0 - a) * energy_in);
                TEST(scale > 0.0);
                for (uint32_t i = 0; i < n; ++i) {
                    const double r = static_cast<double>(m.ring[i]) - a * static_cast<double>(m.ring[(i + n - 1) % n]);
                    TEST(fabs(r - (1.0 - a) * scale * static_cast<double>(base.ring[i])) < 1e-4);
                }
            }
        }
    }

    // Shortest loops: one sample has nothing to filter and stays zero; two samples keep their
    // alternating shape.
    memset(&m, 0, sizeof(m));
    string_mirror_init(&m, 20000.0f, 0.5f, 71, 0.25f);
    TEST(string_mirror_scalars(20000.0f, 0.5f, 0.0f).n == 1 && m.ring[0] == 0.0f);
    memset(&m, 0, sizeof(m));
    string_mirror_init(&m, 11025.0f, 0.5f, 71, 0.25f);
    TEST(approx(m.ring[0], -m.ring[1], 1e-5f) && m.ring[0] != 0.0f);

    // High-frequency attenuation on a long loop (N is about 400): roughness is near-white at
    // brightness 1 and falls strictly as brightness drops.
    static const float ordered[] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };
    double             previous  = 0.0;
    double             white     = 0.0;
    for (uint32_t i = 0; i < 5; ++i) {
        memset(&m, 0, sizeof(m));
        string_mirror_init(&m, 110.0f, 0.5f, 71, ordered[i]);
        const uint32_t n         = string_mirror_scalars(110.0f, 0.5f, 0.0f).n;
        const double   roughness = string_ring_roughness(m.ring, n);
        if (i == 0) {
            white = roughness;
            TEST(white > 1.6);
        }
        else {
            TEST(roughness < previous);
        }
        previous = roughness;
    }
    TEST(previous < 0.25 * white);
}

// FM carrier ring read: slot position
// q = wr_end - 1 - L + fract(warped_phase) * L, linear interpolation, wrapped
// into [0, 2034). The FM carrier string timbre intentionally differs from the
// natural-phase string sound (fixed post-pass window, interpolated), so these
// tests pin the mapping, not equality with the pass output.
static float string_ring_read(const float* ring, uint32_t wr, float len, float phase, int32_t* index0, int32_t* index1)
{
    const float f  = (phase - floorf(phase)) * len;
    const float q  = static_cast<float>(wr) - 1.0f - len + f;
    int32_t     i0 = static_cast<int32_t>(floorf(q));
    const float a  = q - static_cast<float>(i0);
    i0 += i0 < 0 ? static_cast<int32_t>(string_ring_size) : 0;
    int32_t i1 = i0 + 1;
    i1 -= i1 >= static_cast<int32_t>(string_ring_size) ? static_cast<int32_t>(string_ring_size) : 0;
    *index0 = i0;
    *index1 = i1;
    return ring[i0] + a * (ring[i1] - ring[i0]);
}

static void check_string_mirror_fm_ring_read()
{
    static float ramp[string_ring_size];
    for (uint32_t i = 0; i < string_ring_size; ++i) {
        ramp[i] = static_cast<float>(i) * static_cast<float>(i);
    }
    int32_t i0 = 0;
    int32_t i1 = 0;
    // Hand case, integral L = 10, wr = 20: phase 0 -> slot 9; phase 0.25 ->
    // q = 11.5 -> 121 + 0.5 * (144 - 121) = 132.5.
    TEST(string_ring_read(ramp, 20, 10.0f, 0.0f, &i0, &i1) == 81.0f);
    TEST(string_ring_read(ramp, 20, 10.0f, 0.25f, &i0, &i1) == 132.5f);
    TEST(i0 == 11 && i1 == 12);
    // Non-integral L = 4.5, phase 0.5: f = 2.25, q = 16.75 -> 256 + 0.75 * 33.
    TEST(string_ring_read(ramp, 20, 4.5f, 0.5f, &i0, &i1) == 280.75f);
    // Warped phase 1.25 and -0.75 both map to fract = 0.25.
    TEST(string_ring_read(ramp, 20, 10.0f, 1.25f, &i0, &i1) == 132.5f);
    TEST(string_ring_read(ramp, 20, 10.0f, -0.75f, &i0, &i1) == 132.5f);
    // Constant ring returns the constant at any phase.
    static float flat[string_ring_size];
    for (uint32_t i = 0; i < string_ring_size; ++i) {
        flat[i] = 0.375f;
    }
    TEST(string_ring_read(flat, 700, 133.8f, 0.61f, &i0, &i1) == 0.375f);

    // Two consecutive dispatches with a non-integral L (f0 = 329.63, L = 133.8):
    // the window end advances by exactly 256 and every phase position moves
    // with it, so q2 = q1 + 256 for a fixed warped phase.
    static StringMirror m;
    memset(&m, 0, sizeof(m));
    string_mirror_init(&m, 329.63f, 0.5f, 21);
    float out[256];
    string_mirror_step(&m, 329.63f, 0.5f, 0.0f, false, out, 256);
    const uint32_t wr1 = m.wr;
    const float    len = string_fs / 329.63f;
    TEST(len != floorf(len));
    const float phases[3] = { 0.0f, 0.37f, 0.9f };
    float       read1[3];
    for (uint32_t k = 0; k < 3; ++k) {
        read1[k] = string_ring_read(m.ring, wr1, len, phases[k], &i0, &i1);
    }
    string_mirror_step(&m, 329.63f, 0.5f, 0.0f, false, out, 256);
    const uint32_t wr2 = m.wr;
    TEST(wr2 == (wr1 + 256) % string_ring_size);
    for (uint32_t k = 0; k < 3; ++k) {
        const double q =
            static_cast<double>(wr2) - 1.0 - static_cast<double>(len) + static_cast<double>(phases[k] * len);
        const int32_t base = static_cast<int32_t>(floor(q));
        const double  a    = q - base;
        const double  expected =
            static_cast<double>(m.ring[base]) + a * static_cast<double>(m.ring[base + 1] - m.ring[base]);
        const float read2 = string_ring_read(m.ring, wr2, len, phases[k], &i0, &i1);
        TEST(i0 == base);
        TEST(fabs(static_cast<double>(read2) - expected) < 1e-5);
        TEST(std::isfinite(read1[k]) && std::isfinite(read2));
    }
}

// Edge bounds: indices stay in [0, 2034) and output is finite for wr = 0 and
// 2033, a loop as long as the 22 Hz floor, and the awkward warped phases.
static void check_string_mirror_ring_read_edges()
{
    static float ring[string_ring_size];
    for (uint32_t i = 0; i < string_ring_size; ++i) {
        ring[i] = static_cast<float>(string_pcg_hash(i) & 0xFFFFu) / 32767.5f - 1.0f;
    }
    const uint32_t writes[4] = { 0, 1, 2032, 2033 };
    const float    lens[2]   = { 2004.545f, 3.0f };
    const float    phases[9] = { 0.0f, 0.5f, 0.999f, nextafterf(1.0f, 0.0f), -0.3f, -1e-9f, -2.75f, 5.5f, 1.0f };
    for (uint32_t w = 0; w < 4; ++w) {
        for (uint32_t l = 0; l < 2; ++l) {
            for (uint32_t p = 0; p < 9; ++p) {
                int32_t     i0 = -1;
                int32_t     i1 = -1;
                const float v  = string_ring_read(ring, writes[w], lens[l], phases[p], &i0, &i1);
                TEST(i0 >= 0 && i0 < static_cast<int32_t>(string_ring_size));
                TEST(i1 >= 0 && i1 < static_cast<int32_t>(string_ring_size));
                TEST(std::isfinite(v) && fabsf(v) <= 1.0f);
            }
        }
    }
    // Largest float below 1.0: fract * L may round to L, so q is the endpoint
    // wr - 1 or one slot below; both indices are in bounds (checked above).
    int32_t i0 = 0;
    int32_t i1 = 0;
    string_ring_read(ring, 0, 2004.545f, nextafterf(1.0f, 0.0f), &i0, &i1);
    TEST(i0 == 2032 || i0 == 2033);
    string_ring_read(ring, 2033, 2004.545f, nextafterf(1.0f, 0.0f), &i0, &i1);
    TEST(i0 == 2031 || i0 == 2032);
    // Negative phase that rounds fract to exactly 1.0: closed endpoint wr - 1.
    string_ring_read(ring, 0, 2004.545f, -1e-9f, &i0, &i1);
    TEST(i0 == 2033 && i1 == 0);
}

// FM characterization across the ring layout change. The legacy layout kept an 8-word header and a 2040-slot
// ring; the new one keeps 14 header words and a 2034-slot ring. Both layouts read the same logical last-L
// samples; only the wrap point and the float magnitude of wr differ. This feeds one smooth string-like source
// into a simulated legacy ring (size 2040) and a new ring (size 2034), reads both through the same FM mapping
// across several wrap boundaries of each layout, and bounds the difference by the float rounding of the read
// position (one ULP of a value below 2048 is 2^-13 slot) times the source slope.
// Measured on this grid: max |new - legacy| = 1.94e-5 absolute (1.1e-5 of the 1.75 peak source amplitude), under
// the 8.4e-5 position-rounding bound (float rounding of the read position, ~2^-13 slot, times source slope).
// That is 1.1e-5 of peak amplitude, i.e. ULP-scale of the position arithmetic, not of the sample values
// (reported to the parent: slightly above the 1e-5 relative target); evidence class accepted by user ruling 6.
// Re-measured after OscillatorParams shrank to 88 bytes (pick and sections removed): this test feeds its own simulated
// rings, so the bound is unchanged at max |new - legacy| = 1.94e-5 against the 8.4e-5 position-rounding bound.
static float string_ring_read_sized(const float* ring, uint32_t size, uint32_t wr, float len, float phase)
{
    const float f  = (phase - floorf(phase)) * len;
    const float q  = static_cast<float>(wr) - 1.0f - len + f;
    int32_t     i0 = static_cast<int32_t>(floorf(q));
    const float a  = q - static_cast<float>(i0);
    i0 += i0 < 0 ? static_cast<int32_t>(size) : 0;
    int32_t i1 = i0 + 1;
    i1 -= i1 >= static_cast<int32_t>(size) ? static_cast<int32_t>(size) : 0;
    return ring[i0] + a * (ring[i1] - ring[i0]);
}

static void check_string_fm_layout_characterization()
{
    static float legacy_ring[2040];
    static float new_ring[2034];
    const float  f0s[5]        = { 22.0f, 55.0f, 329.63f, 441.0f, 880.0f };
    const float  phases[6]     = { 0.0f, 0.1f, 0.37f, 0.5f, 0.9f, 0.9999f };
    double       max_diff      = 0.0;
    double       max_ulp_bound = 0.0;
    for (const float f0 : f0s) {
        const double len = 44100.0 / static_cast<double>(f0);
        const double w   = 2.0 * vmath::pi_double / len;
        // Peak slope per sample of the source: sum of k * amplitude_k * w.
        const double slope = w * (1.0 + 2.0 * 0.5 + 3.0 * 0.25);
        memset(legacy_ring, 0, sizeof(legacy_ring));
        memset(new_ring, 0, sizeof(new_ring));
        uint32_t checked = 0;
        for (uint32_t t = 1; t <= 3 * 2040 + 40; ++t) {
            const double x =
                sin(w * (t - 1.0)) + 0.5 * sin(2.0 * w * (t - 1.0) + 1.0) + 0.25 * sin(3.0 * w * (t - 1.0) + 2.0);
            legacy_ring[(t - 1) % 2040] = static_cast<float>(x);
            new_ring[(t - 1) % 2034]    = static_cast<float>(x);
            const uint32_t wr_old       = t % 2040;
            const uint32_t wr_new       = t % 2034;
            // Wrap boundaries of either layout plus a regular sweep.
            const bool boundary = wr_old < 3 || wr_old > 2037 || wr_new < 3 || wr_new > 2031 || t % 211 == 0;
            if (! boundary || t < static_cast<uint32_t>(len) + 2) {
                continue;
            }
            for (const float phase : phases) {
                const float old_v = string_ring_read_sized(legacy_ring, 2040, wr_old, static_cast<float>(len), phase);
                const float new_v = string_ring_read_sized(new_ring, 2034, wr_new, static_cast<float>(len), phase);
                TEST(std::isfinite(old_v) && std::isfinite(new_v));
                max_diff      = fmax(max_diff, fabs(static_cast<double>(new_v) - static_cast<double>(old_v)));
                max_ulp_bound = fmax(max_ulp_bound, 2.0 * slope * 0.0001221);
                ++checked;
            }
        }
        TEST(checked > 200);
    }
    TEST(max_diff < 3e-5);
    TEST(max_diff <= max_ulp_bound);
}

// NaN/inf/huge guard: a poisoned ring slot or filter state never leaves the
// loop.  Only the poisoned sample itself is read raw; everything written is
// flushed by the |v| < 8 rule, so later outputs stay finite and bounded.
static void check_string_mirror_nan_guard()
{
    static StringMirror m;
    const float         poison[3] = { 1e30f, INFINITY, NAN };
    for (uint32_t i = 0; i < 3; ++i) {
        memset(&m, 0, sizeof(m));
        string_mirror_init(&m, 441.0f, 0.5f, 14);
        const uint32_t n     = string_mirror_scalars(441.0f, 0.5f, 0.0f).n;
        m.ring[m.wr - n + 5] = poison[i];
        float out[512];
        string_mirror_step(&m, 441.0f, 0.5f, 0.0f, false, out, 256);
        string_mirror_step(&m, 441.0f, 0.5f, 0.0f, false, out + 256, 256);
        for (uint32_t k = 0; k < 512; ++k) {
            if (k != 5) {
                TEST(std::isfinite(out[k]) && fabsf(out[k]) < 8.0f);
            }
        }
    }
    // Poisoned filter state is flushed the same way.
    memset(&m, 0, sizeof(m));
    string_mirror_init(&m, 441.0f, 0.5f, 14);
    m.t1 = 1e30f;
    m.u1 = NAN;
    float out[256];
    string_mirror_step(&m, 441.0f, 0.5f, 0.0f, false, out, 256);
    for (uint32_t k = 0; k < 256; ++k) {
        TEST(std::isfinite(out[k]) && fabsf(out[k]) < 8.0f);
    }
}

// Overdriven damping destinations clamp to [0, 1] at the host read, and the release
// damping then goes through the curve (which clamps again), so the scalar block never sees
// c >= 0.4 or a release damping outside 0.025..1.667.  Finite bounded output over several
// seconds for both overdriven polarities and NaN.
static void check_string_mirror_damping_clamp()
{
    static StringMirror m;
    const float         drive[4] = { 3.0f, -3.0f, 1e30f, NAN };
    for (uint32_t di = 0; di < 4; ++di) {
        const float         clamped = drive[di] > 0.5f ? 1.0f : 0.0f;
        const float         expect  = drive[di] != drive[di] ? 0.0f : clamped;
        const StringScalars over =
            string_mirror_scalars(220.0f, drive[di], Synth::string_release_damping_curve(drive[di]));
        const StringScalars ref = string_mirror_scalars(220.0f, expect, Synth::string_release_damping_curve(expect));
        TEST(over.n == ref.n && over.c == ref.c && over.g == ref.g && over.gr == ref.gr && over.a == ref.a);
        TEST(over.gr > 0.0f && over.gr <= 1.0f);

        memset(&m, 0, sizeof(m));
        static float out[256];
        for (uint32_t step = 0; step < 4 * 44100 / 256; ++step) {
            const bool release = step > 2 * 44100 / 256;
            string_mirror_play(&m,
                               220.0f,
                               drive[di],
                               Synth::string_release_damping_curve(drive[di]),
                               release,
                               31,
                               out,
                               256);
            for (uint32_t k = 0; k < 256; ++k) {
                TEST(std::isfinite(out[k]) && fabsf(out[k]) < 8.0f);
            }
        }
    }
    // Negative release damping alone must not amplify: the curve maps it to the 0 knob, so gr stays <= 1.
    TEST(string_mirror_scalars(220.0f, 0.5f, Synth::string_release_damping_curve(-2.0f)).gr <= 1.0f);
    TEST(string_mirror_scalars(220.0f, 0.5f, 0.0f).gr == 1.0f);
}

// A note that starts below the 22 Hz floor keeps its init pending until the
// first in-range pass, so a later pitch step plays a fresh pluck and never stale ring data.
static void check_string_mirror_silent_onset_init()
{
    static StringMirror m;
    static StringMirror fresh;
    static float        out[256];
    static float        want[256];

    // Fresh slot: poisoned (arbitrary) memory, silent onset, then pitch into range.
    memset(&m, 0, sizeof(m));
    for (uint32_t i = 0; i < string_ring_size; ++i) {
        m.ring[i] = 0.75f;
    }
    m.wr = 123;
    m.t1 = 1.0f;
    for (uint32_t step = 0; step < 3; ++step) {
        string_mirror_play(&m, 10.0f, 0.5f, 0.0f, false, 77, out, 256);
        for (uint32_t k = 0; k < 256; ++k) {
            TEST(out[k] == 0.0f);
        }
    }
    TEST(! m.tagged);
    string_mirror_play(&m, 441.0f, 0.5f, 0.0f, false, 77, out, 256);
    memset(&fresh, 0, sizeof(fresh));
    string_mirror_init(&fresh, 441.0f, 0.5f, 77);
    string_mirror_step(&fresh, 441.0f, 0.5f, 0.0f, false, want, 256);
    TEST(memcmp(out, want, sizeof(out)) == 0);
    TEST(out[1] != 0.0f);

    // Reused slot: note A plays and is released, then note B starts silent, then pitches in.
    memset(&m, 0, sizeof(m));
    for (uint32_t step = 0; step < 8; ++step) {
        string_mirror_play(&m, 441.0f, 0.5f, 0.5f, step > 4, 5, out, 256);
    }
    TEST(m.tagged && m.tag == 5);
    string_mirror_play(&m, 10.0f, 0.5f, 0.5f, false, 6, out, 256);
    TEST(out[0] == 0.0f && out[255] == 0.0f);
    string_mirror_play(&m, 330.0f, 0.5f, 0.0f, false, 6, out, 256);
    memset(&fresh, 0, sizeof(fresh));
    string_mirror_init(&fresh, 330.0f, 0.5f, 6);
    string_mirror_step(&fresh, 330.0f, 0.5f, 0.0f, false, want, 256);
    TEST(memcmp(out, want, sizeof(out)) == 0);
    TEST(m.tagged && m.tag == 6);
}

// ---- String dispersion. Frozen legacy loop (no cascade) for the d = 0 bit-identity check.
static void string_legacy_step(StringMirror* m, float f0, float damping, float* out, uint32_t count)
{
    const StringScalars s  = string_mirror_scalars(f0, damping, 0.0f);
    const float         kr = 1.0f - expf(-1.0f / (string_fs * 0.003f));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t rd = m->wr >= s.n ? m->wr - s.n : m->wr + string_ring_size - s.n;
        const float    x  = m->ring[rd];
        out[i]            = x;
        m->e += kr * (1.0f - m->e);
        const float t  = (1.0f - s.c) * x + s.c * m->t1;
        const float u  = s.a * t + m->t1 - s.a * m->u1;
        float       v  = u * (s.g * m->e);
        v              = fabsf(v) < 8.0f ? v : 0.0f;
        m->ring[m->wr] = v;
        m->t1          = t;
        m->u1          = u;
        m->wr          = m->wr + 1 == string_ring_size ? 0 : m->wr + 1;
    }
}

// Independent double-precision phase-closure oracle for the loop (lowpass + fractional allpass + cascade):
// returns the frequency (Hz) of the k-th loop resonance, k = 1..4. Its own single-section path and Newton solve.
static void string_disp_oracle(double f0, double dispersion, double damping, double* partials)
{
    const double fs  = 44100.0;
    const double len = fs / f0;
    const double c   = 0.1 + 0.3 * damping;
    const double w0  = 2.0 * vmath::pi_double * f0 / fs;
    // Own single-section path and Newton solve of r^3 - r = strength * dispersion / w0^3, with the same fallback to no
    // section when less than 3 samples of loop delay would remain.
    double count = 1.0;
    double ad    = 0.0;
    double pd    = 0.0;
    {
        const double q = static_cast<double>(string_dispersion_strength) * dispersion / (count * w0 * w0 * w0);
        double       r = 1.0 + cbrt(q);
        for (int it = 0; it < 80; ++it) {
            r -= (r * r * r - r - q) / (3.0 * r * r - 1.0);
        }
        ad = (1.0 - r) / (1.0 + r);
        pd = (2.0 / w0) * atan2((1.0 - ad) * sin(0.5 * w0), (1.0 + ad) * cos(0.5 * w0));
        if (! (len - c / (1.0 - c) - count * pd >= 3.0)) {
            count = 0.0;
        }
    }
    const double delay       = len - c / (1.0 - c) - count * pd;
    const double whole       = floor(delay - 1.0);
    const double frac        = delay - whole;
    const double a           = (1.0 - frac) / (1.0 + frac);
    auto         allpass_lag = [](double coeff, double w) {
        return atan2(sin(w), coeff + cos(w)) - atan2(coeff * sin(w), 1.0 + coeff * cos(w));
    };
    for (int k = 1; k <= 4; ++k) {
        double lo = (k - 0.5) * w0;
        double hi = (k + 0.5) * w0;
        for (int it = 0; it < 80; ++it) {
            const double w = 0.5 * (lo + hi);
            const double lag =
                whole * w + atan2(c * sin(w), 1.0 - c * cos(w)) + allpass_lag(a, w) + count * allpass_lag(ad, w);
            (lag < 2.0 * vmath::pi_double * k ? lo : hi) = w;
        }
        partials[k] = 0.5 * (lo + hi) * fs / (2.0 * vmath::pi_double);
    }
}

// True when a bracketed peak search found a peak: finite, inside 90% of the half width around the centre, and
// stronger than the spectrum at both bracket ends.
static bool string_peak_valid(double peak, double center, double half_width)
{
    if (! std::isfinite(peak) || fabs(peak - center) >= 0.9 * half_width) {
        return false;
    }
    const double at_peak = string_amp_at(string_test_out, 11025, 32768, peak);
    return std::isfinite(at_peak) && at_peak > string_amp_at(string_test_out, 11025, 32768, center - half_width) &&
           at_peak > string_amp_at(string_test_out, 11025, 32768, center + half_width);
}

// Extra measurements of one string_disp_case render: stretch (cents over k f1) of partials 2 and 3, and the
// oracle's 4th-partial stretch.
struct StringDispExtra {
    double stretch_p2;
    double stretch_p3;
    double oracle_stretch;
};

// Renders a fresh note and measures the fundamental and partials 2..4. Returns true when the fundamental is
// within 1 cent of f0, and the stretch of each of partials 2, 3 and 4 (cents over k f1) is within 1 cent of
// the oracle's.
static bool string_disp_case(float f0, float dispersion, double* stretch, StringDispExtra* extra = nullptr)
{
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, f0, 0.0f, 4321, 1.0f, dispersion);
    for (uint32_t i = 0; i < string_test_samples; i += 256) {
        string_mirror_step(&string_test_mirror, f0, 0.0f, 0.0f, false, string_test_out + i, 256, dispersion);
    }
    double oracle[5];
    string_disp_oracle(static_cast<double>(f0), static_cast<double>(dispersion), 0.0, oracle);
    const double f1_width = fmax(0.4, 0.006 * static_cast<double>(f0));
    const double f1       = string_peak_freq(string_test_out, 11025, 32768, static_cast<double>(f0), f1_width);
    double       peaks[5] = {};
    // Each bracketed search must land on a real peak: finite, strictly inside its bracket (not pinned to an
    // edge) and above the spectrum at both bracket ends.
    bool valid = string_peak_valid(f1, static_cast<double>(f0), f1_width);
    for (int k = 2; k <= 4; ++k) {
        peaks[k] = string_peak_freq(string_test_out, 11025, 32768, oracle[k], 1.5);
        valid    = valid && string_peak_valid(peaks[k], oracle[k], 1.5);
    }
    if (! valid) {
        return false;
    }
    double measured[5] = {};
    bool   matches     = true;
    for (int k = 2; k <= 4; ++k) {
        measured[k]           = 1200.0 * log2(peaks[k] / (k * f1));
        const double expected = 1200.0 * log2(oracle[k] / (k * oracle[1]));
        matches               = matches && fabs(measured[k] - expected) < 1.0;
    }
    *stretch = measured[4];
    if (extra) {
        extra->stretch_p2     = measured[2];
        extra->stretch_p3     = measured[3];
        extra->oracle_stretch = 1200.0 * log2(oracle[4] / (4.0 * oracle[1]));
    }
    return fabs(1200.0 * log2(f1 / static_cast<double>(f0))) < 1.0 && matches;
}

// Frozen legacy memory layout (8-word header, 2040-slot ring) running the natural string loop.  Only the
// ring wrap point differs from the current layout (14-word header, 2034 slots); the loop is integer indexed
// (delay_whole + fractional allpass), so its output must match the current layout bit for bit.
static constexpr uint32_t string_legacy_ring_size = 2040;

// Frozen baseline: re-frozen at the voicing wave from the tree that already carried the dispersion recalibration
// (count-normalized solve, strength 0.05863), the release damping curve (applied at the summed write point, outside
// the scalars) and the String Dispersion target migration.  Changes since the original capture at HEAD 34d1b94:
// dispersion cascade and its per-count solve added, recalibration, damping curve, dispersion migration, and now
// pick removal plus the sections freeze (the captured excitation is the pick = 0 burst; the captured scalars take
// the requested section count, and the current code must equal them at a count of one for every requested count).
// The helpers below are verbatim copies of the pre-removal arithmetic, so a regression in the current scalar or
// excitation code cannot hide in the comparison.  Update only deliberately.
static void string_legacy_layout_step(float*               ring,
                                      uint32_t*            wr,
                                      float*               t1,
                                      float*               u1,
                                      float*               e,
                                      float*               ds,
                                      const StringScalars& s,
                                      float*               out,
                                      uint32_t             count)
{
    const float kr = 1.0f - expf(-1.0f / (string_fs * 0.003f));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t rd = *wr >= s.n ? *wr - s.n : *wr + string_legacy_ring_size - s.n;
        const float    x  = ring[rd];
        out[i]            = x;
        *e += kr * (1.0f - *e);
        const float t       = (1.0f - s.c) * x + s.c * *t1;
        const float u       = s.a * t + *t1 - s.a * *u1;
        float       cascade = u;
        for (uint32_t k = 0; k < s.num_sections; ++k) {
            const float section_out = s.disp_a * cascade + ds[k];
            ds[k]                   = cascade - s.disp_a * section_out;
            cascade                 = section_out;
        }
        float v   = cascade * (s.g * *e);
        v         = fabsf(v) < 8.0f ? v : 0.0f;
        ring[*wr] = v;
        *t1       = t;
        *u1       = u;
        *wr       = *wr + 1 == string_legacy_ring_size ? 0 : *wr + 1;
    }
}

static float string_legacy_frozen_ratio(float q)
{
    if (q <= 0.3849002f) {
        const float cos_arg = fminf(1.0f, 2.598076f * q);
        return 1.1547005f * cosf(acosf(cos_arg) * 0.33333334f);
    }
    const float cardano = powf(q * 0.5f + sqrtf(q * q * 0.25f - 0.037037037f), 0.33333334f);
    return cardano + 0.33333334f / cardano;
}

static float string_legacy_frozen_phase_delay(float coeff, float omega)
{
    return (2.0f / omega) * atan2f((1.0f - coeff) * sinf(0.5f * omega), (1.0f + coeff) * cosf(0.5f * omega));
}

static StringScalars string_legacy_frozen_scalars(float f0,
                                                  float damping,
                                                  float release_damping,
                                                  float dispersion,
                                                  float sections)
{
    StringScalars s = {};
    s.silent        = ! (f0 >= 22.0f);
    if (s.silent) {
        return s;
    }
    damping           = string_unit_clamp(damping);
    release_damping   = string_unit_clamp(release_damping);
    float len         = string_fs / f0;
    len               = len < 3.0f ? 3.0f : len;
    s.c               = 0.10f + 0.30f * damping;
    const float tau   = s.c / (1.0f - s.c);
    const float t60   = 20.0f * powf(0.25f / 20.0f, damping);
    const float omega = 6.2831853f * f0 / string_fs;
    const float mag   = (1.0f - s.c) / sqrtf(1.0f - 2.0f * s.c * cosf(omega) + s.c * s.c);
    const float g     = powf(10.0f, -3.0f * len / (string_fs * t60)) / mag;
    s.g               = g < 0.9999f ? g : 0.9999f;
    float disp_delay  = 0.0f;
    if (dispersion > 0.0f) {
        const float    rounded   = floorf(sections + 0.5f);
        const uint32_t requested = static_cast<uint32_t>(fminf(fmaxf(rounded, 1.0f), 6.0f));
        for (uint32_t count = requested; count > 0; --count) {
            const float count_f     = static_cast<float>(count);
            const float disp_q      = 0.05863f * dispersion / (count_f * (omega * omega * omega));
            const float disp_r      = string_legacy_frozen_ratio(disp_q);
            const float coeff       = (1.0f - disp_r) / (1.0f + disp_r);
            const float count_delay = count_f * string_legacy_frozen_phase_delay(coeff, omega);
            if (len - tau - count_delay >= 3.0f) {
                s.disp_a       = coeff;
                s.num_sections = count;
                disp_delay     = count_delay;
                break;
            }
        }
    }
    s.disp_delay      = disp_delay;
    const float delay = len - tau - s.disp_delay;
    s.n               = static_cast<uint32_t>(floorf(delay - 1.0f));
    const float d     = delay - static_cast<float>(s.n);
    s.a               = (1.0f - d) / (1.0f + d);
    s.gr              = powf(10.0f, -release_damping * 3.0f * len / (string_fs * 0.05f));
    return s;
}

// Frozen excitation: the pick = 0 burst (noise fill, then zero mean).
static void string_legacy_frozen_init(float* ring, const StringScalars& s, uint32_t seed)
{
    const uint32_t n = s.n;
    for (uint32_t i = 0; i < n; ++i) {
        ring[i] = static_cast<float>(string_pcg_hash(seed + i) & 0xFFFFu) / 32767.5f - 1.0f;
    }
    float sum = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        sum += ring[k];
    }
    const float mean = sum / static_cast<float>(n);
    for (uint32_t k = 0; k < n; ++k) {
        ring[k] -= mean;
    }
}

// Natural string path: current layout (with the dispersion rows at 0 and with them omitted) versus the legacy
// 2040-slot layout, for dispersion 0 and above.  Output must be bit-identical, and the last 2034 logical ring
// slots must match exactly across several wraps of both layouts.  The frozen side runs with one section, the only
// count the current code has.
static void check_string_natural_path_legacy_capacity()
{
    static StringMirror current;
    static float        legacy_ring[string_legacy_ring_size];
    static float        out_current[6144];
    static float        out_plain[6144];
    static float        out_legacy[6144];
    static StringMirror plain;
    constexpr uint32_t  count          = 6144;
    const float         f0s[9]         = { 22.5f, 30.0f, 55.0f, 110.0f, 261.63f, 441.0f, 880.0f, 3000.0f, 8000.0f };
    const float         dampings[2]    = { 0.5f, 0.0f };
    const float         dispersions[3] = { 0.0f, 0.5f, 1.0f };
    uint32_t            compared       = 0;
    for (const float f0 : f0s) {
        for (const float damping : dampings) {
            for (const float dispersion : dispersions) {
                {
                    const StringScalars s = string_legacy_frozen_scalars(f0, damping, 0.0f, dispersion, 1.0f);
                    TEST(! s.silent && s.n >= 2 && s.n < string_ring_size);
                    const StringScalars now = string_mirror_scalars(f0, damping, 0.0f, dispersion);
                    TEST(now.n == s.n && now.c == s.c && now.a == s.a && now.g == s.g && now.gr == s.gr &&
                         now.disp_a == s.disp_a && now.num_sections == s.num_sections &&
                         now.disp_delay == s.disp_delay);
                    memset(&current, 0, sizeof(current));
                    memset(&plain, 0, sizeof(plain));
                    string_mirror_init(&current, f0, damping, 77, 1.0f, dispersion);
                    // The legacy side uses the frozen scalars and excitation, not the current helpers.
                    memset(legacy_ring, 0, sizeof(legacy_ring));
                    string_legacy_frozen_init(legacy_ring, s, 77);
                    uint32_t legacy_wr               = s.n;
                    float    t1                      = 0.0f;
                    float    u1                      = 0.0f;
                    float    e                       = 1.0f;
                    float    ds[string_max_sections] = {};
                    for (uint32_t i = 0; i < count; i += 256) {
                        string_mirror_step(&current, f0, damping, 0.0f, false, out_current + i, 256, dispersion);
                        string_legacy_layout_step(legacy_ring, &legacy_wr, &t1, &u1, &e, ds, s, out_legacy + i, 256);
                    }
                    TEST(memcmp(out_current, out_legacy, sizeof(out_legacy)) == 0);
                    if (dispersion == 0.0f) {
                        string_mirror_init(&plain, f0, damping, 77);
                        for (uint32_t i = 0; i < count; i += 256) {
                            string_mirror_step(&plain, f0, damping, 0.0f, false, out_plain + i, 256);
                        }
                        TEST(memcmp(out_plain, out_legacy, sizeof(out_legacy)) == 0);
                    }
                    double energy = 0.0;
                    for (uint32_t i = 0; i < count; ++i) {
                        energy += static_cast<double>(out_legacy[i]) * static_cast<double>(out_legacy[i]);
                    }
                    TEST(energy > 0.0);
                    TEST(current.wr == (s.n + count) % string_ring_size &&
                         legacy_wr == (s.n + count) % string_legacy_ring_size);
                    // Logical write j lives at (n + j) modulo the layout's ring size; compare the last 2034 writes.
                    bool ring_same = true;
                    for (uint32_t j = count - string_ring_size; j < count; ++j) {
                        ring_same = ring_same && current.ring[(s.n + j) % string_ring_size] ==
                                                     legacy_ring[(s.n + j) % string_legacy_ring_size];
                    }
                    TEST(ring_same);
                    ++compared;
                }
            }
        }
    }
    TEST(compared == 54);
}

// Held note whose pitch sweeps from `start` up to `peak` (in the fallback region) and down to `target`, then
// holds. Every sample stays finite and bounded. Returns true when the settled fundamental (measured long after
// the sweep) is within 1 cent of the target.
static bool string_disp_held_sweep(float start, float peak, float target, float dispersion)
{
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, start, 0.0f, 4321, 1.0f, dispersion);
    for (uint32_t block = 0; block < string_test_samples / 256; ++block) {
        float f0 = target;
        if (block < 10) {
            f0 = start * powf(peak / start, static_cast<float>(block + 1) / 10.0f);
        }
        else if (block < 20) {
            f0 = peak * powf(target / peak, static_cast<float>(block - 9) / 10.0f);
        }
        float* const out = string_test_out + block * 256;
        string_mirror_step(&string_test_mirror, f0, 0.0f, 0.0f, false, out, 256, dispersion);
        for (uint32_t i = 0; i < 256; ++i) {
            TEST(std::isfinite(out[i]) && fabsf(out[i]) < 8.0f);
        }
    }
    double tail_energy = 0.0;
    for (uint32_t i = string_test_samples - 256; i < string_test_samples; ++i) {
        tail_energy += static_cast<double>(string_test_out[i]) * static_cast<double>(string_test_out[i]);
    }
    TEST(tail_energy > 0.0);
    const double width = fmax(0.4, 0.006 * static_cast<double>(target));
    const double f1    = string_peak_freq(string_test_out, 11025, 32768, static_cast<double>(target), width);
    return string_peak_valid(f1, static_cast<double>(target), width) &&
           fabs(1200.0 * log2(f1 / static_cast<double>(target))) < 1.0;
}

// Inertness of the stored section count: the shader always runs one section, so renders made with the stored
// count at 1 and at 6 must be bit-identical.  Mutation 6 reads the stored count instead of the constant 1, which
// makes the two renders differ.
static bool string_disp_sections_inert()
{
    static float render_one[4096];
    static float render_six[4096];
    const float  pitches[3] = { 55.0f, 110.0f, 220.0f };
    bool         ok         = true;
    for (const float f0 : pitches) {
        for (uint32_t pass = 0; pass < 2; ++pass) {
            string_test_stored_sections = pass == 0 ? 1.0f : 6.0f;
            float* const out            = pass == 0 ? render_one : render_six;
            memset(&string_test_mirror, 0, sizeof(string_test_mirror));
            string_mirror_init(&string_test_mirror, f0, 0.0f, 4321, 1.0f, 1.0f);
            for (uint32_t i = 0; i < 4096; i += 256) {
                string_mirror_step(&string_test_mirror, f0, 0.0f, 0.0f, false, out + i, 256, 1.0f);
            }
        }
        string_test_stored_sections = 1.0f;
        ok                          = memcmp(render_one, render_six, sizeof(render_one)) == 0 && ok;
    }
    return ok;
}

// Release damping curve: 0.025 * 2^(x * 6.0588937) is 0.05 / T60 with T60 = 2.0 * 0.015^x, so x = 0 rings for
// about 2.0 s, x = 0.5 for about 0.245 s and x = 1 chokes in about 0.03 s.  Inputs outside [0, 1] clamp and NaN
// maps to 0.
static void check_string_release_damping_curve()
{
    const double ln2            = 0.6931471805599453;
    const double neg_log2_ratio = -log(0.015) / ln2;
    TEST(Synth::string_release_damping_curve(0.0f) == 0.025f);
    TEST(fabs(static_cast<double>(Synth::string_release_damping_curve(1.0f)) - 0.05 / 0.03) < 0.003);
    TEST(fabs(static_cast<double>(Synth::string_release_damping_curve(0.5f)) - 0.05 / (2.0 * sqrt(0.015))) < 0.002);
    for (uint32_t i = 0; i <= 100; ++i) {
        const double x        = static_cast<double>(i) / 100.0;
        const double expected = 0.025 * exp2(x * neg_log2_ratio);
        const double actual   = static_cast<double>(Synth::string_release_damping_curve(static_cast<float>(x)));
        TEST(fabs(actual / expected - 1.0) < 0.01); // mstd::exp2 is a cubic approximation
    }
    float previous = Synth::string_release_damping_curve(0.0f);
    for (uint32_t i = 1; i <= 1000; ++i) {
        const float value = Synth::string_release_damping_curve(static_cast<float>(i) / 1000.0f);
        TEST(value > previous);
        previous = value;
    }
    TEST(Synth::string_release_damping_curve(-0.5f) == Synth::string_release_damping_curve(0.0f));
    TEST(Synth::string_release_damping_curve(-INFINITY) == Synth::string_release_damping_curve(0.0f));
    TEST(Synth::string_release_damping_curve(1.5f) == Synth::string_release_damping_curve(1.0f));
    TEST(Synth::string_release_damping_curve(INFINITY) == Synth::string_release_damping_curve(1.0f));
    TEST(Synth::string_release_damping_curve(NAN) == Synth::string_release_damping_curve(0.0f));
}

// Dispersion as a modulation target: a slow LFO sweeps the per-block dispersion between 0.05 and 0.95 over a
// 4 s period.  The retune subtracts the cascade phase delay every block, so the fundamental holds at the
// sampled points while the 4th-partial stretch follows the modulation.
static void check_string_dispersion_modulation()
{
    constexpr float    f0            = 110.0f;
    constexpr uint32_t blocks        = string_test_samples / 256;
    const auto         dispersion_at = [](uint32_t block) {
        const double t = static_cast<double>(block) * 256.0 / 44100.0;
        return static_cast<float>(0.5 + 0.45 * sin(2.0 * vmath::pi_double * t / 4.0));
    };
    memset(&string_test_mirror, 0, sizeof(string_test_mirror));
    string_mirror_init(&string_test_mirror, f0, 0.0f, 4321, 1.0f, dispersion_at(0));
    for (uint32_t b = 0; b < blocks; ++b) {
        string_mirror_step(&string_test_mirror,
                           f0,
                           0.0f,
                           0.0f,
                           false,
                           string_test_out + b * 256,
                           256,
                           dispersion_at(b));
    }
    for (uint32_t i = 0; i < string_test_samples; ++i) {
        TEST(std::isfinite(string_test_out[i]));
    }
    // Windows centred on t = 0.6, 1.0 (LFO peak), 2.0, 3.0 (LFO trough) seconds.
    const uint32_t window     = 16384;
    const double   centres[4] = { 0.6, 1.0, 2.0, 3.0 };
    double         stretch[4] = {};
    for (uint32_t w = 0; w < 4; ++w) {
        const uint32_t start  = static_cast<uint32_t>(centres[w] * 44100.0) - window / 2;
        double         energy = 0.0;
        for (uint32_t i = start; i < start + window; ++i) {
            energy += static_cast<double>(string_test_out[i]) * static_cast<double>(string_test_out[i]);
        }
        TEST(energy > 1.0e-3);
        const double f1 = string_peak_freq(string_test_out, start, window, static_cast<double>(f0), 0.66);
        TEST(fabs(1200.0 * log2(f1 / static_cast<double>(f0))) < 3.0);
        const double p4 = string_peak_freq(string_test_out, start, window, 4.0 * f1, 8.0);
        stretch[w]      = 1200.0 * log2(p4 / (4.0 * f1));
    }
    // d is about 0.95 at t = 1 s and 0.05 at t = 3 s: about 14 cents (single section, 15 cents at d = 1)
    // versus about 1 cent on the 4th partial.
    TEST(stretch[1] > stretch[3] + 8.0);
    TEST(stretch[1] > 10.0);
    TEST(stretch[3] < 6.0);
    TEST(stretch[1] > stretch[0] && stretch[0] > stretch[3]);
}

// A routed String Dispersion (modulation block under the same JSON key) round-trips as an object and an
// unrouted one as a plain number.
static void check_string_dispersion_routed_codec()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Synth::Oscillator& layer         = instrument.layers[0];
    layer.osc_type[0]                = Synth::WaveType::string_wave;
    layer.fir_band_count             = 1;
    Synth::TargetBinding& routed     = layer.targets[Synth::mod_string_dispersion];
    routed.base_value                = 0.4f;
    routed.envelope_depth            = 1.0f;
    routed.lfo_desc_id               = 1;
    routed.lfo_op                    = Synth::SourceOp::add;
    routed.lfo_depth                 = 0.25f;
    bank.bank.instruments.entries[0] = instrument;
    TEST(Synth::validate_instrument_bank(&bank.bank));
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    uint32_t length                             = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);
    TEST(strstr(payload, "\"string_dispersion\":{") != nullptr);
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
    const Synth::Oscillator& dec =
        decoded.bank.instruments.entries[decoded.bank.channel_zones[0][0].instrument].layers[0];
    TEST(dec.targets[Synth::mod_string_dispersion].base_value == 0.4f);
    TEST(dec.targets[Synth::mod_string_dispersion].lfo_desc_id == 1);
    TEST(dec.targets[Synth::mod_string_dispersion].lfo_depth == 0.25f);
    // Unrouted again: the same value encodes as a number, and the object form is gone.
    bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion]            = {};
    bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = 0.4f;
    length = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(length > 0);
    TEST(strstr(payload, "\"string_dispersion\":0.4") != nullptr);
    TEST(strstr(payload, "\"string_dispersion\":{") == nullptr);
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
    const Synth::TargetBinding& plain = decoded.bank.instruments.entries[decoded.bank.channel_zones[0][0].instrument]
                                            .layers[0]
                                            .targets[Synth::mod_string_dispersion];
    TEST(plain.base_value == 0.4f && plain.lfo_desc_id == 0 && plain.num_inputs == 0);
    // Routed with an out-of-range base is refused by the validator.
    bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion]            = routed;
    bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_string_dispersion].base_value = 1.5f;
    TEST(! Synth::validate_instrument_bank(&bank.bank));
}

static void check_string_mirror_dispersion()
{
    static StringMirror a;
    static StringMirror b;
    static StringMirror legacy;
    static float        out_a[2048];
    static float        out_b[2048];
    static float        out_l[2048];
    static float        ring_l[string_ring_size];
    // (a) d = 0 is bit-identical to the frozen legacy loop.  At damping 0.5: 441 Hz gives N = 98 and 400 Hz N = 108
    // (even), 420 Hz gives N = 103 (odd).
    const float legacy_freqs[3] = { 441.0f, 400.0f, 420.0f };
    for (const float f0 : legacy_freqs) {
        const StringScalars plain = string_mirror_scalars(f0, 0.5f, 0.0f);
        TEST(plain.n % 2 == (f0 == 420.0f ? 1u : 0u));
        const StringScalars gated = string_mirror_scalars(f0, 0.5f, 0.0f, 0.0f);
        TEST(gated.num_sections == 0 && gated.disp_delay == 0.0f && gated.n == plain.n && gated.a == plain.a);
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&legacy, 0, sizeof(legacy));
        string_mirror_init(&a, f0, 0.5f, 55, 1.0f, 0.0f);
        string_mirror_init(&b, f0, 0.5f, 55);
        string_mirror_init(&legacy, f0, 0.5f, 55);
        string_mirror_step(&a, f0, 0.5f, 0.0f, false, out_a, 2048, 0.0f);
        string_mirror_step(&b, f0, 0.5f, 0.0f, false, out_b, 2048);
        for (uint32_t i = 0; i < 2048; i += 256) {
            string_legacy_step(&legacy, f0, 0.5f, out_l + i, 256);
        }
        TEST(memcmp(out_a, out_l, sizeof(out_a)) == 0 && memcmp(out_b, out_l, sizeof(out_b)) == 0);
        memcpy(ring_l, legacy.ring, sizeof(ring_l));
        TEST(memcmp(a.ring, ring_l, sizeof(ring_l)) == 0 && memcmp(b.ring, ring_l, sizeof(ring_l)) == 0);
        TEST(a.ds[0] == 0.0f);
    }
    // (b) Tuning and (c) stretch vs the oracle, over d x f0; the 4th-partial stretch is also near 15 cents * d and
    // pitch-invariant across [55, 880] Hz.
    const float disps[3]   = { 0.01f, 0.5f, 1.0f };
    const float pitches[4] = { 55.0f, 220.0f, 440.0f, 880.0f };
    for (const float d : disps) {
        double lowest   = 1e9;
        double highest  = -1e9;
        double lowest2  = 1e9;
        double highest2 = -1e9;
        double lowest3  = 1e9;
        double highest3 = -1e9;
        for (const float f0 : pitches) {
            double          stretch = 0.0;
            StringDispExtra extra   = {};
            TEST(string_disp_case(f0, d, &stretch, &extra));
            TEST(stretch > 0.0 && extra.stretch_p2 > 0.0 && extra.stretch_p3 > 0.0);
            TEST(stretch > extra.stretch_p3 && extra.stretch_p3 > extra.stretch_p2);
            // Gate boundary just above zero: the stretch is small, positive and matches the oracle.
            if (d == 0.01f) {
                // Small and positive overall.  Above 440 Hz the fractional-allpass re-quantization of the loop delay
                // (measured up to +-3 cents) swamps the increment, so the increment over the d = 0 oracle is only
                // bounded below that.
                TEST(stretch < 4.0);
                if (f0 <= 440.0f) {
                    double flat[5];
                    double tiny[5];
                    string_disp_oracle(static_cast<double>(f0), 0.0, 0.0, flat);
                    string_disp_oracle(static_cast<double>(f0), 0.01, 0.0, tiny);
                    const double increment = 1200.0 * log2((tiny[4] / tiny[1]) / (flat[4] / flat[1]));
                    TEST(increment > 0.05 && increment < 0.4);
                }
                TEST(fabs(stretch - extra.oracle_stretch) < 1.0);
            }
            lowest   = fmin(lowest, stretch);
            highest  = fmax(highest, stretch);
            lowest2  = fmin(lowest2, extra.stretch_p2);
            highest2 = fmax(highest2, extra.stretch_p2);
            lowest3  = fmin(lowest3, extra.stretch_p3);
            highest3 = fmax(highest3, extra.stretch_p3);
            if (d == 1.0f) {
                TEST(fabs(stretch - 15.0 * static_cast<double>(d)) < 6.0);
            }
        }
        TEST(highest - lowest < 4.5);
        // Partials 2 and 3 stretch less (about (k^2 - 1) / 15 of partial 4) and stay pitch invariant.
        TEST(highest2 - lowest2 < 1.0);
        TEST(highest3 - lowest3 < 2.5);
    }
    // (d) The stored section count is inert (mutation 6 breaks it).
    TEST(string_disp_sections_inert());
    string_disp_mutation = 6;
    TEST(! string_disp_sections_inert());
    string_disp_mutation        = 0;
    string_test_stored_sections = 1.0f;

    // (e) Fallback sweep: scalars stay in domain over a wide f0 range, the single section is used unreduced inside
    // [55, 880] Hz, and a held-note pitch sweep up and down stays finite, bounded and audible.
    for (float f0 = 22.0f; f0 < 8000.0f; f0 *= 1.01f) {
        for (const float d : disps) {
            const StringScalars s = string_mirror_scalars(f0, 0.5f, 0.0f, d);
            TEST(std::isfinite(s.disp_a) && s.num_sections <= 1 && s.n >= 2 && s.n < string_ring_size);
            TEST(s.disp_delay >= 0.0f && std::isfinite(s.a) && fabsf(s.a) < 1.0f);
            if (f0 >= 55.0f && f0 <= 880.0f) {
                TEST(s.num_sections == 1);
            }
        }
    }
    memset(&a, 0, sizeof(a));
    string_mirror_init(&a, 880.0f, 0.5f, 7, 1.0f, 1.0f);
    double tail_energy = 0.0;
    for (uint32_t block = 0; block < 100; ++block) {
        const float phase = static_cast<float>(block < 50 ? block : 100 - block) / 50.0f;
        const float f0    = 880.0f * powf(3000.0f / 880.0f, phase);
        string_mirror_step(&a, f0, 0.5f, 0.0f, false, out_a, 256, 1.0f);
        for (uint32_t i = 0; i < 256; ++i) {
            TEST(std::isfinite(out_a[i]) && fabsf(out_a[i]) < 8.0f);
            tail_energy += block >= 99 ? static_cast<double>(out_a[i]) * static_cast<double>(out_a[i]) : 0.0;
        }
    }
    TEST(tail_energy > 0.0);
    // Settled tuning after fallback: the note is held through a sweep into the fallback region (where the section is
    // dropped) and back; once settled, the fundamental is in tune whenever the resulting pitch is inside [55, 880] Hz.
    // Outside that domain only the finite/bounded checks in the helper apply.
    {
        const float sweeps[6][3] = { { 880.0f, 3000.0f, 880.0f },  { 55.0f, 3000.0f, 440.0f },
                                     { 220.0f, 8000.0f, 220.0f },  { 880.0f, 1500.0f, 55.0f },
                                     { 440.0f, 3000.0f, 659.26f }, { 110.0f, 8000.0f, 2000.0f } };
        for (const auto& sweep : sweeps) {
            for (const float d : { 0.01f, 1.0f }) {
                const bool in_domain = sweep[2] >= 55.0f && sweep[2] <= 880.0f;
                if (in_domain) {
                    TEST(string_disp_held_sweep(sweep[0], sweep[1], sweep[2], d));
                }
                else {
                    string_disp_held_sweep(sweep[0], sweep[1], sweep[2], d);
                }
            }
        }
    }
    // (f) Mutations 1, 2, 3 and 5 must each fail the tuning / oracle-stretch case at some pitch.
    const float mutation_pitches[3] = { 55.0f, 220.0f, 880.0f };
    for (const int mutation : { 1, 2, 3, 5 }) {
        string_disp_mutation = mutation;
        bool all_ok          = true;
        for (const float f0 : mutation_pitches) {
            double stretch = 0.0;
            all_ok         = string_disp_case(f0, 1.0f, &stretch) && all_ok;
        }
        string_disp_mutation = 0;
        TEST(! all_ok);
    }
}

// ---- Additive pluck: chain-token model (M2).  The mirror models header words 8-9 as the integer-valued
// split of the slot's 32-bit chain token, exactly as the shader stores it: lo = (token & 0xFFFF) + 1 (the +1
// keeps a zeroed word from matching), hi = token >> 16.  Both are exact floats below 2^24, never NaN.
enum class ChainInit {
    none,
    replace,
    additive
};

static float string_chain_lo(uint32_t token)
{
    return static_cast<float>((token & 0xFFFFu) + 1u);
}

static float string_chain_hi(uint32_t token)
{
    return static_cast<float>(token >> 16);
}

static uint32_t string_chain_join(float lo, float hi)
{
    return (static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) - 1u);
}

static bool string_chain_matches(const StringMirror* m, uint32_t token)
{
    return m->chain_lo == string_chain_lo(token) && m->chain_hi == string_chain_hi(token);
}

// Shader init decision and body.  Nothing happens for a silent pitch or when the seed tag already matches.
// Otherwise the burst is built; it is ADDED onto the live window when the stored chain words equal the
// slot's token, and replaces the ring head otherwise.  Every init rewrites the tag and the chain words.
static ChainInit string_mirror_chain_init(StringMirror* m,
                                          uint32_t      token,
                                          float         f0,
                                          float         damping,
                                          uint32_t      seed,
                                          float         brightness,
                                          float         dispersion)
{
    const StringScalars s = string_mirror_scalars(f0, damping, 0.0f, dispersion);
    if (s.silent || (m->tagged && m->tag == seed)) {
        return ChainInit::none;
    }
    static float   burst[string_ring_size];
    const uint32_t n        = string_mirror_burst(burst, f0, damping, seed, brightness, dispersion);
    const bool     additive = string_chain_matches(m, token);
    if (additive) {
        for (uint32_t i = 0; i < n; ++i) {
            m->ring[(m->wr + i + (string_ring_size - n)) % string_ring_size] += burst[i];
        }
        m->e = 1.0f;
    }
    else {
        for (uint32_t i = 0; i < n; ++i) {
            m->ring[i] = burst[i];
        }
        m->wr = n;
        m->t1 = 0.0f;
        m->u1 = 0.0f;
        m->e  = 1.0f;
        for (uint32_t k = 0; k < string_max_sections; ++k) {
            m->ds[k] = 0.0f;
        }
    }
    m->tagged   = true;
    m->tag      = seed;
    m->chain_lo = string_chain_lo(token);
    m->chain_hi = string_chain_hi(token);
    return additive ? ChainInit::additive : ChainInit::replace;
}

struct ChainSlot {
    uint32_t        token;
    Synth::WaveType type0;
    Synth::WaveType type1;
    uint32_t        mode;
};

static bool chain_slot_string_active(const ChainSlot* slot)
{
    return slot->type0 == Synth::WaveType::string_wave &&
           ! (slot->type1 != Synth::WaveType::no_wave && slot->mode == Synth::osc_mode_hard_sync);
}

static void chain_slot_note_on(ChainSlot*      slot,
                               bool            reset_voice,
                               uint32_t        seed,
                               Synth::WaveType type0,
                               Synth::WaveType type1,
                               uint32_t        mode)
{
    const bool chain_continues = reset_voice && chain_slot_string_active(slot);
    slot->type0                = type0;
    slot->type1                = type1;
    slot->mode                 = mode;
    if (! chain_continues) {
        slot->token = seed;
    }
}

static void chain_claim(ChainSlot* slot, uint32_t seed)
{
    chain_slot_note_on(slot,
                       false,
                       seed,
                       Synth::WaveType::string_wave,
                       Synth::WaveType::no_wave,
                       Synth::osc_mode_blend);
}

static void chain_takeover(ChainSlot* slot, uint32_t seed)
{
    chain_slot_note_on(slot, true, seed, Synth::WaveType::string_wave, Synth::WaveType::no_wave, Synth::osc_mode_blend);
}

static constexpr float chain_damping = 0.5f;

static constexpr float chain_brightness = 0.6f;

static constexpr float chain_dispersion = 0.4f;

static constexpr float chain_f0 = 220.0f;

// Seed of the k-th production layer note-on (noise_start_counter starts at 0, stride 0x9e3779b9).
static uint32_t chain_seed(uint32_t k)
{
    return k * 0x9e3779b9u;
}

// Init only, as the shader does it when the layer is string-active (otherwise the pass is skipped).
static ChainInit chain_init_only(StringMirror* m, const ChainSlot* slot, uint32_t seed, float f0 = chain_f0)
{
    if (! chain_slot_string_active(slot)) {
        return ChainInit::none;
    }
    return string_mirror_chain_init(m, slot->token, f0, chain_damping, seed, chain_brightness, chain_dispersion);
}

// One whole dispatch: init when due, then 256 output samples.
static ChainInit chain_dispatch(StringMirror*    m,
                                const ChainSlot* slot,
                                uint32_t         seed,
                                float            f0      = chain_f0,
                                bool             release = false,
                                float*           out     = nullptr)
{
    static float scratch[256];
    if (! chain_slot_string_active(slot)) {
        return ChainInit::none;
    }
    const ChainInit result = chain_init_only(m, slot, seed, f0);
    string_mirror_step(m, f0, chain_damping, 0.5f, release, out ? out : scratch, 256, chain_dispersion);
    return result;
}

// Additive result is exactly ring + burst on the live window, the filter state and write position kept.
static bool chain_additive_exact(const StringMirror* before, const StringMirror* after, const float* burst, uint32_t n)
{
    const uint32_t start = (before->wr + string_ring_size - n) % string_ring_size;
    bool           ok    = true;
    for (uint32_t idx = 0; idx < string_ring_size; ++idx) {
        const uint32_t k        = (idx + string_ring_size - start) % string_ring_size;
        const float    expected = k < n ? before->ring[idx] + burst[k] : before->ring[idx];
        ok                      = ok && after->ring[idx] == expected;
    }
    ok = ok && after->wr == before->wr && after->t1 == before->t1 && after->u1 == before->u1 && after->e == 1.0f;
    for (uint32_t k = 0; k < string_max_sections; ++k) {
        ok = ok && after->ds[k] == before->ds[k];
    }
    return ok;
}

static uint32_t chain_burst(float* burst, uint32_t seed, float f0 = chain_f0)
{
    return string_mirror_burst(burst, f0, chain_damping, seed, chain_brightness, chain_dispersion);
}

// (a) Same-batch takeover chains.  Single mode and reuse-per-note share the slot policy: two shapes.
// Test-only chain policy model; production runtime/shader execution is not exercised here.
static void check_string_chain_same_batch()
{
    static StringMirror m;
    static StringMirror before;
    static float        burst[string_ring_size];
    for (uint32_t shape = 0; shape < 2; ++shape) {
        memset(&m, 0, sizeof(m));
        ChainSlot      slot = {};
        const uint32_t a    = chain_seed(3 + shape);
        chain_claim(&slot, a);
        TEST(chain_dispatch(&m, &slot, a) == ChainInit::replace);
        for (uint32_t b = 0; b < 3; ++b) {
            chain_dispatch(&m, &slot, a);
        }
        chain_dispatch(&m, &slot, a, chain_f0, true);
        TEST(string_chain_join(m.chain_lo, m.chain_hi) == a);
        const uint32_t b = chain_seed(10);
        const uint32_t c = chain_seed(11);
        const uint32_t d = chain_seed(12);
        chain_takeover(&slot, b);
        chain_takeover(&slot, c);
        TEST(slot.token == a);
        before = m;
        TEST(before.t1 != 0.0f && before.u1 != 0.0f && before.ds[0] != 0.0f && before.e < 1.0f);
        const uint32_t n = chain_burst(burst, c);
        TEST(chain_init_only(&m, &slot, c) == ChainInit::additive);
        TEST(chain_additive_exact(&before, &m, burst, n));
        TEST(m.tag == c && string_chain_join(m.chain_lo, m.chain_hi) == a);
        chain_dispatch(&m, &slot, c);
        chain_takeover(&slot, d);
        TEST(slot.token == a);
        TEST(chain_dispatch(&m, &slot, d) == ChainInit::additive);
        TEST(m.tag == d);
    }
}

// (b) Dirty recycled slot plus a fresh note which starts below the pitch floor.
static void check_string_chain_recycled_slot()
{
    static StringMirror m;
    static StringMirror dirty;
    static StringMirror fresh;
    static float        burst[string_ring_size];
    memset(&m, 0, sizeof(m));
    ChainSlot      slot = {};
    const uint32_t x    = chain_seed(5);
    chain_claim(&slot, x);
    for (uint32_t b = 0; b < 6; ++b) {
        chain_dispatch(&m, &slot, x);
    }
    dirty            = m;
    const uint32_t a = chain_seed(6);
    chain_claim(&slot, a);
    TEST(slot.token == a && slot.token != x);
    TEST(chain_dispatch(&m, &slot, a, 10.0f) == ChainInit::none);
    TEST(memcmp(&dirty, &m, sizeof(m)) == 0);
    const uint32_t b = chain_seed(7);
    chain_takeover(&slot, b);
    TEST(slot.token == a);
    const uint32_t n = chain_burst(burst, b);
    TEST(chain_init_only(&m, &slot, b) == ChainInit::replace);
    for (uint32_t i = 0; i < n; ++i) {
        TEST(m.ring[i] == burst[i]);
    }
    TEST(m.wr == n && m.t1 == 0.0f && m.u1 == 0.0f && m.e == 1.0f);
    for (uint32_t k = 0; k < string_max_sections; ++k) {
        TEST(m.ds[k] == 0.0f);
    }
    TEST(string_chain_join(m.chain_lo, m.chain_hi) == a);
    // Same as the legacy replace over the same dirty ring: no prior tail inside the window.
    fresh = dirty;
    string_mirror_init(&fresh, chain_f0, chain_damping, b, chain_brightness, chain_dispersion);
    TEST(memcmp(fresh.ring, m.ring, sizeof(fresh.ring)) == 0);
}

// (c) A chord note which is claimed and taken over before it ever renders.
static void check_string_chain_never_rendered()
{
    static StringMirror m;
    memset(&m, 0, sizeof(m));
    ChainSlot slot = {};
    chain_claim(&slot, chain_seed(20));
    chain_takeover(&slot, chain_seed(21));
    TEST(slot.token == chain_seed(20));
    TEST(chain_dispatch(&m, &slot, chain_seed(21)) == ChainInit::replace);
    // The same over a dirty region whose header carries another chain.
    memset(&m, 0, sizeof(m));
    ChainSlot other = {};
    chain_claim(&other, chain_seed(2));
    for (uint32_t b = 0; b < 2; ++b) {
        chain_dispatch(&m, &other, chain_seed(2));
    }
    chain_claim(&other, chain_seed(30));
    chain_takeover(&other, chain_seed(31));
    TEST(chain_dispatch(&m, &other, chain_seed(31)) == ChainInit::replace);
}

// (d) Live-ring takeover which starts below the pitch floor, then comes into range.
static void check_string_chain_below_floor_takeover()
{
    static StringMirror m;
    static StringMirror before;
    static StringMirror frozen;
    static float        burst[string_ring_size];
    memset(&m, 0, sizeof(m));
    ChainSlot      slot = {};
    const uint32_t a    = chain_seed(8);
    chain_claim(&slot, a);
    for (uint32_t b = 0; b < 4; ++b) {
        chain_dispatch(&m, &slot, a);
    }
    const uint32_t b = chain_seed(9);
    chain_takeover(&slot, b);
    frozen = m;
    TEST(chain_dispatch(&m, &slot, b, 15.0f) == ChainInit::none);
    TEST(memcmp(&frozen, &m, sizeof(m)) == 0);
    TEST(slot.token == a && m.tag == a && string_chain_join(m.chain_lo, m.chain_hi) == a);
    before           = m;
    const uint32_t n = chain_burst(burst, b);
    TEST(chain_init_only(&m, &slot, b) == ChainInit::additive);
    TEST(chain_additive_exact(&before, &m, burst, n));
    TEST(m.t1 == before.t1 && m.u1 == before.u1 && m.ds[0] == before.ds[0]);
}

// (e) Type and hard-sync transitions: the ring is frozen while the layer is not string-active.
static void check_string_chain_type_transitions()
{
    static StringMirror m;
    static StringMirror frozen;
    using Synth::WaveType;
    for (uint32_t via = 0; via < 2; ++via) {
        memset(&m, 0, sizeof(m));
        ChainSlot      slot = {};
        const uint32_t a    = chain_seed(40);
        chain_claim(&slot, a);
        for (uint32_t b = 0; b < 3; ++b) {
            chain_dispatch(&m, &slot, a);
        }
        // string -> non-string-active layer (sine, or string in hard sync): the previous layer is
        // string-active, so the token is kept, and the pass is skipped, so the header is frozen.
        const uint32_t b = chain_seed(41);
        if (via == 0) {
            chain_slot_note_on(&slot, true, b, WaveType::sine_wave, WaveType::no_wave, Synth::osc_mode_blend);
        }
        else {
            chain_slot_note_on(&slot, true, b, WaveType::string_wave, WaveType::sine_wave, Synth::osc_mode_hard_sync);
        }
        TEST(slot.token == a);
        frozen = m;
        TEST(chain_dispatch(&m, &slot, b) == ChainInit::none);
        TEST(memcmp(&frozen, &m, sizeof(m)) == 0);
        // Back to a string: the previous layer was not string-active, so the token restarts, and the
        // frozen header chain (a) can no longer match.  A wrongly kept token would have matched.
        const uint32_t c = chain_seed(42);
        chain_takeover(&slot, c);
        TEST(slot.token == c);
        TEST(string_chain_matches(&m, a) && ! string_chain_matches(&m, slot.token));
        TEST(chain_dispatch(&m, &slot, c) == ChainInit::replace);
        TEST(string_chain_join(m.chain_lo, m.chain_hi) == c);
    }
    // string -> string keeps the token.
    memset(&m, 0, sizeof(m));
    ChainSlot slot = {};
    chain_claim(&slot, chain_seed(50));
    chain_dispatch(&m, &slot, chain_seed(50));
    chain_takeover(&slot, chain_seed(51));
    TEST(slot.token == chain_seed(50));
    TEST(chain_dispatch(&m, &slot, chain_seed(51)) == ChainInit::additive);
}

// Token lifecycle: claim resets, a string-active takeover preserves, a takeover out of a layer which is
// not string-active resets.
static void check_string_chain_token_lifecycle()
{
    using Synth::WaveType;
    ChainSlot slot = {};
    chain_claim(&slot, chain_seed(1));
    chain_takeover(&slot, chain_seed(2));
    TEST(slot.token == chain_seed(1));
    // A claim over a slot whose previous occupant held another token resets it.
    chain_claim(&slot, chain_seed(3));
    TEST(slot.token == chain_seed(3));
    // A claim ignores the stale layer fields of a recycled slot (reset_voice false).
    slot.type0 = WaveType::string_wave;
    slot.type1 = WaveType::no_wave;
    chain_claim(&slot, chain_seed(4));
    TEST(slot.token == chain_seed(4));
    // Takeover out of sine resets.
    chain_slot_note_on(&slot, true, chain_seed(5), WaveType::sine_wave, WaveType::no_wave, Synth::osc_mode_blend);
    TEST(slot.token == chain_seed(4));
    chain_takeover(&slot, chain_seed(6));
    TEST(slot.token == chain_seed(6));
    // Takeover out of string in hard sync resets.
    chain_slot_note_on(&slot,
                       true,
                       chain_seed(7),
                       WaveType::string_wave,
                       WaveType::sine_wave,
                       Synth::osc_mode_hard_sync);
    TEST(slot.token == chain_seed(6));
    chain_takeover(&slot, chain_seed(8));
    TEST(slot.token == chain_seed(8));
    // A string with a second wave in blend mode is still string-active.
    chain_slot_note_on(&slot, true, chain_seed(9), WaveType::string_wave, WaveType::sine_wave, Synth::osc_mode_blend);
    TEST(slot.token == chain_seed(8));
    chain_takeover(&slot, chain_seed(10));
    TEST(slot.token == chain_seed(8));
}

// Seed uniqueness (a model assumption), stale header chains and the NaN-pattern production seed.
static void check_string_chain_seed_uniqueness()
{
    static StringMirror m;
    static StringMirror live;
    static StringMirror snap;
    static float        burst[string_ring_size];
    constexpr uint32_t  run = 300;
    for (uint32_t i = 1; i <= run; ++i) {
        for (uint32_t j = i + 1; j <= run; ++j) {
            TEST(chain_seed(i) != chain_seed(j));
        }
    }
    // Stale chain: chain a inits, the slot is freed, another note claims the same dirty region.
    memset(&m, 0, sizeof(m));
    for (uint32_t k = 1; k <= 80; ++k) {
        ChainSlot slot = {};
        chain_claim(&slot, chain_seed(k));
        TEST(chain_dispatch(&m, &slot, chain_seed(k)) == ChainInit::replace);
        chain_takeover(&slot, chain_seed(1000 + k));
        TEST(chain_dispatch(&m, &slot, chain_seed(1000 + k)) == ChainInit::additive);
    }
    // The 72nd seed is a NaN bit pattern as a float; the integer split keeps it exact.
    const uint32_t nan_seed = chain_seed(72);
    TEST(nan_seed == 0x7f9a3c08u);
    TEST((nan_seed & 0x7f800000u) == 0x7f800000u);
    TEST(string_chain_lo(nan_seed) == 15369.0f && string_chain_hi(nan_seed) == 32666.0f);
    TEST(string_chain_lo(nan_seed) == string_chain_lo(nan_seed) &&
         string_chain_hi(nan_seed) == string_chain_hi(nan_seed));
    TEST(string_chain_join(string_chain_lo(nan_seed), string_chain_hi(nan_seed)) == nan_seed);
    const uint32_t boundary[] = { 0u, 0xFFFFu, 0x10000u, 0xFFFFFFFFu, 0x7f800000u, 0xff800000u, 0x7fc00000u };
    for (uint32_t token : boundary) {
        const float lo = string_chain_lo(token);
        const float hi = string_chain_hi(token);
        TEST(lo == lo && hi == hi && lo >= 1.0f);
        TEST(string_chain_join(lo, hi) == token);
    }
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint32_t patterns[] = { 0x7f800000u + i * 17u, 0xff800000u + i * 17u, 0x7fc00000u + i, 0x7f9a0000u + i };
        for (uint32_t token : patterns) {
            const float lo = string_chain_lo(token);
            const float hi = string_chain_hi(token);
            TEST(lo == lo && hi == hi);
            TEST(string_chain_join(lo, hi) == token);
        }
    }
    // Live-ring takeover with that seed as the chain token stays additive.
    memset(&live, 0, sizeof(live));
    ChainSlot slot = {};
    chain_claim(&slot, nan_seed);
    TEST(slot.token == nan_seed);
    TEST(chain_dispatch(&live, &slot, nan_seed) == ChainInit::replace);
    TEST(live.chain_lo == 15369.0f && live.chain_hi == 32666.0f);
    for (uint32_t b = 0; b < 3; ++b) {
        chain_dispatch(&live, &slot, nan_seed);
    }
    chain_takeover(&slot, chain_seed(73));
    chain_takeover(&slot, chain_seed(74));
    TEST(slot.token == nan_seed);
    snap             = live;
    const uint32_t n = chain_burst(burst, chain_seed(74));
    TEST(chain_init_only(&live, &slot, chain_seed(74)) == ChainInit::additive);
    TEST(chain_additive_exact(&snap, &live, burst, n));
    TEST(live.chain_lo == 15369.0f && live.chain_hi == 32666.0f);
    // A token differing in one half does not match and replaces.
    const uint32_t near_tokens[] = { 0x7f9a3c09u, 0x7f9b3c08u };
    for (uint32_t token : near_tokens) {
        snap = live;
        TEST(string_mirror_chain_init(&snap,
                                      token,
                                      chain_f0,
                                      chain_damping,
                                      chain_seed(75),
                                      chain_brightness,
                                      chain_dispersion) == ChainInit::replace);
    }
    // Sweep: each consecutive production seed is additive against its own header, replace against its
    // neighbour's.
    for (uint32_t k = 1; k <= 90; ++k) {
        memset(&snap, 0, sizeof(snap));
        ChainSlot owner = {};
        chain_claim(&owner, chain_seed(k));
        chain_dispatch(&snap, &owner, chain_seed(k));
        live = snap;
        TEST(string_mirror_chain_init(&live,
                                      chain_seed(k),
                                      chain_f0,
                                      chain_damping,
                                      chain_seed(2000 + k),
                                      chain_brightness,
                                      chain_dispersion) == ChainInit::additive);
        live = snap;
        TEST(string_mirror_chain_init(&live,
                                      chain_seed(k + 1),
                                      chain_f0,
                                      chain_damping,
                                      chain_seed(2000 + k),
                                      chain_brightness,
                                      chain_dispersion) == ChainInit::replace);
        live = snap;
        TEST(string_mirror_chain_init(&live,
                                      chain_seed(k - 1),
                                      chain_f0,
                                      chain_damping,
                                      chain_seed(2000 + k),
                                      chain_brightness,
                                      chain_dispersion) == ChainInit::replace);
    }
    // A clean region (zeroed words) never matches, whatever the token.
    memset(&m, 0, sizeof(m));
    TEST(! string_chain_matches(&m, 0u) && ! string_chain_matches(&m, 0xFFFFFFFFu) &&
         ! string_chain_matches(&m, nan_seed));
}

// Skipped or discarded work: a pass which never runs changes nothing, and the later decision follows the
// actual state with no CPU rollback.
static void check_string_chain_skipped_work()
{
    static StringMirror m;
    static StringMirror skipped;
    static StringMirror direct;
    memset(&m, 0, sizeof(m));
    ChainSlot      slot = {};
    const uint32_t a    = chain_seed(60);
    chain_claim(&slot, a);
    for (uint32_t b = 0; b < 3; ++b) {
        chain_dispatch(&m, &slot, a);
    }
    const uint32_t b = chain_seed(61);
    chain_takeover(&slot, b);
    // Discarded batch: the pass is not called.  The state is the same object afterwards, and the
    // decision made later equals the decision made directly.
    skipped = m;
    direct  = m;
    TEST(memcmp(&skipped, &m, sizeof(m)) == 0);
    TEST(chain_init_only(&skipped, &slot, b) == ChainInit::additive);
    TEST(chain_init_only(&direct, &slot, b) == ChainInit::additive);
    TEST(memcmp(&skipped, &direct, sizeof(skipped)) == 0);
    // Stale: the chain differs from the token, so the later pass replaces.
    ChainSlot stale = slot;
    stale.token     = chain_seed(99);
    skipped         = m;
    TEST(chain_init_only(&skipped, &stale, b) == ChainInit::replace);
    // Silent passes in between change nothing.
    skipped = m;
    TEST(chain_dispatch(&skipped, &slot, b, 5.0f) == ChainInit::none);
    TEST(memcmp(&skipped, &m, sizeof(m)) == 0);
    TEST(chain_init_only(&skipped, &slot, b) == ChainInit::additive);
}

// Frozen legacy oracle: expected values were produced by the unmodified replace algorithm (the pre-change
// string_mirror_init, f0 / damping / brightness / dispersion / seed per row), recorded as literals.  Columns:
// delay length n, FNV-1a of the n ring floats after init, FNV-1a of 8 dispatches of 256 output samples.
static uint32_t string_oracle_fnv(const void* data, size_t bytes)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t             h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void check_string_chain_legacy_oracle()
{
    struct Row {
        float    f0, damping, brightness, dispersion;
        uint32_t seed, n, ring_hash, out_hash;
    };

    static const Row rows[] = {
        { 441.0f, 0.5f, 1.0f, 0.0f, 0x1234u, 98, 0xd6c5fcffu, 0x68eacc1fu },
        { 110.0f, 0.300000012f, 1.0f, 0.0f, 0xcafef00du, 399, 0xd23ea529u, 0x8ce43b1bu },
        { 220.0f, 0.5f, 0.5f, 0.0f, 0x4du, 199, 0xb3e18b47u, 0x482c2aa7u },
        { 82.4100037f, 0.200000003f, 0.25f, 0.400000006f, 0xdeadbeefu, 509, 0x216ae908u, 0x85cfb720u },
        { 880.0f, 0.800000012f, 1.0f, 0.600000024f, 0x7f9a3c08u, 45, 0x0b17ebaau, 0x20f5ce80u },
        { 330.0f, 0.600000024f, 0.899999976f, 0.300000012f, 0x10000u, 126, 0x0e608697u, 0xc864e597u },
    };
    static StringMirror m;
    static float        out[2048];
    for (const Row& row : rows) {
        for (uint32_t path = 0; path < 3; ++path) {
            memset(&m, 0, sizeof(m));
            if (path == 0) {
                string_mirror_init(&m, row.f0, row.damping, row.seed, row.brightness, row.dispersion);
            }
            else if (path == 1) {
                // Fresh note through the chain-aware init: stale chain words, so it replaces.
                TEST(string_mirror_chain_init(&m,
                                              0x55u,
                                              row.f0,
                                              row.damping,
                                              row.seed,
                                              row.brightness,
                                              row.dispersion) == ChainInit::replace);
            }
            else {
                // Zero ring with a matching chain: additive equals replace, sample for sample.
                m.chain_lo = string_chain_lo(0x55u);
                m.chain_hi = string_chain_hi(0x55u);
                m.wr       = row.n;
                TEST(string_mirror_chain_init(&m,
                                              0x55u,
                                              row.f0,
                                              row.damping,
                                              row.seed,
                                              row.brightness,
                                              row.dispersion) == ChainInit::additive);
            }
            TEST(string_mirror_scalars(row.f0, row.damping, 0.0f, row.dispersion).n == row.n);
            TEST(m.wr == row.n);
            TEST(string_oracle_fnv(m.ring, row.n * sizeof(float)) == row.ring_hash);
            for (uint32_t b = 0; b < 8; ++b) {
                string_mirror_step(&m, row.f0, row.damping, 0.0f, false, out + b * 256, 256, row.dispersion);
            }
            TEST(string_oracle_fnv(out, sizeof(out)) == row.out_hash);
        }
    }
}

// Widened ring index: with the write position at 0 the window wraps through zero.  The unwidened form
// (write_pos + i - n in 32 bit unsigned) differs because 2^32 mod 2034 = 1372.
static void check_string_chain_widened_index()
{
    static StringMirror m;
    static float        burst[string_ring_size];
    memset(&m, 0, sizeof(m));
    const uint32_t n = chain_burst(burst, chain_seed(70));
    TEST(n > 0 && n < string_ring_size);
    m.chain_lo = string_chain_lo(0x77u);
    m.chain_hi = string_chain_hi(0x77u);
    m.wr       = 0;
    TEST(string_mirror_chain_init(&m,
                                  0x77u,
                                  chain_f0,
                                  chain_damping,
                                  chain_seed(70),
                                  chain_brightness,
                                  chain_dispersion) == ChainInit::additive);
    TEST(m.wr == 0);
    for (uint32_t i = 0; i < n; ++i) {
        TEST(m.ring[(0 + i + (string_ring_size - n)) % string_ring_size] == burst[i]);
    }
    for (uint32_t idx = 0; idx < string_ring_size - n; ++idx) {
        TEST(m.ring[idx] == 0.0f);
    }
    const uint32_t unwidened = static_cast<uint32_t>(0u + 0u - n) % string_ring_size;
    TEST(unwidened != (string_ring_size - n) % string_ring_size);
    TEST((0x100000000ull % string_ring_size) == 1372);
}

// Ringing-string continuity: the first lap after an additive takeover is the old ring window plus the new
// burst, exactly, and the boundary step stays within the stream's own steps plus the pluck.
static void check_string_chain_ringing_continuity()
{
    static StringMirror m;
    static StringMirror replace_m;
    static StringMirror snap;
    static float        pre[2048];
    static float        post_add[512];
    static float        post_rep[512];
    static float        burst[string_ring_size];
    memset(&m, 0, sizeof(m));
    ChainSlot      slot = {};
    const uint32_t a    = chain_seed(80);
    chain_claim(&slot, a);
    for (uint32_t b = 0; b < 8; ++b) {
        chain_dispatch(&m, &slot, a, chain_f0, false, pre + b * 256);
    }
    snap             = m;
    const uint32_t b = chain_seed(81);
    chain_takeover(&slot, b);
    const uint32_t n = chain_burst(burst, b);
    TEST(n <= 512);
    float old_peak = 0.0f;
    float pluck    = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float old = snap.ring[(snap.wr + i + (string_ring_size - n)) % string_ring_size];
        old_peak        = fmaxf(old_peak, fabsf(old));
        pluck           = fmaxf(pluck, fabsf(burst[i]));
    }
    TEST(old_peak > 0.0f && pluck > 0.0f);
    replace_m       = m;
    ChainSlot stale = slot;
    stale.token     = chain_seed(999);
    chain_dispatch(&m, &slot, b, chain_f0, false, post_add);
    chain_dispatch(&replace_m, &stale, b, chain_f0, false, post_rep);
    for (uint32_t i = 0; i < n; ++i) {
        const float old = snap.ring[(snap.wr + i + (string_ring_size - n)) % string_ring_size];
        TEST(post_add[i] == old + burst[i]);
        TEST(post_rep[i] == burst[i]);
    }
    const float prior_step = string_max_step(pre, 1024, 2048);
    TEST(fabsf(post_add[0] - pre[2047]) <= prior_step + pluck);
    TEST(m.e <= 1.0f && m.tag == b && string_chain_join(m.chain_lo, m.chain_hi) == a);
}

// True when the text after `member` up to the end of its line holds nothing but a comment and the next
// token is the closing brace of the struct, i.e. `member` is the trailing member.

static void string_chain_m2_regressions()
{
    check_string_chain_same_batch();
    check_string_chain_recycled_slot();
    check_string_chain_never_rendered();
    check_string_chain_below_floor_takeover();
    check_string_chain_type_transitions();
    check_string_chain_token_lifecycle();
    check_string_chain_seed_uniqueness();
    check_string_chain_skipped_work();
    check_string_chain_legacy_oracle();
    check_string_chain_widened_index();
    check_string_chain_ringing_continuity();
}

static void string_mirror_m2_regressions_b()
{
    check_string_mirror_release_click_free();
    check_string_mirror_release_zero_rings();
    check_string_mirror_release_lifetime();
    check_string_mirror_sustain_holds();
    check_string_mirror_dispersion();
    check_string_release_damping_curve();
    check_string_dispersion_modulation();
    check_string_dispersion_routed_codec();
    check_string_natural_path_legacy_capacity();
    check_string_mirror_brightness();
    check_string_mirror_fm_ring_read();
    check_string_mirror_ring_read_edges();
    check_string_fm_layout_characterization();
    check_string_mirror_nan_guard();
    check_string_mirror_damping_clamp();
    check_string_mirror_silent_onset_init();
}

// Compiled slot indices for the string rows and shifted FIR band blocks.
static void check_string_slot_layout()
{
    TEST(Sculptor::osc_band_first_slot == 150);
    TEST(Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride == 205);
    TEST(Sculptor::osc_band_header_slot(3) + Sculptor::osc_band_block_stride == 370);
    TEST(Sculptor::osc_string_extras_first_slot == 28);
    TEST(Sculptor::osc_brightness_slot == 41);
    TEST(Sculptor::osc_target_row_slot[Synth::mod_string_dispersion] == 2);
    TEST(Sculptor::osc_target_row_slot[Synth::mod_string_damping] == 15);
    TEST(Sculptor::osc_target_row_slot[Synth::mod_string_release_damping] == 28);
}

int main()
{
    test_silence_stdout();
    check_string_slot_layout();
    string_wave_m2_regressions();
    string_mirror_m2_regressions_a();
    string_mirror_m2_regressions_b();
    string_chain_m2_regressions();
    check_string_brightness_trap_decode_compile();
    return test_exit_code();
}
