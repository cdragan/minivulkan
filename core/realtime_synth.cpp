// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include "realtime_synth.h"
#include "d_printf.h"
#include "minivulkan.h"
#include "mstdc.h"
#include "resource.h"
#include "suballoc.h"
#include "vecfloat.h"
#include "vmath.h"
#include <math.h>

#include "synth_shaders.h"
#include "shaders.h"

namespace {
    enum BufferTypes: uint8_t {
        data_buf,   // Device buffer which is used by effects etc.
        param_buf,  // Dynamic device buffer with parameters for compute shaders etc.
        output_buf, // Host buffer which is filled with generated audio data
        num_buf_types
    };

    Buffer buffers[num_buf_types];

    // Vulkan command buffer used for the synth
    CommandBuffers<1> audio_cmd_buf;

    // Number of samples rendered in one step.
    // This is also how frequently LFOs and ADSR envelopes are updated.
    // This must match workgroup geometry in compute shaders.
    constexpr uint32_t rt_step_samples = 256;

    // Maximum number of supported voices
    constexpr uint32_t max_voices = 256;

    // Number of FIR filter taps
    constexpr uint32_t num_fir_taps = 1025;

    // Smooth volume adjustment to avoid glitches
    constexpr uint32_t volume_adjustment_samples = 32;

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

    enum WaveType : uint32_t {
        // Wave disabled
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

    // Encoded envelope:
    // u8 N     - number of points
    // f32      - min value
    // f32      - max value
    // u8       - point index of sustain loop begin, >=N means no sustain
    // u8       - point index of sustain end loop (can be same as sustain loop begin)
    // u8[N]    - low byte of next point's delta position, each position is expressed in ticks (256 samples)
    // u8[N]    - low byte of next point's delta value
    // u8[N]    - high byte of next point's delta position (unsigned)
    // u8[N]    - high byte of next point's delta value (signed, but uses zig-zag encoding)
    //
    // Delta values are encoded using zig-zag encoding (0->0, -1->1, 1->2, -2->3, 2->4, etc.).
    //
    // Volume envelope:
    // - Value is in decibels
    // - Value 0 means nominal attenuation
    // - Point at position 0 and last point should indicate lowest possible negative value (no sound)
    struct EnvelopeDescriptor {
        uint8_t num_points;         // Number of points in the envelope
        uint8_t unused_alignment;
        uint8_t sustain_first_point;// Index if sustain loop start point
        uint8_t sustain_last_point; // Index of last point of sustain loop (can be same as first point)
        float   min_value;          // Minimum value produced by the envelope
        float   min_max_delta;      // Delta between minimum and maximum value produced

        struct Point {
            uint16_t position;      // Number of ticks since the beginning of the envelope
            uint16_t value;         // Value at this position (0=min_value, 0xFFFF=min_value+min_max_delta)
        };

        Point points[1];
    };
    EnvelopeDescriptor envelope_descs[10];

    struct LFODescriptor {
        uint8_t  wave;              // LFO wave type
        uint8_t  duty;              // Duty for sawtooth wave (0=left, 0x7F=triangle, 0xFF=right)
        uint16_t period_ms;         // Period of the LFO, in milliseconds
        float    min_value;         // Minimum value produced by the LFO
        float    min_max_delta;     // Delta between minimum and maximum value produced
    };
    LFODescriptor lfo_descs[10];

    struct ParameterDescriptor {
        float    base_value;        // Initial base value (can be overridden from MIDI)
        uint32_t envelope_desc_id;  // Id of envelope descriptor
        uint32_t lfo_desc_id;       // Id of LFO descriptor
    };
    ParameterDescriptor param_descs[10];

    struct Parameter {
        float    cur_value;         // Current value of the parameter
        uint16_t param_desc_id;     // Id of parameter descriptor
        uint16_t lfo_tick;          // Current LFO tick of this parameter
        uint16_t envelope_tick;     // Current envelope tick
        uint16_t envelope_point;    // Current point on the envelope
        uint8_t  voice_id;          // Voice id where this parameter is playing
    };
    Parameter params[10];

