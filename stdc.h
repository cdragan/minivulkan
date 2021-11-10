#include <stdint.h>

namespace std {

uint32_t strlen(const char* name);

int strcmp(const char* s1, const char* s2);

template<typename T, uint32_t N>
constexpr uint32_t array_size(T (&a)[N])
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
