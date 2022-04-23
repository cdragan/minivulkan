// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "../d_printf.h"
#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

constexpr bool play_new_sound = false;

#ifdef ENABLE_GUI
static float    user_roundedness = 111.0f / 127.0f;
static uint32_t user_tess_level  = 12;
static bool     user_wireframe   = false;

static bool create_gui_frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width);
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height);

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
    d_printf("Synth compiled without GUI, exiting\n");
    return false;
}
#endif

uint32_t check_device_features()
{
    uint32_t missing_features = 0;

    missing_features += check_feature(&vk_8b_storage_features.uniformAndStorageBuffer8BitAccess);
    missing_features += check_feature(&vk_16b_storage_features.uniformAndStorageBuffer16BitAccess);
    missing_features += check_feature(&vk_16b_storage_features.storageBuffer16BitAccess);
    missing_features += check_feature(&vk_shader_int8_features.shaderInt8);
    missing_features += check_feature(&vk_features.features.shaderInt16);

    return missing_features;
}

static constexpr uint32_t coherent_heap_size = 1u * 1024u * 1024u;

static DeviceMemoryHeap vk_coherent_heap{DeviceMemoryHeap::coherent_memory};

bool create_additional_heaps()
{
    return vk_coherent_heap.allocate_heap(coherent_heap_size);
}

enum DescSetLayouts {
    dsl_synth,
    dsl_mono_to_stereo,
    num_dsls
};

enum Pipelines {
    pipe_synth,
    pipe_mono_to_stereo,
    num_pips
};

static VkDescriptorSetLayout vk_descriptor_set_layouts[num_dsls];
static VkPipelineLayout      vk_pipeline_layouts[num_pips];
static VkPipeline            vk_pipelines[num_pips];

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

struct Mono2StereoPushConstants {
    uint32_t num_samples;   // Number of samples to generate in the output sound
};

struct SynthPushConstants: public Mono2StereoPushConstants {
    uint32_t base_freq;     // Frequency of the note being played
};

