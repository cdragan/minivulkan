// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_edit.h"
#include "sculptor_instr_bank.h"

#include "../synth/realtime_synth.h"
#include "../synth/midi_input.h"
#include "../synth/synth_serialize.h"
#include "sculptor_undo.h"

#include "../core/gui_imgui.h"
#include "../core/d_printf.h"
#include <stdio.h>
#include <string.h>
#include <type_traits>

namespace {

// Auditioning submits plain note events through the synth's live-MIDI input,
// like any external keyboard; the synth knows nothing about auditioning.
void submit_audition_note(uint32_t channel, uint32_t note, bool note_on)
{
    Synth::MidiEvent event = { };
    event.event = note_on ? Synth::EvType::note_on : Synth::EvType::note_off;
    event.channel = static_cast<uint8_t>(channel);
    event.note = static_cast<uint8_t>(note);
    event.note_data = 127;
    Synth::submit_external_midi_event(event);
}

Synth::InstrumentBank instr_bank; // GUI-thread-owned editable bank.
Sculptor::UndoRedo    undo_redo;
constexpr uint32_t    undo_depth = 10;
uint8_t               undo_buf[(sizeof(Synth::InstrumentBank) + sizeof(uint32_t)) * undo_depth];

// Queue for shipping edited banks from the GUI to the synth audio thread.
Synth::BankUpdateQueue bank_queue;

bool                  bank_changes_pending = false;
Synth::InstrumentBank pending_bank;

void undo_init_once()
{
    static bool inited = false;

    if ( ! inited) {
        undo_redo.init(undo_buf);
        inited = true;
    }
}

bool pump_bank_publish()
{
    if (bank_changes_pending) {
        if ( ! Synth::push_bank_update(&bank_queue, pending_bank)) {
            return false;
        }

        bank_changes_pending = false;
    }

    return true;
}

bool publish_edited_bank()
{
    if ( ! Synth::validate_instrument_bank(&instr_bank)) {
        d_printf("Error: refusing to publish an invalid instrument bank\n");
        return false;
    }

    pending_bank         = instr_bank;
    bank_changes_pending = true;

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
    if ( ! undo_redo.finish_redo_push())
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
    if ( ! undo_redo.finish_undo_push())
        return false;

    if ( ! undo_redo.init_redo())
        return false;

    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_redo();

    return publish_edited_bank();
}

void init_editor()
{
    static_assert(std::is_trivially_copyable_v<Synth::InstrumentBank>);
    instr_bank = Synth::current_bank();

    if (memcmp(&instr_bank, &Synth::current_bank(), sizeof(instr_bank)))
        d_printf("Editor bank snapshot mismatch\n");

    Synth::set_bank_source_callback(&drain_bank_updates);
}

bool save_editor_bank(const char* path)
{
    if ( ! Synth::validate_instrument_bank(&instr_bank)) {
        d_printf("Error: refusing to save an invalid instrument bank\n");
        return false;
    }
    return save_instrument_bank(path, &instr_bank);
}

bool load_editor_bank(const char* path)
{
    // Decode into a scratch candidate and only commit a bank that validates, so a corrupt
    // file can never leave the editable bank half-replaced.
    static Synth::InstrumentBank scratch;

    if ( ! load_instrument_bank(path, &scratch))
        return false;

    if ( ! Synth::validate_instrument_bank(&scratch)) {
        d_printf("Error: refusing to load an invalid instrument bank\n");
        return false;
    }

    editor_snapshot();
    instr_bank = scratch;
    return publish_edited_bank();
}

static_assert(sizeof(undo_buf) <= undo_depth * (sizeof(Synth::InstrumentBank) + sizeof(uint32_t)));
static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);

// Transactional command scratch: structural commands mutate this candidate, validate it,
// and only then commit it over the editable bank (see commit_candidate).
Synth::InstrumentBank candidate;

const char* default_channel_name(char* buf, uint32_t channel)
{
    snprintf(buf, Synth::max_name_len, "Channel %02u", channel + 1);
    return buf;
}

uint32_t zone_count(const Synth::InstrumentBank& bank, uint32_t channel)
{
    uint32_t num = 0;
    while (num < Synth::max_instr_per_channel && bank.channel_zones[channel][num].start_note)
        ++num;
    return num;
}

