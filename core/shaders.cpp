// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#define DEFINE_SHADERS(X)

#include "shaders.h"
#include "minivulkan.h"
#include <assert.h>

static const uint32_t* decode_shader(uint8_t* code, size_t* out_size)
{
    constexpr uint32_t module_obj_size = 8;
    static_assert(module_obj_size == sizeof(VkShaderModule));

    // Size of SPIR-V header, in 32-bit words
    constexpr uint32_t spirv_header_words = 5;

    const uint16_t* const header = reinterpret_cast<uint16_t*>(code + module_obj_size);
    const uint32_t num_words = *header;
    *out_size = (spirv_header_words + num_words) * sizeof(uint32_t);
    return reinterpret_cast<const uint32_t*>(header + 2);
}

VkShaderModule load_shader(uint8_t* shader)
{
    VkShaderModule* shader_module = reinterpret_cast<VkShaderModule*>(shader);

    if ( ! *shader_module) {
        static VkShaderModuleCreateInfo create_info = {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            nullptr,
            0,
            0,
            nullptr
        };

        create_info.pCode = decode_shader(shader, &create_info.codeSize);

        const VkResult res = CHK(vkCreateShaderModule(vk_dev, &create_info, nullptr, shader_module));
        if (res != VK_SUCCESS)
            return VK_NULL_HANDLE;

        set_vk_object_name(VK_OBJECT_TYPE_SHADER_MODULE, *shader_module, "shader");
    }

    return *shader_module;
}