    void advance_param(uint32_t param_id)
    {
        Parameter& param = params[param_id];

        float value = 0;

        if (param.param_desc_id) {

            const ParameterDescriptor& desc = param_descs[param.param_desc_id - 1];

            value = desc.base_value; // TODO use value from MIDI, if available

            if (desc.envelope_desc_id) {
                const EnvelopeDescriptor& envelope = envelope_descs[desc.envelope_desc_id - 1];

                constexpr bool sustain = false; // TODO take from voice

                uint32_t env_point = param.envelope_point;
                uint32_t env_tick  = param.envelope_tick;

                // Apply current position of the envelope
                const EnvelopeDescriptor::Point pt1 = envelope.points[env_point];

                int env_value = pt1.value;

                if (env_tick > pt1.position) {
                    assert(env_point + 1 < envelope.num_points);

                    const EnvelopeDescriptor::Point pt2 = envelope.points[env_point + 1];

                    const int delta_pos = static_cast<int>(env_tick - pt1.position);
                    const int duration  = static_cast<int>(pt2.position - pt1.position);
                    const int range     = static_cast<int>(pt2.value) - env_value;

                    env_value = (delta_pos * range) / duration;
                }

                value += envelope.min_value + static_cast<float>(env_value) * envelope.min_max_delta;

                for (;;) {
                    // Advance envelope
                    if ( ! sustain || env_point < envelope.sustain_last_point) {

                        const uint32_t next_point = env_point + 1;
                        if (next_point < envelope.num_points) {
                            ++env_tick;

                            // Advance envelope point
                            const uint32_t next_tick  = envelope.points[next_point].position;
                            if (env_tick == next_tick)
                                env_point = next_point;
                        }
                        else {
                            assert(env_tick == envelope.points[envelope.num_points - 1].position);
                        }
                    }
                    // Apply sustain loop
                    else {
                        assert(env_point == envelope.sustain_last_point);

                        if (env_point == envelope.sustain_first_point) {
                            assert(env_tick == envelope.points[env_point].position);
                        }
                        else {
                            assert(env_tick == envelope.points[env_point].position);
                            env_point = envelope.sustain_first_point;
                            env_tick  = envelope.points[env_point].position;

                            // Loop back and try to advance to next tick from envelope start
                            continue;
                        }
                    }
                    break;
                }

                param.envelope_point = static_cast<uint16_t>(env_point);
                param.envelope_tick  = static_cast<uint16_t>(env_tick);
            }

            if (desc.lfo_desc_id) {

                const LFODescriptor& lfo = lfo_descs[desc.lfo_desc_id - 1];

                value += lfo.min_value;

                const float phase = static_cast<float>(param.lfo_tick) * static_cast<float>(rt_step_samples * 1000) /
                                    static_cast<float>(lfo.period_ms * Synth::rt_sampling_rate);

                switch (lfo.wave) {
                    case sine_wave:
                        {
                            const float sval = vmath::sincos(phase * vmath::two_pi).sin;

                            value += (sval + 1.0f) * 0.5f * lfo.min_max_delta;
                        }
                        break;

                    case sawtooth_wave:
                        {
                            const float frac_phase = phase - truncf(phase);
                            const float duty       = static_cast<float>(lfo.duty) / 255.0f;

                            if (frac_phase <= duty) {
                                value += lfo.min_max_delta * frac_phase / duty;
                            }
                            else {
                                value += lfo.min_max_delta * (1.0f - frac_phase) / (1.0f - duty);
                            }
                        }
                        break;

                    default:
                        assert(lfo.wave == sine_wave || lfo.wave == sawtooth_wave);
                        break;
                }

                ++param.lfo_tick;
            }
        }

        param.cur_value = value;
    }

    namespace ShaderParams {

        // Note: These defintions must match the structs inside the shaders

        // synth_fir_coeff shader
        struct FIRCoeff {
            uint32_t taps_offs;
            uint32_t highpass_cutoff_freq;
            uint32_t lowpass_cutoff_freq;
        };

        // synth_oscillator shader
        struct Oscillator {
            uint32_t out_sound_offs;
            float    phase;
            float    phase_step;
            WaveType osc_type[2];
            float    duty[2];
            float    osc_mix;

