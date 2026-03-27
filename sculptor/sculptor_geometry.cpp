// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_geometry.h"

#include "../core/barrier.h"
#include "../core/mstdc.h"
#include "../core/suballoc.h"
#include "../core/vmath.h"

#include <errno.h>
#include <iterator>
#include <stdio.h>
#include <string.h>

constexpr uint32_t max_indices          = 65536;
constexpr uint32_t max_face_indices     = 43008;
constexpr uint32_t max_edge_indices     = 22528;
static_assert(max_edge_indices + max_face_indices == max_indices);
constexpr uint32_t num_host_copies      = 3;

constexpr uint32_t host_vertices_offset = 0;
constexpr uint32_t gpu_vertices_offset  = 0;
constexpr uint32_t vertices_stride      = Sculptor::Geometry::max_vertices * sizeof(Sculptor::Geometry::Vertex);

constexpr uint32_t host_indices_offset  = vertices_stride;
constexpr uint32_t gpu_indices_offset   = vertices_stride;
constexpr uint32_t indices_stride       = max_indices * sizeof(uint16_t);

constexpr uint32_t host_faces_offset    = vertices_stride + indices_stride * num_host_copies;
constexpr uint32_t gpu_faces_offset     = vertices_stride + indices_stride;
constexpr uint32_t faces_stride         = sizeof(Sculptor::Geometry::FacesBuf) + (Sculptor::Geometry::max_faces - 1) * sizeof(Sculptor::Geometry::FaceData);

// Scratch buffer used for temporary data
alignas(16) static uint8_t scratch_buf[Sculptor::Geometry::max_vertices * 64];

// Maps edges to indices
static const uint8_t edge_indices[] = {
    0,  1,  2,  3,  // top
    0,  4,  8,  12, // left
    3,  7,  11, 15, // right
    12, 13, 14, 15  // bottom
};

static const uint8_t  file_type[4] = {'G', 'E', 'O', 'M'};
static const uint16_t file_version = 1;

static int16_t to_int16(float f)
{
    return static_cast<int16_t>(vmath::clamp(f, -32767.0f, 32767.0f));
}

struct EdgeIndex {
    uint32_t idx;
    bool     inverse_edge;
};

static int32_t get_edge_sel(const int32_t edge_sel, const bool inverse_edge)
{
    return inverse_edge ? (-edge_sel - 1) : edge_sel;
}

static EdgeIndex get_edge_idx(const Sculptor::Geometry::Face& face, const uint32_t i_edge)
{
    const int32_t edge_sel     = face.edges[i_edge];
    const bool    inverse_edge = edge_sel < 0;

    return { static_cast<uint32_t>(get_edge_sel(edge_sel, inverse_edge)), inverse_edge };
}

bool Sculptor::Geometry::allocate()
{
    undo_redo.init(undo_buf);

    if ( ! gpu_buffer.allocate(Usage::fixed,
                               vertices_stride + indices_stride + faces_stride,
                               VK_FORMAT_UNDEFINED,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               "geometry buffer"))
        return false;

    if ( ! host_buffer.allocate(Usage::host_only,
                                vertices_stride + (indices_stride + faces_stride) * num_host_copies,
                                VK_FORMAT_UNDEFINED,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                "geometry host buffer"))
        return false;

    return true;
}

