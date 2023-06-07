// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#pragma once

enum class Usage {
    // Constant resources created on host and transferred to device, e.g. textures, vertex buffers, etc.
    fixed,
    // Frequently changing resources, e.g. uniform buffers
    dynamic,
    // Resources initialized and used on the device, never accessed on the host
    device_only,
    // Resources allocated on the host
    host_only,
    // Resources used on the device, which are occasionally purged, e.g. depth buffers
    device_temporary
};
