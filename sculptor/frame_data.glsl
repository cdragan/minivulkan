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
};

// Per-frame flags
#define FRAME_FLAG_SELECT_FACES     1u // face selection is active
#define FRAME_FLAG_SELECT_VERTICES  2u // vertex selection is active
#define FRAME_FLAG_WIREFRAME_MODE   4u // object is rendered in wireframe mode
