// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_parameters.h"
#include "../core/mstdc.h"
#include "../core/rng.h"
#include "../core/vecfloat.h"
#include "../core/vmath.h"
#include "synth_instrument.h"
#include <assert.h>

namespace Synth {

static constexpr int pitch_bend_full_scale = 8192;

float pitch_bend_to_semitones(int16_t centered_bend, float range_semitones)
{
    return static_cast<float>(centered_bend) / static_cast<float>(pitch_bend_full_scale) * range_semitones;
}

float note_to_frequency(int midi_note, float pitch_semitones, uint32_t freq_mult)
{
    const float note_pitch = static_cast<float>(midi_note - 69) + pitch_semitones;
    return static_cast<float>(freq_mult * 440) * mstd::exp2(note_pitch / 12.0f);
}

float random_pitch_skew(RNG* rng, float amount_semitones)
{
    // Amount 0 leaves the pitch untouched and the generator unadvanced (deterministic, unchanged).
    if (amount_semitones == 0.0f) {
        return 0.0f;
    }

    const float normalized = static_cast<float>(rng->get_random()) / static_cast<float>(0xFFFFFFFFu);
    return (normalized * 2.0f - 1.0f) * amount_semitones;
}

// Normalized LFO wave in [0, 1] at the given tick, using period_ms for the rate.
static float eval_lfo_normalized(const LFODescriptor& lfo,
                                 uint32_t             lfo_tick,
                                 uint32_t             step_samples,
                                 uint32_t             sampling_rate,
                                 uint32_t             period_ms)
{
    const float phase = static_cast<float>(lfo_tick) * static_cast<float>(step_samples * 1000u) /
                        static_cast<float>(period_ms * sampling_rate);

    switch (lfo.wave) {
        case WaveType::sine_wave:
            return (vmath::sincos(phase * vmath::two_pi).sin + 1.0f) * 0.5f;

        case WaveType::sawtooth_wave: {
            const float frac_phase = phase - static_cast<float>(static_cast<int>(phase));
            const float duty       = static_cast<float>(lfo.duty) / 255.0f;

            if (frac_phase <= duty) {
                return (duty > 0.0f) ? frac_phase / duty : 0.0f;
            }

            const float fall = 1.0f - duty;
            return (fall > 0.0f) ? (1.0f - frac_phase) / fall : 0.0f;
        }

        default:
            // Only sine and sawtooth LFOs are supported
            assert(lfo.wave == WaveType::sine_wave || lfo.wave == WaveType::sawtooth_wave);
            return 0.0f;
    }
}

float eval_lfo(const LFODescriptor& lfo, uint32_t lfo_tick, uint32_t step_samples, uint32_t sampling_rate)
{
    const float wave = eval_lfo_normalized(lfo, lfo_tick, step_samples, sampling_rate, lfo.period_ms);

    return lfo.min_value + wave * lfo.min_max_delta;
}

float eval_lfo_mod(const LFODescriptor& lfo,
                   uint32_t             lfo_tick,
                   uint32_t             step_samples,
                   uint32_t             sampling_rate,
                   uint32_t             period_ms,
                   float                depth,
                   SourceOp             op)
{
    const uint32_t rate_period = period_ms ? period_ms : lfo.period_ms;
    const float    wave        = eval_lfo_normalized(lfo, lfo_tick, step_samples, sampling_rate, rate_period);

    if (op == SourceOp::multiply) {
        return 1.0f - depth * (1.0f - wave);
    }

    return depth * (2.0f * wave - 1.0f);
}

// File-local envelope evaluation: the envelope output at the given state's
// position, without touching the state.
static float envelope_value_at(const EnvelopeDescriptor& envelope, const EnvelopeState& state)
{
    const uint32_t env_point = state.point;
    const uint32_t env_tick  = state.tick;

    const EnvelopeDescriptor::Point pt1 = envelope.points[env_point];

    int env_value = pt1.value;

    if (env_tick > pt1.position) {
        assert(env_point + 1 < envelope.num_points);

        const EnvelopeDescriptor::Point pt2 = envelope.points[env_point + 1];

        const int delta_pos = static_cast<int>(env_tick - pt1.position);
        const int duration  = static_cast<int>(pt2.position - pt1.position);
        const int range     = static_cast<int>(pt2.value) - env_value;

        if (duration > 0) {
            env_value += (delta_pos * range) / duration;
        }
    }

    return envelope.min_value + static_cast<float>(env_value) * envelope.min_max_delta;
}

float eval_envelope_at(const EnvelopeDescriptor& envelope, const EnvelopeState& state)
{
    return envelope_value_at(envelope, state);
}

float eval_envelope(const EnvelopeDescriptor& envelope, EnvelopeState* state, bool sustain)
{
    uint32_t env_point = state->point;
    uint32_t env_tick  = state->tick;

    const float value = envelope_value_at(envelope, *state);

    // Ping-pong sustain span: the direction reverses at both span ends
    // instead of looping back to the span start.  Single-point spans (and
    // ping_pong off) keep the legacy loop below.
    if (sustain && envelope.ping_pong && envelope.sustain_last_point > envelope.sustain_first_point) {
        if (! state->reverse) {
            if (env_point < envelope.sustain_last_point) {
                // Ordinary forward advance through the span, as without ping-pong.
                const uint32_t next_point = env_point + 1;
                ++env_tick;
                if (env_tick == envelope.points[next_point].position) {
                    env_point = next_point;
                }
            }
            else {
                // Parked at the span end: reverse and step back immediately.
                state->reverse = 1;
                --env_point;
                --env_tick;
                if (env_tick < envelope.points[env_point].position) {
                    env_tick = envelope.points[env_point].position; // zero-gap points clamp in place
                }
            }
        }
        else if (env_tick > envelope.points[env_point].position) {
            --env_tick; // stepping backward through the current segment
        }
        else if (env_point > envelope.sustain_first_point) {
            // Parked at a segment start: continue into the previous segment's tail.
            --env_point;
            --env_tick;
            if (env_tick < envelope.points[env_point].position) {
                env_tick = envelope.points[env_point].position; // zero-gap points clamp in place
            }
        }
        else {
            // Parked at the span start: bounce back to the forward direction.
            state->reverse            = 0;
            const uint32_t next_point = env_point + 1;
            ++env_tick;
            if (env_tick == envelope.points[next_point].position) {
                env_point = next_point;
            }
        }
    }
    else {
        for (;;) {
            // Advance envelope
            if (! sustain || env_point < envelope.sustain_last_point) {

                const uint32_t next_point = env_point + 1;
                if (next_point < envelope.num_points) {
                    ++env_tick;

                    // Advance envelope point
                    const uint32_t next_tick = envelope.points[next_point].position;
                    if (env_tick == next_tick) {
                        env_point = next_point;
                    }
                }
                else {
                    assert(env_tick == envelope.points[envelope.num_points - 1].position);
                }
            }
            // Apply sustain loop
            else {
                assert(env_point == envelope.sustain_last_point);

                if (env_point == envelope.sustain_first_point) {
                    assert(env_tick == envelope.points[env_point].position);
                }
                else {
                    assert(env_tick == envelope.points[env_point].position);
                    env_point = envelope.sustain_first_point;
                    env_tick  = envelope.points[env_point].position;

                    // Loop back and try to advance to next tick from envelope start
                    continue;
                }
            }
            break;
        }
    }

    state->point = static_cast<uint16_t>(env_point);
    state->tick  = static_cast<uint16_t>(env_tick);

    return value;
}

void propagate_parameters(Parameter* params, const ParamDescriptor* descs, uint32_t num_params)
{
    // Snapshot every parameter's value so all reads this step see a consistent previous
    // state.  Envelope generators are exempt: their step pre-pass writes the pair (value at
    // the step's starting tick, prev_value one tick ahead) themselves, which gives consumers
    // an old/new pair spanning exactly this step's tick interval.
    for (uint32_t param_idx = 0; param_idx < num_params; param_idx++) {
        if (descs[param_idx].kind == ParamKind::envelope) {
            continue;
        }
        params[param_idx].prev_value = params[param_idx].value;
    }

    for (uint32_t param_idx = 0; param_idx < num_params; param_idx++) {
        const ParamDescriptor& desc = descs[param_idx];
        if (desc.kind != ParamKind::plain) {
            continue;
        }

        float value = desc.plain.base_value;
        for (uint32_t source_idx = 0; source_idx < desc.plain.num_sources; source_idx++) {
            const SourceParam& source       = desc.plain.sources[source_idx];
            const float        contribution = source.scale * params[source.param_id].prev_value;
            if (source.op == SourceOp::add) {
                value += contribution;
            }
            else {
                value *= contribution;
            }
        }

        params[param_idx].value = value;
    }
}

void configure_lfo(ParamDescriptor*     desc,
                   const LFODescriptor& lfo,
                   SourceOp             lfo_op,
                   float                lfo_depth,
                   uint16_t             lfo_depth_param_id,
                   uint16_t             lfo_rate_param_id,
                   float                lfo_rate_scale)
{
    *desc                    = {};
    desc->kind               = ParamKind::lfo;
    desc->lfo.lfo            = lfo;
    desc->lfo.op             = lfo_op;
    desc->lfo.depth          = lfo_depth;
    desc->lfo.depth_param_id = lfo_depth_param_id;
    desc->lfo.rate_param_id  = lfo_rate_param_id;
    desc->lfo.rate_scale_ms  = lfo_rate_scale;
}

void configure_plain(ParamDescriptor*   desc,
                     float              base_value,
                     uint16_t           env_param_id,
                     float              env_scale,
                     uint16_t           lfo_param_id,
                     SourceOp           lfo_op,
                     const SourceParam* inputs,
                     uint32_t           num_inputs)
{
    assert((env_param_id ? 1u : 0u) + (lfo_param_id ? 1u : 0u) + num_inputs <= max_param_sources);

    *desc      = {};
    desc->kind = ParamKind::plain;

    uint32_t num_sources = 0;

    if (env_param_id) {
        desc->plain.sources[num_sources++] = { env_param_id, env_scale, SourceOp::add };
    }

    if (lfo_param_id) {
        desc->plain.sources[num_sources++] = { lfo_param_id, 1.0f, lfo_op };
    }

    for (uint32_t input_idx = 0; input_idx < num_inputs; input_idx++) {
        desc->plain.sources[num_sources++] = inputs[input_idx];
    }

    desc->plain.base_value  = base_value;
    desc->plain.num_sources = static_cast<uint16_t>(num_sources);
}

uint32_t get_ringbuf_data_size(const uint64_t write_pos, const uint64_t read_pos)
{
    return static_cast<uint32_t>(write_pos - read_pos);
}

uint32_t get_ringbuf_avail_space(const uint64_t write_pos, const uint64_t read_pos, const uint32_t capacity)
{
    return capacity - get_ringbuf_data_size(write_pos, read_pos);
}

uint32_t get_ringbuf_contig_tail(const uint64_t pos, const uint32_t capacity)
{
    return capacity - static_cast<uint32_t>(pos % capacity);
}

} // namespace Synth

