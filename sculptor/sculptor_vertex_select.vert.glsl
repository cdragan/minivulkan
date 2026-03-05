// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "frame_data.glsl"
#include "transforms.glsl"

layout(location = 1) out flat uint out_vtx_idx;
layout(location = 2) out flat uint out_in_rect;

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

    out_vtx_idx = uint(gl_InstanceIndex);

    // Check if vertex center falls within selection rectangle (vertex selection mode only)
    out_in_rect = 0u;
    if ((frame_flags & FRAME_FLAG_SELECT_VERTICES) != 0u) {
        const vec2 ndc = pos.xy / pos.w;
        const vec2 vp  = vec2(ndc.x + 1.0, 1.0 - ndc.y) / pixel_dim;
        if (vp.x >= selection_rect_min.x && vp.x <= selection_rect_max.x &&
            vp.y >= selection_rect_min.y && vp.y <= selection_rect_max.y)
            out_in_rect = 1u;
    }

    pos.xy += vec2(ivec2(gl_VertexIndex & 1, gl_VertexIndex >> 1) * 2 - 1) * pixel_dim * 2.0 * pos.w;
    pos.z  *= 1.00195325;

    gl_Position = pos;
}
