// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_osc_graph.h"
#include "../core/d_printf.h"
#include "sculptor_bank_json.h"
#include "sculptor_effect_graph.h"
#include "sculptor_instr_envelope_edit.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <string.h>

using Sculptor::bounded_real_slot;
using Sculptor::input_slot;
using Sculptor::int_slot;
using Sculptor::LabelTooltip;
using Sculptor::lfo_start_names;
using Sculptor::lfo_sync_names;
using Sculptor::lfo_wave_names;
using Sculptor::list_slot;
using Sculptor::make_slot;
using Sculptor::mod_source_descs;
using Sculptor::output_slot;
using Sculptor::real_slot;
using Sculptor::source_op_names;
using Sculptor::with_tooltip;

using Sculptor::connection_into;
using Sculptor::Graph;
using Sculptor::osc_band_block_level;
using Sculptor::osc_band_block_stride;
using Sculptor::osc_band_first_slot;
using Sculptor::osc_band_header_slot;
using Sculptor::osc_brightness_slot;
using Sculptor::osc_dispersion_sections_slot;
using Sculptor::osc_dispersion_slot;
using Sculptor::osc_pick_slot;
using Sculptor::osc_slot_count;
using Sculptor::osc_target_block_base;
using Sculptor::osc_target_block_field;
using Sculptor::osc_target_block_stride;
using Sculptor::osc_target_env_depth;
using Sculptor::osc_target_env_dot;
using Sculptor::osc_target_lfo_dot;
using Sculptor::osc_target_row_slot;
using Sculptor::osc_target_src_dot;

enum NodeRole {
    role_none,
    role_input,
    role_osc,
    role_env,
    role_lfo,
    role_output
};

enum OutputKind {
    out_none,
    out_input,
    out_osc,
    out_env,
    out_lfo
};

enum InputKind {
    in_none,
    in_depth,
    in_rate,
    in_sum
};

// Generator instance identity: (kind, descriptor, LFO depth/rate sources).
// The sources take part because they ride the LFO node's depth/rate input
// edges - two parameters sharing one LFO descriptor but differing in depth
// source each need their own instance.
struct InstanceKey {
    uint8_t kind;
    uint8_t desc_id;
    uint8_t depth_source;
    uint8_t rate_source;
};

using Sculptor::find_record;

struct PortableNode {
    Synth::InstrumentGraphLayout locator;
    uint8_t                      index;
    uint8_t                      served[Synth::num_mod_targets];
};

static_assert(sizeof(PortableNode) == (sizeof(Synth::InstrumentGraphLayout) + 1 + Synth::num_mod_targets + 3) / 4 * 4);

namespace {
PortableNode                        clipboard_source_nodes[Synth::instrument_graph_layout_capacity];
PortableNode                        clipboard_decoded_nodes[Synth::instrument_graph_layout_capacity];
Synth::InstrumentGraphLayout        clipboard_portable_records[Synth::instrument_graph_layout_capacity];
Synth::GraphNodeLayout              clipboard_mapped_records[Synth::instrument_graph_layout_capacity];
const Synth::InstrumentGraphLayout* clipboard_saved_layout[Synth::instrument_graph_layout_capacity];

Synth::InstrumentEditorBank clipboard_candidate;
Synth::InstrumentBank       clipboard_model_bank;
Synth::GraphNodeLayout      clipboard_candidate_records[Synth::instrument_graph_layout_capacity];
uint16_t                    clipboard_envelope_ids[Synth::instrument_max_envelopes];
uint16_t                    clipboard_lfo_ids[Synth::instrument_max_lfos];
char                        clipboard_destination_name[Synth::max_name_len];

// The projection arithmetic relies on this exact ModTarget ordering.
static_assert(static_cast<uint32_t>(Synth::mod_panning) == 2);
static_assert(static_cast<uint32_t>(Synth::mod_duty0) == 3);
static_assert(static_cast<uint32_t>(Synth::mod_fm_index) == 6);
static_assert(Synth::num_mod_targets == 28);
static_assert(static_cast<uint32_t>(Synth::mod_fm_ratio) == 8);
static_assert(static_cast<uint32_t>(Synth::mod_pad_tilt0) == 9);
static_assert(Synth::max_mod_inputs == 2);

// One target row's label and hover text, indexed by ModTarget.  The labels
// are display-only: serialization keys live in sculptor_bank_json.cpp
// (mod_target_names), so label text is free to change.
const LabelTooltip target_descs[Synth::num_mod_targets] = {
    { "Volume", "Amplitude of the output oscillator wave" },
    { "Pitch", "Tuning of the oscillator waves (in semitones, -24..24)" },
    { "Pan", "Stereo position of the output (0 = left, 0.5 = center, 1 = right)" },
    { "Duty A", "Shape adjustment of oscillator wave (sawtooth ramp point, pulse width, pad wander)" },
    { "Duty B", "Shape adjustment of oscillator wave (sawtooth ramp point, pulse width, pad wander)" },
    { "Waveform Mix", "Balance between primary and secondary oscillator wave (0 = waveform A, 1 = Waveform B)" },
    { "FM Depth", "Amount of frequency modulation applied by secondary wave to primary wave" },
    { "Band spread", "Band width around its center (in semitones)" },
    { "FM Ratio", "Secondary oscillator wave frequency ratio to the note pitch" },
    { "Pad Tone A", "Pad wave harmonic tilt (-1 flat, +1 mellow)" },
    { "Pad Tone B", "Pad wave harmonic tilt (-1 flat, +1 mellow)" },
    { "FIR Slope", "Filter edge sharpness" },
    { "Offset", "Band center offset from the note pitch, in semitones" },
    { "Gain", "Band's output amplitude" },
    { "Offset", "Band center offset from the note pitch, in semitones" },
    { "Spread", "Band width around its center (in semitones)" },
    { "Gain", "Band's output amplitude" },
    { "FIR Slope", "Filter edge sharpness" },
    { "Offset", "Band center offset from the note pitch, in semitones" },
    { "Spread", "Band width around its center (in semitones)" },
    { "Gain", "Band's output amplitude" },
    { "FIR Slope", "Filter edge sharpness" },
    { "Offset", "Band center offset from the note pitch, in semitones" },
    { "Spread", "Band width around its center (in semitones)" },
    { "Gain", "Band's output amplitude" },
    { "FIR Slope", "Filter edge sharpness" },
    { "Damping", "How fast the string vibration decays while the key is held" },
    { "Release Damping", "Extra string damping applied while the key is released" },
};
const char* const wave_names[7]     = { "off", "sine", "sawtooth", "pulse", "noise", "pad", "string" };
const char* const osc_mode_names[4] = { "blend", "frequency modulation", "hard sync", "ring modulation" };
const char* const filter_type_names[Synth::num_filter_types] = { "off", "bandpass", "lowpass", "highpass" };

// Dropdown order puts "off" first.  The stored enum order is fixed: zero must

// The FIR spread row's label and hover text, indexed by the stored
// FilterType value.  The off row is a dead variant: under that filter
// type the row shows no tooltip.
const LabelTooltip fir_spread_descs[Synth::num_filter_types] = { { "Filter Off", nullptr },
                                                                 { "Spread",
                                                                   "Band width around its center (in semitones)" },
                                                                 { "Cutoff (Hz)", "Cutoff frequency (in Hz)" },
                                                                 { "Cutoff (Hz)", "Cutoff frequency (in Hz)" } };

// Oscillator node slot layout. The order is fixed: project fills slots in
// exactly this order and compile reads properties back by index, so the two
// stay consistent by construction.
// 0 output
// 1 waveform a (list)
// 2 duty a (per-layer value, [0, 1])
// 3..14 duty a's 12-slot modulation block
// 15 pad tone a (per-layer value, [-1, +1])
// 16..27 pad tone a's 12-slot modulation block
// 28 pick position (per oscillator, plain scalar 0..1; nested under waveform a)
// 29 pluck brightness (per oscillator, plain scalar 0..1, 1 = unfiltered; nested under waveform a)
// 30 string dispersion (per oscillator, plain scalar 0..1, 0 = off; nested under waveform a)
// 31 dispersion sections (per oscillator, integer 1..6; nested under waveform a)
// Damping and Release damping have no rows of their own: with waveform a ==
// string, the duty a row (2) projects as Damping (target 26) and the pad tone
// a row (15) as Release damping (target 27), each with its block.

// 32 waveform b (list)
// 33 duty b (per-layer value, [0, 1])
// 34..45 duty b's 12-slot modulation block
// 46 pad tone b (per-layer value, [-1, +1])
// 47..58 pad tone b's 12-slot modulation block
// 59 mix mode (list)
// 60 waveform mix (per-layer value, [0, 1])
// 61..72 waveform mix's 12-slot modulation block
// 73 fm depth (per-layer value)
// 74..85 fm depth's 12-slot modulation block
// 86 fm ratio (per-layer base value)
// 87..98 fm ratio's 12-slot modulation block
// 99 volume (per-layer base value)
// 100..111 volume's 12-slot modulation block
// 112 pan (per-layer base value)
// 113..124 pan's 12-slot modulation block
// 125 pitch (per-layer base value)
// 126..137 pitch's 12-slot modulation block
// 138 octave offset (per oscillator, [-12, +12] whole octaves)
// 139 filter type (per oscillator, list)
// 140.. one uniform block per existing band, starting at
// osc_band_first_slot (140) with a 55-slot stride (osc_band_block_stride):
// the "Band N" header label, the band's [-] click slot, the band's [+]
// click slot, then the band's four parameter rows, each followed by its
// 12-slot modulation block (a 13-slot stride, in osc_target_row_slot
// order). Only the stored band count's blocks are projected; band 0's
// rows sit at 143/156/169/182 and bands 1..3 shift by 3 slots per
// earlier band (198/211/224/237, 253/266/279/292, 308/321/334/347).
constexpr uint32_t osc_waveform_a_prop    = 1;
constexpr uint32_t osc_waveform_b_prop    = 32;
constexpr uint32_t osc_mode_prop          = 59;
constexpr uint32_t osc_fm_ratio_prop      = 86;
constexpr uint32_t osc_octave_offset_prop = 138;
constexpr uint32_t osc_fir_slope_prop     = 182;
constexpr uint32_t osc_fir_type_prop      = 139;
// Header labels of the band blocks, indexed by band.
const char* const band_header_names[Sculptor::osc_band_max_bands] = { "Band 1", "Band 2", "Band 3", "Band 4" };
// Per-target presentation views: the single source of truth for value
// bounds, bank-unit scaling and slider curve.  Oscillator rows and
// parameter base values show display units; the bank stores scaled
// units (radians for fm depth).
constexpr Sculptor::OscTargetView osc_target_views[Synth::num_mod_targets] = {
    { 0.0f, 1.0f, 1.0f, false },                                         // volume
    { -24.0f, 24.0f, 1.0f, false },                                      // pitch (semitones)
    { 0.0f, 1.0f, 1.0f, false },                                         // pan (0.5 = center)
    { 0.0f, 1.0f, 1.0f, false },                                         // duty a
    { 0.0f, 1.0f, 1.0f, false },                                         // duty b
    { 0.0f, 1.0f, 1.0f, false },                                         // waveform mix
    { 0.0f, 1.0f, 6.2831853f, false },                                   // fm depth (1.0 == 2*pi radians)
    { 0.0f, 36.0f, 1.0f, false },                                        // fir pitch spread (semitones)
    { Synth::osc_mod_ratio_min, Synth::osc_mod_ratio_max, 1.0f, false }, // fm ratio
    { -1.0f, 1.0f, 1.0f, false },                                        // pad tone a
    { -1.0f, 1.0f, 1.0f, false },                                        // pad tone b
    { 1.0f, 100.0f, 1.0f, false },                                       // fir slope (percent)
    { -36.0f, 36.0f, 1.0f, false },                                      // band 0 offset (semitones)
    { 0.01f, 1.0f, 1.0f, false },                                        // band 0 gain
    { -36.0f, 36.0f, 1.0f, false },                                      // band 1 offset (semitones)
    { 0.0f, 36.0f, 1.0f, false },                                        // band 1 spread (semitones)
    { 0.01f, 1.0f, 1.0f, false },                                        // band 1 gain
    { 1.0f, 100.0f, 1.0f, false },                                       // band 1 slope (percent)
    { -36.0f, 36.0f, 1.0f, false },                                      // band 2 offset (semitones)
    { 0.0f, 36.0f, 1.0f, false },                                        // band 2 spread (semitones)
    { 0.01f, 1.0f, 1.0f, false },                                        // band 2 gain
    { 1.0f, 100.0f, 1.0f, false },                                       // band 2 slope (percent)
    { -36.0f, 36.0f, 1.0f, false },                                      // band 3 offset (semitones)
    { 0.0f, 36.0f, 1.0f, false },                                        // band 3 spread (semitones)
    { 0.01f, 1.0f, 1.0f, false },                                        // band 3 gain
    { 1.0f, 100.0f, 1.0f, false },                                       // band 3 slope (percent)
    { 0.0f, 1.0f, 1.0f, false },                                         // string damping
    { 0.0f, 1.0f, 1.0f, false },                                         // string release damping
};

// Low-pass and high-pass cutoff slider.  The editor writes a zero cutoff (edge off)
// when a low-pass slider reaches the maximum or a high-pass slider reaches the
// minimum, like the effect FIR sliders.  A zero cutoff stays valid in the JSON.
constexpr float                   osc_fir_cutoff_min_hz = 20.0f;
constexpr float                   osc_fir_cutoff_max_hz = 22050.0f;
constexpr Sculptor::OscTargetView osc_fir_cutoff_view   = { osc_fir_cutoff_min_hz, osc_fir_cutoff_max_hz, 1.0f, true };

// Filter off keeps the old value of either unit, so it accepts both ranges.
constexpr Sculptor::OscTargetView osc_fir_off_view = { 0.0f, osc_fir_cutoff_max_hz, 1.0f, false };

// Envelope node slot layout: 0 output, then descriptor-content properties.

// LFO node slot layout: 0 output, 1 depth source, 2 rate source, then
// descriptor-content properties.
constexpr uint32_t lfo_depth_input = 1;
constexpr uint32_t lfo_rate_input  = 2;

// Output node slot layout: one input per layer.

// Canonical kind-0 numbering is shared with the JSON codec; it lives in
// sculptor_instr_bank.h.  Only the fixed nodes (inputs, sum, oscillators)
// carry canonical indices; generator instances are record-keyed.
constexpr uint32_t canonical_first_osc  = Synth::graph_canonical_first_osc;
constexpr uint32_t canonical_first_env  = Synth::graph_canonical_first_env;
constexpr uint32_t canonical_node_count = Synth::graph_canonical_node_count;
} // namespace

// Oscillator-node rows whose widgets only make sense for the current
// waveform/mode selection.  Slot indices follow the projection layout
// contract in sculptor_osc_graph.cpp (the duty rows and their blocks need
// a sawtooth, pulse or pad wave on their side; waveform b, mix mode and
// fm ratio, plus the duty b, waveform mix and fm depth rows, need
// waveform b; the pad tone rows and their blocks need the pad waveform;
// the pick, brightness, dispersion, damping and release damping rows and their blocks need the
// string waveform (it grays no mode row);
// the cutoff row, its block and the fir slope need a filter type other
// than off).
bool Sculptor::oscillator_slot_waveform_mode_disabled(const Sculptor::Node& node, uint32_t slot_idx)
{
    if (! node.oscillator) {
        return false; // the disable rule applies to oscillator layer nodes only
    }
    if (node.slots.num_allocated < osc_slot_count) {
        return false;
    }
    const uint32_t wave_b = node.slots.entries[osc_waveform_b_prop].value.list_index;
    const uint32_t mode   = node.slots.entries[osc_mode_prop].value.list_index;
    const uint32_t duty0  = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_duty0)];
    const uint32_t duty1  = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_duty1)];
    const uint32_t mix    = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_osc_mix)];
    const uint32_t fm     = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_fm_index)];
    const uint32_t tone0  = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_pad_tilt0)];
    const uint32_t tone1  = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_pad_tilt1)];
    // Waveform a's list starts at sine, so pad sits one below the stored value.
    const uint32_t wave_a = node.slots.entries[osc_waveform_a_prop].value.list_index;
    if (wave_a != static_cast<uint32_t>(Synth::WaveType::string_wave) - 1u &&
        (slot_idx == osc_pick_slot || slot_idx == osc_brightness_slot || slot_idx == osc_dispersion_slot ||
         slot_idx == osc_dispersion_sections_slot)) {
        return true; // pick position, pluck brightness and dispersion only apply to the string waveform
    }
    if (wave_a != static_cast<uint32_t>(Synth::WaveType::pad_wave) - 1u &&
        wave_a != static_cast<uint32_t>(Synth::WaveType::string_wave) - 1u && slot_idx >= tone0 &&
        slot_idx <= tone0 + osc_target_block_stride) {
        return true; // pad tone a and its block only apply to the pad waveform (release damping under string)
    }
    if (slot_idx >= duty0 && slot_idx <= duty0 + osc_target_block_stride &&
        (wave_a == static_cast<uint32_t>(Synth::WaveType::sine_wave) - 1u ||
         wave_a == static_cast<uint32_t>(Synth::WaveType::noise_wave) - 1u)) {
        return true; // duty a shapes sawtooth and pulse and scales pad wander (damping under string); sine and
                     // noise ignore it
    }
    if (slot_idx >= duty1 && slot_idx <= duty1 + osc_target_block_stride &&
        (wave_b == static_cast<uint32_t>(Synth::WaveType::sine_wave) ||
         wave_b == static_cast<uint32_t>(Synth::WaveType::noise_wave))) {
        return true; // duty b is unused with sine and noise waveforms b
    }
    if (wave_b != static_cast<uint32_t>(Synth::WaveType::pad_wave) && slot_idx >= tone1 &&
        slot_idx <= tone1 + osc_target_block_stride) {
        return true; // pad tone b and its block only apply to the pad waveform
    }
    if (wave_b == 0 && (slot_idx == duty1 || slot_idx == osc_mode_prop || slot_idx == mix || slot_idx == fm ||
                        slot_idx == osc_fm_ratio_prop)) {
        return true; // duty b, mix mode, waveform mix and both fm rows need waveform b
    }
    if (slot_idx == fm && mode != 1) {
        return true; // fm depth only drives the fm mix mode
    }
    if (slot_idx == mix && (mode == 1 || mode == 2)) {
        return true; // waveform mix drives the blend and ring mod modes only
    }
    if (slot_idx == osc_fm_ratio_prop && mode == 0) {
        return true; // fm ratio drives fm, hard sync and ring mod, not blend
    }
    const uint32_t fir_type = node.slots.entries[osc_fir_type_prop].value.list_index;
    if (fir_type == static_cast<uint32_t>(Synth::fir_off)) {
        const uint32_t spread = osc_target_row_slot[static_cast<uint32_t>(Synth::mod_fir_spread)];
        const uint32_t slope_end =
            osc_target_row_slot[static_cast<uint32_t>(Synth::mod_fir_slope)] + osc_target_block_stride;
        if (slot_idx >= spread && slot_idx <= slope_end) {
            return true; // the cutoff row, its block, the slope row and its block are unused with the filter off
        }
    }
    return false;
}

// Parameter target index space: the ModTarget enumeration (0..9).  Every
// target has an oscillator value row, so there is no
// projected-index mapping of a five-target subset.

static uint32_t osc_target_prop(Synth::ModTarget target)
{
    return osc_target_row_slot[static_cast<uint32_t>(target)];
}

// The ModTarget (0..9) of a target value row, or -1 when the row is not a
// target value row at all.
static int32_t osc_row_target_index(uint32_t slot_idx)
{
    for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
        if (osc_target_prop(static_cast<Synth::ModTarget>(target)) == slot_idx) {
            return static_cast<int32_t>(target);
        }
    }
    return -1;
}

static bool osc_row_is_target(uint32_t slot_idx)
{
    return osc_row_target_index(slot_idx) >= 0;
}

static Sculptor::OscTargetView target_view_for(Synth::ModTarget target, Synth::FilterType fir_type)
{
    if (target == Synth::mod_fir_spread) {
        if (fir_type == Synth::fir_lowpass || fir_type == Synth::fir_highpass) {
            return osc_fir_cutoff_view;
        }
        if (fir_type == Synth::fir_off) {
            return osc_fir_off_view;
        }
    }
    return osc_target_views[static_cast<uint32_t>(target)];
}

// Spread row value in slider units: a zero low-pass cutoff shows at the maximum,
// a zero high-pass cutoff at the minimum.
static float fir_spread_display_value(const Synth::Oscillator& osc)
{
    const float base = osc.targets[Synth::mod_fir_spread].base_value;
    if (base == 0.0f && osc.fir_type == Synth::fir_lowpass) {
        return osc_fir_cutoff_max_hz;
    }
    if (base == 0.0f && osc.fir_type == Synth::fir_highpass) {
        return osc_fir_cutoff_min_hz;
    }
    return base;
}

// Inverse of fir_spread_display_value, with the slider range applied.  A type change keeps the number,
// so the range clamp also converts the old unit.  Filter off keeps the value.
static float fir_spread_bank_value(Synth::FilterType fir_type, float value)
{
    if (fir_type == Synth::fir_lowpass) {
        if (! (value > 0.0f) || value >= osc_fir_cutoff_max_hz) {
            return 0.0f;
        }
        return value < osc_fir_cutoff_min_hz ? osc_fir_cutoff_min_hz : value;
    }
    if (fir_type == Synth::fir_highpass) {
        if (! (value > osc_fir_cutoff_min_hz)) {
            return 0.0f;
        }
        return value > osc_fir_cutoff_max_hz ? osc_fir_cutoff_max_hz : value;
    }
    if (fir_type == Synth::fir_bandpass) {
        return value > 36.0f ? 36.0f : value;
    }
    return value;
}

static Sculptor::Slot osc_view_display_slot(const char*       name,
                                            float             display_value,
                                            Synth::ModTarget  target,
                                            Synth::FilterType fir_type = Synth::fir_bandpass)
{
    const Sculptor::OscTargetView view = target_view_for(target, fir_type);
    Sculptor::Slot                slot = bounded_real_slot(name, display_value, view.min_value, view.max_value);
    slot.real_logarithmic              = view.logarithmic;
    return slot;
}

// Connectable target value row: the layer's own base value, editable
// regardless of wiring; direct modulator wires drive the modulators on
// top (the block's envelope/LFO/Source dots), never the base.
static Sculptor::Slot osc_view_connectable_slot(const char*       name,
                                                float             display_value,
                                                Synth::ModTarget  target,
                                                bool              child_row = false,
                                                Synth::FilterType fir_type  = Synth::fir_bandpass)
{
    Sculptor::Slot slot = osc_view_display_slot(name, display_value, target, fir_type);
    slot.connectable    = true;
    slot.child_row      = child_row;
    slot.tooltip        = target_descs[target].tooltip;
    // The drop-menu anchor: exactly the ten target value rows take
    // creation menus when a drag from the row's dot releases over empty
    // canvas.
    slot.drop_menu = true;
    return slot;
}

// Band block grammar: one renderer line per band packs the band header
// label and its two click slots, as a child row of the Filter Type row
// (the legacy Cutoff row's depth), so the band tree reads one level
// shallower than the wave-5 depths while keeping the parent-child
// relation to Filter Type: header at 1, rows at 2, blocks at 3, rate
// and amount rows at 4.
// (The row-group ids live in the header; the renderer keys on the range;
// the block level sits in the anonymous namespace.)

// Band b's parameter target for parameter p (0 offset, 1 spread, 2 gain,
// 3 slope).  Band 0 keeps the legacy spread/slope targets so saved banks
// stay bit-compatible; only the row slots moved.
static constexpr Synth::ModTarget band_target(const uint32_t band, const uint32_t param)
{
    constexpr Synth::ModTarget band0_targets[4] = {
        Synth::mod_fir_band0_offset,
        Synth::mod_fir_spread,
        Synth::mod_fir_band0_gain,
        Synth::mod_fir_slope,
    };
    return band == 0 ? band0_targets[param] : static_cast<Synth::ModTarget>(14u + 4u * (band - 1u) + param);
}

// The layer's band count, clamped into the 1..4 projection range.
static uint32_t osc_band_model_count(const Synth::Oscillator& osc)
{
    return osc.fir_band_count < 1 ? 1u : osc.fir_band_count > 4 ? 4u : static_cast<uint32_t>(osc.fir_band_count);
}

