# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

gui_project_name   = example_gui
nogui_project_name = example

src_files       += example.cpp
gui_src_files   += example_gui.cpp
nogui_src_files += example_nogui.cpp

shader_files += example_pass_through.vert.glsl
shader_files += example_simple.vert.glsl
shader_files += example_rounded_cube.vert.glsl
shader_files += example_phong.frag.glsl
shader_files += example_bezier_surface_cubic.tesc.glsl
shader_files += example_bezier_surface_cubic.tese.glsl
shader_files += example_bezier_surface_quadratic.tesc.glsl
shader_files += example_bezier_surface_quadratic.tese.glsl
