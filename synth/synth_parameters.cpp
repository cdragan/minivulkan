// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_parameters.h"
#include "../core/mstdc.h"
#include "../core/rng.h"
#include "../core/vecfloat.h"
#include "../core/vmath.h"
#include "synth_instrument.h"
#include "synth_soundtrack.h"
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

// Pulse waveshaper: morphs the normalized wave toward a pulse (step) shape.
// mix 0 returns the wave bit-exactly; mix 255 returns the hard step, the min or
// max of the normalized range switching at the mid value (inclusive on the high
// side).  In between, half_width = mix / 510 narrows the linear middle band
// [half_width, 1 - half_width] and grows the flat regions toward 0 and 1; the
// output stays within [0, 1] and is monotonic in both wave_value and mix.
static float pulse_shaper(float wave_value, uint8_t pulse_mix)
{
    if (! pulse_mix) {
        return wave_value; // mix 0: bit-exact identity
    }

    if (pulse_mix == 255) {
        return (wave_value >= 0.5f) ? 1.0f : 0.0f; // mix 255: step, no division
    }

    const float half_width = static_cast<float>(pulse_mix) / 510.0f;
    if (wave_value < half_width) {
        return 0.0f; // flat floor: the exact minimum
    }
    if (wave_value > 1.0f - half_width) {
        return 1.0f; // flat ceiling: the exact maximum (the division alone can round an ulp short)
    }
    const float shaped = (wave_value - half_width) / (1.0f - 2.0f * half_width);
    return (shaped > 1.0f) ? 1.0f : shaped; // ponytail: clamps the one-ulp overshoot on the upper boundary
}

// Normalized LFO wave in [0, 1] at the given tick, using period_ms for the rate.
static float eval_lfo_normalized(const LFODescriptor& lfo,
                                 uint32_t             lfo_tick,
                                 uint32_t             step_samples,
                                 uint32_t             sampling_rate,
                                 uint32_t             period_ms)
{
    // Absolute descriptors may carry period_ms == 0 (editor slot, decoded
    // banks); mirror the >= 1 ms clamp used for resolved divisions so the
    // phase division can never see 0.
    if (period_ms == 0) {
        period_ms = 1;
    }
    const float phase = static_cast<float>(lfo_tick) * static_cast<float>(step_samples * 1000u) /
                        static_cast<float>(static_cast<uint64_t>(period_ms) * sampling_rate);

    float wave;

    switch (lfo.wave) {
        case WaveType::sine_wave: {
            // Start point: the cycle position p the wave shows at phase 0
            // (middle 0, lowest 3/4, highest 1/4); see the offset note at the
            // sawtooth case.
            assert(lfo.start_point <= 2);
            const float offset = (lfo.start_point == 1) ? 0.75f : ((lfo.start_point == 2) ? 0.25f : 0.0f);

            wave = (vmath::sincos((phase + offset) * vmath::two_pi).sin + 1.0f) * 0.5f;
            break;
        }

        case WaveType::sawtooth_wave: {
            const float duty = static_cast<float>(lfo.duty) / 255.0f;
            float       frac_phase = phase - static_cast<float>(static_cast<int>(phase));

            // Start point: the cycle position p the wave shows at phase 0
            // (middle duty/2, lowest 0, highest duty).  The direct form
            // frac - (1 - p) can go negative and the frac wrap truncates toward
            // zero, so the equivalent positive form frac + p is used, computed
            // as p itself (never as 1 - (1 - p), whose double rounding shifts
            // the exact phase-0 values by an ulp).
            assert(lfo.start_point <= 2);
            if (lfo.start_point == 2) {
                frac_phase += duty;
            }
            else if (lfo.start_point == 0) {
                frac_phase += duty * 0.5f;
            }
            if (frac_phase >= 1.0f) {
                frac_phase -= 1.0f;
            }

            if (frac_phase <= duty) {
                wave = (duty > 0.0f) ? frac_phase / duty : 0.0f;
            }
            else {
                const float fall = 1.0f - duty;
                wave = (fall > 0.0f) ? (1.0f - frac_phase) / fall : 0.0f;
            }
            break;
        }

        default:
            // Only sine and sawtooth LFOs are supported
            assert(lfo.wave == WaveType::sine_wave || lfo.wave == WaveType::sawtooth_wave);
            return 0.0f;
    }

    // The pulse morph composes with the duty shape and the start-point offset.
    return pulse_shaper(wave, lfo.pulse_mix);
}

