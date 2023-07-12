// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "example.h"

#include "../gui.h"
#include "../minivulkan.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

const int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard;

const unsigned gui_num_descriptors = 1;

void notify_gui_heap_freed()
{
}

bool create_gui_frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width)  / vk_surface_scale;
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height) / vk_surface_scale;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Shape settings");
    ImGui::SliderFloat("Roundedness", &user_roundedness, 0.0f, 1.0f);
    ImGui::SliderInt("Tessellation Level", reinterpret_cast<int*>(&user_tess_level), 1, 20);
    ImGui::Checkbox("Wireframe", &user_wireframe);
    ImGui::End();

    return true;
}
