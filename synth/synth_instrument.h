// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../core/pool.h"
#include "synth_config.h"
#include "synth_effects.h"
#include "synth_parameters.h"

#include <stdint.h>

namespace Synth {

// Per-oscillator modulated quantities; an instrument binds each independently.
enum ModTarget : uint8_t {
    mod_volume,
    mod_pitch,
    mod_panning,
    mod_duty0,
    mod_duty1,
    mod_osc_mix,
    mod_fm_index,
    mod_fir_spread,
    mod_fm_ratio,
    mod_pad_tilt0,
    mod_pad_tilt1,
    mod_fir_slope,
    mod_fir_band0_offset,
    mod_fir_band0_gain,
    mod_fir_band1_offset,
    mod_fir_band1_spread,
    mod_fir_band1_gain,
    mod_fir_band1_slope,
    mod_fir_band2_offset,
    mod_fir_band2_spread,
    mod_fir_band2_gain,
    mod_fir_band2_slope,
    mod_fir_band3_offset,
    mod_fir_band3_spread,
    mod_fir_band3_gain,
    mod_fir_band3_slope,
    mod_string_damping,
    mod_string_release_damping,
    mod_string_dispersion,
    num_mod_targets
};

// Clamp bounds for the mod_fm_ratio target and the mod_ratio field (slider range).
constexpr float osc_mod_ratio_min = 0.125f;
constexpr float osc_mod_ratio_max = 8.0f;

// Modulation input source roles
enum class ModSource : uint8_t {
    none,
    pitch_bend,       // per-channel
    mod_wheel,        // per-channel
    channel_pressure, // per-channel
    velocity,         // per-note (note-on constant)
    aftertouch,       // per-note (polyphonic aftertouch)
    pressure_combine, // per-voice: max(per-note aftertouch, per-channel pressure)
    cc                // per-channel assignable CC (ModInput::cc_number selects 16..31)
};

// One input feeding a modulated quantity (volume, pitch, cutoff, ...)
struct ModInput {
    ModSource source;
    SourceOp  op;
    uint8_t   cc_number; // source == cc: MSB controller number (16..31 assignable); otherwise 0
    float     scale;
};

static_assert(sizeof(ModInput) == 8);

// One layer's complete binding for a single modulation target: a base value plus the MIDI input
// sources combined onto it (each added or multiplied per its op), then this layer's own envelope and
// LFO generators (with sourceable depth and rate) combining on top.  Each layer instantiates its own
// generators, so layers that name the same descriptor id evaluate identically and in phase (the
// generators are deterministic in the tick).  No runtime state.
struct TargetBinding {
    float    base_value;
    float    lfo_depth;
    float    lfo_rate_scale_ms;
    uint16_t num_inputs;
    ModInput inputs[max_mod_inputs];
    uint16_t envelope_desc_id;
    // Per-binding multiplier on the envelope generator's contribution into
    // this target.  The shared descriptor stays untouched, so several
    // targets can reuse one envelope shape at different amounts.  The
    // decoded default is 1 (the legacy unscaled contribution); 0 silences
    // the contribution while the wire stays bound.
    float     envelope_depth;
    uint16_t  lfo_desc_id;
    SourceOp  lfo_op; // How the LFO is combined into the target
    ModSource lfo_depth_source;
    ModSource lfo_rate_source;
};

// Oscillator FIR filter type.  The numeric values are stored in bank memory, so the order is frozen once
// banks ship.  "off" is zero so zeroed memory never dispatches a filter.  Band-pass uses the fir spread
// target as a spread in semitones around the note pitch.  Low-pass and high-pass use it as a fixed cutoff
// in Hz.  New layers default to off through the member initializer, so a fresh
// oscillator starts with the filter disabled.
enum FilterType : uint8_t {
    fir_off      = 0,
    fir_bandpass = 1,
    fir_lowpass  = 2,
    fir_highpass = 3,
    num_filter_types
};

enum OscMode : uint32_t {
    osc_mode_blend     = 0, // Mix osc_type[0] and osc_type[1] by osc_mix
    osc_mode_fm        = 1, // osc_type[0] is carrier, osc_type[1] is modulator
    osc_mode_hard_sync = 2, // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave
    osc_mode_ring_mod = 3 // osc 1 is the note-tied master; osc 2 is the ring-mod carrier evaluated at mod_ratio * osc 1
                          // phase; osc_mix crossfades dry osc 1 to the product
};

struct Oscillator {
    WaveType osc_type[2];
    OscMode  osc_mode;
    float    mod_ratio;   // FM ratio, hard-sync cycle ratio or ring-mod carrier ratio; note-on seeds the
                          // running oscillator from this scalar (layer.mod_ratio), and target
                          // configuration independently reads the mod_fm_ratio binding's base_value
    int8_t octave_offset; // Whole-octave pitch shift, counted in whole octaves: 12 * octave_offset semitones are added
                          // to the pitch
    // FIR window slope percent 1..100; 100 = the full 1025-tap span (sharpest).
    // The member default seeds every value-initialized construction path.
    uint8_t       fir_slope_percent = 100;
    FilterType    fir_type          = fir_off;
    uint8_t       fir_band_count    = 1;    // Bandpass bands 1..4; band 0 reuses mod_fir_spread/slope.
    TargetBinding targets[num_mod_targets]; // One binding per modulated quantity (volume/panning/etc.)
    // String wave pluck brightness 0..1: 1 leaves the excitation burst unfiltered (the default), lower values
    // low-pass it for a warmer pluck.  A plain scalar, not a mod target.
    float string_brightness = 1.0f;
    // Dispersion section count, an integer value 1..6.  Stored, validated and serialized for a future re-enable,
    // but inert: the synth always runs a single section.
    float string_dispersion_sections = 4.0f;
};

struct Instrument {
    uint32_t   layer_count;
    Oscillator layers[max_layers];
    float      note_skew_semitones;  // Random pitch skew applied once per note (0 = none)
    float      layer_skew_semitones; // Random pitch skew drawn per layer (0 = none)
    float      release_seconds;      // Sustain decay length in seconds (0 = legacy hold/loop);
                                     // applies to the volume target only, runtime-interpreted
    uint8_t    voice_mode;           // Voice allocation: 0 = multiple, 1 = reuse per-note, 2 = single;
                                     // zero is the legacy behavior
};

// Keyboard zone table entry. start_note stores the zone's first note + 1 (1..128), so a
// zone starting at note 0 stores 1; 0 marks an unused trailing slot.  Zone 0 always
// starts at note 0, so slot 0 of a non-empty table stores 1.
struct Zone {
    uint8_t start_note;
    uint8_t instrument;
};

// One effect parameter's binding. Mirrors the runtime EffectParamMod field-for-field;
// sources are channel-wide ModSource roles that the runtime resolves to modulation
// nodes at expansion time (effects are not note-triggered, so there are no envelopes).
struct EffectParamBinding {
    float     base_value;
    uint16_t  lfo_desc_id; // 0 = none
    SourceOp  lfo_op;      // how the LFO combines into the param
    float     lfo_depth;
    ModSource lfo_depth_source; // none = constant depth
    ModSource lfo_rate_source;  // none = the LFO's own period
    float     lfo_rate_scale;
    uint16_t  num_inputs;
    ModInput  inputs[max_mod_inputs]; // channel MIDI sources (master chain resolves from channel index 0)
};

// A FIR effect's per-band bindings.  The array's index meaning follows the slot's
// effect type.  The plain FIR effect keeps its dormant legacy layout: band 0's
// lowpass, highpass and slope stay in the slot's legacy bindings[0..2], index 0
// carries band 0's gain, and bands 1..3 store lowpass, highpass, gain and slope each
// (1 + 4 * (band - 1) + kind).  EffectType::fir_multiband reads the same array
// uniformly: band b lives at 4 * b + (0 = center, 1 = spread, 2 = gain, 3 = slope)
// for all four bands, and the coefficient staging derives each band's edges from
// its center/spread pair.  Every band parameter reuses the effect modulation
// machinery (LFO + channel MIDI, no envelopes).
struct FirEffectBands {
    uint8_t band_count; // Active bands 1..max_fir_effect_bands; 0 = the single band (the plain FIR's legacy marker,
                        // the multiband's normal shape)
    EffectParamBinding bands[num_fir_effect_band_params];
};

// One effect slot in a chain; only bindings[0..get_effect_param_floats(type)) are
// meaningful for the slot's type.
struct EffectSlotBinding {
    EffectType type;
    bool       enabled;
    // Wired enable source for the slot (the Enabled row's wire); zero = the
    // saved checkbox alone decides.  A wired routing overrides the saved bit
    // at runtime: the resolved source's leaf value reaches the threshold
    // (raw 14-bit >= 8192).
    ModInput           enable_input;
    EffectParamBinding bindings[max_effect_param_floats];
    FirEffectBands     fir_bands; // FIR effect only; the plain fir keeps band 0 in bindings[0..2]
};

struct EffectChainBinding {
    uint8_t           num_effects; // 0..max_chain_effects
    EffectSlotBinding effects[max_chain_effects];
};

struct InstrumentBank {
    Pool<Instrument, max_instruments>       instruments;
    Pool<EnvelopeDescriptor, max_envelopes> envelopes;
    Pool<LFODescriptor, max_lfos>           lfos;
    Pool<ParamDescriptor, max_parameters>   parameters;

