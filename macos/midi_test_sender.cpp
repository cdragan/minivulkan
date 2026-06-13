// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Standalone CoreMIDI test sender for exercising the synth's live MIDI input.  Creates a
// virtual MIDI source and emits a short sequence (notes across channels, two non-contiguous
// channels held together, a held note swept with pitch bend and the modulation wheel, then a
// held note with a channel-pressure swell that drives tremolo depth up and back down).
// The synth connects to the new source automatically.
//
// Built as Out/<config>/midi_test_sender.  Run it while sculptor is running to drive the synth.

#include <CoreMIDI/CoreMIDI.h>
#include <cstdint>
#include <unistd.h>

static MIDIClientRef   client;
static MIDIEndpointRef source;

static void send_bytes(const uint8_t* bytes, int length)
{
    Byte            storage[256];
    MIDIPacketList* packet_list = reinterpret_cast<MIDIPacketList*>(storage);
    MIDIPacket*     packet      = MIDIPacketListInit(packet_list);

    packet = MIDIPacketListAdd(packet_list, sizeof(storage), packet, 0,
                               static_cast<ByteCount>(length), bytes);
    MIDIReceived(source, packet_list);
}

static void note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    const uint8_t bytes[] = { static_cast<uint8_t>(0x90 | channel), note, velocity };
    send_bytes(bytes, 3);
}

static void note_off(uint8_t channel, uint8_t note)
{
    const uint8_t bytes[] = { static_cast<uint8_t>(0x80 | channel), note, 0 };
    send_bytes(bytes, 3);
}

static void channel_pressure(uint8_t channel, uint8_t value)
{
    const uint8_t bytes[] = { static_cast<uint8_t>(0xD0 | channel), value };
    send_bytes(bytes, 2);
}

// Pitch bend, mod wheel and channel pressure are persistent channel state; reset them so each
// run starts neutral and leaves the channel neutral.
static void reset_controllers()
{
    const uint8_t pitch_center[] = { 0xE0, 0x00, 0x40 }; // 8192 = no bend
    send_bytes(pitch_center, 3);
    const uint8_t mod_zero[]     = { 0xB0, 0x01, 0x00 };
    send_bytes(mod_zero, 3);
    channel_pressure(0, 0);
}

int main()
{
    MIDIClientCreate(CFSTR("midi test sender"), nullptr, nullptr, &client);
    MIDISourceCreate(client, CFSTR("test source"), &source);

    // Let the synth notice the new source and connect to it.
    sleep(1);

    // Orphan note-off for a note never pressed: a live source can send this (panic,
    // voice stealing); the synth must ignore it, not crash.
    note_off(0, 48);

    // Start from a neutral channel so the melody is not skewed by a prior run's bend.
    reset_controllers();

    const uint8_t melody[] = { 60, 64, 67, 72 };
    for (int idx = 0; idx < 4; idx++) {
        note_on(0, melody[idx], 100);
        usleep(400000);
        note_off(0, melody[idx]);
        usleep(100000);
    }

    // Exercise every MIDI channel: each routes through instr_routing[channel].
    for (uint8_t channel = 0; channel < 16; channel++) {
        note_on(channel, 60, 100);
        usleep(60000);
        note_off(channel, 60);
    }

    // Hold two non-contiguous channels at once (0 and 5): exercises the mix's per-channel
    // input routing when the active channel set is not contiguous from zero.
    note_on(0, 60, 100);
    note_on(5, 67, 100);
    usleep(500000);
    note_off(0, 60);
    note_off(5, 67);

    // Hold a note while sweeping pitch bend up, then the modulation wheel.
    note_on(0, 60, 100);
    for (int bend = 0; bend <= 16383; bend += 512) {
        const uint8_t bytes[] = { 0xE0,
                                  static_cast<uint8_t>(bend & 0x7F),
                                  static_cast<uint8_t>((bend >> 7) & 0x7F) };
        send_bytes(bytes, 3);
        usleep(20000);
    }
    for (int value = 0; value <= 127; value += 8) {
        const uint8_t bytes[] = { 0xB0, 0x01, static_cast<uint8_t>(value) };
        send_bytes(bytes, 3);
        usleep(20000);
    }
    note_off(0, 60);

    // Hold a note while swelling channel pressure 0 -> full -> 0: drives tremolo (volume LFO)
    // depth up and back down, so the held note pulses progressively deeper, then settles.
    note_on(0, 60, 100);
    for (int value = 0; value <= 127; value += 4) {
        channel_pressure(0, static_cast<uint8_t>(value));
        usleep(30000);
    }
    usleep(1000000);   // hold full pressure: steady deep tremolo
    for (int value = 127; value >= 0; value -= 4) {
        channel_pressure(0, static_cast<uint8_t>(value));
        usleep(30000);
    }
    note_off(0, 60);

    // Leave the channel neutral for the next run / any other source.
    reset_controllers();

    return 0;
}