constexpr bool is_black_note(uint32_t note)
{
    switch (note % 12) {
    case 1: case 3: case 6: case 8: case 10:
        return true;
    default:
        return false;
    }
}

// Black keys below pitch class p within its octave: C# D# F# G# A# = pcs 1 3 6 8 10.
constexpr uint32_t black_notes_below_pc(uint32_t pc)
{
    switch (pc) {
    case 0: case 1: return 0;
    case 2: case 3: return 1;
    case 4: case 5: case 6: return 2;
    case 7: case 8: return 3;
    case 9: case 10: return 4;
    default: return 5;
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
    if ( ! editor_bank_initialized) {
        editor_bank_initialized = true;
        init_editor();
    }
    return true;
}

void SynthEditor::release_held_audition()
{
    if ( ! audition_held)
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
    const Synth::InstrumentBank& bank = instr_bank;

    // Empty/disabled channels hold no selection: slot 0 of a cleared table identifies an
    // unallocated instrument, and a disabled channel's table may hold stale bytes.
    if ( ! bank.channel_enabled[channel] || bank.channel_zones[channel][0].start_note == 0) {
        selected_zone[channel] = -1;
        return;
    }

    int32_t zone = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], last_audition_note[channel]));
    if (zone < 0)
        zone = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], 0)); // zone 0 always starts at note 0
    selected_zone[channel] = zone;
}

void SynthEditor::rederive_all_selections()
{
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++)
        rederive_zone_selection(channel);
}

bool SynthEditor::commit_candidate(const Synth::InstrumentBank& candidate_bank)
{
    if ( ! Synth::validate_instrument_bank(&candidate_bank)) {
        d_printf("Synth: refusing to commit an invalid bank\n");
        return false;
    }

    editor_snapshot();
    Synth::InstrumentBank& bank = instr_bank;
    bank = candidate_bank;

    if ( ! publish_edited_bank())
        d_printf("Synth: bank publish backpressured; will retry\n");

    rederive_all_selections();
    return true;
}

void SynthEditor::do_initialize(uint32_t channel)
{
    const Synth::InstrumentBank& bank = instr_bank;
    candidate = bank;
    // Orphaned instruments may hold the only free pool slots, so reclaim before checking.
    Synth::reclaim_unused_slots(&candidate);

    if ( ! Synth::init_default_channel(&candidate, channel)) {
        d_printf("Synth: cannot initialize channel %u: pool space exhausted\n", channel);
        return;
    }

    candidate.channel_enabled[channel] = 1;
    // The channel's old instruments are now unreferenced by its replacement zone table.
    Synth::reclaim_unused_slots(&candidate);

    selected_target = channel;
    commit_candidate(candidate);
}

