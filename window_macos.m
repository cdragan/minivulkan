
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include "window.h"

static int macos_draw_frame(void *target);

@interface VulkanViewController: NSViewController
    - (id)initWithSize: (NSSize)aSize
                window: (Window *)aWindow;
@end

@interface VulkanView: NSView
@end

@interface AppDelegate: NSObject<NSApplicationDelegate>
    - (id)initWithTitle: (NSString *)title
                minSize: (NSSize)aMinSize
                   size: (NSSize)aSize
                 window: (Window *)aCtx;

    - (void)createMenu;
@end

@implementation VulkanViewController
    {
        NSSize           size_;
        CVDisplayLinkRef display_link_;
        Window          *win_;
    }

    - (id)initWithSize: (NSSize)aSize
                window: (Window *)aWindow
    {
        self = [super init];
        if (self) {
            size_ = aSize;
            win_  = aWindow;
        }
        return self;
    }

    - (void)dealloc
    {
        CVDisplayLinkRelease(display_link_);
        //[super dealloc];
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

        win_->layer_ptr = (__bridge void*)self.view.layer;

        CVDisplayLinkCreateWithActiveCGDisplays(&display_link_);
        CVDisplayLinkSetOutputCallback(display_link_, &display_link_callback, win_);
        CVDisplayLinkStart(display_link_);
    }

    static CVReturn display_link_callback(CVDisplayLinkRef   displayLink,
                                          const CVTimeStamp *now,
                                          const CVTimeStamp *outputTime,
                                          CVOptionFlags      flagsIn,
                                          CVOptionFlags     *flagsOut,
                                          void              *target)
    {
        if (macos_draw_frame(target)) {
            [NSApp terminate: nil];
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

    - (CALayer*)makeBackingLayer
    {
        CALayer* layer      = [self.class.layerClass layer];
        CGSize   view_scale = [self convertSizeToBacking: CGSizeMake(1.0, 1.0)];
        layer.contentsScale = MIN(view_scale.width, view_scale.height);
        return layer;
    }

@end

@implementation AppDelegate
    {
        NSString *title_;
        NSSize    min_size_;
        NSSize    size_;
        Window   *win_;
    }

    - (id)initWithTitle: (NSString *)aTitle
                minSize: (NSSize)aMinSize
                   size: (NSSize)aSize
                 window: (Window *)aWindow
    {
        self = [super init];
        if (self) {
            title_    = aTitle;
            min_size_ = aMinSize;
            size_     = aSize;
            win_      = aWindow;
        }
        return self;
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
        NSWindow *window = [[NSWindow alloc]
            initWithContentRect: NSMakeRect(0, 0, size_.width, size_.height)
            styleMask:           (NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable |
                                  NSWindowStyleMaskResizable)
            backing:             NSBackingStoreBuffered
            defer:               NO
        ];
        [window center];
        [window makeKeyAndOrderFront: nil];
        window.title   = title_;
        window.minSize = min_size_;

        id view_ctrl = [[VulkanViewController alloc]
            initWithSize: size_
            window:       win_
        ];
        window.contentViewController = view_ctrl;
    }

    - (void)applicationWillTerminate: (NSNotification *)notification
    {
    }

    - (BOOL)applicationShouldTerminateAfterLastWindowClosed: (NSApplication *)sender
    {
        return YES;
    }

@end

int macos_draw_frame(void *target)
{
    return 0;
}

bool create_window(Window* w)
{
    [NSApplication sharedApplication];
    id delegate = [[AppDelegate alloc]
        initWithTitle: @"minivulkan"
        minSize:       NSMakeSize(640, 480)
        size:          NSMakeSize(1024, 768)
        window:        w
    ];
    NSApp.delegate = delegate;

    [delegate createMenu];

    return true;
}

int event_loop(Window *w)
{
    [NSApp run];
    return 0;
}
