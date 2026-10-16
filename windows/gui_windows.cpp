// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "main_windows.h"

#include "../core/minivulkan.h"
#include "../thirdparty/imgui/src/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT gui_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Redraw whenever receiving any type of message
    RECT rc = { };
    GetClientRect(hWnd, &rc);
    InvalidateRect(hWnd, &rc, FALSE);

    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

bool need_redraw(struct Window* w)
{
    RECT rc = { };
    if ( ! GetUpdateRect(w->window, &rc, FALSE))
        return false;

    // Validate window (we will draw it in draw_frame())
    ValidateRect(w->window, &rc);
    return true;
}

static void CALLBACK redraw_timer_proc(HWND, UINT, UINT_PTR, DWORD)
{
    gui_new_frame();

    if (skip_frame(nullptr))
        return;

    draw_frame();
}

void gui_init(HWND hwnd)
{
    ImGui_ImplWin32_Init(hwnd);

    constexpr UINT redraw_timeout_ms = 16;

    SetTimer(nullptr, 1, redraw_timeout_ms, redraw_timer_proc);
}

void gui_new_frame()
{
    ImGui_ImplWin32_NewFrame();
}
