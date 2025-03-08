// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "d_printf.h"
#include "gui.h"
#include "main_linux.h"
#include "minivulkan.h"

#include <string.h>
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

static xcb_intern_atom_reply_t* intern_atom(xcb_connection_t* conn,
                                            const char*       str,
                                            uint16_t          str_size)
{
    const xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, false, str_size, str);

    return xcb_intern_atom_reply(conn, cookie, nullptr);
}

static void set_fullscreen(xcb_connection_t* conn,
                           xcb_window_t      window)
{
    static const char net_wm_state[] = "_NET_WM_STATE";
    xcb_intern_atom_reply_t* const atom_wm_state = intern_atom(conn,
                                                               net_wm_state,
                                                               sizeof(net_wm_state) - 1);
    if ( ! atom_wm_state) {
        d_printf("Failed to get _NET_WM_STATE atom\n");
        return;
    }

    static const char net_wm_state_fullscreen[] = "_NET_WM_STATE_FULLSCREEN";
    xcb_intern_atom_reply_t* const atom_wm_fullscreen = intern_atom(conn,
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

static xcb_intern_atom_reply_t* atom_wm_delete_window;

static bool create_window(Window* w)
{
    w->connection = xcb_connect(nullptr, nullptr);

    if ( ! w->connection) {
        d_printf("Failed to connect to X server\n");
        return false;
    }

    const bool full_screen = is_full_screen();

    // To reduce number of symbols pulled in, only check for error
    // in non-fullscreen builds (typically builds with GUI)
    if ( ! full_screen && xcb_connection_has_error(w->connection)) {
        d_printf("Failed to connect to X server\n");
        return false;
    }

    xcb_screen_t* const screen = xcb_setup_roots_iterator(xcb_get_setup(w->connection)).data;

    w->window = xcb_generate_id(w->connection);

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

    static const char wm_delete_window[] = "WM_DELETE_WINDOW";
    atom_wm_delete_window = intern_atom(w->connection, wm_delete_window, sizeof(wm_delete_window) - 1);

    static const char wm_protocols[] = "WM_PROTOCOLS";
    xcb_intern_atom_reply_t* const atom_wm_protocols = intern_atom(w->connection,
                                                                   wm_protocols,
                                                                   sizeof(wm_protocols) - 1);

    xcb_change_property(w->connection,
                        XCB_PROP_MODE_REPLACE,
                        w->window,
                        atom_wm_protocols->atom,
                        4,
                        32,
                        1,
                        &atom_wm_delete_window->atom);

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

                    constexpr uint8_t keycode_esc = 9;
                    if (key_event->detail == keycode_esc)
                        quit = true;

                    handle_key_press(event);
                    break;
                }

                case XCB_CLIENT_MESSAGE: {
                    xcb_client_message_event_t* const client_event =
                        reinterpret_cast<xcb_client_message_event_t*>(event);
                    if (client_event->data.data32[0] == atom_wm_delete_window->atom)
                        quit = true;
                    break;
                }

                default:
                    handle_gui_event(event);
                    break;
            }
        }

        if ( ! need_redraw(w) && skip_frame(w))
            continue;

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
