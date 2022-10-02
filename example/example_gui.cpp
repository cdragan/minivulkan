// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "example.h"

#include "../minivulkan.h"

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_vulkan.h"

int gui_config_flags = ImGuiConfigFlags_NavEnableKeyboard;

bool create_gui_frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(vk_surface_caps.currentExtent.width);
    io.DisplaySize.y = static_cast<float>(vk_surface_caps.currentExtent.height);

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    ImGui::Text("Hello, world!");
    ImGui::Separator();
    ImGui::SliderFloat("Roundedness", &user_roundedness, 0.0f, 1.0f);
    ImGui::SliderInt("Tessellation Level", reinterpret_cast<int*>(&user_tess_level), 1, 20);
    ImGui::Checkbox("Wireframe", &user_wireframe);

    return true;
}
