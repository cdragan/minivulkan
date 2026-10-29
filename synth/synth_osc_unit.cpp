// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

// Target-block field offsets (the dots come from the osc_target_ helpers).
constexpr uint32_t t7_env_depth_field = 1;

constexpr uint32_t t7_lfo_op_field = 3;

constexpr uint32_t t7_lfo_depth_field = 4;

constexpr uint32_t t7_rate_field = 5;

// The value row slot of each ModTarget on an oscillator node; its
// 12-slot block starts directly after it.
constexpr uint32_t t7_target_row_slots[Synth::num_mod_targets] = {
    109, // volume
    135, // pitch
    122, // pan
    2,   // duty a
    43,  // duty b
    70,  // waveform mix
    83,  // fm depth
    166, // fir pitch spread
    96,  // fm ratio
    15,  // pad tone a
    56,  // pad tone b
    192, // fir slope
    153, // fir band 0 offset
    179, // fir band 0 gain
    208, // fir band 1 offset
    221, // fir band 1 spread
    234, // fir band 1 gain
    247, // fir band 1 slope
    263, // fir band 2 offset
    276, // fir band 2 spread
    289, // fir band 2 gain
    302, // fir band 2 slope
    318, // fir band 3 offset
    331, // fir band 3 spread
    344, // fir band 3 gain
    357, // fir band 3 slope
    15,  // damping
    28,  // release damping
    2,   // dispersion
};

// True when target t has a projected row on this layer: the duty a / pad tone a
// rows carry String Dispersion / Sustain Damping under the string wave, so the other
// of each pair has no row (its binding is stored but not projected); the string
// targets have no row under any other wave.
static bool t7_target_has_row(const Synth::Oscillator& osc, uint32_t t)
{
    const bool string_wave = osc.osc_type[0] == Synth::WaveType::string_wave;
    const bool string_only =
        t == Synth::mod_string_damping || t == Synth::mod_string_release_damping || t == Synth::mod_string_dispersion;
    const bool shared = t == Synth::mod_duty0 || t == Synth::mod_pad_tilt0;
    return string_wave ? ! shared : ! string_only;
}

// Shared graph and mapping for the direct-row blocks; every block
// re-projects, which rebuilds both from scratch.
static const std::unique_ptr<Sculptor::Graph> t7_graph_storage = std::make_unique<Sculptor::Graph>();

static Sculptor::Graph& t7_graph = *t7_graph_storage;

static Sculptor::OscGraphMapping t7_mapping;

// Model-based evidence for the add contract, not an executable
// GUI test: the unit target does not link the GUI translation
// unit.  Canvas Add and pulled-slot Add share do_osc_add_oscillator:
// the fresh oscillator appends at the editor layer_count with fresh
// default parameters (no inheritance), and its wiring slot resolves
// through the pure lowest-free-slot helper, so red holes fill before
// the gray add slot and a full run refuses.  The consumer must not
// consult the dragged row beyond the slot bounds check: a disconnect
// leaves the oscillator on the canvas and any free slot accepts any
// oscillator.  Both menus must
// disable the Add Oscillator entry when no slot is free (the
// occupied-cell disabled style) instead of offering it unguarded.

// ---------------------------------------------------------------------------
// Bug 1 oscillator-wiring tests: disconnect cleanup, per-zone source rows,
// playback compilation, identity install, and the osc_sources JSON keys.
// The tests below assert the target contract against the current build.
// ---------------------------------------------------------------------------

// Disconnects the sum wire into output input slot `slot` through the real
// apply path; returns true on success.
static bool disconnect_osc_sum_slot(Synth::InstrumentEditorBank* bank,
                                    Sculptor::Graph&             graph,
                                    Sculptor::OscGraphMapping&   mapping,
                                    uint32_t                     slot)
{
    const Sculptor::EndPoint input = { mapping.output_node, mapping.output_layer_input_slot[slot] };
    const uint32_t           conn  = find_osc_graph_connection(graph, input);
    if (conn == Sculptor::pool_no_slot) {
        return false;
    }

    Sculptor::GraphChange change = {};
    change.kind                  = Sculptor::ChangeKind::connection_deleted;
    change.connection_idx        = conn;
    // The event carries its endpoint pair: the pool slot may already be
    // freed or reused when the change is applied.
    change.connection_output = graph.get_connection(conn).output;
    change.connection_input  = graph.get_connection(conn).input;

    graph.delete_connection(conn);
    return Sculptor::apply_osc_graph_change(bank, &graph, &mapping, change);
}

// The pure lowest-free-slot seam: the first slot whose source entry is 0,
// or pool_no_slot when every slot carries a wire.
static void check_osc_lowest_free_slot()
{
    uint8_t row[Synth::max_layers] = {};
    TEST(Sculptor::osc_lowest_free_slot(row) == 0);

    row[0] = 1;
    TEST(Sculptor::osc_lowest_free_slot(row) == 1);

    // {o0, none, o2}: the red hole at slot 1 fills before the gray add slot.
    row[2] = 3;
    TEST(Sculptor::osc_lowest_free_slot(row) == 1);

    row[1] = 2;
    TEST(Sculptor::osc_lowest_free_slot(row) == 3);

    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        row[slot] = static_cast<uint8_t>(slot + 1);
    }
    TEST(Sculptor::osc_lowest_free_slot(row) == pool_no_slot);
}

// Criterion 1: disconnect = wire gone, nothing more.  The stored editor
// instrument keeps every oscillator with its parameters, and any oscillator
// may wire into any free output input slot.
static void check_osc_wire_any_free_slot()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
    graph.set_validator(Sculptor::osc_graph_validate, &mapping);
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    // Disconnect the wire into output input 1.
    TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
    // The authoring form keeps the disconnected oscillator: the stored
    // instrument still holds all three layers with their parameters.
    const Synth::Instrument& stored = bank.bank.instruments.entries[0];
    TEST(stored.layer_count == 3);
    TEST(stored.layers[1].osc_type[0] == Synth::WaveType::sawtooth_wave);
    TEST(stored.layers[1].targets[Synth::mod_volume].envelope_desc_id == 2);

    // Any oscillator wires into the freed slot: osc 2 into output input 1.
    const Sculptor::EndPoint osc2_out = { mapping.osc_nodes[2], mapping.osc_output_slot };
    const Sculptor::EndPoint slot1_in = { mapping.output_node, mapping.output_layer_input_slot[1] };
    TEST(Sculptor::osc_graph_validate(&mapping, graph, osc2_out, slot1_in));
    TEST(graph.add_connection(osc2_out, slot1_in) != Sculptor::pool_no_slot);
    Sculptor::GraphChange added[Sculptor::max_pending_changes] = {};
    const uint32_t        dbg_tc = graph.take_changes(added, Sculptor::max_pending_changes);
    TEST(dbg_tc == 1);
    TEST(added[0].kind == Sculptor::ChangeKind::connection_added);
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, added[0]));
    TEST(find_osc_graph_connection(graph, slot1_in) != Sculptor::pool_no_slot);

    // An occupied slot refuses: osc 1 cannot also feed slot 0.
    const Sculptor::EndPoint osc1_out = { mapping.osc_nodes[1], mapping.osc_output_slot };
    const Sculptor::EndPoint slot0_in = { mapping.output_node, mapping.output_layer_input_slot[0] };
    TEST(! Sculptor::osc_graph_validate(&mapping, graph, osc1_out, slot0_in));
}

// Criterion 1 (auto-move): an oscillator output carries at most one wire.
// Wiring an already-wired oscillator into a free slot auto-moves its old
// wire; retargeting a wire's output end to an already-wired oscillator does
// the same.  A hand-built fan-out graph compiles: the repeated source
// ordinal is accepted, and the compile copies the referenced layer into
// every wired slot in slot order.
static void check_osc_wire_auto_move()
{
    // (i) A new wire moves the oscillator's existing wire.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        graph.set_validator(Sculptor::osc_graph_validate, &mapping);
        Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);

        // Free slot 1: osc 1 becomes unwired and stays on the canvas.
        TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
        // Wire osc 2 - whose output already feeds slot 2 - into slot 1.
        const Sculptor::EndPoint osc2_out = { mapping.osc_nodes[2], mapping.osc_output_slot };
        const Sculptor::EndPoint slot1_in = { mapping.output_node, mapping.output_layer_input_slot[1] };
        const Sculptor::EndPoint slot2_in = { mapping.output_node, mapping.output_layer_input_slot[2] };
        TEST(Sculptor::osc_graph_validate(&mapping, graph, osc2_out, slot1_in));
        TEST(graph.add_connection(osc2_out, slot1_in) != Sculptor::pool_no_slot);
        Sculptor::GraphChange added[Sculptor::max_pending_changes] = {};
        TEST(graph.take_changes(added, Sculptor::max_pending_changes) == 1);
        TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, added[0]));
        // The applier moved the oscillator's old wire: slot 1 carries osc 2
        // and slot 2 is free again.
        TEST(find_fx_graph_connection(graph, osc2_out, slot1_in) != Sculptor::pool_no_slot);
        TEST(find_osc_graph_connection(graph, slot2_in) == Sculptor::pool_no_slot);
    }

    // (ii) Retargeting a wire's output end to an already-wired oscillator
    // deletes that oscillator's old wire and keeps the retargeted one.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        graph.set_validator(Sculptor::osc_graph_validate, &mapping);
        Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);

        const Sculptor::EndPoint osc1_out  = { mapping.osc_nodes[1], mapping.osc_output_slot };
        const Sculptor::EndPoint osc2_out  = { mapping.osc_nodes[2], mapping.osc_output_slot };
        const Sculptor::EndPoint slot1_in  = { mapping.output_node, mapping.output_layer_input_slot[1] };
        const Sculptor::EndPoint slot2_in  = { mapping.output_node, mapping.output_layer_input_slot[2] };
        const uint32_t           osc1_wire = find_fx_graph_connection(graph, osc1_out, slot1_in);
        TEST(osc1_wire != Sculptor::pool_no_slot);
        // The retarget is accepted even though osc 2 already feeds slot 2.
        TEST(graph.move_connection_end(osc1_wire, true, osc2_out));
        Sculptor::GraphChange changed[Sculptor::max_pending_changes] = {};
        TEST(graph.take_changes(changed, Sculptor::max_pending_changes) == 1);
        TEST(changed[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changed[0]));
        // The retargeted wire lands on osc 2 and osc 2's old wire is gone;
        // osc 1 is unwired, on the canvas, untouched.
        TEST(find_fx_graph_connection(graph, osc2_out, slot1_in) != Sculptor::pool_no_slot);
        TEST(find_osc_graph_connection(graph, slot2_in) == Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph, osc1_out, slot1_in) == Sculptor::pool_no_slot);
        TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[0] }) !=
             Sculptor::pool_no_slot);
    }

    // (iii) A hand-built graph with two wires out of one oscillator still
    // compiles: both slots name the same layer, and the derived row keeps
    // the ordinal of each slot's source in slot order.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank.bank, &graph, &mapping));
        Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);
        TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
        TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 2));
        const Sculptor::EndPoint osc2_out = { mapping.osc_nodes[2], mapping.osc_output_slot };
        const Sculptor::EndPoint slot1_in = { mapping.output_node, mapping.output_layer_input_slot[1] };
        const Sculptor::EndPoint slot2_in = { mapping.output_node, mapping.output_layer_input_slot[2] };
        TEST(graph.add_connection(osc2_out, slot1_in) != Sculptor::pool_no_slot);
        TEST(graph.add_connection(osc2_out, slot2_in) != Sculptor::pool_no_slot);
        Synth::Instrument compiled                       = {};
        uint8_t           derived_row[Synth::max_layers] = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled, derived_row));

        const uint8_t expected_row[Synth::max_layers] = { 1, 3, 3 };
        TEST(memcmp(derived_row, expected_row, sizeof(expected_row)) == 0);

        // The retained refusal: a raw connection from a second oscillator
        // into an already-occupied sum input refuses to compile.
        const Sculptor::EndPoint osc1_out = { mapping.osc_nodes[1], mapping.osc_output_slot };
        TEST(graph.add_connection(osc1_out, slot1_in) != Sculptor::pool_no_slot);
        Synth::Instrument refused = {};
        TEST(! Sculptor::compile_graph_to_instrument(graph, mapping, &refused, derived_row));
    }
}

// The source-row tests.  See the block comment at the top of this section.

static void check_osc_red_run_derivation()
{
    // {o0, none, o2}: the run ends after the highest wired slot, the hole
    // renders red, the gray add slot follows the run, and slots past it hide.
    uint8_t row[Synth::max_layers] = {};
    row[0]                         = 1;
    row[2]                         = 3;
    TEST(Sculptor::osc_row_run_length(row) == 3);
    TEST(Sculptor::osc_row_is_red(row, 1));
    TEST(! Sculptor::osc_row_is_red(row, 0));
    TEST(! Sculptor::osc_row_is_red(row, 2));
    TEST(Sculptor::osc_row_gray_slot(row) == 3);
    TEST(! Sculptor::osc_layer_slot_hidden(3, row));
    TEST(Sculptor::osc_layer_slot_hidden(4, row));
    TEST(! Sculptor::osc_layer_slot_hidden(0, row));
    TEST(! Sculptor::osc_layer_slot_hidden(2, row));

    // An all-zero row: the run is empty, the gray add slot sits at 0, and
    // nothing renders red.
    const uint8_t empty[Synth::max_layers] = {};
    TEST(Sculptor::osc_row_run_length(empty) == 0);
    TEST(Sculptor::osc_row_gray_slot(empty) == 0);
    TEST(! Sculptor::osc_row_is_red(empty, 0));
    TEST(! Sculptor::osc_layer_slot_hidden(0, empty));
    TEST(Sculptor::osc_layer_slot_hidden(1, empty));

    // A full row leaves no gray slot and hides nothing inside the run.
    uint8_t full[Synth::max_layers] = {};
    for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
        full[slot] = static_cast<uint8_t>(slot + 1);
    }
    TEST(Sculptor::osc_row_run_length(full) == Synth::max_layers);
    TEST(Sculptor::osc_row_gray_slot(full) == Synth::max_layers);
    TEST(! Sculptor::osc_layer_slot_hidden(Synth::max_layers - 1, full));
}

// Criterion 3: gaps compile to a valid playback form in slot order with the
// instrument-level fields preserved, and a zone with no wired slot publishes
// its channel disabled.
static void check_osc_playback_gaps()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};

    build_binding_fixture(&bank, &instrument);
    bank.graph_osc_source[0][0][0] = 1;
    bank.graph_osc_source[0][0][2] = 3;

    Synth::Instrument playback = {};
    TEST(Sculptor::compile_playback_instrument(bank.bank.instruments.entries[0],
                                               bank.graph_osc_source[0][0],
                                               &playback));

    TEST(playback.layer_count == 2);
    TEST(memcmp(&playback.layers[0], &instrument.layers[0], sizeof(Synth::Oscillator)) == 0);
    TEST(memcmp(&playback.layers[1], &instrument.layers[2], sizeof(Synth::Oscillator)) == 0);
    TEST(playback.note_skew_semitones == instrument.note_skew_semitones);
    TEST(playback.layer_skew_semitones == instrument.layer_skew_semitones);
    TEST(playback.release_seconds == instrument.release_seconds);
    TEST(playback.voice_mode == instrument.voice_mode);
    // The voice mode rides into the playback form for every mode.
    for (uint32_t mode = 1; mode < 3; mode++) {
        bank.bank.instruments.entries[0].voice_mode = static_cast<uint8_t>(mode);
        Synth::Instrument moded                     = {};
        TEST(Sculptor::compile_playback_instrument(bank.bank.instruments.entries[0],
                                                   bank.graph_osc_source[0][0],
                                                   &moded));
        TEST(moded.voice_mode == mode);
    }
    bank.bank.instruments.entries[0].voice_mode = instrument.voice_mode;
    TEST(Synth::validate_instrument_bank(&bank.bank));

    // A zone whose row is all zero but that still carries oscillator nodes
    // publishes its channel disabled; with identity wiring it publishes
    // enabled.
    const std::unique_ptr<Synth::InstrumentEditorBank> silent_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       silent         = *silent_storage;
    build_binding_fixture(&silent, &instrument);
    Sculptor::set_identity_osc_sources(&silent, 0, 0);
    memset(silent.graph_osc_source[0][0], 0, Synth::max_layers);
    uint8_t enabled[Synth::max_channels] = {};
    Sculptor::compute_publish_channel_enabled(silent, enabled);
    TEST(enabled[0] == 0);
    Sculptor::set_identity_osc_sources(&silent, 0, 0);
    Sculptor::compute_publish_channel_enabled(silent, enabled);
    TEST(enabled[0] == 1);
}

// Criterion 2: a disconnect leaves the oscillator's parameters in the
// stored editor instrument; an edit on the unwired node lands there; and
// rewiring it into a free slot compiles a playback layer with those
// parameters.
static void check_osc_unwired_params_persist()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    graph.set_validator(Sculptor::osc_graph_validate, &mapping);
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
    // The editor layer keeps its parameters; the slot clears.
    Synth::Instrument& editor = bank.bank.instruments.entries[0];
    TEST(editor.layer_count == 3);
    TEST(editor.layers[1].targets[Synth::mod_volume].envelope_desc_id == 2);
    TEST(bank.graph_osc_source[0][0][1] == 0);

    // An edit on the now-unwired node's parameter lands on the stored layer
    // (modeled like the suite's other direct bank edits).
    editor.layers[1].targets[Synth::mod_pitch].base_value = -5.0f;

    // Rewiring osc 1 into the free slot 3 stores the new source entry.
    const Sculptor::EndPoint osc1_out = { mapping.osc_nodes[1], mapping.osc_output_slot };
    const Sculptor::EndPoint slot3_in = { mapping.output_node, mapping.output_layer_input_slot[3] };
    TEST(Sculptor::osc_graph_validate(&mapping, graph, osc1_out, slot3_in));
    TEST(graph.add_connection(osc1_out, slot3_in) != Sculptor::pool_no_slot);
    Sculptor::GraphChange added[Sculptor::max_pending_changes] = {};
    TEST(graph.take_changes(added, Sculptor::max_pending_changes) == 1);
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, added[0]));
    TEST(editor.layers[1].targets[Synth::mod_pitch].base_value == -5.0f);

    // The playback form carries the edited layer in slot order: the
    // identity row keeps slot 2 wired (osc 3 stays), so the rewire into
    // slot 3 leaves three wired slots and the edited osc 1 plays last.
    Synth::Instrument playback = {};
    TEST(Sculptor::compile_playback_instrument(editor, bank.graph_osc_source[0][0], &playback));
    TEST(playback.layer_count == 3);
    TEST(playback.layers[2].targets[Synth::mod_pitch].base_value == -5.0f);

    // Reprojection keeps the node on the canvas and rewires it from the
    // stored row: slot 3 wired, slot 1 still free.
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(mapping.osc_nodes[1] != Sculptor::pool_no_slot);
    const Sculptor::EndPoint rewired_out = { mapping.osc_nodes[1], mapping.osc_output_slot };
    TEST(find_fx_graph_connection(graph, rewired_out, slot3_in) != Sculptor::pool_no_slot);
    TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[1] }) ==
         Sculptor::pool_no_slot);
}

// Canvas Add, modeled as the consumer performs it (direct bank edits, no UI
// path): append a fresh-default oscillator at the editor layer_count and
// wire it into osc_lowest_free_slot, then reproject.
static void check_osc_add_lowest_free_slot()
{
    // With sources {o0, none, o2} the red hole at slot 1 fills first.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
        Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);
        TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
        const uint32_t slot = Sculptor::osc_lowest_free_slot(bank.graph_osc_source[0][0]);
        TEST(slot == 1);
        Synth::Instrument& editor = bank.bank.instruments.entries[0];
        Synth::Oscillator  fresh  = {};
        fresh.osc_type[0]         = Synth::WaveType::sine_wave;
        editor.layers[3]          = fresh;
        editor.layer_count        = 4;
        set_default_dispersion_sections(editor);
        bank.graph_osc_source[0][0][slot] = 4;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);
        TEST(mapping.osc_nodes[3] != Sculptor::pool_no_slot);
        TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[1] }) !=
             Sculptor::pool_no_slot);
    }

    // With sources {o0, o1} the gray add slot at 2 takes the new wire.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        bank.graph_osc_source[0][0][2]                       = 0; // sources {o0, o1}: layer 3 starts unwired
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
        const uint32_t slot = Sculptor::osc_lowest_free_slot(bank.graph_osc_source[0][0]);
        TEST(slot == 2);
        Synth::Instrument& editor = bank.bank.instruments.entries[0];
        Synth::Oscillator  fresh  = {};
        fresh.osc_type[0]         = Synth::WaveType::sine_wave;
        editor.layers[3]          = fresh;
        editor.layer_count        = 4;
        set_default_dispersion_sections(editor);
        bank.graph_osc_source[0][0][slot] = 4;
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
        TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[2] }) !=
             Sculptor::pool_no_slot);
    }

    // With every slot wired there is no free slot: canvas Add refuses.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        Synth::Instrument& editor = bank.bank.instruments.entries[0];
        editor.layer_count        = Synth::max_layers;
        set_default_dispersion_sections(editor);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        TEST(Sculptor::osc_lowest_free_slot(bank.graph_osc_source[0][0]) == Sculptor::pool_no_slot);
    }
}

// Identity initialization: exercise the helper and library load, checking
// the installed source ordinals and the compiled instrument.
static void check_osc_source_identity_install()
{
    // The helper wires the first layer_count slots to their own ordinals.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&bank, &instrument);
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
            TEST(bank.graph_osc_source[0][0][slot] == (slot < 3 ? static_cast<uint8_t>(slot + 1) : 0));
        }
        // The identity row compiles the instrument unchanged.
        Synth::Instrument playback = {};
        TEST(Sculptor::compile_playback_instrument(bank.bank.instruments.entries[0],
                                                   bank.graph_osc_source[0][0],
                                                   &playback));
        TEST(playback.layer_count == 3);
        TEST(memcmp(&playback.layers[0], &instrument.layers[0], 3 * sizeof(Synth::Oscillator)) == 0);
    }

    // Library load installs identity rows.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source     = *source_storage;
        Synth::Instrument            instrument = {};
        build_binding_fixture(&source, &instrument);
        strcpy(source.instrument_names[0], "Identity");
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const char* const path = "synth_osc_identity.tmp";
        remove(path);
        TEST(Synth::save_library_record(path, "Regression", "Identity", &source, 0) == 0);
        Synth::LibraryEntry entry = {};
        TEST(Synth::read_library_index(path, &entry, 1) == 1);
        const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& destination = *destination_storage;
        uint16_t                     slot        = 0xBEEF;
        TEST(Synth::load_library_instrument(path, &entry, &destination, 2, &slot));
        for (uint32_t s = 0; s < Synth::max_layers; ++s) {
            TEST(destination.graph_osc_source[2][0][s] == (s < 3 ? static_cast<uint8_t>(s + 1) : 0));
        }
        remove(path);
    }
}

// Deleting an oscillator node clears the deleted ordinal's source entries in
// every zone that references the instrument, compacts the higher ordinals,
// and leaves the kind-0 record positions reprojection-stable, with no
// refusal.
static void check_osc_delete_clears_shared_sources()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    // A second zone shares the instrument (hand-edited JSON territory; the
    // editor never creates sharing).  Zone 1 wires different slots.
    bank.bank.channel_zones[0][1].start_note = 61;
    bank.bank.channel_zones[0][1].instrument = 0;
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    bank.graph_osc_source[0][1][0] = 3;
    bank.graph_osc_source[0][1][1] = 1;
    // Kind-0 records for the three ordinals of zone 0.
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 0,
                        10,
                        11,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 1,
                        21,
                        22,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 2,
                        31,
                        32,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    Sculptor::GraphChange change = {};
    change.kind                  = Sculptor::ChangeKind::node_deleted;
    change.node_idx              = mapping.osc_nodes[1];
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));

    // The editor instrument compacts and both zones' rows follow: the
    // deleted ordinal's entries clear, ordinal 2 renumbers to 1.
    TEST(bank.bank.instruments.entries[0].layer_count == 2);
    TEST(bank.graph_osc_source[0][0][0] == 1);
    TEST(bank.graph_osc_source[0][0][1] == 0);
    TEST(bank.graph_osc_source[0][0][2] == 2);
    TEST(bank.graph_osc_source[0][1][0] == 2);
    TEST(bank.graph_osc_source[0][1][1] == 1);
    TEST(bank.graph_osc_source[0][1][2] == 0);
    // The surviving kind-0 record re-keyed from ordinal 2 to ordinal 1; the
    // deleted ordinal's record is gone.
    TEST(count_records_matching(bank, 0, 0, 0, Synth::graph_canonical_first_osc + 1) == 1);
    TEST(count_records_matching(bank, 0, 0, 0, Synth::graph_canonical_first_osc + 2) == 0);
    TEST(Sculptor::validate_editor_metadata(bank));

    // Reprojection keeps both zones' nodes and wires.
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
    TEST(mapping.osc_nodes[1] != Sculptor::pool_no_slot);
    TEST(mapping.osc_nodes[2] == Sculptor::pool_no_slot);
    TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[0] }) !=
         Sculptor::pool_no_slot);
    TEST(find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[1] }) ==
         Sculptor::pool_no_slot);
}

// Clipboard copy/paste keeps the authoring form: unwired oscillators survive
// with their parameters and wiring.
// Deleting an oscillator in a zone whose bank also carries a master-effect
// layout record (kind 4, channel == max_channels, outside channel_zones)
// must not read out of bounds and must leave the effect record untouched.
static void check_osc_delete_with_master_effect_record()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 0,
                        10,
                        11,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 1,
                        21,
                        22,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    // Master-effect record: kind 4 on the master chain, keyed by name.
    add_detached_record(&bank, Synth::max_channels, 0, 4, 0, 40, 41, Synth::ModSource::none, Synth::ModSource::none, 0);
    snprintf(bank.graph_layout[bank.graph_layout_count - 1].name, sizeof(bank.graph_layout[0].name), "All channels");

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);
    Sculptor::GraphChange change = {};
    change.kind                  = Sculptor::ChangeKind::node_deleted;
    change.node_idx              = mapping.osc_nodes[1];
    TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));

    // The master-effect record survives untouched; the surviving kind-0
    // record re-keys from ordinal 1 to ordinal 0.
    TEST(bank.graph_layout_count == 2);
    TEST(bank.graph_layout[1].kind == 4);
    TEST(bank.graph_layout[1].channel == Synth::max_channels);
    TEST(bank.graph_layout[1].x == 40 && bank.graph_layout[1].y == 41);
    TEST(count_records_matching(bank, 0, 0, 0, Synth::graph_canonical_first_osc + 0) == 1);
    TEST(Sculptor::validate_editor_metadata(bank));
}

// A late compile failure must leave the stored wiring row and instrument
// untouched (both-or-neither store contract).  Hand-built graphs only: a
// wire landing on an oscillator's base target row bypasses the applier's
// relocation and fails compile after the row is derived.
static void check_osc_store_atomic_failure()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};

    build_binding_fixture(&bank, &instrument);
    bank.graph_osc_source[0][0][0] = 1;
    bank.graph_osc_source[0][0][1] = 2;

    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    // Hand-built bad wire: osc 1's output into osc 0's pitch value row.
    const uint32_t           pitch_row      = Sculptor::osc_target_row_slot[static_cast<uint32_t>(Synth::mod_pitch)];
    const Sculptor::EndPoint osc1_out       = { mapping.osc_nodes[1], mapping.osc_output_slot };
    const Sculptor::EndPoint osc0_pitch_row = { mapping.osc_nodes[0], pitch_row };
    TEST(graph.add_connection(osc1_out, osc0_pitch_row) != Sculptor::pool_no_slot);
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    // Eventless disconnect of osc 1's sum wire: the graph now derives
    // {1,0}, which differs from the stored row {1,2}.  A premature row
    // install would fail the memcmp below even though the compile still
    // fails late on the bad wire.
    for (uint32_t c = 0; c < Sculptor::max_connections; ++c) {
        if (! graph.connection_occupied(c)) {
            continue;
        }
        const Sculptor::Connection& wire = graph.get_connection(c);
        if (wire.output.node_idx == mapping.osc_nodes[1] && wire.input.node_idx == mapping.output_node &&
            wire.input.slot_idx == mapping.output_layer_input_slot[1]) {
            graph.delete_connection(c);
        }
    }
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    uint8_t row_before[Synth::max_layers];
    memcpy(row_before, bank.graph_osc_source[0][0], sizeof(row_before));
    const Synth::Instrument instr_before = bank.bank.instruments.entries[0];
    Sculptor::GraphChange   change       = {};
    change.kind                          = Sculptor::ChangeKind::value_changed;
    change.node_idx                      = mapping.osc_nodes[0];
    change.slot_idx                      = pitch_row;

    TEST(! Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, change));

    // Both outputs untouched: the row keeps {1,2} (a premature install of
    // the derived {1,0} would fail the memcmp) and the instrument bytes
    // are unchanged.
    TEST(memcmp(row_before, bank.graph_osc_source[0][0], sizeof(row_before)) == 0);
    TEST(memcmp(&instr_before, &bank.bank.instruments.entries[0], sizeof(instr_before)) == 0);
}

