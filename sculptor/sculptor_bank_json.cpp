// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan
#include "sculptor_bank_json.h"
#include "../synth/synth_effect_expansion.h"
#include "sculptor_atomic_file.h"
#include <sys/stat.h>
#ifdef _WIN32
#    include <windows.h>
#else
#    include <unistd.h>
#endif
#include "../core/d_printf.h"
#include "../core/vmath.h"
#include "sculptor_effect_graph.h"
#include "sculptor_instr_envelope_edit.h"
#include "sculptor_osc_graph.h"

// The vendored parser builds warning-clean under -Wall only; the format's strictness
// lives in this file's decoder, so its integer-conversion noise is silenced here.
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
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Writer (compact JSON)
// ---------------------------------------------------------------------------

// Copies the staged record name back into the 32-byte field after the
// object walk, when the record kind is known.  Kind 0 (oscillator) names
// are counted bytes: a full 32-byte name without a terminator is valid.
// Any other kind requires termination within 32 bytes.
static bool stage_record_name(char*       record_name,
                              uint32_t    record_name_size,
                              const char* staged,
                              uint32_t    staged_size,
                              uint8_t     kind)
{
    if (kind != 0 && ! memchr(staged, 0, record_name_size)) {
        return false;
    }

    memset(record_name, 0, record_name_size);
    Sculptor::copy_name_bounded(record_name, record_name_size, staged, staged_size);
    return true;
}

static bool string_content_ok(const char* s, uint32_t len);

struct Out {
    char*    buf;
    uint32_t size;
    uint32_t pos;
    bool     overflow;
    bool     invalid; // input string lacks a bounded terminator or well-formed UTF-8

    void raw(const char* s, uint32_t len)
    {
        if (overflow || pos + len > size) {
            overflow = true;
            return;
        }
        memcpy(buf + pos, s, len);
        pos += len;
    }

    void str(const char* s) { raw(s, static_cast<uint32_t>(strlen(s))); }

    void ch(char c) { raw(&c, 1); }

    // Item separator: a comma unless directly after an opener or another separator,
    // so empty composites and first items need no bookkeeping.
    void sep()
    {
        if (! pos)
            return;
        const char last = buf[pos - 1];
        if (last != '{' && last != '[' && last != ',')
            ch(',');
    }

    void key(const char* name)
    {
        sep();
        ch('"');
        str(name);
        str("\":");
    }

    void uint_value(uint32_t v)
    {
        char      tmp[12];
        const int n = snprintf(tmp, sizeof(tmp), "%u", v);
        if (n <= 0 || static_cast<uint32_t>(n) >= sizeof(tmp))
            overflow = true;
        else
            raw(tmp, static_cast<uint32_t>(n));
    }

    void float_value(float v)
    {
        if (! std::isfinite(v)) {
            overflow = true; // JSON cannot spell a non-finite float
            return;
        }
        if (v == 0.0f && ! std::signbit(v)) {
            ch('0'); // canonical zero; a negative zero keeps its sign via %.9g
            return;
        }
        char tmp[24];
        // %.9g round-trips every finite binary32 exactly
        const int n = snprintf(tmp, sizeof(tmp), "%.9g", static_cast<double>(v));
        if (n <= 0 || static_cast<uint32_t>(n) >= sizeof(tmp))
            overflow = true;
        else
            raw(tmp, static_cast<uint32_t>(n));
    }

    void bool_value(bool v) { str(v ? "true" : "false"); }

    // Names live in fixed char arrays; the bound replaces the NUL scan so an
    // unterminated array cannot drive the encoder past the bank structure.
    void text_value(const char* s, uint32_t max_len)
    {
        if (! memchr(s, 0, max_len)) {
            invalid = true;
            return;
        }
        ch('"');
        const uint32_t start = pos;
        for (const char* p = s; *p; ++p) {
            const char c = *p;
            switch (c) {
                case '"':
                    str("\\\"");
                    break;
                case '\\':
                    str("\\\\");
                    break;
                case '\n':
                    str("\\n");
                    break;
                case '\r':
                    str("\\r");
                    break;
                case '\t':
                    str("\\t");
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                        str(tmp);
                    }
                    else
                        ch(c);
                    break;
            }
        }
        // Escaped contents use the decoder's string grammar, including literal UTF-8.
        if (! overflow && ! string_content_ok(buf + start, pos - start))
            invalid = true;
        ch('"');
    }
};

struct Walker {
    const char*      doc;
    uint32_t         len;
    const jsmntok_t* toks;
    uint32_t         num_toks;
    bool             failed;
    bool             strict_editor_metadata;
    bool             lenient;
    bool             guarded;
};

// A key's decoded stream: every literal byte sequence and every \u escape
// yields its Unicode CODE POINT, so an escaped spelling of a name equals its
// literal UTF-8 form, two escape spellings of one code point are equal
// regardless of hex case, and any code point matches no other spelling of
// itself.  The grammar pass already validated escape spelling and UTF-8
// well-formedness, so the walk cannot fail.  Streams need no whole-key buffer:
// uniqueness and dispatch hold at any key length.
static bool escape_code_point(const char* src, uint32_t len, uint32_t& pos, int32_t& code);

struct KeySymbols {
    const char* src;
    uint32_t    len;
    uint32_t    pos;

    // The next symbol: a decoded code point, or -1 at the end of the key.
    int next()
    {
        if (pos >= len)
            return -1;

        const char c = src[pos++];

        if (c != '\\')
            return literal_code_point(static_cast<unsigned char>(c));

        const char esc = src[pos++];

        switch (esc) {
            case '"':
                return '"';
            case '\\':
                return '\\';
            case '/':
                return '/';
            case 'b':
                return '\b';
            case 'f':
                return '\f';
            case 'n':
                return '\n';
            case 'r':
                return '\r';
            case 't':
                return '\t';
            case 'u': {
                int32_t code;
                if (! escape_code_point(src, len, pos, code))
                    return -1; // unreachable: the grammar pass paired the surrogates
                return code;
            }
            default:
                return -1; // unreachable: the grammar pass validated escape spelling
        }
    }

    // Literal UTF-8: the continuation bytes follow the lead; the grammar pass
    // already rejected every malformed spelling, so the walk cannot fail.
    int literal_code_point(unsigned char lead)
    {
        if (lead < 0x80)
            return lead;

        uint32_t need;
        uint32_t cp = lead;
        if (lead >= 0xC2 && lead <= 0xDF) {
            need = 1;
            cp &= 0x1F;
        }
        else if (lead >= 0xE0 && lead <= 0xEF) {
            need = 2;
            cp &= 0x0F;
        }
        else if (lead >= 0xF0 && lead <= 0xF4) {
            need = 3;
            cp &= 0x07;
        }
        else
            return -1; // unreachable: the grammar pass rejected it

        if (pos + need > len)
            return -1;

        for (uint32_t k = 0; k < need; k++) {
            const unsigned char cc = static_cast<unsigned char>(src[pos++]);
            cp                     = (cp << 6) | (cc & 0x3F);
        }
        return static_cast<int>(cp);
    }
};

// Envelope fields appear both in envelope pool entries and inside parameter entries,
// so the member handling is shared.  num_points and the points array imply each
// other; given only one, the other is derived.
struct EnvelopeFields {
    uint32_t num_points        = 0;
    uint32_t num_point_entries = 0;
    bool     have_count        = false;
    bool     have_points       = false;
};

// Staged decode result. Static like every other staging in this file: the
// codec runs only on the GUI thread.
struct InstrumentDocState {
    Synth::Instrument         instr;
    Synth::EnvelopeDescriptor envelopes[Synth::instrument_max_envelopes];
    Synth::LFODescriptor      lfos[Synth::instrument_max_lfos];
    uint32_t                  envelope_count;
    uint16_t                  envelope_ids[Synth::instrument_max_envelopes];
    uint16_t                  lfo_ids[Synth::instrument_max_lfos];
    uint32_t                  lfo_count;
};

// ---------------------------------------------------------------------------
// Shared enum spelling: one name per value, used by both encoder and decoder
// ---------------------------------------------------------------------------

constexpr uint32_t    num_waves             = 7;
constexpr const char* wave_names[num_waves] = { "none", "sine", "sawtooth", "pulse", "noise", "pad", "string" };

constexpr uint32_t    num_voice_modes                   = 3;
constexpr const char* voice_mode_names[num_voice_modes] = { "multiple", "reuse per-note", "single" };

constexpr uint32_t    num_osc_modes                 = 4;
constexpr const char* osc_mode_names[num_osc_modes] = { "blend", "fm", "sync", "ring_mod" };

constexpr uint32_t num_filter_types = Synth::num_filter_types;
// Indexed by the stored FilterType value, so this order is fixed once banks ship.
constexpr const char* filter_type_names[num_filter_types] = { "off", "bandpass", "lowpass", "highpass" };

constexpr uint32_t    num_source_ops                  = 2;
constexpr const char* source_op_names[num_source_ops] = { "add", "multiply" };

constexpr uint32_t    num_mod_sources                   = 8;
constexpr const char* mod_source_names[num_mod_sources] = {
    "none", "pitch_bend", "mod_wheel", "channel_pressure", "velocity", "aftertouch", "pressure_combine", "cc"
};

constexpr const char* effect_type_names[Synth::num_effect_types] = { "none",   "distortion", "delay",
                                                                     "chorus", "reverb",     "compressor",
                                                                     "fir",    "ring_mod",   "fir_multiband" };

constexpr uint32_t num_param_kinds = 4;
// Stable JSON keys, enum-indexed rather than canvas menu order.
static constexpr const char* effect_param_keys[Synth::num_effect_types][Synth::max_effect_param_floats] = {
    {},
    { "drive", "mix", "output_gain" },
    { "time", "feedback", "mix" },
    { "rate", "depth", "mix", "center" },
    { "room", "damping", "wet" },
    { "threshold", "ratio", "attack", "release", "output_gain" },
    { "lowpass", "highpass", "slope" },
    { "rate", "mix" },
    { "lowpass", "highpass", "slope" }
};

constexpr const char* param_kind_names[num_param_kinds] = { "external", "envelope", "lfo", "plain" };

constexpr const char* mod_target_names[Synth::num_mod_targets] = { "volume",
                                                                   "pitch",
                                                                   "panning",
                                                                   "duty_a",
                                                                   "duty_b",
                                                                   "osc_mix",
                                                                   "fm_index",
                                                                   "fir_pitch_spread",
                                                                   "fm_ratio",
                                                                   "pad_tone_a",
                                                                   "pad_tone_b",
                                                                   "fir_slope",
                                                                   "fir_band0_offset",
                                                                   "fir_band0_gain",
                                                                   "fir_band1_offset",
                                                                   "fir_band1_spread",
                                                                   "fir_band1_gain",
                                                                   "fir_band1_slope",
                                                                   "fir_band2_offset",
                                                                   "fir_band2_spread",
                                                                   "fir_band2_gain",
                                                                   "fir_band2_slope",
                                                                   "fir_band3_offset",
                                                                   "fir_band3_spread",
                                                                   "fir_band3_gain",
                                                                   "fir_band3_slope",
                                                                   "string_damping",
                                                                   "string_release_damping",
                                                                   "string_dispersion",
                                                                   "string_brightness",
                                                                   "string_strike" };

static_assert(static_cast<uint32_t>(Synth::WaveType::string_wave) == num_waves - 1);
static_assert(static_cast<uint32_t>(Synth::ModSource::cc) == num_mod_sources - 1);
static_assert(static_cast<uint32_t>(Synth::SourceOp::multiply) == num_source_ops - 1);
static_assert(static_cast<uint32_t>(Synth::EffectType::fir) == Synth::num_effect_types - 3);
static_assert(static_cast<uint32_t>(Synth::EffectType::ring_mod) == Synth::num_effect_types - 2);
static_assert(static_cast<uint32_t>(Synth::EffectType::fir_multiband) == Synth::num_effect_types - 1);
static_assert(static_cast<uint32_t>(Synth::ParamKind::plain) == num_param_kinds - 1);
static_assert(Synth::osc_mode_ring_mod == num_osc_modes - 1);

// ---------------------------------------------------------------------------
// Bounded static staging
// ---------------------------------------------------------------------------

// Document staging bound: the largest JSON document the codec accepts.  The
// library's payload bound (library_payload_max) matches this by design.
constexpr uint32_t bank_json_text_size = 1024 * 1024;
// One token index per parser token; the decoder pins the vendored parser's
// 16-byte token size so the key-index scratch stays a fixed fraction of the pool.
constexpr uint32_t json_token_count = 32768;
static char        json_text[bank_json_text_size];
static jsmntok_t   json_tokens[json_token_count];

// Token storage uses the parser's fixed four-int representation.
static_assert(sizeof(jsmntok_t) == 16);

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

constexpr uint32_t invalid_index = 0xFFFFFFFFu;

// Post-order key-uniqueness scratch: by the time an object checks its own keys,
// every nested object finished checking (theirs ran during the recursive value
// walks below), so one shared array serves all of them.
static uint32_t object_keys[json_token_count];

// Resident parser storage is counted by the owner in sculptor_instr_edit.cpp.
static_assert(sizeof(json_text) + sizeof(json_tokens) + sizeof(object_keys) == Synth::bank_json_parser_bytes);

// ---------------------------------------------------------------------------

// Shared staging for the transactional decode: the GUI thread runs one decode at a
// time, so one static scratch serves every caller (decode is not reentrant).
static Synth::InstrumentEditorBank decode_scratch;

// ---------------------------------------------------------------------------
// Clipboard instrument documents: friendly-unit JSON for one instrument
// (doc/synth_instrument_schema.json). Decoding quantizes to the bank's
// resolution and the encoding emits those quantized values, so a document
// the editor copies out always pastes back.
// ---------------------------------------------------------------------------

constexpr const char instrument_format_tag[] = "synth-instrument-v2";

static InstrumentDocState           instrument_doc;
static Synth::InstrumentGraphLayout instrument_layout_staging[Synth::instrument_graph_layout_capacity];
static Synth::GraphNodeLayout       instrument_mapped_staging[Synth::instrument_graph_layout_capacity];

static char                         editor_export_text[128 * 1024];
static Synth::Instrument            editor_export_decoded;
static Synth::EnvelopeDescriptor    editor_export_envelopes[Synth::instrument_max_envelopes];
static Synth::LFODescriptor         editor_export_lfos[Synth::instrument_max_lfos];
static Synth::InstrumentGraphLayout editor_export_source_layout[Synth::instrument_graph_layout_capacity];
static Synth::InstrumentGraphLayout editor_export_normalized[Synth::instrument_graph_layout_capacity];
static uint8_t                      editor_export_osc_source[Synth::max_layers];

static constexpr const char* graph_kind_names[] = { "input", "sum",    "oscillator", "envelope",
                                                    "lfo",   "effect", "parameters" };

static constexpr const char* effect_input_names[] = { "disconnected", "first_slot", "output" };
static constexpr const char* effect_feed_names[]  = { "disconnected", "next", "output" };

// FIR band-object decoder keys: the plain FIR's dormant band objects name
// their edge bindings lowpass/highpass; the multiband type's name the same
// positions center/spread.
constexpr const char* fir_edge_keys[2]           = { "lowpass", "highpass" };
constexpr const char* multiband_fir_edge_keys[2] = { "center", "spread" };
// The band-object keys both FIR types share: the plain FIR's dormant band
// objects are fir_band1..3 (band 0 rides the fir_band0_gain key) and the
// multiband's are fir_band0..3.
constexpr const char* fir_band_keys[Synth::max_fir_effect_bands] = { "fir_band0",
                                                                     "fir_band1",
                                                                     "fir_band2",
                                                                     "fir_band3" };

static void key_uint(Out& o, const char* name, uint32_t v)
{
    o.key(name);
    o.uint_value(v);
}

// Fields whose default is zero are omitted; the decoder fills that default.
static void key_uint_nonzero(Out& o, const char* name, uint32_t v)
{
    if (v == 0)
        return;
    key_uint(o, name, v);
}

static void key_float(Out& o, const char* name, float v)
{
    o.key(name);
    o.float_value(v);
}

static void key_float_nonzero(Out& o, const char* name, float v)
{
    if (v == 0.0f)
        return; // the canonical form keeps no negative zero; decode yields positive zero
    key_float(o, name, v);
}

// A dispersion binding carrying any modulation: it encodes as a target block under the key "string_dispersion",
// while an unmodulated one encodes as the plain number under the same key (the FIR slope convention).
static bool dispersion_routed(const Synth::TargetBinding& binding)
{
    return binding.num_inputs != 0 || binding.envelope_desc_id != 0 || binding.lfo_desc_id != 0;
}

// Dispersion and its section count.  The encoder emits only values the decoder accepts: a finite dispersion in
// [0,1] other than zero, and an integer section count in [1,6] other than the default four.  Anything else stays out
// of the document and decodes as the default.
static void key_dispersion(Out& o, const Synth::Oscillator& osc)
{
    const Synth::TargetBinding& dispersion = osc.targets[Synth::mod_string_dispersion];
    if (! dispersion_routed(dispersion) && dispersion.base_value >= 0.0f && dispersion.base_value <= 1.0f) {
        key_float_nonzero(o, "string_dispersion", dispersion.base_value);
    }
    const float sections = osc.string_dispersion_sections;
    if (sections >= 1.0f && sections <= 6.0f && sections != 4.0f &&
        sections == static_cast<float>(static_cast<int32_t>(sections))) {
        key_float(o, "string_dispersion_sections", sections);
    }
}

static void key_enum(Out& o, const char* name, const char* const* names, uint32_t num_names, uint32_t v)
{
    if (v == 0)
        return; // the zero value of every encoded enum is its default
    if (v >= num_names) {
        o.overflow = true; // a value the format cannot spell
        return;
    }
    o.key(name);
    o.ch('"');
    o.str(names[v]);
    o.ch('"');
}

// Writes the oscillator filter type.  Unlike key_enum, the zero value ("off")
// is spelled out: an absent key decodes as band-pass, the historic default of
// documents written before the type existed.
static void key_fir_type(Out& o, const Synth::Oscillator& osc)
{
    if (osc.fir_type == Synth::fir_bandpass) {
        return;
    }
    o.key("fir_type");
    o.ch('"');
    o.str(filter_type_names[static_cast<uint32_t>(osc.fir_type)]);
    o.ch('"');
}

static void key_bool(Out& o, const char* name, bool v)
{
    o.key(name);
    o.bool_value(v);
}

static bool gen_is_default(const Synth::TargetBinding& gen)
{
    return gen.envelope_desc_id == 0 && gen.lfo_desc_id == 0 && gen.lfo_op == Synth::SourceOp::add &&
           gen.lfo_depth == 0.0f && gen.lfo_depth_source == Synth::ModSource::none &&
           gen.lfo_rate_source == Synth::ModSource::none && gen.lfo_rate_scale_ms == 0.0f;
}

static bool routing_is_default(const Synth::TargetBinding& routing)
{
    return routing.base_value == 0.0f && routing.num_inputs == 0;
}

static void enc_mod_input(Out& o, const Synth::ModInput& in, bool ms = false)
{
    o.ch('{');
    key_enum(o, "source", mod_source_names, num_mod_sources, static_cast<uint32_t>(in.source));
    key_enum(o, "op", source_op_names, num_source_ops, static_cast<uint32_t>(in.op));
    key_uint_nonzero(o, "cc_number", in.cc_number);
    key_float_nonzero(o, ms ? "scale_ms" : "scale", in.scale);
    o.ch('}');
}

static void enc_gen(Out& o, const Synth::TargetBinding& gen)
{
    o.ch('{');
    key_uint_nonzero(o, "envelope_desc_id", gen.envelope_desc_id);
    if (gen.envelope_desc_id != 0 && gen.envelope_depth != 1.0f) {
        o.key("envelope_depth");
        o.float_value(gen.envelope_depth);
    }
    key_uint_nonzero(o, "lfo_desc_id", gen.lfo_desc_id);
    key_enum(o, "lfo_op", source_op_names, num_source_ops, static_cast<uint32_t>(gen.lfo_op));
    key_float_nonzero(o, "lfo_depth", gen.lfo_depth);
    key_enum(o, "lfo_depth_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(gen.lfo_depth_source));
    key_enum(o, "lfo_rate_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(gen.lfo_rate_source));
    key_float_nonzero(o, "lfo_rate_scale_ms", gen.lfo_rate_scale_ms);
    o.ch('}');
}

static void enc_routing(Out& o, const Synth::TargetBinding& routing)
{
    if (routing.num_inputs > Synth::max_mod_inputs) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_float_nonzero(o, "base_value", routing.base_value);
    if (routing.num_inputs) {
        o.key("inputs");
        o.ch('[');
        for (uint32_t i = 0; i < routing.num_inputs; i++) {
            o.sep();
            enc_mod_input(o, routing.inputs[i]);
        }
        o.ch(']');
    }
    o.ch('}');
}

static void enc_oscillator(Out& o, const Synth::Oscillator& osc)
{
    o.ch('{');
    key_enum(o, "wave_a", wave_names, num_waves, static_cast<uint32_t>(osc.osc_type[0]));
    key_enum(o, "wave_b", wave_names, num_waves, static_cast<uint32_t>(osc.osc_type[1]));
    key_enum(o, "mode", osc_mode_names, num_osc_modes, osc.osc_mode);
    key_float_nonzero(o, "mod_ratio", osc.mod_ratio);
    // Full brightness is the default and stays out of the document.
    if (osc.string_brightness != 1.0f) {
        key_float(o, "string_brightness", osc.string_brightness);
    }
    key_float_nonzero(o, "string_strike", osc.string_strike);
    if (osc.string_drive != 3.0f) {
        key_float(o, "string_drive", osc.string_drive);
    }
    if (osc.string_spectrum != 2.0f) {
        key_float(o, "string_spectrum", osc.string_spectrum);
    }
    key_dispersion(o, osc);
    key_float_nonzero(o, "octave_offset", static_cast<float>(osc.octave_offset));

    // The default slope omits the key, matching the nonzero-key omission
    // convention of this encoder.
    if (osc.fir_slope_percent != 100) {
        o.key("fir_slope");
        o.uint_value(osc.fir_slope_percent);
    }
    key_fir_type(o, osc);
    // The count key rides only multi-band layers: a single-band instrument
    // keeps the document short and the absent count decodes as 1.
    if (osc.fir_band_count > 1) {
        o.key("fir_bands");
        o.uint_value(osc.fir_band_count);
    }

    o.key("generators");
    o.ch('{');
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        if (gen_is_default(osc.targets[t]) ||
            Sculptor::osc_target_band_index(t) >=
                static_cast<int32_t>(osc.fir_band_count < 1u ? 1u : osc.fir_band_count)) {
            continue; // default generator, or a band beyond the active count
        }
        o.sep();
        o.ch('"');
        o.str(mod_target_names[t]);
        o.str("\":");
        enc_gen(o, osc.targets[t]);
    }
    o.ch('}');

    // Routing is per layer; the object is always emitted.  The only
    // exception is the zeroed-binding-with-zero-scalar case: fm_ratio
    // with a zeroed binding and a zero scalar is omitted, because
    // decode backfills the target from the scalar whenever no block
    // names fm_ratio (see decode_oscillator).  A zeroed binding's
    // base stays zero even when the scalar is nonzero; the special
    // branch below emits the scalar instead, so the nonzero value
    // survives the round trip.
    o.key("routing");
    o.ch('{');
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        const bool fm_ratio  = t == static_cast<uint32_t>(Synth::mod_fm_ratio);
        const bool band_gain = t == static_cast<uint32_t>(Synth::mod_fir_band0_gain) ||
                               t == static_cast<uint32_t>(Synth::mod_fir_band1_gain) ||
                               t == static_cast<uint32_t>(Synth::mod_fir_band2_gain) ||
                               t == static_cast<uint32_t>(Synth::mod_fir_band3_gain);
        if (Sculptor::osc_target_band_index(t) >=
            static_cast<int32_t>(osc.fir_band_count < 1u ? 1u : osc.fir_band_count)) {
            continue; // a band beyond the active count never leaks into the document
        }
        // A base-0 unwired gain in a multi-band bandpass layer encodes as
        // the empty block, which decodes as the gain-0 band: the explicit
        // inactive band survives the round trip instead of reloading as the
        // 1.0 absent default.  A single-band layer keeps the stored
        // omission: its absent key decodes the stored audible 1.0, the
        // pre-band-UI convention for the zeroed single-band gain binding.
        // A non-bandpass layer keeps the stored omission too: its band
        // count is a retained stored/hand-edited state, and its absent
        // band gain decodes the zeroed default.
        const bool band_gain_zero = band_gain && routing_is_default(osc.targets[t]) &&
                                    osc.fir_type == Synth::fir_bandpass && osc.fir_band_count > 1u;
        if (routing_is_default(osc.targets[t]) && (! fm_ratio || osc.mod_ratio == 0.0f) && ! band_gain_zero) {
            continue;
        }
        if (t == static_cast<uint32_t>(Synth::mod_string_dispersion) && ! dispersion_routed(osc.targets[t])) {
            continue; // the unmodulated value rides the plain "string_dispersion" number
        }
        o.sep();
        o.ch('"');
        o.str(mod_target_names[t]);
        o.str("\":");
        if (fm_ratio && routing_is_default(osc.targets[t])) {
            o.ch('{');
            key_float_nonzero(o, "base_value", osc.mod_ratio);
            o.ch('}');
        }
        else {
            enc_routing(o, osc.targets[t]);
        }
    }
    o.ch('}');
    o.ch('}');
}

static void enc_instrument(Out& o, const Synth::Instrument& instr, uint32_t id = 0, const char* name = nullptr)
{
    if (instr.layer_count > Synth::max_layers) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_uint_nonzero(o, "id", id);
    if (name) {
        o.key("name");
        o.text_value(name, Synth::max_name_len);
    }

    o.key("layers");
    o.ch('[');
    for (uint32_t layer = 0; layer < instr.layer_count; layer++) {
        o.sep();
        enc_oscillator(o, instr.layers[layer]);
    }
    o.ch(']');

    key_float_nonzero(o, "note_skew_semitones", instr.note_skew_semitones);
    key_float_nonzero(o, "layer_skew_semitones", instr.layer_skew_semitones);
    key_float_nonzero(o, "release_seconds", instr.release_seconds);
    key_enum(o, "voices", voice_mode_names, num_voice_modes, instr.voice_mode);
    o.key("pitch_bend_lock");
    o.bool_value(instr.pitch_bend_lock != 0);
    o.ch('}');
}

static void enc_envelope_members(Out& o, const Synth::EnvelopeDescriptor& env)
{
    if (env.num_points > Synth::max_envelope_points) {
        o.overflow = true; // unchecked standalone parameter contents
        return;
    }

    key_uint(o, "num_points", env.num_points);
    key_uint_nonzero(o, "sustain_first", env.sustain_first_point);
    key_uint_nonzero(o, "sustain_last", env.sustain_last_point);
    key_uint_nonzero(o, "ping_pong", (env.flags & Synth::envelope_ping_pong) != 0);
    key_uint_nonzero(o, "quick_release", (env.flags & Synth::envelope_quick_release) != 0);
    key_float_nonzero(o, "min_value", env.min_value);
    key_float_nonzero(o, "min_max_delta", env.min_max_delta);

    o.key("points");
    o.ch('[');
    for (uint32_t p = 0; p < env.num_points; p++) {
        const Synth::EnvelopeDescriptor::Point& point = env.points[p];
        o.sep();
        o.ch('{');
        key_uint_nonzero(o, "position", point.position);
        key_uint_nonzero(o, "value", point.value);
        o.ch('}');
    }
    o.ch(']');
}

static void enc_envelope(Out& o, const Synth::EnvelopeDescriptor& env, uint32_t id = 0, const char* name = nullptr)
{
    o.ch('{');
    key_uint_nonzero(o, "id", id);
    if (name) {
        o.key("name");
        o.text_value(name, Synth::max_name_len);
    }
    enc_envelope_members(o, env);
    o.ch('}');
}

static void enc_lfo(Out& o, const Synth::LFODescriptor& lfo, uint32_t id = 0, const char* name = nullptr)
{
    o.ch('{');
    key_uint_nonzero(o, "id", id);
    if (name) {
        o.key("name");
        o.text_value(name, Synth::max_name_len);
    }
    key_enum(o, "wave", wave_names, num_waves, static_cast<uint32_t>(lfo.wave));
    key_uint_nonzero(o, "duty", lfo.duty);
    key_uint_nonzero(o, "period_ms", lfo.period_ms);
    key_float_nonzero(o, "min_value", lfo.min_value);
    key_float_nonzero(o, "min_max_delta", lfo.min_max_delta);
    key_uint_nonzero(o, "pulse_mix", lfo.pulse_mix);
    key_uint_nonzero(o, "beat_div", lfo.beat_div);
    key_uint_nonzero(o, "start_point", lfo.start_point);
    o.ch('}');
}

static void enc_param(Out& o, const Synth::ParamDescriptor& param, uint32_t id = 0, const char* name = nullptr)
{
    const uint32_t kind = static_cast<uint32_t>(param.kind);
    if (kind >= num_param_kinds) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_uint_nonzero(o, "id", id);
    if (name) {
        o.key("name");
        o.text_value(name, Synth::max_name_len);
    }
    o.key("kind");
    o.ch('"');
    o.str(param_kind_names[kind]);
    o.ch('"');

    switch (param.kind) {
        case Synth::ParamKind::external:
            break;
        case Synth::ParamKind::envelope:
            enc_envelope_members(o, param.envelope);
            break;
        case Synth::ParamKind::lfo:
            o.key("lfo");
            enc_lfo(o, param.lfo.lfo);
            key_enum(o, "op", source_op_names, num_source_ops, static_cast<uint32_t>(param.lfo.op));
            key_float_nonzero(o, "depth", param.lfo.depth);
            key_uint(o, "depth_param_id", static_cast<uint32_t>(param.lfo.depth_param_id) + 1);
            key_uint(o, "rate_param_id", static_cast<uint32_t>(param.lfo.rate_param_id) + 1);
            key_float_nonzero(o, "rate_scale_ms", param.lfo.rate_scale_ms);
            break;
        case Synth::ParamKind::plain:
            if (param.plain.num_sources > Synth::max_param_sources) {
                o.overflow = true; // the validator leaves standalone contents unchecked
                return;
            }
            key_float_nonzero(o, "base_value", param.plain.base_value);
            if (param.plain.num_sources) {
                o.key("sources");
                o.ch('[');
                for (uint32_t s = 0; s < param.plain.num_sources; s++) {
                    const Synth::SourceParam& src = param.plain.sources[s];
                    o.sep();
                    o.ch('{');
                    key_uint(o, "param_id", static_cast<uint32_t>(src.param_id) + 1);
                    key_enum(o, "op", source_op_names, num_source_ops, static_cast<uint32_t>(src.op));
                    key_float_nonzero(o, "scale", src.scale);
                    o.ch('}');
                }
                o.ch(']');
            }
            break;
    }
    o.ch('}');
}

