# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

lib_name = zlib

src_dir := zlib-1.3

src_files += $(src_dir)/adler32.c
src_files += $(src_dir)/crc32.c
src_files += $(src_dir)/deflate.c
src_files += $(src_dir)/inffast.c
src_files += $(src_dir)/inflate.c
src_files += $(src_dir)/inftrees.c
src_files += $(src_dir)/trees.c
src_files += $(src_dir)/zutil.c
