// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Graph widget data model: pools, change events, colors, persistence stubs.
// Deliberately free of ImGui calls so the unit test can link this file;
// rendering and interaction live in sculptor_graph_render.cpp.

#include "sculptor_graph.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace Sculptor {

GraphColors default_graph_colors()
{
    GraphColors colors = {};
    colors.node_background          = 0x2B2B2BFFu;
    colors.node_border              = 0x5A5A5AFFu;
    colors.node_selected_border     = 0xFFFFFFFFu;
    colors.node_title               = 0xE0E0E0FFu;
    colors.grid_line                = 0x353535FFu;
    colors.grid_axis                = 0x404040FFu;
    colors.connector                = 0x909090FFu;
    colors.connector_hover          = 0xFFFFFFE0u;
    colors.connector_connected      = 0xE0E0E0FFu;
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

// Packed 0xRRGGBBAA, one place inside the widget; callers may override the
// whole set via set_colors().
// Bounds-checked append for save().  Returns false when the buffer is too
// small; the caller treats that as a 0 return from save().
bool append_bytes(uint8_t* buffer, uint32_t buffer_size, uint32_t& off,
                  const void* src, uint32_t n)
{
    if (off + n > buffer_size) {
        return false;
    }
    memcpy(buffer + off, src, n);
    off += n;
    return true;
}

// Bounds-checked read for load().
bool read_bytes(const uint8_t* buffer, uint32_t buffer_size, uint32_t& off,
                void* dst, uint32_t n)
{
    if (off + n > buffer_size) {
        return false;
    }
    memcpy(dst, buffer + off, n);
    off += n;
    return true;
}

struct SnapshotSlot {
    bool present;
    Slot slot;
};

struct SnapshotNode {
    bool present;
    char name[64];
    vmath::vec2 position;
    uint32_t color_override;
    float content_width_override;
    float content_height_override;
    bool ghost;
    bool has_state_widget;
    SnapshotSlot slots[max_node_slots];
};

struct SnapshotConnection {
    bool present;
    EndPoint output;
    EndPoint input;
};

struct Snapshot {
    SnapshotNode nodes[max_nodes];
    SnapshotConnection connections[max_connections];
    vmath::vec2 view_origin;
    float zoom;
    GraphColors colors;
    uint32_t caller_state_size;
};

// Parses and validates the graph sections of a snapshot (everything except
// the caller tail).  Returns false on bad version, truncation or any
// malformed field, so load() can bail before mutating live state.
bool parse_snapshot(const uint8_t* buffer, uint32_t buffer_size, uint32_t& off,
                    Snapshot& snapshot)
{
    uint16_t version = 0;
    if ( ! read_bytes(buffer, buffer_size, off, &version, 2) || version != 1) {
        return false;
    }

    uint32_t node_count = 0;
    if ( ! read_bytes(buffer, buffer_size, off, &node_count, 4) || node_count > max_nodes) {
        return false;
    }
    for (uint32_t i = 0; i < node_count; ++i) {
        uint16_t node_idx = 0;
        if ( ! read_bytes(buffer, buffer_size, off, &node_idx, 2) ||
            node_idx >= max_nodes || snapshot.nodes[node_idx].present) {
            return false;
        }
        SnapshotNode& node = snapshot.nodes[node_idx];
        node.present = true;
        if ( ! read_bytes(buffer, buffer_size, off, node.name, sizeof(node.name)) ||
            ! read_bytes(buffer, buffer_size, off, &node.position, sizeof(node.position)) ||
            ! read_bytes(buffer, buffer_size, off, &node.color_override, 4) ||
            ! read_bytes(buffer, buffer_size, off, &node.content_width_override, 4) ||
            ! read_bytes(buffer, buffer_size, off, &node.content_height_override, 4)) {
            return false;
        }
        uint8_t ghost       = 0;
        uint8_t has_widget  = 0;
        uint16_t slot_count = 0;
        if ( ! read_bytes(buffer, buffer_size, off, &ghost, 1) ||
            ! read_bytes(buffer, buffer_size, off, &has_widget, 1) ||
            ! read_bytes(buffer, buffer_size, off, &slot_count, 2) ||
            slot_count > max_node_slots) {
            return false;
        }
        node.ghost           = ghost != 0;
        node.has_state_widget = has_widget != 0;
        for (uint32_t s = 0; s < slot_count; ++s) {
            uint16_t slot_idx = 0;
            if ( ! read_bytes(buffer, buffer_size, off, &slot_idx, 2) ||
                slot_idx >= max_node_slots || node.slots[slot_idx].present) {
                return false;
            }
            SnapshotSlot& slot = node.slots[slot_idx];
            slot.present       = true;
            uint8_t kind          = 0;
            uint8_t connectable   = 0;
            uint8_t property_type = 0;
            uint8_t num_options   = 0;
            if ( ! read_bytes(buffer, buffer_size, off, slot.slot.name, sizeof(slot.slot.name)) ||
                ! read_bytes(buffer, buffer_size, off, &kind, 1) ||
                kind < static_cast<uint8_t>(SlotKind::input) ||
                kind > static_cast<uint8_t>(SlotKind::property) ||
                ! read_bytes(buffer, buffer_size, off, &connectable, 1) ||
                ! read_bytes(buffer, buffer_size, off, &property_type, 1) ||
                property_type > static_cast<uint8_t>(PropertyType::list) ||
                ! read_bytes(buffer, buffer_size, off, &slot.slot.value, sizeof(slot.slot.value)) ||
                ! read_bytes(buffer, buffer_size, off, &num_options, 1) ||
                num_options > 8) {
                return false;
            }
            slot.slot.kind             = static_cast<SlotKind>(kind);
            slot.slot.connectable      = connectable != 0;
            slot.slot.property_type    = static_cast<PropertyType>(property_type);
            slot.slot.num_list_options = num_options;
            for (uint32_t o = 0; o < num_options; ++o) {
                if ( ! read_bytes(buffer, buffer_size, off, slot.slot.list_options[o],
                                  sizeof(slot.slot.list_options[o]))) {
                    return false;
                }
            }
        }
    }

    uint32_t conn_count = 0;
    if ( ! read_bytes(buffer, buffer_size, off, &conn_count, 4) || conn_count > max_connections) {
        return false;
    }
    for (uint32_t i = 0; i < conn_count; ++i) {
        uint16_t conn_idx = 0;
        if ( ! read_bytes(buffer, buffer_size, off, &conn_idx, 2) ||
            conn_idx >= max_connections || snapshot.connections[conn_idx].present) {
            return false;
        }
        SnapshotConnection& connection = snapshot.connections[conn_idx];
        connection.present             = true;
        if ( ! read_bytes(buffer, buffer_size, off, &connection.output.node_idx, 4) ||
            ! read_bytes(buffer, buffer_size, off, &connection.output.slot_idx, 4) ||
            ! read_bytes(buffer, buffer_size, off, &connection.input.node_idx, 4) ||
            ! read_bytes(buffer, buffer_size, off, &connection.input.slot_idx, 4)) {
            return false;
        }
        // Endpoints must reference present snapshot nodes/slots and satisfy the
        // kind contract. load() places connections directly at snapshot indices,
        // bypassing add_connection, so parse_snapshot re-checks everything a
        // snapshot connection needs before it can exist in a live graph.
        if (connection.output.node_idx >= max_nodes ||
            connection.input.node_idx >= max_nodes ||
            ! snapshot.nodes[connection.output.node_idx].present ||
            ! snapshot.nodes[connection.input.node_idx].present ||
            connection.output.slot_idx >= max_node_slots ||
            connection.input.slot_idx >= max_node_slots ||
            ! snapshot.nodes[connection.output.node_idx].slots[connection.output.slot_idx].present ||
            ! snapshot.nodes[connection.input.node_idx].slots[connection.input.slot_idx].present) {
            return false;
        }
        const Slot& out_slot =
            snapshot.nodes[connection.output.node_idx].slots[connection.output.slot_idx].slot;
        const Slot& in_slot =
            snapshot.nodes[connection.input.node_idx].slots[connection.input.slot_idx].slot;
        if (out_slot.kind != SlotKind::output ||
            ! (in_slot.kind == SlotKind::input ||
               (in_slot.kind == SlotKind::property && in_slot.connectable))) {
            return false;
        }
    }

    if ( ! read_bytes(buffer, buffer_size, off, &snapshot.view_origin, sizeof(snapshot.view_origin)) ||
        ! read_bytes(buffer, buffer_size, off, &snapshot.zoom, 4) ||
        ! read_bytes(buffer, buffer_size, off, &snapshot.colors, sizeof(snapshot.colors))) {
        return false;
    }

    if ( ! read_bytes(buffer, buffer_size, off, &snapshot.caller_state_size, 4) ||
        snapshot.caller_state_size > buffer_size - off) {
        return false;
    }
    return true;
}

// Kind/type/options identity: value and name are diffed separately.
bool slot_structure_equal(const Slot& a, const Slot& b)
{
    if (a.kind != b.kind || a.connectable != b.connectable ||
        a.property_type != b.property_type || a.num_list_options != b.num_list_options) {
        return false;
    }
    for (uint32_t i = 0; i < a.num_list_options; ++i) {
        if (strncmp(a.list_options[i], b.list_options[i], sizeof(a.list_options[i])) != 0) {
            return false;
        }
    }
    return true;
}

// Maps a snapshot connection endpoint onto live pool indices.  Slots are
// always placed at their exact snapshot index (allocate_at), so only the
// node index needs remapping through the name match.
void remap_snapshot_connection(const int32_t* snap_to_live,
                               const SnapshotConnection& connection, EndPoint& out, EndPoint& in)
{
    out.node_idx = static_cast<uint32_t>(snap_to_live[connection.output.node_idx]);
    out.slot_idx = connection.output.slot_idx;
    in.node_idx  = static_cast<uint32_t>(snap_to_live[connection.input.node_idx]);
    in.slot_idx  = connection.input.slot_idx;
}
}  // namespace