void SynthEditor::do_delete(uint32_t channel)
{
    const Synth::InstrumentBank& bank = instr_bank;
    candidate = bank;

    candidate.channel_enabled[channel] = 0;
    default_channel_name(candidate.channel_names[channel], channel);
    memset(candidate.channel_zones[channel], 0, sizeof(candidate.channel_zones[channel]));
    memset(&candidate.channel_chains[channel], 0, sizeof(candidate.channel_chains[channel]));
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
    if ( ! ImGui::IsMouseDown(ImGuiMouseButton_Left))
        release_held_audition();
    if (window_was_focused && ! ImGui::IsWindowFocused())
        release_held_audition();

    if ( ! ImGui::Begin("Synth")) {
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
        if (is_ctrl_down() &&
                (( ! is_shift_down() && ImGui::IsKeyPressed(ImGuiKey_Y)) ||
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

    ImGui::End();
    return true;
}

void SynthEditor::gui_channel_list()
{
    const Synth::InstrumentBank& bank = instr_bank;

    if (ImGui::Selectable("Master Effects", selected_target == target_master))
        selected_target = target_master;

    ImGui::Separator();

    char label[Synth::max_name_len];
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        const bool enabled = bank.channel_enabled[channel];
        const char* const name = enabled ? bank.channel_names[channel]
                                         : default_channel_name(label, channel);

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

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            release_held_audition(); // opening the menu releases the held note
            menu_channel = channel;
            ImGui::OpenPopup("##synth_channel_menu");
        }
    }
}

void SynthEditor::gui_channel_pane(uint32_t channel)
{
    const Synth::InstrumentBank& bank = instr_bank;
    const uint32_t num_zones = zone_count(bank, channel);

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
        zone = 0;
        selected_zone[channel] = 0;
    }

    const uint32_t instrument = bank.channel_zones[channel][zone].instrument;

    // The name box owns its buffer while active; it re-syncs from the bank only when
    // the user is not editing (selection change, undo/redo, external commit).
    ImGui::SetNextItemWidth(-140.0f); // leave room for the mode toggle buttons
    if (ImGui::InputText("##zone_name", name_buf, sizeof(name_buf))) {
        if (ImGui::IsItemDeactivatedAfterEdit() && memcmp(name_buf, bank.instrument_names[instrument], sizeof(name_buf)) != 0) {
            const Synth::InstrumentBank& current = instr_bank;
            candidate = current;
            memcpy(candidate.instrument_names[instrument], name_buf, sizeof(name_buf));
            commit_candidate(candidate);
        }
    }
    if ( ! ImGui::IsItemActive()) {
        if (memcmp(name_buf, bank.instrument_names[instrument], sizeof(name_buf)) != 0) {

            memcpy(name_buf, bank.instrument_names[instrument], sizeof(name_buf));
        }
    }

    ImGui::SameLine();
    if ( ! show_effects_mode)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
    if (ImGui::Button("Oscillators##synth_mode"))
        show_effects_mode = false;
    if ( ! show_effects_mode)
        ImGui::PopStyleColor();
    ImGui::SameLine();
    if (show_effects_mode)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
    if (ImGui::Button("Effects##synth_mode"))
        show_effects_mode = true;
    if (show_effects_mode)
        ImGui::PopStyleColor();

    ImGui::Separator();

    char zone_label[Synth::max_name_len + 8];
    for (uint32_t entry = 0; entry < num_zones; entry++) {
        char name[Synth::max_name_len];
        Synth::get_zone_name(&bank, channel, entry, name, sizeof(name));
        // The ##suffix keeps the item id unique when two zones share an instrument name.
        snprintf(zone_label, sizeof(zone_label), "%s##zone%u", name, entry);
        if (ImGui::Selectable(zone_label, selected_zone[channel] == static_cast<int32_t>(entry)))
            selected_zone[channel] = static_cast<int32_t>(entry);
    }

    ImGui::Separator();

    // The Oscillators view keeps a bottom strip for the keyboard. Avail inside an
    // auto-height table cell is unbounded down to the window bottom, so subtract what
    // the layout adds below the placeholder: the cell padding under the row, plus the
    // item spacing before the keyboard child; otherwise the column overflows the
    // window into a scrollbar.
    const float keyboard_h = show_effects_mode ? 0.0f : 72.0f;
    float placeholder_h = ImGui::GetContentRegionAvail().y - keyboard_h - ImGui::GetStyle().CellPadding.y;
    if ( ! show_effects_mode)
        placeholder_h -= ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##synth_pane_ph", ImVec2(0, placeholder_h < 0.0f ? 0.0f : placeholder_h), true);
    if (show_effects_mode)
        ImGui::TextDisabled("Effects editor is not available yet");
    else
        ImGui::TextDisabled("Oscillators editor is not available yet");
    ImGui::EndChild();

    // Audition is per-channel zone behavior, so the keyboard lives in the channel's
    // Oscillators view only.
    if ( ! show_effects_mode)
        gui_keyboard();
}

