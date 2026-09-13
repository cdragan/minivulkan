// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_edit.h"
#include "sculptor_instr_bank.h"

#include "../synth/realtime_synth.h"
#include "../synth/synth_serialize.h"
#include "sculptor_undo.h"

#include "../core/d_printf.h"
#include <string.h>
#include <type_traits>

namespace {

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

    bool publish_edited_bank()
    {
        if ( ! Synth::validate_instrument_bank(&instr_bank)) {
            d_printf("Error: refusing to publish an invalid instrument bank\n");
            return false;
        }

        pending_bank         = instr_bank;
        bank_changes_pending = true;

        return Synth::pump_bank_publish();
    }

    void drain_bank_updates()
    {
        while (const Synth::InstrumentBank* const packet = Synth::peek_bank_update(&bank_queue)) {
            Synth::set_current_bank(*packet);
            Synth::consume_bank_update(&bank_queue);
        }
    }

} // anonymous namespace

void Synth::init_editor()
{
    static_assert(std::is_trivially_copyable_v<Synth::InstrumentBank>);
    memcpy(&instr_bank, &Synth::current_bank(), sizeof(instr_bank));

    if (memcmp(&instr_bank, &Synth::current_bank(), sizeof(instr_bank)))
        d_printf("Editor bank snapshot mismatch\n");

    Synth::set_bank_source_callback(&drain_bank_updates);
}

Synth::InstrumentBank& Synth::editable_bank()
{
    return instr_bank;
}

bool Synth::publish_bank()
{
    return publish_edited_bank();
}

bool Synth::pump_bank_publish()
{
    if (bank_changes_pending) {
        if ( ! Synth::push_bank_update(&bank_queue, pending_bank)) {
            return false;
        }

        bank_changes_pending = false;
    }

    return true;
}

void Synth::editor_snapshot()
{
    undo_init_once();

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo_push();

    undo_redo.clear_redo();
}

bool Synth::editor_undo()
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

bool Synth::editor_redo()
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

bool Synth::save_editor_bank(const char* path)
{
    if ( ! Synth::validate_instrument_bank(&instr_bank)) {
        d_printf("Error: refusing to save an invalid instrument bank\n");
        return false;
    }
    return save_instrument_bank(path, &instr_bank);
}

bool Synth::load_editor_bank(const char* path)
{
    if ( ! load_instrument_bank(path, &instr_bank))
        return false;

    return publish_edited_bank();
}

static_assert(sizeof(undo_buf) <= undo_depth * (sizeof(Synth::InstrumentBank) + sizeof(uint32_t)));
static_assert(sizeof(Synth::BankUpdateQueue) <= 2 * sizeof(Synth::InstrumentBank) + 32);
