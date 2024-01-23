// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geometry.h"
#include "sculptor_materials.h"
#include "sculptor_geom_edit.h"

#include "../d_printf.h"
#include "../gui.h"
#include "../gui_imgui.h"
#include "../memory_heap.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

#include <math.h>
#include <stdio.h>

const char app_name[] = "Object Editor";

const int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard
                           | ImGuiConfigFlags_DockingEnable;

namespace {

    enum class ViewType {
        free_moving,
        front
    };

    enum class MouseMode {
        select_faces,
        select_edges
    };

    struct MouseSelection {
        bool     is_active;
        uint32_t x;
        uint32_t y;
        uint32_t object_id;
        uint32_t clicked_id;
    };

    struct Viewport {
        const char*     name;
        bool            enabled;
        uint32_t        id;
        ViewType        view_type;
        vmath::vec3     camera_pos;
        union {
            float       camera_distance;
            float       view_height;
        };
        float           camera_yaw;
        float           camera_pitch;
        MouseSelection  selection;

        // Rendered viewport image, sampled as texture by ImGui
        uint32_t        width;
        uint32_t        height;
        Image           color_buffer[max_swapchain_size];
        Image           depth_buffer[max_swapchain_size];
        VkDescriptorSet gui_tex[max_swapchain_size];

        // Small images used for querying selection under mouse cursor
        Image           select_query[max_swapchain_size];
        Image           select_query_host[max_swapchain_size];
        bool            select_query_pending[max_swapchain_size];
    };

    using GridVertex = Sculptor::Geometry::Vertex;

    enum MaterialsForShaders {
        mat_grid,
        mat_object_edge,
        num_materials
    };

}

static Sculptor::GeometryEditor geometry_editor;

// Global list of all possible editor windows, this collection is used for generic handling
// of editor windows, like drawing and event passing to visible editors
static Sculptor::Editor* const editors[] = {
    &geometry_editor
};

static Viewport viewports[] = {
    { "Front View", false, 0, ViewType::front,       { 0.0f, 0.0f, -2.0f }, { 4096.0f  } },
    { "3D View",    false, 1, ViewType::free_moving, { 0.0f, 0.0f,  0.0f }, {    0.25f }, 0.0f, 1.0f }
};

// Which viewport has captured mouse
static int viewport_mouse = -1;

static MouseMode mouse_mode = MouseMode::select_faces;

// +1 for geometry editor
const unsigned gui_num_descriptors = (mstd::array_size(viewports) + 1) * max_swapchain_size;

static VkSampler viewport_sampler;

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

static VkPipeline         grid_pipeline;
static VkPipeline         sculptor_object_pipeline;
static VkPipeline         sculptor_object_id_pipeline;
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
static constexpr uint32_t select_query_range      = 5;
static constexpr uint32_t select_mid_point        = (select_query_range >> 1) + 1;

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
                                   max_grid_lines * 2 * max_swapchain_size * mstd::array_size(viewports) * sizeof(GridVertex),
                                   VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

