// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 2) const uint num_taps = 1025;

layout(constant_id = 6) const uint sampling_rate = 1;

const uint no_wave       = 0;
const uint sine_wave     = 1;
const uint sawtooth_wave = 2;
const uint pulse_wave    = 3;
const uint noise_wave    = 4;
const uint pad_wave      = 5;
const uint string_wave   = 6;
const uint noise_salt    = 0x5bd1e995u; // Decorrelates the second oscillator's noise

// Pad wave time base wraps every 2^20 samples; drift rates are quantized to
// whole cycles per wrap period so the pad stays phase-continuous across the mask.
const uint pad_time_wrap = 1u << 20;
const float inv_pad_time_wrap = 1.0f / 1048576.0f;

const float two_pi = 6.283185307179586;

const uint osc_mode_blend     = 0;  // Mix osc_type[0] and osc_type[1] by osc_mix (default)
const uint osc_mode_fm        = 1;  // osc_type[0] is carrier, osc_type[1] is modulator
const uint osc_mode_hard_sync = 2;  // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave
const uint osc_mode_ring_mod  = 3;  // osc_type[0] is the note-tied master, osc_type[1] is the ring-mod carrier

struct OscillatorParams {
    uint  out_sound_offs;   // Offset of output sound data
    float phase;            // Initial phase value at first sample to render, 1 is equivalent to wave length
    float phase_step;       // Phase step between samples, derived from oscillator's and sampling frequencies
    uint  osc_pack;         // Bits 0-7: osc_type[0], 8-15: osc_type[1], 16-23: osc_mode, 24: voice releasing (string), 25-31 spare (zero)
    float duty[2];          // Duty cycle for sawtooth and pulse oscillator [0..1]
    float osc_mix;          // Mixing between osc_type[0] and osc_type[1] [0..1]
    float mod_ratio;        // FM: modulator frequency / carrier frequency.  Hard sync: slave cycles per master cycle.  Ring mod: carrier cycles per master cycle
    float fm_index;         // FM modulation depth
    float mod_phase;        // Modulator's initial phase value at first sample to render
    float mod_phase_step;   // Modulator's phase step between samples

    // Optional filter parameters
    uint  fir_memory_offs;            // Offset of FIR filter's memory
    uint  taps_offs;                  // Offset of FIR filter's taps
    uint  noise_counter;              // Sample index into the noise stream, independent of pitch
    uint  note_seed;                  // Per-note constant: the note-on noise counter, never advanced
    float pad_tilt[2];                // Pad spectral tilt: harmonic exponent is 1 + clamp(tilt, -1, 1)
    uint  string_state_offs;          // String state region in floats: header words 0..13, then the ring
    float string_damping;             // String damping [0..1], higher = shorter decay
    float string_release_damping;     // Effective release damping 0.025..1.667: 0.05 / T60 in s, curved from the knob
    float string_brightness;          // String pluck brightness [0..1], 1 = unfiltered burst, applied at note-on
    float string_dispersion;          // String dispersion [0..1], 0 = off (the plain string, bit-identical)
    uint  string_chain;               // Note seed which started the string chain; a takeover keeps it
};

layout(binding = 0) buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer param_buf { OscillatorParams params[]; };

shared float cached_input[num_taps - 1 + work_group_size];