uint32_t Synth::sustain_decay_ticks(const float release_seconds)
{
    // Zero (and negative or NaN, which no decoded bank can carry) is the
    // legacy gate: no decay.
    if (! (release_seconds > 0.0f)) {
        return 0;
    }

    // One control step is rt_step_samples / rt_sampling_rate seconds.
    const float ticks = release_seconds * static_cast<float>(rt_sampling_rate) / static_cast<float>(rt_step_samples);

    // Saturate instead of trapping on an out-of-range float-to-unsigned
    // conversion (hand-built JSON is adversarial even though the decoder
    // bounds the value to 3600 s).
    if (! (ticks < 4.0e9f)) {
        return UINT32_MAX;
    }

    // Round to nearest, but any positive release decays for at least one step.
    const uint32_t rounded = static_cast<uint32_t>(ticks + 0.5f);
    return rounded ? rounded : 1;
}

float Synth::sustain_decay_per_tick(const uint32_t total_ticks)
{
    // exp2(log2(cutoff) / total_ticks): the factor crosses the cutoff at
    // exactly tick total_ticks.  No decay without a release length.
    return total_ticks ? mstd::exp2(-10.0f / static_cast<float>(total_ticks)) : 1.0f;
}

bool Synth::sustain_decay_owns_termination(const bool decaying, const float dest_value)
{
    // The decay owns termination only while the completed dest level is still
    // audible; a silent dest frees through the existing completion rule
    // (the factor times 0 is 0).
    return decaying && dest_value != 0.0f;
}

