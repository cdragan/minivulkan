// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#if defined(__riscv)

#include <cmath>
#include <cstdint>

namespace vmath {

struct float4;

struct float1 {
    float data;

    float1() = default;

    constexpr float1(float v)
        : data(v) { }

    constexpr float1(const float4& v);

    constexpr float get0() const {
        return data;
    }

    float1& operator+=(const float1& v) {
        data += v.data;
        return *this;
    }

    float1& operator-=(const float1& v) {
        data -= v.data;
        return *this;
    }

    float1& operator*=(const float1& v) {
        data *= v.data;
        return *this;
    }

    float1& operator/=(const float1& v) {
        data /= v.data;
        return *this;
    }

    static constexpr float get_all1s() {
        constexpr unsigned int u = ~0U;
        return *reinterpret_cast<const float*>(&u);
    }
};

inline float1 operator+(float1 v1, const float1& v2)
{
    return float1{v1.data + v2.data};
}

inline float1 operator-(float1 v1, const float1& v2)
{
    return float1{v1.data - v2.data};
}

inline float1 operator*(float1 v1, const float1& v2)
{
    return float1{v1.data * v2.data};
}

inline float1 operator/(float1 v1, const float1& v2)
{
    return float1{v1.data / v2.data};
}

inline float1 operator==(const float1& v1, const float1& v2)
{
    return (v1.data == v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 operator!=(const float1& v1, const float1& v2)
{
    return (v1.data != v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 operator<(const float1& v1, const float1& v2)
{
    return (v1.data < v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 operator<=(const float1& v1, const float1& v2)
{
    return (v1.data <= v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 operator>(const float1& v1, const float1& v2)
{
    return (v1.data > v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 operator>=(const float1& v1, const float1& v2)
{
    return (v1.data >= v2.data) ? float1{float1::get_all1s()} : float1{0};
}

inline float1 rcp(const float1& v)
{
    return float1{1.0f / v.data};
}

inline float1 sqrt(const float1& v)
{
    return float1{sqrtf(v.data)};
}

inline float1 rsqrt(const float1& v)
{
    return float1{1.0f / sqrtf(v.data)};
}

inline float1 abs(const float1& v)
{
    return float1{std::abs(v.data)};
}

inline float1 min(const float1& v1, const float1& v2)
{
    return float1{std::min(v1.data, v2.data)};
}

inline float1 max(const float1& v1, const float1& v2)
{
    return float1{std::max(v1.data, v2.data)};
}

struct float4 {
    float data[4];

    float4() = default;

    constexpr float4(float1 v)
        : data{v.get0(), 0, 0, 0} { }

    constexpr float4(float a, float b, float c, float d)
        : data{a, b, c, d} { }

    static constexpr float4 load_zero() {
        return float4{0, 0, 0, 0};
    }

    static constexpr float4 load2(const float* ptr) {
        return float4{ptr[0], ptr[1], 0, 0};
    }

    static constexpr float4 load4_aligned(const float* ptr) {
        return load4(ptr);
    }

    static constexpr float4 load4(const float* ptr) {
        return float4{ptr[0], ptr[1], ptr[2], ptr[3]};
    }

    static constexpr float4 load_mask(int a, int b, int c, int d) {
        const auto as_float = [](int32_t v) -> float {
            return *reinterpret_cast<float*>(&v);
        };
        return float4{
            as_float(a),
            as_float(b),
            as_float(c),
            as_float(d)
        };
    }

    constexpr bool all() const {
        const uint32_t sign_bits = as_uint32(data[0]) &
                                   as_uint32(data[1]) &
                                   as_uint32(data[2]) &
                                   as_uint32(data[3]);
        return !! (sign_bits & 0x80000000U);
    }

    constexpr bool any() const {
        const uint32_t sign_bits = as_uint32(data[0]) |
                                   as_uint32(data[1]) |
                                   as_uint32(data[2]) |
                                   as_uint32(data[3]);
        return !! (sign_bits & 0x80000000U);
    }

    constexpr float get0() const {
        return data[0];
    }

    constexpr float get1() const {
        return data[1];
    }

    constexpr float get2() const {
        return data[2];
    }

    constexpr float get3() const {
        return data[3];
    }

    constexpr float operator[](int idx) const {
        return data[idx];
    }

    constexpr void store4_aligned(float* ptr) const {
        store4(ptr);
    }

    constexpr void store2(float* ptr) const {
        ptr[0] = data[0];
        ptr[1] = data[1];
    }

    constexpr void store3(float* ptr) const {
        ptr[0] = data[0];
        ptr[1] = data[1];
        ptr[2] = data[2];
    }

    constexpr void store4(float* ptr) const {
        ptr[0] = data[0];
        ptr[1] = data[1];
        ptr[2] = data[2];
        ptr[3] = data[3];
    }

    constexpr void stream4(float* ptr) const {
        store4(ptr);
    }

    constexpr float4& operator+=(const float4& v) {
        for (int i = 0; i < 4; i++)
            data[i] += v.data[i];
        return *this;
    }

    constexpr float4& operator-=(const float4& v) {
        for (int i = 0; i < 4; i++)
            data[i] -= v.data[i];
        return *this;
    }

    constexpr float4& operator*=(const float4& v) {
        for (int i = 0; i < 4; i++)
            data[i] *= v.data[i];
        return *this;
    }

    constexpr float4& operator/=(const float4& v) {
        for (int i = 0; i < 4; i++)
            data[i] /= v.data[i];
        return *this;
    }

    constexpr float4& operator&=(const float4& v) {
        for (int i = 0; i < 4; i++)
            as_uint32_writable(data[i]) = as_uint32(data[i]) & as_uint32(v.data[i]);
        return *this;
    }

    constexpr float4& operator|=(const float4& v) {
        for (int i = 0; i < 4; i++)
            as_uint32_writable(data[i]) = as_uint32(data[i]) | as_uint32(v.data[i]);
        return *this;
    }

    constexpr float4& operator^=(const float4& v) {
        for (int i = 0; i < 4; i++)
            as_uint32_writable(data[i]) = as_uint32(data[i]) ^ as_uint32(v.data[i]);
        return *this;
    }

private:
    static uint32_t& as_uint32_writable(float& v) {
        return *reinterpret_cast<uint32_t*>(&v);
    }
    static constexpr uint32_t as_uint32(float v) {
        return *reinterpret_cast<uint32_t*>(&v);
    }
};

inline constexpr float1::float1(const float4& v)
: data(v.data[0])
{
}

inline constexpr float4 spread4(float1 v)
{
    return float4{v.get0(), v.get0(), v.get0(), v.get0()};
}

inline constexpr float4 operator+(float4 v1, const float4& v2)
{
    v1 += v2;
    return v1;
}

inline constexpr float4 operator-(float4 v1, const float4& v2)
{
    v1 -= v2;
    return v1;
}

inline constexpr float4 operator*(float4 v1, const float4& v2)
{
    v1 *= v2;
    return v1;
}

inline constexpr float4 operator/(float4 v1, const float4& v2)
{
    v1 /= v2;
    return v1;
}

inline constexpr float4 operator&(float4 v1, const float4& v2)
{
    v1 &= v2;
    return v1;
}

inline constexpr float4 operator|(float4 v1, const float4& v2)
{
    v1 |= v2;
    return v1;
}

inline constexpr float4 operator^(float4 v1, const float4& v2)
{
    v1 ^= v2;
    return v1;
}

inline constexpr float4 operator==(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] == v2.data[0]) ? -1 : 0,
        (v1.data[1] == v2.data[1]) ? -1 : 0,
        (v1.data[2] == v2.data[2]) ? -1 : 0,
        (v1.data[3] == v2.data[3]) ? -1 : 0
    );
}

inline constexpr float4 operator!=(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] != v2.data[0]) ? -1 : 0,
        (v1.data[1] != v2.data[1]) ? -1 : 0,
        (v1.data[2] != v2.data[2]) ? -1 : 0,
        (v1.data[3] != v2.data[3]) ? -1 : 0
    );
}

inline constexpr bool equal(const float4& v1, const float4& v2)
{
    return (v1.data[0] == v2.data[0]) &&
           (v1.data[1] == v2.data[1]) &&
           (v1.data[2] == v2.data[2]) &&
           (v1.data[3] == v2.data[3]);
}

inline constexpr bool not_equal(const float4& v1, const float4& v2)
{
    return (v1.data[0] != v2.data[0]) ||
           (v1.data[1] != v2.data[1]) ||
           (v1.data[2] != v2.data[2]) ||
           (v1.data[3] != v2.data[3]);
}

inline constexpr float4 operator<(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] < v2.data[0]) ? -1 : 0,
        (v1.data[1] < v2.data[1]) ? -1 : 0,
        (v1.data[2] < v2.data[2]) ? -1 : 0,
        (v1.data[3] < v2.data[3]) ? -1 : 0
    );
}

inline constexpr float4 operator<=(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] <= v2.data[0]) ? -1 : 0,
        (v1.data[1] <= v2.data[1]) ? -1 : 0,
        (v1.data[2] <= v2.data[2]) ? -1 : 0,
        (v1.data[3] <= v2.data[3]) ? -1 : 0
    );
}

