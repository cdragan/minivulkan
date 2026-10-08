// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_bank_json.h"
#include "../core/d_printf.h"
#include "sculptor_atomic_file.h"
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
    bool             strict_effects;
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
    bool                      routing_set[Synth::num_mod_targets];
    uint32_t                  envelope_count;
    uint32_t                  lfo_count;
};

namespace {

// ---------------------------------------------------------------------------
// Shared enum spelling: one name per value, used by both encoder and decoder
// ---------------------------------------------------------------------------

constexpr uint32_t    num_waves             = 5;
constexpr const char* wave_names[num_waves] = { "none", "sine", "sawtooth", "pulse", "noise" };

constexpr uint32_t    num_osc_modes                 = 3;
constexpr const char* osc_mode_names[num_osc_modes] = { "blend", "fm", "sync" };

constexpr uint32_t    num_source_ops                  = 2;
constexpr const char* source_op_names[num_source_ops] = { "add", "multiply" };

constexpr uint32_t    num_mod_sources                   = 7;
constexpr const char* mod_source_names[num_mod_sources] = {
    "none", "pitch_bend", "mod_wheel", "channel_pressure", "velocity", "aftertouch", "pressure_combine"
};

constexpr const char* effect_type_names[Synth::num_effect_types] = { "none",   "distortion", "delay", "chorus",
                                                                     "reverb", "compressor", "fir" };

constexpr uint32_t    num_param_kinds                   = 4;
constexpr const char* param_kind_names[num_param_kinds] = { "external", "envelope", "lfo", "plain" };

constexpr const char* mod_target_names[Synth::num_mod_targets] = { "volume",   "pitch",          "panning",
                                                                   "duty_a",   "duty_b",         "osc_mix",
                                                                   "fm_index", "lowpass_cutoff", "highpass_cutoff" };

static_assert(static_cast<uint32_t>(Synth::WaveType::noise_wave) == num_waves - 1);
static_assert(static_cast<uint32_t>(Synth::ModSource::pressure_combine) == num_mod_sources - 1);
static_assert(static_cast<uint32_t>(Synth::SourceOp::multiply) == num_source_ops - 1);
static_assert(static_cast<uint32_t>(Synth::EffectType::fir) == Synth::num_effect_types - 1);
static_assert(static_cast<uint32_t>(Synth::ParamKind::plain) == num_param_kinds - 1);
static_assert(Synth::osc_mode_hard_sync == num_osc_modes - 1);

// ---------------------------------------------------------------------------
// Bounded static staging
// ---------------------------------------------------------------------------

// Document staging bound: the largest JSON document the codec accepts.  The
// library's payload bound (library_payload_max) matches this by design.
constexpr uint32_t bank_json_text_size = 1024 * 1024;
// One token index per parser token; the decoder pins the vendored parser's
// 16-byte token size so the key-index scratch stays a fixed fraction of the pool.
constexpr uint32_t json_token_count = 8192;
char               json_text[bank_json_text_size];
jsmntok_t          json_tokens[json_token_count];

// Token storage uses the parser's fixed four-int representation.
static_assert(sizeof(jsmntok_t) == 16);

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

constexpr uint32_t invalid_index = 0xFFFFFFFFu;

// Post-order key-uniqueness scratch: by the time an object checks its own keys,
// every nested object finished checking (theirs ran during the recursive value
// walks below), so one shared array serves all of them.
uint32_t object_keys[json_token_count];

// Resident parser storage is counted by the owner in sculptor_instr_edit.cpp.
static_assert(sizeof(json_text) + sizeof(json_tokens) + sizeof(object_keys) == 1212416);

// ---------------------------------------------------------------------------

// Shared staging for the transactional decode: the GUI thread runs one decode at a
// time, so one static scratch serves every caller (decode is not reentrant).
Synth::InstrumentEditorBank decode_scratch;

// ---------------------------------------------------------------------------
// Clipboard instrument documents: friendly-unit JSON for one instrument
// (doc/synth_instrument_schema.json). Decoding quantizes to the bank's
// resolution and the encoding emits those quantized values, so a document
// the editor copies out always pastes back.
// ---------------------------------------------------------------------------

constexpr const char instrument_format_tag[] = "synth-instrument-v1";

InstrumentDocState           instrument_doc;
Synth::InstrumentGraphLayout instrument_layout_staging[Synth::instrument_graph_layout_capacity];
Synth::GraphNodeLayout       instrument_mapped_staging[Synth::instrument_graph_layout_capacity];

char                         editor_export_text[64 * 1024];
Synth::Instrument            editor_export_decoded;
Synth::EnvelopeDescriptor    editor_export_envelopes[Synth::instrument_max_envelopes];
Synth::LFODescriptor         editor_export_lfos[Synth::instrument_max_lfos];
Synth::InstrumentGraphLayout editor_export_source_layout[Synth::instrument_graph_layout_capacity];
Synth::InstrumentGraphLayout editor_export_normalized[Synth::instrument_graph_layout_capacity];
} // namespace

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

static void key_bool(Out& o, const char* name, bool v)
{
    o.key(name);
    o.bool_value(v);
}

static bool gen_is_default(const Synth::LayerGen& gen)
{
    return gen.envelope_desc_id == 0 && gen.lfo_desc_id == 0 && gen.lfo_op == Synth::SourceOp::add &&
           gen.lfo_depth == 0.0f && gen.lfo_depth_source == Synth::ModSource::none &&
           gen.lfo_rate_source == Synth::ModSource::none && gen.lfo_rate_scale_ms == 0.0f;
}

static bool routing_is_default(const Synth::InputRouting& routing)
{
    return routing.base_value == 0.0f && routing.num_inputs == 0;
}

static void enc_mod_input(Out& o, const Synth::ModInput& in)
{
    o.ch('{');
    key_enum(o, "source", mod_source_names, num_mod_sources, static_cast<uint32_t>(in.source));
    key_enum(o, "op", source_op_names, num_source_ops, static_cast<uint32_t>(in.op));
    key_float_nonzero(o, "scale", in.scale);
    o.ch('}');
}

static void enc_gen(Out& o, const Synth::LayerGen& gen)
{
    o.ch('{');
    key_uint_nonzero(o, "envelope_desc_id", gen.envelope_desc_id);
    key_uint_nonzero(o, "lfo_desc_id", gen.lfo_desc_id);
    key_enum(o, "lfo_op", source_op_names, num_source_ops, static_cast<uint32_t>(gen.lfo_op));
    key_float_nonzero(o, "lfo_depth", gen.lfo_depth);
    key_enum(o, "lfo_depth_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(gen.lfo_depth_source));
    key_enum(o, "lfo_rate_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(gen.lfo_rate_source));
    key_float_nonzero(o, "lfo_rate_scale_ms", gen.lfo_rate_scale_ms);
    o.ch('}');
}

static void enc_routing(Out& o, const Synth::InputRouting& routing)
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
    key_float_nonzero(o, "pitch_offset", osc.pitch_offset);

    o.key("generators");
    o.ch('{');
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        if (gen_is_default(osc.gen[t]))
            continue;
        o.sep();
        o.ch('"');
        o.str(mod_target_names[t]);
        o.str("\":");
        enc_gen(o, osc.gen[t]);
    }
    o.ch('}');
    o.ch('}');
}

static void enc_instrument(Out& o, const Synth::Instrument& instr)
{
    if (instr.layer_count > Synth::max_layers) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_uint(o, "layer_count", instr.layer_count);

    o.key("layers");
    o.ch('[');
    for (uint32_t layer = 0; layer < instr.layer_count; layer++) {
        o.sep();
        enc_oscillator(o, instr.layers[layer]);
    }
    o.ch(']');

    o.key("routing");
    o.ch('{');
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        if (routing_is_default(instr.routing[t]))
            continue;
        o.sep();
        o.ch('"');
        o.str(mod_target_names[t]);
        o.str("\":");
        enc_routing(o, instr.routing[t]);
    }
    o.ch('}');

    key_float_nonzero(o, "note_skew_semitones", instr.note_skew_semitones);
    key_float_nonzero(o, "layer_skew_semitones", instr.layer_skew_semitones);
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

static void enc_envelope(Out& o, const Synth::EnvelopeDescriptor& env)
{
    o.ch('{');
    enc_envelope_members(o, env);
    o.ch('}');
}

static void enc_lfo(Out& o, const Synth::LFODescriptor& lfo)
{
    o.ch('{');
    key_enum(o, "wave", wave_names, num_waves, static_cast<uint32_t>(lfo.wave));
    key_uint_nonzero(o, "duty", lfo.duty);
    key_uint_nonzero(o, "period_ms", lfo.period_ms);
    key_float_nonzero(o, "min_value", lfo.min_value);
    key_float_nonzero(o, "min_max_delta", lfo.min_max_delta);
    o.ch('}');
}

