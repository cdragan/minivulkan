// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"

#include <atomic>

#include <stdint.h>

namespace Synth {

// Editor/dev-time bank utilities: display naming, validation (the editor validates before
// save/publish; the build embeds the validated image, so the player runtime never validates),
// and the GUI-to-audio bank swap queue. None of this is linked into a minimal player.

// Instrument bank editable in GUI
struct InstrumentEditorBank {
    InstrumentBank bank;
    char           instrument_names[max_instruments][max_name_len];
    char           channel_names[max_channels][max_name_len];
};

static_assert(std::is_trivially_copyable_v<InstrumentEditorBank>,
              "InstrumentEditorBank must be trivially copyable for byte-snapshot undo");

// Factory default channel names ("Channel 01".."Channel 16", channel 10 = "Drum Track").
extern const char default_channel_names[max_channels][max_name_len];

// Zone display name: instrument_names[instrument], or "Zone X" (X = zero-based zone index
// within the channel) when the instrument name is empty. Caller owns the buffer.
// Default channel/instrument setup for a fresh project: the factory recipe
// instrument with its envelope, LFOs and demo effect chains.
bool init_default_channel(InstrumentBank* bank, uint32_t channel);
void init_default_bank(InstrumentBank* bank);

void get_zone_name(const InstrumentEditorBank* bank, uint32_t channel, uint32_t zone_entry, char* out, uint32_t out_size);

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

// Full bank validation. Returns false and fills nothing on any violation: pool metadata
// bounds and DENSE pools, zone ordering/terminator conventions with reachable-reference checks,
// descriptor reference validity, envelope/LFO/oscillator invariants, enum/range checks.
bool validate_instrument_bank(const InstrumentBank* bank);

// Reclaims pool slots no live reference can reach: instruments referenced by no enabled
// channel's zone table are removed, then descriptors referenced by no surviving instrument
// and no effect chain (any channel or the master) are removed.  Survivors compact to the
// dense prefix; every zone table and every descriptor reference is remapped in the same
// pass, and instrument_names move with their instruments.  The bank must be valid on entry
// (descriptor ids in bounds); the result is valid whenever the input was.
void reclaim_unused_slots(InstrumentEditorBank* bank);

// Fixed-depth SPSC bank-swap queue (producer: GUI thread, consumer: the app's audio-step hook).
// Packets hold a complete, self-consistent bank; the consumer copies it over the runtime bank
// between steps. Room iff tail - head < capacity (unsigned bounded distance).
// Ordering: producer - load own tail relaxed, load head ACQUIRE (slot reuse), write packet,
// store tail RELEASE. Consumer peek - load own head relaxed, load tail ACQUIRE (packet writes
// visible), read slot; consume - store head+1 RELEASE only AFTER the bank copy fully applied.
constexpr uint32_t bank_queue_capacity = 2;

struct BankUpdateQueue {
    InstrumentBank           packets[bank_queue_capacity];
    std::atomic<uint32_t>    head{0}; // consumer position (monotonic)
    std::atomic<uint32_t>    tail{0}; // producer position (monotonic)
};

// Returns the next undrained bank without committing it (nullptr when empty).
const InstrumentBank* peek_bank_update(BankUpdateQueue* queue);

// Commits the head bank. The consumer calls this ONLY after it fully copied the bank.
void consume_bank_update(BankUpdateQueue* queue);

// Producer side: appends one complete bank. Returns false when the queue is full (the
// producer keeps its pending copy and retries later).
bool push_bank_update(BankUpdateQueue* queue, const InstrumentBank& bank);

} // namespace Synth
