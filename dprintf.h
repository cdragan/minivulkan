// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#ifdef NDEBUG
#   define dprintf(...)
#else
#   include <stdio.h>
#   include <string.h>
#   define dprintf printf
#endif
