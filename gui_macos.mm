// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "main_macos.h"
#include "gui.h"
#include "thirdparty/imgui/src/backends/imgui_impl_osx.h"

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
}

void init_os_gui_frame(NSView *view)
{
    ImGui_ImplOSX_NewFrame(view);
}
