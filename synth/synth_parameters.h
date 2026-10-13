// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"

#include <stdint.h>

class RNG;

namespace Synth {

enum class WaveType : uint8_t {
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
    WaveType wave;          // LFO wave type
    uint8_t  duty;          // Duty for sawtooth wave (0=left, 0x7F=triangle, 0xFF=right)
    uint16_t period_ms;     // Period of the LFO, in milliseconds
    float    min_value;     // Minimum value produced by the LFO
    float    min_max_delta; // Delta between minimum and maximum value produced
};

struct EnvelopeDescriptor {
    uint8_t num_points;
    uint8_t ping_pong;           // Sustain span bounces between its ends instead of looping (multi-point spans only)
    uint8_t sustain_first_point; // Index of sustain loop start point
    uint8_t sustain_last_point;  // Index of last point of sustain loop (can be same as first point)
    float   min_value;           // Minimum value produced by the envelope
    float   min_max_delta;       // Delta between minimum and maximum value produced

    struct Point {
        uint16_t position; // Number of ticks since the beginning of the envelope
        uint16_t value;    // Value at this position (0=min_value, 0xFFFF=min_value+min_max_delta)
    };

    Point points[max_envelope_points];
};

// Mutable position of a voice within an envelope.  The caller persists this
// between steps.
struct EnvelopeState {
    uint16_t tick;
    uint16_t point;
    uint8_t  reverse; // Ping-pong direction: 0 = forward, 1 = backward
};

enum class SourceOp : uint8_t {
    add,
    multiply
};

// One input of a parameter
struct SourceParam {
    uint16_t param_id;
    float    scale;
    SourceOp op;
};

enum class ParamKind : uint8_t {
    external, // value written from outside (a MIDI input, or the reserved sentinel)
    envelope, // value produced each step by the envelope generator (the per-step pre-pass)
    lfo,      // value produced each step by the LFO generator (the per-step pre-pass)
    plain     // base_value combined with source parameters
};

struct ParamDescriptor {
    struct LFOParam {
        LFODescriptor lfo;
        SourceOp      op;
        float         depth;
        uint16_t      depth_param_id;
        uint16_t      rate_param_id;
        float         rate_scale_ms;
    };

    struct PlainParam {
        float       base_value;
        uint16_t    num_sources;
        SourceParam sources[max_param_sources];
    };

    ParamKind kind;

    union {
        EnvelopeDescriptor envelope;
        LFOParam           lfo;
        PlainParam         plain;
    };

