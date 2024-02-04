// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "main_linux.h"
#include <stdlib.h>
#include <xcb/xcb.h>

const uint32_t* get_window_events()
{
    static uint32_t events[2] = {
        0,
        XCB_EVENT_MASK_KEY_PRESS
    };

    return events;
}

bool install_keyboard_events(void* void_conn)
{
    return true;
}

void handle_key_press(void* event)
{
    handle_gui_event(event);
}

void handle_gui_event(void* event)
{
#ifndef NDEBUG
    // Deliberately leak event memory in release builds without GUI
    // In these builds we don't want/need to pull in the free() function
    free(event);
#endif
}
