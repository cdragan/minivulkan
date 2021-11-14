// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include "stdc.h"
#include <assert.h>

uint32_t std::strlen(const char* name)
{
    assert(name);

    const char* end = name;

    while (*end)
        ++end;

    return static_cast<uint32_t>(end - name);
}

int std::strcmp(const char* s1, const char* s2)
{
    assert(s1);
    assert(s2);

    uint8_t c1;

    do {
        c1 = static_cast<uint8_t>(*(s1++));
        const uint8_t c2 = static_cast<uint8_t>(*(s2++));

        const int diff = static_cast<int>(c1) - static_cast<int>(c2);

        if (diff)
            return diff;

    } while (c1);

    return 0;
}

void std::mem_zero(void* dest_ptr, uint32_t num_bytes)
{
    assert(dest_ptr);
    assert(num_bytes);

    uint8_t* dest_byte = static_cast<uint8_t*>(dest_ptr);

    while (num_bytes--)
        *(dest_byte++) = 0;
}

void std::mem_copy(void* dest_ptr, const void* src_ptr, uint32_t num_bytes)
{
    assert(dest_ptr);
    assert(src_ptr);
    assert(num_bytes);

    uint8_t*       dest_byte = static_cast<uint8_t*>(dest_ptr);
    const uint8_t* src_byte  = static_cast<const uint8_t*>(src_ptr);

    assert(src_byte + num_bytes <= dest_byte ||
           dest_byte + num_bytes <= src_byte);

    do
        *(dest_byte++) = *(src_byte++);
    while (--num_bytes);
}