float lfo_beat_div_period_ms(uint8_t beat_div)
{
    assert(beat_div <= 5);

    float beats;
    switch (beat_div) {
        case 1:
            beats = 4.0f;   // 1 note
            break;
        case 2:
            beats = 2.0f;   // 1/2 note
            break;
        case 3:
            beats = 1.0f;   // 1/4 note
            break;
        case 4:
            beats = 0.5f;   // 1/8 note
            break;
        case 5:
            beats = 0.25f;  // 1/16 note
            break;
        default:
            return 0.0f;    // div 0 (and invalid values): absolute period applies
    }

    return beats * 60000.0f / static_cast<float>(get_runtime_tempo_bpm());
}

// Resolves an LFO's evaluation period in ms: a set beat_div resolves against the
// runtime tempo (rounded to the nearest ms, clamped to at least 1 ms so the phase
// division can never see 0); div 0 keeps the descriptor's absolute period.
static uint32_t resolved_lfo_period_ms(const LFODescriptor& lfo)
{
    const float resolved_ms = lfo_beat_div_period_ms(lfo.beat_div);
    if (resolved_ms == 0.0f) {
        return lfo.period_ms;
    }

    const uint32_t period = static_cast<uint32_t>(resolved_ms + 0.5f);
    return period ? period : 1;
}