void SynthEditor::gui_keyboard()
{
    // Zero window padding so the 72px child holds exactly the 8px report margin and the
    // 64px key strip; default padding would overflow the content into a scrollbar.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##synth_keyboard", ImVec2(0, 72), true);
    ImGui::PopStyleVar();

    const Synth::InstrumentBank& bank = instr_bank;
    const bool channel_target = selected_target != target_master;
    const uint32_t channel = channel_target ? selected_target : 0;
    const bool active = channel_target && bank.channel_enabled[channel];

    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 64.0f;
    const ImVec2 origin = ImVec2(top.x, top.y + 8.0f);
    const float white_w = width / 75.0f; // 75 white keys across the full MIDI range
    const float black_w = white_w * 0.6f;
    const float black_h = height * 0.6f;

    // Fixed piano colors: a dark theme would otherwise invert the keys.
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const uint32_t color_white = IM_COL32(240, 240, 240, 255);
    const uint32_t color_line = IM_COL32(120, 120, 120, 255);
    const uint32_t color_black_key = IM_COL32(35, 35, 35, 255);
    const uint32_t color_held = IM_COL32(90, 160, 255, 255);

    for (uint32_t wk = 0; wk < 75; wk++) {
        const float x = origin.x + static_cast<float>(wk) * white_w;
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + white_w, origin.y + height), color_white);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), color_line);
    }
    draw->AddLine(ImVec2(origin.x + width, origin.y), ImVec2(origin.x + width, origin.y + height), color_line);

    for (uint32_t note = 0; note < 128; note++) {
        if ( ! is_black_note(note))
            continue;
        const float x = origin.x + boundary_x_of(note, white_w, black_w);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + black_w, origin.y + black_h), color_black_key);
    }

    // Zone boundaries: a separator at each zone's first note, colored by zone index.
    if (active) {
        static const uint32_t zone_palette[8] = {
            IM_COL32(255, 80, 80, 255), IM_COL32(80, 255, 120, 255), IM_COL32(80, 160, 255, 255),
            IM_COL32(255, 200, 60, 255), IM_COL32(200, 100, 255, 255), IM_COL32(60, 220, 220, 255),
            IM_COL32(255, 120, 200, 255), IM_COL32(180, 180, 80, 255)
        };

        const Synth::Zone* const zones = bank.channel_zones[channel];
        for (uint32_t entry = 1; entry < Synth::max_instr_per_channel; entry++) {
            if (zones[entry].start_note == 0)
                break;
            const float x = origin.x + boundary_x_of(zones[entry].start_note - 1, white_w, black_w);
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), zone_palette[entry % 8]);
        }
    }

    // The held note is highlighted on top of everything else.
    if (audition_held) {
        const float x = origin.x + boundary_x_of(audition_note, white_w, black_w);
        const float key_w = is_black_note(audition_note) ? black_w : white_w;
        const float key_h = is_black_note(audition_note) ? black_h : height;
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + key_w, origin.y + key_h), color_held);
    }

    // Hit test: black keys are on top, then white keys.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool over = mouse.x >= origin.x && mouse.x < origin.x + width
                      && mouse.y >= origin.y && mouse.y < origin.y + height;
    int32_t hit = -1;
    if (over) {
        for (uint32_t note = 0; note < 128 && hit < 0; note++) {
            if ( ! is_black_note(note))
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
            const int32_t zone = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], static_cast<uint32_t>(hit)));
            if (zone >= 0) {
                selected_zone[channel] = zone;
                last_audition_note[channel] = static_cast<uint8_t>(hit);
            }
            if ( ! audition_held) {
                submit_audition_note(channel, static_cast<uint32_t>(hit), true);
                audition_held = true;
                audition_channel = channel;
                audition_note = static_cast<uint32_t>(hit);
            }
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            release_held_audition(); // opening the menu releases the held note
            zone_menu_channel = channel;
            zone_menu_note = static_cast<uint32_t>(hit);
            ImGui::OpenPopup("##synth_zone_menu");
        }
    }

    ImGui::Dummy(ImVec2(width, 8.0f + height));
    ImGui::EndChild();
}

void SynthEditor::gui_zone_menu()
{
    if ( ! ImGui::BeginPopup("##synth_zone_menu"))
        return;

    const uint32_t channel = zone_menu_channel;
    const uint32_t note = zone_menu_note;
    const Synth::InstrumentBank& bank = instr_bank;
    const Synth::Zone* const zones = bank.channel_zones[channel];
    const int32_t entry = static_cast<int32_t>(Synth::zone_entry_at(zones, note));
    const uint32_t num_zones = zone_count(bank, channel);

    const bool has_next = entry >= 0 && static_cast<uint32_t>(entry) + 1 < num_zones;
    // A split at a zone's first note swaps the instrument in place; any other split
    // inserts an entry after it, which a full 16-slot table refuses.
    const bool first_note = entry >= 0 && note + 1 == zones[entry].start_note;
    const bool table_full = ! first_note && num_zones >= Synth::max_instr_per_channel;

    if (ImGui::MenuItem("Add to previous zone", nullptr, entry > 0))
        do_zone_join_previous(channel, note);
    if (ImGui::MenuItem("Add to next zone", nullptr, has_next))
        do_zone_join_next(channel, note);
    if (ImGui::MenuItem("Create new zone", nullptr, entry >= 0 && ! table_full
                        && bank.instruments.num_allocated < Synth::max_instruments))
        do_zone_split_new(channel, note);

    ImGui::EndPopup();
}

