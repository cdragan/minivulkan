// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

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
float dot_product(const vec<dim>& v1, const vec<dim>& v2);

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
};

inline quat operator*(quat q1, const quat& q2)
{
    q1 *= q2;
    return q1;
}

quat conjugate(const quat& q);
quat normalize(const quat& q);

struct mat3 {
    union {
        struct {
            alignas(4 * sizeof(float))
            float a00;
            float a10, a20;
            float a01, a11, a21;
            float a02, a12, a22;
        };
        alignas(4 * sizeof(float)) float data[9];
    };

    mat3() = default;
    explicit mat3(const float* ptr);
    explicit mat3(const quat& q);
    void set_identity();
    static mat3 identity();
};

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
vec4 operator*(const vec4& v, const mat4& mtx);
vec4 operator*(const mat4& mtx, const vec4& v);
mat4 transpose(const mat4& mtx);
mat4 projection(float aspect, float fov_radians, float near_plane, float far_plane, float depth_bias);
mat4 translate(float x, float y, float z);
mat4 scale(float x, float y, float z);

inline mat4 translate(const vec3& v)
{
    return translate(v.x, v.y, v.z);
}

inline mat4 scale(const vec3& v)
{
    return scale(v.x, v.y, v.z);
}

} // namespace vmath
