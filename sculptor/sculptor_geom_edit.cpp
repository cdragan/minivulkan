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
        d_printf("Updating host selection feedback surface\n");
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
            res.host_select_feedback.destroy();

        if ( ! res.color.allocate(color_info))
            return false;

        if ( ! res.depth.allocate(depth_info))
            return false;

        if ( ! res.select_feedback.allocate(select_query_info))
            return false;

        if ( ! res.host_select_feedback.allocate(select_query_host_info))
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
        res.select_feedback.destroy();
        res.host_select_feedback.destroy_and_keep_memory();

        res.selection_pending = false;
    }
}

void GeometryEditor::gui_status_bar()
{
    const ImVec2 item_pos = ImGui::GetItemRectMin();
    const ImVec2 win_size = ImGui::GetWindowSize();

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

    const float frame_height = ImGui::GetFrameHeight();

    const ImVec2 content_size = ImGui::GetWindowSize();

    const uint32_t new_width  = static_cast<uint32_t>(content_size.x);
    const uint32_t new_height = static_cast<uint32_t>(content_size.y - frame_height * 2);

    if ((new_width != window_width) || (new_height != window_height))
        *need_realloc = true;

    window_width  = new_width;
    window_height = new_height;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});

    ImGui::Image(reinterpret_cast<ImTextureID>(view.res[image_idx].gui_texture),
                 ImVec2{static_cast<float>(window_width),
                        static_cast<float>(window_height)});

    ImGui::PopStyleVar();

    gui_status_bar();

    ImGui::End();

    return true;
}

bool GeometryEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    if ( ! draw_geometry_view(cmdbuf, view, image_idx))
        return false;

    if ( ! draw_selection_feedback(cmdbuf, view, image_idx))
        return false;

    return true;
}

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

bool GeometryEditor::draw_geometry_view(VkCommandBuffer cmdbuf, View& dst_view, uint32_t image_idx)
{
    Resources& res = dst_view.res[image_idx];

    static const Image::Transition render_viewport_layout = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    res.color.set_image_layout(cmdbuf, render_viewport_layout);

    static const Image::Transition depth_init = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };

    if (res.depth.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        res.depth.set_image_layout(cmdbuf, depth_init);

    color_att.imageView  = res.color.get_view();
    color_att.clearValue = make_clear_color(0.2f, 0.2f, 0.2f, 1);
    depth_att.imageView  = res.depth.get_view();
    rendering_info.renderArea.extent.width  = dst_view.width;
    rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRenderingKHR(cmdbuf, &rendering_info);
    vkCmdEndRenderingKHR(cmdbuf);

    static const Image::Transition gui_image_layout = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    res.color.set_image_layout(cmdbuf, gui_image_layout);

    return true;
}

bool GeometryEditor::draw_selection_feedback(VkCommandBuffer cmdbuf, View& dst_view, uint32_t image_idx)
{
    Resources& res = dst_view.res[image_idx];

    static const Image::Transition render_viewport_layout = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    res.select_feedback.set_image_layout(cmdbuf, render_viewport_layout);

    assert(res.depth.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    color_att.imageView                  = res.select_feedback.get_view();
    color_att.clearValue.color.uint32[0] = 0;
    depth_att.imageView                  = res.depth.get_view();
    rendering_info.renderArea.extent.width  = dst_view.width;
    rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRenderingKHR(cmdbuf, &rendering_info);
    vkCmdEndRenderingKHR(cmdbuf);

    static const Image::Transition transfer_src_image_layout = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
    };
    res.select_feedback.set_image_layout(cmdbuf, transfer_src_image_layout);

    if (res.host_select_feedback.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        static const Image::Transition transfer_dst_image_layout = {
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        };
        res.host_select_feedback.set_image_layout(cmdbuf, transfer_dst_image_layout);
    }

    res.selection_pending = true;

    static VkImageCopy region = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { },        // srcOffset
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { },        // dstOffset
        { 0, 0, 1 } // extent
    };

    region.extent.width  = dst_view.width;
    region.extent.height = dst_view.height;

    vkCmdCopyImage(cmdbuf,
                   res.select_feedback.get_image(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   res.host_select_feedback.get_image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);

    return true;
}
