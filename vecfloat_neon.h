// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#if defined(__aarch64__) || defined(_M_ARM64) 

#include <arm_neon.h>
#include <math.h>

namespace vmath {

struct float4_base {
    float32x4_t data;

    float4_base() = default;

    constexpr float4_base(float32x4_t v)
        : data(v) { }

    constexpr operator float32x4_t() const {
        return data;
    }

    static float4_base load_mask(int a, int b, int c, int d) {
        return vreinterpretq_f32_s32(int32x4_t{a, b, c, d});
    }

    bool all() const {
        const int32x4_t as_int = vreinterpretq_s32_f32(data);
        const int32x4_t add1   = vpaddq_s32(as_int, as_int);
        const int32x4_t add2   = vpaddq_s32(add1, add1);
        return vgetq_lane_s32(add2, 0) == -4;
    }

    float get0() const {
        return vgetq_lane_f32(data, 0);
    }

    float get1() const {
        return vgetq_lane_f32(data, 1);
    }

    float get2() const {
        return vgetq_lane_f32(data, 2);
    }

    float get3() const {
        return vgetq_lane_f32(data, 3);
    }

    float operator[](int idx) const {
        switch (idx) {
            case 1:
                return get1();
            case 2:
                return get2();
            case 3:
                return get3();
            default:
                break;
        }
        return get0();
    }

    void store4_aligned(float* ptr) const {
        vst1q_f32(ptr, data);
    }

    void store2(float* ptr) const {
        vst1_f32(ptr, vget_low_f32(data));
    }

    void store3(float* ptr) const {
        vst1_f32(ptr, vget_low_f32(data));
        ptr[2] = vgetq_lane_f32(data, 2);
    }

    void stream4(float* ptr) const {
        vst1q_f32(ptr, data);
    }
};

struct float1 {
    float data;

    float1() = default;

    explicit float1(float32x4_t v)
        : data(vgetq_lane_f32(v, 0)) { }

    constexpr float1(float v)
        : data(v) { }

    float get0() const {
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
    float value = vrecpes_f32(v.data);
    value *= vrecpss_f32(value, v.data);
    value *= vrecpss_f32(value, v.data);
    return float1{value};
}

inline float1 sqrt(const float1& v)
{
    return float1{sqrtf(v.data)};
}

inline float1 rsqrt(const float1& v)
{
    float value = vrsqrtes_f32(v.data);
    value *= vrsqrtss_f32(value * v.data, value);
    value *= vrsqrtss_f32(value * v.data, value);
    return float1{value};
}

inline float1 abs(const float1& v)
{
    return float1{fabs(v.data)};
}

inline float1 min(const float1& v1, const float1& v2)
{
    return float1{fmin(v1.data, v2.data)};
}

inline float1 max(const float1& v1, const float1& v2)
{
    return float1{fmax(v1.data, v2.data)};
}

struct float4: public float4_base {
    float4() = default;

    constexpr explicit float4(float32x4_t v)
        : float4_base(v) { }

    constexpr explicit float4(const float1& v)
        : float4_base(float32x4_t{v.data, 0, 0, 0}) { }

    constexpr float4(float a, float b, float c, float d)
        : float4_base(float32x4_t{a, b, c, d}) { }

    static float4 load_zero() {
        return float4{float32x4_t{0, 0, 0, 0}};
    }

    static float4 load2(const float* ptr) {
        return float4{float32x4_t{ptr[0], ptr[1], 0, 0}};
    }

    static float4 load4_aligned(const float* ptr) {
        return float4{vld1q_f32(ptr)};
    }

    static constexpr float4 load4(const float* ptr) {
        return float4{float32x4_t{ptr[0], ptr[1], ptr[2], ptr[3]}};
    }

    float4& operator+=(const float4& v) {
        data = vaddq_f32(data, v.data);
        return *this;
    }

    float4& operator-=(const float4& v) {
        data = vsubq_f32(data, v.data);
        return *this;
    }

    float4& operator*=(const float4& v) {
        data = vmulq_f32(data, v.data);
        return *this;
    }

    float4& operator/=(const float4& v) {
        data = vdivq_f32(data, v.data);
        return *this;
    }

    float4& operator&=(const float4& v) {
        data = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(data), vreinterpretq_u32_f32(v)));
        return *this;
    }

    float4& operator|=(const float4& v) {
        data = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(data), vreinterpretq_u32_f32(v)));
        return *this;
    }

    float4& operator^=(const float4& v) {
        data = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(data), vreinterpretq_u32_f32(v)));
        return *this;
    }
};

inline float4 spread4(const float1& v)
{
    return float4{vmovq_n_f32(v.data)};
}

inline float4 spread4(float v)
{
    return float4{vmovq_n_f32(v)};
}

inline float4 operator+(float4 v1, const float4& v2)
{
    return float4{vaddq_f32(v1, v2)};
}

inline float4 operator-(float4 v1, const float4& v2)
{
    return float4{vsubq_f32(v1, v2)};
}

