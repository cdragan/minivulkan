// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(constant_id = 2) const uint num_taps = 1025;

const uint effect_none       = 0u;
const uint effect_distortion = 1u;
const uint effect_delay      = 2u;
const uint effect_chorus     = 3u;
const uint effect_reverb     = 4u;
const uint effect_compressor = 5u;
const uint effect_fir        = 6u;

struct EffectParams {
    uint  type;
    uint  sound_offs;   // float index into data[], interleaved stereo, read + write in place
    uint  state_offs;   // float index into data[] for persistent state
    uint  pad;          // keep params[] 16-byte aligned for std430
    float params[5];    // effect's tweakable values, uploaded every step
};

layout(binding = 0) buffer data_buf { float data[]; };

layout(binding = 1, std430) readonly buffer eff_param_buf { EffectParams effects[]; };

// Delay ring length in stereo frames; the host supplies it from
// Synth::effect_delay_max_samples.
layout(constant_id = 4) const uint delay_max = 1;

// Chorus ring length in stereo frames; the host supplies it from
// Synth::effect_chorus_max_samples.
layout(constant_id = 5) const uint chorus_max = 1;

// Output sampling rate in Hz; the host supplies it from Synth::rt_sampling_rate.
// Needed to turn an LFO rate in Hz into a per-sample phase increment.
layout(constant_id = 6) const uint sampling_rate = 1;

shared float samples_left[work_group_size];
shared float samples_right[work_group_size];

// tanh waveshaper with a dry/wet mix.  drive sets the overdrive amount.
vec2 apply_distortion(in EffectParams eff)
{
    const float drive      = eff.params[0];
    const float mix_amount = eff.params[1];
    const float in_left    = samples_left[gl_LocalInvocationID.x];
    const float in_right   = samples_right[gl_LocalInvocationID.x];

    return vec2(mix(in_left,  tanh(in_left  * drive), mix_amount),
                mix(in_right, tanh(in_right * drive), mix_amount));
}

// Feedback delay line.  Each lane reads a prior frame, mixes it in, and writes the
// dry input plus feedback back into the ring.
vec2 apply_delay(in EffectParams eff)
{
    const uint  base_pos   = uint(data[eff.state_offs]);
    const float feedback   = eff.params[1];
    const float mix_amount = eff.params[2];

    // Clamp delay so reads target a prior step's slot (no intra-step hazard) and
    // never alias the current write slot.  delay_max - work_group_size keeps every
    // lane's read frame below every lane's write frame.
    const uint delay_samples = uint(clamp(eff.params[0], float(work_group_size), float(delay_max - work_group_size)));

    const uint ring = eff.state_offs + 1u;

    // Adding delay_max keeps the subtraction in unsigned range (no underflow).
    const uint  read_frame    = (base_pos + gl_LocalInvocationID.x + delay_max - delay_samples) % delay_max;
    const float delayed_left  = data[ring + read_frame * 2];
    const float delayed_right = data[ring + read_frame * 2 + 1];

    const float in_left  = samples_left[gl_LocalInvocationID.x];
    const float in_right = samples_right[gl_LocalInvocationID.x];
    const vec2  result   = vec2(in_left  * (1.0 - mix_amount) + delayed_left  * mix_amount,
                                in_right * (1.0 - mix_amount) + delayed_right * mix_amount);

    const uint write_frame = (base_pos + gl_LocalInvocationID.x) % delay_max;
    data[ring + write_frame * 2]     = in_left  + delayed_left  * feedback;
    data[ring + write_frame * 2 + 1] = in_right + delayed_right * feedback;

    barrier();

    // Every lane has read the ring; advance the write position for the next step.
    if (gl_LocalInvocationID.x == 0u) {
        data[eff.state_offs] = float((base_pos + work_group_size) % delay_max);
    }

    return result;
}

