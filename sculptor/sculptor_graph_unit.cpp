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

namespace {

// Unique marker color standing in for the pending-wire stroke in the
// frozen-wire test: connector_hover is the color the in-progress wire
// and the connection mid-dot hover share.
constexpr uint32_t marker_wire_color = IM_COL32(0x0A, 0xCD, 0x0A, 0xFF);

struct MarkerVertex {
    float x;
    float y;
};

// Drop-menu test state: the callback counts invocations; the close request
// turns the next invocation into an item click that closes the popup itself.
uint32_t drop_menu_invocations = 0;
bool     drop_menu_item_chosen = false;
bool     drop_menu_close_item  = false;

} // namespace

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
    memset(static_cast<void*>(&unit_graphs[slot]), 0, sizeof(Graph));
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

// sizeof(Graph) exceeds the 8 MiB main-thread stack under AddressSanitizer,
// so the fixture graph lives in heap storage rooted at these globals.  The
// pristine copy restores the value-initialized state that the former
// inline member provided on every fixture construction.
static Graph& fixture_graph  = *new Graph{};
static Graph& pristine_graph = *new Graph{};

struct TwoNodeFixture {
    Graph&   graph;
    uint32_t node_a;
    uint32_t node_b;
    uint32_t output_slot;
    uint32_t input_slot;
    EndPoint output_end;
    EndPoint input_end;

    TwoNodeFixture() : graph(fixture_graph)
    {
        graph               = pristine_graph;
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

// Connection capacity covers the editor's worst-case oscillator graph (1099
// reachable edges at seven layers with per-target blocks, the bound pinned
// by the sculptor_osc_graph.h static_assert); the pool must
// hold every reachable state, not just the common one.
static void test_connection_capacity_raised_to_1152()
{
    TEST(max_connections == 1152);

    Graph& g = reset_unit_graph(0);
    g.clear();
    const uint32_t source = g.create_node("src", vmath::vec2(0.0f, 0.0f));
    TEST(source != Sculptor::pool_no_slot);
    Slot out_slot = {};
    out_slot.kind = SlotKind::output;
    TEST(g.add_slot(source, out_slot) != Sculptor::pool_no_slot);

    // 1152 single-connection inputs across sink nodes (the full per-node
    // slot pool).
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
    for (uint32_t i = 0; i < 1152; ++i) {
        const EndPoint out = { source, 0 };
        const EndPoint in  = { sinks[i / max_node_slots], i % max_node_slots };
        TEST(g.add_connection(out, in) != Sculptor::pool_no_slot);
    }
    TEST(g.connection_count() == 1152);
}

// Render harness shared by the renderer-side tests: one full window frame
// with the graph rendered, returning the window draw list for vertex-color
// scans.  The renderer context comes from create_renderer_context(),
// defined below and called by each renderer-driven test.
static void create_renderer_context();

static const ImDrawList* begin_rendered_graph_frame(Graph& graph)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1024, 768));
    ImGui::Begin("graph mouse regression",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    graph.render(vmath::vec2(1000, 740), nullptr);
    return ImGui::GetWindowDrawList();
}

static void end_rendered_graph_frame()
{
    ImGui::End();
    ImGui::Render();
}

// Per-slot render color, editor-settable through one callback: unconnected
// sum endpoints draw red from the zone's source row instead of the default
// connector color.  Rendered with one unconnected input dot and one
// connected dot: the callback runs for the unconnected dot (its override
// color is drawn); the connected dot keeps the connected connector color
// and never consults the callback.
static void test_slot_color_callback_api()
{
    create_renderer_context();
    Graph& g = reset_unit_graph(0);
    g.clear();
    GraphColors colors         = Sculptor::default_graph_colors();
    colors.connection          = 0x111D2BFF;
    colors.connector           = 0x222324FF;
    colors.connector_connected = 0x252627FF;
    g.set_colors(colors);
    const uint32_t node_a = g.create_node("A", vmath::vec2(64.0f, 64.0f));
    const uint32_t node_b = g.create_node("B", vmath::vec2(320.0f, 64.0f));
    TEST(node_a != Sculptor::pool_no_slot && node_b != Sculptor::pool_no_slot);
    Slot in      = {};
    in.kind      = SlotKind::input;
    in.row_group = 1;
    strcpy(in.name, "In");
    Slot out      = {};
    out.kind      = SlotKind::output;
    out.row_group = 1;
    strcpy(out.name, "Out");
    TEST(g.add_slot(node_a, in) == 0);
    TEST(g.add_slot(node_a, out) == 1);
    TEST(g.add_slot(node_b, in) == 0);
    // Node A slot 1 wires into node B slot 0: A slot 0 renders as the
    // unconnected dot, A slot 1 and B slot 0 as connected dots.
    TEST(g.add_connection({ node_a, 1 }, { node_b, 0 }) != Sculptor::pool_no_slot);
    GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)g.take_changes(discard, Sculptor::max_pending_changes);

    static uint32_t calls = 0;
    calls                 = 0;
    const auto callback   = [](void*, const Graph&, uint32_t node_idx, uint32_t slot_idx, uint32_t* color_out) {
        (void)node_idx;
        calls++;
        if (slot_idx == 0) {
            *color_out = 0xFF4040FF;
            return true;
        }
        return false;
    };
    g.set_slot_color_callback(callback, nullptr);

    const ImU32       custom             = IM_COL32(0xFF, 0x40, 0x40, 0xFF); // 0xFF4040FF, packed 0xRRGGBBAA
    const ImU32       connected_color    = IM_COL32(0x25, 0x26, 0x27, 0xFF);
    uint32_t          custom_vertices    = 0;
    uint32_t          connected_vertices = 0;
    const ImDrawList* draw               = begin_rendered_graph_frame(g);
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == custom) {
            ++custom_vertices;
        }
        else if (vertex.col == connected_color) {
            ++connected_vertices;
        }
    }
    end_rendered_graph_frame();
    // Only the unconnected dot consults the callback; the override color
    // is drawn and the connected dot keeps its default color.
    TEST(calls == 1);
    TEST(custom_vertices > 0);
    TEST(connected_vertices > 0);

    // Unregistering returns the dot to the default connector color: no
    // custom-color vertices on a second frame.
    g.set_slot_color_callback(nullptr, nullptr);
    custom_vertices = 0;
    draw            = begin_rendered_graph_frame(g);
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == custom) {
            ++custom_vertices;
        }
    }
    end_rendered_graph_frame();
    TEST(calls == 1); // no further consultations without the callback
    TEST(custom_vertices == 0);
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
// Defined with the drop-menu renderer tests below: the drop-menu fields are
// renderer-set private state, so clear()'s reset needs one real menu open.
static void test_clear_resets_drop_menu_state();

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

    test_clear_resets_drop_menu_state();
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

static uint32_t render_frame(Graph&        graph,
                             ImVec2        mouse,
                             bool          down,
                             bool          escape       = false,
                             bool          right        = false,
                             uint32_t*     marker_count = nullptr,
                             MarkerVertex* marker_out   = nullptr,
                             uint32_t      marker_cap   = 0)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, down);
    io.AddMouseButtonEvent(1, right);
    io.AddKeyEvent(ImGuiKey_Escape, escape);
    const ImDrawList* draw              = begin_rendered_graph_frame(graph);
    uint32_t          old_wire_vertices = 0;
    uint32_t          markers           = 0;
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == IM_COL32(17, 29, 43, 255)) {
            ++old_wire_vertices;
        }
        else if (marker_count != nullptr && vertex.col == marker_wire_color) {
            if (marker_out != nullptr && markers < marker_cap) {
                marker_out[markers].x = vertex.pos.x;
                marker_out[markers].y = vertex.pos.y;
            }
            ++markers;
        }
    }
    end_rendered_graph_frame();
    if (marker_count != nullptr) {
        *marker_count = markers;
    }
    return old_wire_vertices;
}

