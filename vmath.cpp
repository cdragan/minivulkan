// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vmath.h"
#include "vecfloat.h"
#include "mstdc.h"
#include <assert.h>

using namespace vmath;

namespace {
    static constexpr float small = 1.0f / (1024 * 1024 * 1024);
}

vmath::sin_cos_result4 vmath::sincos(float4 radians)
{
    const float4 sign_mask = float4{float4::load_mask(1 << 31, 1 << 31, 1 << 31, 1 << 31)};

    // Remove sign, because cos(x) == cos(-x) (for sin(x) we use sin_sign)
    const float4 abs_radians = abs(radians);

    // Function has period of two_pi, so divide modulo two_pi
    const float4 two_pi4          = spread4(two_pi);
    const float4 int_div          = floor(abs_radians / two_pi4) * two_pi4;
    const float4 radians_0_two_pi = abs_radians - int_div;

    // cos(x) == cos(two_pi - x), so put it in range [0..pi]
    const float4 radians_0_pi = min(radians_0_two_pi, two_pi4 - radians_0_two_pi);

    // Remember original sign, needed for flipping sin(x) when x<0, because sin(x) == -sin(-x)
    // The sign on second half of sine wave must also be flipped, because sin(x) == -sin(pi - x)
    const float4 sin_sign = (radians ^ (radians_0_pi != radians_0_two_pi)) & sign_mask;

    // Bhaskara I's approximation breaks down at x > pi/2
    // cos(x) == -cos(pi - x), so put it in range [0..pi/2] and remember the sign flip
    const float4 radians_0_pi_half = min(radians_0_pi, spread4(pi) - radians_0_pi);
    const float4 cos_sign = (radians_0_pi != radians_0_pi_half) & sign_mask;

    const float4 pi_sq4  = spread4(pi_squared);
    const float4 rad_sq4 = radians_0_pi_half * radians_0_pi_half;

    // Bhaskara I's approximation
    sin_cos_result4 result;
    result.cos = ((pi_sq4 - spread4(4) * rad_sq4) / (pi_sq4 + rad_sq4)) ^ cos_sign;
    result.sin = sqrt(spread4(1) - result.cos * result.cos) ^ sin_sign;

    return result;
}

vmath::sin_cos_result vmath::sincos(float radians)
{
    const sin_cos_result4 result4 = sincos(float4{float1{radians}});

    sin_cos_result result;
    result.cos = result4.cos.get0();
    result.sin = result4.sin.get0();

    return result;
}

namespace vmath {
    float tan(float radians)
    {
        // From Bhaskara I's approximation:
        // cos(x) = (pi * pi - 4 * x * x) / (pi * pi + x * x)
        //
        // sin(x) = sqrt(1 - cos(x) * cos(x))
        // tan(x) = sin(x) / cos(x)
        // tan(x) * tan(x) = (1 - cos(x) * cos(x)) / (cos(x) * cos(x))
        // tan(x) * tan(x) = (1 / (cos(x) * cos(x))) - 1
        // tan(x) * tan(x) = rcp(sqr(cos(x))) - 1
        // tan(x) * tan(x) = sqr((pi * pi + x * x) / (pi * pi - 4 * x * x)) - 1
        const float sq_x = radians * radians;
        const float rcp_cos = (pi_squared + sq_x) / (pi_squared - 4 * sq_x);
        return sqrt(float1{rcp_cos * rcp_cos - 1}).get0();
    }
}

vec3& vec3::operator+=(const vec3& v)
{
    const float4 result = float4::load4_aligned(data) + float4::load4_aligned(v.data);
    result.store3(data);
    return *this;
}

vec3& vec3::operator-=(const vec3& v)
{
    const float4 result = float4::load4_aligned(data) - float4::load4_aligned(v.data);
    result.store3(data);
    return *this;
}

vec3& vec3::operator*=(const float c)
{
    const float4 result = float4::load4_aligned(data) * float4{c, c, c, c};
    result.store3(data);
    return *this;
}

vec3& vec3::operator/=(const float c)
{
    const float4 result = float4::load4_aligned(data) / spread4(c);
    result.store3(data);
    return *this;
}

vec3& vec3::operator*=(const vec3& v)
{
    const float4 result = float4::load4_aligned(data) * float4::load4_aligned(v.data);
    result.store3(data);
    return *this;
}

vec3& vec3::operator/=(const vec3& v)
{
    const float4 result = float4::load4_aligned(data) / float4::load4_aligned(v.data);
    result.store3(data);
    return *this;
}

