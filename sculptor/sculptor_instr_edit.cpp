// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_edit.h"
#include "sculptor_bank_json.h"
#include "sculptor_effect_graph.h"
#include "sculptor_graph.h"
#include "sculptor_instr_bank.h"
#include "sculptor_instr_envelope_edit.h"
#include "sculptor_notifications.h"
#include "sculptor_osc_graph.h"

#include "../synth/midi_input.h"
#include "../synth/realtime_synth.h"
#include "sculptor_undo.h"

#include "../core/d_printf.h"
#include "../core/gui_imgui.h"
#include <string.h>
#include <type_traits>

// Per-envelope-node state-widget contexts, indexed by graph node index and
// rebound on every projection.  A reset on projection also aborts gestures
// and selections, which cannot outlive the graph they were made in.  The
// projection generation gates in-flight gestures against envelope pool
// compaction that remaps descriptor ids without a re-projection.
struct EnvelopeWidgetContext {
    uint32_t                     node_idx        = Sculptor::pool_no_slot;
    Sculptor::SynthEditor*       editor          = nullptr;
    uint32_t                     generation      = 0;
    Synth::EnvelopeDescriptor    gesture_start   = {};
    uint32_t                     gesture_desc_id = 0;
    Sculptor::UndoGroupTag       gesture_tag     = {};
    bool                         gesture_active  = false;
    Sculptor::EnvelopeCurveState ui;
};

// Canvas popup commands are only queued during render; the add commands run
// afterwards against the committed bank.
enum OscCanvasCommand {
    osc_cmd_none,
    osc_cmd_add_oscillator,
    osc_cmd_add_envelope,
    osc_cmd_add_lfo,
    osc_cmd_add_parameter,
    osc_cmd_change_target_param
};

// Effects canvas and node-menu commands, queued during render and run
// after the drain against the committed bank.
enum FxCanvasCommand {
    fx_cmd_none,
    fx_cmd_add_effect,
    fx_cmd_add_lfo,
    fx_cmd_change_type
};

// The delete veto needs the graph to report a specific refusal, so the
// mapping rides with a pointer to the widget instance it describes.
struct OscDeleteVetoContext {
    const Sculptor::OscGraphMapping* mapping;
    Sculptor::Graph*                 graph;
};

namespace {

// The scan-refusal messages are shared by the save path and the browser so the
// wording cannot drift between them.
const char* const library_oversized_refusal    = "Synth: %s holds a record too large to rebuild; saving is refused";
const char* const library_invalid_save_refusal = "Synth: %s is not a valid instrument library; saving is refused";
const char* const library_invalid_open_refusal = "Synth: %s is invalid or unreadable";

Synth::InstrumentEditorBank instr_bank; // GUI-thread-owned editable bank (names included).
Sculptor::UndoRedo          undo_redo;
constexpr uint32_t          undo_depth = 10;

// Queue for shipping edited banks from the GUI to the synth audio thread.
Synth::BankUpdateQueue bank_queue;

bool                  bank_changes_pending = false;
Synth::InstrumentBank pending_bank;

// The disk save follows the audible publish a couple of frames behind, so a
// mid-drag stream of publishes never queues file writes.  A crash can lose
// the unsaved tail of an unfinished gesture; the next full save after the
// gesture recovers the file.
bool     save_pending         = false;
uint32_t frames_since_publish = 0;

// The oscillator graph editor: one widget instance plus its mapping, kept in
// step with the committed bank by re-projection (selection change, undo/redo,
// load, zone operations, structural or refused commits) and by per-frame
// change application (drain -> apply -> single commit -> publish).
Sculptor::Graph           osc_graph;
Sculptor::OscGraphMapping osc_mapping;
Sculptor::UndoGroupState  osc_undo_group         = {};
uint32_t                  osc_graph_zone_channel = 0;
uint32_t                  osc_graph_zone_index   = 0;
bool                      osc_graph_projected    = false; // mapping matches the selection
bool                      osc_graph_reproject    = false; // deferred wholesale rebuild

// The effects graph projection: the same widget instance hosts the channel
// and master effect chains, mutually exclusive with the oscillator
// projection. The chain binding is the truth; a moved node keeps a
// kind-4 bank record, so its place survives re-projections, undo and file
// saves, while untouched nodes re-derive from the deterministic layout.
Sculptor::EffectGraphMapping fx_mapping;
bool                         fx_graph_projected = false;
bool                         fx_graph_reproject = false;

// LFO descriptors the canvas must show even while nothing references them
// (a freshly added LFO awaiting its first wire).  Session-only view state:
// the projection reads it, the bank never stores it.
bool fx_lfo_pinned[Synth::max_lfos] = {};

EnvelopeWidgetContext envelope_widget_contexts[Sculptor::max_nodes];
uint32_t              osc_graph_generation = 0;

OscCanvasCommand osc_canvas_command         = osc_cmd_none;
uint32_t         osc_canvas_retarget_node   = Sculptor::pool_no_slot; // pending osc_cmd_change_target_param
uint32_t         osc_canvas_retarget_target = 0;

FxCanvasCommand   fx_canvas_command = fx_cmd_none;
Synth::EffectType fx_canvas_type    = Synth::EffectType::none;
uint32_t          fx_menu_node      = Sculptor::pool_no_slot;

// Undo tags: every commit kind has its own tag so one edit never coalesces
// into another's undo entry.  Drained batches use graph_base + change kind,
// layout-only commits use graph_base with the moved node, and batches with
// no single-field identity (two different change kinds in one frame, or a
// multi-node layout gesture) take a fresh unique tag so they never amend a
// batch-shaped or node-shaped entry.
constexpr uint32_t osc_tag_graph_base          = 16;
constexpr uint32_t osc_tag_init                = 1;
constexpr uint32_t osc_tag_delete              = 2;
constexpr uint32_t osc_tag_join_prev           = 3;
constexpr uint32_t osc_tag_join_next           = 4;
constexpr uint32_t osc_tag_zone_split          = 5;
constexpr uint32_t osc_tag_zone_drop           = 6;
constexpr uint32_t osc_tag_rename              = 7;
constexpr uint32_t osc_tag_load                = 8;
constexpr uint32_t osc_tag_add_osc             = 9;
constexpr uint32_t osc_tag_add_env             = 10;
constexpr uint32_t osc_tag_add_lfo             = 11;
constexpr uint32_t osc_tag_add_param           = 12;
constexpr uint32_t osc_tag_change_target_param = 13;
constexpr uint32_t osc_tag_fresh               = osc_tag_graph_base + 12; // outside every batch kind
uint32_t           osc_fresh_tag_serial        = 0;                       // keeps consecutive fresh tags distinct

// Effects undo tags: kinds distinct from every osc tag, same grouping rules
// (a same-tag per-frame commit amends one undo entry, a different tag
// snapshots).  fx_tag_value and fx_tag_panel carry node/slot/field ids so a
// slider drag stays one entry.
constexpr uint32_t fx_tag_value     = 32;
constexpr uint32_t fx_tag_add       = 33;
constexpr uint32_t fx_tag_type      = 34;
constexpr uint32_t fx_tag_add_lfo   = 35; // id0 = the fresh descriptor id
constexpr uint32_t fx_tag_node_move = 36;

OscDeleteVetoContext osc_veto_context = { &osc_mapping, &osc_graph };

// Layout of every node as the projection built it: a node whose live layout
// still matches needs no layout record, so records stay sparse.
vmath::vec2 projected_position[Sculptor::max_nodes];
float       projected_width[Sculptor::max_nodes];
float       projected_height[Sculptor::max_nodes];

static const char bank_state_path[] = "assets/instrument_bank.synth";

static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);

// Transactional command scratch: structural commands mutate this candidate, validate it,
// and only then commit it over the editable bank (see commit_candidate).
Synth::InstrumentEditorBank candidate;

// One clipboard document: copy encodes into it, paste decodes out of it.
static char                         zone_clipboard_text[64 * 1024];
static Synth::Instrument            clipboard_decoded_instrument;
static Synth::EnvelopeDescriptor    clipboard_decoded_envelopes[Synth::instrument_max_envelopes];
static Synth::LFODescriptor         clipboard_decoded_lfos[Synth::instrument_max_lfos];
static Synth::InstrumentGraphLayout clipboard_source_layout[Synth::instrument_graph_layout_capacity];
static Synth::InstrumentGraphLayout clipboard_decoded_layout[Synth::instrument_graph_layout_capacity];
uint8_t                             undo_buf[(sizeof(Synth::InstrumentEditorBank) + 2 * sizeof(uint32_t)) * undo_depth];
static_assert(sizeof(undo_buf) <= undo_depth * (sizeof(Synth::InstrumentEditorBank) + 2 * sizeof(uint32_t)));

// Banks: ten undo snapshots, editable/candidate/undo scratch, library load/save,
// JSON decode and clipboard candidate. Runtime, pending publish and clipboard
// model validation each own a distinct InstrumentBank; the queue owns two more.
constexpr size_t editor_resident_state_bytes =
    (undo_depth + 7) * sizeof(Synth::InstrumentEditorBank) + 3 * sizeof(Synth::InstrumentBank) + sizeof(bank_queue) +
    1212416 +                                                  // JSON text, tokens and keys
    sizeof(Sculptor::SynthEditor) +                            // includes the browser's entry/category arrays
    Synth::library_max_records * sizeof(Synth::LibraryEntry) + // library rewrite index
    64 * 1024 +                                                // library record-copy chunk
    sizeof(undo_redo) + 2 * undo_depth * sizeof(uint32_t) + sizeof(osc_graph) + sizeof(osc_mapping) +
    sizeof(fx_mapping) + sizeof(envelope_widget_contexts) + sizeof(fx_lfo_pinned) + sizeof(projected_position) +
    sizeof(projected_width) + sizeof(projected_height) + sizeof(zone_clipboard_text) +
    sizeof(clipboard_decoded_instrument) + sizeof(clipboard_decoded_envelopes) + sizeof(clipboard_decoded_lfos) +
    sizeof(clipboard_source_layout) + sizeof(clipboard_decoded_layout) +
    sizeof(Synth::Instrument) + // codec document model
    Synth::instrument_max_envelopes * sizeof(Synth::EnvelopeDescriptor) +
    Synth::instrument_max_lfos * sizeof(Synth::LFODescriptor) + Synth::num_mod_targets * sizeof(bool) +
    2 * sizeof(uint32_t) +
    Synth::instrument_graph_layout_capacity *
        (sizeof(Synth::InstrumentGraphLayout) + 2 * sizeof(Synth::GraphNodeLayout)) +
    (Synth::max_lfos + Synth::instrument_max_envelopes + Synth::instrument_max_lfos) * sizeof(uint16_t) +
    Synth::max_name_len + Sculptor::notification_state_bytes +
    2 * Synth::instrument_graph_layout_capacity *
        (sizeof(Synth::InstrumentGraphLayout) + sizeof(uint8_t) * (1 + Synth::num_mod_targets) + 2) +
    Synth::instrument_graph_layout_capacity *
        (sizeof(Synth::InstrumentGraphLayout) + sizeof(Synth::GraphNodeLayout) + sizeof(void*)) +
    4096; // remaining editor scalar flags, constants and alignment
static_assert(editor_resident_state_bytes <= 16 * 1024 * 1024);

} // namespace

// Pressing a keyboard key submits plain note events through the synth's live-MIDI
// input, like any external keyboard; the synth knows nothing about the editor.
// Returns false when the event was dropped (input ring buffer full).
static bool submit_note_event(uint32_t channel, uint32_t note, bool note_on)
{
    Synth::MidiEvent event = {};
    event.event            = note_on ? Synth::EvType::note_on : Synth::EvType::note_off;
    event.channel          = static_cast<uint8_t>(channel);
    event.note             = static_cast<uint8_t>(note);
    event.note_data        = 127;
    return Synth::submit_external_midi_event(event);
}

static void init_osc_graph_widget();

// Fixed nodes are structural: the MIDI input nodes and the oscillator sum
// node cannot be deleted.  Oscillator layer nodes remove their layer and
// generator nodes are freely deletable, so neither is vetoed - except the
// last remaining oscillator layer: an instrument needs one layer, and a
// commit-time refusal would delete the node for one frame and resurrect it.
static bool osc_node_delete_veto(void* user_data, uint32_t node_idx)
{
    OscDeleteVetoContext* context = static_cast<OscDeleteVetoContext*>(user_data);
    // The context is the file-static initializer above: both pointers are
    // set once and never cleared, and delete_node vetoes only occupied
    // nodes, so there is nothing to null-check here.
    const Sculptor::OscGraphMapping* mapping = context->mapping;
    if (mapping->input_node == node_idx) {
        return true;
    }
    if (mapping->output_node == node_idx) {
        return true;
    }
    if (mapping->osc_nodes[0] == node_idx && mapping->osc_nodes[1] == Sculptor::pool_no_slot) {
        context->graph->set_error("The last oscillator layer cannot be deleted");
        return true;
    }
    return false;
}

// The canvas menu is fully caller-provided; these items cover every node
// kind the graph can express.
static void osc_canvas_menu(void* user_data)
{
    (void)user_data;
    if (ImGui::MenuItem("Add Oscillator")) {
        osc_canvas_command = osc_cmd_add_oscillator;
    }
    if (ImGui::MenuItem("Add Envelope")) {
        osc_canvas_command = osc_cmd_add_envelope;
    }
    if (ImGui::MenuItem("Add LFO")) {
        osc_canvas_command = osc_cmd_add_lfo;
    }
    if (ImGui::MenuItem("Add Parameter")) {
        osc_canvas_command = osc_cmd_add_parameter;
    }
}

