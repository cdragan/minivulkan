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
    noise_wave,
    // Pad, live additive harmonic sum with slow per-harmonic wander
    pad_wave
};

struct LFODescriptor {
    WaveType wave;          // LFO wave type
    uint8_t  duty;          // Duty for sawtooth wave (0=left, 0x7F=triangle, 0xFF=right)
    uint16_t period_ms;     // Period of the LFO, in milliseconds
    float    min_value;     // Minimum value produced by the LFO
    float    min_max_delta; // Delta between minimum and maximum value produced
};

// Per-envelope behavior bits, stored in EnvelopeDescriptor::flags.
enum : uint8_t {
    envelope_ping_pong     = 1 << 0, // Sustain span bounces between its ends (multi-point spans only)
    envelope_quick_release = 1 << 1, // Note-off jumps forward to the first release-tail point at or below the current
                                     // level; the final point is the fallback
};

struct EnvelopeDescriptor {
    uint8_t num_points;
    uint8_t flags;               // Bitfield of envelope_ping_pong / envelope_quick_release
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

// Quick-release target: the first point index >= sustain_last_point
// (clamped into [0, num_points-1]) whose effective value is <= the envelope's
// current effective value at the given state (effective = min_value + raw *
// min_max_delta, the same arithmetic as eval_envelope_at).  The comparison is
// on effective values because min_max_delta is not guaranteed nonnegative (a
// negative delta reverses the effective-value order, a zero delta makes every
// effective value equal, so the scan start wins).  The final point when no
// point qualifies.  Pure: mutates nothing.
uint16_t quick_release_target(const EnvelopeDescriptor& envelope, const EnvelopeState& state);

// Applies the quick-release note-off jump in place: moves the state to
// quick_release_target when that location is strictly ahead of the current
// position ((point, tick) compared; already at the end -> no move), sets
// the tick to the target point's position, and clears the reverse flag (the
// state direction is forward after the jump).  The parked state is
// exactly what the next eval_envelope step with sustain == false expects.
// Returns true when moved.
bool apply_quick_release(const EnvelopeDescriptor& envelope, EnvelopeState* state);

// Converts a MIDI note to frequency in Hz.
// - midi_note       - input MIDI note (69 = A4 = 440 Hz when freq_mult is 1)
// - pitch_semitones - pitch offset (in semitones) to add to the note
// - freq_mult       - scales the base frequency.
float note_to_frequency(int midi_note, float pitch_semitones, uint32_t freq_mult);

// Final equivalent MIDI pitch of an oscillator: base note + 12 * octave_offset +
// per-oscillator pitch offset (including random skews) + modulated pitch target value.
// The result is an ABSOLUTE equivalent MIDI pitch (69 = A4).
float oscillator_final_pitch(int midi_note, int octave_offset, float pitch_value);

// Note-on trigger gate: an oscillator only sounds when its final equivalent
// MIDI pitch lies within [0, 127] inclusive.
bool oscillator_pitch_in_gate(float final_pitch);

// Converts an absolute equivalent MIDI pitch to frequency in Hz: 69 = A4 = 440 Hz at
// freq_mult 1.  Equivalent to note_to_frequency(69, absolute_pitch - 69.0f, freq_mult);
// the subtraction keeps fractional pitches exact (no integer rounding).
float pitch_to_frequency(float absolute_pitch, uint32_t freq_mult);

enum FilterType : uint8_t; // Defined in synth_instrument.h.

// Effective FIR cutoffs for one oscillator, by filter type.
// Band-pass: the lowpass edge sits `effective_spread` semitones above the
// absolute equivalent MIDI pitch, the highpass edge the same distance below,
// both converted to Hz at the oscillator's frequency multiplier.
// Low-pass and high-pass: `effective_spread` is a fixed cutoff in Hz.  It is
// not scaled by freq_mult and does not follow the pitch.  The unused edge is
// exactly 0 Hz, which the coefficient shader reads as "edge off".
// Returns false when the filter is off: fir_off, or a non-positive or NaN
// effective_spread.  The caller must then skip coefficient dispatch for the
// oscillator.  Generator levels stay free finite values, so negatives are
// normal.
// A band-pass highpass edge below oscillator_fir_highpass_floor_hz is
// transported as exactly 0 Hz, because the coefficient shader reaches its
// pure-lowpass form only at highpass_cutoff_freq == 0 while cutoff_hz_to_int
// maps any positive sub-1-Hz value to the 1 Hz floor (a band-pass edge, not a
// disabled one).
bool oscillator_fir_cutoffs(float      effective_pitch,
                            float      effective_spread,
                            uint32_t   freq_mult,
                            FilterType type,
                            float*     lowpass_hz,
                            float*     highpass_hz);

// Highpass edges under this Hz are heard as no highpass at all; at or above
// it the edge stays a (quantized) band-pass edge.
constexpr float oscillator_fir_highpass_floor_hz = 1.0f;

// The coefficient shader's band-pass branch requires highpass < lowpass;
// equal positive edges cancel to a normalized impulse (passthrough). A
// positive spread must therefore keep at least 1 Hz of bandwidth after
// integer-Hz quantization: when the quantized pair has none, this pushes the
// lowpass edge up 1 Hz, or pulls the highpass edge down 1 Hz when the
// lowpass already sits at the quantizer's ceiling (both edges clamped at
// max_cutoff_hz).  A zero highpass (pure lowpass) is left untouched.
void oscillator_fir_band_guard(uint32_t* lowpass_cutoff_freq, uint32_t* highpass_cutoff_freq);

// Effective FIR window span in taps for a FIR Slope percent.  The percent
// clamps to [1, 100] (NaN reads as the minimum).  The span grows
// exponentially: 11 * (num_fir_taps / 11) ^ ((slope - 1) / 99), rounded to an
// odd tap count.  Slope 1 gives exactly 11 taps (the sane minimum; a
// degenerate one-tap span is unreachable) and slope 100 gives exactly the
// full num_fir_taps span (the window itself is byte-identical to the fixed
// window; band-pass coefficients are instead normalized to unity gain at the
// band center).  Monotonically non-decreasing in the slope.
uint32_t fir_effective_span(float slope_percent);

// What to do with the cached FIR coefficient param block this step.
enum FirCoeffCacheAction {
    fir_coeff_skip,       // The previously dispatched coefficients still match: no dispatch.
    fir_coeff_dispatch,   // Build and dispatch a fresh coefficient computation.
    fir_coeff_invalidate, // No active filters: drop the snapshot, dispatch nothing.
};

// Words per FIRCoeff entry: taps_offs, highpass, lowpass, window_span (four uint32_t members,
// no padding, so a byte compare over the entry covers every member).
constexpr uint32_t fir_coeff_entry_words = 4;

// Decide what to do with the FIR coefficient cache this step.  prev_entries and cur_entries point
// at fir_coeff_entry_words-word entries (the FIRCoeff param records, compared bytewise);
// prev_entry_count 0 means "no valid snapshot".  taps_cleared is set when effect-state clears or
// live RESET fills ran this step (they may have zeroed FIR coefficient taps).
FirCoeffCacheAction fir_coeff_cache_action(const void* prev_entries,
                                           uint32_t    prev_entry_count,
                                           const void* cur_entries,
                                           uint32_t    cur_entry_count,
                                           bool        taps_cleared);

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

// Evaluates one sourced LFO node: the descriptor depth scales the depth source's
// value, the rate source's value offsets the LFO's own period (ms) by rate_scale_ms,
// and the offset result clamps to at least 1 ms (0 = use the descriptor period
// unchanged; the clamp keeps an editor-supplied scale from driving the period
// negative (unsigned underflow) or to zero (a divide-by-zero in the LFO phase)).
// - gen                - the node's LFO generator descriptor
// - depth_source_value - depth source leaf value (the advancing caller's previous
//                        step value, the note-on onset value in the trigger caller)
// - rate_source_value  - rate source leaf value, same snapshot rule
// - lfo_tick           - evaluation tick (the advancing caller's current tick, 0 at note-on)
// Pure: mutates nothing, so the advancing caller keeps its own tick increment.
float eval_sourced_lfo(const ParamDescriptor::LFOParam& gen,
                       float                            depth_source_value,
                       float                            rate_source_value,
                       uint32_t                         lfo_tick);

// Calculates frames available to read from a ring buffer.
uint32_t get_ringbuf_data_size(uint64_t write_pos, uint64_t read_pos);

// Calculates frames of free space to write into a ring buffer of the given capacity.
uint32_t get_ringbuf_avail_space(uint64_t write_pos, uint64_t read_pos, uint32_t capacity);

// Calculates fontiguous frames from pos to the end of the buffer before it wraps (read or write pos).
uint32_t get_ringbuf_contig_tail(uint64_t pos, uint32_t capacity);

// Note-on trigger evaluation of a configured parameter node: plain nodes combine
// base_value with each source's onset value; envelope nodes evaluate at their onset
// state (first point, state untouched); LFO nodes evaluate at tick 0 with their
// depth/rate sources' onset values, without advancing the LFO tick.  External leaves
// read their current value (the MIDI state at the note-on event; note-on setup seeds
// value+prev identically via write_external_leaf).
// Descriptor graphs are acyclic and shallow, so recursion is bounded.
float param_onset_value(const Parameter* params, const ParamDescriptor* descs, uint16_t param_id);

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

// Configures a modulated parameter, composed of multiple input parameters.
// env_scale multiplies the envelope source's contribution; 1 is the legacy
// unscaled behavior.
void configure_plain(ParamDescriptor*   desc,
                     float              base_value,
                     uint16_t           env_param_id,
                     float              env_scale,
                     uint16_t           lfo_param_id,
                     SourceOp           lfo_op,
                     const SourceParam* inputs,
                     uint32_t           num_inputs);

} // namespace Synth
