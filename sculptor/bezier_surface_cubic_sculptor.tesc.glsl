// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"
#include "frame_data.glsl"

layout(vertices = 16) out;

const float pixels_per_segment = 16.0;

float edge_tess(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float max_level)
{
    const float distance = length((p1 - p0) / pixel_dim)
                         + length((p2 - p1) / pixel_dim)
                         + length((p3 - p2) / pixel_dim);
    const float screen_level = distance / pixels_per_segment;

    return clamp(screen_level, 1.0, max_level);
}

void calculate_tess_level()
{
    if ((frame_flags & FRAME_FLAG_TESSELLATION_OFF) != 0u) {
        gl_TessLevelOuter[0] = 1.0;
        gl_TessLevelOuter[1] = 1.0;
        gl_TessLevelOuter[2] = 1.0;
        gl_TessLevelOuter[3] = 1.0;
        gl_TessLevelInner[0] = 1.0;
        gl_TessLevelInner[1] = 1.0;
    }
    else {
        const float max_tess_level = 10.0;
        const uint  face_max       = faces[gl_PrimitiveID].max_tess_level;
        const float max_level      = face_max > 0u ? float(face_max) : max_tess_level;

        vec2 screen_pos[16];
        for (int i = 0; i < 16; i++) {
            const vec4 clip = projection((gl_in[i].gl_Position * model_view).xyz);
            screen_pos[i] = clip.xy / max(clip.w, 0.001);
        }

        float vert_level[4];
        for (int i = 0; i < 4; i++) {
            vert_level[i] = edge_tess(screen_pos[i], screen_pos[i + 4], screen_pos[i + 8], screen_pos[i + 12], max_level);
        }

        float horiz_level[4];
        for (int i = 0; i < 4; i++) {
            const int j = i * 4;
            horiz_level[i] = edge_tess(screen_pos[j], screen_pos[j + 1], screen_pos[j + 2], screen_pos[j + 3], max_level);
        }

        gl_TessLevelOuter[0] = vert_level[0];
        gl_TessLevelOuter[1] = horiz_level[0];
        gl_TessLevelOuter[2] = vert_level[3];
        gl_TessLevelOuter[3] = horiz_level[3];

        gl_TessLevelInner[0] = max(max(horiz_level[0], horiz_level[1]), max(horiz_level[2], horiz_level[3]));
        gl_TessLevelInner[1] = max(max(vert_level[0],  vert_level[1]),  max(vert_level[2],  vert_level[3]));
    }
}

void main()
{
    if (gl_InvocationID == 0)
        calculate_tess_level();

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position * model_view;
}