uint pcg_hash(uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// PAD-style additive tone: sums the band-limited harmonic series of the base
// frequency.  Each harmonic holds a fixed random phase and drifts slowly and
// independently in phase, which approximates the smeared, decorrelated
// spectrum of a PADsynth wavetable without storing one.  duty scales the
// wander depth; pad_tilt sets the spectral exponent (1 + clamp(tilt, -1, 1)).
float pad_oscillator(float phase, float duty, float tilt, uint seed, float pad_time, float phase_step)
{
    // Silent when the fundamental is at or above Nyquist; otherwise at least one
    // harmonic sounds, and a harmonic sitting exactly on Nyquist is excluded.
    if (phase_step >= 0.5) {
        return 0.0;
    }
    const float harmonic_limit = 0.5 / max(phase_step, 1e-9) - 1e-6;
    const uint  num_harmonics  = uint(min(24.0, floor(harmonic_limit)));

    // Duty sets the wander depth (about 2 percent at duty 1).  The spectral tilt
    // is separate: exponent 1 + clamp(tilt, -1, 1), so 0 keeps the saw-like 1/n
    // spectrum, +1 gives 1/n^2 (mellow) and -1 gives a flat spectrum.
    const float duty_depth = clamp(duty, 0.0, 1.0);
    const float wander_ratio = duty_depth * 0.02;
    const float base_freq = phase_step * float(sampling_rate);
    const float exponent = 1.0 + clamp(tilt, -1.0, 1.0);

    float value = 0.0;
    float norm = 0.0;

    for (uint n = 1u; n <= num_harmonics; n++) {
        const float nfloat = float(n);
        const float rand_a = float(pcg_hash(seed ^ (n * 0x85ebca6bu))) / 4294967296.0;
        const float rand_b = float(pcg_hash(seed ^ (n * 0x9e3779b9u))) / 4294967296.0;
        const float rand_c = float(pcg_hash(seed ^ (n * 0xc2b2ae35u))) / 4294967296.0;

        // Fixed random phase decorrelates the harmonic from note onset.
        const float phase_offs = two_pi * rand_a;

        // Drift rate quantized to 3..10 cycles per wrap period, which is
        // k * sampling_rate / 1048576 Hz.  Whole cycles per wrap keep the wave
        // phase-continuous when the masked time base wraps.
        const float k = 3.0 + float(uint(8.0 * rand_b));

        // The amplitude divides by the drift rate, so the pitch deviation stays
        // at wander_ratio of the harmonic's frequency regardless of the drawn rate.
        const float wander_amp = wander_ratio * nfloat * base_freq * float(pad_time_wrap) /
            (two_pi * k * float(sampling_rate));
        const float wander = wander_amp * sin(two_pi * (k * pad_time + rand_c));

        const float amp = 1.0 / pow(nfloat, exponent);
        value += amp * sin(two_pi * (nfloat * phase + wander) + phase_offs);
        norm  += amp;
    }

    return value / max(norm, 1e-9);
}

// Band-limited step residual (PolyBLEP).  t is the phase in [0, 1) and dt the phase
// step per sample.  Add (jump / 2) * poly_blep(phase - discontinuity) to the naive
// wave, so a step of height jump at phase 0 does not alias.
float poly_blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    else if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }

    return 0.0;
}

// Integral of poly_blep (PolyBLAMP) for a change of slope.  Add
// (slope_change / 2) * dt * poly_blamp(phase - kink, dt) to the naive wave.
float poly_blamp(float t, float dt)
{
    if (t < dt) {
        t = 1.0 - t / dt;
        return t * t * t / 3.0;
    }
    else if (t > 1.0 - dt) {
        t = 1.0 + (t - 1.0) / dt;
        return t * t * t / 3.0;
    }

    return 0.0;
}

