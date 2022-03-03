// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(location = 0) in vec3 in_pos;

void main()
{
    gl_Position = vec4(in_pos, 1);
}
