// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_library.h"

#include "sculptor_atomic_file.h"
#include "sculptor_bank_json.h"
#include "sculptor_effect_graph.h"
#include "sculptor_instr_bank.h"
#include "sculptor_osc_graph.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t library_file_magic = 0x42494C49;

char                         library_transfer_text[64 * 1024];
Synth::Instrument            library_transfer_decoded;
Synth::EnvelopeDescriptor    library_transfer_envs[Synth::instrument_max_envelopes];
Synth::LFODescriptor         library_transfer_lfos[Synth::instrument_max_lfos];
Synth::InstrumentGraphLayout library_transfer_layout[Synth::instrument_graph_layout_capacity];
Synth::InstrumentGraphLayout library_transfer_original_layout[Synth::instrument_graph_layout_capacity];

Synth::InstrumentEditorBank library_load_candidate;
} // namespace

struct RecordHeader {
    char     category[Synth::library_category_len];
    char     name[Synth::library_name_len];
    uint32_t payload_size;
};

static_assert(sizeof(RecordHeader) == 52);

static bool names_terminated(const char* category, const char* name)
{
    return memchr(category, 0, Synth::library_category_len) != nullptr &&
           memchr(name, 0, Synth::library_name_len) != nullptr;
}

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
    if (fread(header, sizeof(header), 1, file) != 1 || header[0] != library_file_magic) {
        fclose(file);
        return 0;
    }

    if (header[1] != Synth::library_version) {
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

    uint32_t num_loaded         = 0;
    uint32_t num_scanned        = 0;
    bool     oversized          = false; // any record past the payload bound, displayable or not
    bool     unterminated_names = false; // a skipped record the rebuild would delete
    while (num_scanned < header[2]) {

        RecordHeader record;
        if (fread(&record, sizeof(record), 1, file) != 1)
            break;

        const long payload_offset = ftell(file);
        if (payload_offset < 0 || record.payload_size > static_cast<uint32_t>(file_size - payload_offset))
            break;

        if (record.payload_size > Synth::library_payload_max)
            oversized = true;
        num_scanned++;

        const bool terminated = names_terminated(record.category, record.name);
        if (! terminated)
            unterminated_names = true;
        if (terminated && num_loaded < max_entries) {
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
    if (oversized) {
        if (out_status)
            *out_status = Synth::library_oversized;
        return num_loaded;
    }
    // A record with unterminated names was skipped from the index: the library
    // is corrupt, and rebuilding over it would silently delete that record.
    if (unterminated_names) {
        if (out_status)
            *out_status = Synth::library_invalid;
        return num_loaded;
    }
    if (out_status)
        *out_status = Synth::library_valid;

    if (out_index_truncated)
        *out_index_truncated = header[2] > max_entries;

    return num_loaded;
}

bool Synth::library_record_matches(const LibraryEntry& entry, const char* category, const char* name)
{
    return strncmp(entry.category, category, Synth::library_category_len) == 0 &&
           strncmp(entry.name, name, Synth::library_name_len) == 0;
}

static bool validate_record(const Synth::InstrumentEditorBank* const editor_bank)
{
    const Synth::InstrumentBank* const bank = &editor_bank->bank;
    if (bank->parameters.num_allocated != 0)
        return false;

    if (editor_bank->effect_audio[Synth::max_channels].explicit_edges) {
        return false;
    }
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

        // The whole-channel record carries the effect chain only on channel 0.
        if (channel != 0 && editor_bank->effect_audio[channel].explicit_edges) {
            return false;
        }
        if (channel != 0 && bank->channel_chains[channel].num_effects != 0)
            return false;

        if (channel == 0)
            continue;

        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {
            if (bank->channel_zones[channel][zone].start_note != 0)
                return false;
        }
    }

    // The record must carry a usable whole-channel zone table: nonempty, anchored
    // at note 0, strictly ascending, referencing occupied instruments, and
    // covering every instrument in the record (no orphans).  Full bank validation
    // skips these because record channels are disabled.
    if (bank->channel_zones[0][0].start_note != 1)
        return false;

    bool    referenced[Synth::max_instruments] = {};
    uint8_t prev_start                         = 0;
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {

        const Synth::Zone& zone_desc = bank->channel_zones[0][zone];

        if (zone_desc.start_note == 0)
            break;

        if (zone_desc.start_note <= prev_start || zone_desc.start_note > 128)
            return false;
        prev_start = zone_desc.start_note;

        if (zone_desc.instrument >= bank->instruments.num_allocated)
            return false;
        referenced[zone_desc.instrument] = true;
    }
    if (prev_start == 0)
        return false;
    for (uint32_t slot = 0; slot < bank->instruments.num_allocated; slot++) {
        if (! referenced[slot])
            return false;
    }

    for (uint32_t index = 0; index < editor_bank->graph_layout_count; ++index) {
        const Synth::GraphNodeLayout& record = editor_bank->graph_layout[index];
        if (record.channel != 0 || (record.kind != 4 && ! bank->channel_zones[0][record.zone].start_note))
            return false;
    }
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
            if (editor_bank->graph_missing_sum[channel][zone])
                return false;
        }
    }

    // Instrument names come from file bytes; a run without NUL would make name
    // readers run past the buffer.
    for (uint32_t slot = 0; slot < bank->instruments.num_allocated; slot++) {
        if (! memchr(editor_bank->instrument_names[slot], 0, Synth::max_name_len))
            return false;
    }

    // The record's chain travels with the instrument: effect types must be real
    // and LFO references must resolve inside the record's dense LFO pool.
    const Synth::EffectChainBinding& chain = bank->channel_chains[0];
    if (chain.num_effects > Synth::max_chain_effects)
        return false;
    for (uint32_t slot = 0; slot < chain.num_effects; slot++) {
        if (static_cast<uint32_t>(chain.effects[slot].type) >= Synth::num_effect_types)
            return false;
        const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
        for (uint32_t param = 0; param < num_params; param++) {
            if (chain.effects[slot].bindings[param].lfo_desc_id > bank->lfos.num_allocated)
                return false;
        }
    }
    return true;
}

