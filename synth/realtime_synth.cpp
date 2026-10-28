// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "realtime_synth.h"

#include "../core/d_printf.h"
#include "../core/minivulkan.h"
#include "../core/mstdc.h"
#include "../core/resource.h"
#include "../core/rng.h"
#include "../core/suballoc.h"
#include "synth_effect_expansion.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_midi_cc.h"
#include "synth_soundtrack.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <math.h>
#include <type_traits>

#include "synth_shaders.h"

#include "../core/shaders.h"

namespace {
enum BufferTypes : uint8_t {
    data_buf,   // Device buffer which is used by effects etc.
    param_buf,  // Dynamic device buffer with parameters for compute shaders etc.
    output_buf, // Host buffer which is filled with generated audio data
    num_buf_types
};

Buffer buffers[num_buf_types];

// Vulkan command buffer used for the synth
CommandBuffers<1> audio_cmd_buf;

// Polyphony limits
constexpr uint32_t max_voices = 64; // Max notes are playing

using Synth::max_layers; // Max layers (oscillators) per note
using Synth::num_fir_taps;
using Synth::osc_mode_hard_sync;

// Number of samples which have been rendered since playback started.
uint32_t rendered_samples;

// Stores current per-channel time measured in samples
uint32_t channel_samples[Synth::max_channels];

// Saved state of event decode, per-channel
uint8_t events_decode_state[Synth::max_channels];

// MidiEvent, including note duration (for note_on event)
struct DispatchedMidiEvent : Synth::MidiEvent {
    uint32_t release_sample;
};

// Map notes in each note in each channel to voices
typedef uint8_t NoteToVoice[128];
NoteToVoice     note_to_voice[Synth::max_channels];

// Random pitch skew: a few cents of analog-style drift, applied per note and per layer.
constexpr uint32_t note_skew_seed     = 0x5eed1234u;
constexpr uint32_t noise_start_stride = 0x9e3779b9u; // Spreads note-on noise starts across the 32 bit range

// Default pitch bend range in semitones (standard MIDI default is +/- 2)
constexpr float default_pitch_bend_range_semitones = 2.0f;

// MIDI continuous controller number for the modulation wheel
constexpr uint32_t mod_wheel_cc = 1;

// Voice is a single playing note of a single instrument
struct Voice {
    bool     active;
    uint8_t  channel;
    uint8_t  note;                // MIDI note this voice plays, set at note-on
    uint8_t  instrument;          // Instrument index this voice plays, set at note-on
    uint8_t  osc_ids[max_layers]; // Oscillator slots owned by this voice
    uint8_t  osc_count;           // Number of live oscillator slots owned (0 = none)
    bool     releasing;           // True after note-off, until the volume envelope finishes
    bool     note_finished;       // True after release stage is finished and the note is no longer playing
    bool     sustained;           // Key is up but the sustain pedal holds the voice
    uint32_t release_total_ticks; // Instrument release in control steps (0 = legacy hold/loop)
    uint32_t release_sample;      // Absolute sample to auto-release a duration-model note (0 = none)
};

Voice voices[max_voices];

// Raw 14-bit assignable-CC accumulators, one per MIDI channel (see synth_midi_cc.h).
// CC state is MIDI-driven, not bank-driven, so it survives bank swaps like the mod
// wheel does.  init_modulation seeds every LSB register with the 0xFF sentinel.
Synth::ChannelCcState channel_cc_state[Synth::max_channels];

using Synth::EffectChainBinding;
using Synth::EffectParamBinding;
using Synth::EffectSlotBinding;
using Synth::EffectType;
using Synth::EnvelopeDescriptor;
using Synth::Instrument;
using Synth::InstrumentBank;
using Synth::ModInput;
using Synth::ModSource;
using Synth::ModTarget;
using Synth::Oscillator;
using Synth::OscMode;
using Synth::SourceOp;
using Synth::SourceParam;
using Synth::TargetBinding;
using Synth::WaveType;
using enum Synth::ModTarget;
using enum Synth::OscMode;

// The bank of instruments, envelopes and LFOs the runtime builds at init and reads while playing.
InstrumentBank synth_bank;

// Callback for reading modified instrument bank from the editor.
std::atomic<void (*)()> bank_source_callback = nullptr;

// parameters[] is partitioned into a sentinel (param 0) then per-owner blocks.  Each owner has a
// fixed set of roles; a source addresses a parameter by computing its block index.  A modulation
// target that gets graph nodes occupies a {dest, env, lfo} role triple (the dest is the value the
// consumer reads; env and lfo are its optional generator leaves).  The per-voice input leaves are
// the MIDI sources a binding can route from (see resolve_source).
enum ChannelParamRole : uint32_t {
    chan_param_bend,
    chan_param_mod_wheel,
    chan_param_pressure,
    chan_param_volume,  // CC 7, external leaf
    chan_param_panning, // CC 10, external leaf
    chan_param_cc_0,    // + slot (0..15): the channel's assignable-CC leaves (CC 16..31)
    num_channel_roles = chan_param_cc_0 + Synth::num_assignable_ccs
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
    osc_spread_dest,
    osc_spread_env,
    osc_spread_lfo,
    osc_panning_dest,
    osc_panning_env,
    osc_panning_lfo,
    osc_duty0_dest,
    osc_duty0_env,
    osc_duty0_lfo,
    osc_duty1_dest,
    osc_duty1_env,
    osc_duty1_lfo,
    osc_mix_dest,
    osc_mix_env,
    osc_mix_lfo,
    osc_fm_index_dest,
    osc_fm_index_env,
    osc_fm_index_lfo,
    osc_fm_ratio_dest,
    osc_fm_ratio_env,
    osc_fm_ratio_lfo,
    osc_pad_tilt0_dest,
    osc_pad_tilt0_env,
    osc_pad_tilt0_lfo,
    osc_pad_tilt1_dest,
    osc_pad_tilt1_env,
    osc_pad_tilt1_lfo,
    osc_fir_slope_dest,
    osc_fir_slope_env,
    osc_fir_slope_lfo,
    osc_fir_band0_offset_dest,
    osc_fir_band0_offset_env,
    osc_fir_band0_offset_lfo,
    osc_fir_band0_gain_dest,
    osc_fir_band0_gain_env,
    osc_fir_band0_gain_lfo,
    osc_fir_band1_offset_dest,
    osc_fir_band1_offset_env,
    osc_fir_band1_offset_lfo,
    osc_fir_band1_spread_dest,
    osc_fir_band1_spread_env,
    osc_fir_band1_spread_lfo,
    osc_fir_band1_gain_dest,
    osc_fir_band1_gain_env,
    osc_fir_band1_gain_lfo,
    osc_fir_band1_slope_dest,
    osc_fir_band1_slope_env,
    osc_fir_band1_slope_lfo,
    osc_fir_band2_offset_dest,
    osc_fir_band2_offset_env,
    osc_fir_band2_offset_lfo,
    osc_fir_band2_spread_dest,
    osc_fir_band2_spread_env,
    osc_fir_band2_spread_lfo,
    osc_fir_band2_gain_dest,
    osc_fir_band2_gain_env,
    osc_fir_band2_gain_lfo,
    osc_fir_band2_slope_dest,
    osc_fir_band2_slope_env,
    osc_fir_band2_slope_lfo,
    osc_fir_band3_offset_dest,
    osc_fir_band3_offset_env,
    osc_fir_band3_offset_lfo,
    osc_fir_band3_spread_dest,
    osc_fir_band3_spread_env,
    osc_fir_band3_spread_lfo,
    osc_fir_band3_gain_dest,
    osc_fir_band3_gain_env,
    osc_fir_band3_gain_lfo,
    // clang-format off
    osc_fir_band3_slope_dest, osc_fir_band3_slope_env, osc_fir_band3_slope_lfo,
    osc_string_damping_dest,
    osc_string_damping_env,
    osc_string_damping_lfo,
    osc_string_release_damping_dest,
    osc_string_release_damping_env,
    osc_string_release_damping_lfo,
    osc_string_dispersion_dest,
    osc_string_dispersion_env,
    osc_string_dispersion_lfo,
    // clang-format on
    num_osc_roles
};

static_assert(num_osc_roles == 87);

constexpr uint32_t channel_block_base = 1;
constexpr uint32_t voice_block_base   = channel_block_base + Synth::max_channels * num_channel_roles;
constexpr uint32_t osc_block_base     = voice_block_base + max_voices * num_voice_roles;
constexpr uint32_t effect_pool_base   = osc_block_base + Synth::max_oscillators * num_osc_roles;

// Effect-param modulation pool: a bump region holding the dest node and optional LFO-leaf node
// for each modulated effect param.  Unlike voices, effects are configured once (not per note), so
// a node is allocated only for a param actually modulated rather than reserving a fixed block.
constexpr uint32_t effect_pool_nodes = Synth::max_effect_mod_params * 2; // dest + optional LFO leaf each
constexpr uint32_t total_params      = effect_pool_base + effect_pool_nodes;

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
// and dest_role + 2.
struct ModTargetNode {
    ModTarget target;
    uint32_t  dest_role;
};

constexpr ModTargetNode mod_target_nodes[] = {
    { mod_pitch, osc_pitch_dest },
    { mod_volume, osc_volume_dest },
    { mod_fir_spread, osc_spread_dest },
    { mod_panning, osc_panning_dest },
    { mod_duty0, osc_duty0_dest },
    { mod_duty1, osc_duty1_dest },
    { mod_osc_mix, osc_mix_dest },
    { mod_fm_index, osc_fm_index_dest },
    { mod_fm_ratio, osc_fm_ratio_dest },
    { mod_pad_tilt0, osc_pad_tilt0_dest },
    { mod_pad_tilt1, osc_pad_tilt1_dest },
    { mod_fir_slope, osc_fir_slope_dest },
    { mod_fir_band0_offset, osc_fir_band0_offset_dest },
    { mod_fir_band0_gain, osc_fir_band0_gain_dest },
    { mod_fir_band1_offset, osc_fir_band1_offset_dest },
    { mod_fir_band1_spread, osc_fir_band1_spread_dest },
    { mod_fir_band1_gain, osc_fir_band1_gain_dest },
    { mod_fir_band1_slope, osc_fir_band1_slope_dest },
    { mod_fir_band2_offset, osc_fir_band2_offset_dest },
    { mod_fir_band2_spread, osc_fir_band2_spread_dest },
    { mod_fir_band2_gain, osc_fir_band2_gain_dest },
    { mod_fir_band2_slope, osc_fir_band2_slope_dest },
    { mod_fir_band3_offset, osc_fir_band3_offset_dest },
    { mod_fir_band3_spread, osc_fir_band3_spread_dest },
    { mod_fir_band3_gain, osc_fir_band3_gain_dest },
    { mod_fir_band3_slope, osc_fir_band3_slope_dest },
    { mod_string_damping, osc_string_damping_dest },
    { mod_string_release_damping, osc_string_release_damping_dest },
    { mod_string_dispersion, osc_string_dispersion_dest },
};
static_assert(sizeof(mod_target_nodes) / sizeof(mod_target_nodes[0]) == Synth::num_mod_targets,
              "mod target table must cover every ModTarget");

// Dest roles of bands 1..3, indexed [band - 1][param] in the uniform
// offset/spread/gain/slope order.
constexpr uint32_t band_dest_roles[3][4] = {
    { osc_fir_band1_offset_dest, osc_fir_band1_spread_dest, osc_fir_band1_gain_dest, osc_fir_band1_slope_dest },
    { osc_fir_band2_offset_dest, osc_fir_band2_spread_dest, osc_fir_band2_gain_dest, osc_fir_band2_slope_dest },
    { osc_fir_band3_offset_dest, osc_fir_band3_spread_dest, osc_fir_band3_gain_dest, osc_fir_band3_slope_dest },
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
template <typename T> uint32_t allocate_unused_slot(T* pool, uint32_t count, bool T::* active)
{
    for (uint32_t slot = 1; slot < count; slot++) {
        if (! (pool[slot].*active)) {
            return slot;
        }
    }

    return 0;
}

// File-local shader parameter types: one struct scope standing in for a
// namespace (named namespace blocks are forbidden in implementation
// files), so the ShaderParams::X references below keep compiling.
// Note: These defintions must match the structs inside the shaders
struct ShaderParams {
    // synth_fir_coeff shader
    struct FIRCoeff {
        struct Band {
            uint32_t highpass_cutoff_freq; // High pass frequency (if 0, it's a pure low pass)
            uint32_t lowpass_cutoff_freq;  // Low pass frequency (if 0, it's a pure high pass)
            uint32_t window_span; // Effective odd window span in taps, 11..num_taps (num_taps = full sharpness)
            float    gain;        // Band gain; the shader scales the band's normalized kernel with it
        };

        uint32_t taps_offs;  // Offset of FIR filter's taps
        uint32_t band_count; // Bands staged in bands[0..band_count-1], 1..4; band 0 covers the single-band form
        Band     bands[4];
    };

    // synth_oscillator shader
    struct Oscillator {
        uint32_t out_sound_offs;
        float    phase;
        float    phase_step;
        uint32_t osc_pack; // Bits 0-7: osc_type[0], 8-15: osc_type[1], 16-23: osc_mode, 24: releasing, 25-31 spare
        float    duty[2];
        float    osc_mix;
        float    mod_ratio; // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
        float    fm_index;
        float    mod_phase;
        float    mod_phase_step;

        // Optional filter parameters
        uint32_t fir_memory_offs;
        uint32_t taps_offs;
        uint32_t noise_counter;
        uint32_t note_seed;           // Per-note constant: the note-on noise counter, never advanced
        float    pad_tilt[2];         // Pad spectral tilt; harmonic exponent is 1 + clamp(tilt, -1, 1).
        uint32_t string_state_offs;   // String state region in floats: header, then ring
        float    string_damping;      // String damping 0..1, higher = shorter decay
        float string_release_damping; // Effective release damping 0.025..1.667: 0.05 / T60 in s, curved from the knob
        float string_brightness;      // String pluck brightness 0..1, used by the note-on excitation
        float string_dispersion;      // String dispersion 0..1, 0 = off
        uint32_t string_chain;        // Note seed which started the string chain, kept across string takeovers
    };

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
    struct EffectParams {
        uint32_t type;
        uint32_t sound_offs;
        uint32_t state_offs;
        uint32_t bypass;
        float    params[Synth::max_effect_param_floats];
    };

    // synth_output_* shaders
    struct OutputPushConst {
        uint32_t in_sound_offs;
    };

    static_assert(sizeof(Oscillator) == 92);

    static constexpr uint32_t max_param_range = sizeof(Oscillator) * 256;
};

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

enum DescSetTypes : uint8_t {
    one_buffer_ds,
    two_buffers_ds,
    one_double_buffer_ds,
    num_desc_set_layouts
};

VkDescriptorSetLayout desc_set_layouts[num_desc_set_layouts];

bool create_shaders()
{
    static const DescSetBindingInfo bindings[] = { { one_buffer_ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { one_buffer_ds, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { two_buffers_ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { two_buffers_ds, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { two_buffers_ds, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { one_double_buffer_ds, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                                   { one_double_buffer_ds, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
                                                   { num_desc_set_layouts, 0, 0, 0 } };

    if (! create_compute_descriptor_set_layouts(bindings, num_desc_set_layouts, desc_set_layouts))
        return false;

    struct ShaderInfo {
        ComputeShaderInfo shader_info;
        DescSetTypes      desc_set;
    };

    static const ShaderInfo shaders[] = { { {
                                                shader_synth_fir_coeff_comp,
                                                1,
                                            },
                                            one_buffer_ds },
                                          { {
                                                shader_synth_oscillator_comp,
                                                0,
                                            },
                                            one_buffer_ds },
                                          { {
                                                shader_synth_chan_combine_comp,
                                                0,
                                            },
                                            two_buffers_ds },
                                          { {
                                                shader_synth_master_mix_comp,
                                                0,
                                            },
                                            two_buffers_ds },
                                          { {
                                                shader_synth_effect_comp,
                                                0,
                                            },
                                            one_buffer_ds },
                                          // TODO load only in builds which need it
                                          { {
                                                shader_synth_output_16_interlv_comp,
                                                1,
                                            },
                                            one_buffer_ds },
                                          // TODO load only in builds which need it
                                          { {
                                                shader_synth_output_f32_interlv_comp,
                                                1,
                                            },
                                            one_buffer_ds },
                                          // TODO load only in builds which need it
                                          { {
                                                shader_synth_output_f32_separate_comp,
                                                1,
                                            },
                                            one_double_buffer_ds } };

    assert(std::size(shaders) == std::size(pipes));
    assert(std::size(pipes) == num_synth_pipes);

    for (uint32_t i = 0; i < num_synth_pipes; i++) {

        if (! shaders[i].shader_info.shader)
            continue;

        const VkDescriptorSetLayout ds_layouts[] = {
            desc_set_layouts[shaders[i].desc_set],
            VK_NULL_HANDLE // list terminator
        };

        static const VkSpecializationMapEntry map_entries[] = {
            { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 4, 12, 4 }, { 5, 16, 4 }, { 6, 20, 4 },
        };

        static uint32_t spec_data[] = {
            Synth::rt_step_samples,  0, num_fir_taps, Synth::effect_delay_max_samples, Synth::effect_chorus_max_samples,
            Synth::rt_sampling_rate,
        };
        spec_data[1] = vk11_props.subgroupSize;

        static const VkSpecializationInfo spec_constants = { std::size(map_entries),
                                                             map_entries,
                                                             sizeof(spec_data),
                                                             &spec_data };

        if (! create_compute_shader(shaders[i].shader_info, ds_layouts, &spec_constants, &pipe_layouts[i], &pipes[i]))
            return false;
    }

    return true;
}

constexpr VkDeviceSize device_buf_size = Synth::effect_buffer_bytes;

SubAllocator<1024> data_allocator;

SubAllocator<1> param_allocator;

size_t synth_alignment;

template <typename T> T& get_param(uint32_t offset)
{
    return *buffers[param_buf].get_ptr<T>(offset);
}

struct RunningOscillator {
    // Constants which don't change for this oscillator's instance's life time
    uint32_t          midi_channel;    // MIDI channel on which this note was played
    uint32_t          output_channel;  // Output (mixing) channel for this MIDI channel/note
    uint32_t          note;            // MIDI note
    uint32_t          freq_mult;       // Frequency multiplier for component frequencies (1 for base frequency)
    WaveType          osc_type[2];     // Two oscillator types
    uint32_t          osc_output_offs; // Oscillator data output offset
    uint32_t          fir_memory_offs; // FIR filter memory offset
    uint32_t          fir_taps_offs;   // FIR filter taps offset
    uint32_t          osc_mode;        // osc_mode_blend, osc_mode_fm or osc_mode_hard_sync
    float             mod_ratio;       // FM: modulator/carrier freq ratio.  Hard sync: slave cycles per master cycle
    int8_t            octave_offset;   // Whole-octave pitch shift: 12 * octave_offset semitones are added to the pitch
                                       // (instrument constant for this note's layer)
    Synth::FilterType fir_type; // Band-pass: spread in semitones.  Low-pass/high-pass: spread is a fixed cutoff in Hz
    uint8_t           fir_band_count; // Active bandpass bands 1..4 (forced to 1 for other filter types)

    // Current phase state
    float    phase;         // Current position of the oscillator
    float    mod_phase;     // Current position of the FM modulator
    uint32_t noise_counter; // Sample index of the noise stream, advanced every step
    uint32_t note_seed;     // Per-note constant copy of the note-on noise counter, never advanced
    float    old_volume;    // Previous volume
    float    old_panning;   // Previous panning

    // Resolved values written by update_modulation each step, read by the
    // shader-param fill.
    float    volume;                 // Current volume
    float    panning;                // Current panning
    float    pitch;                  // Absolute equivalent MIDI pitch (69 = A4), resolved by update_modulation from
                                     // note + octave offset + pitch offset/skews + the modulated pitch target value
    float    spread;                 // FIR Pitch Spread in semitones, from the spread target's dest
                                     // node; <= 0 means the oscillator's FIR is off
    float    fir_slope;              // FIR window slope percent, from the slope target's dest node
    float    duty[2];                // Duty cycle for sawtooth and pulse oscillator (0..1)
    float    pad_tilt[2];            // Pad spectral tilt; harmonic exponent is 1 + clamp(tilt, -1, 1).
    float    string_damping;         // String damping 0..1, resolved each step
    float    string_release_damping; // String release damping 0..1, resolved each step
    float    string_brightness;      // String pluck brightness 0..1, copied from the layer at note-on
    float    string_dispersion;      // String dispersion 0..1, resolved each step
    uint32_t string_state_offs;      // String state region offset in bytes, fixed per slot
    uint32_t string_chain;           // Note seed of the first note of the current string chain on this slot
    float    osc_mix;                // Mix between osc_type[0] and osc_type[1] (0..1)
    float    fm_index;               // FM modulation depth

    // Multi-band bandpass state, refreshed by update_modulation each step.
    // Band 0 mirrors the legacy spread/slope fields above, so its kernel math
    // stays bit-identical to the single-band form.
    float band_offset[4]; // Offset of the band center above the pitch, semitones
    float band_spread[4]; // Band width in semitones (band 0 = spread)
    float band_gain[4];   // Band gain 0..1
    float band_slope[4];  // Window slope percent (band 0 = fir_slope)
    bool  fir_active;     // Current-step FIR engagement: some staged band has a
                          // positive spread (bandpass), or the spread itself is
                          // positive (single-edge). Gains only scale the kernel,
                          // so an all-zero-gain oscillator still stages a zeroed
                          // kernel (silence) instead of bypassing.

    uint8_t layer_idx;      // This oscillator's index in its voice's osc_ids list
    uint8_t source_layer;   // Instrument layer this oscillator plays.  It is fixed for the note, unlike
                            // layer_idx, which changes when a finished oscillator leaves osc_ids.
    uint8_t voice_id;       // Voice which owns this oscillator (0 = none/free)
    bool    clear_fir_hist; // Set on note-on of a filtered slot; the next render
                            // zeroes this slot's FIR history before the shader reads
                            // it, so a reused slot does not bleed the previous note.
    bool    terminating;    // Schedules removal on the next modulation step.  Normal voice-lifetime
                            // termination frees after its final fade block has rendered; a note-on
                            // pitch-gate rejection frees before anything renders.

    float    decay_mult;   // Per-step multiplier, precomputed once at note-on (one exp2)
    float    decay_factor; // Running factor; 1.0 until decay starts
    uint32_t decay_tick;   // Steps since decay started
    bool     decaying;     // Sustain decay active (only ever true when release is set)
};

static RunningOscillator oscillators[Synth::max_oscillators];

RNG note_skew_rng; // Shared generator for per-note pitch skew

// Gives every note-on its own noise start, so repeated notes do not repeat the same noise.
static uint32_t noise_start_counter;

static constexpr uint32_t max_mix_channels = Synth::max_channels;

struct Channel {
    uint32_t chan_output_offs; // Channel data output offset
    float    volume;           // Channel volume (linear), default 1.0
    float    panning;          // Channel pan: 0 = left, 0.5 = center, 1 = right
    float    old_volume;       // Previous step's volume, for the block gradient
    float    old_panning;      // Previous step's panning, for the block gradient
};

static Channel mix_channels[max_mix_channels];

// Dedicated interleaved-stereo master output, summed from all channels
uint32_t master_output_offs;

using Synth::EffectChain;
using Synth::EffectInstance;

static EffectChain channel_chains[max_mix_channels];
static EffectChain master_chain;

struct FirSlot {
    uint32_t coeff_offs;
    uint32_t history_offs;
};

FirSlot fir_slots[Synth::max_oscillators];

// A layer carries a filter iff its spread target can modulate at all: a
static void init_fir()
{
    constexpr uint32_t coeff_bytes   = num_fir_taps * sizeof(float);
    constexpr uint32_t history_bytes = (num_fir_taps - 1) * sizeof(float);

    fir_slots[0] = { 0, 0 };

    // Every oscillator slot gets its own coeff and history buffer so any slot
    // can play a filtered note and sweep independently; slot 0 is the reserved
    // sentinel and stays unused.
    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        const SubAllocatorBase::Chunk coeff_chunk = data_allocator.allocate(coeff_bytes, synth_alignment);
        assert(coeff_chunk.offset + coeff_chunk.size <= device_buf_size);
        fir_slots[osc_idx].coeff_offs = static_cast<uint32_t>(coeff_chunk.offset);

        const SubAllocatorBase::Chunk history_chunk = data_allocator.allocate(history_bytes, synth_alignment);
        assert(history_chunk.offset + history_chunk.size <= device_buf_size);
        fir_slots[osc_idx].history_offs = static_cast<uint32_t>(history_chunk.offset);
    }
}

// Returns the n-th enabled, non-none effect of a chain, or nullptr.
static const EffectInstance* nth_enabled_effect(const EffectChain& chain, uint32_t n)
{
    uint32_t enabled_seen = 0;

    for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
        const EffectInstance& instance = chain.effects[effect_idx];

        if (! instance.enabled || instance.type == Synth::EffectType::none) {
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

// Format of the live audio ring, registered by the running instantiation of
// produce_audio_batch.  The peek accessor dispatches on it, because the
// format is a build-time platform choice and exactly one instantiation runs
// per build.
enum class ActiveRingFormat : uint8_t {
    none,
    int16_interleaved,
    float_interleaved,
    float_planar
};

std::atomic<ActiveRingFormat> active_ring_format{ ActiveRingFormat::none };

// FIR coefficient staging and snapshot: the per-step param block is built into the staging array,
// compared against the last dispatched snapshot and only re-dispatched when the active filter set,
// any coefficient input or the taps content changed.  The snapshot records the last dispatched
// inputs and is refreshed only on dispatch; the GPU taps are written by the coefficient dispatch
// and may be zeroed by effect-state clears or live off->on RESET fills, whose hooks set
// fir_taps_cleared in the same step, before the decision.
constexpr uint32_t max_fir_coeff_entries =
    (Synth::max_oscillators - 1) + (Synth::max_channels + 1) * Synth::max_chain_effects;
static_assert(sizeof(ShaderParams::FIRCoeff) == Synth::fir_coeff_entry_words * sizeof(uint32_t));
static_assert(sizeof(Synth::FirEffectCoeffEntry) == sizeof(ShaderParams::FIRCoeff));
static_assert(max_fir_coeff_entries * sizeof(ShaderParams::FIRCoeff) <= ShaderParams::max_param_range);
ShaderParams::FIRCoeff fir_coeff_stage[max_fir_coeff_entries];
ShaderParams::FIRCoeff fir_coeff_prev[max_fir_coeff_entries];
uint32_t               fir_coeff_prev_count = 0; // 0 = no valid snapshot
bool                   fir_taps_cleared     = false;

} // namespace

// Every oscillator slot gets its own string wave state region, so any slot can play a string
// note.  Slot 0 is the reserved sentinel and stays unused.
static void init_string_state()
{
    constexpr uint32_t state_bytes = Synth::string_state_floats * sizeof(float);

    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        const SubAllocatorBase::Chunk chunk = data_allocator.allocate(state_bytes, Synth::effect_state_alignment);
        assert(chunk.offset + chunk.size <= device_buf_size);
        oscillators[osc_idx].string_state_offs = static_cast<uint32_t>(chunk.offset);
    }
}

// Player tick rate, owned by synth_soundtrack.cpp (set_runtime_tempo_bpm).
using Synth::samples_per_midi_tick;

// A layer carries a filter iff its spread target can modulate at all: a
// nonzero base, an envelope, an LFO, or any routed input.  Spread is the
// only oscillator filter, and a base-0 LFO/MIDI routing still produces
// positive spread at runtime, so the slot must be reserved at note-on.
static bool osc_has_filter(const Synth::Instrument& instrument, uint32_t layer_idx)
{
    const Synth::Oscillator&    layer  = instrument.layers[layer_idx];
    const Synth::TargetBinding& spread = layer.targets[Synth::mod_fir_spread];

    if (layer.fir_type == Synth::fir_off) {
        return false;
    }

    // Multi-band bandpass: any higher band's bindings can modulate even with
    // a zeroed band-0 spread, so the count alone reserves the slot.
    if (layer.fir_type == Synth::fir_bandpass && layer.fir_band_count > 1) {
        return true;
    }

    return spread.base_value != 0.0f || spread.envelope_desc_id || spread.lfo_desc_id || spread.num_inputs;
}

// A layer engages its FIR at note-on iff some staged band has a positive
// spread (bandpass) or the spread itself is positive (single-edge). Same
// rule the modulation step re-evaluates on the running oscillator.
static bool layer_fir_active(const Synth::Oscillator& layer)
{
    if (layer.fir_type == Synth::fir_bandpass) {
        const float spreads[4] = {
            layer.targets[Synth::mod_fir_spread].base_value,
            layer.targets[Synth::mod_fir_band1_spread].base_value,
            layer.targets[Synth::mod_fir_band2_spread].base_value,
            layer.targets[Synth::mod_fir_band3_spread].base_value,
        };
        const uint32_t band_count = layer.fir_band_count < 1    ? 1u
                                    : layer.fir_band_count > 4u ? 4u
                                                                : layer.fir_band_count;
        for (uint32_t band = 0; band < band_count; band++) {
            if (spreads[band] > 0.0f) {
                return true;
            }
        }
        return false;
    }
    return layer.targets[Synth::mod_fir_spread].base_value > 0.0f;
}

// The same engagement rule on the running oscillator, from the modulation
// step's refreshed band values. Single source of truth for the coefficient
// count, the staging decision and the shader's taps gate.
static bool oscillator_fir_active(const RunningOscillator& osc)
{
    if (osc.fir_type == Synth::fir_bandpass) {
        for (uint32_t band = 0; band < osc.fir_band_count; band++) {
            if (osc.band_spread[band] > 0.0f) {
                return true;
            }
        }
        return false;
    }
    return osc.spread > 0.0f;
}

static uint32_t writer_alloc_node(const void* ctx);
static uint16_t writer_resolve_source(const void* ctx, Synth::ModSource source, uint8_t cc_number, uint32_t channel);
static void     writer_configure_dest(const void*               ctx,
                                      uint32_t                  node,
                                      uint16_t                  lfo_node,
                                      float                     base_value,
                                      Synth::SourceOp           lfo_op,
                                      const Synth::SourceParam* sources,
                                      uint32_t                  num_inputs);
static void     writer_configure_lfo(const void*     ctx,
                                     uint32_t        node,
                                     uint16_t        lfo_desc_id,
                                     Synth::SourceOp lfo_op,
                                     float           lfo_depth,
                                     uint16_t        depth_source,
                                     uint16_t        rate_source,
                                     float           rate_scale);
static bool     expand_effects(const InstrumentBank& candidate);

// Expands a candidate bank's effect chains into the runtime: a pure preflight (state
// placement, pool and budget checks) followed by an infallible commit. On preflight
// failure nothing is mutated - the previously committed chains keep sounding - and the
// error is reported. Runs at init and at every bank publish (a step boundary).

static bool expand_effects(const InstrumentBank& candidate)
{
    static Synth::EffectExpansionPlan plan;
    const char*                       error = nullptr;
    if (! Synth::preflight_effect_expansion(candidate, &plan, &error)) {
        d_printf("Effect chain expansion failed: %s\n", error);
        return false;
    }

    reset_effect_pool();
    const Synth::EffectNodeWriter writer = { &candidate,
                                             writer_alloc_node,
                                             writer_resolve_source,
                                             writer_configure_dest,
                                             writer_configure_lfo };
    Synth::commit_effect_expansion(candidate, plan, channel_chains, &master_chain, writer);
    return true;
}

static void init_oscillator_buffers()
{
    assert(Synth::num_channels <= max_mix_channels);

    note_skew_rng.init(note_skew_seed);

    noise_start_counter = 0;

    for (uint32_t channel = 0; channel < max_mix_channels; channel++) {
        mix_channels[channel].chan_output_offs = static_cast<uint32_t>(
            data_allocator.allocate(sizeof(float) * Synth::rt_step_samples * 2, synth_alignment).offset);
        mix_channels[channel].volume      = 1.0f;
        mix_channels[channel].panning     = 0.5f;
        mix_channels[channel].old_volume  = 1.0f;
        mix_channels[channel].old_panning = 0.5f;
    }

    for (uint32_t osc_idx = 0; osc_idx < Synth::max_oscillators; osc_idx++) {
        oscillators[osc_idx].osc_output_offs = static_cast<uint32_t>(
            data_allocator.allocate(sizeof(float) * Synth::rt_step_samples, synth_alignment).offset);
    }

    master_output_offs = static_cast<uint32_t>(
        data_allocator.allocate(sizeof(float) * Synth::rt_step_samples * 2, synth_alignment).offset);

    assert(synth_alignment <= Synth::effect_state_alignment);
    const SubAllocatorBase::Chunk effect_state_region =
        data_allocator.allocate(Synth::effect_state_budget, Synth::effect_state_alignment);
    assert(effect_state_region.size == Synth::effect_state_budget);
    Synth::init_effect_state_region(static_cast<uint32_t>(effect_state_region.offset));

    expand_effects(synth_bank);

    init_fir();
    init_string_state();
}

static void init_modulation()
{
    // Each channel's pitch bend, mod wheel and pressure are externally-driven inputs;
    // propagate_parameters must not overwrite them with a fold.  The two mix leaves
    // (volume CC 7, panning CC 10) and the 16 assignable-CC leaves are likewise
    // externally-driven (process_controller writes them).
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        param_descs[channel_param(channel, chan_param_bend)].kind      = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_mod_wheel)].kind = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_pressure)].kind  = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_volume)].kind    = Synth::ParamKind::external;
        param_descs[channel_param(channel, chan_param_panning)].kind   = Synth::ParamKind::external;
        for (uint32_t slot = 0; slot < Synth::num_assignable_ccs; slot++) {
            param_descs[channel_param(channel, chan_param_cc_0 + slot)].kind = Synth::ParamKind::external;
        }
    }

    // Initialize every channel's mix leaves (volume CC 7, panning CC 10) at the
    // mix_channels defaults (1.0 volume, 0.5 panning); zero-init would mute every
    // channel at the first upload.  init_modulation runs once from init_synth
    // before audio starts, and bank publish does not re-run initialization or
    // reset the channel mix leaves.  Only MIDI CC 7 and CC 10 change the leaf
    // values after initialization (propagate_parameters snapshots prev_value
    // from the value each step, preserving them), so live values persist across
    // publishes.  This matches bend, mod wheel, pressure and assignable-CC
    // leaves.  The one-time guard defends the mix leaves against any future
    // re-entry of initialization.
    static bool mix_leaves_initialized = false;
    if (! mix_leaves_initialized) {
        for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
            Synth::Parameter& volume_parameter  = parameters[channel_param(channel, chan_param_volume)];
            Synth::Parameter& panning_parameter = parameters[channel_param(channel, chan_param_panning)];
            volume_parameter.value = volume_parameter.prev_value = 1.0f;
            panning_parameter.value = panning_parameter.prev_value = 0.5f;
        }
        mix_leaves_initialized = true;
    }

