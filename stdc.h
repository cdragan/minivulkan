// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include <stdint.h>

namespace std {

uint32_t strlen(const char* name);

int strcmp(const char* s1, const char* s2);

void mem_zero(void* dest_ptr, uint32_t num_bytes);

void mem_copy(void* dest_ptr, const void* src_ptr, uint32_t num_bytes);

template<typename T, uint32_t N>
constexpr uint32_t array_size(T (&)[N])
{
    return N;
}

template<typename T>
constexpr T min(T a, T b)
{
    return (a < b) ? a : b;
}

template<typename T>
constexpr T max(T a, T b)
{
    return (a > b) ? a : b;
}

}