inline float4 operator*(float4 v1, const float4& v2)
{
    return float4{vmulq_f32(v1, v2)};
}

inline float4 operator/(float4 v1, const float4& v2)
{
    return float4{vdivq_f32(v1, v2)};
}

inline float4 operator&(float4 v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v1), vreinterpretq_u32_f32(v2)))};
}

inline float4 operator|(float4 v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(v1), vreinterpretq_u32_f32(v2)))};
}

inline float4 operator^(float4 v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(v1), vreinterpretq_u32_f32(v2)))};
}

inline float4 operator==(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vceqq_f32(v1, v2))};
}

inline float4 operator!=(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vmvnq_u32(vceqq_f32(v1, v2)))};
}

inline float4 operator<(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vcltq_f32(v1, v2))};
}

inline float4 operator<=(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vcleq_f32(v1, v2))};
}

inline float4 operator>(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vcgtq_f32(v1, v2))};
}

inline float4 operator>=(const float4& v1, const float4& v2)
{
    return float4{vreinterpretq_f32_u32(vcgeq_f32(v1, v2))};
}

inline float4 rcp(const float4& v)
{
    float32x4_t value = vrecpeq_f32(v);
    value = vmulq_f32(value, vrecpsq_f32(value, v));
    value = vmulq_f32(value, vrecpsq_f32(value, v));
    return float4{value};
}

inline float4 sqrt(const float4& v)
{
    return float4{vsqrtq_f32(v)};
}

inline float4 rsqrt(const float4& v)
{
    float32x4_t value = vrsqrteq_f32(v);
    value = vmulq_f32(value, vrsqrtsq_f32(vmulq_f32(value, v.data), value));
    value = vmulq_f32(value, vrsqrtsq_f32(vmulq_f32(value, v.data), value));
    return float4{value};
}

inline float4 abs(const float4& v)
{
    return float4{vabsq_f32(v)};
}

inline float4 min(const float4& v1, const float4& v2)
{
    return float4{vminnmq_f32(v1, v2)};
}

inline float4 max(const float4& v1, const float4& v2)
{
    return float4{vmaxnmq_f32(v1, v2)};
}

inline float4 floor(const float4& v)
{
    return float4{vrndmq_f32(v)};
}

inline float4 ceil(const float4& v)
{
    return float4{vrndpq_f32(v)};
}

inline float4 unpacklo(const float4& v1, const float4& v2)
{
    return float4{vzip1q_f32(v1, v2)};
}

inline float4 unpackhi(const float4& v1, const float4& v2)
{
    return float4{vzip2q_f32(v1, v2)};
}

inline float4 movelh(const float4& v1, const float4& v2)
{
    const float32x4_t a2a3a0a1 = vextq_f32(v1, v1, 2);
    return float4{vextq_f32(a2a3a0a1, v2, 2)};
}

inline float4 movehl(const float4& v1, const float4& v2)
{
    const float32x4_t a2a3a0a1 = vextq_f32(v1, v1, 2);
    return float4{vextq_f32(v2, a2a3a0a1, 2)};
}

inline float4 hadd(const float4& v1, const float4& v2)
{
    return float4{vpaddq_f32(v1, v2)};
}

inline void transpose(float4& a, float4& b, float4& c, float4& d)
{
    const float32x4_t a0b0a2b2 = vtrn1q_f32(a, b);
    const float32x4_t a1b1a3b3 = vtrn2q_f32(a, b);
    const float32x4_t c0d0c2d2 = vtrn1q_f32(c, d);
    const float32x4_t c1d1c3d3 = vtrn2q_f32(c, d);

    const float32x4_t a2b2a0b0 = vextq_f32(a0b0a2b2, a0b0a2b2, 2);
    a = float4{vextq_f32(a2b2a0b0, c0d0c2d2, 2)};

    const float32x4_t a3b3a1b1 = vextq_f32(a1b1a3b3, a1b1a3b3, 2);
    b = float4{vextq_f32(a3b3a1b1, c1d1c3d3, 2)};

    const float32x4_t c2d2c0d0 = vextq_f32(c0d0c2d2, c0d0c2d2, 2);
    c = float4{vextq_f32(a0b0a2b2, c2d2c0d0, 2)};

    const float32x4_t c3d3c1d1 = vextq_f32(c1d1c3d3, c1d1c3d3, 2);
    d = float4{vextq_f32(a1b1a3b3, c3d3c1d1, 2)};
}

inline float4 dot_product3(const float4& v1, const float4& v2)
{
    const float32x4_t mult = vsetq_lane_f32(0, vmulq_f32(v1, v2), 3);
    const float32x4_t add1 = vpaddq_f32(mult, mult);
    return float4{vpaddq_f32(add1, add1)};
}

inline float4 dot_product4(const float4& v1, const float4& v2)
{
    const float32x4_t mult = vmulq_f32(v1, v2);
    const float32x4_t add1 = vpaddq_f32(mult, mult);
    return float4{vpaddq_f32(add1, add1)};
}

} // namespace vmath

#endif
