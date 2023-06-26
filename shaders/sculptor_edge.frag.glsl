// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(0.9, 0.9, 0.9, 1);
}
