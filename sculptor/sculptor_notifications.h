// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>

#ifdef __GNUC__
#    define printf_format(pos) [[gnu::format(printf, pos, pos + 1)]]
#else
#    define printf_format(pos)
#endif

namespace Sculptor {

enum class NotificationSeverity : uint8_t {
    info,
    warning,
    error
};

struct Notification {
    char                 text[256];
    uint64_t             timestamp_ms; // monotonic milliseconds when posted
    NotificationSeverity severity;
    bool                 sticky;
};

// Byte budget the editor state reservation accounts for the notification
// pool; the pool size is pinned to it in sculptor_notifications.cpp.
constexpr uint32_t notification_state_bytes = 8 * sizeof(Notification);

printf_format(1) void notify_info(const char* format, ...);
printf_format(1) void notify_warning(const char* format, ...);
printf_format(1) void notify_error(const char* format, ...);
printf_format(2) void notify_sticky(NotificationSeverity severity, const char* format...);

// The attribute lives on the declarations; the macro name is not part of the
// surface.
#undef printf_format

void render_notifications();

} // namespace Sculptor
