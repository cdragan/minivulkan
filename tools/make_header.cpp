// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2024 Chris Dragan

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Declare input buffer
// Input buffer is large enough for any reasonable SPIR-V shader.
// Larger shaders wouldn't fit in a 64KB intro anyway.
static uint8_t input_buf[256 * 1024];

static int write_c_output(const uint8_t* output_buf,
                          size_t         output_size,
                          FILE*          output_file,
                          const char*    variable_name)
{
    if (fprintf(output_file, "#pragma once\n") < 0)
        return EXIT_FAILURE;

    if (fprintf(output_file, "const uint8_t %s[%u] = {\n", variable_name,
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
        "Usage: make_header <VARIABLE_NAME> <INPUT_FILE> <OUTPUT_FILE>\n";
    if (argc != 4) {
        fprintf(stderr, "%s", usage);
        return EXIT_FAILURE;
    }

    const char* const variable_name   = argv[argc - 3];
    const char* const input_filename  = argv[argc - 2];
    const char* const output_filename = argv[argc - 1];

    // Open input file
    FILE* const input_file = fopen(input_filename, "rb");
    if ( ! input_file) {
        perror("make_header");
        fprintf(stderr, "make_header: failed to open %s\n", input_filename);
        return EXIT_FAILURE;
    }

    // Read the contents of the input file
    const size_t num_read = fread(input_buf, 1, sizeof(input_buf), input_file);
    if (ferror(input_file)) {
        perror("make_header");
        fprintf(stderr, "make_header: failed to read from %s\n", input_filename);
        return EXIT_FAILURE;
    }
    fclose(input_file);

    if (num_read == sizeof(input_buf)) {
        fprintf(stderr, "make_header: input file %s is too large\n", input_filename);
        return EXIT_FAILURE;
    }

    // Open output file
    FILE* const output_file = fopen(output_filename, "w+");
    if ( ! output_file) {
        perror("make_header");
        fprintf(stderr, "make_header: failed to open %s\n", output_filename);
        return EXIT_FAILURE;
    }

    // Write output buffer to the output file
    const int ret = write_c_output(input_buf, num_read, output_file, variable_name);
    fclose(output_file);
    if (ret) {
        perror("make_header");
        fprintf(stderr, "make_header: failed to write to %s\n", output_filename);
        return ret;
    }

    return EXIT_SUCCESS;
}
