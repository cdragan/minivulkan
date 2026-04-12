// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_editor.h"
#include "sculptor_tex_data.h"
#include "../core/resource.h"

namespace Sculptor {

class AssetBrowser;

class TextureEditor : public Editor {
    public:
        TextureEditor() = default;
        ~TextureEditor() override = default;
        const char* get_editor_name() const override;
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
        bool allocate_resources() override;
        void free_resources() override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

        void set_asset_browser(AssetBrowser* browser) { asset_browser = browser; }

        TextureStore&       get_store()       { return store; }
        const TextureStore& get_store() const { return store; }

        // Thumbnail atlas for the asset browser list
        VkDescriptorSet get_thumbnail_atlas() const { return thumbnail_descriptor; }

        static constexpr uint32_t thumbnail_size     = 24;
        static constexpr uint32_t thumbnail_grid     = 8; // 8x8 grid
        static constexpr uint32_t thumbnail_atlas_px = thumbnail_size * thumbnail_grid; // 192

    private:
        AssetBrowser* asset_browser = nullptr;
        TextureStore  store = {};

        // Vulkan resources for texture generation
        Image              texture_array;
        Buffer             modifier_buf;
        VkDescriptorSetLayout texgen_ds_layout = VK_NULL_HANDLE;
        VkPipelineLayout   texgen_pipe_layout  = VK_NULL_HANDLE;
        VkPipeline         texgen_pipeline     = VK_NULL_HANDLE;

        // Thumbnail atlas (2D texture, 192x192, 8x8 grid of 24x24 thumbnails)
        Image              thumbnail_atlas;
        VkSampler          thumbnail_sampler   = VK_NULL_HANDLE;
        VkDescriptorSet    thumbnail_descriptor = VK_NULL_HANDLE;

        bool create_texgen_pipeline();
};

}
