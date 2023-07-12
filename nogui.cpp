// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "gui.h"

bool init_gui(GuiClear clear)
{
    return true;
}

void resize_gui()
{
}

bool send_gui_to_gpu(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    return true;
}

bool is_full_screen()
{
    return true;
}

uint32_t get_main_window_width()
{
    return 800;
}

uint32_t get_main_window_height()
{
    return 600;
}
