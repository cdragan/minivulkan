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
#include <atomic>
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
    using Synth::max_layers;                 // Max layers (oscillators) per note

    // Oscillator modes (must match osc_mode_* constants in synth_oscillator.comp.glsl)
    constexpr uint32_t osc_mode_blend     = 0;  // Mix osc_type[0] and osc_type[1] by osc_mix
    constexpr uint32_t osc_mode_fm        = 1;  // osc_type[0] is carrier, osc_type[1] is modulator
    constexpr uint32_t osc_mode_hard_sync = 2;  // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave

    using Synth::num_fir_taps;

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

    // Maximum vibrato swing in semitones when the mod wheel is fully on
    constexpr float vibrato_depth_semitones = 0.5f;

    // Vibrato LFO period at rest (~6 Hz); the mod wheel shortens it toward a faster sweep, so
    // raising the wheel makes the vibrato both deeper and faster (a parameter-sourced LFO rate).
    constexpr uint16_t vibrato_period_ms     = 167;
    constexpr float    vibrato_rate_range_ms = 107.0f;  // full wheel -> ~60 ms (~16 Hz)

    // Default pitch bend range in semitones (standard MIDI default is +/- 2)
    constexpr float default_pitch_bend_range_semitones = 2.0f;

    // MIDI continuous controller number for the modulation wheel
    constexpr uint32_t mod_wheel_cc = 1;

    // Voice is a single playing note of a single instrument
    struct Voice {
        bool     active;
        uint8_t  channel;
        uint8_t  instrument;
        uint8_t  osc_ids[max_layers];    // Oscillator slots owned by this voice
        uint8_t  osc_count;              // Number of live oscillator slots owned (0 = none)
        bool     releasing;              // True after note-off, until the volume envelope finishes
    };

    Voice voices[max_voices];

    using Synth::WaveType;
    using Synth::LFODescriptor;
    using Synth::EnvelopeDescriptor;
    using Synth::LayerGen;
    using Synth::TargetRouting;
    using Synth::ModInput;
    using Synth::ModSource;
    using Synth::ModTarget;
    using Synth::SourceOp;
    using enum Synth::ModTarget;

    constexpr uint32_t max_envelope_points = 8;

    // Backing storage giving each envelope room for up to max_envelope_points
    // contiguous points (EnvelopeDescriptor itself ends in a flexible points[1]).
    struct StoredEnvelope {
        EnvelopeDescriptor        desc;
        EnvelopeDescriptor::Point extra_points[max_envelope_points - 1];
    };
    StoredEnvelope envelopes[12];

    LFODescriptor lfo_descs[10];

    // Envelope ids (1-based into envelopes[]); set up in init_modulation.
    constexpr uint16_t volume_envelope_id        = 1;   // envelopes[0]
    constexpr uint16_t cutoff_sweep_envelope_id  = 2;   // envelopes[1]; one sweep shared by all layer cutoff nodes
    constexpr uint16_t release_envelope_base     = 3;   // base id; per-layer release id = base + layer_idx
    constexpr float    cutoff_base_hz            = 400.0f;

    // LFO descriptor ids (1-based into lfo_descs[]); set up in init_modulation.
    constexpr uint16_t vibrato_lfo_desc_id = 1;   // sine vibrato driving voice pitch
    constexpr uint16_t tremolo_lfo_desc_id = 2;   // sine tremolo gain driving voice volume
    constexpr uint16_t master_fir_sweep_lfo_desc_id = 3;   // TEMP demo: triangle sweep of master FIR cutoff

    // parameters[] is partitioned into a sentinel (param 0) then per-owner blocks.  Each owner has a
    // fixed set of roles; a source addresses a parameter by computing its block index.  A modulation
    // target that gets graph nodes occupies a {dest, env, lfo} role triple (the dest is the value the
    // consumer reads; env and lfo are its optional generator leaves).  The per-voice input leaves are
    // the MIDI sources a binding can route from (see resolve_source).
    enum ChannelParamRole : uint32_t {
        chan_param_bend,
        chan_param_mod_wheel,
        chan_param_pressure,
        num_channel_roles
    };
    enum VoiceParamRole : uint32_t {
        voice_input_velocity,
        voice_input_aftertouch,
        voice_input_pressure_combine,
        num_voice_roles
    };
    enum OscParamRole : uint32_t {
        osc_pitch_dest,
        osc_pitch_env,
        osc_pitch_lfo,
        osc_volume_dest,
        osc_volume_env,
        osc_volume_lfo,
        osc_lowpass_dest,
        osc_lowpass_env,
        osc_lowpass_lfo,
        osc_highpass_dest,
        osc_highpass_env,
        osc_highpass_lfo,
        num_osc_roles
    };

    constexpr uint32_t channel_block_base = 1;
    constexpr uint32_t voice_block_base   = channel_block_base + Synth::max_channels * num_channel_roles;
    constexpr uint32_t osc_block_base     = voice_block_base + max_voices * num_voice_roles;
    constexpr uint32_t effect_pool_base   = osc_block_base + max_oscillators * num_osc_roles;

    // Effect-param modulation pool: a bump region holding the dest node and optional LFO-leaf node
    // for each modulated effect param.  Unlike voices, effects are configured once (not per note), so
    // a node is allocated only for a param actually modulated rather than reserving a fixed block.
    constexpr uint32_t max_effect_mod_params = 32;
    constexpr uint32_t effect_pool_nodes     = max_effect_mod_params * 2;  // dest + optional LFO leaf each
    constexpr uint32_t total_params          = effect_pool_base + effect_pool_nodes;

    constexpr uint32_t channel_param(uint32_t channel, uint32_t role)
    {
        return channel_block_base + channel * num_channel_roles + role;
    }
    constexpr uint32_t voice_param(uint32_t voice, uint32_t role)
    {
        return voice_block_base + voice * num_voice_roles + role;
    }
    constexpr uint32_t osc_param(uint32_t osc, uint32_t role)
    {
        return osc_block_base + osc * num_osc_roles + role;
    }

    // Modulation targets which get graph nodes.  Each builds a node triple per layer oscillator;
    // dest_role is the triple's first role, and the env and lfo generator roles are dest_role + 1
    // and dest_role + 2.  Targets absent here are unmodulated constants read straight from the
    // target's routing base_value.
    struct ModTargetNode {
        ModTarget target;
        uint32_t  dest_role;
    };
    constexpr ModTargetNode mod_target_nodes[] = {
        { mod_pitch,           osc_pitch_dest    },
        { mod_volume,          osc_volume_dest   },
        { mod_lowpass_cutoff,  osc_lowpass_dest  },
        { mod_highpass_cutoff, osc_highpass_dest },
    };

    Synth::Parameter       parameters[total_params];
    Synth::ParamDescriptor param_descs[total_params];

    // Bump cursor into the effect-param pool [effect_pool_base, total_params); reset before the
    // effect bindings are (re)expanded.
    uint32_t effect_pool_next = effect_pool_base;

    void reset_effect_pool()
    {
        effect_pool_next = effect_pool_base;
    }

    // Hands out the next pool node, or 0 (the sentinel) when the pool is exhausted; a caller that
    // gets 0 leaves the effect param as an unmodulated constant.
    uint32_t allocate_effect_pool_node()
    {
        if (effect_pool_next >= total_params) {
            return 0;
        }
        return effect_pool_next++;
    }

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
            uint32_t osc_type[2];    // WaveType, raw uint to match the shader layout
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
        fir_coeff_pipe,
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
                    shader_synth_fir_coeff_comp,
                    1,
                },
                one_buffer_ds
            },
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

    constexpr VkDeviceSize device_buf_size = 2 * 1024 * 1024; // TODO

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
        uint32_t midi_channel;              // MIDI channel on which this note was played
        uint32_t output_channel;            // Output (mixing) channel for this MIDI channel/note
        uint32_t note;                      // MIDI note
        uint32_t freq_mult;                 // Frequency multiplier for component frequencies (1 for base frequency)
        WaveType osc_type[2];               // Two oscillator types
        uint32_t osc_output_offs;           // Oscillator data output offset
        uint32_t fir_memory_offs;           // FIR filter memory offset
        uint32_t fir_taps_offs;             // FIR filter taps offset
        uint32_t osc_mode;                  // osc_mode_blend, osc_mode_fm or osc_mode_hard_sync
        float    mod_ratio;                 // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
        float    pitch_offset;              // Per-oscillator pitch offset in semitones (instrument constant for this note's layers)

        // Current phase state
        float    phase;                     // Current position of the oscillator
        float    mod_phase;                 // Current position of the FM modulator
        float    old_volume;                // Previous volume
        float    old_panning;               // Previous panning

        // Resolved values written by update_modulation each step, read by the
        // shader-param fill.  Each comes from its bound parameter or, when the
        // target is unbound, the instrument's base value.
        float    volume;                    // Current volume
        float    panning;                   // Current panning
        float    pitch;                     // Pitch adjustment in semitones
        float    duty[2];                   // Duty cycle for sawtooth and pulse oscillator (0..1)
        float    osc_mix;                   // Mix between osc_type[0] and osc_type[1] (0..1)
        float    fm_index;                  // FM modulation depth

        uint8_t  layer_idx;                 // This oscillator's index in its voice's osc_ids list
        uint8_t  voice_id;                  // Voice which owns this oscillator (0 = none/free)
        bool     clear_fir_hist;            // Set on note-on of a filtered slot; the next render
                                            // zeroes this slot's FIR history before the shader reads
                                            // it, so a reused slot does not bleed the previous note.
    };

    static Oscillator oscillators[max_oscillators];

    // TODO Runtime instrument definition.  Hardcoded for now; an editor will
    // populate these later.  Everything is filled in init_instruments.

    // One oscillator (layer) of an instrument: its waveform, plus its per-target generators (each
    // target's envelope and LFO, which it may own or share from another layer).
    struct OscDescriptor {
        WaveType osc_type[2];                  // osc_type[1] == WaveType::no_wave means single oscillator
        uint32_t osc_mode;                     // osc_mode_blend, osc_mode_fm or osc_mode_hard_sync
        float    mod_ratio;                    // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
        float    pitch_offset;                 // This layer's pitch offset in semitones
        LayerGen gen[Synth::num_mod_targets];  // Per-target envelope/LFO generators
    };
    struct RuntimeInstrument {
        uint32_t      layer_count;                       // Number of layers a note of this instrument uses (1 = mono)
        OscDescriptor layers[max_layers];                // Per-layer oscillator descriptors
        TargetRouting routing[Synth::num_mod_targets];   // Per-target voice-wide base value and inputs
    };
    RuntimeInstrument instruments[5];

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
        uint16_t          src_param_id[ShaderParams::max_effect_param_floats]; // 0 = unmodulated constant
    };

    struct EffectChain {
        uint32_t       num_effects;
        EffectInstance effects[max_effects_per_chain];
    };

    // One effect param's modulation declaration (editor-ready static data, no runtime state).  Effects
    // are not note-triggered, so there is no envelope: a param is base_value driven by an optional LFO
    // and optional channel MIDI input sources.  configure_effect_param expands it onto pool nodes.
    struct EffectParamMod {
        uint8_t   param_index;        // which of the effect's params[] this drives
        float     base_value;
        uint16_t  lfo_desc_id;        // 0 = no LFO
        SourceOp  lfo_op;
        float     lfo_depth;
        ModSource lfo_depth_source;   // none = constant depth
        ModSource lfo_rate_source;    // none = the LFO's own period
        float     lfo_rate_scale;
        uint16_t  num_inputs;
        ModInput  inputs[Synth::max_mod_inputs];   // channel MIDI sources only
    };

    static EffectChain channel_chains[max_mix_channels];
    static EffectChain master_chain;

    struct FirSlot {
        uint32_t coeff_offs;
        uint32_t history_offs;
    };
    FirSlot fir_slots[max_oscillators];

    // Envelope desc id driving a target on a layer.  0 means no envelope.
    static uint16_t layer_envelope_id(const RuntimeInstrument& instrument, ModTarget target, uint32_t layer_idx)
    {
        return instrument.layers[layer_idx].gen[target].envelope_desc_id;
    }

    // A layer has a filter iff a cutoff target has an envelope or a nonzero base.
    static bool osc_has_filter(const RuntimeInstrument& instrument, uint32_t layer_idx)
    {
        return layer_envelope_id(instrument, mod_lowpass_cutoff, layer_idx)
            || layer_envelope_id(instrument, mod_highpass_cutoff, layer_idx)
            || instrument.routing[mod_lowpass_cutoff].base_value  != 0.0f
            || instrument.routing[mod_highpass_cutoff].base_value != 0.0f;
    }

    static bool any_instrument_has_filter()
    {
        for (uint32_t instr_idx = 0; instr_idx < std::size(instruments); instr_idx++) {
            for (uint32_t layer_idx = 0; layer_idx < max_layers; layer_idx++) {
                if (osc_has_filter(instruments[instr_idx], layer_idx)) {
                    return true;
                }
            }
        }

        return false;
    }

    static void init_fir()
    {
        constexpr uint32_t coeff_bytes   = num_fir_taps * sizeof(float);
        constexpr uint32_t history_bytes = (num_fir_taps - 1) * sizeof(float);

        fir_slots[0] = { 0, 0 };

        // Optional allocation: when no instrument declares a filter, leave all FIR
        // offsets 0 so no device bytes are consumed at all.
        if ( ! any_instrument_has_filter()) {
            for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
                fir_slots[osc_idx] = { 0, 0 };
            }
            return;
        }

        // Every oscillator slot gets its own coeff and history buffer so any slot
        // can play a filtered note and sweep independently; slot 0 is the reserved
        // sentinel and stays unused.
        for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
            const SubAllocatorBase::Chunk coeff_chunk = data_allocator.allocate(coeff_bytes, synth_alignment);
            assert(coeff_chunk.offset + coeff_chunk.size <= device_buf_size);
            fir_slots[osc_idx].coeff_offs = static_cast<uint32_t>(coeff_chunk.offset);

            const SubAllocatorBase::Chunk history_chunk = data_allocator.allocate(history_bytes, synth_alignment);
            assert(history_chunk.offset + history_chunk.size <= device_buf_size);
            fir_slots[osc_idx].history_offs = static_cast<uint32_t>(history_chunk.offset);
        }
    }

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

            const uint32_t num_state_floats = ( ! instance.enabled || instance.type == Synth::EffectType::none)
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
        // TEMP demo routing: the two mix channels run different per-channel chains and
        // both sum into the master chain, exercising the full multi-channel mix.  All of
        // this is scaffolding the mixer GUI will replace.  Channel 0 -> distortion then
        // delay; channel 1 -> chorus; master -> reverb then compressor.

        // Channel 0: distortion only (tanh drive, fully wet).  The feedback delay is
        // disabled here so its repeating echo does not obscure the per-note FIR cutoff
        // sweep during the filter listen-check.
        channel_chains[0].num_effects = 1;
        channel_chains[0].effects[0]  = { Synth::EffectType::distortion, true, { 5.0f, 1.0f }, 0 };

        // Channel 1: a single LFO-modulated chorus.
        channel_chains[1].num_effects = 1;
        channel_chains[1].effects[0]  = { Synth::EffectType::chorus, true, { 1.5f, 440.0f, 0.5f }, 0 };

        // Master: reverb then a gentle musical compressor.  attack/release are smoothing
        // coefficients (closer to 1 is slower), roughly 0.2 ms attack and 46 ms release
        // at 44100 Hz, chosen directly to avoid pulling in libc exp() on the host.
        constexpr float compressor_threshold = 0.3f;
        constexpr float compressor_ratio     = 4.0f;
        constexpr float compressor_attack    = 0.9f;
        constexpr float compressor_release   = 0.9995f;
        constexpr float compressor_makeup    = 1.5f;
        // TEMP demo: a low-pass FIR on the master bus whose cutoff is swept by an LFO through the
        // modulation graph (bound in configure_effect_modulation).  params[0] = lowpass Hz -- the
        // value here is only a fallback used if the sweep is not bound; the per-step pull overrides
        // it each step.  params[1] = highpass Hz (0 = off).  Remove once routing is GUI-configured.
        constexpr float master_fir_fallback_lowpass_hz = 3125.0f;  // sweep center
        master_chain.num_effects = 3;
        master_chain.effects[0]  = { Synth::EffectType::reverb, true, { 0.7f, 0.5f, 0.3f, 0.0f, 0.0f }, 0 };
        master_chain.effects[1]  = { Synth::EffectType::compressor, true,
            { compressor_threshold, compressor_ratio, compressor_attack, compressor_release, compressor_makeup }, 0 };
        master_chain.effects[2]  = { Synth::EffectType::fir, true, { master_fir_fallback_lowpass_hz, 0.0f }, 0 };

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

            if ( ! instance.enabled || instance.type == Synth::EffectType::none) {
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

// Fills count layers of an instrument with one waveform; per-layer generators are set afterwards.
static void set_instrument_layers(RuntimeInstrument* instr,
                                  uint32_t           count,
                                  WaveType           type0,
                                  WaveType           type1,
                                  uint32_t           mode,
                                  float              ratio)
{
    instr->layer_count = count;
    for (uint32_t layer_idx = 0; layer_idx < count; layer_idx++) {
        OscDescriptor& osc = instr->layers[layer_idx];
        osc.osc_type[0] = type0;
        osc.osc_type[1] = type1;
        osc.osc_mode    = mode;
        osc.mod_ratio   = ratio;
    }
}

// Fills each instrument's waveforms and per-target modulation.  Every instrument gets the same
// default pitch and volume modulation; per-instrument specifics (FM depth, duty, the supersaw demo)
// follow.  note-on expands these declarations into graph nodes.
static void init_instruments()
{
    // Index 0 is a plain mono sine.  Index 1 is a 7-layer supersaw with a symmetric pitch spread of
    // about +/- 18 cents.  Index 2 is a sine-on-sine FM voice (mod_ratio 2.0, fm_index 3.0).  Index 3
    // is a hard-sync voice: a sine master sets the pitch and a sawtooth slave is synced at ratio 2.5.
    // Index 4 is a mellow piano-ish voice: a triangle (sawtooth at duty 0.5).
    set_instrument_layers(&instruments[0], 1,          WaveType::sine_wave,     WaveType::no_wave,       osc_mode_blend,     0.0f);
    set_instrument_layers(&instruments[1], max_layers, WaveType::sawtooth_wave, WaveType::no_wave,       osc_mode_blend,     0.0f);
    set_instrument_layers(&instruments[2], 1,          WaveType::sine_wave,     WaveType::sine_wave,     osc_mode_fm,        2.0f);
    set_instrument_layers(&instruments[3], 1,          WaveType::sine_wave,     WaveType::sawtooth_wave, osc_mode_hard_sync, 2.5f);
    set_instrument_layers(&instruments[4], 1,          WaveType::sawtooth_wave, WaveType::no_wave,       osc_mode_blend,     0.0f);

    // Supersaw pitch spread: symmetric about +/- 18 cents across the layers.
    static const float supersaw_pitch_offsets[max_layers] = { -0.18f, -0.12f, -0.06f, 0.0f, 0.06f, 0.12f, 0.18f };
    for (uint32_t layer_idx = 0; layer_idx < max_layers; layer_idx++) {
        instruments[1].layers[layer_idx].pitch_offset = supersaw_pitch_offsets[layer_idx];
    }

    for (RuntimeInstrument& instr : instruments) {
        // Pitch routing: channel bend folds in voice-wide on top of the per-layer vibrato.
        instr.routing[mod_pitch].base_value = 0.0f;
        instr.routing[mod_pitch].num_inputs = 1;
        instr.routing[mod_pitch].inputs[0]  = { ModSource::pitch_bend, SourceOp::add, 1.0f };

        // Volume routing: note velocity scales the result voice-wide.
        instr.routing[mod_volume].base_value = 0.0f;
        instr.routing[mod_volume].num_inputs = 1;
        instr.routing[mod_volume].inputs[0]  = { ModSource::velocity, SourceOp::multiply, 1.0f };

        // Panning: centered constant (no generators).
        instr.routing[mod_panning].base_value = 0.5f;

        for (uint32_t layer_idx = 0; layer_idx < instr.layer_count; layer_idx++) {
            // Pitch: a mod-wheel-scaled vibrato whose rate also speeds up with the wheel (a sourced
            // LFO rate).  Each layer runs its own vibrato instance off the same descriptor, so the
            // layers stay in phase (the LFO is a deterministic function of the tick).  No envelope.
            LayerGen& pitch_gen        = instr.layers[layer_idx].gen[mod_pitch];
            pitch_gen.lfo_desc_id      = vibrato_lfo_desc_id;
            pitch_gen.lfo_op           = SourceOp::add;
            pitch_gen.lfo_depth        = vibrato_depth_semitones;
            pitch_gen.lfo_depth_source = ModSource::mod_wheel;
            pitch_gen.lfo_rate_source  = ModSource::mod_wheel;
            pitch_gen.lfo_rate_scale   = -vibrato_rate_range_ms;

            // Volume: an ADSR envelope attenuated by a pressure-driven tremolo LFO (the supersaw
            // overrides the envelope per layer below).
            LayerGen& volume_gen        = instr.layers[layer_idx].gen[mod_volume];
            volume_gen.envelope_desc_id = volume_envelope_id;
            volume_gen.lfo_desc_id      = tremolo_lfo_desc_id;
            volume_gen.lfo_op           = SourceOp::multiply;
            volume_gen.lfo_depth        = 1.0f;
            volume_gen.lfo_depth_source = ModSource::pressure_combine;
            volume_gen.lfo_rate_source  = ModSource::none;
        }
    }

    // FM voice: constant FM depth 3.0.
    instruments[2].routing[mod_fm_index].base_value = 3.0f;

    // Triangle-ish piano: sawtooth at duty 0.5.
    instruments[4].routing[mod_duty0].base_value = 0.5f;

    // Supersaw demo: each layer gets its own swept low pass and its own volume envelope with a
    // staggered release, so the layers fade out at audibly different rates.
    RuntimeInstrument& supersaw = instruments[1];
    supersaw.routing[mod_lowpass_cutoff].base_value = cutoff_base_hz;
    for (uint32_t layer_idx = 0; layer_idx < supersaw.layer_count; layer_idx++) {
        supersaw.layers[layer_idx].gen[mod_lowpass_cutoff].envelope_desc_id = cutoff_sweep_envelope_id;

        supersaw.layers[layer_idx].gen[mod_volume].envelope_desc_id =
            static_cast<uint16_t>(release_envelope_base + layer_idx);
    }
}

static void init_oscillator_buffers()
{
    assert(Synth::num_channels <= max_mix_channels);

    init_instruments();

    for (uint32_t channel = 0; channel < max_mix_channels; channel++) {
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

    init_fir();
}

// Expands every effect param's modulation declaration onto pool nodes; defined after resolve_source,
// forward-declared here because init_modulation runs earlier in the file.
static void configure_effect_modulation();

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

    // TODO TEMP demo: cutoff sweep envelope (id 2 => envelopes[1]).  It decays from
    // the full sweep amount down to 0 over the note, so the cutoff parameter sweeps
    // from base + sweep_hz down to base.  Value 0xFFFF maps to sweep_hz Hz
    // (min_max_delta = sweep_hz / 65535); 1 tick ~ 6 ms, so ~125 ticks ~ 0.75 s
    // matches the demo note length.  No sustain plateau: the sweep runs to 0 even
    // while the key is held.
    constexpr float cutoff_sweep_hz = 6000.0f;
    StoredEnvelope& cutoff_envelope = envelopes[1];
    cutoff_envelope.desc.num_points          = 3;
    cutoff_envelope.desc.unused_alignment    = 0;
    cutoff_envelope.desc.sustain_first_point = 2;
    cutoff_envelope.desc.sustain_last_point  = 2;
    cutoff_envelope.desc.min_value           = 0.0f;
    cutoff_envelope.desc.min_max_delta       = cutoff_sweep_hz / 65535.0f;
    cutoff_envelope.desc.points[0] = { 0,   0xFFFF };  // full sweep amount at note-on
    cutoff_envelope.desc.points[1] = { 125, 0      };  // decays to base over ~0.75 s
    cutoff_envelope.desc.points[2] = { 126, 0      };  // stays at base (sustain)

    // TEMP demo: per-layer volume envelopes for the supersaw (envelopes[2..]).  Each
    // copies the piano volume envelope but lengthens the release so the layers
    // tail off at audibly different rates.
    for (uint32_t layer_idx = 0; layer_idx < max_layers; layer_idx++) {
        StoredEnvelope& release_envelope = envelopes[2 + layer_idx];
        release_envelope = volume_envelope;

        // Release runs from the sustain point (5) to the final point (6); lengthen it
        // per layer index from ~0.15 s up to ~2.1 s.
        const uint16_t release_ticks = static_cast<uint16_t>(25 + layer_idx * 55);
        release_envelope.desc.points[6].position =
            static_cast<uint16_t>(release_envelope.desc.points[5].position + release_ticks);
    }

    // Vibrato LFO: sine, ~6 Hz (167 ms); min/delta unused by eval_lfo_mod.
    lfo_descs[vibrato_lfo_desc_id - 1] = { Synth::WaveType::sine_wave, 0, vibrato_period_ms, 0.0f, 1.0f };

    // Tremolo LFO: sine, ~5 Hz (200 ms); min/delta unused by eval_lfo_mod.
    lfo_descs[tremolo_lfo_desc_id - 1] = { Synth::WaveType::sine_wave, 0, 200, 0.0f, 1.0f };

    // TEMP demo: master FIR cutoff sweep LFO -- a 4 s triangle (sawtooth, duty 0x7F); min/delta unused.
    lfo_descs[master_fir_sweep_lfo_desc_id - 1] = { Synth::WaveType::sawtooth_wave, 0x7F, 4000, 0.0f, 1.0f };

    // Each channel's pitch bend, mod wheel and pressure are externally-driven inputs;
    // propagate_parameters must not overwrite them with a fold.
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        param_descs[channel_param(channel, chan_param_bend)].kind      = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_mod_wheel)].kind = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_pressure)].kind  = Synth::ParamKind::external;
    }

    // Expand effect-param modulation onto pool nodes (resolves channel sources, so it runs after the
    // channel leaves above; the chains themselves were built earlier in init_effects).
    configure_effect_modulation();
}

