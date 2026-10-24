// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_editor.h"
#include "sculptor_instr_library.h"

#include <stdint.h>
#include <string.h>

namespace Sculptor {

struct UndoGroupTag;

class SynthEditor : public Editor {
public:
    SynthEditor();

    ~SynthEditor() override = default;

    const char* get_editor_name() const override { return "Synth"; }

    bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;

    // Per-frame work that must run even when the editor is disabled: publish
    // pumping.  Called before the editors loop.
    void delayed_updates();
    bool allocate_resources() override;

    void free_resources() override {}

    bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override { return true; }

private:
    static constexpr uint32_t target_master = Synth::max_channels;

    void rederive_zone_selection(uint32_t channel);
    void rederive_all_selections();
    void apply_edit_origin(uint32_t origin);
    bool commit_candidate(const Synth::InstrumentEditorBank& candidate, Sculptor::UndoGroupTag tag);
    // Projects the bank onto the oscillator graph and binds the envelope
    // curve state widgets onto the envelope nodes.
    void reproject_osc_graph(uint32_t channel, uint32_t zone);
    // State-widget entry for envelope curve editing; the per-node context
    // carries the editor pointer so the thunk can commit through the normal
    // candidate path.  render_scale scales the chart width to the node's
    // current on-screen content size.
    static int envelope_state_widget_entry(void* user_data, float render_scale);
    // Effects-graph counterparts: the modulation panel entry and its
    // one-field edit dispatcher, both committing through the candidate path.
    void reproject_fx_graph(uint32_t chain);
    void drain_projection_ring();
    bool commit_graph_node_move(bool osc_pane, uint32_t node_tag, const char* refusal_text);
    void gui_fx_graph(uint32_t chain);
    void drain_fx_graph();
    void run_fx_canvas_command();
    void do_fx_add_effect(Synth::EffectType type);
    void do_fx_add_lfo(bool from_drop);
    void do_fx_change_type(uint32_t node_idx, Synth::EffectType type);
    // Exchanges the node's effect with its adjacent projected neighbor
    // (direction -1 = previous, +1 = next); a missing neighbor does nothing.
    void do_fx_swap(uint32_t node_idx, int32_t direction);
    void do_fx_rejoin_lane(uint32_t node_idx);
    // One added-CC registry-bit edit shared by both panes' handlers.
    void edit_added_cc_mask(uint32_t mask_idx, uint32_t cc_number, bool add, uint32_t undo_tag, bool* reproject);
    void do_fx_add_cc(uint32_t cc_number);
    void do_fx_remove_cc(uint32_t cc_number);
    void do_initialize(uint32_t channel);
    void do_delete(uint32_t channel);
    void do_zone_join(uint32_t channel, uint32_t note, bool previous);
    void do_zone_split_new(uint32_t channel, uint32_t note);
    void do_zone_delete(uint32_t channel, uint32_t entry);
    void do_zone_copy_instrument(uint32_t channel, uint32_t note);
    void do_zone_paste_instrument(uint32_t channel, uint32_t note);
    void gui_osc_graph(uint32_t channel);
    void drain_osc_graph(uint32_t channel, uint32_t zone);
    void run_osc_canvas_command();
    // The canvas menu passes the unsnapped popup position and pool_no_slot
    // (canvas Add wires into the lowest free input slot, red holes first);
    // the drop variants pass the snapped release position and the dragged
    // slot (a red hole or the gray add slot) - both converge on one add
    // routine that appends the fresh oscillator at the next editor ordinal
    // with default parameters.  A generator drop also passes the binding
    // cell to preset (the target oscillator's editor ordinal); the
    // detached-canvas variants pass the sentinels (max_layers /
    // num_mod_targets) and create a free-standing node only.
    void do_osc_add_oscillator(vmath::vec2 position, uint32_t dragged_slot);
    void do_osc_add_generator(bool is_env, vmath::vec2 position, uint32_t bind_layer, uint32_t bind_target);
    void do_osc_add_cc(uint32_t cc_number);
    void do_osc_remove_cc(uint32_t cc_number);

    void        gui_channel_list();
    void        gui_channel_pane(uint32_t channel);
    void        gui_keyboard();
    void        release_held_note();
    void        gui_channel_popup();
    void        gui_zone_menu();
    void        gui_zone_menu_items(uint32_t channel, uint32_t note, bool keyboard_range);
    static void osc_canvas_menu_entry(void* user_data);
    void        gui_rename_popup();
    void        gui_library_popups();
    void        gui_library_browser();
    void        gui_library_save_popups();

    bool do_library_load(const Synth::LibraryEntry& entry);
    bool save_instrument_to_library(const char* category, const char* name);
    bool finish_library_save(const char* category, const char* name);

    uint32_t selected_target = 0;                         // channel 0..15 or target_master
    int32_t  selected_zone[Synth::max_channels];          // selected zone entry per channel, -1 = none
    uint8_t  last_clicked_note[Synth::max_channels] = {}; // selection re-derivation anchor
    bool     editor_bank_initialized                = false;

    bool    held_note_active  = false; // single held note played by clicking a keyboard key
    uint8_t held_note_channel = 0;
    uint8_t held_note         = 0;