static void check_osc_clipboard_preserves_unwired()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
    // An edit on the unwired node distinguishes its parameters from defaults.
    bank.bank.instruments.entries[0].layers[1].targets[Synth::mod_pitch].base_value = -5.0f;

    const std::unique_ptr<FixedArray<char, 128 * 1024>> text_storage = std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&text)[128 * 1024]                                         = text_storage->elements;
    const uint32_t len =
        Sculptor::encode_editor_instrument_json(bank, 0, 0, text, sizeof(text), Synth::InstrumentJsonDomain::clipboard);
    TEST(len > 0);
    TEST(strstr(text, "\"osc_sources\"") != nullptr);

    const std::unique_ptr<ClipboardDecode> decoded_storage        = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded                = *decoded_storage;
    uint8_t                                row[Synth::max_layers] = {};
    TEST(Synth::decode_instrument_json_for_domain(text,
                                                  len,
                                                  &decoded.instr,
                                                  decoded.envs,
                                                  &decoded.env_count,
                                                  decoded.lfos,
                                                  &decoded.lfo_count,
                                                  decoded.layout,
                                                  max_portable_records,
                                                  &decoded.layout_count,
                                                  Synth::InstrumentJsonDomain::clipboard,
                                                  row));
    // Every node, wired or not, with its parameters and the source row.
    TEST(decoded.instr.layer_count == 3);
    TEST(decoded.instr.layers[1].targets[Synth::mod_pitch].base_value == -5.0f);
    TEST(row[0] == 1 && row[1] == 0 && row[2] == 3);

    // Paste through the real replacement API.  The destination holds a
    // smaller one-layer instrument: the decoded row names ordinal 3, so
    // installing the row before the decoded instrument would fail the
    // interim validation (paste installs both together).
    const std::unique_ptr<Synth::InstrumentEditorBank> paste_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       paste         = *paste_storage;
    build_binding_fixture(&paste, &instrument);
    paste.bank.channel_zones[0][1].start_note = 61;
    const uint32_t small_slot                 = paste.bank.instruments.allocate();
    TEST(small_slot != pool_no_slot);
    Synth::Instrument small_instr = {};
    small_instr.layer_count       = 1;
    set_default_dispersion_sections(small_instr);
    small_instr.layers[0].osc_type[0]          = Synth::WaveType::sine_wave;
    paste.bank.instruments.entries[small_slot] = small_instr;
    paste.bank.channel_zones[0][1].instrument  = static_cast<uint8_t>(small_slot);
    Sculptor::set_identity_osc_sources(&paste, 0, 1);
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;
    TEST(Sculptor::replace_zone_instrument_candidate(paste,
                                                     0,
                                                     1,
                                                     decoded.instr,
                                                     decoded.envs,
                                                     decoded.env_count,
                                                     decoded.lfos,
                                                     decoded.lfo_count,
                                                     decoded.layout,
                                                     decoded.layout_count,
                                                     row,
                                                     &candidate));
    // Model the GUI's commit: the candidate becomes the live bank.
    paste = candidate;
    TEST(paste.bank.instruments.entries[paste.bank.channel_zones[0][1].instrument].layer_count == 3);
    TEST(paste.graph_osc_source[0][1][0] == 1);
    TEST(paste.graph_osc_source[0][1][1] == 0);
    TEST(paste.graph_osc_source[0][1][2] == 3);
    const std::unique_ptr<Sculptor::Graph> paste_graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       paste_graph         = *paste_graph_storage;
    Sculptor::OscGraphMapping              paste_mapping       = {};
    TEST(Sculptor::project_editor_to_graph(paste, &paste_graph, &paste_mapping, 0, 1));
    TEST(paste_mapping.osc_nodes[1] != Sculptor::pool_no_slot);
    TEST(find_osc_graph_connection(paste_graph,
                                   { paste_mapping.output_node, paste_mapping.output_layer_input_slot[1] }) ==
         Sculptor::pool_no_slot);
    // The playback form is slot order: (o0, o2).  The pasted layers equal
    // the decoded authoring layers with descriptor ids remapped into the
    // receiving bank's pools.
    Synth::Instrument playback = {};
    TEST(
        Sculptor::compile_playback_instrument(paste.bank.instruments.entries[paste.bank.channel_zones[0][1].instrument],
                                              paste.graph_osc_source[0][1],
                                              &playback));
    TEST(playback.layer_count == 2);
    for (uint32_t l = 0; l < 2; ++l) {
        const Synth::Oscillator& pasted_layer = playback.layers[l];
        const Synth::Oscillator& doc_layer    = decoded.instr.layers[l == 0 ? 0 : 2];
        TEST(memcmp(pasted_layer.osc_type, doc_layer.osc_type, sizeof(pasted_layer.osc_type)) == 0);
        TEST(pasted_layer.targets[Synth::mod_pitch].base_value == doc_layer.targets[Synth::mod_pitch].base_value);
    }
    {
        const uint32_t env_id = playback.layers[0].targets[Synth::mod_pitch].envelope_desc_id;
        TEST(env_id >= 1 && env_id <= paste.bank.envelopes.num_allocated);
        TEST(memcmp(&paste.bank.envelopes.entries[env_id - 1], &decoded.envs[1], sizeof(Synth::EnvelopeDescriptor)) ==
             0);
    }

    // An all-unwired zone copies and pastes successfully.
    {
        Synth::InstrumentEditorBank& unwired_source = bank;
        memset(unwired_source.graph_osc_source[0][0], 0, Synth::max_layers);
        const uint32_t unwired_len = Sculptor::encode_editor_instrument_json(unwired_source,
                                                                             0,
                                                                             0,
                                                                             text,
                                                                             sizeof(text),
                                                                             Synth::InstrumentJsonDomain::clipboard);
        TEST(unwired_len > 0);
        const std::unique_ptr<ClipboardDecode> unwired_storage                = std::make_unique<ClipboardDecode>();
        ClipboardDecode&                       unwired                        = *unwired_storage;
        uint8_t                                unwired_row[Synth::max_layers] = {};
        TEST(Synth::decode_instrument_json_for_domain(text,
                                                      unwired_len,
                                                      &unwired.instr,
                                                      unwired.envs,
                                                      &unwired.env_count,
                                                      unwired.lfos,
                                                      &unwired.lfo_count,
                                                      unwired.layout,
                                                      max_portable_records,
                                                      &unwired.layout_count,
                                                      Synth::InstrumentJsonDomain::clipboard,
                                                      unwired_row));
        TEST(unwired.instr.layer_count == 3);
        for (uint32_t s = 0; s < Synth::max_layers; ++s) {
            TEST(unwired_row[s] == 0);
        }
        // The all-unwired zone pastes through the replacement API too.
        TEST(Sculptor::replace_zone_instrument_candidate(paste,
                                                         0,
                                                         1,
                                                         unwired.instr,
                                                         unwired.envs,
                                                         unwired.env_count,
                                                         unwired.lfos,
                                                         unwired.lfo_count,
                                                         unwired.layout,
                                                         unwired.layout_count,
                                                         unwired_row,
                                                         &candidate));
        for (uint32_t s = 0; s < Synth::max_layers; ++s) {
            TEST(candidate.graph_osc_source[0][1][s] == 0);
        }
    }
}

// Portable export and library save drop unwired oscillators permanently: the
// document holds the playback form with kind-0 records translated to the
// wired ordinals, and no osc_sources key.
static void check_osc_export_drops_unwired()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    // Reordered transport: slot 0 wires ordinal 2, slot 2 wires ordinal 0;
    // slot 1 stays unwired.  Playback order is (o2, o0).
    bank.graph_osc_source[0][0][0] = 3;
    bank.graph_osc_source[0][0][2] = 1;
    // Moved-node records: ordinal 1 is unwired, so its record must be
    // dropped while the wired ordinals translate to their playback ranks.
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 0,
                        10,
                        11,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 1,
                        21,
                        22,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    add_detached_record(&bank,
                        0,
                        0,
                        0,
                        Synth::graph_canonical_first_osc + 2,
                        31,
                        32,
                        Synth::ModSource::none,
                        Synth::ModSource::none,
                        0);
    // A generator record rides the same translation: envelope 1 serves the
    // wired layers, its position must survive the round trip.
    add_detached_record(&bank, 0, 0, 1, 1, 55, 56, Synth::ModSource::none, Synth::ModSource::none, 1);

    const std::unique_ptr<FixedArray<char, 128 * 1024>> text_storage = std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&text)[128 * 1024]                                         = text_storage->elements;
    const uint32_t len = Sculptor::encode_editor_instrument_json(bank,
                                                                 0,
                                                                 0,
                                                                 text,
                                                                 sizeof(text),
                                                                 Synth::InstrumentJsonDomain::validated_bank);
    TEST(len > 0);
    TEST(strstr(text, "\"osc_sources\"") == nullptr);

    const std::unique_ptr<ClipboardDecode> decoded_storage = std::make_unique<ClipboardDecode>();
    ClipboardDecode&                       decoded         = *decoded_storage;
    TEST(Synth::decode_instrument_json_for_domain(text,
                                                  len,
                                                  &decoded.instr,
                                                  decoded.envs,
                                                  &decoded.env_count,
                                                  decoded.lfos,
                                                  &decoded.lfo_count,
                                                  decoded.layout,
                                                  max_portable_records,
                                                  &decoded.layout_count,
                                                  Synth::InstrumentJsonDomain::validated_bank,
                                                  nullptr));
    // The playback form: two layers in slot order, o1 absent.
    TEST(decoded.instr.layer_count == 2);
    TEST(memcmp(&decoded.instr.layers[0], &instrument.layers[2], sizeof(Synth::Oscillator)) == 0);
    TEST(memcmp(&decoded.instr.layers[1], &instrument.layers[0], sizeof(Synth::Oscillator)) == 0);
    // The kind-0 records name the playback form's canonical indices:
    // rank 0 is ordinal 2 (x=31), rank 1 is ordinal 0 (x=10); the
    // unwired ordinal's record is gone and the generator record rides
    // along with its position.
    TEST(decoded.layout_count == 3);
    TEST(decoded.layout[0].kind == 0 && decoded.layout[0].canonical_index == Synth::graph_canonical_first_osc + 0);
    TEST(decoded.layout[1].kind == 0 && decoded.layout[1].canonical_index == Synth::graph_canonical_first_osc + 1);
    TEST(decoded.layout[2].kind == 1);
    TEST(decoded.layout[0].x == 31 && decoded.layout[0].y == 32);
    TEST(decoded.layout[1].x == 10 && decoded.layout[1].y == 11);
    TEST(decoded.layout[2].x == 55 && decoded.layout[2].y == 56);

    // The library round trip gives the same playback form.
    strcpy(bank.instrument_names[0], "Drops Unwired");
    const char* const path = "synth_osc_drops_unwired.tmp";
    remove(path);
    TEST(Synth::save_library_record(path, "Regression", "Drops Unwired", &bank, 0) == 0);
    Synth::LibraryEntry entry = {};
    TEST(Synth::read_library_index(path, &entry, 1) == 1);
    const std::unique_ptr<Synth::InstrumentEditorBank> destination_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& destination = *destination_storage;
    uint16_t                     slot        = 0xBEEF;
    TEST(Synth::load_library_instrument(path, &entry, &destination, 1, &slot));
    const Synth::Instrument& loaded = destination.bank.instruments.entries[slot];
    // The loaded instrument is the playback form: (o2, o0).
    TEST(loaded.layer_count == 2);
    TEST(memcmp(&loaded.layers[0], &instrument.layers[2], sizeof(Synth::Oscillator)) == 0);
    TEST(memcmp(&loaded.layers[1], &instrument.layers[0], sizeof(Synth::Oscillator)) == 0);
    TEST(loaded.layers[0].targets[Synth::mod_pitch].envelope_desc_id == 0);
    TEST(loaded.layers[1].targets[Synth::mod_pitch].envelope_desc_id == 2);
    TEST(destination.graph_osc_source[1][0][0] == 1);
    TEST(destination.graph_osc_source[1][0][1] == 2);
    // Reordered positions survive reload: the record riding playback rank
    // 0 (ordinal 2) keeps x=31, rank 1 (ordinal 0) keeps x=10, and the
    // generator record keeps its position under the remapped descriptor.
    uint32_t loaded_osc_at[3]         = {};
    uint32_t loaded_generator_records = 0;
    for (uint32_t i = 0; i < destination.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = destination.graph_layout[i];
        if (record.channel != 1 || record.zone != 0) {
            continue;
        }
        if (record.kind == 0 && record.index >= Synth::graph_canonical_first_osc &&
            record.index < Synth::graph_canonical_first_env) {
            loaded_osc_at[record.index - Synth::graph_canonical_first_osc] = static_cast<uint32_t>(record.x);
        }
        if (record.kind == 1) {
            ++loaded_generator_records;
            TEST(record.x == 55 && record.y == 56);
            TEST(record.index >= 1 && record.index <= destination.bank.envelopes.num_allocated);
        }
    }
    TEST(loaded_osc_at[0] == 31);
    TEST(loaded_osc_at[1] == 10);
    TEST(loaded_generator_records == 1);
    TEST(Sculptor::validate_editor_metadata(destination));
    remove(path);
}

// Zero-wired portable export stays refused; the clipboard still copies the
// all-unwired zone (covered in check_osc_clipboard_preserves_unwired).
static void check_osc_export_zero_wired_refused()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};

    build_binding_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    memset(bank.graph_osc_source[0][0], 0, Synth::max_layers);

    const std::unique_ptr<FixedArray<char, 128 * 1024>> text_storage = std::make_unique<FixedArray<char, 128 * 1024>>();
    char (&text)[128 * 1024]                                         = text_storage->elements;
    TEST(Sculptor::encode_editor_instrument_json(bank,
                                                 0,
                                                 0,
                                                 text,
                                                 sizeof(text),
                                                 Synth::InstrumentJsonDomain::validated_bank) == 0);
    TEST(Sculptor::encode_editor_instrument_json(bank,
                                                 0,
                                                 0,
                                                 text,
                                                 sizeof(text),
                                                 Synth::InstrumentJsonDomain::clipboard) != 0);
}

static void check_osc_undo_source_rows()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::Instrument                                  instrument   = {};
    build_binding_fixture(&bank, &instrument);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};
    TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping, 0, 0));
    graph.set_validator(Sculptor::osc_graph_validate, &mapping);
    Sculptor::GraphChange discard[Sculptor::max_pending_changes] = {};
    (void)graph.take_changes(discard, Sculptor::max_pending_changes);

    // Five undo groups, one per step; each entry holds the pre-step bank.
    Sculptor::UndoRedo undo = {};
    const std::unique_ptr<FixedArray<uint8_t, 8 * sizeof(Synth::InstrumentEditorBank) + 8 * sizeof(uint32_t)>>
        undo_buf_storage =
            std::make_unique<FixedArray<uint8_t, 8 * sizeof(Synth::InstrumentEditorBank) + 8 * sizeof(uint32_t)>>();
    uint8_t (&undo_buf)[8 * sizeof(Synth::InstrumentEditorBank) + 8 * sizeof(uint32_t)] = undo_buf_storage->elements;
    undo.init(undo_buf);
    const uint32_t                                     steps             = 5;
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage[5] = {
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>()
    };
    const std::unique_ptr<Synth::InstrumentEditorBank> after_storage[5] = {
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>(),
        std::make_unique<Synth::InstrumentEditorBank>()
    };
    for (uint32_t step = 0; step < steps; ++step) {
        *before_storage[step] = bank;
        undo.init_undo_push();
        undo.push(&bank, sizeof(bank));
        undo.push(step + 1u);
        TEST(undo.finish_undo_push());
        switch (step) {
            case 0:
                // Disconnect: the wire into slot 1 goes, the oscillator stays.
                TEST(disconnect_osc_sum_slot(&bank, graph, mapping, 1));
                break;
            case 1: {
                // Reconnect with auto-move: osc 2 (still feeding slot 2) wires
                // into slot 1, its old wire auto-deletes.
                const Sculptor::EndPoint osc2_out = { mapping.osc_nodes[2], mapping.osc_output_slot };
                const Sculptor::EndPoint slot1_in = { mapping.output_node, mapping.output_layer_input_slot[1] };
                TEST(Sculptor::osc_graph_validate(&mapping, graph, osc2_out, slot1_in));
                TEST(graph.add_connection(osc2_out, slot1_in) != Sculptor::pool_no_slot);
                Sculptor::GraphChange added[Sculptor::max_pending_changes] = {};
                TEST(graph.take_changes(added, Sculptor::max_pending_changes) == 1);
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, added[0]));
                break;
            }
            case 2: {
                // Retarget auto-move: osc 0's wire moves its output end to
                // osc 2, which already feeds slot 1; that old wire auto-deletes.
                const Sculptor::EndPoint osc0_out = { mapping.osc_nodes[0], mapping.osc_output_slot };
                const Sculptor::EndPoint osc2_out = { mapping.osc_nodes[2], mapping.osc_output_slot };
                const uint32_t           osc0_wire =
                    find_fx_graph_connection(graph,
                                             osc0_out,
                                             { mapping.output_node, mapping.output_layer_input_slot[0] });
                TEST(osc0_wire != Sculptor::pool_no_slot);
                TEST(graph.move_connection_end(osc0_wire, true, osc2_out));
                Sculptor::GraphChange changed[Sculptor::max_pending_changes] = {};
                TEST(graph.take_changes(changed, Sculptor::max_pending_changes) == 1);
                TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, changed[0]));
                break;
            }
            case 3: {
                // Pulled-slot Add, modeled as the consumer does it: a visible
                // slot (the gray add row at the run's end), a fresh
                // production-default oscillator (sine), the row entry, and
                // the new layer's layout record.
                Synth::Instrument& editor      = bank.bank.instruments.entries[0];
                const uint32_t     target_slot = 1;
                TEST(target_slot <= Sculptor::osc_row_run_length(bank.graph_osc_source[0][0]));
                TEST(bank.graph_osc_source[0][0][target_slot] == 0);
                Synth::Oscillator fresh                  = {};
                fresh.osc_type[0]                        = Synth::WaveType::sine_wave;
                const uint32_t ordinal                   = editor.layer_count;
                editor.layers[ordinal]                   = fresh;
                bank.graph_osc_source[0][0][target_slot] = static_cast<uint8_t>(ordinal + 1);
                ++editor.layer_count;
                Synth::GraphNodeLayout record = {};
                record.channel                = 0;
                record.zone                   = 0;
                record.kind                   = 0;
                record.index                  = static_cast<uint8_t>(Synth::graph_canonical_first_osc + ordinal);
                record.x                      = 70;
                record.y                      = 71;
                bank.graph_layout[bank.graph_layout_count++] = record;
                break;
            }
            case 4: {
                // Canvas Add: the lowest free slot takes the fresh oscillator
                // (production defaults, plus the layout record).
                Synth::Instrument& editor = bank.bank.instruments.entries[0];
                const uint32_t     slot   = Sculptor::osc_lowest_free_slot(bank.graph_osc_source[0][0]);
                TEST(slot != Sculptor::pool_no_slot);
                Synth::Oscillator fresh           = {};
                fresh.osc_type[0]                 = Synth::WaveType::sine_wave;
                const uint32_t ordinal            = editor.layer_count;
                editor.layers[ordinal]            = fresh;
                bank.graph_osc_source[0][0][slot] = static_cast<uint8_t>(ordinal + 1);
                ++editor.layer_count;
                Synth::GraphNodeLayout record = {};
                record.channel                = 0;
                record.zone                   = 0;
                record.kind                   = 0;
                record.index                  = static_cast<uint8_t>(Synth::graph_canonical_first_osc + ordinal);
                record.x                      = 72;
                record.y                      = 73;
                bank.graph_layout[bank.graph_layout_count++] = record;
                break;
            }
        }
        *after_storage[step] = bank;
    }
    // The end state: three wired slots in slot order, two unwired nodes,
    // and layout records for both added oscillators.
    TEST(bank.bank.instruments.entries[0].layer_count == 5);
    TEST(bank.graph_osc_source[0][0][0] == 3);
    TEST(bank.graph_osc_source[0][0][1] == 4);
    TEST(bank.graph_osc_source[0][0][2] == 5);
    TEST(count_records_matching(bank, 0, 0, 0, Synth::graph_canonical_first_osc + 3) == 1);
    TEST(count_records_matching(bank, 0, 0, 0, Synth::graph_canonical_first_osc + 4) == 1);

    // Undo all: every step restores its pre-step bank byte-for-byte.  The
    // post-step state of each step rides onto the redo stack as it goes.
    for (uint32_t step = steps; step > 0; --step) {
        TEST(! undo.undo_empty());
        TEST(undo.init_undo());
        TEST(undo.pop_u32() == step);
        undo.pop(&bank, sizeof(bank));
        undo.finish_undo();
        TEST(memcmp(&bank, before_storage[step - 1].get(), sizeof(bank)) == 0);
        undo.init_redo_push();
        undo.push(after_storage[step - 1].get(), sizeof(bank));
        undo.push(step);
        TEST(undo.finish_redo_push());
    }
    TEST(undo.undo_empty());

    // Redo all: every step restores its post-step bank byte-for-byte.
    for (uint32_t step = 0; step < steps; ++step) {
        TEST(! undo.redo_empty());
        TEST(undo.init_redo());
        TEST(undo.pop_u32() == step + 1);
        undo.pop(&bank, sizeof(bank));
        undo.finish_redo();
        TEST(memcmp(&bank, after_storage[step].get(), sizeof(bank)) == 0);
    }
    TEST(undo.redo_empty());
}

// Oscillator-pane CC authoring regressions (M6).  The registry shares the
// per-channel added_cc_mask with the fx pane; the projection adds one MIDI
// Inputs output dot per set bit and wires the layer bindings' cc inputs
// through it; the validator confines CC dots to the Source A/B rows; bank
// validation and the routing encoder admit cc with cc_number 16..31 and
// keep the non-CC-must-be-zero rule.  The menu commands are GUI-only
// (sculptor_instr_edit.cpp is not linked into this suite), so the tests
// drive the mask bits directly - the same shape the fx-pane registry tests
// use.
// T-M6-10: the editor's move commit persists a moved bound node as a kind-0
// canonical-index record; adding a CC afterwards re-projects the graph, and
// the moved position must survive that re-projection.
static void osc_inputs_move_reprojection_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    const std::unique_ptr<Sculptor::Graph>             regraph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   regraph         = *regraph_storage;
    Sculptor::OscGraphMapping                          mapping         = {};
    Sculptor::OscGraphMapping                          remapping       = {};
    Sculptor::GraphChange                              discarded[Sculptor::max_pending_changes] = {};
    build_zone_fixture(&source, &instrument);
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    // Drag the MIDI Inputs node: the widget moves it live and pushes no events.
    const vmath::vec2 moved(304.0f, 192.0f);
    graph.set_node_layout(mapping.input_node, moved, 0, 0);
    // The move commit syncs the layout into the candidate bank the way
    // sync_osc_graph_layout keys bound nodes: kind 0, canonical index 0.
    TEST(Sculptor::find_record(source, 0, 0, 0, 0, 0) < 0);
    TEST(source.graph_layout_count < Synth::max_graph_records);
    Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
    memset(static_cast<void*>(&record), 0, sizeof(record));
    record.channel = 0;
    record.zone    = 0;
    record.kind    = 0;
    record.index   = 0;
    record.x       = moved.x;
    record.y       = moved.y;
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::validate_editor_metadata(source));
    // Adding CC 16 commits the mask bit and re-projects the graph.
    source.added_cc_mask[0] = 1u << 0;
    TEST(Sculptor::project_editor_to_graph(source, &regraph, &remapping));
    TEST(remapping.input_node != Sculptor::pool_no_slot);
    TEST(regraph.node(remapping.input_node).position.x == moved.x);
    TEST(regraph.node(remapping.input_node).position.y == moved.y);
    TEST(regraph.node(remapping.input_node).slots.num_allocated == Sculptor::num_osc_graph_inputs + 1);
}

// The oscillator and effect projections tag every modulator block slot with
// child_row = true, and leave base-value rows, fixed slots, and
// non-oscillator / non-effect node slots at child_row = false.
static void child_row_projection_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::OscGraphMapping                          mapping        = {};
    build_zone_fixture(&source, &instrument);
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));

    const uint32_t osc_node = mapping.osc_nodes[0];
    TEST(osc_node != Sculptor::pool_no_slot);
    const Sculptor::Node& osc = graph.node(osc_node);
    for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
        // Duty, pad tone, mix, fm and filter-spread rows nest under
        // their parent rows; volume, pan and pitch stay at the top level.
        // Bands beyond the count project no rows at all.
        if (osc_target_band_of_target(t) > 0) {
            continue;
        }
        TEST(osc.slots.entries[Sculptor::osc_target_row(t)].child_row == (t >= 3));
        TEST(osc.slots.entries[Sculptor::osc_target_env_dot(t)].child_row);
        for (uint32_t f = 0; f <= 3; ++f) {
            TEST(osc.slots.entries[Sculptor::osc_target_lfo_dot(t) + f].child_row);
        }
        for (uint32_t input = 0; input < Synth::max_mod_inputs; ++input) {
            for (uint32_t f = 0; f <= 2; ++f) {
                TEST(osc.slots.entries[Sculptor::osc_target_src_dot(t, input) + f].child_row);
            }
        }
    }
    TEST(! osc.slots.entries[mapping.osc_output_slot].child_row);
    for (uint32_t s = 0; s < osc.slots.num_allocated; ++s) {
        uint32_t   block_target       = 0;
        uint32_t   block_field        = 0;
        const bool is_block           = Sculptor::osc_target_block_field(s, &block_target, &block_field);
        bool       child_row_expected = is_block;
        for (uint32_t t = 3; t < Synth::num_mod_targets; ++t) {
            child_row_expected = child_row_expected || s == Sculptor::osc_target_row(t);
        }
        // Pitch offset, octave offset, brightness, the dispersion row and its block, the band header rows, the band
        // parameter rows and their blocks (everything in a band block but
        // the two click slots) and the fir slope row nest under their
        // parents.
        const bool band_header = s >= Sculptor::osc_band_first_slot &&
                                 (s - Sculptor::osc_band_first_slot) % Sculptor::osc_band_block_stride == 0u;
        child_row_expected     = child_row_expected || band_header || s == 28 || s == 29 || (s >= 30 && s <= 41) ||
                                 s == 148 || s == 192 ||
                                 (s >= Sculptor::osc_band_first_slot + 3u &&
                                  s < Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride);
        TEST(osc.slots.entries[s].child_row == child_row_expected);
    }

    const uint32_t non_osc_nodes[] = { mapping.input_node, mapping.instrument_node, mapping.output_node };
    for (uint32_t node_idx : non_osc_nodes) {
        TEST(node_idx != Sculptor::pool_no_slot);
        const Sculptor::Node& node = graph.node(node_idx);
        for (uint32_t s = 0; s < node.slots.num_allocated; ++s) {
            TEST(! node.slots.entries[s].child_row);
        }
    }
    TEST(mapping.detached_count >= 2);
    for (uint32_t i = 0; i < mapping.detached_count; ++i) {
        const uint32_t node_idx = mapping.detached[i].node_idx;
        TEST(node_idx != Sculptor::pool_no_slot);
        const Sculptor::Node& node = graph.node(node_idx);
        for (uint32_t s = 0; s < node.slots.num_allocated; ++s) {
            TEST(! node.slots.entries[s].child_row);
        }
    }

    Sculptor::EffectGraphMapping fx_mapping = {};
    Synth::EffectChainBinding&   chain      = add_cc_chain_fixture(&source, 0);
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::compressor);
    chain.effects[0].enabled = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &fx_mapping));

    const uint32_t fx_node = fx_mapping.effect_nodes[0];
    TEST(fx_node != Sculptor::pool_no_slot);
    const Sculptor::Node& fx = graph.node(fx_node);
    TEST(! fx.slots.entries[0].child_row);
    TEST(! fx.slots.entries[1].child_row);
    TEST(! fx.slots.entries[2].child_row);
    for (uint32_t param = 0; param < Sculptor::effect_type_info(Synth::EffectType::compressor).num_params; ++param) {
        const uint32_t base = Sculptor::fx_param_row(param);
        TEST(! fx.slots.entries[base].child_row);
        for (uint32_t field = 1; field < Sculptor::fx_param_stride; ++field) {
            TEST(fx.slots.entries[base + field].child_row);
        }
    }
    const uint32_t non_fx_nodes[] = { fx_mapping.input_node, fx_mapping.output_node, fx_mapping.midi_node };
    for (uint32_t node_idx : non_fx_nodes) {
        TEST(node_idx != Sculptor::pool_no_slot);
        const Sculptor::Node& node = graph.node(node_idx);
        for (uint32_t s = 0; s < node.slots.num_allocated; ++s) {
            TEST(! node.slots.entries[s].child_row);
        }
    }
}

