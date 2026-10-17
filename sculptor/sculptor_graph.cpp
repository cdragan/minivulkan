// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Graph widget data model: pools, change events, colors, persistence.
// Deliberately free of ImGui calls so the unit test can link this file;
// rendering and interaction live in sculptor_graph_render.cpp.

#include "sculptor_graph.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

const char* const Sculptor::source_op_names[2] = { "+", "x" };
// Effect and oscillator LFO waves the runtime can evaluate (eval_lfo_mod);
// pulse and noise would trip the runtime's palette assert, and bank
// validation rejects them.
const char* const Sculptor::lfo_wave_names[2] = { "Sine", "Sawtooth" };

const char* const Sculptor::mod_source_names[6] = { "Pitch bend", "Mod wheel",  "Channel press.",
                                                    "Velocity",   "Aftertouch", "Pressure (max)" };

// Pure floor-to-grid math, shared by the renderer and the drop-menu
// consumers; the negative direction snaps toward negative infinity, not
// toward zero.
float Sculptor::snap_to_grid(float value)
{
    return static_cast<float>(floor(value / graph_grid_spacing) * graph_grid_spacing);
}

Sculptor::Slot Sculptor::make_slot(const char* name, Sculptor::SlotKind kind, Sculptor::PropertyType property_type)
{
    Sculptor::Slot slot = {};
    snprintf(slot.name, sizeof(slot.name), "%s", name);
    slot.kind          = kind;
    slot.connectable   = false;
    slot.property_type = property_type;
    return slot;
}

Sculptor::Slot Sculptor::output_slot(const char* name)
{
    return make_slot(name, Sculptor::SlotKind::output);
}

Sculptor::Slot Sculptor::input_slot(const char* name)
{
    return make_slot(name, Sculptor::SlotKind::input);
}

Sculptor::Slot Sculptor::real_slot(const char* name, float value)
{
    Sculptor::Slot slot = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::real);
    slot.value.real     = value;
    return slot;
}

// Bounded real: the renderer draws a slider clamped to the range.
Sculptor::Slot Sculptor::bounded_real_slot(const char* name,
                                           float       value,
                                           float       min_value,
                                           float       max_value,
                                           bool        logarithmic)
{
    Sculptor::Slot slot   = real_slot(name, value);
    slot.real_min         = min_value;
    slot.real_max         = max_value;
    slot.real_bounded     = true;
    slot.real_logarithmic = logarithmic;
    return slot;
}

Sculptor::Slot Sculptor::int_slot(const char* name, int32_t value)
{
    Sculptor::Slot slot = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::integer);
    slot.value.integer  = value;
    return slot;
}

// Bounded integer: the renderer draws a slider clamped to the range.
Sculptor::Slot Sculptor::bounded_int_slot(const char* name, int32_t value, int32_t min_value, int32_t max_value)
{
    Sculptor::Slot slot = int_slot(name, value);
    slot.real_min       = static_cast<float>(min_value);
    slot.real_max       = static_cast<float>(max_value);
    slot.int_bounded    = true;
    return slot;
}

Sculptor::Slot Sculptor::list_slot(const char* name, const char* const* options, uint32_t num_options, uint32_t index)
{
    Sculptor::Slot slot   = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::list);
    slot.num_list_options = static_cast<uint8_t>(num_options);
    for (uint32_t i = 0; i < num_options; ++i) {
        snprintf(slot.list_options[i], sizeof(slot.list_options[i]), "%s", options[i]);
    }
    slot.value.list_index = static_cast<uint8_t>(index);
    return slot;
}

Sculptor::Slot Sculptor::toggle_slot(const char* name, bool value)
{
    Sculptor::Slot slot = make_slot(name, Sculptor::SlotKind::property, Sculptor::PropertyType::toggle);
    slot.value.toggle   = value;
    return slot;
}

void Sculptor::append_mod_source_row(Sculptor::Graph* graph,
                                     uint32_t         node,
                                     uint32_t         input_idx,
                                     uint8_t          row_group,
                                     uint32_t         op_index,
                                     float            amount,
                                     uint8_t          level_base)
{
    Sculptor::Slot src_dot = input_slot(input_idx == 0 ? "Source A" : "Source B");
    src_dot.row_group      = row_group;
    src_dot.child_row      = true;
    src_dot.child_level    = level_base;
    graph->add_slot(node, src_dot);

    Sculptor::Slot src_op = list_slot("Op", source_op_names, 2, op_index);
    src_op.row_group      = row_group;
    src_op.child_row      = true;
    src_op.child_level    = level_base;
    graph->add_slot(node, src_op);

    Sculptor::Slot src_amount = real_slot("Amount", amount);
    src_amount.child_row      = true;
    src_amount.child_level    = static_cast<uint8_t>(level_base + 1);
    graph->add_slot(node, src_amount);
}

