// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_modulation.h"
#include <assert.h>
#include "../core/mstdc.h"
#include "../core/vmath.h"
#include "../core/vecfloat.h"  // provides vmath::sincos

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

uint32_t effect_param_floats(EffectType type)
{
    switch (type) {
        case effect_distortion: return 2;
        case effect_delay:      return 3;
        case effect_chorus:     return 3;
        case effect_reverb:     return 3;
        case effect_compressor: return 5;
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

        case effect_reverb:
            // One master state word plus stereo (x2) comb and allpass filter memory.
            // effect_reverb_num_combs extra slots hold the per-comb filter state scalars.
            return 1 + 2 * (effect_reverb_comb_sum + effect_reverb_num_combs + effect_reverb_allpass_sum);

        case effect_compressor:
            // One envelope follower state float.
            return 1;

        default:
            return 0;
    }
}

} // namespace Synth
