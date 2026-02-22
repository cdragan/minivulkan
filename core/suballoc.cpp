// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "suballoc.h"
#include "d_printf.h"
#include "minivulkan.h"
#include "mstdc.h"

void SubAllocatorBase::reset()
{
    assert(total_size);

    init_base(total_size, num_slots);
}

void SubAllocatorBase::init_base(size_t size, uint32_t slots)
{
    total_size      = size;
#ifndef NDEBUG
    used_size       = 0;
#endif
    num_free_chunks = 1;
    num_slots       = slots;
    free_chunk[0]   = { 0, size };
}

void SubAllocatorBase::remove_free_chunk(uint32_t i_chunk)
{
    assert(i_chunk < num_free_chunks);

    for (; i_chunk + 1 < num_free_chunks; i_chunk++)
        free_chunk[i_chunk] = free_chunk[i_chunk + 1];

    --num_free_chunks;
}

SubAllocatorBase::Chunk SubAllocatorBase::allocate(size_t size, size_t alignment)
{
    assert(alignment);
    // Make sure alignment is a power of two
    assert( ! (alignment & (alignment - 1)));

    uint32_t i_chunk;

    for (i_chunk = 0; i_chunk < num_free_chunks; i_chunk++) {
        if (size > free_chunk[i_chunk].size)
            continue;

        Chunk& chunk = free_chunk[i_chunk];

        size_t new_offset = chunk.offset;
        size_t new_size   = mstd::align_up(size, alignment);

        if (new_offset & (alignment - 1)) {

            new_offset = mstd::align_down(chunk.offset + chunk.size - size, alignment);

            if (new_offset < chunk.offset)
                continue;

            new_size = chunk.size - new_offset + chunk.offset;
        }
        else {

            new_size = mstd::align_up(size, alignment);

            if (new_size > chunk.size)
                continue;

            chunk.offset += new_size;
        }

        chunk.size -= new_size;

        if (chunk.size == 0) {
            remove_free_chunk(i_chunk);
        }

#ifndef NDEBUG
        used_size += new_size;
        if (used_size > max_used_size)
            max_used_size = used_size;
#endif

        return { new_offset, new_size };
    }

    assert(i_chunk < num_free_chunks);

    d_printf("Suballocator failed to allocate 0x%" PRIx64 " bytes - %s\n",
             static_cast<uint64_t>(size),
             num_free_chunks ? "note: heap is fragmented" : "out of memory");

    return  { total_size, 0 };
}

void SubAllocatorBase::free(size_t offset, size_t size)
{
#ifndef NDEBUG
    used_size -= size;
#endif

    const size_t end_offset = offset + size;

    uint32_t i_chunk;

    for (i_chunk = 0; i_chunk < num_free_chunks; i_chunk++) {
        if (offset < free_chunk[i_chunk].offset)
            break;
    }

    if (i_chunk > 0) {
        Chunk& prev = free_chunk[i_chunk - 1];

        if (prev.offset + prev.size == offset) {
            prev.size += size;

            if (i_chunk < num_free_chunks) {
                const Chunk& next = free_chunk[i_chunk];

                if (end_offset == next.offset) {
                    prev.size += next.size;

                    remove_free_chunk(i_chunk);
                }
            }

            return;
        }
    }

    if (i_chunk < num_free_chunks) {
        Chunk& next = free_chunk[i_chunk];

        if (end_offset == next.offset) {
            next.offset -= size;
            next.size   += size;

            return;
        }
    }

    assert(num_free_chunks < num_slots);

    if (num_free_chunks == num_slots) {
        d_printf("Suballocator free heap is too fragmented\n");
        return;
    }

    for (uint32_t i_slot = num_free_chunks; i_slot > i_chunk; i_slot--) {
        free_chunk[i_slot] = free_chunk[i_slot - 1];
    }

    free_chunk[i_chunk] = { offset, size };

    ++num_free_chunks;
}
