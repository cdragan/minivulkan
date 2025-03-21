// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>

static constexpr uint32_t rt_sampling_rate = 44100;

bool init_real_time_synth();

void render_audio_buffer(uint32_t num_frames,
                         float*   left_channel,
                         float*   right_channel);
