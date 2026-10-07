// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_instr_bank.h"
#include "sculptor_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exit_code = 0;

#define TEST(cond)                                                 \
    do {                                                           \
        if (! (cond)) {                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit_code = 1;                                         \
        }                                                          \
    } while (0)

using Sculptor::UndoRedo;

static void test_empty_state()
{
    alignas(4) uint8_t buf[1];
    UndoRedo           ur;
    ur.init(buf);
    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
    TEST(! ur.init_undo());
    TEST(! ur.init_redo());
}

static void test_single_undo_push_pop()
{
    alignas(4) uint8_t buf[24];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    ur.push(uint32_t{ 2 });
    ur.push(3.14f);
    TEST(ur.finish_undo_push());

    TEST(ur.redo_empty());
    TEST(! ur.init_redo());

    TEST(ur.init_undo());
    const float f = ur.pop_f32();
    TEST(f > 3.13f && f < 3.15f);
    TEST(ur.pop_u32() == 2);
    TEST(ur.pop_u32() == 1);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(! ur.init_undo());
}

static void test_multiple_undo_entries()
{
    alignas(4) uint8_t buf[24];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 10 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 20 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 30 });
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 30);
    ur.finish_undo();

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 20);
    ur.finish_undo();

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 10);
    ur.finish_undo();

    TEST(! ur.init_undo());
}

static void test_single_redo_push_pop()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{ 100 });
    ur.push(uint32_t{ 200 });
    TEST(ur.finish_redo_push());

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 200);
    TEST(ur.pop_u32() == 100);
    ur.finish_redo();

    TEST(ur.redo_empty());
    TEST(! ur.init_redo());
}

static void test_undo_redo_cycle()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 42 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 99 });
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 99);
    ur.finish_undo();

    ur.init_redo_push();
    ur.push(uint32_t{ 99 });
    TEST(ur.finish_redo_push());

    // Undo A
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    // Redo B
    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 99);
    ur.finish_redo();

    TEST(! ur.init_redo());
}

static void test_clear_redo()
{
    alignas(4) uint8_t buf[8];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{ 5 });
    TEST(ur.finish_redo_push());

    ur.clear_redo();

    TEST(ur.redo_empty());
    TEST(! ur.init_redo());
}

static void test_overflow_trims_oldest()
{
    alignas(4) uint8_t buf[20];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_undo_push());

    // Pushing one more block will remove oldest undo block
    ur.init_undo_push();
    ur.push(uint32_t{ 3 });
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 3);
    ur.finish_undo();

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 2);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(! ur.init_undo());
}

static void test_multiple_redo_entries()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_redo_push());

    ur.init_redo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_redo_push());

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 2);
    ur.finish_redo();

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 1);
    ur.finish_redo();

    TEST(! ur.init_redo());
}

static void test_overflow_undo_push_clears_both_stacks()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_undo_push());

    ur.init_redo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_redo_push());

    TEST(! ur.undo_empty());
    TEST(! ur.redo_empty());

    uint8_t data[13] = {};
    ur.init_undo_push();
    ur.push(data, sizeof data);
    TEST(! ur.finish_undo_push());

    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
}

static void test_overflow_redo_push_clears_both_stacks()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_undo_push());

    ur.init_redo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_redo_push());

    TEST(! ur.undo_empty());
    TEST(! ur.redo_empty());

    uint8_t data[13] = {};
    ur.init_redo_push();
    ur.push(data, sizeof data);
    TEST(! ur.finish_redo_push());

    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
}

static void test_overflow_no_space()
{
    // Buffer too small to fit data + header (needs 8 bytes, only 4 available)
    alignas(4) uint8_t buf[4];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(! ur.finish_undo_push());
    TEST(ur.undo_empty());
    TEST(! ur.init_undo());
}

static void test_finish_undo_consumes_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 42 });
    TEST(ur.finish_undo_push());

    TEST(! ur.undo_empty());
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(! ur.init_undo());
}

static void test_restore_undo_preserves_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 42 });
    TEST(ur.finish_undo_push());

    // Read entry but leave it on the stack
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.restore_undo();

    TEST(! ur.undo_empty());

    // Entry can be read again
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.restore_undo();

    TEST(! ur.undo_empty());

    // Entry consumed with finish_undo
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    TEST(ur.undo_empty());
}

static void test_restore_undo_with_multiple_entries()
{
    alignas(4) uint8_t buf[24];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 2);
    ur.restore_undo();

    TEST(! ur.undo_empty());

    // Consume both in order
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 2);
    ur.finish_undo();

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 1);
    ur.finish_undo();

    TEST(ur.undo_empty());
}

static void test_skip_undo_on_empty()
{
    alignas(4) uint8_t buf[8];
    UndoRedo           ur;
    ur.init(buf);

    TEST(! ur.skip_undo());
    TEST(ur.undo_empty());
}

static void test_skip_undo_removes_single_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 42 });
    TEST(ur.finish_undo_push());

    TEST(! ur.undo_empty());
    TEST(ur.skip_undo());
    TEST(ur.undo_empty());
    TEST(! ur.init_undo());
}

static void test_skip_undo_removes_top_entry_only()
{
    alignas(4) uint8_t buf[24];
    UndoRedo           ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{ 1 });
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{ 2 });
    TEST(ur.finish_undo_push());

    TEST(ur.skip_undo());

    TEST(! ur.undo_empty());
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 1);
    ur.finish_undo();

    TEST(ur.undo_empty());
}

