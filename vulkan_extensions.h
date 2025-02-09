// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#define SUPPORTED_INSTANCE_EXTENSIONS_BASE_RELEASE \
    X(VK_KHR_surface,                   REQUIRED)

#ifdef NDEBUG
#define SUPPORTED_INSTANCE_EXTENSIONS_BASE_DEBUG
#else
#define SUPPORTED_INSTANCE_EXTENSIONS_BASE_DEBUG \
    X(VK_EXT_debug_utils,               OPTIONAL)
#endif

#define SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    SUPPORTED_INSTANCE_EXTENSIONS_BASE_RELEASE \
    SUPPORTED_INSTANCE_EXTENSIONS_BASE_DEBUG

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

#if defined(__linux__) && defined(LINUX_USE_XCB)
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_xcb_surface,               REQUIRED)
#endif

#if defined(__linux__) && !defined(LINUX_USE_XCB)
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_wayland_surface,           REQUIRED)
#endif

#ifdef __linux__
#   define SUPPORTED_DEVICE_EXTENSIONS SUPPORTED_DEVICE_EXTENSIONS_BASE
#endif

#ifdef _WIN32
#   define SUPPORTED_INSTANCE_EXTENSIONS SUPPORTED_INSTANCE_EXTENSIONS_BASE \
    X(VK_KHR_win32_surface,             REQUIRED) \
    X(VK_KHR_get_surface_capabilities2, REQUIRED)

#   define SUPPORTED_DEVICE_EXTENSIONS SUPPORTED_DEVICE_EXTENSIONS_BASE \
    X(VK_EXT_full_screen_exclusive,     REQUIRED)
#endif