static ImVec2 audio_dot(const Graph& graph, uint32_t node, bool output)
{
    // Dot rows use the actual built-in font metrics and override-aware measured width.
    const float line_height  = ImGui::GetFontSize();
    const float frame_height = line_height + 2 * ImGui::GetStyle().FramePadding.y;
    return ImVec2(8 + graph.node(node).position.x + (output ? graph.content_size(node).x : 0),
                  8 + graph.node(node).position.y + 8 + line_height + 4 + (frame_height + 4) * 0.5f);
}

// Renderer-harness context setup shared by every renderer-driven test
// (1024x768 display, no ini or log output).
static void create_renderer_context()
{
    ImGui::CreateContext();

    ImGuiIO& io                     = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.LogFilename                  = nullptr;
    io.DisplaySize                  = ImVec2(1024, 768);
    io.DeltaTime                    = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // the test does not use a real renderer backend

    ImGui::NewFrame();
    ImGui::EndFrame();

    ImTextureData* tex = io.Fonts->TexData;
    TEST(tex && tex->Pixels && tex->Width > 0 && tex->Height > 0);
}

static void test_renderer_audio_endpoint_mouse_frames()
{
    create_renderer_context();
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
            ImGui::GetIO().AddMouseWheelEvent(0, scale < 1 ? -100.0f : 100.0f);
            render_frame(graph, ImVec2(8, 8), false);
            render_frame(graph, ImVec2(8, 8), false);
            const uint32_t node  = endpoint ? source : target;
            const float    font  = ImGui::GetFontSize();
            const float    frame = font + 2 * ImGui::GetStyle().FramePadding.y;
            const ImVec2   dot(8 + graph.node(node).position.x * scale + (endpoint ? graph.content_size(node).x : 0),
                               8 + graph.node(node).position.y * scale + (8 + font + 4 + (frame + 4) * 0.5f) * scale);
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
            const float y = 8 + 16 * scale + (8 + font + 4 + (frame + 4) * 0.5f) * scale;
            render_frame(graph, ImVec2(x, y), true);
            TEST(graph.interaction_active());
            render_frame(graph, ImVec2(x, y), false, true);
            TEST(! graph.interaction_active());
        }
    }
    ImGui::DestroyContext();
}

struct ElbowSegment {
    ImVec2 p1;
    ImVec2 p2;
};

static bool hide_slots_from(void* user_data, const Graph& graph, uint32_t node_idx, uint32_t slot_idx)
{
    (void)graph;
    (void)node_idx;
    return slot_idx >= *static_cast<const uint32_t*>(user_data);
}

static void build_three_slot_mod_node(Graph& graph, bool mark_children)
{
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t node = graph.create_node("Osc", vmath::vec2(32.0f, 32.0f));
    TEST(node != Sculptor::pool_no_slot);

    Slot volume        = Sculptor::real_slot("Volume", 0.5f);
    volume.connectable = true;
    TEST(graph.add_slot(node, volume) == 0);

    Slot env      = Sculptor::input_slot("Envelope");
    env.child_row = mark_children;
    TEST(graph.add_slot(node, env) == 1);

    Slot lfo      = Sculptor::input_slot("LFO");
    lfo.child_row = mark_children;
    TEST(graph.add_slot(node, lfo) == 2);
}

static uint32_t render_and_collect_connector_lines(Graph&        graph,
                                                   ElbowSegment* segments,
                                                   uint32_t      segment_cap,
                                                   uint32_t*     out_segment_count)
{
    const ImDrawList* draw   = begin_rendered_graph_frame(graph);
    const uint32_t    packed = Sculptor::default_graph_colors().connector;
    const ImU32       connector_col =
        IM_COL32((packed >> 24) & 0xFFu, (packed >> 16) & 0xFFu, (packed >> 8) & 0xFFu, packed & 0xFFu);
    uint32_t connector_vtx = 0;
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == connector_col) {
            ++connector_vtx;
        }
    }
    uint32_t       seg_count = 0;
    const uint32_t vtx_count = static_cast<uint32_t>(draw->VtxBuffer.Size);
    for (uint32_t i = 0; i + 3 < vtx_count;) {
        const ImDrawVert& v0 = draw->VtxBuffer[static_cast<int>(i)];
        const ImDrawVert& v1 = draw->VtxBuffer[static_cast<int>(i + 1)];
        const ImDrawVert& v2 = draw->VtxBuffer[static_cast<int>(i + 2)];
        const ImDrawVert& v3 = draw->VtxBuffer[static_cast<int>(i + 3)];
        if (v0.col == connector_col && v1.col == connector_col && v2.col == connector_col && v3.col == connector_col) {
            const bool vertical   = v0.pos.y == v1.pos.y && v2.pos.y == v3.pos.y && v0.pos.x == v2.pos.x &&
                                    v1.pos.x == v3.pos.x && v0.pos.y != v2.pos.y;
            const bool horizontal = v0.pos.x == v1.pos.x && v2.pos.x == v3.pos.x && v0.pos.y == v2.pos.y &&
                                    v1.pos.y == v3.pos.y && v0.pos.x != v2.pos.x;
            if (vertical || horizontal) {
                if (segments != nullptr && seg_count < segment_cap) {
                    segments[seg_count].p1 = ImVec2(0.5f * (v0.pos.x + v1.pos.x), 0.5f * (v0.pos.y + v1.pos.y));
                    segments[seg_count].p2 = ImVec2(0.5f * (v2.pos.x + v3.pos.x), 0.5f * (v2.pos.y + v3.pos.y));
                }
                ++seg_count;
                i += 4;
                continue;
            }
        }
        ++i;
    }
    end_rendered_graph_frame();
    if (out_segment_count != nullptr) {
        *out_segment_count = seg_count;
    }
    return connector_vtx;
}

