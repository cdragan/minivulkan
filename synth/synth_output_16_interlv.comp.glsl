// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

#extension GL_EXT_shader_16bit_storage : require

layout(set = 0, binding = 0) buffer InputData { float buf[]; } input_data[];

layout(set = 0, binding = 1) buffer OutputData { int16_t buf[]; } output_data;

layout(local_size_x = 256) in;

int16_t convert(float value)
{
    return int16_t(int(value * 32767.0));
}

void main()
{
    output_data.buf[gl_LocalInvocationID.x * 2]     = convert(input_data[0].buf[gl_LocalInvocationID.x]);
    output_data.buf[gl_LocalInvocationID.x * 2 + 1] = convert(input_data[1].buf[gl_LocalInvocationID.x]);
}
