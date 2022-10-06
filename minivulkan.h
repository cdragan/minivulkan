// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"
#include <assert.h>
#include <stdint.h>

#ifndef APPNAME
#   define APPNAME "minivulkan"
#endif

extern VkInstance                  vk_instance;
extern VkSurfaceKHR                vk_surface;
extern VkPhysicalDevice            vk_phys_dev;
extern VkDevice                    vk_dev;
extern uint32_t                    vk_queue_family_index;
extern VkQueue                     vk_queue;
extern uint32_t                    vk_num_swapchain_images;
extern VkRenderPass                vk_render_pass;
extern VkSurfaceCapabilitiesKHR    vk_surface_caps;
extern VkPhysicalDeviceProperties2 vk_phys_props;

#define FEATURE_SETS \
    X(_shader_int8_features, nullptr,                  VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) \
    X(_8b_storage_features,  &vk_shader_int8_features, VkPhysicalDevice8BitStorageFeatures,       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)        \
    X(_16b_storage_features, &vk_8b_storage_features,  VkPhysicalDevice16BitStorageFeatures,      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)       \
    X(_features,             &vk_16b_storage_features, VkPhysicalDeviceFeatures2,                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)

#define X(set, prev, type, tag) extern type vk##set;
FEATURE_SETS
#undef X

struct Window;

bool init_vulkan(struct Window* w);
bool init_sound();
bool create_surface(struct Window* w);
bool draw_frame();
bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence);
VkResult idle_queue();
uint64_t get_current_time_ms();
bool load_sound(uint32_t sound_id, const void* data, uint32_t size);
bool play_sound(uint32_t sound_id);

uint32_t check_device_features();
bool create_additional_heaps();
bool init_assets();

#ifdef NDEBUG
#   define CHK(call) call
#else
#   define CHK(call) check_vk_call(#call, __FILE__, __LINE__, (call))
VkResult check_vk_call(const char* call_str, const char* file, int line, VkResult res);
#endif

#ifdef NDEBUG
uint32_t check_feature(const VkBool32* feature);
#else
#   define check_feature(feature) check_feature_str(#feature, feature)
uint32_t check_feature_str(const char* name, const VkBool32* feature);
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

        constexpr DeviceMemoryHeap() = default;
        constexpr DeviceMemoryHeap(Location loc) : memory_location(loc), host_visible(loc == host_memory) { }
        DeviceMemoryHeap(const DeviceMemoryHeap&) = delete;
        DeviceMemoryHeap& operator=(const DeviceMemoryHeap&) = delete;

        bool allocate_heap_once(const VkMemoryRequirements& requirements);
        bool allocate_heap(VkDeviceSize size);
        void free_heap();
        void reset_heap() { next_free_offs = 0; }
        bool allocate_memory(const VkMemoryRequirements& requirements, VkDeviceSize* offset);

        VkDeviceMemory  get_memory() const { return memory; }
        bool            is_host_memory() const { return host_visible; }
        uint32_t        get_memory_type() const { return vk_memory_type[memory_location]; }
        VkDeviceSize    get_heap_size() const { return heap_size; }

    private:
        static bool init_heap_info();

        static uint32_t vk_memory_type[3];

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
        bool flush(uint32_t offset, uint32_t size);
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
        DeviceMemoryHeap* mapped_heap   = nullptr;
        void*             mapped_ptr    = nullptr;
        VkDeviceSize      mapped_offset = 0;
        VkDeviceSize      mapped_size   = 0;
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
        constexpr Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;

        template<typename T>
        Map<T> map() const { return Map<T>(owning_heap, heap_offset, memory_reqs.size); }

        bool allocated() const { return !! memory_reqs.size; }
        VkDeviceSize size() const { return memory_reqs.size; }
        uint32_t get_memory_type() const { return owning_heap->get_memory_type(); }

    protected:
        DeviceMemoryHeap*    owning_heap = nullptr;
        VkDeviceSize         heap_offset = 0;
        VkMemoryRequirements memory_reqs = { };
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

        constexpr Image() = default;

        const VkImage& get_image() const { return image; }
        VkImageView get_view() const { return view; }

        bool create(const ImageInfo& image_info, VkImageTiling tiling);
        bool allocate(DeviceMemoryHeap& heap);
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
        VkImage            image      = VK_NULL_HANDLE;
        VkImageView        view       = VK_NULL_HANDLE;
        VkFormat           format     = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t           mip_levels = 0;
};

class Buffer: public Resource {
    public:
        constexpr Buffer() = default;
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

bool allocate_depth_buffers(Image (&depth_buffers)[max_swapchain_size], uint32_t num_depth_buffers);

struct CommandBuffersBase {
    VkCommandPool   pool    = VK_NULL_HANDLE;
    VkCommandBuffer bufs[1] = { };
};

bool reset_and_begin_command_buffer(VkCommandBuffer cmd_buf);
bool allocate_command_buffers(CommandBuffersBase* bufs, uint32_t num_buffers);
inline bool allocate_command_buffers_once(CommandBuffersBase* bufs, uint32_t num_buffers)
{
    return bufs->pool ? true : allocate_command_buffers(bufs, num_buffers);
}

template<uint32_t num_buffers>
struct CommandBuffers: public CommandBuffersBase {
    static constexpr uint32_t size() { return num_buffers; }

    private:
        VkCommandBuffer tail[num_buffers - 1] = { };
};

template<>
struct CommandBuffers<1>: public CommandBuffersBase {
    static constexpr uint32_t size() { return 1; }
};

template<uint32_t num_buffers>
inline bool allocate_command_buffers(CommandBuffers<num_buffers>* bufs)
{
    return allocate_command_buffers(bufs, num_buffers);
}

template<uint32_t num_buffers>
inline bool allocate_command_buffers_once(CommandBuffers<num_buffers>* bufs)
{
    if (bufs->pool)
        return true;

    return allocate_command_buffers(bufs, num_buffers);
}

class HostFiller {
    public:
        constexpr HostFiller() = default;

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

inline constexpr VkClearValue make_clear_color(float r, float g, float b, float a)
{
    VkClearValue value = { };
    value.color.float32[0] = r;
    value.color.float32[1] = g;
    value.color.float32[2] = b;
    value.color.float32[3] = a;
    return value;
}

inline constexpr VkClearValue make_clear_depth(float depth, uint32_t stencil)
{
    VkClearValue value = { };
    value.depthStencil.depth   = depth;
    value.depthStencil.stencil = stencil;
    return value;
}

extern Image         vk_swapchain_images[max_swapchain_size];
extern Image         vk_depth_buffers[max_swapchain_size];
extern VkFramebuffer vk_frame_buffers[max_swapchain_size];

void send_viewport_and_scissor(VkCommandBuffer cmd_buf,
                               float           image_ratio,
                               uint32_t        viewport_width,
                               uint32_t        viewport_height);

#endif // __cplusplus
