// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "editor.h"
#include "../resource.h"
#include "../minivulkan.h"

class GeometryEditor: public Editor {
    public:
        ~GeometryEditor() override = default;
        void set_name(const char* new_name);
        bool create_gui_frame(uint32_t image_idx) override;
        bool draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx) override;

    private:
        struct Resources {
            Image           color;
            Image           depth;
            Image           selection;
            Image           host_selection;
            bool            selection_pending = false;
            VkDescriptorSet gui_texture       = VK_NULL_HANDLE;
        };

        struct View {
            uint32_t  width  = 0;
            uint32_t  height = 0;
            Resources res[max_swapchain_size];
        };

        bool alloc_view_resources(View*     dst_view,
                                  uint32_t  width,
                                  uint32_t  height,
                                  VkSampler viewport_sampler);
        void free_view_resources(View* dst_view);
        void gui_status_bar();

        View view;
        char name[128] = { };
};
