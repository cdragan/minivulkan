// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Reusable ImGui node graph widget.
// The data model lives here and in sculptor_graph.cpp (no ImGui calls, unit
// testable); rendering and interaction live in sculptor_graph_render.cpp.
// See .pi/develop/criteria.md and .pi/develop/m1/plan.md for the design.

#pragma once

#include "../core/gui_imgui.h"
#include "../core/pool.h"

#include <stdint.h>

namespace Sculptor {

enum class SlotKind : uint8_t {
    unused   = 0,  // marks empty slot
    input    = 1,
    output   = 2,
    property = 3,  // parameter; may double as input when connectable
};

enum class PropertyType : uint8_t {
    unused  = 0,
    integer = 1,
    real    = 2,
    list    = 3,
};

union PropertyValue {  // active member selected by PropertyType
    int32_t integer;
    float   real;
    uint8_t list_index;
};

struct Slot {
    char          name[32];
    SlotKind      kind;
    bool          connectable;  // meaningful for property slots
    PropertyType  property_type;
    PropertyValue value;
    uint8_t       num_list_options;  // for list properties
    char          list_options[8][32];
};

constexpr uint32_t max_node_slots = 16;

// Draws the optional caller state widget at the bottom of a node.
// Returns the widget height in pixels; the height is cached one frame.
using StateWidgetCallback = int (*)(void* user_data);

struct Node {
    char                       name[64];
    vmath::vec2                position;        // graph space, snapped to grid on drag
    uint32_t color_override; // 0 = use default set (packed RGBA)
    float                      content_width_override;   // 0 = auto (content width)
    float                      content_height_override;  // 0 = auto (content height)
    bool                       ghost;           // ghost marker, see set_ghost
    Pool<Slot, max_node_slots> slots;           // num_allocated = slot count
    StateWidgetCallback        state_widget;    // optional, may be null
    void*                      state_widget_data;
};

constexpr uint32_t max_nodes = 64;

struct EndPoint {
    uint32_t node_idx;
    uint32_t slot_idx;
};

struct Connection {
    EndPoint output;  // from
    EndPoint input;   // to
};

constexpr uint32_t max_connections = 128;

// Caller validation for connection attempts.  Runs after the widget's
// structural checks (endpoint kinds, bounds, single connection per input);
// return false to reject with an error overlay.  May be null.
class Graph;
using ValidationCallback = bool (*)(void* user_data, const Graph& graph,
                                    EndPoint output, EndPoint input);

// Pools never compact: connections store stable node/slot indices, so
// defragment() is deliberately unused.  Pools, view state, change queue and
// colors are non-static members so callers may keep several widget instances
// alive at once (each owns its own state).  Only the default color table is
// shared and it is a constant.

enum class ChangeKind : uint8_t {
    none               = 0,
    node_added         = 1,
    node_deleted       = 2,
    slot_added         = 3,
    slot_deleted       = 4,
    connection_added   = 5,
    connection_deleted = 6,
    value_changed      = 7,
    name_changed       = 8,
    color_changed      = 9,
    ghost_placed       = 10,
    connection_changed = 11, // one end retargeted in place
};

// Events carry indices, not values: the caller reads current state via the
// const accessors, so a live synth can ramp from its own smooth value.
struct GraphChange {
    ChangeKind kind;
    uint32_t   node_idx;
    uint32_t   slot_idx;        // when kind refers to a slot or value
    uint32_t   connection_idx;  // when kind refers to a connection
};

constexpr uint32_t max_pending_changes = 256;

// Shared between the data model and the renderer.
constexpr float graph_grid_spacing = 16.0f;   // nodes and drags snap to this
constexpr int   graph_grid_axis_cells = 4;    // stronger line every N cells
constexpr float graph_min_zoom = 0.25f;
constexpr float graph_max_zoom = 4.0f;

struct GraphColors {  // packed 0xRRGGBBAA, converted to ImU32 at draw time
    uint32_t node_background;
    uint32_t node_border;
    uint32_t node_selected_border;
    uint32_t node_title;
    uint32_t grid_line;
    uint32_t grid_axis;                 // darker line every 4 cells
    uint32_t connector;                 // unfilled dot
    uint32_t connector_hover;
    uint32_t connector_connected;       // filled dot
    uint32_t connection;
    uint32_t ghost_node;
    uint32_t property_value;            // editable value text
    uint32_t property_connected_value;  // greyed value text
    uint32_t error_background;          // error overlay
    uint32_t error_text;
    uint32_t selection_outline;         // outline around selected nodes
    uint32_t selection_band;            // rubber band fill/outline
};

// Default color table, shared by all widget instances; defined in sculptor_graph.cpp.
GraphColors default_graph_colors();

// Multi-node layout commands for the selection context menu.
enum class AlignKind : uint8_t {
    left         = 0,
    right        = 1,
    top          = 2,
    bottom       = 3,
    equal_width  = 4,
    equal_height = 5,
};

// Optional caller-state serialization hooks, appended to graph snapshots so
// undo/redo and files include caller state (e.g. synth patch parameters).
using SerializeState   = uint32_t (*)(void* user_data, uint8_t* buffer, uint32_t buffer_size);
using DeserializeState = bool (*)(void* user_data, const uint8_t* buffer, uint32_t buffer_size);

class Graph {
    public:
        Graph() = default;

