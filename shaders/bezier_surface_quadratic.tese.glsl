// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#version 460 core
#extension GL_EXT_scalar_block_layout: require

layout(quads, ccw, equal_spacing) in;

layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_normal;

layout(set = 0, binding = 0, std430) uniform ubo_data
{
    mat4   model_view_proj;
    mat4   model;
    mat3x4 model_normal;
} ubo;

vec3 bezier_curve_quadratic(vec3 p0, vec3 p1, vec3 p2, float t)
{
    const vec3 p01 = mix(p0, p1, t);
    const vec3 p12 = mix(p1, p2, t);
    return mix(p01, p12, t);
}

vec3 bezier_derivative_quadratic(vec3 p0, vec3 p1, vec3 p2, float t)
{
    return 2 * mix(p1 - p0, p2 - p1, t);
}

void main()
{
    vec3 p[3];
    for (uint i = 0; i < 3; i++) {
        p[i] = bezier_curve_quadratic(gl_in[i * 3].gl_Position.xyz,
                                      gl_in[i * 3 + 1].gl_Position.xyz,
                                      gl_in[i * 3 + 2].gl_Position.xyz,
                                      gl_TessCoord.x);
    }

    const vec3 obj_pos = bezier_curve_quadratic(p[0], p[1], p[2], gl_TessCoord.y);
    gl_Position        = vec4(obj_pos, 1) * ubo.model_view_proj;
    out_pos            = (vec4(obj_pos, 1) * ubo.model).xyz;

    const vec3 du = bezier_derivative_quadratic(p[0], p[1], p[2], gl_TessCoord.y);

    for (uint i = 0; i < 3; i++) {
        p[i] = bezier_curve_quadratic(gl_in[i].gl_Position.xyz,
                                      gl_in[i + 3].gl_Position.xyz,
                                      gl_in[i + 6].gl_Position.xyz,
                                      gl_TessCoord.y);
    }
    const vec3 dv = bezier_derivative_quadratic(p[0], p[1], p[2], gl_TessCoord.x);

    const vec3 obj_normal = cross(du, dv);
    out_normal = obj_normal * mat3(ubo.model_normal);
}
