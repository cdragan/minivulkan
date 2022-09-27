// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "../gui.h"
#include "../minivulkan.h"
#include "../mstdc.h"
#include "../shaders.h"
#include "../vmath.h"

uint32_t check_device_features()
{
    return 1;
}

bool create_additional_heaps()
{
    return false;
}

bool create_pipeline_layouts()
{
    return false;
}

bool create_pipelines()
{
    return false;
}

bool create_gui_frame()
{
    return false;
}

bool draw_frame(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    return false;
}
