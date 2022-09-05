// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

LRESULT gui_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void gui_init(HWND hwnd);
void gui_new_frame();
