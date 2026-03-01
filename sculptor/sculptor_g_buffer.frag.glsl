// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "frame_data.glsl"

layout(location = 1) in  vec3      in_normal;
layout(location = 2) in  flat uint in_object_id;

layout(location = 0) out uint      out_obj_id;
layout(location = 1) out vec4      out_normal;

layout(binding = 5) coherent buffer sel_buf_data { uint data[]; } sel_buf;

void main()
{
    // Write object id + 1, use 0 to indicate no object at this pixel
    out_obj_id = in_object_id + 1;

    // Compress normal components from [-1, 1] range to [0, 1] range
    out_normal = vec4(in_normal * 0.5 + 0.5, 0);

    // Detect selected faces - shallow selection for opaque faces
    if ((frame_flags & FRAME_FLAG_SELECTION_ACTIVE) != 0u) {
        const vec2 pos = gl_FragCoord.xy;
        if (pos.x >= selection_rect_min.x && pos.x <= selection_rect_max.x &&
            pos.y >= selection_rect_min.y && pos.y <= selection_rect_max.y) {
            const uint word_idx = in_object_id >> 2;
            const uint byte_idx = in_object_id & 3u;
            atomicOr(sel_buf.data[word_idx], 2u << (byte_idx * 8u));
        }
    }
}
