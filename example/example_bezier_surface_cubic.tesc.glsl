// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "ubo_data.glsl"

layout(vertices = 16) out;

void main()
{
    if (gl_InvocationID == 0) {
        const uint width  = uint(ubo.params.y);
        const uint height = uint(ubo.params.y);

        gl_TessLevelOuter[0] = height;
        gl_TessLevelOuter[1] = width;
        gl_TessLevelOuter[2] = height;
        gl_TessLevelOuter[3] = width;

        gl_TessLevelInner[0] = width;
        gl_TessLevelInner[1] = height;
    }

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}
