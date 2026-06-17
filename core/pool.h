// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <assert.h>
#include <stdint.h>
#include <type_traits>

constexpr uint32_t pool_no_slot = 0xFFFFFFFFu;

class PoolBase {
    public:
        uint32_t num_allocated;

        uint32_t allocate(bool* occupied, uint32_t capacity);

        void free(bool* occupied, uint32_t slot);

        void defragment(void* entries, uint32_t elem_size, bool* occupied, uint32_t capacity, uint32_t* old_to_new);
};

template<typename T, uint32_t capacity>
class Pool: private PoolBase {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Pool element type must be trivially copyable for byte-snapshot undo and save/load");

    public:
        using PoolBase::num_allocated;

        T entries[capacity];

        uint32_t allocate()
        {
            return PoolBase::allocate(occupied, capacity);
        }

        void free(uint32_t slot)
        {
            assert(slot < capacity);
            PoolBase::free(occupied, slot);
        }

        void defragment(uint32_t (&old_to_new)[capacity])
        {
            PoolBase::defragment(entries, sizeof(T), occupied, capacity, old_to_new);
        }

        bool is_occupied(uint32_t slot) const
        {
            assert(slot < capacity);
            return occupied[slot];
        }

    private:
        bool occupied[capacity];
};
