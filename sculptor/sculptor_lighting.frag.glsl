// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"
#include "frame_data.glsl"
#include "transforms.glsl"

layout(binding = 0) uniform usampler2D obj_id_att;
layout(binding = 3) uniform sampler2D  normal_att;
layout(binding = 4) uniform sampler2D  depth_att;
layout(binding = 5) readonly buffer sel_buf_data { uint data[]; } sel_buf;
layout(binding = 7) buffer hover_pos_data { vec4 pos; } hover_pos_buf;

layout(location = 0) out vec4 out_color;

void main()
{
    // Pixel-exact coordinates in the input attachments
    const ivec2 coord = ivec2(gl_FragCoord.xy);

    // Read object id for this pixel
    const uint obj_id = texelFetch(obj_id_att, coord, 0).r;

    // If there is no object at this pixel, output transparent color
    if (obj_id == 0) {
        out_color = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Read object ids for surrounding pixels
    const ivec2 max_coord = textureSize(obj_id_att, 0) - 1;
    const uint  obj_id_up = texelFetch(obj_id_att, clamp(coord - ivec2(0, 1), ivec2(0), max_coord), 0).r;
    const uint  obj_id_dn = texelFetch(obj_id_att, clamp(coord + ivec2(0, 1), ivec2(0), max_coord), 0).r;
    const uint  obj_id_lt = texelFetch(obj_id_att, clamp(coord - ivec2(1, 0), ivec2(0), max_coord), 0).r;
    const uint  obj_id_rt = texelFetch(obj_id_att, clamp(coord + ivec2(1, 0), ivec2(0), max_coord), 0).r;

    // Draw edges
    if (obj_id != obj_id_up ||
        obj_id != obj_id_lt ||
        obj_id_rt == 0 ||
        obj_id_dn == 0) {

        out_color = vec4(0.93, 0.93, 0.93, 1);
        return;
    }

    // In wireframe mode, non-border pixels are transparent
    if ((frame_flags & FRAME_FLAG_WIREFRAME_MODE) != 0u) {
        out_color = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Read Z value from depth buffer
    const float depth = texelFetch(depth_att, coord, 0).r;

    // Read normal, remap components from [0, 1] range back to [-1, 1] range
    const vec3 normal = (texelFetch(normal_att, coord, 0).rgb - 0.5) * 2.0;

    // Reconstruct world-space position from depth and fragment coordinates
    // Note: viewport Y is flipped (height is negative)
    const vec2  ndc_xy    = vec2(gl_FragCoord.x * pixel_dim.x - 1.0,
                                 1.0 - gl_FragCoord.y * pixel_dim.y);
    const float view_z    = (proj.w - depth * proj_w.w) / (depth * proj_w.z - proj.z);
    const float clip_w    = view_z * proj_w.z + proj_w.w;
    const vec3  view_pos  = vec3(ndc_xy * clip_w / proj.xy, view_z);
    const vec3  world_pos = vec4(view_pos, 1.0) * view_inverse;

    // Write world position for CPU readback at the mouse cursor pixel
    if (coord == ivec2(mouse_pos))
        hover_pos_buf.pos = vec4(world_pos, 1.0);

    // Uncomment to visualize normals:
    //out_color = vec4(texelFetch(normal_att, coord, 0).rgb, 1.0);

    // Uncomment to visualize depth:
    //out_color = vec4(vec3(depth), 1.0);

    vec3 color = vec3(0.5);

    // Read object state from selection buffer
    // obj_id has 1 added to it (0 = no object), so actual object index is obj_id - 1.
    // There is one byte per object, but we're reading it as uint32
    const uint obj_idx = obj_id - 1u;
    const uint word    = sel_buf.data[obj_idx >> 2];
    const uint state   = (word >> ((obj_idx & 3u) * 8u)) & 0xFFu;

    if ((state & 2u) != 0u)      // obj_hovered
        color *= vec3(1.2, 1.1, 1.1);
    else if ((state & 1u) != 0u) // obj_selected
        color *= vec3(1, 1, 1.4);

    out_color = vec4(color, 1);
}