static void enc_binding(Out& o, const Synth::EffectParamBinding& binding, bool ms = false)
{
    if (binding.num_inputs > Synth::max_mod_inputs) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    if (ms) {
        o.key("base_value_ms");
        o.float_value(binding.base_value);
    }
    else
        key_float_nonzero(o, "base_value", binding.base_value);
    key_uint_nonzero(o, "lfo_desc_id", binding.lfo_desc_id);
    key_enum(o, "lfo_op", source_op_names, num_source_ops, static_cast<uint32_t>(binding.lfo_op));
    key_float_nonzero(o, ms ? "lfo_depth_ms" : "lfo_depth", binding.lfo_depth);
    key_enum(o, "lfo_depth_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(binding.lfo_depth_source));
    key_enum(o, "lfo_rate_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(binding.lfo_rate_source));
    key_float_nonzero(o, "lfo_rate_scale", binding.lfo_rate_scale);
    if (binding.num_inputs) {
        o.key("inputs");
        o.ch('[');
        for (uint32_t i = 0; i < binding.num_inputs; i++) {
            o.sep();
            enc_mod_input(o, binding.inputs[i], ms);
        }
        o.ch(']');
    }
    o.ch('}');
}

// One FIR effect band's four bindings (edges, gain, slope).  Sub-keys the
// decoder reads as defaults stay omitted, so a fully default band encodes nothing and
// decodes back unchanged.  An unwired zero gain encodes as the empty binding block: an
// absent gain key would decode as the neutral 1.0 instead.  edge_keys names the two
// edge bindings (the plain FIR's lowpass/highpass pair or the multiband type's
// center/spread pair).
static void enc_fir_band_object(Out&                             o,
                                const char*                      band_key,
                                const Synth::EffectParamBinding* band,
                                const char* const*               edge_keys)
{
    const Synth::EffectParamBinding& lowpass  = band[0];
    const Synth::EffectParamBinding& highpass = band[1];
    const Synth::EffectParamBinding& gain     = band[2];
    const Synth::EffectParamBinding& slope    = band[3];
    const bool lowpass_default  = ! lowpass.lfo_desc_id && ! lowpass.num_inputs && lowpass.base_value == 0.0f;
    const bool highpass_default = ! highpass.lfo_desc_id && ! highpass.num_inputs && highpass.base_value == 0.0f;
    const bool gain_default     = ! gain.lfo_desc_id && ! gain.num_inputs && gain.base_value == 1.0f;
    const bool slope_default    = ! slope.lfo_desc_id && ! slope.num_inputs && slope.base_value == 100.0f;
    if (lowpass_default && highpass_default && gain_default && slope_default)
        return;
    o.key(band_key);
    o.ch('{');
    if (! lowpass_default) {
        o.key(edge_keys[0]);
        enc_binding(o, lowpass);
    }
    if (! highpass_default) {
        o.key(edge_keys[1]);
        enc_binding(o, highpass);
    }
    if (! gain_default) {
        o.key("gain");
        enc_binding(o, gain);
    }
    if (! slope_default) {
        o.key("slope");
        enc_binding(o, slope);
    }
    o.ch('}');
}

// The plain FIR effect's dormant extra bands: keys fir_band1..3, lowpass/highpass
// edge naming, bands 1..3 only.
static void enc_fir_effect_band(Out& o, const Synth::EffectParamBinding* band, uint32_t band_index)
{
    if (band_index < 1 || band_index > 3) {
        o.overflow = true;
        return;
    }
    enc_fir_band_object(o, fir_band_keys[band_index], band, fir_edge_keys);
}

static void enc_effect(Out& o, const Synth::EffectSlotBinding& slot, bool strict = false, uint8_t feed = 0)
{
    const uint32_t type = static_cast<uint32_t>(slot.type);
    if (type >= Synth::num_effect_types) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_enum(o, "feeds", effect_feed_names, 3, feed);
    if (strict) {
        o.key("type");
        o.text_value(effect_type_names[type], 32);
    }
    else {
        key_enum(o, "type", effect_type_names, Synth::num_effect_types, type);
    }
    if (slot.enabled)
        key_bool(o, "enabled", true);

    // The enable routing encodes only when wired, so an unwired slot keeps
    // the omission-normalized shape old banks load from.
    if (slot.enable_input.source != Synth::ModSource::none) {
        o.key("enable_input");
        enc_mod_input(o, slot.enable_input);
    }

    if (slot.type != Synth::EffectType::none) {
        o.key("params");
        o.ch('{');
        const uint32_t num_params = Synth::get_effect_param_floats(slot.type);
        for (uint32_t p = 0; p < num_params; p++) {
            o.key(effect_param_keys[type][p]);
            enc_binding(o, slot.bindings[p], Synth::effect_param_is_ms(slot.type, p));
        }
        o.ch('}');
    }
    // FIR bands: the count key appears only above the stored single band.
    // Band 0's gain key encodes whenever its binding is non-default, so an
    // authored single-band gain survives the round trip; the stored zeroed
    // block stays omitted (the stored default) and the multi-band neutral
    // 1.0 stays omitted (an absent key decodes the neutral 1.0, so an
    // explicit zero gain rides the empty block).
    if (slot.type == Synth::EffectType::fir) {
        uint32_t band_count = slot.fir_bands.band_count;
        if (band_count < 1)
            band_count = 1;
        if (band_count > Synth::max_fir_effect_bands)
            band_count = Synth::max_fir_effect_bands;
        if (band_count > 1)
            key_uint(o, "fir_bands", band_count);
        const Synth::EffectParamBinding& gain0 = slot.fir_bands.bands[0];
        const bool                       gain0_default =
            ! gain0.lfo_desc_id && ! gain0.num_inputs && gain0.base_value == (band_count > 1 ? 1.0f : 0.0f);
        if (! gain0_default) {
            o.key("fir_band0_gain");
            enc_binding(o, gain0);
        }
        for (uint32_t band = 1; band < band_count; band++)
            enc_fir_effect_band(o, slot.fir_bands.bands + 1 + 4 * (band - 1), band);
    }
    // The multiband type reads the band array uniformly (band b at 4 * b +
    // center/spread/gain/slope), so every band of the clamped count encodes the
    // same way; the count key appears only above the single band.
    else if (slot.type == Synth::EffectType::fir_multiband) {
        uint32_t band_count = slot.fir_bands.band_count;
        if (band_count < 1)
            band_count = 1;
        if (band_count > Synth::max_fir_effect_bands)
            band_count = Synth::max_fir_effect_bands;
        if (band_count > 1)
            key_uint(o, "fir_bands", band_count);
        for (uint32_t band = 0; band < band_count; band++) {
            const Synth::EffectParamBinding* band_bindings = slot.fir_bands.bands + 4 * band;
            // A zeroed band is the fresh slot's inactive state; it encodes
            // nothing and decodes the neutral defaults (the dead gain/slope
            // zero bytes normalize to 1/100 on the round trip, and the band
            // stays inactive either way because its center/spread read 0).
            bool zeroed = true;
            for (uint32_t kind = 0; kind < 4; kind++)
                zeroed = zeroed && ! band_bindings[kind].lfo_desc_id && ! band_bindings[kind].num_inputs &&
                         band_bindings[kind].base_value == 0.0f;
            if (zeroed)
                continue;
            enc_fir_band_object(o, fir_band_keys[band], band_bindings, multiband_fir_edge_keys);
        }
    }
    o.ch('}');
}

static bool chain_is_empty(const Synth::EffectChainBinding& chain, const Sculptor::EffectChainRouting& routing)
{
    // Effects keep editor intent even when the chain input is disconnected.
    // Only a chain with no effects and no routing can be omitted.
    if (chain.num_effects || routing.in != Sculptor::effect_in_disconnected)
        return false;
    for (uint32_t s = 0; s < Synth::max_chain_effects; ++s)
        if (routing.out[s] != Sculptor::effect_out_disconnected)
            return false;
    return true;
}

static void enc_chain_content(Out&                                o,
                              const Synth::EffectChainBinding&    chain,
                              const Sculptor::EffectChainRouting& routing,
                              bool                                strict = false)
{
    if (chain.num_effects > Synth::max_chain_effects) {
        o.overflow = true;
        return;
    }
    key_enum(o, "input", effect_input_names, 3, routing.in);
    if (strict || chain.num_effects) {
        o.key("effects");
        o.ch('[');
        for (uint32_t s = 0; s < chain.num_effects; s++) {
            o.sep();
            enc_effect(o, chain.effects[s], strict, routing.out[s]);
        }
        o.ch(']');
    }
}

static void enc_chain(Out& output, const Synth::EffectChainBinding& chain, const Sculptor::EffectChainRouting& routing)
{
    output.ch('{');
    enc_chain_content(output, chain, routing);
    output.ch('}');
}

static bool channel_has_custom_name(const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    if (! bank.channel_names[channel][0])
        return false;
    char default_name[Synth::max_name_len];
    Synth::get_default_channel_name(channel, default_name, sizeof(default_name));
    return strncmp(bank.channel_names[channel], default_name, Synth::max_name_len) != 0;
}

static bool channel_has_category(const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    if (! bank.channel_category[channel][0])
        return false;
    char text[160];
    static_assert(sizeof(text) >= Synth::max_name_len * 6 + 2);
    Out output = { text, sizeof(text), 0, false, false };
    output.text_value(bank.channel_category[channel], Synth::max_name_len);
    return ! output.invalid && ! output.overflow;
}

static bool channel_is_default(const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    if (! bank.bank.channel_enabled[channel])
        return true;

    return ! bank.bank.channel_zones[channel][0].start_note &&
           chain_is_empty(bank.bank.channel_chains[channel], bank.effect_routing[channel]) &&
           ! channel_has_custom_name(bank, channel) && ! channel_has_category(bank, channel);
}

static void enc_channel(Out& o, const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    const Synth::Zone* const zones = bank.bank.channel_zones[channel];
    const bool empty_chain         = chain_is_empty(bank.bank.channel_chains[channel], bank.effect_routing[channel]);
    o.ch('{');
    // Interior defaults keep their channel position.  Decode restores the
    // disabled state and factory name; empty names have the same display name.
    if (channel_is_default(bank, channel)) {
        o.ch('}');
        return;
    }
    if (channel_has_custom_name(bank, channel)) {
        o.key("name");
        o.text_value(bank.channel_names[channel], Synth::max_name_len);
    }
    if (channel_has_category(bank, channel)) {
        o.key("category");
        o.text_value(bank.channel_category[channel], Synth::max_name_len);
    }
    if (zones[0].start_note) {
        o.key("zones");
        o.ch('[');
        for (uint32_t z = 0; z < Synth::max_instr_per_channel && zones[z].start_note != 0; z++) {
            o.sep();
            o.ch('{');
            // start is the zone's first note; the stored byte keeps the +1 encoding
            key_uint(o, "start", static_cast<uint32_t>(zones[z].start_note) - 1);
            key_uint(o, "instrument", static_cast<uint32_t>(zones[z].instrument) + 1);
            o.ch('}');
        }
        o.ch(']');
    }
    if (! empty_chain) {
        o.key("chain");
        enc_chain(o, bank.bank.channel_chains[channel], bank.effect_routing[channel]);
    }
    o.ch('}');
}

uint32_t Synth::encode_editor_bank_json(const InstrumentEditorBank* bank, char* dest, uint32_t dest_size)
{
    // Pool counts drive the loops below; unchecked metadata must never index past
    // a pool's entries
    if (bank->bank.instruments.num_allocated > Synth::max_instruments ||
        bank->bank.envelopes.num_allocated > Synth::max_envelopes || bank->bank.lfos.num_allocated > Synth::max_lfos ||
        bank->bank.parameters.num_allocated > Synth::max_parameters ||
        bank->graph_layout_count > Synth::max_graph_records)
        return 0;

    for (uint32_t owner = 0; owner <= max_channels; ++owner) {
        const EffectChainBinding& chain =
            owner < max_channels ? bank->bank.channel_chains[owner] : bank->bank.master_chain;
        if (! Sculptor::validate_effect_routing(chain, bank->effect_routing[owner])) {
            return 0;
        }
    }
    Out o = { dest, dest_size, 0, false, false };
    o.str("{\"instrument_editor_bank\":{");
    o.key("format");
    o.text_value("synth-bank-v2", sizeof("synth-bank-v2"));

    o.key("instruments");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.instruments.num_allocated; i++) {
        o.sep();
        enc_instrument(o, bank->bank.instruments.entries[i], i + 1, bank->instrument_names[i]);
    }
    o.ch(']');

    o.key("envelopes");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.envelopes.num_allocated; i++) {
        o.sep();
        enc_envelope(o, bank->bank.envelopes.entries[i], i + 1);
    }
    o.ch(']');

    o.key("lfos");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.lfos.num_allocated; i++) {
        o.sep();
        enc_lfo(o, bank->bank.lfos.entries[i], i + 1);
    }
    o.ch(']');

    o.key("parameters");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.parameters.num_allocated; i++) {
        o.sep();
        enc_param(o, bank->bank.parameters.entries[i], i + 1);
    }
    o.ch(']');

    uint32_t channel_count = Synth::max_channels;
    while (channel_count && channel_is_default(*bank, channel_count - 1))
        --channel_count;
    // Missing trailing entries use decode defaults and need no position markers.
    if (channel_count) {
        o.key("channels");
        o.ch('[');
        for (uint32_t c = 0; c < channel_count; c++) {
            o.sep();
            enc_channel(o, *bank, c);
        }
        o.ch(']');
    }
    if (! chain_is_empty(bank->bank.master_chain, bank->effect_routing[Synth::max_channels])) {
        o.key("master_chain");
        enc_chain(o, bank->bank.master_chain, bank->effect_routing[Synth::max_channels]);
    }
    // Editor-side state (sparse per-zone node layout records and the missing
    // oscillator-sum masks).
    o.key("editor");
    o.ch('{');
    o.key("layouts");
    o.ch('[');
    for (uint32_t i = 0; i < bank->graph_layout_count; i++) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        o.sep();
        o.ch('{');
        uint32_t kind = record.kind == 0   ? (record.index < 2 ? record.index : 2u)
                        : record.kind == 1 ? 3u
                        : record.kind == 2 ? 4u
                        : record.kind == 4 ? 5u
                                           : 6u;
        o.key("kind");
        o.text_value(graph_kind_names[kind], 16);
        if (record.kind == 4) {
            o.key("chain");
            if (record.channel == Synth::max_channels)
                o.text_value("master", sizeof("master"));
            else
                o.uint_value(record.channel);
        }
        else {
            key_uint(o, "channel", record.channel);
            key_uint(o, "zone", record.zone);
        }
        if (kind == 2)
            key_uint(o, "layer", record.index - 2u);
        if (kind == 3)
            key_uint(o, "envelope_id", record.index);
        if (kind == 4)
            key_uint(o, "lfo_id", record.index);
        key_float(o, "x", record.x);
        key_float(o, "y", record.y);
        key_float(o, "width", record.width_override);
        key_float(o, "height", record.height_override);
        if (record.kind == 2) {
            if (record.depth_source >= 7 || record.rate_source >= 7) {
                o.overflow = true;
                return 0;
            }
            o.key("depth_source");
            o.text_value(mod_source_names[record.depth_source], 32);
            o.key("rate_source");
            o.text_value(mod_source_names[record.rate_source], 32);
        }
        key_uint(o, "uid", record.uid);
        uint16_t lfo_id = 0;
        if (record.kind == 4 && Sculptor::parse_effect_lfo_title(record.name, &lfo_id) && lfo_id) {
            o.key("node");
            o.text_value("lfo", sizeof("lfo"));
            key_uint(o, "lfo_id", lfo_id);
        }
        else {
            o.key("name");
            char staged_name[sizeof(record.name) + 1] = {};
            Sculptor::copy_name_bounded(staged_name, sizeof(staged_name), record.name, sizeof(record.name));
            o.text_value(staged_name, sizeof(staged_name));
        }
        o.ch('}');
    }
    o.ch(']');
    o.key("osc_sources");
    o.ch('[');
    for (uint32_t c = 0; c < Synth::max_channels; ++c) {
        for (uint32_t z = 0; z < Synth::max_instr_per_channel; ++z) {
            bool wired = false;
            for (uint32_t slot = 0; slot < Synth::max_layers; ++slot)
                wired = wired || bank->graph_osc_source[c][z][slot] != 0;
            if (! wired)
                continue;
            o.sep();
            o.ch('{');
            key_uint(o, "channel", c);
            key_uint(o, "zone", z);
            o.key("wiring");
            o.ch('[');
            for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
                const uint8_t layer = bank->graph_osc_source[c][z][slot];
                if (! layer)
                    continue;
                o.sep();
                o.ch('{');
                key_uint(o, "slot", slot);
                key_uint(o, "layer", layer);
                o.ch('}');
            }
            o.ch(']');
            o.ch('}');
        }
    }
    o.ch(']');
    // The added-CC registry rides the editor section only when any chain
    // added a CC; absence decodes to all-zero masks.
    bool any_added_cc = false;
    for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
        if (bank->added_cc_mask[c] != 0) {
            any_added_cc = true;
            break;
        }
    }

    if (any_added_cc) {
        o.key("added_ccs");
        o.ch('[');
        for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
            const uint16_t mask = bank->added_cc_mask[c];
            if (! mask)
                continue;
            o.sep();
            o.ch('{');
            o.key("chain");
            if (c == Synth::max_channels)
                o.text_value("master", sizeof("master"));
            else
                o.uint_value(c);
            o.key("ccs");
            o.ch('[');
            for (uint32_t bit = 0; bit < 16; ++bit) {
                if (! (mask & (1u << bit)))
                    continue;
                o.sep();
                o.uint_value(16 + bit);
            }
            o.ch(']');
            o.ch('}');
        }
        o.ch(']');
    }

    o.ch('}');

    o.str("}}");

    if (o.overflow || o.invalid || o.pos + 1 > dest_size)
        return 0;

    dest[o.pos] = 0;
    return o.pos;
}

static bool tok_text_eq(const Walker& w, uint32_t i, const char* literal)
{
    const jsmntok_t& t   = w.toks[i];
    const uint32_t   len = static_cast<uint32_t>(t.end - t.start);
    // Tokens never span a NUL, so a literal shorter than the token mismatches on its
    // terminator and a longer one on the extra character.
    return strncmp(w.doc + t.start, literal, len) == 0 && literal[len] == 0;
}

// Index one past the token's complete subtree, or invalid_index when the subtree is
// structurally inconsistent (odd object size, non-string key, walk off the pool).
static uint32_t skip_value(const Walker& w, uint32_t i)
{
    if (i >= w.num_toks)
        return invalid_index;

    const jsmntok_t& t = w.toks[i];

    switch (t.type) {
        case JSMN_STRING:
        case JSMN_PRIMITIVE:
            return i + 1;

        case JSMN_ARRAY:
        case JSMN_OBJECT: {
            const bool is_object = t.type == JSMN_OBJECT;

            uint32_t  child = i + 1;
            const int units = t.size; // objects count pairs, arrays count elements

            for (int u = 0; u < units; u++) {
                if (is_object) {
                    if (child >= w.num_toks || w.toks[child].type != JSMN_STRING)
                        return invalid_index;
                    child++;
                }

                child = skip_value(w, child);
                if (child == invalid_index)
                    return invalid_index;
            }

            return child;
        }

        default:
            return invalid_index;
    }
}

// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?  - the whole JSON number grammar,
// so leading zeros, lone signs and truncated exponents are rejected.
static bool number_grammar_ok(const char* s, uint32_t len)
{
    uint32_t i = 0;

    if (i < len && s[i] == '-')
        i++;

    if (i >= len)
        return false;

    if (s[i] == '0')
        i++;
    else if (s[i] >= '1' && s[i] <= '9') {
        while (i < len && s[i] >= '0' && s[i] <= '9')
            i++;
    }
    else
        return false;

    if (i < len && s[i] == '.') {
        i++;
        if (i >= len || s[i] < '0' || s[i] > '9')
            return false;
        while (i < len && s[i] >= '0' && s[i] <= '9')
            i++;
    }

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-'))
            i++;
        if (i >= len || s[i] < '0' || s[i] > '9')
            return false;
        while (i < len && s[i] >= '0' && s[i] <= '9')
            i++;
    }

    return i == len;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Grammar of every primitive token, including the subtrees the schema below never
// visits: malformed content anywhere in the document is rejected.
static bool primitive_is_valid(const Walker& w, uint32_t i)
{
    const jsmntok_t&  t   = w.toks[i];
    const char* const s   = w.doc + t.start;
    const uint32_t    len = static_cast<uint32_t>(t.end - t.start);

    if (len == 4 && strncmp(s, "true", 4) == 0)
        return true;
    if (len == 5 && strncmp(s, "false", 5) == 0)
        return true;
    if (len == 4 && strncmp(s, "null", 4) == 0)
        return true;
    return number_grammar_ok(s, len);
}

// ---------------------------------------------------------------------------
// Document-wide grammar, verified before any schema decoding
// ---------------------------------------------------------------------------

// jsmn produces a token tree but tolerates loose JSON: it does not enforce element
// separators, and its string scanner accepts raw control characters.  The bank
// format is stricter: this pass walks every token together with the raw text
// between tokens, so malformed content is rejected everywhere in the document,
// including inside unknown subtrees the schema decoder never visits.

// Four hexadecimal digits at src[pos..pos+4): the value, with pos advanced.
static bool read_hex4(const char* src, uint32_t len, uint32_t& pos, uint32_t& value)
{
    if (pos + 4 > len)
        return false;
    uint32_t code = 0;
    for (uint32_t h = 0; h < 4; h++) {
        const char hc = src[pos++];
        code <<= 4;
        if (hc >= '0' && hc <= '9')
            code |= static_cast<uint32_t>(hc - '0');
        else if (hc >= 'a' && hc <= 'f')
            code |= static_cast<uint32_t>(hc - 'a' + 10);
        else if (hc >= 'A' && hc <= 'F')
            code |= static_cast<uint32_t>(hc - 'A' + 10);
        else
            return false;
    }
    value = code;
    return true;
}

// String grammar: a JSON string whose decoded form must be one well-defined code
// point stream.  Literal content must be well-formed UTF-8 (no truncated
// sequences, no overlong spellings, no surrogates, nothing past U+10FFFF) and
// \u escapes must appear in valid surrogate pairs, so every key has exactly
// one decoded spelling and neither duplicate detection nor dispatch can be
// bypassed by re-spelling.
static bool string_content_ok(const char* s, uint32_t len)
{
    for (uint32_t i = 0; i < len;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        if (c < 0x20)
            return false; // raw control characters are not part of bank text

        if (c < 0x80) {
            if (c != '\\') {
                ++i;
                continue;
            }
            if (++i >= len)
                return false; // a trailing backslash never closes an escape
            const char esc = s[i++];
            switch (esc) {
                case '"':
                case '/':
                case '\\':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    break;
                case 'u': {
                    uint32_t code;
                    if (! read_hex4(s, len, i, code))
                        return false; // \u needs four hex digits
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (i + 6 > len || s[i] != '\\' || s[i + 1] != 'u')
                            return false; // a high surrogate must be followed by a low one
                        uint32_t low_pos = i + 2;
                        uint32_t low;
                        if (! read_hex4(s, len, low_pos, low) || low < 0xDC00 || low > 0xDFFF)
                            return false;
                        i += 6;
                    }
                    else if (code >= 0xDC00 && code <= 0xDFFF)
                        return false; // a lone low surrogate
                    break;
                }
                default:
                    return false;
            }
            continue;
        }

        // Literal multi-byte UTF-8: the lead selects the continuation count
        uint32_t need;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            cp   = c & 0x1F;
        }
        else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            cp   = c & 0x0F;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            cp   = c & 0x07;
        }
        else
            return false; // a lone continuation, an overlong lead or an out-of-range lead
        if (i + need + 1 > len)
            return false; // a truncated sequence never reaches its last byte
        for (uint32_t k = 1; k <= need; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false; // not a continuation byte
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false; // an overlong spelling, a surrogate, or a code point past U+10FFFF
        i += need + 1;
    }
    return true;
}

// One code point as UTF-8 (1..4 bytes); every code point the grammar accepts is
// encodable.
static void encode_utf8(uint32_t cp, char dst[4], uint32_t& out_len)
{
    if (cp < 0x80) {
        dst[0]  = static_cast<char>(cp);
        out_len = 1;
    }
    else if (cp < 0x800) {
        dst[0]  = static_cast<char>(0xC0 | (cp >> 6));
        dst[1]  = static_cast<char>(0x80 | (cp & 0x3F));
        out_len = 2;
    }
    else if (cp < 0x10000) {
        dst[0]  = static_cast<char>(0xE0 | (cp >> 12));
        dst[1]  = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        dst[2]  = static_cast<char>(0x80 | (cp & 0x3F));
        out_len = 3;
    }
    else {
        dst[0]  = static_cast<char>(0xF0 | (cp >> 18));
        dst[1]  = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        dst[2]  = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        dst[3]  = static_cast<char>(0x80 | (cp & 0x3F));
        out_len = 4;
    }
}

// Where the gap to the next sibling starts: jsmn ends strings on the closing quote
// and primitives on their terminator, but composites one past the closer.
static uint32_t token_gap_start(const jsmntok_t& t)
{
    return static_cast<uint32_t>(t.end) + (t.type == JSMN_STRING ? 1 : 0);
}

// Where the token's raw source begins: jsmn starts string tokens one past their
// opening quote; separator gaps reach up to that quote.
static uint32_t token_raw_start(const jsmntok_t& t)
{
    return static_cast<uint32_t>(t.start) - (t.type == JSMN_STRING ? 1 : 0);
}

// The span [from, to) must be whitespace only.
static bool gap_is_ws(const char* doc, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++)
        if (! is_ws(doc[i]))
            return false;
    return true;
}

// The span [from, to) must be exactly one separator character surrounded by
// whitespace.
static bool gap_is_sep(const char* doc, uint32_t from, uint32_t to, char sep)
{
    uint32_t i = from;
    while (i < to && is_ws(doc[i]))
        i++;
    if (i >= to || doc[i] != sep)
        return false;
    for (i++; i < to; i++)
        if (! is_ws(doc[i]))
            return false;
    return true;
}

// Decodes one \uXXXX escape (with surrogate pairs) into its code point.  The
// document-wide grammar pass validated the four hex digits and paired lone
// surrogates, so only the high/low pairing itself can fail here.
static bool escape_code_point(const char* src, uint32_t len, uint32_t& pos, int32_t& code)
{
    uint32_t high;
    if (! read_hex4(src, len, pos, high))
        return false;
    if (high >= 0xD800 && high <= 0xDBFF) {
        if (pos + 2 > len)
            return false;
        pos += 2; // skip the low escape's backslash and 'u'
        uint32_t low;
        if (! read_hex4(src, len, pos, low) || low < 0xDC00 || low > 0xDFFF)
            return false;
        code = static_cast<int32_t>(0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00));
    }
    else {
        code = static_cast<int32_t>(high);
    }
    return true;
}

// Unescapes JSON string content into dst (NUL-terminated).  Returns false when
// the text does not decode into bank text (a raw control character, a \u
// escape of zero, an invalid escape, a malformed literal UTF-8 sequence) or
// dst cannot hold the decoded bytes.  The document-wide grammar pass already
// validated escape spelling; this walks the same escape set.  Unicode escapes
// decode to the same UTF-8 bytes their literal spellings produce, so both
// spellings of a name are interchangeable and the re-encoded document stays
// valid JSON.
static bool unescape_string(const char* src, uint32_t len, char* dst, uint32_t dst_size)
{
    if (dst_size == 0)
        return false;

    uint32_t o = 0;
    uint32_t c = 0;
    while (c < len) {
        char ch = src[c++];
        if (ch == '\\') {
            if (c >= len)
                return false;
            const char esc = src[c++];
            switch (esc) {
                case '"':
                    ch = '"';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case '/':
                    ch = '/';
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u': {
                    int32_t code;
                    if (! escape_code_point(src, len, c, code))
                        return false;
                    if (code == 0)
                        return false; // a zero code point would truncate the bank text
                    char     utf8[4];
                    uint32_t utf8_len = 0;
                    encode_utf8(static_cast<uint32_t>(code), utf8, utf8_len);
                    if (o + utf8_len + 1 > dst_size)
                        return false;
                    memcpy(dst + o, utf8, utf8_len);
                    o += utf8_len;
                    continue;
                }
                default:
                    return false;
            }
        }
        else if (static_cast<unsigned char>(ch) < 0x20) {
            return false;
        }
        else if (static_cast<unsigned char>(ch) >= 0x80) {
            // Literal multi-byte sequences must be well-formed UTF-8: bank text is
            // re-encoded verbatim, so a malformed sequence would produce an
            // unencodable document.  Shortest form only, no surrogates, at most
            // U+10FFFF - the same rules the key path enforces.
            const unsigned char lead = static_cast<unsigned char>(ch);
            uint32_t            need = 0;
            if (lead >= 0xC2 && lead <= 0xDF)
                need = 1;
            else if (lead >= 0xE0 && lead <= 0xEF)
                need = 2;
            else if (lead >= 0xF0 && lead <= 0xF4)
                need = 3;
            else
                return false;
            if (c + need > len)
                return false;
            const unsigned char second = static_cast<unsigned char>(src[c]);
            const bool          second_ok =
                (need == 1 && second >= 0x80 && second <= 0xBF) ||
                (need == 2 && ((lead == 0xE0 && second >= 0xA0) ||
                               (lead != 0xE0 && second >= 0x80 && ! (lead == 0xED && second >= 0xA0)))) ||
                (need == 3 && ((lead == 0xF0 && second >= 0x90) || (lead == 0xF4 && second <= 0x8F) ||
                               (lead != 0xF0 && lead != 0xF4 && second >= 0x80)));
            if (! second_ok)
                return false;
            if (o + need + 2 > dst_size)
                return false;
            dst[o++] = ch;
            for (uint32_t k = 0; k < need; k++) {
                const unsigned char cc = static_cast<unsigned char>(src[c++]);
                if ((cc & 0xC0) != 0x80)
                    return false; // unreachable: the grammar pass validated it
                dst[o++] = static_cast<char>(cc);
            }
            continue;
        }
        if (o + 1 >= dst_size)
            return false;
        dst[o++] = ch;
    }
    dst[o] = 0;
    return true;
}

static KeySymbols key_symbols(const Walker& w, uint32_t key_idx)
{
    const jsmntok_t& t = w.toks[key_idx];
    return { w.doc + t.start, static_cast<uint32_t>(t.end - t.start), 0 };
}

// Dispatch comparison: schema strings compare by DECODED text, so escaped
// spellings of a name dispatch to that name - keys and enum values alike.
static bool key_eq(const Walker& w, uint32_t key_idx, const char* literal)
{
    if (w.toks[key_idx].type != JSMN_STRING)
        return false;

    KeySymbols key = key_symbols(w, key_idx);

    for (const char* p = literal;; p++) {
        const int sym = key.next();
        if (sym < 0)
            return *p == 0; // the key ended: equal only if the literal ended too
        if (*p == 0 || sym != static_cast<int>(static_cast<unsigned char>(*p)))
            return false;
    }
}

// Duplicate detection compares DECODED key streams, so spellings of one name
// count as the same key at any length - overflow can never weaken the check.
static bool keys_equal(const Walker& w, uint32_t a, uint32_t b)
{
    KeySymbols sa = key_symbols(w, a);
    KeySymbols sb = key_symbols(w, b);

    for (;;) {
        const int a_sym = sa.next();
        const int b_sym = sb.next();
        if (a_sym != b_sym)
            return false;
        if (a_sym < 0)
            return true; // both streams ended together
    }
}

// Validates one token's complete subtree: primitive and string grammar, child
// containment, separators between children, the closer after the last child, and
// key uniqueness in objects.  Returns the index one past the subtree, or
// invalid_index on any violation.
static uint32_t check_value(Walker& w, uint32_t i)
{
    if (i >= w.num_toks)
        return invalid_index;

    const jsmntok_t& t = w.toks[i];

    if (t.start < 0 || t.end < t.start || static_cast<uint32_t>(t.end) > w.len)
        return invalid_index;

    switch (t.type) {
        case JSMN_PRIMITIVE:
            if (! primitive_is_valid(w, i))
                return invalid_index;
            return i + 1;

        case JSMN_STRING:
            if (! string_content_ok(w.doc + t.start, static_cast<uint32_t>(t.end - t.start)))
                return invalid_index;
            return i + 1;

        case JSMN_ARRAY:
        case JSMN_OBJECT: {
            const bool is_object = t.type == JSMN_OBJECT;
            const char closer    = is_object ? '}' : ']';

            uint32_t  child    = i + 1;
            uint32_t  prev_pos = static_cast<uint32_t>(t.start) + 1; // just after the opener
            const int units    = t.size;                             // objects count pairs, arrays count elements

            for (int u = 0; u < units; u++) {
                if (child >= w.num_toks)
                    return invalid_index;
                const jsmntok_t& key_or_elem = w.toks[child];

                const uint32_t raw_start = token_raw_start(key_or_elem);

                if (raw_start < prev_pos)
                    return invalid_index;

                // Elements and pairs are separated by exactly one comma.
                if (u == 0 ? ! gap_is_ws(w.doc, prev_pos, raw_start) : ! gap_is_sep(w.doc, prev_pos, raw_start, ','))
                    return invalid_index;

                if (is_object) {
                    // A pair is a string key, one colon, then the value
                    if (key_or_elem.type != JSMN_STRING ||
                        ! string_content_ok(w.doc + key_or_elem.start,
                                            static_cast<uint32_t>(key_or_elem.end - key_or_elem.start)))
                        return invalid_index;
                    if (child + 1 >= w.num_toks)
                        return invalid_index;
                    if (! gap_is_sep(w.doc, token_gap_start(key_or_elem), token_raw_start(w.toks[child + 1]), ':'))
                        return invalid_index;
                    child++;
                }

                // The unit's own token index: a composite's gap starts after its closer,
                // not after its last descendant (which lies inside the composite).
                const uint32_t unit_idx = child;

                const uint32_t next = check_value(w, child);
                if (next == invalid_index)
                    return invalid_index;

                const uint32_t child_end = static_cast<uint32_t>(w.toks[next - 1].end);
                if (child_end > static_cast<uint32_t>(t.end))
                    return invalid_index; // a child reaching past its parent

                prev_pos = token_gap_start(w.toks[unit_idx]);
                child    = next;
            }

            // Only whitespace may precede the closer (this also rejects trailing commas)
            if (prev_pos > static_cast<uint32_t>(t.end) - 1 ||
                ! gap_is_ws(w.doc, prev_pos, static_cast<uint32_t>(t.end) - 1) || w.doc[t.end - 1] != closer)
                return invalid_index;

            if (is_object && units > 1) {
                // Duplicate keys are ambiguous (the schema would silently take the last
                // one), so the format rejects them.
                uint32_t num_keys = 0;
                uint32_t k        = i + 1;
                for (int u = 0; u < units; u++) {
                    if (k >= w.num_toks)
                        return invalid_index;
                    if (w.toks[k].type == JSMN_OBJECT) {
                        // An embedded continuation object is not a key; skip it whole.
                        k = skip_value(w, k);
                        if (k == invalid_index)
                            return invalid_index;
                        continue;
                    }
                    if (num_keys >= sizeof(object_keys) / sizeof(object_keys[0]) || w.toks[k].type != JSMN_STRING)
                        return invalid_index;
                    object_keys[num_keys++] = k;
                    k                       = skip_value(w, k + 1);
                    if (k == invalid_index)
                        return invalid_index;
                }
                for (uint32_t a = 0; a + 1 < num_keys; a++)
                    for (uint32_t b = a + 1; b < num_keys; b++)
                        if (keys_equal(w, object_keys[a], object_keys[b]))
                            return invalid_index;
            }

            return child;
        }

        default:
            return invalid_index;
    }
}

static char     ignored_names[8][48];
static uint32_t ignored_count = 0;

static void record_ignored(const char* name)
{
    if (ignored_count < 8)
        snprintf(ignored_names[ignored_count], 48, "%s", name);
    ++ignored_count;
}

static char     skipped_names[8][48];
static uint32_t skipped_count;

static void record_skipped(const char* what)
{
    record_ignored(what);
    if (skipped_count < 8)
        snprintf(skipped_names[skipped_count], 48, "%s", what);
    ++skipped_count;
}

static bool* damaged_entry;
static bool  damaged_instruments[Synth::max_instruments];
static bool  damaged_envelopes[Synth::max_envelopes];
static bool  damaged_lfos[Synth::max_lfos];
static bool  damaged_parameters[Synth::max_parameters];
static bool  damaged_channels[Synth::max_channels];
static bool  damaged_zones[Synth::max_channels][Synth::max_instr_per_channel];
static bool  damaged_effects[Synth::max_channels + 1][Synth::max_chain_effects];
static bool  damaged_layouts[Synth::max_graph_records];

static void mark_damage(const char* what)
{
    if (damaged_entry && *damaged_entry)
        record_ignored(what);
    else {
        record_skipped(what);
        if (damaged_entry)
            *damaged_entry = true;
    }
}

struct DamageScope {
    bool* previous;

    DamageScope(bool& flag) : previous(damaged_entry) { damaged_entry = &flag; }

    ~DamageScope() { damaged_entry = previous; }
};

static void record_lost(const char* what)
{
    mark_damage(what);
}

uint32_t Synth::take_skipped_entries(char (*names)[48], uint32_t capacity)
{
    const uint32_t count = skipped_count;
    for (uint32_t i = 0; i < count && i < capacity && i < 8; ++i)
        memcpy(names[i], skipped_names[i], 48);
    skipped_count = 0;
    return count;
}

uint32_t Synth::take_ignored_fields(char (*names)[48], uint32_t capacity)
{
    const uint32_t count = ignored_count;
    for (uint32_t i = 0; i < count && i < capacity && i < 8; ++i) {
        memcpy(names[i], ignored_names[i], 48);
    }
    ignored_count = 0;
    return count;
}

