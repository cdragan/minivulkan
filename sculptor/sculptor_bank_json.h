// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_instr_bank.h"
#include <stdio.h>

#include <stdint.h>

namespace Synth {

// Documents are bounded: the codec stages one document at a time in a shared
// static buffer whose sizes are internal to the codec's translation unit, and
// documents or counts past the bounds are rejected rather than truncated.
// Callers surface rejections as visible notifications.
// Stream operations: the codec's staging buffer never leaves its translation
// unit; callers see only self-contained reads and writes of whole documents.

// Encodes the bank as JSON and writes it to file at the current position. The
// document is verified to reload through the load path's decode before any
// byte is written, and documents past max_len are refused. Reports the encoded
// length. Returns 0 on success, otherwise an errno value; nothing is written
// on validation or encoding failure. An I/O failure may leave a partial
// document in the stream, which the caller must discard.
int write_editor_bank_json(FILE* file, const InstrumentEditorBank* bank, uint32_t max_len, uint32_t* out_len);

// Reads a JSON document of len bytes from file at the current position into
// out. Transactional: on any failure out is left untouched.
bool read_editor_bank_json(FILE* file, uint32_t len, InstrumentEditorBank* out);

// Encodes the bank as one JSON document into dest. Returns the document length, or 0
// when dest is too small, a pool's num_allocated exceeds the pool capacity, or a count
// field exceeds its array (the encoder never trusts bank metadata - the validator
// accepts standalone parameter contents unchecked, so the encoder bounds-checks
// every count-driven loop before reading).
uint32_t encode_editor_bank_json(const InstrumentEditorBank* bank, char* dest, uint32_t dest_size);

// Decodes a JSON document into out.  The decode is transactional: it stages into
// shared static scratch, validates and canonicalizes, and copies into out only on
// full success - on ANY failure out is left untouched.
bool decode_editor_bank_json(const char* text, uint32_t len, InstrumentEditorBank* out);

enum class BankFileStatus {
    ok,       // a JSON document decoded into out
    absent,   // the file does not exist (a fresh project)
    invalid,  // exists but is malformed; out is untouched
    too_large // exceeds the bounded parse buffer; out is untouched
};

// Writes the bank as a JSON document. The document is re-parsed before writing, so a
// successful save is always reloadable; a bank whose document exceeds the bounded
// staging is visibly refused instead of written. Returns 0 on success, otherwise an
// errno value describing the failure.
int save_editor_bank_file(const char* path, const InstrumentEditorBank* bank);

// Reads a bank file: a JSON document starts with '{' after optional whitespace.
// Transactional: on any failure out is left untouched.
BankFileStatus load_editor_bank_file(const char* path, InstrumentEditorBank* out);

// ---------------------------------------------------------------------------
// Clipboard instrument documents (doc/synth_instrument_schema.json)
// ---------------------------------------------------------------------------

// One generator per layer per mod target bounds the descriptors a single
// instrument document can reference.
constexpr uint32_t instrument_max_envelopes = max_layers * num_mod_targets;
constexpr uint32_t instrument_max_lfos      = max_layers * num_mod_targets;

// Portable layout identity for a node derived from a copied instrument.
enum InstrumentGraphLayoutKind : uint8_t {
    instrument_graph_layout_canonical = 0,
    instrument_graph_layout_envelope  = 1,
    instrument_graph_layout_lfo       = 2,
    instrument_graph_layout_parameter = 3
};

constexpr uint32_t instrument_graph_layout_capacity = graph_canonical_node_count + 3u * max_layers * 5u;
static_assert(instrument_graph_layout_capacity == 114);

struct InstrumentGraphLayout {
    uint8_t kind;
    uint8_t canonical_index;
    uint8_t layer;
    uint8_t target;
    uint8_t depth_source;
    uint8_t rate_source;
    uint8_t parameter_ordinal;
    float   x;
    float   y;
    float   width_override;
    float   height_override;
};

// Decodes one instrument document into out_instr. Generator descriptor ids in
// the decoded instrument are 1-based indices into out_envelopes/out_lfos
// (0 = none), ready for remap_envelopes/remap_lfos/remap_instrument. Returns
// false with every output untouched when the text is not a valid document. An
// absent or empty "graph_layout" is accepted; a nonempty one is refused rather
// than discarded, so a caller of this entry point never silently drops layout.
bool decode_instrument_json(const char*         text,
                            uint32_t            len,
                            Instrument*         out_instr,
                            EnvelopeDescriptor* out_envelopes,
                            uint32_t*           out_envelope_count,
                            LFODescriptor*      out_lfos,
                            uint32_t*           out_lfo_count);

// Encodes instr, resolving its generator descriptor ids against desc_bank,
// into dest as one document. Returns the document length, or 0 when dest is
// too small or the instrument references a descriptor the bank does not hold.
// Equivalent to the layout overload with (nullptr, 0): the document carries an
// empty "graph_layout" array.
uint32_t encode_instrument_json(char*                 dest,
                                uint32_t              dest_size,
                                const Instrument*     instr,
                                const InstrumentBank* desc_bank);

// Layout-aware encoder overload. instr and desc_bank are always required; layout
// may be null only when layout_count is zero, and its records must already be
// normalized portable records resolving against the document-decoded model (see
// Sculptor::normalize_instrument_graph_layout). dest_size must hold the document
// plus its terminator, so a capacity one byte short of the document length + 1
// fails instead of truncating. Returns the document length excluding the NUL the
// caller writes, or 0 on any failure; the writer emits straight into dest, so
// destination bytes are unspecified when it returns 0.
uint32_t encode_instrument_json(char*                        dest,
                                uint32_t                     dest_size,
                                const Instrument*            instr,
                                const InstrumentBank*        desc_bank,
                                const InstrumentGraphLayout* layout,
                                uint32_t                     layout_count);

// Layout-aware decoder overload. All outputs are required except out_layout,
// which may be null only when the document carries zero layout records; the
// envelope and LFO counts always report the descriptors the instrument holds.
// Transactional across the instrument, both descriptor arrays, the layout array
// and all three counts: any failure - including a null or too-small layout
// output for a nonempty array - leaves every output untouched.
bool decode_instrument_json(const char*            text,
                            uint32_t               len,
                            Instrument*            out_instr,
                            EnvelopeDescriptor*    out_envelopes,
                            uint32_t*              out_envelope_count,
                            LFODescriptor*         out_lfos,
                            uint32_t*              out_lfo_count,
                            InstrumentGraphLayout* out_layout,
                            uint32_t               layout_capacity,
                            uint32_t*              out_layout_count);

} // namespace Synth