// Node height includes the 4 px title-to-first-row spacing, child rows
// indent by tree_indent (14 px), hidden child rows draw no elbow, and each
// visible child row draws a two-segment sharp-cornered elbow continuing the
// vertical spine from the parent down through sibling child rows.
static void test_renderer_title_spacing_and_child_row_elbows()
{
    create_renderer_context();

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t single_node = graph.create_node("N", vmath::vec2(32.0f, 32.0f));
    TEST(single_node != Sculptor::pool_no_slot);
    Slot single_in = Sculptor::input_slot("In");
    TEST(graph.add_slot(single_node, single_in) == 0);
    render_frame(graph, ImVec2(900.0f, 600.0f), false);
    const float title_h = ImGui::GetTextLineHeightWithSpacing();
    const float frame_h = ImGui::GetFrameHeight();
    TEST(graph.content_size(single_node).y == 8.0f + title_h + 4.0f + (frame_h + 4.0f) + 8.0f);

    graph.clear();
    const uint32_t top_node   = graph.create_node("N", vmath::vec2(32.0f, 32.0f));
    const uint32_t child_node = graph.create_node("N", vmath::vec2(256.0f, 32.0f));
    TEST(top_node != Sculptor::pool_no_slot);
    TEST(child_node != Sculptor::pool_no_slot);
    Slot top_slot        = Sculptor::input_slot("Envelope");
    Slot child_slot      = top_slot;
    child_slot.child_row = true;
    TEST(graph.add_slot(top_node, top_slot) == 0);
    TEST(graph.add_slot(child_node, child_slot) == 0);
    render_frame(graph, ImVec2(900.0f, 600.0f), false);
    TEST(graph.content_size(child_node).x == graph.content_size(top_node).x + 14.0f);

    uint32_t seg_count = 0;
    build_three_slot_mod_node(graph, false);
    const uint32_t baseline_verts = render_and_collect_connector_lines(graph, nullptr, 0, &seg_count);
    TEST(baseline_verts > 0);
    TEST(seg_count == 0);

    uint32_t min_hidden = 1;
    graph.set_slot_hidden_callback(&hide_slots_from, &min_hidden);
    build_three_slot_mod_node(graph, false);
    const uint32_t hidden_base_verts = render_and_collect_connector_lines(graph, nullptr, 0, &seg_count);
    TEST(seg_count == 0);
    build_three_slot_mod_node(graph, true);
    const uint32_t hidden_child_verts = render_and_collect_connector_lines(graph, nullptr, 0, &seg_count);
    TEST(hidden_child_verts == hidden_base_verts);
    TEST(seg_count == 0);

    min_hidden = 2;
    build_three_slot_mod_node(graph, false);
    const uint32_t one_vis_base_verts = render_and_collect_connector_lines(graph, nullptr, 0, &seg_count);
    TEST(seg_count == 0);
    ElbowSegment segs[4] = {};
    build_three_slot_mod_node(graph, true);
    const uint32_t one_vis_child_verts = render_and_collect_connector_lines(graph, segs, 4, &seg_count);
    TEST(one_vis_child_verts == one_vis_base_verts + 8);
    TEST(seg_count == 2);

    graph.set_slot_hidden_callback(nullptr, nullptr);
    build_three_slot_mod_node(graph, true);
    const uint32_t two_vis_child_verts = render_and_collect_connector_lines(graph, segs, 4, &seg_count);
    TEST(two_vis_child_verts == baseline_verts + 16);
    TEST(seg_count == 4);
    if (seg_count == 4) {
        // First child elbow: vertical spine down to corner, horizontal arm right.
        TEST(segs[0].p1.x == segs[0].p2.x);
        TEST(segs[0].p1.y < segs[0].p2.y);
        TEST(segs[1].p1.x == segs[0].p2.x);
        TEST(segs[1].p1.y == segs[0].p2.y);
        TEST(segs[1].p2.y == segs[1].p1.y);
        TEST(segs[1].p2.x > segs[1].p1.x);
        // Second child elbow: vertical spine continues from first child's y_center.
        TEST(segs[2].p1.x == segs[0].p1.x);
        TEST(segs[2].p2.x == segs[2].p1.x);
        TEST(segs[2].p1.y == segs[0].p2.y);
        TEST(segs[2].p2.y > segs[2].p1.y);
        TEST(segs[3].p1.x == segs[2].p2.x);
        TEST(segs[3].p1.y == segs[2].p2.y);
        TEST(segs[3].p2.y == segs[3].p1.y);
        TEST(segs[3].p2.x == segs[1].p2.x);
    }

    ImGui::DestroyContext();
}

// A deeper child row keeps the shallower columns' rails running past it:
// a level-2 row draws the level-1 rail beside itself down to the next
// sibling row, so the parent's rail stays continuous.
static void test_renderer_child_row_sibling_rails()
{
    create_renderer_context();

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t node = graph.create_node("N", vmath::vec2(32.0f, 32.0f));
    TEST(node != Sculptor::pool_no_slot);
    Slot volume        = Sculptor::real_slot("Volume", 0.5f);
    volume.connectable = true;
    TEST(graph.add_slot(node, volume) == 0);
    Slot env      = Sculptor::input_slot("Envelope");
    env.child_row = true;
    TEST(graph.add_slot(node, env) == 1);
    Slot rate        = Sculptor::real_slot("Rate x", 1.0f);
    rate.child_row   = true;
    rate.child_level = 2;
    TEST(graph.add_slot(node, rate) == 2);
    Slot src      = Sculptor::input_slot("Source A");
    src.child_row = true;
    TEST(graph.add_slot(node, src) == 3);

    ElbowSegment segs[8]   = {};
    uint32_t     seg_count = 0;
    render_and_collect_connector_lines(graph, segs, 8, &seg_count);
    // Envelope spine + arm, rate spine + arm + level-1 rail, source spine + arm.
    TEST(seg_count == 7);
    if (seg_count == 7) {
        // The rate row's rail sits on the level-1 column and continues the
        // envelope's spine down through the rate row.
        TEST(segs[4].p1.x == segs[4].p2.x);
        TEST(segs[4].p1.x == segs[0].p1.x);
        TEST(segs[4].p1.y == segs[0].p2.y);
        TEST(segs[4].p2.y > segs[4].p1.y);
        // The rate row's own spine sits one indent deeper.
        TEST(segs[2].p1.x == segs[0].p1.x + 14.0f);
        TEST(segs[2].p1.y == segs[0].p2.y);
        // The source row's spine continues the level-1 rail from the rate row.
        TEST(segs[5].p1.x == segs[0].p1.x);
        TEST(segs[5].p1.y == segs[4].p2.y);
        TEST(segs[5].p2.y > segs[5].p1.y);
    }

    // A deeper last child row must not draw a dangling rail: with no later
    // sibling the level-1 rail stops at the level-1 row's own elbow.
    Graph& tail_graph = reset_unit_graph(0);
    tail_graph.clear();
    tail_graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t tail_node = tail_graph.create_node("N", vmath::vec2(32.0f, 32.0f));
    TEST(tail_node != Sculptor::pool_no_slot);
    Slot tail_volume        = Sculptor::real_slot("Volume", 0.5f);
    tail_volume.connectable = true;
    TEST(tail_graph.add_slot(tail_node, tail_volume) == 0);
    Slot tail_env      = Sculptor::input_slot("Envelope");
    tail_env.child_row = true;
    TEST(tail_graph.add_slot(tail_node, tail_env) == 1);
    Slot tail_rate        = Sculptor::real_slot("Rate x", 1.0f);
    tail_rate.child_row   = true;
    tail_rate.child_level = 2;
    TEST(tail_graph.add_slot(tail_node, tail_rate) == 2);

    seg_count = 0;
    render_and_collect_connector_lines(tail_graph, segs, 8, &seg_count);
    // Envelope spine + arm, rate spine + arm; no rail beside the rate row.
    TEST(seg_count == 4);

    // A flat row after the deeper row also ends the rail: the run left the
    // parent's child list, so the rail must not cross it.
    Graph& flat_graph = reset_unit_graph(0);
    flat_graph.clear();
    flat_graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t flat_node = flat_graph.create_node("N", vmath::vec2(32.0f, 32.0f));
    TEST(flat_node != Sculptor::pool_no_slot);
    Slot flat_volume        = Sculptor::real_slot("Volume", 0.5f);
    flat_volume.connectable = true;
    TEST(flat_graph.add_slot(flat_node, flat_volume) == 0);
    Slot flat_env      = Sculptor::input_slot("Envelope");
    flat_env.child_row = true;
    TEST(flat_graph.add_slot(flat_node, flat_env) == 1);
    Slot flat_rate        = Sculptor::real_slot("Rate x", 1.0f);
    flat_rate.child_row   = true;
    flat_rate.child_level = 2;
    TEST(flat_graph.add_slot(flat_node, flat_rate) == 2);
    TEST(flat_graph.add_slot(flat_node, Sculptor::real_slot("Pan", 0.0f)) == 3);

    seg_count = 0;
    render_and_collect_connector_lines(flat_graph, segs, 8, &seg_count);
    TEST(seg_count == 4);

    ImGui::DestroyContext();
}

// Builds the anchor: one node with one connectable parameter on its
// first line, flagged for the drop menu or left unflagged.
static uint32_t add_param_anchor(Graph& graph, vmath::vec2 position, bool flagged)
{
    const uint32_t node = graph.create_node("osc", position);
    TEST(node != Sculptor::pool_no_slot);
    Slot param          = {};
    param.kind          = SlotKind::property;
    param.connectable   = true;
    param.property_type = PropertyType::real;
    param.value.real    = 0.5f;
    param.drop_menu     = flagged;
    snprintf(param.name, sizeof(param.name), "volume");
    TEST(graph.add_slot(node, param) == 0);
    return node;
}

static void test_drop_menu_callback(void* user_data)
{
    (void)user_data;
    ++drop_menu_invocations;
    if (drop_menu_close_item) {
        ImGui::CloseCurrentPopup();
        drop_menu_item_chosen = true;
    }
}