static void unknown_field(Walker& w, uint32_t key_idx)
{
    // The logged name is the key's decoded prefix, encoded as UTF-8 as far as the
    // bounded display buffer holds it.  A code point that no longer fits ends the
    // prefix, so the log always shows a faithful head of the decoded name,
    // whatever spelling the document used.
    char     display[48];
    uint32_t o = 0;

    KeySymbols key = key_symbols(w, key_idx);

    for (;;) {
        const int sym = key.next();
        if (sym < 0)
            break; // the key ended
        char     enc[4];
        uint32_t enc_len;
        encode_utf8(static_cast<uint32_t>(sym), enc, enc_len);
        if (o + enc_len >= sizeof(display))
            break; // the prefix ends where the next code point no longer fits
        memcpy(display + o, enc, enc_len);
        o += enc_len;
    }
    display[o] = 0;

    record_ignored(display);
    d_printf("Ignored unknown bank field: %s\n", display);
}

// Bounded unsigned-magnitude token walk shared by want_uint (full token slice)
// and want_int (sign-stripped slice): digit validation and per-step bound
// refusal.  Leading zeros are rejected by the document-wide grammar pass, so the
// digit walk plus the per-step bound check needs no separate overflow guard for
// the small bounds used here.  On failure the walker is flagged and the
// destination is left untouched.
static void want_bounded_magnitude(Walker& w, const char* s, uint32_t len, uint32_t max_value, uint32_t* out)
{
    uint32_t value = 0;
    for (uint32_t c = 0; c < len; c++) {
        if (s[c] < '0' || s[c] > '9') {
            w.failed = true;
            return;
        }
        value = value * 10u + static_cast<uint32_t>(s[c] - '0');
        if (value > max_value) {
            w.failed = true;
            return;
        }
    }
    *out = value;
}

// Validates the parsed double against [min, max] BEFORE the binary32
// narrowing, so spellings that would round into or out of the range are
// judged on their true value.  Unlike want_float, the encoder's
// finite-extrema round-trip property does not hold here; bounded slider
// bases never approach the float range.
static void want_bounded_real(Walker& w, uint32_t key_idx, double min, double max, float* out)
{
    const jsmntok_t& t = w.toks[key_idx];
    if (t.type != JSMN_PRIMITIVE) {
        w.failed = true;
        return;
    }
    const char* const s   = w.doc + t.start;
    const uint32_t    len = static_cast<uint32_t>(t.end - t.start);
    if (! number_grammar_ok(s, len)) {
        w.failed = true;
        return;
    }
    const double value = strtod(s, nullptr);
    if (value < min || value > max) {
        w.failed = true;
        return;
    }
    *out = static_cast<float>(value);
}

static void want_uint(Walker& w, uint32_t i, uint32_t max_value, uint32_t* out)
{
    const jsmntok_t& t = w.toks[i];
    if (t.type != JSMN_PRIMITIVE) {
        w.failed = true;
        return;
    }

    want_bounded_magnitude(w, w.doc + t.start, static_cast<uint32_t>(t.end - t.start), max_value, out);
}

// One-byte variant of want_uint: same grammar and bound refusal, storing
// through a one-byte pointer directly (no 32-bit staging local). On failure
// the destination is left untouched; the caller refuses the whole document
// via w.failed.
static void want_u8(Walker& w, uint32_t i, uint32_t max_value, uint8_t* out)
{
    uint32_t value = 0;
    want_uint(w, i, max_value, &value);
    if (! w.failed) {
        *out = static_cast<uint8_t>(value);
    }
}

// Bounded signed integer parsed straight from the token, before any binary32
// narrowing: the shared magnitude walk rejects '.', 'e'/'E' and any other
// non-digit spelling, and range-checks during accumulation.  On failure the
// destination is left untouched; the caller refuses the whole document via
// w.failed.
static void want_int(Walker& w, uint32_t i, int32_t min_value, int32_t max_value, int32_t* out)
{
    const jsmntok_t& t = w.toks[i];
    if (t.type != JSMN_PRIMITIVE) {
        w.failed = true;
        return;
    }

    const char* const s   = w.doc + t.start;
    const uint32_t    len = static_cast<uint32_t>(t.end - t.start);

    uint32_t pos      = 0;
    bool     negative = false;
    if (pos < len && s[pos] == '-') {
        negative = true;
        pos++;
    }
    if (pos == len) { // an empty token or a lone minus
        w.failed = true;
        return;
    }
    // An int32 magnitude has at most 10 digits; longer spellings would make
    // the digit walk's own per-step bound check wrap, so refuse them first.
    if (len - pos > 10) {
        w.failed = true;
        return;
    }

    uint32_t magnitude_limit;
    if (! negative) {
        magnitude_limit = static_cast<uint32_t>(max_value);
    }
    else if (min_value >= 0) {
        // A nonnegative minimum admits no negative spelling: a zero magnitude
        // limit rejects every negative token at the digit walk (only "-0"
        // decodes, as zero, for the caller's own range check to judge).
        magnitude_limit = 0;
    }
    else {
        // The minimum's magnitude as unsigned arithmetic: -(min_value + 1) + 1
        // stays inside int32 for every minimum, including INT32_MIN.
        magnitude_limit = static_cast<uint32_t>(-(min_value + 1)) + 1;
    }
    uint32_t magnitude = 0;
    want_bounded_magnitude(w, s + pos, len - pos, magnitude_limit, &magnitude);
    if (! w.failed) {
        // The negation goes through unsigned arithmetic so the INT32_MIN
        // magnitude stays defined.
        *out = negative ? static_cast<int32_t>(0u - magnitude) : static_cast<int32_t>(magnitude);
    }
}

// The dispersion section count is an integer 1..6 stored as a float; any non-integer spelling is refused.
static void want_dispersion_sections(Walker& w, uint32_t i, float* out)
{
    int32_t sections = 0;
    want_int(w, i, 1, 6, &sections);
    // want_int applies a nonnegative minimum only to negative spellings, so zero needs its own check.
    if (! w.failed && sections < 1) {
        w.failed = true;
    }
    if (! w.failed) {
        *out = static_cast<float>(sections);
    }
}

static void want_float(Walker& w, uint32_t i, float* out)
{
    const jsmntok_t& t = w.toks[i];
    if (t.type != JSMN_PRIMITIVE) {
        w.failed = true;
        return;
    }

    const char* const s   = w.doc + t.start;
    const uint32_t    len = static_cast<uint32_t>(t.end - t.start);
    if (! number_grammar_ok(s, len)) {
        w.failed = true;
        return;
    }

    // The grammar scan passed, so strtod consumes exactly the token slice.
    // Range checking happens after the binary32 conversion: the encoder's %.9g
    // spelling of the finite extrema (3.40282347e+38) sits just past the exact
    // float32 maximum as a double yet rounds back to the same finite float, and
    // a bank holding that value must round-trip.  Only a conversion that leaves
    // the finite float range (or the grammar-banned infinities/nans) fails.
    const double value     = strtod(s, nullptr);
    const float  converted = static_cast<float>(value);
    if (! std::isfinite(converted)) {
        w.failed = true;
        return;
    }
    *out = converted;
}

// Time-key values retain their seeded default and warn on invalid numeric content.
static void want_time_float(Walker& w, uint32_t key, uint32_t value, float* out)
{
    const auto& token = w.toks[value];
    if (token.type == JSMN_PRIMITIVE &&
        number_grammar_ok(w.doc + token.start, static_cast<uint32_t>(token.end - token.start))) {
        const float converted = static_cast<float>(strtod(w.doc + token.start, nullptr));
        if (std::isfinite(converted)) {
            *out = converted;
            return;
        }
    }
    unknown_field(w, key);
}

static void want_bool(Walker& w, uint32_t i, bool* out)
{
    if (w.toks[i].type == JSMN_PRIMITIVE && tok_text_eq(w, i, "true"))
        *out = true;
    else if (w.toks[i].type == JSMN_PRIMITIVE && tok_text_eq(w, i, "false"))
        *out = false;
    else
        w.failed = true;
}

// Reads one instrument release in seconds: parses the float and enforces the
// decoder's range policy (finite, 0..3600).  Both JSON forms share the policy.
static void want_release_seconds(Walker& w, uint32_t val_idx, float* value)
{
    float parsed = *value;
    want_float(w, val_idx, &parsed);
    if (! w.failed && (parsed < 0.0f || parsed > 3600.0f))
        w.failed = true;
    if (! w.failed)
        *value = parsed;
}

// Reads a STRING token into out (NUL-terminated), unescaping JSON escapes.  Rejects
// over-long text and content that cannot appear in bank text (control characters,
// a \u escape for zero - which would truncate the name - or malformed
// UTF-8).  Multi-byte UTF-8 and non-ASCII escapes are accepted.
static void want_text(Walker& w, uint32_t i, char* out, uint32_t out_size)
{
    const jsmntok_t& t = w.toks[i];
    if (t.type != JSMN_STRING || out_size == 0) {
        w.failed = true;
        return;
    }

    if (! unescape_string(w.doc + t.start, static_cast<uint32_t>(t.end - t.start), out, out_size)) {
        if (w.lenient)
            memset(out, 0, out_size);
        w.failed = true;
    }
}

static void want_enum(Walker& w, uint32_t i, const char* const* names, uint32_t num_names, uint32_t* out)
{
    if (w.toks[i].type != JSMN_STRING) {
        w.failed = true;
        return;
    }
    for (uint32_t v = 0; v < num_names; v++) {
        if (key_eq(w, i, names[v])) {
            *out = v;
            return;
        }
    }
    w.failed = true;
}

// Forward extensions of named routing leave the default, unlike understood
// numeric values whose range checks remain strict.
static void want_routing_name(Walker&            w,
                              uint32_t           key,
                              uint32_t           token,
                              const char* const* names,
                              uint32_t           count,
                              uint32_t*          out)
{
    for (uint32_t i = 0; i < count; ++i)
        if (tok_text_eq(w, token, names[i]) && w.toks[token].type == JSMN_STRING) {
            *out = i;
            return;
        }
    unknown_field(w, key);
    if (w.toks[token].type != JSMN_STRING)
        mark_damage("routing value, defaulted");
}

static bool unsigned_literal(Walker& w, uint32_t token, uint32_t maximum, uint32_t* out)
{
    const auto& t = w.toks[token];
    if (t.type != JSMN_PRIMITIVE || t.start == t.end)
        return false;
    uint32_t value = 0;
    for (int i = t.start; i < t.end; ++i) {
        const char ch = w.doc[i];
        if (ch < '0' || ch > '9')
            return false;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
            return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static uint16_t reference_id(Walker& w, uint32_t key, uint32_t token)
{
    uint32_t value = 0;
    if (! unsigned_literal(w, token, 0xFFFF, &value) || ! value) {
        unknown_field(w, key);
        mark_damage("invalid reference, defaulted");
        return 0;
    }
    return static_cast<uint16_t>(value);
}

static void normalize_bypass_routing(Sculptor::EffectChainRouting& routing)
{
    if (routing.in == Sculptor::effect_in_output)
        for (auto& feed : routing.out)
            if (feed == Sculptor::effect_out_output)
                feed = Sculptor::effect_out_disconnected;
}

// Reads the instrument voice mode.  Only the three spelled names are valid.
static void want_voice_mode(Walker& w, uint32_t i, uint8_t* out)
{
    uint32_t mode = 0;
    want_enum(w, i, voice_mode_names, num_voice_modes, &mode);
    if (! w.failed) {
        *out = static_cast<uint8_t>(mode);
    }
}

// Walks one object token, invoking handle(key_index, value_index) for every pair.
// The grammar pass guarantees well-formed, duplicate-free pairs; handle dispatches
// on keys.
template <typename F> static void walk_object(Walker& w, uint32_t obj, F handle)
{
    const jsmntok_t& t = w.toks[obj];
    if (t.type != JSMN_OBJECT) {
        w.failed = true;
        return;
    }

    uint32_t  idx   = obj + 1;
    const int pairs = t.size; // an object token counts its key-value pairs

    for (int p = 0; p < pairs && ! w.failed; p++) {
        const uint32_t key_idx = idx;
        if (key_idx >= w.num_toks || w.toks[key_idx].type != JSMN_STRING) {
            w.failed = true;
            return;
        }
        const uint32_t val_idx = idx + 1;
        idx                    = skip_value(w, val_idx);
        if (idx == invalid_index) {
            w.failed = true;
            return;
        }
        handle(key_idx, val_idx);
        if (w.failed && w.lenient && damaged_entry) {
            w.failed = false;
            unknown_field(w, key_idx);
            mark_damage("invalid entry field, defaulted");
        }
    }
}

// Walks one array token, invoking handle(element_index, element_position) for every
// element. Strict mode treats overflow as malformed; lenient named collections
// count each element beyond max_len as skipped. A missing tail keeps the default fill.
template <typename F> static void walk_array_impl(Walker& w, uint32_t arr, uint32_t max_len, const char* kind, F handle)
{
    const auto& token = w.toks[arr];
    if (token.type != JSMN_ARRAY || (static_cast<uint32_t>(token.size) > max_len && (! w.lenient || ! kind))) {
        w.failed = true;
        return;
    }
    uint32_t next = arr + 1;
    for (uint32_t e = 0; e < static_cast<uint32_t>(token.size) && ! w.failed; ++e) {
        const uint32_t element = next;
        next                   = skip_value(w, element);
        if (next == invalid_index) {
            w.failed = true;
            return;
        }
        if (e >= max_len) {
            char context[48];
            snprintf(context, sizeof(context), "%s %u, skipped", kind, e);
            record_skipped(context);
        }
        else
            handle(element, e);
    }
}

template <typename F> static void walk_array(Walker& w, uint32_t arr, uint32_t max_len, F handle)
{
    // Inner arrays belong to one entry: overflow invalidates that entry.
    walk_array_impl(w, arr, max_len, nullptr, handle);
}

template <typename F> static void walk_entries(Walker& w, uint32_t arr, uint32_t max_len, const char* kind, F handle)
{
    walk_array_impl(w, arr, max_len, kind, handle);
}

// Field walk shared by the bank and document decoders; unknown_field carries
// the warning-and-ignore policy for both formats, and source_set
// tells strict callers whether "source" was present.
static void decode_mod_input_fields(Walker&          w,
                                    uint32_t         obj,
                                    Synth::ModInput& in,
                                    bool*            source_set,
                                    void (*unknown)(Walker&, uint32_t),
                                    bool ms = false)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "source")) {
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &value);
            in.source   = static_cast<Synth::ModSource>(value);
            *source_set = true;
        }
        else if (key_eq(w, key_idx, "op")) {
            want_enum(w, val_idx, source_op_names, num_source_ops, &value);
            in.op = static_cast<Synth::SourceOp>(value);
        }
        else if (key_eq(w, key_idx, ms ? "scale_ms" : "scale")) {
            if (ms)
                want_time_float(w, key_idx, val_idx, &in.scale);
            else
                want_float(w, val_idx, &in.scale);
        }
        else if (key_eq(w, key_idx, "cc_number")) {
            // Controller numbers are 7-bit: a cc_number above 127 is refused at decode.
            want_u8(w, val_idx, 0x7F, &in.cc_number);
        }
        else
            unknown(w, key_idx);
    });
}

static void decode_mod_input(Walker& w, uint32_t obj, Synth::ModInput& in, bool ms = false)
{
    bool source_set; // unused: the bank format makes "source" optional
    decode_mod_input_fields(w, obj, in, &source_set, unknown_field, ms);
}

static void decode_gen(Walker& w, uint32_t obj, Synth::TargetBinding& gen, bool* touched)
{
    // Only caller passes &bank_touched[t]; no portable (nullable) path.
    *touched = true;
    // Absent key = the stored unscaled contribution; the pre-set survives
    // memset-zeroed decode targets.
    gen.envelope_depth = 1.0f;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "envelope_desc_id")) {
            gen.envelope_desc_id = reference_id(w, key_idx, val_idx);
        }
        else if (key_eq(w, key_idx, "envelope_depth"))
            want_float(w, val_idx, &gen.envelope_depth);
        else if (key_eq(w, key_idx, "lfo_desc_id")) {
            gen.lfo_desc_id = reference_id(w, key_idx, val_idx);
        }
        else if (key_eq(w, key_idx, "lfo_op")) {
            want_enum(w, val_idx, source_op_names, num_source_ops, &value);
            gen.lfo_op = static_cast<Synth::SourceOp>(value);
        }
        else if (key_eq(w, key_idx, "lfo_depth"))
            want_float(w, val_idx, &gen.lfo_depth);
        else if (key_eq(w, key_idx, "lfo_depth_source")) {
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &value);
            gen.lfo_depth_source = static_cast<Synth::ModSource>(value);
        }
        else if (key_eq(w, key_idx, "lfo_rate_source")) {
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &value);
            gen.lfo_rate_source = static_cast<Synth::ModSource>(value);
        }
        else if (key_eq(w, key_idx, "lfo_rate_scale_ms"))
            want_float(w, val_idx, &gen.lfo_rate_scale_ms);
        else
            unknown_field(w, key_idx);
    });
}

// The filter type decides the spread base bounds, but its key may follow the target
// blocks in the object.  This pre-pass reads it first.  Absent means band-pass.
// The main walk skips the key.
static Synth::FilterType prescan_fir_type(Walker& w, uint32_t obj)
{
    uint32_t type = Synth::fir_bandpass;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "fir_type")) {
            want_enum(w, val_idx, filter_type_names, num_filter_types, &type);
        }
    });

    return static_cast<Synth::FilterType>(type);
}

static void decode_routing(Walker&               w,
                           uint32_t              obj,
                           Synth::ModTarget      target,
                           Synth::FilterType     fir_type,
                           Synth::TargetBinding& routing,
                           bool*                 touched)
{
    if (touched) {
        *touched = true;
    }
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "base_value") || key_eq(w, key_idx, "base")) {
            if (target == Synth::mod_string_brightness || target == Synth::mod_string_strike) {
                w.failed = true;
                return;
            }
            // The spread base is slider-bounded (semitones or Hz by filter type),
            // validated before the binary32 narrowing; the FIR slope base is an
            // integer percent 1..100 like the layer scalar; the higher bands'
            // offset/spread/gain bases carry their osc_target_view bounds
            // (a zero gain base stays decodable as an inactive band); every
            // other target keeps the generic free-level parsing.
            if (target == Synth::mod_string_damping || target == Synth::mod_string_release_damping ||
                target == Synth::mod_string_dispersion)
                want_bounded_real(w, val_idx, 0.0, 1.0, &routing.base_value);
            else if (target == Synth::mod_fir_spread)
                want_bounded_real(w,
                                  val_idx,
                                  0.0,
                                  static_cast<double>(Sculptor::osc_target_view(target, fir_type).max_value),
                                  &routing.base_value);
            else if (target == Synth::mod_fir_band1_offset || target == Synth::mod_fir_band2_offset ||
                     target == Synth::mod_fir_band3_offset || target == Synth::mod_fir_band1_spread ||
                     target == Synth::mod_fir_band2_spread || target == Synth::mod_fir_band3_spread ||
                     target == Synth::mod_fir_band1_gain || target == Synth::mod_fir_band2_gain ||
                     target == Synth::mod_fir_band3_gain)
                want_bounded_real(w,
                                  val_idx,
                                  static_cast<double>(target == Synth::mod_fir_band1_gain ||
                                                              target == Synth::mod_fir_band2_gain ||
                                                              target == Synth::mod_fir_band3_gain
                                                          ? 0.0f
                                                          : Sculptor::osc_target_view(target, fir_type).min_value),
                                  static_cast<double>(Sculptor::osc_target_view(target, fir_type).max_value),
                                  &routing.base_value);
            else if (target == Synth::mod_fir_slope) {
                int32_t base_int = 0;
                want_int(w, val_idx, 1, 100, &base_int);
                // want_int rejects negative spellings and out-of-range
                // magnitudes outright; this explicit range check stays as
                // defense in depth.
                if (base_int < 1 || base_int > 100)
                    w.failed = true;
                else
                    routing.base_value = static_cast<float>(base_int);
            }
            else
                want_float(w, val_idx, &routing.base_value);
        }
        else if (key_eq(w, key_idx, "inputs"))
            walk_array(w, val_idx, Synth::max_mod_inputs, [&](uint32_t elem, uint32_t pos) {
                decode_mod_input(w, elem, routing.inputs[pos]);
                routing.num_inputs = static_cast<uint16_t>(pos + 1);
            });
        else
            unknown_field(w, key_idx);
    });
}

// The name-keyed routing dispatch shared by the saved-bank decoder and the
// document decoder's "routing" alias: each key names a target, an explicit
// fm_ratio block stays authoritative (monotonic tracking), and the target's
// routing block decodes straight into the caller's oscillator.  Both callers
// report and ignore unknown fields.
static void decode_routing_object(Walker&            w,
                                  uint32_t           val_idx,
                                  Synth::FilterType  fir_type,
                                  Synth::Oscillator& osc,
                                  bool&              fm_ratio_routed,
                                  bool&              slope_routed,
                                  bool*              touched_targets,
                                  void (*unknown_target)(Walker&, uint32_t))
{
    walk_object(w, val_idx, [&](uint32_t r_key, uint32_t r_val) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            if (key_eq(w, r_key, mod_target_names[t])) {
                if (t == static_cast<uint32_t>(Synth::mod_fm_ratio))
                    fm_ratio_routed = true;
                if (t == static_cast<uint32_t>(Synth::mod_fir_slope))
                    slope_routed = true;
                decode_routing(w,
                               r_val,
                               static_cast<Synth::ModTarget>(t),
                               fir_type,
                               osc.targets[t],
                               touched_targets ? &touched_targets[t] : nullptr);
                return;
            }
        }
        unknown_target(w, r_key);
    });
}

// Stored inheritance clamp shared by the bank and document decoders:
// exactly zero stays zero (the stored unset carrier value); anything
// else is clamped to the slider range.
static float clamp_stored_mod_ratio(float ratio)
{
    return ratio != 0.0f ? vmath::clamp(ratio, Synth::osc_mod_ratio_min, Synth::osc_mod_ratio_max) : 0.0f;
}

// Per-band absent-key defaults shared by the bank and portable document
// decoders: within the decoded band count an untouched gain decodes 1.0
// and bands 1-3 slopes decode 100, so a band added by count without
// stating every key still routes audibly. Keys that are present decode as
// stated, including an explicit zero gain (an inactive band).
static void apply_fir_band_absent_defaults(Synth::Oscillator&      osc,
                                           const bool*             touched_targets,
                                           const Synth::FilterType fir_type)
{
    for (uint32_t band = 0; band < 4; band++) {
        if (band >= osc.fir_band_count)
            break;
        const uint32_t gain_t = band == 0 ? static_cast<uint32_t>(Synth::mod_fir_band0_gain)
                                          : static_cast<uint32_t>(Synth::mod_fir_band1_offset) + 4u * (band - 1) + 2u;
        if (! touched_targets[gain_t]) {
            const Sculptor::OscTargetView gain_view =
                Sculptor::osc_target_view(static_cast<Synth::ModTarget>(gain_t), fir_type);
            osc.targets[gain_t].base_value = 1.0f * gain_view.bank_scale;
        }
        if (band > 0) {
            const uint32_t slope_t = static_cast<uint32_t>(Synth::mod_fir_band1_offset) + 4u * (band - 1) + 3u;
            if (! touched_targets[slope_t]) {
                const Sculptor::OscTargetView slope_view =
                    Sculptor::osc_target_view(static_cast<Synth::ModTarget>(slope_t), fir_type);
                osc.targets[slope_t].base_value = 100.0f * slope_view.bank_scale;
            }
        }
    }
}

static void repair_mod_input(Synth::ModInput& input, bool effect = false, bool enable = false)
{
    const bool valid = effect ? (enable ? Synth::is_valid_effect_mod_input(input) : Synth::is_valid_effect_input(input))
                              : (input.source == Synth::ModSource::cc
                                     ? Synth::cc_is_assignable(input.cc_number)
                                     : input.source <= Synth::ModSource::pressure_combine && ! input.cc_number);
    if (! valid || (enable && input.source != Synth::ModSource::none && input.source != Synth::ModSource::cc)) {
        input.source    = Synth::ModSource::none;
        input.cc_number = 0;
        mark_damage("modulation input source, defaulted");
    }
}

static bool effect_source_valid(Synth::ModSource source)
{
    return source == Synth::ModSource::none || source == Synth::ModSource::pitch_bend ||
           source == Synth::ModSource::mod_wheel || source == Synth::ModSource::channel_pressure;
}

static void repair_oscillator(Synth::Oscillator& osc)
{
    if (osc.osc_type[1] > Synth::WaveType::pad_wave) {
        osc.osc_type[1] = Synth::WaveType::no_wave;
        mark_damage("wave B, defaulted");
    }
    for (auto& binding : osc.targets) {
        if (binding.lfo_depth_source >= Synth::ModSource::cc) {
            binding.lfo_depth_source = Synth::ModSource::none;
            mark_damage("LFO depth source, defaulted");
        }
        if (binding.lfo_rate_source >= Synth::ModSource::cc) {
            binding.lfo_rate_source = Synth::ModSource::none;
            mark_damage("LFO rate source, defaulted");
        }
        for (uint32_t i = 0; i < binding.num_inputs; ++i)
            repair_mod_input(binding.inputs[i]);
    }
}

static void decode_oscillator(Walker& w, uint32_t obj, Synth::Oscillator& osc)
{
    bool     fm_ratio_routed                      = false;
    bool     slope_routed                         = false;
    uint32_t band_count                           = 1;
    bool     fir_bands_stated                     = false;
    bool     bank_touched[Synth::num_mod_targets] = {};
    int32_t  octave_offset                        = 0;
    int32_t  fir_slope                            = 100;
    float    pitch_offset                         = 0.0f;
    float    string_brightness                    = 1.0f; // absent means full brightness, not the zeroed bank memory

    float string_dispersion_sections = 4.0f;
    osc.string_drive                 = 3.0f;
    osc.string_spectrum              = 2.0f;
    if (w.lenient && w.toks[obj].type != JSMN_OBJECT) {
        osc.string_brightness                             = 1.0f;
        osc.string_dispersion_sections                    = 4.0f;
        osc.fir_slope_percent                             = 100;
        osc.fir_type                                      = Synth::fir_bandpass;
        osc.fir_band_count                                = 1;
        osc.targets[Synth::mod_fir_band0_gain].base_value = 1.0f;
        mark_damage("oscillator field, defaulted");
        return;
    }
    const Synth::FilterType fir_type = prescan_fir_type(w, obj);
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "wave_a")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
            osc.osc_type[0] = static_cast<Synth::WaveType>(value);
        }
        else if (key_eq(w, key_idx, "wave_b")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
            if (value > static_cast<uint32_t>(Synth::WaveType::pad_wave))
                w.failed = true; // string is wave A only
            osc.osc_type[1] = static_cast<Synth::WaveType>(value);
        }
        else if (key_eq(w, key_idx, "mode")) {
            want_enum(w, val_idx, osc_mode_names, num_osc_modes, &value);
            osc.osc_mode = static_cast<Synth::OscMode>(value);
        }
        else if (key_eq(w, key_idx, "string_brightness"))
            want_bounded_real(w, val_idx, 0.0, 1.0, &string_brightness);
        else if (key_eq(w, key_idx, "string_dispersion") && w.toks[val_idx].type != JSMN_OBJECT)
            want_bounded_real(w, val_idx, 0.0, 1.0, &osc.targets[Synth::mod_string_dispersion].base_value);
        else if (key_eq(w, key_idx, "string_strike"))
            want_bounded_real(w, val_idx, 0.0, 1.0, &osc.string_strike);
        else if (key_eq(w, key_idx, "string_drive"))
            want_bounded_real(w, val_idx, 0.5, 10.0, &osc.string_drive);
        else if (key_eq(w, key_idx, "string_spectrum"))
            want_bounded_real(w, val_idx, 1.5, 4.0, &osc.string_spectrum);
        else if (key_eq(w, key_idx, "string_dispersion_sections"))
            want_dispersion_sections(w, val_idx, &string_dispersion_sections);
        else if (key_eq(w, key_idx, "mod_ratio"))
            want_float(w, val_idx, &osc.mod_ratio);
        else if (key_eq(w, key_idx, "pitch_offset"))
            want_float(w, val_idx, &pitch_offset);
        else if (key_eq(w, key_idx, "octave_offset"))
            want_int(w, val_idx, Sculptor::osc_octave_offset_min, Sculptor::osc_octave_offset_max, &octave_offset);
        else if (key_eq(w, key_idx, "fir_slope"))
            want_int(w, val_idx, 1, 100, &fir_slope);
        else if (key_eq(w, key_idx, "fir_type")) {
            // Read by prescan_fir_type before the routing blocks.
        }
        else if (key_eq(w, key_idx, "generators"))
            walk_object(w, val_idx, [&](uint32_t gen_key, uint32_t gen_val) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                    if (key_eq(w, gen_key, mod_target_names[t])) {
                        decode_gen(w, gen_val, osc.targets[t], &bank_touched[t]);
                        return;
                    }
                }
                unknown_field(w, gen_key);
            });
        else if (key_eq(w, key_idx, "routing"))
            // This layer's own routing decodes straight into its bindings; a
            // target the object omits keeps the zeroed binding (fm_ratio is
            // backfilled from the scalar below when no block names it).
            decode_routing_object(w,
                                  val_idx,
                                  fir_type,
                                  osc,
                                  fm_ratio_routed,
                                  slope_routed,
                                  bank_touched,
                                  unknown_field);
        else if (key_eq(w, key_idx, "fir_bands")) {
            // Multi-band bank documents state the active band count; band keys
            // beyond the count decode but stay dormant (the count gates the runtime
            // and the re-encode skips them).
            want_uint(w, val_idx, 4, &band_count);
            if (band_count < 1) {
                w.failed = true;
            }
            fir_bands_stated = true;
        }
        else {
            uint32_t t = 0;
            while (t < Synth::num_mod_targets && ! key_eq(w, key_idx, mod_target_names[t])) {
                t++;
            }
            if (t < Synth::num_mod_targets) {
                if (t == static_cast<uint32_t>(Synth::mod_fm_ratio)) {
                    fm_ratio_routed = true;
                }
                if (t == static_cast<uint32_t>(Synth::mod_fir_slope)) {
                    slope_routed = true;
                }
                decode_routing(w,
                               val_idx,
                               static_cast<Synth::ModTarget>(t),
                               fir_type,
                               osc.targets[t],
                               &bank_touched[t]);
            }
            else {
                unknown_field(w, key_idx);
            }
        }
    });

    if (w.failed)
        return;

    osc.octave_offset = static_cast<int8_t>(octave_offset);
    // want_int rejects negative spellings and out-of-range magnitudes
    // outright; this explicit range check stays as defense in depth.
    if (fir_slope < 1 || fir_slope > 100) {
        if (w.lenient) {
            fir_slope = 100;
            mark_damage("FIR slope, defaulted");
        }
        else
            w.failed = true;
    }
    if (w.lenient && band_count < 1) {
        band_count = 1;
        mark_damage("FIR bands, defaulted");
    }
    osc.fir_slope_percent = static_cast<uint8_t>(fir_slope);
    osc.string_brightness = string_brightness;

    osc.string_dispersion_sections = string_dispersion_sections;
    osc.fir_type                   = fir_type;
    osc.fir_band_count             = static_cast<uint8_t>(band_count);
    // Per-band absent-key defaults, mirroring the portable document
    // decoder, apply to a stored single-band bandpass bank (no fir_bands,
    // no band keys) too. Band 0's offset decodes the neutral 0.
    // Non-bandpass layers keep the stored zeroed gain binding: the gain row
    // means nothing there, and a backfilled key would change the re-encoded
    // bytes of every pre-existing document.
    if (fir_type == Synth::fir_bandpass)
        apply_fir_band_absent_defaults(osc, bank_touched, fir_type);
    // Multi-band banks treat band 0's slope as a real band parameter: when the
    // count key is stated and no block names fir_slope, the unrouted binding's
    // base seeds from the scalar so the band row and the compiled base agree
    // (stored documents without the count key keep the zeroed binding, the
    // M1 scalar convention).
    if (fir_bands_stated && ! slope_routed) {
        osc.targets[Synth::mod_fir_slope].base_value = static_cast<float>(fir_slope);
    }

    // Stored pitch_offset folds into the pitch target's base value; the
    // combined static trim must stay inside the pitch editing range.
    if (! w.failed &&
        (pitch_offset < Sculptor::osc_pitch_offset_min || pitch_offset > Sculptor::osc_pitch_offset_max)) {
        if (w.lenient) {
            pitch_offset = 0.0f;
            mark_damage("pitch offset, defaulted");
        }
        else
            w.failed = true;
    }
    if (! w.failed) {
        const Sculptor::OscTargetView pitch_view = Sculptor::osc_target_view(Synth::mod_pitch, fir_type);
        osc.targets[Synth::mod_pitch].base_value = vmath::clamp(osc.targets[Synth::mod_pitch].base_value + pitch_offset,
                                                                pitch_view.min_value,
                                                                pitch_view.max_value);
    }

    // Stored banks state only the scalar mod_ratio; when no routing block
    // names fm_ratio the target inherits it, clamped to the slider range
    // when nonzero (0 stays 0).
    if (! fm_ratio_routed) {
        osc.targets[Synth::mod_fm_ratio].base_value = clamp_stored_mod_ratio(osc.mod_ratio);
    }
    if (w.lenient)
        repair_oscillator(osc);
}

static void decode_instrument(Walker& w, uint32_t obj, Synth::Instrument& instr)
{
    uint32_t layer_count = 0;
    uint32_t num_layers  = 0;
    bool     have_count  = false;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "id") || key_eq(w, key_idx, "name"))
            return;
        if (key_eq(w, key_idx, "layer_count")) {
            want_uint(w, val_idx, Synth::max_layers, &layer_count);
            have_count = true;
        }
        else if (key_eq(w, key_idx, "layers")) {
            walk_array(w, val_idx, Synth::max_layers, [&](uint32_t elem, uint32_t pos) {
                decode_oscillator(w, elem, instr.layers[pos]);
                num_layers = pos + 1;
            });
        }
        else if (key_eq(w, key_idx, "note_skew_semitones"))
            want_float(w, val_idx, &instr.note_skew_semitones);
        else if (key_eq(w, key_idx, "layer_skew_semitones"))
            want_float(w, val_idx, &instr.layer_skew_semitones);
        else if (key_eq(w, key_idx, "release_seconds")) {
            want_release_seconds(w, val_idx, &instr.release_seconds);
        }
        else if (key_eq(w, key_idx, "pitch_bend_lock")) {
            bool lock = true;
            want_bool(w, val_idx, &lock);
            instr.pitch_bend_lock = lock ? 1 : 0;
        }
        else if (key_eq(w, key_idx, "voices")) {
            want_voice_mode(w, val_idx, &instr.voice_mode);
        }
        else
            unknown_field(w, key_idx);
    });

    if (w.failed)
        return;

    // The array defines the layers.  An old count can be stale, so warn on a
    // mismatch and keep loading the layers that are present.
    if (have_count && layer_count != num_layers)
        record_ignored("layer_count mismatch");
    instr.layer_count = num_layers;
}

