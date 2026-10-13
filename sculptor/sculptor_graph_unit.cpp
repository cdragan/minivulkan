// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Unit tests for the Sculptor::Graph M1 data model.
// Uses only the public API declared in sculptor_graph.h per the approved
// M1 plan.  TEST-macro pattern copied from sculptor_undo_unit.cpp.

#include "../thirdparty/imgui/src/imgui_internal.h"
#include "sculptor_graph.h"
#include "sculptor_osc_graph.h"

#include <stdio.h>
#include <string.h>

#ifdef GRAPH_UNIT_AUDIO_RENDER_TEST_STUB
bool Sculptor::oscillator_slot_waveform_mode_disabled(const Sculptor::Node&, uint32_t)
{
    return false;
}
#endif

static int exit_code = 0;

#define TEST(cond)                                                 \
    do {                                                           \
        if (! (cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit_code = 1;                                         \
        }                                                          \
    } while (0)

using Sculptor::ChangeKind;
using Sculptor::Connection;
using Sculptor::EndPoint;
using Sculptor::Graph;
using Sculptor::GraphChange;
using Sculptor::GraphColors;
using Sculptor::max_connections;
using Sculptor::max_node_slots;
using Sculptor::max_nodes;
using Sculptor::max_pending_changes;
using Sculptor::Node;
using Sculptor::PropertyType;
using Sculptor::Slot;
using Sculptor::SlotKind;

static uint32_t drain_changes(Graph& graph, GraphChange* out, uint32_t out_size)
{
    const uint32_t count = graph.take_changes(out, out_size);
    return count;
}

// Shared test graphs: each test resets its slot by zero-filling it, which
// restores every field including callbacks, view state and the change ring
// (the zero-filled state is directly usable); Graph::clear() only deletes
// nodes and popup state.  Five slots cover the widest concurrent use
// inside one test.
static Graph unit_graphs[5];

static Graph& reset_unit_graph(uint32_t slot)
{
    // The zero-filled state is valid: zero zoom renders as 1, idle is zero,
    // and colors stay unset until the owner assigns a palette.
    memset(&unit_graphs[slot], 0, sizeof(Graph));
    return unit_graphs[slot];
}

static void test_create_node_distinct_indices()
{
    Graph& g = reset_unit_graph(0);

    uint32_t idx[max_nodes]  = {};
    bool     seen[max_nodes] = {};

    for (uint32_t i = 0; i < max_nodes; ++i) {
        idx[i] = g.create_node("node", vmath::vec2(0.0f, 0.0f));
        TEST(idx[i] != Sculptor::pool_no_slot);
        if (idx[i] == Sculptor::pool_no_slot) {
            break; // already reported above; skip out-of-bounds checks below
        }
        TEST(idx[i] < max_nodes);
        TEST(! seen[idx[i]]);
        seen[idx[i]] = true;
        TEST(! g.node(idx[i]).ghost);
    }
}

static void test_create_node_exhaustion_is_safe()
{
    Graph& g = reset_unit_graph(0);

    for (uint32_t i = 0; i < max_nodes; ++i) {
        TEST(g.create_node("node", vmath::vec2(0.0f, 0.0f)) != Sculptor::pool_no_slot);
    }

    // Pool is full: the extra allocation must fail safely, not crash.
    TEST(g.create_node("overflow", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);
    TEST(g.create_node("overflow", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);
}

static void test_create_node_snaps_position_to_grid()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t idx = g.create_node("node", vmath::vec2(10.0f, 23.0f));
    TEST(idx != Sculptor::pool_no_slot);
    const vmath::vec2 pos = g.node(idx).position;
    TEST(pos.x == 0.0f);  // 10 snapped down to the 16px grid
    TEST(pos.y == 16.0f); // 23 snapped down to the 16px grid
}

static void test_add_slot_grows_within_limit()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_idx = g.create_node("node", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);

    bool seen[max_node_slots] = {};

    for (uint32_t i = 0; i < max_node_slots; ++i) {
        Slot slot               = {};
        slot.kind               = SlotKind::input;
        const uint32_t slot_idx = g.add_slot(node_idx, slot);
        TEST(slot_idx != Sculptor::pool_no_slot);
        if (slot_idx == Sculptor::pool_no_slot) {
            break; // already reported above; skip out-of-bounds checks below
        }
        TEST(slot_idx < max_node_slots);
        TEST(! seen[slot_idx]);
        seen[slot_idx] = true;
    }
}

static void test_add_slot_overflow_returns_no_slot()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_idx = g.create_node("node", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);

    for (uint32_t i = 0; i < max_node_slots; ++i) {
        Slot slot = {};
        slot.kind = SlotKind::output;
        TEST(g.add_slot(node_idx, slot) != Sculptor::pool_no_slot);
    }

    // Node slot pool is full: overflow must return the no-slot marker.
    Slot extra_slot = {};
    extra_slot.kind = SlotKind::output;
    TEST(g.add_slot(node_idx, extra_slot) == Sculptor::pool_no_slot);
}

static void test_delete_node_frees_index_and_drops_connections()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);
    TEST(node_b != Sculptor::pool_no_slot);

    Slot slot_a               = {};
    slot_a.kind               = SlotKind::output;
    const uint32_t slot_a_idx = g.add_slot(node_a, slot_a);
    Slot           slot_b     = {};
    slot_b.kind               = SlotKind::input;
    const uint32_t slot_b_idx = g.add_slot(node_b, slot_b);
    TEST(slot_a_idx != Sculptor::pool_no_slot);
    TEST(slot_b_idx != Sculptor::pool_no_slot);

    const EndPoint out = { node_a, slot_a_idx };
    const EndPoint in  = { node_b, slot_b_idx };
    const uint32_t con = g.add_connection(out, in);
    TEST(con != Sculptor::pool_no_slot);
    TEST(g.get_connection(con).output.node_idx == node_a);
    TEST(g.get_connection(con).input.node_idx == node_b);

    // Empty the queue so the delete checks below only see deletion events.
    GraphChange discard[32] = {};
    drain_changes(g, discard, 32);

    g.delete_node(node_a);

    // Freed index must be reused by the next allocation (first-fit pool).
    const uint32_t reused = g.create_node("c", vmath::vec2(0.0f, 0.0f));
    TEST(reused == node_a);

    // delete_node must have dropped the connection touching node_a.
    GraphChange    events[8]              = {};
    const uint32_t num_events             = drain_changes(g, events, 8);
    uint32_t       num_connection_deleted = 0;
    uint32_t       num_node_deleted       = 0;
    uint32_t       num_node_added         = 0;
    for (uint32_t i = 0; i < num_events; ++i) {
        if (events[i].kind == ChangeKind::connection_deleted) {
            ++num_connection_deleted;
            TEST(events[i].connection_idx == con);
        }
        else if (events[i].kind == ChangeKind::node_deleted) {
            ++num_node_deleted;
        }
        else if (events[i].kind == ChangeKind::node_added) {
            ++num_node_added;
        }
    }
    TEST(num_connection_deleted == 1);
    TEST(num_node_deleted == 1);
    TEST(num_node_added == 1);
}

static void test_add_delete_connection_basics()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);
    TEST(node_b != Sculptor::pool_no_slot);

    // Connections require occupied slots at both endpoints.
    Slot slot_a = {};
    slot_a.kind = SlotKind::output;
    TEST(g.add_slot(node_a, slot_a) != Sculptor::pool_no_slot);
    Slot slot_b = {};
    slot_b.kind = SlotKind::input;
    TEST(g.add_slot(node_b, slot_b) != Sculptor::pool_no_slot);

    // Connections return distinct indices up to the pool capacity.
    bool seen[max_connections] = {};
    for (uint32_t i = 0; i < max_connections; ++i) {
        const EndPoint out = { node_a, 0 };
        const EndPoint in  = { node_b, 0 };
        const uint32_t con = g.add_connection(out, in);
        TEST(con != Sculptor::pool_no_slot);
        if (con == Sculptor::pool_no_slot) {
            break; // already reported above; skip out-of-bounds checks below
        }
        TEST(con < max_connections);
        TEST(! seen[con]);
        seen[con] = true;
    }

    // Exhaustion is safe.
    const EndPoint out = { node_a, 0 };
    const EndPoint in  = { node_b, 0 };
    TEST(g.add_connection(out, in) == Sculptor::pool_no_slot);

    // After deleting one connection, its index can be allocated again.
    g.delete_connection(0);
    const uint32_t reused = g.add_connection(out, in);
    TEST(reused == 0);

    // Endpoints are stored correctly (observable via the const accessor).
    TEST(g.get_connection(reused).output.node_idx == node_a);
    TEST(g.get_connection(reused).input.node_idx == node_b);
}

