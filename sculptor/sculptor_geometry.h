// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

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
            uint32_t state;
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

        uint32_t add_vertex(int16_t x, int16_t y, int16_t z);
        uint32_t get_num_vertices() const { return num_vertices; }
        void     set_vertex(uint32_t vtx, int16_t x, int16_t y, int16_t z);
        uint32_t add_edge(uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        void     set_edge(uint32_t edge, uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        uint32_t get_num_edges() const { return num_edges; }
        uint32_t add_face(int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                          uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        void     set_face(uint32_t face_id, int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                          uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        uint32_t get_num_faces() const { return num_faces; }
        void     validate_face(uint32_t face_id);

        void set_cube();
        void set_hovered_face(uint32_t face_id);
        void select_face(uint32_t face_id);
        void deselect_face(uint32_t face_id);
        void deselect_all_faces();

    private:
        Buffer   gpu_buffer;
        Buffer   host_buffer;

        uint32_t last_buffer         = 0;
        uint32_t hovered_face_id     = ~0U;
        uint32_t num_vertices        = 0;
        uint32_t num_indices         = 0;
        uint32_t num_edge_indices    = 0;
        uint32_t num_edges           = 0;
        uint32_t num_faces           = 0;
        bool     dirty               = true;

        struct Edge {
            uint32_t vertices[4];
            bool     selected;
        };
        Edge obj_edges[max_edges] = { };

        struct Face {
            int32_t  edges[4];
            uint32_t ctrl_vertices[4];
            uint32_t material_id;
            bool     selected;
        };
        Face obj_faces[max_faces] = { };
        uint32_t get_face_state(uint32_t face_id, const Face& face) const;
};

}
