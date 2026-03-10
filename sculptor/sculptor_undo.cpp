// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_undo.h"

#include <assert.h>
#include <string.h>

namespace Sculptor {

void UndoRedo::init(uint8_t* new_buf, size_t size)
{
    assert(mode == Mode::inactive);
    assert(buf_size == 0);
    assert(undo_idx == 0);
    assert(undo_bak == 0);
    assert(redo_idx == 0);

    buf      = new_buf;
    buf_size = static_cast<uint32_t>(size);
    redo_idx = static_cast<uint32_t>(size);
}

void UndoRedo::clear_redo()
{
    redo_idx = buf_size;
}

bool UndoRedo::make_room(uint32_t size)
{
    const uint32_t avail_size = redo_idx - undo_idx;

    if (size <= avail_size)
        return true;

    uint32_t clear_size = size - avail_size;

    // Find how much we need to clear in undo stack
    uint32_t idx = (mode == Mode::undo_push) ? (undo_idx - cur_size) : undo_idx;
    while (idx > clear_size) {
        uint32_t block_size;
        memcpy(&block_size, buf + idx - header_size, header_size);

        const uint32_t next_idx = idx - header_size - block_size;

        if (next_idx < clear_size)
            break;
        idx = next_idx;
    }

    // Clear undo stack
    undo_idx -= idx;
    undo_bak = undo_idx;
    if (idx)
        memmove(buf, buf + idx, undo_idx);

    if (clear_size <= idx)
        return true;

    clear_size -= idx;

    // Find how much we need to clear in redo stack
    idx = (mode == Mode::redo_push) ? (redo_idx + cur_size) : redo_idx;
    while (idx < buf_size && buf_size - idx > clear_size) {
        uint32_t block_size;
        memcpy(&block_size, buf + idx, header_size);

        const uint32_t next_idx = idx + header_size + block_size;

        if (buf_size - next_idx < clear_size)
            break;
        idx = next_idx;
    }
    const uint32_t end_space = buf_size - idx;

    // Clear redo stack
    if (end_space)
        memmove(buf + redo_idx + end_space, buf + redo_idx, idx - redo_idx);
    redo_idx += end_space;

    return clear_size <= end_space;
}

void UndoRedo::init_undo_push()
{
    assert(mode == Mode::inactive);
    assert(cur_size == 0);

    mode = Mode::undo_push;
}

bool UndoRedo::finish_undo_push()
{
    assert(mode == Mode::undo_push);

    if (overflow) {
        undo_idx -= cur_size;
        undo_bak  = undo_idx;
        cur_size  = 0;
        overflow  = false;
        mode      = Mode::inactive;
        return false;
    }

    mode = Mode::inactive;

    static_assert(sizeof cur_size == header_size);
    memcpy(buf + undo_idx, &cur_size, header_size);
    undo_idx += header_size;
    undo_bak = undo_idx;
    cur_size = 0;

    return true;
}

void UndoRedo::init_redo_push()
{
    assert(mode == Mode::inactive);
    assert(cur_size == 0);

    mode = Mode::redo_push;
}

bool UndoRedo::finish_redo_push()
{
    assert(mode == Mode::redo_push);

    if (overflow) {
        redo_idx += cur_size;
        cur_size  = 0;
        overflow  = false;
        mode      = Mode::inactive;
        return false;
    }

    mode = Mode::inactive;

    redo_idx -= header_size;
    static_assert(sizeof cur_size == header_size);
    memcpy(buf + redo_idx, &cur_size, header_size);
    cur_size = 0;

    return true;
}

void UndoRedo::push(const void* data, size_t size)
{
    assert(mode == Mode::undo_push || mode == Mode::redo_push);

    if (overflow)
        return;

    assert(static_cast<uint32_t>(size + header_size) == size + header_size);
    if (!make_room(static_cast<uint32_t>(size + header_size))) {
        overflow = true;
        return;
    }

    void* dest_ptr;

    if (mode == Mode::undo_push) {
        dest_ptr = buf + undo_idx;
        undo_idx += static_cast<uint32_t>(size);
    }
    else {
        redo_idx -= static_cast<uint32_t>(size);
        dest_ptr = buf + redo_idx;
    }

    memcpy(dest_ptr, data, size);

    cur_size += size;
}

void UndoRedo::push(uint32_t v)
{
    push(&v, sizeof v);
}

void UndoRedo::push(float v)
{
    push(&v, sizeof v);
}

bool UndoRedo::init_undo()
{
    assert(mode == Mode::inactive);
    assert(cur_size == 0);

    if (undo_idx == 0)
        return false;

    static_assert(sizeof cur_size == header_size);
    memcpy(&cur_size, buf + undo_idx - header_size, header_size);
    undo_idx -= header_size;

    mode = Mode::undo_pop;
    return true;
}

void UndoRedo::finish_undo()
{
    assert(mode == Mode::undo_pop);
    assert(cur_size == 0);

    mode     = Mode::inactive;
    undo_bak = undo_idx;
}

void UndoRedo::restore_undo()
{
    assert(mode == Mode::undo_pop);
    assert(cur_size == 0);

    mode     = Mode::inactive;
    undo_idx = undo_bak;
}

bool UndoRedo::skip_undo()
{
    assert(mode == Mode::inactive);
    assert(cur_size == 0);

    if ( ! undo_idx)
        return false;

    uint32_t block_size;
    static_assert(sizeof block_size == header_size);
    memcpy(&block_size, buf + undo_idx - header_size, header_size);

    undo_idx -= header_size + block_size;
    undo_bak = undo_idx;

    return true;
}

bool UndoRedo::init_redo()
{
    assert(mode == Mode::inactive);
    assert(cur_size == 0);

    if (redo_idx == buf_size)
        return false;

    static_assert(sizeof cur_size == header_size);
    memcpy(&cur_size, buf + redo_idx, header_size);
    redo_idx += header_size;

    mode = Mode::redo_pop;
    return true;
}

void UndoRedo::finish_redo()
{
    assert(mode == Mode::redo_pop);
    assert(cur_size == 0);

    mode = Mode::inactive;
}

void UndoRedo::pop(void* data, size_t size)
{
    assert(mode == Mode::undo_pop || mode == Mode::redo_pop);
    assert(size <= cur_size);

    if (mode == Mode::undo_pop) {
        undo_idx -= static_cast<uint32_t>(size);
        memcpy(data, buf + undo_idx, size);
    }
    else {
        memcpy(data, buf + redo_idx, size);
        redo_idx += static_cast<uint32_t>(size);
    }

    cur_size -= static_cast<uint32_t>(size);
}

uint32_t UndoRedo::pop_u32()
{
    uint32_t value;
    pop(&value, sizeof value);
    return value;
}

float UndoRedo::pop_f32()
{
    float value;
    pop(&value, sizeof value);
    return value;
}

} // namespace Sculptor
