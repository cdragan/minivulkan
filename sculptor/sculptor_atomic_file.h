// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <variant>

std::variant<FILE*, int> atomic_write_begin(const char* file_path, char* staging_path, uint32_t staging_capacity);

int atomic_write_commit(const char* file_path, const char* staging_path, FILE* staging_file);
