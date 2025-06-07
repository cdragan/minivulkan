// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(set = 0, binding = 0) readonly buffer data_buf { float data[]; };

layout(set = 1, binding = 0) writeonly buffer output_buf { float output_data[]; } chan[2];

layout(push_constant) uniform param_buf {
    uint in_sound_offs;
};

void main()
{
    chan[0].output_data[gl_LocalInvocationID.x] = data[in_sound_offs + gl_LocalInvocationID.x * 2];
    chan[1].output_data[gl_LocalInvocationID.x] = data[in_sound_offs + gl_LocalInvocationID.x * 2 + 1];
}
