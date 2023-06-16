// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_materials.h"

#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"

static VkDescriptorSetLayout desc_set_layout[4];
static VkPipelineLayout      material_layout;

bool create_material_layouts()
{
    {
        static const VkDescriptorSetLayoutCreateInfo create_empty_set_layout = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            0, // flags
            0,
            nullptr
        };

        const VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_empty_set_layout, nullptr, &desc_set_layout[0]));
        if (res != VK_SUCCESS)
            return false;
    }

    desc_set_layout[1] = desc_set_layout[0];
    desc_set_layout[2] = desc_set_layout[0];

    {
        static const VkDescriptorSetLayoutBinding per_object_set[] = {
            {
                0, // binding 0: uniform buffer with transforms
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                1,
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                nullptr
            },
            {
                1, // binding 1: storage buffer with patch face data
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
                    | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                nullptr
            }
        };

        static const VkDescriptorSetLayoutCreateInfo create_per_object_set_layout = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            0, // flags
            mstd::array_size(per_object_set),
            per_object_set
        };

        const VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_per_object_set_layout, nullptr, &desc_set_layout[3]));
        if (res != VK_SUCCESS)
            return false;
    }

    {
        static VkPipelineLayoutCreateInfo layout_create_info = {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr,
            0,      // flags
            mstd::array_size(desc_set_layout),
            desc_set_layout,
            0,      // pushConstantRangeCount
            nullptr // pPushConstantRanges
        };

        const VkResult res = CHK(vkCreatePipelineLayout(vk_dev, &layout_create_info, nullptr, &material_layout));
        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

bool create_material(const ShaderInfo& shader_info, VkPipeline* pipeline)
{
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
    for (num_stages = 0; num_stages < mstd::array_size(shader_info.shader_ids); num_stages++) {
        uint8_t* const shader = shader_info.shader_ids[num_stages];
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

    static VkPipelineRenderingCreateInfo rendering_info = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        nullptr,
        0,                      // viewMask
        1,                      // colorAttachmentCount
        &swapchain_create_info.imageFormat,
        VK_FORMAT_UNDEFINED,    // depthAttachmentFormat
        VK_FORMAT_UNDEFINED     // stencilAttachmentFormat
    };

    rendering_info.depthAttachmentFormat = vk_depth_format;

    static VkGraphicsPipelineCreateInfo pipeline_create_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        &rendering_info,
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

    pipeline_create_info.layout = material_layout;

    const VkResult res = CHK(vkCreateGraphicsPipelines(vk_dev,
                                                       VK_NULL_HANDLE,
                                                       1,
                                                       &pipeline_create_info,
                                                       nullptr,
                                                       pipeline));
    return res == VK_SUCCESS;
}
