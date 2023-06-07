// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#define SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_surface,                   REQUIRED)

#define SUPPORTED_DEVICE_EXTENSIONS_BASE \
    X(VK_KHR_swapchain,                 REQUIRED) \
    X(VK_KHR_dynamic_rendering,         REQUIRED) \
    X(VK_KHR_8bit_storage,              REQUIRED)

#ifdef __APPLE__
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_EXT_metal_surface,             REQUIRED) \
    X(VK_KHR_portability_enumeration,   OPTIONAL)

#   define SUPPORTED_DEVICE_EXTENSIONS SUPPORTED_DEVICE_EXTENSIONS_BASE \
    X(VK_KHR_portability_subset,        OPTIONAL)
#endif

#ifdef __linux__
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_xcb_surface,               REQUIRED)

#   define SUPPORTED_DEVICE_EXTENSIONS SUPPORTED_DEVICE_EXTENSIONS_BASE
#endif

#ifdef _WIN32
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_win32_surface,             REQUIRED) \
    X(VK_KHR_get_surface_capabilities2, REQUIRED)

#   define SUPPORTED_DEVICE_EXTENSIONS SUPPORTED_DEVICE_EXTENSIONS_BASE \
    X(VK_EXT_full_screen_exclusive,     REQUIRED)
#endif
