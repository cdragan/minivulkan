// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include "vmath.h"
#include "vecfloat.h"
#include "stdc.h"

using namespace vmath;

vec2& vec2::operator=(const float4_base& v)
{
    x = v.get0();
    y = v.get1();
    return *this;
}

vec3& vec3::operator=(const float4_base& v)
{
    x = v.get0();
    y = v.get1();
    z = v.get2();
    return *this;
}

vec3& vec3::operator+=(const vec3& v)
{
    *this = float4{x, y, z, 0} + float4{v.x, v.y, v.z, 0};
    return *this;
}

vec3& vec3::operator-=(const vec3& v)
{
    *this = float4{x, y, z, 0} - float4{v.x, v.y, v.z, 0};
    return *this;
}

vec3& vec3::operator*=(const float c)
{
    x *= c;
    y *= c;
    z *= c;
    return *this;
}

vec3& vec3::operator/=(const float c)
{
    x /= c;
    y /= c;
    z /= c;
    return *this;
}

vec3& vec3::operator*=(const vec3& v)
{
    *this = float4{x, y, z, 0} * float4{v.x, v.y, v.z, 0};
    return *this;
}

vec3& vec3::operator/=(const vec3& v)
{
    x /= v.x;
    y /= v.y;
    z /= v.z;
    return *this;
}

vec4& vec4::operator=(const float4_base& v)
{
    v.store4_aligned(data);
    return *this;
}

vec4& vec4::operator+=(const vec4& v)
{
    *this = float4::load4_aligned(data) + float4::load4_aligned(v.data);
    return *this;
}

vec4& vec4::operator-=(const vec4& v)
{
    *this = float4::load4_aligned(data) - float4::load4_aligned(v.data);
    return *this;
}

vec4& vec4::operator*=(const float c)
{
    *this = float4::load4_aligned(data) * float4{c, c, c, c};
    return *this;
}

vec4& vec4::operator/=(const float c)
{
    *this = float4::load4_aligned(data) / float4{c, c, c, c};
    return *this;
}

vec4& vec4::operator*=(const vec4& v)
{
    *this = float4::load4_aligned(data) * float4::load4_aligned(v.data);
    return *this;
}

vec4& vec4::operator/=(const vec4& v)
{
    *this = float4::load4_aligned(data) / float4::load4_aligned(v.data);
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
    return vec4{-x, -y, -z, -w};
}

float vec4::sum_components() const
{
    const float4 a = float4::load4_aligned(data);
    const float4 b = hadd(a, a);
    const float4 c = hadd(b, b);
    return c.get0();
}

template<unsigned dim>
float vmath::dot(const vec<dim>& v1, const vec<dim>& v2)
{
    const vec<dim> mult = v1 * v2;
    return mult.sum_components();
}

template float vmath::dot(const vec<2>&, const vec<2>&);
template float vmath::dot(const vec<3>&, const vec<3>&);
template float vmath::dot(const vec<4>&, const vec<4>&);

mat4::mat4(const mat3& mtx)
{
    std::mem_zero(&data, sizeof(data));

    std::mem_copy(&data,    &mtx.data[0], sizeof(float) * 3);
    std::mem_copy(&data[4], &mtx.data[3], sizeof(float) * 3);
    std::mem_copy(&data[8], &mtx.data[6], sizeof(float) * 3);

    a33 = 1;
}

mat4::mat4(const float* ptr)
{
    std::mem_copy(&data, ptr, sizeof(data));
}

mat4 operator*(const mat4& m1, const mat4& m2)
{
    mat4 result;

    for (unsigned row_offs = 0; row_offs < 16; ) {

        float4 dst_row{0, 0, 0, 0};

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
    float4 dst{0, 0, 0, 0};

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
    float4 dst{0, 0, 0, 0};

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