            // Optional filter parameters
            uint32_t fir_memory_offs;
            uint32_t taps_offs;
        };

        constexpr uint32_t max_param_range = sizeof(Oscillator) * 256;

        // synth_chan_combine shader (binding 0)
        struct ChannelCombineInput {
            uint32_t in_sound_offs;
            float    old_volume;
            float    volume;
            float    old_panning;
            float    panning;
        };

        // synth_chan_combine shader (binding 1)
        struct ChannelCombine {
            uint32_t out_sound_offs;
            uint32_t input_params_offs;
            uint32_t num_inputs;
        };

        // synth_output_* shaders
        struct OutputPushConst {
            uint32_t in_sound_offs;
        };
    }

    enum SynthPipelines {
        oscillator_pipe,
        chan_combine_pipe,
        output_16i_pipe,
        output_32fi_pipe,
        output_32f_pipe,
        num_synth_pipes
    };

    VkPipelineLayout pipe_layouts[num_synth_pipes];
    VkPipeline       pipes[num_synth_pipes];

    enum DescSetTypes: uint8_t {
        one_buffer_ds,
        two_buffers_ds,
        one_double_buffer_ds,
        num_desc_set_layouts
    };

    VkDescriptorSetLayout desc_set_layouts[num_desc_set_layouts];