// LFO-modulated short delay (chorus).  state[0] holds the LFO phase, state[1] the
// write position.
vec2 apply_chorus(in EffectParams eff)
{
    const float base_lfo_phase = data[eff.state_offs];
    const uint  base_pos       = uint(data[eff.state_offs + 1u]);

    const float rate_hz       = eff.params[0];
    const float depth_samples = eff.params[1];
    const float mix_amount    = eff.params[2];

    const float pi = 3.14159265358979;

    // Center delay the LFO swings the read offset around.  Derived from the sample
    // rate so it stays ~11 ms at any rate.
    const float chorus_base_delay_ms = 11.0;
    const float chorus_base_delay    = chorus_base_delay_ms * float(sampling_rate) / 1000.0;

    const float phase = base_lfo_phase + float(gl_LocalInvocationID.x) * rate_hz / float(sampling_rate);

    // Clamp like the delay branch so reads always target a prior frame and never
    // alias any lane's write slot; the +1 / -1.0 leave room for the fractional
    // neighbor frame0 + 1.
    const float read_offset = clamp(chorus_base_delay + depth_samples * (0.5 + 0.5 * sin(2.0 * pi * phase)),
                                    float(work_group_size) + 1.0,
                                    float(chorus_max - work_group_size) - 1.0);

    const uint ring = eff.state_offs + 2u;

    // Adding chorus_max keeps the read position non-negative before the modulo.
    const float read_pos = float(base_pos + gl_LocalInvocationID.x) - read_offset + float(chorus_max);
    const uint  frame0   = uint(read_pos) % chorus_max;
    const uint  frame1   = (frame0 + 1u) % chorus_max;
    const float frac     = fract(read_pos);

    const float delayed_left  = mix(data[ring + frame0 * 2],     data[ring + frame1 * 2],     frac);
    const float delayed_right = mix(data[ring + frame0 * 2 + 1], data[ring + frame1 * 2 + 1], frac);

    const float in_left  = samples_left[gl_LocalInvocationID.x];
    const float in_right = samples_right[gl_LocalInvocationID.x];
    const vec2  result   = vec2(in_left  * (1.0 - mix_amount) + delayed_left  * mix_amount,
                                in_right * (1.0 - mix_amount) + delayed_right * mix_amount);

    // Write the dry input into the ring (no feedback).
    const uint write_frame = (base_pos + gl_LocalInvocationID.x) % chorus_max;
    data[ring + write_frame * 2]     = in_left;
    data[ring + write_frame * 2 + 1] = in_right;

    barrier();

    // Every lane has read the ring; advance the LFO phase and write position.
    if (gl_LocalInvocationID.x == 0u) {
        data[eff.state_offs]      = fract(base_lfo_phase + float(work_group_size) * rate_hz / float(sampling_rate));
        data[eff.state_offs + 1u] = float((base_pos + work_group_size) % chorus_max);
    }

    return result;
}

