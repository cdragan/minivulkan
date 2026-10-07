// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// Reusable ImGui node graph widget.
// The data model lives here and in sculptor_graph.cpp (no ImGui calls, unit
// testable); rendering and interaction live in sculptor_graph_render.cpp.

#pragma once

#include "../core/gui_imgui.h"
#include "../core/pool.h"

#include <stdint.h>

namespace Sculptor {

enum class SlotKind : uint8_t {
    unused   = 0, // marks empty slot
    input    = 1,
    output   = 2,
    property = 3, // parameter; may double as input when connectable
};

enum class PropertyType : uint8_t {
    unused  = 0,
    integer = 1,
    real    = 2,
    list    = 3,
};

union PropertyValue { // active member selected by PropertyType
    int32_t integer;
    float   real;
    uint8_t list_index;
};

struct Slot {
    char          name[32];
    SlotKind      kind;
    bool          connectable; // meaningful for property slots
    PropertyType  property_type;
    PropertyValue value;
    uint8_t       num_list_options; // for list properties
    uint8_t       row_group;        // 0 = own line; nonzero packs same-id slots onto one renderer line
    float         real_min;         // bounded reals render as a slider clamped to [real_min, real_max]
    float         real_max;
    bool          real_bounded;
    bool          real_logarithmic; // bounded slider uses a logarithmic curve
    char          list_options[8][32];
};

// Shared slot construction helpers, the grammar both projections (oscillator
// and effect) build their node rows from.
Slot make_slot(const char* name, SlotKind kind, PropertyType property_type = PropertyType::unused);
Slot output_slot(const char* name);
Slot input_slot(const char* name);
Slot real_slot(const char* name, float value);
Slot bounded_real_slot(const char* name, float value, float min_value, float max_value, bool logarithmic = false);
Slot int_slot(const char* name, int32_t value);
Slot list_slot(const char* name, const char* const* option_names, uint32_t num_options, uint32_t index);

// Option labels of the two-way source op list (SourceOp::add, SourceOp::multiply).
extern const char* const source_op_names[2];

// Sized for the widest projected node - a 5-param effect carrying the full
// modulation grammar (base + LFO row + two MIDI source rows per param:
// 3 + 5*10 = 53 slots).  Slot carries list_options[8][32], so a larger cap
// would balloon every Node allocation.
constexpr uint32_t max_node_slots = 56;

// Labels of the channel/note modulation source roles, indexed by
// ModSource - 1 (pitch_bend first).
extern const char* const mod_source_names[6];

// Draws the optional caller state widget at the bottom of a node.
// Returns the widget height in drawn pixels at render_scale (cached one
// frame and converted back to graph units by the renderer). render_scale is
// the node content scale the renderer currently draws with, so the widget
// can match the node's on-screen size.
using StateWidgetCallback = int (*)(void* user_data, float render_scale);

struct Node {
    char                       name[64];
    vmath::vec2                position;                // graph space, snapped to grid on drag
    uint32_t                   color_override;          // 0 = use default set (packed RGBA)
    float                      content_width_override;  // 0 = auto (content width)
    float                      content_height_override; // 0 = auto (content height)
    bool                       ghost;                   // ghost marker, see set_ghost
    bool                       renamable;    // title rename allowed even when node_state_edits_enabled is off
    Pool<Slot, max_node_slots> slots;        // num_allocated = slot count
    StateWidgetCallback        state_widget; // optional, may be null
    void*                      state_widget_data;
};

constexpr uint32_t max_nodes = 128;

// Visual roles the projection can install on nodes and slots (zero = plain;
// the renderer treats any other value as plain too).
constexpr uint8_t node_role_parameter  = 1; // parameter node tint
constexpr uint8_t slot_role_shared_row = 1; // shared routing row marker

struct EndPoint {
    uint32_t node_idx;
    uint32_t slot_idx;
};

struct Connection {
    EndPoint output; // from
    EndPoint input;  // to
};

// Connection budget: the reachable worst case is one oscillator->sum edge
// per layer plus 7 edges per parameter cell (value, envelope, LFO, two
// source and two LFO depth/rate edges) - 252 at seven layers, the exact
// bound pinned in sculptor_osc_graph.h; the pool holds headroom above it,
// so no connection-budget refusal path exists.  sizeof(Connection) = 16 B,
// absorbed by the fixed max_graph_bytes reserve.
constexpr uint32_t max_connections = 352;

// Static size cap for one editor-resident Graph instance.  The editor state
// budget in sculptor_instr_bank.cpp reserves exactly this much per graph;
// a capacity raise that outgrows it must fail the build instead of silently
// busting that reserve.
constexpr uint32_t max_graph_bytes = 5 * 1024 * 1024;

// Caller validation for connection attempts.  Runs after the widget's
// structural checks (endpoint kinds, bounds, single connection per input);
// return false to reject with an error overlay.  May be null.
class Graph;
using ValidationCallback = bool (*)(void* user_data, Graph& graph, EndPoint output, EndPoint input);

// Caller veto for node deletion; return true to refuse the delete.  May be
// null.  When it fires, delete_node is a no-op and reports the refusal
// through the error overlay: a veto may set a specific message via
// set_error, otherwise the generic refusal text applies.
using NodeDeleteVeto = bool (*)(void* user_data, uint32_t node_idx);

// Caller items for the empty-canvas right-click popup; the canvas menu is
// fully caller-provided (the widget adds no items of its own). May be null.
using CanvasMenuCallback = void (*)(void* user_data);

// Caller items for the node right-click popup, invoked with the right-clicked
// node before the widget's built-in items.  Returns true when items were added
// (the widget then separates them from its own).  May be null.
using NodeMenuCallback = bool (*)(void* user_data, uint32_t node_idx);

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
    uint32_t   slot_idx;       // when kind refers to a slot or value
    uint32_t   connection_idx; // when kind refers to a connection
    // Connection events carry their endpoint pair: by drain time the pool
    // slot may already be freed or reused, so apply paths must never read
    // the live pool to resolve a connection event.
    EndPoint connection_output; // valid for connection_* kinds
    EndPoint connection_input;
    // connection_changed only: the input endpoint before the retarget, so
    // apply paths can undo derived state (mirrored siblings) that keyed on
    // the old wire.  pool_no_slot endpoints for the other connection kinds.
    EndPoint connection_prev_input;
};