// A four-layer instrument with an appended oscillator projects: the new
// layer's node carries its waveforms (A sawtooth, B pulse FM), its
// bindings and its FIR slope.
static void osc_layer_projection_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    build_zone_fixture(&source, &instrument);
    TEST(instrument.layer_count == 1);

    instrument.layers[1].osc_type[0]                                 = Synth::WaveType::sine_wave;
    instrument.layers[2].osc_type[0]                                 = Synth::WaveType::sawtooth_wave;
    instrument.layers[2].osc_type[1]                                 = Synth::WaveType::pulse_wave;
    instrument.layers[2].osc_mode                                    = Synth::osc_mode_fm;
    instrument.layers[2].mod_ratio                                   = 3.0f;
    instrument.layers[2].targets[Synth::mod_volume].envelope_desc_id = 2;
    instrument.layers[2].targets[Synth::mod_volume].lfo_desc_id      = 1;
    instrument.layers[2].targets[Synth::mod_volume].lfo_depth        = 0.25f;
    instrument.layers[2].targets[Synth::mod_pitch].envelope_desc_id  = 1;
    instrument.layers[2].fir_slope_percent                           = 37;
    instrument.layer_count                                           = 3;
    set_default_dispersion_sections(instrument);

    const Synth::Oscillator appended = instrument.layers[2];
    Synth::Instrument&      entry    = source.bank.instruments.entries[0];
    entry.layers[3]                  = appended;
    entry.layer_count                = 4;
    set_default_dispersion_sections(entry);
    Sculptor::set_identity_osc_sources(&source, 0, 0);

    TEST(Synth::validate_instrument_bank(&source.bank));
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::OscGraphMapping              mapping       = {};

    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
    TEST(mapping.osc_nodes[3] != Sculptor::pool_no_slot);
    TEST(memcmp(&entry.layers[3], &appended, sizeof(Synth::Oscillator)) == 0);
    uint32_t wave_slot = Sculptor::pool_no_slot;
    TEST(find_graph_slot(graph, mapping.osc_nodes[3], "Waveform A", &wave_slot));
    TEST(graph.node(mapping.osc_nodes[3]).slots.entries[wave_slot].value.list_index == 1);
    TEST(entry.layers[3].targets[Synth::mod_volume].envelope_desc_id == 2);
    TEST(entry.layers[3].targets[Synth::mod_volume].lfo_desc_id == 1);
    TEST(entry.layers[3].targets[Synth::mod_pitch].envelope_desc_id == 1);
    TEST(entry.layers[3].fir_slope_percent == 37); // the projection reads the stored layer
    uint32_t wave_b_slot = Sculptor::pool_no_slot;
    TEST(find_graph_slot(graph, mapping.osc_nodes[3], "Waveform B", &wave_b_slot));
    TEST(graph.node(mapping.osc_nodes[3]).slots.entries[wave_b_slot].value.list_index == 3);
}

// One layer whose twelve targets each bind an envelope, an LFO and two MIDI
// inputs.  Volume keeps a volume-shaped descriptor (id 1); the other eleven
// targets share one non-volume descriptor (id 2).  One LFO descriptor (id 1)
// serves every cell, so the projection derives exactly three generator
// instances and no parameter nodes.
static void build_full_block_fixture(Synth::InstrumentBank* bank, Synth::Instrument* instrument)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    *instrument = {};
    TEST(bank->envelopes.allocate() == 0);
    TEST(bank->envelopes.allocate() == 1);
    Synth::EnvelopeDescriptor& vol_env   = bank->envelopes.entries[0];
    vol_env.num_points                   = 2;
    vol_env.sustain_first_point          = 0;
    vol_env.sustain_last_point           = 1;
    vol_env.min_value                    = 0.0f;
    vol_env.min_max_delta                = 1.0f / 65535.0f;
    vol_env.points[0]                    = { 0, 0 };
    vol_env.points[1]                    = { 100, 0xFFFF };
    bank->envelopes.entries[1]           = vol_env;
    bank->envelopes.entries[1].min_value = -0.5f; // non-volume shape
    TEST(bank->lfos.allocate() == 0);
    Synth::LFODescriptor& lfo = bank->lfos.entries[0];
    lfo.wave                  = Synth::WaveType::sine_wave;
    lfo.period_ms             = 250;
    lfo.min_value             = -1.0f;
    lfo.min_max_delta         = 2.0f;

    instrument->layer_count           = 1;
    instrument->layers[0].osc_type[0] = Synth::WaveType::sawtooth_wave;
    for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
        Synth::TargetBinding& gen = instrument->layers[0].targets[t];
        gen.envelope_desc_id      = t == Synth::mod_volume ? 1 : 2;
        gen.lfo_desc_id           = 1;
        gen.envelope_depth        = 0.25f + 0.125f * static_cast<float>(t);
        gen.lfo_op                = (t % 2) != 0 ? Synth::SourceOp::multiply : Synth::SourceOp::add;
        gen.lfo_depth             = 0.25f * static_cast<float>(t + 1);
        gen.lfo_rate_scale_ms     = 10.0f * static_cast<float>(t + 1);
        gen.num_inputs            = 2;
        gen.inputs[0].source      = Synth::ModSource::velocity;
        gen.inputs[0].op          = Synth::SourceOp::add;
        gen.inputs[0].scale       = 0.5f * static_cast<float>(t + 1);
        gen.inputs[1].source      = Synth::ModSource::mod_wheel;
        gen.inputs[1].op          = Synth::SourceOp::multiply;
        gen.inputs[1].scale       = -0.25f * static_cast<float>(t + 1);
    }
}

// Checks one target block's dot/field grammar and its seeded values against
// the binding that produced it: the envelope row is the dot plus its nested
// depth row, the LFO row carries op + depth + rate scale, and the source
// rows carry op + scale.
static void check_target_block_grammar(const Sculptor::Node&       osc,
                                       uint32_t                    target,
                                       const Synth::TargetBinding& gen,
                                       Synth::FilterType           fir_type)
{

    // Targets from duty a on nest under their parent rows (volume, pan and
    // pitch stay at the top level), so their whole block shifts one deeper;
    // the band rows nest one level under their Band N header, so their
    // blocks shift two deeper (level 3, rate/amount 4).  The legacy
    // single-edge pair (spread, slope) keeps the legacy block level 2
    // under a non-bandpass filter, where it renders as the Cutoff /
    // FIR Slope rows.
    const bool band_row = osc_target_band_of_target(target) >= 0;
    const bool legacy_pair =
        target == static_cast<uint32_t>(Synth::mod_fir_spread) || target == static_cast<uint32_t>(Synth::mod_fir_slope);
    const uint8_t  level_base = ! band_row                                       ? (target >= 3 ? 2u : 1u)
                                : legacy_pair && fir_type != Synth::fir_bandpass ? 2u
                                                                                 : 3u;
    const uint32_t base       = Sculptor::osc_target_block_base(target);
    TEST(Sculptor::osc_target_row(target) == t7_target_row_slots[target]);
    TEST(base == t7_target_row_slots[target] + 1); // the block follows its row
    TEST(osc.slots.entries[base - 1].connectable); // the row slot itself
    TEST(Sculptor::osc_target_env_dot(target) == base + 0);
    TEST(Sculptor::osc_target_env_depth(target) == base + 1);
    TEST(Sculptor::osc_target_lfo_dot(target) == base + 2);
    TEST(Sculptor::osc_target_src_dot(target, 0) == base + 6);
    TEST(Sculptor::osc_target_src_dot(target, 1) == base + 9);

    const Sculptor::Slot& env_dot = osc.slots.entries[base];
    TEST(env_dot.kind == Sculptor::SlotKind::input);
    TEST(strncmp(env_dot.name, "Envelope", sizeof(env_dot.name)) == 0);
    TEST(env_dot.row_group == 4 * target + 4); // the env dot shares its line with the depth
    const Sculptor::Slot& env_depth = osc.slots.entries[base + t7_env_depth_field];
    TEST(env_depth.property_type == Sculptor::PropertyType::real);
    TEST(env_depth.name[0] == '\0'); // unlabeled: the dot's label names the line
    TEST(env_depth.row_group == 4 * target + 4);
    TEST(env_depth.child_level == level_base); // one line with the dot
    TEST(env_depth.real_bounded);              // rendered as a slider
    TEST(env_depth.real_min == 0.0f && env_depth.real_max == 1.0f);
    TEST(approx(env_depth.value.real, gen.envelope_depth, 1.0e-6f));

    const Sculptor::Slot& lfo_dot = osc.slots.entries[base + 2];
    TEST(lfo_dot.kind == Sculptor::SlotKind::input);
    TEST(strncmp(lfo_dot.name, "LFO Op", sizeof(lfo_dot.name)) == 0);
    const uint8_t lfo_group = static_cast<uint8_t>(4 * target + 1);
    TEST(osc.slots.entries[base + t7_lfo_op_field].name[0] == '\0'); // unlabeled: the dot's label names the line
    TEST(osc.slots.entries[base + t7_lfo_op_field].property_type == Sculptor::PropertyType::list);
    TEST(osc.slots.entries[base + t7_lfo_op_field].num_list_options == 2);
    TEST(strncmp(osc.slots.entries[base + t7_lfo_depth_field].name,
                 "Depth",
                 sizeof(osc.slots.entries[base + t7_lfo_depth_field].name)) == 0);
    TEST(strncmp(osc.slots.entries[base + t7_rate_field].name,
                 "Rate x",
                 sizeof(osc.slots.entries[base + t7_rate_field].name)) == 0);
    for (uint32_t f = 2; f <= 3; f++) {
        TEST(osc.slots.entries[base + f].row_group == lfo_group);
    }
    // Depth and Rate x nest one tree level deeper on their own lines.
    TEST(osc.slots.entries[base + t7_lfo_depth_field].row_group == 0);
    TEST(osc.slots.entries[base + t7_lfo_depth_field].child_level == level_base + 1);
    TEST(osc.slots.entries[base + t7_rate_field].row_group == 0);
    TEST(osc.slots.entries[base + t7_rate_field].child_level == level_base + 1);
    TEST(osc.slots.entries[base + t7_lfo_op_field].value.list_index == static_cast<uint32_t>(gen.lfo_op));
    TEST(approx(osc.slots.entries[base + t7_lfo_depth_field].value.real, gen.lfo_depth, 1.0e-6f));
    TEST(approx(osc.slots.entries[base + t7_rate_field].value.real, gen.lfo_rate_scale_ms, 1.0e-4f));

    for (uint32_t i = 0; i < Synth::max_mod_inputs; i++) {
        const uint32_t        dot     = Sculptor::osc_target_src_dot(target, i);
        const Sculptor::Slot& src_dot = osc.slots.entries[dot];
        TEST(src_dot.kind == Sculptor::SlotKind::input);
        TEST(strncmp(src_dot.name, i == 0 ? "Source A Op" : "Source B Op", sizeof(src_dot.name)) == 0);
        const uint8_t src_group = static_cast<uint8_t>(4 * target + 2 + i);
        for (uint32_t f = 0; f <= 1; f++) {
            TEST(osc.slots.entries[dot + f].row_group == src_group);
        }
        // Amount nests one tree level deeper on its own line.
        TEST(osc.slots.entries[dot + 2].row_group == 0);
        TEST(osc.slots.entries[dot + 2].child_level == level_base + 1);
        const Sculptor::Slot& op = osc.slots.entries[dot + 1];
        TEST(op.property_type == Sculptor::PropertyType::list);
        TEST(op.num_list_options == 2);
        TEST(op.value.list_index == static_cast<uint32_t>(gen.inputs[i].op));
        const Sculptor::Slot& scale = osc.slots.entries[dot + 2];
        TEST(strncmp(scale.name, "Amount", sizeof(scale.name)) == 0);
        TEST(approx(scale.value.real, gen.inputs[i].scale, 1.0e-6f));
    }
}

// T-M7-4 (O1): no parameter node is ever projected.  No node keeps the
// 13-slot parameter grammar.
static void check_no_param_nodes(const Sculptor::Graph& graph)
{
    for (uint32_t i = 0; i < Sculptor::max_nodes; i++) {
        if (! graph.node_occupied(i)) {
            continue;
        }
        TEST(graph.node(i).slots.num_allocated != 13);
    }
}

// One editor-bank projection of the small fixture, drained; false skips the
// caller's block.
static bool begin_small_graph(Synth::InstrumentEditorBank* bank,
                              Sculptor::Graph*             graph,
                              Sculptor::OscGraphMapping*   mapping)
{
    Synth::Instrument instrument = {};
    build_zone_fixture(bank, &instrument);
    if (! Sculptor::project_editor_to_graph(*bank, graph, mapping)) {
        return false;
    }
    Sculptor::GraphChange discard[8] = {};
    (void)graph->take_changes(discard, 8);
    return true;
}