// The ModTarget the shared duty a / pad tone a row carries: under the
// string wave the same slot and block hold Damping / Release damping.
// Other targets pass through.
static Synth::ModTarget osc_row_target_for_wave(bool string_wave, Synth::ModTarget target)
{
    if (string_wave && target == Synth::mod_duty0) {
        return Synth::mod_string_damping;
    }
    if (string_wave && target == Synth::mod_pad_tilt0) {
        return Synth::mod_string_release_damping;
    }
    return target;
}

// True when `target` owns a projected row under wave a: the shared rows
// belong to duty / pad tone unless the wave is string, and the damping
// targets exist only under string.  An unowned binding gets no row or wires
// (like a band beyond the count) and keeps its stored value.
static bool osc_target_owns_row(bool string_wave, uint32_t target)
{
    const bool damping = target == Synth::mod_string_damping || target == Synth::mod_string_release_damping;
    const bool shared  = target == Synth::mod_duty0 || target == Synth::mod_pad_tilt0;
    return string_wave ? ! shared : ! damping;
}

static bool osc_target_projected(const Synth::Oscillator& osc, uint32_t target)
{
    return osc_target_owns_row(osc.osc_type[0] == Synth::WaveType::string_wave, target);
}

// True when ModTarget `target` owns its row on `node` under the node's
// current wave a (osc_target_projected, read from the projected slots).
bool Sculptor::osc_node_target_row_active(const Sculptor::Node& node, uint32_t target)
{
    const bool string_wave = node.slots.entries[osc_waveform_a_prop].value.list_index ==
                             static_cast<uint32_t>(Synth::WaveType::string_wave) - 1u;
    return osc_target_owns_row(string_wave, target);
}

// True when the slot is one of the band header rows' [-]/[+] click slots
// (header slot + 1 and + 2 in every band block).
bool Sculptor::is_band_button_slot(const uint32_t slot_idx)
{
    if (slot_idx < osc_band_first_slot) {
        return false;
    }
    const uint32_t offset = (slot_idx - osc_band_first_slot) % osc_band_block_stride;
    return offset == 1u || offset == 2u;
}

// Connection families a target block's rows carry.
enum TargetFamily {
    family_env,
    family_lfo,
    family_src
};

// Adds target `target`'s 12-slot modulation block directly after the
// target's value row (the caller adds the osc node's slots in row
// order), seeded from the cell binding: field 1 carries the envelope
// depth, fields 3..5 the LFO op/depth/rate scale, fields 7..8 and
// 10..11 the MIDI source op/amount pairs.  Op shares its input row.
// Depth, Rate x, and Amount use separate nested rows.  Inputs past
// num_inputs seed the neutral add/0 grammar the compile reads through
// when a wire lands later.  Level_base shifts the whole block one tree
// level deeper: nested value rows pass 2, flat rows keep the default 1.
static void append_target_block(Graph*                      graph,
                                uint32_t                    node,
                                Synth::ModTarget            target,
                                const Synth::TargetBinding& gen,
                                uint8_t                     level_base = 1)
{
    const uint32_t t       = static_cast<uint32_t>(target);
    Sculptor::Slot env_dot = input_slot("Envelope");
    env_dot.child_row      = true;
    env_dot.child_level    = level_base;
    env_dot.row_group      = static_cast<uint8_t>(4 * t + 4);
    env_dot.tooltip        = "Pre-multiplier for envelope value before it's added to base parameter value";
    graph->add_slot(node, env_dot);
    Sculptor::Slot env_depth = bounded_real_slot("", gen.envelope_depth, 0.0f, 1.0f, false);
    env_depth.child_row      = true;
    env_depth.child_level    = level_base;
    env_depth.row_group      = static_cast<uint8_t>(4 * t + 4);
    env_depth.tooltip        = env_dot.tooltip;
    graph->add_slot(node, env_depth);
    Sculptor::append_lfo_row(graph,
                             node,
                             static_cast<uint8_t>(4 * t + 1),
                             static_cast<uint32_t>(gen.lfo_op),
                             gen.lfo_depth,
                             nullptr,
                             nullptr,
                             level_base);
    Sculptor::Slot rate_scale = real_slot("Rate x", gen.lfo_rate_scale_ms);
    rate_scale.child_row      = true;
    rate_scale.child_level    = static_cast<uint8_t>(level_base + 1);
    rate_scale.tooltip =
        "Pre-multiplier for MIDI input parameter connected to LFO Rate, applied to LFO wave length (period in ms), affects LFO speed";
    graph->add_slot(node, rate_scale);
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        const Synth::SourceOp op    = i < gen.num_inputs ? gen.inputs[i].op : Synth::SourceOp::add;
        const float           scale = i < gen.num_inputs ? gen.inputs[i].scale : 0.0f;
        Sculptor::append_mod_source_row(graph,
                                        node,
                                        i,
                                        static_cast<uint8_t>(4 * t + 2 + i),
                                        static_cast<uint32_t>(op),
                                        scale,
                                        level_base);
    }
}

// One band-parameter value row plus its 12-slot modulation block.  The
// per-band slope rows are integer-percent slots like the legacy FIR slope
// row; every other band row is a real slider over its view bounds.
static void append_band_row(Graph* graph, uint32_t node, Synth::ModTarget target, const Synth::Oscillator& osc)
{
    const uint32_t t        = static_cast<uint32_t>(target);
    const uint32_t slope_t  = static_cast<uint32_t>(Synth::mod_fir_band1_slope);
    const bool     is_slope = (t - slope_t) % 4u == 0u && t >= slope_t;
    // Band 1's Offset and Gain rows are inactive under the off, lowpass
    // and highpass filter types: their rows stay visible but carry no
    // tooltip.
    const bool band0_inactive = (target == Synth::mod_fir_band0_offset || target == Synth::mod_fir_band0_gain) &&
                                osc.fir_type != Synth::fir_bandpass;
    if (is_slope) {
        Sculptor::Slot slot = Sculptor::bounded_int_slot(target_descs[t].name,
                                                         static_cast<int32_t>(osc.targets[target].base_value + 0.5f),
                                                         1,
                                                         100);
        slot.child_row      = true;
        slot.child_level    = 2;
        slot.connectable    = true;
        slot.drop_menu      = true;
        slot.tooltip        = band0_inactive ? nullptr : target_descs[t].tooltip;
        graph->add_slot(node, slot);
    }
    else {
        Sculptor::Slot slot =
            osc_view_connectable_slot(target_descs[t].name, osc.targets[target].base_value, target, true);
        slot.child_level = 2;
        if (band0_inactive) {
            slot.tooltip = nullptr;
        }
        graph->add_slot(node, slot);
    }
    append_target_block(graph, node, target, osc.targets[target], osc_band_block_level);
}

// A wire being moved still occupies its old landing dot while the
// drag is in flight: the moving connection does not count as taken,
// so relocating it inside its own block works.
static bool dot_taken_for_move(const Graph& graph, uint32_t node_idx, uint32_t dot_idx)
{
    const uint32_t conn = connection_into(graph, node_idx, dot_idx);
    return conn != pool_no_slot && conn != graph.moving_connection;
}

// The first free input dot of target `target`'s block for connection
// family `family`: pool_no_slot when the family has no free input.  A
// base-row drop resolves its landing dot here; the source family fills
// Source A before Source B.
static uint32_t osc_target_free_input(const Sculptor::Graph& graph, uint32_t node_idx, uint32_t target, uint32_t family)
{
    if (family == family_env) {
        const uint32_t dot = osc_target_env_dot(target);
        return dot_taken_for_move(graph, node_idx, dot) ? pool_no_slot : dot;
    }
    if (family == family_lfo) {
        const uint32_t dot = osc_target_lfo_dot(target);
        return dot_taken_for_move(graph, node_idx, dot) ? pool_no_slot : dot;
    }
    for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
        const uint32_t dot = osc_target_src_dot(target, i);
        if (! dot_taken_for_move(graph, node_idx, dot)) {
            return dot;
        }
    }
    return pool_no_slot;
}

// Decodes an oscillator-node input endpoint: `target` receives the
// ModTarget index, `family` the row family, `dot` the specific input dot
// (pool_no_slot for the base value row, the standing drop target), and
// `base_row` reports the base row.  Returns false for endpoints that are
// not target-block inputs.
static NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx);

static bool decode_osc_input(const Sculptor::OscGraphMapping& mapping,
                             Sculptor::EndPoint               point,
                             bool*                            base_row,
                             uint32_t*                        target,
                             uint32_t*                        family,
                             uint32_t*                        dot)
{
    if (node_role(mapping, point.node_idx) != role_osc) {
        return false;
    }

    *base_row                = false;
    *dot                     = pool_no_slot;
    const int32_t row_target = osc_row_target_index(point.slot_idx);
    if (row_target >= 0) {
        *base_row = true;
        *target   = static_cast<uint32_t>(row_target);
        *family   = family_src; // decided by the wire's output end
        return true;
    }

    uint32_t block_target = 0;
    uint32_t block_field  = 0;
    if (! osc_target_block_field(point.slot_idx, &block_target, &block_field)) {
        return false;
    }

    *target = block_target;
    if (block_field == 0) {
        *family = family_env;
        *dot    = osc_target_env_dot(*target);
        return true;
    }
    if (block_field == 2) {
        *family = family_lfo;
        *dot    = osc_target_lfo_dot(*target);
        return true;
    }
    if (block_field == 6 || block_field == 9) {
        *family = family_src;
        *dot    = osc_target_src_dot(*target, block_field == 6 ? 0 : 1);
        return true;
    }
    return false;
}

static NodeRole node_role(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    if (mapping.input_node == node_idx) {
        return role_input;
    }
    if (mapping.instrument_node == node_idx) {
        return role_input;
    }
    if (mapping.output_node == node_idx) {
        return role_output;
    }
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == node_idx) {
            return role_osc;
        }
    }
    // Generator instances resolve through the registry, so they validate on
    // connect and their content edits route into the descriptor-edit path.
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].node_idx == node_idx) {
            return mapping.detached[i].kind == 1 ? role_env : role_lfo;
        }
    }
    return role_none;
}

static int32_t find_instance(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx)
{
    if (node_idx == Sculptor::pool_no_slot) {
        return -1;
    }
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].node_idx == node_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

static OutputKind classify_output(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_input:
            return out_input;
        case role_osc:
            return point.slot_idx == mapping.osc_output_slot ? out_osc : out_none;
        case role_env:
            return point.slot_idx == mapping.env_output_slot ? out_env : out_none;
        case role_lfo:
            return point.slot_idx == mapping.lfo_output_slot ? out_lfo : out_none;
        default:
            return out_none;
    }
}

static InputKind classify_input(const Sculptor::OscGraphMapping& mapping, Sculptor::EndPoint point)
{
    switch (node_role(mapping, point.node_idx)) {
        case role_lfo:
            if (point.slot_idx == mapping.lfo_depth_input_slot) {
                return in_depth;
            }
            if (point.slot_idx == mapping.lfo_rate_input_slot) {
                return in_rate;
            }
            return in_none;
        case role_output:
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (point.slot_idx == mapping.output_layer_input_slot[layer]) {
                    return in_sum;
                }
            }
            return in_none;
        default:
            return in_none;
    }
}

// Resolves the MIDI source feeding a connector through the inputs node's
// per-source slots; false when the source is not a projected MIDI input.
static bool edge_source(const Sculptor::Graph&           graph,
                        const Sculptor::OscGraphMapping& mapping,
                        uint32_t                         connection_idx,
                        Synth::ModSource*                source)
{
    const uint32_t node_idx = graph.get_connection(connection_idx).output.node_idx;
    if (node_idx != mapping.input_node) {
        return false;
    }
    const uint32_t slot_idx = graph.get_connection(connection_idx).output.slot_idx;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        if (mapping.input_source_slots[i] == slot_idx) {
            *source = static_cast<Synth::ModSource>(i + 1);
            return true;
        }
    }
    return false;
}

// Resolves an inputs-node output slot to its assignable CC index; -1 when
// the slot is not a projected CC dot.
static int32_t input_cc_index(const Sculptor::OscGraphMapping& mapping, uint32_t slot_idx)
{
    if (slot_idx == Sculptor::pool_no_slot) {
        return -1;
    }

    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        if (mapping.input_cc_slots[i] == slot_idx) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Resolves the assignable CC slot index feeding a connector through the
// inputs node's CC dots; -1 when the connection does not leave a CC dot.
static int32_t edge_cc_index(const Sculptor::Graph&           graph,
                             const Sculptor::OscGraphMapping& mapping,
                             uint32_t                         connection_idx)
{
    const uint32_t node_idx = graph.get_connection(connection_idx).output.node_idx;
    if (node_idx != mapping.input_node) {
        return -1;
    }
    return input_cc_index(mapping, graph.get_connection(connection_idx).output.slot_idx);
}

static bool read_list_prop(const Sculptor::Slot& slot, uint32_t* index)
{
    if (slot.property_type != Sculptor::PropertyType::list || slot.value.list_index >= slot.num_list_options) {
        return false;
    }
    *index = slot.value.list_index;
    return true;
}

static void clear_mapping(Sculptor::OscGraphMapping* mapping)
{
    mapping->source_bank = nullptr;
    mapping->input_node  = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < Sculptor::num_osc_graph_inputs; ++i) {
        mapping->input_source_slots[i] = Sculptor::pool_no_slot;
    }
    for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
        mapping->input_cc_slots[i] = Sculptor::pool_no_slot;
    }
    mapping->output_node = Sculptor::pool_no_slot;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        mapping->output_layer_input_slot[layer] = Sculptor::pool_no_slot;
        mapping->osc_nodes[layer]               = Sculptor::pool_no_slot;
    }
    mapping->osc_output_slot = Sculptor::pool_no_slot;
    mapping->instrument_node = Sculptor::pool_no_slot;
    for (uint32_t i = 0; i < 3; ++i) {
        mapping->instrument_slots[i] = Sculptor::pool_no_slot;
    }
    // Uniform node layouts; populated even when no such node exists so the
    // mapping always describes the full projection layout.
    mapping->env_output_slot      = 0;
    mapping->lfo_output_slot      = 0;
    mapping->lfo_depth_input_slot = lfo_depth_input;
    mapping->lfo_rate_input_slot  = lfo_rate_input;
    mapping->channel              = 0;
    mapping->zone                 = 0;
    for (uint32_t i = 0; i < Sculptor::max_osc_instances; ++i) {
        mapping->detached[i].node_idx     = Sculptor::pool_no_slot;
        mapping->detached[i].kind         = 0;
        mapping->detached[i].desc_id      = 0;
        mapping->detached[i].uid          = 0;
        mapping->detached[i].depth_source = 0;
        mapping->detached[i].rate_source  = 0;
    }
    mapping->detached_count = 0;
}

static void project_osc_node(Sculptor::Graph*           graph,
                             uint32_t                   layer,
                             const Synth::Oscillator&   osc,
                             Sculptor::OscGraphMapping* mapping)
{
    char name[32];
    snprintf(name, sizeof(name), "Oscillator %u", layer + 1);
    const uint32_t node = graph->create_node(name, vmath::vec2(512.0f, 128.0f + 256.0f * static_cast<float>(layer)));
    if (node == Sculptor::pool_no_slot) {
        return;
    }
    graph->set_node_oscillator(node, true);
    mapping->osc_nodes[layer] = node;

    mapping->osc_output_slot = graph->add_slot(node, with_tooltip(output_slot("Output"), "Oscillator output"));

    // Waveform A cannot be off: the list starts at sine and compile adds
    // one back onto the wave type.  Old banks storing off clamp to sine.
    graph->add_slot(
        node,
        with_tooltip(
            list_slot("Waveform A",
                      wave_names + 1,
                      6,
                      osc.osc_type[0] == Synth::WaveType::no_wave ? 0 : static_cast<uint32_t>(osc.osc_type[0]) - 1),
            "Primary oscillator wave shape"));
    // With the string wave the duty a row projects as Damping (target 26) and
    // the pad tone a row as Release damping (target 27).
    const bool             string_wave     = osc.osc_type[0] == Synth::WaveType::string_wave;
    const Synth::ModTarget duty_row_target = osc_row_target_for_wave(string_wave, Synth::mod_duty0);
    const Synth::ModTarget tone_row_target = osc_row_target_for_wave(string_wave, Synth::mod_pad_tilt0);
    // One 12-slot modulation block per ModTarget, directly after the
    // target's value row, seeded from the layer's own binding
    // (display-unit fields carry the raw values; only the base rows are
    // scaled).
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[duty_row_target].name,
                                              osc.targets[duty_row_target].base_value,
                                              duty_row_target,
                                              true));
    append_target_block(graph, node, duty_row_target, osc.targets[duty_row_target], 2);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[tone_row_target].name,
                                              osc.targets[tone_row_target].base_value,
                                              tone_row_target,
                                              true));
    append_target_block(graph, node, tone_row_target, osc.targets[tone_row_target], 2);
    // Pick position (a plain layer scalar, not modulatable) nests under
    // Waveform A after the pad tone row's block.
    Sculptor::Slot pick_slot =
        with_tooltip(bounded_real_slot("Pick position", osc.string_pick, 0.0f, 1.0f),
                     "Position along the string where the pluck happens; shapes the note-onset timbre");
    pick_slot.child_row = true;
    graph->add_slot(node, pick_slot);
    // Pluck Brightness (a plain layer scalar like Pick position) follows it.
    Sculptor::Slot brightness_slot =
        with_tooltip(bounded_real_slot("Pluck Brightness", osc.string_brightness, 0.0f, 1.0f),
                     "Brightness of the pluck; lower values low-pass the initial burst for a warmer, softer attack");
    brightness_slot.child_row = true;
    graph->add_slot(node, brightness_slot);
    // String Dispersion and its section count (plain layer scalars) follow.
    Sculptor::Slot dispersion_slot =
        with_tooltip(bounded_real_slot("String Dispersion", osc.string_dispersion, 0.0f, 1.0f),
                     "Stiff-string inharmonicity: raises the upper partials, strongest on low notes (0 = off)");
    dispersion_slot.child_row = true;
    graph->add_slot(node, dispersion_slot);
    // A stepper (unbounded integer row): the renderer clamps typed or stepped values into [real_min, real_max].
    Sculptor::Slot dispersion_sections_slot =
        with_tooltip(int_slot("Dispersion Sections", static_cast<int32_t>(osc.string_dispersion_sections)),
                     "Number of dispersion filter sections (1 to 6); more sections stretch the partials further");
    dispersion_sections_slot.real_min  = 1.0f;
    dispersion_sections_slot.real_max  = 6.0f;
    dispersion_sections_slot.child_row = true;
    graph->add_slot(node, dispersion_sections_slot);

    graph->add_slot(node,
                    with_tooltip(list_slot("Waveform B", wave_names, 6, static_cast<uint32_t>(osc.osc_type[1])),
                                 "Secondary oscillator wave shape"));
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[4].name,
                                              osc.targets[Synth::mod_duty1].base_value,
                                              Synth::mod_duty1,
                                              true));
    append_target_block(graph, node, Synth::mod_duty1, osc.targets[Synth::mod_duty1], 2);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[10].name,
                                              osc.targets[Synth::mod_pad_tilt1].base_value,
                                              Synth::mod_pad_tilt1,
                                              true));
    append_target_block(graph, node, Synth::mod_pad_tilt1, osc.targets[Synth::mod_pad_tilt1], 2);
    graph->add_slot(node,
                    with_tooltip(list_slot("Mix Mode", osc_mode_names, 4, static_cast<uint32_t>(osc.osc_mode)),
                                 "How the two oscillator waves are combined"));
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[5].name,
                                              osc.targets[Synth::mod_osc_mix].base_value,
                                              Synth::mod_osc_mix,
                                              true));
    append_target_block(graph, node, Synth::mod_osc_mix, osc.targets[Synth::mod_osc_mix], 2);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[6].name,
                                              osc.targets[Synth::mod_fm_index].base_value /
                                                  osc_target_views[Synth::mod_fm_index].bank_scale,
                                              Synth::mod_fm_index,
                                              true));
    append_target_block(graph, node, Synth::mod_fm_index, osc.targets[Synth::mod_fm_index], 2);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[8].name,
                                              osc.targets[Synth::mod_fm_ratio].base_value,
                                              Synth::mod_fm_ratio,
                                              true));
    append_target_block(graph, node, Synth::mod_fm_ratio, osc.targets[Synth::mod_fm_ratio], 2);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[Synth::mod_volume].name,
                                              osc.targets[Synth::mod_volume].base_value,
                                              Synth::mod_volume));
    append_target_block(graph, node, Synth::mod_volume, osc.targets[Synth::mod_volume]);
    graph->add_slot(node,
                    osc_view_connectable_slot(target_descs[Synth::mod_panning].name,
                                              osc.targets[Synth::mod_panning].base_value,
                                              Synth::mod_panning));
    append_target_block(graph, node, Synth::mod_panning, osc.targets[Synth::mod_panning]);
    // The Pitch base value is a bounded bare input box: populated min/max
    // on an unbounded real slot (real_bounded false) means clamp-on-edit;
    // the renderer's guard is min < max. The removed Pitch Offset row
    // was a narrower static subset of this same value.
    Sculptor::Slot pitch_slot = osc_view_connectable_slot(target_descs[Synth::mod_pitch].name,
                                                          osc.targets[Synth::mod_pitch].base_value,
                                                          Synth::mod_pitch);
    pitch_slot.real_bounded   = false;
    graph->add_slot(node, pitch_slot);
    append_target_block(graph, node, Synth::mod_pitch, osc.targets[Synth::mod_pitch]);
    Sculptor::Slot octave_offset_slot = with_tooltip(Sculptor::bounded_int_slot("Octave Offset",
                                                                                osc.octave_offset,
                                                                                Sculptor::osc_octave_offset_min,
                                                                                Sculptor::osc_octave_offset_max),
                                                     "Octave shift applied to pitch (-12..12)");
    octave_offset_slot.child_row      = true;
    graph->add_slot(node, octave_offset_slot);
    graph->add_slot(
        node,
        with_tooltip(
            list_slot("Filter Type", filter_type_names, Synth::num_filter_types, static_cast<uint32_t>(osc.fir_type)),
            "Filter applied to oscillator output"));

    // One uniform block per existing band, nested under Filter Type:
    // the "Band N" header label row with its [-]/[+] click slots one
    // level under the Filter Type row, then the band's four parameter
    // rows, each followed by its 12-slot modulation block. Every existing
    // band is always visible - there is no band selection. [-] on band N
    // removes band N (later bands shift up); [+] inserts a copy of band N
    // after it. The click slots' toggle values stay untouched (a press is
    // an action, never a state); the edit-disabled flags gray them at the
    // count bounds.
    const uint32_t band_count = osc_band_model_count(osc);
    for (uint32_t band = 0; band < band_count; ++band) {
        const uint8_t  row_group = static_cast<uint8_t>(Sculptor::osc_band_row_group + band);
        Sculptor::Slot header    = Sculptor::make_slot(band_header_names[band], Sculptor::SlotKind::property);
        header.row_group         = row_group;
        // The header line anchors the row group's nesting: one level under
        // Filter Type, like the legacy Cutoff row.
        header.child_row   = true;
        header.child_level = 1;
        graph->add_slot(node, header);
        Sculptor::Slot band_minus_slot = Sculptor::toggle_slot("-", false);
        band_minus_slot.row_group      = row_group;
        band_minus_slot.tooltip        = "Delete this band";
        graph->add_slot(node, band_minus_slot);
        Sculptor::Slot band_plus_slot = Sculptor::toggle_slot("+", false);
        band_plus_slot.row_group      = row_group;
        band_plus_slot.tooltip        = "Add a new band";
        graph->add_slot(node, band_plus_slot);

        // One uniform row group per band: Offset, Spread, Gain, Slope.  Band 0's
        // Spread/Slope rows keep their legacy targets (7/11) so saved banks stay
        // bit-compatible; only their slots moved.  Every band row nests
        // one level under the band header row, so its modulation block
        // shifts with it.  Band 0's Spread/Slope slots double as the legacy
        // single-edge rows (Cutoff (Hz) / FIR Slope) under low-pass and
        // high-pass: those keep the legacy depths (row 1, block 2), while
        // the bandpass band block sits one level deeper (rows 2, blocks 3).
        const bool    bandpass_layout   = osc.fir_type == Synth::fir_bandpass;
        const uint8_t band0_row_level   = bandpass_layout ? 2 : 1;
        const uint8_t band0_block_level = bandpass_layout ? osc_band_block_level : 2;
        if (band == 0) {
            append_band_row(graph, node, Synth::mod_fir_band0_offset, osc);
            {
                Sculptor::Slot spread_row = osc_view_connectable_slot(fir_spread_descs[osc.fir_type].name,
                                                                      fir_spread_display_value(osc),
                                                                      Synth::mod_fir_spread,
                                                                      true,
                                                                      osc.fir_type);
                // The Spread row's text follows the filter type, not the
                // Band spread target: under lowpass and highpass the row
                // shows the cutoff text, and under off it stays exempt.
                spread_row.tooltip     = fir_spread_descs[osc.fir_type].tooltip;
                spread_row.child_level = band0_row_level;
                graph->add_slot(node, spread_row);
            }
            append_target_block(graph,
                                node,
                                Synth::mod_fir_spread,
                                osc.targets[Synth::mod_fir_spread],
                                band0_block_level);
            append_band_row(graph, node, Synth::mod_fir_band0_gain, osc);
            // The FIR Slope row stays an integer-percent slot: it displays the effective slope (the routed
            // base when the binding is present, the layer's fir_slope scalar otherwise, per the shared
            // seed rule the runtime note-on uses), clamped to 1..100 before the integer cast. Compile
            // reads a wired row back into the binding base; an unwired layer keeps its zeroed binding and
            // the scalar carries the value.
            const float slope_seed =
                Synth::osc_fir_slope_seed_value(osc.targets[Synth::mod_fir_slope], osc.fir_slope_percent);
            const float    slope_display = slope_seed < 1.0f ? 1.0f : slope_seed > 100.0f ? 100.0f : slope_seed;
            Sculptor::Slot fir_slope_slot =
                Sculptor::bounded_int_slot("FIR Slope", static_cast<int32_t>(slope_display + 0.5f), 1, 100);
            fir_slope_slot.child_row   = true;
            fir_slope_slot.child_level = band0_row_level;
            fir_slope_slot.connectable = true;
            fir_slope_slot.drop_menu   = true;
            // Under the off filter type the slope row is a dead variant:
            // visible but exempt from tooltips.
            fir_slope_slot.tooltip =
                osc.fir_type == Synth::fir_off ? nullptr : target_descs[Synth::mod_fir_slope].tooltip;
            graph->add_slot(node, fir_slope_slot);
            append_target_block(graph,
                                node,
                                Synth::mod_fir_slope,
                                osc.targets[Synth::mod_fir_slope],
                                band0_block_level);
        }
        else {
            append_band_row(graph, node, band_target(band, 0), osc);
            append_band_row(graph, node, band_target(band, 1), osc);
            append_band_row(graph, node, band_target(band, 2), osc);
            append_band_row(graph, node, band_target(band, 3), osc);
        }
    }
}