bool Sculptor::Geometry::send_to_gpu(VkCommandBuffer cmd_buf)
{
    if ( ! dirty)
        return true;

    static const Buffer::Transition gpu_buffer_graphics_to_transfer = {
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT
    };
    gpu_buffer.barrier(gpu_buffer_graphics_to_transfer);

    static const Buffer::Transition host_buffer_host_to_transfer = {
        VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_HOST_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    };
    host_buffer.barrier(host_buffer_host_to_transfer);

    send_barrier(cmd_buf);

    last_buffer = (last_buffer + 1) % num_host_copies;

    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    if (num_vertices) {
        copy_region.srcOffset = host_vertices_offset;
        copy_region.dstOffset = gpu_vertices_offset;
        copy_region.size      = num_vertices * sizeof(Vertex);

        vkCmdCopyBuffer(cmd_buf, host_buffer.get_buffer(), gpu_buffer.get_buffer(), 1, &copy_region);
    }

    const uint32_t cur_faces_offset = host_faces_offset + last_buffer * faces_stride;
    FacesBuf* const faces_ptr = host_buffer.get_ptr<FacesBuf>(cur_faces_offset);
    faces_ptr->tess_level[0] = tess_level;

    // Write 16 indices for each face to the host copy of the index buffer
    const uint32_t cur_indices_offset = host_indices_offset + last_buffer * indices_stride;
    num_indices = 0;
    uint16_t* const indices_ptr = host_buffer.get_ptr<uint16_t>(cur_indices_offset);
    for (uint32_t i_face = 0; i_face < num_faces; i_face++) {
        assert(num_indices + 16 <= max_face_indices);

        const Face& face = obj_faces[i_face];

        faces_ptr->face_data[i_face].material_id = face.material_id;

        // Write indices for the edges; note the indices of corners overlap
        for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
            const auto [edge_idx, inverse_edge] = get_edge_idx(face, i_edge);
            const Edge& edge = obj_edges[edge_idx];

            for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
                const uint32_t src_idx = inverse_edge ? (3 - i_idx) : i_idx;
                assert(edge.vertices[src_idx] < max_vertices);
                const uint32_t dest_idx = num_indices + edge_indices[i_edge * 4 + i_idx];
                assert(dest_idx < num_indices + 16);
                indices_ptr[dest_idx] = static_cast<uint16_t>(edge.vertices[src_idx]);
            }
        }

        static const uint32_t ctrl_idx_map[] = {
            5,  6,
            9, 10
        };

        // Write 4 center indices which control the face, which are not included in edges
        for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
            assert(face.ctrl_vertices[i_idx] < max_vertices);
            const uint32_t dest_idx = num_indices + ctrl_idx_map[i_idx];
            assert(dest_idx < num_indices + 16);
            indices_ptr[dest_idx] = static_cast<uint16_t>(face.ctrl_vertices[i_idx]);
        }

        num_indices += 16;
    }

    // Write indices for each edge to the host copy of the index buffer;
    // The edge indices are used for drawing patch edges and follow face indices
    num_edge_indices = 0;
    for (uint32_t i_edge = 0; i_edge < num_edges; i_edge++) {
        assert(num_edge_indices + 4 <= max_edge_indices);

        const Edge& edge = obj_edges[i_edge];

        for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
            assert(edge.vertices[i_idx] < max_vertices);
            const uint32_t dest_idx = max_face_indices + num_edge_indices + i_idx;
            indices_ptr[dest_idx] = static_cast<uint16_t>(edge.vertices[i_idx]);
        }

        num_edge_indices += 4;
    }

    if (num_indices + num_edge_indices) {
        copy_region.srcOffset = cur_indices_offset;
        copy_region.dstOffset = gpu_indices_offset;
        copy_region.size      = (max_face_indices + num_edge_indices) * sizeof(uint16_t);

        vkCmdCopyBuffer(cmd_buf, host_buffer.get_buffer(), gpu_buffer.get_buffer(), 1, &copy_region);
    }

    if (num_faces) {
        copy_region.srcOffset = cur_faces_offset;
        copy_region.dstOffset = gpu_faces_offset;
        copy_region.size      = sizeof(FacesBuf) + (num_faces - 1) * sizeof(FaceData);

        vkCmdCopyBuffer(cmd_buf, host_buffer.get_buffer(), gpu_buffer.get_buffer(), 1, &copy_region);
    }

    static const Buffer::Transition gpu_buffer_transfer_to_graphics = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT
    };
    gpu_buffer.barrier(gpu_buffer_transfer_to_graphics);

    static const Buffer::Transition host_buffer_transfer_to_host = {
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_HOST_WRITE_BIT
    };
    host_buffer.barrier(host_buffer_transfer_to_host);

    send_barrier(cmd_buf);

    dirty = false;

    return true;
}

void Sculptor::Geometry::move_vertex(const uint32_t vtx, const vmath::vec3 delta)
{
    assert(vtx < num_vertices);
    Vertex& vertex = host_buffer.get_ptr<Vertex>(host_vertices_offset)[vtx];
    vertex.pos[0] = to_int16(vertex.pos[0] + delta.x);
    vertex.pos[1] = to_int16(vertex.pos[1] + delta.y);
    vertex.pos[2] = to_int16(vertex.pos[2] + delta.z);
}