static void enc_param(Out& o, const Synth::ParamDescriptor& param)
{
    const uint32_t kind = static_cast<uint32_t>(param.kind);
    if (kind >= num_param_kinds) {
        o.overflow = true;
        return;
    }

    o.ch('{');
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
            key_uint_nonzero(o, "depth_param_id", param.lfo.depth_param_id);
            key_uint_nonzero(o, "rate_param_id", param.lfo.rate_param_id);
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
                    key_uint_nonzero(o, "param_id", src.param_id);
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

static void enc_binding(Out& o, const Synth::EffectParamBinding& binding)
{
    if (binding.num_inputs > Synth::max_mod_inputs) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    key_float_nonzero(o, "base_value", binding.base_value);
    key_uint_nonzero(o, "lfo_desc_id", binding.lfo_desc_id);
    key_enum(o, "lfo_op", source_op_names, num_source_ops, static_cast<uint32_t>(binding.lfo_op));
    key_float_nonzero(o, "lfo_depth", binding.lfo_depth);
    key_enum(o, "lfo_depth_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(binding.lfo_depth_source));
    key_enum(o, "lfo_rate_source", mod_source_names, num_mod_sources, static_cast<uint32_t>(binding.lfo_rate_source));
    key_float_nonzero(o, "lfo_rate_scale", binding.lfo_rate_scale);
    if (binding.num_inputs) {
        o.key("inputs");
        o.ch('[');
        for (uint32_t i = 0; i < binding.num_inputs; i++) {
            o.sep();
            enc_mod_input(o, binding.inputs[i]);
        }
        o.ch(']');
    }
    o.ch('}');
}

static void enc_effect(Out& o, const Synth::EffectSlotBinding& slot, bool strict = false)
{
    const uint32_t type = static_cast<uint32_t>(slot.type);
    if (type >= Synth::num_effect_types) {
        o.overflow = true;
        return;
    }

    o.ch('{');
    if (strict) {
        o.key("type");
        o.text_value(effect_type_names[type], 32);
    }
    else {
        key_enum(o, "type", effect_type_names, Synth::num_effect_types, type);
    }
    if (slot.enabled)
        key_bool(o, "enabled", true);
    if (slot.type != Synth::EffectType::none) {
        o.key("params");
        o.ch('[');
        const uint32_t num_params = Synth::get_effect_param_floats(slot.type);
        for (uint32_t p = 0; p < num_params; p++) {
            o.sep();
            enc_binding(o, slot.bindings[p]);
        }
        o.ch(']');
    }
    o.ch('}');
}

static void enc_chain_content(Out& o, const Synth::EffectChainBinding& chain, bool strict = false)
{
    if (chain.num_effects > Synth::max_chain_effects) {
        o.overflow = true;
        return;
    }

    o.key("effects");
    o.ch('[');
    for (uint32_t s = 0; s < chain.num_effects; s++) {
        o.sep();
        enc_effect(o, chain.effects[s], strict);
    }
    o.ch(']');
}

static void enc_chain(Out& output, const Synth::EffectChainBinding& chain)
{
    output.ch('{');
    enc_chain_content(output, chain);
    output.ch('}');
}

static void enc_channel(Out& o, const Synth::InstrumentEditorBank& bank, uint32_t channel)
{
    o.ch('{');
    key_bool(o, "enabled", bank.bank.channel_enabled[channel] != 0);

    o.key("zones");
    o.ch('[');
    const Synth::Zone* const zones = bank.bank.channel_zones[channel];
    for (uint32_t z = 0; z < Synth::max_instr_per_channel && zones[z].start_note != 0; z++) {
        o.sep();
        o.ch('{');
        // start is the zone's first note; the stored byte keeps the +1 encoding
        key_uint(o, "start", static_cast<uint32_t>(zones[z].start_note) - 1);
        key_uint(o, "instrument", zones[z].instrument);
        o.ch('}');
    }
    o.ch(']');

    o.key("chain");
    enc_chain(o, bank.bank.channel_chains[channel]);
    o.ch('}');
}

static void enc_effect_audio(Out& output, const Sculptor::EffectAudioTopology& audio)
{
    output.ch('[');
    for (uint32_t index = 0; index <= Synth::max_chain_effects; ++index) {
        output.sep();
        output.uint_value(audio.next[index]);
    }
    output.ch(']');
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
        if (! Sculptor::validate_effect_audio_topology(chain, bank->effect_audio[owner])) {
            return 0;
        }
    }
    bool explicit_audio = false;
    for (uint32_t owner = 0; owner <= max_channels; ++owner) {
        explicit_audio |= bank->effect_audio[owner].explicit_edges != 0;
    }
    Out o = { dest, dest_size, 0, false, false };
    o.str(explicit_audio ? "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                         : "{\"instrument_editor_bank\":{");
    key_uint(o, "drum_track_channel", bank->bank.drum_track_channel);

    o.key("instrument_names");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.instruments.num_allocated; i++) {
        o.sep();
        o.text_value(bank->instrument_names[i], Synth::max_name_len);
    }
    o.ch(']');

    o.key("channel_names");
    o.ch('[');
    for (uint32_t c = 0; c < Synth::max_channels; c++) {
        o.sep();
        o.text_value(bank->channel_names[c], Synth::max_name_len);
    }
    o.ch(']');

    o.key("instruments");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.instruments.num_allocated; i++) {
        o.sep();
        enc_instrument(o, bank->bank.instruments.entries[i]);
    }
    o.ch(']');

    o.key("envelopes");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.envelopes.num_allocated; i++) {
        o.sep();
        enc_envelope(o, bank->bank.envelopes.entries[i]);
    }
    o.ch(']');

    o.key("lfos");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.lfos.num_allocated; i++) {
        o.sep();
        enc_lfo(o, bank->bank.lfos.entries[i]);
    }
    o.ch(']');

    o.key("parameters");
    o.ch('[');
    for (uint32_t i = 0; i < bank->bank.parameters.num_allocated; i++) {
        o.sep();
        enc_param(o, bank->bank.parameters.entries[i]);
    }
    o.ch(']');

    o.key("channels");
    o.ch('[');
    for (uint32_t c = 0; c < Synth::max_channels; c++) {
        o.sep();
        enc_channel(o, *bank, c);
    }
    o.ch(']');

    o.key("master_chain");
    enc_chain(o, bank->bank.master_chain);
    // Editor-side state (sparse per-zone node layout records and the missing
    // oscillator-sum masks).
    o.key("editor");
    o.ch('{');
    if (explicit_audio) {
        o.key("effect_audio");
        o.ch('[');
        for (uint32_t owner = 0; owner <= max_channels; ++owner) {
            o.sep();
            if (bank->effect_audio[owner].explicit_edges) {
                enc_effect_audio(o, bank->effect_audio[owner]);
            }
            else {
                o.str("null");
            }
        }
        o.ch(']');
    }
    o.key("layouts");
    o.ch('[');
    for (uint32_t i = 0; i < bank->graph_layout_count; i++) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        o.sep();
        o.ch('{');
        key_uint(o, "channel", record.channel);
        key_uint(o, "zone", record.zone);
        key_uint(o, "kind", record.kind);
        key_uint(o, "index", record.index);
        key_float(o, "x", record.x);
        key_float(o, "y", record.y);
        key_float(o, "width", record.width_override);
        key_float(o, "height", record.height_override);
        key_uint(o, "depth_source", record.depth_source);
        key_uint(o, "rate_source", record.rate_source);
        key_uint(o, "uid", record.uid);
        // Kind-3 partial-wiring persistence; the keys are optional and
        // decode to zero when absent.
        key_uint(o, "served", record.served);
        key_uint(o, "env_desc_id", record.env_desc_id);
        key_uint(o, "lfo_desc_id", record.lfo_desc_id);
        key_uint(o, "lfo_depth_source", record.lfo_depth_source);
        key_uint(o, "lfo_rate_source", record.lfo_rate_source);
        // Absent when zero: a zero ordinal joins the target's uid-order
        // positional attach.
        key_uint_nonzero(o, "param_slot", record.param_slot);
        o.key("name");
        o.text_value(record.name, sizeof(record.name));
        o.ch('}');
    }
    o.ch(']');
    o.key("missing_sum");
    o.ch('[');
    for (uint32_t c = 0; c < Synth::max_channels; c++) {
        o.sep();
        o.ch('[');
        for (uint32_t z = 0; z < Synth::max_instr_per_channel; z++) {
            o.sep();
            o.uint_value(bank->graph_missing_sum[c][z]);
        }
        o.ch(']');
    }
    o.ch(']');
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

