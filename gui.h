// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"

extern const int      gui_config_flags;
extern const unsigned gui_num_descriptors;
extern float          vk_surface_scale;

bool init_gui();
bool send_gui_to_gpu(VkCommandBuffer cmdbuf);
bool is_full_screen();
