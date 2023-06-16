// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "sculptor_geometry.h"
#include "sculptor_materials.h"

#include "../d_printf.h"
#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

const char app_name[] = "Object Editor";

const int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                           | ImGuiConfigFlags_DockingEnable;

struct Viewport {
    const char*     name;
    bool            enabled;
    uint32_t        width;
    uint32_t        height;
    Image           color_buffer[max_swapchain_size];
    Image           depth_buffer[max_swapchain_size];
    VkDescriptorSet gui_tex[max_swapchain_size];
};

static Viewport viewports[] = {
    { "Front View", true },
    { "3D View",    true }
};

const unsigned gui_num_descriptors = mstd::array_size(viewports) * max_swapchain_size;

static bool viewports_changed = true;

static VkSampler viewport_sampler;

uint32_t check_device_features()
{
    return 0;
}

static VkDescriptorSetLayout vk_per_frame_desc_set_layout = VK_NULL_HANDLE;
static VkPipelineLayout      vk_gr_pipeline_layout        = VK_NULL_HANDLE;

static bool create_pipeline_layouts()
{
    static const VkDescriptorSetLayoutBinding create_binding = {
        0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        1,
        VK_SHADER_STAGE_VERTEX_BIT
            | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
            | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
            | VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
    };

    static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0, // flags
        1,
        &create_binding
    };

    VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_desc_set_layout, nullptr, &vk_per_frame_desc_set_layout));
    if (res != VK_SUCCESS)
        return false;

    static VkPushConstantRange push_constants = {
        VK_SHADER_STAGE_VERTEX_BIT
            | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
        0,
        // One 4x4 matrix of floats - model-view matrix
        // One 3x3 matrix of floats - inverse-transpose of model-view matrix, for normals
        100
    };

    static VkPipelineLayoutCreateInfo layout_create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,      // flags
        1,
        &vk_per_frame_desc_set_layout,
        1,      // pushConstantRangeCount
        &push_constants
    };

    res = CHK(vkCreatePipelineLayout(vk_dev, &layout_create_info, nullptr, &vk_gr_pipeline_layout));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