static void unknown_field(Walker& w, uint32_t key_idx)
{
    if (w.strict_effects) {
        w.failed = true;
        return;
    }
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

    d_printf("Ignored unknown bank field: %s\n", display);
}

static void want_uint(Walker& w, uint32_t i, uint32_t max_value, uint32_t* out)
{
    const jsmntok_t& t = w.toks[i];
    if (t.type != JSMN_PRIMITIVE) {
        w.failed = true;
        return;
    }

    const char* const s   = w.doc + t.start;
    const uint32_t    len = static_cast<uint32_t>(t.end - t.start);

    // Leading zeros are rejected by the document-wide grammar pass, so the digit
    // walk plus the per-step bound check needs no separate overflow guard.
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

static void want_bool(Walker& w, uint32_t i, bool* out)
{
    if (w.toks[i].type == JSMN_PRIMITIVE && tok_text_eq(w, i, "true"))
        *out = true;
    else if (w.toks[i].type == JSMN_PRIMITIVE && tok_text_eq(w, i, "false"))
        *out = false;
    else
        w.failed = true;
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

    if (! unescape_string(w.doc + t.start, static_cast<uint32_t>(t.end - t.start), out, out_size))
        w.failed = true;
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
    }
}

// Walks one array token, invoking handle(element_index, element_position) for every
// element; more elements than max_len is malformed input, fewer is fine (the missing
// tail keeps the default fill).
template <typename F> static void walk_array(Walker& w, uint32_t arr, uint32_t max_len, F handle)
{
    const jsmntok_t& t = w.toks[arr];
    if (t.type != JSMN_ARRAY || static_cast<uint32_t>(t.size) > max_len) {
        w.failed = true;
        return;
    }

    uint32_t  idx   = arr + 1;
    const int elems = t.size;

    for (int e = 0; e < elems && ! w.failed; e++) {
        const uint32_t elem = idx;
        idx                 = skip_value(w, elem);
        if (idx == invalid_index) {
            w.failed = true;
            return;
        }
        handle(elem, static_cast<uint32_t>(e));
    }
}

// Field walk shared by the bank and document decoders; unknown_field carries
// each format's policy (the bank skips, the document refuses) and source_set
// tells strict callers whether "source" was present.
static void decode_mod_input_fields(Walker&          w,
                                    uint32_t         obj,
                                    Synth::ModInput& in,
                                    bool*            source_set,
                                    void (*unknown)(Walker&, uint32_t))
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
        else if (key_eq(w, key_idx, "scale"))
            want_float(w, val_idx, &in.scale);
        else
            unknown(w, key_idx);
    });
}

static void decode_mod_input(Walker& w, uint32_t obj, Synth::ModInput& in)
{
    bool source_set; // unused: the bank format makes "source" optional
    decode_mod_input_fields(w, obj, in, &source_set, unknown_field);
}

static void decode_gen(Walker& w, uint32_t obj, Synth::LayerGen& gen)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "envelope_desc_id")) {
            want_uint(w, val_idx, Synth::max_envelopes, &value);
            gen.envelope_desc_id = static_cast<uint16_t>(value);
        }
        else if (key_eq(w, key_idx, "lfo_desc_id")) {
            want_uint(w, val_idx, Synth::max_lfos, &value);
            gen.lfo_desc_id = static_cast<uint16_t>(value);
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

static void decode_routing(Walker& w, uint32_t obj, Synth::InputRouting& routing)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "base_value"))
            want_float(w, val_idx, &routing.base_value);
        else if (key_eq(w, key_idx, "inputs"))
            walk_array(w, val_idx, Synth::max_mod_inputs, [&](uint32_t elem, uint32_t pos) {
                decode_mod_input(w, elem, routing.inputs[pos]);
                routing.num_inputs = static_cast<uint16_t>(pos + 1);
            });
        else
            unknown_field(w, key_idx);
    });
}

static void decode_oscillator(Walker& w, uint32_t obj, Synth::Oscillator& osc)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "wave_a")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
            osc.osc_type[0] = static_cast<Synth::WaveType>(value);
        }
        else if (key_eq(w, key_idx, "wave_b")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
            osc.osc_type[1] = static_cast<Synth::WaveType>(value);
        }
        else if (key_eq(w, key_idx, "mode")) {
            want_enum(w, val_idx, osc_mode_names, num_osc_modes, &value);
            osc.osc_mode = static_cast<Synth::OscMode>(value);
        }
        else if (key_eq(w, key_idx, "mod_ratio"))
            want_float(w, val_idx, &osc.mod_ratio);
        else if (key_eq(w, key_idx, "pitch_offset"))
            want_float(w, val_idx, &osc.pitch_offset);
        else if (key_eq(w, key_idx, "generators"))
            walk_object(w, val_idx, [&](uint32_t gen_key, uint32_t gen_val) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                    if (key_eq(w, gen_key, mod_target_names[t])) {
                        decode_gen(w, gen_val, osc.gen[t]);
                        return;
                    }
                }
                unknown_field(w, gen_key);
            });
        else
            unknown_field(w, key_idx);
    });
}

static void decode_instrument(Walker& w, uint32_t obj, Synth::Instrument& instr)
{
    uint32_t layer_count = 0;
    uint32_t num_layers  = 0;
    bool     have_count  = false;
    bool     have_layers = false;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "layer_count")) {
            want_uint(w, val_idx, Synth::max_layers, &layer_count);
            have_count = true;
        }
        else if (key_eq(w, key_idx, "layers")) {
            have_layers = true;
            walk_array(w, val_idx, Synth::max_layers, [&](uint32_t elem, uint32_t pos) {
                decode_oscillator(w, elem, instr.layers[pos]);
                num_layers = pos + 1;
            });
        }
        else if (key_eq(w, key_idx, "routing"))
            walk_object(w, val_idx, [&](uint32_t r_key, uint32_t r_val) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                    if (key_eq(w, r_key, mod_target_names[t])) {
                        decode_routing(w, r_val, instr.routing[t]);
                        return;
                    }
                }
                unknown_field(w, r_key);
            });
        else if (key_eq(w, key_idx, "note_skew_semitones"))
            want_float(w, val_idx, &instr.note_skew_semitones);
        else if (key_eq(w, key_idx, "layer_skew_semitones"))
            want_float(w, val_idx, &instr.layer_skew_semitones);
        else
            unknown_field(w, key_idx);
    });

    if (w.failed)
        return;

    if (have_count && have_layers && layer_count != num_layers) {
        w.failed = true;
        return;
    }

    // Absent layers and count still form a valid one-oscillator instrument.
    instr.layer_count = have_count ? layer_count : (have_layers ? num_layers : 1);
}

static bool envelope_member(Walker&                    w,
                            uint32_t                   key_idx,
                            uint32_t                   val_idx,
                            Synth::EnvelopeDescriptor& env,
                            EnvelopeFields&            fields)
{
    uint32_t value = 0;

    if (key_eq(w, key_idx, "num_points")) {
        want_uint(w, val_idx, Synth::max_envelope_points, &fields.num_points);
        fields.have_count = true;
        return true;
    }
    if (key_eq(w, key_idx, "sustain_first")) {
        want_uint(w, val_idx, Synth::max_envelope_points - 1, &value);
        env.sustain_first_point = static_cast<uint8_t>(value);
        return true;
    }
    if (key_eq(w, key_idx, "sustain_last")) {
        want_uint(w, val_idx, Synth::max_envelope_points - 1, &value);
        env.sustain_last_point = static_cast<uint8_t>(value);
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
        w.failed = true;
        return;
    }

    env.num_points = static_cast<uint8_t>(fields.have_count ? fields.num_points : fields.num_point_entries);

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
        if (! envelope_member(w, key_idx, val_idx, env, fields))
            unknown_field(w, key_idx);
    });
    envelope_finish(w, env, fields);
}