static bool transfer_zone_layout(const Synth::InstrumentEditorBank& source,
                                 uint32_t                           channel,
                                 uint32_t                           zone,
                                 const uint16_t*                    env_ids,
                                 const uint16_t*                    lfo_ids,
                                 uint32_t                           dst_channel,
                                 Synth::InstrumentEditorBank*       candidate)
{
    uint32_t       env_count = 0, lfo_count = 0, count = 0, original_count = 0;
    const uint32_t length = Sculptor::encode_editor_instrument_json(source,
                                                                    channel,
                                                                    zone,
                                                                    library_transfer_text,
                                                                    sizeof(library_transfer_text),
                                                                    Synth::InstrumentJsonDomain::validated_bank);
    if (! length || ! Synth::decode_instrument_json_for_domain(library_transfer_text,
                                                               length,
                                                               &library_transfer_decoded,
                                                               library_transfer_envs,
                                                               &env_count,
                                                               library_transfer_lfos,
                                                               &lfo_count,
                                                               library_transfer_layout,
                                                               Synth::instrument_graph_layout_capacity,
                                                               &count,
                                                               Synth::InstrumentJsonDomain::validated_bank))
        return false;
    const Synth::Instrument& original =
        source.bank.instruments.entries[source.bank.channel_zones[channel][zone].instrument];
    return Sculptor::normalize_instrument_graph_layout(library_transfer_decoded,
                                                       library_transfer_envs,
                                                       env_count,
                                                       library_transfer_lfos,
                                                       lfo_count,
                                                       library_transfer_layout,
                                                       count,
                                                       original,
                                                       source.bank.envelopes.entries,
                                                       source.bank.envelopes.num_allocated,
                                                       source.bank.lfos.entries,
                                                       source.bank.lfos.num_allocated,
                                                       library_transfer_original_layout,
                                                       Synth::instrument_graph_layout_capacity,
                                                       &original_count) &&
           Sculptor::append_instrument_graph_layout(original,
                                                    source.bank.envelopes.entries,
                                                    source.bank.envelopes.num_allocated,
                                                    source.bank.lfos.entries,
                                                    source.bank.lfos.num_allocated,
                                                    library_transfer_original_layout,
                                                    original_count,
                                                    env_ids,
                                                    lfo_ids,
                                                    dst_channel,
                                                    zone,
                                                    candidate);
}

