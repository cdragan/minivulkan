// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "sculptor_editor.h"
#include "../gui_imgui.h"

#include <stdio.h>

void Sculptor::Editor::set_object_name(const char* new_name)
{
    snprintf(object_name, sizeof(object_name), "%s", new_name);
}

vmath::vec2 Sculptor::Editor::get_rel_mouse_pos(const UserInput& input)
{
    const vmath::vec2 abs_window_pos = ImGui::GetItemRectMin();
    return input.abs_mouse_pos - abs_window_pos;
}

namespace {
    Sculptor::Editor* editor_with_mouse = nullptr;
}

void Sculptor::Editor::capture_mouse()
{
    editor_with_mouse = this;
}

void Sculptor::Editor::release_mouse()
{
    assert(editor_with_mouse == this);
    editor_with_mouse = nullptr;
}

bool Sculptor::Editor::has_captured_mouse() const
{
    return editor_with_mouse == this;
}

bool Sculptor::Editor::is_mouse_captured()
{
    return editor_with_mouse != nullptr;
}
