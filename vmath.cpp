// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include "vmath.h"
#include "vecfloat.h"
#include "mstdc.h"
#include <cmath>

using namespace vmath;

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
    const float  rc     = rcp(float1{c}).get0();
    const float4 result = float4::load4_aligned(data) * float4{rc, rc, rc, rc};
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
    const float  rc     = rcp(float1{c}).get0();
    const float4 result = float4::load4_aligned(data) * float4{rc, rc, rc, rc};
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
    // TODO
}

quat::quat(const vec3& euler_xyz)
{
    vec3 r{euler_xyz.x, euler_xyz.y, euler_xyz.z};
    r *= 0.5f;
    const vec3 cosr(std::cos(r.x), std::cos(r.y), std::cos(r.z));
    const vec3 sinr(std::sin(r.x), std::sin(r.y), std::sin(r.z));

    float4 dst1 = float4{sinr.x, cosr.x, cosr.x, cosr.x};
    dst1       *= float4{cosr.y, sinr.y, cosr.y, cosr.y};
    dst1       *= float4{cosr.z, cosr.z, sinr.z, cosr.z};

    float4 dst2 = float4{-cosr.x, sinr.x, -sinr.x, sinr.x};
    dst2       *= float4{ sinr.y, cosr.y,  sinr.y, sinr.y};
    dst2       *= float4{ sinr.z, sinr.z,  cosr.z, sinr.z};

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
        const float rlen = rsqrt(float1{dp}).get0();
        fq *= float4{rlen, rlen, rlen, rlen};
    }

    quat result;
    fq.store4_aligned(result.data);
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
    // TODO
}

mat4 mat4::identity()
{
    mat4 result;

    mstd::mem_zero(result.data, sizeof(result.data));

    result.a00 = 1;
    result.a11 = 1;
    result.a22 = 1;
    result.a33 = 1;

    return result;
}

mat4 projection(float aspect, float fov, float near_plane, float far_plane, float depth_bias)
{
    const float fov_tan = std::tan(radians(fov) * 0.5f);
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
