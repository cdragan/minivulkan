// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "vulkan_functions.h"

#include "usage.h"
#include "suballoc.h"

// Memory heap is a single range of device memory allocated once during init.
//
// Resources (imaged, buffers used for textures, swapchains, etc.) are allocated
// from this memory range.
//
// There can be a "shadow" host memory range allocated with the same size, used for
// filling out resources on the host, then transferring them to device.  For devices
// with unified memory, the host memory range is not allocated.
//
// A separate heap can also be used for dynamic resources, such as uniform buffers.
// This heap has a separate memory range, which can be either on the device
// or on the host, depending on what is the optimal memory type available.

class MemoryHeap {
    public:
        constexpr MemoryHeap()                   = default;
        MemoryHeap(const MemoryHeap&)            = delete;
        MemoryHeap& operator=(const MemoryHeap&) = delete;

        bool allocate_heap(int req_memory_type, VkDeviceSize size);

        bool allocate_memory(const VkMemoryRequirements& requirements,
                             VkDeviceSize*               offset,
                             VkDeviceSize*               size);
        void free_memory(VkDeviceSize offset, VkDeviceSize size);

        VkDeviceMemory get_memory()   const { return memory; }
        void*          get_host_ptr() const { return host_ptr; }

        bool check_memory_type(uint32_t memory_type_bits) const
        {
            return !! (memory_type_bits & (1u << memory_type));
        }

        void print_stats(const char* heap_name) const;

    private:
        VkDeviceMemory    memory      = VK_NULL_HANDLE;
        void*             host_ptr    = nullptr;
        VkDeviceSize      heap_size   = 0;
        uint32_t          memory_type = 0;
        SubAllocator<256> suballoc;
};

class MemoryAllocator {
    public:
        constexpr MemoryAllocator()                        = default;
        MemoryAllocator(const MemoryAllocator&)            = delete;
        MemoryAllocator& operator=(const MemoryAllocator&) = delete;
#ifndef NDEBUG
        ~MemoryAllocator();
#endif

        bool init_heaps(VkDeviceSize device_heap_size,
                        VkDeviceSize host_heap_size,
                        VkDeviceSize dynamic_heap_size);

        bool allocate_memory(const VkMemoryRequirements& requirements,
                             Usage                       heap_usage,
                             VkDeviceSize*               offset,
                             VkDeviceSize*               size,
                             MemoryHeap**                heap);

        bool need_host_copy(Usage heap_usage);

    private:
        MemoryHeap device_heap;
        MemoryHeap host_heap;
        MemoryHeap dynamic_heap;
};

extern MemoryAllocator mem_mgr;
