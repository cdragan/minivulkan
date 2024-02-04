// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "gui.h"

#include "gui_imgui.h"
#include "minivulkan.h"
#include "mstdc.h"
#include "resource.h"
#include "d_printf.h"

#include "imgui_internal.h" // For checking pending events

float vk_surface_scale = 1.0f;

static VkRenderPass  vk_gui_render_pass;
static VkFramebuffer vk_framebuffers[max_swapchain_size];

static PFN_vkCmdBeginRenderPass myCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass   myCmdEndRenderPass;
static PFN_vkCreateFramebuffer  myCreateFramebuffer;
static PFN_vkDestroyFramebuffer myDestroyFramebuffer;

static void check_gui_result(VkResult imgui_error)
{
    if (CHK(imgui_error) != VK_SUCCESS) {
    }
}

static PFN_vkVoidFunction load_vk_function(const char* name, void* cookie)
{
    PFN_vkVoidFunction func = vkGetDeviceProcAddr(vk_dev, name);
    if ( ! func) {
        func = vkGetInstanceProcAddr(vk_instance, name);
        if ( ! func) {
            d_printf("Failed to load function %s for GUI\n", name);
        }
    }
    return func;
}

static bool create_render_pass(VkRenderPass* render_pass, GuiClear clear)
{
    static VkAttachmentDescription attachments[] = {
        {
            0, // flags
            VK_FORMAT_UNDEFINED,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_LOAD,
            VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        },
        {
            0, // flags
            VK_FORMAT_UNDEFINED,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };

    if (clear == GuiClear::clear)
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

    if ( ! find_optimal_tiling_format(&swapchain_create_info.imageFormat,
                                      1,
                                      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
                                      &attachments[0].format)) {
        d_printf("Error: surface format does not support color attachments\n");
        return false;
    }

    attachments[1].format = vk_depth_format;

    static const VkAttachmentReference color_att_ref = {
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    static const VkAttachmentReference depth_att_ref = {
        1,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    static const VkSubpassDescription subpass = {
        0,          // flags
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        0,          // inputAttachmentCount
        nullptr,    // pInputAttachments
        1,
        &color_att_ref,
        nullptr,    // pResolveAttachments
        &depth_att_ref,
        0,          // preserveAttachmentCount
        nullptr     // pPreserveAttachments
    };
    static VkRenderPassCreateInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        nullptr,    // pNext
        0,          // flags
        mstd::array_size(attachments),
        attachments,
        1,          // subpassCount
        &subpass
    };

    if (clear != GuiClear::clear) {
        static const VkSubpassDependency dependency = {
            VK_SUBPASS_EXTERNAL,
            0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            0
        };

        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies   = &dependency;
    }

    PFN_vkCreateRenderPass vkCreateRenderPass;

    vkCreateRenderPass   = reinterpret_cast<PFN_vkCreateRenderPass>(  load_vk_function("vkCreateRenderPass",   nullptr));
    myCmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(load_vk_function("vkCmdBeginRenderPass", nullptr));
    myCmdEndRenderPass   = reinterpret_cast<PFN_vkCmdEndRenderPass>(  load_vk_function("vkCmdEndRenderPass",   nullptr));
    myCreateFramebuffer  = reinterpret_cast<PFN_vkCreateFramebuffer>( load_vk_function("vkCreateFramebuffer",  nullptr));
    myDestroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(load_vk_function("vkDestroyFramebuffer", nullptr));

    if ( ! vkCreateRenderPass || ! myCmdBeginRenderPass || ! myCmdEndRenderPass || ! myCreateFramebuffer || ! myDestroyFramebuffer)
        return false;

    const VkResult res = CHK(vkCreateRenderPass(vk_dev, &render_pass_info, nullptr, render_pass));
    return res == VK_SUCCESS;
}

static bool create_framebuffer(uint32_t image_idx)
{
    if (vk_framebuffers[image_idx])
        return true;

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
    frame_buffer_info.renderPass = vk_gui_render_pass;
    frame_buffer_info.width      = vk_surface_caps.currentExtent.width;
    frame_buffer_info.height     = vk_surface_caps.currentExtent.height;

    attachments[0] = vk_swapchain_images[image_idx].get_view();
    attachments[1] = vk_depth_buffers[image_idx].get_view();

    const VkResult res = CHK(myCreateFramebuffer(vk_dev,
                                                 &frame_buffer_info,
                                                 nullptr,
                                                 &vk_framebuffers[image_idx]));
    return res == VK_SUCCESS;
}

void notify_gui_heap_freed();

void resize_gui()
{
    notify_gui_heap_freed();

    for (uint32_t i = 0; i < mstd::array_size(vk_framebuffers); i++) {
        if (vk_framebuffers[i]) {
            myDestroyFramebuffer(vk_dev, vk_framebuffers[i], nullptr);
            vk_framebuffers[i] = VK_NULL_HANDLE;
        }
    }
}

bool gui_has_pending_events()
{
    ImGuiContext& ctx = *GImGui;

    return ctx.InputEventsQueue.Size > 0;
}

static bool begin_gui_render_pass(VkCommandBuffer buf, uint32_t image_idx)
{
    if ( ! create_framebuffer(image_idx))
        return false;

    static const VkClearValue clear_values[] = {
        make_clear_color(0, 0, 0, 1),
        make_clear_depth(0, 0)
    };

    static VkRenderPassBeginInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr,
        VK_NULL_HANDLE,     // renderPass
        VK_NULL_HANDLE,     // framebuffer
        { },                // renderArea
        mstd::array_size(clear_values),
        clear_values
    };

    render_pass_info.renderPass        = vk_gui_render_pass;
    render_pass_info.framebuffer       = vk_framebuffers[image_idx];
    render_pass_info.renderArea.extent = vk_surface_caps.currentExtent;

    myCmdBeginRenderPass(buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    return true;
}

bool init_gui(GuiClear clear)
{
    IMGUI_CHECKVERSION();
    if ( ! ImGui::CreateContext()) {
        d_printf("Failed to initialize GUI context\n");
        return false;
    }

    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= gui_config_flags;

    if ( ! ImGui_ImplVulkan_LoadFunctions(load_vk_function, nullptr))
        return false;

    static VkDescriptorPoolSize pool_sizes[] = {
        {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  // type
            0                                           // descriptorCount
        }
    };
    pool_sizes[0].descriptorCount = gui_num_descriptors;

    static VkDescriptorPoolCreateInfo pool_create_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr,
        0,            // flags
        0,            // maxSets
        mstd::array_size(pool_sizes),
        pool_sizes
    };

    pool_create_info.maxSets = gui_num_descriptors;

    VkDescriptorPool desc_pool;

    VkResult res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &desc_pool));
    if (res != VK_SUCCESS)
        return false;

    assert(vk_gui_render_pass == VK_NULL_HANDLE);
    if ( ! create_render_pass(&vk_gui_render_pass, clear))
        return false;

    ImGui_ImplVulkan_InitInfo init_info = { };
    init_info.Instance        = vk_instance;
    init_info.PhysicalDevice  = vk_phys_dev;
    init_info.Device          = vk_dev;
    init_info.QueueFamily     = vk_queue_family_index;
    init_info.Queue           = vk_queue;
    init_info.DescriptorPool  = desc_pool;
    init_info.MinImageCount   = vk_num_swapchain_images;
    init_info.ImageCount      = vk_num_swapchain_images;
    init_info.CheckVkResultFn = check_gui_result;
    if ( ! ImGui_ImplVulkan_Init(&init_info, vk_gui_render_pass)) {
        d_printf("Failed to initialize Vulkan GUI\n");
        return false;
    }

    // Send fonts to GPU
    CommandBuffers<1> cmd_buf;
    if ( ! allocate_command_buffers(&cmd_buf))
        return false;

    if ( ! reset_and_begin_command_buffer(cmd_buf.bufs[0]))
        return false;

    if ( ! ImGui_ImplVulkan_CreateFontsTexture(cmd_buf.bufs[0])) {
        d_printf("Failed to create GUI font texture\n");
        return false;
    }

    res = CHK(vkEndCommandBuffer(cmd_buf.bufs[0]));
    if (res != VK_SUCCESS)
        return false;

    static const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0,                  // waitSemaphoreCount
        nullptr,            // pWaitSemaphored
        &dst_stage,         // pWaitDstStageMask
        1,                  // commandBufferCount
        cmd_buf.bufs,
        0,                  // signalSemaphoreCount
        nullptr             // pSignalSemaphores
    };

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, vk_fens[fen_copy_to_dev]));
    if (res != VK_SUCCESS)
        return false;

    if ( ! wait_and_reset_fence(fen_copy_to_dev))
        return false;

    ImGui_ImplVulkan_DestroyFontUploadObjects();

    GET_VK_DEV_FUNCTION(vkDestroyCommandPool, vk_dev);
    vkDestroyCommandPool(vk_dev, cmd_buf.pool, nullptr);

    return true;
}

bool send_gui_to_gpu(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    if ( ! begin_gui_render_pass(cmdbuf, image_idx))
        return false;

    ImGui::Render();

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);

    myCmdEndRenderPass(cmdbuf);

    return true;
}

bool is_full_screen()
{
    return false;
}
