// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_EXT_shader_16bit_storage : require

layout(local_size_x_id = 0) in;

layout(constant_id = 0) const uint work_group_size = 1;

layout(binding = 0) readonly buffer data_buf { float data[]; };

layout(binding = 1) writeonly buffer output_buf { int16_t output_data[]; };

layout(push_constant) uniform param_buf {
    uint in_sound_offs;
};

int16_t convert(float value)
{
    return int16_t(int(clamp(value, -1.0, 1.0) * 32767.0));
}

void main()
{
    output_data[gl_LocalInvocationID.x * 2]     = convert(data[in_sound_offs + gl_LocalInvocationID.x * 2]);
    output_data[gl_LocalInvocationID.x * 2 + 1] = convert(data[in_sound_offs + gl_LocalInvocationID.x * 2 + 1]);
}