vec4& vec4::operator+=(const vec4& v)
{
    const float4 result = float4::load4_aligned(data) + float4::load4_aligned(v.data);
    result.store4_aligned(data);
    return *this;
}

vec4& vec4::operator-=(const vec4& v)
{
    const float4 result = float4::load4_aligned(data) - float4::load4_aligned(v.data);
    result.store4_aligned(data);
    return *this;
}

vec4& vec4::operator*=(const float c)
{
    const float4 result = float4::load4_aligned(data) * float4{c, c, c, c};
    result.store4_aligned(data);
    return *this;
}

vec4& vec4::operator/=(const float c)
{
    const float4 result = float4::load4_aligned(data) / spread4(c);
    result.store4_aligned(data);
    return *this;
}

vec4& vec4::operator*=(const vec4& v)
{
    const float4 result = float4::load4_aligned(data) * float4::load4_aligned(v.data);
    result.store4_aligned(data);
    return *this;
}

vec4& vec4::operator/=(const vec4& v)
{
    const float4 result = float4::load4_aligned(data) / float4::load4_aligned(v.data);
    result.store4_aligned(data);
    return *this;
}

bool vec4::operator==(const vec4& v) const
{
    return (float4::load4_aligned(data) == float4::load4_aligned(v.data)).all();
}

bool vec4::operator!=(const vec4& v) const
{
    return ! (float4::load4_aligned(data) == float4::load4_aligned(v.data)).all();
}

vec4 vec4::operator-() const
{
    vec4 result;
    (float4::load_zero() - float4::load4_aligned(data)).store4_aligned(result.data);
    return result;
}

template<>
float vmath::dot_product<2>(const vec<2>& v1, const vec<2>& v2)
{
    const float4 multiplied = float4::load2(v1.data) * float4::load2(v2.data);
    return hadd(multiplied, multiplied).get0();
}

template<>
float vmath::dot_product<3>(const vec<3>& v1, const vec<3>& v2)
{
    return dot_product3(float4::load4_aligned(v1.data), float4::load4_aligned(v2.data)).get0();
}

template<>
float vmath::dot_product<4>(const vec<4>& v1, const vec<4>& v2)
{
    return dot_product4(float4::load4_aligned(v1.data), float4::load4_aligned(v2.data)).get0();
}

vec3 vmath::cross_product(const vec3& v1, const vec3& v2)
{
    const vec3 a{v1.y, v1.z, v1.x};
    const vec3 b{v2.z, v2.x, v2.y};
    const vec3 c{v1.z, v1.x, v1.y};
    const vec3 d{v2.y, v2.z, v2.x};

    return a * b - c * d;
}

template<unsigned dim>
float vmath::length(const vec<dim>& v)
{
    const float dp = dot_product(v, v);
    return (dp > small) ? sqrt(float1{dp}).get0() : 0.0f;
}

template float vmath::length<2>(const vec<2>&);
template float vmath::length<3>(const vec<3>&);
template float vmath::length<4>(const vec<4>&);

template<unsigned dim>
float vmath::rlength(const vec<dim>& v)
{
    const float dp = dot_product(v, v);
    return (dp > small) ? rsqrt(float1{dp}).get0() : 0.0f;
}

template float vmath::rlength<2>(const vec<2>&);
template float vmath::rlength<3>(const vec<3>&);
template float vmath::rlength<4>(const vec<4>&);

template<unsigned dim>
vec<dim> vmath::normalize(const vec<dim>& v)
{
    return v * rlength(v);
}

template vec<2> vmath::normalize(const vec<2>&);
template vec<3> vmath::normalize(const vec<3>&);
template vec<4> vmath::normalize(const vec<4>&);

quat::quat(const vec3& axis, float angle_radians)
{
    const float4 axis_f4 = float4::load4_aligned(axis.data);
    const float4 sq_len  = dot_product3(axis_f4, axis_f4);
    if (sq_len.get0() > small) {
        const float          rcp_len = rsqrt(float1{sq_len}).get0();
        const sin_cos_result sc_half = sincos(angle_radians * 0.5f);
        const float          scale   = sc_half.sin * rcp_len;
        (axis_f4 * float4{scale, scale, scale, scale}).store4_aligned(data);
        w = sc_half.cos;
    }
}

