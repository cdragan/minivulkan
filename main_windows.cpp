// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include "minivulkan.h"
#include "mstdc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct Window {
    HINSTANCE instance;
    HWND      window;
};

bool create_surface(Window* w)
{
    static VkWin32SurfaceCreateInfoKHR surf_create_info = {
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        nullptr,
        0,  // flags
        0,  // hinstance
        0   // hwnd
    };

    surf_create_info.hinstance = w->instance;
    surf_create_info.hwnd      = w->window;

    const VkResult res = CHK(vkCreateWin32SurfaceKHR(vk_instance,
                                                     &surf_create_info,
                                                     nullptr,
                                                     &vk_surface));
    return res == VK_SUCCESS;
}

uint64_t get_current_time_ms()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    uint64_t time_100ns = static_cast<uint64_t>(ft.dwLowDateTime);
    time_100ns += static_cast<uint64_t>(ft.dwHighDateTime) << 32;

    return time_100ns / 10'000;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
    switch (umsg) {

        case WM_NCCREATE:
            {
                const LPVOID w = reinterpret_cast<CREATESTRUCT*>(lparam)->lpCreateParams;
                SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
            }
            [[fallthrough]];

        default:
            return DefWindowProc(hwnd, umsg, wparam, lparam);

        case WM_CREATE: {
            Window* const w = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

            w->window = hwnd;

            if ( ! init_vulkan(w))
                break;

            return 0;
        }

        case WM_PAINT:
            if ( ! draw_frame())
                break;

            return 0;

        case WM_DESTROY:
            break;

        case WM_CHAR:
            if (wparam == VK_ESCAPE)
                break;
            return 0;
    }

    PostQuitMessage(0);
    return 0;
}

static bool create_window(Window* w)
{
    static const char title[] = APPNAME;

    static WNDCLASS wnd_class = {
        CS_HREDRAW | CS_VREDRAW | CS_OWNDC,     // style
        window_proc,                            // lpfnWndProc
        0,                                      // cbClsExtra
        0,                                      // cbWndExtra
        0,                                      // hInstance
        0,                                      // hIcon
        0,                                      // hCursor
        0,                                      // hbrBackground
        nullptr,                                // lpszMenuName
        title                                   // lpszClassName
    };

    w->instance = GetModuleHandle(nullptr);

    wnd_class.hInstance = w->instance;

    if ( ! RegisterClass(&wnd_class)) {
        //dprintf("Failed to register window class\n");
        return false;
    }

    const HWND hwnd = CreateWindowEx(WS_EX_APPWINDOW,       // dwExStyle
                                     title,                 // lpClassName
                                     title,                 // lpWindowName
                                     WS_OVERLAPPEDWINDOW,   // dwStyle
                                     CW_USEDEFAULT,         // X
                                     CW_USEDEFAULT,         // Y
                                     800,                   // nWidth
                                     600,                   // nHeight
                                     nullptr,               // hWndParent
                                     nullptr,               // hMenu
                                     w->instance,           // hInstance
                                     w);                    // lpParam

    if ( ! hwnd) {
        //dprintf("Failed to create window\n");
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);

    return true;
}

static int event_loop(Window* w)
{
    MSG msg;

    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

#ifdef NOSTDLIB
int __stdcall WinMainCRTStartup()
#else
int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE hprev_instance, PSTR cmd_line, INT cmd_show)
#endif
{
    Window w = { };

    if ( ! create_window(&w))
        return 1;

    const int ret = event_loop(&w);

#ifdef NOSTDLIB
    ExitProcess(ret);
#endif

    return ret;
}
