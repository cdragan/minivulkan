// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "sculptor_materials.h"

#include "../core/minivulkan.h"
#include "../core/mstdc.h"

#include "sculptor_shaders.h"
#include "../core/shaders.h"

VkDescriptorSetLayout Sculptor::desc_set_layout;
VkPipelineLayout      Sculptor::material_layout;

bool Sculptor::create_material_layouts()
{
    {
        static const VkDescriptorSetLayoutBinding per_object_set[] = {
            {
                0, // binding 0: uniform buffer with materials
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                1,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr
            },
            {
                1, // binding 1: uniform buffer with transforms
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                1,
                VK_SHADER_STAGE_VERTEX_BIT
                    | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                nullptr
            },
            {
                2, // binding 2: storage buffer with patch face data
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_VERTEX_BIT
                    | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
                    | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                    | VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr
            },
            {
                3, // binding 3: storage buffer with index buffer for edges
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_VERTEX_BIT,
                nullptr
            },
            {
                4, // binding 4: storrage buffer with vertex buffer for edges
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_VERTEX_BIT,
                nullptr
            },
        };

        static const VkDescriptorSetLayoutCreateInfo create_per_object_set_layout = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT, // flags
            mstd::array_size(per_object_set),
            per_object_set
        };

        const VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev,
                                                             &create_per_object_set_layout,
                                                             nullptr,
                                                             &desc_set_layout));
        if (res != VK_SUCCESS)
            return false;
    }

    {
        static VkPipelineLayoutCreateInfo layout_create_info = {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr,
            0,      // flags
            1,
            &desc_set_layout,
            0,      // pushConstantRangeCount
            nullptr // pPushConstantRanges
        };

        const VkResult res = CHK(vkCreatePipelineLayout(vk_dev,
                                                        &layout_create_info,
                                                        nullptr,
                                                        &material_layout));
        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

bool Sculptor::create_material(const MaterialInfo& mat_info, VkPipeline* pipeline)
{
    assert(*pipeline == VK_NULL_HANDLE);

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

    uint32_t num_stages;
    for (num_stages = 0; num_stages < mstd::array_size(mat_info.shader_ids); num_stages++) {
        uint8_t* const shader = mat_info.shader_ids[num_stages];
        if ( ! shader)
            break;
        shader_stages[num_stages].module = load_shader(shader);
    }

    static VkVertexInputBindingDescription vertex_bindings[] = {
        {
            0, // binding
            0, // stride
            VK_VERTEX_INPUT_RATE_VERTEX
        }
    };
    vertex_bindings[0].stride = mat_info.vertex_stride;

    static VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,      // flags
        0,
        nullptr,
        0,      // vertexAttributeDescriptionCount
        nullptr // pVertexAttributeDescriptions
    };
    vertex_input_state.vertexBindingDescriptionCount   =
        mat_info.vertex_stride ? mstd::array_size(vertex_bindings) : 0U;
    vertex_input_state.pVertexBindingDescriptions      =
        mat_info.vertex_stride ? vertex_bindings : nullptr;
    vertex_input_state.vertexAttributeDescriptionCount = mat_info.num_vertex_attributes;
    vertex_input_state.pVertexAttributeDescriptions    = mat_info.vertex_attributes;

    static VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE
    };
    input_assembly_state.topology = static_cast<VkPrimitiveTopology>(mat_info.primitive_topology);

    static VkPipelineTessellationStateCreateInfo tessellation_state = {
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        0   // patchControlPoints
    };
    tessellation_state.patchControlPoints = mat_info.patch_control_points;

    static VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        1,
        nullptr,
        1,
        nullptr
    };

    static VkPipelineRasterizationStateCreateInfo rasterization_state = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_FALSE,   // depthClampEnable
        VK_FALSE,   // rasterizerDiscardEnable
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE,
        VK_FRONT_FACE_COUNTER_CLOCKWISE,
        VK_FALSE,   // depthBiasEnable
        0,          // depthBiasConstantFactor
        0,          // depthBiasClamp
        0,          // depthBiasSlopeFactor
        1           // lineWidth
    };

    rasterization_state.polygonMode             = static_cast<VkPolygonMode>(mat_info.polygon_mode);
    rasterization_state.cullMode                = static_cast<VkCullModeFlags>(mat_info.cull_mode);
    rasterization_state.depthBiasEnable         = mat_info.depth_bias != 0.0f;
    rasterization_state.depthBiasConstantFactor = mat_info.depth_bias;

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
        VK_FALSE,   // depthTestEnable
        VK_FALSE,   // depthWriteEnable
        VK_COMPARE_OP_GREATER_OR_EQUAL,
        VK_FALSE,   // depthBoundsTestEnable
        VK_FALSE,   // stencilTestEnable
        { },        // front
        { },        // back
        0,          // minDepthBounds
        0           // maxDepthBounds
    };
    depth_stencil_state.depthTestEnable  = mat_info.depth_test;
    depth_stencil_state.depthWriteEnable = mat_info.depth_write;

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

    static VkFormat color_format = VK_FORMAT_UNDEFINED;

    static VkPipelineRenderingCreateInfo rendering_info = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        nullptr,
        0,                      // viewMask
        1,                      // colorAttachmentCount
        &color_format,
        VK_FORMAT_UNDEFINED,    // depthAttachmentFormat
        VK_FORMAT_UNDEFINED     // stencilAttachmentFormat
    };

    color_format = (mat_info.color_format == VK_FORMAT_UNDEFINED)
        ? swapchain_create_info.imageFormat
        : static_cast<VkFormat>(mat_info.color_format);

    rendering_info.depthAttachmentFormat = vk_depth_format;

    static VkGraphicsPipelineCreateInfo pipeline_create_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        &rendering_info,
        0,              // flags
        0,
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

    pipeline_create_info.stageCount = num_stages;
    pipeline_create_info.layout     = material_layout;

    const VkResult res = CHK(vkCreateGraphicsPipelines(vk_dev,
                                                       VK_NULL_HANDLE,
                                                       1,
                                                       &pipeline_create_info,
                                                       nullptr,
                                                       pipeline));
    return res == VK_SUCCESS;
}
