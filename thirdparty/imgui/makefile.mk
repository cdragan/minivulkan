# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

lib_name = imgui

ifeq ($(UNAME), Darwin)
    gui_src_files += src/backends/imgui_impl_osx.mm
endif

ifeq ($(UNAME), Windows)
    gui_src_files += src/backends/imgui_impl_win32.cpp
endif

gui_src_files += src/imgui.cpp
gui_src_files += src/imgui_draw.cpp
gui_src_files += src/imgui_tables.cpp
gui_src_files += src/imgui_widgets.cpp
gui_src_files += src/backends/imgui_impl_vulkan.cpp

ifneq ($(debug), 0)
    gui_src_files += src/imgui_demo.cpp
endif

$(call OBJ_FROM_SRC, $(gui_src_files)): CFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -Ithirdparty/imgui/src
