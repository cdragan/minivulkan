# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2023 Chris Dragan

lib_name = libpng

libpng_src_files += png.c
libpng_src_files += pngerror.c
libpng_src_files += pngget.c
libpng_src_files += pngmem.c
libpng_src_files += pngpread.c
libpng_src_files += pngread.c
libpng_src_files += pngrio.c
libpng_src_files += pngrtran.c
libpng_src_files += pngrutil.c
libpng_src_files += pngset.c
libpng_src_files += pngtrans.c
libpng_src_files += pngwio.c
libpng_src_files += pngwrite.c
libpng_src_files += pngwtran.c
libpng_src_files += pngwutil.c

src_dir := libpng-1.6.40

gui_src_files += $(addprefix $(src_dir)/,$(libpng_src_files))

$(call OBJ_FROM_SRC,$(gui_src_files)): CFLAGS += -Ithirdparty/libpng -Ithirdparty/zlib/zlib-1.3