// Adds the LFO row shared by the oscillator and effect projections: the
// LFO wire lands on the dot, Op picks how the wave combines with the
// base, and Depth sets the strength on its own nested line.  Either
// output pointer may be null.
void Sculptor::append_lfo_row(Sculptor::Graph* graph,
                              uint32_t         node,
                              uint8_t          row_group,
                              uint32_t         op_index,
                              float            depth,
                              uint32_t*        op_slot,
                              uint32_t*        depth_slot,
                              uint8_t          level_base)
{
    Sculptor::Slot lfo_dot = input_slot("LFO");
    lfo_dot.row_group      = row_group;
    lfo_dot.child_row      = true;
    lfo_dot.child_level    = level_base;
    graph->add_slot(node, lfo_dot);

    Sculptor::Slot lfo_op = list_slot("Op", source_op_names, 2, op_index);
    lfo_op.row_group      = row_group;
    lfo_op.child_row      = true;
    lfo_op.child_level    = level_base;
    const uint32_t op_idx = graph->add_slot(node, lfo_op);

    Sculptor::Slot lfo_depth = real_slot("Depth", depth);
    lfo_depth.child_row      = true;
    lfo_depth.child_level    = static_cast<uint8_t>(level_base + 1);
    const uint32_t depth_idx = graph->add_slot(node, lfo_depth);

    if (op_slot != nullptr) {
        *op_slot = op_idx;
    }

    if (depth_slot != nullptr) {
        *depth_slot = depth_idx;
    }
}

Sculptor::GraphColors Sculptor::default_graph_colors()
{

    Sculptor::GraphColors colors    = {};
    colors.node_background          = 0x2B2B2BFFu;
    colors.node_border              = 0x5A5A5AFFu;
    colors.node_selected_border     = 0xFFFFFFFFu;
    colors.node_title               = 0xE0E0E0FFu;
    colors.grid_line                = 0x353535FFu;
    colors.grid_axis                = 0x404040FFu;
    colors.connector                = 0x909090FFu;
    colors.connector_hover          = 0xFFFFFFE0u;
    colors.connector_connected      = 0xE0E0E0FFu;
    colors.connector_missing        = 0xFF4040FFu;
    colors.connection               = 0xC0C0C0FFu;
    colors.ghost_node               = 0x4A4A4A80u;
    colors.property_value           = 0xD0D0D0FFu;
    colors.property_connected_value = 0x808080FFu;
    colors.error_background         = 0x602020E0u;
    colors.error_text               = 0xFFE0E0FFu;
    colors.selection_outline        = 0xFFFFFFFFu;
    colors.selection_band           = 0xFFFFFF30u;

    return colors;
}

namespace {

// The one-edge refusal names the node already wired into the input, so the
// user can find the competing wire without tracing it by hand.
void set_input_taken_error(Sculptor::Graph& graph, Sculptor::EndPoint input)
{
    for (uint32_t i = 0; i < Sculptor::max_connections; ++i) {
        if (! graph.connection_occupied(i)) {
            continue;
        }
        const Sculptor::Connection& connection = graph.get_connection(i);
        if (connection.input.node_idx == input.node_idx && connection.input.slot_idx == input.slot_idx) {
            char message[96];
            snprintf(message,
                     sizeof(message),
                     "Input already connected (%s)",
                     graph.node(connection.output.node_idx).name);
            graph.set_error(message);
            return;
        }
    }
    graph.set_error("Input already connected");
}
} // namespace

// The editor state carve-out in sculptor_instr_bank.cpp reserves one
// editor-resident Graph slot at exactly max_graph_bytes; a capacity raise
// that outgrows it must fail the build instead of silently busting that
// reserve.
static_assert(sizeof(Sculptor::Graph) <= Sculptor::max_graph_bytes);

bool Sculptor::Graph::push_change(ChangeKind kind, uint32_t node_idx, uint32_t slot_idx, uint32_t connection_idx)
{
    // A clear() rebuild suppresses its own construction traffic; the next
    // drain re-arms the ring.
    if (changes_quiet) {
        return false;
    }
    if (changes_count == max_pending_changes) {
        // Caller did not drain fast enough: force a resync instead of losing
        // events silently.
        changes_overflowed_flag = true;
        return false;
    }
    GraphChange& change               = changes[(changes_head + changes_count) % max_pending_changes];
    change.kind                       = kind;
    change.node_idx                   = node_idx;
    change.slot_idx                   = slot_idx;
    change.connection_idx             = connection_idx;
    change.connection_output.node_idx = pool_no_slot;
    change.connection_output.slot_idx = pool_no_slot;
    change.connection_input.node_idx  = pool_no_slot;
    change.connection_input.slot_idx  = pool_no_slot;
    ++changes_count;
    return true;
}

void Sculptor::Graph::push_connection_change(ChangeKind kind,
                                             uint32_t   connection_idx,
                                             EndPoint   output,
                                             EndPoint   input,
                                             EndPoint   prev_input)
{
    // Only extend an event that was actually recorded: a suppressed or dropped
    // event leaves no ring slot to amend.
    if (! push_change(kind, pool_no_slot, pool_no_slot, connection_idx)) {
        return;
    }
    GraphChange& change          = changes[(changes_head + changes_count - 1) % max_pending_changes];
    change.connection_output     = output;
    change.connection_input      = input;
    change.connection_prev_input = prev_input;
}