// Freeverb: 8 parallel feedback combs (each with a one-pole damping lowpass) summed
// into 4 series allpasses.  Per-sample feedback forbids splitting a side across lanes,
// so lane 0 runs the whole left side and lane 1 the whole right side; other lanes idle.
vec2 apply_reverb(in EffectParams eff)
{
    const float room_size = eff.params[0];
    const float damping   = eff.params[1];
    const float wet       = eff.params[2];

    // Freeverb's published tuning constants.
    const float scale_room  = 0.28;
    const float offset_room = 0.7;
    const float scale_damp  = 0.4;
    const float fixed_gain  = 0.015;

    const float feedback         = room_size * scale_room + offset_room;
    const float damp1            = damping * scale_damp;
    const float damp2            = 1.0 - damp1;
    const float input_gain       = fixed_gain;
    const float allpass_feedback = 0.5;

    // Exact write position, bit-cast so it stays an integer across steps.
    const uint counter = floatBitsToUint(data[eff.state_offs]);

    if (gl_LocalInvocationID.x == 0u || gl_LocalInvocationID.x == 1u) {
        // Canonical Freeverb comb/allpass lengths at freeverb_base_rate, mirroring the
        // host's effect_reverb_comb_base/allpass_base (synth_modulation.h).  Scaled to
        // sampling_rate with the SAME integer division the host uses to size the state,
        // so these rings and the host allocation stay in lockstep.
        const uint freeverb_base_rate = 44100u;
        const uint comb_base[8]    = uint[8](1116u, 1188u, 1277u, 1356u, 1422u, 1491u, 1557u, 1617u);
        const uint allpass_base[4] = uint[4](556u, 441u, 341u, 225u);

        uint comb_len[8];
        uint comb_off[8];
        uint comb_sum = 0u;
        for (uint c = 0u; c < 8u; c++) {
            comb_len[c] = comb_base[c] * sampling_rate / freeverb_base_rate;
            comb_off[c] = comb_sum;
            comb_sum += comb_len[c];
        }

        uint allpass_len[4];
        uint allpass_off[4];
        uint allpass_sum = 0u;
        for (uint a = 0u; a < 4u; a++) {
            allpass_len[a] = allpass_base[a] * sampling_rate / freeverb_base_rate;
            allpass_off[a] = allpass_sum;
            allpass_sum += allpass_len[a];
        }

        // Per-side layout: comb rings, then one lowpass state per comb, then allpass rings.
        const uint filterstore    = comb_sum;
        const uint allpass_region = comb_sum + 8u;
        const uint side_stride    = comb_sum + 8u + allpass_sum;

        // Lane 0 owns the left side, lane 1 owns the right side; their device regions
        // are disjoint, so the writes never race.
        const uint side_base = eff.state_offs + 1u + gl_LocalInvocationID.x * side_stride;

        float fs[8];
        for (uint c = 0u; c < 8u; c++) {
            fs[c] = data[side_base + filterstore + c];
        }

        // Each input sample is read once (dry) and its slot is then dead, so the wet
        // result is written straight back into samples_left/samples_right in place; lanes
        // 0 and 1 own disjoint arrays.  The barrier below publishes these to every lane.
        for (uint i = 0u; i < work_group_size; i++) {
            const float dry = (gl_LocalInvocationID.x == 0u) ? samples_left[i] : samples_right[i];
            const float comb_in = dry * input_gain;

            float acc = 0.0;
            for (uint c = 0u; c < 8u; c++) {
                const uint cbase = side_base + comb_off[c];
                const uint p = (counter + i) % comb_len[c];
                const float y = data[cbase + p];
                fs[c] = y * damp2 + fs[c] * damp1;
                data[cbase + p] = comb_in + fs[c] * feedback;
                acc += y;
            }

            float x = acc;
            for (uint a = 0u; a < 4u; a++) {
                const uint abase = side_base + allpass_region + allpass_off[a];
                const uint p = (counter + i) % allpass_len[a];
                const float bufout = data[abase + p];
                data[abase + p] = x + bufout * allpass_feedback;
                x = bufout - x;  // Freeverb allpass: output = -input + bufout
            }

            const float w = dry * (1.0 - wet) + x * wet;
            if (gl_LocalInvocationID.x == 0u) {
                samples_left[i] = w;
            }
            else {
                samples_right[i] = w;
            }
        }

        for (uint c = 0u; c < 8u; c++) {
            data[side_base + filterstore + c] = fs[c];
        }
    }

    barrier();

    // Publish lane 0/1's in-place results to every lane, then advance the counter.
    if (gl_LocalInvocationID.x == 0u) {
        // state[0] is still the old value here, so recompute from it.
        data[eff.state_offs] = uintBitsToFloat(floatBitsToUint(data[eff.state_offs]) + work_group_size);
    }

    return vec2(samples_left[gl_LocalInvocationID.x], samples_right[gl_LocalInvocationID.x]);
}

// Peak-following dynamics compressor.  The per-sample envelope feeds back into the
// next sample's gain, so lane 0 runs the whole step serially; other lanes idle.
vec2 apply_compressor(in EffectParams eff)
{
    const float threshold = eff.params[0];
    const float ratio     = eff.params[1];
    const float attack    = eff.params[2];
    const float release   = eff.params[3];
    const float makeup    = eff.params[4];

    if (gl_LocalInvocationID.x == 0u) {
        float env = data[eff.state_offs];

        // (1 - 1/ratio) maps the over-threshold level through the ratio in the log domain.
        const float gain_exponent = 1.0 - 1.0 / ratio;

        for (uint i = 0u; i < work_group_size; i++) {
            const float level = max(abs(samples_left[i]), abs(samples_right[i]));

            // Fast attack when the level rises, slow release when it falls.
            env = mix(level, env, (level > env) ? attack : release);

            // threshold > 0 makes env > threshold imply env > 0, so the divide is safe;
            // below threshold the signal passes unchanged.
            float gain = 1.0;
            if (env > threshold) {
                gain = pow(threshold / env, gain_exponent);
            }

            samples_left[i]  *= gain * makeup;
            samples_right[i] *= gain * makeup;
        }

        // Only lane 0 touched env; persist it for the next step.
        data[eff.state_offs] = env;
    }

    barrier();

    // Publish lane 0's in-place results to every lane.
    return vec2(samples_left[gl_LocalInvocationID.x], samples_right[gl_LocalInvocationID.x]);
}

