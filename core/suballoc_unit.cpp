// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "suballoc.h"
#include "minivulkan.h"
#include <stdio.h>

#define TEST(test) if ( ! (test)) { failed(#test, __FILE__, __LINE__); }

static int exit_code = 0;

static void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n",
            file, line, test);
}

int main()
{
    vk_phys_props.properties.limits.minStorageBufferOffsetAlignment = 256;

    static SubAllocator<2> ator;

    ator.init(1024);

    TEST(ator.allocate(1) == 0);
    TEST(ator.allocate(1) == 256);
    TEST(ator.allocate(1) == 512);
    TEST(ator.allocate(1) == 768);

    // Have two free chunks
    ator.free(768, 256);
    ator.free(0, 256);

    TEST(ator.allocate(1) == 0);

    // Join two free chunks with one in-between
    ator.free(256, 256);
    ator.free(512, 256);

    TEST(ator.allocate(1) == 256);
    TEST(ator.allocate(1) == 512);
    TEST(ator.allocate(1) == 768);

    // Join at the end of each chunk
    ator.free(0, 256);
    ator.free(256, 256);
    ator.free(512, 256);
    ator.free(768, 256);

    TEST(ator.allocate(1) == 0);
    TEST(ator.allocate(1) == 256);
    TEST(ator.allocate(1) == 512);
    TEST(ator.allocate(1) == 768);

    // Join at the beginning of each chunk
    ator.free(768, 256);
    ator.free(512, 256);
    ator.free(256, 256);
    ator.free(0, 256);

    TEST(ator.allocate(1) == 0);
    TEST(ator.allocate(1) == 256);
    TEST(ator.allocate(1) == 512);
    TEST(ator.allocate(1) == 768);

    // Free all at the same time
    ator.init(1024);

    TEST(ator.allocate(1) == 0);
    TEST(ator.allocate(1) == 256);
    TEST(ator.allocate(1) == 512);
    TEST(ator.allocate(1) == 768);

    return exit_code;
}
