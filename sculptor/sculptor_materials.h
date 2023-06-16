// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "../vulkan_functions.h"

#include <stdint.h>

bool create_material_layouts();

struct ShaderInfo {
    uint8_t*                                 shader_ids[4];
    uint8_t                                  vertex_stride;
    uint8_t                                  patch_control_points;
    uint8_t                                  polygon_mode;
    uint8_t                                  num_vertex_attributes;
    const VkVertexInputAttributeDescription* vertex_attributes;
};

bool create_material(const ShaderInfo& shader_info, VkPipeline* pipeline);
