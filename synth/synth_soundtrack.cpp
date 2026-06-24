// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_soundtrack.h"
#include "synth_config.h"

namespace Synth {

// Upper bound on events a single encode/decode handles.  Project soundtracks stay well under this.
static constexpr uint32_t max_soundtrack_events = 65536;

// Pairing scratch: the in-progress note_on event index (plus one; 0 means none) per channel/note,
// and the resolved stored duration per input event (only note_on entries are read back).
static uint32_t pending_note_on[max_channels][128];
static uint32_t event_duration[max_soundtrack_events];

// Event types stored in the soundtrack.
// Some MIDI events are not stored in the soundtrack, specifically:
// - note_off is folded into a note_on duration
// - program_change and channel_pressure are not supported, so dropped.
static bool is_stored_event(EvType event)
{
    switch (event) {
        case EvType::note_on:
        case EvType::aftertouch:
        case EvType::controller:
        case EvType::pitch_bend:
            return true;

        default:
            return false;
    }
}

// Writes a single byte, guarding the destination bound.
static bool write_byte(uint8_t* dest, uint32_t dest_size, uint32_t* pos, uint8_t value)
{
    if (*pos >= dest_size) {
        return false;
    }

    dest[*pos] = value;
    (*pos)++;

    return true;
}

// Writes a variable-length quantity (7 bits per byte, high bit marks continuation), MSB-first.
static bool write_vlq(uint8_t* dest, uint32_t dest_size, uint32_t* pos, uint32_t value)
{
    uint8_t  group[5];
    uint32_t group_count = 0;

    do {
        group[group_count] = static_cast<uint8_t>(value & 0x7Fu);
        group_count++;
        value >>= 7;
    } while (value != 0);

    for (uint32_t index = 0; index < group_count; index++) {
        const uint32_t source = group_count - 1 - index;
        const uint8_t  cont   = (source != 0) ? 0x80u : 0x00u;

        if ( ! write_byte(dest, dest_size, pos, static_cast<uint8_t>(group[source] | cont))) {
            return false;
        }
    }

    return true;
}

// Reads a variable-length quantity written by write_vlq, advancing *ptr.
static uint32_t read_vlq(const uint8_t** ptr)
{
    uint32_t value = 0;

    for (;;) {
        const uint8_t byte = *(*ptr)++;

        value = (value << 7) | (byte & 0x7Fu);

        if ((byte & 0x80u) == 0) {
            return value;
        }
    }
}

// Resolves note_on/note_off pairing into event_duration[i] for every input event.  A note_on is
// matched to the next note_off on the same channel and note; an overlapping re-trigger closes the
// earlier note at the new note_on's time.  Unmatched note_ons stay unbounded.
static void pair_note_durations(const MidiEvent* events, uint32_t count)
{
    for (uint32_t channel = 0; channel < max_channels; channel++) {
        for (uint32_t note = 0; note < 128; note++) {
            pending_note_on[channel][note] = 0;
        }
    }

    for (uint32_t index = 0; index < count; index++) {
        event_duration[index] = soundtrack_duration_unbounded;

        const MidiEvent& event = events[index];
        if (event.event != EvType::note_on && event.event != EvType::note_off) {
            continue;
        }

        const uint32_t open = pending_note_on[event.channel][event.note];
        if (open != 0) {
            const uint32_t duration = event.time - events[open - 1].time;
            event_duration[open - 1] = duration + 1; // stored value, reserving 0 for unbounded
        }

        pending_note_on[event.channel][event.note] =
            (event.event == EvType::note_on) ? (index + 1) : 0;
    }
}

uint32_t encode_soundtrack(const MidiEvent*  events,
                           const uint32_t    num_events,
                           uint8_t* const    dest,
                           const uint32_t    dest_size,
                           Soundtrack* const out)
{
    if (num_events > max_soundtrack_events) {
        return 0;
    }

    uint32_t used_channels = 0;

    for (uint32_t index = 0; index < num_events; index++) {

        const MidiEvent& event = events[index];
        if (event.channel >= max_channels) {
            return 0;
        }

        // Reject malformed data bytes: note/data values index static pairing scratch and must
        // stay 7-bit.  This guards pending_note_on[channel][note] against out-of-bounds access.
        if (event.event == EvType::note_on || event.event == EvType::note_off ||
            event.event == EvType::aftertouch) {
            if (event.note >= 128 || event.note_data >= 128) {
                return 0;
            }
        }
        else if (event.event == EvType::controller) {
            if (event.controller >= 128 || event.controller_data >= 128) {
                return 0;
            }
        }

        if (event.channel + 1u > used_channels) {
            used_channels = event.channel + 1u;
        }
    }

    out->num_channels = used_channels;

    pair_note_durations(events, num_events);

    uint32_t pos = 0;

    // Delta plane: per-channel VLQ tick deltas, end_of_channel terminated.
    for (uint32_t channel = 0; channel < used_channels; channel++) {

        out->delta_times[channel] = dest + pos;

        uint32_t prev_time = 0;
        for (uint32_t index = 0; index < num_events; index++) {

            if (events[index].channel != channel || ! is_stored_event(events[index].event)) {
                continue;
            }

            if ( ! write_vlq(dest, dest_size, &pos, events[index].time - prev_time)) {
                return 0;
            }

            prev_time = events[index].time;
        }

        if ( ! write_vlq(dest, dest_size, &pos, soundtrack_end_of_channel)) {
            return 0;
        }
    }

    // Event plane: 4-bit event codes, two per byte, high nibble first.
    for (uint32_t channel = 0; channel < used_channels; channel++) {

        out->events[channel] = dest + pos;

        bool    second_ev = false;
        uint8_t high      = 0;

        for (uint32_t index = 0; index < num_events; index++) {

            if (events[index].channel != channel || ! is_stored_event(events[index].event)) {
                continue;
            }

            const uint8_t code = static_cast<uint8_t>(events[index].event);

            if ( ! second_ev) {
                high = code;
            }
            else if ( ! write_byte(dest, dest_size, &pos, static_cast<uint8_t>((high << 4) | code))) {
                return 0;
            }
            second_ev = ! second_ev;
        }

        if (second_ev) {
            if ( ! write_byte(dest, dest_size, &pos, static_cast<uint8_t>(high << 4))) {
                return 0;
            }
        }
    }

    // Note and note_data planes: one byte each per note_on / aftertouch event.
    for (uint32_t plane = 0; plane < 2; plane++) {
        for (uint32_t channel = 0; channel < used_channels; channel++) {

            (plane ? out->note_data : out->notes)[channel] = dest + pos;

            for (uint32_t index = 0; index < num_events; index++) {

                if (events[index].channel != channel) {
                    continue;
                }

                if (events[index].event != EvType::note_on && events[index].event != EvType::aftertouch) {
                    continue;
                }

                const uint8_t value = plane ? events[index].note_data : events[index].note;
                if ( ! write_byte(dest, dest_size, &pos, value)) {
                    return 0;
                }
            }
        }
    }

    // Duration plane: one VLQ tick duration per note_on (stored value, 0 = unbounded).
    for (uint32_t channel = 0; channel < used_channels; channel++) {

        out->durations[channel] = dest + pos;

        for (uint32_t index = 0; index < num_events; index++) {

            if (events[index].channel != channel || events[index].event != EvType::note_on) {
                continue;
            }

            if ( ! write_vlq(dest, dest_size, &pos, event_duration[index])) {
                return 0;
            }
        }
    }

    // Controller planes: number and value, one byte each per controller event.
    for (uint32_t plane = 0; plane < 2; plane++) {
        for (uint32_t channel = 0; channel < used_channels; channel++) {

            (plane ? out->ctrl_data : out->ctrl)[channel] = dest + pos;

            for (uint32_t index = 0; index < num_events; index++) {

                if (events[index].channel != channel || events[index].event != EvType::controller) {
                    continue;
                }

                const uint8_t value = plane ? events[index].controller_data : events[index].controller;
                if ( ! write_byte(dest, dest_size, &pos, value)) {
                    return 0;
                }
            }
        }
    }

    // Pitch-bend planes: 7-bit LSB and MSB of the centered 14-bit value, per pitch_bend event.
    for (uint32_t plane = 0; plane < 2; plane++) {
        for (uint32_t channel = 0; channel < used_channels; channel++) {

            (plane ? out->pitch_bend_hi : out->pitch_bend_lo)[channel] = dest + pos;

            for (uint32_t index = 0; index < num_events; index++) {

                if (events[index].channel != channel || events[index].event != EvType::pitch_bend) {
                    continue;
                }

                const uint32_t raw14 = static_cast<uint32_t>(events[index].pitch_bend + soundtrack_pitch_bend_center);
                const uint8_t  value = static_cast<uint8_t>((plane ? (raw14 >> 7) : raw14) & 0x7Fu);
                if ( ! write_byte(dest, dest_size, &pos, value)) {
                    return 0;
                }
            }
        }
    }

    return pos;
}

// Stable insertion sort by (time, channel).  Event counts stay small enough that the O(n^2) worst
// case is acceptable, and it needs no scratch storage.
static void sort_by_time_channel(MidiEvent* events, uint32_t count)
{
    for (uint32_t outer = 1; outer < count; outer++) {

        const MidiEvent key = events[outer];

        uint32_t inner = outer;
        while (inner > 0 &&
               (events[inner - 1].time > key.time ||
                (events[inner - 1].time == key.time && events[inner - 1].channel > key.channel))) {

            events[inner] = events[inner - 1];
            inner--;
        }

        events[inner] = key;
    }
}

uint32_t decode_soundtrack(const Soundtrack& soundtrack, MidiEvent* events, uint32_t max_events)
{
    uint32_t out = 0;

    for (uint32_t channel = 0; channel < soundtrack.num_channels; channel++) {
        const uint8_t* delta_ptr     = soundtrack.delta_times[channel];
        const uint8_t* note_ptr      = soundtrack.notes[channel];
        const uint8_t* note_data_ptr = soundtrack.note_data[channel];
        const uint8_t* duration_ptr  = soundtrack.durations[channel];
        const uint8_t* ctrl_ptr      = soundtrack.ctrl[channel];
        const uint8_t* ctrl_data_ptr = soundtrack.ctrl_data[channel];
        const uint8_t* bend_lo_ptr   = soundtrack.pitch_bend_lo[channel];
        const uint8_t* bend_hi_ptr   = soundtrack.pitch_bend_hi[channel];
        const uint8_t* event_ptr     = soundtrack.events[channel];

        uint32_t event_index = 0;
        uint32_t time        = 0;

        for (;;) {
            const uint32_t delta = read_vlq(&delta_ptr);
            if (delta == soundtrack_end_of_channel) {
                break;
            }

            time += delta;

            // Two event codes per byte, high nibble first (the second of each pair is the low nibble).
            const uint8_t packed = event_ptr[event_index / 2];
            const uint8_t code   = (event_index & 1u) ? (packed & 0xFu)
                                                       : static_cast<uint8_t>((packed >> 4) & 0xFu);
            event_index++;

            if (out >= max_events) {
                return 0;
            }

            MidiEvent& decoded = events[out];
            decoded         = MidiEvent { };
            decoded.time    = time;
            decoded.channel = static_cast<uint8_t>(channel);
            decoded.event   = static_cast<EvType>(code);

            out++;

            if (static_cast<EvType>(code) == EvType::controller) {
                decoded.controller      = *(ctrl_ptr++);
                decoded.controller_data = *(ctrl_data_ptr++);
            }
            else if (static_cast<EvType>(code) == EvType::pitch_bend) {
                const int lo = *(bend_lo_ptr++);
                const int hi = *(bend_hi_ptr++);

                decoded.pitch_bend = static_cast<int16_t>((hi << 7) + lo - soundtrack_pitch_bend_center);
            }
            else {
                decoded.note      = *(note_ptr++);
                decoded.note_data = *(note_data_ptr++);

                if (static_cast<EvType>(code) == EvType::note_on) {
                    const uint32_t stored = read_vlq(&duration_ptr);

                    if (stored != soundtrack_duration_unbounded) {
                        if (out >= max_events) {
                            return 0;
                        }

                        MidiEvent& note_off = events[out];
                        note_off           = MidiEvent { };
                        note_off.time      = time + (stored - 1);
                        note_off.channel   = static_cast<uint8_t>(channel);
                        note_off.event     = EvType::note_off;
                        note_off.note      = decoded.note;
                        note_off.note_data = 0;

                        out++;
                    }
                }
            }
        }
    }

    sort_by_time_channel(events, out);

    return out;
}

uint32_t soundtrack_note_release_sample(uint32_t note_start, uint32_t stored_duration,
                                        uint32_t samples_per_tick)
{
    if (stored_duration == soundtrack_duration_unbounded) {
        return 0;
    }

    return note_start + (stored_duration - 1) * samples_per_tick;
}

} // namespace Synth
