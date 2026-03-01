// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_geom_edit.h"
#include "sculptor_geometry.h"
#include "sculptor_materials.h"
#include "../core/barrier.h"
#include "../core/d_printf.h"
#include "../core/gui_imgui.h"
#include "../core/load_png.h"
#include "../core/mstdc.h"

#include "sculptor_shaders.h"
#include "../core/shaders.h"

#include <algorithm>
#include <iterator>
#include <stdio.h>

#include "toolbar.png.h"

/*

Object editor UI
----------------

- One viewport, drawing sequence:
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


Modes of operation
------------------

* Select - default mode
    - Add to selection              LMB
    - Remove from selection         Shift+LMB
    - Add rectangle to selection    LMB drag
    - Remove rectangle from sel     Shift+LMB drag
    - Clear selection               ???
    - Select all                    Cmd+A
    - Pan                           Shift+Mouse move
    - Rotate                        Ctrl+Mouse move
    - Scale                         Mouse wheel (smooth)
    - Select vertices/edges/faces   1/2/3
    - Focus on selection            .                  << no toolbar!
* Other modes are actions
    - Actions are completed with click (LMB) or Space
    - Actions are cancelled with Esc
    - In each case go back to select mode
* Transform/Move
* Transform/Rotate
* Transform/Scale
* Extrude

Toolbar in Object Editor
------------------------

- New Object
    - Cube
- Edit
    - Undo                      Cmd+Z
    - Redo                      Shift+Cmd+Z
    - Copy                      Cmd+C
    - Paste                     Cmd+V
    - Cut                       Cmd+X
- Select
    - Vertices                  1
    - Edges                     2
    - Faces                     3
    - Clear selection           ???
- Viewport
    - Perspective view          5
    - Orthographic view Z       6 (toggle front/back)
    - Orthographic view X       7 (toggle right/left)
    - Orthographic view Y       8 (toggle top/bottom)
- View options
    - Toggle tessellation       Alt-T
    - Toggle wireframe          Alt-W
- Constraint (applies to move, scale, pan, rotate, edit operations, etc.)
    - X                         X
    - Y                         Y
    - Z                         Z
- Transform
    - Move                      G, GX, GY, GZ, click to finish or Space
    - Rotate                    R, click to finish or Space
    - Scale                     S, SX, SY, SZ, click to finish or Space
- Edit
    - Delete                    Del
    - Extrude                   E

*/

namespace {
    enum MaterialsForShaders {
        mat_grid,
        mat_vertex_sel,
        mat_wireframe,
        num_materials
    };

    enum FrameFlags : uint32_t {
        frame_flag_selection_active = 1u,
        frame_flag_wireframe_mode   = 2u,
    };

    struct FrameData {
        vmath::vec2 selection_rect_min;
        vmath::vec2 selection_rect_max;
        vmath::vec2 mouse_pos;
        uint32_t    flags;
        uint32_t    pad[1];
    };

    struct Transforms {
        vmath::mat4 model_view;
        vmath::mat3 model_view_normal;
        vmath::mat3 view_inverse; // mat3x4: last column of inverse is always [0, 0, 0, 1], so omitted
        vmath::vec4 proj;
        vmath::vec4 proj_w;
        vmath::vec2 pixel_dim;
    };

    constexpr float    fov_radians             = vmath::radians(30.0f);
    constexpr uint32_t transforms_per_viewport = 1;
    constexpr float    int16_scale             = 32767.0f;
    constexpr uint32_t max_grid_lines          = 4096;
    constexpr uint32_t max_objects             = 0x10000u;
    constexpr VkFormat selection_format        = (max_objects <= 0x10000u) ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT;

    ImageWithHostCopy  toolbar_image;
    VkSampler          point_sampler;

    struct ToolbarInfo {
        const char* tag;
        const char* tooltip;
        const char* combo;
        bool        first_in_group;
    };

    const ToolbarInfo toolbar_info[] = {
#       define X(tag, first, combo, desc) { "geom_tb_" #tag, desc, combo, first != 0 },
        TOOLBAR_BUTTONS
#       undef X
    };

    // Bits used to determine state of an object
    enum ObjectState : uint8_t {
        // Object is selected
        obj_selected = 1,
        // Mouse is over the object, or the object is inside selection rectangle
        obj_hovered  = 2,
    };
}

namespace Sculptor {

vmath::quat GeometryEditor::Camera::get_perspective_rotation_quat() const
{
    return vmath::quat::from_euler(vmath::radians(vmath::vec3{pitch, yaw, 0.0f}));
}

void GeometryEditor::Camera::move(const vmath::vec3& delta)
{
    constexpr vmath::vec3 max_pos{1.1f};
    const vmath::vec3 moved_pos = pos + get_perspective_rotation_quat().rotate(delta);
    const vmath::vec3 fixed_pos = vmath::max(-max_pos, vmath::min(moved_pos, max_pos));
    if (fixed_pos == moved_pos)
        pos = fixed_pos;
}

GeometryEditor::Camera GeometryEditor::Camera::get_rotated_camera() const
{
    Camera camera{*this};

    if (camera.pivot) {
        const vmath::vec3 cam_vec = camera.pos - *camera.pivot;

        const vmath::vec3 cam_pos_rot_v{camera.rot_pitch, camera.rot_yaw, 0.0f};
        const vmath::quat cam_pos_rot = vmath::quat::from_euler(vmath::radians(cam_pos_rot_v));

        camera.pos    = *camera.pivot + cam_pos_rot.rotate(cam_vec);
        camera.pitch += camera.rot_pitch;
        camera.yaw   += camera.rot_yaw;

        camera.pivot = std::nullopt;
    }

    return camera;
}

std::optional<vmath::vec3> GeometryEditor::read_mouse_world_pos(const View& src_view, uint32_t image_idx) const
{
    std::optional<vmath::vec3> ret;

    const float* const hover = src_view.res[image_idx].hover_pos_host_buf.get_ptr<float>();

    if (hover[3] > 0.5f)
        ret = vmath::vec3{hover[0], hover[1], hover[2]};
    else
        ret = std::nullopt;

    return ret;
}

std::optional<vmath::vec3> GeometryEditor::calc_grid_world_pos(const View& src_view) const
{
    if (src_view.width == 0 || src_view.height == 0)
        return std::nullopt;

    // TODO implement for other view types
    if (src_view.view_type != ViewType::free_moving)
        return std::nullopt;

    const Camera      camera = src_view.camera[static_cast<int>(ViewType::free_moving)].get_rotated_camera();
    const vmath::quat q      = camera.get_perspective_rotation_quat();

    const vmath::vec3 cam_forward = q.rotate(vmath::vec3{0, 0, 1});
    const vmath::vec3 cam_right   = q.rotate(vmath::vec3{1, 0, 0});
    const vmath::vec3 cam_up      = q.rotate(vmath::vec3{0, 1, 0});

    // Convert mouse position to NDC (note: Y is inverted)
    const float aspect = static_cast<float>(src_view.width) / static_cast<float>(src_view.height);
    const float ndc_x  = src_view.mouse_pos.x / static_cast<float>(src_view.width)  * 2.0f - 1.0f;
    const float ndc_y  = 1.0f - src_view.mouse_pos.y / static_cast<float>(src_view.height) * 2.0f;

    const float fov_tan = vmath::tan(fov_radians / 2);
    const vmath::vec3 ray_dir = cam_forward
                              + cam_right * (ndc_x * aspect * fov_tan)
                              + cam_up    * (ndc_y * fov_tan);

    if (std::abs(ray_dir.y) < 0.001f)
        return std::nullopt;

    const float t = -camera.pos.y / ray_dir.y;

    if (t < 0.0f)
        return std::nullopt;

    return camera.pos + ray_dir * t;
}

const char* GeometryEditor::get_editor_name() const
{
    return "Geometry Editor";
}

bool GeometryEditor::allocate_resources()
{
    if ( ! point_sampler) {
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

        VkResult res = CHK(vkCreateSampler(vk_dev, &sampler_info, nullptr, &point_sampler));
        if (res != VK_SUCCESS)
            return false;

    }

    if ( ! toolbar_texture) {
        if ( ! load_png(toolbar,
                        sizeof(toolbar),
                        &toolbar_image))
            return false;

        toolbar_texture = ImGui_ImplVulkan_AddTexture(point_sampler,
                                                      toolbar_image.get_view(),
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    if ( ! alloc_view_resources(&view, window_width, window_height, point_sampler))
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
            Usage::device_only
        };

        color_info.width  = width;
        color_info.height = height;
        color_info.format = swapchain_create_info.imageFormat;

        static ImageInfo obj_id_info {
            0, // width
            0, // height
            selection_format,
            1, // mip_levels
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            Usage::device_only
        };

        obj_id_info.width  = width;
        obj_id_info.height = height;

        static ImageInfo normal_info {
            0, // width
            0, // height
            VK_FORMAT_A2R10G10B10_UNORM_PACK32,
            1, // mip_levels
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            Usage::device_only
        };

        normal_info.width  = width;
        normal_info.height = height;

        static ImageInfo depth_info {
            0, // width
            0, // height
            VK_FORMAT_UNDEFINED,
            1, // mip_levels
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            Usage::device_only
        };

        depth_info.width  = width;
        depth_info.height = height;
        depth_info.format = vk_depth_format;

        if ( ! res.color.allocate(color_info, {"view color output", i_img}))
            return false;

        if ( ! res.obj_id.allocate(obj_id_info, {"g-buffer object id", i_img}))
            return false;

        if ( ! res.normal.allocate(normal_info, {"g-buffer normal", i_img}))
            return false;

        if ( ! res.depth.allocate(depth_info, {"view depth", i_img}))
            return false;

        if ( ! res.frame_data.allocate(Usage::device_only,
                                       sizeof(FrameData),
                                       VK_FORMAT_UNDEFINED,
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       {"frame data", i_img}))
            return false;

        if ( ! res.sel_host_buf.allocated()) {
            if ( ! res.sel_host_buf.allocate(Usage::host_only,
                                             max_objects,
                                             VK_FORMAT_UNDEFINED,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             {"selection host buffer", i_img}))
                return false;
            mstd::mem_zero(res.sel_host_buf.get_ptr<uint8_t>(), max_objects);
        }

        if ( ! res.hover_pos_buf.allocated()) {
            if ( ! res.hover_pos_buf.allocate(Usage::device_only,
                                              sizeof(float) * 4,
                                              VK_FORMAT_UNDEFINED,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                              {"hover position buffer", i_img}))
                return false;
        }

        if ( ! res.hover_pos_host_buf.allocated()) {
            static const float zeros[4] = {};
            if ( ! res.hover_pos_host_buf.allocate(Usage::host_only,
                                                   sizeof(float) * 4,
                                                   VK_FORMAT_UNDEFINED,
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   {"hover position host buffer", i_img}))
                return false;
            mstd::mem_copy(res.hover_pos_host_buf.get_ptr<float>(), zeros, sizeof(zeros));
        }

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

            // We don't use descriptor sets, put instead we use push descriptors.
            // But ImGui relies on descriptor sets, so we need to explicitly load
            // this Vulkan function here (which otherwise we don't want to load).
            VK_FUNCTION(vkUpdateDescriptorSets)(vk_dev, 1, &write_desc, 0, nullptr);
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

        res.color.free();
        res.obj_id.free();
        res.normal.free();
        res.depth.free();
        res.frame_data.free();
        res.sel_host_buf.free();
        res.hover_pos_buf.free();
        res.hover_pos_host_buf.free();
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

    if ( ! create_grid_buffer())
        return false;

    // Allocate selection buffer: max_objects bytes packed as uint32_t (4 objects per word)
    constexpr uint32_t sel_buf_size = max_objects; // 1 byte per object
    if ( ! sel_buf.allocate(Usage::device_only,
                            sel_buf_size,
                            VK_FORMAT_UNDEFINED,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            "selection buffer"))
        return false;

    // Create compute pipeline for clearing hover bits in sel_buf each frame
    {
        static const DescSetBindingInfo bindings[] = {
            { 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { 1, 0, 0, 0 }  // terminator: set_layout_id = num_layouts = 1
        };

        if ( ! create_compute_descriptor_set_layouts(bindings, 1, &sel_buf_ds_layout))
            return false;

        const ComputeShaderInfo shader_info = {
            shader_sculptor_clear_hover_comp,
            0 // no push constants
        };

        const VkDescriptorSetLayout ds_layouts[] = { sel_buf_ds_layout, VK_NULL_HANDLE };
        if ( ! create_compute_shader(shader_info, ds_layouts, nullptr,
                                     &sel_buf_pipe_layout, &clear_hover_pipe))
            return false;
    }

    view.camera[static_cast<int>(ViewType::free_moving)] = Camera{ { 0.0f, 0.01f, -0.2f },    0.0f, 0.0f, 1.0f };
    view.camera[static_cast<int>(ViewType::front)]       = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };
    view.camera[static_cast<int>(ViewType::back)]        = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };
    view.camera[static_cast<int>(ViewType::left)]        = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };
    view.camera[static_cast<int>(ViewType::right)]       = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };
    view.camera[static_cast<int>(ViewType::bottom)]      = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };
    view.camera[static_cast<int>(ViewType::top)]         = Camera{ { 0.0f, 0.0f,   0.0f }, 4096.0f, 0.0f, 0.0f };

    toolbar_state.view_perspective = true;

    return true;
}

