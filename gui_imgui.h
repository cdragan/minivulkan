// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#pragma once

#include "vmath.h"

#define IM_VEC2_CLASS_EXTRA                                  \
        constexpr ImVec2(const vmath::vec2& v) : x(v.x), y(v.y) { } \
        constexpr operator vmath::vec2() const { return vmath::vec2(x, y); }

#include "thirdparty/imgui/src/imgui.h"
#include "thirdparty/imgui/src/backends/imgui_impl_vulkan.h"

inline ImTextureID make_texture_id(VkDescriptorSet ds)
{
    union {
        uint64_t        u64;
        ImTextureID     texId;
        VkDescriptorSet ds;
    } convert = { };
    convert.ds = ds;
    return convert.texId;
}
