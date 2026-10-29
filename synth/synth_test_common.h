// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#ifdef _MSC_VER
#    pragma warning(disable : 4996)
#endif

#include "../core/rng.h"
#include "../core/vecfloat.h" // vmath::sincos for the LFO reference formula below
#include "../core/vmath.h"
#include "../sculptor/sculptor_bank_json.h"
#include "../sculptor/sculptor_effect_graph.h"
#include "../sculptor/sculptor_graph.h"
#include "../sculptor/sculptor_instr_bank.h"
#include "../sculptor/sculptor_instr_edit.h"
#include "../sculptor/sculptor_instr_envelope_edit.h"
#include "../sculptor/sculptor_instr_library.h"
#include "../sculptor/sculptor_osc_graph.h"
#include "../sculptor/sculptor_undo.h"
#include "midi_file.h"
#include "midi_input.h"
#include "synth_effect_expansion.h"
#include "synth_effects.h"
#include "synth_instrument.h"
#include "synth_midi_cc.h"
#include "synth_parameters.h"
#include "synth_soundtrack.h"
#include <cmath>
#include <float.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#define TEST(test)                         \
    if (! (test)) {                        \
        failed(#test, __FILE__, __LINE__); \
    }

// Heap storage for test scratch arrays. sizeof() and array decay keep working
// on the elements member, and make_unique value-initializes the array like the
// zero-initialized static it replaces.
template <typename Element, uint32_t count> struct FixedArray {
    Element elements[count];
};

// The legacy raw-seam blocks start from a plain bank: give the projection
// the serial routing the v1 migration would install, so its derived wires
// match the old implicit-serial expectations.
void implicit_serial_routing(Synth::InstrumentEditorBank& bank, uint32_t owner);
bool project_legacy_effects(const Synth::InstrumentBank&  bank,
                            uint32_t                      owner,
                            Sculptor::Graph*              graph,
                            Sculptor::EffectGraphMapping* mapping,
                            const bool*                   pins = nullptr);

// Fixed portable-record bound: the nine canonical nodes plus one envelope
// and one LFO per (layer,target) cell.
inline constexpr uint32_t max_portable_records =
    Synth::graph_canonical_node_count + 2u * Synth::max_layers * Synth::num_mod_targets;

inline constexpr uint32_t clipboard_token_bound = 32768;

inline constexpr uint32_t fake_region_base = 8192;

// The editor's clipboard staging capacity. The copy path encodes into
// capacity - 1 and terminates the document itself, so a document of L payload
// bytes needs capacity >= L + 1. This mirrors the editor's staging buffer in
// sculptor_instr_edit.cpp; a larger document must raise that buffer, never
// truncate the document.
inline constexpr uint32_t clipboard_capacity = 256 * 1024;

// Every output of one clipboard decode, so a rejected document can be shown to
// have left all of them - both descriptor arrays and all three counts -
// byte-identical.
struct ClipboardDecode {
    Synth::Instrument            instr;
    Synth::EnvelopeDescriptor    envs[Synth::instrument_max_envelopes];
    Synth::LFODescriptor         lfos[Synth::instrument_max_lfos];
    Synth::InstrumentGraphLayout layout[max_portable_records];
    uint32_t                     env_count;
    uint32_t                     lfo_count;
    uint32_t                     layout_count;
};

struct FakeWriter {
    uint32_t next_node;
    uint32_t dest_count;
    uint32_t lfo_count;
    uint16_t last_dest_node;
    uint16_t last_dest_lfo_node;
    uint16_t last_dest_source_param_id;

    struct Resolved {
        Synth::ModSource source;
        uint8_t          cc_number;
        uint32_t         chain;
    };

    Resolved resolved[32];
    uint32_t num_resolved;
};

void failed(const char* test, const char* file, int line);
bool approx(float left, float right, float eps);
// True for the four targets these fixtures wire modulators and MIDI
// inputs onto (volume, pitch, panning, FIR spread). All twelve targets
// are uniformly modulate-able; the remaining eight (FM ratio, pad tone
// A/B, duty A/B, osc mix, FM depth and FIR slope) keep base-only
// bindings here purely to bound fixture size.
bool osc_graph_target(Synth::ModTarget target);

// The connection terminating at an input endpoint, or pool_no_slot.
uint32_t find_osc_graph_connection(const Sculptor::Graph& graph, const Sculptor::EndPoint& input);
// The band index (0..3) of a band-parameter target, -1 otherwise: the
// test-side mirror of the production band_target mapping (band 0 keeps
// the legacy spread/slope targets 7 and 11).
int32_t osc_target_band_of_target(const uint32_t target);
// Checks via TEST that a projection filled the mapping consistently: every
// fixed node exists and is distinct, each occupied layer's oscillator has
// exactly 15 rows plus its target blocks, and the instance registry
// carries exactly env_instances + lfo_instances instances. The distinct
// node total is 3 + layer_count + env_instances + lfo_instances (the
// inputs node + the instrument parameters node + sum + oscillators +
// generator instances).
void check_osc_graph_mapping(const Sculptor::Graph&           graph,
                             const Sculptor::OscGraphMapping& mapping,
                             uint32_t                         layer_count,
                             uint32_t                         env_instances,
                             uint32_t                         lfo_instances,
                             uint32_t                         band_count = 1);
// Finds a slot by name on a node; slots are scanned in creation order.

bool find_graph_slot(const Sculptor::Graph& graph, uint32_t node_idx, const char* name, uint32_t* slot_idx);

// Tooltip sweep: walks every occupied node and slot of one projected
// graph and counts violations.  Exempt rows must carry nullptr; every
// other slot must carry a non-empty string.  Every shared row (including
// the unlabeled Op lists and envelope-depth slider) carries its own
// tooltip, so only node-specific rows go through the caller predicate.
using TooltipExempt = bool (*)(uint32_t node_idx, const Sculptor::Slot& slot, void* user_data);
// Connection matching both endpoints: fx reference wires land on rows that
// also carry property state, so endpoint identity alone is the test key.
uint32_t find_fx_graph_connection(const Sculptor::Graph&    graph,
                                  const Sculptor::EndPoint& output,
                                  const Sculptor::EndPoint& input);
// Registry index of the instance projected onto `node_idx`, or -1.
int32_t find_instance_by_node(const Sculptor::OscGraphMapping& mapping, uint32_t node_idx);
// Fixture helper: the fixtures seed per-layer generator variety
// explicitly; this copies only layer 0's base value and MIDI inputs onto
// every live layer, so the MIDI routing is uniform while the generators
// stay per-layer (the per-layer format round-trips uniform instruments
// unchanged; nothing fans anything out at runtime).
void fan_out_routing(Synth::Instrument& instrument);
// Zeroed instrument memory carries no dispersion section count; the validator requires an integer 1..6, so
// fixtures that build instruments from raw memory restore the default four.
void set_default_dispersion_sections(Synth::Instrument& instrument);
// Builds a max-complexity projection fixture: 7 layers, an envelope and an LFO
// bound on every layer for each of the five projected targets, full two-input
// routing with mixed ops and scales (volume repeats velocity across its two
// ordered inputs), nonzero skews, and one LFO descriptor aliased across two
// layers.  Writes the referenced 1-based descriptor ids (0 = unbound), indexed
// [layer][Synth::ModTarget], to env_ids/lfo_ids.
void build_osc_graph_max_fixture(Synth::InstrumentBank* bank,
                                 Synth::Instrument*     instrument,
                                 uint16_t (*env_ids)[Synth::num_mod_targets],
                                 uint16_t (*lfo_ids)[Synth::num_mod_targets]);
// Builds a minimal projection fixture with an aliased envelope pair: one layer,
// volume bound to an envelope and an LFO, pitch bound to an envelope sharing the
// volume envelope's descriptor id.  The LFO's depth/rate sources stay unset so
// the LFO node's source connectors are free for validator connection attempts.
// Writes the pitch envelope id, the LFO id and the volume envelope id (all 1-based).
void build_osc_graph_small_fixture(Synth::InstrumentBank* bank,
                                   Synth::Instrument*     instrument,
                                   uint16_t*              shared_env_id,
                                   uint16_t*              lfo_id,
                                   uint16_t*              volume_env_id);

const Synth::EffectNodeWriter fake_writer_binding(FakeWriter& writer);
Synth::EffectSlotBinding*     enabled_effect(Synth::InstrumentBank& bank,
                                             uint32_t               channel,
                                             uint32_t               slot,
                                             Synth::EffectType      type);
// ---- Instrument graph editor test helpers ----

// Appends one detached-node layout record to the bank's editor state.
void add_detached_record(Synth::InstrumentEditorBank* bank,
                         uint32_t                     channel,
                         uint32_t                     zone,
                         uint32_t                     kind,
                         uint32_t                     desc_id,
                         float                        x,
                         float                        y,
                         Synth::ModSource             depth_source,
                         Synth::ModSource             rate_source,
                         uint8_t                      uid);
// Builds an editor bank whose channel 0 zone 0 holds the small projection
// fixture instrument (bound envelope + LFO on volume, envelope on pitch) with
// a valid zone table; no layout records yet.
void build_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument);
// Allocates one detached LFO descriptor (sine, 300 ms) in the bank and returns
// its 1-based id.
uint16_t add_detached_lfo_descriptor(Synth::InstrumentEditorBank* bank);
// Number of layout records matching one (channel, zone, kind, index) key.
uint32_t count_records_matching(const Synth::InstrumentEditorBank& bank,
                                uint32_t                           channel,
                                uint32_t                           zone,
                                uint32_t                           kind,
                                uint32_t                           index);
