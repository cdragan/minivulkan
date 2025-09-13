// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "main_linux.h"
#include "main_linux_wayland.h"
#include <stdlib.h>

bool init_wl_gui(Window*)
{
    return false;
}

void handle_wl_key_press(uint32_t)
{
}

void handle_wl_key_release(uint32_t)
{
}

void handle_wl_focus(bool)
{
}

void handle_wl_cursor_enter(wl_pointer*, uint32_t, Window*)
{
}

void handle_wl_pointer_motion(float x, float y)
{
}

void handle_wl_pointer_button(uint32_t button, uint32_t state)
{
}

void handle_wl_scroll(uint32_t axis, float delta)
{
}
