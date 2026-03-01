// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "bezier_cubic_data.glsl"

layout(location = 2) in flat uint in_object_id;

layout(binding = 5) coherent buffer sel_buf_data { uint data[]; } sel_buf;

void main()
{
    // Mark this object as hovered (bit 1)
    const uint word_idx = in_object_id >> 2;
    const uint byte_idx = in_object_id & 3u;
    const uint shift    = byte_idx * 8u;
    atomicOr(sel_buf.data[word_idx], 2u << shift);
}