static bool create_materials_buffer()
{
    materials_stride = static_cast<uint32_t>(mstd::align_up(
                static_cast<VkDeviceSize>(sizeof(Sculptor::ShaderMaterial)),
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

static void set_material_buf(const Sculptor::MaterialInfo& mat_info, uint32_t mat_id)
{
    for (uint32_t i = 0; i < vk_num_swapchain_images; i++) {
        const uint32_t abs_mat_id = (i * num_materials) + mat_id;

        Sculptor::ShaderMaterial* const material = materials_buf.get_ptr<Sculptor::ShaderMaterial>(abs_mat_id, materials_stride);

        for (uint32_t comp = 0; comp < 3; comp++)
            material->diffuse_color[comp] = static_cast<float>(mat_info.diffuse_color[comp]) / 255.0f;

        material->diffuse_color[3] = 1.0f;
    }
}

bool init_assets()
{
    geometry_editor.set_object_name("unnamed");

    if ( ! create_samplers())
        return false;

    if ( ! Sculptor::create_material_layouts())
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

    static const Sculptor::MaterialInfo grid_info = {
        {
            shader_sculptor_simple_vert,
            shader_sculptor_color_frag
        },
        vertex_attributes,
        0.0f, // depth_bias
        mstd::array_size(vertex_attributes),
        sizeof(GridVertex),
        VK_FORMAT_UNDEFINED,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        0, // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        true,                // depth_test
        { 0x55, 0x55, 0x55 } // diffuse
    };

    if ( ! Sculptor::create_material(grid_info, &grid_pipeline))
        return false;
    set_material_buf(grid_info, mat_grid);

    static const Sculptor::MaterialInfo object_mat_info = {
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

    if ( ! Sculptor::create_material(object_mat_info, &sculptor_object_pipeline))
        return false;

    static const Sculptor::MaterialInfo edge_mat_info = {
        {
            shader_pass_through_vert,
            shader_sculptor_color_frag,
            shader_bezier_line_cubic_sculptor_tesc,
            shader_bezier_line_cubic_sculptor_tese
        },
        vertex_attributes,
        2048.0f, // depth_bias
        mstd::array_size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        VK_FORMAT_UNDEFINED,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        4, // patch_control_points
        VK_POLYGON_MODE_LINE,
        VK_CULL_MODE_NONE,
        true,                // depth_test
        { 0xEE, 0xEE, 0xEE } // diffuse
    };

    if ( ! Sculptor::create_material(edge_mat_info, &sculptor_edge_pipeline))
        return false;
    set_material_buf(edge_mat_info, mat_object_edge);

    if ( ! materials_buf.flush())
        return false;

    static const Sculptor::MaterialInfo object_id_info = {
        {
            shader_pass_through_vert,
            shader_sculptor_object_id_frag,
            shader_bezier_surface_cubic_sculptor_tesc,
            shader_bezier_surface_cubic_sculptor_tese
        },
        vertex_attributes,
        0.0f, // depth_bias
        mstd::array_size(vertex_attributes),
        sizeof(Sculptor::Geometry::Vertex),
        VK_FORMAT_R32_UINT,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16, // patch_control_points
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        true,                // depth_test
        { 0x00, 0x00, 0x00 } // diffuse
    };

    if ( ! Sculptor::create_material(object_id_info, &sculptor_object_id_pipeline))
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
                viewport.select_query[i_img].destroy();
            }
        }

        mem_mgr.restore_heap_checkpoint(heap_low_checkpoint, heap_high_checkpoint);
        heap_low_checkpoint  = 0;
        heap_high_checkpoint = 0;
        viewports_allocated  = false;
    }
}

void notify_gui_heap_freed()
{
    for (Sculptor::Editor* editor : editors)
        editor->free_resources();

    free_viewport_images();
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

            static ImageInfo select_query_info {
                0, // width
                0, // height
                VK_FORMAT_R32_UINT,
                1, // mip_levels
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                Usage::device_temporary
            };

            select_query_info.width  = viewport.width;
            select_query_info.height = viewport.height;

            static ImageInfo select_query_host_info {
                0, // width
                0, // height
                VK_FORMAT_R32_UINT,
                1, // mip_levels
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                Usage::host_only
            };

            select_query_host_info.width  = select_query_range;
            select_query_host_info.height = select_query_range;

            if ( ! viewport.color_buffer[i_img].allocate(color_info))
                return false;

            if ( ! viewport.depth_buffer[i_img].allocate(depth_info))
                return false;

            if ( ! viewport.select_query[i_img].allocate(select_query_info))
                return false;

            if ( ! viewport.select_query_host[i_img].get_view() && ! viewport.select_query_host[i_img].allocate(select_query_host_info))
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

    for (Sculptor::Editor* editor : editors)
        if (editor->enabled && ! editor->allocate_resources())
            return false;

    heap_high_checkpoint = mem_mgr.get_heap_checkpoint();
    if (heap_high_checkpoint != heap_low_checkpoint)
        viewports_allocated = true;

    return true;
}

static void read_last_selection(Viewport& viewport, uint32_t image_idx)
{
    if (viewport.select_query_pending[image_idx]) {

        viewport.select_query_pending[image_idx] = false;

        const uint32_t* const ptr = viewport.select_query_host[image_idx].get_ptr<uint32_t>();

        // TODO invalidate cache for non-coherent host surfaces

        const uint32_t pitch = viewport.select_query_host[image_idx].get_pitch();

        const uint32_t center_object_id = ptr[(select_mid_point * pitch / sizeof(uint32_t)) + select_mid_point];

        if (center_object_id) {
            viewport.selection.object_id = center_object_id;
            return;
        }

        for (uint32_t y = 0; y < select_query_range; y++) {
            for (uint32_t x = 0; x < select_query_range; x++) {
                const uint32_t object_id = ptr[(y * pitch / sizeof(uint32_t)) + x];
                if (object_id) {
                    viewport.selection.object_id = object_id;
                    return;
                }
            }
        }
    }

    viewport.selection.object_id = 0;
}

static bool create_gui_frame(uint32_t image_idx)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width)  / vk_surface_scale;
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height) / vk_surface_scale;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    const ImVec2 abs_mouse_pos = ImGui::GetMousePos();
    const bool   ctrl_down     = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool   shift_down    = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const float  wheel_delta   = io.MouseWheel;

    if ( ! ctrl_down)
        viewport_mouse = -1;

    static ImVec2 prev_mouse_pos;
    const ImVec2 mouse_delta = ImVec2(abs_mouse_pos.x - prev_mouse_pos.x, abs_mouse_pos.y - prev_mouse_pos.y);
    prev_mouse_pos = abs_mouse_pos;

    const Sculptor::Editor::UserInput input = {
        abs_mouse_pos,
        mouse_delta,
        wheel_delta
    };

    static const Sculptor::Editor::UserInput no_input = {
        vmath::vec2(-(1 << 20), -(1 << 20)),
        vmath::vec2(0, 0),
        0
    };

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

        for (Sculptor::Editor* editor : editors) {
            char name[128];
            snprintf(name, sizeof(name), "%s: %s", editor->get_editor_name(), editor->get_object_name());
            ImGui::Checkbox(name, &editor->enabled);
        }

        ImGui::Separator();

        if (ImGui::RadioButton("Select faces", mouse_mode == MouseMode::select_faces)) mouse_mode = MouseMode::select_faces;
        if (ImGui::RadioButton("Select edges", mouse_mode == MouseMode::select_edges)) mouse_mode = MouseMode::select_edges;
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

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
        ImGui::Begin(viewport.name, &viewport.enabled, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();

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

        const ImVec2 abs_window_pos  = ImGui::GetItemRectMin();
        const ImVec2 rel_mouse_pos   = ImVec2(abs_mouse_pos.x - abs_window_pos.x, abs_mouse_pos.y - abs_window_pos.y);
        const ImVec2 window_size     = ImGui::GetItemRectSize();
        viewport.selection.is_active = ImGui::IsItemHovered() &&
                                       (rel_mouse_pos.x >= select_mid_point) && (rel_mouse_pos.y >= select_mid_point) &&
                                       (rel_mouse_pos.x < window_size.x - select_mid_point) &&
                                       (rel_mouse_pos.y < window_size.y - select_mid_point);
        viewport.selection.x         = static_cast<uint32_t>(rel_mouse_pos.x);
        viewport.selection.y         = static_cast<uint32_t>(rel_mouse_pos.y);

        read_last_selection(viewport, image_idx);

        if (viewport_mouse == -1 && ctrl_down && ImGui::IsItemHovered())
            viewport_mouse = static_cast<int>(viewport.id);

        if (viewport.id == static_cast<uint32_t>(viewport_mouse)) {
            constexpr float rot_scale_factor = 0.3f;

            switch (viewport.view_type) {

                case ViewType::free_moving:
                    viewport.camera_yaw   += rot_scale_factor * mouse_delta.x;
                    viewport.camera_pitch += rot_scale_factor * mouse_delta.y;
                    viewport.camera_pitch  = mstd::min(mstd::max(viewport.camera_pitch, -90.0f), 90.0f);
                    break;

                case ViewType::front: {
                    const float view_bounds        = 1.1f;
                    const float ortho_scale_factor = viewport.view_height * 0.0000001f;
                    viewport.camera_pos.x = mstd::min(mstd::max(viewport.camera_pos.x - ortho_scale_factor * mouse_delta.x, -view_bounds), view_bounds);
                    viewport.camera_pos.y = mstd::min(mstd::max(viewport.camera_pos.y + ortho_scale_factor * mouse_delta.y, -view_bounds), view_bounds);
                    break;
                }

                default:
                    assert(0);
            }
        }

        if (ImGui::IsItemHovered()) {
            if (wheel_delta != 0.0f) {
                const float min_dist   = (viewport.view_type == ViewType::free_moving) ? 0.05f : 128.0f;
                const float max_dist   = (viewport.view_type == ViewType::free_moving) ? 8.0f  : 65536.0f;
                const float dist_scale = (viewport.view_type == ViewType::free_moving) ? 1.0f  : 1024.0f;

                viewport.view_height = mstd::min(mstd::max(viewport.view_height + dist_scale * wheel_delta, min_dist), max_dist);
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                viewport.selection.clicked_id = viewport.selection.object_id;

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if ((viewport.selection.clicked_id == viewport.selection.object_id) && viewport.selection.clicked_id) {
                    switch (mouse_mode) {

                        case MouseMode::select_faces:
                            if (ctrl_down)
                                patch_geometry.deselect_face(viewport.selection.clicked_id - 1);
                            else {
                                if ( ! shift_down)
                                    patch_geometry.deselect_all_faces();
                                patch_geometry.select_face(viewport.selection.clicked_id - 1);
                            }
                            break;

                        case MouseMode::select_edges:
                            // TODO
                            break;
                    }
                }

                viewport.selection.clicked_id = 0;
            }
        }
        else
            viewport.selection.clicked_id = 0;

        ImGui::End();
    }

    if (viewports_changed && ! destroy_viewports())
        return false;

    if ( ! allocate_viewports())
        return false;

    return true;
}