// Node construction shared by the derived projection and record-driven
// creation, so the slot layouts cannot drift.  Slot 0 is always the output;
// env content follows, then for LFOs the depth/rate inputs.
static uint32_t create_env_node(Sculptor::Graph*                 graph,
                                const char*                      name,
                                vmath::vec2                      position,
                                const Synth::EnvelopeDescriptor& env)
{
    const uint32_t node = graph->create_node(name, position);
    if (node == Sculptor::pool_no_slot) {
        return Sculptor::pool_no_slot;
    }
    // Records carry arbitrary (unsnapped) editor positions; the exact
    // position overrides create_node's grid snap.  Envelope nodes are wider
    // so the curve widget has room to work.
    graph->set_node_layout(node, position, Sculptor::envelope_node_content_width, 0.0f);
    graph->add_slot(node, with_tooltip(output_slot("Value"), "Envelope output"));
    graph->add_slot(node,
                    with_tooltip(real_slot("Value (min)", env.min_value), "Lowest value produced by the envelope"));
    graph->add_slot(node,
                    with_tooltip(real_slot("Value (max)", env.min_value + 65535.0f * env.min_max_delta),
                                 "Highest value produced by the envelope"));
    graph->add_slot(node,
                    with_tooltip(Sculptor::toggle_slot("Ping-Pong", (env.flags & Synth::envelope_ping_pong) != 0),
                                 "Bounces the sustain loop back and forth instead of looping it"));
    graph->add_slot(
        node,
        with_tooltip(Sculptor::toggle_slot("Quick Release", (env.flags & Synth::envelope_quick_release) != 0),
                     "On note off, jumps behind sustain loop end"));
    // Point and sustain editing lives in the curve state widget; property
    // slots for those fields would be a second stale UI over the same data.
    return node;
}

static uint32_t create_lfo_node(Sculptor::Graph*            graph,
                                const char*                 name,
                                vmath::vec2                 position,
                                const Synth::LFODescriptor& lfo)
{
    const uint32_t node = graph->create_node(name, position);
    if (node == Sculptor::pool_no_slot) {
        return Sculptor::pool_no_slot;
    }
    // Records carry arbitrary (unsnapped) editor positions; the exact
    // position overrides create_node's grid snap.
    graph->set_node_layout(node, position, 0.0f, 0.0f);
    graph->add_slot(node, with_tooltip(output_slot("Value"), "LFO output"));
    graph->add_slot(node, with_tooltip(input_slot("Depth"), "MIDI input parameter multiplier for LFO value"));
    graph->add_slot(node,
                    with_tooltip(input_slot("Rate"),
                                 "MIDI input parameter applied to LFO wave length (period in ms), multiplied by Rate x "
                                 "from the target"));
    graph->add_slot(
        node,
        with_tooltip(list_slot("Waveform", lfo_wave_names, 2, lfo.wave == Synth::WaveType::sine_wave ? 0 : 1),
                     "LFO wave shape"));
    // Duty shapes sawtooth only: sine has no phase to widen.
    const uint32_t duty_idx =
        graph->add_slot(node,
                        with_tooltip(bounded_real_slot("Duty", static_cast<float>(lfo.duty) / 255.0f, 0.0f, 1.0f),
                                     "Sawtooth peak position (0 = falling ramp, 0.5 = triangle, 1 = rising ramp)"));
    graph->set_slot_edit_disabled(node, duty_idx, lfo.wave == Synth::WaveType::sine_wave);
    const uint32_t period_idx =
        graph->add_slot(node, with_tooltip(int_slot("Period (ms)", lfo.period_ms), "LFO wave length (period in ms)"));
    graph->set_slot_edit_disabled(node, period_idx, lfo.beat_div != 0);
    // Shared slot order with the effect-graph LFO projection
    // (sculptor_effect_graph.cpp): the fx apply dispatch is index-based.
    graph->add_slot(node,
                    with_tooltip(bounded_real_slot("Pulse", static_cast<float>(lfo.pulse_mix) / 255.0f, 0.0f, 1.0f),
                                 "Clips the LFO wave toward a square pulse (0 = smooth, 1 = square)"));
    graph->add_slot(node,
                    with_tooltip(list_slot("Sync", lfo_sync_names, 6, lfo.beat_div <= 5 ? lfo.beat_div : 0),
                                 "Synchronize LFO wave length with tempo (in note durations), instead of Period (ms)"));
    graph->add_slot(node,
                    with_tooltip(list_slot("Start", lfo_start_names, 3, lfo.start_point <= 2 ? lfo.start_point : 0),
                                 "Initial value of the LFO wave at note start"));
    return node;
}

// Appends a derivation key unless an identical key is already listed;
// the keys array lists each distinct instance once.
static void append_instance_key(InstanceKey* keys, uint32_t& count, const InstanceKey& key)
{
    uint32_t k = 0;
    while (k < count && (keys[k].kind != key.kind || keys[k].desc_id != key.desc_id ||
                         keys[k].depth_source != key.depth_source || keys[k].rate_source != key.rate_source)) {
        ++k;
    }
    if (k == count) {
        keys[count++] = key;
    }
}

// Deterministic derivation: one key per distinct (kind, descriptor,
// sources) tuple over the projected rows, in target order then row
// order: the deterministic instance numbering the projection and the
// node-count check share.  A binding's envelope key comes before its
// LFO key.  rows[row] names the oscillator the row projects; the walk
// covers rows 0..run_length - 1.
static uint32_t derive_row_instance_keys(const Synth::Oscillator* const* rows, uint32_t run_length, InstanceKey* keys)
{
    uint32_t count = 0;
    for (uint32_t target_idx = 0; target_idx < Synth::num_mod_targets; ++target_idx) {
        for (uint32_t row = 0; row < run_length; ++row) {
            if (rows[row] == nullptr) {
                continue;
            }

            const Synth::TargetBinding& gen = rows[row]->targets[target_idx];
            if (gen.envelope_desc_id != 0) {
                const InstanceKey key = { 1, static_cast<uint8_t>(gen.envelope_desc_id), 0, 0 };
                append_instance_key(keys, count, key);
            }

            if (gen.lfo_desc_id != 0) {
                const InstanceKey key = { 2,
                                          static_cast<uint8_t>(gen.lfo_desc_id),
                                          static_cast<uint8_t>(gen.lfo_depth_source),
                                          static_cast<uint8_t>(gen.lfo_rate_source) };
                append_instance_key(keys, count, key);
            }
        }
    }

    return count;
}

static uint32_t derive_instance_keys(const Synth::Instrument& instrument, InstanceKey* keys)
{
    const Synth::Oscillator* rows[Synth::max_layers] = {};
    uint32_t                 run_length              = 0;
    const uint32_t           layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    for (; run_length < layer_count; ++run_length) {
        rows[run_length] = &instrument.layers[run_length];
    }

    return derive_row_instance_keys(rows, layer_count, keys);
}

// Wires one target block's source rows from the cell binding's MIDI
// inputs.  Standard sources resolve through the input node's source
// slots; CC sources through the assignable-CC dots, where an absent dot,
// or a cc_number outside the assignable range, leaves the row unwired -
// decode never synthesizes registry bits.  Skips the none source and the
// one-past-the-end sentinel slot.  Returns false when a wire cannot be
// added: a silently dropped source wire loses the decoded MIDI routing
// at the next compile.
static bool wire_target_sources(Sculptor::Graph*                 graph,
                                const Sculptor::OscGraphMapping& mapping,
                                uint32_t                         osc_node_idx,
                                uint32_t                         target,
                                const Synth::ModInput*           inputs,
                                uint32_t                         num_inputs)
{
    if (num_inputs > Synth::max_mod_inputs) {
        num_inputs = Synth::max_mod_inputs;
    }
    for (uint32_t i = 0; i < num_inputs; ++i) {
        if (inputs[i].source == Synth::ModSource::cc) {
            const uint32_t cc_number = inputs[i].cc_number;
            if (cc_number < Synth::cc_msb_base || cc_number - Synth::cc_msb_base >= Synth::num_assignable_ccs) {
                continue;
            }
            const uint32_t cc_index = cc_number - Synth::cc_msb_base;
            if (mapping.input_cc_slots[cc_index] == Sculptor::pool_no_slot) {
                continue;
            }
            if (graph->add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_cc_slots[cc_index] },
                                      Sculptor::EndPoint{ osc_node_idx, osc_target_src_dot(target, i) }) ==
                Sculptor::pool_no_slot) {
                return false;
            }
            continue;
        }
        const uint32_t source = static_cast<uint32_t>(inputs[i].source);
        if (source == 0 || source > Sculptor::num_osc_graph_inputs) {
            continue;
        }
        if (graph->add_connection(Sculptor::EndPoint{ mapping.input_node, mapping.input_source_slots[source - 1] },
                                  Sculptor::EndPoint{ osc_node_idx, osc_target_src_dot(target, i) }) ==
            Sculptor::pool_no_slot) {
            return false;
        }
    }
    return true;
}

// Every band header's [-]/[+] buttons stay visible at the band-count
// bounds and gray instead: all [+] disable at four bands, all [-] at one.
// Runs on the projection's rebuild: the band insert/remove edits are
// structural and rebuild the graph from the model.
static void stamp_band_click_disabled(Sculptor::Graph* graph, const uint32_t osc_node)
{
    const Sculptor::Node& node = graph->node(osc_node);
    if (node.slots.num_allocated < osc_band_first_slot + osc_band_block_stride) {
        return;
    }
    const uint32_t count = (node.slots.num_allocated - osc_band_first_slot) / osc_band_block_stride;
    for (uint32_t band = 0; band < count; ++band) {
        const uint32_t header = osc_band_header_slot(band);
        graph->set_slot_edit_disabled(osc_node, header + 1u, count <= 1u);
        graph->set_slot_edit_disabled(osc_node, header + 2u, count >= 4u);
    }
}

// A target block's LFO and source-row knobs shape the signal arriving on
// that row's input; with no input connected they change nothing.  Runs
// over every projected oscillator node: the projection calls it after a
// rebuild and the apply path after an eventless relocation.
static void install_edit_disabled_flags(Sculptor::Graph* graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        const uint32_t node = mapping.osc_nodes[layer];
        if (node == Sculptor::pool_no_slot || ! graph->node_occupied(node)) {
            continue;
        }
        stamp_band_click_disabled(graph, node);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const bool env_wired = graph->slot_is_connected(node, osc_target_env_dot(t));
            graph->set_slot_edit_disabled(node, osc_target_env_depth(t), ! env_wired);
            const bool lfo_wired = graph->slot_is_connected(node, osc_target_lfo_dot(t));
            for (uint32_t f = 3; f <= 5; ++f) {
                graph->set_slot_edit_disabled(node, osc_target_block_base(t) + f, ! lfo_wired);
            }
            for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
                const uint32_t dot       = osc_target_src_dot(t, i);
                const bool     src_wired = graph->slot_is_connected(node, dot);
                graph->set_slot_edit_disabled(node, dot + 1, ! src_wired);
                graph->set_slot_edit_disabled(node, dot + 2, ! src_wired);
            }
        }
    }
}

// Editor-facing helpers exported from sculptor_osc_graph.h.  Defined at
// file scope with explicit prefixes; the file-local sections above and below
// reach them through using-declarations.

bool Sculptor::env_volume_shape_ok(const Synth::EnvelopeDescriptor& env)
{
    // The span must be nonnegative too: a negative delta would evaluate the
    // interior points below the zero floor.
    // Tail values remain authored (a sustain release may end above 0), and
    // the first point's value is authored like every other point: the
    // runtime ramps volume from silence at note-on.
    return env.min_value == 0.0f && env.min_max_delta >= 0.0f;
}

// Forces the volume-envelope shape: minimum 0 with the effective top preserved;
// raw point values stay authored.  Returns false when the effective top
// is negative, which cannot be preserved under the nonnegative volume range.
// The drop-commit preset converts a freshly allocated descriptor before the
// commit, so a volume binding never lands in the bipolar default shape the
// apply path would have to convert (or the validator refuse).
bool Sculptor::env_convert_to_volume_shape(Synth::EnvelopeDescriptor& env)
{
    const float max_value = env.min_value + 65535.0f * env.min_max_delta;
    if (max_value < 0.0f) {
        return false;
    }
    env.min_value     = 0.0f;
    env.min_max_delta = max_value / 65535.0f;
    return true;
}

// Per-family modulator-row visibility for the target's block: the family
// rows render only while their family dot carries a wire (the fx
// param_field pattern).  The base row never hides: it is always
// connectable and always renders its value.
bool Sculptor::osc_target_field_hidden(const Graph& graph, uint32_t node_idx, uint32_t target, uint32_t field)
{
    if (field <= 1) {
        return connection_into(graph, node_idx, osc_target_env_dot(target)) == pool_no_slot;
    }
    if (field <= 5) {
        return connection_into(graph, node_idx, osc_target_lfo_dot(target)) == pool_no_slot;
    }
    const uint32_t src = field < 9 ? 0 : 1;
    return connection_into(graph, node_idx, osc_target_src_dot(target, src)) == pool_no_slot;
}

// The projected filter type of an oscillator node (its Filter Type list
// slot), for callers composing the band visibility with the legacy
// single-edge rows.  A node too small to carry the slot reads as fir_off.
static Synth::FilterType osc_node_filter_type(const Graph& graph, const uint32_t node_idx)
{
    const Sculptor::Node& node = graph.node(node_idx);
    if (node.slots.num_allocated <= osc_fir_type_prop) {
        return Synth::fir_off;
    }
    return static_cast<Synth::FilterType>(node.slots.entries[osc_fir_type_prop].value.list_index);
}

// Band-derived render visibility: the band UI hides wholesale for
// the single-edge filter types; under bandpass every existing band is
// always visible (headers, parameter rows, blocks and their wires), so
// nothing in the band region hides. Only existing bands' blocks are
// projected, so there is no per-band hiding left.
bool Sculptor::osc_band_slot_hidden(const Graph& graph, const uint32_t node_idx, const uint32_t slot_idx)
{
    const Sculptor::Node& node = graph.node(node_idx);
    if (node.slots.num_allocated <= osc_band_first_slot || slot_idx < osc_band_first_slot) {
        return false;
    }
    return osc_node_filter_type(graph, node_idx) != Synth::fir_bandpass;
}

// The composed render filter for an oscillator node's slot: band-derived
// visibility, except the legacy cutoff and FIR Slope rows and their
// blocks, which a non-bandpass layer keeps showing as its own rows; then
// the per-target block field rule on whatever remains.  The editor's
// callback composes this with the output-node layer rule, which reads
// editor state and stays in the editor TU.
bool Sculptor::osc_node_slot_hidden(const Graph& graph, const uint32_t node_idx, const uint32_t slot_idx)
{
    const Synth::FilterType filter_type = osc_node_filter_type(graph, node_idx);
    const bool              legacy_row_group =
        (slot_idx >= osc_filter_cutoff_slot && slot_idx < osc_filter_cutoff_slot + osc_target_block_stride + 1u) ||
        (slot_idx >= osc_filter_slope_slot && slot_idx < osc_filter_slope_slot + osc_target_block_stride + 1u);
    // Under bandpass the legacy rows are band 0's rows: they show like
    // every other always-visible band row.  Only a non-bandpass layer's
    // legacy rows skip the band check.
    if (filter_type == Synth::fir_bandpass || ! legacy_row_group) {
        if (osc_band_slot_hidden(graph, node_idx, slot_idx)) {
            return true;
        }
    }

    uint32_t target = 0;
    uint32_t field  = 0;
    if (! osc_target_block_field(slot_idx, &target, &field)) {
        return false;
    }

    return osc_target_field_hidden(graph, node_idx, target, field);
}

// The gray add dot's slot on the Oscillator Output node: the slot after
// the run.  The visibility predicate, the projection's drop flags and the
// gray-dot color seam share this one slot.
uint32_t Sculptor::osc_row_gray_slot(const uint8_t source_row[Synth::max_layers])
{
    return osc_row_run_length(source_row);
}

uint32_t Sculptor::osc_lowest_free_slot(const uint8_t source_row[Synth::max_layers])
{
    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        if (source_row[slot] == 0) {
            return slot;
        }
    }

    return pool_no_slot;
}

bool Sculptor::osc_layer_slot_hidden(uint32_t slot_idx, const uint8_t source_row[Synth::max_layers])
{
    // The visible rows extend to the gray add slot (the slot after the
    // run); the run's rows stay visible (wired slots and red holes alike)
    // and every row above the gray slot hides.  The caller resolves the
    // output node's layer rows; other slot kinds never reach this.
    return slot_idx < Synth::max_layers && slot_idx > Sculptor::osc_row_gray_slot(source_row);
}

void Sculptor::env_target_usage(const Graph&           graph,
                                const OscGraphMapping& mapping,
                                uint16_t               desc_id,
                                bool*                  volume_used,
                                bool*                  other_used,
                                uint32_t               exclude_connection)
{
    *volume_used = false;
    *other_used  = false;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        const uint32_t node = mapping.osc_nodes[layer];
        if (node == pool_no_slot || ! graph.node_occupied(node)) {
            continue;
        }
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const uint32_t env_conn = connection_into(graph, node, osc_target_env_dot(t));
            if (env_conn == pool_no_slot || env_conn == exclude_connection) {
                continue;
            }
            const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
            if (inst < 0 || mapping.detached[inst].kind != 1 || mapping.detached[inst].desc_id != desc_id) {
                continue;
            }
            if (static_cast<Synth::ModTarget>(t) == Synth::mod_volume) {
                *volume_used = true;
            }
            else {
                *other_used = true;
            }
        }
    }
    // Descriptor ids live in a bank-wide pool, so users in other instruments
    // and zones count too; the projection records the bank it read.  The
    // projected instrument is skipped: the graph scan above is authoritative
    // for it (it reflects pending edits, which the committed bank alone
    // cannot attribute).
    if (mapping.source_bank != nullptr) {
        uint32_t self_instrument = Synth::max_instruments;
        if (mapping.channel < Synth::max_channels && mapping.zone < Synth::max_instr_per_channel) {
            const Synth::Zone& zone_entry = mapping.source_bank->channel_zones[mapping.channel][mapping.zone];
            // Only a valid zone entry identifies the projected instrument; a
            // bare bank (no zone table) has none to skip.
            if (zone_entry.start_note != 0 && zone_entry.instrument < mapping.source_bank->instruments.num_allocated) {
                self_instrument = zone_entry.instrument;
            }
        }
        for (uint32_t instr_idx = 0; instr_idx < mapping.source_bank->instruments.num_allocated; ++instr_idx) {
            if (instr_idx == self_instrument) {
                continue;
            }
            const Synth::Instrument& instrument = mapping.source_bank->instruments.entries[instr_idx];
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                    if (instrument.layers[layer].targets[t].envelope_desc_id != desc_id) {
                        continue;
                    }
                    if (static_cast<Synth::ModTarget>(t) == Synth::mod_volume) {
                        *volume_used = true;
                    }
                    else {
                        *other_used = true;
                    }
                }
            }
        }
    }
}

Sculptor::OscTargetView Sculptor::osc_target_view(Synth::ModTarget target, Synth::FilterType fir_type)
{
    return target_view_for(target, fir_type);
}

bool Sculptor::osc_change_restructures(const Sculptor::OscGraphMapping& mapping, const Sculptor::GraphChange& change)
{
    if (change.kind != Sculptor::ChangeKind::value_changed || node_role(mapping, change.node_idx) != role_osc) {
        return false;
    }
    if (change.slot_idx == osc_fir_type_prop || change.slot_idx == osc_waveform_a_prop) {
        return true; // wave a retargets the shared duty/pad tone rows to damping and back
    }
    // A band [-]/[+] click rewrites the stored bands (insert after band N
    // as a copy, or remove band N with later bands shifting up), so the
    // graph rebuilds from the model like any structural edit.
    return Sculptor::is_band_button_slot(change.slot_idx);
}

// Slot index on an oscillator node holding the value row of ModTarget
// `target` (0..9); rows are not contiguous.
uint32_t Sculptor::osc_target_row(uint32_t target)
{
    return osc_target_prop(static_cast<Synth::ModTarget>(target));
}

// Decodes an oscillator-node block slot into its (target, field): false
// when the slot is one of the fixed slots (a property or a target value
// row) or past the last block.
bool Sculptor::osc_target_block_field(uint32_t slot_idx, uint32_t* target, uint32_t* field)
{
    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const uint32_t base = osc_target_block_base(t);
        if (slot_idx >= base && slot_idx < base + osc_target_block_stride) {
            *target = t;
            *field  = slot_idx - base;
            return true;
        }
    }

    return false;
}

