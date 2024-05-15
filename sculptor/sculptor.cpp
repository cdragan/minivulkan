// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "sculptor_materials.h"
#include "sculptor_geom_edit.h"

#include "../d_printf.h"
#include "../gui.h"
#include "../gui_imgui.h"
#include "../memory_heap.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../vmath.h"

#include "sculptor_shaders.h"
#include "../shaders.h"

#include <math.h>
#include <stdio.h>

const char app_name[] = "Sculptor";

const int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                           | ImGuiConfigFlags_DockingEnable;

static Sculptor::GeometryEditor geometry_editor;

// Global list of all possible editor windows, this collection is used for generic handling
// of editor windows, like drawing and event passing to visible editors
static Sculptor::Editor* const editors[] = {
    &geometry_editor
};

// Need +1 for ImGui full window itself
const unsigned gui_num_descriptors = (mstd::array_size(editors) + 1) * max_swapchain_size;

uint32_t check_device_features()
{
    uint32_t missing_features = 0;

    missing_features += check_feature(&vk_features.features.tessellationShader);
    missing_features += check_feature(&vk_features.features.fillModeNonSolid);
    missing_features += check_feature(&vk_dyn_rendering_features.dynamicRendering);

    return missing_features;
}

bool skip_frame(struct Window* w)
{
    static int    skip_count = 0;
    constexpr int max_skip_count = 2;

    // TODO check if any editor is animating

    if (gui_has_pending_events())
        skip_count = 0;
    else if (skip_count < max_skip_count)
        ++skip_count;

    return skip_count >= max_skip_count;
}

bool init_assets()
{
    geometry_editor.set_object_name("unnamed");

    if ( ! Sculptor::create_material_layouts())
        return false;

    if ( ! init_gui(GuiClear::clear))
        return false;

    return true;
}

static VkDeviceSize heap_low_checkpoint;
static VkDeviceSize heap_high_checkpoint;
static bool         viewports_allocated;

void notify_gui_heap_freed()
{
    for (Sculptor::Editor* editor : editors)
        editor->free_resources();
}

static bool destroy_viewports()
{
    if ( ! idle_queue())
        return false;

    notify_gui_heap_freed();

    return true;
}

static bool allocate_viewports()
{
    if ( ! viewports_allocated)
        heap_low_checkpoint = mem_mgr.get_heap_checkpoint();

    for (Sculptor::Editor* editor : editors)
        if (editor->enabled && ! editor->allocate_resources())
            return false;

    heap_high_checkpoint = mem_mgr.get_heap_checkpoint();
    if (heap_high_checkpoint != heap_low_checkpoint)
        viewports_allocated = true;

    return true;
}

static bool create_gui_frame(uint32_t image_idx)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width)  / vk_surface_scale;
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height) / vk_surface_scale;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    static vmath::vec2 prev_mouse_pos;

    const vmath::vec2 abs_mouse_pos = ImGui::GetMousePos();
    const float       wheel_delta   = io.MouseWheel;

    static vmath::vec2 initial_pos;
    static bool        initial_pos_set;
    if ( ! initial_pos_set) {
        initial_pos     = abs_mouse_pos;
        prev_mouse_pos  = abs_mouse_pos;
        initial_pos_set = true;
    }
    const bool is_mouse_pos_valid = prev_mouse_pos.x != initial_pos.x;

    const vmath::vec2 mouse_delta = abs_mouse_pos - prev_mouse_pos;
    prev_mouse_pos = abs_mouse_pos;

    const Sculptor::Editor::UserInput input = {
        is_mouse_pos_valid ? abs_mouse_pos : vmath::vec2{0.0f, 0.0f},
        is_mouse_pos_valid ? mouse_delta   : vmath::vec2{0.0f, 0.0f},
        wheel_delta
    };

    static const Sculptor::Editor::UserInput no_input = {
        vmath::vec2{-(1 << 20), -(1 << 20)},
        vmath::vec2{0, 0},
        0
    };

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {
            }
            if (ImGui::MenuItem("Open", CTRL_KEY "O")) {
            }
            if (ImGui::MenuItem("Save", CTRL_KEY "S")) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", CTRL_KEY "Z")) {
            }
            if (ImGui::MenuItem("Redo", CTRL_KEY "Y", false, false)) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", CTRL_KEY "X")) {
            }
            if (ImGui::MenuItem("Copy", CTRL_KEY "C")) {
            }
            if (ImGui::MenuItem("Paste", CTRL_KEY "V")) {
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

        for (Sculptor::Editor* editor : editors) {
            char name[128];
            snprintf(name, sizeof(name), "%s: %s", editor->get_editor_name(), editor->get_object_name());
            ImGui::Checkbox(name, &editor->enabled);
        }
    }
    ImGui::End();

    bool viewports_changed = false;

    for (Sculptor::Editor* editor : editors) {
        if ( ! editor->enabled)
            continue;

        const bool real_input = ! editor->is_mouse_captured() || editor->has_captured_mouse();

        bool need_realloc = false;
        if ( ! editor->create_gui_frame(image_idx,
                                        &need_realloc,
                                        real_input ? input : no_input))
            return false;

        if (need_realloc)
            viewports_changed = true;
    }

    if (viewports_changed && ! destroy_viewports())
        return false;

    if ( ! allocate_viewports())
        return false;

    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence, uint32_t sem_id)
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

    for (Sculptor::Editor* editor : editors)
        if (editor->enabled && ! editor->draw_frame(buf, image_idx))
            return false;

    if ( ! send_gui_to_gpu(buf, image_idx))
        return false;

    static const Image::Transition color_att_present = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
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
        1,                                  // waitSemaphoreCount
        nullptr,                            // pWaitSemaphores
        &dst_stage,                         // pWaitDstStageMask
        1,                                  // commandBufferCount
        nullptr,                            // pCommandBuffers
        1,                                  // signalSemaphoreCount
        nullptr,                            // pSignalSemaphores
    };

    submit_info.pWaitSemaphores   = &vk_sems[sem_id + sem_acquire];
    submit_info.pSignalSemaphores = &vk_sems[sem_id + sem_present];

    submit_info.pCommandBuffers = &buf;

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, queue_fence));
    if (res != VK_SUCCESS)
        return false;

    return true;
}
