// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "../resource.h"

namespace Sculptor {

class Geometry {
    public:
        constexpr Geometry() = default;

        struct Vertex {
            float pos[3];
        };

        struct PatchFace {
            uint32_t material_id;
        };

        bool allocate();
        bool send_to_gpu(VkCommandBuffer cmd_buf);

        uint32_t add_vertex(float x, float y, float z);
        uint32_t get_num_vertices() const { return num_vertices; }
        void     set_vertex(uint32_t vtx, float x, float y, float z);
        uint32_t add_edge(uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        void     set_edge(uint32_t edge, uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3);
        uint32_t get_num_edges() const { return num_edges; }
        uint32_t add_face(int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3);
        void     set_face(uint32_t face, int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3);
        uint32_t get_num_faces() const { return num_faces; }

    private:
        Buffer   vertices;
        Buffer   host_vertices;
        Buffer   indices;
        Buffer   host_indices;
        Buffer   faces;
        Buffer   host_faces;

        uint32_t num_vertices = 0;
        uint32_t num_indices  = 0;
        uint32_t num_edges    = 0;
        uint32_t num_faces    = 0;

        using Edge = uint32_t[4];
        static constexpr uint32_t max_edges = 0x10000U;
        Edge obj_edges[max_edges] = { };

        struct Face {
            int32_t edges[4];
        };
        static constexpr uint32_t max_faces = 0x10000U / 16U;
        Face obj_faces[max_faces] = { };
};

}
