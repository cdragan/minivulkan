// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

namespace vmath {

struct float4_base;

constexpr float pi = 3.14159265f;

template<unsigned dim> struct vec;
struct quat;
struct mat3;
struct mat4;

typedef struct vec<2> vec2;
typedef struct vec<3> vec3;
typedef struct vec<4> vec4;

template<>
struct vec<2> {
    union {
        struct {
            alignas(2 * sizeof(float))
            float x;
            float y;
        };
        alignas(2 * sizeof(float)) float data[2];
    };

    vec() = default;

    constexpr explicit vec(float x)
        : x(x), y(x) { }

    constexpr vec(float x, float y)
        : x(x), y(y) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]) { }

    constexpr explicit vec(const vec3& v);

    constexpr explicit vec(const vec4& v);

    vec& operator=(const float4_base& v);

    constexpr float& operator[](unsigned i) {
        return data[i];
    }

    constexpr const float& operator[](unsigned i) const {
        return data[i];
    }

    constexpr bool operator==(const vec& v) const {
        return (x == v.x) && (y == v.y);
    }

    constexpr bool operator!=(const vec& v) const {
        return (x != v.x) || (y != v.y);
    }

    constexpr vec operator-() const {
        return vec(-x, -y);
    }

    constexpr float sum_components() const {
        return x + y;
    }

    constexpr vec& operator+=(const vec& v) {
        x += v.x;
        y += v.y;
        return *this;
    }

    constexpr vec& operator-=(const vec& v) {
        x -= v.x;
        y -= v.y;
        return *this;
    }

    constexpr vec& operator*=(float c) {
        x *= c;
        y *= c;
        return *this;
    }

    constexpr vec& operator/=(float c) {
        x /= c;
        y /= c;
        return *this;
    }

    constexpr vec& operator*=(const vec& v) {
        x *= v.x;
        y *= v.y;
        return *this;
    }

    constexpr vec& operator/=(const vec& v) {
        x /= v.x;
        y /= v.y;
        return *this;
    }
};

template<>
struct vec<3> {
    union {
        struct {
            alignas(4 * sizeof(float))
            float x;
            float y;
            float z;
        };
        alignas(4 * sizeof(float)) float data[3];
    };

    vec() = default;

    constexpr explicit vec(float x)
        : x(x), y(x), z(x) { }

    constexpr vec(float x, float y, float z)
        : x(x), y(y), z(z) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]) { }

    constexpr explicit vec(const vec2& v)
        : x(v.x), y(v.y), z(1) { }

    constexpr explicit vec(const vec2& v, float z)
        : x(v.x), y(v.y), z(z) { }

    constexpr explicit vec(const vec4& v);

    vec& operator=(const float4_base& v);

    constexpr float& operator[](unsigned i) {
        return data[i];
    }

    constexpr const float& operator[](unsigned i) const {
        return data[i];
    }

    constexpr bool operator==(const vec& v) const {
        return (x == v.x) && (y == v.y) && (z == v.z);
    }

    constexpr bool operator!=(const vec& v) const {
        return (x != v.x) || (y != v.y) || (z != v.z);
    }

    constexpr vec operator-() const {
        return vec(-x, -y, -z);
    }

    constexpr float sum_components() const {
        return x + y + z;
    }

    vec& operator+=(const vec& v);
    vec& operator-=(const vec& v);
    vec& operator*=(const float c);
    vec& operator/=(const float c);
    vec& operator*=(const vec& v);
    vec& operator/=(const vec& v);
};

template<>
struct vec<4> {
    union {
        struct {
            alignas(4 * sizeof(float))
            float x;
            float y;
            float z;
            float w;
        };
        alignas(4 * sizeof(float)) float data[4];
    };

    vec() = default;

    constexpr explicit vec(float x)
        : x(x), y(x), z(x), w(x) { }

    constexpr vec(float x, float y, float z)
        : x(x), y(y), z(z), w(1) { }

