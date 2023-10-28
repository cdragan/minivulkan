// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geom_edit.h"
#include "../mstdc.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <stdio.h>

/*

- One viewport
    - Should there me multiple viewports?
    - Draw
        - Background
        - Grid
        - Solid object fill
            - Toggle solid fill on/off (off - pure wireframe)
            - Toggle tessellation on/off
        - Wireframe
            - Toggle between generated triangles and just quad surrounds, i.e. patch wireframe
        - Vertices
        - Connectors to control vertices
        - Selection highlight (surround around selected features)
        - Hovered and selected features have distinct color
        - Toggle texture/color/etc. versus plain
        - Selection rectangle
        - Draw selection surface used to determine what mouse and selection rectangle are touching
- Status: current mode, total vertices/edges/faces, number of selected vertices/edges/faces
- Toolbar with key shortcuts

User input
----------

- View
    - Num 5 :: toggle orthographic view and perspective
    - Num . :: focus on selection
    - Num 1/2/3 orthographic view aligned to axis
    - Alt-Z toggle all vertices (wireframe) and surface vertices (solid) - impacts selection
    - Middle mouse button :: rotate view
    - Shift + Middle mouse button :: pan view
    - Mouse scroll :: zoom view
- Select
    - Drag rectangle to select
    - Additive/subtractive selection
- Manipulation
    - G :: grab and move objects
        - click to finish
        - GX/GY/GZ to snap
        - mouse scroll to change range of the effect
    - R :: rotate
    - S :: scale
        - SX/SY/SZ to snap
    - Alt-S :: move along normals
    - Tab :: toggle between object mode and edit mode
    - Ctrl + Mouse :: snap
- Edit
    - E :: extrude - add faces/edges, begin moving
*/

void GeometryEditor::set_name(const char* new_name)
{
    const uint32_t len = mstd::min(mstd::strlen(new_name),
                                   static_cast<uint32_t>(sizeof(name)) - 1U);
    mstd::mem_copy(name, new_name, len);
    name[len] = 0;
}

bool GeometryEditor::alloc_view_resources(View*     dst_view,
                                          uint32_t  width,
                                          uint32_t  height,
                                          VkSampler viewport_sampler)
{
    dst_view->width  = width;
    dst_view->height = height;

    for (uint32_t i_img = 0; i_img < vk_num_swapchain_images; i_img++) {
        Resources& res = dst_view->res[i_img];

        if (res.color.get_view())
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

        color_info.width  = width;
        color_info.height = height;
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

        depth_info.width  = width;
        depth_info.height = height;
        depth_info.format = vk_depth_format;

        static ImageInfo select_query_info {
            0, // width
            0, // height
            VK_FORMAT_R32_UINT,
            1, // mip_levels
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            Usage::device_temporary
        };

        select_query_info.width  = width;
        select_query_info.height = height;

        static ImageInfo select_query_host_info {
            0, // width
            0, // height
            VK_FORMAT_R32_UINT,
            1, // mip_levels
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            Usage::host_only
        };

        // TODO specify and allocate maximum selection size
        select_query_host_info.width  = width;
        select_query_host_info.height = height;

        if ( ! res.color.allocate(color_info))
            return false;

        if ( ! res.depth.allocate(depth_info))
            return false;

        if ( ! res.selection.allocate(select_query_info))
            return false;

        if ( ! res.host_selection.get_view() && ! res.host_selection.allocate(select_query_host_info))
            return false;

        res.selection_pending = false;

        if (res.gui_texture) {

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
            image_info.imageView = res.color.get_view();
            write_desc.dstSet    = res.gui_texture;

            vkUpdateDescriptorSets(vk_dev, 1, &write_desc, 0, nullptr);
        }
        else {
            res.gui_texture = ImGui_ImplVulkan_AddTexture(
                    viewport_sampler,
                    res.color.get_view(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    return true;
}

void GeometryEditor::free_view_resources(View* dst_view)
{
    dst_view->width  = 0;
    dst_view->height = 0;

    for (uint32_t i_img = 0; i_img < max_swapchain_size; i_img++) {
        Resources& res = dst_view->res[i_img];

        res.color.destroy();
        res.depth.destroy();
        res.selection.destroy();

        res.selection_pending = false;
    }
}

void GeometryEditor::gui_status_bar()
{
    const ImVec2 item_pos = ImGui::GetItemRectMin();
    const ImVec2 win_size = ImGui::GetWindowSize();

    ImGui::SetNextWindowPos(ImVec2(item_pos.x,
                                   item_pos.y + win_size.y - ImGui::GetFrameHeight()));

    const ImGuiWindowFlags status_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_MenuBar;

    const ImVec2 status_bar_size{win_size.x,
                                 ImGui::GetTextLineHeightWithSpacing()};

    if (ImGui::BeginChild("##Geometry Editor Status Bar", status_bar_size, false, status_flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::Text("Pos: %ux%u", static_cast<unsigned>(item_pos.x), static_cast<unsigned>(item_pos.y));
            ImGui::Separator();
            ImGui::Text("Window: %ux%u", static_cast<unsigned>(win_size.x), static_cast<unsigned>(win_size.y));

            ImGui::EndMenuBar();
        }
    }
    ImGui::EndChild();
}

bool GeometryEditor::create_gui_frame(uint32_t image_idx)
{
    char window_title[sizeof(name) + 36];
    snprintf(window_title, sizeof(window_title), "Geometry Editor - %s###Geometry Editor", name);

    const ImGuiWindowFlags geom_win_flags =
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoScrollbar;

    if ( ! ImGui::Begin(window_title, nullptr, geom_win_flags)) {
        ImGui::End();
        return true;
    }

    gui_status_bar();

    ImGui::End();

    return true;
}

bool GeometryEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    return true;
}
