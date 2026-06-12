// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../synth/midi_input.h"
#include "../synth/midi_decode.h"
#include "../core/d_printf.h"

#include <CoreMIDI/CoreMIDI.h>

namespace {

MIDIClientRef      midi_client;
MIDIPortRef        midi_input_port;
Synth::MidiDecoder midi_decoder; // touched only by the CoreMIDI read thread

void connect_all_sources()
{
    const ItemCount num_sources = MIDIGetNumberOfSources();

    for (ItemCount idx = 0; idx < num_sources; idx++) {

        const MIDIEndpointRef source = MIDIGetSource(idx);

        if (source) {
            MIDIPortConnectSource(midi_input_port, source, nullptr);
        }
    }
}

void read_callback(const MIDIPacketList* packet_list, void*, void*)
{
    const MIDIPacket* packet = &packet_list->packet[0];

    for (UInt32 packet_idx = 0; packet_idx < packet_list->numPackets; packet_idx++) {

        for (UInt16 byte_idx = 0; byte_idx < packet->length; byte_idx++) {

            Synth::MidiEvent event;
            if (Synth::midi_decode_byte(&midi_decoder, packet->data[byte_idx], &event)) {
                Synth::submit_external_midi_event(event);
            }
        }

        packet = MIDIPacketNext(packet);
    }
}

void notify_callback(const MIDINotification* message, void*)
{
    if (message->messageID == kMIDIMsgSetupChanged) {
        connect_all_sources();
    }
}

} // anonymous namespace

namespace Synth {

bool init_midi_os()
{
    if (MIDIClientCreate(CFSTR("minivulkan synth"), notify_callback, nullptr, &midi_client) != noErr) {
        d_printf("Failed to create MIDI client\n");
        return false;
    }

    if (MIDIInputPortCreate(midi_client, CFSTR("input"), read_callback, nullptr, &midi_input_port) != noErr) {
        d_printf("Failed to create MIDI input port\n");
        return false;
    }

    connect_all_sources();
    return true;
}

void stop_midi_os()
{
    if (midi_client) {
        MIDIClientDispose(midi_client);
        midi_client = 0;
    }
}

} // namespace Synth
