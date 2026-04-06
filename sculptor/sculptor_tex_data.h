// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

namespace Sculptor {

constexpr uint32_t max_textures       = 64;
constexpr uint32_t max_modifiers      = 16; // per texture
constexpr uint32_t texture_resolution = 512;

enum class ModifierType : uint32_t {
    solid_color  = 0,
    value_noise  = 1,
    perlin_noise = 2,
    voronoi      = 3,
    gradient     = 4,
    brick        = 5,
    num_types
};

enum class BlendMode : uint32_t {
    normal    = 0,
    multiply  = 1,
    add       = 2,
    subtract  = 3,
    overlay   = 4,
    blend_min = 5,
    blend_max = 6,
    num_modes
};

struct Modifier {
    uint32_t type;
    uint32_t enabled;
    uint32_t blend_mode;
    float    blend_factor;
    float    params[12];
};

// Parameter layout per modifier type (indices into Modifier::params[]):
//
// solid_color:  [0]=R  [1]=G  [2]=B
// value_noise:  [0]=frequency  [1]=amplitude  [2]=octaves  [3]=seed
// perlin_noise: [0]=frequency  [1]=amplitude  [2]=octaves  [3]=seed
// voronoi:      [0]=frequency  [1]=cell_mode (0=distance, 1=cell_id)  [2]=seed
// gradient:     [0]=angle (radians)  [1]=start_R  [2]=start_G  [3]=start_B
//               [4]=end_R  [5]=end_G  [6]=end_B
// brick:        [0]=tile_size_x  [1]=tile_size_y  [2]=mortar_width
//               [3]=mortar_R  [4]=mortar_G  [5]=mortar_B
//               [6]=brick_R  [7]=brick_G  [8]=brick_B

struct TextureSlot {
    char     name[64];
    bool     is_active;
    uint32_t num_modifiers;
    Modifier modifiers[max_modifiers];
};

struct TextureStore {
    TextureSlot slots[max_textures];
    uint32_t    display_order[max_textures]; // maps list position to slot index in the UI
    uint32_t    num_active_slots;
};

}
