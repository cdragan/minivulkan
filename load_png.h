// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "resource.h"

bool load_png_file(const char*     filename,
                   Image*          image,
                   uint32_t*       width,
                   uint32_t*       height,
                   VkCommandBuffer cmd_buf);

bool load_png(const uint8_t*  png,
              size_t          png_size,
              Image*          image,
              uint32_t*       width_ptr,
              uint32_t*       height_ptr,
              VkCommandBuffer cmd_buf);
