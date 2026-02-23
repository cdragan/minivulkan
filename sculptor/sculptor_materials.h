// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/vulkan_functions.h"

#include <stdint.h>

namespace Sculptor {

constexpr uint8_t VK_FORMAT_DISABLED = 255;

bool create_material_layouts();

extern VkDescriptorSetLayout desc_set_layout;
extern VkPipelineLayout      material_layout;
extern VkDescriptorSetLayout lighting_desc_set_layout;
extern VkPipelineLayout      lighting_layout;
extern VkSampler             gbuffer_sampler;

struct ByteColor {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

constexpr inline ByteColor make_byte_color(float red, float green, float blue)
{
    return ByteColor{static_cast<uint8_t>(red * 255.0f), static_cast<uint8_t>(green * 255.0f), static_cast<uint8_t>(blue * 255.0f)};
}

struct MaterialInfo {
    uint8_t*                                 shader_ids[4];
    const VkVertexInputAttributeDescription* vertex_attributes;
    float                                    depth_bias;
    uint8_t                                  num_vertex_attributes;
    uint8_t                                  vertex_stride;
    uint8_t                                  color_formats[4];
    uint8_t                                  primitive_topology;
    uint8_t                                  patch_control_points;
    uint8_t                                  polygon_mode;
    uint8_t                                  cull_mode;
    uint8_t                                  depth_test;
    uint8_t                                  depth_write;
    ByteColor                                diffuse_color;
};

bool create_material(const MaterialInfo& mat_info, VkPipeline* pipeline, VkPipelineLayout layout = VK_NULL_HANDLE);

struct ShaderMaterial {
    float diffuse_color[4];
};

}
