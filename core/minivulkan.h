// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#pragma once

#include "vulkan_functions.h"
#include "usage.h"
#include <assert.h>
#include <stdint.h>

extern const char                  app_name[];
extern VkInstance                  vk_instance;
extern VkSurfaceKHR                vk_surface;
extern VkPhysicalDevice            vk_phys_dev;
extern VkDevice                    vk_dev;
extern uint32_t                    graphics_family_index;
extern uint32_t                    compute_family_index;
extern VkSwapchainCreateInfoKHR    swapchain_create_info;
extern VkQueue                     vk_graphics_queue;
extern VkQueue                     vk_compute_queue;
extern uint32_t                    vk_num_swapchain_images;
extern VkSurfaceCapabilitiesKHR    vk_surface_caps;
extern VkExtent2D                  vk_window_extent;
extern VkPhysicalDeviceProperties2 vk_phys_props;

static constexpr uint32_t no_queue_family = ~0u;

#define FEATURE_SETS \
    X(_shader_int8_features,   nullptr,                    VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) \
    X(_8b_storage_features,    &vk_shader_int8_features,   VkPhysicalDevice8BitStorageFeatures,       VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)        \
    X(_16b_storage_features,   &vk_8b_storage_features,    VkPhysicalDevice16BitStorageFeatures,      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)       \
    X(_dyn_rendering_features, &vk_16b_storage_features,   VkPhysicalDeviceDynamicRenderingFeatures,  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)   \
    X(_features,               &vk_dyn_rendering_features, VkPhysicalDeviceFeatures2,                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)

#define X(set, prev, type, tag) extern type vk##set;
FEATURE_SETS
#undef X

struct Window;

bool init_vulkan(struct Window* w);
bool init_sound();
bool create_surface(struct Window* w);
bool skip_frame(struct Window* w);
bool need_redraw(struct Window* w);
bool draw_frame();
bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence, uint32_t sem_id);
bool idle_queue();
uint64_t get_current_time_ms();
bool load_sound_track(const void* data, uint32_t size);
bool play_sound_track();

uint32_t check_device_features();
bool init_assets();

#ifdef NDEBUG
inline constexpr void set_vk_object_name(VkObjectType type, uint64_t handle, Description desc) { }
#else
void set_vk_object_name(VkObjectType type, uint64_t handle, Description desc);
#endif

#if !defined(_MSC_VER) || !defined(_M_IX86)
template<typename T>
void set_vk_object_name(VkObjectType type, T handle, Description desc)
{
    set_vk_object_name(type, reinterpret_cast<uint64_t>(handle), desc);
}
#endif

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

PFN_vkVoidFunction load_vk_function(const char* name);

#define VK_FUNCTION(name) static_load_vk_function<PFN_##name>(#name)

template<typename T>
T static_load_vk_function(const char* name)
{
    static T func;
    if ( ! func)
        func = reinterpret_cast<T>(load_vk_function(name));
    return func;
}

static constexpr uint32_t max_swapchain_size = 3;

enum eSemId {
    sem_acquire,
    sem_present,

    num_semaphore_types
};

static constexpr uint32_t num_semaphores = (max_swapchain_size + 1) * num_semaphore_types;

extern VkSemaphore vk_sems[num_semaphores];

enum eFenceId {
    fen_submit,
    fen_copy_to_dev = fen_submit + max_swapchain_size,
    fen_compute,

    num_fences
};

extern VkFence vk_fens[num_fences];

bool wait_and_reset_fence(enum eFenceId fence);

bool find_optimal_tiling_format(const VkFormat* preferred_formats,
                                uint32_t        num_preferred_formats,
                                uint32_t        format_feature_flags,
                                VkFormat*       out_format);

#ifdef __cplusplus

class Image;

bool allocate_depth_buffers(Image (&depth_buffers)[max_swapchain_size], uint32_t num_depth_buffers);

struct CommandBuffersBase {
    VkCommandPool   pool    = VK_NULL_HANDLE;
    VkCommandBuffer bufs[1] = { };
};

bool reset_and_begin_command_buffer(VkCommandBuffer cmd_buf);
bool send_to_device_and_wait(VkCommandBuffer cmd_buf, VkQueue queue, eFenceId fence);

bool allocate_command_buffers(CommandBuffersBase* bufs,
                              uint32_t            num_buffers,
                              uint32_t            queue_family_index = graphics_family_index);

inline bool allocate_command_buffers_once(CommandBuffersBase* bufs,
                                          uint32_t            num_buffers,
                                          uint32_t            queue_family_index = graphics_family_index)
{
    return bufs->pool ? true : allocate_command_buffers(bufs, num_buffers, queue_family_index);
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

    operator VkCommandBuffer() const { return bufs[0]; }
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
extern VkFormat      vk_depth_format;

void configure_viewport_and_scissor(VkViewport* viewport,
                                    VkRect2D*   scissor,
                                    float       image_ratio,
                                    uint32_t    viewport_width,
                                    uint32_t    viewport_height);

void configure_viewport_and_scissor(VkViewport* viewport,
                                    VkRect2D*   scissor,
                                    uint32_t    viewport_width,
                                    uint32_t    viewport_height);

void send_viewport_and_scissor(VkCommandBuffer cmd_buf,
                               float           image_ratio,
                               uint32_t        viewport_width,
                               uint32_t        viewport_height);

void send_viewport_and_scissor(VkCommandBuffer cmd_buf,
                               uint32_t        viewport_width,
                               uint32_t        viewport_height);

VkShaderModule load_shader(uint8_t* shader);

struct DescSetBindingInfo {
    uint8_t set_layout_id;
    uint8_t binding;
    uint8_t desc_type;
    uint8_t desc_count;
};

bool create_compute_descriptor_set_layouts(const DescSetBindingInfo* binding_desc,
                                           uint32_t                  num_layouts,
                                           VkDescriptorSetLayout*    out_layouts);

struct ComputeShaderInfo {
    uint8_t* shader;
    uint8_t  num_push_constants;
};

bool create_compute_shader(const ComputeShaderInfo&     shader_desc,
                           const VkDescriptorSetLayout* desc_set_layouts,
                           VkPipelineLayout*            out_pipe_layout,
                           VkPipeline*                  out_pipe);

#endif // __cplusplus
