// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "main_linux.h"

const uint32_t* get_window_events()
{
    static uint32_t events[2] = {
        0,
        XCB_EVENT_MASK_KEY_PRESS
    };

    return events;
}

bool install_keyboard_events()
{
    return true;
}

void handle_key_press(void* event)
{
}

void handle_gui_event(void* event)
{
}