uint32_t Sculptor::Graph::peek_changes(GraphChange* out, uint32_t capacity, bool* overflow) const
{
    if (! overflow) {
        return 0;
    }
    *overflow = changes_overflowed_flag || capacity < changes_count || (! out && capacity);
    if (! out) {
        return 0;
    }
    const uint32_t count = capacity < changes_count ? capacity : changes_count;
    for (uint32_t index = 0; index < count; ++index) {
        out[index] = changes[(changes_head + index) % max_pending_changes];
    }
    return count;
}

uint32_t Sculptor::Graph::take_changes(GraphChange* out, uint32_t out_size)
{
    // Any drain re-arms event reporting after a clear() rebuild.
    changes_quiet           = false;
    bool           overflow = false;
    const uint32_t count    = peek_changes(out, out_size, &overflow);
    changes_head            = (changes_head + count) % max_pending_changes;
    changes_count -= count;
    return count;
}

bool Sculptor::Graph::changes_overflowed()
{
    const bool overflowed   = changes_overflowed_flag;
    changes_overflowed_flag = false;
    return overflowed;
}

bool Sculptor::Graph::slot_is_connected(uint32_t node_idx, uint32_t slot_idx) const
{
    for (uint32_t i = 0; i < max_connections; ++i) {
        if (! connections.is_occupied(i)) {
            continue;
        }
        const Connection& connection = connections.entries[i];
        if (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx) {
            return true;
        }
        if (connection.output.node_idx == node_idx && connection.output.slot_idx == slot_idx) {
            return true;
        }
    }
    return false;
}

// The release path and the projections must agree on exactly which anchors
// open the drop menu: the slot is flagged at projection time and
// unconnected (connected dots retarget existing connections).
bool Sculptor::Graph::drop_menu_eligible(EndPoint anchor) const
{
    if (anchor.node_idx >= max_nodes || ! nodes.is_occupied(anchor.node_idx) || anchor.slot_idx >= max_node_slots ||
        ! nodes.entries[anchor.node_idx].slots.is_occupied(anchor.slot_idx)) {
        return false;
    }
    const Slot& slot = nodes.entries[anchor.node_idx].slots.entries[anchor.slot_idx];
    return slot.drop_menu && ! slot_is_connected(anchor.node_idx, anchor.slot_idx);
}

uint32_t Sculptor::Graph::create_node(const char* name, vmath::vec2 position)
{
    const uint32_t node_idx = nodes.allocate();
    if (node_idx == pool_no_slot) {
        return pool_no_slot;
    }
    Node& new_node = nodes.entries[node_idx];
    new_node       = Node();
    // Defensive: parallel per-slot arrays must not leak state into a reused
    // slot (delete_node clears them too; this covers slots freed elsewhere).
    selected[node_idx]      = false;
    content_sizes[node_idx] = vmath::vec2(0.0f, 0.0f);
    snprintf(new_node.name, sizeof(new_node.name), "%s", name ? name : "");
    // Snap the initial position so new nodes align with the grid.
    new_node.position = vmath::vec2(Sculptor::snap_to_grid(position.x), Sculptor::snap_to_grid(position.y));
    push_change(ChangeKind::node_added, node_idx, pool_no_slot, pool_no_slot);
    return node_idx;
}

uint32_t Sculptor::Graph::delete_selected()
{
    uint32_t deleted = 0;
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (! nodes.is_occupied(i) || ! selected[i] || nodes.entries[i].ghost) {
            continue;
        }
        delete_node(i);
        if (! nodes.is_occupied(i)) {
            ++deleted; // a vetoed delete leaves the node in place
        }
    }
    return deleted;
}

void Sculptor::Graph::delete_node(uint32_t node_idx)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    // The veto runs before any mutation: a refused delete leaves the node,
    // its slots and its connections untouched and pushes no change events;
    // the refusal surfaces through the error overlay.  A veto may report a
    // specific reason via set_error; the generic text applies only when it
    // did not.
    if (delete_veto) {
        const bool had_error = error_active;
        error_active         = false;
        if (delete_veto(delete_veto_user_data, node_idx)) {
            if (! error_active) {
                set_error("Node deletion refused");
            }
            return;
        }
        error_active = had_error;
    }
    delete_node_unvetoed(node_idx);
}

