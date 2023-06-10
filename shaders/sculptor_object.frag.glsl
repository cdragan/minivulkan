// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(location = 0) in  vec3      in_pos;
layout(location = 1) in  vec3      in_normal;
layout(location = 2) in  flat uint in_material_id;

layout(location = 0) out vec4      out_color;

void main()
{
    out_color = vec4((in_normal + 1) / 2, 1);
}