    // Every LSB register starts at the "no LSB seen yet" sentinel; zero-init would
    // mean "LSB 0 received" and corrupt MSB-only normalization (MSB/127).
    for (uint32_t channel = 0; channel < Synth::max_channels; channel++) {
        memset(channel_cc_state[channel].lsb, 0xFF, sizeof(channel_cc_state[channel].lsb));
    }
}

static bool allocate_oscillators(uint8_t* osc_ids, uint32_t num_osc, uint32_t voice_idx, const Instrument& instrument)
{
    uint32_t allocated = 0;

    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators && allocated < num_osc; osc_idx++) {
        if (oscillators[osc_idx].osc_type[0] == WaveType::no_wave) {
            oscillators[osc_idx].voice_id    = static_cast<uint8_t>(voice_idx);
            oscillators[osc_idx].osc_type[0] = instrument.layers[allocated].osc_type[0];
            osc_ids[allocated]               = static_cast<uint8_t>(osc_idx);
            ++allocated;
        }
    }

    if (allocated < num_osc) {
        for (uint32_t rollback_idx = 0; rollback_idx < allocated; rollback_idx++) {

            RunningOscillator& osc = oscillators[osc_ids[rollback_idx]];
            osc.voice_id           = 0;
            osc.osc_type[0]        = WaveType::no_wave;

            osc_ids[rollback_idx] = 0;
        }
        return false;
    }

    return true;
}

