// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_edit.h"
#include "sculptor_bank_json.h"
#include "sculptor_instr_bank.h"
#include "sculptor_notifications.h"

#include "../synth/midi_input.h"
#include "../synth/realtime_synth.h"
#include "sculptor_undo.h"

#include "../core/d_printf.h"
#include "../core/gui_imgui.h"
#include <stdio.h>
#include <string.h>
#include <type_traits>

namespace {

// Auditioning submits plain note events through the synth's live-MIDI input,
// like any external keyboard; the synth knows nothing about auditioning.
// The scan-refusal messages are shared by the save path and the browser so the
// wording cannot drift between them.
const char* const library_oversized_refusal    = "Synth: %s holds a record too large to rebuild; saving is refused";
const char* const library_invalid_save_refusal = "Synth: %s is not a valid instrument library; saving is refused";
const char* const library_invalid_open_refusal = "Synth: %s is invalid or unreadable";
void              submit_audition_note(uint32_t channel, uint32_t note, bool note_on)
{
    Synth::MidiEvent event = {};
    event.event            = note_on ? Synth::EvType::note_on : Synth::EvType::note_off;
    event.channel          = static_cast<uint8_t>(channel);
    event.note             = static_cast<uint8_t>(note);
    event.note_data        = 127;
    Synth::submit_external_midi_event(event);
}

Synth::InstrumentEditorBank instr_bank; // GUI-thread-owned editable bank (names included).
Sculptor::UndoRedo          undo_redo;
constexpr uint32_t          undo_depth = 10;
uint8_t                     undo_buf[(sizeof(Synth::InstrumentEditorBank) + sizeof(uint32_t)) * undo_depth];

// Queue for shipping edited banks from the GUI to the synth audio thread.
Synth::BankUpdateQueue bank_queue;

bool                  bank_changes_pending = false;
Synth::InstrumentBank pending_bank;

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

    pending_bank         = instr_bank.bank; // names are editor-only and never reach the audio thread
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

    return publish_edited_bank();
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

    return publish_edited_bank();
}

bool save_editor_bank(const char* path)
{
    if (! Synth::validate_instrument_bank(&instr_bank.bank)) {
        Sculptor::notify_error("Synth: refusing to save an invalid instrument bank");
        return false;
    }

    const int save_error = Synth::save_editor_bank_file(path, &instr_bank);
    if (save_error) {
        Sculptor::notify_error("Synth: bank save failed: %s: %s", path, strerror(save_error));
        return false;
    }

    Sculptor::notify_info("Synth: bank saved: %s", path);

    return true;
}

// Loads a bank file into the editable bank. A decode is transactional (the decoder
// stages and validates before committing), so a corrupt file never leaves the
// editable bank half-replaced. A missing file is a fresh project at startup and
// stays silent; an explicit user open of a missing file is a visible failure.
bool load_editor_bank(const char* path, bool notify_absent)
{
    static Synth::InstrumentEditorBank scratch;

    const Synth::BankFileStatus status = Synth::load_editor_bank_file(path, &scratch);

    if (status == Synth::BankFileStatus::absent) {
        if (notify_absent)
            Sculptor::notify_error("Synth: bank file not found: %s", path);
        return false;
    }

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
    instr_bank = scratch; // names ride in the bank file
    return publish_edited_bank();
}

void init_editor()
{
    static_assert(std::is_trivially_copyable_v<Synth::InstrumentEditorBank>);

    // The player starts with an empty, silent bank; the editor restores the last
    // session or builds the factory recipe for a fresh project and publishes it.
    Synth::set_bank_source_callback(&drain_bank_updates);

    if (load_editor_bank(bank_state_path, false))
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

namespace Sculptor {

SynthEditor::SynthEditor()
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

void SynthEditor::release_held_audition()
{
    if (! audition_held)
        return;
    // Retry each frame until accepted; a refused off would leave the note sounding.
    submit_audition_note(audition_channel, audition_note, false);
    audition_held = false;
}

void SynthEditor::trigger_save()
{
    dialog_save = true;
}

void SynthEditor::trigger_load()
{
    dialog_load = true;
}

void SynthEditor::rederive_zone_selection(uint32_t channel)
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
    int32_t zone = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], last_audition_note[channel]));
    if (zone < 0)
        zone = static_cast<int32_t>(
            Synth::zone_entry_at(bank.channel_zones[channel], 0)); // zone 0 always starts at note 0
    selected_zone[channel] = zone;
    zone_tab_force_entry   = zone;
}

void SynthEditor::rederive_all_selections()
{
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++)
        rederive_zone_selection(channel);
}