// Index of the zone's layout record with the given key, or -1.
int32_t Sculptor::find_record(const Synth::InstrumentEditorBank& bank,
                              uint32_t                           channel,
                              uint32_t                           zone,
                              uint32_t                           kind,
                              uint32_t                           index,
                              uint8_t                            uid)
{
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == kind && record.index == index &&
            record.uid == uid) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool Sculptor::project_instrument_to_graph(const Synth::Instrument&     instrument,
                                           const Synth::InstrumentBank& bank,
                                           Graph*                       graph,
                                           OscGraphMapping*             mapping,
                                           const uint16_t*              added_cc_mask,
                                           const uint8_t*               osc_source)
{
    // The graph cannot express a sourceless routing input: dropping it would
    // change synthesis on commit (the eval multiplies by the zero sentinel),
    // so refuse visibly instead of compacting silently.  Checked before any
    // state is touched; the refusal is reported through the error overlay.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            const Synth::TargetBinding& binding = instrument.layers[layer].targets[t];
            const uint32_t              num_inputs =
                binding.num_inputs <= Synth::max_mod_inputs ? binding.num_inputs : Synth::max_mod_inputs;
            for (uint32_t i = 0; i < num_inputs; ++i) {
                if (binding.inputs[i].source == Synth::ModSource::none) {
                    graph->set_error("Cannot project: sourceless modulation input");
                    return false;
                }
            }
        }
    }
    clear_mapping(mapping);
    mapping->source_bank = &bank;
    graph->clear();

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;

    // MIDI Inputs starts low enough that the Instrument Parameters node
    // fits right above it, so both are inside the default view.
    const uint32_t input_node = graph->create_node("MIDI Inputs", vmath::vec2(0.0f, 128.0f));
    if (input_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->input_node = input_node;
    // Display order groups performance inputs first; the mapping stays
    // indexed by ModSource, so wire semantics are unaffected.
    constexpr Synth::ModSource input_display_order[Sculptor::num_osc_graph_inputs] = {
        Synth::ModSource::velocity,         Synth::ModSource::aftertouch, Synth::ModSource::channel_pressure,
        Synth::ModSource::pressure_combine, Synth::ModSource::pitch_bend, Synth::ModSource::mod_wheel,
    };
    for (uint32_t i = 0; i < num_osc_graph_inputs; ++i) {
        const uint32_t source                   = static_cast<uint32_t>(input_display_order[i]);
        mapping->input_source_slots[source - 1] = graph->add_slot(
            input_node,
            with_tooltip(output_slot(mod_source_descs[source - 1].name), mod_source_descs[source - 1].tooltip));
    }
    if (added_cc_mask) {
        for (uint32_t i = 0; i < Synth::num_assignable_ccs; ++i) {
            if (! (*added_cc_mask & (1u << i))) {
                continue;
            }
            char label[32];
            Sculptor::fx_cc_pair_label(Synth::cc_msb_base + i, label, sizeof(label));
            mapping->input_cc_slots[i] =
                graph->add_slot(input_node,
                                with_tooltip(output_slot(label), "MIDI continuous controller value (0..1)"));
        }
    }
    // The note/layer detune and release rows live on a dedicated node
    // right above MIDI Inputs (same X), so they survive CC-dot changes
    // and stay editable as one group.  The offset clears the node's own
    // rendered height (three rows); saved layout records override it.
    const uint32_t instrument_node = graph->create_node("Instrument Parameters", vmath::vec2(0.0f, 0.0f));
    if (instrument_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->instrument_node = instrument_node;
    // Random pitch offset drawn once per note for every layer, in semitones.
    mapping->instrument_slots[0] = graph->add_slot(
        instrument_node,
        with_tooltip(bounded_real_slot("Random Note Detune", instrument.note_skew_semitones, 0.0f, 1.0f),
                     "Max random pitch offset per note, shared by all oscillators (in semitones)"));
    // Independent random pitch offset drawn per layer per note, in semitones.
    mapping->instrument_slots[1] = graph->add_slot(
        instrument_node,
        with_tooltip(bounded_real_slot("Random Oscillator Detune", instrument.layer_skew_semitones, 0.0f, 1.0f),
                     "Max random pitch offset per note, separate for each oscillator (in semitones)"));
    mapping->instrument_slots[2] =
        graph->add_slot(instrument_node,
                        with_tooltip(bounded_real_slot("Release (s)", instrument.release_seconds, 0.0f, 30.0f),
                                     "Sustain volume decay time in seconds (0 = no decay)"));

    // One node per editor layer, in order: mapping.osc_nodes[o] names node
    // o, whether or not a slot names it.  The source row (slot -> editor
    // node) decides the wiring; an unwired layer projects as a normal node
    // with no output wire, keeping its parameters and kind-0 record
    // position across re-projections.  A null osc_source means identity
    // wiring (slot s fed by editor node s), which matches a freshly
    // authored instrument with no disconnects.
    uint8_t identity_row[Synth::max_layers] = {};
    if (osc_source == nullptr) {
        for (uint32_t slot = 0; slot < Synth::max_layers && slot < layer_count; ++slot) {
            identity_row[slot] = static_cast<uint8_t>(slot + 1);
        }
        osc_source = identity_row;
    }
    const uint32_t output_node = graph->create_node("Oscillator Output", vmath::vec2(1024.0f, 128.0f));
    if (output_node == Sculptor::pool_no_slot) {
        return false;
    }
    mapping->output_node = output_node;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        char name[32];
        snprintf(name, sizeof(name), "Layer %u", layer + 1);
        Sculptor::Slot slot = input_slot(name);
        // The drop-menu anchors: the gray add slot after the run and the
        // red holes (unwired slots) below it offer their dots; every
        // other slot offers nothing.
        slot.drop_menu =
            layer == Sculptor::osc_row_run_length(osc_source) || Sculptor::osc_row_is_red(osc_source, layer);
        mapping->output_layer_input_slot[layer] = graph->add_slot(output_node, slot);
    }

    const Synth::Oscillator* rows[Synth::max_layers] = {};
    for (uint32_t o = 0; o < layer_count; ++o) {
        rows[o] = &instrument.layers[o];
        project_osc_node(graph, o, instrument.layers[o], mapping);
        if (mapping->osc_nodes[o] == Sculptor::pool_no_slot) {
            return false;
        }
    }

    // Deterministic derivation: one key per distinct (kind, descriptor,
    // sources) tuple over the editor layers (unwired layers included, so
    // their bindings re-derive on re-projection), then the envelope/LFO
    // instance nodes.
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_row_instance_keys(rows, layer_count, keys);
    uint32_t       env_seq   = 0;
    uint32_t       lfo_seq   = 0;
    for (uint32_t k = 0; k < key_count; ++k) {
        const InstanceKey& key           = keys[k];
        const bool         is_env        = key.kind == 1;
        const uint32_t     num_allocated = is_env ? bank.envelopes.num_allocated : bank.lfos.num_allocated;
        // A stale descriptor id projects no node; compile drops the binding
        // (the field is dormant - the runtime gates on valid ids too).
        if (key.desc_id == 0 || key.desc_id > num_allocated) {
            continue;
        }
        char     name[32];
        uint32_t ordinal;
        if (is_env) {
            ordinal = ++env_seq;
            snprintf(name, sizeof(name), "Envelope %u", ordinal);
        }
        else {
            ordinal = ++lfo_seq;
            snprintf(name, sizeof(name), "LFO %u", ordinal);
        }
        const float    y = 128.0f * static_cast<float>(ordinal);
        const uint32_t node =
            is_env ? create_env_node(graph, name, vmath::vec2(1024.0f, y), bank.envelopes.entries[key.desc_id - 1])
                   : create_lfo_node(graph, name, vmath::vec2(1536.0f, y), bank.lfos.entries[key.desc_id - 1]);
        if (node == Sculptor::pool_no_slot) {
            return false;
        }
        if (mapping->detached_count >= max_osc_instances) {
            graph->set_error("Cannot project: too many generator instances");
            return false;
        }
        DetachedNode& entry = mapping->detached[mapping->detached_count++];
        entry.node_idx      = node;
        entry.kind          = key.kind;
        entry.desc_id       = key.desc_id;
        entry.uid           = 0;
        entry.depth_source  = key.depth_source;
        entry.rate_source   = key.rate_source;
    }

    for (uint32_t row = 0; row < layer_count; ++row) {
        const uint32_t osc_node   = mapping->osc_nodes[row];
        const uint32_t band_count = osc_band_model_count(*rows[row]);
        for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
            // Only existing bands' blocks are projected: a binding riding a
            // band beyond the count (hand-built state) gets no wire, so it
            // cannot land on a slot that does not exist.
            const int32_t band = osc_target_band_index(t);
            if ((band >= 0 && static_cast<uint32_t>(band) >= band_count) || ! osc_target_projected(*rows[row], t)) {
                continue;
            }
            const Synth::TargetBinding& gen = rows[row]->targets[t];
            if (gen.envelope_desc_id != 0) {
                uint32_t best = Sculptor::pool_no_slot;
                for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                    if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 1 &&
                        mapping->detached[i].desc_id == gen.envelope_desc_id) {
                        best = i;
                    }
                }
                if (best != Sculptor::pool_no_slot &&
                    graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->env_output_slot },
                                          EndPoint{ osc_node, osc_target_env_dot(t) }) == Sculptor::pool_no_slot) {
                    // A dropped wire would silently compile the cell base-only
                    // over the bank's stored modulators.
                    return false;
                }
            }
            if (gen.lfo_desc_id != 0) {
                uint32_t best = Sculptor::pool_no_slot;
                for (uint32_t i = 0; i < mapping->detached_count; ++i) {
                    if (best == Sculptor::pool_no_slot && mapping->detached[i].kind == 2 &&
                        mapping->detached[i].desc_id == gen.lfo_desc_id &&
                        mapping->detached[i].depth_source == static_cast<uint8_t>(gen.lfo_depth_source) &&
                        mapping->detached[i].rate_source == static_cast<uint8_t>(gen.lfo_rate_source)) {
                        best = i;
                    }
                }
                if (best != Sculptor::pool_no_slot &&
                    graph->add_connection(EndPoint{ mapping->detached[best].node_idx, mapping->lfo_output_slot },
                                          EndPoint{ osc_node, osc_target_lfo_dot(t) }) == Sculptor::pool_no_slot) {
                    return false;
                }
            }
            if (gen.num_inputs != 0 &&
                ! wire_target_sources(graph, *mapping, osc_node, t, gen.inputs, gen.num_inputs)) {
                return false;
            }
        }
    }

    // The LFO instances' depth/rate inputs keep their MIDI-source wires.
    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const DetachedNode& entry = mapping->detached[i];
        if (entry.kind != 2) {
            continue;
        }
        if (entry.depth_source != 0 &&
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.depth_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_depth_input_slot }) ==
                Sculptor::pool_no_slot) {
            return false;
        }
        if (entry.rate_source != 0 &&
            graph->add_connection(EndPoint{ mapping->input_node, mapping->input_source_slots[entry.rate_source - 1] },
                                  EndPoint{ entry.node_idx, mapping->lfo_rate_input_slot }) == Sculptor::pool_no_slot) {
            return false;
        }
    }

    // The source row decides the osc -> sum wiring: slot s takes the node
    // it names; unwired slots stay open (the red holes and the gray add
    // slot).
    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        if (osc_source[slot] == 0) {
            continue;
        }
        const uint32_t o = osc_source[slot] - 1;
        if (o >= layer_count || mapping->osc_nodes[o] == Sculptor::pool_no_slot) {
            continue; // the row names no projected node; the slot stays open
        }
        if (graph->add_connection(EndPoint{ mapping->osc_nodes[o], mapping->osc_output_slot },
                                  EndPoint{ mapping->output_node, mapping->output_layer_input_slot[slot] }) ==
            Sculptor::pool_no_slot) {
            return false;
        }
    }

    return true;
}

// Reads one oscillator layer back from its projected node's slots: the
// inverse of project_osc_node's slot layout.  The compile reads one layer
// per editor node.
static bool read_oscillator_layer(const Sculptor::Graph&           graph,
                                  const Sculptor::OscGraphMapping& mapping,
                                  uint32_t                         layer,
                                  Synth::Oscillator*               osc_out)
{
    const Sculptor::Node& node = graph.node(mapping.osc_nodes[layer]);
    if (node.slots.num_allocated < osc_slot_count) {
        return false;
    }

    const Sculptor::Slot* slots = node.slots.entries;
    Synth::Oscillator&    osc   = *osc_out;

    uint32_t index = 0;
    if (! read_list_prop(slots[osc_waveform_a_prop], &index)) {
        return false;
    }
    osc.osc_type[0] = static_cast<Synth::WaveType>(index + 1);
    if (! read_list_prop(slots[osc_waveform_b_prop], &index)) {
        return false;
    }
    osc.osc_type[1] = static_cast<Synth::WaveType>(index);
    if (! read_list_prop(slots[osc_mode_prop], &index)) {
        return false;
    }
    osc.osc_mode         = static_cast<Synth::OscMode>(index);
    const int32_t octave = slots[osc_octave_offset_prop].value.integer;
    if (octave < Sculptor::osc_octave_offset_min || octave > Sculptor::osc_octave_offset_max) {
        return false;
    }
    osc.octave_offset   = static_cast<int8_t>(octave);
    const int32_t slope = slots[osc_fir_slope_prop].value.integer;
    // The FIR Slope row displays the effective slope: the routed base when the slope binding is
    // wired, the fir_slope scalar otherwise; both are integer percents 1..100.  An unwired
    // out-of-range row is a corrupt scalar and refuses like the octave guard; a wired row supplies
    // the binding's base and also refreshes the dormant scalar.
    const bool slope_wired =
        connection_into(graph,
                        mapping.osc_nodes[layer],
                        osc_target_env_dot(static_cast<uint32_t>(Synth::mod_fir_slope))) != pool_no_slot ||
        connection_into(graph,
                        mapping.osc_nodes[layer],
                        osc_target_lfo_dot(static_cast<uint32_t>(Synth::mod_fir_slope))) != pool_no_slot ||
        connection_into(graph,
                        mapping.osc_nodes[layer],
                        osc_target_src_dot(static_cast<uint32_t>(Synth::mod_fir_slope), 0)) != pool_no_slot ||
        connection_into(graph,
                        mapping.osc_nodes[layer],
                        osc_target_src_dot(static_cast<uint32_t>(Synth::mod_fir_slope), 1)) != pool_no_slot;
    if (! slope_wired && (slope < 1 || slope > 100)) {
        return false;
    }
    osc.fir_slope_percent = static_cast<uint8_t>(slope < 1 ? 1 : slope > 100 ? 100 : slope);

    if (! read_list_prop(slots[osc_fir_type_prop], &index)) {
        return false;
    }
    osc.fir_type                      = static_cast<Synth::FilterType>(index);
    osc.string_pick                   = slots[osc_pick_slot].value.real;
    osc.string_brightness             = slots[osc_brightness_slot].value.real;
    const int32_t dispersion_sections = slots[osc_dispersion_sections_slot].value.integer;
    if (dispersion_sections < 1 || dispersion_sections > 6) {
        return false;
    }
    osc.string_dispersion          = slots[osc_dispersion_slot].value.real;
    osc.string_dispersion_sections = static_cast<float>(dispersion_sections);

    // The projected band blocks encode the count: one uniform block per
    // existing band after Filter Type.
    if (node.slots.num_allocated < osc_band_first_slot + osc_band_block_stride) {
        return false;
    }
    const uint32_t band_count = (node.slots.num_allocated - osc_band_first_slot) / osc_band_block_stride;
    osc.fir_band_count        = static_cast<uint8_t>(band_count < 1 ? 1 : band_count > 4 ? 4 : band_count);

    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        const Synth::ModTarget target = static_cast<Synth::ModTarget>(t);
        // Only existing bands' rows are projected: a band beyond the count
        // contributes nothing, so its binding compiles as zeroed.
        const int32_t band = Sculptor::osc_target_band_index(t);
        if ((band >= 0 && static_cast<uint32_t>(band) >= band_count) || ! osc_target_projected(osc, t)) {
            continue;
        }
        // Each layer owns its cell: the base value comes from the layer's
        // own row (display units scaled to bank units), except the FIR
        // slope, whose wired base is the integer-percent row itself and
        // whose unwired binding stays zeroed; the block rows wired to a
        // target contribute the modulators only.  A family dot with no
        // wire leaves the family's fields dormant (they compile to
        // zero), so rewiring rebinds without re-projection.
        Synth::TargetBinding gen = {};
        if (target == Synth::mod_fir_slope) {
            // While wired, the row is the binding's base: the routed base is the row's integer
            // percent, already clamped into fir_slope_percent above.  An unwired layer keeps its
            // zeroed binding; the scalar refreshed above carries the value.
            if (slope_wired) {
                gen.base_value = osc.fir_slope_percent;
            }
        }
        else {
            // The per-band slope rows are integer-percent slots like the
            // legacy FIR slope row, so their base reads from the integer
            // property instead of the real union field.
            gen.base_value = target == Synth::mod_fir_band1_slope || target == Synth::mod_fir_band2_slope ||
                                     target == Synth::mod_fir_band3_slope
                                 ? static_cast<float>(slots[osc_target_prop(target)].value.integer)
                                 : slots[osc_target_prop(target)].value.real * osc_target_views[t].bank_scale;
        }
        if (target == Synth::mod_fir_spread) {
            gen.base_value = fir_spread_bank_value(osc.fir_type, gen.base_value);
        }
        // A connection still ending on the base target row is a
        // validator/apply invariant violation: the apply path relocates
        // every committed wire onto its family dot before this runs.
        if (connection_into(graph, mapping.osc_nodes[layer], osc_target_prop(target)) != pool_no_slot) {
            return false;
        }
        const uint32_t env_conn = connection_into(graph, mapping.osc_nodes[layer], osc_target_env_dot(t));
        // The depth slot mirrors the binding whether or not an envelope is
        // wired, so the projection round trip is bit-identical even for the
        // dormant field.
        gen.envelope_depth = slots[osc_target_env_depth(t)].value.real;
        if (env_conn != pool_no_slot) {
            const int32_t inst = find_instance(mapping, graph.get_connection(env_conn).output.node_idx);
            if (inst < 0 || mapping.detached[inst].kind != 1) {
                return false;
            }
            gen.envelope_desc_id = mapping.detached[inst].desc_id;
        }
        const uint32_t lfo_conn = connection_into(graph, mapping.osc_nodes[layer], osc_target_lfo_dot(t));
        if (lfo_conn != pool_no_slot) {
            const int32_t inst = find_instance(mapping, graph.get_connection(lfo_conn).output.node_idx);
            if (inst < 0 || mapping.detached[inst].kind != 2) {
                return false;
            }
            gen.lfo_desc_id = mapping.detached[inst].desc_id;
            // Depth/rate sources resolve from the edges into the bound
            // LFO instance; no edge means the source stays none.
            const uint32_t depth_conn =
                connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_depth_input_slot);
            if (depth_conn != pool_no_slot && ! edge_source(graph, mapping, depth_conn, &gen.lfo_depth_source)) {
                return false;
            }
            const uint32_t rate_conn =
                connection_into(graph, mapping.detached[inst].node_idx, mapping.lfo_rate_input_slot);
            if (rate_conn != pool_no_slot && ! edge_source(graph, mapping, rate_conn, &gen.lfo_rate_source)) {
                return false;
            }
            uint32_t lfo_index = 0;
            if (! read_list_prop(slots[osc_target_lfo_dot(t) + 1], &lfo_index)) {
                return false;
            }
            gen.lfo_op            = static_cast<Synth::SourceOp>(lfo_index);
            gen.lfo_depth         = slots[osc_target_lfo_dot(t) + 2].value.real;
            gen.lfo_rate_scale_ms = slots[osc_target_lfo_dot(t) + 3].value.real;
        }
        uint32_t num_inputs = 0;
        for (uint32_t i = 0; i < Synth::max_mod_inputs; ++i) {
            const uint32_t dot  = osc_target_src_dot(t, i);
            const uint32_t conn = connection_into(graph, mapping.osc_nodes[layer], dot);
            if (conn == pool_no_slot) {
                continue;
            }
            const int32_t    cc_index = edge_cc_index(graph, mapping, conn);
            Synth::ModSource source   = Synth::ModSource::none;
            if (cc_index < 0 && ! edge_source(graph, mapping, conn, &source)) {
                return false;
            }
            if (node.slots.num_allocated <= dot + 2) {
                return false;
            }
            uint32_t op_index = 0;
            if (! read_list_prop(slots[dot + 1], &op_index)) {
                return false;
            }
            Synth::ModInput& input = gen.inputs[num_inputs++];
            if (cc_index >= 0) {
                // A CC wire compiles to the assignable cc source; the
                // validate_instrument 16..31 rule protects the runtime
                // resolve_source slot assert.
                input.source    = Synth::ModSource::cc;
                input.cc_number = static_cast<uint8_t>(Synth::cc_msb_base + static_cast<uint32_t>(cc_index));
            }
            else {
                input.source = source;
            }
            input.op    = static_cast<Synth::SourceOp>(op_index);
            input.scale = slots[dot + 2].value.real;
        }
        gen.num_inputs = static_cast<uint16_t>(num_inputs);
        osc.targets[t] = gen;
    }
    osc.mod_ratio = osc.targets[Synth::mod_fm_ratio].base_value;
    return true;
}

bool Sculptor::compile_graph_to_instrument(const Graph&             graph,
                                           const OscGraphMapping&   mapping,
                                           Synth::Instrument*       out,
                                           uint8_t*                 osc_source_out,
                                           const Synth::Instrument* previous)
{
    *out = Synth::Instrument{};

    // The editor nodes run consecutively from ordinal 0; the run's length
    // is the editor layer count.
    uint32_t layer_count = 0;
    while (layer_count < Synth::max_layers && mapping.osc_nodes[layer_count] != pool_no_slot) {
        ++layer_count;
    }
    for (uint32_t layer = layer_count; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != pool_no_slot) {
            // gap in the oscillator node run
            return false;
        }
    }
    if (mapping.output_node == pool_no_slot) {
        return false;
    }

    // The authoring form keeps every editor layer in ordinal order -
    // unwired layers included, so their parameters persist.  The source
    // row, derived from the wiring below, decides which layers play.
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        if (! read_oscillator_layer(graph, mapping, layer, &out->layers[layer])) {
            return false;
        }
        if (previous != nullptr) {
            // Bands beyond the projected count have no graph rows: their
            // bindings stay as stored (the band edits zero them on
            // removal), so a re-projection round trip is bit-identical.
            const uint32_t count = osc_band_model_count(out->layers[layer]);
            for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                if (osc_target_band_index(t) >= static_cast<int32_t>(count) ||
                    ! osc_target_projected(out->layers[layer], t)) {
                    out->layers[layer].targets[t] = previous->layers[layer].targets[t];
                }
            }
        }
    }
    out->layer_count = layer_count;

    // Derive the source row from the wiring: slot s takes the ordinal of
    // the oscillator feeding its input slot; an open slot stays 0.  Two
    // wires into one slot mean a hand-built graph: refuse instead of
    // picking one silently.  A wire from a node outside the oscillator
    // run leaves the slot 0; the validator refuses those sources.
    uint8_t derived[Synth::max_layers] = {};
    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        const uint32_t in_slot = mapping.output_layer_input_slot[slot];
        if (in_slot == pool_no_slot) {
            return false;
        }
        uint32_t source = pool_no_slot;
        for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
            if (! graph.connection_occupied(c)) {
                continue;
            }
            const Sculptor::Connection& conn = graph.get_connection(c);
            if (conn.input.node_idx != mapping.output_node || conn.input.slot_idx != in_slot) {
                continue;
            }
            if (source != pool_no_slot) {
                return false; // two wires into one sum input slot
            }
            source = conn.output.node_idx;
        }
        for (uint32_t o = 0; o < layer_count; ++o) {
            if (mapping.osc_nodes[o] == source) {
                derived[slot] = static_cast<uint8_t>(o + 1);
                break;
            }
        }
    }
    // A repeated ordinal is accepted: both slots name the same layer, and
    // the compile copies that layer into every wired slot in slot order.
    if (osc_source_out != nullptr) {
        memcpy(osc_source_out, derived, sizeof(derived));
    }

    // Instrument-parameter rows live on the dedicated node; direct
    // project_instrument_to_graph callers (the unit tests) always project
    // it, so a missing node means a hand-built graph: parameters stay at
    // the Instrument{} zero-init instead of a slot read.
    if (mapping.instrument_node != pool_no_slot) {
        const Node& params = graph.node(mapping.instrument_node);
        if (params.slots.num_allocated < 3) {
            return false;
        }
        for (uint32_t i = 0; i < 3; ++i) {
            if (mapping.instrument_slots[i] == pool_no_slot) {
                return false;
            }
        }
        out->note_skew_semitones  = params.slots.entries[mapping.instrument_slots[0]].value.real;
        out->layer_skew_semitones = params.slots.entries[mapping.instrument_slots[1]].value.real;
        out->release_seconds      = params.slots.entries[mapping.instrument_slots[2]].value.real;
    }

    // Invariant: every wire into an oscillator lands on a block family
    // dot.  A connection still ending on a base target row means a
    // headless caller bypassed the relocation, and compiling it would
    // silently drop the wire.
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] == pool_no_slot) {
            continue;
        }
        for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
            if (! graph.connection_occupied(c)) {
                continue;
            }
            const Sculptor::Connection& conn = graph.get_connection(c);
            if (conn.input.node_idx == mapping.osc_nodes[layer] && osc_row_is_target(conn.input.slot_idx)) {
                return false;
            }
        }
    }

    return true;
}