static void test_add_connection_rejects_bad_endpoints()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);

    const EndPoint missing_node      = { 61, 0 };
    const EndPoint missing_slot      = { node_a, 5 };
    const EndPoint out_of_range_slot = { node_a, max_node_slots + 1 };
    const EndPoint valid_empty_slot  = { node_a, 0 };

    TEST(g.add_connection(missing_node, valid_empty_slot) == Sculptor::pool_no_slot);
    TEST(g.add_connection(valid_empty_slot, missing_node) == Sculptor::pool_no_slot);
    TEST(g.add_connection(valid_empty_slot, missing_slot) == Sculptor::pool_no_slot);
    TEST(g.add_connection(out_of_range_slot, valid_empty_slot) == Sculptor::pool_no_slot);
    TEST(g.add_connection(valid_empty_slot, out_of_range_slot) == Sculptor::pool_no_slot);

    // Semantic endpoint kinds: from an output, to an input or connectable
    // property.  Everything else is rejected.
    const uint32_t node_c = g.create_node("c", vmath::vec2(0.0f, 0.0f));
    TEST(node_c != Sculptor::pool_no_slot);
    Slot output_slot          = {};
    output_slot.kind          = SlotKind::output;
    const uint32_t output_idx = g.add_slot(node_a, output_slot);
    TEST(output_idx != Sculptor::pool_no_slot);
    const EndPoint valid_output = { node_a, output_idx };
    Slot           input_slot   = {};
    input_slot.kind             = SlotKind::input;
    const uint32_t input_idx    = g.add_slot(node_c, input_slot);
    TEST(input_idx != Sculptor::pool_no_slot);
    Slot plain_property        = {};
    plain_property.kind        = SlotKind::property;
    plain_property.connectable = false;
    const uint32_t plain_idx   = g.add_slot(node_c, plain_property);
    TEST(plain_idx != Sculptor::pool_no_slot);
    Slot connectable_property        = {};
    connectable_property.kind        = SlotKind::property;
    connectable_property.connectable = true;
    const uint32_t connectable_idx   = g.add_slot(node_c, connectable_property);
    TEST(connectable_idx != Sculptor::pool_no_slot);

    const EndPoint wrong_direction = { node_c, input_idx }; // input slot as output end
    TEST(g.add_connection(wrong_direction, valid_empty_slot) == Sculptor::pool_no_slot);

    const EndPoint plain_property_ep = { node_c, plain_idx }; // not connectable
    TEST(g.add_connection(valid_output, plain_property_ep) == Sculptor::pool_no_slot);

    const EndPoint connectable_ep = { node_c, connectable_idx };
    TEST(g.add_connection(valid_output, connectable_ep) != Sculptor::pool_no_slot);
}

static void test_ghost_is_flag_on_normal_node()
{
    Graph& g = reset_unit_graph(0);

    // The caller creates the node normally, then marks it as a ghost.
    const uint32_t node_idx = g.create_node("ghost", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);
    TEST(! g.node(node_idx).ghost);

    g.set_ghost(node_idx, true);
    TEST(g.node(node_idx).ghost);

    // Marking is idempotent and does not consume another pool entry.
    g.set_ghost(node_idx, true);
    TEST(g.node(node_idx).ghost);

    // Placement clears the flag and pushes ghost_placed.
    g.set_ghost(node_idx, false);
    TEST(! g.node(node_idx).ghost);
    GraphChange    events[4]        = {};
    const uint32_t num_events       = drain_changes(g, events, 4);
    uint32_t       num_ghost_placed = 0;
    for (uint32_t i = 0; i < num_events; ++i) {
        if (events[i].kind == ChangeKind::ghost_placed) {
            ++num_ghost_placed;
            TEST(events[i].node_idx == node_idx);
        }
    }
    TEST(num_ghost_placed == 1);

    // Cancellation deletes the node: same pool as regular nodes, no conflict.
    g.set_ghost(node_idx, true);
    g.delete_node(node_idx);
    TEST(g.create_node("node", vmath::vec2(0.0f, 0.0f)) == node_idx);
}

static void test_colors_api()
{
    Graph& g = reset_unit_graph(0);

    // set_colors copies the whole set (observable via colors()).
    GraphColors colors     = {};
    colors.node_background = 0x202020FFu;
    colors.node_border     = 0x808080FFu;
    g.set_colors(colors);
    TEST(g.colors().node_background == 0x202020FFu);
    TEST(g.colors().node_border == 0x808080FFu);

    const uint32_t node_idx = g.create_node("node", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);

    g.set_node_color(node_idx, 0xFF0000FFu); // override
    TEST(g.node(node_idx).color_override == 0xFF0000FFu);
    g.set_node_color(node_idx, 0); // back to default set
    TEST(g.node(node_idx).color_override == 0);
    g.set_node_color(max_nodes, 0); // out of range must not crash
}

static void test_change_events_queue_and_drain()
{
    Graph& g = reset_unit_graph(0);

    TEST(drain_changes(g, nullptr, 0) == 0);
    TEST(! g.changes_overflowed());

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    Slot           slot_a = {};
    slot_a.kind           = SlotKind::output;
    TEST(g.add_slot(node_a, slot_a) != Sculptor::pool_no_slot);
    Slot slot_b = {};
    slot_b.kind = SlotKind::input;
    TEST(g.add_slot(node_b, slot_b) != Sculptor::pool_no_slot);
    const EndPoint out = { node_a, 0 };
    const EndPoint in  = { node_b, 0 };
    const uint32_t con = g.add_connection(out, in);

    GraphChange    events[8]  = {};
    const uint32_t num_events = drain_changes(g, events, 8);
    TEST(num_events == 5);
    if (num_events == 5) {
        TEST(events[0].kind == ChangeKind::node_added);
        TEST(events[0].node_idx == node_a);
        TEST(events[1].kind == ChangeKind::node_added);
        TEST(events[1].node_idx == node_b);
        TEST(events[2].kind == ChangeKind::slot_added);
        TEST(events[3].kind == ChangeKind::slot_added);
        TEST(events[4].kind == ChangeKind::connection_added);
        TEST(events[4].connection_idx == con);
    }

    // Queue is empty after draining.
    TEST(drain_changes(g, events, 8) == 0);

    // Partial drain keeps the remaining events for the next call.
    g.create_node("c", vmath::vec2(0.0f, 0.0f));
    g.create_node("d", vmath::vec2(0.0f, 0.0f));
    TEST(drain_changes(g, events, 1) == 1);
    TEST(drain_changes(g, events, 8) == 1);
}

static void test_change_events_overflow_is_reported()
{
    Graph& g = reset_unit_graph(0);

    // Overflow the ring buffer: more than max_pending_changes events without
    // draining.  Each cycle pushes two events (node_added, node_deleted).
    for (uint32_t i = 0; i <= max_pending_changes / 2 + 1; ++i) {
        const uint32_t idx = g.create_node("cycle", vmath::vec2(0.0f, 0.0f));
        if (idx == Sculptor::pool_no_slot) {
            break; // pool exhaustion also overflows the queue; either is fine
        }
        g.delete_node(idx);
    }
    TEST(g.changes_overflowed());

    // The flag is reported once, then cleared.
    TEST(! g.changes_overflowed());
}

// --- M2: connections ---

static bool validator_rejects_same_node(void* user_data, Sculptor::Graph& graph, EndPoint output, EndPoint input)
{
    return output.node_idx != input.node_idx;
}

static bool validator_rejects_all(void* user_data, Sculptor::Graph& graph, EndPoint output, EndPoint input)
{
    return false;
}

struct TwoNodeFixture {
    Graph    graph{};
    uint32_t node_a;
    uint32_t node_b;
    uint32_t output_slot;
    uint32_t input_slot;
    EndPoint output_end;
    EndPoint input_end;

    TwoNodeFixture()
    {
        node_a              = graph.create_node("a", vmath::vec2(0.0f, 0.0f));
        node_b              = graph.create_node("b", vmath::vec2(0.0f, 0.0f));
        Slot out_slot       = {};
        out_slot.kind       = SlotKind::output;
        output_slot         = graph.add_slot(node_a, out_slot);
        Slot in_slot        = {};
        in_slot.kind        = SlotKind::input;
        input_slot          = graph.add_slot(node_b, in_slot);
        output_end.node_idx = node_a;
        output_end.slot_idx = output_slot;
        input_end.node_idx  = node_b;
        input_end.slot_idx  = input_slot;
    }
};

