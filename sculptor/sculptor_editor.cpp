// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2023 Chris Dragan

#include "sculptor_editor.h"

#include <stdio.h>

void Sculptor::Editor::set_object_name(const char* new_name)
{
    snprintf(object_name, sizeof(object_name), "%s", new_name);
}