void GeometryEditor::set_material_buf(const MaterialInfo& mat_info, uint32_t mat_id)
{
    for (uint32_t i = 0; i < vk_num_swapchain_images; i++) {
        const uint32_t abs_mat_id = (i * num_materials) + mat_id;

        Sculptor::ShaderMaterial* const material = materials_buf.get_ptr<Sculptor::ShaderMaterial>(abs_mat_id, materials_stride);

        material->diffuse_color[0] = static_cast<float>(mat_info.diffuse_color.red)   / 255.0f;
        material->diffuse_color[1] = static_cast<float>(mat_info.diffuse_color.green) / 255.0f;
        material->diffuse_color[2] = static_cast<float>(mat_info.diffuse_color.blue)  / 255.0f;
        material->diffuse_color[3] = 1.0f;
    }
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
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  "materials buffer"))
        return false;

    static const MaterialInfo object_mat_info = {
        {
            shader_sculptor_pass_through_vert,
            shader_sculptor_object_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f,    // depth_bias
        std::size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16,      // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,    // use_depth
        false,   // alpha_blend
        make_byte_color(0, 0, 0) // diffuse
    };

    if ( ! create_material(object_mat_info, &gray_patch_mat))
        return false;

    static const MaterialInfo gbuffer_mat_info = {
        {
            shader_sculptor_pass_through_vert,
            shader_sculptor_g_buffer_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f,    // depth_bias
        std::size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        { static_cast<uint8_t>(selection_format), VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16,      // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,    // use_depth
        false,   // alpha_blend
        make_byte_color(0, 0, 0) // diffuse
    };

    if ( ! create_material(gbuffer_mat_info, &gray_patch_gbuffer_mat))
        return false;

    static const MaterialInfo selection_mat_info = {
        {
            shader_sculptor_pass_through_vert,
            shader_sculptor_selection_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f,    // depth_bias
        std::size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        { VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16,      // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,    // use_depth
        false,   // alpha_blend
        make_byte_color(0, 0, 0) // diffuse
    };

    if ( ! create_material(selection_mat_info, &selection_mat))
        return false;

    static const MaterialInfo vertex_info = {
        {
            shader_sculptor_vertex_select_vert,
            shader_sculptor_vertex_select_frag
        },
        nullptr,
        0.0f,
        0,
        0,
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        0,       // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,    // use_depth
        false,   // alpha_blend
        make_byte_color(0.9372f, 0.9372f, 0.9568f) // diffuse
    };

    if ( ! create_material(vertex_info, &vertex_mat))
        return false;
    set_material_buf(vertex_info, mat_vertex_sel);

    static const MaterialInfo grid_info = {
        {
            shader_sculptor_simple_vert,
            shader_sculptor_color_frag
        },
        vertex_attributes,
        0.0f,    // depth_bias
        std::size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        0,       // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        true,    // use_depth
        false,   // alpha_blend
        make_byte_color(0.333f, 0.333f, 0.333f) // diffuse
    };

    if ( ! Sculptor::create_material(grid_info, &grid_mat))
        return false;
    set_material_buf(grid_info, mat_grid);

    static const MaterialInfo wireframe_mat_info = {
        {
            shader_bezier_line_cubic_sculptor_vert,
            shader_sculptor_color_frag
        },
        nullptr, // vertex_attributes (uses vertex pulling from SSBOs at bindings 3 and 4)
        0.0f,    // depth_bias
        0,       // num_vertex_attributes
        0,       // vertex_stride
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        0,       // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        false,   // use_depth (wireframe shows hidden edges too)
        false,   // alpha_blend
        make_byte_color(0.3f, 0.3f, 0.3f)
    };

    if ( ! Sculptor::create_material(wireframe_mat_info, &wireframe_mat))
        return false;
    set_material_buf(wireframe_mat_info, mat_wireframe);

    static const MaterialInfo wireframe_tess_mat_info = {
        {
            shader_sculptor_pass_through_vert,
            shader_sculptor_color_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f,    // depth_bias
        std::size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16,      // patch_control_points
        VK_POLYGON_MODE_LINE,
        VK_CULL_MODE_NONE,
        false,   // use_depth: show hidden edges too
        false,   // alpha_blend
        make_byte_color(0.93f, 0.93f, 0.93f) // wireframe color
    };

    if ( ! Sculptor::create_material(wireframe_tess_mat_info, &wireframe_tess_mat))
        return false;

    static const MaterialInfo lighting_mat_info = {
        {
            shader_sculptor_lighting_vert,
            shader_sculptor_lighting_frag
        },
        nullptr, // vertex_attributes
        0.0f,    // depth_bias
        0,       // num_vertex_attributes
        0,       // vertex_stride
        { VK_FORMAT_UNDEFINED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED, VK_FORMAT_DISABLED },
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        0,       // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        false,   // use_depth
        true,    // alpha_blend: alpha=0 passes through wireframe/background, alpha=1 draws surface
        make_byte_color(0, 0, 0) // diffuse (unused)
    };

    if ( ! Sculptor::create_material(lighting_mat_info, &lighting_mat, Sculptor::lighting_layout))
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
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   "transforms buffer");
}

bool GeometryEditor::create_grid_buffer()
{
    return grid_buf.allocate(Usage::dynamic,
                             max_grid_lines * 2 * max_swapchain_size * sizeof(Sculptor::Geometry::Vertex),
                             VK_FORMAT_UNDEFINED,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             "grid buffer");
}

static void push_descriptor(VkCommandBuffer               cmdbuf,
                            VkPipelineBindPoint           bind_point,
                            VkPipelineLayout              layout,
                            uint8_t                       binding,
                            uint8_t                       desc_type,
                            const VkDescriptorBufferInfo& buffer_info)
{
    static VkWriteDescriptorSet write_desc_set = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,             // dstSet
        0,                          // dstBinding
        0,                          // dstArrayElement
        1,                          // descriptorCount
        VK_DESCRIPTOR_TYPE_SAMPLER, // descriptorType
        nullptr,                    // pImageInfo
        nullptr,                    // pBufferInfo
        nullptr                     // pTexelBufferView
    };

    write_desc_set.dstBinding     = binding;
    write_desc_set.descriptorType = static_cast<VkDescriptorType>(desc_type);
    write_desc_set.pBufferInfo    = &buffer_info;

    vkCmdPushDescriptorSet(cmdbuf,
                           bind_point,
                           layout,
                           0, // set
                           1,
                           &write_desc_set);
}

static void push_descriptor(VkCommandBuffer              cmdbuf,
                            VkPipelineBindPoint          bind_point,
                            VkPipelineLayout             layout,
                            uint8_t                      binding,
                            const VkDescriptorImageInfo& image_info)
{
    static VkWriteDescriptorSet write_desc_set = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,                         // dstSet
        0,                                      // dstBinding
        0,                                      // dstArrayElement
        1,                                      // descriptorCount
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, // descriptorType
        nullptr,                                // pImageInfo
        nullptr,                                // pBufferInfo
        nullptr                                 // pTexelBufferView
    };

    write_desc_set.dstBinding = binding;
    write_desc_set.pImageInfo = &image_info;

    vkCmdPushDescriptorSet(cmdbuf,
                           bind_point,
                           layout,
                           0, // set
                           1,
                           &write_desc_set);
}

void GeometryEditor::gui_status_bar()
{
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
            static const char* const mode_names[] = {
#               define X(mode, name) name,
                MODE_LIST
#               undef X
            };

            static const char* const view_names[] = {
                "Perspective",
                "Front",
                "Back",
                "Left",
                "Right",
                "Bottom",
                "Top",
            };

            const unsigned view_idx = static_cast<unsigned>(view.view_type);
            assert(view_idx < std::size(view_names));

            ImGui::Text("%s", mode_names[static_cast<unsigned>(mode)]);
            ImGui::Separator();
            ImGui::Text("%s", view_names[view_idx]);
            ImGui::Separator();
            ImGui::Text("Mouse: %dx%d", static_cast<int>(view.mouse_pos.x), static_cast<int>(view.mouse_pos.y));
            if (view.mouse_world_pos) {
                ImGui::Separator();
                ImGui::Text("Hover: %.3f %.3f %.3f", view.mouse_world_pos->x, view.mouse_world_pos->y, view.mouse_world_pos->z);
            }
            const Camera& camera = view.camera[0];
            if (camera.pivot) {
                ImGui::Separator();
                ImGui::Text("Pivot: %.3f %.3f %.3f yaw %.3f pitch %.3f",
                            camera.pivot->x, camera.pivot->y, camera.pivot->z, camera.rot_yaw, camera.rot_pitch);
            }

            ImGui::EndMenuBar();
        }
    }
    ImGui::EndChild();
}

