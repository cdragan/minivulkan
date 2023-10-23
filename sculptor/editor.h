// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#pragma once

#include "../vulkan_functions.h"
#include <stdint.h>

class Editor {
    public:
        Editor() = default;
        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        virtual ~Editor() = default;

        virtual bool create_gui_frame(uint32_t image_idx) = 0;
        virtual bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) = 0;
};
