// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "d_printf.h"
#include "gui.h"
#include "main_linux.h"
#include "minivulkan.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
//#include <wayland-client-protocol.h>
#include <wayland-client-core.h>
#include <wayland-client.h>
#include "linux/xdg-shell.h" // generated file

struct Window {
    bool              quit;
    wl_display*       display;
    wl_surface*       surface;
    wl_compositor*    compositor;

    // Windowed
    wl_shell*         shell;
    wl_shell_surface* shell_surface;

    // Fullscreen
    xdg_wm_base*      wm_base;
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

static void registry_handler(void        *data,
                             wl_registry *registry,
                             uint32_t     id,
                             const char  *interface,
                             uint32_t     version)
{
    Window* const w = static_cast<Window*>(data);

    if ( ! strcmp(interface, "wl_compositor")) {
        w->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, id, &wl_compositor_interface, 1));
    }
    else if ( ! is_full_screen() && ! strcmp(interface, "wl_shell")) {
        w->shell = static_cast<wl_shell*>(
            wl_registry_bind(registry, id, &wl_shell_interface, 1));
    }
    else if (is_full_screen() && ! strcmp(interface, "xdg_wm_base")) {
        w->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, id, &xdg_wm_base_interface, 1));
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

    // Check compositor
    if ( ! w->compositor) {
        d_printf("Wayland compositor is not available\n");
        return false;
    }

    // Create compositor surface
    w->surface = wl_compositor_create_surface(w->compositor);
    if ( ! w->surface) {
        d_printf("Failed to create wayland surface\n");
        return false;
    }

    // Create shell surface
    if ( ! full_screen) {
        if ( ! w->shell) {
            d_printf("Wayland shell is not available\n");
            return false;
        }

        w->shell_surface = wl_shell_get_shell_surface(w->shell, w->surface);
        if ( ! w->shell_surface) {
            d_printf("Failed to get Wayland shell surface\n");
            return false;
        }

        wl_shell_surface_set_toplevel(w->shell_surface);
    }

    // Create fullscreen window
    if (full_screen) {
        if ( ! w->wm_base) {
            d_printf("Failed to get window manager base\n");
            return false;
        }

        static const xdg_wm_base_listener wm_base_listener = {
            .ping = handle_wm_ping,
        };

        xdg_wm_base_add_listener(w->wm_base, &wm_base_listener, w);

        xdg_surface*  wm_surface = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
        xdg_toplevel* toplevel   = xdg_surface_get_toplevel(wm_surface);
        xdg_toplevel_set_fullscreen(toplevel, nullptr);
    }

    wl_surface_commit(w->surface);
    wl_display_roundtrip(w->display);

    //return install_keyboard_events(w->connection);
    return true;
}

static void frame_done(void* data, wl_callback* callback, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

static void frame_done(void* data, wl_callback* callback, uint32_t time)
{
    // Destroy the previous callback
    wl_callback_destroy(callback);

    // Trigger rendering
    Window* const w = static_cast<Window*>(data);
    if (need_redraw(w) || ! skip_frame(w)) {
        if ( ! draw_frame()) {

            // Quit on error
            w->quit = true;
            return;
        }
    }

    // Register a new frame callback
    wl_callback_add_listener(wl_surface_frame(w->surface), &frame_listener, w);
}

static int event_loop(Window* w)
{
    wl_callback_add_listener(wl_surface_frame(w->surface), &frame_listener, w);

    while ( ! w->quit && wl_display_dispatch(w->display) != -1) {
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
