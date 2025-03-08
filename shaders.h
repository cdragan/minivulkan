// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

#include "vulkan_functions.h"

#define X(shader) extern uint8_t shader_##shader[];
DEFINE_SHADERS(X)
#undef X

VkShaderModule load_shader(uint8_t* shader);