// Node context menu: a parameter node gains a Change Target submenu of the
// five connectable targets (the target re-key; free-text renaming lives in
// the title editor).  A derived parameter (uid == 0) cannot change target: the
// generator bindings would re-derive the old parameter.
static bool osc_node_menu(void* user_data, uint32_t node_idx)
{
    (void)user_data;
    if (node_idx >= Sculptor::max_nodes || ! osc_graph.node_occupied(node_idx)) {
        return false;
    }
    for (uint32_t p = 0; p < osc_mapping.param_count; ++p) {
        if (osc_mapping.params[p].node_idx != node_idx) {
            continue;
        }
        const Sculptor::ParamEntry& param = osc_mapping.params[p];
        ImGui::BeginDisabled(param.uid == 0);
        if (ImGui::BeginMenu("Change Target")) {
            for (uint32_t target = 0; target < 5; ++target) {
                if (ImGui::MenuItem(Sculptor::param_target_names[target], nullptr, false, target != param.target)) {
                    osc_canvas_retarget_node   = node_idx;
                    osc_canvas_retarget_target = target;
                    osc_canvas_command         = osc_cmd_change_target_param;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        return true;
    }
    return false;
}

// The effects projection vetoes only its fixed endpoints; effect nodes are
// freely deletable (the drain removes the chain slot) and LFO node deletion
// clears this chain's references to the descriptor.
static bool fx_node_delete_veto(void* user_data, uint32_t node_idx)
{
    (void)user_data;
    return node_idx == fx_mapping.input_node || node_idx == fx_mapping.output_node || node_idx == fx_mapping.midi_node;
}

// Connection grammar for the effects graph: a parameter dot takes an LFO's
// Value wire (the wire is the binding), a serial In dot takes audio from
// the fixed input or an effect's Out.  Everything else refuses with a
// message naming the rule it broke.
static bool fx_connection_validator(void*              user_data,
                                    Sculptor::Graph&   graph,
                                    Sculptor::EndPoint output,
                                    Sculptor::EndPoint input)
{
    (void)user_data;
    if (output.node_idx == input.node_idx) {
        graph.set_error("A node cannot connect to itself");
        return false;
    }
    const int32_t in_slot = Sculptor::fx_effect_slot_of(fx_mapping, input.node_idx);
    if (in_slot >= 0 && input.slot_idx >= Sculptor::fx_param_lfo_dot(0) &&
        (input.slot_idx - Sculptor::fx_param_lfo_dot(0)) % Sculptor::fx_param_stride == 0) {
        if (Sculptor::fx_lfo_desc_of(fx_mapping, output.node_idx) == 0) {
            graph.set_error("Only an LFO can modulate a parameter");
            return false;
        }
        return true;
    }
    const uint32_t param_field = input.slot_idx >= Sculptor::fx_param_row(0)
                                     ? (input.slot_idx - Sculptor::fx_param_row(0)) % Sculptor::fx_param_stride
                                     : Sculptor::fx_param_stride;
    if (in_slot >= 0 && (param_field == 4 || param_field == 7)) {
        if (output.node_idx != fx_mapping.midi_node) {
            graph.set_error("Effects take channel-wide MIDI sources only");
            return false;
        }
        return true;
    }
    const bool serial_in = input.node_idx == fx_mapping.output_node || (in_slot >= 0 && input.slot_idx == 0);
    if (! serial_in) {
        return false;
    }
    const bool serial_out = output.node_idx == fx_mapping.input_node ||
                            (Sculptor::fx_effect_slot_of(fx_mapping, output.node_idx) >= 0 && output.slot_idx == 1);
    if (! serial_out) {
        graph.set_error("The chain wire carries audio only");
        return false;
    }
    return true;
}

// Canvas menu: the six real effect types, capped by the chain length.
static void fx_canvas_menu(void* user_data)
{
    (void)user_data;
    const Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&instr_bank.bank, fx_mapping.chain);
    ImGui::BeginDisabled(chain.num_effects >= Synth::max_chain_effects);
    for (uint32_t type = 1; type < Synth::num_effect_types; ++type) {
        const Sculptor::EffectTypeInfo& info = Sculptor::effect_type_info(static_cast<Synth::EffectType>(type));
        if (ImGui::MenuItem(info.name)) {
            fx_canvas_command = fx_cmd_add_effect;
            fx_canvas_type    = static_cast<Synth::EffectType>(type);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::MenuItem("Add LFO")) {
        fx_canvas_command = fx_cmd_add_lfo;
    }
}

// Canvas menu "Add LFO": a fresh descriptor the editor pins onto this
// canvas.  The projection shows referenced descriptors only, so a
// not-yet-wired LFO needs its pin to stay visible and receive a first wire.
// Node menu on effect nodes: type replacement.  Chain order is a wire
// gesture (drag the connector), LFO nodes carry only their own rows, so
// the callback reports no items for them.
static bool fx_node_menu(void* user_data, uint32_t node_idx)
{
    (void)user_data;
    const int32_t slot = Sculptor::fx_effect_slot_of(fx_mapping, node_idx);
    if (slot < 0) {
        return false;
    }
    const Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&instr_bank.bank, fx_mapping.chain);
    if (ImGui::BeginMenu("Change Type")) {
        for (uint32_t type = 1; type < Synth::num_effect_types; ++type) {
            const bool is_current = chain.effects[slot].type == static_cast<Synth::EffectType>(type);
            if (ImGui::MenuItem(Sculptor::effect_type_info(static_cast<Synth::EffectType>(type)).name,
                                nullptr,
                                false,
                                ! is_current)) {
                fx_canvas_command = fx_cmd_change_type;
                fx_canvas_type    = static_cast<Synth::EffectType>(type);
                fx_menu_node      = node_idx;
            }
        }
        ImGui::EndMenu();
    }
    return true;
}

// Whether an occupied node's live layout still matches the projected
// snapshot: a node the user has not touched needs no layout record.
static bool node_layout_moved(uint32_t node)
{
    const Sculptor::Node& node_ref = osc_graph.node(node);
    return node_ref.position.x != projected_position[node].x || node_ref.position.y != projected_position[node].y ||
           node_ref.content_width_override != projected_width[node] ||
           node_ref.content_height_override != projected_height[node];
}

// Re-bases one node's snapshot on its live layout, after a commit or a
// re-projection.
static void snapshot_node_layout(uint32_t node)
{
    const Sculptor::Node& node_ref = osc_graph.node(node);
    projected_position[node]       = node_ref.position;
    projected_width[node]          = node_ref.content_width_override;
    projected_height[node]         = node_ref.content_height_override;
}

// Rebuilds the graph for one zone from the committed bank and drains the
// projection's own construction traffic, so projection events never echo
// into commits.  Also clears the undo group tag: a re-projected graph starts
// a new edit context.

// Resolves a graph node to its envelope descriptor in the committed bank,
// nullptr when the node is not a live envelope instance.  Re-resolved every
// frame so envelope pool compaction cannot leave the widget pointing at a
// recycled descriptor.
static const Synth::EnvelopeDescriptor* envelope_widget_descriptor(uint32_t node_idx, uint32_t* out_desc_id)
{
    for (uint32_t i = 0; i < osc_mapping.detached_count; ++i) {
        const Sculptor::DetachedNode& detached = osc_mapping.detached[i];
        if (detached.node_idx != node_idx || detached.kind != 1 || detached.desc_id == 0 ||
            detached.desc_id > instr_bank.bank.envelopes.num_allocated) {
            continue;
        }
        *out_desc_id = detached.desc_id;
        return &instr_bank.bank.envelopes.entries[detached.desc_id - 1];
    }
    return nullptr;
}

static void init_osc_graph_widget()
{
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    osc_graph.set_colors(Sculptor::default_graph_colors());
    // The apply path drops color/ghost events (projection state), so the
    // widget must not offer those edits.  Titles regenerate on re-projection
    // for every node except parameters (their record stores the name), so
    // the projection opts parameter nodes into the title editor one by one
    // through the per-node renamable flag.
    osc_graph.node_state_edits_enabled = false;
}

// Lowest live node whose layout no longer matches what the projection
// built, with the number of moved nodes; pool_no_slot when the layout is
// unchanged.  One moved node tags per node, so moving node A then node B
// stays two undo entries while one drag gesture coalesces through the
// unchanged tag; a multi-node drag takes a fresh unique tag, so a later
// single-node move of the lowest node cannot amend the multi-node entry.
static uint32_t graph_moved_node(uint32_t* moved_count)
{
    *moved_count    = 0;
    uint32_t lowest = Sculptor::pool_no_slot;
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (! osc_graph.node_occupied(node) || ! node_layout_moved(node)) {
            continue;
        }
        ++*moved_count;
        if (lowest == Sculptor::pool_no_slot) {
            lowest = node;
        }
    }
    return lowest;
}

// Re-bases the projected-layout snapshot on the live graph after a commit, so
// an unchanged layout never commits twice.
static void refresh_projected_layout()
{
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (osc_graph.node_occupied(node)) {
            snapshot_node_layout(node);
        }
    }
}

// Moves one existing layout record onto a node's live position.
static void write_record_position(Synth::InstrumentEditorBank* bank, int32_t record_idx, const Sculptor::Node& node_ref)
{
    Synth::GraphNodeLayout& record = bank->graph_layout[record_idx];
    record.x                       = node_ref.position.x;
    record.y                       = node_ref.position.y;
    record.width_override          = node_ref.content_width_override;
    record.height_override         = node_ref.content_height_override;
}

// Writes every mapped node's current layout into the bank's records: bound
// nodes key kind-0 records by canonical index, detached nodes update their
// own record in place.  A node still at its projected layout writes nothing,
// so records stay sparse.  Returns false when a moved node would need a new
// record but the global record list is full.
static bool sync_osc_graph_layout(Synth::InstrumentEditorBank* bank)
{
    const uint32_t channel = osc_graph_zone_channel;
    const uint32_t zone    = osc_graph_zone_index;
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (! osc_graph.node_occupied(node)) {
            continue;
        }
        const Sculptor::Node& node_ref = osc_graph.node(node);
        const bool            moved    = node_layout_moved(node);
        if (! moved) {
            continue;
        }
        const uint32_t canonical = Sculptor::osc_graph_canonical_index(osc_mapping, node);
        if (canonical != Sculptor::pool_no_slot) {
            const int32_t record_idx = Sculptor::find_record(*bank, channel, zone, 0, canonical, 0);
            if (record_idx >= 0) {
                write_record_position(bank, record_idx, node_ref);
                continue;
            }
            if (! Sculptor::graph_records_have_capacity(*bank, 1)) {
                return false;
            }
            Synth::GraphNodeLayout record                  = {};
            record.channel                                 = static_cast<uint8_t>(channel);
            record.zone                                    = static_cast<uint8_t>(zone);
            record.kind                                    = 0;
            record.index                                   = static_cast<uint8_t>(canonical);
            record.x                                       = node_ref.position.x;
            record.y                                       = node_ref.position.y;
            record.width_override                          = node_ref.content_width_override;
            record.height_override                         = node_ref.content_height_override;
            bank->graph_layout[bank->graph_layout_count++] = record;
            continue;
        }
        // Detached nodes carry their own record; its position follows the
        // node so a re-projection keeps the move.  A record-less derived
        // instance that the user moves gains its kind-1/2 record (fresh uid,
        // live source wires) so the move survives re-projection too.
        for (uint32_t i = 0; i < osc_mapping.detached_count; ++i) {
            const Sculptor::DetachedNode& entry = osc_mapping.detached[i];
            if (entry.node_idx != node) {
                continue;
            }
            int32_t record_idx = Sculptor::find_record(*bank, channel, zone, entry.kind, entry.desc_id, entry.uid);
            if (record_idx < 0) {
                if (entry.uid != 0) {
                    break; // record vanished under a live uid: nothing to update
                }
                if (! Sculptor::detach_osc_graph_instance(bank, osc_graph, &osc_mapping, channel, zone, i)) {
                    return false;
                }
                break;
            }
            write_record_position(bank, record_idx, node_ref);
            break;
        }

        // Parameter nodes carry their own kind-3 record; a record-less
        // (derived) parameter that the user moves gains one - seeded from
        // its live wiring, like a detach - so the move survives
        // re-projection.  Refused while an earlier-enumerated same-target
        // derived parameter is still record-less: the new record would
        // attach to that sibling positionally at re-projection and hijack
        // its node, so the node keeps no record and the move stays
        // session-only.
        for (uint32_t i = 0; i < osc_mapping.param_count; ++i) {
            const Sculptor::ParamEntry& param = osc_mapping.params[i];
            if (param.node_idx != node) {
                continue;
            }
            int32_t record_idx = Sculptor::find_record(*bank, channel, zone, 3, param.target, param.uid);
            if (record_idx < 0 && ! Sculptor::param_has_recordless_predecessor(osc_mapping, i)) {
                if (! Sculptor::graph_records_have_capacity(*bank, 1)) {
                    return false;
                }
                Synth::GraphNodeLayout record = {};
                record.channel                = static_cast<uint8_t>(channel);
                record.zone                   = static_cast<uint8_t>(zone);
                record.kind                   = 3;
                record.index                  = param.target;
                record.param_slot             = Sculptor::param_group_ordinal(osc_mapping, i);
                if (! Sculptor::store_param_wiring(osc_graph, osc_mapping, param, &record)) {
                    return false;
                }
                record.uid = Sculptor::allocate_detached_uid(*bank, channel, zone, 3);
                bank->graph_layout[bank->graph_layout_count++] = record;
                // Write the fresh uid back into the mapping so a later move
                // updates this record instead of appending duplicates.
                osc_mapping.params[i].uid = record.uid;
                record_idx                = static_cast<int32_t>(bank->graph_layout_count - 1);
                // The naive ordinal above can miscount dormant and merged
                // siblings; the re-stamp inside the refresh replaces it with
                // the derived truth before the record is ever read back.
                Sculptor::refresh_osc_graph_compilation(bank, osc_graph, osc_mapping, channel, zone);
                // The re-stamp's merge reconciliation can remove records,
                // shifting graph_layout indices; re-derive this record's
                // slot by key instead of trusting the pre-refresh index.
                record_idx = Sculptor::find_record(*bank, channel, zone, 3, record.index, record.uid);
            }
            else if (record_idx < 0) {
                Sculptor::notify_error("Synth: the earlier parameter of this target must be renamed first");
            }
            if (record_idx >= 0) {
                write_record_position(bank, record_idx, node_ref);
            }
        }
    }
    return true;
}

static void undo_init_once()
{
    static bool inited = false;

    if (! inited) {
        undo_redo.init(undo_buf);
        inited = true;
    }
}

static bool pump_bank_publish()
{
    if (bank_changes_pending) {
        if (! Synth::push_bank_update(&bank_queue, pending_bank)) {
            return false;
        }

        bank_changes_pending = false;
    }

    return true;
}

static bool publish_edited_bank()
{
    if (! Synth::validate_instrument_bank(&instr_bank.bank)) {
        Sculptor::notify_error("Synth: refusing to publish an invalid instrument bank");
        return false;
    }

    pending_bank = instr_bank.bank; // names are editor-only and never reach the audio thread
    // A zone with a broken oscillator sum publishes its channel disabled so
    // notes cannot trigger it; the stored channel_enabled values stay 1.
    uint8_t publish_enabled[Synth::max_channels];
    Sculptor::compute_publish_channel_enabled(instr_bank, publish_enabled);
    memcpy(pending_bank.channel_enabled, publish_enabled, sizeof(publish_enabled));
    bank_changes_pending = true;

    // The queue push is the last step on the sound path: the audio thread
    // consumes this bank at its next step, mid-drag frames included.  The
    // file save trails in delayed_updates, off this path.
    save_pending         = true;
    frames_since_publish = 0;

    return pump_bank_publish();
}

static void drain_bank_updates()
{
    while (const Synth::InstrumentBank* const packet = Synth::peek_bank_update(&bank_queue)) {
        Synth::set_current_bank(*packet);
        Synth::consume_bank_update(&bank_queue);
    }
}

// The origin packs the channel and pane the edit was made in; undo/redo
// refocus them so a whole-bank restore is never silent about what changed.
static void editor_snapshot(uint32_t origin)
{
    undo_init_once();

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.push(origin);
    undo_redo.finish_undo_push();

    undo_redo.clear_redo();
}

