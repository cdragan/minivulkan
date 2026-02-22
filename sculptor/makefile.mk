# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

gui_project_name = sculptor

src_files += sculptor.cpp
src_files += sculptor_editor.cpp
src_files += sculptor_geometry.cpp
src_files += sculptor_materials.cpp
src_files += sculptor_geom_edit.cpp

shader_files += sculptor_pass_through.vert.glsl
shader_files += bezier_line_cubic_sculptor.vert.glsl
shader_files += sculptor_simple.vert.glsl
shader_files += bezier_surface_cubic_sculptor.tesc.glsl
shader_files += bezier_surface_cubic_sculptor.tese.glsl
shader_files += sculptor_object.frag.glsl
shader_files += sculptor_g_buffer.frag.glsl
shader_files += sculptor_lighting.vert.glsl
shader_files += sculptor_lighting.frag.glsl
shader_files += sculptor_edge_color.frag.glsl
shader_files += sculptor_color.frag.glsl

shader_files += sculptor_vertex_select.vert.glsl
shader_files += sculptor_vertex_select.frag.glsl

bin_to_header_files += toolbar.png

$(call OBJ_FROM_SRC,sculptor_geom_edit.cpp): $(gen_headers_dir)/toolbar.png.h
