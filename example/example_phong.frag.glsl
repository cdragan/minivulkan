// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

layout(location = 0) in  vec3 in_pos;
layout(location = 1) in  vec3 in_normal;

layout(location = 0) out vec4 out_color;

#include "ubo_data.glsl"

float calculate_lighting(vec3 light_pos, vec3 surface_pos, vec3 surface_normal)
{
    const float shininess      = 16;
    const float ambient        = 0.1;
    const vec3  light_dir      = light_pos - surface_pos;
    const float diffuse        = max(0, dot(surface_normal, normalize(light_dir)));
    float       specular       = 0;

    // Blinn-Phong specular
    if (diffuse > 0) {
        const vec3  view_dir = normalize(-surface_pos); // assuming eye is at [0,0,0] in view space

        const vec3  half_dir = normalize(light_dir + view_dir);
        const float spec_cos = max(0, dot(half_dir, surface_normal));
        specular             = pow(spec_cos, shininess);
    }

    return ambient + diffuse + specular;
}

void main()
{
    vec3 lit_color = ubo.color.xyz;

    if (num_lights > 0) {

        float attenuation = 0;

        const vec3 n_normal = normalize(in_normal);

        for (uint i = 0; i < num_lights; i++) {
            attenuation += calculate_lighting(ubo.lights[i].xyz, in_pos, n_normal);
        }

        lit_color = lit_color * attenuation;
    }

    const float gamma = 2.2;
    out_color = vec4(pow(lit_color, vec3(1 / gamma)), 1);
}
