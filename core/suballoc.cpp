// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "suballoc.h"
#include "minivulkan.h"
#include "mstdc.h"

void SubAllocatorBase::init(uint32_t size)
{
    num_free_chunks = 1;
    free_chunk[0] = { 0, size };
}

uint32_t SubAllocatorBase::align_size(uint32_t size)
{
    const uint32_t alignment = static_cast<uint32_t>(
            vk_phys_props.properties.limits.minStorageBufferOffsetAlignment);

    return mstd::align_up(size, alignment);
}

void SubAllocatorBase::remove_free_chunk(uint32_t i_chunk)
{
    assert(i_chunk < num_free_chunks);

    for (; i_chunk + 1 < num_free_chunks; i_chunk++)
        free_chunk[i_chunk] = free_chunk[i_chunk + 1];

    --num_free_chunks;
}

uint32_t SubAllocatorBase::allocate(uint32_t size)
{
    size = align_size(size);

    uint32_t i_chunk;

    for (i_chunk = 0; i_chunk < num_free_chunks; i_chunk++) {
        if (size <= free_chunk[i_chunk].size)
            break;
    }

    assert(i_chunk < num_free_chunks);

    if (i_chunk == num_free_chunks) {
        return 0; // TODO return something better than 0
    }

    FreeChunk& chunk = free_chunk[i_chunk];

    const uint32_t offset = chunk.offset;

    chunk.offset = offset + size;
    chunk.size  -= size;

    if (chunk.size == 0) {
        remove_free_chunk(i_chunk);
    }

    return offset;
}

void SubAllocatorBase::free(uint32_t offset, uint32_t size)
{
    size = align_size(size);

    const uint32_t end_offset = offset + size;

    uint32_t i_chunk;

    for (i_chunk = 0; i_chunk < num_free_chunks; i_chunk++) {
        if (offset < free_chunk[i_chunk].offset)
            break;
    }

    if (i_chunk > 0) {
        FreeChunk& prev = free_chunk[i_chunk - 1];

        if (prev.offset + prev.size == offset) {
            prev.size += size;

            if (i_chunk < num_free_chunks) {
                const FreeChunk& next = free_chunk[i_chunk];

                if (end_offset == next.offset) {
                    prev.size += next.size;

                    remove_free_chunk(i_chunk);
                }
            }

            return;
        }
    }

    if (i_chunk < num_free_chunks) {
        FreeChunk& next = free_chunk[i_chunk];

        if (end_offset == next.offset) {
            next.offset -= size;
            next.size   += size;

            return;
        }
    }

    assert(num_free_chunks < num_slots);

    if (num_free_chunks == num_slots)
        return; // TODO return some error

    for (uint32_t i_slot = num_free_chunks; i_slot > i_chunk; i_slot--) {
        free_chunk[i_slot] = free_chunk[i_slot - 1];
    }

    FreeChunk& chunk = free_chunk[i_chunk];
    chunk.offset = offset;
    chunk.size   = size;

    ++num_free_chunks;
}
