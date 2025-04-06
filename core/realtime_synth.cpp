// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "realtime_synth.h"
#include "d_printf.h"
#include "minivulkan.h"
#include "mstdc.h"
#include "resource.h"
#include "vmath.h"
#include <math.h>

#include "synth_shaders.h"
#include "shaders.h"

namespace {
    // Host buffer which is filled with generated audio data
    Buffer host_audio_output_buf;

    // Vulkan command buffer used for the synth
    CommandBuffers<1> audio_cmd_buf;

    // Number of samples rendered in one step.
    // This is also how frequently LFOs and ADSR envelopes are updated.
    // This must match workgroup geometry in compute shaders.
    constexpr uint32_t rt_step_samples = 256;

    // Maximum number of supported voices
    constexpr uint32_t max_voices = 256;

    // Number of samples which have been rendered since playback started.
    uint32_t rendered_samples;

    // Actual tempo converted to samples
    uint32_t samples_per_midi_tick = 0;

    // Stores current per-channel time measured in samples
    uint32_t channel_samples[Synth::max_channels];

    // Saved state of event decode, per-channel
    uint8_t events_decode_state[Synth::max_channels];

    // Map notes in each note in each channel to voices
    typedef uint8_t NoteToVoice[128];
    NoteToVoice note_to_voice[Synth::max_channels];

    // Maximum number of parameters per voice channel (single instrument note)
    constexpr uint32_t max_parameters = 16;

    // Fixed-point constants, parameters use fixed-point numbers with 32768 corresponding to 1.0
    constexpr int32_t fxp_one = 32768;  // 1.0
    constexpr int32_t fxp_pi  = 102944; // pi
    constexpr int32_t fxp_2pi = 205887; // 2.0 * pi

    enum Parameters {
        param_cur_amplitude
    };

    // Voice is a single playing note of a single instrument
    struct Voice {
        bool    active;
        uint8_t channel;
        uint8_t instrument;
        int32_t parameters[max_parameters];
    };

    Voice voices[max_voices];
}

namespace Synth {
    bool init_synth_os();
}

bool Synth::init_synth()
{
    if (compute_family_index == no_queue_family) {
        d_printf("No async compute queue available for synth\n");
        return false;
    }

    if ( ! init_synth_os())
        return false;

    // TODO - use project-dependent audio length
    constexpr uint32_t seconds = 1;
    constexpr uint32_t sample_size = sizeof(float);
    constexpr VkDeviceSize buf_size = mstd::align_up(Synth::rt_sampling_rate * 2U * sample_size * seconds, rt_step_samples);

    if ( ! host_audio_output_buf.allocate(Usage::host_only,
                                          buf_size,
                                          VK_FORMAT_UNDEFINED,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          { "host audio buffer" }))
        return false;

    if ( ! allocate_command_buffers_once(&audio_cmd_buf, 1, compute_family_index))
        return false;

    load_shader(shader_synth_wave_gen_comp);

    return true;
}

enum WaveType : uint32_t {
    // Wave disabled - for WaveState that can only be wave_type[1]
    no_wave,
    // Normal sine wave, duty is ignored
    sine_wave,
    // Sawtooth wave
    //
    // |\  duty=0    /\ duty=0.5           duty=1  /|
    // | \          /  \ (triangle wave)          / |
    // |  \             \  /                     /  |
    // |   \             \/                     /   |
    sawtooth_wave,
    // Pulse wave
    //
    // | duty=0   +--+ duty=0.5        duty=1 |
    // |          |  | (square wave)          |
    // |             |  |                     |
    // +---          +--+              -------+
    pulse_wave,
    // White noise
    noise_wave
};

using ParamIndex = uint32_t;

struct WaveState {
    WaveType   wave_type[2];
    ParamIndex duty[2];     // Duty for sawtooth or pulse
    ParamIndex wave_mix;    // Amount of mixing between 2 waves
    ParamIndex amplitude;   // Wave amplitude
    ParamIndex pitch_adj;   // Pitch adjustment
    float      pitch_base;  // Base pitch from note
    float      phase;       // Current phase
};

