// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_instr_bank.h"

#include <stdint.h>

namespace Synth {

constexpr uint32_t library_category_len = 24;
constexpr uint32_t library_name_len     = 24;
constexpr uint32_t library_max_records  = 256;
constexpr uint32_t library_version      = 2;

// A record payload is the reduced one-instrument editor bank encoded as JSON text,
// with its explicit length in the record header.  A payload beyond the capacity is
// rejected on load, and a rebuild of a library containing such a record is refused:
// its bytes cannot pass through the bounded staging buffers, so it is preserved in
// place and the failure is surfaced, never silently discarded.
// A record whose category or name fields lack a NUL terminator is likewise
// preserved: the scan reports the corruption instead of indexing around it.
constexpr uint32_t library_payload_max = 1024 * 1024;

struct LibraryEntry {
    char     category[library_category_len];
    char     name[library_name_len];
    uint32_t payload_offset;
    uint32_t payload_size;
};

enum LibraryScanStatus {
    library_absent,   // the file does not exist
    library_valid,    // the index was read completely
    library_invalid,  // unreadable, corrupt or truncated
    library_oversized // a record exceeds the payload capacity; rebuilding is refused
};

// Reads information about records from the library
//
// - path - path to instrument library file
// - entries - buffer of entries that is filled out
// - max_entries - capacity of entries buffer
// - out_status - (output) whether the function succeeded
// - out_index_truncated - (output) set to `true` if there were more entries in the library than `max_entries`
//
// Returns the number of entries read into `entries` from the library.
uint32_t read_library_index(const char*        path,
                            LibraryEntry*      entries,
                            uint32_t           max_entries,
                            LibraryScanStatus* out_status          = nullptr,
                            bool*              out_index_truncated = nullptr);

// Whether an index entry denotes the (category, name) record. The editor and
// the library writer must agree on record identity, so the comparison lives
// here; both strings are compared with the record's fixed field lengths.
bool library_record_matches(const LibraryEntry& entry, const char* category, const char* name);

// Loads an instrument from the library into a channel
//
// - path - path to instrument library file
// - entry - library entry loaded via `read_library_index()`
// - dst_bank - instrument bank into which the instrument is loaded
// - channel - channel in that instrument bank into which the instrument is placed
// - out_first_slot - first slot in the instrument pool where instruments were loaded,
//                    typically corresponds to the first zone of the target channel
//
// Returns `true` if the load succeeded
bool load_library_instrument(const char*           path,
                             const LibraryEntry*   entry,
                             InstrumentEditorBank* dst_bank,
                             uint32_t              channel,
                             uint16_t*             out_first_slot);

// Writes a multi-zone instrument from an instrument bank into instrument library.
// The reduced record payload is re-decoded before writing, so a record that could
// not be loaded back is visibly refused.
//
// - path - path to instrument library file
// - category - name of the instrument category in the library
// - name - name of the instrument in the library
// - src_bank - instrument bank to read the instrument from
// - channel - channel in that instrument bank, which will be saved
// - out_status - (output) why a refused save failed
//
// Returns 0 on success, otherwise an errno value (the refused-save reason
// also lands in out_status).
int save_library_record(const char*                 path,
                        const char*                 category,
                        const char*                 name,
                        const InstrumentEditorBank* src_bank,
                        uint32_t                    channel,
                        LibraryScanStatus*          out_status = nullptr);

} // namespace Synth
