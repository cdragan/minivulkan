// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "midi_file.h"
#include "midi_decode.h"

namespace Synth {

// Upper bound on events buffered during the merge.  Standard MIDI files in this project
// stay well under this; a malformed file that would exceed it is rejected.
static constexpr uint32_t max_merge_events = 65536;

// Default tempo before any FF 51 tempo meta event: 500000 us/quarter = 120 BPM.
static constexpr uint32_t default_us_per_quarter = 500000;

// One merged event with its absolute tick position.  Tempo changes ride the same timeline
// as channel-voice events so the tick-to-sample conversion sees them in order.
struct MergedEvent {
    uint32_t abs_tick;        // Absolute tick position from the start of the track set
    uint32_t order;           // Stable-sort tiebreaker preserving track/insertion order
    bool     is_tempo;        // True for a tempo change, false for a channel-voice event
    uint32_t us_per_quarter;  // Tempo value when is_tempo is set
    uint8_t  status;          // Channel-voice status byte when is_tempo is clear
    uint8_t  data0;
    uint8_t  data1;
};

static MergedEvent merge_buffer[max_merge_events];

// Reads a big-endian unsigned integer of the given byte width.  The caller guarantees the
// range is in bounds.
static uint32_t read_be(const uint8_t* data, uint32_t width)
{
    uint32_t value = 0;

    for (uint32_t index = 0; index < width; index++) {
        value = (value << 8) | data[index];
    }

    return value;
}

// Reads a variable-length quantity at *pos, advancing it.  Returns false if the buffer ends
// before the VLQ terminates or it spans more than 4 bytes (the SMF limit, 28 bits).
static bool read_vlq(const uint8_t* data, uint32_t size, uint32_t* pos, uint32_t* out)
{
    uint32_t value = 0;

    for (uint32_t consumed = 0; consumed < 4; consumed++) {

        if (*pos >= size) {
            return false;
        }

        const uint8_t byte = data[*pos];
        (*pos)++;

        value = (value << 7) | (byte & 0x7Fu);

        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
    }

    return false;
}

static uint8_t channel_voice_data_bytes(uint8_t status)
{
    switch (status & 0xF0u) {
        case 0xC0u: // program change
        case 0xD0u: // channel pressure
            return 1;

        default:
            return 2;
    }
}

// Parses one MTrk into merge_buffer, appending to *out_size.  Returns false on any malformation.
static bool parse_track(const uint8_t* data,
                        uint32_t       track_begin,
                        uint32_t       track_end,
                        uint32_t*      out_size,
                        uint32_t*      order)
{
    uint32_t pos        = track_begin;
    uint32_t abs_tick   = 0;
    uint8_t  cur_status = 0;

    while (pos < track_end) {

        uint32_t delta = 0;
        if ( ! read_vlq(data, track_end, &pos, &delta)) {
            return false;
        }
        abs_tick += delta;

        if (pos >= track_end) {
            return false;
        }

        uint8_t status = data[pos];
        if (status & 0x80u) {
            pos++;
        }
        else {
            // Reuse the previous channel-voice status byte
            status = cur_status;
            if ((status & 0x80u) == 0) {
                return false;
            }
        }

        // Meta event: type byte, VLQ length, payload
        if (status == 0xFFu) {

            cur_status = 0;
            if (pos >= track_end) {
                return false;
            }

            const uint8_t meta_type = data[pos];
            pos++;

            uint32_t meta_len = 0;
            if ( ! read_vlq(data, track_end, &pos, &meta_len)) {
                return false;
            }

            if (meta_len > track_end - pos) {
                return false;
            }

            if (meta_type == 0x51u && meta_len == 3) {

                if (*out_size >= max_merge_events) {
                    return false;
                }

                MergedEvent* event = &merge_buffer[*out_size];

                event->abs_tick       = abs_tick;
                event->order          = *order;
                event->is_tempo       = true;
                event->us_per_quarter = read_be(&data[pos], 3);
                event->status         = 0;
                event->data0          = 0;
                event->data1          = 0;

                (*out_size)++;
                (*order)++;
            }

            pos += meta_len;
            continue;
        }

        // SysEx: VLQ length then payload, consumed and not emitted.
        if (status == 0xF0u || status == 0xF7u) {

            cur_status = 0;

            uint32_t sysex_len = 0;
            if ( ! read_vlq(data, track_end, &pos, &sysex_len)) {
                return false;
            }

            if (sysex_len > track_end - pos) {
                return false;
            }

            pos += sysex_len;
            continue;
        }

        // System common or stray data byte: unsupported, reject.
        if ((status & 0x80u) == 0 || (status & 0xF0u) == 0xF0u) {
            return false;
        }

        // Channel-voice message.
        cur_status = status;

        const uint8_t num_data = channel_voice_data_bytes(status);

        if (num_data > track_end - pos) {
            return false;
        }

        if (*out_size >= max_merge_events) {
            return false;
        }

        MergedEvent* event = &merge_buffer[*out_size];

        event->abs_tick       = abs_tick;
        event->order          = *order;
        event->is_tempo       = false;
        event->us_per_quarter = 0;
        event->status         = status;
        event->data0          = data[pos];
        event->data1          = (num_data > 1) ? data[pos + 1] : 0;

        pos += num_data;
        (*out_size)++;
        (*order)++;
    }

    return true;
}

// Sort channel for the merge order.  Tempo changes carry no channel; ordering them as channel 0
// among same-tick events is timing-neutral because a same-tick delta is zero.
static uint8_t merged_sort_channel(const MergedEvent& event)
{
    return event.is_tempo ? 0u : static_cast<uint8_t>(event.status & 0x0Fu);
}

// True if left should be ordered after right, lexicographically by (abs_tick, channel, order).
// Ordering by channel makes the emitted stream canonical (by time, then channel), matching the
// music-score codec so a parse -> encode -> decode round-trip is exact.
static bool merged_orders_after(const MergedEvent& left, const MergedEvent& right)
{
    if (left.abs_tick != right.abs_tick) {
        return left.abs_tick > right.abs_tick;
    }

    const uint8_t left_channel  = merged_sort_channel(left);
    const uint8_t right_channel = merged_sort_channel(right);

    if (left_channel != right_channel) {
        return left_channel > right_channel;
    }

    return left.order > right.order;
}

// Stable insertion sort.  The event count stays small enough that the O(n^2) worst case is
// acceptable, and it needs no scratch storage.
static void sort_merged(uint32_t count)
{
    for (uint32_t outer = 1; outer < count; outer++) {

        const MergedEvent key = merge_buffer[outer];

        uint32_t inner = outer;
        while (inner > 0 && merged_orders_after(merge_buffer[inner - 1], key)) {
            merge_buffer[inner] = merge_buffer[inner - 1];
            inner--;
        }

        merge_buffer[inner] = key;
    }
}

uint32_t parse_midi_file(const uint8_t* data,
                         uint32_t       size,
                         MidiEvent*     events,
                         uint32_t       max_events,
                         uint32_t       sampling_rate,
                         bool           emit_ticks)
{
    constexpr uint32_t header_chunk_size = 14; // 4 magic + 4 length + 6 body
    if (data == nullptr || size < header_chunk_size) {
        return 0;
    }

    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        return 0;
    }

