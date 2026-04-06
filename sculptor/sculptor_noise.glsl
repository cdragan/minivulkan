// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

uint pcg_hash(uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint pcg_hash_2d(ivec2 coord, uint seed)
{
    return pcg_hash(uint(coord.x) + pcg_hash(uint(coord.y) + seed));
}

float pcg_hash_2d_float(ivec2 coord, uint seed)
{
    return float(pcg_hash_2d(coord, seed)) / 4294967295.0;
}

vec2 hash_2d_gradient(ivec2 coord, uint seed)
{
    const uint  hash  = pcg_hash_2d(coord, seed);
    const float angle = float(hash) * (6.28318530718 / 4294967295.0);
    return vec2(cos(angle), sin(angle));
}

float value_noise_single(vec2 coord, uint seed)
{
    const ivec2 icoord     = ivec2(floor(coord));
    const vec2  frac_coord = fract(coord);
    const vec2  amount     = frac_coord * frac_coord * (3.0 - 2.0 * frac_coord);

    const float a = pcg_hash_2d_float(icoord,               seed);
    const float b = pcg_hash_2d_float(icoord + ivec2(1, 0), seed);
    const float c = pcg_hash_2d_float(icoord + ivec2(0, 1), seed);
    const float d = pcg_hash_2d_float(icoord + ivec2(1, 1), seed);

    return mix(mix(a, b, amount.x), mix(c, d, amount.x), amount.y);
}

float perlin_noise_single(vec2 coord, uint seed)
{
    const ivec2 icoord     = ivec2(floor(coord));
    const vec2  frac_coord = fract(coord);
    const vec2  amount     = frac_coord * frac_coord * frac_coord * (frac_coord * (frac_coord * 6.0 - 15.0) + 10.0);

    const float a = dot(hash_2d_gradient(icoord,               seed), frac_coord);
    const float b = dot(hash_2d_gradient(icoord + ivec2(1, 0), seed), frac_coord - vec2(1, 0));
    const float c = dot(hash_2d_gradient(icoord + ivec2(0, 1), seed), frac_coord - vec2(0, 1));
    const float d = dot(hash_2d_gradient(icoord + ivec2(1, 1), seed), frac_coord - vec2(1, 1));

    return mix(mix(a, b, amount.x), mix(c, d, amount.x), amount.y) * 0.5 + 0.5;
}

float fractal_brownian_motion(vec2 coord, float freq, float amp, uint octaves, uint seed, bool use_perlin)
{
    float result    = 0.0;
    float total_amp = 0.0;

    for (uint i = 0; i < octaves; i++) {
        float n;
        if (use_perlin)
            n = perlin_noise_single(coord * freq, seed + i * 31u);
        else
            n = value_noise_single(coord * freq, seed + i * 31u);
        result    += n * amp;
        total_amp += amp;
        freq      *= 2.0;
        amp       *= 0.5;
    }

    return result / total_amp;
}

float voronoi_noise(vec2 coord, float freq, uint seed, uint cell_mode)
{
    coord *= freq;
    const ivec2 icoord = ivec2(floor(coord));
    const vec2  frac_coord = fract(coord);

    float min_dist     = 1e10;
    ivec2 closest_cell = ivec2(0);

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            const ivec2 neighbor = ivec2(x, y);
            const ivec2 cell     = icoord + neighbor;
            const vec2  point    = vec2(neighbor) + vec2(pcg_hash_2d_float(cell, seed),
                                                         pcg_hash_2d_float(cell, seed + 1u)) - frac_coord;
            const float dist     = dot(point, point);
            if (dist < min_dist) {
                min_dist     = dist;
                closest_cell = cell;
            }
        }
    }

    if (cell_mode == 0u)
        return sqrt(min_dist);
    else
        return pcg_hash_2d_float(closest_cell, seed + 2u);
}