bool GeometryEditor::toolbar_button(ToolbarButton button, bool* checked)
{
    const ImVec2 button_size{static_cast<float>(toolbar_image.get_height()),
                             static_cast<float>(toolbar_image.get_height())};

    const uint32_t idx        = static_cast<uint32_t>(button);
    const uint32_t start_offs = idx * toolbar_image.get_height();
    const uint32_t end_offs   = start_offs + toolbar_image.get_height();

    const ImVec2 uv0{static_cast<float>((start_offs * 1.0) / toolbar_image.get_width()), 0};
    const ImVec2 uv1{static_cast<float>((end_offs   * 1.0) / toolbar_image.get_width()), 1};

    const ToolbarInfo& info = toolbar_info[idx];

    if (idx > 0)
        ImGui::SameLine(0.0f, info.first_in_group ? -1.0f : 0.0f);

    ImVec4 button_color;
    if (checked && *checked)
        button_color = ImVec4{0.18f, 0.18f, 0.17f, 1};
    else
        button_color = ImVec4{0.34f, 0.34f, 0.33f, 1};

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{3, 3});
    ImGui::PushStyleColor(ImGuiCol_Button,        button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.45f, 0.45f, 0.45f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4{0.20f, 0.20f, 0.20f, 1});

    const bool clicked = ImGui::ImageButton(info.tag,
                                            make_texture_id(toolbar_texture),
                                            button_size,
                                            uv0,
                                            uv1);

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    if (info.combo && info.combo[0])
        ImGui::SetItemTooltip("%s (%s)", info.tooltip, info.combo);
    else
        ImGui::SetItemTooltip("%s", info.tooltip);

    if (clicked && checked)
        *checked = ! *checked;

    return clicked;
}

void GeometryEditor::handle_mouse_actions(const UserInput& input, bool view_hovered)
{
    const bool mouse_moved = input.mouse_pos_delta.x != 0 || input.mouse_pos_delta.y != 0;

    // When mouse moves, but is not captured, detect a new mouse action
    if ( ! has_captured_mouse() && view_hovered && ! is_mouse_captured()) {

        assert(mouse_action == Action::none);

        if (mouse_moved && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)))
            mouse_action = Action::rotate;
        else if (mouse_moved && (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)))
            mouse_action = Action::pan;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            mouse_action = (mode == Mode::select) ? Action::select : Action::execute;

        if (mouse_action != Action::none) {
            capture_mouse();
            mouse_action_init = input.abs_mouse_pos;
        }
    }

    // Finish rotation
    if (mouse_action != Action::rotate) {
        Camera& camera = view.camera[static_cast<int>(view.view_type)];
        camera = camera.get_rotated_camera();
    }

    if (has_captured_mouse()) {
        switch (mouse_action) {

            case Action::rotate:
                if ( ! ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ! ImGui::IsKeyDown(ImGuiKey_RightCtrl))
                    release_mouse();
                else if (mouse_moved) {
                    constexpr float rot_scale_factor = 0.3f;

                    Camera& camera = view.camera[static_cast<int>(view.view_type)];

                    switch (view.view_type) {

                        case ViewType::free_moving:
                            // Initiate rotation
                            if (view.mouse_world_pos && ! camera.pivot) {
                                camera.pivot     = *view.mouse_world_pos;
                                camera.rot_yaw   = 0;
                                camera.rot_pitch = 0;
                            }
                            {
                                camera.rot_yaw += rot_scale_factor * input.mouse_pos_delta.x;

                                // Make sure pitch doesn't exceed limits
                                constexpr float max_pitch = 87.0f;
                                const float pitch_delta = rot_scale_factor * input.mouse_pos_delta.y;
                                const float old_pitch   = camera.pitch + camera.rot_pitch;
                                const float new_pitch   = vmath::clamp(old_pitch + pitch_delta, -max_pitch, max_pitch);

                                camera.rot_pitch += new_pitch - old_pitch;
                            }
                            break;

                        default: {
                            // TODO switch to free_moving and apply rotation
                            constexpr float view_bounds        = 1.1f;
                            const     float ortho_scale_factor = static_cast<float>(view.height) * 0.0000001f;
                            camera.pos.x = vmath::clamp(camera.pos.x - ortho_scale_factor * input.mouse_pos_delta.x, -view_bounds, view_bounds);
                            camera.pos.y = vmath::clamp(camera.pos.y + ortho_scale_factor * input.mouse_pos_delta.y, -view_bounds, view_bounds);
                            break;
                        }
                    }
                }
                break;

            case Action::pan:
                if ( ! ImGui::IsKeyDown(ImGuiKey_LeftShift) && ! ImGui::IsKeyDown(ImGuiKey_RightShift))
                    release_mouse();
                else if (mouse_moved) {
                    const float pan_factor = static_cast<float>(view.height) * 0.0000001f;

                    Camera& camera = view.camera[static_cast<int>(view.view_type)];

                    switch (view.view_type) {

                        default:
                            assert(view.view_type == ViewType::free_moving);
                            camera.move(vmath::vec3{-pan_factor, pan_factor, 0} * vmath::vec3{input.mouse_pos_delta});
                            break;

                        case ViewType::front:
                            camera.move(vmath::vec3{-pan_factor, pan_factor, 0} * vmath::vec3{input.mouse_pos_delta});
                            break;

                        case ViewType::back:
                            camera.move(vmath::vec3{pan_factor, pan_factor, 0} * vmath::vec3{input.mouse_pos_delta});
                            break;

                        case ViewType::left:
                            camera.move(vmath::vec3{0, pan_factor, pan_factor} *
                                        vmath::vec3{0, input.mouse_pos_delta.y, input.mouse_pos_delta.x});
                            break;

                        case ViewType::right:
                            camera.move(vmath::vec3{0, pan_factor, -pan_factor} *
                                        vmath::vec3{0, input.mouse_pos_delta.y, input.mouse_pos_delta.x});
                            break;

                        case ViewType::bottom:
                            camera.move(vmath::vec3{pan_factor, 0, pan_factor} *
                                        vmath::vec3{input.mouse_pos_delta.x, 0, input.mouse_pos_delta.y});
                            break;

                        case ViewType::top:
                            camera.move(vmath::vec3{-pan_factor, 0, pan_factor} *
                                        vmath::vec3{input.mouse_pos_delta.x, 0, input.mouse_pos_delta.y});
                            break;
                    }
                }
                break;

            case Action::select:
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    release_mouse();
                    // TODO finish selection
                }
                else if (mouse_moved) {
                    // TODO update selection rectangle
                }
                break;

            case Action::execute:
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    release_mouse();
                    finish_edit_mode();
                    switch_mode(Mode::select);
                }
                break;

            default:
                assert("missing action" == nullptr);
        }

        if ( ! has_captured_mouse()) {
            mouse_action = Action::none;
        }
    }
    else {
        assert(mouse_action == Action::none);
    }

    if ((mode == Mode::select) && view_hovered && (mouse_action != Action::select)) {
        // TODO draw hover selection of selectable items
    }

    if ((mode != Mode::select) && ! has_captured_mouse() && mouse_moved) {
        // TODO adjust modification
    }

    if (input.wheel_delta != 0) {

        Camera& camera = view.camera[static_cast<int>(view.view_type)];

        constexpr float perspective_zoom_factor = -0.02f;
        constexpr float ortho_zoom_factor       = 200.0f;

        switch (view.view_type) {

            default:
                assert(view.view_type == ViewType::free_moving);
                if (view.mouse_world_pos) {
                    const float       zoom_frac = input.wheel_delta * perspective_zoom_factor;
                    const vmath::vec3 to_target = *view.mouse_world_pos - camera.pos;
                    camera.pos = camera.pos + to_target * zoom_frac;
                } else {
                    camera.move(vmath::vec3{0, 0, input.wheel_delta * perspective_zoom_factor});
                }
                break;

            case ViewType::front:
            case ViewType::back:
            case ViewType::left:
            case ViewType::right:
            case ViewType::bottom:
            case ViewType::top:
                camera.view_height += input.wheel_delta * ortho_zoom_factor;
                break;
        }
    }
}

