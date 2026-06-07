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

shared float block_left[work_group_size];
shared float block_right[work_group_size];

void main()
{
    const EffectParams eff = effects[gl_WorkGroupID.x];
    const uint s = gl_LocalInvocationID.x;

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
    // delay / chorus / reverb / compressor branches are added by later tasks.

    data[eff.sound_offs + s * 2]     = out_left;
    data[eff.sound_offs + s * 2 + 1] = out_right;
}
