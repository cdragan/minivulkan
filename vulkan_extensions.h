// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#define SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_surface,               REQUIRED)

#ifdef __APPLE__
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_EXT_metal_surface,         REQUIRED)
#endif

#ifdef __linux__
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_xcb_surface,           REQUIRED)
#endif

#ifdef _WIN32
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_win32_surface,         REQUIRED)
    X(VK_EXT_full_screen_exclusive, REQUIRED)
#endif

#define SUPPORTED_DEVICE_EXTENSIONS \
    X(VK_KHR_portability_subset,    OPTIONAL) \
    X(VK_KHR_swapchain,             REQUIRED)
