// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#pragma once

#include "../vulkan_functions.h"
#include <stdint.h>

namespace Sculptor {

class Editor {
    public:
        Editor() = default;
        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        virtual ~Editor() = default;

        void set_object_name(const char* new_name);
        const char* get_object_name() const { return object_name; }

        virtual const char* get_editor_name() const = 0;
        virtual bool create_gui_frame(uint32_t image_idx, bool* need_realloc) = 0;
        virtual bool allocate_resources() = 0;
        virtual void free_resources() = 0;
        virtual bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) = 0;

        bool enabled = true;

    protected:
        char object_name[128] = { };
};

}
