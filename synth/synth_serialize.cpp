// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_serialize.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace Synth {

// Marker and version preceding the raw bank image.  Bump the version whenever InstrumentBank's
// layout changes, so a stale file is rejected rather than misread.
// Marker: SYnth Instrument Bank
static const uint8_t  bank_marker[4]     = { 'S', 'Y', 'I', 'B' };
static const uint16_t bank_version       = 1;
static const uint32_t bank_payload_size  = static_cast<uint32_t>(sizeof(InstrumentBank));

uint32_t encode_instrument_bank(const InstrumentBank* bank, uint8_t* dest, uint32_t dest_size)
{
    if (dest_size < instrument_bank_image_size) {
        return 0;
    }

    memcpy(dest, bank_marker, sizeof(bank_marker));
    memcpy(dest + 4, &bank_version, sizeof(bank_version));
    memcpy(dest + 6, &bank_payload_size, sizeof(bank_payload_size));
    memcpy(dest + instrument_bank_header_size, bank, sizeof(*bank));

    return instrument_bank_image_size;
}

bool decode_instrument_bank(const uint8_t* src, uint32_t src_size, InstrumentBank* bank)
{
    if (src_size < instrument_bank_image_size) {
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
    if (payload_size != bank_payload_size) {
        return false;
    }

    memcpy(bank, src + instrument_bank_header_size, sizeof(*bank));
    return true;
}

bool save_instrument_bank(const char* path, const InstrumentBank* bank)
{
    uint8_t image[instrument_bank_image_size];
    const uint32_t image_size = encode_instrument_bank(bank, image, sizeof(image));
    if ( ! image_size) {
        return false;
    }

    FILE* const file = fopen(path, "wb");
    if ( ! file) {
        fprintf(stderr, "Error: Failed to open %s for writing: %s\n", path, strerror(errno));
        return false;
    }

    const bool written = fwrite(image, 1, image_size, file) == image_size;
    if ( ! written) {
        fprintf(stderr, "Error: Failed to save %s: %s\n", path, strerror(errno));
    }

    fclose(file);
    return written;
}

bool load_instrument_bank(const char* path, InstrumentBank* bank)
{
    FILE* const file = fopen(path, "rb");
    if ( ! file) {
        fprintf(stderr, "Error: Failed to open %s for reading: %s\n", path, strerror(errno));
        return false;
    }

    uint8_t image[instrument_bank_image_size];
    const size_t read_size = fread(image, 1, sizeof(image), file);
    fclose(file);

    return decode_instrument_bank(image, static_cast<uint32_t>(read_size), bank);
}

} // namespace Synth