static void decode_lfo(Walker& w, uint32_t obj, Synth::LFODescriptor& lfo)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "wave")) {
            want_enum(w, val_idx, wave_names, num_waves, &value);
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

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "kind")) {
            want_enum(w, val_idx, param_kind_names, num_param_kinds, &kind);
            have_kind = true;
        }
    });

    if (w.failed || ! have_kind) {
        w.failed = true;
        return;
    }
    param.kind = static_cast<Synth::ParamKind>(kind);

    EnvelopeFields fields;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "kind"))
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
                    want_uint(w, val_idx, 0xFFFF, &value);
                    param.lfo.depth_param_id = static_cast<uint16_t>(value);
                }
                else if (key_eq(w, key_idx, "rate_param_id")) {
                    want_uint(w, val_idx, 0xFFFF, &value);
                    param.lfo.rate_param_id = static_cast<uint16_t>(value);
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
                                want_uint(w, s_val, 0xFFFF, &value);
                                src.param_id = static_cast<uint16_t>(value);
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

static void decode_binding(Walker& w, uint32_t obj, Synth::EffectParamBinding& binding)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "base_value"))
            want_float(w, val_idx, &binding.base_value);
        else if (key_eq(w, key_idx, "lfo_desc_id")) {
            want_uint(w, val_idx, Synth::max_lfos, &value);
            binding.lfo_desc_id = static_cast<uint16_t>(value);
        }
        else if (key_eq(w, key_idx, "lfo_op")) {
            want_enum(w, val_idx, source_op_names, num_source_ops, &value);
            binding.lfo_op = static_cast<Synth::SourceOp>(value);
        }
        else if (key_eq(w, key_idx, "lfo_depth"))
            want_float(w, val_idx, &binding.lfo_depth);
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
                decode_mod_input(w, elem, binding.inputs[pos]);
                binding.num_inputs = static_cast<uint16_t>(pos + 1);
            });
        else
            unknown_field(w, key_idx);
    });
}

static void decode_effect(Walker& w, uint32_t obj, Synth::EffectSlotBinding& slot)
{
    uint32_t num_params  = 0;
    bool     have_params = false;
    bool     have_type   = false;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "type")) {
            have_type = true;
            want_enum(w, val_idx, effect_type_names, Synth::num_effect_types, &value);
            slot.type = static_cast<Synth::EffectType>(value);
        }
        else if (key_eq(w, key_idx, "enabled")) {
            bool enabled = false;
            want_bool(w, val_idx, &enabled);
            slot.enabled = enabled;
        }
        else if (key_eq(w, key_idx, "params")) {
            have_params = true;
            walk_array(w, val_idx, Synth::max_effect_param_floats, [&](uint32_t elem, uint32_t pos) {
                decode_binding(w, elem, slot.bindings[pos]);
                num_params = pos + 1;
            });
        }
        else
            unknown_field(w, key_idx);
    });

    if (w.failed)
        return;

    // Params must cover exactly the fields the effect type reads.
    if (w.strict_effects && (! have_type || (slot.type != Synth::EffectType::none && ! have_params))) {
        w.failed = true;
    }
    if (have_params && num_params != Synth::get_effect_param_floats(slot.type))
        w.failed = true;
}

static void decode_chain(Walker& w, uint32_t obj, Synth::EffectChainBinding& chain)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "effects"))
            walk_array(w, val_idx, Synth::max_chain_effects, [&](uint32_t elem, uint32_t pos) {
                decode_effect(w, elem, chain.effects[pos]);
                chain.num_effects = static_cast<uint8_t>(pos + 1);
            });
        else
            unknown_field(w, key_idx);
    });
}

static void decode_zone(Walker& w, uint32_t obj, Synth::Zone& zone)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        uint32_t value = 0;
        if (key_eq(w, key_idx, "start")) {
            want_uint(w, val_idx, 127, &value);
            zone.start_note = static_cast<uint8_t>(value + 1);
        }
        else if (key_eq(w, key_idx, "instrument")) {
            want_uint(w, val_idx, Synth::max_instruments - 1, &value);
            zone.instrument = static_cast<uint8_t>(value);
        }
        else
            unknown_field(w, key_idx);
    });
}

static void decode_channel(Walker& w, uint32_t obj, Synth::InstrumentEditorBank* out, uint32_t channel)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "enabled")) {
            bool enabled = false;
            want_bool(w, val_idx, &enabled);
            out->bank.channel_enabled[channel] = enabled ? 1 : 0;
        }
        else if (key_eq(w, key_idx, "zones"))
            walk_array(w, val_idx, Synth::max_instr_per_channel, [&](uint32_t elem, uint32_t pos) {
                decode_zone(w, elem, out->bank.channel_zones[channel][pos]);
            });
        else if (key_eq(w, key_idx, "chain"))
            decode_chain(w, val_idx, out->bank.channel_chains[channel]);
        else
            unknown_field(w, key_idx);
    });
}

static void decode_names(Walker& w, uint32_t arr, uint32_t cap, char (*names)[Synth::max_name_len])
{
    walk_array(w, arr, cap, [&](uint32_t elem, uint32_t pos) { want_text(w, elem, names[pos], Synth::max_name_len); });
}

// Pool entries decode into zeroed locals (absent entry fields take their zero
// defaults), then take one slot of the pool: sequential allocation rebuilds the
// dense prefix the bank invariants require.  Every allocation is checked - the
// pool fills up when the document repeats pool arrays, and a full pool must reject
// the document, never write past its entries.
static void decode_instruments(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    walk_array(w, arr, Synth::max_instruments, [&](uint32_t elem, uint32_t pos) {
        Synth::Instrument instr = {};
        decode_instrument(w, elem, instr);
        if (w.failed)
            return;
        const uint32_t slot = out->bank.instruments.allocate();
        if (slot == pool_no_slot) {
            w.failed = true;
            return;
        }
        out->bank.instruments.entries[slot] = instr;
    });
}

static void decode_envelopes(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    walk_array(w, arr, Synth::max_envelopes, [&](uint32_t elem, uint32_t pos) {
        Synth::EnvelopeDescriptor env = {};
        decode_envelope(w, elem, env);
        if (w.failed)
            return;
        const uint32_t slot = out->bank.envelopes.allocate();
        if (slot == pool_no_slot) {
            w.failed = true;
            return;
        }
        out->bank.envelopes.entries[slot] = env;
    });
}

static void decode_lfos(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    walk_array(w, arr, Synth::max_lfos, [&](uint32_t elem, uint32_t pos) {
        Synth::LFODescriptor lfo = {};
        decode_lfo(w, elem, lfo);
        if (w.failed)
            return;
        const uint32_t slot = out->bank.lfos.allocate();
        if (slot == pool_no_slot) {
            w.failed = true;
            return;
        }
        out->bank.lfos.entries[slot] = lfo;
    });
}

static void decode_parameters(Walker& w, uint32_t arr, Synth::InstrumentEditorBank* out)
{
    walk_array(w, arr, Synth::max_parameters, [&](uint32_t elem, uint32_t pos) {
        Synth::ParamDescriptor param = {};
        decode_param(w, elem, param);
        if (w.failed)
            return;
        const uint32_t slot = out->bank.parameters.allocate();
        if (slot == pool_no_slot) {
            w.failed = true;
            return;
        }
        out->bank.parameters.entries[slot] = param;
    });
}

// One sparse layout record.  Field-level checks that do not depend on the
// descriptor pools reject here; the descriptor-range and implied-node-count
// checks run after the whole document decoded (decode order is free).
static void decode_editor_record(Walker& w, uint32_t obj, Synth::GraphNodeLayout* record)
{
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "channel")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->channel = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "zone")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->zone = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "kind")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->kind = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "index")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->index = static_cast<uint8_t>(value);
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
            want_uint(w, val_idx, 0xFF, &value);
            record->depth_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "rate_source")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->rate_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "uid")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->uid = static_cast<uint8_t>(value);
        }
        // Kind-3 partial-wiring persistence; the keys are optional and
        // decode to zero when absent.
        else if (key_eq(w, key_idx, "served")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->served = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "env_desc_id")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->env_desc_id = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "lfo_desc_id")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->lfo_desc_id = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "lfo_depth_source")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->lfo_depth_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "lfo_rate_source")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->lfo_rate_source = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "param_slot")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            record->param_slot = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "name"))
            want_text(w, val_idx, record->name, sizeof(record->name));
        else if (w.strict_editor_metadata)
            w.failed = true;
        else
            unknown_field(w, key_idx);
    });
    if (w.failed)
        return;
    if ((record->channel >= Synth::max_channels && ! (record->kind == 4 && record->channel == Synth::max_channels)) ||
        record->zone >= Synth::max_instr_per_channel || record->kind > 4) {
        w.failed = true;
        return;
    }
    // Kind 0 keys a bound node in the fixed canonical numbering (inputs, sum
    // node, oscillator layers), and only by index: a nonzero uid would
    // create a second key for one node.
    if (record->kind == 0 && (record->index >= Synth::graph_canonical_node_count || record->uid != 0)) {
        w.failed = true;
        return;
    }
    // Source bytes name ModSource values; none (0) is valid.
    if (record->depth_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine) ||
        record->rate_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine) ||
        record->lfo_depth_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine) ||
        record->lfo_rate_source > static_cast<uint8_t>(Synth::ModSource::pressure_combine)) {
        w.failed = true;
        return;
    }
}

