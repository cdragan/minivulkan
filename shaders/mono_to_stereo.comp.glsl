// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

#extension GL_EXT_shader_explicit_arithmetic_types_int16: require

layout(push_constant) uniform push_constants {
    uint num_samples;
} push;

layout(set = 0, binding = 0) buffer input_data  { float   in_sound[];  };
layout(set = 0, binding = 1) buffer output_data { int16_t out_sound[]; };

layout(local_size_x = 1024) in;

void main()
{
    const uint delta = gl_WorkGroupSize.x * gl_NumWorkGroups.x;

    for (uint in_offs = gl_GlobalInvocationID.x; in_offs < push.num_samples; in_offs += delta) {

        const uint out_offs = in_offs * 2;

        const float   value   = clamp(in_sound[in_offs], -1.0, 1.0);
        const int16_t value16 = int16_t(value * 32767.0);

        out_sound[out_offs]     = value16;
        out_sound[out_offs + 1] = value16;
    }
}
