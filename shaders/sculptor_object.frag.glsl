// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"

layout(location = 0) in  vec3      in_pos;
layout(location = 1) in  vec3      in_normal;
layout(location = 2) in  flat uint in_object_id;

layout(location = 0) out vec4      out_color;

void main()
{
    vec3 color = vec3(0.5, 0.5, 0.5);

    const uint state = faces[in_object_id].state;

    if (state == 1)
        color *= vec3(1.2, 1.1, 1.1);
    else if (state == 2)
        color *= vec3(1, 1, 1.4);

    out_color = vec4(color, 1);
}
