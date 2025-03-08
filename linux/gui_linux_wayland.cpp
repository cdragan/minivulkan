// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "d_printf.h"
#include "main_linux.h"

#include "thirdparty/imgui/src/imgui.h"

static bool window_needs_update = false;

bool need_redraw(struct Window*)
{
    const bool needs_update = window_needs_update;

    window_needs_update = false;

    return needs_update;
}
