// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

layout(location = 2) in flat uint in_object_id;

layout(location = 0) out     uint out_object_id;

void main()
{
    // 0 indicates no selection, so add 1 to distinguish between object 0 and no selection
    out_object_id = in_object_id + 1;
}
