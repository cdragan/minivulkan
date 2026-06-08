// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "realtime_synth.h"

#include "../core/d_printf.h"
#include "../core/minivulkan.h"
#include "../core/mstdc.h"
#include "../core/resource.h"
#include "../core/suballoc.h"
#include "synth_modulation.h"
#include <algorithm>
#include <iterator>
#include <math.h>

#include "synth_shaders.h"
#include "../core/shaders.h"

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

    // Polyphony limits
    constexpr uint32_t max_voices      = 64; // Max notes are playing
    constexpr uint32_t max_oscillators = 64; // Max oscillators are playing
    constexpr uint32_t max_unison      = 7;  // Max oscillators per note

    // Oscillator modes (must match osc_mode_* constants in synth_oscillator.comp.glsl)
    constexpr uint32_t osc_mode_blend     = 0;  // Mix osc_type[0] and osc_type[1] by osc_mix
    constexpr uint32_t osc_mode_fm        = 1;  // osc_type[0] is carrier, osc_type[1] is modulator
    constexpr uint32_t osc_mode_hard_sync = 2;  // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave

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

    // Per-channel MIDI expression state
    float channel_pitch_bend[Synth::max_channels];  // semitones
    float channel_mod_wheel[Synth::max_channels];   // 0..1
    float channel_pressure[Synth::max_channels];    // 0..1

    // TODO temporary test
    // LFO driving vibrato (pitch), depth scaled by the mod wheel.  Bipolar [-1, 1]
    // so the pitch swings symmetrically around the channel bend.  ~6 Hz.
    static const Synth::LFODescriptor vibrato_lfo = { Synth::sine_wave, 0, 167, -1.0f, 2.0f };

    // TODO temporary test
    // LFO driving tremolo (volume), depth scaled by pressure / aftertouch.  Unipolar
    // [0, 1] so the gain never inverts phase and never exceeds 1.  ~5 Hz.
    static const Synth::LFODescriptor tremolo_lfo = { Synth::sine_wave, 0, 200, 0.0f, 1.0f };

    // Maximum vibrato swing in semitones when the mod wheel is fully on
    constexpr float vibrato_depth_semitones = 0.5f;

    // Continuous LFO phase shared by all voices, advanced once per render step
    uint32_t modulation_lfo_tick;

    // Default pitch bend range in semitones (standard MIDI default is +/- 2)
    constexpr float default_pitch_bend_range_semitones = 2.0f;

    // MIDI continuous controller number for the modulation wheel
    constexpr uint32_t mod_wheel_cc = 1;

    // Maximum number of parameters per voice channel (single instrument note)
    constexpr uint32_t max_parameters = 16;

    // Voice is a single playing note of a single instrument
    struct Voice {
        bool     active;
        uint8_t  channel;
        uint8_t  instrument;
        uint8_t  osc_ids[max_unison];    // Oscillator slots owned by this voice
        uint8_t  osc_count;              // Number of live oscillator slots owned (0 = none)
        uint16_t volume_param_id;        // Per-voice volume parameter shared by all oscillators (0 = none)
        uint16_t pitch_param_id;         // Per-voice pitch parameter shared by all oscillators (0 = none)
        float    velocity;               // Note-on velocity, 0..1
        float    aftertouch;             // Per-note polyphonic aftertouch pressure, 0..1
        bool     releasing;              // True after note-off, until the volume envelope finishes
    };

    Voice voices[max_voices];

    using Synth::WaveType;
    using Synth::no_wave;
    using Synth::sine_wave;
    using Synth::LFODescriptor;
    using Synth::EnvelopeDescriptor;

    constexpr uint32_t max_envelope_points = 8;

    // Backing storage giving each envelope room for up to max_envelope_points
    // contiguous points (EnvelopeDescriptor itself ends in a flexible points[1]).
    struct StoredEnvelope {
        EnvelopeDescriptor        desc;
        EnvelopeDescriptor::Point extra_points[max_envelope_points - 1];
    };
    StoredEnvelope envelopes[4];

    LFODescriptor lfo_descs[10];

    // Raw MIDI input a parameter reads as an additive source
    enum MidiSource : uint8_t {
        midi_none,
        midi_pitch_bend
    };

    struct ParameterDescriptor {
        float      base_value;        // Initial base value (can be overridden from MIDI)
        uint32_t   envelope_desc_id;  // Id of envelope descriptor
        uint32_t   lfo_desc_id;       // Id of LFO descriptor
        MidiSource midi_source;       // Raw MIDI input added to the value
    };
    ParameterDescriptor param_descs[10];

    struct Parameter {
        bool                  active;           // True while this slot is in use
        float                 cur_value;        // Current value of the parameter
        uint16_t              param_desc_id;    // Id of parameter descriptor
        Synth::ParameterState state;            // Running modulation state (envelope and LFO ticks)
        uint8_t               voice_id;         // Voice id where this parameter is playing
    };

    // Per-voice parameter pool.  Slot 0 is a reserved sentinel (param id 0 = none).
    constexpr uint32_t max_params = max_voices * max_parameters;
    Parameter params[max_params];

    // Finds the first free slot (active == false) in a pool over [1, count),
    // skipping slot 0 which is a reserved sentinel.  Returns 0 if none is free.
    template<typename T>
    uint32_t allocate_unused_slot(T* pool, uint32_t count, bool T::* active)
    {
        for (uint32_t slot = 1; slot < count; slot++) {
            if ( ! (pool[slot].*active)) {
                return slot;
            }
        }

        return 0;
    }

    // Resolves the raw MIDI source for a parameter into an additive value.
    float eval_midi_source(const MidiSource midi_source, const Voice& owning_voice)
    {
        switch (midi_source) {
            case midi_pitch_bend:
                return channel_pitch_bend[owning_voice.channel];

            case midi_none:
                break;
        }

        return 0.0f;
    }

    void advance_param(const uint32_t param_id)
    {
        Parameter& param = params[param_id];

        if ( ! param.param_desc_id) {
            param.cur_value = 0;
            return;
        }

        const ParameterDescriptor& desc = param_descs[param.param_desc_id - 1];

        const EnvelopeDescriptor* envelope = nullptr;
        if (desc.envelope_desc_id) {
            envelope = &envelopes[desc.envelope_desc_id - 1].desc;
        }

        const LFODescriptor* lfo = nullptr;
        if (desc.lfo_desc_id) {
            lfo = &lfo_descs[desc.lfo_desc_id - 1];
        }

        const Voice& owning_voice = voices[param.voice_id];
        const bool   sustain      = owning_voice.active && ! owning_voice.releasing;
        const float  midi_value   = eval_midi_source(desc.midi_source, owning_voice);

        param.cur_value = Synth::eval_parameter(desc.base_value,
                                                envelope,
                                                sustain,
                                                lfo,
                                                midi_value,
                                                &param.state,
                                                rt_step_samples,
                                                Synth::rt_sampling_rate);
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
            uint32_t osc_mode;
            float    mod_ratio;      // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
            float    fm_index;
            float    mod_phase;
            float    mod_phase_step;

            // Optional filter parameters
            uint32_t fir_memory_offs;
            uint32_t taps_offs;
        };

        constexpr uint32_t max_param_range = sizeof(Oscillator) * 256;

        // synth_chan_combine shader (binding 0); the master-mix pass reuses the
        // same layout to describe each per-channel input it sums.
        struct ChannelCombineInput {
            uint32_t in_sound_offs;
            float    old_volume;
            float    volume;
            float    old_panning;
            float    panning;
        };

        // synth_chan_combine shader (binding 1); also describes the master-mix output.
        struct ChannelCombine {
            uint32_t out_sound_offs;
            uint32_t input_params_offs;
            uint32_t num_inputs;
        };

        // synth_effect shader (binding 1); one per effect instance in a wave, rebuilt
        // and uploaded every step.  params[] holds the effect's tweakable values;
        // state_offs points at its persistent state (delay lines etc.) in the device buffer.
        static constexpr uint32_t max_effect_param_floats = 5;
        struct EffectParams {
            uint32_t type;
            uint32_t sound_offs;
            uint32_t state_offs;
            uint32_t pad;
            float    params[max_effect_param_floats];
        };

        // synth_output_* shaders
        struct OutputPushConst {
            uint32_t in_sound_offs;
        };
    }

    enum SynthPipelines {
        oscillator_pipe,
        chan_combine_pipe,
        master_mix_pipe,
        effect_pipe,
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
            {
                {
                    shader_synth_master_mix_comp,
                    0,
                },
                two_buffers_ds
            },
            {
                {
                    shader_synth_effect_comp,
                    0,
                },
                one_buffer_ds
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

        assert(std::size(shaders) == std::size(pipes));
        assert(std::size(pipes) == num_synth_pipes);

        for (uint32_t i = 0; i < num_synth_pipes; i++) {

            if (!shaders[i].shader_info.shader)
                continue;

            const VkDescriptorSetLayout ds_layouts[] = {
                desc_set_layouts[shaders[i].desc_set],
                VK_NULL_HANDLE // list terminator
            };

            static const VkSpecializationMapEntry map_entries[] = {
                { 0, 0,  4 },
                { 1, 4,  4 },
                { 2, 8,  4 },
                { 3, 12, 4 },
                { 4, 16, 4 },
                { 5, 20, 4 },
                { 6, 24, 4 },
            };

            static uint32_t spec_data[] = {
                rt_step_samples,
                0,
                num_fir_taps,
                volume_adjustment_samples,
                Synth::effect_delay_max_samples,
                Synth::effect_chorus_max_samples,
                Synth::rt_sampling_rate,
            };
            spec_data[1] = vk11_props.subgroupSize;

            static const VkSpecializationInfo spec_constants = {
                std::size(map_entries),
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

    constexpr VkDeviceSize device_buf_size = 1024 * 1024; // TODO

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
        uint32_t osc_mode;          // osc_mode_blend, osc_mode_fm or osc_mode_hard_sync
        float    mod_ratio;         // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
        float    fm_index;          // FM modulation depth

        // Current values
        float    phase;             // Current position of the oscillator
        float    mod_phase;         // Current position of the FM modulator
        float    old_volume;        // Previous volume
        float    old_panning;       // Previous panning

        // Values from LFOs, envelopes, notes and instrument constants
        // TODO convert these to param ids
        float    volume;            // Current volume
        float    panning;           // Current panning
        float    pitch;             // Pitch adjustment in semitones = (midi_pitch_bend - 8192) / 4096
        float    detune;            // Per-oscillator pitch offset in semitones (instrument constant for this note's unison stack)
        float    duty[2];           // Duty cycle for sawtooth and pulse oscillator (0..1)
        float    osc_mix;           // Mix between osc_type[0] and osc_type[1] (0..1)
        uint8_t  voice_id;          // Voice which owns this oscillator (0 = none/free)
    };

    static Oscillator oscillators[max_oscillators];

    // TODO Runtime instrument definition.  Hardcoded for now; an editor will
    // populate these later.
    struct RuntimeInstrument {
        WaveType osc_type[2];               // osc_type[1] == no_wave means single oscillator
        float    duty[2];
        float    osc_mix;
        uint32_t osc_mode;                  // osc_mode_blend, osc_mode_fm or osc_mode_hard_sync
        float    mod_ratio;                 // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
        float    fm_index;                  // FM modulation depth
        uint32_t unison_count;              // Number of oscillator slots a note of this instrument uses (1 = mono)
        float    detune_semitones[max_unison]; // Per-oscillator pitch offset in semitones; entry [idx] applies to the idx-th allocated oscillator
    };
    // Index 0 is a plain mono sine.  Index 1 is a 7-voice supersaw with a
    // symmetric detune spread of about +/- 18 cents.  Index 2 is a sine-on-sine
    // FM voice (mod_ratio 2.0, fm_index 3.0).  Index 3 is a hard-sync voice:
    // a sine master sets the pitch and a sawtooth slave is synced at ratio 2.5.
    // Index 4 is a mellow piano-ish voice: a triangle (sawtooth at duty 0.5, so
    // soft harmonics) paired with the percussive piano envelope.  It is much
    // easier to listen to than the buzzy sawtooth/FM voices, so the effects
    // (chorus, reverb) are clearly audible on it.
    RuntimeInstrument instruments[5] = {
        { { sine_wave,     no_wave }, { 0.0f, 0.0f }, 0.0f, osc_mode_blend, 0.0f, 0.0f, 1, { 0.0f } },
        { { Synth::sawtooth_wave, no_wave }, { 0.0f, 0.0f }, 0.0f, osc_mode_blend, 0.0f, 0.0f, max_unison,
          { -0.18f, -0.12f, -0.06f, 0.0f, 0.06f, 0.12f, 0.18f } },
        { { sine_wave, sine_wave }, { 0.0f, 0.0f }, 0.0f, osc_mode_fm, 2.0f, 3.0f, 1, { 0.0f } },
        { { sine_wave, Synth::sawtooth_wave }, { 0.0f, 0.0f }, 0.0f, osc_mode_hard_sync, 2.5f, 0.0f, 1, { 0.0f } },
        { { Synth::sawtooth_wave, no_wave }, { 0.5f, 0.0f }, 0.0f, osc_mode_blend, 0.0f, 0.0f, 1, { 0.0f } }
    };

    static constexpr uint32_t max_mix_channels = Synth::max_channels;

    struct Channel {
        uint32_t chan_output_offs;  // Channel data output offset
        float    volume;            // Channel volume (linear), default 1.0
        float    panning;           // Channel pan: 0 = left, 0.5 = center, 1 = right
        float    old_volume;        // Previous step's volume, for 32-sample smoothing
        float    old_panning;       // Previous step's panning, for 32-sample smoothing
    };
    static Channel mix_channels[max_mix_channels];

    // Dedicated interleaved-stereo master output, summed from all channels
    uint32_t master_output_offs;

    constexpr uint32_t max_effects_per_chain = 4;

    struct EffectInstance {
        Synth::EffectType type;
        bool              enabled;
        float             params[ShaderParams::max_effect_param_floats];
        uint32_t          state_offs;   // byte offset into data_buf; 0 when the effect is stateless
    };

    struct EffectChain {
        uint32_t       num_effects;
        EffectInstance effects[max_effects_per_chain];
    };

    static EffectChain channel_chains[max_mix_channels];
    static EffectChain master_chain;

    // One-time zero-fill of the persistent device effect-state region, computed in
    // init_effects and recorded on the first render (no command buffer exists yet at
    // init time).  Bytes 0 means there is no state to clear.
    uint32_t effect_state_fill_offset;
    uint32_t effect_state_fill_bytes;

    // Allocates persistent device state for one chain's enabled effects, growing the
    // contiguous state region tracked by fill_offset / fill_bytes.
    static void init_chain_state(EffectChain* chain, uint32_t* fill_offset, uint32_t* fill_bytes)
    {
        for (uint32_t effect_idx = 0; effect_idx < chain->num_effects; effect_idx++) {
            EffectInstance& instance = chain->effects[effect_idx];

            const uint32_t num_state_floats = ( ! instance.enabled || instance.type == Synth::effect_none)
                                              ? 0
                                              : Synth::effect_state_floats(instance.type);
            if ( ! num_state_floats) {
                instance.state_offs = 0;
                continue;
            }

            const SubAllocatorBase::Chunk chunk =
                data_allocator.allocate(num_state_floats * sizeof(float), synth_alignment);
            assert(chunk.offset + chunk.size <= device_buf_size);

            instance.state_offs = static_cast<uint32_t>(chunk.offset);

            if ( ! *fill_bytes) {
                *fill_offset = instance.state_offs;
            }
            *fill_bytes = static_cast<uint32_t>(chunk.offset + chunk.size) - *fill_offset;
        }
    }

    static void init_effects()
    {
        // TODO TEMP compressor A/B scaffolding: channel 0 carries the compressor under
        // test, channel 1 is dry.  temp_drive_test_notes plays one channel at a time,
        // so alternating notes are heard back-to-back with and without compression.  No
        // chorus here on purpose: its LFO-modulated detune sounds like vibrato and would
        // muddy the comparison.  Replaced when a mixer GUI configures chains.
        //
        // Compressor params: threshold (linear), ratio, attack coeff, release coeff,
        // makeup gain.  attack/release are smoothing coefficients (closer to 1 is
        // slower); they are roughly 0.2 ms attack and 46 ms release at 44100 Hz, chosen
        // directly to avoid pulling in libc exp() on the host.  These are exaggerated
        // settings (very low threshold, high ratio) so the gain reduction is
        // unmistakable; musical values to restore once confirmed: threshold 0.3,
        // ratio 4.0, makeup 1.5.
        constexpr float compressor_threshold = 0.05f;
        constexpr float compressor_ratio     = 20.0f;
        constexpr float compressor_attack    = 0.9f;
        constexpr float compressor_release   = 0.9995f;
        constexpr float compressor_makeup    = 1.0f;
        channel_chains[0].num_effects = 1;
        channel_chains[0].effects[0]  = { Synth::effect_compressor, true,
            { compressor_threshold, compressor_ratio, compressor_attack, compressor_release, compressor_makeup }, 0 };

        // Channel 1 is the dry reference (no per-channel effects).
        channel_chains[1].num_effects = 0;

        // Master reverb applies equally to both channels, so it does not confound the
        // compressor A/B.
        master_chain.num_effects = 1;
        master_chain.effects[0]  = { Synth::effect_reverb, true, { 0.7f, 0.5f, 0.3f, 0.0f, 0.0f }, 0 };

        effect_state_fill_offset = 0;
        effect_state_fill_bytes  = 0;

        for (uint32_t channel = 0; channel < Synth::num_channels; channel++) {
            init_chain_state(&channel_chains[channel], &effect_state_fill_offset, &effect_state_fill_bytes);
        }

        init_chain_state(&master_chain, &effect_state_fill_offset, &effect_state_fill_bytes);
    }

    // Returns the n-th enabled, non-none effect of a chain, or nullptr.
    static const EffectInstance* nth_enabled_effect(const EffectChain& chain, uint32_t n)
    {
        uint32_t enabled_seen = 0;

        for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
            const EffectInstance& instance = chain.effects[effect_idx];

            if ( ! instance.enabled || instance.type == Synth::effect_none) {
                continue;
            }

            if (enabled_seen == n) {
                return &instance;
            }
            ++enabled_seen;
        }

        return nullptr;
    }

    static bool chain_has_enabled_effect(const EffectChain& chain)
    {
        return nth_enabled_effect(chain, 0) != nullptr;
    }
}

namespace Synth {
    bool init_synth_os();
    void stop_synth_os();
}

static void init_oscillator_buffers()
{
    assert(Synth::num_channels <= max_mix_channels);

    for (uint32_t channel = 0; channel < Synth::num_channels; channel++) {
        mix_channels[channel].chan_output_offs = static_cast<uint32_t>(data_allocator.allocate(sizeof(float) * rt_step_samples * 2, synth_alignment).offset);
        mix_channels[channel].volume      = 1.0f;
        mix_channels[channel].panning     = 0.5f;
        mix_channels[channel].old_volume  = 1.0f;
        mix_channels[channel].old_panning = 0.5f;
    }

    for (uint32_t osc_idx = 0; osc_idx < max_oscillators; osc_idx++) {
        oscillators[osc_idx].osc_output_offs = static_cast<uint32_t>(data_allocator.allocate(sizeof(float) * rt_step_samples, synth_alignment).offset);
    }

    master_output_offs = static_cast<uint32_t>(data_allocator.allocate(sizeof(float) * rt_step_samples * 2, synth_alignment).offset);

    init_effects();
}

static void init_modulation()
{
    // TODO TEMP test settings
    // Piano-ish volume envelope: a fast percussive attack then a long, roughly
    // exponential decay (approximated by piecewise-linear points) down to near
    // silence, held there while the key is down, then a short release.  The quick
    // attack and decaying tail make the effects (especially the reverb) easy to
    // hear.  Value 0xFFFF maps to gain 1.0 (min_max_delta = 1/65535); 1 tick ~ 6 ms.
    StoredEnvelope& volume_envelope = envelopes[0];
    volume_envelope.desc.num_points          = 7;
    volume_envelope.desc.unused_alignment    = 0;
    volume_envelope.desc.sustain_first_point = 5;
    volume_envelope.desc.sustain_last_point  = 5;
    volume_envelope.desc.min_value           = 0.0f;
    volume_envelope.desc.min_max_delta       = 1.0f / 65535.0f;
    volume_envelope.desc.points[0] = { 0,   0      };  // silent
    volume_envelope.desc.points[1] = { 1,   0xFFFF };  // fast attack (~6 ms)
    volume_envelope.desc.points[2] = { 12,  0xB000 };  // initial fast decay
    volume_envelope.desc.points[3] = { 45,  0x6000 };
    volume_envelope.desc.points[4] = { 120, 0x2000 };
    volume_envelope.desc.points[5] = { 210, 0x0800 };  // long tail to ~3% (sustain)
    volume_envelope.desc.points[6] = { 235, 0      };  // release (~145 ms)

    // TODO Parameter descriptor 0 (id 1): volume driven by the ADSR envelope, no LFO.
    param_descs[0].base_value       = 0.0f;
    param_descs[0].envelope_desc_id = 1;
    param_descs[0].lfo_desc_id      = 0;
    param_descs[0].midi_source      = midi_none;

    // TODO Parameter descriptor 1 (id 2): pitch driven by the MIDI pitch bend, no envelope or LFO.
    param_descs[1].base_value       = 0.0f;
    param_descs[1].envelope_desc_id = 0;
    param_descs[1].lfo_desc_id      = 0;
    param_descs[1].midi_source      = midi_pitch_bend;
}

static bool allocate_oscillators(uint8_t*                 osc_ids,
                                 uint32_t                 num_osc,
                                 uint32_t                 voice_idx,
                                 const RuntimeInstrument& instrument)
{
    uint32_t allocated = 0;

    for (uint32_t osc_idx = 1; osc_idx < max_oscillators && allocated < num_osc; osc_idx++) {
        if (oscillators[osc_idx].osc_type[0] == no_wave) {
            oscillators[osc_idx].voice_id    = static_cast<uint8_t>(voice_idx);
            oscillators[osc_idx].osc_type[0] = instrument.osc_type[0];
            osc_ids[allocated]               = static_cast<uint8_t>(osc_idx);
            ++allocated;
        }
    }

    if (allocated < num_osc) {
        for (uint32_t rollback_idx = 0; rollback_idx < allocated; rollback_idx++) {

            Oscillator& osc = oscillators[osc_ids[rollback_idx]];
            osc.voice_id    = 0;
            osc.osc_type[0] = no_wave;

            osc_ids[rollback_idx] = 0;
        }
        return false;
    }

    return true;
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

    init_oscillator_buffers();

    init_modulation();

    if ( ! init_synth_os())
        return false;

    return true;
}

void Synth::stop_synth()
{
    stop_synth_os();
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
    return allocate_unused_slot(voices, max_voices, &Voice::active);
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

    voices[voice_idx].releasing = true;
}

// TODO temporary test scaffolding: when not temp_no_force_instrument,
// process_note_on forces this instrument index so a chosen voice is audible.
// The temp note driver sets this only around its own note-ons.
constexpr uint8_t temp_no_force_instrument = 0xFF;
static uint8_t temp_force_instrument = temp_no_force_instrument;

// Returns a partially-allocated note to the free pool when allocation fails
// part-way through.  Frees any oscillator slots and parameters the voice has
// acquired and marks the voice free.  Only resources actually acquired
// (osc_count, the param ids) are touched, so it is safe at any failure point.
static void drop_voice(uint32_t voice_idx, uint32_t channel, uint32_t note)
{
    Voice& voice = voices[voice_idx];

    for (uint32_t unison_idx = 0; unison_idx < voice.osc_count; ++unison_idx) {
        Oscillator& osc = oscillators[voice.osc_ids[unison_idx]];
        osc.osc_type[0] = no_wave;
        osc.voice_id    = 0;
    }
    voice.osc_count = 0;

    if (voice.volume_param_id) {
        params[voice.volume_param_id].active = false;
        voice.volume_param_id = 0;
    }

    if (voice.pitch_param_id) {
        params[voice.pitch_param_id].active = false;
        voice.pitch_param_id = 0;
    }

    voice.active                 = false;
    note_to_voice[channel][note] = 0;
}

static void process_note_on(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    uint32_t       voice_idx = note_to_voice[channel][note];

    // Resolve the instrument once so the re-trigger match assert below and the
    // assignment agree.  temp_force_instrument is throwaway scaffolding
    // (see its definition).
    const uint8_t target_instrument = (temp_force_instrument != temp_no_force_instrument)
                                      ? temp_force_instrument
                                      : select_instrument(channel, note);

    if ( ! voice_idx) {

        voice_idx = allocate_unused_voice();

        if ( ! voice_idx) {
            d_printf("All voices are active, dropping note %u on channel %u\n", note, channel);
            return;
        }

        // A freshly allocated voice slot must own no oscillators.  Voice
        // finalization in update_modulation zeroes osc_count (and osc_ids) when
        // the last oscillator is freed, so the free pool only ever hands back
        // fully released slots.
        assert(voices[voice_idx].osc_count == 0);

        note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);
    }
    else {
        assert(voices[voice_idx].channel    == channel);
        assert(voices[voice_idx].instrument == target_instrument);
    }

    voices[voice_idx].channel    = static_cast<uint8_t>(channel);
    voices[voice_idx].instrument = target_instrument;
    voices[voice_idx].active     = true;

    const RuntimeInstrument& instrument = instruments[voices[voice_idx].instrument < std::size(instruments) ? voices[voice_idx].instrument : 0];

    // Number of oscillator slots this note uses (1 = mono).
    const uint32_t unison_count = instrument.unison_count;
    assert(unison_count >= 1 && unison_count <= max_unison);

    if ( ! voices[voice_idx].osc_count) {
        if ( ! allocate_oscillators(voices[voice_idx].osc_ids, unison_count, voice_idx, instrument)) {
            d_printf("All oscillators are active, dropping note %u on channel %u\n", note, channel);
            drop_voice(voice_idx, channel, note);
            return;
        }

        voices[voice_idx].osc_count = static_cast<uint8_t>(unison_count);
    }
    else {
        // Re-triggering a still-releasing voice reuses its existing oscillator
        // slots.  All of a voice's oscillators share ONE volume parameter, so
        // they cross the silence threshold in the same update_modulation pass
        // and are freed atomically (osc_count goes unison_count -> 0 within one
        // pass, never partial between steps).  Hence on reuse osc_count must
        // still equal the full unison_count.  A future change that breaks that
        // atomicity will trip this assert instead of silently corrupting.
        assert(voices[voice_idx].osc_count == unison_count);
    }

    for (uint32_t unison_idx = 0; unison_idx < voices[voice_idx].osc_count; ++unison_idx) {
        assert(unison_idx < max_unison);
        Oscillator& osc     = oscillators[voices[voice_idx].osc_ids[unison_idx]];
        osc.voice_id        = static_cast<uint8_t>(voice_idx);
        osc.midi_channel    = channel;
        osc.output_channel  = channel;
        osc.note            = note;
        osc.freq_mult       = 1;
        osc.osc_type[0]     = instrument.osc_type[0];
        osc.osc_type[1]     = instrument.osc_type[1];
        osc.duty[0]         = instrument.duty[0];
        osc.duty[1]         = instrument.duty[1];
        osc.osc_mix         = instrument.osc_mix;
        osc.osc_mode        = instrument.osc_mode;
        osc.mod_ratio       = instrument.mod_ratio;
        osc.fm_index        = instrument.fm_index;
        osc.phase           = 0.0f;
        osc.mod_phase       = 0.0f;
        osc.pitch           = 0.0f;
        osc.detune          = instrument.detune_semitones[unison_idx];
        osc.volume          = 0.0f;
        osc.old_volume      = 0.0f;         // start ramped up from silence to avoid a click
        osc.panning         = 0.5f;
        osc.old_panning     = 0.5f;
        osc.fir_memory_offs = 0;
        osc.fir_taps_offs   = 0;
    }

    voices[voice_idx].velocity   = static_cast<float>(event.note_data) / 127.0f;
    voices[voice_idx].releasing  = false;
    voices[voice_idx].aftertouch = 0.0f;

    // Allocate the per-voice parameters shared by all the voice's oscillators.
    // TODO turn this into a loop which walks all params
    if ( ! voices[voice_idx].volume_param_id) {
        const uint32_t allocated_param_id = allocate_unused_slot(params, max_params, &Parameter::active);

        if ( ! allocated_param_id) {
            d_printf("All parameters are active, dropping note %u on channel %u\n", note, channel);
            drop_voice(voice_idx, channel, note);
            return;
        }

        voices[voice_idx].volume_param_id = static_cast<uint16_t>(allocated_param_id);
        params[allocated_param_id].active = true;
    }

    if ( ! voices[voice_idx].pitch_param_id) {
        const uint32_t allocated_param_id = allocate_unused_slot(params, max_params, &Parameter::active);

        if ( ! allocated_param_id) {
            d_printf("All parameters are active, dropping note %u on channel %u\n", note, channel);
            drop_voice(voice_idx, channel, note);
            return;
        }

        voices[voice_idx].pitch_param_id  = static_cast<uint16_t>(allocated_param_id);
        params[allocated_param_id].active = true;
    }

    // ADSR volume descriptor (id 1)
    constexpr uint16_t volume_param_desc_id = 1;

    Parameter& volume_param    = params[voices[voice_idx].volume_param_id];
    volume_param.cur_value     = 0.0f;
    volume_param.param_desc_id = volume_param_desc_id;
    volume_param.state         = { };
    volume_param.voice_id      = static_cast<uint8_t>(voice_idx);

    // Pitch bend descriptor (id 2)
    constexpr uint16_t pitch_param_desc_id = 2;

    Parameter& pitch_param    = params[voices[voice_idx].pitch_param_id];
    pitch_param.cur_value     = 0.0f;
    pitch_param.param_desc_id = pitch_param_desc_id;
    pitch_param.state         = { };
    pitch_param.voice_id      = static_cast<uint8_t>(voice_idx);
}

