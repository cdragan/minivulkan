// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "../vulkan_functions.h"

#include <stdint.h>

namespace Sculptor {

bool create_material_layouts();

extern VkDescriptorSetLayout desc_set_layout[3];
extern VkPipelineLayout      material_layout;

struct MaterialInfo {
    uint8_t*                                 shader_ids[4];
    const VkVertexInputAttributeDescription* vertex_attributes;
    float                                    depth_bias;
    uint8_t                                  num_vertex_attributes;
    uint8_t                                  vertex_stride;
    uint8_t                                  color_format;
    uint8_t                                  primitive_topology;
    uint8_t                                  patch_control_points;
    uint8_t                                  polygon_mode;
    uint8_t                                  cull_mode;
    uint8_t                                  depth_test;
    uint8_t                                  diffuse_color[3];
};

bool create_material(const MaterialInfo& mat_info, VkPipeline* pipeline);

struct ShaderMaterial {
    float diffuse_color[4];
};

}
