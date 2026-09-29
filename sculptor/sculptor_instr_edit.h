// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"
#include "sculptor_editor.h"
#include "sculptor_instr_library.h"

#include <stdint.h>

namespace Sculptor {

struct UndoGroupTag;

class SynthEditor : public Editor {
public:
    SynthEditor();

    ~SynthEditor() override = default;

    const char* get_editor_name() const override { return "Synth"; }
    bool        create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
    bool        allocate_resources() override;
    void        free_resources() override {}
    bool        draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override { return true; }

    // File-menu triggers; the dialogs open on the next create_gui_frame.
    void trigger_save();
    void trigger_load();

private:
    static constexpr uint32_t target_master = Synth::max_channels;

    void rederive_zone_selection(uint32_t channel);
    void rederive_all_selections();
    bool commit_candidate(const Synth::InstrumentEditorBank& candidate, Sculptor::UndoGroupTag tag);
    void do_initialize(uint32_t channel);
    void do_delete(uint32_t channel);
    void do_zone_join_previous(uint32_t channel, uint32_t note);
    void do_zone_join_next(uint32_t channel, uint32_t note);
    void do_zone_split_new(uint32_t channel, uint32_t note);
    void do_zone_delete(uint32_t channel, uint32_t entry);
    void gui_osc_graph(uint32_t channel);
    void drain_osc_graph(uint32_t channel, uint32_t zone);
    void run_osc_canvas_command();
    void do_osc_add_oscillator();
    void do_osc_add_generator(bool is_env);
    void do_osc_add_parameter();
    void do_osc_change_target_param(uint32_t node_idx, uint32_t new_target);
    void release_held_audition();

    void gui_channel_list();
    void gui_channel_pane(uint32_t channel);
    void gui_keyboard();
    void gui_channel_popup();
    void gui_zone_menu();
    void gui_rename_popup();
    void gui_bank_popups();
    void gui_library_popups();
    void gui_library_browser();
    void gui_library_save_popups();

    bool do_library_load(const Synth::LibraryEntry& entry);
    bool save_instrument_to_library(const char* category, const char* name);
    bool finish_library_save(const char* category, const char* name);

    uint32_t selected_target = 0;                             // channel 0..15 or target_master
    int32_t  selected_zone[Synth::max_channels];              // selected zone entry per channel, -1 = none
    uint8_t  last_audition_note[Synth::max_channels] = {};    // selection re-derivation anchor
    bool     audition_held                           = false; // single held audition note (channel + note below)
    bool     editor_bank_initialized                 = false;
    uint32_t audition_channel                        = 0;
    uint32_t audition_note                           = 0;
    bool     window_was_focused                      = false; // edge-detects focus loss to release the held note

    bool     dialog_save          = false;
    bool     dialog_load          = false;
    uint32_t menu_channel         = target_master; // channel bound to the open right-click menu
    uint32_t zone_menu_channel    = 0;
    uint32_t zone_menu_note       = 0;     // key under the open zone spec menu
    bool     channel_menu_open    = false; // deferred: OpenPopup must run outside the table ID scope
    bool     zone_menu_open       = false; // deferred: same ID-scope rule as channel_menu_open
    int32_t  zone_tab_force_entry = -1;    // one-shot: make the zone tab bar select this entry, cleared once shown
    bool     zone_menu_from_tab   = false; // menu opened from a zone tab, not a keyboard key

    char     rename_buf[Synth::max_name_len] = {};
    bool     rename_popup_open               = false;
    bool     rename_zone                     = false; // rename modal targets a zone instrument
    uint32_t rename_zone_entry               = 0;     // zone entry the rename modal targets
    bool     show_effects_mode               = false; // Oscillators/Effects choice; persists across targets

    // Instrument library. Entries refresh on every browser open and before every
    // save; errors surface as notifications and a failed scan never opens
    // the browser.
    char                     library_path[64] = "assets/instruments.library";
    bool                     library_open     = false; // deferred browser open
    uint32_t                 library_channel  = 0;     // channel the browser/save acts on
    Synth::LibraryEntry      library_entries[Synth::library_max_records];
    uint32_t                 library_num_entries                                 = 0;
    Synth::LibraryScanStatus library_scan_status                                 = Synth::library_valid;
    char                     library_categories[64][Synth::library_category_len] = {};
    uint32_t                 library_num_categories                              = 0;
    int32_t                  library_category = -1; // selected category index, -1 = none

    char save_category[Synth::max_name_len]    = {}; // last-used category
    char save_as_name[Synth::max_name_len]     = {};
    char save_as_category[Synth::max_name_len] = {};
    bool save_as_open                          = false; // deferred Save As popup
    bool save_confirm                          = false; // deferred overwrite-acknowledge popup
    char confirm_category[Synth::max_name_len] = {};
    char confirm_name[Synth::max_name_len]     = {};
};

} // namespace Sculptor
