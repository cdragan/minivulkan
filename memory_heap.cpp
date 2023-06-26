// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "memory_heap.h"
#include "minivulkan.h"

#include "d_printf.h"
#include "mstdc.h"

#include <assert.h>

MemoryAllocator mem_mgr;

static VkPhysicalDeviceMemoryProperties vk_mem_props;

static unsigned in_mb(VkDeviceSize size)
{
    constexpr VkDeviceSize one_mb = 1024u * 1024u;
    return static_cast<unsigned>(mstd::align_up(size, one_mb) / one_mb);
}

bool MemoryHeap::allocate_heap(int req_memory_type, VkDeviceSize size)
{
    assert(req_memory_type >= 0);
    assert(req_memory_type <  static_cast<int>(mstd::array_size(vk_mem_props.memoryTypes)));
    assert(memory          == VK_NULL_HANDLE);
    assert(host_ptr        == nullptr);
    assert(next_free_offs  == 0);
    assert(last_free_offs  == 0);
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

    heap_size       = size;
    last_free_offs  = size;
    memory_type     = static_cast<uint32_t>(req_memory_type);
#ifndef NDEBUG
    lowest_end_offs = size;
#endif

    d_printf("Allocated heap size 0x%" PRIx64 " bytes (%u MB) with memory type %d\n",
             static_cast<uint64_t>(size), in_mb(size), req_memory_type);

    return true;
}

bool MemoryHeap::allocate_memory(const VkMemoryRequirements& requirements,
                                 Placement                   placement,
                                 VkDeviceSize*               offset)
{
    assert(next_free_offs <= last_free_offs);
    assert(last_free_offs <= heap_size);

    const VkDeviceSize alignment = requirements.alignment;

    const VkDeviceSize aligned_offs = (placement == Placement::front)
             ? mstd::align_up(next_free_offs, alignment)
             : mstd::align_down(last_free_offs - requirements.size, alignment);

    const VkDeviceSize end_offs = aligned_offs + requirements.size;

    assert(aligned_offs >= next_free_offs);
    assert(aligned_offs % alignment == 0);

    if (requirements.size > last_free_offs - next_free_offs ||
        aligned_offs < next_free_offs ||
        end_offs > last_free_offs) {

        d_printf("Not enough device memory\n");
        d_printf("Requested surface size 0x%" PRIx64 ", used heap size 0x%" PRIx64 ", total heap size 0x%" PRIx64 "\n",
                static_cast<uint64_t>(requirements.size),
                static_cast<uint64_t>(heap_size - last_free_offs + next_free_offs),
                static_cast<uint64_t>(heap_size));
        return false;
    }

    *offset = aligned_offs;

    if (placement == Placement::front)
        next_free_offs = end_offs;
    else
        last_free_offs = aligned_offs;

    return true;
}

void MemoryHeap::restore_checkpoint(VkDeviceSize low_checkpoint, VkDeviceSize high_checkpoint)
{
    assert(low_checkpoint > high_checkpoint);
    assert(low_checkpoint <= heap_size);
    assert(last_free_offs == high_checkpoint);

#ifndef NDEBUG
    lowest_end_offs = mstd::min(lowest_end_offs, last_free_offs);
#endif

    last_free_offs = low_checkpoint;
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
                                 VkDeviceSize dynamic_heap_size)
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

    const int device_type_index  = find_mem_type(preferred_device_heap_flags,  allow_device_memory);
    int       host_type_index    = find_mem_type(preferred_host_heap_flags,    require_host_memory);
    const int dynamic_type_index = find_mem_type(preferred_dynamic_heap_flags, allow_device_memory);

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

    if ( ! device_heap.allocate_heap(device_type_index, device_heap_size))
        return false;

    if (host_type_index < 0)
        host_type_index = dynamic_type_index;

    if ( ! host_heap.allocate_heap(host_type_index, host_heap_size))
        return false;

    return true;
}

bool MemoryAllocator::allocate_memory(const VkMemoryRequirements& requirements,
                                      Usage                       heap_usage,
                                      VkDeviceSize*               offset,
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

        default:
            break;
    }

    *heap = selected_heap;

    using Placement = MemoryHeap::Placement;
    const Placement placement = (heap_usage == Usage::device_temporary) ? Placement::back : Placement::front;

    return selected_heap->allocate_memory(requirements, placement, offset);
}

bool MemoryAllocator::need_host_copy(Usage heap_usage)
{
    return heap_usage == Usage::fixed && host_heap.get_memory();
}

#ifndef NDEBUG
MemoryAllocator::~MemoryAllocator()
{
    device_heap.print_stats("device");
    host_heap.print_stats("host");
    dynamic_heap.print_stats("dynamic");
}

void MemoryHeap::print_stats(const char* heap_name) const
{
    if (heap_size) {
        const VkDeviceSize max_top_alloc_size = heap_size - mstd::min(lowest_end_offs, last_free_offs);
        d_printf("Memory type %u, used %u MB out of %u MB, bottom %u MB, top %u MB in %s heap\n",
                 memory_type,
                 in_mb(next_free_offs + max_top_alloc_size),
                 in_mb(heap_size),
                 in_mb(next_free_offs),
                 in_mb(max_top_alloc_size),
                 heap_name);
    }
}
#endif
