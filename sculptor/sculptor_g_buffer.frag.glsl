// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"

layout(location = 0) in  vec4      in_pos;
layout(location = 1) in  vec3      in_normal;
layout(location = 2) in  flat uint in_object_id;

layout(location = 0) out uint      out_obj_id;
layout(location = 1) out vec4      out_normal;

void main()
{
    // Write object id + 1, use 0 to indicate no object at this pixel
    out_obj_id = in_object_id + 1;

    // Compress normal components from [-1, 1] range to [0, 1] range
    out_normal = vec4(in_normal * 0.5 + 0.5, 0);

    // in_pos.w contains z/w
    gl_FragDepth = in_pos.w;
}