static void decode_effect_audio(Walker& walker, uint32_t token, Sculptor::EffectAudioTopology* out)
{
    if (walker.toks[token].type != JSMN_ARRAY || walker.toks[token].size != Synth::max_chain_effects + 1) {
        walker.failed = true;
        return;
    }
    out->explicit_edges = 1;
    walk_array(walker, token, Synth::max_chain_effects + 1, [&](uint32_t element, uint32_t position) {
        uint32_t value = 0;
        want_uint(walker, element, Synth::max_chain_effects + 1, &value);
        out->next[position] = static_cast<uint8_t>(value);
    });
}

static void decode_editor_state(Walker& w, uint32_t val_idx, Synth::InstrumentEditorBank* out, bool v2 = false)
{
    bool audio_present = false;
    walk_object(w, val_idx, [&](uint32_t key_idx, uint32_t inner_idx) {
        if (key_eq(w, key_idx, "effect_audio")) {
            audio_present = true;
            if (! v2 || w.toks[inner_idx].type != JSMN_ARRAY || w.toks[inner_idx].size != Synth::max_channels + 1) {
                w.failed = true;
                return;
            }
            walk_array(w, inner_idx, Synth::max_channels + 1, [&](uint32_t element, uint32_t owner) {
                if (w.toks[element].type == JSMN_PRIMITIVE && tok_text_eq(w, element, "null")) {
                    return;
                }
                decode_effect_audio(w, element, &out->effect_audio[owner]);
            });
        }
        else if (key_eq(w, key_idx, "layouts")) {
            uint16_t detached_per_zone[Synth::max_channels][Synth::max_instr_per_channel] = {};
            walk_array(w, inner_idx, Synth::max_graph_records, [&](uint32_t elem, uint32_t) {
                Synth::GraphNodeLayout record = {};
                decode_editor_record(w, elem, &record);
                if (w.failed)
                    return;
                if (record.kind != 0 && record.kind != 4) {
                    if (record.channel >= Synth::max_channels || record.zone >= Synth::max_instr_per_channel ||
                        ++detached_per_zone[record.channel][record.zone] > Synth::max_detached_per_zone) {
                        w.failed = true;
                        return;
                    }
                }
                // One key addresses one node: compatibility decoding drops later records;
                // strict metadata decoding refuses duplicates before any deduplication.
                for (uint32_t i = 0; i < out->graph_layout_count; ++i) {
                    const Synth::GraphNodeLayout& other = out->graph_layout[i];
                    if (Sculptor::graph_layout_record_identity_equal(other, record)) {
                        if (w.strict_editor_metadata)
                            w.failed = true;
                        return;
                    }
                }
                if (out->graph_layout_count >= Synth::max_graph_records) {
                    w.failed = true;
                    return;
                }
                out->graph_layout[out->graph_layout_count++] = record;
            });
        }
        else if (key_eq(w, key_idx, "missing_sum")) {
            walk_array(w, inner_idx, Synth::max_channels, [&](uint32_t row_elem, uint32_t channel) {
                walk_array(w, row_elem, Synth::max_instr_per_channel, [&](uint32_t elem, uint32_t zone) {
                    uint32_t value = 0;
                    want_uint(w, elem, 0x7F, &value); // only the seven layer bits are meaningful
                    if (w.failed)
                        return;
                    out->graph_missing_sum[channel][zone] = static_cast<uint8_t>(value);
                });
            });
        }
        else if (w.strict_editor_metadata)
            w.failed = true;
        else
            unknown_field(w, key_idx);
    });
    if (v2 && ! audio_present) {
        w.failed = true;
    }
}

static void decode_bank(Walker& w, uint32_t obj, Synth::InstrumentEditorBank* out, bool v2 = false)
{
    bool editor_present = false;
    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "drum_track_channel")) {
            uint32_t value = 0;
            want_uint(w, val_idx, 0xFF, &value);
            out->bank.drum_track_channel = static_cast<uint8_t>(value);
        }
        else if (key_eq(w, key_idx, "instruments"))
            decode_instruments(w, val_idx, out);
        else if (key_eq(w, key_idx, "envelopes"))
            decode_envelopes(w, val_idx, out);
        else if (key_eq(w, key_idx, "lfos"))
            decode_lfos(w, val_idx, out);
        else if (key_eq(w, key_idx, "parameters"))
            decode_parameters(w, val_idx, out);
        else if (key_eq(w, key_idx, "channels"))
            walk_array(w, val_idx, Synth::max_channels, [&](uint32_t elem, uint32_t pos) {
                decode_channel(w, elem, out, pos);
            });
        else if (key_eq(w, key_idx, "master_chain"))
            decode_chain(w, val_idx, out->bank.master_chain);
        else if (key_eq(w, key_idx, "instrument_names"))
            decode_names(w, val_idx, Synth::max_instruments, out->instrument_names);
        else if (key_eq(w, key_idx, "channel_names"))
            decode_names(w, val_idx, Synth::max_channels, out->channel_names);
        else if (key_eq(w, key_idx, "editor")) {
            editor_present = true;
            decode_editor_state(w, val_idx, out, v2);
        }
        else if (v2) {
            w.failed = true;
        }
        else
            unknown_field(w, key_idx);
    });
    if (v2 && ! editor_present) {
        w.failed = true;
    }
}

