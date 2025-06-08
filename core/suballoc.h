// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

class SubAllocatorBase {
    public:
        void init(uint32_t size);

        uint32_t allocate(uint32_t size);
        void     free(uint32_t offset, uint32_t size);

    protected:
        constexpr explicit SubAllocatorBase(uint32_t max_free_chunks)
            : num_slots(max_free_chunks) { }

        struct FreeChunk {
            uint32_t offset;
            uint32_t size;
        };

    private:
        static uint32_t align_size(uint32_t size);
        void remove_free_chunk(uint32_t i_chunk);

        uint32_t  num_free_chunks = 0;
        uint32_t  num_slots       = 0;
        FreeChunk free_chunk[1]   = { };
};

template<uint32_t max_free_chunks>
class SubAllocator: public SubAllocatorBase {
    public:
        constexpr SubAllocator() : SubAllocatorBase(max_free_chunks) { }

    private:
        FreeChunk remaining_chunks[max_free_chunks - 1] = { };
};

template<>
class SubAllocator<1>: public SubAllocatorBase {
    public:
        constexpr SubAllocator() : SubAllocatorBase(1) { }
};
