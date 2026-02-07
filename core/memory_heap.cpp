// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "memory_heap.h"
#include "minivulkan.h"

#include "d_printf.h"
#include "mstdc.h"

#include <assert.h>
#include <iterator>

MemoryAllocator mem_mgr;

static VkPhysicalDeviceMemoryProperties vk_mem_props;

#ifndef NDEBUG
static unsigned in_mb(VkDeviceSize size)
{
    constexpr VkDeviceSize one_mb = 1024u * 1024u;
    return static_cast<unsigned>(mstd::align_up(size, one_mb) / one_mb);
}
#endif

bool MemoryHeap::allocate_heap(int req_memory_type, VkDeviceSize size)
{
    assert(req_memory_type >= 0);
    assert(req_memory_type <  static_cast<int>(std::size(vk_mem_props.memoryTypes)));
    assert(memory          == VK_NULL_HANDLE);
    assert(host_ptr        == nullptr);
    assert(heap_size       == 0);

    size = mstd::align_up(size, VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));

    static VkMemoryAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        0,  // allocationSize
        0   // memoryTypeIndex
    };
    alloc_info.allocationSize  = size;
    alloc_info.memoryTypeIndex = static_cast<uint32_t>(req_memory_type);

    VkResult res = CHK(vkAllocateMemory(vk_dev, &alloc_info, nullptr, &memory));
    if (res != VK_SUCCESS)
        return false;

    if (vk_mem_props.memoryTypes[req_memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        res = CHK(vkMapMemory(vk_dev, memory, 0, size, 0, &host_ptr));
        if (res != VK_SUCCESS)
            return false;
    }

    heap_size   = size;
    memory_type = static_cast<uint32_t>(req_memory_type);

    suballoc.init(static_cast<size_t>(size));

    d_printf("Allocated %s heap size 0x%" PRIx64 " bytes (%u MB) with memory type %d\n",
             heap_name, static_cast<uint64_t>(size), in_mb(size), req_memory_type);

    return true;
}

bool MemoryHeap::allocate_memory(const VkMemoryRequirements& requirements,
                                 VkDeviceSize*               offset,
                                 VkDeviceSize*               size)
{
    const SubAllocatorBase::Chunk chunk = suballoc.allocate(requirements.size, requirements.alignment);

    if (chunk.offset >= heap_size) {
        d_printf("Not enough device memory\n");
        d_printf("Requested surface size 0x%" PRIx64 ", used heap size 0x%" PRIx64 ", total heap size 0x%" PRIx64 "\n",
                static_cast<uint64_t>(requirements.size),
                static_cast<uint64_t>(suballoc.get_used_size()),
                static_cast<uint64_t>(heap_size));
        return false;
    }

    if (chunk.offset % requirements.alignment) {
        d_printf("Invalid alignment from suballocator, requested alignment 0x%" PRIx64 ", got offset 0x%" PRIx64 "\n",
                 static_cast<uint64_t>(requirements.alignment),
                 static_cast<uint64_t>(chunk.offset));
        return false;
    }

    *offset = static_cast<VkDeviceSize>(chunk.offset);
    *size   = static_cast<VkDeviceSize>(chunk.size);

    return true;
}

void MemoryHeap::free_memory(VkDeviceSize offset, VkDeviceSize size)
{
    suballoc.free(static_cast<size_t>(offset), static_cast<size_t>(size));
}

#ifndef NDEBUG
static void str_append(char* buf, const char* str)
{
    while (*buf)
        ++buf;

    while (*str)
        *(buf++) = *(str++);

    *buf = 0;
}
#endif

enum DevicePlacement {
    require_host_memory,
    allow_device_memory
};

