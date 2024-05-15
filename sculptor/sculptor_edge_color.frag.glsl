// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "sculptor_material.glsl"

layout(location = 0) in  vec4 in_pos;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(diffuse_color.xyz, 1);

    gl_FragDepth = in_pos.w * 1.0078125;
}
