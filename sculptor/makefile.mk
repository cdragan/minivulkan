# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

gui_project_name = sculptor

all_sculptor_src_files += $(libpng_src_files)
all_sculptor_src_files += $(zlib_src_files)

src_files += sculptor.cpp
src_files += sculptor_geometry.cpp
src_files += sculptor_materials.cpp
src_files += sculptor_geom_edit.cpp
