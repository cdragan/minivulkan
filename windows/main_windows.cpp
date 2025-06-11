// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "main_windows.h"
#include "../core/d_printf.h"
#include "../core/gui.h"
#include "../core/minivulkan.h"
#include "../core/mstdc.h"
#include "../core/realtime_synth.h"

/* TODO Just including xaudio2.h somehow calls LoadLibraryEx - figure out how to avoid that */
#include <xaudio2.h>

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

static IXAudio2*            x_audio;
static IXAudio2SourceVoice* sound_track;

bool load_sound_track(const void* data, uint32_t size)
{
    assert( ! x_audio);
    assert( ! sound_track);

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        d_printf("Failed to initialize COM\n");
        return false;
    }

    if (FAILED(XAudio2Create(&x_audio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        d_printf("Failed to initialize XAudio2\n");
        return false;
    }

    IXAudio2MasteringVoice* mastering_voice;
    if (FAILED(x_audio->CreateMasteringVoice(&mastering_voice))) {
        d_printf("Failed to create mastering voice\n");
        return false;
    }

    constexpr uint32_t num_channels = 2;

    static WAVEFORMATEX wave_format = {
        sample_format,
        num_channels,    // nChannels
        Synth::rt_sampling_rate,   // nSamplesPerSec
        Synth::rt_sampling_rate * num_channels * bits_per_sample / 8, // nAvgBytesPerSec
        num_channels * bits_per_sample / 8,                 // nBlockAlign
        bits_per_sample, // wBitsPerSample
        0                // cbSize
    };

    IXAudio2SourceVoice* source_voice;
    if (FAILED(x_audio->CreateSourceVoice(&source_voice, &wave_format))) {
        d_printf("Failed to create source voice\n");
        return false;
    }

    static XAUDIO2_BUFFER buffer = {
        0,       // Flags
        0,       // AudioBytes
        nullptr, // pAudioData
        0,       // PlayBegin
        0,       // PlayLength
        0,       // LoopBegin
        0,       // LoopLength
        0,       // LoopCount
        nullptr  // pContext
    };
    buffer.AudioBytes = size;
    buffer.pAudioData = static_cast<const BYTE*>(data);

    if (FAILED(source_voice->SubmitSourceBuffer(&buffer))) {
        d_printf("Failed to submit sound data\n");
        return false;
    }

    sound_track = source_voice;

    return true;
}

bool play_sound_track()
{
    assert(sound_track);

    if (FAILED(sound_track->Start(0))) {
        d_printf("Failed to start soundtrack\n");
        return false;
    }

    return true;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT umsg, WPARAM wparam, LPARAM lparam)
{
    if (gui_WndProcHandler(hwnd, umsg, wparam, lparam))
        return 0;

    Window* const w = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

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
            w->window = hwnd;

            if ( ! init_vulkan(w))
                break;

            gui_init(hwnd);

            return 0;
        }

        case WM_PAINT:
            gui_new_frame();

            if (skip_frame(w) && ! need_redraw(w))
                return 0;

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

    idle_queue();

    PostQuitMessage(0);
    return 0;
}

static bool create_window(Window* w)
{
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
        app_name                                // lpszClassName
    };

    w->instance = GetModuleHandle(nullptr);

    wnd_class.hInstance = w->instance;

    if ( ! RegisterClass(&wnd_class)) {
        d_printf("Failed to register window class\n");
        return false;
    }

    const bool full_screen = is_full_screen();

    DWORD ws_ex;
    DWORD ws;
    int   x;
    int   y;
    int   width;
    int   height;

    if (full_screen) {

        const HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(),
                                                   MONITOR_DEFAULTTOPRIMARY);

        static MONITORINFO monitor_info = { sizeof(MONITORINFO) };
        if ( ! GetMonitorInfo(monitor, &monitor_info)) {
            d_printf("Failed to get current video mode\n");
            return false;
        }

        ws_ex  = WS_EX_APPWINDOW | WS_EX_TOPMOST;
        ws     = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_POPUP;
        x      = static_cast<int>(monitor_info.rcMonitor.left);
        y      = static_cast<int>(monitor_info.rcMonitor.top);
        width  = static_cast<int>(monitor_info.rcMonitor.right  - monitor_info.rcMonitor.left);
        height = static_cast<int>(monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top);
    }
    else {
        ws_ex  = WS_EX_APPWINDOW;
        ws     = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
        x      = CW_USEDEFAULT;
        y      = CW_USEDEFAULT;
        width  = get_main_window_width();
        height = get_main_window_height();
    }

    const HWND hwnd = CreateWindowEx(ws_ex,       // dwExStyle
                                     app_name,    // lpClassName
                                     app_name,    // lpWindowName
                                     ws,          // dwStyle
                                     x,           // X
                                     y,           // Y
                                     width,       // nWidth
                                     height,      // nHeight
                                     nullptr,     // hWndParent
                                     nullptr,     // hMenu
                                     w->instance, // hInstance
                                     w);          // lpParam

    if ( ! hwnd) {
        d_printf("Failed to create window\n");
        return false;
    }

    d_printf("Created window %ux%u at [%u, %u]\n", width, height, x, y);

    if (full_screen)
        ShowCursor(FALSE);

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

int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE hprev_instance, PSTR cmd_line, INT cmd_show)
{
#ifndef NDEBUG
    AttachConsole((DWORD)-1);
#endif

    Window w = { };

    if ( ! create_window(&w))
        return 1;

    return event_loop(&w);
}