    Zone               channel_zones[max_channels][max_instr_per_channel]; // per-channel keyboard zones
    EffectChainBinding channel_chains[max_channels];                       // per-channel effect chains
    EffectChainBinding master_chain;                                       // sums all channel outputs
    uint8_t channel_enabled[max_channels]; // 0/1 per channel; the runtime drops note-ons on disabled channels
};

static_assert(std::is_trivially_copyable_v<InstrumentBank>,
              "InstrumentBank must be trivially copyable for byte-snapshot undo and save/load");

// True when the binding carries anything at all: a nonzero base, at least one routed MIDI input,
// or a bound envelope/LFO descriptor.  Depth/rate-scale/op fields without a bound id or input do
// not make a binding present.
bool osc_fir_slope_binding_present(const TargetBinding& binding);

// The slope value a layer starts from: the routed base when the binding is present, the
// pre-existing scalar otherwise.  One spelling shared by the note-on seed, the editor display and
// the tests.
float osc_fir_slope_seed_value(const TargetBinding& binding, uint8_t fir_slope_percent);

uint8_t route_instrument(const Zone* zones, uint32_t num_zones, uint8_t note);

// Copies `num` descriptors into free slots of the bank's pool, mapping 1-based source ids
// (1..num, src[0] = id 1) to their new 1-based bank ids in out_ids.  Returns false with the
// bank unmodified when the pool lacks space.
bool remap_envelopes(InstrumentBank* bank, const EnvelopeDescriptor* src, uint32_t num, uint16_t* out_ids);
bool remap_lfos(InstrumentBank* bank, const LFODescriptor* src, uint32_t num, uint16_t* out_ids);

// Copies an instrument or effect chain, rewriting descriptor references through the remap
// tables (table[i] = new 1-based id of source id i + 1; 0 = source id unused).
void remap_instrument(const Instrument& src, const uint16_t* env_ids, const uint16_t* lfo_ids, Instrument* dst);
void remap_effect_chain(const EffectChainBinding& src, const uint16_t* lfo_ids, EffectChainBinding* dst);

} // namespace Synth
