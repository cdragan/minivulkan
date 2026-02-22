// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "usage.h"

#include "vulkan_functions.h"

#include <assert.h>

class MemoryHeap;

class Resource {
    public:
        constexpr Resource()                 = default;
        Resource(const Resource&)            = delete;
        Resource& operator=(const Resource&) = delete;

        bool         allocated() const { return !! alloc_size; }
        VkDeviceSize size()      const { return alloc_size; }

        template<typename T>
        T* get_ptr() {
            assert(sizeof(T) <= alloc_size);
            return static_cast<T*>(get_raw_ptr());
        }

        template<typename T>
        const T* get_ptr() const {
            assert(sizeof(T) <= alloc_size);
            return static_cast<const T*>(get_raw_ptr());
        }

        template<typename T>
        T* get_ptr(VkDeviceSize offset) {
            assert(offset + sizeof(T) <= alloc_size);
            return static_cast<T*>(get_raw_ptr(offset));
        }

        template<typename T>
        const T* get_ptr(VkDeviceSize offset) const {
            assert(offset + sizeof(T) <= alloc_size);
            return static_cast<const T*>(get_raw_ptr(offset));
        }

        template<typename T>
        T* get_ptr(VkDeviceSize idx, VkDeviceSize stride) {
            assert(sizeof(T) <= stride);
            return static_cast<T*>(get_raw_ptr(idx, stride));
        }

        template<typename T>
        const T* get_ptr(VkDeviceSize idx, VkDeviceSize stride) const {
            assert(sizeof(T) <= stride);
            return static_cast<const T*>(get_raw_ptr(idx, stride));
        }

    protected:
        void* get_raw_ptr() const;
        void* get_raw_ptr(VkDeviceSize idx, VkDeviceSize stride) const;
        void* get_raw_ptr(VkDeviceSize offset) const;
        bool flush_range(VkDeviceSize offset, VkDeviceSize size);
        bool flush_whole();
        bool invalidate_whole();

        MemoryHeap*  owning_heap = nullptr;
        VkDeviceSize heap_offset = 0;
        VkDeviceSize alloc_size  = 0;
};

struct ImageInfo {
    uint32_t           width;
    uint32_t           height;
    VkFormat           format;
    uint32_t           mip_levels;
    VkImageAspectFlags aspect;
    VkImageUsageFlags  usage;
    Usage              heap_usage;
};

class Image: public Resource {
    public:
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

        constexpr Image()              = default;
        Image(const Image&)            = delete;
        Image& operator=(const Image&) = delete;

        const VkImage& get_image() const { return image; }
        VkImageView    get_view()  const { return view; }
        uint32_t       get_pitch() const { return pitch; }

        bool allocate(const ImageInfo& image_info, Description desc);
        bool flush() { return flush_whole(); }
        void free();

        struct Transition {
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2        src_access;
            VkPipelineStageFlags2 dest_stage;
            VkAccessFlags2        dest_access;
            VkImageLayout         new_layout;
        };

        void barrier(const Transition& transition);

        // Used with swapchains
        void set_image(VkImage new_image) {
            assert(image == VK_NULL_HANDLE);
            image  = new_image;
            aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        void set_view(VkImageView new_view) {
            assert(view == VK_NULL_HANDLE);
            view = new_view;
        }

    private:
        VkImage            image      = VK_NULL_HANDLE;
        VkImageView        view       = VK_NULL_HANDLE;
        VkFormat           format     = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
        Usage              heap_usage = Usage::fixed;
        uint32_t           mip_levels = 0;
        uint32_t           pitch      = 0;
};

enum {
    VK_BUFFER_USAGE_ASYNC_COMPUTE_BIT = 0x02000000
};

class Buffer: public Resource {
    public:
        constexpr Buffer()               = default;
        Buffer(const Buffer&)            = delete;
        Buffer& operator=(const Buffer&) = delete;

        const VkBuffer& get_buffer() const { return buffer; }
        VkBufferView    get_view()   const { return view; }

        bool allocate(Usage              heap_usage,
                      uint32_t           size,
                      VkFormat           format,
                      VkBufferUsageFlags usage,
                      Description        desc);
        void cpu_fill(const void* data, uint32_t size);
        bool flush() { return flush_whole(); }
        bool flush(VkDeviceSize idx, VkDeviceSize stride);
        bool invalidate() { return invalidate_whole(); }
        void free(); // GUI only

        struct Transition {
            VkPipelineStageFlags2 src_stage;
            VkAccessFlags2        src_access;
            VkPipelineStageFlags2 dest_stage;
            VkAccessFlags2        dest_access;
        };

        void barrier(const Transition& transition);

    private:
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkBufferView view   = VK_NULL_HANDLE;
};

struct ImageWithHostCopy: public Image {
    public:
        constexpr ImageWithHostCopy()                          = default;
        ImageWithHostCopy(const ImageWithHostCopy&)            = delete;
        ImageWithHostCopy& operator=(const ImageWithHostCopy&) = delete;

        bool allocate(const ImageInfo& image_info, Description desc);

        const Image& get_host_image() const { return host_image; }
        Image& get_host_image() { dirty = true; return host_image; }

        bool send_to_gpu(VkCommandBuffer cmdbuf);

        uint32_t get_width()  const { return width; }
        uint32_t get_height() const { return height; }

    private:
        Image    host_image;
        uint32_t width  = 0;
        uint32_t height = 0;
        bool     dirty  = false;
};
