// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geom_edit.h"
#include "sculptor_materials.h"
#include "../d_printf.h"
#include "../mstdc.h"
#include "../shaders.h"

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

namespace {
    enum MaterialsForShaders {
        mat_grid,
        mat_object_edge,
        num_materials
    };

    struct Transforms {
        vmath::mat4 model_view;
        vmath::mat3 model_view_normal;
        vmath::vec4 proj;
        vmath::vec4 proj_w;
    };

    constexpr uint32_t transforms_per_viewport = 1;
    constexpr float    int16_scale             = 32767.0f;
}

namespace Sculptor {

const char* GeometryEditor::get_editor_name() const
{
    return "Geometry Editor";
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

    if ( ! alloc_view_resources(&view, window_width, window_height, viewport_sampler))
        return false;

    if ( ! allocate_resources_once())
        return false;

    return true;
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

bool GeometryEditor::allocate_resources_once()
{
    // Check if already allocated
    if (gray_patch_mat)
        return true;

    if ( ! patch_geometry.allocate())
        return false;

    // TODO load user-specified geometry
    patch_geometry.set_cube();

    if ( ! create_materials())
        return false;

    if ( ! create_transforms_buffer())
        return false;

    if ( ! create_descriptor_sets())
        return false;

    view.camera[static_cast<int>(ViewType::free_moving)] = Camera{ { 0.0f, 0.0f,  0.0f }, 0.25f,    0.0f, 0.0f, 1.0f };
    view.camera[static_cast<int>(ViewType::front)]       = Camera{ { 0.0f, 0.0f, -2.0f },  0.0f, 4096.0f, 0.0f, 0.0f };

    return true;
}

bool GeometryEditor::create_materials()
{
    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0, // location
            0, // binding
            VK_FORMAT_R16G16B16_SNORM,
            offsetof(Sculptor::Geometry::Vertex, pos)
        }
    };

    materials_stride = static_cast<uint32_t>(mstd::align_up(
                static_cast<VkDeviceSize>(sizeof(ShaderMaterial)),
                vk_phys_props.properties.limits.minUniformBufferOffsetAlignment));

    if ( ! materials_buf.allocate(Usage::dynamic,
                                  materials_stride * max_swapchain_size * num_materials,
                                  VK_FORMAT_UNDEFINED,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
        return false;

    static const MaterialInfo object_mat_info = {
        {
            shader_pass_through_vert,
            shader_sculptor_object_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f, // depth_bias
        mstd::array_size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        VK_FORMAT_UNDEFINED,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16, // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,                // depth_test
        { 0x00, 0x00, 0x00 } // diffuse
    };

    if ( ! create_material(object_mat_info, &gray_patch_mat))
        return false;

    return true;
}

bool GeometryEditor::create_transforms_buffer()
{
    transforms_stride = static_cast<uint32_t>(mstd::align_up(
                static_cast<VkDeviceSize>(sizeof(Transforms)),
                vk_phys_props.properties.limits.minUniformBufferOffsetAlignment));

    return transforms_buf.allocate(Usage::dynamic,
                                   transforms_stride * max_swapchain_size * transforms_per_viewport,
                                   VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
}

bool GeometryEditor::create_descriptor_sets()
{
    static VkDescriptorSetAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr,
        VK_NULL_HANDLE,                 // descriptorPool
        2,                              // descriptorSetCount
        &Sculptor::desc_set_layout[1]   // pSetLayouts
    };

    {
        static VkDescriptorPoolSize pool_sizes[] = {
            {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                2
            },
            {
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1
            }
        };

        static VkDescriptorPoolCreateInfo pool_create_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            nullptr,
            0, // flags
            2, // maxSets
            mstd::array_size(pool_sizes),
            pool_sizes
        };

        const VkResult res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &alloc_info.descriptorPool));
        if (res != VK_SUCCESS)
            return false;
    }
    {
        const VkResult res = CHK(vkAllocateDescriptorSets(vk_dev, &alloc_info, &desc_set[1]));
        if (res != VK_SUCCESS)
            return false;
    }

