# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

gui_project_name   = example_gui
nogui_project_name = example

all_example_gui_src_files += $(imgui_src_files)

src_files       += example.cpp
gui_src_files   += example_gui.cpp
nogui_src_files += example_nogui.cpp
