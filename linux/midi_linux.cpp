// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"
#include "../synth/midi_decode.h"
#include "../synth/midi_input.h"
#include <alsa/asoundlib.h>
#include <atomic>
#include <errno.h>
#include <pthread.h>
#include <time.h>

static uint32_t midi_alsa_convert(const snd_seq_event_t& event, uint8_t bytes[3])
{
    if (event.type == SND_SEQ_EVENT_NOTEON || event.type == SND_SEQ_EVENT_NOTEOFF ||
        event.type == SND_SEQ_EVENT_KEYPRESS) {
        if (event.data.note.channel > 15 || event.data.note.note > 127 || event.data.note.velocity > 127) {
            return 0;
        }

        const uint8_t status = event.type == SND_SEQ_EVENT_NOTEON    ? 0x90
                               : event.type == SND_SEQ_EVENT_NOTEOFF ? 0x80
                                                                     : 0xA0;
        bytes[0]             = static_cast<uint8_t>(status | event.data.note.channel);
        bytes[1]             = event.data.note.note;
        bytes[2]             = event.data.note.velocity;
        return 3;
    }

    if (event.type != SND_SEQ_EVENT_CONTROLLER && event.type != SND_SEQ_EVENT_PGMCHANGE &&
        event.type != SND_SEQ_EVENT_CHANPRESS && event.type != SND_SEQ_EVENT_PITCHBEND) {
        // Duration notes need a scheduler.  Accept only raw channel messages.
        return 0;
    }

    if (event.data.control.channel > 15) {
        return 0;
    }

    const int value = event.data.control.value;
    if (event.type == SND_SEQ_EVENT_PITCHBEND) {
        if (value < -8192 || value > 8191) {
            return 0;
        }

        const uint32_t bend = static_cast<uint32_t>(value + 8192);
        bytes[0]            = static_cast<uint8_t>(0xE0 | event.data.control.channel);
        bytes[1]            = static_cast<uint8_t>(bend & 0x7F);
        bytes[2]            = static_cast<uint8_t>(bend >> 7);
        return 3;
    }

    if (value < 0 || value > 127) {
        return 0;
    }

    if (event.type == SND_SEQ_EVENT_CONTROLLER) {
        if (event.data.control.param > 127) {
            return 0;
        }

        bytes[0] = static_cast<uint8_t>(0xB0 | event.data.control.channel);
        bytes[1] = static_cast<uint8_t>(event.data.control.param);
        bytes[2] = static_cast<uint8_t>(value);
        return 3;
    }

    bytes[0] = static_cast<uint8_t>((event.type == SND_SEQ_EVENT_PGMCHANGE ? 0xC0 : 0xD0) | event.data.control.channel);
    bytes[1] = static_cast<uint8_t>(value);
    return 2;
}

namespace {
static snd_seq_t*        sequencer;
static int               input_port;
static pthread_t         input_thread;
static bool              input_created;
static std::atomic<bool> input_running;
} // namespace

static bool connect_all_sources()
{
    snd_seq_client_info_t* client_info;
    snd_seq_port_info_t*   port_info;
    snd_seq_client_info_alloca(&client_info);
    snd_seq_port_info_alloca(&port_info);

    const int own_client = snd_seq_client_id(sequencer);
    if (own_client < 0) {
        d_printf("Failed to get ALSA MIDI client: %s\n", snd_strerror(own_client));
        return false;
    }

    // Connect once.  New devices need an external connection or a restart.
    snd_seq_client_info_set_client(client_info, -1);
    int result;

    while ((result = snd_seq_query_next_client(sequencer, client_info)) >= 0) {
        const int client = snd_seq_client_info_get_client(client_info);
        if (client == 0 || client == own_client) {
            continue;
        }

        snd_seq_port_info_set_client(port_info, client);
        snd_seq_port_info_set_port(port_info, -1);
        int port_result;

        while ((port_result = snd_seq_query_next_port(sequencer, port_info)) >= 0) {
            const unsigned caps     = snd_seq_port_info_get_capability(port_info);
            const unsigned required = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((caps & required) != required) {
                continue;
            }

            const int port      = snd_seq_port_info_get_port(port_info);
            const int connected = snd_seq_connect_from(sequencer, input_port, client, port);
            if (connected < 0) {
                d_printf("Failed to connect ALSA MIDI source %d:%d: %s\n", client, port, snd_strerror(connected));
            }
        }

        if (port_result != -ENOENT) {
            d_printf("Failed to query ALSA MIDI ports: %s\n", snd_strerror(port_result));
            return false;
        }
    }

    if (result != -ENOENT) {
        d_printf("Failed to query ALSA MIDI clients: %s\n", snd_strerror(result));
        return false;
    }

    return true;
}

static void* midi_input_main(void*)
{
    constexpr timespec sleep_time = { 0, 1'000'000 };

    while (input_running.load(std::memory_order_relaxed)) {
        snd_seq_event_t* input  = nullptr;
        const int        result = snd_seq_event_input(sequencer, &input);
        if (result == -EAGAIN) {
            // ponytail: sleep poll at 1 ms.  Use poll if measured latency requires it.
            nanosleep(&sleep_time, nullptr);
            continue;
        }

        if (result == -EINTR) {
            continue;
        }

        if (result < 0) {
            d_printf("ALSA MIDI input failed: %s\n", snd_strerror(result));
            break;
        }

        if (! input) {
            continue;
        }

        uint8_t        bytes[3];
        const uint32_t count = midi_alsa_convert(*input, bytes);
        // Each event is complete.  Sources cannot share running status.
        Synth::MidiDecoder decoder = {};
        Synth::MidiEvent   event;

        for (uint32_t idx = 0; idx < count; idx++) {
            if (Synth::midi_decode_byte(&decoder, bytes[idx], &event)) {
                Synth::submit_device_midi_event(event);
            }
        }
    }

    return nullptr;
}

bool Synth::init_midi_os()
{
    int result = snd_seq_open(&sequencer, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (result < 0) {
        d_printf("Failed to open ALSA MIDI: %s\n", snd_strerror(result));
        stop_midi_os();
        return false;
    }

    result = snd_seq_set_client_name(sequencer, "minivulkan synth");
    if (result < 0) {
        d_printf("Failed to name ALSA MIDI client: %s\n", snd_strerror(result));
        stop_midi_os();
        return false;
    }

    input_port = snd_seq_create_simple_port(sequencer,
                                            "input",
                                            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (input_port < 0) {
        d_printf("Failed to create ALSA MIDI input port: %s\n", snd_strerror(input_port));
        stop_midi_os();
        return false;
    }

    if (! connect_all_sources()) {
        stop_midi_os();
        return false;
    }

    input_running.store(true, std::memory_order_relaxed);
    result = pthread_create(&input_thread, nullptr, midi_input_main, nullptr);
    if (result != 0) {
        d_printf("Failed to start MIDI input thread: %d\n", result);
        stop_midi_os();
        return false;
    }

    input_created = true;

    return true;
}

void Synth::stop_midi_os()
{
    input_running.store(false, std::memory_order_relaxed);
    if (input_created) {
        pthread_join(input_thread, nullptr);
        input_created = false;
    }

    if (sequencer) {
        snd_seq_close(sequencer);
        sequencer = nullptr;
    }
}