static void osc_direct_modulator_regressions()
{
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes] = {};

    // T-M7-1 (O4/O9): the per-target block grammar on one oscillator with
    // all twelve targets fully bound; every family row carries its seeded
    // binding values, and the waveform/mode guard keeps working on the
    // widened node.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        build_full_block_fixture(&bank, &instrument);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_no_param_nodes(graph);
        TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
        if (mapping.osc_nodes[0] != Sculptor::pool_no_slot) {
            const Sculptor::Node& osc = graph.node(mapping.osc_nodes[0]);
            // 35 fixed slots plus Filter Type and one band block (the
            // fixture keeps one band), then twelve 12-slot target blocks.
            TEST(osc.slots.num_allocated ==
                 Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride * instrument.layers[0].fir_band_count);
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                if (osc_target_band_of_target(t) >= static_cast<int32_t>(instrument.layers[0].fir_band_count) ||
                    ! t7_target_has_row(instrument.layers[0], t)) {
                    continue;
                }
                check_target_block_grammar(osc, t, instrument.layers[0].targets[t], instrument.layers[0].fir_type);
            }
            // Regression: the waveform/mode guard must not early-out on
            // the widened slot count.  Wave B starts off and the mode starts
            // on blend, so the duty b / mix mode / waveform mix / fm depth / fm
            // ratio rows plus both pad tone families are disabled and nothing else is.
            const uint32_t wave_b_slot    = 42; // waveform b, layout contract
            const uint32_t mode_slot      = 69; // mix mode, layout contract
            const uint32_t fm_depth_slot  = Sculptor::osc_target_row(Synth::mod_fm_index);
            const uint32_t fm_ratio_slot  = 96; // fm ratio, layout contract
            const uint32_t gated_slots[4] = { Sculptor::osc_target_row(Synth::mod_duty1),
                                              mode_slot,
                                              fm_depth_slot,
                                              fm_ratio_slot };
            for (uint32_t g = 0; g < 4; g++) {
                TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, gated_slots[g]));
            }
            TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, 1));
            TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, Sculptor::osc_target_row(Synth::mod_volume)));
            // Wave B on and FM mode: the guard rules that no longer apply
            // release their rows.
            Sculptor::PropertyValue selection = {};
            selection.list_index              = 1;
            graph.set_slot_value(mapping.osc_nodes[0], wave_b_slot, selection); // Waveform B
            selection.list_index = 1;
            graph.set_slot_value(mapping.osc_nodes[0], mode_slot, selection); // Mix Mode: FM
            for (uint32_t g = 1; g < 4; g++) {
                TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, gated_slots[g]));
            }
            // Sine wave b keeps the duty b family gated.
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, gated_slots[0]));
            // Waveform mix drives blend and ring mod only, so it stays gated in FM mode.
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, Sculptor::osc_target_row(Synth::mod_osc_mix)));
            selection.list_index = 0;
            graph.set_slot_value(mapping.osc_nodes[0], mode_slot, selection); // blend again
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm_depth_slot));
            TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm_ratio_slot));
            TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, Sculptor::osc_target_row(Synth::mod_osc_mix)));
            TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, 4));
        }
    }

    // T-M7-2 (O2/O3, C2/C3): wires dropped on base target rows commit onto
    // their family's dot in the dropped-on target's block; the compiled
    // bindings carry the connected generators; re-projection reproduces the
    // family wiring; the relocation is eventless (second drain empty).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& candidate = *candidate_storage;
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            // A CC dot for the fourth family.
            source.added_cc_mask[0] = 1u << 0;
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);

            const int32_t env_inst = find_detached_by_desc(t7_mapping, 1, 1); // pitch env, non-volume
            const int32_t lfo_inst = find_detached_by_desc(t7_mapping, 2, 1);
            TEST(env_inst >= 0 && lfo_inst >= 0);
            if (env_inst >= 0 && lfo_inst >= 0) {
                const Sculptor::EndPoint env_out      = { t7_mapping.detached[env_inst].node_idx,
                                                          t7_mapping.env_output_slot };
                const Sculptor::EndPoint lfo_out      = { t7_mapping.detached[lfo_inst].node_idx,
                                                          t7_mapping.lfo_output_slot };
                const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
                const Sculptor::EndPoint velocity_out = { t7_mapping.input_node,
                                                          t7_mapping.input_source_slots[velocity_idx] };
                char                     cc_label[32];
                Sculptor::fx_cc_pair_label(Synth::cc_msb_base, cc_label, sizeof(cc_label));
                uint32_t cc_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(t7_graph, t7_mapping.input_node, cc_label, &cc_slot));
                const Sculptor::EndPoint cc_out      = { t7_mapping.input_node, cc_slot };
                const uint32_t           osc         = t7_mapping.osc_nodes[0];
                const Sculptor::EndPoint spread_row  = { osc, Sculptor::osc_target_row(Synth::mod_fir_spread) };
                const Sculptor::EndPoint duty1_row   = { osc, Sculptor::osc_target_row(Synth::mod_duty1) };
                const Sculptor::EndPoint panning_row = { osc, Sculptor::osc_target_row(Synth::mod_panning) };
                const Sculptor::EndPoint duty0_row   = { osc, Sculptor::osc_target_row(Synth::mod_duty0) };

                // The validator admits every family onto its base row.
                TEST(Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env_out, spread_row));
                TEST(Sculptor::osc_graph_validate(&t7_mapping, t7_graph, lfo_out, duty1_row));
                TEST(Sculptor::osc_graph_validate(&t7_mapping, t7_graph, velocity_out, panning_row));
                TEST(Sculptor::osc_graph_validate(&t7_mapping, t7_graph, cc_out, duty0_row));

                TEST(t7_graph.add_connection(env_out, spread_row) != Sculptor::pool_no_slot);
                TEST(t7_graph.add_connection(lfo_out, duty1_row) != Sculptor::pool_no_slot);
                TEST(t7_graph.add_connection(velocity_out, panning_row) != Sculptor::pool_no_slot);
                TEST(t7_graph.add_connection(cc_out, duty0_row) != Sculptor::pool_no_slot);
                Sculptor::GraphChange changes[8] = {};
                const uint32_t        count      = t7_graph.take_changes(changes, 8);
                TEST(count == 4);
                candidate = source;
                for (uint32_t i = 0; i < count; i++) {
                    TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, changes[i]));
                }
                // The relocation rewrites endpoints eventlessly, so a
                // second drain is empty and no deferred base-row delete can
                // trip a refusal.
                TEST(t7_graph.take_changes(changes, 8) == 0);

                // Every wire re-landed on its family dot; no wire stays on
                // a base row.
                TEST(
                    find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_env_dot(Synth::mod_fir_spread) }) !=
                    Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_lfo_dot(Synth::mod_duty1) }) !=
                     Sculptor::pool_no_slot);
                TEST(
                    find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_src_dot(Synth::mod_panning, 0) }) !=
                    Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_src_dot(Synth::mod_duty0, 0) }) !=
                     Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, spread_row) == Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, duty1_row) == Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, panning_row) == Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, duty0_row) == Sculptor::pool_no_slot);

                // The compiled bindings carry the connected generators and
                // the seeded neutral row values.
                Synth::Instrument compiled = {};
                TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled));
                TEST(compiled.layers[0].targets[Synth::mod_fir_spread].envelope_desc_id == 1);
                // The first envelope wire seeded the legacy unscaled amount.
                TEST(approx(compiled.layers[0].targets[Synth::mod_fir_spread].envelope_depth, 1.0f, 1.0e-6f));
                TEST(compiled.layers[0].targets[Synth::mod_duty1].lfo_desc_id == 1);
                TEST(compiled.layers[0].targets[Synth::mod_duty1].lfo_op == Synth::SourceOp::add);
                TEST(approx(compiled.layers[0].targets[Synth::mod_duty1].lfo_depth, 0.5f, 1.0e-6f));
                TEST(compiled.layers[0].targets[Synth::mod_duty1].lfo_depth_source == Synth::ModSource::none);
                TEST(compiled.layers[0].targets[Synth::mod_duty1].lfo_rate_source == Synth::ModSource::none);
                TEST(approx(compiled.layers[0].targets[Synth::mod_duty1].lfo_rate_scale_ms, 0.0f, 1.0e-6f));
                TEST(compiled.layers[0].targets[Synth::mod_panning].num_inputs == 1);
                TEST(compiled.layers[0].targets[Synth::mod_panning].inputs[0].source == Synth::ModSource::velocity);
                TEST(compiled.layers[0].targets[Synth::mod_panning].inputs[0].op == Synth::SourceOp::add);
                TEST(approx(compiled.layers[0].targets[Synth::mod_panning].inputs[0].scale, 1.0f, 1.0e-6f));
                TEST(compiled.layers[0].targets[Synth::mod_duty0].num_inputs == 1);
                TEST(compiled.layers[0].targets[Synth::mod_duty0].inputs[0].source == Synth::ModSource::cc);
                TEST(compiled.layers[0].targets[Synth::mod_duty0].inputs[0].cc_number == Synth::cc_msb_base);
                TEST(compiled.layers[0].targets[Synth::mod_duty0].inputs[0].op == Synth::SourceOp::add);
                TEST(approx(compiled.layers[0].targets[Synth::mod_duty0].inputs[0].scale, 1.0f, 1.0e-6f));
                // Untouched bindings stay untouched.
                TEST(compiled.layers[0].targets[Synth::mod_volume].envelope_desc_id == 2);
                TEST(approx(compiled.layers[0].targets[Synth::mod_volume].inputs[0].scale, 2.0f, 1.0e-6f));
                // The seeded row values landed in the slots too.
                const uint32_t duty1_base = Sculptor::osc_target_block_base(Synth::mod_duty1);
                TEST(t7_graph.node(osc).slots.entries[duty1_base + t7_lfo_op_field].value.list_index == 0);
                TEST(approx(t7_graph.node(osc).slots.entries[duty1_base + t7_lfo_depth_field].value.real,
                            0.5f,
                            1.0e-6f));
                TEST(approx(t7_graph.node(osc).slots.entries[duty1_base + t7_rate_field].value.real, 0.0f, 1.0e-6f));
                const uint32_t panning_base = Sculptor::osc_target_block_base(Synth::mod_panning);
                TEST(approx(t7_graph.node(osc).slots.entries[panning_base + 8].value.real, 1.0f, 1.0e-6f));

                // Re-projection of the applied bank reproduces the family
                // wiring and projects no parameter nodes.
                TEST(Sculptor::project_editor_to_graph(candidate, &t7_graph, &t7_mapping));
                (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
                check_no_param_nodes(t7_graph);
                TEST(find_osc_graph_connection(
                         t7_graph,
                         { t7_mapping.osc_nodes[0], Sculptor::osc_target_env_dot(Synth::mod_fir_spread) }) !=
                     Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(
                         t7_graph,
                         { t7_mapping.osc_nodes[0], Sculptor::osc_target_lfo_dot(Synth::mod_duty1) }) !=
                     Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(
                         t7_graph,
                         { t7_mapping.osc_nodes[0], Sculptor::osc_target_src_dot(Synth::mod_panning, 0) }) !=
                     Sculptor::pool_no_slot);
            }
        }
    }

    // T-M7-3 (O3/O5): row visibility derives from live connection state.
    // Unconnected family rows hide, connected rows render, the base row
    // never hides, and deleting the wire hides the row again with the
    // compiled binding zeroed.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& candidate = *candidate_storage;
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            // Bind the FIR spread LFO so the projection wires the block's LFO
            // dot.
            source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].lfo_desc_id = 1;
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
            const uint32_t osc = t7_mapping.osc_nodes[0];
            TEST(osc != Sculptor::pool_no_slot);
            if (osc != Sculptor::pool_no_slot) {
                // Unconnected families hide; the connected LFO row renders.
                for (uint32_t f = 0; f <= 11; f++) {
                    const bool connected_lfo = f >= 2 && f <= 5;
                    TEST(Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, f) == ! connected_lfo);
                }
                // The base row is never hidden: it stays the standing drop
                // target.
                TEST(t7_graph.node(osc).slots.entries[Sculptor::osc_target_row(Synth::mod_fir_spread)].connectable);
                // Wires on the env dot and both source dots open their rows.
                const int32_t env_inst = find_detached_by_desc(t7_mapping, 1, 1);
                TEST(env_inst >= 0);
                if (env_inst >= 0) {
                    const Sculptor::EndPoint env_out      = { t7_mapping.detached[env_inst].node_idx,
                                                              t7_mapping.env_output_slot };
                    const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
                    const Sculptor::EndPoint velocity_out = { t7_mapping.input_node,
                                                              t7_mapping.input_source_slots[velocity_idx] };
                    const uint32_t           wheel_idx    = static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1;
                    const Sculptor::EndPoint wheel_out    = { t7_mapping.input_node,
                                                              t7_mapping.input_source_slots[wheel_idx] };
                    TEST(t7_graph.add_connection(env_out,
                                                 { osc, Sculptor::osc_target_env_dot(Synth::mod_fir_spread) }) !=
                         Sculptor::pool_no_slot);
                    TEST(t7_graph.add_connection(velocity_out,
                                                 { osc, Sculptor::osc_target_src_dot(Synth::mod_fir_spread, 0) }) !=
                         Sculptor::pool_no_slot);
                    TEST(t7_graph.add_connection(wheel_out,
                                                 { osc, Sculptor::osc_target_src_dot(Synth::mod_fir_spread, 1) }) !=
                         Sculptor::pool_no_slot);
                    TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, 0));
                    TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, 1));
                    for (uint32_t f = 6; f <= 11; f++) {
                        TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, f));
                    }
                    // The three direct-dot wires drain and apply first; the O5
                    // delete below must be the only pending change.
                    candidate                           = source;
                    Sculptor::GraphChange pending[8]    = {};
                    uint32_t              pending_count = t7_graph.take_changes(pending, 8);
                    TEST(pending_count == 3);
                    for (uint32_t i = 0; i < pending_count; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, pending[i]));
                    }
                    // O5: deleting the wire hides the row again.
                    const uint32_t lfo_wire =
                        find_osc_graph_connection(t7_graph,
                                                  { osc, Sculptor::osc_target_lfo_dot(Synth::mod_fir_spread) });
                    TEST(lfo_wire != Sculptor::pool_no_slot);
                    if (lfo_wire != Sculptor::pool_no_slot) {
                        t7_graph.delete_connection(lfo_wire);
                        for (uint32_t f = 2; f <= 5; f++) {
                            TEST(Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, f));
                        }
                        TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, 0));
                        TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, 1));
                        TEST(! Sculptor::osc_target_field_hidden(t7_graph, osc, Synth::mod_fir_spread, 6));
                        // The compiled binding zeroes once the deletion
                        // applies: the family's fields go dormant on their
                        // own, nothing stored clears them.
                        Sculptor::GraphChange changes[8] = {};
                        const uint32_t        count      = t7_graph.take_changes(changes, 8);
                        TEST(count == 1);
                        candidate = source;
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, changes[0]));
                        Synth::Instrument compiled = {};
                        TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled));
                        TEST(compiled.layers[0].targets[Synth::mod_fir_spread].lfo_desc_id == 0);
                        TEST(approx(compiled.layers[0].targets[Synth::mod_fir_spread].lfo_depth, 0.0f, 1.0e-6f));
                        TEST(
                            approx(compiled.layers[0].targets[Synth::mod_fir_spread].lfo_rate_scale_ms, 0.0f, 1.0e-6f));
                        // Re-projection keeps the row hidden (nothing is
                        // stored about visibility).
                        TEST(Sculptor::project_editor_to_graph(candidate, &t7_graph, &t7_mapping));
                        (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
                        for (uint32_t f = 2; f <= 5; f++) {
                            TEST(Sculptor::osc_target_field_hidden(t7_graph,
                                                                   t7_mapping.osc_nodes[0],
                                                                   Synth::mod_fir_spread,
                                                                   f));
                        }
                    }
                }
            }
        }
    }

    // T-M7-4 (O1): zero parameter nodes across every fixture shape.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage  = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank          = *bank_storage;
        Synth::Instrument                            instrument    = {};
        uint16_t                                     shared_env_id = 0;
        uint16_t                                     lfo_id        = 0;
        uint16_t                                     volume_env_id = 0;
        build_osc_graph_small_fixture(&bank, &instrument, &shared_env_id, &lfo_id, &volume_env_id);
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);

        build_full_block_fixture(&bank, &instrument);
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);

        const std::unique_ptr<Synth::InstrumentEditorBank> editor_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& editor_bank = *editor_bank_storage;
        if (begin_small_graph(&editor_bank, &t7_graph, &t7_mapping)) {
            check_no_param_nodes(t7_graph);
        }

        const std::unique_ptr<Synth::InstrumentEditorBank> binding_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& binding_bank       = *binding_bank_storage;
        Synth::Instrument            binding_instrument = {};
        build_binding_fixture(&binding_bank, &binding_instrument);
        TEST(Sculptor::project_editor_to_graph(binding_bank, &t7_graph, &t7_mapping));
        (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
        check_no_param_nodes(t7_graph);

        uint16_t env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);
    }

    // The project -> compile round trip stays bit-identical for direct
    // modulator rows: the adapted max fixture, a multi-layer instrument
    // whose cells carry per-layer binding tuples, and an envelope alias
    // shared across cells and layers.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage       = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank               = *bank_storage;
        Synth::Instrument                            instrument         = {};
        const std::unique_ptr<Synth::InstrumentBank> bank_image_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank_image         = *bank_image_storage;
        Synth::Instrument                            compiled           = {};
        uint16_t                                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        bank_image = bank;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        // The instance registry keeps its 28 + 28 generator instances.
        TEST(t7_mapping.detached_count == 56);
        TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled, nullptr, &instrument));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);

        // Multi-layer per-cell binding tuples: layers 0 and 2 share one
        // volume tuple, layer 1 differs; compile writes each layer's own
        // block state, so the shared tuple carries over bit-identically.
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        instrument = {};
        TEST(bank.envelopes.allocate() == 0);
        TEST(bank.envelopes.allocate() == 1);
        Synth::EnvelopeDescriptor& env1 = bank.envelopes.entries[0];
        env1.num_points                 = 2;
        env1.sustain_first_point        = 0;
        env1.sustain_last_point         = 1;
        env1.min_value                  = -1.0f;
        env1.min_max_delta              = 2.0f;
        env1.points[0]                  = { 0, 0x2000 };
        env1.points[1]                  = { 100, 0x4000 };
        bank.envelopes.entries[1]       = env1;
        TEST(bank.lfos.allocate() == 0);
        bank.lfos.entries[0].wave      = Synth::WaveType::sine_wave;
        bank.lfos.entries[0].period_ms = 300;
        Synth::TargetBinding shared    = {};
        shared.envelope_desc_id        = 1;
        shared.lfo_desc_id             = 1;
        shared.lfo_op                  = Synth::SourceOp::add;
        shared.lfo_depth               = 0.5f;
        shared.lfo_depth_source        = Synth::ModSource::velocity;
        shared.lfo_rate_source         = Synth::ModSource::mod_wheel;
        shared.lfo_rate_scale_ms       = 20.0f;
        instrument.layer_count         = 3;
        set_default_dispersion_sections(instrument);
        for (uint32_t layer = 0; layer < 3; layer++) {
            instrument.layers[layer].osc_type[0] = Synth::WaveType::sawtooth_wave;
        }
        instrument.layers[0].targets[Synth::mod_volume] = shared;
        instrument.layers[2].targets[Synth::mod_volume] = shared;
        Synth::TargetBinding other                      = shared;
        other.envelope_desc_id                          = 2;
        instrument.layers[1].targets[Synth::mod_volume] = other;
        // The env-2 instance also serves layer 0's pitch cell (one node
        // aliasing across targets and layers).
        instrument.layers[0].targets[Synth::mod_pitch].envelope_desc_id = 2;
        instrument.layers[0].targets[Synth::mod_volume].base_value      = 0.8f;
        instrument.layers[1].targets[Synth::mod_volume].base_value      = 0.3f;
        instrument.layers[2].targets[Synth::mod_volume].base_value      = 0.9f;
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);
        // Two envelope instances (desc 1 and desc 2) and one LFO instance.
        TEST(t7_mapping.detached_count == 3);
        TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled, nullptr, &instrument));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        // Each served cell carries its own block rows with the tuple
        // values; the base values stay per layer.
        for (uint32_t layer = 0; layer < 3; layer++) {
            const uint32_t        base = Sculptor::osc_target_block_base(Synth::mod_volume);
            const Sculptor::Node& osc  = t7_graph.node(t7_mapping.osc_nodes[layer]);
            TEST(osc.slots.entries[base + t7_lfo_op_field].value.list_index ==
                 static_cast<uint32_t>(Synth::SourceOp::add));
            TEST(approx(osc.slots.entries[base + t7_lfo_depth_field].value.real, 0.5f, 1.0e-6f));
            TEST(approx(osc.slots.entries[base + t7_rate_field].value.real, 20.0f, 1.0e-4f));
        }
        // The env-2 instance wires into layer 1 volume and layer 0 pitch.
        const int32_t env2 = find_detached_by_desc(t7_mapping, 1, 2);
        TEST(env2 >= 0);
        if (env2 >= 0) {
            const uint32_t env_node = t7_mapping.detached[env2].node_idx;
            const uint32_t vol1_wire =
                find_osc_graph_connection(t7_graph,
                                          { t7_mapping.osc_nodes[1], Sculptor::osc_target_env_dot(Synth::mod_volume) });
            const uint32_t pitch_wire =
                find_osc_graph_connection(t7_graph,
                                          { t7_mapping.osc_nodes[0], Sculptor::osc_target_env_dot(Synth::mod_pitch) });
            TEST(vol1_wire != Sculptor::pool_no_slot && pitch_wire != Sculptor::pool_no_slot);
            if (vol1_wire != Sculptor::pool_no_slot && pitch_wire != Sculptor::pool_no_slot) {
                TEST(t7_graph.get_connection(vol1_wire).output.node_idx == env_node);
                TEST(t7_graph.get_connection(pitch_wire).output.node_idx == env_node);
            }
        }
    }

    // T-M7-8 (O9): one (layer, target) carries envelope + LFO + two MIDI
    // sources at once, each on its own row, and the compiled binding
    // carries all four.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        build_full_block_fixture(&bank, &instrument);
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);
        const uint32_t osc = t7_mapping.osc_nodes[0];
        TEST(osc != Sculptor::pool_no_slot);
        if (osc != Sculptor::pool_no_slot) {
            const uint32_t env_dot   = Sculptor::osc_target_env_dot(Synth::mod_volume);
            const uint32_t lfo_dot   = Sculptor::osc_target_lfo_dot(Synth::mod_volume);
            const uint32_t src0      = Sculptor::osc_target_src_dot(Synth::mod_volume, 0);
            const uint32_t src1      = Sculptor::osc_target_src_dot(Synth::mod_volume, 1);
            const uint32_t env_wire  = find_osc_graph_connection(t7_graph, { osc, env_dot });
            const uint32_t lfo_wire  = find_osc_graph_connection(t7_graph, { osc, lfo_dot });
            const uint32_t src0_wire = find_osc_graph_connection(t7_graph, { osc, src0 });
            const uint32_t src1_wire = find_osc_graph_connection(t7_graph, { osc, src1 });
            TEST(env_wire != Sculptor::pool_no_slot && lfo_wire != Sculptor::pool_no_slot);
            TEST(src0_wire != Sculptor::pool_no_slot && src1_wire != Sculptor::pool_no_slot);
            if (env_wire != Sculptor::pool_no_slot && lfo_wire != Sculptor::pool_no_slot &&
                src0_wire != Sculptor::pool_no_slot && src1_wire != Sculptor::pool_no_slot) {
                const int32_t env_inst =
                    find_instance_by_node(t7_mapping, t7_graph.get_connection(env_wire).output.node_idx);
                const int32_t lfo_inst =
                    find_instance_by_node(t7_mapping, t7_graph.get_connection(lfo_wire).output.node_idx);
                TEST(env_inst >= 0 && lfo_inst >= 0);
                if (env_inst >= 0 && lfo_inst >= 0) {
                    TEST(t7_mapping.detached[env_inst].desc_id == 1); // the volume envelope
                    TEST(t7_mapping.detached[lfo_inst].desc_id == 1);
                    TEST(t7_graph.get_connection(src0_wire).output.slot_idx ==
                         t7_mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::velocity) - 1]);
                    TEST(t7_graph.get_connection(src1_wire).output.slot_idx ==
                         t7_mapping.input_source_slots[static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1]);
                }
                Synth::Instrument compiled = {};
                TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled));
                const Synth::TargetBinding& gen = compiled.layers[0].targets[Synth::mod_volume];
                TEST(gen.envelope_desc_id == 1 && gen.lfo_desc_id == 1);
                TEST(gen.lfo_op == Synth::SourceOp::add);
                TEST(approx(gen.lfo_depth, 0.25f, 1.0e-6f));
                TEST(approx(gen.lfo_rate_scale_ms, 10.0f, 1.0e-4f));
                TEST(gen.num_inputs == 2);
                TEST(gen.inputs[0].source == Synth::ModSource::velocity);
                TEST(approx(gen.inputs[0].scale, 0.5f, 1.0e-6f));
                TEST(gen.inputs[1].source == Synth::ModSource::mod_wheel);
                TEST(approx(gen.inputs[1].scale, -0.25f, 1.0e-6f));
                // Each family row stays on its own renderer line.
                TEST(t7_graph.node(osc).slots.entries[env_dot].row_group == 4); // target 0: 4*0+4
                TEST(t7_graph.node(osc).slots.entries[lfo_dot].row_group == 1);
                TEST(t7_graph.node(osc).slots.entries[src0].row_group == 2);
                TEST(t7_graph.node(osc).slots.entries[src1].row_group == 3);
            }
        }
    }

    // T-M7-9 (I5): undo/redo and snapshot round-trips for the new
    // connection types (envelope, LFO, standard MIDI and CC) and a
    // family-field edit.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> wired_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& wired    = *wired_storage;
        Synth::Instrument            compiled = {};
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            // One assignable CC dot so the MIDI and CC wires ride the
            // same undo/redo snapshots (I5).
            source.added_cc_mask[0] = 1u << 0;
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
            const int32_t env_inst = find_detached_by_desc(t7_mapping, 1, 1);
            const int32_t lfo_inst = find_detached_by_desc(t7_mapping, 2, 1);
            TEST(env_inst >= 0 && lfo_inst >= 0);
            if (env_inst >= 0 && lfo_inst >= 0) {
                const Sculptor::EndPoint env_out    = { t7_mapping.detached[env_inst].node_idx,
                                                        t7_mapping.env_output_slot };
                const Sculptor::EndPoint lfo_out    = { t7_mapping.detached[lfo_inst].node_idx,
                                                        t7_mapping.lfo_output_slot };
                const uint32_t           osc        = t7_mapping.osc_nodes[0];
                const Sculptor::EndPoint spread_row = { osc, Sculptor::osc_target_row(Synth::mod_fir_spread) };
                const Sculptor::EndPoint duty1_row  = { osc, Sculptor::osc_target_row(Synth::mod_duty1) };
                TEST(t7_graph.add_connection(env_out, spread_row) != Sculptor::pool_no_slot);
                TEST(t7_graph.add_connection(lfo_out, duty1_row) != Sculptor::pool_no_slot);
                const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
                const Sculptor::EndPoint velocity_out = { t7_mapping.input_node,
                                                          t7_mapping.input_source_slots[velocity_idx] };
                char                     cc_label[32];
                Sculptor::fx_cc_pair_label(Synth::cc_msb_base, cc_label, sizeof(cc_label));
                uint32_t cc_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(t7_graph, t7_mapping.input_node, cc_label, &cc_slot));
                const Sculptor::EndPoint cc_out      = { t7_mapping.input_node, cc_slot };
                const Sculptor::EndPoint panning_row = { osc, Sculptor::osc_target_row(Synth::mod_panning) };
                const Sculptor::EndPoint duty_row    = { osc, Sculptor::osc_target_row(Synth::mod_duty0) };
                TEST(t7_graph.add_connection(velocity_out, panning_row) != Sculptor::pool_no_slot);
                TEST(t7_graph.add_connection(cc_out, duty_row) != Sculptor::pool_no_slot);
                Sculptor::GraphChange changes[8] = {};
                const uint32_t        count      = t7_graph.take_changes(changes, 8);
                TEST(count == 4);

                // Whole-bank undo: the pre-wire bank restores bit-identically
                // and re-projects with no family wiring.
                const std::unique_ptr<FixedArray<uint8_t, sizeof(Synth::InstrumentEditorBank) * 4 + 256>>
                    undo_storage_storage =
                        std::make_unique<FixedArray<uint8_t, sizeof(Synth::InstrumentEditorBank) * 4 + 256>>();
                uint8_t (&undo_storage)[sizeof(Synth::InstrumentEditorBank) * 4 + 256] = undo_storage_storage->elements;
                Sculptor::UndoRedo undo;
                undo.init(undo_storage);
                undo.init_undo_push();
                undo.push(&source, sizeof(source));
                TEST(undo.finish_undo_push());
                candidate = source;
                for (uint32_t i = 0; i < count; i++) {
                    TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, changes[i]));
                }
                const Sculptor::EndPoint panning_src0 = { osc, Sculptor::osc_target_src_dot(Synth::mod_panning, 0) };
                const Sculptor::EndPoint duty_src0    = { osc, Sculptor::osc_target_src_dot(Synth::mod_duty0, 0) };
                // The MIDI and CC base-row drops relocated onto their
                // source dots in the same drain.
                TEST(find_osc_graph_connection(t7_graph, panning_src0) != Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, duty_src0) != Sculptor::pool_no_slot);
                wired = candidate;
                undo.init_redo_push();
                undo.push(&wired, sizeof(wired));
                TEST(undo.finish_redo_push());
                TEST(undo.init_undo());
                undo.pop(&candidate, sizeof(candidate));
                undo.finish_undo();
                TEST(memcmp(&candidate, &source, sizeof(source)) == 0);
                TEST(Sculptor::project_editor_to_graph(candidate, &t7_graph, &t7_mapping));
                (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
                TEST(find_osc_graph_connection(
                         t7_graph,
                         { t7_mapping.osc_nodes[0], Sculptor::osc_target_env_dot(Synth::mod_fir_spread) }) ==
                     Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, panning_src0) == Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, duty_src0) == Sculptor::pool_no_slot);
                // Redo restores the wired bank and its rows.
                TEST(undo.init_redo());
                undo.pop(&candidate, sizeof(candidate));
                undo.finish_redo();
                TEST(memcmp(&candidate, &wired, sizeof(wired)) == 0);
                TEST(Sculptor::project_editor_to_graph(candidate, &t7_graph, &t7_mapping));
                (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
                TEST(find_osc_graph_connection(
                         t7_graph,
                         { t7_mapping.osc_nodes[0], Sculptor::osc_target_env_dot(Synth::mod_fir_spread) }) !=
                     Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, panning_src0) != Sculptor::pool_no_slot);
                TEST(find_osc_graph_connection(t7_graph, duty_src0) != Sculptor::pool_no_slot);
                // A family-field edit writes through to the compiled
                // binding and undoes back to the wired snapshot.
                const uint32_t depth_slot     = Sculptor::osc_target_block_base(Synth::mod_duty1) + t7_lfo_depth_field;
                Sculptor::PropertyValue depth = {};
                depth.real                    = 0.25f;
                t7_graph.set_slot_value(t7_mapping.osc_nodes[0], depth_slot, depth);
                Sculptor::GraphChange edit[8] = {};
                edit[0].kind                  = Sculptor::ChangeKind::value_changed;
                edit[0].node_idx              = t7_mapping.osc_nodes[0];
                edit[0].slot_idx              = depth_slot;
                undo.init_undo_push();
                undo.push(&candidate, sizeof(candidate));
                TEST(undo.finish_undo_push());
                TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit[0]));
                TEST(Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled));
                TEST(approx(compiled.layers[0].targets[Synth::mod_duty1].lfo_depth, 0.25f, 1.0e-6f));
                TEST(undo.init_undo());
                undo.pop(&candidate, sizeof(candidate));
                undo.finish_undo();
                TEST(memcmp(&candidate, &wired, sizeof(wired)) == 0);
            }
        }
    }

    // Oscillator LFO descriptor edits: the real Duty slot applies, NaN is
    // refused, and every projected instance sharing the descriptor stays
    // in step (wave value, duty value, and Duty editability).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& candidate = *candidate_storage;
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            candidate = source;

            // Alias a second instance onto the projected LFO descriptor
            // with different modulation sources, then reproject: the two
            // nodes are separate views of one shared descriptor.
            uint32_t holder_layer  = Synth::max_layers;
            uint32_t holder_target = Synth::num_mod_targets;
            for (uint32_t layer = 0; layer < Synth::max_layers && holder_layer == Synth::max_layers; ++layer) {
                for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                    if (source.bank.instruments.entries[0].layers[layer].targets[t].lfo_desc_id == 1) {
                        holder_layer  = layer;
                        holder_target = t;
                        break;
                    }
                }
            }

            TEST(holder_layer < Synth::max_layers);
            Synth::TargetBinding alias = source.bank.instruments.entries[0].layers[holder_layer].targets[holder_target];
            alias.lfo_depth_source     = Synth::ModSource::mod_wheel;
            alias.lfo_rate_source      = Synth::ModSource::channel_pressure;
            source.bank.instruments.entries[0]
                .layers[(holder_layer + 2) % Synth::max_layers]
                .targets[Synth::mod_volume] = alias;
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);

            const int32_t  edited       = find_detached_by_desc(t7_mapping, 2, 1);
            const uint32_t aliased_desc = 1;
            TEST(edited >= 0);
            if (edited >= 0) {
                const uint32_t node      = t7_mapping.detached[edited].node_idx;
                uint32_t       duty_slot = Sculptor::pool_no_slot;
                uint32_t       wave_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(t7_graph, node, "Duty", &duty_slot));
                TEST(find_graph_slot(t7_graph, node, "Waveform", &wave_slot));

                Sculptor::GraphChange edit = {};
                edit.kind                  = Sculptor::ChangeKind::value_changed;
                edit.node_idx              = node;
                edit.slot_idx              = duty_slot;

                // Duty applies through the real slot.
                Sculptor::PropertyValue duty = {};
                duty.real                    = 0.25f;
                t7_graph.set_slot_value(node, duty_slot, duty);
                TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit));
                TEST(candidate.bank.lfos.entries[aliased_desc - 1].duty == 64);

                // NaN is refused and leaves the descriptor unchanged.
                Sculptor::PropertyValue nan_duty = {};
                nan_duty.real                    = NAN;
                t7_graph.set_slot_value(node, duty_slot, nan_duty);
                TEST(! Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit));
                TEST(candidate.bank.lfos.entries[aliased_desc - 1].duty == 64);

                // A wave edit syncs every instance sharing the descriptor.
                edit.slot_idx               = wave_slot;
                Sculptor::PropertyValue saw = {};
                saw.list_index              = 1;
                t7_graph.set_slot_value(node, wave_slot, saw);
                TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit));
                TEST(candidate.bank.lfos.entries[aliased_desc - 1].wave == Synth::WaveType::sawtooth_wave);
                for (uint32_t i = 0; i < t7_mapping.detached_count; ++i) {
                    const Sculptor::DetachedNode& entry = t7_mapping.detached[i];
                    if (entry.kind != 2 || entry.desc_id != aliased_desc || entry.node_idx == Sculptor::pool_no_slot) {
                        continue;
                    }

                    uint32_t alias_wave = Sculptor::pool_no_slot;
                    uint32_t alias_duty = Sculptor::pool_no_slot;
                    TEST(find_graph_slot(t7_graph, entry.node_idx, "Waveform", &alias_wave));
                    TEST(find_graph_slot(t7_graph, entry.node_idx, "Duty", &alias_duty));
                    TEST(t7_graph.node(entry.node_idx).slots.entries[alias_wave].value.list_index == 1);
                    TEST(approx(t7_graph.node(entry.node_idx).slots.entries[alias_duty].value.real,
                                64.0f / 255.0f,
                                1.0e-6f));
                    TEST(! t7_graph.slot_edit_disabled(entry.node_idx, alias_duty));
                }

                // Sync and Start edits apply through the same list dispatch
                // as the Waveform edit above: beat_div and start_point write
                // through to the descriptor.
                uint32_t sync_slot  = Sculptor::pool_no_slot;
                uint32_t start_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(t7_graph, node, "Sync", &sync_slot));
                TEST(find_graph_slot(t7_graph, node, "Start", &start_slot));
                edit.slot_idx                      = sync_slot;
                Sculptor::PropertyValue sync_value = {};
                sync_value.list_index              = 3;
                t7_graph.set_slot_value(node, sync_slot, sync_value);
                TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit));
                TEST(candidate.bank.lfos.entries[aliased_desc - 1].beat_div == 3);
                edit.slot_idx                       = start_slot;
                Sculptor::PropertyValue start_value = {};
                start_value.list_index              = 2;
                t7_graph.set_slot_value(node, start_slot, start_value);
                TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, edit));
                TEST(candidate.bank.lfos.entries[aliased_desc - 1].start_point == 2);
            }
        }
    }

    // T-M7-10 (validator edges, C2/C8): capacity and wrong-family
    // refusals, the volume rule both directions, output-end retargets,
    // input-end target moves, the base-row delete refusal, the per-cell
    // source order and the stray-connection compile refusal.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source = *source_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& candidate = *candidate_storage;
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            Synth::Instrument& instr = source.bank.instruments.entries[0];
            // Bindings that pre-wire the dots the capacity refusals need:
            // FIR spread carries the shared non-volume envelope and two MIDI
            // sources, duty b carries the LFO, duty0 carries a third
            // non-volume envelope.
            instr.layers[0].targets[Synth::mod_fir_spread].envelope_desc_id = 1;
            instr.layers[0].targets[Synth::mod_fir_spread].num_inputs       = 2;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[0].source = Synth::ModSource::velocity;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[0].op     = Synth::SourceOp::add;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[0].scale  = 1.0f;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[1].source = Synth::ModSource::mod_wheel;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[1].op     = Synth::SourceOp::multiply;
            instr.layers[0].targets[Synth::mod_fir_spread].inputs[1].scale  = 1.0f;
            instr.layers[0].targets[Synth::mod_duty1].lfo_desc_id           = 1;
            TEST(source.bank.envelopes.allocate() == 2);
            Synth::EnvelopeDescriptor& env3                            = source.bank.envelopes.entries[2];
            env3                                                       = Synth::EnvelopeDescriptor{};
            env3.num_points                                            = 2;
            env3.sustain_first_point                                   = 0;
            env3.sustain_last_point                                    = 1;
            env3.min_value                                             = -0.5f;
            env3.min_max_delta                                         = 2.0f;
            env3.points[0]                                             = { 0, 0x4000 };
            env3.points[1]                                             = { 100, 0xC000 };
            instr.layers[0].targets[Synth::mod_duty0].envelope_desc_id = 3;
            TEST(source.bank.lfos.allocate() == 1);
            Synth::LFODescriptor& lfo2                              = source.bank.lfos.entries[1];
            lfo2                                                    = Synth::LFODescriptor{};
            lfo2.wave                                               = Synth::WaveType::sine_wave;
            lfo2.period_ms                                          = 300;
            lfo2.min_value                                          = -1.0f;
            lfo2.min_max_delta                                      = 2.0f;
            instr.layers[0].targets[Synth::mod_panning].lfo_desc_id = 2;
            source.added_cc_mask[0]                                 = 1u << 0;
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
            t7_graph.set_validator(Sculptor::osc_graph_validate, &t7_mapping);
            candidate = source;

            const int32_t env1  = find_detached_by_desc(t7_mapping, 1, 1);
            const int32_t env2  = find_detached_by_desc(t7_mapping, 1, 2);
            const int32_t env3i = find_detached_by_desc(t7_mapping, 1, 3);
            const int32_t lfo1  = find_detached_by_desc(t7_mapping, 2, 1);
            TEST(env1 >= 0 && env2 >= 0 && env3i >= 0 && lfo1 >= 0);
            if (env1 >= 0 && env2 >= 0 && env3i >= 0 && lfo1 >= 0) {
                const Sculptor::EndPoint env1_out = { t7_mapping.detached[env1].node_idx, t7_mapping.env_output_slot };
                const Sculptor::EndPoint env2_out = { t7_mapping.detached[env2].node_idx, t7_mapping.env_output_slot };
                const Sculptor::EndPoint env3_out = { t7_mapping.detached[env3i].node_idx, t7_mapping.env_output_slot };
                const Sculptor::EndPoint lfo_out  = { t7_mapping.detached[lfo1].node_idx, t7_mapping.lfo_output_slot };
                const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
                const Sculptor::EndPoint velocity_out = { t7_mapping.input_node,
                                                          t7_mapping.input_source_slots[velocity_idx] };
                const uint32_t           wheel_idx    = static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1;
                const Sculptor::EndPoint wheel_out    = { t7_mapping.input_node,
                                                          t7_mapping.input_source_slots[wheel_idx] };
                char                     cc_label[32];
                Sculptor::fx_cc_pair_label(Synth::cc_msb_base, cc_label, sizeof(cc_label));
                uint32_t cc_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(t7_graph, t7_mapping.input_node, cc_label, &cc_slot));
                const Sculptor::EndPoint cc_out       = { t7_mapping.input_node, cc_slot };
                const uint32_t           osc          = t7_mapping.osc_nodes[0];
                const Sculptor::EndPoint spread_row   = { osc, Sculptor::osc_target_row(Synth::mod_fir_spread) };
                const Sculptor::EndPoint duty1_row    = { osc, Sculptor::osc_target_row(Synth::mod_duty1) };
                const Sculptor::EndPoint volume_row   = { osc, Sculptor::osc_target_row(Synth::mod_volume) };
                const Sculptor::EndPoint panning_row  = { osc, Sculptor::osc_target_row(Synth::mod_panning) };
                const Sculptor::EndPoint panning_env  = { osc, Sculptor::osc_target_env_dot(Synth::mod_panning) };
                const Sculptor::EndPoint panning_lfo  = { osc, Sculptor::osc_target_lfo_dot(Synth::mod_panning) };
                const Sculptor::EndPoint panning_src0 = { osc, Sculptor::osc_target_src_dot(Synth::mod_panning, 0) };

                // One envelope per (layer, target) - the spread env
                // dot is taken by the projection wiring.
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env2_out, spread_row));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env3_out, spread_row));
                // One LFO per (layer, target): the duty b LFO dot is
                // taken by the projection wiring.
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, lfo_out, duty1_row));
                // max_mod_inputs sources: both spread source dots are
                // taken by the projection wiring.
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, cc_out, spread_row));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, velocity_out, spread_row));
                // The volume rule, both directions: a volume-shaped
                // descriptor never serves a non-volume row, a non-volume
                // descriptor never serves the volume row.
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env2_out, panning_row));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env3_out, volume_row));
                // Wrong-family refusals: sources, CC dots, envelopes and
                // LFOs never touch each other's dots.
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, velocity_out, panning_env));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, velocity_out, panning_lfo));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, cc_out, panning_env)); // Source rows only
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, cc_out, panning_lfo));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, lfo_out, panning_env));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env1_out, panning_lfo));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, env1_out, panning_src0));
                TEST(! Sculptor::osc_graph_validate(&t7_mapping, t7_graph, lfo_out, panning_src0));

                // Moving an existing wire's output end stays valid, and
                // the input end may move to another target's same-family
                // dot but never onto another family's dot.
                const uint32_t env_wire =
                    find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_env_dot(Synth::mod_fir_spread) });
                TEST(env_wire != Sculptor::pool_no_slot);
                if (env_wire != Sculptor::pool_no_slot) {
                    TEST(t7_graph.move_connection_end(env_wire, true, env3_out)); // output-end retarget
                    Sculptor::GraphChange moves[8] = {};
                    uint32_t              moved    = t7_graph.take_changes(moves, 8);
                    TEST(moved == 1);
                    for (uint32_t i = 0; i < moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, moves[i]));
                    }
                    TEST(t7_graph.move_connection_end(env_wire, false, panning_env)); // input-end target move
                    moved = t7_graph.take_changes(moves, 8);
                    TEST(moved == 1);
                    for (uint32_t i = 0; i < moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, moves[i]));
                    }
                    TEST(! t7_graph.move_connection_end(env_wire, false, panning_lfo)); // wrong family
                    (void)t7_graph.take_changes(moves, 8);
                }

                // C2 family coverage: the LFO wire moves like the envelope
                // wire - the output end retargets to the second LFO
                // instance and the input end moves to another target's
                // free LFO dot.
                const int32_t lfo2i = find_detached_by_desc(t7_mapping, 2, 2);
                TEST(lfo2i >= 0);
                const uint32_t lfo_wire =
                    find_osc_graph_connection(t7_graph, { osc, Sculptor::osc_target_lfo_dot(Synth::mod_duty1) });
                TEST(lfo_wire != Sculptor::pool_no_slot);
                if (lfo2i >= 0 && lfo_wire != Sculptor::pool_no_slot) {
                    const Sculptor::EndPoint lfo2_out = { t7_mapping.detached[lfo2i].node_idx,
                                                          t7_mapping.lfo_output_slot };
                    TEST(t7_graph.move_connection_end(lfo_wire, true, lfo2_out)); // output-end retarget
                    Sculptor::GraphChange lfo_moves[8] = {};
                    uint32_t              lfo_moved    = t7_graph.take_changes(lfo_moves, 8);
                    TEST(lfo_moved == 1);
                    for (uint32_t i = 0; i < lfo_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, lfo_moves[i]));
                    }
                    const Sculptor::EndPoint duty_lfo = { osc, Sculptor::osc_target_lfo_dot(Synth::mod_duty0) };
                    TEST(t7_graph.move_connection_end(lfo_wire, false, duty_lfo)); // input-end target move
                    lfo_moved = t7_graph.take_changes(lfo_moves, 8);
                    TEST(lfo_moved == 1);
                    for (uint32_t i = 0; i < lfo_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, lfo_moves[i]));
                    }
                    TEST(! t7_graph.move_connection_end(lfo_wire, false, panning_env)); // wrong family
                    (void)t7_graph.take_changes(lfo_moves, 8);
                }

                // Per-cell source order: a wire dropped on the base row
                // fills the first free source dot (source 0 before 1), even
                // when source 1 was wired directly first.
                TEST(t7_graph.add_connection(velocity_out,
                                             { osc, Sculptor::osc_target_src_dot(Synth::mod_panning, 1) }) !=
                     Sculptor::pool_no_slot);
                Sculptor::GraphChange adds[8] = {};
                uint32_t              added   = t7_graph.take_changes(adds, 8);
                TEST(added == 1);
                for (uint32_t i = 0; i < added; i++) {
                    TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, adds[i]));
                }
                TEST(Sculptor::osc_graph_validate(&t7_mapping, t7_graph, wheel_out, panning_row));
                TEST(t7_graph.add_connection(wheel_out, panning_row) != Sculptor::pool_no_slot);
                added = t7_graph.take_changes(adds, 8);
                TEST(added == 1);
                for (uint32_t i = 0; i < added; i++) {
                    TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, adds[i]));
                }
                TEST(find_osc_graph_connection(t7_graph, panning_src0) != Sculptor::pool_no_slot);

                // C2 family coverage for the standard MIDI and CC wires:
                // output-end replacement and input-end movement, mirroring
                // the envelope and LFO wires.
                const uint32_t midi_wire = find_osc_graph_connection(t7_graph, panning_src0);
                TEST(midi_wire != Sculptor::pool_no_slot);
                if (midi_wire != Sculptor::pool_no_slot) {
                    TEST(t7_graph.move_connection_end(midi_wire, true, velocity_out)); // output-end replacement
                    Sculptor::GraphChange wire_moves[8] = {};
                    uint32_t              wire_moved    = t7_graph.take_changes(wire_moves, 8);
                    TEST(wire_moved == 1);
                    for (uint32_t i = 0; i < wire_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, wire_moves[i]));
                    }
                    const Sculptor::EndPoint duty_src0 = { osc, Sculptor::osc_target_src_dot(Synth::mod_duty0, 0) };
                    TEST(t7_graph.move_connection_end(midi_wire, false, duty_src0)); // input-end target move
                    wire_moved = t7_graph.take_changes(wire_moves, 8);
                    TEST(wire_moved == 1);
                    for (uint32_t i = 0; i < wire_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, wire_moves[i]));
                    }
                }
                // CC wire: the mask's CC dot wires the pitch row's first
                // free source dot, then moves like the others.
                const Sculptor::EndPoint pitch_row = { osc, Sculptor::osc_target_row(Synth::mod_pitch) };
                TEST(t7_graph.add_connection(cc_out, pitch_row) != Sculptor::pool_no_slot);
                Sculptor::GraphChange cc_adds[8] = {};
                uint32_t              cc_added   = t7_graph.take_changes(cc_adds, 8);
                TEST(cc_added == 1);
                for (uint32_t i = 0; i < cc_added; i++) {
                    TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, cc_adds[i]));
                }
                const Sculptor::EndPoint pitch_src0 = { osc, Sculptor::osc_target_src_dot(Synth::mod_pitch, 0) };
                const uint32_t           cc_wire    = find_osc_graph_connection(t7_graph, pitch_src0);
                TEST(cc_wire != Sculptor::pool_no_slot);
                if (cc_wire != Sculptor::pool_no_slot) {
                    const Sculptor::EndPoint duty1_src0 = { osc, Sculptor::osc_target_src_dot(Synth::mod_duty1, 0) };
                    TEST(t7_graph.move_connection_end(cc_wire, false, duty1_src0)); // input-end target move
                    Sculptor::GraphChange cc_moves[8] = {};
                    uint32_t              cc_moved    = t7_graph.take_changes(cc_moves, 8);
                    TEST(cc_moved == 1);
                    for (uint32_t i = 0; i < cc_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, cc_moves[i]));
                    }
                    TEST(t7_graph.move_connection_end(cc_wire, true, velocity_out)); // output-end replacement
                    cc_moved = t7_graph.take_changes(cc_moves, 8);
                    TEST(cc_moved == 1);
                    for (uint32_t i = 0; i < cc_moved; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, cc_moves[i]));
                    }
                }

                // A delete whose endpoint is still a base row is refused:
                // the apply-side bind and the relocation land in the same
                // drain, so a delete never observes a base-row endpoint.
                Sculptor::GraphChange del = {};
                del.kind                  = Sculptor::ChangeKind::connection_deleted;
                del.connection_output     = wheel_out;
                del.connection_input      = panning_row;
                TEST(! Sculptor::apply_osc_graph_change(&candidate, &t7_graph, &t7_mapping, del));

                // A stray base-row connection injected headlessly (raw add,
                // no validator) makes the compile refuse: a connection
                // still ending on a base target row is an invariant
                // violation.
                TEST(t7_graph.add_connection(velocity_out, spread_row) != Sculptor::pool_no_slot);
                Synth::Instrument compiled = {};
                TEST(! Sculptor::compile_graph_to_instrument(t7_graph, t7_mapping, &compiled));
            }
            t7_graph.set_validator(nullptr, nullptr);
        }
    }

    // T-M7-11: the max fixture keeps its shape (seven oscillators, 28
    // envelope + 28 LFO instances) and gains the per-target block grammar
    // on every layer; the parameter counts leave the mapping checks.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        uint16_t                                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &t7_graph, &t7_mapping));
        check_no_param_nodes(t7_graph);
        TEST(t7_mapping.detached_count == 56);
        for (uint32_t layer = 0; layer < Synth::max_layers; layer++) {
            TEST(t7_mapping.osc_nodes[layer] != Sculptor::pool_no_slot);
            if (t7_mapping.osc_nodes[layer] == Sculptor::pool_no_slot) {
                continue;
            }
            const Sculptor::Node& osc = t7_graph.node(t7_mapping.osc_nodes[layer]);
            TEST(osc.slots.num_allocated ==
                 Sculptor::osc_band_first_slot +
                     Sculptor::osc_band_block_stride * instrument.layers[layer].fir_band_count);
            for (uint32_t t = 0; t < Synth::num_mod_targets; t++) {
                if (osc_target_band_of_target(t) >= static_cast<int32_t>(instrument.layers[layer].fir_band_count) ||
                    ! t7_target_has_row(instrument.layers[layer], t)) {
                    continue;
                }
                check_target_block_grammar(osc,
                                           t,
                                           instrument.layers[layer].targets[t],
                                           instrument.layers[layer].fir_type);
            }
        }
    }

    // Compile refuses a corrupted mapping whose oscillator node run has a
    // gap: the projected layer run must stay contiguous.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        uint16_t                                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        TEST(mapping.osc_nodes[0] != Sculptor::pool_no_slot);
        TEST(mapping.osc_nodes[Synth::max_layers - 1] != Sculptor::pool_no_slot);
        const uint32_t saved       = mapping.osc_nodes[3];
        mapping.osc_nodes[3]       = Sculptor::pool_no_slot;
        Synth::Instrument compiled = {};
        TEST(! Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        mapping.osc_nodes[3] = saved;
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    }

    // Registry capacity: every target of every layer bound with distinct
    // generator keys projects the full 2*layers*targets instance registry.
    // The legal all-cells projection needs 126 distinct instance keys - more
    // than the detached-record cap - so the registry is sized for the node
    // pool, not for the record cap.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        Synth::init_default_bank(&bank);
        instrument             = Synth::Instrument{};
        instrument.layer_count = Synth::max_layers;
        set_default_dispersion_sections(instrument);
        for (uint32_t cell = 0; cell < Synth::max_layers * Synth::num_mod_targets; cell++) {
            const uint32_t layer    = cell / Synth::num_mod_targets;
            const uint32_t target   = cell % Synth::num_mod_targets;
            const uint32_t env_slot = bank.envelopes.allocate();
            TEST(env_slot != Sculptor::pool_no_slot);
            Synth::EnvelopeDescriptor& env = bank.envelopes.entries[env_slot];
            env                            = Synth::EnvelopeDescriptor{};
            env.num_points                 = 2;
            env.sustain_first_point        = 0;
            env.sustain_last_point         = 1;
            env.min_value                  = -0.5f;
            env.min_max_delta              = 2.0f;
            env.points[0]                  = { 0, 0x4000 };
            env.points[1]                  = { 100, 0xC000 };
            const uint32_t lfo_slot        = bank.lfos.allocate();
            TEST(lfo_slot != Sculptor::pool_no_slot);
            Synth::LFODescriptor& lfo                                 = bank.lfos.entries[lfo_slot];
            lfo                                                       = Synth::LFODescriptor{};
            lfo.wave                                                  = Synth::WaveType::sine_wave;
            lfo.period_ms                                             = 300;
            lfo.min_value                                             = -1.0f;
            lfo.min_max_delta                                         = 2.0f;
            instrument.layers[layer].targets[target].envelope_desc_id = static_cast<uint16_t>(env_slot + 1);
            instrument.layers[layer].targets[target].lfo_desc_id      = static_cast<uint16_t>(lfo_slot + 1);
        }
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        TEST(mapping.detached_count == 2 * Synth::max_layers * Synth::num_mod_targets);
        check_no_param_nodes(graph);
    }
    // Registry capacity, low layer count: one layer binds all twelve targets
    // (24 derived instances) and 120 surplus envelope records append 120
    // more generators, so the projection builds 145 nodes - within the
    // node pool but past any layer-count-sized registry.  Metadata
    // validation accepts the shape and the projection succeeds.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> low_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& low_bank   = *low_bank_storage;
        Synth::Instrument            instrument = {};
        memset(static_cast<void*>(&low_bank), 0, sizeof(low_bank));
        instrument             = Synth::Instrument{};
        instrument.layer_count = 1;
        set_default_dispersion_sections(instrument);
        for (uint32_t target = 0; target < Synth::num_mod_targets; target++) {
            const uint32_t env_slot = low_bank.bank.envelopes.allocate();
            TEST(env_slot != pool_no_slot);
            Synth::EnvelopeDescriptor& env = low_bank.bank.envelopes.entries[env_slot];
            env                            = Synth::EnvelopeDescriptor{};
            env.num_points                 = 2;
            env.sustain_first_point        = 0;
            env.sustain_last_point         = 1;
            env.min_value                  = -0.5f;
            env.min_max_delta              = 2.0f;
            env.points[0]                  = { 0, 0x4000 };
            env.points[1]                  = { 100, 0xC000 };
            const uint32_t lfo_slot        = low_bank.bank.lfos.allocate();
            TEST(lfo_slot != pool_no_slot);
            Synth::LFODescriptor& lfo                             = low_bank.bank.lfos.entries[lfo_slot];
            lfo                                                   = Synth::LFODescriptor{};
            lfo.wave                                              = Synth::WaveType::sine_wave;
            lfo.period_ms                                         = 300;
            lfo.min_value                                         = -1.0f;
            lfo.min_max_delta                                     = 2.0f;
            instrument.layers[0].targets[target].envelope_desc_id = static_cast<uint16_t>(env_slot + 1);
            instrument.layers[0].targets[target].lfo_desc_id      = static_cast<uint16_t>(lfo_slot + 1);
        }
        // One unbound envelope descriptor carries all 120 surplus records.
        const uint32_t surplus_env = low_bank.bank.envelopes.allocate();
        TEST(surplus_env != pool_no_slot);
        Synth::EnvelopeDescriptor& surplus = low_bank.bank.envelopes.entries[surplus_env];
        surplus                            = low_bank.bank.envelopes.entries[0];
        low_bank.bank.channel_enabled[0]   = 1;
        TEST(low_bank.bank.instruments.allocate() == 0);
        low_bank.bank.instruments.entries[0]         = instrument;
        low_bank.bank.channel_zones[0][0].start_note = 1;
        low_bank.bank.channel_zones[0][0].instrument = 0;
        for (uint32_t i = 0; i < Synth::max_detached_per_zone; i++) {
            add_detached_record(&low_bank,
                                0,
                                0,
                                1,
                                surplus_env + 1,
                                11.0f,
                                22.0f,
                                Synth::ModSource::none,
                                Synth::ModSource::none,
                                static_cast<uint8_t>(i + 1));
        }
        TEST(Sculptor::validate_editor_metadata(low_bank));
        const std::unique_ptr<Sculptor::Graph> low_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       low_graph         = *low_graph_storage;
        Sculptor::OscGraphMapping              low_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(low_bank, &low_graph, &low_mapping));
        TEST(low_mapping.detached_count == 2 * Synth::num_mod_targets + Synth::max_detached_per_zone);
        check_no_param_nodes(low_graph);
    }

    // C1 persistence: deleting a generator's last wire keeps the generator
    // projected with a layout record across re-projection.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        if (begin_small_graph(&source, &t7_graph, &t7_mapping)) {
            TEST(source.graph_layout_count == 0); // record-less derived instances
            const int32_t lfo_inst = find_detached_by_desc(t7_mapping, 2, 1);
            TEST(lfo_inst >= 0);
            if (lfo_inst >= 0) {
                const uint32_t lfo_node = t7_mapping.detached[lfo_inst].node_idx;
                // The LFO's last reference edge is the wire into the volume
                // block's LFO dot.
                const uint32_t wire = find_osc_graph_connection(
                    t7_graph,
                    { t7_mapping.osc_nodes[0], Sculptor::osc_target_lfo_dot(Synth::mod_volume) });
                TEST(wire != Sculptor::pool_no_slot);
                if (wire != Sculptor::pool_no_slot) {
                    TEST(t7_graph.get_connection(wire).output.node_idx == lfo_node);
                    t7_graph.delete_connection(wire);
                    Sculptor::GraphChange changes[8] = {};
                    const uint32_t        count      = t7_graph.take_changes(changes, 8);
                    TEST(count == 1);
                    for (uint32_t i = 0; i < count; i++) {
                        TEST(Sculptor::apply_osc_graph_change(&source, &t7_graph, &t7_mapping, changes[i]));
                    }
                    // The record-less, unreferenced generator gains a
                    // layout record.
                    TEST(count_records_matching(source, 0, 0, 2, 1) == 1);
                    // Re-projection keeps the free-standing generator
                    // projected.
                    TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
                    (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
                    const int32_t still = find_detached_by_desc(t7_mapping, 2, 1);
                    TEST(still >= 0);
                    if (still >= 0) {
                        TEST(t7_mapping.detached[still].node_idx != Sculptor::pool_no_slot);
                        TEST(t7_mapping.detached[still].uid != 0);
                    }
                }
            }
        }
    }

    // C1 persistence: a free-standing LFO's depth/rate source records
    // follow the live edges across re-projection and undo/redo.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source     = *source_storage;
        Synth::Instrument            instrument = {};
        build_zone_fixture(&source, &instrument);
        const uint16_t free_lfo = add_detached_lfo_descriptor(&source);
        add_detached_record(&source,
                            0,
                            0,
                            2,
                            free_lfo,
                            120.0f,
                            60.0f,
                            Synth::ModSource::none,
                            Synth::ModSource::none,
                            1);
        TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
        (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
        const int32_t inst = find_detached_by_desc(t7_mapping, 2, free_lfo);
        TEST(inst >= 0); // the record materializes the free-standing node
        if (inst >= 0) {
            const uint32_t           node         = t7_mapping.detached[inst].node_idx;
            const uint32_t           velocity_idx = static_cast<uint32_t>(Synth::ModSource::velocity) - 1;
            const Sculptor::EndPoint velocity_out = { t7_mapping.input_node,
                                                      t7_mapping.input_source_slots[velocity_idx] };
            // Validator-enabled depth/rate regression: the standard MIDI
            // source output into the LFO depth and rate inputs passes
            // osc_graph_validate (the C1(b) raw add below deliberately
            // bypasses it), and the same inputs refuse a CC dot.
            t7_graph.set_validator(Sculptor::osc_graph_validate, &t7_mapping);
            TEST(Sculptor::osc_graph_validate(&t7_mapping,
                                              t7_graph,
                                              velocity_out,
                                              { node, t7_mapping.lfo_depth_input_slot }));
            TEST(Sculptor::osc_graph_validate(&t7_mapping,
                                              t7_graph,
                                              velocity_out,
                                              { node, t7_mapping.lfo_rate_input_slot }));
            {
                const std::unique_ptr<Sculptor::Graph> cc_graph_storage = std::make_unique<Sculptor::Graph>();
                Sculptor::Graph&                       cc_graph         = *cc_graph_storage;
                Sculptor::OscGraphMapping              cc_mapping       = {};
                const std::unique_ptr<Synth::InstrumentEditorBank> cc_probe_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& cc_probe = *cc_probe_storage;
                Synth::Instrument            cc_instr = {};
                build_zone_fixture(&cc_probe, &cc_instr);
                cc_probe.added_cc_mask[0] = 1u << 0;
                const uint16_t probe_lfo  = add_detached_lfo_descriptor(&cc_probe);
                add_detached_record(&cc_probe,
                                    0,
                                    0,
                                    2,
                                    probe_lfo,
                                    120.0f,
                                    60.0f,
                                    Synth::ModSource::none,
                                    Synth::ModSource::none,
                                    1);
                TEST(Sculptor::project_editor_to_graph(cc_probe, &cc_graph, &cc_mapping));
                (void)cc_graph.take_changes(discarded, Sculptor::max_pending_changes);
                char cc_label[32];
                Sculptor::fx_cc_pair_label(Synth::cc_msb_base, cc_label, sizeof(cc_label));
                uint32_t cc_slot = Sculptor::pool_no_slot;
                TEST(find_graph_slot(cc_graph, cc_mapping.input_node, cc_label, &cc_slot));
                TEST(cc_slot != Sculptor::pool_no_slot);
                const int32_t probe_inst = find_detached_by_desc(cc_mapping, 2, probe_lfo);
                TEST(probe_inst >= 0);
                if (cc_slot != Sculptor::pool_no_slot && probe_inst >= 0) {
                    TEST(! Sculptor::osc_graph_validate(
                        &cc_mapping,
                        cc_graph,
                        { cc_mapping.input_node, cc_slot },
                        { cc_mapping.detached[probe_inst].node_idx, cc_mapping.lfo_depth_input_slot }));
                }
            }
            t7_graph.set_validator(nullptr, nullptr);
            TEST(t7_graph.add_connection(velocity_out, { node, t7_mapping.lfo_depth_input_slot }) !=
                 Sculptor::pool_no_slot);
            Sculptor::GraphChange changes[8] = {};
            const uint32_t        count      = t7_graph.take_changes(changes, 8);
            TEST(count == 1);
            // Whole-bank undo around the rewire.
            const std::unique_ptr<FixedArray<uint8_t, sizeof(Synth::InstrumentEditorBank) * 4 + 256>>
                undo_storage_storage =
                    std::make_unique<FixedArray<uint8_t, sizeof(Synth::InstrumentEditorBank) * 4 + 256>>();
            uint8_t (&undo_storage)[sizeof(Synth::InstrumentEditorBank) * 4 + 256] = undo_storage_storage->elements;
            Sculptor::UndoRedo undo;
            undo.init(undo_storage);
            undo.init_undo_push();
            undo.push(&source, sizeof(source));
            TEST(undo.finish_undo_push());
            TEST(Sculptor::apply_osc_graph_change(&source, &t7_graph, &t7_mapping, changes[0]));
            // The record followed the live edge.
            const Synth::GraphNodeLayout* record = nullptr;
            for (uint32_t i = 0; i < source.graph_layout_count; i++) {
                if (source.graph_layout[i].kind == 2 && source.graph_layout[i].uid == 1) {
                    record = &source.graph_layout[i];
                }
            }
            TEST(record != nullptr);
            if (record) {
                TEST(record->depth_source == static_cast<uint8_t>(Synth::ModSource::velocity));
            }
            const std::unique_ptr<Synth::InstrumentEditorBank> wired_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& wired = *wired_storage;
            wired                              = source;
            undo.init_redo_push();
            undo.push(&wired, sizeof(wired));
            TEST(undo.finish_redo_push());
            // Undo: the record reverts and re-projection has no depth wire.
            TEST(undo.init_undo());
            undo.pop(&source, sizeof(source));
            undo.finish_undo();
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
            const int32_t undone = find_detached_by_desc(t7_mapping, 2, free_lfo);
            TEST(undone >= 0);
            if (undone >= 0) {
                TEST(find_osc_graph_connection(t7_graph,
                                               { t7_mapping.detached[undone].node_idx,
                                                 t7_mapping.lfo_depth_input_slot }) == Sculptor::pool_no_slot);
            }
            // Redo: the record carries the source again and re-projection
            // restores the wire.
            TEST(undo.init_redo());
            undo.pop(&source, sizeof(source));
            undo.finish_redo();
            TEST(Sculptor::project_editor_to_graph(source, &t7_graph, &t7_mapping));
            (void)t7_graph.take_changes(discarded, Sculptor::max_pending_changes);
            const int32_t redone = find_detached_by_desc(t7_mapping, 2, free_lfo);
            TEST(redone >= 0);
            if (redone >= 0) {
                TEST(find_osc_graph_connection(t7_graph,
                                               { t7_mapping.detached[redone].node_idx,
                                                 t7_mapping.lfo_depth_input_slot }) != Sculptor::pool_no_slot);
                // Unwiring zeroes the record's source again.
                const uint32_t depth_wire = find_osc_graph_connection(
                    t7_graph,
                    { t7_mapping.detached[redone].node_idx, t7_mapping.lfo_depth_input_slot });
                t7_graph.delete_connection(depth_wire);
                const uint32_t deleted = t7_graph.take_changes(changes, 8);
                TEST(deleted == 1);
                TEST(Sculptor::apply_osc_graph_change(&source, &t7_graph, &t7_mapping, changes[0]));
                const Synth::GraphNodeLayout* unwired = nullptr;
                for (uint32_t i = 0; i < source.graph_layout_count; i++) {
                    if (source.graph_layout[i].kind == 2 && source.graph_layout[i].uid == 1) {
                        unwired = &source.graph_layout[i];
                    }
                }
                TEST(unwired != nullptr);
                if (unwired) {
                    TEST(unwired->depth_source == 0);
                }
            }
        }
    }
}

