// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"
#include "../synth/synth_serialize.h"
#include "sculptor_instr_bank.h"

#include <stdint.h>

namespace Synth {

constexpr uint32_t library_category_len = 24;
constexpr uint32_t library_name_len     = 24;
constexpr uint32_t library_max_records  = 256;
constexpr uint32_t library_version      = 1;

// A record payload is the whole serialized editor bank image, names included:
// names are editor-only state the synth runtime never reads.
constexpr uint32_t library_payload_size = instrument_bank_header_size + static_cast<uint32_t>(sizeof(InstrumentEditorBank));
struct LibraryEntry {
    char     category[library_category_len];
    char     name[library_name_len];
    uint32_t payload_offset;
    uint32_t payload_size;
};

enum LibraryScanStatus {
    library_absent,
    library_valid,
    library_invalid
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

// Loads an instrument from the library into a channel
//
// - path - path to the instrument library file
// - entry - library entry loaded via `read_library_index()`
// - dst_bank - instrument bank into which the instrument is loaded
// - channel - channel in that instrument bank into which the instrument is placed
// - out_first_slot - first slot in the instrument pool where instruments were loaded,
//                    typically corresponds to the first zone of the target channel
//
// Returns `true` if the load succeeded
bool load_library_instrument(const char*         path,
                             const LibraryEntry* entry,
                             InstrumentEditorBank* dst_bank,
                             uint32_t            channel,
                             uint16_t*           out_first_slot);

// Writes a multi-zone instrument from an instrument bank into instrument library
//
// - path - path to the instrument library file
// - category - name of the instrument category in the library
// - name - name of the instrument in the library
// - src_bank - instrument bank to read the instrument from
// - channel - channel in that instrument bank, which will be saved
//
// Returns `true` if the write succeeded.
bool save_library_record(const char*           path,
                         const char*           category,
                         const char*           name,
                         const InstrumentEditorBank* src_bank,
                         uint32_t              channel);

} // namespace Synth
