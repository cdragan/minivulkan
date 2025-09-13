// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "../core/d_printf.h"
#include "../core/gui.h"
#include "main_linux.h"
#include "main_linux_wayland.h"
#include "../core/minivulkan.h"

#include "../thirdparty/imgui/src/imgui.h"

#include <errno.h>
#include <libdecor-0/libdecor.h>
#include <linux/input-event-codes.h>

static bool window_needs_update;

bool need_redraw(struct Window*)
{
    const bool needs_update = window_needs_update;

    window_needs_update = false;

    return needs_update;
}

static void decor_error(libdecor*      context,
                        libdecor_error error,
                        const char*    message)
{
}

static bool decor_configured;

static void decor_configure(libdecor_frame*         frame,
                            libdecor_configuration* configuration,
                            void*                   user_data)
{
    const uint32_t width  = vk_window_extent.width  ? vk_window_extent.width  : get_main_window_width();
    const uint32_t height = vk_window_extent.height ? vk_window_extent.height : get_main_window_height();

    vk_window_extent.width  = width;
    vk_window_extent.height = height;

    d_printf("Decor resize %ux%u\n", width, height);

    libdecor_state* const state = libdecor_state_new(width, height);
    libdecor_frame_commit(frame, state, nullptr);
    libdecor_state_free(state);

    decor_configured = true;
}

static void decor_close(libdecor_frame* frame,
                        void*           user_data)
{
    *static_cast<bool*>(user_data) = true;
}

static void decor_commit(libdecor_frame* frame,
                         void*           user_data)
{
}

static void decor_dismiss_popup(libdecor_frame* frame,
                                const char*     seat_name,
                                void*           user_data)
{
}

static WaylandCursor create_cursor(const char*      name,
                                   wl_compositor*   compositor,
                                   wl_cursor_theme* cursor_theme)
{
    wl_cursor* const cursor = wl_cursor_theme_get_cursor(cursor_theme, name);
    if ( ! cursor)
        return { };

    wl_cursor_image* const image = cursor->images[0];
    if ( ! image)
        return { };

    wl_surface* const surface = wl_compositor_create_surface(compositor);
    if ( ! surface)
        return { };

    wl_buffer* const buffer = wl_cursor_image_get_buffer(image);
    if ( ! buffer)
        return { };

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);

    return { surface, image->hotspot_x, image->hotspot_y };
}

bool init_wl_gui(Window* w)
{
    wl_cursor_theme* cursor_theme = nullptr;

    if ( ! w->shm) {
        d_printf("Wayland shared memory is not available\n");
    }
    else {
        cursor_theme = wl_cursor_theme_load(nullptr, 24, w->shm);

        if ( ! cursor_theme)
            d_printf("Wayland cursor theme is not available\n");
    }

    if (cursor_theme) {
        w->cursors.left_ptr = create_cursor("left_ptr", w->compositor, cursor_theme);
    }

    static libdecor_interface decor_callbacks = {
        .error = decor_error
    };

    libdecor* const context = libdecor_new(w->display, &decor_callbacks);
    if ( ! context) {
        d_printf("Failed to initialize libdecor\n");
        return false;
    }

    static libdecor_frame_interface frame_callbacks = {
        .configure     = decor_configure,
        .close         = decor_close,
        .commit        = decor_commit,
        .dismiss_popup = decor_dismiss_popup
    };

    libdecor_frame* const frame = libdecor_decorate(context,
                                                    w->surface,
                                                    &frame_callbacks,
                                                    &w->quit);
    if ( ! frame) {
        d_printf("Failed to create libdecor frame\n");
        return false;
    }

    libdecor_frame_set_app_id(frame, app_name);
    //libdecor_frame_set_title(frame, app_name);

    libdecor_frame_map(frame);

    while ( ! decor_configured) {
        if (wl_display_roundtrip(w->display) == -1) {
            d_printf("Failed to dispatch Wayland events: %s\n", strerror(errno));
            return false;
        }
    }

    return true;
}

#if 1
#   define CHARKEY(x) x
#else
#   define CHARKEY(x) ImGuiKey_None
#endif

