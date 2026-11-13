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
const uint  pad_time_wrap     = 1u << 20;
const float inv_pad_time_wrap = 1.0f / 1048576.0f;

const float two_pi = 6.283185307179586;

const uint production_step_samples = 256u;

const uint osc_mode_blend     = 0; // Mix osc_type[0] and osc_type[1] by osc_mix (default)
const uint osc_mode_fm        = 1; // osc_type[0] is carrier, osc_type[1] is modulator
const uint osc_mode_hard_sync = 2; // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave
const uint osc_mode_ring_mod  = 3; // osc_type[0] is the note-tied master, osc_type[1] is the ring-mod carrier

struct OscillatorParams {
    uint  out_sound_offs; // Offset of output sound data
    float phase;          // Initial phase value at first sample to render, 1 is equivalent to wave length
    float phase_step;     // Phase step between samples, derived from oscillator's and sampling frequencies
    uint  osc_pack;       // Bits 0-7: osc_type[0], 8-15: osc_type[1], 16-23: osc_mode, 24: voice releasing (string), 25-31
                          // spare (zero)
    float duty[2];        // Duty cycle for sawtooth and pulse oscillator [0..1]
    float osc_mix;        // Mixing between osc_type[0] and osc_type[1] [0..1]
    float mod_ratio;      // FM: modulator frequency / carrier frequency.  Hard sync: slave cycles per master cycle.  Ring
                          // mod: carrier cycles per master cycle
    float fm_index;       // FM modulation depth
    float mod_phase;      // Modulator's initial phase value at first sample to render
    float mod_phase_step; // Modulator's phase step between samples

    // Optional filter parameters
    uint  fir_memory_offs;        // Offset of FIR filter's memory
    uint  taps_offs;              // Offset of FIR filter's taps
    uint  noise_counter;          // Sample index into the noise stream, independent of pitch
    uint  note_seed;              // Per-note constant: the note-on noise counter, never advanced
    float pad_tilt[2];            // Pad spectral tilt: harmonic exponent is 1 + clamp(tilt, -1, 1)
    uint  string_state_offs;      // String state region in floats: header words 0..13, then the ring
    float string_damping;         // String damping [0..1], higher = shorter decay
    float string_release_damping; // Effective release damping 0.025..1.667: 0.05 / T60 in s, curved from the knob
    float string_brightness;      // String pluck brightness [0..1], 1 = unfiltered burst, applied at note-on
    float string_dispersion;      // String dispersion [0..1], 0 = off (the plain string, bit-identical)
    uint  string_chain;           // Note seed which started the string chain; a takeover keeps it
    float string_hammer;          // Hammer mix
    float string_hammer_contact;  // Hammer contact window in milliseconds
    float string_hammer_gain;     // Source gain captured at note-on
    uint  fir_window;
};

layout(binding = 0) buffer data_buf
{
    float data[];
};

layout(binding = 0) readonly buffer coeff_buf
{
    vec4 coefficients[];
};

layout(binding = 1, std430) readonly buffer param_buf
{
    OscillatorParams params[];
};

// One shared region with three tenants whose lifetimes are strictly ordered.  Order:
// string_burst during string_pass, then pad_constants from the lane 0 fill until the last
// oscillator() read, then cached_input (the FIR window) after the barrier before the FIR fill.
// The string numerator needs 2509 floats at the pitch and contact limits.
const uint fir_input_floats =
    num_taps - 1u + (work_group_size > production_step_samples ? work_group_size : production_step_samples);
const uint   string_burst_floats = 2509u;
shared float shared_region[fir_input_floats > string_burst_floats ? fir_input_floats : string_burst_floats];
#define string_burst  shared_region
#define pad_constants shared_region
#define cached_input  shared_region

