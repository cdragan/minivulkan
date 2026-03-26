// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

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
        explicit SubAllocator(const size_t size) { init_base(size, max_free_chunks); }

        using SubAllocatorBase::Chunk;

        void init(const size_t size) { init_base(size, max_free_chunks); }

    private:
        Chunk remaining_chunks[max_free_chunks - 1] = { };
};

template<>
class SubAllocator<1>: public SubAllocatorBase {
    public:
        constexpr SubAllocator() = default;
        explicit SubAllocator(const size_t size) { init_base(size, 1); }

        using SubAllocatorBase::Chunk;

        void init(const size_t size) { init_base(size, 1); }
};

class BufferSubAllocatorBase {
    public:
        constexpr BufferSubAllocatorBase(SubAllocatorBase* const suballoc, void* const buf)
            : alloc(suballoc), buffer(static_cast<uint8_t*>(buf)) { }

        template<typename T>
        T* allocate(const size_t count) {
            return static_cast<T*>(allocate_raw(count, sizeof(T)));
        }

        template<typename T>
        void free(const T* const ptr, const size_t count) { free_raw(ptr, count, sizeof(T)); }

    private:
        void* allocate_raw(size_t count, size_t elem_size);
        void  free_raw(const void* ptr, size_t count, size_t elem_size);

        SubAllocatorBase* alloc;
        uint8_t*          buffer;
};

template<uint32_t max_free_chunks>
class BufferSubAllocator: public BufferSubAllocatorBase {
    public:
        template<typename T, uint32_t N>
        explicit BufferSubAllocator(T (&buf)[N]) : BufferSubAllocator(buf, N * sizeof(T)) { }

        BufferSubAllocator(void* const buf, const size_t buf_capacity) :
            BufferSubAllocatorBase(&suballoc, buf), suballoc(buf_capacity) { }

    private:
        SubAllocator<max_free_chunks> suballoc;
};
