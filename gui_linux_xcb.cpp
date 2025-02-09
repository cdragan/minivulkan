// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "d_printf.h"
#include "main_linux.h"

#include "thirdparty/imgui/src/imgui.h"

#include <assert.h>
#include <stdlib.h>
#include <xcb/xcb.h>

const uint32_t* get_window_events()
{
    static uint32_t events[2] = {
        0,
        XCB_EVENT_MASK_KEY_PRESS
        | XCB_EVENT_MASK_KEY_RELEASE
        | XCB_EVENT_MASK_POINTER_MOTION
        | XCB_EVENT_MASK_BUTTON_MOTION
        | XCB_EVENT_MASK_BUTTON_PRESS
        | XCB_EVENT_MASK_BUTTON_RELEASE
        | XCB_EVENT_MASK_ENTER_WINDOW
        | XCB_EVENT_MASK_LEAVE_WINDOW
        | XCB_EVENT_MASK_EXPOSURE
    };

    return events;
}

static bool window_needs_update = false;

bool need_redraw(struct Window*)
{
    const bool needs_update = window_needs_update;

    window_needs_update = false;

    return needs_update;
}

constexpr uint8_t min_keycode = 10;
constexpr uint8_t max_keycode = 91;

static uint32_t      xcb_keysyms_per_code = 0;
static xcb_keysym_t* xcb_keysyms          = nullptr;

bool install_keyboard_events(void* void_conn)
{
    xcb_connection_t* const conn = static_cast<xcb_connection_t*>(void_conn);

    constexpr uint8_t num_keycodes = max_keycode - min_keycode + 1;
    const auto cookie = xcb_get_keyboard_mapping_unchecked(conn,
                                                           min_keycode,
                                                           num_keycodes);
    xcb_generic_error_t* error = nullptr;
    const auto reply = xcb_get_keyboard_mapping_reply(conn,
                                                      cookie,
                                                      &error);
    if (reply && ! error) {
        xcb_keysyms_per_code = reply->keysyms_per_keycode;
        xcb_keysyms          = xcb_get_keyboard_mapping_keysyms(reply);
        assert(num_keycodes * xcb_keysyms_per_code ==
               static_cast<uint32_t>(xcb_get_keyboard_mapping_keysyms_length(reply)));
    }
    else {
        d_printf("Failed to get keyboard mappings\n");
        return false;
    }

    return true;
}

#if 0
#   define CHARKEY(x) x
#else
#   define CHARKEY(x) ImGuiKey_None
#endif

