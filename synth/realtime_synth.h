// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_modulation.h"

#include <stdint.h>

namespace Synth {

// Maximum number of supported channels
static constexpr uint32_t max_channels = 16;

// Maximum variants per instrument (different instruments assigned to different notes)
static constexpr uint32_t max_instr_per_channel = 16;

// MIDI data
extern const uint32_t num_channels;         // Total number of used channels
extern const uint8_t* midi_delta_times[];   // Encoded event delta times, per-channel
extern const uint8_t* midi_events[];        // Encoded events, per-channel
extern const uint8_t* midi_notes[];         // Per-channel notes for note events
extern const uint8_t* midi_note_data[];     // Per-channel note data for note events
extern const uint8_t* midi_ctrl[];          // Per-channel controllers for controller events
extern const uint8_t* midi_ctrl_data[];     // Per-channel controller data for controller events
extern const uint8_t* midi_pitch_bend_lo[]; // Per-channel pitch bend LSB values
extern const uint8_t* midi_pitch_bend_hi[]; // Per-channel pitch bend MSB values

// Per-channel routing of notes to instruments
struct InstrumentRouting {
    struct {
        uint8_t start_note;
        uint8_t instrument;
    } note_routing[max_instr_per_channel];
};
extern const InstrumentRouting instr_routing[max_channels];

#define MIDI_EVENT_TYPES(X) \
    X(note_off)             \
    X(note_on)              \
    X(aftertouch)           \
    X(controller)           \
    X(program_change)       \
    X(channel_pressure)     \
    X(pitch_bend)           \

enum class EvType : uint8_t {
    #define X(name) name,
    MIDI_EVENT_TYPES(X)
    #undef X
    num_event_types
};

struct MidiEvent {
    uint32_t time; // Event time, in samples, since the beginning of playback
    EvType   event;
    uint8_t  channel;
    union {
        struct { // Used by note_off, note_on and aftertouch events
            uint8_t note;
            uint8_t note_data;
        };
        struct { // Used by controller events
            uint8_t controller;
            uint8_t controller_data;
        };
        int16_t pitch_bend; // Used by pitch_bend events
    };
};

bool init_synth();

void stop_synth();

void apply_midi_event(const MidiEvent& event);

// Drains live MIDI input into the synth
void pump_live_midi();

// Fills the caller's output channels from the audio ring buffer
template<typename T, bool interleaved>
uint32_t consume_audio(uint32_t num_frames, T* channel0, T* channel1);

// Tops up the audio ring buffer
template<typename T, bool interleaved>
bool produce_audio_batch();

// Audio ring buffer health, for the GUI buffer indicator.
struct AudioRingStatus {
    uint32_t fill_frames;       // frames currently buffered ahead of the callback
    uint32_t lead_frames;       // target lead the producer keeps buffered
    uint32_t underrun_count;    // times the ring ran dry and the callback got silence
};

AudioRingStatus get_audio_ring_status();

uint64_t get_current_timestamp_ms();

} // namespace Synth