bool Synth::load_library_instrument(const char* const            path,
                                    const Synth::LibraryEntry*   entry,
                                    Synth::InstrumentEditorBank* dst_editor_bank,
                                    uint32_t const               channel,
                                    uint16_t* const              out_first_slot)
{
    if (! dst_editor_bank || ! entry || ! out_first_slot || ! Synth::validate_instrument_bank(&dst_editor_bank->bank) ||
        ! Sculptor::validate_editor_metadata(*dst_editor_bank))
        return false;
    library_load_candidate                = *dst_editor_bank;
    Synth::InstrumentBank* const dst_bank = &library_load_candidate.bank;
    if (entry->payload_size > Synth::library_payload_max || channel >= Synth::max_channels)
        return false;

    FILE* const file = fopen(path, "rb");
    if (! file)
        return false;

    RecordHeader record_header;

    const long header_offset = static_cast<long>(entry->payload_offset - sizeof(RecordHeader));

    // Ensure the record in the library did not change since we loaded the index
    const bool header_ok = fseek(file, header_offset, SEEK_SET) == 0 &&
                           fread(&record_header, sizeof(record_header), 1, file) == 1 &&
                           memcmp(record_header.category, entry->category, Synth::library_category_len) == 0 &&
                           memcmp(record_header.name, entry->name, Synth::library_name_len) == 0 &&
                           record_header.payload_size == entry->payload_size &&
                           fseek(file, static_cast<long>(entry->payload_offset), SEEK_SET) == 0;

    // The read validates and canonicalizes into the record scratch and leaves it
    // untouched on failure; the merge below reads the scratch with no decode
    // between.
    static Synth::InstrumentEditorBank record_bank;
    const bool load_ok = header_ok && Synth::read_editor_bank_json(file, entry->payload_size, &record_bank, true);
    fclose(file);
    if (! load_ok)
        return false;

    if (! validate_record(&record_bank))
        return false;

    const uint32_t num_envelopes = record_bank.bank.envelopes.num_allocated;
    const uint32_t num_lfos      = record_bank.bank.lfos.num_allocated;
    const uint32_t num_instrs    = record_bank.bank.instruments.num_allocated;

    if (dst_bank->envelopes.num_allocated + num_envelopes > Synth::max_envelopes ||
        dst_bank->lfos.num_allocated + num_lfos > Synth::max_lfos ||
        dst_bank->instruments.num_allocated + num_instrs > Synth::max_instruments)
        return false;

    uint16_t env_ids[Synth::max_envelopes] = {};
    uint16_t lfo_ids[Synth::max_lfos]      = {};

    for (uint32_t i = 0; i < num_envelopes; i++) {
        const uint32_t slot               = dst_bank->envelopes.allocate();
        dst_bank->envelopes.entries[slot] = record_bank.bank.envelopes.entries[i];
        env_ids[i]                        = static_cast<uint16_t>(slot + 1);
    }

    if (! Synth::remap_lfos(dst_bank, record_bank.bank.lfos.entries, num_lfos, lfo_ids)) {
        return false;
    }

    const uint32_t first_slot = dst_bank->instruments.num_allocated;

    for (uint32_t i = 0; i < num_instrs; i++) {
        const uint32_t slot = dst_bank->instruments.allocate();
        remap_instrument(record_bank.bank.instruments.entries[i],
                         env_ids,
                         lfo_ids,
                         &dst_bank->instruments.entries[slot]);
        memcpy(library_load_candidate.instrument_names[slot], record_bank.instrument_names[i], Synth::max_name_len);
    }

    Sculptor::channel_records_reset(&library_load_candidate, channel);

    memset(dst_bank->channel_zones[channel], 0, sizeof(dst_bank->channel_zones[channel]));
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; zone++) {

        const Synth::Zone& zone_src = record_bank.bank.channel_zones[0][zone];

        if (zone_src.start_note == 0)
            break;

        dst_bank->channel_zones[channel][zone]            = zone_src;
        dst_bank->channel_zones[channel][zone].instrument = static_cast<uint8_t>(zone_src.instrument + first_slot);
    }

    // The record's chain replaces the target channel's chain; the load flow's
    // subsequent reclaim frees the replaced chain's exclusive LFOs.
    Synth::remap_effect_chain(record_bank.bank.channel_chains[0], lfo_ids, &dst_bank->channel_chains[channel]);
    library_load_candidate.effect_audio[channel] = record_bank.effect_audio[0];

    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel && record_bank.bank.channel_zones[0][zone].start_note;
         ++zone) {
        if (! transfer_zone_layout(record_bank, 0, zone, env_ids, lfo_ids, channel, &library_load_candidate))
            return false;
    }
    if (! Sculptor::transfer_effect_layout(&record_bank, 0, lfo_ids, channel, &library_load_candidate) ||
        ! Synth::validate_instrument_bank(&library_load_candidate.bank) ||
        ! Sculptor::validate_editor_metadata(library_load_candidate))
        return false;
    *dst_editor_bank = library_load_candidate;
    *out_first_slot  = static_cast<uint16_t>(first_slot);
    return true;
}

