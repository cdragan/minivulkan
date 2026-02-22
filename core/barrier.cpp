// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "barrier.h"
#include <assert.h>

static constexpr uint32_t max_buffer_barriers = 4;
static constexpr uint32_t max_image_barriers  = 4;

static VkBufferMemoryBarrier2 buffer_barriers[max_buffer_barriers];

static VkImageMemoryBarrier2 image_barriers[max_image_barriers];

static VkDependencyInfo dependency_info = {
    VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    nullptr,    // pNext
    0,          // dependecyFlags
    0,          // memoryBarrierCount,
    nullptr,    // pMemoryBarriers
    0,          // bufferMemoryBarrierCount
    buffer_barriers,
    0,          // imageMemoryBarrierCount
    image_barriers
};

void add_barrier(const VkBufferMemoryBarrier2& barrier)
{
    assert(dependency_info.bufferMemoryBarrierCount < max_buffer_barriers);

    buffer_barriers[dependency_info.bufferMemoryBarrierCount++] = barrier;
}

void add_barrier(const VkImageMemoryBarrier2& barrier)
{
    assert(dependency_info.imageMemoryBarrierCount < max_image_barriers);

    image_barriers[dependency_info.imageMemoryBarrierCount++] = barrier;
}

void send_barrier(VkCommandBuffer cmd_buf)
{
    vkCmdPipelineBarrier2(cmd_buf, &dependency_info);

    dependency_info.bufferMemoryBarrierCount = 0;
    dependency_info.imageMemoryBarrierCount  = 0;
}
