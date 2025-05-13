// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "vmath.h"
#include "mstdc.h"
#include "vecfloat.h"
#include <math.h>
#include <stdio.h>

#define TEST(test) if ( ! (test)) { failed(#test, __FILE__, __LINE__); }

static int exit_code = 0;

static bool is_near(float value1, float value2, float max_error = 0.005f)
{
    return fabs(value1 - value2) < max_error;
}

static void failed(const char* test, const char* file, int line)
{
    exit_code = 1;
    fprintf(stderr, "%s:%d: Error: Failed condition %s\n",
            file, line, test);
}

int main()
{
    //////////////////////////////////////////////////////////////////////////////////////////
    // exp2

    TEST(is_near(mstd::exp2(-5.0f), exp2f(-5.0f)));
    TEST(is_near(mstd::exp2(-4.9f), exp2f(-4.9f)));
    TEST(is_near(mstd::exp2(-4.8f), exp2f(-4.8f)));
    TEST(is_near(mstd::exp2(-4.7f), exp2f(-4.7f)));
    TEST(is_near(mstd::exp2(-4.6f), exp2f(-4.6f)));
    TEST(is_near(mstd::exp2(-4.5f), exp2f(-4.5f)));
    TEST(is_near(mstd::exp2(-4.4f), exp2f(-4.4f)));
    TEST(is_near(mstd::exp2(-4.3f), exp2f(-4.3f)));
    TEST(is_near(mstd::exp2(-4.2f), exp2f(-4.2f)));
    TEST(is_near(mstd::exp2(-4.1f), exp2f(-4.1f)));
    TEST(is_near(mstd::exp2(-4.0f), exp2f(-4.0f)));
    TEST(is_near(mstd::exp2(-3.9f), exp2f(-3.9f)));
    TEST(is_near(mstd::exp2(-3.8f), exp2f(-3.8f)));
    TEST(is_near(mstd::exp2(-3.7f), exp2f(-3.7f)));
    TEST(is_near(mstd::exp2(-3.6f), exp2f(-3.6f)));
    TEST(is_near(mstd::exp2(-3.5f), exp2f(-3.5f)));
    TEST(is_near(mstd::exp2(-3.4f), exp2f(-3.4f)));
    TEST(is_near(mstd::exp2(-3.3f), exp2f(-3.3f)));
    TEST(is_near(mstd::exp2(-3.2f), exp2f(-3.2f)));
    TEST(is_near(mstd::exp2(-3.1f), exp2f(-3.1f)));
    TEST(is_near(mstd::exp2(-3.0f), exp2f(-3.0f)));
    TEST(is_near(mstd::exp2(-2.9f), exp2f(-2.9f)));
    TEST(is_near(mstd::exp2(-2.8f), exp2f(-2.8f)));
    TEST(is_near(mstd::exp2(-2.7f), exp2f(-2.7f)));
    TEST(is_near(mstd::exp2(-2.6f), exp2f(-2.6f)));
    TEST(is_near(mstd::exp2(-2.5f), exp2f(-2.5f)));
    TEST(is_near(mstd::exp2(-2.4f), exp2f(-2.4f)));
    TEST(is_near(mstd::exp2(-2.3f), exp2f(-2.3f)));
    TEST(is_near(mstd::exp2(-2.2f), exp2f(-2.2f)));
    TEST(is_near(mstd::exp2(-2.1f), exp2f(-2.1f)));
    TEST(is_near(mstd::exp2(-2.0f), exp2f(-2.0f)));
    TEST(is_near(mstd::exp2(-1.9f), exp2f(-1.9f)));
    TEST(is_near(mstd::exp2(-1.8f), exp2f(-1.8f)));
    TEST(is_near(mstd::exp2(-1.7f), exp2f(-1.7f)));
    TEST(is_near(mstd::exp2(-1.6f), exp2f(-1.6f)));
    TEST(is_near(mstd::exp2(-1.5f), exp2f(-1.5f)));
    TEST(is_near(mstd::exp2(-1.4f), exp2f(-1.4f)));
    TEST(is_near(mstd::exp2(-1.3f), exp2f(-1.3f)));
    TEST(is_near(mstd::exp2(-1.2f), exp2f(-1.2f)));
    TEST(is_near(mstd::exp2(-1.1f), exp2f(-1.1f)));
    TEST(is_near(mstd::exp2(-1.0f), exp2f(-1.0f)));
    TEST(is_near(mstd::exp2(-0.9f), exp2f(-0.9f), 0.01f));
    TEST(is_near(mstd::exp2(-0.8f), exp2f(-0.8f)));
    TEST(is_near(mstd::exp2(-0.7f), exp2f(-0.7f)));
    TEST(is_near(mstd::exp2(-0.6f), exp2f(-0.6f)));
    TEST(is_near(mstd::exp2(-0.5f), exp2f(-0.5f)));
    TEST(is_near(mstd::exp2(-0.4f), exp2f(-0.4f)));
    TEST(is_near(mstd::exp2(-0.3f), exp2f(-0.3f)));
    TEST(is_near(mstd::exp2(-0.2f), exp2f(-0.2f)));
    TEST(is_near(mstd::exp2(-0.1f), exp2f(-0.1f)));
    TEST(is_near(mstd::exp2(0.0f), exp2f(0.0f)));
    TEST(is_near(mstd::exp2(0.1f), exp2f(0.1f)));
    TEST(is_near(mstd::exp2(0.2f), exp2f(0.2f)));
    TEST(is_near(mstd::exp2(0.3f), exp2f(0.3f)));
    TEST(is_near(mstd::exp2(0.4f), exp2f(0.4f)));
    TEST(is_near(mstd::exp2(0.5f), exp2f(0.5f)));
    TEST(is_near(mstd::exp2(0.6f), exp2f(0.6f)));
    TEST(is_near(mstd::exp2(0.7f), exp2f(0.7f)));
    TEST(is_near(mstd::exp2(0.8f), exp2f(0.8f)));
    TEST(is_near(mstd::exp2(0.9f), exp2f(0.9f), 0.01f));
    TEST(is_near(mstd::exp2(1.0f), exp2f(1.0f)));
    TEST(is_near(mstd::exp2(1.1f), exp2f(1.1f)));
    TEST(is_near(mstd::exp2(1.2f), exp2f(1.2f)));
    TEST(is_near(mstd::exp2(1.3f), exp2f(1.3f)));
    TEST(is_near(mstd::exp2(1.4f), exp2f(1.4f)));
    TEST(is_near(mstd::exp2(1.5f), exp2f(1.5f)));
    TEST(is_near(mstd::exp2(1.6f), exp2f(1.6f)));
    TEST(is_near(mstd::exp2(1.7f), exp2f(1.7f), 0.01f));
    TEST(is_near(mstd::exp2(1.8f), exp2f(1.8f), 0.01f));
    TEST(is_near(mstd::exp2(1.9f), exp2f(1.9f), 0.02f));
    TEST(is_near(mstd::exp2(2.0f), exp2f(2.0f)));
    TEST(is_near(mstd::exp2(2.1f), exp2f(2.1f)));
    TEST(is_near(mstd::exp2(2.2f), exp2f(2.2f)));
    TEST(is_near(mstd::exp2(2.3f), exp2f(2.3f)));
    TEST(is_near(mstd::exp2(2.4f), exp2f(2.4f)));
    TEST(is_near(mstd::exp2(2.5f), exp2f(2.5f)));
    TEST(is_near(mstd::exp2(2.6f), exp2f(2.6f), 0.01f));
    TEST(is_near(mstd::exp2(2.7f), exp2f(2.7f), 0.02f));
    TEST(is_near(mstd::exp2(2.8f), exp2f(2.8f), 0.02f));
    TEST(is_near(mstd::exp2(2.9f), exp2f(2.9f), 0.03f));
    TEST(is_near(mstd::exp2(3.0f), exp2f(3.0f)));
    TEST(is_near(mstd::exp2(3.1f), exp2f(3.1f)));
    TEST(is_near(mstd::exp2(3.2f), exp2f(3.2f)));
    TEST(is_near(mstd::exp2(3.3f), exp2f(3.3f)));
    TEST(is_near(mstd::exp2(3.4f), exp2f(3.4f)));
    TEST(is_near(mstd::exp2(3.5f), exp2f(3.5f), 0.01f));
    TEST(is_near(mstd::exp2(3.6f), exp2f(3.6f), 0.02f));
    TEST(is_near(mstd::exp2(3.7f), exp2f(3.7f), 0.03f));
    TEST(is_near(mstd::exp2(3.8f), exp2f(3.8f), 0.04f));
    TEST(is_near(mstd::exp2(3.9f), exp2f(3.9f), 0.06f));
    TEST(is_near(mstd::exp2(4.0f), exp2f(4.0f)));
    TEST(is_near(mstd::exp2(4.1f), exp2f(4.1f)));
    TEST(is_near(mstd::exp2(4.2f), exp2f(4.2f)));
    TEST(is_near(mstd::exp2(4.3f), exp2f(4.3f)));
    TEST(is_near(mstd::exp2(4.4f), exp2f(4.4f)));
    TEST(is_near(mstd::exp2(4.5f), exp2f(4.5f), 0.02f));
    TEST(is_near(mstd::exp2(4.6f), exp2f(4.6f), 0.03f));
    TEST(is_near(mstd::exp2(4.7f), exp2f(4.7f), 0.05f));
    TEST(is_near(mstd::exp2(4.8f), exp2f(4.8f), 0.08f));
    TEST(is_near(mstd::exp2(4.9f), exp2f(4.9f), 0.12f));
    TEST(is_near(mstd::exp2(5.0f), exp2f(5.0f)));

    //////////////////////////////////////////////////////////////////////////////////////////
    // constants

    TEST(is_near(vmath::pi,             3.141592f));
    TEST(is_near(vmath::pi_half,        1.570796f));
    TEST(is_near(vmath::pi_squared,     9.869604f));
    TEST(is_near(vmath::two_pi,         6.283185f));
    TEST(is_near(vmath::radians(60.0f), 1.047197f));
    TEST(is_near(vmath::degrees(1.0f),  57.295779f));

    //////////////////////////////////////////////////////////////////////////////////////////
    // sincos

    {
        const vmath::sin_cos_result sc = vmath::sincos(0);
        TEST(sc.sin == 0);
        TEST(sc.cos == 1);
    }

    for (float radians = 0.5f; radians < 7.0f; radians += 0.5f) {

        const float exp_sin = sinf(radians);
        const float exp_cos = cosf(radians);

        const vmath::sin_cos_result sc = vmath::sincos(radians);

        if ( ! is_near(sc.cos, exp_cos)) {
            fprintf(stderr, "Error: cos %f is %f but should be %f\n",
                    static_cast<double>(radians),
                    static_cast<double>(sc.cos),
                    static_cast<double>(exp_cos));
            exit_code = 1;
        }

        if ( ! is_near(sc.sin, exp_sin)) {
            fprintf(stderr, "Error: sin %f is %f but should be %f\n",
                    static_cast<double>(radians),
                    static_cast<double>(sc.sin),
                    static_cast<double>(exp_sin));
            exit_code = 1;
        }
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // sincos4

    {
        const vmath::sin_cos_result4 sc = vmath::sincos(vmath::float4::load_zero());
        TEST(sc.sin[0] == 0);
        TEST(sc.sin[1] == 0);
        TEST(sc.sin[2] == 0);
        TEST(sc.sin[3] == 0);
        TEST(sc.cos[0] == 1);
        TEST(sc.cos[1] == 1);
        TEST(sc.cos[2] == 1);
        TEST(sc.cos[3] == 1);
    }

    for (float radians = -6.5f; radians < 7.0f; radians += 2.0f) {

        const float in_radians[4] = { radians, radians + 0.5f, radians + 1.0f, radians + 1.5f };

        const float exp_sin[4] = { sinf(in_radians[0]), sinf(in_radians[1]), sinf(in_radians[2]), sinf(in_radians[3]) };
        const float exp_cos[4] = { cosf(in_radians[0]), cosf(in_radians[1]), cosf(in_radians[2]), cosf(in_radians[3]) };

        const vmath::sin_cos_result4 sc = vmath::sincos(vmath::float4::load4(in_radians));

        for (int i = 0; i < 4; i++) {

            const float cur_cos = sc.cos[i];

            if ( ! is_near(cur_cos, exp_cos[i])) {
                fprintf(stderr, "Error: cos %f at position %d is %f but should be %f\n",
                        static_cast<double>(in_radians[i]),
                        i,
                        static_cast<double>(cur_cos),
                        static_cast<double>(exp_cos[i]));
                exit_code = 1;
            }

            const float cur_sin = sc.sin[i];

            if ( ! is_near(cur_sin, exp_sin[i])) {
                fprintf(stderr, "Error: sin %f at position %d is %f but should be %f\n",
                        static_cast<double>(in_radians[i]),
                        i,
                        static_cast<double>(cur_sin),
                        static_cast<double>(exp_sin[i]));
                exit_code = 1;
            }
        }
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // vec2

    // vec2(), data sizes
    {
        vmath::vec2 v;

        static_assert(mstd::array_size(v.data) == 2);
        static_assert(sizeof(v.data) == 8);
    }

    // vec2(x)
    {
        const vmath::vec2 v{42};

        TEST(v.x == 42);
        TEST(v.y == 42);
    }

    // vec2(x, y), access
    {
        const vmath::vec2 v{8, 9};

        TEST(v.x == 8);
        TEST(v.y == 9);

        TEST(v.data[0] == 8);
        TEST(v.data[1] == 9);

        TEST(v[0] == 8);
        TEST(v[1] == 9);
    }

    // vec2(float*)
    {
        const float xy[] = { -10, -11 };
        const vmath::vec2 v{xy};

        TEST(v.x == -10);
        TEST(v.y == -11);
    }

    // vec2::operator[]
    {
        vmath::vec2 v;

        v[0] = 3;
        v[1] = 4;

        TEST(v.x == 3);
        TEST(v.y == 4);

        TEST(v[0] == 3);
        TEST(v[1] == 4);
    }

    // vec2(vec3)
    {
        const vmath::vec2 v{vmath::vec3{10, 11, 12}};

        TEST(v.x == 10);
        TEST(v.y == 11);
    }

    // vec2(vec4)
    {
        const vmath::vec2 v{vmath::vec4{13, 14, 15}};

        TEST(v.x == 13);
        TEST(v.y == 14);
    }

    // vec2::operator== and !=
    {
        const vmath::vec2 v1{1, 2};
        const vmath::vec2 v2{1, 2};
        const vmath::vec2 v3{-1, 2};
        const vmath::vec2 v4{1, -2};
        const vmath::vec2 v5{-1, -2};

        TEST(v1 == v1);
        TEST(v1 == v2);
        TEST( ! (v1 == v3));
        TEST( ! (v1 == v4));
        TEST( ! (v1 == v5));

        TEST( ! (v1 != v1));
        TEST( ! (v1 != v2));
        TEST(v1 != v3);
        TEST(v1 != v4);
        TEST(v1 != v5);
    }

    // vec2::operator- (negation)
    {
        const vmath::vec2 v1{3, 4};
        const vmath::vec2 v2 = -v1;

        TEST(v2.x == -3);
        TEST(v2.y == -4);
    }

    // vec2::operator+=
    {
        vmath::vec2 v1{1, 2};
        const vmath::vec2 v2{3, -3};

        v1 += v2;

        TEST(v1.x == 4);
        TEST(v1.y == -1);
    }

    // vec2::operator-=
    {
        vmath::vec2 v1{1, 2};
        const vmath::vec2 v2{3, -3};

        v1 -= v2;

        TEST(v1.x == -2);
        TEST(v1.y == 5);
    }

    // vec2::operator*=
    {
        vmath::vec2 v1{1, 2};
        const vmath::vec2 v2{3, -3};

        v1 *= v2;

        TEST(v1.x == 3);
        TEST(v1.y == -6);
    }

    // vec2::operator*=
    {
        vmath::vec2 v1{2, 3};

        v1 *= 4;

        TEST(v1.x == 8);
        TEST(v1.y == 12);
    }

    // vec2::operator/=
    {
        vmath::vec2 v1{21, 24};
        const vmath::vec2 v2{7, 6};

        v1 /= v2;

        TEST(v1.x == 3);
        TEST(v1.y == 4);
    }

    // vec2::operator/=
    {
        vmath::vec2 v1{35, 14};

        v1 /= 7;

        TEST(v1.x == 5);
        TEST(v1.y == 2);
    }

    // dot_product(vec2)
    {
        const vmath::vec2 v1{2, 4};
        const vmath::vec2 v2{3, 5};

        const float dp = vmath::dot_product(v1, v2);

        TEST(dp == 26);
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // vec3

    // vec3(), data sizes
    {
        vmath::vec3 v;

        static_assert(mstd::array_size(v.data) == 3);
        static_assert(sizeof(v.data) == 12);
    }

    // vec3(x)
    {
        const vmath::vec3 v{42};

        TEST(v.x == 42);
        TEST(v.y == 42);
        TEST(v.z == 42);
    }

    // vec3(x, y, z), access
    {
        const vmath::vec3 v{21, 22, 23};

        TEST(v.x == 21);
        TEST(v.y == 22);
        TEST(v.z == 23);

        TEST(v.data[0] == 21);
        TEST(v.data[1] == 22);
        TEST(v.data[2] == 23);

        TEST(v[0] == 21);
        TEST(v[1] == 22);
        TEST(v[2] == 23);
    }

    // vec3(float*)
    {
        const float xyz[] = { -10, -11, -12 };
        const vmath::vec3 v{xyz};

        TEST(v.x == -10);
        TEST(v.y == -11);
        TEST(v.z == -12);
    }

    // vec3::operator[]
    {
        vmath::vec3 v;

        v[0] = 3;
        v[1] = 4;
        v[2] = 5;

        TEST(v.x == 3);
        TEST(v.y == 4);
        TEST(v.z == 5);

        TEST(v[0] == 3);
        TEST(v[1] == 4);
        TEST(v[2] == 5);
    }

    // vec3(vec2)
    {
        const vmath::vec3 v{vmath::vec2{8, 9}};

        TEST(v.x == 8);
        TEST(v.y == 9);
        TEST(v.z == 0);
    }

    // vec3(vec2, z)
    {
        const vmath::vec3 v{vmath::vec2{9, 7}, 6};

        TEST(v.x == 9);
        TEST(v.y == 7);
        TEST(v.z == 6);
    }

    // vec3(vec4)
    {
        const vmath::vec3 v{vmath::vec4{13, 14, 15, 16}};

        TEST(v.x == 13);
        TEST(v.y == 14);
        TEST(v.z == 15);
    }

    // vec3::operator== and !=
    {
        const vmath::vec3 v1{1, 2, 3};
        const vmath::vec3 v2{1, 2, 3};
        const vmath::vec3 v3{-1, 2, 3};
        const vmath::vec3 v4{1, -2, 3};
        const vmath::vec3 v5{1, 2, -3};
        const vmath::vec3 v6{-1, -2, -3};

        TEST(v1 == v1);
        TEST(v1 == v2);
        TEST( ! (v1 == v3));
        TEST( ! (v1 == v4));
        TEST( ! (v1 == v5));
        TEST( ! (v1 == v6));

        TEST( ! (v1 != v1));
        TEST( ! (v1 != v2));
        TEST(v1 != v3);
        TEST(v1 != v4);
        TEST(v1 != v5);
        TEST(v1 != v6);
    }

    // vec3::operator- (negation)
    {
        const vmath::vec3 v1{3, 4, 5};
        const vmath::vec3 v2 = -v1;

        TEST(v2.x == -3);
        TEST(v2.y == -4);
        TEST(v2.z == -5);
    }

    // vec3::operator+=
    {
        vmath::vec3 v1{1, 2, 3};
        const vmath::vec3 v2{3, -3, 10};

        v1 += v2;

        TEST(v1.x == 4);
        TEST(v1.y == -1);
        TEST(v1.z == 13);
    }

    // vec3::operator-=
    {
        vmath::vec3 v1{1, 2, 3};
        const vmath::vec3 v2{3, -3, 10};

        v1 -= v2;

        TEST(v1.x == -2);
        TEST(v1.y == 5);
        TEST(v1.z == -7);
    }

    // vec3::operator*=
    {
        vmath::vec3 v1{1, 2, -3};
        const vmath::vec3 v2{3, -3, 4};

        v1 *= v2;

        TEST(v1.x == 3);
        TEST(v1.y == -6);
        TEST(v1.z == -12);
    }

    // vec3::operator*=
    {
        vmath::vec3 v1{2, 3, 4};

        v1 *= 4;

        TEST(v1.x == 8);
        TEST(v1.y == 12);
        TEST(v1.z == 16);
    }

    // vec3::operator/=
    {
        vmath::vec3 v1{21, 24, 26};
        const vmath::vec3 v2{7, 6, 2};

        v1 /= v2;

        TEST(v1.x == 3);
        TEST(v1.y == 4);
        TEST(v1.z == 13);
    }

    // vec3::operator/=
    {
        vmath::vec3 v1{35, 14, -56};

        v1 /= 7;

        TEST(v1.x == 5);
        TEST(v1.y == 2);
        TEST(v1.z == -8);
    }

    // dot_product(vec3)
    {
        const vmath::vec3 v1{2, 4, -1};
        const vmath::vec3 v2{3, 5, 7};

        const float dp = vmath::dot_product(v1, v2);

        TEST(dp == 19);
    }

    // dot_product3(float4)
    {
        const vmath::float4 v1{2, 4, -1, 2};
        const vmath::float4 v2{3, 5, 7, 6};

        const vmath::float4 dp = vmath::dot_product3(v1, v2);

        TEST(dp[0] == 19);
        TEST(dp[1] == 19);
        TEST(dp[2] == 19);
        TEST(dp[3] == 19);
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // vec4

    // vec4(), data sizes
    {
        vmath::vec4 v;

        static_assert(mstd::array_size(v.data) == 4);
        static_assert(sizeof(v.data) == 16);
    }

    // vec4(x)
    {
        const vmath::vec4 v{42};

        TEST(v.x == 42);
        TEST(v.y == 42);
        TEST(v.z == 42);
        TEST(v.w == 42);
    }

    // vec4(x, y, z)
    {
        const vmath::vec4 v{21, 22, 23};

        TEST(v.x == 21);
        TEST(v.y == 22);
        TEST(v.z == 23);
        TEST(v.w == 1);
    }

    // vec4(x, y, z, w), access
    {
        const vmath::vec4 v{21, 22, 23, 24};

        TEST(v.x == 21);
        TEST(v.y == 22);
        TEST(v.z == 23);
        TEST(v.w == 24);

        TEST(v.data[0] == 21);
        TEST(v.data[1] == 22);
        TEST(v.data[2] == 23);
        TEST(v.data[3] == 24);

        TEST(v[0] == 21);
        TEST(v[1] == 22);
        TEST(v[2] == 23);
        TEST(v[3] == 24);
    }

    // vec4(float*)
    {
        const float xyzw[] = { -10, -11, -12, -13 };
        const vmath::vec4 v{xyzw};

        TEST(v.x == -10);
        TEST(v.y == -11);
        TEST(v.z == -12);
        TEST(v.w == -13);
    }

    // vec4::operator[]
    {
        vmath::vec4 v;

        v[0] = 3;
        v[1] = 4;
        v[2] = 5;
        v[3] = 6;

        TEST(v.x == 3);
        TEST(v.y == 4);
        TEST(v.z == 5);
        TEST(v.w == 6);

        TEST(v[0] == 3);
        TEST(v[1] == 4);
        TEST(v[2] == 5);
        TEST(v[3] == 6);
    }

    // vec4(vec2)
    {
        const vmath::vec4 v{vmath::vec2{8, 9}};

        TEST(v.x == 8);
        TEST(v.y == 9);
        TEST(v.z == 0);
        TEST(v.w == 1);
    }

    // vec4(vec2, z)
    {
        const vmath::vec4 v{vmath::vec2{9, 7}, 6};

        TEST(v.x == 9);
        TEST(v.y == 7);
        TEST(v.z == 6);
        TEST(v.w == 1);
    }

    // vec4(vec2, z, w)
    {
        const vmath::vec4 v{vmath::vec2{9, 7}, 6, 8};

        TEST(v.x == 9);
        TEST(v.y == 7);
        TEST(v.z == 6);
        TEST(v.w == 8);
    }

    // vec4(vec3)
    {
        const vmath::vec4 v{vmath::vec3{9, 7, 8}};

        TEST(v.x == 9);
        TEST(v.y == 7);
        TEST(v.z == 8);
        TEST(v.w == 1);
    }

    // vec4(vec3, w)
    {
        const vmath::vec4 v{vmath::vec3{9, 7, 8}, 6};

        TEST(v.x == 9);
        TEST(v.y == 7);
        TEST(v.z == 8);
        TEST(v.w == 6);
    }

    // vec4::operator== and !=
    {
        const vmath::vec4 v1{1, 2, 3, 4};
        const vmath::vec4 v2{1, 2, 3, 4};
        const vmath::vec4 v3{-1, 2, 3, 4};
        const vmath::vec4 v4{1, -2, 3, 4};
        const vmath::vec4 v5{1, 2, -3, 4};
        const vmath::vec4 v6{1, 2, 3, -4};
        const vmath::vec4 v7{-1, -2, -3, -4};

        TEST(v1 == v1);
        TEST(v1 == v2);
        TEST( ! (v1 == v3));
        TEST( ! (v1 == v4));
        TEST( ! (v1 == v5));
        TEST( ! (v1 == v6));
        TEST( ! (v1 == v7));

        TEST( ! (v1 != v1));
        TEST( ! (v1 != v2));
        TEST(v1 != v3);
        TEST(v1 != v4);
        TEST(v1 != v5);
        TEST(v1 != v6);
        TEST(v1 != v7);
    }

    // float4::operator== and !=
    {
        const vmath::float4 v1{1, 2, 3, 4};
        const vmath::float4 v2{1, 2, 3, 4};
        const vmath::float4 v3{-1, 2, 3, 4};
        const vmath::float4 v4{1, -2, 3, 4};
        const vmath::float4 v5{1, 2, -3, 4};
        const vmath::float4 v6{1, 2, 3, -4};
        const vmath::float4 v7{-1, -2, -3, -4};

        const auto check = [](float f, bool is_true) -> bool
        {
            const uint32_t u = *reinterpret_cast<uint32_t*>(&f);
            if (is_true)
                return u == ~0U;
            else
                return u == 0;
        };

        vmath::float4 cmp;

        cmp = v1 == v1;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], true));
        TEST(cmp.all());

        cmp = v1 == v2;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], true));
        TEST(cmp.all());

        cmp = v1 == v3;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], true));
        TEST(!cmp.all());

        cmp = v1 == v4;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], true));
        TEST(!cmp.all());

        cmp = v1 == v5;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], true));
        TEST(!cmp.all());

        cmp = v1 == v6;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], false));
        TEST(!cmp.all());

        cmp = v1 == v7;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], false));
        TEST(!cmp.all());

        cmp = v1 != v1;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], false));
        TEST(!cmp.any());

        cmp = v1 != v2;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], false));
        TEST(!cmp.any());

        cmp = v1 != v3;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], false));
        TEST(cmp.any());

        cmp = v1 != v4;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], false));
        TEST(cmp.any());

        cmp = v1 != v5;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], false));
        TEST(cmp.any());

        cmp = v1 != v6;
        TEST(check(cmp[0], false));
        TEST(check(cmp[1], false));
        TEST(check(cmp[2], false));
        TEST(check(cmp[3], true));
        TEST(cmp.any());

        cmp = v1 != v7;
        TEST(check(cmp[0], true));
        TEST(check(cmp[1], true));
        TEST(check(cmp[2], true));
        TEST(check(cmp[3], true));
        TEST(cmp.any());
    }

    // vec4::operator- (negation)
    {
        const vmath::vec4 v1{3, 4, 5, 6};
        const vmath::vec4 v2 = -v1;

        TEST(v2.x == -3);
        TEST(v2.y == -4);
        TEST(v2.z == -5);
        TEST(v2.w == -6);
    }

    // vec4::operator+=
    {
        vmath::vec4 v1{1, 2, 3, -2};
        const vmath::vec4 v2{3, -3, 10, 9};

        v1 += v2;

        TEST(v1.x == 4);
        TEST(v1.y == -1);
        TEST(v1.z == 13);
        TEST(v1.w == 7);
    }

    // vec4::operator-=
    {
        vmath::vec4 v1{1, 2, 3};
        const vmath::vec4 v2{3, -3, 10};

        v1 -= v2;

        TEST(v1.x == -2);
        TEST(v1.y == 5);
        TEST(v1.z == -7);
    }

    // vec4::operator*=
    {
        vmath::vec4 v1{1, 2, -3, 4};
        const vmath::vec4 v2{3, -3, 4, 5};

        v1 *= v2;

        TEST(v1.x == 3);
        TEST(v1.y == -6);
        TEST(v1.z == -12);
        TEST(v1.w == 20);
    }

    // vec4::operator*=
    {
        vmath::vec4 v1{2, 3, 4, 5};

        v1 *= 4;

        TEST(v1.x == 8);
        TEST(v1.y == 12);
        TEST(v1.z == 16);
        TEST(v1.w == 20);
    }

    // vec4::operator/=
    {
        vmath::vec4 v1{21, 24, 26, 45};
        const vmath::vec4 v2{7, 6, 2, 5};

        v1 /= v2;

        TEST(v1.x == 3);
        TEST(v1.y == 4);
        TEST(v1.z == 13);
        TEST(v1.w == 9);
    }

    // vec4::operator/=
    {
        vmath::vec4 v1{35, 14, -56, 21};

        v1 /= 7;

        TEST(v1.x == 5);
        TEST(v1.y == 2);
        TEST(v1.z == -8);
        TEST(v1.w == 3);
    }

    // dot_product(vec4)
    {
        const vmath::vec4 v1{2, 4, -1, 6};
        const vmath::vec4 v2{3, 5, 7, 8};

        const float dp = vmath::dot_product(v1, v2);

        TEST(dp == 67);
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // quat

    // conjugate
    {
        const vmath::quat q{1, 2, 3, 4};
        const vmath::quat c = vmath::conjugate(q);

        TEST(c.x == -1);
        TEST(c.y == -2);
        TEST(c.z == -3);
        TEST(c.w == 4);
    }

    // quat::operator-
    {
        const vmath::quat q{1, 2, 3, 4};
        const vmath::quat c = -q;

        TEST(c.x == -1);
        TEST(c.y == -2);
        TEST(c.z == -3);
        TEST(c.w == -4);
    }

    // normalize
    {
        const vmath::quat q{10.0f, 10.0f, 10.0f, 10.0f};
        const vmath::quat n = vmath::normalize(q);

        TEST(is_near(n.x, 0.5f));
        TEST(is_near(n.y, 0.5f));
        TEST(is_near(n.z, 0.5f));
        TEST(is_near(n.w, 0.5f));
    }

    // quat, axis, angle
    {
        const vmath::quat q{vmath::vec3{0, 0, 1}, vmath::pi_half};
        const vmath::vec3 v{q.rotate(vmath::vec3{1, 0, 0})};

        TEST(is_near(v.x, 0));
        TEST(is_near(v.y, 1));
        TEST(is_near(v.z, 0));
    }
    {
        const vmath::quat q{vmath::vec3{0, 1, 0}, vmath::pi_half};
        const vmath::vec3 v{q.rotate(vmath::vec3{0, 0, 1})};

        TEST(is_near(v.x, 1));
        TEST(is_near(v.y, 0));
        TEST(is_near(v.z, 0));
    }
    {
        const vmath::quat q{vmath::vec3{1, 0, 0}, vmath::pi_half};
        const vmath::vec3 v{q.rotate(vmath::vec3{0, 1, 0})};

        TEST(is_near(v.x, 0));
        TEST(is_near(v.y, 0));
        TEST(is_near(v.z, 1));
    }

    // quat, euler_xyz
    {
        const vmath::quat q = vmath::quat::from_euler(vmath::vec3{0, 0, vmath::pi_half});
        const vmath::vec3 v{q.rotate(vmath::vec3{1, 0, 0})};

        TEST(is_near(v.x, 0));
        TEST(is_near(v.y, 1));
        TEST(is_near(v.z, 0));
    }
    {
        const vmath::quat q = vmath::quat::from_euler(vmath::vec3{0, vmath::pi_half, 0});
        const vmath::vec3 v{q.rotate(vmath::vec3{0, 0, 1})};

        TEST(is_near(v.x, 1));
        TEST(is_near(v.y, 0));
        TEST(is_near(v.z, 0));
    }
    {
        const vmath::quat q = vmath::quat::from_euler(vmath::vec3{vmath::pi_half, 0, 0});
        const vmath::vec3 v{q.rotate(vmath::vec3{0, 1, 0})};

        TEST(is_near(v.x, 0));
        TEST(is_near(v.y, 0));
        TEST(is_near(v.z, 1));
    }

    //////////////////////////////////////////////////////////////////////////////////////////
    // mat3

    // TODO

    //////////////////////////////////////////////////////////////////////////////////////////
    // mat4

    // mat4, data sizes
    {
        vmath::mat4 m;

        static_assert(mstd::array_size(m.data) == 16);
        static_assert(sizeof(m) == 64);
    }

    // mat4(float*)
    {
        const float raw[] = { 10, 20, 30, 40,
                              11, 21, 31, 41,
                              12, 22, 32, 42,
                              13, 23, 33, 43 };

        const vmath::mat4 m{raw};

        TEST(m.a00 == 10);
        TEST(m.a10 == 20);
        TEST(m.a20 == 30);
        TEST(m.a30 == 40);
        TEST(m.a01 == 11);
        TEST(m.a11 == 21);
        TEST(m.a21 == 31);
        TEST(m.a31 == 41);
        TEST(m.a02 == 12);
        TEST(m.a12 == 22);
        TEST(m.a22 == 32);
        TEST(m.a32 == 42);
        TEST(m.a03 == 13);
        TEST(m.a13 == 23);
        TEST(m.a23 == 33);
        TEST(m.a33 == 43);

        TEST(m.data[0] == 10);
        TEST(m.data[1] == 20);
        TEST(m.data[2] == 30);
        TEST(m.data[3] == 40);
        TEST(m.data[4] == 11);
        TEST(m.data[5] == 21);
        TEST(m.data[6] == 31);
        TEST(m.data[7] == 41);
        TEST(m.data[8] == 12);
        TEST(m.data[9] == 22);
        TEST(m.data[10] == 32);
        TEST(m.data[11] == 42);
        TEST(m.data[12] == 13);
        TEST(m.data[13] == 23);
        TEST(m.data[14] == 33);
        TEST(m.data[15] == 43);
    }

    // mat4::identity()
    {
        const vmath::mat4 m = vmath::mat4::identity();

        TEST(m.data[0] == 1);
        TEST(m.data[1] == 0);
        TEST(m.data[2] == 0);
        TEST(m.data[3] == 0);

        TEST(m.data[4] == 0);
        TEST(m.data[5] == 1);
        TEST(m.data[6] == 0);
        TEST(m.data[7] == 0);

        TEST(m.data[8] == 0);
        TEST(m.data[9] == 0);
        TEST(m.data[10] == 1);
        TEST(m.data[11] == 0);

        TEST(m.data[12] == 0);
        TEST(m.data[13] == 0);
        TEST(m.data[14] == 0);
        TEST(m.data[15] == 1);
    }

    // mat4::set_identity()
    {
        vmath::mat4 m;
        m.set_identity();

        TEST(m.data[0] == 1);
        TEST(m.data[1] == 0);
        TEST(m.data[2] == 0);
        TEST(m.data[3] == 0);

        TEST(m.data[4] == 0);
        TEST(m.data[5] == 1);
        TEST(m.data[6] == 0);
        TEST(m.data[7] == 0);

        TEST(m.data[8] == 0);
        TEST(m.data[9] == 0);
        TEST(m.data[10] == 1);
        TEST(m.data[11] == 0);

        TEST(m.data[12] == 0);
        TEST(m.data[13] == 0);
        TEST(m.data[14] == 0);
        TEST(m.data[15] == 1);
    }

    // mat4::operator*
    {
        const float raw1[] = { 2, 3, 5, 7,
                               11, 13, 17, 19,
                               23, 29, 31, 37,
                               41, 43, 47, 53 };

        const float raw2[] = { 59, 61, 67, 71,
                               73, 79, 83, 89,
                               97, 101, 103, 107,
                               109, 113, 127, 131 };

        const vmath::mat4 m1{raw1};
        const vmath::mat4 m2{raw2};

        const vmath::mat4 m3 = m1 * m2;

        TEST(m3.data[0] == 5241);
        TEST(m3.data[1] == 5966);
        TEST(m3.data[2] == 6746);
        TEST(m3.data[3] == 7814);
        TEST(m3.data[4] == 6573);
        TEST(m3.data[5] == 7480);
        TEST(m3.data[6] == 8464);
        TEST(m3.data[7] == 9800);
        TEST(m3.data[8] == 8061);
        TEST(m3.data[9] == 9192);
        TEST(m3.data[10] == 10424);
        TEST(m3.data[11] == 12080);
        TEST(m3.data[12] == 9753);
        TEST(m3.data[13] == 11112);
        TEST(m3.data[14] == 12560);
        TEST(m3.data[15] == 14552);
    }

    // operator*(vec4, mat4)
    {
        const float raw1[] = { 59, 61, 67, 71 };

        const float raw2[] = { 2, 3, 5, 7,
                               11, 13, 17, 19,
                               23, 29, 31, 37,
                               41, 43, 47, 53 };

        const vmath::vec4 v{raw1};
        const vmath::mat4 m{raw2};

        const vmath::vec4 result = v * m;

        TEST(result.x == 1133);
        TEST(result.y == 3930);
        TEST(result.z == 7830);
        TEST(result.w == 11954);
    }

    // operator*(mat4, vec4)
    {
        const float raw1[] = { 59, 61, 67, 71 };

        const float raw2[] = { 2, 11, 23, 41,
                               3, 13, 29, 43,
                               5, 17, 31, 47,
                               7, 19, 37, 53 };

        const vmath::vec4 v{raw1};
        const vmath::mat4 m{raw2};

        const vmath::vec4 result = m * v;

        TEST(result.x == 1133);
        TEST(result.y == 3930);
        TEST(result.z == 7830);
        TEST(result.w == 11954);
    }

    // translate(x, y, z)
    {
        const vmath::mat4 m = vmath::translate(4, 5, 6);

        TEST(m.a00 == 1);
        TEST(m.a01 == 0);
        TEST(m.a02 == 0);
        TEST(m.a03 == 0);
        TEST(m.a10 == 0);
        TEST(m.a11 == 1);
        TEST(m.a12 == 0);
        TEST(m.a13 == 0);
        TEST(m.a20 == 0);
        TEST(m.a21 == 0);
        TEST(m.a22 == 1);
        TEST(m.a23 == 0);
        TEST(m.a30 == 4);
        TEST(m.a31 == 5);
        TEST(m.a32 == 6);
        TEST(m.a33 == 1);
    }

    // translate(vec3)
    {
        const vmath::mat4 m = vmath::translate(vmath::vec3{6, 7, 8});

        TEST(m.a00 == 1);
        TEST(m.a01 == 0);
        TEST(m.a02 == 0);
        TEST(m.a03 == 0);
        TEST(m.a10 == 0);
        TEST(m.a11 == 1);
        TEST(m.a12 == 0);
        TEST(m.a13 == 0);
        TEST(m.a20 == 0);
        TEST(m.a21 == 0);
        TEST(m.a22 == 1);
        TEST(m.a23 == 0);
        TEST(m.a30 == 6);
        TEST(m.a31 == 7);
        TEST(m.a32 == 8);
        TEST(m.a33 == 1);
    }

    // scale(x, y, z)
    {
        const vmath::mat4 m = vmath::scale(4, 5, 6);

        TEST(m.a00 == 4);
        TEST(m.a01 == 0);
        TEST(m.a02 == 0);
        TEST(m.a03 == 0);
        TEST(m.a10 == 0);
        TEST(m.a11 == 5);
        TEST(m.a12 == 0);
        TEST(m.a13 == 0);
        TEST(m.a20 == 0);
        TEST(m.a21 == 0);
        TEST(m.a22 == 6);
        TEST(m.a23 == 0);
        TEST(m.a30 == 0);
        TEST(m.a31 == 0);
        TEST(m.a32 == 0);
        TEST(m.a33 == 1);
    }

    // scale(vec3)
    {
        const vmath::mat4 m = vmath::scale(vmath::vec3{6, 7, 8});

        TEST(m.a00 == 6);
        TEST(m.a01 == 0);
        TEST(m.a02 == 0);
        TEST(m.a03 == 0);
        TEST(m.a10 == 0);
        TEST(m.a11 == 7);
        TEST(m.a12 == 0);
        TEST(m.a13 == 0);
        TEST(m.a20 == 0);
        TEST(m.a21 == 0);
        TEST(m.a22 == 8);
        TEST(m.a23 == 0);
        TEST(m.a30 == 0);
        TEST(m.a31 == 0);
        TEST(m.a32 == 0);
        TEST(m.a33 == 1);
    }

    // projection
    {
        const vmath::mat4 m = vmath::projection(1, vmath::pi_half, 1, 5);

        TEST(is_near(m.a00, 1, 0.005f));
        TEST(m.a01 == 0);
        TEST(m.a02 == 0);
        TEST(m.a03 == 0);
        TEST(m.a10 == 0);
        TEST(is_near(m.a11, 1, 0.005f));
        TEST(m.a12 == 0);
        TEST(m.a13 == 0);
        TEST(m.a20 == 0);
        TEST(m.a21 == 0);
        TEST(is_near(m.a22, -0.25f));
        TEST(m.a23 == 1);
        TEST(m.a30 == 0);
        TEST(m.a31 == 0);
        TEST(is_near(m.a32, 5.0f / 4.0f));
        TEST(m.a33 == 0);
    }

    // projection_vector
    {
        const vmath::vec4 v = vmath::projection_vector(1, vmath::pi_half, 1, 5);

        TEST(is_near(v.x, 1, 0.005f));
        TEST(is_near(v.y, 1, 0.005f));
        TEST(is_near(v.z, -0.25f));
        TEST(is_near(v.w, 5.0f / 4.0f));
    }

    return exit_code;
}
