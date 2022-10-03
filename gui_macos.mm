// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "main_macos.h"
#include "gui.h"
#include "imgui/backends/imgui_impl_osx.h"

@interface GUIVulkanViewController: VulkanViewController
@end

@implementation GUIVulkanViewController

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
}

void init_os_gui_frame(NSView *view)
{
    ImGui_ImplOSX_NewFrame(view);
}

VulkanViewController *alloc_view_controller()
{
    return [GUIVulkanViewController alloc];
}
