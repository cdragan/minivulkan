// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "main_windows.h"

#include "thirdparty/imgui/src/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT gui_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

bool need_redraw(struct Window* w)
{
    RECT rc = { };
    if ( ! GetUpdateRect(w->window, &rc, FALSE))
        return false;

    // Validate window (we will draw it in draw_frame())
    // TODO not reacting to window resize or to mouse/key events!
    //ValidateRect(w->window, &rc);
    return true;
}

void gui_init(HWND hwnd)
{
    ImGui_ImplWin32_Init(hwnd);
}

void gui_new_frame()
{
    ImGui_ImplWin32_NewFrame();
}
