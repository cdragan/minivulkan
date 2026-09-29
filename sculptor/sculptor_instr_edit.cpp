// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_edit.h"
#include "sculptor_bank_json.h"
#include "sculptor_graph.h"
#include "sculptor_instr_bank.h"
#include "sculptor_notifications.h"
#include "sculptor_osc_graph.h"

#include "../synth/midi_input.h"
#include "../synth/realtime_synth.h"
#include "sculptor_undo.h"

#include "../core/d_printf.h"
#include "../core/gui_imgui.h"
#include <stdio.h>
#include <string.h>
#include <type_traits>

namespace {

// The scan-refusal messages are shared by the save path and the browser so the
// wording cannot drift between them.
const char* const library_oversized_refusal    = "Synth: %s holds a record too large to rebuild; saving is refused";
const char* const library_invalid_save_refusal = "Synth: %s is not a valid instrument library; saving is refused";
const char* const library_invalid_open_refusal = "Synth: %s is invalid or unreadable";
Synth::InstrumentEditorBank instr_bank; // GUI-thread-owned editable bank (names included).
Sculptor::UndoRedo          undo_redo;
constexpr uint32_t          undo_depth = 10;
uint8_t                     undo_buf[(sizeof(Synth::InstrumentEditorBank) + sizeof(uint32_t)) * undo_depth];

// Queue for shipping edited banks from the GUI to the synth audio thread.
Synth::BankUpdateQueue bank_queue;

bool                  bank_changes_pending = false;
Synth::InstrumentBank pending_bank;

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
OscCanvasCommand osc_canvas_command         = osc_cmd_none;
uint32_t         osc_canvas_retarget_node   = Sculptor::pool_no_slot; // pending osc_cmd_change_target_param
uint32_t         osc_canvas_retarget_target = 0;                      // pending osc_cmd_change_target_param

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

void init_osc_graph_widget();

// The delete veto needs the graph to report a specific refusal, so the
// mapping rides with a pointer to the widget instance it describes.
struct OscDeleteVetoContext {
    const Sculptor::OscGraphMapping* mapping;
    Sculptor::Graph*                 graph;
};
OscDeleteVetoContext osc_veto_context = { &osc_mapping, &osc_graph };

// Fixed nodes are structural: the MIDI input nodes and the oscillator sum
// node cannot be deleted.  Oscillator layer nodes remove their layer and
// generator nodes are freely deletable, so neither is vetoed - except the
// last remaining oscillator layer: an instrument needs one layer, and a
// commit-time refusal would delete the node for one frame and resurrect it.
bool osc_node_delete_veto(void* user_data, uint32_t node_idx)
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
void osc_canvas_menu(void* user_data)
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
bool osc_node_menu(void* user_data, uint32_t node_idx)
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

// Layout of every node as the projection built it: a node whose live layout
// still matches needs no kind-0 record, so records stay sparse.
vmath::vec2 osc_projected_position[Sculptor::max_nodes];
float       osc_projected_width[Sculptor::max_nodes];
float       osc_projected_height[Sculptor::max_nodes];

// Whether an occupied node's live layout still matches the projected
// snapshot: a node the user has not touched needs no kind-0 record.
bool osc_node_layout_moved(uint32_t node)
{
    const Sculptor::Node& node_ref = osc_graph.node(node);
    return node_ref.position.x != osc_projected_position[node].x ||
           node_ref.position.y != osc_projected_position[node].y ||
           node_ref.content_width_override != osc_projected_width[node] ||
           node_ref.content_height_override != osc_projected_height[node];
}

// Re-bases one node's snapshot on its live layout, after a commit or a
// re-projection.
void osc_snapshot_node_layout(uint32_t node)
{
    const Sculptor::Node& node_ref = osc_graph.node(node);
    osc_projected_position[node]   = node_ref.position;
    osc_projected_width[node]      = node_ref.content_width_override;
    osc_projected_height[node]     = node_ref.content_height_override;
}

// Rebuilds the graph for one zone from the committed bank and drains the
// projection's own construction traffic, so projection events never echo
// into commits.  Also clears the undo group tag: a re-projected graph starts
// a new edit context.
void reproject_osc_graph(uint32_t channel, uint32_t zone)
{
    osc_graph_zone_channel = channel;
    osc_graph_zone_index   = zone;
    osc_graph_projected    = Sculptor::project_editor_to_graph(instr_bank, &osc_graph, &osc_mapping, channel, zone);
    if (osc_graph_projected) {
        for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
            if (osc_graph.node_occupied(node)) {
                osc_snapshot_node_layout(node);
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

void init_osc_graph_widget()
{
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    osc_graph.set_colors(Sculptor::default_graph_colors());
    osc_graph.set_validator(&Sculptor::osc_graph_validate, &osc_mapping);
    osc_graph.set_delete_veto(&osc_node_delete_veto, &osc_veto_context);
    osc_graph.set_canvas_menu_callback(&osc_canvas_menu, nullptr);
    osc_graph.set_node_menu_callback(&osc_node_menu, nullptr);
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
uint32_t osc_moved_node(uint32_t* moved_count)
{
    *moved_count    = 0;
    uint32_t lowest = Sculptor::pool_no_slot;
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (! osc_graph.node_occupied(node) || ! osc_node_layout_moved(node)) {
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
void refresh_osc_projected_layout()
{
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (osc_graph.node_occupied(node)) {
            osc_snapshot_node_layout(node);
        }
    }
}

// Moves one existing layout record onto a node's live position.
void write_record_position(Synth::InstrumentEditorBank* bank, int32_t record_idx, const Sculptor::Node& node_ref)
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
bool sync_osc_graph_layout(Synth::InstrumentEditorBank* bank)
{
    const uint32_t channel = osc_graph_zone_channel;
    const uint32_t zone    = osc_graph_zone_index;
    for (uint32_t node = 0; node < Sculptor::max_nodes; ++node) {
        if (! osc_graph.node_occupied(node)) {
            continue;
        }
        const Sculptor::Node& node_ref = osc_graph.node(node);
        const bool            moved    = osc_node_layout_moved(node);
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

void undo_init_once()
{
    static bool inited = false;

    if (! inited) {
        undo_redo.init(undo_buf);
        inited = true;
    }
}

bool pump_bank_publish()
{
    if (bank_changes_pending) {
        if (! Synth::push_bank_update(&bank_queue, pending_bank)) {
            return false;
        }

        bank_changes_pending = false;
    }

    return true;
}

static const char bank_state_path[] = "assets/instrument_bank.synth";

bool publish_edited_bank()
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

    const int save_error = Synth::save_editor_bank_file(bank_state_path, &instr_bank);
    if (save_error)
        Sculptor::notify_error("Synth: cannot write %s: %s", bank_state_path, strerror(save_error));

    return pump_bank_publish();
}

void drain_bank_updates()
{
    while (const Synth::InstrumentBank* const packet = Synth::peek_bank_update(&bank_queue)) {
        Synth::set_current_bank(*packet);
        Synth::consume_bank_update(&bank_queue);
    }
}

void editor_snapshot()
{
    undo_init_once();

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo_push();

    undo_redo.clear_redo();
}

bool editor_undo()
{
    undo_init_once();

    if (undo_redo.undo_empty())
        return false;

    undo_redo.init_redo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    if (! undo_redo.finish_redo_push())
        return false;

    undo_redo.init_undo();
    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo();

    // Backpressure only defers the audible switch; the restoration itself
    // succeeded, so the caller must resynchronize selections either way.
    publish_edited_bank();
    return true;
}

bool editor_redo()
{
    undo_init_once();

    if (undo_redo.redo_empty())
        return false;

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    if (! undo_redo.finish_undo_push())
        return false;

    if (! undo_redo.init_redo())
        return false;

    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_redo();

    // Backpressure only defers the audible switch; the restoration itself
    // succeeded, so the caller must resynchronize selections either way.
    publish_edited_bank();
    return true;
}

// Loads a bank file into the editable bank.  A decode is transactional (the decoder
// stages and validates before committing), so a corrupt file never leaves the
// editable bank half-replaced.  The only caller is startup, where a missing file
// is a fresh project and stays silent.
bool load_editor_bank(const char* path)
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

    editor_snapshot();
    instr_bank          = scratch; // names ride in the bank file
    osc_graph_reproject = true;
    return publish_edited_bank();
}

void init_editor()
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

static_assert(sizeof(undo_buf) <= undo_depth * (sizeof(Synth::InstrumentEditorBank) + sizeof(uint32_t)));
static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);

// Transactional command scratch: structural commands mutate this candidate, validate it,
// and only then commit it over the editable bank (see commit_candidate).
Synth::InstrumentEditorBank candidate;

uint32_t zone_count(const Synth::InstrumentBank& bank, uint32_t channel)
{
    uint32_t num = 0;
    while (num < Synth::max_instr_per_channel && bank.channel_zones[channel][num].start_note)
        ++num;
    return num;
}

// The zone whose instrument Save/Save As write: the selected zone, or the note-0 zone
// when the selection is out of range. pool_no_slot for an empty or disabled channel.
uint32_t save_zone_entry(const Synth::InstrumentBank& bank, uint32_t channel, int32_t selected)
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

constexpr bool is_black_note(uint32_t note)
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
constexpr uint32_t black_notes_below_pc(uint32_t pc)
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
constexpr uint32_t white_index_of(uint32_t note)
{
    return note - 5u * (note / 12u) - black_notes_below_pc(note % 12u);
}

// The note drawn on white key wk (0..74).
constexpr uint32_t note_at_white(uint32_t wk)
{
    constexpr uint32_t white_in_octave[7] = { 0, 2, 4, 5, 7, 9, 11 };
    return 12u * (wk / 7u) + white_in_octave[wk % 7u];
}

// Left edge of the key (white or black) that starts at `note`, relative to the
// keyboard's origin; zone boundaries are drawn here.
float boundary_x_of(uint32_t note, float white_w, float black_w)
{
    float x = static_cast<float>(white_index_of(note)) * white_w;
    if (is_black_note(note))
        x -= black_w * 0.5f;
    return x;
}

} // anonymous namespace

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
        editor_snapshot();
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
}

bool Sculptor::SynthEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    (void)image_idx;
    (void)input;
    *need_realloc = false;

    // Publish pumping lives in delayed_updates(), which runs every frame
    // regardless of the enabled flag.

    if (! ImGui::Begin("Synth")) {
        ImGui::End();
        return true;
    }

    // Focus queries must run inside the Synth window's Begin scope: outside it
    // they compare against whatever window is current, not Synth.

    // Undo/redo act only while this window is focused and no text field is being
    // edited; the geometry editor gates its own shortcuts the same way, so one
    // keystroke can never undo both editors.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused() && ! io.WantTextInput) {
        if (is_ctrl_down() && ! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (editor_undo()) {
                osc_graph_reproject = true;
                rederive_all_selections();
            }
        }
        if (is_ctrl_down() && ((! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
                               (is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
            if (editor_redo()) {
                osc_graph_reproject = true;
                rederive_all_selections();
            }
        }
    }

    ImGui::BeginTable("##synth_layout", 2, ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("##channels", ImGuiTableColumnFlags_WidthFixed, 220.0f);
    // A stretch column claims the remaining width immediately; a fit column would
    // re-fit its width over several frames and make the pane look like it animates in.
    ImGui::TableSetupColumn("##pane", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    gui_channel_list();
    ImGui::TableSetColumnIndex(1);
    if (selected_target == target_master) {
        ImGui::TextDisabled("Master effects editor is not available yet");
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

    if (ImGui::BeginTabBar("synth_mode")) {
        if (ImGui::BeginTabItem("Oscillators")) {
            show_effects_mode = false;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            show_effects_mode = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
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
    const float keyboard_h    = show_effects_mode ? 0.0f : 72.0f;
    float       placeholder_h = ImGui::GetContentRegionAvail().y - keyboard_h - ImGui::GetStyle().CellPadding.y;
    if (! show_effects_mode)
        placeholder_h -= ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##synth_pane_ph", ImVec2(0, placeholder_h < 0.0f ? 0.0f : placeholder_h), true);
    if (show_effects_mode)
        ImGui::TextDisabled("Effects editor is not available yet");
    else
        gui_osc_graph(channel);
    ImGui::EndChild();

    // Zone selection is per-channel behavior, so the keyboard lives in the
    // channel's Oscillators view only.
    if (! show_effects_mode)
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
        // frame during a drag would run the full publish path, including the
        // bank-file write, on every gesture frame.
        uint32_t       moved_count = 0;
        const uint32_t moved_node  = osc_moved_node(&moved_count);
        if (moved_node == Sculptor::pool_no_slot || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            return;
        }
        candidate                      = instr_bank;
        const uint32_t layout_tag_kind = moved_count > 1 ? osc_tag_fresh : osc_tag_graph_base;
        const uint32_t layout_tag_id0  = moved_count > 1 ? ++osc_fresh_tag_serial : moved_node;
        if (sync_osc_graph_layout(&candidate) &&
            commit_candidate(candidate, Sculptor::UndoGroupTag{ layout_tag_kind, layout_tag_id0, 0 })) {
            refresh_osc_projected_layout();
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
    refresh_osc_projected_layout();
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

// Canvas menu "Add Oscillator": one more oscillator layer at the menu
// position, with the model's fresh-Oscillator defaults.  Node capacity is
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
        const uint32_t        slot = candidate.bank.lfos.allocate();
        Synth::LFODescriptor& lfo  = candidate.bank.lfos.entries[slot];
        lfo                        = Synth::LFODescriptor{};
        lfo.wave                   = Synth::WaveType::sine_wave;
        lfo.period_ms              = 300; // a zero period would not oscillate
        lfo.min_value              = -1.0f;
        lfo.min_max_delta          = 2.0f;
        record.index               = static_cast<uint8_t>(slot + 1);
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

    if (active && hit >= 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int32_t zone =
                static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], static_cast<uint32_t>(hit)));
            if (zone >= 0) {
                selected_zone[channel]     = zone;
                last_clicked_note[channel] = static_cast<uint8_t>(hit);
                zone_tab_force_entry       = zone; // keyboard click moves the tab bar too
            }
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
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
