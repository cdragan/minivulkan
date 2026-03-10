// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_undo.h"

#include <stdio.h>
#include <stdlib.h>

static int exit_code = 0;

#define TEST(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit_code = 1; \
    } \
} while(0)

using Sculptor::UndoRedo;

static void test_empty_state()
{
    alignas(4) uint8_t buf[1];
    UndoRedo ur;
    ur.init(buf);
    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
    TEST(!ur.init_undo());
    TEST(!ur.init_redo());
}

static void test_single_undo_push_pop()
{
    alignas(4) uint8_t buf[24];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    ur.push(uint32_t{2});
    ur.push(3.14f);
    TEST(ur.finish_undo_push());

    TEST(ur.redo_empty());
    TEST(!ur.init_redo());

    TEST(ur.init_undo());
    const float f = ur.pop_f32();
    TEST(f > 3.13f && f < 3.15f);
    TEST(ur.pop_u32() == 2);
    TEST(ur.pop_u32() == 1);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(!ur.init_undo());
}

static void test_multiple_undo_entries()
{
    alignas(4) uint8_t buf[24];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{10});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{20});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{30});
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

    TEST(!ur.init_undo());
}

static void test_single_redo_push_pop()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{100});
    ur.push(uint32_t{200});
    TEST(ur.finish_redo_push());

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 200);
    TEST(ur.pop_u32() == 100);
    ur.finish_redo();

    TEST(ur.redo_empty());
    TEST(!ur.init_redo());
}

static void test_undo_redo_cycle()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{42});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{99});
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 99);
    ur.finish_undo();

    ur.init_redo_push();
    ur.push(uint32_t{99});
    TEST(ur.finish_redo_push());

    // Undo A
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    // Redo B
    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 99);
    ur.finish_redo();

    TEST(!ur.init_redo());
}

static void test_clear_redo()
{
    alignas(4) uint8_t buf[8];
    UndoRedo ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{5});
    TEST(ur.finish_redo_push());

    ur.clear_redo();

    TEST(ur.redo_empty());
    TEST(!ur.init_redo());
}

static void test_overflow_trims_oldest()
{
    alignas(4) uint8_t buf[20];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_undo_push());

    // Pushing one more block will remove oldest undo block
    ur.init_undo_push();
    ur.push(uint32_t{3});
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 3);
    ur.finish_undo();

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 2);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(!ur.init_undo());
}

static void test_multiple_redo_entries()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_redo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_redo_push());

    ur.init_redo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_redo_push());

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 2);
    ur.finish_redo();

    TEST(ur.init_redo());
    TEST(ur.pop_u32() == 1);
    ur.finish_redo();

    TEST(!ur.init_redo());
}

static void test_overflow_undo_push_clears_both_stacks()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_undo_push());

    ur.init_redo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_redo_push());

    TEST(!ur.undo_empty());
    TEST(!ur.redo_empty());

    uint8_t data[13] = {};
    ur.init_undo_push();
    ur.push(data, sizeof data);
    TEST(!ur.finish_undo_push());

    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
}

static void test_overflow_redo_push_clears_both_stacks()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_undo_push());

    ur.init_redo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_redo_push());

    TEST(!ur.undo_empty());
    TEST(!ur.redo_empty());

    uint8_t data[13] = {};
    ur.init_redo_push();
    ur.push(data, sizeof data);
    TEST(!ur.finish_redo_push());

    TEST(ur.undo_empty());
    TEST(ur.redo_empty());
}

static void test_overflow_no_space()
{
    // Buffer too small to fit data + header (needs 8 bytes, only 4 available)
    alignas(4) uint8_t buf[4];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(!ur.finish_undo_push());
    TEST(ur.undo_empty());
    TEST(!ur.init_undo());
}

static void test_finish_undo_consumes_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{42});
    TEST(ur.finish_undo_push());

    TEST(!ur.undo_empty());
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    TEST(ur.undo_empty());
    TEST(!ur.init_undo());
}

static void test_restore_undo_preserves_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{42});
    TEST(ur.finish_undo_push());

    // Read entry but leave it on the stack
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.restore_undo();

    TEST(!ur.undo_empty());

    // Entry can be read again
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.restore_undo();

    TEST(!ur.undo_empty());

    // Entry consumed with finish_undo
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 42);
    ur.finish_undo();

    TEST(ur.undo_empty());
}

static void test_restore_undo_with_multiple_entries()
{
    alignas(4) uint8_t buf[24];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_undo_push());

    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 2);
    ur.restore_undo();

    TEST(!ur.undo_empty());

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
    UndoRedo ur;
    ur.init(buf);

    TEST(!ur.skip_undo());
    TEST(ur.undo_empty());
}

static void test_skip_undo_removes_single_entry()
{
    alignas(4) uint8_t buf[16];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{42});
    TEST(ur.finish_undo_push());

    TEST(!ur.undo_empty());
    TEST(ur.skip_undo());
    TEST(ur.undo_empty());
    TEST(!ur.init_undo());
}

static void test_skip_undo_removes_top_entry_only()
{
    alignas(4) uint8_t buf[24];
    UndoRedo ur;
    ur.init(buf);

    ur.init_undo_push();
    ur.push(uint32_t{1});
    TEST(ur.finish_undo_push());

    ur.init_undo_push();
    ur.push(uint32_t{2});
    TEST(ur.finish_undo_push());

    TEST(ur.skip_undo());

    TEST(!ur.undo_empty());
    TEST(ur.init_undo());
    TEST(ur.pop_u32() == 1);
    ur.finish_undo();

    TEST(ur.undo_empty());
}

int main()
{
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