// Windowed-sinc convolution: out[n] = sum over k of coeff[k] * input[n - k].  State
// layout: coeff buffer [num_taps], a stereo input-history ring of num_taps - 1 frames,
// then a write-position counter.  The ring holds the most recent num_taps - 1 dry input
// samples per channel for cross-step continuity.
vec2 apply_fir(in EffectParams eff)
{
    const uint fir_ring     = eff.state_offs + num_taps;
    const uint fir_ring_len = num_taps - 1u;
    const uint fir_pos_offs = eff.state_offs + num_taps + 2u * fir_ring_len;
    const uint fir_base_pos = uint(data[fir_pos_offs]);

    float acc_left  = 0.0;
    float acc_right = 0.0;

    // Taps whose input[n - k] lies in this step: read from shared.
    for (uint tap = 0u; tap <= gl_LocalInvocationID.x; tap++) {
        const float coeff = data[eff.state_offs + tap];
        acc_left  += coeff * samples_left[gl_LocalInvocationID.x - tap];
        acc_right += coeff * samples_right[gl_LocalInvocationID.x - tap];
    }

    // Taps whose input[n - k] lies in a prior step: read from the history ring.
    for (uint tap = gl_LocalInvocationID.x + 1u; tap < num_taps; tap++) {
        const float coeff = data[eff.state_offs + tap];

        // Adding fir_ring_len keeps the frame non-negative before the modulo without
        // shifting the position (it is a whole number of ring wraps).
        const uint read_frame = (fir_base_pos + gl_LocalInvocationID.x + fir_ring_len - tap) % fir_ring_len;
        acc_left  += coeff * data[fir_ring + read_frame * 2];
        acc_right += coeff * data[fir_ring + read_frame * 2 + 1];
    }

    barrier();

    // Save input in history for next step
    const uint write_frame = (fir_base_pos + gl_LocalInvocationID.x) % fir_ring_len;
    data[fir_ring + write_frame * 2]     = samples_left[gl_LocalInvocationID.x];
    data[fir_ring + write_frame * 2 + 1] = samples_right[gl_LocalInvocationID.x];
    if (gl_LocalInvocationID.x == 0u) {
        data[fir_pos_offs] = float((fir_base_pos + work_group_size) % fir_ring_len);
    }

    return vec2(acc_left, acc_right);
}

void main()
{
    const EffectParams eff = effects[gl_WorkGroupID.x];

    // Load and deinterleave samples
    samples_left[gl_LocalInvocationID.x]  = data[eff.sound_offs + gl_LocalInvocationID.x * 2];
    samples_right[gl_LocalInvocationID.x] = data[eff.sound_offs + gl_LocalInvocationID.x * 2 + 1];

    barrier();

    vec2 result = vec2(samples_left[gl_LocalInvocationID.x], samples_right[gl_LocalInvocationID.x]);
    if (eff.type == effect_distortion) {
        result = apply_distortion(eff);
    }
    else if (eff.type == effect_delay) {
        result = apply_delay(eff);
    }
    else if (eff.type == effect_chorus) {
        result = apply_chorus(eff);
    }
    else if (eff.type == effect_reverb) {
        result = apply_reverb(eff);
    }
    else if (eff.type == effect_compressor) {
        result = apply_compressor(eff);
    }
    else if (eff.type == effect_fir) {
        result = apply_fir(eff);
    }

    data[eff.sound_offs + gl_LocalInvocationID.x * 2]     = result.x;
    data[eff.sound_offs + gl_LocalInvocationID.x * 2 + 1] = result.y;
}