static void test_attempt_connection_success()
{
    TwoNodeFixture f;
    TEST(f.output_slot != Sculptor::pool_no_slot);
    TEST(f.input_slot != Sculptor::pool_no_slot);

    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(f.graph.connection_count() == 1);
    TEST(f.graph.get_connection(0).output.node_idx == f.node_a);
    TEST(f.graph.get_connection(0).input.node_idx == f.node_b);
    TEST(! f.graph.has_error());

    GraphChange events[8];
    // node_added + slot_added per fixture node, then connection_added.
    TEST(drain_changes(f.graph, events, 8) == 5);
    TEST(events[4].kind == ChangeKind::connection_added);
    TEST(events[4].connection_idx == 0);
}

static void test_attempt_connection_validator_rejects()
{
    TwoNodeFixture f;
    f.graph.set_validator(validator_rejects_same_node, nullptr);

    // Same-node connection: validator rejects, error is set, nothing added.
    TEST(! f.graph.attempt_connection(f.output_end, f.output_end));
    TEST(f.graph.has_error());
    TEST(f.graph.error_text() != nullptr);
    TEST(f.graph.error_text()[0] != '\0');
    TEST(f.graph.connection_count() == 0);

    GraphChange events[8];
    // Only the fixture's node_added + slot_added events; nothing added.
    TEST(drain_changes(f.graph, events, 8) == 4);
    TEST(events[3].kind != ChangeKind::connection_added);

    f.graph.dismiss_error();
    TEST(! f.graph.has_error());

    // Different nodes pass the validator and connect.
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(f.graph.connection_count() == 1);
    TEST(! f.graph.has_error());
}

static void test_attempt_connection_structural_rejection_sets_error()
{
    TwoNodeFixture f;

    // Occupied input: second attempt at the same input fails with error.
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(! f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(f.graph.has_error());
    TEST(f.graph.connection_count() == 1);
    f.graph.dismiss_error();

    // Kind mismatch: output to output is rejected with error.
    TEST(! f.graph.attempt_connection(f.output_end, f.output_end));
    TEST(f.graph.has_error());
    TEST(f.graph.connection_count() == 1);
}

static void test_move_connection_end_success()
{
    TwoNodeFixture f;
    const uint32_t node_c   = f.graph.create_node("c", vmath::vec2(0.0f, 0.0f));
    Slot           out_slot = {};
    out_slot.kind           = SlotKind::output;
    const uint32_t c_output = f.graph.add_slot(node_c, out_slot);
    Slot           in_slot  = {};
    in_slot.kind            = SlotKind::input;
    const uint32_t c_input  = f.graph.add_slot(node_c, in_slot);
    TEST(c_output != Sculptor::pool_no_slot);
    TEST(c_input != Sculptor::pool_no_slot);

    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32];
    drain_changes(f.graph, scratch, 32);

    // Retarget the input end to node c.
    const EndPoint new_input = { node_c, c_input };
    TEST(f.graph.move_connection_end(0, false, new_input));
    TEST(f.graph.connection_count() == 1);
    TEST(f.graph.get_connection(0).input.node_idx == node_c);
    TEST(f.graph.get_connection(0).input.slot_idx == c_input);
    TEST(f.graph.get_connection(0).output.node_idx == f.node_a);

    // Retarget the output end to node c as well.
    const EndPoint new_output = { node_c, c_output };
    TEST(f.graph.move_connection_end(0, true, new_output));
    TEST(f.graph.get_connection(0).output.node_idx == node_c);
    TEST(f.graph.get_connection(0).output.slot_idx == c_output);
    TEST(f.graph.get_connection(0).input.node_idx == node_c);

    // Retargets push connection_changed events, not delete+add.
    GraphChange    events[8];
    const uint32_t num_events = drain_changes(f.graph, events, 8);
    TEST(num_events == 2);
    TEST(events[0].kind == ChangeKind::connection_changed);
    TEST(events[0].connection_idx == 0);
    TEST(events[1].kind == ChangeKind::connection_changed);
}

static void test_move_connection_end_failure_snaps_back()
{
    TwoNodeFixture f;
    // The validator also rejects move targets, so create the connection
    // without a validator, then install it.
    f.graph.set_validator(nullptr, nullptr);
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32];
    drain_changes(f.graph, scratch, 32);
    f.graph.set_validator(validator_rejects_all, nullptr);

    const EndPoint nowhere = { f.node_b, f.input_slot };
    TEST(! f.graph.move_connection_end(0, false, nowhere));
    // A refused retarget snaps back: the connection survives untouched,
    // the error overlay is set, and no change events are pushed (a phantom
    // delete would commit and erase the overlay through reprojection).
    TEST(f.graph.connection_count() == 1);
    TEST(f.graph.has_error());
    GraphChange events[8];
    TEST(drain_changes(f.graph, events, 8) == 0);
}

static void test_move_connection_end_structural_failure_snaps_back()
{
    TwoNodeFixture f;
    const uint32_t node_c  = f.graph.create_node("c", vmath::vec2(0.0f, 0.0f));
    Slot           in_slot = {};
    in_slot.kind           = SlotKind::input;
    const uint32_t c_input = f.graph.add_slot(node_c, in_slot);
    TEST(c_input != Sculptor::pool_no_slot);

    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32];
    drain_changes(f.graph, scratch, 32);

    // Retarget to an out-of-range slot: structural check fails, the
    // connection snaps back and the error overlay is set.
    const EndPoint bad_slot = { node_c, max_node_slots + 3 };
    TEST(! f.graph.move_connection_end(0, false, bad_slot));
    TEST(f.graph.connection_count() == 1);
    TEST(f.graph.has_error());
    GraphChange events[8];
    TEST(drain_changes(f.graph, events, 8) == 0);
}

static void test_selection_basics()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);

    TEST(! g.is_selected(a));
    TEST(! g.is_selected(b));

    g.set_selected(a, true);
    TEST(g.is_selected(a));
    TEST(! g.is_selected(b));

    g.set_selected(b, true);
    TEST(g.is_selected(a));
    TEST(g.is_selected(b));

    g.set_selected(a, false);
    TEST(! g.is_selected(a));
    TEST(g.is_selected(b));

    g.select_none();
    TEST(! g.is_selected(a));
    TEST(! g.is_selected(b));

    // Selection changes are pure view state: no change events.
    GraphChange scratch[8] = {};
    drain_changes(g, scratch, 8); // drain the two node_added events
    g.set_selected(b, true);
    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 0);
}

static void test_delete_node_clears_selection()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.set_selected(a, true);
    g.delete_node(a);

    // The pool reuses the index; the reused node must start unselected with
    // a clean content size (it reads 0 until its first drawn frame).
    const uint32_t reused = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(reused == a);
    TEST(! g.is_selected(reused));
    TEST(g.content_size(reused).x == 0.0f);
    TEST(g.content_size(reused).y == 0.0f);
}

static void test_selection_api_ignores_unoccupied_slots()
{
    Graph& g = reset_unit_graph(0);

    // Out-of-range and unoccupied slots are no-ops, never seed state.
    g.set_selected(Sculptor::max_nodes, true);
    g.set_selected(0, true);
    g.report_content_size(Sculptor::max_nodes, vmath::vec2(50.0f, 50.0f));
    g.report_content_size(0, vmath::vec2(50.0f, 50.0f));
    TEST(! g.is_selected(Sculptor::max_nodes));
    TEST(! g.is_selected(0));
    TEST(g.content_size(Sculptor::max_nodes).x == 0.0f);
    TEST(g.content_size(0).x == 0.0f);

    // A node created later at the touched slot starts clean.
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(a == 0);
    TEST(! g.is_selected(a));
    TEST(g.content_size(a).x == 0.0f);
}