uint pcg_hash(uint state)
{
    state     = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// PAD-style additive tone: sums the band-limited harmonic series of the base
// frequency.  Each harmonic holds a fixed random phase and drifts slowly and
// independently in phase, which approximates the smeared, decorrelated
// spectrum of a PADsynth wavetable without storing one.  duty scales the
// wander depth; pad_tilt sets the spectral exponent (1 + clamp(tilt, -1, 1)).
void fill_pad_constants(uint slot, float duty, float tilt, uint seed, float phase_step)
{
    const uint  base           = slot * 148u;
    const float harmonic_limit = 0.5 / max(phase_step, 1e-9) - 1e-6;
    const uint  count          = phase_step >= 0.5 ? 0u : uint(min(24.0, floor(harmonic_limit)));
    pad_constants[base + 2u]   = float(count);
    const float clamped_duty   = clamp(duty, 0.0, 1.0);
    const float tilt_exp       = 1.0 + clamp(tilt, -1.0, 1.0);
    float       norm           = 0.0;
    for (uint n = 1u; n <= count; ++n) {
        const float nfloat         = float(n);
        const float rand_a         = float(pcg_hash(seed ^ (n * 0x85ebca6bu))) / 4294967296.0;
        const float rand_b         = float(pcg_hash(seed ^ (n * 0x9e3779b9u))) / 4294967296.0;
        const float rand_c         = float(pcg_hash(seed ^ (n * 0xc2b2ae35u))) / 4294967296.0;
        const float k              = 3.0 + float(uint(8.0 * rand_b));
        const uint  record         = base + 4u + (n - 1u) * 6u;
        pad_constants[record]      = nfloat;
        pad_constants[record + 1u] = two_pi * rand_a;
        pad_constants[record + 2u] = k;
        pad_constants[record + 3u] = rand_c;
        pad_constants[record + 4u] = clamped_duty * 0.02 * nfloat * (phase_step * float(sampling_rate)) *
                                     float(pad_time_wrap) / (two_pi * k * float(sampling_rate));
        const float amp            = 1.0 / pow(nfloat, tilt_exp);
        pad_constants[record + 5u] = amp;
        norm += amp;
    }
    pad_constants[base + 3u] = max(norm, 1e-9);
}

float pad_oscillator(float phase, float pad_time, uint slot)
{
    const uint base  = slot * 148u;
    const uint count = uint(pad_constants[base + 2u]);
    float      value = 0.0;
    for (uint n = 0u; n < count; ++n) {
        const uint  record = base + 4u + n * 6u;
        const float wander = pad_constants[record + 4u] *
                             sin(two_pi * (pad_constants[record + 2u] * pad_time + pad_constants[record + 3u]));
        value += pad_constants[record + 5u] *
                 sin(two_pi * (pad_constants[record] * phase + wander) + pad_constants[record + 1u]);
    }
    return value / pad_constants[base + 3u];
}

// Band-limited step residual (PolyBLEP).  t is the phase in [0, 1) and dt the phase
// step per sample.  Add (jump / 2) * poly_blep(phase - discontinuity) to the naive
// wave, so a step of height jump at phase 0 does not alias.
float poly_blep(float t, float dt)
{
    float value = 0.0;
    if (t < dt) {
        t /= dt;
        value = t + t - t * t - 1.0;
    }
    else if (t > 1.0 - dt) {
        t     = (t - 1.0) / dt;
        value = t * t + t + t + 1.0;
    }

    return value;
}

// Integral of poly_blep for a change of slope.
float poly_blamp(float t, float dt)
{
    float value = 0.0;
    if (t < dt || t > 1.0 - dt) {
        t     = t < dt ? 1.0 - t / dt : 1.0 + (t - 1.0) / dt;
        value = t * t * t / 3.0;
    }

    return value;
}

float oscillator(uint  osc_type,
                 float phase,
                 float duty,
                 float tilt,
                 uint  noise_index,
                 uint  note_seed,
                 float pad_time,
                 float phase_step,
                 uint  slot)
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
        const float kink_dt    = pad_constants[slot * 148u];
        const float kink_half  = pad_constants[slot * 148u + 1u];
        const float correction = poly_blamp(phase, kink_dt) - poly_blamp(fract(phase - duty), kink_dt);

        return value - 1.0 + kink_half * kink_dt * correction;
    }
    else if (osc_type == pulse_wave) {
        phase = fract(phase);

        const float value = (phase < duty) ? 1.0 : -1.0;

        return value + poly_blep(phase, step_dt) - poly_blep(fract(phase - duty), step_dt);
    }
    else if (osc_type == pad_wave) {
        return pad_oscillator(phase, pad_time, slot);
    }
    else { // noise_wave
        const uint uvalue = pcg_hash(noise_index);
        return (float(uvalue & 0xFFFFu) / 32767.5) - 1.0;
    }
}

const uint   string_ring_size           = 2034;
const uint   string_header_words        = 14;
const float  string_min_freq            = 22.0;
const float  string_dispersion_strength = 0.05863;
shared float string_block_out[256];
#define string_params string_block_out
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
// 5-6 seed tag, 7 dispersion section state, 8-9 chain token (two-word integer),
// 10-13 unused.
float string_reduce(uint count, bool energy)
{
    float      total = 0.0;
    const uint lane  = gl_LocalInvocationID.x;
    for (uint base = 0u; base < count; base += 256u) {
        for (uint j = lane; j < 256u; j += work_group_size) {
            float v = 0.0;
            if (base + j < count) {
                v = string_burst[base + j];
            }
            string_block_out[j] = energy ? v * v : v;
        }
        barrier();
        for (uint stride = 128u; stride != 0u; stride >>= 1u) {
            for (uint j = lane; j < stride; j += work_group_size) {
                string_block_out[j] += string_block_out[j + stride];
            }
            barrier();
        }
        total += string_block_out[0];
        barrier();
    }
    return total;
}

float string_burst_sample(uint count, float age, float loop_length)
{
    const float position = fract(age / loop_length) * float(count);
    const uint  first    = uint(floor(position));
    const uint  next     = first + 1u == count ? 0u : first + 1u;
    return string_burst[first] + (string_burst[next] - string_burst[first]) * (position - float(first));
}

const uint shape_kernel_size = 103u;
const uint shape_drive_max   = 442u;
const uint shape_tail_size   = 64u;
// Pending header words 0 and 1 hold the read cursor and end.  Samples start at word 4.
// Consume each sample once.  Add restrikes at the cursor and drop samples past capacity.
const uint shape_source_capacity = 4096u;
// The drive occupies the unused suffix while the excitation occupies the first 2003 floats.
const uint shape_drive_offset = 2003u;
#define shape_kernel       string_block_out
#define shape_drive(index) shared_region[shape_drive_offset + (index)]

float shape_power(float coefficient, uint exponent)
{
    return exponent == 0u
               ? 1.0
               : pow(abs(coefficient), float(exponent)) * (coefficient < 0.0 && (exponent & 1u) != 0u ? -1.0 : 1.0);
}

float shape_band_tap(uint tap, uint span, float low, float high)
{
    const float center = float(tap) - float(span - 1u) * 0.5;
    const float window = 0.42 - 0.5 * cos(two_pi * float(tap) / float(span - 1u)) +
                         0.08 * cos(2.0 * two_pi * float(tap) / float(span - 1u));
    const float value =
        center == 0.0
            ? 2.0 * (low - high) / float(sampling_rate)
            : (sin(two_pi * low * center / float(sampling_rate)) - sin(two_pi * high * center / float(sampling_rate))) /
                  (0.5 * two_pi * center);

    return value * window;
}

