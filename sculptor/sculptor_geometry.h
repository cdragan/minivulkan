// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "sculptor_undo.h"
#include "../core/resource.h"

namespace Sculptor {

class Geometry {
    public:
        constexpr Geometry() = default;

        struct Vertex {
            int16_t pos[3];
            int16_t unused;
        };

        struct FaceData {
            uint32_t material_id;
        };

        struct FacesBuf {
            int32_t  tess_level[4];
            FaceData face_data[1];
        };

        static constexpr uint32_t max_edges = 0x10000U;
        static constexpr uint32_t max_faces = 0x10000U / 16U;

        bool allocate();
        void set_dirty() { dirty = true; }
        bool send_to_gpu(VkCommandBuffer cmd_buf);
        void write_faces_descriptor(VkDescriptorBufferInfo* desc);
        void write_edge_indices_descriptor(VkDescriptorBufferInfo* desc);
        void write_edge_vertices_descriptor(VkDescriptorBufferInfo* desc);
        void render(VkCommandBuffer cmd_buf);
        void render_vertices(VkCommandBuffer cmd_buf);

        void     set_tess_level(int32_t level);
        uint32_t add_vertex(int16_t x, int16_t y, int16_t z);
        uint32_t get_num_vertices() const { return num_vertices; }
        void     set_vertex(uint32_t vtx, int16_t x, int16_t y, int16_t z);
        void     move_vertex(uint32_t vtx, float dx, float dy, float dz);
        uint32_t add_edge(uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        void     set_edge(uint32_t edge, uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        uint32_t get_num_edges() const { return num_edges; }
        uint32_t add_face(int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                          uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        void     set_face(uint32_t face_id, int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                          uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        uint32_t get_num_faces() const { return num_faces; }
        void     validate_face(uint32_t face_id);
        void     get_face_vertex_indices(uint32_t face_id, uint32_t out_vtx[16]) const;

        void set_cube();

        bool snapshot_state();   // Push a snapshot of geometry onto the undo stack
        bool restore_snapshot(); // Pop the snapshot of geometry from the undo stack
        bool apply_snapshot();   // Apply the snapshot of geometry without popping it from the stack
        bool drop_snapshot();    // Pop the snapshot from the undo stack without applying it
        bool undo();             // Save current geometry onto the redo stack and pop snapshot from undo stack
        bool redo();             // Push a snapshot of geometry onto the undo stack and pop snapshot from redo stack
        void clear_redo() { undo_redo.clear_redo(); }
        bool undo_empty() const { return undo_redo.undo_empty(); }
        bool redo_empty() const { return undo_redo.redo_empty(); }

    private:
        bool apply_snapshot(bool pop_undo);

        Buffer   gpu_buffer;
        Buffer   host_buffer;

        int32_t  tess_level       = 3;
        uint32_t last_buffer      = 0;
        uint32_t num_vertices     = 0;
        uint32_t num_indices      = 0;
        uint32_t num_edge_indices = 0;
        uint32_t num_edges        = 0;
        uint32_t num_faces        = 0;
        bool     dirty            = true;

        struct Edge {
            uint32_t vertices[4];
        };
        Edge obj_edges[max_edges] = { };

        struct Face {
            int32_t  edges[4];
            uint32_t ctrl_vertices[4];
            uint32_t material_id;
        };
        Face obj_faces[max_faces] = { };

        static constexpr uint32_t undo_buf_size = 512 * 1024;
        alignas(4) uint8_t undo_buf[undo_buf_size];
        UndoRedo undo_redo;
};

}
