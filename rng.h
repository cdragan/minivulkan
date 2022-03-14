// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include <stdint.h>

class RNG {
    public:
        void init(uint32_t seed);
        void init(uint64_t init_state, uint64_t init_stream);
        uint32_t get_random();

    private:
        uint64_t m_state;
        uint64_t m_stream;
};
