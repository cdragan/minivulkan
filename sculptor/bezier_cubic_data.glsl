// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

struct face_data {
    uint material_id;
    uint state; // 0: default, 1: hovered, 2: selected
};

layout(set = 2, binding = 1) readonly buffer faces_data {
    ivec4     tess_level;
    face_data faces[]; // Indexed with gl_PrimitiveID
};

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
