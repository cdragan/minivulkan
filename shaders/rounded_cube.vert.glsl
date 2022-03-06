// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(location = 0) in vec3 in_pos;

layout(set = 0, binding = 0) uniform ubo_data
{
    mat4   model_view_proj;
    mat4   model;
    mat3x4 model_normal;
    vec4   color;
} ubo;

const float inset_value = 111.0 / 127.0;

float fix(float value)
{
    return (value ==  inset_value) ? ubo.color.w :
           (value == -inset_value) ? -ubo.color.w :
           value;
}

void main()
{
    vec3 pos = in_pos;
    pos.x = fix(pos.x);
    pos.y = fix(pos.y);
    pos.z = fix(pos.z);
    gl_Position = vec4(pos, 1);
}