quat::quat(const vec3& euler_xyz)
{
    const sin_cos_result4 sc_half = sincos(float4::load4_aligned(euler_xyz.data) * spread4(0.5f));

#ifdef USE_SSE
    const float4 sc_xy = shuffle<0, 1, 0, 1>(sc_half.sin, sc_half.cos); // [ sinx, siny, cosx, cosy ]
    const float4 sc_z  = shuffle<2, 2, 2, 2>(sc_half.sin, sc_half.cos); // [ sinz, sinz, cosz, cosz ]

    const float4 cx = shuffle<0, 2, 2, 2>(sc_xy, sc_xy);
    const float4 cy = shuffle<3, 1, 3, 3>(sc_xy, sc_xy);
    const float4 cz = shuffle<2, 2, 0, 2>(sc_z,  sc_z);

    const float4 sx = shuffle<2, 0, 0, 0>(sc_xy, sc_xy);
    const float4 sy = shuffle<1, 3, 1, 1>(sc_xy, sc_xy);
    const float4 sz = shuffle<0, 0, 2, 0>(sc_z,  sc_z);
#else
    const float4 cx{sc_half.sin[0], sc_half.cos[0], sc_half.cos[0], sc_half.cos[0]};
    const float4 cy{sc_half.cos[1], sc_half.sin[1], sc_half.cos[1], sc_half.cos[1]};
    const float4 cz{sc_half.cos[2], sc_half.cos[2], sc_half.sin[2], sc_half.cos[2]};
    const float4 sx{sc_half.cos[0], sc_half.sin[0], sc_half.sin[0], sc_half.sin[0]};
    const float4 sy{sc_half.sin[1], sc_half.cos[1], sc_half.sin[1], sc_half.sin[1]};
    const float4 sz{sc_half.sin[2], sc_half.sin[2], sc_half.cos[2], sc_half.sin[2]};
#endif

    const float4 result = cx * cy * cz + ((sx * sy * sz) ^ float4{float4_base::load_mask(1 << 31, 0, 1 << 31, 0)});

    result.store4_aligned(data);
}

quat::quat(const mat3& rot_mtx)
{
    // TODO
    assert(0);
}

quat::quat(const mat4& rot_mtx)
{
    // TODO
    assert(0);
}

quat& quat::operator*=(const quat& q)
{
    float4 result = float4{w, w, w, w}  * float4::load4_aligned(q.data);
    result       += float4{x, y, z, -x} * float4{q.w, q.w, q.w, q.x};
    result       += float4{y, z, x, -y} * float4{q.z, q.x, q.y, q.y};
    result       -= float4{z, x, y, z}  * float4{q.y, q.z, q.x, q.z};

    result.store4_aligned(data);

    return *this;
}

bool quat::operator==(const quat& q) const
{
    return (float4::load4_aligned(data) == float4::load4_aligned(q.data)).all();
}

bool quat::operator!=(const quat& q) const
{
    return ! (float4::load4_aligned(data) == float4::load4_aligned(q.data)).all();
}

quat quat::operator-() const
{
    const float4 result = float4::load4_aligned(data) ^ float4{float4_base::load_mask(1 << 31, 1 << 31, 1 << 31, 1 << 31)};
    quat new_q;
    result.store4_aligned(new_q.data);
    return new_q;
}

quat vmath::conjugate(const quat& q)
{
    const float4 result = float4::load4_aligned(q.data) ^ float4{float4_base::load_mask(1 << 31, 1 << 31, 1 << 31, 0)};
    quat new_q;
    result.store4_aligned(new_q.data);
    return new_q;
}

quat vmath::normalize(const quat& q)
{
    float4       fq  = float4::load4_aligned(q.data);
    const float4 dp  = dot_product4(fq, fq);

    if (dp.get0() > 0) {
        const float1 rcp_len = rsqrt(float1{dp});
        fq *= spread4(rcp_len);
    }

    quat result;
    fq.store4_aligned(result.data);
    return result;
}

vec3 quat::rotate(const vec3& v) const
{
    const quat result = *this * vmath::quat{v.x, v.y, v.z, 0} * conjugate(*this);

    return vec3{result.data};
}

mat3::mat3(const mat4& mtx)
{
    mstd::mem_copy(&data[0], &mtx.data[0], sizeof(data));
}

mat3::mat3(const float* ptr)
{
    mstd::mem_copy(data, ptr, sizeof(data));
}

mat3::mat3(const quat& q)
{
    // TODO this is incorrect, it assumed mat3x3, but the layout has changed to mat3x4 (3 columns, 4 rows)
    assert(0);
    const float4 q2f = float4::load4_aligned(q.data) * float4{2, 2, 2, 2};

    quat q2w;
    (q2f * float4{q.w, q.w, q.w, q.w}).store4_aligned(q2w.data);

    quat q2x;
    (q2f * float4{q.x, q.x, q.x, q.x}).store4_aligned(q2x.data);

    quat q2y;
    (q2f * float4{q.y, q.y, q.y, q.y}).store4_aligned(q2y.data);

    const float z2z = q2f.get2() * q.z;

    (float4{1, q2x.y, q2x.z, q2x.y} - float4{q2y.y + z2z, q2w.z, -q2w.y, -q2w.z}).store4_aligned(&data[0]);
    (float4{1, q2y.z, q2x.z, q2y.z} - float4{q2x.x + z2z, q2w.x, q2w.y, -q2w.x}).store4_aligned(&data[4]);
    data[8] = 1 - q2x.x + q2y.y;
}

