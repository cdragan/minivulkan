// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#pragma once

#include "realtime_synth.h"

#include <stdint.h>

namespace Synth {

// Parses a Standard MIDI File (format 0 or 1) from the in-memory buffer [data, data+size).
//
// Merges all tracks into a single event stream ordered by absolute time.  Only channel-voice
// events are emitted; meta and SysEx events are consumed but not emitted.  Writes up to
// max_events events; returns the number written, or 0 on a malformed file.
//
// With emit_ticks false, event times are samples computed from the file's division (ticks/quarter)
// and tempo (FF 51 03) meta events.  With emit_ticks true, event times are raw MIDI ticks and
// sampling_rate is ignored.
uint32_t parse_midi_file(const uint8_t* data,
                         uint32_t       size,
                         MidiEvent*     events,
                         uint32_t       max_events,
                         uint32_t       sampling_rate,
                         bool           emit_ticks = false);

} // namespace Synth
