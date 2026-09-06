// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Unit tests for the Sculptor::Graph M1 data model.
// Uses only the public API declared in sculptor_graph.h per the approved
// M1 plan.  TEST-macro pattern copied from sculptor_undo_unit.cpp.

#include "sculptor_graph.h"

#include <stdio.h>
#include <string.h>

static int exit_code = 0;

#define TEST(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit_code = 1; \
    } \
} while(0)

using Sculptor::ChangeKind;
using Sculptor::EndPoint;
using Sculptor::Graph;
using Sculptor::GraphChange;
using Sculptor::GraphColors;
using Sculptor::max_connections;
using Sculptor::max_node_slots;
using Sculptor::max_nodes;
using Sculptor::max_pending_changes;
using Sculptor::Connection;
using Sculptor::Node;
using Sculptor::PropertyType;
using Sculptor::Slot;
using Sculptor::SlotKind;

static uint32_t drain_changes(Graph& graph, GraphChange* out, uint32_t out_size)
{
    const uint32_t count = graph.take_changes(out, out_size);
    return count;
}

static void test_create_node_distinct_indices()
{
    Graph g;

    uint32_t idx[max_nodes]  = {};
    bool     seen[max_nodes] = {};

    for (uint32_t i = 0; i < max_nodes; ++i) {
        idx[i] = g.create_node("node", vmath::vec2(0.0f, 0.0f));
        TEST(idx[i] != Sculptor::pool_no_slot);
        if (idx[i] == Sculptor::pool_no_slot) {
            break;  // already reported above; skip out-of-bounds checks below
        }
        TEST(idx[i] < max_nodes);
        TEST(!seen[idx[i]]);
        seen[idx[i]] = true;
        TEST(!g.node(idx[i]).ghost);
    }
}

static void test_create_node_exhaustion_is_safe()
{
    Graph g;

    for (uint32_t i = 0; i < max_nodes; ++i) {
        TEST(g.create_node("node", vmath::vec2(0.0f, 0.0f)) != Sculptor::pool_no_slot);
    }

    // Pool is full: the extra allocation must fail safely, not crash.
    TEST(g.create_node("overflow", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);
    TEST(g.create_node("overflow", vmath::vec2(0.0f, 0.0f)) == Sculptor::pool_no_slot);
}

static void test_create_node_snaps_position_to_grid()
{
    Graph g;

    const uint32_t idx = g.create_node("node", vmath::vec2(10.0f, 23.0f));
    TEST(idx != Sculptor::pool_no_slot);
    const vmath::vec2 pos = g.node(idx).position;
    TEST(pos.x == 0.0f);   // 10 snapped down to the 16px grid
    TEST(pos.y == 16.0f);  // 23 snapped down to the 16px grid
}

static void test_add_slot_grows_within_limit()
{
    Graph g;

    const uint32_t node_idx = g.create_node("node", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);

    bool seen[max_node_slots] = {};

    for (uint32_t i = 0; i < max_node_slots; ++i) {
        Slot slot     = {};
        slot.kind     = SlotKind::input;
        const uint32_t slot_idx = g.add_slot(node_idx, slot);
        TEST(slot_idx != Sculptor::pool_no_slot);
        if (slot_idx == Sculptor::pool_no_slot) {
            break;  // already reported above; skip out-of-bounds checks below
        }
        TEST(slot_idx < max_node_slots);
        TEST(!seen[slot_idx]);
        seen[slot_idx] = true;
    }
}

static void test_add_slot_overflow_returns_no_slot()
{
    Graph g;

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
    Graph g;

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);
    TEST(node_b != Sculptor::pool_no_slot);

    Slot slot_a = {};
    slot_a.kind = SlotKind::output;
    const uint32_t slot_a_idx = g.add_slot(node_a, slot_a);
    Slot slot_b = {};
    slot_b.kind = SlotKind::input;
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
    GraphChange events[8] = {};
    const uint32_t num_events       = drain_changes(g, events, 8);
    uint32_t num_connection_deleted = 0;
    uint32_t num_node_deleted       = 0;
    uint32_t num_node_added         = 0;
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
    Graph g;

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
            break;  // already reported above; skip out-of-bounds checks below
        }
        TEST(con < max_connections);
        TEST(!seen[con]);
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
    Graph g;

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
    Slot output_slot = {};
    output_slot.kind = SlotKind::output;
    const uint32_t output_idx = g.add_slot(node_a, output_slot);
    TEST(output_idx != Sculptor::pool_no_slot);
    const EndPoint valid_output = { node_a, output_idx };
    Slot input_slot = {};
    input_slot.kind = SlotKind::input;
    const uint32_t input_idx = g.add_slot(node_c, input_slot);
    TEST(input_idx != Sculptor::pool_no_slot);
    Slot plain_property = {};
    plain_property.kind = SlotKind::property;
    plain_property.connectable = false;
    const uint32_t plain_idx = g.add_slot(node_c, plain_property);
    TEST(plain_idx != Sculptor::pool_no_slot);
    Slot connectable_property = {};
    connectable_property.kind = SlotKind::property;
    connectable_property.connectable = true;
    const uint32_t connectable_idx = g.add_slot(node_c, connectable_property);
    TEST(connectable_idx != Sculptor::pool_no_slot);

    const EndPoint wrong_direction = { node_c, input_idx };  // input slot as output end
    TEST(g.add_connection(wrong_direction, valid_empty_slot) == Sculptor::pool_no_slot);

    const EndPoint plain_property_ep = { node_c, plain_idx };  // not connectable
    TEST(g.add_connection(valid_output, plain_property_ep) == Sculptor::pool_no_slot);

    const EndPoint connectable_ep = { node_c, connectable_idx };
    TEST(g.add_connection(valid_output, connectable_ep) != Sculptor::pool_no_slot);
}

static void test_ghost_is_flag_on_normal_node()
{
    Graph g;

    // The caller creates the node normally, then marks it as a ghost.
    const uint32_t node_idx = g.create_node("ghost", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);
    TEST(!g.node(node_idx).ghost);

    g.set_ghost(node_idx, true);
    TEST(g.node(node_idx).ghost);

    // Marking is idempotent and does not consume another pool entry.
    g.set_ghost(node_idx, true);
    TEST(g.node(node_idx).ghost);

    // Placement clears the flag and pushes ghost_placed.
    g.set_ghost(node_idx, false);
    TEST(!g.node(node_idx).ghost);
    GraphChange events[4] = {};
    const uint32_t num_events = drain_changes(g, events, 4);
    uint32_t num_ghost_placed = 0;
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
    Graph g;

    // set_colors copies the whole set (observable via colors()).
    GraphColors colors             = {};
    colors.node_background         = 0x202020FFu;
    colors.node_border             = 0x808080FFu;
    g.set_colors(colors);
    TEST(g.colors().node_background == 0x202020FFu);
    TEST(g.colors().node_border == 0x808080FFu);

    const uint32_t node_idx = g.create_node("node", vmath::vec2(0.0f, 0.0f));
    TEST(node_idx != Sculptor::pool_no_slot);

    g.set_node_color(node_idx, 0xFF0000FFu);  // override
    TEST(g.node(node_idx).color_override == 0xFF0000FFu);
    g.set_node_color(node_idx, 0);            // back to default set
    TEST(g.node(node_idx).color_override == 0);
    g.set_node_color(max_nodes, 0);           // out of range must not crash
}

