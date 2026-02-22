// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "transforms.glsl"

layout(location = 0) out float out_depth;

struct vertex_data {
    uint xy;
    uint z;
};

layout(binding = 4) readonly buffer object_vertices {
    vertex_data vertices[];
};

vec3 read_vertex(uint index)
{
    const vertex_data data = vertices[index];

    const int ix = int(data.xy << 16) >> 16;
    const int iy = int(data.xy) >> 16;
    const int iz = int(data.z << 16) >> 16;

    const float x = float(ix) / 32767.0;
    const float y = float(iy) / 32767.0;
    const float z = float(iz) / 32767.0;

    return vec3(x, y, z);
}

void main()
{
    const vec3 orig_vertex = read_vertex(gl_InstanceIndex);
    const vec4 view_pos    = vec4(orig_vertex, 1) * model_view;
    vec4       pos         = projection(view_pos.xyz);

    pos.xy += vec2(ivec2(gl_VertexIndex & 1, gl_VertexIndex >> 1) * 2 - 1) * pixel_dim * 3.0 * pos.w;

    gl_Position = pos;
    out_depth   = pos.z / pos.w;
}