// The entry's origin says where its edit was made: the redo entry this
// creates replays that edit, and the caller refocuses that channel and
// pane so a whole-bank restore is never silent about what changed.
static bool editor_undo(uint32_t* origin)
{
    undo_init_once();

    if (undo_redo.undo_empty())
        return false;

    const Sculptor::UndoRedo::Snapshot snap         = undo_redo.get_snapshot();
    uint32_t                           entry_origin = 0;
    memcpy(&entry_origin, snap.buf + sizeof(instr_bank), sizeof entry_origin);

    undo_redo.init_redo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.push(entry_origin);
    if (! undo_redo.finish_redo_push())
        return false;

    undo_redo.init_undo();
    undo_redo.pop_u32();
    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo();

    if (origin)
        *origin = entry_origin;

    // Backpressure only defers the audible switch; the restoration itself
    // succeeded, so the caller must resynchronize selections either way.
    publish_edited_bank();
    return true;
}

// A redo entry's payload reads forward, so its origin sits at the start:
// the edit it replays becomes visible at that origin again.
static bool editor_redo(uint32_t* origin)
{
    undo_init_once();

    if (undo_redo.redo_empty())
        return false;

    const Sculptor::UndoRedo::Snapshot snap         = undo_redo.get_redo_snapshot();
    uint32_t                           entry_origin = 0;
    memcpy(&entry_origin, snap.buf, sizeof entry_origin);

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.push(entry_origin);
    if (! undo_redo.finish_undo_push())
        return false;

    if (! undo_redo.init_redo())
        return false;

    undo_redo.pop_u32();
    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_redo();

    if (origin)
        *origin = entry_origin;

    // Backpressure only defers the audible switch; the restoration itself
    // succeeded, so the caller must resynchronize selections either way.
    publish_edited_bank();
    return true;
}

// Loads a bank file into the editable bank.  A decode is transactional (the decoder
// stages and validates before committing), so a corrupt file never leaves the
// editable bank half-replaced.  The only caller is startup, where a missing file
// is a fresh project and stays silent.
static bool load_editor_bank(const char* path)
{
    static Synth::InstrumentEditorBank scratch;

    const Synth::BankFileStatus status = Synth::load_editor_bank_file(path, &scratch);

    if (status == Synth::BankFileStatus::absent)
        return false;

    if (status == Synth::BankFileStatus::too_large) {
        Sculptor::notify_error("Synth: bank file too large to parse: %s", path);
        return false;
    }

    if (status == Synth::BankFileStatus::invalid) {
        Sculptor::notify_error("Synth: bank load failed: %s", path);
        return false;
    }

    if (! Synth::validate_instrument_bank(&scratch.bank)) {
        Sculptor::notify_error("Synth: refusing to load an invalid instrument bank");
        return false;
    }

    instr_bank          = scratch; // names ride in the bank file
    osc_graph_reproject = true;
    fx_graph_reproject  = true;
    return publish_edited_bank();
}

static void init_editor()
{
    static_assert(std::is_trivially_copyable_v<Synth::InstrumentEditorBank>);

    // The player starts with an empty, silent bank; the editor restores the last
    // session or builds the bare default bank for a fresh project and publishes it.
    Synth::set_bank_source_callback(&drain_bank_updates);

    if (load_editor_bank(bank_state_path))
        return;

    Synth::init_default_bank(&instr_bank.bank);

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++)
        Synth::get_default_channel_name(channel, instr_bank.channel_names[channel], Synth::max_name_len);

    publish_edited_bank();
}

static uint32_t zone_count(const Synth::InstrumentBank& bank, uint32_t channel)
{
    uint32_t num = 0;
    while (num < Synth::max_instr_per_channel && bank.channel_zones[channel][num].start_note)
        ++num;
    return num;
}

// The zone whose instrument Save/Save As write: the selected zone, or the note-0 zone
// when the selection is out of range. pool_no_slot for an empty or disabled channel.
static uint32_t save_zone_entry(const Synth::InstrumentBank& bank, uint32_t channel, int32_t selected)
{
    if (! bank.channel_enabled[channel])
        return pool_no_slot;
    const uint32_t num_zones = zone_count(bank, channel);

    if (num_zones == 0)
        return pool_no_slot;
    if (selected < 0 || selected >= static_cast<int32_t>(num_zones))
        return 0;
    return static_cast<uint32_t>(selected);
}

static constexpr bool is_black_note(uint32_t note)
{
    switch (note % 12) {
        case 1:
        case 3:
        case 6:
        case 8:
        case 10:
            return true;
        default:
            return false;
    }
}

// Black keys below pitch class p within its octave: C# D# F# G# A# = pcs 1 3 6 8 10.
static constexpr uint32_t black_notes_below_pc(uint32_t pc)
{
    switch (pc) {
        case 0:
        case 1:
            return 0;
        case 2:
        case 3:
            return 1;
        case 4:
        case 5:
        case 6:
            return 2;
        case 7:
        case 8:
            return 3;
        case 9:
        case 10:
            return 4;
        default:
            return 5;
    }
}

// 0-based index (0..74) of the white key a note is drawn on.  For black notes this is
// the white key to the note's right; the black key is drawn straddling the boundary to
// its left, so this also locates that boundary.
static constexpr uint32_t white_index_of(uint32_t note)
{
    return note - 5u * (note / 12u) - black_notes_below_pc(note % 12u);
}

// The note drawn on white key wk (0..74).
static constexpr uint32_t note_at_white(uint32_t wk)
{
    constexpr uint32_t white_in_octave[7] = { 0, 2, 4, 5, 7, 9, 11 };
    return 12u * (wk / 7u) + white_in_octave[wk % 7u];
}

// Left edge of the key (white or black) that starts at `note`, relative to the
// keyboard's origin; zone boundaries are drawn here.
static float boundary_x_of(uint32_t note, float white_w, float black_w)
{
    float x = static_cast<float>(white_index_of(note)) * white_w;
    if (is_black_note(note))
        x -= black_w * 0.5f;
    return x;
}

// The chain's kind-4 record carrying a projected node's saved place, -1
// when the node never moved.
static int32_t find_fx_layout_record(const Synth::InstrumentEditorBank& bank, uint32_t chain, const char* name)
{
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.kind == 4 && record.channel == chain && strcmp(record.name, name) == 0) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Writes every moved fx node's place into the bank's kind-4 records: node
// places ride the bank, so re-projections, undo/redo and file saves all
// restore them. Returns false when a moved node would need a new record
// but the global record list is full.
static bool sync_fx_graph_layout(Synth::InstrumentEditorBank* bank)
{
    const uint32_t chain = fx_mapping.chain;
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (! osc_graph.node_occupied(node) || ! node_layout_moved(node)) {
            continue;
        }
        const Sculptor::Node& node_ref   = osc_graph.node(node);
        const int32_t         record_idx = find_fx_layout_record(*bank, chain, node_ref.name);
        if (record_idx >= 0) {
            write_record_position(bank, record_idx, node_ref);
            continue;
        }
        if (! Sculptor::graph_records_have_capacity(*bank, 1)) {
            return false;
        }
        Synth::GraphNodeLayout record = {};
        record.channel                = static_cast<uint8_t>(chain);
        record.kind                   = 4;
        snprintf(record.name, sizeof(record.name), "%s", node_ref.name);
        const int32_t fresh_idx                        = static_cast<int32_t>(bank->graph_layout_count);
        bank->graph_layout[bank->graph_layout_count++] = record;
        write_record_position(bank, fresh_idx, node_ref);
    }
    return true;
}

void Sculptor::SynthEditor::reproject_osc_graph(uint32_t channel, uint32_t zone)
{
    osc_graph_zone_channel = channel;
    osc_graph_zone_index   = zone;
    osc_graph_projected    = Sculptor::project_editor_to_graph(instr_bank, &osc_graph, &osc_mapping, channel, zone);
    ++osc_graph_generation;
    // The pane's connection grammar and menus install after the projection:
    // the widget may last have hosted the effects projection, and the
    // projection itself owns no callbacks.
    osc_graph.set_validator(&Sculptor::osc_graph_validate, &osc_mapping);
    osc_graph.set_delete_veto(&osc_node_delete_veto, &osc_veto_context);
    osc_graph.set_canvas_menu_callback(&osc_canvas_menu, nullptr);
    osc_graph.set_node_menu_callback(&osc_node_menu, nullptr);
    fx_graph_projected = false;
    // Rebind envelope state widgets from scratch: gestures and point
    // selections cannot outlive the graph they were made in.
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        EnvelopeWidgetContext& ctx = envelope_widget_contexts[node];
        ctx                        = {};
        ctx.node_idx               = node;
        ctx.editor                 = this;
        ctx.generation             = osc_graph_generation;
    }
    if (osc_graph_projected) {
        for (uint32_t i = 0; i < osc_mapping.detached_count; ++i) {
            const Sculptor::DetachedNode& detached = osc_mapping.detached[i];
            if (detached.kind == 1 && detached.node_idx != Sculptor::pool_no_slot && detached.desc_id != 0) {
                envelope_widget_contexts[detached.node_idx].node_idx        = detached.node_idx;
                envelope_widget_contexts[detached.node_idx].gesture_desc_id = detached.desc_id;
                osc_graph.set_state_widget(detached.node_idx,
                                           &envelope_state_widget_entry,
                                           &envelope_widget_contexts[detached.node_idx],
                                           static_cast<float>(Sculptor::envelope_widget_height()));
            }
        }
        for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
            if (osc_graph.node_occupied(node)) {
                snapshot_node_layout(node);
            }
        }
    }
    // Drain until the ring is empty: a projection can queue more events
    // than one batch holds, and leftovers would leak into the next frame's
    // apply batch.  The buffer is sized to the ring so one pass always drains.
    Sculptor::GraphChange discard[Sculptor::max_pending_changes];
    while (osc_graph.take_changes(discard, Sculptor::max_pending_changes) != 0) {
    }
    (void)osc_graph.changes_overflowed();
    Sculptor::undo_group_reset(&osc_undo_group);
}

// State-widget entry for envelope nodes: draws the curve widget and turns
// its edit requests into constrained descriptor edits committed through the
// normal candidate path.  A drag or an edit box is ONE gesture: one undo tag
// captured at its first edit, per-frame commits while it lasts.
int Sculptor::SynthEditor::envelope_state_widget_entry(void* user_data, float render_scale)
{
    EnvelopeWidgetContext*           ctx     = static_cast<EnvelopeWidgetContext*>(user_data);
    uint32_t                         desc_id = 0;
    const Synth::EnvelopeDescriptor* env     = envelope_widget_descriptor(ctx->node_idx, &desc_id);
    if (env == nullptr) {
        return 0;
    }
    const int widget_height = Sculptor::envelope_widget_height(render_scale);
    // Envelopes whose positions overlap (reachable only from hand-built JSON
    // banks) are read-only: no edit can produce a valid descriptor from them.
    const bool placed = Sculptor::env_positions_strictly_increasing(*env);
    // An envelope has exactly one output and no wire-independent knobs: with
    // nothing consuming that output its curve editor cannot affect the sound.
    const bool unwired     = ! osc_graph.slot_is_connected(ctx->node_idx, osc_mapping.env_output_slot);
    const bool interactive = osc_graph.is_selected(ctx->node_idx) && placed && ! unwired;
    if (osc_graph.is_selected(ctx->node_idx) && ! placed && ! ctx->ui.read_only_notified) {
        ctx->ui.read_only_notified = true;
        Sculptor::notify_warning("Synth: envelope points overlap; the curve is read-only");
    }
    if (osc_graph.is_selected(ctx->node_idx) && unwired && ! ctx->ui.unwired_notified) {
        ctx->ui.unwired_notified = true;
        Sculptor::notify_warning("Synth: envelope is not wired to a parameter; connect it to edit");
    }
    Sculptor::EnvelopeCurveEdit edit;
    Sculptor::gui_envelope_curve(&ctx->ui,
                                 *env,
                                 interactive,
                                 &edit,
                                 render_scale,
                                 osc_graph.is_selected(ctx->node_idx) && ! interactive);
    if (edit.kind == Sculptor::EnvelopeEditKind::none) {
        return widget_height;
    }
    if (edit.kind == Sculptor::EnvelopeEditKind::select_node) {
        // Eventless selection: no undo entry, no drag, no menu.
        osc_graph.set_selected(ctx->node_idx, true);
        return widget_height;
    }
    if (edit.kind == Sculptor::EnvelopeEditKind::gesture_end) {
        ctx->gesture_active = false;
        return widget_height;
    }

    // A re-projection or a remapped descriptor id invalidates the gesture:
    // abort without a final commit, the graph no longer matches what the
    // user started dragging.
    if (ctx->gesture_active && (ctx->generation != osc_graph_generation || ctx->gesture_desc_id != desc_id)) {
        ctx->gesture_active = false;
        return widget_height;
    }

    const bool gesture_edit = edit.kind == Sculptor::EnvelopeEditKind::move ||
                              edit.kind == Sculptor::EnvelopeEditKind::set_position ||
                              edit.kind == Sculptor::EnvelopeEditKind::set_value;
    if (gesture_edit && ! ctx->gesture_active) {
        ctx->gesture_start   = *env;
        ctx->gesture_desc_id = desc_id;
        ctx->generation      = osc_graph_generation;
        ctx->gesture_tag     = Sculptor::UndoGroupTag{ osc_tag_fresh, ++osc_fresh_tag_serial, 0 };
        Sculptor::undo_group_reset(&osc_undo_group);
        ctx->gesture_active = true;
    }

    // Drag edits apply to the drag-start copy so per-frame commits never
    // feed back into the target position; typed edits apply to the current
    // committed descriptor, each keystroke restating the absolute value.
    const Synth::EnvelopeDescriptor& source = edit.kind == Sculptor::EnvelopeEditKind::move ? ctx->gesture_start : *env;
    Synth::EnvelopeDescriptor        edited;
    bool                             ok = false;
    switch (edit.kind) {
        case Sculptor::EnvelopeEditKind::move:
            ok = Sculptor::env_move_point(source, edit.point_idx, edit.position, edit.value, &edited);
            break;
        case Sculptor::EnvelopeEditKind::set_position:
            ok = Sculptor::env_move_point(*env,
                                          edit.point_idx,
                                          edit.position,
                                          env->points[edit.point_idx].value,
                                          &edited);
            break;
        case Sculptor::EnvelopeEditKind::set_value:
            ok = Sculptor::env_move_point(*env,
                                          edit.point_idx,
                                          env->points[edit.point_idx].position,
                                          edit.value,
                                          &edited);
            break;
        case Sculptor::EnvelopeEditKind::insert_before:
            ok = Sculptor::env_insert_point(*env, edit.point_idx, false, &edited);
            break;
        case Sculptor::EnvelopeEditKind::insert_after:
            ok = Sculptor::env_insert_point(*env, edit.point_idx, true, &edited);
            break;
        case Sculptor::EnvelopeEditKind::remove:
            ok = Sculptor::env_remove_point(*env, edit.point_idx, &edited);
            break;
        case Sculptor::EnvelopeEditKind::sustain_first:
            ok = Sculptor::env_set_sustain_first(*env, edit.point_idx, &edited);
            break;
        case Sculptor::EnvelopeEditKind::sustain_last:
            ok = Sculptor::env_set_sustain_last(*env, edit.point_idx, &edited);
            break;
        case Sculptor::EnvelopeEditKind::none:
        case Sculptor::EnvelopeEditKind::gesture_end:
        case Sculptor::EnvelopeEditKind::select_node:
            break;
    }
    if (! ok) {
        if (gesture_edit) {
            // A refused mid-gesture edit must not coalesce into the next
            // gesture; end the gesture without publishing.
            ctx->gesture_active = false;
            Sculptor::undo_group_reset(&osc_undo_group);
        }
        else {
            const bool insert_edit = edit.kind == Sculptor::EnvelopeEditKind::insert_before ||
                                     edit.kind == Sculptor::EnvelopeEditKind::insert_after;
            Sculptor::notify_warning(insert_edit ? "Synth: no room for another envelope point here"
                                                 : "Synth: envelope edit rejected");
        }
        return widget_height;
    }

    // A volume envelope keeps the volume shape: minimum 0, first and last
    // point 0.  The constraint follows the descriptor's volume wiring, so
    // every node aliasing the descriptor is constrained the same way.
    bool volume_used = false;
    bool other_used  = false;
    Sculptor::env_target_usage(osc_graph, osc_mapping, static_cast<uint16_t>(desc_id), &volume_used, &other_used);
    if (volume_used && ! Sculptor::env_volume_shape_ok(edited)) {
        if (gesture_edit) {
            ctx->gesture_active = false;
            Sculptor::undo_group_reset(&osc_undo_group);
        }
        Sculptor::notify_warning(
            "Synth: volume envelope keeps the first and the last point at 0; unwire it from Volume to edit freely");
        return widget_height;
    }

    candidate                                     = instr_bank;
    candidate.bank.envelopes.entries[desc_id - 1] = edited;
    const Sculptor::UndoGroupTag tag =
        gesture_edit ? ctx->gesture_tag : Sculptor::UndoGroupTag{ osc_tag_fresh, ++osc_fresh_tag_serial, 0 };
    if (! ctx->editor->commit_candidate(candidate, tag)) {
        Sculptor::undo_group_reset(&osc_undo_group);
        ctx->gesture_active = false;
        return widget_height;
    }

    // Structural edits move the selection onto the affected point.
    switch (edit.kind) {
        case Sculptor::EnvelopeEditKind::insert_before:
            ctx->ui.selected_point = static_cast<int32_t>(edit.point_idx);
            break;
        case Sculptor::EnvelopeEditKind::insert_after:
            ctx->ui.selected_point = static_cast<int32_t>(edit.point_idx + 1);
            break;
        case Sculptor::EnvelopeEditKind::remove:
            if (ctx->ui.selected_point > static_cast<int32_t>(edit.point_idx)) {
                --ctx->ui.selected_point;
            }
            else if (ctx->ui.selected_point == static_cast<int32_t>(edit.point_idx)) {
                ctx->ui.selected_point = static_cast<int32_t>(edit.point_idx) - 1;
            }
            break;
        case Sculptor::EnvelopeEditKind::sustain_first:
        case Sculptor::EnvelopeEditKind::sustain_last:
            if (ctx->ui.selected_point >= static_cast<int32_t>(edited.num_points)) {
                ctx->ui.selected_point = static_cast<int32_t>(edited.num_points) - 1;
            }
            break;
        default:
            break;
    }
    return widget_height;
}

