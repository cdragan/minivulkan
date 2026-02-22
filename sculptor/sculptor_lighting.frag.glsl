// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "transforms.glsl"

layout(set = 0, binding = 0) uniform usampler2D obj_id_att;
layout(set = 0, binding = 3) uniform sampler2D  normal_att;
layout(set = 0, binding = 4) uniform sampler2D  depth_att;

layout(location = 0) out vec4 out_color;

void main()
{
    // Pixel-exact coordinates to the input attachments
    const ivec2 coord = ivec2(gl_FragCoord.xy);

    // Read object id for this pixel
    const uint obj_id = texelFetch(obj_id_att, coord, 0).r;

    // If there is no object at this pixel, render background color
    if (obj_id == 0) {
        out_color = vec4(0.2, 0.2, 0.2, 1);
        return;
    }

    // Read object ids for surrounding pixels
    const ivec2 max_coord = textureSize(obj_id_att, 0) - 1;
    const uint obj_id_up = texelFetch(obj_id_att, clamp(coord - ivec2(0, 1), ivec2(0), max_coord), 0).r;
    const uint obj_id_dn = texelFetch(obj_id_att, clamp(coord + ivec2(0, 1), ivec2(0), max_coord), 0).r;
    const uint obj_id_lt = texelFetch(obj_id_att, clamp(coord - ivec2(1, 0), ivec2(0), max_coord), 0).r;
    const uint obj_id_rt = texelFetch(obj_id_att, clamp(coord + ivec2(1, 0), ivec2(0), max_coord), 0).r;

    // Draw edges
    if (obj_id != obj_id_up ||
        obj_id != obj_id_lt ||
        obj_id_rt == 0 ||
        obj_id_dn == 0) {

        out_color = vec4(0.93, 0.93, 0.93, 1);
        return;
    }

    // Read Z value from depth buffer
    const float depth = texelFetch(depth_att, coord, 0).r;

    // Read normal, remap components from [0, 1] range back to [-1, 1] range
    const vec3 normal = (texelFetch(normal_att, coord, 0).rgb - 0.5) * 2.0;

    // Reconstruct world-space position from depth and fragment coordinates
    const vec2  ndc_xy    = gl_FragCoord.xy * pixel_dim - 1.0;
    const float view_z    = (proj.w - depth * proj_w.w) / (depth * proj_w.z - proj.z);
    const float clip_w    = view_z * proj_w.z + proj_w.w;
    const vec3  view_pos  = vec3(ndc_xy * clip_w / proj.xy, view_z);
    const vec3  world_pos = vec4(view_pos, 1.0) * view_inverse;

    // Uncomment to visualize normals:
    //out_color = vec4(texelFetch(normal_att, coord, 0).rgb, 1.0);

    // Uncomment to visualize depth:
    //out_color = vec4(vec3(depth), 1.0);

    vec3 color = vec3(0.5);

    // Read face state, note that obj_id has 1 added to it, because 0 indicates no object
    const uint state = faces[obj_id - 1].state;

    if (state == 1)
        color *= vec3(1.2, 1.1, 1.1);
    else if (state == 2)
        color *= vec3(1, 1, 1.4);

    out_color = vec4(color, 1);
}
