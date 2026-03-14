// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Sculptor {

class UndoRedo {
    public:
        constexpr UndoRedo() = default;

        void init(uint8_t* new_buf, size_t size);

        template<size_t N>
        void init(uint8_t (&new_buf)[N])
        {
            init(new_buf, N);
        }

        void clear_redo();
        void clear();

        struct Snapshot {
            uint8_t* buf  = nullptr;
            uint32_t size = 0;
        };

        Snapshot get_snapshot();
        Snapshot get_snapshot_space();
        void     push_snapshot(uint32_t size);

        void init_undo_push();
        bool finish_undo_push();

        void init_redo_push();
        bool finish_redo_push();

        void push(const void* data, size_t size);
        void push(uint32_t v);
        void push(float v);

        bool undo_empty() const { return ! undo_idx; }
        bool redo_empty() const { return redo_idx == buf_size; }

        bool init_undo();
        void finish_undo();  // Finish undo operation, pop from undo stack
        void restore_undo(); // Finish undo operation, but keep it on undo stack
        bool skip_undo();    // Pop entry from undo stack without using it

        bool init_redo();
        void finish_redo();

        void     pop(void* data, size_t size);
        uint32_t pop_u32();
        float    pop_f32();

    private:
        static constexpr uint32_t header_size = sizeof(uint32_t);

        enum class Mode {
            inactive,
            undo_push,
            redo_push,
            undo_pop,
            redo_pop,
            snapshot_write,
        };

        bool make_room(uint32_t size);

        uint8_t* buf      = nullptr;
        uint32_t buf_size = 0;
        uint32_t undo_idx = 0;
        uint32_t undo_bak = 0;
        uint32_t redo_idx = 0;
        uint32_t cur_size = 0;
        Mode     mode     = Mode::inactive;
        bool     overflow = false;
};

} // namespace Sculptor
