// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_asset_list.h"
#include "sculptor_editor.h"
#include "../core/resource.h"

namespace Sculptor {

class AssetBrowser : public Editor {
    public:
        ~AssetBrowser() override = default;
        const char* get_editor_name() const override;
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
        bool allocate_resources() override;
        void free_resources() override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

        enum class AssetTab {
            geometries,
            textures,
            materials,
            num_tabs
        };

        // Register an asset type's data for a tab
        void register_tab(AssetTab        tab,
                          AssetSlotHeader* slots,
                          uint32_t        slot_stride,
                          AssetType       asset_type,
                          AssetListState* state,
                          const char*     add_label,
                          const char*     default_name);

        AssetTab get_active_tab()    const { return active_tab; }
        int32_t  get_selected_slot() const;

    private:
        struct TabData {
            AssetSlotHeader* slots        = nullptr;
            uint32_t         slot_stride  = 0;
            AssetType        asset_type   = AssetType::unknown;
            AssetListState*  state        = nullptr;
            int32_t          selected     = -1;
            const char*      add_label    = nullptr;
            const char*      default_name = nullptr;
        };

        AssetTab active_tab = AssetTab::textures;
        TabData  tabs[static_cast<int>(AssetTab::num_tabs)] = {};

        // Icon strip for tab buttons
        ImageWithHostCopy icon_image;
        VkSampler         icon_sampler  = VK_NULL_HANDLE;
        VkDescriptorSet   icon_texture  = VK_NULL_HANDLE;
        float             icon_h        = 0;
        float             tab_uv_width  = 0; // UV width per tab icon

        bool render_tab_button(AssetTab tab);
};

}