static ImGuiKey map_key(uint32_t key)
{
    switch (key) {
        case KEY_ESC:        return ImGuiKey_Escape;
        case KEY_1:          return CHARKEY(ImGuiKey_1);
        case KEY_2:          return CHARKEY(ImGuiKey_2);
        case KEY_3:          return CHARKEY(ImGuiKey_3);
        case KEY_4:          return CHARKEY(ImGuiKey_4);
        case KEY_5:          return CHARKEY(ImGuiKey_5);
        case KEY_6:          return CHARKEY(ImGuiKey_6);
        case KEY_7:          return CHARKEY(ImGuiKey_7);
        case KEY_8:          return CHARKEY(ImGuiKey_8);
        case KEY_9:          return CHARKEY(ImGuiKey_9);
        case KEY_0:          return CHARKEY(ImGuiKey_0);
        case KEY_MINUS:      return CHARKEY(ImGuiKey_Minus);
        case KEY_EQUAL:      return CHARKEY(ImGuiKey_Equal);
        case KEY_BACKSPACE:  return ImGuiKey_Backspace;
        case KEY_TAB:        return ImGuiKey_Tab;
        case KEY_Q:          return CHARKEY(ImGuiKey_Q);
        case KEY_W:          return CHARKEY(ImGuiKey_W);
        case KEY_E:          return CHARKEY(ImGuiKey_E);
        case KEY_R:          return CHARKEY(ImGuiKey_R);
        case KEY_T:          return CHARKEY(ImGuiKey_T);
        case KEY_Y:          return CHARKEY(ImGuiKey_Y);
        case KEY_U:          return CHARKEY(ImGuiKey_U);
        case KEY_I:          return CHARKEY(ImGuiKey_I);
        case KEY_O:          return CHARKEY(ImGuiKey_O);
        case KEY_P:          return CHARKEY(ImGuiKey_P);
        case KEY_LEFTBRACE:  return CHARKEY(ImGuiKey_LeftBracket);
        case KEY_RIGHTBRACE: return CHARKEY(ImGuiKey_RightBracket);
        case KEY_ENTER:      return ImGuiKey_Enter;
        case KEY_LEFTCTRL:   return ImGuiKey_LeftCtrl;
        case KEY_A:          return CHARKEY(ImGuiKey_A);
        case KEY_S:          return CHARKEY(ImGuiKey_S);
        case KEY_D:          return CHARKEY(ImGuiKey_D);
        case KEY_F:          return CHARKEY(ImGuiKey_F);
        case KEY_G:          return CHARKEY(ImGuiKey_G);
        case KEY_H:          return CHARKEY(ImGuiKey_H);
        case KEY_J:          return CHARKEY(ImGuiKey_J);
        case KEY_K:          return CHARKEY(ImGuiKey_K);
        case KEY_L:          return CHARKEY(ImGuiKey_L);
        case KEY_SEMICOLON:  return CHARKEY(ImGuiKey_Semicolon);
        case KEY_APOSTROPHE: return CHARKEY(ImGuiKey_Apostrophe);
        case KEY_GRAVE:      return CHARKEY(ImGuiKey_GraveAccent);
        case KEY_LEFTSHIFT:  return ImGuiKey_LeftShift;
        case KEY_BACKSLASH:  return CHARKEY(ImGuiKey_Backslash);
        case KEY_Z:          return CHARKEY(ImGuiKey_Z);
        case KEY_X:          return CHARKEY(ImGuiKey_X);
        case KEY_C:          return CHARKEY(ImGuiKey_C);
        case KEY_V:          return CHARKEY(ImGuiKey_V);
        case KEY_B:          return CHARKEY(ImGuiKey_B);
        case KEY_N:          return CHARKEY(ImGuiKey_N);
        case KEY_M:          return CHARKEY(ImGuiKey_M);
        case KEY_COMMA:      return CHARKEY(ImGuiKey_Comma);
        case KEY_DOT:        return CHARKEY(ImGuiKey_Period);
        case KEY_SLASH:      return CHARKEY(ImGuiKey_Slash);
        case KEY_RIGHTSHIFT: return ImGuiKey_RightShift;
        case KEY_KPASTERISK: return ImGuiKey_KeypadMultiply;
        case KEY_LEFTALT:    return ImGuiKey_LeftAlt;
        case KEY_SPACE:      return CHARKEY(ImGuiKey_Space);
        case KEY_CAPSLOCK:   return ImGuiKey_CapsLock;
        case KEY_F1:         return ImGuiKey_F1;
        case KEY_F2:         return ImGuiKey_F2;
        case KEY_F3:         return ImGuiKey_F3;
        case KEY_F4:         return ImGuiKey_F4;
        case KEY_F5:         return ImGuiKey_F5;
        case KEY_F6:         return ImGuiKey_F6;
        case KEY_F7:         return ImGuiKey_F7;
        case KEY_F8:         return ImGuiKey_F8;
        case KEY_F9:         return ImGuiKey_F9;
        case KEY_F10:        return ImGuiKey_F10;

        case KEY_KP7:        return ImGuiKey_Keypad7;
        case KEY_KP8:        return ImGuiKey_Keypad8;
        case KEY_KP9:        return ImGuiKey_Keypad9;
        case KEY_KPMINUS:    return ImGuiKey_KeypadSubtract;
        case KEY_KP4:        return ImGuiKey_Keypad4;
        case KEY_KP5:        return ImGuiKey_Keypad5;
        case KEY_KP6:        return ImGuiKey_Keypad6;
        case KEY_KPPLUS:     return ImGuiKey_KeypadAdd;
        case KEY_KP1:        return ImGuiKey_Keypad1;
        case KEY_KP2:        return ImGuiKey_Keypad2;
        case KEY_KP3:        return ImGuiKey_Keypad3;
        case KEY_KP0:        return ImGuiKey_Keypad0;
        case KEY_KPDOT:      return ImGuiKey_KeypadDecimal;

        case KEY_F11:        return ImGuiKey_F11;
        case KEY_F12:        return ImGuiKey_F12;

        case KEY_KPENTER:    return ImGuiKey_KeypadEnter;
        case KEY_RIGHTCTRL:  return ImGuiKey_RightCtrl;
        case KEY_KPSLASH:    return ImGuiKey_KeypadDivide;

        case KEY_RIGHTALT:   return ImGuiKey_RightAlt;

        case KEY_HOME:       return ImGuiKey_Home;
        case KEY_UP:         return ImGuiKey_UpArrow;
        case KEY_PAGEUP:     return ImGuiKey_PageUp;
        case KEY_LEFT:       return ImGuiKey_LeftArrow;
        case KEY_RIGHT:      return ImGuiKey_RightArrow;
        case KEY_END:        return ImGuiKey_End;
        case KEY_DOWN:       return ImGuiKey_DownArrow;
        case KEY_PAGEDOWN:   return ImGuiKey_PageDown;

        case KEY_INSERT:     return ImGuiKey_Insert;
        case KEY_DELETE:     return ImGuiKey_Delete;

        case KEY_KPEQUAL:    return ImGuiKey_KeypadEqual;

        case KEY_LEFTMETA:   return ImGuiKey_LeftSuper;
        case KEY_RIGHTMETA:  return ImGuiKey_RightSuper;

        default:             break;
    }
    return ImGuiKey_None;
}

