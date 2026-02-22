// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct Window {
    HINSTANCE instance;
    HWND      window;
};

LRESULT gui_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void gui_init(HWND hwnd);
void gui_new_frame();
