// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include <stdint.h>
#include <utility>

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

template<typename F>
class Deferred {
    public:
        explicit Deferred(F&& func) : func_(std::move(func)) { }
        ~Deferred() { func_(); }

        Deferred(const Deferred&)            = delete;
        Deferred& operator=(const Deferred&) = delete;

    private:
        F func_;
};

struct Deferrer {
    template<typename F>
    Deferred<F> operator=(F&& func) const
    {
        return Deferred<F>(std::forward<F>(func));
    }
};

}

#define DEFER_CONCAT_IMPL(a, b) a##b
#define DEFER_CONCAT(a, b) DEFER_CONCAT_IMPL(a, b)
#define DEFER auto DEFER_CONCAT(defer_, __LINE__) = mstd::Deferrer{} = [&]()
