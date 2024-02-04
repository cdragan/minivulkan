// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#pragma once

#include "vmath.h"

#define IM_VEC2_CLASS_EXTRA                                  \
        constexpr ImVec2(const vmath::vec2& v) : x(v.x), y(v.y) { } \
        constexpr operator vmath::vec2() const { return vmath::vec2(x, y); }

#include "thirdparty/imgui/src/imgui.h"
#include "thirdparty/imgui/src/backends/imgui_impl_vulkan.h"
