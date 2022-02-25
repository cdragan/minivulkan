// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "vecfloat_sse.h"

namespace vmath {

struct sin_cos_result4 {
    float4 sin;
    float4 cos;
};

sin_cos_result4 sincos(float4 radians);

struct sin_cos_result {
    float sin;
    float cos;
};

sin_cos_result sincos(float radians);

} // namespace vmath
