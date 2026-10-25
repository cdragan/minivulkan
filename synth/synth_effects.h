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
    ring_mod,
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

// Chain capacity, shared by the runtime EffectChain and the bank's EffectChainBinding.
constexpr uint32_t max_chain_effects = 4;

// Upper bound on the scalar float params an effect type uses (the compressor uses the most).
constexpr uint32_t max_effect_param_floats = 5;

// Effect-param modulation pool sizing: a bump region holds a dest node and an optional
// LFO leaf for each modulated effect param.
constexpr uint32_t max_effect_mod_params = 32;

// Multi-band FIR effect model: at most four bands per effect.  Band 0's
// lowpass, highpass and slope stay in the slot's legacy scalar params; the
// dedicated band bindings carry band 0's gain plus the full parameter set of
// bands 1..3 (lowpass, highpass, gain, slope each).
constexpr uint32_t max_fir_effect_bands       = 4;
constexpr uint32_t num_fir_effect_band_params = 1 + (max_fir_effect_bands - 1) * 4;

// Storage-buffer offset alignment, pinned to the Vulkan spec minimum of
// minStorageBufferOffsetAlignment; the runtime asserts the device honors it.
constexpr uint32_t effect_state_alignment = 256;

// Device bytes reserved for effect state across all chains: the device buffer size minus
// the named non-effect consumers (oscillator FIR coefficient/history buffers, per-step
// output buffers) minus headroom for small auxiliary device allocations.  Admits a
// worst-case chain of 4 delay effects (static_assert in synth_effects.cpp).  The runtime
// pins the exact carve accounting when it initializes the buffer.
constexpr uint32_t effect_state_round = effect_state_alignment - 1;
constexpr uint32_t effect_fir_reserve = (max_oscillators - 1)
    * (((num_fir_taps * sizeof(float) + effect_state_round) & ~effect_state_round)
       + (((num_fir_taps - 1) * sizeof(float) + effect_state_round) & ~effect_state_round));
constexpr uint32_t effect_output_reserve =
    (max_channels + 1) * ((rt_step_samples * 2 * sizeof(float) + effect_state_round) & ~effect_state_round)
    + max_oscillators * ((rt_step_samples * sizeof(float) + effect_state_round) & ~effect_state_round);
// Device bytes reserved for the per-oscillator string wave state regions.
constexpr uint32_t effect_string_reserve =
    (max_oscillators - 1) * ((string_state_floats * sizeof(float) + effect_state_round) & ~effect_state_round);
constexpr uint32_t effect_state_headroom = 8192;
constexpr uint32_t effect_state_budget =
    effect_buffer_bytes - effect_fir_reserve - effect_output_reserve - effect_string_reserve - effect_state_headroom;

// Number of scalar float params an effect type uses.
uint32_t get_effect_param_floats(EffectType type);

uint32_t get_effect_state_floats(EffectType type);

// Aligned device bytes one enabled effect instance reserves for state.
uint32_t get_effect_state_bytes(EffectType type);

// One staged FIR band's words, matching the coefficient shader's FIRBand record layout
// word for word (no padding).
struct FirEffectCoeffBand {
    uint32_t highpass_cutoff_freq; // High pass frequency (0 = a pure low pass)
    uint32_t lowpass_cutoff_freq;  // Low pass frequency (0 = a pure high pass)
    uint32_t window_span;          // Effective odd window span in taps, 11..num_fir_taps
    float    gain;                 // The band's normalized kernel scales with this
};

// One staged FIR coefficient entry: the coefficient shader's FIRParams record layout,
// word for word (fir_coeff_entry_words words, no padding).
struct FirEffectCoeffEntry {
    uint32_t           taps_offs;  // Offset of the FIR filter's taps
    uint32_t           band_count; // Bands staged in bands[0..band_count - 1]
    FirEffectCoeffBand bands[max_fir_effect_bands];
};

// Stages one FIR effect's coefficient entry from its live values.  band_count 1 (or the
// legacy 0) stages the legacy single-band form with the gain forced to 1.0, so legacy
// banks keep their bit-exact staging.  band_count 2..4 stages every active band and
// returns false when no band is active, so the caller drops the entry like the
// oscillator staging does.  band_params holds the FirEffectBands-order live values
// (index 0 = band 0's gain, then bands 1..3 lowpass, highpass, gain, slope).
bool stage_fir_effect_coeff_entry(uint32_t             taps_offs,
                                  uint8_t              band_count,
                                  float                band0_lowpass_hz,
                                  float                band0_highpass_hz,
                                  float                band0_slope_percent,
                                  const float*         band_params,
                                  FirEffectCoeffEntry* entry);

} // namespace Synth
