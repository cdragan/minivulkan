// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#pragma once

#include "../core/vmath.h"
#include "../core/vulkan_functions.h"
#include <stdint.h>

#ifdef __APPLE__
#    define CTRL_KEY "Cmd "
#else
#    define CTRL_KEY "Ctrl "
#endif

namespace Sculptor {

class Editor {
    public:
        Editor() = default;
        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;
        virtual ~Editor() = default;

        void set_object_name(const char* new_name);
        const char* get_object_name() const { return object_name; }

        struct UserInput {
            vmath::vec2 abs_mouse_pos;
            vmath::vec2 mouse_pos_delta;
            float       wheel_delta;
        };

        virtual const char* get_editor_name() const = 0;
        virtual bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) = 0;
        virtual bool allocate_resources() = 0;
        virtual void free_resources() = 0;
        virtual bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) = 0;

        void capture_mouse();
        void release_mouse();
        bool has_captured_mouse() const;
        static bool is_mouse_captured();

        bool enabled = true;

    protected:
        static vmath::vec2 get_rel_mouse_pos(const UserInput& input);

        char object_name[128] = { };
};

}