static void test_align_left_and_top()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(96.0f, 48.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 80.0f));
    const uint32_t c = g.create_node("c", vmath::vec2(64.0f, 16.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);
    TEST(c != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.report_content_size(b, vmath::vec2(60.0f, 20.0f));
    g.report_content_size(c, vmath::vec2(50.0f, 40.0f));

    GraphChange scratch[8] = {};
    drain_changes(g, scratch, 8); // drain the node_added events

    g.set_selected(a, true);
    g.set_selected(b, true);
    g.set_selected(c, true);

    // Leftmost selected node is b (x = 32).
    g.align_selected(Sculptor::AlignKind::left);
    TEST(g.node(a).position.x == 32.0f);
    TEST(g.node(b).position.x == 32.0f);
    TEST(g.node(c).position.x == 32.0f);

    // Topmost selected node is c (y = 16).
    g.align_selected(Sculptor::AlignKind::top);
    TEST(g.node(a).position.y == 16.0f);
    TEST(g.node(b).position.y == 16.0f);
    TEST(g.node(c).position.y == 16.0f);

    // Aligns move nodes without emitting change events.
    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 0);
}

static void test_align_right_and_bottom()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(96.0f, 48.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 80.0f));
    const uint32_t c = g.create_node("c", vmath::vec2(64.0f, 16.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);
    TEST(c != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.report_content_size(b, vmath::vec2(60.0f, 20.0f));
    g.report_content_size(c, vmath::vec2(50.0f, 40.0f));

    g.set_selected(a, true);
    g.set_selected(b, true);
    g.set_selected(c, true);

    // Rightmost selected edge is a (96 + 40 = 136): each node's right edge
    // lands there.
    g.align_selected(Sculptor::AlignKind::right);
    TEST(g.node(a).position.x == 96.0f);
    TEST(g.node(b).position.x == 76.0f); // 136 - 60
    TEST(g.node(c).position.x == 86.0f); // 136 - 50

    // Bottommost selected edge is b (80 + 20 = 100).
    g.align_selected(Sculptor::AlignKind::bottom);
    TEST(g.node(a).position.y == 70.0f); // 100 - 30
    TEST(g.node(b).position.y == 80.0f);
    TEST(g.node(c).position.y == 60.0f); // 100 - 40
}

static void test_align_equal_width_and_height()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.report_content_size(b, vmath::vec2(60.0f, 20.0f));

    g.set_selected(a, true);
    g.set_selected(b, true);

    // Widest selected width (60) becomes every selected node's override.
    g.align_selected(Sculptor::AlignKind::equal_width);
    TEST(g.node(a).content_width_override == 60.0f);
    TEST(g.node(b).content_width_override == 60.0f);
    // Positions are untouched by equal-size commands.
    TEST(g.node(a).position.x == 0.0f);
    TEST(g.node(b).position.x == 32.0f);
}

static void test_align_equal_height_applies_as_equal_width()
{
    // Equal height sets every selected node's height override to the
    // tallest selected height.
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.report_content_size(b, vmath::vec2(60.0f, 20.0f));

    g.set_selected(a, true);
    g.set_selected(b, true);

    g.align_selected(Sculptor::AlignKind::equal_height);
    TEST(g.node(a).content_height_override == 30.0f);
    TEST(g.node(b).content_height_override == 30.0f);
    // Widths are untouched by equal_height.
    TEST(g.node(a).content_width_override == 0.0f);
    TEST(g.node(b).content_width_override == 0.0f);
}

static void test_align_skips_ghosts_and_empty_selection()
{
    Graph&         g     = reset_unit_graph(0);
    const uint32_t a     = g.create_node("a", vmath::vec2(32.0f, 0.0f));
    const uint32_t b     = g.create_node("b", vmath::vec2(64.0f, 32.0f));
    const uint32_t ghost = g.create_node("ghost", vmath::vec2(96.0f, 64.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);
    TEST(ghost != Sculptor::pool_no_slot);
    g.set_ghost(ghost, true);

    // Empty selection: align is a no-op.
    g.align_selected(Sculptor::AlignKind::left);
    TEST(g.node(a).position.x == 32.0f);

    g.set_selected(a, true);
    g.set_selected(b, true);
    g.set_selected(ghost, true);

    // Leftmost selected (non-ghost) node is a (x = 32); the ghost must not
    // move (it follows the mouse while in ghost mode).
    g.align_selected(Sculptor::AlignKind::left);
    TEST(g.node(a).position.x == 32.0f);
    TEST(g.node(b).position.x == 32.0f);
    TEST(g.node(ghost).position.x == 96.0f);
}

// ---------------------------------------------------------------------------
// Delete veto: the editor refuses deletions of nodes referenced elsewhere.

static bool veto_single_node(void* user_data, uint32_t node_idx)
{
    return node_idx == *static_cast<const uint32_t*>(user_data);
}

// Delete key support: delete_selected removes every occupied, selected,
// non-ghost node through delete_node (the veto applies), returns the count,
// and the events reflect exactly the performed deletions.
static void test_delete_selected()
{
    Graph&         g = reset_unit_graph(0);
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(60.0f, 0.0f));
    const uint32_t c = g.create_node("c", vmath::vec2(120.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot && b != Sculptor::pool_no_slot && c != Sculptor::pool_no_slot);
    g.set_selected(a, true);
    g.set_selected(b, true);
    g.set_selected(c, true);
    GraphChange scratch[8] = {};
    (void)drain_changes(g, scratch, 8);

    // One of the three is vetoed: it survives and is not counted.
    uint32_t vetoed = b;
    g.set_delete_veto(veto_single_node, &vetoed);
    TEST(g.delete_selected() == 2);
    TEST(g.node_occupied(b));
    TEST(! g.node_occupied(a));
    TEST(! g.node_occupied(c));
    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 2);
    TEST(events[0].kind == Sculptor::ChangeKind::node_deleted);
    TEST(events[1].kind == Sculptor::ChangeKind::node_deleted);

    // Ghost and unselected nodes stay.
    g.set_delete_veto(nullptr, nullptr);
    g.select_none();
    const uint32_t d = g.create_node("d", vmath::vec2(180.0f, 0.0f));
    TEST(d != Sculptor::pool_no_slot);
    g.set_ghost(d, true);
    g.set_selected(d, true);
    (void)drain_changes(g, scratch, 8);
    TEST(g.delete_selected() == 0);
    TEST(g.node_occupied(d));
}

static void test_delete_node_veto()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);

    Slot out_slot        = {};
    out_slot.kind        = SlotKind::output;
    Slot in_slot         = {};
    in_slot.kind         = SlotKind::input;
    const uint32_t a_out = g.add_slot(a, out_slot);
    const uint32_t b_in  = g.add_slot(b, in_slot);
    TEST(a_out != Sculptor::pool_no_slot);
    TEST(b_in != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{ a, a_out }, EndPoint{ b, b_in }) != Sculptor::pool_no_slot);

    GraphChange scratch[8] = {};
    drain_changes(g, scratch, 8); // drain the setup events

    // Fill the pool: the probe assertions below need a full pool, with a
    // and b as occupants (indices 0 and 1) and no free index anywhere else.
    for (uint32_t i = 2; i < max_nodes; ++i) {
        TEST(g.create_node("fill", vmath::vec2(0.0f, 0.0f)) != Sculptor::pool_no_slot);
    }
    GraphChange fill[max_nodes] = {};
    drain_changes(g, fill, max_nodes);

    uint32_t vetoed = a;
    g.set_delete_veto(veto_single_node, &vetoed);

    // Vetoed delete: the node, its slots and its touching connections all
    // survive, and no change event is emitted.
    g.delete_node(a);
    TEST(g.has_error());
    g.delete_node(a);
    TEST(strcmp(g.node(a).name, "a") == 0);
    TEST(g.node(a).slots.is_occupied(a_out));
    TEST(g.connection_count() == 1);
    // The vetoed node still occupies its pool index: nothing is free.
    TEST(g.create_node("probe", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);

    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 0);

    // The veto is per node: deleting a different node succeeds, dropping
    // its connections (connection_deleted first) and then the node itself.
    g.delete_node(b);
    TEST(g.connection_count() == 0);
    TEST(drain_changes(g, events, 8) == 2);
    TEST(events[0].kind == ChangeKind::connection_deleted);
    TEST(events[1].kind == ChangeKind::node_deleted && events[1].node_idx == b);
    // The freed index is reused by the next allocation (first-fit pool).
    TEST(g.create_node("probe", vmath::vec2(0.0f, 0.0f)) == b);
}

// Capacity raises: the pools saturate at the raised limits and refuse
// further allocation safely, without crashing or corrupting existing state.