// Oscillator-pane CC regressions (M6 carry-over).  The registry shares the
// per-channel added_cc_mask with the fx pane; the projection adds one MIDI
// Inputs output dot per set bit and the layer bindings' cc inputs wire the
// oscillator source rows through it; the validator confines CC dots to the
// Source rows; bank validation and the routing encoder admit cc with
// cc_number 16..31 and keep the non-CC-must-be-zero rule.  The menu commands
// are GUI-only (sculptor_instr_edit.cpp is not linked into this suite), so
// the tests drive the mask bits directly - the same shape the fx-pane
// registry tests use.
static void osc_cc_regressions()
{
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes] = {};

    // T-M6-1: the editor CC mask projects one CC output dot per set bit
    // (labeled like the fx pane's pairs), the wired cell compiles the cc
    // input, and clearing the mask bit drops the dot so the re-projection
    // leaves the cell unwired and the compile drops the input.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        build_zone_fixture(&source, &instrument);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        Synth::TargetBinding& routing = source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume];
        routing.num_inputs            = 1;
        routing.inputs[0].source      = Synth::ModSource::cc;
        routing.inputs[0].op          = Synth::SourceOp::add;
        routing.inputs[0].cc_number   = Synth::cc_msb_base;
        routing.inputs[0].scale       = 0.5f;
        source.added_cc_mask[0]       = 1u << 0;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        (void)graph.take_changes(discarded, Sculptor::max_pending_changes);
        char cc_label[32];
        Sculptor::fx_cc_pair_label(Synth::cc_msb_base, cc_label, sizeof(cc_label));
        uint32_t cc_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.input_node, cc_label, &cc_slot));
        TEST(mapping.input_cc_slots[0] == cc_slot);
        // The detune rows live on the dedicated Instrument Parameters node.
        uint32_t note_detune_slot = Sculptor::pool_no_slot;
        uint32_t osc_detune_slot  = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.instrument_node, "Random Note Detune", &note_detune_slot));
        TEST(find_graph_slot(graph, mapping.instrument_node, "Random Oscillator Detune", &osc_detune_slot));
        TEST(note_detune_slot == mapping.instrument_slots[0]);
        TEST(osc_detune_slot == mapping.instrument_slots[1]);
        // The detune rows drain through the value_changed apply path: the
        // event routes the Instrument Parameters node's edits into the
        // zone's compiled instrument.
        Sculptor::PropertyValue detune_edit = {};
        detune_edit.real                    = 0.4f;
        graph.set_slot_value(mapping.instrument_node, mapping.instrument_slots[0], detune_edit);
        Sculptor::GraphChange detune_change = {};
        detune_change.kind                  = Sculptor::ChangeKind::value_changed;
        detune_change.node_idx              = mapping.instrument_node;
        detune_change.slot_idx              = mapping.instrument_slots[0];
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, detune_change));
        TEST(source.bank.instruments.entries[0].note_skew_semitones == 0.4f);
        detune_edit.real = 0.9f;
        graph.set_slot_value(mapping.instrument_node, mapping.instrument_slots[1], detune_edit);
        detune_change.slot_idx = mapping.instrument_slots[1];
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, detune_change));
        TEST(source.bank.instruments.entries[0].layer_skew_semitones == 0.9f);
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].targets[Synth::mod_volume].num_inputs == 1);
        TEST(compiled.layers[0].targets[Synth::mod_volume].inputs[0].source == Synth::ModSource::cc);
        TEST(compiled.layers[0].targets[Synth::mod_volume].inputs[0].cc_number == Synth::cc_msb_base);
        source.added_cc_mask[0] = 0;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        (void)graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.input_cc_slots[0] == Sculptor::pool_no_slot);
        uint32_t gone_slot = Sculptor::pool_no_slot;
        TEST(! find_graph_slot(graph, mapping.input_node, cc_label, &gone_slot));
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].targets[Synth::mod_volume].num_inputs == 0);
        // The bank keeps carrying the undotted routing.
        TEST(source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].source ==
             Synth::ModSource::cc);
    }

    // T-M6-3 (undotted legacy shape): a cc routing with no registry bit
    // projects the source rows unwired, the compile drops the input, and
    // the bank keeps carrying it.  This pins the
    // decode-never-synthesizes-registry-bits rule (T-M4-9 analogue).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        build_zone_fixture(&source, &instrument);
        Synth::TargetBinding& legacy_routing = source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume];
        legacy_routing.num_inputs            = 1;
        legacy_routing.inputs[0].source      = Synth::ModSource::cc;
        legacy_routing.inputs[0].op          = Synth::SourceOp::add;
        legacy_routing.inputs[0].cc_number   = static_cast<uint8_t>(Synth::cc_msb_base);
        legacy_routing.inputs[0].scale       = 0.5f;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        (void)graph.take_changes(discarded, Sculptor::max_pending_changes);
        const uint32_t osc = mapping.osc_nodes[0];
        TEST(osc != Sculptor::pool_no_slot);
        if (osc != Sculptor::pool_no_slot) {
            TEST(find_osc_graph_connection(graph, { osc, Sculptor::osc_target_src_dot(Synth::mod_volume, 0) }) ==
                 Sculptor::pool_no_slot);
            TEST(find_osc_graph_connection(graph, { osc, Sculptor::osc_target_src_dot(Synth::mod_volume, 1) }) ==
                 Sculptor::pool_no_slot);
            Synth::Instrument compiled = {};
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            for (uint32_t layer = 0; layer < compiled.layer_count; ++layer) {
                TEST(compiled.layers[layer].targets[Synth::mod_volume].num_inputs == 0);
            }
            TEST(source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].source ==
                 Synth::ModSource::cc);
        }
    }

    // T-M6-5: removal-refusal inputs.  The removal handlers are GUI-only
    // (sculptor_instr_edit.cpp is not linked into this suite), so the tests
    // pin the predicates the refusals consult.  The master chain keeps the
    // fx-chain-only guard: fx_chain_uses_cc is true when the master chain
    // wires the CC and stays false when only a zone's osc routing wires it,
    // so the master case never consults channel_zones.  The channel-wide
    // widened guard (channel_uses_cc over the fx chain OR the zone osc
    // bindings) is pinned below: it fires for the zone osc wiring and for
    // the channel's own fx chain, not for master-chain wiring.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source     = *source_storage;
        Synth::Instrument            instrument = {};
        build_zone_fixture(&source, &instrument);
        Synth::TargetBinding& routing = source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume];
        routing.num_inputs            = 1;
        routing.inputs[0].source      = Synth::ModSource::cc;
        routing.inputs[0].op          = Synth::SourceOp::add;
        routing.inputs[0].cc_number   = Synth::cc_msb_base;
        routing.inputs[0].scale       = 0.5f;
        Synth::EffectSlotBinding* const master_slot =
            enabled_effect(source.bank, Synth::max_channels, 0, Synth::EffectType::delay);
        master_slot->bindings[0].num_inputs           = 1;
        master_slot->bindings[0].inputs[0].source     = Synth::ModSource::cc;
        master_slot->bindings[0].inputs[0].op         = Synth::SourceOp::add;
        master_slot->bindings[0].inputs[0].cc_number  = static_cast<uint8_t>(Synth::cc_msb_base + 2);
        master_slot->bindings[0].inputs[0].scale      = 0.5f;
        Synth::EffectSlotBinding* const channel_slot  = enabled_effect(source.bank, 0, 0, Synth::EffectType::delay);
        channel_slot->bindings[0].num_inputs          = 1;
        channel_slot->bindings[0].inputs[0].source    = Synth::ModSource::cc;
        channel_slot->bindings[0].inputs[0].op        = Synth::SourceOp::add;
        channel_slot->bindings[0].inputs[0].cc_number = static_cast<uint8_t>(Synth::cc_msb_base + 1);
        channel_slot->bindings[0].inputs[0].scale     = 0.5f;
        TEST(Sculptor::fx_chain_uses_cc(source.bank.master_chain, Synth::cc_msb_base + 2));
        TEST(! Sculptor::fx_chain_uses_cc(source.bank.master_chain, Synth::cc_msb_base));
        TEST(! Sculptor::fx_chain_uses_cc(source.bank.channel_chains[0], Synth::cc_msb_base));
        // The channel-wide widened guard: the zone osc binding, the
        // channel's own fx chain, no master-chain leak, no out-of-range hit.
        TEST(Sculptor::channel_uses_cc(source, 0, Synth::cc_msb_base));
        TEST(Sculptor::channel_uses_cc(source, 0, Synth::cc_msb_base + 1));
        TEST(! Sculptor::channel_uses_cc(source, 0, Synth::cc_msb_base + 2));
        TEST(! Sculptor::channel_uses_cc(source, 0, Synth::cc_msb_base + 3));
        TEST(! Sculptor::channel_uses_cc(source, 0, 0));
        TEST(! Sculptor::channel_uses_cc(source, Synth::max_channels, Synth::cc_msb_base));
        // An empty zone slot's instrument byte is meaningless: a channel
        // with no zones must not alias instrument 0's wiring.
        TEST(! Sculptor::channel_uses_cc(source, 1, Synth::cc_msb_base));
    }

    // T-M6-6: persistence.  An osc cc routing input rides the existing
    // per-layer sources arrays: the bank encoder already spells cc, so
    // bank validation is the decode-time admission gate; once admitted,
    // encode/decode is byte-identical.  Decode refuses a cc_number outside
    // 16..31 and a non-cc input carrying a stale cc_number; a legacy bank
    // without osc cc decodes unchanged.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source     = *source_storage;
        Synth::Instrument                                  instrument = {};
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                                        decoded = *decoded_storage;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&payload)[Synth::library_payload_max] = payload_storage->elements;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> reencoded_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&reencoded)[Synth::library_payload_max] = reencoded_storage->elements;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> patched_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&patched)[Synth::library_payload_max] = patched_storage->elements;
        build_zone_fixture(&source, &instrument);
        Synth::TargetBinding& routing = source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume];
        routing.num_inputs            = 1;
        routing.inputs[0].source      = Synth::ModSource::cc;
        routing.inputs[0].op          = Synth::SourceOp::add;
        routing.inputs[0].cc_number   = static_cast<uint8_t>(Synth::cc_msb_base);
        routing.inputs[0].scale       = 0.5f;
        const uint32_t encoded_len    = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(encoded_len > 0);
        if (encoded_len > 0) {
            TEST(Synth::decode_editor_bank_json(payload, encoded_len, &decoded));
            TEST(decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].source ==
                 Synth::ModSource::cc);
            TEST(decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].cc_number ==
                 Synth::cc_msb_base);
            const uint32_t again_len = Synth::encode_editor_bank_json(&decoded, reencoded, sizeof(reencoded));
            TEST(again_len == encoded_len);
            TEST(memcmp(payload, reencoded, encoded_len) == 0);
        }
        // Decode refusals, shaped by splicing the compact encoder output:
        // the fixture's single velocity input is the only source spelling
        // in the document, so the needle is unique by construction.
        build_zone_fixture(&source, &instrument);
        const uint32_t clean_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(clean_len > 0);
        if (clean_len > 0) {
            uint32_t    velocity_count = 0;
            const char* scan           = payload;
            while ((scan = strstr(scan, "\"source\":\"velocity\"")) != nullptr) {
                velocity_count++;
                scan++;
            }
            TEST(velocity_count == 1);
            const uint32_t low_len = splice_single_occurrence(payload,
                                                              clean_len,
                                                              "\"source\":\"velocity\"",
                                                              "\"source\":\"cc\",\"cc_number\":15",
                                                              patched,
                                                              sizeof(patched));
            TEST(low_len > 0);
            if (low_len > 0) {
                TEST(! Synth::decode_editor_bank_json(patched, low_len, &decoded));
            }
            const uint32_t high_len = splice_single_occurrence(payload,
                                                               clean_len,
                                                               "\"source\":\"velocity\"",
                                                               "\"source\":\"cc\",\"cc_number\":32",
                                                               patched,
                                                               sizeof(patched));
            TEST(high_len > 0);
            if (high_len > 0) {
                TEST(! Synth::decode_editor_bank_json(patched, high_len, &decoded));
            }
            const uint32_t stale_len = splice_single_occurrence(payload,
                                                                clean_len,
                                                                "\"source\":\"velocity\"",
                                                                "\"source\":\"velocity\",\"cc_number\":17",
                                                                patched,
                                                                sizeof(patched));
            TEST(stale_len > 0);
            if (stale_len > 0) {
                TEST(! Synth::decode_editor_bank_json(patched, stale_len, &decoded));
            }
        }
        // A legacy bank without osc cc decodes unchanged.
        build_zone_fixture(&source, &instrument);
        const uint32_t legacy_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(legacy_len > 0);
        if (legacy_len > 0) {
            TEST(Synth::decode_editor_bank_json(payload, legacy_len, &decoded));
            TEST(decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].source ==
                 Synth::ModSource::velocity);
            TEST(decoded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].inputs[0].cc_number == 0);
        }
    }

    // T-M6-8: end-to-end runtime seam.  A bank whose layer-0 volume binding
    // carries a cc input modulates through the production data path: the
    // validate preflight admits the 16..31 cc_number (the rule that keeps
    // resolve_source's slot assert in realtime_synth.cpp unreachable),
    // apply_channel_cc drives the channel's CC leaf exactly the way
    // process_controller writes it, and the binding's inputs configure the
    // volume dest whose value propagate_parameters folds.
    // configure_target_modulation and resolve_source are static in
    // realtime_synth.cpp and not linked here, so the block below is the
    // production-shape replica of their cc case, built only from exported
    // primitives and constants and mirroring realtime_synth.cpp:1039-1171.
    {
        const std::unique_ptr<Synth::InstrumentBank> runtime_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       runtime_bank         = *runtime_bank_storage;
        memset(static_cast<void*>(&runtime_bank), 0, sizeof(runtime_bank));
        const uint32_t instr_slot = runtime_bank.instruments.allocate();
        TEST(instr_slot == 0);
        if (instr_slot == 0) {
            Synth::Instrument&    cc_instr  = runtime_bank.instruments.entries[instr_slot];
            Synth::TargetBinding& cc_volume = cc_instr.layers[0].targets[Synth::mod_volume];
            cc_instr.layer_count            = 1;
            set_default_dispersion_sections(cc_instr);
            cc_instr.layers[0].osc_type[0]              = Synth::WaveType::sine_wave;
            cc_volume.base_value                        = 0.8f;
            cc_volume.num_inputs                        = 1;
            cc_volume.inputs[0].source                  = Synth::ModSource::cc;
            cc_volume.inputs[0].op                      = Synth::SourceOp::add;
            cc_volume.inputs[0].cc_number               = static_cast<uint8_t>(Synth::cc_msb_base);
            cc_volume.inputs[0].scale                   = 0.5f;
            runtime_bank.channel_zones[0][0].start_note = 1;
            runtime_bank.channel_zones[0][0].instrument = static_cast<uint8_t>(instr_slot);
            runtime_bank.channel_enabled[0]             = 1;
            TEST(Synth::validate_instrument_bank(&runtime_bank));
            // Leaf ids mirror the runtime layout: the sentinel occupies id 0
            // and the channel CC leaves start at 1, one per assignable slot
            // (channel_param(channel, chan_param_cc_0 + slot)).
            constexpr uint32_t     cc_leaf_base          = 1;
            constexpr uint32_t     replica_count         = cc_leaf_base + Synth::num_assignable_ccs + 1;
            Synth::ParamDescriptor descs[replica_count]  = {};
            Synth::Parameter       params[replica_count] = {};
            memset(static_cast<void*>(descs), 0, sizeof(descs));
            memset(static_cast<void*>(params), 0, sizeof(params));
            Synth::ChannelCcState cc_state = {};
            memset(static_cast<void*>(&cc_state), 0, sizeof(cc_state));
            const Synth::TargetBinding& volume_binding =
                runtime_bank.instruments.entries[instr_slot].layers[0].targets[Synth::mod_volume];
            Synth::SourceParam sources[Synth::max_mod_inputs] = {};
            uint32_t           num_sources                    = 0;
            for (uint32_t i = 0; i < volume_binding.num_inputs && i < Synth::max_mod_inputs; ++i) {
                const Synth::ModInput& input = volume_binding.inputs[i];
                TEST(input.source == Synth::ModSource::cc);
                TEST(Synth::cc_is_assignable(input.cc_number));
                sources[num_sources].param_id =
                    static_cast<uint16_t>(cc_leaf_base + (input.cc_number - Synth::cc_msb_base));
                sources[num_sources].scale = input.scale;
                sources[num_sources].op    = input.op;
                num_sources++;
            }
            Synth::configure_plain(&descs[cc_leaf_base + Synth::num_assignable_ccs],
                                   volume_binding.base_value,
                                   0,
                                   1.0f,
                                   0,
                                   Synth::SourceOp::add,
                                   sources,
                                   num_sources);
            const uint32_t cc_leaf     = cc_leaf_base + 0;
            const uint32_t volume_dest = cc_leaf_base + Synth::num_assignable_ccs;
            // Stage one: CC 16 at zero keeps the volume at its base value.
            const float closed = Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 0);
            TEST(approx(closed, 0.0f, 0.0001f));
            Synth::write_external_leaf(closed,
                                       Synth::ExternalLeaf{ &params[cc_leaf].value, &params[cc_leaf].prev_value });
            Synth::propagate_parameters(params, descs, replica_count);
            TEST(approx(params[volume_dest].value, 0.8f, 0.001f));
            // Stage two: CC 16 at full deflection moves the volume.  The
            // zero-initialized channel state carries lsb 0 (the 0xFF
            // not-yet-seen sentinel exists only in freshly constructed
            // production state), so the 14-bit value is cc_14bit_value(127, 0).
            const float open = Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 127);
            TEST(approx(open, Synth::cc_14bit_value(127, 0), 0.0001f));
            Synth::write_external_leaf(open,
                                       Synth::ExternalLeaf{ &params[cc_leaf].value, &params[cc_leaf].prev_value });
            Synth::propagate_parameters(params, descs, replica_count);
            TEST(approx(params[volume_dest].value, 0.8f + 0.5f * Synth::cc_14bit_value(127, 0), 0.001f));
        }
    }

    // T-M6-9: a target LFO cc source fails decode - LFO depth/rate sources
    // carry no cc_number, so bank validation refuses cc there.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       source     = *source_storage;
        Synth::Instrument                                  instrument = {};
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                                        decoded = *decoded_storage;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&payload)[Synth::library_payload_max] = payload_storage->elements;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> patched_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&patched)[Synth::library_payload_max] = patched_storage->elements;
        build_zone_fixture(&source, &instrument);
        source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_depth_source =
            Synth::ModSource::velocity;
        const uint32_t clean_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(clean_len > 0);
        if (clean_len > 0) {
            TEST(Synth::decode_editor_bank_json(payload, clean_len, &decoded));
            // The volume binding is the document's only lfo with a
            // depth_source spelling, so the needle is unique by construction.
            // (The bank format spells generator sources as lfo_depth_source.)
            uint32_t    depth_source_count = 0;
            const char* scan               = payload;
            while ((scan = strstr(scan, "\"lfo_depth_source\":\"velocity\"")) != nullptr) {
                depth_source_count++;
                scan++;
            }
            TEST(depth_source_count == 1);
            const uint32_t spliced_len = splice_single_occurrence(payload,
                                                                  clean_len,
                                                                  "\"lfo_depth_source\":\"velocity\"",
                                                                  "\"lfo_depth_source\":\"cc\"",
                                                                  patched,
                                                                  sizeof(patched));
            TEST(spliced_len > 0);
            if (spliced_len > 0) {
                TEST(! Synth::decode_editor_bank_json(patched, spliced_len, &decoded));
            }
        }
    }
}

