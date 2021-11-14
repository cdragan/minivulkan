// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "minivulkan.h"

struct Window
{
    CAMetalLayer* layer;
};

bool create_surface(struct Window* w)
{
    static VkMetalSurfaceCreateInfoEXT surf_create_info = {
        VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
    };

    surf_create_info.pLayer = w->layer;

    return vkCreateMetalSurfaceEXT(vk_instance,
                                   &surf_create_info,
                                   NULL,
                                   &vk_surface) == VK_SUCCESS;
}

@interface VulkanViewController: NSViewController
    - (id)initWithSize: (NSSize)aSize;
@end

@interface VulkanView: NSView
@end

@interface AppDelegate: NSObject<NSApplicationDelegate>
    - (void)createMenu;
@end

@implementation VulkanViewController
    {
        NSSize           size_;
        CVDisplayLinkRef display_link_;
    }

    - (id)initWithSize: (NSSize)aSize
    {
        self = [super init];
        if (self) {
            size_ = aSize;
        }
        return self;
    }

    - (void)dealloc
    {
        CVDisplayLinkRelease(display_link_);
        [super dealloc];
    }

    - (void)loadView
    {
        NSView *view = [[VulkanView alloc]
            initWithFrame: NSMakeRect(0, 0, size_.width, size_.height)
        ];
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.view = view;
    }

    - (void)viewDidLoad
    {
        [super viewDidLoad];

        self.view.wantsLayer = YES;

        struct Window win;
        win.layer = (__bridge CAMetalLayer *)self.view.layer;

        if ( ! init_vulkan(&win)) {
            [NSApp terminate: nil];
            return;
        }

        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, &display_link_callback, (__bridge void *)self.view);
        CVDisplayLinkStart(display_link_);
    }

    static CVReturn display_link_callback(CVDisplayLinkRef   displayLink,
                                          const CVTimeStamp *now,
                                          const CVTimeStamp *outputTime,
                                          CVOptionFlags      flagsIn,
                                          CVOptionFlags     *flagsOut,
                                          void              *target)
    {
        if ( ! draw_frame()) {
            [NSApp terminate: nil];
            return kCVReturnError;
        }

        return kCVReturnSuccess;
    }

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
        const NSSize  view_scale = [self convertSizeToBacking: NSMakeSize(1, 1)];
        layer.contentsScale      = MIN(view_scale.width, view_scale.height);
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
    {
    }

    - (void)createMenu
    {
        id main_menu = [[NSMenu alloc] init];
        NSApp.mainMenu = main_menu;

        NSMenuItem *app_item = [[NSMenuItem alloc] init];
        [main_menu addItem: app_item];

        id app_menu = [[NSMenu alloc] init];
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
        NSRect screen_frame = [[NSScreen mainScreen] frame];
        NSRect frame_rect   = NSMakeRect(0, 0,
                                         screen_frame.size.width / 2,
                                         screen_frame.size.height / 2);

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
        window.title   = @ APPNAME;
        window.minSize = NSMakeSize(256, 256);

        id view_ctrl = [[VulkanViewController alloc]
            initWithSize: frame_rect.size
        ];
        window.contentViewController = view_ctrl;

        [window setBackgroundColor: NSColor.blackColor];
        [window setCollectionBehavior: NSWindowCollectionBehaviorFullScreenPrimary];
        [window setFrame: screen_frame display: YES];
        [window toggleFullScreen: self];
    }

    - (void)applicationWillTerminate: (NSNotification *)notification
    {
    }

    - (BOOL)applicationShouldTerminateAfterLastWindowClosed: (NSApplication *)sender
    {
        return YES;
    }

@end

int main()
{
    [NSApplication sharedApplication];
    id delegate = [[AppDelegate alloc] init];
    NSApp.delegate = delegate;

    [delegate createMenu];

    [NSApp run];

    return 0;
}