static void test_capacity_smoke()
{
    Graph& g = reset_unit_graph(0);

    uint32_t nodes[max_nodes] = {};
    for (uint32_t i = 0; i < max_nodes; ++i) {
        nodes[i] = g.create_node("n", vmath::vec2(0.0f, 0.0f));
        TEST(nodes[i] != Sculptor::pool_no_slot);
        if (nodes[i] == Sculptor::pool_no_slot) {
            return; // already reported above; skip out-of-bounds checks below
        }
    }
    // One past the node pool: refused safely, and stays refused.
    TEST(g.create_node("over", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);
    TEST(g.create_node("over", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);

    // A single node holds max_node_slots slots.
    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        TEST(g.add_slot(nodes[0], in_slot) != Sculptor::pool_no_slot);
    }
    TEST(g.add_slot(nodes[0], in_slot) == Sculptor::pool_no_slot);
    TEST(g.add_slot(nodes[0], in_slot) == Sculptor::pool_no_slot);

    // Connections: the raw API allows fan-in onto one input slot (see
    // test_add_delete_connection_basics), so a single output/input pair
    // saturates the connection pool.
    Slot out_slot      = {};
    out_slot.kind      = SlotKind::output;
    const uint32_t out = g.add_slot(nodes[1], out_slot);
    const uint32_t in  = g.add_slot(nodes[1], in_slot);
    TEST(out != Sculptor::pool_no_slot);
    TEST(in != Sculptor::pool_no_slot);
    for (uint32_t i = 0; i < max_connections; ++i) {
        TEST(g.add_connection(EndPoint{ nodes[1], out }, EndPoint{ nodes[1], in }) != Sculptor::pool_no_slot);
    }
    TEST(g.add_connection(EndPoint{ nodes[1], out }, EndPoint{ nodes[1], in }) == Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{ nodes[1], out }, EndPoint{ nodes[1], in }) == Sculptor::pool_no_slot);
    TEST(g.connection_count() == max_connections);
    TEST(g.get_connection(max_connections - 1).output.node_idx == nodes[1]);
}

// ---------------------------------------------------------------------------
// Canvas menu callback: the popup itself lives in the renderer
// (sculptor_graph_render.cpp), so only the callback type and setter are
// reachable headlessly.  The callback body runs the same way here as the
// render side runs it when the empty-canvas popup opens.

static void canvas_menu_adds_node(void* user_data)
{
    static_cast<Graph*>(user_data)->create_node("canvas", vmath::vec2(0.0f, 0.0f));
}

// Connection capacity covers the editor's worst-case oscillator graph (385
// reachable edges at seven layers with per-target blocks); the pool must
// hold every reachable state, not just the common one.
static void test_connection_capacity_raised_to_512()
{
    TEST(max_connections == 512);

    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t source = g.create_node("src", vmath::vec2(0.0f, 0.0f));
    TEST(source != Sculptor::pool_no_slot);
    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    TEST(g.add_slot(source, out_slot) != Sculptor::pool_no_slot);

    // 512 single-connection inputs across sixteen sink nodes (120 slots each,
    // the full per-node slot pool).
    Slot in_slot       = {};
    in_slot.kind       = SlotKind::input;
    uint32_t sinks[16] = {};
    for (uint32_t sink = 0; sink < 16; ++sink) {
        sinks[sink] = g.create_node("sink", vmath::vec2(0.0f, 0.0f));
        TEST(sinks[sink] != Sculptor::pool_no_slot);
        for (uint32_t s = 0; s < max_node_slots; ++s) {
            TEST(g.add_slot(sinks[sink], in_slot) != Sculptor::pool_no_slot);
        }
    }
    for (uint32_t i = 0; i < 512; ++i) {
        const EndPoint out = { source, 0 };
        const EndPoint in  = { sinks[i / max_node_slots], i % max_node_slots };
        TEST(g.add_connection(out, in) != Sculptor::pool_no_slot);
    }
    TEST(g.connection_count() == 512);
}

// Per-slot "missing" render state, editor-settable so the renderer draws
// unconnected sum endpoints red after the oscillator-sum edge is deleted.
static void test_slot_missing_flag_api()
{
    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t node = g.create_node("sum", vmath::vec2(0.0f, 0.0f));
    TEST(node != Sculptor::pool_no_slot);
    Slot in                   = {};
    in.kind                   = SlotKind::input;
    const uint32_t slot       = g.add_slot(node, in);
    const uint32_t other_slot = g.add_slot(node, in);
    TEST(slot != Sculptor::pool_no_slot && other_slot != Sculptor::pool_no_slot);
    TEST(! g.slot_missing(node, slot)); // default: not missing
    g.set_slot_missing(node, slot, true);
    TEST(g.slot_missing(node, slot));
    TEST(! g.slot_missing(node, other_slot)); // per-slot, not per-node
    g.set_slot_missing(node, slot, false);
    TEST(! g.slot_missing(node, slot));
    // Missing marks never survive a wholesale rebuild: a reused (node, slot)
    // index must not inherit a stale red dot.
    g.set_slot_missing(node, slot, true);
    g.clear();
    TEST(! g.slot_missing(node, slot));
    TEST(! g.slot_missing(node, other_slot));
}

// Eventless property write for the shared-routing fan-out - sets the
// slot value without pushing a change event.
static void test_eventless_slot_value_write()
{
    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t node = g.create_node("osc", vmath::vec2(0.0f, 0.0f));
    TEST(node != Sculptor::pool_no_slot);
    Slot prop           = {};
    prop.kind           = SlotKind::property;
    prop.property_type  = PropertyType::real;
    prop.value.real     = 1.0f;
    const uint32_t slot = g.add_slot(node, prop);
    TEST(slot != Sculptor::pool_no_slot);
    GraphChange changes[8] = {};
    TEST(g.take_changes(changes, 8) == 0);
    Sculptor::PropertyValue edited = {};
    edited.real                    = 3.5f;
    g.set_slot_value(node, slot, edited);
    TEST(g.node(node).slots.entries[slot].value.real == 3.5f);
    TEST(g.take_changes(changes, 8) == 0); // eventless: no echo
}

// Eventless connection write for apply-side mirroring - the single edge
// entering an input slot is made to carry exactly the requested output
// (added, rewritten, removed) without pushing a change event.
static void test_eventless_slot_input_write()
{
    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t src = g.create_node("src", vmath::vec2(0.0f, 0.0f));
    const uint32_t dst = g.create_node("dst", vmath::vec2(64.0f, 0.0f));
    TEST(src != Sculptor::pool_no_slot && dst != Sculptor::pool_no_slot);
    Slot out                = {};
    out.kind                = SlotKind::output;
    const uint32_t out_slot = g.add_slot(src, out);
    Slot           in       = {};
    in.kind                 = SlotKind::input;
    const uint32_t in_slot  = g.add_slot(dst, in);
    TEST(out_slot != Sculptor::pool_no_slot && in_slot != Sculptor::pool_no_slot);

    // Drain the setup traffic so the ring is armed: the zero-events assertion
    // below must be able to fail.
    GraphChange setup_changes[8] = {};
    g.take_changes(setup_changes, 8); // discard setup traffic, re-arm the ring

    // Add: the slot had no edge, one is allocated.
    g.set_slot_input(dst, in_slot, EndPoint{ src, out_slot });
    TEST(g.connection_count() == 1);
    TEST(g.get_connection(0).input.node_idx == dst && g.get_connection(0).input.slot_idx == in_slot);
    TEST(g.get_connection(0).output.node_idx == src && g.get_connection(0).output.slot_idx == out_slot);

    // Rewrite: still exactly one edge entering the slot, new output.
    const uint32_t src2 = g.create_node("src2", vmath::vec2(0.0f, 64.0f));
    TEST(src2 != Sculptor::pool_no_slot);
    Slot out2                = {};
    out2.kind                = SlotKind::output;
    const uint32_t out_slot2 = g.add_slot(src2, out2);
    TEST(out_slot2 != Sculptor::pool_no_slot);
    g.set_slot_input(dst, in_slot, EndPoint{ src2, out_slot2 });
    TEST(g.connection_count() == 1);
    TEST(g.get_connection(0).output.node_idx == src2 && g.get_connection(0).output.slot_idx == out_slot2);

    // Remove: the edge is freed, the slot is open again.
    g.set_slot_input(dst, in_slot, EndPoint{ Sculptor::pool_no_slot, Sculptor::pool_no_slot });
    TEST(g.connection_count() == 0);

    // Input slots only: a connectable property slot is not a mirror target.
    Slot prop                = {};
    prop.kind                = SlotKind::property;
    prop.connectable         = true;
    const uint32_t prop_slot = g.add_slot(dst, prop);
    TEST(prop_slot != Sculptor::pool_no_slot);
    g.set_slot_input(dst, prop_slot, EndPoint{ src, out_slot });
    TEST(g.connection_count() == 0);

    // Eventless throughout: node/slot setup events may reach the drain, but
    // no connection event may - the slot-input writes must stay eventless.
    GraphChange    changes[8]       = {};
    const uint32_t drained          = g.take_changes(changes, 8);
    bool           connection_event = false;
    for (uint32_t i = 0; i < drained; i++) {
        connection_event = connection_event || changes[i].kind == ChangeKind::connection_added ||
                           changes[i].kind == ChangeKind::connection_deleted ||
                           changes[i].kind == ChangeKind::connection_changed;
    }
    TEST(! connection_event);
}

