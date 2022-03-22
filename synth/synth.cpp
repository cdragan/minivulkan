// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "../dprintf.h"
#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#ifdef ENABLE_GUI
static float    user_roundedness = 111.0f / 127.0f;
static uint32_t user_tess_level  = 12;
static bool     user_wireframe   = false;

static bool create_gui_frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = vk_surface_caps.currentExtent.width;
    io.DisplaySize.y = vk_surface_caps.currentExtent.height;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    ImGui::Text("Hello, world!");
    ImGui::Separator();
    ImGui::SliderFloat("Roundedness", &user_roundedness, 0.0f, 1.0f);
    ImGui::SliderInt("Tessellation Level", reinterpret_cast<int*>(&user_tess_level), 1, 20);
    ImGui::Checkbox("Wireframe", &user_wireframe);
    return true;
}
#else
static bool create_gui_frame()
{
    // The synth is useless without the GUI
    dprintf("Synth compiled without GUI, exiting\n");
    return false;
}
#endif

static constexpr uint32_t coherent_heap_size = 1u * 1024u * 1024u;

static DeviceMemoryHeap vk_coherent_heap{DeviceMemoryHeap::coherent_memory};

bool create_additional_heaps()
{
    return vk_coherent_heap.allocate_heap(coherent_heap_size);
}

static VkPipelineLayout vk_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline       vk_pipeline        = VK_NULL_HANDLE;

static VkDescriptorSetLayout vk_desc_set_layout = VK_NULL_HANDLE;

// Interface to the synth compute shader
enum WaveType {
    wave_sine,
    wave_triangle,
    wave_sawtooth,
    wave_square,
    wave_noise
};

struct Component {
    uint8_t  freq_mult;     // Base frequency multiplier
    uint8_t  wave_type;     // See wave_* constants
    uint16_t delay_us;      // Delay after which this component starts playing
    uint8_t  amplitude_lfo;
    uint8_t  freq_lfo;
    uint8_t  amplitude_env;
    uint8_t  freq_env;
};

struct LFO {
    uint8_t  wave_type;     // See wave_* constants
    uint8_t  dummy1;
    uint16_t period_ms;     // 0 means that LFO is disabled
    uint16_t peak_delta;
    uint16_t dummy2;
};

struct ADSR {
    uint16_t init_value;
    uint16_t max_value;
    uint16_t sustain_value;
    uint16_t end_value;
    uint16_t attack_ms;
    uint16_t decay_ms;
    uint16_t dummy;
    uint16_t release_ms;
};

constexpr uint32_t max_components = 32;
constexpr uint32_t max_lfos       = 4;

struct UboData {
    LFO       lfo[max_lfos];
    ADSR      envelope[max_lfos];
    Component comps[max_components];
    uint32_t  num_comps;
};

struct PushConstants {
    uint32_t base_freq;     // Frequency of the note being played
    uint32_t duration_ms;   // Entire duration of the note, including release
};

bool create_pipeline_layouts()
{
    static const VkDescriptorSetLayoutBinding create_bindings[] = {
        // UboData
        {
            0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            1,
            VK_SHADER_STAGE_COMPUTE_BIT,
            nullptr
        },
        // Buffer of floats
        {
            1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            1,
            VK_SHADER_STAGE_COMPUTE_BIT,
            nullptr
        }
    };

    static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0, // flags
        mstd::array_size(create_bindings),
        create_bindings
    };

    VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_desc_set_layout, nullptr, &vk_desc_set_layout));
    if (res != VK_SUCCESS)
        return false;

    static const VkPushConstantRange push_constant_range = {
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(PushConstants)
    };

    static VkPipelineLayoutCreateInfo layout_create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,      // flags
        1,
        &vk_desc_set_layout,
        1,      // pushConstantRangeCount
        &push_constant_range
    };

    res = CHK(vkCreatePipelineLayout(vk_dev, &layout_create_info, nullptr, &vk_pipeline_layout));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

static bool create_compute_pipeline(VkPipelineLayout layout, uint8_t* shader, VkPipeline* pipeline)
{
    if (*pipeline != VK_NULL_HANDLE)
        return true;

    static VkComputePipelineCreateInfo pipeline_create_info = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        nullptr,
        0,              // flags
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_COMPUTE_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        VK_NULL_HANDLE, // layout
        VK_NULL_HANDLE, // basePipelineHandle
        -1              // basePipelineIndex
    };

    pipeline_create_info.layout       = layout;
    pipeline_create_info.stage.module = load_shader(shader);

    const VkResult res = CHK(vkCreateComputePipelines(vk_dev,
                                                      VK_NULL_HANDLE,
                                                      1,
                                                      &pipeline_create_info,
                                                      nullptr,
                                                      pipeline));
    return res == VK_SUCCESS;
}

bool create_pipelines()
{
    return create_compute_pipeline(vk_pipeline_layout, shader_synth_comp, &vk_pipeline);
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    VkResult res;

    if ( ! create_gui_frame())
        return false;

    // Regenerate sound if needed and draw GUI
    Image& image = vk_swapchain_images[image_idx];

    static CommandBuffers<2 * mstd::array_size(vk_swapchain_images)> bufs;

    if ( ! allocate_command_buffers_once(&bufs))
        return false;

    const VkCommandBuffer buf = bufs.bufs[image_idx];

    if ( ! reset_and_begin_command_buffer(buf))
        return false;

    static const Image::Transition color_att_init = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    image.set_image_layout(buf, color_att_init);

    if (vk_depth_buffers[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        static const Image::Transition depth_init = {
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };

        vk_depth_buffers[image_idx].set_image_layout(buf, depth_init);
    }

    static const VkClearValue clear_values[2] = {
        make_clear_color(0, 0, 0, 0),
        make_clear_depth(0, 0)
    };

    // TODO create own render pass without depth buffer
    static VkRenderPassBeginInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr,
        VK_NULL_HANDLE,     // renderPass
        VK_NULL_HANDLE,     // framebuffer
        { },
        mstd::array_size(clear_values),
        clear_values
    };

    render_pass_info.renderPass        = vk_render_pass;
    render_pass_info.framebuffer       = vk_frame_buffers[image_idx];
    render_pass_info.renderArea.extent = vk_surface_caps.currentExtent;

    vkCmdBeginRenderPass(buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    if ( ! send_gui_to_gpu(buf))
        return false;

    vkCmdEndRenderPass(buf);

    static const Image::Transition color_att_present = {
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    image.set_image_layout(buf, color_att_present);

    res = CHK(vkEndCommandBuffer(buf));
    if (res != VK_SUCCESS)
        return false;

    static const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        1,                      // waitSemaphoreCount
        &vk_sems[sem_acquire],  // pWaitSemaphores
        &dst_stage,             // pWaitDstStageMask
        1,                      // commandBufferCount
        nullptr,                // pCommandBuffers
        1,                      // signalSemaphoreCount
        &vk_sems[sem_acquire]   // pSignalSemaphores
    };

    submit_info.pCommandBuffers = &buf;

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, queue_fence));
    if (res != VK_SUCCESS)
        return false;

    return true;
}