// Encodes `bank`, splices `"editor":<editor_json>` in as a proper key-value
// pair before the final close brace, and decodes the result into *out.
// `editor_json` is the editor object's inner value, e.g. {"layouts":[...]}.
bool decode_bank_with_editor_section(const Synth::InstrumentEditorBank& bank,
                                     const char*                        editor_json,
                                     Synth::InstrumentEditorBank*       out);
// Channel 0 zone 0 binding fixture: three layers; volume is bound on
// layers 0 and 2 with one shared envelope + LFO tuple (depth source
// velocity, rate source mod wheel) and on layer 1 with a second envelope
// descriptor (different tuple); pitch is bound on layer 0 with that
// second envelope; layer 1/2 pitch stay unbound constants. Volume
// routing carries one velocity input. No layout records.
void build_binding_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument);
// ---------------------------------------------------------------------------
// Clipboard portable layout records: fixtures, measurement and comparison.
// ---------------------------------------------------------------------------

// Real parser token count of one document under the vendored parser the codec
// uses. Returns -1 on any parse failure, including a document needing more tokens
// than the bound: it reports "cannot be measured within the bound", never a
// partial count.
int32_t count_clipboard_tokens(const char* text, uint32_t len);
void    fill_clipboard_sentinels(ClipboardDecode* out);
bool    clipboard_sentinels_intact(const ClipboardDecode* a, const ClipboardDecode* b);
// Decodes `doc` into every tracked output with `layout_capacity` slots of layout
// output.
bool decode_clipboard(ClipboardDecode* out, const char* doc, uint32_t layout_capacity);
// One zone of one channel: a single layer with a distinct envelope and a distinct
// LFO on volume (depth from velocity, rate from mod wheel) and a second distinct
// envelope on pitch, so the instrument projects canonical, envelope and LFO
// nodes with pairwise distinct identities. No layout records, no
// effect chain.
void     build_clipboard_zone_fixture(Synth::InstrumentEditorBank* bank, Synth::Instrument* instrument);
bool     export_clipboard_fixture(const Synth::InstrumentEditorBank& bank, char* text, uint32_t capacity);
void     write_library_fixture(const char* path,
                               const char* payload,
                               uint32_t    length,
                               const char* category = "Regression",
                               const char* name     = "FM");
