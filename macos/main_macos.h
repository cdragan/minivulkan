// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#import <AppKit/AppKit.h>

@interface VulkanViewController: NSViewController
@end

void init_mouse_tracking(NSViewController *view_controller, NSView *view);
void init_os_gui(NSView *view);
void init_os_gui_frame(NSView *view);
