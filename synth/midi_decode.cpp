// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "midi_decode.h"

#include <assert.h>

namespace Synth {

// MIDI status byte ranges.
constexpr uint8_t status_bit            = 0x80; // Set on every status byte
constexpr uint8_t system_common         = 0xF0; // 0xF0..0xF7: System Common (incl. SysEx)
constexpr uint8_t system_realtime_event = 0xF8; // 0xF8..0xFF: System Real-Time
constexpr uint8_t sysex_start           = 0xF0;

constexpr int pitch_bend_center = 8192; // 14-bit center; our MidiEvent.pitch_bend is centered

static uint8_t get_num_data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
        case 0xC0: // program change
        case 0xD0: // channel pressure
            return 1;
        default:
            return 2;
    }
}

static void decode_event(uint8_t status, const uint8_t* data, MidiEvent* out)
{
    out->time    = 0;
    out->channel = status & 0x0F;

    switch (status & 0xF0) {
        case 0x80:
            out->event     = EvType::note_off;
            out->note      = data[0];
            out->note_data = data[1];
            break;

        case 0x90:
            out->event     = data[1] ? EvType::note_on : EvType::note_off;
            out->note      = data[0];
            out->note_data = data[1];
            break;

        case 0xA0:
            out->event     = EvType::aftertouch;
            out->note      = data[0];
            out->note_data = data[1];
            break;

        case 0xB0:
            out->event           = EvType::controller;
            out->controller      = data[0];
            out->controller_data = data[1];
            break;

        case 0xC0:
            out->event     = EvType::program_change;
            out->note      = 0;
            out->note_data = data[0];
            break;

        case 0xD0:
            out->event     = EvType::channel_pressure;
            out->note      = 0;
            out->note_data = data[0];
            break;

        default:
            assert((status & 0xF0) == 0xE0);
            out->event      = EvType::pitch_bend;
            out->pitch_bend = static_cast<int16_t>(
                ((static_cast<int>(data[1]) << 7) | data[0]) - pitch_bend_center);
            break;
    }
}

bool midi_decode_byte(MidiDecoder* decoder, uint8_t byte, MidiEvent* out)
{
    // Ignore System Real-Time events
    if (byte >= system_realtime_event) {
        return false;
    }

    if (byte & status_bit) {
        if (byte >= system_common) {
            // System Common (incl. SysEx start/end) cancels running status.
            decoder->status   = 0;
            decoder->in_sysex = (byte == sysex_start);
            return false;
        }

        // Channel-voice status begins a new message.
        decoder->status     = byte;
        decoder->data_count = 0;
        decoder->in_sysex   = false;
        return false;
    }

    // Ignore data byte and sysex
    if (decoder->in_sysex || ! decoder->status) {
        return false;
    }

    decoder->data[decoder->data_count++] = byte;

    if (decoder->data_count < get_num_data_bytes(decoder->status)) {
        return false;
    }

    decoder->data_count = 0;
    decode_event(decoder->status, decoder->data, out);
    return true;
}

} // namespace Synth