inline constexpr float4 operator>(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] > v2.data[0]) ? -1 : 0,
        (v1.data[1] > v2.data[1]) ? -1 : 0,
        (v1.data[2] > v2.data[2]) ? -1 : 0,
        (v1.data[3] > v2.data[3]) ? -1 : 0
    );
}

inline constexpr float4 operator>=(const float4& v1, const float4& v2)
{
    return float4::load_mask(
        (v1.data[0] >= v2.data[0]) ? -1 : 0,
        (v1.data[1] >= v2.data[1]) ? -1 : 0,
        (v1.data[2] >= v2.data[2]) ? -1 : 0,
        (v1.data[3] >= v2.data[3]) ? -1 : 0
    );
}

inline constexpr float4 rcp(const float4& v)
{
    return float4{
        1.0f / v.data[0],
        1.0f / v.data[1],
        1.0f / v.data[2],
        1.0f / v.data[3]
    };
}

inline constexpr float4 sqrt(const float4& v)
{
    return float4{
        std::sqrt(v.data[0]),
        std::sqrt(v.data[1]),
        std::sqrt(v.data[2]),
        std::sqrt(v.data[3])
    };
}

inline constexpr float4 rsqrt(const float4& v)
{
    return float4{
        1.0f / std::sqrt(v.data[0]),
        1.0f / std::sqrt(v.data[1]),
        1.0f / std::sqrt(v.data[2]),
        1.0f / std::sqrt(v.data[3])
    };
}