constexpr uint32_t max_pending_changes = 256;

// Shared between the data model and the renderer.
constexpr float graph_grid_spacing    = 16.0f; // nodes and drags snap to this
constexpr int   graph_grid_axis_cells = 4;     // stronger line every N cells
constexpr float graph_min_zoom        = 0.25f;
constexpr float graph_max_zoom        = 4.0f;

struct GraphColors { // packed 0xRRGGBBAA, converted to ImU32 at draw time
    uint32_t node_background;
    uint32_t node_border;
    uint32_t node_selected_border;
    uint32_t node_title;
    uint32_t grid_line;
    uint32_t grid_axis; // darker line every 4 cells
    uint32_t connector; // unfilled dot
    uint32_t connector_hover;
    uint32_t connector_connected; // filled dot
    uint32_t connector_missing;   // unconnected dot flagged missing (zero = no red mark)
    uint32_t connection;
    uint32_t ghost_node;
    uint32_t property_value;            // editable value text
    uint32_t property_connected_value;  // greyed value text
    uint32_t shared_row_marker;         // marker on rows backed by instrument-wide storage (zero = no mark)
    uint32_t parameter_node_background; // parameter node face (zero = node_background)
    uint32_t parameter_node_border;     // parameter node outline (zero = node_border)
    uint32_t error_background;          // error overlay
    uint32_t error_text;
    uint32_t selection_outline; // outline around selected nodes
    uint32_t selection_band;    // rubber band fill/outline
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
    // Renames a node (the projection assigns deterministic parameter names
    // after record attachment).  Pushes name_changed like an interactive rename.
    void rename_node(uint32_t node_idx, const char* name);
    void delete_node(uint32_t node_idx);
    // Deletes every occupied, selected, non-ghost node through delete_node
    // (the veto and change events apply unchanged); returns how many were
    // actually deleted.
    uint32_t delete_selected();

    // Removes every node (and with them every connection) without consulting
    // the delete veto: wholesale rebuilds (projection, load) replace
    // caller-driven state, they are not user deletes.
    void     clear();
    void     remove_slot(uint32_t node_idx, uint32_t slot_idx); // drops touching connections
    uint32_t add_slot(uint32_t node_idx, const Slot& slot);     // returns slot idx
    uint32_t add_connection(EndPoint output, EndPoint input);   // returns connection idx
    void     delete_connection(uint32_t connection_idx);