float Synth::oscillator_final_pitch(const int midi_note, const int octave_offset, const float pitch_value)
{
    return static_cast<float>(midi_note) + 12.0f * static_cast<float>(octave_offset) + pitch_value;
}

bool Synth::oscillator_pitch_in_gate(const float final_pitch)
{
    // Inclusive bounds, no rounding; NaN compares false and rejects.
    return final_pitch >= 0.0f && final_pitch <= 127.0f;
}

float Synth::pitch_to_frequency(const float absolute_pitch, const uint32_t freq_mult)
{
    return note_to_frequency(69, absolute_pitch - 69.0f, freq_mult);
}

bool Synth::oscillator_fir_cutoffs(const float      effective_pitch,
                                   const float      effective_spread,
                                   const uint32_t   freq_mult,
                                   const FilterType type,
                                   float*           lowpass_hz,
                                   float*           highpass_hz)
{
    // A single NaN-safe test: NaN, 0 and negatives all read as filter-off.
    if (type == fir_off || ! (effective_spread > 0.0f)) {
        return false;
    }

    if (type == fir_lowpass) {
        *lowpass_hz  = effective_spread;
        *highpass_hz = 0.0f;
        return true;
    }

    if (type == fir_highpass) {
        *lowpass_hz  = 0.0f; // Transport "edge off" so the shader runs pure highpass.
        *highpass_hz = effective_spread;
        return true;
    }

    *lowpass_hz  = pitch_to_frequency(effective_pitch + effective_spread, freq_mult);
    *highpass_hz = pitch_to_frequency(effective_pitch - effective_spread, freq_mult);

    if (*highpass_hz < oscillator_fir_highpass_floor_hz) {
        *highpass_hz = 0.0f; // Transport "edge off" so the shader runs pure lowpass.
    }

    return true;
}

