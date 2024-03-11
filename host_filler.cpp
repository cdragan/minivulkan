// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "host_filler.h"

#include "memory_heap.h"

#include "d_printf.h"

bool HostFiller::fill_buffer(VkCommandBuffer    cmd_buf,
                             Buffer*            buffer,
                             Usage              heap_usage,
                             VkFormat           format,
                             VkBufferUsageFlags usage,
                             const void*        data,
                             uint32_t           size)
{
    const bool need_host_copy = mem_mgr.need_host_copy(heap_usage);

    if ( ! buffer->allocate(heap_usage,
                            size,
                            format,
                            usage,
                            "host filler buffer"))
        return false;

    if ( ! need_host_copy) {
        buffer->cpu_fill(data, size);

        return buffer->flush();
    }

    assert(num_buffers < max_buffers);

    if (num_buffers == max_buffers) {
        d_printf("Maximum number of host buffers exceeded\n");
        return false;
    }

    Buffer& host_buffer = buffers[num_buffers++];

    if ( ! host_buffer.allocate(Usage::host_only,
                                size,
                                VK_FORMAT_UNDEFINED,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                "host filler host buffer"))
        return false;

    host_buffer.cpu_fill(data, size);

    if ( ! host_buffer.flush())
        return false;

    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    copy_region.size = size;

    vkCmdCopyBuffer(cmd_buf, host_buffer.get_buffer(), buffer->get_buffer(), 1, &copy_region);

    return true;
}