void GeometryEditor::handle_keyboard_actions()
{
    Mode new_mode = mode;

    const auto IsCtrl = []() -> bool {
#ifdef __APPLE__
        constexpr ImGuiKey left_ctrl  = ImGuiKey_LeftSuper;
        constexpr ImGuiKey right_ctrl = ImGuiKey_RightSuper;
#else
        constexpr ImGuiKey left_ctrl  = ImGuiKey_LeftCtrl;
        constexpr ImGuiKey right_ctrl = ImGuiKey_RightCtrl;
#endif

        return ImGui::IsKeyDown(left_ctrl) || ImGui::IsKeyDown(right_ctrl);
    };

    if (ImGui::IsKeyPressed(ImGuiKey_Z) && IsCtrl()) {
        // TODO undo
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Z) && IsCtrl() &&
        (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift))) {
        // TODO redo
    }

    if (ImGui::IsKeyPressed(ImGuiKey_C) && IsCtrl()) {
        // TODO copy
    }

    if (ImGui::IsKeyPressed(ImGuiKey_V) && IsCtrl()) {
        // TODO paste
    }

    if (ImGui::IsKeyPressed(ImGuiKey_X) && IsCtrl()) {
        // TODO cut
    }

    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        toolbar_state.select.vertices = ! toolbar_state.select.vertices;
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_2)) {
        toolbar_state.select.edges = ! toolbar_state.select.edges;
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        toolbar_state.select.faces = ! toolbar_state.select.faces;
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_5)) {
        toolbar_state.view_perspective = true;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = false;
        view.view_type = ViewType::free_moving;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_6)) {
        if (toolbar_state.view_ortho_z)
            view.view_type = (view.view_type == ViewType::front) ? ViewType::back : ViewType::front;
        else
            view.view_type = ViewType::front;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_7)) {
        if (toolbar_state.view_ortho_x)
            view.view_type = (view.view_type == ViewType::left) ? ViewType::right : ViewType::left;
        else
            view.view_type = ViewType::left;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = true;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_8)) {
        if (toolbar_state.view_ortho_y)
            view.view_type = (view.view_type == ViewType::bottom) ? ViewType::top : ViewType::bottom;
        else
            view.view_type = ViewType::bottom;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = true;
        toolbar_state.view_ortho_z = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_T) &&
            (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
        toolbar_state.toggle_tessellation = ! toolbar_state.toggle_tessellation;

    if (ImGui::IsKeyPressed(ImGuiKey_W) &&
            (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt)))
        toolbar_state.toggle_wireframe = ! toolbar_state.toggle_wireframe;

    if (ImGui::IsKeyPressed(ImGuiKey_X))
        toolbar_state.snap_x = ! toolbar_state.snap_x;

    if (ImGui::IsKeyPressed(ImGuiKey_Y))
        toolbar_state.snap_y = ! toolbar_state.snap_y;

    if (ImGui::IsKeyPressed(ImGuiKey_Z))
        toolbar_state.snap_z = ! toolbar_state.snap_z;

    if (ImGui::IsKeyPressed(ImGuiKey_G)) {
        toolbar_state.move = true;
        new_mode = Mode::move;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        toolbar_state.rotate = true;
        new_mode = Mode::rotate;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S)) {
        toolbar_state.scale = true;
        new_mode = Mode::scale;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        // TODO delete
    }

    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
        toolbar_state.extrude = true;
        new_mode = Mode::extrude;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        new_mode     = Mode::select;
        mouse_action = Action::none;
        if (has_captured_mouse())
            release_mouse();
    }

    switch_mode(new_mode);
}

