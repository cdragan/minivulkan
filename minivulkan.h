// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"
#include <assert.h>
#include <stdint.h>

#define APPNAME "minivulkan"

extern VkInstance               vk_instance;
extern VkSurfaceKHR             vk_surface;
extern VkPhysicalDevice         vk_phys_dev;
extern VkDevice                 vk_dev;
extern uint32_t                 vk_queue_family_index;
extern VkQueue                  vk_queue;
extern uint32_t                 vk_num_swapchain_images;
extern VkRenderPass             vk_render_pass;
extern VkSurfaceCapabilitiesKHR vk_surface_caps;

struct Window;

#if defined(__APPLE__) && defined(__cplusplus)
#   define PORTABLE extern "C"
#else
#   define PORTABLE
#endif

PORTABLE bool init_vulkan(struct Window* w);
PORTABLE bool create_surface(struct Window* w);
PORTABLE bool draw_frame();
PORTABLE void idle_queue();
PORTABLE uint64_t get_current_time_ms();

#ifdef NDEBUG
#   define CHK(call) call
#else
#   define CHK(call) check_vk_call(#call, __FILE__, __LINE__, (call))
PORTABLE VkResult check_vk_call(const char* call_str, const char* file, int line, VkResult res);
#endif

enum eLimits {
    max_swapchain_size = 3
};

enum eSemId {
    sem_acquire,

    num_semaphores
};

extern VkSemaphore vk_sems[num_semaphores];

enum eFenceId {
    fen_submit,
    fen_copy_to_dev = fen_submit + max_swapchain_size,

    num_fences
};

extern VkFence vk_fens[num_fences];

bool wait_and_reset_fence(enum eFenceId fence);

#ifdef __cplusplus

class DeviceMemoryHeap {
    public:
        enum Location {
            device_memory,
            host_memory,
            coherent_memory
        };

        DeviceMemoryHeap() = default;
        DeviceMemoryHeap(Location loc) : memory_location(loc), host_visible(loc == host_memory) { }
        DeviceMemoryHeap(const DeviceMemoryHeap&) = delete;
        DeviceMemoryHeap& operator=(const DeviceMemoryHeap&) = delete;

        bool allocate_heap_once(const VkMemoryRequirements& requirements);
        bool allocate_heap(VkDeviceSize size);
        void free_heap();
        bool allocate_memory(const VkMemoryRequirements& requirements, VkDeviceSize* offset);

        VkDeviceMemory  get_memory() const { return memory; }
        bool            is_host_memory() const { return host_visible; }
        static uint32_t get_memory_type() { return device_memory_type; }

    private:
        static bool init_heap_info();

        static uint32_t device_memory_type;
        static uint32_t host_memory_type;
        static uint32_t coherent_memory_type;

        VkDeviceMemory  memory          = VK_NULL_HANDLE;
        VkDeviceSize    next_free_offs  = 0;
        VkDeviceSize    heap_size       = 0;
        Location        memory_location = device_memory;
        bool            host_visible    = false;
        bool            mapped          = false;

        friend class MapBase;
};

class MapBase {
    public:
        MapBase(const MapBase&) = delete;
        MapBase& operator=(const MapBase&) = delete;

        bool mapped() const { return mapped_ptr != nullptr; }
        void unmap();

    protected:
        MapBase() = default;
        MapBase(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size);
        ~MapBase() { unmap(); }

        void move_from(MapBase& map);
        void* get_ptr() { return mapped_ptr; }
        const void* get_ptr() const { return mapped_ptr; }
        void* get_end_ptr() { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mapped_ptr) + mapped_size); }
        const void* get_end_ptr() const { return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mapped_ptr) + mapped_size); }

    private:
        DeviceMemoryHeap* mapped_heap = nullptr;
        void*             mapped_ptr  = nullptr;
        VkDeviceSize      mapped_size = 0;
};

template<typename T>
class Map: public MapBase {
    public:
        Map() = default;
        Map(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size)
            : MapBase(heap, offset, size) { }

        Map(Map&& map) { move_from(map); }
        Map& operator=(Map&& map) {
            move_from(map);
            return *this;
        }

        uint32_t size() const { return end() - begin(); }

        T* data() { return static_cast<T*>(get_ptr()); }
        const T* data() const { return static_cast<const T*>(get_ptr()); }

