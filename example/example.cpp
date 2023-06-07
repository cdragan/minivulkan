// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "example.h"

#include "../gui.h"
#include "../host_filler.h"
#include "../memory_heap.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

const char app_name[] = "minivulkan example";

constexpr float image_ratio = 0.0f;

float    user_roundedness = 111.0f / 127.0f;
uint32_t user_tess_level  = 12;
bool     user_wireframe   = false;

static HostFiller filler;

enum WhatGeometry {
    geom_cube,
    geom_cubic_patch,
    geom_quadratic_patch
};
#ifdef __aarch64__
static constexpr WhatGeometry what_geometry = geom_cube;
#else
static constexpr WhatGeometry what_geometry = geom_quadratic_patch;
#endif

uint32_t check_device_features()
{
    uint32_t missing_features = 0;

    if (what_geometry != geom_cube)
        missing_features += check_feature(&vk_features.features.tessellationShader);

    missing_features += check_feature(&vk_features.features.fillModeNonSolid);

    return missing_features;
}

static VkPipelineLayout vk_gr_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline       vk_gr_pipeline[2];

static VkDescriptorSetLayout vk_desc_set_layout = VK_NULL_HANDLE;

static bool create_pipeline_layouts()
{
    static const VkDescriptorSetLayoutBinding create_binding = {
        0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        1,
        VK_SHADER_STAGE_VERTEX_BIT
            | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
            | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
            | VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
    };

    static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0, // flags
        1,
        &create_binding
    };

    VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_desc_set_layout, nullptr, &vk_desc_set_layout));
    if (res != VK_SUCCESS)
        return false;

    static VkPipelineLayoutCreateInfo layout_create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,      // flags
        1,
        &vk_desc_set_layout,
        0,      // pushConstantRangeCount
        nullptr // pPushConstantRanges
    };

    res = CHK(vkCreatePipelineLayout(vk_dev, &layout_create_info, nullptr, &vk_gr_pipeline_layout));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

struct Vertex {
    int8_t pos[4];
    int8_t normal[4];
};

struct ShaderInfo {
    uint8_t*                                 shader_ids[4];
    uint8_t                                  vertex_stride;
    uint8_t                                  patch_control_points;
    uint8_t                                  polygon_mode;
    uint8_t                                  num_vertex_attributes;
    const VkVertexInputAttributeDescription* vertex_attributes;
};

