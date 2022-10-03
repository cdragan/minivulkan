// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                     | ImGuiConfigFlags_DockingEnable;

uint32_t check_device_features()
{
    return 0;
}

bool create_additional_heaps()
{
    return true;
}

bool create_pipeline_layouts()
{
    return true;
}

bool create_pipelines()
{
    return true;
}

bool create_gui_frame()
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

        static float user_roundedness;
        ImGui::SliderFloat("Roundedness", &user_roundedness, 0.0f, 1.0f);
    }
    ImGui::End();

    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    if ( ! create_gui_frame())
        return false;

    // Render image
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
        make_clear_color(0.2f, 0.2f, 0.2f, 0),
        make_clear_depth(0, 0)
    };

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

    // ...

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
