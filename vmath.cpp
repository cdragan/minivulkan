// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vmath.h"
#include "vecfloat.h"
#include "mstdc.h"

using namespace vmath;

namespace {
    static constexpr float small = 1.0f / (1024 * 1024 * 1024);

    struct sin_cos_result4 {
        float4 sin;
        float4 cos;
    };

    sin_cos_result4 sincos(float4 radians)
    {
        sin_cos_result4 result;

        const float4 pi_sq4  = spread4(pi_squared);
        const float4 rad_sq4 = radians * radians;

        // Bhaskara I's approximation
        result.cos = (pi_sq4 - spread4(4) * rad_sq4) / (pi_sq4 + rad_sq4);
        result.sin = sqrt(spread4(1) - result.cos * result.cos);

        return result;
    }

    struct sin_cos_result {
        float sin;
        float cos;
    };

    sin_cos_result sincos(float radians)
    {
        const sin_cos_result4 result4 = sincos(float4{float1{radians}});

        sin_cos_result result;
        result.cos = result4.cos.get0();
        result.sin = result4.sin.get0();

        return result;
    }

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
    const float1 rcp_c  = rcp(float1{c});
    const float4 result = float4::load4_aligned(data) * spread4(rcp_c);
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
    const float4 result = float4::load4_aligned(data) * rcp(float4::load4_aligned(v.data));
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
    const float1 rcp_c  = rcp(float1{c});
    const float4 result = float4::load4_aligned(data) * spread4(rcp_c);
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
    const float4 result = float4::load4_aligned(data) * rcp(float4::load4_aligned(v.data));
    result.store4_aligned(data);
    return *this;
}

bool vec4::operator==(const vec4& v) const
{
    return ! (float4::load4_aligned(data) == float4::load4_aligned(v.data)).movemask();
}

bool vec4::operator!=(const vec4& v) const
{
    return ! (float4::load4_aligned(data) != float4::load4_aligned(v.data)).movemask();
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

quat::quat(const vec3& axis, float angle)
{
    const float4 axis_f4 = float4::load4_aligned(axis.data);
    const float4 sq_len  = dot_product3(axis_f4, axis_f4);
    if (sq_len.get0() > small) {
        const float          rcp_len = rsqrt(float1{sq_len}).get0();
        const sin_cos_result sc_half = sincos(angle * 0.5f);
        const float          scale   = sc_half.sin * rcp_len;
        (axis_f4 * float4{scale, scale, scale, scale}).store4_aligned(data);
        w = sc_half.cos;
    }
}

quat::quat(const vec3& euler_xyz)
{
    const sin_cos_result4 sc_half = sincos(float4::load4_aligned(euler_xyz.data) * spread4(0.5f));
    const float4          sc_xy   = shuffle<0, 1, 0, 2>(sc_half.sin, sc_half.cos);
    const float4          sc_z    = shuffle<2, 2, 2, 2>(sc_half.sin, sc_half.cos);

    float4 dst1 = shuffle<0, 2, 2, 2>(sc_xy, sc_xy);
    dst1       *= shuffle<3, 1, 3, 3>(sc_xy, sc_xy);
    dst1       *= shuffle<2, 2, 0, 2>(sc_z,  sc_z);

    float4 dst2 = shuffle<2, 0, 0, 0>(sc_xy, sc_xy) ^ float4{float4_base::load_mask(1 << 31, 0, 1 << 31, 0)};
    dst2       *= shuffle<1, 3, 1, 1>(sc_xy, sc_xy);
    dst2       *= shuffle<0, 0, 2, 0>(sc_z,  sc_z);

    dst1 += dst2;
    dst1.store4_aligned(data);
}

quat::quat(const mat3& rot_mtx)
{
    // TODO
}

quat::quat(const mat4& rot_mtx)
{
    // TODO
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
    return ! (float4::load4_aligned(data) == float4::load4_aligned(q.data)).movemask();
}

bool quat::operator!=(const quat& q) const
{
    return ! (float4::load4_aligned(data) != float4::load4_aligned(q.data)).movemask();
}

quat quat::operator-() const
{
    return quat{-x, -y, -z, -w};
}

quat conjugate(const quat& q)
{
    return quat{-q.x, -q.y, -q.z, q.w};
}

quat normalize(const quat& q)
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

mat3::mat3(const float* ptr)
{
    mstd::mem_copy(data, ptr, sizeof(data));
}

mat3::mat3(const quat& q)
{
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

mat4::mat4(const mat3& mtx)
{
    mstd::mem_zero(data, sizeof(data));

    mstd::mem_copy(&data[0], &mtx.data[0], sizeof(float) * 3);
    mstd::mem_copy(&data[4], &mtx.data[3], sizeof(float) * 3);
    mstd::mem_copy(&data[8], &mtx.data[6], sizeof(float) * 3);

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

mat4 projection(float aspect, float fov, float near_plane, float far_plane, float depth_bias)
{
    const float fov_tan = tan(radians(fov) * 0.5f);
    const float rrange  = rcp(float1{far_plane - near_plane}).get0();

    mat4 result;

    mstd::mem_zero(result.data, sizeof(result.data));

    result.a00 = rcp(float1{aspect * fov_tan}).get0();
    result.a11 = -rcp(float1{fov_tan}).get0();
    result.a22 = depth_bias - near_plane * rrange;
    result.a32 = (far_plane * near_plane) * rrange;
    result.a23 = 1;

    return result;
}

mat4 operator*(const mat4& m1, const mat4& m2)
{
    mat4 result;

    for (unsigned row_offs = 0; row_offs < 16; ) {

        float4 dst_row = float4::load_zero();

        for (unsigned col_offs = 0; col_offs < 16; col_offs += 4) {
            const float c = m1.data[row_offs++];
            dst_row += float4{c, c, c, c} * float4::load4_aligned(&m2.data[col_offs]);
        }

        dst_row.store4_aligned(&result.data[row_offs]);
    }

    return result;
}

vec4 operator*(const vec4& v, const mat4& mtx)
{
    float4 dst = float4::load_zero();

    for (unsigned row = 0; row < 4; row++) {
        const float c = v.data[row];
        dst += float4{c, c, c, c} * float4::load4_aligned(&mtx.data[row * 4]);
    }

    vec4 result;
    dst.store4_aligned(result.data);
    return result;
}

vec4 operator*(const mat4& mtx, const vec4& v)
{
    float4 dst = float4::load_zero();

    for (unsigned col = 0; col < 4; col++) {
        const float c = v.data[col];
        dst += float4{c, c, c, c} * float4{mtx.data[col],
                                           mtx.data[col + 4],
                                           mtx.data[col + 8],
                                           mtx.data[col + 12]};
    }

    vec4 result;
    dst.store4_aligned(result.data);
    return result;
}

mat4 transpose(const mat4& mtx)
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

mat4 translate(float x, float y, float z)
{
    mat4 result = mat4::identity();
    result.a03 = x;
    result.a13 = y;
    result.a23 = z;
    return result;
}

mat4 scale(float x, float y, float z)
{
    mat4 result = mat4::identity();
    result.a00 = x;
    result.a11 = y;
    result.a22 = z;
    return result;
}
