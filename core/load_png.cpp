// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "load_png.h"
#include "d_printf.h"
#include "mstdc.h"

#include "../thirdparty/libpng/libpng-1.6.40/png.h"

#include <algorithm>
#include <string.h>

static bool read_png_into_image(png_structp        png_ptr,
                                png_infop          info_ptr,
                                ImageWithHostCopy* image)
{
    constexpr int transforms =
        PNG_TRANSFORM_STRIP_16 |
        PNG_TRANSFORM_PACKING  |
        PNG_TRANSFORM_EXPAND;

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

    const png_bytepp row_pointers = png_get_rows(png_ptr, info_ptr);
    if ( ! row_pointers)
        return false;

    static ImageInfo image_info = {
        0,
        0,
        VK_FORMAT_R8G8B8A8_UNORM,
        1,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        Usage::fixed
    };

    image_info.width  = width;
    image_info.height = height;

    if ( ! image->allocate(image_info, "png image"))
        return false;

    Image& host_image = image->get_host_image();

    uint8_t* host_ptr = host_image.get_ptr<uint8_t>();

    for (uint32_t i = 0; i < height; i++, host_ptr += host_image.get_pitch())
        mstd::mem_copy(host_ptr, row_pointers[i], width * 4);

    return true;
}

bool load_png_file(const char*        filename,
                   ImageWithHostCopy* image)
{
    FILE* const file = fopen(filename, "rb");
    if ( ! file) {
        d_printf("Failed to open file %s\n", filename);
        return false;
    }

    png_structp png_ptr  = nullptr;
    png_infop   info_ptr = nullptr;

    // It would be nice to have a defer statement instead of this boilerplate...
    class destroy {
        FILE*       file;
        png_structp png_ptr;
        png_infop   info_ptr;

        public:
            destroy(FILE* in_file, png_structp in_png_ptr, png_infop in_info_ptr)
                : file(in_file), png_ptr(in_png_ptr), info_ptr(in_info_ptr) { }
            ~destroy() {
                png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
                fclose(file);
            }
    };
    destroy on_return(file, png_ptr, info_ptr);

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if ( ! png_ptr)
        return false;

    info_ptr = png_create_info_struct(png_ptr);
    if ( ! info_ptr)
        return false;

    if (setjmp(png_jmpbuf(png_ptr))) {
        d_printf("Failed to read PNG from %s\n", filename);
        return false;
    }

    png_init_io(png_ptr, file);

    return read_png_into_image(png_ptr, info_ptr, image);
}

struct PngInputData {
    const uint8_t* bytes;
    size_t         size_left;
};

static void read_png_from_memory(png_structp png_ptr,
                                 png_bytep   out_bytes,
                                 png_size_t  bytes_to_read)
{
    const png_voidp io_ptr = png_get_io_ptr(png_ptr);

    if (io_ptr) {
        PngInputData* input_data = (PngInputData*)io_ptr;

        const size_t read_size = std::min(bytes_to_read, input_data->size_left);

        memcpy(out_bytes, input_data->bytes, read_size);

        input_data->bytes     += read_size;
        input_data->size_left -= read_size;
    }
}

bool load_png(const uint8_t*     png,
              size_t             png_size,
              ImageWithHostCopy* image)
{
    png_structp png_ptr  = nullptr;
    png_infop   info_ptr = nullptr;

    // It would be nice to have a defer statement instead of this boilerplate...
    class destroy {
        png_structp png_ptr;
        png_infop   info_ptr;

        public:
            destroy(png_structp in_png_ptr, png_infop in_info_ptr)
                : png_ptr(in_png_ptr), info_ptr(in_info_ptr) { }
            ~destroy() {
                png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
            }
    };
    destroy on_return(png_ptr, info_ptr);

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if ( ! png_ptr)
        return false;

    info_ptr = png_create_info_struct(png_ptr);
    if ( ! info_ptr)
        return false;

    if (setjmp(png_jmpbuf(png_ptr))) {
        d_printf("Failed to read PNG\n");
        return false;
    }

    PngInputData input_data = { png, png_size };
    png_set_read_fn(png_ptr, &input_data, read_png_from_memory);

    return read_png_into_image(png_ptr, info_ptr, image);
}