bool SynthEditor::commit_candidate(const Synth::InstrumentEditorBank& candidate_bank)
{
    if (! Synth::validate_instrument_bank(&candidate_bank.bank)) {
        Sculptor::notify_error("Synth: refusing to commit an invalid bank");
        return false;
    }

    editor_snapshot();
    instr_bank = candidate_bank;

    if (! publish_edited_bank())
        d_printf("Synth: bank publish backpressured; will retry\n");

    rederive_all_selections();
    return true;
}

void SynthEditor::do_initialize(uint32_t channel)
{
    candidate = instr_bank;
    // Orphaned instruments may hold the only free pool slots, so reclaim before checking.
    Synth::reclaim_unused_slots(&candidate);

    if (! Synth::init_default_channel(&candidate.bank, channel)) {
        Sculptor::notify_error("Synth: cannot initialize channel %u: pool space exhausted", channel + 1);
        return;
    }

    candidate.bank.channel_enabled[channel] = 1;
    // The channel's old instruments are now unreferenced by its replacement zone table.
    Synth::reclaim_unused_slots(&candidate);

    selected_target = channel;
    commit_candidate(candidate);
}

void SynthEditor::do_delete(uint32_t channel)
{
    candidate = instr_bank;

    candidate.bank.channel_enabled[channel] = 0;
    Synth::get_default_channel_name(channel, candidate.channel_names[channel], Synth::max_name_len);
    memset(candidate.bank.channel_zones[channel], 0, sizeof(candidate.bank.channel_zones[channel]));
    memset(&candidate.bank.channel_chains[channel], 0, sizeof(candidate.bank.channel_chains[channel]));
    // Instruments the channel's old zone table referenced are now unreferenced.
    Synth::reclaim_unused_slots(&candidate);

    selected_target = channel;
    commit_candidate(candidate);
}

bool SynthEditor::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    (void)image_idx;
    (void)input;
    *need_realloc = false;

    pump_bank_publish();

    // A held audition note requires the left button to be down, so a release that
    // happened while this frame was not running (editor disabled, focus loss) is
    // caught here too. Window close releases at its own site; menu opens release at
    // their OpenPopup sites.
    if (! ImGui::IsMouseDown(ImGuiMouseButton_Left))
        release_held_audition();
    if (window_was_focused && ! ImGui::IsWindowFocused())
        release_held_audition();

    if (! ImGui::Begin("Synth")) {
        release_held_audition();
        ImGui::End();
        return true;
    }
    window_was_focused = ImGui::IsWindowFocused();

    // Undo/redo act only while this window is focused and no text field is being
    // edited; the geometry editor gates its own shortcuts the same way, so one
    // keystroke can never undo both editors.
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused() && ! io.WantTextInput) {
        if (is_ctrl_down() && ! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (editor_undo())
                rederive_all_selections();
        }
        if (is_ctrl_down() && ((! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
                               (is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
            if (editor_redo())
                rederive_all_selections();
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
    gui_bank_popups();
    gui_library_popups();

    ImGui::End();
    return true;
}

void SynthEditor::gui_channel_list()
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
            release_held_audition(); // opening the menu releases the held note
            menu_channel      = channel;
            channel_menu_open = true;
        }
    }
}

void SynthEditor::gui_channel_pane(uint32_t channel)
{
    const Synth::InstrumentBank& bank      = instr_bank.bank;
    const uint32_t               num_zones = zone_count(bank, channel);

    if (num_zones == 0) {
        // The empty-selection state: no instrument is selected, so the name box,
        // audition, and zone mutations stay disabled for this target.
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

    // The Oscillators view keeps a bottom strip for the keyboard. Avail inside an
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
        ImGui::TextDisabled("Oscillators editor is not available yet");
    ImGui::EndChild();

    // Audition is per-channel zone behavior, so the keyboard lives in the channel's
    // Oscillators view only.
    if (! show_effects_mode)
        gui_keyboard();
}
void SynthEditor::gui_keyboard()
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
    if (audition_held) {
        const float x     = origin.x + boundary_x_of(audition_note, white_w, black_w);
        const float key_w = is_black_note(audition_note) ? black_w : white_w;
        const float key_h = is_black_note(audition_note) ? black_h : height;
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

    if (active && hit >= 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int32_t zone =
                static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], static_cast<uint32_t>(hit)));
            if (zone >= 0) {
                selected_zone[channel]      = zone;
                last_audition_note[channel] = static_cast<uint8_t>(hit);
                zone_tab_force_entry        = zone; // keyboard click moves the tab bar too
            }
            if (! audition_held) {
                submit_audition_note(channel, static_cast<uint32_t>(hit), true);
                audition_held    = true;
                audition_channel = channel;
                audition_note    = static_cast<uint32_t>(hit);
            }
        }
        else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            release_held_audition(); // opening the menu releases the held note
            zone_menu_channel  = channel;
            zone_menu_note     = static_cast<uint32_t>(hit);
            zone_menu_from_tab = false;
            zone_menu_open     = true;
        }
    }

    ImGui::Dummy(ImVec2(width, 8.0f + height));
    ImGui::EndChild();
}

