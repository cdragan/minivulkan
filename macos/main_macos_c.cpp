// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// This version of main() uses pure C API to create window and instantiate
// Vulkan surface, without Objective-C.  This helps us reduce executable size.

#include "objc_msgsend.h"
#include "../core/gui.h"
#include "../core/minivulkan.h"
#include "../core/d_printf.h"

#include <time.h>

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 150000
#   include <CoreVideo/CoreVideo.h>
#endif

// AppKit/Foundation enum values, hardcoded because their headers are Objective-C.
static constexpr unsigned long style_mask_titled         = 1ul << 0;
static constexpr unsigned long style_mask_closable       = 1ul << 1;
static constexpr unsigned long style_mask_miniaturizable = 1ul << 2;
static constexpr unsigned long style_mask_resizable      = 1ul << 3;
static constexpr unsigned long backing_store_buffered    = 2;
static constexpr unsigned long full_screen_primary       = 1ul << 7;
static constexpr unsigned long view_width_sizable        = 1ul << 1;
static constexpr unsigned long view_height_sizable       = 1ul << 4;

static constexpr unsigned long mod_flag_shift   = 1ul << 17;
static constexpr unsigned long mod_flag_control = 1ul << 18;
static constexpr unsigned long mod_flag_option  = 1ul << 19;
static constexpr unsigned long mod_flag_command = 1ul << 20;

static constexpr unsigned short key_esc = 53;
static constexpr unsigned short key_q   = 12;

static constexpr BOOL bool_yes = static_cast<BOOL>(1);
static constexpr BOOL bool_no  = static_cast<BOOL>(0);

static id app;
static id app_delegate;
static id main_window;
static id main_view;
static id metal_layer;

struct Window {
    id layer;
};

bool create_surface(struct Window* w)
{
    static VkMetalSurfaceCreateInfoEXT surf_create_info = {
        VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
    };

    surf_create_info.pLayer = reinterpret_cast<const CAMetalLayer*>(w->layer);

    const VkResult res = CHK(vkCreateMetalSurfaceEXT(vk_instance,
                                                     &surf_create_info,
                                                     nullptr,
                                                     &vk_surface));
    return res == VK_SUCCESS;
}

uint64_t get_current_time_ms()
{
    uint64_t time_ms = 0;

    struct timespec ts;

    if ( ! clock_gettime(CLOCK_UPTIME_RAW, &ts)) {
        time_ms =  (uint64_t)ts.tv_sec * 1000;
        time_ms += (uint64_t)ts.tv_nsec / 1000000;
    }

    return time_ms;
}

static void draw_callback()
{
    if ( ! need_redraw(nullptr) && skip_frame(nullptr))
        return;

    if ( ! draw_frame())
        send_message<void>(app, sel_registerName("terminate:"), static_cast<id>(nullptr));
}

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 150000
static void display_link_fired(id self, SEL cmd, id display_link)
{
    draw_callback();
}
#else
static CVReturn cv_display_link_callback(CVDisplayLinkRef   display_link,
                                         const CVTimeStamp* now,
                                         const CVTimeStamp* output_time,
                                         CVOptionFlags      flags_in,
                                         CVOptionFlags*     flags_out,
                                         void*              target)
{
    draw_callback();
    return kCVReturnSuccess;
}

static CVDisplayLinkRef cv_display_link;
#endif

static BOOL view_perform_key_equivalent(id self, SEL cmd, id event)
{
    const unsigned long mods = mod_flag_shift | mod_flag_control | mod_flag_option | mod_flag_command;

    const unsigned short key      = send_message<unsigned short>(event, sel_registerName("keyCode"));
    const unsigned long  modifier = send_message<unsigned long>(event, sel_registerName("modifierFlags")) & mods;

    if ((key == key_esc) || (key == key_q && modifier == mod_flag_command))
        send_message<void>(app, sel_registerName("terminate:"), static_cast<id>(nullptr));

    return bool_yes;
}

static id make_string(const char* const str)
{
    return send_message<id>(get_class("NSString"), sel_registerName("stringWithUTF8String:"), str);
}

static void create_menu()
{
    const id main_menu = send_message<id>(get_class("NSMenu"), sel_registerName("new"));
    send_message<void>(app, sel_registerName("setMainMenu:"), main_menu);

    const id app_item = send_message<id>(get_class("NSMenuItem"), sel_registerName("new"));
    send_message<void>(main_menu, sel_registerName("addItem:"), app_item);

    const id app_menu = send_message<id>(get_class("NSMenu"), sel_registerName("new"));
    send_message<void>(app_item, sel_registerName("setSubmenu:"), app_menu);

    const id quit_title = make_string("Quit");
    const id quit_key   = make_string("q");
    const id quit_item  = send_message<id>(send_message<id>(get_class("NSMenuItem"), sel_registerName("alloc")),
                                           sel_registerName("initWithTitle:action:keyEquivalent:"),
                                           quit_title,
                                           sel_registerName("terminate:"),
                                           quit_key);
    send_message<void>(app_menu, sel_registerName("addItem:"), quit_item);
}