    // Connection validation and validated connect/retarget.  attempt_connection
    // and move_connection_end are the render side's entry points: they run the
    // structural checks plus the caller validator and report failure through
    // the error overlay instead of silently returning an index.
    void set_validator(ValidationCallback callback, void* user_data);

    // Connection index excluded from validation while move_connection_end
    // re-lands an existing wire (pool_no_slot otherwise).  The validator sees
    // the graph still in its pre-move state, so a constraint that counts the
    // moved wire's current endpoint would refuse the move spuriously.  Managed
    // by move_connection_end; read-only for validators.
    uint32_t moving_connection = pool_no_slot;

    // Refusal convention: delete_node stays void and refuses silently for
    // bad indices; a vetoed delete also stays void, mutates nothing,
    // pushes no events and reports through the error overlay.
    // Snapshot restoration (load) bypasses the veto: it restores
    // caller-driven state rather than acting on a user delete.
    void set_delete_veto(NodeDeleteVeto callback, void* user_data);
    bool attempt_connection(EndPoint output, EndPoint input);

    // Retargets one end of a connection in place (connection_changed event).
    // A refused retarget snaps back: the connection keeps its old
    // endpoints, pushes no change events, and the refusal surfaces through
    // the error overlay.  Destroying the connection here would queue a
    // phantom delete whose commit/reprojection cycle erases the overlay
    // before it renders.  Only a release off any connector deletes the
    // connection (user intent).
    bool move_connection_end(uint32_t connection_idx, bool move_output_end, EndPoint new_point);

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
    void        report_content_size(uint32_t node_idx, vmath::vec2 size);
    vmath::vec2 content_size(uint32_t node_idx) const;

    // Colors
    void set_colors(const GraphColors& colors);                   // caller-provided default set
    void set_node_color(uint32_t node_idx, uint32_t packed_rgba); // 0 = default

    // Optional state widget drawn at the bottom of the node.  The node is
    // laid out from the widget's height one frame before the widget's first
    // draw reports it, so a caller that knows the height up front passes it
    // to keep the first frame after attach laid out correctly.
    void set_state_widget(uint32_t            node_idx,
                          StateWidgetCallback callback,
                          void*               user_data,
                          float               initial_height = 0.0f);

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

    // Empty-canvas right-click popup: the menu is fully caller-provided and
    // the widget adds no items of its own; add-commands belong to the callback.
    void set_canvas_menu_callback(CanvasMenuCallback callback, void* user_data);
    // Node right-click popup: caller items render before the widget's built-in
    // align/equal-size/Delete items.
    void set_node_menu_callback(NodeMenuCallback callback, void* user_data);
    // Graph-space position of the right-click that opened the canvas popup,
    // for callers that place added nodes at the menu point.
    vmath::vec2 canvas_popup_pos() const;
    uint32_t    save(uint8_t* buffer, uint32_t buffer_size) const; // returns bytes
    bool        load(const uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_consumed);

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
    bool     changes_overflowed();

    // Read accessors (const)
    const Node& node(uint32_t node_idx) const;
    // True when the node pool slot is live (same sparsity rule as connections).
    bool node_occupied(uint32_t node_idx) const;
    // Restores a moved/resized node layout after a re-projection; pushes no
    // change events (layout state, not model state).
    void set_node_layout(uint32_t node_idx, vmath::vec2 position, float width_override, float height_override);
    const Connection&  get_connection(uint32_t connection_idx) const;
    const GraphColors& colors() const;
    uint32_t           connection_count() const;

    // True when the connection pool slot is live.  Pool indices are stable but
    // sparse after deletions, so callers enumerate 0..max_connections-1 with
    // this guard instead of assuming a dense 0..connection_count()-1 range.
    bool connection_occupied(uint32_t connection_idx) const;

    // Per-slot "missing" render state: the editor marks the sum input
    // slot of a layer whose oscillator->sum edge was deleted, so the renderer
    // draws unconnected endpoints red.  Kept out of graph snapshots and
    // re-applied after re-projection.
    void set_slot_missing(uint32_t node_idx, uint32_t slot_idx, bool missing);
    bool slot_missing(uint32_t node_idx, uint32_t slot_idx) const;

