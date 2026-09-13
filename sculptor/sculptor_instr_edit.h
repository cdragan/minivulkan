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

// Publish the edited bank to the synth (GUI thread). Returns false when the bank queue
// is backpressured; the pending copy is retained and retried by pump_bank_publish().
bool publish_bank();

// GUI thread, once per frame: retries a backpressured publish (see publish_bank).
bool pump_bank_publish();

} // namespace Synth