void Synth::set_bank_source_callback(void (*callback)())
{
    bank_source_callback.store(callback, std::memory_order_release);
}

const Synth::InstrumentBank& Synth::current_bank()
{
    return synth_bank;
}

void Synth::set_current_bank(const InstrumentBank& bank)
{
    // Preflight and commit the candidate's effect expansion BEFORE installing it: on
    // failure the previous bank and the previously committed chains both stay intact and
    // sounding (nothing was mutated); the failure is reported by expand_effects.
    if (! expand_effects(bank)) {
        return;
    }

    synth_bank = bank;
}

bool Synth::init_synth()
{
    if (compute_family_index == no_queue_family) {
        d_printf("No async compute queue available for synth\n");
        return false;
    }

    // TODO - use project-dependent audio length
    constexpr uint32_t     seconds     = 1;
    constexpr uint32_t     sample_size = sizeof(float);
    constexpr VkDeviceSize output_buf_size =
        mstd::align_up(Synth::rt_sampling_rate * 2U * sample_size * seconds, Synth::rt_step_samples);
    constexpr VkDeviceSize param_buf_size = 1024 * 1024; // TODO

    if (! buffers[output_buf].allocate(Usage::host_only,
                                       output_buf_size,
                                       VK_FORMAT_UNDEFINED,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       { "host audio buffer" }))
        return false;

    if (! buffers[data_buf].allocate(Usage::device_only,
                                     device_buf_size,
                                     VK_FORMAT_UNDEFINED,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     { "audio work buffer" }))
        return false;

    data_allocator.init(device_buf_size);

    if (! buffers[param_buf].allocate(Usage::dynamic,
                                      param_buf_size,
                                      VK_FORMAT_UNDEFINED,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      { "audio param buffer" }))
        return false;

    if (! create_shaders())
        return false;

    if (! allocate_command_buffers_once(&audio_cmd_buf, 1, compute_family_index))
        return false;

    synth_alignment = static_cast<size_t>(vk_phys_props.properties.limits.minMemoryMapAlignment);

    init_oscillator_buffers();

    init_modulation();

    if (! init_synth_os())
        return false;

    return true;
}

void Synth::stop_synth()
{
    stop_synth_os();
}

// Template boilerplate to support different output audio buffer types,
// for example float non-interleaved or int16_t interleaved.

template <typename T, bool interleaved> struct StereoPtr {
    T* left;
    T* right;

    StereoPtr<T, interleaved>& operator+=(size_t offset)
    {
        left += offset;
        right += offset;
        return *this;
    }

    static StereoPtr<T, interleaved> from_buffer(Buffer& buffer)
    {
        T* const     ptr  = buffer.get_ptr<T>();
        const size_t size = static_cast<size_t>(buffer.size());
        return { ptr, ptr + (size / (2 * sizeof(T))) };
    }
};

template <typename T> static StereoPtr<T, false> operator+(StereoPtr<T, false> ptr, size_t offset)
{
    return { ptr.left + offset, ptr.right + offset };
}

template <typename T> struct StereoPtr<T, true> {
    T* data;

    // offset is in frames; interleaved stores 2 samples per frame.
    StereoPtr<T, true>& operator+=(size_t offset)
    {
        data += offset * 2;
        return *this;
    }

    static StereoPtr<T, true> from_buffer(Buffer& buffer) { return { buffer.get_ptr<T>() }; }
};

template <typename T> static StereoPtr<T, true> operator+(StereoPtr<T, true> ptr, size_t offset)
{
    return { ptr.data + offset * 2 };
}

static void copy_audio_data(void* dest, const void* src, size_t size)
{
    memcpy(dest, src, static_cast<uint32_t>(size));
}

template <typename T>
static void copy_audio_data(StereoPtr<T, false> dest, StereoPtr<T, false> src, uint32_t num_samples)
{
    copy_audio_data(dest.left, src.left, num_samples * sizeof(T));
    copy_audio_data(dest.right, src.right, num_samples * sizeof(T));
}

template <typename T> static void copy_audio_data(StereoPtr<T, true> dest, StereoPtr<T, true> src, uint32_t num_samples)
{
    copy_audio_data(dest.data, src.data, num_samples * 2 * sizeof(T));
}

static uint32_t allocate_unused_voice()
{
    return allocate_unused_slot(voices, max_voices, &Voice::active);
}