void Sculptor::Geometry::get_face_vertex_indices(uint32_t face_id, uint32_t out_vtx[16]) const
{
    assert(face_id < num_faces);

    const Face& face = obj_faces[face_id];

    for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
        const auto [edge_idx, inverse_edge] = get_edge_idx(face, i_edge);
        const Edge& edge = obj_edges[edge_idx];

        for (uint32_t i_idx = 0; i_idx < 4; i_idx++) {
            const uint32_t src_idx  = inverse_edge ? (3 - i_idx) : i_idx;
            const uint32_t dest_idx = edge_indices[i_edge * 4 + i_idx];
            out_vtx[dest_idx] = edge.vertices[src_idx];
        }
    }

    static const uint32_t ctrl_idx_map[] = { 5, 6, 9, 10 };
    for (uint32_t i_idx = 0; i_idx < 4; i_idx++)
        out_vtx[ctrl_idx_map[i_idx]] = face.ctrl_vertices[i_idx];
}

vmath::vec3 Sculptor::Geometry::get_vertex(const uint32_t vtx) const
{
    const Vertex* const vertices = host_buffer.get_ptr<Vertex>(host_vertices_offset);

    return {
        static_cast<float>(vertices[vtx].pos[0]),
        static_cast<float>(vertices[vtx].pos[1]),
        static_cast<float>(vertices[vtx].pos[2])
    };
}

void Sculptor::Geometry::freeze_selection(const uint8_t* const face_sel,
                                          const uint8_t* const vtx_sel)
{
    static_assert(max_faces <= 1 << (8 * sizeof(uint16_t)));
    num_sel_faces = 0;
    for (uint32_t i = 0; i < num_faces; i++)
        if (face_sel[i] & obj_selected)
            sel_faces[num_sel_faces++] = static_cast<uint16_t>(i);

    static_assert(max_vertices <= 1 << (8 * sizeof(uint16_t)));
    num_sel_vertices = 0;
    for (uint32_t i = 0; i < num_vertices; i++)
        if (vtx_sel[i] & obj_selected)
            sel_vertices[num_sel_vertices++] = static_cast<uint16_t>(i);
}

void Sculptor::Geometry::invalidate_selection()
{
    num_sel_faces    = 0;
    num_sel_vertices = 0;
}

void Sculptor::Geometry::get_vertex_dirs_from_face_normals(vmath::vec3* const vtx_dirs)
{
    for (uint32_t i_face = 0; i_face < num_sel_faces; i_face++) {
        uint32_t vtx_list[16];
        get_face_vertex_indices(sel_faces[i_face], vtx_list);

        // Face's corner vertices
        //  0 ..  3
        //  : ::  :
        // 12 .. 15
        const vmath::vec3 v0  = get_vertex(vtx_list[0]);
        const vmath::vec3 v3  = get_vertex(vtx_list[3]);
        const vmath::vec3 v12 = get_vertex(vtx_list[12]);
        const vmath::vec3 v15 = get_vertex(vtx_list[15]);

        const vmath::vec3 face_horiz = (v3  - v0) + (v15 - v12);
        const vmath::vec3 face_vert  = (v12 - v0) + (v15 - v3);
        const vmath::vec3 normal     = vmath::normalize(vmath::cross_product(face_horiz, face_vert));

        for (uint32_t i = 0; i < std::size(vtx_list); i++)
            vtx_dirs[vtx_list[i]] += normal;
    }
}

void Sculptor::Geometry::move_selection(const vmath::vec3 delta, const MoveMode mode)
{
    BufferSubAllocator<1> alloc{scratch_buf};

    const uint32_t num_slots = mstd::align_up(num_vertices, 32u) / 32u;
    uint32_t* const vtx_bitmap = alloc.allocate<uint32_t>(num_slots);
    memset(vtx_bitmap, 0, num_slots * sizeof(uint32_t));

    vmath::vec3* vtx_dirs = nullptr;
    if (mode == MoveMode::along_normal) {
        vtx_dirs = alloc.allocate<vmath::vec3>(num_vertices);
        memset(vtx_dirs, 0, num_vertices * sizeof(vmath::vec3));
        get_vertex_dirs_from_face_normals(vtx_dirs);
    }

    for (uint32_t i_face = 0; i_face < num_sel_faces; i_face++) {
        uint32_t vtx_list[16];
        get_face_vertex_indices(sel_faces[i_face], vtx_list);

        for (uint32_t i = 0; i < std::size(vtx_list); i++) {
            const uint32_t i_vtx   = vtx_list[i];
            const uint32_t slot    = i_vtx >> 5;
            const uint32_t bitmask = 1u << (i_vtx & 31u);
            assert(i_vtx < num_vertices);

            if ( ! (vtx_bitmap[slot] & bitmask)) {
                vtx_bitmap[slot] |= bitmask;

                vmath::vec3 vtx_delta = delta;
                if (vtx_dirs)
                    vtx_delta = vmath::normalize(vtx_dirs[i_vtx]) * (delta.x + delta.y + delta.z);

                move_vertex(i_vtx, vtx_delta);
            }
        }
    }

    const uint32_t num_vert_to_move = vtx_dirs ? 0 : num_sel_vertices;
    for (uint32_t i = 0; i < num_vert_to_move; i++) {
        const uint32_t i_vtx   = sel_vertices[i];
        const uint32_t slot    = i_vtx >> 5;
        const uint32_t bitmask = 1u << (i_vtx & 31u);
        assert(i_vtx < num_vertices);

        if ( ! (vtx_bitmap[slot] & bitmask)) {
            vtx_bitmap[slot] |= bitmask;
            move_vertex(i_vtx, delta);
        }
    }

    set_dirty();
}

