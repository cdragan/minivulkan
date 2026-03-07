// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "frame_data.glsl"

layout(location = 1) in  vec3      in_normal;
layout(location = 2) in  flat uint in_object_id;

layout(location = 0) out vec4      out_color;

void main()
{
    vec3 color = color_face_base.rgb;

    const uint state = faces[in_object_id].state;

    if (state == 3)
        color = color_face_hovered_selected.rgb;
    else if (state == 1)
        color = color_face_hovered.rgb;
    else if (state == 2)
        color = color_face_selected.rgb;

    out_color = vec4(color, 1);
}
