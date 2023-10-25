// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geom_edit.h"
#include "../mstdc.h"
#include "../imgui/imgui.h"
#include <stdio.h>

/*

- One viewport
    - Should there me multiple viewports?
    - Draw
        - Background
        - Grid
        - Solid object fill
            - Toggle solid fill on/off (off - pure wireframe)
            - Toggle tessellation on/off
        - Wireframe
            - Toggle between generated triangles and just quad surrounds, i.e. patch wireframe
        - Vertices
        - Connectors to control vertices
        - Selection highlight (surround around selected features)
        - Hovered and selected features have distinct color
        - Toggle texture/color/etc. versus plain
        - Selection rectangle
        - Draw selection surface used to determine what mouse and selection rectangle are touching
- Status: current mode, total vertices/edges/faces, number of selected vertices/edges/faces
- Toolbar with key shortcuts

User input
----------

- View
    - Num 5 :: toggle orthographic view and perspective
    - Num . :: focus on selection
    - Num 1/2/3 orthographic view aligned to axis
    - Alt-Z toggle all vertices (wireframe) and surface vertices (solid) - impacts selection
    - Middle mouse button :: rotate view
    - Shift + Middle mouse button :: pan view
    - Mouse scroll :: zoom view
- Select
    - Drag rectangle to select
    - Additive/subtractive selection
- Manipulation
    - G :: grab and move objects
        - click to finish
        - GX/GY/GZ to snap
        - mouse scroll to change range of the effect
    - R :: rotate
    - S :: scale
        - SX/SY/SZ to snap
    - Alt-S :: move along normals
    - Tab :: toggle between object mode and edit mode
    - Ctrl + Mouse :: snap
- Edit
    - E :: extrude - add faces/edges, begin moving
*/

void GeometryEditor::set_name(const char* new_name)
{
    const uint32_t len = mstd::min(mstd::strlen(new_name),
                                   static_cast<uint32_t>(sizeof(name)) - 1U);
    mstd::mem_copy(name, new_name, len);
    name[len] = 0;
}

void GeometryEditor::gui_status_bar()
{
    const ImVec2 item_pos = ImGui::GetItemRectMin();
    const ImVec2 win_size = ImGui::GetWindowSize();

    ImGui::SetNextWindowPos(ImVec2(item_pos.x,
                                   item_pos.y + win_size.y - ImGui::GetFrameHeight()));

    const ImGuiWindowFlags status_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_MenuBar;

    const ImVec2 status_bar_size{win_size.x,
                                 ImGui::GetTextLineHeightWithSpacing()};

    if (ImGui::BeginChild("##Geometry Editor Status Bar", status_bar_size, false, status_flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::Text("Pos: %ux%u", static_cast<unsigned>(item_pos.x), static_cast<unsigned>(item_pos.y));
            ImGui::Separator();
            ImGui::Text("Window: %ux%u", static_cast<unsigned>(win_size.x), static_cast<unsigned>(win_size.y));

            ImGui::EndMenuBar();
        }
    }
    ImGui::EndChild();
}

bool GeometryEditor::create_gui_frame(uint32_t image_idx)
{
    char window_title[sizeof(name) + 36];
    snprintf(window_title, sizeof(window_title), "Geometry Editor - %s###Geometry Editor", name);

    const ImGuiWindowFlags geom_win_flags =
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoScrollbar;

    if ( ! ImGui::Begin(window_title, nullptr, geom_win_flags)) {
        ImGui::End();
        return true;
    }

    gui_status_bar();

    ImGui::End();

    return true;
}

bool GeometryEditor::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    return true;
}