void SynthEditor::do_zone_delete(uint32_t channel, uint32_t entry)
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
        entry               = 1;
    }
    // Boundaries are stored as zone starts, so dropping the entry extends the
    // previous zone over the deleted range.
    for (uint32_t zone = entry; zone + 1 < Synth::max_instr_per_channel; zone++)
        zones[zone] = zones[zone + 1];
    memset(&zones[Synth::max_instr_per_channel - 1], 0, sizeof(zones[0]));
    Synth::reclaim_unused_slots(&candidate);
    commit_candidate(candidate);
}

void SynthEditor::gui_zone_menu()
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

void SynthEditor::do_zone_join_previous(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry <= 0)
        return;

    candidate = instr_bank;
    if (! Synth::zone_join_previous(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::do_zone_join_next(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0 || static_cast<uint32_t>(entry) + 1 >= zone_count(bank, channel))
        return;

    candidate = instr_bank;
    if (! Synth::zone_join_next(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::do_zone_split_new(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank  = instr_bank.bank;
    const int32_t                entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0)
        return;

    candidate = instr_bank;
    if (! Synth::zone_split_new(candidate.bank.channel_zones[channel], static_cast<uint32_t>(entry), note, &candidate))
        return; // pool or table full: the menu item is grayed, but stay safe

    // Splitting at the zone's first note orphans its old instrument.
    Synth::reclaim_unused_slots(&candidate);
    // The split hands the clicked key to the new zone; select it so the tab
    // bar follows the zone the user just created.
    selected_zone[channel] = static_cast<int32_t>(Synth::zone_entry_at(candidate.bank.channel_zones[channel], note));
    zone_tab_force_entry   = selected_zone[channel];
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::gui_channel_popup()
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

void SynthEditor::gui_rename_popup()
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

    // Esc is Cancel: close without committing. Every widget stays rendered so
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
        commit_candidate(candidate);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SynthEditor::gui_bank_popups()
{
    static char path[256];

    if (dialog_save) {
        snprintf(path, sizeof(path), "%s", bank_state_path);
        dialog_save = false;
        ImGui::OpenPopup("##synth_save_bank");
    }

    if (ImGui::BeginPopupModal("##synth_save_bank", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("##path", path, sizeof(path));
        if (ImGui::Button("Save")) {
            save_editor_bank(path); // failure raises a notification
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (dialog_load) {
        snprintf(path, sizeof(path), "%s", bank_state_path);
        dialog_load = false;
        ImGui::OpenPopup("##synth_load_bank");
    }

    if (ImGui::BeginPopupModal("##synth_load_bank", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("##path", path, sizeof(path));
        if (ImGui::Button("Open")) {
            if (load_editor_bank(path, true))
                rederive_all_selections(); // failure raises a notification
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

bool SynthEditor::do_library_load(const Synth::LibraryEntry& entry)
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

    // Loading enables the channel; the effect chain stays untouched. The old
    // instruments lose their zone roots and are reclaimed from the candidate.
    candidate.bank.channel_enabled[library_channel] = 1;

    // The channel takes the record's name so the library identity carries over.
    memcpy(candidate.channel_names[library_channel], entry.name, sizeof(candidate.channel_names[library_channel]));
    Synth::reclaim_unused_slots(&candidate);
    selected_target                     = library_channel;
    last_audition_note[library_channel] = 0;
    commit_candidate(candidate);

    return true;
}

bool SynthEditor::save_instrument_to_library(const char* category, const char* name)
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

bool SynthEditor::finish_library_save(const char* category, const char* name)
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

void SynthEditor::gui_library_popups()
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
        // ponytail: 64-category browser cap; raise if real libraries outgrow it
        for (uint32_t i = 0; i < library_num_entries && library_num_categories < 64; i++) {
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
        // (all zones), so the channel name is the record name. The channel may have
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

void SynthEditor::gui_library_browser()
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
    ImGui::TextDisabled("Load replaces the channel's zoning with a single all-keys zone; the effect chain is kept.");

    ImGui::EndPopup();
}

void SynthEditor::gui_library_save_popups()
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

} // namespace Sculptor
