// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#ifdef ENABLE_GUI
bool init_gui();
bool create_gui_frame();
bool send_gui_to_gpu(VkCommandBuffer cmdbuf);
#else
inline bool init_gui() { return true; }
inline bool create_gui_frame() { return true; }
inline bool send_gui_to_gpu(VkCommandBuffer cmdbuf) { return true; }
#endif
