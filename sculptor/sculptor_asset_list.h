// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../core/gui_imgui.h"

#include <stdint.h>

namespace Sculptor {

enum class AssetType : uint8_t {
    unknown  = 0, // marks unused slots
    texture  = 1,
    material = 2,
    geometry = 3,
};

struct AssetSlotHeader {
    char      name[64];
    AssetType type;
};

constexpr uint32_t max_asset_slots = 64;

struct AssetListState {
    uint32_t  display_order[max_asset_slots]; // maps UI list position to slot index
    uint32_t  num_active_slots;
};

struct AssetListAction {
    enum Type {
        none,
        selected,
        added,
        deleted
    };

    Type    type;
    int32_t slot_idx; // the slot affected, or -1
};

// Optional thumbnail atlas info for rendering thumbnails next to item names.
// The atlas is a 2D texture with a grid of thumbnails.
struct ThumbnailAtlas {
    ImTextureID texture_id;
    uint32_t    thumb_size;  // thumbnail size in pixels (square)
    uint32_t    grid_cols;   // number of columns in the atlas grid
};

// Renders a reusable asset list with drag-drop reordering, selection, delete, and add.
AssetListAction render_asset_list(AssetSlotHeader*      slots,
                                  uint32_t              slot_stride,
                                  AssetType             asset_type,
                                  AssetListState*       state,
                                  int32_t*              selected,
                                  const char*           add_label,
                                  const char*           default_name,
                                  const ThumbnailAtlas* thumbs = nullptr);

}
