// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "gui.h"
#include "minivulkan.h"
#include "mstdc.h"
#include "d_printf.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

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

bool init_gui()
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

    static constexpr uint32_t num_samplers = 1;

    static VkDescriptorPoolSize pool_sizes[] = {
        {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  // type
            num_samplers                                // descriptorCount
        }
    };

    static VkDescriptorPoolCreateInfo pool_create_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr,
        0,            // flags
        num_samplers, // maxSets
        mstd::array_size(pool_sizes),
        pool_sizes
    };

    VkDescriptorPool desc_pool;

    VkResult res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &desc_pool));
    if (res != VK_SUCCESS)
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
    if ( ! ImGui_ImplVulkan_Init(&init_info, vk_render_pass)) {
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

bool send_gui_to_gpu(VkCommandBuffer cmdbuf)
{
    ImGui::Render();

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);

    return true;
}

bool is_full_screen()
{
    return false;
}
