// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "synth_instrument.h"

#include <stdint.h>

namespace Synth {

InstrumentBank& editable_bank();

void editor_snapshot();

bool editor_undo();
bool editor_redo();

bool save_editor_bank(const char* path);
bool load_editor_bank(const char* path);

// Publish the instrument bank to the synth.
// Must be called from a single producer GUI thread.
void publish_bank();

// Read published instrument bank (used by synth/audio thread)
const InstrumentBank* acquire_audio_bank();

} // namespace Synth