// The instrument editor's entries ride a small origin u32 behind the bank
// payload: undo entries pop it first (stack grows up), redo entries read it
// first (stack grows down).  This pins both layouts plus the peek helpers the
// editor uses to carry the origin across the undo/redo hand-off.
static void test_edit_origin_entry()
{
    static constexpr uint32_t payload_words = 3; // stand-in bank (2 words) + origin

    alignas(4) uint8_t buf[(payload_words * sizeof(uint32_t) + 4) * 4];
    UndoRedo           ur;
    ur.init(buf);

    // editor_snapshot(): bank payload, then origin.
    ur.init_undo_push();
    ur.push(uint32_t{ 0x11111111u });
    ur.push(uint32_t{ 0x22222222u });
    ur.push(uint32_t{ 0x33u });
    TEST(ur.finish_undo_push());
    TEST(! ur.undo_empty());

    // editor_undo peeks the undo top: origin sits after the bank bytes.
    const UndoRedo::Snapshot undo_snap = ur.get_snapshot();
    TEST(undo_snap.buf != nullptr);
    uint32_t entry_origin = 0;
    memcpy(&entry_origin, undo_snap.buf + 2 * sizeof(uint32_t), sizeof entry_origin);
    TEST(entry_origin == 0x33u);

    // editor_undo(): the redo entry replays bank + origin, the undo pop is LIFO.
    ur.init_redo_push();
    ur.push(uint32_t{ 0x44444444u });
    ur.push(entry_origin);
    TEST(ur.finish_redo_push());
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 0x33u);
    TEST(ur.pop_u32() == 0x22222222u);
    TEST(ur.pop_u32() == 0x11111111u);
    ur.finish_undo();
    TEST(ur.undo_empty());
    TEST(! ur.redo_empty());

    // editor_redo peeks the redo top: origin is first in the forward layout.
    const UndoRedo::Snapshot redo_snap = ur.get_redo_snapshot();
    TEST(redo_snap.buf != nullptr);
    uint32_t redo_origin = 0;
    memcpy(&redo_origin, redo_snap.buf, sizeof redo_origin);
    TEST(redo_origin == 0x33u);

    // editor_redo(): the undo entry replays bank + origin, the redo pop is FIFO.
    ur.init_undo_push();
    ur.push(uint32_t{ 0x55555555u });
    ur.push(redo_origin);
    TEST(ur.finish_undo_push());
    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 0x33u);
    TEST(ur.pop_u32() == 0x44444444u);
    ur.finish_redo();

    // The redo produced a fresh undo entry with the same origin; consuming it
    // without a redo push leaves both stacks empty.
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 0x33u);
    TEST(ur.pop_u32() == 0x55555555u);
    ur.finish_undo();
    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
}

// Source inspection pins the GUI startup path; real UndoRedo below exercises
// whole-bank snapshots without running the GUI or writing persistent assets.
static void test_startup_load_undo_baseline()
{
    static char source[256 * 1024];
    FILE* const file = fopen("sculptor/sculptor_instr_edit.cpp", "rb");
    TEST(file != nullptr);
    if (! file)
        return;
    const size_t length = fread(source, 1, sizeof(source) - 1, file);
    TEST(! ferror(file));
    TEST(feof(file));
    fclose(file);
    source[length]                = 0;
    const char* const load        = strstr(source, "static bool load_editor_bank(const char* path)");
    const char* const startup     = load ? strstr(load, "static void init_editor()") : nullptr;
    const char* const startup_end = startup ? strstr(startup, "static uint32_t zone_count(") : nullptr;
    TEST(load != nullptr && startup != nullptr && startup_end != nullptr);
    if (! load || ! startup || ! startup_end)
        return;
    const char* const snapshot = strstr(load, "editor_snapshot(");
    TEST(! snapshot || snapshot >= startup_end);
    const char* const assignment = strstr(load, "= scratch;");
    TEST(assignment && assignment < startup);

    static Synth::InstrumentEditorBank loaded;
    static Synth::InstrumentEditorBank editable;
    loaded                         = {};
    loaded.bank.channel_enabled[3] = 1;
    strcpy(loaded.channel_names[3], "Loaded channel");
    alignas(4) static uint8_t storage[2 * (sizeof(loaded) + 2 * sizeof(uint32_t))];
    UndoRedo                  undo;
    undo.init(storage);
    editable = loaded;
    TEST(undo.undo_empty());
    TEST(! undo.init_undo());

    undo.init_undo_push();
    undo.push(&editable, sizeof(editable));
    undo.push(uint32_t{ 3 });
    TEST(undo.finish_undo_push());
    editable.bank.channel_enabled[3] = 0;
    TEST(undo.init_undo());
    TEST(undo.pop_u32() == 3);
    undo.pop(&editable, sizeof(editable));
    undo.finish_undo();
    TEST(memcmp(&editable, &loaded, sizeof(loaded)) == 0);
    TEST(undo.undo_empty());
    TEST(! undo.init_undo());
    TEST(memcmp(&editable, &loaded, sizeof(loaded)) == 0);
}

int main()
{
    test_startup_load_undo_baseline();
    test_edit_origin_entry();
    test_empty_state();
    test_single_undo_push_pop();
    test_multiple_undo_entries();
    test_single_redo_push_pop();
    test_undo_redo_cycle();
    test_clear_redo();
    test_overflow_trims_oldest();
    test_overflow_undo_push_clears_both_stacks();
    test_overflow_redo_push_clears_both_stacks();
    test_overflow_no_space();
    test_multiple_redo_entries();
    test_finish_undo_consumes_entry();
    test_restore_undo_preserves_entry();
    test_restore_undo_with_multiple_entries();
    test_skip_undo_on_empty();
    test_skip_undo_removes_single_entry();
    test_skip_undo_removes_top_entry_only();

    return exit_code;
}