static bool get_next_midi_event(DispatchedMidiEvent* event, uint32_t end_samples)
{
    static uint32_t last_channel;
    uint32_t        channel = last_channel;

    // The note_on event sets this.
    // Keep 0 for events other than note_on.
    event->release_sample = 0; // Only note_on overwrites this; default keeps other events release-free.

    for (;;) {
        const uint8_t* encoded_delta_time = Synth::midi_delta_times[channel];
        const uint32_t delta_time         = Synth::decode_vlq(&encoded_delta_time);

        const uint32_t delta_samples = delta_time * samples_per_midi_tick;
        const uint32_t event_samples = channel_samples[channel] + delta_samples;

        if (event_samples < end_samples && delta_time < Synth::soundtrack_end_of_channel) {
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

    // Two 4-bit event codes share a byte, high nibble first; event_state toggles which half.
    event_code = (event_code >> (event_state * 4u)) & 0xFu;

    event->event = static_cast<Synth::EvType>(event_code);

    if (event_code <= static_cast<uint8_t>(Synth::EvType::aftertouch)) {
        event->note      = *(Synth::midi_notes[channel]++);
        event->note_data = *(Synth::midi_note_data[channel]++);

        // A note_on carries a stored tick duration; schedule its auto-release.  Stored 0 means
        // unbounded (no stored note_off); a stored value v means v - 1 ticks.
        if (event_code == static_cast<uint8_t>(Synth::EvType::note_on)) {
            const uint8_t* duration_ptr         = Synth::midi_note_durations[channel];
            const uint32_t stored               = Synth::decode_vlq(&duration_ptr);
            Synth::midi_note_durations[channel] = duration_ptr;

            event->release_sample = Synth::soundtrack_note_release_sample(event->time, stored, samples_per_midi_tick);
        }
    }
    else if (static_cast<Synth::EvType>(event_code) == Synth::EvType::controller) {
        event->controller      = *(Synth::midi_ctrl[channel]++);
        event->controller_data = *(Synth::midi_ctrl_data[channel]++);
    }
    else if (static_cast<Synth::EvType>(event_code) == Synth::EvType::tempo) {
        event->tempo_bpm = *(Synth::midi_tempo_bpm[channel]++);
    }
    else {
        assert(event_code == static_cast<uint8_t>(Synth::EvType::pitch_bend));
        const int16_t lo = *(Synth::midi_pitch_bend_lo[channel]++);
        const int16_t hi = *(Synth::midi_pitch_bend_hi[channel]++);
        assert(lo <= 0x7F);
        assert(hi <= 0x7F);
        event->pitch_bend = static_cast<int16_t>((hi << 7) + lo - Synth::soundtrack_pitch_bend_center);
    }

    return true;
}

// Enters a voice's release: marks it releasing (an already-releasing voice
// keeps fading and must never jump twice) and applies the note-off rule to
// every envelope parameter owned by the voice (a backward ping-pong direction
// turns forward, and a quick-release envelope jumps to its release target).
// All release entry points call this before the next update_modulation()
// step, so the change lands before the next envelope evaluation.
static void begin_voice_release(uint32_t voice_idx)
{
    Voice& voice = voices[voice_idx];
    if (voice.releasing) {
        return;
    }
    voice.releasing = true;
    for (uint32_t node_idx = 1; node_idx < total_params; node_idx++) {
        if (param_descs[node_idx].kind == Synth::ParamKind::envelope &&
            parameters[node_idx].sustain_voice == voice_idx) {
            Synth::envelope_note_off(param_descs[node_idx].envelope, &parameters[node_idx].envelope);
        }
    }
}

static void process_note_off(uint32_t delta_samples, const DispatchedMidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    if (voice_idx) {
        assert(voices[voice_idx].active);

        if (channel_cc_state[channel].sustain_pedal_down) {
            voices[voice_idx].sustained = true; // the pedal holds the voice; note-off defers
        }
        else {
            begin_voice_release(voice_idx);
        }
    }
}

// Pedal-up sweep: every key-up-held voice on the channel releases through the
// same path as key-up.
static void release_sustained_voices(uint32_t channel)
{
    for (uint32_t voice_idx = 1; voice_idx < max_voices; voice_idx++) {
        Voice& voice = voices[voice_idx];
        if (voice.active && voice.sustained && voice.channel == channel) {
            voice.sustained = false;
            begin_voice_release(voice_idx);
        }
    }
}

static void free_oscillator(uint32_t osc_idx)
{
    oscillators[osc_idx].osc_type[0]  = WaveType::no_wave;
    oscillators[osc_idx].voice_id     = 0;
    oscillators[osc_idx].terminating  = false;
    oscillators[osc_idx].decaying     = false; // slot reuse and bank publishes cannot leak decay state
    oscillators[osc_idx].decay_factor = 1.0f;

    for (uint32_t role = 0; role < num_osc_roles; role++) {
        param_descs[osc_param(osc_idx, role)] = {};
        parameters[osc_param(osc_idx, role)]  = {};
    }
}

// Maps a binding's input-source role to the concrete parameter id of the channel or voice leaf
// that carries its live value.  none (and anything unrecognized) maps to the value-0 sentinel.
static uint32_t resolve_source(ModSource source, uint8_t cc_number, uint32_t channel, uint32_t voice_idx)
{
    switch (source) {
        case ModSource::pitch_bend:
            return channel_param(channel, chan_param_bend);
        case ModSource::mod_wheel:
            return channel_param(channel, chan_param_mod_wheel);
        case ModSource::channel_pressure:
            return channel_param(channel, chan_param_pressure);
        case ModSource::cc: {
            const uint32_t slot = cc_number - Synth::cc_msb_base;
            assert(slot < Synth::num_assignable_ccs); // preflight validated 16..31
            return channel_param(channel, chan_param_cc_0 + slot);
        }
        case ModSource::velocity:
            return voice_param(voice_idx, voice_input_velocity);
        case ModSource::aftertouch:
            return voice_param(voice_idx, voice_input_aftertouch);
        case ModSource::pressure_combine:
            return voice_param(voice_idx, voice_input_pressure_combine);
        default:
            return 0;
    }
}

static uint32_t writer_alloc_node(const void*)
{
    const uint32_t node = allocate_effect_pool_node();
    assert(node); // the preflight counted this node into the pool budget
    return node;
}

static uint16_t writer_resolve_source(const void*, ModSource source, uint8_t cc_number, uint32_t channel)
{
    return static_cast<uint16_t>(resolve_source(source, cc_number, channel, 0));
}

static void writer_configure_dest(const void*               ctx,
                                  uint32_t                  node,
                                  uint16_t                  lfo_node,
                                  float                     base_value,
                                  Synth::SourceOp           lfo_op,
                                  const Synth::SourceParam* sources,
                                  uint32_t                  num_inputs)
{
    parameters[node] = {};
    Synth::configure_plain(&param_descs[node],
                           base_value,
                           0, // effects have no envelope
                           1.0f,
                           lfo_node,
                           lfo_op,
                           sources,
                           num_inputs);
}

static void writer_configure_lfo(const void*     ctx,
                                 uint32_t        node,
                                 uint16_t        lfo_desc_id,
                                 Synth::SourceOp lfo_op,
                                 float           lfo_depth,
                                 uint16_t        depth_source,
                                 uint16_t        rate_source,
                                 float           rate_scale)
{
    const InstrumentBank& candidate = *static_cast<const InstrumentBank*>(ctx);
    Synth::configure_lfo(&param_descs[node],
                         candidate.lfos.entries[lfo_desc_id - 1],
                         lfo_op,
                         lfo_depth,
                         depth_source,
                         rate_source,
                         rate_scale);
}

// Expands one layer's target into its {dest, env, lfo} node triple at the layer's oscillator slot.
// The dest folds, in order, an additive source from the envelope generator, a source from the LFO
// generator (per lfo_op), then each routed input source; absent generators contribute no source.
// Each layer instantiates its own generators, so layers with the same descriptor id evaluate the
// same value in phase without sharing nodes.  Every route is data, not code here.
// An in-place restart passes the level of the previous envelope node in env_seed.  The restarted
// envelope ramps from that level through its first segment.  Zero means no seed (a new note).
static void configure_target_modulation(const Instrument& instrument,
                                        ModTarget         target,
                                        uint32_t          dest_role,
                                        uint32_t          osc_slot,
                                        uint32_t          channel,
                                        uint32_t          voice_idx,
                                        uint32_t          layer_idx,
                                        float             base_extra,
                                        float             env_seed)
{
    const TargetBinding& binding = instrument.layers[layer_idx].targets[target];

    const uint32_t env_node = osc_param(osc_slot, dest_role + 1);
    const uint32_t lfo_node = osc_param(osc_slot, dest_role + 2);

    // Envelope generator node.  Clear it first to drop stale state from a reused slot: an absent
    // envelope leaves it kind external (unreferenced, value 0), so it contributes no source.
    param_descs[env_node] = {};
    parameters[env_node]  = {};
    if (binding.envelope_desc_id) {
        param_descs[env_node].kind              = Synth::ParamKind::envelope;
        param_descs[env_node].envelope          = synth_bank.envelopes.entries[binding.envelope_desc_id - 1];
        parameters[env_node].sustain_voice      = static_cast<uint16_t>(voice_idx);
        parameters[env_node].envelope.old_value = env_seed;
    }

    // LFO generator node.  Clear it first; configure_lfo overrides the descriptor when the layer has
    // an LFO, otherwise it stays kind external (unreferenced, since configure_plain gets 0 below).
    param_descs[lfo_node] = {};
    parameters[lfo_node]  = {};
    if (binding.lfo_desc_id) {
        Synth::configure_lfo(&param_descs[lfo_node],
                             synth_bank.lfos.entries[binding.lfo_desc_id - 1],
                             binding.lfo_op,
                             binding.lfo_depth,
                             static_cast<uint16_t>(resolve_source(binding.lfo_depth_source, 0, channel, voice_idx)),
                             static_cast<uint16_t>(resolve_source(binding.lfo_rate_source, 0, channel, voice_idx)),
                             binding.lfo_rate_scale_ms);
    }

    // Resolve the target's routed input sources to concrete param ids.
    Synth::SourceParam sources[Synth::max_mod_inputs];
    for (uint32_t input_idx = 0; input_idx < binding.num_inputs; input_idx++) {
        const ModInput& input = binding.inputs[input_idx];
        sources[input_idx] = { static_cast<uint16_t>(resolve_source(input.source, input.cc_number, channel, voice_idx)),
                               input.scale,
                               input.op };
    }

    Synth::configure_plain(&param_descs[osc_param(osc_slot, dest_role)],
                           binding.base_value + base_extra,
                           binding.envelope_desc_id ? static_cast<uint16_t>(env_node) : uint16_t(0),
                           binding.envelope_depth,
                           binding.lfo_desc_id ? static_cast<uint16_t>(lfo_node) : uint16_t(0),
                           binding.lfo_op,
                           sources,
                           binding.num_inputs);
}

// Wraps a parameter node's value/prev_value pair as an externally-driven leaf source.
static Synth::ExternalLeaf leaf(uint32_t node)
{
    return Synth::ExternalLeaf{ &parameters[node].value, &parameters[node].prev_value };
}

// Seeds an externally-driven leaf (a MIDI input source) with an initial value+prev so a consumer
// reads it with zero lag on the first step.
static void set_input_leaf(uint32_t node, float value)
{
    param_descs[node]      = {};
    param_descs[node].kind = Synth::ParamKind::external;
    Synth::write_external_leaf(value, leaf(node));
}

// Values of Instrument::voice_mode.  Zero (multiple) is the default for every bank.
constexpr uint8_t voice_mode_reuse_per_note = 1;
constexpr uint8_t voice_mode_single         = 2;

// A playing voice restarts in place only while it still holds every layer of the instrument
// which it plays.  A layer which already ended, or a bank swap which changed the layer count,
// sends the note to a new voice instead.
static bool voice_restartable_in_place(const Voice& voice, uint32_t layer_count)
{
    if (voice.osc_count != layer_count) {
        return false;
    }
    for (uint32_t osc_pos = 0; osc_pos < voice.osc_count; ++osc_pos) {
        if (oscillators[voice.osc_ids[osc_pos]].source_layer != osc_pos) {
            return false;
        }
    }
    return true;
}

static void process_note_on(uint32_t delta_samples, const DispatchedMidiEvent& event)
{
    const uint32_t channel = event.channel;
    const uint32_t note    = event.note;

    if (! synth_bank.channel_enabled[channel]) {
        return;
    }

    const uint8_t target_instrument =
        route_instrument(synth_bank.channel_zones[channel], Synth::max_instr_per_channel, static_cast<uint8_t>(note));

    const Instrument& instrument  = synth_bank.instruments.entries[target_instrument];
    const uint32_t    layer_count = instrument.layer_count;
    assert(layer_count >= 1 && layer_count <= max_layers);

    // A note still alive on this key (held or releasing, possibly with some layers already
    // silenced) is replaced only after the new note has its voice and oscillators.  When any
    // allocation fails, the new note is dropped and the playing voice and its mapping stay
    // untouched.
    const uint32_t existing_voice = note_to_voice[channel][note];

    // The voice mode can pick a playing voice of this instrument to restart in place.
    // Zero means the note takes a new voice.
    uint32_t reset_voice = 0;
    if (instrument.voice_mode == voice_mode_reuse_per_note) {
        // Only the same note on the same instrument restarts; another instrument's voice stays.
        if (existing_voice && ! voices[existing_voice].note_finished &&
            voices[existing_voice].instrument == target_instrument &&
            voice_restartable_in_place(voices[existing_voice], layer_count)) {
            reset_voice = existing_voice;
        }
    }
    else if (instrument.voice_mode == voice_mode_single) {
        // One note sounds per instrument, across all channels.  The lowest live voice restarts.
        for (uint32_t scan_idx = 1; scan_idx < max_voices && ! reset_voice; scan_idx++) {
            const Voice& scan_voice = voices[scan_idx];
            if (scan_voice.active && ! scan_voice.note_finished && scan_voice.instrument == target_instrument &&
                voice_restartable_in_place(scan_voice, layer_count)) {
                reset_voice = scan_idx;
            }
        }
    }

    // The envelope level of each target which carries over a restart, per instrument layer.
    float    env_seeds[max_layers][Synth::num_mod_targets] = {};
    uint32_t previous_channel                              = 0;
    uint32_t previous_note                                 = 0;
    uint32_t voice_idx                                     = reset_voice;

    if (reset_voice) {
        // A restart updates the voice's own oscillators in place.  It allocates nothing, so it
        // cannot fail and the playing sound is never at risk.
        const Voice& previous = voices[reset_voice];

        previous_channel = previous.channel;
        previous_note    = previous.note;

        // Read the envelope levels before any node is cleared.  Each envelope node carries its
        // current level (the end-of-step value which its dest consumed) into the restart, so no
        // target drops to its first point.
        for (uint32_t osc_pos = 0; osc_pos < previous.osc_count; ++osc_pos) {
            const RunningOscillator& previous_osc = oscillators[previous.osc_ids[osc_pos]];
            for (const ModTargetNode& node : mod_target_nodes) {
                const uint32_t env_node = osc_param(previous.osc_ids[osc_pos], node.dest_role + 1);
                if (param_descs[env_node].kind == Synth::ParamKind::envelope) {
                    env_seeds[previous_osc.source_layer][node.target] = parameters[env_node].prev_value;
                }
            }
        }
    }
    else {
        voice_idx = allocate_unused_voice();
        if (! voice_idx) {
            d_printf("All voices are active, dropping note %u on channel %u\n", note, channel);
            return;
        }
    }

    Voice& voice         = voices[voice_idx];
    voice.channel        = static_cast<uint8_t>(channel);
    voice.note           = static_cast<uint8_t>(note);
    voice.instrument     = target_instrument;
    voice.active         = true;
    voice.releasing      = false;
    voice.note_finished  = false;
    voice.sustained      = false;
    voice.release_sample = event.release_sample;

    voice.release_total_ticks = Synth::sustain_decay_ticks(instrument.release_seconds);

    // Per-voice input leaves the bindings route from.  velocity is the note's constant; aftertouch
    // starts at zero (driven by poly-aftertouch events); pressure_combine is recomputed each step as
    // max(aftertouch, channel pressure) by advance_parameters.
    const float velocity = static_cast<float>(event.note_data) / 127.0f;
    set_input_leaf(voice_param(voice_idx, voice_input_velocity), velocity);
    set_input_leaf(voice_param(voice_idx, voice_input_aftertouch), 0.0f);
    set_input_leaf(voice_param(voice_idx, voice_input_pressure_combine), 0.0f);

    if (reset_voice) {
        assert(voice.osc_count == layer_count);
    }
    else {
        if (! allocate_oscillators(voice.osc_ids, layer_count, voice_idx, instrument)) {
            // allocate_oscillators frees its own partial claims.  The voice slot goes back to the
            // free pool, and the playing voice and its mapping stay untouched.
            d_printf("All oscillators are active, dropping note %u on channel %u\n", note, channel);
            voice.active    = false;
            voice.osc_count = 0;
            return;
        }
        voice.osc_count = static_cast<uint8_t>(layer_count);
    }

    // A restart for another key gives up the old key.  The old mapping is cleared only while
    // it still points to this voice.
    if (reset_voice && (previous_channel != channel || previous_note != note) &&
        note_to_voice[previous_channel][previous_note] == voice_idx) {
        note_to_voice[previous_channel][previous_note] = 0;
    }

    // Single mode: every other live voice of this instrument fades out over its final block.
    // The sweep runs only after the new note succeeded.  A swept voice gives up its key only
    // while the key still points to it.
    if (instrument.voice_mode == voice_mode_single) {
        for (uint32_t sweep_idx = 1; sweep_idx < max_voices; sweep_idx++) {
            Voice& swept = voices[sweep_idx];
            if (sweep_idx != voice_idx && swept.active && ! swept.note_finished &&
                swept.instrument == target_instrument) {
                if (note_to_voice[swept.channel][swept.note] == sweep_idx) {
                    note_to_voice[swept.channel][swept.note] = 0;
                }
                swept.note_finished = true;
            }
        }
    }

    // The new note owns its voice now.  The replaced note fades out over its final block
    // instead of being cut, and gives up the note mapping to the new note.
    if (existing_voice && existing_voice != voice_idx) {
        voices[existing_voice].note_finished = true;
    }
    note_to_voice[channel][note] = static_cast<uint8_t>(voice_idx);

    // One per-note pitch skew applies to every layer; each layer adds its own skew on top.
    const float note_skew = Synth::random_pitch_skew(&note_skew_rng, instrument.note_skew_semitones);

    for (uint32_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {

        RunningOscillator& osc = oscillators[voice.osc_ids[layer_idx]];
        osc.layer_idx          = static_cast<uint8_t>(layer_idx);
        osc.source_layer       = static_cast<uint8_t>(layer_idx);
        osc.midi_channel       = channel;
        osc.output_channel     = channel;
        osc.note               = note;
        osc.freq_mult          = 1;

        // The slot's previous layer fields are read before they are overwritten: a takeover out of a
        // string keeps the chain, so the shader adds the new pluck to the ringing string.
        const bool chain_continues = reset_voice && osc.osc_type[0] == WaveType::string_wave &&
                                     ! (osc.osc_type[1] != WaveType::no_wave && osc.osc_mode == osc_mode_hard_sync);

        const Oscillator& layer = instrument.layers[layer_idx];
        osc.osc_type[0]         = layer.osc_type[0];
        osc.osc_type[1]         = layer.osc_type[1];
        osc.osc_mode            = layer.osc_mode;
        osc.mod_ratio           = layer.mod_ratio;
        osc.string_brightness   = layer.string_brightness;
        osc.octave_offset       = layer.octave_offset;
        osc.fir_type            = layer.fir_type;
        osc.fir_band_count = layer.fir_type == Synth::fir_bandpass ? (layer.fir_band_count < 1   ? 1
                                                                      : layer.fir_band_count > 4 ? 4
                                                                                                 : layer.fir_band_count)
                                                                   : 1;
        // The per-note pitch skew rides on the pitch target: it joins the
        // binding's base value in the dest parameter's initial state.
        const float layer_skew = Synth::random_pitch_skew(&note_skew_rng, instrument.layer_skew_semitones);
        if (! reset_voice) {
            // A new note starts every running quantity from silence.
            osc.phase      = 0.0f;
            osc.mod_phase  = 0.0f;
            osc.old_volume = 0.0f;
            // old_panning keeps its base-value seed: the dest node is stale until the first advance.
            osc.old_panning = layer.targets[mod_panning].base_value;
        }
        // Otherwise the oscillator keeps running through the restart.  Classification of the fields
        // which the note-on initialization touches:
        //  - Persist (running signal state; the waveform stays continuous): phase, mod_phase,
        //    old_volume and old_panning (the block gradient continues from the last rendered
        //    values), the FIR history (clear_fir_hist is left alone), fir_active (the engagement
        //    edge detector), the slot constants (osc_output_offs, string_state_offs, fir offsets)
        //    and the volume, pan and pitch values which update_modulation rewrites every step.
        //  - Reset (note-dependent): layer parameters, midi/output channel and note, the pitch
        //    skew in the pitch dest base, the noise and note seeds, the terminating flag and the
        //    release decay control (decay_mult, decay_factor, decay_tick, decaying).  The
        //    envelope and LFO generator nodes restart through configure_target_modulation, with
        //    the envelope levels carried by env_seeds.
        osc.terminating  = false;
        osc.decay_mult   = Synth::sustain_decay_per_tick(voice.release_total_ticks);
        osc.decay_factor = 1.0f;
        osc.decay_tick   = 0;
        osc.decaying     = false;

        noise_start_counter += noise_start_stride;
        osc.noise_counter = noise_start_counter;
        osc.note_seed     = noise_start_counter;
        if (! chain_continues) {
            osc.string_chain = osc.note_seed;
        }

        // Expand each modulation target into its node triple at this oscillator's slot.
        const uint32_t osc_slot = voice.osc_ids[layer_idx];
        for (const ModTargetNode& node : mod_target_nodes) {
            float base_extra = node.target == Synth::mod_pitch ? note_skew + layer_skew : 0.0f;
            if (node.target == Synth::mod_fir_slope) {
                // Legacy scalar inheritance: an unrouted (zeroed) slope binding starts from the
                // layer's fir_slope scalar, a routed one from its own base.  The seed enters the
                // plain descriptor base (configure_plain writes base_value + base_extra), not the
                // running parameter value: propagation restarts the dest from the descriptor base
                // every step, so a value-only seed would be clobbered and the legacy slope would
                // collapse to the 11-tap floor.
                base_extra =
                    Synth::osc_fir_slope_seed_value(layer.targets[Synth::mod_fir_slope], layer.fir_slope_percent) -
                    layer.targets[Synth::mod_fir_slope].base_value;
            }
            configure_target_modulation(instrument,
                                        node.target,
                                        node.dest_role,
                                        osc_slot,
                                        channel,
                                        voice_idx,
                                        layer_idx,
                                        base_extra,
                                        env_seeds[layer_idx][node.target]);
        }

        // Trigger-time gate: the oscillator only sounds when its final
        // equivalent MIDI pitch at note-on lies within [0, 127].  The onset
        // evaluation is read-only, so no parameter state moves here.
        const float onset_pitch = Synth::oscillator_final_pitch(
            static_cast<int>(osc.note),
            osc.octave_offset,
            Synth::param_onset_value(parameters,
                                     param_descs,
                                     static_cast<uint16_t>(osc_param(osc_slot, osc_pitch_dest))));
        if (! Synth::oscillator_pitch_in_gate(onset_pitch)) {
            // Trigger-time rejection: the oscillator never sounds.  The normal
            // termination path frees it on the first modulation step, so the
            // voice's other oscillators and the voice lifetime machinery are
            // untouched.
            osc.terminating = true;
            osc.volume      = 0.0f;
        }
    }

    // Set up each oscillator's FIR once spread bindings are resolved.
    for (uint32_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        RunningOscillator& osc      = oscillators[voice.osc_ids[layer_idx]];
        const uint32_t     osc_slot = voice.osc_ids[layer_idx];

        // A restarted oscillator which already runs this filter keeps its history and its
        // engagement state; a restarted one which gains or loses the filter starts it like a new note.
        const bool keeps_filter = reset_voice && (osc.fir_taps_offs != 0) == osc_has_filter(instrument, layer_idx);

        if (osc_has_filter(instrument, layer_idx)) {
            osc.fir_taps_offs   = fir_slots[osc_slot].coeff_offs;
            osc.fir_memory_offs = fir_slots[osc_slot].history_offs;
            if (! keeps_filter) {
                osc.clear_fir_hist = true;
            }
        }
        else {
            osc.fir_taps_offs   = 0;
            osc.fir_memory_offs = 0;
            osc.clear_fir_hist  = false;
        }
        if (! keeps_filter) {
            // Note-on seed from the layer's own binding bases; the first
            // modulation step re-evaluates it from the dest parameters.
            osc.fir_active = layer_fir_active(instrument.layers[layer_idx]);
        }
    }
}

static void process_aftertouch(uint32_t delta_samples, const DispatchedMidiEvent& event)
{
    const uint32_t channel   = event.channel;
    const uint32_t note      = event.note;
    const uint32_t voice_idx = note_to_voice[channel][note];

    if (voice_idx) {
        assert(voices[voice_idx].active);

        // Drive the per-note aftertouch leaf (value and prev_value) so the combine reads it with zero lag.
        const uint32_t aftertouch_node = voice_param(voice_idx, voice_input_aftertouch);
        const float    pressure        = static_cast<float>(event.note_data) / 127.0f;
        Synth::write_external_leaf(pressure, leaf(aftertouch_node));
    }
}

static void process_controller(uint32_t, const DispatchedMidiEvent& event)
{
    if (event.controller == Synth::sustain_pedal_cc) {
        const bool was_down = channel_cc_state[event.channel].sustain_pedal_down;
        const bool down     = Synth::apply_sustain_pedal(&channel_cc_state[event.channel], event.controller_data);
        if (was_down && ! down) {
            release_sustained_voices(event.channel);
        }
    }
    else if (event.controller == mod_wheel_cc) {
        const uint32_t node = channel_param(event.channel, chan_param_mod_wheel);
        Synth::write_external_leaf(static_cast<float>(event.controller_data) / 127.0f, leaf(node));
    }
    else if (Synth::mix_cc_role(event.controller) != Synth::MixRole::none) {
        Synth::write_mix_cc(event.controller,
                            event.controller_data,
                            leaf(channel_param(event.channel, chan_param_volume)),
                            leaf(channel_param(event.channel, chan_param_panning)));
        return;
    }
    else if (Synth::is_assignable_cc(event.controller)) {
        const float value =
            Synth::apply_channel_cc(&channel_cc_state[event.channel], event.controller, event.controller_data);
        const uint32_t node =
            channel_param(event.channel, chan_param_cc_0 + Synth::assignable_cc_slot(event.controller));
        Synth::write_external_leaf(value, leaf(node));
    }
    // Other controllers ignored.
}

static void process_pitch_bend(uint32_t delta_samples, const DispatchedMidiEvent& event)
{
    // Drive the channel bend leaf (value and prev_value) so consumers read it with zero lag.
    const uint32_t bend_node = channel_param(event.channel, chan_param_bend);
    const float    bend      = Synth::pitch_bend_to_semitones(event.pitch_bend, default_pitch_bend_range_semitones);
    Synth::write_external_leaf(bend, leaf(bend_node));
}

static void process_channel_pressure(uint32_t delta_samples, const DispatchedMidiEvent& event)
{
    // Drive the per-channel pressure leaf (value and prev_value) so the combine reads it with zero lag.
    const uint32_t pressure_node = channel_param(event.channel, chan_param_pressure);
    Synth::write_external_leaf(static_cast<float>(event.note_data) / 127.0f, leaf(pressure_node));
}

static void process_tempo(uint32_t, const DispatchedMidiEvent& event)
{
    // The delta argument is unused: the new tick rate applies from the next event on.
    Synth::set_runtime_tempo_bpm(event.tempo_bpm);
}

using EventHandler = void (*)(uint32_t delta_samples, const DispatchedMidiEvent& event);

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

    // Drop malformed events before they index per-channel/per-note arrays.
    if (event.channel >= max_channels) {
        return;
    }
    if ((event.event == Synth::EvType::note_off || event.event == Synth::EvType::note_on ||
         event.event == Synth::EvType::aftertouch) &&
        event.note >= 128) {
        return;
    }

    // Live input notes have no stored duration; they end on a real note_off, not an auto-release,
    // so they dispatch with release_sample 0.
    const EventHandler handler = event_handlers[static_cast<uint8_t>(event.event)];

    if (handler) {
        DispatchedMidiEvent dispatched;
        static_cast<Synth::MidiEvent&>(dispatched) = event;
        dispatched.release_sample                  = 0;
        handler(0, dispatched);
    }
}

static void process_events(uint32_t start_samples, uint32_t end_samples)
{
    DispatchedMidiEvent event;

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
    static VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                        nullptr, // pNext
                                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                        VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_NONE,
                                        VK_ACCESS_2_NONE };

    // The chained source (set at the end of the previous call) covers the most
    // recent producer.  extra_src_* additionally sources an earlier producer in a
    // different stage, so a single dependency can make several writes visible at
    // once (e.g. compute-written FIR taps AND a transfer history fill).  It applies
    // only to this call; the chain reset below restores src = dst.
    barrier.srcStageMask |= extra_src_stage;
    barrier.srcAccessMask |= extra_src_access;
    barrier.dstStageMask  = dst_stage;
    barrier.dstAccessMask = dst_access;

    static const VkDependencyInfo dependency_info = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        nullptr,  // pNext
        0,        // dependecyFlags
        1,        // memoryBarrierCount,
        &barrier, // pMemoryBarriers
        0,        // bufferMemoryBarrierCount
        nullptr,  // pBufferMemoryBarriers
        0,        // imageMemoryBarrierCount
        nullptr   // pImageMemoryBarriers
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
        VK_NULL_HANDLE,                    // dstSet
        0,                                 // dstBinding
        0,                                 // dstArrayElement
        1,                                 // descriptorCount
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // descriptorType
        nullptr,                           // pImageInfo
        &buffer_info,                      // pBufferInfo
        nullptr                            // pTexelBufferView
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
    uint32_t           sound_offs; // FLOAT index of the buffer to process in place
};

// Runs the targets as dependency-depth waves: wave d = the d-th enabled effect of
// every target, packed into one dispatch; a barrier precedes each wave.  Chains are
// sequential per buffer, but the buffers are distinct, so one wave has no conflicts.
// Defined below apply_effects; the dispatch needs a FIR effect's staging
// result to drive the shader's bypass flag.
static bool stage_effect_fir_entry(const EffectInstance& instance, Synth::FirEffectCoeffEntry* entry);

static void apply_effects(const EffectTarget* targets, uint32_t num_targets)
{
    for (uint32_t depth = 0;; depth++) {

        // Gather the depth-th enabled effect of every target first, so the param
        // buffer is only allocated for a non-empty wave.  An empty depth means all
        // chains are exhausted and the pass is done.
        const EffectInstance* wave_instances[max_mix_channels];
        uint32_t              wave_sound_offs[max_mix_channels];
        uint32_t              wave_count = 0;

        for (uint32_t target_idx = 0; target_idx < num_targets; target_idx++) {

            const EffectInstance* const instance = nth_enabled_effect(*targets[target_idx].chain, depth);
            if (! instance) {
                continue;
            }

            wave_instances[wave_count]  = instance;
            wave_sound_offs[wave_count] = targets[target_idx].sound_offs;
            ++wave_count;
        }

        if (! wave_count) {
            return;
        }

        const uint32_t param_size = wave_count * static_cast<uint32_t>(sizeof(ShaderParams::EffectParams));
        const uint32_t param_offs = static_cast<uint32_t>(param_allocator.allocate(param_size, synth_alignment).offset);

        for (uint32_t wave_idx = 0; wave_idx < wave_count; wave_idx++) {

            const uint32_t cur_param_offs =
                param_offs + wave_idx * static_cast<uint32_t>(sizeof(ShaderParams::EffectParams));
            ShaderParams::EffectParams& param = get_param<ShaderParams::EffectParams>(cur_param_offs);

            param.type       = static_cast<uint32_t>(wave_instances[wave_idx]->type);
            param.sound_offs = wave_sound_offs[wave_idx];
            param.state_offs = wave_instances[wave_idx]->state_offs / 4;
            param.bypass     = 0;
            // A multiband FIR whose coefficient entry failed to stage (all
            // bands inactive) dispatches as pass-through instead of
            // convolving with a stale kernel; the shader keeps the history
            // advancing.  The plain FIR never reads the flag.
            Synth::FirEffectCoeffEntry fir_entry;
            if (wave_instances[wave_idx]->type == Synth::EffectType::fir_multiband &&
                ! stage_effect_fir_entry(*wave_instances[wave_idx], &fir_entry)) {
                param.bypass = 1;
            }
            memcpy(param.params, wave_instances[wave_idx]->params, sizeof(param.params));
        }

        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[effect_pipe]);

        static const PushDescriptorInfo push_effect_data = { effect_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
        push_descriptor(push_effect_data, 0);

        static const PushDescriptorInfo push_effect_param = { effect_pipe,
                                                              1,
                                                              0,
                                                              param_buf,
                                                              ShaderParams::max_param_range };
        push_descriptor(push_effect_param, param_offs);

        vkCmdDispatch(audio_cmd_buf, wave_count, 1, 1);
    }
}

// FIR coefficient staging and snapshot live in the file's anonymous
// namespace above; compute_fir_coefficients builds, compares and dispatches
// the block.

// Copies each modulated effect param's current graph value into the param float the effect shader
// reads (and compute_fir_coefficients reads for the FIR effect).  Unmodulated params (src_param_id
// 0) keep their constant value.  Also evaluates the enable decision for every instance and
// records a RESET vkCmdFillBuffer on the off->on edge.
static void pull_effect_param_chain(EffectChain* chain, bool* reset_barrier_needed)
{
    for (uint32_t effect_idx = 0; effect_idx < chain->num_effects; effect_idx++) {
        EffectInstance& instance = chain->effects[effect_idx];

        // A wired enable routing takes its live state from the resolved leaf
        // each block; the saved checkbox governs only unwired instances.  On
        // the off->on transition the instance state is cleared once, so a
        // re-enabled effect starts from silence instead of replaying what its
        // last live run left in the buffer.  instance.enabled holds the
        // previous block's decision here, for wired and unwired instances
        // alike: the commit seeds retained slots with it, so transitions that
        // cross a publish stay visible.
        const bool previous_enabled = instance.enabled;
        bool       live_enabled     = instance.saved_enabled;
        if (instance.enable_param_id) {
            live_enabled = Synth::effect_live_enabled(instance.enable_param_id,
                                                      parameters[instance.enable_param_id].value,
                                                      instance.saved_enabled);
        }
        if (Synth::effect_needs_reset(previous_enabled, live_enabled) && instance.state_offs) {
            if (! *reset_barrier_needed) {
                // The state range was last written by a live effect run,
                // so the fill sources the compute stage explicitly: the
                // chained producer alone does not cover it.
                memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                *reset_barrier_needed = true;
            }
            vkCmdFillBuffer(audio_cmd_buf,
                            buffers[data_buf].get_buffer(),
                            instance.state_offs,
                            Synth::get_effect_state_bytes(instance.type),
                            0); // data
            // A FIR effect's coefficient taps live at the head of this state range, so the fill
            // may have zeroed them; the coefficient snapshot must not survive it.
            fir_taps_cleared = true;
        }
        instance.enabled = live_enabled;

        if (! instance.enabled) {
            continue;
        }
        for (uint32_t param_idx = 0; param_idx < Synth::max_effect_param_floats; param_idx++) {
            if (instance.src_param_id[param_idx]) {
                instance.params[param_idx] = parameters[instance.src_param_id[param_idx]].value;
            }
        }
        // FIR band values refresh through the same graph-leaf mechanism; the commit records one
        // node per modulated active-band parameter.
        if (is_fir_effect_type(instance.type)) {
            for (uint32_t band_param = 0; band_param < Synth::num_fir_effect_band_params; band_param++) {
                if (instance.fir_bands.src_param_id[band_param]) {
                    instance.fir_bands.params[band_param] =
                        parameters[instance.fir_bands.src_param_id[band_param]].value;
                }
            }
        }
    }
}

static void pull_effect_params()
{
    bool reset_barrier_needed = false;
    for (uint32_t channel = 0; channel < max_mix_channels; channel++) {
        pull_effect_param_chain(&channel_chains[channel], &reset_barrier_needed);
    }
    pull_effect_param_chain(&master_chain, &reset_barrier_needed);
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

            Synth::Parameter& param = parameters[node_idx];

            // The pair written here spans exactly this step's tick interval: value is the
            // envelope at the starting tick, prev_value one tick ahead.  Consumers read
            // prev_value, so a plain dest gets the end-of-step value while the carried
            // previous dest value forms the old end of its old/new gradient.
            const Synth::EnvelopeDescriptor& envelope_desc = gen.envelope;
            const uint32_t                   last_point    = envelope_desc.num_points - 1;
            const bool                       entry_parked =
                param.envelope.point == last_point && param.envelope.tick == envelope_desc.points[last_point].position;

            param.value      = Synth::eval_envelope(envelope_desc, &param.envelope, sustain);
            param.prev_value = Synth::eval_envelope_at(envelope_desc, param.envelope);

            // Count how long a released envelope has entered already parked on its final
            // point; the volume target times its final fade tick from this (update_modulation).
            param.last_point_ticks = static_cast<uint16_t>(entry_parked && ! sustain ? param.last_point_ticks + 1 : 0);
        }
        else if (gen.kind == Synth::ParamKind::lfo) {
            // One-step delay: the depth/rate source leaves read prev_value.
            parameters[node_idx].value = Synth::eval_sourced_lfo(gen.lfo,
                                                                 parameters[gen.lfo.depth_param_id].prev_value,
                                                                 parameters[gen.lfo.rate_param_id].prev_value,
                                                                 parameters[node_idx].lfo_tick);
            parameters[node_idx].lfo_tick++;
        }
    }

    Synth::propagate_parameters(parameters, param_descs, total_params);
}