static bool create_graphics_pipeline(const ShaderInfo& shader_info, VkPipeline* pipeline)
{
    if (*pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_dev, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
    }

    static VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_VERTEX_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        }
    };

    uint32_t num_stages = 0;
    for (uint32_t i = 0; i < mstd::array_size(shader_info.shader_ids); i++) {
        uint8_t* const shader = shader_info.shader_ids[i];
        if ( ! shader)
            break;
        shader_stages[i].module = load_shader(shader);
        ++num_stages;
    }

    static VkVertexInputBindingDescription vertex_bindings[] = {
        {
            0, // binding
            0, // stride
            VK_VERTEX_INPUT_RATE_VERTEX
        }
    };
    vertex_bindings[0].stride = shader_info.vertex_stride;

    static VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,      // flags
        mstd::array_size(vertex_bindings),
        vertex_bindings,
        0,      // vertexAttributeDescriptionCount
        nullptr // pVertexAttributeDescriptions
    };
    vertex_input_state.vertexAttributeDescriptionCount = shader_info.num_vertex_attributes;
    vertex_input_state.pVertexAttributeDescriptions    = shader_info.vertex_attributes;

    static VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE
    };
    if (shader_info.patch_control_points)
        input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

    static VkPipelineTessellationStateCreateInfo tessellation_state = {
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        0   // patchControlPoints
    };
    tessellation_state.patchControlPoints = shader_info.patch_control_points;

    static VkViewport viewport = {
        0,      // x
        0,      // y
        0,      // width
        0,      // height
        0,      // minDepth
        1       // maxDepth
    };

    static VkRect2D scissor = {
        { 0, 0 },   // offset
        { 0, 0 }    // extent
    };

    static VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        1,
        &viewport,
        1,
        &scissor
    };

    static VkPipelineRasterizationStateCreateInfo rasterization_state = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_FALSE,   // depthClampEnable
        VK_FALSE,   // rasterizerDiscardEnable
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        VK_FRONT_FACE_COUNTER_CLOCKWISE,
        VK_FALSE,   // depthBiasEnable
        0,          // depthBiasConstantFactor
        0,          // depthBiasClamp
        0,          // depthBiasSlopeFactor
        1           // lineWidth
    };

    rasterization_state.polygonMode = static_cast<VkPolygonMode>(shader_info.polygon_mode);

    static VkPipelineMultisampleStateCreateInfo multisample_state = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,   // sampleShadingEnable
        0,          // minSampleShading
        nullptr,    // pSampleMask
        VK_FALSE,   // alphaToCoverageEnable
        VK_FALSE    // alphaToOneEnable
    };

    static VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_TRUE,    // depthTestEnable
        VK_TRUE,    // depthWriteEnable
        VK_COMPARE_OP_GREATER_OR_EQUAL,
        VK_FALSE,   // depthBoundsTestEnable
        VK_FALSE,   // stencilTestEnable
        { },        // front
        { },        // back
        0,          // minDepthBounds
        0           // maxDepthBounds
    };

    static VkPipelineColorBlendAttachmentState color_blend_att = {
        VK_FALSE,               // blendEnable
        VK_BLEND_FACTOR_ZERO,   // srcColorBlendFactor
        VK_BLEND_FACTOR_ZERO,   // dstColorBlendFactor
        VK_BLEND_OP_ADD,        // colorBlendOp
        VK_BLEND_FACTOR_ZERO,   // srcAlphaBlendFactor
        VK_BLEND_FACTOR_ZERO,   // dstAlphaBlendFactor
        VK_BLEND_OP_ADD,        // alphaBlendOp
                                // colorWriteMask
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
    };

    static VkPipelineColorBlendStateCreateInfo color_blend_state = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_FALSE,   // logicOpEnable
        VK_LOGIC_OP_CLEAR,
        1,          // attachmentCount
        &color_blend_att,
        { }         // blendConstants
    };

    static VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    static VkPipelineDynamicStateCreateInfo dynamic_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        mstd::array_size(dynamic_states),
        dynamic_states
    };

    static VkGraphicsPipelineCreateInfo pipeline_create_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        nullptr,
        0,              // flags
        num_stages,
        shader_stages,
        &vertex_input_state,
        &input_assembly_state,
        &tessellation_state,
        &viewport_state,
        &rasterization_state,
        &multisample_state,
        &depth_stencil_state,
        &color_blend_state,
        &dynamic_state,
        VK_NULL_HANDLE, // layout
        VK_NULL_HANDLE, // renderPass
        0,              // subpass
        VK_NULL_HANDLE, // basePipelineHandle
        -1              // basePipelineIndex
    };

    pipeline_create_info.layout     = vk_gr_pipeline_layout;
    pipeline_create_info.renderPass = vk_render_pass;

    const VkResult res = CHK(vkCreateGraphicsPipelines(vk_dev,
                                                       VK_NULL_HANDLE,
                                                       1,
                                                       &pipeline_create_info,
                                                       nullptr,
                                                       pipeline));
    return res == VK_SUCCESS;
}