void Sculptor::Geometry::extrude_faces(const uint8_t* const face_sel,
                                       vmath::vec3    const delta,
                                       MoveMode       const mode)
{
    if (delta.x == 0 && delta.y == 0 && delta.z == 0)
        return;

    BufferSubAllocator<1> alloc{scratch_buf};

    // Find inner edges, which will not be extruded.
    // Also count how many outer edges connect to each corner vertex.
    uint8_t* const edge_count   = alloc.allocate<uint8_t>(max_edges);
    uint8_t* const outer_corner = alloc.allocate<uint8_t>(max_vertices);
    memset(edge_count,   0, num_edges    * sizeof(uint8_t));
    memset(outer_corner, 0, num_vertices * sizeof(uint8_t));

    for (uint32_t i_face = 0; i_face < num_sel_faces; i_face++) {
        const Face& face = obj_faces[sel_faces[i_face]];

        for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
            const uint32_t edge_idx = get_edge_idx(face, i_edge).idx;

            uint8_t& count = edge_count[edge_idx];
            ++count;
            assert(count <= 2);

            const Edge& edge = obj_edges[edge_idx];
            outer_corner[edge.vertices[0]] += (count == 1) ? 1 : -1;
            outer_corner[edge.vertices[3]] += (count == 1) ? 1 : -1;
        }
    }

    constexpr uint16_t no_object = 0xFFFFu;

    // Map for corner vertices (no_object = not created yet)
    uint16_t* const corner_vtx = alloc.allocate<uint16_t>(max_vertices);
    memset(corner_vtx, 0xFF, num_vertices * sizeof(uint16_t));

    // Map corner vertices to adjacent extruded edges, to reuse the edges
    // for adjacent faces
    uint16_t* const conn_edge = alloc.allocate<uint16_t>(max_vertices);
    memset(conn_edge, 0xFF, num_vertices * sizeof(uint16_t));

    // Bitmap tracking which original vertices have been moved (for inner non-extruded edges and vertices)
    uint32_t* const moved_vtx_bitmap = alloc.allocate<uint32_t>(mstd::align_up(max_vertices, 32u) / 32u);
    memset(moved_vtx_bitmap, 0, mstd::align_up(num_vertices, 32u) / 32u * sizeof(uint32_t));

    vmath::vec3* vtx_dirs = nullptr;
    if (mode == MoveMode::along_normal) {
        vtx_dirs = alloc.allocate<vmath::vec3>(max_vertices);
        memset(vtx_dirs, 0, num_vertices * sizeof(vmath::vec3));
        get_vertex_dirs_from_face_normals(vtx_dirs);
    }
    const auto get_delta = [&](const uint32_t i_vtx) -> vmath::vec3 {
        if (vtx_dirs)
            return vmath::normalize(vtx_dirs[i_vtx]) * (delta.x + delta.y + delta.z);
        return delta;
    };

    for (uint32_t i_face = 0; i_face < num_sel_faces; i_face++) {
        Face& face = obj_faces[sel_faces[i_face]];

        for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
            const auto [edge_idx, inverse_edge] = get_edge_idx(face, i_edge);

            const Edge& edge = obj_edges[edge_idx];

            // Inner edges (with selected face on both sides) are moved and not extruded
            if (edge_count[edge_idx] >= 2) {
                assert(edge_count[edge_idx] <= 3);

                // Use 3 to mark an edge has already been moved
                if (edge_count[edge_idx] == 3)
                    continue;
                edge_count[edge_idx] = 3;

                // Duplicate boundary corners on the outer edges
                for (uint32_t i_idx = 0; i_idx <= 3; i_idx += 3) {
                    const uint32_t i_vtx = edge.vertices[i_idx];

                    if (outer_corner[i_vtx]) {
                        if (corner_vtx[i_vtx] == no_object)
                            corner_vtx[i_vtx] = static_cast<uint16_t>(add_vertex(get_vertex(i_vtx) + get_delta(i_vtx)));

                        obj_edges[edge_idx].vertices[i_idx] = corner_vtx[i_vtx];
                    }
                    else {
                        const uint32_t slot    = i_vtx >> 5;
                        const uint32_t bitmask = 1u << (i_vtx & 31u);

                        if ( ! (moved_vtx_bitmap[slot] & bitmask)) {
                            moved_vtx_bitmap[slot] |= bitmask;
                            move_vertex(i_vtx, get_delta(i_vtx));
                        }
                    }
                }

                // Move mid-edge control points for inner edges to new position
                for (uint32_t i_idx = 1; i_idx <= 2; i_idx++) {
                    const uint32_t i_vtx       = edge.vertices[i_idx];
                    const uint32_t slot    = i_vtx >> 5;
                    const uint32_t bitmask = 1u << (i_vtx & 31u);
                    if ( ! (moved_vtx_bitmap[slot] & bitmask)) {
                        moved_vtx_bitmap[slot] |= bitmask;
                        move_vertex(i_vtx, get_delta(i_vtx));
                    }
                }
                continue;
            }

            const vmath::vec3 delta0 = get_delta(edge.vertices[0]);
            const vmath::vec3 delta1 = get_delta(edge.vertices[1]);
            const vmath::vec3 delta2 = get_delta(edge.vertices[2]);
            const vmath::vec3 delta3 = get_delta(edge.vertices[3]);

            // Create new edge for the current (selected) face which has been moved
            const vmath::vec3 vtx0_pos = get_vertex(edge.vertices[0]);
            const vmath::vec3 vtx3_pos = get_vertex(edge.vertices[3]);
            if (corner_vtx[edge.vertices[0]] == no_object)
                corner_vtx[edge.vertices[0]] = static_cast<uint16_t>(add_vertex(vtx0_pos + delta0));
            if (corner_vtx[edge.vertices[3]] == no_object)
                corner_vtx[edge.vertices[3]] = static_cast<uint16_t>(add_vertex(vtx3_pos + delta3));
            const uint32_t new_vtx0     = corner_vtx[edge.vertices[0]];
            const uint32_t new_ctl1_vtx = add_vertex(get_vertex(edge.vertices[1]) + delta1);
            const uint32_t new_ctl2_vtx = add_vertex(get_vertex(edge.vertices[2]) + delta2);
            const uint32_t new_vtx3     = corner_vtx[edge.vertices[3]];
            const uint32_t new_edge_idx = add_edge(new_vtx0, new_ctl1_vtx, new_ctl2_vtx, new_vtx3);
            face.edges[i_edge]          = get_edge_sel(static_cast<int32_t>(new_edge_idx), inverse_edge);

            // Maintain correct order of edges to make sure the new side face will always
            // be facing outwards
            const bool inverse_new = (i_edge == 0 || i_edge == 2) ^ inverse_edge;

            // Create new side edges for the side face
            if (conn_edge[edge.vertices[0]] == no_object) {
                const uint32_t ctl1_vtx = add_vertex(vtx0_pos + delta0 * (2.0f / 3.0f));
                const uint32_t ctl2_vtx = add_vertex(vtx0_pos + delta0 * (1.0f / 3.0f));
                conn_edge[edge.vertices[0]] = static_cast<uint16_t>(add_edge(new_vtx0, ctl1_vtx, ctl2_vtx, edge.vertices[0]));
            }
            if (conn_edge[edge.vertices[3]] == no_object) {
                const uint32_t ctl1_vtx = add_vertex(vtx3_pos + delta3 * (2.0f / 3.0f));
                const uint32_t ctl2_vtx = add_vertex(vtx3_pos + delta3 * (1.0f / 3.0f));
                conn_edge[edge.vertices[3]] = static_cast<uint16_t>(add_edge(new_vtx3, ctl1_vtx, ctl2_vtx, edge.vertices[3]));
            }
            const uint32_t left_edge  = conn_edge[inverse_new ? edge.vertices[3] : edge.vertices[0]];
            const uint32_t right_edge = conn_edge[inverse_new ? edge.vertices[0] : edge.vertices[3]];

            // Select orientation of the original and duplicated edge from the current face, used for the new face
            const int32_t top_edge    = get_edge_sel(static_cast<int32_t>(new_edge_idx), inverse_new);
            const int32_t bottom_edge = get_edge_sel(static_cast<int32_t>(edge_idx),     inverse_new);

            // Create side face's control points
            const vmath::vec3 left_ctl    = get_vertex(edge.vertices[inverse_new ? 2 : 1]);
            const vmath::vec3 right_ctl   = get_vertex(edge.vertices[inverse_new ? 1 : 2]);
            const vmath::vec3 delta_left  = inverse_new ? delta2 : delta1;
            const vmath::vec3 delta_right = inverse_new ? delta1 : delta2;

            const uint32_t ctl0 = add_vertex(left_ctl  + delta_left  * (2.0f / 3.0f));
            const uint32_t ctl1 = add_vertex(right_ctl + delta_right * (2.0f / 3.0f));
            const uint32_t ctl2 = add_vertex(left_ctl  + delta_left  * (1.0f / 3.0f));
            const uint32_t ctl3 = add_vertex(right_ctl + delta_right * (1.0f / 3.0f));

            // Create side face
            add_face(top_edge,
                     static_cast<int32_t>(left_edge),
                     static_cast<int32_t>(right_edge),
                     bottom_edge,
                     ctl0, ctl1, ctl2, ctl3);
        }

        // Move this face's interior control points
        for (uint32_t i_ctl = 0; i_ctl < 4; i_ctl++)
            move_vertex(face.ctrl_vertices[i_ctl], get_delta(face.ctrl_vertices[i_ctl]));
    }

    set_dirty();
}

