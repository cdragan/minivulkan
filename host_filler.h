// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "resource.h"

class HostFiller {
    public:
        constexpr HostFiller() = default;

        bool fill_buffer(VkCommandBuffer    cmd_buf,
                         Buffer*            buffer,
                         Usage              heap_usage,
                         VkFormat           format,
                         VkBufferUsageFlags usage,
                         const void*        data,
                         uint32_t           size);

    private:
        static constexpr uint32_t max_buffers = 4;
        Buffer                    buffers[max_buffers];
        uint32_t                  num_buffers = 0;
};
