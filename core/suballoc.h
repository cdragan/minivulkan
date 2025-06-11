// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stddef.h>
#include <stdint.h>

class SubAllocatorBase {
    public:
        void reset();

        struct Chunk {
            size_t offset;
            size_t size;
        };

        Chunk allocate(size_t size, size_t alignment);
        void  free(size_t offset, size_t size);

#ifndef NDEBUG
        size_t get_used_size()     const { return used_size;     }
        size_t get_max_used_size() const { return max_used_size; }
#endif

    protected:
        constexpr SubAllocatorBase() = default;

        void init_base(size_t size, uint32_t slots);

    private:
        void remove_free_chunk(uint32_t i_chunk);

        size_t   total_size      = 0;
#ifndef NDEBUG
        size_t   used_size       = 0;
        size_t   max_used_size   = 0;
#endif
        uint32_t num_free_chunks = 0;
        uint32_t num_slots       = 0;
        Chunk    free_chunk[1]   = { };
};

template<uint32_t max_free_chunks>
class SubAllocator: public SubAllocatorBase {
    public:
        constexpr SubAllocator() = default;

        using SubAllocatorBase::Chunk;

        void init(size_t size) { init_base(size, max_free_chunks); }

    private:
        Chunk remaining_chunks[max_free_chunks - 1] = { };
};

template<>
class SubAllocator<1>: public SubAllocatorBase {
    public:
        constexpr SubAllocator() = default;

        using SubAllocatorBase::Chunk;

        void init(size_t size) { init_base(size, 1); }
};
