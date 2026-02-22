// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

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

    const auto allocate = [](size_t size, size_t alignment = 256) -> size_t
    {
        const auto chunk = ator.allocate(size, alignment);
        TEST(chunk.size >= size);
        TEST( ! (chunk.size % alignment));
        return chunk.offset;
    };

    //////////////////////////////////////////////////////////////////////////

    TEST(allocate(1) == 0);
    TEST(allocate(1) == 256);
    TEST(allocate(1) == 512);
    TEST(allocate(1) == 768);

    // Have two free chunks
    ator.free(768, 256);
    ator.free(0, 256);

#ifndef NDEBUG
    TEST(ator.get_used_size() == 512);
#endif

    TEST(allocate(1) == 0);

    // Join two free chunks with one in-between
    ator.free(256, 256);
    ator.free(512, 256);

    TEST(allocate(1) == 256);
    TEST(allocate(1) == 512);
    TEST(allocate(1) == 768);

    // Join at the end of each chunk
    ator.free(0, 256);
    ator.free(256, 256);
    ator.free(512, 256);
    ator.free(768, 256);

#ifndef NDEBUG
    TEST(ator.get_used_size() == 0);
#endif

    //////////////////////////////////////////////////////////////////////////

    TEST(allocate(1) == 0);
    TEST(allocate(1) == 256);
    TEST(allocate(1) == 512);
    TEST(allocate(1) == 768);

    // Join at the beginning of each chunk
    ator.free(768, 256);
    ator.free(512, 256);
    ator.free(256, 256);
    ator.free(0, 256);

#ifndef NDEBUG
    TEST(ator.get_used_size() == 0);
#endif

    //////////////////////////////////////////////////////////////////////////

    TEST(allocate(1) == 0);
    TEST(allocate(1) == 256);
    TEST(allocate(1) == 512);
    TEST(allocate(1) == 768);

    ator.reset(); // Free all at the same time

#ifndef NDEBUG
    TEST(ator.get_used_size() == 0);
    TEST(ator.get_max_used_size() == 1024);
#endif

    //////////////////////////////////////////////////////////////////////////

    TEST(allocate(1) == 0);
    TEST(allocate(1) == 256);
    TEST(allocate(1) == 512);
    TEST(allocate(1) == 768);

    ator.reset(); // Free all at the same time

    //////////////////////////////////////////////////////////////////////////

    // Situation: [free:256] [used:256] [free:512]
    TEST(allocate(1) == 0);
    TEST(allocate(1) == 256);
    ator.free(0, 256);

    // Allocate the second (larger) free chunk
    TEST(allocate(385) == 512);
#ifndef NDEBUG
    TEST(ator.get_used_size() == 768);
#endif

    ator.reset(); // Free all at the same time

    //////////////////////////////////////////////////////////////////////////

    // Situation: [used:128] [free:768] [used:128]
    TEST(allocate(128, 1) == 0);
    TEST(allocate(768, 1) == 128);
    TEST(allocate(128, 1) == 896);
    ator.free(128, 768);

    // Allocate from the end of free block, because beginning does not match alignment
    {
        const auto chunk = ator.allocate(256, 256);
        TEST(chunk.size == 384);
        TEST(chunk.offset == 512);
    }

    // Allocate from the end of free block, because beginning does not match alignment
    {
        const auto chunk = ator.allocate(1, 256);
        TEST(chunk.size == 256);
        TEST(chunk.offset == 256);
    }

    ator.reset(); // Free all at the same time

    //////////////////////////////////////////////////////////////////////////

    // Situation: [used:16] [free:33] [used:15] [free:960]
    TEST(allocate(16, 1) == 0);
    TEST(allocate(33, 1) == 16);
    TEST(allocate(15, 1) == 49);
    ator.free(16, 33);

    // Allocate from next block, the first one can't be used
    TEST(allocate(32, 32) == 64);

    ator.reset(); // Free all at the same time

    return exit_code;
}