float oscillator(uint osc_type, float phase, float duty, float tilt, uint noise_index, uint note_seed, float pad_time, float phase_step)
{
    // Wave edges are band-limited, so the wave does not alias at high pitch.
    const float step_dt = min(phase_step, 0.5);

    if (osc_type == sine_wave) {
        return sin(two_pi * phase);
    }
    else if (osc_type == sawtooth_wave) {
        phase = fract(phase);

        float value = 2.0 * phase;

        if (duty < 0.0001) {
            // Falling ramp: step up at the wrap.
            return 1.0 - value + poly_blep(phase, step_dt);
        }
        else if (duty > 0.999) {
            // Rising ramp: step down at the wrap.
            return value - 1.0 - poly_blep(phase, step_dt);
        }
        else if (phase < duty) {
            value /= duty;
        }
        else {
            value = (2.0 - value) / (1.0 - duty);
        }

        // Two slope changes: at the wrap and at the duty point, equal and opposite.
        // The kink window cannot exceed half of the shorter segment, or the
        // large slope change would overshoot.
        const float kink_dt = min(step_dt, 0.5 * min(duty, 1.0 - duty));
        const float kink_half = 1.0 / (duty * (1.0 - duty));
        const float correction = poly_blamp(phase, kink_dt) - poly_blamp(fract(phase - duty), kink_dt);

        return value - 1.0 + kink_half * kink_dt * correction;
    }
    else if (osc_type == pulse_wave) {
        phase = fract(phase);

        const float value = (phase < duty) ? 1.0 : -1.0;

        return value + poly_blep(phase, step_dt) - poly_blep(fract(phase - duty), step_dt);
    }
    else if (osc_type == pad_wave) {
        return pad_oscillator(phase, duty, tilt, note_seed, pad_time, phase_step);
    }
    else { // noise_wave
        const uint uvalue = pcg_hash(noise_index);
        return (float(uvalue & 0xFFFFu) / 32767.5) - 1.0;
    }
}

const uint   string_ring_size    = 2034;
const uint   string_header_words = 14;
const float  string_min_freq     = 22.0;
const float  string_dispersion_strength = 0.05863;
shared float string_block_out[256];
shared float string_burst[2048];
shared uint  string_write_pos;
shared float string_loop_length;

// Largest root r >= 1 of r^3 - r = q for q > 0.  Closed form, no iteration: three real roots up to
// q = 2 / (3 sqrt(3)) (trigonometric form), one real root beyond (Cardano form).
float string_dispersion_ratio(float q)
{
    if (q <= 0.3849002) {
        const float cos_arg = min(1.0, 2.598076 * q);
        return 1.1547005 * cos(acos(cos_arg) * 0.33333334);
    }
    const float cardano = pow(q * 0.5 + sqrt(q * q * 0.25 - 0.037037037), 0.33333334);
    return cardano + 0.33333334 / cardano;
}

// Phase delay in samples of the first-order allpass (coeff + z^-1) / (1 + coeff * z^-1) at omega.
float string_allpass_phase_delay(float coeff, float omega)
{
    return (2.0 / omega) * atan((1.0 - coeff) * sin(0.5 * omega), (1.0 + coeff) * cos(0.5 * omega));
}

float string_ring_read(uint ring_offs, float loop_phase)
{
    if (string_loop_length == 0.0) {
        return 0.0;
    }

    const float loop_offset = (loop_phase - floor(loop_phase)) * string_loop_length;
    const float read_pos    = float(string_write_pos) - 1.0 - string_loop_length + loop_offset;
    int         i0          = int(floor(read_pos));
    const float read_frac   = read_pos - float(i0);

    i0 += (i0 < 0) ? int(string_ring_size) : 0;

    int i1 = i0 + 1;
    i1 -= (i1 >= int(string_ring_size)) ? int(string_ring_size) : 0;

    const float s0 = data[ring_offs + uint(i0)];
    const float s1 = data[ring_offs + uint(i1)];
    return s0 + read_frac * (s1 - s0);
}

