// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "sculptor_material.glsl"

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(diffuse_color.xyz, 1);
}