bool GeometryEditor::gui_toolbar()
{
    // Skip if it's not loaded yet
    if ( ! toolbar_texture)
        return true;

    constexpr uint32_t margin = 10;
    ImGui::SetCursorPos(ImVec2{margin, margin + ImGui::GetTextLineHeightWithSpacing()});

    Mode new_mode = mode;

    if (toolbar_button(ToolbarButton::new_cube)) {
        // TODO new cube
    }

    if (toolbar_button(ToolbarButton::undo)) {
        // TODO undo
    }

    if (toolbar_button(ToolbarButton::redo)) {
        // TODO redo
    }

    if (toolbar_button(ToolbarButton::copy)) {
        // TODO copy
    }

    if (toolbar_button(ToolbarButton::paste)) {
        // TODO paste
    }

    if (toolbar_button(ToolbarButton::cut)) {
        // TODO cut
    }

    if (toolbar_button(ToolbarButton::sel_vertices, &toolbar_state.select.vertices)) {
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    if (toolbar_button(ToolbarButton::sel_edges, &toolbar_state.select.edges)) {
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    if (toolbar_button(ToolbarButton::sel_faces, &toolbar_state.select.faces)) {
        if (mode != Mode::select)
            saved_select = toolbar_state.select;
        new_mode = Mode::select;
    }

    toolbar_button(ToolbarButton::sel_clear);
    if (toolbar_button(ToolbarButton::view_perspective, &toolbar_state.view_perspective)) {
        toolbar_state.view_perspective = true;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = false;
        view.view_type = ViewType::free_moving;
    }

    if (toolbar_button(ToolbarButton::view_ortho_z, &toolbar_state.view_ortho_z)) {
        if (toolbar_state.view_ortho_z)
            view.view_type = ViewType::front;
        else
            view.view_type = (view.view_type == ViewType::front) ? ViewType::back : ViewType::front;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = true;
    }

    if (toolbar_button(ToolbarButton::view_ortho_x, &toolbar_state.view_ortho_x)) {
        if (toolbar_state.view_ortho_x)
            view.view_type = ViewType::left;
        else
            view.view_type = (view.view_type == ViewType::left) ? ViewType::right : ViewType::left;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = true;
        toolbar_state.view_ortho_y = false;
        toolbar_state.view_ortho_z = false;
    }

    if (toolbar_button(ToolbarButton::view_ortho_y, &toolbar_state.view_ortho_y)) {
        if (toolbar_state.view_ortho_y)
            view.view_type = ViewType::bottom;
        else
            view.view_type = (view.view_type == ViewType::bottom) ? ViewType::top : ViewType::bottom;
        toolbar_state.view_perspective = false;
        toolbar_state.view_ortho_x = false;
        toolbar_state.view_ortho_y = true;
        toolbar_state.view_ortho_z = false;
    }

    toolbar_button(ToolbarButton::toggle_tessell, &toolbar_state.toggle_tessellation);

    toolbar_button(ToolbarButton::toggle_wireframe, &toolbar_state.toggle_wireframe);

    toolbar_button(ToolbarButton::snap_x, &toolbar_state.snap_x);

    toolbar_button(ToolbarButton::snap_y, &toolbar_state.snap_y);

    toolbar_button(ToolbarButton::snap_z, &toolbar_state.snap_z);

    if (toolbar_button(ToolbarButton::move, &toolbar_state.move))
        new_mode = toolbar_state.move ? Mode::move : Mode::select;

    if (toolbar_button(ToolbarButton::rotate, &toolbar_state.rotate))
        new_mode = toolbar_state.rotate ? Mode::rotate : Mode::select;

    if (toolbar_button(ToolbarButton::scale, &toolbar_state.scale))
        new_mode = toolbar_state.scale ? Mode::scale : Mode::select;

    if (toolbar_button(ToolbarButton::erase)) {
        // TODO delete
    }

    if (toolbar_button(ToolbarButton::extrude, &toolbar_state.extrude))
        new_mode = toolbar_state.extrude ? Mode::extrude : Mode::select;

    switch_mode(new_mode);

    return true;
}

void GeometryEditor::switch_mode(Mode new_mode)
{
    if (new_mode != mode) {

        if (mode == Mode::select)
            saved_select = toolbar_state.select;

        toolbar_state.select  = { false, false, false };
        toolbar_state.move    = false;
        toolbar_state.rotate  = false;
        toolbar_state.scale   = false;
        toolbar_state.extrude = false;

        switch (new_mode) {

            case Mode::select:
                toolbar_state.select = saved_select;
                cancel_edit_mode();
                break;

            case Mode::move:
                toolbar_state.move = true;
                break;

            case Mode::rotate:
                toolbar_state.rotate = true;
                break;

            case Mode::scale:
                toolbar_state.scale = true;
                break;

            case Mode::extrude:
                toolbar_state.extrude = true;
                break;
        }

        mode = new_mode;
    }

    // At least one thing is always selectable
    if ((mode == Mode::select) && ! toolbar_state.select.vertices && ! toolbar_state.select.edges)
        toolbar_state.select.faces = true;
}

bool GeometryEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    handle_keyboard_actions();

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

    const float frame_height  = ImGui::GetFrameHeight();
    const float min_win_space = frame_height * 2;

    const ImVec2 content_size = ImGui::GetWindowSize();

    const uint32_t new_width  = static_cast<uint32_t>(content_size.x > 0.0f ? content_size.x : 1.0f);
    const uint32_t new_height = static_cast<uint32_t>(content_size.y > min_win_space ?
                                                      content_size.y - min_win_space : 1.0f);

    if ((new_width != window_width) || (new_height != window_height))
        *need_realloc = true;

    window_width  = new_width;
    window_height = new_height;

    const ImVec2 image_pos = ImGui::GetCursorPos();
    const ImVec2 image_size{static_cast<float>(window_width), static_cast<float>(window_height)};

    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});
        ImGui::Image(make_texture_id(view.res[image_idx].gui_texture),
                     image_size);
        ImGui::PopStyleVar();
    }

    UserInput local_input = input;
    local_input.abs_mouse_pos -= vmath::vec2(ImGui::GetItemRectMin());

    view.mouse_pos = local_input.abs_mouse_pos;

    // Read mouse position from geometry feedback
    view.mouse_world_pos = std::nullopt;
    if (view.res[image_idx].hover_pos_host_buf.allocated())
        view.mouse_world_pos = read_mouse_world_pos(view, image_idx);
    if ( ! view.mouse_world_pos)
        view.mouse_world_pos = calc_grid_world_pos(view);

    handle_mouse_actions(local_input, ImGui::IsItemHovered());

    const ImVec2 status_bar_pos = ImGui::GetCursorPos();

    if ( ! gui_toolbar())
        return false;

    const ImVec2 mask_pos{image_pos.x, ImGui::GetCursorPos().y};
    const ImVec2 mask_size{image_size.x, image_size.y - (mask_pos.y - image_pos.y)};

    ImGui::SetCursorPos(mask_pos);

    if (mask_size.x > 0 && mask_size.y > 0) {
        // Prevent mouse from dragging the entire window
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 0});
        ImGui::InvisibleButton("Obscure Geometry Editor View", mask_size);
        ImGui::PopStyleVar();
    }

    ImGui::SetCursorPos(status_bar_pos);

    gui_status_bar();

    ImGui::End();

    return true;
}

bool GeometryEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    if ( ! toolbar_image.send_to_gpu(cmdbuf))
        return false;

    // Send any updates/modifications to geometry to the GPU
    if ( ! patch_geometry.send_to_gpu(cmdbuf))
        return false;

    // TODO
    // * Draw solid geometry
    //   - Toggle tessellation
    //   - Toggle wireframe
    //   - Toggle material or just plain shaded
    // * Draw patch outline
    // * Draw edges (observe selection)
    // * Draw vertices (including control vertices) and connectors (observe selection)
    // * In all cases observe selection and hover highlight

    // Calculate and set up per-frame data
    set_frame_data(cmdbuf, image_idx);

    // Copy selection state from host to GPU, then clear hover bits in sel_buf (compute).
    // Barriers issued here cover both the sel_buf transfer and the frame_data transfer.
    // Surface hover detection is now performed inline in the G-buffer fragment shader
    // using frame_data, so no separate selection geometry pass is needed.
    if ( ! setup_selection(cmdbuf, image_idx))
        return false;

    // Draw G-buffer.  This is several output attachments:
    // - Object ID (R16_UINT or R32_UINT) - e.g. which face is rendered at each pixel
    // - Normal (A2R10G10B10_UNORM_PACK32) - surface normal at each screen pixel
    // - Depth (D32_SFLOAT) - the normal depth/stencil attachment, in the lighting pass we
    //   use viewport location and inverse proj*view matrix to restore world position coordinates
    //
    // The fragment shader also detects if the mouse hovers over any face (for shallow selection)
    if ( ! draw_geometry_pass(cmdbuf, view, image_idx))
        return false;

    // If deep selection is enabled, e.g. in wireframe mode, render geometry inside
    // selection rectangle to detect all hovered hidden faces
    if ( ! draw_deep_selection(cmdbuf, view, image_idx))
        return false;

    // If wireframe mode is enabled, render wireframe for the geometry
    if ( ! draw_wireframe_pass(cmdbuf, view, image_idx))
        return false;

    // Perform final rendering pass.  Use G-buffers as input and apply fragment
    // shader to each output pixel.  Use object ID from G-buffer and read
    // the selection state from object state buffer/data, to select color.
    //
    // Edge outlines for patches are drawn by using object ID from G-buffer
    // to detect boundaries of patches.
    if ( ! draw_lighting_pass(cmdbuf, view, image_idx))
        return false;

    // Render the grid
    if ( ! render_grid(cmdbuf, view, image_idx))
        return false;

    // Copy selection buffer back to host for next frame
    static const Buffer::Transition sel_buf_before_readback = {
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    };
    sel_buf.barrier(sel_buf_before_readback);

    send_barrier(cmdbuf);

    static const VkBufferCopy sel_copy_region = { 0, 0, max_objects };
    vkCmdCopyBuffer(cmdbuf, sel_buf.get_buffer(),
                    view.res[image_idx].sel_host_buf.get_buffer(),
                    1, &sel_copy_region);

    return true;
}

static const Image::Transition render_viewport_layout = {
    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
    VK_ACCESS_2_NONE,
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
};

