// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#version 460 core

#extension GL_GOOGLE_include_directive: require

#include "sculptor_noise.glsl"

layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0, rgba8) writeonly uniform image2DArray output_textures;

struct Modifier {
    uint  type;
    uint  enabled;
    uint  blend_mode;
    float blend_factor;
    float params[12];
};

const uint max_modifiers = 16u;

layout(binding = 1) readonly buffer modifier_buf { Modifier modifiers[]; };

layout(push_constant) uniform PushConstants {
    uint texture_index; // which texture to regenerate, or 0xFFFFFFFF for all
};

vec4 evaluate_modifier(Modifier modifier, vec2 uv)
{
    switch (modifier.type) {
        case 0u: // solid color
            return vec4(modifier.params[0],
                        modifier.params[1],
                        modifier.params[2],
                        1.0);

        case 1u: // value noise
            return vec4(vec3(fractal_brownian_motion(uv,
                                                     modifier.params[0],
                                                     modifier.params[1],
                                                     uint(modifier.params[2]),
                                                     uint(modifier.params[3]),
                                                     false)),
                        1.0);

        case 2u: // perlin noise
            return vec4(vec3(fractal_brownian_motion(uv,
                                                     modifier.params[0],
                                                     modifier.params[1],
                                                     uint(modifier.params[2]),
                                                     uint(modifier.params[3]),
                                                     true)),
                        1.0);

        case 3u: // voronoi
            return vec4(vec3(voronoi_noise(uv,
                                           modifier.params[0],
                                           uint(modifier.params[2]),
                                           uint(modifier.params[1]))),
                        1.0);

        case 4u: { // gradient
            const float angle = modifier.params[0];

            float t = dot(uv - 0.5, vec2(cos(angle), sin(angle))) + 0.5;
            t       = clamp(t, 0.0, 1.0);

            const vec3 start_color = vec3(modifier.params[1], modifier.params[2], modifier.params[3]);
            const vec3 end_color   = vec3(modifier.params[4], modifier.params[5], modifier.params[6]);

            return vec4(mix(start_color, end_color, t), 1.0);
        }

        case 5u: { // brick
            const float tile_x   = modifier.params[0];
            const float tile_y   = modifier.params[1];
            const float mortar_w = modifier.params[2];
            const vec3  mortar_c = vec3(modifier.params[3], modifier.params[4], modifier.params[5]);
            const vec3  brick_c  = vec3(modifier.params[6], modifier.params[7], modifier.params[8]);

            vec2        scaled      = uv * vec2(tile_x, tile_y);
            const float row         = floor(scaled.y);
            const float half_mortar = mortar_w * 0.5;

            // Offset every other row by half
            if (mod(row, 2.0) > 0.5)
                scaled.x += 0.5;

            const vec2 brick_uv = fract(scaled);

            if (brick_uv.x < half_mortar || brick_uv.x > 1.0 - half_mortar ||
                brick_uv.y < half_mortar || brick_uv.y > 1.0 - half_mortar)
                return vec4(mortar_c, 1.0);

            return vec4(brick_c, 1.0);
        }
    }

    return vec4(0);
}

vec4 blend(vec4 base, vec4 layer, uint mode, float factor)
{
    vec4 result;

    switch (mode) {
        case 0u: result = layer;        break; // normal (replace)
        case 1u: result = base * layer; break; // multiply
        case 2u: result = base + layer; break; // add
        case 3u: result = base - layer; break; // subtract
        case 4u: {                             // overlay
            const vec4 dark  = 2.0 * base * layer;
            const vec4 light = 1.0 - 2.0 * (1.0 - base) * (1.0 - layer);
            result = vec4(mix(dark, light, step(0.5, base)).rgb, 1.0);
            break;
        }
        case 5u: result = min(base, layer); break; // min
        case 6u: result = max(base, layer); break; // max
        default: result = layer;            break;
    }

    return mix(base, clamp(result, 0.0, 1.0), factor);
}

void main()
{
    const uvec3 gid     = gl_GlobalInvocationID;
    const uint  tex_idx = gid.z;

    // Skip if generating a single texture and this is not it
    if (texture_index != 0xFFFFFFFFu && tex_idx != texture_index)
        return;

    const ivec3 img_size = imageSize(output_textures);
    if (gid.x >= uint(img_size.x) || gid.y >= uint(img_size.y))
        return;

    const vec2 uv = (vec2(gid.xy) + 0.5) / vec2(img_size.xy);

    const uint base_idx = tex_idx * max_modifiers;

    vec4 result = vec4(0);
    bool first  = true;

    for (uint i = 0; i < max_modifiers; i++) {
        Modifier modifier = modifiers[base_idx + i];

        if (modifier.enabled == 0u)
            continue;

        const vec4 value = evaluate_modifier(modifier, uv);

        if (first) {
            result = value;
            first  = false;
        }
        else {
            result = blend(result, value, modifier.blend_mode, modifier.blend_factor);
        }
    }

    imageStore(output_textures, ivec3(gid.xy, tex_idx), result);
}