    const uint32_t header_len = read_be(&data[4], 4);
    if (header_len != 6) {
        return 0;
    }

    const uint32_t format = read_be(&data[8], 2);
    if (format != 0 && format != 1) {
        return 0;
    }

    const uint32_t division = read_be(&data[12], 2);
    if (division & 0x8000u) {
        return 0; // SMPTE timing is not supported.
    }
    if (division == 0) {
        return 0;
    }

    uint32_t merged_count = 0;
    uint32_t order        = 0;
    uint32_t pos          = header_chunk_size;

    while (pos + 8 <= size) {

        const bool is_track = data[pos]     == 'M' &&
                              data[pos + 1] == 'T' &&
                              data[pos + 2] == 'r' &&
                              data[pos + 3] == 'k';

        const uint32_t chunk_len = read_be(&data[pos + 4], 4);
        if (chunk_len > size - (pos + 8)) {
            return 0; // Truncated or malformed chunk length.
        }

        const uint32_t chunk_begin = pos + 8;
        if (is_track) {
            if ( ! parse_track(data, chunk_begin, chunk_begin + chunk_len, &merged_count, &order)) {
                return 0;
            }
        }

        pos = chunk_begin + chunk_len;
    }

    sort_merged(merged_count);

    // Walk the merged stream, accumulating elapsed samples piecewise across tempo changes.
    uint32_t out_count       = 0;
    uint32_t prev_tick       = 0;
    uint32_t us_per_quarter  = default_us_per_quarter;
    double   elapsed_samples = 0.0;

    MidiDecoder decoder = { };

    for (uint32_t index = 0; index < merged_count; index++) {
        const MergedEvent* merged = &merge_buffer[index];

        // Advance the sample clock to this event's tick using the current tempo.
        const double samples_per_tick =
            (static_cast<double>(us_per_quarter) / static_cast<double>(division)) *
            static_cast<double>(sampling_rate) / 1.0e6;

        elapsed_samples += static_cast<double>(merged->abs_tick - prev_tick) * samples_per_tick;
        prev_tick       =  merged->abs_tick;

        if (merged->is_tempo) {
            us_per_quarter = merged->us_per_quarter;
            continue;
        }

        // Decode the channel-voice message by feeding its bytes through the shared decoder.
        decoder.status     = 0;
        decoder.data_count = 0;
        decoder.in_sysex   = false;

        MidiEvent decoded    = { };

        const uint8_t num_data = channel_voice_data_bytes(merged->status);

        bool have_event = midi_decode_byte(&decoder, merged->status, &decoded);
        have_event      = midi_decode_byte(&decoder, merged->data0, &decoded) || have_event;
        if (num_data > 1) {
            have_event  = midi_decode_byte(&decoder, merged->data1, &decoded) || have_event;
        }
        if ( ! have_event) {
            continue;
        }

        if (out_count >= max_events) {
            break;
        }

        if (emit_ticks) {
            decoded.time = merged->abs_tick;
        }
        else {
            // Reject a file whose sample time outruns the 32-bit time field: a small file with an
            // extreme tempo and fine division can do this, and the cast would otherwise be UB.
            const double rounded_samples = elapsed_samples + 0.5;
            if (rounded_samples > static_cast<double>(UINT32_MAX)) {
                return 0;
            }

            decoded.time = static_cast<uint32_t>(rounded_samples);
        }

        events[out_count] = decoded;
        out_count++;
    }

    return out_count;
}

} // namespace Synth