// Exercise pitch composition and gating helpers, model the termination
// decisions, and check graph projection and compilation. realtime_synth.cpp
// is not linked here; the models do not prove runtime fade or freeing behavior.

static void check_osc_pitch_gating()
{
    // Absolute pitch composition and conversion: the octave shift doubles
    // the frequency, and no consumer adds the base note a second time.
    TEST(approx(Synth::pitch_to_frequency(Synth::oscillator_final_pitch(69, 0, 0), 1), 440.0f, 0.001f));
    TEST(approx(Synth::pitch_to_frequency(Synth::oscillator_final_pitch(69, 1, 0), 1), 880.0f, 0.001f));
    TEST(approx(Synth::pitch_to_frequency(Synth::oscillator_final_pitch(69, 0, 0), 3), 1320.0f, 0.001f));
    TEST(Synth::oscillator_final_pitch(60, -1, 1.25f) == 49.25f);

    // Gate predicate: inclusive bounds, no rounding.
    TEST(Synth::oscillator_pitch_in_gate(0.0f));
    TEST(Synth::oscillator_pitch_in_gate(127.0f));
    TEST(! Synth::oscillator_pitch_in_gate(-0.5f));
    TEST(! Synth::oscillator_pitch_in_gate(127.5f));
    TEST(! Synth::oscillator_pitch_in_gate(-12.0f));
    TEST(! Synth::oscillator_pitch_in_gate(139.0f));
    TEST(Synth::oscillator_final_pitch(60, 12, 0) == 204.0f);
    TEST(! Synth::oscillator_pitch_in_gate(Synth::oscillator_final_pitch(60, 12, 0)));

    // param_onset_value: read-only trigger-time evaluation of a configured
    // parameter node.
    {
        Synth::Parameter       params[8] = {};
        Synth::ParamDescriptor descs[8]  = {};
        memset(static_cast<void*>(params), 0, sizeof(params));
        memset(static_cast<void*>(descs), 0, sizeof(descs));

        // External leaf: the leaf's current value, value and prev seeded alike.
        descs[1].kind = Synth::ParamKind::external;
        Synth::write_external_leaf(3.25f, Synth::ExternalLeaf{ &params[1].value, &params[1].prev_value });
        TEST(params[1].value == params[1].prev_value);
        TEST(Synth::param_onset_value(params, descs, 1) == 3.25f);

        // Plain add: base 1.0 + scale 2 * source 3.25.
        descs[2].kind              = Synth::ParamKind::plain;
        descs[2].plain.base_value  = 1.0f;
        descs[2].plain.num_sources = 1;
        descs[2].plain.sources[0]  = { 1, 2.0f, Synth::SourceOp::add };
        TEST(Synth::param_onset_value(params, descs, 2) == 7.5f);

        // Plain multiply: base 2.0 * scale 0.5 * source 3.25.
        descs[3].kind              = Synth::ParamKind::plain;
        descs[3].plain.base_value  = 2.0f;
        descs[3].plain.num_sources = 1;
        descs[3].plain.sources[0]  = { 1, 0.5f, Synth::SourceOp::multiply };
        TEST(approx(Synth::param_onset_value(params, descs, 3), 3.25f, 1.0e-6f));

        // Envelope: the onset is the first point's value at the cleared
        // note-on state, and the state stays untouched.  The first point sits
        // inside the gate while one advanced tick already reads outside it,
        // so the straddle pins the no-advance rule.
        descs[4]                              = {};
        descs[4].kind                         = Synth::ParamKind::envelope;
        descs[4].envelope.num_points          = 2;
        descs[4].envelope.sustain_first_point = 0;
        descs[4].envelope.sustain_last_point  = 1;
        descs[4].envelope.min_value           = 0.0f;
        descs[4].envelope.min_max_delta       = 200.0f / 65535.0f;
        descs[4].envelope.points[0]           = { 0, 0 };
        descs[4].envelope.points[1]           = { 1, 0xFFFF };
        const float envelope_onset            = Synth::param_onset_value(params, descs, 4);
        TEST(envelope_onset == 0.0f);
        TEST(Synth::oscillator_pitch_in_gate(envelope_onset));
        TEST(params[4].envelope.tick == 0 && params[4].envelope.point == 0);
        Synth::EnvelopeState advanced = params[4].envelope;
        advanced.tick                 = 1;
        TEST(approx(Synth::eval_envelope_at(descs[4].envelope, advanced), 200.0f, 1.0e-3f));
        TEST(! Synth::oscillator_pitch_in_gate(Synth::eval_envelope_at(descs[4].envelope, advanced)));

        // LFO: tick-zero evaluation with external depth/rate source leaves,
        // and the LFO tick stays zero (no state advance).
        descs[5]                 = {};
        Synth::LFODescriptor lfo = {};
        lfo.wave                 = Synth::WaveType::sine_wave;
        lfo.duty                 = 0x7F;
        lfo.period_ms            = 100;
        lfo.min_value            = -1.0f;
        lfo.min_max_delta        = 2.0f;
        Synth::configure_lfo(&descs[5], lfo, Synth::SourceOp::multiply, 0.5f, 6, 7, 10.0f);
        descs[6].kind = Synth::ParamKind::external;
        Synth::write_external_leaf(0.8f, Synth::ExternalLeaf{ &params[6].value, &params[6].prev_value });
        // The onset must read the leaf's current value: driving prev_value to 0 makes a
        // prev_value-reading regression collapse the depth (result 1.0) and fail here.
        params[6].prev_value = 0.0f;
        descs[7].kind        = Synth::ParamKind::external;
        Synth::write_external_leaf(0.0f, Synth::ExternalLeaf{ &params[7].value, &params[7].prev_value });
        params[5] = {};
        // Sine at tick 0 reads wave 0.5, so multiply depth 0.5 * leaf 0.8
        // yields 1 - 0.4 * 0.5 = 0.8.
        TEST(approx(Synth::param_onset_value(params, descs, 5), 0.8f, 1.0e-4f));
        TEST(params[5].lfo_tick == 0);

        // Sourced-rate transport: the rate source's value offsets the period, so the
        // same node reads differently at tick 1 with different rate values.  At tick 0
        // the sine wave is period-independent, which is why the transport check needs
        // the later tick.
        Synth::ParamDescriptor::LFOParam rate_gen = {};
        rate_gen.lfo                              = lfo; // sine, 100 ms
        rate_gen.op                               = Synth::SourceOp::add;
        rate_gen.depth                            = 1.0f;
        rate_gen.rate_param_id                    = 7;
        rate_gen.rate_scale_ms                    = 100.0f;
        TEST(approx(Synth::eval_sourced_lfo(rate_gen, 1.0f, 0.0f, 0),
                    Synth::eval_sourced_lfo(rate_gen, 1.0f, 1.0f, 0),
                    1.0e-6f));
        TEST(! approx(Synth::eval_sourced_lfo(rate_gen, 1.0f, 0.0f, 1),
                      Synth::eval_sourced_lfo(rate_gen, 1.0f, 1.0f, 1),
                      1.0e-3f));
        // Without a rate source the descriptor period passes through unchanged.
        rate_gen.rate_param_id = 0;
        TEST(approx(Synth::eval_sourced_lfo(rate_gen, 1.0f, 0.0f, 1),
                    Synth::eval_sourced_lfo(rate_gen, 1.0f, 900.0f, 1),
                    1.0e-6f));

        // No mutation: a full snapshot before and after every call above.
        Synth::Parameter       params_snapshot[8] = {};
        Synth::ParamDescriptor descs_snapshot[8]  = {};
        memcpy(params_snapshot, params, sizeof(params));
        memcpy(descs_snapshot, descs, sizeof(descs));
        (void)Synth::param_onset_value(params, descs, 2);
        (void)Synth::param_onset_value(params, descs, 4);
        (void)Synth::param_onset_value(params, descs, 5);
        TEST(memcmp(params_snapshot, params, sizeof(params)) == 0);
        TEST(memcmp(descs_snapshot, descs, sizeof(descs)) == 0);

        // Later-leaf-write ordering: the verdict is computed once from the
        // onset state; a same-batch write after it cannot change the verdict.
        const bool verdict = Synth::oscillator_pitch_in_gate(
            Synth::oscillator_final_pitch(69, 0, Synth::param_onset_value(params, descs, 2)));
        TEST(verdict);
        Synth::write_external_leaf(900.0f, Synth::ExternalLeaf{ &params[1].value, &params[1].prev_value });
        TEST(! Synth::oscillator_pitch_in_gate(
            Synth::oscillator_final_pitch(69, 0, Synth::param_onset_value(params, descs, 2))));
        // Documentation, not a state check: the runtime consumes the verdict captured
        // at the event, never a re-read against later leaf writes.
        TEST(verdict);
    }

    // Trigger-time mirror: the note-on verdict flows through the
    // update_modulation free path exactly like a terminated oscillator
    // (free_now reads terminating first, a live oscillator enters termination
    // only through the voice-lifetime guard, and the free branch compacts the
    // voice's live list).
    {
        struct MirrorOsc {
            bool     alive; // the no_wave analog of an occupied slot
            bool     terminating;
            float    volume;
            uint8_t  layer_idx;
            bool     decaying;   // release decay running
            uint32_t decay_tick; // steps since the decay started
            bool     faded;      // took the guard's final fade block
            bool     freed;
        };

        struct MirrorVoice {
            uint32_t osc_count;
            uint8_t  osc_ids[3];
            bool     active;
            bool     note_mapped;
            bool     releasing;
            bool     note_finished;
            uint32_t release_total_ticks;
        };

        // One update_modulation pass over a two-slot pool (slot 0 unused,
        // mirroring the 1-based slot iteration and the no_wave skip).  The
        // completion guard is modeled here; the separate release-only
        // volume_termination_due path is not exercised by this mirror.
        auto modulation_step = [](MirrorVoice* voice, MirrorOsc* oscs) {
            for (uint32_t slot = 1; slot <= 2; ++slot) {
                MirrorOsc& osc = oscs[slot];
                if (! osc.alive) {
                    continue;
                }
                const bool free_now = osc.terminating;
                if (! free_now && (voice->note_finished || (voice->releasing && osc.decaying &&
                                                            osc.decay_tick >= voice->release_total_ticks))) {
                    osc.terminating = true;
                    osc.volume      = 0.0f; // Final fade block renders this step; the slot frees next step.
                    osc.faded       = true;
                }
                if (free_now) {
                    osc.freed = true;
                    osc.alive = false;
                    TEST(voice->osc_count != 0);
                    --voice->osc_count;
                    const uint8_t moved           = voice->osc_ids[voice->osc_count];
                    voice->osc_ids[osc.layer_idx] = moved;
                    oscs[moved].layer_idx         = osc.layer_idx;
                    if (! voice->osc_count) {
                        voice->active      = false;
                        voice->note_mapped = false;
                    }
                }
            }
        };

        // The verdicts come from the same helpers the runtime calls: one
        // accepted layer (note 69) and one rejected layer (octave +12 -> 204).
        const bool in_gate     = Synth::oscillator_pitch_in_gate(Synth::oscillator_final_pitch(69, 0, 0));
        const bool out_of_gate = ! Synth::oscillator_pitch_in_gate(Synth::oscillator_final_pitch(69, 12, 0));
        TEST(in_gate && out_of_gate);

        // (a) One accepted + one rejected layer: the rejected slot frees on
        // the first step and the accepted layer keeps its osc_ids position.
        {
            MirrorOsc   oscs[3] = {};
            MirrorVoice voice   = {};
            voice.osc_count     = 2;
            voice.osc_ids[0]    = 1;
            voice.osc_ids[1]    = 2;
            voice.active        = true;
            voice.note_mapped   = true;
            oscs[1].alive       = true; // accepted layer renders on
            oscs[1].layer_idx   = 0;
            oscs[2].alive       = true; // rejected at note-on, volume 0
            oscs[2].layer_idx   = 1;
            oscs[2].terminating = out_of_gate;
            oscs[2].volume      = 0.0f;
            modulation_step(&voice, oscs);
            TEST(oscs[2].freed);
            TEST(! oscs[1].freed && oscs[1].alive);
            TEST(voice.osc_count == 1 && voice.osc_ids[0] == 1);
            TEST(voice.active && voice.note_mapped);
        }

        // (b) All layers rejected: the voice empties, deactivates and gives up
        // the note mapping.
        {
            MirrorOsc   oscs[3] = {};
            MirrorVoice voice   = {};
            voice.osc_count     = 2;
            voice.osc_ids[0]    = 1;
            voice.osc_ids[1]    = 2;
            voice.active        = true;
            voice.note_mapped   = true;
            oscs[1].alive       = true;
            oscs[1].layer_idx   = 0;
            oscs[1].terminating = true;
            oscs[2].alive       = true;
            oscs[2].layer_idx   = 1;
            oscs[2].terminating = true;
            modulation_step(&voice, oscs);
            TEST(oscs[1].freed && oscs[2].freed);
            TEST(voice.osc_count == 0);
            TEST(! voice.active && ! voice.note_mapped);
        }

        // (c) Rejection alongside release/fade: with the voice releasing and the
        // slots' release decay past its cutoff, the rejected slot (free_now
        // already true) frees on the first step without taking the guard's fade
        // branch, while a live slot in the same state fades first (volume zeroed,
        // terminating set) and frees only on the next step.
        {
            MirrorOsc   oscs[3]       = {};
            MirrorVoice voice         = {};
            voice.osc_count           = 2;
            voice.osc_ids[0]          = 1;
            voice.osc_ids[1]          = 2;
            voice.active              = true;
            voice.releasing           = true;
            voice.release_total_ticks = 10;
            oscs[1].alive             = true; // rejected at note-on, volume 0
            oscs[1].layer_idx         = 0;
            oscs[1].terminating       = true;
            oscs[2].alive             = true; // live, release decay past the cutoff
            oscs[2].layer_idx         = 1;
            oscs[2].decaying          = true;
            oscs[2].decay_tick        = 10;
            oscs[2].volume            = 0.5f;
            modulation_step(&voice, oscs);
            TEST(oscs[1].freed && ! oscs[1].faded); // freed on step 1, no fade block
            TEST(! oscs[2].freed && oscs[2].faded && oscs[2].volume == 0.0f);
            TEST(voice.osc_count == 1 && voice.osc_ids[0] == 2);
            TEST(voice.active);
            modulation_step(&voice, oscs);
            TEST(oscs[2].freed);
            TEST(voice.osc_count == 0 && ! voice.active);
        }
    }

    // Projection and compile path: the Pitch row is a bounded bare input
    // box (no slider) at its target slot, the Octave Offset row is a
    // bounded integer slider, and the compile path validates the octave
    // before narrowing.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);

        uint32_t pitch_slot  = Sculptor::pool_no_slot;
        uint32_t octave_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "Pitch", &pitch_slot));
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "Octave Offset", &octave_slot));
        TEST(pitch_slot == Sculptor::osc_target_row(Synth::mod_pitch));
        TEST(octave_slot == 148);
        const Sculptor::Slot& pitch_row = graph.node(mapping.osc_nodes[0]).slots.entries[pitch_slot];
        TEST(pitch_row.property_type == Sculptor::PropertyType::real);
        TEST(! pitch_row.real_bounded); // a bare input box, not a slider
        TEST(pitch_row.real_min == Sculptor::osc_target_view(Synth::mod_pitch).min_value);
        TEST(pitch_row.real_max == Sculptor::osc_target_view(Synth::mod_pitch).max_value);
        TEST(pitch_row.value.real == instrument.layers[0].targets[Synth::mod_pitch].base_value);
        const Sculptor::Slot& octave_row = graph.node(mapping.osc_nodes[0]).slots.entries[octave_slot];
        TEST(octave_row.property_type == Sculptor::PropertyType::integer);
        TEST(octave_row.int_bounded);
        TEST(octave_row.real_min == static_cast<float>(Sculptor::osc_octave_offset_min));
        TEST(octave_row.real_max == static_cast<float>(Sculptor::osc_octave_offset_max));
        TEST(octave_row.value.integer == 0);
        const Sculptor::OscTargetView spread_view = Sculptor::osc_target_view(Synth::mod_fir_spread);
        TEST(spread_view.min_value == 0.0f && spread_view.max_value == 36.0f);
        TEST(spread_view.bank_scale == 1.0f && ! spread_view.logarithmic);
        TEST(Sculptor::osc_target_row(Synth::mod_fir_spread) == 166);

        // Round-trip: an octave edit applies through the value_changed path,
        // lands in the bank and reprojection shows it again.
        Sculptor::PropertyValue octave_edit = {};
        octave_edit.integer                 = -3;
        graph.set_slot_value(mapping.osc_nodes[0], octave_slot, octave_edit);
        Sculptor::GraphChange octave_change = {};
        octave_change.kind                  = Sculptor::ChangeKind::value_changed;
        octave_change.node_idx              = mapping.osc_nodes[0];
        octave_change.slot_idx              = octave_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, octave_change));
        TEST(source.bank.instruments.entries[0].layers[0].octave_offset == -3);

        const std::unique_ptr<Synth::InstrumentEditorBank> reprojected_source_storage =
            std::make_unique<Synth::InstrumentEditorBank>(source);
        Synth::InstrumentEditorBank&           reprojected_source        = *reprojected_source_storage;
        const std::unique_ptr<Sculptor::Graph> reprojected_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       reprojected_graph         = *reprojected_graph_storage;
        Sculptor::OscGraphMapping              reprojected_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(reprojected_source, &reprojected_graph, &reprojected_mapping));
        TEST(find_graph_slot(reprojected_graph, reprojected_mapping.osc_nodes[0], "Octave Offset", &octave_slot));
        TEST(reprojected_graph.node(reprojected_mapping.osc_nodes[0]).slots.entries[octave_slot].value.integer == -3);

        // Endpoint values compile; an out-of-range edit refuses the projection.
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].octave_offset == -3);
        // Round-trip: a pitch edit applies into the pitch target's base
        // value and reprojection shows it again.
        Sculptor::PropertyValue pitch_edit = {};
        pitch_edit.real                    = 7.5f;
        graph.set_slot_value(mapping.osc_nodes[0], pitch_slot, pitch_edit);
        Sculptor::GraphChange pitch_change = {};
        pitch_change.kind                  = Sculptor::ChangeKind::value_changed;
        pitch_change.node_idx              = mapping.osc_nodes[0];
        pitch_change.slot_idx              = pitch_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, pitch_change));
        TEST(source.bank.instruments.entries[0].layers[0].targets[Synth::mod_pitch].base_value == 7.5f);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].targets[Synth::mod_pitch].base_value == 7.5f);
        const int32_t octave_endpoints[2] = { -12, 12 };
        for (const int32_t endpoint : octave_endpoints) {
            octave_edit.integer = endpoint;
            graph.set_slot_value(mapping.osc_nodes[0], octave_slot, octave_edit);
            TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, octave_change));
            TEST(source.bank.instruments.entries[0].layers[0].octave_offset == endpoint);
            TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
            TEST(compiled.layers[0].octave_offset == endpoint);
        }
        octave_edit.integer = 13;
        graph.set_slot_value(mapping.osc_nodes[0], octave_slot, octave_edit);
        TEST(! Sculptor::apply_osc_graph_change(&source, &graph, &mapping, octave_change));
        TEST(! Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        // The refused edit leaves the compiled bank at the last good value.
        TEST(source.bank.instruments.entries[0].layers[0].octave_offset == 12);
    }
    // The FIR Slope row: the bounded integer slider at slot 149, seeded
    // from the bank field, round-tripping through the value_changed apply
    // path, and refusing out-of-range compile reads.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};
        build_zone_fixture(&source, &instrument);
        source.bank.instruments.entries[0].layers[0].fir_slope_percent = 37;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        uint32_t slope_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "FIR Slope", &slope_slot));
        TEST(slope_slot == 192);
        const Sculptor::Slot& slope_row = graph.node(mapping.osc_nodes[0]).slots.entries[slope_slot];
        TEST(slope_row.property_type == Sculptor::PropertyType::integer);
        TEST(slope_row.int_bounded);
        TEST(slope_row.real_min == 1.0f);
        TEST(slope_row.real_max == 100.0f);
        TEST(slope_row.value.integer == 37);
        Sculptor::PropertyValue slope_edit = {};
        slope_edit.integer                 = 64;
        graph.set_slot_value(mapping.osc_nodes[0], slope_slot, slope_edit);
        Sculptor::GraphChange slope_change = {};
        slope_change.kind                  = Sculptor::ChangeKind::value_changed;
        slope_change.node_idx              = mapping.osc_nodes[0];
        slope_change.slot_idx              = slope_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, slope_change));
        TEST(source.bank.instruments.entries[0].layers[0].fir_slope_percent == 64);
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].fir_slope_percent == 64);
        // A stale slot value outside 1..100 refuses the apply and the
        // compile, mirroring the octave_offset guard.
        slope_edit.integer = 0;
        graph.set_slot_value(mapping.osc_nodes[0], slope_slot, slope_edit);
        TEST(! Sculptor::apply_osc_graph_change(&source, &graph, &mapping, slope_change));
        TEST(! Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        slope_edit.integer = 101;
        graph.set_slot_value(mapping.osc_nodes[0], slope_slot, slope_edit);
        TEST(! Sculptor::apply_osc_graph_change(&source, &graph, &mapping, slope_change));
        TEST(! Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
    }

    // A type switch from low-pass to band-pass caps the carried cutoff at
    // the spread maximum, so the shared value never escapes the new range.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};
        build_zone_fixture(&source, &instrument);
        source.bank.instruments.entries[0].layers[0].fir_type                                  = Synth::fir_lowpass;
        source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].base_value = 440.0f;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        uint32_t type_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.osc_nodes[0], "Filter Type", &type_slot));
        Sculptor::PropertyValue type_edit = {};
        type_edit.list_index =
            1; // bandpass, at dropdown position 1 // bandpass // bandpass, after the leading "off" entry
        graph.set_slot_value(mapping.osc_nodes[0], type_slot, type_edit);
        Sculptor::GraphChange type_change = {};
        type_change.kind                  = Sculptor::ChangeKind::value_changed;
        type_change.node_idx              = mapping.osc_nodes[0];
        type_change.slot_idx              = type_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, type_change));
        TEST(source.bank.instruments.entries[0].layers[0].targets[Synth::mod_fir_spread].base_value == 36.0f);
        const std::unique_ptr<Sculptor::Graph> regraph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       regraph         = *regraph_storage;
        Sculptor::OscGraphMapping              remapping       = {};
        TEST(Sculptor::project_editor_to_graph(source, &regraph, &remapping));
        const uint32_t spread_row = Sculptor::osc_target_row(Synth::mod_fir_spread);
        TEST(regraph.node(remapping.osc_nodes[0]).slots.entries[spread_row].value.real == 36.0f);
    }

    // With the filter off, the cutoff row, its block and the slope gray out.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        build_zone_fixture(&source, &instrument);
        source.bank.instruments.entries[0].layers[0].fir_type = Synth::fir_off;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        const Sculptor::Node& node          = graph.node(mapping.osc_nodes[0]);
        const uint32_t        spread        = Sculptor::osc_target_row(Synth::mod_fir_spread);
        const uint32_t        fir_type_row  = spread - 1;  // the Filter Type list sits before the cutoff row
        const uint32_t        fir_slope_row = spread + 13; // the cutoff row plus its 12-slot block
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(node, spread));
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(node, spread + 5));
        TEST(Sculptor::oscillator_slot_waveform_mode_disabled(node, fir_slope_row));
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(node, fir_type_row));
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(node, Sculptor::osc_target_row(Synth::mod_volume)));
        source.bank.instruments.entries[0].layers[0].fir_type         = Synth::fir_bandpass;
        const std::unique_ptr<Sculptor::Graph> bandpass_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       bandpass_graph         = *bandpass_graph_storage;
        Sculptor::OscGraphMapping              bandpass_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(source, &bandpass_graph, &bandpass_mapping));
        const Sculptor::Node& bandpass_node = bandpass_graph.node(bandpass_mapping.osc_nodes[0]);
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(bandpass_node,
                                                                Sculptor::osc_target_row(Synth::mod_fir_spread)));
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(bandpass_node, fir_slope_row));
    }

    // The cutoff row label follows the selected filter type.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        build_zone_fixture(&source, &instrument);
        const uint32_t           spread                            = Sculptor::osc_target_row(Synth::mod_fir_spread);
        static const char* const expected[Synth::num_filter_types] = { "Filter Off",
                                                                       "Spread",
                                                                       "Cutoff (Hz)",
                                                                       "Cutoff (Hz)" };
        for (uint32_t type = 0; type < Synth::num_filter_types; type++) {
            source.bank.instruments.entries[0].layers[0].fir_type = static_cast<Synth::FilterType>(type);
            TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
            const Sculptor::Slot& row = graph.node(mapping.osc_nodes[0]).slots.entries[spread];
            TEST(strncmp(row.name, expected[type], sizeof(row.name)) == 0);
        }
    }

    // The FIR effect node: 33 slots (In/Out/Enabled + 3 params x 10), the
    // slope row is a bounded integer slider whose commit never writes the
    // zero-cutoff "off" state, which stays reserved for the two cutoff
    // rows at their slider extremes.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> authored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& authored = *authored_storage;
        Synth::init_default_bank(&authored.bank);
        Sculptor::set_identity_osc_sources(&authored, 0, 0);
        enabled_effect(authored.bank, 0, 0, Synth::EffectType::fir);
        authored.bank.channel_chains[0].effects[0].bindings[0].base_value = 8000.0f;
        authored.bank.channel_chains[0].effects[0].bindings[1].base_value = 0.0f;
        authored.bank.channel_chains[0].effects[0].bindings[2].base_value = 37.0f;
        TEST(Synth::validate_instrument_bank(&authored.bank));
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(Sculptor::project_effect_chain_to_graph(authored, 0, &graph, &mapping));
        TEST(graph.node(mapping.effect_nodes[0]).slots.num_allocated == 3 + 3 * Sculptor::fx_param_stride);
        uint32_t lowpass_row   = Sculptor::pool_no_slot;
        uint32_t highpass_row  = Sculptor::pool_no_slot;
        uint32_t slope_row_idx = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.effect_nodes[0], "Lowpass Hz", &lowpass_row));
        TEST(find_graph_slot(graph, mapping.effect_nodes[0], "Highpass Hz", &highpass_row));
        TEST(find_graph_slot(graph, mapping.effect_nodes[0], "FIR Slope", &slope_row_idx));
        TEST(lowpass_row == Sculptor::fx_param_row(0));
        TEST(highpass_row == Sculptor::fx_param_row(1));
        TEST(slope_row_idx == Sculptor::fx_param_row(2));
        const Sculptor::Slot& hp_row = graph.node(mapping.effect_nodes[0]).slots.entries[highpass_row];
        TEST(hp_row.value.real == 20.0f); // zero cutoff displays the minimum edge
        const Sculptor::Slot& slope_row2 = graph.node(mapping.effect_nodes[0]).slots.entries[slope_row_idx];
        TEST(slope_row2.property_type == Sculptor::PropertyType::integer);
        TEST(slope_row2.int_bounded);
        TEST(slope_row2.real_min == 1.0f && slope_row2.real_max == 100.0f);
        TEST(slope_row2.value.integer == 37);
        // The slope commit lands as an integer base; its slider extremes
        // are valid values, never the zero-cutoff off state.
        Sculptor::GraphChange change                             = {};
        change.kind                                              = Sculptor::ChangeKind::value_changed;
        change.node_idx                                          = mapping.effect_nodes[0];
        change.slot_idx                                          = slope_row_idx;
        Sculptor::PropertyValue                            value = {};
        const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>(authored);
        Synth::InstrumentEditorBank& candidate      = *candidate_storage;
        const int32_t                slope_values[] = { 55, 1, 100 };
        for (const int32_t slope : slope_values) {
            value.integer = slope;
            graph.set_slot_value(mapping.effect_nodes[0], slope_row_idx, value);
            candidate = authored;
            TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change));
            TEST(candidate.bank.channel_chains[0].effects[0].bindings[2].base_value == static_cast<float>(slope));
        }
        // The cutoff rows still write the zero "off" state at their extremes.
        change.slot_idx = lowpass_row;
        value.real      = 20.0f;
        graph.set_slot_value(mapping.effect_nodes[0], lowpass_row, value);
        candidate = authored;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change));
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 0.0f);
        change.slot_idx = highpass_row;
        value.real      = 22050.0f;
        graph.set_slot_value(mapping.effect_nodes[0], highpass_row, value);
        candidate = authored;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change));
        TEST(candidate.bank.channel_chains[0].effects[0].bindings[1].base_value == 0.0f);
    }
}

