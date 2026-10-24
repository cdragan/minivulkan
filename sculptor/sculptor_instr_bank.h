// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"

#include <atomic>

#include <stddef.h>
#include <stdint.h>

namespace Sculptor {

// Per-slot output connection flags plus the per-chain input routing, the
// editor-side routing model: an effect's output is disconnected, feeds the
// next occupied slot (the wire derives from adjacency) or feeds the graph
// output node. The channel input node's Out lands nowhere, on the first
// effect slot, or straight on the graph output node (the bypass wire: it
// overrides the effect wiring audibly while set, and severing it restores
// the wiring). Wires derive from these flags plus slot order at projection
// time; nothing is stored in the synth runtime.
enum EffectOut : uint8_t {
    effect_out_disconnected,
    effect_out_next,
    effect_out_output
};

enum EffectIn : uint8_t {
    effect_in_disconnected,
    effect_in_first_slot,
    effect_in_output
};

struct EffectChainRouting {
    uint8_t out[Synth::max_chain_effects]; // per-slot EffectOut
    uint8_t in;                            // channel input node, an EffectIn value
};

} // namespace Sculptor

namespace Synth {

struct InstrumentGraphLayout;

// One sparse per-zone node layout
// record.  An entry exists only for a node that was moved/resized (bound)
// or is detached; absence means the deterministic layout applies.
struct GraphNodeLayout {
    float   x;
    float   y;
    float   width_override;
    float   height_override;
    uint8_t channel;      // 0..15
    uint8_t zone;         // 0..15
    uint8_t kind;         // 0 = bound node (index = canonical 0..8),
                          // 1 = envelope instance, 2 = LFO instance
                          // (index = descriptor id 1..128),
                          // 4 = effect-graph node (name = node title, channel 16 = master chain),
                          // 5 = Instrument Parameters node (index and uid unused, 0)
    uint8_t index;        // canonical index or descriptor id
    uint8_t depth_source; // detached LFO only, else 0 (ModSource value)
    uint8_t rate_source;  // detached LFO only, else 0 (ModSource value)
    uint8_t uid;          // detached instance id, 0 for bound
    char    name[32];     // node title override, empty = derived name
};

static_assert(sizeof(GraphNodeLayout) == 56);

inline bool editor_record_kind_valid(uint8_t kind)
{
    return kind == 0 || kind == 1 || kind == 2 || kind == 4 || kind == 5;
}

constexpr uint32_t max_graph_records     = 2560; // global record cap
constexpr uint32_t max_detached_per_zone = 120;  // per-(channel, zone) detached cap

// Canonical bound-node numbering for kind-0 layout records, shared by the
// graph projection and the JSON codec: the inputs node 0, the sum node 1,
// oscillator layers 2..8. Generator instances are
// record-keyed (descriptor/target + uid), never canonical.
constexpr uint32_t graph_canonical_input_count      = 6; // MIDI source roles (ModSource except none)
constexpr uint32_t graph_canonical_input_node_count = 1; // all source roles share one node
constexpr uint32_t graph_canonical_first_osc        = graph_canonical_input_node_count + 1;
constexpr uint32_t graph_canonical_first_env        = graph_canonical_first_osc + max_layers;
constexpr uint32_t graph_canonical_node_count       = graph_canonical_first_env;
static_assert(graph_canonical_node_count == 9);

// Editor/dev-time bank utilities: display naming, validation (the editor validates before
// save/publish; the build embeds the validated image, so the player runtime never validates),
// and the GUI-to-audio bank swap queue.  None of this is linked into a minimal player.

// Instrument bank editable in GUI
struct InstrumentEditorBank {
    // Authoring payload collections, not playable chains: compile before publish.
    InstrumentBank               bank;
    Sculptor::EffectChainRouting effect_routing[max_channels + 1];
    char                         instrument_names[max_instruments][max_name_len];
    char                         channel_names[max_channels][max_name_len];
    // Editor-side per-zone wiring state, sparse with a global cap.
    GraphNodeLayout graph_layout[max_graph_records];
    uint32_t        graph_layout_count;
    // Per-zone oscillator wiring: output input slot s of zone (channel, zone)
    // is fed by oscillator node ordinal (entry - 1); 0 = unwired.  The rows
    // drive projection, the red-run rendering and the playback compile.
    uint8_t graph_osc_source[max_channels][max_instr_per_channel][Synth::max_layers];
    // Added-CC output dots per effect chain, bit i = CC (16 + i) is added on
    // the chain (channel index, or max_channels for the master chain).  A dot
    // exists only when its bit is set; wiring a CC never sets or clears bits.
    // Zero = no dot.  Self-validating: every 16-bit pattern names only valid CCs.
    // One spare entry keeps the struct free of tail padding: whole-bank
    // snapshots and comparisons stay stable across plain assignment, which
    // need not copy compiler padding.
    uint16_t added_cc_mask[max_channels + 2];
};

static_assert(std::is_trivially_copyable_v<InstrumentEditorBank>,
              "InstrumentEditorBank must be trivially copyable for byte-snapshot undo");

// Byte-snapshot undo and whole-bank test comparisons compare complete
// objects: plain assignment need not copy compiler tail padding, so the
// last member must end at the object boundary.

// InstrumentEditorBank is not standard-layout: InstrumentBank contains Pool
// members, and Pool inherits PoolBase privately.  offsetof stays valid for
// this use, so we suppress the offsetof warning here.  The assert must keep
// detecting tail padding after the last member.
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
#if defined(_MSC_VER) && ! defined(__clang__)
#    pragma warning(push)
#    pragma warning(disable : 4644) // offsetof used on non-standard-layout type
#endif
static_assert(offsetof(InstrumentEditorBank, added_cc_mask) + sizeof(InstrumentEditorBank::added_cc_mask) ==
                  sizeof(InstrumentEditorBank),
              "InstrumentEditorBank must not carry tail padding");
#if defined(_MSC_VER) && ! defined(__clang__)
#    pragma warning(pop)
#endif
#if defined(__clang__) || defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif

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
// Overload that reports the LFO renumbering it applied. old_to_new_lfo_ids may be
// null; when non-null it receives max_lfos entries, indexed by old zero-based LFO
// pool slot (old one-based id - 1), holding the survivor's one-based descriptor id
// and 0 for a freed or never-allocated slot.  It is exactly the map the surviving
// instruments and effect bindings are remapped with, so a caller can re-key
// external caches.  Effect LFO layout names are remapped by reclaim itself.
void reclaim_unused_slots(InstrumentEditorBank* bank, uint16_t* old_to_new_lfo_ids);

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

// Copies at most min(source_bound, destination_bound) bytes from source,
// stopping at the first NUL in the source; no terminator is written.  A
// source that fills its bound without a NUL is never read past the bound,
// so a full 32-byte record name copies safely.
inline void copy_name_bounded(char* destination, uint32_t destination_bound, const char* source, uint32_t source_bound)
{
    const uint32_t bound = source_bound < destination_bound ? source_bound : destination_bound;

    for (uint32_t i = 0; i < bound && source[i] != 0; ++i) {
        destination[i] = source[i];
    }
}

// Routing-flag validation: occupied slots contiguous from slot 0,
// empty slots carry effect_out_disconnected, effect_out_next only feeds an
// occupied successor, at most one effect_out_output holder, and
// in is effect_in_first_slot only on a chain with occupied slots; the
// effect_in_output bypass is legal on any chain, empty or wired.
bool validate_effect_routing(const Synth::EffectChainBinding& chain, const EffectChainRouting& routing);

// Terminal slot of the complete input-led path, or -1 when no complete
// input-led path exists (the input bypassed straight to the output node, no
// input wire, a broken run, or no output holder on the run's end).
int32_t effect_path_terminal(const Synth::EffectChainBinding& chain, const EffectChainRouting& routing);
// Success-only output: complete paths compile in traversal order; incomplete
// paths bypass dry. Off-path payloads never enter runtime chains.
bool compile_editor_playback_bank(const Synth::InstrumentEditorBank& source, Synth::InstrumentBank* out);

// One GUI-free paste transaction: installs a decoded clipboard instrument and its
// normalized layout over one zone of `source` and writes the result to
// `out_candidate`, which must not alias `source` (aliasing is a caller precondition
// violation, not a supported call). `source` is only read. A destination channel
// that is disabled is enabled privately while its zone table is validated and
// instrument roots are counted with the reclaim rule, then the original enabled
// flag is restored. Exactly one root reuses its own instrument slot; a shared
// destination needs a separate free or unallocated slot obtainable BEFORE reclaim
// (an allocated-but-unrooted slot is not free). Only the destination zone's
// non-effect layout records and its source row are replaced; effect records,
// other zones and every other root are preserved. Invalid source metadata, an
// invalid destination table or an unobtainable placeholder returns false with
// `out_candidate` untouched.
bool replace_zone_instrument_candidate(const Synth::InstrumentEditorBank&  source,
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
                                       Synth::InstrumentEditorBank*        out_candidate);

// Kind-4 names must be terminated; only kind-4 names participate in identity.
bool graph_layout_record_identity_equal(const Synth::GraphNodeLayout& a, const Synth::GraphNodeLayout& b);

// Editor graph metadata: record accounting and validation shared by the JSON
// codec and the editor's candidate commit.  Defined in sculptor_instr_bank.cpp
// so every binary that links the editor bank links its validation too.
// validate_editor_metadata: record bounds, descriptor ids within their pools,
// per-zone and global record caps, duplicate record keys, per-zone source-row
// values (each within the instrument's layer count; a repeated value is
// accepted and the compile copies the referenced layer into every wired slot
// in slot order), and the implied projected node count per zone within the
// node pool.
bool validate_editor_metadata(const Synth::InstrumentEditorBank& bank);
// Number of detached (kind 1/2) records stored for one zone.
uint32_t count_detached_records(const Synth::InstrumentEditorBank& bank, uint32_t channel, uint32_t zone);
// Whole-bank effect budget accounting (modulated-parameter count and static
// enabled-state bytes), matching the bank validator's counting exactly.
void count_effect_budgets(const Synth::InstrumentBank& bank, uint32_t* num_modulated, uint32_t* state_bytes);

} // namespace Sculptor