// The canvas menu is fully caller-provided ("Add Oscillator"/"Add
// Envelope"/"Add LFO" belong to the caller's menu callback).
static void test_canvas_menu_contract_caller_provided() {}

static void test_canvas_menu_callback_api()
{
    Graph& g = reset_unit_graph(0);

    g.set_canvas_menu_callback(canvas_menu_adds_node, &g);
    Sculptor::CanvasMenuCallback callback = canvas_menu_adds_node;

    GraphChange scratch[8] = {};
    drain_changes(g, scratch, 8);
    callback(&g);

    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 1);
    TEST(events[0].kind == ChangeKind::node_added);
    TEST(strcmp(g.node(0).name, "canvas") == 0);
}

// clear() must leave no stale interaction state behind: the error overlay
// is dismissed and the canvas popup target is reset so the renderer's
// Delete item cannot act on a reused connection index.
static void test_clear_resets_popup_and_error()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);
    TEST(node_b != Sculptor::pool_no_slot);
    Slot slot_a = {};
    slot_a.kind = SlotKind::output;
    TEST(g.add_slot(node_a, slot_a) != Sculptor::pool_no_slot);
    Slot slot_b = {};
    slot_b.kind = SlotKind::input;
    TEST(g.add_slot(node_b, slot_b) != Sculptor::pool_no_slot);
    const EndPoint out = { node_a, 0 };
    const EndPoint in  = { node_b, 0 };
    TEST(g.add_connection(out, in) != Sculptor::pool_no_slot);

    // A refused connect attempt leaves the error overlay active.
    TEST(! g.attempt_connection(out, out));
    TEST(g.has_error());

    g.clear();
    TEST(! g.has_error());
    TEST(g.connection_popup() == Sculptor::pool_no_slot);

    // State is clean after a rebuild into the same graph.
    const uint32_t node_c = g.create_node("c", vmath::vec2(0.0f, 0.0f));
    TEST(node_c != Sculptor::pool_no_slot);
    Slot slot_c = {};
    slot_c.kind = SlotKind::output;
    TEST(g.add_slot(node_c, slot_c) != Sculptor::pool_no_slot);
    const uint32_t node_d = g.create_node("d", vmath::vec2(0.0f, 0.0f));
    TEST(node_d != Sculptor::pool_no_slot);
    Slot slot_d = {};
    slot_d.kind = SlotKind::input;
    TEST(g.add_slot(node_d, slot_d) != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{ node_c, 0 }, EndPoint{ node_d, 0 }) != Sculptor::pool_no_slot);
    TEST(! g.has_error());
    TEST(g.connection_popup() == Sculptor::pool_no_slot);
}

// Row groups pack several slots of one node onto a single renderer line
// (connector dot first, then widgets left to right).  The model stores only
// the per-slot group id and the slot order; the renderer owns the line math.
static void test_row_group_layout_inputs()
{
    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t node = g.create_node("param", vmath::vec2(0.0f, 0.0f));
    TEST(node != Sculptor::pool_no_slot);

    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    snprintf(out_slot.name, sizeof(out_slot.name), "out");
    TEST(g.add_slot(node, out_slot) != Sculptor::pool_no_slot); // group 0: own line

    // One lfo-style line: input connector followed by two widget slots.
    Slot lfo_in      = {};
    lfo_in.kind      = SlotKind::input;
    lfo_in.row_group = 3;
    snprintf(lfo_in.name, sizeof(lfo_in.name), "lfo");
    TEST(g.add_slot(node, lfo_in) != Sculptor::pool_no_slot);

    Slot lfo_op             = {};
    lfo_op.kind             = SlotKind::property;
    lfo_op.property_type    = PropertyType::list;
    lfo_op.num_list_options = 2;
    lfo_op.row_group        = 3;
    snprintf(lfo_op.name, sizeof(lfo_op.name), "op");
    snprintf(lfo_op.list_options[0], sizeof(lfo_op.list_options[0]), "add");
    snprintf(lfo_op.list_options[1], sizeof(lfo_op.list_options[1]), "multiply");
    TEST(g.add_slot(node, lfo_op) != Sculptor::pool_no_slot);

    Slot lfo_amount          = {};
    lfo_amount.kind          = SlotKind::property;
    lfo_amount.property_type = PropertyType::real;
    lfo_amount.value.real    = 0.5f;
    lfo_amount.row_group     = 3;
    snprintf(lfo_amount.name, sizeof(lfo_amount.name), "amount");
    TEST(g.add_slot(node, lfo_amount) != Sculptor::pool_no_slot);

    // A second grouped line keeps its own group id.
    Slot scale          = {};
    scale.kind          = SlotKind::property;
    scale.property_type = PropertyType::real;
    scale.row_group     = 5;
    snprintf(scale.name, sizeof(scale.name), "scale");
    TEST(g.add_slot(node, scale) != Sculptor::pool_no_slot);

    const Node& n = g.node(node);
    TEST(n.slots.num_allocated == 5);
    TEST(n.slots.entries[0].row_group == 0); // ungrouped slots own their line
    TEST(n.slots.entries[1].row_group == 3);
    TEST(n.slots.entries[2].row_group == 3);
    TEST(n.slots.entries[3].row_group == 3);
    TEST(n.slots.entries[4].row_group == 5);
}