bool GeometryEditor::draw_geometry_pass(VkCommandBuffer cmdbuf,
                                        View&           dst_view,
                                        uint32_t        image_idx)
{
    Resources& res = dst_view.res[image_idx];

    res.obj_id.barrier(render_viewport_layout);
    res.normal.barrier(render_viewport_layout);

    static const Image::Transition depth_init = {
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
    };

    if (res.depth.layout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
        res.depth.barrier(depth_init);

    send_barrier(cmdbuf);

    static VkRenderingAttachmentInfo gbuf_color_att[] = {
        // Object ID attachment
        {
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            nullptr,
            VK_NULL_HANDLE,                   // imageView (obj_id)
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_RESOLVE_MODE_NONE,
            VK_NULL_HANDLE,                   // resolveImageView
            VK_IMAGE_LAYOUT_UNDEFINED,        // resolveImageLayout
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_STORE,
            make_clear_color(0, 0, 0, 0)
        },
        // Normal attachment
        {
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            nullptr,
            VK_NULL_HANDLE,                   // imageView (normal)
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_RESOLVE_MODE_NONE,
            VK_NULL_HANDLE,                   // resolveImageView
            VK_IMAGE_LAYOUT_UNDEFINED,        // resolveImageLayout
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_STORE,
            make_clear_color(0, 0, 0, 0)
        },
    };

    gbuf_color_att[0].imageView = res.obj_id.get_view();
    gbuf_color_att[1].imageView = res.normal.get_view();

    static VkRenderingAttachmentInfo depth_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,                         // imageView
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,                         // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,              // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        make_clear_depth(0, 0)
    };

    depth_att.imageView = res.depth.get_view();

    static VkRenderingInfo gbuf_rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,                          // flags
        { },                        // renderArea
        1,                          // layerCount
        0,                          // viewMask
        2,                          // colorAttachmentCount
        gbuf_color_att,
        &depth_att,
        nullptr                     // pStencilAttachment
    };

    gbuf_rendering_info.renderArea.offset        = { 0, 0 };
    gbuf_rendering_info.renderArea.extent.width  = dst_view.width;
    gbuf_rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRendering(cmdbuf, &gbuf_rendering_info);

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, gray_patch_gbuffer_mat);
    vkCmdSetDepthTestEnable(cmdbuf, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmdbuf, VK_TRUE);

    send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

    if ( ! render_geometry(cmdbuf, dst_view, image_idx))
        return false;

    vkCmdEndRendering(cmdbuf);

    // Transition G-buffers and depth to shader-readable layout for the lighting pass
    static const Image::Transition gbuf_to_shader_read = {
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    res.obj_id.barrier(gbuf_to_shader_read);
    res.normal.barrier(gbuf_to_shader_read);

    static const Image::Transition depth_to_shader_read = {
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    res.depth.barrier(depth_to_shader_read);

    send_barrier(cmdbuf);

    return true;
}

bool GeometryEditor::draw_lighting_pass(VkCommandBuffer cmdbuf,
                                        View&           dst_view,
                                        uint32_t        image_idx)
{
    Resources& res = dst_view.res[image_idx];

    // Clear hover_pos_buf to zero so the shader only needs to write when there is a hit
    vkCmdFillBuffer(cmdbuf, res.hover_pos_buf.get_buffer(), 0, sizeof(float) * 4, 0);

    // Barrier: wait for sel_buf writes from geometry/deep selection passes before reading here.
    // Also: hover_pos_buf fill must complete before the fragment shader writes to it.
    static const Buffer::Transition sel_buf_for_lighting = {
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT
    };
    sel_buf.barrier(sel_buf_for_lighting);

    static const Buffer::Transition hover_fill_to_shader = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT
    };
    res.hover_pos_buf.barrier(hover_fill_to_shader);

    // Barrier: color attachment was written by wireframe pass; transition to COLOR_ATTACHMENT
    // for the lighting pass load (it is already in COLOR_ATTACHMENT_OPTIMAL from wireframe pass,
    // but we need to add a dependency on the previous color attachment write)
    static const Image::Transition color_for_lighting = {
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    res.color.barrier(color_for_lighting);

    send_barrier(cmdbuf);

    static VkRenderingAttachmentInfo light_color_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,                   // imageView
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,                   // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,        // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_LOAD,       // preserve wireframe/background from wireframe pass
        VK_ATTACHMENT_STORE_OP_STORE,
        make_clear_color(0, 0, 0, 0)
    };

    light_color_att.imageView = res.color.get_view();

    static VkRenderingInfo light_rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,                          // flags
        { },                        // renderArea
        1,                          // layerCount
        0,                          // viewMask
        1,                          // colorAttachmentCount
        &light_color_att,
        nullptr,                    // pDepthAttachment
        nullptr                     // pStencilAttachment
    };

    light_rendering_info.renderArea.offset        = { 0, 0 };
    light_rendering_info.renderArea.extent.width  = dst_view.width;
    light_rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRendering(cmdbuf, &light_rendering_info);

    static VkDescriptorImageInfo obj_id_image_info = {
        VK_NULL_HANDLE, // sampler (filled below)
        VK_NULL_HANDLE, // imageView
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    static VkDescriptorImageInfo normal_image_info = {
        VK_NULL_HANDLE, // sampler (filled below)
        VK_NULL_HANDLE, // imageView
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    static VkDescriptorImageInfo depth_image_info = {
        VK_NULL_HANDLE, // sampler (filled below)
        VK_NULL_HANDLE, // imageView
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    obj_id_image_info.sampler   = Sculptor::gbuffer_sampler;
    obj_id_image_info.imageView = res.obj_id.get_view();
    normal_image_info.sampler   = Sculptor::gbuffer_sampler;
    normal_image_info.imageView = res.normal.get_view();
    depth_image_info.sampler    = Sculptor::gbuffer_sampler;
    depth_image_info.imageView  = res.depth.get_view();

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    0, obj_id_image_info);

    static VkDescriptorBufferInfo transforms_buf_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    transforms_buf_info.buffer = transforms_buf.get_buffer();
    transforms_buf_info.offset = (image_idx * transforms_per_viewport) * transforms_stride;
    transforms_buf_info.range  = transforms_stride;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, transforms_buf_info);

    static VkDescriptorBufferInfo faces_buf_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    patch_geometry.write_faces_descriptor(&faces_buf_info);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, faces_buf_info);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    3, normal_image_info);
    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    4, depth_image_info);

    static VkDescriptorBufferInfo sel_buf_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    sel_buf_info.buffer = sel_buf.get_buffer();
    sel_buf_info.offset = 0;
    sel_buf_info.range  = VK_WHOLE_SIZE;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sel_buf_info);

    static VkDescriptorBufferInfo frame_data_buf_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    frame_data_buf_info.buffer = res.frame_data.get_buffer();
    frame_data_buf_info.offset = 0;
    frame_data_buf_info.range  = sizeof(FrameData);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, frame_data_buf_info);

    static VkDescriptorBufferInfo hover_pos_buf_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    hover_pos_buf_info.buffer = res.hover_pos_buf.get_buffer();
    hover_pos_buf_info.offset = 0;
    hover_pos_buf_info.range  = sizeof(float) * 4;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::lighting_layout,
                    7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, hover_pos_buf_info);

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, lighting_mat);
    vkCmdSetDepthTestEnable(cmdbuf, VK_FALSE);
    vkCmdSetDepthWriteEnable(cmdbuf, VK_FALSE);

    send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

    vkCmdDraw(cmdbuf, 3, 1, 0, 0);

    vkCmdEndRendering(cmdbuf);

    // Barrier: fragment shader write to hover_pos_buf must complete before transfer reads it
    static const Buffer::Transition hover_shader_to_copy = {
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    };
    res.hover_pos_buf.barrier(hover_shader_to_copy);

    send_barrier(cmdbuf);

    static const VkBufferCopy hover_copy_region = { 0, 0, sizeof(float) * 4 };
    vkCmdCopyBuffer(cmdbuf, res.hover_pos_buf.get_buffer(),
                    res.hover_pos_host_buf.get_buffer(), 1, &hover_copy_region);

    return true;
}

void GeometryEditor::set_frame_data(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    FrameData frame_data = {};

    if (mode == Mode::select) {
        const vmath::vec2 view_max{static_cast<float>(view.width  - 1),
                                   static_cast<float>(view.height - 1)};

        if (mouse_action == Action::none) {
            // Mouse hover: use a small snap rectangle around the cursor
            constexpr vmath::vec2 mouse_snap_px{1.0f};
            frame_data.selection_rect_min = vmath::max(view.mouse_pos - mouse_snap_px, vmath::vec2{0.0f});
            frame_data.selection_rect_max = vmath::min(view.mouse_pos + mouse_snap_px, view_max);
        }
        else if (mouse_action == Action::select) {
            // Active drag: use the full selection rectangle
            frame_data.selection_rect_min = vmath::max(vmath::min(view.mouse_pos, mouse_action_init), vmath::vec2{0.0f});
            frame_data.selection_rect_max = vmath::min(vmath::max(view.mouse_pos, mouse_action_init), view_max);
        }

        if (frame_data.selection_rect_max.x > frame_data.selection_rect_min.x &&
            frame_data.selection_rect_max.y > frame_data.selection_rect_min.y)
            frame_data.flags |= frame_flag_selection_active;
    }

    if (toolbar_state.toggle_wireframe)
        frame_data.flags |= frame_flag_wireframe_mode;

    frame_data.mouse_pos = view.mouse_pos;

    vkCmdUpdateBuffer(cmdbuf, view.res[image_idx].frame_data.get_buffer(), 0,
                      sizeof(frame_data), &frame_data);
}