    bool create_shaders()
    {
        static const DescSetBindingInfo bindings[] = {
            { one_buffer_ds,        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { one_buffer_ds,        1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { two_buffers_ds,       0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { two_buffers_ds,       1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { two_buffers_ds,       2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { one_double_buffer_ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
            { one_double_buffer_ds, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
            { num_desc_set_layouts, 0, 0,                                 0 }
        };

        if ( ! create_compute_descriptor_set_layouts(bindings,
                                                     num_desc_set_layouts,
                                                     desc_set_layouts))
            return false;

        struct ShaderInfo {
            ComputeShaderInfo shader_info;
            DescSetTypes      desc_set;
        };

        static const ShaderInfo shaders[] = {
            {
                {
                    shader_synth_oscillator_comp,
                    0,
                },
                one_buffer_ds
            },
            {
                {
                    shader_synth_chan_combine_comp,
                    0,
                },
                two_buffers_ds
            },
            // TODO load only in builds which need it
            {
                {
                    shader_synth_output_16_interlv_comp,
                    1,
                },
                one_buffer_ds
            },
            // TODO load only in builds which need it
            {
                {
                    shader_synth_output_f32_interlv_comp,
                    1,
                },
                one_buffer_ds
            },
            // TODO load only in builds which need it
            {
                {
                    shader_synth_output_f32_separate_comp,
                    1,
                },
                one_double_buffer_ds
            }
        };

        assert(mstd::array_size(shaders) == mstd::array_size(pipes));
        assert(mstd::array_size(pipes) == num_synth_pipes);

        for (uint32_t i = 0; i < num_synth_pipes; i++) {

            if (!shaders[i].shader_info.shader)
                continue;

            const VkDescriptorSetLayout ds_layouts[] = {
                desc_set_layouts[shaders[i].desc_set],
                VK_NULL_HANDLE // list terminator
            };

            static const VkSpecializationMapEntry map_entries[] = {
                { 0, 0, 4 },
                { 1, 4, 4 },
                { 2, 8, 4 },
            };

            static const uint32_t spec_data[] = {
                rt_step_samples,
                num_fir_taps,
                volume_adjustment_samples,
            };

            static const VkSpecializationInfo spec_constants = {
                mstd::array_size(map_entries),
                map_entries,
                sizeof(spec_data),
                &spec_data
            };

            if ( ! create_compute_shader(shaders[i].shader_info,
                                         ds_layouts,
                                         &spec_constants,
                                         &pipe_layouts[i],
                                         &pipes[i]))
                return false;
        }

        return true;
    }

    SubAllocator<1024> data_allocator;

    SubAllocator<1> param_allocator;

    size_t synth_alignment;

    template<typename T>
    T& get_param(uint32_t offset)
    {
        return *buffers[param_buf].get_ptr<T>(offset);
    }

    struct Oscillator {
        // Constants which don't change for this oscillator's instance's life time
        // TODO move some of these to OscillatorDescriptor
        uint32_t midi_channel;      // MIDI channel on which this note was played
        uint32_t output_channel;    // Output (mixing) channel for this MIDI channel/note
        uint32_t note;              // MIDI note
        uint32_t freq_mult;         // Frequency multiplier for component frequencies (1 for base frequency)
        WaveType osc_type[2];       // Two oscillator types
        uint32_t osc_output_offs;   // Oscillator data output offset
        uint32_t fir_memory_offs;   // FIR filter memory offset
        uint32_t fir_taps_offs;     // FIR filter taps offset

        // Current values
        float    phase;             // Current position of the oscillator
        float    old_volume;        // Previous volume
        float    old_panning;       // Previous panning

        // Values from LFOs, envelopes, notes and instrument constants
        // TODO convert these to param ids
        float    volume;            // Current volume
        float    panning;           // Current panning
        float    pitch;             // Pitch adjustment in semitones = (midi_pitch_bend - 8192) / 4096
        float    duty[2];           // Duty cycle for sawtooth and pulse oscillator (0..1)
        float    osc_mix;           // Mix between osc_type[0] and osc_type[1] (0..1)
    };
    static Oscillator oscillators[10];

    static constexpr uint32_t max_mix_channels = 8;

    struct Channel {
        uint32_t chan_output_offs;  // Channel data output offset
    };
    static Channel mix_channels[max_mix_channels];
}

namespace Synth {
    bool init_synth_os();
}

static void temp_init_osc_and_channel()
{
    mix_channels[0].chan_output_offs = static_cast<uint32_t>(data_allocator.allocate(sizeof(float) * rt_step_samples * 2, synth_alignment).offset);

    Oscillator& osc = oscillators[0];
    osc.note            = 69; // 69=A4
    osc.freq_mult       = 1;
    osc.osc_type[0]     = sine_wave;
    osc.osc_output_offs = static_cast<uint32_t>(data_allocator.allocate(sizeof(float) * rt_step_samples, synth_alignment).offset);
    osc.volume          = 1.0;
    osc.panning         = 0.5;
}

bool Synth::init_synth()
{
    if (compute_family_index == no_queue_family) {
        d_printf("No async compute queue available for synth\n");
        return false;
    }

    // TODO - use project-dependent audio length
    constexpr uint32_t seconds = 1;
    constexpr uint32_t sample_size = sizeof(float);
    constexpr VkDeviceSize output_buf_size = mstd::align_up(Synth::rt_sampling_rate * 2U * sample_size * seconds, rt_step_samples);
    constexpr VkDeviceSize device_buf_size = 1024 * 1024; // TODO
    constexpr VkDeviceSize param_buf_size  = 1024 * 1024; // TODO

    if ( ! buffers[output_buf].allocate(Usage::host_only,
                                        output_buf_size,
                                        VK_FORMAT_UNDEFINED,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        { "host audio buffer" }))
        return false;

    if ( ! buffers[data_buf].allocate(Usage::device_only,
                                      device_buf_size,
                                      VK_FORMAT_UNDEFINED,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      { "audio work buffer" }))
        return false;

    data_allocator.init(device_buf_size);

    if ( ! buffers[param_buf].allocate(Usage::dynamic,
                                       param_buf_size,
                                       VK_FORMAT_UNDEFINED,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       { "audio param buffer" }))
        return false;

    if ( ! create_shaders())
        return false;

    if ( ! allocate_command_buffers_once(&audio_cmd_buf, 1, compute_family_index))
        return false;

    synth_alignment = static_cast<size_t>(vk_phys_props.properties.limits.minMemoryMapAlignment);

    temp_init_osc_and_channel();

    if ( ! init_synth_os())
        return false;

    return true;
}

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

    // TODO transition voice to release state
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

    mstd::mem_zero(&voices[voice_idx].parameters, sizeof(voices[voice_idx].parameters));

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
        assert(handler);

        handler(delta_samples, event);
    }
}

static void memory_barrier(VkAccessFlags        dst_access,
                           VkPipelineStageFlags dst_stage)
{
    static VkAccessFlags        src_access = 0;
    static VkPipelineStageFlags src_stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkMemoryBarrier barrier = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        nullptr,
        0,
        0
    };

    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;

    vkCmdPipelineBarrier(audio_cmd_buf,
                         src_stage,
                         dst_stage,
                         0,             // dependencyFlags
                         1,             // memoryBarrierCount
                         &barrier,
                         0,             // bufferMemoryBarrierCount
                         nullptr,       // pBufferMemoryBarriers
                         0,             // imageMemoryBarrierCount
                         nullptr);      // pImageMemoryBarriers

    src_access = dst_access;
    src_stage  = dst_stage;
}

struct PushDescriptorInfo {
    uint8_t      pipeline_layout;
    uint8_t      binding;
    uint8_t      array_element;
    uint8_t      buffer_idx;
    VkDeviceSize buffer_range;
};

static void push_descriptor(const PushDescriptorInfo& info, uint32_t buffer_offset)
{
    static VkDescriptorBufferInfo buffer_info = {
        VK_NULL_HANDLE, // buffer
        0,              // offset
        0               // range
    };

    static VkWriteDescriptorSet write_desc_set = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        nullptr,
        VK_NULL_HANDLE,                     // dstSet
        0,                                  // dstBinding
        0,                                  // dstArrayElement
        1,                                  // descriptorCount
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
        nullptr,                            // pImageInfo
        &buffer_info,                       // pBufferInfo
        nullptr                             // pTexelBufferView
    };

    buffer_info.buffer = buffers[info.buffer_idx].get_buffer();
    buffer_info.offset = buffer_offset;
    buffer_info.range  = info.buffer_range;

    write_desc_set.dstBinding      = info.binding;
    write_desc_set.dstArrayElement = info.array_element;

    vkCmdPushDescriptorSet(audio_cmd_buf,
                           VK_PIPELINE_BIND_POINT_COMPUTE,
                           pipe_layouts[info.pipeline_layout],
                           0,
                           1,
                           &write_desc_set);
}

static void render_audio_step()
{
    const uint32_t start_samples = rendered_samples;
    const uint32_t end_samples   = start_samples + rt_step_samples;

    // TODO move this to the end or outside of this function
    rendered_samples = end_samples;

    process_events(start_samples, end_samples);

    // TODO update_lfos();

    // ======================================================================

#if 0
    if ( ! num_filters) {
        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // TODO update_filters();
    }
#endif

    // ======================================================================

    uint32_t num_oscillators  = 0;
    uint32_t num_mix_channels = 0;
    uint32_t channel_osc_count[max_mix_channels] = { };

    for (const Oscillator& oscillator : oscillators) {
        if ( ! oscillator.osc_type[0])
            continue;

        ++num_oscillators;

        // Channel 0 is master channel
        // TODO uncomment
#if 0
        assert(oscillator.output_channel);
#else
        assert( ! oscillator.output_channel);
#endif

        if ( ! channel_osc_count[oscillator.output_channel]++)
            ++num_mix_channels;
    }

    if ( ! num_oscillators) {
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        vkCmdFillBuffer(audio_cmd_buf,
                        buffers[data_buf].get_buffer(),
                        mix_channels[0].chan_output_offs,
                        sizeof(float) * 2 * rt_step_samples,
                        0); // data
        return;
    }

    // ======================================================================

    const uint32_t osc_base_param_size = num_oscillators * static_cast<uint32_t>(sizeof(ShaderParams::Oscillator));
    const uint32_t osc_base_param_offs = static_cast<uint32_t>(param_allocator.allocate(osc_base_param_size, synth_alignment).offset);
    uint32_t cur_param_offs = osc_base_param_offs;

    for (Oscillator& oscillator : oscillators) {
        if ( ! oscillator.osc_type[0])
            continue;

        ShaderParams::Oscillator& param = get_param<ShaderParams::Oscillator>(cur_param_offs);

        const float note_pitch  = static_cast<float>(static_cast<int>(oscillator.note) - 69);
        const float note_freq   = (oscillator.freq_mult * 440.0f) * mstd::exp2((note_pitch + oscillator.pitch) / 12.f);
        const float phase_step  = (static_cast<float>(rt_step_samples) * note_freq) / static_cast<float>(Synth::rt_sampling_rate);

        param.out_sound_offs  = oscillator.osc_output_offs / 4;
        param.phase           = oscillator.phase;
        param.phase_step      = phase_step / static_cast<float>(rt_step_samples);
        param.osc_type[0]     = oscillator.osc_type[0];
        param.osc_type[1]     = oscillator.osc_type[1];
        param.duty[0]         = oscillator.duty[0];
        param.duty[1]         = oscillator.duty[1];
        param.osc_mix         = oscillator.osc_mix;
        param.fir_memory_offs = oscillator.fir_memory_offs / 4;
        param.taps_offs       = oscillator.fir_taps_offs   / 4;

        oscillator.phase += phase_step;

        cur_param_offs += static_cast<uint32_t>(sizeof(ShaderParams::Oscillator));
    }

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[oscillator_pipe]);

    static const PushDescriptorInfo push_osc_data = { oscillator_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_osc_data, 0);

    static const PushDescriptorInfo push_osc_param = { oscillator_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_osc_param, osc_base_param_offs);

    vkCmdDispatch(audio_cmd_buf, num_oscillators, 1, 1);

    // ======================================================================

    const uint32_t input_param_size = num_oscillators * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
    const uint32_t chan_param_size  = num_mix_channels * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine));
    const uint32_t input_param_offs = static_cast<uint32_t>(param_allocator.allocate(input_param_size, synth_alignment).offset);
    const uint32_t chan_param_offs  = static_cast<uint32_t>(param_allocator.allocate(chan_param_size, synth_alignment).offset);