// The bank every absent field defaults to: an empty, all-disabled bank with the
// factory channel names.
static void fill_default_editor_bank(Synth::InstrumentEditorBank* bank)
{
    memset(bank, 0, sizeof(*bank));
    bank->bank.drum_track_channel = 9;
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
    const uint32_t num_params = Synth::get_effect_param_floats(slot.type);
    for (uint32_t p = 0; p < num_params; p++)
        canonicalize_binding(slot.bindings[p]);
    if (num_params < Synth::max_effect_param_floats)
        memset(&slot.bindings[num_params], 0, (Synth::max_effect_param_floats - num_params) * sizeof(slot.bindings[0]));
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
            env.unused_alignment           = 0;
            env.min_value                  = canonical_float(env.min_value);
            env.min_max_delta              = canonical_float(env.min_max_delta);
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
        memset(&bank.instruments.entries[i], 0, sizeof(bank.instruments.entries[i]));
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
    for (uint32_t c = 0; c < Synth::max_channels; c++)
        canonicalize_name(editor_bank->channel_names[c]);

    for (uint32_t i = 0; i < bank.envelopes.num_allocated; i++) {
        Synth::EnvelopeDescriptor& env = bank.envelopes.entries[i];
        env.unused_alignment           = 0;
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

        for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
            Synth::InputRouting& routing = instr.routing[t];
            routing.base_value           = canonical_float(routing.base_value);
            for (uint32_t j = 0; j < routing.num_inputs; j++)
                canonicalize_mod_input(routing.inputs[j]);
            if (routing.num_inputs < Synth::max_mod_inputs)
                memset(&routing.inputs[routing.num_inputs],
                       0,
                       (Synth::max_mod_inputs - routing.num_inputs) * sizeof(routing.inputs[0]));
        }

        for (uint32_t l = 0; l < instr.layer_count; l++) {
            Synth::Oscillator& osc = instr.layers[l];
            osc.mod_ratio          = canonical_float(osc.mod_ratio);
            osc.pitch_offset       = canonical_float(osc.pitch_offset);
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                Synth::LayerGen& gen  = osc.gen[t];
                gen.lfo_depth         = canonical_float(gen.lfo_depth);
                gen.lfo_rate_scale_ms = canonical_float(gen.lfo_rate_scale_ms);
            }
        }

        if (instr.layer_count < Synth::max_layers)
            memset(&instr.layers[instr.layer_count],
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

// Document-unit default for a target whose block (or the whole target) is
// absent: untouched instruments stay audible and centered like the factory
// default.
static float doc_base_default(Synth::ModTarget target)
{
    if (target == Synth::mod_volume)
        return 1.0f;
    if (target == Synth::mod_panning)
        return 0.5f;
    return 0.0f;
}

// A value is in the documented range: [min, max] where cutoffs additionally
// allow exactly 0 (bypass); the gap below the slider floor has no meaning.
static bool doc_value_valid(const Sculptor::OscTargetView& view, float value)
{
    return (value >= view.min_value || value == 0.0f) && value <= view.max_value;
}

// The instrument format is versioned by its "format" tag, so an unknown field
// means a mismatched document: refuse instead of skipping.
static void doc_unknown_field(Walker& w, uint32_t key_idx)
{
    (void)key_idx;
    w.failed = true;
}

// Point levels are the envelope's output values in the target's display units;
// the descriptor stores them scaled to bank units (fm_index: radians).
// Sustain defaults to holding the last point.
static void decode_envelope_doc(Walker& w, uint32_t obj, Synth::ModTarget target, Synth::EnvelopeDescriptor& env)
{
    const Sculptor::OscTargetView view = Sculptor::osc_target_view(target);

    float    times[Synth::max_envelope_points]     = {};
    float    levels[Synth::max_envelope_points]    = {};
    uint16_t quantized[Synth::max_envelope_points] = {};
    uint32_t num_points                            = 0;
    uint32_t sustain_start                         = 0;
    uint32_t sustain_end                           = 0;
    bool     sustain_start_set                     = false;
    bool     sustain_end_set                       = false;

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

    float min_level = levels[0];
    float max_level = levels[0];
    for (uint32_t i = 1; i < num_points; i++) {
        if (levels[i] < min_level)
            min_level = levels[i];
        if (levels[i] > max_level)
            max_level = levels[i];
    }

    // Sustain defaults to the last point: hold the end value until release.
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
    env.sustain_last_point  = static_cast<uint8_t>(sustain_end);

    // A flat envelope (delta 0) is the constant min: every quantized value 0.
    const float delta = max_level - min_level;
    for (uint32_t i = 0; i < num_points; i++) {
        env.points[i].position = quantized[i];
        env.points[i].value =
            delta > 0.0f ? static_cast<uint16_t>((levels[i] - min_level) / delta * 65535.0f + 0.5f) : 0;
    }
}

// The versioned instrument document refuses unknown fields inside a mod
// input, and "source" is required.
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
                           Synth::LFODescriptor& lfo,
                           Synth::LayerGen&      gen)
{
    const Sculptor::OscTargetView view = Sculptor::osc_target_view(target);

    float    frequency_hz  = 0.0f;
    bool     frequency_set = false;
    bool     wave_set      = false;
    float    duty          = 0.0f;
    float    min_level     = view.min_value;
    float    max_level     = view.max_value;
    uint32_t op            = 0;
    float    depth         = 0.0f;
    uint32_t depth_source  = 0;
    uint32_t rate_source   = 0;
    float    rate_scale_ms = 0.0f;

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

// One target block: shared routing (base, sources) plus per-layer generators
// (envelope, LFO). The routing is shared by every layer, so base/sources are
// accepted from the first layer that supplies base or sources (a layer
// with only generators does not claim the routing) and refused afterwards.
static void decode_target_doc(Walker&             w,
                              uint32_t            obj,
                              Synth::ModTarget    target,
                              Synth::Oscillator&  osc,
                              InstrumentDocState& doc)
{
    const Sculptor::OscTargetView view = Sculptor::osc_target_view(target);

    float base        = doc_base_default(target);
    bool  base_set    = false;
    bool  sources_set = false;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "base")) {
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
            decode_envelope_doc(w, val_idx, target, env);
            if (! w.failed) {
                const uint16_t match             = find_matching_descriptor(doc.envelopes, doc.envelope_count, env);
                osc.gen[target].envelope_desc_id = match != 0 ? match : static_cast<uint16_t>(doc.envelope_count + 1);
                if (match == 0)
                    ++doc.envelope_count;
            }
        }
        else if (key_eq(w, key_idx, "lfo")) {
            if (doc.lfo_count >= Synth::instrument_max_lfos) {
                w.failed = true;
                return;
            }
            Synth::LFODescriptor& lfo = doc.lfos[doc.lfo_count];
            lfo                       = Synth::LFODescriptor{};
            decode_lfo_doc(w, val_idx, target, lfo, osc.gen[target]);
            if (! w.failed) {
                const uint16_t match        = find_matching_descriptor(doc.lfos, doc.lfo_count, lfo);
                osc.gen[target].lfo_desc_id = match != 0 ? match : static_cast<uint16_t>(doc.lfo_count + 1);
                if (match == 0)
                    ++doc.lfo_count;
            }
        }
        else if (key_eq(w, key_idx, "sources")) {
            walk_array(w, val_idx, Synth::max_mod_inputs, [&](uint32_t elem, uint32_t pos) {
                decode_mod_input_doc(w, elem, doc.instr.routing[target].inputs[pos]);
            });
            doc.instr.routing[target].num_inputs = static_cast<uint16_t>(w.toks[val_idx].size);
            sources_set                          = true;
        }
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed)
        return;

    if (! doc_value_valid(view, base)) {
        w.failed = true;
        return;
    }

    if (base_set || sources_set) {
        if (doc.routing_set[target]) {
            w.failed = true; // one layer may set the shared routing
            return;
        }
        doc.routing_set[target]              = true;
        doc.instr.routing[target].base_value = base * view.bank_scale;
    }
}

// One oscillator layer.  wave_a is the one required field.
static void decode_layer_doc(Walker&                     w,
                             uint32_t                    obj,
                             Synth::Oscillator&          osc,
                             InstrumentDocState&         doc,
                             Synth::InstrumentJsonDomain domain)
{
    uint32_t wave_a       = 0;
    bool     wave_a_set   = false;
    uint32_t wave_b       = 0;
    uint32_t mode         = 0;
    float    mod_ratio    = 1.0f;
    float    pitch_offset = 0.0f;

    walk_object(w, obj, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "wave_a")) {
            want_enum(w, val_idx, wave_names, num_waves, &wave_a);
            wave_a_set = true;
        }
        else if (key_eq(w, key_idx, "wave_b"))
            want_enum(w, val_idx, wave_names, num_waves, &wave_b);
        else if (key_eq(w, key_idx, "mode"))
            want_enum(w, val_idx, osc_mode_names, num_osc_modes, &mode);
        else if (key_eq(w, key_idx, "mod_ratio"))
            want_float(w, val_idx, &mod_ratio);
        else if (key_eq(w, key_idx, "pitch_offset_semitones"))
            want_float(w, val_idx, &pitch_offset);
        else {
            uint32_t target = 0;
            while (target < Synth::num_mod_targets && ! key_eq(w, key_idx, mod_target_names[target]))
                target++;
            if (target < Synth::num_mod_targets)
                decode_target_doc(w, val_idx, static_cast<Synth::ModTarget>(target), osc, doc);
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
    if (domain == Synth::InstrumentJsonDomain::clipboard &&
        ((mod_ratio != 0.0f && (mod_ratio < Sculptor::osc_fm_ratio_min || mod_ratio > Sculptor::osc_fm_ratio_max)) ||
         pitch_offset < Sculptor::osc_pitch_offset_min || pitch_offset > Sculptor::osc_pitch_offset_max)) {
        w.failed = true;
        return;
    }

    osc.osc_type[0]  = static_cast<Synth::WaveType>(wave_a);
    osc.osc_type[1]  = static_cast<Synth::WaveType>(wave_b);
    osc.osc_mode     = static_cast<Synth::OscMode>(mode);
    osc.mod_ratio    = mod_ratio;
    osc.pitch_offset = pitch_offset;
}

// One root object, nothing before or after it, grammar-clean everywhere.
static bool parse_document(const char* text, uint32_t len, Walker& w)
{
    if (len > bank_json_text_size)
        return false;

    jsmn_parser parser;
    jsmn_init(&parser);

    const int num_tokens = jsmn_parse(&parser,
                                      text,
                                      len,
                                      json_tokens,
                                      static_cast<unsigned int>(sizeof(json_tokens) / sizeof(json_tokens[0])));
    if (num_tokens <= 0)
        return false;

    w.doc      = text;
    w.len      = len;
    w.toks     = json_tokens;
    w.num_toks = static_cast<uint32_t>(num_tokens);
    w.failed   = false;

    // The document-wide grammar pass runs before any schema decoding, so
    // malformed content is rejected everywhere - including unknown subtrees the
    // schema walk never visits.
    const uint32_t grammar_next = check_value(w, 0);
    if (grammar_next != w.num_toks || w.failed || w.toks[0].type != JSMN_OBJECT)
        return false;

    // Nothing may precede the root object: jsmn consumes separators without a
    // root, so ",{}" would otherwise pass as an empty document.
    if (! gap_is_ws(text, 0, static_cast<uint32_t>(w.toks[0].start)))
        return false;

    for (uint32_t i = static_cast<uint32_t>(w.toks[0].end); i < len; i++)
        if (! is_ws(text[i]))
            return false; // one root object, nothing trailing

    return true;
}

// Decode pipeline into decode_scratch: grammar pass, schema overlay
// onto the defaults, then descriptor validation.