void SynthEditor::do_zone_join_previous(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank = instr_bank;
    const int32_t entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry <= 0)
        return;

    candidate = bank;
    if ( ! Synth::zone_join_previous(candidate.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::do_zone_join_next(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank = instr_bank;
    const int32_t entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0 || static_cast<uint32_t>(entry) + 1 >= zone_count(bank, channel))
        return;

    candidate = bank;
    if ( ! Synth::zone_join_next(candidate.channel_zones[channel], static_cast<uint32_t>(entry), note))
        return;

    // The zone may have been dropped and its instrument orphaned.
    Synth::reclaim_unused_slots(&candidate);
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::do_zone_split_new(uint32_t channel, uint32_t note)
{
    const Synth::InstrumentBank& bank = instr_bank;
    const int32_t entry = static_cast<int32_t>(Synth::zone_entry_at(bank.channel_zones[channel], note));
    if (entry < 0)
        return;

    candidate = bank;
    if ( ! Synth::zone_split_new(candidate.channel_zones[channel], static_cast<uint32_t>(entry), note, &candidate))
        return; // pool or table full: the menu item is grayed, but stay safe

    // Splitting at the zone's first note orphans its old instrument.
    Synth::reclaim_unused_slots(&candidate);
    last_audition_note[channel] = static_cast<uint8_t>(note);
    commit_candidate(candidate);
}

void SynthEditor::gui_channel_popup()
{
    if ( ! ImGui::BeginPopup("##synth_channel_menu"))
        return;

    const uint32_t channel = menu_channel;

    if (ImGui::MenuItem("Rename...")) {
        rename_popup_open = false; // force a re-sync of rename_buf on open
        ImGui::OpenPopup("##synth_rename");
    }
    ImGui::Separator();
    // Per-channel records are not available yet; the items stay visible but disabled.
    ImGui::MenuItem("Load...", nullptr, false);
    ImGui::MenuItem("Save...", nullptr, false);
    ImGui::MenuItem("Save As...", nullptr, false);
    ImGui::Separator();
    if (ImGui::MenuItem("Initialize"))
        do_initialize(channel);
    if (ImGui::MenuItem("Delete"))
        do_delete(channel);

    ImGui::EndPopup();
}

void SynthEditor::gui_rename_popup()
{
    if ( ! ImGui::BeginPopupModal("##synth_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        rename_popup_open = false;
        return;
    }

    if ( ! rename_popup_open) {
        rename_popup_open = true;
        memcpy(rename_buf, instr_bank.channel_names[menu_channel], sizeof(rename_buf));
    }

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("##rename", rename_buf, sizeof(rename_buf));

    if (ImGui::Button("OK") && rename_buf[0]) {
        const Synth::InstrumentBank& current = instr_bank;
        candidate = current;
        memcpy(candidate.channel_names[menu_channel], rename_buf, sizeof(rename_buf));
        commit_candidate(candidate);
        rename_popup_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        rename_popup_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void SynthEditor::gui_bank_popups()
{
    static char path[256];

    if (dialog_save) {
        snprintf(path, sizeof(path), "instrument_bank.synth");
        dialog_save = false;
        ImGui::OpenPopup("##synth_save_bank");
    }

    if (ImGui::BeginPopupModal("##synth_save_bank", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("##path", path, sizeof(path));
        if (ImGui::Button("Save")) {
            if ( ! save_editor_bank(path))
                d_printf("Synth: bank save failed: %s\n", path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (dialog_load) {
        path[0] = 0;
        dialog_load = false;
        ImGui::OpenPopup("##synth_load_bank");
    }

    if (ImGui::BeginPopupModal("##synth_load_bank", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("##path", path, sizeof(path));
        if (ImGui::Button("Open")) {
            if (load_editor_bank(path))
                rederive_all_selections();
            else
                d_printf("Synth: bank load failed: %s\n", path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace Sculptor