// Decodes one envelope flag key (the value must be 0 or 1) and merges its bit
// into flags when nonzero; a failed decode merges nothing.
static void decode_envelope_flag(Walker& w, uint32_t val_idx, uint8_t flag_mask, uint8_t* flags)
{
    uint8_t flag = 0;
    want_u8(w, val_idx, 1, &flag);
    if (flag) {
        *flags |= flag_mask;
    }
}

static bool envelope_member(Walker&                    w,
                            uint32_t                   key_idx,
                            uint32_t                   val_idx,
                            Synth::EnvelopeDescriptor& env,
                            EnvelopeFields&            fields)
{

    if (key_eq(w, key_idx, "num_points")) {
        want_uint(w, val_idx, Synth::max_envelope_points, &fields.num_points);
        fields.have_count = true;
        return true;
    }
    if (key_eq(w, key_idx, "sustain_first")) {
        want_u8(w, val_idx, Synth::max_envelope_points - 1, &env.sustain_first_point);
        return true;
    }
    if (key_eq(w, key_idx, "sustain_last")) {
        want_u8(w, val_idx, Synth::max_envelope_points - 1, &env.sustain_last_point);
        return true;
    }
    if (key_eq(w, key_idx, "ping_pong")) {
        // The envelope starts zero-initialized, so the bits merge into flags.
        decode_envelope_flag(w, val_idx, Synth::envelope_ping_pong, &env.flags);
        return true;
    }
    if (key_eq(w, key_idx, "quick_release")) {
        decode_envelope_flag(w, val_idx, Synth::envelope_quick_release, &env.flags);
        return true;
    }
    if (key_eq(w, key_idx, "min_value")) {
        want_float(w, val_idx, &env.min_value);
        return true;
    }
    if (key_eq(w, key_idx, "min_max_delta")) {
        want_float(w, val_idx, &env.min_max_delta);
        return true;
    }
    if (key_eq(w, key_idx, "points")) {
        fields.have_points = true;
        walk_array(w, val_idx, Synth::max_envelope_points, [&](uint32_t elem, uint32_t pos) {
            Synth::EnvelopeDescriptor::Point& point = env.points[pos];
            walk_object(w, elem, [&](uint32_t p_key, uint32_t p_val) {
                uint32_t num = 0;
                if (key_eq(w, p_key, "position")) {
                    want_uint(w, p_val, 0xFFFF, &num);
                    point.position = static_cast<uint16_t>(num);
                }
                else if (key_eq(w, p_key, "value")) {
                    want_uint(w, p_val, 0xFFFF, &num);
                    point.value = static_cast<uint16_t>(num);
                }
                else
                    unknown_field(w, p_key);
            });
            fields.num_point_entries = pos + 1;
        });
        return true;
    }

    return false;
}

static void envelope_finish(Walker& w, Synth::EnvelopeDescriptor& env, const EnvelopeFields& fields)
{
    if (w.failed)
        return;

    if (fields.have_count && fields.have_points && fields.num_points != fields.num_point_entries) {
        if (! w.lenient) {
            w.failed = true;
            return;
        }
        mark_damage("envelope point count, defaulted");
    }

    env.num_points = static_cast<uint8_t>(fields.have_points ? fields.num_point_entries : fields.num_points);

    if (! fields.have_points) {
        // Default contour for a point count given without points: one tick per point.
        for (uint32_t p = 0; p < env.num_points; p++) {
            env.points[p].position = static_cast<uint16_t>(p);
            env.points[p].value    = 0;
        }
    }
}

static void decode_envelope(Walker& w, uint32_t obj, Synth::EnvelopeDescriptor& env)
{
    EnvelopeFields fields;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "id"))
            return;
        if (! envelope_member(w, key_idx, val_idx, env, fields))
            unknown_field(w, key_idx);
    });
    envelope_finish(w, env, fields);

    // With ping-pong off, a multi-point span loops seamlessly by
    // construction: the span's last point value is forced to the first
    // point's value.  An unset span stays a single-point span (defaults 0),
    // so nothing is forced there.
    if ((env.flags & Synth::envelope_ping_pong) == 0 && env.sustain_last_point > env.sustain_first_point) {
        env.points[env.sustain_last_point].value = env.points[env.sustain_first_point].value;
    }
}

static void decode_lfo(Walker& w, uint32_t obj, Synth::LFODescriptor& lfo)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "id"))
            return;
        if (key_eq(w, key_idx, "wave")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
            if (value > static_cast<uint32_t>(Synth::WaveType::noise_wave))
                w.failed = true; // LFO decode accepts waves only up to noise_wave
            lfo.wave = static_cast<Synth::WaveType>(value);
        }
        else if (key_eq(w, key_idx, "duty")) {
            want_uint(w, val_idx, 0xFF, &value);
            lfo.duty = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "period_ms")) {
            want_uint(w, val_idx, 0xFFFF, &value);
            lfo.period_ms = static_cast<uint16_t>(value);
        }
        else if (key_eq(w, key_idx, "min_value"))
            want_float(w, val_idx, &lfo.min_value);
        else if (key_eq(w, key_idx, "min_max_delta"))
            want_float(w, val_idx, &lfo.min_max_delta);
        else if (key_eq(w, key_idx, "pulse_mix")) {
            // Garbage-tolerant like its siblings: accept a wider bound, then
            // clamp instead of failing the whole decode.
            want_uint(w, val_idx, 0xFFFF, &value);
            lfo.pulse_mix = static_cast<uint8_t>(value > 255 ? 255 : value);
        }
        else if (key_eq(w, key_idx, "beat_div")) {
            want_uint(w, val_idx, 0xFF, &value);
            lfo.beat_div = (value <= 5) ? static_cast<uint8_t>(value) : 0;
        }
        else if (key_eq(w, key_idx, "start_point")) {
            want_uint(w, val_idx, 0xFF, &value);
            lfo.start_point = (value <= 2) ? static_cast<uint8_t>(value) : 0;
        }
        else
            unknown_field(w, key_idx);
    });
}

static void decode_param(Walker& w, uint32_t obj, Synth::ParamDescriptor& param)
{
    // "kind" selects the meaning of the remaining fields and may appear anywhere in
    // the object, so the entry is walked twice: once for the kind, once for the rest.
    uint32_t kind      = 0;
    bool     have_kind = false;

    bool* saved_damage = damaged_entry;
    damaged_entry      = nullptr;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "kind")) {
            want_enum(w, val_idx, param_kind_names, num_param_kinds, &kind);
            have_kind = true;
        }
    });

    damaged_entry = saved_damage;
    if (w.failed || ! have_kind) {
        w.failed = true;
        return;
    }
    param.kind = static_cast<Synth::ParamKind>(kind);

    EnvelopeFields fields;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "kind") || key_eq(w, key_idx, "id"))
            return; // consumed by the first pass

        switch (param.kind) {
            case Synth::ParamKind::external:
                unknown_field(w, key_idx); // an external parameter carries no fields
                break;

            case Synth::ParamKind::envelope:
                if (! envelope_member(w, key_idx, val_idx, param.envelope, fields))
                    unknown_field(w, key_idx);
                break;

            case Synth::ParamKind::lfo: {
                uint32_t value = 0;
                if (key_eq(w, key_idx, "lfo"))
                    decode_lfo(w, val_idx, param.lfo.lfo);
                else if (key_eq(w, key_idx, "op")) {
                    want_enum(w, val_idx, source_op_names, num_source_ops, &value);
                    param.lfo.op = static_cast<Synth::SourceOp>(value);
                }
                else if (key_eq(w, key_idx, "depth"))
                    want_float(w, val_idx, &param.lfo.depth);
                else if (key_eq(w, key_idx, "depth_param_id")) {
                    param.lfo.depth_param_id = reference_id(w, key_idx, val_idx);
                }
                else if (key_eq(w, key_idx, "rate_param_id")) {
                    param.lfo.rate_param_id = reference_id(w, key_idx, val_idx);
                }
                else if (key_eq(w, key_idx, "rate_scale_ms"))
                    want_float(w, val_idx, &param.lfo.rate_scale_ms);
                else
                    unknown_field(w, key_idx);
                break;
            }

            case Synth::ParamKind::plain: {
                uint32_t value = 0;
                if (key_eq(w, key_idx, "base_value"))
                    want_float(w, val_idx, &param.plain.base_value);
                else if (key_eq(w, key_idx, "sources"))
                    walk_array(w, val_idx, Synth::max_param_sources, [&](uint32_t elem, uint32_t pos) {
                        Synth::SourceParam& src = param.plain.sources[pos];
                        walk_object(w, elem, [&](uint32_t s_key, uint32_t s_val) {
                            if (key_eq(w, s_key, "param_id")) {
                                src.param_id = reference_id(w, s_key, s_val);
                            }
                            else if (key_eq(w, s_key, "scale"))
                                want_float(w, s_val, &src.scale);
                            else if (key_eq(w, s_key, "op")) {
                                want_enum(w, s_val, source_op_names, num_source_ops, &value);
                                src.op = static_cast<Synth::SourceOp>(value);
                            }
                            else
                                unknown_field(w, s_key);
                        });
                        param.plain.num_sources = static_cast<uint16_t>(pos + 1);
                    });
                else
                    unknown_field(w, key_idx);
                break;
            }
        }
    });

    if (param.kind == Synth::ParamKind::envelope)
        envelope_finish(w, param.envelope, fields);
}

static void decode_binding(Walker&                    w,
                           uint32_t                   obj,
                           Synth::EffectParamBinding& binding,
                           Synth::EffectType          type  = Synth::EffectType::none,
                           uint32_t                   index = 0)
{
    const bool ms = Synth::effect_param_is_ms(type, index);
    if (ms)
        binding.base_value = Sculptor::effect_param_default(type, index);
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, ms ? "base_value_ms" : "base_value")) {
            if (ms)
                want_time_float(w, key_idx, val_idx, &binding.base_value);
            else
                want_float(w, val_idx, &binding.base_value);
        }
        else if (key_eq(w, key_idx, "lfo_desc_id")) {
            binding.lfo_desc_id = reference_id(w, key_idx, val_idx);
        }
        else if (key_eq(w, key_idx, "lfo_op")) {
            want_enum(w, val_idx, source_op_names, num_source_ops, &value);
            binding.lfo_op = static_cast<Synth::SourceOp>(value);
        }
        else if (key_eq(w, key_idx, ms ? "lfo_depth_ms" : "lfo_depth")) {
            if (ms)
                want_time_float(w, key_idx, val_idx, &binding.lfo_depth);
            else
                want_float(w, val_idx, &binding.lfo_depth);
        }
        else if (key_eq(w, key_idx, "lfo_depth_source")) {
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &value);
            binding.lfo_depth_source = static_cast<Synth::ModSource>(value);
        }
        else if (key_eq(w, key_idx, "lfo_rate_source")) {
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &value);
            binding.lfo_rate_source = static_cast<Synth::ModSource>(value);
        }
        else if (key_eq(w, key_idx, "lfo_rate_scale"))
            want_float(w, val_idx, &binding.lfo_rate_scale);
        else if (key_eq(w, key_idx, "inputs"))
            walk_array(w, val_idx, Synth::max_mod_inputs, [&](uint32_t elem, uint32_t pos) {
                decode_mod_input(w, elem, binding.inputs[pos], ms);
                binding.num_inputs = static_cast<uint16_t>(pos + 1);
            });
        else
            unknown_field(w, key_idx);
    });
    if (ms) {
        const float max_value = type == Synth::EffectType::delay ? 1000.0f : 50.0f;
        const float old       = binding.base_value;
        binding.base_value =
            std::isfinite(old) ? fmaxf(0.0f, fminf(old, max_value)) : Sculptor::effect_param_default(type, index);
        if (old != binding.base_value)
            record_ignored("clamped base_value_ms");
    }
}

// Pre-narrowing FIR slope validation: the binding walk narrows
// base_value through want_float's permissive grammar, so fractional spellings
// such as 50.5 or 1e2 would otherwise reach the percent field.   Element 2 of
// either FIR type's shared stored params array (the schema's  slope_binding)
// is re-read with want_int's integer grammar; an
// omitted base_value defaults to 100 on the plain FIR (its live default),  while
// the multiband keeps the decoded value: its zeroed stored block is the
// fresh slot's normal state, so injecting the default would break the
// byte-identical save/load round trip.
static void decode_fir_slope(Walker& w, uint32_t binding_token, Synth::EffectSlotBinding& slot)
{
    bool have_base = false;
    walk_object(w, binding_token, [&](uint32_t key_idx, uint32_t val_idx) {
        if (! key_eq(w, key_idx, "base_value"))
            return;
        have_base     = true;
        int32_t slope = 0;
        want_int(w, val_idx, 1, 100, &slope);
        if (! w.failed && (slope < 1 || slope > 100))
            w.failed = true;
        if (! w.failed)
            slot.bindings[2].base_value = static_cast<float>(slope);
    });
    if (! w.failed && ! have_base && slot.type == Synth::EffectType::fir)
        slot.bindings[2].base_value = 100.0f;
}

// Prescans the effect's type so the FIR band keys decode in any key order, mirroring
// the oscillator's fir_type prescan.  An absent type reads as none, so band keys on a
// type-less object are reported and ignored like any unknown field.
static Synth::EffectType prescan_effect_type(Walker& w, uint32_t obj)
{
    uint32_t type = 0;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "type"))
            want_enum(w, val_idx, effect_type_names, Synth::num_effect_types, &type);
    });
    return static_cast<Synth::EffectType>(type);
}

// One FIR effect band object: two edge bindings (lowpass/highpass on the plain FIR,
// center/spread on the multiband type), gain and slope bindings.  The slope
// base re-reads with the integer grammar like the stored FIR Slope row (decode_fir_slope);
// absent keys decode the neutral defaults, so an unconfigured band stays inactive (its
// edges read 0) and its gain reads the neutral 1.0.
static void decode_fir_effect_band(Walker&                    w,
                                   uint32_t                   obj,
                                   Synth::EffectParamBinding* band,
                                   const char* const*         edge_keys)
{
    uint32_t slope_token     = 0;
    bool     have_gain       = false;
    bool     have_slope_base = false;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, edge_keys[0]))
            decode_binding(w, val_idx, band[0]);
        else if (key_eq(w, key_idx, edge_keys[1]))
            decode_binding(w, val_idx, band[1]);
        else if (key_eq(w, key_idx, "gain")) {
            have_gain = true;
            decode_binding(w, val_idx, band[2]);
        }
        else if (key_eq(w, key_idx, "slope"))
            slope_token = val_idx;
        else
            unknown_field(w, key_idx);
    });
    if (w.failed)
        return;
    if (slope_token) {
        decode_binding(w, slope_token, band[3]);
        if (w.failed)
            return;
        walk_object(w, slope_token, [&](uint32_t key_idx, uint32_t val_idx) {
            if (key_eq(w, key_idx, "base_value")) {
                have_slope_base = true;
                int32_t slope   = 0;
                want_int(w, val_idx, 1, 100, &slope);
                // want_int rejects negative spellings and out-of-range magnitudes
                // outright; this explicit range check stays as defense in depth.
                if (! w.failed && (slope < 1 || slope > 100))
                    w.failed = true;
                if (! w.failed)
                    band[3].base_value = static_cast<float>(slope);
            }
        });
        if (w.failed)
            return;
    }
    if (! have_gain)
        band[2].base_value = 1.0f;
    if (! have_slope_base)
        band[3].base_value = 100.0f;
}

// An absent band object decodes the neutral defaults: the zeroed edge pair keeps
// the band inactive and the gain/slope read their neutral 1.0 / 100.
static void decode_fir_band_neutral(Synth::EffectParamBinding* band)
{
    band[2].base_value = 1.0f;
    band[3].base_value = 100.0f;
}

// A band key beyond the declared count is reported and ignored.
// tokens[band - 1] and key_ids[band - 1] hold the token and key of band 1..max-1.
static void report_out_of_count_bands(Walker&         w,
                                      const uint32_t* tokens,
                                      const uint32_t* key_ids,
                                      uint32_t        band_count,
                                      bool            have_count)
{
    for (uint32_t band = 1; band < Synth::max_fir_effect_bands; ++band) {
        if (tokens[band - 1] && (! have_count || band >= band_count)) {
            unknown_field(w, key_ids[band - 1]);
        }
    }
}

static void decode_effect(Walker& w, uint32_t obj, Synth::EffectSlotBinding& slot, uint8_t* feed = nullptr)
{
    bool     params_seen[Synth::max_effect_param_floats] = {};
    uint32_t slope_token                                 = 0;
    bool     have_fir_bands                              = false;
    uint32_t fir_band_count =
        1; // absent fir_bands decodes as the single band (the plain FIR's stored form, the multiband's normal shape)
    uint32_t fir_band0_gain_token = 0;
    uint32_t fir_band_tokens[3]   = { 0, 0, 0 };
    uint32_t fir_band_key_ids[3]  = { 0, 0, 0 }; // each token's key, for the out-of-count ignored-field log
    uint32_t fir_band0_gain_key   = 0;
    uint32_t multiband_band_tokens[Synth::max_fir_effect_bands]  = { 0, 0, 0, 0 };
    uint32_t multiband_band_key_ids[Synth::max_fir_effect_bands] = { 0, 0, 0, 0 }; // keys, for the ignored-field log
    // Prescan: band keys decode in any key order, mirroring the oscillator's fir_type
    // prescan.
    const Synth::EffectType prescanned_type  = prescan_effect_type(w, obj);
    const bool              is_fir_effect    = prescanned_type == Synth::EffectType::fir;
    const bool              is_fir_multiband = prescanned_type == Synth::EffectType::fir_multiband;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "type")) {
            want_enum(w, val_idx, effect_type_names, Synth::num_effect_types, &value);
            slot.type = static_cast<Synth::EffectType>(value);
        }
        else if (key_eq(w, key_idx, "enabled")) {
            bool enabled = false;
            want_bool(w, val_idx, &enabled);
            slot.enabled = enabled;
        }
        else if (key_eq(w, key_idx, "feeds")) {
            if (feed) {
                want_routing_name(w, key_idx, val_idx, effect_feed_names, 3, &value);
                *feed = static_cast<uint8_t>(value);
            }
            else
                unknown_field(w, key_idx);
        }
        else if (key_eq(w, key_idx, "enable_input")) {
            decode_mod_input(w, val_idx, slot.enable_input);
        }
        else if (key_eq(w, key_idx, "params")) {
            if (w.toks[val_idx].type != JSMN_OBJECT) {
                mark_damage("effect params, defaulted");
                return;
            }
            walk_object(w, val_idx, [&](uint32_t param_key, uint32_t binding_token) {
                const uint32_t count = Synth::get_effect_param_floats(prescanned_type);
                for (uint32_t param = 0; param < count; ++param) {
                    if (! key_eq(w, param_key, effect_param_keys[static_cast<uint32_t>(prescanned_type)][param]))
                        continue;
                    if (w.toks[binding_token].type != JSMN_OBJECT) {
                        mark_damage("effect parameter, defaulted");
                        return;
                    }
                    params_seen[param] = true;
                    decode_binding(w, binding_token, slot.bindings[param], prescanned_type, param);
                    if (param == 2)
                        slope_token = binding_token;
                    return;
                }
                unknown_field(w, param_key);
            });
        }
        else if (key_eq(w, key_idx, "fir_bands")) {
            if (is_fir_effect || is_fir_multiband) {
                have_fir_bands = true;
                want_uint(w, val_idx, Synth::max_fir_effect_bands, &fir_band_count);
                if (! w.failed && fir_band_count < 1)
                    w.failed = true;
            }
            else
                unknown_field(w, key_idx);
        }
        else if (key_eq(w, key_idx, "fir_band0_gain")) {
            if (is_fir_effect) {
                fir_band0_gain_token = val_idx;
                fir_band0_gain_key   = key_idx;
            }
            else
                unknown_field(w, key_idx);
        }
        else if (is_fir_effect && key_eq(w, key_idx, fir_band_keys[1])) {
            fir_band_tokens[0]  = val_idx;
            fir_band_key_ids[0] = key_idx;
        }
        else if (is_fir_effect && key_eq(w, key_idx, fir_band_keys[2])) {
            fir_band_tokens[1]  = val_idx;
            fir_band_key_ids[1] = key_idx;
        }
        else if (is_fir_effect && key_eq(w, key_idx, fir_band_keys[3])) {
            fir_band_tokens[2]  = val_idx;
            fir_band_key_ids[2] = key_idx;
        }
        else if (is_fir_multiband && key_eq(w, key_idx, fir_band_keys[0])) {
            multiband_band_tokens[0]  = val_idx;
            multiband_band_key_ids[0] = key_idx;
        }
        else if (is_fir_multiband && key_eq(w, key_idx, fir_band_keys[1])) {
            multiband_band_tokens[1]  = val_idx;
            multiband_band_key_ids[1] = key_idx;
        }
        else if (is_fir_multiband && key_eq(w, key_idx, fir_band_keys[2])) {
            multiband_band_tokens[2]  = val_idx;
            multiband_band_key_ids[2] = key_idx;
        }
        else if (is_fir_multiband && key_eq(w, key_idx, fir_band_keys[3])) {
            multiband_band_tokens[3]  = val_idx;
            multiband_band_key_ids[3] = key_idx;
        }
        else
            unknown_field(w, key_idx);
    });

    if (w.failed)
        return;

    // Missing named parameters retain the type's audible defaults. Unknown
    // shapes and keys are skipped rather than reinterpreted positionally.
    const uint32_t expected_count = Synth::get_effect_param_floats(slot.type);
    for (uint32_t param = 0; ! w.failed && param < expected_count; ++param) {
        if (params_seen[param])
            continue;
        slot.bindings[param]            = Synth::EffectParamBinding{};
        slot.bindings[param].base_value = Sculptor::effect_param_default(slot.type, param);
    }
    if (! w.failed && slope_token && Synth::is_fir_effect_type(slot.type))
        decode_fir_slope(w, slope_token, slot);
    if (w.failed)
        return;
    if (w.lenient && ! fir_band_count) {
        fir_band_count = 1;
        mark_damage("effect band count, defaulted");
    }
    if (have_fir_bands)
        slot.fir_bands.band_count = static_cast<uint8_t>(fir_band_count);
    if (slot.type == Synth::EffectType::fir) {
        // Out-of-count band keys misstate the format: a band key beyond
        // the declared count, or any band key in the stored single-band
        // form, is reported and ignored instead of
        // decoding into a binding nothing stages.
        report_out_of_count_bands(w, fir_band_tokens, fir_band_key_ids, fir_band_count, have_fir_bands);
        if (w.failed)
            return;
        // Band 0's gain decodes once.  In the stored single-band form the key
        // must carry state: the zeroed stored block is not producible by the
        // encoder (it omits the key), so it refuses through the unknown-field
        // path, while an authored gain decodes into band 0 and re-encodes (the
        // count-1 encode rule).  A multi-band document (fir_bands stated)
        // decodes straight into band 0 and takes the neutral 1.0 when the key is
        // absent.  The stored form keeps the zeroed gain binding so
        // byte-comparable snapshots survive the round trip.
        if (fir_band0_gain_token && ! have_fir_bands) {
            Synth::EffectParamBinding gain0 = {};
            decode_binding(w, fir_band0_gain_token, gain0);
            if (! w.failed && ! gain0.lfo_desc_id && ! gain0.num_inputs && gain0.base_value == 0.0f) {
                unknown_field(w, fir_band0_gain_key);
                w.failed = true;
            }
            if (w.failed)
                return;
            slot.fir_bands.bands[0] = gain0;
        }
        else if (fir_band0_gain_token)
            decode_binding(w, fir_band0_gain_token, slot.fir_bands.bands[0]);
        else if (have_fir_bands)
            slot.fir_bands.bands[0].base_value = 1.0f;
        for (uint32_t band = 1; band < Synth::max_fir_effect_bands; band++) {
            Synth::EffectParamBinding* band_bindings = &slot.fir_bands.bands[1 + 4 * (band - 1)];
            if (band >= fir_band_count)
                break; // dormant bands stay zeroed
            if (fir_band_tokens[band - 1])
                decode_fir_effect_band(w, fir_band_tokens[band - 1], band_bindings, fir_edge_keys);
            else
                decode_fir_band_neutral(band_bindings);
            if (w.failed)
                return;
        }
    }
    else if (slot.type == Synth::EffectType::fir_multiband) {
        // Out-of-count band keys misstate the format: a band key beyond the
        // declared count is reported and ignored instead of
        // decoding into a binding nothing stages.  Band 0 always decodes: the
        // single-band form (the absent count key) is this type's normal shape,
        // not a stored marker.
        report_out_of_count_bands(w,
                                  multiband_band_tokens + 1,
                                  multiband_band_key_ids + 1,
                                  fir_band_count,
                                  have_fir_bands);
        if (w.failed)
            return;
        for (uint32_t band = 0; band < Synth::max_fir_effect_bands; band++) {
            Synth::EffectParamBinding* band_bindings = &slot.fir_bands.bands[4 * band];
            if (band >= fir_band_count)
                break; // dormant bands stay zeroed
            if (multiband_band_tokens[band])
                decode_fir_effect_band(w, multiband_band_tokens[band], band_bindings, multiband_fir_edge_keys);
            else
                decode_fir_band_neutral(band_bindings);
            if (w.failed)
                return;
        }
    }
    if (w.lenient && ! w.failed) {
        repair_mod_input(slot.enable_input, true, true);
        auto repair = [&](Synth::EffectParamBinding& binding) {
            if (! effect_source_valid(binding.lfo_depth_source)) {
                binding.lfo_depth_source = Synth::ModSource::none;
                mark_damage("effect depth source, defaulted");
            }
            if (! effect_source_valid(binding.lfo_rate_source)) {
                binding.lfo_rate_source = Synth::ModSource::none;
                mark_damage("effect rate source, defaulted");
            }
            for (uint32_t i = 0; i < binding.num_inputs; ++i)
                repair_mod_input(binding.inputs[i], true);
        };
        for (auto& binding : slot.bindings)
            repair(binding);
        for (auto& binding : slot.fir_bands.bands)
            repair(binding);
    }
}

static void decode_chain(Walker&                       w,
                         uint32_t                      obj,
                         Synth::EffectChainBinding&    chain,
                         Sculptor::EffectChainRouting& routing,
                         uint32_t                      owner = Synth::max_channels)
{
    if (w.toks[obj].type != JSMN_OBJECT) {
        if (w.lenient) {
            chain   = {};
            routing = {};
            mark_damage("effect chain, skipped");
        }
        else
            w.failed = true;
        return;
    }
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "effects")) {
            walk_entries(w, val_idx, Synth::max_chain_effects, "effect", [&](uint32_t elem, uint32_t pos) {
                DamageScope              scope(damaged_effects[owner][pos]);
                Synth::EffectSlotBinding slot = {};
                uint8_t                  feed = 0;
                decode_effect(w, elem, slot, &feed);
                if (w.failed && w.lenient) {
                    w.failed = false;
                    mark_damage("effect entry, skipped");
                    return;
                }
                chain.effects[pos] = slot;
                routing.out[pos]   = feed;
                chain.num_effects  = static_cast<uint8_t>(pos + 1);
            });
            if (w.failed && w.lenient) {
                w.failed = false;
                chain    = {};
                routing  = {};
                mark_damage("effect array, skipped");
            }
        }
        else if (key_eq(w, key_idx, "input")) {
            uint32_t value = 0;
            want_routing_name(w, key_idx, val_idx, effect_input_names, 3, &value);
            routing.in = static_cast<uint8_t>(value);
        }
        else
            unknown_field(w, key_idx);
    });
}

// File identities exist only during the bounded document walk. Zero means absent;
// table values are dense slot+1, so descriptor references resolve directly.
static uint16_t file_pool_slots[4][65536];
static uint16_t file_zone_ids[Synth::max_channels][Synth::max_instr_per_channel];
static uint16_t file_layout_ids[Synth::max_graph_records];
static uint16_t staged_layout_id;
static bool     salvage_identity[Synth::max_channels][Synth::max_instr_per_channel];
static bool     zone_present[Synth::max_channels][Synth::max_instr_per_channel];
static bool     zone_invalid[Synth::max_channels][Synth::max_instr_per_channel];
static_assert(sizeof(file_pool_slots) + sizeof(file_zone_ids) + sizeof(file_layout_ids) + sizeof(staged_layout_id) ==
              bank_json_id_scratch_bytes);

static bool decode_pool_id(Walker& w, uint32_t obj, uint16_t* table, uint32_t slot)
{
    if (w.toks[obj].type != JSMN_OBJECT) {
        w.failed = true;
        return false;
    }
    uint32_t id   = 0;
    bool     seen = false;
    walk_object(w, obj, [&](uint32_t key, uint32_t val) {
        if (key_eq(w, key, "id")) {
            seen = true;
            want_uint(w, val, 0xFFFF, &id);
        }
    });
    if (! seen) {
        w.failed = true;
        return false;
    }
    if (! id || table[id]) {
        w.failed = true;
        return false;
    }
    if (w.failed)
        return false;
    table[id] = static_cast<uint16_t>(slot + 1);
    return true;
}

static bool resolve_file_id(uint16_t& id, const uint16_t* table, bool zero_based = false)
{
    if (! id) {
        if (zero_based)
            record_ignored("absent parameter/instrument reference, using default");
        return true;
    }
    if (! table[id])
        return false;
    id = static_cast<uint16_t>(table[id] - (zero_based ? 1 : 0));
    return true;
}

static bool resolve_chain_ids(Synth::EffectChainBinding& chain, const uint16_t* lfos)
{
    for (uint32_t e = 0; e < chain.num_effects; ++e) {
        Synth::EffectSlotBinding& effect = chain.effects[e];
        for (auto& binding : effect.bindings)
            if (! resolve_file_id(binding.lfo_desc_id, lfos))
                return false;
        for (auto& binding : effect.fir_bands.bands)
            if (! resolve_file_id(binding.lfo_desc_id, lfos))
                return false;
    }
    return true;
}

static void decode_zone(Walker& w, uint32_t obj, Synth::Zone& zone, uint16_t& instrument_id)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "start")) {
            uint8_t start = 0;
            want_u8(w, val_idx, 127, &start);
            if (! w.failed)
                zone.start_note = static_cast<uint8_t>(start + 1);
        }
        else if (key_eq(w, key_idx, "instrument")) {
            instrument_id = reference_id(w, key_idx, val_idx);
        }
        else
            unknown_field(w, key_idx);
    });
}

static void canonicalize_name(char* name);

static bool chain_object_has_effects(Walker& w, uint32_t obj)
{
    if (w.toks[obj].type != JSMN_OBJECT)
        return false;
    bool has_effects = false;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        const jsmntok_t& token = w.toks[val_idx];
        if (key_eq(w, key_idx, "effects") && token.type == JSMN_ARRAY && token.size > 0)
            has_effects = true;
    });
    return has_effects;
}

static void decode_channel(Walker& w, uint32_t obj, Synth::InstrumentEditorBank* out, uint32_t channel)
{
    DamageScope scope(damaged_channels[channel]);
    int         content_pairs = w.toks[obj].type == JSMN_OBJECT ? w.toks[obj].size : 0;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "category"))
            --content_pairs;
        else if (key_eq(w, key_idx, "chain") && ! chain_object_has_effects(w, val_idx))
            --content_pairs;
    });

    if (content_pairs > 0)
        out->bank.channel_enabled[channel] = 1;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        // An explicit empty name is stored as-is but is not custom.  The next
        // encode omits it, so the factory name returns on the next decode.
        if (key_eq(w, key_idx, "name")) {
            want_text(w, val_idx, out->channel_names[channel], Synth::max_name_len);
            if (w.failed && w.lenient) {
                w.failed = false;
                Synth::get_default_channel_name(channel, out->channel_names[channel], Synth::max_name_len);
                mark_damage("channel name, defaulted");
            }
        }
        else if (key_eq(w, key_idx, "category")) {
            char             category[Synth::max_name_len] = {};
            const jsmntok_t& token                         = w.toks[val_idx];
            if (token.type != JSMN_STRING || ! unescape_string(w.doc + token.start,
                                                               static_cast<uint32_t>(token.end - token.start),
                                                               category,
                                                               sizeof(category))) {
                record_ignored("category");
                memset(out->channel_category[channel], 0, Synth::max_name_len);
            }
            else {
                memcpy(out->channel_category[channel], category, sizeof(category));
                canonicalize_name(out->channel_category[channel]);
            }
        }
        else if (key_eq(w, key_idx, "zones"))
            walk_entries(w, val_idx, Synth::max_instr_per_channel, "zone", [&](uint32_t elem, uint32_t pos) {
                DamageScope zone_scope(damaged_zones[channel][pos]);
                zone_present[channel][pos] = true;
                decode_zone(w, elem, out->bank.channel_zones[channel][pos], file_zone_ids[channel][pos]);
                if (w.lenient && ! out->bank.channel_zones[channel][pos].start_note)
                    w.failed = true;
                if (w.failed && w.lenient) {
                    w.failed                              = false;
                    out->bank.channel_zones[channel][pos] = {};
                    zone_invalid[channel][pos]            = true;
                    file_zone_ids[channel][pos]           = 0;
                }
            });
        else if (key_eq(w, key_idx, "chain"))
            decode_chain(w, val_idx, out->bank.channel_chains[channel], out->effect_routing[channel], channel);
        else
            unknown_field(w, key_idx);
        if (w.failed && w.lenient) {
            w.failed = false;
            mark_damage("channel field, defaulted");
        }
    });
    // A chain alone has no playable zones.  Keep its editor state disabled,
    // rather than treating this healthy channel as a damaged empty channel.
    if ((w.guarded || w.lenient) && ! damaged_channels[channel] && ! zone_present[channel][0] &&
        out->bank.channel_chains[channel].num_effects)
        out->bank.channel_enabled[channel] = 0;
}

