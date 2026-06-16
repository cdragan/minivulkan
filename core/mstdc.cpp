// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "mstdc.h"
#include <assert.h>
#include <math.h>

uint32_t mstd::strlen(const char* name)
{
    assert(name);

    const char* end = name;

    while (*end)
        ++end;

    return static_cast<uint32_t>(end - name);
}

int mstd::strcmp(const char* s1, const char* s2)
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

float mstd::exp2(float x)
{
    // modff()
    const float integral = truncf(x);
    const float frac     = x - integral;

    constexpr float c0 = 1.0f;
    constexpr float c1 = 0.6931468f;
    constexpr float c2 = 0.2402293f;
    constexpr float c3 = 0.0555039f;

    const float poly = c0 + frac * (c1 + frac * (c2 + frac * c3));

    // ldexpf()
    union {
        float f;
        int   i;
    } convert;

    const int int_integral = static_cast<int>(integral);

    convert.f = poly;
    convert.i += int_integral << 23;

    return convert.f;
}
