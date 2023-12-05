// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_editor.h"
#include "../gui_imgui.h"

#include <stdio.h>

void Sculptor::Editor::set_object_name(const char* new_name)
{
    snprintf(object_name, sizeof(object_name), "%s", new_name);
}

vmath::vec2 Sculptor::Editor::get_rel_mouse_pos(const MouseInfo& mouse)
{
    const vmath::vec2 abs_window_pos = ImGui::GetItemRectMin();
    return mouse.abs_mouse_pos - abs_window_pos;
}
