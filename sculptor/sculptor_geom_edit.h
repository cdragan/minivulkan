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
        bool create_gui_frame(uint32_t image_idx, bool* need_realloc) override;
        bool allocate_resources() override;
        void free_resources() override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

    private:
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
        Sculptor::Geometry patch_geometry;
        Buffer             materials_buf;
        Buffer             transforms_buf;
};

}