// Keep source filtering parallel without changing the legacy pluck arithmetic.
float string_excitation(uint count, uint seed, float brightness, bool shaped, bool harmonic, bool measure_energy)
{
    const uint lane             = gl_LocalInvocationID.x;
    const uint samples_per_lane = 8u;
    float      scan_values[samples_per_lane];
    const uint harmonic_count = (count - 1u) / 2u;

    for (uint index = lane; index < count; index += work_group_size) {
        float value = float(pcg_hash(seed + index) & 0xFFFFu) / 32767.5 - 1.0;
        if (harmonic) {
            value = 0.0;
            for (uint harmonic_index = 1u; harmonic_index <= harmonic_count; ++harmonic_index) {
                const float harmonic_float = float(harmonic_index);
                const float amplitude = harmonic_index % 24u == 0u
                                            ? 0.0
                                            : sin(0.5 * two_pi * harmonic_float / 24.0) / pow(harmonic_float, 1.85);
                const float phase     = two_pi * float(pcg_hash(harmonic_index * 0x85ebca6bu)) / 4294967296.0;
                value += amplitude * cos(two_pi * harmonic_float * float(index) / float(count) + phase);
            }
        }

        string_burst[index] = value;
    }
    barrier();

    const bool filtered        = shaped || brightness < 1.0;
    const uint reduction_steps = measure_energy ? 5u : filtered ? 4u : 1u;
    float      energy_in       = 0.0;
    float      tone_sum        = 0.0;
    float      noise_energy    = 0.0;

    // One reduction call keeps the tree and accumulation order equal for every excitation pass.
    for (uint reduction_step = 0u; reduction_step < reduction_steps; ++reduction_step) {
        const float total = string_reduce(count, (reduction_step & 1u) != 0u || reduction_step == 4u);
        if (reduction_step == 0u) {
            const float mean = total / float(count);
            for (uint index = lane; index < count; index += work_group_size) {
                string_burst[index] -= mean;
            }
            barrier();
        }
        else if (reduction_step == 1u) {
            energy_in              = total;
            const float tone_coeff = shaped ? 0.95 : 0.95 * (1.0 - clamp(brightness, 0.0, 1.0));
            if (shaped) {
                for (uint index = lane; index < count; index += work_group_size) {
                    string_burst[index] *= 1.0 - 0.95;
                }

                barrier();
                for (uint stride = 1u; stride < count; stride <<= 1u) {
                    const float coefficient = pow(0.95, float(stride));
                    for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
                        const uint index = lane + lane_sample * work_group_size;
                        if (index < count) {
                            scan_values[lane_sample] = string_burst[index];
                            if (index >= stride) {
                                scan_values[lane_sample] += coefficient * string_burst[index - stride];
                            }
                        }
                    }

                    barrier();
                    for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
                        const uint index = lane + lane_sample * work_group_size;
                        if (index < count) {
                            string_burst[index] = scan_values[lane_sample];
                        }
                    }

                    barrier();
                }

                const float initial_state = string_burst[count - 1u] / (1.0 - pow(0.95, float(count)));
                for (uint index = lane; index < count; index += work_group_size) {
                    scan_values[index / work_group_size] =
                        string_burst[index] + pow(0.95, float(index + 1u)) * initial_state;
                }

                barrier();
                for (uint index = lane; index < count; index += work_group_size) {
                    string_burst[index] = scan_values[index / work_group_size];
                }

                barrier();
            }
            else {
                if (lane == 0u) {
                    float tone_state = 0.0;
                    for (uint index = 0u; index < count; ++index) {
                        tone_state = (1.0 - tone_coeff) * string_burst[index] + tone_coeff * tone_state;
                    }
                    tone_state /= 1.0 - pow(tone_coeff, float(count));

                    for (uint index = 0u; index < count; ++index) {
                        tone_state          = (1.0 - tone_coeff) * string_burst[index] + tone_coeff * tone_state;
                        string_burst[index] = tone_state;
                    }
                }
                barrier();
            }
        }
        else if (reduction_step == 2u) {
            tone_sum = total;
        }
        else if (reduction_step == 3u) {
            const float tone_mean = tone_sum / float(count);
            const float tone_var  = total - tone_sum * tone_mean;
            const float tone_gain = tone_var > 0.0 ? sqrt(energy_in / tone_var) : 1.0;
            for (uint index = lane; index < count; index += work_group_size) {
                string_burst[index] = (string_burst[index] - tone_mean) * tone_gain;
            }
            barrier();
        }
        else {
            noise_energy = total;
        }
    }

    return noise_energy;
}

