// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

namespace Synth {

enum WaveType : uint32_t {
    // Wave disabled
    no_wave,
    // Normal sine wave, duty is ignored
    sine_wave,
    // Sawtooth wave
    //
    // |\  duty=0    /\ duty=0.5           duty=1  /|
    // | \          /  \ (triangle wave)          / |
    // |  \             \  /                     /  |
    // |   \             \/                     /   |
    sawtooth_wave,
    // Pulse wave
    //
    // | duty=0   +--+ duty=0.5        duty=1 |
    // |          |  | (square wave)          |
    // |             |  |                     |
    // +---          +--+              -------+
    pulse_wave,
    // White noise
    noise_wave
};

struct LFODescriptor {
    uint8_t  wave;              // LFO wave type
    uint8_t  duty;              // Duty for sawtooth wave (0=left, 0x7F=triangle, 0xFF=right)
    uint16_t period_ms;         // Period of the LFO, in milliseconds
    float    min_value;         // Minimum value produced by the LFO
    float    min_max_delta;     // Delta between minimum and maximum value produced
};

// Encoded envelope:
// u8 N     - number of points
// f32      - min value
// f32      - max value
// u8       - point index of sustain loop begin, >=N means no sustain
// u8       - point index of sustain end loop (can be same as sustain loop begin)
// u8[N]    - low byte of next point's delta position, each position is expressed in ticks (256 samples)
// u8[N]    - low byte of next point's delta value
// u8[N]    - high byte of next point's delta position (unsigned)
// u8[N]    - high byte of next point's delta value (signed, but uses zig-zag encoding)
//
// Delta values are encoded using zig-zag encoding (0->0, -1->1, 1->2, -2->3, 2->4, etc.).
//
// Volume envelope:
// - Value is in decibels
// - Value 0 means nominal attenuation
// - Point at position 0 and last point should indicate lowest possible negative value (no sound)
struct EnvelopeDescriptor {
    uint8_t num_points;         // Number of points in the envelope
    uint8_t unused_alignment;
    uint8_t sustain_first_point;// Index if sustain loop start point
    uint8_t sustain_last_point; // Index of last point of sustain loop (can be same as first point)
    float   min_value;          // Minimum value produced by the envelope
    float   min_max_delta;      // Delta between minimum and maximum value produced

    struct Point {
        uint16_t position;      // Number of ticks since the beginning of the envelope
        uint16_t value;         // Value at this position (0=min_value, 0xFFFF=min_value+min_max_delta)
    };

    Point points[1];
};

// Mutable position of a voice within an envelope.  The caller persists this
// between steps; eval_envelope advances it by one tick per call.
struct EnvelopeState {
    uint16_t tick;
    uint16_t point;
};

// Evaluates the envelope at its current state and returns the value
// (min_value + interpolated point value scaled by min_max_delta), then advances
// the state by one tick.  When sustain is true the state holds/loops at the
// sustain points; when false it runs through to the final point.
float eval_envelope(const EnvelopeDescriptor& envelope, EnvelopeState* state, bool sustain);

// Converts a MIDI note (69 = A4 = 440 Hz when freq_mult is 1) plus a continuous
// semitone pitch offset into a frequency in Hz.  freq_mult scales the base frequency.
float note_to_frequency(int midi_note, float pitch_semitones, uint32_t freq_mult);

// Converts a MIDI pitch-bend value (centered: 0 = no bend, range
// [-8192, 8191]) into a pitch offset in semitones, scaled so full
// deflection reaches +/- range_semitones.
float pitch_bend_to_semitones(int16_t centered_bend, float range_semitones);

// Evaluates an LFO's contribution (its min_value offset plus the wave shape) at
// the given tick.  step_samples is the LFO update granularity; sampling_rate is
// the audio sampling rate.  The caller advances the tick between steps.
float eval_lfo(const LFODescriptor& lfo, uint32_t lfo_tick, uint32_t step_samples, uint32_t sampling_rate);

} // namespace Synth
