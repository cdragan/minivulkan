// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include <stdint.h>

#include "vulkan_functions.h"

#define DEFINE_SHADERS \
    X(bezier_line_cubic_sculptor_tesc) \
    X(bezier_line_cubic_sculptor_tese) \
    X(bezier_surface_cubic_sculptor_tesc) \
    X(bezier_surface_cubic_sculptor_tese) \
    X(bezier_surface_cubic_tesc) \
    X(bezier_surface_cubic_tese) \
    X(bezier_surface_quadratic_tesc) \
    X(bezier_surface_quadratic_tese) \
    X(mono_to_stereo_comp) \
    X(pass_through_vert) \
    X(phong_frag) \
    X(rounded_cube_vert) \
    X(sculptor_color_frag) \
    X(sculptor_object_frag) \
    X(sculptor_simple_vert) \
    X(simple_vert) \
    X(synth_comp)

#define X(shader) extern uint8_t shader_##shader[];
DEFINE_SHADERS
#undef X

VkShaderModule load_shader(uint8_t* shader);
