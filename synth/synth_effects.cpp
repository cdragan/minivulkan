// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_effects.h"
#include "synth_parameters.h"

static bool fir_effect_band_active(const float lowpass_hz, const float highpass_hz)
{
    // NaN fails both comparisons, so a NaN edge reads as absent.
    return highpass_hz > 0.0f && highpass_hz < lowpass_hz;
}

bool Synth::stage_fir_effect_coeff_entry(const uint32_t       taps_offs,
                                         const uint8_t        band_count,
                                         const float          band0_lowpass_hz,
                                         const float          band0_highpass_hz,
                                         const float          band0_slope_percent,
                                         const float*         band_params,
                                         FirEffectCoeffEntry* entry)
{
    static_assert(sizeof(FirEffectCoeffEntry) == fir_coeff_entry_words * sizeof(uint32_t));

    *entry           = FirEffectCoeffEntry();
    entry->taps_offs = taps_offs;

    // The legacy single band stages unconditionally with the gain forced to 1.0, so
    // legacy banks keep their bit-exact staging.
    if (band_count <= 1) {
        entry->band_count                    = 1;
        entry->bands[0].lowpass_cutoff_freq  = cutoff_hz_to_int(band0_lowpass_hz);
        entry->bands[0].highpass_cutoff_freq = cutoff_hz_to_int(band0_highpass_hz);
        entry->bands[0].window_span          = fir_effective_span(band0_slope_percent);
        entry->bands[0].gain                 = 1.0f;
        return true;
    }

    uint32_t num_bands = 0;
    for (uint32_t band = 0; band < max_fir_effect_bands && band < band_count; band++) {
        // Band 0's edges and slope stay in the slot's legacy scalar params; its gain sits at
        // band_params[0].  Bands 1..3 carry all four values in FirEffectBands order.
        const uint32_t band_base     = band == 0 ? 0 : 1 + (band - 1) * 4;
        const float    lowpass_hz    = band == 0 ? band0_lowpass_hz : band_params[band_base + 0];
        const float    highpass_hz   = band == 0 ? band0_highpass_hz : band_params[band_base + 1];
        const float    slope_percent = band == 0 ? band0_slope_percent : band_params[band_base + 3];
        float          gain          = band == 0 ? band_params[0] : band_params[band_base + 2];
        // Band 0 keeps the legacy edges, so a pure lowpass with the highpass
        // absent stays active (the ruled inactivity test applies to bands 1..3,
        // which need both edges present and ordered).
        const bool band_active =
            band == 0 ? ! (highpass_hz >= lowpass_hz) : fir_effect_band_active(lowpass_hz, highpass_hz);
        if (! band_active) {
            continue;
        }
        // The modulated gain clamps to [0, 1]; a gain <= 0 or NaN stages a zero-gain band,
        // mirroring the oscillator's band loop.
        if (gain > 1.0f) {
            gain = 1.0f;
        }
        if (! (gain > 0.0f)) {
            gain = 0.0f;
        }
        uint32_t lowpass  = cutoff_hz_to_int(lowpass_hz);
        uint32_t highpass = cutoff_hz_to_int(highpass_hz);
        // The guard widens a post-conversion zero-bandwidth or double-clamped pair the way
        // the oscillator's band loop does.
        oscillator_fir_band_guard(&lowpass, &highpass);
        FirEffectCoeffBand* out = &entry->bands[num_bands];
        num_bands++;
        out->highpass_cutoff_freq = highpass;
        out->lowpass_cutoff_freq  = lowpass;
        out->window_span          = fir_effective_span(slope_percent);
        out->gain                 = gain;
    }
    if (num_bands == 0) {
        return false;
    }
    entry->band_count = num_bands;
    return true;
}

bool Synth::stage_fir_multiband_coeff_entry(const uint32_t       taps_offs,
                                            const uint8_t        band_count,
                                            const float*         band_params,
                                            FirEffectCoeffEntry* entry)
{
    static_assert(sizeof(FirEffectCoeffEntry) == fir_coeff_entry_words * sizeof(uint32_t));

    *entry           = FirEffectCoeffEntry();
    entry->taps_offs = taps_offs;

    // The count byte travels unvalidated between banks, so the clamp precedes
    // every use; a zeroed count is the single-band marker the runtime reads as 1.
    uint32_t active_count = band_count;
    if (active_count < 1) {
        active_count = 1;
    }

    uint32_t    num_bands  = 0;
    const float nyquist_hz = static_cast<float>(Synth::rt_sampling_rate / 2);
    for (uint32_t band = 0; band < max_fir_effect_bands && band < active_count; band++) {
        const uint32_t band_base     = 4 * band;
        const float    center_hz     = band_params[band_base + 0];
        const float    spread_hz     = band_params[band_base + 1];
        const float    slope_percent = band_params[band_base + 3];
        float          gain          = band_params[band_base + 2];
        // A band is active when its spread and its center are both positive;
        // NaN fails the comparisons, so a NaN pair reads as absent.  The
        // edges derive as center -+ spread/2 and clamp to [0, Nyquist], so a
        // band may be half-open: a highpass edge of 0 is a pure low band,
        // and a lowpass edge at or above Nyquist is a pure high band, staged
        // with the lowpass cutoff 0 so no lowpass kernel runs (matching the
        // plain FIR's edge-off semantics exactly).
        if (! (spread_hz > 0.0f && center_hz > 0.0f)) {
            continue;
        }
        float highpass_hz = center_hz - spread_hz * 0.5f;
        float lowpass_hz  = center_hz + spread_hz * 0.5f;
        if (! (highpass_hz > 0.0f)) {
            highpass_hz = 0.0f;
        }
        if (lowpass_hz > nyquist_hz) {
            lowpass_hz = nyquist_hz;
        }
        // The modulated gain clamps to [0, 1]; a gain <= 0 or NaN stages a zero-gain band,
        // mirroring the oscillator's band loop.
        if (gain > 1.0f) {
            gain = 1.0f;
        }
        if (! (gain > 0.0f)) {
            gain = 0.0f;
        }
        uint32_t highpass = cutoff_hz_to_int(highpass_hz);
        uint32_t lowpass;
        if (lowpass_hz >= nyquist_hz) {
            lowpass = 0; // Pure high band: no lowpass kernel runs.
        }
        else {
            lowpass = cutoff_hz_to_int(lowpass_hz);
            // The guard widens a post-quantization zero-bandwidth or double-clamped
            // pair the way the oscillator's band loop does.
            oscillator_fir_band_guard(&lowpass, &highpass);
        }
        FirEffectCoeffBand* out = &entry->bands[num_bands];
        num_bands++;
        out->highpass_cutoff_freq = highpass;
        out->lowpass_cutoff_freq  = lowpass;
        out->window_span          = fir_effective_span(slope_percent);
        out->gain                 = gain;
    }
    if (num_bands == 0) {
        return false;
    }
    entry->band_count = num_bands;
    return true;
}

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
        case Synth::EffectType::fir_multiband:
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
        case Synth::EffectType::fir_multiband:
            // The legacy FIR scalars ride along unused (the uniform bands carry
            // center/spread/gain/slope in FirEffectBands), so the slot and
            // document shape match the plain FIR's exactly.
            return 3;
        case Synth::EffectType::ring_mod:
            return 2; // carrier rate Hz, mix
        default:
            return 0;
    }
}