// Pool entries decode into zeroed locals (absent entry fields take their zero
// defaults), then take one slot of the pool: sequential allocation rebuilds the
// dense prefix the bank invariants require.  Every allocation is checked - the
// pool fills up when the document repeats pool arrays, and a full pool must reject
// the document, never write past its entries.
// Pool staging uses the original token's id only after the entry validates.
static uint32_t pool_entry_id(Walker& w, uint32_t elem)
{
    uint32_t id = 0;
    if (w.toks[elem].type != JSMN_OBJECT) {
        w.failed = true;
        return 0;
    }
    walk_object(w, elem, [&](uint32_t key, uint32_t val) {
        if (key_eq(w, key, "id"))
            want_uint(w, val, 65535, &id);
    });
    if (! id)
        w.failed = true;
    return id;
}

static void repair_envelope(Walker& w, Synth::EnvelopeDescriptor& env)
{
    if (! Synth::salvage_envelope_valid(env)) {
        if (! env.num_points || env.num_points > Synth::max_envelope_points)
            env.num_points = 1;
        if (env.sustain_first_point > env.sustain_last_point || env.sustain_last_point >= env.num_points) {
            env.sustain_first_point = 0;
            env.sustain_last_point  = 0;
        }
        if (! Synth::salvage_envelope_valid(env))
            for (uint32_t i = 0; i < env.num_points; ++i)
                env.points[i].position = 0;
        mark_damage("envelope field, defaulted");
    }
    (void)w;
}

static void repair_lfo(Walker& w, Synth::LFODescriptor& lfo)
{
    if (! lfo.period_ms) {
        lfo.period_ms = 1000;
        mark_damage("LFO period, defaulted");
    }
    if (lfo.wave != Synth::WaveType::sine_wave && lfo.wave != Synth::WaveType::sawtooth_wave) {
        lfo.wave = Synth::WaveType::sine_wave;
        mark_damage("LFO wave, defaulted");
    }
    (void)w;
}

static bool salvage_param_valid(const Synth::ParamDescriptor& param)
{
    switch (param.kind) {
        case Synth::ParamKind::external:
            return true;
        case Synth::ParamKind::envelope:
            return Synth::salvage_envelope_valid(param.envelope);
        case Synth::ParamKind::lfo:
            return Synth::salvage_lfo_valid(param.lfo.lfo);
        case Synth::ParamKind::plain:
            return param.plain.num_sources <= Synth::max_param_sources;
    }
    return false;
}

// Pool staging shares identity, allocation and damage bookkeeping.  Entry-specific
// decoding and repair stay in the callers so public strict decoding stays unchanged.
template <typename Entry, typename Pool, typename Decode, typename Repair, typename Valid>
static void decode_pool(Walker&     w,
                        uint32_t    arr,
                        Pool&       pool,
                        uint32_t    capacity,
                        uint32_t    kind,
                        const char* name,
                        bool*       flags,
                        Decode      decode,
                        Repair      repair,
                        Valid       valid)
{
    walk_entries(w, arr, capacity, name, [&](uint32_t elem, uint32_t) {
        bool           damaged = false;
        DamageScope    scope(damaged);
        const uint32_t id    = pool_entry_id(w, elem);
        Entry          entry = {};
        if (! w.failed)
            decode(elem, entry);
        if (w.lenient && ! w.failed)
            repair(entry);
        if (w.lenient && ! w.failed && ! valid(entry))
            w.failed = true;
        if (! w.failed && (file_pool_slots[kind][id] || pool.num_allocated >= capacity))
            w.failed = true;
        if (w.failed) {
            if (w.lenient) {
                w.failed = false;
                char context[48];
                snprintf(context, sizeof(context), "%s entry id %u, skipped", name, id);
                mark_damage(context);
            }
            return;
        }
        const uint32_t slot = pool.allocate();
        if (slot == pool_no_slot) {
            w.failed = true;
            return;
        }
        flags[slot]               = damaged;
        pool.entries[slot]        = entry;
        file_pool_slots[kind][id] = static_cast<uint16_t>(slot + 1);
    });
    if (w.failed && w.lenient) {
        w.failed = false;
        char context[48];
        snprintf(context, sizeof(context), "%s collection, skipped", name);
        record_skipped(context);
    }
}

static void decode_instruments(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    decode_pool<Synth::Instrument>(
        w,
        arr,
        out->bank.instruments,
        Synth::max_instruments,
        0,
        "instrument",
        damaged_instruments,
        [&](uint32_t elem, Synth::Instrument& entry) {
            decode_instrument(w, elem, entry);
            const uint32_t slot = out->bank.instruments.num_allocated;
            if (! w.failed && slot < Synth::max_instruments) {
                // A rejected entry must not leave its name in the next reused slot.
                memset(out->instrument_names[slot], 0, Synth::max_name_len);
                walk_object(w, elem, [&](uint32_t key, uint32_t val) {
                    if (key_eq(w, key, "name"))
                        want_text(w, val, out->instrument_names[slot], Synth::max_name_len);
                });
            }
        },
        [](Synth::Instrument&) {},
        Synth::salvage_instrument_valid);
}

static void decode_envelopes(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    decode_pool<Synth::EnvelopeDescriptor>(
        w,
        arr,
        out->bank.envelopes,
        Synth::max_envelopes,
        1,
        "envelope",
        damaged_envelopes,
        [&](uint32_t elem, Synth::EnvelopeDescriptor& entry) { decode_envelope(w, elem, entry); },
        [&](Synth::EnvelopeDescriptor& entry) { repair_envelope(w, entry); },
        Synth::salvage_envelope_valid);
}

static void decode_lfos(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    decode_pool<Synth::LFODescriptor>(
        w,
        arr,
        out->bank.lfos,
        Synth::max_lfos,
        2,
        "LFO",
        damaged_lfos,
        [&](uint32_t elem, Synth::LFODescriptor& entry) { decode_lfo(w, elem, entry); },
        [&](Synth::LFODescriptor& entry) { repair_lfo(w, entry); },
        Synth::salvage_lfo_valid);
}

static void decode_parameters(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    decode_pool<Synth::ParamDescriptor>(
        w,
        arr,
        out->bank.parameters,
        Synth::max_parameters,
        3,
        "parameter",
        damaged_parameters,
        [&](uint32_t elem, Synth::ParamDescriptor& entry) {
            decode_param(w, elem, entry);
            // Runtime validation does not validate standalone parameter semantics.
            // Only the guarded strict pass requests a repair retry for these entries.
            if (w.guarded && ! w.lenient && ! w.failed && ! salvage_param_valid(entry))
                mark_damage("parameter field, defaulted");
        },
        [&](Synth::ParamDescriptor& entry) {
            if (entry.kind == Synth::ParamKind::envelope)
                repair_envelope(w, entry.envelope);
            if (entry.kind == Synth::ParamKind::lfo)
                repair_lfo(w, entry.lfo.lfo);
        },
        salvage_param_valid);
}

// One sparse layout record.  Field-level checks that do not depend on the
// descriptor pools reject here; the descriptor-range and implied-node-count
// checks run after the whole document decoded (decode order is free).
// Recognize kind before walking locators so unknown/numeric records cannot
// reach invariant validation through default kind zero.
static uint32_t graph_record_kind(Walker& w, uint32_t object, const char* const* names, uint32_t count)
{
    uint32_t kind      = count;
    bool     kind_seen = false;
    walk_object(w, object, [&](uint32_t key, uint32_t value) {
        if (! key_eq(w, key, "kind"))
            return;
        kind_seen = true;
        if (w.toks[value].type == JSMN_STRING) {
            for (uint32_t i = 0; i < count; ++i)
                if (tok_text_eq(w, value, names[i])) {
                    kind = i;
                    return;
                }
        }
        unknown_field(w, key);
        record_lost("unknown layout kind, skipped");
    });
    if (! kind_seen)
        record_skipped("layout missing kind, skipped");
    return kind;
}

static bool decode_editor_record(Walker& w, uint32_t obj, Synth::GraphNodeLayout* record)
{
    staged_layout_id    = 0;
    const uint32_t kind = graph_record_kind(w, obj, graph_kind_names, 7);
    if (kind >= 7 || w.failed)
        return false;
    uint32_t locators    = 0;
    bool     lfo_node    = false;
    bool     names_known = true;
    walk_object(w, obj, [&](uint32_t key, uint32_t value) {
        if (kind == 4 && (key_eq(w, key, "depth_source") || key_eq(w, key, "rate_source"))) {
            bool recognized = false;
            if (w.toks[value].type == JSMN_STRING)
                for (uint32_t i = 0; i < 7; ++i)
                    recognized = recognized || tok_text_eq(w, value, mod_source_names[i]);
            if (! recognized) {
                unknown_field(w, key);
                names_known = false;
            }
        }
        if (key_eq(w, key, "channel")) {
            uint32_t locator = 0;
            if (! unsigned_literal(w, value, Synth::max_channels - 1, &locator) && kind != 5)
                locators |= 128;
            locators |= 1;
        }
        if (key_eq(w, key, "zone")) {
            uint32_t locator = 0;
            if (! unsigned_literal(w, value, Synth::max_instr_per_channel - 1, &locator))
                locators |= 128;
            locators |= 2;
        }
        if (key_eq(w, key, "layer"))
            locators |= 4;
        if (key_eq(w, key, "envelope_id") || key_eq(w, key, "lfo_id"))
            locators |= 8;
        if (key_eq(w, key, "uid"))
            locators |= 16;
        if (key_eq(w, key, "chain"))
            locators |= 32;
        if (key_eq(w, key, "name"))
            locators |= 64;
        if (key_eq(w, key, "node") && tok_text_eq(w, value, "lfo"))
            lfo_node = true;
    });
    if (! names_known) {
        record_lost("layout source name, defaulted");
        if (! w.lenient)
            return false;
    } // unknown roles skip the detached LFO
    const uint32_t required = kind == 5 ? (32u | (lfo_node ? 8u : 64u))
                                        : (3u | (kind == 2                ? 4u
                                                 : kind == 3 || kind == 4 ? 24u
                                                                          : 0u));
    if (w.lenient && (locators & 128)) {
        mark_damage("layout invalid locator, skipped");
        return false;
    }
    if ((locators & required) != required) {
        mark_damage("layout missing locator, skipped");
        return false;
    }
    const uint8_t stored_kinds[] = { 0, 0, 0, 1, 2, 4, 5 };
    record->kind                 = stored_kinds[kind];
    if (kind == 1)
        record->index = 1;
    // unescape_string needs one spare terminator byte, so the name stages
    // into a 33-byte buffer; the kind-gated copy back runs after the object
    // walk, when record->kind is known.
    char staged_name[sizeof(record->name) + 1] = {};
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "channel")) {
            want_u8(w, val_idx, 0xFF, &record->channel);
        }
        else if (key_eq(w, key_idx, "zone")) {
            want_u8(w, val_idx, 0xFF, &record->zone);
        }
        else if (key_eq(w, key_idx, "kind")) {
        }
        else if (key_eq(w, key_idx, "layer") && kind == 2) {
            uint8_t layer = 0;
            want_u8(w, val_idx, Synth::max_layers - 1, &layer);
            record->index = static_cast<uint8_t>(layer + 2);
        }
        else if ((key_eq(w, key_idx, "envelope_id") && kind == 3) || (key_eq(w, key_idx, "lfo_id") && kind == 4)) {
            staged_layout_id = reference_id(w, key_idx, val_idx);
        }
        else if (key_eq(w, key_idx, "chain") && kind == 5) {
            if (tok_text_eq(w, val_idx, "master"))
                record->channel = Synth::max_channels;
            else
                want_u8(w, val_idx, Synth::max_channels - 1, &record->channel);
        }
        else if (key_eq(w, key_idx, "node") && kind == 5) {
            if (! tok_text_eq(w, val_idx, "lfo"))
                unknown_field(w, key_idx);
        }
        else if (key_eq(w, key_idx, "lfo_id") && kind == 5) {
            staged_layout_id = reference_id(w, key_idx, val_idx);
            snprintf(staged_name, sizeof(staged_name), "LFO %u", staged_layout_id);
        }
        else if (key_eq(w, key_idx, "x"))
            want_float(w, val_idx, &record->x);
        else if (key_eq(w, key_idx, "y"))
            want_float(w, val_idx, &record->y);
        else if (key_eq(w, key_idx, "width"))
            want_float(w, val_idx, &record->width_override);
        else if (key_eq(w, key_idx, "height"))
            want_float(w, val_idx, &record->height_override);
        else if (key_eq(w, key_idx, "depth_source")) {
            uint32_t value = 0;
            want_routing_name(w, key_idx, val_idx, mod_source_names, 7, &value);
            record->depth_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "rate_source")) {
            uint32_t value = 0;
            want_routing_name(w, key_idx, val_idx, mod_source_names, 7, &value);
            record->rate_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "uid")) {
            want_u8(w, val_idx, 0xFF, &record->uid);
        }
        else if (key_eq(w, key_idx, "name")) {
            want_text(w, val_idx, staged_name, sizeof(staged_name));
        }
        else
            unknown_field(w, key_idx);
    });
    if ((kind == 3 || kind == 4 || (kind == 5 && lfo_node)) && ! staged_layout_id) {
        mark_damage("layout without valid descriptor reference, skipped");
        return false;
    }
    if (w.failed)
        return false;

    if (! stage_record_name(record->name, sizeof(record->name), staged_name, sizeof(staged_name), record->kind)) {
        w.failed = true;
        return false;
    }

    if ((record->channel >= Synth::max_channels && ! (record->kind == 4 && record->channel == Synth::max_channels)) ||
        record->zone >= Synth::max_instr_per_channel || ! Synth::editor_record_kind_valid(record->kind)) {
        w.failed = true;
        return false;
    }

    // Kind 0 keys a bound node in the fixed canonical numbering (inputs, sum
    // node, oscillator layers), and only by index: a nonzero uid would
    // create a second key for one node.  Kind 5 keys the zone's Instrument
    // Parameters node alone, so its index and uid are unused and must be 0.
    if (record->kind == 0 && (record->index >= Synth::graph_canonical_node_count || record->uid != 0)) {
        w.failed = true;
        return false;
    }

    if (record->kind == 5 && (record->index != 0 || record->uid != 0)) {
        w.failed = true;
        return false;
    }

    // Kind 2 reads the source bytes as ModSource values; none (0) is
    // valid.  Other kinds never read them.
    if (record->kind == 2 && (record->depth_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine) ||
                              record->rate_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine))) {
        w.failed = true;
        return false;
    }
    if (((record->kind == 1 || record->kind == 2) && record->uid == 0) ||
        (record->kind == 4 && (record->zone != 0 || record->index != 0 || record->uid != 0 || ! record->name[0]))) {
        record_lost("invalid layout record, skipped");
        return false;
    }
    return true;
}

// Reject legacy positional rows before interpreting any element.
static bool object_array(Walker& w, uint32_t token)
{
    if (w.toks[token].type != JSMN_ARRAY)
        return false;
    uint32_t element = token + 1;
    for (int i = 0; i < w.toks[token].size; ++i) {
        if (w.toks[element].type != JSMN_OBJECT)
            return false;
        element = skip_value(w, element);
        if (element == invalid_index)
            return false;
    }
    return true;
}

static bool decode_source_wiring(Walker& w, uint32_t token, uint8_t* row)
{
    if (! object_array(w, token))
        return false;
    bool valid = true;
    walk_array(w, token, Synth::max_layers, [&](uint32_t element, uint32_t) {
        uint32_t slot  = Synth::max_layers;
        uint32_t layer = 0;
        walk_object(w, element, [&](uint32_t key, uint32_t value) {
            if (key_eq(w, key, "slot"))
                want_uint(w, value, 0xFFFF, &slot);
            else if (key_eq(w, key, "layer"))
                want_uint(w, value, 0xFFFF, &layer);
            else
                unknown_field(w, key);
        });
        if (slot >= Synth::max_layers || layer == 0)
            valid = false;
        else if (layer > 0xFF)
            w.failed = true;
        else
            row[slot] = static_cast<uint8_t>(layer); // layer-count validation runs after the walk
    });
    return valid && ! w.failed;
}

static bool decode_chain_owner(Walker& w, uint32_t value, uint32_t* owner)
{
    if (w.toks[value].type == JSMN_STRING) {
        if (! tok_text_eq(w, value, "master"))
            return false;
        *owner = Synth::max_channels;
        return true;
    }
    if (w.toks[value].type != JSMN_PRIMITIVE)
        return false;
    return unsigned_literal(w, value, Synth::max_channels - 1, owner);
}

static void decode_editor_state(Walker&                      w,
                                uint32_t                     val_idx,
                                Synth::InstrumentEditorBank* out,
                                bool*                        routing_present = nullptr,
                                bool*                        editor_seen     = nullptr)
{
    if (w.lenient && w.toks[val_idx].type != JSMN_OBJECT) {
        record_lost("editor collection, skipped");
        return;
    }
    walk_object(w, val_idx, [&](uint32_t key_idx, uint32_t inner_idx) {
        if (editor_seen) {
            *editor_seen = true;
        }
        if (key_eq(w, key_idx, "layouts")) {
            if (w.lenient && w.toks[inner_idx].type != JSMN_ARRAY) {
                record_lost("layout collection, skipped");
                return;
            }
            uint16_t detached_per_zone[Synth::max_channels][Synth::max_instr_per_channel] = {};
            walk_entries(w, inner_idx, Synth::max_graph_records, "layout", [&](uint32_t elem, uint32_t) {
                bool        damaged = false;
                DamageScope scope(damaged);
                if (w.lenient && w.toks[elem].type != JSMN_OBJECT) {
                    record_lost("layout entry, skipped");
                    return;
                }
                Synth::GraphNodeLayout record         = {};
                const uint32_t         skipped_before = skipped_count;
                if (! decode_editor_record(w, elem, &record)) {
                    if (w.failed && w.lenient) {
                        w.failed = false;
                        if (skipped_count == skipped_before)
                            record_lost("layout entry, skipped");
                    }
                    return;
                }
                if (record.kind != 0 && record.kind != 4 && record.kind != 5) {
                    if (record.channel >= Synth::max_channels || record.zone >= Synth::max_instr_per_channel ||
                        ++detached_per_zone[record.channel][record.zone] > Synth::max_detached_per_zone) {
                        if (w.lenient) {
                            record_lost("layout capacity entry, skipped");
                            return;
                        }
                        w.failed = true;
                        return;
                    }
                }
                // One key addresses one node: compatibility decoding drops later records;
                // strict metadata decoding refuses duplicates before any deduplication.
                for (uint32_t i = 0; i < out->graph_layout_count; ++i) {
                    const Synth::GraphNodeLayout& other = out->graph_layout[i];
                    if (file_layout_ids[i] == staged_layout_id &&
                        Sculptor::graph_layout_record_identity_equal(other, record)) {
                        mark_damage("duplicate layout record, skipped");
                        if (w.strict_editor_metadata)
                            w.failed = true;
                        return;
                    }
                }
                if (out->graph_layout_count >= Synth::max_graph_records) {
                    w.failed = true;
                    return;
                }
                file_layout_ids[out->graph_layout_count]     = staged_layout_id;
                damaged_layouts[out->graph_layout_count]     = damaged;
                out->graph_layout[out->graph_layout_count++] = record;
            });
        }
        else if (key_eq(w, key_idx, "osc_sources")) {
            if (w.toks[inner_idx].type != JSMN_ARRAY) {
                unknown_field(w, key_idx);
                record_lost("editor array, skipped");
                return;
            }
            bool seen[Synth::max_channels][Synth::max_instr_per_channel] = {};
            walk_entries(w,
                         inner_idx,
                         Synth::max_channels * Synth::max_instr_per_channel,
                         "osc_sources",
                         [&](uint32_t elem, uint32_t) {
                             bool        damaged = false;
                             DamageScope scope(damaged);
                             if (w.toks[elem].type != JSMN_OBJECT) {
                                 record_lost("osc_sources element, skipped");
                                 return;
                             }
                             uint32_t channel = Synth::max_channels;
                             uint32_t zone    = Synth::max_instr_per_channel;
                             uint32_t wiring  = 0;
                             walk_object(w, elem, [&](uint32_t key, uint32_t value) {
                                 if (key_eq(w, key, "channel"))
                                     want_uint(w, value, 0xFFFF, &channel);
                                 else if (key_eq(w, key, "zone"))
                                     want_uint(w, value, 0xFFFF, &zone);
                                 else if (key_eq(w, key, "wiring"))
                                     wiring = value;
                                 else
                                     unknown_field(w, key);
                             });
                             uint8_t row[Synth::max_layers] = {};
                             if (w.failed) {
                                 if (w.lenient) {
                                     w.failed = false;
                                     record_lost("editor entry, skipped");
                                 }
                                 return;
                             }
                             if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel ||
                                 seen[channel][zone] || ! wiring || w.toks[wiring].size == 0 ||
                                 ! decode_source_wiring(w, wiring, row)) {
                                 unknown_field(w, key_idx);
                                 record_lost("editor entry, skipped");
                                 if (w.lenient)
                                     w.failed = false;
                                 if (w.lenient && channel < Synth::max_channels &&
                                     zone < Synth::max_instr_per_channel && ! seen[channel][zone])
                                     salvage_identity[channel][zone] = true;
                                 return;
                             }
                             seen[channel][zone] = true;
                             memcpy(out->graph_osc_source[channel][zone], row, sizeof(row));
                         });
        }
        else if (key_eq(w, key_idx, "added_ccs")) {
            if (w.toks[inner_idx].type != JSMN_ARRAY) {
                unknown_field(w, key_idx);
                record_lost("editor array, skipped");
                return;
            }
            bool seen[Synth::max_channels + 1] = {};
            walk_entries(w, inner_idx, Synth::max_channels + 1, "added_ccs", [&](uint32_t elem, uint32_t) {
                if (w.toks[elem].type != JSMN_OBJECT) {
                    record_lost("added_ccs element, skipped");
                    return;
                }
                uint32_t owner = Synth::max_channels + 1;
                uint32_t ccs   = 0;
                bool     valid = true;
                walk_object(w, elem, [&](uint32_t key, uint32_t value) {
                    if (key_eq(w, key, "chain"))
                        valid = decode_chain_owner(w, value, &owner);
                    else if (key_eq(w, key, "ccs"))
                        ccs = value;
                    else
                        unknown_field(w, key);
                });
                if (w.failed) {
                    if (w.lenient) {
                        w.failed = false;
                        record_lost("editor entry, skipped");
                    }
                    return;
                }
                if (! valid || owner > Synth::max_channels || seen[owner] || ! ccs || w.toks[ccs].type != JSMN_ARRAY) {
                    unknown_field(w, key_idx);
                    record_lost("editor entry, skipped");
                    return;
                }
                uint16_t mask = 0;
                walk_array(w, ccs, 16, [&](uint32_t value, uint32_t) {
                    uint32_t cc = 0;
                    if (! unsigned_literal(w, value, 31, &cc) || cc < 16) {
                        unknown_field(w, key_idx);
                        valid = false;
                        return;
                    }
                    mask |= static_cast<uint16_t>(1u << (cc - 16));
                });
                if (w.failed) {
                    if (w.lenient) {
                        w.failed = false;
                        record_lost("editor entry, skipped");
                    }
                    return;
                }
                if (! valid) {
                    record_lost("added_ccs entry, skipped");
                    return;
                }
                seen[owner]               = true;
                out->added_cc_mask[owner] = mask;
            });
        }
        else
            unknown_field(w, key_idx);
    });
}

// Count dependent layout loss before the editor helpers filter records.  Move
// scratch identities and flags with the same filter, so later repairs count once.
static void drop_dependent_layouts(Synth::InstrumentEditorBank& out,
                                   uint32_t                     channel,
                                   uint32_t                     zone,
                                   bool                         whole_channel)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < out.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = out.graph_layout[i];
        if (record.channel == channel && (whole_channel || (record.kind != 4 && record.zone == zone))) {
            DamageScope scope(damaged_layouts[i]);
            mark_damage("dependent layout, skipped");
            continue;
        }
        file_layout_ids[kept]   = file_layout_ids[i];
        damaged_layouts[kept++] = damaged_layouts[i];
    }
    if (whole_channel)
        Sculptor::channel_records_reset(&out, channel);
    else
        Sculptor::zone_records_drop_zone(&out, channel, zone);
}

static bool salvage_resolve_bank_ids(Synth::InstrumentEditorBank& out)
{
    Synth::InstrumentBank& bank = out.bank;
    static bool            drop_instr[Synth::max_instruments];
    static bool            drop_env[Synth::max_envelopes];
    static bool            drop_lfo[Synth::max_lfos];
    static bool            drop_param[Synth::max_parameters];
    static bool            default_param[Synth::max_parameters][2 + Synth::max_param_sources];
    memset(drop_instr, 0, sizeof(drop_instr));
    memset(drop_env, 0, sizeof(drop_env));
    memset(drop_lfo, 0, sizeof(drop_lfo));
    memset(drop_param, 0, sizeof(drop_param));
    memset(default_param, 0, sizeof(default_param));
    for (uint32_t i = 0; i < bank.instruments.num_allocated; ++i) {
        DamageScope scope(damaged_instruments[i]);
        for (uint32_t l = 0; l < bank.instruments.entries[i].layer_count; ++l)
            for (auto& target : bank.instruments.entries[i].layers[l].targets) {
                if (! resolve_file_id(target.envelope_desc_id, file_pool_slots[1])) {
                    target.envelope_desc_id = 0;
                    mark_damage("instrument envelope reference, defaulted");
                }
                if (! resolve_file_id(target.lfo_desc_id, file_pool_slots[2])) {
                    target.lfo_desc_id = 0;
                    mark_damage("instrument LFO reference, defaulted");
                }
            }
    }
    for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i) {
        Synth::ParamDescriptor& param   = bank.parameters.entries[i];
        auto                    resolve = [&](uint16_t& id, uint32_t reference) {
            default_param[i][reference] = id == 0;
            if (! resolve_file_id(id, file_pool_slots[3], true)) {
                drop_param[i] = true;
                id            = 0;
            }
        };
        if (param.kind == Synth::ParamKind::lfo) {
            resolve(param.lfo.depth_param_id, 0);
            resolve(param.lfo.rate_param_id, 1);
        }
        else if (param.kind == Synth::ParamKind::plain)
            for (uint32_t j = 0; j < param.plain.num_sources; ++j)
                resolve(param.plain.sources[j].param_id, 2 + j);
    }
    for (uint32_t round = 0; round < Synth::max_parameters; ++round) {
        bool changed  = false;
        bool survivor = false;
        for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i)
            survivor |= ! drop_param[i];
        for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i) {
            if (drop_param[i])
                continue;
            Synth::ParamDescriptor& param   = bank.parameters.entries[i];
            bool                    invalid = false;
            auto                    missing = [&](uint16_t id, uint32_t reference) {
                return default_param[i][reference] ? ! survivor : id >= bank.parameters.num_allocated || drop_param[id];
            };
            if (param.kind == Synth::ParamKind::lfo)
                invalid = missing(param.lfo.depth_param_id, 0) || missing(param.lfo.rate_param_id, 1);
            else if (param.kind == Synth::ParamKind::plain)
                for (uint32_t j = 0; j < param.plain.num_sources; ++j)
                    invalid |= missing(param.plain.sources[j].param_id, 2 + j);
            if (invalid) {
                drop_param[i] = true;
                changed       = true;
            }
        }
        if (! changed)
            break;
    }
    uint16_t default_slot = 0;
    for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i)
        if (! drop_param[i]) {
            default_slot = static_cast<uint16_t>(i);
            break;
        }
    for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i) {
        if (drop_param[i]) {
            DamageScope scope(damaged_parameters[i]);
            mark_damage("dependent parameter, skipped");
            continue;
        }
        Synth::ParamDescriptor& param = bank.parameters.entries[i];
        if (param.kind == Synth::ParamKind::lfo) {
            if (default_param[i][0])
                param.lfo.depth_param_id = default_slot;
            if (default_param[i][1])
                param.lfo.rate_param_id = default_slot;
        }
        else if (param.kind == Synth::ParamKind::plain)
            for (uint32_t j = 0; j < param.plain.num_sources; ++j)
                if (default_param[i][2 + j])
                    param.plain.sources[j].param_id = default_slot;
    }
    for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
        Synth::EffectChainBinding&    chain   = c < Synth::max_channels ? bank.channel_chains[c] : bank.master_chain;
        Sculptor::EffectChainRouting& routing = out.effect_routing[c];
        Synth::EffectChainBinding     trial   = {};
        Sculptor::EffectChainRouting  kept_routing = routing;
        memset(kept_routing.out, 0, sizeof(kept_routing.out));
        for (uint32_t e = 0; e < chain.num_effects; ++e) {
            DamageScope              scope(damaged_effects[c][e]);
            Synth::EffectSlotBinding effect = chain.effects[e];

            auto resolve = [&](uint16_t& id) {
                if (! resolve_file_id(id, file_pool_slots[2])) {
                    id = 0;
                    mark_damage("effect LFO reference, defaulted");
                }
            };
            for (auto& binding : effect.bindings)
                resolve(binding.lfo_desc_id);
            for (auto& binding : effect.fir_bands.bands)
                resolve(binding.lfo_desc_id);
            if (effect.type == Synth::EffectType::none)
                continue;
            trial.effects[trial.num_effects] = effect;
            ++trial.num_effects;
            if (! Synth::salvage_effect_chain_valid(trial, bank.lfos.num_allocated)) {
                --trial.num_effects;
                trial.effects[trial.num_effects] = {};
                char context[48];
                snprintf(context, sizeof(context), "effect %u of chain %u, skipped", e, c);
                mark_damage(context);
            }
            else {
                kept_routing.out[trial.num_effects - 1]   = routing.out[e];
                damaged_effects[c][trial.num_effects - 1] = damaged_effects[c][e];
            }
        }
        chain   = trial;
        routing = kept_routing;
        Sculptor::fx_compact_chain(&chain, &routing);
        Sculptor::fx_sanitize_routing(&chain, &routing);
    }
    uint32_t kept_layouts = 0;
    for (uint32_t i = 0; i < out.graph_layout_count; ++i) {
        DamageScope            scope(damaged_layouts[i]);
        Synth::GraphNodeLayout record = out.graph_layout[i];
        uint16_t               id     = file_layout_ids[i];
        if (record.kind == 1 || record.kind == 2 || (record.kind == 4 && id)) {
            if (! resolve_file_id(id, file_pool_slots[record.kind == 1 ? 1 : 2]) || ! id) {
                mark_damage("layout descriptor, skipped");
                continue;
            }
            if ((record.kind == 1 ? drop_env[id - 1] : drop_lfo[id - 1])) {
                mark_damage("dependent layout descriptor, skipped");
                continue;
            }
            if (record.kind == 4)
                snprintf(record.name, sizeof(record.name), "LFO %u", id);
            else
                record.index = static_cast<uint8_t>(id);
        }
        if (record.kind != 4 && ! zone_present[record.channel][record.zone]) {
            mark_damage("layout without decoded zone, skipped");
            continue;
        }
        file_layout_ids[kept_layouts]    = file_layout_ids[i];
        damaged_layouts[kept_layouts]    = damaged_layouts[i];
        out.graph_layout[kept_layouts++] = record;
    }
    out.graph_layout_count = kept_layouts;
    // Resolve zones before pool compaction.  Only invalid zones are removed.
    for (uint32_t c = 0; c < Synth::max_channels; ++c) {
        uint32_t z        = 0;
        uint8_t  previous = 0;
        while (z < Synth::max_instr_per_channel) {
            Synth::Zone& zone = bank.channel_zones[c][z];
            DamageScope  scope(damaged_zones[c][z]);
            if (! zone_present[c][z])
                break;
            uint16_t   id    = file_zone_ids[c][z];
            const bool valid = ! zone_invalid[c][z] && zone.start_note != 0 &&
                               resolve_file_id(id, file_pool_slots[0], true) && id < bank.instruments.num_allocated &&
                               ! drop_instr[id] && zone.start_note > previous;
            if (! valid) {
                char context[48];
                snprintf(context, sizeof(context), "zone %u of channel %u, skipped", z, c);
                mark_damage(context);
                drop_dependent_layouts(out, c, z, false);
                for (uint32_t next = z; next + 1 < Synth::max_instr_per_channel; ++next) {
                    bank.channel_zones[c][next] = bank.channel_zones[c][next + 1];
                    file_zone_ids[c][next]      = file_zone_ids[c][next + 1];
                    zone_present[c][next]       = zone_present[c][next + 1];
                    damaged_zones[c][next]      = damaged_zones[c][next + 1];
                    zone_invalid[c][next]       = zone_invalid[c][next + 1];
                    salvage_identity[c][next]   = salvage_identity[c][next + 1];
                }
                bank.channel_zones[c][Synth::max_instr_per_channel - 1] = {};
                file_zone_ids[c][Synth::max_instr_per_channel - 1]      = 0;
                zone_present[c][Synth::max_instr_per_channel - 1]       = false;
                zone_invalid[c][Synth::max_instr_per_channel - 1]       = false;
                salvage_identity[c][Synth::max_instr_per_channel - 1]   = false;
            }
            else {
                zone.instrument = static_cast<uint8_t>(id);
                previous        = zone.start_note;
                ++z;
            }
        }
        const uint32_t kept = z;
        if (kept && bank.channel_zones[c][0].start_note != 1) {
            DamageScope scope(damaged_zones[c][0]);
            mark_damage("first zone start, repaired");
            bank.channel_zones[c][0].start_note = 1;
        }
        else if (! kept) {
            DamageScope scope(damaged_channels[c]);
            bool        reset = damaged_channels[c];
            for (uint32_t entry = 0; entry < Synth::max_instr_per_channel; ++entry)
                reset |= damaged_zones[c][entry];
            reset |= bank.channel_enabled[c] && ! bank.channel_chains[c].num_effects;
            if (reset && bank.channel_enabled[c])
                mark_damage("channel without decoded zones, disabled");
            bank.channel_enabled[c] = 0;
            // Healthy disabled channels can retain channel-wide effect layouts.
            if (reset)
                drop_dependent_layouts(out, c, 0, true);
        }
    }
    Synth::salvage_compact(&out, drop_instr, drop_env, drop_lfo, drop_param, damaged_layouts);
    // Reset invalid wiring zone-by-zone, without dropping the editor block.
    for (uint32_t c = 0; c < Synth::max_channels; ++c)
        for (uint32_t z = 0; z < Synth::max_instr_per_channel; ++z) {
            const Synth::Zone& zone = bank.channel_zones[c][z];
            if (! zone.start_note) {
                memset(out.graph_osc_source[c][z], 0, Synth::max_layers);
                continue;
            }
            const uint32_t layers = bank.instruments.entries[zone.instrument].layer_count;
            bool           valid  = true;
            for (uint8_t source : out.graph_osc_source[c][z])
                valid &= source <= layers;
            if (! valid || salvage_identity[c][z]) {
                Sculptor::set_identity_osc_sources(&out, c, z);
                if (! salvage_identity[c][z])
                    record_lost("osc_sources wiring, skipped");
            }
        }
    // All entry invariants hold now; only the whole-bank budget can remain.
    for (int c = static_cast<int>(Synth::max_channels); c >= 0 && ! Synth::validate_instrument_bank(&bank, true); --c) {
        Synth::EffectChainBinding& chain =
            c == static_cast<int>(Synth::max_channels) ? bank.master_chain : bank.channel_chains[c];
        while (chain.num_effects && ! Synth::validate_instrument_bank(&bank, true)) {
            --chain.num_effects;
            chain.effects[chain.num_effects]             = {};
            out.effect_routing[c].out[chain.num_effects] = 0;
            DamageScope scope(damaged_effects[c][chain.num_effects]);
            mark_damage("effect budget slot, skipped");
        }
    }
    return Synth::validate_instrument_bank(&bank, true);
}