void mat3::set_identity()
{
    mstd::mem_zero(data, sizeof(data));

    a00 = 1;
    a11 = 1;
    a22 = 1;
}

mat3 mat3::identity()
{
    mat3 result;
    result.set_identity();
    return result;
}

mat3 vmath::transpose(const mat3& mtx)
{
    float4 row[4] = {
        float4::load4_aligned(&mtx.data[0]),
        float4::load4_aligned(&mtx.data[4]),
        float4::load4_aligned(&mtx.data[8]),
        float4::load_zero()
    };

    transpose(row[0], row[1], row[2], row[3]);

    mat3 result;
    row[0].store4_aligned(&result.data[0]);
    row[1].store4_aligned(&result.data[4]);
    row[2].store4_aligned(&result.data[8]);
    return result;
}

mat3 vmath::inverse(const mat3& mtx)
{
    mat3 result;
    mstd::mem_zero(&result, sizeof(result));

    result.a00 =  (mtx.a11 * mtx.a22 - mtx.a21 * mtx.a12);
    result.a10 = -(mtx.a10 * mtx.a22 - mtx.a20 * mtx.a12);
    result.a20 =  (mtx.a10 * mtx.a21 - mtx.a20 * mtx.a11);
    result.a01 = -(mtx.a01 * mtx.a22 - mtx.a21 * mtx.a02);
    result.a11 =  (mtx.a00 * mtx.a22 - mtx.a20 * mtx.a02);
    result.a21 = -(mtx.a00 * mtx.a21 - mtx.a20 * mtx.a01);
    result.a02 =  (mtx.a01 * mtx.a12 - mtx.a11 * mtx.a02);
    result.a12 = -(mtx.a00 * mtx.a12 - mtx.a10 * mtx.a02);
    result.a22 =  (mtx.a00 * mtx.a11 - mtx.a10 * mtx.a01);

    const float det = (mtx.a00 * result.a00) + (mtx.a01 * result.a10) + (mtx.a02 * result.a20);

    const float rdet = 1.0f / det;

    result.a00 *= rdet; result.a01 *= rdet; result.a02 *= rdet;
    result.a10 *= rdet; result.a11 *= rdet; result.a12 *= rdet;
    result.a20 *= rdet; result.a21 *= rdet; result.a22 *= rdet;
    return result;

}

mat4::mat4(const mat3& mtx)
{
    mstd::mem_zero(data, sizeof(data));

    mstd::mem_copy(data, mtx.data, sizeof(data));

    a33 = 1;
}

mat4::mat4(const float* ptr)
{
    mstd::mem_copy(data, ptr, sizeof(data));
}

mat4::mat4(const quat& q)
{
    const float4 q2f = float4::load4_aligned(q.data) * float4{2, 2, 2, 2};

    quat q2w;
    (q2f * float4{q.w, q.w, q.w, q.w}).store4_aligned(q2w.data);

    quat q2x;
    (q2f * float4{q.x, q.x, q.x, q.x}).store4_aligned(q2x.data);

    quat q2y;
    (q2f * float4{q.y, q.y, q.y, q.y}).store4_aligned(q2y.data);

    const float z2z = q2f.get2() * q.z;

    (float4{1, q2x.y, q2x.z, 0} - float4{q2y.y + z2z, q2w.z, -q2w.y, 0}).store4_aligned(&data[0]);
    (float4{q2x.y, 1, q2y.z, 0} - float4{-q2w.z, q2x.x + z2z, q2w.x, 0}).store4_aligned(&data[4]);
    (float4{q2x.z, q2y.z, 1, 0} - float4{q2w.y, -q2w.x, q2x.x + q2y.y, 0}).store4_aligned(&data[8]);
    float4{0, 0, 0, 1}.store4_aligned(&data[12]);
}

void mat4::set_identity()
{
    mstd::mem_zero(data, sizeof(data));

    a00 = 1;
    a11 = 1;
    a22 = 1;
    a33 = 1;
}

mat4 mat4::identity()
{
    mat4 result;
    result.set_identity();
    return result;
}