enum ParamType : uint32_t {
    const_param,            // Constant, fixed value
    adsr_param,             // Envelope
    lfo_param               // Low frequency oscillator
};

struct ParamState {
    ParamIndex param;       // Index of global parameter description/config
    float      value;       // Current value (e.g. LFO)
    float      phase;       // Current phase step for ADSR/LFO
};

struct Parameter {
    ParamType param_type;
    union {
        float        const_value;
        struct {
            uint32_t duration_ms[4]; // Attack, decay, sustain, release
            float    value[4];       // Init, peak, sustain, end
        } adsr;
        struct {
            WaveType wave_type;
            float    duty;
            uint32_t freq_hz;
            float    value[2];  // Min, max
        } lfo;
    } param;
};

// TODO filter

// Template boilerplate to support different output audio buffer types,
// for example float non-interleaved or int16_t interleaved.

template<typename T, bool interleaved>
struct StereoPtr {
    T* left;
    T* right;

    StereoPtr<T, interleaved>& operator+=(size_t offset) {
        left  += offset;
        right += offset;
        return *this;
    }

    static StereoPtr<T, interleaved> from_buffer(Buffer& buffer) {
        T* const ptr = buffer.get_ptr<T>();
        const size_t size = buffer.size();
        return { ptr, ptr + (size / (2 * sizeof(T))) };
    }
};

template<typename T>
static StereoPtr<T, false> operator+(StereoPtr<T, false> ptr, size_t offset)
{
    return { ptr.left + offset, ptr.right + offset };
}

template<typename T>
struct StereoPtr<T, true> {
    T* data;

    StereoPtr<T, true>& operator+=(size_t offset) {
        data += offset;
        return *this;
    }

    static StereoPtr<T, true> from_buffer(Buffer& buffer) {
        return { buffer.get_ptr<T>() };
    }
};

template<typename T>
static StereoPtr<T, true> operator+(StereoPtr<T, true> ptr, size_t offset)
{
    return { ptr.data + offset };
}

static void copy_audio_data(void* dest, const void* src, size_t size)
{
    mstd::mem_copy(dest, src, static_cast<uint32_t>(size));
}

template<typename T>
static void copy_audio_data(StereoPtr<T, false> dest, StereoPtr<T, false> src, uint32_t num_samples)
{
    copy_audio_data(dest.left,  src.left,  num_samples * sizeof(T));
    copy_audio_data(dest.right, src.right, num_samples * sizeof(T));
}

template<typename T>
static void copy_audio_data(StereoPtr<T, true> dest, StereoPtr<T, true> src, uint32_t num_samples)
{
    copy_audio_data(dest.data, src.data, num_samples * 2 * sizeof(T));
}

static uint32_t allocate_unused_voice()
{
    for (uint32_t i = 1; i < max_voices; i++) {
        if ( ! voices[i].active) {
            assert(voices[i].parameters[param_cur_amplitude] == 0);
            return i;
        }
    }

    return 0;
}

static uint8_t select_instrument(uint32_t channel, uint32_t note)
{
    uint32_t instr_idx;

    for (instr_idx = 0; instr_idx < Synth::max_instr_per_channel; instr_idx++) {
        const uint32_t start_note = Synth::instr_routing[channel].note_routing[instr_idx].start_note;
        if (note < start_note || ! start_note) {
            if (instr_idx)
                --instr_idx;
            break;
        }
    }

    if (instr_idx == Synth::max_instr_per_channel)
        --instr_idx;

    return Synth::instr_routing[channel].note_routing[instr_idx].instrument;
}

