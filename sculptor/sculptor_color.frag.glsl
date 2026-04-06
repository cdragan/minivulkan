// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(push_constant) uniform push_data {
    vec4 diffuse_color;
};

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(diffuse_color.xyz, 1);
}
