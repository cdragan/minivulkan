# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

lib_name = imgui

ifeq ($(UNAME), Darwin)
    src_files += src/backends/imgui_impl_osx.mm
endif

ifeq ($(UNAME), Windows)
    src_files += src/backends/imgui_impl_win32.cpp
endif

src_files += src/imgui.cpp
src_files += src/imgui_draw.cpp
src_files += src/imgui_tables.cpp
src_files += src/imgui_widgets.cpp
src_files += src/backends/imgui_impl_vulkan.cpp

ifneq ($(debug), 0)
    src_files += src/imgui_demo.cpp
endif

$(call OBJ_FROM_SRC, $(src_files)): CFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -Ithirdparty/imgui/src
