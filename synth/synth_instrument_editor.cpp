// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_instrument_editor.h"

#include "synth_serialize.h"
#include "../sculptor/sculptor_undo.h"

#include <atomic>

namespace {

Synth::InstrumentBank instr_bank; // GUI-thread-owned editable bank.
Sculptor::UndoRedo    undo_redo;
uint8_t               undo_buf[(sizeof(Synth::InstrumentBank) + sizeof(uint32_t)) * 32];

Synth::InstrumentBank               banks[3];
std::atomic<Synth::InstrumentBank*> published_bank{nullptr};  // latest stable bank
std::atomic<Synth::InstrumentBank*> reading_bank{nullptr};    // buffer the consumer currently holds
uint32_t                            write_idx = 0;

} // anonymous namespace

static void ensure_undo_init()
{
    static bool inited = false;

    if ( ! inited) {
        undo_redo.init(undo_buf);
        inited = true;
    }
}

Synth::InstrumentBank& Synth::editable_bank()
{
    return instr_bank;
}

void Synth::publish_bank()
{
    Synth::InstrumentBank* const held = reading_bank.load(std::memory_order_seq_cst);
    Synth::InstrumentBank* const cur  = published_bank.load(std::memory_order_seq_cst);

    do {
        write_idx = (write_idx + 1u) % 3u;
    } while (&banks[write_idx] == held || &banks[write_idx] == cur);

    banks[write_idx] = instr_bank;
    published_bank.store(&banks[write_idx], std::memory_order_seq_cst);
}

const Synth::InstrumentBank* Synth::acquire_audio_bank()
{
    for (;;) {
        Synth::InstrumentBank* const bank_ptr = published_bank.load(std::memory_order_seq_cst);
        if ( ! bank_ptr)
            return nullptr; // nothing published yet

        reading_bank.store(bank_ptr, std::memory_order_seq_cst);

        if (published_bank.load(std::memory_order_seq_cst) == bank_ptr)
            return bank_ptr;
    }
}

void Synth::editor_snapshot()
{
    ensure_undo_init();

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo_push();

    undo_redo.clear_redo();
}

bool Synth::editor_undo()
{
    ensure_undo_init();

    if (undo_redo.undo_empty())
        return false;

    undo_redo.init_redo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    if ( ! undo_redo.finish_redo_push())
        return false;

    undo_redo.init_undo();
    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_undo();

    publish_bank();
    return true;
}

bool Synth::editor_redo()
{
    ensure_undo_init();

    if (undo_redo.redo_empty())
        return false;

    undo_redo.init_undo_push();
    undo_redo.push(&instr_bank, sizeof(instr_bank));
    if ( ! undo_redo.finish_undo_push())
        return false;

    undo_redo.init_redo();
    undo_redo.pop(&instr_bank, sizeof(instr_bank));
    undo_redo.finish_redo();

    publish_bank();
    return true;
}

bool Synth::save_editor_bank(const char* path)
{
    return save_instrument_bank(path, &instr_bank);
}

bool Synth::load_editor_bank(const char* path)
{
    if ( ! load_instrument_bank(path, &instr_bank))
        return false;

    publish_bank();
    return true;
}
