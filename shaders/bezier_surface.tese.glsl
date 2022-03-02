// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(quads, ccw, equal_spacing) in;

void main()
{
    const vec3 ab = mix(gl_in[0].gl_Position.xyz, gl_in[1].gl_Position.xyz, gl_TessCoord.x);
    const vec3 cd = mix(gl_in[2].gl_Position.xyz, gl_in[3].gl_Position.xyz, gl_TessCoord.x);
    gl_Position = vec4(mix(ab, cd, gl_TessCoord.x), 1);
}
