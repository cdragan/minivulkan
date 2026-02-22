// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#if defined(__x86_64__) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)

#include <immintrin.h>
#define USE_SSE

namespace vmath {

struct float4_base {
    __m128 data;

    float4_base() = default;

    constexpr float4_base(__m128 v)
        : data(v) { }

    constexpr operator __m128() const {
        return data;
    }

    static float4_base load_mask(int a, int b, int c, int d) {
        return _mm_castsi128_ps(_mm_set_epi32(d, c, b, a));
    }

    bool all() const {
        return _mm_movemask_ps(data) == 0b1111;
    }

    bool any() const {
        return _mm_movemask_ps(data) != 0;
    }

    float get0() const {
        float value;
        _mm_store_ss(&value, data);
        return value;
    }

    float get1() const {
        float value;
        _mm_store_ss(&value, _mm_shuffle_ps(data, data, 1));
        return value;
    }

    float get2() const {
        float value;
        _mm_store_ss(&value, _mm_shuffle_ps(data, data, 2));
        return value;
    }

    float get3() const {
        float value;
        _mm_store_ss(&value, _mm_shuffle_ps(data, data, 3));
        return value;
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
        _mm_store_ps(ptr, data);
    }

    void store2(float* ptr) const {
        _mm_store_sd(reinterpret_cast<double*>(ptr), _mm_castps_pd(data));
    }

    void store3(float* ptr) const {
        _mm_store_sd(reinterpret_cast<double*>(ptr), _mm_castps_pd(data));
        _mm_store_ss(ptr + 2, _mm_shuffle_ps(data, data, 2));
    }

    void store4(float* ptr) const {
        _mm_storeu_ps(ptr, data);
    }

    void stream4(float* ptr) const {
        _mm_stream_ps(ptr, data);
    }
};

struct float1: public float4_base {
    float1() = default;

    constexpr explicit float1(__m128 v)
        : float4_base(v) { }

    constexpr float1(float v)
        : float4_base(__m128{v, 0, 0, 0}) { }

    float1& operator+=(const float1& v) {
        data = _mm_add_ss(*this, v);
        return *this;
    }

    float1& operator-=(const float1& v) {
        data = _mm_sub_ss(*this, v);
        return *this;
    }

    float1& operator*=(const float1& v) {
        data = _mm_mul_ss(*this, v);
        return *this;
    }

    float1& operator/=(const float1& v) {
        data = _mm_div_ss(*this, v);
        return *this;
    }
};

inline float1 operator+(float1 v1, const float1& v2)
{
    v1 += v2;
    return v1;
}

inline float1 operator-(float1 v1, const float1& v2)
{
    v1 -= v2;
    return v1;
}

inline float1 operator*(float1 v1, const float1& v2)
{
    v1 *= v2;
    return v1;
}

inline float1 operator/(float1 v1, const float1& v2)
{
    v1 /= v2;
    return v1;
}

inline float1 operator==(const float1& v1, const float1& v2)
{
    return float1{_mm_cmpeq_ss(v1, v2)};
}

inline float1 operator!=(const float1& v1, const float1& v2)
{
    return float1{_mm_cmpneq_ss(v1, v2)};
}

inline float1 operator<(const float1& v1, const float1& v2)
{
    return float1{_mm_cmplt_ss(v1, v2)};
}

inline float1 operator<=(const float1& v1, const float1& v2)
{
    return float1{_mm_cmple_ss(v1, v2)};
}

inline float1 operator>(const float1& v1, const float1& v2)
{
    return float1{_mm_cmpgt_ss(v1, v2)};
}

inline float1 operator>=(const float1& v1, const float1& v2)
{
    return float1{_mm_cmpge_ss(v1, v2)};
}

inline float1 rcp(const float1& v)
{
    return float1{_mm_rcp_ss(v)};
}

inline float1 sqrt(const float1& v)
{
    return float1{_mm_sqrt_ss(v)};
}

inline float1 rsqrt(const float1& v)
{
    return float1{_mm_rsqrt_ss(v)};
}

inline float1 abs(const float1& v)
{
    return float1{_mm_andnot_ps(float4_base::load_mask(1 << 31, 0, 0, 0), v)};
}

inline float1 min(const float1& v1, const float1& v2)
{
    return float1{_mm_min_ss(v1, v2)};
}

inline float1 max(const float1& v1, const float1& v2)
{
    return float1{_mm_max_ss(v1, v2)};
}

struct float4: public float4_base {
    float4() = default;

    constexpr explicit float4(__m128 v)
        : float4_base(v) { }

    constexpr float4(float a, float b, float c, float d)
        : float4_base(__m128{a, b, c, d}) { }

    static float4 load_zero() {
        const __m128 some = _mm_undefined_ps();
        return float4{_mm_xor_ps(some, some)};
    }

