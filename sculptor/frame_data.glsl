// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

layout(binding = 6) uniform frame_data {
    // Selection rectangle or mouse surroundings
    vec2 selection_rect_min;
    vec2 selection_rect_max;
    // Exact mouse cursor position in viewport pixels
    vec2 mouse_pos;
    uint frame_flags;
};

// Per-frame flags
#define FRAME_FLAG_SELECTION_ACTIVE 1u // mouse hover selection is in progress
#define FRAME_FLAG_WIREFRAME_MODE   2u // object is rendered in wireframe mode