static void test_change_events_queue_and_drain()
{
    Graph g;

    TEST(drain_changes(g, nullptr, 0) == 0);
    TEST(!g.changes_overflowed());

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    Slot slot_a = {};
    slot_a.kind = SlotKind::output;
    TEST(g.add_slot(node_a, slot_a) != Sculptor::pool_no_slot);
    Slot slot_b = {};
    slot_b.kind = SlotKind::input;
    TEST(g.add_slot(node_b, slot_b) != Sculptor::pool_no_slot);
    const EndPoint out = { node_a, 0 };
    const EndPoint in  = { node_b, 0 };
    const uint32_t con = g.add_connection(out, in);

    GraphChange events[8] = {};
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
    Graph g;

    // Overflow the ring buffer: more than max_pending_changes events without
    // draining.  Each cycle pushes two events (node_added, node_deleted).
    for (uint32_t i = 0; i <= max_pending_changes / 2 + 1; ++i) {
        const uint32_t idx = g.create_node("cycle", vmath::vec2(0.0f, 0.0f));
        if (idx == Sculptor::pool_no_slot) {
            break;  // pool exhaustion also overflows the queue; either is fine
        }
        g.delete_node(idx);
    }
    TEST(g.changes_overflowed());

    // The flag is reported once, then cleared.
    TEST(!g.changes_overflowed());
}

// --- M2: connections ---

static bool validator_rejects_same_node(void* user_data, const Sculptor::Graph& graph,
                                        EndPoint output, EndPoint input)
{
    return output.node_idx != input.node_idx;
}

static bool validator_rejects_all(void* user_data, const Sculptor::Graph& graph,
                                  EndPoint output, EndPoint input)
{
    return false;
}

struct TwoNodeFixture {
    Graph      graph;
    uint32_t   node_a;
    uint32_t   node_b;
    uint32_t   output_slot;
    uint32_t   input_slot;
    EndPoint   output_end;
    EndPoint   input_end;

    TwoNodeFixture()
    {
        node_a = graph.create_node("a", vmath::vec2(0.0f, 0.0f));
        node_b = graph.create_node("b", vmath::vec2(0.0f, 0.0f));
        Slot out_slot    = {};
        out_slot.kind    = SlotKind::output;
        output_slot      = graph.add_slot(node_a, out_slot);
        Slot in_slot     = {};
        in_slot.kind     = SlotKind::input;
        input_slot       = graph.add_slot(node_b, in_slot);
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
    TEST(!f.graph.has_error());

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
    TEST(!f.graph.attempt_connection(f.output_end, f.output_end));
    TEST(f.graph.has_error());
    TEST(f.graph.error_text() != nullptr);
    TEST(f.graph.error_text()[0] != '\0');
    TEST(f.graph.connection_count() == 0);

    GraphChange events[8];
    // Only the fixture's node_added + slot_added events; nothing added.
    TEST(drain_changes(f.graph, events, 8) == 4);
    TEST(events[3].kind != ChangeKind::connection_added);

    f.graph.dismiss_error();
    TEST(!f.graph.has_error());

    // Different nodes pass the validator and connect.
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(f.graph.connection_count() == 1);
    TEST(!f.graph.has_error());
}

static void test_attempt_connection_structural_rejection_sets_error()
{
    TwoNodeFixture f;

    // Occupied input: second attempt at the same input fails with error.
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(!f.graph.attempt_connection(f.output_end, f.input_end));
    TEST(f.graph.has_error());
    TEST(f.graph.connection_count() == 1);
    f.graph.dismiss_error();

    // Kind mismatch: output to output is rejected with error.
    TEST(!f.graph.attempt_connection(f.output_end, f.output_end));
    TEST(f.graph.has_error());
    TEST(f.graph.connection_count() == 1);
}

static void test_move_connection_end_success()
{
    TwoNodeFixture f;
    const uint32_t node_c = f.graph.create_node("c", vmath::vec2(0.0f, 0.0f));
    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    const uint32_t c_output = f.graph.add_slot(node_c, out_slot);
    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    const uint32_t c_input = f.graph.add_slot(node_c, in_slot);
    TEST(c_output != Sculptor::pool_no_slot);
    TEST(c_input != Sculptor::pool_no_slot);

    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32]; drain_changes(f.graph, scratch, 32);

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
    GraphChange events[8];
    const uint32_t num_events = drain_changes(f.graph, events, 8);
    TEST(num_events == 2);
    TEST(events[0].kind == ChangeKind::connection_changed);
    TEST(events[0].connection_idx == 0);
    TEST(events[1].kind == ChangeKind::connection_changed);
}

static void test_move_connection_end_failure_deletes()
{
    TwoNodeFixture f;
    f.graph.set_validator(validator_rejects_all, nullptr);

    // The validator also rejects move targets, so create the connection
    // without a validator, then install it.
    f.graph.set_validator(nullptr, nullptr);
    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32];
    drain_changes(f.graph, scratch, 32);
    f.graph.set_validator(validator_rejects_all, nullptr);

    const EndPoint nowhere = { f.node_b, f.input_slot };
    TEST(!f.graph.move_connection_end(0, false, nowhere));
    // Failed retarget destroys the connection, same rule as a failed drop.
    TEST(f.graph.connection_count() == 0);
    TEST(f.graph.has_error());

    GraphChange events[8];
    TEST(drain_changes(f.graph, events, 8) == 1);
    TEST(events[0].kind == ChangeKind::connection_deleted);
    TEST(events[0].connection_idx == 0);
}

static void test_move_connection_end_structural_failure_deletes()
{
    TwoNodeFixture f;
    const uint32_t node_c = f.graph.create_node("c", vmath::vec2(0.0f, 0.0f));
    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    const uint32_t c_input = f.graph.add_slot(node_c, in_slot);
    TEST(c_input != Sculptor::pool_no_slot);

    TEST(f.graph.attempt_connection(f.output_end, f.input_end));
    GraphChange scratch[32]; drain_changes(f.graph, scratch, 32);

    // Retarget to an out-of-range slot: structural check fails, connection
    // is deleted and the error overlay is set.
    const EndPoint bad_slot = { node_c, max_node_slots + 3 };
    TEST(!f.graph.move_connection_end(0, false, bad_slot));
    TEST(f.graph.connection_count() == 0);
    TEST(f.graph.has_error());
}

