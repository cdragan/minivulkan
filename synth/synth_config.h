// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

namespace Synth {

// Sampling frequency, i.e. frequency of the produced audio buffer.
constexpr uint32_t rt_sampling_rate = 44100;

// Maximum number of supported MIDI channels.
constexpr uint32_t max_channels = 16;

// Maximum instruments per channel (keyboard splits).
constexpr uint32_t max_instr_per_channel = 16;

// Maximum oscillators a single note uses.
constexpr uint32_t max_layers = 7;

// Maximum points in an envelope.
constexpr uint32_t max_envelope_points = 8;

// Maximum source parameters feeding one parameter.
constexpr uint32_t max_param_sources = 4;

// Maximum input parameters a single target binding declares (beyond its envelope and LFO).
constexpr uint32_t max_mod_inputs = 2;

// Editor-side pool capacities.
constexpr uint32_t max_instruments = 16;
constexpr uint32_t max_envelopes   = 32;
constexpr uint32_t max_lfos        = 32;
constexpr uint32_t max_parameters  = 64;

// Maximum length of an instrument or channel name, including the terminator.
constexpr uint32_t max_name_len = 24;

// FIR filter tap count, shared by the per-oscillator FIR and the FIR effect.
constexpr uint32_t num_fir_taps = 1025;

// Effect delay-line capacities.
constexpr uint32_t effect_delay_max_samples  = rt_sampling_rate;       // 1 s
constexpr uint32_t effect_chorus_max_samples = rt_sampling_rate / 20;  // 50 ms

// Freeverb tuning: comb and allpass delay lengths are published at freeverb_base_rate and
// scaled to rt_sampling_rate (see get_freeverb_scaled_length in synth_effects.h).
constexpr uint32_t freeverb_base_rate        = 44100;
constexpr uint32_t effect_reverb_num_combs   = 8;
constexpr uint32_t effect_reverb_num_allpass = 4;

} // namespace Synth