Sculptor::SynthEditor::SynthEditor()
{
    for (int32_t& zone : selected_zone)
        zone = -1;
}

bool Sculptor::SynthEditor::allocate_resources()
{
    // Viewport (re)allocation revisits enabled editors; the bank snapshot must be
    // taken once, otherwise a revisit would wipe unsaved edits with the embedded bank.
    if (! editor_bank_initialized) {
        editor_bank_initialized = true;
        init_editor();
    }
    return true;
}

void Sculptor::SynthEditor::delayed_updates()
{
    // Runs from sculptor.cpp before the editors loop, regardless of the
    // enabled flag: a disabled editor must still publish pending banks.
    pump_bank_publish();

    if (save_pending) {
        // Publishes happen during the GUI frame; two passes without a new
        // publish mean the edit gesture ended, and the bank goes to disk.
        ++frames_since_publish;
        if (frames_since_publish >= 2) {
            save_pending         = false;
            const int save_error = Synth::save_editor_bank_file(bank_state_path, &instr_bank);
            if (save_error)
                Sculptor::notify_error("Synth: cannot write %s: %s", bank_state_path, strerror(save_error));
        }
    }
}

void Sculptor::SynthEditor::rederive_zone_selection(uint32_t channel)
{
    const Synth::InstrumentBank& bank = instr_bank.bank;
    // Empty/disabled channels hold no selection: slot 0 of a cleared table identifies an
    // unallocated instrument, and a disabled channel's table may hold stale bytes.
    if (! bank.channel_enabled[channel] || bank.channel_zones[channel][0].start_note == 0) {
        selected_zone[channel] = -1;
        zone_tab_force_entry   = -1;
        return;
    }
    // The zone tab bar owns the selection; keep it while it still names a live
    // zone so commits (splits, joins, renames) do not move it.
    const int32_t current = selected_zone[channel];
    if (current >= 0 && static_cast<uint32_t>(current) < zone_count(bank, channel))
        return;
    int32_t zone = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], last_clicked_note[channel]));
    if (zone < 0)
        zone = static_cast<int32_t>(
            Synth::zone_entry_at(bank.channel_zones[channel], 0)); // zone 0 always starts at note 0
    selected_zone[channel] = zone;
    zone_tab_force_entry   = zone;
}

// Undo and redo restore whole-bank snapshots that are otherwise silent about
// what changed, so the restored entry's origin refocuses the channel and
// pane where the change becomes visible.
void Sculptor::SynthEditor::apply_edit_origin(uint32_t origin)
{
    selected_target   = origin >> 1;
    show_effects_mode = (origin & 1u) != 0;
    // The master pane has no tab bar, so a master-focused restore leaves the
    // channel pane's tabs untouched.
    effects_tab_pend = selected_target == target_master ? -1 : (show_effects_mode ? 1 : 0);
}

void Sculptor::SynthEditor::rederive_all_selections()
{
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++)
        rederive_zone_selection(channel);
}

// Commits a validated candidate over the editable bank.  The undo group tag
// decides whether the commit opens a new undo entry or amends the state of
// the previous same-tag commit; a refused commit remembers no tag, so the
// next commit always snapshots.
bool Sculptor::SynthEditor::commit_candidate(const Synth::InstrumentEditorBank& candidate_bank,
                                             Sculptor::UndoGroupTag             tag)
{
    if (! Synth::validate_instrument_bank(&candidate_bank.bank)) {
        Sculptor::notify_error("Synth: refusing to commit an invalid bank");
        return false;
    }
    if (! Sculptor::validate_editor_metadata(candidate_bank)) {
        Sculptor::notify_error("Synth: refusing to commit invalid graph state");
        return false;
    }

    if (Sculptor::undo_group_needs_snapshot(&osc_undo_group, tag)) {
        editor_snapshot((selected_target << 1) | (show_effects_mode ? 1u : 0u));
    }
    instr_bank = candidate_bank;

    if (! publish_edited_bank())
        d_printf("Synth: bank publish backpressured; will retry\n");

    rederive_all_selections();
    return true;
}

void Sculptor::SynthEditor::do_initialize(uint32_t channel)
{
    candidate = instr_bank;
    // Orphaned instruments may hold the only free pool slots, so reclaim before checking.
    Synth::reclaim_unused_slots(&candidate);

    if (! Synth::init_default_channel(&candidate.bank, channel)) {
        Sculptor::notify_error("Synth: cannot initialize channel %u: pool space exhausted", channel + 1);
        return;
    }

    candidate.bank.channel_enabled[channel] = 1;
    Synth::get_default_channel_name(channel, candidate.channel_names[channel], Synth::max_name_len);
    // The channel's old instruments are now unreferenced by its replacement zone table.
    Synth::reclaim_unused_slots(&candidate);
    // The channel's graph state is replaced together with its zone tables.
    Sculptor::channel_records_reset(&candidate, channel);

    selected_target = channel;
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_init, channel, 0 }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::do_delete(uint32_t channel)
{
    candidate = instr_bank;

    candidate.bank.channel_enabled[channel] = 0;
    Synth::get_default_channel_name(channel, candidate.channel_names[channel], Synth::max_name_len);
    memset(candidate.bank.channel_zones[channel], 0, sizeof(candidate.bank.channel_zones[channel]));
    memset(&candidate.bank.channel_chains[channel], 0, sizeof(candidate.bank.channel_chains[channel]));
    // Instruments the channel's old zone table referenced are now unreferenced.
    Synth::reclaim_unused_slots(&candidate);
    // The channel's graph state is replaced together with its zone tables.
    Sculptor::channel_records_reset(&candidate, channel);

    selected_target = channel;
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_delete, channel, 0 }))
        osc_graph_reproject = true;
    fx_graph_reproject = true;
}

void Sculptor::SynthEditor::release_held_note()
{
    if (! held_note_active)
        return;
    // Retry each frame until accepted; a dropped off would leave the note sounding.
    if (submit_note_event(held_note_channel, held_note, false)) {
        held_note_active = false;
    }
}