static void test_selection_basics()
{
    Graph g;
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t b = g.create_node("b", vmath::vec2(32.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    TEST(b != Sculptor::pool_no_slot);

    TEST(!g.is_selected(a));
    TEST(!g.is_selected(b));

    g.set_selected(a, true);
    TEST(g.is_selected(a));
    TEST(!g.is_selected(b));

    g.set_selected(b, true);
    TEST(g.is_selected(a));
    TEST(g.is_selected(b));

    g.set_selected(a, false);
    TEST(!g.is_selected(a));
    TEST(g.is_selected(b));

    g.select_none();
    TEST(!g.is_selected(a));
    TEST(!g.is_selected(b));

    // Selection changes are pure view state: no change events.
    GraphChange scratch[8] = {};
    drain_changes(g, scratch, 8);  // drain the two node_added events
    g.set_selected(b, true);
    GraphChange events[8] = {};
    TEST(drain_changes(g, events, 8) == 0);
}

static void test_delete_node_clears_selection()
{
    Graph g;
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);

    g.report_content_size(a, vmath::vec2(40.0f, 30.0f));
    g.set_selected(a, true);
    g.delete_node(a);

    // The pool reuses the index; the reused node must start unselected with
    // a clean content size (it reads 0 until its first drawn frame).
    const uint32_t reused = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    TEST(reused == a);
    TEST(!g.is_selected(reused));
    TEST(g.content_size(reused).x == 0.0f);
    TEST(g.content_size(reused).y == 0.0f);
}

static void test_selection_api_ignores_unoccupied_slots()
{
    Graph g;

    // Out-of-range and unoccupied slots are no-ops, never seed state.
    g.set_selected(Sculptor::max_nodes, true);
    g.set_selected(0, true);
    g.report_content_size(Sculptor::max_nodes, vmath::vec2(50.0f, 50.0f));
    g.report_content_size(0, vmath::vec2(50.0f, 50.0f));
    TEST(!g.is_selected(Sculptor::max_nodes));
    TEST(!g.is_selected(0));
    TEST(g.content_size(Sculptor::max_nodes).x == 0.0f);
    TEST(g.content_size(0).x == 0.0f);

    // A node created later at the touched slot starts clean.
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(a == 0);
    TEST(!g.is_selected(a));
    TEST(g.content_size(a).x == 0.0f);
}

static void test_align_left_and_top()
{
    Graph g;
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
    drain_changes(g, scratch, 8);  // drain the node_added events

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
    Graph g;
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
    TEST(g.node(b).position.x == 76.0f);  // 136 - 60
    TEST(g.node(c).position.x == 86.0f);  // 136 - 50

    // Bottommost selected edge is b (80 + 20 = 100).
    g.align_selected(Sculptor::AlignKind::bottom);
    TEST(g.node(a).position.y == 70.0f);  // 100 - 30
    TEST(g.node(b).position.y == 80.0f);
    TEST(g.node(c).position.y == 60.0f);  // 100 - 40
}

static void test_align_equal_width_and_height()
{
    Graph g;
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
    Graph g;
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
    Graph g;
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
static bool colors_state_equal(const GraphColors& a, const GraphColors& b)
{
    return a.node_background == b.node_background && a.node_border == b.node_border &&
           a.node_selected_border == b.node_selected_border && a.node_title == b.node_title &&
           a.grid_line == b.grid_line && a.grid_axis == b.grid_axis && a.connector == b.connector &&
           a.connector_hover == b.connector_hover && a.connector_connected == b.connector_connected &&
           a.connection == b.connection && a.ghost_node == b.ghost_node &&
           a.property_value == b.property_value &&
           a.property_connected_value == b.property_connected_value &&
           a.error_background == b.error_background && a.error_text == b.error_text &&
           a.selection_outline == b.selection_outline && a.selection_band == b.selection_band;
}

static bool slot_state_equal(const Slot& a, const Slot& b)
{
    if (strncmp(a.name, b.name, sizeof(a.name)) != 0) {
        return false;
    }
    if (a.kind != b.kind || a.connectable != b.connectable || a.property_type != b.property_type ||
        a.num_list_options != b.num_list_options) {
        return false;
    }
    if (memcmp(&a.value, &b.value, sizeof(a.value)) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < a.num_list_options; ++i) {
        if (strncmp(a.list_options[i], b.list_options[i], sizeof(a.list_options[i])) != 0) {
            return false;
        }
    }
    return true;
}

static bool node_state_equal(const Node& a, const Node& b)
{
    if (strncmp(a.name, b.name, sizeof(a.name)) != 0) {
        return false;
    }
    if (a.position.x != b.position.x || a.position.y != b.position.y) {
        return false;
    }
    if (a.color_override != b.color_override || a.content_width_override != b.content_width_override ||
        a.content_height_override != b.content_height_override || a.ghost != b.ghost) {
        return false;
    }
    if (a.slots.num_allocated != b.slots.num_allocated) {
        return false;
    }
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        if (a.slots.is_occupied(i) != b.slots.is_occupied(i)) {
            return false;
        }
        if (a.slots.is_occupied(i) && !slot_state_equal(a.slots.entries[i], b.slots.entries[i])) {
            return false;
        }
    }
    return true;
}

static bool connection_state_equal(const Connection& a, const Connection& b)
{
    return a.output.node_idx == b.output.node_idx && a.output.slot_idx == b.output.slot_idx &&
           a.input.node_idx == b.input.node_idx && a.input.slot_idx == b.input.slot_idx;
}

static bool test_serialize_called    = false;
static bool test_deserialize_called = false;

static uint32_t test_serialize_state_overflow(void* /*user_data*/, uint8_t* /*buffer*/, uint32_t /*remaining*/)
{
    // Lies about the tail size without writing anything.
    return 0xFFFFFFFFu;
}

static uint32_t test_serialize_state(void* /*user_data*/, uint8_t* buffer, uint32_t /*buffer_size*/)
{
    test_serialize_called = true;
    buffer[0]             = 0xde;
    buffer[1]             = 0xad;
    buffer[2]             = 0xbe;
    buffer[3]             = 0xef;
    return 4;
}

static bool test_deserialize_state(void* /*user_data*/, const uint8_t* buffer, uint32_t buffer_size)
{
    test_deserialize_called = buffer_size == 4 && buffer[0] == 0xde && buffer[1] == 0xad &&
                              buffer[2] == 0xbe && buffer[3] == 0xef;
    return test_deserialize_called;
}

// Builds one node named "n" with a single integer property slot set to value.
static void build_int_property_graph(Graph& g, int32_t value, uint32_t* node_idx, uint32_t* slot_idx)
{
    const uint32_t n = g.create_node("n", vmath::vec2(0.0f, 0.0f));
    TEST(n != Sculptor::pool_no_slot);
    Slot p          = {};
    p.kind          = SlotKind::property;
    p.property_type = PropertyType::integer;
    p.value.integer = value;
    const uint32_t s = g.add_slot(n, p);
    TEST(s != Sculptor::pool_no_slot);
    *node_idx = n;
    *slot_idx = s;
}

// Test 1: full round-trip through save/load preserves all graph state.
static void test_save_load_round_trip()
{
    Graph g;

    GraphColors custom = {};
    custom.node_background          = 0x11223344;
    custom.node_border              = 0x22334455;
    custom.node_selected_border     = 0x33445566;
    custom.node_title               = 0x44556677;
    custom.grid_line                = 0x55667788;
    custom.grid_axis                = 0x66778899;
    custom.connector                = 0x778899aa;
    custom.connector_hover          = 0x8899aabb;
    custom.connector_connected      = 0x99aabbcc;
    custom.connection               = 0xaabbccdd;
    custom.ghost_node               = 0xbbccddee;
    custom.property_value           = 0xccddeeff;
    custom.property_connected_value = 0xddeeff00;
    custom.error_background         = 0xeeff0011;
    custom.error_text               = 0xff001122;
    custom.selection_outline        = 0x00112233;
    custom.selection_band           = 0x11223344;
    g.set_colors(custom);

    const uint32_t a = g.create_node("source", vmath::vec2(32.0f, 48.0f));
    TEST(a != Sculptor::pool_no_slot);
    const uint32_t b = g.create_node("dest", vmath::vec2(160.0f, 96.0f));
    TEST(b != Sculptor::pool_no_slot);
    const uint32_t c = g.create_node("ghost", vmath::vec2(64.0f, 64.0f));
    TEST(c != Sculptor::pool_no_slot);

    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    snprintf(out_slot.name, sizeof(out_slot.name), "out");
    const uint32_t a_out = g.add_slot(a, out_slot);
    TEST(a_out != Sculptor::pool_no_slot);

    Slot mode_slot = {};
    mode_slot.kind             = SlotKind::property;
    mode_slot.property_type    = PropertyType::list;
    mode_slot.num_list_options = 2;
    mode_slot.value.list_index = 1;
    snprintf(mode_slot.name, sizeof(mode_slot.name), "mode");
    snprintf(mode_slot.list_options[0], sizeof(mode_slot.list_options[0]), "sine");
    snprintf(mode_slot.list_options[1], sizeof(mode_slot.list_options[1]), "saw");
    const uint32_t a_mode = g.add_slot(a, mode_slot);
    TEST(a_mode != Sculptor::pool_no_slot);

    Slot count_slot = {};
    count_slot.kind          = SlotKind::property;
    count_slot.property_type = PropertyType::integer;
    count_slot.value.integer = 7;
    snprintf(count_slot.name, sizeof(count_slot.name), "count");
    const uint32_t a_count = g.add_slot(a, count_slot);
    TEST(a_count != Sculptor::pool_no_slot);

    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    snprintf(in_slot.name, sizeof(in_slot.name), "in");
    const uint32_t b_in = g.add_slot(b, in_slot);
    TEST(b_in != Sculptor::pool_no_slot);

    // Connectable property on the input side: connections may target it.
    Slot gain_slot = {};
    gain_slot.kind          = SlotKind::property;
    gain_slot.connectable   = true;
    gain_slot.property_type = PropertyType::real;
    gain_slot.value.real    = 1.5f;
    snprintf(gain_slot.name, sizeof(gain_slot.name), "gain");
    const uint32_t b_gain = g.add_slot(b, gain_slot);
    TEST(b_gain != Sculptor::pool_no_slot);

    // One output fans out to an input and to a connectable property.
    TEST(g.add_connection(EndPoint{a, a_out}, EndPoint{b, b_in}) != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{a, a_out}, EndPoint{b, b_gain}) != Sculptor::pool_no_slot);

    g.set_node_color(a, 0xff00ff00);
    g.set_ghost(c, true);

    // Content size overrides via the public align API.
    g.report_content_size(a, vmath::vec2(100.0f, 50.0f));
    g.report_content_size(b, vmath::vec2(80.0f, 40.0f));
    g.set_selected(a, true);
    g.set_selected(b, true);
    g.align_selected(Sculptor::AlignKind::equal_width);
    g.align_selected(Sculptor::AlignKind::equal_height);

    uint8_t buffer[4096] = {};
    const uint32_t saved = g.save(buffer, sizeof(buffer));
    TEST(saved > 0);
    TEST(saved <= sizeof(buffer));
    if (saved == 0) {
        return;  // save is not implemented yet; the checks below need a snapshot
    }

    Graph g2;
    uint32_t consumed = 0;
    const bool loaded = g2.load(buffer, saved, &consumed);
    TEST(loaded);
    TEST(consumed == saved);
    if (!loaded) {
        return;  // load is not implemented yet
    }

    TEST(colors_state_equal(g.colors(), g2.colors()));
    TEST(node_state_equal(g.node(a), g2.node(a)));
    TEST(node_state_equal(g.node(b), g2.node(b)));
    TEST(node_state_equal(g.node(c), g2.node(c)));
    TEST(g2.node(c).ghost);
    TEST(g2.connection_count() == g.connection_count());
    TEST(connection_state_equal(g.get_connection(0), g2.get_connection(0)));
    TEST(connection_state_equal(g.get_connection(1), g2.get_connection(1)));
}