static bool allocate_oscillators(uint8_t*                 osc_ids,
                                 uint32_t                 num_osc,
                                 uint32_t                 voice_idx,
                                 const RuntimeInstrument& instrument)
{
    uint32_t allocated = 0;

    for (uint32_t osc_idx = 1; osc_idx < max_oscillators && allocated < num_osc; osc_idx++) {
        if (oscillators[osc_idx].osc_type[0] == WaveType::no_wave) {
            oscillators[osc_idx].voice_id    = static_cast<uint8_t>(voice_idx);
            oscillators[osc_idx].osc_type[0] = instrument.layers[allocated].osc_type[0];
            osc_ids[allocated]               = static_cast<uint8_t>(osc_idx);
            ++allocated;
        }
    }

    if (allocated < num_osc) {
        for (uint32_t rollback_idx = 0; rollback_idx < allocated; rollback_idx++) {

            Oscillator& osc = oscillators[osc_ids[rollback_idx]];
            osc.voice_id    = 0;
            osc.osc_type[0] = WaveType::no_wave;

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

    // offset is in frames; interleaved stores 2 samples per frame.
    StereoPtr<T, true>& operator+=(size_t offset) {
        data += offset * 2;
        return *this;
    }

    static StereoPtr<T, true> from_buffer(Buffer& buffer) {
        return { buffer.get_ptr<T>() };
    }
};

template<typename T>
static StereoPtr<T, true> operator+(StereoPtr<T, true> ptr, size_t offset)
{
    return { ptr.data + offset * 2 };
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

    if (voice_idx) {
        assert(voices[voice_idx].active);

        voices[voice_idx].releasing = true;
    }
}

static const RuntimeInstrument& voice_instrument(const Voice& voice)
{
    return instruments[voice.instrument < std::size(instruments) ? voice.instrument : 0];
}

// Returns a partially-allocated note to the free pool when allocation fails
// part-way through, and reclaims a still-alive note on re-trigger.  osc_ids[0,
// osc_count) is kept exactly the live oscillators, so this frees all of them; it is
// safe at any failure point.
static void drop_voice(uint32_t voice_idx, uint32_t channel, uint32_t note)
{
    Voice& voice = voices[voice_idx];

    for (uint32_t layer_idx = 0; layer_idx < voice.osc_count; ++layer_idx) {
        Oscillator& osc = oscillators[voice.osc_ids[layer_idx]];
        osc.osc_type[0] = WaveType::no_wave;
        osc.voice_id    = 0;
    }
    voice.osc_count = 0;

    voice.active                 = false;
    note_to_voice[channel][note] = 0;
}

// Maps a binding's input-source role to the concrete parameter id of the channel or voice leaf
// that carries its live value.  none (and anything unrecognized) maps to the value-0 sentinel.
static uint32_t resolve_source(ModSource source, uint32_t channel, uint32_t voice_idx)
{
    switch (source) {
        case ModSource::pitch_bend:       return channel_param(channel, chan_param_bend);
        case ModSource::mod_wheel:        return channel_param(channel, chan_param_mod_wheel);
        case ModSource::channel_pressure: return channel_param(channel, chan_param_pressure);
        case ModSource::velocity:         return voice_param(voice_idx, voice_input_velocity);
        case ModSource::aftertouch:       return voice_param(voice_idx, voice_input_aftertouch);
        case ModSource::pressure_combine: return voice_param(voice_idx, voice_input_pressure_combine);
        default:                          return 0;
    }
}

// Expands one layer's target into its {dest, env, lfo} node triple at the layer's oscillator slot.
// The dest folds, in order, an additive source from the envelope generator, a source from the LFO
// generator (per lfo_op), then each routed input source; absent generators contribute no source.
// Each layer instantiates its own generators, so layers with the same descriptor id evaluate the
// same value in phase without sharing nodes.  Every route is data, not code here.
static void configure_target_modulation(const RuntimeInstrument& instrument,
                                        ModTarget                target,
                                        uint32_t                 dest_role,
                                        uint32_t                 osc_slot,
                                        uint32_t                 channel,
                                        uint32_t                 voice_idx,
                                        uint32_t                 layer_idx)
{
    const LayerGen&      gen     = instrument.layers[layer_idx].gen[target];
    const TargetRouting& routing = instrument.routing[target];

    const uint32_t env_node = osc_param(osc_slot, dest_role + 1);
    const uint32_t lfo_node = osc_param(osc_slot, dest_role + 2);

    // Envelope generator node.  Clear it first to drop stale state from a reused slot: an absent
    // envelope leaves it kind external (unreferenced, value 0), so it contributes no source.
    param_descs[env_node] = { };
    parameters[env_node]  = { };
    if (gen.envelope_desc_id) {
        param_descs[env_node].kind             = Synth::ParamKind::envelope;
        param_descs[env_node].envelope.desc_id = gen.envelope_desc_id;
        parameters[env_node].sustain_voice     = static_cast<uint16_t>(voice_idx);
    }

    // LFO generator node.  Clear it first; configure_lfo overrides the descriptor when the layer has
    // an LFO, otherwise it stays kind external (unreferenced, since configure_plain gets 0 below).
    param_descs[lfo_node] = { };
    parameters[lfo_node]  = { };
    if (gen.lfo_desc_id) {
        Synth::configure_lfo(&param_descs[lfo_node],
                            gen.lfo_desc_id,
                            gen.lfo_op,
                            gen.lfo_depth,
                            static_cast<uint16_t>(resolve_source(gen.lfo_depth_source, channel, voice_idx)),
                            static_cast<uint16_t>(resolve_source(gen.lfo_rate_source, channel, voice_idx)),
                            gen.lfo_rate_scale);
    }

    // Resolve the target's routed input sources to concrete param ids.
    Synth::SourceParam sources[Synth::max_mod_inputs];
    for (uint32_t input_idx = 0; input_idx < routing.num_inputs; input_idx++) {
        const ModInput& input = routing.inputs[input_idx];
        sources[input_idx] = { static_cast<uint16_t>(resolve_source(input.source, channel, voice_idx)),
                               input.op, input.multiplier };
    }

    Synth::configure_plain(&param_descs[osc_param(osc_slot, dest_role)],
                                routing.base_value,
                                gen.envelope_desc_id ? static_cast<uint16_t>(env_node) : uint16_t(0),
                                gen.lfo_desc_id ? static_cast<uint16_t>(lfo_node) : uint16_t(0),
                                gen.lfo_op,
                                sources,
                                routing.num_inputs);
}

// Seeds an externally-driven leaf (a MIDI input source) with an initial value+prev so a consumer
// reads it with zero lag on the first step.
static void set_input_leaf(uint32_t node, float value)
{
    param_descs[node] = { };
    param_descs[node].kind      = Synth::ParamKind::external;
    parameters[node].value      = value;
    parameters[node].prev_value = value;
}

// Effects route only channel-wide MIDI sources; per-voice sources have no voice in an effect's context.
static bool is_channel_mod_source(ModSource source)
{
    return source == ModSource::pitch_bend
        || source == ModSource::mod_wheel
        || source == ModSource::channel_pressure;
}

// Expands one effect param's modulation declaration onto pool nodes: a dest node (folded by
// propagate_parameters) plus an LFO leaf when present, recording the dest in src_param_id so the
// per-step pull copies its value into the param the shader reads.  channel resolves the input sources
// (master effects pass no inputs, so channel is unused there).
static void configure_effect_param(EffectInstance* effect, const EffectParamMod& mod, uint32_t channel)
{
    const uint32_t dest_node = allocate_effect_pool_node();
    if ( ! dest_node) {
        return;   // pool exhausted: the param keeps its constant value
    }
    parameters[dest_node] = { };

    uint32_t lfo_node = 0;
    if (mod.lfo_desc_id) {
        lfo_node = allocate_effect_pool_node();
        if ( ! lfo_node) {
            return;
        }
        Synth::configure_lfo(&param_descs[lfo_node],
                            mod.lfo_desc_id,
                            mod.lfo_op,
                            mod.lfo_depth,
                            static_cast<uint16_t>(resolve_source(mod.lfo_depth_source, channel, 0)),
                            static_cast<uint16_t>(resolve_source(mod.lfo_rate_source, channel, 0)),
                            mod.lfo_rate_scale);
        parameters[lfo_node] = { };   // fresh LFO phase
    }

    Synth::SourceParam sources[Synth::max_mod_inputs];
    for (uint32_t input_idx = 0; input_idx < mod.num_inputs; input_idx++) {
        const ModInput& input = mod.inputs[input_idx];
        assert(is_channel_mod_source(input.source));
        sources[input_idx] = { static_cast<uint16_t>(resolve_source(input.source, channel, 0)),
                               input.op, input.multiplier };
    }

    Synth::configure_plain(&param_descs[dest_node],
                                mod.base_value,
                                0,   // effects have no envelope
                                static_cast<uint16_t>(lfo_node),   // 0 when no LFO
                                mod.lfo_op,
                                sources,
                                mod.num_inputs);

    effect->src_param_id[mod.param_index] = static_cast<uint16_t>(dest_node);
}

static void configure_effect_modulation()
{
    reset_effect_pool();

    // TEMP demo: sweep the master FIR lowpass cutoff with a 4 s triangle LFO (250..6000 Hz), now
    // through the modulation graph instead of a host-side hack.  Removed once effect modulation is
    // GUI-configured.  The sweep is linear in Hz (the LFO is linear), where the old hack was
    // exponential -- close enough for demo scaffolding.
    constexpr float sweep_low_hz  = 250.0f;
    constexpr float sweep_high_hz = 6000.0f;

    for (uint32_t effect_idx = 0; effect_idx < master_chain.num_effects; effect_idx++) {
        EffectInstance& instance = master_chain.effects[effect_idx];
        if (instance.enabled && instance.type == Synth::EffectType::fir) {
            EffectParamMod mod   = { };
            mod.param_index      = 0;   // FIR lowpass cutoff Hz
            mod.base_value       = (sweep_low_hz + sweep_high_hz) * 0.5f;
            mod.lfo_desc_id      = master_fir_sweep_lfo_desc_id;
            mod.lfo_op           = SourceOp::add;
            mod.lfo_depth        = (sweep_high_hz - sweep_low_hz) * 0.5f;
            configure_effect_param(&instance, mod, 0);   // master: LFO only
        }
    }
}

static void process_note_on(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel = event.channel;
    const uint32_t note    = event.note;

    const uint8_t target_instrument = select_instrument(channel, note);

    // Re-triggering a note still alive (held or releasing, possibly with some
    // layers already silenced) reclaims it cleanly so the new note
    // starts from a fully-allocated voice.
    const uint32_t existing_voice = note_to_voice[channel][note];
    if (existing_voice) {
        drop_voice(existing_voice, channel, note);
    }

    const uint32_t voice_idx = allocate_unused_voice();
    if ( ! voice_idx) {
        d_printf("All voices are active, dropping note %u on channel %u\n", note, channel);
        return;
    }
    assert(voices[voice_idx].osc_count == 0);

    Voice& voice     = voices[voice_idx];
    voice.channel    = static_cast<uint8_t>(channel);
    voice.instrument = target_instrument;
    voice.active     = true;
    voice.releasing  = false;

    note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);

    const RuntimeInstrument& instrument  = voice_instrument(voice);
    const uint32_t           layer_count = instrument.layer_count;
    assert(layer_count >= 1 && layer_count <= max_layers);

    // Per-voice input leaves the bindings route from.  velocity is the note's constant; aftertouch
    // starts at zero (driven by poly-aftertouch events); pressure_combine is recomputed each step as
    // max(aftertouch, channel pressure) by advance_parameters.
    const float velocity = static_cast<float>(event.note_data) / 127.0f;
    set_input_leaf(voice_param(voice_idx, voice_input_velocity),         velocity);
    set_input_leaf(voice_param(voice_idx, voice_input_aftertouch),       0.0f);
    set_input_leaf(voice_param(voice_idx, voice_input_pressure_combine), 0.0f);

    if ( ! allocate_oscillators(voice.osc_ids, layer_count, voice_idx, instrument)) {
        d_printf("All oscillators are active, dropping note %u on channel %u\n", note, channel);
        drop_voice(voice_idx, channel, note);
        return;
    }
    voice.osc_count = static_cast<uint8_t>(layer_count);

    // Initialize each oscillator's constants and phase/smoothing state.  Resolved
    // values (volume/pitch/duty/osc_mix/fm_index/panning) are written every step by
    // update_modulation; only the smoothing history is seeded here.
    for (uint32_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        Oscillator& osc    = oscillators[voice.osc_ids[layer_idx]];
        osc.layer_idx      = static_cast<uint8_t>(layer_idx);
        osc.midi_channel   = channel;
        osc.output_channel = channel;
        osc.note           = note;
        osc.freq_mult      = 1;
        const OscDescriptor& layer = instrument.layers[layer_idx];
        osc.osc_type[0]    = layer.osc_type[0];
        osc.osc_type[1]    = layer.osc_type[1];
        osc.osc_mode       = layer.osc_mode;
        osc.mod_ratio      = layer.mod_ratio;
        osc.pitch_offset   = layer.pitch_offset;
        osc.phase          = 0.0f;
        osc.mod_phase      = 0.0f;
        osc.old_volume     = 0.0f;   // ramp up from silence to avoid a click
        osc.old_panning    = instrument.routing[mod_panning].base_value;

        // Expand each modulation target into its node triple at this oscillator's slot.
        const uint32_t osc_slot = voice.osc_ids[layer_idx];
        for (const ModTargetNode& node : mod_target_nodes) {
            configure_target_modulation(instrument,
                                        node.target,
                                        node.dest_role,
                                        osc_slot,
                                        channel,
                                        voice_idx,
                                        layer_idx);
        }
    }

    // Set up each oscillator's FIR once cutoff bindings are resolved.
    for (uint32_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        Oscillator&    osc      = oscillators[voice.osc_ids[layer_idx]];
        const uint32_t osc_slot = voice.osc_ids[layer_idx];

        if (osc_has_filter(instrument, layer_idx)) {
            osc.fir_taps_offs   = fir_slots[osc_slot].coeff_offs;
            osc.fir_memory_offs = fir_slots[osc_slot].history_offs;
            osc.clear_fir_hist  = true;
        }
        else {
            osc.fir_taps_offs   = 0;
            osc.fir_memory_offs = 0;
            osc.clear_fir_hist  = false;
        }
    }
}

static void process_aftertouch(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    if (voice_idx) {
        assert(voices[voice_idx].active);

        // Drive the per-note aftertouch leaf (value and prev_value) so the combine reads it with zero lag.
        const uint32_t aftertouch_node = voice_param(voice_idx, voice_input_aftertouch);
        const float    pressure        = static_cast<float>(event.note_data) / 127.0f;
        parameters[aftertouch_node].value      = pressure;
        parameters[aftertouch_node].prev_value = pressure;
    }
}

static void process_controller(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // TODO Only the modulation wheel is supported for now; other controllers ignored.
    if (event.controller == mod_wheel_cc) {
        const uint32_t node  = channel_param(event.channel, chan_param_mod_wheel);
        const float    value = static_cast<float>(event.controller_data) / 127.0f;
        parameters[node].value      = value;
        parameters[node].prev_value = value;
    }
}

static void process_pitch_bend(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // Drive the channel bend leaf (value and prev_value) so consumers read it with zero lag.
    const uint32_t bend_node = channel_param(event.channel, chan_param_bend);
    const float    bend      = Synth::pitch_bend_to_semitones(event.pitch_bend, default_pitch_bend_range_semitones);
    parameters[bend_node].value      = bend;
    parameters[bend_node].prev_value = bend;
}

static void process_channel_pressure(uint32_t delta_samples, const Synth::MidiEvent& event)
{
    // Drive the per-channel pressure leaf (value and prev_value) so the combine reads it with zero lag.
    const uint32_t pressure_node = channel_param(event.channel, chan_param_pressure);
    const float    pressure      = static_cast<float>(event.note_data) / 127.0f;
    parameters[pressure_node].value      = pressure;
    parameters[pressure_node].prev_value = pressure;
}

using EventHandler = void (*)(uint32_t delta_samples, const Synth::MidiEvent& event);

// Program change is unused and thus unsupported.
constexpr EventHandler process_program_change = nullptr;

static const EventHandler event_handlers[] = {
    #define X(name) process_##name,
    MIDI_EVENT_TYPES(X)
    #undef X
};

void Synth::apply_midi_event(const Synth::MidiEvent& event)
{
    assert(static_cast<uint32_t>(event.event) < std::size(event_handlers));

    const EventHandler handler = event_handlers[static_cast<uint8_t>(event.event)];

    if (handler) {
        handler(0, event);
    }
}

static void process_events(uint32_t start_samples, uint32_t end_samples)
{
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
                           VkPipelineStageFlags dst_stage,
                           VkAccessFlags        extra_src_access = 0,
                           VkPipelineStageFlags extra_src_stage  = 0)
{
    static VkMemoryBarrier2 barrier = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        nullptr, // pNext
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE
    };

    // The chained source (set at the end of the previous call) covers the most
    // recent producer.  extra_src_* additionally sources an earlier producer in a
    // different stage, so a single dependency can make several writes visible at
    // once (e.g. compute-written FIR taps AND a transfer history fill).  It applies
    // only to this call; the chain reset below restores src = dst.
    barrier.srcStageMask  |= extra_src_stage;
    barrier.srcAccessMask |= extra_src_access;
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

// Copies each modulated effect param's current graph value into the param float the effect shader
// reads (and compute_fir_coefficients reads for the FIR effect).  Unmodulated params (src_param_id
// 0) keep their constant value.
static void pull_effect_param_chain(EffectChain* chain)
{
    for (uint32_t effect_idx = 0; effect_idx < chain->num_effects; effect_idx++) {
        EffectInstance& instance = chain->effects[effect_idx];
        if ( ! instance.enabled) {
            continue;
        }
        for (uint32_t param_idx = 0; param_idx < ShaderParams::max_effect_param_floats; param_idx++) {
            if (instance.src_param_id[param_idx]) {
                instance.params[param_idx] = parameters[instance.src_param_id[param_idx]].value;
            }
        }
    }
}

static void pull_effect_params()
{
    for (uint32_t channel = 0; channel < Synth::num_channels; channel++) {
        pull_effect_param_chain(&channel_chains[channel]);
    }
    pull_effect_param_chain(&master_chain);
}

// Advances the modulation graph one control-rate step: first compute every generator leaf's value
// (reading depth/rate sources from prev_value -> one-step delay), then propagate base+edges.
static void advance_parameters()
{
    // Tremolo depth is the stronger of per-note aftertouch and per-channel pressure -- the one
    // non-additive combine the graph cannot express as an edge.  Written value+prev for zero lag.
    for (uint32_t voice_idx = 1; voice_idx < max_voices; voice_idx++) {
        if (voices[voice_idx].active) {
            const uint32_t channel    = voices[voice_idx].channel;
            const float    aftertouch = parameters[voice_param(voice_idx, voice_input_aftertouch)].value;
            const float    pressure   = parameters[channel_param(channel, chan_param_pressure)].value;
            const float    depth      = aftertouch > pressure ? aftertouch : pressure;
            const uint32_t depth_node = voice_param(voice_idx, voice_input_pressure_combine);

            parameters[depth_node].value      = depth;
            parameters[depth_node].prev_value = depth;
        }
    }

    for (uint32_t node_idx = 1; node_idx < total_params; node_idx++) {
        const Synth::ParamDescriptor& gen = param_descs[node_idx];

        if (gen.kind == Synth::ParamKind::envelope) {
            const uint16_t sustain_voice = parameters[node_idx].sustain_voice;
            const Voice&   owner         = voices[sustain_voice];
            const bool     sustain       = sustain_voice ? (owner.active && ! owner.releasing) : true;
            parameters[node_idx].value = Synth::eval_envelope(envelopes[gen.envelope.desc_id - 1].desc,
                                                              &parameters[node_idx].envelope, sustain);
        }
        else if (gen.kind == Synth::ParamKind::lfo) {
            const float    depth  = gen.lfo.depth_param_id
                                  ? parameters[gen.lfo.depth_param_id].prev_value * gen.lfo.depth
                                  : gen.lfo.depth;
            // A rate source offsets the LFO's own period (ms) by source * rate_scale; without one,
            // period 0 tells eval_lfo_mod to use the descriptor period unchanged.  Clamp the offset
            // result to at least 1 ms so an editor-supplied scale can never drive the period negative
            // (unsigned underflow) or to zero (a divide-by-zero in the LFO phase).
            const float    base_ms    = static_cast<float>(lfo_descs[gen.lfo.desc_id - 1].period_ms);
            const float    offset_ms  = base_ms + parameters[gen.lfo.rate_param_id].prev_value * gen.lfo.rate_scale;
            const uint32_t period     = gen.lfo.rate_param_id
                                      ? static_cast<uint32_t>(offset_ms > 1.0f ? offset_ms : 1.0f)
                                      : 0;
            parameters[node_idx].value = Synth::eval_lfo_mod(lfo_descs[gen.lfo.desc_id - 1],
                                                             parameters[node_idx].lfo_tick,
                                                             rt_step_samples,
                                                             Synth::rt_sampling_rate,
                                                             period,
                                                             depth,
                                                             gen.lfo.op);
            parameters[node_idx].lfo_tick++;
        }
    }

    Synth::propagate_parameters(parameters, param_descs, total_params);
}

static void update_modulation()
{
    advance_parameters();

    // Resolve each live oscillator's values from the modulation graph.
    constexpr float silence_threshold = 0.0005f;

    for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
        Oscillator& osc = oscillators[osc_idx];
        if (osc.osc_type[0] == WaveType::no_wave) {
            continue;
        }

        Voice&                   voice      = voices[osc.voice_id];
        const RuntimeInstrument& instrument = voice_instrument(voice);

        // Pitch adds the per-layer pitch offset on top of the layer's graph pitch
        // node (which already folds in channel bend and the vibrato generator).
        osc.pitch    = parameters[osc_param(osc_idx, osc_pitch_dest)].value
                     + osc.pitch_offset;
        // These targets are unmodulated constants: read the target's routing base value.
        osc.panning  = instrument.routing[mod_panning].base_value;
        osc.duty[0]  = instrument.routing[mod_duty0].base_value;
        osc.duty[1]  = instrument.routing[mod_duty1].base_value;
        osc.osc_mix  = instrument.routing[mod_osc_mix].base_value;
        osc.fm_index = instrument.routing[mod_fm_index].base_value;

        // Volume: the oscillator's volume dest node, which already folds the ADSR envelope, the tremolo
        // LFO edge and the velocity input edge.  free-on-silence keys off the raw ADSR envelope node.
        const float vol_env_value = parameters[osc_param(osc_idx, osc_volume_env)].value;
        osc.volume = parameters[osc_param(osc_idx, osc_volume_dest)].value;

        // Free the oscillator once its volume decays to silence during release.
        // Oscillator-scope volume can release at different rates per layer index,
        // so a voice's oscillators may free in different steps; the voice itself is
        // finalized only when its last oscillator frees.
        if (voice.releasing && vol_env_value < silence_threshold) {
            osc.osc_type[0] = WaveType::no_wave;
            osc.voice_id    = 0;

            // Remove this slot from the voice's live list without a search: its position is
            // osc.layer_idx, so move the last live entry into it (and update that moved
            // oscillator's stored position) to keep osc_ids[0, osc_count) the live set.
            assert(voice.osc_count);
            --voice.osc_count;
            const uint8_t moved_slot          = voice.osc_ids[voice.osc_count];
            voice.osc_ids[osc.layer_idx]      = moved_slot;
            oscillators[moved_slot].layer_idx = osc.layer_idx;

            if ( ! voice.osc_count) {
                note_to_voice[osc.midi_channel][osc.note] = 0;
                voice.active = false;

                // Clear the owned oscillator slot ids so a reused voice cannot
                // read stale ids.
                mstd::mem_zero(voice.osc_ids, sizeof(voice.osc_ids));
            }
        }
    }
}

