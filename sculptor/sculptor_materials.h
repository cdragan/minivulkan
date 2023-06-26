// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "../vulkan_functions.h"

#include <stdint.h>

bool create_material_layouts();

extern VkDescriptorSetLayout sculptor_desc_set_layout[3];
extern VkPipelineLayout      sculptor_material_layout;

struct MaterialInfo {
    uint8_t*                                 shader_ids[4];
    uint8_t                                  vertex_stride;
    uint8_t                                  patch_control_points;
    uint8_t                                  polygon_mode;
    uint8_t                                  cull_mode;
    uint8_t                                  num_vertex_attributes;
    const VkVertexInputAttributeDescription* vertex_attributes;
};

bool create_material(const MaterialInfo& mat_info, VkPipeline* pipeline);