static void handle_key_event(uint32_t key_code, bool down)
{
    ImGuiIO& io = ImGui::GetIO();

    const ImGuiKey key = map_key(key_code);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, down);
        io.SetKeyEventNativeData(key, key_code, -1);
    }

    window_needs_update = true;
}

void handle_wl_key_press(uint32_t key_code)
{
    handle_key_event(key_code, true);
}

void handle_wl_key_release(uint32_t key_code)
{
    handle_key_event(key_code, false);
}

void handle_wl_focus(bool focused)
{
    ImGui::GetIO().AddFocusEvent(focused);
}

void handle_wl_cursor_enter(wl_pointer* pointer, uint32_t serial, Window* w)
{
    WaylandCursor& left_ptr = w->cursors.left_ptr;
    if (left_ptr.cursor_surface)
        wl_pointer_set_cursor(pointer,
                              serial,
                              left_ptr.cursor_surface,
                              left_ptr.hotspot_x,
                              left_ptr.hotspot_y);
}

void handle_wl_pointer_motion(float x, float y)
{
    ImGui::GetIO().AddMousePosEvent(x, y);
}

void handle_wl_pointer_button(uint32_t button, uint32_t state)
{
    const int button_idx = (int)button - BTN_LEFT;
    if (button_idx >= 0 && button_idx < (int)ImGuiMouseButton_COUNT)
        ImGui::GetIO().AddMouseButtonEvent(button_idx, !! state);
}

void handle_wl_scroll(uint32_t axis, float delta)
{
    if (axis == 0 && delta != 0.0f)
        ImGui::GetIO().AddMouseWheelEvent(0, (delta > 0.0f) ? 1 : -1);
}