static void process_aftertouch(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    assert(voice_idx);
    assert(voices[voice_idx].active);

    voices[voice_idx].aftertouch = static_cast<float>(event.note_data) / 127.0f;
}

static void process_controller(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // TODO Only the modulation wheel is supported for now; other controllers ignored.
    if (event.controller == mod_wheel_cc) {
        channel_mod_wheel[event.channel] = static_cast<float>(event.controller_data) / 127.0f;
    }
}

static void process_pitch_bend(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    channel_pitch_bend[event.channel] = Synth::pitch_bend_to_semitones(event.pitch_bend, default_pitch_bend_range_semitones);
}

static void process_channel_pressure(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // TODO get_next_midi_event does not yet decode real channel-pressure messages;
    // only the temp test driver populates note_data here.  Real-MIDI decode is
    // a follow-up (live/file MIDI input is currently out of scope).
    channel_pressure[event.channel] = static_cast<float>(event.note_data) / 127.0f;
}

static void process_events(uint32_t start_samples, uint32_t end_samples)
{
    using EventHandler = void (*)(uint32_t delta_samples, const Synth::MidiEvent& event);

    // Program change is unused and thus unsupported
    constexpr EventHandler process_program_change = nullptr;

    static const EventHandler event_handlers[] = {
        #define X(name) process_##name,
        MIDI_EVENT_TYPES(X)
        #undef X
    };

    Synth::MidiEvent event;

    while (get_next_midi_event(&event, end_samples)) {

        const uint32_t delta_samples = (event.time >= start_samples) ? (event.time - start_samples) : 0;

        assert(static_cast<uint32_t>(event.event) < std::size(event_handlers));
        const EventHandler handler = event_handlers[static_cast<uint8_t>(event.event)];
        assert(handler);

        handler(delta_samples, event);
    }
}

