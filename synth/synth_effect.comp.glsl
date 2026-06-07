// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

const uint effect_none       = 0u;
const uint effect_distortion = 1u;
const uint effect_delay      = 2u;
const uint effect_chorus     = 3u;
const uint effect_reverb     = 4u;
const uint effect_compressor = 5u;

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

shared float block_left[work_group_size];
shared float block_right[work_group_size];

void main()
{
    const EffectParams eff = effects[gl_WorkGroupID.x];
    const uint s = gl_LocalInvocationID.x;

    // Read the write position before the barrier so every lane sees it before
    // lane 0 advances it later (delay and chorus both use it).  Chorus stores its
    // LFO phase at state_offs and the write position at state_offs + 1.
    uint base_pos = uint(data[eff.state_offs + (eff.type == effect_chorus ? 1u : 0u)]);
    float base_lfo_phase = data[eff.state_offs];

    // Load this instance's interleaved-stereo input block into shared memory so the
    // effect can read inputs while writing outputs back to the same offset in place.
    block_left[s]  = data[eff.sound_offs + s * 2];
    block_right[s] = data[eff.sound_offs + s * 2 + 1];
    barrier();

    float out_left  = block_left[s];
    float out_right = block_right[s];

    if (eff.type == effect_distortion) {
        // tanh waveshaper with a dry/wet mix.  drive sets the overdrive amount.
        const float drive = eff.params[0];
        const float mix_amount = eff.params[1];
        out_left  = mix(block_left[s],  tanh(block_left[s]  * drive), mix_amount);
        out_right = mix(block_right[s], tanh(block_right[s] * drive), mix_amount);
    }
    else if (eff.type == effect_delay) {
        const float feedback = eff.params[1];
        const float mix_amount = eff.params[2];

        // Clamp delay so reads target a prior block's slot (no intra-block hazard)
        // and never alias the current write slot.  Upper bound delay_max -
        // work_group_size keeps every lane's read frame below every lane's write frame.
        const uint delay_samples = uint(clamp(eff.params[0], float(work_group_size), float(delay_max - work_group_size)));

        const uint ring = eff.state_offs + 1u;

        // Adding delay_max keeps the subtraction in unsigned range (no underflow).
        const uint read_frame = (base_pos + s + delay_max - delay_samples) % delay_max;
        const float delayed_left  = data[ring + read_frame * 2];
        const float delayed_right = data[ring + read_frame * 2 + 1];

        out_left  = block_left[s]  * (1.0 - mix_amount) + delayed_left  * mix_amount;
        out_right = block_right[s] * (1.0 - mix_amount) + delayed_right * mix_amount;

        const uint write_frame = (base_pos + s) % delay_max;
        data[ring + write_frame * 2]     = block_left[s]  + delayed_left  * feedback;
        data[ring + write_frame * 2 + 1] = block_right[s] + delayed_right * feedback;
    }
    else if (eff.type == effect_chorus) {
        const float rate_hz      = eff.params[0];
        const float depth_samples = eff.params[1];
        const float mix_amount   = eff.params[2];

        const float pi = 3.14159265358979;

        // Center delay the LFO swings the read offset around.  Expressed as a time
        // and derived from the sample rate so it stays ~11 ms at any rate.
        const float chorus_base_delay_ms = 11.0;
        const float chorus_base_delay    = chorus_base_delay_ms * float(sampling_rate) / 1000.0;

        const float phase = base_lfo_phase + float(s) * rate_hz / float(sampling_rate);

        // Clamp like the delay branch so reads always target a prior frame and
        // never alias any lane's write slot.  The lower bound work_group_size + 1
        // keeps the fractional neighbor frame0 + 1 strictly below every lane's
        // write frame; the upper -1.0 leaves room for that same neighbor.
        const float read_offset = clamp(chorus_base_delay + depth_samples * (0.5 + 0.5 * sin(2.0 * pi * phase)),
                                        float(work_group_size) + 1.0,
                                        float(chorus_max - work_group_size) - 1.0);

        const uint ring = eff.state_offs + 2u;

        // Adding chorus_max keeps the read position non-negative before the modulo.
        const float read_pos = float(base_pos + s) - read_offset + float(chorus_max);
        const uint  frame0 = uint(read_pos) % chorus_max;
        const uint  frame1 = (frame0 + 1u) % chorus_max;
        const float frac   = fract(read_pos);

        const float delayed_left  = mix(data[ring + frame0 * 2],     data[ring + frame1 * 2],     frac);
        const float delayed_right = mix(data[ring + frame0 * 2 + 1], data[ring + frame1 * 2 + 1], frac);

        out_left  = block_left[s]  * (1.0 - mix_amount) + delayed_left  * mix_amount;
        out_right = block_right[s] * (1.0 - mix_amount) + delayed_right * mix_amount;

        // Write the dry input into the ring (no feedback).
        const uint write_frame = (base_pos + s) % chorus_max;
        data[ring + write_frame * 2]     = block_left[s];
        data[ring + write_frame * 2 + 1] = block_right[s];
    }
    // reverb / compressor branches are added by later tasks.

    // All ring reads/writes for this block are done; advance the write position.
    barrier();
    if (eff.type == effect_delay && s == 0u) {
        data[eff.state_offs] = float((base_pos + work_group_size) % delay_max);
    }
    else if (eff.type == effect_chorus && s == 0u) {
        data[eff.state_offs]      = fract(base_lfo_phase + float(work_group_size) * eff.params[0] / float(sampling_rate));
        data[eff.state_offs + 1u] = float((base_pos + work_group_size) % chorus_max);
    }

    data[eff.sound_offs + s * 2]     = out_left;
    data[eff.sound_offs + s * 2 + 1] = out_right;
}
