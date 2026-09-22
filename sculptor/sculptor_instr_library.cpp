// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_library.h"

#include "../synth/synth_serialize.h"
#include "sculptor_instr_bank.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t library_file_magic = 0x42494C49;

struct RecordHeader {
    char     category[Synth::library_category_len];
    char     name[Synth::library_name_len];
    uint32_t payload_size;
};

static_assert(sizeof(RecordHeader) == 52);

bool names_terminated(const char* category, const char* name)
{
    return memchr(category, 0, Synth::library_category_len) != nullptr &&
           memchr(name, 0, Synth::library_name_len) != nullptr;
}

bool same_record(const char* a, const char* b)
{
    return strncmp(a, b, Synth::library_category_len) == 0;
}

} // namespace

uint32_t Synth::read_library_index(const char* const               path,
                                   Synth::LibraryEntry* const      entries,
                                   uint32_t const                  max_entries,
                                   Synth::LibraryScanStatus* const out_status,
                                   bool* const                     out_index_truncated)
{
    if (out_status)
        *out_status = Synth::library_invalid;

    FILE* const file = fopen(path, "rb");
    if (! file) {
        if (out_status && errno == ENOENT)
            *out_status = Synth::library_absent;
        return 0;
    }

    uint32_t header[3] = {};
    if (fread(header, sizeof(header), 1, file) != 1 || header[0] != library_file_magic ||
        header[1] != Synth::library_version) {

        fclose(file);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    const long file_size = ftell(file);
    if (file_size < 0 || fseek(file, sizeof(header), SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    uint32_t num_loaded  = 0;
    uint32_t num_scanned = 0;
    while (num_scanned < header[2]) {

        RecordHeader record;
        if (fread(&record, sizeof(record), 1, file) != 1)
            break;

        const long payload_offset = ftell(file);
        if (payload_offset < 0 || record.payload_size > static_cast<uint32_t>(file_size - payload_offset))
            break;

        num_scanned++;

        if (names_terminated(record.category, record.name) && num_loaded < max_entries) {
            memcpy(entries[num_loaded].category, record.category, Synth::library_category_len);
            memcpy(entries[num_loaded].name, record.name, Synth::library_name_len);
            entries[num_loaded].payload_offset = static_cast<uint32_t>(payload_offset);
            entries[num_loaded].payload_size   = record.payload_size;
            num_loaded++;
        }

        if (fseek(file, static_cast<long>(record.payload_size), SEEK_CUR) != 0)
            break;
    }

    fclose(file);

    if (num_scanned < header[2])
        return num_loaded;

    if (out_status)
        *out_status = Synth::library_valid;

    if (out_index_truncated)
        *out_index_truncated = header[2] > max_entries;

    return num_loaded;
}

namespace {

bool validate_record(const Synth::InstrumentEditorBank* const editor_bank)
{
    const Synth::InstrumentBank* const bank = &editor_bank->bank;
    if (bank->parameters.num_allocated != 0)
        return false;

    if (bank->master_chain.num_effects != 0)
        return false;

    if (bank->instruments.num_allocated == 0)
        return false;

    for (uint32_t slot = 0; slot < bank->instruments.num_allocated; slot++) {
        if (! bank->instruments.is_occupied(slot))
            return false;
    }

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {

        if (bank->channel_enabled[channel])
            return false;

        if (bank->channel_chains[channel].num_effects != 0)
            return false;

        if (channel == 0)
            continue;

        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {
            if (bank->channel_zones[channel][zone].start_note != 0)
                return false;
        }
    }

    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {

        const Synth::Zone& zone_desc = bank->channel_zones[0][zone];

        if (zone_desc.start_note == 0)
            break;

        if (zone_desc.instrument >= bank->instruments.num_allocated)
            return false;
    }

    // Instrument names come from file bytes; a run without NUL would make name
    // readers run past the buffer.
    for (uint32_t slot = 0; slot < bank->instruments.num_allocated; slot++) {
        if (! memchr(editor_bank->instrument_names[slot], 0, Synth::max_name_len))
            return false;
    }
    return true;
}

} // namespace

bool Synth::load_library_instrument(const char* const                path,
                                    const Synth::LibraryEntry* const entry,
                                    Synth::InstrumentEditorBank* const dst_editor_bank,
                                    uint32_t const                   channel,
                                    uint16_t* const                  out_first_slot)
{
    Synth::InstrumentBank* const dst_bank = &dst_editor_bank->bank;
    if (entry->payload_size != Synth::library_payload_size || channel >= Synth::max_channels)
        return false;

    static uint8_t image[Synth::library_payload_size];

    FILE* const file = fopen(path, "rb");
    if (! file)
        return false;

    RecordHeader record_header;

    const long header_offset = static_cast<long>(entry->payload_offset - sizeof(RecordHeader));

    // Ensure the record in the library did not change since we loaded the index
    const bool read_ok = fseek(file, header_offset, SEEK_SET) == 0 &&
                         fread(&record_header, sizeof(record_header), 1, file) == 1 &&
                         memcmp(record_header.category, entry->category, Synth::library_category_len) == 0 &&
                         memcmp(record_header.name, entry->name, Synth::library_name_len) == 0 &&
                         record_header.payload_size == entry->payload_size &&
                         fseek(file, static_cast<long>(entry->payload_offset), SEEK_SET) == 0 &&
                         fread(image, sizeof(image), 1, file) == 1;

    fclose(file);

    if (! read_ok)
        return false;

    static Synth::InstrumentEditorBank record;
    memset(&record, 0, sizeof(record));

    if (! Synth::decode_instrument_bank(image, sizeof(image), &record))
        return false;

    if (! validate_record(&record))
        return false;

    if (! Synth::validate_instrument_bank(&record.bank))
        return false;
    const uint32_t num_envelopes = record.bank.envelopes.num_allocated;
    const uint32_t num_lfos      = record.bank.lfos.num_allocated;
    const uint32_t num_instrs    = record.bank.instruments.num_allocated;

    if (dst_bank->envelopes.num_allocated + num_envelopes > Synth::max_envelopes ||
        dst_bank->lfos.num_allocated + num_lfos > Synth::max_lfos ||
        dst_bank->instruments.num_allocated + num_instrs > Synth::max_instruments)
        return false;

    uint16_t env_ids[Synth::max_envelopes];
    uint16_t lfo_ids[Synth::max_lfos];

    for (uint32_t i = 0; i < num_envelopes; i++) {
        const uint32_t slot               = dst_bank->envelopes.allocate();
        dst_bank->envelopes.entries[slot] = record.bank.envelopes.entries[i];
        env_ids[i]                        = static_cast<uint16_t>(slot + 1);
    }

    for (uint32_t i = 0; i < num_lfos; i++) {
        const uint32_t slot          = dst_bank->lfos.allocate();
        dst_bank->lfos.entries[slot] = record.bank.lfos.entries[i];
        lfo_ids[i]                   = static_cast<uint16_t>(slot + 1);
    }

    const uint32_t first_slot = dst_bank->instruments.num_allocated;

    for (uint32_t i = 0; i < num_instrs; i++) {
        const uint32_t slot = dst_bank->instruments.allocate();
        remap_instrument(record.bank.instruments.entries[i], env_ids, lfo_ids, &dst_bank->instruments.entries[slot]);
        memcpy(dst_editor_bank->instrument_names[slot], record.instrument_names[i], Synth::max_name_len);
    }

    *out_first_slot = static_cast<uint16_t>(first_slot);

    memset(dst_bank->channel_zones[channel], 0, sizeof(dst_bank->channel_zones[channel]));
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {

        const Synth::Zone& zone_src = record.bank.channel_zones[0][zone];

        if (zone_src.start_note == 0)
            break;

        dst_bank->channel_zones[channel][zone]            = zone_src;
        dst_bank->channel_zones[channel][zone].instrument = static_cast<uint8_t>(zone_src.instrument + first_slot);
    }

    return true;
}

namespace {

bool write_library_file(const char* const    path,
                        const char* const    category,
                        const char* const    name,
                        const uint8_t* const payload,
                        uint32_t const       payload_size)
{
    static Synth::LibraryEntry entries[Synth::library_max_records];

    Synth::LibraryScanStatus status          = Synth::library_invalid;
    bool                     index_truncated = false;

    const uint32_t num_entries =
        Synth::read_library_index(path, entries, Synth::library_max_records, &status, &index_truncated);

    if (status == Synth::library_invalid)
        return false;

    if (index_truncated)
        return false;

    uint32_t num_kept = 0;
    bool     replace  = false;

    for (uint32_t i = 0; i < num_entries; i++) {
        if (entries[i].payload_size != Synth::library_payload_size)
            continue;

        if (same_record(entries[i].category, category) && same_record(entries[i].name, name))
            replace = true;
        else
            num_kept++;
    }

    if (! replace && num_kept + 1 > Synth::library_max_records)
        return false;

    char      tmp_path[512];
    const int num_written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (num_written < 0 || num_written >= static_cast<int>(sizeof(tmp_path)))
        return false;

    FILE* const file = fopen(tmp_path, "wb");
    if (! file)
        return false;

    const uint32_t header[3] = { library_file_magic, Synth::library_version, num_kept + 1 };
    bool           ok        = fwrite(header, sizeof(header), 1, file) == 1;

    FILE* const old = (ok && status == Synth::library_valid) ? fopen(path, "rb") : nullptr;
    if (status == Synth::library_valid && ! old)
        ok = false;

    if (old) {
        static uint8_t record_copy[sizeof(RecordHeader) + Synth::library_payload_size];

        for (uint32_t i = 0; ok && i < num_entries; i++) {
            if (entries[i].payload_size != Synth::library_payload_size)
                continue;

            if (same_record(entries[i].category, category) && same_record(entries[i].name, name))
                continue;

            const uint32_t header_offset = entries[i].payload_offset - sizeof(RecordHeader);
            const uint32_t record_size   = sizeof(RecordHeader) + entries[i].payload_size;

            ok = fseek(old, static_cast<long>(header_offset), SEEK_SET) == 0 &&
                 fread(record_copy, record_size, 1, old) == 1 && fwrite(record_copy, record_size, 1, file) == 1;
        }

        fclose(old);
    }

    if (ok) {
        RecordHeader record;
        snprintf(record.category, sizeof(record.category), "%s", category);
        snprintf(record.name, sizeof(record.name), "%s", name);
        record.payload_size = payload_size;
        ok = fwrite(&record, sizeof(record), 1, file) == 1 && fwrite(payload, payload_size, 1, file) == 1;
    }

    ok = fclose(file) == 0 && ok;
    if (! ok) {
        remove(tmp_path);
        return false;
    }

#ifdef _WIN32
    // Windows rename() refuses an existing target. Move the original aside first;
    // if installing the new file then fails, put the original back.
    if (status == Synth::library_valid) {
        char      backup_path[512];
        const int backup_written = snprintf(backup_path, sizeof(backup_path), "%s.bak", path);

        if (backup_written < 0 || backup_written >= static_cast<int>(sizeof(backup_path))) {
            remove(tmp_path);
            return false;
        }

        remove(backup_path);

        if (rename(path, backup_path) != 0) {
            remove(tmp_path);
            return false;
        }

        if (rename(tmp_path, path) != 0) {
            rename(backup_path, path);
            return false;
        }

        remove(backup_path);

        return true;
    }
#endif

    if (rename(tmp_path, path) != 0)
        return false;

    return true;
}

} // namespace

bool Synth::save_library_record(const char* const                  path,
                                const char* const                  category,
                                const char* const                  name,
                                const Synth::InstrumentEditorBank* const src_editor_bank,
                                uint32_t const                     channel)
{
    const Synth::InstrumentBank* const src_bank = &src_editor_bank->bank;
    if (channel >= Synth::max_channels || ! src_bank->channel_enabled[channel])
        return false;

    uint32_t num_zones = 0;

    while (num_zones < Synth::max_instr_per_channel && src_bank->channel_zones[channel][num_zones].start_note != 0)
        num_zones++;

    if (num_zones == 0)
        return false;

    bool keep_instr[Synth::max_instruments] = {};

    for (uint32_t zone = 0; zone < num_zones; zone++) {
        const uint32_t id = src_bank->channel_zones[channel][zone].instrument;

        if (id >= src_bank->instruments.num_allocated || ! src_bank->instruments.is_occupied(id))
            return false;

        keep_instr[id] = true;
    }

    bool keep_env[Synth::max_envelopes] = {};
    bool keep_lfo[Synth::max_lfos]      = {};

    for (uint32_t id = 0; id < src_bank->instruments.num_allocated; id++) {
        if (! keep_instr[id])
            continue;

        const Instrument& src_instrument = src_bank->instruments.entries[id];

        for (uint32_t layer = 0; layer < src_instrument.layer_count; layer++) {
            for (uint32_t target = 0; target < num_mod_targets; target++) {

                const LayerGen& gen = src_instrument.layers[layer].gen[target];

                if (gen.envelope_desc_id && gen.envelope_desc_id <= Synth::max_envelopes)
                    keep_env[gen.envelope_desc_id - 1] = true;

                if (gen.lfo_desc_id && gen.lfo_desc_id <= Synth::max_lfos)
                    keep_lfo[gen.lfo_desc_id - 1] = true;
            }
        }
    }

    static const Synth::InstrumentEditorBank empty_bank = {};
    static Synth::InstrumentEditorBank reduced;
    static uint8_t image[Synth::library_payload_size];

    reduced = empty_bank;

    uint16_t env_ids[Synth::max_envelopes] = {};
    uint16_t lfo_ids[Synth::max_lfos]      = {};

    for (uint32_t id = 1; id <= Synth::max_envelopes; id++) {
        if (! keep_env[id - 1])
            continue;

        const uint32_t slot             = reduced.bank.envelopes.allocate();
        reduced.bank.envelopes.entries[slot] = src_bank->envelopes.entries[id - 1];
        env_ids[id - 1]                 = static_cast<uint16_t>(slot + 1);
    }

    for (uint32_t id = 1; id <= Synth::max_lfos; id++) {
        if (! keep_lfo[id - 1])
            continue;

        const uint32_t slot        = reduced.bank.lfos.allocate();
        reduced.bank.lfos.entries[slot] = src_bank->lfos.entries[id - 1];
        lfo_ids[id - 1]            = static_cast<uint16_t>(slot + 1);
    }

    uint16_t instr_ids[Synth::max_instruments] = {};

    for (uint32_t id = 0; id < src_bank->instruments.num_allocated; id++) {

        if (! keep_instr[id])
            continue;

        const uint32_t slot = reduced.bank.instruments.allocate();

        remap_instrument(src_bank->instruments.entries[id], env_ids, lfo_ids, &reduced.bank.instruments.entries[slot]);

        memcpy(reduced.instrument_names[slot], src_editor_bank->instrument_names[id], Synth::max_name_len);
        instr_ids[id] = static_cast<uint16_t>(slot);
    }

    for (uint32_t zone = 0; zone < num_zones; zone++) {
        reduced.bank.channel_zones[0][zone] = src_bank->channel_zones[channel][zone];
        reduced.bank.channel_zones[0][zone].instrument =
            static_cast<uint8_t>(instr_ids[src_bank->channel_zones[channel][zone].instrument]);
    }

    if (encode_instrument_bank(&reduced, image, sizeof(image)) != sizeof(image))
        return false;

    const uint32_t encoded = encode_instrument_bank(&reduced, image, sizeof(image));
    if (encoded != sizeof(image)) {
        return false;
    }

    return write_library_file(path, category, name, image, encoded);
}
