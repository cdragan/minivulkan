// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#import <AppKit/AppKit.h>

@interface VulkanViewController: NSViewController
    - (id)initWithSize: (NSSize)aSize;
@end

void init_mouse_tracking(NSViewController *view_controller, NSView *view);
void init_os_gui(NSView *view);
void init_os_gui_frame(NSView *view);
