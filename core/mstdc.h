// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

// Workaround windows headers
#ifdef min
#   undef min
#endif

#ifdef max
#   undef max
#endif

namespace mstd {

uint32_t strlen(const char* name);

int strcmp(const char* s1, const char* s2);

void mem_zero(void* dest_ptr, uint32_t num_bytes);

void mem_copy(void* dest_ptr, const void* src_ptr, uint32_t num_bytes);

template<typename T>
constexpr T align_down(T value, T alignment)
{
    return value - (value % alignment);
}

template<typename T>
constexpr T align_up(T value, T alignment)
{
    return ((value - 1) / alignment) * alignment + alignment;
}

float exp2(float x);

}
