// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#pragma once

namespace vmath {

constexpr double pi_double  = 3.141592653589793;
constexpr float  pi         = static_cast<float>(pi_double);
constexpr float  pi_squared = static_cast<float>(pi_double * pi_double);
constexpr float  pi_half    = static_cast<float>(pi_double / 2);
constexpr float  two_pi     = static_cast<float>(pi_double * 2);

static inline constexpr float radians(float deg)
{
    constexpr float to_rad = static_cast<float>(pi_double / 180.0);
    return deg * to_rad;
}

static inline constexpr float degrees(float rad)
{
    constexpr float to_deg = static_cast<float>(180.0 / pi_double);
    return rad * to_deg;
}

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

    constexpr explicit vec(float ax)
        : x(ax), y(ax) { }

    constexpr vec(float ax, float ay)
        : x(ax), y(ay) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]) { }

    constexpr explicit vec(const vec3& v);

    constexpr explicit vec(const vec4& v);

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

    constexpr explicit vec(float ax)
        : x(ax), y(ax), z(ax) { }

    constexpr vec(float ax, float ay, float az)
        : x(ax), y(ay), z(az) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]) { }

    constexpr explicit vec(const vec2& v)
        : x(v.x), y(v.y), z(0) { }

    constexpr explicit vec(const vec2& v, float az)
        : x(v.x), y(v.y), z(az) { }

    constexpr explicit vec(const vec4& v);

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

    constexpr explicit vec(float ax)
        : x(ax), y(ax), z(ax), w(ax) { }

    constexpr vec(float ax, float ay, float az)
        : x(ax), y(ay), z(az), w(1) { }

    constexpr vec(float ax, float ay, float az, float aw)
        : x(ax), y(ay), z(az), w(aw) { }

    constexpr explicit vec(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]), w(ptr[3]) { }

    constexpr explicit vec(const vec2& v)
        : x(v.x), y(v.y), z(0), w(1) { }

    constexpr vec(const vec2& v, float az)
        : x(v.x), y(v.y), z(az), w(1) { }

    constexpr vec(const vec2& v, float az, float aw)
        : x(v.x), y(v.y), z(az), w(aw) { }

    constexpr explicit vec(const vec3& v)
        : x(v.x), y(v.y), z(v.z), w(1) { }

    constexpr vec(const vec3& v, float aw)
        : x(v.x), y(v.y), z(v.z), w(aw) { }

    constexpr float& operator[](unsigned i) {
        return data[i];
    }

    constexpr const float& operator[](unsigned i) const {
        return data[i];
    }

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
    return v1;
}

template<unsigned dim>
inline vec<dim> operator-(vec<dim> v1, const vec<dim>& v2)
{
    v1 -= v2;
    return v1;
}

template<unsigned dim>
inline vec<dim> operator*(vec<dim> v, float c)
{
    v *= c;
    return v;
}

template<unsigned dim>
inline vec<dim> operator*(float c, vec<dim> v)
{
    v *= c;
    return v;
}

template<unsigned dim>
inline vec<dim> operator/(vec<dim> v, float c)
{
    v /= c;
    return v;
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
float dot_product(const vec<dim>& v1, const vec<dim>& v2);

vec3 cross_product(const vec3& v1, const vec3& v2);

template<unsigned dim>
float length(const vec<dim>& v);

template<unsigned dim>
float rlength(const vec<dim>& v);

template<unsigned dim>
vec<dim> normalize(const vec<dim>& v);

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

    constexpr quat(float ax, float ay, float az, float aw)
        : x(ax), y(ay), z(az), w(aw) { }

    constexpr explicit quat(const float* ptr)
        : x(ptr[0]), y(ptr[1]), z(ptr[2]), w(ptr[3]) { }

    quat(const vec3& axis, float angle_radians);
    explicit quat(const vec3& euler_xyz);
    explicit quat(const mat3& rot_mtx);
    explicit quat(const mat4& rot_mtx);

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

    vec3 rotate(const vec3& v) const;
};

inline quat operator*(quat q1, const quat& q2)
{
    q1 *= q2;
    return q1;
}

quat conjugate(const quat& q);
quat normalize(const quat& q);

// mat3 represents a 3x3 matrix with column-major layout
// The storage for mat3 is actually mat3x4 (3 columns, 4 rows) to match
// how we pass mat3 data to shaders to satisfy alignment requirements
struct mat3 {
    union {
        struct {
            alignas(4 * sizeof(float))
            float a00;
            float a10, a20, a30;
            float a01, a11, a21, a31;
            float a02, a12, a22, a32;
        };
        alignas(4 * sizeof(float)) float data[12];
    };

    mat3() = default;
    explicit mat3(const mat4& mtx);
    explicit mat3(const float* ptr);
    explicit mat3(const quat& q);
    void set_identity();
    static mat3 identity();
};

