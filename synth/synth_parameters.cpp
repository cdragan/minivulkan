// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_parameters.h"
#include "../core/mstdc.h"
#include "../core/vmath.h"
#include "../core/vecfloat.h"
#include "../core/rng.h"
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

        case WaveType::sawtooth_wave:
            {
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

float eval_envelope(const EnvelopeDescriptor& envelope, EnvelopeState* state, bool sustain)
{
    uint32_t env_point = state->point;
    uint32_t env_tick  = state->tick;

    // Apply current position of the envelope
    const EnvelopeDescriptor::Point pt1 = envelope.points[env_point];

    int env_value = pt1.value;

    if (env_tick > pt1.position) {
        assert(env_point + 1 < envelope.num_points);

        const EnvelopeDescriptor::Point pt2 = envelope.points[env_point + 1];

        const int delta_pos = static_cast<int>(env_tick - pt1.position);
        const int duration  = static_cast<int>(pt2.position - pt1.position);
        const int range     = static_cast<int>(pt2.value) - env_value;

        env_value += (delta_pos * range) / duration;
    }

    const float value = envelope.min_value + static_cast<float>(env_value) * envelope.min_max_delta;

    for (;;) {
        // Advance envelope
        if ( ! sustain || env_point < envelope.sustain_last_point) {

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

    state->point = static_cast<uint16_t>(env_point);
    state->tick  = static_cast<uint16_t>(env_tick);

    return value;
}

void propagate_parameters(Parameter* params, const ParamDescriptor* descs, uint32_t num_params)
{
    // Snapshot every parameter's value so all reads this step see a consistent previous
    // state.
    for (uint32_t param_idx = 0; param_idx < num_params; param_idx++) {
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

void configure_lfo(ParamDescriptor* desc,
                   uint16_t         lfo_desc_id,
                   SourceOp         lfo_op,
                   float            lfo_depth,
                   uint16_t         lfo_depth_param_id,
                   uint16_t         lfo_rate_param_id,
                   float            lfo_rate_scale)
{
    *desc = { };
    desc->kind                = ParamKind::lfo;
    desc->lfo.desc_id         = lfo_desc_id;
    desc->lfo.op              = lfo_op;
    desc->lfo.depth           = lfo_depth;
    desc->lfo.depth_param_id  = lfo_depth_param_id;
    desc->lfo.rate_param_id   = lfo_rate_param_id;
    desc->lfo.rate_scale_ms   = lfo_rate_scale;
}

void configure_plain(ParamDescriptor*   desc,
                     float              base_value,
                     uint16_t           env_param_id,
                     uint16_t           lfo_param_id,
                     SourceOp           lfo_op,
                     const SourceParam* inputs,
                     uint32_t           num_inputs)
{
    assert((env_param_id ? 1u : 0u) + (lfo_param_id ? 1u : 0u) + num_inputs <= max_param_sources);

    *desc = { };
    desc->kind = ParamKind::plain;

    uint32_t num_sources = 0;

    if (env_param_id) {
        desc->plain.sources[num_sources++] = { env_param_id, 1.0f, SourceOp::add };
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
