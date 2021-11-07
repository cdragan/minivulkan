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