// Instrument Parameters node regressions: the dedicated node carries the
// note/layer detune and release rows, its edits drain through the
// value_changed apply path, and the envelope Ping-Pong toggle snaps the
// sustain span with the decode rule.
static void osc_instrument_node_regressions()
{
    // Slot projection: the three property rows live on the dedicated node
    // with values seeded from the instrument fields; MIDI Inputs contains
    // only source-output slots.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.instrument_node != Sculptor::pool_no_slot);
        uint32_t note_slot    = Sculptor::pool_no_slot;
        uint32_t layer_slot   = Sculptor::pool_no_slot;
        uint32_t release_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.instrument_node, "Random Note Detune", &note_slot));
        TEST(find_graph_slot(graph, mapping.instrument_node, "Random Oscillator Detune", &layer_slot));
        TEST(find_graph_slot(graph, mapping.instrument_node, "Release (s)", &release_slot));
        TEST(note_slot == mapping.instrument_slots[0]);
        TEST(layer_slot == mapping.instrument_slots[1]);
        TEST(release_slot == mapping.instrument_slots[2]);
        TEST(graph.node(mapping.instrument_node).slots.entries[note_slot].value.real == instrument.note_skew_semitones);
        TEST(graph.node(mapping.instrument_node).slots.entries[layer_slot].value.real ==
             instrument.layer_skew_semitones);
        TEST(graph.node(mapping.instrument_node).slots.entries[release_slot].value.real == instrument.release_seconds);
        TEST(! find_graph_slot(graph, mapping.input_node, "Random Note Detune", &note_slot));

        // Voices row: the fourth row sits on the same node with the three codec names, seeded
        // from voice_mode, and a stored mode survives a re-projection.
        uint32_t voices_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, mapping.instrument_node, "Voices", &voices_slot));
        TEST(voices_slot == mapping.instrument_slots[3]);
        TEST(! find_graph_slot(graph, mapping.input_node, "Voices", &voices_slot));
        const Sculptor::Slot& voices_row = graph.node(mapping.instrument_node).slots.entries[voices_slot];
        TEST(voices_row.kind == Sculptor::SlotKind::property);
        TEST(voices_row.num_list_options == 3);
        TEST(strcmp(voices_row.list_options[0], "multiple") == 0);
        TEST(strcmp(voices_row.list_options[1], "reuse per-note") == 0);
        TEST(strcmp(voices_row.list_options[2], "single") == 0);
        TEST(voices_row.value.list_index == instrument.voice_mode);
        TEST(voices_row.value.list_index == 0);
        for (uint32_t mode = 1; mode < 3; mode++) {
            source.bank.instruments.entries[0].voice_mode = static_cast<uint8_t>(mode);
            TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
            graph.take_changes(discarded, Sculptor::max_pending_changes);
            TEST(graph.node(mapping.instrument_node).slots.entries[mapping.instrument_slots[3]].value.list_index ==
                 mode);
        }
    }

    // Release drain: a slot write plus a real value_changed event moves the
    // bank field immediately (set_slot_value alone is eventless, so a
    // direct compile call here would bypass the node_role routing).  A
    // zero seed compiles back as zero.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(source.bank.instruments.entries[0].release_seconds == 0.0f);
        TEST(graph.node(mapping.instrument_node).slots.entries[mapping.instrument_slots[2]].value.real == 0.0f);

        Sculptor::PropertyValue release_edit = {};
        release_edit.real                    = 12.5f;
        graph.set_slot_value(mapping.instrument_node, mapping.instrument_slots[2], release_edit);
        Sculptor::GraphChange release_change = {};
        release_change.kind                  = Sculptor::ChangeKind::value_changed;
        release_change.node_idx              = mapping.instrument_node;
        release_change.slot_idx              = mapping.instrument_slots[2];
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, release_change));
        TEST(source.bank.instruments.entries[0].release_seconds == 12.5f);

        // Voices drain: each list edit moves the bank field through the same apply path.
        TEST(source.bank.instruments.entries[0].voice_mode == 0);
        const uint32_t edits[3] = { 2, 1, 0 };
        for (uint32_t edit_mode : edits) {
            Sculptor::PropertyValue voices_edit = {};
            voices_edit.list_index              = static_cast<uint8_t>(edit_mode);
            graph.set_slot_value(mapping.instrument_node, mapping.instrument_slots[3], voices_edit);
            Sculptor::GraphChange voices_change = {};
            voices_change.kind                  = Sculptor::ChangeKind::value_changed;
            voices_change.node_idx              = mapping.instrument_node;
            voices_change.slot_idx              = mapping.instrument_slots[3];
            TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, voices_change));
            TEST(source.bank.instruments.entries[0].voice_mode == edit_mode);
            TEST(source.bank.instruments.entries[0].release_seconds == 12.5f);
        }
    }

    // Sentinel fallback: a mapping the projection did not build (no
    // instrument node) compiles with the four fields at the
    // Instrument{} zero-init instead of failing or reading wild slots.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        mapping.instrument_node = Sculptor::pool_no_slot;
        for (uint32_t i = 0; i < 4; ++i) {
            mapping.instrument_slots[i] = Sculptor::pool_no_slot;
        }
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.note_skew_semitones == 0.0f);
        TEST(compiled.layer_skew_semitones == 0.0f);
        TEST(compiled.release_seconds == 0.0f);
        TEST(compiled.voice_mode == 0);
    }

    // Ping-pong toggle snap: toggling a multi-point span off snaps the live
    // span end to the span start immediately (no reload); toggling on moves
    // no points; a single-point span toggles off without a point write.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&           source        = *source_storage;
        Synth::Instrument                      instrument    = {};
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

        build_zone_fixture(&source, &instrument);
        // The volume envelope's descriptor gets a multi-point sustain span
        // with unequal endpoints and ping-pong on.
        source.bank.envelopes.entries[1].flags               = Synth::envelope_ping_pong;
        source.bank.envelopes.entries[1].sustain_first_point = 1;
        source.bank.envelopes.entries[1].sustain_last_point  = 2;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        int32_t env_instance = find_detached_by_desc(mapping, 1, 2);
        TEST(env_instance >= 0);
        uint32_t env_node = mapping.detached[env_instance].node_idx;
        TEST(env_node != Sculptor::pool_no_slot);
        TEST(env_node != Sculptor::pool_no_slot);
        uint32_t ping_pong_slot = Sculptor::pool_no_slot;
        TEST(find_graph_slot(graph, env_node, "Ping-Pong", &ping_pong_slot));
        TEST(graph.node(env_node).slots.entries[ping_pong_slot].value.toggle == true);
        TEST(source.bank.envelopes.entries[1].points[2].value != source.bank.envelopes.entries[1].points[1].value);

        // Toggle off: the span end snaps to the span start immediately.
        Sculptor::PropertyValue toggle_edit = {};
        toggle_edit.toggle                  = false;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        Sculptor::GraphChange toggle_change = {};
        toggle_change.kind                  = Sculptor::ChangeKind::value_changed;
        toggle_change.node_idx              = env_node;
        toggle_change.slot_idx              = ping_pong_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) == 0);
        TEST(source.bank.envelopes.entries[1].points[2].value == source.bank.envelopes.entries[1].points[1].value);

        // Toggle on with unequal endpoints: no point moves.
        source.bank.envelopes.entries[1].flags           = 0;
        source.bank.envelopes.entries[1].points[2].value = 0xC000;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        env_instance = find_detached_by_desc(mapping, 1, 2);
        TEST(env_instance >= 0);
        env_node = mapping.detached[env_instance].node_idx;
        TEST(find_graph_slot(graph, env_node, "Ping-Pong", &ping_pong_slot));
        toggle_edit.toggle = true;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        toggle_change.node_idx = env_node;
        toggle_change.slot_idx = ping_pong_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) != 0);
        TEST(source.bank.envelopes.entries[1].points[2].value == 0xC000);
        TEST(source.bank.envelopes.entries[1].points[2].value != source.bank.envelopes.entries[1].points[1].value);

        // Single-point span: toggling off flips the flag and writes no point.
        source.bank.envelopes.entries[1].flags               = Synth::envelope_ping_pong;
        source.bank.envelopes.entries[1].sustain_first_point = 1;
        source.bank.envelopes.entries[1].sustain_last_point  = 1;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        env_instance = find_detached_by_desc(mapping, 1, 2);
        TEST(env_instance >= 0);
        env_node = mapping.detached[env_instance].node_idx;
        TEST(find_graph_slot(graph, env_node, "Ping-Pong", &ping_pong_slot));
        toggle_edit.toggle = false;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        toggle_change.node_idx = env_node;
        toggle_change.slot_idx = ping_pong_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) == 0);
        TEST(source.bank.envelopes.entries[1].points[1].value == 0x8000);
        TEST(source.bank.envelopes.entries[1].points[2].value == 0xC000);

        // Already-equal span endpoints: toggling off flips the flag and
        // writes no point.
        source.bank.envelopes.entries[1].flags              = Synth::envelope_ping_pong;
        source.bank.envelopes.entries[1].sustain_last_point = 2;
        source.bank.envelopes.entries[1].points[2].value    = source.bank.envelopes.entries[1].points[1].value;
        TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        env_instance = find_detached_by_desc(mapping, 1, 2);
        TEST(env_instance >= 0);
        env_node = mapping.detached[env_instance].node_idx;
        TEST(find_graph_slot(graph, env_node, "Ping-Pong", &ping_pong_slot));
        toggle_edit.toggle = false;
        graph.set_slot_value(env_node, ping_pong_slot, toggle_edit);
        toggle_change.node_idx = env_node;
        toggle_change.slot_idx = ping_pong_slot;
        TEST(Sculptor::apply_osc_graph_change(&source, &graph, &mapping, toggle_change));
        TEST((source.bank.envelopes.entries[1].flags & Synth::envelope_ping_pong) == 0);
        TEST(source.bank.envelopes.entries[1].points[2].value == source.bank.envelopes.entries[1].points[1].value);
    }
}

// Instrument Parameters node position persistence: the node has no
// canonical slot, so its kind-5 layout record keys the zone alone; the
// decode accepts it with the kind-0 sanity rules, the projection applies
// the saved position, and a bank without the record keeps the default.
static void osc_instrument_node_layout_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::OscGraphMapping                          mapping        = {};
    Sculptor::GraphChange                              discarded[Sculptor::max_pending_changes] = {};

    build_zone_fixture(&source, &instrument);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const vmath::vec2 default_position = graph.node(mapping.instrument_node).position;

    // A saved kind-5 record re-applies the moved position and size
    // overrides on re-projection.
    Synth::GraphNodeLayout record                    = {};
    record.channel                                   = 0;
    record.zone                                      = 0;
    record.kind                                      = 5;
    record.x                                         = 304.0f;
    record.y                                         = 192.0f;
    record.width_override                            = 96.0f;
    source.graph_layout[source.graph_layout_count++] = record;
    TEST(Sculptor::validate_editor_metadata(source));

    const std::unique_ptr<Sculptor::Graph> moved_graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       moved_graph         = *moved_graph_storage;
    Sculptor::OscGraphMapping              moved_mapping       = {};
    TEST(Sculptor::project_editor_to_graph(source, &moved_graph, &moved_mapping));
    TEST(moved_graph.node(moved_mapping.instrument_node).position.x == 304.0f);
    TEST(moved_graph.node(moved_mapping.instrument_node).position.y == 192.0f);
    TEST(moved_graph.node(moved_mapping.instrument_node).content_width_override == 96.0f);

    // The record round-trips through the editor JSON section unchanged.
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& decoded = *decoded_storage;
    char                         doc[16 * 1024];
    const uint32_t               written = Synth::encode_editor_bank_json(&source, doc, sizeof(doc));
    TEST(written > 0);
    TEST(Synth::decode_editor_bank_json(doc, written, &decoded));
    TEST(Sculptor::validate_editor_metadata(decoded));
    const std::unique_ptr<Sculptor::Graph> regraph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       regraph         = *regraph_storage;
    Sculptor::OscGraphMapping              remapping       = {};
    TEST(Sculptor::project_editor_to_graph(decoded, &regraph, &remapping));
    TEST(regraph.node(remapping.instrument_node).position.x == 304.0f);
    TEST(regraph.node(remapping.instrument_node).position.y == 192.0f);

    // The kind-5 record is bank-only metadata: the portable instrument
    // document must still encode (the exporter refuses unrepresented zone
    // records) and must not carry the record.
    const Synth::InstrumentJsonDomain domains[] = { Synth::InstrumentJsonDomain::clipboard,
                                                    Synth::InstrumentJsonDomain::validated_bank };
    for (Synth::InstrumentJsonDomain domain : domains) {
        const std::unique_ptr<FixedArray<char, clipboard_capacity>> text_storage =
            std::make_unique<FixedArray<char, clipboard_capacity>>();
        char (&text)[clipboard_capacity] = text_storage->elements;
        TEST(Sculptor::encode_editor_instrument_json(source, 0, 0, text, sizeof(text), domain) != 0);
        TEST(! strstr(text, "\"kind\":5"));
    }

    // Legacy record set: without the kind-5 record the node keeps its
    // deterministic default position.
    source.graph_layout_count                                   = 0;
    const std::unique_ptr<Sculptor::Graph> legacy_graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       legacy_graph         = *legacy_graph_storage;
    Sculptor::OscGraphMapping              legacy_mapping       = {};
    TEST(Sculptor::project_editor_to_graph(source, &legacy_graph, &legacy_mapping));
    TEST(legacy_graph.node(legacy_mapping.instrument_node).position.x == default_position.x);
    TEST(legacy_graph.node(legacy_mapping.instrument_node).position.y == default_position.y);

    // Kind-5 sanity: a nonzero uid or index refuses in the validate pass,
    // and both the decode and the validate pass still refuse kind 6.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bad_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bad = *bad_storage;
        build_zone_fixture(&bad, &instrument);
        Synth::GraphNodeLayout malformed           = {};
        malformed.channel                          = 0;
        malformed.zone                             = 0;
        malformed.kind                             = 5;
        malformed.uid                              = 1;
        malformed.x                                = 1.0f;
        malformed.y                                = 2.0f;
        bad.graph_layout[bad.graph_layout_count++] = malformed;
        TEST(! Sculptor::validate_editor_metadata(bad));

        const std::unique_ptr<Synth::InstrumentEditorBank> indexed_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& indexed = *indexed_storage;
        build_zone_fixture(&indexed, &instrument);
        Synth::GraphNodeLayout with_index                  = malformed;
        with_index.uid                                     = 0;
        with_index.index                                   = 3;
        indexed.graph_layout[indexed.graph_layout_count++] = with_index;
        TEST(! Sculptor::validate_editor_metadata(indexed));

        const std::unique_ptr<Synth::InstrumentEditorBank> kind6_bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& kind6_bank = *kind6_bank_storage;
        build_zone_fixture(&kind6_bank, &instrument);
        Synth::GraphNodeLayout kind6                             = malformed;
        kind6.uid                                                = 0;
        kind6.index                                              = 0;
        kind6.kind                                               = 6;
        kind6_bank.graph_layout[kind6_bank.graph_layout_count++] = kind6;
        TEST(! Sculptor::validate_editor_metadata(kind6_bank));

        char      overkind[256];
        const int printed = snprintf(overkind,
                                     sizeof(overkind),
                                     "{\"layouts\":[{\"channel\":0,\"zone\":0,\"kind\":6,"
                                     "\"x\":1.0,\"y\":2.0,\"width\":0.0,\"height\":0.0}]}");
        TEST(printed > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> refused_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& refused = *refused_storage;
        TEST(! decode_bank_with_editor_section(source, overkind, &refused));

        // A kind-5 record with a nonzero uid refuses in the decode too.
        char      baduid[256];
        const int uid_printed = snprintf(baduid,
                                         sizeof(baduid),
                                         "{\"layouts\":[{\"channel\":0,\"zone\":0,\"kind\":5,\"uid\":2,"
                                         "\"x\":1.0,\"y\":2.0,\"width\":0.0,\"height\":0.0}]}");
        TEST(uid_printed > 0);
        TEST(! decode_bank_with_editor_section(source, baduid, &refused));

        // A kind-5 record with a nonzero index refuses in the decode too.
        char      badindex[256];
        const int index_printed = snprintf(badindex,
                                           sizeof(badindex),
                                           "{\"layouts\":[{\"channel\":0,\"zone\":0,\"kind\":5,\"index\":3,"
                                           "\"x\":1.0,\"y\":2.0,\"width\":0.0,\"height\":0.0}]}");
        TEST(index_printed > 0);
        TEST(! decode_bank_with_editor_section(source, badindex, &refused));

        // A plain kind-5 record decodes and applies through the editor JSON.
        char      good[256];
        const int good_printed = snprintf(good,
                                          sizeof(good),
                                          "{\"layouts\":[{\"channel\":0,\"zone\":0,\"kind\":5,"
                                          "\"x\":304.0,\"y\":192.0,\"width\":0.0,\"height\":0.0}]}");
        TEST(good_printed > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> applied_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& applied = *applied_storage;
        TEST(decode_bank_with_editor_section(source, good, &applied));
        const std::unique_ptr<Sculptor::Graph> applied_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       applied_graph         = *applied_graph_storage;
        Sculptor::OscGraphMapping              applied_mapping       = {};
        TEST(Sculptor::project_editor_to_graph(applied, &applied_graph, &applied_mapping));
        TEST(applied_graph.node(applied_mapping.instrument_node).position.x == 304.0f);
        TEST(applied_graph.node(applied_mapping.instrument_node).position.y == 192.0f);
    }
}

