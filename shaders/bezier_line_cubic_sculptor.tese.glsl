// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"

layout(quads, ccw, equal_spacing) in;

void main()
{
    const vec3 obj_pos = bezier_curve_cubic(gl_in[0].gl_Position.xyz,
                                            gl_in[1].gl_Position.xyz,
                                            gl_in[2].gl_Position.xyz,
                                            gl_in[3].gl_Position.xyz,
                                            gl_TessCoord.x);

    const vec4 view_pos = vec4(obj_pos, 1) * model_view;

    const float depth_bias = 0.0001f;

    gl_Position = vec4(view_pos.xy * proj.xy, view_pos.z * proj.z + view_pos.w * proj.w + depth_bias, view_pos.z);
}
