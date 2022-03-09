// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(location = 0) in  vec3 in_pos;
layout(location = 1) in  vec3 in_normal;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;

#include "ubo_data.glsl"

void main()
{
    gl_Position = vec4(in_pos, 1) * ubo.model_view_proj;
    out_pos     = (vec4(in_pos, 1) * ubo.model).xyz;
    out_normal  = in_normal * mat3(ubo.model_normal);
}
