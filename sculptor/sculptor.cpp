// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "sculptor_geometry.h"
#include "sculptor_materials.h"

#include "../d_printf.h"
#include "../gui.h"
#include "../memory_heap.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

#include <math.h>

const char app_name[] = "Object Editor";

const int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                           | ImGuiConfigFlags_DockingEnable;

namespace {

    enum class ViewType {
        free_moving,
        front
    };

    struct Viewport {
        const char*     name;
        bool            enabled;
        uint32_t        id;
        ViewType        view_type;
        vmath::vec3     camera_pos;
        float           camera_distance;
        float           camera_yaw;
        float           camera_pitch;
        uint32_t        width;
        uint32_t        height;
        Image           color_buffer[max_swapchain_size];
        Image           depth_buffer[max_swapchain_size];
        VkDescriptorSet gui_tex[max_swapchain_size];
    };

    using GridVertex = Sculptor::Geometry::Vertex;

    enum MaterialsForShaders {
        mat_grid,
        mat_object_edge,
        num_materials
    };

}

static Viewport viewports[] = {
    { "Front View", true, 0, ViewType::front,       { 0.0f, 0.0f, -2.0f }, 4096.0f },
    { "3D View",    true, 1, ViewType::free_moving, { 0.0f, 0.0f,  0.0f },    0.2f }
};

// Which viewport has captured mouse
static int viewport_mouse = -1;

const unsigned gui_num_descriptors = mstd::array_size(viewports) * max_swapchain_size;

static VkSampler viewport_sampler;

uint32_t check_device_features()
{
    uint32_t missing_features = 0;

    missing_features += check_feature(&vk_features.features.tessellationShader);
    missing_features += check_feature(&vk_features.features.fillModeNonSolid);
    missing_features += check_feature(&vk_dyn_rendering_features.dynamicRendering);

    return missing_features;
}

static VkPipeline         grid_pipeline;
static VkPipeline         sculptor_object_pipeline;
static VkPipeline         sculptor_edge_pipeline;
static VkDescriptorSet    desc_set[3];
static Sculptor::Geometry patch_geometry;
static Buffer             materials_buf;
static Buffer             transforms_buf;
static Buffer             grid_lines_buf;
static uint32_t           materials_stride;
static uint32_t           transforms_stride;

struct Transforms {
    vmath::mat4 model_view;
    vmath::mat3 model_view_normal;
    vmath::vec4 proj;
    vmath::vec4 proj_w;
};

static constexpr uint32_t max_grid_lines          = 4096;
static constexpr uint32_t transforms_per_viewport = 1;
static constexpr float    int16_scale             = 32767.0f;

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

