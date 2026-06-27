// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "../synth/synth_instrument.h"

#include <stdint.h>

namespace Synth {

void init_editor();

InstrumentBank& editable_bank();

void editor_snapshot();

bool editor_undo();
bool editor_redo();

bool save_editor_bank(const char* path);
bool load_editor_bank(const char* path);

// Publish the editable bank to the synth (GUI thread).
void publish_bank();

// Acquire the latest published bank for the current audio block (in synth/audio thread)
const InstrumentBank* acquire_audio_bank();

// Returns the latest published bank, but only if it changed.
const InstrumentBank* next_bank_update();

} // namespace Synth