bool Sculptor::SynthEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    (void)image_idx;
    (void)input;
    *need_realloc = false;

    // Publish pumping lives in delayed_updates(), which runs every frame
    // regardless of the enabled flag.

    // The held note requires the left button to be down, so a release that
    // happened while this frame was not running (editor disabled, focus loss) is
    // caught here too. Window close releases at its own site; menu opens release
    // at their OpenPopup sites.
    if (! ImGui::IsMouseDown(ImGuiMouseButton_Left))
        release_held_note();

    if (! ImGui::Begin("Synth")) {
        release_held_note();
        ImGui::End();
        return true;
    }

    // Focus queries must run inside the Synth window's Begin scope: outside it
    // they compare against whatever window is current, not Synth. The ChildWindows
    // flag is required: the bare query compares NavWindow against the Synth root,
    // and after a keyboard click the focused window is the keyboard child, which
    // would release the held note every frame.
    if (! ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        release_held_note();

    // Undo/redo act only while this window is focused and no text field is being
    // edited; the geometry editor gates its own shortcuts the same way, so one
    // keystroke can never undo both editors.
    const ImGuiIO& io = ImGui::GetIO();
    // Child windows (the graph panes) hold focus while the user edits them,
    // so the shortcut must consider the whole Synth window tree - the same
    // ChildWindows rule the held-note release uses.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ! io.WantTextInput) {
        if (is_ctrl_down() && ! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            uint32_t origin = 0;
            if (editor_undo(&origin)) {
                apply_edit_origin(origin);
                osc_graph_reproject = true;
                fx_graph_reproject  = true;
                rederive_all_selections();
            }
        }
        if (is_ctrl_down() && ((! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
                               (is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
            uint32_t origin = 0;
            if (editor_redo(&origin)) {
                apply_edit_origin(origin);
                osc_graph_reproject = true;
                fx_graph_reproject  = true;
                rederive_all_selections();
            }
        }
    }

    ImGui::BeginTable("##synth_layout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable);
    ImGui::TableSetupColumn("##channels", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    // A stretch column claims the remaining width immediately; a fit column would
    // re-fit its width over several frames and make the pane look like it animates in.
    ImGui::TableSetupColumn("##pane", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    gui_channel_list();
    ImGui::TableSetColumnIndex(1);
    if (selected_target == target_master) {
        const float pane_h = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().CellPadding.y;
        ImGui::BeginChild("##synth_master_pane",
                          ImVec2(0, pane_h < 0.0f ? 0.0f : pane_h),
                          true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        gui_fx_graph(Synth::max_channels);
        ImGui::EndChild();
    }
    else
        gui_channel_pane(selected_target);
    ImGui::EndTable();

    gui_channel_popup();
    gui_zone_menu();
    gui_rename_popup();
    gui_library_popups();

    ImGui::End();
    return true;
}

void Sculptor::SynthEditor::gui_channel_list()
{
    const Synth::InstrumentBank& bank = instr_bank.bank;

    if (ImGui::Selectable("Master Effects", selected_target == target_master))
        selected_target = target_master;

    ImGui::Separator();

    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        const bool        enabled = bank.channel_enabled[channel];
        const char* const name    = instr_bank.channel_names[channel];

        if (enabled) {
            char item_id[Synth::max_name_len + 8];
            snprintf(item_id, sizeof(item_id), "%s##chan%u", name, channel);
            if (ImGui::Selectable(item_id, selected_target == channel)) {
                selected_target = channel;
                rederive_zone_selection(channel);
            }
        }
        else {
            // Disabled channels are not selectable; their table may hold stale bytes.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(name);
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            menu_channel      = channel;
            channel_menu_open = true;
        }
    }
}

void Sculptor::SynthEditor::gui_channel_pane(uint32_t channel)
{
    const Synth::InstrumentBank& bank      = instr_bank.bank;
    const uint32_t               num_zones = zone_count(bank, channel);

    if (! bank.channel_enabled[channel]) {
        ImGui::TextDisabled("Channel disabled - use Initialize or Load");
        return;
    }

    if (ImGui::BeginTabBar("synth_mode")) {
        // Undo/redo refocus the pane the restored change lives in.  The
        // pending selection survives the one frame ImGui still reports the
        // stale tab, then clears once the forced tab is shown.
        const ImGuiTabItemFlags osc_flags = effects_tab_pend == 0 ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags fx_flags  = effects_tab_pend == 1 ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Oscillators", nullptr, osc_flags)) {
            show_effects_mode = false;
            if (osc_flags) {
                effects_tab_pend = -1;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects", nullptr, fx_flags)) {
            show_effects_mode = true;
            if (fx_flags) {
                effects_tab_pend = -1;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // The channel effects chain is channel-wide state: it edits with no zone
    // selected, so it runs before the zone-table early returns. The keyboard
    // stays available so notes sound while the chain is edited.
    if (show_effects_mode) {
        float pane_h = ImGui::GetContentRegionAvail().y - 72.0f - ImGui::GetStyle().CellPadding.y -
                       ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("##synth_pane_ph",
                          ImVec2(0, pane_h < 0.0f ? 0.0f : pane_h),
                          true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        gui_fx_graph(channel);
        ImGui::EndChild();
        gui_keyboard();
        return;
    }

    if (num_zones == 0) {
        // The empty-selection state: no instrument is selected, so the name box
        // and zone mutations stay disabled for this target.
        if (bank.channel_enabled[channel])
            ImGui::TextDisabled("No zones - use Initialize or Load");
        else
            ImGui::TextDisabled("Channel disabled - use Initialize or Load");
        return;
    }

    int32_t zone = selected_zone[channel];
    if (zone < 0 || static_cast<uint32_t>(zone) >= num_zones) {
        // Defensive: re-derivation keeps this in range, but an out-of-range index would
        // read a freed pool slot.
        zone                   = 0;
        selected_zone[channel] = 0;
        zone_tab_force_entry   = 0;
    }

    // The tab bar owns the visible selection; selected_zone mirrors it so keyboard
    // clicks and rederivation stay in sync in both directions.
    if (ImGui::BeginTabBar("zone_tabs")) {
        for (uint32_t entry = 0; entry < num_zones; entry++) {
            char name[Synth::max_name_len];
            Synth::get_zone_name(&instr_bank, channel, entry, name, sizeof(name));
            char label[32];
            if (strlen(name) > 14)
                snprintf(label, sizeof(label), "%.11s...###zone%u", name, entry);
            else
                snprintf(label, sizeof(label), "%s###zone%u", name, entry);
            ImGuiTabItemFlags tab_flags = 0;
            if (zone_tab_force_entry == static_cast<int32_t>(entry))
                tab_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(label, nullptr, tab_flags)) {
                if (zone_tab_force_entry == static_cast<int32_t>(entry))
                    zone_tab_force_entry = -1; // one-shot consumed once the bar shows the tab
                selected_zone[channel] = static_cast<int32_t>(entry);
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                zone_menu_channel  = channel;
                zone_menu_note     = static_cast<uint32_t>(bank.channel_zones[channel][entry].start_note) - 1;
                zone_menu_from_tab = true;
                zone_menu_open     = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nfrom note %u",
                                  name,
                                  static_cast<uint32_t>(bank.channel_zones[channel][entry].start_note) - 1);
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();

    // The Oscillators view keeps a bottom strip for the keyboard.  Avail inside an
    // auto-height table cell is unbounded down to the window bottom, so subtract what
    // the layout adds below the placeholder: the cell padding under the row, plus the
    // item spacing before the keyboard child; otherwise the column overflows the
    // window into a scrollbar.
    float pane_h =
        ImGui::GetContentRegionAvail().y - 72.0f - ImGui::GetStyle().CellPadding.y - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##synth_pane_ph",
                      ImVec2(0, pane_h < 0.0f ? 0.0f : pane_h),
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    gui_osc_graph(channel);
    ImGui::EndChild();

    gui_keyboard();
}

// The Oscillators pane: the zone's instrument as an editable node graph.  The
// widget edits the graph; the committed bank is updated per frame from the
// drained change events, and wholesale model changes re-project the graph.
void Sculptor::SynthEditor::gui_osc_graph(uint32_t channel)
{
    init_osc_graph_widget();

    const int32_t zone = selected_zone[channel];
    if (osc_graph_reproject || ! osc_graph_projected || osc_graph_zone_channel != channel || zone < 0 ||
        osc_graph_zone_index != static_cast<uint32_t>(zone)) {
        osc_graph_reproject = false;
        if (zone >= 0) {
            reproject_osc_graph(channel, static_cast<uint32_t>(zone));
        }
        else {
            osc_graph_zone_channel = channel;
            osc_graph_projected    = false;
        }
    }

    if (! osc_graph_projected || zone < 0) {
        ImGui::TextDisabled("Oscillators editor is not available for this zone");
        return;
    }

    osc_graph.render(ImGui::GetContentRegionAvail(), nullptr);
    drain_osc_graph(channel, static_cast<uint32_t>(zone));
    run_osc_canvas_command();
}

void Sculptor::SynthEditor::drain_osc_graph(uint32_t channel, uint32_t zone)
{
    // One frame's events always fit one batch: a smaller buffer would split
    // a frame's events across frames, breaking the apply/commit semantics
    // that assume a whole batch belongs to one user gesture.
    Sculptor::GraphChange changes[Sculptor::max_pending_changes];
    const uint32_t        count = osc_graph.take_changes(changes, Sculptor::max_pending_changes);
    if (osc_graph.changes_overflowed()) {
        // The ring overflowed, so the batch is incomplete: resynchronize from
        // the committed bank instead of applying a partial batch.
        reproject_osc_graph(channel, zone);
        return;
    }
    if (count == 0) {
        // Node moves and resizes push no change events; they persist as their
        // own commit, tagged per node so one drag gesture stays one undo
        // entry.  The commit waits for the button to come up: committing per
        // frame during a drag would run the full bank validation and publish
        // path on every gesture frame.
        uint32_t       moved_count = 0;
        const uint32_t moved_node  = graph_moved_node(&moved_count);
        if (moved_node == Sculptor::pool_no_slot || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            return;
        }
        candidate                      = instr_bank;
        const uint32_t layout_tag_kind = moved_count > 1 ? osc_tag_fresh : osc_tag_graph_base;
        const uint32_t layout_tag_id0  = moved_count > 1 ? ++osc_fresh_tag_serial : moved_node;
        if (sync_osc_graph_layout(&candidate) &&
            commit_candidate(candidate, Sculptor::UndoGroupTag{ layout_tag_kind, layout_tag_id0, 0 })) {
            refresh_projected_layout();
        }
        else {
            Sculptor::notify_error("Synth: oscillator edit refused");
            reproject_osc_graph(channel, zone);
        }
        return;
    }

    candidate       = instr_bank;
    bool ok         = true;
    bool structural = false;
    for (uint32_t i = 0; i < count && ok; ++i) {
        const Sculptor::GraphChange& change = changes[i];
        structural                          = structural || change.kind != Sculptor::ChangeKind::value_changed;
        ok = Sculptor::apply_osc_graph_change(&candidate, &osc_graph, &osc_mapping, change, channel, zone);
    }
    if (ok) {
        ok = sync_osc_graph_layout(&candidate);
    }
    // One tag for the whole batch when every change edits the same field; a
    // mixed batch gets a fresh unique tag instead, so it can never amend an
    // entry tagged with one change's field identity.
    const Sculptor::GraphChange& first = changes[0];
    const uint32_t first_id0 = first.node_idx != Sculptor::pool_no_slot ? first.node_idx : first.connection_idx;
    bool           mixed     = false;
    for (uint32_t i = 1; i < count; ++i) {
        const Sculptor::GraphChange& other = changes[i];
        const uint32_t other_id0 = other.node_idx != Sculptor::pool_no_slot ? other.node_idx : other.connection_idx;
        if (other.kind != first.kind || other_id0 != first_id0 || other.slot_idx != first.slot_idx) {
            mixed = true;
            break;
        }
    }
    const Sculptor::UndoGroupTag tag =
        mixed
            ? Sculptor::UndoGroupTag{ osc_tag_fresh, ++osc_fresh_tag_serial, 0 }
            : Sculptor::UndoGroupTag{ osc_tag_graph_base + static_cast<uint32_t>(first.kind),
                                      first.node_idx != Sculptor::pool_no_slot ? first.node_idx : first.connection_idx,
                                      first.slot_idx };
    if (! ok || ! commit_candidate(candidate, tag)) {
        // Refused batch: graph and model resynchronize from the last committed
        // bank, and the refused edit does not coalesce into the next one.
        Sculptor::notify_error("Synth: oscillator edit refused");
        reproject_osc_graph(channel, zone);
        return;
    }
    refresh_projected_layout();
    if (structural) {
        // Structural applies (layer removal, binding rewiring) rebuild the
        // whole mapping from the committed bank before more edits arrive.
        osc_graph_reproject = true;
    }
}

void Sculptor::SynthEditor::run_osc_canvas_command()
{
    const OscCanvasCommand command = osc_canvas_command;
    osc_canvas_command             = osc_cmd_none;
    switch (command) {
        case osc_cmd_add_oscillator:
            do_osc_add_oscillator();
            break;
        case osc_cmd_add_envelope:
            do_osc_add_generator(true);
            break;
        case osc_cmd_add_lfo:
            do_osc_add_generator(false);
            break;
        case osc_cmd_add_parameter:
            do_osc_add_parameter();
            break;
        case osc_cmd_change_target_param:
            do_osc_change_target_param(osc_canvas_retarget_node, osc_canvas_retarget_target);
            break;
        case osc_cmd_none:
            break;
    }
}

void Sculptor::SynthEditor::reproject_fx_graph(uint32_t chain)
{
    osc_graph_projected = false;

    fx_graph_projected =
        Sculptor::project_effect_chain_to_graph(instr_bank.bank, chain, &osc_graph, &fx_mapping, fx_lfo_pinned);

    // Moved nodes' places ride the bank as kind-4 records, so every
    // re-projection (commits, undo/redo, tab and channel switches, file
    // loads) restores them by name - the projection's stable node identity.
    // set_node_layout pushes no events, so the restore cannot echo back
    // into the edit pipeline.
    for (uint32_t idx = 0; idx < Sculptor::max_nodes; ++idx) {
        if (! osc_graph.node_occupied(idx)) {
            continue;
        }
        const Sculptor::Node& node_ref   = osc_graph.node(idx);
        const int32_t         record_idx = find_fx_layout_record(instr_bank, chain, node_ref.name);
        if (record_idx >= 0) {
            const Synth::GraphNodeLayout& record = instr_bank.graph_layout[record_idx];
            osc_graph.set_node_layout(idx,
                                      vmath::vec2(record.x, record.y),
                                      record.width_override,
                                      record.height_override);
        }
    }
    refresh_projected_layout();

    // The pane's connection grammar and menus install after the projection:
    // the widget may last have hosted the oscillator projection, and the
    // projection itself owns no callbacks.
    osc_graph.set_validator(&fx_connection_validator, nullptr);
    osc_graph.set_delete_veto(&fx_node_delete_veto, nullptr);
    osc_graph.set_canvas_menu_callback(&fx_canvas_menu, nullptr);
    osc_graph.set_node_menu_callback(&fx_node_menu, nullptr);
    // Projection construction is quiet; drain the ring anyway so no leftover
    // event leaks into the next apply batch.
    Sculptor::GraphChange discard[Sculptor::max_pending_changes];
    while (osc_graph.take_changes(discard, Sculptor::max_pending_changes) != 0) {
    }
    (void)osc_graph.changes_overflowed();
    Sculptor::undo_group_reset(&osc_undo_group);
}

// The effects pane: re-project on selection or structural change, then apply
// drained edits per frame.  Same shape as gui_osc_graph.
void Sculptor::SynthEditor::gui_fx_graph(uint32_t chain)
{
    init_osc_graph_widget();

    if (fx_graph_reproject || ! fx_graph_projected || fx_mapping.chain != chain) {
        fx_graph_reproject = false;
        reproject_fx_graph(chain);
    }

    if (! fx_graph_projected) {
        ImGui::TextDisabled("The effects graph did not fit this chain");
        return;
    }

    osc_graph.render(ImGui::GetContentRegionAvail(), nullptr);
    drain_fx_graph(chain);
    run_fx_canvas_command();
}

void Sculptor::SynthEditor::drain_fx_graph(uint32_t chain)
{
    (void)chain; // the mapping carries the chain
    Sculptor::GraphChange changes[Sculptor::max_pending_changes];
    const uint32_t        count = osc_graph.take_changes(changes, Sculptor::max_pending_changes);
    if (osc_graph.changes_overflowed()) {
        reproject_fx_graph(fx_mapping.chain);
        return;
    }
    if (count == 0) {
        // Node moves push no change events; they persist as their own
        // commit, tagged per node so one drag gesture stays one undo entry.
        // The commit waits for the button to come up: committing per frame
        // during a drag would run the full bank validation and publish
        // path on every gesture frame.
        uint32_t       moved_count = 0;
        const uint32_t moved_node  = graph_moved_node(&moved_count);
        if (moved_node == Sculptor::pool_no_slot || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            return;
        }
        candidate                      = instr_bank;
        const uint32_t layout_tag_kind = moved_count > 1 ? osc_tag_fresh : fx_tag_node_move;
        const uint32_t layout_tag_id0  = moved_count > 1 ? ++osc_fresh_tag_serial : moved_node;
        if (sync_fx_graph_layout(&candidate) &&
            commit_candidate(candidate, Sculptor::UndoGroupTag{ layout_tag_kind, layout_tag_id0, 0 })) {
            refresh_projected_layout();
        }
        else {
            Sculptor::notify_error("Synth: effect edit refused");
            reproject_fx_graph(fx_mapping.chain);
        }
        return;
    }

    // Effect-node deletions apply from the highest slot down: removing a
    // slot shifts later slots, so ascending indices would target the wrong
    // slot.  Value events and LFO-node deletions are order-independent.
    uint32_t delete_order[Synth::max_chain_effects];
    uint32_t num_deletes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (changes[i].kind != Sculptor::ChangeKind::node_deleted) {
            continue;
        }
        const int32_t slot = Sculptor::fx_effect_slot_of(fx_mapping, changes[i].node_idx);
        if (slot < 0) {
            // A deleted LFO node loses its pin with its canvas presence.
            const uint32_t desc = Sculptor::fx_lfo_desc_of(fx_mapping, changes[i].node_idx);
            if (desc != 0) {
                fx_lfo_pinned[desc - 1] = false;
            }
            continue;
        }
        uint32_t insert = num_deletes++;
        while (insert > 0 && delete_order[insert - 1] < static_cast<uint32_t>(slot)) {
            delete_order[insert] = delete_order[insert - 1];
            --insert;
        }
        delete_order[insert] = static_cast<uint32_t>(slot);
    }

    // A serial-wire disconnect is refused by the apply path: name the rule
    // instead of the generic refusal, so the gesture's snap-back reads as
    // intentional protection.
    bool serial_disconnect = false;
    for (uint32_t i = 0; i < count && ! serial_disconnect; ++i) {
        const Sculptor::GraphChange& change = changes[i];
        if (change.kind != Sculptor::ChangeKind::connection_deleted) {
            continue;
        }
        if (! osc_graph.node_occupied(change.connection_input.node_idx) ||
            ! osc_graph.node_occupied(change.connection_output.node_idx)) {
            continue; // a byproduct of a node deletion in the same batch
        }
        const int32_t in_slot             = Sculptor::fx_effect_slot_of(fx_mapping, change.connection_input.node_idx);
        const int32_t out_slot            = Sculptor::fx_effect_slot_of(fx_mapping, change.connection_output.node_idx);
        const bool    into_output         = change.connection_input.node_idx == fx_mapping.output_node;
        const bool    out_is_chain_source = out_slot >= 0 || change.connection_output.node_idx == fx_mapping.input_node;
        if ((in_slot >= 0 && change.connection_input.slot_idx == 0 && out_is_chain_source) ||
            (into_output && out_is_chain_source)) {
            serial_disconnect = true;
        }
    }

    candidate       = instr_bank;
    bool ok         = true;
    bool structural = false;
    for (uint32_t i = 0; i < count && ok; ++i) {
        const bool is_effect_delete = changes[i].kind == Sculptor::ChangeKind::node_deleted &&
                                      Sculptor::fx_effect_slot_of(fx_mapping, changes[i].node_idx) >= 0;
        if (is_effect_delete) {
            continue; // applied in descending slot order below
        }
        structural = structural || changes[i].kind != Sculptor::ChangeKind::value_changed;
        ok         = Sculptor::apply_fx_graph_change(&candidate.bank, osc_graph, fx_mapping, changes[i]);
    }
    for (uint32_t i = 0; i < num_deletes && ok; ++i) {
        Sculptor::GraphChange delete_change = {};
        delete_change.kind                  = Sculptor::ChangeKind::node_deleted;
        delete_change.node_idx              = fx_mapping.effect_nodes[delete_order[i]];
        structural                          = true;
        ok = Sculptor::apply_fx_graph_change(&candidate.bank, osc_graph, fx_mapping, delete_change);
    }
    if (ok) {
        // A batch commit is also the moment moved nodes' places persist:
        // the sync stays sparse (only genuinely moved nodes write records).
        ok = sync_fx_graph_layout(&candidate);
    }

    // One tag for a single-field batch so a slider drag's per-frame events
    // amend one undo entry; mixed batches and wire gestures take a fresh
    // unique tag (a wire edit is one discrete undo step).
    const Sculptor::GraphChange& first = changes[0];
    bool                         mixed = false;
    for (uint32_t i = 1; i < count; ++i) {
        if (changes[i].kind != first.kind || changes[i].node_idx != first.node_idx ||
            changes[i].slot_idx != first.slot_idx) {
            mixed = true;
            break;
        }
    }
    for (uint32_t i = 0; i < count && ! mixed; ++i) {
        const auto kind = changes[i].kind;
        mixed = kind == Sculptor::ChangeKind::connection_added || kind == Sculptor::ChangeKind::connection_deleted ||
                kind == Sculptor::ChangeKind::connection_changed;
    }
    const Sculptor::UndoGroupTag tag = mixed ? Sculptor::UndoGroupTag{ osc_tag_fresh, ++osc_fresh_tag_serial, 0 }
                                             : Sculptor::UndoGroupTag{ fx_tag_value, first.node_idx, first.slot_idx };

    // A wire can bind one LFO too many: the bank validator would refuse the
    // commit, so surface the budget error instead of a generic refusal.
    uint32_t num_modulated = 0;
    uint32_t state_bytes   = 0;
    Sculptor::count_effect_budgets(candidate.bank, &num_modulated, &state_bytes);

    if (! ok || num_modulated > Synth::max_effect_mod_params || ! commit_candidate(candidate, tag)) {
        Sculptor::notify_error(num_modulated > Synth::max_effect_mod_params
                                   ? "Synth: the effect modulation pool is full (32 modulated parameters)"
                               : serial_disconnect
                                   ? "Synth: the chain wire carries the audio - delete the effect node instead"
                                   : "Synth: effect edit refused");
        reproject_fx_graph(fx_mapping.chain);
        return;
    }
    if (structural) {
        fx_graph_reproject = true; // the slot layout changed, nodes re-derive
    }
}

// Canvas and node-menu commands, run after the drain against the committed
// bank.  Every command is one undo entry and one re-projection.
void Sculptor::SynthEditor::run_fx_canvas_command()
{
    const FxCanvasCommand command = fx_canvas_command;
    const uint32_t        node    = fx_menu_node;
    fx_canvas_command             = fx_cmd_none;
    fx_menu_node                  = Sculptor::pool_no_slot;
    if (command == fx_cmd_none || ! fx_graph_projected) {
        return;
    }
    switch (command) {
        case fx_cmd_add_effect:
            do_fx_add_effect(fx_canvas_type);
            break;
        case fx_cmd_add_lfo:
            do_fx_add_lfo();
            break;
        case fx_cmd_change_type:
            do_fx_change_type(node, fx_canvas_type);
            break;
        case fx_cmd_none:
            break;
    }
}

void Sculptor::SynthEditor::do_fx_add_effect(Synth::EffectType type)
{
    candidate                        = instr_bank;
    Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&candidate.bank, fx_mapping.chain);
    if (chain.num_effects >= Synth::max_chain_effects) {
        Sculptor::notify_error("Synth: the chain already holds four effects");
        return;
    }

    // Node preflight: the projection must still be able to show the chain
    // plus one more node, or the pane would break with no way back.
    if (Sculptor::fx_projected_node_count(instr_bank.bank, fx_mapping.chain, fx_lfo_pinned) + 1 > Sculptor::max_nodes) {
        Sculptor::notify_error("Synth: cannot add another effect: the graph node budget is full");
        return;
    }
    Sculptor::fx_init_slot(&chain, chain.num_effects, type);
    chain.effects[chain.num_effects].enabled = true;
    ++chain.num_effects;

    uint32_t num_modulated = 0;
    uint32_t state_bytes   = 0;
    Sculptor::count_effect_budgets(candidate.bank, &num_modulated, &state_bytes);
    if (state_bytes > Synth::effect_state_budget) {
        Sculptor::notify_error("Synth: adding this effect would exceed the effect state budget");
        return;
    }

    if (! commit_candidate(candidate,
                           Sculptor::UndoGroupTag{ fx_tag_add, fx_mapping.chain, static_cast<uint32_t>(type) })) {
        return;
    }
    fx_graph_reproject = true;
}

// Canvas menu "Add LFO": a fresh descriptor pinned onto the canvas so the
// projection keeps its node while nothing references it yet; wiring the
// Value dot to a parameter is what binds it.
void Sculptor::SynthEditor::do_fx_add_lfo()
{
    candidate        = instr_bank;
    uint16_t desc_id = 0;
    // Node preflight: the pin demands a node from the same pool the chain's
    // effects and endpoints draw from; refuse before the descriptor exists.
    if (Sculptor::fx_projected_node_count(instr_bank.bank, fx_mapping.chain, fx_lfo_pinned) + 1 > Sculptor::max_nodes) {
        Sculptor::notify_error("Synth: cannot add an LFO: the graph node budget is full");
        return;
    }
    if (! Sculptor::allocate_default_lfo(&candidate.bank, &desc_id)) {
        Sculptor::notify_error("Synth: cannot add an LFO: the descriptor pool is full");
        return;
    }
    if (! commit_candidate(candidate, Sculptor::UndoGroupTag{ fx_tag_add_lfo, desc_id, 0 })) {
        return;
    }
    fx_lfo_pinned[desc_id - 1] = true;
    fx_graph_reproject         = true;
}

// Type change replaces the whole slot with initialized bindings of the new
// type, so no dormant modulation of the old type survives the reclaim.
void Sculptor::SynthEditor::do_fx_change_type(uint32_t node_idx, Synth::EffectType type)
{
    const int32_t slot = Sculptor::fx_effect_slot_of(fx_mapping, node_idx);
    if (slot < 0) {
        return;
    }
    candidate                        = instr_bank;
    Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&candidate.bank, fx_mapping.chain);
    // The effect keeps its place and its on/off state across a type change;
    // only the type and its parameter defaults are replaced.
    const bool enabled = chain.effects[slot].enabled;
    Sculptor::fx_init_slot(&chain, static_cast<uint32_t>(slot), type);
    chain.effects[slot].enabled = enabled;
    if (! commit_candidate(candidate, Sculptor::UndoGroupTag{ fx_tag_type, node_idx, static_cast<uint32_t>(type) })) {
        return;
    }
    fx_graph_reproject = true;
}

// Canvas menu "Add Oscillator": one more oscillator layer at the menu
// position, with the model's fresh-Oscillator defaults. Node capacity is
// checked against the actual projected node count: a full node pool refuses
// even below the seven-layer limit.
void Sculptor::SynthEditor::do_osc_add_oscillator()
{
    const uint32_t channel        = osc_graph_zone_channel;
    const uint32_t zone           = osc_graph_zone_index;
    candidate                     = instr_bank;
    const Synth::Zone& zone_entry = candidate.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= candidate.bank.instruments.num_allocated) {
        return;
    }
    Synth::Instrument& instrument = candidate.bank.instruments.entries[zone_entry.instrument];
    if (instrument.layer_count >= Synth::max_layers) {
        Sculptor::notify_error("Synth: cannot add an oscillator: all seven layers exist");
        return;
    }
    if (Sculptor::count_projected_nodes(candidate, channel, zone) + 1 > Sculptor::max_nodes) {
        Sculptor::notify_error("Synth: cannot add an oscillator: the graph is full");
        return;
    }
    if (! Sculptor::graph_records_have_capacity(candidate, 1)) {
        Sculptor::notify_error("Synth: cannot add an oscillator: the graph state is full");
        return;
    }

    const uint32_t new_layer     = instrument.layer_count;
    instrument.layers[new_layer] = Synth::Oscillator{};
    // Waveform A cannot be off, so a fresh layer starts on sine.
    instrument.layers[new_layer].osc_type[0] = Synth::WaveType::sine_wave;
    instrument.layer_count                   = static_cast<uint8_t>(new_layer + 1);

    // The new layer's node lands at the menu position via its layout record.
    Synth::GraphNodeLayout record = {};
    record.channel                = static_cast<uint8_t>(channel);
    record.zone                   = static_cast<uint8_t>(zone);
    record.kind                   = 0;
    record.index                  = static_cast<uint8_t>(Synth::graph_canonical_first_osc + new_layer);
    const vmath::vec2 popup_pos   = osc_graph.canvas_popup_pos();
    record.x                      = popup_pos.x;
    record.y                      = popup_pos.y;
    candidate.graph_layout[candidate.graph_layout_count++] = record;

    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_add_osc, channel, zone }))
        osc_graph_reproject = true;
}

// Canvas menu "Add Envelope"/"Add LFO": a fresh descriptor plus a detached
// node record at the menu position; the re-projection builds the node.
void Sculptor::SynthEditor::do_osc_add_generator(bool is_env)
{
    const uint32_t channel = osc_graph_zone_channel;
    const uint32_t zone    = osc_graph_zone_index;
    candidate              = instr_bank;

    // Preflight before any mutation: descriptor pool, node pool, record list
    // and the per-zone detached cap.
    if (is_env ? candidate.bank.envelopes.num_allocated >= Synth::max_envelopes
               : candidate.bank.lfos.num_allocated >= Synth::max_lfos) {
        Sculptor::notify_error("Synth: cannot add: the descriptor pool is full");
        return;
    }
    if (Sculptor::count_projected_nodes(candidate, channel, zone) + 1 > Sculptor::max_nodes) {
        Sculptor::notify_error("Synth: cannot add: the graph is full");
        return;
    }
    if (Sculptor::count_detached_records(candidate, channel, zone) >= Sculptor::max_detached_nodes ||
        ! Sculptor::graph_records_have_capacity(candidate, 1)) {
        Sculptor::notify_error("Synth: cannot add: the graph state is full");
        return;
    }

    Synth::GraphNodeLayout record = {};
    record.channel                = static_cast<uint8_t>(channel);
    record.zone                   = static_cast<uint8_t>(zone);
    record.kind                   = is_env ? 1 : 2;
    const vmath::vec2 popup_pos   = osc_graph.canvas_popup_pos();
    record.x                      = popup_pos.x;
    record.y                      = popup_pos.y;
    if (is_env) {
        const uint32_t slot = candidate.bank.envelopes.allocate();
        // The default must satisfy bank validation: an attack from min to
        // max in about 30 ms (positions are control ticks, 256/44100 s
        // each) with sustain at the peak and a ~0.5 s release tail to neutral (0x8000), spanning -1..+1.  The delta is
        // in per-65535 point units: 0xFFFF scales to min_value + min_max_delta.
        Synth::EnvelopeDescriptor& env = candidate.bank.envelopes.entries[slot];
        env                            = Synth::EnvelopeDescriptor{};
        env.num_points                 = 3;
        env.sustain_first_point        = 1;
        env.sustain_last_point         = 1;
        env.min_value                  = -1.0f;
        env.min_max_delta              = 2.0f / 65535.0f;
        env.points[0].position         = 0;
        env.points[0].value            = 0x0000;
        env.points[1].position         = 5;
        env.points[1].value            = 0xFFFF;
        env.points[2].position         = 91;
        env.points[2].value            = 0x8000;
        record.index                   = static_cast<uint8_t>(slot + 1);
    }
    else {
        uint16_t lfo_desc_id = 0;
        if (! Sculptor::allocate_default_lfo(&candidate.bank, &lfo_desc_id)) {
            Sculptor::notify_error("Synth: the LFO descriptor pool is full");
            return;
        }
        record.index = static_cast<uint8_t>(lfo_desc_id);
    }
    record.uid = Sculptor::allocate_detached_uid(candidate, channel, zone, record.kind);
    candidate.graph_layout[candidate.graph_layout_count++] = record;

    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate,
                         Sculptor::UndoGroupTag{ is_env ? osc_tag_add_env : osc_tag_add_lfo, channel, zone }))
        osc_graph_reproject = true;
}

// Canvas menu "Add Parameter": a free-standing Volume parameter node at
// the menu position, via a kind-3 record; the re-projection builds the node.
void Sculptor::SynthEditor::do_osc_add_parameter()
{
    const uint32_t channel        = osc_graph_zone_channel;
    const uint32_t zone           = osc_graph_zone_index;
    candidate                     = instr_bank;
    const Synth::Zone& zone_entry = candidate.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= candidate.bank.instruments.num_allocated) {
        return;
    }
    // The live mapping mirrors the committed bank, and the new record is
    // free-standing, so the projection adds at most one parameter node.
    char add_error[128];
    if (osc_graph_projected && Sculptor::osc_add_parameter_refused(osc_mapping, 0, add_error, sizeof(add_error))) {
        Sculptor::notify_error("%s", add_error);
        return;
    }

    // Same preflights as Add Envelope/LFO, minus the descriptor pool:
    // node pool, record list and the per-zone detached cap.
    if (Sculptor::count_projected_nodes(candidate, channel, zone) + 1 > Sculptor::max_nodes) {
        Sculptor::notify_error("Synth: cannot add a parameter: the graph is full");
        return;
    }
    if (Sculptor::count_detached_records(candidate, channel, zone) >= Sculptor::max_detached_nodes ||
        ! Sculptor::graph_records_have_capacity(candidate, 1)) {
        Sculptor::notify_error("Synth: cannot add a parameter: the graph state is full");
        return;
    }
    Synth::GraphNodeLayout record = {};
    record.channel                = static_cast<uint8_t>(channel);
    record.zone                   = static_cast<uint8_t>(zone);
    record.kind                   = 3;
    record.index                  = 0; // volume, projection order
    record.param_slot             = Synth::graph_record_param_free;
    const vmath::vec2 popup_pos   = osc_graph.canvas_popup_pos();
    record.x                      = popup_pos.x;
    record.y                      = popup_pos.y;
    record.uid                    = Sculptor::allocate_detached_uid(candidate, channel, zone, 3);
    snprintf(record.name, sizeof(record.name), "Parameter %u", record.uid);
    candidate.graph_layout[candidate.graph_layout_count++] = record;
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_add_param, channel, zone })) {
        osc_graph_reproject = true;
    }
}