void Sculptor::Graph::delete_node_unvetoed(uint32_t node_idx)
{
    // Drop connections touching the node first, reporting each one, so the
    // caller never sees a connection referencing a dead node.  delete_connection
    // also cancels an active retarget of a dropped connection.
    for (uint32_t i = 0; i < max_connections; ++i) {
        if (! connections.is_occupied(i)) {
            continue;
        }
        const Connection& connection = connections.entries[i];
        if (connection.input.node_idx == node_idx || connection.output.node_idx == node_idx) {
            delete_connection(i);
        }
    }
    // Cancel an active new-connection drag whose anchor node is being deleted;
    // a later release must never connect from a reused pool slot.
    if (interaction == Interaction::connecting && connecting_from.node_idx == node_idx) {
        interaction         = Interaction::idle;
        retarget_connection = pool_no_slot;
    }
    // Same for an open drop menu anchored to the deleted node: a stale mode
    // would keep reading a freed (possibly reused) pool slot.
    if (interaction == Interaction::drop_menu && drop_menu_from.node_idx == node_idx) {
        interaction     = Interaction::idle;
        drop_menu_from  = EndPoint{ pool_no_slot, pool_no_slot };
        drop_menu_point = {};
    }
    nodes.free(node_idx);
    selected[node_idx]      = false;
    content_sizes[node_idx] = vmath::vec2(0.0f, 0.0f);
    memset(&slot_edit_disabled_flags[node_idx * max_node_slots], 0, max_node_slots);
    if (dragged_node == node_idx) {
        dragged_node = pool_no_slot;
        interaction  = Interaction::idle;
    }
    if (renaming_node == node_idx) {
        renaming_node = pool_no_slot;
        interaction   = Interaction::idle;
    }
    if (popup_node == node_idx) {
        popup_node = pool_no_slot;
    }
    push_change(ChangeKind::node_deleted, node_idx, pool_no_slot, pool_no_slot);
}

// Wholesale rebuild primitive: deletes every node, which drops every
// connection with it.  Bypasses the delete veto: this is caller-driven
// state replacement (projection, bank load), not a user delete.
void Sculptor::Graph::clear()
{
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (nodes.is_occupied(i)) {
            delete_node_unvetoed(i);
        }
    }
    // Pool entries keep their bytes after deletion.  clear() resets the
    // retained pool storage so freed node bytes never survive a rebuild.
    // Normal creation is safe regardless: create_node() value-initializes
    // each entry.  Reset per entry instead of assigning a fresh Pool
    // temporary: the temporary is pool-sized and overflows the 8 MiB main
    // stack under AddressSanitizer now that the pool holds 178 nodes.
    for (uint32_t i = 0; i < max_nodes; ++i) {
        nodes.entries[i] = Node{};
    }
    // The canvas popup and the error overlay describe pre-clear state: a
    // stale popup must not delete a reused connection index, and no stale
    // error text survives a rebuild.
    popup_connection = pool_no_slot;
    popup_node       = pool_no_slot;
    error_message[0] = 0;
    error_active     = false;
    // A rebuild can run mid-gesture (undo/redo during a drag, refused
    // commits): pool reuse would bind stale interaction state to different
    // nodes, so every interaction cursor resets with the graph.
    interaction         = Interaction::idle;
    dragged_node        = pool_no_slot;
    renaming_node       = pool_no_slot;
    retarget_connection = pool_no_slot;
    connecting_from     = EndPoint{ pool_no_slot, pool_no_slot };
    drop_menu_from      = EndPoint{ pool_no_slot, pool_no_slot };
    drop_menu_point     = {};
    // Missing marks belong to the pre-rebuild projection; stale bits would
    // hand red dots to reused (node, slot) indices.
    memset(slot_missing_bits, 0, sizeof(slot_missing_bits));
    // Rebuild traffic must not echo into the edit pipeline: reset the ring
    // with the graph and suppress events until the next drain re-arms it.
    changes_head            = 0;
    changes_count           = 0;
    changes_overflowed_flag = false;
    changes_quiet           = true;
}

// Removes a single slot, dropping connections that reference it first
// (same hygiene as delete_node).  Used by callers rebuilding a node's
// slot set incrementally.
void Sculptor::Graph::remove_slot(uint32_t node_idx, uint32_t slot_idx)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx) || slot_idx >= max_node_slots ||
        ! nodes.entries[node_idx].slots.is_occupied(slot_idx)) {
        return;
    }
    for (uint32_t i = 0; i < max_connections; ++i) {
        if (! connections.is_occupied(i)) {
            continue;
        }
        const Connection& connection = connections.entries[i];
        if ((connection.output.node_idx == node_idx && connection.output.slot_idx == slot_idx) ||
            (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx)) {
            delete_connection(i);
        }
    }
    // Same as delete_node: an open drop menu anchored to the removed slot
    // must not survive it reading a freed (possibly reused) pool slot.
    if (interaction == Interaction::drop_menu && drop_menu_from.node_idx == node_idx &&
        drop_menu_from.slot_idx == slot_idx) {
        interaction     = Interaction::idle;
        drop_menu_from  = EndPoint{ pool_no_slot, pool_no_slot };
        drop_menu_point = {};
    }
    nodes.entries[node_idx].slots.free(slot_idx);
    push_change(ChangeKind::slot_deleted, node_idx, slot_idx, pool_no_slot);
}

uint32_t Sculptor::Graph::add_slot(uint32_t node_idx, const Slot& slot)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return pool_no_slot;
    }
    Node&          node     = nodes.entries[node_idx];
    const uint32_t slot_idx = node.slots.allocate();
    if (slot_idx == pool_no_slot) {
        return pool_no_slot;
    }
    node.slots.entries[slot_idx]                                                     = slot;
    node.slots.entries[slot_idx].name[sizeof(node.slots.entries[slot_idx].name) - 1] = '\0';
    push_change(ChangeKind::slot_added, node_idx, slot_idx, pool_no_slot);
    return slot_idx;
}

