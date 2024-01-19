// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_editor.h"
#include "sculptor_geometry.h"
#include "../resource.h"
#include "../minivulkan.h"
#include "../vmath.h"

namespace Sculptor {

class GeometryEditor: public Editor {
    public:
        ~GeometryEditor() override = default;
        const char* get_editor_name() const override;
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input) override;
        bool allocate_resources() override;
        void free_resources() override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

    private:
#       ifdef __APPLE__
#           define CTRL_KEY "Cmd "
#           define SHIFT_CTRL_KEY "Shift Cmd "
#       else
#           define CTRL_KEY "Ctrl "
#           define SHIFT_CTRL_KEY "Shift Ctrl "
#       endif
#       define TOOLBAR_BUTTONS \
            X(new_cube,         0, "",                 "New cube")                    \
            X(undo,             1, CTRL_KEY "Z",       "Undo")                        \
            X(redo,             0, SHIFT_CTRL_KEY "Z", "Redo")                        \
            X(copy,             0, CTRL_KEY "C",       "Copy")                        \
            X(paste,            0, CTRL_KEY "V",       "Paste")                       \
            X(cut,              0, CTRL_KEY "X",       "Cut")                         \
            X(sel_vertices,     1, "1",                "Select vertices")             \
            X(sel_edges,        0, "2",                "Select edges")                \
            X(sel_faces,        0, "3",                "Select faces")                \
            X(sel_clear,        0, "",                 "Clear selection")             \
            X(view_perspective, 1, "5",                "Perspective view")            \
            X(view_ortho_z,     0, "6",                "Orthographic view in Z axis") \
            X(view_ortho_x,     0, "7",                "Orthographic view in X axis") \
            X(view_ortho_y,     0, "8",                "Orthographic view in Y axis") \
            X(toggle_tessell,   1, "Alt T",            "Toggle tessellation")         \
            X(toggle_wireframe, 0, "Alt W",            "Toggle wireframe")            \
            X(snap_x,           1, "X",                "Snap to X")                   \
            X(snap_y,           0, "Y",                "Snap to Y")                   \
            X(snap_z,           0, "Z",                "Snap to Z")                   \
            X(move,             1, "G",                "Move")                        \
            X(rotate,           0, "R",                "Rotate")                      \
            X(scale,            0, "S",                "Scale")                       \
            X(erase,            1, "Del",              "Delete selection")            \
            X(extrude,          0, "E",                "Extrude")                     \

        enum class ToolbarButton {
#           define X(tag, first, combo, desc) tag,
            TOOLBAR_BUTTONS
#           undef X
        };

        struct Resources {
            Image           color;
            Image           depth;
            Image           select_feedback;
            Image           host_select_feedback;
            bool            selection_pending = false;
            VkDescriptorSet gui_texture       = VK_NULL_HANDLE;
        };

        enum class ViewType {
            free_moving,
            front,
            num_types
        };

        struct Camera {
            vmath::vec3 pos         {0.0f};
            float       distance    = 0;
            float       view_height = 0;
            float       yaw         = 0;
            float       pitch       = 0;
        };

        struct View {
            uint32_t  width         = 0;
            uint32_t  height        = 0;
            uint32_t  host_sel_size = 0;
            ViewType  view_type     = ViewType::free_moving;
            Camera    camera[static_cast<int>(ViewType::num_types)];
            Resources res[max_swapchain_size];
        };

        bool alloc_view_resources(View*     dst_view,
                                  uint32_t  width,
                                  uint32_t  height,
                                  VkSampler viewport_sampler);
        bool allocate_resources_once();
        void free_view_resources(View* dst_view);
        bool create_materials();
        bool create_transforms_buffer();
        bool create_descriptor_sets();
        void gui_status_bar();
        bool gui_toolbar();
        bool toolbar_button(ToolbarButton button, bool* checked = nullptr);
        bool draw_geometry_view(VkCommandBuffer cmdbuf, View& dst_view, uint32_t image_idx);
        bool draw_selection_feedback(VkCommandBuffer cmdbuf, View& dst_view, uint32_t image_idx);
        bool render_geometry(VkCommandBuffer cmdbuf, const View& dst_view, uint32_t image_idx);
        bool set_patch_transforms(const View& dst_view, uint32_t transform_id);

        View               view;
        uint32_t           window_width      = 0;
        uint32_t           window_height     = 0;
        uint32_t           materials_stride  = 0;
        uint32_t           transforms_stride = 0;
        // 3 descriptor sets:
        // - desc set 0: global and per-frame resources
        // - desc set 1: per-material resources
        // - desc set 2: per-object resources
        VkDescriptorSet    desc_set[3]       = { };
        VkPipeline         gray_patch_mat    = VK_NULL_HANDLE;
        VkDescriptorSet    toolbar_texture   = VK_NULL_HANDLE;
        Sculptor::Geometry patch_geometry;
        Buffer             materials_buf;
        Buffer             transforms_buf;

        struct ToolbarState {
            bool sel_vertices;
            bool sel_edges;
            bool sel_faces;

            bool view_perspective;
            bool view_ortho_z;
            bool view_ortho_x;
            bool view_ortho_y;

            bool toggle_tessellation;
            bool toggle_wireframe;

            bool snap_x;
            bool snap_y;
            bool snap_z;

            bool move;
            bool rotate;
            bool scale;

            bool extrude;
        };

        ToolbarState       toolbar_state     = { };
};

}
