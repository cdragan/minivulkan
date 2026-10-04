// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"

#include <atomic>

#include <stdint.h>

namespace Synth {

// One sparse per-zone node layout
// record.  An entry exists only for a node that was moved/resized (bound)
// or is detached; absence means the deterministic layout applies.
struct GraphNodeLayout {
    float   x, y, width_override, height_override;
    uint8_t channel;      // 0..15
    uint8_t zone;         // 0..15
    uint8_t kind;         // 0 = bound node (index = canonical 0..13),
                          // 1 = envelope instance, 2 = LFO instance
                          // (index = descriptor id 1..128),
                          // 3 = parameter (index = dynamic target 0..4),
                          // 4 = effect-graph node (name = node title, channel 16 = master chain)
    uint8_t index;        // canonical index or descriptor id
    uint8_t depth_source; // detached LFO only, else 0 (ModSource value)
    uint8_t rate_source;  // detached LFO only, else 0 (ModSource value)
    uint8_t uid;          // detached instance id, 0 for bound
    // Kind-3 partial-wiring persistence (zero = not stored): a wired
    // constant parameter compiles to a zero generator cell that the
    // projection cannot distinguish from unwired, so the in-progress
    // wiring survives re-projection only through these fields.  served
    // bit i means the parameter value-wires oscillator layer i;
    // env_desc_id/lfo_desc_id name the instances wired into the
    // parameter's envelope/LFO inputs; the lfo source pair completes the
    // kind-2 instance key that identifies the LFO; name overrides the
    // derived node title.  Kind 0/1/2 records must keep all of them zero.
    uint8_t served;
    uint8_t env_desc_id;
    uint8_t lfo_desc_id;
    uint8_t lfo_depth_source; // ModSource value
    uint8_t lfo_rate_source;  // ModSource value
                              // Kind-3 record pairing: how the record binds to the target's derived
                              // parameters at projection. 1..254 = binds to the k-th derived
                              // same-target parameter in layer order; reconcile_bindings re-stamps
                              // these ordinals to the current derived order whenever the graph
                              // re-derives. 0 and 255 = the record joins the target's uid-order
                              // positional attach over the groups left unclaimed, and only
                              // materializes its own node when no eligible group remains; a record
                              // carrying stored generator state binds only a group carrying the
                              // same tuple (in either pass), and a tuple-less record carrying
                              // persisted value wires binds only a tuple-less carrier group. An
                              // explicit ordinal beyond the target's group count makes the
                              // projection distrust the explicit pass wholesale and attach every
                              // record positionally. Kinds 0/1/2 must keep zero.
    uint8_t param_slot;
    char    name[32]; // node title override, empty = derived name
};

static_assert(sizeof(GraphNodeLayout) == 64);
// param_slot value for a record outside the explicit ordinal scheme (added
// parameters, retargeted parameters, edge-loss detach): it joins the
// target's uid-order positional attach over unclaimed groups and
// materializes a surplus node when no eligible group remains.
constexpr uint8_t graph_record_param_free = 255;

constexpr uint32_t max_graph_records     = 1280; // global record cap
constexpr uint32_t max_detached_per_zone = 120;  // per-(channel, zone) detached cap

// Canonical bound-node numbering for kind-0 layout records, shared by the
// graph projection and the JSON codec: the inputs node 0, the sum node 1,
// oscillator layers 2..8. Parameters and generator instances are
// record-keyed (descriptor/target + uid), never canonical.
constexpr uint32_t graph_canonical_input_count      = 6; // MIDI source roles (ModSource except none)
constexpr uint32_t graph_canonical_input_node_count = 1; // all source roles share one node
constexpr uint32_t graph_canonical_first_osc        = graph_canonical_input_node_count + 1;
constexpr uint32_t graph_canonical_first_env        = graph_canonical_first_osc + max_layers;
constexpr uint32_t graph_canonical_node_count       = graph_canonical_first_env;
static_assert(graph_canonical_node_count == 9);

// The five targets the runtime can modulate per oscillator; duty A/B, osc mix
// and FM index are per-oscillator constants carried as base values only.
constexpr bool graph_target_projected(ModTarget target)
{
    return target <= mod_panning || target >= mod_lowpass_cutoff;
}

// Inverse of the projected index order: volume, pitch and panning take
// projected indices 0..2, then lowpass and highpass take 3..4, skipping the
// four constant targets (duty A, duty B, osc mix, FM index) that sit between
// them in ModTarget order.
constexpr ModTarget graph_projected_target(uint32_t projected_index)
{
    return static_cast<ModTarget>(projected_index < 3 ? projected_index : projected_index + 4u);
}

// Editor/dev-time bank utilities: display naming, validation (the editor validates before
// save/publish; the build embeds the validated image, so the player runtime never validates),
// and the GUI-to-audio bank swap queue.  None of this is linked into a minimal player.

// Instrument bank editable in GUI
struct InstrumentEditorBank {
    InstrumentBank bank;
    char           instrument_names[max_instruments][max_name_len];
    char           channel_names[max_channels][max_name_len];
    // Editor-side per-zone graph state, sparse with a global cap.
    GraphNodeLayout graph_layout[max_graph_records];
    uint32_t        graph_layout_count;
    uint8_t graph_missing_sum[max_channels][max_instr_per_channel]; // [channel][zone], one bit per oscillator layer
};

static_assert(std::is_trivially_copyable_v<InstrumentEditorBank>,
              "InstrumentEditorBank must be trivially copyable for byte-snapshot undo");

// Factory default channel names ("Channel 01".."Channel 16", channel 10 = "Drum Track").
extern const char default_channel_names[max_channels][max_name_len];

// Zone display name: instrument_names[instrument], or "Zone X" (X = zero-based zone index
// within the channel) when the instrument name is empty.  Caller owns the buffer.
// Default channel/instrument setup for a fresh project: a bare instrument (one
// sine layer in blend mode, neutral volume/panning bases, no envelopes, LFOs,
// generator bindings, MIDI routing inputs or channel effect chain).
bool init_default_channel(InstrumentBank* bank, uint32_t channel);
void init_default_bank(InstrumentBank* bank);

void get_zone_name(const InstrumentEditorBank* bank,
                   uint32_t                    channel,
                   uint32_t                    zone_entry,
                   char*                       out,
                   uint32_t                    out_size);

// Keyboard zone table operations.  A table is sorted by start_note and terminated
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
bool zone_split_new(Zone* zones, uint32_t i, uint32_t note, InstrumentEditorBank* bank);

// Copies the factory default name of a channel (including "Drum Track" for the drum
// track channel) into out, NUL-terminated; no-op when the channel is out of range.
void get_default_channel_name(uint32_t channel, char* out, uint32_t out_size);

// Full bank validation.  Returns false and fills nothing on any violation: pool metadata
// bounds and DENSE pools, zone ordering/terminator conventions with reachable-reference checks,
// descriptor reference validity, envelope/LFO/oscillator invariants, enum/range checks.
// check_lfo_waves=false only defers the LFO-wave check to the caller: the
// decode path uses it so a bank decodes before editor metadata validation,
// while the editor's load path revalidates strictly, so unsupported waves
// are still refused before anything reaches the runtime.
bool validate_instrument_bank(const InstrumentBank* bank, bool check_lfo_waves = true);

// Reclaims pool slots no live reference can reach: instruments referenced by no enabled
// channel's zone table are removed, then descriptors referenced by no surviving instrument
// and no effect chain (any channel or the master) are removed.  Survivors compact to the
// dense prefix; every zone table and every descriptor reference is remapped in the same
// pass, and instrument_names move with their instruments.  The bank must be valid on entry
// (descriptor ids in bounds); the result is valid whenever the input was.
void reclaim_unused_slots(InstrumentEditorBank* bank);

// Fixed-depth SPSC bank-swap queue (producer: GUI thread, consumer: the app's audio-step hook).
// Packets hold a complete, self-consistent bank; the consumer copies it over the runtime bank
// between steps.  Room iff tail - head < capacity (unsigned bounded distance).
// Ordering: producer - load own tail relaxed, load head ACQUIRE (slot reuse), write packet,
// store tail RELEASE. Consumer peek - load own head relaxed, load tail ACQUIRE (packet writes
// visible), read slot; consume - store head+1 RELEASE only AFTER the bank copy fully applied.
constexpr uint32_t bank_queue_capacity = 2;

struct BankUpdateQueue {
    InstrumentBank        packets[bank_queue_capacity];
    std::atomic<uint32_t> head{ 0 }; // consumer position (monotonic)
    std::atomic<uint32_t> tail{ 0 }; // producer position (monotonic)
};

// Returns the next undrained bank without committing it (nullptr when empty).
const InstrumentBank* peek_bank_update(BankUpdateQueue* queue);

// Commits the head bank.  The consumer calls this ONLY after it fully copied the bank.
void consume_bank_update(BankUpdateQueue* queue);

// Producer side: appends one complete bank.  Returns false when the queue is full (the
// producer keeps its pending copy and retries later).
bool push_bank_update(BankUpdateQueue* queue, const InstrumentBank& bank);

} // namespace Synth

namespace Sculptor {

// Editor graph metadata: record accounting and validation shared by the JSON
// codec and the editor's candidate commit.  Defined in sculptor_instr_bank.cpp
// so every binary that links the editor bank links its validation too.
// validate_editor_metadata: record bounds, descriptor ids within their pools,
// per-zone and global record caps, duplicate record keys, missing-sum bits
// within each zone's layer count, and the implied projected node count per
// zone within the node pool.
bool validate_editor_metadata(const Synth::InstrumentEditorBank& bank);
// Number of detached (kind 1/2/3) records stored for one zone.
uint32_t count_detached_records(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone);
// Whole-bank effect budget accounting (modulated-parameter count and static
// enabled-state bytes), matching the bank validator's counting exactly.
void count_effect_budgets(const Synth::InstrumentBank& bank, uint32_t* num_modulated, uint32_t* state_bytes);

} // namespace Sculptor