// True when this oscillator's volume target is due its final fade block under the voice-lifetime
// rules: a running instrument release decay owns termination while it is still incomplete and the
// dest level is audible (it keeps decaying through note-off and frees at the cutoff); an
// oscillator without a volume envelope fades on the first released block; every released
// envelope that ends parked at the final point completes after the standard two entered-parked
// steps, then the final fade.
static bool volume_termination_due(uint32_t osc_idx)
{
    const uint32_t                env_node = osc_param(osc_idx, osc_volume_env);
    const Synth::ParamDescriptor& env_desc = param_descs[env_node];

    const Voice& voice = voices[oscillators[osc_idx].voice_id];
    if (voice.release_total_ticks && oscillators[osc_idx].decaying) {
        if (oscillators[osc_idx].decay_tick >= voice.release_total_ticks) {
            return true;
        }
        if (Synth::sustain_decay_owns_termination(oscillators[osc_idx].decaying,
                                                  parameters[osc_param(osc_idx, osc_volume_dest)].value)) {
            return false;
        }
    }

    if (env_desc.kind != Synth::ParamKind::envelope) {
        return true;
    }
    return parameters[env_node].last_point_ticks >= 2;
}

// Clamps to [0, 1]; NaN maps to 0.  String damping destinations are unbounded after routing.
static float unit_range_clamp(float value)
{
    return value >= 0.0f ? (value <= 1.0f ? value : 1.0f) : 0.0f;
}

