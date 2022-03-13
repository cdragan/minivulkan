// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "shaders.h"
#include "minivulkan.h"
#include <assert.h>

#include "simple.vert.h"
#include "phong.frag.h"
#include "pass_through.vert.h"
#include "rounded_cube.vert.h"
#include "bezier_surface_quadratic.tesc.h"
#include "bezier_surface_quadratic.tese.h"
#include "bezier_surface_cubic.tesc.h"
#include "bezier_surface_cubic.tese.h"

static const uint32_t* decode_shader(uint8_t* code, size_t* out_size)
{
    static uint32_t out_code[64 * 1024];

    constexpr uint32_t module_obj_size = 8;
    static_assert(module_obj_size == sizeof(VkShaderModule));
    uint16_t* header = reinterpret_cast<uint16_t*>(code + module_obj_size);

    const uint32_t total_opcodes = *(header++);
    const uint32_t total_words   = *(header++);
    const uint32_t version       = *(header++);
    const uint32_t bound         = *(header++);
    assert(total_opcodes);
    assert(total_words > total_opcodes);

    uint8_t* opcodes = reinterpret_cast<uint8_t*>(header);

    constexpr uint32_t spirv_header_words = 5;

    const uint32_t size = (spirv_header_words + total_words) * sizeof(uint32_t);
    assert(size <= sizeof(out_code));

    // Fill out SPIR-V header
    out_code[0] = 0x07230203;
    out_code[1] = version << 8;
    out_code[3] = bound;

    uint8_t*       output         = reinterpret_cast<uint8_t*>(out_code + 5);
    const uint8_t* operands       = opcodes + total_opcodes * 4;
    const uint32_t total_operands = total_words - total_opcodes;

    uint32_t num_left = total_opcodes;
    do {
        output[0] = opcodes[0];
        output[1] = opcodes[total_opcodes];

        const uint32_t num_operands_lo = opcodes[total_opcodes * 2];
        const uint32_t num_operands_hi = opcodes[total_opcodes * 3];
        const uint32_t num_operands    = (num_operands_hi << 8) + num_operands_lo;

        reinterpret_cast<uint16_t*>(output)[1] = static_cast<uint16_t>(num_operands + 1);
        output += 4;
        ++opcodes;

        const uint8_t* const operands_end = operands + num_operands;
        for ( ; operands < operands_end; operands++) {
            output[0] = operands[0];
            output[1] = operands[total_operands];
            output[2] = operands[total_operands * 2];
            output[3] = operands[total_operands * 3];
            output += 4;
        }
    } while (--num_left);

    *out_size = size;
    return out_code;
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
    }

    return *shader_module;
}