static bool get_next_midi_event(Synth::MidiEvent* event, uint32_t end_samples)
{
    static uint32_t last_channel;
    uint32_t        channel = last_channel;

    for (;;) {
        const uint8_t* encoded_delta_time = Synth::midi_delta_times[channel];

        uint32_t delta_time = *(encoded_delta_time++);
        if (delta_time > 0x7Fu) {
            assert(*encoded_delta_time <= 0x7Fu);
            delta_time = ((delta_time & 0x7Fu) << 7) | *(encoded_delta_time++);
        }

        const uint32_t delta_samples = delta_time * samples_per_midi_tick;
        const uint32_t event_samples = channel_samples[channel] + delta_samples;

        const uint32_t end_of_channel = 0x3FFFu;

        if (event_samples < end_samples && delta_time < end_of_channel) {
            last_channel                     = channel;
            channel_samples[channel]         = event_samples;
            Synth::midi_delta_times[channel] = encoded_delta_time;
            event->time                      = event_samples;
            event->channel                   = static_cast<uint8_t>(channel);
            break;
        }

        channel = (channel + 1) % Synth::num_channels;
        if (channel == last_channel)
            return false;
    }

    const uint8_t* const encoded_event_ptr = Synth::midi_events[channel];
    uint8_t              event_code        = *encoded_event_ptr;
    uint8_t              event_state       = events_decode_state[channel];

    Synth::midi_events[channel] = encoded_event_ptr + event_state;

    event_state ^= 1u;
    events_decode_state[channel] = event_state;

    event_code = (event_code >> (event_state * 7u)) & 0xFu;

    event->event = static_cast<Synth::EvType>(event_code);

    if (event_code <= static_cast<uint8_t>(Synth::EvType::aftertouch)) {
        event->note      = *(Synth::midi_notes[channel]++);
        event->note_data = *(Synth::midi_note_data[channel]++);
    }
    else if (static_cast<Synth::EvType>(event_code) == Synth::EvType::controller) {
        event->controller      = *(Synth::midi_ctrl[channel]++);
        event->controller_data = *(Synth::midi_ctrl_data[channel]++);
    }
    else {
        assert(event_code == static_cast<uint8_t>(Synth::EvType::pitch_bend));
        const int16_t lo = *(Synth::midi_pitch_bend_lo[channel]++);
        const int16_t hi = *(Synth::midi_pitch_bend_hi[channel]++);
        assert(lo <= 0x7F);
        assert(hi <= 0x7F);
        event->pitch_bend = static_cast<int16_t>((hi << 7) + lo - 0x2000);
    }

    return true;
}

static void process_note_off(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    assert(voice_idx);
    assert(voices[voice_idx].active);

    // TODO transition voice to decay state
}

static void process_note_on(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    uint32_t       voice_idx = note_to_voice[channel][note];
    int32_t        amplitude = 0;

    if ( ! voice_idx) {

        voice_idx = allocate_unused_voice();

        if ( ! voice_idx) {
            d_printf("All voices are active, dropping note %u on channel %u\n", note, channel);
            return;
        }

        note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);
    }
    else {
        assert(voices[voice_idx].channel    == channel);
        assert(voices[voice_idx].instrument == select_instrument(channel, note));

        amplitude = voices[voice_idx].parameters[param_cur_amplitude];
    }

    voices[voice_idx].channel    = static_cast<uint8_t>(channel);
    voices[voice_idx].instrument = select_instrument(channel, note);

    memset(&voices[voice_idx].parameters, 0, sizeof(voices[voice_idx].parameters));

    // Preserve amplitude if the same note is replayed
    voices[voice_idx].parameters[param_cur_amplitude] = amplitude;

    // TODO set velocity
}

static void process_aftertouch(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    assert(voice_idx);
    assert(voices[voice_idx].active);

    // TODO apply aftertouch
}

static void process_controller(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // TODO
}

static void process_pitch_bend(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // TODO
}