static bool create_simple_graphics_pipeline()
{
    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0,  // location
            0,  // binding
            VK_FORMAT_R8G8B8A8_SNORM,
            offsetof(Vertex, pos)
        },
        {
            1,  // location
            0,  // binding
            VK_FORMAT_R8G8B8A8_SNORM,
            offsetof(Vertex, normal)
        }
    };

    static ShaderInfo shader_info = {
        {
            shader_simple_vert,
            shader_phong_frag
        },
        sizeof(Vertex),
        0, // patch_control_points
        VK_POLYGON_MODE_FILL,
        mstd::array_size(vertex_attributes),
        vertex_attributes
    };

    shader_info.polygon_mode = VK_POLYGON_MODE_FILL;
    if ( ! create_graphics_pipeline(shader_info, &vk_gr_pipeline[0]))
        return false;

    shader_info.polygon_mode = VK_POLYGON_MODE_LINE;
    return create_graphics_pipeline(shader_info, &vk_gr_pipeline[1]);
}

static bool create_patch_graphics_pipeline()
{
    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0,  // location
            0,  // binding
            VK_FORMAT_R8G8B8A8_SNORM,
            offsetof(Vertex, pos)
        }
    };

    static ShaderInfo shader_info = {
        {
            shader_pass_through_vert,
            shader_phong_frag,
            shader_bezier_surface_cubic_tesc,
            shader_bezier_surface_cubic_tese
        },
        sizeof(Vertex),
        16, // patch_control_points
        VK_POLYGON_MODE_FILL,
        mstd::array_size(vertex_attributes),
        vertex_attributes
    };
    if (what_geometry == geom_quadratic_patch) {
        shader_info.patch_control_points = 9;
        shader_info.shader_ids[0]        = shader_rounded_cube_vert;
        shader_info.shader_ids[2]        = shader_bezier_surface_quadratic_tesc;
        shader_info.shader_ids[3]        = shader_bezier_surface_quadratic_tese;
    }

    shader_info.polygon_mode = VK_POLYGON_MODE_FILL;
    if ( ! create_graphics_pipeline(shader_info, &vk_gr_pipeline[0]))
        return false;

    shader_info.polygon_mode = VK_POLYGON_MODE_LINE;
    return create_graphics_pipeline(shader_info, &vk_gr_pipeline[1]);
}

static bool create_pipelines()
{
    // TODO unify common parts
    if (what_geometry == geom_cube)
        return create_simple_graphics_pipeline();
    else
        return create_patch_graphics_pipeline();
}

static CommandBuffers<1> cmd_buf;

static bool create_cube(Buffer* vertex_buffer, Buffer* index_buffer)
{
    if ( ! cmd_buf.pool && ! allocate_command_buffers(&cmd_buf))
        return false;

    if ( ! reset_and_begin_command_buffer(cmd_buf.bufs[0]))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127, -127 }, {    0,    0, -127 } },
        { {  127,  127, -127 }, {    0,    0, -127 } },
        { { -127, -127, -127 }, {    0,    0, -127 } },
        { {  127, -127, -127 }, {    0,    0, -127 } },
        { { -127, -127,  127 }, {    0,    0,  127 } },
        { {  127, -127,  127 }, {    0,    0,  127 } },
        { { -127,  127,  127 }, {    0,    0,  127 } },
        { {  127,  127,  127 }, {    0,    0,  127 } },
        { { -127,  127,  127 }, {    0,  127,    0 } },
        { {  127,  127,  127 }, {    0,  127,    0 } },
        { { -127,  127, -127 }, {    0,  127,    0 } },
        { {  127,  127, -127 }, {    0,  127,    0 } },
        { { -127, -127, -127 }, {    0, -127,    0 } },
        { {  127, -127, -127 }, {    0, -127,    0 } },
        { { -127, -127,  127 }, {    0, -127,    0 } },
        { {  127, -127,  127 }, {    0, -127,    0 } },
        { {  127,  127, -127 }, {  127,    0,    0 } },
        { {  127,  127,  127 }, {  127,    0,    0 } },
        { {  127, -127, -127 }, {  127,    0,    0 } },
        { {  127, -127,  127 }, {  127,    0,    0 } },
        { { -127,  127,  127 }, { -127,    0,    0 } },
        { { -127,  127, -127 }, { -127,    0,    0 } },
        { { -127, -127,  127 }, { -127,    0,    0 } },
        { { -127, -127, -127 }, { -127,    0,    0 } },
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              vertex_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices,
                              sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        2, 3, 0,
        0, 3, 1,
        6, 7, 4,
        4, 7, 5,
        10, 11, 8,
        8, 11, 9,
        14, 15, 12,
        12, 15, 13,
        18, 19, 16,
        16, 19, 17,
        22, 23, 20,
        20, 23, 21,
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              index_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices,
                              sizeof(indices)))
        return false;

    return send_to_device_and_wait(cmd_buf.bufs[0]);
}

