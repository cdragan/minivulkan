// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "main_windows.h"

#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT gui_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
}

void gui_init(HWND hwnd)
{
    ImGui_ImplWin32_Init(hwnd);
}

void gui_new_frame()
{
    ImGui_ImplWin32_NewFrame();
}
