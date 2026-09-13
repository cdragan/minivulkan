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

// Zone display name: instrument_names[instrument], or "Zone X" (X = zero-based zone index
// within the channel) when the instrument name is empty. Caller owns the buffer.
void get_zone_name(const InstrumentBank* bank, uint32_t channel, uint32_t zone_entry, char* out, uint32_t out_size);

// Full bank validation. Returns false and fills nothing on any violation: pool metadata
// bounds and DENSE pools, zone ordering/terminator conventions with reachable-reference checks,
// descriptor reference validity, envelope/LFO/oscillator invariants, enum/range checks.
bool validate_instrument_bank(const InstrumentBank* bank);

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