// Connectable dynamic value rows are one-edge inputs whose inline value
// stays a live view while wired; the one-edge refusal names the wired node.
static void test_connectable_dynamic_value_row()
{
    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t src = g.create_node("Velocity", vmath::vec2(0.0f, 0.0f));
    const uint32_t dst = g.create_node("osc", vmath::vec2(96.0f, 0.0f));
    TEST(src != Sculptor::pool_no_slot && dst != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    TEST(g.add_slot(src, out) != Sculptor::pool_no_slot);
    Slot gain          = {};
    gain.kind          = SlotKind::property;
    gain.connectable   = true;
    gain.property_type = PropertyType::real;
    gain.value.real    = 0.5f;
    snprintf(gain.name, sizeof(gain.name), "volume");
    const uint32_t gain_slot = g.add_slot(dst, gain);
    TEST(gain_slot != Sculptor::pool_no_slot);

    TEST(g.add_connection(EndPoint{ src, 0 }, EndPoint{ dst, gain_slot }) != Sculptor::pool_no_slot);
    bool connected = false;
    for (uint32_t i = 0; i < Sculptor::max_connections; i++) {
        if (g.connection_occupied(i) && g.get_connection(i).input.node_idx == dst &&
            g.get_connection(i).input.slot_idx == gain_slot) {
            connected = true;
        }
    }
    TEST(connected);

    // One-edge rule: a second source is refused and the error names the node
    // already wired into the input.
    const uint32_t src2 = g.create_node("Mod wheel", vmath::vec2(0.0f, 64.0f));
    TEST(src2 != Sculptor::pool_no_slot);
    Slot out2 = {};
    out2.kind = SlotKind::output;
    TEST(g.add_slot(src2, out2) != Sculptor::pool_no_slot);
    GraphChange drain[8] = {};
    g.take_changes(drain, 8); // re-arm the ring
    TEST(! g.attempt_connection(EndPoint{ src2, 0 }, EndPoint{ dst, gain_slot }));
    TEST(g.has_error());
    TEST(strstr(g.error_text(), "Velocity") != nullptr); // names the wired node
    TEST(g.connection_count() == 1);                     // the refused attempt added nothing

    // Grey-out is presentational: the value stays a live view of the same
    // storage while connected.
    Sculptor::PropertyValue edited = {};
    edited.real                    = 0.9f;
    g.set_slot_value(dst, gain_slot, edited);
    TEST(g.node(dst).slots.entries[gain_slot].value.real == 0.9f);
}

static void test_retarget_occupied_input_without_interceptor()
{
    Graph&         graph  = reset_unit_graph(0);
    const uint32_t source = graph.create_node("source", {});
    const uint32_t other  = graph.create_node("other", {});
    const uint32_t first  = graph.create_node("first", {});
    const uint32_t second = graph.create_node("second", {});
    Slot           output = {};
    output.kind           = SlotKind::output;
    Slot input            = {};
    input.kind            = SlotKind::input;
    graph.add_slot(source, output);
    graph.add_slot(other, output);
    graph.add_slot(first, input);
    graph.add_slot(second, input);
    const uint32_t wire = graph.add_connection({ source, 0 }, { first, 0 });
    TEST(graph.add_connection({ other, 0 }, { second, 0 }) != Sculptor::pool_no_slot);
    GraphChange changes[max_pending_changes];
    graph.take_changes(changes, max_pending_changes);
    TEST(! graph.move_connection_end(wire, false, { second, 0 }));
    TEST(graph.has_error());
    TEST(graph.take_changes(changes, max_pending_changes) == 0);
    TEST(graph.get_connection(wire).input.node_idx == first);
    TEST(graph.get_connection(wire).output.node_idx == source);
    TEST(! graph.attempt_connection({ source, 0 }, { second, 0 }));
    TEST(graph.take_changes(changes, max_pending_changes) == 0);
}

static void test_peek_changes_preserves_ring()
{
    Graph&         graph = reset_unit_graph(0);
    const uint32_t node  = graph.create_node("node", {});
    GraphChange    first[Sculptor::max_pending_changes];
    GraphChange    second[Sculptor::max_pending_changes];
    graph.take_changes(first, Sculptor::max_pending_changes);
    for (uint32_t index = 0; index < 300; ++index) {
        graph.rename_node(node, index % 2 ? "A" : "B");
        bool           overflow = true;
        const uint32_t count    = graph.peek_changes(first, Sculptor::max_pending_changes, &overflow);
        TEST(count == 1 && ! overflow);
        TEST(graph.peek_changes(second, Sculptor::max_pending_changes, &overflow) == count);
        TEST(memcmp(first, second, count * sizeof(first[0])) == 0);
        TEST(graph.peek_changes(nullptr, 0, &overflow) == 0 && overflow);
        TEST(graph.take_changes(second, Sculptor::max_pending_changes) == count);
        TEST(memcmp(first, second, count * sizeof(first[0])) == 0);
    }
    for (uint32_t index = 0; index <= Sculptor::max_pending_changes; ++index) {
        graph.rename_node(node, index % 2 ? "A" : "B");
    }
    bool overflow = false;
    TEST(graph.peek_changes(first, Sculptor::max_pending_changes, &overflow) == Sculptor::max_pending_changes);
    TEST(overflow);
    TEST(graph.peek_changes(second, 1, &overflow) == 1 && overflow);
    TEST(graph.peek_changes(nullptr, 1, &overflow) == 0 && overflow);
    TEST(graph.peek_changes(second, Sculptor::max_pending_changes, &overflow) == Sculptor::max_pending_changes);
    TEST(memcmp(first, second, sizeof(first)) == 0);
    TEST(graph.take_changes(second, Sculptor::max_pending_changes) == Sculptor::max_pending_changes);
    TEST(memcmp(first, second, sizeof(first)) == 0);
    TEST(graph.changes_overflowed());
    TEST(! graph.changes_overflowed());
}

static void test_user_disconnect_and_intent_capacity()
{
    Graph&         graph  = reset_unit_graph(0);
    const uint32_t source = graph.create_node("source", {});
    const uint32_t target = graph.create_node("target", {});
    graph.add_slot(source, Sculptor::output_slot("Out"));
    graph.add_slot(target, Sculptor::input_slot("In"));
    const uint32_t wire = graph.add_connection({ source, 0 }, { target, 0 });
    GraphChange    changes[max_pending_changes];
    graph.take_changes(changes, max_pending_changes);
    // The pending-change ring saturates: an event beyond capacity is
    // dropped and reported through the overflow flag, never silently
    // wedging later edits.
    for (uint32_t index = 0; index < max_pending_changes; ++index) {
        graph.rename_node(target, index % 2 ? "A" : "B");
    }
    graph.rename_node(target, "overflow-probe");
    bool overflow = false;
    TEST(graph.peek_changes(changes, max_pending_changes, &overflow) == max_pending_changes);
    TEST(overflow);
    graph.take_changes(changes, max_pending_changes);
    TEST(graph.changes_overflowed());
    TEST(graph.user_delete_connection(wire));
    TEST(! graph.user_delete_connection(wire));
    TEST(graph.take_changes(changes, max_pending_changes) == 1);
    TEST(changes[0].kind == ChangeKind::connection_deleted);
    const uint32_t next_wire = graph.add_connection({ source, 0 }, { target, 0 });
    graph.take_changes(changes, max_pending_changes);
    graph.delete_node(target);
    TEST(! graph.connection_occupied(next_wire));
    graph.clear();
    graph.take_changes(changes, max_pending_changes);
    const uint32_t next_source = graph.create_node("source", {});
    const uint32_t next_target = graph.create_node("target", {});
    graph.add_slot(next_source, Sculptor::output_slot("Out"));
    graph.add_slot(next_target, Sculptor::input_slot("In"));
    const uint32_t rebuilt_wire = graph.add_connection({ next_source, 0 }, { next_target, 0 });
    graph.take_changes(changes, max_pending_changes);
    TEST(graph.user_delete_connection(rebuilt_wire));
    TEST(! graph.connection_occupied(rebuilt_wire));
    // Generic self wiring refuses on structural grounds alone.
    const uint32_t self_wire = graph.add_connection({ next_source, 0 }, { next_target, 0 });
    graph.take_changes(changes, max_pending_changes);
    TEST(! graph.move_connection_end(self_wire, false, { next_source, 0 }));
    TEST(graph.take_changes(changes, max_pending_changes) == 0);
}

static uint32_t render_frame(Graph& graph, ImVec2 mouse, bool down, bool escape = false, bool right = false)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, down);
    io.AddMouseButtonEvent(1, right);
    io.AddKeyEvent(ImGuiKey_Escape, escape);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1024, 768));
    ImGui::Begin("graph mouse regression",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    graph.render(vmath::vec2(1000, 740), nullptr);
    const ImDrawList* draw              = ImGui::GetWindowDrawList();
    uint32_t          old_wire_vertices = 0;
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == IM_COL32(17, 29, 43, 255)) {
            ++old_wire_vertices;
        }
    }
    ImGui::End();
    ImGui::Render();
    return old_wire_vertices;
}

static ImVec2 audio_dot(const Graph& graph, uint32_t node, bool output)
{
    // Dot rows use the actual built-in font metrics and override-aware measured width.
    const float line_height  = ImGui::GetFontSize();
    const float frame_height = line_height + 2 * ImGui::GetStyle().FramePadding.y;
    return ImVec2(8 + graph.node(node).position.x + (output ? graph.content_size(node).x : 0),
                  8 + graph.node(node).position.y + 8 + line_height + (frame_height + 4) * 0.5f);
}