bool Sculptor::Graph::endpoints_structurally_valid(EndPoint output, EndPoint input) const
{
    if (output.node_idx >= max_nodes || input.node_idx >= max_nodes || output.slot_idx >= max_node_slots ||
        input.slot_idx >= max_node_slots) {
        return false;
    }
    if (! nodes.is_occupied(output.node_idx) || ! nodes.is_occupied(input.node_idx)) {
        return false;
    }
    if (! nodes.entries[output.node_idx].slots.is_occupied(output.slot_idx) ||
        ! nodes.entries[input.node_idx].slots.is_occupied(input.slot_idx)) {
        return false;
    }
    // Semantic endpoint contract: connections go from an output slot to an
    // input slot or a connectable property.  The caller validator (if any)
    // runs on top of this structural check.
    const Slot& output_slot = nodes.entries[output.node_idx].slots.entries[output.slot_idx];
    const Slot& input_slot  = nodes.entries[input.node_idx].slots.entries[input.slot_idx];
    if (output_slot.kind != SlotKind::output) {
        return false;
    }
    if (input_slot.kind != SlotKind::input && ! (input_slot.kind == SlotKind::property && input_slot.connectable)) {
        return false;
    }
    return true;
}

bool Sculptor::Graph::input_slot_taken(uint32_t node_idx, uint32_t slot_idx, uint32_t except_connection) const
{
    for (uint32_t i = 0; i < max_connections; ++i) {
        if (i == except_connection || ! connections.is_occupied(i)) {
            continue;
        }
        const Connection& connection = connections.entries[i];
        if (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx) {
            return true;
        }
    }
    return false;
}

uint32_t Sculptor::Graph::add_connection(EndPoint output, EndPoint input)
{
    if (! endpoints_structurally_valid(output, input)) {
        return pool_no_slot;
    }
    // Note: no single-connection-per-input rule here.  The raw API allows
    // fan-in (the M1 pool-fill test relies on it); the validated user-facing
    // paths attempt_connection and move_connection_end enforce the rule.
    const uint32_t connection_idx = connections.allocate();
    if (connection_idx == pool_no_slot) {
        return pool_no_slot;
    }
    Connection& connection = connections.entries[connection_idx];
    connection.output      = output;
    connection.input       = input;
    push_connection_change(ChangeKind::connection_added, connection_idx, output, input, { pool_no_slot, pool_no_slot });
    return connection_idx;
}

void Sculptor::Graph::set_validator(ValidationCallback callback, void* user_data)
{
    validator           = callback;
    validator_user_data = user_data;
}

void Sculptor::Graph::set_slot_hidden_callback(SlotHiddenCallback callback, void* user_data)
{
    slot_hidden_callback  = callback;
    slot_hidden_user_data = user_data;
}

void Sculptor::Graph::set_slot_gray_callback(SlotGrayCallback callback, void* user_data)
{
    slot_gray_callback  = callback;
    slot_gray_user_data = user_data;
}

bool Sculptor::Graph::user_delete_connection(uint32_t connection_idx)
{
    if (connection_idx >= max_connections || ! connections.is_occupied(connection_idx)) {
        return false;
    }
    delete_connection(connection_idx);
    return true;
}

void Sculptor::Graph::set_delete_veto(NodeDeleteVeto callback, void* user_data)
{
    delete_veto           = callback;
    delete_veto_user_data = user_data;
}

void Sculptor::Graph::set_canvas_menu_callback(CanvasMenuCallback callback, void* user_data)
{
    canvas_menu_callback  = callback;
    canvas_menu_user_data = user_data;
}

void Sculptor::Graph::set_drop_menu_callback(DropMenuCallback callback, void* user_data)
{
    drop_menu_callback  = callback;
    drop_menu_user_data = user_data;
}

void Sculptor::Graph::set_node_menu_callback(NodeMenuCallback callback, void* user_data)
{
    node_menu_callback  = callback;
    node_menu_user_data = user_data;
}

// Per-slot "missing" render bits: live only while the editor marks them, so
// they never enter graph snapshots and the zero-filled state marks nothing.
void Sculptor::Graph::set_slot_missing(uint32_t node_idx, uint32_t slot_idx, bool missing)
{
    if (node_idx >= nodes.num_allocated || ! nodes.is_occupied(node_idx)) {
        return;
    }
    const Node& node = nodes.entries[node_idx];
    if (slot_idx >= node.slots.num_allocated) {
        return;
    }
    const uint32_t bit  = node_idx * max_node_slots + slot_idx;
    const uint8_t  mask = static_cast<uint8_t>(1u << (bit % 8u));
    if (missing) {
        slot_missing_bits[bit / 8u] |= mask;
    }
    else {
        slot_missing_bits[bit / 8u] &= static_cast<uint8_t>(~mask);
    }
}