// Test 2: caller-state serialize/deserialize hooks wrap the graph snapshot.
static void test_save_load_caller_state_hook()
{
    Graph with_hooks;
    with_hooks.set_state_callbacks(test_serialize_state, test_deserialize_state, nullptr);
    Graph plain;

    const uint32_t n1 = with_hooks.create_node("n", vmath::vec2(0.0f, 0.0f));
    TEST(n1 != Sculptor::pool_no_slot);
    Slot slot = {};
    slot.kind = SlotKind::output;
    TEST(with_hooks.add_slot(n1, slot) != Sculptor::pool_no_slot);

    const uint32_t n2 = plain.create_node("n", vmath::vec2(0.0f, 0.0f));
    TEST(n2 != Sculptor::pool_no_slot);
    TEST(plain.add_slot(n2, slot) != Sculptor::pool_no_slot);

    uint8_t buf_with[2048]     = {};
    uint8_t buf_plain[2048]    = {};
    const uint32_t total_with  = with_hooks.save(buf_with, sizeof(buf_with));
    const uint32_t total_plain = plain.save(buf_plain, sizeof(buf_plain));
    TEST(test_serialize_called);
    TEST(total_with > 0);
    TEST(total_plain > 0);
    TEST(total_with == total_plain + 4);

    if (total_with == 0) {
        return;  // save is not implemented yet; the checks below need a snapshot
    }

    Graph restore;
    restore.set_state_callbacks(nullptr, test_deserialize_state, nullptr);
    uint32_t consumed = 0;
    const bool loaded = restore.load(buf_with, total_with, &consumed);
    TEST(loaded);
    TEST(test_deserialize_called);
    TEST(consumed == total_with);
    if (!loaded) {
        return;
    }

    // Deserialize hook is optional: the caller tail is skipped, load still
    // succeeds and still reports the tail as consumed.
    Graph no_hooks;
    consumed = 0;
    TEST(no_hooks.load(buf_with, total_with, &consumed));
    TEST(consumed == total_with);

    // Hook tail that does not fit the buffer: save must refuse (return 0)
    // rather than truncate the caller state.
    TEST(with_hooks.save(buf_with, total_with - 1) == 0);

    // Over-reporting hook (huge size, near UINT32_MAX): save must refuse
    // rather than wrap the remaining-space arithmetic.
    Graph over;
    over.set_state_callbacks(test_serialize_state_overflow, test_deserialize_state, nullptr);
    TEST(over.create_node("n", vmath::vec2(0.0f, 0.0f)) != Sculptor::pool_no_slot);
    TEST(over.save(buf_with, sizeof(buf_with)) == 0);
}

