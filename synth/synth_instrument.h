// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"
#include "synth_parameters.h"
#include "synth_effects.h"
#include "../core/pool.h"

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
    mod_lowpass_cutoff,
    mod_highpass_cutoff,
    num_mod_targets
};

// Modulation input source roles
enum class ModSource : uint8_t {
    none,
    pitch_bend,        // per-channel
    mod_wheel,         // per-channel
    channel_pressure,  // per-channel
    velocity,          // per-note (note-on constant)
    aftertouch,        // per-note (polyphonic aftertouch)
    pressure_combine   // per-voice: max(per-note aftertouch, per-channel pressure)
};

// One input feeding a modulated quantity (volume, pitch, cutoff, ...)
struct ModInput {
    ModSource source;
    SourceOp  op;
    float     scale;
};

static_assert(sizeof(ModInput) == 8);

// One layer's generators for a single modulation target: an optional envelope and an optional LFO
// (with sourceable depth and rate).  Each layer instantiates its own generators, so layers that name
// the same descriptor id evaluate identically and in phase (the generators are deterministic in the
// tick).
struct LayerGen {
    uint16_t  envelope_desc_id;
    uint16_t  lfo_desc_id;
    SourceOp  lfo_op;             // How the LFO is combined into the target
    float     lfo_depth;
    ModSource lfo_depth_source;
    ModSource lfo_rate_source;
    float     lfo_rate_scale_ms;
};

// Voice-wide MIDI-input routing for one modulation target, shared by all the instrument's layers:
// a base value plus the input sources combined onto it (each added or multiplied per its op).  The
// per-layer envelope and LFO that combine on top live in each layer's LayerGen.  No runtime state.
struct InputRouting {
    float     base_value;
    uint16_t  num_inputs;
    ModInput  inputs[max_mod_inputs];
};

enum OscMode : uint32_t {
    osc_mode_blend     = 0, // Mix osc_type[0] and osc_type[1] by osc_mix
    osc_mode_fm        = 1, // osc_type[0] is carrier, osc_type[1] is modulator
    osc_mode_hard_sync = 2  // osc_type[0] sets master frequency, osc_type[1] is the hard-synced slave
};

struct Oscillator {
    WaveType osc_type[2];
    OscMode  osc_mode;
    float    mod_ratio;            // FM ratio or hard-sync cycle ratio
    float    pitch_offset;         // This oscillator's pitch offset in semitones
    LayerGen gen[num_mod_targets]; // One generator per modulated quantity (volume/panning/etc.)
};

struct Instrument {
    uint32_t     layer_count;
    Oscillator   layers[max_layers];
    InputRouting routing[num_mod_targets];  // Per-target MIDI-input routing
    float        note_skew_semitones;       // Random pitch skew applied once per note (0 = none)
    float        layer_skew_semitones;      // Random pitch skew drawn per layer (0 = none)
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
    uint16_t  lfo_desc_id;      // 0 = none
    SourceOp  lfo_op;           // how the LFO combines into the param
    float     lfo_depth;
    ModSource lfo_depth_source; // none = constant depth
    ModSource lfo_rate_source;  // none = the LFO's own period
    float     lfo_rate_scale;
    uint16_t  num_inputs;
    ModInput  inputs[max_mod_inputs]; // channel MIDI sources (master chain: none)
};

// One effect slot in a chain; only bindings[0..get_effect_param_floats(type)) are
// meaningful for the slot's type.
struct EffectSlotBinding {
    EffectType         type;
    bool               enabled;
    EffectParamBinding bindings[max_effect_param_floats];
};

struct EffectChainBinding {
    uint8_t           num_effects; // 0..max_chain_effects
    EffectSlotBinding effects[max_chain_effects];
};

struct InstrumentBank {
    Pool<Instrument,         max_instruments> instruments;
    Pool<EnvelopeDescriptor, max_envelopes>   envelopes;
    Pool<LFODescriptor,      max_lfos>        lfos;
    Pool<ParamDescriptor,    max_parameters>  parameters;

    Zone               channel_zones[max_channels][max_instr_per_channel]; // per-channel keyboard zones
    char               instrument_names[max_instruments][max_name_len];
    char               channel_names[max_channels][max_name_len];
    EffectChainBinding channel_chains[max_channels]; // per-channel effect chains
    EffectChainBinding master_chain;                 // sums all channel outputs
    uint8_t            channel_enabled[max_channels]; // 0/1 per channel; the runtime drops note-ons on disabled channels
    uint8_t            drum_track_channel;
};

static_assert(std::is_trivially_copyable_v<InstrumentBank>,
              "InstrumentBank must be trivially copyable for byte-snapshot undo and save/load");

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

// The default channel recipe: one sine instrument (wheel-driven vibrato, ADSR volume with
// pressure tremolo) plus the demo channel chain.  init_default_channel appends the recipe
// into the bank's free pool slots, wires the channel's zone table and chain, and returns
// false with the bank unmodified when any pool lacks space.
bool init_default_channel(InstrumentBank* bank, uint32_t channel); // Keyboard zone table operations.  A table is sorted by start_note and terminated
// by an empty slot; entry i covers [start_note[i]-1, start_note[i+1]-2] and the last
// entry covers through note 127.
// Index of the entry covering note, or pool_no_slot if none does.
uint32_t zone_entry_at(const Zone* zones, uint32_t note);
// Entry i-1 takes note: entry i starts at note+1 and is dropped if that leaves it
// empty.  false when i == 0 or note is not inside entry i.
bool zone_join_previous(Zone* zones, uint32_t i, uint32_t note);
// Entry i+1 takes note: it starts at note, and entry i is dropped if that leaves it
// empty.  false when entry i+1 does not exist or note is not inside entry i.
bool zone_join_next(Zone* zones, uint32_t i, uint32_t note);
// Splits entry i at note: a new zone starting at note gets a CLONE of entry i's
// instrument (same bytes, name copied, so the two rename independently); entry i keeps
// the notes below and is dropped if note was its first note.  false, bank unmodified,
// when the zone table or the instrument pool lacks a free slot.
bool zone_split_new(Zone* zones, uint32_t i, uint32_t note, InstrumentBank* bank);

// Builds the first-run bank: all channels disabled with default names ("Channel 01".."Channel
// 16", channel 10 = "Drum Track"), channel 0 enabled with the default recipe, and the demo
// master chain.
void init_default_bank(InstrumentBank* bank);

} // namespace Synth
