// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_modulation.h"
#include "../core/mstdc.h"
#include "../core/vmath.h"
#include "../core/vecfloat.h"
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

float eval_lfo(const LFODescriptor& lfo, uint32_t lfo_tick, uint32_t step_samples, uint32_t sampling_rate)
{
    float value = lfo.min_value;

    const float phase = static_cast<float>(lfo_tick) * static_cast<float>(step_samples * 1000u) /
                        static_cast<float>(lfo.period_ms * sampling_rate);

    switch (lfo.wave) {
        case sine_wave:
            {
                const float sval = vmath::sincos(phase * vmath::two_pi).sin;

                value += (sval + 1.0f) * 0.5f * lfo.min_max_delta;
            }
            break;

        case sawtooth_wave:
            {
                const float frac_phase = phase - static_cast<float>(static_cast<int>(phase));
                const float duty       = static_cast<float>(lfo.duty) / 255.0f;

                if (frac_phase <= duty) {
                    if (duty > 0.0f) {
                        value += lfo.min_max_delta * frac_phase / duty;
                    }
                }
                else {
                    const float fall = 1.0f - duty;
                    if (fall > 0.0f) {
                        value += lfo.min_max_delta * (1.0f - frac_phase) / fall;
                    }
                }
            }
            break;

        default:
            // Only sine and sawtooth LFOs are supported
            assert(lfo.wave == sine_wave || lfo.wave == sawtooth_wave);
            break;
    }

    return value;
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

float eval_parameter(float                     base_value,
                     const EnvelopeDescriptor* envelope,
                     bool                      sustain,
                     const LFODescriptor*      lfo,
                     float                     midi_value,
                     ParameterState*           state,
                     uint32_t                  step_samples,
                     uint32_t                  sampling_rate)
{
    float value = base_value;

    if (envelope) {
        value += eval_envelope(*envelope, &state->envelope, sustain);
    }

    if (lfo) {
        value += eval_lfo(*lfo, state->lfo_tick, step_samples, sampling_rate);
        ++state->lfo_tick;
    }

    value += midi_value;

    return value;
}

uint32_t instrument_param_slot_count(const TargetBinding* bindings, uint32_t unison_count)
{
    uint32_t count = 0;

    for (uint32_t target = 0; target < num_mod_targets; target++) {
        if (bindings[target].scope == scope_voice) {
            if (bindings[target].param_desc_id[0]) {
                count++;
            }
        }
        else {
            for (uint32_t unison_idx = 0; unison_idx < unison_count; unison_idx++) {
                if (bindings[target].param_desc_id[unison_idx]) {
                    count++;
                }
            }
        }
    }

    return count;
}

uint32_t effect_param_floats(EffectType type)
{
    switch (type) {
        case effect_distortion: return 2;
        case effect_delay:      return 3;
        case effect_chorus:     return 3;
        case effect_reverb:     return 3;
        case effect_compressor: return 5;
        case effect_fir:        return 2;  // lowpass cutoff Hz, highpass cutoff Hz (0 = edge disabled)
        default:                return 0;
    }
}

uint32_t effect_state_floats(EffectType type)
{
    switch (type) {
        case effect_distortion:
            return 0;

        case effect_delay:
            // One write-position counter plus a stereo (x2) ring buffer
            // of effect_delay_max_samples samples per channel.
            return 1 + 2 * effect_delay_max_samples;

        case effect_chorus:
            // An LFO phase accumulator and a write-position counter, plus a
            // stereo (x2) ring buffer of effect_chorus_max_samples per channel.
            return 2 + 2 * effect_chorus_max_samples;

        case effect_reverb: {
            // One master state word plus, per stereo side (x2): the rate-scaled comb
            // rings, one lowpass state per comb, and the rate-scaled allpass rings.
            uint32_t comb_total = 0;
            for (uint32_t comb_idx = 0; comb_idx < effect_reverb_num_combs; comb_idx++) {
                comb_total += freeverb_scaled_length(effect_reverb_comb_base[comb_idx]);
            }
            uint32_t allpass_total = 0;
            for (uint32_t allpass_idx = 0; allpass_idx < effect_reverb_num_allpass; allpass_idx++) {
                allpass_total += freeverb_scaled_length(effect_reverb_allpass_base[allpass_idx]);
            }
            return 1 + 2 * (comb_total + effect_reverb_num_combs + allpass_total);
        }

        case effect_compressor:
            // One envelope follower state float.
            return 1;

        case effect_fir:
            // The coeff buffer (num_fir_taps), a stereo (x2) input-history ring of
            // num_fir_taps - 1 frames, and one write-position counter.
            return num_fir_taps + 2 * (num_fir_taps - 1) + 1;

        default:
            return 0;
    }
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
