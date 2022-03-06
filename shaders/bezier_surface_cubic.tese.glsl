// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core

layout(quads, ccw, equal_spacing) in;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;

layout(set = 0, binding = 0) uniform ubo_data
{
    mat4   model_view_proj;
    mat4   model;
    mat3x4 model_normal;
} ubo;

vec3 bezier_curve_cubic(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
    const vec3 p01  = mix(p0, p1, t);
    const vec3 p12  = mix(p1, p2, t);
    const vec3 p23  = mix(p2, p3, t);
    const vec3 p012 = mix(p01, p12, t);
    const vec3 p123 = mix(p12, p23, t);
    return mix(p012, p123, t);
}

vec3 bezier_derivative_cubic(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
    const vec3 p012 = mix(p1 - p0, p2 - p1, t);
    const vec3 p123 = mix(p2 - p1, p3 - p2, t);
    return 3 * mix(p012, p123, t);
}

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

    const vec3 obj_pos = bezier_curve_cubic(p[0], p[1], p[2], p[3], gl_TessCoord.y);
    gl_Position        = vec4(obj_pos, 1) * ubo.model_view_proj;
    out_pos            = (vec4(obj_pos, 1) * ubo.model).xyz;

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
    out_normal = obj_normal * mat3(ubo.model_normal);
}
