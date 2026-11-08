// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_atomic_file.h"

#include <errno.h>
#include <stdio.h>
#ifdef _WIN32
#    include <io.h>
#    include <windows.h>
#else
#    include <unistd.h>
#endif

std::variant<FILE*, int> atomic_write_begin(const char* file_path, char* staging_path, uint32_t staging_capacity)
{
    const int num_written = snprintf(staging_path, staging_capacity, "%s.tmp", file_path);
    if (num_written < 0 || static_cast<uint32_t>(num_written) >= staging_capacity)
        return ENAMETOOLONG;

    FILE* const staging_file = fopen(staging_path, "wb");
    if (! staging_file)
        return errno;

    return staging_file;
}

int atomic_write_commit(const char* file_path, const char* staging_path, FILE* staging_file)
{
    if (! staging_file)
        return EINVAL;

    int error = 0;

    if (ferror(staging_file))
        error = EIO;
    else if (fflush(staging_file))
        error = errno;
    else {
#ifdef _WIN32
        if (_commit(_fileno(staging_file)))
            error = errno;
#else
        if (fsync(fileno(staging_file)))
            error = errno;
#endif
    }

    if (fclose(staging_file) && ! error)
        error = errno;

    if (error) {
        remove(staging_path);
        return error;
    }

#ifdef _WIN32
    if (! MoveFileExA(staging_path, file_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = EIO;
#else
    if (rename(staging_path, file_path))
        error = errno;
#endif

    if (error) {
        remove(staging_path);
        return error;
    }

    return 0;
}

RenameResult rename_no_replace(const char* from, const char* to)
{
#ifdef _WIN32
    if (MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH))
        return RenameResult::renamed;

    const DWORD error = GetLastError();
    errno             = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? EEXIST : EIO;

    return RenameResult::failed;
#else
    if (link(from, to))
        return RenameResult::failed;

    if (unlink(from) == 0 || unlink(from) == 0)
        return RenameResult::renamed;

    return RenameResult::renamed_partial;
#endif
}