        // Node management (thin wrappers over Pool::allocate/free)
        uint32_t create_node(const char* name, vmath::vec2 position);
        void delete_node(uint32_t node_idx);
        void remove_slot(uint32_t node_idx, uint32_t slot_idx); // drops touching connections
        uint32_t add_slot(uint32_t node_idx, const Slot& slot); // returns slot idx
        uint32_t add_connection(EndPoint output, EndPoint input); // returns connection idx
        void delete_connection(uint32_t connection_idx);

        // Connection validation and validated connect/retarget.  attempt_connection
        // and move_connection_end are the render side's entry points: they run the
        // structural checks plus the caller validator and report failure through
        // the error overlay instead of silently returning an index.
        void set_validator(ValidationCallback callback, void* user_data);
        bool attempt_connection(EndPoint output, EndPoint input);
        // Retargets one end of a connection in place (connection_changed event).
        // A failed retarget destroys the connection, same rule as a failed drop.
        bool move_connection_end(uint32_t connection_idx, bool move_output_end,
                                 EndPoint new_point);

        // Ghost mode: the caller creates the node normally via create_node (fully
        // allocated in the pool) and marks it with set_ghost.  While marked, the
        // node follows the mouse when rendered.  On placement the widget clears
        // the flag (pushing ghost_placed); on cancel the caller deletes the node.
        void set_ghost(uint32_t node_idx, bool ghost);

        // Selection (pure view state: not serialized, no change events).  The
        // renderer never selects ghost nodes and align_selected skips them.
        bool is_selected(uint32_t node_idx) const;
        void set_selected(uint32_t node_idx, bool node_selected);
        void select_none();

        // Multi-node layout commands (selection context menu).  left/top move
        // every selected node onto the leftmost/topmost selected edge; right/
        // bottom onto the rightmost/bottommost edge.  equal_width sets every
        // selected node's content width override to the widest selected width;
        // equal_height sets every selected node's content height override to the
        // tallest selected height (the extra space renders below the content).
        // Position moves push no change events.
        void align_selected(AlignKind kind);

        // Renderer-reported node content sizes (graph space), used by align and
        // the Home fit-view.  Sizes read 0 until the node's first drawn frame.
        void report_content_size(uint32_t node_idx, vmath::vec2 size);
        vmath::vec2 content_size(uint32_t node_idx) const;

        // Colors
        void set_colors(const GraphColors& colors); // caller-provided default set
        void set_node_color(uint32_t node_idx, uint32_t packed_rgba); // 0 = default

        // Optional state widget drawn at the bottom of the node.
        void set_state_widget(uint32_t node_idx, StateWidgetCallback callback, void* user_data);

        // Persistence (M4): save writes a tightly packed, versioned snapshot:
        // u16 version, graph pools, view state, colors, ghost flags, then - if
        // state hooks are installed - caller state bytes.  load restores the graph
        // and reports bytes consumed via *bytes_consumed so the caller can parse
        // its own tail; the same snapshot doubles as the UndoRedo entry and file
        // payload (Sculptor::Geometry pattern).
        // Application is event-based: load diffs the live state against the
        // incoming snapshot and enqueues the difference as normal GraphChange
        // events, so a live synth ramps surgically instead of being rebuilt.
        void set_state_callbacks(SerializeState serialize, DeserializeState deserialize, void* user_data);
        uint32_t save(uint8_t* buffer, uint32_t buffer_size) const; // returns bytes
        bool load(const uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_consumed);

        // True while a state-mutating interaction is in progress (drag, edit...).
        // The caller uses the false->true edge to push a pre-change snapshot and
        // groups all changes of one interaction into a single undo entry.
        bool interaction_active() const;

        // Change notification: every mutation pushes an event.  take_changes
        // drains events in order; the caller reacts incrementally (e.g. the synth
        // applies smooth ramps for value_changed).  Node moves push nothing.
        // If the queue overflows, changes_overflowed() returns true once and the
        // caller must resynchronize from the full graph state.
        uint32_t take_changes(GraphChange* out, uint32_t out_size);
        bool changes_overflowed();