    uint32_t chan_input_indices[max_mix_channels] = { };
    uint32_t chan_map[max_mix_channels]           = { };

    for (uint32_t input_idx = 0, used_chan_idx = 0, chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        if ( ! channel_osc_count[chan_idx])
            continue;

        assert(used_chan_idx < num_mix_channels);
        chan_map[used_chan_idx]           = chan_idx;
        chan_input_indices[used_chan_idx] = input_idx;

        input_idx += channel_osc_count[chan_idx];
        ++used_chan_idx;
    }

    uint32_t gen_chan_input_indices[max_mix_channels];
    mstd::mem_copy(gen_chan_input_indices, chan_input_indices, sizeof(chan_input_indices));

    for (Oscillator& oscillator : oscillators) {
        if ( ! oscillator.osc_type[0])
            continue;

        const uint32_t cur_param_idx = gen_chan_input_indices[oscillator.output_channel]++;
        cur_param_offs = input_param_offs + cur_param_idx * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
        ShaderParams::ChannelCombineInput& param = get_param<ShaderParams::ChannelCombineInput>(cur_param_offs);

        param.in_sound_offs = oscillator.osc_output_offs / 4;
        param.volume        = oscillator.volume;
        param.panning       = oscillator.panning;
        param.old_volume    = oscillator.old_volume;
        param.old_panning   = oscillator.old_panning;

        oscillator.old_volume  = oscillator.volume;
        oscillator.old_panning = oscillator.panning;
    }

