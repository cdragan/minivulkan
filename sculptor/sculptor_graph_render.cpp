// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Graph widget rendering and interaction.  Split from sculptor_graph.cpp so
// the unit test can link the data model without ImGui symbols.  This file
// implements Graph::render(); it is called inside an already-open window and
// never calls ImGui::Begin/End.

#include "../thirdparty/imgui/src/imgui_internal.h"
#include "sculptor_graph.h"
#include <stdio.h>

#include <float.h>
#include <math.h>

namespace {

using Sculptor::ChangeKind;
using Sculptor::graph_grid_axis_cells;
using Sculptor::graph_grid_spacing;
using Sculptor::graph_max_zoom;
using Sculptor::graph_min_zoom;
using Sculptor::max_node_slots;
using Sculptor::max_nodes;
using Sculptor::PropertyType;
using Sculptor::SlotKind;

constexpr float node_padding         = 8.0f;                     // inner margins of a node rect
constexpr float node_corner_rounding = 4.0f;                     // slight corner radius on node rects
constexpr float dot_radius           = 5.0f;                     // connector dot radius
constexpr float dot_space            = 2.0f * dot_radius + 2.0f; // room a dot claims
constexpr float property_widget_w    = 80.0f;                    // width of inline value widgets

// A two-option list renders as a label+radio pair, wider than the fixed
// property widget column: reserve its measured width so the pair never
// overlaps the element that follows it on the line.
static float slot_widget_w(const Sculptor::Slot& slot)
{
    if (slot.property_type == Sculptor::PropertyType::list && slot.num_list_options == 2) {
        return ImGui::CalcTextSize(slot.list_options[0]).x + ImGui::CalcTextSize(slot.list_options[1]).x +
               2.0f * ImGui::GetFrameHeight() + 3.0f * ImGui::GetStyle().ItemSpacing.x;
    }
    return property_widget_w;
}

constexpr float fit_view_margin = 32.0f; // empty margin around Home fit

// Oscillator-node rows whose widgets only make sense for the current
// waveform/mode selection.  Slot indices follow the projection layout
// contract in sculptor_osc_graph.cpp (15 slots; waveform b at 3, mix mode
// at 5, fm depth at 7, fm ratio at 8).
bool osc_slot_disabled(const Sculptor::Node& node, uint32_t slot_idx)
{
    if (node.slots.num_allocated != 15) {
        return false;
    }
    const uint32_t wave_b = node.slots.entries[3].value.list_index;
    const uint32_t mode   = node.slots.entries[5].value.list_index;
    if (wave_b == 0 && (slot_idx == 4 || slot_idx == 5 || slot_idx == 6 || slot_idx == 7 || slot_idx == 8)) {
        return true; // duty b, mix mode, waveform mix and both fm rows need waveform b
    }
    if (slot_idx == 7 && mode != 1) {
        return true; // fm depth only drives the fm mix mode
    }
    if (slot_idx == 8 && mode == 0) {
        return true; // fm ratio drives fm and hard sync, not blend
    }
    return false;
}

constexpr float zoom_wheel_factor  = 1.2f;              // zoom step per wheel tick
constexpr float click_max_distance = 5.0f;              // press-release distance still a click
constexpr float dot_pick_radius    = dot_radius + 4.0f; // dot hit-test radius
constexpr float curve_min_reach    = 30.0f;             // bezier control point reach, px

ImU32 to_imgui(uint32_t packed)
{
    return IM_COL32((packed >> 24) & 0xFFu, (packed >> 16) & 0xFFu, (packed >> 8) & 0xFFu, packed & 0xFFu);
}

// Keep one rasterization across zoom levels so controls cannot jump at font boundaries.
static float push_zoom_font(float scale)
{
    const float previous_scale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(previous_scale * scale);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * scale, style.FramePadding.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * scale, style.ItemSpacing.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
                        ImVec2(style.ItemInnerSpacing.x * scale, style.ItemInnerSpacing.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, style.GrabMinSize * scale);
    return previous_scale;
}

static void pop_zoom_font(float previous_scale)
{
    ImGui::PopStyleVar(4);
    ImGui::SetWindowFontScale(previous_scale);
}

// Draw-list text at zoom size: ImGui 1.89 stretches the baked glyphs when
// AddText gets a size other than the font's baked size.
void add_text_scaled(ImDrawList* draw_list, vmath::vec2 pos, float scale, ImU32 color, const char* text)
{
    draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, ImVec2(pos.x, pos.y), color, text);
}

float snap_to_grid(float value)
{
    return floor(value / graph_grid_spacing) * graph_grid_spacing;
}

} // namespace

