// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_envelope_edit.h"

#include "../core/gui_imgui.h"

#include <stdio.h>

namespace {

constexpr float chart_height     = 96.0f;
constexpr float ruler_height     = 14.0f;
constexpr float grab_radius      = 7.0f;
constexpr float point_radius     = 3.5f;
constexpr float dotted_dash      = 3.0f;
constexpr float dotted_gap       = 3.0f;
constexpr float min_span_ms      = 40.0f;
constexpr float scrollbar_height = 4.0f;
// Inner margin the node renderer leaves around node content.
constexpr float node_pad = 8.0f;

constexpr ImU32 curve_color        = IM_COL32(210, 210, 210, 255);
constexpr ImU32 hold_segment_color = IM_COL32(150, 150, 150, 255);
constexpr ImU32 point_color        = IM_COL32(235, 235, 235, 255);
constexpr ImU32 selected_color     = IM_COL32(255, 255, 100, 255);
constexpr ImU32 sustain_fill_color = IM_COL32(255, 180, 40, 36);
constexpr ImU32 sustain_line_color = IM_COL32(255, 190, 60, 255);
constexpr ImU32 point_line_color   = IM_COL32(255, 255, 255, 70);
constexpr ImU32 grid_color         = IM_COL32(255, 255, 255, 26);
constexpr ImU32 chart_bg_color     = IM_COL32(0, 0, 0, 96);
constexpr ImU32 scrollbar_color    = IM_COL32(255, 255, 255, 60);
// Dimmed variants of the colors above, for a chart that cannot be edited.
constexpr ImU32 curve_color_dim        = IM_COL32(210, 210, 210, 90);
constexpr ImU32 hold_segment_color_dim = IM_COL32(150, 150, 150, 70);
constexpr ImU32 point_color_dim        = IM_COL32(235, 235, 235, 90);
constexpr ImU32 selected_color_dim     = IM_COL32(255, 255, 100, 110);
constexpr ImU32 sustain_fill_color_dim = IM_COL32(255, 180, 40, 16);
constexpr ImU32 sustain_line_color_dim = IM_COL32(255, 190, 60, 90);
constexpr ImU32 point_line_color_dim   = IM_COL32(255, 255, 255, 35);

// x axis span shown by the chart: the last point plus breathing room.
float chart_span_ms(const Synth::EnvelopeDescriptor& env)
{
    const float last_ms = Sculptor::envelope_ticks_to_ms(env.points[env.num_points - 1].position);
    const float span    = last_ms * 1.1f;
    return span < min_span_ms ? min_span_ms : span;
}

struct ChartMapping {
    float origin_x;
    float origin_y;
    float width;
    float height;
    float ms_span;
    float ms_scroll;    // model time at the chart's left edge
    float value_bottom; // effective value at the chart's bottom edge
    float value_top;    // effective value at the chart's top edge
};

// x maps model time to screen: ms_scroll sits at the left edge, ms_scroll +
// ms_span at the right.  The y axis spans the full effective value range of
// the descriptor: raw value 0 maps to min_value and raw value 65535 to
// min_value + 65535 * min_max_delta, exactly as the runtime evaluates the
// curve.  A negative scale renders an inverted curve; a zero scale renders a
// flat line.
ChartMapping chart_mapping(const Synth::EnvelopeDescriptor& env,
                           const ImVec2&                    origin,
                           float                            width,
                           float                            ms_span,
                           float                            ms_scroll)
{
    ChartMapping m;
    m.origin_x     = origin.x;
    m.origin_y     = origin.y;
    m.width        = width;
    m.height       = chart_height;
    m.ms_span      = ms_span;
    m.ms_scroll    = ms_scroll;
    const float a  = env.min_value;
    const float b  = env.min_value + 65535.0f * env.min_max_delta;
    m.value_bottom = a < b ? a : b;
    m.value_top    = a < b ? b : a;
    if (m.value_top - m.value_bottom < 0.000001f) {
        // Flat envelope: give the axis a small display range so the curve is
        // a visible line; value editing is disabled in this state.
        m.value_bottom -= 0.5f;
        m.value_top += 0.5f;
    }
    return m;
}

float tick_to_x(const ChartMapping& m, uint16_t position)
{
    return m.origin_x + (Sculptor::envelope_ticks_to_ms(position) - m.ms_scroll) / m.ms_span * m.width;
}

float effective_to_y(const ChartMapping& m, float effective)
{
    const float fraction = (effective - m.value_bottom) / (m.value_top - m.value_bottom);
    return m.origin_y + m.height - fraction * m.height;
}

float point_y(const ChartMapping& m, const Synth::EnvelopeDescriptor& env, uint32_t idx)
{
    const float effective = env.min_value + static_cast<float>(env.points[idx].value) * env.min_max_delta;
    return effective_to_y(m, effective);
}

bool sustain_boundary_point(const Synth::EnvelopeDescriptor& env, uint32_t idx)
{
    return static_cast<uint32_t>(env.sustain_first_point) == idx ||
           static_cast<uint32_t>(env.sustain_last_point) == idx;
}

// Nearest point to the mouse within the grab radius, -1 when none.
int32_t point_at(const ChartMapping& m, const Synth::EnvelopeDescriptor& env, const ImVec2& mouse)
{
    if (mouse.x < m.origin_x - grab_radius || mouse.x > m.origin_x + m.width + grab_radius ||
        mouse.y < m.origin_y - grab_radius || mouse.y > m.origin_y + m.height + grab_radius) {
        return -1;
    }
    int32_t best         = -1;
    float   best_dist_sq = grab_radius * grab_radius;
    for (uint32_t i = 0; i < env.num_points; ++i) {
        const float dx     = mouse.x - tick_to_x(m, env.points[i].position);
        const float dy     = mouse.y - point_y(m, env, i);
        const float distSq = dx * dx + dy * dy;
        if (distSq <= best_dist_sq) {
            best_dist_sq = distSq;
            best         = static_cast<int32_t>(i);
        }
    }
    return best;
}

void draw_dotted_line(ImDrawList* draw_list, float x, float y0, float height, ImU32 color)
{
    for (float y = 0.0f; y < height; y += dotted_dash + dotted_gap) {
        const float y1 = y0 + y + dotted_dash;
        draw_list->AddLine(ImVec2(x, y0 + y), ImVec2(x, y1 < y0 + height ? y1 : y0 + height), color);
    }
}

// Picks a ruler label interval that yields a handful of ticks for the span.
float ruler_step_ms(float span_ms)
{
    const float steps[] = { 1.0f,   2.0f,   5.0f,    10.0f,   20.0f,   50.0f,   100.0f,
                            200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f };
    for (float step : steps) {
        if (span_ms / step <= 8.0f) {
            return step;
        }
    }
    return steps[sizeof(steps) / sizeof(steps[0]) - 1];
}

void draw_ruler(const ChartMapping& m)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float step      = ruler_step_ms(m.ms_span);
    char        label[32];
    for (float ms = 0.0f; ms <= m.ms_scroll + m.ms_span + 0.001f; ms += step) {
        const float x = m.origin_x + (ms - m.ms_scroll) / m.ms_span * m.width;
        if (x < m.origin_x - 0.5f) {
            continue;
        }
        if (x > m.origin_x + m.width + 0.5f) {
            break;
        }
        draw_list->AddLine(ImVec2(x, m.origin_y + m.height), ImVec2(x, m.origin_y + m.height + 3.0f), grid_color);
        snprintf(label, sizeof(label), "%d", static_cast<int>(ms + 0.5f));
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw_list->AddText(ImVec2(x - text_size.x * 0.5f, m.origin_y + m.height + 3.0f),
                           ImGui::GetColorU32(ImGuiCol_Text),
                           label);
        if (m.ms_scroll + m.ms_span - ms < step * 0.5f) {
            break;
        }
    }
}