static bool resolve_bank_ids(Synth::InstrumentEditorBank& out)
{
    auto& bank = out.bank;
    for (uint32_t i = 0; i < bank.instruments.num_allocated; ++i) {
        auto& instrument = bank.instruments.entries[i];
        for (uint32_t layer = 0; layer < instrument.layer_count; ++layer)
            for (auto& target : instrument.layers[layer].targets)
                if (! resolve_file_id(target.envelope_desc_id, file_pool_slots[1]) ||
                    ! resolve_file_id(target.lfo_desc_id, file_pool_slots[2]))
                    return false;
    }
    for (uint32_t i = 0; i < bank.parameters.num_allocated; ++i) {
        auto& param = bank.parameters.entries[i];
        if (param.kind == Synth::ParamKind::lfo) {
            if (! resolve_file_id(param.lfo.depth_param_id, file_pool_slots[3], true) ||
                ! resolve_file_id(param.lfo.rate_param_id, file_pool_slots[3], true))
                return false;
        }
        else if (param.kind == Synth::ParamKind::plain)
            for (uint32_t j = 0; j < param.plain.num_sources; ++j)
                if (! resolve_file_id(param.plain.sources[j].param_id, file_pool_slots[3], true))
                    return false;
    }
    for (uint32_t c = 0; c < Synth::max_channels; ++c) {
        uint32_t kept_zones = 0;
        for (uint32_t z = 0; z < Synth::max_instr_per_channel; ++z) {
            auto& zone = bank.channel_zones[c][z];
            if (! zone.start_note) {
                if (zone_present[c][z]) {
                    char context[48];
                    snprintf(context, sizeof(context), "zone %u of channel %u, skipped", z, c);
                    record_skipped(context);
                    drop_dependent_layouts(out, c, kept_zones, false);
                }
                continue;
            }
            uint16_t id = file_zone_ids[c][z];
            if (! resolve_file_id(id, file_pool_slots[0], true))
                return false;
            if (! bank.instruments.num_allocated) {
                record_skipped("zone without decoded instrument, skipped");
                continue;
            }
            zone.instrument                     = static_cast<uint8_t>(id);
            bank.channel_zones[c][kept_zones++] = zone;
        }
        for (uint32_t z = kept_zones; z < Synth::max_instr_per_channel; ++z)
            bank.channel_zones[c][z] = {};
        if (! bank.instruments.num_allocated && ! kept_zones && bank.channel_enabled[c]) {
            record_skipped("channel without decoded zones, disabled");
            bank.channel_enabled[c] = 0;
        }
        if (! resolve_chain_ids(bank.channel_chains[c], file_pool_slots[2]))
            return false;
    }
    if (! resolve_chain_ids(bank.master_chain, file_pool_slots[2]))
        return false;
    uint32_t kept_layouts = 0;
    for (uint32_t i = 0; i < out.graph_layout_count; ++i) {
        DamageScope scope(damaged_layouts[i]);
        auto&       record = out.graph_layout[i];
        uint16_t    id     = file_layout_ids[i];
        if (record.kind == 1 || record.kind == 2 || (record.kind == 4 && id)) {
            if (! resolve_file_id(id, file_pool_slots[record.kind == 1 ? 1 : 2]))
                return false;
            if (! id) {
                mark_damage("layout reference defaulted, skipped");
                continue;
            }
            if (record.kind == 4)
                snprintf(record.name, sizeof(record.name), "LFO %u", id);
            else
                record.index = static_cast<uint8_t>(id);
        }
        if (record.kind != 4 && ! bank.channel_zones[record.channel][record.zone].start_note) {
            mark_damage("layout without decoded zone, skipped");
            continue;
        }
        out.graph_layout[kept_layouts++] = record;
    }
    out.graph_layout_count = kept_layouts;
    return true;
}

static void decode_bank(Walker&                      w,
                        uint32_t                     obj,
                        Synth::InstrumentEditorBank* out,
                        bool*                        routing_present = nullptr,
                        bool*                        editor_seen     = nullptr)
{
    bool format_seen = false;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "format")) {
            format_seen = true;
            if (! tok_text_eq(w, val_idx, "synth-bank-v2")) {
                record_ignored("unrecognized bank format tag, loading anyway");
            }
        }
        else if (key_eq(w, key_idx, "instruments"))
            decode_instruments(w, val_idx, out);
        else if (key_eq(w, key_idx, "envelopes"))
            decode_envelopes(w, val_idx, out);
        else if (key_eq(w, key_idx, "lfos"))
            decode_lfos(w, val_idx, out);
        else if (key_eq(w, key_idx, "parameters"))
            decode_parameters(w, val_idx, out);
        else if (key_eq(w, key_idx, "channels")) {
            if (w.lenient && w.toks[val_idx].type != JSMN_ARRAY) {
                record_skipped("channel collection, skipped");
                return;
            }
            walk_entries(w, val_idx, Synth::max_channels, "channel", [&](uint32_t elem, uint32_t pos) {
                DamageScope scope(damaged_channels[pos]);
                if (w.toks[elem].type != JSMN_OBJECT) {
                    if (w.lenient) {
                        mark_damage("channel entry, skipped");
                        return;
                    }
                    w.failed = true;
                    return;
                }
                decode_channel(w, elem, out, pos);
                if (w.failed && w.lenient) {
                    w.failed                       = false;
                    out->bank.channel_enabled[pos] = 0;
                    memset(out->bank.channel_zones[pos], 0, sizeof(out->bank.channel_zones[pos]));
                    out->bank.channel_chains[pos] = {};
                    Sculptor::channel_records_reset(out, pos);
                    mark_damage("channel entry, skipped");
                }
            });
        }
        else if (key_eq(w, key_idx, "master_chain"))
            decode_chain(w, val_idx, out->bank.master_chain, out->effect_routing[Synth::max_channels]);
        else if (key_eq(w, key_idx, "editor")) {
            decode_editor_state(w, val_idx, out, routing_present, editor_seen);
        }
        else
            unknown_field(w, key_idx);
    });
    if (! format_seen) {
        record_ignored("missing bank format tag, loading anyway");
    }
}

// The bank every absent field defaults to: an empty, all-disabled bank with the
// factory channel names.
static void fill_default_editor_bank(Synth::InstrumentEditorBank* bank)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    for (uint32_t c = 0; c < Synth::max_channels; c++)
        Synth::get_default_channel_name(c, bank->channel_names[c], Synth::max_name_len);
}

// ---------------------------------------------------------------------------
// Canonical form
// ---------------------------------------------------------------------------

// The canonical form only normalizes floats and zeroes unused bytes; every count it
// walks was validated first (canonicalize requires a valid bank).
static float canonical_float(float v)
{
    return v == 0.0f && std::signbit(v) ? 0.0f : v; // no negative zero survives a decode
}

static void canonicalize_name(char* name)
{
    const void* const nul = memchr(name, 0, Synth::max_name_len);
    if (! nul) {
        name[Synth::max_name_len - 1] = 0; // an unterminated name cannot leave the decoder
        return;
    }
    const uint32_t len = static_cast<uint32_t>(static_cast<const char*>(nul) - name);
    memset(name + len + 1, 0, Synth::max_name_len - len - 1);
}

static void canonicalize_mod_input(Synth::ModInput& in)
{
    in.scale = canonical_float(in.scale);
}

static void canonicalize_binding(Synth::EffectParamBinding& binding)
{
    binding.base_value     = canonical_float(binding.base_value);
    binding.lfo_depth      = canonical_float(binding.lfo_depth);
    binding.lfo_rate_scale = canonical_float(binding.lfo_rate_scale);
    for (uint32_t i = 0; i < binding.num_inputs; i++)
        canonicalize_mod_input(binding.inputs[i]);
    if (binding.num_inputs < Synth::max_mod_inputs)
        memset(&binding.inputs[binding.num_inputs],
               0,
               (Synth::max_mod_inputs - binding.num_inputs) * sizeof(binding.inputs[0]));
}

static void canonicalize_effect(Synth::EffectSlotBinding& slot)
{
    // An unwired enable routing is zero by definition, so byte-comparable
    // snapshots hold across save/load.
    if (slot.enable_input.source == Synth::ModSource::none)
        memset(&slot.enable_input, 0, sizeof(slot.enable_input));
    else
        canonicalize_mod_input(slot.enable_input);

    const uint32_t num_params = Synth::get_effect_param_floats(slot.type);
    for (uint32_t p = 0; p < num_params; p++)
        canonicalize_binding(slot.bindings[p]);
    if (num_params < Synth::max_effect_param_floats)
        memset(&slot.bindings[num_params], 0, (Synth::max_effect_param_floats - num_params) * sizeof(slot.bindings[0]));

    // FIR band bindings normalize like the scalar ones.  A zeroed stored count stays
    // zero: it is the stored single-band marker the runtime reads as 1, and rewriting
    // it would break byte-comparable bank snapshots.  Bands beyond the active count are
    // dormant bytes and zero out, and a non-FIR slot's band block is stale bytes by
    // definition.  A count above the array bound is in-memory corruption; the local
    // clamp only keeps the dormant-zero walk in bounds.
    if (Synth::is_fir_effect_type(slot.type)) {
        uint32_t band_count = slot.fir_bands.band_count;
        if (band_count > Synth::max_fir_effect_bands)
            band_count = Synth::max_fir_effect_bands;
        // A fir_multiband slot reads the band array uniformly: every band of the
        // clamped count is active.  The plain FIR's count-1 band block is the
        // stored marker: zeroed it stays zero (byte-comparable snapshots), but an
        // authored band-0 gain (the count-1 encode rule) canonicalizes like any
        // active binding instead of being wiped with the dormant bytes.
        uint32_t active_band_params = Synth::fir_active_band_param_count(slot.type, band_count);
        if (slot.type == Synth::EffectType::fir && band_count <= 1) {
            const Synth::EffectParamBinding& band0 = slot.fir_bands.bands[0];
            active_band_params = band0.lfo_desc_id || band0.num_inputs || band0.base_value != 0.0f ? 1 : 0;
        }
        for (uint32_t band_param = 0; band_param < active_band_params; band_param++)
            canonicalize_binding(slot.fir_bands.bands[band_param]);
        if (active_band_params < Synth::num_fir_effect_band_params)
            memset(&slot.fir_bands.bands[active_band_params],
                   0,
                   (Synth::num_fir_effect_band_params - active_band_params) * sizeof(slot.fir_bands.bands[0]));
    }
    else {
        memset(&slot.fir_bands, 0, sizeof(slot.fir_bands));
    }
}

static void canonicalize_chain(Synth::EffectChainBinding& chain)
{
    for (uint32_t s = 0; s < chain.num_effects; s++)
        canonicalize_effect(chain.effects[s]);
    if (chain.num_effects < Synth::max_chain_effects)
        memset(&chain.effects[chain.num_effects],
               0,
               (Synth::max_chain_effects - chain.num_effects) * sizeof(chain.effects[0]));
}

// The standalone parameters pool carries no validated content (the runtime never
// reads it), so an entry with counts outside their arrays cannot be trusted: it is
// stale bytes by definition and canonicalizes to zero.
static bool param_counts_in_range(const Synth::ParamDescriptor& param)
{
    if (static_cast<uint32_t>(param.kind) >= num_param_kinds)
        return false;
    switch (param.kind) {
        case Synth::ParamKind::envelope:
            return param.envelope.num_points <= Synth::max_envelope_points;
        case Synth::ParamKind::plain:
            return param.plain.num_sources <= Synth::max_param_sources;
        default:
            return true;
    }
}

static void canonicalize_param(Synth::ParamDescriptor& param)
{
    if (! param_counts_in_range(param)) {
        memset(&param, 0, sizeof(param));
        return;
    }

    // The union's unused arm bytes are stale by construction; rebuild the entry
    // from its live member so the decoded image is arm-independent.
    Synth::ParamDescriptor fresh = {};
    switch (param.kind) {
        case Synth::ParamKind::envelope: {
            fresh.kind                     = param.kind;
            fresh.envelope                 = param.envelope;
            Synth::EnvelopeDescriptor& env = fresh.envelope;
            // The decoded flags carry only the two known bits; the mask keeps
            // union-arm rebuilds arm-independent.
            env.flags &= Synth::envelope_ping_pong | Synth::envelope_quick_release;
            env.min_value     = canonical_float(env.min_value);
            env.min_max_delta = canonical_float(env.min_max_delta);
            memset(&env.points[env.num_points],
                   0,
                   (Synth::max_envelope_points - env.num_points) * sizeof(env.points[0]));
            break;
        }
        case Synth::ParamKind::lfo: {
            fresh.kind                  = param.kind;
            fresh.lfo                   = param.lfo;
            fresh.lfo.lfo.min_value     = canonical_float(fresh.lfo.lfo.min_value);
            fresh.lfo.lfo.min_max_delta = canonical_float(fresh.lfo.lfo.min_max_delta);
            fresh.lfo.depth             = canonical_float(fresh.lfo.depth);
            fresh.lfo.rate_scale_ms     = canonical_float(fresh.lfo.rate_scale_ms);
            break;
        }
        case Synth::ParamKind::plain: {
            fresh.kind             = param.kind;
            fresh.plain            = param.plain;
            fresh.plain.base_value = canonical_float(fresh.plain.base_value);
            for (uint32_t s = 0; s < fresh.plain.num_sources; s++)
                fresh.plain.sources[s].scale = canonical_float(fresh.plain.sources[s].scale);
            if (fresh.plain.num_sources < Synth::max_param_sources)
                memset(&fresh.plain.sources[fresh.plain.num_sources],
                       0,
                       (Synth::max_param_sources - fresh.plain.num_sources) * sizeof(fresh.plain.sources[0]));
            break;
        }
        case Synth::ParamKind::external:
            fresh.kind = param.kind;
            break;
    }
    param = fresh;
}

// Brings an editor bank to the canonical form every successful decode
// produces.  Requires a validated bank (decode validates first).
static void canonicalize_editor_bank(Synth::InstrumentEditorBank* editor_bank)
{
    Synth::InstrumentBank& bank = editor_bank->bank;

    // Unused pool entries and name slots carry no information; zeroing them makes
    // the decoded image independent of the editing history that produced the file.
    for (uint32_t i = bank.instruments.num_allocated; i < Synth::max_instruments; i++) {
        memset(static_cast<void*>(&bank.instruments.entries[i]), 0, sizeof(bank.instruments.entries[i]));
        memset(editor_bank->instrument_names[i], 0, Synth::max_name_len);
    }
    for (uint32_t i = bank.envelopes.num_allocated; i < Synth::max_envelopes; i++)
        memset(&bank.envelopes.entries[i], 0, sizeof(bank.envelopes.entries[i]));
    for (uint32_t i = bank.lfos.num_allocated; i < Synth::max_lfos; i++)
        memset(&bank.lfos.entries[i], 0, sizeof(bank.lfos.entries[i]));
    for (uint32_t i = bank.parameters.num_allocated; i < Synth::max_parameters; i++)
        memset(&bank.parameters.entries[i], 0, sizeof(bank.parameters.entries[i]));

    for (uint32_t i = 0; i < bank.instruments.num_allocated; i++)
        canonicalize_name(editor_bank->instrument_names[i]);
    for (uint32_t c = 0; c < Synth::max_channels; c++) {
        canonicalize_name(editor_bank->channel_names[c]);
        canonicalize_name(editor_bank->channel_category[c]);
    }

    for (uint32_t i = 0; i < bank.envelopes.num_allocated; i++) {
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[i];
        env.flags                      = env.flags & (Synth::envelope_ping_pong | Synth::envelope_quick_release);
        env.min_value                  = canonical_float(env.min_value);
        env.min_max_delta              = canonical_float(env.min_max_delta);
        memset(&env.points[env.num_points], 0, (Synth::max_envelope_points - env.num_points) * sizeof(env.points[0]));
    }

    for (uint32_t i = 0; i < bank.lfos.num_allocated; i++) {
        Synth::LFODescriptor& lfo = bank.lfos.entries[i];
        lfo.min_value             = canonical_float(lfo.min_value);
        lfo.min_max_delta         = canonical_float(lfo.min_max_delta);
    }

    for (uint32_t i = 0; i < bank.parameters.num_allocated; i++)
        canonicalize_param(bank.parameters.entries[i]);

    for (uint32_t i = 0; i < bank.instruments.num_allocated; i++) {
        Synth::Instrument& instr   = bank.instruments.entries[i];
        instr.note_skew_semitones  = canonical_float(instr.note_skew_semitones);
        instr.layer_skew_semitones = canonical_float(instr.layer_skew_semitones);
        instr.release_seconds      = canonical_float(instr.release_seconds);

        for (uint32_t l = 0; l < instr.layer_count; l++) {
            Synth::Oscillator& osc                               = instr.layers[l];
            osc.mod_ratio                                        = canonical_float(osc.mod_ratio);
            osc.string_brightness                                = canonical_float(osc.string_brightness);
            osc.string_strike                                    = canonical_float(osc.string_strike);
            osc.targets[Synth::mod_string_brightness].base_value = 0.0f;
            osc.targets[Synth::mod_string_strike].base_value     = 0.0f;
            osc.string_drive                                     = canonical_float(osc.string_drive);
            osc.string_spectrum                                  = canonical_float(osc.string_spectrum);

            osc.string_dispersion_sections = canonical_float(osc.string_dispersion_sections);
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                Synth::TargetBinding& binding = osc.targets[t];
                binding.lfo_depth             = canonical_float(binding.lfo_depth);
                binding.envelope_depth        = canonical_float(binding.envelope_depth);
                binding.lfo_rate_scale_ms     = canonical_float(binding.lfo_rate_scale_ms);
                // Routing canonicalizes per layer: layers may diverge, so each
                // layer's base and inputs are cleaned independently.
                binding.base_value = canonical_float(binding.base_value);
                for (uint32_t j = 0; j < binding.num_inputs; j++) {
                    canonicalize_mod_input(binding.inputs[j]);
                }
                if (binding.num_inputs < Synth::max_mod_inputs) {
                    memset(&binding.inputs[binding.num_inputs],
                           0,
                           (Synth::max_mod_inputs - binding.num_inputs) * sizeof(binding.inputs[0]));
                }
            }
        }

        if (instr.layer_count < Synth::max_layers)
            memset(static_cast<void*>(&instr.layers[instr.layer_count]),
                   0,
                   (Synth::max_layers - instr.layer_count) * sizeof(instr.layers[0]));
    }

    for (uint32_t c = 0; c < Synth::max_channels; c++) {
        // Zone tables hold a live prefix terminated by the first empty slot; bytes
        // past the terminator are stale.
        Synth::Zone* const zones = bank.channel_zones[c];
        uint32_t           z     = 0;
        while (z < Synth::max_instr_per_channel && zones[z].start_note != 0)
            z++;
        memset(&zones[z], 0, (Synth::max_instr_per_channel - z) * sizeof(zones[0]));

        canonicalize_chain(bank.channel_chains[c]);
    }

    canonicalize_chain(bank.master_chain);
}

// Document-unit default for a target whose block states sources but
// omits base (base_set || sources_set gates the assignment in
// decode_target_doc), and the layer-0 preseed value: untouched
// instruments stay audible and centered like the factory default.
// Only layer 0 is pre-seeded to these defaults; later layers begin
// with zeroed bindings, so a later-layer block that states neither
// base nor sources keeps the zeroed base.  A wholly absent fm_ratio
// block instead inherits the layer's mod_ratio scalar
// (decode_layer_doc).
static float doc_base_default(Synth::ModTarget target)
{
    if (target == Synth::mod_volume)
        return 1.0f;
    if (target == Synth::mod_panning)
        return 0.5f;
    if (target == Synth::mod_fm_ratio)
        return 1.0f;
    return 0.0f;
}

// A value is in the documented range: [min, max] where cutoffs additionally
// allow exactly 0 (bypass); the gap below the slider floor has no meaning.
static bool doc_value_valid(const Sculptor::OscTargetView& view, float value)
{
    return (value >= view.min_value || value == 0.0f) && value <= view.max_value;
}

// Unknown instrument fields are reported and ignored; loading continues.
static void doc_unknown_field(Walker& w, uint32_t key_idx)
{
    unknown_field(w, key_idx);
}

// Point levels are the envelope's output values in the target's display units;
// the descriptor stores them scaled to bank units (fm_index: radians).
// Sustain defaults to the last point.
static void decode_envelope_doc(Walker&                    w,
                                uint32_t                   obj,
                                Synth::ModTarget           target,
                                Synth::FilterType          fir_type,
                                Synth::EnvelopeDescriptor& env,
                                float*                     amount,
                                uint16_t*                  group_id)
{
    const Sculptor::OscTargetView view = Sculptor::osc_target_view(target, fir_type);

    float    times[Synth::max_envelope_points]     = {};
    float    levels[Synth::max_envelope_points]    = {};
    uint16_t quantized[Synth::max_envelope_points] = {};
    uint32_t num_points                            = 0;
    uint32_t sustain_start                         = 0;
    uint32_t sustain_end                           = 0;
    float    depth                                 = 1.0f;
    bool     sustain_start_set                     = false;
    bool     sustain_end_set                       = false;
    uint8_t  flags                                 = 0;
    uint32_t encoded_id                            = 0;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "points")) {
            num_points = static_cast<uint32_t>(w.toks[val_idx].size);
            walk_array(w, val_idx, Synth::max_envelope_points, [&](uint32_t elem, uint32_t pos) {
                float time      = 0.0f;
                float level     = 0.0f;
                bool  time_set  = false;
                bool  level_set = false;
                walk_object(w, elem, [&](uint32_t point_key, uint32_t point_val) {
                    if (key_eq(w, point_key, "time_seconds")) {
                        want_float(w, point_val, &time);
                        time_set = true;
                    }
                    else if (key_eq(w, point_key, "level")) {
                        want_float(w, point_val, &level);
                        level_set = true;
                    }
                    else
                        doc_unknown_field(w, point_key);
                });
                if (! w.failed && (! time_set || ! level_set))
                    w.failed = true;
                times[pos]  = time;
                levels[pos] = level;
            });
        }
        else if (key_eq(w, key_idx, "sustain_start")) {
            want_uint(w, val_idx, Synth::max_envelope_points - 1, &sustain_start);
            sustain_start_set = true;
        }
        else if (key_eq(w, key_idx, "sustain_end")) {
            want_uint(w, val_idx, Synth::max_envelope_points - 1, &sustain_end);
            sustain_end_set = true;
        }
        else if (key_eq(w, key_idx, "ping_pong"))
            decode_envelope_flag(w, val_idx, Synth::envelope_ping_pong, &flags);
        else if (key_eq(w, key_idx, "quick_release"))
            decode_envelope_flag(w, val_idx, Synth::envelope_quick_release, &flags);
        else if (key_eq(w, key_idx, "depth"))
            want_float(w, val_idx, &depth);
        else if (key_eq(w, key_idx, "envelope_id"))
            want_uint(w, val_idx, 0xFFFFu, &encoded_id);
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed || num_points == 0) {
        w.failed = true; // points are required: a one-point envelope is the minimum
        return;
    }

    // Quantized positions define the curve, so ordering is checked on ticks: a
    // leading run of position 0 is the descriptor's unplaced-point layout and
    // stays valid; every placed point must advance on its predecessor.
    const float max_time_seconds = Sculptor::envelope_ticks_to_ms(0xFFFF) / 1000.0f;
    for (uint32_t i = 0; i < num_points; i++) {
        if (times[i] < 0.0f || times[i] > max_time_seconds || ! std::isfinite(levels[i])) {
            w.failed = true;
            return;
        }
        const uint16_t position = Sculptor::envelope_ms_to_ticks(times[i] * 1000.0f);
        if (i > 0 && (position < quantized[i - 1] || (position == quantized[i - 1] && position != 0))) {
            w.failed = true;
            return;
        }
        quantized[i] = position;
    }

    // With ping-pong off, the effective sustain span's last point is
    // forced to the first point's value so the span loops seamlessly.
    // The effective span uses the unset defaults (the last point)
    // because the raw fields are defaulted only later; normalizing a
    // decode that later fails validation is harmless.
    const uint32_t start_eff = sustain_start_set ? sustain_start : num_points - 1;
    const uint32_t end_eff   = sustain_end_set ? sustain_end : num_points - 1;
    if ((flags & Synth::envelope_ping_pong) == 0 && end_eff > start_eff) {
        levels[end_eff] = levels[start_eff];
    }

    float min_level = levels[0];
    float max_level = levels[0];
    for (uint32_t i = 1; i < num_points; i++) {
        if (levels[i] < min_level)
            min_level = levels[i];
        if (levels[i] > max_level)
            max_level = levels[i];
    }

    // Sustain defaults to the last point.
    if (! sustain_start_set)
        sustain_start = num_points - 1;
    if (! sustain_end_set)
        sustain_end = num_points - 1;
    if (sustain_start > sustain_end || sustain_end >= num_points) {
        w.failed = true;
        return;
    }

    // The runtime evaluates min_value + raw_value * min_max_delta, so the delta
    // spans the level range across the 16-bit raw values, and both fields are
    // stored in bank units (fm_index: radians).
    env.min_value     = min_level * view.bank_scale;
    env.min_max_delta = (max_level - min_level) * view.bank_scale / 65535.0f;
    if (! std::isfinite(env.min_value) || ! std::isfinite(env.min_max_delta)) {
        w.failed = true;
        return;
    }
    env.num_points          = static_cast<uint8_t>(num_points);
    env.sustain_first_point = static_cast<uint8_t>(sustain_start);
    env.flags               = flags;
    env.sustain_last_point  = static_cast<uint8_t>(sustain_end);

    // A flat envelope (delta 0) is the constant min: every quantized value 0.
    const float delta = max_level - min_level;
    for (uint32_t i = 0; i < num_points; i++) {
        env.points[i].position = quantized[i];
        env.points[i].value =
            delta > 0.0f ? static_cast<uint16_t>((levels[i] - min_level) / delta * 65535.0f + 0.5f) : 0;
    }
    if (amount) {
        *amount = depth;
    }
    if (group_id) {
        *group_id = static_cast<uint16_t>(encoded_id);
    }
}

// Unknown mod-input fields are reported and ignored; "source" is required.
static void decode_mod_input_doc(Walker& w, uint32_t obj, Synth::ModInput& input)
{
    bool source_set = false;
    decode_mod_input_fields(w, obj, input, &source_set, doc_unknown_field);
    if (w.failed || ! source_set)
        w.failed = true;
}

// One LFO generator bound to a target. The wave and frequency are required;
// everything else has a working default.
static void decode_lfo_doc(Walker&               w,
                           uint32_t              obj,
                           Synth::ModTarget      target,
                           Synth::FilterType     fir_type,
                           Synth::LFODescriptor& lfo,
                           Synth::TargetBinding& gen,
                           uint16_t*             group_id)
{
    const Sculptor::OscTargetView view          = Sculptor::osc_target_view(target, fir_type);
    uint32_t                      value         = 0;
    float                         frequency_hz  = 0.0f;
    bool                          frequency_set = false;
    bool                          wave_set      = false;
    float                         duty          = 0.0f;
    float                         min_level     = view.min_value;
    float                         max_level     = view.max_value;
    uint32_t                      op            = 0;
    float                         depth         = 0.0f;
    uint32_t                      depth_source  = 0;
    uint32_t                      rate_source   = 0;
    float                         rate_scale_ms = 0.0f;
    uint32_t                      encoded_id    = 0;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "wave")) {
            uint32_t wave = 0;
            want_enum(w, val_idx, wave_names, num_waves, &wave);
            if (! w.failed && wave != static_cast<uint32_t>(Synth::WaveType::sine_wave) &&
                wave != static_cast<uint32_t>(Synth::WaveType::sawtooth_wave)) {
                w.failed = true; // the bank pool only holds sine and sawtooth LFOs
            }
            wave_set = true;
            lfo.wave = static_cast<Synth::WaveType>(wave);
        }
        else if (key_eq(w, key_idx, "frequency_hz")) {
            want_float(w, val_idx, &frequency_hz);
            frequency_set = true;
        }
        else if (key_eq(w, key_idx, "duty"))
            want_float(w, val_idx, &duty);
        else if (key_eq(w, key_idx, "min_level"))
            want_float(w, val_idx, &min_level);
        else if (key_eq(w, key_idx, "max_level"))
            want_float(w, val_idx, &max_level);
        else if (key_eq(w, key_idx, "op"))
            want_enum(w, val_idx, source_op_names, num_source_ops, &op);
        else if (key_eq(w, key_idx, "depth"))
            want_float(w, val_idx, &depth);
        else if (key_eq(w, key_idx, "depth_source"))
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &depth_source);
        else if (key_eq(w, key_idx, "rate_source"))
            want_enum(w, val_idx, mod_source_names, num_mod_sources, &rate_source);
        else if (key_eq(w, key_idx, "rate_scale_ms"))
            want_float(w, val_idx, &rate_scale_ms);
        else if (key_eq(w, key_idx, "lfo_id"))
            want_uint(w, val_idx, 0xFFFFu, &encoded_id);
        else if (key_eq(w, key_idx, "pulse_mix")) {
            // Garbage-tolerant like its siblings: accept a wider bound, then
            // clamp instead of failing the whole decode.
            want_uint(w, val_idx, 0xFFFF, &value);
            lfo.pulse_mix = static_cast<uint8_t>(value > 255 ? 255 : value);
        }
        else if (key_eq(w, key_idx, "beat_div")) {
            want_uint(w, val_idx, 0xFF, &value);
            lfo.beat_div = (value <= 5) ? static_cast<uint8_t>(value) : 0;
        }
        else if (key_eq(w, key_idx, "start_point")) {
            want_uint(w, val_idx, 0xFF, &value);
            lfo.start_point = (value <= 2) ? static_cast<uint8_t>(value) : 0;
        }
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed || ! wave_set || ! frequency_set || frequency_hz <= 0.0f) {
        w.failed = true;
        return;
    }

    // period_ms is a uint16, so the frequency must land in 1 ms..65535 ms.
    const float period_ms = 1000.0f / frequency_hz;
    if (period_ms < 1.0f || period_ms > 65535.0f || duty < 0.0f || duty > 1.0f || min_level > max_level ||
        ! std::isfinite(min_level) || ! std::isfinite(max_level)) {
        w.failed = true;
        return;
    }

    lfo.duty      = static_cast<uint8_t>(duty * 255.0f + 0.5f);
    lfo.period_ms = static_cast<uint16_t>(period_ms + 0.5f);
    // The runtime sweeps min_value..min_value+delta with a normalized wave, so
    // the levels store bank units directly (fm_index: radians).
    lfo.min_value     = min_level * view.bank_scale;
    lfo.min_max_delta = (max_level - min_level) * view.bank_scale;
    if (! std::isfinite(lfo.min_value) || ! std::isfinite(lfo.min_max_delta)) {
        w.failed = true;
        return;
    }

    gen.lfo_op            = static_cast<Synth::SourceOp>(op);
    gen.lfo_depth         = depth;
    gen.lfo_depth_source  = static_cast<Synth::ModSource>(depth_source);
    gen.lfo_rate_source   = static_cast<Synth::ModSource>(rate_source);
    gen.lfo_rate_scale_ms = rate_scale_ms;
    if (group_id) {
        *group_id = static_cast<uint16_t>(encoded_id);
    }
}

// An instrument's layers rarely need many distinct descriptors; reusing the
// pool slot of an identical one keeps pasted instruments compact. The
// descriptors are zero-initialized before decoding, so memcmp compares only
// written content.
template <typename Descriptor>
static uint16_t find_matching_descriptor(const Descriptor* descriptors, uint32_t count, const Descriptor& candidate)
{
    for (uint32_t i = 0; i < count; i++) {
        if (memcmp(&descriptors[i], &candidate, sizeof(Descriptor)) == 0)
            return static_cast<uint16_t>(i + 1);
    }
    return 0;
}

