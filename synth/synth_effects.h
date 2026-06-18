// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"

#include <stdint.h>

namespace Synth {

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

// Freeverb's comb and allpass delay-line lengths are a published tuning specified
// at freeverb_base_rate.  They are scaled to rt_sampling_rate with the same integer
// division the reverb shader applies, so the reverb keeps its voicing at any rate and
// the host state size matches the shader's rings exactly.
constexpr uint32_t effect_reverb_comb_base[effect_reverb_num_combs] =
    { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr uint32_t effect_reverb_allpass_base[effect_reverb_num_allpass] =
    { 556, 441, 341, 225 };

// A Freeverb delay-line length scaled from freeverb_base_rate to rt_sampling_rate.
constexpr uint32_t get_freeverb_scaled_length(uint32_t base_length)
{
    return base_length * rt_sampling_rate / freeverb_base_rate;
}

// Number of scalar float params an effect type uses.
uint32_t get_effect_param_floats(EffectType type);

// Number of persistent float slots an effect type keeps in the device state region.
uint32_t get_effect_state_floats(EffectType type);

} // namespace Synth
