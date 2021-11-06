#include "window.h"

#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <xcb/xcb.h>

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
    if (!atom_wm_state)
        return;

    static const char net_wm_state_fullscreen[] = "_NET_WM_STATE_FULLSCREEN";
    xcb_intern_atom_reply_t* const atom_wm_fullscreen = intern_atom(conn,
                                                                    false,
                                                                    net_wm_state_fullscreen,
                                                                    sizeof(net_wm_state_fullscreen) - 1);

    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        window,
                        atom_wm_state->atom,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_wm_fullscreen->atom);
}

int create_window(Window* w)
{
    w->conn = xcb_connect(nullptr, nullptr);

    if ( ! w->conn)
        return 1;

    xcb_screen_t* const screen = xcb_setup_roots_iterator(xcb_get_setup(w->conn)).data;

    const xcb_window_t window = xcb_generate_id(w->conn);

    uint32_t values[2] = {
        0,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_KEY_PRESS
    };

    xcb_create_window(w->conn,
                      XCB_COPY_FROM_PARENT,
                      window,
                      screen->root,
                      0, 0, 1, 1,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                      values);

    set_fullscreen(w->conn, window);

    xcb_map_window(w->conn, window);

    xcb_flush(w->conn);

    return 0;
}

int event_loop(Window* w)
{
    bool quit = false;

    while ( ! quit) {
        xcb_generic_event_t* const event = xcb_wait_for_event(w->conn);

        if ( ! event)
            break;

        switch (event->response_type & ~0x80) {

            case XCB_BUTTON_PRESS: // mouse
            case XCB_KEY_PRESS: // keyboard
                quit = true;
                break;
        }

        //free(event);
    }

    return 0;
}