// Rounds a cutoff in Hz to the int the coeff shader wants; <=0 disables the edge, else clamped to [1, Nyquist-1].
static uint32_t cutoff_hz_to_int(float value)
{
    if (value <= 0.0f) {
        return 0;
    }

    constexpr uint32_t min_cutoff_hz = 1;
    const uint32_t     max_cutoff_hz = Synth::rt_sampling_rate / 2 - 1;

    if (value <= static_cast<float>(min_cutoff_hz)) {
        return min_cutoff_hz;
    }
    if (value >= static_cast<float>(max_cutoff_hz)) {
        return max_cutoff_hz;
    }

    return static_cast<uint32_t>(value + 0.5f);
}

static void set_effect_fir_coeffs(const EffectChain& chain, uint32_t* cur_param_offs)
{
    for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
        const EffectInstance& instance = chain.effects[effect_idx];
        if ( ! instance.enabled || instance.type != Synth::EffectType::fir) {
            continue;
        }

        ShaderParams::FIRCoeff& param = get_param<ShaderParams::FIRCoeff>(*cur_param_offs);
        param.taps_offs            = instance.state_offs / 4;
        param.lowpass_cutoff_freq  = cutoff_hz_to_int(instance.params[0]);
        param.highpass_cutoff_freq = cutoff_hz_to_int(instance.params[1]);

        *cur_param_offs += static_cast<uint32_t>(sizeof(ShaderParams::FIRCoeff));
    }
}