        // Read accessors (const)
        const Node& node(uint32_t node_idx) const;
        const Connection& get_connection(uint32_t connection_idx) const;
        const GraphColors& colors() const;
        uint32_t connection_count() const;

        // Error overlay state, set by rejected connect/retarget attempts and
        // shown by the renderer until the user dismisses it with Esc.
        bool has_error() const;
        const char* error_text() const;
        void dismiss_error();

        // Rendering: call inside an already-open window/child.  Never calls
        // Begin/End.  Defined in sculptor_graph_render.cpp (ImGui linkage stays
        // out of sculptor_graph.cpp so the unit test can link the data model).
        void render(vmath::vec2 size, void* user_data);

    private:
        // Event queue (ring buffer)
        void push_change(ChangeKind kind, uint32_t node_idx, uint32_t slot_idx, uint32_t connection_idx);

        // True when any connection references this slot (input/property: as input
        // endpoint; output: as output endpoint).
        bool slot_is_connected(uint32_t node_idx, uint32_t slot_idx) const;

        // Structural checks shared by add_connection, attempt_connection and
        // move_connection_end: bounds, node/slot existence, endpoint kinds.
        bool endpoints_structurally_valid(EndPoint output, EndPoint input) const;
        // Input endpoints (input and connectable property slots) accept a single
        // connection; output endpoints fan out freely.
        bool input_slot_taken(uint32_t node_idx, uint32_t slot_idx,
        uint32_t except_connection) const;
        void set_error(const char* message);

        Pool<Node, max_nodes> nodes                   = {};
        Pool<Connection, max_connections> connections = {};

        // Caller validation callback.
        ValidationCallback     validator           = nullptr;
        void*                  validator_user_data = nullptr;

        // Error overlay state.
        char       error_message[128]            = {};
        bool       error_active                  = false;
        GraphChange changes[max_pending_changes] = {};                      //  ring buffer
        uint32_t   changes_head                  = 0;
        uint32_t   changes_count                 = 0;
        bool       changes_overflowed_flag       = false;
        GraphColors colors_                      = default_graph_colors();

        // View state: graph-space position shown at the widget origin, and zoom.
        vmath::vec2 view_origin = {};
        float       zoom        = 1.0f;

        // One active interaction mode at a time.
        enum class Interaction : uint8_t {
            idle = 0,
            dragging_node = 1,
            panning = 2,
            renaming = 3,
            connecting = 4, // dragging a new connection from a free dot
            retargeting = 5, // dragging one end of an existing connection
            rubber_band = 6, // Ctrl + drag selection rectangle on empty canvas
        };
        Interaction interaction    = Interaction::idle;
        uint32_t    dragged_node   = pool_no_slot;
        uint32_t    renaming_node  = pool_no_slot;
        bool        renaming_focus = false;              //  first frame of rename: set keyboard focus
        bool        title_pressed  = false;              //  drag started on title: click (no move) yet
        vmath::vec2 drag_offset    = {};                 //  mouse offset within node at drag start
        vmath::vec2 band_start     = {};                 //  rubber band start corner (graph space)

        // Connection dragging: anchor endpoint and, when retargeting, which
        // connection end is being moved.
        EndPoint   connecting_from     = {pool_no_slot, pool_no_slot};  //  anchor dot
        uint32_t   retarget_connection = pool_no_slot;                  //  pool_no_slot when connecting anew
        bool       retarget_output_end = false;                         //  true when the dragged end is the output
        uint32_t   popup_connection    = pool_no_slot;                  //  connection shown in the Delete popup

        // State widget heights cached from the previous frame (1-frame lag).
        float state_widget_heights[max_nodes];

        // Selection flags, parallel to the node pool slots.
        bool selected[max_nodes] = {};

        // Node content sizes (graph space) cached from the previous frame, used by
        // the Home fit-view.
        vmath::vec2 content_sizes[max_nodes] = {};

        // Dot screen positions recorded while drawing (1-frame lag for hit
        // tests), indexed by node_idx * max_node_slots + slot_idx.
        vmath::vec2 dot_positions[max_nodes * max_node_slots] = {};

        // Persistence hooks (M4)
        SerializeState   serialize_state   = nullptr;
        DeserializeState deserialize_state = nullptr;
        void*            state_user_data   = nullptr;
};

// core/pool.h defines pool_no_slot at global scope; re-exported so callers can
// stay inside the namespace.
constexpr uint32_t pool_no_slot = ::pool_no_slot;

}  // namespace Sculptor