static void update_modulation()
{
    advance_parameters();

    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        RunningOscillator& osc = oscillators[osc_idx];
        if (osc.osc_type[0] == WaveType::no_wave) {
            continue;
        }

        Voice& voice = voices[osc.voice_id];

        osc.pitch              = Synth::oscillator_final_pitch(static_cast<int>(osc.note),
                                                               osc.octave_offset,
                                                               parameters[osc_param(osc_idx, osc_pitch_dest)].value);
        const float new_spread = parameters[osc_param(osc_idx, osc_spread_dest)].value;
        osc.spread             = new_spread;
        osc.panning            = parameters[osc_param(osc_idx, osc_panning_dest)].value;

        const float    dest_value = parameters[osc_param(osc_idx, osc_volume_dest)].value;
        const uint32_t env_node   = osc_param(osc_idx, osc_volume_env);
        osc.volume                = dest_value;
        if (voice.release_total_ticks && param_descs[env_node].kind == Synth::ParamKind::envelope) {
            if (! osc.decaying) {
                const Synth::EnvelopeDescriptor& env_desc  = param_descs[env_node].envelope;
                const Synth::EnvelopeState&      env_state = parameters[env_node].envelope;
                if (voice.active && ! voice.releasing && env_state.point >= env_desc.sustain_first_point &&
                    env_state.point <= env_desc.sustain_last_point) {
                    osc.decaying     = true;
                    osc.decay_tick   = 0;
                    osc.decay_factor = 1.0f;
                }
            }
            else {
                ++osc.decay_tick;
                osc.decay_factor *= osc.decay_mult;
                osc.volume = dest_value * osc.decay_factor;
            }
        }
        else if (voice.release_total_ticks && ! osc.decaying) {
            // No volume envelope: the instrument release still decays, starting at note
            // onset (the no-envelope analog of sustain entry).  decay_mult and
            // decay_factor carry their note-on values, so nothing is recomputed here.
            if (voice.active && ! voice.releasing) {
                osc.decaying   = true;
                osc.decay_tick = 0;
            }
        }
        else if (voice.release_total_ticks) {
            // The decay advances every step once started, held or released.
            ++osc.decay_tick;
            osc.decay_factor *= osc.decay_mult;
            osc.volume = dest_value * osc.decay_factor;
        }
        osc.duty[0]        = parameters[osc_param(osc_idx, osc_duty0_dest)].value;
        osc.duty[1]        = parameters[osc_param(osc_idx, osc_duty1_dest)].value;
        osc.pad_tilt[0]    = parameters[osc_param(osc_idx, osc_pad_tilt0_dest)].value;
        osc.pad_tilt[1]    = parameters[osc_param(osc_idx, osc_pad_tilt1_dest)].value;
        osc.string_damping = unit_range_clamp(parameters[osc_param(osc_idx, osc_string_damping_dest)].value);
        osc.string_release_damping =
            unit_range_clamp(parameters[osc_param(osc_idx, osc_string_release_damping_dest)].value);
        osc.string_dispersion = unit_range_clamp(parameters[osc_param(osc_idx, osc_string_dispersion_dest)].value);
        osc.osc_mix           = parameters[osc_param(osc_idx, osc_mix_dest)].value;
        osc.fm_index          = parameters[osc_param(osc_idx, osc_fm_index_dest)].value;
        // The ratio clamps like the static row: legacy 0 ratios play at the
        // slider floor and hand-written out-of-range values stay in bounds.
        const float ratio_dest = parameters[osc_param(osc_idx, osc_fm_ratio_dest)].value;
        osc.mod_ratio          = ratio_dest < Synth::osc_mod_ratio_min   ? Synth::osc_mod_ratio_min
                                 : ratio_dest > Synth::osc_mod_ratio_max ? Synth::osc_mod_ratio_max
                                                                         : ratio_dest;
        // The slope feeds the span clamp directly: a routed modulation overdriven through zero
        // continues smoothly into fir_effective_span's 11-tap floor instead of jumping back to the
        // note-on scalar's span.
        osc.fir_slope = parameters[osc_param(osc_idx, osc_fir_slope_dest)].value;

        // Multi-band bandpass state: every band's four leaves refresh like the
        // other targets; band 0 mirrors the legacy spread/slope dests read
        // above, so its kernel math stays bit-identical to the single-band form.
        osc.band_offset[0] = parameters[osc_param(osc_idx, osc_fir_band0_offset_dest)].value;
        osc.band_gain[0]   = parameters[osc_param(osc_idx, osc_fir_band0_gain_dest)].value;
        osc.band_spread[0] = osc.spread;
        osc.band_slope[0]  = osc.fir_slope;
        for (uint32_t band = 1; band < osc.fir_band_count; band++) {
            osc.band_offset[band] = parameters[osc_param(osc_idx, band_dest_roles[band - 1][0])].value;
            osc.band_spread[band] = parameters[osc_param(osc_idx, band_dest_roles[band - 1][1])].value;
            osc.band_gain[band]   = parameters[osc_param(osc_idx, band_dest_roles[band - 1][2])].value;
            osc.band_slope[band]  = parameters[osc_param(osc_idx, band_dest_roles[band - 1][3])].value;
        }
        // The re-engagement edge rides the single-source activity predicate:
        // a false -> true transition (band 1..3's spread modulating positive
        // while band 0 stays zero included) clears the frozen history.
        const bool prev_fir_active = osc.fir_active;
        osc.fir_active             = oscillator_fir_active(osc);
        if (osc.fir_taps_offs && ! prev_fir_active && osc.fir_active) {
            osc.clear_fir_hist = true; // Re-engaged filter: the frozen history holds stale audio.
        }

        bool free_now = osc.terminating;
        if (! free_now && (voice.note_finished || (voice.releasing && volume_termination_due(osc_idx)) ||
                           (osc.decaying && osc.decay_tick >= voice.release_total_ticks))) {
            osc.terminating = true;
            osc.volume      = 0.0f; // Final block: the volume gradient ramps from the last rendered volume to silence.
        }
        if (free_now) {
            free_oscillator(osc_idx);

            // Remove this slot from the voice's live list without a search: its position is
            // osc.layer_idx, so move the last live entry into it (and update that moved
            // oscillator's stored position) to keep osc_ids[0, osc_count) the live set.
            assert(voice.osc_count);
            --voice.osc_count;
            const uint8_t moved_slot          = voice.osc_ids[voice.osc_count];
            voice.osc_ids[osc.layer_idx]      = moved_slot;
            oscillators[moved_slot].layer_idx = osc.layer_idx;

            if (! voice.osc_count) {
                if (! voice.note_finished) {
                    note_to_voice[osc.midi_channel][osc.note] = 0;
                }
                voice.active = false;

                // Clear the owned oscillator slot ids so a reused voice cannot
                // read stale ids.
                memset(voice.osc_ids, 0, sizeof(voice.osc_ids));
            }
        }
    }
}

// Stages one FIR effect's coefficient entry.  The count predicate calls the same helper, so the
// count condition and the staging compaction rule stay identical by construction (the
// invariant compute_fir_coefficients asserts).
static bool stage_effect_fir_entry(const EffectInstance& instance, Synth::FirEffectCoeffEntry* entry)
{
    // The multiband form derives every band's edges from its uniform
    // center/spread pair; the plain FIR keeps its bit-exact legacy staging.
    if (instance.type == Synth::EffectType::fir_multiband) {
        return Synth::stage_fir_multiband_coeff_entry(instance.state_offs / 4,
                                                      instance.fir_bands.band_count,
                                                      instance.fir_bands.params,
                                                      entry);
    }
    return Synth::stage_fir_effect_coeff_entry(instance.state_offs / 4,
                                               instance.fir_bands.band_count,
                                               instance.params[0],
                                               instance.params[1],
                                               instance.params[2],
                                               instance.fir_bands.params,
                                               entry);
}

// A FIR effect contributes a coefficient entry iff its staging produces one: a single-band
// effect always stages the legacy entry (byte-identical legacy staging), a multi-band effect
// stages when at least one band is active (the oscillator fir_active analog).
static uint32_t count_effect_fir(const EffectChain& chain)
{
    uint32_t count = 0;
    for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
        const EffectInstance& instance = chain.effects[effect_idx];
        if (! instance.enabled || ! is_fir_effect_type(instance.type)) {
            continue;
        }
        Synth::FirEffectCoeffEntry entry;
        if (stage_effect_fir_entry(instance, &entry)) {
            ++count;
        }
    }

    return count;
}

