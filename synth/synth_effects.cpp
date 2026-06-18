// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_effects.h"

namespace Synth {

uint32_t get_effect_param_floats(EffectType type)
{
    switch (type) {
        case EffectType::distortion: return 2;
        case EffectType::delay:      return 3;
        case EffectType::chorus:     return 3;
        case EffectType::reverb:     return 3;
        case EffectType::compressor: return 5;
        case EffectType::fir:        return 2;  // lowpass cutoff Hz, highpass cutoff Hz (0 = edge disabled)
        default:                     return 0;
    }
}

uint32_t get_effect_state_floats(EffectType type)
{
    switch (type) {
        case EffectType::distortion:
            return 0;

        case EffectType::delay:
            // One write-position counter plus a stereo (x2) ring buffer
            // of effect_delay_max_samples samples per channel.
            return 1 + 2 * effect_delay_max_samples;

        case EffectType::chorus:
            // An LFO phase accumulator and a write-position counter, plus a
            // stereo (x2) ring buffer of effect_chorus_max_samples per channel.
            return 2 + 2 * effect_chorus_max_samples;

        case EffectType::reverb: {
            // One master state word plus, per stereo side (x2): the rate-scaled comb
            // rings, one lowpass state per comb, and the rate-scaled allpass rings.
            uint32_t comb_total = 0;
            for (uint32_t comb_idx = 0; comb_idx < effect_reverb_num_combs; comb_idx++) {
                comb_total += get_freeverb_scaled_length(effect_reverb_comb_base[comb_idx]);
            }
            uint32_t allpass_total = 0;
            for (uint32_t allpass_idx = 0; allpass_idx < effect_reverb_num_allpass; allpass_idx++) {
                allpass_total += get_freeverb_scaled_length(effect_reverb_allpass_base[allpass_idx]);
            }
            return 1 + 2 * (comb_total + effect_reverb_num_combs + allpass_total);
        }

        case EffectType::compressor:
            // One envelope follower state float.
            return 1;

        case EffectType::fir:
            // The coeff buffer (num_fir_taps), a stereo (x2) input-history ring of
            // num_fir_taps - 1 frames, and one write-position counter.
            return num_fir_taps + 2 * (num_fir_taps - 1) + 1;

        default:
            return 0;
    }
}

} // namespace Synth