bool GeometryEditor::setup_selection(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    Resources& res = view.res[image_idx];

    // Barrier: wait for previous frame's readback (TRANSFER_READ) before writing sel_buf from host.
    // Also covers frame_data written by vkCmdUpdateBuffer, readable in fragment shaders.
    static const Buffer::Transition sel_buf_before_copy = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT
    };
    sel_buf.barrier(sel_buf_before_copy);

    static const Buffer::Transition frame_data_after_update = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_UNIFORM_READ_BIT
    };
    res.frame_data.barrier(frame_data_after_update);

    send_barrier(cmdbuf);

    // Copy host selection state to GPU
    static const VkBufferCopy sel_copy_region = { 0, 0, max_objects };
    vkCmdCopyBuffer(cmdbuf, res.sel_host_buf.get_buffer(), sel_buf.get_buffer(),
                    1, &sel_copy_region);

    // Barrier: wait for copy before compute clears hover bits in sel_buf
    static const Buffer::Transition sel_buf_before_clear = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
    };
    sel_buf.barrier(sel_buf_before_clear);

    send_barrier(cmdbuf);

    // Dispatch compute shader to clear obj_hovered (bit 1) while preserving obj_selected (bit 0)
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, clear_hover_pipe);

    static VkDescriptorBufferInfo sel_buf_compute_info = {
        VK_NULL_HANDLE,
        0,
        0
    };
    sel_buf_compute_info.buffer = sel_buf.get_buffer();
    sel_buf_compute_info.offset = 0;
    sel_buf_compute_info.range  = VK_WHOLE_SIZE;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_COMPUTE, sel_buf_pipe_layout,
                    0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sel_buf_compute_info);

    constexpr uint32_t clear_hover_groups = max_objects / 4 / 64;
    vkCmdDispatch(cmdbuf, clear_hover_groups, 1, 1);

    // Barrier: wait for compute write before fragment shader reads/writes sel_buf
    static const Buffer::Transition sel_buf_after_clear = {
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
    };
    sel_buf.barrier(sel_buf_after_clear);

    send_barrier(cmdbuf);

    return true;
}

bool GeometryEditor::draw_deep_selection(VkCommandBuffer cmdbuf,
                                         View&           dst_view,
                                         uint32_t        image_idx)
{
    // Deep selection is only needed in wireframe mode during a rectangle drag.
    // It rerenders the geometry with depth test OFF so that objects hidden behind
    // other geometry are also marked as hovered.
    if ( ! toolbar_state.toggle_wireframe || mouse_action != Action::select)
        return true;

    const vmath::vec2 view_max{static_cast<float>(dst_view.width  - 1),
                               static_cast<float>(dst_view.height - 1)};
    const vmath::vec2 capture_rect_min = vmath::max(vmath::min(dst_view.mouse_pos, mouse_action_init), vmath::vec2{0.0f});
    const vmath::vec2 capture_rect_max = vmath::min(vmath::max(dst_view.mouse_pos, mouse_action_init), view_max);

    if (capture_rect_max.x <= capture_rect_min.x || capture_rect_max.y <= capture_rect_min.y)
        return true;

    // Barrier: geometry pass fragment shader has written to sel_buf; synchronize before more writes
    static const Buffer::Transition sel_buf_before_deep = {
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT
    };
    sel_buf.barrier(sel_buf_before_deep);

    send_barrier(cmdbuf);

    // Render with no color or depth attachment: the fragment shader writes to sel_buf
    // via atomicOr. No depth attachment needed since depth test is disabled.
    static VkRenderingInfo deep_sel_rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,       // flags
        { },     // renderArea
        1,       // layerCount
        0,       // viewMask
        0,       // colorAttachmentCount
        nullptr,
        nullptr, // pDepthAttachment (depth test off, no attachment needed)
        nullptr  // pStencilAttachment
    };

    deep_sel_rendering_info.renderArea.offset        = { 0, 0 };
    deep_sel_rendering_info.renderArea.extent.width  = dst_view.width;
    deep_sel_rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRendering(cmdbuf, &deep_sel_rendering_info);

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, selection_mat);

    // Deep selection: depth test OFF so all geometry along the ray is marked
    vkCmdSetDepthTestEnable(cmdbuf, VK_FALSE);
    vkCmdSetDepthWriteEnable(cmdbuf, VK_FALSE);

    // Set viewport to full image but restrict scissor to selection rectangle
    static VkViewport viewport = { 0, 0, 0, 0, 0, 1};
    VkRect2D          scissor;
    configure_viewport_and_scissor(&viewport, &scissor, dst_view.width, dst_view.height);
    vkCmdSetViewport(cmdbuf, 0, 1, &viewport);

    scissor.offset.x      = static_cast<int32_t>(capture_rect_min.x);
    scissor.offset.y      = static_cast<int32_t>(capture_rect_min.y);
    scissor.extent.width  = static_cast<uint32_t>(capture_rect_max.x - capture_rect_min.x);
    scissor.extent.height = static_cast<uint32_t>(capture_rect_max.y - capture_rect_min.y);
    vkCmdSetScissor(cmdbuf, 0, 1, &scissor);

    if ( ! render_geometry(cmdbuf, dst_view, image_idx))
        return false;

    vkCmdEndRendering(cmdbuf);

    return true;
}

bool GeometryEditor::draw_wireframe_pass(VkCommandBuffer cmdbuf,
                                         View&           dst_view,
                                         uint32_t        image_idx)
{
    Resources& res = dst_view.res[image_idx];

    res.color.barrier(render_viewport_layout);

    send_barrier(cmdbuf);

    // Clear the color attachment to the background color.
    // When wireframe mode is enabled, also draw Bezier edge curves on top.
    // The lighting pass uses LOAD_OP_LOAD and preserves this background for
    // pixels where no solid geometry is present (alpha=0 in lighting shader).
    static VkRenderingAttachmentInfo wire_color_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,                   // imageView (filled below)
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,                   // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,        // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        make_clear_color(0.2f, 0.2f, 0.2f, 1.0f) // background color
    };

    wire_color_att.imageView = res.color.get_view();

    static VkRenderingInfo wire_rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,       // flags
        { },     // renderArea
        1,       // layerCount
        0,       // viewMask
        1,       // colorAttachmentCount
        &wire_color_att,
        nullptr, // pDepthAttachment
        nullptr  // pStencilAttachment
    };

    wire_rendering_info.renderArea.offset        = { 0, 0 };
    wire_rendering_info.renderArea.extent.width  = dst_view.width;
    wire_rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRendering(cmdbuf, &wire_rendering_info);

    if (toolbar_state.toggle_wireframe && patch_geometry.get_num_faces() > 0) {
        vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe_tess_mat);
        vkCmdSetDepthTestEnable(cmdbuf, VK_FALSE);
        vkCmdSetDepthWriteEnable(cmdbuf, VK_FALSE);

        send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

        VkDescriptorBufferInfo buffer_info = {
            VK_NULL_HANDLE,
            0,
            0
        };

        const uint32_t wire_mat_id  = (image_idx * num_materials) + mat_wireframe;
        const uint32_t transform_id = image_idx * transforms_per_viewport;

        buffer_info.buffer = materials_buf.get_buffer();
        buffer_info.offset = wire_mat_id * materials_stride;
        buffer_info.range  = materials_stride;
        push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

        buffer_info.buffer = transforms_buf.get_buffer();
        buffer_info.offset = transform_id * transforms_stride;
        buffer_info.range  = transforms_stride;
        push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                        1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

        patch_geometry.write_faces_descriptor(&buffer_info);
        push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                        2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_info);

        patch_geometry.render(cmdbuf);
    }

    vkCmdEndRendering(cmdbuf);

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
                const Camera      r_camera   = camera.get_rotated_camera();
                const vmath::quat q          = r_camera.get_perspective_rotation_quat();
                const vmath::vec3 cam_vector = q.rotate(vmath::vec3{0, 0, 1});
                model_view = vmath::look_at(r_camera.pos, r_camera.pos + cam_vector, vmath::vec3{0, 1, 0});
            }
            break;

        case ViewType::front:
            model_view = vmath::look_at(vmath::vec3{camera.pos.x, camera.pos.y, -2},
                                        vmath::vec3{camera.pos.x, camera.pos.y, 0},
                                        vmath::vec3{0, 1, 0});
            break;

        case ViewType::back:
            model_view = vmath::look_at(vmath::vec3{camera.pos.x, camera.pos.y, 2},
                                        vmath::vec3{camera.pos.x, camera.pos.y, 0},
                                        vmath::vec3{0, 1, 0});
            break;

        case ViewType::left:
            model_view = vmath::look_at(vmath::vec3{-2, camera.pos.y, camera.pos.z},
                                        vmath::vec3{0, camera.pos.y, camera.pos.z},
                                        vmath::vec3{0, 1, 0});
            break;

        case ViewType::right:
            model_view = vmath::look_at(vmath::vec3{2, camera.pos.y, camera.pos.z},
                                        vmath::vec3{0, camera.pos.y, camera.pos.z},
                                        vmath::vec3{0, 1, 0});
            break;

        case ViewType::bottom:
            model_view = vmath::look_at(vmath::vec3{camera.pos.x, -2, camera.pos.z},
                                        vmath::vec3{camera.pos.x, 0, camera.pos.z},
                                        vmath::vec3{0, 0, 1});
            break;

        case ViewType::top:
            model_view = vmath::look_at(vmath::vec3{camera.pos.x, 2, camera.pos.z},
                                        vmath::vec3{camera.pos.x, 0, camera.pos.z},
                                        vmath::vec3{0, 0, 1});
            break;

        default:
            assert(0);
    }

    transforms->model_view = model_view;

    transforms->model_view_normal = vmath::transpose(vmath::inverse(vmath::mat3(model_view)));

    // Compute inverse of the rigid-body view matrix analytically (no general mat4 inverse needed).
    // The last column of the inverse is always [0,0,0,1] so we store only 3 columns as mat3x4.
    // transpose(mat3(M)) gives R^T; the eye position goes into the padding row (row 3).
    // Eye position = -t * R^T = -dot(t, each column of inv).
    {
        vmath::mat3 inv = vmath::transpose(vmath::mat3(model_view));
        const vmath::vec3 t{model_view.a30, model_view.a31, model_view.a32};
        inv.a30 = -vmath::dot_product(t, vmath::column<3>(inv, 0));
        inv.a31 = -vmath::dot_product(t, vmath::column<3>(inv, 1));
        inv.a32 = -vmath::dot_product(t, vmath::column<3>(inv, 2));
        transforms->view_inverse = inv;
    }

    const float aspect = static_cast<float>(dst_view.width) / static_cast<float>(dst_view.height);

    constexpr float near_plane = 0.01f;
    constexpr float far_plane  = 3.0f;

    if (dst_view.view_type == ViewType::free_moving) {
        transforms->proj = vmath::projection_vector(aspect,
                                                    fov_radians,
                                                    near_plane,
                                                    far_plane);
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    }
    else {
        transforms->proj = vmath::ortho_vector(aspect,
                                               camera.view_height / int16_scale,
                                               near_plane,
                                               far_plane);
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    transforms->pixel_dim = vmath::vec2(2.0f) / vmath::vec2(static_cast<float>(dst_view.width),
                                                            static_cast<float>(dst_view.height));

    return transforms_buf.flush(transform_id, transforms_stride);
}