void Sculptor::Graph::render(vmath::vec2 size, void* user_data)
{
    ImDrawList* const draw_list = ImGui::GetWindowDrawList();
    const ImVec2 cursor_position = ImGui::GetCursorScreenPos();
    const vmath::vec2 origin(cursor_position.x, cursor_position.y);
    ImGuiIO&          io = ImGui::GetIO();
    const vmath::vec2 widget_size(size.x, size.y);
    const vmath::vec2 mouse_screen(io.MousePos.x, io.MousePos.y);

    // Seed all item IDs with the instance address so several graphs can share
    // one window without ID collisions.
    ImGui::PushID(this);
    // Keep node drawing inside the widget rect: nodes near the edge must not
    // paint over caller content in the same window.
    draw_list->PushClipRect(ImVec2(origin.x, origin.y),
                            ImVec2(origin.x + widget_size.x, origin.y + widget_size.y),
                            true);

    // A zero zoom (zero-filled state) is treated as 1 everywhere below.
    const float zoom_scale = zoom != 0.0f ? zoom : 1.0f;
    // Uniform zoom: node content (text, widgets, dots) renders at zoom scale
    // at every zoom level, so zooming out yields a true miniature of the graph.
    const float render_scale = zoom_scale;
    const auto  mouse_graph  = [&]() { return (view_origin + ((mouse_screen - origin) / zoom_scale)); };
    const auto  in_widget    = [&](vmath::vec2 pos_screen) {
        return pos_screen.x >= origin.x && pos_screen.y >= origin.y && pos_screen.x <= (origin + widget_size).x &&
               pos_screen.y <= (origin + widget_size).y;
    };
    const auto in_rect = [&](vmath::vec2 pos_screen, vmath::vec2 rect_min, vmath::vec2 rect_max) {
        return pos_screen.x >= rect_min.x && pos_screen.y >= rect_min.y && pos_screen.x <= rect_max.x &&
               pos_screen.y <= rect_max.y;
    };

    // Ghost nodes follow the mouse until placed or cancelled; position is
    // updated before layout so the node draws at the mouse position.  Hover
    // inside the widget is required: without it io.MousePos is -FLT_MAX
    // off-screen when the window loses focus.
    uint32_t ghost_idx = pool_no_slot;
    if (ImGui::IsWindowHovered() && in_widget(mouse_screen)) {
        for (uint32_t i = 0; i < max_nodes; ++i) {
            if (nodes.is_occupied(i) && nodes.entries[i].ghost) {
                ghost_idx      = i;
                Node& ghost    = nodes.entries[i];
                ghost.position = vmath::vec2(snap_to_grid(mouse_graph().x), snap_to_grid(mouse_graph().y));
                break;
            }
        }
    }

    // Wheel zoom around the mouse cursor (not during drags: zooming would
    // rescale mouse_graph and make the dragged node jump).  The mouse sits
    // over a state widget with a 1-frame lag: wheel over the envelope chart
    // zooms the chart, not the canvas.
    bool state_widget_has_mouse = false;
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (state_widget_hovered[i]) {
            state_widget_has_mouse = true;
            break;
        }
    }
    if (ImGui::IsWindowHovered() && interaction == Interaction::idle && in_widget(mouse_screen) &&
        io.MouseWheel != 0.0f && ! state_widget_has_mouse) {
        const float       scaled      = zoom_scale * powf(zoom_wheel_factor, io.MouseWheel);
        const float       new_zoom    = scaled < graph_min_zoom   ? graph_min_zoom
                                        : scaled > graph_max_zoom ? graph_max_zoom
                                                                  : scaled;
        const vmath::vec2 mouse_point = mouse_graph();
        view_origin                   = (mouse_point - ((mouse_screen - origin) / new_zoom));
        zoom                          = new_zoom;
    }

    // Home or "." fits all nodes into view.  Focus gates the shortcut so the
    // pointer does not need to hover the canvas; hover still works before the
    // first click gives the window focus.  Blocked while typing elsewhere.
    if (! io.WantTextInput && (ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) &&
        (ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_Period))) {
        bool  have_bbox      = false;
        float min_x          = 0.0f;
        float min_y          = 0.0f;
        float content_right  = 0.0f;
        float content_bottom = 0.0f;
        for (uint32_t i = 0; i < max_nodes; ++i) {
            if (! nodes.is_occupied(i) || nodes.entries[i].ghost) {
                continue;
            }
            const vmath::vec2 position(nodes.entries[i].position);
            const vmath::vec2 node_max(position + content_sizes[i]);
            if (! have_bbox) {
                have_bbox      = true;
                min_x          = position.x;
                min_y          = position.y;
                content_right  = node_max.x;
                content_bottom = node_max.y;
                continue;
            }
            min_x          = position.x < min_x ? position.x : min_x;
            min_y          = position.y < min_y ? position.y : min_y;
            content_right  = node_max.x > content_right ? node_max.x : content_right;
            content_bottom = node_max.y > content_bottom ? node_max.y : content_bottom;
        }
        if (have_bbox) {
            // Everything scales with zoom, so one bbox solve fits the graph.
            const float z_hi_x = (widget_size.x - 2.0f * fit_view_margin) / (content_right - min_x);
            const float z_hi_y = (widget_size.y - 2.0f * fit_view_margin) / (content_bottom - min_y);
            const float z_fit  = z_hi_x < z_hi_y ? z_hi_x : z_hi_y;
            float       fitted = z_fit > graph_max_zoom ? graph_max_zoom : z_fit;
            if (fitted < graph_min_zoom) {
                fitted = graph_min_zoom;
            }
            // Screen position of a graph point is origin + (g - view_origin) * zoom,
            // so the visual bbox must be centered with view_origin = center - widget / (2 * zoom).
            const float eff       = 1.0f / fitted;
            float       vis_max_x = -FLT_MAX;
            float       vis_max_y = -FLT_MAX;
            for (uint32_t i = 0; i < max_nodes; ++i) {
                if (! nodes.is_occupied(i) || nodes.entries[i].ghost) {
                    continue;
                }
                const vmath::vec2 extent(nodes.entries[i].position + content_sizes[i] * eff);
                vis_max_x = extent.x > vis_max_x ? extent.x : vis_max_x;
                vis_max_y = extent.y > vis_max_y ? extent.y : vis_max_y;
            }
            zoom = fitted;
            const vmath::vec2 center((min_x + vis_max_x) * 0.5f, (min_y + vis_max_y) * 0.5f);
            view_origin = (center - (widget_size / (2.0f * fitted)));
        }
    }

    // Grid, aligned to graph space, stronger line every N cells.
    {
        const int32_t first_col = static_cast<int32_t>(floor(view_origin.x / graph_grid_spacing));
        const int32_t last_col =
            static_cast<int32_t>(floor((view_origin.x + widget_size.x / zoom_scale) / graph_grid_spacing));
        for (int32_t col = first_col; col <= last_col; ++col) {
            const float gx    = static_cast<float>(col) * graph_grid_spacing;
            const float sx    = origin.x + (gx - view_origin.x) * zoom_scale;
            const bool  axis  = (col % graph_grid_axis_cells) == 0;
            const ImU32 color = to_imgui(axis ? colors_.grid_axis : colors_.grid_line);
            draw_list->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + widget_size.y), color);
        }
        const int32_t first_row = static_cast<int32_t>(floor(view_origin.y / graph_grid_spacing));
        const int32_t last_row =
            static_cast<int32_t>(floor((view_origin.y + widget_size.y / zoom_scale) / graph_grid_spacing));
        for (int32_t row = first_row; row <= last_row; ++row) {
            const float gy    = static_cast<float>(row) * graph_grid_spacing;
            const float sy    = origin.y + (gy - view_origin.y) * zoom_scale;
            const bool  axis  = (row % graph_grid_axis_cells) == 0;
            const ImU32 color = to_imgui(axis ? colors_.grid_axis : colors_.grid_line);
            draw_list->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + widget_size.x, sy), color);
        }
    }

    // Hit tracking for the press handler at the end: topmost node wins, so
    // later nodes overwrite earlier hits.
    bool     node_hit  = false;
    uint32_t hit_node  = pool_no_slot;
    bool     title_hit = false;

    // Two draw channels: connections go on channel 0 (background), nodes and
    // their widgets on channel 1, so lines never paint over node bodies.
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);

    // Dot under the mouse, from the previous frame's recorded positions
    // (1-frame lag, same accepted trade-off as the state-widget heights).
    // Ghost nodes expose no live dots and non-connectable property slots
    // have no dot at all.
    const auto dot_at = [&](vmath::vec2 pos_screen) {
        EndPoint result;
        result.node_idx = pool_no_slot;
        result.slot_idx = pool_no_slot;
        float best_sq   = dot_pick_radius * dot_pick_radius;
        for (uint32_t n = 0; n < max_nodes; ++n) {
            if (! nodes.is_occupied(n) || nodes.entries[n].ghost) {
                continue;
            }
            for (uint32_t s = 0; s < max_node_slots; ++s) {
                if (! nodes.entries[n].slots.is_occupied(s)) {
                    continue;
                }
                const Slot& slot    = nodes.entries[n].slots.entries[s];
                const bool  has_dot = slot.kind == SlotKind::input || slot.kind == SlotKind::output ||
                                      (slot.kind == SlotKind::property && slot.connectable);
                if (! has_dot) {
                    continue;
                }
                const vmath::vec2 d       = dot_positions[n * max_node_slots + s];
                const float       dx      = pos_screen.x - d.x;
                const float       dy      = pos_screen.y - d.y;
                const float       dist_sq = dx * dx + dy * dy;
                if (dist_sq <= best_sq) {
                    best_sq         = dist_sq;
                    result.node_idx = n;
                    result.slot_idx = s;
                }
            }
        }
        return result;
    };

    const float line_h  = ImGui::GetTextLineHeightWithSpacing();
    const float frame_h = ImGui::GetFrameHeight();

    for (uint32_t node_idx = 0; node_idx < max_nodes; ++node_idx) {
        if (! nodes.is_occupied(node_idx)) {
            state_widget_hovered[node_idx] = false;
            continue;
        }
        Node&      node     = nodes.entries[node_idx];
        const bool is_ghost = node.ghost;

        // Layout: measure content (graph space, zoom applied when drawing).
        const float pad        = node_padding;
        const float title_h    = line_h * render_scale;
        float       content_h  = pad + title_h;
        float       max_line_w = ImGui::CalcTextSize(node.name).x;

        // Lines: every slot belongs to the line of its row_group (0 = own
        // line).  Same-group slots pack onto one line in slot order, so a
        // connector and its inline widgets share a row.
        struct LineElem {
            uint32_t slot_idx;
            bool     dot; // draws a connector dot
        };

        struct Line {
            LineElem elems[max_node_slots];
            uint32_t num_elems  = 0;
            float    y          = 0.0f; // top of the line, graph space
            bool     has_widget = false;
        };

        Line    lines[max_node_slots];
        int32_t group_line[256]; // row_group id -> line index, -1 = none yet
        for (uint32_t g = 0; g < 256; ++g) {
            group_line[g] = -1;
        }
        uint32_t num_lines = 0;

        for (uint32_t slot_idx = 0; slot_idx < max_node_slots; ++slot_idx) {
            if (! node.slots.is_occupied(slot_idx)) {
                continue;
            }
            const Slot& slot = node.slots.entries[slot_idx];
            uint32_t    line_idx;
            if (slot.row_group != 0 && group_line[slot.row_group] >= 0) {
                line_idx = static_cast<uint32_t>(group_line[slot.row_group]);
            }
            else {
                line_idx = num_lines++;
                if (slot.row_group != 0) {
                    group_line[slot.row_group] = static_cast<int32_t>(line_idx);
                }
                lines[line_idx].num_elems  = 0;
                lines[line_idx].has_widget = false;
            }
            Line& line                          = lines[line_idx];
            line.elems[line.num_elems].slot_idx = slot_idx;
            line.elems[line.num_elems].dot      = slot.kind == SlotKind::input || slot.kind == SlotKind::output ||
                                                  (slot.kind == SlotKind::property && slot.connectable);
            ++line.num_elems;
            if (slot.kind == SlotKind::property) {
                line.has_widget = true;
            }
        }

        // Widget columns: the k-th inline property widget of every line
        // aligns at the widest line's offset, so values line up across
        // rows instead of trailing their labels.  Names keep their
        // flowing position.
        float value_col[8] = {};
        float max_col_end  = 0.0f;
        for (uint32_t pass_idx = 0; pass_idx < 8; ++pass_idx) {
            bool columns_changed = false;
            for (uint32_t line_idx = 0; line_idx < num_lines; ++line_idx) {
                const Line& line      = lines[line_idx];
                uint32_t    left_dots = 0;
                for (uint32_t e = 0; e < line.num_elems; ++e) {
                    const Slot& slot = node.slots.entries[line.elems[e].slot_idx];
                    if (line.elems[e].dot && slot.kind != SlotKind::output) {
                        ++left_dots;
                    }
                }
                float    cursor = pad + static_cast<float>(left_dots > 0 ? left_dots : 1) * dot_space;
                uint32_t prop   = 0;
                for (uint32_t e = 0; e < line.num_elems; ++e) {
                    const Slot& slot = node.slots.entries[line.elems[e].slot_idx];
                    if (slot.kind == SlotKind::output) {
                        continue;
                    }
                    if (slot.kind != SlotKind::property) {
                        cursor += ImGui::CalcTextSize(slot.name).x + 8.0f;
                        continue;
                    }
                    const float x = cursor + ImGui::CalcTextSize(slot.name).x + 8.0f;
                    // Chain from the aligned column exactly like the draw pass, so
                    // no widget covers its label and the measured widths agree
                    // with what is drawn.
                    float widget_x = x;
                    if (prop < 8) {
                        if (value_col[prop] < x) {
                            widget_x        = x;
                            columns_changed = true;
                        }
                        else {
                            widget_x = value_col[prop];
                        }
                        value_col[prop] = widget_x;
                    }
                    const float col_end = widget_x + slot_widget_w(slot);
                    max_col_end         = col_end > max_col_end ? col_end : max_col_end;
                    ++prop;
                    cursor = widget_x + slot_widget_w(slot);
                }
            }
            if (! columns_changed) {
                break;
            }
        }
        if (max_col_end > 0.0f) {
            max_line_w = max_col_end > max_line_w ? max_col_end : max_line_w;
        }

        // Line sizing: dot columns plus each element's name; property
        // elements add their value widget.  All lines get frame height plus
        // half padding so connector rows breathe as loosely as widget rows.
        for (uint32_t line_idx = 0; line_idx < num_lines; ++line_idx) {
            Line& line   = lines[line_idx];
            float line_w = 0.0f;
            for (uint32_t e = 0; e < line.num_elems; ++e) {
                const Slot& slot = node.slots.entries[line.elems[e].slot_idx];
                if (line.elems[e].dot) {
                    line_w += dot_space;
                }
                line_w += ImGui::CalcTextSize(slot.name).x;
                if (slot.kind == SlotKind::property) {
                    line_w += 8.0f + slot_widget_w(slot);
                }
            }
            max_line_w = line_w > max_line_w ? line_w : max_line_w;
            line.y     = content_h;
            content_h  = content_h + frame_h + 0.5f * node_padding;
        }

        const float state_h = node.state_widget ? state_widget_heights[node_idx] : 0.0f;
        // An explicit width override (equal-width command) wins over the
        // automatic content-driven width.
        const float content_w =
            node.content_width_override > 0.0f ? node.content_width_override : 2.0f * pad + max_line_w;
        content_h += state_h + pad;
        // An explicit height override (equal-height command) wins over the
        // automatic content-driven height; the extra space renders below.
        const float node_h      = node.content_height_override > 0.0f ? node.content_height_override : content_h;
        content_sizes[node_idx] = vmath::vec2(content_w, node_h);

        const vmath::vec2 rect_min = (origin + ((node.position - view_origin) * zoom_scale));
        // The drawn rect must use the override-aware height, not the auto
        // content height, or equal-height renders as nothing and bottom/
        // right aligns land short of the reference edge.
        const vmath::vec2 rect_max    = (rect_min + (vmath::vec2(content_w, node_h) * render_scale));
        const uint8_t     visual_role = node_visual_roles[node_idx];
        const ImU32 bg_color = to_imgui(is_ghost                   ? colors_.ghost_node
                                        : node.color_override != 0 ? node.color_override
                                        : visual_role == node_role_parameter && colors_.parameter_node_background != 0
                                            ? colors_.parameter_node_background
                                            : colors_.node_background);
        draw_list->AddRectFilled(ImVec2(rect_min.x, rect_min.y),
                                 ImVec2(rect_max.x, rect_max.y),
                                 bg_color,
                                 node_corner_rounding * render_scale);
        draw_list->AddRect(ImVec2(rect_min.x, rect_min.y),
                           ImVec2(rect_max.x, rect_max.y),
                           to_imgui(visual_role == node_role_parameter && colors_.parameter_node_border != 0
                                        ? colors_.parameter_node_border
                                        : colors_.node_border),
                           node_corner_rounding * render_scale);
        if (is_selected(node_idx)) {
            draw_list->AddRect(ImVec2(rect_min.x - 2.0f, rect_min.y - 2.0f),
                               ImVec2(rect_max.x + 2.0f, rect_max.y + 2.0f),
                               to_imgui(colors_.node_selected_border),
                               node_corner_rounding * render_scale,
                               0,
                               2.0f);
        }

        const vmath::vec2 title_pos = vmath::vec2(rect_min.x + pad * render_scale, rect_min.y + pad * render_scale);
        add_text_scaled(draw_list,
                        title_pos,
                        render_scale,
                        to_imgui(is_ghost ? colors_.ghost_node : colors_.node_title),
                        node.name);
        draw_list->AddLine(ImVec2(rect_min.x, rect_min.y + title_h + pad * render_scale),
                           ImVec2(rect_max.x, rect_min.y + title_h + pad * render_scale),
                           to_imgui(colors_.node_border));

        // Rename editor replaces the title text while active.
        if (renaming_node == node_idx && ! is_ghost) {
            if (renaming_focus) {
                ImGui::SetKeyboardFocusHere();
                renaming_focus = false;
            }
            ImGui::SetCursorScreenPos(ImVec2(title_pos.x, title_pos.y));
            const float previous_font_scale = push_zoom_font(render_scale);
            ImGui::PushItemWidth((rect_max.x - rect_min.x) - 2.0f * pad * render_scale);
            ImGui::PushID(static_cast<int>(node_idx));
            ImGui::InputText("##name", node.name, sizeof(node.name));
            if (ImGui::IsItemDeactivated()) {
                // Enter and focus loss commit; Esc reverts the text in ImGui,
                // so the event fires only for actual edits.
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    push_change(ChangeKind::name_changed, node_idx, pool_no_slot, pool_no_slot);
                }
                interaction   = Interaction::idle;
                renaming_node = pool_no_slot;
            }
            ImGui::PopItemWidth();
            pop_zoom_font(previous_font_scale);
            ImGui::PopID();
        }

        // Line drawing: input and connectable-property dots pack at the
        // left node edge, output dots sit on the right edge; element names
        // and value widgets then run left to right.  A missing mark paints
        // the free dot red: the editor flags inputs whose expected
        // connection is absent (a broken oscillator sum edge).
        for (uint32_t line_idx = 0; line_idx < num_lines; ++line_idx) {
            const Line& line      = lines[line_idx];
            const float line_span = frame_h + 0.5f * node_padding;
            const float y_center  = rect_min.y + (line.y + 0.5f * line_span) * render_scale;

            uint32_t left_dots  = 0;
            uint32_t right_dots = 0;
            for (uint32_t e = 0; e < line.num_elems; ++e) {
                const uint32_t elem_slot = line.elems[e].slot_idx;
                if (! line.elems[e].dot) {
                    continue;
                }
                const Slot& slot      = node.slots.entries[elem_slot];
                const bool  connected = slot_is_connected(node_idx, elem_slot);
                const float dot_x     = slot.kind == SlotKind::output
                                            ? rect_max.x - static_cast<float>(right_dots++) * dot_space * render_scale
                                            : rect_min.x + static_cast<float>(left_dots++) * dot_space * render_scale;
                const ImU32 dot_color = to_imgui(connected                           ? colors_.connector_connected
                                                 : slot_missing(node_idx, elem_slot) ? colors_.connector_missing
                                                                                     : colors_.connector);
                dot_positions[node_idx * max_node_slots + elem_slot] = vmath::vec2(dot_x, y_center);
                if (connected) {
                    draw_list->AddCircleFilled(ImVec2(dot_x, y_center), dot_radius * render_scale, dot_color);
                }
                else {
                    draw_list->AddCircle(ImVec2(dot_x, y_center), dot_radius * render_scale, dot_color, 0, 1.5f);
                }
                if (slot.kind == SlotKind::output) {
                    // Output names right-align against their dot so the
                    // label sits inside the node next to the connector.
                    const float name_w = ImGui::CalcTextSize(slot.name).x * render_scale;
                    add_text_scaled(
                        draw_list,
                        vmath::vec2(dot_x - pad * render_scale - name_w, y_center - line_h * render_scale * 0.5f),
                        render_scale,
                        to_imgui(colors_.property_value),
                        slot.name);
                }
            }

            // Shared routing rows (parameter value row + source op/scale
            // rows) carry a marker so the fan-out target is visible at a
            // glance; one glyph per rendered line.
            bool line_shared = false;
            for (uint32_t e = 0; e < line.num_elems; ++e) {
                if (slot_visual_roles[node_idx * max_node_slots + line.elems[e].slot_idx] == slot_role_shared_row) {
                    line_shared = true;
                    break;
                }
            }

            // A line with no input dots still starts its names one dot
            // column in, matching the input-tagged lines beside it.
            float cursor =
                rect_min.x + (pad + static_cast<float>(left_dots > 0 ? left_dots : 1) * dot_space) * render_scale;
            uint32_t line_prop = 0;
            for (uint32_t e = 0; e < line.num_elems; ++e) {
                const uint32_t slot_idx = line.elems[e].slot_idx;
                const Slot&    slot     = node.slots.entries[slot_idx];
                if (slot.kind == SlotKind::output) {
                    continue; // name drawn beside its dot above
                }
                const float name_w = ImGui::CalcTextSize(slot.name).x * render_scale;
                if (line_shared) {
                    line_shared = false;
                    if (colors_.shared_row_marker != 0) {
                        draw_list->AddCircleFilled(ImVec2(cursor - 4.0f * render_scale, y_center),
                                                   2.0f * render_scale,
                                                   to_imgui(colors_.shared_row_marker));
                    }
                }
                const bool disabled = slot.kind == SlotKind::property &&
                                      (osc_slot_disabled(node, slot_idx) || slot_edit_disabled(node_idx, slot_idx));
                add_text_scaled(draw_list,
                                vmath::vec2(cursor, y_center - line_h * render_scale * 0.5f),
                                render_scale,
                                to_imgui(disabled ? colors_.property_connected_value : colors_.property_value),
                                slot.name);
                if (slot.kind != SlotKind::property) {
                    cursor += name_w + 8.0f * render_scale;
                    continue;
                }
                const float min_widget_x = cursor + name_w + 8.0f * render_scale;
                float       widget_x = line_prop < 8 ? rect_min.x + value_col[line_prop] * render_scale : min_widget_x;
                if (widget_x < min_widget_x) {
                    widget_x = min_widget_x;
                }
                const float widget_y = y_center - frame_h * render_scale * 0.5f;
                cursor               = widget_x + slot_widget_w(slot) * render_scale;
                ++line_prop;
                {
                    const bool connected = slot_is_connected(node_idx, slot_idx);
                    if (connected || is_ghost) {
                        // Greyed-out value text: the connection drives the value,
                        // and ghosts submit no live widgets at all.
                        char value_text[32] = {};
                        if (slot.property_type == PropertyType::integer) {
                            snprintf(value_text, sizeof(value_text), "%d", slot.value.integer);
                        }
                        else if (slot.property_type == PropertyType::real) {
                            snprintf(value_text, sizeof(value_text), "%.3f", static_cast<double>(slot.value.real));
                        }
                        else {
                            snprintf(value_text, sizeof(value_text), "%u", slot.value.list_index);
                        }
                        add_text_scaled(draw_list,
                                        vmath::vec2(widget_x, widget_y + (frame_h - line_h) * render_scale * 0.5f),
                                        render_scale,
                                        to_imgui(colors_.property_connected_value),
                                        value_text);
                        continue;
                    }

                    ImGui::PushID(static_cast<int>(node_idx * max_node_slots + slot_idx));
                    const float widget_width        = slot_widget_w(slot) * render_scale;
                    const float previous_font_scale = push_zoom_font(render_scale);
                    ImGui::SetCursorScreenPos(ImVec2(widget_x, widget_y));
                    ImGui::PushItemWidth(widget_width);
                    if (disabled) {
                        ImGui::BeginDisabled(true);
                    }
                    bool item_submitted = false;
                    bool value_edited   = false;
                    switch (slot.property_type) {
                        case PropertyType::integer:
                            value_edited   = ImGui::InputInt("##value", &node.slots.entries[slot_idx].value.integer);
                            item_submitted = true;
                            break;
                        case PropertyType::real:
                            if (slot.real_bounded) {
                                value_edited = ImGui::SliderFloat("##value",
                                                                  &node.slots.entries[slot_idx].value.real,
                                                                  slot.real_min,
                                                                  slot.real_max,
                                                                  "%.3f",
                                                                  slot.real_logarithmic ? ImGuiSliderFlags_Logarithmic
                                                                                        : ImGuiSliderFlags_None);
                            }
                            else {
                                value_edited = ImGui::InputFloat("##value", &node.slots.entries[slot_idx].value.real);
                            }
                            item_submitted = true;
                            break;
                        case PropertyType::list: {
                            const char* items[8] = {};
                            for (uint8_t option = 0; option < slot.num_list_options && option < 8; ++option) {
                                items[option] = slot.list_options[option];
                            }
                            int list_index = static_cast<int>(slot.value.list_index);
                            // num_list_options is an unconstrained public field;
                            // clamp so Combo never reads past the items array.
                            const int num_items =
                                static_cast<int>(slot.num_list_options < 8 ? slot.num_list_options : 8);
                            if (num_items == 2) {
                                // Two-option lists are mode selectors (add vs
                                // multiply): a horizontal radio pair reads
                                // faster than a dropdown, with the option
                                // label ahead of its button.
                                int radio_index = list_index;
                                for (int option_i = 0; option_i < 2; ++option_i) {
                                    if (option_i > 0) {
                                        ImGui::SameLine();
                                    }
                                    ImGui::AlignTextToFramePadding();
                                    ImGui::TextUnformatted(items[option_i]);
                                    ImGui::SameLine();
                                    ImGui::RadioButton(option_i == 0 ? "##op0" : "##op1", &radio_index, option_i);
                                }
                                if (radio_index != list_index) {
                                    node.slots.entries[slot_idx].value.list_index = static_cast<uint8_t>(radio_index);
                                    value_edited                                  = true;
                                }
                                item_submitted = true;
                                break;
                            }
                            value_edited = ImGui::Combo("##value", &list_index, items, num_items);
                            if (value_edited) {
                                node.slots.entries[slot_idx].value.list_index = static_cast<uint8_t>(list_index);
                            }
                            break;
                        }
                        case PropertyType::unused:
                            // A SetCursorScreenPos that lands outside the window
                            // bounds must be followed by an item, or ImGui's
                            // boundary-extent error check fires on the next move.
                            ImGui::Dummy(ImVec2(property_widget_w * render_scale, ImGui::GetTextLineHeight()));
                            item_submitted = true;
                            break;
                    }
                    if (! item_submitted) {
                        ImGui::Dummy(ImVec2(property_widget_w * render_scale, ImGui::GetTextLineHeight()));
                    }
                    if (value_edited) {
                        push_change(ChangeKind::value_changed, node_idx, slot_idx, pool_no_slot);
                    }
                    if (disabled) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopItemWidth();
                    pop_zoom_font(previous_font_scale);
                    ImGui::PopID();
                }
            }
        }

        // Optional caller state widget at the bottom of the node.
        if (node.state_widget && ! is_ghost) {
            ImGui::PushID(static_cast<int>(node_idx) + 100000);
            ImGui::SetCursorScreenPos(
                ImVec2(rect_min.x + pad * render_scale, rect_max.y - (pad + state_h) * render_scale));
            ImGui::PushClipRect(ImVec2(rect_min.x, rect_min.y), ImVec2(rect_max.x, rect_max.y), true);
            const float previous_font_scale = push_zoom_font(render_scale);
            const int   widget_h            = node.state_widget(node.state_widget_data, render_scale);
            pop_zoom_font(previous_font_scale);
            ImGui::PopClipRect();
            state_widget_heights[node_idx] = widget_h > 0 ? static_cast<float>(widget_h) / render_scale : 0.0f;
            state_widget_hovered[node_idx] =
                in_rect(mouse_screen,
                        vmath::vec2(rect_min.x + pad * render_scale, rect_max.y - (pad + state_h) * render_scale),
                        vmath::vec2(rect_max.x - pad * render_scale, rect_max.y - pad * render_scale));
            ImGui::PopID();
        }

        // Hit tracking: topmost (latest drawn) node wins.  Ghost nodes are
        // placed by the click handler below, never dragged.
        if (! is_ghost && in_rect(mouse_screen, rect_min, rect_max)) {
            node_hit  = true;
            hit_node  = node_idx;
            title_hit = mouse_screen.y <= rect_min.y + title_h + pad * render_scale;
        }
    }

    // Connections on the background channel, so they always sit under the
    // nodes drawn on channel 1 above.
    draw_list->ChannelsSetCurrent(0);
    for (uint32_t c = 0; c < max_connections; ++c) {
        if (! connections.is_occupied(c)) {
            continue;
        }
        const Connection& connection = connections.entries[c];
        const vmath::vec2 p0 = dot_positions[connection.output.node_idx * max_node_slots + connection.output.slot_idx];
        const vmath::vec2 p3 = dot_positions[connection.input.node_idx * max_node_slots + connection.input.slot_idx];
        const float dx = fabsf(p3.x - p0.x) * 0.5f > curve_min_reach ? fabsf(p3.x - p0.x) * 0.5f : curve_min_reach;
        const vmath::vec2 p1(p0.x + dx, p0.y);
        const vmath::vec2 p2(p3.x - dx, p3.y);
        // Cubic bezier point at t = 0.5.
        const vmath::vec2 mid(
            vmath::vec2(p0.x + 3.0f * p1.x + 3.0f * p2.x + p3.x, p0.y + 3.0f * p1.y + 3.0f * p2.y + p3.y) *
            (1.0f / 8.0f));

        const float mid_dx = mouse_screen.x - mid.x;
        const float mid_dy = mouse_screen.y - mid.y;
        const bool  mid_hovered =
            in_widget(mouse_screen) && mid_dx * mid_dx + mid_dy * mid_dy <= dot_pick_radius * dot_pick_radius;
        const ImU32 line_color = to_imgui(mid_hovered ? colors_.connector_hover : colors_.connection);
        draw_list->AddBezierCubic(ImVec2(p0.x, p0.y),
                                  ImVec2(p1.x, p1.y),
                                  ImVec2(p2.x, p2.y),
                                  ImVec2(p3.x, p3.y),
                                  line_color,
                                  1.5f);
        draw_list->AddCircleFilled(ImVec2(mid.x, mid.y), (dot_radius * render_scale) * 0.6f, line_color);

        if (mid_hovered && ! node_hit && ImGui::IsMouseClicked(1)) {

            popup_connection = c;
            ImGui::OpenPopup("connection_menu");
        }
    }

    // In-progress line: anchor dot to the mouse, hollow dot at the mouse.
    // A stale retarget anchor (connection deleted mid-drag) draws nothing.
    const bool retarget_alive =
        interaction != Interaction::retargeting ||
        (retarget_connection < max_connections && connections.is_occupied(retarget_connection) &&
         nodes.is_occupied(connecting_from.node_idx));
    if ((interaction == Interaction::connecting || (interaction == Interaction::retargeting && retarget_alive)) &&
        connecting_from.node_idx < max_nodes) {
        const vmath::vec2 p0 = dot_positions[connecting_from.node_idx * max_node_slots + connecting_from.slot_idx];
        const vmath::vec2 p3 = mouse_screen;
        const float dx = fabsf(p3.x - p0.x) * 0.5f > curve_min_reach ? fabsf(p3.x - p0.x) * 0.5f : curve_min_reach;
        // The curve leaves an output dot rightward, an input or property dot
        // leftward, so dragging from an input bends away from its node.
        const Slot&       anchor_slot = nodes.entries[connecting_from.node_idx].slots.entries[connecting_from.slot_idx];
        const bool        anchor_is_output = anchor_slot.kind == SlotKind::output;
        const vmath::vec2 p1(anchor_is_output ? p0.x + dx : p0.x - dx, p0.y);
        const vmath::vec2 p2(anchor_is_output ? p3.x - dx : p3.x + dx, p3.y);
        draw_list->AddBezierCubic(ImVec2(p0.x, p0.y),
                                  ImVec2(p1.x, p1.y),
                                  ImVec2(p2.x, p2.y),
                                  ImVec2(p3.x, p3.y),
                                  to_imgui(colors_.connector_hover),
                                  1.5f);
        draw_list->AddCircle(ImVec2(p3.x, p3.y), dot_radius, to_imgui(colors_.connector_hover), 0, 1.5f);
    }

    // Right-click menu on a hovered middle dot.
    if (ImGui::BeginPopup("connection_menu")) {
        if (ImGui::MenuItem("Delete") && popup_connection < max_connections &&
            connections.is_occupied(popup_connection)) {
            delete_connection(popup_connection);
        }
        ImGui::EndPopup();
    }

    // Right-click menu on a selected node: caller items first, then align,
    // equal-size and delete commands.  Delete runs the caller veto: a refused
    // delete mutates nothing and is reported through the error overlay.
    if (ImGui::BeginPopup("node_menu")) {
        if (node_menu_callback && node_menu_callback(node_menu_user_data, popup_node)) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Align left")) {
            align_selected(AlignKind::left);
        }
        if (ImGui::MenuItem("Align right")) {
            align_selected(AlignKind::right);
        }
        if (ImGui::MenuItem("Align top")) {
            align_selected(AlignKind::top);
        }
        if (ImGui::MenuItem("Align bottom")) {
            align_selected(AlignKind::bottom);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Equal width")) {
            align_selected(AlignKind::equal_width);
        }
        if (ImGui::MenuItem("Equal height")) {
            align_selected(AlignKind::equal_height);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete") && popup_node < max_nodes && nodes.is_occupied(popup_node)) {
            delete_node(popup_node);
            popup_node = pool_no_slot;
        }
        ImGui::EndPopup();
    }

    // Right-click menu on the empty canvas: fully caller-provided items; the
    // widget adds none of its own (the caller places added nodes itself).
    if (ImGui::BeginPopup("canvas_menu")) {
        if (canvas_menu_callback) {
            canvas_menu_callback(canvas_menu_user_data);
        }
        ImGui::EndPopup();
    }

    draw_list->ChannelsMerge();

    // Rubber band while active (screen space so it stays crisp while panning
    // is impossible anyway: zoom is frozen during interactions).
    if (interaction == Interaction::rubber_band) {
        const vmath::vec2 band_now   = mouse_graph();
        const float       band_min_x = band_start.x < band_now.x ? band_start.x : band_now.x;
        const float       band_min_y = band_start.y < band_now.y ? band_start.y : band_now.y;
        const float       band_max_x = band_start.x > band_now.x ? band_start.x : band_now.x;
        const float       band_max_y = band_start.y > band_now.y ? band_start.y : band_now.y;
        const vmath::vec2 scr_min    = (origin + ((vmath::vec2(band_min_x, band_min_y) - view_origin) * zoom_scale));
        const vmath::vec2 scr_max    = (origin + ((vmath::vec2(band_max_x, band_max_y) - view_origin) * zoom_scale));
        draw_list->AddRectFilled(ImVec2(scr_min.x, scr_min.y),
                                 ImVec2(scr_max.x, scr_max.y),
                                 to_imgui(colors_.selection_band));
        draw_list->AddRect(ImVec2(scr_min.x, scr_min.y),
                           ImVec2(scr_max.x, scr_max.y),
                           to_imgui(colors_.selection_outline));
    }

    // Error overlay, top-right of the widget area; Esc dismisses (handled in
    // the key handling below).
    if (error_active) {
        const char* text    = error_message;
        const float text_w  = ImGui::CalcTextSize(text).x;
        const float text_h  = ImGui::GetTextLineHeight();
        const float box_pad = 8.0f;
        // Anchor the box inside the top-right corner of the widget area; it
        // grows downward, fully inside the clip rect.
        const vmath::vec2 box_size(text_w + 2.0f * box_pad, text_h + 2.0f * box_pad);
        const vmath::vec2 box_min(origin.x + widget_size.x - 8.0f - box_size.x, origin.y + 8.0f);
        const vmath::vec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);
        draw_list->AddRectFilled(ImVec2(box_min.x, box_min.y),
                                 ImVec2(box_max.x, box_max.y),
                                 to_imgui(colors_.error_background),
                                 4.0f);
        draw_list->AddText(ImVec2(box_min.x + box_pad, box_min.y + box_pad), to_imgui(colors_.error_text), text);
    }

    // Active interaction progression.  InputText handles the renaming state;
    // the other modes end on mouse release.
    switch (interaction) {
        case Interaction::dragging_node: {
            if (! io.MouseDown[0]) {
                // A press on the title without movement becomes a rename.
                // node_state_edits_enabled stays off for projections that
                // regenerate every title; a node the projection marks
                // renamable owns its stored title and opts in per node.
                const ImVec2 drag_delta = ImGui::GetMouseDragDelta(0);
                const float  drag_sq    = drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y;
                if (title_pressed && drag_sq <= click_max_distance * click_max_distance &&
                    dragged_node != pool_no_slot &&
                    (node_state_edits_enabled || nodes.entries[dragged_node].renamable)) {
                    interaction    = Interaction::renaming;
                    renaming_node  = dragged_node;
                    renaming_focus = true;
                }
                else {
                    // A plain click (no drag) on an already selected node
                    // collapses the selection to just that node; a real drag
                    // keeps the multi-selection so it can be moved or aligned.
                    if (! title_pressed && drag_sq <= click_max_distance * click_max_distance &&
                        dragged_node != pool_no_slot && is_selected(dragged_node)) {
                        select_none();
                        set_selected(dragged_node, true);
                    }
                    interaction = Interaction::idle;
                }
                title_pressed = false;
                dragged_node  = pool_no_slot;
                break;
            }
            if (dragged_node != pool_no_slot && nodes.is_occupied(dragged_node)) {
                Node&             node = nodes.entries[dragged_node];
                const vmath::vec2 target(snap_to_grid(mouse_graph().x - drag_offset.x),
                                         snap_to_grid(mouse_graph().y - drag_offset.y));
                const vmath::vec2 delta = (target - node.position);
                node.position           = target;
                // Multi-selection: the dragged node leads; the rest follow
                // with the same delta so relative layout is preserved.
                if (delta.x != 0.0f || delta.y != 0.0f) {
                    for (uint32_t i = 0; i < max_nodes; ++i) {
                        if (i != dragged_node && nodes.is_occupied(i) && selected[i] && ! nodes.entries[i].ghost) {
                            nodes.entries[i].position = (nodes.entries[i].position + delta);
                        }
                    }
                }
            }
            break;
        }
        case Interaction::panning: {
            if (! io.MouseDown[0]) {
                interaction = Interaction::idle;
                break;
            }
            view_origin = (view_origin - (vmath::vec2(io.MouseDelta.x, io.MouseDelta.y) / zoom_scale));
            break;
        }
        case Interaction::connecting: {
            if (io.MouseDown[0]) {
                break;
            }
            // Released over a dot: connect in whichever direction the drag
            // goes.  Released elsewhere: cancel, nothing added.
            const EndPoint drop = dot_at(mouse_screen);
            if (drop.node_idx != pool_no_slot) {
                const Slot& from_slot = nodes.entries[connecting_from.node_idx].slots.entries[connecting_from.slot_idx];
                EndPoint    output_end = connecting_from;
                EndPoint    input_end  = drop;
                if (from_slot.kind != SlotKind::output) {
                    output_end = drop;
                    input_end  = connecting_from;
                }
                attempt_connection(output_end, input_end);
            }
            interaction = Interaction::idle;
            break;
        }
        case Interaction::retargeting: {
            if (io.MouseDown[0]) {
                break;
            }
            // The dragged connection may have been deleted mid-drag; never
            // touch a stale pool entry.
            if (retarget_connection >= max_connections || ! connections.is_occupied(retarget_connection)) {
                interaction         = Interaction::idle;
                retarget_connection = pool_no_slot;
                break;
            }
            const EndPoint drop = dot_at(mouse_screen);
            if (drop.node_idx == pool_no_slot) {
                // Released off any connector: the connection is destroyed.
                delete_connection(retarget_connection);
            }
            else {
                const Connection& connection  = connections.entries[retarget_connection];
                const EndPoint    current_end = retarget_output_end ? connection.output : connection.input;
                const bool same_drop = drop.node_idx == current_end.node_idx && drop.slot_idx == current_end.slot_idx;
                // Dropping on the same dot is a no-op, not a retarget.
                if (! same_drop) {
                    move_connection_end(retarget_connection, retarget_output_end, drop);
                }
            }
            interaction         = Interaction::idle;
            retarget_connection = pool_no_slot;
            break;
        }
        case Interaction::rubber_band: {
            if (io.MouseDown[0]) {
                break;
            }
            const vmath::vec2 band_now   = mouse_graph();
            const float       band_min_x = band_start.x < band_now.x ? band_start.x : band_now.x;
            const float       band_min_y = band_start.y < band_now.y ? band_start.y : band_now.y;
            const float       band_max_x = band_start.x > band_now.x ? band_start.x : band_now.x;
            const float       band_max_y = band_start.y > band_now.y ? band_start.y : band_now.y;
            // The band replaces the selection unless Shift is held (which
            // adds to it): Ctrl is the band modifier here because plain drag
            // pans, so without this any earlier stray selection would ride
            // along and skew align references.
            if (! io.KeyShift) {
                select_none();
            }
            const float band_render_scale = zoom_scale;
            for (uint32_t i = 0; i < max_nodes; ++i) {
                if (! nodes.is_occupied(i) || nodes.entries[i].ghost) {
                    continue;
                }
                const vmath::vec2 node_min = nodes.entries[i].position;
                const vmath::vec2 node_max = (node_min + (content_sizes[i] * band_render_scale / zoom_scale));
                if (node_min.x < band_max_x && node_max.x > band_min_x && node_min.y < band_max_y &&
                    node_max.y > band_min_y) {
                    set_selected(i, true);
                }
            }
            interaction = Interaction::idle;
            break;
        }
        case Interaction::renaming:
        case Interaction::idle:
            break;
    }

    // Esc dismisses the error overlay first, then aborts connection drags
    // (a retarget keeps its original endpoints), then drags and panning.
    // Renaming reverts via InputText itself.
    if (ImGui::IsWindowHovered() && in_widget(mouse_screen) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (error_active) {
            dismiss_error();
        }
        else if (interaction == Interaction::connecting || interaction == Interaction::retargeting) {
            interaction         = Interaction::idle;
            retarget_connection = pool_no_slot;
        }
        else if (interaction == Interaction::rubber_band) {
            interaction = Interaction::idle;
        }
        else if (interaction == Interaction::dragging_node || interaction == Interaction::panning) {
            interaction   = Interaction::idle;
            dragged_node  = pool_no_slot;
            title_pressed = false;
        }
    }

    // Ghost resolution: LMB places, Esc or RMB cancels.
    if (ghost_idx != pool_no_slot && ImGui::IsWindowHovered() && in_widget(mouse_screen)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(1)) {
            delete_node(ghost_idx);
        }
        else if (ImGui::IsMouseClicked(0)) {
            set_ghost(ghost_idx, false); // pushes ghost_placed
        }
    }

    // Delete: delete the selected nodes (the veto applies per node).  With
    // an empty selection the hovered node is the target: a state-widget
    // node body is its own items, so a body click never selects the node
    // and the key must act on what is under the mouse.  Only while no
    // interaction is active and without key repeat, so holding or dragging
    // never re-fires the delete mid-gesture.  The text-input guard keeps an
    // active node-rename editor from swallowing the keypress as a delete.
    if (ImGui::IsWindowHovered() && in_widget(mouse_screen) && interaction == Interaction::idle &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) && ! ImGui::GetIO().WantTextInput) {
        const bool deleted_anything = delete_selected() > 0;
        if (! deleted_anything) {
            bool any_selected = false;
            for (uint32_t i = 0; i < max_nodes; ++i) {
                if (nodes.is_occupied(i) && is_selected(i)) {
                    any_selected = true;
                    break;
                }
            }
            if (! any_selected && node_hit && ! nodes.entries[hit_node].ghost) {
                delete_node(hit_node);
            }
        }
    }

    // A right-click inside the widget with no ghost pending opens a popup:
    // on a node the align/equal-size menu (a right-click on an unselected
    // node selects only it; on an already selected node the current,
    // possibly multi-node, selection is kept), on the empty canvas the
    // add-node menu.  Merely hovering an item does not block the menu: a
    // state-widget node body is its own items, so the menu must open over
    // it.  An active item (slider drag, rename editor) still owns the mouse.
    const bool canvas_right_click = interaction == Interaction::idle && ghost_idx == pool_no_slot &&
                                    ImGui::IsWindowHovered() && in_widget(mouse_screen) && ImGui::IsMouseClicked(1) &&
                                    ! ImGui::IsAnyItemActive();
    if (canvas_right_click && node_hit) {
        if (! is_selected(hit_node)) {
            select_none();
            set_selected(hit_node, true);
        }

        popup_node = hit_node;
        ImGui::OpenPopup("node_menu");
    }
    else if (canvas_right_click && ! node_hit) {
        popup_canvas_pos = mouse_graph();

        ImGui::OpenPopup("canvas_menu");
    }

    // New press-starts, only inside the widget rect and when no ghost is
    // pending and no item takes the mouse (property widgets, state widget,
    // rename editor).
    else if (interaction == Interaction::idle && ImGui::IsWindowHovered() && in_widget(mouse_screen) &&
             ImGui::IsMouseClicked(0) && ! ImGui::IsAnyItemHovered()) {
        const EndPoint pressed_dot = dot_at(mouse_screen);
        if (pressed_dot.node_idx != pool_no_slot) {
            // A connected dot picks up its existing connection, a free dot
            // starts a new one.  ponytail: fan-out pickup takes the first
            // match; revisit only if re-dragging one of many ever matters.
            uint32_t existing        = pool_no_slot;
            bool     dragging_output = false;
            for (uint32_t c = 0; c < max_connections; ++c) {
                if (! connections.is_occupied(c)) {
                    continue;
                }
                const Connection& connection = connections.entries[c];
                if (connection.input.node_idx == pressed_dot.node_idx &&
                    connection.input.slot_idx == pressed_dot.slot_idx) {
                    existing        = c;
                    dragging_output = false;
                    break;
                }
                if (connection.output.node_idx == pressed_dot.node_idx &&
                    connection.output.slot_idx == pressed_dot.slot_idx) {
                    existing        = c;
                    dragging_output = true;
                    break;
                }
            }
            if (existing != pool_no_slot) {
                const Connection& connection = connections.entries[existing];
                interaction                  = Interaction::retargeting;
                retarget_connection          = existing;
                retarget_output_end          = dragging_output;
                connecting_from              = dragging_output ? connection.input : connection.output;
            }
            else {
                interaction         = Interaction::connecting;
                retarget_connection = pool_no_slot;
                connecting_from     = pressed_dot;
            }
        }
        else if (node_hit) {
            if (io.KeyShift) {
                // Shift-click toggles the node in or out of the selection
                // without starting a drag.
                set_selected(hit_node, ! is_selected(hit_node));
            }
            else {
                // Plain click on an unselected node selects only it; on an
                // already selected node the multi-selection is kept so the
                // drag below moves all selected nodes.
                if (! is_selected(hit_node)) {
                    select_none();
                    set_selected(hit_node, true);
                }
                interaction   = Interaction::dragging_node;
                dragged_node  = hit_node;
                drag_offset   = (mouse_graph() - nodes.entries[hit_node].position);
                title_pressed = title_hit;
            }
        }
        else if (io.KeyCtrl) {
            interaction = Interaction::rubber_band;
            band_start  = mouse_graph();
        }
        else {
            select_none();
            interaction = Interaction::panning;
        }
    }

    draw_list->PopClipRect();
    ImGui::PopID();

    // Restore the window cursor past the widget so caller code after
    // render() (labels, event readouts) does not draw inside the graph.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + widget_size.y));
    // Validate the extended extent: when the window scrolls, this position
    // is outside the visible region and no caller item may follow it.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    (void)user_data;
}