static uint32_t count_occupied_nodes(const Graph& graph)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (graph.node_occupied(i)) {
            ++count;
        }
    }
    return count;
}

// Drives the harness from idle to an open drop menu: press the anchor
// parameter dot, drag to the release point, let go, then one frame so the
// popup opens and the callback runs.  The optional explicit screen-space
// anchor overrides the audio_dot-based press math (which assumes zoom 1
// and zero view origin) for pan/zoom tests that pass the transformed
// position.
static void open_drop_menu(Graph&        graph,
                           uint32_t      anchor_node,
                           uint32_t      anchor_slot,
                           ImVec2        release,
                           const ImVec2* anchor_dot_screen = nullptr)
{
    Sculptor::DropMenuCallback install = &test_drop_menu_callback;
    graph.set_drop_menu_callback(install, nullptr);

    // One settle frame first: audio_dot measures live font metrics, which
    // a fresh context only has after its first NewFrame.
    render_frame(graph, ImVec2(900.0f, 600.0f), false);
    const ImVec2 dot = anchor_dot_screen ? *anchor_dot_screen : audio_dot(graph, anchor_node, false);
    render_frame(graph, dot, false);
    render_frame(graph, dot, true);
    TEST(graph.interaction_active());
    render_frame(graph, release, true);
    render_frame(graph, release, false);
    TEST(graph.interaction_active());
    TEST(graph.drop_menu_anchor().node_idx == anchor_node);
    TEST(graph.drop_menu_anchor().slot_idx == anchor_slot);
    render_frame(graph, release, false); // popup opens, callback runs
    TEST(graph.interaction_active());
    TEST(drop_menu_invocations > 0);
}

static bool marker_vertices_equal(const MarkerVertex* a, const MarkerVertex* b, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y) {
            return false;
        }
    }
    return true;
}

// True when any captured marker vertex sits within `tolerance` of `point`:
// the analytic position of the wire's anchor or hollow endpoint.
static bool marker_has_vertex_near(const MarkerVertex* vertices, uint32_t count, ImVec2 point, float tolerance)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float dx = vertices[i].x - point.x;
        const float dy = vertices[i].y - point.y;
        if (dx * dx + dy * dy <= tolerance * tolerance) {
            return true;
        }
    }
    return false;
}

// The anchor dot's true on-screen center, measured from the rendered
// unconnected-connector ring (the graph under test has exactly one dot, so
// the one gray ring is unambiguous).  The audio_dot formula is only exact
// enough in identity view: the dot pick radius is a constant 9 px on
// screen, so the formula's few-px row-offset error scales past it at
// extreme zoom and a formula-based press would grab the node body instead.
// The press itself is not the assertion; the analytic view checks below
// are.
static ImVec2 rendered_dot_center(Graph& graph)
{
    const ImDrawList* draw  = begin_rendered_graph_frame(graph);
    float             sum_x = 0.0f;
    float             sum_y = 0.0f;
    uint32_t          ring  = 0;
    for (const ImDrawVert& vertex : draw->VtxBuffer) {
        if (vertex.col == IM_COL32(0x90, 0x90, 0x90, 0xFF)) { // the default connector color
            sum_x += vertex.pos.x;
            sum_y += vertex.pos.y;
            ++ring;
        }
    }
    end_rendered_graph_frame();
    TEST(ring > 0);
    return ImVec2(sum_x / static_cast<float>(ring), sum_y / static_cast<float>(ring));
}

// Pure-math model test of the exported grid snap: floor to the 16 px grid
// with no graph state involved.  Exactly at a half step snaps down, and
// negative inputs snap toward negative infinity, not toward zero.
static void test_snap_to_grid_values()
{
    TEST(Sculptor::snap_to_grid(0.0f) == 0.0f);
    TEST(Sculptor::snap_to_grid(7.9f) == 0.0f); // below the half step
    TEST(Sculptor::snap_to_grid(8.0f) == 0.0f); // at the half step snaps down
    TEST(Sculptor::snap_to_grid(8.1f) == 0.0f); // above the half step, same cell
    TEST(Sculptor::snap_to_grid(15.9f) == 0.0f);
    TEST(Sculptor::snap_to_grid(16.0f) == 16.0f); // on-grid input stays
    TEST(Sculptor::snap_to_grid(23.9f) == 16.0f);
    TEST(Sculptor::snap_to_grid(24.0f) == 16.0f); // half step of the next cell
    TEST(Sculptor::snap_to_grid(24.1f) == 16.0f);
    TEST(Sculptor::snap_to_grid(31.9f) == 16.0f);
    TEST(Sculptor::snap_to_grid(32.0f) == 32.0f);
    TEST(Sculptor::snap_to_grid(-0.1f) == -16.0f);
    TEST(Sculptor::snap_to_grid(-8.0f) == -16.0f); // at the half step snaps down
    TEST(Sculptor::snap_to_grid(-15.9f) == -16.0f);
    TEST(Sculptor::snap_to_grid(-16.0f) == -16.0f);
    TEST(Sculptor::snap_to_grid(-17.0f) == -32.0f);
}

// Every dismissal route (Esc, click elsewhere, item click) returns the
// widget to idle, clears the seam state and creates nothing.
static void test_drop_menu_dismissal_paths_leave_graph_unchanged()
{
    create_renderer_context();

    for (uint32_t path = 0; path < 3; ++path) {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        drop_menu_invocations = 0;
        drop_menu_item_chosen = false;
        drop_menu_close_item  = path == 2;

        Graph& graph = reset_unit_graph(0);
        graph.clear();
        graph.set_colors(Sculptor::default_graph_colors());
        const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
        GraphChange    changes[max_pending_changes];
        graph.take_changes(changes, max_pending_changes);

        const ImVec2 release(700.0f, 600.0f);
        open_drop_menu(graph, anchor, 0, release);
        const uint32_t baseline_nodes = count_occupied_nodes(graph);

        if (path == 0) {
            // Esc closes the popup from inside the widget's popup branch.
            render_frame(graph, release, false, true);
        }
        else if (path == 1) {
            // Click outside the widget: the dismissal click must not start
            // a pan or any other interaction.
            render_frame(graph, ImVec2(1016.0f, 762.0f), true);
            render_frame(graph, ImVec2(1016.0f, 762.0f), false);
        }
        else {
            // Item click: the caller menu entry closed the popup itself.
            TEST(drop_menu_item_chosen);
        }

        // Dismissal resolves on the frame after the closing event.
        render_frame(graph, release, false);

        TEST(! graph.interaction_active());
        TEST(count_occupied_nodes(graph) == baseline_nodes);
        TEST(graph.connection_count() == 0);
        TEST(! graph.has_error());
        TEST(graph.take_changes(changes, max_pending_changes) == 0);
        TEST(graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);
        TEST(graph.drop_menu_pos().x == 0.0f);
        TEST(graph.drop_menu_pos().y == 0.0f);
    }
    ImGui::DestroyContext();
}

