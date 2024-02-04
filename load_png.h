// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "resource.h"

bool load_png_file(const char*        filename,
                   ImageWithHostCopy* image);

bool load_png(const uint8_t*     png,
              size_t             png_size,
              ImageWithHostCopy* image);