bool Sculptor::Graph::slot_missing(uint32_t node_idx, uint32_t slot_idx) const
{
    // The bits carry no occupancy state of their own: clear() wipes them,
    // so between rebuilds only the node-occupancy and slot-range guards
    // keep a stale or out-of-range bit from surfacing through the getter.
    if (node_idx >= nodes.num_allocated || ! nodes.is_occupied(node_idx)) {
        return false;
    }
    if (slot_idx >= nodes.entries[node_idx].slots.num_allocated) {
        return false;
    }
    const uint32_t bit = node_idx * max_node_slots + slot_idx;
    return (slot_missing_bits[bit / 8u] & (1u << (bit % 8u))) != 0;
}

void Sculptor::Graph::set_slot_edit_disabled(uint32_t node_idx, uint32_t slot_idx, bool disabled)
{
    if (node_idx >= max_nodes || slot_idx >= max_node_slots) {
        return;
    }
    slot_edit_disabled_flags[node_idx * max_node_slots + slot_idx] = disabled ? 1 : 0;
}

bool Sculptor::Graph::slot_edit_disabled(const uint32_t node_idx, const uint32_t slot_idx) const
{
    return node_idx < max_nodes && slot_idx < max_node_slots &&
           slot_edit_disabled_flags[node_idx * max_node_slots + slot_idx] != 0;
}

void Sculptor::Graph::set_node_renamable(uint32_t node_idx, bool renamable)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    nodes.entries[node_idx].renamable = renamable;
}

void Sculptor::Graph::set_node_oscillator(uint32_t node_idx, bool oscillator)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    nodes.entries[node_idx].oscillator = oscillator;
}

// Eventless property write: the shared-routing fan-out updates sibling
// oscillator nodes without echoing change events back into the edit pipeline.
void Sculptor::Graph::set_slot_value(uint32_t node_idx, uint32_t slot_idx, PropertyValue value)
{
    if (node_idx >= nodes.num_allocated || ! nodes.is_occupied(node_idx)) {
        return;
    }
    Node& node = nodes.entries[node_idx];
    if (slot_idx >= node.slots.num_allocated || node.slots.entries[slot_idx].kind != SlotKind::property) {
        return;
    }
    node.slots.entries[slot_idx].value = value;
}

void Sculptor::Graph::set_slot_input(uint32_t node_idx, uint32_t slot_idx, EndPoint output)
{
    if (node_idx >= nodes.num_allocated || ! nodes.is_occupied(node_idx)) {
        return;
    }
    // Input slots only: the connect paths never attach edges to another
    // slot kind, so the mirror target cannot be created elsewhere.
    if (slot_idx >= nodes.entries[node_idx].slots.num_allocated ||
        nodes.entries[node_idx].slots.entries[slot_idx].kind != SlotKind::input) {
        return;
    }
    // The single edge entering the slot is the mirror target; fan-in never
    // exists on these input slots (the widget's connect paths enforce it).
    const uint32_t existing = connection_into(*this, node_idx, slot_idx);
    if (output.node_idx == pool_no_slot) {
        if (existing != pool_no_slot) {
            connections.free(existing);
        }
        return;
    }
    if (! endpoints_structurally_valid(output, EndPoint{ node_idx, slot_idx })) {
        return;
    }
    if (existing != pool_no_slot) {
        connections.entries[existing].output = output;
        return;
    }
    const uint32_t added = connections.allocate();
    if (added == pool_no_slot) {
        return;
    }
    connections.entries[added].output = output;
    connections.entries[added].input  = EndPoint{ node_idx, slot_idx };
}

// Eventless connection rewrite: see the header comment. The rewrite never
// validates semantics - callers compute structurally valid endpoints (the
// retarget shift moves a wire within one oscillator node's row block).
void Sculptor::Graph::set_connection_input(uint32_t connection_idx, EndPoint input)
{
    if (connection_idx >= max_connections || ! connections.is_occupied(connection_idx)) {
        return;
    }
    if (input.node_idx == pool_no_slot) {
        connections.free(connection_idx);
        return;
    }
    connections.entries[connection_idx].input = input;
}

void Sculptor::Graph::rename_node(uint32_t node_idx, const char* name)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx) || ! name) {
        return;
    }
    Node& node = nodes.entries[node_idx];
    snprintf(node.name, sizeof(node.name), "%s", name);
    push_change(ChangeKind::name_changed, node_idx, pool_no_slot, pool_no_slot);
}

void Sculptor::Graph::set_error(const char* message)
{
    snprintf(error_message, sizeof error_message, "%s", message);
    error_active = true;
}

bool Sculptor::Graph::attempt_connection(EndPoint output, EndPoint input)
{
    if (! endpoints_structurally_valid(output, input)) {
        set_error("Invalid connection");
        return false;
    }
    if (validator) {
        // A validator may set a specific refusal message; only fall back to
        // the generic text when it did not.  The stale overlay (if any) is
        // superseded by this attempt either way.
        error_active = false;
        if (! validator(validator_user_data, *this, output, input)) {
            if (! error_active) {
                set_error("Connection rejected");
            }
            return false;
        }
    }
    if (input_slot_taken(input.node_idx, input.slot_idx, pool_no_slot)) {
        set_input_taken_error(*this, input);
        return false;
    }
    if (add_connection(output, input) == pool_no_slot) {
        set_error("No free connection slots");
        return false;
    }
    return true;
}

