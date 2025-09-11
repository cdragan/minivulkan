// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

const uint32_t* get_window_events();
bool install_keyboard_events(void* void_conn);

void handle_key_press(void* event);
void handle_gui_event(void* event);

bool init_wl_gui(void* display, void* surface, bool* quit);
void handle_wl_key_press(uint32_t key_code);
void handle_wl_key_release(uint32_t key_code);
void handle_wl_focus(bool focused);
void handle_wl_pointer_motion(float x, float y);
void handle_wl_pointer_button(uint32_t button, uint32_t state);
void handle_wl_scroll(uint32_t axis, float delta);
