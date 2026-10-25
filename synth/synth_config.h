// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

namespace Synth {

// Sampling frequency, i.e. frequency of the produced audio buffer.
constexpr uint32_t rt_sampling_rate = 44100;

// Nyquist-clamped integer cutoff ceiling shared by the runtime quantizer and
// the FIR band-pass bandwidth guard.
constexpr uint32_t max_cutoff_hz = rt_sampling_rate / 2 - 1;

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

// Editor-side pool capacities. The envelope/LFO pools stay capped at 255:
// descriptor ids ride in one-byte editor fields (layout record index, the
// graph's detached-node registry), so 256 would silently truncate.
constexpr uint32_t max_instruments = max_channels * max_instr_per_channel;
constexpr uint32_t max_envelopes   = 255;
constexpr uint32_t max_lfos        = 255;
constexpr uint32_t max_parameters  = 64;

// Maximum length of an instrument or channel name, including the terminator.
constexpr uint32_t max_name_len = 24;

// FIR filter tap count, shared by the per-oscillator FIR and the FIR effect.
constexpr uint32_t num_fir_taps = 1025;

// Number of samples rendered in one step. This is also how frequently LFOs and ADSR
// envelopes are updated. This must match workgroup geometry in compute shaders.
constexpr uint32_t rt_step_samples = 256;

// Max oscillators are playing.
constexpr uint32_t max_oscillators = 64;

// Device bytes feeding the synth: effect state, oscillator FIR buffers, per-step outputs.
// Floats of string wave state per oscillator slot: 8 header floats + the 2040-float ring (loop delays
// down to 22 Hz).
constexpr uint32_t string_state_floats = 2048;
// 2 MiB of effect state plus one string state region (8192 bytes) for each of oscillator slots
// 1..max_oscillators-1, i.e. 2 MiB + 63 * 8192 bytes.
constexpr uint32_t effect_buffer_bytes = 2 * 1024 * 1024 + (max_oscillators - 1) * string_state_floats * sizeof(float);

// Effect delay-line capacities.
constexpr uint32_t effect_delay_max_samples  = rt_sampling_rate;      // 1 s
constexpr uint32_t effect_chorus_max_samples = rt_sampling_rate / 20; // 50 ms

// Freeverb tuning: comb and allpass delay lengths are published at freeverb_base_rate and
// scaled to rt_sampling_rate (see get_freeverb_scaled_length in synth_effects.h).
constexpr uint32_t freeverb_base_rate        = 44100;
constexpr uint32_t effect_reverb_num_combs   = 8;
constexpr uint32_t effect_reverb_num_allpass = 4;

} // namespace Synth