void string_generate_source(uint  source_offs,
                            uint  ring_offs,
                            uint  delay_whole,
                            float loop_length,
                            float damping_coeff,
                            float allpass_coeff,
                            float loop_gain,
                            bool  disp_active,
                            float disp_coeff,
                            bool  additive,
                            float source_gain,
                            float contact_ms,
                            float hammer,
                            uint  note_seed,
                            float brightness)
{
    const uint  lane               = gl_LocalInvocationID.x;
    const float hammer_noise_mix   = 0.83;
    const float contact_period_cap = 0.35;
    const float treble_start_hz    = 523.251;
    const float treble_full_hz     = 1046.502;
    const float f0                 = float(sampling_rate) / loop_length;
    const float user_contact       = contact_ms * 0.001 * float(sampling_rate);
    const float treble_weight      = smoothstep(treble_start_hz, treble_full_hz, f0);

    // Short contact keeps the treble drive below one string period.
    const float contact = mix(user_contact, min(user_contact, contact_period_cap * loop_length), treble_weight);
    const uint  shape_drive_size    = min(uint(ceil(contact)), shape_drive_max);
    const uint  shape_filtered_size = shape_drive_size + shape_tail_size;
    const uint  initial_pos         = additive ? floatBitsToUint(data[ring_offs - string_header_words]) : delay_whole;
    const uint  pending_pos         = additive ? uint(data[source_offs]) : 0u;

    float      noise_energy = 0.0;
    const uint pass_end     = hammer > 0.0 ? 3u : 1u;
    for (uint excitation_pass = 0u; excitation_pass < pass_end; ++excitation_pass) {
        if (excitation_pass == 0u && hammer == 1.0) {
            for (uint index = lane; index < delay_whole; index += work_group_size) {
                string_burst[index] = 0.0;
            }
        }
        else {
            const float energy = string_excitation(delay_whole,
                                                   excitation_pass == 0u ? note_seed : 0u,
                                                   brightness,
                                                   excitation_pass != 0u,
                                                   excitation_pass == 1u,
                                                   excitation_pass == 2u);
            if (excitation_pass == 2u) {
                noise_energy = energy;
            }
        }

        if (excitation_pass == 0u) {
            if (hammer > 0.0 && hammer < 1.0) {
                for (uint index = lane; index < delay_whole; index += work_group_size) {
                    string_burst[index] *= 1.0 - hammer;
                }
            }

            if (! additive && hammer > 0.0) {
                for (uint index = lane; index < string_ring_size; index += work_group_size) {
                    data[ring_offs + index] = 0.0;
                }
            }

            for (uint index = lane; index < delay_whole; index += work_group_size) {
                if (additive) {
                    data[ring_offs + (initial_pos + index + string_ring_size - delay_whole) % string_ring_size] +=
                        string_burst[index];
                }
                else {
                    data[ring_offs + index] = string_burst[index];
                }
            }
            memoryBarrierBuffer();
            barrier();
        }
        else if (excitation_pass == 1u) {
            for (uint source_sample = lane; source_sample < shape_filtered_size; source_sample += work_group_size) {
                const float age = float(source_sample);
                shape_drive(source_sample) =
                    source_sample < shape_drive_size
                        ? (0.5 - 0.5 * cos(two_pi * age / contact)) * string_burst_sample(delay_whole, age, loop_length)
                        : 0.0;
            }

            barrier();
        }
        else {
            // Shorten only the hammer noise input.  Keep the loop cancellation and spectral filters unchanged.
            const float noise_span = mix(loop_length, min(loop_length, contact), treble_weight);

            for (uint index = lane; index < delay_whole; index += work_group_size) {
                const float noise_weight =
                    float(index) < noise_span
                        ? mix(1.0, 0.5 + 0.5 * cos(0.5 * two_pi * float(index) / noise_span), treble_weight)
                        : 0.0;

                string_burst[index] *= (1.0 - hammer_noise_mix) * noise_weight;
            }
        }
    }

    if (hammer == 0.0) {
        return;
    }

    for (uint tap = lane; tap < 144u; tap += work_group_size) {
        const bool  band0 = tap < shape_kernel_size;
        const uint  span  = band0 ? shape_kernel_size : 41u;
        const uint  index = band0 ? tap : tap - shape_kernel_size;
        const float low   = floor(f0 * (band0 ? 0.25 : pow(2.0, 0.5)) + 0.5);
        const float high  = floor(f0 * (band0 ? 5.7 : pow(2.0, 2.95)) + 0.5);
        shape_kernel[tap] = shape_band_tap(index, span, low, high);
    }

    barrier();

    for (uint tap = lane; tap < 144u; tap += work_group_size) {
        const bool  band0     = tap < shape_kernel_size;
        const uint  span      = band0 ? shape_kernel_size : 41u;
        const float low       = floor(f0 * (band0 ? 0.25 : pow(2.0, 0.5)) + 0.5);
        const float high      = floor(f0 * (band0 ? 5.7 : pow(2.0, 2.95)) + 0.5);
        float       real_part = 0.0;
        float       imag_part = 0.0;

        for (uint index = 0u; index < span; ++index) {
            const float angle =
                two_pi * (low + high) * 0.5 * (float(index) - float(span - 1u) * 0.5) / float(sampling_rate);
            const float coefficient = shape_band_tap(index, span, low, high);
            real_part += coefficient * cos(angle);
            imag_part += coefficient * sin(angle);
        }

        shape_kernel[tap] *= (band0 ? 0.945 : 0.62) / max(length(vec2(real_part, imag_part)), 1e-12);
    }

    float drive_energy = 0.0;
    for (uint source_sample = 0u; source_sample < shape_drive_size; ++source_sample) {
        drive_energy += shape_drive(source_sample) * shape_drive(source_sample);
    }

    // Finish every lane's energy reads before any lane scales shape_drive.
    barrier();

    const float drive_gain       = min(16.5609913, sqrt(noise_energy / max(drive_energy, 1e-20))) * hammer_noise_mix;
    const uint  samples_per_lane = 2u;
    float       drive_samples[samples_per_lane];
    float       filtered_samples[samples_per_lane];
    float       stage_samples[samples_per_lane];
    for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
        const uint source_sample = lane + lane_sample * work_group_size;
        if (source_sample < shape_filtered_size) {
            drive_samples[lane_sample] = shape_drive(source_sample) * drive_gain;
            shape_drive(source_sample) = drive_samples[lane_sample];
        }
    }
    barrier();

    // Each stage finishes cross-lane reads before the suffix receives the next stage.
    for (uint filter_stage = 0u; filter_stage < 3u; ++filter_stage) {
        for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
            const uint source_sample = lane + lane_sample * work_group_size;
            if (source_sample < shape_filtered_size) {
                float value = 0.0;
                if (filter_stage == 0u) {
                    for (uint index = 0u; index <= source_sample; ++index) {
                        value += shape_drive(index) * (1.0 - damping_coeff) *
                                 pow(damping_coeff, float(source_sample - index));
                    }
                }
                else if (filter_stage == 1u) {
                    value = allpass_coeff * shape_drive(source_sample);
                    for (uint index = 0u; index < source_sample; ++index) {
                        value += shape_drive(index) * (1.0 - allpass_coeff * allpass_coeff) *
                                 shape_power(-allpass_coeff, source_sample - index - 1u);
                    }
                }
                else {
                    value = shape_drive(source_sample);
                    if (disp_active) {
                        value *= disp_coeff;
                        for (uint index = 0u; index < source_sample; ++index) {
                            value += shape_drive(index) * (1.0 - disp_coeff * disp_coeff) *
                                     shape_power(-disp_coeff, source_sample - index - 1u);
                        }
                    }
                    value *= loop_gain;
                }

                stage_samples[lane_sample] = value;
            }
        }
        barrier();

        for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
            const uint source_sample = lane + lane_sample * work_group_size;
            if (source_sample < shape_filtered_size) {
                if (filter_stage == 2u) {
                    filtered_samples[lane_sample] = stage_samples[lane_sample];
                }
                else {
                    shape_drive(source_sample) = stage_samples[lane_sample];
                }
            }
        }
        barrier();
    }

    const uint numerator_size = delay_whole + shape_filtered_size;
    // The drive and filtered samples now stay in registers while the numerator reuses their suffix.
    for (uint source_sample = lane; source_sample < numerator_size; source_sample += work_group_size) {
        if (source_sample >= delay_whole) {
            string_burst[source_sample] = 0.0;
        }
    }
    barrier();

    for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
        const uint source_sample = lane + lane_sample * work_group_size;
        if (source_sample < shape_drive_size) {
            string_burst[source_sample] += drive_samples[lane_sample];
        }
    }
    barrier();

    for (uint lane_sample = 0u; lane_sample < samples_per_lane; ++lane_sample) {
        const uint source_sample = lane + lane_sample * work_group_size;
        if (source_sample < shape_filtered_size) {
            string_burst[delay_whole + source_sample] += drive_samples[lane_sample] - filtered_samples[lane_sample];
        }
    }
    barrier();

    // The fixed gain curve offsets the shorter drive's treble energy loss.
    const float treble_gain_base    = 0.22;
    const float treble_gain_slope   = 0.64;
    const float treble_gain_octaves = 0.75;

    const float noise_gain_base  = 0.08;
    const float noise_gain_slope = -0.17;

    const float treble_octaves = clamp(log2(f0 / treble_full_hz), 0.0, treble_gain_octaves);
    const float treble_gain    = exp2(treble_weight * (treble_gain_base + treble_gain_slope * treble_octaves +
                                                       noise_gain_base + noise_gain_slope * treble_octaves));

    // Drain the phase filter so truncation does not add an excitation edge.
    const uint phase_tail_size = 128u;
    const uint source_size     = numerator_size + shape_kernel_size - 1u + (treble_weight > 0.0 ? phase_tail_size : 0u);
    const uint source_samples_per_lane = 11u;
    float      source_values[source_samples_per_lane];

    for (uint source_sample = lane; source_sample < source_size; source_sample += work_group_size) {
        float value = 0.0;
        for (uint tap = 0u; tap < shape_kernel_size; ++tap) {
            if (source_sample >= tap && source_sample - tap < numerator_size) {
                const float coefficient = shape_kernel[tap] + (tap < 41u ? shape_kernel[shape_kernel_size + tap] : 0.0);
                value += coefficient * string_burst[source_sample - tap];
            }
        }

        value *= source_gain * treble_gain;
        source_values[source_sample / work_group_size] = value;
    }

    barrier();

    if (treble_weight > 0.0) {
        for (uint source_sample = lane; source_sample < source_size; source_sample += work_group_size) {
            string_burst[source_sample] = source_values[source_sample / work_group_size];
        }

        barrier();

        if (lane == 0u) {
            // Phase dispersion reduces the drive crest without removing upper harmonics.
            const uint  phase_stages      = 2u;
            const uint  phase_delay       = 4u;
            const float phase_coefficient = mix(1.0, 0.6, treble_weight);

            for (uint stage = 0u; stage < phase_stages; ++stage) {
                float phase_state[phase_delay];
                for (uint index = 0u; index < phase_delay; ++index) {
                    phase_state[index] = 0.0;
                }

                for (uint source_sample = 0u; source_sample < source_size; ++source_sample) {
                    const uint  state_index     = source_sample % phase_delay;
                    const float phase_input     = string_burst[source_sample];
                    const float phase_output    = phase_coefficient * phase_input + phase_state[state_index];
                    phase_state[state_index]    = phase_input - phase_coefficient * phase_output;
                    string_burst[source_sample] = phase_output;
                }
            }
        }

        barrier();
    }

    for (uint source_sample = lane; source_sample < source_size; source_sample += work_group_size) {
        const float value =
            treble_weight > 0.0 ? string_burst[source_sample] : source_values[source_sample / work_group_size];

        if (source_sample < delay_whole) {
            const uint ring_index = (initial_pos + source_sample + string_ring_size - delay_whole) % string_ring_size;
            data[ring_offs + ring_index] += value;
        }
        else {
            const uint pending_index = pending_pos + source_sample - delay_whole;
            if (pending_index < shape_source_capacity) {
                data[source_offs + 4u + pending_index] += value;
            }
        }
    }

    if (lane == 0u) {
        data[source_offs]      = float(pending_pos);
        data[source_offs + 1u] = float(
            min(shape_source_capacity, max(uint(data[source_offs + 1u]), pending_pos + source_size - delay_whole)));
    }

    memoryBarrierBuffer();
    barrier();
}