static ImGuiKey map_key(xcb_keycode_t key)
{
    switch (key) {
        case 9:   return ImGuiKey_Escape;
        case 10:  return CHARKEY(ImGuiKey_1);
        case 11:  return CHARKEY(ImGuiKey_2);
        case 12:  return CHARKEY(ImGuiKey_3);
        case 13:  return CHARKEY(ImGuiKey_4);
        case 14:  return CHARKEY(ImGuiKey_5);
        case 15:  return CHARKEY(ImGuiKey_6);
        case 16:  return CHARKEY(ImGuiKey_7);
        case 17:  return CHARKEY(ImGuiKey_8);
        case 18:  return CHARKEY(ImGuiKey_9);
        case 19:  return CHARKEY(ImGuiKey_0);
        case 20:  return CHARKEY(ImGuiKey_Minus);
        case 21:  return CHARKEY(ImGuiKey_Equal);
        case 22:  return ImGuiKey_Backspace;
        case 23:  return ImGuiKey_Tab;
        case 24:  return CHARKEY(ImGuiKey_Q);
        case 25:  return CHARKEY(ImGuiKey_W);
        case 26:  return CHARKEY(ImGuiKey_E);
        case 27:  return CHARKEY(ImGuiKey_R);
        case 28:  return CHARKEY(ImGuiKey_T);
        case 29:  return CHARKEY(ImGuiKey_Y);
        case 30:  return CHARKEY(ImGuiKey_U);
        case 31:  return CHARKEY(ImGuiKey_I);
        case 32:  return CHARKEY(ImGuiKey_O);
        case 33:  return CHARKEY(ImGuiKey_P);
        case 34:  return CHARKEY(ImGuiKey_LeftBracket);
        case 35:  return CHARKEY(ImGuiKey_RightBracket);
        case 36:  return ImGuiKey_Enter;
        case 37:  return ImGuiKey_LeftCtrl;
        case 38:  return CHARKEY(ImGuiKey_A);
        case 39:  return CHARKEY(ImGuiKey_S);
        case 40:  return CHARKEY(ImGuiKey_D);
        case 41:  return CHARKEY(ImGuiKey_F);
        case 42:  return CHARKEY(ImGuiKey_G);
        case 43:  return CHARKEY(ImGuiKey_H);
        case 44:  return CHARKEY(ImGuiKey_J);
        case 45:  return CHARKEY(ImGuiKey_K);
        case 46:  return CHARKEY(ImGuiKey_L);
        case 47:  return CHARKEY(ImGuiKey_Semicolon);
        case 48:  return CHARKEY(ImGuiKey_Apostrophe);
        case 49:  return CHARKEY(ImGuiKey_GraveAccent);
        case 50:  return ImGuiKey_LeftShift;
        case 51:  return CHARKEY(ImGuiKey_Backslash);
        case 52:  return CHARKEY(ImGuiKey_Z);
        case 53:  return CHARKEY(ImGuiKey_X);
        case 54:  return CHARKEY(ImGuiKey_C);
        case 55:  return CHARKEY(ImGuiKey_V);
        case 56:  return CHARKEY(ImGuiKey_B);
        case 57:  return CHARKEY(ImGuiKey_N);
        case 58:  return CHARKEY(ImGuiKey_M);
        case 59:  return CHARKEY(ImGuiKey_Comma);
        case 60:  return CHARKEY(ImGuiKey_Period);
        case 61:  return CHARKEY(ImGuiKey_Slash);
        case 62:  return ImGuiKey_RightShift;
        case 63:  return ImGuiKey_KeypadMultiply;
        case 64:  return ImGuiKey_LeftAlt;
        case 65:  return CHARKEY(ImGuiKey_Space);
        case 66:  return ImGuiKey_CapsLock;
        case 67:  return ImGuiKey_F1;
        case 68:  return ImGuiKey_F2;
        case 69:  return ImGuiKey_F3;
        case 70:  return ImGuiKey_F4;
        case 71:  return ImGuiKey_F5;
        case 72:  return ImGuiKey_F6;
        case 73:  return ImGuiKey_F7;
        case 74:  return ImGuiKey_F8;
        case 75:  return ImGuiKey_F9;
        case 76:  return ImGuiKey_F10;

        case 79:  return ImGuiKey_Keypad7;
        case 80:  return ImGuiKey_Keypad8;
        case 81:  return ImGuiKey_Keypad9;
        case 82:  return ImGuiKey_KeypadSubtract;
        case 83:  return ImGuiKey_Keypad4;
        case 84:  return ImGuiKey_Keypad5;
        case 85:  return ImGuiKey_Keypad6;
        case 86:  return ImGuiKey_KeypadAdd;
        case 87:  return ImGuiKey_Keypad1;
        case 88:  return ImGuiKey_Keypad2;
        case 89:  return ImGuiKey_Keypad3;
        case 90:  return ImGuiKey_Keypad0;
        case 91:  return ImGuiKey_KeypadDecimal;

        case 95:  return ImGuiKey_F11;
        case 96:  return ImGuiKey_F12;

        case 104: return ImGuiKey_KeypadEnter;
        case 105: return ImGuiKey_RightCtrl;
        case 106: return ImGuiKey_KeypadDivide;

        case 108: return ImGuiKey_RightAlt;

        case 110: return ImGuiKey_Home;
        case 111: return ImGuiKey_UpArrow;
        case 112: return ImGuiKey_PageUp;
        case 113: return ImGuiKey_LeftArrow;
        case 114: return ImGuiKey_RightArrow;
        case 115: return ImGuiKey_End;
        case 116: return ImGuiKey_DownArrow;
        case 117: return ImGuiKey_PageDown;

        case 119: return ImGuiKey_Delete;

        case 125: return ImGuiKey_KeypadEqual;

        case 133: return ImGuiKey_LeftSuper;
        case 134: return ImGuiKey_RightSuper;

        default:  break;
    }
    return ImGuiKey_None;
}

