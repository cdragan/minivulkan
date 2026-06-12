// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "midi_input.h"

#include <atomic>

namespace {

// MIDI event circular buffer: single-producer (OS MIDI thread), single-consumer (audio
// producer thread).  Sized so real MIDI rates never overflow between render steps.
constexpr uint32_t midi_buffer_capacity = 1024;

Synth::MidiEvent      midi_event_buffer[midi_buffer_capacity];
std::atomic<uint64_t> midi_write;
std::atomic<uint64_t> midi_read;

} // anonymous namespace

// Drains queued events on the audio producer thread; the engine calls it once per render step.
void Synth::pump_live_midi()
{
    const uint64_t read_pos  = midi_read.load(std::memory_order_relaxed);
    const uint64_t write_pos = midi_write.load(std::memory_order_acquire);
    const uint32_t available = get_ringbuf_data_size(write_pos, read_pos);

    for (uint32_t idx = 0; idx < available; idx++) {
        apply_midi_event(midi_event_buffer[(read_pos + idx) % midi_buffer_capacity]);
    }

    midi_read.store(read_pos + available, std::memory_order_release);
}

void Synth::submit_external_midi_event(const Synth::MidiEvent& event)
{
    const uint64_t write_pos = midi_write.load(std::memory_order_relaxed);
    const uint64_t read_pos  = midi_read.load(std::memory_order_acquire);

    // Drop on overflow; only reachable under pathological flooding.
    if ( ! get_ringbuf_avail_space(write_pos, read_pos, midi_buffer_capacity)) {
        return;
    }

    midi_event_buffer[write_pos % midi_buffer_capacity] = event;
    midi_write.store(write_pos + 1, std::memory_order_release);
}
