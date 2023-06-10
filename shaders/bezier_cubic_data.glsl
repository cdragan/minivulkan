// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

struct face_data {
    uint material_id;
};

layout(set = 3, binding = 1) buffer faces_data {
    ivec4     tess_level;
    face_data faces[]; // Indexed with gl_PrimitiveID
};