bool Sculptor::compile_playback_instrument(const Synth::Instrument& editor_instr,
                                           const uint8_t            source_row[Synth::max_layers],
                                           Synth::Instrument*       out)
{
    if (! out) {
        return false;
    }

    // The playback form holds the wired layers in slot order: slot 0's
    // layer first and so on.  Unwired slots drop, and an all-zero row
    // compiles to a silent, valid instrument with layer_count 0.
    *out           = editor_instr;
    uint32_t wired = 0;

    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        if (source_row[slot] == 0) {
            continue;
        }
        const uint32_t ordinal = source_row[slot] - 1;
        if (ordinal >= Synth::max_layers || ordinal >= editor_instr.layer_count) {
            return false; // the row names no editor layer
        }
        out->layers[wired++] = editor_instr.layers[ordinal];
    }

    for (uint32_t l = wired; l < Synth::max_layers; ++l) {
        out->layers[l] = Synth::Oscillator{};
    }

    out->layer_count = wired;
    return true;
}

void Sculptor::set_identity_osc_sources(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (! bank || channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }

    uint8_t* source = bank->graph_osc_source[channel][zone];
    memset(source, 0, Synth::max_layers);

    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return;
    }

    const uint32_t layer_count = bank->bank.instruments.entries[zone_entry.instrument].layer_count;
    for (uint32_t slot = 0; slot < Synth::max_layers && slot < layer_count; ++slot) {
        source[slot] = static_cast<uint8_t>(slot + 1);
    }
}

bool Sculptor::osc_graph_validate(void* user_data, Graph& graph, EndPoint output, EndPoint input)
{
    const OscGraphMapping* mapping = static_cast<const OscGraphMapping*>(user_data);
    if (! mapping) {
        return false;
    }

    switch (classify_output(*mapping, output)) {
        case out_env: {
            // An envelope wire lands on a target block's envelope dot: the
            // base value row resolves the dot in the apply path, a direct
            // dot drop names it here.  One envelope per (layer, target):
            // the destination dot must be free.  A volume envelope
            // (minimum 0, nonnegative span) serves only the volume
            // target, and only a volume envelope serves the volume target:
            // the descriptor is shared by every target that wires it, and
            // the two roles want different ranges.
            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_env) {
                return false;
            }
            const uint32_t env_dot = osc_target_env_dot(target);
            if (dot_taken_for_move(graph, input.node_idx, env_dot)) {
                graph.set_error("That envelope row already carries a wire");
                return false;
            }
            const int32_t inst = find_instance(*mapping, output.node_idx);
            if (inst < 0 || mapping->detached[inst].kind != 1) {
                return false;
            }
            bool volume_used = false;
            bool other_used  = false;
            env_target_usage(graph,
                             *mapping,
                             mapping->detached[inst].desc_id,
                             &volume_used,
                             &other_used,
                             graph.moving_connection);
            if (target == static_cast<uint32_t>(Synth::mod_volume) && other_used) {
                graph.set_error("Envelope already serves a non-volume target");
                return false;
            }
            if (target != static_cast<uint32_t>(Synth::mod_volume) && volume_used) {
                graph.set_error("Envelope already serves the volume target");
                return false;
            }
            return true;
        }
        case out_lfo: {
            // One LFO per (layer, target): the block has one LFO dot, so a
            // fresh wire needs it free.
            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_lfo) {
                return false;
            }
            const uint32_t lfo_dot = osc_target_lfo_dot(target);
            if (dot_taken_for_move(graph, input.node_idx, lfo_dot)) {
                graph.set_error("That LFO row already carries a wire");
                return false;
            }
            return true;
        }
        case out_input: {
            // A CC dot is a source-role output: the runtime resolves cc for
            // routing inputs only, so LFO depth/rate refuse it
            // (validate_instrument refuses cc there too), and envelope/LFO
            // dots refuse every source-role output.
            const bool cc_dot = input_cc_index(*mapping, output.slot_idx) >= 0;

            // LFO depth/rate inputs accept standard MIDI source outputs
            // (the runtime resolves cc for routing inputs only, so CC
            // dots refuse there; validate_instrument refuses cc too).
            const InputKind input_kind = classify_input(*mapping, input);
            if (input_kind == in_depth || input_kind == in_rate) {
                if (cc_dot) {
                    graph.set_error("CC wires into the Source rows only");
                    return false;
                }
                return true;
            }

            bool     base_row = false;
            uint32_t target   = 0;
            uint32_t family   = 0;
            uint32_t dot      = pool_no_slot;
            if (! decode_osc_input(*mapping, input, &base_row, &target, &family, &dot)) {
                return false;
            }
            if (! base_row && family != family_src) {
                return false;
            }
            if (family == family_src) {
                // A direct drop names its dot; a base-row drop takes the
                // first free Source dot in the apply path, so one free dot
                // in the block suffices.
                if (dot != pool_no_slot) {
                    if (dot_taken_for_move(graph, input.node_idx, dot)) {
                        graph.set_error("That source row already carries a wire");
                        return false;
                    }
                    return true;
                }
                if (osc_target_free_input(graph, input.node_idx, target, family_src) == pool_no_slot) {
                    graph.set_error("Both source rows carry wires");
                    return false;
                }
                return true;
            }
            graph.set_error(cc_dot ? "CC wires into the Source rows only"
                                   : "MIDI sources wire into the Source rows only");
            return false;
        }
        case out_osc: {
            if (classify_input(*mapping, input) != in_sum) {
                return false;
            }
            // Any oscillator may wire into any open sum input slot; the
            // auto-move keeps one wire per oscillator.  Wiring into an
            // occupied slot is refused (the moving exception lets a
            // retarget re-place its own wire).
            bool from_osc = false;
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                if (mapping->osc_nodes[layer] == output.node_idx) {
                    from_osc = true;
                    break;
                }
            }
            if (! from_osc) {
                return false;
            }
            if (dot_taken_for_move(graph, input.node_idx, input.slot_idx)) {
                graph.set_error("That sum input already carries a wire");
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}

bool Sculptor::apply_osc_graph_descriptor_edit(const Graph&           graph,
                                               const OscGraphMapping& mapping,
                                               Synth::InstrumentBank* bank,
                                               uint32_t               node_idx,
                                               uint32_t               slot_idx,
                                               PropertyValue          value)
{
    // Generator instances resolve through the registry: their content edits
    // write through to the pool descriptor by id, aliasing included.
    const int32_t inst = find_instance(mapping, node_idx);
    if (inst < 0) {
        return false;
    }
    const uint16_t desc_id = mapping.detached[inst].desc_id;
    const bool     is_env  = mapping.detached[inst].kind == 1;
    if (desc_id == 0) {
        return false;
    }

    const Node& node = graph.node(node_idx);
    if (slot_idx >= node.slots.num_allocated) {
        return false;
    }
    const Slot& slot = node.slots.entries[slot_idx];
    if (slot.kind != SlotKind::property) {
        return false;
    }

    if (is_env) {
        if (desc_id > bank->envelopes.num_allocated) {
            return false;
        }
        Synth::EnvelopeDescriptor& env = bank->envelopes.entries[desc_id - 1];
        if (slot.property_type == PropertyType::real) {
            // The runtime evaluates the curve as min_value + raw_value *
            // min_max_delta with raw_value in 0..65535, so the slots expose
            // the effective range endpoints: the top re-derives the span
            // (max - min) / 65535, and moving the floor pins the top.
            if (strncmp(slot.name, "Value (min)", sizeof(slot.name)) == 0) {
                // A volume envelope keeps its minimum at 0; the descriptor is
                // shared, so the constraint follows its volume wiring.  The
                // refusing caller reports the refused batch.
                bool volume_used = false;
                bool other_used  = false;
                env_target_usage(graph, mapping, desc_id, &volume_used, &other_used);
                if (volume_used && value.real != 0.0f) {
                    return false;
                }
                const float max_value = env.min_value + 65535.0f * env.min_max_delta;
                env.min_value         = value.real;
                env.min_max_delta     = (max_value - env.min_value) / 65535.0f;
                return true;
            }
            if (strncmp(slot.name, "Value (max)", sizeof(slot.name)) == 0) {
                // A volume envelope keeps its effective top at or above its
                // minimum; the descriptor is shared, so the constraint follows
                // its volume wiring.
                bool volume_used = false;
                bool other_used  = false;
                env_target_usage(graph, mapping, desc_id, &volume_used, &other_used);
                if (volume_used && value.real < env.min_value) {
                    return false;
                }
                env.min_max_delta = (value.real - env.min_value) / 65535.0f;
                return true;
            }
            return false;
        }
        if (slot.property_type == PropertyType::toggle && strncmp(slot.name, "Ping-Pong", sizeof(slot.name)) == 0) {
            // Toggling a multi-point sustain span off snaps the live span end to
            // the span start immediately (same rule as decode,
            // sculptor_bank_json.cpp).  Events carry no old value, so the
            // previous flag state comes from the descriptor itself.
            if ((env.flags & Synth::envelope_ping_pong) != 0 && ! value.toggle &&
                env.sustain_last_point > env.sustain_first_point) {
                env.points[env.sustain_last_point].value = env.points[env.sustain_first_point].value;
            }
            if (value.toggle) {
                env.flags |= Synth::envelope_ping_pong;
            }
            else {
                env.flags &= ~Synth::envelope_ping_pong;
            }
            return true;
        }
        if (slot.property_type == PropertyType::toggle && strncmp(slot.name, "Quick Release", sizeof(slot.name)) == 0) {
            // Sibling of the Ping-Pong bit; no cross-field forcing, so the
            // descriptor stays symmetric with its JSON decode.
            if (value.toggle) {
                env.flags |= Synth::envelope_quick_release;
            }
            else {
                env.flags &= ~Synth::envelope_quick_release;
            }
            return true;
        }
        return false;
    }

    if (desc_id > bank->lfos.num_allocated) {
        return false;
    }
    Synth::LFODescriptor& lfo = bank->lfos.entries[desc_id - 1];
    if (slot.property_type == PropertyType::real) {
        if (! (value.real >= 0.0f && value.real <= 1.0f)) {
            return false;
        }
        if (strncmp(slot.name, "Duty", sizeof(slot.name)) == 0) {
            lfo.duty = static_cast<uint8_t>(value.real * 255.0f + 0.5f);
            return true;
        }
        if (strncmp(slot.name, "Pulse", sizeof(slot.name)) == 0) {
            lfo.pulse_mix = static_cast<uint8_t>(value.real * 255.0f + 0.5f);
            return true;
        }
        return false;
    }
    if (slot.property_type == PropertyType::list && value.list_index < slot.num_list_options) {
        if (strncmp(slot.name, "Waveform", sizeof(slot.name)) == 0) {
            // The two-option wave list keeps the descriptor inside the runtime's
            // sine/sawtooth palette.
            lfo.wave = value.list_index == 0 ? Synth::WaveType::sine_wave : Synth::WaveType::sawtooth_wave;
            return true;
        }
        if (strncmp(slot.name, "Sync", sizeof(slot.name)) == 0) {
            lfo.beat_div = static_cast<uint8_t>(value.list_index);
            return true;
        }
        if (strncmp(slot.name, "Start", sizeof(slot.name)) == 0) {
            lfo.start_point = static_cast<uint8_t>(value.list_index);
            return true;
        }
    }
    if (slot.property_type == PropertyType::integer && value.integer >= 0) {
        if (strncmp(slot.name, "Period (ms)", sizeof(slot.name)) == 0 && value.integer <= 65535) {
            lfo.period_ms = static_cast<uint16_t>(value.integer);
            return true;
        }
    }
    return false;
}

// The projected node a kind-0 record keys, or pool_no_slot when the current
// projection has no such node (shorter layer run).
static uint32_t node_at_canonical_index(const Sculptor::OscGraphMapping& mapping, uint32_t index)
{
    if (index == 0) {
        return mapping.input_node;
    }
    if (index == 1) {
        return mapping.output_node;
    }
    if (index < canonical_first_env) {
        return mapping.osc_nodes[index - canonical_first_osc];
    }
    return Sculptor::pool_no_slot;
}

static void remove_record_at(Synth::InstrumentEditorBank* bank, uint32_t idx)
{
    for (uint32_t i = idx; i + 1 < bank->graph_layout_count; ++i) {
        bank->graph_layout[i] = bank->graph_layout[i + 1];
    }
    bank->graph_layout_count--;
}

static void append_record(Synth::InstrumentEditorBank* bank, const Synth::GraphNodeLayout& record)
{
    bank->graph_layout[bank->graph_layout_count++] = record;
}

// Generator-record persistence: a record-less, unreferenced generator
// instance gains a layout record (free-standing state survives
// re-projection), and a recorded LFO's depth/rate sources follow the
// live edges so a mid-session rewire is what the record stores.
// Reference status derives from the direct target-block edges: an
// instance wired into any oscillator block dot is referenced.
static bool reconcile_generator_records(Synth::InstrumentEditorBank* bank,
                                        Sculptor::Graph*             graph,
                                        Sculptor::OscGraphMapping*   mapping,
                                        uint32_t                     channel,
                                        uint32_t                     zone)
{
    for (uint32_t i = 0; i < mapping->detached_count; ++i) {
        const Sculptor::DetachedNode& entry = mapping->detached[i];
        if (entry.node_idx == Sculptor::pool_no_slot || ! graph->node_occupied(entry.node_idx)) {
            continue;
        }
        bool referenced = false;
        for (uint32_t layer = 0; layer < Synth::max_layers && ! referenced; ++layer) {
            const uint32_t node = mapping->osc_nodes[layer];
            if (node == Sculptor::pool_no_slot || ! graph->node_occupied(node)) {
                continue;
            }
            for (uint32_t t = 0; t < Synth::num_mod_targets && ! referenced; ++t) {
                const uint32_t env_conn = connection_into(*graph, node, osc_target_env_dot(t));
                const uint32_t lfo_conn = connection_into(*graph, node, osc_target_lfo_dot(t));
                if (env_conn != Sculptor::pool_no_slot &&
                    graph->get_connection(env_conn).output.node_idx == entry.node_idx) {
                    referenced = true;
                }
                if (lfo_conn != Sculptor::pool_no_slot &&
                    graph->get_connection(lfo_conn).output.node_idx == entry.node_idx) {
                    referenced = true;
                }
            }
        }
        if (! referenced && entry.uid == 0 &&
            ! Sculptor::detach_osc_graph_instance(bank, *graph, mapping, channel, zone, i)) {
            return false;
        }
        if (entry.kind == 2 && entry.uid != 0) {
            // Free-standing LFO source records follow the live depth/rate
            // edges, so edits while free-standing survive re-projection.
            const int32_t record_idx = find_record(*bank, channel, zone, 2, entry.desc_id, entry.uid);
            if (record_idx < 0) {
                continue;
            }
            Synth::GraphNodeLayout& record     = bank->graph_layout[record_idx];
            const uint32_t          depth_conn = connection_into(*graph, entry.node_idx, mapping->lfo_depth_input_slot);
            const uint32_t          rate_conn  = connection_into(*graph, entry.node_idx, mapping->lfo_rate_input_slot);
            Synth::ModSource        source     = Synth::ModSource::none;
            record.depth_source                = 0;
            record.rate_source                 = 0;
            if (depth_conn != Sculptor::pool_no_slot) {
                if (! edge_source(*graph, *mapping, depth_conn, &source)) {
                    return false;
                }
                record.depth_source = static_cast<uint8_t>(source);
            }
            if (rate_conn != Sculptor::pool_no_slot) {
                if (! edge_source(*graph, *mapping, rate_conn, &source)) {
                    return false;
                }
                record.rate_source = static_cast<uint8_t>(source);
            }
        }
    }
    return true;
}

// True while a mapped oscillator node is freed: its layer removal is still
// pending in this drained batch (a node's wire events always drain before
// its own node_deleted), so a compile now would read the freed node's stale
// slots.  The batch's last oscillator node_deleted finds only live nodes and
// produces the final compile.
static bool osc_deletion_pending(const Sculptor::Graph& graph, const Sculptor::OscGraphMapping& mapping)
{
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping.osc_nodes[layer] != Sculptor::pool_no_slot && ! graph.node_occupied(mapping.osc_nodes[layer])) {
            return true;
        }
    }
    return false;
}

// Rewrites the zone's instrument from the graph (canonical form: everything
// the graph does not express compiles to zero).  MIDI routing inputs survive
// in the cell bindings only while the block's source rows wire them:
// compile zeroes the inputs of a block with unconnected source dots.
static bool store_compiled_instrument(Synth::InstrumentEditorBank*     bank,
                                      const Sculptor::Graph&           graph,
                                      const Sculptor::OscGraphMapping& mapping,
                                      uint32_t                         channel,
                                      uint32_t                         zone,
                                      bool                             preserve_layers = false)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }

    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return false;
    }

    if (osc_deletion_pending(graph, mapping)) {
        return true; // the pending node_deleted recompiles after the removal
    }

    Synth::Instrument compiled;
    // Compile into a local row and commit both outputs together: a late
    // compile failure (hand-built graphs only) must leave the stored row
    // and instrument untouched, the both-or-neither store contract.
    uint8_t derived_row[Synth::max_layers];

    // The stored entry donates the bindings of bands beyond the projected
    // count: those bands project no rows, so the graph cannot express
    // them (the band edits zero them on removal).
    const Synth::Instrument& stored = bank->bank.instruments.entries[zone_entry.instrument];
    if (! compile_graph_to_instrument(graph, mapping, &compiled, derived_row, &stored)) {
        return false;
    }

    memcpy(bank->graph_osc_source[channel][zone], derived_row, sizeof(derived_row));
    if (preserve_layers) {
        // A pure osc-to-sum rewire changes only the wiring: the entry's
        // authoring layers stay as stored, so a bank-side edit on any
        // layer survives an apply that merely moves a wire.  The compile
        // above still validates the graph shape and derives the row.
        return true;
    }

    bank->bank.instruments.entries[zone_entry.instrument] = compiled;
    return true;
}

// Syncs every projected instance of one LFO descriptor: wave, duty, and
// period slots are views of the shared descriptor, and Duty editability
// follows the wave list.  Value edits never reproject, so one edit
// through one node must update the sibling nodes silently.
static void sync_lfo_descriptor_views(Synth::InstrumentEditorBank*     bank,
                                      Sculptor::Graph*                 graph,
                                      const Sculptor::OscGraphMapping& mapping,
                                      uint32_t                         node_idx)
{
    uint32_t desc_id = 0;
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        if (mapping.detached[i].kind == 2 && mapping.detached[i].node_idx == node_idx) {
            desc_id = mapping.detached[i].desc_id;
        }
    }

    if (desc_id == 0 || desc_id > bank->bank.lfos.num_allocated) {
        return;
    }

    const Synth::LFODescriptor& lfo = bank->bank.lfos.entries[desc_id - 1];
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        const Sculptor::DetachedNode& entry = mapping.detached[i];
        if (entry.kind != 2 || entry.desc_id != desc_id || entry.node_idx == Sculptor::pool_no_slot) {
            continue;
        }

        const Sculptor::Node& node = graph->node(entry.node_idx);
        for (uint32_t slot = 0; slot < node.slots.num_allocated; ++slot) {
            const Sculptor::Slot&   slot_ref = node.slots.entries[slot];
            Sculptor::PropertyValue value    = {};
            if (strncmp(slot_ref.name, "Waveform", sizeof(slot_ref.name)) == 0) {
                value.list_index = lfo.wave == Synth::WaveType::sine_wave ? 0 : 1;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Duty", sizeof(slot_ref.name)) == 0) {
                value.real = static_cast<float>(lfo.duty) / 255.0f;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Period (ms)", sizeof(slot_ref.name)) == 0) {
                value.integer = lfo.period_ms;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Pulse", sizeof(slot_ref.name)) == 0) {
                value.real = static_cast<float>(lfo.pulse_mix) / 255.0f;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Sync", sizeof(slot_ref.name)) == 0) {
                value.list_index = lfo.beat_div <= 5 ? lfo.beat_div : 0;
                graph->set_slot_value(entry.node_idx, slot, value);
            }

            if (strncmp(slot_ref.name, "Start", sizeof(slot_ref.name)) == 0) {
                value.list_index = lfo.start_point <= 2 ? lfo.start_point : 0;
                graph->set_slot_value(entry.node_idx, slot, value);
            }
        }

        refresh_lfo_edit_disabled(*graph, entry.node_idx);
    }
}

// Copies band `from`'s four bindings into band `to`.  Band 0's unrouted
// slope binding keeps the zeroed sentinel while the layer scalar carries
// its displayed value, so a copy out of band 0 seeds the destination's
// slope base with the scalar (the value the band-0 slope row displays).
static void copy_osc_band(Synth::Oscillator& osc, const uint32_t from, const uint32_t to)
{
    for (uint32_t p = 0; p < 4; ++p) {
        const Synth::ModTarget from_t = band_target(from, p);
        const Synth::ModTarget to_t   = band_target(to, p);
        osc.targets[to_t]             = osc.targets[from_t];
        if (p == 3 && from == 0 && ! Synth::osc_fir_slope_binding_present(osc.targets[from_t])) {
            osc.targets[to_t].base_value = static_cast<float>(osc.fir_slope_percent);
        }
    }
}

// Zeroes band `band`'s four bindings.
static void zero_osc_band(Synth::Oscillator& osc, const uint32_t band)
{
    for (uint32_t p = 0; p < 4; ++p) {
        osc.targets[band_target(band, p)] = Synth::TargetBinding{};
    }
}

// The band header rows' [-]/[+] click edits.  [-] on band N removes band
// N: bands N+1.. shift up into the hole and the last band zeroes.  [+]
// on band N inserts a copy of band N after it: bands above N shift up and
// the vacated block receives band N's bindings (values and full
// modulation state).  Both rewrite the stored oscillator layer in place;
// the graph is stale until the structural re-projection rebuilds it from
// the model, so the caller must not compile the graph over the rewritten
// bands.  The count-bound clicks are no-ops: the buttons gray at the
// bounds, so a GUI press there cannot occur, and a stray headless edit
// still changes nothing.  Returns true when the change was a band click;
// false falls through to the ordinary value-change store.
static bool apply_osc_band_edit(Synth::InstrumentEditorBank*     bank,
                                const Sculptor::OscGraphMapping& mapping,
                                const Sculptor::GraphChange&     change,
                                const uint32_t                   channel,
                                const uint32_t                   zone)
{
    if (! Sculptor::is_band_button_slot(change.slot_idx)) {
        return false;
    }
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return false;
    }
    uint32_t layer = Synth::max_layers;
    for (uint32_t l = 0; l < Synth::max_layers; ++l) {
        if (mapping.osc_nodes[l] == change.node_idx) {
            layer = l;
            break;
        }
    }
    if (layer == Synth::max_layers) {
        return false;
    }
    Synth::Oscillator& osc             = bank->bank.instruments.entries[zone_entry.instrument].layers[layer];
    const uint32_t     count           = osc_band_model_count(osc);
    const uint32_t     band            = (change.slot_idx - osc_band_first_slot) / osc_band_block_stride;
    const uint32_t     offset_in_block = (change.slot_idx - osc_band_first_slot) % osc_band_block_stride;
    if (band >= count) {
        return true; // a block beyond the count is not projected; nothing to edit
    }
    if (offset_in_block == 2u) {
        // [+]: insert a copy of band `band` after it.
        if (count >= 4) {
            return true;
        }
        for (uint32_t b = count; b > band; --b) {
            copy_osc_band(osc, b - 1u, b);
        }
        osc.fir_band_count = static_cast<uint8_t>(count + 1u);
        return true;
    }
    // [-]: remove band `band`.
    if (count <= 1) {
        return true;
    }
    for (uint32_t b = band; b + 1u < count; ++b) {
        copy_osc_band(osc, b + 1u, b);
    }
    zero_osc_band(osc, count - 1u);
    osc.fir_band_count = static_cast<uint8_t>(count - 1u);
    return true;
}

