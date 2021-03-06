// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#import <AppKit/AppKit.h>
#import <AVFoundation/AVAudioPlayer.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "minivulkan.h"
#include "mstdc.h"
#include "d_printf.h"
#ifdef ENABLE_GUI
#   include "imgui/backends/imgui_impl_osx.h"
#endif
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

struct Sound {
    AVAudioPlayer* sounds[1];
};

static Sound sound;

bool load_sound(uint32_t sound_id, const void* data, uint32_t size)
{
    if (sound_id >= mstd::array_size(sound.sounds)) {
        d_printf("Sound %u doesn't exist\n", sound_id);
        return false;
    }

    #ifdef ENABLE_GUI
    if (sound.sounds[sound_id]) {
        [sound.sounds[sound_id] stop];
        sound.sounds[sound_id] = nullptr;
    }
    #endif

    NSData *sound_data = [NSData dataWithBytes: data
                                 length:        size];
    sound.sounds[sound_id] = [[AVAudioPlayer alloc] initWithData: sound_data
                                                    error:        nullptr];

    if ( ! sound.sounds[sound_id]) {
        d_printf("Failed to load sound %u\n", sound_id);
        return false;
    }

    d_printf("Sound %u duration %.3f s\n", sound_id, sound.sounds[sound_id].duration);

    if ( ! [sound.sounds[sound_id] prepareToPlay]) {
        d_printf("Failed to initialize sound %u for playback\n", sound_id);
        return false;
    }

    return true;
}

bool play_sound(uint32_t sound_id)
{
    if (sound_id >= mstd::array_size(sound.sounds)) {
        d_printf("Sound %u doesn't exist\n", sound_id);
        return false;
    }

    if ( ! [sound.sounds[sound_id] play]) {
        d_printf("Failed to play sound %u\n", sound_id);
        return false;
    }

    return true;
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
        NSSize           m_size;
        CVDisplayLinkRef m_display_link;
    }

    - (id)initWithSize: (NSSize)aSize
    {
        self = [super init];
        if (self) {
            m_size = aSize;
        }
        return self;
    }

    - (void)dealloc
    {
        CVDisplayLinkRelease(m_display_link);
        [super dealloc];
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

        #ifdef ENABLE_GUI
        // Enable correct mouse tracking
        NSTrackingArea *trackingArea =
            [[NSTrackingArea alloc] initWithRect: NSZeroRect
                                    options:      NSTrackingMouseMoved | NSTrackingInVisibleRect | NSTrackingActiveAlways
                                    owner:        self
                                    userInfo:     nil];
        [self.view addTrackingArea: trackingArea];

        ImGui_ImplOSX_Init(self.view);
        #endif

        CVDisplayLinkCreateWithActiveCGDisplays(&m_display_link);
        CVDisplayLinkSetOutputCallback(m_display_link, &display_link_callback, (__bridge void *)self.view);
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
        #ifdef ENABLE_GUI
        ImGui_ImplOSX_NewFrame((__bridge NSView *)target);
        #endif

        if ( ! draw_frame()) {
            [NSApp terminate: nil];
            return kCVReturnError;
        }

        return kCVReturnSuccess;
    }

    #ifdef ENABLE_GUI
    - (void)mouseDown:         (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)rightMouseDown:    (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)otherMouseDown:    (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)mouseUp:           (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)rightMouseUp:      (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)otherMouseUp:      (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)mouseMoved:        (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)mouseDragged:      (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)rightMouseMoved:   (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)rightMouseDragged: (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)otherMouseMoved:   (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)otherMouseDragged: (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
    - (void)scrollWheel:       (NSEvent *)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
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
        #ifdef ENABLE_GUI
        constexpr bool full_screen = false;
        #else
        constexpr bool full_screen = true;
        #endif

        NSRect screen_frame = [[NSScreen mainScreen] frame];
        NSRect frame_rect   = NSMakeRect(0, 0,
                                         screen_frame.size.width,
                                         screen_frame.size.height);
        if ( ! full_screen)
            frame_rect = NSMakeRect(0, 0, 800, 600);

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
