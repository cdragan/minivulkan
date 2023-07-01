// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

layout(set = 2, binding = 0) uniform transform_data {
    mat4   model_view;
    mat3x4 model_view_normal;
    vec4   proj;
    vec4   proj_w;
};