static bool apply_value_change(Synth::InstrumentEditorBank* bank,
                               Sculptor::Graph*             graph,
                               Sculptor::OscGraphMapping*   mapping,
                               const Sculptor::GraphChange& change,
                               uint32_t                     channel,
                               uint32_t                     zone)
{
    if (change.node_idx >= Sculptor::max_nodes || ! graph->node_occupied(change.node_idx)) {
        return true;
    }

    const Sculptor::Node& node = graph->node(change.node_idx);
    if (change.slot_idx >= node.slots.num_allocated) {
        return true;
    }

    const Sculptor::Slot& slot = node.slots.entries[change.slot_idx];
    switch (node_role(*mapping, change.node_idx)) {
        case role_osc:
            // Switching to bandpass seeds an audible default: an unwired
            // Band Gain row starts at 1.0 so the newly selected filter type
            // is not silent until the gain is raised. A wired gain family
            // keeps its routing. The seed rides a genuine switch: the
            // stored layer's filter type is the pre-edit value, so
            // re-selecting the already-stored bandpass type never resets a
            // hand-set unwired gain.
            if (change.slot_idx == osc_fir_type_prop && slot.property_type == Sculptor::PropertyType::list &&
                slot.value.list_index == static_cast<uint32_t>(Synth::fir_bandpass)) {
                const Synth::Zone& zone_entry          = bank->bank.channel_zones[channel][zone];
                bool               previously_bandpass = false;
                if (zone_entry.start_note != 0 && zone_entry.instrument < bank->bank.instruments.num_allocated) {
                    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                        if (mapping->osc_nodes[layer] == change.node_idx) {
                            previously_bandpass =
                                bank->bank.instruments.entries[zone_entry.instrument].layers[layer].fir_type ==
                                Synth::fir_bandpass;
                            break;
                        }
                    }
                }
                if (! previously_bandpass) {
                    const uint32_t gain_t = static_cast<uint32_t>(Synth::mod_fir_band0_gain);
                    const bool     gain_wired =
                        connection_into(*graph, change.node_idx, osc_target_env_dot(gain_t)) != pool_no_slot ||
                        connection_into(*graph, change.node_idx, osc_target_lfo_dot(gain_t)) != pool_no_slot ||
                        connection_into(*graph, change.node_idx, osc_target_src_dot(gain_t, 0)) != pool_no_slot ||
                        connection_into(*graph, change.node_idx, osc_target_src_dot(gain_t, 1)) != pool_no_slot;
                    if (! gain_wired) {
                        Sculptor::PropertyValue seed = {};
                        seed.real                    = 1.0f;
                        graph->set_slot_value(change.node_idx, osc_target_row_slot[gain_t], seed);
                    }
                }
            }
            // The band headers' click edits rewrite the stored bands in
            // place (insert a copy after band N, or remove band N with
            // later bands shifting up); the graph is stale until the
            // structural re-projection rebuilds it from the model, so no
            // compile runs here.
            if (apply_osc_band_edit(bank, *mapping, change, channel, zone)) {
                return true;
            }
            // Per-layer values: the edited row is that layer's own base; no
            // sibling view syncs.  An unwired node is an ordinary editor
            // node: its edits store into the authoring instrument like any
            // wired node's.
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        case role_env:
        case role_lfo: {
            const bool applied = apply_osc_graph_descriptor_edit(*graph,
                                                                 *mapping,
                                                                 &bank->bank,
                                                                 change.node_idx,
                                                                 change.slot_idx,
                                                                 slot.value);
            if (applied) {
                sync_lfo_descriptor_views(bank, graph, *mapping, change.node_idx);
            }

            return applied;
        }
        case role_input:
        case role_output:
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        default:
            return true;
    }
}

// Removes one oscillator layer from the zone's instrument (graph node
// delete): the editor layers shift down by ordinal, and the source row of
// every zone that references the same instrument clears the deleted
// ordinal's entry and decrements higher ones, never refusing.  The
// same kind-0 removal and rekey loop runs for every referencing zone, so
// other zones' records follow the ordinal shift.  Generator records are
// descriptor-keyed, so no record remapping beyond the oscillator range is
// needed.
static bool remove_osc_layer(Synth::InstrumentEditorBank* bank,
                             Sculptor::Graph*             graph,
                             Sculptor::OscGraphMapping*   mapping,
                             uint32_t                     channel,
                             uint32_t                     zone,
                             uint32_t                     layer)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank->bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank->bank.instruments.num_allocated) {
        return false;
    }
    Synth::Instrument& instrument      = bank->bank.instruments.entries[zone_entry.instrument];
    const uint32_t     instrument_slot = zone_entry.instrument;
    if (layer < instrument.layer_count) {
        for (uint32_t l = layer; l + 1 < Synth::max_layers; ++l) {
            instrument.layers[l] = instrument.layers[l + 1];
        }
        instrument.layers[Synth::max_layers - 1] = Synth::Oscillator{};
        instrument.layer_count--;
    }

    // Every zone referencing the same instrument slot: clear the deleted
    // ordinal's source entry and decrement higher ones.  The deleting
    // zone's row is re-derived from its graph wiring by the batch's final
    // compile; the cleared row keeps other zones' state consistent until
    // then.
    for (uint32_t ch = 0; ch < Synth::max_channels; ++ch) {
        for (uint32_t z = 0; z < Synth::max_instr_per_channel; ++z) {
            const Synth::Zone& other = bank->bank.channel_zones[ch][z];
            if (other.start_note == 0 || other.instrument != instrument_slot) {
                continue;
            }
            uint8_t* source = bank->graph_osc_source[ch][z];
            for (uint32_t s = 0; s < Synth::max_layers; ++s) {
                if (source[s] == layer + 1) {
                    source[s] = 0;
                }
                else if (source[s] > layer + 1) {
                    source[s] = static_cast<uint8_t>(source[s] - 1);
                }
            }
        }
    }

    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout record = bank->graph_layout[i];
        // Master-effect records use channel == max_channels, outside the
        // channel_zones table; only kind-0 records on an ordinary channel
        // reference instrument layers.
        bool references = false;
        if (record.kind == 0 && record.channel < Synth::max_channels && record.zone < Synth::max_instr_per_channel) {
            const Synth::Zone& rz = bank->bank.channel_zones[record.channel][record.zone];
            references            = rz.start_note != 0 && rz.instrument == instrument_slot;
        }
        if (references && record.index >= canonical_first_osc && record.index < canonical_first_env) {
            const uint32_t ordinal = record.index - canonical_first_osc;
            if (ordinal == layer) {
                continue;
            }
            if (ordinal > layer) {
                record.index = static_cast<uint8_t>(record.index - 1);
            }
        }
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;

    // The mapping's oscillator run compacts exactly like the editor
    // layers: osc_nodes[o] names the node of editor ordinal o, so a
    // multi-node delete batch whose later events recompile the instrument
    // never sees a gap in the run.
    for (uint32_t l = layer; l + 1 < Synth::max_layers; ++l) {
        mapping->osc_nodes[l] = mapping->osc_nodes[l + 1];
    }
    mapping->osc_nodes[Synth::max_layers - 1] = Sculptor::pool_no_slot;

    return true;
}

static bool apply_node_deleted(Synth::InstrumentEditorBank* bank,
                               Sculptor::Graph*             graph,
                               Sculptor::OscGraphMapping*   mapping,
                               const Sculptor::GraphChange& change,
                               uint32_t                     channel,
                               uint32_t                     zone)
{
    const uint32_t node_idx = change.node_idx;

    const int32_t instance = find_instance(*mapping, node_idx);
    if (instance >= 0) {
        const Sculptor::DetachedNode entry = mapping->detached[instance];
        if (entry.uid != 0) {
            const int32_t record_idx = find_record(*bank, channel, zone, entry.kind, entry.desc_id, entry.uid);
            if (record_idx >= 0) {
                remove_record_at(bank, static_cast<uint32_t>(record_idx));
            }
        }
        for (uint32_t i = static_cast<uint32_t>(instance); i + 1 < mapping->detached_count; ++i) {
            mapping->detached[i] = mapping->detached[i + 1];
        }
        mapping->detached_count--;
        return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
    }

    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        if (mapping->osc_nodes[layer] == node_idx) {
            // The compaction realigns the mapping run with the instrument's
            // layers, and the recompile from live wires rewrites the zone
            // (cells whose wires disappear with the layer, targets that lose
            // their last serving modulator).  When another oscillator of this
            // batch is still pending deletion, the store defers to that
            // node_deleted's final compile.
            if (! remove_osc_layer(bank, graph, mapping, channel, zone, layer)) {
                return false;
            }
            if (! reconcile_generator_records(bank, graph, mapping, channel, zone)) {
                return false;
            }
            return store_compiled_instrument(bank, *graph, *mapping, channel, zone);
        }
    }
    return true; // fixed nodes are vetoed upstream; nothing else carries state
}

// Relocates one occupied connection off its base target row onto the
// family dot its output end implies (eventless; the fx param_field
// pattern): an envelope/LFO instance output lands on the block's
// envelope/LFO dot, a MIDI or CC output lands on the first free Source
// dot (A before B). Seeds the row's audible defaults: a first LFO wire
// adopts depth 0.5 while the depth still reads 0 (0 silences the LFO
// entirely), a fresh source row gets the neutral add/1.0 seeds.
// *relocated reports whether the wire moved. Returns false when the
// family has no free input dot; the validator pre-checks every
// well-formed edit, so only a headless caller that bypassed it refuses.
static bool relocate_base_row_wire(Sculptor::Graph*                 graph,
                                   const Sculptor::OscGraphMapping* mapping,
                                   uint32_t                         connection_idx,
                                   bool*                            relocated)
{
    *relocated                             = false;
    const Sculptor::Connection& connection = graph->get_connection(connection_idx);
    bool                        base_row   = false;
    uint32_t                    target     = 0;
    uint32_t                    family     = 0;
    uint32_t                    dot        = Sculptor::pool_no_slot;
    if (! decode_osc_input(*mapping, connection.input, &base_row, &target, &family, &dot) || ! base_row) {
        return true;
    }

    uint32_t      wire_family = family_src;
    const int32_t inst        = find_instance(*mapping, connection.output.node_idx);
    if (inst >= 0 && mapping->detached[inst].kind == 1) {
        wire_family = family_env;
    }
    else if (inst >= 0 && mapping->detached[inst].kind == 2) {
        wire_family = family_lfo;
    }
    const uint32_t landing = osc_target_free_input(*graph, connection.input.node_idx, target, wire_family);
    if (landing == Sculptor::pool_no_slot) {
        return false;
    }

    graph->set_connection_input(connection_idx, Sculptor::EndPoint{ connection.input.node_idx, landing });
    if (wire_family == family_env &&
        graph->node(connection.input.node_idx).slots.entries[landing + 1].value.real == 0.0f) {
        // A first envelope wire adopts the legacy unscaled amount: the
        // row's depth defaults to 0, which silences the envelope entirely,
        // and 0 is almost never the intent.
        graph->set_slot_value(connection.input.node_idx, landing + 1, Sculptor::PropertyValue{ .real = 1.0f });
    }
    if (wire_family == family_lfo &&
        graph->node(connection.input.node_idx).slots.entries[landing + 2].value.real == 0.0f) {
        // A first LFO wire adopts an audible depth: the row's depth
        // defaults to 0, which silences the LFO entirely, and 0 is
        // almost never the intent.
        graph->set_slot_value(connection.input.node_idx, landing + 2, Sculptor::PropertyValue{ .real = 0.5f });
    }
    if (wire_family == family_src) {
        // Neutral add/1.0 seeds so a fresh source row is audible immediately (fx parity).
        graph->set_slot_value(connection.input.node_idx, landing + 1, Sculptor::PropertyValue{ .list_index = 0 });
        graph->set_slot_value(connection.input.node_idx, landing + 2, Sculptor::PropertyValue{ .real = 1.0f });
    }
    *relocated = true;
    return true;
}

static bool apply_connection_change(Synth::InstrumentEditorBank* bank,
                                    Sculptor::Graph*             graph,
                                    Sculptor::OscGraphMapping*   mapping,
                                    const Sculptor::GraphChange& change,
                                    uint32_t                     channel,
                                    uint32_t                     zone)
{
    // A wire dropped on a base target row relocates eventlessly onto the
    // family dot its output end implies (relocate_base_row_wire);
    // the guard refusals mirror the validator, so a well-formed edit
    // never refuses here; a headless caller that bypasses the validator
    // gets a refused batch.
    // A deletion event never observes a base-row endpoint: the apply-side
    // bind and the relocation land in the same drain, so by the time a
    // delete drains its wire already sits on the family dot.  A delete
    // event that still records a base-row input is a headless injection.
    if (change.kind == Sculptor::ChangeKind::connection_deleted) {
        bool     deleted_row    = false;
        uint32_t deleted_target = 0;
        uint32_t deleted_family = 0;
        uint32_t deleted_dot    = Sculptor::pool_no_slot;
        if (decode_osc_input(*mapping,
                             change.connection_input,
                             &deleted_row,
                             &deleted_target,
                             &deleted_family,
                             &deleted_dot) &&
            deleted_row) {
            return false;
        }
    }

    bool relocated = false;
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx)) {
        bool event_relocated = false;
        if (! relocate_base_row_wire(graph, mapping, change.connection_idx, &event_relocated)) {
            return false; // validator pre-checked
        }
        relocated = event_relocated;
    }

    // Wiring an envelope into the volume target forces the volume-envelope
    // shape on the shared descriptor: minimum 0 with the top preserved; raw
    // point values, including the first, stay as drawn.  The wire-time
    // validator refuses cross-target sharing, so the conversion never changes
    // what a non-volume user hears.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx)) {
        const Sculptor::Connection& connection = graph->get_connection(change.connection_idx);
        bool                        base_row   = false;
        uint32_t                    target     = 0;
        uint32_t                    family     = 0;
        uint32_t                    dot        = Sculptor::pool_no_slot;
        if (decode_osc_input(*mapping, connection.input, &base_row, &target, &family, &dot) && family == family_env &&
            target == static_cast<uint32_t>(Synth::mod_volume)) {
            const int32_t inst = find_instance(*mapping, connection.output.node_idx);
            if (inst >= 0 && mapping->detached[inst].kind == 1 && mapping->detached[inst].desc_id != 0 &&
                mapping->detached[inst].desc_id <= bank->bank.envelopes.num_allocated) {
                Synth::EnvelopeDescriptor& env = bank->bank.envelopes.entries[mapping->detached[inst].desc_id - 1];
                if (! Sculptor::env_volume_shape_ok(env)) {
                    // A negative effective top cannot be preserved under the
                    // nonnegative volume range; refuse the wire.
                    if (! Sculptor::env_convert_to_volume_shape(env)) {
                        return false;
                    }
                }
            }
        }
    }

    // A batch may hold further wire-landing events behind this one; their
    // connections still sit on their base rows and would trip the compile's
    // invariant. Relocate every remaining base-row wire now (eventless, the
    // helper above), so an applied batch always leaves a compile-clean
    // graph; a headless row wire that never went through the applier still
    // refuses at compile time.
    for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
        if (! graph->connection_occupied(c) || c == change.connection_idx) {
            continue;
        }
        bool other_relocated = false;
        if (! relocate_base_row_wire(graph, mapping, c, &other_relocated)) {
            return false;
        }
        relocated = relocated || other_relocated;
    }

    if (relocated) {
        // The relocated row's LFO/source fields become live (or dormant,
        // for a moved-away wire): the edit-disabled flags follow the new
        // wiring eventlessly, like the projection's rebuild does.
        install_edit_disabled_flags(graph, *mapping);
    }

    // One wire per oscillator: on a wire landing, the
    // oscillator's other sum-input wires die in the same apply step,
    // so the graph, the stored row and the undo snapshot stay in step
    // without a refusal.  The in-apply delete precedes the store, so
    // the compile below derives the row from the settled wiring.
    if ((change.kind == Sculptor::ChangeKind::connection_added ||
         change.kind == Sculptor::ChangeKind::connection_changed) &&
        change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx)) {
        const Sculptor::Connection& connection = graph->get_connection(change.connection_idx);
        bool                        from_osc   = false;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            if (mapping->osc_nodes[layer] == connection.output.node_idx) {
                from_osc = true;
                break;
            }
        }
        if (from_osc && classify_input(*mapping, connection.input) == in_sum) {
            for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
                if (c == change.connection_idx || ! graph->connection_occupied(c)) {
                    continue;
                }
                const Sculptor::Connection& other = graph->get_connection(c);
                if (other.output.node_idx == connection.output.node_idx &&
                    classify_input(*mapping, other.input) == in_sum) {
                    graph->delete_connection(c);
                }
            }
        }
    }

    if (! reconcile_generator_records(bank, graph, mapping, channel, zone)) {
        return false;
    }
    const bool osc_sum_rewire =
        (change.kind == Sculptor::ChangeKind::connection_deleted)
            ? classify_output(*mapping, change.connection_output) == out_osc &&
                  classify_input(*mapping, change.connection_input) == in_sum
            : change.connection_idx < Sculptor::max_connections && graph->connection_occupied(change.connection_idx) &&
                  classify_output(*mapping, graph->get_connection(change.connection_idx).output) == out_osc &&
                  classify_input(*mapping, graph->get_connection(change.connection_idx).input) == in_sum;
    const bool stored = store_compiled_instrument(bank, *graph, *mapping, channel, zone, osc_sum_rewire);
    // The apply consumes the change it processed and settles the events its
    // in-apply rewrites produced (relocations, in-apply deletes): the
    // caller's next drain sees only edits made after the apply returned.
    Sculptor::GraphChange scratch[Sculptor::max_pending_changes] = {};
    (void)graph->take_changes(scratch, Sculptor::max_pending_changes);
    return stored;
}

// The run spans slots 0..k where k is the highest wired slot; 0 when
// nothing is wired.  Derived from the zone's source row alone.
uint32_t Sculptor::osc_row_run_length(const uint8_t source_row[Synth::max_layers])
{
    uint32_t run = 0;

    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        if (source_row[slot] != 0) {
            run = slot + 1;
        }
    }

    return run;
}

bool Sculptor::osc_row_is_red(const uint8_t source_row[Synth::max_layers], uint32_t slot)
{
    return slot < osc_row_run_length(source_row) && source_row[slot] == 0;
}

uint32_t Sculptor::osc_graph_canonical_index(const OscGraphMapping& mapping, uint32_t node_idx)
{
    if (node_idx == pool_no_slot) {
        return pool_no_slot;
    }
    for (uint32_t index = 0; index < canonical_node_count; ++index) {
        if (node_at_canonical_index(mapping, index) == node_idx) {
            return index;
        }
    }
    return pool_no_slot;
}

// A generator instance no wire references becomes free-standing: a
// kind-1/2 record preserves it across re-projection.  Instances already
// carrying a record keep it.
bool Sculptor::detach_osc_graph_instance(Synth::InstrumentEditorBank* bank,
                                         const Sculptor::Graph&       graph,
                                         Sculptor::OscGraphMapping*   mapping,
                                         uint32_t                     channel,
                                         uint32_t                     zone,
                                         uint32_t                     registry_idx)
{
    if (bank->graph_layout_count >= Synth::max_graph_records) {
        return false;
    }
    const Sculptor::DetachedNode entry  = mapping->detached[registry_idx];
    Synth::GraphNodeLayout       record = {};
    record.channel                      = static_cast<uint8_t>(channel);
    record.zone                         = static_cast<uint8_t>(zone);
    record.kind                         = entry.kind;
    record.index                        = entry.desc_id;
    const Sculptor::Node& node          = graph.node(entry.node_idx);
    record.x                            = node.position.x;
    record.y                            = node.position.y;
    record.width_override               = node.content_width_override;
    record.height_override              = node.content_height_override;
    if (entry.kind == 2) {
        // The sources ride the live depth/rate edges, so edits while
        // free-standing survive re-projection.
        const uint32_t   depth_conn = connection_into(graph, entry.node_idx, mapping->lfo_depth_input_slot);
        const uint32_t   rate_conn  = connection_into(graph, entry.node_idx, mapping->lfo_rate_input_slot);
        Synth::ModSource source     = Synth::ModSource::none;
        if (depth_conn != Sculptor::pool_no_slot) {
            if (! edge_source(graph, *mapping, depth_conn, &source)) {
                return false;
            }
            record.depth_source = static_cast<uint8_t>(source);
        }
        if (rate_conn != Sculptor::pool_no_slot) {
            if (! edge_source(graph, *mapping, rate_conn, &source)) {
                return false;
            }
            record.rate_source = static_cast<uint8_t>(source);
        }
    }
    record.uid = Sculptor::allocate_detached_uid(*bank, channel, zone, entry.kind);
    append_record(bank, record);
    mapping->detached[registry_idx].uid = record.uid;
    return true;
}

// Lowest per-(zone, kind) instance id not in use by a live record.
uint8_t Sculptor::allocate_detached_uid(const Synth::InstrumentEditorBank& bank,
                                        uint32_t                           channel,
                                        uint32_t                           zone,
                                        uint32_t                           kind)
{
    bool used[256] = {};
    for (uint32_t i = 0; i < bank.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind == kind) {
            used[record.uid] = true;
        }
    }
    for (uint32_t uid = 1; uid < 256; ++uid) {
        if (! used[uid]) {
            return static_cast<uint8_t>(uid);
        }
    }
    // Unreachable while the caller enforces the per-zone detached cap (120
    // records cannot cover 255 ids).
    assert(false);
    return 0;
}

// Applies one oscillator rename to the zone's kind-0 layout record: the
// bounded node title rides the existing bound-node record, and a missing
// record appends with the node's live position and size.  An empty node
// title writes nothing, so an empty rename commit stays a no-op and the
// re-projection restores the stored (or default) title.
static bool upsert_osc_name_record(Synth::InstrumentEditorBank*     bank,
                                   const Sculptor::Graph&           graph,
                                   const Sculptor::OscGraphMapping& mapping,
                                   uint32_t                         node_idx,
                                   uint32_t                         channel,
                                   uint32_t                         zone)
{
    if (node_idx >= Sculptor::max_nodes || ! graph.node_occupied(node_idx)) {
        return true;
    }

    const uint32_t canonical = Sculptor::osc_graph_canonical_index(mapping, node_idx);
    if (canonical == Sculptor::pool_no_slot) {
        return true;
    }

    const Sculptor::Node& node = graph.node(node_idx);
    if (node.name[0] == 0) {
        return true;
    }

    Synth::GraphNodeLayout* record =
        Sculptor::upsert_graph_layout_record(bank, channel, zone, 0, static_cast<uint8_t>(canonical), node);
    if (! record) {
        return false;
    }

    memset(record->name, 0, sizeof(record->name));
    Sculptor::copy_name_bounded(record->name, sizeof(record->name), node.name, sizeof(node.name));
    return true;
}

bool Sculptor::apply_osc_graph_change(Synth::InstrumentEditorBank* bank,
                                      Graph*                       graph,
                                      OscGraphMapping*             mapping,
                                      const GraphChange&           change,
                                      uint32_t                     channel,
                                      uint32_t                     zone)
{
    switch (change.kind) {
        case ChangeKind::value_changed:
            return apply_value_change(bank, graph, mapping, change, channel, zone);
        case ChangeKind::connection_added:
        case ChangeKind::connection_deleted:
        case ChangeKind::connection_changed:
            return apply_connection_change(bank, graph, mapping, change, channel, zone);
        case ChangeKind::node_deleted:
            return apply_node_deleted(bank, graph, mapping, change, channel, zone);
        case ChangeKind::name_changed:
            return upsert_osc_name_record(bank, *graph, *mapping, change.node_idx, channel, zone);
        default:
            return true; // node/slot additions and colors carry no model state
    }
}