    // Per-node/per-slot visual roles the projection installs (zero = plain).
    // Like the missing marks, roles are projection state: clear() wipes them
    // and the projection re-installs them, so they stay out of snapshots.
    // The renderer maps a parameter node to the parameter background/border
    // colors and a shared-row slot to the shared_row_marker glyph.
    void    set_node_visual_role(uint32_t node_idx, uint8_t role);
    void    set_slot_visual_role(uint32_t node_idx, uint32_t slot_idx, uint8_t role);
    uint8_t node_visual_role(uint32_t node_idx) const;
    uint8_t slot_visual_role(uint32_t node_idx, uint32_t slot_idx) const;
    // A row's knobs are inert while the row's input is unconnected.
    void set_slot_edit_disabled(uint32_t node_idx, uint32_t slot_idx, bool disabled);
    bool slot_edit_disabled(uint32_t node_idx, uint32_t slot_idx) const;

    // True when any connection references this slot (input/property: as input
    // endpoint; output: as output endpoint).
    bool slot_is_connected(uint32_t node_idx, uint32_t slot_idx) const;

    // Per-node title-rename opt-in: the osc graph disables node-state edits
    // graph-wide, so parameter nodes opt in one by one.  Projection state
    // like the visual roles - clear() wipes it and the projection
    // re-installs it.
    void set_node_renamable(uint32_t node_idx, bool renamable);

    // Eventless property write: sets a property slot's value WITHOUT
    // pushing a change event, used by the shared-routing fan-out so a shared
    // edit on one oscillator node updates the sibling nodes silently.
    void set_slot_value(uint32_t node_idx, uint32_t slot_idx, PropertyValue value);

    // Eventless connection write, the connection-side counterpart of
    // set_slot_value: makes the single edge entering (node_idx, slot_idx)
    // carry exactly `output`, or removes that edge when output.node_idx is
    // pool_no_slot.  Used by apply-side mirroring, which must not echo events
    // back into the drain.
    void set_slot_input(uint32_t node_idx, uint32_t slot_idx, EndPoint output);
    // Eventless connection rewrite, the connection-side counterpart of
    // set_slot_input for connectable-property inputs (oscillator value rows
    // are connectable properties, which set_slot_input refuses): rewrites
    // the connection's input endpoint in place, or frees the connection when
    // the endpoint is no-slot. Used by apply-side retargeting, which must
    // not echo events back into the drain.
    void set_connection_input(uint32_t connection_idx, EndPoint input);

    // Error overlay state, set by rejected connect/retarget attempts and
    // shown by the renderer until the user dismisses it with Esc.
    bool        has_error() const;
    const char* error_text() const;
    void        dismiss_error();

    // Reports a caller-side failure through the error overlay (used by the
    // projection to surface refusals).
    void set_error(const char* message);

    // Connection index shown in the renderer's connection context menu;
    // exposed so tests can verify that clear() leaves no stale popup target.
    uint32_t connection_popup() const;

    // Rendering: call inside an already-open window/child.  Never calls
    // Begin/End.  Defined in sculptor_graph_render.cpp (ImGui linkage stays
    // out of sculptor_graph.cpp so the unit test can link the data model).
    void render(vmath::vec2 size, void* user_data);

    // Rename, node recolor and ghost placement push events (name_changed,
    // color_changed, ghost_placed) that carry no model state a caller cannot
    // reconstruct: a caller that does not apply those events must keep this
    // false, or user input is silently dropped and re-projected away.  Zero =
    // the interactions stay hidden.
    bool node_state_edits_enabled;

private:
    // Event queue (ring buffer); returns false when the event was suppressed
    // (quiet rebuild) or dropped (overflow).
    bool push_change(ChangeKind kind, uint32_t node_idx, uint32_t slot_idx, uint32_t connection_idx);
    // Connection events additionally record the endpoint pair, taken while
    // the connection is still in the pool (deletes) or as just written
    // (adds and retargets).
    void push_connection_change(ChangeKind kind,
                                uint32_t   connection_idx,
                                EndPoint   output,
                                EndPoint   input,
                                EndPoint   prev_input);

    // Applies a node deletion without consulting the delete veto.  Snapshot
    // restoration uses it because the veto guards user-facing deletes, not
    // restores.  The caller has bounds/occupancy checked node_idx.
    void delete_node_unvetoed(uint32_t node_idx);

