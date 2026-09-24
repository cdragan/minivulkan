// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_notifications.h"
#include "../core/minivulkan.h"

#include "../core/d_printf.h"
#include "../core/gui_imgui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t notification_slots = 8;
static_assert(Sculptor::notification_state_bytes == notification_slots * sizeof(Sculptor::Notification));

Sculptor::Notification notification_pool[notification_slots];
uint32_t               num_notifications;

constexpr uint32_t notification_lifetime_ms = 6000; // milliseconds until a slot frees
constexpr uint32_t notification_fade_out_ms = 1000; // milliseconds of fading before expiry
constexpr uint32_t notification_fade_in_ms  = 200;  // milliseconds of fading after posting
constexpr float    notification_width       = 380.0f;
constexpr float    notification_spacing     = 6.0f;
constexpr float    notification_padding     = 8.0f;
constexpr float    notification_margin      = 16.0f;
constexpr float    notification_top         = 48.0f;

// printf-style forwarding: the format string arrives from the notify_* callers.
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
void post_notification(Sculptor::NotificationSeverity severity, const char* format, va_list args)
{
    if (num_notifications == notification_slots) {
        memmove(&notification_pool[0], &notification_pool[1], sizeof(notification_pool) - sizeof(notification_pool[0]));
        num_notifications--;
    }

    Sculptor::Notification& note = notification_pool[num_notifications++];
    vsnprintf(note.text, sizeof(note.text), format, args);
    note.timestamp_ms = get_current_time_ms();
    note.severity     = severity;
    d_printf("%s\n", note.text);
}
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

} // namespace

void Sculptor::notify_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    post_notification(NotificationSeverity::info, format, args);
    va_end(args);
}

void Sculptor::notify_warning(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    post_notification(NotificationSeverity::warning, format, args);
    va_end(args);
}

void Sculptor::notify_error(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    post_notification(NotificationSeverity::error, format, args);
    va_end(args);
}

void Sculptor::render_notifications()
{
    const ImGuiIO& io     = ImGui::GetIO();
    const uint64_t now_ms = get_current_time_ms();

    // Expiry frees the oldest slots first, so the expired range is always a prefix.
    uint32_t expired = 0;
    while (expired < num_notifications && now_ms - notification_pool[expired].timestamp_ms >= notification_lifetime_ms)
        expired++;
    if (expired) {
        memmove(&notification_pool[0],
                &notification_pool[expired],
                (num_notifications - expired) * sizeof(notification_pool[0]));
        num_notifications -= expired;
    }

    if (! num_notifications)
        return;

    // Severity colors, named by channel so the packed channel order of the
    // current ImGui build (IM_COL32: red in the low byte) stays explicit.
    struct SeverityColor {
        uint32_t r, g, b;
    };
    static const SeverityColor severity_color[] = {
        { 0x2E, 0x5C, 0xA6 }, // info: blue
        { 0xB2, 0x8A, 0x20 }, // warning: yellow
        { 0xB2, 0x36, 0x36 }  // error: red
    };

    const auto color = [](uint32_t r, uint32_t g, uint32_t b, uint32_t alpha) { return IM_COL32(r, g, b, alpha); };

    // Text wraps at the box width; each box hugs the wrapped text height, so
    // long messages are never clipped.
    const float pad        = notification_padding;
    const float text_width = notification_width - 2.0f * pad;
    const float text_left  = io.DisplaySize.x - notification_margin - notification_width + pad;

    ImDrawList* const draw_list = ImGui::GetForegroundDrawList();

    // Oldest first, so the stack reads top-down with the newest at the bottom.
    float y = notification_top;
    for (uint32_t i = 0; i < num_notifications; i++) {
        const Notification& note   = notification_pool[i];
        const uint64_t      age_ms = now_ms - note.timestamp_ms;

        float alpha = 1.0f;
        if (age_ms < notification_fade_in_ms)
            alpha *= static_cast<float>(age_ms) / static_cast<float>(notification_fade_in_ms);
        const uint64_t left_ms = notification_lifetime_ms - age_ms;
        if (left_ms < notification_fade_out_ms)
            alpha *= static_cast<float>(left_ms) / static_cast<float>(notification_fade_out_ms);

        const char* text = note.text;
        // ImGui measures the wrapped height and draws the wrapped text itself,
        // so the box always hugs every message and no line can be clipped.
        const float          height     = ImGui::CalcTextSize(text, nullptr, false, text_width).y + 2.0f * pad;
        const float          x0         = io.DisplaySize.x - notification_margin - notification_width;
        const SeverityColor& severity   = severity_color[static_cast<uint32_t>(note.severity)];
        const uint32_t       alpha_u    = static_cast<uint32_t>(alpha * 216.0f);
        const uint32_t       text_alpha = static_cast<uint32_t>(alpha * 255.0f);

        draw_list->AddRectFilled(ImVec2(x0, y),
                                 ImVec2(x0 + notification_width, y + height),
                                 color(severity.r, severity.g, severity.b, alpha_u));
        draw_list->AddRect(ImVec2(x0, y),
                           ImVec2(x0 + notification_width, y + height),
                           color(255, 255, 255, alpha_u / 3));

        draw_list->AddText(ImGui::GetFont(),
                           ImGui::GetFontSize(),
                           ImVec2(text_left, y + pad),
                           color(255, 255, 255, text_alpha),
                           text,
                           nullptr,
                           text_width);

        y += height + notification_spacing;
    }
}
