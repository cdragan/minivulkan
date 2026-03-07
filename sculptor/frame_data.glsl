// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

layout(binding = 6) uniform frame_data {
    // Selection rectangle or mouse surroundings
    vec2 selection_rect_min;
    vec2 selection_rect_max;
    // Exact mouse cursor position in viewport pixels
    vec2 mouse_pos;
    uint frame_flags;
    uint pad;
    // Pixel dimensions in normalized device coordinates
    vec2 pixel_dim;
    vec2 pad2;

    // Light positions in world space
    vec4 light_pos[4];

    // GUI colors
    vec4 color_face_base;
    vec4 color_face_hovered;
    vec4 color_face_hovered_selected;
    vec4 color_face_selected;
    vec4 color_edge;
    vec4 color_vertex_hovered;
    vec4 color_vertex_hovered_selected;
    vec4 color_vertex_selected;
};

// Per-frame flags
#define FRAME_FLAG_SELECT_FACES     1u // face selection is active
#define FRAME_FLAG_SELECT_VERTICES  2u // vertex selection is active
#define FRAME_FLAG_WIREFRAME_MODE   4u // object is rendered in wireframe mode