static bool decode_document_into_scratch(const char* text, uint32_t len, bool strict_editor_metadata = false)
{
    Walker w = {};
    if (! parse_document(text, len, w))
        return false;

    w.strict_editor_metadata = strict_editor_metadata;
    fill_default_editor_bank(&decode_scratch);

    bool      guard = false, legacy = false, v2 = false, root_editor = false;
    uint32_t  idx   = 1;
    const int pairs = w.toks[0].size; // the root object counts its key-value pairs

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
                guard = true;
            }
            else {
                legacy = true;
                decode_bank(w, val_idx, &decode_scratch);
            }
        }
        else if (key_eq(w, key_idx, "instrument_editor_bank_v2")) {
            v2                       = true;
            const bool strict        = w.strict_editor_metadata;
            w.strict_editor_metadata = true;
            decode_bank(w, val_idx, &decode_scratch, true);
            w.strict_editor_metadata = strict;
        }
        else if (key_eq(w, key_idx, "editor")) {
            root_editor = true;
            decode_editor_state(w, val_idx, &decode_scratch);
        }
        else
            unknown_field(w, key_idx);
    }

    if (w.failed || guard != v2 || (v2 && (legacy || root_editor))) {
        return false;
    }

    if (! Synth::validate_instrument_bank(&decode_scratch.bank, false)) {
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

int Synth::write_editor_bank_json(FILE* file, const InstrumentEditorBank* bank, uint32_t max_len, uint32_t* out_len)
{
    const uint32_t len = encode_editor_bank_json(bank, json_text, sizeof(json_text));

    if (! len)
        return EOVERFLOW;
    if (len > max_len)
        return EOVERFLOW;

    // Reload guarantee: the document must parse back through the same bounded
    // buffers the next load will use; a document the codec itself cannot reload
    // is refused instead of written.
    if (! decode_document_into_scratch(json_text, len))
        return EINVAL;

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
                                              InstrumentJsonDomain   domain)
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
        const Sculptor::OscTargetView view   = Sculptor::osc_target_view(static_cast<ModTarget>(target));
        doc.instr.routing[target].base_value = doc_base_default(static_cast<ModTarget>(target)) * view.bank_scale;
        doc.routing_set[target]              = false;
    }
    doc.envelope_count = 0;
    doc.lfo_count      = 0;

    bool     format_ok    = false;
    bool     layers_seen  = false;
    uint32_t layer_count  = 0;
    uint32_t layout_count = 0;

    walk_object(w, 0, [&](uint32_t key_idx, uint32_t val_idx) {
        if (key_eq(w, key_idx, "format")) {
            if (w.toks[val_idx].type != JSMN_STRING || ! tok_text_eq(w, val_idx, instrument_format_tag))
                w.failed = true;
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
        else if (key_eq(w, key_idx, "layers")) {
            layers_seen = true;
            layer_count = static_cast<uint32_t>(w.toks[val_idx].size);
            walk_array(w, val_idx, Synth::max_layers, [&](uint32_t elem, uint32_t pos) {
                decode_layer_doc(w, elem, doc.instr.layers[pos], doc, domain);
            });
        }
        else if (key_eq(w, key_idx, "graph_layout")) {
            walk_array(w, val_idx, Synth::instrument_graph_layout_capacity, [&](uint32_t elem, uint32_t pos) {
                InstrumentGraphLayout& record = instrument_layout_staging[pos];
                record                        = {};
                uint32_t present              = 0;
                walk_object(w, elem, [&](uint32_t field, uint32_t value) {
                    const char* const keys[] = { "kind",
                                                 "x",
                                                 "y",
                                                 "width",
                                                 "height",
                                                 "canonical_index",
                                                 "layer",
                                                 "target",
                                                 "depth_source",
                                                 "rate_source",
                                                 "parameter_ordinal",
                                                 "name" };
                    uint32_t          key    = 0;
                    while (key < 12 && ! key_eq(w, field, keys[key]))
                        ++key;
                    if (key == 12) {
                        w.failed = true;
                        return;
                    }
                    present |= 1u << key;
                    if (key == 11) {
                        want_text(w, value, record.name, sizeof(record.name));
                    }
                    else if (key >= 1 && key <= 4) {
                        float number = 0;
                        want_float(w, value, &number);
                        if (key == 1)
                            record.x = number;
                        if (key == 2)
                            record.y = number;
                        if (key == 3)
                            record.width_override = number;
                        if (key == 4)
                            record.height_override = number;
                    }
                    else {
                        uint32_t number = 0;
                        want_uint(w, value, 255, &number);
                        if (key == 0)
                            record.kind = static_cast<uint8_t>(number);
                        if (key == 5)
                            record.canonical_index = static_cast<uint8_t>(number);
                        if (key == 6)
                            record.layer = static_cast<uint8_t>(number);
                        if (key == 7)
                            record.target = static_cast<uint8_t>(number);
                        if (key == 8)
                            record.depth_source = static_cast<uint8_t>(number);
                        if (key == 9)
                            record.rate_source = static_cast<uint8_t>(number);
                        if (key == 10)
                            record.parameter_ordinal = static_cast<uint8_t>(number);
                    }
                });
                const uint32_t locator_keys[] = { 1u << 5,
                                                  (1u << 6) | (1u << 7),
                                                  (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9),
                                                  (1u << 7) | (1u << 10) };
                if (record.kind > 3 ||
                    (present & ~(record.kind == 3 ? 1u << 11 : 0u)) != (31u | locator_keys[record.kind]))
                    w.failed = true;
                ++layout_count;
            });
        }
        else
            doc_unknown_field(w, key_idx);
    });

    if (w.failed || ! format_ok || ! layers_seen || layer_count == 0)
        return false;

    doc.instr.layer_count = layer_count;

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
                                                &mapped_count))
        return false;
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
    return true;
}

