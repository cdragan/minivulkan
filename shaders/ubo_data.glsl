// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

layout(constant_id = 0) const uint num_lights = 1;

layout(set = 0, binding = 0) uniform ubo_data {
    mat4   model_view_proj;
    mat4   model;
    mat3x4 model_normal;
    vec4   color;
    vec4   params;
    vec4   lights[num_lights > 0 ? num_lights : 1];
} ubo;
