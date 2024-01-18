# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

gui_project_name = sculptor

src_files += sculptor.cpp
src_files += sculptor_editor.cpp
src_files += sculptor_geometry.cpp
src_files += sculptor_materials.cpp
src_files += sculptor_geom_edit.cpp

bin_to_header_files += toolbar.png

$(call OBJ_FROM_SRC,sculptor_geom_edit.cpp): $(gen_headers_dir)/toolbar.png.h
