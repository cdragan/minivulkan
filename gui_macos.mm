// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "main_macos.h"
#include "gui.h"
#include "thirdparty/imgui/src/backends/imgui_impl_osx.h"

static bool window_needs_update = true;

bool need_redraw(struct Window*)
{
    const bool needs_update = window_needs_update;

    window_needs_update = false;

    return needs_update;
}

@interface EventHandlingDelegate: NSObject<NSWindowDelegate>
@end

@implementation EventHandlingDelegate

    - (void)windowDidResize: (NSNotification *)notification
    {
        window_needs_update = true;
    }

@end

void init_mouse_tracking(NSViewController *view_controller, NSView *view)
{
    // Enable correct mouse tracking
    NSTrackingArea *trackingArea =
        [[NSTrackingArea alloc] initWithRect: NSZeroRect
                                options:      NSTrackingMouseMoved | NSTrackingInVisibleRect | NSTrackingActiveAlways
                                owner:        view_controller
                                userInfo:     nil];
    [view addTrackingArea: trackingArea];

    // Save view scaling for ImGui
    const NSSize view_scale = [view convertSizeToBacking: NSMakeSize(1, 1)];
    vk_surface_scale = static_cast<float>(MIN(view_scale.width, view_scale.height));
}

void init_os_gui(NSView *view)
{
    ImGui_ImplOSX_Init(view);

    EventHandlingDelegate* delegate = [[EventHandlingDelegate alloc] init];
    [[[NSApp orderedWindows] firstObject] setDelegate: delegate];
}

void init_os_gui_frame(NSView *view)
{
    ImGui_ImplOSX_NewFrame(view);
}