static bool create_cubic_patch(Buffer* vertex_buffer, Buffer* index_buffer)
{
    if ( ! cmd_buf.pool && ! allocate_command_buffers(&cmd_buf))
        return false;

    if ( ! reset_and_begin_command_buffer(cmd_buf.bufs[0]))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127,  127 }, { } },
        { {  127,  127,  127 }, { } },
        { {  127, -127,  127 }, { } },
        { { -127, -127,  127 }, { } },
        { { -127,  127, -127 }, { } },
        { {  127,  127, -127 }, { } },
        { {  127, -127, -127 }, { } },
        { { -127, -127, -127 }, { } },
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              vertex_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices,
                              sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        0, 0, 3, 3,
        0, 1, 2, 3,
        4, 5, 6, 7,
        4, 4, 7, 7,
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              index_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices,
                              sizeof(indices)))
        return false;

    return send_to_device_and_wait(cmd_buf.bufs[0]);
}

static bool create_quadratic_patch(Buffer* vertex_buffer, Buffer* index_buffer)
{
    if ( ! cmd_buf.pool && ! allocate_command_buffers(&cmd_buf))
        return false;

    if ( ! reset_and_begin_command_buffer(cmd_buf.bufs[0]))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127, -127 }, {} },
        { { -111,  127, -127 }, {} },
        { {  111,  127, -127 }, {} },
        { {  127,  127, -127 }, {} },
        { { -127,  111, -127 }, {} },
        { { -111,  111, -127 }, {} },
        { {  111,  111, -127 }, {} },
        { {  127,  111, -127 }, {} },
        { { -127, -111, -127 }, {} },
        { { -111, -111, -127 }, {} },
        { {  111, -111, -127 }, {} },
        { {  127, -111, -127 }, {} },
        { { -127, -127, -127 }, {} },
        { { -111, -127, -127 }, {} },
        { {  111, -127, -127 }, {} },
        { {  127, -127, -127 }, {} },

        { { -127,  127, -111 }, {} },
        { { -111,  127, -111 }, {} },
        { {  111,  127, -111 }, {} },
        { {  127,  127, -111 }, {} },
        { { -127,  111, -111 }, {} },
        { {                  }, {} }, // unneeded
        { {                  }, {} }, // unneeded
        { {  127,  111, -111 }, {} },
        { { -127, -111, -111 }, {} },
        { {                  }, {} }, // unneeded
        { {                  }, {} }, // unneeded
        { {  127, -111, -111 }, {} },
        { { -127, -127, -111 }, {} },
        { { -111, -127, -111 }, {} },
        { {  111, -127, -111 }, {} },
        { {  127, -127, -111 }, {} },

        { { -127,  127,  111 }, {} },
        { { -111,  127,  111 }, {} },
        { {  111,  127,  111 }, {} },
        { {  127,  127,  111 }, {} },
        { { -127,  111,  111 }, {} },
        { {                  }, {} }, // unneeded
        { {                  }, {} }, // unneeded
        { {  127,  111,  111 }, {} },
        { { -127, -111,  111 }, {} },
        { {                  }, {} }, // unneeded
        { {                  }, {} }, // unneeded
        { {  127, -111,  111 }, {} },
        { { -127, -127,  111 }, {} },
        { { -111, -127,  111 }, {} },
        { {  111, -127,  111 }, {} },
        { {  127, -127,  111 }, {} },

        { { -127,  127,  127 }, {} },
        { { -111,  127,  127 }, {} },
        { {  111,  127,  127 }, {} },
        { {  127,  127,  127 }, {} },
        { { -127,  111,  127 }, {} },
        { { -111,  111,  127 }, {} },
        { {  111,  111,  127 }, {} },
        { {  127,  111,  127 }, {} },
        { { -127, -111,  127 }, {} },
        { { -111, -111,  127 }, {} },
        { {  111, -111,  127 }, {} },
        { {  127, -111,  127 }, {} },
        { { -127, -127,  127 }, {} },
        { { -111, -127,  127 }, {} },
        { {  111, -127,  127 }, {} },
        { {  127, -127,  127 }, {} },
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              vertex_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices,
                              sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        17, 17, 17, // front left-top
        16,  0,  1,
        20,  4,  5,
        17, 17, 18, // front middle-top
         1,  1,  2,
         5,  5,  6,
        18, 18, 18, // front right-top
         2,  3, 19,
         6,  7, 23,
        20,  4,  5, // front left-middle
        20,  4,  5,
        24,  8,  9,
         5,  5,  6, // front middle
         5,  5,  6,
         9,  9, 10,
         6,  7, 23, // front right-mddile
         6,  7, 23,
        10, 11, 27,
        24,  8,  9, // front left-bottom
        28, 12, 13,
        29, 29, 29,
         9,  9, 10, // front middle-bottom
        13, 13, 14,
        29, 29, 30,
        10, 11, 27, // front right-bottom
        14, 15, 31,
        30, 30, 30,
        36, 36, 20, // left side middle
        36, 36, 20,
        40, 40, 24,
        33, 33, 17, // left side top
        32, 32, 16,
        36, 36, 20,
        40, 40, 24, // left side bottom
        44, 44, 28,
        45, 45, 29,
        33, 33, 34, // top middle
        33, 33, 34,
        17, 17, 18,
        18, 18, 34, // right side top
        19, 19, 35,
        23, 23, 39,
        23, 23, 39, // right side middle
        23, 23, 39,
        27, 27, 43,
        27, 27, 43, // right side bottom
        31, 31, 47,
        30, 30, 46,
        29, 29, 30, // bottom middle
        45, 45, 46,
        45, 45, 46,
        33, 33, 33, // back left-top
        49, 48, 32,
        53, 52, 36,
        34, 34, 33, // back middle-top
        50, 50, 49,
        54, 54, 53,
        34, 34, 34, // back right-top
        35, 51, 50,
        39, 55, 54,
        39, 55, 54, // back right-middle
        39, 55, 54,
        43, 59, 58,
        43, 59, 58, // back right-bottom
        47, 63, 62,
        46, 46, 46,
        45, 45, 46, // back middle-bottom
        61, 61, 62,
        57, 57, 58,
        40, 40, 40, // back left-bottom
        56, 60, 44,
        57, 61, 45,
        57, 57, 53, // back left-middle
        56, 56, 52,
        40, 40, 36,
        54, 54, 53, // back middle
        54, 54, 53,
        58, 58, 57,
    };

    if ( ! filler.fill_buffer(cmd_buf.bufs[0],
                              index_buffer,
                              Usage::fixed,
                              VK_FORMAT_UNDEFINED,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices,
                              sizeof(indices)))
        return false;

    return send_to_device_and_wait(cmd_buf.bufs[0]);
}

