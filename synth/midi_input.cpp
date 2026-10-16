// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "midi_input.h"

#include <atomic>

namespace {

// MIDI event circular buffer: single-consumer (audio producer thread).  The two
// producers (OS MIDI thread, synth editor GUI thread) are serialized by
// submit_lock, keeping the ring effectively single-producer.  Sized so real MIDI
// rates never overflow between render steps.
constexpr uint32_t midi_buffer_capacity = 1024;

Synth::MidiEvent      midi_event_buffer[midi_buffer_capacity];
std::atomic<uint64_t> midi_write;
std::atomic<uint64_t> midi_read;
std::atomic_flag      submit_lock = ATOMIC_FLAG_INIT;

} // anonymous namespace

// Display-only held-note bits per channel (bit n%32 of word n/32).  Producers
// update them with relaxed atomics; the synth editor keyboard reads them.
static std::atomic<uint32_t> external_held_notes[Synth::max_channels][4];

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

bool Synth::submit_external_midi_event(const Synth::MidiEvent& event)
{
    // Spinlock serializes the two producers; the critical section is a few
    // stores, so spinning is negligible.
    while (submit_lock.test_and_set(std::memory_order_acquire)) {
        // Spin until the other producer releases the lock.
    }

    const uint64_t write_pos = midi_write.load(std::memory_order_relaxed);
    const uint64_t read_pos  = midi_read.load(std::memory_order_acquire);

    // Drop on overflow; only reachable under pathological flooding.
    const bool queued = get_ringbuf_avail_space(write_pos, read_pos, midi_buffer_capacity);
    if (queued) {
        midi_event_buffer[write_pos % midi_buffer_capacity] = event;
        midi_write.store(write_pos + 1, std::memory_order_release);
    }

    // Check the event type first because note shares a union with other fields.
    // The channel and note guards match the drop rules of apply_midi_event.
    const bool is_note = event.event == Synth::EvType::note_on || event.event == Synth::EvType::note_off;
    if (queued && is_note && event.channel < Synth::max_channels && event.note < 128) {
        std::atomic<uint32_t>& word = external_held_notes[event.channel][event.note >> 5];
        const uint32_t         bit  = 1u << (event.note & 31u);

        if (event.event == Synth::EvType::note_on && event.note_data) {
            word.fetch_or(bit, std::memory_order_relaxed);
        }
        else {
            // note_off, or note_on with velocity 0
            word.fetch_and(~bit, std::memory_order_relaxed);
        }
    }

    submit_lock.clear(std::memory_order_release);

    return queued;
}

void Synth::get_external_held_notes(uint32_t channel, uint32_t* held_mask)
{
    for (uint32_t i = 0; i < 4; i++) {
        held_mask[i] = channel < max_channels ? external_held_notes[channel][i].load(std::memory_order_relaxed) : 0;
    }
}
