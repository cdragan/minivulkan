// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "d_printf.h"
#include "gui.h"
#include "main_linux.h"
#include "minivulkan.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

struct Window {
    xcb_connection_t* connection;
    xcb_window_t      window;
};

bool create_surface(struct Window* w)
{
    static VkXcbSurfaceCreateInfoKHR surf_create_info = {
        VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        nullptr,
        0,       // flags
        nullptr, // connection
        0        // window
    };

    surf_create_info.connection = w->connection;
    surf_create_info.window     = w->window;

    const VkResult res = CHK(vkCreateXcbSurfaceKHR(vk_instance,
                                                   &surf_create_info,
                                                   nullptr,
                                                   &vk_surface));
    return res == VK_SUCCESS;
}

uint64_t get_current_time_ms()
{
    uint64_t time_ms = 0;

    struct timespec ts;

    if ( ! clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
        time_ms =  static_cast<uint64_t>(ts.tv_sec) * 1000;
        time_ms += static_cast<uint64_t>(ts.tv_nsec) / 1'000'000;
    }

    return time_ms;
}

bool load_sound_track(const void* data, uint32_t size)
{
    // TODO
    return true;
}

bool play_sound_track()
{
    // TODO
    return true;
}

static xcb_intern_atom_reply_t* intern_atom(xcb_connection_t* conn,
                                            bool              only_if_exists,
                                            const char*       str,
                                            uint16_t          str_size)
{
    const xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, only_if_exists, str_size, str);

    return xcb_intern_atom_reply(conn, cookie, nullptr);
}

static void set_fullscreen(xcb_connection_t* conn,
                           xcb_window_t      window)
{
    static const char net_wm_state[] = "_NET_WM_STATE";
    xcb_intern_atom_reply_t* const atom_wm_state = intern_atom(conn,
                                                               false,
                                                               net_wm_state,
                                                               sizeof(net_wm_state) - 1);
    if ( ! atom_wm_state) {
        d_printf("Failed to get _NET_WM_STATE atom\n");
        return;
    }

    static const char net_wm_state_fullscreen[] = "_NET_WM_STATE_FULLSCREEN";
    xcb_intern_atom_reply_t* const atom_wm_fullscreen = intern_atom(conn,
                                                                    false,
                                                                    net_wm_state_fullscreen,
                                                                    sizeof(net_wm_state_fullscreen) - 1);
    if ( ! atom_wm_fullscreen) {
        d_printf("Failed to get _NET_WM_STATE_FULLSCREEN atom\n");
        return;
    }

    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        window,
                        atom_wm_state->atom,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_wm_fullscreen->atom);

    xcb_xfixes_query_version(conn, 4, 0);
    xcb_xfixes_hide_cursor(conn, window);
}

static bool create_window(Window* w)
{
    w->connection = xcb_connect(nullptr, nullptr);

    if ( ! w->connection) {
        d_printf("Failed to connect to X server\n");
        return false;
    }

    xcb_screen_t* const screen = xcb_setup_roots_iterator(xcb_get_setup(w->connection)).data;

    w->window = xcb_generate_id(w->connection);

    const bool full_screen = is_full_screen();

    const uint16_t width  = full_screen ? screen->width_in_pixels  : static_cast<uint16_t>(get_main_window_width());
    const uint16_t height = full_screen ? screen->height_in_pixels : static_cast<uint16_t>(get_main_window_height());

    xcb_create_window(w->connection,
                      XCB_COPY_FROM_PARENT,
                      w->window,
                      screen->root,
                      0, 0, width, height,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                      get_window_events());

    xcb_change_property(w->connection,
                        XCB_PROP_MODE_REPLACE,
                        w->window,
                        XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING,
                        8,
                        static_cast<uint32_t>(strlen(app_name)),
                        app_name);

    if (full_screen)
        set_fullscreen(w->connection, w->window);

    xcb_map_window(w->connection, w->window);

    xcb_flush(w->connection);

    return install_keyboard_events(w->connection);
}

static int event_loop(Window* w)
{
    bool quit = false;

    while ( ! quit) {

        for (;;) {
            xcb_generic_event_t* const event = xcb_poll_for_event(w->connection);

            if ( ! event)
                break;

            switch (event->response_type & ~0x80) {

                case XCB_KEY_PRESS: {
                    xcb_key_press_event_t* const key_event =
                        reinterpret_cast<xcb_key_press_event_t*>(event);

                    handle_key_press(event);

                    constexpr uint8_t keycode_esc = 9;
                    if (key_event->detail == keycode_esc)
                        quit = true;
                    break;
                }

                default:
                    handle_gui_event(event);
                    break;
            }

            free(event);
        }

        if ( ! draw_frame())
            return 1;
    }

    idle_queue();

    return 0;
}

int main()
{
    Window w;

    if ( ! create_window(&w))
        return 1;

    if ( ! init_vulkan(&w))
        return 1;

    return event_loop(&w);
}