mat4 vmath::projection(float aspect, float fov_radians, float near_plane, float far_plane, float depth_bias)
{
    const float fov_tan = vmath::tan(fov_radians * 0.5f);
    const float rrange  = rcp(float1{far_plane - near_plane}).get0();

    mat4 result;

    mstd::mem_zero(result.data, sizeof(result.data));

    result.a00 = rcp(float1{aspect * fov_tan}).get0();
    result.a11 = rcp(float1{fov_tan}).get0();
    result.a22 = depth_bias - near_plane * rrange;
    result.a32 = (far_plane * near_plane) * rrange;
    result.a23 = 1;

    return result;
}

vec4 vmath::projection_vector(float aspect, float fov_radians, float near_plane, float far_plane, float depth_bias)
{
    const float fov_tan = vmath::tan(fov_radians * 0.5f);
    const float rrange  = rcp(float1{far_plane - near_plane}).get0();

    vec4 result;

    result.x = rcp(float1{aspect * fov_tan}).get0();
    result.y = rcp(float1{fov_tan}).get0();
    result.z = depth_bias - near_plane * rrange;
    result.w = (far_plane * near_plane) * rrange;

    return result;
}

mat4 vmath::look_at(const vec3& eye_pos, const vec3& target)
{
    constexpr vec3 up{0, 1, 0};
    const vec3 z_axis = normalize(target - eye_pos);
    const vec3 x_axis = normalize(cross_product(up, z_axis));
    const vec3 y_axis = cross_product(z_axis, x_axis);

    mat4 result;

    mstd::mem_zero(result.data, sizeof(result.data));
    mstd::mem_copy(&result.data[0], &x_axis.data[0], sizeof(x_axis.data));
    mstd::mem_copy(&result.data[4], &y_axis.data[0], sizeof(y_axis.data));
    mstd::mem_copy(&result.data[8], &z_axis.data[0], sizeof(z_axis.data));

    result.a30 = -dot_product(x_axis, eye_pos);
    result.a31 = -dot_product(y_axis, eye_pos);
    result.a32 = -dot_product(z_axis, eye_pos);
    result.a33 = 1.0f;

    return result;
}

mat4 vmath::operator*(const mat4& m1, const mat4& m2)
{
    mat4 result;

    for (unsigned row_offs = 0; row_offs < 16; ) {

        float* const dest = &result.data[row_offs];

        float4 dst_row = float4::load_zero();

        for (unsigned col_offs = 0; col_offs < 16; col_offs += 4) {
            const float c = m2.data[row_offs++];
            dst_row += float4{c, c, c, c} * float4::load4_aligned(&m1.data[col_offs]);
        }

        dst_row.store4_aligned(dest);
    }

    return result;
}

vec4 vmath::operator*(const vec4& v, const mat4& mtx)
{
    float4 dst = float4::load_zero();

    for (unsigned row = 0; row < 4; row++) {
        const float c = v.data[row];
        dst += float4{c, c, c, c} * float4{mtx.data[row],
                                           mtx.data[row + 4],
                                           mtx.data[row + 8],
                                           mtx.data[row + 12]};
    }

    vec4 result;
    dst.store4_aligned(result.data);
    return result;
}

vec4 vmath::operator*(const mat4& mtx, const vec4& v)
{
    float4 dst = float4::load_zero();

    for (unsigned col = 0; col < 4; col++) {
        const float c = v.data[col];
        dst += float4{c, c, c, c} * float4::load4_aligned(&mtx.data[col * 4]);
    }

    vec4 result;
    dst.store4_aligned(result.data);
    return result;
}

mat4 vmath::transpose(const mat4& mtx)
{
    float4 row[4] = {
        float4::load4_aligned(&mtx.data[0]),
        float4::load4_aligned(&mtx.data[4]),
        float4::load4_aligned(&mtx.data[8]),
        float4::load4_aligned(&mtx.data[12])
    };

    transpose(row[0], row[1], row[2], row[3]);

    mat4 result;
    row[0].store4_aligned(&result.data[0]);
    row[1].store4_aligned(&result.data[4]);
    row[2].store4_aligned(&result.data[8]);
    row[3].store4_aligned(&result.data[12]);
    return result;
}

mat4 vmath::translate(float x, float y, float z)
{
    mat4 result = mat4::identity();
    result.a30 = x;
    result.a31 = y;
    result.a32 = z;
    return result;
}

mat4 vmath::scale(float x, float y, float z)
{
    mat4 result = mat4::identity();
    result.a00 = x;
    result.a11 = y;
    result.a22 = z;
    return result;
}
