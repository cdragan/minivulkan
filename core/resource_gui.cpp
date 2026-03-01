// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "resource.h"

#include "memory_heap.h"
#include "minivulkan.h"
#include "mstdc.h"

void Buffer::free()
{
    MemoryHeap*  const heap   = owning_heap;
    VkDeviceSize const offset = heap_offset;
    VkDeviceSize const size   = alloc_size;

    VK_FUNCTION(vkDestroyBuffer)(vk_dev, buffer, nullptr);

    if (size)
        heap->free_memory(offset, size);

    mstd::mem_zero(this, sizeof(*this));
}
