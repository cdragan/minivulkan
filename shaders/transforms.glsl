// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

layout(set = 2, binding = 0) uniform transform_data {
    mat4   model_view;
    mat3x4 model_view_normal;
    vec4   proj;
    vec4   proj_w; // perspective: [0, 0, 1, 0], orthographic: [0, 0, 0, 1]
};

vec4 projection(vec3 pos)
{
    return vec4(pos.xy * proj.xy,
                pos.z * proj.z + proj.w,
                pos.z * proj_w.z + proj_w.w);
}