// Test 3: diff application emits exactly the changed value, nothing else.
static void test_load_diff_value_changed_only()
{
    Graph    g;
    uint32_t n = Sculptor::pool_no_slot;
    uint32_t s = Sculptor::pool_no_slot;
    build_int_property_graph(g, 5, &n, &s);
    GraphChange changes[8];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    Slot p              = {};
    p.kind              = SlotKind::property;
    p.property_type     = PropertyType::integer;
    p.value.integer     = 5;

    // Snapshot of the same graph with only the property value changed.
    Graph    g2;
    uint32_t n2 = Sculptor::pool_no_slot;
    uint32_t s2 = Sculptor::pool_no_slot;
    build_int_property_graph(g2, 6, &n2, &s2);
    uint8_t buffer[1024] = {};
    const uint32_t saved = g2.save(buffer, sizeof(buffer));
    TEST(saved > 0);

    uint32_t consumed = 0;
    TEST(g.load(buffer, saved, &consumed));
    const uint32_t count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 1);
    if (count == 1) {
        TEST(changes[0].kind == ChangeKind::value_changed);
        TEST(changes[0].node_idx == n);
        TEST(changes[0].slot_idx == s);
    }
    TEST(g.node(n).slots.entries[s].value.integer == 6);

    // Position-only difference: identical names, slots and values, different
    // node positions.  Positions are silent, so no events may be emitted.
    Graph    g7;
    uint32_t n7 = Sculptor::pool_no_slot;
    uint32_t s7 = Sculptor::pool_no_slot;
    build_int_property_graph(g7, 5, &n7, &s7);
    g7.delete_node(n7);
    g7.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    n7 = g7.create_node("n", vmath::vec2(48.0f, 48.0f));
    TEST(n7 != Sculptor::pool_no_slot);
    s7 = g7.add_slot(n7, p);
    TEST(s7 != Sculptor::pool_no_slot);
    g7.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain rebuild events

    Graph    g8;
    uint32_t n8 = Sculptor::pool_no_slot;
    uint32_t s8 = Sculptor::pool_no_slot;
    build_int_property_graph(g8, 5, &n8, &s8);  // identical, at the origin
    uint8_t pos_buffer[1024] = {};
    const uint32_t pos_saved = g8.save(pos_buffer, sizeof(pos_buffer));
    TEST(pos_saved > 0);
    if (pos_saved == 0) {
        return;
    }

    consumed = 0;
    const bool pos_loaded = g7.load(pos_buffer, pos_saved, &consumed);
    TEST(pos_loaded);
    if (pos_loaded) {
        TEST(g7.take_changes(changes, sizeof(changes) / sizeof(changes[0])) == 0);
        TEST(g7.node(n7).slots.entries[s7].value.integer == 5);
    }
}

// Test 4: deletions (connections first, then nodes) precede additions.
// Test 3b: value_changed, color_changed and slot-level name_changed all fire
// for a matched node, in that order (node name diffs cannot occur: matching
// is by name).
static void test_load_diff_value_name_color()
{
    Graph g;
    const uint32_t x = g.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind          = SlotKind::output;
    out.value.integer = 5;
    const uint32_t x_out = g.add_slot(x, out);
    TEST(x_out != Sculptor::pool_no_slot);
    Slot prop             = {};
    prop.kind             = SlotKind::property;
    prop.property_type    = PropertyType::integer;
    prop.value.integer    = 7;
    snprintf(prop.name, sizeof(prop.name), "p1");
    const uint32_t x_prop = g.add_slot(x, prop);
    TEST(x_prop != Sculptor::pool_no_slot);
    GraphChange changes[8];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    Graph g2;
    const uint32_t x2 = g2.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x2 != Sculptor::pool_no_slot);
    Slot out6    = out;
    out6.value.integer = 6;
    TEST(g2.add_slot(x2, out6) != Sculptor::pool_no_slot);
    Slot prop2   = prop;
    snprintf(prop2.name, sizeof(prop2.name), "p2");
    TEST(g2.add_slot(x2, prop2) != Sculptor::pool_no_slot);
    g2.set_node_color(x2, 0xff0000ff);

    uint8_t buffer[1024]    = {};
    const uint32_t saved    = g2.save(buffer, sizeof(buffer));
    TEST(saved > 0);
    uint32_t consumed       = 0;
    TEST(g.load(buffer, saved, &consumed));

    const uint32_t count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 3);
    if (count == 3) {
        TEST(changes[0].kind == ChangeKind::color_changed);
        TEST(changes[0].node_idx == x);
        TEST(changes[1].kind == ChangeKind::value_changed);
        TEST(changes[1].node_idx == x && changes[1].slot_idx == x_out);
        TEST(changes[2].kind == ChangeKind::name_changed);
        TEST(changes[2].node_idx == x && changes[2].slot_idx == x_prop);
    }
    TEST(g.node(x).color_override == 0xff0000ff);
    TEST(g.node(x).slots.entries[x_out].value.integer == 6);
    TEST(strncmp(g.node(x).slots.entries[x_prop].name, "p2", sizeof(prop.name)) == 0);
}

// Test 3c: per-slot structural recreate lands on the same pool index, a
// slot index-set change recreates the node's slots, and a surviving
// connection retargets onto a recreated slot (connection_changed).
static void test_load_diff_structural_recreate()
{
    Graph g;
    const uint32_t x = g.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x != Sculptor::pool_no_slot);
    const uint32_t y = g.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y != Sculptor::pool_no_slot);
    Slot out1 = {};
    out1.kind          = SlotKind::output;
    out1.value.integer = 1;
    const uint32_t x_out = g.add_slot(x, out1);
    TEST(x_out != Sculptor::pool_no_slot);
    Slot in_plain      = {};
    in_plain.kind      = SlotKind::input;
    const uint32_t y_in = g.add_slot(y, in_plain);
    TEST(y_in != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{x, x_out}, EndPoint{y, y_in}) != Sculptor::pool_no_slot);
    GraphChange changes[8];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    Graph g2;
    const uint32_t x2 = g2.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x2 != Sculptor::pool_no_slot);
    const uint32_t y2 = g2.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y2 != Sculptor::pool_no_slot);
    Slot out_typed    = out1;
    out_typed.property_type = PropertyType::integer;  // structural diff, same index
    TEST(g2.add_slot(x2, out_typed) != Sculptor::pool_no_slot);
    Slot in_a = {};
    in_a.kind = SlotKind::input;
    snprintf(in_a.name, sizeof(in_a.name), "a");
    TEST(g2.add_slot(y2, in_a) != Sculptor::pool_no_slot);
    Slot in_b = in_a;
    snprintf(in_b.name, sizeof(in_b.name), "b");
    const uint32_t y2_in2 = g2.add_slot(y2, in_b);
    TEST(y2_in2 != Sculptor::pool_no_slot);
    TEST(g2.add_connection(EndPoint{x2, x_out}, EndPoint{y2, y2_in2}) != Sculptor::pool_no_slot);

    uint8_t buffer[1024] = {};
    const uint32_t saved = g2.save(buffer, sizeof(buffer));
    TEST(saved > 0);
    uint32_t consumed    = 0;
    TEST(g.load(buffer, saved, &consumed));

    const uint32_t count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 6);
    if (count == 6) {
        TEST(changes[0].kind == ChangeKind::slot_deleted);
        TEST(changes[0].node_idx == x && changes[0].slot_idx == x_out);
        TEST(changes[1].kind == ChangeKind::slot_added);
        TEST(changes[1].node_idx == x && changes[1].slot_idx == x_out);
        TEST(changes[2].kind == ChangeKind::slot_deleted);
        TEST(changes[2].node_idx == y && changes[2].slot_idx == y_in);
        TEST(changes[3].kind == ChangeKind::slot_added);
        TEST(changes[3].node_idx == y);
        TEST(changes[4].kind == ChangeKind::slot_added);
        TEST(changes[4].node_idx == y);
        TEST(changes[5].kind == ChangeKind::connection_changed);
    }
    TEST(g.node(x).slots.entries[x_out].property_type == PropertyType::integer);
    TEST(g.node(y).slots.is_occupied(0) && g.node(y).slots.is_occupied(1));
    TEST(g.connection_count() == 1);
    TEST(g.get_connection(0).output.node_idx == x);
    TEST(g.get_connection(0).output.slot_idx == x_out);
    TEST(g.get_connection(0).input.node_idx == y);
    TEST(g.get_connection(0).input.slot_idx == 1);
}