// One target block: one layer's routing (base, sources) plus that layer's
// generators (envelope, LFO).  Every layer owns its routing, so base and
// sources stage straight into the decoding layer's binding.
static void decode_target_doc(Walker&             w,
                              uint32_t            obj,
                              Synth::ModTarget    target,
                              Synth::FilterType   fir_type,
                              Synth::Oscillator&  osc,
                              InstrumentDocState& doc,
                              bool*               touched)
{
    const Sculptor::OscTargetView view = Sculptor::osc_target_view(target, fir_type);

    float base        = target == Synth::mod_string_brightness ? 1.0f : doc_base_default(target);
    bool  base_set    = false;
    bool  sources_set = false;
    if (touched) {
        *touched = true;
    }

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "base")) {
            // The spread base is slider-bounded (semitones or Hz by filter type),
            // validated before the binary32 narrowing; the FIR slope base is an
            // integer percent 1..100 like the layer scalar; every other target
            // keeps the generic free-level parsing.
            if (target == Synth::mod_string_brightness || target == Synth::mod_string_strike)
                want_bounded_real(w, val_idx, 0.0, 1.0, &base);
            else if (target == Synth::mod_fir_spread)
                want_bounded_real(w, val_idx, 0.0, static_cast<double>(view.max_value), &base);
            else if (target == Synth::mod_fir_slope) {
                int32_t base_int = 0;
                want_int(w, val_idx, 1, 100, &base_int);
                // want_int rejects negative spellings and out-of-range
                // magnitudes outright; this explicit range check stays as
                // defense in depth.
                if (base_int < 1 || base_int > 100)
                    w.failed = true;
                else
                    base = static_cast<float>(base_int);
            }
            else
                want_float(w, val_idx, &base);
            base_set = true;
        }
        else if (key_eq(w, key_idx, "envelope")) {
            if (doc.envelope_count >= Synth::instrument_max_envelopes) {
                w.failed = true;
                return;
            }
            Synth::EnvelopeDescriptor& env = doc.envelopes[doc.envelope_count];
            env                            = Synth::EnvelopeDescriptor{};
            float    env_depth             = 1.0f;
            uint16_t group_id              = 0;
            decode_envelope_doc(w, val_idx, target, fir_type, env, &env_depth, &group_id);
            if (! w.failed) {
                uint16_t desc_id = 0;
                if (group_id != 0) {
                    // The encoded pool id is authoritative: bindings carrying the
                    // same id share one descriptor even when another descriptor
                    // holds identical content. Stored text carries no ids and
                    // keeps the content-equality grouping below.
                    uint32_t found = doc.envelope_count;
                    for (uint32_t i = 0; i < doc.envelope_count; i++) {
                        if (doc.envelope_ids[i] == group_id) {
                            found = i;
                            break;
                        }
                    }
                    if (found == doc.envelope_count) {
                        doc.envelope_ids[doc.envelope_count] = group_id;
                        ++doc.envelope_count;
                    }
                    desc_id = static_cast<uint16_t>(found + 1);
                }
                else {
                    const uint16_t match = find_matching_descriptor(doc.envelopes, doc.envelope_count, env);
                    if (match == 0) {
                        doc.envelope_ids[doc.envelope_count] = 0;
                        ++doc.envelope_count;
                        desc_id = static_cast<uint16_t>(doc.envelope_count);
                    }
                    else {
                        desc_id = match;
                    }
                }
                osc.targets[target].envelope_desc_id = desc_id;
                osc.targets[target].envelope_depth   = env_depth;
            }
        }
        else if (key_eq(w, key_idx, "lfo")) {
            if (doc.lfo_count >= Synth::instrument_max_lfos) {
                w.failed = true;
                return;
            }
            Synth::LFODescriptor& lfo = doc.lfos[doc.lfo_count];
            lfo                       = Synth::LFODescriptor{};
            uint16_t group_id         = 0;
            decode_lfo_doc(w, val_idx, target, fir_type, lfo, osc.targets[target], &group_id);
            if (! w.failed) {
                uint16_t desc_id = 0;
                if (group_id != 0) {
                    // The encoded pool id is authoritative, like the envelope id
                    // above: identical content alone cannot decide the grouping.
                    uint32_t found = doc.lfo_count;
                    for (uint32_t i = 0; i < doc.lfo_count; i++) {
                        if (doc.lfo_ids[i] == group_id) {
                            found = i;
                            break;
                        }
                    }
                    if (found == doc.lfo_count) {
                        doc.lfo_ids[doc.lfo_count] = group_id;
                        ++doc.lfo_count;
                    }
                    desc_id = static_cast<uint16_t>(found + 1);
                }
                else {
                    const uint16_t match = find_matching_descriptor(doc.lfos, doc.lfo_count, lfo);
                    if (match == 0) {
                        doc.lfo_ids[doc.lfo_count] = 0;
                        ++doc.lfo_count;
                        desc_id = static_cast<uint16_t>(doc.lfo_count);
                    }
                    else {
                        desc_id = match;
                    }
                }
                osc.targets[target].lfo_desc_id = desc_id;
            }
        }
        else if (key_eq(w, key_idx, "sources")) {
            walk_array(w, val_idx, Synth::max_mod_inputs, [&](uint32_t elem, uint32_t pos) {
                decode_mod_input_doc(w, elem, osc.targets[target].inputs[pos]);
            });
            osc.targets[target].num_inputs = static_cast<uint16_t>(w.toks[val_idx].size);
            sources_set                    = true;
        }
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed) {
        return;
    }

    if (! doc_value_valid(view, base)) {
        w.failed = true;
        return;
    }

    if (target == Synth::mod_string_brightness || target == Synth::mod_string_strike) {
        if (target == Synth::mod_string_brightness) {
            osc.string_brightness = base;
        }
        else {
            osc.string_strike = base;
        }
    }
    else if (base_set || sources_set) {
        osc.targets[target].base_value = base * view.bank_scale;
    }
    if (! base_set && ! sources_set &&
        (target == Synth::mod_fir_band0_gain || target == Synth::mod_fir_band1_gain ||
         target == Synth::mod_fir_band2_gain || target == Synth::mod_fir_band3_gain)) {
        // The explicit-empty-block convention: a present gain block with an
        // omitted base states the gain-0 band, overriding the 1.0 absent default.
        osc.targets[target].base_value = 0.0f;
    }
}

// One oscillator layer.  wave_a is the one required field.
static void decode_layer_doc(Walker&                     w,
                             uint32_t                    obj,
                             Synth::Oscillator&          osc,
                             InstrumentDocState&         doc,
                             Synth::InstrumentJsonDomain domain)
{
    uint32_t wave_a            = 0;
    bool     wave_a_set        = false;
    uint32_t wave_b            = 0;
    uint32_t mode              = 0;
    float    mod_ratio         = 1.0f;
    bool     fm_ratio_set      = false;
    float    string_brightness = 1.0f;

    float string_dispersion_sections                            = 4.0f;
    osc.string_drive                                            = 3.0f;
    osc.string_spectrum                                         = 2.0f;
    bool                    doc_slope_routed                    = false;
    uint32_t                doc_band_count                      = 1;
    bool                    doc_touched[Synth::num_mod_targets] = {};
    float                   pitch_offset                        = 0.0f;
    int32_t                 octave_offset                       = 0;
    int32_t                 fir_slope                           = 100;
    const Synth::FilterType fir_type                            = prescan_fir_type(w, obj);
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "wave_a")) {
            want_enum(w, val_idx, wave_names, num_waves, &wave_a);
            wave_a_set = true;
        }
        else if (key_eq(w, key_idx, "wave_b")) {
            want_enum(w, val_idx, wave_names, num_waves, &wave_b);
            if (wave_b > static_cast<uint32_t>(Synth::WaveType::pad_wave))
                w.failed = true; // string is wave A only
        }
        else if (key_eq(w, key_idx, "string_brightness")) {
            if (w.toks[val_idx].type == JSMN_OBJECT) {
                decode_target_doc(w,
                                  val_idx,
                                  Synth::mod_string_brightness,
                                  fir_type,
                                  osc,
                                  doc,
                                  &doc_touched[Synth::mod_string_brightness]);
            }
            else {
                want_bounded_real(w, val_idx, 0.0, 1.0, &string_brightness);
            }
        }
        else if (key_eq(w, key_idx, "string_dispersion") && w.toks[val_idx].type != JSMN_OBJECT)
            want_bounded_real(w, val_idx, 0.0, 1.0, &osc.targets[Synth::mod_string_dispersion].base_value);
        else if (key_eq(w, key_idx, "string_strike")) {
            if (w.toks[val_idx].type == JSMN_OBJECT) {
                decode_target_doc(w,
                                  val_idx,
                                  Synth::mod_string_strike,
                                  fir_type,
                                  osc,
                                  doc,
                                  &doc_touched[Synth::mod_string_strike]);
            }
            else {
                want_bounded_real(w, val_idx, 0.0, 1.0, &osc.string_strike);
            }
        }
        else if (key_eq(w, key_idx, "string_drive"))
            want_bounded_real(w, val_idx, 0.5, 10.0, &osc.string_drive);
        else if (key_eq(w, key_idx, "string_spectrum"))
            want_bounded_real(w, val_idx, 1.5, 4.0, &osc.string_spectrum);
        else if (key_eq(w, key_idx, "string_dispersion_sections"))
            want_dispersion_sections(w, val_idx, &string_dispersion_sections);
        else if (key_eq(w, key_idx, "mode"))
            want_enum(w, val_idx, osc_mode_names, num_osc_modes, &mode);
        else if (key_eq(w, key_idx, "mod_ratio"))
            want_float(w, val_idx, &mod_ratio);
        else if (key_eq(w, key_idx, "pitch_offset_semitones"))
            want_float(w, val_idx, &pitch_offset);
        else if (key_eq(w, key_idx, "octave_offset"))
            want_int(w, val_idx, Sculptor::osc_octave_offset_min, Sculptor::osc_octave_offset_max, &octave_offset);
        else if (key_eq(w, key_idx, "fir_slope")) {
            // The FIR Slope target block shares the layer scalar's key: the token
            // kind disambiguates (an object is the routing block, a number the
            // stored scalar).
            if (w.toks[val_idx].type == JSMN_OBJECT) {
                decode_target_doc(w,
                                  val_idx,
                                  Synth::mod_fir_slope,
                                  fir_type,
                                  osc,
                                  doc,
                                  &doc_touched[static_cast<uint32_t>(Synth::mod_fir_slope)]);
            }
            else {
                want_int(w, val_idx, 1, 100, &fir_slope);
            }
        }
        else if (key_eq(w, key_idx, "fir_type")) {
            // Read by prescan_fir_type before the target blocks.
        }
        else if (key_eq(w, key_idx, "fir_bands")) {
            // The multi-band count: absent decodes as the stored single band; bands
            // beyond the count decode but stay dormant (the re-encode skips them).
            want_uint(w, val_idx, 4, &doc_band_count);
            if (doc_band_count < 1) {
                w.failed = true;
            }
        }
        else {
            uint32_t target = 0;
            while (target < Synth::num_mod_targets && ! key_eq(w, key_idx, mod_target_names[target]))
                target++;
            if (target < Synth::num_mod_targets) {
                // Monotonic like the bank decoder's fm_ratio_routed: an
                // explicit fm_ratio block stays authoritative no matter which
                // target keys follow it.
                if (target == static_cast<uint32_t>(Synth::mod_fm_ratio))
                    fm_ratio_set = true;
                decode_target_doc(w,
                                  val_idx,
                                  static_cast<Synth::ModTarget>(target),
                                  fir_type,
                                  osc,
                                  doc,
                                  &doc_touched[target]);
            }
            else if (key_eq(w, key_idx, "routing")) {
                // Bank-style routing object, accepted as a portable alias so a routing block can
                // share a layer with the fir_slope scalar key without colliding.
                decode_routing_object(w,
                                      val_idx,
                                      fir_type,
                                      osc,
                                      fm_ratio_set,
                                      doc_slope_routed,
                                      doc_touched,
                                      doc_unknown_field);
            }
            else
                doc_unknown_field(w, key_idx);
        }
    });

    if (w.failed || ! wave_a_set)
        w.failed = true;
    if (w.failed)
        return;

    // An explicitly written zero ratio is bank-legal (blend ignores the ratio
    // and FM/sync banks can hold one); clipboard nonzero ratios are slider-bounded.
    if (domain == Synth::InstrumentJsonDomain::clipboard && mod_ratio != 0.0f &&
        (mod_ratio < Sculptor::osc_fm_ratio_min || mod_ratio > Sculptor::osc_fm_ratio_max)) {
        w.failed = true;
        return;
    }

    // Pitch offsets are bounded in every domain: a bank holding an
    // out-of-range offset could never round-trip through the editor.
    if (pitch_offset < Sculptor::osc_pitch_offset_min || pitch_offset > Sculptor::osc_pitch_offset_max) {
        w.failed = true;
        return;
    }

    osc.osc_type[0] = static_cast<Synth::WaveType>(wave_a);
    osc.osc_type[1] = static_cast<Synth::WaveType>(wave_b);
    osc.osc_mode    = static_cast<Synth::OscMode>(mode);
    osc.mod_ratio   = mod_ratio;
    if (! doc_touched[Synth::mod_string_brightness]) {
        osc.string_brightness = string_brightness;
    }

    osc.string_dispersion_sections = string_dispersion_sections;
    // Stored documents state only the scalar ratio; when the document has
    // no fm_ratio block the target inherits it, clamped to the slider
    // range when nonzero (0 stays 0).
    if (! fm_ratio_set) {
        osc.targets[Synth::mod_fm_ratio].base_value = clamp_stored_mod_ratio(mod_ratio);
    }
    // Stored pitch offset folds into the pitch target's base value;
    // the combined static trim stays inside the pitch editing range.
    const Sculptor::OscTargetView pitch_view = Sculptor::osc_target_view(Synth::mod_pitch);
    osc.targets[Synth::mod_pitch].base_value = vmath::clamp(osc.targets[Synth::mod_pitch].base_value + pitch_offset,
                                                            pitch_view.min_value,
                                                            pitch_view.max_value);
    osc.octave_offset                        = static_cast<int8_t>(octave_offset);
    // want_int rejects negative spellings and out-of-range magnitudes
    // outright; this explicit range check stays as defense in depth.
    if (fir_slope < 1 || fir_slope > 100) {
        w.failed = true;
    }
    osc.fir_slope_percent = static_cast<uint8_t>(fir_slope);
    osc.fir_type          = fir_type;
    osc.fir_band_count    = static_cast<uint8_t>(doc_band_count < 1u ? 1u : doc_band_count);

    // Per-band absent defaults apply to every layer, band 0's gain
    // included: the routing alias and the generator keys mark their
    // targets, so a stated gain or slope survives and the absent ones
    // decode the audible defaults.
    apply_fir_band_absent_defaults(osc, doc_touched, fir_type);
}

// One root object, nothing before or after it, grammar-clean everywhere.
static bool parse_document(const char* text, uint32_t len, Walker& w)
{
    if (len > bank_json_text_size) {
        return false;
    }

    jsmn_parser parser;
    jsmn_init(&parser);

    const int num_tokens = jsmn_parse(&parser,
                                      text,
                                      len,
                                      json_tokens,
                                      static_cast<unsigned int>(sizeof(json_tokens) / sizeof(json_tokens[0])));
    if (num_tokens <= 0) {
        return false;
    }

    w.doc      = text;
    w.len      = len;
    w.toks     = json_tokens;
    w.num_toks = static_cast<uint32_t>(num_tokens);
    w.failed   = false;

    // The document-wide grammar pass runs before any schema decoding, so
    // malformed content is rejected everywhere - including unknown subtrees the
    // schema walk never visits.
    const uint32_t grammar_next = check_value(w, 0);
    if (grammar_next != w.num_toks || w.failed || w.toks[0].type != JSMN_OBJECT) {
        return false;
    }

    // Nothing may precede the root object: jsmn consumes separators without a
    // root, so ",{}" would otherwise pass as an empty document.
    if (! gap_is_ws(text, 0, static_cast<uint32_t>(w.toks[0].start))) {
        return false;
    }
    for (uint32_t i = static_cast<uint32_t>(w.toks[0].end); i < len; i++) {
        if (! is_ws(text[i])) {
            return false;
        }
    }

    return true;
}

// Decode pipeline into decode_scratch: grammar pass, schema overlay
// onto the defaults, then descriptor validation.

static bool decode_document_into_scratch(const char* text,
                                         uint32_t    len,
                                         bool        strict_editor_metadata = false,
                                         bool        lenient                = false,
                                         bool        guarded                = false)
{
    skipped_count = 0;
    Walker w      = {};
    if (! parse_document(text, len, w)) {
        return false;
    }

    w.strict_editor_metadata = strict_editor_metadata;
    w.lenient                = lenient;
    w.guarded                = guarded;
    memset(damaged_channels, 0, sizeof(damaged_channels));
    memset(damaged_instruments, 0, sizeof(damaged_instruments));
    memset(damaged_envelopes, 0, sizeof(damaged_envelopes));
    memset(damaged_lfos, 0, sizeof(damaged_lfos));
    memset(damaged_parameters, 0, sizeof(damaged_parameters));
    memset(damaged_zones, 0, sizeof(damaged_zones));
    memset(damaged_effects, 0, sizeof(damaged_effects));
    memset(damaged_layouts, 0, sizeof(damaged_layouts));
    damaged_entry = nullptr;
    memset(file_pool_slots, 0, sizeof(file_pool_slots));
    memset(file_zone_ids, 0, sizeof(file_zone_ids));
    memset(file_layout_ids, 0, sizeof(file_layout_ids));
    memset(salvage_identity, 0, sizeof(salvage_identity));
    memset(zone_present, 0, sizeof(zone_present));
    memset(zone_invalid, 0, sizeof(zone_invalid));
    fill_default_editor_bank(&decode_scratch);

    bool      routing_present = false;
    bool      editor_seen     = false;
    uint32_t  idx             = 1;
    const int pairs           = w.toks[0].size; // the root object counts its key-value pairs

    for (int p = 0; p < pairs && ! w.failed; p++) {
        const uint32_t key_idx = idx;
        const uint32_t val_idx = idx + 1;
        idx                    = skip_value(w, val_idx);
        if (idx == invalid_index) {
            w.failed = true;
            break;
        }
        if (key_eq(w, key_idx, "instrument_editor_bank")) {
            if (w.toks[val_idx].type == JSMN_PRIMITIVE && tok_text_eq(w, val_idx, "null")) {
                unknown_field(w, key_idx);
            }
            else {
                decode_bank(w, val_idx, &decode_scratch, &routing_present, &editor_seen);
            }
        }
        else if (key_eq(w, key_idx, "editor")) {
            decode_editor_state(w, val_idx, &decode_scratch, &routing_present, &editor_seen);
        }
        else
            unknown_field(w, key_idx);
    }

    if (w.failed) {
        return false;
    }

    if (lenient ? ! salvage_resolve_bank_ids(decode_scratch) : ! resolve_bank_ids(decode_scratch))
        return false;
    for (uint32_t owner = 0; owner <= Synth::max_channels; ++owner) {
        Synth::EffectChainBinding& chain =
            owner < Synth::max_channels ? decode_scratch.bank.channel_chains[owner] : decode_scratch.bank.master_chain;
        Sculptor::EffectChainRouting& routing = decode_scratch.effect_routing[owner];
        normalize_bypass_routing(routing);
        Sculptor::fx_compact_chain(&chain, &routing);
    }

    if (! Synth::validate_instrument_bank(&decode_scratch.bank, lenient)) {
        return false;
    }
    // Editor metadata needs the decoded pools (descriptor ranges) and the
    // full zone tables (implied node counts), so it validates after the walk.
    if (! Sculptor::validate_editor_metadata(decode_scratch)) {
        return false;
    }
    canonicalize_editor_bank(&decode_scratch);
    return Sculptor::normalize_effect_layout(&decode_scratch);
}

// Encodes the bank into the static json_text buffer and verifies that the
// document reloads through the bounded decode path.  All failure modes happen
// here, before the caller touches the destination file, so a refused save
// never creates or truncates anything.
static int prepare_editor_bank_json(const Synth::InstrumentEditorBank* bank, uint32_t* out_len)
{
    const uint32_t len = encode_editor_bank_json(bank, json_text, sizeof(json_text));

    if (! len)
        return EOVERFLOW;

    // Reload guarantee: the document must parse back through the same bounded
    // buffers the next load will use; a document the codec itself cannot reload
    // is refused instead of written.
    if (! decode_document_into_scratch(json_text, len))
        return EINVAL;

    *out_len = len;

    return 0;
}

int Synth::write_editor_bank_json(FILE* file, const InstrumentEditorBank* bank, uint32_t max_len, uint32_t* out_len)
{
    uint32_t len = 0;

    const int error = prepare_editor_bank_json(bank, &len);
    if (error)
        return error;

    if (len > max_len)
        return EOVERFLOW;

    if (fwrite(json_text, 1, len, file) != len)
        return EIO;

    *out_len = len;

    return 0;
}

bool Synth::read_editor_bank_json(FILE* file, uint32_t len, InstrumentEditorBank* out, bool strict_editor_metadata)
{
    if (len > sizeof(json_text))
        return false;

    if (fread(json_text, 1, len, file) != len)
        return false;

    if (! decode_document_into_scratch(json_text, len, strict_editor_metadata))
        return false;
    *out = decode_scratch;
    return true;
}

bool Synth::decode_editor_bank_json(const char* text, uint32_t len, InstrumentEditorBank* out)
{
    if (! decode_document_into_scratch(text, len))
        return false;

    memcpy(out, &decode_scratch, sizeof(*out));

    return true;
}

bool Synth::decode_instrument_json(const char*         text,
                                   uint32_t            len,
                                   Instrument*         out_instr,
                                   EnvelopeDescriptor* out_envelopes,
                                   uint32_t*           out_envelope_count,
                                   LFODescriptor*      out_lfos,
                                   uint32_t*           out_lfo_count)
{
    uint32_t layout_count = 0;
    return decode_instrument_json(text,
                                  len,
                                  out_instr,
                                  out_envelopes,
                                  out_envelope_count,
                                  out_lfos,
                                  out_lfo_count,
                                  nullptr,
                                  0,
                                  &layout_count);
}

bool Synth::decode_instrument_json(const char*            text,
                                   uint32_t               len,
                                   Instrument*            out_instr,
                                   EnvelopeDescriptor*    out_envelopes,
                                   uint32_t*              out_envelope_count,
                                   LFODescriptor*         out_lfos,
                                   uint32_t*              out_lfo_count,
                                   InstrumentGraphLayout* out_layout,
                                   uint32_t               layout_capacity,
                                   uint32_t*              out_layout_count)
{
    return decode_instrument_json_for_domain(text,
                                             len,
                                             out_instr,
                                             out_envelopes,
                                             out_envelope_count,
                                             out_lfos,
                                             out_lfo_count,
                                             out_layout,
                                             layout_capacity,
                                             out_layout_count,
                                             InstrumentJsonDomain::clipboard);
}

bool Synth::decode_instrument_json_for_domain(const char*            text,
                                              uint32_t               len,
                                              Instrument*            out_instr,
                                              EnvelopeDescriptor*    out_envelopes,
                                              uint32_t*              out_envelope_count,
                                              LFODescriptor*         out_lfos,
                                              uint32_t*              out_lfo_count,
                                              InstrumentGraphLayout* out_layout,
                                              uint32_t               layout_capacity,
                                              uint32_t*              out_layout_count,
                                              InstrumentJsonDomain   domain,
                                              uint8_t*               osc_source_out,
                                              uint16_t*              envelope_ids_out,
                                              uint16_t*              lfo_ids_out)
{
    if (! text || ! out_instr || ! out_envelopes || ! out_envelope_count || ! out_lfos || ! out_lfo_count ||
        ! out_layout_count)
        return false;
    Walker w = {};
    if (! parse_document(text, len, w))
        return false;

    InstrumentDocState& doc = instrument_doc;
    doc.instr               = Synth::Instrument{};
    for (uint32_t target = 0; target < num_mod_targets; target++) {
        const Sculptor::OscTargetView view = Sculptor::osc_target_view(static_cast<ModTarget>(target));
        // Layer 0 keeps the document defaults for targets whose blocks omit
        // base; later layers decode an omitted routing as the zeroed binding.
        // A wholly absent fm_ratio block instead inherits the layer's scalar
        // (decode_layer_doc).
        doc.instr.layers[0].targets[target].base_value =
            doc_base_default(static_cast<ModTarget>(target)) * view.bank_scale;
    }
    doc.envelope_count = 0;
    doc.lfo_count      = 0;

    bool     format_ok                          = false;
    bool     layers_seen                        = false;
    uint32_t layer_count                        = 0;
    uint32_t layout_count                       = 0;
    uint8_t  decoded_sources[Synth::max_layers] = {};
    bool     sources_seen                       = false;

    walk_object(w, 0, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "format")) {
            if (! tok_text_eq(w, val_idx, instrument_format_tag))
                record_ignored("unrecognized instrument format tag, loading anyway");
            format_ok = true;
        }
        else if (key_eq(w, key_idx, "note_skew_semitones")) {
            want_float(w, val_idx, &doc.instr.note_skew_semitones);
            if (! w.failed && (doc.instr.note_skew_semitones < 0.0f || doc.instr.note_skew_semitones > 1.0f))
                w.failed = true;
        }
        else if (key_eq(w, key_idx, "layer_skew_semitones")) {
            want_float(w, val_idx, &doc.instr.layer_skew_semitones);
            if (! w.failed && (doc.instr.layer_skew_semitones < 0.0f || doc.instr.layer_skew_semitones > 1.0f))
                w.failed = true;
        }
        else if (key_eq(w, key_idx, "release_seconds")) {
            want_release_seconds(w, val_idx, &doc.instr.release_seconds);
        }
        else if (key_eq(w, key_idx, "pitch_bend_lock")) {
            bool lock = true;
            want_bool(w, val_idx, &lock);
            doc.instr.pitch_bend_lock = lock ? 1 : 0;
        }
        else if (key_eq(w, key_idx, "voices")) {
            want_voice_mode(w, val_idx, &doc.instr.voice_mode);
        }
        else if (key_eq(w, key_idx, "layers")) {
            layers_seen = true;
            layer_count = static_cast<uint32_t>(w.toks[val_idx].size);
            walk_array(w, val_idx, Synth::max_layers, [&](uint32_t elem, uint32_t pos) {
                decode_layer_doc(w, elem, doc.instr.layers[pos], doc, domain);
            });
        }
        else if (key_eq(w, key_idx, "graph_layout")) {
            // Record ordering: every record is decoded and classified before
            // any capacity accounting, so unregistered kinds are skipped by
            // the generic path and the portable capacity only ever counts
            // registered records.
            const jsmntok_t& arr = w.toks[val_idx];
            if (arr.type != JSMN_ARRAY) {
                w.failed = true;
            }
            else {
                uint32_t  idx    = val_idx + 1;
                uint32_t  staged = 0;
                const int elems  = arr.size;
                for (int element_index = 0; element_index < elems && ! w.failed; element_index++) {
                    const uint32_t elem = idx;
                    idx                 = skip_value(w, elem);
                    if (idx == invalid_index) {
                        w.failed = true;
                        break;
                    }

                    // The staged name outlives the walk lambda: the
                    // kind-gated copy back runs after the object walk.
                    const uint32_t named_kind = graph_record_kind(w, elem, graph_kind_names, 5);
                    if (named_kind >= 5)
                        continue;
                    bool names_known = true;
                    walk_object(w, elem, [&](uint32_t key, uint32_t value) {
                        const bool target = key_eq(w, key, "target");
                        const bool source = key_eq(w, key, "depth_source") || key_eq(w, key, "rate_source");
                        if (! target && ! source)
                            return;
                        const uint32_t     count = target ? Synth::num_mod_targets : 7;
                        const char* const* names = target ? mod_target_names : mod_source_names;
                        for (uint32_t i = 0; i < count; ++i)
                            if (tok_text_eq(w, value, names[i]))
                                return;
                        unknown_field(w, key);
                        names_known = false;
                    });
                    if (! names_known)
                        continue;
                    InstrumentGraphLayout record = {};
                    record.kind                  = named_kind < 3 ? 0 : static_cast<uint8_t>(named_kind - 2);
                    record.canonical_index       = named_kind < 2 ? static_cast<uint8_t>(named_kind) : 2;
                    char     staged_name[sizeof(record.name) + 1] = {};
                    uint32_t present                              = 0;
                    walk_object(w, elem, [&](uint32_t field, uint32_t value) {
                        const char* const keys[] = { "kind",        "x",      "y",
                                                     "width",       "height", "canonical_index",
                                                     "layer",       "target", "depth_source",
                                                     "rate_source", "name" };
                        uint32_t          key    = 0;
                        while (key < 11 && ! key_eq(w, field, keys[key])) {
                            ++key;
                        }
                        if (key == 11 || key == 5) {
                            unknown_field(w, field);
                            return;
                        }

                        present |= 1u << key;
                        if (key == 10) {
                            want_text(w, value, staged_name, sizeof(staged_name));
                        }
                        else if (key >= 1 && key <= 4) {
                            float number = 0;
                            want_float(w, value, &number);
                            if (key == 1) {
                                record.x = number;
                            }
                            if (key == 2) {
                                record.y = number;
                            }
                            if (key == 3) {
                                record.width_override = number;
                            }
                            if (key == 4) {
                                record.height_override = number;
                            }
                        }
                        else if (key == 0) {
                        }
                        else if (key == 7 || key == 8 || key == 9) {
                            uint32_t number = 0;
                            want_enum(w,
                                      value,
                                      key == 7 ? mod_target_names : mod_source_names,
                                      key == 7 ? Synth::num_mod_targets : 7,
                                      &number);
                            if (key == 7)
                                record.target = static_cast<uint8_t>(number);
                            if (key == 8)
                                record.depth_source = static_cast<uint8_t>(number);
                            if (key == 9)
                                record.rate_source = static_cast<uint8_t>(number);
                        }
                        else {
                            uint8_t number = 0;
                            want_u8(w, value, 255, &number);
                            if (key == 6) {
                                record.layer = number;
                                if (named_kind == 2) {
                                    if (number >= Synth::max_layers) {
                                        w.failed = true;
                                        return;
                                    }
                                    record.canonical_index = static_cast<uint8_t>(number + 2);
                                }
                            }
                        }
                    });
                    if (w.failed) {
                        break;
                    }

                    if (! stage_record_name(record.name,
                                            sizeof(record.name),
                                            staged_name,
                                            sizeof(staged_name),
                                            record.kind)) {
                        w.failed = true;
                        break;
                    }

                    // Unregistered kinds are skipped before capacity counts them.
                    if (record.kind > 2) {
                        continue;
                    }

                    const uint32_t locator_keys[] = { named_kind == 2 ? 1u << 6 : 0u,
                                                      (1u << 6) | (1u << 7),
                                                      (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) };
                    if ((present & ~(1u << 10)) != (31u | locator_keys[record.kind])) {
                        w.failed = true;
                        break;
                    }

                    if (staged >= Synth::instrument_graph_layout_capacity) {
                        w.failed = true;
                        break;
                    }

                    instrument_layout_staging[staged++] = record;
                }
                layout_count = staged;
            }
        }
        else if (key_eq(w, key_idx, "osc_sources")) {
            if (domain != Synth::InstrumentJsonDomain::clipboard || ! object_array(w, val_idx)) {
                doc_unknown_field(w, key_idx);
                return;
            }
            uint8_t row[Synth::max_layers] = {};
            if (! decode_source_wiring(w, val_idx, row)) {
                doc_unknown_field(w, key_idx);
                return;
            }
            sources_seen = true;
            memcpy(decoded_sources, row, sizeof(row));
        }
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed) {
        return false;
    }
    if (! format_ok)
        record_ignored("missing instrument format tag, loading anyway");
    if (! layers_seen) {
        return false;
    }
    if (layer_count == 0) {
        return false;
    }

    doc.instr.layer_count = layer_count;

    if (sources_seen) {
        // A repeated ordinal is accepted: the compile copies the referenced
        // layer into every wired slot in slot order, so duplicates stay
        // deterministic.  Values above the layer count still refuse.
        for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
            const uint8_t value = decoded_sources[slot];
            if (value != 0 && value > layer_count) {
                return false;
            }
        }
    }
    else {
        for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
            decoded_sources[slot] = slot < layer_count ? static_cast<uint8_t>(slot + 1) : 0;
        }
    }

    uint32_t mapped_count = 0;
    if (layout_count > layout_capacity || (! out_layout && layout_count) ||
        ! Sculptor::map_instrument_graph_layout(doc.instr,
                                                doc.envelopes,
                                                doc.envelope_count,
                                                doc.lfos,
                                                doc.lfo_count,
                                                instrument_layout_staging,
                                                layout_count,
                                                instrument_mapped_staging,
                                                Synth::instrument_graph_layout_capacity,
                                                &mapped_count)) {
        return false;
    }
    // Only now, with the whole document proven valid, do the caller's outputs
    // change: every failure path above leaves them untouched.
    memcpy(out_instr, &doc.instr, sizeof(*out_instr));
    memcpy(out_envelopes, doc.envelopes, doc.envelope_count * sizeof(*out_envelopes));
    memcpy(out_lfos, doc.lfos, doc.lfo_count * sizeof(*out_lfos));
    *out_envelope_count = doc.envelope_count;
    *out_lfo_count      = doc.lfo_count;
    if (layout_count)
        memcpy(out_layout, instrument_layout_staging, layout_count * sizeof(*out_layout));
    *out_layout_count = layout_count;
    if (osc_source_out) {
        memcpy(osc_source_out, decoded_sources, Synth::max_layers);
    }
    // The document's descriptor id per decoded position (0 for stored
    // content-dedup text): callers whose id maps are keyed by pool id need
    // the translation these ids provide when pool ids and document positions
    // no longer coincide.
    if (envelope_ids_out) {
        memcpy(envelope_ids_out, doc.envelope_ids, doc.envelope_count * sizeof(*envelope_ids_out));
    }
    if (lfo_ids_out) {
        memcpy(lfo_ids_out, doc.lfo_ids, doc.lfo_count * sizeof(*lfo_ids_out));
    }
    return true;
}

static uint32_t encode_instrument_json_for_domain(char*                               dest,
                                                  uint32_t                            dest_size,
                                                  const Synth::Instrument*            instr,
                                                  const Synth::InstrumentBank*        desc_bank,
                                                  const Synth::InstrumentGraphLayout* layout,
                                                  uint32_t                            layout_count,
                                                  Synth::InstrumentJsonDomain         domain,
                                                  const uint8_t*                      osc_source = nullptr);

uint32_t Synth::encode_instrument_json(char*                 dest,
                                       uint32_t              dest_size,
                                       const Instrument*     instr,
                                       const InstrumentBank* desc_bank)
{
    return encode_instrument_json(dest, dest_size, instr, desc_bank, nullptr, 0);
}

uint32_t Synth::encode_instrument_json(char*                        dest,
                                       uint32_t                     dest_size,
                                       const Instrument*            instr,
                                       const InstrumentBank*        desc_bank,
                                       const InstrumentGraphLayout* layout,
                                       uint32_t                     layout_count)
{
    return encode_instrument_json_for_domain(dest,
                                             dest_size,
                                             instr,
                                             desc_bank,
                                             layout,
                                             layout_count,
                                             InstrumentJsonDomain::clipboard);
}

