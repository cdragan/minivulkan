// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_editor.h"
#include "sculptor_tex_data.h"
#include "sculptor_undo.h"
#include "../core/resource.h"

namespace Sculptor {

class TextureEditor : public Editor {
    public:
        ~TextureEditor() override = default;
        const char* get_editor_name() const override;
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
        bool allocate_resources() override;
        void free_resources() override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

    private:
        TextureStore store = {};

        Image              texture_array;
        Buffer             modifier_buf;
        VkSampler          texture_sampler     = VK_NULL_HANDLE;
        VkDescriptorSetLayout texgen_ds_layout = VK_NULL_HANDLE;
        VkPipelineLayout   texgen_pipe_layout  = VK_NULL_HANDLE;
        VkPipeline         texgen_pipeline     = VK_NULL_HANDLE;

        bool create_texgen_pipeline();
};

}