bool Sculptor::Graph::move_connection_end(uint32_t connection_idx, bool move_output_end, EndPoint new_point)
{
    if (connection_idx >= max_connections || ! connections.is_occupied(connection_idx)) {
        return false;
    }
    const Connection& connection = connections.entries[connection_idx];
    const EndPoint    new_output = move_output_end ? new_point : connection.output;
    const EndPoint    new_input  = move_output_end ? connection.input : new_point;
    const EndPoint    prev_input = connection.input;

    // Same rules as a fresh drop, but a refused retarget snaps back instead
    // of destroying the connection: no change events are pushed, so the
    // editor never sees a phantom delete and the error overlay survives
    // to render.  Only a release off any connector deletes (user intent).
    if (! endpoints_structurally_valid(new_output, new_input)) {
        set_error("Invalid connection");
        return false;
    }
    if (validator) {
        error_active       = false;
        moving_connection  = connection_idx;
        const bool allowed = validator(validator_user_data, *this, new_output, new_input);
        moving_connection  = pool_no_slot;
        if (! allowed) {
            if (! error_active) {
                set_error("Connection rejected");
            }
            return false;
        }
    }
    if (input_slot_taken(new_input.node_idx, new_input.slot_idx, connection_idx)) {
        set_input_taken_error(*this, new_input);
        return false;
    }

    Connection& mutable_connection = connections.entries[connection_idx];
    mutable_connection.output      = new_output;
    mutable_connection.input       = new_input;
    push_connection_change(ChangeKind::connection_changed, connection_idx, new_output, new_input, prev_input);
    return true;
}

void Sculptor::Graph::delete_connection(uint32_t connection_idx)
{
    if (connection_idx >= max_connections || ! connections.is_occupied(connection_idx)) {
        return;
    }
    // Deleting the connection being retargeted cancels the interaction, so a
    // later release can never touch a stale or reused pool slot.
    if (interaction == Interaction::retargeting && retarget_connection == connection_idx) {
        interaction         = Interaction::idle;
        retarget_connection = pool_no_slot;
    }
    // The endpoints are captured before the slot is freed: a drained delete
    // event must report the pair that was removed, not whatever reused slot.
    const Connection removed = connections.entries[connection_idx];
    connections.free(connection_idx);
    push_connection_change(ChangeKind::connection_deleted,
                           connection_idx,
                           removed.output,
                           removed.input,
                           { pool_no_slot, pool_no_slot });
}

void Sculptor::Graph::set_ghost(uint32_t node_idx, bool ghost)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    Node& node = nodes.entries[node_idx];
    if (node.ghost == ghost) {
        return;
    }
    node.ghost = ghost;
    if (! ghost) {
        // Placement finishes the ghost mode; cancellation deletes the node and
        // is reported as a regular node_deleted event.
        push_change(ChangeKind::ghost_placed, node_idx, pool_no_slot, pool_no_slot);
    }
}

// Selection: pure view state (see header).
bool Sculptor::Graph::is_selected(uint32_t node_idx) const
{
    return node_idx < max_nodes && nodes.is_occupied(node_idx) && selected[node_idx];
}

void Sculptor::Graph::set_selected(uint32_t node_idx, bool node_selected)
{
    if (node_idx < max_nodes && nodes.is_occupied(node_idx)) {
        selected[node_idx] = node_selected;
    }
}

void Sculptor::Graph::select_none()
{
    for (uint32_t i = 0; i < max_nodes; ++i) {
        selected[i] = false;
    }
}

void Sculptor::Graph::align_selected(AlignKind kind)
{
    // First pass: collect the reference edges over selected, non-ghost nodes.
    bool  any_selected = false;
    float min_x        = 0.0f;
    float min_y        = 0.0f;
    float max_right    = 0.0f;
    float max_bottom   = 0.0f;
    float max_width    = 0.0f;
    float max_height   = 0.0f;
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (! nodes.is_occupied(i) || ! selected[i] || nodes.entries[i].ghost) {
            continue;
        }
        const Node& node   = nodes.entries[i];
        const float width  = node.content_width_override > 0.0f ? node.content_width_override : content_sizes[i].x;
        const float height = node.content_height_override > 0.0f ? node.content_height_override : content_sizes[i].y;
        if (! any_selected) {
            min_x        = node.position.x;
            min_y        = node.position.y;
            max_right    = node.position.x + width;
            max_bottom   = node.position.y + height;
            any_selected = true;
        }
        else {
            if (node.position.x < min_x) {
                min_x = node.position.x;
            }
            if (node.position.y < min_y) {
                min_y = node.position.y;
            }
            if (node.position.x + width > max_right) {
                max_right = node.position.x + width;
            }
            if (node.position.y + height > max_bottom) {
                max_bottom = node.position.y + height;
            }
        }
        if (width > max_width) {
            max_width = width;
        }
        if (height > max_height) {
            max_height = height;
        }
    }
    if (! any_selected) {
        return;
    }

    // Second pass: apply.  Node moves are pure view state: no change events,
    // same rule as dragging.
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (! nodes.is_occupied(i) || ! selected[i] || nodes.entries[i].ghost) {
            continue;
        }
        Node&       node   = nodes.entries[i];
        const float width  = node.content_width_override > 0.0f ? node.content_width_override : content_sizes[i].x;
        const float height = node.content_height_override > 0.0f ? node.content_height_override : content_sizes[i].y;
        switch (kind) {
            case AlignKind::left:
                node.position.x = min_x;
                break;
            case AlignKind::right:
                node.position.x = max_right - width;
                break;
            case AlignKind::top:
                node.position.y = min_y;
                break;
            case AlignKind::bottom:
                node.position.y = max_bottom - height;
                break;
            case AlignKind::equal_width:
                node.content_width_override = max_width;
                break;
            case AlignKind::equal_height:
                node.content_height_override = max_height;
                break;
        }
    }
}