static void test_save_load_round_trip_fan_in()
{
    Graph g;
    const uint32_t x = g.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x != Sculptor::pool_no_slot);
    const uint32_t y = g.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    const uint32_t x_out = g.add_slot(x, out);
    TEST(x_out != Sculptor::pool_no_slot);
    Slot in_plain = {};
    in_plain.kind = SlotKind::input;
    const uint32_t y_in = g.add_slot(y, in_plain);
    TEST(y_in != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{x, x_out}, EndPoint{y, y_in}) != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{x, x_out}, EndPoint{y, y_in}) != Sculptor::pool_no_slot);
    TEST(g.connection_count() == 2);

    uint8_t buffer[1024] = {};
    const uint32_t saved = g.save(buffer, sizeof(buffer));
    TEST(saved > 0);

    Graph g2;
    uint32_t consumed = 0;
    TEST(g2.load(buffer, saved, &consumed));
    TEST(consumed == saved);
    TEST(g2.connection_count() == 2);
    TEST(g2.get_connection(0).output.node_idx == x && g2.get_connection(0).input.node_idx == y);
    TEST(g2.get_connection(1).output.node_idx == x && g2.get_connection(1).input.node_idx == y);
    TEST(g2.get_connection(0).input.slot_idx == y_in);
    TEST(g2.get_connection(1).input.slot_idx == y_in);
}

// Test 3d: matched endpoint slots whose kind changes are recreated in place
// (sparse indices included), new connections land at exact snapshot indices
// even when lower connection slots are free, and connection validation runs
// against the recreated slot structure.
// Test 3e: the raw API allows fan-in, so a graph with two connections to
// one input must round-trip through save/load unchanged.
static void test_load_diff_sparse_recreate()
{
    Graph g;
    const uint32_t x = g.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x != Sculptor::pool_no_slot);
    const uint32_t y = g.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    const uint32_t x_out = g.add_slot(x, out);
    TEST(x_out != Sculptor::pool_no_slot);
    Slot in_plain = {};
    in_plain.kind = SlotKind::input;
    const uint32_t x_in = g.add_slot(x, in_plain);
    TEST(x_in != Sculptor::pool_no_slot);
    // Sparse slot layout for y: occupied indices 0 and 3.
    const uint32_t y_in0 = g.add_slot(y, in_plain);
    TEST(y_in0 != Sculptor::pool_no_slot);
    TEST(g.add_slot(y, in_plain) != Sculptor::pool_no_slot);
    TEST(g.add_slot(y, in_plain) != Sculptor::pool_no_slot);
    const uint32_t y_in3 = g.add_slot(y, in_plain);
    TEST(y_in3 != Sculptor::pool_no_slot);
    g.remove_slot(y, 1);  // sparse live layout {0, 3} to match the snapshot
    g.remove_slot(y, 2);
    TEST(g.add_connection(EndPoint{x, x_out}, EndPoint{y, y_in0}) != Sculptor::pool_no_slot);
    GraphChange changes[16];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain

    Graph g2;
    const uint32_t x2 = g2.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x2 != Sculptor::pool_no_slot);
    const uint32_t y2 = g2.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y2 != Sculptor::pool_no_slot);
    const uint32_t w2 = g2.create_node("w", vmath::vec2(320.0f, 0.0f));
    TEST(w2 != Sculptor::pool_no_slot);
    TEST(g2.add_slot(x2, out) != Sculptor::pool_no_slot);
    TEST(g2.add_slot(x2, in_plain) != Sculptor::pool_no_slot);
    // y: output at 0, filler inputs at 1 and 2, output at 3, fillers freed.
    TEST(g2.add_slot(y2, out) != Sculptor::pool_no_slot);
    TEST(g2.add_slot(y2, in_plain) != Sculptor::pool_no_slot);
    TEST(g2.add_slot(y2, in_plain) != Sculptor::pool_no_slot);
    const uint32_t y2_out3 = g2.add_slot(y2, out);
    TEST(y2_out3 != Sculptor::pool_no_slot);
    g2.remove_slot(y2, 1);
    g2.remove_slot(y2, 2);
    Slot in_w = {};
    in_w.kind = SlotKind::input;
    TEST(g2.add_slot(w2, in_w) != Sculptor::pool_no_slot);
    TEST(g2.add_slot(w2, in_w) != Sculptor::pool_no_slot);
    // Snapshot connections at indices 0 and 2: index 1 deleted, so the pool
    // is sparse and index 1 must stay free after load.
    TEST(g2.add_connection(EndPoint{y2, 0}, EndPoint{w2, 0}) != Sculptor::pool_no_slot);
    TEST(g2.add_connection(EndPoint{y2, y2_out3}, EndPoint{x2, 1}) != Sculptor::pool_no_slot);
    TEST(g2.add_connection(EndPoint{x2, 0}, EndPoint{w2, 1}) != Sculptor::pool_no_slot);
    g2.delete_connection(1);

    uint8_t buffer[1024] = {};
    const uint32_t saved = g2.save(buffer, sizeof(buffer));
    TEST(saved > 0);
    uint32_t consumed = 0;
    TEST(g.load(buffer, saved, &consumed));

    const uint32_t count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 10);
    if (count == 10) {
        TEST(changes[0].kind == ChangeKind::connection_deleted);
        TEST(changes[1].kind == ChangeKind::node_added);
        TEST(changes[2].kind == ChangeKind::slot_added);
        TEST(changes[3].kind == ChangeKind::slot_added);
        TEST(changes[4].kind == ChangeKind::slot_deleted);
        TEST(changes[4].node_idx == y && changes[4].slot_idx == y_in0);
        TEST(changes[5].kind == ChangeKind::slot_added);
        TEST(changes[5].node_idx == y && changes[5].slot_idx == y_in0);
        TEST(changes[6].kind == ChangeKind::slot_deleted);
        TEST(changes[6].node_idx == y && changes[6].slot_idx == y_in3);
        TEST(changes[7].kind == ChangeKind::slot_added);
        TEST(changes[7].node_idx == y && changes[7].slot_idx == y_in3);
        TEST(changes[8].kind == ChangeKind::connection_added);
        TEST(changes[9].kind == ChangeKind::connection_added);
    }

    // End state: y slots recreated in place as outputs (sparse indices
    // kept), connections restored at exact snapshot indices 0 and 2.
    TEST(g.node(y).slots.is_occupied(0) && g.node(y).slots.is_occupied(3));
    TEST(g.node(y).slots.entries[0].kind == SlotKind::output);
    TEST(g.node(y).slots.entries[3].kind == SlotKind::output);
    TEST(g.node(y).slots.entries[0].kind == SlotKind::output);
    TEST(!g.node(y).slots.is_occupied(1) && !g.node(y).slots.is_occupied(2));
    TEST(g.connection_count() == 2);
    TEST(g.get_connection(0).output.node_idx == y);
    TEST(g.get_connection(0).output.slot_idx == 0);
    TEST(g.get_connection(0).input.node_idx == w2);
    TEST(g.get_connection(0).input.slot_idx == 0);
    TEST(g.get_connection(2).output.node_idx == x);
    TEST(g.get_connection(2).output.slot_idx == x_out);
    TEST(g.get_connection(2).input.node_idx == w2);
    TEST(g.get_connection(2).input.slot_idx == 1);
}