inline constexpr float4 abs(const float4& v)
{
    return float4{
        std::abs(v.data[0]),
        std::abs(v.data[1]),
        std::abs(v.data[2]),
        std::abs(v.data[3])
    };
}

inline constexpr float4 min(const float4& v1, const float4& v2)
{
    return float4{
        std::min(v1.data[0], v2.data[0]),
        std::min(v1.data[1], v2.data[1]),
        std::min(v1.data[2], v2.data[2]),
        std::min(v1.data[3], v2.data[3])
    };
}

inline constexpr float4 max(const float4& v1, const float4& v2)
{
    return float4{
        std::max(v1.data[0], v2.data[0]),
        std::max(v1.data[1], v2.data[1]),
        std::max(v1.data[2], v2.data[2]),
        std::max(v1.data[3], v2.data[3])
    };
}

inline constexpr float4 floor(const float4& v)
{
    return float4{
        floorf(v.data[0]),
        floorf(v.data[1]),
        floorf(v.data[2]),
        floorf(v.data[3])
    };
}

inline constexpr float4 ceil(const float4& v)
{
    return float4{
        ceilf(v.data[0]),
        ceilf(v.data[1]),
        ceilf(v.data[2]),
        ceilf(v.data[3])
    };
}

inline constexpr float4 hadd(const float4& v1, const float4& v2)
{
    return float4{
        v1.data[0] + v1.data[1],
        v1.data[2] + v1.data[3],
        v2.data[0] + v2.data[1],
        v2.data[2] + v2.data[3]
    };
}

inline constexpr void transpose(float4& a, float4& b, float4& c, float4& d)
{
    float v = a.data[1];
    a.data[1] = b.data[0];
    b.data[0] = v;

    v = a.data[2];
    a.data[2] = c.data[0];
    c.data[0] = v;

    v = a.data[3];
    a.data[3] = d.data[0];
    d.data[0] = v;

    v = b.data[2];
    b.data[2] = c.data[1];
    c.data[1] = v;

    v = b.data[3];
    b.data[3] = d.data[1];
    d.data[1] = v;

    v = c.data[3];
    c.data[3] = d.data[2];
    d.data[2] = v;
}

inline constexpr float4 dot_product3(const float4& v1, const float4& v2)
{
    const float v = v1.data[0] * v2.data[0] +
                    v1.data[1] * v2.data[1] +
                    v1.data[2] * v2.data[2];
    return float4{v, v, v, v};
}

inline constexpr float4 dot_product4(const float4& v1, const float4& v2)
{
    const float v = v1.data[0] * v2.data[0] +
                    v1.data[1] * v2.data[1] +
                    v1.data[2] * v2.data[2] +
                    v1.data[3] * v2.data[3];
    return float4{v, v, v, v};
}

} // namespace vmath

#endif