static uint32_t count_effect_fir(const EffectChain& chain)
{
    uint32_t count = 0;
    for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
        const EffectInstance& instance = chain.effects[effect_idx];
        if (instance.enabled && instance.type == Synth::EffectType::fir) {
            ++count;
        }
    }

    return count;
}

static void compute_fir_coefficients()
{
    uint32_t num_active_filters = 0;
    for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
        if (oscillators[osc_idx].osc_type[0] != WaveType::no_wave && oscillators[osc_idx].fir_taps_offs) {
            ++num_active_filters;
        }
    }

    for (uint32_t chan_idx = 0; chan_idx < Synth::num_channels; chan_idx++) {
        num_active_filters += count_effect_fir(channel_chains[chan_idx]);
    }
    num_active_filters += count_effect_fir(master_chain);

    if ( ! num_active_filters) {
        return;
    }

    const uint32_t param_size = num_active_filters * static_cast<uint32_t>(sizeof(ShaderParams::FIRCoeff));
    const uint32_t param_offs = static_cast<uint32_t>(param_allocator.allocate(param_size, synth_alignment).offset);

    uint32_t cur_param_offs = param_offs;
    for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
        const Oscillator& osc = oscillators[osc_idx];
        if (osc.osc_type[0] == WaveType::no_wave || ! osc.fir_taps_offs) {
            continue;
        }

        ShaderParams::FIRCoeff& param = get_param<ShaderParams::FIRCoeff>(cur_param_offs);
        param.taps_offs            = osc.fir_taps_offs / 4;
        param.lowpass_cutoff_freq  = cutoff_hz_to_int(parameters[osc_param(osc_idx, osc_lowpass_dest)].value);
        param.highpass_cutoff_freq = cutoff_hz_to_int(parameters[osc_param(osc_idx, osc_highpass_dest)].value);

        cur_param_offs += static_cast<uint32_t>(sizeof(ShaderParams::FIRCoeff));
    }

    for (uint32_t chan_idx = 0; chan_idx < Synth::num_channels; chan_idx++) {
        set_effect_fir_coeffs(channel_chains[chan_idx], &cur_param_offs);
    }
    set_effect_fir_coeffs(master_chain, &cur_param_offs);

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[fir_coeff_pipe]);

    static const PushDescriptorInfo push_fir_data = { fir_coeff_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_fir_data, 0);

    static const PushDescriptorInfo push_fir_param = { fir_coeff_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_fir_param, param_offs);

    const uint32_t sampling_freq = Synth::rt_sampling_rate;
    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[fir_coeff_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,                     // offset
                       sizeof(sampling_freq), // size
                       &sampling_freq);       // pValues

    vkCmdDispatch(audio_cmd_buf, num_active_filters, 1, 1);
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

    Synth::pump_live_midi();

    update_modulation();

    pull_effect_params();

    // ======================================================================

    compute_fir_coefficients();

    // ======================================================================

    // Clear FIR history buffer for new notes
    bool any_history_cleared = false;
    for (uint32_t osc_idx = 1; osc_idx < max_oscillators; osc_idx++) {
        if ( ! oscillators[osc_idx].clear_fir_hist) {
            continue;
        }

        if ( ! any_history_cleared) {
            memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            any_history_cleared = true;
        }

        vkCmdFillBuffer(audio_cmd_buf,
                        buffers[data_buf].get_buffer(),
                        fir_slots[osc_idx].history_offs,
                        (num_fir_taps - 1) * sizeof(float),
                        0); // data

        oscillators[osc_idx].clear_fir_hist = false;
    }

    // ======================================================================

    uint32_t num_oscillators = 0;
    uint32_t channel_osc_count[max_mix_channels] = { };

    for (const Oscillator& oscillator : oscillators) {
        if (oscillator.osc_type[0] == WaveType::no_wave) {
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
        if (oscillator.osc_type[0] == WaveType::no_wave)
            continue;

        ShaderParams::Oscillator& param = get_param<ShaderParams::Oscillator>(cur_param_offs);

        const float note_freq   = Synth::note_to_frequency(static_cast<int>(oscillator.note), oscillator.pitch, oscillator.freq_mult);
        const float phase_step  = (static_cast<float>(rt_step_samples) * note_freq) / static_cast<float>(Synth::rt_sampling_rate);

        param.out_sound_offs  = oscillator.osc_output_offs / 4;
        param.phase           = oscillator.phase;
        param.phase_step      = phase_step / static_cast<float>(rt_step_samples);
        param.osc_type[0]     = static_cast<uint32_t>(oscillator.osc_type[0]);
        param.osc_type[1]     = static_cast<uint32_t>(oscillator.osc_type[1]);
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

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[oscillator_pipe]);

    static const PushDescriptorInfo push_osc_data = { oscillator_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_osc_data, 0);

    static const PushDescriptorInfo push_osc_param = { oscillator_pipe, 1, 0, param_buf, ShaderParams::max_param_range };
    push_descriptor(push_osc_param, osc_base_param_offs);

    if (num_oscillators) {
        vkCmdDispatch(audio_cmd_buf, num_oscillators, 1, 1);
    }

    // ======================================================================

    const uint32_t input_param_size = num_oscillators * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
    const uint32_t chan_param_size  = num_mix_channels * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine));
    const uint32_t input_param_offs = static_cast<uint32_t>(param_allocator.allocate(input_param_size, synth_alignment).offset);
    const uint32_t chan_param_offs  = static_cast<uint32_t>(param_allocator.allocate(chan_param_size, synth_alignment).offset);

    uint32_t chan_input_indices[max_mix_channels]     = { }; // indexed by compacted used_chan_idx
    uint32_t chan_map[max_mix_channels]               = { };
    uint32_t gen_chan_input_indices[max_mix_channels] = { }; // indexed by raw channel, for the oscillator loop

    for (uint32_t input_idx = 0, used_chan_idx = 0, chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        if ( ! channel_osc_count[chan_idx] && ! chain_has_enabled_effect(channel_chains[chan_idx])) {
            continue;
        }

        assert(used_chan_idx < num_mix_channels);
        chan_map[used_chan_idx]           = chan_idx;
        chan_input_indices[used_chan_idx] = input_idx;
        gen_chan_input_indices[chan_idx]  = input_idx;

        input_idx += channel_osc_count[chan_idx];
        ++used_chan_idx;
    }

    for (Oscillator& oscillator : oscillators) {
        if (oscillator.osc_type[0] == WaveType::no_wave)
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

constexpr uint32_t audio_ring_frames      = Synth::rt_sampling_rate;
constexpr uint32_t audio_lead_frames      = Synth::rt_sampling_rate / 50;
constexpr uint32_t audio_max_batch_frames = rt_step_samples * 16;

// Ring buffer holding rendered sound in the platform's output format.  Sized for either
// channel layout: two channels of audio_ring_frames frames.
template<typename T, bool interleaved>
struct AudioRingStorage {
    static T data[audio_ring_frames * 2];
};

template<typename T, bool interleaved>
T AudioRingStorage<T, interleaved>::data[audio_ring_frames * 2];

// Frame counters and the dry-ring count are format independent, so they are shared and the
// GUI can read them without knowing the format.  Exactly one <T, interleaved> instantiation
// may run per build (each has its own ring storage); using two would desync these counters.
static std::atomic<uint64_t> audio_ring_write;     // frames produced; only the producer stores
static std::atomic<uint64_t> audio_ring_read;      // frames consumed; only the callback stores
static std::atomic<uint32_t> audio_underrun_count; // ring ran dry; read by the GUI indicator

// A StereoPtr addressing the ring storage at a frame offset.
template<typename T, bool interleaved>
static StereoPtr<T, interleaved> ring_stereo(uint32_t frame_offset)
{
    T* const base = AudioRingStorage<T, interleaved>::data;

    if constexpr (interleaved) {
        return { base + frame_offset * 2 };
    }
    else {
        return { base + frame_offset, base + audio_ring_frames + frame_offset };
    }
}

// A StereoPtr over the caller's output channels (channel1 is unused when interleaved).
template<typename T, bool interleaved>
static StereoPtr<T, interleaved> output_stereo(T* channel0, T* channel1)
{
    if constexpr (interleaved) {
        return { channel0 };
    }
    else {
        return { channel0, channel1 };
    }
}

template<typename T, bool interleaved>
static void zero_output(StereoPtr<T, interleaved> dest, uint32_t num_frames)
{
    if constexpr (interleaved) {
        mstd::mem_zero(dest.data, num_frames * 2 * sizeof(T));
    }
    else {
        mstd::mem_zero(dest.left,  num_frames * sizeof(T));
        mstd::mem_zero(dest.right, num_frames * sizeof(T));
    }
}

Synth::AudioRingStatus Synth::get_audio_ring_status()
{
    // Read before write so the monotonic counters cannot make the difference underflow.
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_relaxed);
    const uint64_t write_pos = audio_ring_write.load(std::memory_order_relaxed);

    return { get_ringbuf_data_size(write_pos, read_pos),
             audio_lead_frames,
             audio_underrun_count.load(std::memory_order_relaxed) };
}

