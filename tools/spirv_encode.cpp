// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32le(const void* buf)
{
    const uint8_t* const bytes = static_cast<const uint8_t*>(buf);

    const uint32_t b0 = bytes[0];
    const uint32_t b1 = bytes[1];
    const uint32_t b2 = bytes[2];
    const uint32_t b3 = bytes[3];

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static int can_remove(uint32_t opcode)
{
    // Instructions which have no semantic meaning are safe to remove
    switch (opcode) {
        case 0:   // OpNop
        case 2:   // OpSourceContinued
        case 3:   // OpSource
        case 4:   // OpSourceExtension
        case 5:   // OpName
        case 6:   // OpMemberName
        case 7:   // OpString
        case 8:   // OpLine
        case 317: // OpNoLine
        case 330: // OpModuleProcessed
            return 1;

        default:
            break;
    }

    return 0;
}

static bool opt_remove_unused = false;

// Declare input buffer
// Input buffer is large enough for any reasonable SPIR-V shader.
// Larger shaders wouldn't fit in a 64KB intro anyway.
static uint8_t input_buf[256 * 1024];

static constexpr uint32_t spirv_header_size = 20;

template<typename T>
int walk_spirv(size_t num_read, const char* input_filename, T func)
{
    const uint8_t*       input = &input_buf[spirv_header_size];
    const uint8_t* const end   = &input_buf[num_read];

    do {
        const uint32_t opcode_word = read32le(input);
        const uint32_t opcode      = opcode_word & 0xFFFFu;
        const uint32_t word_count  = opcode_word >> 16;

        if ( ! opt_remove_unused || ! can_remove(opcode)) {

            const uint8_t* const next = input + word_count * 4;
            if (next > end) {
                fprintf(stderr, "spirv_encode: instruction word count exceeds SPIR-V size in %s\n", input_filename);
                return EXIT_FAILURE;
            }

            if (word_count == 0) {
                fprintf(stderr, "spirv_encode: invalid word count 0 in %s\n", input_filename);
                return EXIT_FAILURE;
            }

            func(opcode, word_count - 1, input + 4);
        }

        input += word_count * 4;
    } while (input < end);

    return EXIT_SUCCESS;
}

static int write_c_output(const uint8_t* output_buf,
                          size_t         output_size,
                          FILE*          output_file,
                          const char*    variable_name)
{
    if (fprintf(output_file, "#pragma once\n") < 0)
        return EXIT_FAILURE;

    if (fprintf(output_file, "uint8_t %s[%u] = {\n", variable_name,
                static_cast<unsigned>(output_size)) < 0)
        return EXIT_FAILURE;

    constexpr uint32_t columns = 16;

    for (uint32_t i = 0; i < output_size; i++) {
        if (i % columns == 0) {
            if (fprintf(output_file, "    ") < 0)
                return EXIT_FAILURE;
        }

        if (fprintf(output_file, "0x%02x", output_buf[i]) < 0)
            return EXIT_FAILURE;

        if (i + 1 < output_size) {
            if (fprintf(output_file, ",") < 0)
                return EXIT_FAILURE;
        }

        if ((i % columns == columns - 1) || (i + 1 == output_size)) {
            if (fprintf(output_file, "\n") < 0)
                return EXIT_FAILURE;
        }
    }
    if (fprintf(output_file, "};\n") < 0)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

int main(int argc, char* argv[])
{
    static const char usage[] =
        "Usage: spirv_encode [--remove-unused] [--no-shuffle] [--binary] <VARIABLE_NAME> <INPUT_FILE> <OUTPUT_FILE>\n";
    if (argc < 4) {
        fprintf(stderr, "%s", usage);
        return EXIT_FAILURE;
    }

    bool opt_shuffle = true;
    bool opt_binary  = false;

    for (int i = 1; i < argc - 3; i++) {
        const char* const arg = argv[i];

        if (strcmp(arg, "--remove-unused") == 0)
            opt_remove_unused = true;
        else if (strcmp(arg, "--no-shuffle") == 0)
            opt_shuffle = false;
        else if (strcmp(arg, "--binary") == 0)
            opt_binary = true;
        else {
            fprintf(stderr, "%s", usage);
            return EXIT_FAILURE;
        }
    }
    const char* const variable_name   = argv[argc - 3];
    const char* const input_filename  = argv[argc - 2];
    const char* const output_filename = argv[argc - 1];

    // Open input file
    FILE* const input_file = fopen(input_filename, "rb");
    if ( ! input_file) {
        perror("spirv_encode");
        fprintf(stderr, "spirv_encode: failed to open %s\n", input_filename);
        return EXIT_FAILURE;
    }

    // Read the contents of SPIR-V file
    const size_t num_read = fread(input_buf, 1, sizeof(input_buf), input_file);
    if (ferror(input_file)) {
        perror("spirv_encode");
        fprintf(stderr, "spirv_encode: failed to read from %s\n", input_filename);
        return EXIT_FAILURE;
    }
    fclose(input_file);

    if (num_read < 6 * 4) {
        fprintf(stderr, "spirv_encode: invalid SPIR-V in %s\n", input_filename);
        return EXIT_FAILURE;
    }

    if (num_read == sizeof(input_buf)) {
        fprintf(stderr, "spirv_encode: input file %s is too large\n", input_filename);
        return EXIT_FAILURE;
    }

    if ((num_read % 4) != 0) {
        fprintf(stderr, "spirv_encode: input file %s size is not modulo 4\n", input_filename);
        return EXIT_FAILURE;
    }

    // Validate SPIR-V header
    if (read32le(input_buf) == 0x03022307) {
        fprintf(stderr, "spirv_encode: big endian SPIR-V is not supported in %s\n", input_filename);
        return EXIT_FAILURE;
    }
    if (read32le(input_buf) != 0x07230203) {
        fprintf(stderr, "spirv_encode: %s does not contain valid SPIR-V\n", input_filename);
        return EXIT_FAILURE;
    }
    const uint32_t version = read32le(&input_buf[4]);
    if ((version & 0xFF0000FFu) != 0) {
        fprintf(stderr, "spirv_encode: %s contains unsupported version 0x%x in the header\n",
                input_filename, version);
        return EXIT_FAILURE;
    }
    const uint32_t bound = read32le(&input_buf[12]);
    if (bound > 0xFFFFu) {
        fprintf(stderr, "spirv_encode: bound 0x%x exceeds 16 bits in %s\n", bound, input_filename);
        return EXIT_FAILURE;
    }
    if (read32le(&input_buf[16]) != 0) {
        fprintf(stderr, "spirv_encode: %s contains unrecognized value 0x%x in reserved word in the header\n",
                input_filename, read32le(&input_buf[16]));
        return EXIT_FAILURE;
    }

    // Count how many opcodes there are in the SPIR-V
    uint32_t total_opcodes = 0;
    uint32_t total_words   = 0;
    int ret = walk_spirv(num_read, input_filename,
                         [&](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
        ++total_opcodes;
        total_words += 1 + num_operands;
    });
    if (ret)
        return ret;

    // Declare output buffer
    // The size of encoded SPIR-V is roughly the same as input SPIR-V
    static uint8_t output_buf[sizeof(input_buf) + 16];
    uint8_t* output = output_buf;

    const auto output16 = [&](uint16_t value) {
        output[0] = static_cast<uint8_t>(value);
        output[1] = static_cast<uint8_t>(value >> 8);
        output += 2;
    };

    // 8 bytes for storing loaded VkShaderModule object
    memset(output, 0, 8);
    output += 8;

    if (opt_shuffle) {

        // Dump header
        output16(static_cast<uint16_t>(total_words));
        output16(static_cast<uint16_t>(total_opcodes));
        output16(static_cast<uint16_t>(version >> 8));
        output16(static_cast<uint16_t>(bound));

        // Dump low byte of each opcode
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
            *(output++) = static_cast<uint8_t>(opcode);
        });
        if (ret)
            return ret;

        // Dump low byte of each number of operands
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
            *(output++) = static_cast<uint8_t>(num_operands);
        });
        if (ret)
            return ret;

        // Dump low byte of each operand
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
            const uint8_t* const end = operands + num_operands * 4;
            for (; operands < end; operands += 4)
                *(output++) = *operands;
        });
        if (ret)
            return ret;

        // Dump high byte of each opcode
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
            *(output++) = static_cast<uint8_t>(opcode >> 8);
        });
        if (ret)
            return ret;

        // Dump high byte of each number of operands
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
            *(output++) = static_cast<uint8_t>(num_operands >> 8);
        });
        if (ret)
            return ret;

        // Dump 3 high bytes of each operand, first byte 1, then byte 2, then byte 3
        for (int op_byte = 1; op_byte < 4; op_byte++) {
            ret = walk_spirv(num_read, input_filename,
                             [&output, op_byte](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {
                operands += op_byte;
                const uint8_t* const end = operands + num_operands * 4;
                for (; operands < end; operands += 4)
                    *(output++) = *operands;
            });
            if (ret)
                return ret;
        }
    }
    else {

        // Dump size in words, excluding header
        output16(static_cast<uint16_t>(total_words));
        output16(0); // alignment

        // Dump header
        memcpy(output, input_buf, spirv_header_size);
        output += spirv_header_size;

        // Dump opcodes and operands
        ret = walk_spirv(num_read, input_filename,
                         [&output](uint32_t opcode, uint32_t num_operands, const uint8_t* operands) {

            const uint32_t num_words = num_operands + 1;

            *(output++) = static_cast<uint8_t>(opcode);
            *(output++) = static_cast<uint8_t>(opcode >> 8);
            *(output++) = static_cast<uint8_t>(num_words);
            *(output++) = static_cast<uint8_t>(num_words >> 8);

            memcpy(output, operands, num_operands * sizeof(uint32_t));
            output += num_operands * sizeof(uint32_t);
        });
        if (ret)
            return ret;
    }

    // Open output file
    FILE* const output_file = fopen(output_filename, "w+");
    if ( ! output_file) {
        perror("spirv_encode");
        fprintf(stderr, "spirv_encode: failed to open %s\n", output_filename);
        return EXIT_FAILURE;
    }

    // Write output buffer to the output file
    const size_t output_size = static_cast<size_t>(output - output_buf);
    if (opt_binary) {
        if (fwrite(output_buf, 1, output_size, output_file) != output_size)
            ret = EXIT_FAILURE;
    }
    else
        ret = write_c_output(output_buf, output_size, output_file, variable_name);
    fclose(output_file);
    if (ret) {
        perror("spirv_encode");
        fprintf(stderr, "spirv_encode: failed to write to %s\n", output_filename);
        return ret;
    }

    return EXIT_SUCCESS;
}
