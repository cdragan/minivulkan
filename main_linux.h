// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include <stdint.h>

const uint32_t* get_window_events();
bool install_keyboard_events(void* void_conn);
void handle_key_press(void* event);
void handle_gui_event(void* event);
