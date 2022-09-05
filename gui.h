// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vulkan_functions.h"

#ifdef ENABLE_GUI

#   include "imgui/imgui.h"
#   include "imgui/backends/imgui_impl_vulkan.h"

bool init_gui();
bool send_gui_to_gpu(VkCommandBuffer cmdbuf);

#else

inline bool init_gui() { return true; }
template<typename T>
inline bool send_gui_to_gpu(T cmdbuf) { return true; }

#endif

bool is_full_screen();
