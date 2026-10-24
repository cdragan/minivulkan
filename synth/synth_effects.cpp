// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_effects.h"

static constexpr uint32_t get_freeverb_scaled_length(uint32_t base_length)
{
    return base_length * Synth::rt_sampling_rate / Synth::freeverb_base_rate;
}

static constexpr uint32_t align_effect_state_bytes(uint32_t bytes)
{
    return (bytes + Synth::effect_state_alignment - 1) & ~(Synth::effect_state_alignment - 1);
}

static constexpr uint32_t effect_state_floats(Synth::EffectType type)
{
    switch (type) {
        case Synth::EffectType::distortion:
            // The previous raw input sample of each channel (ADAA).
            return 2;
        case Synth::EffectType::delay:
            // One write-position counter plus a stereo (x2) ring buffer
            // of effect_delay_max_samples samples per channel.
            return 1 + 2 * Synth::effect_delay_max_samples;

        case Synth::EffectType::chorus:
            // An LFO phase accumulator and a write-position counter, plus a
            // stereo (x2) ring buffer of effect_chorus_max_samples per channel.
            return 2 + 2 * Synth::effect_chorus_max_samples;

        case Synth::EffectType::reverb: {
            // One master state word plus, per stereo side (x2): the rate-scaled comb
            // rings, one lowpass state per comb, and the rate-scaled allpass rings.
            uint32_t comb_total = 0;
            for (uint32_t comb_idx = 0; comb_idx < Synth::effect_reverb_num_combs; comb_idx++) {
                comb_total += get_freeverb_scaled_length(Synth::effect_reverb_comb_base[comb_idx]);
            }
            uint32_t allpass_total = 0;
            for (uint32_t allpass_idx = 0; allpass_idx < Synth::effect_reverb_num_allpass; allpass_idx++) {
                allpass_total += get_freeverb_scaled_length(Synth::effect_reverb_allpass_base[allpass_idx]);
            }
            return 1 + 2 * (comb_total + Synth::effect_reverb_num_combs + allpass_total);
        }

        case Synth::EffectType::compressor:
            // One envelope follower state float.
            return 1;

        case Synth::EffectType::ring_mod:
            // One carrier phase accumulator.
            return 1;

        case Synth::EffectType::fir:
            // The coeff buffer (num_fir_taps), a stereo (x2) input-history ring of
            // num_fir_taps - 1 frames, and one write-position counter.
            return Synth::num_fir_taps + 2 * (Synth::num_fir_taps - 1) + 1;

        default:
            return 0;
    }
}

// The state budget must admit a worst-case chain: every slot a delay effect.
static_assert(Synth::max_chain_effects *
                  align_effect_state_bytes(effect_state_floats(Synth::EffectType::delay) * sizeof(float)) <=
              Synth::effect_state_budget);

uint32_t Synth::get_effect_state_floats(Synth::EffectType type)
{
    return effect_state_floats(type);
}

uint32_t Synth::get_effect_state_bytes(Synth::EffectType type)
{
    return align_effect_state_bytes(effect_state_floats(type) * sizeof(float));
}

uint32_t Synth::get_effect_param_floats(Synth::EffectType type)
{
    switch (type) {
        case Synth::EffectType::distortion:
            return 3; // drive, mix, output gain
        case Synth::EffectType::delay:
            return 3;
        case Synth::EffectType::chorus:
            return 3;
        case Synth::EffectType::reverb:
            return 3;
        case Synth::EffectType::compressor:
            return 5;
        case Synth::EffectType::fir:
            return 3; // lowpass cutoff Hz, highpass cutoff Hz, FIR slope percent
        case Synth::EffectType::ring_mod:
            return 2; // carrier rate Hz, mix
        default:
            return 0;
    }
}