// The pending wire stays drawn from the anchor dot to the
// release point while the menu is open, frozen against mouse moves.
static void test_drop_menu_pending_wire_frozen_at_release_point()
{
    create_renderer_context();

    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_item_chosen = false;
    drop_menu_close_item  = false;

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    GraphColors colors     = Sculptor::default_graph_colors();
    colors.connector_hover = 0x0ACD0AFFu; // the marker color
    graph.set_colors(colors);
    const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
    GraphChange    changes[max_pending_changes];
    graph.take_changes(changes, max_pending_changes);

    // The probe touches no dot and no connection mid-dot, so only the
    // pending wire can carry the marker color.
    const ImVec2 probe(300.0f, 650.0f);
    const ImVec2 release(700.0f, 600.0f);

    uint32_t marker_count = 0;
    render_frame(graph, probe, false, false, false, &marker_count);
    const uint32_t idle_baseline = marker_count;

    MarkerVertex before[512];
    MarkerVertex after[512];

    open_drop_menu(graph, anchor, 0, release);
    const vmath::vec2 pos_before = graph.drop_menu_pos();
    TEST(pos_before.x != 0.0f || pos_before.y != 0.0f);

    render_frame(graph, release, false, false, false, &marker_count, before, 512);
    const uint32_t open_count = marker_count;
    TEST(open_count > idle_baseline);

    // Mouse moves elsewhere: the wire endpoint must stay at the release
    // point, not follow the mouse.
    render_frame(graph, probe, false, false, false, &marker_count);
    render_frame(graph, probe, false, false, false, &marker_count, after, 512);
    const uint32_t moved_count = marker_count;
    TEST(moved_count == open_count);
    TEST(marker_vertices_equal(before, after, moved_count));
    TEST(graph.drop_menu_pos().x == pos_before.x);
    TEST(graph.drop_menu_pos().y == pos_before.y);

    // Dismissal removes the wire back to the idle baseline.
    render_frame(graph, ImVec2(1016.0f, 762.0f), true);
    render_frame(graph, ImVec2(1016.0f, 762.0f), false);
    TEST(! graph.interaction_active());
    render_frame(graph, probe, false, false, false, &marker_count);
    TEST(marker_count == idle_baseline);
    ImGui::DestroyContext();
}

// The pending wire freezes in identity view coordinates: with a zoomed or
// panned view the wire endpoint stays at the true release point, mapped
// back through the live view state, and dismissal returns the idle
// baseline.
static void test_drop_menu_pending_wire_identity_view()
{
    create_renderer_context();
    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_item_chosen = false;
    drop_menu_close_item  = false;

    MarkerVertex before[512];
    MarkerVertex after[512];
    GraphChange  changes[max_pending_changes];
    const ImVec2 origin(8.0f, 8.0f); // widget content origin in the harness window

    // Zoom leg: wheel-zoom to both bounds with the mouse parked at the
    // widget origin corner (the retarget zoom test's trick), which keeps
    // the view origin at zero, so every screen position stays analytic
    // (origin + graph position * scale).
    const float zooms[] = { Sculptor::graph_min_zoom, Sculptor::graph_max_zoom };
    for (float scale : zooms) {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        drop_menu_invocations = 0;
        drop_menu_close_item  = false;
        Graph& graph          = reset_unit_graph(0);
        graph.clear();
        GraphColors colors     = Sculptor::default_graph_colors();
        colors.connector_hover = 0x0ACD0AFFu; // the marker color
        graph.set_colors(colors);
        const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
        graph.take_changes(changes, max_pending_changes);

        render_frame(graph, origin, false);
        render_frame(graph, origin, false);
        ImGui::GetIO().AddMouseWheelEvent(0, scale < 1 ? -100.0f : 100.0f);
        render_frame(graph, origin, false);
        render_frame(graph, origin, false);
        // The view state itself is verified implicitly: with the zoom
        // applied and the view origin at zero, drop_menu_pos() below maps
        // the release screen position back to release_graph exactly.

        // The probe touches no dot and no connection mid-dot, so only the
        // pending wire can carry the marker color.
        const ImVec2 probe(300.0f, 650.0f);
        uint32_t     marker_count = 0;
        render_frame(graph, probe, false, false, false, &marker_count);
        const uint32_t idle_baseline = marker_count;

        // Release over empty canvas inside the widget at this zoom: the
        // graph-space release point maps to the analytic screen position.
        const vmath::vec2 release_graph = scale < 1 ? vmath::vec2(700.0f, 600.0f) : vmath::vec2(200.0f, 160.0f);
        const ImVec2      release(origin.x + release_graph.x * scale, origin.y + release_graph.y * scale);
        const ImVec2      anchor_screen = rendered_dot_center(graph);

        open_drop_menu(graph, anchor, 0, release, &anchor_screen);

        // While the menu is open the wire endpoint is the stored release
        // point mapped back through the view: with a zero view origin the
        // mapping is exact, so the hollow endpoint must sit at the release
        // screen position and the wire must leave the transformed anchor.
        render_frame(graph, release, false, false, false, &marker_count, before, 512);
        const uint32_t open_count = marker_count;
        TEST(open_count > idle_baseline);
        TEST(marker_has_vertex_near(before, open_count, release, 2.0f));
        TEST(marker_has_vertex_near(before, open_count, anchor_screen, 2.0f));
        TEST(graph.drop_menu_pos().x == release_graph.x); // power-of-two scales divide exactly
        TEST(graph.drop_menu_pos().y == release_graph.y);

        // Mouse moves elsewhere: the wire stays frozen at the release point.
        render_frame(graph, probe, false, false, false, &marker_count);
        render_frame(graph, probe, false, false, false, &marker_count, after, 512);
        TEST(marker_count == open_count);
        TEST(marker_vertices_equal(before, after, marker_count));

        // Dismissal removes the wire back to this zoom's idle baseline.
        render_frame(graph, ImVec2(1016.0f, 762.0f), true);
        render_frame(graph, ImVec2(1016.0f, 762.0f), false);
        TEST(! graph.interaction_active());
        render_frame(graph, probe, false, false, false, &marker_count);
        TEST(marker_count == idle_baseline);
    }

    // Pan leg: pan the identity view by a known screen delta first, which
    // shifts the view origin by exactly -D, then open the menu from the
    // transformed anchor and assert the frozen endpoint at the analytic
    // release position in the panned view.
    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_close_item  = false;
    Graph& graph          = reset_unit_graph(0);
    graph.clear();
    GraphColors colors     = Sculptor::default_graph_colors();
    colors.connector_hover = 0x0ACD0AFFu; // the marker color
    graph.set_colors(colors);
    const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
    graph.take_changes(changes, max_pending_changes);

    const ImVec2 pan_start(500.0f, 400.0f); // empty canvas far from the anchor node
    const ImVec2 pan_delta(120.0f, 60.0f);
    render_frame(graph, pan_start, false);
    render_frame(graph, pan_start, true); // press on empty canvas: panning starts
    render_frame(graph, ImVec2(pan_start.x + pan_delta.x, pan_start.y + pan_delta.y), true);
    render_frame(graph, ImVec2(pan_start.x + pan_delta.x, pan_start.y + pan_delta.y), false);
    TEST(! graph.interaction_active());
    // The pan itself is verified implicitly: with the view origin shifted
    // by exactly -D, the transformed anchor press below grabs the dot and
    // drop_menu_pos() reports the graph-space position under the release
    // point (release_graph, not release_graph + D).

    const ImVec2 probe(300.0f, 650.0f);
    uint32_t     marker_count = 0;
    render_frame(graph, probe, false, false, false, &marker_count);
    const uint32_t idle_baseline = marker_count;

    const vmath::vec2 release_graph(600.0f, 500.0f);
    // Screen = origin + (graph - view_origin) = origin + graph + D at zoom 1.
    const ImVec2 release(origin.x + release_graph.x + pan_delta.x, origin.y + release_graph.y + pan_delta.y);
    const ImVec2 anchor_screen = rendered_dot_center(graph);

    open_drop_menu(graph, anchor, 0, release, &anchor_screen);

    render_frame(graph, release, false, false, false, &marker_count, before, 512);
    const uint32_t open_count = marker_count;
    TEST(open_count > idle_baseline);
    TEST(marker_has_vertex_near(before, open_count, release, 2.0f));
    TEST(marker_has_vertex_near(before, open_count, anchor_screen, 2.0f));
    TEST(graph.drop_menu_pos().x == release_graph.x); // the mouse sits over release_graph in the panned view
    TEST(graph.drop_menu_pos().y == release_graph.y);

    render_frame(graph, probe, false, false, false, &marker_count);
    render_frame(graph, probe, false, false, false, &marker_count, after, 512);
    TEST(marker_count == open_count);
    TEST(marker_vertices_equal(before, after, marker_count));

    render_frame(graph, ImVec2(1016.0f, 762.0f), true);
    render_frame(graph, ImVec2(1016.0f, 762.0f), false);
    TEST(! graph.interaction_active());
    render_frame(graph, probe, false, false, false, &marker_count);
    TEST(marker_count == idle_baseline);

    ImGui::DestroyContext();
}

