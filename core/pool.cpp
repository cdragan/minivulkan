// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "pool.h"
#include <string.h>

uint32_t PoolBase::allocate(bool* occupied, uint32_t capacity)
{
    for (uint32_t slot = 0; slot < capacity; slot++) {

        if ( ! occupied[slot]) {
            occupied[slot] = true;
            ++num_allocated;
            return slot;
        }
    }

    return pool_no_slot;
}

uint32_t PoolBase::allocate_at(bool* occupied, uint32_t capacity, uint32_t index)
{
    if (index >= capacity || occupied[index]) {
        return pool_no_slot;
    }

    occupied[index] = true;
    ++num_allocated;
    return index;
}

void PoolBase::free(bool* occupied, uint32_t slot)
{
    if (occupied[slot]) {
        occupied[slot] = false;
        --num_allocated;
    }
}

void PoolBase::defragment(void* entries, uint32_t elem_size, bool* occupied, uint32_t capacity, uint32_t* old_to_new)
{
    uint8_t* const bytes     = static_cast<uint8_t*>(entries);
    uint32_t       dest_slot = 0;

    for (uint32_t slot = 0; slot < capacity; slot++) {

        if (occupied[slot]) {
            if (slot != dest_slot) {
                memcpy(bytes + dest_slot * elem_size, bytes + slot * elem_size, elem_size);
            }

            old_to_new[slot] = dest_slot;
            ++dest_slot;
        }
        else {
            old_to_new[slot] = pool_no_slot;
        }
    }

    assert(num_allocated == dest_slot);

    static_assert(sizeof(bool) == 1);
    if (dest_slot) {
        memset(occupied, true, dest_slot);
    }
    if (dest_slot < capacity) {
        memset(&occupied[dest_slot], false, capacity - dest_slot);
    }
}
