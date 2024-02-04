// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"

layout(vertices = 4) out;

void main()
{
    if (gl_InvocationID == 0) {
        // TODO calculate tessellation level based on distance from camera
        const uint level = tess_level.x;

        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelOuter[3] = level;

        gl_TessLevelInner[0] = level;
        gl_TessLevelInner[1] = level;
    }

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}
