// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"

#include <assert.h>
#include <stdarg.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void d_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    char buf[1024];
    const size_t chars = static_cast<size_t>(vsnprintf(buf, sizeof(buf), format, args));

    va_end(args);

    assert(chars < sizeof(buf));

    static HANDLE out = INVALID_HANDLE_VALUE;
    if (out == INVALID_HANDLE_VALUE)
    {
        out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (out != INVALID_HANDLE_VALUE)
    {
        DWORD numWritten = 0;
        WriteFile(out, buf, static_cast<DWORD>(chars), &numWritten, nullptr);
    }
}