        operator T*() { return static_cast<T*>(get_ptr()); }
        operator const T*() const { return static_cast<const T*>(get_ptr()); }

        T& operator[](uintptr_t i) { return static_cast<T*>(get_ptr())[i]; }
        const T& operator[](uintptr_t i) const { return static_cast<const T*>(get_ptr())[i]; }

        T* begin() { return static_cast<T*>(get_ptr()); }
        T* end()   { return static_cast<T*>(get_end_ptr()); }

        const T* begin() const { return static_cast<const T*>(get_ptr()); }
        const T* end()   const { return static_cast<const T*>(get_end_ptr()); }

        const T* cbegin() const { return static_cast<const T*>(get_ptr()); }
        const T* cend()   const { return static_cast<const T*>(get_end_ptr()); }
};

class Resource {
    public:
        Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;

        template<typename T>
        Map<T> map() const { return Map<T>(owning_heap, heap_offset, bytes); }

        bool allocated() const { return !! bytes; }

    protected:
        DeviceMemoryHeap* owning_heap = nullptr;
        VkDeviceSize      heap_offset = 0;
        VkDeviceSize      bytes       = 0;
};

struct ImageInfo {
    uint32_t           width;
    uint32_t           height;
    VkFormat           format;
    uint32_t           mip_levels;
    VkImageAspectFlags aspect;
    VkImageUsageFlags  usage;
};

class Image: public Resource {
    public:
        static constexpr VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageLayout layout = initial_layout;

        Image() = default;

        const VkImage& get_image() const { return image; }
        VkImageView get_view() const { return view; }

        bool allocate(DeviceMemoryHeap& heap, const ImageInfo& image_info);
        void destroy();

        struct Transition {
            VkPipelineStageFlags src_stage;
            VkAccessFlags        src_access;
            VkPipelineStageFlags dest_stage;
            VkAccessFlags        dest_access;
            VkImageLayout        new_layout;
        };

        void set_image_layout(VkCommandBuffer buf, const Transition& transition);

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
        VkImage            image  = VK_NULL_HANDLE;
        VkImageView        view   = VK_NULL_HANDLE;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

class Buffer: public Resource {
    public:
        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        const VkBuffer& get_buffer() const { return buffer; }
        VkBufferView get_view() const { return view; }

        bool allocate(DeviceMemoryHeap&  heap,
                      uint32_t           alloc_size,
                      VkBufferUsageFlags usage);
        bool create_view(VkFormat format);
        void destroy();

    private:
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkBufferView view   = VK_NULL_HANDLE;
};

struct CommandBuffersBase {
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer bufs[1];
};

bool reset_and_begin_command_buffer(VkCommandBuffer cmd_buf);
bool allocate_command_buffers(CommandBuffersBase* bufs, uint32_t num_buffers);

template<uint32_t num_buffers>
struct CommandBuffers: public CommandBuffersBase {
    static constexpr uint32_t size() { return num_buffers; }

    private:
        VkCommandBuffer tail[num_buffers - 1];
};

template<>
struct CommandBuffers<1>: public CommandBuffersBase {
    static constexpr uint32_t size() { return 1; }
};

template<uint32_t num_buffers>
static bool allocate_command_buffers(CommandBuffers<num_buffers>* bufs)
{
    return allocate_command_buffers(bufs, num_buffers);
}

template<uint32_t num_buffers>
static bool allocate_command_buffers_once(CommandBuffers<num_buffers>* bufs)
{
    if (bufs->pool)
        return true;

    return allocate_command_buffers(bufs, num_buffers);
}

class HostFiller {
    public:
        HostFiller() = default;

        bool init(VkDeviceSize heap_size);

        bool fill_buffer(Buffer*            buffer,
                         VkBufferUsageFlags usage,
                         const void*        data,
                         uint32_t           size);

        bool send_to_gpu();
        bool wait_until_done();

    private:
        DeviceMemoryHeap          host_heap{DeviceMemoryHeap::host_memory};
        CommandBuffers<1>         cmd_buf;

        static constexpr uint32_t max_buffers = 4;
        Buffer                    buffers[max_buffers];
        uint32_t                  num_buffers = 0;
};

#endif // __cplusplus
