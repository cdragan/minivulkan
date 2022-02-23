// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(location = 0) in  vec3 pos;
layout(location = 1) in  vec3 normal;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;

layout(set = 0, binding = 0) uniform data
{
    mat4 model_view_proj;
    mat4 model_view;
};

void main()
{
    gl_Position = vec4(pos, 1) * model_view_proj;
    out_pos     = (vec4(pos, 1) * model_view).xyz;
    out_normal  = normal * mat3(model_view); // assume uniform scaling, so no inverse transpose
}
