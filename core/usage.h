// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#pragma once

#include <stdint.h>

enum class Usage {
    // Constant resources created on host and transferred to device, e.g. textures, vertex buffers, etc.
    fixed,
    // Frequently changing resources, e.g. uniform buffers
    dynamic,
    // Resources initialized and used on the device, never accessed on the host
    device_only,
    // Resources used temporarily on the device during frame generation, e.g. G-buffers
    transient,
    // Resources allocated on the host, device can still transfer to/from these resources
    host_only
};

struct Description {
#ifdef NDEBUG
    constexpr Description(const char*, uint32_t) { }
    constexpr Description(const char*) { }
#else
    constexpr Description(const char* dbg_name, uint32_t dbg_idx)
        : name(dbg_name), idx(dbg_idx) { }
    constexpr Description(const char* dbg_name)
        : name(dbg_name) { }

    const char* name = nullptr;
    uint32_t    idx  = ~0U;
#endif
};