    constexpr vec(float x, float y, float z, float w)
        : x(x), y(y), z(z), w(w) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]), w(ptr[3]) { }

    constexpr explicit vec(const vec2& v)
        : x(v.x), y(v.y), z(0), w(1) { }

    constexpr vec(const vec2& v, float z)
        : x(v.x), y(v.y), z(z), w(1) { }

    constexpr vec(const vec2& v, float z, float w)
        : x(v.x), y(v.y), z(z), w(w) { }

    constexpr explicit vec(const vec3& v)
        : x(v.x), y(v.y), z(v.z), w(1) { }

    constexpr vec(const vec3& v, float w)
        : x(v.x), y(v.y), z(v.z), w(w) { }

    vec& operator=(const float4_base& v);

    constexpr float& operator[](unsigned i) {
        return data[i];
    }

    constexpr const float& operator[](unsigned i) const {
        return data[i];
    }

    float sum_components() const;
    vec& operator+=(const vec& v);
    vec& operator-=(const vec& v);
    vec& operator*=(const float c);
    vec& operator/=(const float c);
    vec& operator*=(const vec& v);
    vec& operator/=(const vec& v);
    bool operator==(const vec& v) const;
    bool operator!=(const vec& v) const;
    vec operator-() const;
};

constexpr inline vec<2>::vec(const vec3& v)
    : x(v.x), y(v.y) { }

constexpr inline vec<2>::vec(const vec4& v)
    : x(v.x), y(v.y) { }

constexpr inline vec<3>::vec(const vec4& v)
    : x(v.x), y(v.y), z(v.z) { }

template<unsigned dim>
inline vec<dim> operator+(vec<dim> v1, const vec<dim>& v2)
{
    v1 += v2;
    return *v1;
}

template<unsigned dim>
inline vec<dim> operator-(vec<dim> v1, const vec<dim>& v2)
{
    v1 -= v2;
    return *v1;
}

template<unsigned dim>
inline vec<dim> operator*(vec<dim> v, float c)
{
    v *= c;
    return *v;
}

template<unsigned dim>
inline vec<dim> operator*(float c, vec<dim> v)
{
    v *= c;
    return *v;
}

template<unsigned dim>
inline vec<dim> operator/(vec<dim> v, float c)
{
    v /= c;
    return *v;
}

template<unsigned dim>
inline vec<dim> operator*(vec<dim> v1, const vec<dim>& v2)
{
    v1 *= v2;
    return v1;
}

template<unsigned dim>
inline vec<dim> operator/(vec<dim> v1, const vec<dim>& v2)
{
    v1 /= v2;
    return *v1;
}

template<unsigned dim>
float dot(const vec<dim>& v1, const vec<dim>& v2);

struct quat {
    union {
        struct {
            alignas(4 * sizeof(float))
            float x;
            float y;
            float z;
            float w;
        };
        alignas(4 * sizeof(float)) float data[4];
    };

    quat() = default;

    constexpr quat(float x, float y, float z, float w)
        : x(x), y(y), z(z), w(w) { }

    constexpr explicit quat(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]), w(ptr[3]) { }

    quat(const vec3& axis, float angle);
    explicit quat(const vec3& euler_xyz);
    explicit quat(const mat3& rot_mtx);
    explicit quat(const mat4& rot_mtx);

    quat& operator=(const float4_base& v);

    constexpr float& operator[](unsigned i) {
        return data[i];
    }

    constexpr const float& operator[](unsigned i) const {
        return data[i];
    }

    quat& operator*=(const quat& q);
    bool operator==(const quat& q) const;
    bool operator!=(const quat& q) const;
    quat operator-() const;
};

inline quat operator*(quat q1, const quat& q2)
{
    q1 *= q2;
    return q1;
}

struct mat3 {
    union {
        struct {
            alignas(4 * sizeof(float))
            float a00, a10, a20;
            float a01, a11, a21;
            float a02, a12, a22;
        };
        alignas(4 * sizeof(float)) float data[9];
    };

    mat3() = default;
};

struct mat4 {
    union {
        struct {
            alignas(4 * sizeof(float))
            float a00, a10, a20, a30;
            float a01, a11, a21, a31;
            float a02, a12, a22, a32;
            float a03, a13, a23, a33;
        };
        alignas(4 * sizeof(float)) float data[16];
    };

    mat4() = default;

    explicit mat4(const mat3& mtx);

    explicit mat4(const float* ptr);
};

mat4 operator*(const mat4& m1, const mat4& m2);

vec4 operator*(const vec4& v, const mat4& mtx);

vec4 operator*(const mat4& mtx, const vec4& v);

} // namespace vmath
