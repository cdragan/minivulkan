// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "../core/d_printf.h"
#include "../core/gui.h"
#include "main_linux.h"
#include "../core/minivulkan.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <wayland-client-core.h>
#include <wayland-client.h>
#include "xdg-shell.h" // generated file

struct Window {
    wl_display*    display;
    wl_surface*    surface;
    wl_compositor* compositor;
    wl_seat*       seat;
    wl_pointer*    pointer;
    wl_keyboard*   keyboard;
    xdg_wm_base*   wm_base;
    bool           quit;
};

bool create_surface(struct Window* w)
{
    static VkWaylandSurfaceCreateInfoKHR surf_create_info = {
        VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        nullptr,
        0,       // flags
        nullptr, // display
        nullptr  // surface
    };

    surf_create_info.display = w->display;
    surf_create_info.surface = w->surface;

    const VkResult res = CHK(vkCreateWaylandSurfaceKHR(vk_instance,
                                                       &surf_create_info,
                                                       nullptr,
                                                       &vk_surface));
    return res == VK_SUCCESS;
}

struct WaylandInterfaceMapping {
    const char*         name;
    const wl_interface* iface;
    size_t              ptr_offset;
};

static void registry_handler(void        *data,
                             wl_registry *registry,
                             uint32_t     id,
                             const char  *interface,
                             uint32_t     version)
{
    static const WaylandInterfaceMapping interfaces[] = {
        { "wl_compositor", &wl_compositor_interface, offsetof(Window, compositor) }, // Wayland compositor
        { "wl_seat",       &wl_seat_interface,       offsetof(Window, seat)       }, // Input devices
        { "xdg_wm_base",   &xdg_wm_base_interface,   offsetof(Window, wm_base)    }, // Window manager
        { nullptr,         nullptr,                  0                            }
    };

    for (const WaylandInterfaceMapping* mapping = interfaces; mapping->name; mapping++) {
        if ( ! strcmp(mapping->name, interface)) {
            void** const ptr_ptr = reinterpret_cast<void**>(
                static_cast<char*>(data) + mapping->ptr_offset);

            *ptr_ptr = wl_registry_bind(registry, id, mapping->iface, 1);

            break;
        }
    }
}

static void registry_remover(void        *data,
                             wl_registry *registry,
                             uint32_t     id)
{
}

static void handle_wm_ping(void* data, xdg_wm_base* wm_base, uint32_t serial)
{
    xdg_wm_base_pong(static_cast<Window*>(data)->wm_base, serial);
}

static void handle_wm_configure(void* data, xdg_surface* wm_surface, uint32_t serial)
{
    xdg_surface_ack_configure(wm_surface, serial);
}

static void handle_toplevel_configure(void*         data,
                                      xdg_toplevel* toplevel,
                                      int32_t       width,
                                      int32_t       height,
                                      wl_array*     states)
{
    d_printf("Window resize %dx%d\n", width, height);

    if (width > 0 && height > 0) {
        vk_window_extent.width  = static_cast<uint32_t>(width);
        vk_window_extent.height = static_cast<uint32_t>(height);
    }
    else {
        vk_window_extent.width  = get_main_window_width();
        vk_window_extent.height = get_main_window_height();
    }
}

static void handle_toplevel_close(void*         data,
                                  xdg_toplevel* toplevel)
{
    static_cast<Window*>(data)->quit = true;
}

static bool create_window(Window* w)
{
    const bool full_screen = is_full_screen();

    w->display = wl_display_connect(nullptr);

    if ( ! w->display) {
        d_printf("Failed to connect to wayland display\n");
        return false;
    }

    // Initialize registry
    static const wl_registry_listener registry_listener = {
        .global        = registry_handler,
        .global_remove = registry_remover
    };
    wl_registry_add_listener(wl_display_get_registry(w->display), &registry_listener, w);
    wl_display_roundtrip(w->display);

    // Check interfaces
    if ( ! w->compositor) {
        d_printf("Wayland compositor is not available\n");
        return false;
    }
    if ( ! w->seat) {
        d_printf("Wayland input devices are not available\n");
        return false;
    }
    if ( ! w->wm_base) {
        d_printf("Wayland window manager is not available\n");
        return false;
    }

    // Create compositor surface
    w->surface = wl_compositor_create_surface(w->compositor);
    if ( ! w->surface) {
        d_printf("Failed to create Wayland compositor surface\n");
        return false;
    }

    static const xdg_wm_base_listener wm_base_listener = {
        .ping = handle_wm_ping,
    };

    xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);

    xdg_surface* wm_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);

    if ( ! wm_surface) {
        d_printf("Failed to create window manager's surface\n");
        return false;
    }

    static const xdg_surface_listener surface_listener = {
        .configure = handle_wm_configure
    };

    xdg_surface_add_listener(wm_surface, &surface_listener, w);

    xdg_toplevel* toplevel = xdg_surface_get_toplevel(wm_surface);

    if ( ! toplevel) {
        d_printf("Failed to create window (toplevel)\n");
        return false;
    }

    static const xdg_toplevel_listener toplevel_listener = {
        .configure = handle_toplevel_configure,
        .close     = handle_toplevel_close
    };

    xdg_toplevel_add_listener(toplevel, &toplevel_listener, w);

    xdg_toplevel_set_title(toplevel, app_name);
    xdg_toplevel_set_app_id(toplevel, app_name);

    if (full_screen)
        xdg_toplevel_set_fullscreen(toplevel, nullptr);

    // Apply changes to the surface
    wl_surface_commit(w->surface);

    // Receive configuration request
    wl_display_roundtrip(w->display);

    // Complete configuration
    wl_surface_commit(w->surface);

    //return install_keyboard_events(w->connection);
    return true;
}

static int event_loop(Window* w)
{
    while ( ! w->quit && wl_display_dispatch_pending(w->display) != -1) {
        if ( ! need_redraw(w) && skip_frame(w))
            continue;

        if ( ! draw_frame())
            return 1;
    }

    idle_queue();

    return 0;
}

int main()
{
    static Window w = { };

    if ( ! create_window(&w))
        return 1;

    if ( ! init_vulkan(&w))
        return 1;

    return event_loop(&w);
}