    // Structural checks shared by add_connection, attempt_connection and
    // move_connection_end: bounds, node/slot existence, endpoint kinds.
    bool endpoints_structurally_valid(EndPoint output, EndPoint input) const;
    // Input endpoints (input and connectable property slots) accept a single
    // connection; output endpoints fan out freely.
    bool input_slot_taken(uint32_t node_idx, uint32_t slot_idx, uint32_t except_connection) const;

    Pool<Node, max_nodes>             nodes       = {};
    Pool<Connection, max_connections> connections = {};

    // Caller validation callback.
    ValidationCallback validator           = nullptr;
    void*              validator_user_data = nullptr;

    // Caller veto for delete_node and items for the canvas popup.
    NodeDeleteVeto     delete_veto           = nullptr;
    void*              delete_veto_user_data = nullptr;
    CanvasMenuCallback canvas_menu_callback  = nullptr;
    NodeMenuCallback   node_menu_callback    = nullptr;
    void*              node_menu_user_data   = nullptr;
    void*              canvas_menu_user_data = nullptr;

    // Error overlay state.
    char        error_message[128]           = {};
    bool        error_active                 = false;
    GraphChange changes[max_pending_changes] = {}; //  ring buffer
    uint32_t    changes_head                 = 0;
    uint32_t    changes_count                = 0;
    bool        changes_overflowed_flag      = false;
    // Set by clear(): a wholesale rebuild must not echo its construction
    // traffic into the edit pipeline, so events stay suppressed until the next
    // drain re-arms the ring for user-driven changes.
    bool        changes_quiet = false;
    GraphColors colors_; // zero colors render nothing: the owner calls
    // set_colors with a palette before showing the widget

    // View state: graph-space position shown at the widget origin, and zoom.
    // A zero zoom is treated as 1 by the renderer, so the zero-filled state
    // is directly usable.
    vmath::vec2 view_origin = {};
    float       zoom;

    // One active interaction mode at a time.
    enum class Interaction : uint8_t {
        idle          = 0,
        dragging_node = 1,
        panning       = 2,
        renaming      = 3,
        connecting    = 4, // dragging a new connection from a free dot
        retargeting   = 5, // dragging one end of an existing connection
        rubber_band   = 6, // Ctrl + drag selection rectangle on empty canvas
    };
    Interaction interaction = Interaction::idle;
    uint32_t    dragged_node;
    uint32_t    renaming_node;
    bool        renaming_focus = false; //  first frame of rename: set keyboard focus
    bool        title_pressed  = false; //  drag started on title: click (no move) yet
    vmath::vec2 drag_offset    = {};    //  mouse offset within node at drag start
    vmath::vec2 band_start     = {};    //  rubber band start corner (graph space)

    // Connection dragging: anchor endpoint and, when retargeting, which
    // connection end is being moved.
    EndPoint    connecting_from;             //  anchor dot
    uint32_t    retarget_connection;         //  pool_no_slot when connecting anew
    bool        retarget_output_end = false; //  true when the dragged end is the output
    uint32_t    popup_connection;            //  connection shown in the Delete popup
    uint32_t    popup_node;                  //  node shown in the align/Delete popup
    vmath::vec2 popup_canvas_pos = {};       //  right-click point for the canvas popup add

    // Per-slot "missing" render state (red unconnected dot), editor-settable.
    // Kept out of graph snapshots: the editor re-applies the marks after every
    // re-projection.  One bit per (node, slot); zero-filled state = no marks.
    uint8_t slot_missing_bits[(max_nodes * max_node_slots + 7) / 8] = {};

    // Per-node/per-slot visual roles (see set_node_visual_role); projection
    // state, wiped by clear() like the missing marks.
    uint8_t node_visual_roles[max_nodes]                         = {};
    uint8_t slot_visual_roles[max_nodes * max_node_slots]        = {};
    uint8_t slot_edit_disabled_flags[max_nodes * max_node_slots] = {};

    // State widget heights cached from the previous frame (1-frame lag).
    float state_widget_heights[max_nodes];
    // Whether the mouse sat over a node's state widget last frame (1-frame
    // lag, like the heights).  The canvas leaves wheel zoom to the widget
    // (the envelope chart zooms itself) while the mouse is over one.
    bool state_widget_hovered[max_nodes] = {};

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

// The live connection terminating at an input connector, or pool_no_slot.
uint32_t connection_into(const Graph& graph, uint32_t node_idx, uint32_t slot_idx);
} // namespace Sculptor
