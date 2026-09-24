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

} // namespace Synth
