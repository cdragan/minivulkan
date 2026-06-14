// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

namespace Synth {

// Sampling frequency, i.e. frequency of the produced audio buffer.
constexpr uint32_t rt_sampling_rate = 44100;

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
    WaveType wave;              // LFO wave type
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

// Maximum oscillators a single note (unison stack) uses.
constexpr uint32_t max_unison = 7;

// Per-oscillator modulatable quantities.  An instrument binds each independently.
enum ModTarget : uint8_t {
    mod_volume,
    mod_pitch,
    mod_panning,
    mod_duty0,
    mod_duty1,
    mod_osc_mix,
    mod_fm_index,
    mod_lowpass_cutoff,
    mod_highpass_cutoff,
    num_mod_targets
};

// Where a bound parameter is allocated.
enum class ParamScope : uint8_t {
    voice,        // One shared parameter for all the voice's oscillators
    oscillator    // One parameter per oscillator (per unison index)
};

enum class EffectType : uint8_t {
    none,
    distortion,
    delay,
    chorus,
    reverb,
    compressor,
    fir,
    num_types
};

constexpr uint32_t num_effect_types = static_cast<uint32_t>(EffectType::num_types);

// FIR filter tap count, shared by the per-oscillator FIR and the FIR effect.
constexpr uint32_t num_fir_taps = 1025;

constexpr uint32_t effect_delay_max_samples  = rt_sampling_rate;       // 1 s
constexpr uint32_t effect_chorus_max_samples = rt_sampling_rate / 20;  // 50 ms
                                                                       //
// Freeverb's comb and allpass delay-line lengths are a published tuning specified
// at freeverb_base_rate.  They are scaled to rt_sampling_rate with the same integer
// division the reverb shader applies, so the reverb keeps its voicing at any rate and
// the host state size matches the shader's rings exactly.
constexpr uint32_t freeverb_base_rate        = 44100;
constexpr uint32_t effect_reverb_num_combs   = 8;
constexpr uint32_t effect_reverb_num_allpass = 4;
constexpr uint32_t effect_reverb_comb_base[effect_reverb_num_combs] =
    { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr uint32_t effect_reverb_allpass_base[effect_reverb_num_allpass] =
    { 556, 441, 341, 225 };

// A Freeverb delay-line length scaled from freeverb_base_rate to rt_sampling_rate.
constexpr uint32_t freeverb_scaled_length(uint32_t base_length)
{
    return base_length * rt_sampling_rate / freeverb_base_rate;
}

// Number of scalar float params an effect type uses.
uint32_t effect_param_floats(EffectType type);

// Number of persistent float slots an effect type keeps in the device state region.
uint32_t effect_state_floats(EffectType type);

// Frames available to read from a ring buffer.
uint32_t get_ringbuf_data_size(uint64_t write_pos, uint64_t read_pos);

// Frames of free space to write into a ring buffer of the given capacity.
uint32_t get_ringbuf_avail_space(uint64_t write_pos, uint64_t read_pos, uint32_t capacity);

// Contiguous frames from pos to the end of the buffer before it wraps (read or write pos).
uint32_t get_ringbuf_contig_tail(uint64_t pos, uint32_t capacity);

// How a source parameter folds into another parameter
enum class SourceOp : uint8_t {
    add,        // neutral contribution 0
    multiply    // neutral contribution 1
};

// Maximum source parameters feeding one parameter
constexpr uint32_t max_param_sources = 4;

// One input of a parameter: reads the PREVIOUS-step value of a source parameter
// (one-step-delayed), scales it by multiplier, and folds it in per op.  param_id 0
// references the reserved sentinel parameter (value 0).
struct SourceParam {
    uint16_t param_id;
    SourceOp op;
    float    multiplier;
};

// What computes a parameter's value, and therefore which per-kind fields are live.  A node is exactly
// one kind, so the fields are a union: illegal mixes (base+sources together with a generator) cannot
// be expressed.  external is the zero-initialized default (value 0), which also covers the sentinel.
enum class ParamKind : uint8_t {
    external,   // value written from outside (a MIDI input, or the reserved sentinel); never computed
    envelope,   // value produced each step by the envelope generator (the per-step pre-pass)
    lfo,        // value produced each step by the LFO generator (the per-step pre-pass)
    plain       // value computed by propagate_parameters: base_value folded with the source params
};

// Description of how one parameter is computed.  Holds no per-sample runtime state (so the playback
// program can compress/pack these); the engine instantiates it per note, expanding an InstrModBinding
// onto the note's allocated slots.  kind selects the live union member.  A generator or external value
// is set before the fold, so a source reading it sees it with zero lag; a plain->plain hop is delayed
// one step.
struct ParamDescriptor {
    // envelope
    struct EnvelopeParam {
        uint16_t desc_id;            // 1-based into the envelope table
    };

    // lfo
    struct LfoParam {
        uint16_t desc_id;            // 1-based into the LFO table
        SourceOp op;                 // how the LFO contribution is shaped (see eval_lfo_mod)
        float    depth;
        uint16_t depth_param_id;     // 0 = use the constant depth; else scales it by a parameter
        uint16_t rate_param_id;      // 0 = use the LFO's own period; else a parameter offsets it
        float    rate_scale;         // ms of period offset per unit of the rate parameter
    };

    // plain
    struct PlainParam {
        float       base_value;
        uint16_t    num_sources;
        SourceParam sources[max_param_sources];
    };

    ParamKind kind;
    union {
        EnvelopeParam envelope;
        LfoParam      lfo;
        PlainParam    plain;
    };
};

// Input source ROLES an instrument binding can reference.  note-on resolves each to a concrete
// parameter id via the engine's per-channel and per-voice leaves.
enum class ModSource : uint8_t {
    none,
    pitch_bend,        // channel pitch-bend leaf
    mod_wheel,         // channel modulation-wheel leaf
    channel_pressure,  // channel pressure leaf
    velocity,          // per-note velocity leaf
    aftertouch,        // per-note polyphonic aftertouch leaf
    pressure_combine   // per-voice max(aftertouch, channel pressure) leaf
};

// Maximum input parameters a single target binding declares (beyond its envelope and LFO).
constexpr uint32_t max_mod_inputs = 2;

// One modulation input of a target: folds a resolved source parameter in per op, scaled.
struct ModInput {
    ModSource source;
    SourceOp  op;
    float     multiplier;
};

// An instrument's complete modulation declaration for one target: a base value, an optional
// envelope generator (per-unison descriptor), an optional LFO generator (sourceable depth and
// rate), and a list of input sources.  note-on expands this into graph nodes generically, and the
// editor edits these declarations directly.  No runtime state lives here, so it can be packed.
struct InstrModBinding {
    ParamScope scope;                         // envelope-descriptor selector: voice -> [0], oscillator -> [unison]
    float      base_value;
    uint16_t   envelope_desc_id[max_unison];  // 1-based into the envelope table; 0 = no envelope
    uint16_t   lfo_desc_id;                   // 1-based into the LFO table; 0 = no LFO
    SourceOp   lfo_op;                        // how the LFO folds into the target (see eval_lfo_mod)
    ModSource  lfo_depth_source;              // none -> use lfo_depth constant; else scales it
    float      lfo_depth;
    ModSource  lfo_rate_source;               // none -> use the LFO's own period; else offsets it
    float      lfo_rate_scale;                // ms of period offset per unit of the rate source
    uint16_t   num_inputs;
    ModInput   inputs[max_mod_inputs];
};

// Runtime value of one parameter, plus the generator state carried between render steps when the
// parameter is an envelope or LFO leaf (the generator fields are unused for other parameters).
struct Parameter {
    float         value;
    float         prev_value;
    EnvelopeState envelope;       // Envelope tick and point
    uint16_t      lfo_tick;       // LFO tick
    uint16_t      sustain_voice;  // Voice gating the envelope sustain (0 = always sustain)
};

// Advances the parameters' base + source propagation one control-rate step:
//   1. snapshot prev_value = value for every parameter (so reads are consistent),
//   2. recompute each plain parameter: value = base, then fold each source's
//      amount * source.prev_value in by add or multiply per its op.
// Non-plain kinds (external, envelope, lfo) are left untouched -- they are set before this runs.
// Because sources read prev_value, such a source is seen with zero lag while a plain->plain hop is
// delayed exactly one step; cycles are well-defined and bounded.
void propagate_parameters(Parameter* params, const ParamDescriptor* descs, uint32_t count);

// Configures a ParamDescriptor as an LFO generator (kind == lfo): propagate_parameters leaves it
// alone; the per-step pre-pass fills its value by evaluating the LFO.  Depth and rate can be driven
// by parameters -- the pre-pass reads the depth/rate source parameters (one step delayed) when
// computing the wave -- but those are generator inputs, not propagate sources.
void configure_lfo(ParamDescriptor* leaf,
                   uint16_t         lfo_desc_id,
                   SourceOp         lfo_op,
                   float            lfo_depth,
                   uint16_t         lfo_depth_param_id,
                   uint16_t         lfo_rate_param_id,
                   float            lfo_rate_scale);

// Assembles a modulated parameter's dest descriptor: base_value folded with an optional envelope
// source (env_param_id != 0, additive), an optional LFO source (lfo_param_id != 0, folded with
// lfo_op), then each pre-resolved input source in order.  The caller configures the leaves
// themselves (the envelope leaf, and the LFO leaf via configure_lfo); this only builds the dest's
// source list.  Each input is a SourceParam whose param_id is already a concrete graph node.
void configure_plain(ParamDescriptor*   dest,
                     float              base_value,
                     uint16_t           env_param_id,
                     uint16_t           lfo_param_id,
                     SourceOp           lfo_op,
                     const SourceParam* inputs,
                     uint32_t           num_inputs);

// LFO contribution with sourceable rate and depth, shaped by op:
//   add      -> depth * bipolar_wave        in [-depth, depth], neutral 0
//   multiply -> 1 - depth * (1 - unipolar_wave)  in [1-depth, 1], neutral 1
// period_ms sets the rate (0 = use the descriptor's period_ms); only the wave shape
// and duty are taken from lfo (its min_value / min_max_delta are ignored here).
float eval_lfo_mod(const LFODescriptor& lfo,
                   uint32_t             lfo_tick,
                   uint32_t             step_samples,
                   uint32_t             sampling_rate,
                   uint32_t             period_ms,
                   float                depth,
                   SourceOp             op);

} // namespace Synth