static void handle_key_event(xcb_keycode_t key_code, uint16_t state, bool down)
{
    ImGuiIO& io = ImGui::GetIO();

    // Shift (1) or CapsLock (2)
    const uint16_t shift = ((state >> 1) ^ state) & 1;
    io.AddKeyEvent(ImGuiMod_Shift, !! shift);

    // Ctrl (4)
    io.AddKeyEvent(ImGuiMod_Ctrl,  !! (state & 4));

    // Alt (8, 128)
    io.AddKeyEvent(ImGuiMod_Ctrl,  !! (state & (8 | 128)));

    const ImGuiKey key = map_key(key_code);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, down);
        io.SetKeyEventNativeData(key, key_code, -1);
    }
    else if ( ! down && xcb_keysyms &&
             (key_code >= min_keycode) && (key_code < max_keycode)) {

        const uint32_t idx = (key_code - min_keycode) * xcb_keysyms_per_code;
        const xcb_keysym_t sym = xcb_keysyms[idx + shift];
        io.AddInputCharacterUTF16(static_cast<ImWchar16>(sym));
    }
}

void handle_key_press(void* event)
{
    window_needs_update = true;

    xcb_key_press_event_t* const key_event = static_cast<xcb_key_press_event_t*>(event);

    handle_key_event(key_event->detail, key_event->state, true);

    free(event);
}

static void handle_key_release(xcb_key_release_event_t* event)
{
    handle_key_event(event->detail, event->state, false);
}

static int map_mouse_button(int button)
{
    switch (button) {
        case 1: return ImGuiPopupFlags_MouseButtonLeft;
        case 2: return ImGuiPopupFlags_MouseButtonMiddle;
        case 3: return ImGuiPopupFlags_MouseButtonRight;

        default:
            break;
    }

    return -1;
}

static void handle_button_press(xcb_button_press_event_t* event)
{
    const int button = map_mouse_button(event->detail);

    if (button >= 0 && button < ImGuiMouseButton_COUNT)
        ImGui::GetIO().AddMouseButtonEvent(button, true);
}

static void handle_button_release(xcb_button_release_event_t* event)
{
    ImGuiIO& io = ImGui::GetIO();

    const int button = map_mouse_button(event->detail);

    if (button >= 0 && button < ImGuiMouseButton_COUNT)
        io.AddMouseButtonEvent(button, false);
    else if (event->detail == 4)
        io.AddMouseWheelEvent(0, 1);
    else if (event->detail == 5)
        io.AddMouseWheelEvent(0, -1);
}

static void handle_mouse_motion(xcb_motion_notify_event_t* event)
{
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(event->event_x),
                                    static_cast<float>(event->event_y));
}

void handle_gui_event(void* event)
{
    window_needs_update = true;

    switch (static_cast<xcb_generic_event_t*>(event)->response_type & ~0x80) {

        case XCB_KEY_RELEASE: {
            handle_key_release(static_cast<xcb_key_release_event_t*>(event));
            break;
        }

        case XCB_BUTTON_PRESS: {
            handle_button_press(static_cast<xcb_button_press_event_t*>(event));
            break;
        }

        case XCB_BUTTON_RELEASE: {
            handle_button_release(static_cast<xcb_button_release_event_t*>(event));
            break;
        }

        case XCB_MOTION_NOTIFY: {
            handle_mouse_motion(static_cast<xcb_motion_notify_event_t*>(event));
            break;
        }

        case XCB_ENTER_NOTIFY:
            ImGui::GetIO().AddFocusEvent(true);
            break;

        case XCB_LEAVE_NOTIFY:
            ImGui::GetIO().AddFocusEvent(false);
            break;
    }

    free(event);
}