void Graph::push_change(ChangeKind kind, uint32_t node_idx, uint32_t slot_idx, uint32_t connection_idx)
{
    if (changes_count == max_pending_changes) {
        // Caller did not drain fast enough: force a resync instead of losing
        // events silently.
        changes_overflowed_flag = true;
        return;
    }
    GraphChange& change = changes[(changes_head + changes_count) % max_pending_changes];
    change.kind          = kind;
    change.node_idx      = node_idx;
    change.slot_idx      = slot_idx;
    change.connection_idx = connection_idx;
    ++changes_count;
}

uint32_t Graph::take_changes(GraphChange* out, uint32_t out_size)
{
    const uint32_t count = out_size < changes_count ? out_size : changes_count;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = changes[(changes_head + i) % max_pending_changes];
    }
    changes_head = (changes_head + count) % max_pending_changes;
    changes_count -= count;
    return count;
}

bool Graph::changes_overflowed()
{
    const bool overflowed = changes_overflowed_flag;
    changes_overflowed_flag = false;
    return overflowed;
}

bool Graph::slot_is_connected(uint32_t node_idx, uint32_t slot_idx) const
{
    for (uint32_t i = 0; i < max_connections; ++i) {
        if ( ! connections.is_occupied(i)) {
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

uint32_t Graph::create_node(const char* name, vmath::vec2 position)
{
    const uint32_t node_idx = nodes.allocate();
    if (node_idx == pool_no_slot) {
        return pool_no_slot;
    }
        Node& new_node = nodes.entries[node_idx];
    new_node   = Node();
    // Defensive: parallel per-slot arrays must not leak state into a reused
    // slot (delete_node clears them too; this covers slots freed elsewhere).
    selected[node_idx]      = false;
    content_sizes[node_idx] = vmath::vec2(0.0f, 0.0f);
    snprintf(new_node.name, sizeof(new_node.name), "%s", name ? name : "");
    // Snap the initial position so new nodes align with the grid.
    new_node.position = vmath::vec2(floor(position.x / graph_grid_spacing) * graph_grid_spacing,
                                    floor(position.y / graph_grid_spacing) * graph_grid_spacing);
    push_change(ChangeKind::node_added, node_idx, pool_no_slot, pool_no_slot);
    return node_idx;
}

void Graph::delete_node(uint32_t node_idx)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    // Drop connections touching the node first, reporting each one, so the
    // caller never sees a connection referencing a dead node.  delete_connection
    // also cancels an active retarget of a dropped connection.
    for (uint32_t i = 0; i < max_connections; ++i) {
        if ( ! connections.is_occupied(i)) {
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
    nodes.free(node_idx);
    selected[node_idx]      = false;
    content_sizes[node_idx] = vmath::vec2(0.0f, 0.0f);
    if (dragged_node == node_idx) {
        dragged_node = pool_no_slot;
        interaction  = Interaction::idle;
    }
    if (renaming_node == node_idx) {
        renaming_node = pool_no_slot;
        interaction   = Interaction::idle;
    }
    push_change(ChangeKind::node_deleted, node_idx, pool_no_slot, pool_no_slot);
}

// Removes a single slot, dropping connections that reference it first
// (same hygiene as delete_node).  Used by callers rebuilding a node's
// slot set incrementally.
void Graph::remove_slot(uint32_t node_idx, uint32_t slot_idx)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx) ||
        slot_idx >= max_node_slots || ! nodes.entries[node_idx].slots.is_occupied(slot_idx)) {
        return;
    }
    for (uint32_t i = 0; i < max_connections; ++i) {
        if ( ! connections.is_occupied(i)) {
            continue;
        }
        const Connection& connection = connections.entries[i];
        if ((connection.output.node_idx == node_idx && connection.output.slot_idx == slot_idx) ||
            (connection.input.node_idx == node_idx && connection.input.slot_idx == slot_idx)) {
            delete_connection(i);
        }
    }
    nodes.entries[node_idx].slots.free(slot_idx);
    push_change(ChangeKind::slot_deleted, node_idx, slot_idx, pool_no_slot);
}

uint32_t Graph::add_slot(uint32_t node_idx, const Slot& slot)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return pool_no_slot;
    }
    Node& node = nodes.entries[node_idx];
    const uint32_t slot_idx = node.slots.allocate();
    if (slot_idx == pool_no_slot) {
        return pool_no_slot;
    }
    node.slots.entries[slot_idx] = slot;
    node.slots.entries[slot_idx].name[sizeof(node.slots.entries[slot_idx].name) - 1] = '\0';
    push_change(ChangeKind::slot_added, node_idx, slot_idx, pool_no_slot);
    return slot_idx;
}

