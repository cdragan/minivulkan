// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#import <AVFoundation/AVAudioPlayer.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#include "../core/gui.h"
#include "main_macos.h"
#include "../core/minivulkan.h"
#include "../core/mstdc.h"
#include "../core/d_printf.h"
#include <time.h>

struct Window {
    CAMetalLayer* layer;
};

bool create_surface(struct Window* w)
{
    static VkMetalSurfaceCreateInfoEXT surf_create_info = {
        VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
    };

    surf_create_info.pLayer = w->layer;

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

static AVAudioPlayer* sound_track;

bool load_sound_track(const void* data, uint32_t size)
{
    assert( ! sound_track);

    NSData *sound_data = [NSData dataWithBytes: data
                                 length:        size];
    sound_track = [[AVAudioPlayer alloc] initWithData: sound_data
                                         error:        nullptr];

    if ( ! sound_track) {
        d_printf("Failed to load soundtrack\n");
        return false;
    }

    d_printf("Soundtrack duration %.3f s\n", sound_track.duration);

    if ( ! [sound_track prepareToPlay]) {
        d_printf("Failed to initialize soundtrack for playback\n");
        return false;
    }

    return true;
}

bool play_sound_track()
{
    assert(sound_track);

    if ( ! [sound_track play]) {
        d_printf("Failed to play soundtrack\n");
        return false;
    }

    return true;
}

@interface VulkanView: NSView
@end

@interface AppDelegate: NSObject<NSApplicationDelegate>
@end

@implementation VulkanViewController
    {
        NSSize           m_size;
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 150000
        CADisplayLink   *m_display_link;
#else
        CVDisplayLinkRef m_display_link;
#endif
    }

    - (id)initWithSize: (NSSize)aSize
    {
        self = [super init];
        if (self) {
            m_size = aSize;
        }
        return self;
    }

    - (void)loadView
    {
        NSView *view = [[VulkanView alloc]
            initWithFrame: NSMakeRect(0, 0, m_size.width, m_size.height)
        ];
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.view = view;
    }

    - (void)viewDidLoad
    {
        [super viewDidLoad];

        self.view.wantsLayer = YES;

        struct Window win;
        win.layer = (CAMetalLayer *)self.view.layer;

        if ( ! init_vulkan(&win)) {
            [NSApp terminate: nil];
            return;
        }

        init_mouse_tracking(self, self.view);
        init_os_gui(self.view);

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 150000
        CVDisplayLinkCreateWithActiveCGDisplays(&m_display_link);
        CVDisplayLinkSetOutputCallback(m_display_link, &display_link_callback, (__bridge void *)self.view);
#endif
    }

    static bool draw_callback(NSView *view)
    {
        init_os_gui_frame(view);

        if ( ! need_redraw(nullptr) && skip_frame(nullptr))
            return true;

        if ( ! draw_frame()) {
            [NSApp terminate: nil];
            return false;
        }

        return true;
    }

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 150000
    - (void)viewDidAppear
    {
        [super viewDidAppear];

        if ( ! m_display_link) {
            m_display_link = [self.view displayLinkWithTarget: self
                                        selector: @selector(displayLinkFired:)];
            [m_display_link addToRunLoop: [NSRunLoop mainRunLoop]
                            forMode: NSRunLoopCommonModes];
        }
    }

    - (void)viewWillDisappear
    {
        [super viewWillDisappear];

        if (m_display_link) {
            // Technically invalidate should be called, however it crashes, solution not known
            //[m_display_link invalidate];
            m_display_link = nullptr;
        }
    }

    - (void)displayLinkFired: (CADisplayLink *)display_link
    {
        draw_callback(self.view);
    }
#else
    - (void)dealloc
    {
        CVDisplayLinkRelease(m_display_link);
        [super dealloc];
    }

    - (void)viewDidAppear
    {
        CVDisplayLinkStart(m_display_link);
    }

    - (void)viewWillDisappear
    {
        CVDisplayLinkStop(m_display_link);
    }

    static CVReturn display_link_callback(CVDisplayLinkRef   displayLink,
                                          const CVTimeStamp *now,
                                          const CVTimeStamp *outputTime,
                                          CVOptionFlags      flagsIn,
                                          CVOptionFlags     *flagsOut,
                                          void              *target)
    {
        return draw_callback((__bridge NSView *)target) ? kCVReturnSuccess : kCVReturnError;
    }
#endif

@end

@implementation VulkanView

    - (BOOL)wantsUpdateLayer
    {
        return YES;
    }

    + (Class)layerClass
    {
        return [CAMetalLayer class];
    }

    - (CALayer *)makeBackingLayer
    {
        CAMetalLayer* layer      = [self.class.layerClass layer];

        // Avoid UI scaling in full screen mode, assuming that in this mode we don't
        // need cursor interaction (otherwise cursor position would need to be scaled properly).
        if ( ! is_full_screen()) {
            const NSSize  view_scale = [self convertSizeToBacking: NSMakeSize(1, 1)];
            layer.contentsScale      = MIN(view_scale.width, view_scale.height);
        }

        return layer;
    }

    - (BOOL)performKeyEquivalent: (NSEvent *)event
    {
        const uint32_t key_esc  = 53;
        const uint32_t key_q    = 12;
        const uint32_t mods     = NSEventModifierFlagCommand
                                | NSEventModifierFlagOption
                                | NSEventModifierFlagControl
                                | NSEventModifierFlagShift;

        const uint32_t key      = event.keyCode;
        const uint32_t modifier = event.modifierFlags & mods;

        if ((key == key_esc) || (key == key_q && modifier == NSEventModifierFlagCommand)) {
            [NSApp terminate: nil];
        }

        return TRUE;
    }
@end

@implementation AppDelegate

    - (void)createMenu
    {
        id main_menu = [NSMenu new];
        NSApp.mainMenu = main_menu;

        NSMenuItem *app_item = [NSMenuItem new];
        [main_menu addItem: app_item];

        id app_menu = [NSMenu new];
        app_item.submenu = app_menu;

        id quit_item = [[NSMenuItem alloc]
            initWithTitle: @"Quit"
            action:        @selector(terminate:)
            keyEquivalent: @"q"
        ];
        [app_menu addItem: quit_item];
    }

    - (void)applicationDidFinishLaunching: (NSNotification *)notification
    {
        const bool full_screen = is_full_screen();

        NSRect screen_frame = [[NSScreen mainScreen] frame];
        NSRect frame_rect   = NSMakeRect(0, 0,
                                         screen_frame.size.width,
                                         screen_frame.size.height);
        if ( ! full_screen)
            frame_rect = NSMakeRect(0, 0, get_main_window_width(), get_main_window_height());

        NSWindow *window = [[NSWindow alloc]
            initWithContentRect: frame_rect
            styleMask:           (NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable |
                                  NSWindowStyleMaskResizable)
            backing:             NSBackingStoreBuffered
            defer:               NO
        ];
        [window center];
        [window makeKeyAndOrderFront: nil];
        window.title   = [[NSString alloc]
                            initWithCString: app_name
                            encoding: NSASCIIStringEncoding];
        window.minSize = NSMakeSize(512, 384);

        id view_ctrl = [[VulkanViewController alloc]
            initWithSize: frame_rect.size
        ];
        window.contentViewController = view_ctrl;

        if (full_screen) {
            [NSCursor hide];
            [window setBackgroundColor: NSColor.blackColor];
            [window setCollectionBehavior: NSWindowCollectionBehaviorFullScreenPrimary];
            [window setFrame: screen_frame display: YES];
            [window toggleFullScreen: self];
        }
        else
            [NSApp activateIgnoringOtherApps: YES];
    }

    - (void)applicationWillTerminate: (NSNotification *)notification
    {
        idle_queue();
    }

    - (BOOL)applicationShouldTerminateAfterLastWindowClosed: (NSApplication *)sender
    {
        return YES;
    }

@end

int main()
{
    [NSApplication sharedApplication];
    id delegate = [AppDelegate new];
    NSApp.delegate = delegate;

    [delegate createMenu];

    [NSApp run];

    return 0;
}
