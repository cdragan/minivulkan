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
    fir_multiband,
    num_types
};

constexpr uint32_t num_effect_types = static_cast<uint32_t>(EffectType::num_types);

// Freeverb's comb and allpass delay-line lengths are a published tuning specified
// at freeverb_base_rate.  They are scaled to rt_sampling_rate with the same integer
// division the reverb shader applies, so the reverb keeps its voicing at any rate and
// the host state size matches the shader's rings exactly.
constexpr uint32_t effect_reverb_comb_base[effect_reverb_num_combs] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
constexpr uint32_t effect_reverb_allpass_base[effect_reverb_num_allpass] = { 556, 441, 341, 225 };

// Chain capacity, shared by the runtime EffectChain and the bank's EffectChainBinding.
constexpr uint32_t max_chain_effects = 4;

// Upper bound on the scalar float params an effect type uses (the compressor uses the most).
constexpr uint32_t max_effect_param_floats = 5;

// Effect-param modulation pool sizing: a bump region holds a dest node and an optional
// LFO leaf for each modulated effect param.
constexpr uint32_t max_effect_mod_params = 32;

// Multi-band FIR effect model: at most four bands per effect.  The band
// bindings' index meaning follows the slot's effect type.  The plain FIR
// effect keeps its dormant legacy layout: index 0 is band 0's gain and bands
// 1..3 carry lowpass, highpass, gain and slope each (1 + 4 * (band - 1) +
// kind), with band 0's edges and slope in the slot's legacy scalar params.
// EffectType::fir_multiband reads the same array uniformly: band b lives at
// 4 * band + (0 = center, 1 = spread, 2 = gain, 3 = slope), all four bands
// alike, and the coefficient staging derives each band's edges from its
// center/spread pair.
constexpr uint32_t max_fir_effect_bands       = 4;
constexpr uint32_t num_fir_effect_band_params = max_fir_effect_bands * 4;

// Active band-binding count for a FIR effect slot, the one type-aware statement
// of the dual-layout policy: the plain fir keeps its legacy layout (band 0's
// gain plus each extra band's four values; a count of 0 or 1 is the single-band
// marker with no active band-array params), while fir_multiband reads the array
// uniformly at four bindings per band with a zeroed count meaning one.  Other
// types carry no band array.  Callers clamp or refuse out-of-bound counts
// before use, per their own contract.
constexpr uint32_t fir_active_band_param_count(EffectType type, uint32_t band_count)
{
    if (type == EffectType::fir_multiband) {
        return 4 * (band_count < 1 ? 1 : band_count);
    }
    if (type == EffectType::fir) {
        return band_count <= 1 ? 0 : 1 + 4 * (band_count - 1);
    }
    return 0;
}

// Band-array binding count that can carry a reference, the one bound shared by
// load validation, reclaim rooting and reference scans: the plain fir is
// dormant-inclusive (all 16 bindings, its dormant seam can carry authored
// bindings), while a fir_multiband covers only its clamped active bands (its
// dormant tail is always zeroed).  The clamp is a no-op for validation, which
// refuses oversized counts first.
constexpr uint32_t fir_reference_band_param_count(EffectType type, uint32_t band_count)
{
    return type == EffectType::fir
               ? num_fir_effect_band_params
               : fir_active_band_param_count(type,
                                             band_count > max_fir_effect_bands ? max_fir_effect_bands : band_count);
}

// Whether the slot's legacy scalar bindings (bindings[0..num_params)) carry live
// parameters.  A fir_multiband keeps them only as unused stored shape (its band
// array carries the parameters), so they validate but are never counted, built,
// rooted or walked as references.
constexpr bool effect_reads_legacy_params(EffectType type)
{
    return type != EffectType::fir_multiband;
}

// The two FIR effect types share the coefficient-staging machinery; the band
// array's index meaning differs (the plain FIR's dormant legacy order vs the
// multiband's uniform center/spread/gain/slope), so every type gate and the
// staging dispatch key on this one predicate.
constexpr bool is_fir_effect_type(EffectType type)
{
    return type == EffectType::fir || type == EffectType::fir_multiband;
}

// Storage-buffer offset alignment, pinned to the Vulkan spec minimum of
// minStorageBufferOffsetAlignment; the runtime asserts the device honors it.
constexpr uint32_t effect_state_alignment = 256;

// Device bytes reserved for effect state across all chains: the device buffer size minus
// the named non-effect consumers (oscillator FIR coefficient/history buffers, per-step
// output buffers) minus headroom for small auxiliary device allocations.  Admits a
// worst-case chain of 4 delay effects (static_assert in synth_effects.cpp).  The runtime
// pins the exact carve accounting when it initializes the buffer.
constexpr uint32_t effect_state_round = effect_state_alignment - 1;
constexpr uint32_t effect_fir_reserve =
    (max_oscillators - 1) * (((num_fir_taps * sizeof(float) + effect_state_round) & ~effect_state_round) +
                             (((num_fir_taps - 1) * sizeof(float) + effect_state_round) & ~effect_state_round));
constexpr uint32_t effect_output_reserve =
    (max_channels + 1) * ((rt_step_samples * 2 * sizeof(float) + effect_state_round) & ~effect_state_round) +
    max_oscillators * ((rt_step_samples * sizeof(float) + effect_state_round) & ~effect_state_round);
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
// oscillator staging does.  band_params holds the plain FIR's FirEffectBands-order live
// values (index 0 = band 0's gain, then bands 1..3 lowpass, highpass, gain, slope).
bool stage_fir_effect_coeff_entry(uint32_t             taps_offs,
                                  uint8_t              band_count,
                                  float                band0_lowpass_hz,
                                  float                band0_highpass_hz,
                                  float                band0_slope_percent,
                                  const float*         band_params,
                                  FirEffectCoeffEntry* entry);

// Stages one fir_multiband effect's coefficient entry from its live band values.
// band_params holds the uniform FirEffectBands-order live values (band b at 4 * b +
// 0/1/2/3 = center Hz, spread Hz, gain, slope percent); every band's edges derive from
// its center/spread pair (highpass = center - spread/2, lowpass = center + spread/2)
// and clamp to [0, Nyquist], so a band may be half-open: a highpass edge of 0 is a
// pure low band, and a lowpass edge at or above Nyquist stages as a pure high band
// (lowpass cutoff 0, no lowpass kernel).  A band whose spread is nonpositive or whose
// center is nonpositive is inactive and contributes nothing.  band_count 0 reads as
// the single-band marker.  Returns false when no band is active, so the caller drops
// the entry like the oscillator staging does.
bool stage_fir_multiband_coeff_entry(uint32_t             taps_offs,
                                     uint8_t              band_count,
                                     const float*         band_params,
                                     FirEffectCoeffEntry* entry);

// The one center/spread -> edge policy of a multiband band: highpass = center -
// spread/2 (NaN or nonpositive reads as off, 0), lowpass = center + spread/2
// clamped to Nyquist.  Callers apply the band-activity predicate first.
void fir_multiband_band_edges(float center_hz, float spread_hz, float* highpass, float* lowpass);

// The band-activity predicate: both center and spread positive (NaN reads as absent).
constexpr bool fir_multiband_band_active(const float center_hz, const float spread_hz)
{
    return spread_hz > 0.0f && center_hz > 0.0f;
}

} // namespace Synth