    static float4 load2(const float* ptr) {
        return float4{_mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(ptr)))};
    }

    static float4 load4_aligned(const float* ptr) {
        return float4{_mm_load_ps(ptr)};
    }

    static constexpr float4 load4(const float* ptr) {
        return float4{__m128{ptr[0], ptr[1], ptr[2], ptr[3]}};
    }

    float4& operator+=(const float4& v) {
        data = _mm_add_ps(*this, v);
        return *this;
    }

    float4& operator-=(const float4& v) {
        data = _mm_sub_ps(*this, v);
        return *this;
    }

    float4& operator*=(const float4& v) {
        data = _mm_mul_ps(*this, v);
        return *this;
    }

    float4& operator/=(const float4& v) {
        data = _mm_div_ps(*this, v);
        return *this;
    }

    float4& operator&=(const float4& v) {
        data = _mm_and_ps(*this, v);
        return *this;
    }

    float4& operator|=(const float4& v) {
        data = _mm_or_ps(*this, v);
        return *this;
    }

    float4& operator^=(const float4& v) {
        data = _mm_xor_ps(*this, v);
        return *this;
    }
};

inline float4 spread4(const float1& v)
{
    return float4{_mm_shuffle_ps(v, v, 0)};
}

inline float4 spread4(float v)
{
    const float1 fv{v};
    return float4{_mm_shuffle_ps(fv, fv, 0)};
}

inline float4 operator+(float4 v1, const float4& v2)
{
    v1 += v2;
    return v1;
}

inline float4 operator-(float4 v1, const float4& v2)
{
    v1 -= v2;
    return v1;
}

inline float4 operator*(float4 v1, const float4& v2)
{
    v1 *= v2;
    return v1;
}

inline float4 operator/(float4 v1, const float4& v2)
{
    v1 /= v2;
    return v1;
}

inline float4 operator&(float4 v1, const float4& v2)
{
    v1 &= v2;
    return v1;
}

inline float4 operator|(float4 v1, const float4& v2)
{
    v1 |= v2;
    return v1;
}

inline float4 operator^(float4 v1, const float4& v2)
{
    v1 ^= v2;
    return v1;
}

inline float4 operator==(const float4& v1, const float4& v2)
{
    return float4{_mm_cmpeq_ps(v1, v2)};
}

inline float4 operator!=(const float4& v1, const float4& v2)
{
    return float4{_mm_cmpneq_ps(v1, v2)};
}

inline bool equal(const float4& v1, const float4& v2)
{
    return (v1 == v2).all();
}

inline bool not_equal(const float4& v1, const float4& v2)
{
    return (v1 != v2).any();
}

inline float4 operator<(const float4& v1, const float4& v2)
{
    return float4{_mm_cmplt_ps(v1, v2)};
}

inline float4 operator<=(const float4& v1, const float4& v2)
{
    return float4{_mm_cmple_ps(v1, v2)};
}

inline float4 operator>(const float4& v1, const float4& v2)
{
    return float4{_mm_cmpgt_ps(v1, v2)};
}

inline float4 operator>=(const float4& v1, const float4& v2)
{
    return float4{_mm_cmpge_ps(v1, v2)};
}

template<int lo_x, int lo_y, int hi_z, int hi_w>
inline float4 shuffle(const float4& v_lo, const float4& v_hi)
{
    constexpr int mask = (lo_x & 3) | ((lo_y & 3) << 2) | ((hi_z & 3) << 4) | ((hi_w & 3) << 6);
    return float4{_mm_shuffle_ps(v_lo, v_hi, mask)};
}

inline float4 andnot(const float4& v1, const float4& v2)
{
    return float4{_mm_andnot_ps(v1, v2)};
}

inline float4 rcp(const float4& v)
{
    return float4{_mm_rcp_ps(v)};
}

inline float4 sqrt(const float4& v)
{
    return float4{_mm_sqrt_ps(v)};
}

inline float4 rsqrt(const float4& v)
{
    return float4{_mm_rsqrt_ps(v)};
}

inline float4 abs(const float4& v)
{
    return float4{_mm_andnot_ps(float4_base::load_mask(1 << 31, 1 << 31, 1 << 31, 1 << 31), v)};
}

inline float4 min(const float4& v1, const float4& v2)
{
    return float4{_mm_min_ps(v1, v2)};
}

inline float4 max(const float4& v1, const float4& v2)
{
    return float4{_mm_max_ps(v1, v2)};
}

inline float4 floor(const float4& v)
{
    return float4{_mm_floor_ps(v)};
}

inline float4 ceil(const float4& v)
{
    return float4{_mm_ceil_ps(v)};
}

inline float4 unpacklo(const float4& v1, const float4& v2)
{
    return float4{_mm_unpacklo_ps(v1, v2)};
}

inline float4 unpackhi(const float4& v1, const float4& v2)
{
    return float4{_mm_unpackhi_ps(v1, v2)};
}

inline float4 movelh(const float4& v1, const float4& v2)
{
    return float4{_mm_movelh_ps(v1, v2)};
}

inline float4 movehl(const float4& v1, const float4& v2)
{
    return float4{_mm_movehl_ps(v1, v2)};
}

inline float4 hadd(const float4& v1, const float4& v2)
{
    return float4{_mm_hadd_ps(v1, v2)};
}

inline void transpose(float4& a, float4& b, float4& c, float4& d)
{
    _MM_TRANSPOSE4_PS(a.data, b.data, c.data, d.data);
}

inline float4 dot_product3(const float4& v1, const float4& v2)
{
    return float4{_mm_dp_ps(v1, v2, 0x7F)};
}

inline float4 dot_product4(const float4& v1, const float4& v2)
{
    return float4{_mm_dp_ps(v1, v2, 0xFF)};
}

} // namespace vmath

#endif