    for (uint32_t used_chan_idx = 0; used_chan_idx < num_mix_channels; used_chan_idx++) {

        const uint32_t chan_idx = chan_map[used_chan_idx];

        cur_param_offs = chan_param_offs + used_chan_idx * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine));
        ShaderParams::ChannelCombine& param = get_param<ShaderParams::ChannelCombine>(cur_param_offs);

        param.out_sound_offs    = mix_channels[chan_idx].chan_output_offs / 4;
        param.input_params_offs = chan_input_indices[used_chan_idx];
        param.num_inputs        = channel_osc_count[chan_idx];
    }

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[chan_combine_pipe]);

    push_descriptor(push_osc_data, 0);

    static const PushDescriptorInfo push_comb_param0 = { chan_combine_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_comb_param0, input_param_offs);

    static const PushDescriptorInfo push_comb_param1 = { chan_combine_pipe, 2, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_comb_param1, chan_param_offs);

    vkCmdDispatch(audio_cmd_buf, num_mix_channels, 1, 1);

    // ======================================================================

    // TODO Apply channel effects

    // ======================================================================

    // TODO Sum all channels into master channel

    // ======================================================================

    // TODO Apply master effects

    memory_barrier(VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT);
}

template<typename T, bool interleaved>
void prepare_copy_audio_step_to_host(uint32_t offset);