// Eventless graph rewrites (wire-driven retargeting) carry no change
// events, so a caller outside the drain refreshes the generator records
// and the compiled instrument explicitly, the way apply_connection_change
// does after a drained batch.
bool Sculptor::refresh_osc_graph_compilation(Synth::InstrumentEditorBank* bank,
                                             Graph&                       graph,
                                             OscGraphMapping&             mapping,
                                             const uint32_t               channel,
                                             const uint32_t               zone)
{
    if (! reconcile_generator_records(bank, &graph, &mapping, channel, zone)) {
        return false;
    }
    return store_compiled_instrument(bank, graph, mapping, channel, zone);
}

uint32_t Sculptor::count_projected_nodes(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone)
{
    // Fixed inputs, the instrument-parameters node and the sum node, then
    // whatever the zone's instrument and the zone's records add: oscillator
    // layers, generator instances and record surplus (a record that
    // attaches to a derived instance adds no node).
    uint32_t count = Synth::graph_canonical_input_node_count + 2;
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return count;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return count;
    }
    const Synth::Instrument& instrument = bank.bank.instruments.entries[zone_entry.instrument];

    const uint32_t layer_count =
        instrument.layer_count <= Synth::max_layers ? instrument.layer_count : Synth::max_layers;
    count += layer_count; // one node per editor layer, wired or not
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_instance_keys(instrument, keys);
    for (uint32_t k = 0; k < key_count; ++k) {
        const uint32_t num_allocated =
            keys[k].kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
        if (keys[k].desc_id != 0 && keys[k].desc_id <= num_allocated) {
            ++count; // one node per distinct instance key
        }
    }

    // Records: kind-1/2 records either attach to a derived instance or
    // materialize a surplus node.
    bool claimed[2 * Synth::max_layers * Synth::num_mod_targets] = {};
    for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[r];
        if (record.channel != channel || record.zone != zone) {
            continue;
        }
        if (record.kind == 1 || record.kind == 2) {
            const uint32_t num_allocated =
                record.kind == 1 ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated) {
                continue; // invalid records are validation's business
            }
            bool attached = false;
            for (uint32_t k = 0; k < key_count && ! attached; ++k) {
                if (claimed[k] || keys[k].kind != record.kind || keys[k].desc_id != record.index) {
                    continue;
                }
                if (record.kind == 2 &&
                    (keys[k].depth_source != record.depth_source || keys[k].rate_source != record.rate_source)) {
                    continue;
                }
                claimed[k] = true;
                attached   = true;
            }
            if (! attached) {
                ++count;
            }
        }
    }
    return count;
}

// The channel-wide removal-refusal scan: the shared per-channel registry
// makes a CC wired by the channel's fx chain OR by any zone's osc routing
// refuse removal in both panes.  The zone scan stops at the first empty
// slot (an empty slot's instrument byte is meaningless, like the library
// load).  Out-of-range channel (master-chain callers keep the fx-chain-only
// guard) and out-of-range CC refuse nothing.
bool Sculptor::channel_uses_cc(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t cc_number)
{
    if (channel >= Synth::max_channels || ! Synth::cc_is_assignable(cc_number)) {
        return false;
    }

    if (Sculptor::fx_chain_uses_cc(bank.bank.channel_chains[channel], cc_number)) {
        return true;
    }

    const uint16_t bit = static_cast<uint16_t>(1u << (cc_number - Synth::cc_msb_base));
    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel && bank.bank.channel_zones[channel][zone].start_note;
         ++zone) {
        const uint32_t instr = bank.bank.channel_zones[channel][zone].instrument;
        if (instr >= bank.bank.instruments.num_allocated) {
            continue;
        }
        if (Sculptor::instrument_cc_mask(bank.bank.instruments.entries[instr]) & bit) {
            return true;
        }
    }
    return false;
}

bool Sculptor::project_editor_to_graph(const Synth::InstrumentEditorBank& bank,
                                       Graph*                             graph,
                                       OscGraphMapping*                   mapping,
                                       uint32_t                           channel,
                                       uint32_t                           zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return false;
    }
    const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
    if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
        return false;
    }
    if (! project_instrument_to_graph(bank.bank.instruments.entries[zone_entry.instrument],
                                      bank.bank,
                                      graph,
                                      mapping,
                                      &bank.added_cc_mask[channel],
                                      bank.graph_osc_source[channel][zone])) {
        return false;
    }
    mapping->channel = channel;
    mapping->zone    = zone;

    // Oscillator titles persist in the 32-byte kind-0 record field, so the
    // rename editor accepts at most 32 counted bytes (33 with the
    // terminator InputText requires).
    graph->rename_buffer_size = sizeof(Synth::GraphNodeLayout::name) + 1;

    // Records attach to derived nodes: kind-1/2 records to the first
    // unclaimed instance with a matching key (records visited in array
    // order; attachment consumes one instance per record).  Surplus
    // records materialize free-standing nodes.
    bool claimed[max_osc_instances] = {};
    for (uint32_t r = 0; r < bank.graph_layout_count; ++r) {
        const Synth::GraphNodeLayout& record = bank.graph_layout[r];
        if (record.channel != channel || record.zone != zone) {
            continue;
        }
        if (record.kind == 0) {
            const uint32_t node = node_at_canonical_index(*mapping, record.index);
            if (node == pool_no_slot) {
                continue;
            }
            graph->set_node_layout(node,
                                   vmath::vec2(record.x, record.y),
                                   record.width_override,
                                   record.height_override);
            if (record.name[0] != 0) {
                // rename_node reads its argument to the first NUL, so the
                // record field is staged first: a full 32-byte name has no
                // terminator, and the zeroed buffer keeps one reserved.
                char staged[sizeof(record.name) + 1] = {};
                Sculptor::copy_name_bounded(staged, sizeof(staged), record.name, sizeof(record.name));
                graph->rename_node(node, staged);
            }
            continue;
        }
        if (record.kind == 5) {
            // The Instrument Parameters node is structural but not canonically
            // numbered: its layout record keys the zone alone and re-applies
            // the saved position and size overrides when it exists.
            if (mapping->instrument_node != pool_no_slot) {
                graph->set_node_layout(mapping->instrument_node,
                                       vmath::vec2(record.x, record.y),
                                       record.width_override,
                                       record.height_override);
            }
            continue;
        }
        if (record.kind == 1 || record.kind == 2) {
            const bool     is_env        = record.kind == 1;
            const uint32_t num_allocated = is_env ? bank.bank.envelopes.num_allocated : bank.bank.lfos.num_allocated;
            if (record.index == 0 || record.index > num_allocated || record.depth_source > num_osc_graph_inputs ||
                record.rate_source > num_osc_graph_inputs) {
                return false;
            }
            bool attached = false;
            for (uint32_t i = 0; i < mapping->detached_count && ! attached; ++i) {
                DetachedNode& entry = mapping->detached[i];
                if (claimed[i] || entry.uid != 0 || entry.kind != record.kind || entry.desc_id != record.index) {
                    continue;
                }
                if (record.kind == 2 &&
                    (entry.depth_source != record.depth_source || entry.rate_source != record.rate_source)) {
                    continue;
                }
                claimed[i]         = true;
                attached           = true;
                entry.uid          = record.uid;
                entry.depth_source = record.depth_source;
                entry.rate_source  = record.rate_source;
                graph->set_node_layout(entry.node_idx,
                                       vmath::vec2(record.x, record.y),
                                       record.width_override,
                                       record.height_override);
                char name[32];
                snprintf(name, sizeof(name), is_env ? "Envelope %u" : "LFO %u", record.uid);
                graph->rename_node(entry.node_idx, name);
            }
            if (attached) {
                continue;
            }
            // The registry-full refusal applies to surplus instances only:
            // attaching to an existing instance adds no node.
            if (mapping->detached_count >= max_osc_instances) {
                return false;
            }
            // Surplus record: a free-standing generator instance with its
            // own position, uid and (for LFOs) depth/rate source wires.
            char name[32];
            snprintf(name, sizeof(name), is_env ? "Envelope %u" : "LFO %u", record.uid);
            const uint32_t node = is_env ? create_env_node(graph,
                                                           name,
                                                           vmath::vec2(record.x, record.y),
                                                           bank.bank.envelopes.entries[record.index - 1])
                                         : create_lfo_node(graph,
                                                           name,
                                                           vmath::vec2(record.x, record.y),
                                                           bank.bank.lfos.entries[record.index - 1]);
            if (node == pool_no_slot) {
                return false;
            }
            DetachedNode& entry = mapping->detached[mapping->detached_count++];
            entry.node_idx      = node;
            entry.kind          = record.kind;
            entry.desc_id       = record.index;
            entry.uid           = record.uid;
            entry.depth_source  = record.depth_source;
            entry.rate_source   = record.rate_source;
            if (record.kind == 2 && record.depth_source != 0 &&
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.depth_source - 1] },
                    EndPoint{ node, mapping->lfo_depth_input_slot }) == Sculptor::pool_no_slot) {
                return false;
            }
            if (record.kind == 2 && record.rate_source != 0 &&
                graph->add_connection(
                    EndPoint{ mapping->input_node, mapping->input_source_slots[record.rate_source - 1] },
                    EndPoint{ node, mapping->lfo_rate_input_slot }) == Sculptor::pool_no_slot) {
                return false;
            }
        }
    }

    install_edit_disabled_flags(graph, *mapping);

    return true;
}

bool Sculptor::undo_group_needs_snapshot(UndoGroupState* state, UndoGroupTag tag)
{
    if (state->has_last && state->last.kind == tag.kind && state->last.id0 == tag.id0 && state->last.id1 == tag.id1) {
        return false;
    }
    state->last     = tag;
    state->has_last = true;
    return true;
}

void Sculptor::undo_group_reset(UndoGroupState* state)
{
    state->has_last = false;
}

bool Sculptor::graph_records_have_capacity(const Synth::InstrumentEditorBank& bank, uint32_t additional)
{
    return bank.graph_layout_count + additional <= Synth::max_graph_records;
}

// Updates or appends the layout record keyed by (channel, zone, kind,
// index) with uid 0: the lookup selects bound records whose uid is 0, and
// records with another uid are not selected.  The record's position and
// size overrides follow the node, and a missing record appends zeroed when
// the array has capacity.  Returns the existing-or-appended record, or null
// on capacity refusal.
Synth::GraphNodeLayout* Sculptor::upsert_graph_layout_record(Synth::InstrumentEditorBank* bank,
                                                             uint32_t                     channel,
                                                             uint32_t                     zone,
                                                             uint8_t                      kind,
                                                             uint8_t                      index,
                                                             const Sculptor::Node&        node_ref)
{
    const int32_t record_idx = Sculptor::find_record(*bank, channel, zone, kind, index, 0);
    if (record_idx >= 0) {
        Synth::GraphNodeLayout& record = bank->graph_layout[record_idx];
        record.x                       = node_ref.position.x;
        record.y                       = node_ref.position.y;
        record.width_override          = node_ref.content_width_override;
        record.height_override         = node_ref.content_height_override;
        return &record;
    }

    if (! Sculptor::graph_records_have_capacity(*bank, 1)) {
        return nullptr;
    }

    Synth::GraphNodeLayout record                  = {};
    record.channel                                 = static_cast<uint8_t>(channel);
    record.zone                                    = static_cast<uint8_t>(zone);
    record.kind                                    = kind;
    record.index                                   = index;
    record.x                                       = node_ref.position.x;
    record.y                                       = node_ref.position.y;
    record.width_override                          = node_ref.content_width_override;
    record.height_override                         = node_ref.content_height_override;
    bank->graph_layout[bank->graph_layout_count++] = record;
    return &bank->graph_layout[bank->graph_layout_count - 1];
}

void Sculptor::compute_publish_channel_enabled(const Synth::InstrumentEditorBank& bank,
                                               uint8_t                            out_enabled[Synth::max_channels])
{
    // The stored channel_enabled values stay untouched; a channel
    // publishes disabled while it has a playable zone whose source row is
    // all zero (every oscillator unwired: layer_count 0 reaches the
    // runtime's layer_count >= 1 assertion, so the publication refuses
    // it).  Unwired slots inside a wired row still play.
    for (uint32_t channel = 0; channel < Synth::max_channels; ++channel) {
        bool silent = false;
        for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
            const Synth::Zone& zone_entry = bank.bank.channel_zones[channel][zone];
            if (zone_entry.start_note == 0 || zone_entry.instrument >= bank.bank.instruments.num_allocated) {
                continue;
            }

            const uint8_t* source = bank.graph_osc_source[channel][zone];
            bool           wired  = false;
            for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
                if (source[slot] != 0) {
                    wired = true;
                    break;
                }
            }

            if (! wired) {
                silent = true;
                break;
            }
        }
        out_enabled[channel] = (bank.bank.channel_enabled[channel] != 0 && ! silent) ? 1 : 0;
    }
}

bool Sculptor::zone_records_split_copy(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone + 1 >= Synth::max_instr_per_channel) {
        return false;
    }

    // Preflight before any shift: the zone's records must fit after the
    // copy, so a refusal leaves every record untouched.
    uint32_t record_count = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel == channel && record.zone == zone && record.kind != 4) {
            ++record_count;
        }
    }

    if (bank->graph_layout_count + record_count > Synth::max_graph_records) {
        return false;
    }

    // Shift later zones first, then copy: records and source rows move
    // together.  The split copies the source zone's row, so the new zone
    // starts wired the same way.
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.kind != 4 && record.channel == channel && record.zone > zone) {
            record.zone++;
        }
    }

    for (uint32_t z = Synth::max_instr_per_channel - 1; z > zone + 1; --z) {
        memcpy(bank->graph_osc_source[channel][z], bank->graph_osc_source[channel][z - 1], Synth::max_layers);
    }
    memcpy(bank->graph_osc_source[channel][zone + 1], bank->graph_osc_source[channel][zone], Synth::max_layers);

    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        // The copy preserves the source zone's descriptor ids; the
        // follow-up clone_split_generators pass re-points the clone's
        // generator ids.  Effects are channel-wide and do not copy.
        if (record.channel == channel && record.zone == zone && record.kind != 4) {
            Synth::GraphNodeLayout copy                    = record;
            copy.zone                                      = static_cast<uint8_t>(zone + 1);
            bank->graph_layout[bank->graph_layout_count++] = copy;
        }
    }

    return true;
}

// Clones the generator descriptors the split's new zone references; see the
// header comment.  The new zone's instrument still carries the source zone's
// ids when this runs, so the id set covers the instrument's target bindings
// and the zone's kind-1/2 layout records, including detached instances no
// target binding names.
bool Sculptor::clone_split_generators(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone + 1 >= Synth::max_instr_per_channel) {
        return false;
    }
    Synth::Zone*   zones    = bank->bank.channel_zones[channel];
    const uint32_t dst_slot = zones[zone + 1].instrument;
    if (dst_slot >= Synth::max_instruments) {
        return false;
    }
    Synth::Instrument& dst_instrument = bank->bank.instruments.entries[dst_slot];

    bool     env_seen[Synth::max_envelopes + 1] = {};
    bool     lfo_seen[Synth::max_lfos + 1]      = {};
    uint16_t env_map[Synth::max_envelopes + 1]  = {};
    uint16_t lfo_map[Synth::max_lfos + 1]       = {};
    uint32_t unique_envs                        = 0;
    uint32_t unique_lfos                        = 0;

    for (uint32_t layer = 0; layer < dst_instrument.layer_count && layer < Synth::max_layers; ++layer) {
        const Synth::Oscillator& layer_osc = dst_instrument.layers[layer];
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            const uint16_t env_id = layer_osc.targets[target].envelope_desc_id;
            const uint16_t lfo_id = layer_osc.targets[target].lfo_desc_id;
            if (env_id != 0 && env_id <= Synth::max_envelopes && ! env_seen[env_id]) {
                env_seen[env_id] = true;
                ++unique_envs;
            }
            if (lfo_id != 0 && lfo_id <= Synth::max_lfos && ! lfo_seen[lfo_id]) {
                lfo_seen[lfo_id] = true;
                ++unique_lfos;
            }
        }
    }
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel != channel || record.zone != zone + 1) {
            continue;
        }
        if (record.kind != 1 && record.kind != 2) {
            continue;
        }
        const uint16_t id = record.index;
        if (record.kind == 1 && id != 0 && id <= Synth::max_envelopes && ! env_seen[id]) {
            env_seen[id] = true;
            ++unique_envs;
        }
        if (record.kind == 2 && id != 0 && id <= Synth::max_lfos && ! lfo_seen[id]) {
            lfo_seen[id] = true;
            ++unique_lfos;
        }
    }

    // Both pools are checked before any allocation, so a refusal leaves the
    // candidate untouched.
    if (bank->bank.envelopes.num_allocated + unique_envs > Synth::max_envelopes) {
        return false;
    }
    if (bank->bank.lfos.num_allocated + unique_lfos > Synth::max_lfos) {
        return false;
    }

    for (uint16_t old_id = 1; old_id <= Synth::max_envelopes; ++old_id) {
        if (! env_seen[old_id]) {
            continue;
        }
        const uint32_t slot                = bank->bank.envelopes.allocate();
        bank->bank.envelopes.entries[slot] = bank->bank.envelopes.entries[old_id - 1];
        env_map[old_id]                    = static_cast<uint16_t>(slot + 1);
    }
    for (uint16_t old_id = 1; old_id <= Synth::max_lfos; ++old_id) {
        if (! lfo_seen[old_id]) {
            continue;
        }
        const uint32_t slot           = bank->bank.lfos.allocate();
        bank->bank.lfos.entries[slot] = bank->bank.lfos.entries[old_id - 1];
        lfo_map[old_id]               = static_cast<uint16_t>(slot + 1);
    }

    for (uint32_t layer = 0; layer < dst_instrument.layer_count && layer < Synth::max_layers; ++layer) {
        Synth::Oscillator& layer_osc = dst_instrument.layers[layer];
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            Synth::TargetBinding& binding = layer_osc.targets[target];
            const uint16_t        env_id  = binding.envelope_desc_id;
            const uint16_t        lfo_id  = binding.lfo_desc_id;
            if (env_id != 0 && env_id <= Synth::max_envelopes && env_map[env_id] != 0) {
                binding.envelope_desc_id = env_map[env_id];
            }
            if (lfo_id != 0 && lfo_id <= Synth::max_lfos && lfo_map[lfo_id] != 0) {
                binding.lfo_desc_id = lfo_map[lfo_id];
            }
        }
    }
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout& record = bank->graph_layout[i];
        if (record.channel != channel || record.zone != zone + 1) {
            continue;
        }
        if (record.kind != 1 && record.kind != 2) {
            continue;
        }
        const uint16_t new_id = record.kind == 1 ? env_map[record.index] : lfo_map[record.index];
        if (record.index <= Synth::max_envelopes && new_id != 0) {
            record.index = static_cast<uint8_t>(new_id);
        }
    }
    return true;
}

// Compacts graph_layout by dropping the zone's records; shared by the
// zone-drop and zone-clear operations so the record policy cannot drift
// between them. Kind-4 records are channel-wide effect layouts, not zone
// state: they are never dropped, and a drop that renumbers zones leaves
// them alone.
static void filter_zone_records(Synth::InstrumentEditorBank* bank,
                                uint32_t                     channel,
                                uint32_t                     zone,
                                bool                         shift_later_zones)
{
    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        Synth::GraphNodeLayout record = bank->graph_layout[i];
        if (record.kind != 4 && record.channel == channel) {
            if (record.zone == zone)
                continue;
            if (shift_later_zones && record.zone > zone)
                record.zone--;
        }
        bank->graph_layout[write++] = record;
    }
    bank->graph_layout_count = write;
}

void Sculptor::zone_records_drop_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }

    filter_zone_records(bank, channel, zone, true);

    for (uint32_t z = zone; z + 1 < Synth::max_instr_per_channel; ++z) {
        memcpy(bank->graph_osc_source[channel][z], bank->graph_osc_source[channel][z + 1], Synth::max_layers);
    }
    memset(bank->graph_osc_source[channel][Synth::max_instr_per_channel - 1], 0, Synth::max_layers);
}

void Sculptor::zone_records_clear_zone(Synth::InstrumentEditorBank* bank, uint32_t channel, uint32_t zone)
{
    if (channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel) {
        return;
    }

    filter_zone_records(bank, channel, zone, false);

    memset(bank->graph_osc_source[channel][zone], 0, Synth::max_layers);
}

void Sculptor::channel_records_reset(Synth::InstrumentEditorBank* bank, uint32_t channel)
{
    if (channel >= Synth::max_channels) {
        return;
    }

    uint32_t write = 0;
    for (uint32_t i = 0; i < bank->graph_layout_count; ++i) {
        if (bank->graph_layout[i].channel == channel) {
            continue;
        }
        bank->graph_layout[write++] = bank->graph_layout[i];
    }

    bank->graph_layout_count = write;

    for (uint32_t zone = 0; zone < Synth::max_instr_per_channel; ++zone) {
        memset(bank->graph_osc_source[channel][zone], 0, Synth::max_layers);
    }
}

static bool layout_model_valid(const Synth::Instrument&         instrument,
                               const Synth::EnvelopeDescriptor* envelopes,
                               uint32_t                         envelope_count,
                               const Synth::LFODescriptor*      lfos,
                               uint32_t                         lfo_count)
{
    if (envelope_count > Synth::max_envelopes || lfo_count > Synth::max_lfos || (! envelopes && envelope_count) ||
        (! lfos && lfo_count))
        return false;
    memset(&clipboard_model_bank, 0, sizeof(clipboard_model_bank));
    for (uint32_t index = 0; index < envelope_count; ++index) {
        const uint32_t slot                          = clipboard_model_bank.envelopes.allocate();
        clipboard_model_bank.envelopes.entries[slot] = envelopes[index];
    }
    for (uint32_t index = 0; index < lfo_count; ++index) {
        const uint32_t slot                     = clipboard_model_bank.lfos.allocate();
        clipboard_model_bank.lfos.entries[slot] = lfos[index];
    }
    const uint32_t slot                            = clipboard_model_bank.instruments.allocate();
    clipboard_model_bank.instruments.entries[slot] = instrument;
    clipboard_model_bank.channel_enabled[0]        = 1;
    clipboard_model_bank.channel_zones[0][0]       = { 1, static_cast<uint8_t>(slot) };
    if (! Synth::validate_instrument_bank(&clipboard_model_bank))
        return false;
    for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            const Synth::TargetBinding& binding = instrument.layers[layer].targets[target];
            for (uint32_t input = 0; input < binding.num_inputs; ++input) {
                if (binding.inputs[input].source == Synth::ModSource::none)
                    return false;
            }
        }
    }
    return true;
}

static uint32_t derive_portable_nodes(const Synth::Instrument& instrument, PortableNode* nodes)
{
    uint32_t count = 0;
    for (uint32_t canonical = 0; canonical < 2 + instrument.layer_count; ++canonical) {
        nodes[count]                         = {};
        nodes[count].locator.canonical_index = static_cast<uint8_t>(canonical);
        nodes[count].index                   = static_cast<uint8_t>(canonical);
        ++count;
    }
    InstanceKey    keys[2 * Synth::max_layers * Synth::num_mod_targets];
    const uint32_t key_count = derive_instance_keys(instrument, keys);
    for (uint32_t key = 0; key < key_count; ++key) {
        nodes[count]              = {};
        PortableNode& node        = nodes[count];
        node.locator.kind         = keys[key].kind;
        node.index                = keys[key].desc_id;
        node.locator.depth_source = keys[key].depth_source;
        node.locator.rate_source  = keys[key].rate_source;
        bool first                = true;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                const Synth::TargetBinding& gen = instrument.layers[layer].targets[target];
                const bool serves = node.locator.kind == 1
                                        ? gen.envelope_desc_id == node.index
                                        : gen.lfo_desc_id == node.index &&
                                              static_cast<uint8_t>(gen.lfo_depth_source) == node.locator.depth_source &&
                                              static_cast<uint8_t>(gen.lfo_rate_source) == node.locator.rate_source;
                if (! serves)
                    continue;
                node.served[target] |= static_cast<uint8_t>(1u << layer);
                if (first) {
                    node.locator.layer  = static_cast<uint8_t>(layer);
                    node.locator.target = static_cast<uint8_t>(target);
                    first               = false;
                }
            }
        }
        ++count;
    }
    return count;
}

