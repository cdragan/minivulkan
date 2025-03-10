// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "vecfloat_neon.h"
#include "vecfloat_sse.h"
#include "vecfloat_default.h"

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