void Synth::oscillator_fir_band_guard(uint32_t* lowpass_cutoff_freq, uint32_t* highpass_cutoff_freq)
{
    if (*highpass_cutoff_freq == 0 || *highpass_cutoff_freq < *lowpass_cutoff_freq) {
        return; // Pure lowpass, or already a band-pass with nonzero bandwidth.
    }
    if (*lowpass_cutoff_freq >= max_cutoff_hz) {
        *highpass_cutoff_freq = *lowpass_cutoff_freq - 1; // Both clamped: widen downward.
    }
    else {
        *lowpass_cutoff_freq = *highpass_cutoff_freq + 1; // Zero bandwidth: widen upward.
    }
}

uint32_t Synth::fir_effective_span(const float slope_percent)
{
    // NaN-safe clamp: NaN fails both comparisons and reads as the minimum.
    // The endpoints are forced, so the approximate exp2 cannot miss them.
    if (! (slope_percent > 1.0f)) {
        return 11;
    }
    if (slope_percent >= 100.0f) {
        return num_fir_taps;
    }

    // log2(num_fir_taps / 11) is 6.541976...
    const float    exponent = 6.541977f * (slope_percent - 1.0f) / 99.0f;
    const float    span     = 11.0f * mstd::exp2(exponent);
    const uint32_t odd      = 2 * static_cast<uint32_t>((span - 1.0f) * 0.5f + 0.5f) + 1; // round half up
    return odd < num_fir_taps ? odd : num_fir_taps;
}

float Synth::eval_sourced_lfo(const ParamDescriptor::LFOParam& gen,
                              const float                      depth_source_value,
                              const float                      rate_source_value,
                              const uint32_t                   lfo_tick)
{
    const float depth = gen.depth_param_id ? depth_source_value * gen.depth : gen.depth;
    // A rate source offsets the LFO's own period (ms) by source * rate_scale_ms; without one,
    // period 0 tells eval_lfo_mod to use the descriptor period unchanged.  Clamp the offset
    // result to at least 1 ms so an editor-supplied scale can never drive the period negative
    // (unsigned underflow) or to zero (a divide-by-zero in the LFO phase).
    const float    base_ms   = static_cast<float>(gen.lfo.period_ms);
    const float    offset_ms = base_ms + rate_source_value * gen.rate_scale_ms;
    const uint32_t period    = gen.rate_param_id ? static_cast<uint32_t>(offset_ms > 1.0f ? offset_ms : 1.0f) : 0;
    return eval_lfo_mod(gen.lfo, lfo_tick, rt_step_samples, rt_sampling_rate, period, depth, gen.op);
}

float Synth::param_onset_value(const Parameter* params, const ParamDescriptor* descs, const uint16_t param_id)
{
    const ParamDescriptor& desc = descs[param_id];

    switch (desc.kind) {
        case ParamKind::external:
            // The leaf's current value is the MIDI state at the note-on event.
            return params[param_id].value;

        case ParamKind::envelope:
            // At note-on the state was just cleared, so this is the first
            // point's value; the state stays untouched.
            return eval_envelope_at(desc.envelope, params[param_id].envelope);

        case ParamKind::lfo:
            // Tick-zero evaluation from the current leaf values, mirroring the
            // advance branch without advancing the tick or touching any state.
            return eval_sourced_lfo(desc.lfo,
                                    params[desc.lfo.depth_param_id].value,
                                    params[desc.lfo.rate_param_id].value,
                                    0);

        case ParamKind::plain:
        default: {
            // Same composition as propagate_parameters, but every source is
            // evaluated at its onset value.
            float value = desc.plain.base_value;
            for (uint32_t source_idx = 0; source_idx < desc.plain.num_sources; source_idx++) {
                const SourceParam& source       = desc.plain.sources[source_idx];
                const float        contribution = source.scale * param_onset_value(params, descs, source.param_id);
                if (source.op == SourceOp::add) {
                    value += contribution;
                }
                else {
                    value *= contribution;
                }
            }
            return value;
        }
    }
}
