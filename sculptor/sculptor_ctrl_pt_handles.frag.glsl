// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require
#extension GL_EXT_terminate_invocation: require

#include "frame_data.glsl"

layout(location = 0) in float in_pixel_dist;

layout(location = 0) out vec4 out_color;

void main()
{
    // Create dotted line: 4-pixel dots with 4-pixel gaps
    if (fract(in_pixel_dist / 8.0) > 0.5)
        terminateInvocation;

    out_color = color_ctrl_pt;
}