void Sculptor::Geometry::set_tess_level(const int32_t level)
{
    if (level > 0 && level != tess_level) {
        tess_level = level;
        set_dirty();
    }
}

uint32_t Sculptor::Geometry::add_vertex(int16_t x, int16_t y, int16_t z)
{
    assert(num_vertices < max_vertices);
    const uint32_t vtx = num_vertices++;
    set_vertex(vtx, x, y, z);
    return vtx;
}

uint32_t Sculptor::Geometry::add_vertex(vmath::vec3 pos)
{
    return add_vertex(to_int16(pos.x), to_int16(pos.y), to_int16(pos.z));
}

void Sculptor::Geometry::set_vertex(uint32_t vtx, int16_t x, int16_t y, int16_t z)
{
    assert(vtx < num_vertices);

    Vertex& vertex = host_buffer.get_ptr<Vertex>(host_vertices_offset)[vtx];
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

    obj_edges[edge].vertices[0] = vtx_0;
    obj_edges[edge].vertices[1] = vtx_1;
    obj_edges[edge].vertices[2] = vtx_2;
    obj_edges[edge].vertices[3] = vtx_3;
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

    obj_faces[face_id].material_id = 0;

    validate_face(face_id);
}

void Sculptor::Geometry::validate_face(uint32_t face_id)
{
#ifndef NDEBUG
    const Face& face = obj_faces[face_id];

    uint32_t e_vertices[4][2];

    for (uint32_t i_edge = 0; i_edge < 4; i_edge++) {
        const auto [edge_idx, inverse] = get_edge_idx(face, i_edge);
        assert(edge_idx >= 0);
        assert(static_cast<uint32_t>(edge_idx) < num_edges);
        const Edge& edge    = obj_edges[edge_idx];

        assert(edge.vertices[0] < num_vertices);
        assert(edge.vertices[1] < num_vertices);
        assert(edge.vertices[2] < num_vertices);
        assert(edge.vertices[3] < num_vertices);

        e_vertices[i_edge][0] = edge.vertices[inverse ? 3 : 0];
        e_vertices[i_edge][1] = edge.vertices[inverse ? 0 : 3];
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

    for (unsigned i = 0; i < std::size(cube_vertices); i += 3)
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

    for (unsigned i = 0; i < std::size(cube_edges); i += 4)
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

    assert(std::size(cube_face_edges) == std::size(cube_face_vertices));

    for (unsigned i = 0; i < std::size(cube_face_edges); i += 4)
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

bool Sculptor::Geometry::save(const char* path)
{
    if (!snapshot_state())
        return false;
    DEFER { drop_snapshot(); };

    FILE* const file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Error: Failed to open %s for writing: %s\n", path, strerror(errno));
        return false;
    }
    DEFER { fclose(file); };

    const UndoRedo::Snapshot snapshot = undo_redo.get_snapshot();
    assert(snapshot.buf);

    if (fwrite(file_type,     1, sizeof file_type,    file) != sizeof file_type    ||
        fwrite(&file_version, 1, sizeof file_version, file) != sizeof file_version ||
        fwrite(snapshot.buf,  1, snapshot.size,       file) != snapshot.size) {

        fprintf(stderr, "Error: Failed to save %s: %s\n", path, strerror(errno));
        return false;
    }

    return true;
}

bool Sculptor::Geometry::load(const char* path)
{
    FILE* const file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Error: Failed to open %s for reading: %s\n", path, strerror(errno));
        return false;
    }
    DEFER { fclose(file); };

    uint8_t  marker[sizeof file_type];
    uint16_t version;
    if (fread(marker,   1, sizeof marker,  file) != sizeof marker  ||
        fread(&version, 1, sizeof version, file) != sizeof version ||
        fseek(file, 0, SEEK_END) != 0) {

        fprintf(stderr, "Error: Failed to read %s: %s\n", path, strerror(errno));
        return false;
    }

    if (memcmp(marker, file_type, sizeof marker) != 0) {
        fprintf(stderr, "Error: File %s does not contain geometry\n", path);
        return false;
    }

    if (version != file_version) {
        fprintf(stderr, "Error: Unsupported version %u in file %s (supported version is %u)\n",
                version, path, file_version);
        return false;
    }

    const long file_size = ftell(file);
    if (file_size < static_cast<long>(sizeof file_type + sizeof file_version)) {
        fprintf(stderr, "Error: Invalid %s file size: %ld\n", path, file_size);
        return false;
    }
    fseek(file, sizeof marker + sizeof version, SEEK_SET);

    const UndoRedo::Snapshot buf = undo_redo.get_snapshot_space();

    const size_t snapshot_size = static_cast<size_t>(file_size) - 6;
    if (snapshot_size > buf.size) {
        fprintf(stderr, "Error: Not enough space to load %s (have %u bytes but need %ld)\n",
                path, buf.size, file_size);
        undo_redo.push_snapshot(0);
        return false;
    }

    if (fread(buf.buf, 1, snapshot_size, file) != snapshot_size) {
        fprintf(stderr, "Error: Failed to read %s: %s\n", path, strerror(errno));
        undo_redo.push_snapshot(0);
        return false;
    }

    undo_redo.push_snapshot(static_cast<uint32_t>(snapshot_size));
    restore_snapshot();

    return true;
}

