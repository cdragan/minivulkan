// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"

layout(quads, ccw, equal_spacing) in;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out uint out_object_id;

void main()
{
    vec3 p[4];
    for (uint i = 0; i < 4; i++) {
        p[i] = bezier_curve_cubic(gl_in[i * 4].gl_Position.xyz,
                                  gl_in[i * 4 + 1].gl_Position.xyz,
                                  gl_in[i * 4 + 2].gl_Position.xyz,
                                  gl_in[i * 4 + 3].gl_Position.xyz,
                                  gl_TessCoord.x);
    }

    const vec3 obj_pos  = bezier_curve_cubic(p[0], p[1], p[2], p[3], gl_TessCoord.y);
    const vec4 view_pos = vec4(obj_pos, 1) * model_view;

    out_pos     = view_pos.xyz;
    gl_Position = projection(view_pos.xyz);

    const vec3 du = bezier_derivative_cubic(p[0], p[1], p[2], p[3], gl_TessCoord.y);

    for (uint i = 0; i < 4; i++) {
        p[i] = bezier_curve_cubic(gl_in[i].gl_Position.xyz,
                                  gl_in[i + 4].gl_Position.xyz,
                                  gl_in[i + 8].gl_Position.xyz,
                                  gl_in[i + 12].gl_Position.xyz,
                                  gl_TessCoord.y);
    }
    const vec3 dv = bezier_derivative_cubic(p[0], p[1], p[2], p[3], gl_TessCoord.x);

    const vec3 obj_normal = cross(dv, du);
    out_normal = normalize(obj_normal) * mat3(model_view_normal);

    out_object_id = gl_PrimitiveID;
}