static void compute_fir_coefficients()
{
    uint32_t num_active_filters = 0;
    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        if (oscillators[osc_idx].osc_type[0] != WaveType::no_wave && oscillators[osc_idx].fir_taps_offs) {
            // The cached engagement flag matches the staging compaction rule
            // exactly: a bandpass counts when some band has positive spread
            // (zero gains stage a zeroed kernel), a single-edge type when its
            // spread is positive.
            if (oscillators[osc_idx].fir_active) {
                ++num_active_filters;
            }
        }
    }

    for (uint32_t chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        num_active_filters += count_effect_fir(channel_chains[chan_idx]);
    }
    num_active_filters += count_effect_fir(master_chain);

    // Read-and-clear the zeroing flag before the decision: the clear and RESET hooks above ran
    // earlier in this step.
    const bool taps_cleared = fir_taps_cleared;
    fir_taps_cleared        = false;

    if (! num_active_filters) {
        // No dispatch with zero active filters; the retained snapshot must not survive an inactive
        // step, so the next activation dispatches regardless of what the bytes say.
        fir_coeff_prev_count = 0;
        return;
    }

    // Build the staging block in a deterministic order (oscillators, channel chains, master), so
    // the staged-entry count matches num_active_filters (the cutoff-skip condition matches the
    // count condition).
    uint32_t staged = 0;
    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        const RunningOscillator& osc = oscillators[osc_idx];
        if (osc.osc_type[0] == WaveType::no_wave || ! osc.fir_taps_offs) {
            continue;
        }

        float      lowpass_hz  = 0.0f;
        float      highpass_hz = 0.0f;
        uint32_t   band_span   = 0;
        const bool single_edge = Synth::oscillator_fir_cutoffs(osc.pitch,
                                                               osc.spread,
                                                               osc.freq_mult,
                                                               osc.fir_type,
                                                               &lowpass_hz,
                                                               &highpass_hz);
        // A bandpass with an inactive band 0 still stages its higher bands
        // below, so only the single-edge types passthrough here.
        if (! single_edge && osc.fir_type != Synth::fir_bandpass) {
            continue; // Effective spread <= 0: passthrough, no coefficient dispatch.
        }

        if (osc.fir_type == Synth::fir_bandpass) {
            // Multi-band bandpass: the full entry is zero-initialized, inactive
            // bands included, so the shader's band loop never reads garbage.
            // A band whose gain clamps to zero still stages (it contributes
            // nothing to the accumulation), so an oscillator whose gains are
            // all zero produces a zeroed kernel: silence, continuous with the
            // approaching-zero sweep. An entry only exists while some band's
            // spread is positive, matching the count predicate above.
            ShaderParams::FIRCoeff param;
            memset(&param, 0, sizeof(param));
            param.taps_offs    = osc.fir_taps_offs / 4;
            uint32_t num_bands = 0;
            for (uint32_t band = 0; band < osc.fir_band_count; band++) {
                // The modulated gain clamps to [0, 1] and the
                // offset to +/-36 semitones; NaN reads as the neutral value.
                float gain = osc.band_gain[band];
                if (gain > 1.0f) {
                    gain = 1.0f;
                }
                if (! (gain > 0.0f)) {
                    gain = 0.0f; // Gain <= 0 or NaN: the band stages a zero-gain contribution.
                }
                float offset = osc.band_offset[band];
                if (! (offset >= -36.0f && offset <= 36.0f)) {
                    offset = offset > 0.0f ? 36.0f : offset < 0.0f ? -36.0f : 0.0f;
                }
                if (! Synth::oscillator_fir_band_cutoffs(osc.pitch,
                                                         offset,
                                                         osc.band_spread[band],
                                                         osc.freq_mult,
                                                         osc.band_slope[band],
                                                         &lowpass_hz,
                                                         &highpass_hz,
                                                         &band_span)) {
                    continue; // Spread <= 0 or NaN: an inactive band.
                }
                uint32_t lowpass  = Synth::cutoff_hz_to_int(lowpass_hz);
                uint32_t highpass = Synth::cutoff_hz_to_int(highpass_hz);
                Synth::oscillator_fir_band_guard(&lowpass, &highpass);
                ShaderParams::FIRCoeff::Band* out = &param.bands[num_bands++];
                out->highpass_cutoff_freq         = highpass;
                out->lowpass_cutoff_freq          = lowpass;
                out->window_span                  = band_span;
                out->gain                         = gain;
            }
            if (! num_bands) {
                continue; // Every band's spread <= 0: no entry, passthrough.
            }
            param.band_count          = num_bands;
            fir_coeff_stage[staged++] = param;
            continue;
        }

        uint32_t lowpass  = Synth::cutoff_hz_to_int(lowpass_hz);
        uint32_t highpass = Synth::cutoff_hz_to_int(highpass_hz); // 0 when the helper floor-killed the edge.

        ShaderParams::FIRCoeff& param = fir_coeff_stage[staged++];
        memset(&param, 0, sizeof(param)); // the unused band words carry no stale bytes into the cache
        param.taps_offs                     = osc.fir_taps_offs / 4;
        param.band_count                    = 1;
        param.bands[0].lowpass_cutoff_freq  = lowpass;
        param.bands[0].highpass_cutoff_freq = highpass;
        param.bands[0].window_span          = Synth::fir_effective_span(osc.fir_slope);
        param.bands[0].gain                 = 1.0f; // A single-edge band is unattenuated.
    }

    // Stages one effect chain's FIR entries into the staging array, after the oscillator entries.
    auto stage_effect_fir_coeffs = [](const EffectChain& chain, uint32_t* cur_entry) {
        for (uint32_t effect_idx = 0; effect_idx < chain.num_effects; effect_idx++) {
            const EffectInstance& instance = chain.effects[effect_idx];
            if (! instance.enabled || ! is_fir_effect_type(instance.type)) {
                continue;
            }

            // The helper drops the entry when no band is active; the memcpy is layout-identical
            // to the oscillator path's direct param writes (the static_assert pins the sizes).
            Synth::FirEffectCoeffEntry entry;
            if (! stage_effect_fir_entry(instance, &entry)) {
                continue;
            }
            memcpy(&fir_coeff_stage[(*cur_entry)++], &entry, sizeof(entry));
        }
    };
    for (uint32_t chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        stage_effect_fir_coeffs(channel_chains[chan_idx], &staged);
    }
    stage_effect_fir_coeffs(master_chain, &staged);
    assert(staged == num_active_filters);

    // Skip the dispatch when the previous computation already produced these coefficients and the
    // taps are intact; dispatch otherwise.
    if (Synth::fir_coeff_cache_action(fir_coeff_prev,
                                      fir_coeff_prev_count,
                                      fir_coeff_stage,
                                      num_active_filters,
                                      taps_cleared) == Synth::fir_coeff_skip) {
        return;
    }

    const uint32_t param_size = num_active_filters * static_cast<uint32_t>(sizeof(ShaderParams::FIRCoeff));
    const uint32_t param_offs = static_cast<uint32_t>(param_allocator.allocate(param_size, synth_alignment).offset);

    // The staging array is copied synchronously into the allocated parameter-buffer region before
    // dispatch: the GPU descriptors reference the parameter buffer, never this reusable array.
    memcpy(buffers[param_buf].get_ptr<uint8_t>(param_offs), fir_coeff_stage, param_size);

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

    // The snapshot now describes what this dispatch computes; refresh it only here, never on a
    // skipped step.
    memcpy(fir_coeff_prev, fir_coeff_stage, param_size);
    fir_coeff_prev_count = num_active_filters;
}

static void render_audio_step()
{
    if (void (*const callback)() = bank_source_callback.load(std::memory_order_acquire)) {
        callback();
    }

    const uint32_t start_samples = rendered_samples;
    const uint32_t end_samples   = start_samples + Synth::rt_step_samples;

    // TODO move this to the end or outside of this function
    rendered_samples = end_samples;

    // Zero the effect-state ranges freshly allocated since the last step (init or a bank
    // publish). A range must be cleared before its effect's first shader use, so this runs
    // before the effect dispatches (TRANSFER stage, no command buffer existed at init).
    const Synth::EffectClearList effect_clears = Synth::take_effect_clear_ranges();
    if (effect_clears.count) {
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);

        for (uint32_t range_idx = 0; range_idx < effect_clears.count; range_idx++) {
            vkCmdFillBuffer(audio_cmd_buf,
                            buffers[data_buf].get_buffer(),
                            effect_clears.ranges[range_idx].offset,
                            effect_clears.ranges[range_idx].bytes,
                            0); // data
        }
        // Conservative: any consumed clear batch may have zeroed FIR coefficient taps, so the
        // snapshot is invalid even when the staged coefficients compare equal.
        fir_taps_cleared = true;
    }

    process_events(start_samples, end_samples);

    // Auto-release notes
    for (uint32_t voice_idx = 1; voice_idx < max_voices; voice_idx++) {
        Voice& voice = voices[voice_idx];
        if (voice.active && ! voice.releasing && voice.release_sample) {
            if (voice.release_sample <= end_samples) {
                begin_voice_release(voice_idx);
            }
        }
    }

    Synth::pump_live_midi();

    update_modulation();

    pull_effect_params();

    // ======================================================================

    compute_fir_coefficients();

    // ======================================================================

    // Clear FIR history buffer for new notes
    bool any_history_cleared = false;
    for (uint32_t osc_idx = 1; osc_idx < Synth::max_oscillators; osc_idx++) {
        if (! oscillators[osc_idx].clear_fir_hist) {
            continue;
        }

        if (! any_history_cleared) {
            memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
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

    uint32_t num_oscillators                     = 0;
    uint32_t channel_osc_count[max_mix_channels] = {};

    for (const RunningOscillator& oscillator : oscillators) {
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
    if (! num_mix_channels && ! chain_has_enabled_effect(master_chain)) {
        memory_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);

        vkCmdFillBuffer(audio_cmd_buf,
                        buffers[data_buf].get_buffer(),
                        master_output_offs,
                        sizeof(float) * 2 * Synth::rt_step_samples,
                        0); // data
        return;
    }

    // ======================================================================

    const uint32_t osc_base_param_size = num_oscillators * static_cast<uint32_t>(sizeof(ShaderParams::Oscillator));
    const uint32_t osc_base_param_offs =
        static_cast<uint32_t>(param_allocator.allocate(osc_base_param_size, synth_alignment).offset);
    uint32_t cur_param_offs = osc_base_param_offs;

    for (RunningOscillator& oscillator : oscillators) {
        if (oscillator.osc_type[0] == WaveType::no_wave)
            continue;

        ShaderParams::Oscillator& param = get_param<ShaderParams::Oscillator>(cur_param_offs);

        const float note_freq = Synth::pitch_to_frequency(oscillator.pitch, oscillator.freq_mult);
        const float phase_step =
            (static_cast<float>(Synth::rt_step_samples) * note_freq) / static_cast<float>(Synth::rt_sampling_rate);

        param.out_sound_offs = oscillator.osc_output_offs / 4;
        param.phase          = oscillator.phase;
        param.phase_step     = phase_step / static_cast<float>(Synth::rt_step_samples);
        param.osc_pack       = static_cast<uint32_t>(oscillator.osc_type[0]) |
                               (static_cast<uint32_t>(oscillator.osc_type[1]) << 8) |
                               (static_cast<uint32_t>(oscillator.osc_mode) << 16) |
                               (voices[oscillator.voice_id].releasing ? (1u << 24) : 0u);
        param.duty[0]        = oscillator.duty[0];
        param.duty[1]        = oscillator.duty[1];
        param.osc_mix        = oscillator.osc_mix;
        param.mod_ratio      = oscillator.mod_ratio;
        param.fm_index       = oscillator.fm_index;

        // FM modulator runs at carrier_freq * mod_ratio with its own accumulator
        // so continuity holds at non-integer ratios.  Hard sync derives the slave
        // from the master phase directly, so mod_phase / mod_phase_step are unused
        // in that mode (computed unconditionally here, harmless when mod_ratio is
        // the sync ratio).
        const float mod_phase_step = phase_step * oscillator.mod_ratio;
        param.mod_phase            = oscillator.mod_phase;
        param.mod_phase_step       = mod_phase_step / static_cast<float>(Synth::rt_step_samples);
        param.fir_memory_offs      = oscillator.fir_memory_offs / 4;
        // The cached engagement flag gates the shader: an inactive filter
        // passes through (taps offset 0), an all-zero-gain bandpass keeps
        // reading its staged zeroed kernel (silence).
        param.taps_offs         = oscillator.fir_active ? oscillator.fir_taps_offs / 4 : 0;
        param.noise_counter     = oscillator.noise_counter;
        param.note_seed         = oscillator.note_seed;
        param.pad_tilt[0]       = oscillator.pad_tilt[0];
        param.pad_tilt[1]       = oscillator.pad_tilt[1];
        param.string_state_offs = oscillator.string_state_offs / 4;
        param.string_damping    = oscillator.string_damping;
        // The summed (dest + envelope + LFO) value is clamped to [0, 1] first, then curved once here
        // so every modulation input gets the same response.
        param.string_release_damping = Synth::string_release_damping_curve(oscillator.string_release_damping);
        param.string_brightness      = oscillator.string_brightness;
        param.string_dispersion      = oscillator.string_dispersion;
        param.string_chain           = oscillator.string_chain;

        // Wrap the phases so float precision does not degrade on long notes.
        oscillator.phase += phase_step;
        oscillator.phase -= mstd::floorf(oscillator.phase);
        oscillator.mod_phase += mod_phase_step;
        oscillator.mod_phase -= mstd::floorf(oscillator.mod_phase);
        oscillator.noise_counter += Synth::rt_step_samples;

        cur_param_offs += static_cast<uint32_t>(sizeof(ShaderParams::Oscillator));
    }

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[oscillator_pipe]);

    static const PushDescriptorInfo push_osc_data = { oscillator_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_osc_data, 0);

    static const PushDescriptorInfo push_osc_param = { oscillator_pipe,
                                                       1,
                                                       0,
                                                       param_buf,
                                                       ShaderParams::max_param_range };
    push_descriptor(push_osc_param, osc_base_param_offs);

    if (num_oscillators) {
        vkCmdDispatch(audio_cmd_buf, num_oscillators, 1, 1);
    }

    // ======================================================================

    const uint32_t input_param_size =
        num_oscillators * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
    const uint32_t chan_param_size = num_mix_channels * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine));
    const uint32_t input_param_offs =
        static_cast<uint32_t>(param_allocator.allocate(input_param_size, synth_alignment).offset);
    const uint32_t chan_param_offs =
        static_cast<uint32_t>(param_allocator.allocate(chan_param_size, synth_alignment).offset);

    uint32_t chan_input_indices[max_mix_channels]     = {}; // indexed by compacted used_chan_idx
    uint32_t chan_map[max_mix_channels]               = {};
    uint32_t gen_chan_input_indices[max_mix_channels] = {}; // indexed by raw channel, for the oscillator loop

    for (uint32_t input_idx = 0, used_chan_idx = 0, chan_idx = 0; chan_idx < max_mix_channels; chan_idx++) {
        if (! channel_osc_count[chan_idx] && ! chain_has_enabled_effect(channel_chains[chan_idx])) {
            continue;
        }

        assert(used_chan_idx < num_mix_channels);
        chan_map[used_chan_idx]           = chan_idx;
        chan_input_indices[used_chan_idx] = input_idx;
        gen_chan_input_indices[chan_idx]  = input_idx;

        input_idx += channel_osc_count[chan_idx];
        ++used_chan_idx;
    }

    for (RunningOscillator& oscillator : oscillators) {
        if (oscillator.osc_type[0] == WaveType::no_wave)
            continue;

        const uint32_t cur_param_idx = gen_chan_input_indices[oscillator.output_channel]++;
        cur_param_offs =
            input_param_offs + cur_param_idx * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
        ShaderParams::ChannelCombineInput& param = get_param<ShaderParams::ChannelCombineInput>(cur_param_offs);

        param.in_sound_offs = oscillator.osc_output_offs / 4;
        // Perceptual volume: the linear graph value maps to gain = value*value at the hand-off, so
        // bank values and the editor stay linear while the rendered gain follows a perceptual curve
        // that reaches true silence.
        param.volume      = oscillator.volume * oscillator.volume;
        param.panning     = oscillator.panning;
        param.old_volume  = oscillator.old_volume * oscillator.old_volume;
        param.old_panning = oscillator.old_panning;

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

    static const PushDescriptorInfo push_comb_param0 = { chan_combine_pipe,
                                                         1,
                                                         0,
                                                         param_buf,
                                                         ShaderParams::max_param_range };
    push_descriptor(push_comb_param0, input_param_offs);

    static const PushDescriptorInfo push_comb_param1 = { chan_combine_pipe,
                                                         2,
                                                         0,
                                                         param_buf,
                                                         ShaderParams::max_param_range };
    push_descriptor(push_comb_param1, chan_param_offs);

    vkCmdDispatch(audio_cmd_buf, num_mix_channels, 1, 1);

    // ======================================================================

    // Apply each active channel's effect chain in place, before the master mix sums
    // the channels.  Distinct per-channel buffers run packed in dependency waves.
    EffectTarget channel_effect_targets[max_mix_channels];
    for (uint32_t used_chan_idx = 0; used_chan_idx < num_mix_channels; used_chan_idx++) {
        const uint32_t chan_idx                          = chan_map[used_chan_idx];
        channel_effect_targets[used_chan_idx].chain      = &channel_chains[chan_idx];
        channel_effect_targets[used_chan_idx].sound_offs = mix_channels[chan_idx].chan_output_offs / 4;
    }
    apply_effects(channel_effect_targets, num_mix_channels);

    // ======================================================================

    // Sum all per-channel stereo buffers into the master stereo buffer, applying
    // each channel's volume + pan.  The master-mix shader interpolates volume/pan
    // across the whole block from the previous step's values (old_*), so carry the
    // current values into old_* after emitting them.

    const uint32_t master_input_param_size =
        num_mix_channels * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
    const uint32_t master_input_param_offs =
        static_cast<uint32_t>(param_allocator.allocate(master_input_param_size, synth_alignment).offset);
    const uint32_t master_param_offs = static_cast<uint32_t>(
        param_allocator.allocate(static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombine)), synth_alignment).offset);

    for (uint32_t used_chan_idx = 0; used_chan_idx < num_mix_channels; used_chan_idx++) {

        const uint32_t chan_idx = chan_map[used_chan_idx];
        Channel&       channel  = mix_channels[chan_idx];

        // The leaves are external, so this is the only path that moves them into
        // the mix state; the swap below stays the only old/new fade machinery.
        Synth::mix_leaves_to_channel(parameters[channel_param(chan_idx, chan_param_volume)].value,
                                     parameters[channel_param(chan_idx, chan_param_panning)].value,
                                     &channel.volume,
                                     &channel.panning);

        cur_param_offs =
            master_input_param_offs + used_chan_idx * static_cast<uint32_t>(sizeof(ShaderParams::ChannelCombineInput));
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
    master_param.out_sound_offs                = master_output_offs / 4;
    master_param.input_params_offs             = 0;
    master_param.num_inputs                    = num_mix_channels;

    memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[master_mix_pipe]);

    static const PushDescriptorInfo push_master_data = { master_mix_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_master_data, 0);

    static const PushDescriptorInfo push_master_param0 = { master_mix_pipe,
                                                           1,
                                                           0,
                                                           param_buf,
                                                           ShaderParams::max_param_range };
    push_descriptor(push_master_param0, master_input_param_offs);

    static const PushDescriptorInfo push_master_param1 = { master_mix_pipe,
                                                           2,
                                                           0,
                                                           param_buf,
                                                           ShaderParams::max_param_range };
    push_descriptor(push_master_param1, master_param_offs);

    vkCmdDispatch(audio_cmd_buf, 1, 1, 1);

    // ======================================================================

    // Apply the master effect chain in place on the master buffer, after the mix.
    const EffectTarget master_effect_target = { &master_chain, master_output_offs / 4 };
    apply_effects(&master_effect_target, 1);

    memory_barrier(VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT);
}

