// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

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
    "\"panning\":{\"base\":0.5},\"fir_pitch_spread\":{\"base\":3}}],";

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

// The three records a copy of the clipboard fixture's zone carries: the sum node
// (canonical numbering is input 0, sum 1, oscillator layer n = 2 + n, so this
// one-layer zone owns no canonical index above 2), the volume envelope, the
// volume LFO with its bound source pair.
const char clipboard_doc_records[] = "{\"kind\":0,\"canonical_index\":1,\"x\":64,\"y\":32,\"width\":0,\"height\":0},"
                                     "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":120.5,\"y\":44.25,\"width\":220,"
                                     "\"height\":120},"
                                     "{\"kind\":2,\"layer\":0,\"target\":0,\"depth_source\":4,\"rate_source\":2,"
                                     "\"x\":12.5,\"y\":-8.5,\"width\":0,\"height\":0}";

static void sweep_tooltips(const Sculptor::Graph& graph,
                           TooltipExempt          exempt_slot,
                           void*                  user_data,
                           uint32_t*              required_missing,
                           uint32_t*              exempt_nonnull)
{
    *required_missing = 0;
    *exempt_nonnull   = 0;
    for (uint32_t node_idx = 0; node_idx < Sculptor::max_nodes; ++node_idx) {
        if (! graph.node_occupied(node_idx)) {
            continue;
        }

        const Sculptor::Node& node = graph.node(node_idx);
        for (uint32_t slot_idx = 0; slot_idx < node.slots.num_allocated; ++slot_idx) {
            const Sculptor::Slot& slot   = node.slots.entries[slot_idx];
            bool                  exempt = false;
            if (exempt_slot != nullptr) {
                exempt = exempt_slot(node_idx, slot, user_data);
            }
            if (exempt) {
                if (slot.tooltip != nullptr) {
                    ++*exempt_nonnull;
                }
            }
            else if (slot.tooltip == nullptr || slot.tooltip[0] == '\0') {
                ++*required_missing;
            }
        }
    }
}

// Oscillator-pane exemptions: the sum node Layer rows, the Band N header
// label rows and the band rows that the descriptors leave without text
// under the off/lowpass/highpass filter types.
static bool osc_tooltip_exempt(uint32_t node_idx, const Sculptor::Slot& slot, void* user_data)
{
    const Sculptor::OscGraphMapping& mapping = *static_cast<const Sculptor::OscGraphMapping*>(user_data);
    if (node_idx == mapping.output_node && strncmp(slot.name, "Layer ", 6) == 0) {
        return true;
    }

    if (strncmp(slot.name, "Band ", 5) == 0) {
        return true;
    }

    if (node_idx == mapping.osc_nodes[0] &&
        (strcmp(slot.name, "Filter Off") == 0 || strcmp(slot.name, "FIR Slope") == 0 ||
         strcmp(slot.name, "Offset") == 0 || strcmp(slot.name, "Gain") == 0)) {
        return true;
    }

    if ((node_idx == mapping.osc_nodes[2] || node_idx == mapping.osc_nodes[3]) &&
        (strcmp(slot.name, "Offset") == 0 || strcmp(slot.name, "Gain") == 0)) {
        return true;
    }

    return false;
}

// Effect-pane exemptions: the In/Out dots of the effect nodes themselves;
// the chain endpoint nodes dots stay required. The FIR effect node's band
// header label rows ("Band N", only projected once the
// fx_band_region_projected gate opens for the Multi-band FIR effect) stay
// exempt like the oscillator pane's.
static bool fx_tooltip_exempt(uint32_t node_idx, const Sculptor::Slot& slot, void* user_data)
{
    const Sculptor::EffectGraphMapping& mapping = *static_cast<const Sculptor::EffectGraphMapping*>(user_data);
    for (uint32_t chain_slot = 0; chain_slot < Synth::max_chain_effects; ++chain_slot) {
        if (mapping.effect_nodes[chain_slot] == node_idx &&
            (strcmp(slot.name, "In") == 0 || strcmp(slot.name, "Out") == 0)) {
            return true;
        }
    }

    if (strncmp(slot.name, "Band ", 5) == 0) {
        return true;
    }

    return false;
}

// Index of the first portable record matching a locator, where a field passed as
// 0xFF is a wildcard, or -1 when no record matches.
static int32_t find_portable(const Synth::InstrumentGraphLayout* records,
                             uint32_t                            count,
                             uint32_t                            kind,
                             uint32_t                            layer,
                             uint32_t                            target)
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
        return static_cast<int32_t>(i);
    }
    return -1;
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
    const std::unique_ptr<Synth::InstrumentBank> probe_bank_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       probe_bank         = *probe_bank_storage;
    memset(static_cast<void*>(&probe_bank), 0, sizeof(probe_bank));
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
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument             = {};
    instrument->layer_count = Synth::max_layers;
    for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
        Synth::Oscillator& osc = instrument->layers[layer];
        osc.osc_type[0]        = Synth::WaveType::sine_wave;
        osc.osc_type[1]        = Synth::WaveType::sawtooth_wave;
        osc.osc_mode           = static_cast<Synth::OscMode>(layer % 3);
        osc.mod_ratio          = 1.0f + 0.25f * static_cast<float>(layer);
        // All 26 targets carry live bindings, so the full band count keeps the
        // band rows out of the dormant-skip path.
        osc.fir_band_count = 4;
        for (uint32_t cell = 0; cell < Synth::num_mod_targets; cell++) {
            const Synth::ModTarget target = static_cast<Synth::ModTarget>(cell);

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

            Synth::TargetBinding& gen = osc.targets[target];
            gen.envelope_desc_id      = static_cast<uint16_t>(env_slot + 1);
            gen.lfo_desc_id           = static_cast<uint16_t>(lfo_slot + 1);
            gen.lfo_op                = ((layer + cell) % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
            gen.lfo_depth             = 0.25f * static_cast<float>(layer + 1);
            gen.lfo_depth_source      = static_cast<Synth::ModSource>(1 + ((layer + cell) % 6));
            gen.lfo_rate_source       = static_cast<Synth::ModSource>(1 + ((layer + 2 * cell + 1) % 6));
            gen.lfo_rate_scale_ms     = 10.0f * static_cast<float>(layer + 1) + static_cast<float>(cell);
        }
    }
    for (uint32_t cell = 0; cell < Synth::num_mod_targets; cell++) {
        const Synth::ModTarget target  = static_cast<Synth::ModTarget>(cell);
        Synth::TargetBinding&  routing = instrument->layers[0].targets[target];
        routing.base_value = target == Synth::mod_volume       ? 0.75f
                             : target == Synth::mod_pitch      ? 0.0f
                             : target == Synth::mod_panning    ? 0.5f
                             : target == Synth::mod_fm_ratio   ? 1.5f
                             : target == Synth::mod_fir_spread ? 3.0f
                             : target == Synth::mod_fir_slope  ? 37.0f
                             : (target == Synth::mod_fir_band0_gain || target == Synth::mod_fir_band1_gain ||
                                target == Synth::mod_fir_band2_gain || target == Synth::mod_fir_band3_gain)
                                 ? 0.4f + 0.1f * static_cast<float>((target - Synth::mod_fir_band0_gain) / 4)
                             : (target == Synth::mod_fir_band1_slope || target == Synth::mod_fir_band2_slope ||
                                target == Synth::mod_fir_band3_slope)
                                 ? 90.0f - 10.0f * static_cast<float>((target - Synth::mod_fir_band1_slope) / 4)
                             : (target == Synth::mod_fir_band1_spread || target == Synth::mod_fir_band2_spread ||
                                target == Synth::mod_fir_band3_spread)
                                 ? 4.0f + static_cast<float>((target - Synth::mod_fir_band1_spread) / 4)
                             : (target == Synth::mod_string_damping || target == Synth::mod_string_release_damping ||
                                target == Synth::mod_string_dispersion)
                                 ? 0.5f
                                 : 2.0f + static_cast<float>(target);
        routing.num_inputs = 2;
        routing.inputs[0].source = Synth::ModSource::velocity;
        routing.inputs[0].op     = Synth::SourceOp::add;
        routing.inputs[0].scale  = 0.5f;
        routing.inputs[1].source = Synth::ModSource::mod_wheel;
        routing.inputs[1].op     = Synth::SourceOp::multiply;
        routing.inputs[1].scale  = -0.25f;
    }
    instrument->layers[0].targets[Synth::mod_duty0].base_value     = 0.25f;
    instrument->layers[0].targets[Synth::mod_duty1].base_value     = 0.5f;
    instrument->layers[0].targets[Synth::mod_osc_mix].base_value   = 0.75f;
    instrument->layers[0].targets[Synth::mod_fm_index].base_value  = 0.5f * 6.2831853f;
    instrument->layers[0].targets[Synth::mod_pad_tilt0].base_value = 0.25f;
    instrument->layers[0].targets[Synth::mod_pad_tilt1].base_value = -0.5f;
    fan_out_routing(*instrument);
    instrument->note_skew_semitones  = 0.25f;
    instrument->layer_skew_semitones = 0.5f;

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
        for (uint32_t cell = 0; cell < Synth::num_mod_targets; cell++) {
            const Synth::ModTarget      target = static_cast<Synth::ModTarget>(cell);
            const Synth::TargetBinding& gen    = instrument->layers[layer].targets[target];
            const uint32_t              seed   = layer * 5u + cell;

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
        }
    }
    TEST(count == max_portable_records);
    return count;
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

// Assembles one instrument document.  The document's closing brace is emitted
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

static void check_shared_instrument_json_domains()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                                snapshot         = *snapshot_storage;
    const std::unique_ptr<ClipboardDecode>                      decoded_storage  = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                                            decoded          = *decoded_storage;
    const std::unique_ptr<ClipboardDecode>                      sentinel_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                                            sentinel         = *sentinel_storage;
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text)[clipboard_capacity] = text_storage->elements;
    const char* invalid[]            = {
        "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mod_ratio\":1e999}]}",
        "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"bad\"}]}",
        "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"sources\":[{\"source\":\"bad\"}]}}]}"
    };
    const Synth::InstrumentJsonDomain domains[] = { Synth::InstrumentJsonDomain::clipboard,
                                                    Synth::InstrumentJsonDomain::validated_bank };
    for (Synth::InstrumentJsonDomain domain : domains) {
        for (const char* document : invalid) {
            fill_clipboard_sentinels(&decoded);
            fill_clipboard_sentinels(&sentinel);
            TEST(! Synth::decode_instrument_json_for_domain(document,
                                                            static_cast<uint32_t>(strlen(document)),
                                                            &decoded.instr,
                                                            decoded.envs,
                                                            &decoded.env_count,
                                                            decoded.lfos,
                                                            &decoded.lfo_count,
                                                            decoded.layout,
                                                            max_portable_records,
                                                            &decoded.layout_count,
                                                            domain));
            TEST(clipboard_sentinels_intact(&decoded, &sentinel));
        }
        build_named_fm_fixture(&source);
        snapshot = source;
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) != 0);
        source.bank.instruments.entries[0].layers[0].mod_ratio = NAN;
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == 0);
        source                                                   = snapshot;
        source.bank.instruments.entries[0].layers[0].osc_type[0] = static_cast<Synth::WaveType>(255);
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == 0);
        source = snapshot;
        source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].source =
            static_cast<Synth::ModSource>(255);
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == 0);
    }
    // Titles are bounded UTF-8 strings in both transport domains.
    const char*                                                 title_path = "synth_invalid_title.tmp";
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_before_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> file_before_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    const std::unique_ptr<FixedArray<char, clipboard_capacity>> file_after_storage =
        std::make_unique<FixedArray<char, clipboard_capacity>>();
    char (&text_before)[clipboard_capacity] = text_before_storage->elements;
    char (&file_before)[clipboard_capacity] = file_before_storage->elements;
    char (&file_after)[clipboard_capacity]  = file_after_storage->elements;
    for (Synth::InstrumentJsonDomain domain : domains) {
        for (uint32_t malformed = 0; malformed < 2; ++malformed) {
            build_named_fm_fixture(&source);
            remove(title_path);
            TEST(Synth::save_library_record(title_path, "Regression", "Titles", &source, 0) == 0);
            const uint32_t file_length = read_library_fixture(title_path, file_before, sizeof(file_before));
            const uint32_t valid_length =
                Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain);
            TEST(valid_length > 0);
            TEST(Synth::decode_instrument_json_for_domain(text,
                                                          valid_length,
                                                          &decoded.instr,
                                                          decoded.envs,
                                                          &decoded.env_count,
                                                          decoded.lfos,
                                                          &decoded.lfo_count,
                                                          decoded.layout,
                                                          max_portable_records,
                                                          &decoded.layout_count,
                                                          domain));
            char title[32] = {};
            if (malformed == 0) {
                title[0] = static_cast<char>(0xff);
            }
            else {
                memset(static_cast<void*>(title), 'a', 30);
                title[30] = static_cast<char>(0xc3);
            }
            // A kind-1 record name is stale residue: the portable document
            // ignores it, so the encode and the re-saved record stay
            // byte-identical to the unpoisoned ones.
            memcpy(source.graph_layout[1].name, title, sizeof(title));
            snapshot = source;
            memcpy(text_before, text, sizeof(text));
            TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == valid_length);
            TEST(memcmp(text, text_before, sizeof(text)) == 0);
            TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
            TEST(Synth::save_library_record(title_path, "Regression", "Titles", &source, 0) == 0);
            TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
            TEST(read_library_fixture(title_path, file_after, sizeof(file_after)) == file_length);
            TEST(memcmp(file_before, file_after, file_length) == 0);
        }
    }
    build_named_fm_fixture(&source);
    remove(title_path);

    // Out-of-range pitch base values refuse the editor encode: the encoder
    // verifies its own encode-decode round trip against the shared decode
    // validation.
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].base_value = 25;
    source.bank.instruments.entries[0].layers[0].mod_ratio                            = 9;
    for (Synth::InstrumentJsonDomain domain : domains) {
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == 0);
    }

    // The octave range rejects the encode the same way, with every other
    // control value in range.
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].base_value = 2.0f;
    source.bank.instruments.entries[0].layers[0].mod_ratio                            = 2.0f;
    source.bank.instruments.entries[0].layers[0].octave_offset                        = 13;
    for (Synth::InstrumentJsonDomain domain : domains) {
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) == 0);
    }
    source.bank.instruments.entries[0].layers[0].octave_offset = 0;

    // An in-range control still round-trips through validated_bank (the
    // clipboard decode keeps refusing the out-of-slider-range ratio).
    source.bank.instruments.entries[0].layers[0].mod_ratio = 9;
    const uint32_t in_range_length =
        Sculptor::encode_editor_instrument_json(source,
                                                0,
                                                0,
                                                text,
                                                sizeof(text),
                                                Synth::InstrumentJsonDomain::validated_bank);
    TEST(in_range_length > 0);
    TEST(Synth::decode_instrument_json_for_domain(text,
                                                  in_range_length,
                                                  &decoded.instr,
                                                  decoded.envs,
                                                  &decoded.env_count,
                                                  decoded.lfos,
                                                  &decoded.lfo_count,
                                                  decoded.layout,
                                                  max_portable_records,
                                                  &decoded.layout_count,
                                                  Synth::InstrumentJsonDomain::validated_bank));
    TEST(decoded.instr.layers[0].targets[Synth::mod_pitch].base_value == 2.0f &&
         decoded.instr.layers[0].mod_ratio == 9.0f);
    TEST(! decode_clipboard(&decoded, text, max_portable_records));

    // The octave range rejects alone: an out-of-range octave fails decode
    // even with every other control value in range.
    const char* const octave_out_of_range = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                            "\"pitch_offset_semitones\":2.0,\"octave_offset\":13}]}";
    for (Synth::InstrumentJsonDomain domain : domains) {
        fill_clipboard_sentinels(&decoded);
        fill_clipboard_sentinels(&sentinel);
        TEST(! Synth::decode_instrument_json_for_domain(octave_out_of_range,
                                                        static_cast<uint32_t>(strlen(octave_out_of_range)),
                                                        &decoded.instr,
                                                        decoded.envs,
                                                        &decoded.env_count,
                                                        decoded.lfos,
                                                        &decoded.lfo_count,
                                                        decoded.layout,
                                                        max_portable_records,
                                                        &decoded.layout_count,
                                                        domain));
        TEST(clipboard_sentinels_intact(&decoded, &sentinel));
    }

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max + 1>> exhausted_text_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max + 1>>();
    char (&exhausted_text)[Synth::library_payload_max + 1] = exhausted_text_storage->elements;
    memset(static_cast<void*>(exhausted_text), ' ', sizeof(exhausted_text));
    memcpy(exhausted_text, "{}", 2);
    for (Synth::InstrumentJsonDomain domain : domains) {
        fill_clipboard_sentinels(&decoded);
        fill_clipboard_sentinels(&sentinel);
        TEST(! Synth::decode_instrument_json_for_domain(exhausted_text,
                                                        sizeof(exhausted_text),
                                                        &decoded.instr,
                                                        decoded.envs,
                                                        &decoded.env_count,
                                                        decoded.lfos,
                                                        &decoded.lfo_count,
                                                        decoded.layout,
                                                        max_portable_records,
                                                        &decoded.layout_count,
                                                        domain));
        TEST(clipboard_sentinels_intact(&decoded, &sentinel));
    }
    uint32_t token_length =
        static_cast<uint32_t>(snprintf(exhausted_text, sizeof(exhausted_text), "{\"instrument_editor_bank\": {"));
    // Zero filler keys is minimal: this document decodes to an empty bank,
    // which library record validation refuses (no instruments). The old
    // 12000 distinct keys only added quadratic grammar work, not exhaustion.
    token_length += static_cast<uint32_t>(
        snprintf(exhausted_text + token_length, sizeof(exhausted_text) - token_length, "\"x\":1}}"));
    const std::unique_ptr<Synth::InstrumentEditorBank> empty_record_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    TEST(Synth::decode_editor_bank_json(exhausted_text, token_length, empty_record_storage.get()));
    TEST(empty_record_storage->bank.instruments.num_allocated == 0);
    const char* token_path = "synth_clipboard_empty_record.tmp";
    write_library_fixture(token_path, exhausted_text, token_length);
    Synth::LibraryEntry token_entry;
    TEST(Synth::read_library_index(token_path, &token_entry, 1) == 1);
    build_named_fm_fixture(&source);
    snapshot            = source;
    uint16_t token_slot = 0xBEEF;
    TEST(! Synth::load_library_instrument(token_path, &token_entry, &source, 1, &token_slot));
    TEST(token_slot == 0xBEEF);
    TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
    remove(token_path);

    // A private graph mapping can detach a pending move and refresh without touching live state.
    build_named_fm_fixture(&source);
    source.graph_layout_count                                   = 0;
    const std::unique_ptr<Sculptor::Graph> graph_storage        = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph                = *graph_storage;
    Sculptor::OscGraphMapping              mapping              = {};
    Sculptor::OscGraphMapping              live_before          = {};
    Sculptor::OscGraphMapping              private_mapping      = {};
    const std::unique_ptr<Sculptor::Graph> graph_before_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph_before         = *graph_before_storage;
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping, 0, 0));
    TEST(source.graph_layout_count == 0);
    const uint32_t unchanged_length = Sculptor::encode_editor_instrument_json(source,
                                                                              0,
                                                                              0,
                                                                              text,
                                                                              sizeof(text),
                                                                              Synth::InstrumentJsonDomain::clipboard);
    TEST(unchanged_length > 0);
    TEST(source.graph_layout_count == 0);
    TEST(Synth::decode_instrument_json(text,
                                       unchanged_length,
                                       &decoded.instr,
                                       decoded.envs,
                                       &decoded.env_count,
                                       decoded.lfos,
                                       &decoded.lfo_count,
                                       decoded.layout,
                                       max_portable_records,
                                       &decoded.layout_count));
    live_before     = mapping;
    private_mapping = mapping;
    snapshot        = source;
    const std::unique_ptr<Synth::InstrumentEditorBank> private_source_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& private_source = *private_source_storage;
    private_source                              = source;
    TEST(private_mapping.detached_count > 0);
    const uint32_t node     = private_mapping.detached[0].node_idx;
    vmath::vec2    position = graph.node(node).position;
    position.x += 57;
    graph.set_node_layout(node, position, 0, 0);
    graph_before = graph;
    TEST(Sculptor::detach_osc_graph_instance(&private_source, graph, &private_mapping, 0, 0, 0));
    TEST(Sculptor::refresh_osc_graph_compilation(&private_source, graph, private_mapping, 0, 0));
    TEST(Sculptor::encode_editor_instrument_json(private_source,
                                                 0,
                                                 0,
                                                 text,
                                                 sizeof(text),
                                                 Synth::InstrumentJsonDomain::clipboard) > 0);
    TEST(Sculptor::encode_editor_instrument_json(private_source,
                                                 0,
                                                 0,
                                                 text,
                                                 8,
                                                 Synth::InstrumentJsonDomain::clipboard) == 0);
    TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
    TEST(memcmp(&mapping, &live_before, sizeof(mapping)) == 0);
    TEST(memcmp(&graph, &graph_before, sizeof(graph)) == 0);
    TEST(Sculptor::detach_osc_graph_instance(&source, graph, &mapping, 0, 0, 0));
    TEST(source.graph_layout_count == 1);
    TEST(source.graph_layout[0].x == graph.node(node).position.x);

    // Exact effect identities distinguish canonical spelling from custom titles.
    const char titles[][32] = {
        "LFO 1",
        "LFO 255", // the 255 descriptor-id bound: the exact maximum id parses canonically
        "LFO 0",
        "LFO 256", // one past the bound maps to the 0 id
        "LFO 1280", "LFO 01", "LFO +1", "LFO 1x", "LFO ", "Delay", "LFO 99999999999999999999999"
    };
    const bool     recognized[] = { true, true, true, true, true, false, false, false, false, false, false };
    const uint16_t ids[]        = { 1, Synth::max_lfos, 0, 0, 0 };
    for (uint32_t index = 0; index < sizeof(titles) / sizeof(titles[0]); ++index) {
        uint16_t id = 0xBEEF;
        TEST(Sculptor::parse_effect_lfo_title(titles[index], &id) == recognized[index]);
        TEST(id == (recognized[index] ? ids[index] : 0xBEEF));
    }
    char unterminated_title[32];
    memset(static_cast<void*>(unterminated_title), '1', sizeof(unterminated_title));
    uint16_t id = 0xBEEF;
    TEST(! Sculptor::parse_effect_lfo_title(unterminated_title, &id));
    TEST(id == 0xBEEF);
    uint16_t lfo_ids[Synth::max_lfos] = {};
    for (uint32_t slot = 0; slot < Synth::max_lfos; ++slot) {
        lfo_ids[slot] = static_cast<uint16_t>(slot + 1);
    }
    for (uint32_t index = 0; index < sizeof(titles) / sizeof(titles[0]); ++index) {
        char title[32];
        memcpy(title, titles[index], sizeof(title));
        uint16_t mapped_id = 0xBEEF;
        TEST(Sculptor::translate_effect_lfo_title(title, lfo_ids, &mapped_id) == recognized[index]);
        TEST(mapped_id == (recognized[index] ? ids[index] : 0xBEEF));
        TEST(memcmp(title, titles[index], sizeof(title)) == 0);
    }
    char translated_title[32];
    memcpy(translated_title, unterminated_title, sizeof(translated_title));
    uint16_t mapped_id = 0xBEEF;
    TEST(! Sculptor::translate_effect_lfo_title(translated_title, lfo_ids, &mapped_id));
    TEST(mapped_id == 0xBEEF);
    TEST(memcmp(translated_title, unterminated_title, sizeof(translated_title)) == 0);
    strcpy(translated_title, "LFO 255");
    lfo_ids[Synth::max_lfos - 1] = 2;
    TEST(Sculptor::translate_effect_lfo_title(translated_title, lfo_ids, &mapped_id));
    TEST(mapped_id == 2 && strcmp(translated_title, "LFO 2") == 0);
    strcpy(translated_title, "LFO 1");
    lfo_ids[0] = 0;
    TEST(Sculptor::translate_effect_lfo_title(translated_title, lfo_ids, &mapped_id));
    TEST(mapped_id == 0 && strcmp(translated_title, "LFO 1") == 0);

    // Represented effect LFOs root descriptors independently of parameter bindings.
    build_named_fm_fixture(&source);
    while (source.bank.lfos.num_allocated < Synth::max_lfos) {
        const uint32_t slot            = source.bank.lfos.allocate();
        source.bank.lfos.entries[slot] = source.bank.lfos.entries[0];
    }
    Synth::EffectSlotBinding* high_effect = enabled_effect(source.bank, 0, 0, Synth::EffectType::delay);
    high_effect->bindings[0].lfo_desc_id  = Synth::max_lfos;
    add_detached_record(&source, 0, 0, 4, 0, 57, 59, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(source.graph_layout[source.graph_layout_count - 1].name, "LFO 255");
    add_detached_record(&source, 0, 0, 4, 0, 61, 63, Synth::ModSource::none, Synth::ModSource::none, 0);
    strcpy(source.graph_layout[source.graph_layout_count - 1].name, "LFO 254");
    TEST(Synth::validate_instrument_bank(&source.bank));
    Synth::reclaim_unused_slots(&source);
    TEST(source.bank.lfos.num_allocated == 3);
    TEST(source.graph_layout_count == 6); // 4 fixture records + 2 fx records
    TEST(strcmp(source.graph_layout[4].name, "LFO 3") == 0);
    TEST(strcmp(source.graph_layout[5].name, "LFO 2") == 0);
    TEST(source.graph_layout[4].x == 57 && source.graph_layout[4].y == 59);
    TEST(source.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == 3);

    // Late zone metadata exhaustion is refused before committing any earlier zone records.
    build_named_fm_fixture(&source);
    source.bank.channel_enabled[0] = 0;
    for (uint32_t zone = 1; zone < Synth::max_instr_per_channel; ++zone) {
        source.bank.channel_zones[0][zone] = { static_cast<uint8_t>(1 + 7 * zone), 0 };
        for (uint32_t r = 0; r < 4; ++r) {
            Synth::GraphNodeLayout record                    = source.graph_layout[r];
            record.zone                                      = static_cast<uint8_t>(zone);
            source.graph_layout[source.graph_layout_count++] = record;
        }
    }
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t payload_length               = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(payload_length > 0);
    const char* capacity_path = "synth_multizone_capacity.tmp";
    write_library_fixture(capacity_path, payload, payload_length);
    Synth::LibraryEntry entry;
    TEST(Synth::read_library_index(capacity_path, &entry, 1) == 1);
    build_named_fm_fixture(&private_source);
    private_source.bank.channel_enabled[1]  = 1;
    private_source.bank.channel_zones[1][0] = { 1, 0 };
    while (private_source.graph_layout_count < Synth::max_graph_records - 32) {
        Synth::GraphNodeLayout record = {};
        record.kind                   = 4;
        record.channel                = Synth::max_channels;
        snprintf(record.name, sizeof(record.name), "Custom %u", private_source.graph_layout_count);
        private_source.graph_layout[private_source.graph_layout_count++] = record;
    }
    snapshot      = private_source;
    uint16_t slot = 0xBEEF;
    TEST(! Synth::load_library_instrument(capacity_path, &entry, &private_source, 1, &slot));
    TEST(slot == 0xBEEF);
    TEST(memcmp(&private_source, &snapshot, sizeof(snapshot)) == 0);
    remove(capacity_path);
}

