// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#ifdef NDEBUG
#   define d_printf(...)
#else
#   include <stdio.h>
#   include <string.h>
#   define d_printf printf
#endif
