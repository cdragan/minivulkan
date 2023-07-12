// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "gui.h"

#include <stdio.h>
#include <string.h>

static uint32_t window_width  = 800;
static uint32_t window_height = 600;

static bool begins_with(const char* line, size_t len, const char* str)
{
    const size_t str_len = strlen(str);

    return (len >= str_len) && (memcmp(line, str, str_len) == 0);
}

static void load_gui_config()
{
    static bool loaded = false;

    if (loaded)
        return;

    loaded = true;

    FILE* const file = fopen("imgui.ini", "rb");
    if ( ! file)
        return;

    static char buffer[4096];
    const size_t size = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    if ( ! size)
        return;

    bool found_dock_space_viewport = false;

    int pos_x  = 0;
    int pos_y  = 0;
    int width  = 0;
    int height = 0;

    const char*       line = buffer;
    const char* const end  = line + size;
    while (line < end) {
        char* const eol = const_cast<char*>(static_cast<const char*>(
            memchr(line, '\n', static_cast<size_t>(end - line))));
        if ( ! eol)
            break;

        const size_t line_len = static_cast<size_t>(eol - line);

        if (*line == '[') {

            // Stop parsing if we've already found window size
            if (found_dock_space_viewport)
                break;

            found_dock_space_viewport = begins_with(line, line_len, "[Window][DockSpaceViewport_");
        }
        else if (found_dock_space_viewport) {
            *eol = 0;
            if (begins_with(line, line_len, "Pos="))
                sscanf(line + 4, "%d,%d", &pos_x, &pos_y);
            else if (begins_with(line, line_len, "Size="))
                sscanf(line + 5, "%d,%d", &width, &height);
        }

        line = eol + 1;
    }

    if (pos_x + pos_y + width + height) {
        window_width  = static_cast<uint32_t>(width);
        window_height = static_cast<uint32_t>(pos_y + height);
    }
}

uint32_t get_main_window_width()
{
    load_gui_config();
    return window_width;
}

uint32_t get_main_window_height()
{
    load_gui_config();
    return window_height;
}
