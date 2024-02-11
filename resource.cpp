// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "resource.h"

#include "d_printf.h"
#include "memory_heap.h"
#include "minivulkan.h"
#include "mstdc.h"

void* Resource::get_raw_ptr() const
{
    return get_raw_ptr(0);
}

void* Resource::get_raw_ptr(VkDeviceSize idx, VkDeviceSize stride) const
{
    const VkDeviceSize offset = idx * stride;
    assert(offset + stride <= alloc_size);

    return get_raw_ptr(offset);
}

void* Resource::get_raw_ptr(VkDeviceSize offset) const
{
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

bool Resource::flush_whole()
{
    return flush_range(0, alloc_size);
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

    if ( ! owning_heap) {
        if ( ! mem_mgr.allocate_memory(memory_reqs, heap_usage, &heap_offset, &owning_heap))
            return false;

        alloc_size = memory_reqs.size;
    }
    else {
        assert(alloc_size >= memory_reqs.size);
    }

#ifndef NDEBUG
    if ( ! owning_heap->check_memory_type(memory_reqs.memoryTypeBits)) {
        d_printf("Device memory does not support requested image type\n");
        return false;
    }
#endif

    res = CHK(vkBindImageMemory(vk_dev, image, owning_heap->get_memory(), heap_offset));
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

    if (host_access) {
        VkSubresourceLayout             subresource_layout;
        static const VkImageSubresource select_subresource = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, // mipLevel
            0  // arrayLayer
        };
        vkGetImageSubresourceLayout(vk_dev, image, &select_subresource, &subresource_layout);

        pitch = static_cast<uint32_t>(subresource_layout.rowPitch);
    }

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

void Image::destroy_and_keep_memory()
{
    MemoryHeap*  heap   = owning_heap;
    VkDeviceSize offset = heap_offset;
    VkDeviceSize size   = alloc_size;

    destroy();

    owning_heap = heap;
    heap_offset = offset;
    alloc_size  = size;
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

bool Buffer::flush(VkDeviceSize idx, VkDeviceSize stride)
{
    assert(idx * stride + stride <= alloc_size);
    return flush_range(idx * stride, stride);
}

bool ImageWithHostCopy::allocate(const ImageInfo& image_info)
{
    if ( ! Image::allocate(image_info))
        return false;

    width  = image_info.width;
    height = image_info.height;

    ImageInfo host_image_info = image_info;

    host_image_info.usage      = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    host_image_info.heap_usage = Usage::host_only;

    return host_image.allocate(host_image_info);
}

bool ImageWithHostCopy::send_to_gpu(VkCommandBuffer cmdbuf)
{
    if ( ! dirty)
        return true;

    if ( ! host_image.flush())
        return false;

    static const Image::Transition transfer_src_layout = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
    };

    static const Image::Transition transfer_dst_layout = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    };

    set_image_layout(cmdbuf, transfer_dst_layout);
    host_image.set_image_layout(cmdbuf, transfer_src_layout);

    static VkImageCopy region = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { },                                    // srcOffset
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { },                                    // dstOffset
        { 0, 0, 1 }                             // extent
    };

    region.extent.width  = width;
    region.extent.height = height;

    vkCmdCopyImage(cmdbuf,
                   host_image.get_image(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   get_image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);

    static const Image::Transition texture_layout = {
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    set_image_layout(cmdbuf, texture_layout);

    dirty = false;

    return true;
}
