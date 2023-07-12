// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(location = 0) in vec3 in_pos;

#include "ubo_data.glsl"

const float inset_value = 111.0 / 127.0;

float fix(float value)
{
    return (value ==  inset_value) ? ubo.params.x :
           (value == -inset_value) ? -ubo.params.x :
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