static void process_events(uint32_t start_samples, uint32_t end_samples)
{
    using EventHandler = void (*)(uint32_t delta_samples, const Synth::MidiEvent& event);

    // These two MIDI events are unused and thus are unsupported
    constexpr EventHandler process_program_change   = nullptr;
    constexpr EventHandler process_channel_pressure = nullptr;

    static const EventHandler event_handlers[] = {
        #define X(name) process_##name,
        MIDI_EVENT_TYPES(X)
        #undef X
    };

    Synth::MidiEvent event;

    while (get_next_midi_event(&event, end_samples)) {

        const uint32_t delta_samples = (event.time >= start_samples) ? (event.time - start_samples) : 0;

        assert(static_cast<uint32_t>(event.event) < mstd::array_size(event_handlers));

        const EventHandler handler = event_handlers[static_cast<uint8_t>(event.event)];

        handler(delta_samples, event);
    }
}

static void render_waveforms()
{
    // TODO
}

static void render_audio_step()
{
    const uint32_t start_samples = rendered_samples;
    const uint32_t end_samples   = start_samples + rt_step_samples;

    rendered_samples = end_samples;

    process_events(start_samples, end_samples);

    //update_lfos();

    //update_filters();

    render_waveforms();

    //apply_waveform_filters();

    //add_waveforms_to_channel();

    //apply_channel_filters();

    //add_channels();

    //apply_master_filters();
}

template<typename T, bool interleaved>
static void copy_audio_step_to_host(uint32_t offset)
{
    // TODO vkCmdCopyBuffer or vkCmdDispatch
    const auto buf_ptr = StereoPtr<T, interleaved>::from_buffer(host_audio_output_buf);
    for (uint32_t i = 0; i < rt_step_samples; i++) {
        static float phase = 0;
        static const float frequency = 440.0f;

        buf_ptr.left[offset + i]  = sinf(phase);
        buf_ptr.right[offset + i] = sinf(phase + vmath::pi);

        phase += 2.0f * vmath::pi * frequency / Synth::rt_sampling_rate;

        if (phase > 2.0f * vmath::pi)
            phase -= 2.0f * vmath::pi;
    }
}

template<typename T, bool interleaved>
static bool render_audio(uint32_t num_samples)
{
    assert(num_samples % rt_step_samples == 0 && num_samples > 0);

    if ( ! reset_and_begin_command_buffer(audio_cmd_buf))
        return false;

    for (uint32_t offset = 0; offset < num_samples; offset += rt_step_samples) {
        render_audio_step();

        copy_audio_step_to_host<T, interleaved>(offset);
    }

    if ( ! send_to_device_and_wait(audio_cmd_buf, vk_compute_queue, fen_compute))
        return false;

    host_audio_output_buf.invalidate();

    return true;
}

template<typename T, bool interleaved>
static bool render_audio_buffer(StereoPtr<T, interleaved> output_buf, uint32_t num_samples)
{
    static uint32_t consumed_samples;
    static uint32_t remaining_samples;

    const auto rendered_src = StereoPtr<T, interleaved>::from_buffer(host_audio_output_buf);

    if (remaining_samples) {
        const uint32_t to_copy = mstd::min(remaining_samples, num_samples);

        copy_audio_data(output_buf, rendered_src + consumed_samples, to_copy);

        output_buf        += to_copy;
        num_samples       -= to_copy;
        remaining_samples -= to_copy;
        consumed_samples  += to_copy;

        if (remaining_samples)
            return true;
    }

    const uint32_t to_render = mstd::align_up(num_samples, rt_step_samples);
    if ( ! render_audio<T, interleaved>(to_render))
        return false;

    const uint32_t to_copy = mstd::min(to_render, num_samples);
    copy_audio_data(output_buf, rendered_src, to_copy);

    if (to_render > to_copy) {
        consumed_samples  = to_copy;
        remaining_samples = to_render - to_copy;
    }

    return true;
}

bool Synth::render_audio_buffer(uint32_t num_frames,
                                float*   left_channel,
                                float*   right_channel)
{
    if (num_frames) {
        const StereoPtr<float, false> channels = { left_channel, right_channel };
        return render_audio_buffer(channels, num_frames);
    }

    return true;
}