// Karplus-Strong plucked string, run once per 256-sample block.
// The ring holds one loop of excitation: a zero-mean noise burst, optionally softened
// by a circular one-pole lowpass (brightness below 1) that keeps the burst RMS.
// Each sample is read delay_whole samples behind the write point, then damped by a one-pole
// lowpass (string_damping_coeff) and tuned to the fractional delay by an allpass
// (string_allpass_coeff).  string_loop_gain sets the decay to the target T60.
// string_release_gain glides toward release_gain_target on note release to shorten the tail.
// Dispersion (stiff-string inharmonicity) adds one first-order allpass section after the
// tuning allpass.  Its phase delay at f0 is subtracted from the loop delay so the pitch stays put,
// while its delay falls with frequency, which lifts the partials.  Dispersion 0 skips all of it.
// Header layout: 0 write pos, 1 prev lowpass, 2 prev allpass, 3 release gain, 4 loop length,
// 5-6 seed tag, 7 dispersion section state, 8-9 chain token (two-word integer), 10-13 unused.
void string_pass(uint  base,
                 float f0,
                 float damping,
                 float release_damping,
                 float brightness,
                 float dispersion,
                 bool  release,
                 uint  note_seed,
                 uint  chain_token)
{
    const uint  lane        = gl_LocalInvocationID.x;
    const uint  ring_offs   = base + string_header_words;
    const float sample_rate = float(sampling_rate);

    if (lane == 0u) {
        string_write_pos   = 0u;
        string_loop_length = 0.0;
    }
    if (! (f0 >= string_min_freq)) {
        // Too low to tune: silence, state untouched.
        string_block_out[lane] = 0.0;
        return;
    }

    float loop_length                = sample_rate / f0;
    loop_length                      = loop_length < 3.0 ? 3.0 : loop_length;
    const float string_damping_coeff = 0.10 + 0.30 * damping;
    const float lowpass_delay        = string_damping_coeff / (1.0 - string_damping_coeff);
    const float decay_t60            = 20.0 * pow(0.25 / 20.0, damping);
    const float omega                = 6.2831853 * f0 / sample_rate;
    const float lowpass_mag      = (1.0 - string_damping_coeff) / sqrt(1.0 - 2.0 * string_damping_coeff * cos(omega) +
                                                                       string_damping_coeff * string_damping_coeff);
    const float loop_gain_raw    = pow(10.0, -3.0 * loop_length / (sample_rate * decay_t60)) / lowpass_mag;
    const float string_loop_gain = loop_gain_raw < 0.9999 ? loop_gain_raw : 0.9999;

    // Dispersion block-rate setup, gated off entirely at 0 (a zero-coefficient section still delays one sample).
    // The cascade is a single allpass section (the section count is frozen at one).  It falls back to no section
    // at all when it would leave less than 3 samples of loop delay.
    // Dispersion is modulatable: a modulated value changes the timbre every block while the retune below
    // (loop_length minus the section delay) holds the fundamental.
    float disp_coeff  = 0.0;
    float disp_delay  = 0.0;
    bool  disp_active = false;
    if (dispersion > 0.0) {
        const float disp_q = string_dispersion_strength * dispersion / (omega * omega * omega);
        const float disp_r = string_dispersion_ratio(disp_q);
        const float section_coeff = (1.0 - disp_r) / (1.0 + disp_r);
        const float section_delay = string_allpass_phase_delay(section_coeff, omega);
        if (loop_length - lowpass_delay - section_delay >= 3.0) {
            disp_coeff  = section_coeff;
            disp_delay  = section_delay;
            disp_active = true;
        }
    }
    const float delay                = loop_length - lowpass_delay - disp_delay;
    const uint  delay_whole      = uint(floor(delay - 1.0));
    const float delay_frac       = delay - float(delay_whole);
    const float string_allpass_coeff = (1.0 - delay_frac) / (1.0 + delay_frac);

    // Init runs on the first in-range pass of a note.  The seed tag (header words 5 and 6) records
    // which note seed the ring was filled for, so a note that starts below the pitch floor still
    // gets a fresh pluck on its first tunable pass.  The low half is stored +1 so zeroed memory
    // never matches, and NaN compares false, so corrupt state re-initialises instead of replaying.
    const float string_seed_tag_lo = float((note_seed & 0xFFFFu) + 1u);
    const float string_seed_tag_hi = float(note_seed >> 16);
    const bool  init               = ! (data[base + 5u] == string_seed_tag_lo && data[base + 6u] == string_seed_tag_hi);
    // Chain token (header words 8 and 9), same two-word integer split as the seed tag.  A note which
    // continues the chain of the string already in the region adds its burst to the ringing ring; any
    // other note replaces it.  Zeroed or stale memory never matches, and NaN compares false.
    const float string_chain_lo = float((chain_token & 0xFFFFu) + 1u);
    const float string_chain_hi = float(chain_token >> 16);
    const bool  additive        = init && data[base + 8u] == string_chain_lo && data[base + 9u] == string_chain_hi;
    uint        write_pos;
    float       string_prev_lowpass;
    float       string_prev_allpass;
    float       string_release_gain;
    float       disp_state;
    if (init) {
        // Note-on or retrigger: noise burst, then zero mean.  Lane 0 owns the whole excitation; the other
        // lanes leave the init block and reach the barrier in main() after the pass.
        if (lane == 0u) {
            float sum = 0.0;
            for (uint i = 0u; i < delay_whole; i++) {
                const float noise_value = float(pcg_hash(note_seed + i) & 0xFFFFu) / 32767.5 - 1.0;
                string_burst[i]         = noise_value;
                sum += noise_value;
            }
            const float mean = sum / float(delay_whole);
            for (uint k = 0u; k < delay_whole; k++) {
                string_burst[k] -= mean;
            }
        }
        if (lane == 0u && brightness < 1.0) {
            // Pluck brightness: a circular one-pole lowpass over the finished burst, skipped at 1 so
            // the default excitation stays bit-identical.  A zero-state lap gives the tail, which
            // fixes the exact wrap-around start state so the ring closes without a seam.  The
            // result is re-centered and rescaled to the burst RMS, so brightness shapes tone only.
            const float tone_coeff = 0.95 * (1.0 - clamp(brightness, 0.0, 1.0));
            float       tone_state = 0.0;
            float       energy_in  = 0.0;
            for (uint k = 0u; k < delay_whole; k++) {
                const float sample_in = string_burst[k];
                tone_state            = (1.0 - tone_coeff) * sample_in + tone_coeff * tone_state;
                energy_in += sample_in * sample_in;
            }
            tone_state /= 1.0 - pow(tone_coeff, float(delay_whole));
            float tone_sum    = 0.0;
            float tone_energy = 0.0;
            for (uint k = 0u; k < delay_whole; k++) {
                tone_state          = (1.0 - tone_coeff) * string_burst[k] + tone_coeff * tone_state;
                string_burst[k]     = tone_state;
                tone_sum += tone_state;
                tone_energy += tone_state * tone_state;
            }
            const float tone_mean = tone_sum / float(delay_whole);
            const float tone_var  = tone_energy - tone_sum * tone_mean;
            const float tone_gain = tone_var > 0.0 ? sqrt(energy_in / tone_var) : 1.0;
            for (uint k = 0u; k < delay_whole; k++) {
                string_burst[k] = (string_burst[k] - tone_mean) * tone_gain;
            }
        }
        if (lane == 0u) {
            data[base + 5u] = string_seed_tag_lo;
            data[base + 6u] = string_seed_tag_hi;
            data[base + 8u] = string_chain_lo;
            data[base + 9u] = string_chain_hi;
            if (additive) {
                // Add the burst onto the live window [write_pos - delay_whole, write_pos); the filter and
                // dispersion state keep running, only the release gain restarts.
                write_pos           = floatBitsToUint(data[base]);
                string_prev_lowpass = data[base + 1u];
                string_prev_allpass = data[base + 2u];
                disp_state          = data[base + 7u];
                for (uint i = 0u; i < delay_whole; i++) {
                    data[ring_offs + (write_pos + i + (string_ring_size - delay_whole)) % string_ring_size] +=
                        string_burst[i];
                }
            }
            else {
                for (uint i = 0u; i < delay_whole; i++) {
                    data[ring_offs + i] = string_burst[i];
                }
                write_pos           = delay_whole;
                string_prev_lowpass = 0.0;
                string_prev_allpass = 0.0;
                disp_state          = 0.0;
                data[base + 7u]     = 0.0;
            }
            string_release_gain = 1.0;
        }
    }
    else if (lane == 0u) {
        write_pos           = floatBitsToUint(data[base]);
        string_prev_lowpass = data[base + 1u];
        string_prev_allpass = data[base + 2u];
        string_release_gain = data[base + 3u];
        disp_state          = data[base + 7u];
    }

    if (lane != 0u) {
        return;
    }

    const float release_gain_target  = pow(10.0, -release_damping * 3.0 * loop_length / (sample_rate * 0.05));
    const float release_smooth_coeff = 1.0 - exp(-1.0 / (sample_rate * 0.003));
    const float release_target       = release ? release_gain_target : 1.0;

    for (uint i = 0u; i < 256u; i++) {
        const uint ring_read_pos =
            write_pos >= delay_whole ? write_pos - delay_whole : write_pos + string_ring_size - delay_whole;
        const float pass_input = data[ring_offs + ring_read_pos];

        string_block_out[i] = pass_input;
        string_release_gain += release_smooth_coeff * (release_target - string_release_gain);

        const float lowpass_out =
            (1.0 - string_damping_coeff) * pass_input + string_damping_coeff * string_prev_lowpass;
        const float allpass_out =
            string_allpass_coeff * lowpass_out + string_prev_lowpass - string_allpass_coeff * string_prev_allpass;
        float cascade_out = allpass_out;
        if (disp_active) {
            const float section_out = disp_coeff * cascade_out + disp_state;
            disp_state              = cascade_out - disp_coeff * section_out;
            cascade_out             = section_out;
        }
        float loop_output = cascade_out * (string_loop_gain * string_release_gain);

        // Flush runaway or non-finite values to silence so a blown-up loop cannot poison the ring.
        loop_output                 = abs(loop_output) < 8.0 ? loop_output : 0.0;
        data[ring_offs + write_pos] = loop_output;
        string_prev_lowpass         = lowpass_out;
        string_prev_allpass         = allpass_out;
        write_pos                   = write_pos + 1u == string_ring_size ? 0u : write_pos + 1u;
    }

    data[base]         = uintBitsToFloat(write_pos);
    data[base + 1u]    = string_prev_lowpass;
    data[base + 2u]    = string_prev_allpass;
    data[base + 3u]    = string_release_gain;
    data[base + 4u]    = loop_length;
    if (disp_active) {
        data[base + 7u] = disp_state;
    }
    string_write_pos   = write_pos;
    string_loop_length = loop_length;
}