static int16_t clamp16(int32_t value)
{
    return static_cast<int16_t>(mstd::min(mstd::max(value, -32768), 32767));
}

// Find the highest power of two that is less than or equal to value
static int32_t just_msb(int32_t value)
{
    if (value < 1)
        value = 1;

    for (;;) {
        // Clear lowermost set bit
        const int32_t new_value = value & (value - 1);
        if ( ! new_value)
            break;
        value = new_value;
    }

    return value;
}

static bool draw_grid(const Viewport& viewport, uint32_t image_idx, VkCommandBuffer buf, uint32_t transforms_dyn_offs)
{
    const uint32_t sub_buf_stride = max_grid_lines * 2 * sizeof(GridVertex);
    const uint32_t grid_idx       = viewport.id * max_swapchain_size + image_idx;

    auto     vertices  = grid_lines_buf.get_ptr<GridVertex>(grid_idx, sub_buf_stride);
    uint32_t num_lines = 0;

    if (viewport.view_type == ViewType::front) {

        const float half_seen_height = viewport.view_height * 0.5f;
        const float half_seen_width  = half_seen_height * static_cast<float>(viewport.width)
                                     / static_cast<float>(viewport.height);

        int32_t min_x = static_cast<int32_t>(floorf(viewport.camera_pos.x * int16_scale - half_seen_width));
        int32_t max_x = static_cast<int32_t>(ceilf( viewport.camera_pos.x * int16_scale + half_seen_width));
        int32_t min_y = static_cast<int32_t>(floorf(viewport.camera_pos.y * int16_scale - half_seen_height));
        int32_t max_y = static_cast<int32_t>(ceilf( viewport.camera_pos.y * int16_scale + half_seen_height));

        const int32_t     seen_height     = max_y - min_y;
        constexpr int32_t est_horiz_lines = 16;
        const int32_t     vert_dist       = just_msb(seen_height / est_horiz_lines);

        const int16_t minor_step = (vert_dist < 4096) ? static_cast<int16_t>(vert_dist) : 4096;

        min_x -= min_x % minor_step + minor_step;
        max_x += (max_x % minor_step) + minor_step;
        min_y -= min_y % minor_step + minor_step;
        max_y += (max_y % minor_step) + minor_step;

        for (int32_t x = min_x; x <= max_x; x += minor_step) {
            assert(num_lines < max_grid_lines);

            vertices[0].pos[0] = clamp16(x);
            vertices[0].pos[1] = clamp16(min_y);
            vertices[0].pos[2] = 32767;

            vertices[1].pos[0] = clamp16(x);
            vertices[1].pos[1] = clamp16(max_y);
            vertices[1].pos[2] = 32767;

            vertices += 2;
            ++num_lines;
        }

        for (int32_t y = min_y; y <= max_y; y += minor_step) {
            assert(num_lines < max_grid_lines);

            vertices[0].pos[0] = clamp16(min_x);
            vertices[0].pos[1] = clamp16(y);
            vertices[0].pos[2] = 32767;

            vertices[1].pos[0] = clamp16(max_x);
            vertices[1].pos[1] = clamp16(y);
            vertices[1].pos[2] = 32767;

            vertices += 2;
            ++num_lines;
        }
    }
    else {
        assert(viewport.view_type == ViewType::free_moving);

        const int32_t scaled_dist = static_cast<int32_t>(floorf(32768.0f * viewport.camera_distance));
        const int32_t grid_limit  = just_msb(scaled_dist);
        const int32_t min_x = -grid_limit;
        const int32_t max_x = grid_limit;
        const int32_t min_z = -grid_limit;
        const int32_t max_z = grid_limit;

        const int32_t minor_step = mstd::max(grid_limit / 16, 16);

        for (int32_t x = min_x; x <= max_x; x += minor_step) {
            assert(num_lines < max_grid_lines);

            vertices[0].pos[0] = clamp16(x);
            vertices[0].pos[1] = 0;
            vertices[0].pos[2] = clamp16(min_z);

            vertices[1].pos[0] = clamp16(x);
            vertices[1].pos[1] = 0;
            vertices[1].pos[2] = clamp16(max_z);

            vertices += 2;
            ++num_lines;
        }

        for (int32_t z = min_z; z <= max_z; z += minor_step) {
            assert(num_lines < max_grid_lines);

            vertices[0].pos[0] = clamp16(min_x);
            vertices[0].pos[1] = 0;
            vertices[0].pos[2] = clamp16(z);

            vertices[1].pos[0] = clamp16(max_x);
            vertices[1].pos[1] = 0;
            vertices[1].pos[2] = clamp16(z);

            vertices += 2;
            ++num_lines;
        }
    }

    if ( ! grid_lines_buf.flush(grid_idx, sub_buf_stride))
        return false;

    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline);

    send_viewport_and_scissor(buf, viewport.width, viewport.height);

    const uint32_t grid_mat_id = (image_idx * num_materials) + mat_grid;

    uint32_t dynamic_offsets[] = {
        grid_mat_id * materials_stride,
        transforms_dyn_offs
    };

    vkCmdBindDescriptorSets(buf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            Sculptor::material_layout,
                            1, // firstSet
                            2, // descriptorSetCount
                            &desc_set[1],
                            mstd::array_size(dynamic_offsets),
                            dynamic_offsets);

    const VkDeviceSize vb_offset = grid_idx * sub_buf_stride;

    vkCmdBindVertexBuffers(buf,
                           0, // firstBinding
                           1, // bindingCount
                           &grid_lines_buf.get_buffer(),
                           &vb_offset);

    vkCmdDraw(buf,
              num_lines * 2,
              1,  // instanceCount
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
                                               viewport.view_height / int16_scale,
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

        send_viewport_and_scissor(buf, viewport.width, viewport.height);

        vkCmdBindDescriptorSets(buf,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                Sculptor::material_layout,
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

static bool render_selection(const Viewport& viewport, uint32_t image_idx, VkCommandBuffer buf, VkOffset2D* scissor_offset)
{
    const uint32_t transform_id_base = (image_idx * mstd::array_size(viewports) + viewport.id) * transforms_per_viewport;

    const uint32_t transform_id = transform_id_base + 0;

    uint32_t dynamic_offsets[] = {
        transform_id * transforms_stride
    };

    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, sculptor_object_id_pipeline);

    static VkViewport vk_viewport = {
        0,      // x
        0,      // y
        0,      // width
        0,      // height
        0,      // minDepth
        1       // maxDepth
    };

    VkRect2D vk_scissor = {
        { 0, 0 }, // offset
        { 0, 0 }  // extent
    };

    configure_viewport_and_scissor(&vk_viewport, &vk_scissor, viewport.width, viewport.height);

    vk_scissor.offset.x     += viewport.selection.x - select_mid_point;
    vk_scissor.offset.y     += viewport.selection.y - select_mid_point;
    vk_scissor.extent.width  = select_query_range;
    vk_scissor.extent.height = select_query_range;

    *scissor_offset = vk_scissor.offset;

    vkCmdSetViewport(buf, 0, 1, &vk_viewport);

    vkCmdSetScissor(buf, 0, 1, &vk_scissor);

    vkCmdBindDescriptorSets(buf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            Sculptor::material_layout,
                            2,          // firstSet
                            1,          // descriptorSetCount
                            &desc_set[2],
                            mstd::array_size(dynamic_offsets),
                            dynamic_offsets);

    patch_geometry.render(buf);

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

    uint32_t hovered_object_id = ~0U;
    for (const Viewport& viewport : viewports) {
        if (viewport.enabled && viewport.selection.is_active)
            hovered_object_id = viewport.selection.object_id - 1;
    }
    patch_geometry.set_hovered_face(hovered_object_id);

    if ( ! patch_geometry.send_to_gpu(buf))
        return false;

    for (Viewport& viewport : viewports) {
        if ( ! viewport.enabled)
            continue;

        //////////////////////////
        // Render viewport image

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

        color_att.imageView  = viewport.color_buffer[image_idx].get_view();
        color_att.clearValue = make_clear_color(0.2f, 0.2f, 0.2f, 1);
        depth_att.imageView  = viewport.depth_buffer[image_idx].get_view();
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

        /////////////////////////////////////////////
        // Render the same in selection query image

        if (viewport.selection.is_active) {
            viewport.select_query[image_idx].set_image_layout(buf, render_viewport_layout);

            color_att.imageView                  = viewport.select_query[image_idx].get_view();
            color_att.clearValue.color.uint32[0] = 0;

            vkCmdBeginRenderingKHR(buf, &rendering_info);

            VkOffset2D selection_offset;
            if ( ! render_selection(viewport, image_idx, buf, &selection_offset))
                return false;

            vkCmdEndRenderingKHR(buf);

            static const Image::Transition transfer_src_image_layout = {
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            };
            viewport.select_query[image_idx].set_image_layout(buf, transfer_src_image_layout);

            if (viewport.select_query_host[image_idx].layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                static const Image::Transition transfer_dst_image_layout = {
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                };
                viewport.select_query_host[image_idx].set_image_layout(buf, transfer_dst_image_layout);
            }

            viewport.select_query_pending[image_idx] = true;

            static VkImageCopy region = {
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                { },                                            // srcOffset
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                { },                                            // dstOffset
                { select_query_range, select_query_range, 1 }   // extent
            };

            region.srcOffset.x = selection_offset.x;
            region.srcOffset.y = selection_offset.y;

            vkCmdCopyImage(buf,
                           viewport.select_query[image_idx].get_image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           viewport.select_query_host[image_idx].get_image(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
        }
    }

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
