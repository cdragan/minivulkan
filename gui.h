// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"

extern int gui_config_flags;

bool init_gui();
bool send_gui_to_gpu(VkCommandBuffer cmdbuf);
bool is_full_screen();
