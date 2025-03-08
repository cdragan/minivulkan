// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "transforms.glsl"

layout(location = 0) in  vec3 in_pos;

layout(location = 0) out vec4 out_color;

void main()
{
    const vec4 view_pos = vec4(in_pos, 1) * model_view;

    gl_Position = projection(view_pos.xyz);
}