template <typename T, bool interleaved> void prepare_copy_audio_step_to_host(uint32_t offset);

template <> void prepare_copy_audio_step_to_host<int16_t, true>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[output_16i_pipe]);

    offset *= 2 * sizeof(int16_t);

    static const PushDescriptorInfo push_out_data = { output_16i_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output = { output_16i_pipe,
                                                        1,
                                                        0,
                                                        output_buf,
                                                        sizeof(int16_t) * 2 * Synth::rt_step_samples };
    push_descriptor(push_out_output, offset);

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_16i_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,            // offset
                       sizeof(push), // size
                       &push);       // pValues
}

template <> void prepare_copy_audio_step_to_host<float, true>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[output_32fi_pipe]);

    offset *= 2 * sizeof(float);

    static const PushDescriptorInfo push_out_data = { output_32fi_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output = { output_32fi_pipe,
                                                        1,
                                                        0,
                                                        output_buf,
                                                        sizeof(float) * 2 * Synth::rt_step_samples };
    push_descriptor(push_out_output, offset);

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_32fi_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,            // offset
                       sizeof(push), // size
                       &push);       // pValues
}

template <> void prepare_copy_audio_step_to_host<float, false>(uint32_t offset)
{
    vkCmdBindPipeline(audio_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[output_32f_pipe]);

    offset *= sizeof(float);

    const uint32_t other_chan_offs = static_cast<uint32_t>(buffers[output_buf].size()) / 2 + offset;

    static const PushDescriptorInfo push_out_data = { output_32f_pipe, 0, 0, data_buf, VK_WHOLE_SIZE };
    push_descriptor(push_out_data, 0);

    static const PushDescriptorInfo push_out_output0 = { output_32f_pipe,
                                                         1,
                                                         0,
                                                         output_buf,
                                                         sizeof(float) * Synth::rt_step_samples };
    push_descriptor(push_out_output0, offset);

    static const PushDescriptorInfo push_out_output1 = { output_32f_pipe,
                                                         1,
                                                         1,
                                                         output_buf,
                                                         sizeof(float) * Synth::rt_step_samples };
    push_descriptor(push_out_output1, other_chan_offs);

    const ShaderParams::OutputPushConst push = { master_output_offs / 4 };

    vkCmdPushConstants(audio_cmd_buf,
                       pipe_layouts[output_32f_pipe],
                       VK_SHADER_STAGE_COMPUTE_BIT,
                       0,            // offset
                       sizeof(push), // size
                       &push);       // pValues
}

template <typename T, bool interleaved> static bool render_audio(uint32_t num_samples)
{
    assert(num_samples % Synth::rt_step_samples == 0 && num_samples > 0);

    if (! reset_and_begin_command_buffer(audio_cmd_buf))
        return false;

    param_allocator.init(static_cast<uint32_t>(buffers[param_buf].size()));

    for (uint32_t offset = 0; offset < num_samples; offset += Synth::rt_step_samples) {
        render_audio_step();

        memory_barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        prepare_copy_audio_step_to_host<T, interleaved>(offset);

        vkCmdDispatch(audio_cmd_buf, 1, 1, 1);
    }

    memory_barrier(VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);

    buffers[param_buf].flush();

    if (! send_to_device_and_wait(audio_cmd_buf, vk_compute_queue, fen_compute))
        return false;

    if (! buffers[output_buf].invalidate())
        return false;

    return true;
}

constexpr uint32_t audio_ring_frames      = Synth::rt_sampling_rate;
constexpr uint32_t audio_lead_frames      = Synth::rt_step_samples * 6;
constexpr uint32_t audio_max_batch_frames = Synth::rt_step_samples * 16;

// Ring buffer holding rendered sound in the platform's output format.  Sized for either
// channel layout: two channels of audio_ring_frames frames.
template <typename T, bool interleaved> struct AudioRingStorage {
    static T data[audio_ring_frames * 2];
};

template <typename T, bool interleaved> T AudioRingStorage<T, interleaved>::data[audio_ring_frames * 2];

// Frame counters and the dry-ring count are format independent, so they are shared and the
// GUI can read them without knowing the format.  Exactly one <T, interleaved> instantiation
// may run per build (each has its own ring storage); using two would desync these counters.
static std::atomic<uint64_t> audio_ring_write;     // frames produced; only the producer stores
static std::atomic<uint64_t> audio_ring_read;      // frames consumed; only the callback stores
static std::atomic<uint32_t> audio_underrun_count; // ring ran dry; read by the GUI indicator

template <typename T, bool interleaved> static constexpr ActiveRingFormat ring_format_for()
{
    if constexpr (std::is_same_v<T, int16_t>) {
        static_assert(interleaved, "int16 ring is interleaved only");
        return ActiveRingFormat::int16_interleaved;
    }
    else if constexpr (interleaved)
        return ActiveRingFormat::float_interleaved;
    else
        return ActiveRingFormat::float_planar;
}

// A StereoPtr addressing the ring storage at a frame offset.
template <typename T, bool interleaved> static StereoPtr<T, interleaved> ring_stereo(uint32_t frame_offset)
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
template <typename T, bool interleaved> static StereoPtr<T, interleaved> output_stereo(T* channel0, T* channel1)
{
    if constexpr (interleaved) {
        return { channel0 };
    }
    else {
        return { channel0, channel1 };
    }
}

template <typename T, bool interleaved> static void zero_output(StereoPtr<T, interleaved> dest, uint32_t num_frames)
{
    if constexpr (interleaved) {
        memset(dest.data, 0, num_frames * 2 * sizeof(T));
    }
    else {
        memset(dest.left, 0, num_frames * sizeof(T));
        memset(dest.right, 0, num_frames * sizeof(T));
    }
}

Synth::AudioRingStatus Synth::get_audio_ring_status()
{
    // Read before write so the monotonic counters cannot make the difference underflow.
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_relaxed);
    const uint64_t write_pos = audio_ring_write.load(std::memory_order_relaxed);

    return { get_ringbuf_data_size(write_pos, read_pos),
             audio_lead_frames,
             audio_lead_frames + Synth::rt_step_samples - 1,
             audio_underrun_count.load(std::memory_order_relaxed) };
}

template <typename T, bool interleaved> bool Synth::produce_audio_batch()
{
    // Register the live ring format for the editor's peek accessor.  One
    // relaxed store per batch; the format is a build-time constant.
    active_ring_format.store(ring_format_for<T, interleaved>(), std::memory_order_relaxed);

    const uint64_t write_pos = audio_ring_write.load(std::memory_order_relaxed);
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_acquire);
    const uint32_t available = get_ringbuf_data_size(write_pos, read_pos);

    // The callback drains the lead and the producer tops it back up.
    if (available >= audio_lead_frames) {
        return false;
    }

    // Refill back toward the lead in one submit (whole steps), capped per batch.
    uint32_t to_render = mstd::align_up(audio_lead_frames - available, Synth::rt_step_samples);
    if (to_render > audio_max_batch_frames) {
        to_render = audio_max_batch_frames;
    }

    if (! render_audio<T, interleaved>(to_render)) {
        return false;
    }

    const StereoPtr<T, interleaved> rendered_src = StereoPtr<T, interleaved>::from_buffer(buffers[output_buf]);

    uint32_t copied = 0;
    while (copied < to_render) {
        const uint32_t offset = static_cast<uint32_t>((write_pos + copied) % audio_ring_frames);
        const uint32_t chunk =
            std::min(to_render - copied, get_ringbuf_contig_tail(write_pos + copied, audio_ring_frames));
        copy_audio_data(ring_stereo<T, interleaved>(offset), rendered_src + copied, chunk);
        copied += chunk;
    }

    audio_ring_write.store(write_pos + to_render, std::memory_order_release);
    return true;
}

template <typename T, bool interleaved> uint32_t Synth::consume_audio(uint32_t num_frames, T* channel0, T* channel1)
{
    const uint64_t read_pos  = audio_ring_read.load(std::memory_order_relaxed);
    const uint64_t write_pos = audio_ring_write.load(std::memory_order_acquire);
    const uint32_t available = get_ringbuf_data_size(write_pos, read_pos);
    const uint32_t to_copy   = (available < num_frames) ? available : num_frames;

    const StereoPtr<T, interleaved> dest = output_stereo<T, interleaved>(channel0, channel1);

    uint32_t copied = 0;
    while (copied < to_copy) {
        const uint32_t offset = static_cast<uint32_t>((read_pos + copied) % audio_ring_frames);
        const uint32_t chunk =
            std::min(to_copy - copied, get_ringbuf_contig_tail(read_pos + copied, audio_ring_frames));
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

// Copies one contiguous chunk of ring frames into the caller's float channels,
// converting from the ring's sample format on the way.
template <typename T, bool interleaved>
static void peek_chunk(StereoPtr<T, interleaved> src, float* left, float* right, uint32_t num_frames)
{
    // One interleaved traversal; the conversion is a compile-time scale
    // (identity for float, 1 / 32768 for the full-scale int16 ring).
    if constexpr (interleaved) {
        constexpr float scale = std::is_same_v<T, int16_t> ? 1.0f / 32768.0f : 1.0f;
        for (uint32_t i = 0; i < num_frames; i++) {
            left[i]  = static_cast<float>(src.data[i * 2]) * scale;
            right[i] = static_cast<float>(src.data[i * 2 + 1]) * scale;
        }
    }
    else {
        // Planar float ring: one bulk copy per channel.
        copy_audio_data(StereoPtr<float, false>{ left, right }, src, num_frames);
    }
}

// Copies the consumed history behind the read position, oldest-first.  The
// count comes from the history, not from the unread fill: in steady state the
// fill can drop to zero between refill batches, while the history stays valid.
template <typename T, bool interleaved> static uint32_t peek_copy(uint32_t num_frames, float* left, float* right)
{
    const uint64_t read_pos = audio_ring_read.load(std::memory_order_acquire);

    const uint32_t count = std::min(num_frames, static_cast<uint32_t>(std::min<uint64_t>(read_pos, audio_ring_frames)));

    uint32_t copied = 0;
    while (copied < count) {
        const uint32_t offset = static_cast<uint32_t>((read_pos - count + copied) % audio_ring_frames);
        const uint32_t chunk  = std::min(count - copied, Synth::get_ringbuf_contig_tail(offset, audio_ring_frames));
        peek_chunk<T, interleaved>(ring_stereo<T, interleaved>(offset), left + copied, right + copied, chunk);
        copied += chunk;
    }

    return copied;
}

uint32_t Synth::peek_recent_audio(uint32_t num_frames, float* left, float* right)
{
    switch (active_ring_format.load(std::memory_order_relaxed)) {
        case ActiveRingFormat::int16_interleaved:
            return peek_copy<int16_t, true>(num_frames, left, right);
        case ActiveRingFormat::float_interleaved:
            return peek_copy<float, true>(num_frames, left, right);
        case ActiveRingFormat::float_planar:
            return peek_copy<float, false>(num_frames, left, right);
        case ActiveRingFormat::none:
            break;
    }
    return 0;
}

uint32_t Synth::get_oscillator_wave_types(WaveType* dest, uint32_t max_entries)
{
    const uint32_t count = std::min<uint32_t>(max_entries, Synth::max_oscillators - 1);

    // Slot 0 is reserved; the live slots follow it.
    for (uint32_t i = 0; i < count; i++) {
        dest[i] = oscillators[i + 1].osc_type[0];
    }

    return count;
}

template bool Synth::produce_audio_batch<int16_t, true>();
template bool Synth::produce_audio_batch<float, true>();
template bool Synth::produce_audio_batch<float, false>();

template uint32_t Synth::consume_audio<int16_t, true>(uint32_t, int16_t*, int16_t*);
template uint32_t Synth::consume_audio<float, true>(uint32_t, float*, float*);
template uint32_t Synth::consume_audio<float, false>(uint32_t, float*, float*);