void string_pass(uint  base,
                 float f0,
                 float damping,
                 float release_damping,
                 float brightness,
                 float dispersion,
                 bool  release,
                 uint  note_seed,
                 uint  chain_token,
                 float hammer,
                 float contact_ms,
                 float hammer_gain)
{
    const uint  lane        = gl_LocalInvocationID.x;
    const uint  ring_offs   = base + string_header_words;
    const float sample_rate = float(sampling_rate);
    hammer                  = hammer >= 0.0 ? (hammer <= 1.0 ? hammer : 1.0) : 0.0;
    contact_ms              = contact_ms >= 0.5 ? (contact_ms <= 10.0 ? contact_ms : 10.0) : 0.5;

    if (lane == 0u) {
        string_write_pos   = 0u;
        string_loop_length = 0.0;
    }
    if (! (f0 >= string_min_freq)) {
        // Too low to tune: silence, state untouched.
        string_block_out[lane] = 0.0;
        return;
    }

    float loop_length;
    float string_damping_coeff;
    float string_loop_gain;
    float disp_coeff;
    float string_allpass_coeff;
    bool  disp_active;
    uint  delay_whole;
    if (lane != 0u) {
        // Other lanes join the scalar publication below.
    }
    else {
        loop_length               = sample_rate / f0;
        loop_length               = loop_length < 3.0 ? 3.0 : loop_length;
        string_damping_coeff      = 0.10 + 0.30 * damping;
        const float lowpass_delay = string_damping_coeff / (1.0 - string_damping_coeff);
        const float decay_t60     = 20.0 * pow(0.25 / 20.0, damping);
        const float omega         = 6.2831853 * f0 / sample_rate;
        const float lowpass_mag   = (1.0 - string_damping_coeff) / sqrt(1.0 - 2.0 * string_damping_coeff * cos(omega) +
                                                                        string_damping_coeff * string_damping_coeff);
        const float loop_gain_raw = pow(10.0, -3.0 * loop_length / (sample_rate * decay_t60)) / lowpass_mag;
        string_loop_gain          = loop_gain_raw < 0.9999 ? loop_gain_raw : 0.9999;

        // Dispersion block-rate setup, gated off entirely at 0 (a zero-coefficient section still delays one sample).
        // The cascade is a single allpass section (the section count is frozen at one).  It falls back to no section
        // at all when it would leave less than 3 samples of loop delay.
        // Dispersion is modulatable: a modulated value changes the timbre every block while the retune below
        // (loop_length minus the section delay) holds the fundamental.
        disp_coeff       = 0.0;
        float disp_delay = 0.0;
        disp_active      = false;
        if (dispersion > 0.0) {
            const float disp_q        = string_dispersion_strength * dispersion / (omega * omega * omega);
            const float disp_r        = string_dispersion_ratio(disp_q);
            const float section_coeff = (1.0 - disp_r) / (1.0 + disp_r);
            const float section_delay = string_allpass_phase_delay(section_coeff, omega);
            if (loop_length - lowpass_delay - section_delay >= 3.0) {
                disp_coeff  = section_coeff;
                disp_delay  = section_delay;
                disp_active = true;
            }
        }
        const float delay      = loop_length - lowpass_delay - disp_delay;
        delay_whole            = uint(floor(delay - 1.0));
        const float delay_frac = delay - float(delay_whole);
        string_allpass_coeff   = (1.0 - delay_frac) / (1.0 + delay_frac);

        string_params[0] = loop_length;
        string_params[1] = string_damping_coeff;
        string_params[2] = string_loop_gain;
        string_params[3] = disp_coeff;
        string_params[4] = disp_active ? 1.0 : 0.0;
        string_params[5] = float(delay_whole);
        string_params[6] = string_allpass_coeff;
    }
    barrier();
    loop_length          = string_params[0];
    string_damping_coeff = string_params[1];
    string_loop_gain     = string_params[2];
    disp_coeff           = string_params[3];
    disp_active          = string_params[4] != 0.0;
    delay_whole          = uint(string_params[5]);
    string_allpass_coeff = string_params[6];
    barrier();

    // Init runs on the first in-range pass of a note.  The seed tag (header words 5 and 6) records
    // which note seed the ring was filled for, so a note that starts below the pitch floor still
    // gets a fresh pluck on its first tunable pass.  The low half is stored +1 so zeroed memory
    // never matches, and NaN compares false, so corrupt state re-initialises instead of replaying.
    const float string_seed_tag_lo = float((note_seed & 0xFFFFu) + 1u);
    const float string_seed_tag_hi = float(note_seed >> 16);
    const uint  source_offs        = base + string_header_words + string_ring_size;
    const float header_0           = data[base];
    const float header_1           = data[base + 1u];
    const float header_2           = data[base + 2u];
    const float header_3           = data[base + 3u];
    const float header_5           = data[base + 5u];
    const float header_6           = data[base + 6u];
    const float header_7           = data[base + 7u];
    const float header_8           = data[base + 8u];
    const float header_9           = data[base + 9u];

    const float pending_cursor = data[source_offs];
    const float pending_size   = data[source_offs + 1u];
    const float cursor_floor   = floor(pending_cursor);
    const float size_floor     = floor(pending_size);
    const bool  valid_state    = floatBitsToUint(header_0) < string_ring_size &&
                                 (floatBitsToUint(header_1) & 0x7F800000u) != 0x7F800000u &&
                                 (floatBitsToUint(header_2) & 0x7F800000u) != 0x7F800000u &&
                                 (floatBitsToUint(header_3) & 0x7F800000u) != 0x7F800000u &&
                                 (floatBitsToUint(header_7) & 0x7F800000u) != 0x7F800000u && pending_cursor >= 0.0 &&
                                 pending_cursor <= pending_size && pending_size <= float(shape_source_capacity) &&
                                 pending_cursor == cursor_floor && pending_size == size_floor;
    const bool  init           = ! valid_state || ! (header_5 == string_seed_tag_lo && header_6 == string_seed_tag_hi);

    // Chain token (header words 8 and 9), same two-word integer split as the seed tag.  A note which
    // continues the chain of the string already in the region adds its burst to the ringing ring; any
    // other note replaces it.  Zeroed or stale memory never matches, and NaN compares false.
    const float string_chain_lo = float((chain_token & 0xFFFFu) + 1u);
    const float string_chain_hi = float(chain_token >> 16);
    const bool  additive        = valid_state && init && header_8 == string_chain_lo && header_9 == string_chain_hi;
    // Finish header decisions before recovery clears words read by other lanes.
    memoryBarrierBuffer();
    barrier();

    uint  write_pos;
    float string_prev_lowpass;
    float string_prev_allpass;
    float string_release_gain;
    float disp_state;
    if (init) {
        if (! additive) {
            for (uint index = lane; index < shape_source_capacity; index += work_group_size) {
                data[source_offs + 4u + index] = 0.0;
            }
            if (lane == 0u) {
                data[source_offs]      = 0.0;
                data[source_offs + 1u] = 0.0;
                data[source_offs + 2u] = 0.0;
                data[source_offs + 3u] = 0.0;
            }
            memoryBarrierBuffer();
        }

        string_generate_source(source_offs,
                               ring_offs,
                               delay_whole,
                               loop_length,
                               string_damping_coeff,
                               string_allpass_coeff,
                               string_loop_gain,
                               disp_active,
                               disp_coeff,
                               additive,
                               hammer_gain * hammer,
                               contact_ms,
                               hammer,
                               note_seed,
                               brightness);

        if (lane == 0u) {
            data[base + 5u] = string_seed_tag_lo;
            data[base + 6u] = string_seed_tag_hi;
            data[base + 8u] = string_chain_lo;
            data[base + 9u] = string_chain_hi;
            if (additive) {
                // Add the burst onto the live window [write_pos - delay_whole, write_pos); the filter and
                // dispersion state keep running, only the release gain restarts.
                write_pos           = floatBitsToUint(header_0);
                string_prev_lowpass = header_1;
                string_prev_allpass = header_2;
                disp_state          = header_7;
            }
            else {
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
        write_pos           = floatBitsToUint(header_0);
        string_prev_lowpass = header_1;
        string_prev_allpass = header_2;
        string_release_gain = header_3;
        disp_state          = header_7;
    }

    barrier();
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
        float      loop_output  = cascade_out * (string_loop_gain * string_release_gain);
        const uint pending_age  = uint(data[source_offs]);
        const uint pending_size = uint(data[source_offs + 1u]);
        if (pending_size > 0u && pending_age < pending_size) {
            const float injection                = data[source_offs + 4u + pending_age];
            data[source_offs + 4u + pending_age] = 0.0;
            loop_output += injection;
            if (pending_age + 1u == pending_size) {
                data[source_offs]      = 0.0;
                data[source_offs + 1u] = 0.0;
            }
            else {
                data[source_offs] = float(pending_age + 1u);
            }
        }
        // Flush runaway or non-finite values to silence so a blown-up loop cannot poison the ring.
        loop_output                 = abs(loop_output) < 8.0 ? loop_output : 0.0;
        data[ring_offs + write_pos] = loop_output;
        string_prev_lowpass         = lowpass_out;
        string_prev_allpass         = allpass_out;
        write_pos                   = write_pos + 1u == string_ring_size ? 0u : write_pos + 1u;
    }

    data[base]      = uintBitsToFloat(write_pos);
    data[base + 1u] = string_prev_lowpass;
    data[base + 2u] = string_prev_allpass;
    data[base + 3u] = string_release_gain;
    data[base + 4u] = loop_length;
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

    const uint type1    = param.osc_pack & 0xFFu;
    const uint type2    = (param.osc_pack >> 8) & 0xFFu;
    const uint osc_mode = (param.osc_pack >> 16) & 0xFFu;
    const uint lane     = gl_LocalInvocationID.x;

    const bool is_string = type1 == string_wave;
    if (is_string && ! (type2 != no_wave && osc_mode == osc_mode_hard_sync)) {
        string_pass(param.string_state_offs,
                    param.phase_step * float(sampling_rate),
                    param.string_damping,
                    param.string_release_damping,
                    param.string_brightness,
                    param.string_dispersion,
                    ((param.osc_pack >> 24) & 1u) != 0u,
                    note_seed,
                    param.string_chain,
                    param.string_hammer,
                    param.string_hammer_contact,
                    param.string_hammer_gain);
        // barrier() alone does not publish the data[] ring writes.
        memoryBarrierBuffer();
        barrier();
    }

    if (lane == 0u) {
        const float slave_step = osc_mode == osc_mode_fm ? param.mod_phase_step
                                 : (osc_mode == osc_mode_hard_sync || osc_mode == osc_mode_ring_mod)
                                     ? param.mod_ratio * param.phase_step
                                     : param.phase_step;
        for (uint slot = 0u; slot < 2u; ++slot) {
            const float duty         = param.duty[slot];
            const float phase_step   = slot == 0u ? param.phase_step : slave_step;
            const uint  base         = slot * 148u;
            pad_constants[base]      = min(min(phase_step, 0.5), 0.5 * min(duty, 1.0 - duty));
            pad_constants[base + 1u] = 1.0 / (duty * (1.0 - duty));
            if ((slot == 0u ? type1 : type2) == pad_wave) {
                fill_pad_constants(slot,
                                   duty,
                                   param.pad_tilt[slot],
                                   slot == 0u ? note_seed : note_seed ^ noise_salt,
                                   phase_step);
            }
        }
    }
    barrier();

    float value           = 0.0;
    float secondary_value = 0.0;
    for (uint oscillator_pass = 0u; oscillator_pass < 2u; ++oscillator_pass) {
        const uint slot = 1u - oscillator_pass;
        if (slot == 1u && type2 == no_wave) {
            continue;
        }

        if (slot == 0u && type2 != no_wave && osc_mode == osc_mode_hard_sync) {
            value = secondary_value;
            continue;
        }

        float oscillator_phase = phase;
        float oscillator_step  = param.phase_step;
        uint  oscillator_noise = noise_index ^ noise_key;
        if (slot == 1u) {
            oscillator_noise ^= noise_salt;
            if (osc_mode == osc_mode_fm) {
                oscillator_phase = param.mod_phase + param.mod_phase_step * gl_LocalInvocationID.x;
                oscillator_step  = param.mod_phase_step;
            }
            else if (osc_mode == osc_mode_hard_sync || osc_mode == osc_mode_ring_mod) {
                oscillator_phase = fract(param.mod_ratio * fract(phase));
                oscillator_step  = param.mod_ratio * param.phase_step;
                if (osc_mode == osc_mode_ring_mod) {
                    oscillator_noise = noise_index ^ noise_salt;
                }
            }
        }
        else if (type2 != no_wave && osc_mode == osc_mode_fm) {
            oscillator_phase += param.fm_index * secondary_value;
        }

        float sample_value;
        if (slot == 0u && is_string) {
            sample_value = type2 != no_wave && osc_mode == osc_mode_fm
                               ? string_ring_read(param.string_state_offs + string_header_words, oscillator_phase)
                               : string_block_out[lane];
        }
        else {
            sample_value = oscillator(slot == 0u ? type1 : type2,
                                      oscillator_phase,
                                      param.duty[slot],
                                      param.pad_tilt[slot],
                                      oscillator_noise,
                                      slot == 0u ? note_seed : note_seed ^ noise_salt,
                                      pad_time,
                                      oscillator_step,
                                      slot);
        }

        if (slot == 1u) {
            secondary_value = sample_value;
        }
        else {
            value = sample_value;
            if (type2 != no_wave && osc_mode != osc_mode_fm) {
                value = mix(value,
                            osc_mode == osc_mode_ring_mod ? value * secondary_value : secondary_value,
                            param.osc_mix);
            }
        }
    }

    // Apply FIR filter (optional)
    if (param.taps_offs != 0) {
        barrier();

        // Read previously saved inputs
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            cached_input[tap] = data[param.fir_memory_offs + tap];
        }

        // Store generated sample values
        const uint base_pos    = num_taps - 1 + gl_LocalInvocationID.x;
        cached_input[base_pos] = value;

        barrier();

        // Save current inputs for next invocation
        for (uint tap = gl_LocalInvocationID.x; tap < num_taps - 1; tap += work_group_size) {
            data[param.fir_memory_offs + tap] = cached_input[work_group_size + tap];
        }

        // Preserve ascending accumulation with scalar head and tail.
        uint       tap = param.fir_window & 2047u;
        const uint hi  = min((param.fir_window >> 11u) & 2047u, num_taps);
        value          = tap == 0u && hi != 0u ? value * data[param.taps_offs] : 0.0;
        if (tap == 0u && hi != 0u) {
            ++tap;
        }
        for (; tap < hi && ((param.taps_offs + tap) & 3u) != 0u; ++tap) {
            value += data[param.taps_offs + tap] * cached_input[base_pos - tap];
        }
        for (; tap + 3u < hi; tap += 4u) {
            const vec4 coeff = coefficients[(param.taps_offs + tap) >> 2u];
            value += coeff.x * cached_input[base_pos - tap];
            value += coeff.y * cached_input[base_pos - tap - 1u];
            value += coeff.z * cached_input[base_pos - tap - 2u];
            value += coeff.w * cached_input[base_pos - tap - 3u];
        }
        for (; tap < hi; ++tap) {
            value += data[param.taps_offs + tap] * cached_input[base_pos - tap];
        }
    }

    // Output generated sample
    data[param.out_sound_offs + gl_LocalInvocationID.x] = value;
}