void main()
{
    const OscillatorParams param = params[gl_WorkGroupID.x];

    const float phase = param.phase + param.phase_step * gl_LocalInvocationID.x;

    // Noise advances per sample, not per phase, so its spectrum does not depend on pitch.
    const uint noise_index = param.noise_counter + gl_LocalInvocationID.x;

    // Pad wave time base: the unsalted noise counter masked to the wrap period.
    const float pad_time = float(noise_index & (pad_time_wrap - 1u)) * inv_pad_time_wrap;

    // Per-note seeds: the pad's random draws hash from the note seed, and the
    // noise key is the note seed run through pcg_hash so it is decorrelated
    // from the time base; oscillator B is salted to stay decorrelated from A.
    const uint note_seed = param.note_seed;
    const uint noise_key = pcg_hash(note_seed);

    const uint  type1    = param.osc_pack & 0xFFu;
    const float duty1    = param.duty[0];
    const uint  type2    = (param.osc_pack >> 8) & 0xFFu;
    const float duty2    = param.duty[1];
    const float tilt1    = param.pad_tilt[0];
    const float tilt2    = param.pad_tilt[1];
    const uint  osc_mode = (param.osc_pack >> 16) & 0xFFu;
    const uint  lane     = gl_LocalInvocationID.x;

    const bool is_string = type1 == string_wave;
    if (is_string && !(type2 != no_wave && osc_mode == osc_mode_hard_sync)) {
        string_pass(param.string_state_offs,
                    param.phase_step * float(sampling_rate),
                    param.string_damping,
                    param.string_release_damping,
                    param.string_brightness,
                    param.string_dispersion,
                    ((param.osc_pack >> 24) & 1u) != 0u,
                    note_seed,
                    param.string_chain);
        // barrier() alone does not publish the data[] ring writes.
        memoryBarrierBuffer();
        memoryBarrierShared();
        barrier();
    }

    float value;

    if (type2 == no_wave) {
        value = is_string ? string_block_out[lane] : oscillator(type1, phase, duty1, tilt1, noise_index ^ noise_key, note_seed, pad_time, param.phase_step);
    }
    else if (osc_mode == osc_mode_fm) {
        // Frequency modulation: type1 is the carrier, type2 the modulator.
        // The modulator runs at its own frequency with its own phase.
        const float mod_phase = param.mod_phase + param.mod_phase_step * gl_LocalInvocationID.x;
        const float modulator = oscillator(type2, mod_phase, duty2, tilt2, noise_index ^ noise_key ^ noise_salt, note_seed ^ noise_salt, pad_time, param.mod_phase_step);

        value = is_string ? string_ring_read(param.string_state_offs + string_header_words, phase + param.fm_index * modulator) : oscillator(type1, phase + param.fm_index * modulator, duty1, tilt1, noise_index ^ noise_key, note_seed, pad_time, param.phase_step);
    }
    else if (osc_mode == osc_mode_hard_sync) {
        // first oscillator is the master providing the note frequency
        // second oscillator is the slave whose phase resets each time the master phase wraps
        // mod_ratio is used as the sync ratio (slave cycles per master cycle).
        value = oscillator(type2, fract(param.mod_ratio * fract(phase)), duty2, tilt2, noise_index ^ noise_key ^ noise_salt, note_seed ^ noise_salt, pad_time, param.mod_ratio * param.phase_step);
    }
    else if (osc_mode == osc_mode_ring_mod) {
        // type1 is the note-tied master
        // type2 is the carrier evaluated at mod_ratio times the master phase
        // osc_mix crossfades the dry master to the product.
        const float carrier = oscillator(type2, fract(param.mod_ratio * fract(phase)), duty2, tilt2, noise_index ^ noise_salt, note_seed ^ noise_salt, pad_time, param.mod_ratio * param.phase_step);

        value = is_string ? string_block_out[lane] : oscillator(type1, phase, duty1, tilt1, noise_index ^ noise_key, note_seed, pad_time, param.phase_step);
        value = mix(value, value * carrier, param.osc_mix);
    }
    else {
        // Blend mode: mix the two oscillators by osc_mix.
        value = is_string ? string_block_out[lane] : oscillator(type1, phase, duty1, tilt1, noise_index ^ noise_key, note_seed, pad_time, param.phase_step);

        if (type2 != no_wave) {
            value = mix(value, oscillator(type2, phase, duty2, tilt2, noise_index ^ noise_key ^ noise_salt, note_seed ^ noise_salt, pad_time, param.phase_step), param.osc_mix);
        }
    }

    // Apply FIR filter (optional)
    if (param.taps_offs != 0) {

        // Read previously saved inputs
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            cached_input[tap] = data[param.fir_memory_offs + tap];
        }

        // Store generated sample values
        const uint base_pos = num_taps - 1 + gl_LocalInvocationID.x;
        cached_input[base_pos] = value;

        barrier();

        // Save current inputs for next invocation
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            data[param.fir_memory_offs + tap] = cached_input[work_group_size + tap];
        }

        // Apply FIR filter convolution
        value *= data[param.taps_offs];

        for (uint tap = 1; tap < num_taps; tap++) {
            value += data[param.taps_offs + tap] * cached_input[base_pos - tap];
        }
    }

    // Output generated sample
    data[param.out_sound_offs + gl_LocalInvocationID.x] = value;
}
