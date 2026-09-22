// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan
#pragma once

#include "synth_instrument.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Synth {

// Fixed header preceding a serialized instrument bank: marker + version + payload size.
constexpr uint32_t instrument_bank_header_size = 10;

// TODO: the bank is stored as a raw struct image (easy to load, not compact).
// Once real instruments exist, research a compact
// layout (skip empty pool slots, group fields) and measure candidates with ~/minify.

// Marker and version preceding the raw bank image.  Bump the version whenever the
// serialized bank layout changes, so a stale file is rejected rather than misread.
// Marker: SYnth Instrument Bank
constexpr uint8_t bank_marker[4] = { 'S', 'Y', 'I', 'B' };
constexpr uint16_t bank_version = 4;

// Size of a complete encoded bank image (header + payload), per bank type: the
// editor serializes its whole bank (names included), the plain runtime bank
// serializes without names.
template <typename Bank>
constexpr uint32_t instrument_bank_image_size = instrument_bank_header_size + static_cast<uint32_t>(sizeof(Bank));

// Encodes a bank into dest.  Returns bytes written, or 0 if dest_size is too small.
template <typename Bank>
uint32_t encode_instrument_bank(const Bank* bank, uint8_t* dest, uint32_t dest_size)
{
    constexpr uint32_t image_size = instrument_bank_image_size<Bank>;
    if (dest_size < image_size) {
        return 0;
    }

    memcpy(dest, bank_marker, sizeof(bank_marker));
    memcpy(dest + 4, &bank_version, sizeof(bank_version));
    constexpr uint32_t bank_payload_size = static_cast<uint32_t>(sizeof(Bank));
    memcpy(dest + 6, &bank_payload_size, sizeof(bank_payload_size));
    memcpy(dest + instrument_bank_header_size, bank, sizeof(*bank));

    return image_size;
}

// Validates an encoded image and reconstructs the bank.  Returns false on bad
// marker, version, size mismatch or truncated input.
template <typename Bank>
bool decode_instrument_bank(const uint8_t* src, uint32_t src_size, Bank* bank)
{
    constexpr uint32_t image_size = instrument_bank_image_size<Bank>;
    if (src_size < image_size) {
        return false;
    }

    if (memcmp(src, bank_marker, sizeof(bank_marker)) != 0) {
        return false;
    }

    uint16_t version;
    memcpy(&version, src + 4, sizeof(version));
    if (version != bank_version) {
        return false;
    }

    uint32_t payload_size;
    memcpy(&payload_size, src + 6, sizeof(payload_size));
    if (payload_size != static_cast<uint32_t>(sizeof(Bank))) {
        return false;
    }

    memcpy(bank, src + instrument_bank_header_size, sizeof(*bank));

    return true;
}

// File persistence (whole-bank file).  Scratch buffer lives in static store.
template <typename Bank>
bool save_instrument_bank(const char* path, const Bank* bank)
{
    static uint8_t serialize_image[instrument_bank_image_size<Bank>];

    const uint32_t image_size = encode_instrument_bank(bank, serialize_image, sizeof(serialize_image));
    if ( ! image_size) {
        return false;
    }

    FILE* const file = fopen(path, "wb");
    if ( ! file) {
        fprintf(stderr, "Error: Failed to open %s for writing: %s\n", path, strerror(errno));
        return false;
    }

    const bool written = fwrite(serialize_image, 1, image_size, file) == image_size;
    if ( ! written) {
        fprintf(stderr, "Error: Failed to save %s: %s\n", path, strerror(errno));
    }
    fclose(file);

    return written;
}

template <typename Bank>
bool load_instrument_bank(const char* path, Bank* bank)
{
    static uint8_t serialize_image[instrument_bank_image_size<Bank>];

    FILE* const file = fopen(path, "rb");
    if ( ! file) {
        // For a missing file condition, let the caller print the error
        if (errno != ENOENT) {
            fprintf(stderr, "Error: Failed to open %s for reading: %s\n", path, strerror(errno));
        }
        return false;
    }

    const size_t read_size = fread(serialize_image, 1, sizeof(serialize_image), file);
    fclose(file);

    return decode_instrument_bank(serialize_image, static_cast<uint32_t>(read_size), bank);
}

} // namespace Synth
