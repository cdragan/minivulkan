// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_editor.h"
#include "../synth/synth_instrument.h"

#include <stdint.h>


namespace Sculptor {

// The synth instrument editor: a pure-ImGui window over the GUI-thread editable
// bank. It owns no Vulkan resources. All of its per-frame duties run from
// create_gui_frame.
class SynthEditor: public Editor {
    public:
        SynthEditor();

        ~SynthEditor() override = default;

        const char* get_editor_name() const override { return "Synth"; }
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
        bool allocate_resources() override;
        void free_resources() override { }
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override { return true; }

        // File-menu triggers; the dialogs open on the next create_gui_frame.
        void trigger_save();
        void trigger_load();

    private:
        static constexpr uint32_t target_master = Synth::max_channels;

        void rederive_zone_selection(uint32_t channel);
        void rederive_all_selections();
        bool commit_candidate(const Synth::InstrumentBank& candidate);
        void do_initialize(uint32_t channel);
        void do_delete(uint32_t channel);
        void do_zone_join_previous(uint32_t channel, uint32_t note);
        void do_zone_join_next(uint32_t channel, uint32_t note);
        void do_zone_split_new(uint32_t channel, uint32_t note);
        void release_held_audition();

        void gui_channel_list();
        void gui_channel_pane(uint32_t channel);
        void gui_keyboard();
        void gui_channel_popup();
        void gui_zone_menu();
        void gui_rename_popup();
        void gui_bank_popups();

        uint32_t selected_target = 0;          // channel 0..15 or target_master
        int32_t  selected_zone[Synth::max_channels]; // selected zone entry per channel, -1 = none
        uint8_t  last_audition_note[Synth::max_channels] = { }; // selection re-derivation anchor
        bool     audition_held = false;        // single held audition note (channel + note below)
        bool     editor_bank_initialized = false;
        uint32_t audition_channel = 0;
        uint32_t audition_note = 0;
        bool     window_was_focused = false;   // edge-detects focus loss to release the held note

        bool     show_effects_mode = false;    // Oscillators/Effects choice; persists across targets

        bool     dialog_save = false;
        bool     dialog_load = false;
        uint32_t menu_channel = target_master; // channel bound to the open right-click menu
        uint32_t zone_menu_channel = 0;
        uint32_t zone_menu_note = 0;           // key under the open zone spec menu

        char     rename_buf[Synth::max_name_len] = { };
        bool     rename_popup_open = false;

        char     name_buf[Synth::max_name_len] = { }; // selected zone's instrument name
};

} // namespace Sculptor
