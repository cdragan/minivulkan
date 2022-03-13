// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "vulkan_functions.h"

#define DEFINE_SHADERS \
    X(simple_vert) \
    X(phong_frag) \
    X(pass_through_vert) \
    X(rounded_cube_vert) \
    X(bezier_surface_quadratic_tesc) \
    X(bezier_surface_quadratic_tese) \
    X(bezier_surface_cubic_tesc) \
    X(bezier_surface_cubic_tese)

#define X(shader) extern uint8_t shader_##shader[];
DEFINE_SHADERS
#undef X

VkShaderModule load_shader(uint8_t* shader);