static uint32_t encode_instrument_json_for_domain(char*                               dest,
                                                  uint32_t                            dest_size,
                                                  const Synth::Instrument*            instr,
                                                  const Synth::InstrumentBank*        desc_bank,
                                                  const Synth::InstrumentGraphLayout* layout,
                                                  uint32_t                            layout_count,
                                                  Synth::InstrumentJsonDomain         domain);

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
                                                  Synth::InstrumentJsonDomain         domain)
{
    if (! dest || ! instr || ! desc_bank || (! layout && layout_count) ||
        layout_count > Synth::instrument_graph_layout_capacity || dest_size == 0)
        return 0;
    if (layout_count) {
        const uint32_t content_length =
            encode_instrument_json_for_domain(dest, dest_size, instr, desc_bank, nullptr, 0, domain);
        uint32_t envelope_count = 0;
        uint32_t lfo_count      = 0;
        uint32_t mapped_count   = 0;
        if (! content_length ||
            ! Synth::decode_instrument_json_for_domain(dest,
                                                       content_length,
                                                       &decode_scratch.bank.instruments.entries[0],
                                                       decode_scratch.bank.envelopes.entries,
                                                       &envelope_count,
                                                       decode_scratch.bank.lfos.entries,
                                                       &lfo_count,
                                                       nullptr,
                                                       0,
                                                       &mapped_count,
                                                       domain) ||
            ! Sculptor::map_instrument_graph_layout(decode_scratch.bank.instruments.entries[0],
                                                    decode_scratch.bank.envelopes.entries,
                                                    envelope_count,
                                                    decode_scratch.bank.lfos.entries,
                                                    lfo_count,
                                                    layout,
                                                    layout_count,
                                                    instrument_mapped_staging,
                                                    Synth::instrument_graph_layout_capacity,
                                                    &mapped_count))
            return 0;
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
    o.key("layers");
    o.ch('[');

    for (uint32_t layer = 0; layer < instr->layer_count; layer++) {
        const Synth::Oscillator& osc = instr->layers[layer];
        if (static_cast<uint32_t>(osc.osc_type[0]) >= num_waves || static_cast<uint32_t>(osc.osc_type[1]) >= num_waves)
            return 0;
        if (osc.osc_mode > Synth::osc_mode_hard_sync)
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
        o.key("pitch_offset_semitones");
        o.float_value(osc.pitch_offset);

        for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
            const Synth::ModTarget        mod_target = static_cast<Synth::ModTarget>(target);
            const Sculptor::OscTargetView view       = Sculptor::osc_target_view(mod_target);

            o.key(mod_target_names[target]);
            o.ch('{');

            // The routing is shared by every layer, so base and sources ride
            // the first layer only.
            if (layer == 0) {
                o.key("base");
                o.float_value(instr->routing[target].base_value / view.bank_scale);

                const Synth::InputRouting& routing = instr->routing[target];
                if (routing.num_inputs > Synth::max_mod_inputs)
                    return 0;
                if (routing.num_inputs != 0) {
                    o.key("sources");
                    o.ch('[');
                    for (uint32_t input = 0; input < routing.num_inputs; input++) {
                        const Synth::ModInput& in = routing.inputs[input];
                        if (in.source > Synth::ModSource::pressure_combine || in.op > Synth::SourceOp::multiply)
                            return 0;
                        o.sep();
                        enc_mod_input(o, in);
                    }
                    o.ch(']');
                }
            }

            const Synth::LayerGen& gen = osc.gen[target];

            const uint16_t env_id = gen.envelope_desc_id;
            if (env_id != 0) {
                if (env_id > desc_bank->envelopes.num_allocated)
                    return 0;
                const Synth::EnvelopeDescriptor& env = desc_bank->envelopes.entries[env_id - 1];
                if (env.num_points == 0 || env.num_points > Synth::max_envelope_points)
                    return 0;
                o.key("envelope");
                o.ch('{');
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
                key_enum(o, "wave", wave_names, num_waves, static_cast<uint32_t>(lfo.wave));
                o.key("frequency_hz");
                o.float_value(1000.0f / static_cast<float>(lfo.period_ms));
                o.key("duty");
                o.float_value(static_cast<float>(lfo.duty) / 255.0f);
                o.key("min_level");
                o.float_value(lfo.min_value / view.bank_scale);
                o.key("max_level");
                o.float_value((lfo.min_value + lfo.min_max_delta) / view.bank_scale);
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
    o.key("graph_layout");
    o.ch('[');
    for (uint32_t index = 0; index < layout_count; ++index) {
        const Synth::InstrumentGraphLayout& record = layout[index];
        o.sep();
        o.ch('{');
        o.key("kind");
        o.uint_value(record.kind);
        if (record.kind == 0) {
            o.key("canonical_index");
            o.uint_value(record.canonical_index);
        }
        if (record.kind == 1 || record.kind == 2) {
            o.key("layer");
            o.uint_value(record.layer);
        }
        if (record.kind != 0) {
            o.key("target");
            o.uint_value(record.target);
        }
        if (record.kind == 2) {
            o.key("depth_source");
            o.uint_value(record.depth_source);
            o.key("rate_source");
            o.uint_value(record.rate_source);
        }
        if (record.kind == 3) {
            o.key("parameter_ordinal");
            o.uint_value(record.parameter_ordinal);
            if (record.name[0]) {
                o.key("name");
                o.text_value(record.name, sizeof(record.name));
            }
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

int Synth::save_editor_bank_file(const char* path, const InstrumentEditorBank* bank)
{
    char                           tmp_path[512];
    const std::variant<FILE*, int> staged = atomic_write_begin(path, tmp_path, sizeof(tmp_path));

    if (std::holds_alternative<int>(staged))
        return std::get<int>(staged);

    FILE* const file = std::get<FILE*>(staged);
    uint32_t    len  = 0;

    const int error = write_editor_bank_json(file, bank, sizeof(json_text), &len);
    if (error) {
        fclose(file);
        remove(tmp_path);
        return error;
    }

    return atomic_write_commit(path, tmp_path, file);
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
    if (decode_editor_bank_json(json_text, len, out))
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
    const Synth::Instrument& instrument = source.bank.instruments.entries[entry.instrument];
    uint32_t                 env_count = 0, lfo_count = 0, source_count = 0, count = 0;
    const uint32_t           length = encode_instrument_json_for_domain(editor_export_text,
                                                                        sizeof(editor_export_text),
                                                                        &instrument,
                                                                        &source.bank,
                                                                        nullptr,
                                                                        0,
                                                                        domain);
    if (! length ||
        ! Synth::decode_instrument_json_for_domain(editor_export_text,
                                                   length,
                                                   &editor_export_decoded,
                                                   editor_export_envelopes,
                                                   &env_count,
                                                   editor_export_lfos,
                                                   &lfo_count,
                                                   nullptr,
                                                   0,
                                                   &count,
                                                   domain) ||
        ! Sculptor::encode_instrument_graph_layout(instrument,
                                                   source.bank,
                                                   source,
                                                   channel,
                                                   zone,
                                                   editor_export_source_layout,
                                                   Synth::instrument_graph_layout_capacity,
                                                   &source_count) ||
        ! Sculptor::normalize_instrument_graph_layout(instrument,
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
    const uint32_t named_length = encode_instrument_json_for_domain(editor_export_text,
                                                                    sizeof(editor_export_text),
                                                                    &instrument,
                                                                    &source.bank,
                                                                    editor_export_normalized,
                                                                    count,
                                                                    domain);
    if (! named_length || ! dest || capacity <= named_length)
        return 0;
    memcpy(dest, editor_export_text, named_length);
    return named_length;
}

bool Synth::validate_effects_document(const EffectsDocument* document)
{
    if (! document || document->lfo_count > max_lfos || document->graph_layout_count > effects_graph_layout_capacity) {
        return false;
    }
    memset(&decode_scratch, 0, sizeof(decode_scratch));
    for (uint32_t index = 0; index < document->lfo_count; ++index) {
        decode_scratch.bank.lfos.allocate();
        if (! std::isfinite(document->lfos[index].min_value) || ! std::isfinite(document->lfos[index].min_max_delta)) {
            return false;
        }
    }
    memcpy(decode_scratch.bank.lfos.entries, document->lfos, document->lfo_count * sizeof(document->lfos[0]));
    decode_scratch.bank.channel_chains[0] = document->chain;
    if (! validate_instrument_bank(&decode_scratch.bank) ||
        ! Sculptor::validate_effect_audio_topology(document->chain, document->audio)) {
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
    output.text_value(document->audio.explicit_edges ? "synth-effects-v2" : "synth-effects-v1",
                      sizeof("synth-effects-v1"));
    if (document->audio.explicit_edges) {
        output.key("audio_next");
        enc_effect_audio(output, document->audio);
    }
    output.key("lfos");
    output.ch('[');
    for (uint32_t index = 0; index < document->lfo_count; ++index) {
        output.sep();
        enc_lfo(output, document->lfos[index]);
    }
    output.ch(']');
    enc_chain_content(output, document->chain, true);
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
    if (output.overflow || output.invalid || output.pos >= dest_size ||
        ! decode_effects_json(json_text, output.pos, &effects_json_scratch)) {
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
    walker.strict_effects     = true;
    EffectsDocument& document = effects_json_scratch;
    memset(&document, 0, sizeof(document));
    uint32_t fields = 0;
    bool     v2     = false;
    walk_object(walker, 0, [&](uint32_t key_index, uint32_t value_index) {
        if (key_eq(walker, key_index, "format")) {
            fields |= 1;
            v2 = tok_text_eq(walker, value_index, "synth-effects-v2");
            if (walker.toks[value_index].type != JSMN_STRING ||
                (! v2 && ! tok_text_eq(walker, value_index, "synth-effects-v1"))) {
                walker.failed = true;
            }
        }
        else if (key_eq(walker, key_index, "audio_next")) {
            fields |= 16;
            decode_effect_audio(walker, value_index, &document.audio);
        }
        else if (key_eq(walker, key_index, "lfos")) {
            fields |= 2;
            walk_array(walker, value_index, max_lfos, [&](uint32_t element, uint32_t position) {
                decode_lfo(walker, element, document.lfos[position]);
                document.lfo_count = position + 1;
            });
        }
        else if (key_eq(walker, key_index, "effects")) {
            fields |= 4;
            walk_array(walker, value_index, max_chain_effects, [&](uint32_t element, uint32_t position) {
                decode_effect(walker, element, document.chain.effects[position]);
                document.chain.num_effects = static_cast<uint8_t>(position + 1);
            });
        }
        else if (key_eq(walker, key_index, "graph_layout")) {
            fields |= 8;
            walk_array(walker, value_index, effects_graph_layout_capacity, [&](uint32_t element, uint32_t position) {
                EffectsGraphLayout& record  = document.graph_layout[position];
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
                        want_uint(walker, layout_value, max_lfos, &value);
                        record.index = static_cast<uint16_t>(value);
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
                        walker.failed = true;
                    }
                });
                const uint32_t expected = 25u | (record.kind == effects_graph_layout_effect ? 2u
                                                 : record.kind == effects_graph_layout_lfo  ? 4u
                                                                                            : 0u);
                if (present != expected) {
                    walker.failed = true;
                }
                document.graph_layout_count = position + 1;
            });
        }
        else {
            walker.failed = true;
        }
    });
    if (walker.failed || fields != (v2 ? 31u : 15u) || ! validate_effects_document(&document)) {
        return false;
    }
    canonicalize_chain(document.chain);
    *out = document;
    return true;
}