static void application_did_finish_launching(id self, SEL cmd, id notification)
{
    const bool full_screen = is_full_screen();

    const id     screen       = send_message<id>(get_class("NSScreen"), sel_registerName("mainScreen"));
    const CGRect screen_frame = send_message_struct<CGRect>(screen, sel_registerName("frame"));
    CGRect       frame_rect   = CGRectMake(0, 0, screen_frame.size.width, screen_frame.size.height);
    if ( ! full_screen)
        frame_rect = CGRectMake(0, 0, get_main_window_width(), get_main_window_height());

    const unsigned long style_mask = style_mask_titled
                                    | style_mask_closable
                                    | style_mask_miniaturizable
                                    | style_mask_resizable;

    main_window = send_message<id>(send_message<id>(get_class("NSWindow"), sel_registerName("alloc")),
                                   sel_registerName("initWithContentRect:styleMask:backing:defer:"),
                                   frame_rect,
                                   style_mask,
                                   backing_store_buffered,
                                   bool_no);
    send_message<void>(main_window, sel_registerName("center"));
    send_message<void>(main_window, sel_registerName("makeKeyAndOrderFront:"), static_cast<id>(nullptr));

    const id title = make_string(app_name);
    send_message<void>(main_window, sel_registerName("setTitle:"), title);
    send_message<void>(main_window, sel_registerName("setMinSize:"), CGSizeMake(512, 384));

    const id view_class = reinterpret_cast<id>(objc_getClass("MVView"));
    main_view = send_message<id>(send_message<id>(view_class, sel_registerName("alloc")),
                                 sel_registerName("initWithFrame:"),
                                 frame_rect);
    send_message<void>(main_view, sel_registerName("setAutoresizingMask:"), view_width_sizable | view_height_sizable);

    metal_layer = send_message<id>(get_class("CAMetalLayer"), sel_registerName("layer"));

    // Avoid UI scaling in full screen mode, assuming that in this mode we don't
    // need cursor interaction (otherwise cursor position would need to be scaled properly).
    if ( ! full_screen) {
        const double scale = send_message<double>(main_window, sel_registerName("backingScaleFactor"));
        send_message<void>(metal_layer, sel_registerName("setContentsScale:"), scale);
    }

    send_message<void>(main_view, sel_registerName("setLayer:"), metal_layer);
    send_message<void>(main_view, sel_registerName("setWantsLayer:"), bool_yes);
    send_message<void>(main_window, sel_registerName("setContentView:"), main_view);

    struct Window win;
    win.layer = metal_layer;
    if ( ! init_vulkan(&win)) {
        send_message<void>(app, sel_registerName("terminate:"), static_cast<id>(nullptr));
        return;
    }

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 150000
    const id display_link = send_message<id>(main_view,
                                             sel_registerName("displayLinkWithTarget:selector:"),
                                             app_delegate,
                                             sel_registerName("displayLinkFired:"));
    const id run_loop = send_message<id>(get_class("NSRunLoop"), sel_registerName("mainRunLoop"));
    extern id NSRunLoopCommonModes;
    send_message<void>(display_link, sel_registerName("addToRunLoop:forMode:"), run_loop, NSRunLoopCommonModes);
#else
    CVDisplayLinkCreateWithActiveCGDisplays(&cv_display_link);
    CVDisplayLinkSetOutputCallback(cv_display_link, &cv_display_link_callback, main_view);
    CVDisplayLinkStart(cv_display_link);
#endif

    if (full_screen) {
        send_message<void>(get_class("NSCursor"), sel_registerName("hide"));
        const id black = send_message<id>(get_class("NSColor"), sel_registerName("blackColor"));
        send_message<void>(main_window, sel_registerName("setBackgroundColor:"), black);
        send_message<void>(main_window, sel_registerName("setCollectionBehavior:"), full_screen_primary);
        send_message<void>(main_window, sel_registerName("setFrame:display:"), screen_frame, bool_yes);
        send_message<void>(main_window, sel_registerName("toggleFullScreen:"), app_delegate);
    }
    else
        send_message<void>(app, sel_registerName("activateIgnoringOtherApps:"), bool_yes);
}

static void application_will_terminate(id self, SEL cmd, id notification)
{
    deinit_vulkan();
}

static BOOL application_should_terminate_after_last_window_closed(id self, SEL cmd, id sender)
{
    return bool_yes;
}

static void register_classes()
{
    const Class ns_object = objc_getClass("NSObject");
    const Class delegate_class = objc_allocateClassPair(ns_object, "MVAppDelegate", 0);
    class_addMethod(delegate_class,
                    sel_registerName("applicationDidFinishLaunching:"),
                    reinterpret_cast<IMP>(application_did_finish_launching),
                    "v@:@");
    class_addMethod(delegate_class,
                    sel_registerName("applicationWillTerminate:"),
                    reinterpret_cast<IMP>(application_will_terminate),
                    "v@:@");
    class_addMethod(delegate_class,
                    sel_registerName("applicationShouldTerminateAfterLastWindowClosed:"),
                    reinterpret_cast<IMP>(application_should_terminate_after_last_window_closed),
                    OBJC_BOOL_ENCODING "@:@");
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 150000
    class_addMethod(delegate_class,
                    sel_registerName("displayLinkFired:"),
                    reinterpret_cast<IMP>(display_link_fired),
                    "v@:@");
#endif
    objc_registerClassPair(delegate_class);

    const Class ns_view = objc_getClass("NSView");
    const Class view_class = objc_allocateClassPair(ns_view, "MVView", 0);
    class_addMethod(view_class,
                    sel_registerName("performKeyEquivalent:"),
                    reinterpret_cast<IMP>(view_perform_key_equivalent),
                    OBJC_BOOL_ENCODING "@:@");
    objc_registerClassPair(view_class);
}

int main()
{
    register_classes();

    app = send_message<id>(get_class("NSApplication"), sel_registerName("sharedApplication"));

    const id delegate_class = reinterpret_cast<id>(objc_getClass("MVAppDelegate"));
    app_delegate = send_message<id>(send_message<id>(delegate_class, sel_registerName("alloc")),
                                    sel_registerName("init"));
    send_message<void>(app, sel_registerName("setDelegate:"), app_delegate);

    create_menu();

    send_message<void>(app, sel_registerName("run"));

    return 0;
}