static void test_load_diff_event_order()
{
    // Live state: a -> b connected.  Snapshot: c -> d connected.  Loading must
    // delete first (connection, then nodes) and add afterwards.
    Graph g;
    const uint32_t a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    const uint32_t b = g.create_node("b", vmath::vec2(160.0f, 0.0f));
    TEST(b != Sculptor::pool_no_slot);
    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    const uint32_t a_out = g.add_slot(a, out_slot);
    TEST(a_out != Sculptor::pool_no_slot);
    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    const uint32_t b_in = g.add_slot(b, in_slot);
    TEST(b_in != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{a, a_out}, EndPoint{b, b_in}) != Sculptor::pool_no_slot);
    GraphChange changes[32];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    Graph g2;
    const uint32_t c = g2.create_node("c", vmath::vec2(0.0f, 0.0f));
    TEST(c != Sculptor::pool_no_slot);
    const uint32_t d = g2.create_node("d", vmath::vec2(160.0f, 0.0f));
    TEST(d != Sculptor::pool_no_slot);
    const uint32_t c_out = g2.add_slot(c, out_slot);
    TEST(c_out != Sculptor::pool_no_slot);
    const uint32_t d_in = g2.add_slot(d, in_slot);
    TEST(d_in != Sculptor::pool_no_slot);
    TEST(g2.add_connection(EndPoint{c, c_out}, EndPoint{d, d_in}) != Sculptor::pool_no_slot);

    uint8_t buffer[2048] = {};
    const uint32_t saved = g2.save(buffer, sizeof(buffer));
    TEST(saved > 0);

    uint32_t consumed = 0;
    TEST(g.load(buffer, saved, &consumed));

    const uint32_t count         = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    int first_connection_deleted = -1;
    int first_node_deleted       = -1;
    int first_node_added         = -1;
    int first_connection_added   = -1;
    for (uint32_t i = 0; i < count; ++i) {
        switch (changes[i].kind) {
        case ChangeKind::connection_deleted:
            if (first_connection_deleted < 0) {
                first_connection_deleted = static_cast<int>(i);
            }
            break;
        case ChangeKind::node_deleted:
            if (first_node_deleted < 0) {
                first_node_deleted = static_cast<int>(i);
            }
            break;
        case ChangeKind::node_added:
            if (first_node_added < 0) {
                first_node_added = static_cast<int>(i);
            }
            break;
        case ChangeKind::connection_added:
            if (first_connection_added < 0) {
                first_connection_added = static_cast<int>(i);
            }
            break;
        default:
            break;
        }
    }
    TEST(first_connection_deleted >= 0);
    TEST(first_node_deleted >= 0);
    TEST(first_node_added >= 0);
    TEST(first_connection_added >= 0);
    // Ordering: connection deletions before node deletions, deletions before
    // additions, connection additions after node additions.
    TEST(first_connection_deleted < first_node_deleted);
    TEST(first_node_deleted < first_node_added);
    TEST(first_connection_added > first_node_added);

    // End state: exactly the snapshot's connection between c and d.
    TEST(g.connection_count() == 1);
    const Connection& conn = g.get_connection(0);
    TEST(conn.output.node_idx == c && conn.input.node_idx == d);

    // Second phase: full pinned order including value_changed and
    // connection_changed.  Live: x(out+in) -> y, x -> z.  Snapshot: x keeps
    // its name (survives, out value 5 -> 6), y survives, z is replaced by w;
    // x -> z must be deleted and re-added, x -> x(in) retargets in place.
    Graph g3;
    const uint32_t x = g3.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x != Sculptor::pool_no_slot);
    const uint32_t y = g3.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y != Sculptor::pool_no_slot);
    const uint32_t z = g3.create_node("z", vmath::vec2(320.0f, 0.0f));
    TEST(z != Sculptor::pool_no_slot);
    Slot out5 = {};
    out5.kind          = SlotKind::output;
    out5.value.integer = 5;
    const uint32_t x_out = g3.add_slot(x, out5);
    TEST(x_out != Sculptor::pool_no_slot);
    const uint32_t x_in = g3.add_slot(x, in_slot);
    TEST(x_in != Sculptor::pool_no_slot);
    const uint32_t y_in = g3.add_slot(y, in_slot);
    TEST(y_in != Sculptor::pool_no_slot);
    const uint32_t z_in = g3.add_slot(z, in_slot);
    TEST(z_in != Sculptor::pool_no_slot);
    TEST(g3.add_connection(EndPoint{x, x_out}, EndPoint{y, y_in}) != Sculptor::pool_no_slot);
    TEST(g3.add_connection(EndPoint{x, x_out}, EndPoint{z, z_in}) != Sculptor::pool_no_slot);
    g3.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    Graph g4;
    const uint32_t x4 = g4.create_node("x", vmath::vec2(0.0f, 0.0f));
    TEST(x4 != Sculptor::pool_no_slot);
    const uint32_t y4 = g4.create_node("y", vmath::vec2(160.0f, 0.0f));
    TEST(y4 != Sculptor::pool_no_slot);
    const uint32_t w4 = g4.create_node("w", vmath::vec2(320.0f, 0.0f));
    TEST(w4 != Sculptor::pool_no_slot);
    Slot out6 = {};
    out6.kind          = SlotKind::output;
    out6.value.integer = 6;
    const uint32_t x4_out = g4.add_slot(x4, out6);
    TEST(x4_out != Sculptor::pool_no_slot);
    const uint32_t x4_in = g4.add_slot(x4, in_slot);
    TEST(x4_in != Sculptor::pool_no_slot);
    const uint32_t y4_in = g4.add_slot(y4, in_slot);
    TEST(y4_in != Sculptor::pool_no_slot);
    const uint32_t w4_in = g4.add_slot(w4, in_slot);
    TEST(w4_in != Sculptor::pool_no_slot);
    TEST(g4.add_connection(EndPoint{x4, x4_out}, EndPoint{x4, x4_in}) != Sculptor::pool_no_slot);
    TEST(g4.add_connection(EndPoint{x4, x4_out}, EndPoint{w4, w4_in}) != Sculptor::pool_no_slot);

    const uint32_t saved_order = g4.save(buffer, sizeof(buffer));
    TEST(saved_order > 0);
    consumed = 0;
    TEST(g3.load(buffer, saved_order, &consumed));

    const uint32_t order_count = g3.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    // Exact sequence and pinned order: deletions, additions (node_added
    // carries the new node's slot_added), value changes, connection changes
    // last.
    TEST(order_count == 7);
    if (order_count == 7) {
        TEST(changes[0].kind == ChangeKind::connection_deleted);
        TEST(changes[1].kind == ChangeKind::node_deleted);
        TEST(changes[2].kind == ChangeKind::node_added);
        TEST(changes[3].kind == ChangeKind::slot_added);
        TEST(changes[4].kind == ChangeKind::connection_added);
        TEST(changes[5].kind == ChangeKind::value_changed);
        TEST(changes[6].kind == ChangeKind::connection_changed);
    }

    // End state: x -> x(in) retargeted in place, x -> w re-added, value 6.
    TEST(g3.connection_count() == 2);
    TEST(g3.get_connection(0).output.node_idx == x);
    TEST(g3.get_connection(0).output.slot_idx == x_out);
    TEST(g3.get_connection(0).input.node_idx == x);
    TEST(g3.get_connection(0).input.slot_idx == x_in);
    TEST(g3.get_connection(1).output.node_idx == x);
    TEST(g3.get_connection(1).input.node_idx == w4);
    TEST(g3.node(x).slots.entries[x_out].value.integer == 6);
}