template<typename T, bool interleaved>
bool Synth::produce_audio_batch()
{
    const uint64_t write_pos = audio_ring_write.load(std::memory_order_relaxed);
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_acquire);
    const uint32_t available = get_ringbuf_data_size(write_pos, read_pos);

    // The callback drains the lead and the producer tops it back up.
    if (available >= audio_lead_frames) {
        return false;
    }

    // Refill back toward the lead in one submit (whole steps), capped per batch.
    uint32_t to_render = mstd::align_up(audio_lead_frames - available, rt_step_samples);
    if (to_render > audio_max_batch_frames) {
        to_render = audio_max_batch_frames;
    }

    if ( ! render_audio<T, interleaved>(to_render)) {
        return false;
    }

    const StereoPtr<T, interleaved> rendered_src = StereoPtr<T, interleaved>::from_buffer(buffers[output_buf]);

    uint32_t copied = 0;
    while (copied < to_render) {
        const uint32_t offset = static_cast<uint32_t>((write_pos + copied) % audio_ring_frames);
        const uint32_t chunk  = std::min(to_render - copied, get_ringbuf_contig_tail(write_pos + copied, audio_ring_frames));
        copy_audio_data(ring_stereo<T, interleaved>(offset), rendered_src + copied, chunk);
        copied += chunk;
    }

    audio_ring_write.store(write_pos + to_render, std::memory_order_release);
    return true;
}