// Unflagged dot kinds cancel on an empty-canvas
// release, and a release over a node body opens no menu either.
static void test_drop_menu_non_eligible_anchors_cancel()
{
    create_renderer_context();

    for (uint32_t kind = 0; kind < 3; ++kind) {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        drop_menu_invocations = 0;
        drop_menu_item_chosen = false;
        drop_menu_close_item  = false;

        Graph& graph = reset_unit_graph(0);
        graph.clear();
        graph.set_colors(Sculptor::default_graph_colors());
        const uint32_t node = graph.create_node(kind == 2 ? "param" : "Effect", vmath::vec2(16.0f, 16.0f));
        TEST(node != Sculptor::pool_no_slot);
        Slot slot = {};
        if (kind == 0) {
            slot.kind = SlotKind::output;
            snprintf(slot.name, sizeof(slot.name), "Out");
        }
        else if (kind == 1) {
            slot.kind = SlotKind::input;
            snprintf(slot.name, sizeof(slot.name), "In");
        }
        else {
            slot.kind          = SlotKind::property;
            slot.connectable   = true;
            slot.property_type = PropertyType::real;
            slot.value.real    = 0.5f;
            snprintf(slot.name, sizeof(slot.name), "volume");
        }
        // No drop_menu flag: eligibility is projection state, so unflagged
        // dots cancel whatever their kind.
        TEST(graph.add_slot(node, slot) == 0);
        GraphChange changes[max_pending_changes];
        graph.take_changes(changes, max_pending_changes);
        graph.set_drop_menu_callback(&test_drop_menu_callback, nullptr);

        const ImVec2 release(700.0f, 600.0f);
        // One settle frame first: audio_dot measures live font metrics, which
        // a fresh context only has after its first NewFrame.
        render_frame(graph, ImVec2(900.0f, 600.0f), false);
        const ImVec2 dot = audio_dot(graph, node, kind == 0);
        render_frame(graph, dot, false);
        render_frame(graph, dot, true);
        TEST(graph.interaction_active());
        render_frame(graph, release, true);
        render_frame(graph, release, false);
        render_frame(graph, release, false);

        TEST(! graph.interaction_active());
        TEST(drop_menu_invocations == 0);
        TEST(graph.connection_count() == 0);
        TEST(graph.take_changes(changes, max_pending_changes) == 0);
        TEST(graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);
    }

    // Flagged anchor, but the release lands on a node body: empty canvas
    // means no dot and no node under the drop, so no menu opens there.
    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_item_chosen = false;
    drop_menu_close_item  = false;

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
    const uint32_t body   = graph.create_node("B", vmath::vec2(256.0f, 256.0f));
    TEST(body != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    snprintf(out.name, sizeof(out.name), "Out");
    TEST(graph.add_slot(body, out) == 0);
    Slot in = {};
    in.kind = SlotKind::input;
    snprintf(in.name, sizeof(in.name), "In");
    TEST(graph.add_slot(body, in) == 1);
    GraphChange changes[max_pending_changes];
    graph.take_changes(changes, max_pending_changes);
    graph.set_drop_menu_callback(&test_drop_menu_callback, nullptr);

    const float  font  = ImGui::GetFontSize();
    const float  frame = font + 2.0f * ImGui::GetStyle().FramePadding.y;
    const ImVec2 dot   = audio_dot(graph, anchor, false);
    // The point sits on the body node's second line, clear of its dots.
    const ImVec2 on_body(8.0f + 256.0f + 30.0f, 8.0f + 256.0f + 8.0f + font + 4.0f + (frame + 4.0f) * 1.5f);
    render_frame(graph, ImVec2(900.0f, 600.0f), false);
    render_frame(graph, dot, false);
    render_frame(graph, dot, true);
    TEST(graph.interaction_active());
    render_frame(graph, on_body, true);
    render_frame(graph, on_body, false);
    render_frame(graph, on_body, false);

    TEST(! graph.interaction_active());
    TEST(drop_menu_invocations == 0);
    TEST(graph.take_changes(changes, max_pending_changes) == 0);
    TEST(graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);

    // Release outside the widget: drops landing on other UI always cancel,
    // no menu opens and the callback never runs.
    {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        drop_menu_invocations = 0;
        drop_menu_item_chosen = false;
        drop_menu_close_item  = false;

        Graph& outside_graph = reset_unit_graph(0);
        outside_graph.clear();
        outside_graph.set_colors(Sculptor::default_graph_colors());
        const uint32_t outside_anchor = add_param_anchor(outside_graph, vmath::vec2(16.0f, 16.0f), true);
        TEST(outside_anchor != Sculptor::pool_no_slot);
        GraphChange drop_changes[max_pending_changes];
        outside_graph.take_changes(drop_changes, max_pending_changes);
        outside_graph.set_drop_menu_callback(&test_drop_menu_callback, nullptr);

        // One settle frame first: audio_dot measures live font metrics, which
        // a fresh context only has after its first NewFrame.
        render_frame(outside_graph, ImVec2(900.0f, 600.0f), false);
        const ImVec2 anchor_dot = audio_dot(outside_graph, outside_anchor, false);
        render_frame(outside_graph, anchor_dot, false);
        render_frame(outside_graph, anchor_dot, true);
        TEST(outside_graph.interaction_active());
        const ImVec2 outside(1016.0f, 762.0f);
        render_frame(outside_graph, outside, true);
        render_frame(outside_graph, outside, false);
        render_frame(outside_graph, outside, false);

        TEST(! outside_graph.interaction_active());
        TEST(drop_menu_invocations == 0);
        TEST(outside_graph.take_changes(drop_changes, max_pending_changes) == 0);
        TEST(outside_graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);
    }
    ImGui::DestroyContext();
}

// A connected flagged dot retargets its existing wire; the
// empty-canvas release keeps the disconnect policy and never opens the menu.
static void test_drop_menu_connected_flagged_dot_retargets()
{
    create_renderer_context();

    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_item_chosen = false;
    drop_menu_close_item  = false;

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t source = graph.create_node("src", vmath::vec2(16.0f, 16.0f));
    TEST(source != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    snprintf(out.name, sizeof(out.name), "Out");
    TEST(graph.add_slot(source, out) != Sculptor::pool_no_slot);
    const uint32_t anchor = add_param_anchor(graph, vmath::vec2(256.0f, 256.0f), true);
    TEST(graph.add_connection(EndPoint{ source, 0 }, EndPoint{ anchor, 0 }) != Sculptor::pool_no_slot);
    GraphChange changes[max_pending_changes];
    graph.take_changes(changes, max_pending_changes);
    graph.set_drop_menu_callback(&test_drop_menu_callback, nullptr);

    const ImVec2 release(700.0f, 600.0f);
    // One settle frame first: audio_dot measures live font metrics, which a
    // fresh context only has after its first NewFrame.
    render_frame(graph, ImVec2(900.0f, 600.0f), false);
    const ImVec2 dot = audio_dot(graph, anchor, false);
    render_frame(graph, dot, false);
    render_frame(graph, dot, true);
    // The connected dot picks up its existing wire (retargeting), it does
    // not start a fresh connecting drag.
    TEST(graph.interaction_active());
    render_frame(graph, release, true);
    render_frame(graph, release, false);
    render_frame(graph, release, false);

    TEST(! graph.interaction_active());
    TEST(drop_menu_invocations == 0);
    TEST(graph.connection_count() == 0);
    TEST(graph.drop_menu_pos().x == 0.0f);
    TEST(graph.drop_menu_pos().y == 0.0f);
    bool           found_deleted = false;
    GraphChange    drained[max_pending_changes];
    const uint32_t count = graph.take_changes(drained, max_pending_changes);
    for (uint32_t i = 0; i < count; ++i) {
        found_deleted = found_deleted || drained[i].kind == ChangeKind::connection_deleted;
    }
    TEST(found_deleted);
    TEST(! graph.has_error());
    ImGui::DestroyContext();
}

// Deleting or slot-removing the open menu's anchor cancels the mode: a
// stale drop_menu_from would keep reading a freed (possibly reused) pool
// slot, and no wire may draw from whatever reuses the index.
static void test_drop_menu_anchor_deletion_cancels_mode()
{
    create_renderer_context();

    for (uint32_t route = 0; route < 2; ++route) {
        ImGui::ClosePopupsOverWindow(nullptr, false);
        drop_menu_invocations = 0;
        drop_menu_item_chosen = false;
        drop_menu_close_item  = false;

        Graph& graph = reset_unit_graph(0);
        graph.clear();
        GraphColors colors     = Sculptor::default_graph_colors();
        colors.connector_hover = 0x0ACD0AFFu; // the marker color
        graph.set_colors(colors);
        const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
        TEST(anchor != Sculptor::pool_no_slot);
        GraphChange changes[max_pending_changes];
        graph.take_changes(changes, max_pending_changes);

        open_drop_menu(graph, anchor, 0, ImVec2(700.0f, 600.0f));

        if (route == 0) {
            graph.delete_node(anchor);
        }
        else {
            graph.remove_slot(anchor, 0);
        }

        // The mode dies with the anchor; one rendered frame must not
        // resurrect it or draw a wire off stale state.
        uint32_t marker_count = 0;
        render_frame(graph, ImVec2(700.0f, 600.0f), false, false, false, &marker_count);
        TEST(! graph.interaction_active());
        TEST(graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);
        TEST(graph.drop_menu_anchor().slot_idx == Sculptor::pool_no_slot);
        TEST(graph.drop_menu_pos().x == 0.0f);
        TEST(graph.drop_menu_pos().y == 0.0f);
        TEST(marker_count == 0);
        const uint32_t count = graph.take_changes(changes, max_pending_changes);
        TEST(count == 1);
        if (count == 1) {
            TEST(changes[0].kind == (route == 0 ? ChangeKind::node_deleted : ChangeKind::slot_deleted));
        }

        // A reused node pool index must not be mistaken for the old anchor.
        if (route == 0) {
            const uint32_t reused = graph.create_node("reused", vmath::vec2(16.0f, 16.0f));
            TEST(reused == anchor);
            render_frame(graph, ImVec2(700.0f, 600.0f), false, false, false, &marker_count);
            TEST(! graph.interaction_active());
            TEST(marker_count == 0);
            const uint32_t reused_count = graph.take_changes(changes, max_pending_changes);
            TEST(reused_count == 1);
            if (reused_count == 1) {
                TEST(changes[0].kind == ChangeKind::node_added);
            }
        }
    }
    ImGui::DestroyContext();
}

// clear() must reset the drop-menu mode with the rest of the popup state;
// the fields are renderer-set private state, so this drives one real open.
static void test_clear_resets_drop_menu_state()
{
    create_renderer_context();

    ImGui::ClosePopupsOverWindow(nullptr, false);
    drop_menu_invocations = 0;
    drop_menu_item_chosen = false;
    drop_menu_close_item  = false;

    Graph& graph = reset_unit_graph(0);
    graph.clear();
    graph.set_colors(Sculptor::default_graph_colors());
    const uint32_t anchor = add_param_anchor(graph, vmath::vec2(16.0f, 16.0f), true);
    open_drop_menu(graph, anchor, 0, ImVec2(700.0f, 600.0f));

    graph.clear();
    TEST(! graph.interaction_active());
    TEST(graph.drop_menu_anchor().node_idx == Sculptor::pool_no_slot);
    TEST(graph.drop_menu_anchor().slot_idx == Sculptor::pool_no_slot);
    TEST(graph.drop_menu_pos().x == 0.0f);
    TEST(graph.drop_menu_pos().y == 0.0f);
    ImGui::DestroyContext();
}

// Direct model test of the eligibility predicate: the release path and the
// projections must agree on exactly which anchors open the menu.
static void test_drop_menu_eligible_model_rule()
{
    Graph& graph = reset_unit_graph(0);
    graph.clear();

    const uint32_t node = graph.create_node("osc", vmath::vec2(0.0f, 0.0f));
    const uint32_t src  = graph.create_node("src", vmath::vec2(64.0f, 0.0f));
    TEST(node != Sculptor::pool_no_slot);
    TEST(src != Sculptor::pool_no_slot);

    Slot flagged          = {};
    flagged.kind          = SlotKind::property;
    flagged.connectable   = true;
    flagged.property_type = PropertyType::real;
    flagged.drop_menu     = true;
    snprintf(flagged.name, sizeof(flagged.name), "volume");
    const uint32_t flag_free      = graph.add_slot(node, flagged); // flagged, unconnected
    const uint32_t flag_connected = graph.add_slot(node, flagged); // flagged, wired below
    TEST(flag_free != Sculptor::pool_no_slot);
    TEST(flag_connected != Sculptor::pool_no_slot);

    Slot unflagged      = flagged;
    unflagged.drop_menu = false;
    snprintf(unflagged.name, sizeof(unflagged.name), "tone");
    const uint32_t plain_param = graph.add_slot(node, unflagged); // connectable, unflagged
    TEST(plain_param != Sculptor::pool_no_slot);
    Slot plain_in          = {};
    plain_in.kind          = SlotKind::input;
    const uint32_t in_slot = graph.add_slot(node, plain_in); // unflagged input
    TEST(in_slot != Sculptor::pool_no_slot);
    Slot out = {};
    out.kind = SlotKind::output;
    TEST(graph.add_slot(src, out) != Sculptor::pool_no_slot);
    TEST(graph.add_connection(EndPoint{ src, 0 }, EndPoint{ node, flag_connected }) != Sculptor::pool_no_slot);

    // Flagged and unconnected is the only eligible shape.
    TEST(graph.drop_menu_eligible(EndPoint{ node, flag_free }));
    TEST(! graph.drop_menu_eligible(EndPoint{ node, flag_connected }));
    TEST(! graph.drop_menu_eligible(EndPoint{ node, plain_param }));
    TEST(! graph.drop_menu_eligible(EndPoint{ node, in_slot }));

    // Bounds and occupancy refusals, no flag can make them eligible.
    TEST(! graph.drop_menu_eligible(EndPoint{ Sculptor::pool_no_slot, 0 }));
    TEST(! graph.drop_menu_eligible(EndPoint{ max_nodes, 0 }));
    TEST(! graph.drop_menu_eligible(EndPoint{ max_nodes - 1, 0 })); // unoccupied node slot
    TEST(! graph.drop_menu_eligible(EndPoint{ node, max_node_slots }));
    TEST(! graph.drop_menu_eligible(EndPoint{ node, max_node_slots - 1 })); // unallocated slot
}

// C5 pinning test: the connection pool never compacts, so a deleted
// lower-index connection leaves a hole and survivors keep their stable
// indices.  Connection-derived features must enumerate the whole pool with
// connection_occupied, never a dense 0..connection_count()-1 range.
static void check_connection_pool_sparse_scan()
{
    Graph& g = reset_unit_graph(4);

    const uint32_t node_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    const uint32_t node_c = g.create_node("c", vmath::vec2(0.0f, 0.0f));
    TEST(node_a != Sculptor::pool_no_slot);
    TEST(node_b != Sculptor::pool_no_slot);
    TEST(node_c != Sculptor::pool_no_slot);

    Slot output_slot        = {};
    output_slot.kind        = SlotKind::output;
    Slot input_slot         = {};
    input_slot.kind         = SlotKind::input;
    const uint32_t out_idx  = g.add_slot(node_a, output_slot);
    const uint32_t in_b_idx = g.add_slot(node_b, input_slot);
    const uint32_t in_c_idx = g.add_slot(node_c, input_slot);
    TEST(out_idx != Sculptor::pool_no_slot);
    TEST(in_b_idx != Sculptor::pool_no_slot);
    TEST(in_c_idx != Sculptor::pool_no_slot);

    const uint32_t first  = g.add_connection(EndPoint{ node_a, out_idx }, EndPoint{ node_b, in_b_idx });
    const uint32_t second = g.add_connection(EndPoint{ node_a, out_idx }, EndPoint{ node_c, in_c_idx });
    TEST(first != Sculptor::pool_no_slot);
    TEST(second != Sculptor::pool_no_slot);
    // First-fit allocation gives adjacent indices, so deleting the lower
    // one really opens a hole in the middle of the occupied range.
    TEST(first < second);

    GraphChange discard[8] = {};
    drain_changes(g, discard, 8);

    g.delete_connection(first);
    drain_changes(g, discard, 8);

    TEST(! g.connection_occupied(first));
    TEST(g.connection_occupied(second));
    TEST(g.connection_count() == 1);

    // The documented sparse scan finds exactly one survivor: the
    // higher-index connection, with its endpoints intact.  A dense
    // 0..connection_count()-1 scan would read the freed hole instead.
    uint32_t found = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < max_connections; ++i) {
        if (! g.connection_occupied(i)) {
            continue;
        }
        TEST(found == Sculptor::pool_no_slot); // exactly one survivor
        found = i;
    }
    TEST(found == second);
    TEST(g.get_connection(found).output.node_idx == node_a);
    TEST(g.get_connection(found).output.slot_idx == out_idx);
    TEST(g.get_connection(found).input.node_idx == node_c);
    TEST(g.get_connection(found).input.slot_idx == in_c_idx);
}

// C6 model check: collect_selected_source_inputs marks exactly the input
// slots of one node that receive a connection from a selected node, unions
// across multiple selected sources, and walks the connection pool with
// connection_occupied so a deleted lower-index hole cannot break it.
static void check_collect_selected_source_inputs_marks_fed_inputs()
{
    Graph& g = reset_unit_graph(0);

    const uint32_t source_a = g.create_node("a", vmath::vec2(0.0f, 0.0f));
    const uint32_t source_b = g.create_node("b", vmath::vec2(0.0f, 0.0f));
    const uint32_t receiver = g.create_node("receiver", vmath::vec2(0.0f, 0.0f));
    TEST(source_a != Sculptor::pool_no_slot);
    TEST(source_b != Sculptor::pool_no_slot);
    TEST(receiver != Sculptor::pool_no_slot);

    Slot output_slot         = {};
    output_slot.kind         = SlotKind::output;
    Slot input_slot          = {};
    input_slot.kind          = SlotKind::input;
    const uint32_t out_a     = g.add_slot(source_a, output_slot);
    const uint32_t out_b     = g.add_slot(source_b, output_slot);
    const uint32_t in_slot_0 = g.add_slot(receiver, input_slot);
    const uint32_t in_slot_1 = g.add_slot(receiver, input_slot);
    const uint32_t in_slot_2 = g.add_slot(receiver, input_slot);
    TEST(out_a != Sculptor::pool_no_slot);
    TEST(out_b != Sculptor::pool_no_slot);
    TEST(in_slot_0 != Sculptor::pool_no_slot);
    TEST(in_slot_1 != Sculptor::pool_no_slot);
    TEST(in_slot_2 != Sculptor::pool_no_slot);

    const uint32_t conn_a_0 = g.add_connection(EndPoint{ source_a, out_a }, EndPoint{ receiver, in_slot_0 });
    const uint32_t conn_a_1 = g.add_connection(EndPoint{ source_a, out_a }, EndPoint{ receiver, in_slot_1 });
    const uint32_t conn_b_2 = g.add_connection(EndPoint{ source_b, out_b }, EndPoint{ receiver, in_slot_2 });
    TEST(conn_a_0 != Sculptor::pool_no_slot);
    TEST(conn_a_1 != Sculptor::pool_no_slot);
    TEST(conn_b_2 != Sculptor::pool_no_slot);

    GraphChange discard[8] = {};
    drain_changes(g, discard, 8);

    // Two sentinel entries tail the flag array: the helper must never
    // write past the max_node_slots entries it is given.
    bool flags[max_node_slots + 2] = {};

    // One selected source feeding two inputs: exactly those two entries
    // are set, every other entry stays false.
    g.set_selected(source_a, true);
    Sculptor::collect_selected_source_inputs(g, receiver, flags);
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        TEST(flags[i] == (i == in_slot_0 || i == in_slot_1));
    }
    TEST(! flags[max_node_slots]);
    TEST(! flags[max_node_slots + 1]);

    // No selection: every entry, sentinels included, stays false.
    g.select_none();
    memset(flags, 0, sizeof(flags));
    Sculptor::collect_selected_source_inputs(g, receiver, flags);
    for (uint32_t i = 0; i < max_node_slots + 2; ++i) {
        TEST(! flags[i]);
    }

    // Two selected sources feeding different inputs: the union is marked.
    g.set_selected(source_a, true);
    g.set_selected(source_b, true);
    memset(flags, 0, sizeof(flags));
    Sculptor::collect_selected_source_inputs(g, receiver, flags);
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        TEST(flags[i] == (i == in_slot_0 || i == in_slot_1 || i == in_slot_2));
    }
    TEST(! flags[max_node_slots]);
    TEST(! flags[max_node_slots + 1]);

    // First-fit allocation gives adjacent indices, so deleting conn_a_0
    // (the lower of the source_a pair) opens a hole in the middle of the
    // occupied range.  The surviving connection must still be found, so
    // the helper has to scan the pool sparsely with connection_occupied.
    g.delete_connection(conn_a_0);
    drain_changes(g, discard, 8);
    TEST(! g.connection_occupied(conn_a_0));
    TEST(g.connection_occupied(conn_a_1));

    g.select_none();
    g.set_selected(source_a, true);
    memset(flags, 0, sizeof(flags));
    Sculptor::collect_selected_source_inputs(g, receiver, flags);
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        TEST(flags[i] == (i == in_slot_1));
    }
    TEST(! flags[max_node_slots]);
    TEST(! flags[max_node_slots + 1]);

    // The other source still marks its own slot across the hole.
    g.select_none();
    g.set_selected(source_b, true);
    memset(flags, 0, sizeof(flags));
    Sculptor::collect_selected_source_inputs(g, receiver, flags);
    for (uint32_t i = 0; i < max_node_slots; ++i) {
        TEST(flags[i] == (i == in_slot_2));
    }
    TEST(! flags[max_node_slots]);
    TEST(! flags[max_node_slots + 1]);
}