bool init_assets()
{
    if ( ! create_pipeline_layouts())
        return false;

    if ( ! create_pipelines())
        return false;

    return true;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    static Buffer vertex_buffer;
    static Buffer index_buffer;
    VkResult      res;

    if ( ! create_gui_frame())
        return false;

    if ( ! vertex_buffer.allocated()) {
        if (what_geometry == geom_cube) {
            if ( ! create_cube(&vertex_buffer, &index_buffer))
                return false;
        }
        else if (what_geometry == geom_cubic_patch) {
            if ( ! create_cubic_patch(&vertex_buffer, &index_buffer))
                return false;
        }
        else if (what_geometry == geom_quadratic_patch) {
            if ( ! create_quadratic_patch(&vertex_buffer, &index_buffer))
                return false;
        }
    }

    // Allocate descriptor set
    static VkDescriptorSet desc_set[max_swapchain_size] = { VK_NULL_HANDLE };
    if ( ! desc_set[0]) {
        static VkDescriptorPoolSize pool_sizes[] = {
            {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,      // type
                mstd::array_size(desc_set)              // descriptorCount
            }
        };

        static VkDescriptorPoolCreateInfo pool_create_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            nullptr,
            0,                                      // flags
            mstd::array_size(desc_set),             // maxSets
            mstd::array_size(pool_sizes),
            pool_sizes
        };

        VkDescriptorPool desc_set_pool;

        res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &desc_set_pool));
        if (res != VK_SUCCESS)
            return false;

        static VkDescriptorSetLayout       layouts[max_swapchain_size];
        static VkDescriptorSetAllocateInfo alloc_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            nullptr,
            VK_NULL_HANDLE,             // descriptorPool
            mstd::array_size(layouts),  // descriptorSetCount
            layouts                     // pSetLayouts
        };

        alloc_info.descriptorPool = desc_set_pool;

        for (uint32_t i = 0; i < mstd::array_size(layouts); i++)
            layouts[i] = vk_desc_set_layout;

        res = CHK(vkAllocateDescriptorSets(vk_dev, &alloc_info, desc_set));
        if (res != VK_SUCCESS)
            return false;
    }

    // Create shader data
    static Buffer shader_data;
    struct UniformBuffer {
        vmath::mat4 model_view_proj;  // transforms to camera space for rasterization
        vmath::mat4 model;            // transforms to world space for lighting
        vmath::mat3 model_normal;     // inverse transpose for transforming normals to world space
        vmath::vec4 color;            // object color
        vmath::vec4 params;           // shader-specific parameters
        vmath::vec4 lights[1];        // light positions in world space
    };
    static uint32_t slot_size;
    static uint8_t* host_shader_data;
    if ( ! shader_data.allocated()) {
        slot_size = mstd::align_up(static_cast<uint32_t>(sizeof(UniformBuffer)),
                                   static_cast<uint32_t>(vk_phys_props.properties.limits.minUniformBufferOffsetAlignment));
        slot_size = mstd::align_up(slot_size,
                                   static_cast<uint32_t>(vk_phys_props.properties.limits.nonCoherentAtomSize));
        const uint32_t total_size = slot_size * mstd::array_size(desc_set);
        if ( ! shader_data.allocate(Usage::dynamic, total_size, VK_FORMAT_UNDEFINED, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return false;

        host_shader_data = shader_data.get_ptr<uint8_t>();
        if ( ! host_shader_data)
            return false;
    }

    // Calculate matrices
    const auto uniform_data = reinterpret_cast<UniformBuffer*>(&host_shader_data[slot_size * image_idx]);
    const float angle = vmath::radians(static_cast<float>(time_ms) * 15.0f / 1000.0f);
    const vmath::mat4 model_view = vmath::mat4(vmath::quat(vmath::vec3(0.70710678f, 0.70710678f, 0), angle))
                                 * vmath::translate(0.0f, 0.0f, 7.0f);
    const float aspect = (image_ratio != 0) ? image_ratio
                         : (static_cast<float>(vk_surface_caps.currentExtent.width)
                           / static_cast<float>(vk_surface_caps.currentExtent.height));
    const vmath::mat4 proj = vmath::projection(
            aspect,
            vmath::radians(30.0f),  // fov
            0.01f,                  // near_plane
            100.0f,                 // far_plane
            0.0f);                  // depth_bias
    uniform_data->model_view_proj = model_view * proj;
    uniform_data->model           = model_view;
    uniform_data->model_normal    = vmath::transpose(vmath::inverse(vmath::mat3(model_view)));
    uniform_data->color           = vmath::vec4(0.4f, 0.6f, 0.1f, 1);
    uniform_data->params          = vmath::vec4(user_roundedness,
                                                static_cast<float>(user_tess_level),
                                                0,
                                                0);
    uniform_data->lights[0]       = vmath::vec4(5.0f, 5.0f, -5.0f, 1.0f);

    // Send matrices to GPU
    if ( ! shader_data.flush())
        return false;

    // Update descriptor set
    static VkDescriptorBufferInfo buffer_info = {
        VK_NULL_HANDLE,     // buffer
        0,                  // offset
        0                   // range
    };
    buffer_info.buffer = shader_data.get_buffer();
    buffer_info.offset = slot_size * image_idx;
    buffer_info.range  = slot_size;
    static VkWriteDescriptorSet write_desc_sets[] = {
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            VK_NULL_HANDLE,                     // dstSet
            0,                                  // dstBinding
            0,                                  // dstArrayElement
            1,                                  // descriptorCount
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  // descriptorType
            nullptr,                            // pImageInfo
            &buffer_info,                       // pBufferInfo
            nullptr                             // pTexelBufferView
        }
    };
    write_desc_sets[0].dstSet = desc_set[image_idx];

    vkUpdateDescriptorSets(vk_dev,
                           mstd::array_size(write_desc_sets),
                           write_desc_sets,
                           0,           // descriptorCopyCount
                           nullptr);    // pDescriptorCopies

    // Render image
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

    if (vk_depth_buffers[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        static const Image::Transition depth_init = {
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };

        vk_depth_buffers[image_idx].set_image_layout(buf, depth_init);
    }

    static const VkClearValue clear_values[2] = {
        make_clear_color(0, 0, 0, 0),
        make_clear_depth(0, 0)
    };

    static VkRenderPassBeginInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr,
        VK_NULL_HANDLE,     // renderPass
        VK_NULL_HANDLE,     // framebuffer
        { },
        mstd::array_size(clear_values),
        clear_values
    };

    render_pass_info.renderPass        = vk_render_pass;
    render_pass_info.framebuffer       = vk_frame_buffers[image_idx];
    render_pass_info.renderArea.extent = vk_surface_caps.currentExtent;

    vkCmdBeginRenderPass(buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_gr_pipeline[user_wireframe ? 1 : 0]);

    send_viewport_and_scissor(buf,
                              image_ratio,
                              vk_surface_caps.currentExtent.width,
                              vk_surface_caps.currentExtent.height);

    static const VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(buf,
                           0,   // firstBinding
                           1,   // bindingCount
                           &vertex_buffer.get_buffer(),
                           &vb_offset);

    vkCmdBindIndexBuffer(buf,
                         index_buffer.get_buffer(),
                         0,     // offset
                         VK_INDEX_TYPE_UINT16);

    vkCmdBindDescriptorSets(buf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_gr_pipeline_layout,
                            0,          // firstSet
                            1,          // descriptorSetCount
                            &desc_set[image_idx],
                            0,          // dynamicOffsetCount
                            nullptr);   // pDynamicOffsets

    constexpr uint32_t index_count =
        (what_geometry == geom_cube)            ? 36 :
        (what_geometry == geom_cubic_patch)     ? 16 :
        (what_geometry == geom_quadratic_patch) ? 78 * 3 :
        0;

    vkCmdDrawIndexed(buf,
                     index_count,
                     1,     // instanceCount
                     0,     // firstVertex
                     0,     // vertexOffset
                     0);    // firstInstance

    if ( ! send_gui_to_gpu(buf))
        return false;

    vkCmdEndRenderPass(buf);

    static const Image::Transition color_att_present = {
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    image.set_image_layout(buf, color_att_present);

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