template<>
void prepare_copy_audio_step_to_host<int16_t, true>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipes[output_16i_pipe]);

    offset *= 2 * sizeof(int16_t);

    static const PushDescriptorInfo push_out_data = { output_16i_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output = { output_16i_pipe, 1, 0, output_buf, sizeof(int16_t) * 2 * rt_step_samples };
    push_descriptor(push_out_output, offset);

    const ShaderParams::OutputPushConst push = { mix_channels[0].chan_output_offs };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_16i_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,               // offset
                       sizeof(push),    // size
                       &push);          // pValues
}

template<>
void prepare_copy_audio_step_to_host<float, true>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipes[output_32fi_pipe]);

    offset *= 2 * sizeof(float);

    static const PushDescriptorInfo push_out_data = { output_32fi_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output = { output_32fi_pipe, 1, 0, output_buf, sizeof(float) * 2 * rt_step_samples };
    push_descriptor(push_out_output, offset);

    const ShaderParams::OutputPushConst push = { mix_channels[0].chan_output_offs };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_32fi_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,               // offset
                       sizeof(push),    // size
                       &push);          // pValues
}

template<>
void prepare_copy_audio_step_to_host<float, false>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipes[output_32f_pipe]);

    offset *= sizeof(float);

    const uint32_t other_chan_offs = static_cast<uint32_t>(buffers[output_buf].size()) / 2 + offset;

    static const PushDescriptorInfo push_out_data = { output_32f_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output0 = { output_32f_pipe, 1, 0, output_buf, sizeof(float) * rt_step_samples };
    push_descriptor(push_out_output0, offset);

    static const PushDescriptorInfo push_out_output1 = { output_32f_pipe, 1, 1, output_buf, sizeof(float) * rt_step_samples };
    push_descriptor(push_out_output1, other_chan_offs);

    const ShaderParams::OutputPushConst push = { mix_channels[0].chan_output_offs };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_32f_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,               // offset
                       sizeof(push),    // size
                       &push);          // pValues
}

template<typename T, bool interleaved>
static bool render_audio(uint32_t num_samples)
{
    assert(num_samples % rt_step_samples == 0 && num_samples > 0);

    if ( ! reset_and_begin_command_buffer(audio_cmd_buf))
        return false;

    param_allocator.init(static_cast<uint32_t>(buffers[param_buf].size()));

    for (uint32_t offset = 0; offset < num_samples; offset += rt_step_samples) {
        render_audio_step();

        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        prepare_copy_audio_step_to_host<T, interleaved>(offset);

        vkCmdDispatch(audio_cmd_buf, 1, 1, 1);
    }

    memory_barrier(VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);

    buffers[param_buf].flush();

    if ( ! send_to_device_and_wait(audio_cmd_buf, vk_compute_queue, fen_compute))
        return false;

    buffers[output_buf].invalidate();

    return true;
}

template<typename T, bool interleaved>
static bool render_audio_buffer(StereoPtr<T, interleaved> stereo_ptr, uint32_t num_samples)
{
    static uint32_t consumed_samples;
    static uint32_t remaining_samples;

    const auto rendered_src = StereoPtr<T, interleaved>::from_buffer(buffers[output_buf]);

    if (remaining_samples) {
        const uint32_t to_copy = mstd::min(remaining_samples, num_samples);

        copy_audio_data(stereo_ptr, rendered_src + consumed_samples, to_copy);

        stereo_ptr        += to_copy;
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
    copy_audio_data(stereo_ptr, rendered_src, to_copy);

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
