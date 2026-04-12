// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_asset_list.h"

#include <stdio.h>
#include <string.h>

static Sculptor::AssetSlotHeader& get_slot(Sculptor::AssetSlotHeader* const slots,
                                           uint32_t                   const slot_stride,
                                           uint32_t                   const idx)
{
    const uintptr_t raw_ptr = reinterpret_cast<uintptr_t>(slots) + idx * slot_stride;
    return *reinterpret_cast<Sculptor::AssetSlotHeader*>(raw_ptr);
}

Sculptor::AssetListAction Sculptor::render_asset_list(AssetSlotHeader*      const slots,
                                                      uint32_t              const slot_stride,
                                                      AssetType             const asset_type,
                                                      AssetListState*       const state,
                                                      int32_t*              const selected,
                                                      const char*           const add_label,
                                                      const char*           const default_name,
                                                      const ThumbnailAtlas* const thumbs)
{
    AssetListAction action = { AssetListAction::none, -1 };

    const float thumb_display = thumbs ? static_cast<float>(thumbs->thumb_size) : 0.0f;

    // Rename state (only one rename active at a time across all lists)
    static int32_t renaming_slot    = -1;
    static char    rename_buf[sizeof(AssetSlotHeader::name)];
    static bool    rename_focus_set = false;
    static bool    rename_was_active = false;

    // Render existing items
    for (uint32_t list_idx = 0; list_idx < state->num_active_slots; list_idx++) {
        const uint32_t   slot_idx = state->display_order[list_idx];
        AssetSlotHeader& slot     = get_slot(slots, slot_stride, slot_idx);

        if (slot.type == AssetType::unknown)
            continue;

        const bool is_selected = (*selected == static_cast<int32_t>(slot_idx));
        const bool is_renaming = (renaming_slot == static_cast<int32_t>(slot_idx));

        ImGui::PushID(static_cast<int>(slot_idx));

        // Thumbnail + selectable on the same line
        if (thumbs && thumbs->texture_id) {
            const uint32_t col      = slot_idx % thumbs->grid_cols;
            const uint32_t row      = slot_idx / thumbs->grid_cols;
            const float    inv_grid = 1.0f / static_cast<float>(thumbs->grid_cols);
            const ImVec2   uv0{static_cast<float>(col) * inv_grid,
                               static_cast<float>(row) * inv_grid};
            const ImVec2   uv1{static_cast<float>(col + 1) * inv_grid,
                               static_cast<float>(row + 1) * inv_grid};

            ImGui::Image(thumbs->texture_id, ImVec2{thumb_display, thumb_display}, uv0, uv1);
            ImGui::SameLine();
        }

        if (is_renaming) {
            // Inline rename editor
            if ( ! rename_focus_set) {
                ImGui::SetKeyboardFocusHere();
                rename_focus_set  = true;
                rename_was_active = false;
            }

            const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue
                                            | ImGuiInputTextFlags_AutoSelectAll;

            if (ImGui::InputText("##rename", rename_buf, sizeof(rename_buf), flags)) {
                // Enter pressed: commit rename
                memcpy(slot.name, rename_buf, sizeof(slot.name));
                renaming_slot = -1;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                // Cancel rename
                renaming_slot = -1;
            }
            else {
                const bool is_active = ImGui::IsItemActive();
                // Only detect lost focus after the input was active for at least one frame
                if (rename_was_active && ! is_active) {
                    memcpy(slot.name, rename_buf, sizeof(slot.name));
                    renaming_slot = -1;
                }
                rename_was_active = is_active;
            }
        }
        else {
            if (ImGui::Selectable(slot.name, is_selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2{0, thumb_display > 0 ? thumb_display : 0})) {
                *selected = static_cast<int32_t>(slot_idx);
                action    = { AssetListAction::selected, *selected };

                // Double-click starts rename
                if (ImGui::IsMouseDoubleClicked(0)) {
                    renaming_slot    = static_cast<int32_t>(slot_idx);
                    memcpy(rename_buf, slot.name, sizeof(rename_buf));
                    rename_focus_set = false;
                }
            }

            // Update selection on keyboard nav focus (arrow keys)
            if (ImGui::IsItemFocused() && ! is_selected) {
                *selected = static_cast<int32_t>(slot_idx);
                action    = { AssetListAction::selected, *selected };
            }

            // F2 starts rename on selected item
            if (is_selected && ImGui::IsKeyPressed(ImGuiKey_F2)) {
                renaming_slot    = static_cast<int32_t>(slot_idx);
                memcpy(rename_buf, slot.name, sizeof(rename_buf));
                rename_focus_set = false;
            }
        }

        // Drag source (not while renaming)
        if ( ! is_renaming && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("ASSET_REORDER", &list_idx, sizeof(list_idx));
            ImGui::Text("%s", slot.name);
            ImGui::EndDragDropSource();
        }

        // Drop target
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_REORDER");
            if (payload) {
                const uint32_t src_idx = *static_cast<const uint32_t*>(payload->Data);
                if (src_idx != list_idx) {
                    const uint32_t moved_slot = state->display_order[src_idx];
                    if (src_idx < list_idx) {
                        for (uint32_t j = src_idx; j < list_idx; j++)
                            state->display_order[j] = state->display_order[j + 1];
                    }
                    else {
                        for (uint32_t j = src_idx; j > list_idx; j--)
                            state->display_order[j] = state->display_order[j - 1];
                    }
                    state->display_order[list_idx] = moved_slot;
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
    }

    // Delete selected item with Delete key (slot 0 is always protected, not while renaming)
    if (*selected > 0 && renaming_slot < 0 &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {

        const uint32_t del_idx = static_cast<uint32_t>(*selected);

        // Clear the entire slot (zero out all data, not just the type)
        memset(&get_slot(slots, slot_stride, del_idx), 0, slot_stride);

        // Remove from display order and select nearest remaining item
        for (uint32_t i = 0; i < state->num_active_slots; i++) {
            if (state->display_order[i] == del_idx) {
                for (uint32_t j = i; j + 1 < state->num_active_slots; j++)
                    state->display_order[j] = state->display_order[j + 1];
                state->num_active_slots--;

                // Select the item that took the deleted item's position, or the one before it
                if (state->num_active_slots > 0) {
                    const uint32_t new_pos = (i < state->num_active_slots) ? i : state->num_active_slots - 1;
                    *selected = static_cast<int32_t>(state->display_order[new_pos]);
                }
                else {
                    *selected = -1;
                }
                break;
            }
        }

        action = { AssetListAction::deleted, static_cast<int32_t>(del_idx) };
    }

    // "Add" row at the bottom
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Selectable(add_label, false)) {
        // Find first free slot (skip slot 0, reserved as default)
        for (uint32_t i = 1; i < max_asset_slots; i++) {
            AssetSlotHeader& slot = get_slot(slots, slot_stride, i);
            if (slot.type == AssetType::unknown) {
                slot.type = asset_type;
                snprintf(slot.name, sizeof(slot.name), "%s %u", default_name, i);
                state->display_order[state->num_active_slots] = i;
                state->num_active_slots++;
                *selected = static_cast<int32_t>(i);
                action    = { AssetListAction::added, *selected };
                break;
            }
        }
    }
    ImGui::PopStyleColor();

    return action;
}