bool Graph::endpoints_structurally_valid(EndPoint output, EndPoint input) const
{
    if (output.node_idx >= max_nodes || input.node_idx >= max_nodes ||
        output.slot_idx >= max_node_slots || input.slot_idx >= max_node_slots) {
        return false;
    }
    if ( ! nodes.is_occupied(output.node_idx) || ! nodes.is_occupied(input.node_idx)) {
        return false;
    }
    if ( ! nodes.entries[output.node_idx].slots.is_occupied(output.slot_idx) ||
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
    if (input_slot.kind != SlotKind::input &&
        ! (input_slot.kind == SlotKind::property && input_slot.connectable)) {
        return false;
    }
    return true;
}

bool Graph::input_slot_taken(uint32_t node_idx, uint32_t slot_idx,
                             uint32_t except_connection) const
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

uint32_t Graph::add_connection(EndPoint output, EndPoint input)
{
    if ( ! endpoints_structurally_valid(output, input)) {
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
    connection.output = output;
    connection.input  = input;
    push_change(ChangeKind::connection_added, pool_no_slot, pool_no_slot, connection_idx);
    return connection_idx;
}

void Graph::set_validator(ValidationCallback callback, void* user_data)
{
    validator         = callback;
    validator_user_data = user_data;
}

void Graph::set_error(const char* message)
{
    snprintf(error_message, sizeof error_message, "%s", message);
    error_active = true;
}

bool Graph::attempt_connection(EndPoint output, EndPoint input)
{
    if ( ! endpoints_structurally_valid(output, input)) {
        set_error("Invalid connection");
        return false;
    }
    if (validator && ! validator(validator_user_data, *this, output, input)) {
        set_error("Connection rejected");
        return false;
    }
    if (input_slot_taken(input.node_idx, input.slot_idx, pool_no_slot)) {
        set_error("Input already connected");
        return false;
    }
    if (add_connection(output, input) == pool_no_slot) {
        set_error("No free connection slots");
        return false;
    }
    return true;
}

bool Graph::move_connection_end(uint32_t connection_idx, bool move_output_end,
                                EndPoint new_point)
{
    if (connection_idx >= max_connections || ! connections.is_occupied(connection_idx)) {
        return false;
    }
    const Connection& connection = connections.entries[connection_idx];
    const EndPoint new_output    = move_output_end ? new_point : connection.output;
    const EndPoint new_input     = move_output_end ? connection.input : new_point;

    // Same rules as a fresh drop; a failed retarget destroys the connection.
    if ( ! endpoints_structurally_valid(new_output, new_input)) {
        delete_connection(connection_idx);
        set_error("Invalid connection");
        return false;
    }
    if (validator && ! validator(validator_user_data, *this, new_output, new_input)) {
        delete_connection(connection_idx);
        set_error("Connection rejected");
        return false;
    }
    if (input_slot_taken(new_input.node_idx, new_input.slot_idx, connection_idx)) {
        delete_connection(connection_idx);
        set_error("Input already connected");
        return false;
    }

    Connection& mutable_connection = connections.entries[connection_idx];
    mutable_connection.output = new_output;
    mutable_connection.input  = new_input;
    push_change(ChangeKind::connection_changed, pool_no_slot, pool_no_slot, connection_idx);
    return true;
}

void Graph::delete_connection(uint32_t connection_idx)
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
    connections.free(connection_idx);
    push_change(ChangeKind::connection_deleted, pool_no_slot, pool_no_slot, connection_idx);
}

void Graph::set_ghost(uint32_t node_idx, bool ghost)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    Node& node = nodes.entries[node_idx];
    if (node.ghost == ghost) {
        return;
    }
    node.ghost = ghost;
    if ( ! ghost) {
        // Placement finishes the ghost mode; cancellation deletes the node and
        // is reported as a regular node_deleted event.
        push_change(ChangeKind::ghost_placed, node_idx, pool_no_slot, pool_no_slot);
    }
}

