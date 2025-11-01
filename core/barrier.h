// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#pragma once

#include "vulkan_functions.h"

void add_barrier(const VkBufferMemoryBarrier2& barrier);
void add_barrier(const VkImageMemoryBarrier2& barrier);
void send_barrier(VkCommandBuffer cmd_buf);