    uint32_t menu_channel         = target_master; // channel bound to the open right-click menu
    uint32_t zone_menu_channel    = 0;
    uint32_t zone_menu_note       = 0;     // key under the open zone spec menu
    bool     channel_menu_open    = false; // deferred: OpenPopup must run outside the table ID scope
    bool     zone_menu_open       = false; // deferred: same ID-scope rule as channel_menu_open
    int32_t  zone_tab_force_entry = -1;    // one-shot: make the zone tab bar select this entry, cleared once shown

    char     rename_buf[Synth::max_name_len] = {};
    bool     rename_popup_open               = false;
    bool     rename_zone                     = false; // rename modal targets a zone instrument
    uint32_t rename_zone_entry               = 0;     // zone entry the rename modal targets
    bool     show_effects_mode               = false; // zone vs Effects view; persists across targets
    int32_t  effects_tab_pend                = -1; // one-shot: force-select the Effects tab after an undo/redo restore

    // Instrument library. Entries refresh on every browser open and before every
    // save; errors surface as notifications and a failed scan never opens
    // the browser.
    char                     library_path[64] = "assets/instruments.library";
    bool                     library_open     = false; // deferred browser open
    uint32_t                 library_channel  = 0;     // channel the browser/save acts on
    Synth::LibraryEntry      library_entries[Synth::library_max_records];
    uint32_t                 library_num_entries = 0;
    Synth::LibraryScanStatus library_scan_status = Synth::library_valid;
    char                     library_categories[Synth::library_max_records][Synth::library_category_len] = {};
    uint32_t                 library_num_categories                                                      = 0;
    int32_t                  library_category = -1; // selected category index, -1 = none

    char save_category[Synth::max_name_len]    = {}; // last-used category
    char save_as_name[Synth::max_name_len]     = {};
    char save_as_category[Synth::max_name_len] = {};
    bool save_as_open                          = false; // deferred Save As popup
    bool save_confirm                          = false; // deferred overwrite-acknowledge popup
    char confirm_category[Synth::max_name_len] = {};
    char confirm_name[Synth::max_name_len]     = {};
};

// Shared note naming for the keyboard and zone-tab tooltips.
// The octave is floor(midi / 12) - 2.  Pitch classes are sharps only, so
// MIDI 51 is "D#2".  The octave is signed so MIDI 0..23 does not wrap.
inline void note_name(uint32_t midi, char* out, uint32_t capacity)
{
    static const char* const pitch_classes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int32_t            octave            = static_cast<int32_t>(midi / 12) - 2;

    snprintf(out, capacity, "%s%d", pitch_classes[midi % 12], octave);
}

// First-seen label membership: true when name matches an earlier label
// in the list.  Shared by the deriving callback and the title helper so
// the first-seen policy lives in one place.
inline bool label_seen(const char* const* labels, uint32_t count, const char* name)
{
    for (uint32_t label_index = 0; label_index < count; ++label_index) {
        if (strcmp(labels[label_index], name) == 0) {
            return true;
        }
    }

    return false;
}

// Builds a derived generator title: deduplicated parameter row labels
// in first-seen order, joined with ", ", plus " (<host>)" when host_title
// is non-null and non-empty.  Returns false when there are no labels (a
// null entry is a refused label too): the caller keeps the default
// "Envelope N" / "LFO N" title and out stays untouched.  Pure string
// logic; the text truncates to capacity.
inline bool derive_generator_title(const char* const* labels,
                                   uint32_t           count,
                                   const char*        host_title,
                                   char*              out,
                                   uint32_t           capacity)
{
    if (count == 0 || labels == nullptr || out == nullptr || capacity == 0) {
        return false;
    }

    // A null entry has no label to append; refuse before any write so a
    // false return leaves out untouched.
    for (uint32_t label_index = 0; label_index < count; ++label_index) {
        if (labels[label_index] == nullptr) {
            return false;
        }
    }

    // Bounded append: every fragment stops before the reserved terminator.
    uint32_t used = 0;
    for (uint32_t label_index = 0; label_index < count; ++label_index) {
        if (label_seen(labels, label_index, labels[label_index])) {
            continue;
        }

        // The separator precedes every label but the first.
        const char* pieces[2] = { used == 0 ? "" : ", ", labels[label_index] };
        for (uint32_t fragment_index = 0; fragment_index < 2; ++fragment_index) {
            for (const char* cursor = pieces[fragment_index]; *cursor != 0 && used + 1 < capacity; ++cursor) {
                out[used++] = *cursor;
            }
        }
    }

    // Single-host parenthetical: an empty host is no host.
    if (host_title != nullptr && host_title[0] != 0) {
        const char* pieces[3] = { " (", host_title, ")" };
        for (uint32_t fragment_index = 0; fragment_index < 3; ++fragment_index) {
            for (const char* cursor = pieces[fragment_index]; *cursor != 0 && used + 1 < capacity; ++cursor) {
                out[used++] = *cursor;
            }
        }
    }

    out[used] = 0;
    return true;
}

// Info panel under the synth channel list: tempo, route-to-channel
// checkbox, playing oscillator count, underrun count, fill bar with a
// lingering bottom marker, and a mono log-axis frequency spectrum.
// Displays live synth runtime status, and each frame it
// publishes the device-MIDI routing target from the "Route to Current
// Channel" checkbox via Synth::set_midi_route_target.
void gui_synth_info_panel(uint32_t current_channel);

} // namespace Sculptor