void Sculptor::Graph::report_content_size(uint32_t node_idx, vmath::vec2 size)
{
    if (node_idx < max_nodes && nodes.is_occupied(node_idx)) {
        content_sizes[node_idx] = size;
    }
}

vmath::vec2 Sculptor::Graph::content_size(uint32_t node_idx) const
{
    if (node_idx >= max_nodes) {
        return vmath::vec2(0.0f, 0.0f);
    }
    return content_sizes[node_idx];
}

void Sculptor::Graph::set_colors(const GraphColors& colors)
{
    colors_ = colors;
}

void Sculptor::Graph::set_node_color(uint32_t node_idx, uint32_t packed_rgba)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    Node& node = nodes.entries[node_idx];
    if (node.color_override == packed_rgba) {
        return;
    }
    node.color_override = packed_rgba;
    push_change(ChangeKind::color_changed, node_idx, pool_no_slot, pool_no_slot);
}

void Sculptor::Graph::set_state_widget(uint32_t            node_idx,
                                       StateWidgetCallback callback,
                                       void*               user_data,
                                       float               initial_height)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    nodes.entries[node_idx].state_widget      = callback;
    nodes.entries[node_idx].state_widget_data = user_data;
    if (initial_height > 0.0f) {
        state_widget_heights[node_idx] = initial_height;
    }
}

// Reports any non-idle interaction mode, including the non-mutating drop
// menu; see the declaration in sculptor_graph.h for the caller contract.
bool Sculptor::Graph::interaction_active() const
{
    return interaction != Interaction::idle;
}

const Sculptor::Node& Sculptor::Graph::node(uint32_t node_idx) const
{
    assert(node_idx < max_nodes);
    assert(nodes.is_occupied(node_idx));
    return nodes.entries[node_idx];
}

bool Sculptor::Graph::node_occupied(uint32_t node_idx) const
{
    return node_idx < max_nodes && nodes.is_occupied(node_idx);
}

void Sculptor::Graph::set_node_layout(uint32_t    node_idx,
                                      vmath::vec2 position,
                                      float       width_override,
                                      float       height_override)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx))
        return;
    Node& node                   = nodes.entries[node_idx];
    node.position                = position;
    node.content_width_override  = width_override;
    node.content_height_override = height_override;
}

const Sculptor::Connection& Sculptor::Graph::get_connection(uint32_t connection_idx) const
{
    assert(connection_idx < max_connections);
    assert(connections.is_occupied(connection_idx));
    return connections.entries[connection_idx];
}

const Sculptor::GraphColors& Sculptor::Graph::colors() const
{
    return colors_;
}

uint32_t Sculptor::Graph::connection_count() const
{
    return connections.num_allocated;
}

bool Sculptor::Graph::connection_occupied(uint32_t connection_idx) const
{
    assert(connection_idx < max_connections);
    return connections.is_occupied(connection_idx);
}

bool Sculptor::Graph::has_error() const
{
    return error_active;
}

uint32_t Sculptor::Graph::connection_popup() const
{
    return popup_connection;
}

vmath::vec2 Sculptor::Graph::canvas_popup_pos() const
{
    return popup_canvas_pos;
}

Sculptor::EndPoint Sculptor::Graph::drop_menu_anchor() const
{
    return drop_menu_from;
}

vmath::vec2 Sculptor::Graph::drop_menu_pos() const
{
    return drop_menu_point;
}

const char* Sculptor::Graph::error_text() const
{
    return error_message;
}

void Sculptor::Graph::dismiss_error()
{
    error_active = false;
}

// The live connection terminating at an input connector, or pool_no_slot.
uint32_t Sculptor::connection_into(const Graph& graph, uint32_t node_idx, uint32_t slot_idx)
{
    for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
        if (! graph.connection_occupied(c)) {
            continue;
        }
        const Sculptor::Connection& connection = graph.get_connection(c);
        if (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx) {
            return c;
        }
    }
    return Sculptor::pool_no_slot;
}