bool Sculptor::Geometry::snapshot_state()
{
    undo_redo.init_undo_push();
    undo_redo.push(host_buffer.get_ptr<Vertex>(host_vertices_offset), num_vertices * sizeof(Vertex));
    undo_redo.push(obj_edges, num_edges * sizeof(Edge));
    undo_redo.push(obj_faces, num_faces * sizeof(Face));
    undo_redo.push(num_vertices);
    undo_redo.push(num_edges);
    undo_redo.push(num_faces);
    return undo_redo.finish_undo_push();
}

bool Sculptor::Geometry::drop_snapshot()
{
    return undo_redo.skip_undo();
}

bool Sculptor::Geometry::restore_snapshot()
{
    return apply_snapshot(true);
}

bool Sculptor::Geometry::apply_snapshot()
{
    return apply_snapshot(false);
}

bool Sculptor::Geometry::apply_snapshot(bool pop_undo)
{
    if (undo_redo.undo_empty())
        return false;

    undo_redo.init_undo();
    const uint32_t new_num_faces    = undo_redo.pop_u32();
    const uint32_t new_num_edges    = undo_redo.pop_u32();
    const uint32_t new_num_vertices = undo_redo.pop_u32();
    undo_redo.pop(obj_faces, new_num_faces * sizeof(Face));
    undo_redo.pop(obj_edges, new_num_edges * sizeof(Edge));
    undo_redo.pop(host_buffer.get_ptr<Vertex>(host_vertices_offset), new_num_vertices * sizeof(Vertex));
    if (pop_undo)
        undo_redo.finish_undo();
    else
        undo_redo.restore_undo();

    num_faces    = new_num_faces;
    num_edges    = new_num_edges;
    num_vertices = new_num_vertices;
    set_dirty();
    return true;
}