// Structural validity of a portable layout record: a NUL-terminated name
// (stale title bytes are accepted and never copied into the mapping), finite
// position and size overrides.  A record failing this is document
// corruption; only a structurally valid record that resolves to no node may
// be dropped as removable-target debris.
static bool record_structure_ok(const Synth::InstrumentGraphLayout& record)
{
    return memchr(record.name, 0, sizeof(record.name)) && std::isfinite(record.x) && std::isfinite(record.y) &&
           std::isfinite(record.width_override) && std::isfinite(record.height_override);
}

static uint32_t resolve_portable_node(const Synth::InstrumentGraphLayout& record,
                                      const PortableNode*                 nodes,
                                      uint32_t                            count,
                                      const bool*                         claimed)
{
    if (! record_structure_ok(record)) {
        return pool_no_slot;
    }

    for (uint32_t node = 0; node < count; ++node) {
        const PortableNode& candidate = nodes[node];
        if (candidate.locator.kind != record.kind) {
            continue;
        }

        if (claimed[node] && record.kind == 0) {
            // Kind 1/2 duplicates stay a caller-visible refusal: their
            // resolution ignores claims so a second record naming the
            // same instance still maps to it and trips the duplicate
            // check.  Kind 0 records instead fall through to the next
            // matching node.
            continue;
        }

        if (record.kind == 0 && candidate.index == record.canonical_index) {
            // Kind-0 records name fixed nodes and oscillator layers by
            // canonical index.  Portable documents always carry the
            // encoded (playback) form's canonical indices, so identity
            // is unambiguous: a raw oscillator rank would alias the
            // fixed input/sum nodes at indices 0/1.
            return node;
        }

        if ((record.kind == 1 || record.kind == 2) && record.layer < Synth::max_layers &&
            record.target < Synth::num_mod_targets && (candidate.served[record.target] & (1u << record.layer)) &&
            (record.kind == 1 || (candidate.locator.depth_source == record.depth_source &&
                                  candidate.locator.rate_source == record.rate_source)))
            return node;
    }

    return pool_no_slot;
}

static void copy_portable_position(Synth::InstrumentGraphLayout*       destination,
                                   const Synth::InstrumentGraphLayout& source)
{
    destination->x               = source.x;
    destination->y               = source.y;
    destination->width_override  = source.width_override;
    destination->height_override = source.height_override;
}

// Shared per-record drop policy for the layout mapping and normalization
// walks: structurally checks one record, resolves it against the node
// pool, and classifies the outcome.  A structurally valid kind-1/2 record
// that resolves to nothing is droppable removed-target debris, logged
// here; canonical (kind 0) and unknown kinds are self-inconsistency the
// caller must refuse.  The callers own claims, ordering and compaction.
enum PortableRecordVerdict {
    portable_record_invalid,
    portable_record_skipped,
    portable_record_resolved
};

static PortableRecordVerdict classify_portable_record(const Synth::InstrumentGraphLayout& record,
                                                      const PortableNode*                 nodes,
                                                      uint32_t                            node_count,
                                                      const bool*                         claimed,
                                                      uint32_t*                           out_node)
{
    if (! record_structure_ok(record)) {
        return portable_record_invalid; // Document corruption, not removable debris.
    }

    const uint32_t node = resolve_portable_node(record, nodes, node_count, claimed);
    if (node != pool_no_slot) {
        *out_node = node;
        return portable_record_resolved;
    }

    if (record.kind == 1 || record.kind == 2) {
        d_printf("Ignored unresolvable graph layout record: kind %u layer %u target %u\n",
                 static_cast<unsigned>(record.kind),
                 static_cast<unsigned>(record.layer),
                 static_cast<unsigned>(record.target));
        return portable_record_skipped;
    }
    return portable_record_invalid;
}

bool Sculptor::map_instrument_graph_layout(const Synth::Instrument&            instrument,
                                           const Synth::EnvelopeDescriptor*    envelopes,
                                           uint32_t                            envelope_count,
                                           const Synth::LFODescriptor*         lfos,
                                           uint32_t                            lfo_count,
                                           const Synth::InstrumentGraphLayout* layout,
                                           uint32_t                            layout_count,
                                           Synth::GraphNodeLayout*             out_records,
                                           uint32_t                            capacity,
                                           uint32_t*                           out_count)
{
    if (! out_count || (! layout && layout_count) || (! out_records && layout_count) ||
        layout_count > Synth::instrument_graph_layout_capacity || capacity < layout_count ||
        ! layout_model_valid(instrument, envelopes, envelope_count, lfos, lfo_count))
        return false;

    PortableNode* const           nodes      = clipboard_decoded_nodes;
    const uint32_t                node_count = derive_portable_nodes(instrument, nodes);
    bool                          claimed[Synth::instrument_graph_layout_capacity] = {};
    Synth::GraphNodeLayout* const records                                          = clipboard_mapped_records;
    memset(records, 0, sizeof(clipboard_mapped_records));
    uint32_t count = 0;

    for (uint32_t index = 0; index < layout_count; ++index) {
        uint32_t                    node = 0;
        const PortableRecordVerdict verdict =
            classify_portable_record(layout[index], nodes, node_count, claimed, &node);
        if (verdict == portable_record_skipped) {
            continue; // No output record, no claim, no uid.
        }

        if (verdict == portable_record_invalid) {
            return false; // Self-inconsistent record, not removable debris.
        }

        if (claimed[node]) {
            return false; // A duplicate claim keeps the surviving uids unique.
        }

        claimed[node]                  = true;
        Synth::GraphNodeLayout& record = records[count++];
        record.kind                    = layout[index].kind;
        record.index                   = nodes[node].index;
        record.depth_source            = layout[index].kind == 2 ? layout[index].depth_source : 0;
        record.rate_source             = layout[index].kind == 2 ? layout[index].rate_source : 0;
        record.uid                     = record.kind == 0 ? 0 : static_cast<uint8_t>(node + 1);
        record.x                       = layout[index].x;
        record.y                       = layout[index].y;
        record.width_override          = layout[index].width_override;
        record.height_override         = layout[index].height_override;
    }

    if (count)
        memcpy(out_records, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

bool Sculptor::normalize_instrument_graph_layout(const Synth::Instrument&            source_instr,
                                                 const Synth::EnvelopeDescriptor*    source_envs,
                                                 uint32_t                            source_env_count,
                                                 const Synth::LFODescriptor*         source_lfos,
                                                 uint32_t                            source_lfo_count,
                                                 const Synth::InstrumentGraphLayout* source_layout,
                                                 uint32_t                            source_layout_count,
                                                 const Synth::Instrument&            decoded_instr,
                                                 const Synth::EnvelopeDescriptor*    decoded_envs,
                                                 uint32_t                            decoded_env_count,
                                                 const Synth::LFODescriptor*         decoded_lfos,
                                                 uint32_t                            decoded_lfo_count,
                                                 Synth::InstrumentGraphLayout*       out_layout,
                                                 uint32_t                            capacity,
                                                 uint32_t*                           out_count)
{
    if (! out_count || (! source_layout && source_layout_count)) {
        return false;
    }

    if (source_layout_count > Synth::instrument_graph_layout_capacity) {
        return false;
    }

    if (source_instr.layer_count != decoded_instr.layer_count) {
        return false;
    }

    if (! layout_model_valid(source_instr, source_envs, source_env_count, source_lfos, source_lfo_count)) {
        return false;
    }

    if (! layout_model_valid(decoded_instr, decoded_envs, decoded_env_count, decoded_lfos, decoded_lfo_count)) {
        return false;
    }

    PortableNode* const                        source_nodes  = clipboard_source_nodes;
    PortableNode* const                        decoded_nodes = clipboard_decoded_nodes;
    const uint32_t                             source_count  = derive_portable_nodes(source_instr, source_nodes);
    const uint32_t                             decoded_count = derive_portable_nodes(decoded_instr, decoded_nodes);
    const Synth::InstrumentGraphLayout** const saved         = clipboard_saved_layout;
    memset(saved, 0, sizeof(clipboard_saved_layout));
    bool claimed[Synth::instrument_graph_layout_capacity] = {};

    for (uint32_t index = 0; index < source_layout_count; ++index) {
        uint32_t                    node = 0;
        const PortableRecordVerdict verdict =
            classify_portable_record(source_layout[index], source_nodes, source_count, claimed, &node);
        if (verdict == portable_record_skipped) {
            continue; // Dropped records never enter saved[], never match a
                      // decoded node, and never fire the tail totality check.
        }

        if (verdict == portable_record_invalid) {
            return false;
        }

        if (saved[node]) {
            return false; // A duplicate claim keeps the surviving uids unique.
        }

        saved[node]   = &source_layout[index];
        claimed[node] = true;
    }
    Synth::InstrumentGraphLayout* const records = clipboard_portable_records;
    memset(records, 0, sizeof(clipboard_portable_records));
    uint32_t count                                         = 0;
    bool     used[Synth::instrument_graph_layout_capacity] = {};

    for (uint32_t decoded = 0; decoded < decoded_count; ++decoded) {
        for (uint32_t source = 0; source < source_count; ++source) {
            if (! saved[source] || source_nodes[source].locator.kind != decoded_nodes[decoded].locator.kind)
                continue;

            bool corresponds =
                source_nodes[source].locator.kind == 0 && source_nodes[source].index == decoded_nodes[decoded].index;
            for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                if (source_nodes[source].served[target] & decoded_nodes[decoded].served[target])
                    corresponds = true;
            }

            if (! corresponds)
                continue;

            if (used[source]) {
                return false;
            }

            for (uint32_t other = source + 1; other < source_count; ++other) {
                if (! saved[other] || source_nodes[other].locator.kind != decoded_nodes[decoded].locator.kind)
                    continue;
                for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
                    if (source_nodes[other].served[target] & decoded_nodes[decoded].served[target]) {
                        return false;
                    }
                }
            }

            used[source]   = true;
            records[count] = decoded_nodes[decoded].locator;
            copy_portable_position(&records[count++], *saved[source]);
            break;
        }
    }

    for (uint32_t source = 0; source < source_count; ++source) {
        if (saved[source] && ! used[source]) {
            return false;
        }
    }

    if (capacity < count || (! out_layout && count))
        return false;

    if (count)
        memcpy(out_layout, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

// The playback rank of an editor ordinal: the ordinal's slot's index
// among the wired slots, or pool_no_slot when no slot names it.  No
// existing function maps ordinals to playback indices.
static uint32_t playback_osc_index(const uint8_t source_row[Synth::max_layers], uint32_t ordinal)
{
    uint32_t rank = 0;

    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        if (source_row[slot] == 0) {
            continue;
        }
        if (source_row[slot] - 1 == ordinal) {
            return rank;
        }
        ++rank;
    }

    return pool_no_slot;
}

bool Sculptor::encode_instrument_graph_layout(const Synth::Instrument&           instr,
                                              const Synth::InstrumentBank&       bank,
                                              const Synth::InstrumentEditorBank& editor_bank,
                                              uint32_t                           channel,
                                              uint32_t                           zone,
                                              Synth::InstrumentGraphLayout*      out_records,
                                              uint32_t                           capacity,
                                              uint32_t*                          out_count,
                                              Synth::InstrumentJsonDomain        domain)
{
    if (! out_count || channel >= Synth::max_channels || zone >= Synth::max_instr_per_channel ||
        ! Synth::validate_instrument_bank(&bank) || ! Sculptor::validate_editor_metadata(editor_bank) ||
        ! layout_model_valid(instr,
                             bank.envelopes.entries,
                             bank.envelopes.num_allocated,
                             bank.lfos.entries,
                             bank.lfos.num_allocated)) {
        return false;
    }
    const Synth::Zone& entry = editor_bank.bank.channel_zones[channel][zone];
    if (! entry.start_note || entry.instrument >= editor_bank.bank.instruments.num_allocated) {
        return false;
    }
    // The clipboard carries the authoring instrument, which is exactly the
    // stored entry.  The portable domains encode the playback form the
    // caller compiled, which no longer equals the stored entry, so the
    // identity check is clipboard-only.
    if (domain == Synth::InstrumentJsonDomain::clipboard &&
        memcmp(&instr, &editor_bank.bank.instruments.entries[entry.instrument], sizeof(instr)) != 0) {
        return false;
    }
    // The portable domains translate the zone's records onto the playback
    // form: kind-0 records match through their ordinal's playback rank,
    // and records of unwired ordinals drop.  Generator records resolve
    // against the playback layers through their descriptor keys.
    const uint8_t*                      source_row = editor_bank.graph_osc_source[channel][zone];
    PortableNode* const                 nodes      = clipboard_decoded_nodes;
    const uint32_t                      node_count = derive_portable_nodes(instr, nodes);
    Synth::InstrumentGraphLayout* const records    = clipboard_portable_records;
    memset(records, 0, sizeof(clipboard_portable_records));
    uint32_t count                                 = 0;
    bool     represented[Synth::max_graph_records] = {};
    for (uint32_t index = 0; index < node_count; ++index) {
        const PortableNode& node  = nodes[index];
        uint32_t            saved = pool_no_slot;
        for (uint32_t record_index = 0; record_index < editor_bank.graph_layout_count; ++record_index) {
            const Synth::GraphNodeLayout& record = editor_bank.graph_layout[record_index];
            if (record.channel != channel || record.zone != zone || record.kind != node.locator.kind) {
                continue;
            }
            if (node.locator.kind == 0 && domain != Synth::InstrumentJsonDomain::clipboard &&
                node.index >= canonical_first_osc) {
                // The playback node's rank matches the record whose editor
                // ordinal maps to it; an unwired ordinal matches nothing.  The
                // fixed input/sum records (below canonical_first_osc) belong
                // to the fixed nodes and never match an oscillator node.
                if (record.index < canonical_first_osc) {
                    continue;
                }
                if (playback_osc_index(source_row, record.index - canonical_first_osc) !=
                    node.index - canonical_first_osc) {
                    continue;
                }
            }
            else if (record.index != node.index) {
                continue;
            }

            if (record.kind == 2 &&
                (record.depth_source != node.locator.depth_source || record.rate_source != node.locator.rate_source)) {
                continue;
            }
            saved = record_index;
            break;
        }

        if (saved == pool_no_slot)
            continue;
        const Synth::GraphNodeLayout& record = editor_bank.graph_layout[saved];
        if (represented[saved]) {
            return false;
        }
        represented[saved]             = true;
        records[count]                 = node.locator;
        records[count].x               = record.x;
        records[count].y               = record.y;
        records[count].width_override  = record.width_override;
        records[count].height_override = record.height_override;
        ++count;
    }
    for (uint32_t record = 0; record < editor_bank.graph_layout_count; ++record) {
        const Synth::GraphNodeLayout& saved = editor_bank.graph_layout[record];
        // Kinds 4 and 5 are bank-only records that never enter the portable
        // document: effect-graph nodes and the Instrument Parameters node's
        // position stay out of the clipboard/library format.
        if (saved.channel == channel && saved.zone == zone && saved.kind != 4 && saved.kind != 5 &&
            ! represented[record]) {
            // An unrepresented envelope/LFO record whose descriptor id is
            // allocated (1-based) is eligible for the exemption: it is
            // dropped, logged, instead of failing an export the graph UI
            // cannot repair.  Every other unrepresented record still fails,
            // except a kind-0 record of an unwired ordinal in the playback
            // domains: its node legitimately has no playback position.
            const bool allocated_modulator =
                (saved.kind == 1 && saved.index != 0 && saved.index <= bank.envelopes.num_allocated) ||
                (saved.kind == 2 && saved.index != 0 && saved.index <= bank.lfos.num_allocated);
            if (allocated_modulator) {
                d_printf("Ignored unresolvable graph layout record: kind %u descriptor %u\n",
                         static_cast<unsigned>(saved.kind),
                         static_cast<unsigned>(saved.index));
                continue;
            }
            if (saved.kind == 0 && domain != Synth::InstrumentJsonDomain::clipboard &&
                saved.index >= canonical_first_osc && saved.index < canonical_first_osc + Synth::max_layers &&
                playback_osc_index(source_row, saved.index - canonical_first_osc) == pool_no_slot) {
                continue;
            }
            return false;
        }
    }
    if (capacity < count || (! out_records && count)) {
        return false;
    }
    if (count)
        memcpy(out_records, records, count * sizeof(records[0]));
    *out_count = count;
    return true;
}

bool Sculptor::replace_zone_instrument_candidate(const Synth::InstrumentEditorBank&  source,
                                                 uint32_t                            channel,
                                                 uint32_t                            zone_entry,
                                                 const Synth::Instrument&            decoded_instrument,
                                                 const Synth::EnvelopeDescriptor*    decoded_envelopes,
                                                 uint32_t                            decoded_envelope_count,
                                                 const Synth::LFODescriptor*         decoded_lfos,
                                                 uint32_t                            decoded_lfo_count,
                                                 const Synth::InstrumentGraphLayout* decoded_layout,
                                                 uint32_t                            decoded_layout_count,
                                                 const uint8_t*                      decoded_osc_source,
                                                 Synth::InstrumentEditorBank*        out_candidate)
{
    if (! out_candidate || channel >= Synth::max_channels || zone_entry >= Synth::max_instr_per_channel ||
        decoded_envelope_count > Synth::instrument_max_envelopes || decoded_lfo_count > Synth::instrument_max_lfos ||
        ! Synth::validate_instrument_bank(&source.bank) || ! validate_editor_metadata(source)) {
        return false;
    }

    uint32_t mapped_count = 0;
    if (! map_instrument_graph_layout(decoded_instrument,
                                      decoded_envelopes,
                                      decoded_envelope_count,
                                      decoded_lfos,
                                      decoded_lfo_count,
                                      decoded_layout,
                                      decoded_layout_count,
                                      clipboard_candidate_records,
                                      Synth::instrument_graph_layout_capacity,
                                      &mapped_count)) {
        return false;
    }

    clipboard_candidate = source;

    // The pasted instrument's wired osc CC bits ride into the receiving
    // channel's shared registry mask; pre-existing destination bits survive
    // and the OR is idempotent for a repeated paste.
    clipboard_candidate.added_cc_mask[channel] = static_cast<uint16_t>(
        clipboard_candidate.added_cc_mask[channel] | Sculptor::instrument_cc_mask(decoded_instrument));

    Synth::InstrumentBank& bank    = clipboard_candidate.bank;
    const uint8_t          enabled = bank.channel_enabled[channel];
    bank.channel_enabled[channel]  = 1;

    if (! Synth::validate_instrument_bank(&bank))
        return false;

    const Synth::Zone& destination = bank.channel_zones[channel][zone_entry];
    if (! destination.start_note || destination.instrument >= bank.instruments.num_allocated)
        return false;

    const uint32_t original_slot = destination.instrument;
    memcpy(clipboard_destination_name,
           clipboard_candidate.instrument_names[original_slot],
           sizeof(clipboard_destination_name));

    uint32_t roots = 0;
    for (uint32_t root_channel = 0; root_channel < Synth::max_channels; ++root_channel) {
        if (! bank.channel_enabled[root_channel])
            continue;
        for (uint32_t entry = 0;
             entry < Synth::max_instr_per_channel && bank.channel_zones[root_channel][entry].start_note;
             ++entry) {
            if (bank.channel_zones[root_channel][entry].instrument == original_slot)
                ++roots;
        }
    }

    uint32_t placeholder_slot = original_slot;
    if (roots != 1) {
        placeholder_slot = bank.instruments.allocate();
        if (placeholder_slot == pool_no_slot)
            return false;
    }

    Synth::Instrument placeholder = bank.instruments.entries[original_slot];
    for (uint32_t layer = 0; layer < placeholder.layer_count; ++layer) {
        for (uint32_t target = 0; target < Synth::num_mod_targets; ++target) {
            placeholder.layers[layer].targets[target].envelope_desc_id = 0;
            placeholder.layers[layer].targets[target].lfo_desc_id      = 0;
        }
    }

    bank.instruments.entries[placeholder_slot] = placeholder;
    memcpy(clipboard_candidate.instrument_names[placeholder_slot],
           clipboard_destination_name,
           sizeof(clipboard_destination_name));
    bank.channel_zones[channel][zone_entry].instrument = static_cast<uint8_t>(placeholder_slot);

    zone_records_clear_zone(&clipboard_candidate, channel, zone_entry);

    // The row stays at the destination's previous (valid) state through
    // reclamation; the decoded row may name ordinals beyond the
    // placeholder's layer count, so installing it early would fail the
    // interim validation.
    if (! Synth::validate_instrument_bank(&bank) || ! validate_editor_metadata(clipboard_candidate))
        return false;

    Synth::reclaim_unused_slots(&clipboard_candidate);

    const uint32_t installed_slot = bank.channel_zones[channel][zone_entry].instrument;

    if (! Synth::remap_envelopes(&bank, decoded_envelopes, decoded_envelope_count, clipboard_envelope_ids) ||
        ! Synth::remap_lfos(&bank, decoded_lfos, decoded_lfo_count, clipboard_lfo_ids))
        return false;

    Synth::remap_instrument(decoded_instrument,
                            clipboard_envelope_ids,
                            clipboard_lfo_ids,
                            &bank.instruments.entries[installed_slot]);

    memcpy(clipboard_candidate.instrument_names[installed_slot],
           clipboard_destination_name,
           sizeof(clipboard_destination_name));

    // The paste installs the source row the clipboard document carried
    // together with the decoded instrument; a
    // document without osc_sources pastes identity wiring.  The row may
    // name ordinals beyond the placeholder's layer count, so both go in
    // before the final metadata validation.
    if (decoded_osc_source) {
        memcpy(clipboard_candidate.graph_osc_source[channel][zone_entry], decoded_osc_source, Synth::max_layers);
    }
    else {
        set_identity_osc_sources(&clipboard_candidate, channel, zone_entry);
    }

    if (! append_instrument_graph_layout(decoded_instrument,
                                         decoded_envelopes,
                                         decoded_envelope_count,
                                         decoded_lfos,
                                         decoded_lfo_count,
                                         decoded_layout,
                                         decoded_layout_count,
                                         clipboard_envelope_ids,
                                         clipboard_lfo_ids,
                                         channel,
                                         zone_entry,
                                         &clipboard_candidate))
        return false;

    bank.channel_enabled[channel] = enabled;

    if (! Synth::validate_instrument_bank(&bank) || ! validate_editor_metadata(clipboard_candidate))
        return false;

    *out_candidate = clipboard_candidate;
    return true;
}

bool Sculptor::append_instrument_graph_layout(const Synth::Instrument&            instrument,
                                              const Synth::EnvelopeDescriptor*    envelopes,
                                              uint32_t                            envelope_count,
                                              const Synth::LFODescriptor*         lfos,
                                              uint32_t                            lfo_count,
                                              const Synth::InstrumentGraphLayout* layout,
                                              uint32_t                            layout_count,
                                              const uint16_t*                     envelope_ids,
                                              const uint16_t*                     lfo_ids,
                                              uint32_t                            channel,
                                              uint32_t                            zone,
                                              Synth::InstrumentEditorBank*        candidate)
{
    if (! candidate || ! envelope_ids || ! lfo_ids || channel >= Synth::max_channels ||
        zone >= Synth::max_instr_per_channel || candidate->graph_layout_count > Synth::max_graph_records ||
        layout_count > Synth::max_graph_records - candidate->graph_layout_count)
        return false;
    uint32_t count = 0;
    if (! map_instrument_graph_layout(instrument,
                                      envelopes,
                                      envelope_count,
                                      lfos,
                                      lfo_count,
                                      layout,
                                      layout_count,
                                      clipboard_candidate_records,
                                      Synth::instrument_graph_layout_capacity,
                                      &count))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        Synth::GraphNodeLayout& record = clipboard_candidate_records[index];
        record.channel                 = static_cast<uint8_t>(channel);
        record.zone                    = static_cast<uint8_t>(zone);
        if (record.kind == 1) {
            if (! envelope_ids[record.index - 1] ||
                envelope_ids[record.index - 1] > candidate->bank.envelopes.num_allocated)
                return false;
            record.index = static_cast<uint8_t>(envelope_ids[record.index - 1]);
        }
        if (record.kind == 2) {
            if (! lfo_ids[record.index - 1] || lfo_ids[record.index - 1] > candidate->bank.lfos.num_allocated)
                return false;
            record.index = static_cast<uint8_t>(lfo_ids[record.index - 1]);
        }
    }
    memcpy(candidate->graph_layout + candidate->graph_layout_count,
           clipboard_candidate_records,
           count * sizeof(clipboard_candidate_records[0]));
    candidate->graph_layout_count += count;
    return true;
}
