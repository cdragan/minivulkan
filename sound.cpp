// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "minivulkan.h"
#include "mstdc.h"
#include "vmath.h"
#include "vecfloat.h"

struct WAVHeader {
    char     riff[4];           // "RIFF"
    uint32_t file_size;         // data_size + sizeof(WAVHeader) - 8
    char     wave_fmt[8];       // "WAVEfmt\0"
    uint32_t fmt_len;           // 16
    uint16_t type;              // 1 is PCM
    uint16_t num_channels;      // 2
    uint32_t rate;              // 44100
    uint32_t bytes_per_sec;     // 176400 = rate * bits_per_sample * num_channels / 8
    uint16_t bytes_per_sch;     // 4 = num_channels * bits_per_sample / 8
    uint16_t bits_per_sample;   // 16
    char     data_hdr[4];       // "data"
    uint32_t data_size;         // size of all data
};

static_assert(sizeof(WAVHeader) == 44);

struct Sample16Stereo {
    int16_t left;
    int16_t right;
};

struct WAVFile16Stereo {
    WAVHeader      header;
    Sample16Stereo data[1];
};

static constexpr uint32_t sampling_rate   = 44100;
static constexpr uint16_t num_channels    = 2;
static constexpr uint16_t bits_per_sample = 16;

static const WAVHeader wav_header = {
    { 'R', 'I', 'F', 'F' },
    0,
    { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' },
    16,
    1, // PCM
    num_channels,
    sampling_rate,
    (sampling_rate * num_channels * bits_per_sample) / 8,
    (num_channels * bits_per_sample) / 8,
    bits_per_sample,
    { 'd', 'a', 't', 'a' },
    0
};

bool init_sound()
{
    constexpr uint64_t duration_ms  = 50;
    constexpr uint32_t frequency_hz = 440; // A

    constexpr uint32_t total_samples = static_cast<uint32_t>((duration_ms * sampling_rate) / 1000u);
    constexpr uint32_t data_size     = total_samples * num_channels * (bits_per_sample / 8u);
    constexpr uint32_t alloc_size    = sizeof(wav_header) + data_size;

    static uint8_t audio_buf[alloc_size];

    mstd::mem_copy(audio_buf, &wav_header, sizeof(wav_header));

    WAVFile16Stereo& wav_file = *reinterpret_cast<WAVFile16Stereo*>(audio_buf);
    wav_file.header.file_size = alloc_size - 8;
    wav_file.header.data_size = data_size;

    constexpr float coeff = vmath::two_pi * frequency_hz / sampling_rate;

    for (uint32_t i = 0; i < total_samples; i++) {
        const vmath::sin_cos_result sc = vmath::sincos(static_cast<float>(i) * coeff);
        const int16_t value = static_cast<int16_t>(sc.cos * 0.001f * 32767);

        Sample16Stereo& out = wav_file.data[i];
        out.left  = value;
        out.right = value;
    }

    return load_sound(0, audio_buf, mstd::array_size(audio_buf));
}