static bool create_grid_lines_buffer()
{
    return grid_lines_buf.allocate(Usage::dynamic,
                                   max_grid_lines * 2 * max_swapchain_size * sizeof(GridVertex),
                                   VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

static bool create_materials_buffer()
{
    materials_stride = static_cast<uint32_t>(mstd::align_up(
                static_cast<VkDeviceSize>(sizeof(ShaderMaterial)),
                vk_phys_props.properties.limits.minUniformBufferOffsetAlignment));

    return materials_buf.allocate(Usage::dynamic,
                                  materials_stride * max_swapchain_size * num_materials,
                                  VK_FORMAT_UNDEFINED,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
}

static bool create_transforms_buffer()
{
    transforms_stride = static_cast<uint32_t>(mstd::align_up(
                static_cast<VkDeviceSize>(sizeof(Transforms)),
                vk_phys_props.properties.limits.minUniformBufferOffsetAlignment));

    return transforms_buf.allocate(Usage::dynamic,
                                   transforms_stride * max_swapchain_size * mstd::array_size(viewports) * transforms_per_viewport,
                                   VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
}

static bool create_descriptor_sets()
{
    static VkDescriptorSetAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr,
        VK_NULL_HANDLE,                 // descriptorPool
        2,                              // descriptorSetCount
        &sculptor_desc_set_layout[1]    // pSetLayouts
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

static void set_material_buf(const MaterialInfo& mat_info, uint32_t mat_id)
{
    for (uint32_t i = 0; i < vk_num_swapchain_images; i++) {
        const uint32_t abs_mat_id = (i * num_materials) + mat_id;

        ShaderMaterial* const material = materials_buf.get_ptr<ShaderMaterial>(abs_mat_id, materials_stride);

        for (uint32_t comp = 0; comp < 3; comp++)
            material->diffuse_color[comp] = static_cast<float>(mat_info.diffuse_color[comp]) / 255.0f;

        material->diffuse_color[3] = 1.0f;
    }
}

bool init_assets()
{
    if ( ! create_samplers())
        return false;

    if ( ! create_material_layouts())
        return false;

    if ( ! create_materials_buffer())
        return false;

    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0, // location
            0, // binding
            VK_FORMAT_R16G16B16_SNORM,
            offsetof(Sculptor::Geometry::Vertex, pos)
        }
    };

    static const MaterialInfo grid_info = {
        {
            shader_sculptor_simple_vert,
            shader_sculptor_color_frag
        },
        vertex_attributes,
        mstd::array_size(vertex_attributes),
        sizeof(GridVertex),
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        0, // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        false,               // depth_test
        { 0x55, 0x55, 0x55 } // diffuse
    };

    if ( ! create_material(grid_info, &grid_pipeline))
        return false;
    set_material_buf(grid_info, mat_grid);

    static const MaterialInfo object_mat_info = {
        {
            shader_pass_through_vert,
            shader_sculptor_object_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        mstd::array_size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16, // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,                // depth_test
        { 0x00, 0x00, 0x00 } // diffuse
    };

    if ( ! create_material(object_mat_info, &sculptor_object_pipeline))
        return false;

    static const MaterialInfo edge_mat_info = {
        {
            shader_pass_through_vert,
            shader_sculptor_color_frag,
            shader_bezier_line_cubic_sculptor_tesc,
            shader_bezier_line_cubic_sculptor_tese
        },
        vertex_attributes,
        mstd::array_size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        4, // patch_control_points
        VK_POLYGON_MODE_LINE,
        VK_CULL_MODE_NONE,
        true,                // depth_test
        { 0xEE, 0xEE, 0xEE } // diffuse
    };

    if ( ! create_material(edge_mat_info, &sculptor_edge_pipeline))
        return false;
    set_material_buf(edge_mat_info, mat_object_edge);

    if ( ! materials_buf.flush())
        return false;

    if ( ! patch_geometry.allocate())
        return false;

    patch_geometry.set_cube();

    if ( ! create_transforms_buffer())
        return false;

    if ( ! create_grid_lines_buffer())
        return false;

    if ( ! create_descriptor_sets())
        return false;

    if ( ! init_gui(GuiClear::clear))
        return false;

    return true;
}

static VkDeviceSize heap_low_checkpoint;
static VkDeviceSize heap_high_checkpoint;
static bool         viewports_allocated;

static void free_viewport_images()
{
    if (viewports_allocated) {
        for (Viewport& viewport : viewports) {
            for (uint32_t i_img = 0; i_img < max_swapchain_size; i_img++) {
                viewport.color_buffer[i_img].destroy();
                viewport.depth_buffer[i_img].destroy();
            }
        }

        mem_mgr.restore_heap_checkpoint(heap_low_checkpoint, heap_high_checkpoint);
        heap_low_checkpoint  = 0;
        heap_high_checkpoint = 0;
        viewports_allocated  = false;
    }
}

static bool destroy_viewports()
{
    if ( ! idle_queue())
        return false;

    free_viewport_images();

    return true;
}

void notify_gui_heap_freed()
{
    free_viewport_images();
}

static bool allocate_viewports()
{
    if ( ! viewports_allocated)
        heap_low_checkpoint = mem_mgr.get_heap_checkpoint();

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        for (uint32_t i_img = 0; i_img < vk_num_swapchain_images; i_img++) {
            if (viewport.color_buffer[i_img].get_view())
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

            color_info.width  = viewport.width;
            color_info.height = viewport.height;
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

            depth_info.width  = viewport.width;
            depth_info.height = viewport.height;
            depth_info.format = vk_depth_format;

            if ( ! viewport.color_buffer[i_img].allocate(color_info))
                return false;

            if ( ! viewport.depth_buffer[i_img].allocate(depth_info))
                return false;

            if (viewport.gui_tex[i_img]) {

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
                image_info.imageView = viewport.color_buffer[i_img].get_view();
                write_desc.dstSet    = viewport.gui_tex[i_img];

                vkUpdateDescriptorSets(vk_dev, 1, &write_desc, 0, nullptr);
            }
            else {
                viewport.gui_tex[i_img] = ImGui_ImplVulkan_AddTexture(
                        viewport_sampler,
                        viewport.color_buffer[i_img].get_view(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

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

    const ImVec2 abs_mouse_pos = ImGui::GetMousePos();
    const bool   ctrl_down     = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const float  wheel_delta   = io.MouseWheel;

    if ( ! ctrl_down)
        viewport_mouse = -1;

    static ImVec2 prev_mouse_pos;
    const ImVec2 mouse_delta = ImVec2(abs_mouse_pos.x - prev_mouse_pos.x, abs_mouse_pos.y - prev_mouse_pos.y);
    prev_mouse_pos = abs_mouse_pos;

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

        for (Viewport& viewport : viewports)
            ImGui::Checkbox(viewport.name, &viewport.enabled);
    }
    ImGui::End();

    bool viewports_changed = false;

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        ImGui::Begin(viewport.name, &viewport.enabled, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();

        //const ImVec2 abs_window_pos = ImGui::GetItemRectMin();
        //const ImVec2 rel_mouse_pos  = ImVec2(abs_mouse_pos.x - abs_window_pos.x, abs_mouse_pos.y - abs_window_pos.y);

        {
            const ImVec2 content_size = ImGui::GetContentRegionAvail();

            if ((static_cast<uint32_t>(content_size.x) != viewport.width) ||
                (static_cast<uint32_t>(content_size.y) != viewport.height))
                viewports_changed = true;

            viewport.width  = static_cast<uint32_t>(content_size.x);
            viewport.height = static_cast<uint32_t>(content_size.y);

            ImGui::Image(reinterpret_cast<ImTextureID>(viewport.gui_tex[image_idx]),
                         ImVec2{static_cast<float>(viewport.width),
                                static_cast<float>(viewport.height)});
        }

        if (viewport_mouse == -1 && ctrl_down && ImGui::IsItemHovered())
            viewport_mouse = static_cast<int>(viewport.id);

        if (viewport.id == static_cast<uint32_t>(viewport_mouse)) {
            constexpr float rot_scale_factor   = 0.3f;
            constexpr float ortho_scale_factor = 0.0005f;

            switch (viewport.view_type) {

                case ViewType::free_moving:
                    viewport.camera_yaw   += rot_scale_factor * mouse_delta.x;
                    viewport.camera_pitch += rot_scale_factor * mouse_delta.y;
                    break;

                case ViewType::front:
                    viewport.camera_pos.x -= ortho_scale_factor * mouse_delta.x;
                    viewport.camera_pos.y += ortho_scale_factor * mouse_delta.y;
                    break;

                default:
                    assert(0);
            }
        }

        if ((wheel_delta != 0.0f) && (viewport.view_type != ViewType::free_moving)) {
            viewport.camera_distance += 1024.0f * wheel_delta;

            constexpr float min_dist = 128.0f;
            constexpr float max_dist = 65536.0f;

            if (viewport.camera_distance < min_dist)
                viewport.camera_distance = min_dist;
            else if (viewport.camera_distance > max_dist)
                viewport.camera_distance = max_dist;
        }

        ImGui::End();
    }

    if (viewports_changed && ! destroy_viewports())
        return false;

    if ( ! allocate_viewports())
        return false;

    return true;
}

static bool draw_grid(const Viewport& viewport, uint32_t image_idx, VkCommandBuffer buf, uint32_t transforms_dyn_offs)
{
    if (viewport.view_type != ViewType::front)
        return true;

    const uint32_t sub_buf_stride = max_grid_lines * 2 * sizeof(GridVertex);

    auto     vertices  = grid_lines_buf.get_ptr<GridVertex>(image_idx, sub_buf_stride);
    uint32_t num_lines = 0;

    const float half_seen_height = viewport.camera_distance * 0.5f;
    const float half_seen_width  = half_seen_height * static_cast<float>(viewport.width)
                                 / static_cast<float>(viewport.height);

    int32_t min_x = static_cast<int32_t>(floorf(viewport.camera_pos.x * int16_scale - half_seen_width));
    int32_t max_x = static_cast<int32_t>(ceilf( viewport.camera_pos.x * int16_scale + half_seen_width));
    int32_t min_y = static_cast<int32_t>(floorf(viewport.camera_pos.y * int16_scale - half_seen_height));
    int32_t max_y = static_cast<int32_t>(ceilf( viewport.camera_pos.y * int16_scale + half_seen_height));

    const int32_t     seen_height     = max_y - min_y;
    constexpr int32_t est_horiz_lines = 16;
    int32_t           vert_dist       = seen_height / est_horiz_lines;
    if (vert_dist == 0)
        vert_dist = 1;

    // Find the highest power of two less or equal than vert_dist
    for (;;) {
        // Clear lowermost set bit
        const int32_t new_vert_dist = vert_dist & (vert_dist - 1);
        if ( ! new_vert_dist)
            break;
        vert_dist = new_vert_dist;
    }

    const int16_t minor_step = (vert_dist < 4096) ? static_cast<int16_t>(vert_dist) : 4096;

    min_x -= min_x % minor_step + minor_step;
    max_x += (max_x % minor_step) + minor_step;
    min_y -= min_y % minor_step + minor_step;
    max_y += (max_y % minor_step) + minor_step;

    for (int32_t x = min_x; x <= max_x; x += minor_step) {
        assert(num_lines < max_grid_lines);

        vertices[0].pos[0] = static_cast<int16_t>(x);
        vertices[0].pos[1] = static_cast<int16_t>(min_y);
        vertices[0].pos[2] = 0;

        vertices[1].pos[0] = static_cast<int16_t>(x);
        vertices[1].pos[1] = static_cast<int16_t>(max_y);
        vertices[1].pos[2] = 0;

        vertices += 2;
        ++num_lines;
    }

    for (int32_t y = min_y; y <= max_y; y += minor_step) {
        assert(num_lines < max_grid_lines);

        vertices[0].pos[0] = static_cast<int16_t>(min_x);
        vertices[0].pos[1] = static_cast<int16_t>(y);
        vertices[0].pos[2] = 0;

        vertices[1].pos[0] = static_cast<int16_t>(max_x);
        vertices[1].pos[1] = static_cast<int16_t>(y);
        vertices[1].pos[2] = 0;

        vertices += 2;
        ++num_lines;
    }

    if ( ! grid_lines_buf.flush(image_idx, sub_buf_stride))
        return false;

    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline);

    send_viewport_and_scissor(buf,
                              static_cast<float>(viewport.width) / static_cast<float>(viewport.height),
                              viewport.width,
                              viewport.height);

    const uint32_t grid_mat_id = (image_idx * num_materials) + mat_grid;

    uint32_t dynamic_offsets[] = {
        grid_mat_id * materials_stride,
        transforms_dyn_offs
    };

    vkCmdBindDescriptorSets(buf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            sculptor_material_layout,
                            1,          // firstSet
                            2,          // descriptorSetCount
                            &desc_set[1],
                            mstd::array_size(dynamic_offsets),
                            dynamic_offsets);

    const VkDeviceSize vb_offset = image_idx * sub_buf_stride;

    vkCmdBindVertexBuffers(buf,
                           0, // firstBinding
                           1, // bindingCount
                           &grid_lines_buf.get_buffer(),
                           &vb_offset);

    vkCmdDraw(buf,
              num_lines * 2,
              num_lines,
              0,  // firstVertex
              0); // firstInstance

    return true;
}

static bool set_patch_transforms(const Viewport& viewport, uint32_t transform_id)
{
    Transforms* const transforms = transforms_buf.get_ptr<Transforms>(transform_id, transforms_stride);
    assert(transforms);

    vmath::mat4 model_view;

    switch (viewport.view_type) {

        case ViewType::free_moving:
            {
                const vmath::quat q{vmath::vec3{vmath::radians(viewport.camera_pitch), vmath::radians(viewport.camera_yaw), 0.0f}};
                const vmath::vec3 cam_vector{vmath::vec4(0, 0, viewport.camera_distance, 0) * vmath::mat4(q)};
                model_view = vmath::look_at(viewport.camera_pos - cam_vector, viewport.camera_pos);
            }
            break;

        case ViewType::front:
            model_view = vmath::look_at(viewport.camera_pos, vmath::vec3(viewport.camera_pos.x, viewport.camera_pos.y, 0));
            break;

        default:
            assert(0);
    }

    transforms->model_view = model_view;

    transforms->model_view_normal = vmath::transpose(vmath::inverse(vmath::mat3(model_view)));

    const float aspect = static_cast<float>(viewport.width) / static_cast<float>(viewport.height);

    if (viewport.view_type == ViewType::free_moving) {
        transforms->proj = vmath::projection_vector(aspect,
                                                    vmath::radians(30.0f),
                                                    0.01f,      // near_plane
                                                    1000.0f);   // far_plane
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    }
    else {
        transforms->proj = vmath::ortho_vector(aspect,
                                               viewport.camera_distance / int16_scale,
                                               0.01f,   // near_plane
                                               3.0f);   // far_plane
        transforms->proj_w = vmath::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    return transforms_buf.flush(transform_id, transforms_stride);
}

static bool render_view(const Viewport& viewport, uint32_t image_idx, VkCommandBuffer buf)
{
    const uint32_t edge_mat_id = (image_idx * num_materials) + mat_object_edge;

    const uint32_t transform_id_base = (image_idx * mstd::array_size(viewports) + viewport.id) * transforms_per_viewport;

    const uint32_t transform_id = transform_id_base + 0;

    uint32_t dynamic_offsets[] = {
        edge_mat_id * materials_stride,
        transform_id * transforms_stride
    };

    if ( ! set_patch_transforms(viewport, transform_id))
        return false;

    if ( ! draw_grid(viewport, image_idx, buf, transform_id * transforms_stride))
        return false;

    for (uint32_t i = 0; i < 2; i++) {
        vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          (i == 0) ? sculptor_object_pipeline : sculptor_edge_pipeline);

        send_viewport_and_scissor(buf,
                                  static_cast<float>(viewport.width) / static_cast<float>(viewport.height),
                                  viewport.width,
                                  viewport.height);

        vkCmdBindDescriptorSets(buf,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                sculptor_material_layout,
                                1,          // firstSet
                                2,          // descriptorSetCount
                                &desc_set[1],
                                mstd::array_size(dynamic_offsets),
                                dynamic_offsets);

        if (i == 0)
            patch_geometry.render(buf);
        else
            patch_geometry.render_edges(buf);
    }

    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
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

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        if ( ! patch_geometry.send_to_gpu(buf))
            return false;

        static const Image::Transition render_viewport_layout = {
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        viewport.color_buffer[image_idx].set_image_layout(buf, render_viewport_layout);

        if (viewport.depth_buffer[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            viewport.depth_buffer[image_idx].set_image_layout(buf, depth_init);

        color_att.imageView   = viewport.color_buffer[image_idx].get_view();
        color_att.clearValue  = make_clear_color(0.2f, 0.2f, 0.2f, 1);
        depth_att.imageView   = viewport.depth_buffer[image_idx].get_view();
        rendering_info.renderArea.extent.width  = viewport.width;
        rendering_info.renderArea.extent.height = viewport.height;

        vkCmdBeginRenderingKHR(buf, &rendering_info);

        if ( ! render_view(viewport, image_idx, buf))
            return false;

        vkCmdEndRenderingKHR(buf);

        static const Image::Transition gui_image_layout = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        };
        viewport.color_buffer[image_idx].set_image_layout(buf, gui_image_layout);
    }

    if ( ! send_gui_to_gpu(buf, image_idx))
        return false;

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
