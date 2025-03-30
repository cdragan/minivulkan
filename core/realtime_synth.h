// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

namespace Synth {
    static constexpr uint32_t rt_sampling_rate = 44100;

    extern const uint32_t num_channels;             // Total number of used channels
    extern uint32_t       channel_samples[];        // Stores current per-channel time measured in samples
    extern const uint8_t* delta_times[];            // Encoded event delta times, per-channel
    extern const uint8_t* events[];                 // Encoded events, per-channel
    extern uint8_t        events_decode_state[];    // Saved state of event decode, per-channel
    extern const uint8_t* notes[];                  // Per-channel notes for note events
    extern const uint8_t* note_data[];              // Per-channel note data for note events
    extern const uint8_t* controller[];             // Per-channel controllers for controller events
    extern const uint8_t* controller_data[];        // Per-channel controller data for controller events
    extern const uint8_t* pitch_bend_lo[];          // Per-channel pitch bend LSB values
    extern const uint8_t* pitch_bend_hi[];          // Per-channel pitch bend MSB values
    extern uint32_t       samples_per_midi_tick;    // Actual tempo converted to samples
}

bool init_real_time_synth();

bool render_audio_buffer(uint32_t num_frames,
                         float*   left_channel,
                         float*   right_channel);

uint64_t get_real_time_synth_timestamp_ms();