// Transposes a matrix
mat3 transpose(const mat3& mtx);

// Creates an inverse matrix
mat3 inverse(const mat3& mtx);

// mat4 represents a 4x4 matrix with column-major layout
// The matrices are typically used in vec * mat multiplication with row vectors
// (NOT mat * vec with column vectors!), and column-major layout simplifies operations with vector
// instructions (e.g. SSE).
struct mat4 {
    union {
        struct {
            alignas(4 * sizeof(float))
            float a00;
            float a10, a20, a30;
            float a01, a11, a21, a31;
            float a02, a12, a22, a32;
            float a03, a13, a23, a33;
        };
        alignas(4 * sizeof(float)) float data[16];
    };

    mat4() = default;
    explicit mat4(const mat3& mtx);
    explicit mat4(const float* ptr);
    explicit mat4(const quat& q);
    void set_identity();
    static mat4 identity();
};

mat4 operator*(const mat4& m1, const mat4& m2);

// Multiplies a row vector by a matrix, i.e. vec * mat
// All matrices are constructed with assumption that this operation is used
// to perform transformations.
vec4 operator*(const vec4& v, const mat4& mtx);

// Multiplies a matrix by a column vector, i.e. mat * vec
// This operation is rarely used, the vec * mat operation is preferred, all matrices
// assume that vec * mat is used.
vec4 operator*(const mat4& mtx, const vec4& v);

// Transposes a matrix
mat4 transpose(const mat4& mtx);

// Creates perspective projection transform matrix in the following form:
//
//     [ xf 0  0  0 ]
//     [ 0  yf 0  0 ]
//     [ 0  0  zf 1 ]
//     [ 0  0  wf 0 ]
//
// This projection matrix is for left-handed coordinate system and is used for vec * mat
// multiplications with row vectors (NOT mat * vec with column vectors!).
//
// Input:
// - aspect         - aspect ratio of the rendered view rectangle
// - fov_radians    - horizontal field of view, in radians
// - near_plane     - distance of the near plane, in view space coordinates
// - far_plane      - distance of the far plane, in view space coordinates
//
// The matrix contains the following values (aside from 0s and 1s):
// - xf - multiplication factor for x coordinate = 1 / (aspect * tan(fov_radians / 2))
// - yf - multiplication factor for y coordinate = 1 / tan(fov_radians / 2)
// - zf - multiplication factor for z coordinate = -near_plane / (far_plane - near_plane)
// - wf - multiplication factor for w coordinate = (far_plane * near_plane) / (far_plane - near_plane)
mat4 projection(float aspect, float fov_radians, float near_plane, float far_plane);

// Creates a vector containing non-0/1 factors of the perspective projection transform matrix
// The returned vector is: [xf, yf, zf, wf]
// See projection() for the meaning of inputs and output factors.
// This form simplifies calculations in shaders.
vec4 projection_vector(float aspect, float fov_radians, float near_plane, float far_plane);

// Creates orthographic projection matrix in the form of:
//
//     [ xf 0  0  0 ]
//     [ 0  yf 0  0 ]
//     [ 0  0  zf 0 ]
//     [ 0  0  wf 1 ]
//
// Input:
// - aspect         - aspect ratio of the rendered view rectangle
// - height         - height of view, in view space coordinates
// - near_plane     - distance of the near plane, in view space coordinates
// - far_plane      - distance of the far plane, in view space coordinates
//
// The matrix contains the following values (aside from 0s and 1s):
// - xf - multiplication factor for x coordinate = 1 / (aspect * height / 2)
// - yf - multiplication factor for y coordinate = 1 / (height / 2)
// - zf - multiplication factor for z coordinate = -1 / (far_plane - near_plane)
// - wf - multiplication factor for w coordinate = far_plane / (far_plane - near_plane)
mat4 ortho(float aspect, float height, float near_plane, float far_plane);

// Creates a vector containing non-0/1 factors of the orthographic projection transform matrix
// The returned vector is: [xf, yf, zf, wf]
// See ortho() for the meaning of inputs and output factors.
// This form simplifies calculations in shaders.
vec4 ortho_vector(float aspect, float height, float near_plane, float far_plane);

// Creates a look-at view transform matrix, used for transforming points from world coordinate space
// into view coordinate space.
//
// Input:
// - eye_pos    - position of the camera's "eye"
// - target     - point at which the camera is looking
mat4 look_at(const vec3& eye_pos, const vec3& target);

// Creates a translation transform matrix
mat4 translate(float x, float y, float z);

// Creates a scale transform matrix
mat4 scale(float x, float y, float z);

// Creates a translation transform matrix
inline mat4 translate(const vec3& v)
{
    return translate(v.x, v.y, v.z);
}

// Creates a scale transform matrix
inline mat4 scale(const vec3& v)
{
    return scale(v.x, v.y, v.z);
}

} // namespace vmath