static uint32_t encode_instrument_json_for_domain(char*                               dest,
                                                  uint32_t                            dest_size,
                                                  const Synth::Instrument*            instr,
                                                  const Synth::InstrumentBank*        desc_bank,
                                                  const Synth::InstrumentGraphLayout* layout,
                                                  uint32_t                            layout_count,
                                                  Synth::InstrumentJsonDomain         domain,
                                                  const uint8_t*                      osc_source)
{
    if (! dest || ! instr || ! desc_bank || (! layout && layout_count) ||
        layout_count > Synth::instrument_graph_layout_capacity || dest_size == 0)
        return 0;

    if (layout_count) {
        const uint32_t content_length =
            encode_instrument_json_for_domain(dest, dest_size, instr, desc_bank, nullptr, 0, domain, osc_source);
        uint32_t   envelope_count = 0;
        uint32_t   lfo_count      = 0;
        uint32_t   mapped_count   = 0;
        const bool dec_ok = Synth::decode_instrument_json_for_domain(dest,
                                                                     content_length,
                                                                     &decode_scratch.bank.instruments.entries[0],
                                                                     decode_scratch.bank.envelopes.entries,
                                                                     &envelope_count,
                                                                     decode_scratch.bank.lfos.entries,
                                                                     &lfo_count,
                                                                     nullptr,
                                                                     0,
                                                                     &mapped_count,
                                                                     domain);

        const bool map_ok = dec_ok && Sculptor::map_instrument_graph_layout(decode_scratch.bank.instruments.entries[0],
                                                                            decode_scratch.bank.envelopes.entries,
                                                                            envelope_count,
                                                                            decode_scratch.bank.lfos.entries,
                                                                            lfo_count,
                                                                            layout,
                                                                            layout_count,
                                                                            instrument_mapped_staging,
                                                                            Synth::instrument_graph_layout_capacity,
                                                                            &mapped_count);

        if (! content_length || ! dec_ok || ! map_ok) {
            return 0;
        }
    }
    if (instr->layer_count == 0 || instr->layer_count > Synth::max_layers)
        return 0;

    Out o = { dest, dest_size - 1, 0, false, false };

    o.ch('{');
    o.key("format");
    o.ch('"');
    o.str(instrument_format_tag);
    o.ch('"');
    o.key("note_skew_semitones");
    o.float_value(instr->note_skew_semitones);
    o.key("layer_skew_semitones");
    o.float_value(instr->layer_skew_semitones);
    key_float_nonzero(o, "release_seconds", instr->release_seconds);
    key_enum(o, "voices", voice_mode_names, num_voice_modes, instr->voice_mode);
    o.key("pitch_bend_lock");
    o.bool_value(instr->pitch_bend_lock != 0);
    o.key("layers");
    o.ch('[');

    for (uint32_t layer = 0; layer < instr->layer_count; layer++) {
        const Synth::Oscillator& osc = instr->layers[layer];
        if (static_cast<uint32_t>(osc.osc_type[0]) >= num_waves || static_cast<uint32_t>(osc.osc_type[1]) >= num_waves)
            return 0;
        if (osc.osc_mode > Synth::osc_mode_ring_mod)
            return 0;

        o.sep();
        o.ch('{');
        o.key("wave_a");
        o.ch('"');
        o.str(wave_names[static_cast<uint32_t>(osc.osc_type[0])]);
        o.ch('"');
        key_enum(o, "wave_b", wave_names, num_waves, static_cast<uint32_t>(osc.osc_type[1]));
        key_enum(o, "mode", osc_mode_names, num_osc_modes, static_cast<uint32_t>(osc.osc_mode));
        // Omitted ratios decode as one, including dormant blend ratios.
        o.key("mod_ratio");
        o.float_value(osc.mod_ratio);
        // Full brightness is the default and stays out of the document.
        if (osc.string_brightness != 1.0f && routing_is_default(osc.targets[Synth::mod_string_brightness]) &&
            gen_is_default(osc.targets[Synth::mod_string_brightness])) {
            key_float(o, "string_brightness", osc.string_brightness);
        }
        if (routing_is_default(osc.targets[Synth::mod_string_strike]) &&
            gen_is_default(osc.targets[Synth::mod_string_strike])) {
            key_float_nonzero(o, "string_strike", osc.string_strike);
        }
        if (osc.string_drive != 3.0f) {
            key_float(o, "string_drive", osc.string_drive);
        }
        if (osc.string_spectrum != 2.0f) {
            key_float(o, "string_spectrum", osc.string_spectrum);
        }
        key_dispersion(o, osc);
        o.key("octave_offset");
        o.float_value(static_cast<float>(osc.octave_offset));
        // The FIR Slope target block shares the scalar's key, so the two never appear
        // together in one layer object (the grammar pass refuses duplicate keys): a
        // routed layer states the block and drops the dormant scalar, an unrouted one
        // states the scalar alone and keeps its zeroed binding.  "Routed" is the same
        // binding-presence rule the runtime and the display use, so dormant generator
        // fields alone never drop the scalar.
        const Synth::TargetBinding& slope_binding = osc.targets[Synth::mod_fir_slope];
        const bool                  slope_routed  = Synth::osc_fir_slope_binding_present(slope_binding);
        if (! slope_routed && osc.fir_slope_percent != 100) {
            o.key("fir_slope");
            o.uint_value(osc.fir_slope_percent);
        }
        key_fir_type(o, osc);
        // The count key rides only multi-band layers (the absent count decodes
        // as the stored single band).
        if (osc.fir_band_count > 1) {
            o.key("fir_bands");
            o.uint_value(osc.fir_band_count);
        }

        for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
            const Synth::ModTarget        mod_target = static_cast<Synth::ModTarget>(target);
            const Sculptor::OscTargetView view       = Sculptor::osc_target_view(mod_target);

            if (Sculptor::osc_target_band_index(target) >=
                static_cast<int32_t>(osc.fir_band_count < 1u ? 1u : osc.fir_band_count)) {
                continue; // a band beyond the active count never leaks into the document
            }

            if ((mod_target == Synth::mod_string_brightness || mod_target == Synth::mod_string_strike) &&
                routing_is_default(osc.targets[target]) && gen_is_default(osc.targets[target])) {
                continue;
            }
            if (mod_target == Synth::mod_string_dispersion && ! dispersion_routed(osc.targets[target])) {
                continue; // the unmodulated value rides the plain "string_dispersion" number above
            }
            if ((mod_target == Synth::mod_string_damping || mod_target == Synth::mod_string_release_damping) &&

                routing_is_default(osc.targets[target]) && gen_is_default(osc.targets[target])) {

                continue; // an unused string target states nothing, so old documents re-encode unchanged
            }

            if (mod_target == Synth::mod_fir_slope && ! slope_routed) {
                // Zeroed slope binding: the scalar key above carries the value, and
                // omitting the block is lossless (the layer-0 default base is 0).
                continue;
            }

            o.key(mod_target_names[target]);
            o.ch('{');

            const Synth::TargetBinding& routing = osc.targets[target];

            // Routing is per layer.  Layer 0 always states its base: the
            // decoder pre-seeds layer 0 with the document defaults, so an
            // omitted base there could not express a legitimate zero.  Later
            // layers state routing only when it is not the zeroed binding.
            // Exception: the FIR slope target decodes an omitted base as 0
            // (its own legitimate zero), so a base-0 routed slope block omits
            // the key and reloads with the same zero instead of stating a
            // base its integer grammar rejects.
            if (mod_target == Synth::mod_string_brightness || mod_target == Synth::mod_string_strike) {
                key_float(o,
                          "base",
                          mod_target == Synth::mod_string_brightness ? osc.string_brightness : osc.string_strike);
            }
            else if (layer == 0 || routing.base_value != 0.0f || routing.num_inputs != 0) {
                // A gain-0 band must survive the round trip as an inactive band: the
                // empty block marks "present, base omitted" and decodes 0, so the
                // zero base is never stated for a gain key.
                const bool band_gain_zero =
                    routing.base_value == 0.0f &&
                    (mod_target == Synth::mod_fir_band0_gain || mod_target == Synth::mod_fir_band1_gain ||
                     mod_target == Synth::mod_fir_band2_gain || mod_target == Synth::mod_fir_band3_gain);
                if ((mod_target != Synth::mod_fir_slope || routing.base_value != 0.0f) && ! band_gain_zero) {
                    o.key("base");
                    o.float_value(routing.base_value / view.bank_scale);
                }
            }
            if (routing.num_inputs > Synth::max_mod_inputs)
                return 0;
            if (routing.num_inputs != 0) {
                o.key("sources");
                o.ch('[');
                for (uint32_t input = 0; input < routing.num_inputs; input++) {
                    const Synth::ModInput& in = routing.inputs[input];
                    if (in.op > Synth::SourceOp::multiply)
                        return 0;
                    if (in.source == Synth::ModSource::cc) {
                        // The assignable cc source is admitted with
                        // cc_number 16..31, like the bank validator.
                        if (! Synth::cc_is_assignable(in.cc_number))
                            return 0;
                    }
                    else if (in.source > Synth::ModSource::pressure_combine || in.cc_number != 0)
                        // A non-CC input must carry no stale CC number
                        // (canonicalization hole).
                        return 0;
                    o.sep();
                    enc_mod_input(o, in);
                }
                o.ch(']');
            }

            const Synth::TargetBinding& gen = osc.targets[target];

            const uint16_t env_id = gen.envelope_desc_id;
            if (env_id != 0) {
                if (env_id > desc_bank->envelopes.num_allocated)
                    return 0;
                const Synth::EnvelopeDescriptor& env = desc_bank->envelopes.entries[env_id - 1];
                if (env.num_points == 0 || env.num_points > Synth::max_envelope_points)
                    return 0;
                o.key("envelope");
                o.ch('{');
                // The pool id groups bindings that share one descriptor; identical
                // content alone cannot: two distinct envelopes may hold the same
                // points, and the distinction must survive the round trip.
                key_uint_nonzero(o, "envelope_id", env_id);
                o.key("points");
                o.ch('[');
                for (uint32_t point = 0; point < env.num_points; point++) {
                    o.sep();
                    o.ch('{');
                    o.key("time_seconds");
                    o.float_value(Sculptor::envelope_ticks_to_ms(env.points[point].position) / 1000.0f);
                    o.key("level");
                    o.float_value((env.min_value + static_cast<float>(env.points[point].value) * env.min_max_delta) /
                                  view.bank_scale);
                    o.ch('}');
                }
                o.ch(']');
                o.key("sustain_start");
                o.uint_value(env.sustain_first_point);
                o.key("sustain_end");
                o.uint_value(env.sustain_last_point);
                key_uint_nonzero(o, "ping_pong", (env.flags & Synth::envelope_ping_pong) != 0);
                key_uint_nonzero(o, "quick_release", (env.flags & Synth::envelope_quick_release) != 0);
                // The depth rides beside the shape; 1 is the default and omits the key.
                if (gen.envelope_depth != 1.0f) {
                    o.key("depth");
                    o.float_value(gen.envelope_depth);
                }
                o.ch('}');
            }

            const uint16_t lfo_id = gen.lfo_desc_id;
            if (lfo_id != 0) {
                if (lfo_id > desc_bank->lfos.num_allocated)
                    return 0;
                const Synth::LFODescriptor& lfo = desc_bank->lfos.entries[lfo_id - 1];
                if (lfo.wave > Synth::WaveType::noise_wave || lfo.period_ms == 0 ||
                    gen.lfo_op > Synth::SourceOp::multiply ||
                    gen.lfo_depth_source > Synth::ModSource::pressure_combine ||
                    gen.lfo_rate_source > Synth::ModSource::pressure_combine)
                    return 0;
                o.key("lfo");
                o.ch('{');
                key_uint_nonzero(o, "lfo_id", lfo_id);
                key_enum(o, "wave", wave_names, num_waves, static_cast<uint32_t>(lfo.wave));
                o.key("frequency_hz");
                o.float_value(1000.0f / static_cast<float>(lfo.period_ms));
                o.key("duty");
                o.float_value(static_cast<float>(lfo.duty) / 255.0f);
                o.key("min_level");
                o.float_value(lfo.min_value / view.bank_scale);
                o.key("max_level");
                o.float_value((lfo.min_value + lfo.min_max_delta) / view.bank_scale);
                key_uint_nonzero(o, "pulse_mix", lfo.pulse_mix);
                key_uint_nonzero(o, "beat_div", lfo.beat_div);
                key_uint_nonzero(o, "start_point", lfo.start_point);
                key_enum(o, "op", source_op_names, num_source_ops, static_cast<uint32_t>(gen.lfo_op));
                o.key("depth");
                o.float_value(gen.lfo_depth);
                key_enum(o,
                         "depth_source",
                         mod_source_names,
                         num_mod_sources,
                         static_cast<uint32_t>(gen.lfo_depth_source));
                key_enum(o,
                         "rate_source",
                         mod_source_names,
                         num_mod_sources,
                         static_cast<uint32_t>(gen.lfo_rate_source));
                o.key("rate_scale_ms");
                o.float_value(gen.lfo_rate_scale_ms);
                o.ch('}');
            }

            o.ch('}');
        }

        o.ch('}');
    }

    o.ch(']');

    if (domain == Synth::InstrumentJsonDomain::clipboard) {
        o.key("osc_sources");
        o.ch('[');
        for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
            const uint8_t layer = osc_source                  ? osc_source[slot]
                                  : slot < instr->layer_count ? static_cast<uint8_t>(slot + 1)
                                                              : 0;
            if (! layer)
                continue;
            o.sep();
            o.ch('{');
            key_uint(o, "slot", slot);
            key_uint(o, "layer", layer);
            o.ch('}');
        }
        o.ch(']');
    }
    o.key("graph_layout");
    o.ch('[');
    for (uint32_t index = 0; index < layout_count; ++index) {
        const Synth::InstrumentGraphLayout& record = layout[index];
        o.sep();
        o.ch('{');
        const uint32_t kind =
            record.kind == 0 ? (record.canonical_index < 2 ? record.canonical_index : 2u) : record.kind + 2u;
        o.key("kind");
        o.text_value(graph_kind_names[kind], 16);
        if (kind == 2)
            key_uint(o, "layer", record.canonical_index - 2u);
        if (record.kind == 1 || record.kind == 2)
            key_uint(o, "layer", record.layer);
        if (record.kind != 0) {
            o.key("target");
            o.text_value(mod_target_names[record.target], 32);
        }
        if (record.kind == 2) {
            if (record.depth_source >= 7 || record.rate_source >= 7) {
                o.overflow = true;
                return 0;
            }
            o.key("depth_source");
            o.text_value(mod_source_names[record.depth_source], 32);
            o.key("rate_source");
            o.text_value(mod_source_names[record.rate_source], 32);
        }
        o.key("x");
        o.float_value(record.x);
        o.key("y");
        o.float_value(record.y);
        o.key("width");
        o.float_value(record.width_override);
        o.key("height");
        o.float_value(record.height_override);
        o.ch('}');
    }
    o.ch(']');
    o.ch('}');

    if (o.overflow || o.invalid)
        return 0;
    return o.pos;
}

static Synth::BankGuard::State bank_guard_state = Synth::BankGuard::State::open;

void Synth::BankGuard::reset()
{
    bank_guard_state = State::open;
}

Synth::BankGuard::State Synth::BankGuard::state()
{
    return bank_guard_state;
}

bool Synth::bank_save_allowed()
{
    return bank_guard_state == BankGuard::State::open;
}

Synth::BankLoadResult Synth::load_editor_bank_guarded(const char*           path,
                                                      const char*           backup_path,
                                                      InstrumentEditorBank* out)
{
    BankLoadResult result{ BankLoadStatus::unusable, 0, BankBackupStatus::none };
    skipped_count = 0;
    struct stat before;
#ifdef _WIN32
    const int exists = stat(path, &before);
#else
    const int exists = lstat(path, &before);
#endif
    if (exists && errno == ENOENT) {
        result.status = BankLoadStatus::absent;
        return result;
    }
    bool loaded        = false;
    bool strict_failed = true;
    bool regular       = exists == 0 && (before.st_mode & S_IFMT) == S_IFREG;
    if (regular) {
        FILE* const file = fopen(path, "rb");
        if (file) {
            const size_t got      = fread(json_text, 1, sizeof(json_text), file);
            const int    extra    = got == sizeof(json_text) ? fgetc(file) : EOF;
            const bool   readable = ! ferror(file);
            const bool   closed   = fclose(file) == 0;
            if (readable && closed && extra == EOF) {
                loaded = decode_document_into_scratch(json_text, static_cast<uint32_t>(got), false, false, true);
                if (loaded && ! validate_instrument_bank(&decode_scratch.bank, true))
                    loaded = false;
                strict_failed = ! loaded;
                if (! loaded || skipped_count) {
                    ignored_count = 0;
                    loaded = decode_document_into_scratch(json_text, static_cast<uint32_t>(got), false, true, true);
                }
                if (loaded) {
                    bool surviving =
                        decode_scratch.bank.instruments.num_allocated || decode_scratch.bank.envelopes.num_allocated ||
                        decode_scratch.bank.lfos.num_allocated || decode_scratch.bank.parameters.num_allocated ||
                        decode_scratch.bank.master_chain.num_effects;
                    for (uint32_t c = 0; c < max_channels; ++c)
                        surviving |= decode_scratch.bank.channel_chains[c].num_effects ||
                                     (decode_scratch.bank.channel_enabled[c] &&
                                      decode_scratch.bank.channel_zones[c][0].start_note);
                    if ((strict_failed || skipped_count) && ! surviving)
                        loaded = false;
                }
            }
        }
    }
    result.skipped = skipped_count;
    if (loaded && ! strict_failed && ! result.skipped) {
        result.status = BankLoadStatus::ok;
        memcpy(out, &decode_scratch, sizeof(*out));
        return result;
    }
    result.status = loaded ? BankLoadStatus::salvaged : BankLoadStatus::unusable;
    struct stat backup;
#ifdef _WIN32
    // GetFileAttributes sees entries, including reparse points, without a write.
    const DWORD attributes    = GetFileAttributesA(backup_path);
    const bool  backup_exists = attributes != INVALID_FILE_ATTRIBUTES;
    const bool  backup_absent = ! backup_exists && GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    const int  backup_stat   = lstat(backup_path, &backup);
    const bool backup_exists = backup_stat == 0;
    const bool backup_absent = backup_stat != 0 && errno == ENOENT;
#endif
    (void)backup;
    if (backup_exists) {
        bank_guard_state = BankGuard::State::locked;
        result.status    = BankLoadStatus::locked;
        result.backup    = BankBackupStatus::pre_existing;
        return result;
    }
    if (! backup_absent)
        result.backup = BankBackupStatus::failed;
    else {
        const RenameResult renamed = rename_no_replace(path, backup_path);
        if (renamed == RenameResult::renamed)
            result.backup = BankBackupStatus::renamed;
        else if (renamed == RenameResult::renamed_partial)
            result.backup = BankBackupStatus::renamed_partial;
        else if (errno == EEXIST) {
            bank_guard_state = BankGuard::State::locked;
            result.status    = BankLoadStatus::locked;
            result.backup    = BankBackupStatus::pre_existing;
            return result;
        }
        else
            result.backup = BankBackupStatus::failed;
    }
    if (result.backup != BankBackupStatus::renamed && bank_guard_state != BankGuard::State::locked)
        bank_guard_state = BankGuard::State::blocked;
    if (loaded)
        memcpy(out, &decode_scratch, sizeof(*out));
    else {
        fill_default_editor_bank(out);
        init_default_bank(&out->bank);
        out->effect_routing[0].in            = Sculptor::effect_in_output;
        out->effect_routing[max_channels].in = Sculptor::effect_in_output;
        for (uint32_t z = 0; z < max_instr_per_channel; ++z)
            Sculptor::set_identity_osc_sources(out, 0, z);
    }
    return result;
}

int Synth::save_editor_bank_file(const char* path, const InstrumentEditorBank* bank)
{
    if (! bank_save_allowed())
        return EACCES;
    uint32_t len = 0;

    const int error = prepare_editor_bank_json(bank, &len);
    if (error)
        return error;

    d_printf("Saving instrument bank %s\n", path);

    FILE* const file = fopen(path, "wb");
    if (! file)
        return errno;

    if (fwrite(json_text, 1, len, file) != len) {
        fclose(file);
        return EIO;
    }

    if (fclose(file))
        return errno;

    return 0;
}

Synth::BankFileStatus Synth::load_editor_bank_file(const char* path, InstrumentEditorBank* out)
{
    FILE* const file = fopen(path, "rb");
    if (! file)
        return errno == ENOENT ? BankFileStatus::absent : BankFileStatus::invalid;

    const size_t got       = fread(json_text, 1, sizeof(json_text), file);
    const bool   too_large = got == sizeof(json_text) && fgetc(file) != EOF;
    // A short read from an I/O error must not masquerade as a short document.
    const bool stream_error = ferror(file) != 0;
    const bool close_error  = fclose(file) != 0;
    if (stream_error || close_error)
        return BankFileStatus::invalid;
    if (too_large)
        return BankFileStatus::too_large;

    const uint32_t len = static_cast<uint32_t>(got);

    // Any non-JSON document, or one whose root is not an object, is rejected by
    // the decoder below.
    if (! decode_editor_bank_json(json_text, len, out))
        return BankFileStatus::invalid;

    // A bank saved by a buggy build may carry routing shapes that no longer
    // validate (the one-output-feed invariant, dangling runs). Sanitize the
    // routing instead of refusing: a refusal would push the editor to the
    // default bank, and the next auto-save would overwrite the file.
    for (uint32_t owner = 0; owner <= Synth::max_channels; ++owner) {
        Synth::EffectChainBinding& chain =
            owner < Synth::max_channels ? out->bank.channel_chains[owner] : out->bank.master_chain;
        Sculptor::fx_sanitize_routing(&chain, &out->effect_routing[owner]);
    }
    return BankFileStatus::ok;
    return BankFileStatus::invalid;
}

uint32_t Sculptor::encode_editor_instrument_json(const Synth::InstrumentEditorBank& source,
                                                 uint32_t                           channel,
                                                 uint32_t                           zone,
                                                 char*                              dest,
                                                 uint32_t                           capacity,
                                                 Synth::InstrumentJsonDomain        domain)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel ||
        ! Synth::validate_instrument_bank(&source.bank) || ! Sculptor::validate_editor_metadata(source))
        return 0;
    const Synth::Zone& entry = source.bank.channel_zones[channel][zone];
    if (! entry.start_note || entry.instrument >= source.bank.instruments.num_allocated)
        return 0;
    // The clipboard carries the authoring instrument; the portable
    // domains carry the playback form, which drops unwired oscillators
    // permanently.  A zone with no wired slot compiles to zero layers;
    // the zero-layer refusal below refuses it.
    const Synth::Instrument& stored = source.bank.instruments.entries[entry.instrument];
    Synth::Instrument        playback_form;
    const Synth::Instrument* instrument = &stored;
    if (domain != Synth::InstrumentJsonDomain::clipboard) {
        if (! Sculptor::compile_playback_instrument(stored, source.graph_osc_source[channel][zone], &playback_form)) {
            return 0;
        }
        if (playback_form.layer_count == 0) {
            return 0;
        }
        instrument = &playback_form;
    }

    uint32_t       env_count    = 0;
    uint32_t       lfo_count    = 0;
    uint32_t       source_count = 0;
    uint32_t       count        = 0;
    const uint32_t length       = encode_instrument_json_for_domain(
        editor_export_text,
        sizeof(editor_export_text),
        instrument,
        &source.bank,
        nullptr,
        0,
        domain,
        domain == Synth::InstrumentJsonDomain::clipboard ? source.graph_osc_source[channel][zone] : nullptr);
    if (! length) {
        return 0;
    }

    if (! Synth::decode_instrument_json_for_domain(editor_export_text,
                                                   length,
                                                   &editor_export_decoded,
                                                   editor_export_envelopes,
                                                   &env_count,
                                                   editor_export_lfos,
                                                   &lfo_count,
                                                   nullptr,
                                                   0,
                                                   &count,
                                                   domain,
                                                   editor_export_osc_source))
        return 0;
    if (! Sculptor::encode_instrument_graph_layout(*instrument,
                                                   source.bank,
                                                   source,
                                                   channel,
                                                   zone,
                                                   editor_export_source_layout,
                                                   Synth::instrument_graph_layout_capacity,
                                                   &source_count,
                                                   domain))
        return 0;
    if (! Sculptor::normalize_instrument_graph_layout(*instrument,
                                                      source.bank.envelopes.entries,
                                                      source.bank.envelopes.num_allocated,
                                                      source.bank.lfos.entries,
                                                      source.bank.lfos.num_allocated,
                                                      editor_export_source_layout,
                                                      source_count,
                                                      editor_export_decoded,
                                                      editor_export_envelopes,
                                                      env_count,
                                                      editor_export_lfos,
                                                      lfo_count,
                                                      editor_export_normalized,
                                                      Synth::instrument_graph_layout_capacity,
                                                      &count))
        return 0;
    const uint32_t named_length = encode_instrument_json_for_domain(
        editor_export_text,
        sizeof(editor_export_text),
        instrument,
        &source.bank,
        editor_export_normalized,
        count,
        domain,
        domain == Synth::InstrumentJsonDomain::clipboard ? source.graph_osc_source[channel][zone] : nullptr);
    if (! named_length || ! dest || capacity <= named_length)
        return 0;
    memcpy(dest, editor_export_text, named_length);
    dest[named_length] = 0;
    return named_length;
}

bool Synth::validate_effects_document(const EffectsDocument* document)
{
    if (! document || document->lfo_count > max_lfos || document->graph_layout_count > effects_graph_layout_capacity) {
        return false;
    }
    memset(static_cast<void*>(&decode_scratch), 0, sizeof(decode_scratch));
    for (uint32_t index = 0; index < document->lfo_count; ++index) {
        decode_scratch.bank.lfos.allocate();
        if (! std::isfinite(document->lfos[index].min_value) || ! std::isfinite(document->lfos[index].min_max_delta)) {
            return false;
        }
    }
    memcpy(decode_scratch.bank.lfos.entries, document->lfos, document->lfo_count * sizeof(document->lfos[0]));
    decode_scratch.bank.channel_chains[0] = document->chain;
    if (! validate_instrument_bank(&decode_scratch.bank)) {
        return false;
    }
    if (! Sculptor::validate_effect_routing(document->chain, document->routing)) {
        return false;
    }
    for (uint32_t index = 0; index < document->graph_layout_count; ++index) {
        const EffectsGraphLayout& record = document->graph_layout[index];
        if (record.kind > effects_graph_layout_lfo || ! std::isfinite(record.x) || ! std::isfinite(record.y) ||
            ! std::isfinite(record.width_override) || ! std::isfinite(record.height_override) ||
            record.width_override < 0 || record.height_override < 0) {
            return false;
        }
        if (record.kind == effects_graph_layout_effect) {
            if (record.index >= document->chain.num_effects ||
                document->chain.effects[record.index].type == EffectType::none) {
                return false;
            }
        }
        else if (record.kind == effects_graph_layout_lfo) {
            if (! record.index || record.index > document->lfo_count) {
                return false;
            }
        }
        else if (record.index) {
            return false;
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (document->graph_layout[previous].kind == record.kind &&
                document->graph_layout[previous].index == record.index) {
                return false;
            }
        }
    }
    return true;
}

static const char* const      effects_layout_names[] = { "input", "output", "midi", "effect", "lfo" };
static Synth::EffectsDocument effects_json_scratch;

uint32_t Synth::encode_effects_json(const EffectsDocument* document, char* dest, uint32_t dest_size)
{
    if (! dest || ! validate_effects_document(document)) {
        return 0;
    }
    Out output = { json_text, 65535, 0, false, false };
    output.ch('{');
    output.key("format");
    output.text_value("synth-effects-v4", sizeof("synth-effects-v4"));
    output.key("lfos");
    output.ch('[');
    for (uint32_t index = 0; index < document->lfo_count; ++index) {
        output.sep();
        enc_lfo(output, document->lfos[index], index + 1);
    }
    output.ch(']');
    enc_chain_content(output, document->chain, document->routing, true);
    output.key("graph_layout");
    output.ch('[');
    for (uint32_t index = 0; index < document->graph_layout_count; ++index) {
        const EffectsGraphLayout& record = document->graph_layout[index];
        output.sep();
        output.ch('{');
        output.key("kind");
        output.text_value(effects_layout_names[record.kind], 16);
        if (record.kind == effects_graph_layout_effect || record.kind == effects_graph_layout_lfo) {
            output.key(record.kind == effects_graph_layout_effect ? "slot" : "lfo_desc_id");
            output.uint_value(record.index);
        }
        output.key("x");
        output.float_value(record.x);
        output.key("y");
        output.float_value(record.y);
        key_float_nonzero(output, "width_override", record.width_override);
        key_float_nonzero(output, "height_override", record.height_override);
        output.ch('}');
    }
    output.ch(']');
    output.ch('}');
    if (output.overflow || output.invalid || output.pos >= dest_size) {
        return 0;
    }
    if (! decode_effects_json(json_text, output.pos, &effects_json_scratch)) {
        return 0;
    }
    memcpy(dest, json_text, output.pos);
    dest[output.pos] = 0;
    return output.pos;
}

bool Synth::decode_effects_json(const char* text, uint32_t len, EffectsDocument* out)
{
    if (! text || ! out || len >= 65536) {
        return false;
    }
    Walker walker = {};
    if (! parse_document(text, len, walker)) {
        return false;
    }
    EffectsDocument& document = effects_json_scratch;
    memset(&document, 0, sizeof(document));

    memset(file_pool_slots[2], 0, sizeof(file_pool_slots[2]));
    uint32_t fields         = 0;
    bool     current_format = false;
    walk_object(walker, 0, [&](uint32_t key_index, uint32_t value_index) {
        if (key_eq(walker, key_index, "format")) {
            fields |= 1;
            current_format = tok_text_eq(walker, value_index, "synth-effects-v4");
            if (! current_format)
                record_ignored("unrecognized format tag, loading anyway");
        }
        else if (key_eq(walker, key_index, "input")) {
            uint32_t value = 0;
            want_routing_name(walker, key_index, value_index, effect_input_names, 3, &value);
            document.routing.in = static_cast<uint8_t>(value);
        }
        else if (key_eq(walker, key_index, "lfos")) {
            fields |= 2;
            walk_array(walker, value_index, max_lfos, [&](uint32_t element, uint32_t position) {
                if (! decode_pool_id(walker, element, file_pool_slots[2], document.lfo_count))
                    return;
                decode_lfo(walker, element, document.lfos[document.lfo_count++]);
            });
        }
        else if (key_eq(walker, key_index, "effects")) {
            fields |= 4;
            walk_array(walker, value_index, max_chain_effects, [&](uint32_t element, uint32_t position) {
                decode_effect(walker, element, document.chain.effects[position], &document.routing.out[position]);
                document.chain.num_effects = static_cast<uint8_t>(position + 1);
            });
        }
        else if (key_eq(walker, key_index, "graph_layout")) {
            fields |= 8;
            walk_array(walker, value_index, effects_graph_layout_capacity, [&](uint32_t element, uint32_t position) {
                const uint32_t kind = graph_record_kind(walker, element, effects_layout_names, 5);
                if (kind >= 5)
                    return;
                EffectsGraphLayout& record  = document.graph_layout[document.graph_layout_count];
                uint32_t            present = 0;
                walk_object(walker, element, [&](uint32_t layout_key, uint32_t layout_value) {
                    uint32_t value = 0;
                    if (key_eq(walker, layout_key, "kind")) {
                        present |= 1;
                        want_enum(walker, layout_value, effects_layout_names, 5, &value);
                        record.kind = static_cast<uint8_t>(value);
                    }
                    else if (key_eq(walker, layout_key, "slot")) {
                        present |= 2;
                        want_uint(walker, layout_value, max_chain_effects - 1, &value);
                        record.index = static_cast<uint16_t>(value);
                    }
                    else if (key_eq(walker, layout_key, "lfo_desc_id")) {
                        present |= 4;
                        record.index = reference_id(walker, layout_key, layout_value);
                    }
                    else if (key_eq(walker, layout_key, "x")) {
                        present |= 8;
                        want_float(walker, layout_value, &record.x);
                    }
                    else if (key_eq(walker, layout_key, "y")) {
                        present |= 16;
                        want_float(walker, layout_value, &record.y);
                    }
                    else if (key_eq(walker, layout_key, "width_override")) {
                        want_float(walker, layout_value, &record.width_override);
                    }
                    else if (key_eq(walker, layout_key, "height_override")) {
                        want_float(walker, layout_value, &record.height_override);
                    }
                    else {
                        unknown_field(walker, layout_key);
                    }
                });
                const uint32_t expected = 25u | (record.kind == effects_graph_layout_effect ? 2u
                                                 : record.kind == effects_graph_layout_lfo  ? 4u
                                                                                            : 0u);
                if (present != expected) {
                    walker.failed = true;
                }
                ++document.graph_layout_count;
            });
        }
        else {
            unknown_field(walker, key_index);
        }
    });
    if (walker.failed) {
        return false;
    }
    if (! (fields & 1))
        record_ignored("missing effects format tag, loading anyway");
    if (! resolve_chain_ids(document.chain, file_pool_slots[2]))
        return false;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < document.graph_layout_count; ++i) {
        auto& record = document.graph_layout[i];
        if (record.kind == effects_graph_layout_lfo) {
            if (! resolve_file_id(record.index, file_pool_slots[2]))
                return false;
            if (! record.index) {
                record_ignored("LFO layout reference defaulted, skipped");
                continue;
            }
        }
        document.graph_layout[kept++] = record;
    }
    document.graph_layout_count = kept;
    normalize_bypass_routing(document.routing);
    uint32_t old_to_new[max_chain_effects] = {};
    Sculptor::fx_compact_chain(&document.chain, &document.routing, old_to_new);
    // Slot-keyed effect records follow the permuted payloads; a record
    // pointing at a compacted-away dormant slot refuses.
    for (uint32_t index = 0; index < document.graph_layout_count; ++index) {
        EffectsGraphLayout& record = document.graph_layout[index];
        if (record.kind != effects_graph_layout_effect) {
            continue;
        }
        if (record.index >= max_chain_effects || old_to_new[record.index] >= max_chain_effects) {
            return false;
        }
        record.index = static_cast<uint16_t>(old_to_new[record.index]);
    }
    if (! validate_effects_document(&document)) {
        return false;
    }
    canonicalize_chain(document.chain);
    *out = document;
    return true;
}
