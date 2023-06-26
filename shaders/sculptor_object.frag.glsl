// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"

layout(location = 0) in  vec3 in_pos;
layout(location = 1) in  vec3 in_normal;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4((in_normal + 1) / 2, 1);

    //material_id = faces[gl_PrimitiveID].material_id;
}
