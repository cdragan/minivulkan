// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

// One table for all per-oscillator modulation targets.  A row has these columns:
//   name           token; the enum id is mod_<name> and the JSON key is "<name>"
//   label          GUI row label
//   min, max       slider bounds, in display units
//   scale          factor from display units to stored (bank) units
//   log            true for a logarithmic slider
//   default_value  base value when a routing object has no "base" key, in display units
//   description    GUI hover text
// The row order is the ModTarget order.  It is part of the bank layout, so do not reorder rows.
// Use the table with: #define X(name, label, min, max, scale, log, default_value, description) ...
//
// clang-format off
#define SYNTH_MOD_TARGET_LIST(X) \
    X(volume, "Volume", 0.0f, 1.0f, 1.0f, false, 1.0f, \
        "Amplitude of the output oscillator wave") \
    X(pitch, "Pitch", -24.0f, 24.0f, 1.0f, false, 0.0f, \
        "Tuning of the oscillator waves (in semitones, -24..24)") \
    X(panning, "Pan", 0.0f, 1.0f, 1.0f, false, 0.5f, \
        "Stereo position of the output (0 = left, 0.5 = center, 1 = right)") \
    X(duty_a, "Duty A", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Shape adjustment of oscillator wave (sawtooth ramp point, pulse width, pad wander)") \
    X(duty_b, "Duty B", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Shape adjustment of oscillator wave (sawtooth ramp point, pulse width, pad wander)") \
    X(osc_mix, "Waveform Mix", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Balance between primary and secondary oscillator wave (0 = waveform A, 1 = Waveform B)") \
    X(fm_depth, "FM Depth", 0.0f, 1.0f, 6.2831853f, false, 0.0f, \
        "Amount of frequency modulation applied by secondary wave to primary wave") \
    X(fir_spread, "Band spread", 0.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band width around its center (in semitones)") \
    X(fm_ratio, "FM Ratio", Synth::osc_mod_ratio_min, Synth::osc_mod_ratio_max, 1.0f, false, 1.0f, \
        "Secondary oscillator wave frequency ratio to the note pitch") \
    X(pad_tone_a, "Pad Tone A", -1.0f, 1.0f, 1.0f, false, 0.0f, \
        "Pad wave harmonic tilt (-1 flat, +1 mellow)") \
    X(pad_tone_b, "Pad Tone B", -1.0f, 1.0f, 1.0f, false, 0.0f, \
        "Pad wave harmonic tilt (-1 flat, +1 mellow)") \
    X(fir_slope, "FIR Slope", 1.0f, 100.0f, 1.0f, false, 0.0f, \
        "Filter edge sharpness") \
    X(fir_band0_offset, "Offset", -36.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band center offset from the note pitch, in semitones") \
    X(fir_band0_gain, "Gain", 0.01f, 1.0f, 1.0f, false, 0.0f, \
        "Band's output amplitude") \
    X(fir_band1_offset, "Offset", -36.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band center offset from the note pitch, in semitones") \
    X(fir_band1_spread, "Spread", 0.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band width around its center (in semitones)") \
    X(fir_band1_gain, "Gain", 0.01f, 1.0f, 1.0f, false, 0.0f, \
        "Band's output amplitude") \
    X(fir_band1_slope, "FIR Slope", 1.0f, 100.0f, 1.0f, false, 0.0f, \
        "Filter edge sharpness") \
    X(fir_band2_offset, "Offset", -36.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band center offset from the note pitch, in semitones") \
    X(fir_band2_spread, "Spread", 0.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band width around its center (in semitones)") \
    X(fir_band2_gain, "Gain", 0.01f, 1.0f, 1.0f, false, 0.0f, \
        "Band's output amplitude") \
    X(fir_band2_slope, "FIR Slope", 1.0f, 100.0f, 1.0f, false, 0.0f, \
        "Filter edge sharpness") \
    X(fir_band3_offset, "Offset", -36.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band center offset from the note pitch, in semitones") \
    X(fir_band3_spread, "Spread", 0.0f, 36.0f, 1.0f, false, 0.0f, \
        "Band width around its center (in semitones)") \
    X(fir_band3_gain, "Gain", 0.01f, 1.0f, 1.0f, false, 0.0f, \
        "Band's output amplitude") \
    X(fir_band3_slope, "FIR Slope", 1.0f, 100.0f, 1.0f, false, 0.0f, \
        "Filter edge sharpness") \
    X(string_damping, "Sustain Damping", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "How fast the string vibration decays and darkens while the key is held") \
    X(string_release_damping, "Release Damping", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Extra string damping applied while the key is released") \
    X(string_dispersion, "String Dispersion", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Stiff-string inharmonicity: raises the upper partials, strongest on low notes (0 = off)") \
    X(string_brightness, "Brightness", 0.0f, 1.0f, 1.0f, false, 1.0f, \
        "Brightness of the pluck excitation") \
    X(string_hammer, "Hammer", 0.0f, 1.0f, 1.0f, false, 0.0f, \
        "Amount of felt hammer excitation; gradually overtakes the pluck") \
    X(string_hammer_contact, "Contact (ms)", 0.5f, 10.0f, 1.0f, false, Synth::string_hammer_contact_default, \
        "Felt hammer contact window in milliseconds") \
    X(string_hammer_gain, "Gain", 0.0f, 4.0f, 1.0f, false, Synth::string_hammer_gain_default, \
        "Felt hammer excitation gain") \

// clang-format on
