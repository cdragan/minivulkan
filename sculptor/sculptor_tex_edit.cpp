// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_tex_edit.h"
#include "sculptor_materials.h"

#include "../core/d_printf.h"
#include "../core/minivulkan.h"
#include "../core/resource.h"

#include "sculptor_shaders.h"
#include "../core/shaders.h"

const char* Sculptor::TextureEditor::get_editor_name() const
{
    return "Texture Editor";
}

bool Sculptor::TextureEditor::create_texgen_pipeline()
{
    static const DescSetBindingInfo bindings[] = {
        { 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1 }, // output texture array
        { 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, // modifier data
        { 1, 0, 0, 0 }  // terminator
    };

    if ( ! create_compute_descriptor_set_layouts(bindings, 1, &texgen_ds_layout))
        return false;

    const ComputeShaderInfo shader_info = {
        shader_sculptor_texgen_comp,
        1 // uint texture_index
    };

    const VkDescriptorSetLayout ds_layouts[] = { texgen_ds_layout, VK_NULL_HANDLE };
    if ( ! create_compute_shader(shader_info, ds_layouts, nullptr,
                                 &texgen_pipe_layout, &texgen_pipeline))
        return false;

    return true;
}

bool Sculptor::TextureEditor::allocate_resources()
{
    if ( ! texgen_pipeline) {
        if ( ! create_texgen_pipeline())
            return false;
    }

    if ( ! texture_array.allocated()) {
        static const ImageInfo tex_array_info = {
            texture_resolution,
            texture_resolution,
            VK_FORMAT_R8G8B8A8_UNORM,
            1,            // mip_levels (TODO: add mip generation)
            max_textures, // array_layers
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            Usage::device_only
        };

        if ( ! texture_array.allocate(tex_array_info, "texture array"))
            return false;
    }

    if ( ! modifier_buf.allocated()) {
        const uint32_t modifier_buf_size = max_textures * max_modifiers * sizeof(Modifier);
        if ( ! modifier_buf.allocate(Usage::dynamic,
                                     modifier_buf_size,
                                     VK_FORMAT_UNDEFINED,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     "texture modifier buffer"))
            return false;
    }

    return true;
}

void Sculptor::TextureEditor::free_resources()
{
    texture_array.free();
}

bool Sculptor::TextureEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    // TODO: texture editor GUI
    return true;
}

bool Sculptor::TextureEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    // TODO: dispatch compute shader when textures change
    return true;
}