// Node menu "Change Target": retargets the parameter - the kind-3 record
// re-keys, the value wires shift onto the new target's oscillator rows, and
// the source rows adopt the destination's routing. The rewrite is eventless,
// so the bindings and the compiled instrument refresh before the commit.
// Names are user-owned: the node keeps its title across the retarget.
void Sculptor::SynthEditor::do_osc_change_target_param(uint32_t node_idx, uint32_t new_target)
{
    if (! osc_graph_projected || node_idx >= Sculptor::max_nodes || ! osc_graph.node_occupied(node_idx)) {
        return;
    }
    const uint32_t     channel    = osc_graph_zone_channel;
    const uint32_t     zone       = osc_graph_zone_index;
    const Synth::Zone& zone_entry = instr_bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= instr_bank.bank.instruments.num_allocated) {
        return;
    }
    const int32_t found = Sculptor::find_param(osc_mapping, node_idx);
    if (found < 0) {
        return;
    }
    Sculptor::ParamEntry& param = osc_mapping.params[found];
    if (param.uid == 0) {
        // A derived parameter cannot change target: the generator bindings
        // would re-derive the old parameter on the next projection.
        Sculptor::notify_error("Synth: a derived parameter cannot change target");
        return;
    }
    if (new_target >= 5) {
        Sculptor::notify_error("Synth: invalid parameter target");
        return;
    }
    if (new_target == param.target) {
        Sculptor::notify_error("Synth: the parameter already has that target");
        return;
    }
    if (Sculptor::param_target_has_recordless_derived(osc_mapping, new_target)) {
        // A record re-keyed into the target would attach to its derived
        // parameter positionally at re-projection and hijack that sibling.
        Sculptor::notify_error("Synth: cannot change target: it has a derived parameter - rename that one first");
        return;
    }
    candidate = instr_bank;
    if (! Sculptor::retarget_param(&candidate,
                                   osc_graph,
                                   osc_mapping,
                                   channel,
                                   zone,
                                   static_cast<uint32_t>(found),
                                   new_target)) {
        Sculptor::notify_error("Synth: cannot change target: the graph state is full");
        return;
    }
    // The wire rewrite is eventless, so no drained batch will compile the
    // moved routing: refresh the bindings and the compiled instrument here,
    // before the commit.
    if (! Sculptor::refresh_osc_graph_compilation(&candidate, osc_graph, osc_mapping, channel, zone)) {
        Sculptor::notify_error("Synth: oscillator edit refused");
        reproject_osc_graph(channel, zone);
        return;
    }
    Sculptor::undo_group_reset(&osc_undo_group);
    if (! commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_change_target_param, node_idx, 0 })) {
        // Refused commit: the flipped mapping target and the retargeted node must
        // not linger in the graph; re-derive it from the committed bank (the
        // same recovery the refusal paths in drain_osc_graph use).
        reproject_osc_graph(channel, zone);
        return;
    }
    osc_graph_reproject = true;
}