    {
        static VkDescriptorBufferInfo materials_buffer_info = {
            VK_NULL_HANDLE,     // buffer
            0,                  // offset
            0                   // range
        };
        static VkDescriptorBufferInfo transforms_buffer_info = {
            VK_NULL_HANDLE,     // buffer
            0,                  // offset
            0                   // range
        };
        static VkDescriptorBufferInfo storage_buffer_info = {
            VK_NULL_HANDLE,     // buffer
            0,                  // offset
            VK_WHOLE_SIZE       // range
        };
        static VkWriteDescriptorSet write_desc_sets[] = {
            {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                VK_NULL_HANDLE,                             // dstSet
                0,                                          // dstBinding
                0,                                          // dstArrayElement
                1,                                          // descriptorCount
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,  // descriptorType
                nullptr,                                    // pImageInfo
                &materials_buffer_info,                     // pBufferInfo
                nullptr                                     // pTexelBufferView
            },
            {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                VK_NULL_HANDLE,                             // dstSet
                0,                                          // dstBinding
                0,                                          // dstArrayElement
                1,                                          // descriptorCount
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,  // descriptorType
                nullptr,                                    // pImageInfo
                &transforms_buffer_info,                    // pBufferInfo
                nullptr                                     // pTexelBufferView
            },
            {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                VK_NULL_HANDLE,                             // dstSet
                1,                                          // dstBinding
                0,                                          // dstArrayElement
                1,                                          // descriptorCount
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          // descriptorType
                nullptr,                                    // pImageInfo
                &storage_buffer_info,                       // pBufferInfo
                nullptr                                     // pTexelBufferView
            }
        };

        materials_buffer_info.buffer  = materials_buf.get_buffer();
        materials_buffer_info.range   = materials_stride;
        transforms_buffer_info.buffer = transforms_buf.get_buffer();
        transforms_buffer_info.range  = transforms_stride;
        storage_buffer_info.buffer    = patch_geometry.get_faces_buffer();

        write_desc_sets[0].dstSet     = desc_set[1];
        write_desc_sets[1].dstSet     = desc_set[2];
        write_desc_sets[2].dstSet     = desc_set[2];

        vkUpdateDescriptorSets(vk_dev,
                               mstd::array_size(write_desc_sets),
                               write_desc_sets,
                               0,           // descriptorCopyCount
                               nullptr);    // pDescriptorCopies
    }

    return true;
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
    char window_title[sizeof(object_name) + 36];
    snprintf(window_title, sizeof(window_title),
             "%s - %s###Geometry Editor", get_editor_name(), get_object_name());

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
    if ( ! patch_geometry.send_to_gpu(cmdbuf))
        return false;

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

static const Image::Transition render_viewport_layout = {
    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    0,
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
};

bool GeometryEditor::draw_geometry_view(VkCommandBuffer cmdbuf,
                                        View&           dst_view,
                                        uint32_t        image_idx)
{
    Resources& res = dst_view.res[image_idx];

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

    if ( ! render_geometry(cmdbuf, dst_view, image_idx))
        return false;

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

bool GeometryEditor::draw_selection_feedback(VkCommandBuffer cmdbuf,
                                             View&           dst_view,
                                             uint32_t        image_idx)
{
    Resources& res = dst_view.res[image_idx];

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

bool GeometryEditor::set_patch_transforms(const View& dst_view, uint32_t transform_id)
{
    Transforms* const transforms = transforms_buf.get_ptr<Transforms>(transform_id, transforms_stride);
    assert(transforms);

    const Camera& camera = dst_view.camera[static_cast<int>(dst_view.view_type)];

    vmath::mat4 model_view;

    switch (dst_view.view_type) {

        case ViewType::free_moving:
            {
                const vmath::quat q{vmath::vec3{vmath::radians(camera.pitch), vmath::radians(camera.yaw), 0.0f}};
                const vmath::vec3 cam_vector{vmath::vec4(0, 0, camera.distance, 0) * vmath::mat4(q)};
                model_view = vmath::look_at(camera.pos - cam_vector, camera.pos);
            }
            break;

        case ViewType::front:
            model_view = vmath::look_at(camera.pos, vmath::vec3(camera.pos.x, camera.pos.y, 0));
            break;

        default:
            assert(0);
    }

    transforms->model_view = model_view;

    transforms->model_view_normal = vmath::transpose(vmath::inverse(vmath::mat3(model_view)));

    const float aspect = static_cast<float>(dst_view.width) / static_cast<float>(dst_view.height);

    if (dst_view.view_type == ViewType::free_moving) {
        transforms->proj = vmath::projection_vector(aspect,
                                                    vmath::radians(30.0f),
                                                    0.01f,      // near_plane
                                                    1000.0f);   // far_plane
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    }
    else {
        transforms->proj = vmath::ortho_vector(aspect,
                                               camera.view_height / int16_scale,
                                               0.01f,   // near_plane
                                               3.0f);   // far_plane
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    return transforms_buf.flush(transform_id, transforms_stride);
}

bool GeometryEditor::render_geometry(VkCommandBuffer cmdbuf,
                                     const View&     dst_view,
                                     uint32_t        image_idx)
{
    const uint32_t edge_mat_id = (image_idx * num_materials) + mat_object_edge;

    const uint32_t transform_id_base = image_idx * transforms_per_viewport;

    const uint32_t transform_id = transform_id_base + 0;

    uint32_t dynamic_offsets[] = {
        edge_mat_id * materials_stride,
        transform_id * transforms_stride
    };

    if ( ! set_patch_transforms(dst_view, transform_id))
        return false;

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, gray_patch_mat);

    send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

    vkCmdBindDescriptorSets(cmdbuf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            Sculptor::material_layout,
                            1,          // firstSet
                            2,          // descriptorSetCount
                            &desc_set[1],
                            mstd::array_size(dynamic_offsets),
                            dynamic_offsets);

    patch_geometry.render(cmdbuf);

    return true;
}

} // namespace Sculptor
