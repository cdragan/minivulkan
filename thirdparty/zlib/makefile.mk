# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

lib_name = zlib

src_dir := zlib-1.3

gui_src_files += $(src_dir)/adler32.c
gui_src_files += $(src_dir)/crc32.c
gui_src_files += $(src_dir)/deflate.c
gui_src_files += $(src_dir)/inffast.c
gui_src_files += $(src_dir)/inflate.c
gui_src_files += $(src_dir)/inftrees.c
gui_src_files += $(src_dir)/trees.c
gui_src_files += $(src_dir)/zutil.c