uint32_t read_library_fixture(const char* path, char* bytes, uint32_t capacity);
void     build_named_fm_fixture(Synth::InstrumentEditorBank* bank);
// Replaces the single occurrence of `needle` in `payload` (length `len`)
// with `replacement`, writing the patched document to `out`.  Returns the
// patched length, or 0 when the needle does not occur exactly once or the
// patched document does not fit.  Used to shape hand-decoded routing
// variants the encoder itself refuses to emit.
uint32_t splice_single_occurrence(const char* payload,
                                  uint32_t    len,
                                  const char* needle,
                                  const char* replacement,
                                  char*       out,
                                  uint32_t    capacity);
void     effects_clipboard_fixture(Synth::EffectsDocument* doc, bool midi);
// One enabled delay effect on the chain a CC fixture builds on.  chain is a
// channel index or Sculptor::fx_master_chain.
Synth::EffectChainBinding& add_cc_chain_fixture(Synth::InstrumentEditorBank* bank, uint32_t chain);
// Direct per-connection modulator rows: every oscillator node keeps its
// 18 fixed slots and adds one 12-slot block per ModTarget directly after
// its value row (block_base(t) = osc_target_row_slot[t] + 1); field 0 is
// the env dot, 1 the env depth, 2 the LFO dot, 3 the LFO op, 4 the LFO
// depth, 5 the rate scale, 6 and 9 the source dots, 7/8 and 10/11 the
// op/amount widgets; row groups are 3*t+1..4, the env row 3*t+4 packing
// the dot and its unlabeled depth on one line.  Row visibility is the live predicate
// osc_target_field_hidden(graph, node, target, field).

// Index of the first projected detached generator matching (kind, desc_id),
// or -1.  kind is 1 = envelope, 2 = LFO; desc_id is the 1-based pool id.
int32_t find_detached_by_desc(const Sculptor::OscGraphMapping& mapping, uint32_t kind, uint32_t desc_id);
// Rebuilds the graph from the bank's current state and drains the projection
// changes; false skips the caller's block.
bool reproject_m2_band_graph(Synth::InstrumentEditorBank* bank,
                             Sculptor::Graph*             graph,
                             Sculptor::OscGraphMapping*   mapping);
// Stages one oscillator slot edit on the graph, drains it and applies the
// value change to the bank; the click-style band slots ride this path.
bool apply_m2_slot_edit(Synth::InstrumentEditorBank*  bank,
                        Sculptor::Graph*              graph,
                        Sculptor::OscGraphMapping*    mapping,
                        const uint32_t                node_idx,
                        const uint32_t                slot_idx,
                        const Sculptor::PropertyValue value);
// realtime_synth.cpp is not linked into this unit.  pump_live_midi in
// midi_input.cpp calls apply_midi_event, so this no-op stub swallows the
// drained events.
void Synth::apply_midi_event(const Synth::MidiEvent&);
void make_valid_bank(Synth::InstrumentBank& bank);

static_assert(max_portable_records == Synth::instrument_graph_layout_capacity);
static_assert(max_portable_records == 415);
int  test_exit_code();
void test_silence_stdout();
