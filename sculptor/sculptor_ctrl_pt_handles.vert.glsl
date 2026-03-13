// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "frame_data.glsl"
#include "transforms.glsl"

// Distance of the pixel in screen space from the non-corner vertex (0=non-corner vertex, 1=corner vertex)
layout(location = 0) out float out_pixel_dist;

layout(binding = 3) readonly buffer face_indices_buf {
    uint face_indices[];
};

struct vertex_data {
    uint xy;
    uint z;
};

layout(binding = 4) readonly buffer object_vertices {
    vertex_data vertices[];
};

const ivec2 line_vertices[12] = ivec2[12](
    ivec2( 1,  0),
    ivec2( 2,  3),
    ivec2( 4,  0),
    ivec2( 8, 12),
    ivec2( 7,  3),
    ivec2(11, 15),
    ivec2(13, 12),
    ivec2(14, 15),
    ivec2( 5,  0),
    ivec2( 6,  3),
    ivec2( 9, 12),
    ivec2(10, 15)
);

uint read_face_index(uint face_id, uint local_idx)
{
    const uint flat_idx = face_id * 16u + local_idx;
    return (face_indices[flat_idx >> 1u] >> ((flat_idx & 1u) << 4u)) & 0xFFFFu;
}

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
    const uint face_id      = uint(gl_InstanceIndex) / 12u;
    const uint line_in_face = uint(gl_InstanceIndex) % 12u;

    const ivec2 vtx_idx  = line_vertices[line_in_face];
    const uint  vtx1_idx = (gl_VertexIndex == 0) ? uint(vtx_idx.x) : uint(vtx_idx.y);
    const uint  vtx2_idx = (gl_VertexIndex == 0) ? uint(vtx_idx.y) : uint(vtx_idx.x);

    const vec3 vtx1_orig  = read_vertex(read_face_index(face_id, vtx1_idx));
    const vec3 vtx2_orig = read_vertex(read_face_index(face_id, vtx2_idx));

    const vec4 vtx1 = projection((vec4(vtx1_orig,  1) * model_view).xyz);
    const vec4 vtx2 = projection((vec4(vtx2_orig, 1) * model_view).xyz);

    const vec2  vtx1_screen = (vtx1.xy  / vtx1.w  + 1.0) / pixel_dim;
    const vec2  vtx2_screen = (vtx2.xy / vtx2.w + 1.0) / pixel_dim;
    const float pixel_dist  = length(vtx1_screen - vtx2_screen);

    out_pixel_dist = float(gl_VertexIndex) * pixel_dist;

    gl_Position = vtx1;
}
