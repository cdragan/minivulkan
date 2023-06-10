// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_geometry.h"

constexpr uint32_t max_vertices = 65536;
constexpr uint32_t max_indices  = 65536;

bool Sculptor::Geometry::allocate()
{
    if ( ! vertices.allocate(Usage::fixed,
                             max_vertices * sizeof(Vertex),
                             VK_FORMAT_UNDEFINED,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_vertices.allocate(Usage::host_only,
                                  max_vertices * sizeof(Vertex),
                                  VK_FORMAT_UNDEFINED,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    if ( ! indices.allocate(Usage::fixed,
                            max_indices * sizeof(uint16_t),
                            VK_FORMAT_UNDEFINED,
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_indices.allocate(Usage::host_only,
                                 max_indices * sizeof(uint16_t),
                                 VK_FORMAT_UNDEFINED,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    if ( ! faces.allocate(Usage::fixed,
                          max_faces * sizeof(PatchFace),
                          VK_FORMAT_UNDEFINED,
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_faces.allocate(Usage::host_only,
                               max_faces * sizeof(PatchFace),
                               VK_FORMAT_UNDEFINED,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    return true;
}

bool Sculptor::Geometry::send_to_gpu(VkCommandBuffer cmd_buf)
{
    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    if (num_vertices) {
        copy_region.size = num_vertices * sizeof(Vertex);

        vkCmdCopyBuffer(cmd_buf, host_vertices.get_buffer(), vertices.get_buffer(), 1, &copy_region);
    }

    num_indices = 0;
    uint16_t* const indices_ptr = host_indices.get_ptr<uint16_t>();
    for (uint32_t i_face = 0; i_face < num_faces; i_face++) {
        const Face& face = obj_faces[i_face];

        for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
            const int32_t edge_sel     = face.edges[i_edge];
            const bool    inverse_edge = edge_sel < 0;
            const Edge&   edge         = obj_edges[inverse_edge ? -edge_sel - 1 : edge_sel];

            for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
                assert(num_indices < max_indices);
                assert(edge[i_idx] < max_vertices);
                indices_ptr[num_indices++] = static_cast<uint16_t>(edge[i_idx]);
            }
        }
    }

    if (num_indices) {
        copy_region.size = num_indices * sizeof(Vertex);

        vkCmdCopyBuffer(cmd_buf, host_indices.get_buffer(), indices.get_buffer(), 1, &copy_region);
    }

    if (num_faces) {
        copy_region.size = num_faces * sizeof(PatchFace);

        vkCmdCopyBuffer(cmd_buf, host_faces.get_buffer(), faces.get_buffer(), 1, &copy_region);
    }

    return true;
}

uint32_t Sculptor::Geometry::add_vertex(float x, float y, float z)
{
    assert(num_vertices < max_vertices);
    const uint32_t vtx = num_vertices++;
    set_vertex(vtx, x, y, z);
    return vtx;
}

void Sculptor::Geometry::set_vertex(uint32_t vtx, float x, float y, float z)
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

uint32_t Sculptor::Geometry::add_face(int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3)
{
    assert(num_faces < max_faces);
    const uint32_t face = num_faces++;
    set_face(face, edge_0, edge_1, edge_2, edge_3);
    return face;
}

void Sculptor::Geometry::set_face(uint32_t face, int32_t edge_0, int32_t edge_1, int32_t edge_2, int32_t edge_3)
{
    assert(face < num_faces);
    assert(edge_0 < static_cast<int32_t>(num_edges));
    assert(edge_1 < static_cast<int32_t>(num_edges));
    assert(edge_2 < static_cast<int32_t>(num_edges));
    assert(edge_3 < static_cast<int32_t>(num_edges));
    assert(edge_0 >= -static_cast<int32_t>(num_edges));
    assert(edge_1 >= -static_cast<int32_t>(num_edges));
    assert(edge_2 >= -static_cast<int32_t>(num_edges));
    assert(edge_3 >= -static_cast<int32_t>(num_edges));
    obj_faces[face].edges[0] = edge_0;
    obj_faces[face].edges[1] = edge_1;
    obj_faces[face].edges[2] = edge_2;
    obj_faces[face].edges[3] = edge_3;
}
