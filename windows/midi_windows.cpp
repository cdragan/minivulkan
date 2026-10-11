// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"
#include "../synth/midi_decode.h"
#include "../synth/midi_input.h"
#include "realtime_synth_os.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmsystem.h>

// Live MIDI input through WinMM midiIn*.  All devices present at startup are
// opened into a bounded slot array.  There is no hotplug support.

static constexpr uint32_t max_midi_in_devices = 16;

static HMIDIIN midi_in_handles[max_midi_in_devices];

// WinMM callback thread context.  Never call midiIn* functions, blocking code,
// GUI code or COM code from here; the handler only pushes decoded events.
static void CALLBACK midi_in_proc(HMIDIIN, UINT message, DWORD_PTR, DWORD_PTR param1, DWORD_PTR)
{
    // SysEx and long messages, and open or close notifications, are ignored.
    if (message != MIM_DATA)
        return;

    // One packed short message: status in the low byte, then the data bytes,
    // low byte first.  Messages arrive complete, so each one decodes through a
    // fresh decoder with no running status carried across messages.
    Synth::MidiDecoder decoder = {};
    Synth::MidiEvent   event;
    const uint8_t      status = static_cast<uint8_t>(param1);
    const uint8_t      data_1 = static_cast<uint8_t>(param1 >> 8);
    const uint8_t      data_2 = static_cast<uint8_t>(param1 >> 16);

    if (Synth::midi_decode_byte(&decoder, status, &event))
        Synth::submit_external_midi_event(event);
    if (Synth::midi_decode_byte(&decoder, data_1, &event))
        Synth::submit_external_midi_event(event);
    // Program change and channel pressure carry one data byte; all other
    // channel messages carry two.  The decoder ignores realtime and system
    // common bytes, so trailing data bytes of such messages are harmless.
    if ((status & 0xF0) != 0xC0 && (status & 0xF0) != 0xD0) {
        if (Synth::midi_decode_byte(&decoder, data_2, &event))
            Synth::submit_external_midi_event(event);
    }
}

bool Synth::init_midi_os()
{
    const UINT num_devices = midiInGetNumDevs();
    uint32_t   num_opened  = 0;

    for (UINT device_id = 0; device_id < num_devices; device_id++) {
        if (num_opened >= max_midi_in_devices) {
            d_printf("Too many MIDI input devices; using the first %u\n", num_opened);
            break;
        }

        HMIDIIN device = nullptr;
        if (midiInOpen(&device, device_id, reinterpret_cast<DWORD_PTR>(&midi_in_proc), 0, CALLBACK_FUNCTION) !=
            MMSYSERR_NOERROR) {
            d_printf("Failed to open MIDI input device %u\n", device_id);
            continue;
        }

        if (midiInStart(device) != MMSYSERR_NOERROR) {
            d_printf("Failed to start MIDI input device %u\n", device_id);
            midiInClose(device);
            continue;
        }

        midi_in_handles[num_opened++] = device;
    }

    // No devices present is not an error.
    return true;
}

void Synth::stop_midi_os()
{
    for (uint32_t idx = 0; idx < max_midi_in_devices; idx++) {
        if (! midi_in_handles[idx])
            continue;
        midiInStop(midi_in_handles[idx]);
        midiInReset(midi_in_handles[idx]);
        midiInClose(midi_in_handles[idx]);
        midi_in_handles[idx] = nullptr;
    }
}