// Emits an absolute drag target for the grabbed point, derived from the
// mouse position through the model-space mapping frozen at gesture
// activation (screen-space origin and size stay live so node motion during
// the drag is tolerated); the caller applies it to the drag-start
// descriptor.  Freezing the mapping and keeping the grab offset decouples
// the drag from the per-frame commits: a stationary mouse produces a
// stationary point even while dragging the last point, whose position
// defines the live chart span.
void emit_drag_edit(const Sculptor::EnvelopeCurveState* state,
                    const ChartMapping&                 m,
                    const Synth::EnvelopeDescriptor&    env,
                    int32_t                             drag_point,
                    Sculptor::EnvelopeCurveEdit*        out_edit)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float        ms    = (mouse.x - m.origin_x) / m.width * state->gesture_ms_span + state->gesture_scroll_ms -
                         state->gesture_grab_offset_ms;
    if (ms < 0.0f) {
        ms = 0.0f;
    }
    uint16_t value;
    if (state->gesture_min_max_delta == 0.0f) {
        value = env.points[drag_point].value;
    }
    else {
        const float effective =
            state->gesture_value_bottom + (m.origin_y + chart_height - mouse.y) / chart_height *
                                              (state->gesture_value_top - state->gesture_value_bottom);
        float raw = (effective - state->gesture_min_value) / state->gesture_min_max_delta;
        if (raw < 0.0f) {
            raw = 0.0f;
        }
        if (raw > 65535.0f) {
            raw = 65535.0f;
        }
        value = static_cast<uint16_t>(raw + 0.5f);
    }
    out_edit->kind      = Sculptor::EnvelopeEditKind::move;
    out_edit->point_idx = static_cast<uint32_t>(drag_point);
    out_edit->position  = Sculptor::envelope_ms_to_ticks(ms);
    out_edit->value     = value;
}

