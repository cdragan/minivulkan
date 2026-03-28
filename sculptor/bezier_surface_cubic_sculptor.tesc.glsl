// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"
#include "frame_data.glsl"

layout(vertices = 16) out;

// Target pixels per tessellation segment
const float pixels_per_segment = 16.0;

vec2 to_screen(vec4 obj_pos)
{
    const vec4 clip = projection((obj_pos * model_view).xyz);
    return clip.xy / max(clip.w, 0.001);
}

float edge_level(vec4 p0, vec4 p1, vec4 p2, vec4 p3)
{
    const vec2 n0 = to_screen(p0);
    const vec2 n1 = to_screen(p1);
    const vec2 n2 = to_screen(p2);
    const vec2 n3 = to_screen(p3);

    const float arc_pixels = (length((n1 - n0) / pixel_dim)
                            + length((n2 - n1) / pixel_dim)
                            + length((n3 - n2) / pixel_dim));

    return max(1.0, arc_pixels / pixels_per_segment);
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

        const float lev0 = edge_level(gl_in[ 0].gl_Position, gl_in[ 4].gl_Position, gl_in[ 8].gl_Position, gl_in[12].gl_Position);
        const float lev1 = edge_level(gl_in[ 0].gl_Position, gl_in[ 1].gl_Position, gl_in[ 2].gl_Position, gl_in[ 3].gl_Position);
        const float lev2 = edge_level(gl_in[ 3].gl_Position, gl_in[ 7].gl_Position, gl_in[11].gl_Position, gl_in[15].gl_Position);
        const float lev3 = edge_level(gl_in[12].gl_Position, gl_in[13].gl_Position, gl_in[14].gl_Position, gl_in[15].gl_Position);

        gl_TessLevelOuter[0] = clamp(lev0, 1.0, max_level);
        gl_TessLevelOuter[1] = clamp(lev1, 1.0, max_level);
        gl_TessLevelOuter[2] = clamp(lev2, 1.0, max_level);
        gl_TessLevelOuter[3] = clamp(lev3, 1.0, max_level);
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
        gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
    }
}

void main()
{
    if (gl_InvocationID == 0)
        calculate_tess_level();

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position * model_view;
}
