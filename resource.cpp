// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "resource.h"

#include "d_printf.h"
#include "memory_heap.h"
#include "minivulkan.h"
#include "mstdc.h"

void* Resource::get_raw_ptr() const
{
    return get_raw_ptr(0, 0);
}

void* Resource::get_raw_ptr(VkDeviceSize idx, VkDeviceSize stride) const
{
    const VkDeviceSize offset = idx * stride;
    assert(offset + stride <= alloc_size);

    uint8_t* const ptr = static_cast<uint8_t*>(owning_heap->get_host_ptr());
    return ptr ? (ptr + heap_offset + offset) : ptr;
}

bool Resource::flush_range(VkDeviceSize offset, VkDeviceSize size)
{
    assert(owning_heap);
    assert(offset < alloc_size);
    assert(size <= alloc_size);
    assert(offset + size <= alloc_size);

    if ( ! owning_heap->get_host_ptr())
        return true;

    const VkDeviceSize alignment = vk_phys_props.properties.limits.nonCoherentAtomSize;
    const VkDeviceSize begin     = heap_offset + offset;

    static VkMappedMemoryRange range = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        VK_NULL_HANDLE,     // memory
        0,                  // offset
        0                   // size
    };
    range.memory = owning_heap->get_memory();
    range.offset = mstd::align_down(begin, alignment);
    range.size   = mstd::align_up(size + (begin - range.offset), alignment);

    const VkResult res = CHK(vkFlushMappedMemoryRanges(vk_dev, 1, &range));
    return res == VK_SUCCESS;
}

bool Image::allocate(const ImageInfo& image_info)
{
    const bool host_access = (image_info.heap_usage == Usage::host_only) ||
                             (image_info.heap_usage == Usage::dynamic);

    static VkImageCreateInfo create_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        nullptr,
        0,                  // flags
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_UNDEFINED,
        { 0, 0, 1 },        // extent
        1,                  // mipLevels
        1,                  // arrayLayers
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        0,                  // usage
        VK_SHARING_MODE_EXCLUSIVE,
        1,                  // queueFamilyIndexCount
        &vk_queue_family_index,
        VK_IMAGE_LAYOUT_UNDEFINED
    };
    create_info.format        = image_info.format;
    create_info.extent.width  = image_info.width;
    create_info.extent.height = image_info.height;
    create_info.mipLevels     = image_info.mip_levels;
    create_info.tiling        = host_access ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    create_info.usage         = image_info.usage;

    VkResult res = CHK(vkCreateImage(vk_dev, &create_info, nullptr, &image));
    if (res != VK_SUCCESS)
        return false;

    layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    format     = image_info.format;
    aspect     = image_info.aspect;
    heap_usage = image_info.heap_usage;
    mip_levels = image_info.mip_levels;

    VkMemoryRequirements memory_reqs;
    vkGetImageMemoryRequirements(vk_dev, image, &memory_reqs);

    VkDeviceSize offset;
    MemoryHeap*  heap;
    if ( ! mem_mgr.allocate_memory(memory_reqs, heap_usage, &offset, &heap))
        return false;

#ifndef NDEBUG
    if ( ! heap->check_memory_type(memory_reqs.memoryTypeBits)) {
        d_printf("Device memory does not support requested image type\n");
        return false;
    }
#endif

    res = CHK(vkBindImageMemory(vk_dev, image, heap->get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    if (heap_usage != Usage::host_only) {
        static VkImageViewCreateInfo view_create_info = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0,                  // flags
            VK_NULL_HANDLE,     // image
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_UNDEFINED,
            {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY
            },
            {
                0, // aspectMask
                0, // baseMipLevel
                0, // levelCount
                0, // baseArrayLayer
                1  // layerCount
            }
        };
        view_create_info.image                       = image;
        view_create_info.format                      = format;
        view_create_info.subresourceRange.aspectMask = aspect;
        view_create_info.subresourceRange.levelCount = mip_levels;

        res = CHK(vkCreateImageView(vk_dev, &view_create_info, nullptr, &view));
        if (res != VK_SUCCESS)
            return false;
    }

    owning_heap = heap;
    heap_offset = offset;
    alloc_size  = memory_reqs.size;

    return true;
}

void Image::set_image_layout(VkCommandBuffer buf, const Transition& transition)
{
    static VkImageMemoryBarrier img_barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED
    };

    img_barrier.oldLayout     = layout;
    img_barrier.newLayout     = transition.new_layout;
    img_barrier.srcAccessMask = transition.src_access;
    img_barrier.dstAccessMask = transition.dest_access;

    layout = transition.new_layout;

    img_barrier.srcQueueFamilyIndex         = vk_queue_family_index;
    img_barrier.dstQueueFamilyIndex         = vk_queue_family_index;
    img_barrier.image                       = image;
    img_barrier.subresourceRange.aspectMask = aspect;
    img_barrier.subresourceRange.levelCount = 1;
    img_barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(buf,
                         transition.src_stage,
                         transition.dest_stage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &img_barrier);
}

void Image::destroy()
{
    if (view)
        vkDestroyImageView(vk_dev, view, nullptr);
    if (image)
        vkDestroyImage(vk_dev, image, nullptr);
    mstd::mem_zero(this, sizeof(*this));
}

bool Buffer::allocate(Usage              heap_usage,
                      uint32_t           size,
                      VkFormat           format,
                      VkBufferUsageFlags usage)
{
    static VkBufferCreateInfo create_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,          // flags
        0,          // size
        0,          // usage
        VK_SHARING_MODE_EXCLUSIVE,
        1,          // queueFamilyIndexCount
        &vk_queue_family_index
    };
    create_info.size  = size;
    create_info.usage = usage;

    VkResult res = CHK(vkCreateBuffer(vk_dev, &create_info, nullptr, &buffer));
    if (res != VK_SUCCESS)
        return false;

    VkMemoryRequirements memory_reqs;
    vkGetBufferMemoryRequirements(vk_dev, buffer, &memory_reqs);

    VkDeviceSize offset;
    MemoryHeap*  heap;

    if ( ! mem_mgr.allocate_memory(memory_reqs, heap_usage, &offset, &heap))
        return false;

#ifndef NDEBUG
    if ( ! heap->check_memory_type(memory_reqs.memoryTypeBits)) {
        d_printf("Device memory does not support requested buffer type\n");
        return false;
    }
#endif

    res = CHK(vkBindBufferMemory(vk_dev, buffer, heap->get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    if (heap_usage != Usage::host_only && format != VK_FORMAT_UNDEFINED) {
        static VkBufferViewCreateInfo view_create_info = {
            VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            nullptr,
            0,                      // flags
            VK_NULL_HANDLE,         // buffer
            VK_FORMAT_UNDEFINED,    // format
            0,                      // offset
            VK_WHOLE_SIZE           // range
        };
        view_create_info.buffer = buffer;
        view_create_info.format = format;
        view_create_info.offset = heap_offset;

        res = CHK(vkCreateBufferView(vk_dev, &view_create_info, nullptr, &view));
        if (res != VK_SUCCESS)
            return false;
    }

    owning_heap = heap;
    heap_offset = offset;
    alloc_size  = size;

    return true;
}

void Buffer::cpu_fill(const void* data, uint32_t size)
{
    mstd::mem_copy(get_ptr<void*>(), data, size);
}

bool Buffer::flush()
{
    return flush_range(0, alloc_size);
}

bool Buffer::flush(VkDeviceSize idx, VkDeviceSize stride)
{
    assert(idx * stride + stride <= alloc_size);
    return flush_range(idx * stride, stride);
}