static int write_library_file(const char* const                        path,
                              const char* const                        category,
                              const char* const                        name,
                              const Synth::InstrumentEditorBank* const editor_bank,
                              Synth::LibraryScanStatus* const          out_status)
{
    if (out_status)
        *out_status = Synth::library_invalid;

    static Synth::LibraryEntry entries[Synth::library_max_records];

    Synth::LibraryScanStatus status          = Synth::library_invalid;
    bool                     index_truncated = false;

    const uint32_t num_entries =
        Synth::read_library_index(path, entries, Synth::library_max_records, &status, &index_truncated);

    // Only a fully loadable index may be rebuilt: invalid or truncated input
    // stays untouched, and an oversized record (flagged by the scan even when its
    // names are unusable) can never be kept.
    if (status != Synth::library_valid && status != Synth::library_absent) {
        if (out_status)
            *out_status = status;
        return EINVAL;
    }
    if (index_truncated)
        return EINVAL;

    uint32_t num_kept = 0;
    bool     replace  = false;

    for (uint32_t i = 0; i < num_entries; i++) {
        if (Synth::library_record_matches(entries[i], category, name))
            replace = true;
        else
            num_kept++;
    }

    if (! replace && num_kept + 1 > Synth::library_max_records)
        return EINVAL;

    char                           tmp_path[512];
    const std::variant<FILE*, int> staged = atomic_write_begin(path, tmp_path, sizeof(tmp_path));
    if (std::holds_alternative<int>(staged))
        return std::get<int>(staged);
    FILE* const file = std::get<FILE*>(staged);

    const uint32_t header[3] = { library_file_magic, Synth::library_version, num_kept + 1 };
    bool           ok        = fwrite(header, sizeof(header), 1, file) == 1;

    FILE* const old = (ok && status == Synth::library_valid) ? fopen(path, "rb") : nullptr;
    if (status == Synth::library_valid && ! old)
        ok = false;

    if (old) {
        constexpr uint32_t copy_chunk_size = 64 * 1024;
        static uint8_t     record_copy[copy_chunk_size];

        for (uint32_t i = 0; ok && i < num_entries; i++) {
            if (Synth::library_record_matches(entries[i], category, name))
                continue;

            const uint32_t header_offset = entries[i].payload_offset - sizeof(RecordHeader);
            const uint32_t record_size   = sizeof(RecordHeader) + entries[i].payload_size;

            ok = fseek(old, static_cast<long>(header_offset), SEEK_SET) == 0;

            // Records pass through in bounded chunks: a payload up to the capacity
            // needs no second full-size staging buffer.
            for (uint32_t remaining = record_size; ok && remaining;) {
                const uint32_t chunk = remaining < copy_chunk_size ? remaining : copy_chunk_size;

                ok = fread(record_copy, chunk, 1, old) == 1 && fwrite(record_copy, chunk, 1, file) == 1;
                remaining -= chunk;
            }
        }

        fclose(old);
    }

    if (ok) {
        RecordHeader record;
        snprintf(record.category, sizeof(record.category), "%s", category);
        snprintf(record.name, sizeof(record.name), "%s", name);
        record.payload_size   = 0;
        const long record_pos = ftell(file);
        ok                    = record_pos >= 0 && fwrite(&record, sizeof(record), 1, file) == 1;
        uint32_t payload_size = 0;
        if (ok)
            ok = Synth::write_editor_bank_json(file, editor_bank, Synth::library_payload_max, &payload_size) == 0;
        // The staging file is seekable: the size field is patched once the encoded
        // length is known.
        if (ok) {
            record.payload_size = payload_size;
            ok = fseek(file, record_pos, SEEK_SET) == 0 && fwrite(&record, sizeof(record), 1, file) == 1 &&
                 fseek(file, 0, SEEK_END) == 0;
        }
    }
    if (! ok) {
        fclose(file);
        remove(tmp_path);
        return EIO;
    }

    // Flush + rename inside the commit makes the replacement crash-atomic:
    // readers see either the complete previous library or the complete new one.
    if (out_status)
        *out_status = Synth::library_valid;
    return atomic_write_commit(path, tmp_path, file);
}