int main()
{
    test_renderer_audio_endpoint_mouse_frames();
    test_renderer_title_spacing_and_child_row_elbows();
    test_renderer_child_row_sibling_rails();
    test_drop_menu_eligible_model_rule();
    test_drop_menu_dismissal_paths_leave_graph_unchanged();
    test_drop_menu_pending_wire_frozen_at_release_point();
    test_drop_menu_pending_wire_identity_view();
    test_drop_menu_non_eligible_anchors_cancel();
    test_drop_menu_connected_flagged_dot_retargets();
    test_drop_menu_anchor_deletion_cancels_mode();
    test_user_disconnect_and_intent_capacity();
    test_retarget_occupied_input_without_interceptor();
    test_peek_changes_preserves_ring();
    test_create_node_distinct_indices();
    test_create_node_exhaustion_is_safe();
    test_create_node_snaps_position_to_grid();
    test_snap_to_grid_values();
    test_add_slot_grows_within_limit();
    test_add_slot_overflow_returns_no_slot();
    test_delete_node_frees_index_and_drops_connections();
    test_add_delete_connection_basics();
    test_clear_resets_popup_and_error();
    test_connection_capacity_raised_to_1152();
    test_slot_color_callback_api();
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
    check_connection_pool_sparse_scan();
    check_collect_selected_source_inputs_marks_fed_inputs();

    return exit_code;
}
