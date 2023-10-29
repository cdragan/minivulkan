// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "load_png.h"
#include "d_printf.h"
#include "thirdparty/libpng/libpng-1.6.40/png.h"
#include <algorithm>

bool load_png(const char* filename, Image* image)
{
    FILE* const file = fopen(filename, "rb");
    if ( ! file) {
        d_printf("Failed to open file %s\n", filename);
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if ( ! png_ptr)
        return false;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if ( ! info_ptr)
        return false;

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(file);
        d_printf("Failed to read PNG from %s\n", filename);
        return false;
    }

    png_init_io(png_ptr, file);

    constexpr int transforms =
        PNG_TRANSFORM_STRIP_16 |
        PNG_TRANSFORM_PACKING;

    png_read_png(png_ptr, info_ptr, transforms, nullptr);

    uint32_t width            = 0;
    uint32_t height           = 0;
    int      bit_depth        = 0;
    int      color_type       = 0;
    int      interlace_type   = 0;
    int      compression_type = 0;
    int      filter_method    = 0;

    png_get_IHDR(png_ptr, info_ptr,
                 &width, &height, &bit_depth, &color_type, &interlace_type,
                 &compression_type, &filter_method);

    //row_pointers = png_get_rows(png_ptr, info_ptr);

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

    fclose(file);

    return true;
}
