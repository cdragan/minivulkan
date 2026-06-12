// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "midi_decode.h"
#include <stdio.h>

#define TEST(test) if ( ! (test)) { failed(#test, __FILE__, __LINE__); }

static int exit_code = 0;

static void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n", file, line, test);
}

// Decodes a whole byte stream through a fresh decoder, collecting completed events.
// Returns the number of events produced; the first max_events are stored in events.
static uint32_t decode_all(const uint8_t*    bytes,
                           uint32_t           count,
                           Synth::MidiEvent*  events,
                           uint32_t           max_events)
{
    Synth::MidiDecoder decoder = { };
    uint32_t           num_events = 0;

    for (uint32_t idx = 0; idx < count; idx++) {
        Synth::MidiEvent event;
        if (Synth::midi_decode_byte(&decoder, bytes[idx], &event)) {
            if (num_events < max_events) {
                events[num_events] = event;
            }
            num_events++;
        }
    }

    return num_events;
}

int main()
{
    Synth::MidiEvent events[8];

    // Note on: channel 0, note 64, velocity 100
    {
        const uint8_t bytes[] = { 0x90, 0x40, 0x64 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::note_on);
        TEST(events[0].channel   == 0);
        TEST(events[0].note      == 64);
        TEST(events[0].note_data == 100);
    }

    // Note off: channel 0, note 64
    {
        const uint8_t bytes[] = { 0x80, 0x40, 0x40 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::note_off);
        TEST(events[0].note      == 64);
        TEST(events[0].note_data == 64);
    }

    // Note on with velocity 0 decodes as note off
    {
        const uint8_t bytes[] = { 0x90, 0x3C, 0x00 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::note_off);
        TEST(events[0].note      == 60);
        TEST(events[0].note_data == 0);
    }

    // Channel is the status low nibble
    {
        const uint8_t bytes[] = { 0x95, 0x40, 0x64 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event   == Synth::EvType::note_on);
        TEST(events[0].channel == 5);
    }

    // Running status: a second note on with no repeated status byte
    {
        const uint8_t bytes[] = { 0x90, 0x40, 0x64, 0x42, 0x65 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 2);
        TEST(events[0].event     == Synth::EvType::note_on);
        TEST(events[0].note      == 64);
        TEST(events[0].note_data == 100);
        TEST(events[1].event     == Synth::EvType::note_on);
        TEST(events[1].note      == 66);
        TEST(events[1].note_data == 101);
    }

    // Control change: controller 1 (mod wheel), value 64
    {
        const uint8_t bytes[] = { 0xB0, 0x01, 0x40 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event           == Synth::EvType::controller);
        TEST(events[0].controller      == 1);
        TEST(events[0].controller_data == 64);
    }

    // Polyphonic aftertouch: note 64, pressure 80
    {
        const uint8_t bytes[] = { 0xA0, 0x40, 0x50 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::aftertouch);
        TEST(events[0].note      == 64);
        TEST(events[0].note_data == 80);
    }

    // Channel pressure: one data byte, stored in note_data (matches the consumer)
    {
        const uint8_t bytes[] = { 0xD3, 0x55 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::channel_pressure);
        TEST(events[0].channel   == 3);
        TEST(events[0].note_data == 0x55);
    }

    // Program change: one data byte, stored in note_data
    {
        const uint8_t bytes[] = { 0xC2, 0x07 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::program_change);
        TEST(events[0].channel   == 2);
        TEST(events[0].note_data == 7);
    }

    // Pitch bend: 14-bit value, centered (subtract 8192) into int16
    {
        const uint8_t center[] = { 0xE0, 0x00, 0x40 }; // 8192 -> 0
        TEST(decode_all(center, sizeof(center), events, 8) == 1);
        TEST(events[0].event      == Synth::EvType::pitch_bend);
        TEST(events[0].pitch_bend == 0);

        const uint8_t lowest[] = { 0xE0, 0x00, 0x00 }; // 0 -> -8192
        TEST(decode_all(lowest, sizeof(lowest), events, 8) == 1);
        TEST(events[0].pitch_bend == -8192);

        const uint8_t highest[] = { 0xE0, 0x7F, 0x7F }; // 16383 -> 8191
        TEST(decode_all(highest, sizeof(highest), events, 8) == 1);
        TEST(events[0].pitch_bend == 8191);
    }

    // SysEx is swallowed; a following note on still decodes
    {
        const uint8_t bytes[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7, 0x90, 0x40, 0x64 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event == Synth::EvType::note_on);
        TEST(events[0].note  == 64);
    }

    // System Real-Time byte mid-message is ignored without breaking assembly
    {
        const uint8_t bytes[] = { 0x90, 0x40, 0xF8, 0x64 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event     == Synth::EvType::note_on);
        TEST(events[0].note      == 64);
        TEST(events[0].note_data == 100);
    }

    // System Real-Time byte between messages does not cancel running status
    {
        const uint8_t bytes[] = { 0x90, 0x40, 0x64, 0xF8, 0x42, 0x65 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 2);
        TEST(events[1].event == Synth::EvType::note_on);
        TEST(events[1].note  == 66);
    }

    // System Common cancels running status: trailing data bytes are orphaned
    {
        const uint8_t bytes[] = { 0x90, 0x40, 0x64, 0xF2, 0x00, 0x10, 0x42, 0x65 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 1);
        TEST(events[0].event == Synth::EvType::note_on);
    }

    // Data bytes with no preceding status produce nothing
    {
        const uint8_t bytes[] = { 0x40, 0x64 };
        TEST(decode_all(bytes, sizeof(bytes), events, 8) == 0);
    }

    return exit_code;
}