template<typename T, bool interleaved>
uint32_t Synth::consume_audio(uint32_t num_frames, T* channel0, T* channel1)
{
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_relaxed);
    const uint64_t write_pos = audio_ring_write.load(std::memory_order_acquire);
    const uint32_t available = get_ringbuf_data_size(write_pos, read_pos);
    const uint32_t to_copy   = (available < num_frames) ? available : num_frames;

    const StereoPtr<T, interleaved> dest = output_stereo<T, interleaved>(channel0, channel1);

    uint32_t copied = 0;
    while (copied < to_copy) {
        const uint32_t offset = static_cast<uint32_t>((read_pos + copied) % audio_ring_frames);
        const uint32_t chunk  = std::min(to_copy - copied, get_ringbuf_contig_tail(read_pos + copied, audio_ring_frames));
        copy_audio_data(dest + copied, ring_stereo<T, interleaved>(offset), chunk);
        copied += chunk;
    }

    audio_ring_read.store(read_pos + to_copy, std::memory_order_release);

    // Underrun: emit silence rather than stale or garbage samples, and count it.
    if (to_copy < num_frames) {
        audio_underrun_count.fetch_add(1, std::memory_order_relaxed);
        zero_output<T, interleaved>(dest + to_copy, num_frames - to_copy);
    }

    return to_copy;
}

template bool Synth::produce_audio_batch<int16_t, true>();
template bool Synth::produce_audio_batch<float,   true>();
template bool Synth::produce_audio_batch<float,   false>();

template uint32_t Synth::consume_audio<int16_t, true>(uint32_t, int16_t*, int16_t*);
template uint32_t Synth::consume_audio<float,   true>(uint32_t, float*,   float*);
template uint32_t Synth::consume_audio<float,   false>(uint32_t, float*,   float*);