static bool create_samplers()
{
    static VkSamplerCreateInfo sampler_info = {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        nullptr,
        0,                                          // flags
        VK_FILTER_NEAREST,                          // magFilter
        VK_FILTER_NEAREST,                          // minFilter
        VK_SAMPLER_MIPMAP_MODE_NEAREST,             // mipmapMode
        VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeU
        VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeV
        VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeW
        0,                                          // mipLodBias
        VK_FALSE,                                   // anisotropyEnble
        0,                                          // maxAnisotropy
        VK_FALSE,                                   // compareEnable
        VK_COMPARE_OP_NEVER,                        // compareOp
        0,                                          // minLod
        0,                                          // maxLod
        VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,    // borderColor
        VK_FALSE                                    // unnormailzedCoordinates
    };

    VkResult res = CHK(vkCreateSampler(vk_dev, &sampler_info, nullptr, &viewport_sampler));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

bool init_assets()
{
    if ( ! create_pipeline_layouts())
        return false;

    if ( ! create_samplers())
        return false;

    if ( ! create_material_layouts())
        return false;

    return true;
}

static bool destroy_viewports()
{
    if ( ! idle_queue())
        return false;

    for (Viewport& viewport : viewports) {
        for (uint32_t i_img = 0; i_img < max_swapchain_size; i_img++) {
            viewport.color_buffer[i_img].destroy();
            viewport.depth_buffer[i_img].destroy();
        }

        viewport.width  = 0;
        viewport.height = 0;
    }

    return true;
}

static bool allocate_viewports()
{
    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        for (uint32_t i_img = 0; i_img < vk_num_swapchain_images; i_img++) {
            if (viewport.color_buffer[i_img].get_view())
                continue;

            static ImageInfo color_info {
                0, // width
                0, // height
                VK_FORMAT_UNDEFINED,
                1, // mip_levels
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                Usage::device_temporary
            };

            color_info.width  = viewport.width;
            color_info.height = viewport.height;
            color_info.format = swapchain_create_info.imageFormat;

            static ImageInfo depth_info {
                0, // width
                0, // height
                VK_FORMAT_UNDEFINED,
                1, // mip_levels
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                Usage::device_temporary
            };

            depth_info.width  = viewport.width;
            depth_info.height = viewport.height;
            depth_info.format = vk_depth_format;

            if ( ! viewport.color_buffer[i_img].allocate(color_info))
                return false;

            if ( ! viewport.depth_buffer[i_img].allocate(depth_info))
                return false;

            if (viewport.gui_tex[i_img]) {

                static VkDescriptorImageInfo image_info = {
                    VK_NULL_HANDLE,
                    VK_NULL_HANDLE,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                };

                static VkWriteDescriptorSet write_desc = {
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    nullptr,
                    VK_NULL_HANDLE,     // dstSet
                    0,                  // dstBinding
                    0,                  // dstArrayElement
                    1,                  // descriptorCount
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    &image_info,
                    nullptr,            // pBufferInfo
                    nullptr             // pTexelBufferView
                };

                image_info.sampler   = viewport_sampler;
                image_info.imageView = viewport.color_buffer[i_img].get_view();
                write_desc.dstSet    = viewport.gui_tex[i_img];

                vkUpdateDescriptorSets(vk_dev, 1, &write_desc, 0, nullptr);
            }
            else {
                viewport.gui_tex[i_img] = ImGui_ImplVulkan_AddTexture(
                        viewport_sampler,
                        viewport.color_buffer[i_img].get_view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    return true;
}

static bool create_gui_frame(uint32_t image_idx)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width)  / vk_surface_scale;
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height) / vk_surface_scale;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {
            }
            if (ImGui::MenuItem("Open", "CTRL+O")) {
            }
            if (ImGui::MenuItem("Save", "CTRL+S")) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "CTRL+Z")) {
            }
            if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "CTRL+X")) {
            }
            if (ImGui::MenuItem("Copy", "CTRL+C")) {
            }
            if (ImGui::MenuItem("Paste", "CTRL+V")) {
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::Begin("Hello, Window!");
    {
        const ImVec2 win_size = ImGui::GetWindowSize();
        ImGui::Text("Window Size: %d x %d", static_cast<int>(win_size.x), static_cast<int>(win_size.y));
        const ImVec2 vp_size = ImGui::GetMainViewport()->Size;
        ImGui::Text("Viewport Size: %d x %d", static_cast<int>(vp_size.x), static_cast<int>(vp_size.y));
        ImGui::Text("Surface Size: %u x %u", vk_surface_caps.currentExtent.width, vk_surface_caps.currentExtent.height);

        ImGui::Separator();

        for (uint32_t i = 0; i < mstd::array_size(viewports); i++)
            ImGui::Checkbox(viewports[i].name, &viewports[i].enabled);
    }
    ImGui::End();

    if (viewports_changed)
        if ( ! destroy_viewports())
            return false;
    viewports_changed = false;

    for (uint32_t i = 0; i < mstd::array_size(viewports); i++) {
        if ( ! viewports[i].enabled)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        ImGui::Begin(viewports[i].name, &viewports[i].enabled, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        {
            const ImVec2 content_size = ImGui::GetContentRegionAvail();

            viewports[i].width  = static_cast<uint32_t>(content_size.x);
            viewports[i].height = static_cast<uint32_t>(content_size.y);

            if ((content_size.x != viewports[i].width) || (content_size.y != viewports[i].height))
                viewports_changed = true;

            ImGui::Image(reinterpret_cast<ImTextureID>(viewports[i].gui_tex[image_idx]),
                         ImVec2{static_cast<float>(viewports[i].width),
                                static_cast<float>(viewports[i].height)});
        }
        ImGui::End();
    }

    if ( ! allocate_viewports())
        return false;

    return true;
}

static bool draw_grid(Viewport& viewport, VkCommandBuffer buf)
{
    return true;
}

static bool render_view(Viewport& viewport, uint32_t image_idx, VkCommandBuffer buf)
{
    if ( ! draw_grid(viewport, buf))
        return false;

    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    if ( ! create_gui_frame(image_idx))
        return false;

    Image& image = vk_swapchain_images[image_idx];

    static CommandBuffers<max_swapchain_size> bufs;

    if ( ! allocate_command_buffers_once(&bufs, vk_num_swapchain_images))
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

    static const Image::Transition depth_init = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    if (vk_depth_buffers[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        vk_depth_buffers[image_idx].set_image_layout(buf, depth_init);

    static VkRenderingAttachmentInfo color_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,             // imageView
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,             // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,  // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        make_clear_color(0, 0, 0, 0)
    };

    static VkRenderingAttachmentInfo depth_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,             // imageView
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,             // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,  // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,
        make_clear_depth(0, 0)
    };

    static VkRenderingInfo rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,              // flags
        { },            // renderArea
        1,              // layerCount
        0,              // viewMask
        1,              // colorAttachmentCount
        &color_att,
        &depth_att,
        nullptr         // pStencilAttachment
    };

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        static const Image::Transition render_viewport_layout = {
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        viewport.color_buffer[image_idx].set_image_layout(buf, render_viewport_layout);

        if (viewport.depth_buffer[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            viewport.depth_buffer[image_idx].set_image_layout(buf, depth_init);

        color_att.imageView   = viewport.color_buffer[image_idx].get_view();
        color_att.clearValue  = make_clear_color(0.2f, 0.2f, 0.2f, 1);
        depth_att.imageView   = viewport.depth_buffer[image_idx].get_view();
        rendering_info.renderArea.extent.width  = viewport.width;
        rendering_info.renderArea.extent.height = viewport.height;

        vkCmdBeginRenderingKHR(buf, &rendering_info);

        if ( ! render_view(viewport, image_idx, buf))
            return false;

        vkCmdEndRenderingKHR(buf);

        static const Image::Transition gui_image_layout = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        viewport.color_buffer[image_idx].set_image_layout(buf, gui_image_layout);
    }

    color_att.imageView              = image.get_view();
    color_att.clearValue             = make_clear_color(0, 0, 0, 1);
    depth_att.imageView              = vk_depth_buffers[image_idx].get_view();
    rendering_info.renderArea.extent = vk_surface_caps.currentExtent;

    vkCmdBeginRenderingKHR(buf, &rendering_info);

    if ( ! send_gui_to_gpu(buf))
        return false;

    vkCmdEndRenderingKHR(buf);

    static const Image::Transition color_att_present = {
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    image.set_image_layout(buf, color_att_present);

    VkResult res;

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