// A stamped oscillator node keeps the existing disable rules unchanged:
// the stamp only decides who the rules apply to, not the rules.
static void osc_slot_disable_stamped_rules_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::OscGraphMapping                          mapping        = {};
    build_zone_fixture(&source, &instrument);
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));

    const uint32_t osc_node = mapping.osc_nodes[0];
    TEST(osc_node != Sculptor::pool_no_slot);
    const Sculptor::Node& osc = graph.node(osc_node);
    // The projection stamps oscillator layer nodes and nothing else.
    TEST(osc.oscillator);
    TEST(! graph.node(mapping.input_node).oscillator);
    TEST(! graph.node(mapping.instrument_node).oscillator);
    TEST(! graph.node(mapping.output_node).oscillator);

    const uint32_t duty1       = Sculptor::osc_target_row(Synth::mod_duty1);
    const uint32_t mix         = Sculptor::osc_target_row(Synth::mod_osc_mix);
    const uint32_t fm          = Sculptor::osc_target_row(Synth::mod_fm_index);
    const uint32_t fm_ratio    = Sculptor::osc_target_row(Synth::mod_fm_ratio);
    const uint32_t tone0       = Sculptor::osc_target_row(Synth::mod_pad_tilt0);
    const uint32_t tone1       = Sculptor::osc_target_row(Synth::mod_pad_tilt1);
    const uint32_t volume      = Sculptor::osc_target_row(Synth::mod_volume);
    const uint32_t wave_a_slot = 1;  // waveform a, layout contract
    const uint32_t wave_b_slot = 42; // waveform b, layout contract
    const uint32_t mode_slot   = 69; // mix mode, layout contract

    Sculptor::PropertyValue wave = {};
    // Sine wave a (list 0; the wave a list starts at sine), waveform b
    // off (the wave b list starts at off): the pad-tone windows, the whole
    // duty a family (sine ignores duty) and every wave-b-dependent row are
    // disabled.
    wave.list_index = 0;
    graph.set_slot_value(osc_node, wave_b_slot, wave);
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, duty1));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, mode_slot));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, mix));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm_ratio));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone0));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone0 + Sculptor::osc_target_block_stride));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone1));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone1 + Sculptor::osc_target_block_stride));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, 2)); // duty a row
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, volume));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, Sculptor::osc_target_row(Synth::mod_pitch)));

    // Pad on both waveforms re-enables both tone windows and the
    // wave-b-dependent rows; the fixture's blend mode still gates fm
    // depth and fm ratio.
    wave.list_index = 4; // pad (the wave a list has no off entry)
    graph.set_slot_value(osc_node, wave_a_slot, wave);
    wave.list_index = 5; // pad
    graph.set_slot_value(osc_node, wave_b_slot, wave);
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone0));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone0 + Sculptor::osc_target_block_stride));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone1));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, tone1 + Sculptor::osc_target_block_stride));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, duty1));
    TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(osc, mix));
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm)); // blend mode
    TEST(Sculptor::oscillator_slot_waveform_mode_disabled(osc, fm_ratio));
}

// Stamp isolation: clear() resets the retained pool storage, so a node
// recreated after clear() is value-initialized and carries no stamp.
static void osc_stamp_pool_clear_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::OscGraphMapping                          mapping        = {};
    build_zone_fixture(&source, &instrument);
    TEST(Sculptor::project_editor_to_graph(source, &graph, &mapping));
    TEST(graph.node(mapping.osc_nodes[0]).oscillator);
    const uint32_t stamped_slot = mapping.osc_nodes[0];
    graph.clear();
    // Pool allocation is first-free order, so allocating past the old
    // oscillator's slot index covers its recycled slot.
    for (uint32_t i = 0; i <= stamped_slot; ++i) {
        const uint32_t fresh = graph.create_node("Fresh", vmath::vec2(0.0f, 0.0f));
        TEST(fresh != Sculptor::pool_no_slot);
        TEST(! graph.node(fresh).oscillator);
    }
}

static void check_osc_inline_0()
{
    // Max-complexity round-trip: a fully populated instrument projects to the
    // full graph (3 fixed nodes + 7 oscillators + 28 envelope and 28 LFO
    // instances = 66 nodes, 175 edges) and compiles back
    // bit-identically, with the descriptor pools untouched (no dedup or
    // duplication) and the aliasing pattern preserved.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage       = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank               = *bank_storage;
        const std::unique_ptr<Synth::InstrumentBank> bank_image_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank_image         = *bank_image_storage;
        Synth::Instrument                            instrument         = {};
        uint16_t                                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&bank, &instrument, env_ids, lfo_ids);
        bank_image                                           = bank;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 28, 28, 4);
        TEST(graph.connection_count() == 175);

        // Re-projection onto a populated graph resets it deterministically.
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, Synth::max_layers, 28, 28, 4);
        TEST(graph.connection_count() == 175);

        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);

        // The aliased LFO descriptor serves two cells through two distinct
        // instances (their depth/rate sources differ); both compiled cells
        // carry the shared desc id.
        uint32_t alias_instances = 0;
        for (uint32_t i = 0; i < mapping.detached_count; ++i) {
            if (mapping.detached[i].kind == 2 && mapping.detached[i].desc_id == lfo_ids[1][Synth::mod_pitch]) {
                ++alias_instances;
            }
        }
        TEST(alias_instances == 2);
    }

    // Minimal round-trip: one unbound oscillator layer projects to the fixed
    // nodes plus one oscillator (4 nodes, one hard connection) and compiles
    // back bit-identically.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        instrument.layer_count                                    = 1;
        set_default_dispersion_sections(instrument);
        instrument.layers[0].osc_type[0]                     = Synth::WaveType::sine_wave;
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        check_osc_graph_mapping(graph, mapping, 1, 0, 0);
        TEST(graph.connection_count() == 1);
        Synth::Instrument compiled = {};
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(memcmp(&instrument, &compiled, sizeof(Synth::Instrument)) == 0);
        // Waveform A has no off option: the list starts at sine and compile
        // adds one back onto the wave type.
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].num_list_options == 6);
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].value.list_index == 0);
        // The dropdown spells the wave out; the bank JSON token (sculptor_bank_json's
        // own wave_names array, already "sawtooth") never shared this display string.
        TEST(strncmp(graph.node(mapping.osc_nodes[0]).slots.entries[1].list_options[1],
                     "sawtooth",
                     sizeof(graph.node(mapping.osc_nodes[0]).slots.entries[1].list_options[0])) == 0);
        TEST(strncmp(graph.node(mapping.osc_nodes[0]).slots.entries[42].list_options[2],
                     "sawtooth",
                     sizeof(graph.node(mapping.osc_nodes[0]).slots.entries[42].list_options[0])) == 0);
        Sculptor::PropertyValue saw;
        saw.list_index = 1;
        graph.set_slot_value(mapping.osc_nodes[0], 1, saw); // "sawtooth"
        TEST(graph.node(mapping.osc_nodes[0]).slots.entries[1].value.list_index == 1);
        TEST(Sculptor::compile_graph_to_instrument(graph, mapping, &compiled));
        TEST(compiled.layers[0].osc_type[0] == Synth::WaveType::sawtooth_wave);
    }

    // Projection refuses instruments it cannot express: a routing input with
    // the none source would change synthesis on commit if silently dropped
    // (the eval multiplies by the zero sentinel), so the refusal is visible
    // and leaves the graph, mapping and bank untouched.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage       = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank               = *bank_storage;
        const std::unique_ptr<Synth::InstrumentBank> bank_image_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank_image         = *bank_image_storage;
        Synth::Instrument                            good               = {};
        good.layer_count                                                = 1;
        set_default_dispersion_sections(good);
        good.layers[0].targets[Synth::mod_volume].num_inputs = 1;
        good.layers[0].targets[Synth::mod_volume].inputs[0]  = { Synth::ModSource::velocity,
                                                                 Synth::SourceOp::multiply,
                                                                 0,
                                                                 0.5f };

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(good, bank, &graph, &mapping));
        TEST(mapping.output_node != Sculptor::pool_no_slot);

        Synth::Instrument bad                                     = good;
        bad.layers[0].targets[Synth::mod_volume].inputs[0].source = Synth::ModSource::none;
        bank_image                                                = bank;
        const uint32_t connections_before                         = graph.connection_count();
        const uint32_t output_before                              = mapping.output_node;

        TEST(! Sculptor::project_instrument_to_graph(bad, bank, &graph, &mapping));
        TEST(graph.has_error());
        TEST(graph.connection_count() == connections_before);
        TEST(mapping.output_node == output_before);
        TEST(memcmp(&bank_image, &bank, sizeof(Synth::InstrumentBank)) == 0);
    }

    // Reclamation remaps kind-1/2 records' descriptor ids together
    // with the pool compaction, so detached nodes keep editing their content.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        bank.bank.channel_enabled[0] = 1;
        bank.bank.instruments.allocate();
        Synth::Instrument& instrument = bank.bank.instruments.entries[0];
        instrument.layer_count        = 1;
        set_default_dispersion_sections(instrument);
        instrument.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
        // Envelope id 1 is bound to the instrument; id 2 is referenced only by
        // a detached record.
        const uint32_t env_a = bank.bank.envelopes.allocate();
        const uint32_t env_b = bank.bank.envelopes.allocate();
        TEST(env_a == 0 && env_b == 1);
        bank.bank.envelopes.entries[env_a].num_points                    = 2;
        bank.bank.envelopes.entries[env_a].min_value                     = -0.5f;
        bank.bank.envelopes.entries[env_a].min_max_delta                 = 1.0f;
        bank.bank.envelopes.entries[env_b]                               = bank.bank.envelopes.entries[env_a];
        bank.bank.envelopes.entries[env_b].min_value                     = -0.75f;
        instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = 1;
        bank.bank.channel_zones[0][0].start_note                         = 1;
        bank.bank.channel_zones[0][0].instrument                         = 0;
        add_detached_record(&bank, 0, 0, 1, 2, 5.0f, 6.0f, Synth::ModSource::none, Synth::ModSource::none, 1);
        TEST(Synth::validate_instrument_bank(&bank.bank));

        // Deleting the bound envelope leaves id 2 referenced only by the
        // detached record; reclamation keeps it (kept-id) and compacts it
        // into id 1, and the record's index must follow.
        bank.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].envelope_desc_id = 0;
        Synth::reclaim_unused_slots(&bank);
        TEST(bank.bank.envelopes.is_occupied(0));
        TEST(bank.bank.envelopes.entries[0].min_value == -0.75f);
        TEST(bank.graph_layout_count == 1);
        TEST(bank.graph_layout[0].kind == 1);
        TEST(bank.graph_layout[0].index == 1);
    }

    // Projection flags drop-menu anchors: exactly the nine value rows on
    // every projected oscillator node carry the flag; the fixed property
    // rows (waveforms, mode, FM ratio, pitch offset), the output dot, the
    // block fields, the MIDI inputs and the generator nodes do not.
    {
        const std::unique_ptr<Synth::InstrumentBank> model_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       model_bank         = *model_bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        bank.bank                    = model_bank;
        bank.bank.channel_enabled[0] = 1;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));

        const uint32_t osc_slots =
            Sculptor::osc_band_first_slot + Sculptor::osc_band_block_stride * instrument.layers[0].fir_band_count;
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            const Sculptor::Node& osc = graph.node(mapping.osc_nodes[layer]);
            TEST(osc.slots.num_allocated == osc_slots);
            for (uint32_t s = 0; s < osc_slots; ++s) {
                bool is_value_row = false;
                for (uint32_t t = 0; t < Synth::num_mod_targets; ++t) {
                    is_value_row = is_value_row || s == Sculptor::osc_target_row_slot[t];
                }
                TEST(osc.slots.entries[s].drop_menu == is_value_row);
            }
        }
        // The MIDI inputs node and every generator node: nothing flagged.
        for (uint32_t n = 0; n < Sculptor::max_nodes; ++n) {
            if (! graph.node_occupied(n)) {
                continue;
            }
            bool is_osc = false;
            for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
                is_osc = is_osc || n == mapping.osc_nodes[layer];
            }
            if (is_osc) {
                continue;
            }
            const Sculptor::Node& node = graph.node(n);
            for (uint32_t s = 0; s < node.slots.num_allocated; ++s) {
                TEST(! node.slots.entries[s].drop_menu);
                TEST(! node.slots.entries[s].drop_menu);
            }
        }

        // At seven layers no layer row is flagged: there is no free layer
        // to add an oscillator to, and all seven slots stay allocated.
        TEST(graph.node(mapping.output_node).slots.num_allocated == Synth::max_layers);
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            TEST(! graph.node(mapping.output_node).slots.entries[mapping.output_layer_input_slot[layer]].drop_menu);
        }
    }

    // The run length is the zone's oscillator node count
    // including shells, and it can never exceed max_layers.  The truth table
    // pins the cap arithmetic the menu's disabled state and the add
    // consumer's refusal share: at the cap the Add Oscillator entry is
    // disabled on every dot, so the attempt must be a no-op.
    // Layer visibility truth table: osc_layer_slot_hidden hides exactly
    // the layer rows above the instrument's layer count (with no red
    // rows), and the output node keeps all seven layer slots allocated
    // (rows hide, nothing is removed).
    for (uint32_t layer_count = 0; layer_count <= Synth::max_layers; ++layer_count) {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        instrument             = {};
        instrument.layer_count = layer_count;
        set_default_dispersion_sections(instrument);
        for (uint32_t layer = 0; layer < layer_count; ++layer) {
            instrument.layers[layer].osc_type[0] = Synth::WaveType::sine_wave;
        }
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        uint8_t source_row[Synth::max_layers] = {};
        for (uint32_t s = 0; s < layer_count; ++s) {
            source_row[s] = static_cast<uint8_t>(s + 1);
        }
        for (uint32_t layer = 0; layer < Synth::max_layers; ++layer) {
            const uint32_t slot = mapping.output_layer_input_slot[layer];
            TEST(slot != Sculptor::pool_no_slot);
            TEST(Sculptor::osc_layer_slot_hidden(slot, source_row) == (layer > layer_count));
        }
        // A non-layer slot index and the no-slot sentinel never hide.
        TEST(! Sculptor::osc_layer_slot_hidden(Synth::max_layers, source_row));
        TEST(graph.node(mapping.output_node).slots.num_allocated == Synth::max_layers);
    }

    // Drop-commit preset: a fresh binding on mod_volume whose descriptor
    // carries the drop-shaped default (the bipolar default converted before
    // commit: minimum 0 with the effective top preserved, point values
    // authored) passes env_volume_shape_ok, derives the family wire and
    // keeps the family row visible; the same bipolar default on a
    // non-volume target keeps the bipolar shape and still derives its wire.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::Instrument                            instrument   = {};
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        // The canvas command's bipolar default: three points, sustain over
        // the middle, -1..+1 span, attack to full and a release tail to
        // neutral.
        Synth::EnvelopeDescriptor bipolar = {};
        bipolar.num_points                = 3;
        bipolar.sustain_first_point       = 1;
        bipolar.sustain_last_point        = 1;
        bipolar.min_value                 = -1.0f;
        bipolar.min_max_delta             = 2.0f / 65535.0f;
        bipolar.points[0]                 = { 0, 0x0000 };
        bipolar.points[1]                 = { 5, 0xFFFF };
        bipolar.points[2]                 = { 91, 0x8000 };

        // Volume case: the drop variant converts before commit, through the
        // model's own conversion.
        const uint32_t env_slot = bank.envelopes.allocate();
        TEST(env_slot == 0);
        bank.envelopes.entries[env_slot] = bipolar;
        // Author a non-zero onset: the conversion must leave it alone.
        bank.envelopes.entries[env_slot].points[0].value = 0x4000;
        const float top_before                           = bipolar.min_value + 65535.0f * bipolar.min_max_delta;
        TEST(Sculptor::env_convert_to_volume_shape(bank.envelopes.entries[env_slot]));
        // The effective top is preserved: the converted floor sits at 0 and
        // the delta carries the whole span.
        const Synth::EnvelopeDescriptor& converted = bank.envelopes.entries[env_slot];
        TEST(converted.min_value == 0.0f);
        TEST(converted.min_value + 65535.0f * converted.min_max_delta == top_before);
        // The conversion pins only the range: the authored onset and the
        // release tail survive (the runtime ramps volume from silence at
        // note-on, so a non-zero first point is safe).
        TEST(converted.points[0].value == 0x4000);
        TEST(converted.points[2].value == 0x8000);
        TEST(Sculptor::env_volume_shape_ok(bank.envelopes.entries[env_slot]));

        instrument.layer_count = 1;
        set_default_dispersion_sections(instrument);
        instrument.layers[0].osc_type[0]                                 = Synth::WaveType::sine_wave;
        instrument.layers[0].targets[Synth::mod_volume].envelope_desc_id = static_cast<uint16_t>(env_slot + 1);
        const std::unique_ptr<Sculptor::Graph> graph_storage             = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph                     = *graph_storage;
        Sculptor::OscGraphMapping              mapping                   = {};
        TEST(Sculptor::project_instrument_to_graph(instrument, bank, &graph, &mapping));
        const uint32_t osc_node = mapping.osc_nodes[0];
        TEST(find_osc_graph_connection(graph, { osc_node, Sculptor::osc_target_env_dot(Synth::mod_volume) }) !=
             Sculptor::pool_no_slot);
        TEST(! Sculptor::osc_target_field_hidden(graph, osc_node, Synth::mod_volume, 0));
        // Projection is read-only on descriptors: the committed shape survives.
        TEST(Sculptor::env_volume_shape_ok(bank.envelopes.entries[env_slot]));

        // Sibling: the bipolar default on a non-volume target stays bipolar
        // (the volume shape cannot serve a non-volume target) and still
        // derives its wire and its visible row.
        const std::unique_ptr<Synth::InstrumentBank> pitch_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       pitch_bank         = *pitch_bank_storage;
        Synth::Instrument                            pitch_instrument   = {};
        memset(static_cast<void*>(&pitch_bank), 0, sizeof(pitch_bank));
        const uint32_t pitch_env_slot = pitch_bank.envelopes.allocate();
        TEST(pitch_env_slot == 0);
        pitch_bank.envelopes.entries[pitch_env_slot] = bipolar;
        TEST(! Sculptor::env_volume_shape_ok(pitch_bank.envelopes.entries[pitch_env_slot]));
        pitch_instrument.layer_count = 1;
        set_default_dispersion_sections(pitch_instrument);
        pitch_instrument.layers[0].osc_type[0] = Synth::WaveType::sine_wave;
        pitch_instrument.layers[0].targets[Synth::mod_pitch].envelope_desc_id =
            static_cast<uint16_t>(pitch_env_slot + 1);
        const std::unique_ptr<Sculptor::Graph> pitch_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       pitch_graph         = *pitch_graph_storage;
        Sculptor::OscGraphMapping              pitch_mapping       = {};
        TEST(Sculptor::project_instrument_to_graph(pitch_instrument, pitch_bank, &pitch_graph, &pitch_mapping));
        const uint32_t pitch_node = pitch_mapping.osc_nodes[0];
        TEST(find_osc_graph_connection(pitch_graph, { pitch_node, Sculptor::osc_target_env_dot(Synth::mod_pitch) }) !=
             Sculptor::pool_no_slot);
        TEST(! Sculptor::osc_target_field_hidden(pitch_graph, pitch_node, Synth::mod_pitch, 0));
    }

    // The volume shape constraint is exactly the nonnegative range: a
    // non-zero first point is accepted, a non-zero minimum and a negative
    // delta are refused, and the conversion refuses a negative effective
    // top without touching the descriptor.
    {
        Synth::EnvelopeDescriptor shaped = {};
        shaped.min_value                 = 0.0f;
        shaped.min_max_delta             = 1.0f / 65535.0f;
        shaped.points[0].value           = 0x4000;
        TEST(Sculptor::env_volume_shape_ok(shaped));
        shaped.min_value = 0.5f;
        TEST(! Sculptor::env_volume_shape_ok(shaped));
        shaped.min_value     = 0.0f;
        shaped.min_max_delta = -1.0f / 65535.0f;
        TEST(! Sculptor::env_volume_shape_ok(shaped));
        shaped.min_max_delta   = 1.0f / 65535.0f;
        shaped.min_value       = -2.0f;
        shaped.points[1].value = 0x8000;
        TEST(! Sculptor::env_convert_to_volume_shape(shaped));
        TEST(shaped.min_value == -2.0f);
        TEST(shaped.min_max_delta == 1.0f / 65535.0f);
        TEST(shaped.points[0].value == 0x4000);
        TEST(shaped.points[1].value == 0x8000);
    }

    // The add path installs fresh default parameters: no inheritance.  The
    // editor's append routine constructs a default oscillator (sine, B off)
    // and the inheritance helper is gone from the editor TU.

    // Every parameter row binds its own oscillator's cell, with and without
    // preceding shells: the modeled consumer resolves the drop row's
    // compiled layer as the number of connected rows strictly below it, so
    // the projection derives each row's family wire from that row's own
    // binding.  The drop-created LFO seeds an audible depth of 0.5 on a
    // zero-depth cell.
    {
        const std::unique_ptr<Synth::InstrumentBank> model_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       model_bank         = *model_bank_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank       = *bank_storage;
        Synth::Instrument            instrument = {};
        uint16_t                     env_ids[Synth::max_layers][Synth::num_mod_targets];
        uint16_t                     lfo_ids[Synth::max_layers][Synth::num_mod_targets];
        build_osc_graph_max_fixture(&model_bank, &instrument, env_ids, lfo_ids);
        bank.bank                    = model_bank;
        bank.bank.channel_enabled[0] = 1;
        TEST(bank.bank.instruments.allocate() == 0);
        bank.bank.instruments.entries[0]         = instrument;
        bank.bank.channel_zones[0][0].start_note = 1;
        bank.bank.channel_zones[0][0].instrument = 0;
        Sculptor::set_identity_osc_sources(&bank, 0, 0);
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::OscGraphMapping              mapping       = {};
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        // Without shells: one fresh LFO per row, bound through the modeled
        // consumer's resolved compiled layer.
        uint16_t drop_lfo[Synth::max_layers] = {};
        for (uint32_t row = 0; row < Synth::max_layers; ++row) {
            const uint32_t slot = bank.bank.lfos.allocate();
            TEST(slot != pool_no_slot);
            drop_lfo[row]              = static_cast<uint16_t>(slot + 1);
            Synth::TargetBinding& cell = bank.bank.instruments.entries[0].layers[row].targets[Synth::mod_pitch];
            cell.lfo_desc_id           = drop_lfo[row];
            cell.lfo_depth             = 0.0f; // the zero-depth cell the seed rule serves
            cell.lfo_depth             = 0.5f; // the modeled drop seed
        }
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        for (uint32_t row = 0; row < Synth::max_layers; ++row) {
            const uint32_t conn =
                find_osc_graph_connection(graph,
                                          { mapping.osc_nodes[row], Sculptor::osc_target_lfo_dot(Synth::mod_pitch) });
            TEST(conn != Sculptor::pool_no_slot);
            bool found = false;
            for (uint32_t d = 0; d < mapping.detached_count; ++d) {
                if (mapping.detached[d].node_idx == graph.get_connection(conn).output.node_idx) {
                    TEST(mapping.detached[d].desc_id == drop_lfo[row]);
                    found = true;
                }
            }
            TEST(found);
            TEST(graph.node(mapping.osc_nodes[row])
                     .slots.entries[Sculptor::osc_target_block_base(Synth::mod_pitch) + 4]
                     .value.real == 0.5f);
        }
        // A sum-wire delete only clears the row: the per-layer LFO dots
        // are layer-owned under the authoring model, so every layer keeps
        // its drop LFO and depth, and slot 2 becomes the free add slot.
        const uint32_t gone =
            find_osc_graph_connection(graph, { mapping.output_node, mapping.output_layer_input_slot[2] });
        TEST(gone != Sculptor::pool_no_slot);
        graph.delete_connection(gone);
        Sculptor::GraphChange discard[Sculptor::max_pending_changes];
        (void)graph.take_changes(discard, Sculptor::max_pending_changes);
        (void)graph.changes_overflowed();
        Sculptor::GraphChange cut = {};
        cut.kind                  = Sculptor::ChangeKind::connection_deleted;
        cut.connection_idx        = gone;
        cut.connection_output     = { mapping.osc_nodes[2], mapping.osc_output_slot };
        cut.connection_input      = { mapping.output_node, mapping.output_layer_input_slot[2] };
        TEST(Sculptor::apply_osc_graph_change(&bank, &graph, &mapping, cut));
        TEST(bank.graph_osc_source[0][0][2] == 0);
        TEST(Sculptor::project_editor_to_graph(bank, &graph, &mapping));
        for (uint32_t row = 0; row < Synth::max_layers; ++row) {
            const uint32_t conn =
                find_osc_graph_connection(graph,
                                          { mapping.osc_nodes[row], Sculptor::osc_target_lfo_dot(Synth::mod_pitch) });
            TEST(conn != Sculptor::pool_no_slot);
            bool found = false;
            for (uint32_t d = 0; d < mapping.detached_count; ++d) {
                if (mapping.detached[d].node_idx == graph.get_connection(conn).output.node_idx) {
                    TEST(mapping.detached[d].desc_id == drop_lfo[row]);
                    found = true;
                }
            }
            TEST(found);
        }
    }
}

static void check_osc_inline_1()
{

    // Add Envelope's default descriptor (do_osc_add_generator's construction
    // - the function is GUI-only, so the test replicates it) satisfies bank
    // validation and sustains at the full-scale peak: the delta is in
    // per-65535 point units and the attack spans about 30 ms (positions are
    // control ticks, 256/44100 s each).
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        memset(static_cast<void*>(&bank), 0, sizeof(bank));
        const uint32_t slot = bank.bank.envelopes.allocate();
        TEST(slot != pool_no_slot);
        Synth::EnvelopeDescriptor& env = bank.bank.envelopes.entries[slot];
        env                            = Synth::EnvelopeDescriptor{};
        env.num_points                 = 3;
        env.sustain_first_point        = 1;
        env.sustain_last_point         = 1;
        env.min_value                  = -1.0f;
        env.min_max_delta              = 2.0f / 65535.0f;
        env.points[0].position         = 0;
        env.points[0].value            = 0x0000;
        env.points[1].position         = 5;
        env.points[1].value            = 0xFFFF;
        env.points[2].position         = 91;
        env.points[2].value            = 0x8000;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        Synth::EnvelopeState state = { 0, 0 };
        float                peak  = env.min_value;
        for (int i = 0; i < 8; i++) {
            peak = Synth::eval_envelope(env, &state, true);
        }
        TEST(approx(peak, 1.0f, 1e-4f));
        // The sustain loop holds the peak: further evaluation does not move
        // past the final point.
        TEST(approx(Synth::eval_envelope(env, &state, true), 1.0f, 1e-4f));
        // Note-off: the release tail descends from the peak back to neutral
        // (0.0 on this envelope's -1..1 range) and the envelope stays there,
        // so the voice can free on silence.
        float released = 1.0f;
        for (int i = 0; i < 96; i++) {
            released = Synth::eval_envelope(env, &state, false);
        }
        TEST(approx(released, 0.0f, 1e-4f));
        TEST(approx(Synth::eval_envelope(env, &state, false), 0.0f, 1e-4f));
    }
}

int main()
{
    test_silence_stdout();
    osc_inputs_move_reprojection_regressions();
    osc_slot_disable_stamped_rules_regressions();
    osc_stamp_pool_clear_regressions();
    child_row_projection_regressions();
    osc_layer_projection_regressions();
    osc_direct_modulator_regressions();
    osc_instrument_node_regressions();
    osc_instrument_node_layout_regressions();
    osc_cc_regressions();
    check_osc_pitch_gating();
    check_osc_lowest_free_slot();
    check_osc_wire_any_free_slot();
    check_osc_wire_auto_move();
    check_osc_red_run_derivation();
    check_osc_playback_gaps();
    check_osc_unwired_params_persist();
    check_osc_add_lowest_free_slot();
    check_osc_source_identity_install();
    check_osc_delete_clears_shared_sources();
    check_osc_delete_with_master_effect_record();
    check_osc_store_atomic_failure();
    check_osc_clipboard_preserves_unwired();
    check_osc_export_drops_unwired();
    check_osc_export_zero_wired_refused();
    check_osc_undo_source_rows();
    check_osc_inline_0();
    check_osc_inline_1();
    return test_exit_code();
}
