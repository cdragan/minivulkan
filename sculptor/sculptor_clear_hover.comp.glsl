// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(binding = 0) buffer sel_buf_data { uint data[]; } sel_buf;

void main()
{
    // Clear obj_hovered (bit 1), but selection (bit 0) is preserved
    sel_buf.data[gl_GlobalInvocationID.x] &= 0x01010101u;
}