float eval_lfo(const LFODescriptor& lfo, uint32_t lfo_tick, uint32_t step_samples, uint32_t sampling_rate)
{
    const uint32_t period_ms = resolved_lfo_period_ms(lfo);
    const float    wave      = eval_lfo_normalized(lfo, lfo_tick, step_samples, sampling_rate, period_ms);

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
    // the ping_pong flag off) keep the legacy loop below.
    if (sustain && (envelope.flags & envelope_ping_pong) &&
        envelope.sustain_last_point > envelope.sustain_first_point) {
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

uint16_t Synth::quick_release_target(const EnvelopeDescriptor& envelope, const EnvelopeState& state)
{
    // Defensive: num_points == 0 (hand-built malformed JSON) must not underflow
    // the scan bound.
    const uint32_t last_point = envelope.num_points ? envelope.num_points - 1 : 0;
    // Bank-envelope decode does not validate sustain_last_point < num_points,
    // so the scan start clamps defensively.
    const uint32_t scan_start = envelope.sustain_last_point < last_point ? envelope.sustain_last_point : last_point;
    const float    current    = envelope_value_at(envelope, state);

    for (uint32_t point_idx = scan_start; point_idx < last_point; point_idx++) {
        const float point_value =
            envelope.min_value + static_cast<float>(envelope.points[point_idx].value) * envelope.min_max_delta;
        if (point_value <= current) {
            return static_cast<uint16_t>(point_idx);
        }
    }
    return static_cast<uint16_t>(last_point);
}

bool Synth::apply_quick_release(const EnvelopeDescriptor& envelope, EnvelopeState* state)
{
    const uint16_t target = quick_release_target(envelope, *state);

    // Strictly ahead in (point, tick) order; the point positions are sorted,
    // so a same-point target is ahead only when its position is past the tick.
    const bool ahead =
        target > state->point || (target == state->point && envelope.points[target].position > state->tick);
    if (! ahead) {
        return false;
    }

    state->point   = target;
    state->tick    = envelope.points[target].position;
    state->reverse = 0;
    return true;
}

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

float Synth::string_release_damping_curve(const float x)
{
    // 0.05 / (2.0 * exp2(x * log2(0.015))) = 0.025 * exp2(x * -log2(0.015)).
    constexpr float neg_log2_t60_ratio = 6.0588937f;
    const float clamped                = x >= 0.0f ? (x <= 1.0f ? x : 1.0f) : 0.0f;
    return 0.025f * mstd::exp2(clamped * neg_log2_t60_ratio);
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

bool Synth::oscillator_fir_band_cutoffs(const float    effective_pitch,
                                        const float    offset_semitones,
                                        const float    spread_semitones,
                                        const uint32_t freq_mult,
                                        const float    slope_percent,
                                        float*         lowpass_hz,
                                        float*         highpass_hz,
                                        uint32_t*      window_span)
{
    // The bandpass edges reuse the shared cutoff conversion: the NaN-safe
    // spread refusal and the highpass floor policy live there.
    if (! oscillator_fir_cutoffs(effective_pitch + offset_semitones,
                                 spread_semitones,
                                 freq_mult,
                                 fir_bandpass,
                                 lowpass_hz,
                                 highpass_hz)) {
        return false;
    }

    *window_span = fir_effective_span(slope_percent);
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

// The one cutoff range: positive values clamp to [1, Nyquist - 1] Hz; nonpositive
// values and NaN pass through unchanged (they mean "edge off").
float Synth::clamp_cutoff_hz(const float value)
{
    if (! (value > 0.0f)) {
        return value;
    }

    constexpr float min_cutoff_hz = 1.0f;

    if (value <= min_cutoff_hz) {
        return min_cutoff_hz;
    }
    if (value >= static_cast<float>(Synth::max_cutoff_hz)) {
        return static_cast<float>(Synth::max_cutoff_hz);
    }

    return value;
}

// Rounds a cutoff in Hz to the int the coeff shader wants; <= 0 disables the edge, else
// clamped by clamp_cutoff_hz.  Shared by the oscillator and the effect FIR staging.
uint32_t Synth::cutoff_hz_to_int(const float value)
{
    const float clamped = clamp_cutoff_hz(value);

    if (! (clamped > 0.0f)) {
        return 0;
    }

    return static_cast<uint32_t>(clamped + 0.5f);
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

Synth::FirCoeffCacheAction Synth::fir_coeff_cache_action(const void*    prev_entries,
                                                         const uint32_t prev_entry_count,
                                                         const void*    cur_entries,
                                                         const uint32_t cur_entry_count,
                                                         const bool     taps_cleared)
{
    // Zero active filters: no dispatch, and the retained snapshot must not survive an inactive
    // step (the next activation dispatches regardless of what the bytes say).
    if (! cur_entry_count) {
        return fir_coeff_invalidate;
    }
    // The taps content was zeroed by effect-state clears or RESET fills this step, so it is not
    // what the retained snapshot describes; this also covers reactivation after a cleared gap.
    if (taps_cleared) {
        return fir_coeff_dispatch;
    }
    // First dispatch, or reactivation after an invalidated step.
    if (! prev_entry_count) {
        return fir_coeff_dispatch;
    }
    // The active set's size changed.
    if (prev_entry_count != cur_entry_count) {
        return fir_coeff_dispatch;
    }
    // Bytewise compare over every entry, including taps_offs (the active-set membership carrier).
    const uint32_t bytes = cur_entry_count * fir_coeff_entry_words * static_cast<uint32_t>(sizeof(uint32_t));
    const uint8_t* prev  = static_cast<const uint8_t*>(prev_entries);
    const uint8_t* cur   = static_cast<const uint8_t*>(cur_entries);
    for (uint32_t i = 0; i < bytes; i++) {
        if (prev[i] != cur[i]) {
            return fir_coeff_dispatch;
        }
    }
    return fir_coeff_skip;
}

float Synth::eval_sourced_lfo(const ParamDescriptor::LFOParam& gen,
                              const float                      depth_source_value,
                              const float                      rate_source_value,
                              const uint32_t                   lfo_tick)
{
    const float depth = gen.depth_param_id ? depth_source_value * gen.depth : gen.depth;
    // The base period is the beat_div resolution against the runtime tempo when
    // beat_div is set, otherwise the descriptor period (ms).  A rate source offsets
    // that base by source * rate_scale_ms; the offset result clamps to at least 1 ms
    // so an editor-supplied scale can never drive the period negative (unsigned
    // underflow) or to zero (a divide-by-zero in the LFO phase).
    const float    base_ms   = static_cast<float>(resolved_lfo_period_ms(gen.lfo));
    // The rate source only offsets the base when one is configured; its value is
    // ignored otherwise (the caller still passes the leaf snapshot).
    const float    offset_ms = base_ms + (gen.rate_param_id ? rate_source_value * gen.rate_scale_ms : 0.0f);
    const uint32_t period    = static_cast<uint32_t>(offset_ms > 1.0f ? offset_ms : 1.0f);
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
