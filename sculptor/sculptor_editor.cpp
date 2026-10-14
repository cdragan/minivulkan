// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_editor.h"
#include "../core/gui_imgui.h"

#include <stdio.h>

bool Sculptor::is_actual_ctrl_down()
{
    // On MacOS Ctrl and Super are swapped, but this function wants
    // to check real Ctrl key
#ifdef __APPLE__
    constexpr ImGuiKey left_ctrl  = ImGuiKey_LeftSuper;
    constexpr ImGuiKey right_ctrl = ImGuiKey_RightSuper;
#else
    constexpr ImGuiKey left_ctrl  = ImGuiKey_LeftCtrl;
    constexpr ImGuiKey right_ctrl = ImGuiKey_RightCtrl;
#endif

    return ImGui::IsKeyDown(left_ctrl) || ImGui::IsKeyDown(right_ctrl);
}

bool Sculptor::is_ctrl_down()
{
    return ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
}

bool Sculptor::is_shift_down()
{
    return ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
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
