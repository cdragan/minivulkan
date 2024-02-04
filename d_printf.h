// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#ifdef NDEBUG
#   define d_printf(...)
#else
#   include <stdio.h>
#   include <string.h>

#   define __STDC_FORMAT_MACROS
#   include <inttypes.h>
#   if defined(_WIN32) && defined(NOSTDLIB) && defined(_M_AMD64)
#       undef PRIx64
#       define PRIx64 "Ix"
#   endif

#   define d_printf printf
#endif
