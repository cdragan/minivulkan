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

shared float block_left[work_group_size];
shared float block_right[work_group_size];

void main()
{
    const EffectParams eff = effects[gl_WorkGroupID.x];
    const uint s = gl_LocalInvocationID.x;

    // Read the delay write position before the barrier so every lane sees it
    // before lane 0 advances it later (only the delay branch uses it).
    uint base_pos = uint(data[eff.state_offs]);

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
    // chorus / reverb / compressor branches are added by later tasks.

    // All ring reads/writes for this block are done; advance the write position.
    barrier();
    if (eff.type == effect_delay && s == 0u) {
        data[eff.state_offs] = float((base_pos + work_group_size) % delay_max);
    }

    data[eff.sound_offs + s * 2]     = out_left;
    data[eff.sound_offs + s * 2 + 1] = out_right;
}
