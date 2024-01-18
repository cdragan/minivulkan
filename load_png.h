// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "resource.h"

bool load_png_file(const char* filename, Image* image, VkCommandBuffer cmd_buf);
bool load_png(const uint8_t* png, size_t png_size, Image* image, VkCommandBuffer cmd_buf);