// Captures the model-space mapping the whole drag gesture uses, so per-frame
// commits and live span changes cannot feed back into the drag target.
void capture_gesture_mapping(Sculptor::EnvelopeCurveState*    state,
                             const Synth::EnvelopeDescriptor& env,
                             const ChartMapping&              m,
                             int32_t                          grabbed)
{
    state->gesture_ms_span        = m.ms_span;
    state->gesture_scroll_ms      = m.ms_scroll;
    state->gesture_value_bottom   = m.value_bottom;
    state->gesture_value_top      = m.value_top;
    state->gesture_min_value      = env.min_value;
    state->gesture_min_max_delta  = env.min_max_delta;
    state->gesture_grab_offset_ms = (ImGui::GetIO().MousePos.x - m.origin_x) / m.width * m.ms_span -
                                    Sculptor::envelope_ticks_to_ms(env.points[grabbed].position);
}

} // namespace

int Sculptor::envelope_widget_height()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return static_cast<int>(chart_height + ruler_height + scrollbar_height + 2.0f * ImGui::GetFrameHeight() +
                            ImGui::GetTextLineHeight() + 4.0f * style.ItemSpacing.y + 1.5f + 0.5f);
}

void Sculptor::gui_envelope_curve(EnvelopeCurveState*              state,
                                  const Synth::EnvelopeDescriptor& env,
                                  bool                             interactive,
                                  EnvelopeCurveEdit*               out_edit,
                                  float                            render_scale,
                                  bool                             dim)
{
    *out_edit         = {};
    const float width = (envelope_node_content_width - 2.0f * node_pad) * render_scale;
    // x view: auto-fit until the user zooms; a set span is clamped into the
    // envelope's extent every frame so shrinking content pulls the view back.
    const float content_span = chart_span_ms(env);
    float       ms_span;
    float       ms_scroll;
    if (state->chart_drag >= 0) {
        // A drag uses the mapping frozen at its activation.
        ms_span   = state->gesture_ms_span;
        ms_scroll = state->gesture_scroll_ms;
    }
    else if (state->view_span_ms > 0.0f) {
        ms_span = state->view_span_ms;
        if (ms_span < min_span_ms) {
            ms_span = min_span_ms;
        }
        if (ms_span > content_span) {
            ms_span = content_span;
        }
        ms_scroll              = state->view_scroll_ms;
        const float max_scroll = content_span - ms_span;
        if (ms_scroll < 0.0f) {
            ms_scroll = 0.0f;
        }
        if (ms_scroll > max_scroll) {
            ms_scroll = max_scroll;
        }
        state->view_span_ms   = ms_span;
        state->view_scroll_ms = ms_scroll;
    }
    else {
        ms_span   = content_span;
        ms_scroll = 0.0f;
    }
    const ImVec2       origin    = ImGui::GetCursorScreenPos();
    ImDrawList*        draw_list = ImGui::GetWindowDrawList();
    const ChartMapping m         = chart_mapping(env, origin, width, ms_span, ms_scroll);
    // Row offsets from the widget anchor.  Every row is placed with an
    // explicit SetCursorScreenPos: after each item ImGui resets the cursor x
    // to the window's line start, so cursor flow would throw the rows onto
    // the canvas's left edge instead of keeping them under the chart.
    const ImGuiStyle& style     = ImGui::GetStyle();
    const float       sp        = style.ItemSpacing.y;
    const float       frame_h   = ImGui::GetFrameHeight();
    const float       text_h    = ImGui::GetTextLineHeight();
    const float       buttons_y = chart_height + sp + ruler_height + 1.0f + scrollbar_height + sp;
    const float       info_y    = buttons_y + frame_h + sp;
    const float       boxes_y   = info_y + text_h + sp;

    // Keep zoomed-out content inside the chart; a small margin keeps edge
    // point handles fully visible.
    draw_list->PushClipRect(ImVec2(m.origin_x - grab_radius, m.origin_y - grab_radius),
                            ImVec2(m.origin_x + m.width + grab_radius, m.origin_y + m.height + grab_radius),
                            true);
    const ImU32 curve_col      = dim ? curve_color_dim : curve_color;
    const ImU32 hold_col       = dim ? hold_segment_color_dim : hold_segment_color;
    const ImU32 point_col      = dim ? point_color_dim : point_color;
    const ImU32 selected_col   = dim ? selected_color_dim : selected_color;
    const ImU32 sustain_fill   = dim ? sustain_fill_color_dim : sustain_fill_color;
    const ImU32 sustain_col    = dim ? sustain_line_color_dim : sustain_line_color;
    const ImU32 point_line_col = dim ? point_line_color_dim : point_line_color;

    draw_list->AddRectFilled(ImVec2(m.origin_x, m.origin_y),
                             ImVec2(m.origin_x + m.width, m.origin_y + m.height),
                             chart_bg_color);

    // Sustain span highlight under everything else.
    const float sustain_x0 = tick_to_x(m, env.points[env.sustain_first_point].position);
    const float sustain_x1 = tick_to_x(m, env.points[env.sustain_last_point].position);
    draw_list->AddRectFilled(ImVec2(sustain_x0, m.origin_y), ImVec2(sustain_x1, m.origin_y + m.height), sustain_fill);

    // Gridlines at ruler intervals.
    const float step = ruler_step_ms(m.ms_span);
    for (float ms = step; ms <= m.ms_scroll + m.ms_span + 0.001f; ms += step) {
        const float x = m.origin_x + (ms - m.ms_scroll) / m.ms_span * m.width;
        if (x < m.origin_x - 0.5f) {
            continue;
        }
        if (x > m.origin_x + m.width + 0.5f) {
            break;
        }
        draw_list->AddLine(ImVec2(x, m.origin_y), ImVec2(x, m.origin_y + m.height), grid_color);
        if (m.ms_scroll + m.ms_span - ms < step * 0.5f) {
            break;
        }
    }

    // Dotted vertical guide per point; sustain boundary points stand out.
    for (uint32_t i = 0; i < env.num_points; ++i) {
        draw_dotted_line(draw_list,
                         tick_to_x(m, env.points[i].position),
                         m.origin_y,
                         m.height,
                         sustain_boundary_point(env, i) ? sustain_col : point_line_col);
    }

    // Tick-0 hold segment when the first point sits past the origin.
    if (env.points[0].position > 0) {
        const float y0 = point_y(m, env, 0);
        draw_list->AddLine(ImVec2(m.origin_x, y0), ImVec2(tick_to_x(m, env.points[0].position), y0), hold_col);
    }
    // Hold at the final value after the last point, as the runtime plays it.
    const float last_x = tick_to_x(m, env.points[env.num_points - 1].position);
    const float last_y = point_y(m, env, env.num_points - 1);
    if (last_x < m.origin_x + m.width) {
        draw_list->AddLine(ImVec2(last_x, last_y), ImVec2(m.origin_x + m.width, last_y), hold_col);
    }

    // Curve through the active points.
    for (uint32_t i = 1; i < env.num_points; ++i) {
        draw_list->AddLine(ImVec2(tick_to_x(m, env.points[i - 1].position), point_y(m, env, i - 1)),
                           ImVec2(tick_to_x(m, env.points[i].position), point_y(m, env, i)),
                           curve_col,
                           1.5f);
    }

    // Hover detection (also drives the info line when not editing).
    state->hover_point = point_at(m, env, ImGui::GetIO().MousePos);

    // The chart is ONE submitted item in both states: presses on it never
    // reach the node drag handler, and a click on an unselected node's chart
    // selects the node eventlessly.
    ImGui::InvisibleButton("##envchart", ImVec2(m.width, m.height));
    if (! interactive) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            out_edit->kind = EnvelopeEditKind::select_node;
        }
    }
    else {
        if (ImGui::IsItemActivated()) {
            const int32_t grabbed = point_at(m, env, ImGui::GetIO().MousePos);
            if (grabbed >= 0) {
                state->selected_point = grabbed;
                state->chart_drag     = grabbed;
                capture_gesture_mapping(state, env, m, grabbed);
            }
        }
        if (ImGui::IsItemActive() && state->chart_drag >= 0) {
            emit_drag_edit(state, m, env, state->chart_drag, out_edit);
        }
        if (ImGui::IsItemDeactivated()) {
            state->chart_drag = -1;
            out_edit->kind    = EnvelopeEditKind::gesture_end;
        }
        // Wheel zooms around the cursor; middle-drag pans.  Both act only
        // over the chart itself so the buttons keep their own hover behavior.
        if (ImGui::IsItemHovered()) {
            ImGuiIO&    io    = ImGui::GetIO();
            const float wheel = io.MouseWheel;
            if (wheel != 0.0f) {
                const float factor   = wheel > 0.0f ? 1.25f : 0.8f;
                const float t        = (io.MousePos.x - m.origin_x) / m.width;
                float       new_span = m.ms_span / factor;
                if (new_span < min_span_ms) {
                    new_span = min_span_ms;
                }
                if (new_span > content_span) {
                    new_span = content_span;
                }
                float new_scroll = m.ms_scroll + t * (m.ms_span - new_span);
                if (new_scroll < 0.0f) {
                    new_scroll = 0.0f;
                }
                const float max_scroll = content_span - new_span;
                if (new_scroll > max_scroll) {
                    new_scroll = max_scroll;
                }
                state->view_span_ms   = new_span;
                state->view_scroll_ms = new_scroll;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                float new_scroll = m.ms_scroll - io.MouseDelta.x / m.width * m.ms_span;
                if (new_scroll < 0.0f) {
                    new_scroll = 0.0f;
                }
                const float max_scroll = content_span - m.ms_span;
                if (new_scroll > max_scroll) {
                    new_scroll = max_scroll;
                }
                state->view_scroll_ms = new_scroll;
            }
        }
    }

    // Point handles on top of everything.
    for (uint32_t i = 0; i < env.num_points; ++i) {
        const ImVec2 center(tick_to_x(m, env.points[i].position), point_y(m, env, i));
        const bool   selected = static_cast<int32_t>(i) == state->selected_point;
        const bool   hovered  = static_cast<int32_t>(i) == state->hover_point;
        const float  radius   = selected || hovered ? point_radius + 1.5f : point_radius;
        draw_list->AddCircleFilled(center, radius, selected ? selected_col : point_col);
        if (sustain_boundary_point(env, i)) {
            draw_list->AddCircle(center, radius + 1.5f, sustain_col);
        }
    }
    draw_list->PopClipRect();

    draw_ruler(m);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + chart_height + sp));
    ImGui::Dummy(ImVec2(width, ruler_height));

    // Thin horizontal scrollbar, always reserved.  Dragging it pans the
    // zoomed view; at full zoom-out the thumb spans the whole track.
    const float scrollbar_y = chart_height + sp + ruler_height + 1.0f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + scrollbar_y));
    bool scroll_dragged = false;
    bool scroll_hovered = false;
    if (interactive) {
        ImGui::InvisibleButton("##envscroll", ImVec2(width, scrollbar_height));
        scroll_dragged = ImGui::IsItemActive();
        scroll_hovered = ImGui::IsItemHovered();
        if (scroll_dragged) {
            float new_scroll = m.ms_scroll - ImGui::GetIO().MouseDelta.x / m.width * m.ms_span;
            if (new_scroll < 0.0f) {
                new_scroll = 0.0f;
            }
            const float max_scroll = content_span - m.ms_span;
            if (new_scroll > max_scroll) {
                new_scroll = max_scroll;
            }
            state->view_scroll_ms = new_scroll;
        }
    }
    else {
        ImGui::Dummy(ImVec2(width, scrollbar_height));
    }
    {
        float x0 = m.origin_x + m.ms_scroll / content_span * m.width;
        float x1 = m.origin_x + (m.ms_scroll + m.ms_span) / content_span * m.width;
        if (x1 < x0 + 2.0f) {
            x1 = x0 + 2.0f; // minimum thumb width so the handle stays visible
        }
        draw_list->AddRectFilled(ImVec2(m.origin_x, origin.y + scrollbar_y),
                                 ImVec2(m.origin_x + m.width, origin.y + scrollbar_y + scrollbar_height),
                                 chart_bg_color);
        draw_list->AddRectFilled(ImVec2(x0, origin.y + scrollbar_y),
                                 ImVec2(x1, origin.y + scrollbar_y + scrollbar_height),
                                 scroll_dragged || scroll_hovered ? scrollbar_color : grid_color);
    }

    // The control block always renders so the node shows its editing
    // affordances at a glance; it stays inert until the node is selected.
    ImGui::BeginDisabled(! interactive);

    // Button row: edits act on the selected point.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + buttons_y));
    if (ImGui::SmallButton("Add <")) {
        out_edit->kind      = EnvelopeEditKind::insert_before;
        out_edit->point_idx = static_cast<uint32_t>(state->selected_point);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add point before selected");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add >")) {
        out_edit->kind      = EnvelopeEditKind::insert_after;
        out_edit->point_idx = static_cast<uint32_t>(state->selected_point);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add point after selected");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state->selected_point <= 0);
    if (ImGui::SmallButton("Del")) {
        out_edit->kind      = EnvelopeEditKind::remove;
        out_edit->point_idx = static_cast<uint32_t>(state->selected_point);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Delete selected point");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Sus <")) {
        out_edit->kind      = EnvelopeEditKind::sustain_first;
        out_edit->point_idx = static_cast<uint32_t>(state->selected_point);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sustain loop starts at selected point");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Sus >")) {
        out_edit->kind      = EnvelopeEditKind::sustain_last;
        out_edit->point_idx = static_cast<uint32_t>(state->selected_point);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Sustain loop ends at selected point");
    }

    // Info line: hovered point overrides the selected point's display; the
    // edit boxes always edit the selected point.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + info_y));
    const int32_t shown = state->hover_point >= 0 ? state->hover_point : state->selected_point;
    if (shown >= 0 && static_cast<uint32_t>(shown) < env.num_points) {
        const uint32_t shown_idx = static_cast<uint32_t>(shown);
        const char*    sustain_role;
        if (static_cast<uint32_t>(env.sustain_first_point) == shown_idx &&
            static_cast<uint32_t>(env.sustain_last_point) == shown_idx) {
            sustain_role = "  [sustain start+end]";
        }
        else if (static_cast<uint32_t>(env.sustain_first_point) == shown_idx) {
            sustain_role = "  [sustain start]";
        }
        else if (static_cast<uint32_t>(env.sustain_last_point) == shown_idx) {
            sustain_role = "  [sustain end]";
        }
        else {
            sustain_role = "";
        }
        char info[96];
        snprintf(
            info,
            sizeof(info),
            "P%u  %.0f ms  %.3g%s",
            static_cast<uint32_t>(shown) + 1u,
            static_cast<double>(Sculptor::envelope_ticks_to_ms(env.points[shown_idx].position)),
            static_cast<double>(env.min_value + static_cast<float>(env.points[shown_idx].value) * env.min_max_delta),
            sustain_role);
        ImGui::TextUnformatted(info);
    }

    // Position edit box (milliseconds, snapped to ticks on edit).
    const uint32_t sel = static_cast<uint32_t>(state->selected_point);
    if (sel < env.num_points) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + boxes_y));
        if (! state->position_box_active) {
            state->position_edit_ms = Sculptor::envelope_ticks_to_ms(env.points[sel].position);
        }
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputFloat("##envpos", &state->position_edit_ms, 0.0f, 0.0f, "%.0f");
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemEdited()) {
            out_edit->kind      = EnvelopeEditKind::set_position;
            out_edit->point_idx = sel;
            out_edit->position  = Sculptor::envelope_ms_to_ticks(state->position_edit_ms);
        }
        if (state->position_box_active && ! active) {
            out_edit->kind = EnvelopeEditKind::gesture_end;
        }
        state->position_box_active = active;

        // Value edit box in effective units.
        if (! state->value_box_active) {
            state->value_edit_value = env.min_value + static_cast<float>(env.points[sel].value) * env.min_max_delta;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        const bool value_editable = env.min_max_delta != 0.0f;
        ImGui::BeginDisabled(! value_editable);
        ImGui::InputFloat("##envval", &state->value_edit_value, 0.0f, 0.0f, "%.3g");
        const bool value_active = ImGui::IsItemActive();
        if (value_editable && ImGui::IsItemEdited()) {
            float raw = (state->value_edit_value - env.min_value) / env.min_max_delta;
            if (raw < 0.0f) {
                raw = 0.0f;
            }
            if (raw > 65535.0f) {
                raw = 65535.0f;
            }
            out_edit->kind      = EnvelopeEditKind::set_value;
            out_edit->point_idx = sel;
            out_edit->value     = static_cast<uint16_t>(raw + 0.5f);
        }
        if (state->value_box_active && ! value_active) {
            out_edit->kind = EnvelopeEditKind::gesture_end;
        }
        state->value_box_active = value_active;
        ImGui::EndDisabled();
    }

    ImGui::EndDisabled();
}
