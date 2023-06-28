// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"

extern const int      gui_config_flags;
extern const unsigned gui_num_descriptors;
extern float          vk_surface_scale;

enum class GuiClear {
    preserve,
    clear
};

bool init_gui(GuiClear clear);
bool send_gui_to_gpu(VkCommandBuffer cmdbuf, uint32_t image_idx);

bool is_full_screen();
uint32_t get_main_window_width();
uint32_t get_main_window_height();
void resize_gui();
