// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "../d_printf.h"
#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                     | ImGuiConfigFlags_DockingEnable;

struct Viewport {
    const char*   name;
    bool          enabled;
    uint32_t      width;
    uint32_t      height;
    VkRenderPass  render_pass;
    Image         color_buffer[max_swapchain_size];
    Image         depth_buffer[max_swapchain_size];
    VkFramebuffer frame_buffer[max_swapchain_size];
};

static Viewport viewports[] = {
    { "Front View", true },
    { "3D View",    true }
};

static bool viewports_changed = true;

static DeviceMemoryHeap viewport_heap{DeviceMemoryHeap::device_memory};

static VkSampler viewport_sampler;

uint32_t check_device_features()
{
    return 0;
}

bool create_additional_heaps()
{
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
    if ( ! create_samplers())
        return false;

    return true;
}

static bool destroy_viewports()
{
    if ( ! idle_queue())
        return false;

    for (Viewport& viewport : viewports) {
        for (uint32_t i_img = 0; i_img < max_swapchain_size; i_img++) {
            if (viewport.frame_buffer[i_img]) {
                vkDestroyFramebuffer(vk_dev, viewport.frame_buffer[i_img], nullptr);
                viewport.frame_buffer[i_img] = VK_NULL_HANDLE;
            }

            viewport.color_buffer[i_img].destroy();
            viewport.depth_buffer[i_img].destroy();
        }

        viewport.width  = 0;
        viewport.height = 0;
    }

    return true;
}

static constexpr VkDeviceSize viewport_error = ~VkDeviceSize(0);

static VkDeviceSize create_viewport_images(uint32_t i_view)
{
    Viewport& viewport = viewports[i_view];

    const ImVec2 win_size = ImGui::GetWindowSize();

    if (viewport.width)
        return 0;

    static ImageInfo color_info {
        0, // width
        0, // height
        VK_FORMAT_UNDEFINED,
        1, // mip_levels
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    };

    color_info.width  = static_cast<uint32_t>(win_size.x);
    color_info.height = static_cast<uint32_t>(win_size.y);
    color_info.format = swapchain_create_info.imageFormat;

    static ImageInfo depth_info {
        0, // width
        0, // height
        VK_FORMAT_UNDEFINED,
        1, // mip_levels
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
    };

    depth_info.width  = static_cast<uint32_t>(win_size.x);
    depth_info.height = static_cast<uint32_t>(win_size.y);
    depth_info.format = vk_depth_format;

    VkDeviceSize heap_size = 0;

    for (uint32_t i_img = 0; i_img < vk_num_swapchain_images; i_img++) {

        if ( ! viewport.color_buffer[i_img].create(color_info, VK_IMAGE_TILING_OPTIMAL))
            return viewport_error;

        heap_size += mstd::align_up(viewport.color_buffer[i_img].size(),
                                    VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));

        if ( ! viewport.depth_buffer[i_img].create(depth_info, VK_IMAGE_TILING_OPTIMAL))
            return viewport_error;

        heap_size += mstd::align_up(viewport.depth_buffer[i_img].size(),
                                    VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));
    }

    viewport.width  = static_cast<uint32_t>(win_size.x);
    viewport.height = static_cast<uint32_t>(win_size.y);

    return heap_size;
}

static bool allocate_framebuffer(Viewport& viewport, uint32_t i_img)
{
    static VkImageView attachments[2];

    static VkFramebufferCreateInfo frame_buffer_info = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        nullptr,
        0,                  // flags
        VK_NULL_HANDLE,     // renderPass
        mstd::array_size(attachments),
        attachments,
        0,                  // width
        0,                  // height
        1                   // layers
    };
    frame_buffer_info.renderPass = vk_render_pass;
    frame_buffer_info.width      = viewport.width;
    frame_buffer_info.height     = viewport.height;

    attachments[0] = viewport.color_buffer[i_img].get_view();
    attachments[1] = viewport.depth_buffer[i_img].get_view();

    const VkResult res = CHK(vkCreateFramebuffer(vk_dev,
                                                 &frame_buffer_info,
                                                 nullptr,
                                                 &viewport.frame_buffer[i_img]));
    return res == VK_SUCCESS;
}

static bool allocate_viewports(VkDeviceSize heap_size)
{
    if ( ! viewport_heap.reserve(heap_size))
        return false;

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        for (uint32_t i_img = 0; i_img < vk_num_swapchain_images; i_img++) {
            if ( ! viewport.color_buffer[i_img].allocate(viewport_heap))
                return false;

            if ( ! viewport.depth_buffer[i_img].allocate(viewport_heap))
                return false;

            if ( ! allocate_framebuffer(viewport, i_img))
                return false;
        }
    }

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

        ImGui::Separator();

        for (uint32_t i = 0; i < mstd::array_size(viewports); i++)
            ImGui::Checkbox(viewports[i].name, &viewports[i].enabled);
    }
    ImGui::End();

    if (viewports_changed)
        if ( ! destroy_viewports())
            return false;
    viewports_changed = false;

    VkDeviceSize heap_size = 0;

    for (uint32_t i = 0; i < mstd::array_size(viewports); i++) {
        if ( ! viewports[i].enabled)
            continue;

        ImGui::Begin(viewports[i].name, &viewports[i].enabled);
        {
            VkDeviceSize size = create_viewport_images(i);
            if (size == viewport_error)
                return false;
            heap_size += size;

            const ImVec2 win_size = ImGui::GetWindowSize();
            if ((win_size.x != viewports[i].width) || (win_size.y != viewports[i].height))
                viewports_changed = true;

            ImGui::Text("Window Size: %d x %d", static_cast<int>(win_size.x), static_cast<int>(win_size.y));
        }
        ImGui::End();
    }

    if (heap_size && ! allocate_viewports(heap_size))
        return false;

    return true;
}

static bool render_view(uint32_t i_view, uint32_t image_idx, VkCommandBuffer buf)
{
    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    if ( ! create_gui_frame())
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

    for (uint32_t i_view = 0; i_view < mstd::array_size(viewports); i_view++)
        if (viewports[i_view].enabled && ! render_view(i_view, image_idx, buf))
            return false;

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