    static_assert(sizeof(EnvelopeDescriptor) <= sizeof(PlainParam));
    static_assert(sizeof(LFOParam) <= sizeof(PlainParam));
};

// Runtime value of one parameter
struct Parameter {
    float         value;
    float         prev_value;
    EnvelopeState envelope;
    uint16_t      lfo_tick;
    uint16_t      sustain_voice; // For envelope params, used to detect when to release envelope from sustain
    // Envelope params only: consecutive steps whose state entered already parked on the final
    // point while released.  The voice-lifetime rules use this to time a target's final fade tick.
    uint16_t last_point_ticks;
};

// Evaluates the envelope at its current state and returns the value
// (min_value + interpolated point value scaled by min_max_delta), then advances
// the state by one tick.
// - evenlope - the envelope to evaluate
// - state    - current state (updated by one tick)
// - sustain  - true: state holds/loops at the sustain points (a ping-pong
//              multi-point span reverses direction at both span ends instead
//              of looping back)
//              false: runs through to the final point.
float eval_envelope(const EnvelopeDescriptor& envelope, EnvelopeState* state, bool sustain);

// Evaluates the envelope at the given state's position without modifying the state.
float eval_envelope_at(const EnvelopeDescriptor& envelope, const EnvelopeState& state);

// Converts a MIDI note to frequency in Hz.
// - midi_note       - input MIDI note (69 = A4 = 440 Hz when freq_mult is 1)
// - pitch_semitones - pitch offset (in semitones) to add to the note
// - freq_mult       - scales the base frequency.
float note_to_frequency(int midi_note, float pitch_semitones, uint32_t freq_mult);

// Converts a MIDI pitch-bend value to pitch offset in semitones
// - centered_bend   - 0 = no bend, range [-8192, 8191]
// - range_semttones - output pitch offset range, in semitones, scaled so full
//                     deflection reaches +/- range_semitones.
float pitch_bend_to_semitones(int16_t centered_bend, float range_semitones);

// Draws a uniform random pitch offset in [-amount_semitones, amount_semitones], advancing rng.
// - rng              - the random number generator to draw from and advance
// - amount_semitones - random output range's magnitude; note: 0 returns 0 without advancing RNG,
//                      so a zero skew is deterministic and unchanged.
float random_pitch_skew(RNG* rng, float amount_semitones);

// Level where the sustain decay ends and the voice frees: 1/1024, about
// -60 dB (log2 == -10 exactly).
constexpr float sustain_decay_cutoff = 1.0f / 1024.0f;

// Converts an instrument release in seconds to control-step ticks
// (rt_step_samples * 1000 / rt_sampling_rate per step).  Values > 0 round to
// at least 1 tick; 0 (and negative or NaN input, which no decoded bank can
// carry) returns 0 = the legacy hold/loop behavior; an out-of-range value
// saturates at UINT32_MAX instead of trapping on the float-to-unsigned
// conversion.
uint32_t sustain_decay_ticks(float release_seconds);

// Per-step multiplier so the decaying level crosses sustain_decay_cutoff at
// exactly tick `total_ticks`: exp2(log2(cutoff) / total_ticks).
// total_ticks == 0 returns 1.0 (no decay).
float sustain_decay_per_tick(uint32_t total_ticks);

// Sustain-decay termination gate: true when the decay owns this
// oscillator's termination and the envelope-completion fade must be
// skipped (the completed dest level is still audible).  `decaying` is only
// ever true for release-set oscillators, so a release of 0 never skips.
// Exists as a pure predicate so the termination gating is unit-testable.
bool sustain_decay_owns_termination(bool decaying, float dest_value);

// Evaluates a Low Frequency Oscillator
// - lfo           - the LFO descriptor
// - lfo_tick      - current LFO position, in ticks
// - step_samples  - LFO update granularity
// - sampling_rate - audio sampling rate
float eval_lfo(const LFODescriptor& lfo, uint32_t lfo_tick, uint32_t step_samples, uint32_t sampling_rate);

// Evaluates a Low Frequency Oscillator with sourced rate and depth.
// - lfo           - the LFO descriptor
// - lfo_tick      - current LFO position, in ticks
// - step_samples  - LFO update granularity
// - sampling_rate - audio sampling rate
// - period_ms     - rate override in ms (0 = use the LFO's own period_ms)
// - depth         - modulation amount
// - op            - multiply: attenuation factor in [1-depth, 1] (neutral 1, e.g. tremolo gain);
//                   add: bipolar swing in [-depth, depth] (neutral 0, e.g. vibrato)
float eval_lfo_mod(const LFODescriptor& lfo,
                   uint32_t             lfo_tick,
                   uint32_t             step_samples,
                   uint32_t             sampling_rate,
                   uint32_t             period_ms,
                   float                depth,
                   SourceOp             op);

// Calculates frames available to read from a ring buffer.
uint32_t get_ringbuf_data_size(uint64_t write_pos, uint64_t read_pos);

// Calculates frames of free space to write into a ring buffer of the given capacity.
uint32_t get_ringbuf_avail_space(uint64_t write_pos, uint64_t read_pos, uint32_t capacity);

// Calculates fontiguous frames from pos to the end of the buffer before it wraps (read or write pos).
uint32_t get_ringbuf_contig_tail(uint64_t pos, uint32_t capacity);

// Advances the parameters
void propagate_parameters(Parameter* params, const ParamDescriptor* descs, uint32_t num_params);

// Configures a ParamDescriptor as an LFO generator; the descriptor is captured by value.
void configure_lfo(ParamDescriptor*     desc,
                   const LFODescriptor& lfo,
                   SourceOp             lfo_op,
                   float                lfo_depth,
                   uint16_t             lfo_depth_param_id,
                   uint16_t             lfo_rate_param_id,
                   float                lfo_rate_scale);

// Configures a modulated parameter, composed of multiple input parameters
void configure_plain(ParamDescriptor*   desc,
                     float              base_value,
                     uint16_t           env_param_id,
                     uint16_t           lfo_param_id,
                     SourceOp           lfo_op,
                     const SourceParam* inputs,
                     uint32_t           num_inputs);

} // namespace Synth
