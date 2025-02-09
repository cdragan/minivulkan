// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "main_linux.h"

#include <time.h>

uint64_t get_current_time_ms()
{
    uint64_t time_ms = 0;

    struct timespec ts;

    if ( ! clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
        time_ms =  static_cast<uint64_t>(ts.tv_sec) * 1000;
        time_ms += static_cast<uint64_t>(ts.tv_nsec) / 1'000'000;
    }

    return time_ms;
}

bool load_sound_track(const void* data, uint32_t size)
{
    // TODO
    return true;
}

bool play_sound_track()
{
    // TODO
    return true;
}
