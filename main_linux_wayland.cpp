// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "d_printf.h"
#include "gui.h"
#include "main_linux.h"
#include "minivulkan.h"

#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <wayland-client-core.h>
#include <wayland-client.h>
#include "linux/xdg-shell.h" // generated file

struct Window {
    wl_display*    display;
    wl_surface*    surface;
    wl_compositor* compositor;
    wl_seat*       seat;
    wl_shm*        shm;
    wl_shm_pool*   pool;
    wl_pointer*    pointer;
    wl_keyboard*   keyboard;
    xdg_wm_base*   wm_base;
    uint32_t       width;
    uint32_t       height;
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
    d_printf("Created Wayland surface, for window %ux%u\n", w->width, w->height);
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
        { "wl_compositor", &wl_compositor_interface, offsetof(Window, compositor) },
        { "wl_seat",       &wl_seat_interface,       offsetof(Window, seat)       },
        { "wl_shm",        &wl_shm_interface,        offsetof(Window, shm)        },
        { "xdg_wm_base",   &xdg_wm_base_interface,   offsetof(Window, wm_base)    },
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

static wl_buffer* create_buffer(Window* w, uint32_t width, uint32_t height)
{
    return wl_shm_pool_create_buffer(w->pool,
                                     0,
                                     width,
                                     height,
                                     width * 4,
                                     WL_SHM_FORMAT_ABGR8888);
}

static void handle_wm_ping(void* data, xdg_wm_base* wm_base, uint32_t serial)
{
    xdg_wm_base_pong(static_cast<Window*>(data)->wm_base, serial);
}

static void handle_wm_configure(void* data, xdg_surface* wm_surface, uint32_t serial)
{
    xdg_surface_ack_configure(wm_surface, serial);
}

constexpr uint32_t max_width  = 800;
constexpr uint32_t max_height = 600;

static void handle_toplevel_configure(void*         data,
                                      xdg_toplevel* toplevel,
                                      int32_t       width,
                                      int32_t       height,
                                      wl_array*     states)
{
    d_printf("window resize %dx%d\n", width, height);

    Window* const w = static_cast<Window*>(data);

    if (width > 0 && height > 0) {
        w->width  = width;
        w->height = height;
    }

#if 0
    wl_buffer* const buffer = create_buffer(w, width, height);

    wl_surface_attach(w->surface, buffer, 0, 0);
    wl_surface_commit(w->surface);
#endif
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
        d_printf("Failed to Wayland seat\n");
        return false;
    }
    if ( ! w->shm) {
        d_printf("Failed to Wayland shm\n");
        return false;
    }
    if ( ! w->wm_base) {
        d_printf("Failed to get window manager base\n");
        return false;
    }

    // Create shared memory objects for Wayland compositor
    constexpr uint32_t capacity = max_width * max_height * 4;
    const int fd = memfd_create(app_name, MFD_CLOEXEC);
    if (fd == -1) {
        d_printf("Failed to create shared memory file\n");
        return false;
    }
    if (ftruncate(fd, capacity)) {
        d_printf("Failed to resize shared memory file\n");
        return false;
    }
    void* const memory = mmap(nullptr, capacity, PROT_READ, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        d_printf("Failed to map shared memory file\n");
        return false;
    }
    w->pool = wl_shm_create_pool(w->shm, fd, capacity);
    if ( ! w->pool) {
        d_printf("Failed to create Wayland shared memory pool\n");
        return false;
    }

    // Create compositor surface
    w->surface = wl_compositor_create_surface(w->compositor);
    if ( ! w->surface) {
        d_printf("Failed to create wayland surface\n");
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
        d_printf("Failed to create toplevel window\n");
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
    else
        xdg_surface_set_window_geometry(wm_surface,
                                        0,
                                        0,
                                        get_main_window_width(),
                                        get_main_window_height());

    // Apply changes to the surface
    wl_surface_commit(w->surface);

    // Receive configuration request
    wl_display_roundtrip(w->display);

    // Complete configuration
    wl_surface_commit(w->surface);

    //return install_keyboard_events(w->connection);
    return true;
}

static void frame_done(void* data, wl_callback* callback, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done
};

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
