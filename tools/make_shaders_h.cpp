// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <OUT.H> <SHADER.H> ...\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE* const output_file = fopen(argv[1], "w+");
    if ( ! output_file) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    if (fprintf(output_file, "#define DEFINE_SHADERS(X) \\\n") < 0) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    for (int i = 2; i < argc; i++) {
        if (fprintf(output_file, "    X(%s) \\\n", argv[i]) < 0) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
    }

    if (fprintf(output_file, "\n") < 0) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    fclose(output_file);

    return EXIT_SUCCESS;
}
