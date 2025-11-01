// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "resource.h"

#include "barrier.h"
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

bool Resource::invalidate_whole()
{
    assert(owning_heap);

    if ( ! owning_heap->get_host_ptr())
        return true;

    const VkDeviceSize alignment = vk_phys_props.properties.limits.nonCoherentAtomSize;
    const VkDeviceSize begin     = heap_offset;

    static VkMappedMemoryRange range = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        VK_NULL_HANDLE,
        0,
        0
    };
    range.memory = owning_heap->get_memory();
    range.offset = mstd::align_down(begin, alignment);
    range.size   = mstd::align_up(alloc_size, alignment);

    const VkResult res = CHK(vkInvalidateMappedMemoryRanges(vk_dev, 1, &range));
    return res;
}

bool Image::allocate(const ImageInfo& image_info, Description desc)
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
        &graphics_family_index,
        VK_IMAGE_LAYOUT_UNDEFINED
    };
    create_info.format        = image_info.format;
    create_info.extent.width  = image_info.width;
    create_info.extent.height = image_info.height;
    create_info.mipLevels     = image_info.mip_levels;
    create_info.tiling        = host_access ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    create_info.usage         = image_info.usage;

    if (image_info.heap_usage == Usage::transient && mem_mgr.has_transient_heap())
        create_info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;

    VkResult res = CHK(vkCreateImage(vk_dev, &create_info, nullptr, &image));
    if (res != VK_SUCCESS)
        return false;

    set_vk_object_name(VK_OBJECT_TYPE_IMAGE, image, desc);

    layout     = VK_IMAGE_LAYOUT_UNDEFINED;
    format     = image_info.format;
    aspect     = image_info.aspect;
    heap_usage = image_info.heap_usage;
    mip_levels = image_info.mip_levels;

    VkMemoryRequirements memory_reqs;
    vkGetImageMemoryRequirements(vk_dev, image, &memory_reqs);

    if ( ! owning_heap) {
        if ( ! mem_mgr.allocate_memory(memory_reqs, heap_usage, &heap_offset, &alloc_size, &owning_heap))
            return false;
    }
    else {
        assert(alloc_size >= memory_reqs.size);
    }

#ifndef NDEBUG
    if ( ! owning_heap->check_memory_type(memory_reqs.memoryTypeBits)) {
        d_printf("Device memory does not support requested image type for %s %u\n",
                 desc.name, desc.idx);
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

void Image::barrier(const Transition& transition)
{
    static VkImageMemoryBarrier2 barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        nullptr,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED,
        0,
        0,
        VK_NULL_HANDLE,
        { }
    };

    barrier.srcStageMask                = transition.src_stage;
    barrier.srcAccessMask               = transition.src_access;
    barrier.dstStageMask                = transition.dest_stage;
    barrier.dstAccessMask               = transition.dest_access;
    barrier.oldLayout                   = layout;
    barrier.newLayout                   = transition.new_layout;
    barrier.srcQueueFamilyIndex         = graphics_family_index;
    barrier.dstQueueFamilyIndex         = graphics_family_index;
    barrier.image                       = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    add_barrier(barrier);

    layout = transition.new_layout;
}

void Image::free()
{
    if (view)
        vkDestroyImageView(vk_dev, view, nullptr);

    if (image)
        vkDestroyImage(vk_dev, image, nullptr);

    if (alloc_size)
        owning_heap->free_memory(heap_offset, alloc_size);

    mstd::mem_zero(this, sizeof(*this));
}

bool Buffer::allocate(Usage              heap_usage,
                      uint32_t           size,
                      VkFormat           format,
                      VkBufferUsageFlags usage,
                      Description        desc)
{
    const bool compute = !! (usage & VK_BUFFER_USAGE_ASYNC_COMPUTE_BIT);
    usage &= ~static_cast<uint32_t>(VK_BUFFER_USAGE_ASYNC_COMPUTE_BIT);

    static VkBufferCreateInfo create_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,          // flags
        0,          // size
        0,          // usage
        VK_SHARING_MODE_EXCLUSIVE,
        1,          // queueFamilyIndexCount
        nullptr
    };
    create_info.size  = size;
    create_info.usage = usage;

    create_info.pQueueFamilyIndices = compute ? &compute_family_index
                                              : &graphics_family_index;

    VkResult res = CHK(vkCreateBuffer(vk_dev, &create_info, nullptr, &buffer));
    if (res != VK_SUCCESS)
        return false;

    set_vk_object_name(VK_OBJECT_TYPE_BUFFER, buffer, desc);

    VkMemoryRequirements memory_reqs;
    vkGetBufferMemoryRequirements(vk_dev, buffer, &memory_reqs);

    VkDeviceSize offset;
    VkDeviceSize actual_size;
    MemoryHeap*  heap;

    if ( ! mem_mgr.allocate_memory(memory_reqs, heap_usage, &offset, &actual_size, &heap))
        return false;

#ifndef NDEBUG
    if ( ! heap->check_memory_type(memory_reqs.memoryTypeBits)) {
        d_printf("Device memory does not support requested buffer type for %s %u\n",
                 desc.name, desc.idx);
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
    alloc_size  = actual_size;

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

void Buffer::barrier(const Transition& transition)
{
    static VkBufferMemoryBarrier2 barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        nullptr,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        0,
        0,
        VK_NULL_HANDLE,
        0,
        VK_WHOLE_SIZE
    };

    barrier.srcStageMask        = transition.src_stage;
    barrier.srcAccessMask       = transition.src_access;
    barrier.dstStageMask        = transition.dest_stage;
    barrier.dstAccessMask       = transition.dest_access;
    barrier.srcQueueFamilyIndex = graphics_family_index; // TODO pass in arg
    barrier.dstQueueFamilyIndex = graphics_family_index; // TODO pass in arg
    barrier.buffer              = buffer;

    add_barrier(barrier);
}

bool ImageWithHostCopy::allocate(const ImageInfo& image_info, Description desc)
{
    if ( ! Image::allocate(image_info, desc))
        return false;

    width  = image_info.width;
    height = image_info.height;

    ImageInfo host_image_info = image_info;

    host_image_info.usage      = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    host_image_info.heap_usage = Usage::host_only;

    return host_image.allocate(host_image_info, desc);
}

bool ImageWithHostCopy::send_to_gpu(VkCommandBuffer cmdbuf)
{
    if ( ! dirty)
        return true;

    if ( ! host_image.flush())
        return false;

    static const Image::Transition transfer_src_layout = {
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
    };

    static const Image::Transition transfer_dst_layout = {
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    };

    barrier(transfer_dst_layout);
    host_image.barrier(transfer_src_layout);
    send_barrier(cmdbuf);

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
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    barrier(texture_layout);
    send_barrier(cmdbuf);

    dirty = false;

    return true;
}