static int find_mem_type(const uint8_t  *preferred_flags,
                         DevicePlacement allow_device)
{
    int          found_type      = -1;
    VkDeviceSize found_heap_size = 0;

    for (uint32_t idx = 0; found_type < 0; idx++) {

        const uint32_t try_flags = preferred_flags[idx];

        if ( ! try_flags)
            break;

        for (uint32_t i_type = 0; i_type < vk_mem_props.memoryTypeCount; i_type++) {

            const VkMemoryType& memory_type    = vk_mem_props.memoryTypes[i_type];
            const uint32_t      property_flags = memory_type.propertyFlags;
            const VkDeviceSize  heap_size      = vk_mem_props.memoryHeaps[memory_type.heapIndex].size;

            if ( ! allow_device && (property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                continue;

            if (((property_flags & try_flags) == try_flags) && (heap_size > found_heap_size)) {
                found_type      = static_cast<int>(i_type);
                found_heap_size = heap_size;
            }
        }
    }

    return found_type;
}

bool MemoryAllocator::init_heaps(VkDeviceSize device_heap_size,
                                 VkDeviceSize host_heap_size,
                                 VkDeviceSize dynamic_heap_size,
                                 VkDeviceSize transient_heap_size)
{
    assert( ! device_heap.get_memory());

    vkGetPhysicalDeviceMemoryProperties(vk_phys_dev, &vk_mem_props);

#ifndef NDEBUG
    for (uint32_t i_heap = 0; i_heap < vk_mem_props.memoryHeapCount; i_heap++) {
        d_printf("Memory heap %u, size %u MB\n",
                 i_heap,
                 static_cast<unsigned>(static_cast<uint64_t>(vk_mem_props.memoryHeaps[i_heap].size / (1024u * 1024u))));

        for (uint32_t i_type = 0; i_type < vk_mem_props.memoryTypeCount; i_type++) {

            const VkMemoryType& memory_type    = vk_mem_props.memoryTypes[i_type];
            const uint32_t      property_flags = memory_type.propertyFlags;

            if (memory_type.heapIndex != i_heap)
                continue;

            static char info[64];
            info[0] = 0;
            if (property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                str_append(info, "device, ");
            if (property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                str_append(info, "host_visible, ");
            if (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                str_append(info, "host_coherent, ");
            if (property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                str_append(info, "host_cached, ");
            if (property_flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
                str_append(info, "lazily_allocated, ");
            if (property_flags & VK_MEMORY_PROPERTY_PROTECTED_BIT)
                str_append(info, "protected, ");
            if (info[0])
                info[mstd::strlen(info) - 2] = 0;
            d_printf("    type %u: flags 0x%x (%s)\n",
                     i_type,
                     property_flags,
                     info);
        }
    }
#endif

    static const uint8_t preferred_device_heap_flags[] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        0
    };

    static const uint8_t preferred_transient_heap_flags[] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,
        0
    };

    static const uint8_t preferred_host_heap_flags[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        0
    };

    static const uint8_t preferred_dynamic_heap_flags[] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        0
    };

    const int device_type_index    = find_mem_type(preferred_device_heap_flags,    allow_device_memory);
    int       host_type_index      = find_mem_type(preferred_host_heap_flags,      require_host_memory);
    const int dynamic_type_index   = find_mem_type(preferred_dynamic_heap_flags,   allow_device_memory);
    const int transient_type_index = find_mem_type(preferred_transient_heap_flags, allow_device_memory);

    if (host_type_index < 0) {
        host_type_index = dynamic_type_index;
        unified = true;
    }

    if (transient_type_index < 0)
        device_heap_size += transient_heap_size;

    d_printf("Selected memory types: device=%d, host=%d, dynamic=%d\n",
             device_type_index, host_type_index, dynamic_type_index);

    if (device_type_index < 0 || dynamic_type_index < 0) {
        d_printf("Could not find required memory type\n");
        return false;
    }

    if (dynamic_type_index == device_type_index)
        device_heap_size += dynamic_heap_size;
    else if ( ! dynamic_heap.allocate_heap(dynamic_type_index, dynamic_heap_size))
        return false;

    if (host_type_index == device_type_index) {
        device_heap_size += host_heap_size;
        unified = true;
    }
    else if ( ! host_heap.allocate_heap(host_type_index, host_heap_size))
        return false;

    if ( ! device_heap.allocate_heap(device_type_index, device_heap_size))
        return false;

    if (transient_type_index >= 0)
        if ( ! transient_heap.allocate_heap(transient_type_index, transient_heap_size))
            return false;

    return true;
}

bool MemoryAllocator::allocate_memory(const VkMemoryRequirements& requirements,
                                      Usage                       heap_usage,
                                      VkDeviceSize*               offset,
                                      VkDeviceSize*               size,
                                      MemoryHeap**                heap)
{
    MemoryHeap* selected_heap = &device_heap;

    switch (heap_usage) {

        case Usage::dynamic:
            if (dynamic_heap.get_memory())
                selected_heap = &dynamic_heap;
            break;

        case Usage::host_only:
            if (host_heap.get_memory())
                selected_heap = &host_heap;
            break;

        case Usage::transient:
            if (transient_heap.get_memory())
                selected_heap = &transient_heap;
            break;

        default:
            break;
    }

    *heap = selected_heap;

    return selected_heap->allocate_memory(requirements, offset, size);
}

bool MemoryAllocator::need_host_copy(Usage heap_usage)
{
    return heap_usage == Usage::fixed && host_heap.get_memory();
}

#ifndef NDEBUG
MemoryAllocator::~MemoryAllocator()
{
    device_heap.print_stats();
    host_heap.print_stats();
    dynamic_heap.print_stats();
    transient_heap.print_stats();
}

void MemoryHeap::print_stats() const
{
    if (heap_size) {
        d_printf("Memory type %u, used %u MB out of %u MB in %s heap\n",
                 memory_type,
                 in_mb(static_cast<VkDeviceSize>(suballoc.get_max_used_size())),
                 in_mb(heap_size),
                 heap_name);
    }
}
#endif
