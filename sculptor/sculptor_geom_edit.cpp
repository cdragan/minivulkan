// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geom_edit.h"
#include "../d_printf.h"
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
    - 1: vertices, 2: edges, 3: faces
    - Shift+1/2/3: enable selection of multiple types of items (vertices, edges, faces)
    - Shift: select multiple items, one by one
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

Toolbar in Object Edit mode
---------------------------

- File
    - New
    - Open
    - Save
- Edit
    - Undo
    - Redo
    - Copy
    - Paste
    - Cut
- Select
    - Faces
    - Edges
    - Vertices
- Viewport
    - Perspective view
    - Orthographic view
    - Pan
    - Rotate
    - Zoom
- Transform
    - Move
    - Rotate
    - Scale
- Edit
    - Delete
    - Extrude

*/

void GeometryEditor::set_name(const char* new_name)
{
    const uint32_t len = mstd::min(mstd::strlen(new_name),
                                   static_cast<uint32_t>(sizeof(name)) - 1U);
    mstd::mem_copy(name, new_name, len);
    name[len] = 0;
}

bool GeometryEditor::allocate_resources()
{
    static VkSampler viewport_sampler;

    if ( ! viewport_sampler) {
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
    }

    return alloc_view_resources(&view, window_width, window_height, viewport_sampler);
}

bool GeometryEditor::alloc_view_resources(View*     dst_view,
                                          uint32_t  width,
                                          uint32_t  height,
                                          VkSampler viewport_sampler)
{
    if (dst_view->res[0].color.get_image())
        return true;

    const uint32_t new_host_sel_size = mstd::align_up(width, 1024U) * mstd::align_up(height, 1024U);
    const bool     update_host_sel   = new_host_sel_size > dst_view->host_sel_size;
    if (update_host_sel) {
        dst_view->host_sel_size = new_host_sel_size;
        d_printf("Updating host selection surface\n");
    }

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

        select_query_host_info.width  = mstd::align_up(width,  1024U);
        select_query_host_info.height = mstd::align_up(height, 1024U);

        if (update_host_sel)
            res.host_selection.destroy();

        if ( ! res.color.allocate(color_info))
            return false;

        if ( ! res.depth.allocate(depth_info))
            return false;

        if ( ! res.selection.allocate(select_query_info))
            return false;

        if ( ! res.host_selection.allocate(select_query_host_info))
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

void GeometryEditor::free_resources()
{
    free_view_resources(&view);
}

void GeometryEditor::free_view_resources(View* dst_view)
{
    if ( ! dst_view->res[0].color.get_image())
        return;

    dst_view->width  = 0;
    dst_view->height = 0;

    for (uint32_t i_img = 0; i_img < max_swapchain_size; i_img++) {
        Resources& res = dst_view->res[i_img];

        res.color.destroy();
        res.depth.destroy();
        res.selection.destroy();
        res.host_selection.destroy_and_keep_memory();

        res.selection_pending = false;
    }
}

float GeometryEditor::gui_status_bar()
{
    const ImVec2 item_pos = ImGui::GetItemRectMin();
    const ImVec2 win_size = ImGui::GetWindowSize();

    const float height = ImGui::GetFrameHeight();

    ImGui::SetNextWindowPos(ImVec2(item_pos.x,
                                   item_pos.y + win_size.y - height));

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

    return height;
}

bool GeometryEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc)
{
    char window_title[sizeof(name) + 36];
    snprintf(window_title, sizeof(window_title), "Geometry Editor - %s###Geometry Editor", name);

    const ImGuiWindowFlags geom_win_flags =
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});

    const bool window_ok = ImGui::Begin(window_title, nullptr, geom_win_flags);

    ImGui::PopStyleVar();

    if ( ! window_ok) {
        ImGui::End();
        return true;
    }

    const float status_bar_height = gui_status_bar();

    const ImVec2 content_size = ImGui::GetWindowSize();

    if ((static_cast<uint32_t>(content_size.x) != window_width) ||
        (static_cast<uint32_t>(content_size.y - status_bar_height) != window_height))
        *need_realloc = true;

    window_width  = static_cast<uint32_t>(content_size.x);
    window_height = static_cast<uint32_t>(content_size.y - status_bar_height);

    ImGui::Image(reinterpret_cast<ImTextureID>(view.res[image_idx].gui_texture),
                 ImVec2{static_cast<float>(window_width),
                        static_cast<float>(window_height)});

    ImGui::End();

    return true;
}

bool GeometryEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    return true;
}