// Test 5: snapshot stack undo/redo restores values and reports value_changed.
static void test_snapshot_stack_undo_redo()
{
    Graph    g;
    uint32_t n = Sculptor::pool_no_slot;
    uint32_t s = Sculptor::pool_no_slot;
    build_int_property_graph(g, 5, &n, &s);
    GraphChange changes[8];
    g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));  // drain build events

    // Snapshot stack: "before" (value 5) and "after" (value 6) entries.
    Graph    g_after;
    uint32_t n2 = Sculptor::pool_no_slot;
    uint32_t s2 = Sculptor::pool_no_slot;
    build_int_property_graph(g_after, 6, &n2, &s2);

    uint8_t before[1024]       = {};
    uint8_t after[1024]        = {};
    const uint32_t before_size = g.save(before, sizeof(before));
    const uint32_t after_size  = g_after.save(after, sizeof(after));
    TEST(before_size > 0);
    TEST(after_size > 0);

    // Redo direction: apply the "after" snapshot.
    uint32_t consumed = 0;
    TEST(g.load(after, after_size, &consumed));
    uint32_t count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 1);
    if (count == 1) {
        TEST(changes[0].kind == ChangeKind::value_changed);
    }
    TEST(g.node(n).slots.entries[s].value.integer == 6);

    // Undo: restore the "before" snapshot.
    consumed = 0;
    TEST(g.load(before, before_size, &consumed));
    count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 1);
    if (count == 1) {
        TEST(changes[0].kind == ChangeKind::value_changed);
    }
    TEST(g.node(n).slots.entries[s].value.integer == 5);

    // Redo again: re-apply the "after" snapshot.
    consumed = 0;
    TEST(g.load(after, after_size, &consumed));
    count = g.take_changes(changes, sizeof(changes) / sizeof(changes[0]));
    TEST(count == 1);
    if (count == 1) {
        TEST(changes[0].kind == ChangeKind::value_changed);
    }
    TEST(g.node(n).slots.entries[s].value.integer == 6);
}

// Test 6: save/load failure paths are clean and leave live state untouched.
static void test_save_load_failure_paths()
{
    Graph g;
    const uint32_t a = g.create_node("alpha", vmath::vec2(0.0f, 0.0f));
    TEST(a != Sculptor::pool_no_slot);
    const uint32_t b = g.create_node("beta", vmath::vec2(160.0f, 0.0f));
    TEST(b != Sculptor::pool_no_slot);
    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    const uint32_t a_out = g.add_slot(a, out_slot);
    TEST(a_out != Sculptor::pool_no_slot);
    Slot in_slot = {};
    in_slot.kind = SlotKind::input;
    const uint32_t b_in = g.add_slot(b, in_slot);
    TEST(b_in != Sculptor::pool_no_slot);
    TEST(g.add_connection(EndPoint{a, a_out}, EndPoint{b, b_in}) != Sculptor::pool_no_slot);

    // Insufficient buffer: save must fail cleanly with 0.
    uint8_t tiny[8] = {};
    TEST(g.save(tiny, sizeof(tiny)) == 0);
    TEST(g.save(tiny, 0) == 0);

    uint8_t full[4096]   = {};
    const uint32_t saved = g.save(full, sizeof(full));
    TEST(saved > 0);
    if (saved == 0) {
        return;  // save is not implemented yet; the checks below need a snapshot
    }

    // Bad version: load fails without touching live state.
    uint8_t bad[64] = {};
    bad[0] = 0x99;  // u16 version 0x0399 (little endian), not 1
    bad[1] = 0x03;
    uint32_t consumed = 123;
    TEST(!g.load(bad, sizeof(bad), &consumed));
    TEST(consumed == 0);

    // Truncated buffer: load fails without touching live state.
    consumed = 123;
    TEST(!g.load(full, saved - 1, &consumed));
    TEST(consumed == 0);

    // Live state untouched by the failed loads.
    TEST(strncmp(g.node(a).name, "alpha", sizeof(g.node(a).name)) == 0);
    TEST(g.node(a).slots.is_occupied(a_out));
    TEST(g.node(b).slots.is_occupied(b_in));
    TEST(g.connection_count() == 1);
    TEST(g.get_connection(0).output.node_idx == a);
    TEST(g.get_connection(0).input.node_idx == b);

    // Corrupt list option count: save must refuse rather than read past
    // list_options[8] (add_slot does not validate it).
    Slot bad_options = {};
    bad_options.kind             = SlotKind::property;
    bad_options.property_type    = PropertyType::list;
    bad_options.num_list_options = 9;
    const uint32_t c = g.create_node("c", vmath::vec2(320.0f, 0.0f));
    TEST(c != Sculptor::pool_no_slot);
    TEST(g.add_slot(c, bad_options) != Sculptor::pool_no_slot);
    TEST(g.save(full, sizeof(full)) == 0);
}

int main()
{
    test_create_node_distinct_indices();
    test_create_node_exhaustion_is_safe();
    test_create_node_snaps_position_to_grid();
    test_add_slot_grows_within_limit();
    test_add_slot_overflow_returns_no_slot();
    test_delete_node_frees_index_and_drops_connections();
    test_add_delete_connection_basics();
    test_add_connection_rejects_bad_endpoints();
    test_ghost_is_flag_on_normal_node();
    test_colors_api();
    test_change_events_queue_and_drain();
    test_change_events_overflow_is_reported();
    test_attempt_connection_success();
    test_attempt_connection_validator_rejects();
    test_attempt_connection_structural_rejection_sets_error();
    test_move_connection_end_success();
    test_move_connection_end_failure_deletes();
    test_move_connection_end_structural_failure_deletes();
    test_selection_basics();
    test_delete_node_clears_selection();
    test_selection_api_ignores_unoccupied_slots();
    test_align_left_and_top();
    test_align_right_and_bottom();
    test_align_equal_width_and_height();
    test_align_equal_height_applies_as_equal_width();
    test_align_skips_ghosts_and_empty_selection();
    test_save_load_round_trip();
    test_save_load_caller_state_hook();
    test_load_diff_value_changed_only();
    test_load_diff_event_order();
    test_load_diff_sparse_recreate();
    test_save_load_round_trip_fan_in();
    test_load_diff_value_name_color();
    test_load_diff_structural_recreate();
    test_snapshot_stack_undo_redo();
    test_save_load_failure_paths();

    return exit_code;
}
