// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geometry.h"

#include "../mstdc.h"

constexpr uint32_t max_vertices    = 65536;
constexpr uint32_t max_indices     = 65536;
constexpr uint32_t num_host_copies = 3;
constexpr uint32_t vertices_stride = max_vertices * sizeof(Sculptor::Geometry::Vertex);
constexpr uint32_t indices_stride  = max_indices * sizeof(uint16_t);
constexpr uint32_t faces_stride    = sizeof(Sculptor::Geometry::FacesBuf) + (Sculptor::Geometry::max_faces - 1) * sizeof(Sculptor::Geometry::FaceData);

bool Sculptor::Geometry::allocate()
{
    if ( ! vertices.allocate(Usage::fixed,
                             vertices_stride,
                             VK_FORMAT_UNDEFINED,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_vertices.allocate(Usage::host_only,
                                  vertices_stride * num_host_copies,
                                  VK_FORMAT_UNDEFINED,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    if ( ! indices.allocate(Usage::fixed,
                            indices_stride,
                            VK_FORMAT_UNDEFINED,
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_indices.allocate(Usage::host_only,
                                 indices_stride * num_host_copies,
                                 VK_FORMAT_UNDEFINED,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    if ( ! faces.allocate(Usage::fixed,
                          faces_stride,
                          VK_FORMAT_UNDEFINED,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_faces.allocate(Usage::host_only,
                               faces_stride * num_host_copies,
                               VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    return true;
}

void Sculptor::Geometry::set_hovered_face(uint32_t face_id)
{
    if (face_id != hovered_face_id) {
        hovered_face_id = face_id;
        dirty = true;
    }
}

uint32_t Sculptor::Geometry::get_face_state(uint32_t face_id, const Face& face) const
{
    if (face_id == hovered_face_id)
        return 1;

    return 0;
}

bool Sculptor::Geometry::send_to_gpu(VkCommandBuffer cmd_buf)
{
    if ( ! dirty)
        return true;

    last_buffer = (last_buffer + 1) % num_host_copies;

    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    if (num_vertices) {
        copy_region.srcOffset = 0;
        copy_region.size      = num_vertices * sizeof(Vertex);

        vkCmdCopyBuffer(cmd_buf, host_vertices.get_buffer(), vertices.get_buffer(), 1, &copy_region);
    }

    FacesBuf* const faces_ptr = host_faces.get_ptr<FacesBuf>(last_buffer, faces_stride);
    faces_ptr->tess_level[0] = 12;

    num_indices = 0;
    uint16_t* const indices_ptr = host_indices.get_ptr<uint16_t>(last_buffer, indices_stride);
    for (uint32_t i_face = 0; i_face < num_faces; i_face++) {
        assert(num_indices + 16 <= max_indices);

        const Face& face = obj_faces[i_face];

        faces_ptr->face_data[i_face].material_id = face.material_id;
        faces_ptr->face_data[i_face].state       = get_face_state(i_face, face);

        static const uint32_t idx_map[] = {
             0,  1,  2,  3,
             0,  4,  8, 12,
             3,  7, 11, 15,
            12, 13, 14, 15
        };

        for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
            const int32_t edge_sel     = face.edges[i_edge];
            const bool    inverse_edge = edge_sel < 0;
            const Edge&   edge         = obj_edges[inverse_edge ? (-edge_sel - 1) : edge_sel];

            for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
                const uint32_t src_idx = inverse_edge ? (3 - i_idx) : i_idx;
                assert(edge[src_idx] < max_vertices);
                const uint32_t dest_idx = num_indices + idx_map[i_edge * 4 + i_idx];
                assert(dest_idx < num_indices + 16);
                indices_ptr[dest_idx] = static_cast<uint16_t>(edge[src_idx]);
            }
        }

        static const uint32_t ctrl_idx_map[] = {
            5,  6,
            9, 10
        };

        for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
            assert(face.ctrl_vertices[i_idx] < max_vertices);
            const uint32_t dest_idx = num_indices + ctrl_idx_map[i_idx];
            assert(dest_idx < num_indices + 16);
            indices_ptr[dest_idx] = static_cast<uint16_t>(face.ctrl_vertices[i_idx]);
        }

        num_indices += 16;
    }

    edge_indices_offset = num_indices;
    num_edge_indices = 0;
    for (uint32_t i_edge = 0; i_edge < num_edges; i_edge++) {
        assert(num_edge_indices + 4 <= max_indices);

        const Edge& edge = obj_edges[i_edge];

        for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
            assert(edge[i_idx] < max_vertices);
            const uint32_t dest_idx = edge_indices_offset + num_edge_indices + i_idx;
            indices_ptr[dest_idx] = static_cast<uint16_t>(edge[i_idx]);
        }

        num_edge_indices += 4;
    }

    if (num_indices + num_edge_indices) {
        copy_region.srcOffset = last_buffer * indices_stride;
        copy_region.size      = (num_indices + num_edge_indices) * sizeof(Vertex);

        vkCmdCopyBuffer(cmd_buf, host_indices.get_buffer(), indices.get_buffer(), 1, &copy_region);
    }

    if (num_faces) {
        copy_region.srcOffset = last_buffer * faces_stride;
        copy_region.size      = sizeof(FacesBuf) + (num_faces - 1) * sizeof(FaceData);

        vkCmdCopyBuffer(cmd_buf, host_faces.get_buffer(), faces.get_buffer(), 1, &copy_region);
    }

    dirty = false;

    return true;
}

uint32_t Sculptor::Geometry::add_vertex(int16_t x, int16_t y, int16_t z)
{
    assert(num_vertices < max_vertices);
    const uint32_t vtx = num_vertices++;
    set_vertex(vtx, x, y, z);
    return vtx;
}

void Sculptor::Geometry::set_vertex(uint32_t vtx, int16_t x, int16_t y, int16_t z)
{
    assert(vtx < num_vertices);

    Vertex& vertex = host_vertices.get_ptr<Vertex>()[vtx];
    vertex.pos[0] = x;
    vertex.pos[1] = y;
    vertex.pos[2] = z;
}

uint32_t Sculptor::Geometry::add_edge(uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3)
{
    assert(num_edges < max_edges);
    const uint32_t edge = num_edges++;
    set_edge(edge, vtx_0, vtx_1, vtx_2, vtx_3);
    return edge;
}

void Sculptor::Geometry::set_edge(uint32_t edge, uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3)
{
    assert(edge < num_edges);

    assert(vtx_0 < num_vertices);
    assert(vtx_1 < num_vertices);
    assert(vtx_2 < num_vertices);
    assert(vtx_3 < num_vertices);

    obj_edges[edge][0] = vtx_0;
    obj_edges[edge][1] = vtx_1;
    obj_edges[edge][2] = vtx_2;
    obj_edges[edge][3] = vtx_3;
}

uint32_t Sculptor::Geometry::add_face(int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                                      uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3)
{
    assert(num_faces < max_faces);
    const uint32_t face = num_faces++;
    set_face(face, edge_0, edge_1, edge_2, edge_3, vtx_0, vtx_1, vtx_2, vtx_3);
    return face;
}

void Sculptor::Geometry::set_face(uint32_t face_id, int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3,
                                  uint32_t vtx_0, uint32_t vtx_1, uint32_t vtx_2, uint32_t vtx_3)
{
    assert(face_id < num_faces);

    assert(edge_0 < static_cast<int32_t>(num_edges));
    assert(edge_1 < static_cast<int32_t>(num_edges));
    assert(edge_2 < static_cast<int32_t>(num_edges));
    assert(edge_3 < static_cast<int32_t>(num_edges));
    assert(edge_0 >= -static_cast<int32_t>(num_edges));
    assert(edge_1 >= -static_cast<int32_t>(num_edges));
    assert(edge_2 >= -static_cast<int32_t>(num_edges));
    assert(edge_3 >= -static_cast<int32_t>(num_edges));

    assert(vtx_0 < num_vertices);
    assert(vtx_1 < num_vertices);
    assert(vtx_2 < num_vertices);
    assert(vtx_3 < num_vertices);

    obj_faces[face_id].edges[0] = edge_0;
    obj_faces[face_id].edges[1] = edge_1;
    obj_faces[face_id].edges[2] = edge_2;
    obj_faces[face_id].edges[3] = edge_3;
    obj_faces[face_id].ctrl_vertices[0] = vtx_0;
    obj_faces[face_id].ctrl_vertices[1] = vtx_1;
    obj_faces[face_id].ctrl_vertices[2] = vtx_2;
    obj_faces[face_id].ctrl_vertices[3] = vtx_3;

    validate_face(face_id);
}

void Sculptor::Geometry::validate_face(uint32_t face_id)
{
#ifndef NDEBUG
    const Face& face = obj_faces[face_id];

    const bool inverse_edge[] = {
        face.edges[0] < 0,
        face.edges[1] < 0,
        face.edges[2] < 0,
        face.edges[3] < 0
    };

    const int32_t edge_sel[] {
        inverse_edge[0] ? (-face.edges[0] - 1) : face.edges[0],
        inverse_edge[1] ? (-face.edges[1] - 1) : face.edges[1],
        inverse_edge[2] ? (-face.edges[2] - 1) : face.edges[2],
        inverse_edge[3] ? (-face.edges[3] - 1) : face.edges[3]
    };

    assert(edge_sel[0] < static_cast<int32_t>(num_edges));
    assert(edge_sel[1] < static_cast<int32_t>(num_edges));
    assert(edge_sel[2] < static_cast<int32_t>(num_edges));
    assert(edge_sel[3] < static_cast<int32_t>(num_edges));

    uint32_t e_vertices[4][2];

    for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
        const Edge& edge    = obj_edges[edge_sel[i_edge]];
        const bool  inverse = inverse_edge[i_edge];

        assert(edge[0] < num_vertices);
        assert(edge[1] < num_vertices);
        assert(edge[2] < num_vertices);
        assert(edge[3] < num_vertices);

        e_vertices[i_edge][0] = edge[inverse ? 3 : 0];
        e_vertices[i_edge][1] = edge[inverse ? 0 : 3];
    }

    assert(e_vertices[0][0] == e_vertices[1][0]);
    assert(e_vertices[0][1] == e_vertices[2][0]);
    assert(e_vertices[3][0] == e_vertices[1][1]);
    assert(e_vertices[3][1] == e_vertices[2][1]);
#endif
}

void Sculptor::Geometry::set_cube()
{
    num_vertices = 0;
    num_edges    = 0;
    num_faces    = 0;

    static const int16_t cube_vertices[] = {
        -3,  3, -3,
        -1,  3, -3,
         1,  3, -3,
         3,  3, -3,
        -3,  1, -3,
        -1,  1, -3,
         1,  1, -3,
         3,  1, -3,
        -3, -1, -3,
        -1, -1, -3,
         1, -1, -3,
         3, -1, -3,
        -3, -3, -3,
        -1, -3, -3,
         1, -3, -3,
         3, -3, -3,

        -3,  3, -1,
        -1,  3, -1,
         1,  3, -1,
         3,  3, -1,
        -3,  1, -1,
         3,  1, -1,
        -3, -1, -1,
         3, -1, -1,
        -3, -3, -1,
        -1, -3, -1,
         1, -3, -1,
         3, -3, -1,

        -3,  3,  1,
        -1,  3,  1,
         1,  3,  1,
         3,  3,  1,
        -3,  1,  1,
         3,  1,  1,
        -3, -1,  1,
         3, -1,  1,
        -3, -3,  1,
        -1, -3,  1,
         1, -3,  1,
         3, -3,  1,

        -3,  3,  3,
        -1,  3,  3,
         1,  3,  3,
         3,  3,  3,
        -3,  1,  3,
        -1,  1,  3,
         1,  1,  3,
         3,  1,  3,
        -3, -1,  3,
        -1, -1,  3,
         1, -1,  3,
         3, -1,  3,
        -3, -3,  3,
        -1, -3,  3,
         1, -3,  3,
         3, -3,  3,
    };

    constexpr int16_t multiplier = 128;

    for (unsigned i = 0; i < mstd::array_size(cube_vertices); i += 3)
        add_vertex(cube_vertices[i] * multiplier,
                   cube_vertices[i + 1] * multiplier,
                   cube_vertices[i + 2] * multiplier);

    const uint32_t cube_edges[] = {
         0,  1,  2,  3,
         0,  4,  8, 12,
         3,  7, 11, 15,
        12, 13, 14, 15,

         0, 16, 28, 40,
         3, 19, 31, 43,
        15, 27, 39, 55,
        12, 24, 36, 52,

        40, 41, 42, 43,
        40, 44, 48, 52,
        43, 47, 51, 55,
        52, 53, 54, 55
    };

    for (unsigned i = 0; i < mstd::array_size(cube_edges); i += 4)
        add_edge(cube_edges[i],
                 cube_edges[i + 1],
                 cube_edges[i + 2],
                 cube_edges[i + 3]);

    const int32_t cube_face_edges[] = {
         0,  1,   2,   3,
         4,  0,   8,   5,
         5,  2,  10,   6,
         6, -4, -12,   7,
         7, -2, -10,   4,
        -9, 10,   9, -12
    };

    const uint32_t cube_face_vertices[] = {
         5,  6,  9, 10,
        17, 29, 18, 30,
        21, 33, 23, 35,
        26, 38, 25, 37,
        22, 34, 20, 32,
        46, 45, 50, 49
    };

    assert(mstd::array_size(cube_face_edges) == mstd::array_size(cube_face_vertices));

    for (unsigned i = 0; i < mstd::array_size(cube_face_edges); i += 4)
        add_face(cube_face_edges[i],
                 cube_face_edges[i + 1],
                 cube_face_edges[i + 2],
                 cube_face_edges[i + 3],
                 cube_face_vertices[i],
                 cube_face_vertices[i + 1],
                 cube_face_vertices[i + 2],
                 cube_face_vertices[i + 3]);

    set_dirty();
}

void Sculptor::Geometry::render(VkCommandBuffer cmd_buf)
{
    static const VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd_buf,
                           0, // firstBinding
                           1, // bindingCount
                           &vertices.get_buffer(),
                           &vb_offset);

    vkCmdBindIndexBuffer(cmd_buf,
                         indices.get_buffer(),
                         0, // offset
                         VK_INDEX_TYPE_UINT16);

    vkCmdDrawIndexed(cmd_buf,
                     num_indices,
                     1,
                     0,  // firstVertex
                     0,  // vertexOffset
                     0); // firstInstance
}

void Sculptor::Geometry::render_edges(VkCommandBuffer cmd_buf)
{
    static const VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd_buf,
                           0, // firstBinding
                           1, // bindingCount
                           &vertices.get_buffer(),
                           &vb_offset);

    vkCmdBindIndexBuffer(cmd_buf,
                         indices.get_buffer(),
                         edge_indices_offset * sizeof(uint16_t),
                         VK_INDEX_TYPE_UINT16);

    vkCmdDrawIndexed(cmd_buf,
                     num_edge_indices,
                     1,
                     0,  // firstVertex
                     0,  // vertexOffset
                     0); // firstInstance
}