int Synth::save_library_record(const char* const                        path,
                               const char* const                        category,
                               const char* const                        name,
                               const Synth::InstrumentEditorBank* const src_editor_bank,
                               uint32_t const                           channel,
                               Synth::LibraryScanStatus* const          out_status)
{
    if (out_status)
        *out_status = Synth::library_invalid;

    const Synth::InstrumentBank* const src_bank = &src_editor_bank->bank;
    if (! Synth::validate_instrument_bank(src_bank) || ! Sculptor::validate_editor_metadata(*src_editor_bank) ||
        channel >= Synth::max_channels || ! src_bank->channel_enabled[channel])
        return EINVAL;

    uint32_t num_zones = 0;

    while (num_zones < Synth::max_instr_per_channel && src_bank->channel_zones[channel][num_zones].start_note != 0)
        num_zones++;

    if (num_zones == 0)
        return EINVAL;

    bool keep_instr[Synth::max_instruments] = {};

    for (uint32_t zone = 0; zone < num_zones; zone++) {
        const uint32_t id = src_bank->channel_zones[channel][zone].instrument;

        if (id >= src_bank->instruments.num_allocated || ! src_bank->instruments.is_occupied(id))
            return EINVAL;

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

    const Synth::EffectChainBinding& src_chain = src_bank->channel_chains[channel];
    if (! Sculptor::collect_effect_lfos(src_editor_bank, channel, keep_lfo)) {
        return EINVAL;
    }

    static Synth::InstrumentEditorBank reduced;

    memset(&reduced, 0, sizeof(reduced));

    uint16_t env_ids[Synth::max_envelopes] = {};
    uint16_t lfo_ids[Synth::max_lfos]      = {};

    for (uint32_t id = 1; id <= Synth::max_envelopes; id++) {
        if (! keep_env[id - 1])
            continue;

        const uint32_t slot                  = reduced.bank.envelopes.allocate();
        reduced.bank.envelopes.entries[slot] = src_bank->envelopes.entries[id - 1];
        env_ids[id - 1]                      = static_cast<uint16_t>(slot + 1);
    }

    for (uint32_t id = 1; id <= Synth::max_lfos; id++) {
        if (! keep_lfo[id - 1])
            continue;

        if (! Synth::remap_lfos(&reduced.bank, src_bank->lfos.entries + id - 1, 1, lfo_ids + id - 1)) {
            return EINVAL;
        }
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

    // The chain rides along with its LFO references remapped; the zero-initialized
    // map sends an out-of-range reference to none so it cannot survive the record.
    Synth::remap_effect_chain(src_chain, lfo_ids, &reduced.bank.channel_chains[0]);
    reduced.effect_audio[0] = src_editor_bank->effect_audio[channel];

    for (uint32_t zone = 0; zone < num_zones; zone++) {
        reduced.bank.channel_zones[0][zone] = src_bank->channel_zones[channel][zone];
        reduced.bank.channel_zones[0][zone].instrument =
            static_cast<uint8_t>(instr_ids[src_bank->channel_zones[channel][zone].instrument]);
    }

    for (uint32_t zone = 0; zone < num_zones; ++zone) {
        if (! transfer_zone_layout(*src_editor_bank, channel, zone, env_ids, lfo_ids, 0, &reduced))
            return EINVAL;
    }
    if (! Sculptor::transfer_effect_layout(src_editor_bank, channel, lfo_ids, 0, &reduced) ||
        ! Synth::validate_instrument_bank(&reduced.bank) || ! Sculptor::validate_editor_metadata(reduced))
        return EINVAL;
    return write_library_file(path, category, name, &reduced, out_status);
}