static void check_clipboard_inline_0()
{

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
                                "\"octave_offset\":5,"
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

        Synth::Instrument         instr                                 = {};
        Synth::EnvelopeDescriptor envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  env_count                             = 999;
        uint32_t                  lfo_count                             = 999;
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
        // The legacy key folds into the pitch base: 1.0 + (-12.0).
        TEST(osc.targets[Synth::mod_pitch].base_value == -11.0f);
        TEST(osc.octave_offset == 5);
        // Untouched targets keep neutral defaults.
        TEST(instr.layers[0].targets[Synth::mod_osc_mix].base_value == 0.0f);
        TEST(instr.layers[0].targets[Synth::mod_volume].base_value == 0.8f);
        TEST(instr.layers[0].targets[Synth::mod_volume].num_inputs == 2);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[1].scale == 0.25f);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[0].op == Synth::SourceOp::add);
        TEST(instr.layers[0].targets[Synth::mod_volume].inputs[0].scale == 0.5f);
        // The pitch block base plus the folded legacy key: 1.0 + (-12.0).
        TEST(instr.layers[0].targets[Synth::mod_pitch].base_value == -11.0f);
        TEST(instr.layers[0].targets[Synth::mod_panning].base_value == 0.5f);
        TEST(instr.layers[0].targets[Synth::mod_duty1].base_value == 0.5f);
        // fm_index is 0..1 in the document, 0..2*pi radians in the bank.
        TEST(instr.layers[0].targets[Synth::mod_fm_index].base_value == 0.75f * 6.2831853f);
        // Envelope: seconds -> ticks, levels 0..1 -> 16-bit values, min normalized to 0.
        TEST(osc.targets[Synth::mod_volume].envelope_desc_id == 1);
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
        TEST(osc.targets[Synth::mod_volume].lfo_desc_id == 1);
        TEST(lfo_count == 1);
        const Synth::LFODescriptor& lfo = lfos[0];
        TEST(lfo.wave == Synth::WaveType::sine_wave);
        TEST(lfo.period_ms == 200);
        TEST(lfo.min_value == 0.0f);
        TEST(osc.targets[Synth::mod_volume].lfo_op == Synth::SourceOp::add);
        TEST(osc.targets[Synth::mod_volume].lfo_depth == 0.2f);

        // Install into a bank through the remap helpers, then encode: the
        // document must decode back to the same instrument content.
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
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

        const std::unique_ptr<FixedArray<char, 64 * 1024>> encoded_storage =
            std::make_unique<FixedArray<char, 64 * 1024>>();
        char (&encoded)[64 * 1024] = encoded_storage->elements;
        const uint32_t encoded_len = Synth::encode_instrument_json(encoded, sizeof(encoded), &installed, &bank);
        TEST(encoded_len > 0);
        Synth::Instrument         decoded                                       = {};
        Synth::EnvelopeDescriptor decoded_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      decoded_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  decoded_env_count                             = 999;
        uint32_t                  decoded_lfo_count                             = 999;
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
        TEST(decoded_osc.targets[Synth::mod_pitch].base_value == -11.0f);
        TEST(decoded_osc.octave_offset == 5);
        TEST(decoded.layers[0].targets[Synth::mod_volume].base_value == 0.8f);
        TEST(decoded.layers[0].targets[Synth::mod_volume].num_inputs == 2);
        TEST(decoded.layers[0].targets[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
        TEST(decoded.layers[0].targets[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
        TEST(decoded.layers[0].targets[Synth::mod_volume].inputs[1].scale == 0.25f);
        TEST(decoded.layers[0].targets[Synth::mod_fm_index].base_value == 0.75f * 6.2831853f);
        TEST(decoded_osc.targets[Synth::mod_volume].envelope_desc_id == 1);
        TEST(decoded_env_count == 1);
        TEST(decoded_envs[0].points[1].position == env.points[1].position);
        TEST(decoded_envs[0].points[2].value == 16384);
        TEST(decoded_envs[0].min_value == envs[0].min_value);
        TEST(decoded_envs[0].min_max_delta == envs[0].min_max_delta);
        TEST(decoded_osc.targets[Synth::mod_volume].lfo_desc_id == 1);
        TEST(decoded_lfo_count == 1);
        TEST(decoded_lfos[0].period_ms == 200);
        TEST(decoded_osc.targets[Synth::mod_volume].lfo_depth == 0.2f);

        // -------------------------------------------------------------------
        // Octave Offset serialization: the key round-trips through every
        // instrument JSON domain and the saved-bank dialect, absent decodes 0,
        // and every out-of-range or non-integer spelling fails decode.  Pitch
        // Offset validates in every domain too.
        // -------------------------------------------------------------------
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> octave_source_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&           octave_source           = *octave_source_storage;
            const std::unique_ptr<ClipboardDecode> octave_decoded_storage  = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       octave_decoded          = *octave_decoded_storage;
            const std::unique_ptr<ClipboardDecode> octave_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       octave_sentinel         = *octave_sentinel_storage;
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&text)[clipboard_capacity] = text_storage->elements;

            // Both instrument domains preserve the octave.
            const Synth::InstrumentJsonDomain both_domains[2] = { Synth::InstrumentJsonDomain::clipboard,
                                                                  Synth::InstrumentJsonDomain::validated_bank };
            build_named_fm_fixture(&octave_source);
            Sculptor::set_identity_osc_sources(&octave_source, 0, 0);
            octave_source.bank.instruments.entries[0].layers[0].octave_offset = -3;
            for (const Synth::InstrumentJsonDomain domain : both_domains) {
                TEST(Sculptor::encode_editor_instrument_json(octave_source, 0, 0, text, sizeof(text), domain) != 0);
                fill_clipboard_sentinels(&octave_decoded);
                fill_clipboard_sentinels(&octave_sentinel);
                TEST(Synth::decode_instrument_json_for_domain(text,
                                                              static_cast<uint32_t>(strlen(text)),
                                                              &octave_decoded.instr,
                                                              octave_decoded.envs,
                                                              &octave_decoded.env_count,
                                                              octave_decoded.lfos,
                                                              &octave_decoded.lfo_count,
                                                              octave_decoded.layout,
                                                              max_portable_records,
                                                              &octave_decoded.layout_count,
                                                              domain));
                TEST(octave_decoded.instr.layers[0].octave_offset == -3);
            }

            // An absent key decodes as the zeroed default.
            const char* const no_octave_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                              "\"pitch_offset_semitones\":2.0}]}";
            {
                Synth::Instrument         plain_instr                                 = {};
                Synth::EnvelopeDescriptor plain_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      plain_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  plain_env_count                             = 999;
                uint32_t                  plain_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(no_octave_doc,
                                                   static_cast<uint32_t>(strlen(no_octave_doc)),
                                                   &plain_instr,
                                                   plain_envs,
                                                   &plain_env_count,
                                                   plain_lfos,
                                                   &plain_lfo_count));
                TEST(plain_instr.layers[0].octave_offset == 0);
                TEST(plain_instr.layers[0].targets[Synth::mod_pitch].base_value == 2.0f);
            }

            // Saved-bank dialect: encode and decode preserve the octave; a zero
            // octave omits the key and decodes back to 0.
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&payload)[Synth::library_payload_max] = payload_storage->elements;
            const uint32_t octave_length = Synth::encode_editor_bank_json(&octave_source, payload, sizeof(payload));
            TEST(octave_length > 0);
            const std::unique_ptr<Synth::InstrumentEditorBank> bank_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& bank_decoded = *bank_decoded_storage;
            memset(static_cast<void*>(&bank_decoded), 0, sizeof(bank_decoded));
            TEST(Synth::decode_editor_bank_json(payload, octave_length, &bank_decoded));
            TEST(bank_decoded.bank.instruments.entries[0].layers[0].octave_offset == -3);
            octave_source.bank.instruments.entries[0].layers[0].octave_offset = 0;
            const uint32_t zero_octave_length =
                Synth::encode_editor_bank_json(&octave_source, payload, sizeof(payload));
            TEST(zero_octave_length > 0);
            TEST(strstr(payload, "octave_offset") == nullptr); // zero omits the key
            memset(static_cast<void*>(&bank_decoded), 0, sizeof(bank_decoded));
            TEST(Synth::decode_editor_bank_json(payload, zero_octave_length, &bank_decoded));
            TEST(bank_decoded.bank.instruments.entries[0].layers[0].octave_offset == 0);

            // Out-of-range and non-integer spellings fail the bank decode: each
            // one is spliced over the encoded value of a bank holding -3.
            auto decode_with_octave_spelling = [](const char* spelling) {
                const std::unique_ptr<Synth::InstrumentEditorBank> spelling_source_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& spelling_source = *spelling_source_storage;
                build_named_fm_fixture(&spelling_source);
                spelling_source.bank.instruments.entries[0].layers[0].octave_offset = -3;
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spelling_payload_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spelling_payload)[Synth::library_payload_max] = spelling_payload_storage->elements;
                const uint32_t spelling_length =
                    Synth::encode_editor_bank_json(&spelling_source, spelling_payload, sizeof(spelling_payload));
                TEST(spelling_length > 0);
                const char* const key = "\"octave_offset\":-3";
                const char* const hit = strstr(spelling_payload, key);
                TEST(hit != nullptr);
                if (! hit) {
                    return false;
                }
                const uint32_t key_len      = static_cast<uint32_t>(strlen(key));
                const uint32_t prefix       = static_cast<uint32_t>(hit - spelling_payload);
                const uint32_t suffix       = spelling_length - prefix - key_len;
                const uint32_t spelling_len = static_cast<uint32_t>(strlen(spelling));
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spelling_doc_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spelling_doc)[Synth::library_payload_max] = spelling_doc_storage->elements;
                memcpy(spelling_doc, spelling_payload, prefix);
                memcpy(spelling_doc + prefix, key, 16); // the key text without the value
                memcpy(spelling_doc + prefix + 16, spelling, spelling_len);
                memcpy(spelling_doc + prefix + 16 + spelling_len, hit + key_len, suffix);
                const std::unique_ptr<Synth::InstrumentEditorBank> spelling_decoded_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& spelling_decoded = *spelling_decoded_storage;
                memset(static_cast<void*>(&spelling_decoded), 0, sizeof(spelling_decoded));
                return Synth::decode_editor_bank_json(spelling_doc,
                                                      prefix + 16 + spelling_len + suffix,
                                                      &spelling_decoded);
            };
            TEST(decode_with_octave_spelling("-3")); // the splice itself decodes
            TEST(! decode_with_octave_spelling("13"));
            TEST(! decode_with_octave_spelling("-13"));
            TEST(! decode_with_octave_spelling("1.5"));
            TEST(! decode_with_octave_spelling("1.00000001"));
            TEST(! decode_with_octave_spelling("12.0000001"));

            // Both instrument domains reject every bad octave spelling at the
            // token level, before any binary32 narrowing.
            const char* const octave_values[] = { "13", "-13", "1.5", "1.00000001", "12.0000001" };
            for (const char* spelling : octave_values) {
                char octave_doc[256];
                TEST(snprintf(octave_doc,
                              sizeof(octave_doc),
                              "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                              "\"pitch_offset_semitones\":2.0,\"octave_offset\":%s}]}",
                              spelling) > 0);
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    fill_clipboard_sentinels(&octave_decoded);
                    fill_clipboard_sentinels(&octave_sentinel);
                    TEST(! Synth::decode_instrument_json_for_domain(octave_doc,
                                                                    static_cast<uint32_t>(strlen(octave_doc)),
                                                                    &octave_decoded.instr,
                                                                    octave_decoded.envs,
                                                                    &octave_decoded.env_count,
                                                                    octave_decoded.lfos,
                                                                    &octave_decoded.lfo_count,
                                                                    octave_decoded.layout,
                                                                    max_portable_records,
                                                                    &octave_decoded.layout_count,
                                                                    domain));
                    TEST(clipboard_sentinels_intact(&octave_decoded, &octave_sentinel));
                }
            }
        }
        // -----------------------------------------------------------------
        // FIR Slope serialization: the key round-trips through every
        // instrument JSON domain and the saved-bank dialect, absent decodes
        // 100, the portable encoder always emits it, the bank encoder omits
        // the default, and every out-of-range or non-integer spelling fails
        // decode before any binary32 narrowing.
        // -----------------------------------------------------------------
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> slope_source_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&           slope_source           = *slope_source_storage;
            const std::unique_ptr<ClipboardDecode> slope_decoded_storage  = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       slope_decoded          = *slope_decoded_storage;
            const std::unique_ptr<ClipboardDecode> slope_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       slope_sentinel         = *slope_sentinel_storage;
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&text)[clipboard_capacity]                  = text_storage->elements;
            const Synth::InstrumentJsonDomain both_domains[2] = { Synth::InstrumentJsonDomain::clipboard,
                                                                  Synth::InstrumentJsonDomain::validated_bank };
            build_named_fm_fixture(&slope_source);
            Sculptor::set_identity_osc_sources(&slope_source, 0, 0);
            slope_source.bank.instruments.entries[0].layers[0].fir_slope_percent = 37;
            for (const Synth::InstrumentJsonDomain domain : both_domains) {
                TEST(Sculptor::encode_editor_instrument_json(slope_source, 0, 0, text, sizeof(text), domain) != 0);
                fill_clipboard_sentinels(&slope_decoded);
                fill_clipboard_sentinels(&slope_sentinel);
                TEST(Synth::decode_instrument_json_for_domain(text,
                                                              static_cast<uint32_t>(strlen(text)),
                                                              &slope_decoded.instr,
                                                              slope_decoded.envs,
                                                              &slope_decoded.env_count,
                                                              slope_decoded.lfos,
                                                              &slope_decoded.lfo_count,
                                                              slope_decoded.layout,
                                                              max_portable_records,
                                                              &slope_decoded.layout_count,
                                                              domain));
                TEST(slope_decoded.instr.layers[0].fir_slope_percent == 37);
            }
            // The portable dialect always emits the key; the bank dialect
            // omits it at the 100 default.
            TEST(Sculptor::encode_editor_instrument_json(slope_source,
                                                         0,
                                                         0,
                                                         text,
                                                         sizeof(text),
                                                         Synth::InstrumentJsonDomain::clipboard) != 0);
            TEST(strstr(text, "fir_slope") != nullptr);
            // An absent key decodes as the default 100.
            const char* const no_slope_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                             "\"pitch_offset_semitones\":2.0}]}";
            {
                Synth::Instrument         plain_instr                                 = {};
                Synth::EnvelopeDescriptor plain_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      plain_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  plain_env_count                             = 999;
                uint32_t                  plain_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(no_slope_doc,
                                                   static_cast<uint32_t>(strlen(no_slope_doc)),
                                                   &plain_instr,
                                                   plain_envs,
                                                   &plain_env_count,
                                                   plain_lfos,
                                                   &plain_lfo_count));
                TEST(plain_instr.layers[0].fir_slope_percent == 100);
                TEST(plain_instr.layers[0].targets[Synth::mod_pitch].base_value == 2.0f);
                // fir_type: absent = band-pass; the key may follow the target blocks, and a
                // low-pass spread base is a Hz value up to 22050 while band-pass stays 0..36.
                TEST(plain_instr.layers[0].fir_type == Synth::fir_bandpass);
                const char* const lowpass_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                                "\"fir_pitch_spread\":{\"base\":440.0},\"fir_type\":\"lowpass\"}]}";
                TEST(Synth::decode_instrument_json(lowpass_doc,
                                                   static_cast<uint32_t>(strlen(lowpass_doc)),
                                                   &plain_instr,
                                                   plain_envs,
                                                   &plain_env_count,
                                                   plain_lfos,
                                                   &plain_lfo_count));
                TEST(plain_instr.layers[0].fir_type == Synth::fir_lowpass);
                TEST(plain_instr.layers[0].targets[Synth::mod_fir_spread].base_value == 440.0f);
                const char* const bandpass_hz_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                    "\"fir_pitch_spread\":{\"base\":440.0}}]}";
                TEST(! Synth::decode_instrument_json(bandpass_hz_doc,
                                                     static_cast<uint32_t>(strlen(bandpass_hz_doc)),
                                                     &plain_instr,
                                                     plain_envs,
                                                     &plain_env_count,
                                                     plain_lfos,
                                                     &plain_lfo_count));
                const char* const bad_type_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"notch\"}]}";
                TEST(! Synth::decode_instrument_json(bad_type_doc,
                                                     static_cast<uint32_t>(strlen(bad_type_doc)),
                                                     &plain_instr,
                                                     plain_envs,
                                                     &plain_env_count,
                                                     plain_lfos,
                                                     &plain_lfo_count));
            }
            // Saved-bank dialect: encode and decode preserve the slope; the
            // default omits the key and decodes back to 100.
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&payload)[Synth::library_payload_max] = payload_storage->elements;
            const uint32_t slope_length = Synth::encode_editor_bank_json(&slope_source, payload, sizeof(payload));
            TEST(slope_length > 0);
            TEST(strstr(payload, "\"fir_slope\":37") != nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> bank_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& bank_decoded = *bank_decoded_storage;
            memset(static_cast<void*>(&bank_decoded), 0, sizeof(bank_decoded));
            TEST(Synth::decode_editor_bank_json(payload, slope_length, &bank_decoded));
            TEST(bank_decoded.bank.instruments.entries[0].layers[0].fir_slope_percent == 37);
            slope_source.bank.instruments.entries[0].layers[0].fir_slope_percent          = 100;
            slope_source.bank.instruments.entries[0].layers[0].string_dispersion_sections = 4.0f;
            const uint32_t default_slope_length =
                Synth::encode_editor_bank_json(&slope_source, payload, sizeof(payload));
            TEST(default_slope_length > 0);
            TEST(strstr(payload, "fir_slope") == nullptr); // default omits the key
            memset(static_cast<void*>(&bank_decoded), 0, sizeof(bank_decoded));
            TEST(Synth::decode_editor_bank_json(payload, default_slope_length, &bank_decoded));
            TEST(bank_decoded.bank.instruments.entries[0].layers[0].fir_slope_percent == 100);
            // Out-of-range and non-integer spellings fail the bank decode:
            // each one is spliced over the encoded value of a bank holding 37.
            auto decode_with_slope_spelling = [](const char* spelling) {
                const std::unique_ptr<Synth::InstrumentEditorBank> spelling_source_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& spelling_source = *spelling_source_storage;
                build_named_fm_fixture(&spelling_source);
                spelling_source.bank.instruments.entries[0].layers[0].fir_slope_percent = 37;
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spelling_payload_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spelling_payload)[Synth::library_payload_max] = spelling_payload_storage->elements;
                const uint32_t spelling_length =
                    Synth::encode_editor_bank_json(&spelling_source, spelling_payload, sizeof(spelling_payload));
                TEST(spelling_length > 0);
                const char* const key      = "\"fir_slope\":37";
                char              repl[32] = {};
                memcpy(repl, "\"fir_slope\":", 12);
                memcpy(repl + 12, spelling, strlen(spelling));
                repl[12 + strlen(spelling)] = 0;
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spelling_doc_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spelling_doc)[Synth::library_payload_max] = spelling_doc_storage->elements;
                const uint32_t spliced_len                       = splice_single_occurrence(spelling_payload,
                                                                                            spelling_length,
                                                                                            key,
                                                                                            repl,
                                                                                            spelling_doc,
                                                                                            sizeof(spelling_doc));
                TEST(spliced_len > 0);
                if (! spliced_len) {
                    return false;
                }
                const std::unique_ptr<Synth::InstrumentEditorBank> spelling_decoded_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& spelling_decoded = *spelling_decoded_storage;
                memset(static_cast<void*>(&spelling_decoded), 0, sizeof(spelling_decoded));
                return Synth::decode_editor_bank_json(spelling_doc, spliced_len, &spelling_decoded);
            };
            TEST(decode_with_slope_spelling("37")); // the splice itself decodes
            TEST(! decode_with_slope_spelling("0"));
            TEST(! decode_with_slope_spelling("101"));
            TEST(! decode_with_slope_spelling("-1"));
            TEST(! decode_with_slope_spelling("50.5"));
            TEST(! decode_with_slope_spelling("1e2"));
            TEST(! decode_with_slope_spelling("-4294967295"));
            // Both instrument domains reject every bad slope spelling at the
            // token level, before any binary32 narrowing.
            const char* const slope_values[] = { "0", "101", "-1", "50.5", "1e2", "-4294967295", "\"sine\"", "true" };
            for (const char* spelling : slope_values) {
                char slope_doc[256];
                TEST(snprintf(slope_doc,
                              sizeof(slope_doc),
                              "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                              "\"pitch_offset_semitones\":2.0,\"fir_slope\":%s}]}",
                              spelling) > 0);
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    fill_clipboard_sentinels(&slope_decoded);
                    fill_clipboard_sentinels(&slope_sentinel);
                    TEST(! Synth::decode_instrument_json_for_domain(slope_doc,
                                                                    static_cast<uint32_t>(strlen(slope_doc)),
                                                                    &slope_decoded.instr,
                                                                    slope_decoded.envs,
                                                                    &slope_decoded.env_count,
                                                                    slope_decoded.lfos,
                                                                    &slope_decoded.lfo_count,
                                                                    slope_decoded.layout,
                                                                    max_portable_records,
                                                                    &slope_decoded.layout_count,
                                                                    domain));
                    TEST(clipboard_sentinels_intact(&slope_decoded, &slope_sentinel));
                }
            }
            // Slope routing round-trip: a fir_slope routing block decodes
            // onto the 12th target and survives a second encode/decode
            // pass in both editor dialects.
            {
                const char* const slope_routing_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                    "\"routing\":{\"fir_slope\":{\"base_value\":37,"
                    "\"inputs\":[{\"source\":\"velocity\",\"op\":\"add\",\"scale\":1.0}]}}}]}";
                Synth::Instrument         routed_instr                                 = {};
                Synth::EnvelopeDescriptor routed_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      routed_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  routed_env_count                             = 999;
                uint32_t                  routed_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(slope_routing_doc,
                                                   static_cast<uint32_t>(strlen(slope_routing_doc)),
                                                   &routed_instr,
                                                   routed_envs,
                                                   &routed_env_count,
                                                   routed_lfos,
                                                   &routed_lfo_count));
                TEST(routed_instr.layers[0].targets[Synth::mod_fir_slope].base_value == 37.0f);
                TEST(routed_instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 1);
                TEST(routed_instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].source ==
                     Synth::ModSource::velocity);
                TEST(routed_instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].op == Synth::SourceOp::add);
                TEST(routed_instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].scale == 1.0f);

                const std::unique_ptr<Synth::InstrumentEditorBank> routed_source_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& routed_source = *routed_source_storage;
                build_named_fm_fixture(&routed_source);
                Sculptor::set_identity_osc_sources(&routed_source, 0, 0);
                routed_source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope] =
                    routed_instr.layers[0].targets[Synth::mod_fir_slope];
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    TEST(Sculptor::encode_editor_instrument_json(routed_source, 0, 0, text, sizeof(text), domain) != 0);
                    fill_clipboard_sentinels(&slope_decoded);
                    fill_clipboard_sentinels(&slope_sentinel);
                    TEST(Synth::decode_instrument_json_for_domain(text,
                                                                  static_cast<uint32_t>(strlen(text)),
                                                                  &slope_decoded.instr,
                                                                  slope_decoded.envs,
                                                                  &slope_decoded.env_count,
                                                                  slope_decoded.lfos,
                                                                  &slope_decoded.lfo_count,
                                                                  slope_decoded.layout,
                                                                  max_portable_records,
                                                                  &slope_decoded.layout_count,
                                                                  domain));
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].base_value == 37.0f);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 1);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].source ==
                         Synth::ModSource::velocity);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].op ==
                         Synth::SourceOp::add);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].scale == 1.0f);
                }
            }
            // The bank-style "routing" alias marks its targets as touched:
            // alias-supplied band values survive the portable absent-default
            // pass, and an explicitly empty gain block still decodes 0.
            // Without the alias, the absent keys decode their defaults, so
            // the two paths stay distinguishable.
            {
                const char* const alias_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
                    "\"fir_bands\":2,\"routing\":{\"fir_band0_gain\":{\"base_value\":0.25},"
                    "\"fir_band1_slope\":{\"base_value\":37}}}]}";
                Synth::Instrument         alias_instr                                 = {};
                Synth::EnvelopeDescriptor alias_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      alias_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  alias_env_count                             = 999;
                uint32_t                  alias_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(alias_doc,
                                                   static_cast<uint32_t>(strlen(alias_doc)),
                                                   &alias_instr,
                                                   alias_envs,
                                                   &alias_env_count,
                                                   alias_lfos,
                                                   &alias_lfo_count));
                TEST(alias_instr.layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.25f);
                TEST(alias_instr.layers[0].targets[Synth::mod_fir_band1_slope].base_value == 37.0f);
                const char* const zero_gain_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
                    "\"fir_bands\":2,\"routing\":{\"fir_band0_gain\":{}}}]}";
                Synth::Instrument         zero_gain_instr                                 = {};
                Synth::EnvelopeDescriptor zero_gain_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      zero_gain_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  zero_gain_env_count                             = 999;
                uint32_t                  zero_gain_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(zero_gain_doc,
                                                   static_cast<uint32_t>(strlen(zero_gain_doc)),
                                                   &zero_gain_instr,
                                                   zero_gain_envs,
                                                   &zero_gain_env_count,
                                                   zero_gain_lfos,
                                                   &zero_gain_lfo_count));
                TEST(zero_gain_instr.layers[0].targets[Synth::mod_fir_band0_gain].base_value == 0.0f);
                const char* const absent_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_type\":\"bandpass\","
                    "\"fir_bands\":2}]}";
                Synth::Instrument         absent_instr                                 = {};
                Synth::EnvelopeDescriptor absent_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      absent_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  absent_env_count                             = 999;
                uint32_t                  absent_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(absent_doc,
                                                   static_cast<uint32_t>(strlen(absent_doc)),
                                                   &absent_instr,
                                                   absent_envs,
                                                   &absent_env_count,
                                                   absent_lfos,
                                                   &absent_lfo_count));
                TEST(absent_instr.layers[0].targets[Synth::mod_fir_band0_gain].base_value == 1.0f);
                TEST(absent_instr.layers[0].targets[Synth::mod_fir_band1_slope].base_value == 100.0f);
            }
            // The slope base is an integer percent 1..100 in both dialects:
            // out-of-range and fractional spellings are invalid state, so the
            // encoder can never emit a base its decoder rejects.
            {
                const std::unique_ptr<Synth::InstrumentEditorBank> bad_bank_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& bad_bank    = *bad_bank_storage;
                const char* const            bad_bases[] = { "0", "101", "-1", "37.5", "1e2", "-4294967295" };
                for (const char* bad : bad_bases) {
                    char bad_doc[384];
                    // Document dialect target block.
                    TEST(snprintf(bad_doc,
                                  sizeof(bad_doc),
                                  "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                  "\"fir_slope\":{\"base\":%s,\"sources\":[{\"source\":\"velocity\"}]}}]}",
                                  bad) > 0);
                    for (const Synth::InstrumentJsonDomain domain : both_domains) {
                        fill_clipboard_sentinels(&slope_decoded);
                        fill_clipboard_sentinels(&slope_sentinel);
                        TEST(! Synth::decode_instrument_json_for_domain(bad_doc,
                                                                        static_cast<uint32_t>(strlen(bad_doc)),
                                                                        &slope_decoded.instr,
                                                                        slope_decoded.envs,
                                                                        &slope_decoded.env_count,
                                                                        slope_decoded.lfos,
                                                                        &slope_decoded.lfo_count,
                                                                        slope_decoded.layout,
                                                                        max_portable_records,
                                                                        &slope_decoded.layout_count,
                                                                        domain));
                        TEST(clipboard_sentinels_intact(&slope_decoded, &slope_sentinel));
                    }
                    // Document dialect bank-style routing alias.
                    TEST(snprintf(bad_doc,
                                  sizeof(bad_doc),
                                  "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                  "\"routing\":{\"fir_slope\":{\"base_value\":%s}}}]}",
                                  bad) > 0);
                    for (const Synth::InstrumentJsonDomain domain : both_domains) {
                        fill_clipboard_sentinels(&slope_decoded);
                        fill_clipboard_sentinels(&slope_sentinel);
                        TEST(! Synth::decode_instrument_json_for_domain(bad_doc,
                                                                        static_cast<uint32_t>(strlen(bad_doc)),
                                                                        &slope_decoded.instr,
                                                                        slope_decoded.envs,
                                                                        &slope_decoded.env_count,
                                                                        slope_decoded.lfos,
                                                                        &slope_decoded.lfo_count,
                                                                        slope_decoded.layout,
                                                                        max_portable_records,
                                                                        &slope_decoded.layout_count,
                                                                        domain));
                        TEST(clipboard_sentinels_intact(&slope_decoded, &slope_sentinel));
                    }
                    // Saved-bank dialect: the same fixture with a valid base
                    // decodes, proving the rejections below fail on the slope
                    // grammar rather than on malformed JSON.
                    {
                        const std::unique_ptr<Synth::InstrumentEditorBank> good_bank_storage =
                            std::make_unique<Synth::InstrumentEditorBank>();
                        Synth::InstrumentEditorBank& good_bank = *good_bank_storage;
                        char                         good_doc[384];
                        TEST(snprintf(good_doc,
                                      sizeof(good_doc),
                                      "{\"instrument_editor_bank\":{\"instruments\":"
                                      "[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"sine\","
                                      "\"routing\":{\"fir_slope\":{\"base_value\":37}}}]}],"
                                      "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}") > 0);
                        memset(static_cast<void*>(&good_bank), 0, sizeof(good_bank));
                        TEST(Synth::decode_editor_bank_json(good_doc,
                                                            static_cast<uint32_t>(strlen(good_doc)),
                                                            &good_bank));
                        TEST(good_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope].base_value ==
                             37.0f);
                        TEST(good_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope].num_inputs ==
                             0);
                    }
                    TEST(snprintf(bad_doc,
                                  sizeof(bad_doc),
                                  "{\"instrument_editor_bank\":{\"instruments\":"
                                  "[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"sine\","
                                  "\"routing\":{\"fir_slope\":{\"base_value\":%s}}}]}],"
                                  "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}",
                                  bad) > 0);
                    memset(static_cast<void*>(&bad_bank), 0, sizeof(bad_bank));
                    TEST(! Synth::decode_editor_bank_json(bad_doc, static_cast<uint32_t>(strlen(bad_doc)), &bad_bank));
                }
            }
            // A binding carrying only dormant generator fields (no base, no
            // inputs, no bound descriptor) is not routed: the scalar key
            // survives the save/reload round-trip unchanged.
            {
                const std::unique_ptr<Synth::InstrumentEditorBank> dormant_source_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& dormant_source = *dormant_source_storage;
                build_named_fm_fixture(&dormant_source);
                Sculptor::set_identity_osc_sources(&dormant_source, 0, 0);
                dormant_source.bank.instruments.entries[0].layers[0].fir_slope_percent                       = 37;
                dormant_source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope].lfo_depth = 0.5f;
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    const uint32_t dormant_len =
                        Sculptor::encode_editor_instrument_json(dormant_source, 0, 0, text, sizeof(text), domain);
                    TEST(dormant_len != 0 && dormant_len < sizeof(text));
                    text[dormant_len] = '\0'; // the encoder returns a length, it does not terminate
                    TEST(strstr(text, "\"fir_slope\":37") != nullptr);
                    TEST(strstr(text, "\"routing\":{\"fir_slope\"") == nullptr);
                    if (domain == Synth::InstrumentJsonDomain::clipboard) {
                        // The clipboard layer object has no generators section, so
                        // the only fir_slope key is the scalar.
                        TEST(strstr(text, "\"fir_slope\":{") == nullptr);
                    }
                    fill_clipboard_sentinels(&slope_decoded);
                    fill_clipboard_sentinels(&slope_sentinel);
                    TEST(Synth::decode_instrument_json_for_domain(text,
                                                                  static_cast<uint32_t>(strlen(text)),
                                                                  &slope_decoded.instr,
                                                                  slope_decoded.envs,
                                                                  &slope_decoded.env_count,
                                                                  slope_decoded.lfos,
                                                                  &slope_decoded.lfo_count,
                                                                  slope_decoded.layout,
                                                                  max_portable_records,
                                                                  &slope_decoded.layout_count,
                                                                  domain));
                    TEST(slope_decoded.instr.layers[0].fir_slope_percent == 37);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].base_value == 0.0f);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 0);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].lfo_desc_id == 0);
                }
            }
            // Old banks: a scalar slope with no routing key decodes with
            // the 12th target zeroed and the scalar preserved.
            {
                Synth::Instrument         legacy_instr                                 = {};
                Synth::EnvelopeDescriptor legacy_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      legacy_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  legacy_env_count                             = 999;
                uint32_t                  legacy_lfo_count                             = 999;
                const char* const         legacy_slope_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_slope\":37}]}";
                TEST(Synth::decode_instrument_json(legacy_slope_doc,
                                                   static_cast<uint32_t>(strlen(legacy_slope_doc)),
                                                   &legacy_instr,
                                                   legacy_envs,
                                                   &legacy_env_count,
                                                   legacy_lfos,
                                                   &legacy_lfo_count));
                TEST(legacy_instr.layers[0].fir_slope_percent == 37);
                TEST(legacy_instr.layers[0].targets[Synth::mod_fir_slope].base_value == 0.0f);
                TEST(legacy_instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 0);
                TEST(legacy_instr.layers[0].targets[Synth::mod_fir_slope].envelope_desc_id == 0);
                TEST(legacy_instr.layers[0].targets[Synth::mod_fir_slope].lfo_desc_id == 0);
                // The same legacy shape through the saved-bank codec.
                const char* const legacy_bank_doc =
                    "{\"instrument_editor_bank\":{\"instruments\":"
                    "[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"sine\",\"fir_slope\":37,"
                    "\"routing\":{}}]}],"
                    "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
                const std::unique_ptr<Synth::InstrumentEditorBank> legacy_bank_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& legacy_bank = *legacy_bank_storage;
                memset(static_cast<void*>(&legacy_bank), 0, sizeof(legacy_bank));
                TEST(Synth::decode_editor_bank_json(legacy_bank_doc,
                                                    static_cast<uint32_t>(strlen(legacy_bank_doc)),
                                                    &legacy_bank));
                TEST(legacy_bank.bank.instruments.entries[0].layers[0].fir_slope_percent == 37);
                TEST(legacy_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope].base_value ==
                     0.0f);
                TEST(legacy_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_slope].num_inputs == 0);
            }
            // A schema-conformant document may state a routed slope block
            // without a base (the decode default 0): decode, clipboard-encode
            // and decode again reproduce the base-0 binding exactly, so the
            // encoder never emits a base its own decoder rejects.
            {
                const char* const sources_only_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                    "\"fir_slope\":{\"sources\":[{\"source\":\"velocity\"}]}}]}";
                Synth::Instrument         sources_instr                                 = {};
                Synth::EnvelopeDescriptor sources_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      sources_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  sources_env_count                             = 999;
                uint32_t                  sources_lfo_count                             = 999;
                TEST(Synth::decode_instrument_json(sources_only_doc,
                                                   static_cast<uint32_t>(strlen(sources_only_doc)),
                                                   &sources_instr,
                                                   sources_envs,
                                                   &sources_env_count,
                                                   sources_lfos,
                                                   &sources_lfo_count));
                TEST(sources_instr.layers[0].targets[Synth::mod_fir_slope].base_value == 0.0f);
                TEST(sources_instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 1);
                TEST(sources_instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].source ==
                     Synth::ModSource::velocity);
                slope_source.bank.instruments.entries[0].layers[0] = sources_instr.layers[0];
                const uint32_t sources_len =
                    Sculptor::encode_editor_instrument_json(slope_source,
                                                            0,
                                                            0,
                                                            text,
                                                            sizeof(text),
                                                            Synth::InstrumentJsonDomain::clipboard);
                TEST(sources_len != 0 && sources_len < sizeof(text));
                text[sources_len] = '\0'; // the encoder returns a length, it does not terminate
                // The clipboard slope block states its sources but no base.
                TEST(strstr(text, "\"fir_slope\":{\"sources\"") != nullptr);
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    fill_clipboard_sentinels(&slope_decoded);
                    fill_clipboard_sentinels(&slope_sentinel);
                    TEST(Synth::decode_instrument_json_for_domain(text,
                                                                  static_cast<uint32_t>(strlen(text)),
                                                                  &slope_decoded.instr,
                                                                  slope_decoded.envs,
                                                                  &slope_decoded.env_count,
                                                                  slope_decoded.lfos,
                                                                  &slope_decoded.lfo_count,
                                                                  slope_decoded.layout,
                                                                  max_portable_records,
                                                                  &slope_decoded.layout_count,
                                                                  domain));
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].base_value == 0.0f);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].num_inputs == 1);
                    TEST(slope_decoded.instr.layers[0].targets[Synth::mod_fir_slope].inputs[0].source ==
                         Synth::ModSource::velocity);
                }
            }
            // The published schema's generator-layout target enums admit the
            // slope target: a slope-bound generator layout record decodes
            // with its target preserved.
            {
                const char* const slope_layout_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}],"
                    "\"graph_layout\":[{\"kind\":1,\"x\":0,\"y\":0,\"width\":0,\"height\":0,"
                    "\"layer\":0,\"target\":11}]}";
                for (const Synth::InstrumentJsonDomain domain : both_domains) {
                    fill_clipboard_sentinels(&slope_decoded);
                    fill_clipboard_sentinels(&slope_sentinel);
                    TEST(Synth::decode_instrument_json_for_domain(slope_layout_doc,
                                                                  static_cast<uint32_t>(strlen(slope_layout_doc)),
                                                                  &slope_decoded.instr,
                                                                  slope_decoded.envs,
                                                                  &slope_decoded.env_count,
                                                                  slope_decoded.lfos,
                                                                  &slope_decoded.lfo_count,
                                                                  slope_decoded.layout,
                                                                  max_portable_records,
                                                                  &slope_decoded.layout_count,
                                                                  domain));
                    TEST(slope_decoded.layout_count == 1);
                    TEST(slope_decoded.layout[0].kind == 1);
                    TEST(slope_decoded.layout[0].target == 11);
                }
            }
            // Default omission: a zeroed binding with the scalar default
            // emits no fir_slope keys in either dialect.
            {
                const std::unique_ptr<Synth::InstrumentEditorBank> default_source_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& default_source = *default_source_storage;
                build_named_fm_fixture(&default_source);
                Sculptor::set_identity_osc_sources(&default_source, 0, 0);
                default_source.bank.instruments.entries[0].layers[0].fir_slope_percent          = 100;
                default_source.bank.instruments.entries[0].layers[0].string_dispersion_sections = 4.0f;
                TEST(Sculptor::encode_editor_instrument_json(default_source,
                                                             0,
                                                             0,
                                                             text,
                                                             sizeof(text),
                                                             Synth::InstrumentJsonDomain::clipboard) != 0);
                TEST(strstr(text, "fir_slope") == nullptr);
                TEST(Sculptor::encode_editor_instrument_json(default_source,
                                                             0,
                                                             0,
                                                             text,
                                                             sizeof(text),
                                                             Synth::InstrumentJsonDomain::validated_bank) != 0);
                TEST(strstr(text, "fir_slope") == nullptr);
            }
        }

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
            Synth::Instrument         fm_instr                                 = {};
            Synth::EnvelopeDescriptor fm_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      fm_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  fm_env_count                             = 999;
            uint32_t                  fm_lfo_count                             = 999;
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
            const std::unique_ptr<Synth::InstrumentEditorBank> fm_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& fm_bank = *fm_bank_storage;
            memset(static_cast<void*>(&fm_bank), 0, sizeof(fm_bank));
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

            const std::unique_ptr<FixedArray<char, 64 * 1024>> fm_text_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&fm_text)[64 * 1024] = fm_text_storage->elements;
            const uint32_t fm_text_len =
                Synth::encode_instrument_json(fm_text, sizeof(fm_text), &fm_installed, &fm_bank.bank);
            TEST(fm_text_len > 0);
            Synth::Instrument         fm_back                                       = {};
            Synth::EnvelopeDescriptor fm_back_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      fm_back_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  fm_back_env_count                             = 0;
            uint32_t                  fm_back_lfo_count                             = 0;
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
            Synth::Instrument         dup_instr                                 = {};
            Synth::EnvelopeDescriptor dup_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      dup_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  dup_env_count                             = 999;
            uint32_t                  dup_lfo_count                             = 999;
            TEST(Synth::decode_instrument_json(dup_doc,
                                               static_cast<uint32_t>(strlen(dup_doc)),
                                               &dup_instr,
                                               dup_envs,
                                               &dup_env_count,
                                               dup_lfos,
                                               &dup_lfo_count));
            TEST(dup_instr.layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
            TEST(dup_instr.layers[1].targets[Synth::mod_volume].envelope_desc_id == 1);
            TEST(dup_env_count == 1);
        }

        // Choice B data model (AC11): a layer's duty/mix/fm bindings with routed MIDI
        // inputs survive remap_instrument and a document JSON round-trip.  The runtime
        // expands these four targets like the five projected ones; this pins the storage
        // their routed inputs ride on.
        {
            const char* const routed_doc =
                "{"
                "\"format\":\"synth-instrument-v1\","
                "\"layers\":[{"
                "\"wave_a\":\"sine\","
                "\"duty_a\":{\"base\":0.25,\"sources\":[{\"source\":\"velocity\",\"op\":\"add\",\"scale\":0.5}]},"
                "\"duty_b\":{\"base\":0.5,\"sources\":[{\"source\":\"mod_wheel\",\"op\":\"multiply\",\"scale\":-0.25}]},"
                "\"osc_mix\":{\"base\":0.75,\"sources\":[{\"source\":\"channel_pressure\",\"op\":\"add\",\"scale\":0.1}]},"
                "\"fm_index\":{\"base\":0.5,\"sources\":[{\"source\":\"aftertouch\",\"op\":\"add\",\"scale\":2.0}]}"
                "}]}";
            Synth::Instrument         routed_instr                                 = {};
            Synth::EnvelopeDescriptor routed_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      routed_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  routed_env_count                             = 999;
            uint32_t                  routed_lfo_count                             = 999;
            TEST(Synth::decode_instrument_json(routed_doc,
                                               static_cast<uint32_t>(strlen(routed_doc)),
                                               &routed_instr,
                                               routed_envs,
                                               &routed_env_count,
                                               routed_lfos,
                                               &routed_lfo_count));

            // Expected routed duty/mix/FM bindings: one table drives the
            // decode, remap and round-trip assertions below.
            struct RoutedBindingExpect {
                Synth::ModTarget target;
                float            base;
                Synth::ModSource source;
                Synth::SourceOp  op;
                float            scale;
            };

            // fm_index is 0..1 in the document, 0..2*pi radians in the bank,
            // so its expected base carries the radian conversion.
            static const RoutedBindingExpect routed_expect[] = {
                { Synth::mod_duty0, 0.25f, Synth::ModSource::velocity, Synth::SourceOp::add, 0.5f },
                { Synth::mod_duty1, 0.5f, Synth::ModSource::mod_wheel, Synth::SourceOp::multiply, -0.25f },
                { Synth::mod_osc_mix, 0.75f, Synth::ModSource::channel_pressure, Synth::SourceOp::add, 0.1f },
                { Synth::mod_fm_index, 0.5f * 6.2831853f, Synth::ModSource::aftertouch, Synth::SourceOp::add, 2.0f },
            };
            const auto check_routed_bindings = [](const Synth::Instrument& instrument) {
                for (uint32_t r = 0; r < sizeof(routed_expect) / sizeof(routed_expect[0]); ++r) {
                    const Synth::TargetBinding& binding = instrument.layers[0].targets[routed_expect[r].target];
                    TEST(binding.base_value == routed_expect[r].base);
                    TEST(binding.num_inputs == 1);
                    TEST(binding.inputs[0].source == routed_expect[r].source);
                    TEST(binding.inputs[0].op == routed_expect[r].op);
                    TEST(binding.inputs[0].scale == routed_expect[r].scale);
                }
            };
            check_routed_bindings(routed_instr);

            // Install into a bank through the remap helpers: the routed inputs must survive.
            const std::unique_ptr<Synth::InstrumentBank> routed_bank_storage =
                std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank& routed_bank = *routed_bank_storage;
            memset(static_cast<void*>(&routed_bank), 0, sizeof(routed_bank));
            uint16_t routed_env_ids[Synth::instrument_max_envelopes];
            uint16_t routed_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&routed_bank, routed_envs, routed_env_count, routed_env_ids));
            TEST(Synth::remap_lfos(&routed_bank, routed_lfos, routed_lfo_count, routed_lfo_ids));
            Synth::Instrument routed_installed;
            Synth::remap_instrument(routed_instr, routed_env_ids, routed_lfo_ids, &routed_installed);
            check_routed_bindings(routed_installed);
            const uint32_t routed_slot = routed_bank.instruments.allocate();
            TEST(routed_slot != pool_no_slot);
            routed_bank.instruments.entries[routed_slot] = routed_installed;
            routed_bank.channel_zones[0][0].start_note   = 1;
            routed_bank.channel_zones[0][0].instrument   = static_cast<uint8_t>(routed_slot);
            TEST(Synth::validate_instrument_bank(&routed_bank));

            // Round-trip through the document JSON: encode, decode, re-assert.
            const std::unique_ptr<FixedArray<char, 64 * 1024>> routed_text_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&routed_text)[64 * 1024] = routed_text_storage->elements;
            const uint32_t routed_text_len =
                Synth::encode_instrument_json(routed_text, sizeof(routed_text), &routed_installed, &routed_bank);
            TEST(routed_text_len > 0);
            Synth::Instrument         routed_back                                       = {};
            Synth::EnvelopeDescriptor routed_back_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      routed_back_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  routed_back_env_count                             = 0;
            uint32_t                  routed_back_lfo_count                             = 0;
            TEST(Synth::decode_instrument_json(routed_text,
                                               routed_text_len,
                                               &routed_back,
                                               routed_back_envs,
                                               &routed_back_env_count,
                                               routed_back_lfos,
                                               &routed_back_lfo_count));
            check_routed_bindings(routed_back);
        }

        // Per-layer routing in the document format (AC4): later layers carry
        // their own base and sources, and decode preserves the divergence.
        {
            Synth::Instrument doc_instr = {};
            memset(static_cast<void*>(&doc_instr), 0, sizeof(doc_instr));
            doc_instr.layer_count = 3;
            set_default_dispersion_sections(doc_instr);
            for (uint32_t layer = 0; layer < 3; layer++) {
                doc_instr.layers[layer].osc_type[0]       = Synth::WaveType::sine_wave;
                doc_instr.layers[layer].fir_slope_percent = 100;
            }
            // Layer 0: two ordered volume sources, duty B and mix bases.
            doc_instr.layers[0].targets[Synth::mod_volume].base_value  = 0.8f;
            doc_instr.layers[0].targets[Synth::mod_volume].num_inputs  = 2;
            doc_instr.layers[0].targets[Synth::mod_volume].inputs[0]   = { Synth::ModSource::velocity,
                                                                           Synth::SourceOp::add,
                                                                           0,
                                                                           0.5f };
            doc_instr.layers[0].targets[Synth::mod_volume].inputs[1]   = { Synth::ModSource::mod_wheel,
                                                                           Synth::SourceOp::multiply,
                                                                           0,
                                                                           -0.25f };
            doc_instr.layers[0].targets[Synth::mod_duty1].base_value   = 0.5f;
            doc_instr.layers[0].targets[Synth::mod_osc_mix].base_value = 0.75f;
            // Layer 1: one volume source, its own duty B base.
            doc_instr.layers[1].targets[Synth::mod_volume].base_value = 0.3f;
            doc_instr.layers[1].targets[Synth::mod_volume].num_inputs = 1;
            doc_instr.layers[1].targets[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity,
                                                                          Synth::SourceOp::add,
                                                                          0,
                                                                          0.2f };
            doc_instr.layers[1].targets[Synth::mod_duty1].base_value  = 0.25f;
            // Layer 2 carries no routing at all.

            const std::unique_ptr<Synth::InstrumentBank> desc_bank_storage = std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank&                       desc_bank         = *desc_bank_storage;
            memset(static_cast<void*>(&desc_bank), 0, sizeof(desc_bank));
            const std::unique_ptr<FixedArray<char, 64 * 1024>> doc_text_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&doc_text)[64 * 1024] = doc_text_storage->elements;
            const uint32_t doc_len = Synth::encode_instrument_json(doc_text, sizeof(doc_text), &doc_instr, &desc_bank);
            TEST(doc_len > 0);
            Synth::Instrument         doc_back                                       = {};
            Synth::EnvelopeDescriptor doc_back_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      doc_back_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  doc_back_env_count                             = 0;
            uint32_t                  doc_back_lfo_count                             = 0;
            TEST(Synth::decode_instrument_json(doc_text,
                                               doc_len,
                                               &doc_back,
                                               doc_back_envs,
                                               &doc_back_env_count,
                                               doc_back_lfos,
                                               &doc_back_lfo_count));
            TEST(doc_back.layers[0].targets[Synth::mod_volume].base_value == 0.8f);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].num_inputs == 2);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].inputs[0].scale == 0.5f);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].inputs[1].source == Synth::ModSource::mod_wheel);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].inputs[1].op == Synth::SourceOp::multiply);
            TEST(doc_back.layers[0].targets[Synth::mod_volume].inputs[1].scale == -0.25f);
            TEST(doc_back.layers[0].targets[Synth::mod_duty1].base_value == 0.5f);
            TEST(doc_back.layers[0].targets[Synth::mod_osc_mix].base_value == 0.75f);
            TEST(doc_back.layers[1].targets[Synth::mod_volume].base_value == 0.3f);
            TEST(doc_back.layers[1].targets[Synth::mod_volume].num_inputs == 1);
            TEST(doc_back.layers[1].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::velocity);
            TEST(doc_back.layers[1].targets[Synth::mod_volume].inputs[0].scale == 0.2f);
            TEST(doc_back.layers[1].targets[Synth::mod_duty1].base_value == 0.25f);
            TEST(doc_back.layers[1].targets[Synth::mod_duty1].num_inputs == 0);
            TEST(doc_back.layers[2].targets[Synth::mod_volume].base_value == 0.0f);
            TEST(doc_back.layers[2].targets[Synth::mod_volume].num_inputs == 0);
            TEST(doc_back.layers[2].targets[Synth::mod_duty1].base_value == 0.0f);
            TEST(doc_back.layers[2].targets[Synth::mod_osc_mix].base_value == 0.0f);
        }

        // Pasting over a populated zone drops its stale graph records; channel-wide
        // effect layouts (kind 4), master-chain records, and other zones stay
        // untouched, and the clear owns the zone's mask row.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> clear_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& clear_bank = *clear_bank_storage;
            memset(static_cast<void*>(&clear_bank), 0, sizeof(clear_bank));
            add_detached_record(&clear_bank, 0, 0, 1, 1, 0.0f, 0.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
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
                                0);                   // master-chain effect layout
            clear_bank.graph_osc_source[0][0][0] = 5; // the source row belongs to the clear

            Sculptor::zone_records_clear_zone(&clear_bank, 0, 0);

            TEST(clear_bank.graph_layout_count == 4);
            TEST(count_records_matching(clear_bank, 0, 0, 1, 1) == 0);
            TEST(count_records_matching(clear_bank, 0, 0, 3, 0) == 0);
            TEST(count_records_matching(clear_bank, 0, 1, 1, 1) == 1);
            TEST(count_records_matching(clear_bank, 1, 0, 1, 1) == 1);
            TEST(clear_bank.graph_osc_source[0][0][0] == 0);
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
            // spread base above the slider ceiling: the true double value is out of
            // range even though its binary32 narrowing would round back into it
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":36.0000001}}]}",
            // negative spread underflow: the double is below 0 while its binary32
            // narrowing is negative zero, which a post-narrowing check would accept
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":-1e-50}}]}",
            // spread base out of range, plain spellings
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":37}}]}",
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":-0.5}}]}",
            // duplicate
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"wave_a\":\"sawtooth\"}]}",
            // level span overflows the stored delta
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":-3e38},{\"time_seconds\":0.1,\"level\":3e38}]}}}]}",
            // LFO span overflows the stored delta
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":5.0,\"min_level\":-3e38,\"max_level\":3e38,\"depth\":0.5}}}]}",
            // LFO span overflows the bank-unit conversion
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fm_index\":{\"lfo\":{\"wave\":\"sine\",\"frequency_hz\":5.0,\"max_level\":3e38,\"depth\":0.5}}}]}",
        };
        Synth::Instrument         reference_instr                                 = {};
        Synth::EnvelopeDescriptor reference_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      reference_lfos[Synth::instrument_max_lfos]      = {};
        memset(static_cast<void*>(&reference_instr), 0x5A, sizeof(reference_instr));
        memset(static_cast<void*>(reference_envs), 0x5A, sizeof(reference_envs));
        memset(static_cast<void*>(reference_lfos), 0x5A, sizeof(reference_lfos));
        for (uint32_t i = 0; i < sizeof(rejected_docs) / sizeof(rejected_docs[0]); i++) {
            Synth::Instrument sentinel_instr;
            memset(static_cast<void*>(&sentinel_instr), 0x5A, sizeof(sentinel_instr));
            Synth::EnvelopeDescriptor sentinel_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      sentinel_lfos[Synth::instrument_max_lfos]      = {};
            memset(static_cast<void*>(sentinel_envs), 0x5A, sizeof(sentinel_envs));
            memset(static_cast<void*>(sentinel_lfos), 0x5A, sizeof(sentinel_lfos));
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

        // A zero ratio is bank-legal in FM/sync, a spread base of 0 is the
        // bypass point, and a spread envelope level can quantize just below
        // the slider floor; all of them must decode, and a copy must paste
        // back exactly.
        {
            const char* const zero_ratio_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mode\":\"fm\",\"mod_ratio\":0.0}]}";
            Synth::Instrument         zero_instr                                 = {};
            Synth::EnvelopeDescriptor zero_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      zero_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  zero_env_count                             = 999;
            uint32_t                  zero_lfo_count                             = 999;
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
                const std::unique_ptr<Synth::InstrumentEditorBank> zero_bank_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& zero_bank = *zero_bank_storage;
                memset(static_cast<void*>(&zero_bank), 0, sizeof(zero_bank));
                const uint32_t zero_slot = zero_bank.bank.instruments.allocate();
                TEST(zero_slot != pool_no_slot);
                zero_bank.bank.instruments.entries[zero_slot] = zero_instr;
                const std::unique_ptr<FixedArray<char, 64 * 1024>> zero_text_storage =
                    std::make_unique<FixedArray<char, 64 * 1024>>();
                char (&zero_text)[64 * 1024] = zero_text_storage->elements;
                const uint32_t zero_text_len =
                    Synth::encode_instrument_json(zero_text, sizeof(zero_text), &zero_instr, &zero_bank.bank);
                TEST(zero_text_len > 0);
                Synth::Instrument         zero_back                                       = {};
                Synth::EnvelopeDescriptor zero_back_envs[Synth::instrument_max_envelopes] = {};
                Synth::LFODescriptor      zero_back_lfos[Synth::instrument_max_lfos]      = {};
                uint32_t                  zero_back_env_count                             = 0;
                uint32_t                  zero_back_lfo_count                             = 0;
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

            const char* const spread_zero_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":0.0}}]}";
            TEST(Synth::decode_instrument_json(spread_zero_doc,
                                               static_cast<uint32_t>(strlen(spread_zero_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.layers[0].targets[Synth::mod_fir_spread].base_value == 0.0f);

            const char* const spread_max_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":36.0}}]}";
            TEST(Synth::decode_instrument_json(spread_max_doc,
                                               static_cast<uint32_t>(strlen(spread_max_doc)),
                                               &zero_instr,
                                               zero_envs,
                                               &zero_env_count,
                                               zero_lfos,
                                               &zero_lfo_count));
            TEST(zero_instr.layers[0].targets[Synth::mod_fir_spread].base_value == 36.0f);

            // The level-2 point quantizes to raw 4369, which encodes to just
            // below 2 and must re-quantize to the same raw value.

            const char* const spread_env_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"envelope\":{\"points\":[{\"time_seconds\":0.0,\"level\":0.0},{\"time_seconds\":0.1,\"level\":2.0},{\"time_seconds\":0.2,\"level\":30.0}]}}}]}";
            Synth::EnvelopeDescriptor spread_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      spread_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  spread_env_count                             = 999;
            uint32_t                  spread_lfo_count                             = 999;
            TEST(Synth::decode_instrument_json(spread_env_doc,
                                               static_cast<uint32_t>(strlen(spread_env_doc)),
                                               &zero_instr,
                                               spread_envs,
                                               &spread_env_count,
                                               spread_lfos,
                                               &spread_lfo_count));
            TEST(spread_env_count == 1);
            TEST(spread_envs[0].min_value == 0.0f);
            TEST(spread_envs[0].points[1].value == 4369); // the level-2 point's own quantization step

            const std::unique_ptr<Synth::InstrumentEditorBank> spread_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& spread_bank = *spread_bank_storage;
            memset(static_cast<void*>(&spread_bank), 0, sizeof(spread_bank));
            uint16_t spread_env_ids[Synth::instrument_max_envelopes];
            uint16_t spread_lfo_ids[Synth::instrument_max_lfos];
            TEST(Synth::remap_envelopes(&spread_bank.bank, spread_envs, spread_env_count, spread_env_ids));
            TEST(Synth::remap_lfos(&spread_bank.bank, spread_lfos, spread_lfo_count, spread_lfo_ids));
            Synth::Instrument spread_installed;
            Synth::remap_instrument(zero_instr, spread_env_ids, spread_lfo_ids, &spread_installed);
            const uint32_t spread_slot = spread_bank.bank.instruments.allocate();
            TEST(spread_slot != pool_no_slot);
            spread_bank.bank.instruments.entries[spread_slot] = spread_installed;
            spread_bank.bank.channel_enabled[0]               = 1;
            spread_bank.bank.channel_zones[0][0].start_note   = 1;
            spread_bank.bank.channel_zones[0][0].instrument   = static_cast<uint8_t>(spread_slot);
            TEST(Synth::validate_instrument_bank(&spread_bank.bank));
            const std::unique_ptr<FixedArray<char, 64 * 1024>> spread_text_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&spread_text)[64 * 1024] = spread_text_storage->elements;
            const uint32_t spread_text_len =
                Synth::encode_instrument_json(spread_text, sizeof(spread_text), &spread_installed, &spread_bank.bank);
            TEST(spread_text_len > 0);
            Synth::Instrument         spread_back                                       = {};
            Synth::EnvelopeDescriptor spread_back_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      spread_back_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  spread_back_env_count                             = 0;
            uint32_t                  spread_back_lfo_count                             = 0;
            TEST(Synth::decode_instrument_json(spread_text,
                                               spread_text_len,
                                               &spread_back,
                                               spread_back_envs,
                                               &spread_back_env_count,
                                               spread_back_lfos,
                                               &spread_back_lfo_count));
            TEST(spread_back_env_count == 1);
            TEST(spread_back_envs[0].points[1].value == spread_envs[0].points[1].value);
            TEST(spread_back_envs[0].min_value == spread_envs[0].min_value);
            TEST(spread_back_envs[0].min_max_delta == spread_envs[0].min_max_delta);
        }
        // Editor dialect: a fractional spread base decodes exactly, and an
        // absent key decodes the zeroed binding.
        const char* const spread_half_doc =
            "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":2.5}}]}";
        const char* const no_spread_doc  = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}]}";
        const char* const spread_docs[2] = { spread_half_doc, no_spread_doc };
        for (const char* const spread_doc : spread_docs) {
            Synth::Instrument         half_instr                                 = {};
            Synth::EnvelopeDescriptor half_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      half_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  half_env_count                             = 999;
            uint32_t                  half_lfo_count                             = 999;
            TEST(Synth::decode_instrument_json(spread_doc,
                                               static_cast<uint32_t>(strlen(spread_doc)),
                                               &half_instr,
                                               half_envs,
                                               &half_env_count,
                                               half_lfos,
                                               &half_lfo_count));
            TEST(half_instr.layers[0].targets[Synth::mod_fir_spread].base_value ==
                 (spread_doc == spread_half_doc ? 2.5f : 0.0f));
        }
        // Removed cutoff keys: both dialects ignore them, and
        // the spread target stays at its zeroed default.
        const char* const removed_keys_doc = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                                             "\"lowpass_cutoff\":{\"base\":2000},\"highpass_cutoff\":{\"base\":500}}]}";
        {
            Synth::Instrument         removed_instr                                 = {};
            Synth::EnvelopeDescriptor removed_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      removed_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  removed_env_count                             = 999;
            uint32_t                  removed_lfo_count                             = 999;
            TEST(Synth::decode_instrument_json(removed_keys_doc,
                                               static_cast<uint32_t>(strlen(removed_keys_doc)),
                                               &removed_instr,
                                               removed_envs,
                                               &removed_env_count,
                                               removed_lfos,
                                               &removed_lfo_count));
            TEST(removed_instr.layers[0].targets[Synth::mod_fir_spread].base_value == 0.0f);
        }
        // Saved-bank dialect: a spread base round-trips, out-of-range and
        // underflow spellings fail before narrowing, and an absent key
        // decodes 0.  The splice mirrors the octave-offset pattern above.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> spread_splice_source_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& spread_splice_source = *spread_splice_source_storage;
            build_named_fm_fixture(&spread_splice_source);
            spread_splice_source.bank.instruments.entries[0].layers[0].fir_type = Synth::fir_bandpass;
            spread_splice_source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].base_value = 3.0f;
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spread_payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&spread_payload)[Synth::library_payload_max] = spread_payload_storage->elements;
            const uint32_t spread_payload_len =
                Synth::encode_editor_bank_json(&spread_splice_source, spread_payload, sizeof(spread_payload));
            TEST(spread_payload_len > 0);
            auto decode_with_spread_spelling = [&](const char* spelling) {
                const char* const key = "\"fir_pitch_spread\":{\"base_value\":";
                const char* const hit = strstr(spread_payload, key);
                TEST(hit != nullptr);
                if (! hit) {
                    return false;
                }
                const char* const value_begin = hit + strlen(key);
                const char*       value_end   = value_begin;
                while (*value_end && *value_end != ',' && *value_end != '}') {
                    ++value_end;
                }
                const uint32_t prefix       = static_cast<uint32_t>(hit - spread_payload);
                const uint32_t suffix       = spread_payload_len - static_cast<uint32_t>(value_end - spread_payload);
                const uint32_t spelling_len = static_cast<uint32_t>(strlen(spelling));
                const uint32_t key_len      = static_cast<uint32_t>(strlen(key));

                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spelling_doc_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spelling_doc)[Synth::library_payload_max] = spelling_doc_storage->elements;
                memcpy(spelling_doc, spread_payload, prefix);
                memcpy(spelling_doc + prefix, key, key_len);
                memcpy(spelling_doc + prefix + key_len, spelling, spelling_len);
                memcpy(spelling_doc + prefix + key_len + spelling_len, value_end, suffix);
                const std::unique_ptr<Synth::InstrumentEditorBank> spelling_decoded_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& spelling_decoded = *spelling_decoded_storage;
                memset(static_cast<void*>(&spelling_decoded), 0, sizeof(spelling_decoded));
                return Synth::decode_editor_bank_json(spelling_doc,
                                                      prefix + key_len + spelling_len + suffix,
                                                      &spelling_decoded);
            };
            TEST(decode_with_spread_spelling("3"));   // the splice itself decodes
            TEST(decode_with_spread_spelling("2.5")); // a fractional base round-trips
            TEST(! decode_with_spread_spelling("37"));
            TEST(! decode_with_spread_spelling("-0.5"));
            TEST(! decode_with_spread_spelling("36.0000001"));
            TEST(! decode_with_spread_spelling("-1e-50"));
            // An absent key decodes the zeroed binding: a zero base omits the
            // key entirely.
            spread_splice_source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].base_value = 0.0f;
            const uint32_t zero_payload_len =
                Synth::encode_editor_bank_json(&spread_splice_source, spread_payload, sizeof(spread_payload));
            TEST(zero_payload_len > 0);
            TEST(strstr(spread_payload, "fir_pitch_spread") == nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> zero_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& zero_decoded = *zero_decoded_storage;
            memset(static_cast<void*>(&zero_decoded), 0, sizeof(zero_decoded));
            TEST(Synth::decode_editor_bank_json(spread_payload, zero_payload_len, &zero_decoded));
            TEST(zero_decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].base_value == 0.0f);
            // The removed cutoff keys stay tolerated under the bank dialect's
            // routing object too: the splice must still decode.
            spread_splice_source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value = 0.8f;
            const uint32_t routing_payload_len =
                Synth::encode_editor_bank_json(&spread_splice_source, spread_payload, sizeof(spread_payload));
            TEST(routing_payload_len > 0);
            const char* const routing_key = "\"routing\":{";
            const char* const routing_hit = strstr(spread_payload, routing_key);
            TEST(routing_hit != nullptr);
            if (routing_hit) {
                const char* const removed_keys =
                    "\"lowpass_cutoff\":{\"base_value\":2000},\"highpass_cutoff\":{\"base_value\":500},";
                const uint32_t routing_key_len = static_cast<uint32_t>(strlen(routing_key));
                const uint32_t prefix          = static_cast<uint32_t>(routing_hit - spread_payload) + routing_key_len;
                const uint32_t suffix          = routing_payload_len - prefix;
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> routing_doc_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&routing_doc)[Synth::library_payload_max] = routing_doc_storage->elements;
                memcpy(routing_doc, spread_payload, prefix);
                const int removed_len =
                    snprintf(routing_doc + prefix, sizeof(routing_doc) - prefix, "%s", removed_keys);
                TEST(removed_len > 0);
                memcpy(routing_doc + prefix + removed_len, spread_payload + prefix, suffix);
                const std::unique_ptr<Synth::InstrumentEditorBank> routing_decoded_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& routing_decoded = *routing_decoded_storage;
                memset(static_cast<void*>(&routing_decoded), 0, sizeof(routing_decoded));
                TEST(Synth::decode_editor_bank_json(routing_doc,
                                                    prefix + static_cast<uint32_t>(removed_len) + suffix,
                                                    &routing_decoded));
            }
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
                "\"osc_mix\":{\"base\":0},\"fm_index\":{\"base\":0},"
                "\"fir_pitch_spread\":{\"base\":3}}]}";
            Synth::Instrument         real_instr                                 = {};
            Synth::EnvelopeDescriptor real_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      real_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  real_env_count                             = 999;
            uint32_t                  real_lfo_count                             = 999;
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

            const std::unique_ptr<Synth::InstrumentEditorBank> real_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& real_bank = *real_bank_storage;
            memset(static_cast<void*>(&real_bank), 0, sizeof(real_bank));
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
            const std::unique_ptr<FixedArray<char, 64 * 1024>> real_text_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&real_text)[64 * 1024] = real_text_storage->elements;
            const uint32_t real_text_len =
                Synth::encode_instrument_json(real_text, sizeof(real_text), &real_installed, &real_bank.bank);
            TEST(real_text_len > 0);
            Synth::Instrument         real_back                                       = {};
            Synth::EnvelopeDescriptor real_back_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      real_back_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  real_back_env_count                             = 0;
            uint32_t                  real_back_lfo_count                             = 0;
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
            Synth::Instrument         swing_instr                                 = {};
            Synth::EnvelopeDescriptor swing_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      swing_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  swing_env_count                             = 999;
            uint32_t                  swing_lfo_count                             = 999;
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
            const std::unique_ptr<Synth::InstrumentEditorBank> swing_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& swing_bank = *swing_bank_storage;
            memset(static_cast<void*>(&swing_bank), 0, sizeof(swing_bank));
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
            // A moved canonical, envelope or LFO node survives export,
            // encode, decode and mapping, and lands back on the decoded node
            // identity rather than on a numeric descriptor id.
            const std::unique_ptr<Synth::InstrumentEditorBank> ex_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&                                ex_bank  = *ex_bank_storage;
            Synth::Instrument                                           ex_instr = {};
            Synth::InstrumentGraphLayout                                ex_portable[max_portable_records]      = {};
            Synth::GraphNodeLayout                                      ex_mapped[max_portable_records]        = {};
            Synth::GraphNodeLayout                                      ex_replay_mapped[max_portable_records] = {};
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ex_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ex_text)[clipboard_capacity] = ex_text_storage->elements;
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
            ex_bank.graph_layout[1].width_override  = 220.0f;
            ex_bank.graph_layout[1].height_override = 120.0f;
            TEST(Sculptor::validate_editor_metadata(ex_bank));

            uint32_t ex_portable_count = max_portable_records;
            TEST(Sculptor::encode_instrument_graph_layout(ex_instr,
                                                          ex_bank.bank,
                                                          ex_bank,
                                                          0,
                                                          0,
                                                          ex_portable,
                                                          max_portable_records,
                                                          &ex_portable_count,
                                                          Synth::InstrumentJsonDomain::validated_bank));
            TEST(ex_portable_count == 3);
            int32_t rec =
                find_portable(ex_portable, ex_portable_count, Synth::instrument_graph_layout_canonical, 0xFFu, 0xFFu);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].canonical_index == 1);
            if (rec >= 0)
                TEST(ex_portable[rec].x == 64.0f && ex_portable[rec].y == 32.0f);
            rec = find_portable(ex_portable,
                                ex_portable_count,
                                Synth::instrument_graph_layout_envelope,
                                0,
                                Synth::mod_volume);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].x == 120.5f && ex_portable[rec].width_override == 220.0f);
            if (rec >= 0)
                TEST(ex_portable[rec].height_override == 120.0f);
            rec =
                find_portable(ex_portable, ex_portable_count, Synth::instrument_graph_layout_lfo, 0, Synth::mod_volume);
            TEST(rec >= 0);
            if (rec >= 0)
                TEST(ex_portable[rec].depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
            if (rec >= 0)
                TEST(ex_portable[rec].rate_source == static_cast<uint8_t>(Synth::ModSource::mod_wheel));

            // Exporter refusals leave the record sink and the count untouched: a
            // destination outside the zone table, and a capacity that cannot hold what
            // this zone exports.
            Synth::InstrumentGraphLayout ex_refuse[max_portable_records]          = {};
            Synth::InstrumentGraphLayout ex_refuse_sentinel[max_portable_records] = {};
            memset(static_cast<void*>(ex_refuse), 0x5A, sizeof(ex_refuse));
            memset(static_cast<void*>(ex_refuse_sentinel), 0x5A, sizeof(ex_refuse_sentinel));
            constexpr uint32_t ex_refuse_capacity = sizeof(ex_refuse) / sizeof(ex_refuse[0]);
            uint32_t           ex_refuse_count    = ex_refuse_capacity;
            TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                            ex_bank.bank,
                                                            ex_bank,
                                                            Synth::max_channels, // no such channel
                                                            0,
                                                            ex_refuse,
                                                            ex_refuse_capacity,
                                                            &ex_refuse_count,
                                                            Synth::InstrumentJsonDomain::validated_bank));
            TEST(ex_refuse_count == ex_refuse_capacity);
            TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);
            TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                            ex_bank.bank,
                                                            ex_bank,
                                                            0,
                                                            Synth::max_instr_per_channel, // no such zone
                                                            ex_refuse,
                                                            ex_refuse_capacity,
                                                            &ex_refuse_count,
                                                            Synth::InstrumentJsonDomain::validated_bank));
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
                                                            &ex_refuse_count,
                                                            Synth::InstrumentJsonDomain::validated_bank));
            TEST(ex_refuse_count == ex_refuse_capacity);
            TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);
            // Encode-side exemption controls, called directly so they reach the
            // encoder's own bound rather than the metadata validation.
            {
                // An unrepresented kind-1 record whose descriptor id exceeds the
                // envelope pool's allocation is refused by the encoder: the bound
                // sits in its metadata precondition and again at the exemption
                // itself.
                const std::unique_ptr<Synth::InstrumentEditorBank> ex_over_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& ex_over = *ex_over_storage;
                ex_over                              = ex_bank;
                add_detached_record(&ex_over,
                                    0,
                                    0,
                                    1,
                                    5,
                                    71.0f,
                                    72.0f,
                                    Synth::ModSource::none,
                                    Synth::ModSource::none,
                                    1);
                TEST(! Sculptor::encode_instrument_graph_layout(ex_instr,
                                                                ex_over.bank,
                                                                ex_over,
                                                                0,
                                                                0,
                                                                ex_refuse,
                                                                ex_refuse_capacity,
                                                                &ex_refuse_count,
                                                                Synth::InstrumentJsonDomain::validated_bank));
                TEST(ex_refuse_count == ex_refuse_capacity);
                TEST(memcmp(ex_refuse, ex_refuse_sentinel, sizeof(ex_refuse_sentinel)) == 0);
                // An unrepresented kind-1 record with a validly allocated
                // descriptor id takes the exemption: the export succeeds and the
                // dropped record produces no output.
                const std::unique_ptr<Synth::InstrumentEditorBank> ex_alloc_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& ex_alloc = *ex_alloc_storage;
                ex_alloc                              = ex_bank;
                TEST(ex_alloc.bank.envelopes.allocate() == 2);
                ex_alloc.bank.envelopes.entries[2] = ex_alloc.bank.envelopes.entries[0];
                add_detached_record(&ex_alloc,
                                    0,
                                    0,
                                    1,
                                    3,
                                    71.0f,
                                    72.0f,
                                    Synth::ModSource::none,
                                    Synth::ModSource::none,
                                    1);
                Synth::InstrumentGraphLayout ex_alloc_out[max_portable_records] = {};
                uint32_t                     ex_alloc_count                     = max_portable_records;
                TEST(Sculptor::encode_instrument_graph_layout(ex_instr,
                                                              ex_alloc.bank,
                                                              ex_alloc,
                                                              0,
                                                              0,
                                                              ex_alloc_out,
                                                              max_portable_records,
                                                              &ex_alloc_count,
                                                              Synth::InstrumentJsonDomain::validated_bank));
                TEST(ex_alloc_count == ex_portable_count);
                TEST(memcmp(ex_alloc_out, ex_portable, ex_portable_count * sizeof(ex_portable[0])) == 0);
            }

            const std::unique_ptr<Synth::InstrumentEditorBank> ex_before_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& ex_before = *ex_before_storage;
            ex_before                              = ex_bank;
            const uint32_t ex_len                  = Synth::encode_instrument_json(ex_text,
                                                                                   sizeof(ex_text),
                                                                                   &ex_instr,
                                                                                   &ex_bank.bank,
                                                                                   ex_portable,
                                                                                   ex_portable_count);
            TEST(ex_len > 0);
            TEST(ex_len + 1 <= clipboard_capacity);
            TEST(strstr(ex_text, "\"graph_layout\":[") != nullptr);
            TEST(memcmp(&ex_bank, &ex_before, sizeof(ex_bank)) == 0);

            const std::unique_ptr<ClipboardDecode> ex_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ex_dec         = *ex_dec_storage;
            fill_clipboard_sentinels(&ex_dec);
            TEST(decode_clipboard(&ex_dec, ex_text, max_portable_records));
            TEST(ex_dec.layout_count == ex_portable_count);
            TEST(ex_dec.env_count == 2);
            TEST(ex_dec.lfo_count == 1);
            TEST(ex_dec.instr.layer_count == 1);
            TEST(ex_dec.instr.layers[0].osc_mode == Synth::osc_mode_fm);
            TEST(ex_dec.instr.layers[0].mod_ratio == 2.0f);
            TEST(ex_dec.instr.layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
            TEST(ex_dec.instr.layers[0].targets[Synth::mod_volume].lfo_desc_id == 1);
            TEST(ex_dec.instr.layers[0].targets[Synth::mod_pitch].envelope_desc_id == 2);
            TEST(ex_dec.instr.layers[0].targets[Synth::mod_volume].base_value == 0.8f);
            TEST(ex_dec.envs[0].num_points == 3);
            TEST(ex_dec.envs[1].num_points == 3);
            TEST(ex_dec.lfos[0].period_ms == 250);
            TEST(ex_dec.layout[0].kind == Synth::instrument_graph_layout_canonical);
            TEST(find_portable(ex_dec.layout,
                               ex_dec.layout_count,
                               Synth::instrument_graph_layout_envelope,
                               0,
                               Synth::mod_volume) >= 0);

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
            TEST(ex_mapped_count == 3);
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

            // Mapper refusals: a decoded locator the decoded model does not project, and
            // a kind that is not clipboard data.  Both leave the mapped sink and the count
            // untouched; the same sink accepts the resolved records above, so only the
            // locator differs.
            Synth::InstrumentGraphLayout mp_records[1]       = {};
            Synth::GraphNodeLayout       mp_sink[4]          = {};
            Synth::GraphNodeLayout       mp_sink_sentinel[4] = {};
            memset(static_cast<void*>(mp_sink), 0x5A, sizeof(mp_sink));
            memset(static_cast<void*>(mp_sink_sentinel), 0x5A, sizeof(mp_sink_sentinel));
            constexpr uint32_t mp_capacity = sizeof(mp_sink) / sizeof(mp_sink[0]);
            uint32_t           mp_count    = mp_capacity;
            const uint32_t     mp_cases    = 2;
            for (uint32_t c = 0; c < mp_cases; c++) {
                memset(static_cast<void*>(mp_records), 0, sizeof(mp_records));
                if (c == 0) {
                    // A canonical index past the projection is unresolvable debris
                    // the mapping refuses: only kinds 1/2 are droppable.
                    mp_records[0].kind            = Synth::instrument_graph_layout_canonical;
                    mp_records[0].canonical_index = 9;
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
            const std::unique_ptr<Synth::InstrumentBank> ex_replay_bank_storage =
                std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank& ex_replay_bank = *ex_replay_bank_storage;
            memset(static_cast<void*>(&ex_replay_bank), 0, sizeof(ex_replay_bank));
            uint16_t ex_env_ids[Synth::instrument_max_envelopes];
            uint16_t ex_lfo_ids[Synth::instrument_max_lfos];
            // The copy helpers below index their map outputs with the decoded
            // instrument generator ids, so the replay runs only on a decode that
            // produced the fixture's expected descriptor counts.
            const bool ex_decoded = ex_dec.env_count == 2 && ex_dec.lfo_count == 1 && ex_dec.layout_count == 3;
            TEST(ex_decoded);
            if (ex_decoded) {
                TEST(Synth::remap_envelopes(&ex_replay_bank, ex_dec.envs, ex_dec.env_count, ex_env_ids));
                TEST(Synth::remap_lfos(&ex_replay_bank, ex_dec.lfos, ex_dec.lfo_count, ex_lfo_ids));
                Synth::Instrument ex_replay_instr = {};
                Synth::remap_instrument(ex_dec.instr, ex_env_ids, ex_lfo_ids, &ex_replay_instr);
                const std::unique_ptr<FixedArray<char, clipboard_capacity>> ex_replay_text_storage =
                    std::make_unique<FixedArray<char, clipboard_capacity>>();
                char (&ex_replay_text)[clipboard_capacity] = ex_replay_text_storage->elements;
                const uint32_t ex_replay_len               = Synth::encode_instrument_json(ex_replay_text,
                                                                                           sizeof(ex_replay_text),
                                                                                           &ex_replay_instr,
                                                                                           &ex_replay_bank,
                                                                                           ex_dec.layout,
                                                                                           ex_dec.layout_count);
                TEST(ex_replay_len > 0);
                const std::unique_ptr<ClipboardDecode> ex_replay_dec_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ex_replay_dec         = *ex_replay_dec_storage;
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
            const std::unique_ptr<Synth::InstrumentEditorBank> rm_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&                       rm_bank = *rm_bank_storage;
            const std::unique_ptr<Synth::InstrumentEditorBank> rm_parallel_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& rm_parallel             = *rm_parallel_storage;
            uint16_t                     rm_map[Synth::max_lfos] = {};
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
            TEST(rm_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].lfo_desc_id == 1);
            TEST(Synth::validate_instrument_bank(&rm_bank.bank));

            // The one-argument overload is the same transaction without the map.
            Synth::reclaim_unused_slots(&rm_parallel);
            TEST(memcmp(&rm_parallel, &rm_bank, sizeof(rm_bank)) == 0);
        }

        {
            // Descriptor identity, not descriptor id. Two byte-identical envelopes
            // intern into one decoded descriptor, so a record naming either source
            // cell addresses that one node.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> it_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&it_text)[clipboard_capacity] = it_text_storage->elements;
            const char* const it_records =
                "{\"kind\":1,\"layer\":0,\"target\":1,\"x\":33.5,\"y\":4.5,\"width\":0,\"height\":0}";
            build_layout_doc(it_text, sizeof(it_text), clipboard_alias_head, it_records);
            const std::unique_ptr<ClipboardDecode> it_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       it_dec         = *it_dec_storage;
            fill_clipboard_sentinels(&it_dec);
            TEST(decode_clipboard(&it_dec, it_text, max_portable_records));
            TEST(it_dec.env_count == 1); // interning really happened
            TEST(it_dec.instr.layers[0].targets[Synth::mod_volume].envelope_desc_id == 1);
            TEST(it_dec.instr.layers[0].targets[Synth::mod_pitch].envelope_desc_id == 1);
            TEST(it_dec.layout_count == 1);

            Synth::GraphNodeLayout it_mapped[max_portable_records] = {};
            uint32_t               it_mapped_count                 = max_portable_records;
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

            // Every target projects under the all-target model (AC11): an
            // envelope record on duty0 resolves like any other generator record.
            const char* const dm_head =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"duty_a\":"
                "{\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.25},"
                "{\"time_seconds\":0.5,\"level\":0.75}],\"sustain_start\":0,\"sustain_end\":1}}}],";
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> dm_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&dm_text)[clipboard_capacity] = dm_text_storage->elements;
            build_layout_doc(dm_text,
                             sizeof(dm_text),
                             dm_head,
                             "{\"kind\":1,\"layer\":0,\"target\":3,\"x\":10,\"y\":10,\"width\":0,\"height\":0}");
            const std::unique_ptr<ClipboardDecode> dm_attempt_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       dm_attempt         = *dm_attempt_storage;
            fill_clipboard_sentinels(&dm_attempt);
            TEST(decode_clipboard(&dm_attempt, dm_text, max_portable_records));
            TEST(dm_attempt.env_count == 1);
            TEST(dm_attempt.instr.layers[0].targets[Synth::mod_duty0].envelope_desc_id == 1);
            TEST(dm_attempt.layout_count == 1);
            Synth::GraphNodeLayout dm_mapped[max_portable_records] = {};
            uint32_t               dm_mapped_count                 = max_portable_records;
            TEST(Sculptor::map_instrument_graph_layout(dm_attempt.instr,
                                                       dm_attempt.envs,
                                                       dm_attempt.env_count,
                                                       dm_attempt.lfos,
                                                       dm_attempt.lfo_count,
                                                       dm_attempt.layout,
                                                       dm_attempt.layout_count,
                                                       dm_mapped,
                                                       max_portable_records,
                                                       &dm_mapped_count));
            TEST(dm_mapped_count == 1);
            TEST(dm_mapped[0].kind == 1 && dm_mapped[0].index == 1);
            TEST(dm_mapped[0].x == 10.0f && dm_mapped[0].uid != 0);
        }

        {
            // Generator-layout normalization guarantees: an envelope-layout
            // alias collision refuses, a single saved record re-anchors to
            // the decoded node's earliest serving cell, distinct LFO source
            // pairs keep distinct instances and positions, capacity exactly
            // matching the resolved count succeeds, and every refusal leaves
            // the output array and count untouched.
            Synth::EnvelopeDescriptor nm_envs[3]     = {};
            Synth::EnvelopeDescriptor nm_dec_envs[2] = {};
            Synth::Instrument         nm_src         = {};
            Synth::Instrument         nm_dec         = {};
            memset(static_cast<void*>(nm_envs), 0, sizeof(nm_envs));
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
            set_default_dispersion_sections(nm_src);
            nm_dec.layer_count = 3;
            set_default_dispersion_sections(nm_dec);
            for (uint32_t layer = 0; layer < 3; layer++) {
                nm_src.layers[layer].osc_type[0]                                 = Synth::WaveType::sine_wave;
                nm_src.layers[layer].targets[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(layer + 1);
                nm_dec.layers[layer].osc_type[0]                                 = Synth::WaveType::sine_wave;
                nm_dec.layers[layer].targets[Synth::mod_volume].envelope_desc_id = layer == 2 ? 2 : 1;
                nm_src.layers[layer].mod_ratio                                   = 1.0f;
                nm_dec.layers[layer].mod_ratio                                   = 1.0f;
            }
            TEST(validate_standalone_model(nm_src, nm_envs, 3, nullptr, 0));
            TEST(validate_standalone_model(nm_dec, nm_dec_envs, 2, nullptr, 0));

            // Envelope aliases with two saved source layouts cannot collapse losslessly:
            // both source records resolve to the one decoded envelope node.
            Synth::InstrumentGraphLayout nm_env_records[2] = {};
            memset(static_cast<void*>(nm_env_records), 0, sizeof(nm_env_records));
            nm_env_records[0].kind                     = Synth::instrument_graph_layout_envelope;
            nm_env_records[0].layer                    = 1; // A'
            nm_env_records[0].target                   = static_cast<uint8_t>(Synth::mod_volume);
            nm_env_records[0].x                        = 33.5f;
            nm_env_records[1].kind                     = Synth::instrument_graph_layout_envelope;
            nm_env_records[1].layer                    = 0; // A
            nm_env_records[1].target                   = static_cast<uint8_t>(Synth::mod_volume);
            nm_env_records[1].x                        = 11.5f;
            Synth::InstrumentGraphLayout nm_env_out[4] = {};
            uint32_t                     nm_env_count  = 4;
            TEST(! Sculptor::normalize_instrument_graph_layout(nm_src,
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
            TEST(nm_env_count == 4);
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
            // pair are two distinct projected nodes, so each keeps its own saved position
            // and its own source pair. Capacity exactly equal to the resolved count
            // succeeds; one slot less refuses with the record sink and the count
            // untouched.
            Synth::LFODescriptor na_lfos[1] = {};
            memset(static_cast<void*>(na_lfos), 0, sizeof(na_lfos));
            na_lfos[0].wave          = Synth::WaveType::sine_wave;
            na_lfos[0].duty          = 0x40;
            na_lfos[0].period_ms     = 300;
            na_lfos[0].min_value     = -1.0f;
            na_lfos[0].min_max_delta = 2.0f;

            Synth::Instrument na_src = {};
            Synth::Instrument na_dec = {};
            na_src.layer_count       = 2;
            set_default_dispersion_sections(na_src);
            na_dec.layer_count = 2;
            set_default_dispersion_sections(na_dec);
            for (uint32_t layer = 0; layer < 2; layer++) {
                na_src.layers[layer].osc_type[0] = Synth::WaveType::sine_wave;
                na_dec.layers[layer].osc_type[0] = Synth::WaveType::sine_wave;
                na_src.layers[layer].mod_ratio   = 1.0f;
                na_dec.layers[layer].mod_ratio   = 1.0f;
                Synth::TargetBinding& src_gen    = na_src.layers[layer].targets[Synth::mod_volume];
                Synth::TargetBinding& dec_gen    = na_dec.layers[layer].targets[Synth::mod_volume];
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

            Synth::InstrumentGraphLayout na_pair_records[2] = {};
            memset(static_cast<void*>(na_pair_records), 0, sizeof(na_pair_records));
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
            Synth::InstrumentGraphLayout na_pair_out[4]   = {};
            constexpr uint32_t           na_pair_capacity = sizeof(na_pair_out) / sizeof(na_pair_out[0]);
            uint32_t                     na_pair_count    = na_pair_capacity;
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
            int32_t record_index =
                find_portable(na_pair_out, na_pair_count, Synth::instrument_graph_layout_lfo, 0, Synth::mod_volume);
            TEST(record_index >= 0);
            if (record_index >= 0) {
                TEST(na_pair_out[record_index].x == 7.0f && na_pair_out[record_index].y == -3.5f);
                TEST(na_pair_out[record_index].depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
                TEST(na_pair_out[record_index].rate_source == static_cast<uint8_t>(Synth::ModSource::mod_wheel));
            }
            record_index =
                find_portable(na_pair_out, na_pair_count, Synth::instrument_graph_layout_lfo, 1, Synth::mod_volume);
            TEST(record_index >= 0);
            if (record_index >= 0) {
                TEST(na_pair_out[record_index].x == 8.0f && na_pair_out[record_index].y == -4.5f);
                TEST(na_pair_out[record_index].depth_source == static_cast<uint8_t>(Synth::ModSource::aftertouch));
                TEST(na_pair_out[record_index].rate_source == static_cast<uint8_t>(Synth::ModSource::pitch_bend));
            }
            Synth::InstrumentGraphLayout na_exact[2]       = {};
            constexpr uint32_t           na_exact_capacity = sizeof(na_exact) / sizeof(na_exact[0]);
            uint32_t                     na_exact_count    = na_exact_capacity;
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
            Synth::InstrumentGraphLayout na_refuse[1]       = {};
            Synth::InstrumentGraphLayout na_refuse_sentinel = {};
            memset(static_cast<void*>(na_refuse), 0x5A, sizeof(na_refuse));
            memset(static_cast<void*>(&na_refuse_sentinel), 0x5A, sizeof(na_refuse_sentinel));
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

            // Normalizer refusals: an unknown kind, a canonical index past the
            // projection, and a kind that is not clipboard data.  Each leaves
            // array and count untouched, so a refused normalization cannot
            // leak a partially translated record into the clipboard.  A
            // kind-1/2 locator the model cannot resolve is dropped instead
            // (the removed cutoff targets), pinned by the drop tests below.
            Synth::InstrumentGraphLayout nr_out[2]          = {};
            Synth::InstrumentGraphLayout nr_out_sentinel[2] = {};
            memset(static_cast<void*>(nr_out), 0x5A, sizeof(nr_out));
            memset(static_cast<void*>(nr_out_sentinel), 0x5A, sizeof(nr_out_sentinel));
            constexpr uint32_t nr_capacity = sizeof(nr_out) / sizeof(nr_out[0]);
            uint32_t           nr_count    = nr_capacity;
            for (uint32_t c = 0; c < 3; c++) {
                Synth::InstrumentGraphLayout nr_rec = {};
                if (c == 0) {
                    nr_rec.kind = 3; // unknown kinds are never clipboard data
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
            // normalization instead of truncating it: these two records normalize into
            // two slots when there is room for them.
            Synth::InstrumentGraphLayout nr_tight[1]          = {};
            Synth::InstrumentGraphLayout nr_tight_sentinel[1] = {};
            memset(static_cast<void*>(nr_tight), 0x5A, sizeof(nr_tight));
            memset(static_cast<void*>(nr_tight_sentinel), 0x5A, sizeof(nr_tight_sentinel));
            uint32_t nr_tight_count = 1;
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
                                                               nr_tight,
                                                               1,
                                                               &nr_tight_count));
            TEST(nr_tight_count == 1);
            TEST(memcmp(nr_tight, &nr_tight_sentinel, sizeof(nr_tight_sentinel)) == 0);
        }

        {
            // Every malformed portable record and every locator the model cannot
            // resolve refuses the whole document with all outputs intact. The
            // baseline document decodes, so each refusal is the record's doing.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ml_text)[clipboard_capacity]                        = ml_text_storage->elements;
            const std::unique_ptr<ClipboardDecode> ml_attempt_storage  = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ml_attempt          = *ml_attempt_storage;
            const std::unique_ptr<ClipboardDecode> ml_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ml_sentinel         = *ml_sentinel_storage;
            fill_clipboard_sentinels(&ml_sentinel);
            build_layout_doc(ml_text, sizeof(ml_text), clipboard_doc_head, clipboard_doc_records);
            const std::unique_ptr<ClipboardDecode> ml_baseline_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ml_baseline         = *ml_baseline_storage;
            fill_clipboard_sentinels(&ml_baseline);
            TEST(decode_clipboard(&ml_baseline, ml_text, max_portable_records));
            TEST(ml_baseline.layout_count == 3);

            struct LayoutRejection {
                const char* records;
                const char* dimension;
            };

            const LayoutRejection rejections[] = {
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
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1e400,\"y\":0,\"width\":0,\"height\":0}",
                  "overflowed number" },
                { "{\"kind\":0,\"canonical_index\":9,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "canonical index beyond the bound" },
                { "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":1,\"width\":0,\"height\":0},"
                  "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":2,\"y\":2,\"width\":0,\"height\":0}",
                  "duplicate portable locator" },
            };
            for (uint32_t i = 0; i < sizeof(rejections) / sizeof(rejections[0]); i++) {
                const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_bad_storage =
                    std::make_unique<FixedArray<char, clipboard_capacity>>();
                char (&ml_bad)[clipboard_capacity] = ml_bad_storage->elements;
                build_layout_doc(ml_bad, sizeof(ml_bad), clipboard_doc_head, rejections[i].records);
                fill_clipboard_sentinels(&ml_attempt);
                if (decode_clipboard(&ml_attempt, ml_bad, max_portable_records)) {
                    failed(rejections[i].dimension, __FILE__, __LINE__);
                }
                TEST(clipboard_sentinels_intact(&ml_attempt, &ml_sentinel));
            }
            // A kind-1/2 locator the model cannot resolve (layer, target, or mod
            // source past its bound) is dropped with a console note instead of
            // refusing the document: these are stale modulator positions
            // arriving from older banks and clips.
            const LayoutRejection drops[] = {
                { "{\"kind\":1,\"layer\":7,\"target\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "dropped layer out of range" },
                { "{\"kind\":1,\"layer\":0,\"target\":99,\"x\":0,\"y\":0,\"width\":0,\"height\":0}",
                  "dropped unknown target" },
                { "{\"kind\":2,\"layer\":0,\"target\":0,\"depth_source\":7,\"rate_source\":0,\"x\":0,\"y\":0,"
                  "\"width\":0,\"height\":0}",
                  "dropped source out of range" },
            };
            for (uint32_t i = 0; i < sizeof(drops) / sizeof(drops[0]); i++) {
                const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_bad_storage =
                    std::make_unique<FixedArray<char, clipboard_capacity>>();
                char (&ml_bad)[clipboard_capacity] = ml_bad_storage->elements;
                build_layout_doc(ml_bad, sizeof(ml_bad), clipboard_doc_head, drops[i].records);
                fill_clipboard_sentinels(&ml_attempt);
                TEST(decode_clipboard(&ml_attempt, ml_bad, max_portable_records));
                TEST(ml_attempt.layout_count == 1); // the raw record survives the drop
                TEST(ml_attempt.layout[0].x == 0.0f && ml_attempt.layout[0].y == 0.0f);
            }

            // An unregistered record kind is skipped by the generic decode path:
            // it neither refuses the document nor counts toward the portable
            // capacity.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_skip_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ml_skip)[clipboard_capacity]                            = ml_skip_storage->elements;
            const std::unique_ptr<ClipboardDecode> ml_skip_attempt_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ml_skip_attempt         = *ml_skip_attempt_storage;
            fill_clipboard_sentinels(&ml_skip_attempt);
            build_layout_doc(ml_skip,
                             sizeof(ml_skip),
                             clipboard_doc_head,
                             "{\"kind\":99,\"canonical_index\":0,\"x\":0,\"y\":0,\"width\":0,\"height\":0}");
            TEST(decode_clipboard(&ml_skip_attempt, ml_skip, max_portable_records));
            TEST(ml_skip_attempt.layout_count == 0);
            // Two distinct locators resolve to one interned descriptor: refused.
            // The one-record form of the same document decodes, so the refusal
            // is the second locator's.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_alias_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ml_alias)[clipboard_capacity] = ml_alias_storage->elements;
            build_layout_doc(ml_alias,
                             sizeof(ml_alias),
                             clipboard_alias_head,
                             "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":1,\"width\":0,\"height\":0}");
            const std::unique_ptr<ClipboardDecode> ml_alias_baseline_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ml_alias_baseline         = *ml_alias_baseline_storage;
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
                const std::unique_ptr<FixedArray<char, clipboard_capacity>> ml_over_storage =
                    std::make_unique<FixedArray<char, clipboard_capacity>>();
                char (&ml_over)[clipboard_capacity] = ml_over_storage->elements;
                uint32_t ml_over_pos                = 0;
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
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> cc_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&cc_doc)[clipboard_capacity] = cc_doc_storage->elements;
            const uint32_t cc_len = build_layout_doc(cc_doc, sizeof(cc_doc), clipboard_doc_head, clipboard_doc_records);
            TEST(cc_len > 0);

            // A valid nonempty layout is refused by the legacy decoder rather than
            // silently dropped, with every legacy output left at its sentinel.
            const std::unique_ptr<ClipboardDecode> cc_legacy_attempt_storage  = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       cc_legacy_attempt          = *cc_legacy_attempt_storage;
            const std::unique_ptr<ClipboardDecode> cc_legacy_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       cc_legacy_sentinel         = *cc_legacy_sentinel_storage;
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
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> cc_empty_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&cc_empty)[clipboard_capacity] = cc_empty_storage->elements;
            build_layout_doc(cc_empty, sizeof(cc_empty), clipboard_doc_head, "");
            const std::unique_ptr<ClipboardDecode> cc_empty_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       cc_empty_dec         = *cc_empty_dec_storage;
            fill_clipboard_sentinels(&cc_empty_dec);
            TEST(decode_clipboard(&cc_empty_dec, cc_empty, max_portable_records));
            TEST(cc_empty_dec.layout_count == 0);
            TEST(cc_empty_dec.env_count == 2);
            TEST(cc_empty_dec.lfo_count == 1);
            uint32_t                  cc_null_env                                   = 999;
            uint32_t                  cc_null_lfo                                   = 999;
            uint32_t                  cc_null_rows                                  = 999;
            Synth::Instrument         cc_null_instr                                 = {};
            Synth::EnvelopeDescriptor cc_null_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      cc_null_lfos[Synth::instrument_max_lfos]      = {};
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
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> cc_bare_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&cc_bare_doc)[clipboard_capacity] = cc_bare_doc_storage->elements;
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
            Synth::InstrumentGraphLayout cc_out[max_portable_records]                 = {};
            Synth::Instrument            cc_out_instr                                 = {};
            Synth::EnvelopeDescriptor    cc_out_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor         cc_out_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                     cc_out_env                                   = 999;
            uint32_t                     cc_out_lfo                                   = 999;
            uint32_t                     cc_out_rows                                  = 999;
            TEST(Synth::decode_instrument_json(cc_doc,
                                               cc_len,
                                               &cc_out_instr,
                                               cc_out_envs,
                                               &cc_out_env,
                                               cc_out_lfos,
                                               &cc_out_lfo,
                                               cc_out,
                                               3,
                                               &cc_out_rows));
            TEST(cc_out_rows == 3);
            TEST(cc_out_env == 2);
            TEST(cc_out_lfo == 1);

            const std::unique_ptr<ClipboardDecode> cc_short_storage          = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       cc_short                  = *cc_short_storage;
            const std::unique_ptr<ClipboardDecode> cc_short_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       cc_short_sentinel         = *cc_short_sentinel_storage;
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
                                                 2,
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
            const std::unique_ptr<Synth::InstrumentBank> mf_bank_storage = std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank&                       mf_bank         = *mf_bank_storage;
            Synth::Instrument                            mf_instr        = {};
            Synth::InstrumentGraphLayout                 mf_records[max_portable_records] = {};
            Synth::GraphNodeLayout                       mf_mapped[max_portable_records]  = {};
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> mf_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&mf_text)[clipboard_capacity] = mf_text_storage->elements;
            const uint32_t mf_count             = build_clipboard_max_fixture(&mf_bank, &mf_instr, mf_records);
            const uint32_t mf_slot              = mf_bank.instruments.allocate();
            TEST(mf_slot != pool_no_slot);
            mf_bank.instruments.entries[mf_slot]   = mf_instr;
            mf_bank.channel_enabled[0]             = 1;
            mf_bank.channel_zones[0][0].start_note = 1;
            mf_bank.channel_zones[0][0].instrument = static_cast<uint8_t>(mf_slot);
            TEST(Synth::validate_instrument_bank(&mf_bank));
            TEST(mf_bank.envelopes.num_allocated == Synth::max_layers * Synth::num_mod_targets);
            TEST(mf_bank.lfos.num_allocated == Synth::max_layers * Synth::num_mod_targets);
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
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> mf_short_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&mf_short_text)[clipboard_capacity] = mf_short_text_storage->elements;
            TEST(Synth::encode_instrument_json(mf_short_text, mf_len, &mf_instr, &mf_bank, mf_records, mf_count) == 0);
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> mf_exact_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&mf_exact_text)[clipboard_capacity] = mf_exact_text_storage->elements;
            TEST(Synth::encode_instrument_json(mf_exact_text, mf_len + 1, &mf_instr, &mf_bank, mf_records, mf_count) ==
                 mf_len);
            TEST(mf_tokens > 0);
            TEST(static_cast<uint32_t>(mf_tokens) <= clipboard_token_bound);

            const std::unique_ptr<ClipboardDecode> mf_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       mf_dec         = *mf_dec_storage;
            fill_clipboard_sentinels(&mf_dec);
            TEST(decode_clipboard(&mf_dec, mf_text, max_portable_records));
            TEST(mf_dec.layout_count == max_portable_records);
            TEST(mf_dec.env_count == Synth::max_layers * Synth::num_mod_targets);
            TEST(mf_dec.lfo_count == Synth::max_layers * Synth::num_mod_targets);
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
            const std::unique_ptr<ClipboardDecode> mf_short_storage    = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       mf_short            = *mf_short_storage;
            const std::unique_ptr<ClipboardDecode> mf_sentinel_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       mf_sentinel         = *mf_sentinel_storage;
            fill_clipboard_sentinels(&mf_short);
            fill_clipboard_sentinels(&mf_sentinel);
            TEST(decode_clipboard(&mf_short, mf_text, max_portable_records - 1) == false);
            TEST(clipboard_sentinels_intact(&mf_short, &mf_sentinel));

            // The same instrument through the legacy entry point is smaller by the
            // record array alone and carries an empty layout: the layout array is the
            // only addition to the document.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> mf_legacy_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&mf_legacy)[clipboard_capacity] = mf_legacy_storage->elements;
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
            const char* const nw_head = "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}],";
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> nw_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&nw_text)[clipboard_capacity]                   = nw_text_storage->elements;
            const std::unique_ptr<ClipboardDecode> nw_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       nw_dec         = *nw_dec_storage;
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
            Synth::GraphNodeLayout nw_mapped[8]    = {};
            uint32_t               nw_mapped_count = 8;
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
            };
            for (uint32_t i = 0; i < sizeof(surplus) / sizeof(surplus[0]); i++) {
                const std::unique_ptr<FixedArray<char, clipboard_capacity>> nw_bad_storage =
                    std::make_unique<FixedArray<char, clipboard_capacity>>();
                char (&nw_bad)[clipboard_capacity] = nw_bad_storage->elements;
                build_layout_doc(nw_bad, sizeof(nw_bad), nw_head, surplus[i]);
                const std::unique_ptr<ClipboardDecode> nw_attempt_storage  = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       nw_attempt          = *nw_attempt_storage;
                const std::unique_ptr<ClipboardDecode> nw_sentinel_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       nw_sentinel         = *nw_sentinel_storage;
                fill_clipboard_sentinels(&nw_sentinel);
                fill_clipboard_sentinels(&nw_attempt);
                TEST(decode_clipboard(&nw_attempt, nw_bad, max_portable_records) == false);
                TEST(clipboard_sentinels_intact(&nw_attempt, &nw_sentinel));
            }
            // A kind-1 locator the decoded model cannot resolve is dropped by the
            // mapping/normalization entry points, not by the decode: the record is
            // retained raw here and skipped (with a console note) later.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> nw_drop_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&nw_drop)[clipboard_capacity]                            = nw_drop_storage->elements;
            const std::unique_ptr<ClipboardDecode> nw_drop_attempt_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       nw_drop_attempt         = *nw_drop_attempt_storage;
            build_layout_doc(nw_drop,
                             sizeof(nw_drop),
                             nw_head,
                             "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":1,\"y\":2,\"width\":0,\"height\":0}");
            fill_clipboard_sentinels(&nw_drop_attempt);
            TEST(decode_clipboard(&nw_drop_attempt, nw_drop, max_portable_records));
            TEST(nw_drop_attempt.layout_count == 1);
        }
        // Carrier 2: an old portable document (library-transfer shape)
        // carries removed cutoff bindings and cutoff-target layout records
        // (kind 1 target 7, kind 2 target 8).  The decode succeeds, the
        // cutoff records are dropped by the mapping/normalization entry
        // points (logged, never reinterpreted), and the recognized volume
        // record survives with its position and uid.
        {
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> old_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&old_doc)[clipboard_capacity] = old_doc_storage->elements;
            const int old_written =
                snprintf(old_doc,
                         sizeof(old_doc),
                         "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
                         "\"layers\":[{\"wave_a\":\"sine\",\"volume\":{\"base\":0.8,"
                         "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.5},"
                         "{\"time_seconds\":0.5,\"level\":0.25}]}},"
                         "\"lowpass_cutoff\":{\"base\":2000},\"highpass_cutoff\":{\"base\":500}}],"
                         "\"graph_layout\":["
                         "{\"kind\":1,\"layer\":0,\"target\":7,\"x\":10,\"y\":11,\"width\":0,\"height\":0},"
                         "{\"kind\":2,\"layer\":0,\"target\":8,\"depth_source\":%u,\"rate_source\":0,"
                         "\"x\":12,\"y\":13,\"width\":0,\"height\":0},"
                         "{\"kind\":1,\"layer\":0,\"target\":0,\"x\":14,\"y\":15,\"width\":0,\"height\":0}]}",
                         static_cast<unsigned>(Synth::ModSource::velocity));
            TEST(old_written > 0 && static_cast<uint32_t>(old_written) < sizeof(old_doc));
            Synth::Instrument            old_instr                                 = {};
            Synth::EnvelopeDescriptor    old_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor         old_lfos[Synth::instrument_max_lfos]      = {};
            Synth::InstrumentGraphLayout old_layout[max_portable_records]          = {};
            uint32_t                     old_env_count                             = 999;
            uint32_t                     old_lfo_count                             = 999;
            uint32_t                     old_layout_count                          = 999;
            TEST(Synth::decode_instrument_json_for_domain(old_doc,
                                                          static_cast<uint32_t>(old_written),
                                                          &old_instr,
                                                          old_envs,
                                                          &old_env_count,
                                                          old_lfos,
                                                          &old_lfo_count,
                                                          old_layout,
                                                          max_portable_records,
                                                          &old_layout_count,
                                                          Synth::InstrumentJsonDomain::validated_bank));
            TEST(old_layout_count == 3); // the raw records survive the decode
            Synth::GraphNodeLayout old_mapped[4]    = {};
            uint32_t               old_mapped_count = 4;
            TEST(Sculptor::map_instrument_graph_layout(old_instr,
                                                       old_envs,
                                                       old_env_count,
                                                       old_lfos,
                                                       old_lfo_count,
                                                       old_layout,
                                                       old_layout_count,
                                                       old_mapped,
                                                       4,
                                                       &old_mapped_count));
            TEST(old_mapped_count == 1); // exactly the recognized record
            TEST(old_mapped[0].kind == 1);
            TEST(old_mapped[0].x == 14.0f && old_mapped[0].y == 15.0f);
            TEST(old_mapped[0].uid != 0);
            Synth::InstrumentGraphLayout old_normalized[4]    = {};
            uint32_t                     old_normalized_count = 4;
            TEST(Sculptor::normalize_instrument_graph_layout(old_instr,
                                                             old_envs,
                                                             old_env_count,
                                                             old_lfos,
                                                             old_lfo_count,
                                                             old_layout,
                                                             old_layout_count,
                                                             old_instr,
                                                             old_envs,
                                                             old_env_count,
                                                             old_lfos,
                                                             old_lfo_count,
                                                             old_normalized,
                                                             4,
                                                             &old_normalized_count));
            TEST(old_normalized_count == 1);
            TEST(old_normalized[0].x == 14.0f && old_normalized[0].y == 15.0f);
            // Control: a spread envelope makes target 7 live again, and the
            // same record now resolves onto the spread node.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> new_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&new_doc)[clipboard_capacity] = new_doc_storage->elements;
            const int new_written =
                snprintf(new_doc,
                         sizeof(new_doc),
                         "{\"format\":\"synth-instrument-v1\",\"note_skew_semitones\":0,\"layer_skew_semitones\":0,"
                         "\"layers\":[{\"wave_a\":\"sine\",\"fir_pitch_spread\":{\"base\":3,"
                         "\"envelope\":{\"points\":[{\"time_seconds\":0,\"level\":0.5},"
                         "{\"time_seconds\":0.5,\"level\":0.25}]}}}],"
                         "\"graph_layout\":["
                         "{\"kind\":1,\"layer\":0,\"target\":7,\"x\":21,\"y\":22,\"width\":0,\"height\":0}]}");
            TEST(new_written > 0 && static_cast<uint32_t>(new_written) < sizeof(new_doc));
            Synth::Instrument            new_instr                                 = {};
            Synth::EnvelopeDescriptor    new_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor         new_lfos[Synth::instrument_max_lfos]      = {};
            Synth::InstrumentGraphLayout new_layout[max_portable_records]          = {};
            uint32_t                     new_env_count                             = 999;
            uint32_t                     new_lfo_count                             = 999;
            uint32_t                     new_layout_count                          = 999;
            TEST(Synth::decode_instrument_json_for_domain(new_doc,
                                                          static_cast<uint32_t>(new_written),
                                                          &new_instr,
                                                          new_envs,
                                                          &new_env_count,
                                                          new_lfos,
                                                          &new_lfo_count,
                                                          new_layout,
                                                          max_portable_records,
                                                          &new_layout_count,
                                                          Synth::InstrumentJsonDomain::validated_bank));
            TEST(new_env_count == 1 && new_layout_count == 1);
            Synth::GraphNodeLayout new_mapped[4]    = {};
            uint32_t               new_mapped_count = 4;
            TEST(Sculptor::map_instrument_graph_layout(new_instr,
                                                       new_envs,
                                                       new_env_count,
                                                       new_lfos,
                                                       new_lfo_count,
                                                       new_layout,
                                                       new_layout_count,
                                                       new_mapped,
                                                       4,
                                                       &new_mapped_count));
            TEST(new_mapped_count == 1);
            TEST(new_mapped[0].kind == 1);
            TEST(new_mapped[0].x == 21.0f && new_mapped[0].y == 22.0f);
            TEST(new_mapped[0].uid != 0);
            // Direct-struct controls (JSON bypassed): an unknown kind stays
            // fatal through the direct API; a stale record name is ignored
            // and never copied into the mapping.
            Synth::GraphNodeLayout       control_out[4]     = {};
            uint32_t                     control_count      = 4;
            Synth::InstrumentGraphLayout control_records[1] = {};
            memset(static_cast<void*>(control_records), 0, sizeof(control_records));
            control_records[0].kind   = 3;
            control_records[0].layer  = 0;
            control_records[0].target = 0;
            control_records[0].x      = 1.0f;
            control_records[0].y      = 2.0f;
            TEST(! Sculptor::map_instrument_graph_layout(new_instr,
                                                         new_envs,
                                                         new_env_count,
                                                         new_lfos,
                                                         new_lfo_count,
                                                         control_records,
                                                         1,
                                                         control_out,
                                                         4,
                                                         &control_count));
            TEST(control_count == 4);

            control_records[0].kind   = Synth::instrument_graph_layout_envelope;
            control_records[0].layer  = new_layout[0].layer;
            control_records[0].target = new_layout[0].target;
            strcpy(control_records[0].name, "Stale");
            TEST(Sculptor::map_instrument_graph_layout(new_instr,
                                                       new_envs,
                                                       new_env_count,
                                                       new_lfos,
                                                       new_lfo_count,
                                                       control_records,
                                                       1,
                                                       control_out,
                                                       4,
                                                       &control_count));
            TEST(control_count == 1);
            TEST(control_out[0].name[0] == 0);

            TEST(Sculptor::normalize_instrument_graph_layout(new_instr,
                                                             new_envs,
                                                             new_env_count,
                                                             new_lfos,
                                                             new_lfo_count,
                                                             control_records,
                                                             1,
                                                             new_instr,
                                                             new_envs,
                                                             new_env_count,
                                                             new_lfos,
                                                             new_lfo_count,
                                                             old_normalized,
                                                             4,
                                                             &old_normalized_count));
            TEST(old_normalized_count == 1);
            TEST(old_normalized[0].name[0] == 0);
        }

        {
            // Candidate replacement replaces only the destination zone's layout and
            // mask row: every other root, name, record, effect layout and channel
            // effect title survives, and surviving effect LFO titles follow the
            // actual descriptor renumbering.
            const std::unique_ptr<Synth::InstrumentEditorBank> rp_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& rp_bank  = *rp_bank_storage;
            Synth::Instrument            rp_instr = {};
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
            rp_bank.graph_osc_source[0][0][0]            = 1;
            rp_bank.graph_osc_source[0][1][0]            = 1;
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

            const std::unique_ptr<FixedArray<char, clipboard_capacity>> rp_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&rp_doc)[clipboard_capacity]                    = rp_doc_storage->elements;
            const std::unique_ptr<ClipboardDecode> rp_dec_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       rp_dec         = *rp_dec_storage;
            build_layout_doc(rp_doc, sizeof(rp_doc), clipboard_doc_head, clipboard_doc_records);
            fill_clipboard_sentinels(&rp_dec);
            TEST(decode_clipboard(&rp_dec, rp_doc, max_portable_records));

            const std::unique_ptr<Synth::InstrumentEditorBank> rp_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& rp_candidate = *rp_candidate_storage;
            memset(static_cast<void*>(&rp_candidate), 0x5A, sizeof(rp_candidate));
            const std::unique_ptr<Synth::InstrumentEditorBank> rp_source_before_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& rp_source_before = *rp_source_before_storage;
            rp_source_before                              = rp_bank;
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
                                                                                 nullptr,
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
                TEST(rp_candidate.graph_osc_source[0][0][0] == 1);
                TEST(rp_candidate.graph_osc_source[0][1][0] == 1);

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
                TEST(rp_candidate.bank.instruments.entries[rp_dest_slot]
                         .layers[0]
                         .targets[Synth::mod_volume]
                         .lfo_desc_id == 3);
                TEST(rp_candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == 2);
            }
        }

        {
            // An editor bank whose own metadata is invalid is refused before any
            // candidate is written: the paste must not launder a broken source.
            const std::unique_ptr<Synth::InstrumentEditorBank> md_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& md_bank  = *md_bank_storage;
            Synth::Instrument            md_instr = {};
            build_clipboard_zone_fixture(&md_bank, &md_instr);
            add_detached_record(&md_bank, 0, 0, 4, 0, 1.0f, 2.0f, Synth::ModSource::none, Synth::ModSource::none, 0);
            // An effect record without a node title cannot key its node.
            TEST(md_bank.graph_layout[md_bank.graph_layout_count - 1].name[0] == 0);
            TEST(! Sculptor::validate_editor_metadata(md_bank));

            Synth::Instrument md_bare = {};
            md_bare                   = {};
            md_bare.layer_count       = 1;
            set_default_dispersion_sections(md_bare);
            md_bare.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
            md_bare.layers[0].osc_type[1] = Synth::WaveType::sine_wave;
            md_bare.layers[0].mod_ratio   = 1.0f;

            const std::unique_ptr<Synth::InstrumentEditorBank> md_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&                       md_candidate = *md_candidate_storage;
            const std::unique_ptr<Synth::InstrumentEditorBank> md_sentinel_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& md_sentinel = *md_sentinel_storage;
            memset(static_cast<void*>(&md_candidate), 0x5A, sizeof(md_candidate));
            memset(static_cast<void*>(&md_sentinel), 0x5A, sizeof(md_sentinel));
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
                                                               nullptr,
                                                               &md_candidate));
            TEST(memcmp(&md_candidate, &md_sentinel, sizeof(md_sentinel)) == 0);
        }

        {
            // Placeholder selection and refusal sequencing.
            //
            // A descriptorless instrument pasted over the sole zone of a channel is
            // valid with no descriptor at all, and the uniquely rooted destination
            // slot is reused in place.
            Synth::Instrument pl_bare = {};
            pl_bare                   = {};
            pl_bare.layer_count       = 1;
            set_default_dispersion_sections(pl_bare);
            pl_bare.layers[0].osc_type[0]                            = Synth::WaveType::sine_wave;
            pl_bare.layers[0].osc_type[1]                            = Synth::WaveType::sine_wave;
            pl_bare.layers[0].osc_mode                               = Synth::osc_mode_blend;
            pl_bare.layers[0].mod_ratio                              = 1.0f;
            pl_bare.layers[0].targets[Synth::mod_panning].base_value = 0.5f;

            const std::unique_ptr<Synth::InstrumentEditorBank> pl_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& pl_bank  = *pl_bank_storage;
            Synth::Instrument            pl_instr = {};
            build_clipboard_zone_fixture(&pl_bank, &pl_instr);
            TEST(Synth::validate_instrument_bank(&pl_bank.bank));
            TEST(Sculptor::validate_editor_metadata(pl_bank));

            const std::unique_ptr<Synth::InstrumentEditorBank> pl_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& pl_candidate = *pl_candidate_storage;
            memset(static_cast<void*>(&pl_candidate), 0x5A, sizeof(pl_candidate));
            const std::unique_ptr<Synth::InstrumentEditorBank> pl_before_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& pl_before = *pl_before_storage;
            pl_before                              = pl_bank;
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
                                                             nullptr,
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
            const std::unique_ptr<Synth::InstrumentEditorBank> pl_sentinel_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& pl_sentinel = *pl_sentinel_storage;
            memset(static_cast<void*>(&pl_sentinel), 0x5A, sizeof(pl_sentinel));
            memset(static_cast<void*>(&pl_candidate), 0x5A, sizeof(pl_candidate));
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
                                                               nullptr,
                                                               &pl_candidate));
            TEST(memcmp(&pl_candidate, &pl_sentinel, sizeof(pl_sentinel)) == 0);
            memset(static_cast<void*>(&pl_candidate), 0x5A, sizeof(pl_candidate));
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
                                                               nullptr,
                                                               &pl_candidate));
            TEST(memcmp(&pl_candidate, &pl_sentinel, sizeof(pl_sentinel)) == 0);
        }

        {
            // A fully allocated instrument pool cannot hand out a placeholder, so a
            // uniquely rooted destination succeeds by reusing its own slot while a
            // shared destination must refuse: the pool is full before reclaim, and an
            // allocated-but-unrooted slot is not free.
            const std::unique_ptr<Synth::InstrumentEditorBank> fp_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& fp_bank = *fp_bank_storage;
            memset(static_cast<void*>(&fp_bank), 0, sizeof(fp_bank));
            for (uint32_t i = 0; i < Synth::max_instruments; i++) {
                TEST(fp_bank.bank.instruments.allocate() == i);
                fp_bank.bank.instruments.entries[i].layer_count = 1;
                set_default_dispersion_sections(fp_bank.bank.instruments.entries[i]);
                fp_bank.bank.instruments.entries[i].layers[0].osc_type[0] = Synth::WaveType::sine_wave;
                fp_bank.bank.instruments.entries[i].layers[0].mod_ratio   = 1.0f;
            }
            fp_bank.bank.channel_enabled[0]             = 1;
            fp_bank.bank.channel_zones[0][0].start_note = 1;
            fp_bank.bank.channel_zones[0][0].instrument = 7;
            TEST(Synth::validate_instrument_bank(&fp_bank.bank));

            Synth::Instrument fp_bare = {};
            fp_bare                   = {};
            fp_bare.layer_count       = 1;
            set_default_dispersion_sections(fp_bare);
            fp_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
            fp_bare.layers[0].osc_type[1] = Synth::WaveType::noise_wave;
            fp_bare.layers[0].mod_ratio   = 1.0f;

            const std::unique_ptr<Synth::InstrumentEditorBank> fp_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& fp_candidate = *fp_candidate_storage;
            memset(static_cast<void*>(&fp_candidate), 0x5A, sizeof(fp_candidate));
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
                                                             nullptr,
                                                             &fp_candidate));
            TEST(Synth::validate_instrument_bank(&fp_candidate.bank));
            TEST(fp_candidate.bank.instruments.num_allocated >= 1);
            TEST(fp_candidate.bank.instruments.entries[fp_candidate.bank.channel_zones[0][0].instrument]
                     .layers[0]
                     .osc_type[0] == Synth::WaveType::noise_wave);

            // Same full pool, shared destination: no free or unallocated slot exists
            // before reclaim, so the transaction refuses even though reclaim would
            // later free unrooted slots.
            const std::unique_ptr<Synth::InstrumentEditorBank> fs_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& fs_bank        = *fs_bank_storage;
            fs_bank                                     = fp_bank;
            fs_bank.bank.channel_zones[0][1].start_note = 65;
            fs_bank.bank.channel_zones[0][1].instrument = 7;
            TEST(Synth::validate_instrument_bank(&fs_bank.bank));

            const std::unique_ptr<Synth::InstrumentEditorBank> fs_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&                       fs_candidate = *fs_candidate_storage;
            const std::unique_ptr<Synth::InstrumentEditorBank> fs_sentinel_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& fs_sentinel = *fs_sentinel_storage;
            memset(static_cast<void*>(&fs_candidate), 0x5A, sizeof(fs_candidate));
            memset(static_cast<void*>(&fs_sentinel), 0x5A, sizeof(fs_sentinel));
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
                                                               nullptr,
                                                               &fs_candidate));
            TEST(memcmp(&fs_candidate, &fs_sentinel, sizeof(fs_sentinel)) == 0);
        }

        {
            // A shared destination with a free slot gets its own placeholder, so the
            // other zone keeps playing the instrument it always played.
            const std::unique_ptr<Synth::InstrumentEditorBank> sh_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& sh_bank = *sh_bank_storage;
            memset(static_cast<void*>(&sh_bank), 0, sizeof(sh_bank));
            TEST(sh_bank.bank.instruments.allocate() == 0);
            TEST(sh_bank.bank.instruments.allocate() == 1);
            sh_bank.bank.instruments.entries[0].layer_count = 1;
            set_default_dispersion_sections(sh_bank.bank.instruments.entries[0]);
            sh_bank.bank.instruments.entries[0].layers[0].osc_type[0] = Synth::WaveType::sine_wave;
            sh_bank.bank.instruments.entries[0].layers[0].mod_ratio   = 1.0f;
            sh_bank.bank.instruments.entries[1].layer_count           = 1;
            set_default_dispersion_sections(sh_bank.bank.instruments.entries[1]);
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

            Synth::Instrument sh_bare = {};
            sh_bare                   = {};
            sh_bare.layer_count       = 1;
            set_default_dispersion_sections(sh_bare);
            sh_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
            sh_bare.layers[0].osc_type[1] = Synth::WaveType::noise_wave;
            sh_bare.layers[0].mod_ratio   = 1.0f;

            const std::unique_ptr<Synth::InstrumentEditorBank> sh_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& sh_candidate = *sh_candidate_storage;
            memset(static_cast<void*>(&sh_candidate), 0x5A, sizeof(sh_candidate));
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
                                                             nullptr,
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
                const std::unique_ptr<Synth::InstrumentEditorBank> vr_bank_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& vr_bank  = *vr_bank_storage;
                Synth::Instrument            vr_instr = {};
                build_clipboard_zone_fixture(&vr_bank, &vr_instr);

                Synth::Instrument vr_bare = {};
                vr_bare                   = {};
                vr_bare.layer_count       = 1;
                set_default_dispersion_sections(vr_bare);
                vr_bare.layers[0].osc_type[0] = Synth::WaveType::noise_wave;
                vr_bare.layers[0].mod_ratio   = 1.0f;
                TEST(validate_standalone_model(vr_bare, nullptr, 0, nullptr, 0));

                const std::unique_ptr<Synth::InstrumentEditorBank> vr_ok_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& vr_ok = *vr_ok_storage;
                memset(static_cast<void*>(&vr_ok), 0x5A, sizeof(vr_ok));
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
                                                                 nullptr,
                                                                 &vr_ok));

                const std::unique_ptr<Synth::InstrumentEditorBank> vr_candidate_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank&                       vr_candidate = *vr_candidate_storage;
                const std::unique_ptr<Synth::InstrumentEditorBank> vr_sentinel_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& vr_sentinel = *vr_sentinel_storage;
                memset(static_cast<void*>(&vr_candidate), 0x5A, sizeof(vr_candidate));
                memset(static_cast<void*>(&vr_sentinel), 0x5A, sizeof(vr_sentinel));
                const std::unique_ptr<Synth::InstrumentEditorBank> vr_before_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& vr_before = *vr_before_storage;
                vr_before                              = vr_bank;

                // Envelope id past the descriptor pool, with no descriptors installed.
                Synth::Instrument vr_bad_env = {};
                vr_bad_env                   = vr_bare;
                vr_bad_env.layers[0].targets[Synth::mod_volume].envelope_desc_id =
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
                                                                   nullptr,
                                                                   &vr_candidate));
                TEST(memcmp(&vr_candidate, &vr_sentinel, sizeof(vr_sentinel)) == 0);
                TEST(memcmp(&vr_bank, &vr_before, sizeof(vr_before)) == 0);

                // LFO id past the descriptors that come with the instrument.
                Synth::LFODescriptor vr_lfo                                 = {};
                vr_lfo                                                      = {};
                vr_lfo.wave                                                 = Synth::WaveType::sine_wave;
                vr_lfo.period_ms                                            = 500;
                vr_lfo.min_value                                            = 0.0f;
                vr_lfo.min_max_delta                                        = 1.0f;
                Synth::Instrument vr_bad_lfo                                = {};
                vr_bad_lfo                                                  = vr_bare;
                vr_bad_lfo.layers[0].targets[Synth::mod_volume].lfo_desc_id = 2;
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
                                                                   nullptr,
                                                                   &vr_candidate));
                TEST(memcmp(&vr_candidate, &vr_sentinel, sizeof(vr_sentinel)) == 0);
                TEST(memcmp(&vr_bank, &vr_before, sizeof(vr_before)) == 0);

                // A layout record the decoded instrument cannot resolve at all (kind 0
                // past the projection) still refuses the candidate: only kinds 1/2 are
                // droppable removed-target debris.
                Synth::InstrumentGraphLayout vr_records[1] = {};
                memset(static_cast<void*>(vr_records), 0, sizeof(vr_records));
                vr_records[0].kind            = Synth::instrument_graph_layout_canonical;
                vr_records[0].canonical_index = 9;
                vr_records[0].x               = 3.0f;
                vr_records[0].y               = 4.0f;
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
                                                                   nullptr,
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
            const std::unique_ptr<Synth::InstrumentEditorBank> dc_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& dc_bank = *dc_bank_storage;
            memset(static_cast<void*>(&dc_bank), 0, sizeof(dc_bank));
            TEST(dc_bank.bank.instruments.allocate() == 0);
            TEST(dc_bank.bank.instruments.allocate() == 1);
            dc_bank.bank.instruments.entries[0].layer_count = 1;
            set_default_dispersion_sections(dc_bank.bank.instruments.entries[0]);
            dc_bank.bank.instruments.entries[0].layers[0].osc_type[0] = Synth::WaveType::sawtooth_wave;
            dc_bank.bank.instruments.entries[0].layers[0].mod_ratio   = 3.0f;
            dc_bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value = 0.25f;
            dc_bank.bank.instruments.entries[1].layer_count                                     = 1;
            set_default_dispersion_sections(dc_bank.bank.instruments.entries[1]);
            dc_bank.bank.instruments.entries[1].layers[0].osc_type[0] = Synth::WaveType::pulse_wave;
            dc_bank.bank.instruments.entries[1].layers[0].mod_ratio   = 1.0f;
            dc_bank.bank.channel_zones[0][0].start_note               = 1;
            dc_bank.bank.channel_zones[0][0].instrument               = 0;
            dc_bank.bank.channel_zones[0][1].start_note               = 65;
            dc_bank.bank.channel_zones[0][1].instrument               = 0; // shared with the destination
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
            dc_bank.graph_osc_source[0][1][0]            = 1;
            const Synth::GraphNodeLayout dc_other_record = dc_bank.graph_layout[dc_bank.graph_layout_count - 1];
            // Validate the complete privately enabled state before disabling it.
            const std::unique_ptr<Synth::InstrumentEditorBank> dc_enabled_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& dc_enabled = *dc_enabled_storage;
            dc_enabled                              = dc_bank;
            dc_enabled.bank.channel_enabled[0]      = 1;
            TEST(Synth::validate_instrument_bank(&dc_enabled.bank));
            TEST(Sculptor::validate_editor_metadata(dc_enabled));
            dc_bank.bank.channel_enabled[0] = 0;
            TEST(Synth::validate_instrument_bank(&dc_bank.bank));
            TEST(Sculptor::validate_editor_metadata(dc_bank));
            const Synth::Instrument dc_old           = dc_bank.bank.instruments.entries[0];
            const uint32_t          dc_other_records = Sculptor::count_detached_records(dc_bank, 0, 1);
            const uint32_t          dc_record_count  = dc_bank.graph_layout_count;

            Synth::Instrument dc_bare = {};
            dc_bare                   = {};
            dc_bare.layer_count       = 1;
            set_default_dispersion_sections(dc_bare);
            dc_bare.layers[0].osc_type[0]                           = Synth::WaveType::noise_wave;
            dc_bare.layers[0].osc_type[1]                           = Synth::WaveType::noise_wave;
            dc_bare.layers[0].mod_ratio                             = 1.0f;
            dc_bare.layers[0].targets[Synth::mod_volume].base_value = 0.75f;

            const std::unique_ptr<Synth::InstrumentEditorBank> dc_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& dc_candidate = *dc_candidate_storage;
            memset(static_cast<void*>(&dc_candidate), 0x5A, sizeof(dc_candidate));
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
                                                                                 nullptr,
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
                TEST(dc_candidate.graph_osc_source[0][1][0] == 1);
            }

            // A destination table that is only invalid once the channel is enabled
            // (a zone referencing an instrument the pool does not hold) refuses the
            // whole transaction.
            const std::unique_ptr<Synth::InstrumentEditorBank> dt_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& dt_bank        = *dt_bank_storage;
            dt_bank                                     = dc_bank;
            dt_bank.bank.channel_zones[0][1].instrument = 200;
            TEST(Synth::validate_instrument_bank(&dt_bank.bank)); // disabled: the table is skipped

            const std::unique_ptr<Synth::InstrumentEditorBank> dt_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank&                       dt_candidate = *dt_candidate_storage;
            const std::unique_ptr<Synth::InstrumentEditorBank> dt_sentinel_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& dt_sentinel = *dt_sentinel_storage;
            memset(static_cast<void*>(&dt_candidate), 0x5A, sizeof(dt_candidate));
            memset(static_cast<void*>(&dt_sentinel), 0x5A, sizeof(dt_sentinel));
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
                                                               nullptr,
                                                               &dt_candidate));
            TEST(memcmp(&dt_candidate, &dt_sentinel, sizeof(dt_sentinel)) == 0);
        }

        {
            // This exercises measurement-pool overflow, not production decoder
            // rejection order: a well-formed document exceeds the measurement bound.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ov_doc_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ov_doc)[clipboard_capacity] = ov_doc_storage->elements;
            uint32_t ov_pos                    = 0;
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
            char ov_small_doc[] = "{\"a\":[0,1,2]}";
            TEST(count_clipboard_tokens(ov_small_doc, static_cast<uint32_t>(strlen(ov_small_doc))) > 0);
        }

        {

            // Ordering of the GUI paste command against the candidate helper.
        }

        // The default instrument round-trips too.
        const std::unique_ptr<Synth::InstrumentBank> default_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       default_bank         = *default_bank_storage;
        memset(static_cast<void*>(&default_bank), 0, sizeof(default_bank));
        TEST(Synth::init_default_channel(&default_bank, 0));
        const uint32_t default_encoded_len = Synth::encode_instrument_json(encoded,
                                                                           sizeof(encoded),
                                                                           &default_bank.instruments.entries[0],
                                                                           &default_bank);
        TEST(default_encoded_len > 0);
        // New clipboard output includes graph_layout even when no node moved.
        TEST(strstr(encoded, "\"graph_layout\":[]") != nullptr);
        Synth::Instrument         default_decoded                               = {};
        Synth::EnvelopeDescriptor default_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      default_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  default_env_count                             = 999;
        uint32_t                  default_lfo_count                             = 999;
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
        Synth::Instrument         empty_layout_instr                                 = {};
        Synth::EnvelopeDescriptor empty_layout_envs[Synth::instrument_max_envelopes] = {};
        Synth::LFODescriptor      empty_layout_lfos[Synth::instrument_max_lfos]      = {};
        uint32_t                  empty_layout_env_count                             = 999;
        uint32_t                  empty_layout_lfo_count                             = 999;
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
            const std::unique_ptr<FixedArray<char, 64 * 1024>> example_storage =
                std::make_unique<FixedArray<char, 64 * 1024>>();
            char (&example)[64 * 1024] = example_storage->elements;
            FILE* const example_file   = fopen("doc/synth_instrument_example.json", "rb");
            TEST(example_file != nullptr);
            const size_t got = example_file ? fread(example, 1, sizeof(example) - 1, example_file) : 0;
            if (example_file) {
                fclose(example_file);
            }
            TEST(got > 0 && got < sizeof(example) - 1);
            Synth::Instrument         example_instr                                 = {};
            Synth::EnvelopeDescriptor example_envs[Synth::instrument_max_envelopes] = {};
            Synth::LFODescriptor      example_lfos[Synth::instrument_max_lfos]      = {};
            uint32_t                  example_env_count                             = 0;
            uint32_t                  example_lfo_count                             = 0;
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

    // ---------------------------------------------------------------------
    // Tooltips: every projected slot carries its descriptor text and the
    // exempt rows stay null.
    // ---------------------------------------------------------------------
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        Synth::init_default_bank(&bank.bank);

        Synth::Instrument& instrument = bank.bank.instruments.entries[0];
        instrument.layer_count        = 4;
        set_default_dispersion_sections(instrument);
        for (uint32_t layer = 1; layer < 4; ++layer) {
            instrument.layers[layer].osc_type[0]                            = Synth::WaveType::sine_wave;
            instrument.layers[layer].osc_type[1]                            = Synth::WaveType::no_wave;
            instrument.layers[layer].osc_mode                               = Synth::osc_mode_blend;
            instrument.layers[layer].targets[Synth::mod_volume].base_value  = 1.0f;
            instrument.layers[layer].targets[Synth::mod_panning].base_value = 0.5f;
        }
        instrument.layers[0].fir_type       = Synth::fir_off;
        instrument.layers[0].fir_band_count = 1;
        instrument.layers[1].fir_type       = Synth::fir_bandpass;
        instrument.layers[1].fir_band_count = 4;
        instrument.layers[2].fir_type       = Synth::fir_lowpass;
        instrument.layers[2].fir_band_count = 1;
        instrument.layers[3].fir_type       = Synth::fir_highpass;
        instrument.layers[3].fir_band_count = 1;

        // One envelope and one LFO descriptor, both bound to layer 0 volume.
        const uint32_t env_slot = bank.bank.envelopes.allocate();
        TEST(env_slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank.bank.envelopes.entries[env_slot];
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

        uint16_t lfo_desc_id = 0;
        TEST(Sculptor::allocate_default_lfo(&bank.bank, &lfo_desc_id));

        Synth::TargetBinding& volume = instrument.layers[0].targets[Synth::mod_volume];
        volume.envelope_desc_id      = static_cast<uint16_t>(env_slot + 1);
        volume.lfo_desc_id           = lfo_desc_id;

        bank.added_cc_mask[0] = 1u;
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              osc_mapping   = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &osc_mapping));
        TEST(osc_mapping.osc_nodes[0] != Sculptor::pool_no_slot && osc_mapping.osc_nodes[1] != Sculptor::pool_no_slot);
        TEST(osc_mapping.osc_nodes[2] != Sculptor::pool_no_slot && osc_mapping.osc_nodes[3] != Sculptor::pool_no_slot);

        uint32_t required_missing;
        uint32_t exempt_nonnull;
        sweep_tooltips(graph, osc_tooltip_exempt, &osc_mapping, &required_missing, &exempt_nonnull);
        TEST(required_missing == 0 && exempt_nonnull == 0);

        // The effect panes reuse the same graph (each projection clears it), so
        // the oscillator sweep must finish first.
        Synth::EffectChainBinding& chain = bank.bank.channel_chains[0];
        chain.num_effects                = 4;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::chorus);
        Sculptor::fx_init_slot(&chain, 3, Synth::EffectType::reverb);
        chain.effects[0].bindings[0].lfo_desc_id = lfo_desc_id;

        Synth::EffectChainBinding& master = bank.bank.master_chain;
        master.num_effects                = 3;
        Sculptor::fx_init_slot(&master, 0, Synth::EffectType::compressor);
        Sculptor::fx_init_slot(&master, 1, Synth::EffectType::fir);
        Sculptor::fx_init_slot(&master, 2, Synth::EffectType::ring_mod);
        Sculptor::fx_init_slot(&master, 3, Synth::EffectType::fir_multiband);
        master.num_effects = 4;

        Sculptor::EffectGraphMapping fx_mapping = {};
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &fx_mapping));
        sweep_tooltips(graph, fx_tooltip_exempt, &fx_mapping, &required_missing, &exempt_nonnull);
        TEST(required_missing == 0 && exempt_nonnull == 0);

        TEST(Sculptor::project_effect_chain_to_graph(bank, Sculptor::fx_master_chain, &graph, &fx_mapping));
        sweep_tooltips(graph, fx_tooltip_exempt, &fx_mapping, &required_missing, &exempt_nonnull);
        TEST(required_missing == 0 && exempt_nonnull == 0);
    }
}

int main()
{
    test_silence_stdout();
    check_shared_instrument_json_domains();
    check_clipboard_inline_0();
    return test_exit_code();
}
