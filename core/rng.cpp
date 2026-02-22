// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "rng.h"

void RNG::init(uint32_t seed)
{
    init(seed, ~(uint64_t)seed);

    union {
        uint64_t u64[2];
        uint32_t u32[4];
    } conv;

    for (int i = 0; i < 4; i++)
        conv.u32[i] = get_random();

    init(conv.u64[0], conv.u64[1]);
}

void RNG::init(uint64_t init_state, uint64_t init_stream)
{
    m_stream = (init_stream << 1) | 1u;
    m_state  = m_stream + init_state;
    get_random();
}

// The PCG XSH RR 32 algorithm by Melissa O'Neill, http://www.pcg-random.org
uint32_t RNG::get_random()
{
    uint32_t xorshifted;
    int      rot;

    const uint64_t state = m_state;

    constexpr uint64_t multiplier = 0x5851'F42D'4C95'7F2Dull;

    m_state = state * multiplier + m_stream;

    xorshifted = static_cast<uint32_t>(((state >> 18U) ^ state) >> 27U);
    rot        = static_cast<int>(state >> 59U);

    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}
