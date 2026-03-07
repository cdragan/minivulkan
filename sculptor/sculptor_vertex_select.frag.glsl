// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "sculptor_material.glsl"
#include "frame_data.glsl"

layout(early_fragment_tests) in;

layout(location = 1) in flat uint in_vtx_idx;
layout(location = 2) in flat uint in_in_rect;

layout(location = 0) out vec4  out_color;

layout(binding = 5) coherent buffer vtx_sel_buf_data { uint data[]; } vtx_sel_buf;

void main()
{
    const uint word_idx = in_vtx_idx >> 2;
    const uint byte_idx = in_vtx_idx & 3u;
    const uint shift    = byte_idx * 8u;

    // Mark vertex as hovered if it's in the selection rectangle
    if (in_in_rect != 0u)
        atomicOr(vtx_sel_buf.data[word_idx], 2u << shift);

    // Read current state and choose color
    const uint state = (vtx_sel_buf.data[word_idx] >> shift) & 0xFFu;

    vec3 color;
    if (state == 3u)                      // hovered + selected
        color = color_vertex_hovered_selected.rgb;
    else if ((state & 2u) != 0u)          // hovered
        color = color_vertex_hovered.rgb;
    else if ((state & 1u) != 0u)          // selected
        color = color_vertex_selected.rgb;
    else
        color = diffuse_color.rgb;

    out_color = vec4(color, 1.0);
}
