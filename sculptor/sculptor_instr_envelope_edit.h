// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_config.h"
#include "../synth/synth_parameters.h"

#include <stdint.h>

// Envelope curve editing: pure constraint functions plus the curve widget.
// The constraint functions are defined here (dependency-free) so headless
// tests link them without ImGui; the widget implementation stays in the
// .cpp.  No ImGui here: synth_unit includes this translation unit's header
// only.

namespace Sculptor {

// One envelope tick is one 256-sample control block (the runtime advances
// eval_envelope once per rt_step_samples at rt_sampling_rate).
constexpr float envelope_ms_per_tick =
    static_cast<float>(Synth::rt_step_samples) * 1000.0f / static_cast<float>(Synth::rt_sampling_rate);

inline float envelope_ticks_to_ms(uint16_t ticks)
{
    return static_cast<float>(ticks) * envelope_ms_per_tick;
}

// Converts milliseconds to envelope ticks, rounded to nearest and clamped to
// the uint16 position range.
inline uint16_t envelope_ms_to_ticks(float ms)
{
    if (ms <= 0.0f) {
        return 0;
    }
    const float ticks = ms / envelope_ms_per_tick;
    if (ticks >= 65535.0f) {
        return 65535;
    }
    return static_cast<uint16_t>(ticks + 0.5f);
}

// True when every point position is strictly greater than its predecessor.
// Editor-created descriptors always satisfy this; validate_envelope also
// accepts a leading run of zero positions (hand-built JSON banks only), and
// the editing functions below treat such descriptors as read-only.
inline bool env_positions_strictly_increasing(const Synth::EnvelopeDescriptor& env)
{
    if (env.num_points < 1 || env.num_points > Synth::max_envelope_points) {
        return false;
    }
    for (uint32_t i = 1; i < env.num_points; ++i) {
        if (env.points[i].position <= env.points[i - 1].position) {
            return false;
        }
    }
    return true;
}

// True when the descriptor is a sound editing source: point count in range,
// strictly increasing positions, and sustain indices inside the point range
// (the invariants validate_envelope enforces on fully placed descriptors).
inline bool env_source_valid(const Synth::EnvelopeDescriptor& env)
{
    return env_positions_strictly_increasing(env) && env.sustain_first_point <= env.sustain_last_point &&
           env.sustain_last_point < env.num_points;
}

// All editing functions return false and leave *out untouched when the
// request cannot produce a descriptor that passes envelope validation.
// point_idx out of range, a malformed source, or an impossible placement are
// rejections, never silent clamps into something the runtime would refuse.

// Moves one point.  The new position is clamped strictly between the
// neighbor positions (point 0 may sit at tick 0); when the neighbor gap has
// no free tick the original position is kept.  The value is taken as given
// (uint16 range).
inline bool env_move_point(const Synth::EnvelopeDescriptor& src,
                           uint32_t                         point_idx,
                           uint16_t                         new_position,
                           uint16_t                         new_value,
                           Synth::EnvelopeDescriptor*       out)
{
    if (out == nullptr || point_idx >= src.num_points || ! env_source_valid(src)) {
        return false;
    }
    const uint32_t lower    = point_idx == 0 ? 0 : src.points[point_idx - 1].position + 1;
    const uint32_t upper    = point_idx + 1 == src.num_points ? 0xFFFF : src.points[point_idx + 1].position - 1;
    uint32_t       position = new_position;
    if (lower > upper) {
        // Adjacent neighbors leave no free tick: keep the original position.
        position = src.points[point_idx].position;
    }
    else {
        if (position < lower) {
            position = lower;
        }
        if (position > upper) {
            position = upper;
        }
    }
    *out                            = src;
    out->points[point_idx].position = static_cast<uint16_t>(position);
    out->points[point_idx].value    = new_value;
    return true;
}

// Inserts a point next to the reference point (after it, or before it),
// copying the reference point's value.  Interior insertions land at the
// midpoint of the free tick range between the neighbors; before the first
// point at first_pos - 1; after the last point at last_pos + 1.  Rejected
// when the descriptor already holds max_envelope_points points or no free
// tick exists on the requested side.
inline bool env_insert_point(const Synth::EnvelopeDescriptor& src,
                             uint32_t                         ref_idx,
                             bool                             after,
                             Synth::EnvelopeDescriptor*       out)
{
    if (out == nullptr || ref_idx >= src.num_points || ! env_source_valid(src)) {
        return false;
    }
    if (src.num_points >= Synth::max_envelope_points) {
        return false;
    }
    uint32_t lower;
    uint32_t upper;
    if (after) {
        lower = src.points[ref_idx].position + 1;
        upper = ref_idx + 1 == src.num_points ? 0xFFFF : src.points[ref_idx + 1].position - 1;
    }
    else {
        if (ref_idx == 0 && src.points[0].position == 0) {
            return false;
        }
        lower = ref_idx == 0 ? 0 : src.points[ref_idx - 1].position + 1;
        upper = src.points[ref_idx].position - 1;
    }
    if (lower > upper) {
        return false;
    }
    uint32_t position;
    if (after && ref_idx + 1 == src.num_points) {
        position = lower;
    }
    else if (! after && ref_idx == 0) {
        position = upper;
    }
    else {
        position = lower + (upper - lower) / 2;
    }
    *out                     = src;
    const uint32_t insert_at = after ? ref_idx + 1 : ref_idx;
    // Copy the reference value before shifting: out may alias src.
    const uint16_t ref_value = src.points[ref_idx].value;
    for (uint32_t i = src.num_points; i > insert_at; --i) {
        out->points[i] = out->points[i - 1];
    }
    out->points[insert_at].position = static_cast<uint16_t>(position);
    out->points[insert_at].value    = ref_value;
    ++out->num_points;
    if (out->sustain_first_point >= insert_at) {
        ++out->sustain_first_point;
    }
    if (out->sustain_last_point >= insert_at) {
        ++out->sustain_last_point;
    }
    return true;
}

// Removes one point.  Point 0 and the last remaining point cannot be
// removed.  Sustain indices shift with the removed point and clamp into the
// valid range; the vacated tail slot is zeroed so the inactive tail stays
// canonical for save/load round trips.
inline bool env_remove_point(const Synth::EnvelopeDescriptor& src, uint32_t point_idx, Synth::EnvelopeDescriptor* out)
{
    if (out == nullptr || point_idx >= src.num_points || point_idx == 0 || src.num_points <= 1 ||
        ! env_source_valid(src)) {
        return false;
    }
    *out = src;
    for (uint32_t i = point_idx; i + 1 < src.num_points; ++i) {
        out->points[i] = out->points[i + 1];
    }
    out->points[src.num_points - 1] = {};
    --out->num_points;
    if (out->sustain_first_point > point_idx) {
        --out->sustain_first_point;
    }
    if (out->sustain_last_point > point_idx) {
        --out->sustain_last_point;
    }
    if (out->sustain_last_point >= out->num_points) {
        out->sustain_last_point = static_cast<uint8_t>(out->num_points - 1);
    }
    if (out->sustain_first_point > out->sustain_last_point) {
        out->sustain_first_point = out->sustain_last_point;
    }
    return true;
}

// Sets the sustain loop start to the given point; the loop end follows when
// it would precede the start.
inline bool env_set_sustain_first(const Synth::EnvelopeDescriptor& src,
                                  uint32_t                         point_idx,
                                  Synth::EnvelopeDescriptor*       out)
{
    if (out == nullptr || point_idx >= src.num_points || ! env_positions_strictly_increasing(src)) {
        return false;
    }
    *out                     = src;
    out->sustain_first_point = static_cast<uint8_t>(point_idx);
    if (out->sustain_last_point < out->sustain_first_point) {
        out->sustain_last_point = out->sustain_first_point;
    }
    return true;
}

// Sets the sustain loop end to the given point; the loop start follows when
// it would exceed the end.
inline bool env_set_sustain_last(const Synth::EnvelopeDescriptor& src,
                                 uint32_t                         point_idx,
                                 Synth::EnvelopeDescriptor*       out)
{
    if (out == nullptr || point_idx >= src.num_points || ! env_positions_strictly_increasing(src)) {
        return false;
    }
    *out                    = src;
    out->sustain_last_point = static_cast<uint8_t>(point_idx);
    if (out->sustain_first_point > out->sustain_last_point) {
        out->sustain_first_point = out->sustain_last_point;
    }
    return true;
}

// Edit request the widget emits for the caller to turn into descriptor
// edits.  The widget owns no model state: it reports what the user did, the
// caller applies it through the constraint functions and commits.
enum class EnvelopeEditKind {
    none,
    move,          // drag: point_idx tracks position/value absolutely each frame
    gesture_end,   // drag or edit box deactivated; ends the caller's gesture
    insert_before, // insert before point_idx
    insert_after,  // insert after point_idx
    remove,        // remove point_idx
    sustain_first, // sustain loop start := point_idx
    sustain_last,  // sustain loop end := point_idx
    set_position,  // point_idx := position ticks (value kept)
    set_value,     // point_idx := value (position kept)
    select_node    // click on an unselected node's chart; select, edit nothing
};

struct EnvelopeCurveEdit {
    EnvelopeEditKind kind      = EnvelopeEditKind::none;
    uint32_t         point_idx = 0;
    uint16_t         position  = 0;
    uint16_t         value     = 0;
};

// Widget-local UI state (selection, hover, active items, the mapping frozen
// at drag activation); no model data.
struct EnvelopeCurveState {
    int32_t selected_point      = 0;
    int32_t hover_point         = -1;
    int32_t chart_drag          = -1; // point grabbed by the active chart drag
    bool    position_box_active = false;
    float   position_edit_ms    = 0.0f;
    bool    value_box_active    = false;
    float   value_edit_value    = 0.0f;
    // Model-space mapping frozen at drag activation so per-frame commits and
    // live span changes cannot feed back into the drag target.
    float gesture_ms_span        = 0.0f;
    float gesture_value_bottom   = 0.0f;
    float gesture_value_top      = 0.0f;
    float gesture_min_value      = 0.0f;
    float gesture_min_max_delta  = 0.0f;
    float gesture_grab_offset_ms = 0.0f;
    bool  read_only_notified     = false; // one overlap warning per node context
};

// Draws the envelope curve widget (chart, ms ruler, buttons, info line) at
// the current ImGui cursor position and reports user intents through
// *out_edit (kind none when nothing happened).  When interactive is false
// the chart draws read-only; its chart item still claims input so clicks
// select the node instead of starting a node drag, and the widget reserves
// its full height in both states.
void gui_envelope_curve(EnvelopeCurveState*              state,
                        const Synth::EnvelopeDescriptor& env,
                        bool                             interactive,
                        EnvelopeCurveEdit*               out_edit);

// Fixed full widget height in pixels (chart, ruler, buttons, info line);
// identical whether the node is selected or not.
int envelope_widget_height();

// Content width the projection gives envelope nodes so the chart fits.
constexpr float envelope_node_content_width = 280.0f;

} // namespace Sculptor