bool GeometryEditor::render_geometry(VkCommandBuffer cmdbuf,
                                     const View&     dst_view,
                                     uint32_t        image_idx)
{
    const uint32_t transform_id_base = image_idx * transforms_per_viewport;

    const uint32_t transform_id = transform_id_base + 0;

    if ( ! set_patch_transforms(dst_view, transform_id))
        return false;

    VkDescriptorBufferInfo buffer_info = {
        VK_NULL_HANDLE, // buffer
        0,              // offset
        0               // range
    };

    buffer_info.buffer = transforms_buf.get_buffer();
    buffer_info.offset = transform_id * transforms_stride;
    buffer_info.range  = transforms_stride;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

    patch_geometry.write_faces_descriptor(&buffer_info);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_info);

    // TODO refactor drawing and selecting edges and vertices
    patch_geometry.write_edge_indices_descriptor(&buffer_info);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_info);

    patch_geometry.write_edge_vertices_descriptor(&buffer_info);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_info);

    buffer_info.buffer = sel_buf.get_buffer();
    buffer_info.offset = 0;
    buffer_info.range  = VK_WHOLE_SIZE;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_info);

    buffer_info.buffer = view.res[image_idx].frame_data.get_buffer();
    buffer_info.offset = 0;
    buffer_info.range  = sizeof(FrameData);

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

    patch_geometry.render(cmdbuf);

#if 0
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, vertex_mat);

    send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

    const uint32_t vertex_mat_id = (image_idx * num_materials) + mat_vertex_sel;

    buffer_info.buffer = materials_buf.get_buffer();
    buffer_info.offset = vertex_mat_id * materials_stride;
    buffer_info.range  = materials_stride;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

    patch_geometry.render_vertices(cmdbuf);
#endif

    return true;
}

bool GeometryEditor::render_grid(VkCommandBuffer cmdbuf,
                                 View&           dst_view,
                                 uint32_t        image_idx)
{
    const uint32_t sub_buf_stride = max_grid_lines * 2 * sizeof(Sculptor::Geometry::Vertex);

    auto     vertices  = grid_buf.get_ptr<Sculptor::Geometry::Vertex>(image_idx, sub_buf_stride);
    uint32_t num_lines = 0;

    uint32_t idx_1  = 0;
    uint32_t idx_2  = 2;
    uint32_t idx_z  = 1;
    int32_t  min_1  = -0x8000;
    int32_t  max_1  = 0x8000;
    int32_t  step_1 = 0x800;
    int32_t  min_2  = -0x8000;
    int32_t  max_2  = 0x8000;
    int32_t  step_2 = 0x800;

    const auto fix_pos = [](int32_t x) -> int16_t {
        return (x == 0x8000) ? 0x7FFF : static_cast<int16_t>(x);
    };

    switch (dst_view.view_type) {
        case ViewType::front:
        case ViewType::back:
            idx_2 = 1;
            idx_z = 2;
            break;

        case ViewType::right:
        case ViewType::left:
            idx_1 = 2;
            idx_2 = 1;
            idx_z = 0;
            break;

        default:
            break;
    }

    for (int32_t x = min_1; x <= max_1 && num_lines < max_grid_lines; x += step_1, num_lines++) {
        Sculptor::Geometry::Vertex* const vtx = &vertices[num_lines * 2];
        vtx[0].pos[idx_1] = fix_pos(x);
        vtx[0].pos[idx_2] = fix_pos(min_2);
        vtx[0].pos[idx_z] = 0;
        vtx[1].pos[idx_1] = fix_pos(x);
        vtx[1].pos[idx_2] = fix_pos(max_2);
        vtx[1].pos[idx_z] = 0;
    }

    for (int32_t x = min_2; x <= max_2 && num_lines < max_grid_lines; x += step_2, num_lines++) {
        Sculptor::Geometry::Vertex* const vtx = &vertices[num_lines * 2];
        vtx[0].pos[idx_2] = fix_pos(x);
        vtx[0].pos[idx_1] = fix_pos(min_1);
        vtx[0].pos[idx_z] = 0;
        vtx[1].pos[idx_2] = fix_pos(x);
        vtx[1].pos[idx_1] = fix_pos(max_1);
        vtx[1].pos[idx_z] = 0;
    }

    if ( ! grid_buf.flush(image_idx, sub_buf_stride))
        return false;

    Resources& res = dst_view.res[image_idx];

    // Transition depth from shader-read (used in lighting pass) to read-only depth
    // attachment so it can be used for depth testing during grid rendering
    static const Image::Transition depth_for_grid = {
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
    };
    res.depth.barrier(depth_for_grid);

    send_barrier(cmdbuf);

    static VkRenderingAttachmentInfo grid_color_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,                   // imageView (filled below)
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,                   // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,        // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_ATTACHMENT_STORE_OP_STORE,
        make_clear_color(0, 0, 0, 0)
    };

    grid_color_att.imageView = res.color.get_view();

    static VkRenderingAttachmentInfo grid_depth_att = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        nullptr,
        VK_NULL_HANDLE,                              // imageView (filled below)
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
        VK_RESOLVE_MODE_NONE,
        VK_NULL_HANDLE,                              // resolveImageView
        VK_IMAGE_LAYOUT_UNDEFINED,                   // resolveImageLayout
        VK_ATTACHMENT_LOAD_OP_LOAD,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,
        make_clear_depth(0, 0)
    };

    grid_depth_att.imageView = res.depth.get_view();

    static VkRenderingInfo grid_rendering_info = {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
        nullptr,
        0,                          // flags
        { },                        // renderArea
        1,                          // layerCount
        0,                          // viewMask
        1,                          // colorAttachmentCount
        &grid_color_att,
        &grid_depth_att,
        nullptr                     // pStencilAttachment
    };

    grid_rendering_info.renderArea.offset        = { 0, 0 };
    grid_rendering_info.renderArea.extent.width  = dst_view.width;
    grid_rendering_info.renderArea.extent.height = dst_view.height;

    vkCmdBeginRendering(cmdbuf, &grid_rendering_info);

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_mat);
    vkCmdSetDepthTestEnable(cmdbuf, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmdbuf, VK_FALSE);

    send_viewport_and_scissor(cmdbuf, dst_view.width, dst_view.height);

    const uint32_t grid_mat_id = (image_idx * num_materials) + mat_grid;

    VkDescriptorBufferInfo buffer_info = {
        VK_NULL_HANDLE, // buffer
        0,              // offset
        0               // range
    };

    buffer_info.buffer = materials_buf.get_buffer();
    buffer_info.offset = grid_mat_id * materials_stride;
    buffer_info.range  = materials_stride;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

    const uint32_t transform_id_base = image_idx * transforms_per_viewport;

    const uint32_t transform_id = transform_id_base + 0;

    buffer_info.buffer = transforms_buf.get_buffer();
    buffer_info.offset = transform_id * transforms_stride;
    buffer_info.range  = transforms_stride;

    push_descriptor(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, Sculptor::material_layout,
                    1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer_info);

    const VkDeviceSize vb_offset = image_idx * sub_buf_stride;

    vkCmdBindVertexBuffers(cmdbuf,
                           0, // firstBinding
                           1, // bindingCount
                           &grid_buf.get_buffer(),
                           &vb_offset);

    vkCmdDraw(cmdbuf,
              num_lines * 2,
              1,  // instanceCount
              0,  // firstVertex
              0); // firstInstance

    vkCmdEndRendering(cmdbuf);

    // Transition color output to shader-read layout for the GUI
    static const Image::Transition gui_image_layout = {
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };
    res.color.barrier(gui_image_layout);

    send_barrier(cmdbuf);

    return true;
}

void GeometryEditor::finish_edit_mode()
{
}

void GeometryEditor::cancel_edit_mode()
{
}

} // namespace Sculptor