bool Sculptor::Geometry::undo()
{
    if (undo_redo.undo_empty())
        return false;

    undo_redo.init_redo_push();
    undo_redo.push(host_buffer.get_ptr<Vertex>(host_vertices_offset), num_vertices * sizeof(Vertex));
    undo_redo.push(obj_edges, num_edges * sizeof(Edge));
    undo_redo.push(obj_faces, num_faces * sizeof(Face));
    undo_redo.push(num_vertices);
    undo_redo.push(num_edges);
    undo_redo.push(num_faces);
    if (!undo_redo.finish_redo_push())
        return false;

    return restore_snapshot();
}

bool Sculptor::Geometry::redo()
{
    if (undo_redo.redo_empty())
        return false;

    undo_redo.init_undo_push();
    undo_redo.push(host_buffer.get_ptr<Vertex>(host_vertices_offset), num_vertices * sizeof(Vertex));
    undo_redo.push(obj_edges, num_edges * sizeof(Edge));
    undo_redo.push(obj_faces, num_faces * sizeof(Face));
    undo_redo.push(num_vertices);
    undo_redo.push(num_edges);
    undo_redo.push(num_faces);
    if (!undo_redo.finish_undo_push())
        return false;

    undo_redo.init_redo();
    const uint32_t new_num_faces    = undo_redo.pop_u32();
    const uint32_t new_num_edges    = undo_redo.pop_u32();
    const uint32_t new_num_vertices = undo_redo.pop_u32();
    undo_redo.pop(obj_faces, new_num_faces * sizeof(Face));
    undo_redo.pop(obj_edges, new_num_edges * sizeof(Edge));
    undo_redo.pop(host_buffer.get_ptr<Vertex>(host_vertices_offset), new_num_vertices * sizeof(Vertex));
    undo_redo.finish_redo();

    num_faces    = new_num_faces;
    num_edges    = new_num_edges;
    num_vertices = new_num_vertices;
    set_dirty();
    return true;
}

