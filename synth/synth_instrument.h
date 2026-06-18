// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_config.h"
#include "synth_parameters.h"
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
    float     scale;
    SourceOp  op;
};

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

// Keyboard-split entry
struct NoteRoute {
    uint8_t start_note;
    uint8_t instrument;
};

struct InstrumentBank {
    Pool<Instrument,         max_instruments> instruments;
    Pool<EnvelopeDescriptor, max_envelopes>   envelopes;
    Pool<LFODescriptor,      max_lfos>        lfos;
    Pool<ParamDescriptor,    max_parameters>  parameters;

    NoteRoute channel_routes[max_channels][max_instr_per_channel]; // per-channel keyboard splits
    char      instrument_names[max_instruments][max_name_len];
    char      channel_names[max_channels][max_name_len];
    uint8_t   drum_track_channel;          // canonical drum channel (default 9 == channel 10)
};

static_assert(std::is_trivially_copyable_v<InstrumentBank>,
              "InstrumentBank must be trivially copyable for byte-snapshot undo and save/load");

uint8_t select_instrument(const NoteRoute* routes, uint32_t num_routes, uint8_t note);

void remap_envelope_refs  (InstrumentBank* bank, const uint32_t* old_to_new); // envelope_desc_id in every layer
void remap_lfo_refs       (InstrumentBank* bank, const uint32_t* old_to_new); // lfo_desc_id in every layer
void remap_instrument_refs(InstrumentBank* bank, const uint32_t* old_to_new); // instrument index in every channel split entry

} // namespace Synth