void Sculptor::SynthEditor::gui_keyboard()
{
    // Zero window padding so the 72px child holds exactly the 8px report margin and the
    // 64px key strip; default padding would overflow the content into a scrollbar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##synth_keyboard", ImVec2(0, 72), true);
    ImGui::PopStyleVar();

    const Synth::InstrumentBank& bank           = instr_bank.bank;
    const bool                   channel_target = selected_target != target_master;
    const uint32_t               channel        = channel_target ? selected_target : 0;
    const bool                   active         = channel_target && bank.channel_enabled[channel];

    const ImVec2 top     = ImGui::GetCursorScreenPos();
    const float  width   = ImGui::GetContentRegionAvail().x;
    const float  height  = 64.0f;
    const ImVec2 origin  = ImVec2(top.x, top.y + 8.0f);
    const float  white_w = width / 75.0f; // 75 white keys across the full MIDI range
    const float  black_w = white_w * 0.6f;
    const float  black_h = height * 0.6f;

    // Fixed piano colors: a dark theme would otherwise invert the keys.
    ImDrawList* const draw            = ImGui::GetWindowDrawList();
    const uint32_t    color_white     = IM_COL32(240, 240, 240, 255);
    const uint32_t    color_line      = IM_COL32(120, 120, 120, 255);
    const uint32_t    color_black_key = IM_COL32(35, 35, 35, 255);
    const uint32_t    color_held      = IM_COL32(90, 160, 255, 255);

    for (uint32_t wk = 0; wk < 75; wk++) {
        const float x = origin.x + static_cast<float>(wk) * white_w;
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + white_w, origin.y + height), color_white);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), color_line);
    }
    draw->AddLine(ImVec2(origin.x + width, origin.y), ImVec2(origin.x + width, origin.y + height), color_line);

    for (uint32_t note = 0; note < 128; note++) {
        if (! is_black_note(note))
            continue;
        const float x = origin.x + boundary_x_of(note, white_w, black_w);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + black_w, origin.y + black_h), color_black_key);
    }

    // Zone boundaries: a separator at each zone's first note, colored by zone index.
    if (active) {
        static const uint32_t zone_palette[8] = { IM_COL32(255, 80, 80, 255),   IM_COL32(80, 255, 120, 255),
                                                  IM_COL32(80, 160, 255, 255),  IM_COL32(255, 200, 60, 255),
                                                  IM_COL32(200, 100, 255, 255), IM_COL32(60, 220, 220, 255),
                                                  IM_COL32(255, 120, 200, 255), IM_COL32(180, 180, 80, 255) };

        const Synth::Zone* const zones = bank.channel_zones[channel];
        // Zone strips: the first zone keeps the default key color; later zones tint
        // the bottom of every key they cover, white and black alike.
        for (uint32_t note = 0; note < 128; note++) {
            const int32_t zone = static_cast<int32_t>(Synth::zone_entry_at(zones, note));
            if (zone <= 0)
                continue;
            const float x = origin.x + boundary_x_of(note, white_w, black_w);
            if (is_black_note(note))
                draw->AddRectFilled(ImVec2(x, origin.y + black_h - 8.0f),
                                    ImVec2(x + black_w, origin.y + black_h),
                                    zone_palette[zone % 8]);
            else
                draw->AddRectFilled(ImVec2(x, origin.y + height - 8.0f),
                                    ImVec2(x + white_w, origin.y + height),
                                    zone_palette[zone % 8]);
        }
        // Selection dimming: keys outside the selected zone lose contrast so the
        // selected zone reads at a glance (white keys darken, black keys lighten).
        const int32_t selected = selected_zone[channel];
        if (zone_count(bank, channel) > 1 && selected >= 0) {
            for (uint32_t note = 0; note < 128; note++) {
                if (static_cast<int32_t>(Synth::zone_entry_at(zones, note)) == selected)
                    continue;
                const float x = origin.x + boundary_x_of(note, white_w, black_w);
                if (is_black_note(note))
                    draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + black_w, origin.y + black_h), 0x40ffffff);
                else
                    draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + white_w, origin.y + height), 0x59000000);
            }
        }

        for (uint32_t entry = 1; entry < Synth::max_instr_per_channel; entry++) {
            if (zones[entry].start_note == 0)
                break;
            const float x = origin.x + boundary_x_of(zones[entry].start_note - 1, white_w, black_w);
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), zone_palette[entry % 8]);
        }
    }

    // The held note is highlighted on top of everything else.
    if (held_note_active && held_note_channel == channel) {
        const uint32_t note  = held_note;
        const float    x     = origin.x + boundary_x_of(note, white_w, black_w);
        const float    key_w = is_black_note(note) ? black_w : white_w;
        const float    key_h = is_black_note(note) ? black_h : height;
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + key_w, origin.y + key_h), color_held);
    }

    // Hit test: black keys are on top, then white keys.
    const ImGuiIO& io    = ImGui::GetIO();
    const ImVec2   mouse = io.MousePos;
    const bool     over =
        mouse.x >= origin.x && mouse.x < origin.x + width && mouse.y >= origin.y && mouse.y < origin.y + height;
    int32_t hit = -1;
    if (over) {
        for (uint32_t note = 0; note < 128 && hit < 0; note++) {
            if (! is_black_note(note))
                continue;
            const float x = origin.x + boundary_x_of(note, white_w, black_w);
            if (mouse.x >= x && mouse.x < x + black_w && mouse.y < origin.y + black_h)
                hit = static_cast<int32_t>(note);
        }
        if (hit < 0) {
            const uint32_t wk = static_cast<uint32_t>((mouse.x - origin.x) / white_w);
            if (wk < 75)
                hit = static_cast<int32_t>(note_at_white(wk));
        }
    }

    // The hover gate keeps clicks from occluding windows from triggering keys;
    // the keyboard child is only hovered when it is actually on top.
    if (active && hit >= 0 && ImGui::IsWindowHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int32_t zone =
                static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], static_cast<uint32_t>(hit)));
            if (zone >= 0) {
                selected_zone[channel]     = zone;
                last_clicked_note[channel] = static_cast<uint8_t>(hit);
                zone_tab_force_entry       = zone; // keyboard click moves the tab bar too
            }
            if (! held_note_active) {
                submit_note_event(channel, static_cast<uint32_t>(hit), true);
                held_note_active  = true;
                held_note_channel = static_cast<uint8_t>(channel);
                held_note         = static_cast<uint8_t>(hit);
            }
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            release_held_note(); // opening the menu releases the held note
            zone_menu_channel  = channel;
            zone_menu_note     = static_cast<uint32_t>(hit);
            zone_menu_from_tab = false;
            zone_menu_open     = true;
        }
    }

    ImGui::Dummy(ImVec2(width, 8.0f + height));
    ImGui::EndChild();
}

void Sculptor::SynthEditor::do_zone_delete(uint32_t channel, uint32_t entry)
{
    candidate                    = instr_bank;
    Synth::Zone* const zones     = candidate.bank.channel_zones[channel];
    const uint32_t     num_zones = zone_count(candidate.bank, channel);
    if (entry >= num_zones)
        return;
    if (entry == 0) {
        if (num_zones == 1)
            return;              // the channel keeps at least one playable zone
        zones[1].start_note = 1; // the next zone takes over from note 0
        entry               = 0; // the promoted zone slides into the freed slot
    }
    // Boundaries are stored as zone starts, so dropping the entry extends the
    // previous zone over the deleted range.
    for (uint32_t zone = entry; zone + 1 < Synth::max_instr_per_channel; zone++)
        zones[zone] = zones[zone + 1];
    memset(&zones[Synth::max_instr_per_channel - 1], 0, sizeof(zones[0]));
    // The dropped entry's records and mask row move with the zone table.
    Sculptor::zone_records_drop_zone(&candidate, channel, entry);
    Synth::reclaim_unused_slots(&candidate);
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_zone_drop, channel, entry }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::do_zone_copy_instrument(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0)
        return;

    const uint32_t           instrument = bank.channel_zones[channel][entry].instrument;
    const Synth::Instrument& source     = bank.instruments.entries[instrument];
    uint32_t len = Synth::encode_instrument_json(zone_clipboard_text, sizeof(zone_clipboard_text), &source, &bank);
    uint32_t envelope_count = 0;
    uint32_t lfo_count      = 0;
    uint32_t source_count   = 0;
    uint32_t layout_count   = 0;
    if (! len ||
        ! Synth::decode_instrument_json(zone_clipboard_text,
                                        len,
                                        &clipboard_decoded_instrument,
                                        clipboard_decoded_envelopes,
                                        &envelope_count,
                                        clipboard_decoded_lfos,
                                        &lfo_count) ||
        ! Sculptor::encode_instrument_graph_layout(source,
                                                   bank,
                                                   instr_bank,
                                                   channel,
                                                   static_cast<uint32_t>(entry),
                                                   clipboard_source_layout,
                                                   Synth::instrument_graph_layout_capacity,
                                                   &source_count) ||
        ! Sculptor::normalize_instrument_graph_layout(source,
                                                      bank.envelopes.entries,
                                                      bank.envelopes.num_allocated,
                                                      bank.lfos.entries,
                                                      bank.lfos.num_allocated,
                                                      clipboard_source_layout,
                                                      source_count,
                                                      clipboard_decoded_instrument,
                                                      clipboard_decoded_envelopes,
                                                      envelope_count,
                                                      clipboard_decoded_lfos,
                                                      lfo_count,
                                                      clipboard_decoded_layout,
                                                      Synth::instrument_graph_layout_capacity,
                                                      &layout_count) ||
        ! (len = Synth::encode_instrument_json(zone_clipboard_text,
                                               sizeof(zone_clipboard_text),
                                               &source,
                                               &bank,
                                               clipboard_decoded_layout,
                                               layout_count))) {
        Sculptor::notify_error("Synth: cannot copy zone: the instrument did not encode");
        return;
    }
    zone_clipboard_text[len] = 0;
    ImGui::SetClipboardText(zone_clipboard_text);
}

void Sculptor::SynthEditor::do_zone_paste_instrument(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0)
        return;

    const char* const text = ImGui::GetClipboardText();
    if (! text || ! text[0]) {
        Sculptor::notify_error("Synth: cannot paste: the clipboard holds no instrument document");
        return;
    }

    uint32_t envelope_count = 0;
    uint32_t lfo_count      = 0;
    uint32_t layout_count   = 0;
    if (! Synth::decode_instrument_json(text,
                                        static_cast<uint32_t>(strlen(text)),
                                        &clipboard_decoded_instrument,
                                        clipboard_decoded_envelopes,
                                        &envelope_count,
                                        clipboard_decoded_lfos,
                                        &lfo_count,
                                        clipboard_decoded_layout,
                                        Synth::instrument_graph_layout_capacity,
                                        &layout_count)) {
        Sculptor::notify_error("Synth: cannot paste: not a valid instrument document");
        return;
    }
    if (! Sculptor::replace_zone_instrument_candidate(instr_bank,
                                                      channel,
                                                      static_cast<uint32_t>(entry),
                                                      clipboard_decoded_instrument,
                                                      clipboard_decoded_envelopes,
                                                      envelope_count,
                                                      clipboard_decoded_lfos,
                                                      lfo_count,
                                                      clipboard_decoded_layout,
                                                      layout_count,
                                                      &candidate)) {
        Sculptor::notify_error("Synth: cannot paste: the instrument replacement was refused");
        return;
    }
    // Each paste is its own undo step, so the undo focus shows the change.
    Sculptor::undo_group_reset(&osc_undo_group);
    selected_target   = channel;
    show_effects_mode = false;
    effects_tab_pend  = 0;
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_fresh, ++osc_fresh_tag_serial, 0 }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::gui_zone_menu()
{
    if (zone_menu_open) {
        zone_menu_open = false;
        ImGui::OpenPopup("##synth_zone_menu");
    }
    if (! ImGui::BeginPopup("##synth_zone_menu"))
        return;

    const uint32_t               channel   = zone_menu_channel;
    const uint32_t               note      = zone_menu_note;
    const Synth::InstrumentBank& bank      = instr_bank.bank;
    const Synth::Zone* const     zones     = bank.channel_zones[channel];
    const int32_t                entry     = static_cast<int32_t>(Synth::zone_entry_at(zones, note));
    const uint32_t               num_zones = zone_count(bank, channel);

    const bool has_next = entry >= 0 && static_cast<uint32_t>(entry) + 1 < num_zones;
    // A split at a zone's first note swaps the instrument in place; any other split
    // inserts an entry after it, which a full 16-slot table refuses.
    const bool first_note = entry >= 0 && note + 1 == zones[entry].start_note;
    const bool table_full = ! first_note && num_zones >= Synth::max_instr_per_channel;

    if (ImGui::MenuItem("Rename zone...")) {
        menu_channel      = channel;
        rename_zone_entry = static_cast<uint32_t>(entry);
        rename_zone       = true;
        rename_popup_open = true;
    }
    if (ImGui::MenuItem("Copy to Clipboard", nullptr, false, entry >= 0))
        do_zone_copy_instrument(channel, note);
    // The replacement transaction evaluates capacity and reports refusal
    // visibly, so the menu item stays enabled.
    if (ImGui::MenuItem("Paste from Clipboard", nullptr, false, entry >= 0))
        do_zone_paste_instrument(channel, note);
    if (ImGui::MenuItem("Add to previous zone", nullptr, false, entry > 0))
        do_zone_join_previous(channel, note);
    if (ImGui::MenuItem("Add to next zone", nullptr, false, has_next))
        do_zone_join_next(channel, note);
    if (! zone_menu_from_tab) {
        if (ImGui::MenuItem("Create new zone",
                            nullptr,
                            false,
                            entry >= 0 && ! table_full && bank.instruments.num_allocated < Synth::max_instruments))
            do_zone_split_new(channel, note);
    }
    if (ImGui::MenuItem("Delete zone", nullptr, false, entry >= 0 && (entry > 0 || num_zones > 1)))
        do_zone_delete(channel, static_cast<uint32_t>(entry));

    ImGui::EndPopup();
}