void Sculptor::Geometry::write_faces_descriptor(VkDescriptorBufferInfo* desc)
{
    desc->buffer = gpu_buffer.get_buffer();
    desc->offset = gpu_faces_offset;
    desc->range  = faces_stride;
}

void Sculptor::Geometry::write_face_indices_descriptor(VkDescriptorBufferInfo* desc)
{
    desc->buffer = gpu_buffer.get_buffer();
    desc->offset = gpu_indices_offset;
    desc->range  = max_face_indices * sizeof(uint16_t);
}

void Sculptor::Geometry::write_edge_indices_descriptor(VkDescriptorBufferInfo* desc)
{
    desc->buffer = gpu_buffer.get_buffer();
    desc->offset = gpu_indices_offset + max_face_indices * sizeof(uint16_t);
    desc->range  = max_edge_indices * sizeof(uint16_t);
}

void Sculptor::Geometry::write_edge_vertices_descriptor(VkDescriptorBufferInfo* desc)
{
    desc->buffer = gpu_buffer.get_buffer();
    desc->offset = gpu_vertices_offset;
    desc->range  = vertices_stride;
}

void Sculptor::Geometry::render(VkCommandBuffer cmd_buf)
{
    assert( ! dirty);

    const VkDeviceSize vb_offset = gpu_vertices_offset;
    vkCmdBindVertexBuffers(cmd_buf,
                           0, // firstBinding
                           1, // bindingCount
                           &gpu_buffer.get_buffer(),
                           &vb_offset);

    vkCmdBindIndexBuffer(cmd_buf,
                         gpu_buffer.get_buffer(),
                         gpu_indices_offset, // offset
                         VK_INDEX_TYPE_UINT16);

    vkCmdDrawIndexed(cmd_buf,
                     num_indices,
                     1,  // instanceCount
                     0,  // firstVertex
                     0,  // vertexOffset
                     0); // firstInstance
}

void Sculptor::Geometry::render_vertices(VkCommandBuffer cmd_buf)
{
    assert( ! dirty);

    vkCmdDraw(cmd_buf,
              4,                // vertexCount
              num_vertices,     // instanceCount
              0,                // firstVertex
              0);               // firstInstance
}

void Sculptor::Geometry::render_ctrl_pt_handles(VkCommandBuffer cmd_buf)
{
    assert( ! dirty);

    vkCmdDraw(cmd_buf,
              2,              // vertexCount
              num_faces * 12, // instanceCount (number of control point lines)
              0,              // firstVertex
              0);             // firstInstance
}