// TODO switch to buffer barriers
static void memory_barrier(VkAccessFlags        dst_access,
                           VkPipelineStageFlags dst_stage)
{
    static VkMemoryBarrier2 barrier = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        nullptr, // pNext
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE
    };

    barrier.dstStageMask  = dst_stage;
    barrier.dstAccessMask = dst_access;

    static const VkDependencyInfo dependency_info = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        nullptr,    // pNext
        0,          // dependecyFlags
        1,          // memoryBarrierCount,
        &barrier,   // pMemoryBarriers
        0,          // bufferMemoryBarrierCount
        nullptr,    // pBufferMemoryBarriers
        0,          // imageMemoryBarrierCount
        nullptr     // pImageMemoryBarriers
    };

    vkCmdPipelineBarrier2(audio_cmd_buf, &dependency_info);

    barrier.srcStageMask  = dst_stage;
    barrier.srcAccessMask = dst_access;
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

struct EffectTarget {
    const EffectChain* chain;
    uint32_t           sound_offs;   // FLOAT index of the buffer to process in place
};

// Runs the targets as dependency-depth waves: wave d = the d-th enabled effect of
// every target, packed into one dispatch; a barrier precedes each wave.  Chains are
// sequential per buffer, but the buffers are distinct, so one wave has no conflicts.
static void apply_effects(const EffectTarget* targets, uint32_t num_targets)
{
    for (uint32_t depth = 0; ; depth++) {

        // Gather the depth-th enabled effect of every target first, so the param
        // buffer is only allocated for a non-empty wave.  An empty depth means all
        // chains are exhausted and the pass is done.
        const EffectInstance* wave_instances[max_mix_channels];
        uint32_t              wave_sound_offs[max_mix_channels];
        uint32_t              wave_count = 0;

        for (uint32_t target_idx = 0; target_idx < num_targets; target_idx++) {

            const EffectInstance* const instance = nth_enabled_effect(*targets[target_idx].chain, depth);
            if ( ! instance) {
                continue;
            }

            wave_instances[wave_count]  = instance;
            wave_sound_offs[wave_count] = targets[target_idx].sound_offs;
            ++wave_count;
        }

        if ( ! wave_count) {
            return;
        }

        const uint32_t param_size = wave_count * static_cast<uint32_t>(sizeof(ShaderParams::EffectParams));
        const uint32_t param_offs = static_cast<uint32_t>(param_allocator.allocate(param_size, synth_alignment).offset);

        for (uint32_t wave_idx = 0; wave_idx < wave_count; wave_idx++) {

            const uint32_t cur_param_offs = param_offs + wave_idx * static_cast<uint32_t>(sizeof(ShaderParams::EffectParams));
            ShaderParams::EffectParams& param = get_param<ShaderParams::EffectParams>(cur_param_offs);

            param.type       = static_cast<uint32_t>(wave_instances[wave_idx]->type);
            param.sound_offs = wave_sound_offs[wave_idx];
            param.state_offs = wave_instances[wave_idx]->state_offs / 4;
            param.pad        = 0;
            mstd::mem_copy(param.params, wave_instances[wave_idx]->params, sizeof(param.params));
        }

        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[effect_pipe]);

        static const PushDescriptorInfo push_effect_data = { effect_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
        push_descriptor(push_effect_data, 0);

        static const PushDescriptorInfo push_effect_param = { effect_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
        push_descriptor(push_effect_param, param_offs);

        vkCmdDispatch(audio_cmd_buf, wave_count, 1, 1);
    }
}

// TODO temporary test driver: plays a repeating short pattern so the engine is
// audible without a MIDI soundtrack.  Remove once real MIDI input exists.
static void temp_drive_test_notes(uint32_t start_samples, uint32_t end_samples)
{
    constexpr uint32_t   note_period_samples = Synth::rt_sampling_rate;          // 1 note per 1 s
    // Sustain each note 0.75 s, leaving a 0.25 s gap before the next.
    constexpr uint32_t   note_on_samples     = Synth::rt_sampling_rate * 3 / 4;
    static const uint8_t pattern_notes[]     = { 60, 62, 64, 65, 67, 69, 71, 72 };

    // TEMP compressor A/B scaffolding: both channels carry the same piano voice (4),
    // centered at unity so only the per-channel effect chain differs.  Channel 0 has
    // the compressor, channel 1 is dry.  Removed when real MIDI input replaces this
    // driver.
    mix_channels[0].panning = 0.5f;
    mix_channels[0].volume  = 1.0f;
    mix_channels[1].panning = 0.5f;
    mix_channels[1].volume  = 1.0f;

    constexpr uint8_t demo_instrument = 4;

    // Helper lambda: build a base event for the given channel with zeroed fields.
    auto make_event = [](uint8_t channel, uint8_t note) {
        Synth::MidiEvent ev = { };
        ev.channel = channel;
        ev.note    = note;
        return ev;
    };

    for (uint32_t sample = start_samples; sample < end_samples; sample++) {
        const uint32_t phase_in_period = sample % note_period_samples;

        if (phase_in_period != 0 && phase_in_period != note_on_samples) {
            continue;
        }

        // Each pitch is played twice back-to-back: first on channel 0 (compressor),
        // then on channel 1 (dry), so the A/B is heard one note apart on the same note.
        const uint32_t note_index   = sample / note_period_samples;
        const uint8_t  channel      = static_cast<uint8_t>(note_index % 2);
        const uint32_t step         = (note_index / 2) % std::size(pattern_notes);
        const uint8_t  current_note = pattern_notes[step];

        if (phase_in_period == 0) {
            // Note-on with velocity 100, forcing the demo instrument.
            Synth::MidiEvent on_ev = make_event(channel, current_note);
            on_ev.note_data = 100;

            temp_force_instrument = demo_instrument;
            process_note_on(0, on_ev);
            temp_force_instrument = temp_no_force_instrument;
        }
        else {
            Synth::MidiEvent off_ev = make_event(channel, current_note);
            process_note_off(0, off_ev);
        }
    }
}

static void update_modulation()
{
    // TODO temporary test
    // Evaluate the modulation LFOs once per step so every oscillator shares the
    // same phase this step.
    const float vibrato_wave = eval_lfo(vibrato_lfo, modulation_lfo_tick, rt_step_samples, Synth::rt_sampling_rate);  // [-1, 1]
    const float tremolo_wave = eval_lfo(tremolo_lfo, modulation_lfo_tick, rt_step_samples, Synth::rt_sampling_rate);  // [0, 1]

    // Phase 1: advance every active parameter exactly once this step.
    for (uint32_t param_idx = 1; param_idx < max_params; param_idx++) {
        if (params[param_idx].active) {
            advance_param(param_idx);
        }
    }

    // Phase 2: update each live oscillator from its voice's shared parameters.
    constexpr float silence_threshold = 0.0005f;

    for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
        Oscillator& osc = oscillators[osc_idx];
        if (osc.osc_type[0] == no_wave) {
            continue;
        }

        const uint32_t ch    = osc.midi_channel;
        Voice&         voice = voices[osc.voice_id];

        // Apply the per-voice pitch parameter, the per-oscillator unison detune
        // and the mod-wheel-scaled vibrato term.
        osc.pitch = params[voice.pitch_param_id].cur_value + osc.detune + channel_mod_wheel[ch] * vibrato_depth_semitones * vibrato_wave;

        const uint32_t volume_param_id = voice.volume_param_id;

        // Tremolo depth follows the stronger of per-note aftertouch and channel
        // pressure.  Depth 0 leaves the gain at 1; full depth oscillates the gain
        // down to (1 - depth) and never above 1.
        const float tremolo_depth = std::max(voice.aftertouch, channel_pressure[ch]);
        const float tremolo_gain  = 1.0f - tremolo_depth * (1.0f - tremolo_wave);
        osc.volume = voice.velocity * params[volume_param_id].cur_value * tremolo_gain;

        // Free oscillator once the voice's release has decayed to silence
        if (voice.releasing && params[volume_param_id].cur_value < silence_threshold) {
            osc.osc_type[0] = no_wave;
            osc.voice_id    = 0;

            assert(voice.osc_count <= max_unison);
            if (voice.osc_count) {
                --voice.osc_count;
            }

            if ( ! voice.osc_count) {
                note_to_voice[osc.midi_channel][osc.note] = 0;
                voice.active = false;
                params[volume_param_id].active = false;
                voice.volume_param_id = 0;
                params[voice.pitch_param_id].active = false;
                voice.pitch_param_id = 0;

                // Clear the owned oscillator slot ids so a reused voice cannot
                // read stale ids.
                for (uint32_t unison_idx = 0; unison_idx < max_unison; ++unison_idx) {
                    voice.osc_ids[unison_idx] = 0;
                }
            }
        }
    }

    // Advance the shared LFO phase exactly once per render step.
    modulation_lfo_tick++;
}