void Sculptor::SynthEditor::do_zone_join_previous(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry <= 0)
        return;

    candidate = instr_bank;
    if (! Synth::zone_join_previous(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The dropped entry (when the joined zone became empty) shifts later zones
    // down, so its records and mask row move with the zone table.
    if (zone_count(candidate.bank, channel) < zone_count(instr_bank.bank, channel))
        Sculptor::zone_records_drop_zone(&candidate, channel, static_cast<uint32_t>(entry));

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_clicked_note[channel] = static_cast<uint8_t>(note);
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_join_prev, channel, note }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::do_zone_join_next(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0 || static_cast<uint32_t>(entry) + 1 >= zone_count(bank, channel))
        return;

    candidate = instr_bank;
    if (! Synth::zone_join_next(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The dropped entry (when the joined zone became empty) shifts later zones
    // down, so its records and mask row move with the zone table.
    if (zone_count(candidate.bank, channel) < zone_count(instr_bank.bank, channel))
        Sculptor::zone_records_drop_zone(&candidate, channel, static_cast<uint32_t>(entry));

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_clicked_note[channel] = static_cast<uint8_t>(note);
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_join_next, channel, note }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::do_zone_split_new(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0)
        return;

    // A split at the zone's first note swaps the instrument in place: no zone
    // is inserted, so the graph records stay where they are.  Any other split
    // inserts an entry and shifts later zones, moving records and mask rows.
    const bool first_note = note + 1 == bank.channel_zones[channel][entry].start_note;

    candidate = instr_bank;
    if (! Synth::zone_split_new(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note, &candidate))
        return; // pool or table full: the menu item is grayed, but stay safe

    if (! first_note && ! Sculptor::zone_records_split_copy(&candidate, channel, static_cast<uint32_t>(entry))) {
        Sculptor::notify_error("Synth: cannot split: the graph state is full");
        return;
    }

    // Splitting at the zone's first note orphans its old instrument.
    Synth::reclaim_unused_slots(&candidate);
    // The split hands the clicked key to the new zone; select it so the tab
    // bar follows the zone the user just created.
    selected_zone[channel] = static_cast<int32_t>(Synth::zone_entry_at(candidate.bank.channel_zones[channel], note));
    zone_tab_force_entry   = selected_zone[channel];
    last_clicked_note[channel] = static_cast<uint8_t>(note);
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_zone_split, channel, note }))
        osc_graph_reproject = true;
}

void Sculptor::SynthEditor::gui_channel_popup()
{
    if (channel_menu_open) {
        channel_menu_open = false;
        ImGui::OpenPopup("##synth_channel_menu");
    }
    if (! ImGui::BeginPopup("##synth_channel_menu"))
        return;

    const uint32_t channel = menu_channel;

    if (ImGui::MenuItem("Rename...")) {
        rename_zone       = false;
        rename_popup_open = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Load...")) {
        library_channel = channel;
        library_open    = true;
        ImGui::CloseCurrentPopup();
    }

    // Save/Save As are unavailable for an empty or disabled channel: there is no
    // instrument to record.
    uint32_t   save_entry = save_zone_entry(instr_bank.bank, channel, selected_zone[channel]);
    const bool can_save   = save_entry != pool_no_slot;
    if (can_save && ImGui::MenuItem("Save")) {
        library_channel = channel;
        // The record name comes from the channel list; a channel with a cleared
        // name falls back to its first zone's instrument name.
        char name[Synth::max_name_len];
        if (instr_bank.channel_names[channel][0])
            snprintf(name, sizeof(name), "%s", instr_bank.channel_names[channel]);
        else
            Synth::get_zone_name(&instr_bank, channel, save_entry, name, sizeof(name));
        save_instrument_to_library(save_category, name);
        ImGui::CloseCurrentPopup();
    }
    if (can_save && ImGui::MenuItem("Save As...")) {
        library_channel = channel;
        save_as_open    = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Initialize"))
        do_initialize(channel);
    if (ImGui::MenuItem("Delete"))
        do_delete(channel);

    ImGui::EndPopup();
}

void Sculptor::SynthEditor::gui_rename_popup()
{
    bool just_opened = false;
    if (rename_popup_open) {
        rename_popup_open = false;
        just_opened       = true;
        memcpy(
            rename_buf,
            rename_zone
                ? instr_bank.instrument_names[instr_bank.bank.channel_zones[menu_channel][rename_zone_entry].instrument]
                : instr_bank.channel_names[menu_channel],
            sizeof(rename_buf));
        ImGui::OpenPopup("Rename");
    }

    if (! ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // Esc is Cancel: close without committing.  Every widget stays rendered so
    // the buttons work regardless of key state.
    const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (esc)
        ImGui::CloseCurrentPopup();

    ImGui::SetNextItemWidth(240.0f);
    if (just_opened)
        ImGui::SetKeyboardFocusHere();
    const bool pressed_enter =
        ImGui::InputText("##rename", rename_buf, sizeof(rename_buf), ImGuiInputTextFlags_EnterReturnsTrue) != 0;
    const bool pressed_ok = ImGui::Button("OK");
    ImGui::SameLine();
    const bool pressed_cancel = ImGui::Button("Cancel");
    if (pressed_cancel)
        ImGui::CloseCurrentPopup();
    if ((pressed_enter || pressed_ok) && rename_buf[0] && ! esc) {
        candidate = instr_bank;
        memcpy(
            rename_zone
                ? candidate.instrument_names[candidate.bank.channel_zones[menu_channel][rename_zone_entry].instrument]
                : candidate.channel_names[menu_channel],
            rename_buf,
            sizeof(rename_buf));
        Sculptor::undo_group_reset(&osc_undo_group);
        commit_candidate(candidate,
                         Sculptor::UndoGroupTag{ osc_tag_rename, menu_channel, rename_zone ? rename_zone_entry : 0 });
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool Sculptor::SynthEditor::do_library_load(const Synth::LibraryEntry& entry)
{
    // The record joins the candidate; the editable bank only changes after the
    // whole candidate validates.
    candidate           = instr_bank;
    uint16_t first_slot = 0;
    if (! Synth::load_library_instrument(library_path, &entry, &candidate, library_channel, &first_slot)) {
        Sculptor::notify_error("Synth: library load failed: %s is unreadable, corrupt or the bank is full",
                               library_path);
        return false;
    }

    // Loading enables the channel; the record's effect chain replaces the
    // channel's chain. The old instruments and the replaced chain's
    // now-unreferenced descriptors are reclaimed from the candidate.
    candidate.bank.channel_enabled[library_channel] = 1;
    // The record replaces the channel's zoning, so its graph state resets with it.
    Sculptor::channel_records_reset(&candidate, library_channel);

    // The channel takes the record's name so the library identity carries over.
    memcpy(candidate.channel_names[library_channel], entry.name, sizeof(candidate.channel_names[library_channel]));
    Synth::reclaim_unused_slots(&candidate);
    selected_target                    = library_channel;
    last_clicked_note[library_channel] = 0;
    Sculptor::undo_group_reset(&osc_undo_group);
    if (commit_candidate(candidate, Sculptor::UndoGroupTag{ osc_tag_load, library_channel, 0 }))
        osc_graph_reproject = true;
    fx_graph_reproject = true;

    return true;
}

bool Sculptor::SynthEditor::save_instrument_to_library(const char* category, const char* name)
{
    // Refresh the index so the overwrite check sees the current file contents.
    Synth::LibraryScanStatus scan_status = Synth::library_valid;
    // A failed scan leaves a partial index that cannot support the overwrite
    // check, so the save is refused before any dialog can open.
    library_num_entries =
        Synth::read_library_index(library_path, library_entries, Synth::library_max_records, &scan_status);
    if (scan_status == Synth::library_oversized) {
        Sculptor::notify_error(library_oversized_refusal, library_path);
        return false;
    }
    if (scan_status == Synth::library_invalid) {
        Sculptor::notify_error(library_invalid_save_refusal, library_path);
        return false;
    }

    if (! instr_bank.bank.channel_enabled[library_channel] || zone_count(instr_bank.bank, library_channel) == 0) {
        Sculptor::notify_error("Synth: save failed: channel %u has no instrument", library_channel + 1);
        return false;
    }

    // An existing (category, name) record is only replaced after acknowledgment.
    for (uint32_t i = 0; i < library_num_entries; i++) {
        if (Synth::library_record_matches(library_entries[i], category, name)) {
            save_confirm = true;
            snprintf(confirm_category, sizeof(confirm_category), "%s", category);
            snprintf(confirm_name, sizeof(confirm_name), "%s", name);
            return false; // the overwrite dialog takes over
        }
    }

    return finish_library_save(category, name);
}

bool Sculptor::SynthEditor::finish_library_save(const char* category, const char* name)
{
    if (! instr_bank.bank.channel_enabled[library_channel] || zone_count(instr_bank.bank, library_channel) == 0)
        return false;

    Synth::LibraryScanStatus status = Synth::library_valid;
    const int                save_error =
        Synth::save_library_record(library_path, category, name, &instr_bank, library_channel, &status);
    if (save_error) {
        if (status == Synth::library_oversized)
            Sculptor::notify_error(library_oversized_refusal, library_path);
        else
            Sculptor::notify_error("Synth: library save failed: the record is invalid or %s is not writable",
                                   library_path);
        return false;
    }

    snprintf(save_category, sizeof(save_category), "%s", category);

    Sculptor::notify_info("Synth: instrument saved to library: %s", name);

    return true;
}

void Sculptor::SynthEditor::gui_library_popups()
{
    if (library_open) {
        library_open                    = false;
        Synth::LibraryScanStatus status = Synth::library_valid;

        library_num_entries =
            Synth::read_library_index(library_path, library_entries, Synth::library_max_records, &status);
        library_scan_status = status;

        if (status == Synth::library_invalid || status == Synth::library_oversized) {
            Sculptor::notify_error(status == Synth::library_oversized ? library_oversized_refusal
                                                                      : library_invalid_open_refusal,
                                   library_path);
            return; // failed scans never open the browser: the notification reports why
        }

        library_num_categories = 0;
        for (uint32_t i = 0; i < library_num_entries && library_num_categories < Synth::library_max_records; i++) {
            uint32_t c = 0;
            while (c < library_num_categories &&
                   strncmp(library_categories[c], library_entries[i].category, Synth::library_category_len) != 0)
                c++;
            if (c == library_num_categories) {
                memcpy(library_categories[c], library_entries[i].category, Synth::library_category_len);
                library_num_categories++;
            }
        }
        library_category = library_num_categories ? 0 : -1;
        ImGui::OpenPopup("##synth_library");
    }

    gui_library_browser();

    if (save_as_open) {
        save_as_open = false;

        // Prefill from the channel: the record saves the channel's whole instrument
        // (all zones), so the channel name is the record name.  The channel may have
        // gone empty since the menu click (e.g. via undo); then there is nothing to
        // save and the popup never opens.
        if (! instr_bank.bank.channel_enabled[library_channel] || zone_count(instr_bank.bank, library_channel) == 0) {
            Sculptor::notify_error("Synth: channel %u has no instrument to save", library_channel + 1);
            return;
        }
        snprintf(save_as_name, sizeof(save_as_name), "%s", instr_bank.channel_names[library_channel]);
        snprintf(save_as_category, sizeof(save_as_category), "%s", save_category);
        ImGui::OpenPopup("##synth_library_save_as");
    }

    if (save_confirm) {
        save_confirm = false;
        ImGui::OpenPopup("##synth_library_overwrite");
    }

    gui_library_save_popups();
}

void Sculptor::SynthEditor::gui_library_browser()
{
    if (! ImGui::BeginPopup("##synth_library"))
        return;

    ImGui::Text("Library: %s", library_path);

    if (library_num_entries == 0) {
        if (library_scan_status != Synth::library_valid)
            ImGui::TextDisabled("No instruments"); // the scan failure raised a notification
        else
            ImGui::TextDisabled("No instruments: %s is empty", library_path);
    }
    else {
        ImGui::BeginChild("##synth_lib_cats", ImVec2(180, 280), true);
        for (uint32_t c = 0; c < library_num_categories; c++) {
            char label[Synth::library_category_len + 8];
            snprintf(label,
                     sizeof(label),
                     "%s##cat%u",
                     library_categories[c][0] ? library_categories[c] : "(no category)",
                     c);
            if (ImGui::Selectable(label, library_category == static_cast<int32_t>(c)))
                library_category = static_cast<int32_t>(c);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##synth_lib_instrs", ImVec2(0, 280), true);
        if (library_category >= 0) {
            const char* const category = library_categories[library_category];
            for (uint32_t i = 0; i < library_num_entries; i++) {
                if (strncmp(library_entries[i].category, category, Synth::library_category_len) != 0)
                    continue;
                char label[Synth::library_name_len + 16];
                snprintf(label, sizeof(label), "%s##lib%u", library_entries[i].name, i);
                if (ImGui::Selectable(label)) {
                    if (do_library_load(library_entries[i]))
                        ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Load replaces the channel's zoning and effect chain with the record's.");

    ImGui::EndPopup();
}

void Sculptor::SynthEditor::gui_library_save_popups()
{
    if (ImGui::BeginPopupModal("##synth_library_save_as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("Name", save_as_name, sizeof(save_as_name));
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("Category", save_as_category, sizeof(save_as_category));

        if (ImGui::Button("OK") && save_as_name[0]) {
            // A failed save keeps the popup open so the user can retry
            if (save_instrument_to_library(save_as_category, save_as_name))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("##synth_library_overwrite", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("'%s' already exists in category '%s'.", confirm_name, confirm_category);
        ImGui::TextUnformatted("Overwrite it?");

        if (ImGui::Button("Overwrite")) {
            // A failed overwrite keeps the popup open so the user can retry
            if (finish_library_save(confirm_category, confirm_name))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
