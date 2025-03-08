// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "memory_heap.h"

#include "d_printf.h"
#include "mstdc.h"
#include "vulkan/vulkan_core.h"

#include <assert.h>

namespace {
    constexpr VkDeviceSize min_heap_alignment = 0x1'0000U;
}

bool MemoryHeap::allocate_free_block(const VkMemoryRequirements& requirements,
                                     Placement                   placement,
                                     VkDeviceSize*               offset)
{
    // When allocating memory at the back of the heap, don't use free blocks
    // Memory allocated a the back is used for resizable window, i.e. main render target
    if (placement == Placement::back)
        return false;

    const VkDeviceSize alignment = requirements.alignment;
    const VkDeviceSize size      = mstd::align_up(requirements.size, min_heap_alignment);

    for (uint32_t i = 0; i < num_free_blocks; i++) {

        FreeBlock& block = free_blocks[i];

        const VkDeviceSize block_begin = block.offset;
        const VkDeviceSize block_end   = block_begin + block.size;
        const VkDeviceSize alloc_begin = mstd::align_up(block_begin,
                                                        mstd::max(alignment, min_heap_alignment));

        if (alloc_begin >= block_end || block_end - alloc_begin < size)
            continue;

        const VkDeviceSize alloc_end    = alloc_begin + size;
        const VkDeviceSize before_block = alloc_begin - block_begin;
        const VkDeviceSize after_block  = block.size - (alloc_end - block_begin);

        if (before_block) {
            block.size = before_block;

            if (after_block)
                insert_free_block(i + 1, alloc_end, after_block);
        }
        else if (after_block) {
            block.offset = alloc_end;
            block.size   = after_block;
        }
        else
            delete_free_block(i);

        *offset = alloc_begin;
        return true;
    }

    const VkDeviceSize alloc_begin = mstd::align_up(next_free_offs,
                                                    mstd::max(alignment, min_heap_alignment));
    const VkDeviceSize alloc_end = alloc_begin + size;
    if (alloc_begin >= last_free_offs || alloc_end > last_free_offs) {
        d_printf("No free block large enough for size 0x%" PRIx64 "!\n",
                static_cast<uint64_t>(requirements.size));
        return false;
    }

    const VkDeviceSize before_block = alloc_begin - next_free_offs;
    if (before_block)
        insert_free_block(num_free_blocks, next_free_offs, before_block);

    *offset        = next_free_offs;
    next_free_offs = alloc_end;

    return true;
}

void MemoryHeap::free_memory(VkDeviceSize offset, VkDeviceSize size)
{
    if ( ! size)
        return;

    // free_memory() is only valid with Placement::front
    assert(offset < last_free_offs);

    size = mstd::align_up(size, min_heap_alignment);

    if (num_free_blocks == mstd::array_size(free_blocks)) {
        d_printf("Failed to free block, all slots used, leaked 0x%" PRIx64 " bytes!\n",
                 static_cast<uint64_t>(size));
        return;
    }

    uint32_t i = 0;
    for ( ; i < num_free_blocks; i++) {
        if (free_blocks[i].offset > offset)
            break;
    }

    const VkDeviceSize n_a        = heap_size * 2;
    const VkDeviceSize prev_end   = (i > 0) ? (free_blocks[i - 1].offset + free_blocks[i - 1].size) : n_a;
    const VkDeviceSize next_begin = (i < num_free_blocks) ? free_blocks[i].offset : n_a;
    const VkDeviceSize cur_end    = offset + size;

    if (cur_end == next_begin) {
        free_blocks[i].offset =  offset;
        free_blocks[i].size   += size;

        if (offset == prev_end) {
            free_blocks[i - 1].size += free_blocks[i].size;
            delete_free_block(i);
        }
    }
    else if (offset == prev_end) {
        free_blocks[i - 1].size += size;
    }
    else {
        insert_free_block(i, offset, size);
    }
}

void MemoryHeap::insert_free_block(uint32_t idx, VkDeviceSize offset, VkDeviceSize size)
{
    if (num_free_blocks >= mstd::array_size(free_blocks)) {
        d_printf("Failed to split block, all slots used, leaked 0x%" PRIx64 " bytes!\n",
                    static_cast<uint64_t>(size));
        return;
    }

    if (idx < num_free_blocks) {
        assert(free_blocks[idx].offset > offset);
        assert(free_blocks[idx].offset > offset + size);
    }

    for (uint32_t j = num_free_blocks; j > idx; j--)
        free_blocks[j] = free_blocks[j - 1];

    free_blocks[idx].offset = offset;
    free_blocks[idx].size   = size;

    ++num_free_blocks;
}

void MemoryHeap::delete_free_block(uint32_t idx)
{
    const uint32_t tail_size = static_cast<uint32_t>(sizeof(FreeBlock)) * (num_free_blocks - idx - 1);

    if (tail_size)
        mstd::mem_copy(&free_blocks[idx], &free_blocks[idx + 1], tail_size);

    --num_free_blocks;
}
