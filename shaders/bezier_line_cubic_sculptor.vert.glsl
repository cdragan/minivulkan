// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"

struct index_data {
    uint idx01;
    uint idx23;
};

layout(set = 2, binding = 2) buffer edge_indices {
    index_data indices[]; // Vertex indices
};

struct vertex_data {
    uint xy;
    uint z;
};

layout(set = 2, binding = 3) buffer edge_vertices {
    vertex_data vertices[];
};

uvec4 read_indices(uint edge_index)
{
    const index_data data = indices[edge_index];

    const uint idx0 = data.idx01 & 0xFFFF;
    const uint idx1 = data.idx01 >> 16;
    const uint idx2 = data.idx23 & 0xFFFF;
    const uint idx3 = data.idx23 >> 16;

    return uvec4(idx0, idx1, idx2, idx3);
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

vec3 calc_current_vertex_pos(uint edge_index, float t)
{
    const uvec4 indices = read_indices(edge_index);

    return bezier_curve_cubic(read_vertex(indices.x),
                              read_vertex(indices.y),
                              read_vertex(indices.z),
                              read_vertex(indices.w),
                              t);
}

void main()
{
    const float t = float(gl_VertexIndex) / float(tess_level.x);

    const vec3 pos = calc_current_vertex_pos(gl_InstanceIndex, t);

    const vec4 view_pos = vec4(pos, 1) * model_view;
    const vec4 screen_pos = projection(view_pos.xyz);
    const float depth_bias = 1.0 / 4096.0;
    gl_Position = vec4(screen_pos.xy, screen_pos.z + depth_bias * screen_pos.w, screen_pos.w);
}