static void render_audio_step()
{
    const uint32_t start_samples = rendered_samples;
    const uint32_t end_samples   = start_samples + rt_step_samples;

    // TODO move this to the end or outside of this function
    rendered_samples = end_samples;

    // Zero the persistent device effect-state region once.  No command buffer exists
    // at init time, so the fill is deferred to the first render here, mirroring the
    // silence fill below (TRANSFER stage).
    static bool effect_state_cleared = false;
    if ( ! effect_state_cleared && effect_state_fill_bytes) {
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        vkCmdFillBuffer(audio_cmd_buf,
                        buffers[data_buf].get_buffer(),
                        effect_state_fill_offset,
                        effect_state_fill_bytes,
                        0); // data
    }
    effect_state_cleared = true;

    process_events(start_samples, end_samples);

    temp_drive_test_notes(start_samples, end_samples);

    update_modulation();

    // ======================================================================

#if 0
    if ( ! num_filters) {
        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // TODO update_filters();
    }
#endif

    // ======================================================================

    uint32_t num_oscillators = 0;
    uint32_t channel_osc_count[max_mix_channels] = { };

    for (const Oscillator& oscillator : oscillators) {
        if ( ! oscillator.osc_type[0]) {
            continue;
        }

        ++num_oscillators;
        ++channel_osc_count[oscillator.output_channel];
    }

    // A channel joins the mix graph when it has active oscillators this step or an
    // enabled effect chain (so a delay/reverb tail keeps rendering on a silenced
    // channel buffer after the notes stop).
    uint32_t num_mix_channels = 0;
    for (uint32_t chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        if (channel_osc_count[chan_idx] || chain_has_enabled_effect(channel_chains[chan_idx])) {
            ++num_mix_channels;
        }
    }

    // Nothing to render only when no channel is in the graph and the master chain has
    // no tail of its own.  Otherwise the graph runs so effect tails ring out.
    if ( ! num_mix_channels && ! chain_has_enabled_effect(master_chain)) {
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        vkCmdFillBuffer(audio_cmd_buf,
                        buffers[data_buf].get_buffer(),
                        master_output_offs,
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

        const float note_freq   = Synth::note_to_frequency(static_cast<int>(oscillator.note), oscillator.pitch, oscillator.freq_mult);
        const float phase_step  = (static_cast<float>(rt_step_samples) * note_freq) / static_cast<float>(Synth::rt_sampling_rate);

        param.out_sound_offs  = oscillator.osc_output_offs / 4;
        param.phase           = oscillator.phase;
        param.phase_step      = phase_step / static_cast<float>(rt_step_samples);
        param.osc_type[0]     = oscillator.osc_type[0];
        param.osc_type[1]     = oscillator.osc_type[1];
        param.duty[0]         = oscillator.duty[0];
        param.duty[1]         = oscillator.duty[1];
        param.osc_mix         = oscillator.osc_mix;
        param.osc_mode        = oscillator.osc_mode;
        param.mod_ratio       = oscillator.mod_ratio;
        param.fm_index        = oscillator.fm_index;

        // FM modulator runs at carrier_freq * mod_ratio with its own accumulator
        // so continuity holds at non-integer ratios.  Hard sync derives the slave
        // from the master phase directly, so mod_phase / mod_phase_step are unused
        // in that mode (computed unconditionally here, harmless when mod_ratio is
        // the sync ratio).
        const float mod_phase_step = phase_step * oscillator.mod_ratio;
        param.mod_phase       = oscillator.mod_phase;
        param.mod_phase_step  = mod_phase_step / static_cast<float>(rt_step_samples);
        param.fir_memory_offs = oscillator.fir_memory_offs / 4;
        param.taps_offs       = oscillator.fir_taps_offs   / 4;

        oscillator.phase     += phase_step;
        oscillator.mod_phase += mod_phase_step;

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
        if ( ! channel_osc_count[chan_idx] && ! chain_has_enabled_effect(channel_chains[chan_idx])) {
            continue;
        }

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

    static const PushDescriptorInfo push_comb_data = { chan_combine_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_comb_data, 0);

    static const PushDescriptorInfo push_comb_param0 = { chan_combine_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_comb_param0, input_param_offs);

    static const PushDescriptorInfo push_comb_param1 = { chan_combine_pipe, 2, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_comb_param1, chan_param_offs);

    vkCmdDispatch(audio_cmd_buf, num_mix_channels, 1, 1);

    // ======================================================================

    // Apply each active channel's effect chain in place, before the master mix sums
    // the channels.  Distinct per-channel buffers run packed in dependency waves.
    EffectTarget channel_effect_targets[max_mix_channels];
    for (uint32_t used_chan_idx = 0; used_chan_idx < num_mix_channels; used_chan_idx++) {
        const uint32_t chan_idx = chan_map[used_chan_idx];
        channel_effect_targets[used_chan_idx].chain      = &channel_chains[chan_idx];
        channel_effect_targets[used_chan_idx].sound_offs = mix_channels[chan_idx].chan_output_offs / 4;
    }
    apply_effects(channel_effect_targets, num_mix_channels);

    // ======================================================================

    // Sum all per-channel stereo buffers into the master stereo buffer, applying
    // each channel's volume + pan.  The master-mix shader smooths volume/pan over
    // the first 32 samples from the previous step's values (old_*), so carry the
    // current values into old_* after emitting them.

    const uint32_t master_input_param_size = num_mix_channels * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
    const uint32_t master_input_param_offs = static_cast<uint32_t>(param_allocator.allocate(master_input_param_size, synth_alignment).offset);
    const uint32_t master_param_offs       = static_cast<uint32_t>(param_allocator.allocate(static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine)), synth_alignment).offset);

    for (uint32_t used_chan_idx = 0; used_chan_idx < num_mix_channels; used_chan_idx++) {

        const uint32_t chan_idx = chan_map[used_chan_idx];
        Channel&       channel  = mix_channels[chan_idx];

        cur_param_offs = master_input_param_offs + used_chan_idx * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
        ShaderParams::ChannelCombineInput& param = get_param<ShaderParams::ChannelCombineInput>(cur_param_offs);

        param.in_sound_offs = channel.chan_output_offs / 4;
        param.old_volume    = channel.old_volume;
        param.volume        = channel.volume;
        param.old_panning   = channel.old_panning;
        param.panning       = channel.panning;

        channel.old_volume  = channel.volume;
        channel.old_panning = channel.panning;
    }

    ShaderParams::ChannelCombine& master_param = get_param<ShaderParams::ChannelCombine>(master_param_offs);
    master_param.out_sound_offs    = master_output_offs / 4;
    master_param.input_params_offs = 0;
    master_param.num_inputs        = num_mix_channels;

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[master_mix_pipe]);

    static const PushDescriptorInfo push_master_data = { master_mix_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_master_data, 0);

    static const PushDescriptorInfo push_master_param0 = { master_mix_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_master_param0, master_input_param_offs);

    static const PushDescriptorInfo push_master_param1 = { master_mix_pipe, 2, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_master_param1, master_param_offs);

    vkCmdDispatch(audio_cmd_buf, 1, 1, 1);

    // ======================================================================

    // Apply the master effect chain in place on the master buffer, after the mix.
    const EffectTarget master_effect_target = { &master_chain, master_output_offs / 4 };
    apply_effects(&master_effect_target, 1);

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

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

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

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

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

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

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

    return buffers[output_buf].invalidate();
}

template<typename T, bool interleaved>
static bool render_audio_buffer(StereoPtr<T, interleaved> stereo_ptr, uint32_t num_samples)
{
    static uint32_t consumed_samples;
    static uint32_t remaining_samples;

    const auto rendered_src = StereoPtr<T, interleaved>::from_buffer(buffers[output_buf]);

    if (remaining_samples) {
        const uint32_t to_copy = std::min(remaining_samples, num_samples);

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

    const uint32_t to_copy = std::min(to_render, num_samples);
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