// Selection: pure view state (see header).
bool Graph::is_selected(uint32_t node_idx) const
{
    return node_idx < max_nodes && nodes.is_occupied(node_idx) && selected[node_idx];
}

void Graph::set_selected(uint32_t node_idx, bool node_selected)
{
    if (node_idx < max_nodes && nodes.is_occupied(node_idx)) {
        selected[node_idx] = node_selected;
    }
}

void Graph::select_none()
{
    for (uint32_t i = 0; i < max_nodes; ++i) {
        selected[i] = false;
    }
}

void Graph::align_selected(AlignKind kind)
{
    // First pass: collect the reference edges over selected, non-ghost nodes.
    bool any_selected = false;
    float min_x       = 0.0f;
    float min_y       = 0.0f;
    float max_right   = 0.0f;
    float max_bottom  = 0.0f;
    float max_width   = 0.0f;
    float max_height  = 0.0f;
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if ( ! nodes.is_occupied(i) || ! selected[i] || nodes.entries[i].ghost) {
            continue;
        }
        const Node& node  = nodes.entries[i];
        const float width = node.content_width_override > 0.0f
                          ? node.content_width_override : content_sizes[i].x;
        const float height = node.content_height_override > 0.0f
                           ? node.content_height_override : content_sizes[i].y;
        if ( ! any_selected) {
            min_x      = node.position.x;
            min_y      = node.position.y;
            max_right  = node.position.x + width;
            max_bottom = node.position.y + height;
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
    if ( ! any_selected) {
        return;
    }

    // Second pass: apply.  Node moves are pure view state: no change events,
    // same rule as dragging.
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if ( ! nodes.is_occupied(i) || ! selected[i] || nodes.entries[i].ghost) {
            continue;
        }
        Node& node        = nodes.entries[i];
        const float width = node.content_width_override > 0.0f
                          ? node.content_width_override : content_sizes[i].x;
        const float height = node.content_height_override > 0.0f
                           ? node.content_height_override : content_sizes[i].y;
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

void Graph::report_content_size(uint32_t node_idx, vmath::vec2 size)
{
    if (node_idx < max_nodes && nodes.is_occupied(node_idx)) {
        content_sizes[node_idx] = size;
    }
}

vmath::vec2 Graph::content_size(uint32_t node_idx) const
{
    if (node_idx >= max_nodes) {
        return vmath::vec2(0.0f, 0.0f);
    }
    return content_sizes[node_idx];
}

void Graph::set_colors(const GraphColors& colors)
{
    colors_ = colors;
}

void Graph::set_node_color(uint32_t node_idx, uint32_t packed_rgba)
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

void Graph::set_state_widget(uint32_t node_idx, StateWidgetCallback callback, void* user_data)
{
    if (node_idx >= max_nodes || ! nodes.is_occupied(node_idx)) {
        return;
    }
    nodes.entries[node_idx].state_widget      = callback;
    nodes.entries[node_idx].state_widget_data = user_data;
}

void Graph::set_state_callbacks(SerializeState serialize, DeserializeState deserialize, void* user_data)
{
    serialize_state   = serialize;
    deserialize_state = deserialize;
    state_user_data   = user_data;
}

// Writes a tightly packed, versioned snapshot (see .pi/develop/m4/plan.md).
// Returns total bytes, or 0 when buffer_size is insufficient.  The serialize
// hook, if installed, is called last and its output is prefixed with a u32
// length so load() can report bytes_consumed covering the caller tail.
uint32_t Graph::save(uint8_t* buffer, uint32_t buffer_size) const
{
    if (!buffer || buffer_size < 2) {
        return 0;
    }

    uint32_t off = 0;

    // 1. Version
    const uint16_t version = 1;
    if (!append_bytes(buffer, buffer_size, off, &version, 2)) {
        return 0;
    }

    // 2. Node pool: count, then per occupied slot in index order.
    const uint32_t node_count = nodes.num_allocated;
    if (!append_bytes(buffer, buffer_size, off, &node_count, 4)) {
        return 0;
    }

    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (!nodes.is_occupied(i)) {
            continue;
        }

        const Node& n           = nodes.entries[i];
        const uint16_t node_idx = static_cast<uint16_t>(i);

        if (!append_bytes(buffer, buffer_size, off, &node_idx, 2) ||
            !append_bytes(buffer, buffer_size, off, n.name, sizeof(n.name)) ||
            !append_bytes(buffer, buffer_size, off, &n.position, sizeof(n.position)) ||
            !append_bytes(buffer, buffer_size, off, &n.color_override, 4) ||
            !append_bytes(buffer, buffer_size, off, &n.content_width_override, 4) ||
            !append_bytes(buffer, buffer_size, off, &n.content_height_override, 4)) {
            return 0;
        }

        const uint8_t ghost       = n.ghost ? 1 : 0;
        const uint8_t has_widget  = n.state_widget ? 1 : 0;
        const uint16_t slot_count = static_cast<uint16_t>(n.slots.num_allocated);

        if (!append_bytes(buffer, buffer_size, off, &ghost, 1) ||
            !append_bytes(buffer, buffer_size, off, &has_widget, 1) ||
            !append_bytes(buffer, buffer_size, off, &slot_count, 2)) {
            return 0;
        }

        for (uint32_t s = 0; s < max_node_slots; ++s) {
            if (!n.slots.is_occupied(s)) {
                continue;
            }

            const Slot& slot      = n.slots.entries[s];
            const uint16_t sidx   = static_cast<uint16_t>(s);
            const uint8_t   kind  = static_cast<uint8_t>(slot.kind);
            const uint8_t   conn  = slot.connectable ? 1 : 0;
            const uint8_t   ptype = static_cast<uint8_t>(slot.property_type);

            // add_slot() does not validate this, and iterating it would read
            // past list_options[8]; refuse to serialize such a slot.
            if (slot.num_list_options > 8) {
                return 0;
            }

            if (!append_bytes(buffer, buffer_size, off, &sidx, 2) ||
                !append_bytes(buffer, buffer_size, off, slot.name, sizeof(slot.name)) ||
                !append_bytes(buffer, buffer_size, off, &kind, 1) ||
                !append_bytes(buffer, buffer_size, off, &conn, 1) ||
                !append_bytes(buffer, buffer_size, off, &ptype, 1) ||
                !append_bytes(buffer, buffer_size, off, &slot.value, sizeof(slot.value)) ||
                !append_bytes(buffer, buffer_size, off, &slot.num_list_options, 1)) {
                return 0;
            }

            for (uint32_t o = 0; o < slot.num_list_options; ++o) {
                if (!append_bytes(buffer, buffer_size, off,
                                  slot.list_options[o],
                                  sizeof(slot.list_options[o]))) {
                    return 0;
                }
            }
        }
    }

    // 3. Connection pool: count, then per occupied slot in index order.
    const uint32_t conn_count = connections.num_allocated;
    if (!append_bytes(buffer, buffer_size, off, &conn_count, 4)) {
        return 0;
    }

    for (uint32_t i = 0; i < max_connections; ++i) {
        if (!connections.is_occupied(i)) {
            continue;
        }

        const Connection& c     = connections.entries[i];
        const uint16_t conn_idx = static_cast<uint16_t>(i);

        if (!append_bytes(buffer, buffer_size, off, &conn_idx, 2) ||
            !append_bytes(buffer, buffer_size, off, &c.output.node_idx, 4) ||
            !append_bytes(buffer, buffer_size, off, &c.output.slot_idx, 4) ||
            !append_bytes(buffer, buffer_size, off, &c.input.node_idx, 4) ||
            !append_bytes(buffer, buffer_size, off, &c.input.slot_idx, 4)) {
            return 0;
        }
    }

    // 4. View state
    if (!append_bytes(buffer, buffer_size, off, &view_origin, sizeof(view_origin)) ||
        !append_bytes(buffer, buffer_size, off, &zoom, 4)) {
        return 0;
    }

    // 5. Colors (17 u32s, all fields of GraphColors, no padding)
    if (!append_bytes(buffer, buffer_size, off, &colors_, sizeof(colors_))) {
        return 0;
    }

    // 6. Caller state: u32 length prefix, then hook bytes.  Write a zero
    // placeholder, call the hook into the remaining space, then patch.
    const uint32_t size_field_off = off;
    const uint32_t zero           = 0;
    if (!append_bytes(buffer, buffer_size, off, &zero, 4)) {
        return 0;
    }

    if (serialize_state) {
        const uint32_t remaining = buffer_size - off;
        const uint32_t hook_bytes = serialize_state(state_user_data,
                                                    buffer + off, remaining);
        if (hook_bytes > remaining) {  // no wrap: off <= buffer_size
            return 0;
        }
        memcpy(buffer + size_field_off, &hook_bytes, 4);
        off += hook_bytes;
    }

    return off;
}

// Applies a snapshot DIFFERENTIALLY (see .pi/develop/m4/plan.md): deletions
// first (connections, then nodes), then additions, then in-place updates, so
// a live synth only sees surgical GraphChange events instead of a rebuild.
// Node identity is the name: pool indices churn across deletions, so
// connection endpoints are remapped through the name match.  Nothing is
// mutated until the whole snapshot has parsed and validated.
bool Graph::load(const uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_consumed)
{
    if (bytes_consumed) {
        *bytes_consumed = 0;
    }
    if ( ! buffer || buffer_size < 2) {
        return false;
    }

    // Parse/apply scratch.  Function-local static: BSS only, and the widget
    // is single-threaded like the rest of ImGui.
    static Snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    uint32_t off = 0;
    if ( ! parse_snapshot(buffer, buffer_size, off, snapshot)) {
        return false;
    }

    // Optional deserialize hook runs before any mutation, so a rejected tail
    // still leaves the live graph untouched.
    if (deserialize_state &&
        ! deserialize_state(state_user_data, buffer + off, snapshot.caller_state_size)) {
        return false;
    }
    const uint32_t consumed = off + snapshot.caller_state_size;

    // Match snapshot nodes to live nodes by name.
    int32_t snap_to_live[max_nodes];  // snapshot node idx -> live node idx
    bool    snap_was_matched[max_nodes];
    bool    snap_slots_recreated[max_nodes];
    bool    live_matched[max_nodes];
    for (uint32_t s = 0; s < max_nodes; ++s) {
        snap_to_live[s]        = -1;
        snap_was_matched[s]    = false;
        snap_slots_recreated[s] = false;
    }
    for (uint32_t i = 0; i < max_nodes; ++i) {
        live_matched[i] = false;
    }
    for (uint32_t s = 0; s < max_nodes; ++s) {
        if ( ! snapshot.nodes[s].present) {
            continue;
        }
        for (uint32_t i = 0; i < max_nodes; ++i) {
            if ( ! nodes.is_occupied(i) || live_matched[i]) {
                continue;
            }
            if (strncmp(nodes.entries[i].name, snapshot.nodes[s].name,
                        sizeof(nodes.entries[i].name)) == 0) {
                snap_to_live[s]     = static_cast<int32_t>(i);
                snap_was_matched[s] = true;
                live_matched[i]     = true;
                break;
            }
        }
    }

    // A matched node whose slot index set changed is recreated wholesale:
    // per-slot in-place updates are only sound when the index sets agree.
    for (uint32_t s = 0; s < max_nodes; ++s) {
        if ( ! snap_was_matched[s]) {
            continue;
        }
        const Node& node = nodes.entries[snap_to_live[s]];
        for (uint32_t t = 0; t < max_node_slots; ++t) {
            if (node.slots.is_occupied(t) != snapshot.nodes[s].slots[t].present) {
                snap_slots_recreated[s] = true;
                break;
            }
        }
    }

    // A live connection survives only when the snapshot has one on the same
    // pool slot whose endpoint nodes all survive (are name-matched).  A
    // surviving connection may still be retargeted in the final phase if its
    // remapped endpoints differ (connection_changed).
    bool conn_survives[max_connections] = {};
    for (uint32_t j = 0; j < max_connections; ++j) {
        if ( ! connections.is_occupied(j) || ! snapshot.connections[j].present) {
            continue;
        }
        const SnapshotConnection& sc = snapshot.connections[j];
        conn_survives[j] = snap_was_matched[sc.output.node_idx] &&
                           snap_was_matched[sc.input.node_idx];
    }

    // 1. Deletions: connections first (delete_node would drop them anyway,
    // but emitting them here keeps the pinned event order explicit).
    for (uint32_t j = 0; j < max_connections; ++j) {
        if (connections.is_occupied(j) && ! conn_survives[j]) {
            delete_connection(j);
        }
    }
    for (uint32_t i = 0; i < max_nodes; ++i) {
        if (nodes.is_occupied(i) && ! live_matched[i]) {
            delete_node(i);
        }
    }

    // 2. Additions: new nodes (with slots), positions and other silent state
    // applied directly - node_added/slot_added already tell the caller.  Slots
    // land at their exact snapshot indices so connection endpoints remap 1:1.
    for (uint32_t s = 0; s < max_nodes; ++s) {
        if ( ! snapshot.nodes[s].present || snap_was_matched[s]) {
            continue;
        }
        const SnapshotNode& sn  = snapshot.nodes[s];
        const uint32_t live_idx = create_node(sn.name, sn.position);
        if (live_idx == pool_no_slot) {
            return false; // unreachable: deletions freed enough pool slots
        }
        snap_to_live[s] = static_cast<int32_t>(live_idx);
        Node& node = nodes.entries[live_idx];
        node.position = sn.position;
        node.color_override = sn.color_override;
        node.content_width_override = sn.content_width_override;
        node.content_height_override = sn.content_height_override;
        node.ghost = sn.ghost;
        for (uint32_t t = 0; t < max_node_slots; ++t) {
            if ( ! sn.slots[t].present) {
                continue;
            }
            if (node.slots.allocate_at(t) == pool_no_slot) {
                return false; // unreachable: fresh node, snapshot indices unique
            }
            node.slots.entries[t] = sn.slots[t].slot;
            node.slots.entries[t].name[sizeof(node.slots.entries[t].name) - 1] = '\0';
            push_change(ChangeKind::slot_added, live_idx, t, pool_no_slot);
        }
    }

    // 3. Slot recreation on matched nodes, BEFORE connection additions: the
    // snapshot was validated against the snapshot's slot structure, so live
    // slots must match it before anything validates against them.  Per-slot
    // structural diffs are replaced in place - allocate() is first-fit and
    // would not reliably return a sparse index.  Index-set changes rebuild at
    // exact snapshot indices.
    for (uint32_t s = 0; s < max_nodes; ++s) {
        if ( ! snapshot.nodes[s].present || ! snap_was_matched[s]) {
            continue;
        }
        const SnapshotNode& sn = snapshot.nodes[s];
        const uint32_t i       = static_cast<uint32_t>(snap_to_live[s]);
        Node& node = nodes.entries[i];
        if (snap_slots_recreated[s]) {
            for (uint32_t t = 0; t < max_node_slots; ++t) {
                if (node.slots.is_occupied(t)) {
                    node.slots.free(t);
                    push_change(ChangeKind::slot_deleted, i, t, pool_no_slot);
                }
            }
            for (uint32_t t = 0; t < max_node_slots; ++t) {
                if ( ! sn.slots[t].present) {
                    continue;
                }
                if (node.slots.allocate_at(t) == pool_no_slot) {
                    return false; // unreachable: all slots freed above
                }
                node.slots.entries[t] = sn.slots[t].slot;
                node.slots.entries[t].name[sizeof(node.slots.entries[t].name) - 1] = '\0';
                push_change(ChangeKind::slot_added, i, t, pool_no_slot);
            }
        }
        else {
            for (uint32_t t = 0; t < max_node_slots; ++t) {
                if ( ! sn.slots[t].present) {
                    continue;
                }
                Slot& live_slot = node.slots.entries[t];
                if ( ! slot_structure_equal(live_slot, sn.slots[t].slot)) {
                    live_slot = sn.slots[t].slot;
                    live_slot.name[sizeof(live_slot.name) - 1] = '\0';
                    push_change(ChangeKind::slot_deleted, i, t, pool_no_slot);
                    push_change(ChangeKind::slot_added, i, t, pool_no_slot);
                }
            }
        }
    }

    // 4. Connection additions at exact snapshot indices.  Survivors keep their
    // pool slot and are retargeted in phase 6, after node updates, so
    // connection_changed is emitted last per the pinned event order.
    for (uint32_t j = 0; j < max_connections; ++j) {
        if ( ! snapshot.connections[j].present || connections.is_occupied(j)) {
            continue;
        }
        EndPoint out;
        EndPoint in;
        remap_snapshot_connection(snap_to_live, snapshot.connections[j], out, in);
        if (connections.allocate_at(j) == pool_no_slot) {
            return false; // unreachable: survivors are a subset of the snapshot
        }
        connections.entries[j].output = out;
        connections.entries[j].input = in;
        push_change(ChangeKind::connection_added, pool_no_slot, pool_no_slot, j);
    }

    // 5. In-place updates on matched nodes.
    for (uint32_t s = 0; s < max_nodes; ++s) {
        if ( ! snapshot.nodes[s].present || ! snap_was_matched[s]) {
            continue;
        }
        const SnapshotNode& sn = snapshot.nodes[s];
        const uint32_t i       = static_cast<uint32_t>(snap_to_live[s]);
        Node& node = nodes.entries[i];
        // Silent: positions/overrides/ghost push no events (drag rule).
        node.position = sn.position;
        node.content_width_override = sn.content_width_override;
        node.content_height_override = sn.content_height_override;
        node.ghost = sn.ghost;
        if (node.color_override != sn.color_override) {
            node.color_override = sn.color_override;
            push_change(ChangeKind::color_changed, i, pool_no_slot, pool_no_slot);
        }
        if ( ! snap_slots_recreated[s]) {
            for (uint32_t t = 0; t < max_node_slots; ++t) {
                if ( ! sn.slots[t].present) {
                    continue;
                }
                Slot& live_slot = node.slots.entries[t];
                const Slot& snap_slot = sn.slots[t].slot;
                if (memcmp(&live_slot.value, &snap_slot.value, sizeof(snap_slot.value)) != 0) {
                    live_slot.value = snap_slot.value;
                    push_change(ChangeKind::value_changed, i, t, pool_no_slot);
                }
                if (strncmp(live_slot.name, snap_slot.name, sizeof(live_slot.name)) != 0) {
                    memcpy(live_slot.name, snap_slot.name, sizeof(live_slot.name));
                    push_change(ChangeKind::name_changed, i, t, pool_no_slot);
                }
            }
        }
    }

    // 6. Retarget surviving connections whose remapped endpoints differ.
    // Direct pool mutation plus one connection_changed event, mirroring
    // set_connection_endpoint().
    for (uint32_t j = 0; j < max_connections; ++j) {
        if ( ! conn_survives[j]) {
            continue;
        }
        EndPoint out;
        EndPoint in;
        remap_snapshot_connection(snap_to_live, snapshot.connections[j], out, in);
        Connection& connection = connections.entries[j];
        if (connection.output.node_idx != out.node_idx ||
            connection.output.slot_idx != out.slot_idx ||
            connection.input.node_idx != in.node_idx ||
            connection.input.slot_idx != in.slot_idx) {
            connection.output = out;
            connection.input  = in;
            push_change(ChangeKind::connection_changed, pool_no_slot, pool_no_slot, j);
        }
    }

    // Silent: view state and the color table (no events, like set_colors).
    view_origin = snapshot.view_origin;
    zoom        = snapshot.zoom;
    colors_     = snapshot.colors;

    if (bytes_consumed) {
        *bytes_consumed = consumed;
    }
    return true;
}

bool Graph::interaction_active() const
{
    return interaction != Interaction::idle;
}

const Node& Graph::node(uint32_t node_idx) const
{
    assert(node_idx < max_nodes);
    assert(nodes.is_occupied(node_idx));
    return nodes.entries[node_idx];
}

const Connection& Graph::get_connection(uint32_t connection_idx) const
{
    assert(connection_idx < max_connections);
    assert(connections.is_occupied(connection_idx));
    return connections.entries[connection_idx];
}

const GraphColors& Graph::colors() const
{
    return colors_;
}

uint32_t Graph::connection_count() const
{
    return connections.num_allocated;
}

bool Graph::has_error() const
{
    return error_active;
}

const char* Graph::error_text() const
{
    return error_message;
}

void Graph::dismiss_error()
{
    error_active = false;
}

}  // namespace Sculptor
