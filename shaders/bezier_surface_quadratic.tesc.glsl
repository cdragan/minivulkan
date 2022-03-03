// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(vertices = 9) out;

void main()
{
    if (gl_InvocationID == 0) {
        const uint width  = 3;
        const uint height = 3;

        gl_TessLevelOuter[0] = height;
        gl_TessLevelOuter[1] = width;
        gl_TessLevelOuter[2] = height;
        gl_TessLevelOuter[3] = width;

        gl_TessLevelInner[0] = width;
        gl_TessLevelInner[1] = height;
    }
}