bool create_pipeline_layouts()
{
    ////////////////////////////////////////////////////////////////////////////
    {
        static const VkDescriptorSetLayoutBinding create_bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };

        static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            0, // flags
            mstd::array_size(create_bindings),
            create_bindings
        };

        const VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev,
                                                             &create_desc_set_layout,
                                                             nullptr,
                                                             &vk_descriptor_set_layouts[dsl_synth]));
        if (res != VK_SUCCESS)
            return false;
    }

    ////////////////////////////////////////////////////////////////////////////
    {
        static const VkDescriptorSetLayoutBinding create_bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };

        static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            0, // flags
            mstd::array_size(create_bindings),
            create_bindings
        };

        const VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev,
                                                             &create_desc_set_layout,
                                                             nullptr,
                                                             &vk_descriptor_set_layouts[dsl_mono_to_stereo]));
        if (res != VK_SUCCESS)
            return false;
    }

    ////////////////////////////////////////////////////////////////////////////
    {
        static const VkPushConstantRange push_constant_range = {
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(SynthPushConstants)
        };

        static VkPipelineLayoutCreateInfo layout_create_info = {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr,
            0,      // flags
            1,
            &vk_descriptor_set_layouts[dsl_synth],
            1,      // pushConstantRangeCount
            &push_constant_range
        };

        const VkResult res = CHK(vkCreatePipelineLayout(vk_dev,
                                                        &layout_create_info,
                                                        nullptr,
                                                        &vk_pipeline_layouts[pipe_synth]));
        if (res != VK_SUCCESS)
            return false;
    }

    ////////////////////////////////////////////////////////////////////////////
    {
        static const VkPushConstantRange push_constant_range = {
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(Mono2StereoPushConstants)
        };

        static VkPipelineLayoutCreateInfo layout_create_info = {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr,
            0,      // flags
            1,
            &vk_descriptor_set_layouts[dsl_mono_to_stereo],
            1,      // pushConstantRangeCount
            &push_constant_range
        };

        const VkResult res = CHK(vkCreatePipelineLayout(vk_dev,
                                                        &layout_create_info,
                                                        nullptr,
                                                        &vk_pipeline_layouts[pipe_mono_to_stereo]));
        if (res != VK_SUCCESS)
            return false;
    }

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
    return create_compute_pipeline(vk_pipeline_layouts[pipe_synth],          shader_synth_comp,          &vk_pipelines[pipe_synth]) &&
           create_compute_pipeline(vk_pipeline_layouts[pipe_mono_to_stereo], shader_mono_to_stereo_comp, &vk_pipelines[pipe_mono_to_stereo]);
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    VkResult res;

    // Allocate descriptor set
    static VkDescriptorSet desc_sets[num_dsls];
    if ( ! desc_sets[0]) {
        static VkDescriptorPoolSize pool_sizes[] = {
            {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,      // type
                1                                       // descriptorCount
            },
            {
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,      // type
                3                                       // descriptorCount
            }
        };

        static VkDescriptorPoolCreateInfo pool_create_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            nullptr,
            0, // flags
            1, // maxSets
            mstd::array_size(pool_sizes),
            pool_sizes
        };

        static VkDescriptorSetAllocateInfo alloc_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            nullptr,
            VK_NULL_HANDLE,             // descriptorPool
            mstd::array_size(vk_descriptor_set_layouts),
            vk_descriptor_set_layouts
        };

        res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &alloc_info.descriptorPool));
        if (res != VK_SUCCESS)
            return false;

        res = CHK(vkAllocateDescriptorSets(vk_dev, &alloc_info, desc_sets));
        if (res != VK_SUCCESS)
            return false;

        // Update descriptor set
        static VkDescriptorBufferInfo uniform_buffer_info = {
            VK_NULL_HANDLE,     // buffer
            0,                  // offset
            0                   // range
        };
        //TODO
        //uniform_buffer_info.buffer = shader_data.get_buffer();
        uniform_buffer_info.range  = sizeof(UboData);

        static VkDescriptorBufferInfo storage_buffer_info = {
            VK_NULL_HANDLE,     // buffer
            0,                  // offset
            0                   // range
        };
        //TODO
        //storage_buffer_info.buffer = shader_data.get_buffer();
        //storage_buffer_info.range  = slot_size;

        static VkWriteDescriptorSet write_desc_sets[] = {
            {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                VK_NULL_HANDLE,                     // dstSet
                0,                                  // dstBinding
                0,                                  // dstArrayElement
                1,                                  // descriptorCount
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  // descriptorType
                nullptr,                            // pImageInfo
                &uniform_buffer_info,               // pBufferInfo
                nullptr                             // pTexelBufferView
            },
            {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                VK_NULL_HANDLE,                     // dstSet
                1,                                  // dstBinding
                0,                                  // dstArrayElement
                1,                                  // descriptorCount
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
                nullptr,                            // pImageInfo
                &storage_buffer_info,               // pBufferInfo
                nullptr                             // pTexelBufferView
            }
        };
        write_desc_sets[0].dstSet = desc_sets[0];
        write_desc_sets[1].dstSet = desc_sets[0];

        vkUpdateDescriptorSets(vk_dev,
                               mstd::array_size(write_desc_sets),
                               write_desc_sets,
                               0,           // descriptorCopyCount
                               nullptr);    // pDescriptorCopies
    }

    // Handle GUI
    if ( ! create_gui_frame())
        return false;

    // Prepare command buffer
    static CommandBuffers<2 * mstd::array_size(vk_swapchain_images)> bufs;

    if ( ! allocate_command_buffers_once(&bufs))
        return false;

    const VkCommandBuffer buf = bufs.bufs[image_idx];

    if ( ! reset_and_begin_command_buffer(buf))
        return false;

    // Synthesize the sound
    if (play_new_sound) {

        // Send compute workload to the device

        constexpr uint32_t work_group_size = 1024;
        constexpr uint32_t sound_length    = 44100; // TODO

        vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipelines[pipe_synth]);

        vkCmdBindDescriptorSets(buf,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                vk_pipeline_layouts[pipe_synth],
                                0,        // firstSet
                                1,        // descriptorSetCount
                                &desc_sets[0],
                                0,        // dynamicOffsetCount
                                nullptr); // pDynamicOffsets

        vkCmdDispatch(buf, sound_length / work_group_size, 1, 1);
    }

    // Draw GUI
    Image& image = vk_swapchain_images[image_idx];

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

    // Finish command buffer
    res = CHK(vkEndCommandBuffer(buf));
    if (res != VK_SUCCESS)
        return false;

    // Submit command buffer to queue
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
