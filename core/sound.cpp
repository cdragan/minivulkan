// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "minivulkan.h"
#include "mstdc.h"
#include "realtime_synth.h"
#include "vmath.h"
#include "vecfloat.h"

#include <iterator>

#ifdef __APPLE__
#   define NEED_WAV_HEADER 1
#else
#   define NEED_WAV_HEADER 0
#endif

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

struct StereoSample {
    SoundSampleType left;
    SoundSampleType right;
};

struct WAVFileStereo {
#if NEED_WAV_HEADER
    WAVHeader    header;
#endif
    StereoSample data[1];
};

static constexpr uint16_t num_channels = 2;

#if NEED_WAV_HEADER
static const WAVHeader wav_header = {
    { 'R', 'I', 'F', 'F' },
    0,
    { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' },
    16,
    sample_format,
    num_channels,
    Synth::rt_sampling_rate,
    (Synth::rt_sampling_rate * num_channels * bits_per_sample) / 8,
    (num_channels * bits_per_sample) / 8,
    bits_per_sample,
    { 'd', 'a', 't', 'a' },
    0
};
#endif

bool init_sound()
{
    constexpr uint64_t duration_ms  = 50;
    constexpr uint32_t frequency_hz = 440; // A

    constexpr uint32_t total_samples = static_cast<uint32_t>((duration_ms * Synth::rt_sampling_rate) / 1000u);
    constexpr uint32_t data_size     = total_samples * num_channels * (bits_per_sample / 8u);
    constexpr uint32_t wav_hdr_size  = NEED_WAV_HEADER ? static_cast<uint32_t>(sizeof(WAVHeader)) : 0u;
    constexpr uint32_t alloc_size    = wav_hdr_size + data_size;

    // TODO get this from synth
    static uint8_t audio_buf[alloc_size];

    WAVFileStereo& wav_file = *reinterpret_cast<WAVFileStereo*>(audio_buf);

    #if NEED_WAV_HEADER
    mstd::mem_copy(audio_buf, &wav_header, sizeof(wav_header));

    wav_file.header.file_size = alloc_size - 8;
    wav_file.header.data_size = data_size;
    #endif

    constexpr float coeff = vmath::two_pi * frequency_hz / Synth::rt_sampling_rate;

    // TODO use synth to generate the wave
    for (uint32_t i = 0; i < total_samples; i++) {
        const vmath::sin_cos_result sc = vmath::sincos(static_cast<float>(i) * coeff);

        SoundSampleType value;

        if constexpr (sample_format == sample_pcm) {
            value = static_cast<int16_t>(sc.cos * 0.001f * 32767);
        }
        else {
            value = sc.cos * 0.001f;
        }

        StereoSample& out = wav_file.data[i];
        out.left  = value;
        out.right = value;
    }

    return load_sound_track(audio_buf, std::size(audio_buf));
}
