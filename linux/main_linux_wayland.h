// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "xdg-shell.h" // generated file

struct WaylandCursor {
    wl_surface* cursor_surface;
    uint32_t    hotspot_x;
    uint32_t    hotspot_y;
};

struct WaylandCursors {
    WaylandCursor left_ptr;
};

struct Window {
    wl_display*    display;
    wl_surface*    surface;
    wl_compositor* compositor;
    wl_seat*       seat;
    wl_shm*        shm;
    wl_pointer*    pointer;
    wl_keyboard*   keyboard;
    xdg_wm_base*   wm_base;
    WaylandCursors cursors;
    bool           quit;
};

bool init_wl_gui(Window* w);
void handle_wl_key_press(uint32_t key_code);
void handle_wl_key_release(uint32_t key_code);
void handle_wl_focus(bool focused);
void handle_wl_cursor_enter(wl_pointer* pointer, uint32_t serial, Window* w);
void handle_wl_pointer_motion(float x, float y);
void handle_wl_pointer_button(uint32_t button, uint32_t state);
void handle_wl_scroll(uint32_t axis, float delta);
