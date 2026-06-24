// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_instrument.h"

#include <stdint.h>

namespace Synth {

// Fixed header preceding a serialized instrument bank: marker + version + payload size.
constexpr uint32_t instrument_bank_header_size = 10;

// Size of a complete encoded bank image (header + payload).
// TODO: the bank is stored as a raw struct image (easy to load, not compact).
// Once real instruments exist, research a compact
// layout (skip empty pool slots, group fields) and measure candidates with ~/minify.
constexpr uint32_t instrument_bank_image_size =
    instrument_bank_header_size + static_cast<uint32_t>(sizeof(InstrumentBank));

// Encodes a bank into dest.  Returns bytes written, or 0 if dest_size is too small.
uint32_t encode_instrument_bank(const InstrumentBank* bank, uint8_t* dest, uint32_t dest_size);

// Validates an encoded image and reconstructs the bank.  Returns false on bad marker, version,
// size mismatch or truncated input.
bool decode_instrument_bank(const uint8_t* src, uint32_t src_size, InstrumentBank* bank);

// File persistence (whole-bank file).
bool save_instrument_bank(const char* path, const InstrumentBank* bank);
bool load_instrument_bank(const char* path, InstrumentBank* bank);

} // namespace Synth