static void test_renderer_audio_endpoint_mouse_frames()
{
    ImGui::CreateContext();
    ImGuiIO& io                     = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.LogFilename                  = nullptr;
    io.DisplaySize                  = ImVec2(1024, 768);
    io.DeltaTime                    = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels           = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    TEST(pixels && width > 0 && height > 0);
    for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
        for (uint32_t action = 0; action < 8; ++action) {
            ImGui::ClosePopupsOverWindow(nullptr, false);
            Graph& graph = reset_unit_graph(0);
            graph.clear();
            GraphColors colors = Sculptor::default_graph_colors();
            colors.connection  = 0x111D2BFF;
            graph.set_colors(colors);
            uint32_t nodes[3];
            for (uint32_t i = 0; i < 3; ++i) {
                nodes[i]     = graph.create_node("Effect", vmath::vec2(64.0f + 256.0f * float(i), 64.0f));
                Slot in      = {};
                in.kind      = SlotKind::input;
                in.row_group = 1;
                strcpy(in.name, "In");
                Slot out      = {};
                out.kind      = SlotKind::output;
                out.row_group = 1;
                strcpy(out.name, "Out");
                TEST(graph.add_slot(nodes[i], in) == 0);
                TEST(graph.add_slot(nodes[i], out) == 1);
            }
            const uint32_t wire = graph.add_connection({ nodes[0], 1 }, { nodes[1], 0 });
            if (action == 4) {
                graph.add_connection({ nodes[0], 1 }, { nodes[2], 0 });
            }
            GraphChange changes[16];
            graph.take_changes(changes, 16);
            render_frame(graph, ImVec2(900, 600), false);
            const uint32_t idle_vertices = render_frame(graph, ImVec2(900, 600), false);
            TEST(idle_vertices > 0);
            const bool   moving_output = endpoint == 1;
            const ImVec2 grabbed       = audio_dot(graph, nodes[moving_output ? 0 : 1], moving_output);
            render_frame(graph, grabbed, false);
            const uint32_t pickup_vertices = render_frame(graph, grabbed, true);
            TEST(graph.interaction_active());
            if (! graph.interaction_active()) {
                render_frame(graph, grabbed, false);
                continue;
            }
            TEST(pickup_vertices < idle_vertices);
            TEST(graph.connection_occupied(wire));
            TEST(graph.take_changes(changes, 16) == 0);
            ImVec2 target = audio_dot(graph, nodes[2], moving_output);
            if (action == 1 || action == 2 || action == 6 || action == 7) {
                target = ImVec2(900, 600);
            }
            else if (action == 3) {
                target = grabbed;
            }
            else if (action == 5) {
                target = audio_dot(graph, nodes[2], ! moving_output);
            }
            const uint32_t held_vertices = render_frame(graph, target, true);
            TEST(held_vertices < idle_vertices);
            if (action == 0) {
                const ImVec2 a = audio_dot(graph, nodes[0], true);
                const ImVec2 b = audio_dot(graph, nodes[1], false);
                render_frame(graph, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), true, false, true);
                TEST(graph.connection_popup() == Sculptor::pool_no_slot);
                render_frame(graph, target, true);
            }
            if (action == 7) {
                graph.move_connection_end(wire, false, { nodes[0], 1 });
                TEST(graph.has_error());
            }
            const bool     cancel         = action == 2 || action == 6 || action == 7;
            const uint32_t final_vertices = render_frame(graph, target, action == 2 || action == 7, cancel);
            TEST(! graph.interaction_active());
            const uint32_t count = graph.take_changes(changes, 16);
            if (cancel || action == 3 || action == 5 || (action == 4 && ! moving_output)) {
                TEST(graph.connection_occupied(wire));
                if (graph.connection_occupied(wire)) {
                    TEST(graph.get_connection(wire).output.node_idx == nodes[0]);
                    TEST(graph.get_connection(wire).input.node_idx == nodes[1]);
                }
                TEST(count == 0);
                if (cancel) {
                    TEST(final_vertices >= idle_vertices);
                }
                if (action == 5 || (action == 4 && ! moving_output)) {
                    TEST(graph.has_error());
                }
            }
            else if (action == 1) {
                TEST(! graph.connection_occupied(wire));
                TEST(count == 1);
                if (count == 1) {
                    TEST(changes[0].kind == ChangeKind::connection_deleted);
                }
                TEST(! graph.has_error());
            }
            else {
                TEST(graph.connection_occupied(wire));
                if (graph.connection_occupied(wire)) {
                    TEST(graph.get_connection(wire).output.node_idx == nodes[moving_output ? 2 : 0]);
                    TEST(graph.get_connection(wire).input.node_idx == nodes[moving_output ? 1 : 2]);
                }
                TEST(count == 1);
                if (count == 1) {
                    TEST(changes[0].kind == ChangeKind::connection_changed);
                }
            }
            render_frame(graph, ImVec2(900, 600), false);
        }
    }
    const float zooms[] = { Sculptor::graph_min_zoom, Sculptor::graph_max_zoom };
    for (float scale : zooms) {
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            ImGui::ClosePopupsOverWindow(nullptr, false);
            Graph& graph = reset_unit_graph(0);
            graph.clear();
            graph.set_colors(Sculptor::default_graph_colors());
            const uint32_t source  = graph.create_node("A", { 16, 16 });
            const uint32_t target  = graph.create_node("B", { 96, 16 });
            const uint32_t nodes[] = { source, target };
            for (uint32_t node : nodes) {
                Slot in       = {};
                in.kind       = SlotKind::input;
                in.row_group  = 1;
                Slot out      = {};
                out.kind      = SlotKind::output;
                out.row_group = 1;
                graph.add_slot(node, in);
                graph.add_slot(node, out);
                graph.set_node_layout(node, graph.node(node).position, 64, 64);
            }
            const uint32_t wire = graph.add_connection({ source, 1 }, { target, 0 });
            GraphChange    changes[16];
            graph.take_changes(changes, 16);
            render_frame(graph, ImVec2(8, 8), false);
            render_frame(graph, ImVec2(8, 8), false);
            io.AddMouseWheelEvent(0, scale < 1 ? -100.0f : 100.0f);
            render_frame(graph, ImVec2(8, 8), false);
            render_frame(graph, ImVec2(8, 8), false);
            const uint32_t node  = endpoint ? source : target;
            const float    font  = ImGui::GetFontSize();
            const float    frame = font + 2 * ImGui::GetStyle().FramePadding.y;
            const ImVec2   dot(8 + graph.node(node).position.x * scale + (endpoint ? graph.content_size(node).x : 0),
                               8 + graph.node(node).position.y * scale + (8 + font + (frame + 4) * 0.5f) * scale);
            render_frame(graph, dot, false);
            render_frame(graph, dot, true);
            TEST(graph.interaction_active());
            render_frame(graph, dot, false, true);
            TEST(! graph.interaction_active());
            TEST(graph.connection_occupied(wire));
            TEST(graph.take_changes(changes, 16) == 0);
            // Reprojection clears the old gesture before another pickup.
            graph.clear();
            const uint32_t fresh = graph.create_node("fresh", { 16, 16 });
            Slot           out   = {};
            out.kind             = SlotKind::output;
            graph.add_slot(fresh, out);
            render_frame(graph, ImVec2(8, 8), false);
            const float x = 8 + 16 * scale + graph.content_size(fresh).x;
            const float y = 8 + 16 * scale + (8 + font + (frame + 4) * 0.5f) * scale;
            render_frame(graph, ImVec2(x, y), true);
            TEST(graph.interaction_active());
            render_frame(graph, ImVec2(x, y), false, true);
            TEST(! graph.interaction_active());
        }
    }
    ImGui::DestroyContext();
}

int main()
{
    test_renderer_audio_endpoint_mouse_frames();
    test_user_disconnect_and_intent_capacity();
    test_retarget_occupied_input_without_interceptor();
    test_peek_changes_preserves_ring();
    test_create_node_distinct_indices();
    test_create_node_exhaustion_is_safe();
    test_create_node_snaps_position_to_grid();
    test_add_slot_grows_within_limit();
    test_add_slot_overflow_returns_no_slot();
    test_delete_node_frees_index_and_drops_connections();
    test_add_delete_connection_basics();
    test_clear_resets_popup_and_error();
    test_connection_capacity_raised_to_512();
    test_slot_missing_flag_api();
    test_eventless_slot_value_write();
    test_eventless_slot_input_write();
    test_canvas_menu_contract_caller_provided();

    test_add_connection_rejects_bad_endpoints();
    test_ghost_is_flag_on_normal_node();
    test_colors_api();
    test_change_events_queue_and_drain();
    test_change_events_overflow_is_reported();
    test_attempt_connection_success();
    test_attempt_connection_validator_rejects();
    test_attempt_connection_structural_rejection_sets_error();
    test_move_connection_end_success();
    test_move_connection_end_failure_snaps_back();
    test_move_connection_end_structural_failure_snaps_back();
    test_selection_basics();
    test_delete_node_clears_selection();
    test_delete_selected();
    test_selection_api_ignores_unoccupied_slots();
    test_align_left_and_top();
    test_align_right_and_bottom();
    test_align_equal_width_and_height();
    test_align_equal_height_applies_as_equal_width();
    test_align_skips_ghosts_and_empty_selection();
    test_delete_node_veto();
    test_capacity_smoke();
    test_canvas_menu_callback_api();
    test_row_group_layout_inputs();
    test_connectable_dynamic_value_row();

    return exit_code;
}
