// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "synth_test_common.h"

static void effects_input_gesture_regressions(Synth::InstrumentEditorBank*  source,
                                              Synth::InstrumentEditorBank*  candidate,
                                              Synth::InstrumentEditorBank*  snapshot,
                                              Sculptor::Graph*              graph,
                                              Sculptor::EffectGraphMapping* mapping)
{
    enum class Scenario {
        c_before_a,
        b_before_a,
        c_append,
        own_input,
        adjacent_input,
        last_append,
        source_output_move_after,
        two_intents,
        deleted_target,
        raw_disconnect,
        first_append,
        deleted_grabbed_endpoint,
        user_disconnect_cancel,
        target_deleted_and_reused,
        feed_moved,
        pending_property_edit,
        property_disconnect,
        next_gesture_after_projection,
        midi_disconnect,
        queue_overflow
    };
    const uint32_t owners[] = { 0u, Sculptor::fx_master_chain };
    for (uint32_t owner : owners) {
        for (uint32_t example = 0; example < 20; ++example) {
            const Scenario scenario = static_cast<Scenario>(example);

            memset(static_cast<void*>(source), 0, sizeof(*source));
            Synth::init_default_bank(&source->bank);
            uint16_t root = 0;
            TEST(Sculptor::add_effect_lfo_candidate(source, owner, source, &root, vmath::vec2(96.0f, 320.0f)));
            auto& chain       = Sculptor::fx_graph_chain(&source->bank, owner);
            chain.num_effects = 4;
            for (uint32_t slot = 0; slot < 4; ++slot) {
                Sculptor::fx_init_slot(&chain, slot, Synth::EffectType::delay);
                chain.effects[slot].enabled = slot != 1;
                auto& binding               = chain.effects[slot].bindings[0];
                binding.base_value          = 100.0f + static_cast<float>(slot);
                binding.lfo_desc_id         = root;
                binding.lfo_depth           = 0.25f;
                binding.num_inputs          = 1;
                binding.inputs[0].source    = Synth::ModSource::mod_wheel;
                binding.inputs[0].scale     = 0.5f;
            }
            auto& routing = source->effect_routing[owner];
            routing.in    = Sculptor::effect_in_first_slot;
            for (uint32_t slot = 0; slot < 4; ++slot) {
                routing.out[slot] = slot + 1 < 4 ? Sculptor::effect_out_next : Sculptor::effect_out_output;
            }
            TEST(Sculptor::add_effect_lfo_candidate(source, owner, source, &root, vmath::vec2(96.0f, 320.0f)));
            bool pins[Synth::max_lfos] = {};
            TEST(Sculptor::collect_effect_lfos(source, owner, pins));
            const bool projected = Sculptor::project_effect_chain_to_graph(*source, owner, graph, mapping, pins);
            TEST(projected);
            Sculptor::GraphChange changes[Sculptor::max_pending_changes];
            graph->take_changes(changes, Sculptor::max_pending_changes);
            for (uint32_t slot = 0; slot < 4; ++slot) {
                graph->set_node_layout(mapping->effect_nodes[slot],
                                       { 100.0f + static_cast<float>(slot * 32), 16.0f },
                                       240.0f + static_cast<float>(slot),
                                       180.0f);
            }
            TEST(Sculptor::stage_effect_graph_edits(source, graph, mapping, candidate));
            *source                          = *candidate;
            *snapshot                        = *source;
            const uint32_t           grabbed = scenario == Scenario::first_append  ? 0u
                                               : scenario == Scenario::b_before_a  ? 1u
                                               : scenario == Scenario::last_append ? 3u
                                                                                   : 2u;
            const Sculptor::EndPoint input   = { mapping->effect_nodes[grabbed], 0 };
            const Sculptor::EndPoint output  = { grabbed ? mapping->effect_nodes[grabbed - 1] : mapping->input_node,
                                                 grabbed ? 1u : 0u };
            const uint32_t           wire    = find_fx_graph_connection(*graph, output, input);
            TEST(wire != Sculptor::pool_no_slot);
            TEST(! graph->user_delete_connection(Sculptor::pool_no_slot));
            if (scenario == Scenario::pending_property_edit) {
                auto value = graph->node(mapping->effect_nodes[2]).slots.entries[Sculptor::fx_param_row(0)].value;
                value.real = 105.0f;
                graph->set_slot_value(mapping->effect_nodes[2], Sculptor::fx_param_row(0), value);
            }
            bool expected = true;
            if (scenario == Scenario::queue_overflow) {
                for (uint32_t index = 0; index < Sculptor::max_pending_changes; ++index) {
                    graph->rename_node(mapping->input_node, index % 2 ? "A" : "B");
                }
                TEST(! graph->move_connection_end(wire, false, { mapping->effect_nodes[0], 0 }));
                graph->rename_node(mapping->input_node, "overflow");
                TEST(graph->changes_overflowed());
                *candidate = *snapshot;
                TEST(! Sculptor::stage_effect_graph_edits(source, graph, mapping, candidate));
                TEST(memcmp(candidate, snapshot, sizeof(*candidate)) == 0);
                TEST(memcmp(source, snapshot, sizeof(*source)) == 0);
                continue;
            }
            if (scenario == Scenario::property_disconnect || scenario == Scenario::midi_disconnect) {
                const auto from =
                    scenario == Scenario::property_disconnect
                        ? Sculptor::EndPoint{ mapping->lfo_nodes[0], 0 }
                        : Sculptor::EndPoint{
                              mapping->midi_node,
                              mapping->midi_source_slots[static_cast<uint32_t>(Synth::ModSource::mod_wheel) - 1]
                          };
                const auto to =
                    Sculptor::EndPoint{ mapping->effect_nodes[2],
                                        scenario == Scenario::property_disconnect ? Sculptor::fx_param_lfo_dot(0)
                                                                                  : Sculptor::fx_param_src_dot(0, 0) };
                const uint32_t mod = find_fx_graph_connection(*graph, from, to);
                TEST(graph->user_delete_connection(mod));
                TEST(! graph->connection_occupied(mod));
                TEST(! graph->user_delete_connection(mod));
            }
            else if (scenario == Scenario::raw_disconnect || scenario == Scenario::user_disconnect_cancel ||
                     scenario == Scenario::next_gesture_after_projection) {
                TEST(graph->user_delete_connection(wire));
            }
            else if (scenario == Scenario::deleted_target || scenario == Scenario::deleted_grabbed_endpoint ||
                     scenario == Scenario::target_deleted_and_reused) {
                const uint32_t deleted =
                    scenario == Scenario::deleted_grabbed_endpoint ? input.node_idx : mapping->effect_nodes[0];
                graph->delete_node(deleted);
                if (scenario == Scenario::target_deleted_and_reused) {
                    TEST(graph->create_node("reused", {}) == deleted);
                    graph->add_slot(deleted, Sculptor::Slot{});
                    expected = false;
                }
                TEST(! graph->move_connection_end(wire, false, { deleted, 0 }));
            }
            else if (scenario == Scenario::source_output_move_after || scenario == Scenario::feed_moved) {
                TEST(graph->move_connection_end(wire, true, { mapping->effect_nodes[3], 1 }));
                expected = false; // detached cycle/output branching in final raw graph
            }
            else {
                Sculptor::EndPoint target = { mapping->effect_nodes[0], 0 };
                if (scenario == Scenario::own_input) {
                    target = input;
                }
                if (scenario == Scenario::adjacent_input) {
                    target = { mapping->effect_nodes[3], 0 };
                }
                if (scenario == Scenario::c_append || scenario == Scenario::last_append ||
                    scenario == Scenario::first_append) {
                    target = { mapping->output_node, 0 };
                }
                const bool same = target.node_idx == input.node_idx;
                TEST(graph->move_connection_end(wire, false, target) == same);
                if (scenario == Scenario::two_intents) {
                    TEST(! graph->move_connection_end(wire, false, { mapping->output_node, 0 }));
                }
            }
            const uint32_t count = graph->take_changes(changes, Sculptor::max_pending_changes);
            *candidate           = *snapshot;
            const bool applied =
                Sculptor::apply_effect_graph_batch(source, graph, mapping, changes, count, nullptr, candidate);
            TEST(applied == expected);
            TEST(memcmp(source, snapshot, sizeof(*source)) == 0);
            if (! applied) {
                TEST(memcmp(candidate, snapshot, sizeof(*candidate)) == 0);
                continue;
            }
            const auto& result = Sculptor::fx_graph_chain(&candidate->bank, owner);
            if (scenario == Scenario::deleted_target || scenario == Scenario::deleted_grabbed_endpoint) {
                TEST(result.num_effects == 3);
                TEST(Sculptor::validate_effect_routing(result, candidate->effect_routing[owner]));
                const Synth::EffectSlotBinding empty = {};
                TEST(memcmp(&result.effects[3], &empty, sizeof(empty)) == 0);
                continue;
            }
            if (scenario == Scenario::pending_property_edit) {
                chain.effects[2].bindings[0].base_value = 105.0f;
            }
            if (scenario == Scenario::property_disconnect) {
                chain.effects[2].bindings[0].lfo_desc_id = 0;
            }
            if (scenario == Scenario::midi_disconnect) {
                chain.effects[2].bindings[0].num_inputs = 0;
            }
            TEST(result.num_effects == 4);
            for (uint32_t slot = 0; slot < 4; ++slot) {
                TEST(memcmp(&result.effects[slot], &chain.effects[slot], sizeof(Synth::EffectSlotBinding)) == 0);
                char title[32];
                if (! slot) {
                    snprintf(title, sizeof(title), "Delay");
                }
                else {
                    snprintf(title, sizeof(title), "Delay %u", slot + 1);
                }
                bool geometry = false;
                for (uint32_t index = 0; index < candidate->graph_layout_count; ++index) {
                    const auto& layout = candidate->graph_layout[index];
                    geometry |= layout.channel == owner && strcmp(layout.name, title) == 0 &&
                                layout.x == 100.0f + static_cast<float>(slot * 32) &&
                                layout.width_override == 240.0f + static_cast<float>(slot);
                }
                TEST(geometry);
            }
            bool roots[Synth::max_lfos] = {};
            TEST(Sculptor::collect_effect_lfos(candidate, owner, roots));
            TEST(roots[0] && roots[1]);
            TEST(Sculptor::validate_editor_metadata(*candidate));
            if (scenario == Scenario::next_gesture_after_projection) {
                *source = *candidate;
                TEST(Sculptor::project_effect_chain_to_graph(*source, owner, graph, mapping, roots));
                graph->take_changes(changes, Sculptor::max_pending_changes);
                TEST(graph->attempt_connection(output, input));
                const uint32_t next_count = graph->take_changes(changes, Sculptor::max_pending_changes);
                TEST(next_count == 1);
                TEST(Sculptor::apply_effect_graph_batch(source,
                                                        graph,
                                                        mapping,
                                                        changes,
                                                        next_count,
                                                        nullptr,
                                                        candidate));
                TEST(Sculptor::fx_graph_chain(&candidate->bank, owner).effects[2].bindings[0].base_value == 102.0f);
            }
        }
    }
}

static void effects_literal_topology_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate      = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       before         = *before_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};
    const uint32_t                                     owners[]       = { 0u, Sculptor::fx_master_chain };

    for (uint32_t owner : owners) {
        for (uint32_t endpoint = 0; endpoint < 2; ++endpoint) {
            memset(static_cast<void*>(&source), 0, sizeof(source));
            Synth::init_default_bank(&source.bank);
            Sculptor::set_identity_osc_sources(&source, 0, 0);
            auto& chain       = Sculptor::fx_graph_chain(&source.bank, owner);
            chain.num_effects = 3;
            for (uint32_t slot = 0; slot < 3; ++slot) {
                Sculptor::fx_init_slot(&chain, slot, Synth::EffectType::chorus);
                chain.effects[slot].bindings[0].base_value = 10.0f + float(slot);
            }
            auto& routing  = source.effect_routing[owner];
            routing.in     = Sculptor::effect_in_first_slot;
            routing.out[0] = Sculptor::effect_out_next;
            routing.out[1] = Sculptor::effect_out_next;
            routing.out[2] = Sculptor::effect_out_output;

            uint16_t root = 0;
            TEST(Sculptor::add_effect_lfo_candidate(&source, owner, &source, &root, vmath::vec2(96.0f, 320.0f)));
            chain.effects[0].bindings[0].lfo_desc_id = root;
            chain.effects[1].bindings[0].lfo_desc_id = root;
            TEST(Sculptor::add_effect_lfo_candidate(&source, owner, &source, &root, vmath::vec2(96.0f, 320.0f)));

            TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping));
            Sculptor::GraphChange changes[Sculptor::max_pending_changes];
            graph.take_changes(changes, Sculptor::max_pending_changes);

            for (uint32_t slot = 0; slot < 3; ++slot) {
                graph.set_node_layout(mapping.effect_nodes[slot],
                                      { 64.0f + float(slot * 32), 160.0f },
                                      250.0f + float(slot),
                                      180.0f);
            }

            TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
            source = candidate;
            memcpy(&before, &source, sizeof(before));

            const uint32_t wire =
                find_fx_graph_connection(graph, { mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 });
            TEST(wire != Sculptor::pool_no_slot);

            // Occupied retargets refuse without implicit insertion or event traffic.
            TEST(! graph.move_connection_end(wire, false, { mapping.effect_nodes[2], 0 }));
            TEST(graph.take_changes(changes, Sculptor::max_pending_changes) == 0);
            TEST(graph.has_error());
            graph.dismiss_error();

            TEST(graph.user_delete_connection(wire));
            bool           endpoint_overflow = false;
            const uint32_t count = graph.peek_changes(changes, Sculptor::max_pending_changes, &endpoint_overflow);
            TEST(! endpoint_overflow);
            TEST(count == 1);

            memset(static_cast<void*>(&candidate), 0xA5, sizeof(candidate));
            const bool detached = Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate);
            TEST(detached);
            TEST(memcmp(&source, &before, sizeof(source)) == 0);
            TEST(graph.peek_changes(changes, Sculptor::max_pending_changes, &endpoint_overflow) == count);
            if (! detached) {
                continue;
            }

            const Sculptor::EffectChainRouting expected = { { Sculptor::effect_out_disconnected,
                                                              Sculptor::effect_out_next,
                                                              Sculptor::effect_out_output,
                                                              Sculptor::effect_out_disconnected },
                                                            1 };
            TEST(memcmp(&candidate.effect_routing[owner], &expected, sizeof(expected)) == 0);
            TEST(memcmp(&Sculptor::fx_graph_chain(&candidate.bank, owner),
                        &Sculptor::fx_graph_chain(&before.bank, owner),
                        sizeof(chain)) == 0);
            TEST(memcmp(candidate.graph_layout,
                        source.graph_layout,
                        source.graph_layout_count * sizeof(source.graph_layout[0])) == 0);
            bool roots[Synth::max_lfos] = {};
            TEST(Sculptor::collect_effect_lfos(&candidate, owner, roots));
            TEST(roots[0] && roots[1]);

            const std::unique_ptr<FixedArray<uint8_t, 10 * sizeof(Synth::InstrumentEditorBank) + 512>>
                undo_bytes_storage =
                    std::make_unique<FixedArray<uint8_t, 10 * sizeof(Synth::InstrumentEditorBank) + 512>>();
            uint8_t (&undo_bytes)[10 * sizeof(Synth::InstrumentEditorBank) + 512] = undo_bytes_storage->elements;
            const std::unique_ptr<FixedArray<Synth::InstrumentEditorBank, 5>> states_storage =
                std::make_unique<FixedArray<Synth::InstrumentEditorBank, 5>>();
            Synth::InstrumentEditorBank(&states)[5] = states_storage->elements;

            states[0] = before;
            states[1] = candidate;
            Sculptor::UndoRedo undo;
            undo.init(undo_bytes);
            undo.init_undo_push();
            undo.push(&states[0], sizeof(states[0]));
            TEST(undo.finish_undo_push());

            source = states[1];
            graph.take_changes(changes, Sculptor::max_pending_changes);
            auto value = graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)].value;
            value.real = 7.25f;
            graph.set_slot_value(mapping.effect_nodes[0], Sculptor::fx_param_row(0), value);
            TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
            states[2] = candidate;
            undo.init_undo_push();
            undo.push(&source, sizeof(source));
            TEST(undo.finish_undo_push());

            source = candidate;
            graph.take_changes(changes, Sculptor::max_pending_changes);
            TEST(graph.attempt_connection({ mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 }));
            TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
            TEST(Sculptor::fx_graph_chain(&candidate.bank, owner).effects[0].bindings[0].base_value == 7.25f);
            states[3] = candidate;
            undo.init_undo_push();
            undo.push(&source, sizeof(source));
            TEST(undo.finish_undo_push());

            source = candidate;
            graph.take_changes(changes, Sculptor::max_pending_changes);
            graph.delete_node(mapping.effect_nodes[1]);
            TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
            states[4] = candidate;
            TEST(Sculptor::fx_graph_chain(&candidate.bank, owner).num_effects == 2);
            // Deleting a node splices the chain: the predecessor feeds the
            // deleted node's successor (slot 0 -> compacted old slot 2).
            TEST(candidate.effect_routing[owner].out[0] == Sculptor::effect_out_next);
            TEST(candidate.effect_routing[owner].out[1] == Sculptor::effect_out_output);
            undo.init_undo_push();
            undo.push(&source, sizeof(source));
            TEST(undo.finish_undo_push());

            for (uint32_t step = 4; step > 0; --step) {
                undo.init_redo_push();
                undo.push(&candidate, sizeof(candidate));
                TEST(undo.finish_redo_push());
                TEST(undo.init_undo());
                undo.pop(&candidate, sizeof(candidate));
                undo.finish_undo();
                TEST(memcmp(&candidate, &states[step - 1], sizeof(candidate)) == 0);
            }

            for (uint32_t step = 1; step <= 4; ++step) {
                TEST(undo.init_redo());
                undo.pop(&candidate, sizeof(candidate));
                undo.finish_redo();
                TEST(memcmp(&candidate, &states[step], sizeof(candidate)) == 0);
            }
        }
    }
}

static void effects_structural_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate      = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       loaded         = *loaded_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};
    const std::unique_ptr<FixedArray<char, 262144>>    text_storage   = std::make_unique<FixedArray<char, 262144>>();
    char (&text)[262144]                                              = text_storage->elements;
    effects_literal_topology_regressions();
    // Literal routing preserves independent payload, capacity and layout invariants.
    effects_input_gesture_regressions(&source, &candidate, &loaded, &graph, &mapping);
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    Synth::EffectChainBinding& chain = source.bank.channel_chains[0];
    chain.num_effects                = 3;
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
    Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::chorus);
    Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::delay);
    for (uint32_t slot = 0; slot < 3; ++slot) {
        chain.effects[slot].enabled = true;
    }
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes];
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    graph.set_node_layout(mapping.effect_nodes[0], { 101.0f, 11.0f }, 301.0f, 201.0f);
    graph.set_node_layout(mapping.effect_nodes[1], { 202.0f, 22.0f }, 302.0f, 202.0f);
    graph.set_node_layout(mapping.effect_nodes[2], { 303.0f, 33.0f }, 303.0f, 203.0f);
    TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    source = candidate;
    TEST(graph.add_connection({ mapping.input_node, 0 }, { mapping.effect_nodes[1], 0 }) != Sculptor::pool_no_slot);
    graph.delete_node(mapping.effect_nodes[1]);
    TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 2);
    TEST(candidate.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::delay);
    bool first_geometry  = false;
    bool second_geometry = false;
    for (uint32_t record = 0; record < candidate.graph_layout_count; ++record) {
        const Synth::GraphNodeLayout& layout = candidate.graph_layout[record];
        first_geometry |= strcmp(layout.name, "Delay") == 0 && layout.x == 101.0f;
        second_geometry |= strcmp(layout.name, "Delay 2") == 0 && layout.x == 303.0f;
    }
    TEST(first_geometry && second_geometry);
    // The raw seam carries no routing state and refuses audio wires; the
    // batch seam applies the same hand-built events through the flags.
    for (uint32_t scenario = 0; scenario < 6; ++scenario) {
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        Sculptor::GraphChange change = {};
        change.kind                  = Sculptor::ChangeKind::connection_added;
        change.connection_input      = { mapping.effect_nodes[0], 0 };
        change.connection_output     = { mapping.effect_nodes[0], 1 };
        if (scenario == 1 || scenario == 2) {
            change.connection_input  = { mapping.output_node, 0 };
            change.connection_output = { mapping.effect_nodes[2], 1 };
        }
        if (scenario == 2) {
            change.kind = Sculptor::ChangeKind::connection_deleted;
        }
        if (scenario == 3) {
            change.connection_output = { mapping.input_node, 0 };
        }
        if (scenario == 4) {
            change.connection_input  = { mapping.output_node, 0 };
            change.connection_output = { mapping.effect_nodes[0], 1 };
        }
        if (scenario == 5) {
            graph.delete_node(mapping.output_node);
        }
        candidate          = source;
        const bool applied = Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, change);
        TEST(! applied); // the raw seam refuses audio wires
        const bool batched =
            Sculptor::apply_effect_graph_batch(&source, &graph, &mapping, &change, 1, nullptr, &loaded);
        TEST(batched == (scenario != 0 && scenario != 5)); // self-connect and the deleted-output capture refuse
        if (batched) {
            TEST(memcmp(&source.bank.channel_chains[0],
                        &loaded.bank.channel_chains[0],
                        sizeof(Synth::EffectChainBinding)) == 0);
        }
    }
    candidate                        = source;
    candidate.bank.channel_chains[0] = {};
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    Sculptor::GraphChange empty_wire = {};
    empty_wire.kind                  = Sculptor::ChangeKind::connection_added;
    empty_wire.connection_input      = { mapping.output_node, 0 };
    empty_wire.connection_output     = { mapping.input_node, 0 };
    memset(static_cast<void*>(&loaded), 0xA5, sizeof(loaded));
    // The direct input -> output wire is the bypass: it connects on any
    // chain, empty or wired, and marks the chain input routing.
    TEST(Sculptor::apply_effect_graph_batch(&candidate, &graph, &mapping, &empty_wire, 1, nullptr, &loaded));
    TEST(loaded.effect_routing[0].in == Sculptor::effect_in_output);
    TEST(loaded.effect_routing[0].out[0] == Sculptor::effect_out_disconnected);
    TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[0], loaded.effect_routing[0]));

    // A CC input without a registry dot stays unwired, so any graph edit
    // that would recompile its parameter refuses atomically and preserves
    // the binding; a CC input with a projected dot, added via the editor,
    // compiles and is editable.  Legacy-only rebuilds must not carry stale
    // CC numbers into the new validator.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> cc_source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& cc_source = *cc_source_storage;
        memset(static_cast<void*>(&cc_source), 0, sizeof(cc_source));
        Synth::init_default_bank(&cc_source.bank);
        Sculptor::set_identity_osc_sources(&cc_source, 0, 0);
        Synth::EffectChainBinding& cc_chain = cc_source.bank.channel_chains[0];
        cc_chain.num_effects                = 1;
        Sculptor::fx_init_slot(&cc_chain, 0, Synth::EffectType::delay);
        cc_chain.effects[0].enabled           = true;
        Synth::EffectParamBinding& cc_binding = cc_chain.effects[0].bindings[0];
        cc_binding.num_inputs                 = 2;
        cc_binding.inputs[0].source           = Synth::ModSource::cc;
        cc_binding.inputs[0].cc_number        = 16;
        cc_binding.inputs[0].op               = Synth::SourceOp::add;
        cc_binding.inputs[0].scale            = 0.5f;
        cc_binding.inputs[1].source           = Synth::ModSource::mod_wheel;
        cc_binding.inputs[1].op               = Synth::SourceOp::multiply;
        cc_binding.inputs[1].scale            = 0.25f;
        TEST(Sculptor::project_effect_chain_to_graph(cc_source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        const Synth::EffectParamBinding cc_before = cc_chain.effects[0].bindings[0];

        // A source-row edit on the wired legacy row must refuse atomically.
        Sculptor::GraphChange cc_row = {};
        cc_row.kind                  = Sculptor::ChangeKind::value_changed;
        cc_row.node_idx              = mapping.effect_nodes[0];
        cc_row.slot_idx              = Sculptor::fx_param_src_dot(0, 1) + 2; // legacy row's Amount
        auto cc_row_value            = graph.node(mapping.effect_nodes[0]).slots.entries[cc_row.slot_idx].value;
        cc_row_value.real            = 0.75f;
        graph.set_slot_value(mapping.effect_nodes[0], cc_row.slot_idx, cc_row_value);
        candidate = cc_source;
        TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, cc_row));
        TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].bindings[0], &cc_before, sizeof(cc_before)) == 0);

        // A connection edit on the same row must refuse atomically too.
        const Sculptor::EndPoint cc_wire_from  = { mapping.midi_node, mapping.midi_source_slots[1] };
        const Sculptor::EndPoint cc_wire_input = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 1) };
        const uint32_t           cc_wire       = find_fx_graph_connection(graph, cc_wire_from, cc_wire_input);
        TEST(cc_wire != Sculptor::pool_no_slot);
        TEST(graph.user_delete_connection(cc_wire));
        Sculptor::GraphChange cc_unwire = {};
        cc_unwire.kind                  = Sculptor::ChangeKind::connection_deleted;
        cc_unwire.connection_output     = cc_wire_from;
        cc_unwire.connection_input      = cc_wire_input;
        candidate                       = cc_source;
        TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, cc_unwire));
        TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].bindings[0], &cc_before, sizeof(cc_before)) == 0);
        // A retarget that moves a legacy wire onto a parameter carrying an
        // undotted CC input must refuse atomically: the previous parameter's
        // rebuild must not leak into the bank when the destination refuses.
        // An undotted CC input stays unwired and refuses recompilation; a
        // dotted CC input, added via the editor, compiles and is editable.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> retarget_source_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& retarget_source = *retarget_source_storage;
            memset(static_cast<void*>(&retarget_source), 0, sizeof(retarget_source));
            Synth::init_default_bank(&retarget_source.bank);
            Sculptor::set_identity_osc_sources(&retarget_source, 0, 0);
            Synth::EffectChainBinding& retarget_chain = retarget_source.bank.channel_chains[0];
            retarget_chain.num_effects                = 1;
            Sculptor::fx_init_slot(&retarget_chain, 0, Synth::EffectType::delay);
            retarget_chain.effects[0].enabled        = true;
            Synth::EffectParamBinding& moved_binding = retarget_chain.effects[0].bindings[0];
            moved_binding.num_inputs                 = 1;
            moved_binding.inputs[0].source           = Synth::ModSource::mod_wheel;
            moved_binding.inputs[0].op               = Synth::SourceOp::add;
            moved_binding.inputs[0].scale            = 0.5f;
            Synth::EffectParamBinding& cc_dest       = retarget_chain.effects[0].bindings[1];
            cc_dest.num_inputs                       = 1;
            cc_dest.inputs[0].source                 = Synth::ModSource::cc;
            cc_dest.inputs[0].cc_number              = 16;
            cc_dest.inputs[0].op                     = Synth::SourceOp::add;
            cc_dest.inputs[0].scale                  = 0.5f;
            TEST(Sculptor::project_effect_chain_to_graph(retarget_source, 0, &graph, &mapping));
            graph.take_changes(discarded, Sculptor::max_pending_changes);
            const Sculptor::EndPoint retarget_wire_from  = { mapping.midi_node, mapping.midi_source_slots[1] };
            const Sculptor::EndPoint retarget_prev_input = { mapping.effect_nodes[0],
                                                             Sculptor::fx_param_src_dot(0, 0) };
            const Sculptor::EndPoint retarget_new_input = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) };
            const uint32_t retarget_wire = find_fx_graph_connection(graph, retarget_wire_from, retarget_prev_input);
            TEST(retarget_wire != Sculptor::pool_no_slot);
            TEST(graph.move_connection_end(retarget_wire, false, retarget_new_input));
            graph.take_changes(discarded, Sculptor::max_pending_changes);
            Sculptor::GraphChange retarget = {};
            retarget.kind                  = Sculptor::ChangeKind::connection_changed;
            retarget.connection_output     = retarget_wire_from;
            retarget.connection_input      = retarget_new_input;
            retarget.connection_prev_input = retarget_prev_input;
            candidate                      = retarget_source;
            TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, retarget));
            TEST(memcmp(&candidate.bank, &retarget_source.bank, sizeof(candidate.bank)) == 0);
        }

        // A legacy-only binding rebuild zeroes the CC number on reconstructed
        // inputs so a reused slot cannot carry stale state.
        const std::unique_ptr<Synth::InstrumentEditorBank> legacy_source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& legacy_source = *legacy_source_storage;
        memset(static_cast<void*>(&legacy_source), 0, sizeof(legacy_source));
        Synth::init_default_bank(&legacy_source.bank);
        Sculptor::set_identity_osc_sources(&legacy_source, 0, 0);
        Synth::EffectChainBinding& legacy_chain = legacy_source.bank.channel_chains[0];
        legacy_chain.num_effects                = 1;
        Sculptor::fx_init_slot(&legacy_chain, 0, Synth::EffectType::delay);
        legacy_chain.effects[0].enabled           = true;
        Synth::EffectParamBinding& legacy_binding = legacy_chain.effects[0].bindings[0];
        legacy_binding.num_inputs                 = 1;
        legacy_binding.inputs[0].source           = Synth::ModSource::mod_wheel;
        legacy_binding.inputs[0].cc_number        = 17; // stale hand-authored state
        legacy_binding.inputs[0].op               = Synth::SourceOp::multiply;
        legacy_binding.inputs[0].scale            = 0.25f;
        TEST(Sculptor::project_effect_chain_to_graph(legacy_source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        Sculptor::GraphChange legacy_row = {};
        legacy_row.kind                  = Sculptor::ChangeKind::value_changed;
        legacy_row.node_idx              = mapping.effect_nodes[0];
        legacy_row.slot_idx              = Sculptor::fx_param_src_dot(0, 0) + 2; // wired row's Amount
        auto legacy_row_value            = graph.node(mapping.effect_nodes[0]).slots.entries[legacy_row.slot_idx].value;
        legacy_row_value.real            = 0.75f;
        graph.set_slot_value(mapping.effect_nodes[0], legacy_row.slot_idx, legacy_row_value);
        candidate = legacy_source;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, legacy_row));
        const Synth::ModInput& rebuilt = candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0];
        TEST(rebuilt.source == Synth::ModSource::mod_wheel);
        TEST(rebuilt.cc_number == 0);
        TEST(rebuilt.op == Synth::SourceOp::multiply);
        TEST(rebuilt.scale == 0.75f);
    }

    // Ordinary files may contain obsolete generated effect titles, but not missing LFO roots.
    source.graph_layout[source.graph_layout_count] = {};
    Synth::GraphNodeLayout& obsolete               = source.graph_layout[source.graph_layout_count++];
    obsolete.kind                                  = 4;
    snprintf(obsolete.name, sizeof(obsolete.name), "Chorus 4");
    bool keep[Synth::max_lfos] = {};
    TEST(Sculptor::collect_effect_lfos(&source, 0, keep));
    const char* bank_path = "synth_effects_structural_bank.tmp";
    TEST(Synth::save_editor_bank_file(bank_path, &source) == 0);
    TEST(Synth::load_editor_bank_file(bank_path, &loaded) == Synth::BankFileStatus::ok);
    TEST(Sculptor::collect_effect_lfos(&loaded, 0, keep));
    TEST(loaded.graph_layout_count + 1 == source.graph_layout_count);
    TEST(Synth::save_editor_bank_file(bank_path, &loaded) == 0);
    remove(bank_path);
    const char* library_path = "synth_effects_structural_library.tmp";
    remove(library_path);
    loaded = source;
    memset(static_cast<void*>(loaded.bank.channel_enabled), 0, sizeof(loaded.bank.channel_enabled));
    for (uint32_t owner = 1; owner < Synth::max_channels; ++owner) {
        memset(static_cast<void*>(loaded.bank.channel_zones[owner]), 0, sizeof(loaded.bank.channel_zones[owner]));
    }
    const uint32_t legacy_length = Synth::encode_editor_bank_json(&loaded, text, sizeof(text));
    TEST(legacy_length > 0);
    write_library_fixture(library_path, text, legacy_length);
    TEST(Synth::save_library_record(library_path, "Effects", "Legacy", &source, 0) == 0);
    write_library_fixture(library_path, text, legacy_length);
    Synth::LibraryEntry      entry  = {};
    Synth::LibraryScanStatus status = Synth::library_invalid;
    TEST(Synth::read_library_index(library_path, &entry, 1, &status) == 1);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    uint16_t instrument_slot = 0;
    TEST(Synth::load_library_instrument(library_path, &entry, &loaded, 1, &instrument_slot));
    TEST(Sculptor::collect_effect_lfos(&loaded, 1, keep));
    remove(library_path);
    snprintf(obsolete.name, sizeof(obsolete.name), "LFO 128");
    TEST(! Sculptor::collect_effect_lfos(&source, 0, keep));
    TEST(Synth::encode_editor_bank_json(&source, text, sizeof(text)) > 0);
    snprintf(obsolete.name, sizeof(obsolete.name), "Chorus 4");
    // Type mutations translate every ordinal, including survivors separated by empty slots.
    TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::chorus, &loaded));
    bool chorus_geometry = false;
    bool delay_geometry  = false;
    for (uint32_t index = 0; index < loaded.graph_layout_count; ++index) {
        const Synth::GraphNodeLayout& record = loaded.graph_layout[index];
        chorus_geometry |= strcmp(record.name, "Chorus") == 0 && record.x == 101.0f;
        delay_geometry |= strcmp(record.name, "Delay") == 0 && record.x == 303.0f;
    }
    TEST(chorus_geometry && delay_geometry);
    TEST(Sculptor::change_effect_type_candidate(&loaded, 0, 2, Synth::EffectType::chorus, &loaded));
    TEST(Sculptor::collect_effect_lfos(&loaded, 0, keep));
    TEST(loaded.bank.channel_chains[0].effects[2].type == Synth::EffectType::chorus);
    // Deleted output endpoints, multiple moves and multiple deletions share the drain boundary.
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    graph.set_node_layout(mapping.effect_nodes[0], { 101.0f, 11.0f }, 301.0f, 201.0f);
    graph.set_node_layout(mapping.effect_nodes[2], { 303.0f, 33.0f }, 303.0f, 203.0f);
    TEST(graph.add_connection({ mapping.effect_nodes[1], 1 }, { mapping.effect_nodes[0], 0 }) !=
         Sculptor::pool_no_slot);
    graph.delete_node(mapping.effect_nodes[1]);
    TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &loaded));
    TEST(loaded.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
    TEST(loaded.bank.channel_chains[0].effects[1].type == Synth::EffectType::delay);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection({ mapping.input_node, 0 }, { mapping.effect_nodes[2], 0 }) != Sculptor::pool_no_slot);
    TEST(graph.add_connection({ mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) != Sculptor::pool_no_slot);
    graph.delete_node(mapping.effect_nodes[0]);
    graph.delete_node(mapping.effect_nodes[1]);
    bool           capture_nodes[Sculptor::max_nodes] = {};
    bool           overflow                           = false;
    const uint32_t count = graph.peek_changes(discarded, Sculptor::max_pending_changes, &overflow);
    TEST(! overflow);
    TEST(Sculptor::apply_effect_graph_batch(&source, &graph, &mapping, discarded, count, capture_nodes, &loaded));
    TEST(loaded.bank.channel_chains[0].num_effects == 1);
    TEST(loaded.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
    bool retained_geometry = false;
    for (uint32_t index = 0; index < loaded.graph_layout_count; ++index) {
        retained_geometry |=
            strcmp(loaded.graph_layout[index].name, "Delay") == 0 && loaded.graph_layout[index].x == 303.0f;
    }
    TEST(retained_geometry);
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    Synth::EffectsDocument sparse_document = {};
    effects_clipboard_fixture(&sparse_document, true);
    TEST(Sculptor::replace_effect_chain_candidate(&source, 0, &sparse_document, &source));
    TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::chorus, &candidate));
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::distortion);
    TEST(Sculptor::extract_effect_chain_document(&candidate, 0, nullptr, nullptr, &sparse_document));
    TEST(sparse_document.chain.effects[2].type == Synth::EffectType::delay && sparse_document.lfo_count == 3);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    graph.delete_node(mapping.effect_nodes[0]);
    bool           sparse_capture[Sculptor::max_nodes] = {};
    const uint32_t sparse_count = graph.peek_changes(discarded, Sculptor::max_pending_changes, &overflow);
    TEST(Sculptor::apply_effect_graph_batch(&source,
                                            &graph,
                                            &mapping,
                                            discarded,
                                            sparse_count,
                                            sparse_capture,
                                            &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 3);
    TEST(candidate.bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::delay);
    TEST(Sculptor::extract_effect_chain_document(&candidate, 0, nullptr, nullptr, &sparse_document));
    TEST(sparse_document.lfo_count == 3);
    // Add LFO roots belong to one owner; replacement cannot remove another owner's root or shared descriptor.
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    uint16_t descriptor = 0xBEEF;
    TEST(Sculptor::add_effect_lfo_candidate(&source, 1, &source, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(descriptor == 1 && source.graph_layout_count == 1);
    memset(static_cast<void*>(keep), 0, sizeof(keep));
    TEST(Sculptor::collect_effect_lfos(&source, 0, keep));
    TEST(! keep[0]);
    TEST(Sculptor::collect_effect_lfos(&source, 1, keep));
    TEST(keep[0]);
    TEST(Sculptor::project_effect_chain_to_graph(source, 1, &graph, &mapping, keep));
    TEST(mapping.lfo_nodes[0] != Sculptor::pool_no_slot);
    Synth::EffectsDocument document = {};
    TEST(Sculptor::extract_effect_chain_document(&source, 1, nullptr, nullptr, &document));
    TEST(document.lfo_count == 1 && document.chain.num_effects == 0);
    TEST(Synth::save_editor_bank_file(bank_path, &source) == 0);
    TEST(Synth::load_editor_bank_file(bank_path, &loaded) == Synth::BankFileStatus::ok);
    TEST(loaded.graph_layout_count == 1 && loaded.bank.lfos.num_allocated == 1);
    remove(bank_path);
    source.bank.channel_enabled[1] = true;
    memcpy(source.bank.channel_zones[1], source.bank.channel_zones[0], sizeof(source.bank.channel_zones[1]));
    memcpy(source.graph_osc_source[1], source.graph_osc_source[0], sizeof(source.graph_osc_source[1]));
    TEST(Synth::save_library_record(library_path, "Effects", "Root", &source, 1) == 0);
    TEST(Synth::read_library_index(library_path, &entry, 1, &status) == 1);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    TEST(Synth::load_library_instrument(library_path, &entry, &loaded, 2, &instrument_slot));
    memset(static_cast<void*>(keep), 0, sizeof(keep));
    TEST(Sculptor::collect_effect_lfos(&loaded, 2, keep));
    TEST(keep[0]);
    remove(library_path);
    source.graph_layout[source.graph_layout_count]                                      = source.graph_layout[0];
    source.graph_layout[source.graph_layout_count++].channel                            = 2;
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id = 1;
    document                                                                            = {};
    TEST(Sculptor::replace_effect_chain_candidate(&source, 0, &document, &candidate));
    TEST(candidate.graph_layout_count == 2);
    TEST(Sculptor::replace_effect_chain_candidate(&source, 1, &document, &candidate));
    TEST(candidate.graph_layout_count == 1 && candidate.graph_layout[0].channel == 2);
    TEST(candidate.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id == 1);
    bool legacy_pins[Synth::max_lfos] = {};
    legacy_pins[0]                    = true;
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 1, &graph, &mapping, legacy_pins));
    TEST(mapping.lfo_nodes[0] != Sculptor::pool_no_slot && legacy_pins[0]);
    // New roots freeze the caller's add-time place; restoring a record
    // lands its node exactly there, on the shared LFO lane y.
    const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& snapshot      = *snapshot_storage;
    const uint32_t               root_owners[] = { 0, Sculptor::fx_master_chain };
    for (uint32_t owner : root_owners) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        for (uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
            snapshot = source;
            const vmath::vec2 add_place(96.0f + 112.0f * static_cast<float>(ordinal), 320.0f);
            TEST(Sculptor::add_effect_lfo_candidate(&source, owner, &candidate, &descriptor, add_place));
            TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
            source = candidate;
            memset(static_cast<void*>(keep), 0, sizeof(keep));
            TEST(Sculptor::collect_effect_lfos(&source, owner, keep));
            TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping, keep));
            const uint32_t                node_index = mapping.lfo_nodes[descriptor - 1];
            const Synth::GraphNodeLayout& root       = source.graph_layout[source.graph_layout_count - 1];
            // The record freezes the caller's add-time place; restoring it
            // lands the node exactly there, on the LFO lane y.
            TEST(root.x == add_place.x && root.y == add_place.y);
            graph.set_node_layout(node_index, { root.x, root.y }, root.width_override, root.height_override);
            TEST(graph.node(node_index).position.x == root.x && graph.node(node_index).position.y == root.y);
            TEST(graph.node(node_index).position.y != graph.node(mapping.input_node).position.y);
            if (ordinal) {
                TEST(root.x > source.graph_layout[source.graph_layout_count - 2].x);
            }
        }
    }

    // The one-node GUI precommit predicate includes roots, bindings and legacy pins.
    for (uint32_t owner : root_owners) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const uint32_t pin_fill = Synth::max_lfos - 3u; // three slots under the LFO pool
        // ceiling.  The raised node budget (150) keeps every reachable fx projection
        // legal, so the predicate refuses only out-of-pool pins and invalid descriptors.
        for (uint32_t index = 0; index < pin_fill; ++index) {
            uint16_t allocated = 0;
            TEST(Sculptor::allocate_default_lfo(&source.bank, &allocated));
            Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
            memset(static_cast<void*>(&record), 0, sizeof(record));
            record.kind    = 4;
            record.channel = static_cast<uint8_t>(owner);
            snprintf(record.name, sizeof(record.name), "LFO %u", allocated);
        }
        snapshot = source;
        TEST(Sculptor::fx_can_add_node(&source, owner));
        TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);
        memset(static_cast<void*>(keep), 0, sizeof(keep));
        TEST(Sculptor::collect_effect_lfos(&source, owner, keep));
        TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping, keep));
        TEST(mapping.lfo_count == pin_fill);
        --source.graph_layout_count;
        source.graph_layout[source.graph_layout_count] = {};
        TEST(Sculptor::fx_can_add_node(&source, owner));
        Sculptor::fx_init_slot(&Sculptor::fx_graph_chain(&source.bank, owner), 0, Synth::EffectType::distortion);
        Sculptor::fx_graph_chain(&source.bank, owner).num_effects                        = 1;
        Sculptor::fx_graph_chain(&source.bank, owner).effects[0].bindings[0].lfo_desc_id = 1;
        memset(static_cast<void*>(keep), 0, sizeof(keep));
        TEST(Sculptor::collect_effect_lfos(&source, owner, keep));
        TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping, keep));
        TEST(mapping.lfo_count == pin_fill - 1);
        source.effect_routing[owner] = {}; // an unwired occupied chain is a valid, pass-through-dry state
        TEST(Sculptor::validate_editor_metadata(source));
        source.graph_layout[source.graph_layout_count].kind    = 4;
        source.graph_layout[source.graph_layout_count].channel = static_cast<uint8_t>(owner);
        snprintf(source.graph_layout[source.graph_layout_count].name,
                 sizeof(source.graph_layout[source.graph_layout_count].name),
                 "LFO %u",
                 pin_fill);
        ++source.graph_layout_count;
        TEST(Sculptor::validate_editor_metadata(source)); // the raised ceiling admits the extra record
        --source.graph_layout_count;
        source.graph_layout[source.graph_layout_count] = {};
        TEST(Sculptor::fx_can_add_node(&source, owner));
        Sculptor::fx_graph_chain(&source.bank, owner) = {};
        memset(static_cast<void*>(keep), 0, sizeof(keep));
        keep[pin_fill - 1] = true;
        TEST(Sculptor::fx_can_add_node(&source, owner, keep));
        keep[pin_fill - 1] = false;
        keep[0]            = true;
        TEST(Sculptor::fx_can_add_node(&source, owner, keep));
        keep[Synth::max_lfos - 1] = true;
        TEST(! Sculptor::fx_can_add_node(&source, owner, keep)); // pin beyond the allocated pool refuses
    }
    TEST(! Sculptor::fx_can_add_node(nullptr, 0));
    TEST(! Sculptor::fx_can_add_node(&source, Sculptor::fx_master_chain + 1));
    snprintf(source.graph_layout[0].name, sizeof(source.graph_layout[0].name), "LFO 128");
    TEST(! Sculptor::fx_can_add_node(&source, Sculptor::fx_master_chain));

    // Legacy-only nodes occupy the same ordered lane as rooted and bound nodes.
    for (uint32_t owner : root_owners) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        TEST(Sculptor::allocate_default_lfo(&source.bank, &descriptor));
        TEST(Sculptor::add_effect_lfo_candidate(&source, owner, &source, &descriptor, vmath::vec2(96.0f, 320.0f)));
        Synth::EffectChainBinding& lane_chain = Sculptor::fx_graph_chain(&source.bank, owner);
        lane_chain.num_effects                = 1;
        Sculptor::fx_init_slot(&lane_chain, 0, Synth::EffectType::distortion);
        lane_chain.effects[0].bindings[0].lfo_desc_id = descriptor;
        memset(static_cast<void*>(keep), 0, sizeof(keep));
        keep[0]  = true;
        snapshot = source;
        TEST(Sculptor::add_effect_lfo_candidate(&source,
                                                owner,
                                                &candidate,
                                                &descriptor,
                                                vmath::vec2(208.0f, 320.0f),
                                                keep));
        TEST(memcmp(&source, &snapshot, sizeof(source)) == 0 && keep[0] && ! keep[1]);
        TEST(Sculptor::collect_effect_lfos(&candidate, owner, keep));
        TEST(Sculptor::project_effect_chain_to_graph(candidate, owner, &graph, &mapping, keep));
        const uint32_t                node_index = mapping.lfo_nodes[descriptor - 1];
        const Synth::GraphNodeLayout& root       = candidate.graph_layout[candidate.graph_layout_count - 1];
        graph.set_node_layout(node_index, { root.x, root.y }, root.width_override, root.height_override);
        // The pinned legacy root shares the ordered lane: its frozen place
        // restores exactly and sits past the first root on the lane y.
        TEST(root.x == 208.0f && root.y == 320.0f && mapping.lfo_count == 3);
        TEST(root.x > graph.node(mapping.lfo_nodes[descriptor - 2]).position.x && root.y != 0.0f);
    }

    // Root and descriptor capacity refuse without publishing either output.
    snapshot                  = candidate;
    source.graph_layout_count = Synth::max_graph_records;
    descriptor                = 0xBEEF;
    TEST(! Sculptor::add_effect_lfo_candidate(&source, 1, &candidate, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(descriptor == 0xBEEF && memcmp(&candidate, &snapshot, sizeof(candidate)) == 0);
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    for (uint32_t index = 0; index < Synth::max_lfos; ++index) {
        uint16_t allocated = 0;
        TEST(Sculptor::allocate_default_lfo(&source.bank, &allocated));
    }
    TEST(! Sculptor::add_effect_lfo_candidate(&source, 1, &candidate, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(descriptor == 0xBEEF && memcmp(&candidate, &snapshot, sizeof(candidate)) == 0);
    source.graph_layout_count = 0;
    for (uint32_t index = 0; index < Sculptor::max_nodes - 3; ++index) {
        Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
        memset(static_cast<void*>(&record), 0, sizeof(record));
        record.kind = 4;
        snprintf(record.name, sizeof(record.name), "LFO %u", index + 1);
    }
    source.bank.lfos.num_allocated = Synth::max_lfos - 1;
    TEST(! Sculptor::add_effect_lfo_candidate(&source, 0, &candidate, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(descriptor == 0xBEEF && memcmp(&candidate, &snapshot, sizeof(candidate)) == 0);
}

static void effects_paste_pin_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> saved_source_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       saved_source = *saved_source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> saved_candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           saved_candidate     = *saved_candidate_storage;
    Synth::EffectsDocument                 document            = {};
    const std::unique_ptr<Sculptor::Graph> graph_storage       = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph               = *graph_storage;
    const std::unique_ptr<Sculptor::Graph> saved_graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       saved_graph         = *saved_graph_storage;
    Sculptor::EffectGraphMapping           mapping             = {};
    const uint32_t                         owners[]            = { 0, Sculptor::fx_master_chain };
    for (uint32_t owner : owners) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        const uint32_t pin_limit                    = Synth::max_lfos - 3u; // input, output and MIDI on every chain
        bool           legacy_pins[Synth::max_lfos] = {};
        for (uint32_t index = 0; index < pin_limit; ++index) {
            uint16_t descriptor = 0;
            TEST(Sculptor::allocate_default_lfo(&source.bank, &descriptor));
            legacy_pins[index] = true;
        }
        document                       = {};
        document.lfo_count             = 1;
        document.lfos[0]               = source.bank.lfos.entries[0];
        document.graph_layout_count    = 1;
        document.graph_layout[0].kind  = Synth::effects_graph_layout_lfo;
        document.graph_layout[0].index = 1;
        TEST(Synth::validate_effects_document(&document));
        TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping, legacy_pins));
        Sculptor::GraphChange discarded[Sculptor::max_pending_changes];
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        graph.delete_node(mapping.lfo_nodes[0]);
        saved_graph  = graph;
        saved_source = source;
        bool saved_pins[Synth::max_lfos];
        memcpy(saved_pins, legacy_pins, sizeof(saved_pins));
        // The raised node budget lets the pinned replace through: the pasted
        // descriptor appends and the caller-owned inputs stay untouched.
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, legacy_pins));
        TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
        TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);
        TEST(memcmp(legacy_pins, saved_pins, sizeof(saved_pins)) == 0);

        source = saved_source;

        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate));
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, nullptr));
        legacy_pins[pin_limit - 1] = false;
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, legacy_pins));
        bool represented[Synth::max_lfos];
        memcpy(represented, legacy_pins, sizeof(represented));
        TEST(Sculptor::collect_effect_lfos(&candidate, owner, represented));
        TEST(Sculptor::fx_projected_node_count(candidate.bank, owner, represented) == pin_limit + 3u);
        TEST(Sculptor::project_effect_chain_to_graph(candidate, owner, &graph, &mapping, represented));
        TEST(mapping.lfo_count == pin_limit);
        TEST(Sculptor::fx_can_add_node(&candidate, owner, legacy_pins));
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &source, legacy_pins));
        TEST(memcmp(&source, &candidate, sizeof(source)) == 0);

        // The same descriptor can be pinned, rooted by two owners and bound without costing extra nodes.
        Synth::GraphNodeLayout& root           = source.graph_layout[source.graph_layout_count++];
        root                                   = source.graph_layout[0];
        root.channel                           = static_cast<uint8_t>(owner == 0 ? Sculptor::fx_master_chain : 0);
        legacy_pins[pin_limit]                 = true;
        Synth::EffectChainBinding& other_chain = Sculptor::fx_graph_chain(&source.bank, root.channel);
        other_chain.num_effects                = 1;
        Sculptor::fx_init_slot(&other_chain, 0, Synth::EffectType::distortion);
        other_chain.effects[0].bindings[0].lfo_desc_id = static_cast<uint16_t>(pin_limit + 1);
        document                                       = {};
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, legacy_pins));
        memcpy(represented, legacy_pins, sizeof(represented));
        TEST(Sculptor::collect_effect_lfos(&candidate, owner, represented));
        TEST(Sculptor::fx_projected_node_count(candidate.bank, owner, represented) == pin_limit + 3u);
        TEST(Sculptor::project_effect_chain_to_graph(candidate, owner, &graph, &mapping, represented));
        TEST(candidate.graph_layout_count == 1 && candidate.graph_layout[0].channel == root.channel);
        TEST(Sculptor::fx_graph_chain(&candidate.bank, root.channel).effects[0].bindings[0].lfo_desc_id ==
             pin_limit + 1);
        saved_source                     = source;
        saved_candidate                  = candidate;
        legacy_pins[Synth::max_lfos - 1] = true;
        TEST(! Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, legacy_pins));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);

        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        uint16_t shared_descriptor = 0;
        TEST(Sculptor::allocate_default_lfo(&source.bank, &shared_descriptor));
        document                   = {};
        document.lfo_count         = 1;
        document.lfos[0]           = source.bank.lfos.entries[0];
        document.chain.num_effects = 1;
        Sculptor::fx_init_slot(&document.chain, 0, Synth::EffectType::distortion);
        document.chain.effects[0].bindings[0].lfo_desc_id = 1;
        document.graph_layout_count                       = 1;
        document.graph_layout[0].kind                     = Synth::effects_graph_layout_lfo;
        document.graph_layout[0].index                    = 1;
        memset(static_cast<void*>(legacy_pins), 0, sizeof(legacy_pins));
        legacy_pins[1] = true;
        TEST(Sculptor::replace_effect_chain_candidate(&source, owner, &document, &candidate, legacy_pins));
        memcpy(represented, legacy_pins, sizeof(represented));
        TEST(Sculptor::collect_effect_lfos(&candidate, owner, represented));
        TEST(Sculptor::fx_projected_node_count(candidate.bank, owner, represented) == 5u);
        TEST(Sculptor::project_effect_chain_to_graph(candidate, owner, &graph, &mapping, represented));
        TEST(mapping.lfo_count == 1 && mapping.lfo_nodes[1] != Sculptor::pool_no_slot);
    }
}

static void effects_layout_classification_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> snapshot_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       snapshot = *snapshot_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> saved_candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           saved_candidate = *saved_candidate_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage   = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph           = *graph_storage;
    Sculptor::EffectGraphMapping           mapping         = {};
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);

    uint16_t descriptor = 0;
    TEST(Sculptor::allocate_default_lfo(&source.bank, &descriptor));
    Synth::EffectChainBinding& chain = source.bank.channel_chains[0];
    chain.num_effects                = 1;
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
    const char* titles[] = { "Channel input", "Chorus 4", "LFO 1", "Delay", "Channel output" };
    for (const char* title : titles) {
        Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
        memset(static_cast<void*>(&record), 0, sizeof(record));
        record.kind = 4;
        record.x    = static_cast<float>(source.graph_layout_count);
        snprintf(record.name, sizeof(record.name), "%s", title);
    }

    snapshot  = source;
    candidate = source;

    TEST(Sculptor::normalize_effect_layout(&candidate));
    TEST(candidate.graph_layout_count == 4);
    const uint32_t retained_indices[] = { 0, 2, 3, 4 };
    for (uint32_t index = 0; index < 4; ++index) {
        TEST(memcmp(&candidate.graph_layout[index],
                    &source.graph_layout[retained_indices[index]],
                    sizeof(Synth::GraphNodeLayout)) == 0);
    }

    bool collected[Synth::max_lfos] = {};
    TEST(Sculptor::collect_effect_lfos(&source, 0, collected));
    TEST(collected[0]);

    uint16_t lfo_ids[Synth::max_lfos] = {};
    lfo_ids[0]                        = 1;
    candidate                         = source;
    candidate.graph_layout_count      = 0;
    TEST(Sculptor::transfer_effect_layout(&source, 0, lfo_ids, 0, &candidate));
    TEST(candidate.graph_layout_count == 4);
    for (uint32_t index = 0; index < 4; ++index) {
        TEST(memcmp(&candidate.graph_layout[index],
                    &source.graph_layout[retained_indices[index]],
                    sizeof(Synth::GraphNodeLayout)) == 0);
    }

    TEST(Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::chorus, &candidate));
    TEST(candidate.graph_layout_count == 4 && strcmp(candidate.graph_layout[2].name, "Chorus") == 0);
    TEST(memcmp(&source, &snapshot, sizeof(source)) == 0);

    Synth::EffectsDocument portable_document = {};
    portable_document                        = {};
    portable_document.graph_layout_count     = 1;
    portable_document.graph_layout[0].kind   = Synth::effects_graph_layout_effect;
    TEST(! Synth::validate_effects_document(&portable_document));

    const char* invalid_titles[] = { "LFO 2", "Unknown", "Chorus 5" };
    for (const char* title : invalid_titles) {
        source = snapshot;
        snprintf(source.graph_layout[4].name, sizeof(source.graph_layout[4].name), "%s", title);
        candidate       = source;
        saved_candidate = candidate;
        TEST(! Sculptor::normalize_effect_layout(&candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        bool saved_collected[Synth::max_lfos];
        memcpy(saved_collected, collected, sizeof(collected));
        TEST(! Sculptor::collect_effect_lfos(&source, 0, collected));
        TEST(memcmp(collected, saved_collected, sizeof(collected)) == 0);
        TEST(! Sculptor::transfer_effect_layout(&source, 0, lfo_ids, 0, &candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        TEST(! Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::chorus, &candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        TEST(! Sculptor::change_effect_type_candidate(&source, 0, 0, Synth::EffectType::chorus, &source));
        TEST(memcmp(&source, &saved_candidate, sizeof(source)) == 0);
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        TEST(! Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
    }
}

static void effects_clipboard_regressions()
{
    Synth::EffectsDocument                             doc            = {};
    Synth::EffectsDocument                             decoded        = {};
    Synth::EffectsDocument                             saved_doc      = {};
    Synth::EffectsDocument                             extracted      = {};
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> saved_source_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       saved_source = *saved_source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> saved_candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       saved_candidate = *saved_candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       loaded         = *loaded_storage;
    const std::unique_ptr<FixedArray<char, 65536>>     text_storage   = std::make_unique<FixedArray<char, 65536>>();
    char (&text)[65536]                                               = text_storage->elements;
    const std::unique_ptr<FixedArray<char, 65536>> bank_text_storage  = std::make_unique<FixedArray<char, 65536>>();
    char (&bank_text)[65536]                                          = bank_text_storage->elements;
    const std::unique_ptr<FixedArray<char, 65536>> saved_text_storage = std::make_unique<FixedArray<char, 65536>>();
    char (&saved_text)[65536]                                         = saved_text_storage->elements;
    const std::unique_ptr<Sculptor::Graph> graph_storage              = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph                      = *graph_storage;
    const std::unique_ptr<Sculptor::Graph> saved_graph_storage        = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       saved_graph                = *saved_graph_storage;
    Sculptor::EffectGraphMapping           mapping                    = {};
    const std::unique_ptr<FixedArray<uint8_t, sizeof(source) * 4 + 256>> undo_storage_storage =
        std::make_unique<FixedArray<uint8_t, sizeof(source) * 4 + 256>>();
    uint8_t (&undo_storage)[sizeof(source) * 4 + 256] = undo_storage_storage->elements;
    Sculptor::UndoRedo undo;
    undo.init(undo_storage);

    // All four directions, same-chain replacement, local aliases, equal-distinct
    // descriptors, detached LFO layout, repeated effect types and empty slots.
    const uint32_t origins[] = { 0, Sculptor::fx_master_chain };
    for (uint32_t origin : origins) {
        effects_clipboard_fixture(&doc, true); // master documents may carry MIDI
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t id            = source.bank.lfos.allocate();
            source.bank.lfos.entries[id] = doc.lfos[i];
        }
        Sculptor::fx_graph_chain(&source.bank, origin) = doc.chain;
        source.bank.channel_chains[5]                  = doc.chain; // keeps original descriptors live
        strcpy(source.channel_names[5], "Untouched");
        saved_source = source;
        extracted    = {};
        TEST(Sculptor::extract_effect_chain_document(&source, origin, nullptr, nullptr, &extracted));
        TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
        TEST(extracted.chain.num_effects == 4);
        TEST(extracted.lfo_count == 2); // persisted source has no detached layout yet

        for (uint32_t destination : origins) {
            memset(static_cast<void*>(&candidate), 0, sizeof(candidate));
            const bool replaced = Sculptor::replace_effect_chain_candidate(&source, destination, &doc, &candidate);
            TEST(replaced);
            TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
            if (! replaced) {
                continue;
            }
            TEST(Synth::validate_instrument_bank(&candidate.bank));
            const Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&candidate.bank, destination);
            TEST(chain.num_effects == 4);
            TEST(chain.effects[1].type == Synth::EffectType::distortion);
            TEST(! chain.effects[2].enabled);
            const uint16_t alias    = chain.effects[0].bindings[0].lfo_desc_id;
            const uint16_t distinct = chain.effects[3].bindings[0].lfo_desc_id;
            TEST(alias > 3 && distinct > 3 && alias != distinct);
            TEST(chain.effects[2].bindings[0].lfo_desc_id == alias);
            TEST(candidate.bank.lfos.num_allocated == 6);
            TEST(memcmp(&candidate.bank.lfos.entries[alias - 1], &doc.lfos[0], sizeof(doc.lfos[0])) == 0);
            TEST(memcmp(&candidate.bank.lfos.entries[distinct - 1], &doc.lfos[1], sizeof(doc.lfos[1])) == 0);
            for (uint32_t s = 0; s < 4; ++s) {
                TEST(chain.effects[s].type == doc.chain.effects[s].type);
                for (uint32_t p = 0; p < Synth::get_effect_param_floats(chain.effects[s].type); ++p) {
                    const Synth::EffectParamBinding& b   = chain.effects[s].bindings[p];
                    const Synth::EffectParamBinding& old = doc.chain.effects[s].bindings[p];
                    TEST(b.base_value == old.base_value && b.lfo_depth == old.lfo_depth);
                    TEST(b.lfo_op == old.lfo_op && b.lfo_rate_scale == old.lfo_rate_scale);
                    TEST(b.num_inputs == old.num_inputs);
                    TEST(memcmp(b.inputs, old.inputs, sizeof(b.inputs)) == 0);
                    TEST(b.lfo_depth_source == old.lfo_depth_source && b.lfo_rate_source == old.lfo_rate_source);
                }
            }
            TEST(memcmp(&candidate.bank.channel_chains[5], &source.bank.channel_chains[5], sizeof(doc.chain)) == 0);
            TEST(memcmp(candidate.bank.lfos.entries, source.bank.lfos.entries, 3 * sizeof(doc.lfos[0])) == 0);
            TEST(memcmp(candidate.channel_names, source.channel_names, sizeof(source.channel_names)) == 0);
            TEST(memcmp(candidate.instrument_names, source.instrument_names, sizeof(source.instrument_names)) == 0);
            TEST(memcmp(candidate.bank.instruments.entries,
                        source.bank.instruments.entries,
                        sizeof(source.bank.instruments.entries)) == 0);
            TEST(Sculptor::extract_effect_chain_document(&candidate, destination, nullptr, nullptr, &extracted));
            TEST(extracted.lfo_count == 3);
            TEST(extracted.graph_layout_count == doc.graph_layout_count);
            for (uint32_t i = 0; i < doc.graph_layout_count; ++i) {
                TEST(memcmp(&extracted.graph_layout[i], &doc.graph_layout[i], sizeof(doc.graph_layout[0])) == 0);
            }
            // Existing bank codec must retain the new identities and sparse layout.
            const uint32_t n = Synth::encode_editor_bank_json(&candidate, bank_text, sizeof(bank_text));
            TEST(n > 0);
            TEST(Synth::decode_editor_bank_json(bank_text, n, &loaded));
            TEST(Sculptor::extract_effect_chain_document(&loaded, destination, nullptr, nullptr, &decoded));
            TEST(memcmp(&decoded, &extracted, sizeof(decoded)) == 0);

            // Existing whole-bank snapshot primitive, one entry for replacement.
            undo.clear();
            undo.init_undo_push();
            undo.push(&source, sizeof(source));
            TEST(undo.finish_undo_push());
            saved_candidate = candidate;
            undo.init_redo_push();
            undo.push(&candidate, sizeof(candidate));
            TEST(undo.finish_redo_push());
            TEST(undo.init_undo());
            undo.pop(&candidate, sizeof(candidate));
            undo.finish_undo();
            TEST(memcmp(&candidate, &source, sizeof(source)) == 0 && undo.undo_empty());
            TEST(undo.init_redo());
            undo.pop(&candidate, sizeof(candidate));
            undo.finish_redo();
            TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0 && undo.redo_empty());
        }
    }

    // Copy observes graph edits, but neither consumes pending events nor mutates bank.
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    effects_clipboard_fixture(&doc, true);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t id            = source.bank.lfos.allocate();
        source.bank.lfos.entries[id] = doc.lfos[i];
    }
    source.bank.channel_chains[0] = doc.chain;
    bool pinned[Synth::max_lfos]  = {};
    pinned[2]                     = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned));
    Sculptor::GraphChange discarded[Sculptor::max_pending_changes];
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    Sculptor::PropertyValue value = {};
    value.real                    = -888.0f;
    graph.set_slot_value(mapping.effect_nodes[0], Sculptor::fx_param_row(0), value);
    graph.set_node_layout(mapping.lfo_nodes[2], { -320.0f, 96.0f }, 290.0f, 210.0f);
    saved_graph  = graph;
    saved_source = source;
    TEST(Sculptor::extract_effect_chain_document(&source, 0, &graph, &mapping, &extracted));
    TEST(extracted.chain.effects[0].bindings[0].base_value == -888.0f);
    TEST(extracted.lfo_count == 3);
    bool found_detached = false;
    for (uint32_t i = 0; i < extracted.graph_layout_count; ++i) {
        const Synth::EffectsGraphLayout& layout = extracted.graph_layout[i];
        if (layout.kind == Synth::effects_graph_layout_lfo && layout.index == 3) {
            found_detached = layout.x == -320.0f && layout.y == 96.0f && layout.width_override == 290.0f &&
                             layout.height_override == 210.0f;
        }
    }
    TEST(found_detached);
    TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);
    TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);

    // Bounded codec accepts each real effect, empty documents and exact capacity.
    for (uint32_t type = 1; type < Synth::num_effect_types; ++type) {
        effects_clipboard_fixture(&doc, true);
        doc.chain.effects[0].type = static_cast<Synth::EffectType>(type);
        if (type == static_cast<uint32_t>(Synth::EffectType::fir) ||
            type == static_cast<uint32_t>(Synth::EffectType::fir_multiband)) {
            // The retyped slot keeps the delay fixture's fractional third
            // binding; either FIR type's legacy slope needs a valid integer base.
            doc.chain.effects[0].bindings[2].base_value = 100.0f;
        }
        const uint32_t n = Synth::encode_effects_json(&doc, text, sizeof(text));
        TEST(n > 0);
        TEST(Synth::decode_effects_json(text, n, &decoded));
        if (n) {
            TEST(Synth::encode_effects_json(&doc, bank_text, n + 1) == n);
            TEST(Synth::encode_effects_json(&doc, bank_text, n) == 0);
            TEST(decoded.lfo_count == 3 && decoded.graph_layout_count == 9);
            TEST(decoded.chain.effects[0].type == doc.chain.effects[0].type);
        }
    } // FIR slope carrier tests: the third FIR binding round-trips through
    // the editor bank dialect and the strict effects document, fractional
    // and out-of-range slope tokens refuse the whole document
    // pre-narrowing, a 2-param FIR document fails decode in both carriers,
    // and an absent or modulation-only third base decodes as 100.
    {
        effects_clipboard_fixture(&doc, true);
        doc.chain.effects[0].type                   = Synth::EffectType::fir;
        doc.chain.effects[0].bindings[0]            = {};
        doc.chain.effects[0].bindings[1]            = {};
        doc.chain.effects[0].bindings[2]            = {};
        doc.chain.effects[0].bindings[0].base_value = 8000.0f;
        doc.chain.effects[0].bindings[2].base_value = 37.0f;
        const uint32_t n                            = Synth::encode_effects_json(&doc, text, sizeof(text));
        TEST(n > 0);
        TEST(strstr(text, "{\"base_value\":37}") != nullptr);
        TEST(Synth::decode_effects_json(text, n, &decoded));
        TEST(decoded.chain.effects[0].type == Synth::EffectType::fir);
        TEST(decoded.chain.effects[0].bindings[2].base_value == 37.0f);
        TEST(Synth::encode_effects_json(&decoded, bank_text, sizeof(bank_text)) == n);
        // The canonical re-encode of an absent base writes the explicit
        // default: a spliced-out third base decodes as 100 and round-trips.
        auto decode_with_effect_slope = [](const char* third_binding_json) {
            Synth::EffectsDocument slope_doc = {};
            effects_clipboard_fixture(&slope_doc, true);
            slope_doc.chain.effects[0].type                   = Synth::EffectType::fir;
            slope_doc.chain.effects[0].bindings[0]            = {};
            slope_doc.chain.effects[0].bindings[1]            = {};
            slope_doc.chain.effects[0].bindings[2]            = {};
            slope_doc.chain.effects[0].bindings[0].base_value = 8000.0f;
            slope_doc.chain.effects[0].bindings[2].base_value = 37.0f;
            const std::unique_ptr<FixedArray<char, 65536>> encoded_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&encoded)[65536] = encoded_storage->elements;
            const uint32_t len     = Synth::encode_effects_json(&slope_doc, encoded, sizeof(encoded));
            TEST(len > 0);
            const char* const                              obj = "{\"base_value\":37}";
            const std::unique_ptr<FixedArray<char, 65536>> spliced_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&spliced)[65536] = spliced_storage->elements;
            const uint32_t spliced_len =
                splice_single_occurrence(encoded, len, obj, third_binding_json, spliced, sizeof(spliced));
            TEST(spliced_len > 0);
            if (! spliced_len) {
                return false;
            }
            Synth::EffectsDocument out = {};
            return Synth::decode_effects_json(spliced, spliced_len, &out);
        };
        TEST(decode_with_effect_slope("{\"base_value\":37}")); // the splice itself decodes
        TEST(! decode_with_effect_slope("{\"base_value\":0}"));
        TEST(! decode_with_effect_slope("{\"base_value\":101}"));
        TEST(! decode_with_effect_slope("{\"base_value\":50.5}"));
        TEST(! decode_with_effect_slope("{\"base_value\":1e2}"));
        // An absent base value is a success row: the third binding decodes
        // as the 100 default and the canonical re-encode emits it.
        {
            Synth::EffectsDocument absent = {};
            effects_clipboard_fixture(&absent, true);
            absent.chain.effects[0].type                   = Synth::EffectType::fir;
            absent.chain.effects[0].bindings[0]            = {};
            absent.chain.effects[0].bindings[1]            = {};
            absent.chain.effects[0].bindings[2]            = {};
            absent.chain.effects[0].bindings[0].base_value = 8000.0f;
            absent.chain.effects[0].bindings[2].base_value = 37.0f;
            const std::unique_ptr<FixedArray<char, 65536>> encoded_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&encoded)[65536] = encoded_storage->elements;
            const uint32_t len     = Synth::encode_effects_json(&absent, encoded, sizeof(encoded));
            TEST(len > 0);
            const char* const                              obj = "{\"base_value\":37}";
            const std::unique_ptr<FixedArray<char, 65536>> spliced_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&spliced)[65536]     = spliced_storage->elements;
            const uint32_t spliced_len = splice_single_occurrence(encoded, len, obj, "{}", spliced, sizeof(spliced));
            TEST(spliced_len > 0);
            Synth::EffectsDocument out = {};
            TEST(Synth::decode_effects_json(spliced, spliced_len, &out));
            TEST(out.chain.effects[0].bindings[2].base_value == 100.0f);
            TEST(Synth::encode_effects_json(&out, text, sizeof(text)) > 0);
            TEST(strstr(text, "\"base_value\":100") != nullptr);
        }
        // A modulation-only third binding keeps the default base and the
        // supplied modulation fields.
        {
            Synth::EffectsDocument modulated = {};
            effects_clipboard_fixture(&modulated, true);
            modulated.chain.effects[0].type                   = Synth::EffectType::fir;
            modulated.chain.effects[0].bindings[0]            = {};
            modulated.chain.effects[0].bindings[1]            = {};
            modulated.chain.effects[0].bindings[2]            = {};
            modulated.chain.effects[0].bindings[0].base_value = 8000.0f;
            modulated.chain.effects[0].bindings[2].base_value = 37.0f;
            const std::unique_ptr<FixedArray<char, 65536>> encoded_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&encoded)[65536] = encoded_storage->elements;
            const uint32_t len     = Synth::encode_effects_json(&modulated, encoded, sizeof(encoded));
            TEST(len > 0);
            const char* const                              obj  = "{\"base_value\":37}";
            const char* const                              repl = "{\"lfo_desc_id\":1,\"lfo_depth\":0.5}";
            const std::unique_ptr<FixedArray<char, 65536>> spliced_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&spliced)[65536]     = spliced_storage->elements;
            const uint32_t spliced_len = splice_single_occurrence(encoded, len, obj, repl, spliced, sizeof(spliced));
            TEST(spliced_len > 0);
            Synth::EffectsDocument out = {};
            TEST(Synth::decode_effects_json(spliced, spliced_len, &out));
            TEST(out.chain.effects[0].bindings[2].base_value == 100.0f);
            TEST(out.chain.effects[0].bindings[2].lfo_desc_id == 1);
            TEST(out.chain.effects[0].bindings[2].lfo_depth == 0.5f);
        }
        // A 2-param FIR effect document fails decode: the params array must
        // cover exactly the fields the effect type reads.
        {
            Synth::EffectsDocument two_param = {};
            effects_clipboard_fixture(&two_param, true);
            two_param.chain.effects[0].type                   = Synth::EffectType::fir;
            two_param.chain.effects[0].bindings[0]            = {};
            two_param.chain.effects[0].bindings[1]            = {};
            two_param.chain.effects[0].bindings[2]            = {};
            two_param.chain.effects[0].bindings[0].base_value = 8000.0f;
            two_param.chain.effects[0].bindings[2].base_value = 37.0f;
            const std::unique_ptr<FixedArray<char, 65536>> encoded_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&encoded)[65536] = encoded_storage->elements;
            const uint32_t len     = Synth::encode_effects_json(&two_param, encoded, sizeof(encoded));
            TEST(len > 0);
            const char* const                              first_binding = "{\"base_value\":8000},";
            const std::unique_ptr<FixedArray<char, 65536>> spliced_storage =
                std::make_unique<FixedArray<char, 65536>>();
            char (&spliced)[65536] = spliced_storage->elements;
            const uint32_t spliced_len =
                splice_single_occurrence(encoded, len, first_binding, "", spliced, sizeof(spliced));
            TEST(spliced_len > 0);
            Synth::EffectsDocument out = {};
            TEST(! Synth::decode_effects_json(spliced, spliced_len, &out));
        }
        // The editor bank dialect carrier: a FIR effect with a nondefault
        // slope round-trips, bad slope spellings refuse the whole bank, and
        // a 2-param FIR effect refuses the bank too.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& bank = *bank_storage;
            Synth::init_default_bank(&bank.bank);
            Sculptor::set_identity_osc_sources(&bank, 0, 0);
            enabled_effect(bank.bank, 0, 0, Synth::EffectType::fir);
            bank.bank.channel_chains[0].effects[0].bindings[0].base_value = 8000.0f;
            bank.bank.channel_chains[0].effects[0].bindings[2].base_value = 37.0f;
            TEST(Synth::validate_instrument_bank(&bank.bank));
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&payload)[Synth::library_payload_max] = payload_storage->elements;
            const uint32_t payload_len = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
            TEST(payload_len > 0);
            TEST(strstr(payload, "{\"base_value\":37}") != nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> decoded_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& decoded_bank = *decoded_bank_storage;
            memset(static_cast<void*>(&decoded_bank), 0, sizeof(decoded_bank));
            TEST(Synth::decode_editor_bank_json(payload, payload_len, &decoded_bank));
            TEST(decoded_bank.bank.channel_chains[0].effects[0].type == Synth::EffectType::fir);
            TEST(decoded_bank.bank.channel_chains[0].effects[0].bindings[2].base_value == 37.0f);
            auto decode_with_bank_slope = [](const char* third_binding_json) {
                const std::unique_ptr<Synth::InstrumentEditorBank> slope_bank_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& slope_bank = *slope_bank_storage;
                Synth::init_default_bank(&slope_bank.bank);
                Sculptor::set_identity_osc_sources(&slope_bank, 0, 0);
                enabled_effect(slope_bank.bank, 0, 0, Synth::EffectType::fir);
                slope_bank.bank.channel_chains[0].effects[0].bindings[0].base_value = 8000.0f;
                slope_bank.bank.channel_chains[0].effects[0].bindings[2].base_value = 37.0f;
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> bank_payload_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&bank_payload)[Synth::library_payload_max] = bank_payload_storage->elements;
                const uint32_t bank_len =
                    Synth::encode_editor_bank_json(&slope_bank, bank_payload, sizeof(bank_payload));
                TEST(bank_len > 0);
                const char* const                                                   obj = "{\"base_value\":37}";
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spliced_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spliced)[Synth::library_payload_max] = spliced_storage->elements;
                const uint32_t spliced_len =
                    splice_single_occurrence(bank_payload, bank_len, obj, third_binding_json, spliced, sizeof(spliced));
                TEST(spliced_len > 0);
                if (! spliced_len) {
                    return false;
                }
                const std::unique_ptr<Synth::InstrumentEditorBank> out_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& out = *out_storage;
                memset(static_cast<void*>(&out), 0, sizeof(out));
                return Synth::decode_editor_bank_json(spliced, spliced_len, &out);
            };
            TEST(decode_with_bank_slope("{\"base_value\":37}"));
            TEST(! decode_with_bank_slope("{\"base_value\":0}"));
            TEST(! decode_with_bank_slope("{\"base_value\":101}"));
            TEST(! decode_with_bank_slope("{\"base_value\":50.5}"));
            TEST(! decode_with_bank_slope("{\"base_value\":1e2}"));
            TEST(decode_with_bank_slope("{}"));
            // Dropping the first binding leaves a 2-param FIR effect; the lenient bank
            // path default-fills the missing trailing slope and accepts it.
            {
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> bank_payload_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&bank_payload)[Synth::library_payload_max] = bank_payload_storage->elements;
                const uint32_t bank_len = Synth::encode_editor_bank_json(&bank, bank_payload, sizeof(bank_payload));
                TEST(bank_len > 0);
                const char* const first_binding = "{\"base_value\":8000},";
                const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> spliced_storage =
                    std::make_unique<FixedArray<char, Synth::library_payload_max>>();
                char (&spliced)[Synth::library_payload_max] = spliced_storage->elements;
                const uint32_t spliced_len =
                    splice_single_occurrence(bank_payload, bank_len, first_binding, "", spliced, sizeof(spliced));
                TEST(spliced_len > 0);
                const std::unique_ptr<Synth::InstrumentEditorBank> out_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& out = *out_storage;
                memset(static_cast<void*>(&out), 0, sizeof(out));
                TEST(Synth::decode_editor_bank_json(spliced, spliced_len, &out));
            }
        }
        // ring_mod: distinctive rate/mix values must survive the bank codec
        // round-trip, and the encoded type name must appear verbatim.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& bank = *bank_storage;
            Synth::init_default_bank(&bank.bank);
            Sculptor::set_identity_osc_sources(&bank, 0, 0);
            enabled_effect(bank.bank, 0, 0, Synth::EffectType::ring_mod);
            bank.bank.channel_chains[0].effects[0].bindings[0].base_value = 234.5f;
            bank.bank.channel_chains[0].effects[0].bindings[1].base_value = 0.75f;
            TEST(Synth::validate_instrument_bank(&bank.bank));
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&payload)[Synth::library_payload_max] = payload_storage->elements;
            const uint32_t payload_len = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
            TEST(payload_len > 0);
            TEST(strstr(payload, "\"ring_mod\"") != nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> decoded_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& decoded_bank = *decoded_bank_storage;
            memset(static_cast<void*>(&decoded_bank), 0, sizeof(decoded_bank));
            TEST(Synth::decode_editor_bank_json(payload, payload_len, &decoded_bank));
            TEST(decoded_bank.bank.channel_chains[0].effects[0].type == Synth::EffectType::ring_mod);
            TEST(decoded_bank.bank.channel_chains[0].effects[0].bindings[0].base_value == 234.5f);
            TEST(decoded_bank.bank.channel_chains[0].effects[0].bindings[1].base_value == 0.75f);
        }
        // ring_mod oscillator mode: distinctive values must survive BOTH the
        // bank codec and the clipboard instrument codec round-trip, and a
        // keys-reordered document must decode identically.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> mode_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& mode_bank = *mode_bank_storage;
            Synth::init_default_bank(&mode_bank.bank);
            Sculptor::set_identity_osc_sources(&mode_bank, 0, 0);
            Synth::Oscillator& mode_osc                     = mode_bank.bank.instruments.entries[0].layers[0];
            mode_osc.osc_mode                               = Synth::osc_mode_ring_mod;
            mode_osc.mod_ratio                              = 1.5f;
            mode_osc.osc_type[1]                            = Synth::WaveType::sawtooth_wave;
            mode_osc.targets[Synth::mod_duty1].base_value   = 0.25f;
            mode_osc.targets[Synth::mod_osc_mix].base_value = 0.75f;
            TEST(Synth::validate_instrument_bank(&mode_bank.bank));

            // Bank codec: the encoded mode name must appear verbatim and
            // every distinctive value must survive the round-trip.
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> mode_payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&mode_payload)[Synth::library_payload_max] = mode_payload_storage->elements;
            const uint32_t mode_payload_len =
                Synth::encode_editor_bank_json(&mode_bank, mode_payload, sizeof(mode_payload));
            TEST(mode_payload_len > 0);
            TEST(strstr(mode_payload, "\"ring_mod\"") != nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> mode_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& mode_decoded = *mode_decoded_storage;
            memset(static_cast<void*>(&mode_decoded), 0, sizeof(mode_decoded));
            TEST(Synth::decode_editor_bank_json(mode_payload, mode_payload_len, &mode_decoded));
            const Synth::Oscillator& mode_back = mode_decoded.bank.instruments.entries[0].layers[0];
            TEST(mode_back.osc_mode == Synth::osc_mode_ring_mod);
            TEST(mode_back.mod_ratio == 1.5f);
            TEST(mode_back.osc_type[1] == Synth::WaveType::sawtooth_wave);
            TEST(mode_back.targets[Synth::mod_duty1].base_value == 0.25f);
            TEST(mode_back.targets[Synth::mod_osc_mix].base_value == 0.75f);

            // Clipboard codec: the same fixture through the instrument
            // document codec.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> clip_text_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&clip_text)[clipboard_capacity] = clip_text_storage->elements;
            TEST(export_clipboard_fixture(mode_bank, clip_text, sizeof(clip_text)));
            const std::unique_ptr<ClipboardDecode> clip_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       clip         = *clip_storage;
            memset(static_cast<void*>(&clip), 0, sizeof(clip));
            TEST(decode_clipboard(&clip, clip_text, max_portable_records));
            TEST(clip.instr.layers[0].osc_mode == Synth::osc_mode_ring_mod);
            TEST(clip.instr.layers[0].mod_ratio == 1.5f);
            TEST(clip.instr.layers[0].osc_type[1] == Synth::WaveType::sawtooth_wave);
            TEST(clip.instr.layers[0].targets[Synth::mod_duty1].base_value == 0.25f);
            TEST(clip.instr.layers[0].targets[Synth::mod_osc_mix].base_value == 0.75f);

            // Key-order independence: the mode key may follow the target
            // fields and still decode to the same layer.
            const char* const reordered_doc =
                "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"duty_b\":{\"base\":0.25},"
                "\"osc_mix\":{\"base\":0.75},\"mode\":\"ring_mod\",\"wave_b\":\"sawtooth\",\"mod_ratio\":1.5}]}";
            const std::unique_ptr<ClipboardDecode> reordered_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       reordered         = *reordered_storage;
            memset(static_cast<void*>(&reordered), 0, sizeof(reordered));
            TEST(decode_clipboard(&reordered, reordered_doc, max_portable_records));
            TEST(reordered.instr.layers[0].osc_mode == Synth::osc_mode_ring_mod);
            TEST(reordered.instr.layers[0].mod_ratio == 1.5f);
            TEST(reordered.instr.layers[0].osc_type[1] == Synth::WaveType::sawtooth_wave);
            TEST(reordered.instr.layers[0].targets[Synth::mod_duty1].base_value == 0.25f);
            TEST(reordered.instr.layers[0].targets[Synth::mod_osc_mix].base_value == 0.75f);
        }
        // fm_ratio: the hookable FM Ratio target round-trips through both
        // codecs with distinctive base and depth, a legacy layer that
        // states only the scalar ratio backfills the new target's base on
        // both decode paths (the layer and the phantom shell), and key
        // order does not matter.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> ratio_bank_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& ratio_bank = *ratio_bank_storage;
            Synth::init_default_bank(&ratio_bank.bank);
            Sculptor::set_identity_osc_sources(&ratio_bank, 0, 0);
            const uint32_t ratio_lfo_slot = ratio_bank.bank.lfos.allocate();
            TEST(ratio_lfo_slot != pool_no_slot);
            const uint16_t        ratio_lfo_id                 = static_cast<uint16_t>(ratio_lfo_slot + 1);
            Synth::LFODescriptor& ratio_lfo                    = ratio_bank.bank.lfos.entries[ratio_lfo_slot];
            ratio_lfo.wave                                     = Synth::WaveType::sine_wave;
            ratio_lfo.duty                                     = 0x40;
            ratio_lfo.period_ms                                = 250;
            ratio_lfo.min_value                                = -1.0f;
            ratio_lfo.min_max_delta                            = 2.0f;
            Synth::Oscillator& ratio_osc                       = ratio_bank.bank.instruments.entries[0].layers[0];
            ratio_osc.osc_mode                                 = Synth::osc_mode_fm;
            ratio_osc.targets[Synth::mod_fm_ratio].base_value  = 1.5f;
            ratio_osc.targets[Synth::mod_fm_ratio].lfo_desc_id = ratio_lfo_id;
            ratio_osc.targets[Synth::mod_fm_ratio].lfo_op      = Synth::SourceOp::add;
            ratio_osc.targets[Synth::mod_fm_ratio].lfo_depth   = 0.5f;
            TEST(Synth::validate_instrument_bank(&ratio_bank.bank));
            // Bank codec: the encoded target name must appear verbatim and
            // the whole binding survives the round-trip.
            const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> ratio_payload_storage =
                std::make_unique<FixedArray<char, Synth::library_payload_max>>();
            char (&ratio_payload)[Synth::library_payload_max] = ratio_payload_storage->elements;
            const uint32_t ratio_payload_len =
                Synth::encode_editor_bank_json(&ratio_bank, ratio_payload, sizeof(ratio_payload));
            TEST(ratio_payload_len > 0);
            TEST(strstr(ratio_payload, "\"fm_ratio\"") != nullptr);
            const std::unique_ptr<Synth::InstrumentEditorBank> ratio_decoded_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& ratio_decoded = *ratio_decoded_storage;
            memset(static_cast<void*>(&ratio_decoded), 0, sizeof(ratio_decoded));
            TEST(Synth::decode_editor_bank_json(ratio_payload, ratio_payload_len, &ratio_decoded));
            const Synth::Oscillator& ratio_back = ratio_decoded.bank.instruments.entries[0].layers[0];
            TEST(ratio_back.targets[Synth::mod_fm_ratio].base_value == 1.5f);
            TEST(ratio_back.targets[Synth::mod_fm_ratio].lfo_desc_id == ratio_lfo_id);
            TEST(ratio_back.targets[Synth::mod_fm_ratio].lfo_depth == 0.5f);
            // Clipboard codec: the same fixture through the instrument
            // document codec.
            const std::unique_ptr<FixedArray<char, clipboard_capacity>> ratio_clip_storage =
                std::make_unique<FixedArray<char, clipboard_capacity>>();
            char (&ratio_clip)[clipboard_capacity] = ratio_clip_storage->elements;
            TEST(export_clipboard_fixture(ratio_bank, ratio_clip, sizeof(ratio_clip)));
            const std::unique_ptr<ClipboardDecode> ratio_clip_decoded_storage = std::make_unique<ClipboardDecode>();
            ClipboardDecode&                       ratio_clip_decoded         = *ratio_clip_decoded_storage;
            memset(static_cast<void*>(&ratio_clip_decoded), 0, sizeof(ratio_clip_decoded));
            TEST(decode_clipboard(&ratio_clip_decoded, ratio_clip, max_portable_records));
            TEST(ratio_clip_decoded.lfo_count == 1);
            TEST(ratio_clip_decoded.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 1.5f);
            TEST(ratio_clip_decoded.instr.layers[0].targets[Synth::mod_fm_ratio].lfo_desc_id == ratio_lfo_id);
            TEST(ratio_clip_decoded.instr.layers[0].targets[Synth::mod_fm_ratio].lfo_depth == 0.5f);
            // Legacy backfill, bank codec: a layer that only states the
            // scalar ratio decodes with the fm_ratio base seeded from it.
            {
                // Hand-written legacy payloads state no fm_ratio routing at all, so
                // the decode genuinely takes the backfill branch (the encoder always
                // emits the entry, which would mask the branch).
                const char* const legacy_doc = "{\"instrument_editor_bank\":{\"instruments\":"
                                               "[{\"layer_count\":1,\"layers\":[{\"wave_a\":\"sine\",\"mod_ratio\":2,"
                                               "\"routing\":{}}]}],"
                                               "\"channels\":[{},{},{},{\"zones\":[{\"start\":0,\"instrument\":0}]}]}}";
                const uint32_t    legacy_len = static_cast<uint32_t>(strlen(legacy_doc));
                const std::unique_ptr<Synth::InstrumentEditorBank> legacy_decoded_storage =
                    std::make_unique<Synth::InstrumentEditorBank>();
                Synth::InstrumentEditorBank& legacy_decoded = *legacy_decoded_storage;
                memset(static_cast<void*>(&legacy_decoded), 0, sizeof(legacy_decoded));
                // Absent source row: the document carries no osc_sources, so
                // the decode replaces a pre-filled destination row with an
                // all-zero row.
                for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
                    legacy_decoded.graph_osc_source[3][0][slot] = static_cast<uint8_t>(slot + 1);
                }
                TEST(Synth::decode_editor_bank_json(legacy_doc, legacy_len, &legacy_decoded));
                const Synth::Oscillator& legacy_back = legacy_decoded.bank.instruments.entries[0].layers[0];
                TEST(legacy_back.mod_ratio == 2.0f);
                TEST(legacy_back.targets[Synth::mod_fm_ratio].base_value == 2.0f);
                for (uint32_t slot = 0; slot < Synth::max_layers; ++slot) {
                    TEST(legacy_decoded.graph_osc_source[3][0][slot] == 0);
                }
            }
            // Legacy backfill, clipboard dialect: a hand-written layer
            // without an fm_ratio target decodes with the fm_ratio base
            // seeded from the legacy ratio.  An omitted ratio keeps the
            // legacy default one; an explicit zero keeps the zero (the
            // runtime clamp is not codec behavior).  Key order does not
            // matter: an explicit fm_ratio block stays authoritative when
            // its key and the ratio key follow the other fields.
            {
                const char* const ratio_absent_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\"}]}";
                const std::unique_ptr<ClipboardDecode> ratio_absent_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ratio_absent         = *ratio_absent_storage;
                memset(static_cast<void*>(&ratio_absent), 0, sizeof(ratio_absent));
                TEST(decode_clipboard(&ratio_absent, ratio_absent_doc, max_portable_records));
                TEST(ratio_absent.instr.layers[0].mod_ratio == 1.0f);
                TEST(ratio_absent.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 1.0f);
                const char* const ratio_three_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mod_ratio\":3}]}";
                const std::unique_ptr<ClipboardDecode> ratio_three_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ratio_three         = *ratio_three_storage;
                memset(static_cast<void*>(&ratio_three), 0, sizeof(ratio_three));
                TEST(decode_clipboard(&ratio_three, ratio_three_doc, max_portable_records));
                TEST(ratio_three.instr.layers[0].mod_ratio == 3.0f);
                TEST(ratio_three.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 3.0f);
                const char* const ratio_zero_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"mod_ratio\":0}]}";
                const std::unique_ptr<ClipboardDecode> ratio_zero_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ratio_zero         = *ratio_zero_storage;
                memset(static_cast<void*>(&ratio_zero), 0, sizeof(ratio_zero));
                TEST(decode_clipboard(&ratio_zero, ratio_zero_doc, max_portable_records));
                TEST(ratio_zero.instr.layers[0].mod_ratio == 0.0f);
                TEST(ratio_zero.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 0.0f);
                const char* const ratio_reordered_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\",\"fm_ratio\":{\"base\":1.5},"
                    "\"mode\":\"fm\",\"mod_ratio\":1.5}]}";
                const std::unique_ptr<ClipboardDecode> ratio_reordered_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ratio_reordered         = *ratio_reordered_storage;
                memset(static_cast<void*>(&ratio_reordered), 0, sizeof(ratio_reordered));
                TEST(decode_clipboard(&ratio_reordered, ratio_reordered_doc, max_portable_records));
                TEST(ratio_reordered.instr.layers[0].mod_ratio == 1.5f);
                TEST(ratio_reordered.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 1.5f);
                // Regression: an explicit fm_ratio block stays authoritative when an
                // ordinary target key follows it; the presence flag never resets
                // while the layer object is walked.
                const char* const ratio_after_target_doc =
                    "{\"format\":\"synth-instrument-v1\",\"layers\":[{\"wave_a\":\"sine\","
                    "\"mod_ratio\":3,\"fm_ratio\":{\"base\":1.5},\"volume\":{\"base\":1.0}}]}";
                const std::unique_ptr<ClipboardDecode> ratio_after_target_storage = std::make_unique<ClipboardDecode>();
                ClipboardDecode&                       ratio_after_target         = *ratio_after_target_storage;
                memset(static_cast<void*>(&ratio_after_target), 0, sizeof(ratio_after_target));
                TEST(decode_clipboard(&ratio_after_target, ratio_after_target_doc, max_portable_records));
                TEST(ratio_after_target.instr.layers[0].mod_ratio == 3.0f);
                TEST(ratio_after_target.instr.layers[0].targets[Synth::mod_fm_ratio].base_value == 1.5f);
            }
        }
    }

    // The authored example uses the production codec, including aliases and a layout-only LFO.
    FILE* example_file = fopen("doc/synth_effects_example.json", "rb");
    TEST(example_file != nullptr);
    if (example_file) {
        const uint32_t length = static_cast<uint32_t>(fread(text, 1, sizeof(text), example_file));
        TEST(fclose(example_file) == 0);
        TEST(Synth::decode_effects_json(text, length, &decoded));
        TEST(decoded.chain.num_effects == 2 && decoded.lfo_count == 2); // the v3 doc's dormant slot compacts away
        TEST(decoded.chain.effects[0].bindings[1].lfo_desc_id == decoded.chain.effects[1].bindings[0].lfo_desc_id);
        TEST(Synth::encode_effects_json(&decoded, bank_text, sizeof(bank_text)) > 0);
    }

    // Library's combined descriptor map preserves oscillator/effect sharing and represented unconnected nodes.
    effects_clipboard_fixture(&doc, true);
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    TEST(Sculptor::replace_effect_chain_candidate(&source, 0, &doc, &candidate));
    source = candidate;
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id =
        source.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id;
    saved_source             = source;
    const char* library_path = "synth_effects_clipboard_library.tmp";
    remove(library_path);
    TEST(Synth::save_library_record(library_path, "Effects", "Aliases", &source, 0) == 0);
    Synth::LibraryEntry      entry          = {};
    Synth::LibraryScanStatus library_status = Synth::library_invalid;
    TEST(Synth::read_library_index(library_path, &entry, 1, &library_status) == 1);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    uint16_t first_slot = 0xBEEF;
    TEST(Synth::load_library_instrument(library_path, &entry, &loaded, 2, &first_slot));
    TEST(loaded.bank.instruments.entries[first_slot].layers[0].targets[Synth::mod_volume].lfo_desc_id ==
         loaded.bank.channel_chains[2].effects[0].bindings[0].lfo_desc_id);
    TEST(Sculptor::extract_effect_chain_document(&loaded, 2, nullptr, nullptr, &decoded));
    TEST(decoded.lfo_count == 3 && decoded.graph_layout_count == 9);
    TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
    remove(library_path);

    // Library reduction keeps sparse original IDs ordered and never deduplicates equal descriptors.
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    for (uint32_t index = 0; index < 6; ++index) {
        uint16_t allocated = 0;
        TEST(Sculptor::allocate_default_lfo(&source.bank, &allocated));
    }
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id = 3;
    source.bank.channel_chains[0].num_effects                                           = 1;
    Sculptor::fx_init_slot(&source.bank.channel_chains[0], 0, Synth::EffectType::distortion);
    source.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 3;
    source.bank.channel_chains[0].effects[0].bindings[1].lfo_desc_id = 5;
    source.graph_layout_count                                        = 1;
    source.graph_layout[0]                                           = {};
    source.graph_layout[0].kind                                      = 4;
    strcpy(source.graph_layout[0].name, "LFO 6");
    saved_source = source;
    TEST(Synth::save_library_record(library_path, "Effects", "Sparse", &source, 0) == 0);
    TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
    TEST(Synth::read_library_index(library_path, &entry, 1, &library_status) == 1);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    uint16_t occupied_descriptor = 0;
    TEST(Sculptor::allocate_default_lfo(&loaded.bank, &occupied_descriptor));
    TEST(Synth::load_library_instrument(library_path, &entry, &loaded, 2, &first_slot));
    TEST(loaded.bank.lfos.num_allocated == 4);
    TEST(loaded.bank.instruments.entries[first_slot].layers[0].targets[Synth::mod_volume].lfo_desc_id == 2);
    TEST(loaded.bank.channel_chains[2].effects[0].bindings[0].lfo_desc_id == 2);
    TEST(loaded.bank.channel_chains[2].effects[0].bindings[1].lfo_desc_id == 3);
    TEST(memcmp(&loaded.bank.lfos.entries[1], &loaded.bank.lfos.entries[2], sizeof(Synth::LFODescriptor)) == 0);
    bool library_roots[Synth::max_lfos] = {};
    TEST(Sculptor::collect_effect_lfos(&loaded, 2, library_roots));
    TEST(library_roots[1] && library_roots[2] && library_roots[3]);
    for (uint32_t index = loaded.bank.lfos.num_allocated; index < Synth::max_lfos; ++index) {
        TEST(Sculptor::allocate_default_lfo(&loaded.bank, &occupied_descriptor));
    }
    saved_candidate = loaded;
    first_slot      = 0xBEEF;
    TEST(! Synth::load_library_instrument(library_path, &entry, &loaded, 2, &first_slot));
    TEST(first_slot == 0xBEEF && memcmp(&loaded, &saved_candidate, sizeof(loaded)) == 0);
    remove(library_path);
    // Restore the shared-descriptor fixture for pending-edit checks.
    memset(static_cast<void*>(&source), 0, sizeof(source));
    Synth::init_default_bank(&source.bank);
    Sculptor::set_identity_osc_sources(&source, 0, 0);
    TEST(Sculptor::replace_effect_chain_candidate(&source, 0, &doc, &source));
    source.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id = 1;
    saved_source                                                                        = source;

    // Pending edits to a shared descriptor affect every owner in the successful private candidate only.
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    value         = {};
    value.integer = 777;
    graph.set_slot_value(mapping.lfo_nodes[0], 3, value);
    saved_graph = graph;
    TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    TEST(candidate.bank.lfos.entries[0].period_ms == 777);
    TEST(Sculptor::replace_effect_chain_candidate(&candidate, 0, &doc, &loaded));
    TEST(loaded.bank.instruments.entries[0].layers[0].targets[Synth::mod_volume].lfo_desc_id == 1);
    TEST(loaded.bank.lfos.entries[0].period_ms == 777);
    TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
    TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);

    // Raw duplicate input wires: the model cannot branch the input, so the later
    // connect moves its run to the front and wins. Pending events stay unconsumed.
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection({ mapping.input_node, 0 }, { mapping.effect_nodes[3], 0 }) != Sculptor::pool_no_slot);
    TEST(graph.add_connection({ mapping.input_node, 0 }, { mapping.effect_nodes[2], 0 }) != Sculptor::pool_no_slot);
    saved_graph = graph;
    TEST(Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);
    TEST(candidate.bank.channel_chains[0].effects[0].type ==
         Synth::EffectType::delay); // the second target wins the input
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::fir);
    TEST(candidate.effect_routing[0].in == Sculptor::effect_in_first_slot);
    TEST(Sculptor::extract_effect_chain_document(&source, 0, &graph, &mapping, &decoded));
    TEST(decoded.chain.num_effects == 4 && decoded.chain.effects[1].type == Synth::EffectType::fir);

    // Invalid pending structural edits and ring overflow refuse without consuming or publishing.
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    graph.rename_node(mapping.input_node, "Invalid title");
    saved_graph     = graph;
    saved_candidate = candidate;
    TEST(! Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
    TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);
    for (uint32_t index = 0; index <= Sculptor::max_pending_changes; ++index) {
        graph.rename_node(mapping.input_node, index % 2 ? "A" : "B");
    }
    saved_graph = graph;
    TEST(! Sculptor::stage_effect_graph_edits(&source, &graph, &mapping, &candidate));
    TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
    TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);

    // Byte and token bounds are checked before destination publication.
    memset(static_cast<void*>(text), ' ', sizeof(text));
    TEST(! Synth::decode_effects_json(text, sizeof(text), &decoded));
    const char prefix[] = "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[],\"extra\":[";
    uint32_t   text_length = sizeof(prefix) - 1;
    memcpy(text, prefix, text_length);
    for (uint32_t index = 0; index < 8192; ++index) {
        text[text_length++] = '0';
        text[text_length++] = ',';
    }
    text[text_length - 1] = ']';
    text[text_length++]   = '}';
    TEST(! Synth::decode_effects_json(text, text_length, &decoded));

    effects_clipboard_fixture(&doc, true);
    for (uint32_t index = doc.lfo_count; index < Synth::max_lfos; ++index) {
        doc.lfos[index] = doc.lfos[0];
    }
    doc.lfo_count                 = Synth::max_lfos;
    const uint32_t maximal_length = Synth::encode_effects_json(&doc, text, sizeof(text));
    TEST(maximal_length > 0);
    TEST(count_clipboard_tokens(text, maximal_length) > 0);
    TEST(Synth::decode_effects_json(text, maximal_length, &decoded));
    TEST(decoded.lfo_count == Synth::max_lfos);

    const char empty[] = "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[]}";
    TEST(Synth::decode_effects_json(empty, sizeof(empty) - 1, &decoded));
    TEST(decoded.chain.num_effects == 0 && decoded.lfo_count == 0 && decoded.graph_layout_count == 0);
    doc = {};
    TEST(Sculptor::replace_effect_chain_candidate(&source, 0, &doc, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 0);

    const char* const malformed[] = {
        "{",
        "{}",
        "[]",
        "null",
        "{\"format\":\"synth-instrument-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v2\",\"lfos\":[],\"effects\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[],\"extra\":0}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"eff\\u0065cts\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"delay\",\"params\":[]}],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\",\"params\":[{\"lfo_desc_id\":1},{}]}],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"none\"},{\"type\":\"none\"},{\"type\":\"none\"},{\"type\":\"none\"},{\"type\":\"none\"}],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[{\"kind\":\"effect\",\"slot\":0,\"x\":0,\"y\":0}]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[{\"kind\":\"lfo\",\"lfo_desc_id\":1,\"x\":0,\"y\":0}]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[{\"kind\":\"input\",\"x\":0,\"y\":0},{\"kind\":\"input\",\"x\":1,\"y\":1}]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[{\"kind\":\"input\",\"x\":0,\"y\":0,\"name\":\"Renamed\"}]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[],\"graph_layout\":[{\"kind\":\"oscillator\",\"x\":0,\"y\":0}]}"
    };
    const char* nested_invalid[] = {
        "{\"format\":\"synth-effects-v1\",\"lfos\":[{\"period_ms\":300}],\"effects\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[{\"period_ms\":300,\"extra\":0}],\"effects\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[{\"period_ms\":300,\"period_ms\":400}],\"effects\":[],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\",\"params\":[{\"extra\":0},{}]}],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\",\"params\":[{\"inputs\":[{\"extra\":0}]},{}]}],\"graph_layout\":[]}",
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\",\"params\":[{\"base_value\":1e999},{}]}],\"graph_layout\":[]}"
    };
    for (const char* bad : nested_invalid) {
        memset(static_cast<void*>(&decoded), 0xA5, sizeof(decoded));
        saved_doc = decoded;
        TEST(! Synth::decode_effects_json(bad, static_cast<uint32_t>(strlen(bad)), &decoded));
        TEST(memcmp(&decoded, &saved_doc, sizeof(decoded)) == 0);
    }
    for (const char* bad : malformed) {
        memset(static_cast<void*>(&decoded), 0xA5, sizeof(decoded));
        saved_doc = decoded;
        TEST(! Synth::decode_effects_json(bad, static_cast<uint32_t>(strlen(bad)), &decoded));
        TEST(memcmp(&decoded, &saved_doc, sizeof(decoded)) == 0);
    }
    // Direct candidate preflight must be strict too; no partial bank, clipboard,
    // pending graph or history mutation on refusal.
    for (uint32_t bad = 0; bad < 10; ++bad) {
        effects_clipboard_fixture(&doc, true);
        switch (bad) {
            case 0:
                doc.chain.num_effects = 5;
                break;
            case 1:
                doc.lfo_count = Synth::max_lfos + 1;
                break;
            case 2:
                doc.chain.effects[0].bindings[0].lfo_desc_id = 4;
                break;
            case 3:
                doc.chain.effects[0].bindings[0].num_inputs = Synth::max_mod_inputs + 1;
                break;
            case 4:
                doc.chain.effects[0].bindings[0].inputs[0].source = Synth::ModSource::velocity;
                break;
            case 5:
                doc.graph_layout_count = Synth::effects_graph_layout_capacity + 1;
                break;
            case 6:
                doc.graph_layout[1] = doc.graph_layout[0];
                break;
            case 7:
                doc.graph_layout[2].index = 7;
                break; // record points past the chain
            case 8:
                doc.graph_layout[5].index = 4;
                break;
            case 9:
                doc.chain.effects[0].type = Synth::EffectType::num_types;
                break;
        }
        memset(static_cast<void*>(text), 'Q', sizeof(text));
        memcpy(saved_text, text, sizeof(text));
        TEST(Synth::encode_effects_json(&doc, text, sizeof(text)) == 0);
        TEST(memcmp(text, saved_text, sizeof(text)) == 0);
        memset(static_cast<void*>(&candidate), 0x5A, sizeof(candidate));
        saved_candidate = candidate;
        TEST(! Sculptor::replace_effect_chain_candidate(&source, Sculptor::fx_master_chain, &doc, &candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
        TEST(memcmp(&graph, &saved_graph, sizeof(graph)) == 0);
        TEST(undo.undo_empty() && undo.redo_empty());
    }

    // Whole-bank limits: unrelated channels and disabled bindings still count.
    for (uint32_t budget = 0; budget < 3; ++budget) {
        memset(static_cast<void*>(&source), 0, sizeof(source));
        Synth::init_default_bank(&source.bank);
        Sculptor::set_identity_osc_sources(&source, 0, 0);
        effects_clipboard_fixture(&doc, false);
        if (budget == 0) {
            for (uint32_t i = 0; i < Synth::max_lfos; ++i) {
                const uint32_t id            = source.bank.lfos.allocate();
                source.bank.lfos.entries[id] = doc.lfos[0];
            }
            for (uint32_t i = 0; i < Synth::max_lfos; ++i) {
                Synth::GraphNodeLayout& layout = source.graph_layout[i];
                layout.kind                    = 4;
                layout.channel                 = 5;
                snprintf(layout.name, sizeof(layout.name), "LFO %u", i + 1);
            }
            source.graph_layout_count = Synth::max_lfos;
        }
        else if (budget == 1) {
            for (uint32_t c = 1; c <= 4; ++c) {
                Synth::EffectChainBinding& chain = source.bank.channel_chains[c];
                chain.num_effects                = 4;
                for (uint32_t s = 0; s < 4; ++s) {
                    chain.effects[s].type = Synth::EffectType::distortion;
                    for (uint32_t p = 0; p < 2; ++p) {
                        Synth::EffectParamBinding& b = chain.effects[s].bindings[p];
                        b.num_inputs                 = 1;
                        b.inputs[0].source           = Synth::ModSource::mod_wheel;
                    }
                }
            }
        }
        else {
            uint32_t bytes = 0;
            for (uint32_t c = 1; c < Synth::max_channels; ++c) {
                Synth::EffectChainBinding& chain = source.bank.channel_chains[c];
                for (uint32_t s = 0; s < 4 && bytes + Synth::get_effect_state_bytes(Synth::EffectType::delay) <=
                                                  Synth::effect_state_budget;
                     ++s) {
                    chain.effects[s].type    = Synth::EffectType::delay;
                    chain.effects[s].enabled = true;
                    ++chain.num_effects;
                    bytes += Synth::get_effect_state_bytes(Synth::EffectType::delay);
                }
            }
            doc.chain             = {};
            doc.chain.num_effects = 4;
            for (uint32_t s = 0; s < 4; ++s) {
                doc.chain.effects[s].type    = Synth::EffectType::delay;
                doc.chain.effects[s].enabled = true;
            }
            doc.graph_layout_count = 0;
        }
        TEST(Synth::validate_instrument_bank(&source.bank));
        saved_source = source;
        memset(static_cast<void*>(&candidate), 0x5A, sizeof(candidate));
        saved_candidate = candidate;
        TEST(! Sculptor::replace_effect_chain_candidate(&source, 0, &doc, &candidate));
        TEST(memcmp(&candidate, &saved_candidate, sizeof(candidate)) == 0);
        TEST(memcmp(&source, &saved_source, sizeof(source)) == 0);
    }
}

// Add-CC authoring regressions.  The registry is editor state only:
// Synth::InstrumentEditorBank::added_cc_mask never reaches the runtime bank.
// Each added CC is one MIDI-node output dot; a wire from a CC dot decodes to
// a ModSource::cc input with the dot's CC number.
static void effects_add_cc_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate      = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       loaded         = *loaded_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};
    Sculptor::GraphChange                              discarded[Sculptor::max_pending_changes] = {};
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> reencoded_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&reencoded)[Synth::library_payload_max] = reencoded_storage->elements;

    // Registry bits project MIDI-node output dots; absent bits project none.
    add_cc_chain_fixture(&source, 0);
    source.added_cc_mask[0] = (1u << 0) | (1u << 15);

    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.node(mapping.midi_node).slots.num_allocated == 5);
    for (uint32_t s = 0; s < Sculptor::fx_num_input_sources; ++s) {
        TEST(mapping.midi_source_slots[s] != Sculptor::pool_no_slot);
    }
    TEST(mapping.midi_cc_slots[0] != Sculptor::pool_no_slot);
    TEST(mapping.midi_cc_slots[15] != Sculptor::pool_no_slot);
    for (uint32_t i = 1; i < Synth::num_assignable_ccs - 1; ++i) {
        TEST(mapping.midi_cc_slots[i] == Sculptor::pool_no_slot);
    }
    TEST(strcmp(graph.node(mapping.midi_node).slots.entries[mapping.midi_cc_slots[0]].name, "CC 16 (+48)") == 0);
    TEST(strcmp(graph.node(mapping.midi_node).slots.entries[mapping.midi_cc_slots[15]].name, "CC 31 (+63)") == 0);

    // A CC dot decodes to a ModSource::cc input; unwiring drops the input
    // while the registry bit and the dot remain.
    const uint32_t          cc_amount = Sculptor::fx_param_src_dot(0, 0) + 2;
    Sculptor::PropertyValue cc_value  = graph.node(mapping.effect_nodes[0]).slots.entries[cc_amount].value;
    cc_value.real                     = 0.5f;
    graph.set_slot_value(mapping.effect_nodes[0], cc_amount, cc_value);

    TEST(graph.add_connection({ mapping.midi_node, mapping.midi_cc_slots[0] },
                              { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) != Sculptor::pool_no_slot);
    Sculptor::GraphChange cc_connect = {};
    cc_connect.kind                  = Sculptor::ChangeKind::connection_added;
    cc_connect.connection_output     = { mapping.midi_node, mapping.midi_cc_slots[0] };
    cc_connect.connection_input      = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) };
    candidate                        = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, cc_connect));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source == Synth::ModSource::cc);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number == Synth::cc_msb_base);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].op == Synth::SourceOp::add);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale == 0.5f);

    // A later Amount edit recompiles the row and keeps the CC identity.
    cc_value.real = 0.75f;
    graph.set_slot_value(mapping.effect_nodes[0], cc_amount, cc_value);
    Sculptor::GraphChange cc_edit = {};
    cc_edit.kind                  = Sculptor::ChangeKind::value_changed;
    cc_edit.node_idx              = mapping.effect_nodes[0];
    cc_edit.slot_idx              = cc_amount;
    candidate                     = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, cc_edit));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source == Synth::ModSource::cc);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number == Synth::cc_msb_base);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale == 0.75f);

    // Unwiring drops the cc input; the registry bit and the dot remain.
    // The disconnect starts from the compiled candidate, so the dropped
    // input is the wired CC row and not an empty binding.
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    const Sculptor::EndPoint cc_dot_output = { mapping.midi_node, mapping.midi_cc_slots[0] };
    const Sculptor::EndPoint cc_row_input  = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) };
    const uint32_t           cc_wire       = find_fx_graph_connection(graph, cc_dot_output, cc_row_input);
    TEST(cc_wire != Sculptor::pool_no_slot);
    TEST(graph.user_delete_connection(cc_wire));
    Sculptor::GraphChange cc_unwire = {};
    cc_unwire.kind                  = Sculptor::ChangeKind::connection_deleted;
    cc_unwire.connection_output     = cc_dot_output;
    cc_unwire.connection_input      = cc_row_input;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, cc_unwire));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 0);
    TEST(source.added_cc_mask[0] == ((1u << 0) | (1u << 15)));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.midi_cc_slots[0] != Sculptor::pool_no_slot);

    // Undotted refusals stay.  A cc 17 input with no registry bit
    // refuses a value edit atomically even when a standard row is wired too.
    Synth::EffectChainBinding& refusal_chain = add_cc_chain_fixture(&source, 0);
    Synth::EffectParamBinding& refusal_row   = refusal_chain.effects[0].bindings[0];
    refusal_row.num_inputs                   = 2;
    refusal_row.inputs[0].source             = Synth::ModSource::cc;
    refusal_row.inputs[0].cc_number          = 17;
    refusal_row.inputs[0].op                 = Synth::SourceOp::add;
    refusal_row.inputs[0].scale              = 0.5f;
    refusal_row.inputs[1].source             = Synth::ModSource::mod_wheel;
    refusal_row.inputs[1].op                 = Synth::SourceOp::multiply;
    refusal_row.inputs[1].scale              = 0.25f;

    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Synth::EffectParamBinding refusal_before = refusal_row;
    const uint32_t          refusal_amount = Sculptor::fx_param_src_dot(0, 1) + 2; // wired standard row's Amount
    Sculptor::PropertyValue refusal_value  = graph.node(mapping.effect_nodes[0]).slots.entries[refusal_amount].value;
    refusal_value.real                     = 0.9f;
    graph.set_slot_value(mapping.effect_nodes[0], refusal_amount, refusal_value);
    Sculptor::GraphChange refusal_edit = {};
    refusal_edit.kind                  = Sculptor::ChangeKind::value_changed;
    refusal_edit.node_idx              = mapping.effect_nodes[0];
    refusal_edit.slot_idx              = refusal_amount;
    candidate                          = source;
    TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, refusal_edit));
    TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].bindings[0], &refusal_before, sizeof(refusal_before)) ==
         0);

    // A wire from a slot present in neither slot array refuses too (the
    // source == 0 path).  This projection carries no CC dots, so a mapping
    // copy with a cleared standard slot models a vanished dot.
    Synth::EffectChainBinding& stale_chain = add_cc_chain_fixture(&source, 0);
    Synth::EffectParamBinding& stale_row   = stale_chain.effects[0].bindings[0];
    stale_row.num_inputs                   = 1;
    stale_row.inputs[0].source             = Synth::ModSource::mod_wheel;
    stale_row.inputs[0].op                 = Synth::SourceOp::add;
    stale_row.inputs[0].scale              = 0.25f;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Synth::EffectParamBinding stale_before = stale_row;
    Sculptor::EffectGraphMapping    stale        = mapping;
    stale.midi_source_slots[1]                   = Sculptor::pool_no_slot;
    const uint32_t          stale_amount         = Sculptor::fx_param_src_dot(0, 0) + 2;
    Sculptor::PropertyValue stale_value = graph.node(mapping.effect_nodes[0]).slots.entries[stale_amount].value;
    stale_value.real                    = 0.9f;
    graph.set_slot_value(mapping.effect_nodes[0], stale_amount, stale_value);
    Sculptor::GraphChange stale_edit = {};
    stale_edit.kind                  = Sculptor::ChangeKind::value_changed;
    stale_edit.node_idx              = mapping.effect_nodes[0];
    stale_edit.slot_idx              = stale_amount;
    candidate                        = source;
    TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, stale, stale_edit));
    TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].bindings[0], &stale_before, sizeof(stale_before)) == 0);

    // Late-failure atomicity of both direct rebuild callers.  Source A
    // is wired valid and Source B is wired from a non-MIDI node by direct
    // graph mutation (the retarget test's shape).  A value edit and a connect
    // into the same parameter must both refuse with the whole bank
    // byte-identical.
    Synth::EffectChainBinding& atomic_chain = add_cc_chain_fixture(&source, 0);
    Synth::EffectParamBinding& atomic_row   = atomic_chain.effects[0].bindings[0];
    atomic_row.num_inputs                   = 2;
    atomic_row.inputs[0].source             = Synth::ModSource::channel_pressure;
    atomic_row.inputs[0].op                 = Synth::SourceOp::add;
    atomic_row.inputs[0].scale              = 0.9f;
    atomic_row.inputs[1].source             = Synth::ModSource::velocity;
    atomic_row.inputs[1].op                 = Synth::SourceOp::multiply;
    atomic_row.inputs[1].scale              = 0.8f;

    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection({ mapping.input_node, 0 },
                              { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 1) }) != Sculptor::pool_no_slot);
    const uint32_t          atomic_amount = Sculptor::fx_param_src_dot(0, 0) + 2;
    Sculptor::PropertyValue atomic_value  = graph.node(mapping.effect_nodes[0]).slots.entries[atomic_amount].value;
    atomic_value.real                     = 0.75f;
    graph.set_slot_value(mapping.effect_nodes[0], atomic_amount, atomic_value);

    // The value handler caller: an edit on the valid leading row.
    Sculptor::GraphChange atomic_edit = {};
    atomic_edit.kind                  = Sculptor::ChangeKind::value_changed;
    atomic_edit.node_idx              = mapping.effect_nodes[0];
    atomic_edit.slot_idx              = atomic_amount;
    candidate                         = source;
    TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, atomic_edit));
    TEST(memcmp(&candidate.bank, &source.bank, sizeof(candidate.bank)) == 0);

    // The connect caller: a wire landing on the same parameter's source row.
    Sculptor::GraphChange atomic_wire = {};
    atomic_wire.kind                  = Sculptor::ChangeKind::connection_added;
    atomic_wire.connection_output     = { mapping.midi_node, mapping.midi_source_slots[0] };
    atomic_wire.connection_input      = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) };
    candidate                         = source;
    TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, atomic_wire));
    TEST(memcmp(&candidate.bank, &source.bank, sizeof(candidate.bank)) == 0);

    // Adoption union.  Replacing a chain whose bindings wire CC 17
    // unions CC 17 into the destination registry.  Pre-existing registry
    // bits on the destination and on another chain survive, and the
    // re-projection shows the dot and the wire.
    Synth::EffectChainBinding& pasted_chain = add_cc_chain_fixture(&source, 0);
    Synth::EffectParamBinding& pasted_row   = pasted_chain.effects[0].bindings[0];
    pasted_row.num_inputs                   = 1;
    pasted_row.inputs[0].source             = Synth::ModSource::cc;
    pasted_row.inputs[0].cc_number          = 17;
    pasted_row.inputs[0].op                 = Synth::SourceOp::add;
    pasted_row.inputs[0].scale              = 0.5f;

    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    loaded.added_cc_mask[0] = 1u << 6; // a pre-existing addition on the receiving chain
    loaded.added_cc_mask[1] = 1u << 4; // a pre-existing addition on another chain

    Synth::EffectsDocument document = {};
    TEST(Sculptor::extract_effect_chain_document(&source, 0, nullptr, nullptr, &document));
    TEST(Sculptor::replace_effect_chain_candidate(&loaded, 0, &document, &candidate));
    TEST(candidate.added_cc_mask[0] == ((1u << 1) | (1u << 6))); // CC 17 adopted, existing bit survives
    TEST(candidate.added_cc_mask[1] == (1u << 4));               // the other chain's bit survives
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.midi_cc_slots[1] != Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph,
                                  { mapping.midi_node, mapping.midi_cc_slots[1] },
                                  { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) !=
         Sculptor::pool_no_slot);

    // fx_chain_uses_cc detects a chain's own CC bindings only.
    Synth::EffectChainBinding& uses_chain                 = add_cc_chain_fixture(&source, 0);
    uses_chain.effects[0].bindings[0].num_inputs          = 1;
    uses_chain.effects[0].bindings[0].inputs[0].source    = Synth::ModSource::cc;
    uses_chain.effects[0].bindings[0].inputs[0].cc_number = 16;
    uses_chain.effects[0].bindings[0].inputs[0].op        = Synth::SourceOp::add;
    uses_chain.effects[0].bindings[0].inputs[0].scale     = 0.5f;

    TEST(Sculptor::fx_chain_uses_cc(source.bank.channel_chains[0], 16));
    TEST(! Sculptor::fx_chain_uses_cc(source.bank.channel_chains[0], 17));
    TEST(! Sculptor::fx_chain_uses_cc(loaded.bank.channel_chains[1], 16));

    // Bank-file persistence.  Masks ride the editor section and
    // survive a round trip byte-identically.  A bank with mask bits and a
    // wired chain encodes to the single editor root and carries the key.
    // A legacy file without the key decodes with all-zero masks.  The strict
    // editor object still refuses unknown editor keys.
    add_cc_chain_fixture(&source, 0);
    source.effect_routing[0].in                     = 1;
    source.effect_routing[0].out[0]                 = Sculptor::effect_out_output; // effect feeds the output
    source.added_cc_mask[0]                         = 0x0003;                      // CC 16 and CC 17
    source.added_cc_mask[Sculptor::fx_master_chain] = 0x8000;                      // CC 31

    const uint32_t v2_length = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(v2_length > 0);
    TEST(strncmp(payload, "{\"instrument_editor_bank\":{", strlen("{\"instrument_editor_bank\":")) == 0);
    TEST(strstr(payload, "\"effect_routing\"") != nullptr);
    TEST(strstr(payload, "\"added_ccs\"") != nullptr);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    TEST(Synth::decode_editor_bank_json(payload, v2_length, &loaded));
    TEST(loaded.added_cc_mask[0] == 0x0003);
    TEST(loaded.added_cc_mask[Sculptor::fx_master_chain] == 0x8000);
    const uint32_t v2_again = Synth::encode_editor_bank_json(&loaded, reencoded, sizeof(reencoded));
    TEST(v2_again == v2_length && memcmp(payload, reencoded, v2_length) == 0);

    // Strict v2 refusal: an unknown key inside a legacy v2 root's editor object
    // fails. The production root stays lenient so unknown metadata from newer
    // writers still loads (the tolerance test below pins that side).
    const char     v2_base[]   = "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                                 "\"master_chain\":{\"effects\":[]},"
                                 "\"editor\":{\"effect_audio\":[null,null,null,null,null,null,null,"
                                 "null,null,null,null,null,null,null,null,null,null],\"layouts\":[]}}}";
    const uint32_t v2_base_len = static_cast<uint32_t>(strlen(v2_base));
    TEST(Synth::decode_editor_bank_json(v2_base, v2_base_len, &loaded));
    char v2_document[sizeof(v2_base) + 16] = {};
    memcpy(v2_document, v2_base, v2_base_len);
    char* const editor = strstr(v2_document, "\"editor\":{");
    TEST(editor != nullptr);
    if (editor) {
        const char   unknown[]   = "\"future\":1,";
        const size_t unknown_len = strlen(unknown);
        char* const  insert      = editor + strlen("\"editor\":{");
        const size_t offset      = static_cast<size_t>(insert - v2_document);
        memmove(insert + unknown_len, insert, v2_base_len - offset + 1);
        memcpy(insert, unknown, unknown_len);
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(! Synth::decode_editor_bank_json(v2_document, v2_base_len + static_cast<uint32_t>(unknown_len), &loaded));
    }

    // Legacy root: mask bits with an unwired chain still carry the key.
    add_cc_chain_fixture(&source, 0);
    source.added_cc_mask[0]                         = 0x0001;
    source.added_cc_mask[Sculptor::fx_master_chain] = 0x0002;
    const uint32_t legacy_length                    = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(legacy_length > 0);
    TEST(strncmp(payload, "{\"instrument_editor_bank\":{", strlen("{\"instrument_editor_bank\":")) == 0);
    TEST(strstr(payload, "\"effect_routing\":[{\"in\":0,\"out\":[0]") !=
         nullptr); // unwired chain round-trips its flags
    TEST(strstr(payload, "\"added_ccs\"") != nullptr);
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    TEST(Synth::decode_editor_bank_json(payload, legacy_length, &loaded));
    TEST(loaded.added_cc_mask[0] == 0x0001);
    TEST(loaded.added_cc_mask[Sculptor::fx_master_chain] == 0x0002);

    // A legacy bank without the key decodes with all-zero masks.
    add_cc_chain_fixture(&source, 0);
    const uint32_t old_length = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
    TEST(old_length > 0);
    TEST(strstr(payload, "\"added_ccs\"") == nullptr); // zero masks emit no key
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    TEST(Synth::decode_editor_bank_json(payload, old_length, &loaded));
    for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
        TEST(loaded.added_cc_mask[c] == 0);
    }

    // The master chain (mask index fx_master_chain) authors and
    // decodes CC dots identically to channel chains.
    add_cc_chain_fixture(&source, Sculptor::fx_master_chain);
    source.added_cc_mask[Sculptor::fx_master_chain] = 1u << 2; // CC 18

    TEST(Sculptor::project_effect_chain_to_graph(source, Sculptor::fx_master_chain, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.chain == Sculptor::fx_master_chain);
    TEST(graph.node(mapping.midi_node).slots.num_allocated == 4);
    TEST(mapping.midi_cc_slots[2] != Sculptor::pool_no_slot);
    TEST(strcmp(graph.node(mapping.midi_node).slots.entries[mapping.midi_cc_slots[2]].name, "CC 18 (+50)") == 0);

    TEST(graph.add_connection({ mapping.midi_node, mapping.midi_cc_slots[2] },
                              { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) != Sculptor::pool_no_slot);
    Sculptor::GraphChange master_wire = {};
    master_wire.kind                  = Sculptor::ChangeKind::connection_added;
    master_wire.connection_output     = { mapping.midi_node, mapping.midi_cc_slots[2] };
    master_wire.connection_input      = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) };
    candidate                         = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, master_wire));
    TEST(candidate.bank.master_chain.effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.master_chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::cc);
    TEST(candidate.bank.master_chain.effects[0].bindings[0].inputs[0].cc_number == 18);

    // A wired CC row with an empty mask stays inert until its CC is added;
    // adding an already-set bit adds no second dot.
    Synth::EffectChainBinding& m1_chain = add_cc_chain_fixture(&source, 0);
    Synth::EffectParamBinding& m1_row   = m1_chain.effects[0].bindings[0];
    m1_row.num_inputs                   = 1;
    m1_row.inputs[0].source             = Synth::ModSource::cc;
    m1_row.inputs[0].cc_number          = 16;
    m1_row.inputs[0].op                 = Synth::SourceOp::add;
    m1_row.inputs[0].scale              = 0.5f;

    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.midi_cc_slots[0] == Sculptor::pool_no_slot); // empty mask: no dot yet
    TEST(find_fx_graph_connection(graph,
                                  { mapping.midi_node, mapping.midi_source_slots[0] },
                                  { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) ==
         Sculptor::pool_no_slot);
    TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 1));
    TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 2));

    source.added_cc_mask[0] = 1u << 0;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.node(mapping.midi_node).slots.num_allocated == 4);
    TEST(mapping.midi_cc_slots[0] != Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph,
                                  { mapping.midi_node, mapping.midi_cc_slots[0] },
                                  { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) !=
         Sculptor::pool_no_slot);
    TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 1));
    TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 2));

    const uint32_t          m1_amount = Sculptor::fx_param_src_dot(0, 0) + 2;
    Sculptor::PropertyValue m1_value  = graph.node(mapping.effect_nodes[0]).slots.entries[m1_amount].value;
    m1_value.real                     = 0.6f;
    graph.set_slot_value(mapping.effect_nodes[0], m1_amount, m1_value);
    Sculptor::GraphChange m1_edit = {};
    m1_edit.kind                  = Sculptor::ChangeKind::value_changed;
    m1_edit.node_idx              = mapping.effect_nodes[0];
    m1_edit.slot_idx              = m1_amount;
    candidate                     = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, m1_edit));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source == Synth::ModSource::cc);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].cc_number == 16);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale == 0.6f);

    source.added_cc_mask[0] |= 1u << 0; // re-setting the bit adds no second dot
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    TEST(graph.node(mapping.midi_node).slots.num_allocated == 4);

    // Library round trip.  A save from a non-zero source channel
    // carries that channel's mask, not channel 0's.  A load ORs the record's
    // saved mask and the incoming chain's wired CC numbers into the receiving
    // chain's mask; a pre-existing destination addition survives and a second
    // load of the same record changes nothing.  Other chains' masks stay
    // exactly as they were.  A legacy record without the key decodes with
    // all-zero masks.
    Synth::EffectChainBinding& library_chain                 = add_cc_chain_fixture(&source, 2);
    library_chain.effects[0].bindings[0].num_inputs          = 1;
    library_chain.effects[0].bindings[0].inputs[0].source    = Synth::ModSource::cc;
    library_chain.effects[0].bindings[0].inputs[0].cc_number = 18; // wired but not in the saved mask
    library_chain.effects[0].bindings[0].inputs[0].op        = Synth::SourceOp::add;
    library_chain.effects[0].bindings[0].inputs[0].scale     = 0.5f;

    source.added_cc_mask[2]        = (1u << 0) | (1u << 1); // CC 16 and CC 17 travel
    source.added_cc_mask[0]        = 1u << 5;               // CC 21 on channel 0 must not travel
    source.bank.channel_enabled[2] = true;
    memcpy(source.bank.channel_zones[2], source.bank.channel_zones[0], sizeof(source.bank.channel_zones[2]));
    memcpy(source.graph_osc_source[2], source.graph_osc_source[0], sizeof(source.graph_osc_source[2]));
    const char* library_path = "synth_add_cc_library.tmp";
    TEST(Synth::save_library_record(library_path, "Regression", "CC Round", &source, 2) == 0);
    Synth::LibraryEntry cc_entry;
    TEST(Synth::read_library_index(library_path, &cc_entry, 1) == 1);

    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    uint16_t cc_slot = 0;
    TEST(Synth::load_library_instrument(library_path, &cc_entry, &loaded, 3, &cc_slot));
    TEST(loaded.added_cc_mask[3] == ((1u << 0) | (1u << 1) | (1u << 2))); // saved mask OR wired CC 18
    TEST(loaded.added_cc_mask[0] == 0);                                   // channel 0's mask never travels

    // A pre-existing destination addition on a different CC survives, the
    // other chains' masks stay as they were, and a second load is idempotent.
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    Synth::init_default_bank(&loaded.bank);
    Sculptor::set_identity_osc_sources(&loaded, 0, 0);
    loaded.added_cc_mask[3] = 1u << 4; // pre-existing CC 20 addition
    loaded.added_cc_mask[1] = 1u << 7; // another chain's state
    TEST(Synth::load_library_instrument(library_path, &cc_entry, &loaded, 3, &cc_slot));
    TEST(loaded.added_cc_mask[3] == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 4)));
    TEST(Synth::load_library_instrument(library_path, &cc_entry, &loaded, 3, &cc_slot));
    for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
        if (c == 1) {
            TEST(loaded.added_cc_mask[c] == (1u << 7));
            continue;
        }
        if (c == 3) {
            TEST(loaded.added_cc_mask[c] == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 4)));
            continue;
        }
        TEST(loaded.added_cc_mask[c] == 0);
    }

    // A legacy record without the key decodes with all-zero masks.
    add_cc_chain_fixture(&source, 0); // zero masks: the encoder emits no key
    TEST(Synth::save_library_record(library_path, "Regression", "No CC", &source, 0) == 0);
    Synth::LibraryEntry library_entries[2];
    TEST(Synth::read_library_index(library_path, library_entries, 2) == 2);
    const Synth::LibraryEntry* legacy_entry = nullptr;
    for (uint32_t i = 0; i < 2; ++i) {
        if (Synth::library_record_matches(library_entries[i], "Regression", "No CC")) {
            legacy_entry = &library_entries[i];
        }
    }
    TEST(legacy_entry != nullptr);
    if (legacy_entry) {
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        Synth::init_default_bank(&loaded.bank);
        Sculptor::set_identity_osc_sources(&loaded, 0, 0);
        TEST(Synth::load_library_instrument(library_path, legacy_entry, &loaded, 1, &cc_slot));
        for (uint32_t c = 0; c <= Synth::max_channels; ++c) {
            TEST(loaded.added_cc_mask[c] == 0);
        }
    }
    remove(library_path);
}

// Enable-routing regressions.  EffectSlotBinding carries a ModInput
// enable_input (zero = unwired); the Enabled row is a connectable toggle
// property; the runtime live-enable decision resolves a wired routing's leaf
// value against the raw 14-bit >= 8192 threshold, while an unwired one keeps
// the saved checkbox state.  The pure helpers under test live in
// synth_effect_expansion, the wiring in sculptor_effect_graph.
static void effects_enable_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           decoded       = *decoded_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::EffectGraphMapping           mapping       = {};
    Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;

    const Synth::ModInput zero_input = {};

    // T-M5-1: the wired threshold sits exactly between raw 14-bit 8191 and
    // 8192 (MSB 63 vs 64).  Values arrive through the M1 capture path as two
    // 7-bit halves, never as a prepared float.
    {
        Synth::ChannelCcState cc_state;
        memset(static_cast<void*>(&cc_state), 0, sizeof(cc_state));
        memset(static_cast<void*>(cc_state.lsb), 0xFF, sizeof(cc_state.lsb)); // fresh channel: no LSB seen yet

        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 63);
        const float at_8191 = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 127);
        TEST(approx(at_8191, 8191.0f / 16383.0f, 1e-6f));
        TEST(! Synth::effect_live_enabled(77, at_8191, true)); // a wired low beats a saved-on bit

        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 64);
        const float at_8192 = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 0);
        TEST(approx(at_8192, 8192.0f / 16383.0f, 1e-6f));
        TEST(Synth::effect_live_enabled(77, at_8192, false)); // a wired high beats a saved-off bit
    }

    // T-M5-2: an unwired enable keeps the saved checkbox state: the toggle
    // write-back lands on the Enabled row's value seam.  An unwired
    // saved-off slot stays stateless: no routing resolves, no state reserves.
    {
        add_cc_chain_fixture(&source, 0);
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);

        const Sculptor::Slot& enabled_row = graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_enabled_row];
        TEST(enabled_row.property_type == Sculptor::PropertyType::toggle);
        TEST(enabled_row.value.toggle == true); // the fixture saves the effect on

        Sculptor::PropertyValue off = {};
        off.toggle                  = false;
        graph.set_slot_value(mapping.effect_nodes[0], Sculptor::fx_enabled_row, off);
        Sculptor::GraphChange toggle_off = {};
        toggle_off.kind                  = Sculptor::ChangeKind::value_changed;
        toggle_off.node_idx              = mapping.effect_nodes[0];
        toggle_off.slot_idx              = Sculptor::fx_enabled_row;
        candidate                        = source;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, toggle_off));
        TEST(candidate.bank.channel_chains[0].effects[0].enabled == false);
        TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].enable_input, &zero_input, sizeof(zero_input)) == 0);

        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        bank.channel_chains[0].num_effects        = 1;
        bank.channel_chains[0].effects[0].type    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = false;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == 0); // an unwired saved-off slot stays stateless

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].enabled == false);
        TEST(chains[0].effects[0].enable_param_id == 0);
        TEST(writer.num_resolved == 0); // an unwired enable resolves nothing
        (void)Synth::take_effect_clear_ranges();
    }

    // T-M5-3: a wired enable overrides the saved checkbox in both directions,
    // and the commit resolves the routing so the runtime can ignore the bit.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        bank.channel_chains[1].num_effects                       = 1;
        bank.channel_chains[1].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[1].effects[0].enabled                = true;
        bank.channel_chains[1].effects[0].enable_input.source    = Synth::ModSource::cc;
        bank.channel_chains[1].effects[0].enable_input.op        = Synth::SourceOp::add;
        bank.channel_chains[1].effects[0].enable_input.cc_number = Synth::cc_msb_base;
        bank.channel_chains[1].effects[0].enable_input.scale     = 1.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(writer.num_resolved == 1);
        TEST(writer.resolved[0].source == Synth::ModSource::cc);
        TEST(writer.resolved[0].cc_number == Synth::cc_msb_base);
        TEST(writer.resolved[0].chain == 1); // a channel chain resolves from its own channel
        TEST(chains[1].effects[0].enable_param_id == 77);
        TEST(chains[1].effects[0].enabled == true); // the saved bit still commits as-is
        (void)Synth::take_effect_clear_ranges();

        TEST(! Synth::effect_live_enabled(77, 8191.0f / 16383.0f, true)); // saved on + routed off -> off
        TEST(Synth::effect_live_enabled(77, 8192.0f / 16383.0f, false));  // saved off + routed on -> on
    }

    // T-M5-4: RESET fires exactly on the off->on live-enable transition, so a
    // re-enabled effect starts from silence while a continuously enabled one
    // is never cleared.
    {
        TEST(Synth::effect_needs_reset(false, true));
        TEST(! Synth::effect_needs_reset(true, true));
        TEST(! Synth::effect_needs_reset(true, false));
        TEST(! Synth::effect_needs_reset(false, false));

        // A routed effect that ran, dropped below the threshold and rose back
        // across it clears once at the re-enable edge.
        Synth::ChannelCcState cc_state;
        memset(static_cast<void*>(&cc_state), 0, sizeof(cc_state));
        memset(static_cast<void*>(cc_state.lsb), 0xFF, sizeof(cc_state.lsb));
        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 64);
        const float live_value = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 0);

        bool previous = Synth::effect_live_enabled(77, live_value, true);
        TEST(previous);

        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 63);
        const float dropped  = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 127);
        const bool  disabled = Synth::effect_live_enabled(77, dropped, true);
        TEST(! disabled);
        TEST(! Synth::effect_needs_reset(previous, disabled)); // disabling clears nothing
        previous = disabled;

        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 64);
        const float risen     = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 0);
        const bool  reenabled = Synth::effect_live_enabled(77, risen, true);
        TEST(reenabled);
        TEST(Synth::effect_needs_reset(previous, reenabled)); // the re-enable edge clears
    }

    // T-M5-5: a saved-off, enable-routed effect reserves state and consumes
    // budget like a saved-on one; the budget-exhaustion refusal keeps its
    // existing error path.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);

        // Three saved-on master delays plus one saved-off, enable-routed delay
        // on channel 0: four delays, exactly the state budget.
        bank.master_chain.num_effects = 3;
        for (uint32_t s = 0; s < 3; ++s) {
            bank.master_chain.effects[s].type    = Synth::EffectType::delay;
            bank.master_chain.effects[s].enabled = true;
        }
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled                = false;
        bank.channel_chains[0].effects[0].enable_input.source    = Synth::ModSource::cc;
        bank.channel_chains[0].effects[0].enable_input.op        = Synth::SourceOp::add;
        bank.channel_chains[0].effects[0].enable_input.cc_number = Synth::cc_msb_base;
        bank.channel_chains[0].effects[0].enable_input.scale     = 1.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs != 0); // routed-but-bypassed still reserves state
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == 4 * Synth::get_effect_state_bytes(Synth::EffectType::delay));

        // A fifth delay, itself saved-off but enable-routed, exceeds the
        // budget: the preflight refuses and leaves the plan untouched.
        bank.channel_chains[1].num_effects                       = 1;
        bank.channel_chains[1].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[1].effects[0].enabled                = false;
        bank.channel_chains[1].effects[0].enable_input.source    = Synth::ModSource::cc;
        bank.channel_chains[1].effects[0].enable_input.op        = Synth::SourceOp::add;
        bank.channel_chains[1].effects[0].enable_input.cc_number = Synth::cc_msb_base + 1;
        bank.channel_chains[1].effects[0].enable_input.scale     = 1.0f;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 4 * Synth::get_effect_state_bytes(Synth::EffectType::delay));
    }

    // T-M5-6: bank JSON carries the enable routing only when wired and decodes
    // back identically; a bank without the field (a pre-M5 save) loads as
    // unwired, because the decode zero-fills what the encoder omits (DM3).
    {
        add_cc_chain_fixture(&source, 0);
        source.bank.channel_chains[0].effects[0].enabled = false;
        Synth::ModInput& routing                         = source.bank.channel_chains[0].effects[0].enable_input;
        routing.source                                   = Synth::ModSource::cc;
        routing.op                                       = Synth::SourceOp::add;
        routing.cc_number                                = Synth::cc_msb_base;
        routing.scale                                    = 1.0f;

        const uint32_t wired_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(wired_len > 0);
        payload[wired_len] = '\0';
        TEST(strstr(payload, "\"enable_input\"") != nullptr); // encoded when wired

        TEST(Synth::decode_editor_bank_json(payload, wired_len, &decoded));
        TEST(decoded.bank.channel_chains[0].effects[0].enabled == false);
        TEST(decoded.bank.channel_chains[0].effects[0].enable_input.source == Synth::ModSource::cc);
        TEST(decoded.bank.channel_chains[0].effects[0].enable_input.cc_number == Synth::cc_msb_base);

        // Unwired: the key is omitted, so the same document shape serves the
        // old-bank load and the routing decodes to the zero fill.
        source.bank.channel_chains[0].effects[0].enable_input = zero_input;
        const uint32_t plain_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(plain_len > 0);
        payload[plain_len] = '\0';
        TEST(strstr(payload, "\"enable_input\"") == nullptr);

        TEST(Synth::decode_editor_bank_json(payload, plain_len, &decoded));
        TEST(decoded.bank.channel_chains[0].effects[0].enabled == false);
        TEST(memcmp(&decoded.bank.channel_chains[0].effects[0].enable_input, &zero_input, sizeof(zero_input)) == 0);
    }

    // T-M5-7: wiring the Enabled row is a structural edit, so the projection
    // rebuilds it with the wire into the Enabled slot (rendered greyed);
    // disconnecting restores the editable checkbox at the saved state.
    {
        add_cc_chain_fixture(&source, 0);
        source.bank.channel_chains[0].effects[0].enabled = false; // saved off
        source.added_cc_mask[0]                          = 1u << 0;

        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        const Sculptor::Slot& enabled_row = graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_enabled_row];
        TEST(enabled_row.property_type == Sculptor::PropertyType::toggle);
        TEST(enabled_row.connectable); // the Enabled row admits wires
        TEST(enabled_row.value.toggle == false);

        const Sculptor::EndPoint cc_out    = { mapping.midi_node, mapping.midi_cc_slots[0] };
        const Sculptor::EndPoint enable_in = { mapping.effect_nodes[0], Sculptor::fx_enabled_row };
        TEST(graph.add_connection(cc_out, enable_in) != Sculptor::pool_no_slot);
        Sculptor::GraphChange connect = {};
        connect.kind                  = Sculptor::ChangeKind::connection_added;
        connect.connection_output     = cc_out;
        connect.connection_input      = enable_in;
        candidate                     = source;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, connect));
        TEST(candidate.bank.channel_chains[0].effects[0].enable_input.source == Synth::ModSource::cc);
        TEST(candidate.bank.channel_chains[0].effects[0].enable_input.cc_number == Synth::cc_msb_base);
        TEST(candidate.bank.channel_chains[0].effects[0].enabled == false); // wiring keeps the saved bit

        TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(find_fx_graph_connection(graph, cc_out, enable_in) != Sculptor::pool_no_slot); // the wired row rebuilds

        // Disconnect: the wire drops, the routing zeroes, and the checkbox
        // shows the previously saved state again.
        const uint32_t wire = find_fx_graph_connection(graph, cc_out, enable_in);
        TEST(wire != Sculptor::pool_no_slot);
        TEST(graph.user_delete_connection(wire));
        Sculptor::GraphChange disconnect = {};
        disconnect.kind                  = Sculptor::ChangeKind::connection_deleted;
        disconnect.connection_output     = cc_out;
        disconnect.connection_input      = enable_in;
        TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, disconnect));
        TEST(memcmp(&candidate.bank.channel_chains[0].effects[0].enable_input, &zero_input, sizeof(zero_input)) == 0);
        TEST(candidate.bank.channel_chains[0].effects[0].enabled == false);

        TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(find_fx_graph_connection(graph, cc_out, enable_in) == Sculptor::pool_no_slot);
        const Sculptor::Slot& restored_row =
            graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_enabled_row];
        TEST(restored_row.property_type == Sculptor::PropertyType::toggle);
        TEST(restored_row.value.toggle == false);
    }

    // T-M5-8: the master chain resolves its enable routing through the
    // master->0 chokepoint, so the same threshold governs master effects.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        bank.master_chain.num_effects                       = 1;
        bank.master_chain.effects[0].type                   = Synth::EffectType::delay;
        bank.master_chain.effects[0].enabled                = false;
        bank.master_chain.effects[0].enable_input.source    = Synth::ModSource::cc;
        bank.master_chain.effects[0].enable_input.op        = Synth::SourceOp::add;
        bank.master_chain.effects[0].enable_input.cc_number = Synth::cc_msb_base;
        bank.master_chain.effects[0].enable_input.scale     = 1.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(writer.num_resolved == 1);
        TEST(writer.resolved[0].source == Synth::ModSource::cc);
        TEST(writer.resolved[0].cc_number == Synth::cc_msb_base);
        TEST(writer.resolved[0].chain == 0); // the master chain resolves from channel index 0
        TEST(master.effects[0].enable_param_id == 77);
        TEST(Synth::effect_live_enabled(master.effects[0].enable_param_id, 8192.0f / 16383.0f, false));
        (void)Synth::take_effect_clear_ranges();
    }

    // T-M5-9: an enable routing whose CC lost its registry dot stays inert
    // way: the bank keeps the routing and the runtime still honors the
    // raw CC, while the editor draws no wire and the Enabled row shows the
    // saved checkbox state; a routing-recompiling edit refuses atomically.
    {
        add_cc_chain_fixture(&source, 0);
        source.bank.channel_chains[0].effects[0].enabled = true; // saved on
        Synth::ModInput& stale                           = source.bank.channel_chains[0].effects[0].enable_input;
        stale.source                                     = Synth::ModSource::cc;
        stale.op                                         = Synth::SourceOp::add;
        stale.cc_number                                  = Synth::cc_msb_base + 1; // CC 17, no registry bit
        stale.scale                                      = 1.0f;

        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.midi_cc_slots[1] == Sculptor::pool_no_slot); // the dot is gone
        const Sculptor::EndPoint enable_in  = { mapping.effect_nodes[0], Sculptor::fx_enabled_row };
        bool                     stale_wire = false;
        for (uint32_t i = 0; i < Sculptor::max_connections; ++i) {
            if (graph.connection_occupied(i) && graph.get_connection(i).input.node_idx == enable_in.node_idx &&
                graph.get_connection(i).input.slot_idx == enable_in.slot_idx) {
                stale_wire = true;
            }
        }
        TEST(! stale_wire); // no wire to an absent dot
        const Sculptor::Slot& enabled_row = graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_enabled_row];
        TEST(enabled_row.property_type == Sculptor::PropertyType::toggle);
        TEST(enabled_row.value.toggle == true); // presentational fallback: the saved checkbox

        // The enable wire participates in the wired-CC registry rules, so the
        // undotted CC 17 still counts as wired (removal-refusal input).
        TEST((Sculptor::fx_chain_wired_cc_mask(Sculptor::fx_graph_chain(&source.bank, 0)) & (1u << 1)) != 0);

        // The runtime keeps honoring the routing: the commit resolves it and
        // the CC threshold governs, whatever the saved checkbox says.
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        bank                                                      = source.bank;
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].enable_param_id == 77);
        TEST(! Synth::effect_live_enabled(77, 8191.0f / 16383.0f, true)); // routed low beats saved-on
        (void)Synth::take_effect_clear_ranges();

        // A routing-recompiling edit on the Enabled row refuses atomically
        // while the named CC has no dot.
        const Sculptor::EndPoint wheel_out = { mapping.midi_node, mapping.midi_source_slots[1] };
        TEST(graph.add_connection(wheel_out, enable_in) != Sculptor::pool_no_slot);
        Sculptor::GraphChange reconnect = {};
        reconnect.kind                  = Sculptor::ChangeKind::connection_added;
        reconnect.connection_output     = wheel_out;
        reconnect.connection_input      = enable_in;
        candidate                       = source;
        TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, reconnect));
        TEST(memcmp(&candidate, &source, sizeof(candidate)) == 0);
    }

    // T-M5-10: the RESET decision at the runtime seam across a publish.  A
    // saved-off routed effect that is live on both sides of a commit boundary
    // keeps its state, so a publish never looks like an off->on transition,
    // while a genuine off->on transition still clears - through the routing
    // and through a saved-checkbox toggle on a retained slot.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        bank.channel_chains[0].num_effects                       = 1;
        bank.channel_chains[0].effects[0].type                   = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled                = false;
        bank.channel_chains[0].effects[0].enable_input.source    = Synth::ModSource::cc;
        bank.channel_chains[0].effects[0].enable_input.op        = Synth::SourceOp::add;
        bank.channel_chains[0].effects[0].enable_input.cc_number = Synth::cc_msb_base;
        bank.channel_chains[0].effects[0].enable_input.scale     = 1.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        Synth::EffectInstance& effect = chains[0].effects[0];

        // The hand replication below mirrors pull_effect_param_chain's decision
        // sequence (realtime_synth.cpp:1570-1594): read the previous live state,
        // evaluate the wired/unwired threshold decision, decide the RESET edge,
        // then write the live state back.  The runtime call itself needs a GPU,
        // so the suite replays the same helper calls in the same order.
        // The raw CC arrives through the capture path; the first simulated
        // block takes the routed instance live.
        Synth::ChannelCcState cc_state;
        memset(static_cast<void*>(&cc_state), 0, sizeof(cc_state));
        memset(static_cast<void*>(cc_state.lsb), 0xFF, sizeof(cc_state.lsb));
        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 64);
        const float live_value = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 0);

        effect.enabled = Synth::effect_live_enabled(effect.enable_param_id, live_value, effect.saved_enabled);
        TEST(effect.enabled);

        // A publish re-commits the same bank.  The retained slot keeps its
        // live enable state, so the next block sees no off->on edge: the
        // lived-in tail survives an unrelated knob turn.
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(effect.state_offs != 0); // the state range survived the publish

        const bool previous_enabled = effect.enabled;
        const bool live_enabled = Synth::effect_live_enabled(effect.enable_param_id, live_value, effect.saved_enabled);
        TEST(! Synth::effect_needs_reset(previous_enabled, live_enabled)); // continuously live: no reset
        effect.enabled = live_enabled;

        // A genuine off->on transition still clears: the routing drops below
        // the threshold, then rises back across it.
        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 63);
        const float dropped = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 127);
        effect.enabled      = Synth::effect_live_enabled(effect.enable_param_id, dropped, effect.saved_enabled);
        TEST(! effect.enabled);

        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 64);
        const float risen     = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 0);
        const bool  reenabled = Synth::effect_live_enabled(effect.enable_param_id, risen, effect.saved_enabled);
        TEST(reenabled);
        TEST(Synth::effect_needs_reset(effect.enabled, reenabled)); // the re-enable edge clears

        // Drop back below the threshold: the effect bypasses with its
        // lived-in state still in the range.
        (void)Synth::apply_channel_cc(&cc_state, Synth::cc_msb_base, 63);
        const float bypassed = Synth::apply_channel_cc(&cc_state, Synth::cc_lsb_base, 127);
        effect.enabled       = Synth::effect_live_enabled(effect.enable_param_id, bypassed, effect.saved_enabled);
        TEST(! effect.enabled);

        // Unwiring the routing while bypassed and saving the checkbox on
        // keeps the edge visible across the publish: the commit seeds the
        // retained slot with the previous (off) live state, so the first
        // block still sees off->on and clears.
        bank.channel_chains[0].effects[0].enable_input = zero_input;
        bank.channel_chains[0].effects[0].enabled      = true;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(effect.state_offs != 0); // the lived-in state range is retained
        TEST(! effect.enabled);       // seeded with the previous (bypassed) live state
        TEST(effect.saved_enabled);   // the saved checkbox committed

        const bool unwired_live = Synth::effect_live_enabled(effect.enable_param_id, 0.0f, effect.saved_enabled);
        TEST(unwired_live);                                            // unwired: the saved checkbox decides
        TEST(Synth::effect_needs_reset(effect.enabled, unwired_live)); // the toggle edge clears
    }

    // T-M5-11: a plain unwired checkbox toggle off->on across a publish on a
    // stateful effect.  An unwired saved-off slot is stateless, so the
    // on-commit allocates fresh state: the commit zero-fills it through the
    // clear list (the runtime RESET edge only fires on retained slots, which
    // T-M5-10's unwiring handoff covers), and the fresh instance seeds live
    // from the saved checkbox bit.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        Synth::init_default_bank(&bank);
        bank.channel_chains[0].num_effects        = 1;
        bank.channel_chains[0].effects[0].type    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true; // saved on, unwired

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].state_offs != 0); // stateful while saved on
        TEST(chains[0].effects[0].enable_param_id == 0);
        TEST(chains[0].effects[0].enabled);
        (void)Synth::take_effect_clear_ranges();

        Synth::EffectInstance& effect = chains[0].effects[0];

        // Toggle the checkbox off and publish: the unwired slot goes
        // stateless, so the state range is released.
        bank.channel_chains[0].effects[0].enabled = false;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(effect.state_offs == 0); // an unwired saved-off slot is stateless
        TEST(! effect.enabled);

        // Toggle back on and publish: fresh state arrives zero-filled via
        // the commit's clear list, and the instance seeds live from the
        // saved checkbox bit.
        bank.channel_chains[0].effects[0].enabled = true;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(effect.state_offs != 0); // the state range is re-allocated
        TEST(effect.enabled);         // seeded live from the saved checkbox bit
        TEST(effect.saved_enabled);

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);                            // the fresh state range is zero-filled
        TEST(clears.ranges[0].offset == effect.state_offs); // ...before its first dispatch
        TEST(clears.ranges[0].bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
    }

    // T-M5-12: the Enabled row takes CC routings only (user ruling
    // 2026-10-11).  Hand-authored wires from the three standard channel-wide
    // dots (bend, mod wheel, channel pressure) refuse atomically in the
    // rebuild, leaving the bank byte-identical and the routing unwired; a CC
    // wire still compiles (T-M5-7).  A hand-authored bank whose enable_input
    // names a non-CC source refuses at decode, the same refuse-not-normalize
    // rule out-of-range CC numbers follow.
    {
        add_cc_chain_fixture(&source, 0);
        source.bank.channel_chains[0].effects[0].enabled = false;

        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);

        const Sculptor::EndPoint enable_in = { mapping.effect_nodes[0], Sculptor::fx_enabled_row };
        for (uint32_t standard_dot = 0; standard_dot < Sculptor::fx_num_input_sources; ++standard_dot) {
            const Sculptor::EndPoint standard_out = { mapping.midi_node, mapping.midi_source_slots[standard_dot] };
            TEST(graph.add_connection(standard_out, enable_in) != Sculptor::pool_no_slot);

            Sculptor::GraphChange connect = {};
            connect.kind                  = Sculptor::ChangeKind::connection_added;
            connect.connection_output     = standard_out;
            connect.connection_input      = enable_in;
            candidate                     = source;
            TEST(! Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, connect));
            TEST(memcmp(&candidate, &source, sizeof(candidate)) == 0); // no routing, bank untouched

            // Drop the hand-authored wire so the next dot starts bare.
            const uint32_t wire = find_fx_graph_connection(graph, standard_out, enable_in);
            TEST(wire != Sculptor::pool_no_slot);
            TEST(graph.user_delete_connection(wire));
        }
    }

    // T-M5-12 (continued): the decode-side mirror of the CC-only rule.  Each
    // standard channel-wide source in a bank's enable_input refuses the
    // bank validation and therefore the decode itself.
    {
        for (uint32_t rogue_source = 1; rogue_source <= Sculptor::fx_num_input_sources; ++rogue_source) {
            add_cc_chain_fixture(&source, 0);
            Synth::ModInput& rogue = source.bank.channel_chains[0].effects[0].enable_input;
            rogue.source           = static_cast<Synth::ModSource>(rogue_source); // bend / mod wheel / pressure
            rogue.op               = Synth::SourceOp::add;
            rogue.scale            = 1.0f;

            TEST(! Synth::validate_instrument_bank(&source.bank));

            const uint32_t rogue_len = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
            TEST(rogue_len > 0);
            TEST(! Synth::decode_editor_bank_json(payload, rogue_len, &decoded));
        }
    }
}

// Collapsed modulator rows: a parameter with no modulator projects its
// base-value row connectable (the inline dot), the row set collapses to the
// value row, and an inline wire on the value row binds the row's first free
// modulator input through the same dot apply paths.
static void effects_inline_modulator_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           candidate     = *candidate_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph         = *graph_storage;
    Sculptor::EffectGraphMapping           mapping       = {};
    Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};

    // T-P4-1: the bare base-value row admits wires; the inline dot stays
    // connectable while any family still has a free input.  It closes only
    // when every family input is taken (T-P4-9).
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::Slot& bare_row = graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)];
    TEST(bare_row.kind == Sculptor::SlotKind::property);
    TEST(bare_row.connectable);
    TEST(! Sculptor::fx_param_field_hidden(graph, mapping.effect_nodes[0], 0, 0)); // the base row always renders
    for (uint32_t field = 1; field < 10; ++field) {
        TEST(Sculptor::fx_param_field_hidden(graph, mapping.effect_nodes[0], 0, field)); // no modulator row renders
    }

    Synth::EffectChainBinding& wired_chain              = add_cc_chain_fixture(&source, 0);
    wired_chain.effects[0].bindings[0].num_inputs       = 1;
    wired_chain.effects[0].bindings[0].inputs[0].source = Synth::ModSource::pitch_bend;
    wired_chain.effects[0].bindings[0].inputs[0].op     = Synth::SourceOp::add;
    wired_chain.effects[0].bindings[0].inputs[0].scale  = 1.0f;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    // the LFO family stays free
    TEST(graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)].connectable);

    uint16_t                   lfo_descriptor = 0;
    Synth::EffectChainBinding& lfo_chain      = add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    lfo_chain.effects[0].bindings[0].lfo_desc_id = lfo_descriptor;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    // the source family stays free
    TEST(graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)].connectable);

    // T-P4-2: an inline wire on the bare value row binds the row's first
    // free modulator input: an LFO output the LFO input with the fresh
    // depth default, a channel-wide MIDI source the first free source row
    // with the neutral op/scale.  The re-projection re-lands the wire on
    // the dot it filled.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    bool pinned_lfos[Synth::max_lfos] = {};
    pinned_lfos[lfo_descriptor - 1]   = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::EndPoint lfo_out  = { mapping.lfo_nodes[lfo_descriptor - 1], 0 };
    const Sculptor::EndPoint value_in = { mapping.effect_nodes[0], Sculptor::fx_param_row(0) };
    TEST(graph.add_connection(lfo_out, value_in) != Sculptor::pool_no_slot);
    Sculptor::GraphChange lfo_inline = {};
    lfo_inline.kind                  = Sculptor::ChangeKind::connection_added;
    lfo_inline.connection_output     = lfo_out;
    lfo_inline.connection_input      = value_in;
    candidate                        = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, lfo_inline));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == lfo_descriptor);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_depth == 0.5f);
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(find_fx_graph_connection(graph, lfo_out, { mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(0) }) !=
         Sculptor::pool_no_slot);

    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::EndPoint bend_out  = { mapping.midi_node, mapping.midi_source_slots[0] };
    const Sculptor::EndPoint inline_in = { mapping.effect_nodes[0], Sculptor::fx_param_row(0) };
    TEST(graph.add_connection(bend_out, inline_in) != Sculptor::pool_no_slot);
    Sculptor::GraphChange source_inline = {};
    source_inline.kind                  = Sculptor::ChangeKind::connection_added;
    source_inline.connection_output     = bend_out;
    source_inline.connection_input      = inline_in;
    candidate                           = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, source_inline));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source == Synth::ModSource::pitch_bend);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].op == Synth::SourceOp::add);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].scale == 1.0f);
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(find_fx_graph_connection(graph, bend_out, { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) !=
         Sculptor::pool_no_slot);

    // T-P4-5: retargeting a live source wire onto another param's inline
    // dot moves the binding: the wire leaves the previous source row (its
    // packed input forgets the source) and binds the landing row's first
    // free source input.
    add_cc_chain_fixture(&source, 0);
    Synth::EffectChainBinding& two_chain = add_cc_chain_fixture(&source, 0);
    two_chain.num_effects                = 2;
    Sculptor::fx_init_slot(&two_chain, 1, Synth::EffectType::delay);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::EndPoint row_a = { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) };
    TEST(graph.add_connection(bend_out, row_a) != Sculptor::pool_no_slot);
    Sculptor::GraphChange first_bind = {};
    first_bind.kind                  = Sculptor::ChangeKind::connection_added;
    first_bind.connection_output     = bend_out;
    first_bind.connection_input      = row_a;
    candidate                        = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, first_bind));
    const uint32_t moved_wire = find_fx_graph_connection(graph, bend_out, row_a);
    TEST(moved_wire != Sculptor::pool_no_slot);
    const Sculptor::EndPoint inline_b = { mapping.effect_nodes[1], Sculptor::fx_param_row(0) };
    graph.set_connection_input(moved_wire, inline_b);
    Sculptor::GraphChange retarget = {};
    retarget.kind                  = Sculptor::ChangeKind::connection_changed;
    retarget.connection_output     = bend_out;
    retarget.connection_input      = inline_b;
    retarget.connection_prev_input = row_a;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, retarget));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 0);
    TEST(candidate.bank.channel_chains[0].effects[1].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[1].bindings[0].inputs[0].source == Synth::ModSource::pitch_bend);
    // T-P4-3: the collapse predicate derives live from connection state:
    // no wire on the value row or any modulator dot keeps the param
    // collapsed.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const uint32_t effect_node = mapping.effect_nodes[0];
    TEST(Sculptor::fx_param_is_collapsed(graph, effect_node, 0));
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_lfo_dot(0) }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_is_collapsed(graph, effect_node, 0));

    // T-P4-4: the first-free-input choice fills LFO, then Source A, then
    // Source B, per wire family; an exhausted family refuses.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    memset(static_cast<void*>(pinned_lfos), 0, sizeof(pinned_lfos));
    pinned_lfos[lfo_descriptor - 1] = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(Sculptor::fx_param_free_input(graph, effect_node, 0, true) == Sculptor::fx_param_lfo_dot(0));
    TEST(Sculptor::fx_param_free_input(graph, effect_node, 0, false) == Sculptor::fx_param_src_dot(0, 0));
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_src_dot(0, 0) }) != Sculptor::pool_no_slot);
    TEST(Sculptor::fx_param_free_input(graph, effect_node, 0, false) == Sculptor::fx_param_src_dot(0, 1));
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_src_dot(0, 1) }) != Sculptor::pool_no_slot);
    TEST(Sculptor::fx_param_free_input(graph, effect_node, 0, false) == Sculptor::pool_no_slot);
    TEST(graph.add_connection({ mapping.lfo_nodes[lfo_descriptor - 1], 0 },
                              { effect_node, Sculptor::fx_param_lfo_dot(0) }) != Sculptor::pool_no_slot);
    TEST(Sculptor::fx_param_free_input(graph, effect_node, 0, true) == Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_is_collapsed(graph, effect_node, 0));

    // T-P4-6: per-family row visibility - with exactly one LFO wire on a
    // param, the LFO row renders and both MIDI source rows stay hidden.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    memset(static_cast<void*>(pinned_lfos), 0, sizeof(pinned_lfos));
    pinned_lfos[lfo_descriptor - 1] = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection({ mapping.lfo_nodes[lfo_descriptor - 1], 0 },
                              { effect_node, Sculptor::fx_param_lfo_dot(0) }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 0)); // the base row always renders
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 1));
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 2));
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 3)); // the connected LFO row renders
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 4));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 5));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 6)); // Source A stays hidden
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 7));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 8));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 9)); // Source B stays hidden

    // T-P4-7: with one MIDI source wire on Source A, that source row
    // renders and the LFO row and Source B stay hidden.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_src_dot(0, 0) }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 0));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 1));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 2));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 3)); // the LFO row stays hidden
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 4));
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 5));
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 6)); // the connected Source A row renders
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 7));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 8));
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 9)); // Source B stays hidden

    // T-P4-8: with modulators on two families, every connected family's
    // row renders and the unconnected family stays hidden; adding the last
    // family opens its row too.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    memset(static_cast<void*>(pinned_lfos), 0, sizeof(pinned_lfos));
    pinned_lfos[lfo_descriptor - 1] = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(graph.add_connection({ mapping.lfo_nodes[lfo_descriptor - 1], 0 },
                              { effect_node, Sculptor::fx_param_lfo_dot(0) }) != Sculptor::pool_no_slot);
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_src_dot(0, 0) }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 1)); // the connected LFO row renders
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 4)); // the connected Source A row renders
    TEST(Sculptor::fx_param_field_hidden(graph, effect_node, 0, 7));   // the unconnected Source B stays hidden
    TEST(graph.add_connection(bend_out, { effect_node, Sculptor::fx_param_src_dot(0, 1) }) != Sculptor::pool_no_slot);
    TEST(! Sculptor::fx_param_field_hidden(graph, effect_node, 0, 7)); // the connected Source B row renders

    // T-P4-9: a wire dropped on the base-value row while another family is
    // already connected lands on that family's dot after re-projection;
    // with every family input taken the base-row dot closes again.
    Synth::EffectChainBinding& hand_wired              = add_cc_chain_fixture(&source, 0);
    hand_wired.effects[0].bindings[0].num_inputs       = 1;
    hand_wired.effects[0].bindings[0].inputs[0].source = Synth::ModSource::pitch_bend;
    hand_wired.effects[0].bindings[0].inputs[0].op     = Synth::SourceOp::add;
    hand_wired.effects[0].bindings[0].inputs[0].scale  = 1.0f;
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    memset(static_cast<void*>(pinned_lfos), 0, sizeof(pinned_lfos));
    pinned_lfos[lfo_descriptor - 1] = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::EndPoint lfo_drop_out = { mapping.lfo_nodes[lfo_descriptor - 1], 0 };
    const Sculptor::EndPoint base_row_in  = { effect_node, Sculptor::fx_param_row(0) };
    TEST(graph.add_connection(lfo_drop_out, base_row_in) != Sculptor::pool_no_slot);
    Sculptor::GraphChange lfo_drop = {};
    lfo_drop.kind                  = Sculptor::ChangeKind::connection_added;
    lfo_drop.connection_output     = lfo_drop_out;
    lfo_drop.connection_input      = base_row_in;
    candidate                      = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, lfo_drop));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id == lfo_descriptor);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].lfo_depth == 0.5f);
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(find_fx_graph_connection(graph, lfo_drop_out, { mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(0) }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, lfo_drop_out, { mapping.effect_nodes[0], Sculptor::fx_param_row(0) }) ==
         Sculptor::pool_no_slot);

    // The mirror: with the LFO family connected, a source wire dropped on
    // the base row binds Source A; a second wire on Source B fills the
    // last input and the base-row dot closes.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &lfo_descriptor));
    memset(static_cast<void*>(pinned_lfos), 0, sizeof(pinned_lfos));
    pinned_lfos[lfo_descriptor - 1]                                  = true;
    source.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = lfo_descriptor;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const Sculptor::EndPoint fresh_base_row = { effect_node, Sculptor::fx_param_row(0) };
    TEST(graph.add_connection(bend_out, fresh_base_row) != Sculptor::pool_no_slot);
    Sculptor::GraphChange source_drop = {};
    source_drop.kind                  = Sculptor::ChangeKind::connection_added;
    source_drop.connection_output     = bend_out;
    source_drop.connection_input      = fresh_base_row;
    candidate                         = source;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, source_drop));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 1);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[0].source == Synth::ModSource::pitch_bend);
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(find_fx_graph_connection(graph, bend_out, { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) }) !=
         Sculptor::pool_no_slot);
    const Sculptor::EndPoint wheel_out = { mapping.midi_node, mapping.midi_source_slots[1] };
    const Sculptor::EndPoint source_b  = { effect_node, Sculptor::fx_param_src_dot(0, 1) };
    TEST(graph.add_connection(wheel_out, source_b) != Sculptor::pool_no_slot);
    Sculptor::GraphChange second_source = {};
    second_source.kind                  = Sculptor::ChangeKind::connection_added;
    second_source.connection_output     = wheel_out;
    second_source.connection_input      = source_b;
    TEST(Sculptor::apply_fx_graph_change(&candidate.bank, graph, mapping, second_source));
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].num_inputs == 2);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].inputs[1].source == Synth::ModSource::mod_wheel);
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping, pinned_lfos));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(! graph.node(effect_node).slots.entries[Sculptor::fx_param_row(0)].connectable); // every family input taken
}

// T-M6-11: the fx pane's MIDI Inputs node move must survive the re-projection an
// Add CC triggers: the move commits a kind-4 record keyed by the node name.
static void fx_inputs_move_reprojection_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           candidate       = *candidate_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage   = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph           = *graph_storage;
    const std::unique_ptr<Sculptor::Graph> regraph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       regraph         = *regraph_storage;
    Sculptor::EffectGraphMapping           mapping         = {};
    Sculptor::EffectGraphMapping           remapping       = {};
    Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    // Drag the MIDI Inputs node: live move, no change event.
    const vmath::vec2 moved(312.0f, 88.0f);
    graph.set_node_layout(mapping.midi_node, moved, 0, 0);
    // The fx move commit captures moved nodes into the candidate bank.
    bool capture_nodes[Sculptor::max_nodes] = {};
    capture_nodes[mapping.midi_node]        = true;
    TEST(Sculptor::apply_effect_graph_batch(&source, &graph, &mapping, nullptr, 0, capture_nodes, &candidate));
    TEST(Synth::validate_instrument_bank(&candidate.bank));
    TEST(Sculptor::validate_editor_metadata(candidate));

    // Adding CC 16 commits the mask bit and re-projects the fx graph.
    candidate.added_cc_mask[0] = 1u << 0;
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &regraph, &remapping));
    TEST(remapping.midi_node != Sculptor::pool_no_slot);
    // The editor's re-projection post-pass restores kind-4 records by the
    // projected node's live name; emulate it to pin the key agreement.
    const Synth::GraphNodeLayout* restored = nullptr;
    for (uint32_t i = 0; i < candidate.graph_layout_count && ! restored; i++) {
        if (candidate.graph_layout[i].kind == 4 &&
            strcmp(candidate.graph_layout[i].name, regraph.node(remapping.midi_node).name) == 0) {
            restored = &candidate.graph_layout[i];
        }
    }
    TEST(restored != nullptr);
    TEST(restored->x == moved.x);
    TEST(restored->y == moved.y);
    // A kind-4 record keyed "MIDI" predates the node's retitle to
    // "MIDI Inputs"; normalization drops it instead of refusing the bank.
    const std::unique_ptr<Synth::InstrumentEditorBank> legacy_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       legacy         = *legacy_storage;
    legacy                                                            = source;
    Synth::GraphNodeLayout& stale = legacy.graph_layout[legacy.graph_layout_count++];
    stale                         = {};
    stale.kind                    = 4;
    strcpy(stale.name, "MIDI");
    stale.x = 40.0f;
    stale.y = 50.0f;
    TEST(Sculptor::normalize_effect_layout(&legacy));
    TEST(legacy.graph_layout_count == source.graph_layout_count);
}

// ---- Flag-routing model tests ----
// These tests target the replacement routing model: per-slot EffectOut flags
// plus a per-chain in flag, driven through the same
// candidate seams the canvas gestures use.

// Routing fixture: chain `owner` holds `count` enabled effects of four
// distinct types with base values 1..count, so payload identity stays
// assertable after every reorder.  Every owner's routing table zeroes.
static void build_routing_fixture(Synth::InstrumentEditorBank* bank, uint32_t owner, uint32_t count)
{
    memset(static_cast<void*>(bank), 0, sizeof(*bank));
    Synth::init_default_bank(&bank->bank);
    Sculptor::set_identity_osc_sources(bank, 0, 0);
    Synth::EffectChainBinding& chain   = Sculptor::fx_graph_chain(&bank->bank, owner);
    const Synth::EffectType    types[] = { Synth::EffectType::delay,
                                           Synth::EffectType::chorus,
                                           Synth::EffectType::distortion,
                                           Synth::EffectType::reverb };
    chain.num_effects                  = static_cast<uint8_t>(count);
    for (uint32_t slot = 0; slot < count; ++slot) {
        Sculptor::fx_init_slot(&chain, slot, types[slot & 3]);
        chain.effects[slot].enabled                = true;
        chain.effects[slot].bindings[0].base_value = 1.0f + static_cast<float>(slot);
    }
}

// The output node dragged left of the rightmost effect keeps the moved
// place: the drag opts it out of the arrangement, the release commit
// stores a kind-4 record, and the next projection restores that place
// with the auto-lane stamp cleared.
static void fx_output_node_move_regressions()
{
    std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                 bank          = *bank_storage;
    std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                             graph         = *graph_storage;
    Sculptor::EffectGraphMapping                 mapping       = {};
    Sculptor::GraphChange                        discarded[Sculptor::max_pending_changes] = {};
    build_routing_fixture(&bank, 0, 3);
    bank.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    bank.effect_routing[0].out[0] = Sculptor::effect_out_next;
    bank.effect_routing[0].out[1] = Sculptor::effect_out_next;
    bank.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Synth::validate_instrument_bank(&bank.bank));
    TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    float widths[Sculptor::max_nodes] = {};
    widths[mapping.input_node]        = 64.0f;
    widths[mapping.effect_nodes[0]]   = 96.0f;
    widths[mapping.effect_nodes[1]]   = 96.0f;
    widths[mapping.effect_nodes[2]]   = 96.0f;
    widths[mapping.output_node]       = 64.0f;
    TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
    // The user grabs the arranged output node and drags it left of the
    // rightmost effect. The snapped drag target is a full grid step left.
    const Sculptor::Node& output = graph.node(mapping.output_node);
    const vmath::vec2     grab(output.position.x + 8.0f, output.position.y + 8.0f);
    graph.begin_node_drag(mapping.output_node, grab);
    const vmath::vec2 target(output.position.x - 16.0f, output.position.y);
    graph.apply_node_drag(grab + vmath::vec2(-16.0f, 0.0f));
    TEST(! graph.node_auto_lane(mapping.output_node));
    TEST(graph.node(mapping.output_node).position.x == target.x);
    // The pane's release commit: capture the moved node into a kind-4
    // record the way node_layout_moved marks it.
    bool capture_nodes[Sculptor::max_nodes]                        = {};
    capture_nodes[mapping.output_node]                             = true;
    std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                 candidate         = *candidate_storage;
    TEST(Sculptor::apply_effect_graph_batch(&bank, &graph, &mapping, nullptr, 0, capture_nodes, &candidate));
    // The next projection restores the record and the arrangement must
    // leave the moved output where the user put it.
    TEST(Sculptor::project_effect_chain_to_graph(candidate, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    bool restored = false;
    for (uint32_t i = 0; i < candidate.graph_layout_count; ++i) {
        const Synth::GraphNodeLayout& record = candidate.graph_layout[i];
        if (record.kind == 4 && strcmp(record.name, "Channel output") == 0) {
            graph.set_node_layout(mapping.output_node,
                                  vmath::vec2(record.x, record.y),
                                  record.width_override,
                                  record.height_override);
            restored = true;
        }
    }
    TEST(restored);
    TEST(! graph.node_auto_lane(mapping.output_node));
    TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
    TEST(graph.node(mapping.output_node).position.x == target.x);
    TEST(! Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
}

static void fx_auto_lane_spacing_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};
    Sculptor::GraphChange                              discarded[Sculptor::max_pending_changes] = {};
    const uint32_t                                     owners[] = { 0u, Sculptor::fx_master_chain };
    const uint32_t                                     counts[] = { 0u, 1u, Synth::max_chain_effects };
    for (const uint32_t owner : owners) {
        for (const uint32_t count : counts) {
            add_cc_chain_fixture(&source, owner);
            Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&source.bank, owner);
            chain.num_effects                = static_cast<uint8_t>(count);
            for (uint32_t slot = 0; slot < count; ++slot) {
                Sculptor::fx_init_slot(&chain, slot, Synth::EffectType::delay);
                chain.effects[slot].enabled = true;
            }
            TEST(Synth::validate_instrument_bank(&source.bank));
            TEST(Sculptor::project_effect_chain_to_graph(source, owner, &graph, &mapping));
            graph.take_changes(discarded, Sculptor::max_pending_changes);
            // Creation place: every default node starts at its lane's grid
            // x 0; the arrangement owns its x afterwards.
            TEST(graph.node(mapping.input_node).position.x == 0.0f);
            TEST(graph.node_auto_lane(mapping.input_node));
            TEST(graph.node_auto_lane(mapping.output_node));
            TEST(! graph.node_auto_lane(mapping.midi_node));
            for (uint32_t i = 0; i < count; ++i) {
                TEST(mapping.effect_nodes[i] != Sculptor::pool_no_slot);
                TEST(graph.node_auto_lane(mapping.effect_nodes[i]));
                TEST(graph.node(mapping.effect_nodes[i]).position.x == 0.0f);
            }
            TEST(graph.node(mapping.output_node).position.x == 0.0f);
            const char* const input_name  = owner == Sculptor::fx_master_chain ? "All channels" : "Channel input";
            const char* const output_name = owner == Sculptor::fx_master_chain ? "Master output" : "Channel output";
            TEST(strcmp(graph.node(mapping.input_node).name, input_name) == 0);
            TEST(strcmp(graph.node(mapping.output_node).name, output_name) == 0);
        }
    }

    // Synthetic widths: every gap between two arranged lane neighbors is
    // exactly 32 px edge to edge, no grid snapping - one effect carries a
    // non-grid width (65), so the nodes after it land between grid lines.
    {
        constexpr float                                    gap = 32.0f;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        build_routing_fixture(&bank, 0, 3);
        bank.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        bank.effect_routing[0].out[0] = Sculptor::effect_out_next;
        bank.effect_routing[0].out[1] = Sculptor::effect_out_next;
        bank.effect_routing[0].out[2] = Sculptor::effect_out_output;
        uint16_t desc[2]              = {};
        TEST(Sculptor::allocate_default_lfo(&bank.bank, &desc[0]));
        TEST(Sculptor::allocate_default_lfo(&bank.bank, &desc[1]));
        Synth::EffectChainBinding& chain         = Sculptor::fx_graph_chain(&bank.bank, 0);
        chain.effects[0].bindings[0].lfo_desc_id = desc[0];
        chain.effects[0].bindings[0].lfo_depth   = 0.25f;
        chain.effects[1].bindings[0].lfo_desc_id = desc[1];
        chain.effects[1].bindings[0].lfo_depth   = 0.25f;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.lfo_count == 2);

        float widths[Sculptor::max_nodes] = {};
        widths[mapping.input_node]        = 64.0f;
        widths[mapping.effect_nodes[0]]   = 65.0f;
        widths[mapping.effect_nodes[1]]   = 128.0f;
        widths[mapping.effect_nodes[2]]   = 160.0f;
        widths[mapping.output_node]       = 64.0f;
        widths[mapping.midi_node]         = 144.0f;
        widths[mapping.lfo_nodes[0]]      = 80.0f;
        widths[mapping.lfo_nodes[1]]      = 80.0f;
        TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
        TEST(graph.node(mapping.input_node).position.x == 0.0f);
        TEST(graph.node(mapping.effect_nodes[0]).position.x == 64.0f + gap);
        TEST(graph.node(mapping.effect_nodes[1]).position.x == 64.0f + gap + 65.0f + gap);
        TEST(graph.node(mapping.effect_nodes[2]).position.x == 64.0f + gap + 65.0f + gap + 128.0f + gap);
        TEST(graph.node(mapping.output_node).position.x == 64.0f + gap + 65.0f + gap + 128.0f + gap + 160.0f + gap);
        // Column alignment: LFO i takes effect i's arranged x.
        TEST(graph.node(mapping.lfo_nodes[0]).position.x == graph.node(mapping.effect_nodes[0]).position.x);
        TEST(graph.node(mapping.lfo_nodes[1]).position.x == graph.node(mapping.effect_nodes[1]).position.x);
        // Idempotent: nothing moves on the second pass.
        TEST(! Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));

        // A user move opts the node out for good: the moved box keeps its
        // place and anchors the remaining arranged nodes.
        const vmath::vec2 moved(1024.0f, 0.0f);
        graph.set_node_layout(mapping.effect_nodes[1], moved, 0, 0);
        TEST(! graph.node_auto_lane(mapping.effect_nodes[1]));
        TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
        TEST(graph.node(mapping.effect_nodes[1]).position.x == moved.x);
        TEST(graph.node(mapping.effect_nodes[2]).position.x == moved.x + widths[mapping.effect_nodes[1]] + gap);
        TEST(graph.node(mapping.output_node).position.x ==
             moved.x + widths[mapping.effect_nodes[1]] + gap + widths[mapping.effect_nodes[2]] + gap);
    }

    // LFOs past the effect count keep stepping 32 px on their lane.
    {
        constexpr float                                    gap = 32.0f;
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        build_routing_fixture(&bank, 0, 1);
        bank.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        bank.effect_routing[0].out[0] = Sculptor::effect_out_output;
        uint16_t desc[2]              = {};
        TEST(Sculptor::allocate_default_lfo(&bank.bank, &desc[0]));
        TEST(Sculptor::allocate_default_lfo(&bank.bank, &desc[1]));
        Synth::EffectChainBinding& chain         = Sculptor::fx_graph_chain(&bank.bank, 0);
        chain.effects[0].bindings[0].lfo_desc_id = desc[0];
        chain.effects[0].bindings[0].lfo_depth   = 0.25f;
        chain.effects[0].bindings[1].lfo_desc_id = desc[1];
        chain.effects[0].bindings[1].lfo_depth   = 0.25f;
        TEST(Synth::validate_instrument_bank(&bank.bank));
        TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.lfo_count == 2);

        float widths[Sculptor::max_nodes] = {};
        widths[mapping.input_node]        = 64.0f;
        widths[mapping.effect_nodes[0]]   = 96.0f;
        widths[mapping.output_node]       = 64.0f;
        widths[mapping.midi_node]         = 144.0f;
        widths[mapping.lfo_nodes[0]]      = 65.0f;
        widths[mapping.lfo_nodes[1]]      = 96.0f;
        TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
        // One effect: LFO 0 aligns with it; LFO 1 steps 32 px past LFO 0.
        TEST(graph.node(mapping.lfo_nodes[0]).position.x == graph.node(mapping.effect_nodes[0]).position.x);
        TEST(graph.node(mapping.lfo_nodes[1]).position.x ==
             graph.node(mapping.lfo_nodes[0]).position.x + widths[mapping.lfo_nodes[0]] + gap);
        TEST(! Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
    }

    // Drag seam: a stationary hold moves nothing, so a between-grid placed
    // node keeps its exact place and its auto-lane stamp, and a selected
    // companion keeps both as well. Any mouse movement that changes the
    // snapped target is a drag: the node snaps, and auto-arrangement is
    // opted out. Returning to the exact press point restores the pre-drag
    // placement and stamps.
    {
        constexpr float                                    gap = 32.0f;
        const std::unique_ptr<Synth::InstrumentEditorBank> hold_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& hold = *hold_storage;
        build_routing_fixture(&hold, 0, 3);
        hold.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        hold.effect_routing[0].out[0] = Sculptor::effect_out_next;
        hold.effect_routing[0].out[1] = Sculptor::effect_out_next;
        hold.effect_routing[0].out[2] = Sculptor::effect_out_output;
        TEST(Synth::validate_instrument_bank(&hold.bank));
        TEST(Sculptor::project_effect_chain_to_graph(hold, 0, &graph, &mapping));
        graph.take_changes(discarded, Sculptor::max_pending_changes);

        float widths[Sculptor::max_nodes] = {};
        widths[mapping.input_node]        = 64.0f;
        widths[mapping.effect_nodes[0]]   = 65.0f;
        widths[mapping.effect_nodes[1]]   = 128.0f;
        widths[mapping.effect_nodes[2]]   = 160.0f;
        widths[mapping.output_node]       = 64.0f;
        widths[mapping.midi_node]         = 144.0f;
        TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
        const uint32_t held_node      = mapping.effect_nodes[1];
        const uint32_t companion_node = mapping.effect_nodes[2];
        TEST(graph.node(held_node).position.x == 64.0f + gap + 65.0f + gap); // 193: between grid lines
        TEST(graph.node(companion_node).position.x == 64.0f + gap + 65.0f + gap + 128.0f + gap);
        const vmath::vec2 held_place      = graph.node(held_node).position;
        const vmath::vec2 companion_place = graph.node(companion_node).position;
        graph.set_selected(held_node, true);
        graph.set_selected(companion_node, true);
        graph.begin_node_drag(held_node, held_place);
        graph.apply_node_drag(held_place); // stationary hold: the mouse never moved
        TEST(graph.node(held_node).position.x == 193.0f);
        TEST(graph.node(held_node).position.y == held_place.y);
        TEST(graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x);
        TEST(graph.node(companion_node).position.y == companion_place.y);
        TEST(graph.node_auto_lane(companion_node));
        // A real drag through the same seam moves: the held node snaps to
        // the grid and both dragged nodes lose their stamps.
        graph.apply_node_drag(vmath::vec2(held_place.x + 3.0f, held_place.y));
        const vmath::vec2 snapped(Sculptor::snap_to_grid(held_place.x + 3.0f), Sculptor::snap_to_grid(held_place.y));
        TEST(graph.node(held_node).position.x == snapped.x);
        TEST(! graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x + (snapped.x - held_place.x));
        TEST(! graph.node_auto_lane(companion_node));
        // Returning to the exact press point after that applied move
        // restores the pre-drag placement and stamps of the held node and
        // the selected companion: the whole drag is a no-op.
        graph.apply_node_drag(held_place);
        TEST(graph.node(held_node).position.x == held_place.x);
        TEST(graph.node(held_node).position.y == held_place.y);
        TEST(graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x);
        TEST(graph.node(companion_node).position.y == companion_place.y);
        TEST(graph.node_auto_lane(companion_node));
        // Drag away by a full grid step (+32): the node snaps to 224 and
        // both stamps clear; returning to the exact press point restores
        // the between-grid placement (x=193) and the stamps again.
        graph.apply_node_drag(vmath::vec2(held_place.x + 32.0f, held_place.y));
        TEST(graph.node(held_node).position.x == 224.0f);
        TEST(! graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x + 31.0f);
        TEST(! graph.node_auto_lane(companion_node));
        graph.apply_node_drag(held_place);
        TEST(graph.node(held_node).position.x == held_place.x);
        TEST(graph.node(held_node).position.y == held_place.y);
        TEST(graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x);
        TEST(graph.node(companion_node).position.y == companion_place.y);
        TEST(graph.node_auto_lane(companion_node));
        // Grid-aligned case (x=192): the same drag away and return through
        // the seam restores the aligned placement and the stamps too.
        graph.place_auto_lane_node(held_node, vmath::vec2(192.0f, held_place.y));
        const vmath::vec2 aligned_place = graph.node(held_node).position;
        graph.begin_node_drag(held_node, aligned_place);
        graph.apply_node_drag(vmath::vec2(aligned_place.x + 32.0f, aligned_place.y));
        TEST(graph.node(held_node).position.x == 224.0f);
        TEST(! graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x + 32.0f);
        TEST(! graph.node_auto_lane(companion_node));
        graph.apply_node_drag(aligned_place);
        TEST(graph.node(held_node).position.x == aligned_place.x);
        TEST(graph.node(held_node).position.y == aligned_place.y);
        TEST(graph.node_auto_lane(held_node));
        TEST(graph.node(companion_node).position.x == companion_place.x);
        TEST(graph.node(companion_node).position.y == companion_place.y);
        TEST(graph.node_auto_lane(companion_node));
    }

    // A user-moved output node keeps its place: the move commits a
    // name-keyed kind-4 record, the projection still computes its own
    // default place, and the record survives re-projection.  The GUI's
    // restoration post-pass lives in the editor TU and is covered by the
    // user smoke gate.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const vmath::vec2 moved(2400.0f, 96.0f);
    graph.set_node_layout(mapping.output_node, moved, 0, 0);
    Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
    memset(static_cast<void*>(&record), 0, sizeof(record));
    record.channel = 0;
    record.kind    = 4;
    strcpy(record.name, graph.node(mapping.output_node).name);
    record.x = moved.x;
    record.y = moved.y;
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::validate_editor_metadata(source));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    TEST(graph.node(mapping.output_node).position.x == 0.0f);
    const Synth::GraphNodeLayout* restored = nullptr;
    for (uint32_t i = 0; i < source.graph_layout_count && ! restored; i++) {
        if (source.graph_layout[i].kind == 4 &&
            strcmp(source.graph_layout[i].name, graph.node(mapping.output_node).name) == 0) {
            restored = &source.graph_layout[i];
        }
    }
    TEST(restored != nullptr);
    TEST(restored->x == moved.x);
    TEST(restored->y == moved.y);
}

// Counts the drop-menu-flagged slots across every occupied node's slots.
static uint32_t count_drop_menu_slots(const Sculptor::Graph& graph)
{
    uint32_t flagged = 0;
    for (uint32_t node_idx = 0; node_idx < Sculptor::max_nodes; ++node_idx) {
        if (! graph.node_occupied(node_idx)) {
            continue;
        }
        const Sculptor::Node& node_ref = graph.node(node_idx);
        for (uint32_t slot_idx = 0; slot_idx < node_ref.slots.num_allocated; ++slot_idx) {
            if (node_ref.slots.entries[slot_idx].drop_menu) {
                ++flagged;
            }
        }
    }
    return flagged;
}

// A fresh drag session fully re-snapshots the restore arrays: a
// return-to-press in session two must never restore session one's
// placements, and a later session restores its own press point.
static void fx_drag_session_reset_regressions()
{
    std::unique_ptr<Synth::InstrumentEditorBank> bank_storage  = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                 bank          = *bank_storage;
    std::unique_ptr<Sculptor::Graph>             graph_storage = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                             graph         = *graph_storage;
    Sculptor::EffectGraphMapping                 mapping       = {};
    Sculptor::GraphChange                        discarded[Sculptor::max_pending_changes] = {};
    build_routing_fixture(&bank, 0, 3);
    bank.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    bank.effect_routing[0].out[0] = Sculptor::effect_out_next;
    bank.effect_routing[0].out[1] = Sculptor::effect_out_next;
    bank.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Synth::validate_instrument_bank(&bank.bank));
    TEST(Sculptor::project_effect_chain_to_graph(bank, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    float widths[Sculptor::max_nodes] = {};
    widths[mapping.input_node]        = 64.0f;
    widths[mapping.effect_nodes[0]]   = 64.0f;
    widths[mapping.effect_nodes[1]]   = 64.0f;
    widths[mapping.effect_nodes[2]]   = 64.0f;
    widths[mapping.output_node]       = 64.0f;
    TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
    const uint32_t    node_a  = mapping.effect_nodes[0];
    const uint32_t    node_b  = mapping.effect_nodes[1];
    const vmath::vec2 place_a = graph.node(node_a).position;
    const vmath::vec2 place_b = graph.node(node_b).position;
    // Session one: node A opts out of the arrangement by one step.
    graph.begin_node_drag(node_a, place_a + vmath::vec2(8.0f, 8.0f));
    graph.apply_node_drag(place_a + vmath::vec2(8.0f + 32.0f, 8.0f));
    TEST(graph.node(node_a).position.x == place_a.x + 32.0f);
    TEST(! graph.node_auto_lane(node_a));
    // Session two on node B: a return to B's own press point restores
    // only B's session-two state. A keeps its moved place and its
    // cleared stamp: no stale session-one restore may fire.
    graph.begin_node_drag(node_b, place_b + vmath::vec2(8.0f, 8.0f));
    graph.apply_node_drag(place_b + vmath::vec2(8.0f + 32.0f, 8.0f));
    TEST(graph.node(node_b).position.x == place_b.x + 32.0f);
    graph.apply_node_drag(place_b + vmath::vec2(8.0f, 8.0f));
    TEST(graph.node(node_b).position.x == place_b.x);
    TEST(graph.node_auto_lane(node_b));
    TEST(graph.node(node_a).position.x == place_a.x + 32.0f);
    TEST(! graph.node_auto_lane(node_a));
    // Session three on A from its moved place: returning to that
    // press point restores the moved place and the cleared stamp,
    // not the original auto-lane placement from session one.
    const vmath::vec2 place_a2 = graph.node(node_a).position;
    graph.begin_node_drag(node_a, place_a2 + vmath::vec2(8.0f, 8.0f));
    graph.apply_node_drag(place_a2 + vmath::vec2(8.0f + 32.0f, 8.0f));
    TEST(graph.node(node_a).position.x == place_a2.x + 32.0f);
    graph.apply_node_drag(place_a2 + vmath::vec2(8.0f, 8.0f));
    TEST(graph.node(node_a).position.x == place_a2.x);
    TEST(graph.node(node_a).position.y == place_a2.y);
    TEST(! graph.node_auto_lane(node_a));
}

// T-M3-1.3: the fx projection flags the M1 drop-menu anchor on every
// projected effect's base-value row while that parameter is unconnected
// (no LFO wire, no source wire) and modulator capacity remains; wired
// parameters and an exhausted modulator capacity leave the flag off,
// fields 1-9 never carry it, and no fx node other than an eligible base
// row does (the fx projection never flags an oscillator-pane shape).

static void fx_drop_anchor_flag_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};
    Sculptor::GraphChange                              discarded[Sculptor::max_pending_changes] = {};
    add_cc_chain_fixture(&source, 0);
    // One fresh descriptor for the LFO-wired parameter.
    uint16_t descriptor = 0;
    TEST(Sculptor::allocate_default_lfo(&source.bank, &descriptor));
    Synth::EffectChainBinding& chain = Sculptor::fx_graph_chain(&source.bank, 0);
    chain.num_effects                = 3;
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion); // two unconnected params
    Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);      // LFO, source and capacity cases
    Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::chorus);     // three unconnected params
    for (uint32_t slot = 0; slot < 3; ++slot) {
        chain.effects[slot].enabled = true;
    }
    chain.effects[1].bindings[0].lfo_desc_id      = descriptor;
    chain.effects[1].bindings[0].lfo_depth        = 0.5f;
    chain.effects[1].bindings[1].num_inputs       = 1;
    chain.effects[1].bindings[1].inputs[0].source = Synth::ModSource::mod_wheel;
    chain.effects[1].bindings[1].inputs[0].op     = Synth::SourceOp::multiply;
    chain.effects[1].bindings[1].inputs[0].scale  = 0.5f;
    chain.effects[1].bindings[2].num_inputs       = Synth::max_mod_inputs;
    chain.effects[1].bindings[2].inputs[0].source = Synth::ModSource::channel_pressure;
    chain.effects[1].bindings[2].inputs[0].op     = Synth::SourceOp::add;
    chain.effects[1].bindings[2].inputs[0].scale  = 0.5f;
    chain.effects[1].bindings[2].inputs[1].source = Synth::ModSource::mod_wheel;
    chain.effects[1].bindings[2].inputs[1].op     = Synth::SourceOp::multiply;
    chain.effects[1].bindings[2].inputs[1].scale  = 0.25f;
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    const uint32_t distortion_node = mapping.effect_nodes[0];
    const uint32_t delay_node      = mapping.effect_nodes[1];
    const uint32_t chorus_node     = mapping.effect_nodes[2];
    TEST(distortion_node != Sculptor::pool_no_slot);
    TEST(delay_node != Sculptor::pool_no_slot);
    TEST(chorus_node != Sculptor::pool_no_slot);
    // The eligible anchors: every base-value row of an unconnected
    // parameter with modulator capacity left.
    for (uint32_t param = 0; param < 3; ++param) {
        TEST(graph.node(distortion_node).slots.entries[Sculptor::fx_param_row(param)].drop_menu);
    }
    // A wired parameter leaves the flag off: the LFO wire, the source
    // wire, and the exhausted modulator capacity (both source rows wired).
    TEST(! graph.node(delay_node).slots.entries[Sculptor::fx_param_row(0)].drop_menu);
    TEST(! graph.node(delay_node).slots.entries[Sculptor::fx_param_row(1)].drop_menu);
    TEST(! graph.node(delay_node).slots.entries[Sculptor::fx_param_row(2)].drop_menu);
    for (uint32_t param = 0; param < 3; ++param) {
        TEST(graph.node(chorus_node).slots.entries[Sculptor::fx_param_row(param)].drop_menu);
    }
    // Fields 1-9 of every parameter block never carry the flag.
    const uint32_t effect_nodes[]  = { distortion_node, delay_node, chorus_node };
    const uint32_t effect_params[] = { 3u, 3u, 3u };
    for (uint32_t effect = 0; effect < 3; ++effect) {
        for (uint32_t param = 0; param < effect_params[effect]; ++param) {
            for (uint32_t field = 1; field < Sculptor::fx_param_stride; ++field) {
                TEST(! graph.node(effect_nodes[effect]).slots.entries[Sculptor::fx_param_row(param) + field].drop_menu);
            }
        }
    }
    // The whole graph: exactly the six eligible base rows carry the flag,
    // so the input, output, MIDI and LFO nodes carry none.
    TEST(count_drop_menu_slots(graph) == 6);

    // With no effect projected, no fx node carries the flag.
    add_cc_chain_fixture(&source, 0);
    Sculptor::fx_graph_chain(&source.bank, 0).num_effects = 0;
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(count_drop_menu_slots(graph) == 0);
}

// T-M3-1.3 (modeled): the linkable add-path semantics the fx drop-menu
// consumer relies on.  The descriptor allocation refuses a full LFO pool,
// allocates the next descriptor with the family defaults and writes the
// owner-scoped kind-4 record; and a binding preset the way the consumer
// leaves it re-projects into an LFO node whose Value output wires the
// parameter's LFO dot, with the Op and Depth rows live.
static void fx_add_lfo_modeled_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> refused_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       refused = *refused_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> refused_sentinel_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&           refused_sentinel = *refused_sentinel_storage;
    const std::unique_ptr<Sculptor::Graph> graph_storage    = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                       graph            = *graph_storage;
    Sculptor::EffectGraphMapping           mapping          = {};
    Sculptor::GraphChange                  discarded[Sculptor::max_pending_changes] = {};
    add_cc_chain_fixture(&source, 0);
    // The allocation: the next descriptor id with the family defaults,
    // plus the owner-scoped kind-4 record frozen at the caller's add-time
    // place (the LFO lane y here, a measured lane x in the add scenarios
    // below - never a stride place).
    uint16_t descriptor = 0;
    TEST(Sculptor::add_effect_lfo_candidate(&source, 0, &candidate, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(descriptor == source.bank.lfos.num_allocated + 1);
    TEST(candidate.bank.lfos.num_allocated == 1);
    const Synth::LFODescriptor& fresh = candidate.bank.lfos.entries[descriptor - 1];
    TEST(fresh.wave == Synth::WaveType::sine_wave);
    TEST(fresh.period_ms == 250);
    TEST(fresh.min_value == -1.0f);
    TEST(fresh.min_max_delta == 2.0f);
    TEST(candidate.graph_layout_count == source.graph_layout_count + 1);
    const Synth::GraphNodeLayout& record = candidate.graph_layout[candidate.graph_layout_count - 1];
    TEST(record.kind == 4);
    TEST(record.channel == 0);
    TEST(strcmp(record.name, "LFO 1") == 0);
    // The record freezes exactly the caller's add-time place, never a
    // stride place.
    TEST(record.x == 96.0f);
    TEST(record.x != 320.0f && record.x != 880.0f);
    TEST(record.y == 320.0f);
    TEST(Synth::validate_instrument_bank(&candidate.bank));
    TEST(Sculptor::validate_editor_metadata(candidate));
    // The LFO pool full: the allocation refuses and the candidate stays
    // untouched (the consumer turns the refusal into the notification).
    const std::unique_ptr<Synth::InstrumentEditorBank> full_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       full         = *full_storage;
    add_cc_chain_fixture(&full, 0);
    for (uint32_t i = 0; i < Synth::max_lfos; ++i) {
        TEST(Sculptor::allocate_default_lfo(&full.bank, &descriptor));
    }
    TEST(full.bank.lfos.num_allocated == Synth::max_lfos);
    TEST(Synth::validate_instrument_bank(&full.bank));
    memset(static_cast<void*>(&refused), 0x5A, sizeof(refused));
    refused_sentinel = refused;
    TEST(! Sculptor::add_effect_lfo_candidate(&full, 0, &refused, &descriptor, vmath::vec2(96.0f, 320.0f)));
    TEST(memcmp(&refused, &refused_sentinel, sizeof(refused)) == 0);
    // The binding preset the consumer commits: the descriptor id, the
    // audible depth seed (0.5, mirroring the drag pathway) and the add op
    // default.
    add_cc_chain_fixture(&source, 0);
    TEST(Sculptor::allocate_default_lfo(&source.bank, &descriptor));
    Synth::EffectChainBinding& chain   = Sculptor::fx_graph_chain(&source.bank, 0);
    Synth::EffectParamBinding& binding = chain.effects[0].bindings[1];
    binding.lfo_desc_id                = descriptor;
    binding.lfo_depth                  = 0.5f;
    binding.lfo_op                     = Synth::SourceOp::add;
    TEST(Synth::validate_instrument_bank(&source.bank));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    graph.take_changes(discarded, Sculptor::max_pending_changes);
    TEST(mapping.lfo_count == 1);
    TEST(mapping.lfo_nodes[0] != Sculptor::pool_no_slot);
    // The fresh LFO's Value output wires the parameter's LFO input dot.
    TEST(find_fx_graph_connection(graph,
                                  { mapping.lfo_nodes[0], 0 },
                                  { mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(1) }) !=
         Sculptor::pool_no_slot);
    TEST(graph.slot_is_connected(mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(1)));
    // The wire arms the Op and Depth knobs (the render filter that
    // un-hides the rows reads this same connected state, GUI-side).
    TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 2));
    TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 3));

    // Canvas Add LFO twice on a fresh chain (the modeled consumer): each
    // add freezes its record at the measured default x one more LFO
    // takes on the lane (measured add-time placement), and the records
    // keep the unwired descriptors represented, chain-scoped and
    // persistent; the arrangement agrees with the frozen places.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> canvas_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& canvas = *canvas_storage;
        add_cc_chain_fixture(&canvas, 0);
        uint16_t added                       = 0;
        float    lane_x                      = 0.0f;
        float    widths[Sculptor::max_nodes] = {};
        bool     pins[Synth::max_lfos]       = {};

        // First add: the empty LFO lane's next x is effect 0's column.
        // The pane extends its pin set with the records before projecting,
        // so the unwired root projects the same way.
        memset(static_cast<void*>(pins), 0, sizeof(pins));
        TEST(Sculptor::collect_effect_lfos(&canvas, 0, pins));
        TEST(Sculptor::project_effect_chain_to_graph(canvas, 0, &graph, &mapping, pins));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        widths[mapping.input_node]      = 64.0f;
        widths[mapping.effect_nodes[0]] = 96.0f;
        widths[mapping.output_node]     = 64.0f;
        widths[mapping.midi_node]       = 144.0f;
        TEST(Sculptor::fx_next_lfo_lane_x(graph, mapping, widths, &lane_x));
        TEST(lane_x == 64.0f + 32.0f);
        TEST(Sculptor::add_effect_lfo_candidate(&canvas, 0, &candidate, &added, vmath::vec2(lane_x, 320.0f)));
        canvas = candidate;

        // Second add: the lane's next x steps 32 px past the first LFO.
        memset(static_cast<void*>(pins), 0, sizeof(pins));
        TEST(Sculptor::collect_effect_lfos(&canvas, 0, pins));
        TEST(Sculptor::project_effect_chain_to_graph(canvas, 0, &graph, &mapping, pins));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.lfo_count == 1);
        TEST(pins[0]);
        memset(static_cast<void*>(widths), 0, sizeof(widths));
        widths[mapping.input_node]      = 64.0f;
        widths[mapping.effect_nodes[0]] = 96.0f;
        widths[mapping.output_node]     = 64.0f;
        widths[mapping.midi_node]       = 144.0f;
        widths[mapping.lfo_nodes[0]]    = 80.0f;
        TEST(Sculptor::fx_next_lfo_lane_x(graph, mapping, widths, &lane_x));
        TEST(lane_x == 64.0f + 32.0f + 80.0f + 32.0f);
        TEST(Sculptor::add_effect_lfo_candidate(&canvas, 0, &candidate, &added, vmath::vec2(lane_x, 320.0f)));
        canvas = candidate;

        // The frozen records: two chain-scoped kind-4 entries at the
        // measured lane x, never a stride place.
        TEST(canvas.graph_layout_count == 2);
        for (uint32_t i = 0; i < 2; ++i) {
            const Synth::GraphNodeLayout& layout_entry = canvas.graph_layout[i];
            TEST(layout_entry.kind == 4);
            TEST(layout_entry.channel == 0);
            TEST(layout_entry.x != 320.0f && layout_entry.x != 880.0f);
            TEST(layout_entry.y == 320.0f);
        }
        TEST(strcmp(canvas.graph_layout[0].name, "LFO 1") == 0);
        TEST(strcmp(canvas.graph_layout[1].name, "LFO 2") == 0);
        TEST(canvas.graph_layout[0].x == 96.0f);
        TEST(canvas.graph_layout[1].x == 208.0f);

        // The restored nodes sit exactly at the record places, and the
        // arrangement agrees with them: it spreads the effects lane and
        // then has nothing left to move.
        memset(static_cast<void*>(pins), 0, sizeof(pins));
        TEST(Sculptor::collect_effect_lfos(&canvas, 0, pins));
        TEST(Sculptor::project_effect_chain_to_graph(canvas, 0, &graph, &mapping, pins));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.lfo_count == 2);
        memset(static_cast<void*>(widths), 0, sizeof(widths));
        widths[mapping.input_node]      = 64.0f;
        widths[mapping.effect_nodes[0]] = 96.0f;
        widths[mapping.output_node]     = 64.0f;
        widths[mapping.midi_node]       = 144.0f;
        widths[mapping.lfo_nodes[0]]    = 80.0f;
        widths[mapping.lfo_nodes[1]]    = 96.0f;
        for (uint32_t i = 0; i < 2; ++i) {
            const Synth::GraphNodeLayout& layout_entry = canvas.graph_layout[i];
            graph.set_node_layout(mapping.lfo_nodes[i], vmath::vec2(layout_entry.x, layout_entry.y), 0, 0);
            TEST(graph.node(mapping.lfo_nodes[i]).position.x == layout_entry.x);
        }
        TEST(Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));
        TEST(graph.node(mapping.effect_nodes[0]).position.x == 64.0f + 32.0f);
        for (uint32_t i = 0; i < 2; ++i) {
            TEST(graph.node(mapping.lfo_nodes[i]).position.x == canvas.graph_layout[i].x);
        }
        TEST(! Sculptor::fx_arrange_auto_lane(&graph, mapping, widths));

        // Chain-scoped: the unwired descriptors project on no other chain.
        memset(static_cast<void*>(pins), 0, sizeof(pins));
        TEST(Sculptor::collect_effect_lfos(&canvas, 1, pins));
        TEST(! pins[0] && ! pins[1]);
        TEST(Sculptor::project_effect_chain_to_graph(canvas, 1, &graph, &mapping, pins));
        graph.take_changes(discarded, Sculptor::max_pending_changes);
        TEST(mapping.lfo_count == 0);
        TEST(Sculptor::validate_editor_metadata(canvas));
    }
}

// The shared freshly-wired effect-LFO binding initialization: the drop,
// drag and canvas pathways delegate to one helper, so the descriptor id
// lands and a zero depth seeds to an audible value while a nonzero
// authored depth stays.
static void fx_lfo_binding_seed_regressions()
{
    Synth::EffectParamBinding seeded = {};
    Sculptor::fx_seed_effect_lfo_binding(&seeded, 7);
    TEST(seeded.lfo_desc_id == 7);
    TEST(seeded.lfo_depth == 0.5f);
    seeded.lfo_depth = 0.25f;
    Sculptor::fx_seed_effect_lfo_binding(&seeded, 9);
    TEST(seeded.lfo_desc_id == 9);
    TEST(seeded.lfo_depth == 0.25f);
}

// Adjacency follows the projected slot list: dormant (type none) slots
// project no node, so they are skipped and never appear as neighbors.
static void fx_adjacent_projected_slots_regressions()
{
    const auto chain_with_hole = [](uint32_t dormant) {
        Synth::EffectChainBinding chain = {};
        chain.num_effects               = 3;
        for (uint32_t slot = 0; slot < 3; ++slot) {
            if (slot != dormant) {
                Sculptor::fx_init_slot(&chain, slot, Synth::EffectType::delay);
            }
        }
        return chain;
    };
    {
        // [A, none, B]: slot 0 has no previous and slot 2 as next; the
        // dormant slot projects no node and has no neighbors.
        const Synth::EffectChainBinding chain    = chain_with_hole(1);
        int32_t                         previous = -1;
        int32_t                         next     = -1;
        Sculptor::fx_adjacent_projected_slots(chain, 0, &previous, &next);
        TEST(previous == -1 && next == 2);
        Sculptor::fx_adjacent_projected_slots(chain, 1, &previous, &next);
        TEST(previous == -1 && next == -1);
        Sculptor::fx_adjacent_projected_slots(chain, 2, &previous, &next);
        TEST(previous == 0 && next == -1);
    }
    {
        // [none, A, B]: slot 1 has no previous and slot 2 as next.
        const Synth::EffectChainBinding chain    = chain_with_hole(0);
        int32_t                         previous = -1;
        int32_t                         next     = -1;
        Sculptor::fx_adjacent_projected_slots(chain, 1, &previous, &next);
        TEST(previous == -1 && next == 2);
        Sculptor::fx_adjacent_projected_slots(chain, 2, &previous, &next);
        TEST(previous == 1 && next == -1);
        Sculptor::fx_adjacent_projected_slots(chain, 0, &previous, &next);
        TEST(previous == -1 && next == -1);
    }
    {
        // [A, B, none]: slot 0 has no previous and slot 1 as next.
        const Synth::EffectChainBinding chain    = chain_with_hole(2);
        int32_t                         previous = -1;
        int32_t                         next     = -1;
        Sculptor::fx_adjacent_projected_slots(chain, 0, &previous, &next);
        TEST(previous == -1 && next == 1);
        Sculptor::fx_adjacent_projected_slots(chain, 1, &previous, &next);
        TEST(previous == 0 && next == -1);
        Sculptor::fx_adjacent_projected_slots(chain, 2, &previous, &next);
        TEST(previous == -1 && next == -1);
    }
    {
        // Dense chain: adjacency is the raw slot adjacency.
        const Synth::EffectChainBinding chain    = chain_with_hole(3);
        int32_t                         previous = -1;
        int32_t                         next     = -1;
        Sculptor::fx_adjacent_projected_slots(chain, 0, &previous, &next);
        TEST(previous == -1 && next == 1);
        Sculptor::fx_adjacent_projected_slots(chain, 1, &previous, &next);
        TEST(previous == 0 && next == 2);
        Sculptor::fx_adjacent_projected_slots(chain, 2, &previous, &next);
        TEST(previous == 1 && next == -1);
    }
}

// A dormant middle slot must not confuse an exchange: the two projected
// slots trade complete bindings, including the LFO binding and the
// enable input.  Routing flags belong to slot positions and never move
// with an exchange, so the wiring stays put.
static void fx_exchange_effect_slots_regressions()
{
    Synth::EffectChainBinding chain = {};
    chain.num_effects               = 3;
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::delay);
    Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::chorus);
    chain.effects[0].enabled                 = true;
    chain.effects[0].bindings[0].base_value  = 1.0f;
    chain.effects[0].bindings[0].lfo_desc_id = 3;
    chain.effects[0].enable_input.source     = Synth::ModSource::pitch_bend;
    chain.effects[2].enabled                 = false;
    chain.effects[2].bindings[0].base_value  = 2.0f;
    chain.effects[2].bindings[0].lfo_desc_id = 5;
    chain.effects[2].enable_input.source     = Synth::ModSource::channel_pressure;

    Sculptor::fx_exchange_effect_slots(&chain, 0, 2);

    TEST(chain.effects[0].type == Synth::EffectType::chorus);
    TEST(chain.effects[0].enabled == false);
    TEST(chain.effects[0].bindings[0].base_value == 2.0f);
    TEST(chain.effects[0].bindings[0].lfo_desc_id == 5);
    TEST(chain.effects[0].enable_input.source == Synth::ModSource::channel_pressure);
    TEST(chain.effects[1].type == Synth::EffectType::none);
    TEST(chain.effects[2].type == Synth::EffectType::delay);
    TEST(chain.effects[2].enabled == true);
    TEST(chain.effects[2].bindings[0].base_value == 1.0f);
    TEST(chain.effects[2].bindings[0].lfo_desc_id == 3);
    TEST(chain.effects[2].enable_input.source == Synth::ModSource::pitch_bend);
}

// Contiguity invariant: occupied slots are contiguous from slot 0, empty
// slots only trail, every connected_to_next pair is literally adjacent,
// and at most one slot holds connected_to_output.  The validator must
// also accept the state.  Runs after every gesture.
static void check_chain_contiguous(const Synth::EffectChainBinding& chain, const Sculptor::EffectChainRouting& routing)
{
    TEST(chain.num_effects <= Synth::max_chain_effects);
    int32_t holders    = 0;
    bool    seen_empty = false;
    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
        const bool occupied = slot < chain.num_effects && chain.effects[slot].type != Synth::EffectType::none;
        if (! occupied) {
            TEST(routing.out[slot] == Sculptor::effect_out_disconnected);
            seen_empty = true;
            continue;
        }
        // An occupied slot behind an empty one breaks the trailing-empties
        // rule: the invariant is a prefix, not a filter.
        TEST(! seen_empty);
        if (routing.out[slot] == Sculptor::effect_out_next) {
            TEST(slot + 1 < chain.num_effects && chain.effects[slot + 1].type != Synth::EffectType::none);
        }
        if (routing.out[slot] == Sculptor::effect_out_output) {
            ++holders;
        }
    }
    TEST(holders <= 1);
    TEST(routing.in != Sculptor::effect_in_first_slot || chain.num_effects > 0);
    TEST(Sculptor::validate_effect_routing(chain, routing));
}

// Whole-routing comparison for one chain: expected_out lists the four slot
// flags in slot order.
static void check_routing(const Synth::InstrumentEditorBank& bank,
                          uint32_t                           owner,
                          uint8_t                            expected_in,
                          const uint8_t (&expected_out)[Synth::max_chain_effects])
{
    const Sculptor::EffectChainRouting& routing = bank.effect_routing[owner];
    TEST(routing.in == expected_in);
    TEST(memcmp(routing.out, expected_out, sizeof(expected_out)) == 0);
}

// Derivation rules: validation, the live path prefix (effect_path_terminal),
// the output holder, per-slot input feeds, and the derived canvas wires of
// the projection (degenerate states included).
static void fx_routing_derivation_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       mapping        = {};

    // Serial complete chain: input->A->B->C->D->output.
    build_routing_fixture(&source, 0, 4);
    {
        Sculptor::EffectChainRouting& routing = source.effect_routing[0];
        routing.in                            = 1;
        routing.out[0]                        = Sculptor::effect_out_next;
        routing.out[1]                        = Sculptor::effect_out_next;
        routing.out[2]                        = Sculptor::effect_out_next;
        routing.out[3]                        = Sculptor::effect_out_output;
    }
    const Synth::EffectChainBinding&    chain   = Sculptor::fx_graph_chain(&source.bank, 0);
    const Sculptor::EffectChainRouting& routing = source.effect_routing[0];
    TEST(Sculptor::validate_effect_routing(chain, routing));
    TEST(Sculptor::effect_path_terminal(chain, routing) == 3);
    TEST(Sculptor::fx_output_holder(chain, routing) == 3);
    for (uint32_t slot = 0; slot < 4; ++slot) {
        TEST(Sculptor::fx_slot_input_fed(chain, routing, slot));
    }
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    TEST(find_fx_graph_connection(graph, { mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) !=
         Sculptor::pool_no_slot);
    for (uint32_t slot = 0; slot < 3; ++slot) {
        TEST(find_fx_graph_connection(graph,
                                      { mapping.effect_nodes[slot], 1 },
                                      { mapping.effect_nodes[slot + 1], 0 }) != Sculptor::pool_no_slot);
    }
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[3], 1 }, { mapping.output_node, 0 }) !=
         Sculptor::pool_no_slot);

    // Degenerate segment: A->B->C chained but touching neither input nor
    // output.  Legal state, no complete path.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    TEST(Sculptor::effect_path_terminal(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == -1);
    TEST(Sculptor::fx_output_holder(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == -1);
    TEST(! Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0], 0));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[1], 1 }, { mapping.effect_nodes[2], 0 }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) ==
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[3], 1 }, { mapping.output_node, 0 }) ==
         Sculptor::pool_no_slot);

    // input->A->B plus C->D->output: two segments, no complete path.
    build_routing_fixture(&source, 0, 4);
    {
        Sculptor::EffectChainRouting& segment = source.effect_routing[0];
        segment.in                            = 1;
        segment.out[0]                        = Sculptor::effect_out_next;
        segment.out[2]                        = Sculptor::effect_out_next;
        segment.out[3]                        = Sculptor::effect_out_output;
    }
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    TEST(Sculptor::effect_path_terminal(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == -1);
    TEST(Sculptor::fx_output_holder(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == 3);
    TEST(Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0], 0));
    TEST(! Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0], 2));
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &mapping));
    TEST(find_fx_graph_connection(graph, { mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[1], 1 }, { mapping.effect_nodes[2], 0 }) ==
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[2], 1 }, { mapping.effect_nodes[3], 0 }) !=
         Sculptor::pool_no_slot);
    TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[3], 1 }, { mapping.output_node, 0 }) !=
         Sculptor::pool_no_slot);

    // Validation refusals: two output holders, a next flag on the last
    // occupied slot, a mid-chain empty slot, and in with no
    // occupied slots.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    source.effect_routing[0].out[3] = Sculptor::effect_out_output;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    build_routing_fixture(&source, 0, 4);
    Sculptor::fx_graph_chain(&source.bank, 0).effects[1].type = Synth::EffectType::none;
    source.effect_routing[0].in                               = 1;
    source.effect_routing[0].out[2]                           = Sculptor::effect_out_next;
    source.effect_routing[0].out[3]                           = Sculptor::effect_out_output;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    build_routing_fixture(&source, 0, 0);
    source.effect_routing[0].in = Sculptor::effect_in_first_slot;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    build_routing_fixture(&source, 0, 0);
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    TEST(Sculptor::effect_path_terminal(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == -1);
}

// Gesture semantics: connect moves runs and derives wires from
// adjacency, disconnect splits flags only, output connect demotes the old
// holder, rewires clear the removed wire's earlier endpoint.  Every case
// asserts the contiguity invariant and validation afterwards.
static void fx_routing_gesture_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    // Connect A.Out -> C while A already feeds B: the new wire overwrites
    // A's old one (the removed wire's earlier endpoint is A itself), C's run
    // moves directly after A, and B keeps its parked flag.  Order [A, C, B].
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_connect_slots_candidate(&source, 0, 0, 2, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 3);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 1.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[1].bindings[0].base_value == 3.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[2].bindings[0].base_value == 2.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Connect input -> C while C is parked and unfed: C's run moves to the
    // front, in = Sculptor::effect_in_first_slot, the A->B wire survives.  Order [C, A, B].
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    TEST(Sculptor::fx_connect_input_candidate(&source, 0, 2, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 3.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[1].bindings[0].base_value == 1.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[2].bindings[0].base_value == 2.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Connect input -> C while the input already feeds A: the old input wire
    // is cleared by the move, and the input feeds the new slot 0.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    TEST(Sculptor::fx_connect_input_candidate(&source, 0, 2, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Connect effect -> output: the target takes connected_to_output; a new
    // holder demotes the old holder to disconnected.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    TEST(Sculptor::fx_connect_output_candidate(&source, 0, 2, &candidate));
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::fx_output_holder(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]) == 2);
    TEST(Sculptor::fx_connect_output_candidate(&candidate, 0, 0, &candidate));
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::fx_output_holder(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]) == 0);
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Disconnect: flags clear, no slots move, no healing.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_disconnect_candidate(&source, 0, Sculptor::fx_connector_effect_output, 1, &candidate));
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[1].bindings[0].base_value == 2.0f);
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    TEST(Sculptor::fx_disconnect_candidate(&candidate, 0, Sculptor::fx_connector_global_input, -1, &candidate));
    TEST(candidate.effect_routing[0].in == Sculptor::effect_in_disconnected);
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Occupied inputs refuse: B is already fed by A, so A.Out -> B.In
    // refuses untouched (the canvas refuses occupied inputs too).
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in                                       = 1;
    source.effect_routing[0].out[0]                                   = Sculptor::effect_out_next;
    source.effect_routing[0].out[1]                                   = Sculptor::effect_out_next;
    source.effect_routing[0].out[2]                                   = Sculptor::effect_out_output;
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       before         = *before_storage;
    candidate                                                         = source;
    memcpy(&before, &candidate, sizeof(before));
    TEST(! Sculptor::fx_connect_slots_candidate(&source, 0, 0, 1, &candidate));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);
    TEST(! Sculptor::fx_connect_slots_candidate(&source, 0, 1, 1, &candidate));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);
}

// Cycle refusal: a connect whose source slot lies
// inside the target's run refuses with the chain untouched.  Backward
// connects whose source is outside the target's run still succeed.
static void fx_routing_cycle_refusal_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       candidate      = *candidate_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       before         = *before_storage;

    // Rotation case: [A, B, C, D], out [next, next, disc, output].  B's run
    // is [B, C], so C.Out -> B.In would rotate the run and refuses.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    source.effect_routing[0].out[3] = Sculptor::effect_out_output;
    candidate                       = source;
    memcpy(&before, &candidate, sizeof(before));
    TEST(! Sculptor::fx_connect_slots_candidate(&source, 0, 2, 1, &candidate));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);

    // Backward connect with the source outside the target's run: D.Out ->
    // B.In succeeds.  B is parked and unfed, its run is [B] alone, so B
    // moves directly after D and D takes connected_to_next; D's old
    // output wire is removed by the rewire, so nothing feeds the output.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].out[2] = Sculptor::effect_out_next;
    source.effect_routing[0].out[3] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_connect_slots_candidate(&source, 0, 3, 1, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 1.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[1].bindings[0].base_value == 3.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[2].bindings[0].base_value == 4.0f);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[3].bindings[0].base_value == 2.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 0, expected);
    }
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Self connects refuse untouched.
    build_routing_fixture(&source, 0, 3);
    candidate = source;
    memcpy(&before, &candidate, sizeof(before));
    TEST(! Sculptor::fx_connect_slots_candidate(&source, 0, 1, 1, &candidate));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);
}

// Delete heal: a deleted effect that was connected on both sides
// is healed by its predecessor or the global input; an unfed segment start
// gets no heal and its downstream endpoint goes unconnected.  Every delete
// compacts the chain.
static void fx_routing_delete_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    // input->A->B->C->output, delete B: A feeds the compacted C.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 1, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 2);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[1].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // input->A->B->output, delete the output holder B: A takes over the
    // output connection.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 1, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // input->A->B->output, delete the first slot A: the global input takes
    // over B's feed and in stays Sculptor::effect_in_first_slot.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 0, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 2.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Parked unfed segment C->D->output, delete the unfed segment start C:
    // no heal, D keeps its flag and its input dot goes unconnected.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 0, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).effects[0].bindings[0].base_value == 2.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 0, expected);
    }
    TEST(! Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0], 0));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Deleting the last occupied slot just shrinks the chain.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 1, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Deleting slot 0 invents no wire (B3): A is fed but its output is
    // disconnected, so the surviving B was unfed and must stay unfed.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_disconnected;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 0, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 0, expected);
    }
    TEST(! Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0], 0));
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Same with the deleted slot 0 feeding the output: the input cannot
    // hold connected_to_output, so the successor stays parked.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_output;
    TEST(Sculptor::fx_delete_effect_candidate(&source, 0, 0, &candidate));
    TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 0, expected);
    }
    check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

    // Canvas seam (B2): deleting a fed slot whose own output is
    // disconnected must sanitize the dangling next the flag shift leaves,
    // or the batch's validation would refuse the whole delete.  The seam
    // runs the same delete transform the candidate path runs.
    {
        const std::unique_ptr<Sculptor::Graph> del_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       del_graph         = *del_graph_storage;
        Sculptor::EffectGraphMapping           del_mapping       = {};
        build_routing_fixture(&source, 0, 3);
        source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        source.effect_routing[0].out[0] = Sculptor::effect_out_next;
        source.effect_routing[0].out[1] = Sculptor::effect_out_next;
        source.effect_routing[0].out[2] = Sculptor::effect_out_disconnected;
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &del_graph, &del_mapping));
        Sculptor::GraphChange del_changes[Sculptor::max_pending_changes];
        (void)del_graph.take_changes(del_changes, Sculptor::max_pending_changes); // re-arm the ring
        del_graph.delete_node(del_mapping.effect_nodes[2]);
        const uint32_t del_count = del_graph.take_changes(del_changes, Sculptor::max_pending_changes);
        TEST(Sculptor::apply_effect_graph_batch(&source,
                                                &del_graph,
                                                &del_mapping,
                                                del_changes,
                                                del_count,
                                                nullptr,
                                                &candidate));
        TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 2);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(candidate, 0, 1, expected);
        }
        check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);

        // Same seam, deleting slot 0 with a disconnected downstream: the
        // successor keeps its parked state and the input wire clears.
        build_routing_fixture(&source, 0, 2);
        source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        source.effect_routing[0].out[0] = Sculptor::effect_out_disconnected;
        source.effect_routing[0].out[1] = Sculptor::effect_out_output;
        TEST(Sculptor::project_effect_chain_to_graph(source, 0, &del_graph, &del_mapping));
        (void)del_graph.take_changes(del_changes, Sculptor::max_pending_changes);
        del_graph.delete_node(del_mapping.effect_nodes[0]);
        const uint32_t del_count2 = del_graph.take_changes(del_changes, Sculptor::max_pending_changes);
        TEST(Sculptor::apply_effect_graph_batch(&source,
                                                &del_graph,
                                                &del_mapping,
                                                del_changes,
                                                del_count2,
                                                nullptr,
                                                &candidate));
        TEST(Sculptor::fx_graph_chain(&candidate.bank, 0).num_effects == 1);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(candidate, 0, 0, expected);
        }
        TEST(! Sculptor::fx_slot_input_fed(Sculptor::fx_graph_chain(&candidate.bank, 0),
                                           candidate.effect_routing[0],
                                           0));
        check_chain_contiguous(Sculptor::fx_graph_chain(&candidate.bank, 0), candidate.effect_routing[0]);
    }
}

// Contiguity: after every gesture the occupied slots stay contiguous
// from slot 0 and the validator accepts the state.
static void fx_routing_contiguity_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    build_routing_fixture(&source, 0, 0);
    Synth::InstrumentEditorBank* live = &source;
    // add: the fresh chain's first effect takes the output and the input.
    TEST(Sculptor::fx_add_effect_candidate(live, 0, Synth::EffectType::delay, &candidate));
    live = &candidate;
    TEST(live->effect_routing[0].in == Sculptor::effect_in_first_slot);
    TEST(live->effect_routing[0].out[0] == Sculptor::effect_out_output);
    check_chain_contiguous(Sculptor::fx_graph_chain(&live->bank, 0), live->effect_routing[0]);
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&live->bank, 0), live->effect_routing[0]));
    // add: appends after the last occupied slot and takes the output.
    TEST(Sculptor::fx_add_effect_candidate(live, 0, Synth::EffectType::chorus, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 2);
    TEST(candidate.effect_routing[0].out[0] == Sculptor::effect_out_next);
    TEST(candidate.effect_routing[0].out[1] == Sculptor::effect_out_output);
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    // disconnect, connect, delete, add again: the invariant holds each time.
    TEST(Sculptor::fx_disconnect_candidate(&candidate, 0, Sculptor::fx_connector_effect_output, 1, &candidate));
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    TEST(Sculptor::fx_connect_output_candidate(&candidate, 0, 1, &candidate));
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    TEST(Sculptor::fx_delete_effect_candidate(&candidate, 0, 0, &candidate));
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    TEST(candidate.bank.channel_chains[0].num_effects == 1);
    TEST(candidate.effect_routing[0].out[0] == Sculptor::effect_out_output);
    TEST(Sculptor::fx_add_effect_candidate(&candidate, 0, Synth::EffectType::distortion, &candidate));
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    TEST(candidate.bank.channel_chains[0].num_effects == 2);
    TEST(candidate.effect_routing[0].out[0] == Sculptor::effect_out_next);
    TEST(candidate.effect_routing[0].out[1] == Sculptor::effect_out_output);
    TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[0], candidate.effect_routing[0]));
    // swap in place: the payloads trade while the routing flags stay with
    // the slot positions, so the wire structure is identical and the chain
    // stays connected with the audible order swapped; contiguity is
    // trivially preserved.
    Sculptor::fx_exchange_effect_slots(&candidate.bank.channel_chains[0], 0, 1);
    Sculptor::fx_sanitize_routing(&candidate.bank.channel_chains[0], &candidate.effect_routing[0]);
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    TEST(candidate.bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::chorus);
    TEST(candidate.effect_routing[0].in == Sculptor::effect_in_first_slot);
    TEST(candidate.effect_routing[0].out[0] == Sculptor::effect_out_next);
    TEST(candidate.effect_routing[0].out[1] == Sculptor::effect_out_output);
    TEST(Sculptor::fx_slot_input_fed(candidate.bank.channel_chains[0], candidate.effect_routing[0], 0));
    TEST(Sculptor::fx_slot_input_fed(candidate.bank.channel_chains[0], candidate.effect_routing[0], 1));
    TEST(Sculptor::fx_output_holder(candidate.bank.channel_chains[0], candidate.effect_routing[0]) == 1);
}

// Swap: the exchange trades payloads and layout record bodies, so each
// box physically trades places, while the routing flags stay with the
// slot positions: the wire structure is identical after the swap, so
// the chain stays connected with the audible order swapped.
static void fx_swap_layout_exchange_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_disconnected;
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    // Distinct layout records for the two derived titles.
    Synth::GraphNodeLayout& delay_record = source.graph_layout[source.graph_layout_count++];
    delay_record                         = {};
    delay_record.kind                    = 4;
    delay_record.channel                 = 0;
    snprintf(delay_record.name, sizeof(delay_record.name), "Delay");
    delay_record.x                        = 64.0f;
    delay_record.y                        = 160.0f;
    delay_record.width_override           = 250.0f;
    delay_record.height_override          = 180.0f;
    Synth::GraphNodeLayout& chorus_record = source.graph_layout[source.graph_layout_count++];
    chorus_record                         = {};
    chorus_record.kind                    = 4;
    chorus_record.channel                 = 0;
    snprintf(chorus_record.name, sizeof(chorus_record.name), "Chorus");
    chorus_record.x               = 320.0f;
    chorus_record.y               = 96.0f;
    chorus_record.width_override  = 220.0f;
    chorus_record.height_override = 150.0f;

    candidate = source;
    Sculptor::fx_exchange_effect_slots(&candidate.bank.channel_chains[0], 0, 1);
    Sculptor::fx_swap_effect_layout_records(&candidate, 0, 0, 1);
    Sculptor::fx_sanitize_routing(&candidate.bank.channel_chains[0], &candidate.effect_routing[0]);

    // Payloads traded places while the routing flags stayed with the slot
    // positions: the wire structure is identical in slot space, so the
    // chain still plays through every effect in the new order.
    TEST(candidate.bank.channel_chains[0].effects[0].type == Synth::EffectType::chorus);
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::delay);
    TEST(candidate.bank.channel_chains[0].effects[2].type == Synth::EffectType::distortion);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 2.0f);
    TEST(candidate.bank.channel_chains[0].effects[1].bindings[0].base_value == 1.0f);
    TEST(candidate.bank.channel_chains[0].effects[2].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    // The wire structure is unchanged: slots 0 and 1 stay fed exactly as
    // before the swap (slot 2's input was already unfed), and the output
    // holder did not move.
    TEST(Sculptor::fx_slot_input_fed(candidate.bank.channel_chains[0], candidate.effect_routing[0], 0));
    TEST(Sculptor::fx_slot_input_fed(candidate.bank.channel_chains[0], candidate.effect_routing[0], 1));
    TEST(! Sculptor::fx_slot_input_fed(candidate.bank.channel_chains[0], candidate.effect_routing[0], 2));
    TEST(Sculptor::fx_output_holder(candidate.bank.channel_chains[0], candidate.effect_routing[0]) == 2);
    // The boxes physically traded places: the Delay record body now carries
    // the Chorus position and vice versa.
    const Synth::GraphNodeLayout* delay_after  = nullptr;
    const Synth::GraphNodeLayout* chorus_after = nullptr;
    for (uint32_t i = 0; i < candidate.graph_layout_count; ++i) {
        if (strcmp(candidate.graph_layout[i].name, "Delay") == 0) {
            delay_after = &candidate.graph_layout[i];
        }
        if (strcmp(candidate.graph_layout[i].name, "Chorus") == 0) {
            chorus_after = &candidate.graph_layout[i];
        }
    }
    TEST(delay_after && chorus_after);
    if (delay_after && chorus_after) {
        TEST(delay_after->x == 320.0f && delay_after->y == 96.0f);
        TEST(chorus_after->x == 64.0f && chorus_after->y == 160.0f);
        TEST(delay_after->width_override == 220.0f && delay_after->height_override == 150.0f);
        TEST(chorus_after->width_override == 250.0f && chorus_after->height_override == 180.0f);
    }
    TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[0], candidate.effect_routing[0]));
    TEST(Synth::validate_instrument_bank(&candidate.bank));
    TEST(Sculptor::validate_editor_metadata(candidate));

    // Last-slot sanitizer: a connected_to_next flag on the last occupied
    // slot sanitizes to disconnected.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    Sculptor::fx_sanitize_routing(&source.bank.channel_chains[0], &source.effect_routing[0]);
    TEST(source.effect_routing[0].out[1] == Sculptor::effect_out_disconnected);
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));

    // Two channels with matching slots and layouts: a swap on one chain
    // must leave the other chain's records untouched.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    {
        Synth::EffectChainBinding& chain1 = Sculptor::fx_graph_chain(&source.bank, 1);
        chain1.num_effects                = 2;
        for (uint32_t slot = 0; slot < 2; ++slot) {
            const Synth::EffectType types[2] = { Synth::EffectType::delay, Synth::EffectType::chorus };
            Sculptor::fx_init_slot(&chain1, slot, types[slot]);
            chain1.effects[slot].enabled                = true;
            chain1.effects[slot].bindings[0].base_value = 10.0f + static_cast<float>(slot);
        }
        source.effect_routing[1].in     = Sculptor::effect_in_first_slot;
        source.effect_routing[1].out[0] = Sculptor::effect_out_next;
        source.effect_routing[1].out[1] = Sculptor::effect_out_output;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const char* const names[2] = { "Delay", "Chorus" };
            for (uint32_t slot = 0; slot < 2; ++slot) {
                Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
                record                         = {};
                record.kind                    = 4;
                record.channel                 = static_cast<uint8_t>(channel);
                snprintf(record.name, sizeof(record.name), "%s", names[slot]);
                record.x = 500.0f + 100.0f * static_cast<float>(channel) + 10.0f * static_cast<float>(slot);
                record.y = 100.0f * static_cast<float>(channel);
            }
        }
        TEST(Synth::validate_instrument_bank(&source.bank));
        TEST(Sculptor::validate_editor_metadata(source));
        candidate = source;
        Sculptor::fx_exchange_effect_slots(&candidate.bank.channel_chains[1], 0, 1);
        Sculptor::fx_swap_effect_layout_records(&candidate, 1, 0, 1);
        Sculptor::fx_sanitize_routing(&candidate.bank.channel_chains[1], &candidate.effect_routing[1]);
        TEST(candidate.bank.channel_chains[1].effects[0].type == Synth::EffectType::chorus);
        TEST(candidate.bank.channel_chains[1].effects[1].type == Synth::EffectType::delay);
        TEST(candidate.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::chorus);
        // Channel 1's two records traded bodies; channel 0's kept theirs.
        for (uint32_t index = 0; index < candidate.graph_layout_count; ++index) {
            const Synth::GraphNodeLayout& record = candidate.graph_layout[index];
            if (record.channel != 0 || strcmp(record.name, "Delay") != 0) {
                continue;
            }
            TEST(record.x == 500.0f);
            TEST(record.y == 0.0f);
        }
        for (uint32_t index = 0; index < candidate.graph_layout_count; ++index) {
            const Synth::GraphNodeLayout& record = candidate.graph_layout[index];
            if (record.channel != 0 || strcmp(record.name, "Chorus") != 0) {
                continue;
            }
            TEST(record.x == 510.0f);
            TEST(record.y == 0.0f);
        }
        bool delay1_moved = false;
        for (uint32_t index = 0; index < candidate.graph_layout_count; ++index) {
            const Synth::GraphNodeLayout& record = candidate.graph_layout[index];
            if (record.channel == 1 && strcmp(record.name, "Delay") == 0) {
                delay1_moved = record.x == 610.0f;
            }
        }
        TEST(delay1_moved);
        TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[1], candidate.effect_routing[1]));
        TEST(Synth::validate_instrument_bank(&candidate.bank));
        TEST(Sculptor::validate_editor_metadata(candidate));
    }
}

// Effective enablement: compile copies slots 0..terminal in
// index order, skipping a slot only when its user flag is clear and its
// Enabled row is unwired.  Off-path slots never compile, a wired saved-off
// slot inside the path still compiles, and an incomplete path compiles dry.
static void fx_effective_enablement_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentBank>       playback_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                             playback         = *playback_storage;

    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_next;
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 3);
    TEST(playback.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
    TEST(playback.channel_chains[0].effects[1].bindings[0].base_value == 2.0f);
    TEST(playback.channel_chains[0].effects[2].bindings[0].base_value == 3.0f);
    TEST(Synth::validate_instrument_bank(&playback));

    // A disabled on-path slot is bypassed, not a path breaker.
    source.bank.channel_chains[0].effects[1].enabled = false;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 2);
    TEST(playback.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
    TEST(playback.channel_chains[0].effects[1].bindings[0].base_value == 3.0f);
    source.bank.channel_chains[0].effects[1].enabled = true;

    // A wired saved-off slot inside the path still compiles: the
    // runtime owns the flag through enable_input.
    source.bank.channel_chains[0].effects[1].enabled                = false;
    source.bank.channel_chains[0].effects[1].enable_input.source    = Synth::ModSource::cc;
    source.bank.channel_chains[0].effects[1].enable_input.op        = Synth::SourceOp::add;
    source.bank.channel_chains[0].effects[1].enable_input.cc_number = Synth::cc_msb_base;
    source.bank.channel_chains[0].effects[1].enable_input.scale     = 1.0f;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 3);
    source.bank.channel_chains[0].effects[1].enable_input = Synth::ModInput{};
    source.bank.channel_chains[0].effects[1].enabled      = true;

    // Off-path slots never compile, wired or not: the path is input->A->B;
    // parked C carries a wired saved-off enable that stays inert.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].in                                     = 1;
    source.effect_routing[0].out[0]                                 = Sculptor::effect_out_next;
    source.effect_routing[0].out[1]                                 = Sculptor::effect_out_output;
    source.bank.channel_chains[0].effects[2].enabled                = false;
    source.bank.channel_chains[0].effects[2].enable_input.source    = Synth::ModSource::cc;
    source.bank.channel_chains[0].effects[2].enable_input.op        = Synth::SourceOp::add;
    source.bank.channel_chains[0].effects[2].enable_input.cc_number = Synth::cc_msb_base;
    source.bank.channel_chains[0].effects[2].enable_input.scale     = 1.0f;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 2);
    TEST(playback.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
    TEST(playback.channel_chains[0].effects[1].bindings[0].base_value == 2.0f);

    // Incomplete path or no path: the chain compiles dry.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in = Sculptor::effect_in_first_slot;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 0);
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects ==
         0); // no input wire: the A->B->output run stays parked and compiles dry
    build_routing_fixture(&source, 0, 0);
    TEST(Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(playback.channel_chains[0].num_effects == 0);

    // Invalid routing refuses the compile untouched.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    source.effect_routing[0].out[3] = Sculptor::effect_out_output;
    memset(static_cast<void*>(&playback), 0xA5, sizeof(playback));
    const std::unique_ptr<Synth::InstrumentBank> before_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       before         = *before_storage;
    memcpy(&before, &playback, sizeof(before));
    TEST(! Sculptor::compile_editor_playback_bank(source, &playback));
    TEST(memcmp(&playback, &before, sizeof(playback)) == 0);
}

// Red connector dots: every endpoint kind derives from the
// routing plus occupancy alone.
// The direct input -> output bypass wire: setting in to effect_in_output
// compiles the chain dry while the stored effect wiring survives, clearing
// it restores the wired compile, an empty chain may carry the bypass, and
// out-of-range input values refuse.
static void fx_input_bypass_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentBank>       compiled_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                             compiled         = *compiled_storage;

    // Two-effect wired chain: input feeds slot 0, A feeds B, B feeds the
    // output node.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    TEST(Sculptor::effect_path_terminal(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == 1);
    TEST(Sculptor::compile_editor_playback_bank(source, &compiled));
    TEST(compiled.channel_chains[0].num_effects == 2);

    // Disconnect the run from node first: the bypass then owns the
    // output feed. The compiled chain is empty (dry pass-through) and
    // the stored run survives underneath.
    source.effect_routing[0].out[1] = Sculptor::effect_out_disconnected;
    source.effect_routing[0].in     = Sculptor::effect_in_output;
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    TEST(Sculptor::effect_path_terminal(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]) == -1);
    TEST(Sculptor::compile_editor_playback_bank(source, &compiled));
    TEST(compiled.channel_chains[0].num_effects == 0);
    TEST(source.effect_routing[0].out[0] == Sculptor::effect_out_next);
    TEST(source.effect_routing[0].out[1] == Sculptor::effect_out_disconnected);

    // The bypass and a slot holder cannot coexist: validation refuses.
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&source.bank, 0), source.effect_routing[0]));
    source.effect_routing[0].out[1] = Sculptor::effect_out_disconnected;
    // An empty chain may carry the bypass too.
    const std::unique_ptr<Synth::InstrumentEditorBank> empty_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       empty         = *empty_storage;
    empty.effect_routing[0].in                                       = Sculptor::effect_in_output;
    TEST(Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&empty.bank, 0), empty.effect_routing[0]));
    TEST(Sculptor::compile_editor_playback_bank(empty, &compiled));
    TEST(compiled.channel_chains[0].num_effects == 0);

    // Out-of-range input values refuse.
    empty.effect_routing[0].in = 3;
    TEST(! Sculptor::validate_effect_routing(Sculptor::fx_graph_chain(&empty.bank, 0), empty.effect_routing[0]));

    // Adding an effect while the bypass is set: the new effect stays
    // unwired and the bypass keeps the output feed.
    const std::unique_ptr<Synth::InstrumentEditorBank> added_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       added         = *added_storage;
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::chorus, &added));
    TEST(added.effect_routing[0].in == Sculptor::effect_in_output);
    TEST(added.effect_routing[0].out[2] == Sculptor::effect_out_disconnected);

    // Connecting an effect to node while the bypass is set: the bypass
    // yields and the effect takes the feed.
    const std::unique_ptr<Synth::InstrumentEditorBank> took_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       took         = *took_storage;
    TEST(Sculptor::fx_connect_output_candidate(&source, 0, 1, &took));
    TEST(took.effect_routing[0].in == Sculptor::effect_in_disconnected);
    TEST(took.effect_routing[0].out[1] == Sculptor::effect_out_output);
    TEST(took.effect_routing[0].out[0] == Sculptor::effect_out_next);
    // Reconnecting the run to node and severing the bypass restores
    // the wired compile.
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    TEST(Sculptor::compile_editor_playback_bank(source, &compiled));
    TEST(compiled.channel_chains[0].num_effects == 2);
}

static void fx_connector_red_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const Synth::EffectChainBinding*                   chain          = nullptr;
    const Sculptor::EffectChainRouting*                routing        = nullptr;

    // Fully wired serial chain: nothing is red.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    chain                           = &Sculptor::fx_graph_chain(&source.bank, 0);
    routing                         = &source.effect_routing[0];
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_input, -1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_output, -1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 0));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 0));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 1));

    // Unconnected global input: the global input dot and slot 0's input dot
    // go red.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    chain                           = &Sculptor::fx_graph_chain(&source.bank, 0);
    routing                         = &source.effect_routing[0];
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_input, -1));
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 0));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 1));

    // Disconnected effect output: that output dot goes red and the global
    // output dot goes red with it (no holder).
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    chain                           = &Sculptor::fx_graph_chain(&source.bank, 0);
    routing                         = &source.effect_routing[0];
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 1));
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_output, -1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 0));

    // Parked segment behind a split: input->A->B, parked C->D->output.  C's
    // input dot goes red and B's output dot goes red; the global dots stay
    // fed.
    build_routing_fixture(&source, 0, 4);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[2] = Sculptor::effect_out_next;
    source.effect_routing[0].out[3] = Sculptor::effect_out_output;
    chain                           = &Sculptor::fx_graph_chain(&source.bank, 0);
    routing                         = &source.effect_routing[0];
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_input, -1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_global_output, -1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 1));
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 2));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_input, 3));
    TEST(Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 1));
    TEST(! Sculptor::fx_connector_red(*chain, *routing, Sculptor::fx_connector_effect_output, 3));
}

// Migration: old-format documents load with slots reordered to
// the saved wiring, flags derived from it, and no topology struct.  The
// documents below are literal saved documents: they use the old key names
// and shapes exactly as the previous encoder wrote them.
static void fx_routing_migration_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                 decoded          = *decoded_storage;
    const std::unique_ptr<Synth::InstrumentBank> playback_storage = std::make_unique<Synth::InstrumentBank>();
    Synth::InstrumentBank&                       playback         = *playback_storage;

    // v1 plain root, no effect_audio and no effect_routing: the implicit
    // serial chain loads as input->A->B->output.
    {
        const char* const v1_doc = "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":"
                                   "[{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
                                   "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
                                   "\"master_chain\":{}}}";
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(v1_doc, static_cast<uint32_t>(strlen(v1_doc)), &decoded));
        TEST(decoded.bank.channel_chains[0].num_effects == 2);
        TEST(decoded.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(decoded.bank.channel_chains[0].effects[1].type == Synth::EffectType::chorus);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(decoded, 0, 1, expected);
        }
        TEST(Sculptor::validate_effect_routing(decoded.bank.channel_chains[0], decoded.effect_routing[0]));
    }

    // v2 Saxophone fixture: slots [Reverb, Distortion] wired
    // input->Distortion->Reverb->output load as [Distortion, Reverb] with
    // out flags [next, output] and in = Sculptor::effect_in_first_slot.  Sound unchanged.
    {
        const char* const saxophone_doc = "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                                          "\"channels\":[{\"chain\":{\"effects\":["
                                          "{\"type\":\"reverb\",\"enabled\":true,\"params\":[{},{},{}]},"
                                          "{\"type\":\"distortion\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
                                          "\"master_chain\":{}},"
                                          "\"editor\":{\"effect_audio\":[[5,1,0,0,2],null,null,null,null,null,"
                                          "null,null,null,null,null,null,null,null,null,null,null]}}";
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(saxophone_doc, static_cast<uint32_t>(strlen(saxophone_doc)), &decoded));
        TEST(decoded.bank.channel_chains[0].num_effects == 2);
        TEST(decoded.bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(decoded.bank.channel_chains[0].effects[1].type == Synth::EffectType::reverb);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(decoded, 0, 1, expected);
        }
        TEST(Sculptor::validate_effect_routing(decoded.bank.channel_chains[0], decoded.effect_routing[0]));
        TEST(Sculptor::compile_editor_playback_bank(decoded, &playback));
        TEST(playback.channel_chains[0].num_effects == 2);
        TEST(playback.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(playback.channel_chains[0].effects[1].type == Synth::EffectType::reverb);
    }

    // v2 multi-segment document with mid-chain empties: input->A->B->output
    // over slots [A, none, B, C] with C isolated compacts to
    // [A, B, C] = [next, output, disc].
    {
        const char* const gap_doc = "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                                    "\"channels\":[{\"chain\":{\"effects\":["
                                    "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
                                    "{\"type\":\"none\"},"
                                    "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]},"
                                    "{\"type\":\"distortion\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
                                    "\"master_chain\":{}},"
                                    "\"editor\":{\"effect_audio\":[[3,0,5,0,1],null,null,null,null,null,"
                                    "null,null,null,null,null,null,null,null,null,null,null]}}";
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(gap_doc, static_cast<uint32_t>(strlen(gap_doc)), &decoded));
        TEST(decoded.bank.channel_chains[0].num_effects == 3);
        TEST(decoded.bank.channel_chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(decoded.bank.channel_chains[0].effects[1].type == Synth::EffectType::chorus);
        TEST(decoded.bank.channel_chains[0].effects[2].type == Synth::EffectType::distortion);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(decoded, 0, 1, expected);
        }
        TEST(Sculptor::validate_effect_routing(decoded.bank.channel_chains[0], decoded.effect_routing[0]));
    }

    // v2 isolated zero-edge slot plus multiple segments: slots [A, B, C, D]
    // with B->output, C->D, and no input wire load as [B, C, D, A], all
    // parked segments, in = Sculptor::effect_in_disconnected.
    {
        const char* const segments_doc = "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                                         "\"channels\":[{\"chain\":{\"effects\":["
                                         "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
                                         "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]},"
                                         "{\"type\":\"distortion\",\"enabled\":true,\"params\":[{},{},{}]},"
                                         "{\"type\":\"reverb\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
                                         "\"master_chain\":{}},"
                                         "\"editor\":{\"effect_audio\":[[0,5,4,0,0],null,null,null,null,null,"
                                         "null,null,null,null,null,null,null,null,null,null,null]}}";
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(segments_doc, static_cast<uint32_t>(strlen(segments_doc)), &decoded));
        TEST(decoded.bank.channel_chains[0].num_effects == 4);
        TEST(decoded.bank.channel_chains[0].effects[0].type == Synth::EffectType::chorus);
        TEST(decoded.bank.channel_chains[0].effects[1].type == Synth::EffectType::distortion);
        TEST(decoded.bank.channel_chains[0].effects[2].type == Synth::EffectType::reverb);
        TEST(decoded.bank.channel_chains[0].effects[3].type == Synth::EffectType::delay);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(decoded, 0, 0, expected);
        }
        TEST(Sculptor::validate_effect_routing(decoded.bank.channel_chains[0], decoded.effect_routing[0]));
        // No complete path: the loaded chain compiles dry.
        TEST(Sculptor::compile_editor_playback_bank(decoded, &playback));
        TEST(playback.channel_chains[0].num_effects == 0);
    }

    // v2 direct input->output edge with no effects: in = Sculptor::effect_in_disconnected and
    // the chain stays empty.  Sound identical: both compile dry.
    {
        const char* const bypass_doc = "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
                                       "\"channels\":[{\"chain\":{\"effects\":[]}}],\"master_chain\":{}},"
                                       "\"editor\":{\"effect_audio\":[[0,0,0,0,5],null,null,null,null,null,"
                                       "null,null,null,null,null,null,null,null,null,null,null]}}";
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(bypass_doc, static_cast<uint32_t>(strlen(bypass_doc)), &decoded));
        TEST(decoded.bank.channel_chains[0].num_effects == 0);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            check_routing(decoded, 0, 0, expected);
        }
        TEST(Sculptor::compile_editor_playback_bank(decoded, &playback));
        TEST(playback.channel_chains[0].num_effects == 0);
    }

    // v2 clipboard document with layout records: the decode remaps record
    // indices along with payloads, so pasted boxes keep their saved
    // positions.  Same Saxophone wiring: [Reverb x=672, Distortion x=272]
    // load as [Distortion x=272, Reverb x=672].
    {
        const char* const      clipboard_doc = "{\"format\":\"synth-effects-v2\",\"audio_next\":[5,1,0,0,2],"
                                               "\"lfos\":[],\"effects\":["
                                               "{\"type\":\"reverb\",\"enabled\":true,\"params\":[{},{},{}]},"
                                               "{\"type\":\"distortion\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                               "\"graph_layout\":[{\"kind\":\"effect\",\"slot\":0,\"x\":672,\"y\":160},"
                                               "{\"kind\":\"effect\",\"slot\":1,\"x\":272,\"y\":160}]}";
        Synth::EffectsDocument document      = {};
        TEST(Synth::decode_effects_json(clipboard_doc, static_cast<uint32_t>(strlen(clipboard_doc)), &document));
        TEST(document.chain.num_effects == 2);
        TEST(document.chain.effects[0].type == Synth::EffectType::distortion);
        TEST(document.chain.effects[1].type == Synth::EffectType::reverb);
        {
            const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                                 Sculptor::effect_out_output,
                                                                 Sculptor::effect_out_disconnected,
                                                                 Sculptor::effect_out_disconnected };
            TEST(document.routing.in == Sculptor::effect_in_first_slot);
            TEST(memcmp(document.routing.out, expected, sizeof(expected)) == 0);
        }
        TEST(document.graph_layout_count == 2);
        for (uint32_t i = 0; i < document.graph_layout_count; ++i) {
            const Synth::EffectsGraphLayout& record = document.graph_layout[i];
            TEST(record.kind == Synth::effects_graph_layout_effect);
            if (record.index == 0) {
                TEST(record.x == 272.0f);
            }
            else if (record.index == 1) {
                TEST(record.x == 672.0f);
            }
            else {
                TEST(false);
            }
        }
    }

    // v3 round trip: the new bank format carries effect_routing and decodes
    // byte-equal; the effects clipboard writes synth-effects-v3 with routing.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        build_routing_fixture(&source, 0, 3);
        source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
        source.effect_routing[0].out[0] = Sculptor::effect_out_next;
        source.effect_routing[0].out[1] = Sculptor::effect_out_next;
        source.effect_routing[0].out[2] = Sculptor::effect_out_output;
        const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
            std::make_unique<FixedArray<char, Synth::library_payload_max>>();
        char (&payload)[Synth::library_payload_max] = payload_storage->elements;
        const uint32_t length                       = Synth::encode_editor_bank_json(&source, payload, sizeof(payload));
        TEST(length > 0);
        TEST(strstr(payload, "\"effect_routing\"") != nullptr);
        memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
        TEST(Synth::decode_editor_bank_json(payload, length, &decoded));
        TEST(memcmp(&decoded.effect_routing[0], &source.effect_routing[0], sizeof(source.effect_routing[0])) == 0);

        Synth::EffectsDocument document = {};
        TEST(Sculptor::extract_effect_chain_document(&source, 0, nullptr, nullptr, &document));
        char           text[65536];
        const uint32_t text_length = Synth::encode_effects_json(&document, text, sizeof(text));
        TEST(text_length != 0);
        TEST(strstr(text, "synth-effects-v3") != nullptr);
        Synth::EffectsDocument redecoded = {};
        TEST(Synth::decode_effects_json(text, text_length, &redecoded));
        TEST(memcmp(&redecoded.routing, &document.routing, sizeof(document.routing)) == 0);
    }

    // Malformed legacy wiring refuses the whole decode with the output
    // untouched (B5): a cycle, a duplicate incoming edge, and an edge into
    // an empty slot all violate what the old successor model allowed.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> sentinel_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& sentinel = *sentinel_storage;
        memset(static_cast<void*>(&decoded), 0x5A, sizeof(decoded));
        sentinel = decoded;
        const char* const cycle_doc =
            "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
            "\"channels\":[{\"chain\":{\"effects\":["
            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
            "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
            "\"master_chain\":{}},"
            "\"editor\":{\"effect_audio\":[[2,1,0,0,0],null,null,null,null,null,null,null,null,"
            "null,null,null,null,null,null,null,null]}}";
        TEST(! Synth::decode_editor_bank_json(cycle_doc, static_cast<uint32_t>(strlen(cycle_doc)), &decoded));
        TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);

        const char* const duplicate_doc =
            "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
            "\"channels\":[{\"chain\":{\"effects\":["
            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
            "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
            "\"master_chain\":{}},"
            "\"editor\":{\"effect_audio\":[[2,0,0,0,2],null,null,null,null,null,null,null,null,"
            "null,null,null,null,null,null,null,null]}}";
        TEST(! Synth::decode_editor_bank_json(duplicate_doc, static_cast<uint32_t>(strlen(duplicate_doc)), &decoded));
        TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);

        const char* const empty_target_doc =
            "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{"
            "\"channels\":[{\"chain\":{\"effects\":["
            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}]}}],"
            "\"master_chain\":{}},"
            "\"editor\":{\"effect_audio\":[[3,0,0,0,0],null,null,null,null,null,null,null,null,"
            "null,null,null,null,null,null,null,null]}}";
        TEST(! Synth::decode_editor_bank_json(empty_target_doc,
                                              static_cast<uint32_t>(strlen(empty_target_doc)),
                                              &decoded));
        TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);

        // Same rules on the clipboard: a cycling, duplicate-incoming and
        // empty-target audio_next all refuse and the document output stays
        // untouched.
        Synth::EffectsDocument doc_sentinel = {};
        memset(static_cast<void*>(&doc_sentinel), 0x5A, sizeof(doc_sentinel));
        Synth::EffectsDocument doc_refused = {};
        memcpy(static_cast<void*>(&doc_refused), static_cast<void*>(&doc_sentinel), sizeof(doc_refused));
        const char* const clipboard_cycle = "{\"format\":\"synth-effects-v2\",\"audio_next\":[2,1,0,0,1],"
                                            "\"lfos\":[],\"effects\":["
                                            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
                                            "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                            "\"graph_layout\":[]}";
        TEST(! Synth::decode_effects_json(clipboard_cycle,
                                          static_cast<uint32_t>(strlen(clipboard_cycle)),
                                          &doc_refused));
        TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);

        // Two effects, input and slot 0 both feeding slot 1: the duplicate
        // incoming edge refuses the paste.
        const char* const clipboard_duplicate = "{\"format\":\"synth-effects-v2\",\"audio_next\":[2,0,0,0,2],"
                                                "\"lfos\":[],\"effects\":["
                                                "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]},"
                                                "{\"type\":\"chorus\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                                "\"graph_layout\":[]}";
        TEST(! Synth::decode_effects_json(clipboard_duplicate,
                                          static_cast<uint32_t>(strlen(clipboard_duplicate)),
                                          &doc_refused));
        TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);

        // One effect wired into the empty slot 2: the edge into an empty
        // slot refuses the paste.
        const char* const clipboard_empty_target = "{\"format\":\"synth-effects-v2\",\"audio_next\":[3,0,0,0,0],"
                                                   "\"lfos\":[],\"effects\":["
                                                   "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                                   "\"graph_layout\":[]}";
        TEST(! Synth::decode_effects_json(clipboard_empty_target,
                                          static_cast<uint32_t>(strlen(clipboard_empty_target)),
                                          &doc_refused));
        TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);
    }

    // The v3 out-array length must match the decoded effect count:
    // missing and surplus entries refuse in both formats, with the output
    // untouched.  Null is reserved for an empty chain, so an explicit null
    // with a nonempty chain refuses too, while an empty chain accepts it.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> sentinel_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& sentinel = *sentinel_storage;
        const char* const            routing_nulls =
            ",null,null,null,null,null,null,null,null,null,null,null,null,null,null,null,null]}}";
        const char* const valid_doc =
            "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":["
            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}]}}],\"master_chain\":{}},"
            "\"editor\":{\"effect_routing\":[{\"in\":1,\"out\":[2]}";
        // Control: the hand-authored v3 shape decodes.
        {
            char text[4096];
            snprintf(text, sizeof(text), "%s%s", valid_doc, routing_nulls);
            TEST(Synth::decode_editor_bank_json(text, static_cast<uint32_t>(strlen(text)), &decoded));
            TEST(decoded.bank.channel_chains[0].num_effects == 1);
            TEST(decoded.effect_routing[0].out[0] == Sculptor::effect_out_output);
        }
        memset(static_cast<void*>(&decoded), 0x5A, sizeof(decoded));
        sentinel = decoded;
        // Short out array: one effect needs one flag.
        {
            char text[4096];
            snprintf(text,
                     sizeof(text),
                     "%s%s",
                     "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":["
                     "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}]}}],\"master_chain\":{}},"
                     "\"editor\":{\"effect_routing\":[{\"in\":1,\"out\":[]}",
                     routing_nulls);
            TEST(! Synth::decode_editor_bank_json(text, static_cast<uint32_t>(strlen(text)), &decoded));
            TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
        }
        // Surplus out array: two flags for one effect.
        {
            char text[4096];
            snprintf(text,
                     sizeof(text),
                     "%s%s",
                     "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":["
                     "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}]}}],\"master_chain\":{}},"
                     "\"editor\":{\"effect_routing\":[{\"in\":1,\"out\":[2,0]}",
                     routing_nulls);
            TEST(! Synth::decode_editor_bank_json(text, static_cast<uint32_t>(strlen(text)), &decoded));
            TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
        }
        // Explicit null with a nonempty chain: null is reserved for an
        // empty chain, so the decode refuses with the output untouched.
        {
            char text[4096];
            snprintf(text,
                     sizeof(text),
                     "%s%s",
                     "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":["
                     "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}]}}],\"master_chain\":{}},"
                     "\"editor\":{\"effect_routing\":[null",
                     routing_nulls);
            TEST(! Synth::decode_editor_bank_json(text, static_cast<uint32_t>(strlen(text)), &decoded));
            TEST(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
        }
        // The empty-chain null stays legal: it decodes with no input wire
        // and no effect.
        {
            char text[4096];
            snprintf(text,
                     sizeof(text),
                     "%s%s",
                     "{\"instrument_editor_bank\":{\"channels\":[{\"chain\":{\"effects\":[]}}],\"master_chain\":{}},"
                     "\"editor\":{\"effect_routing\":[null",
                     routing_nulls);
            memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
            TEST(Synth::decode_editor_bank_json(text, static_cast<uint32_t>(strlen(text)), &decoded));
            TEST(decoded.bank.channel_chains[0].num_effects == 0);
            TEST(decoded.effect_routing[0].in == Sculptor::effect_in_disconnected);
        }
        // Clipboard short and long, plus the valid control.
        {
            const char* const      short_doc    = "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":1,\"out\":[]},"
                                                  "\"lfos\":[],\"effects\":["
                                                  "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                                  "\"graph_layout\":[]}";
            Synth::EffectsDocument doc_sentinel = {};
            memset(static_cast<void*>(&doc_sentinel), 0x5A, sizeof(doc_sentinel));
            Synth::EffectsDocument doc_refused = {};
            memcpy(static_cast<void*>(&doc_refused), static_cast<void*>(&doc_sentinel), sizeof(doc_refused));
            TEST(! Synth::decode_effects_json(short_doc, static_cast<uint32_t>(strlen(short_doc)), &doc_refused));
            TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);

            const char* const long_doc = "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":1,\"out\":[2,0]},"
                                         "\"lfos\":[],\"effects\":["
                                         "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                         "\"graph_layout\":[]}";
            TEST(! Synth::decode_effects_json(long_doc, static_cast<uint32_t>(strlen(long_doc)), &doc_refused));
            TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);

            const char* const      ok_doc = "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":1,\"out\":[2]},"
                                            "\"lfos\":[],\"effects\":["
                                            "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                            "\"graph_layout\":[]}";
            Synth::EffectsDocument doc_ok = {};
            TEST(Synth::decode_effects_json(ok_doc, static_cast<uint32_t>(strlen(ok_doc)), &doc_ok));
            TEST(doc_ok.chain.num_effects == 1);
            TEST(doc_ok.routing.out[0] == Sculptor::effect_out_output);

            // Explicit null routing with a nonempty chain refuses; an empty
            // chain accepts it (null is reserved for exactly that shape).
            const char* const null_doc = "{\"format\":\"synth-effects-v3\",\"routing\":null,\"lfos\":[],\"effects\":["
                                         "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                         "\"graph_layout\":[]}";
            TEST(! Synth::decode_effects_json(null_doc, static_cast<uint32_t>(strlen(null_doc)), &doc_refused));
            TEST(memcmp(&doc_refused, &doc_sentinel, sizeof(doc_refused)) == 0);

            const char* const      null_empty_doc = "{\"format\":\"synth-effects-v3\",\"routing\":null,"
                                                    "\"lfos\":[],\"effects\":[],\"graph_layout\":[]}";
            Synth::EffectsDocument doc_empty      = {};
            TEST(Synth::decode_effects_json(null_empty_doc, static_cast<uint32_t>(strlen(null_empty_doc)), &doc_empty));
            TEST(doc_empty.chain.num_effects == 0);
            TEST(doc_empty.routing.in == Sculptor::effect_in_disconnected);

            // The bypass input value round-trips through the codec and an
            // out-of-range value refuses.
            const char* const      bypass_doc = "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":2,\"out\":[0]},"
                                                "\"lfos\":[],\"effects\":["
                                                "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                                "\"graph_layout\":[]}";
            Synth::EffectsDocument doc_bypass = {};
            TEST(Synth::decode_effects_json(bypass_doc, static_cast<uint32_t>(strlen(bypass_doc)), &doc_bypass));
            TEST(doc_bypass.routing.in == Sculptor::effect_in_output);

            // A bank saved before the one-output-feed invariant may carry the
            // bypass and an output holder together; the decode normalizes to
            // the saved audible state (the bypass wins) instead of refusing.
            const char* const legacy_doc =
                "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":2,\"out\":[2]},"
                "\"lfos\":[],\"effects\":[{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                "\"graph_layout\":[]}";
            Synth::EffectsDocument doc_legacy = {};
            TEST(Synth::decode_effects_json(legacy_doc, static_cast<uint32_t>(strlen(legacy_doc)), &doc_legacy));
            TEST(doc_legacy.routing.in == Sculptor::effect_in_output);
            TEST(doc_legacy.routing.out[0] == Sculptor::effect_out_disconnected);
            const char* const      range_doc = "{\"format\":\"synth-effects-v3\",\"routing\":{\"in\":3,\"out\":[2]},"
                                               "\"lfos\":[],\"effects\":["
                                               "{\"type\":\"delay\",\"enabled\":true,\"params\":[{},{},{}]}],"
                                               "\"graph_layout\":[]}";
            Synth::EffectsDocument doc_range = {};
            memset(static_cast<void*>(&doc_range), 0x5A, sizeof(doc_range));
            TEST(! Synth::decode_effects_json(range_doc, static_cast<uint32_t>(strlen(range_doc)), &doc_range));
            TEST(memcmp(&doc_range, &doc_sentinel, sizeof(doc_range)) == 0);
        }
    }
}

// Add effect: a new effect always inserts immediately after the
// connected_to_output holder and takes the flag; the old holder demotes to
// connected_to_next; with no holder it appends after the last occupied
// slot; a fresh empty chain also gets in = Sculptor::effect_in_first_slot.
static void fx_add_effect_default_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    const std::unique_ptr<Synth::InstrumentEditorBank> candidate_storage =
        std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank& candidate = *candidate_storage;

    // Saved kind-4 records for the effects whose boxes must not move: each
    // insertion case installs distinct records first, then asserts the add
    // leaves them byte-identical (names, positions, size overrides).
    Synth::GraphNodeLayout parked_expected[2]     = {};
    const auto             install_parked_records = [&](const char* const(&names)[2]) {
        for (uint32_t i = 0; i < 2; ++i) {
            Synth::GraphNodeLayout& record = source.graph_layout[source.graph_layout_count++];
            record                         = Synth::GraphNodeLayout{};
            record.kind                    = 4;
            record.channel                 = 0;
            record.x                       = 40.0f + 128.0f * static_cast<float>(i);
            record.y                       = 160.0f + 144.0f * static_cast<float>(i);
            record.width_override          = 120.0f + 16.0f * static_cast<float>(i);
            record.height_override         = 64.0f + 8.0f * static_cast<float>(i);
            strcpy(record.name, names[i]);
            parked_expected[i] = record;
        }
    };
    const auto assert_parked_records = [&](const Synth::InstrumentEditorBank& bank) {
        TEST(bank.graph_layout_count == 2);
        for (uint32_t i = 0; i < 2; ++i) {
            TEST(strcmp(bank.graph_layout[i].name, parked_expected[i].name) == 0);
            TEST(memcmp(&bank.graph_layout[i], &parked_expected[i], sizeof(parked_expected[i])) == 0);
        }
    };

    // Holder at slot 2, parked slots 0 and 1 before it: the new effect lands
    // between the holder and nothing, parked slots stay put.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].out[2] = Sculptor::effect_out_output;
    {
        const char* const names[2] = { "Delay", "Chorus" };
        install_parked_records(names);
    }
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::chorus, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 4);
    TEST(candidate.bank.channel_chains[0].effects[3].type == Synth::EffectType::chorus);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
    TEST(candidate.bank.channel_chains[0].effects[1].bindings[0].base_value == 2.0f);
    TEST(candidate.bank.channel_chains[0].effects[2].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output };
        check_routing(candidate, 0, 0, expected);
    }
    TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[0], candidate.effect_routing[0]));
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);
    assert_parked_records(candidate);

    // Holder at slot 0 with parked slots behind it: the new effect inserts
    // between the holder and the parked tail, which shifts without losing
    // its place records (the add writes no layout records, so the parked
    // boxes never move).
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].out[0] = Sculptor::effect_out_output;
    {
        const char* const names[2] = { "Chorus", "Distortion" };
        install_parked_records(names);
    }
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::reverb, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 4);
    TEST(candidate.bank.channel_chains[0].effects[0].bindings[0].base_value == 1.0f);
    TEST(candidate.bank.channel_chains[0].effects[1].type == Synth::EffectType::reverb);
    TEST(candidate.bank.channel_chains[0].effects[2].bindings[0].base_value == 2.0f);
    TEST(candidate.bank.channel_chains[0].effects[3].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 0, expected);
    }
    assert_parked_records(candidate);
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);

    // Holder at slot 1 with a parked slot behind it: the new effect lands
    // between the holder and the parked tail.
    build_routing_fixture(&source, 0, 3);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    source.effect_routing[0].out[1] = Sculptor::effect_out_output;
    {
        const char* const names[2] = { "Delay", "Distortion" };
        install_parked_records(names);
    }
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::reverb, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 4);
    TEST(candidate.bank.channel_chains[0].effects[2].type == Synth::EffectType::reverb);
    TEST(candidate.bank.channel_chains[0].effects[1].bindings[0].base_value == 2.0f);
    TEST(candidate.bank.channel_chains[0].effects[3].bindings[0].base_value == 3.0f);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_next,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    assert_parked_records(candidate);
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);

    // No holder: the new effect appends after the last occupied slot and
    // takes connected_to_output.
    build_routing_fixture(&source, 0, 2);
    source.effect_routing[0].in     = Sculptor::effect_in_first_slot;
    source.effect_routing[0].out[0] = Sculptor::effect_out_next;
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::distortion, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 3);
    TEST(candidate.bank.channel_chains[0].effects[2].type == Synth::EffectType::distortion);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_next,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    TEST(Sculptor::validate_effect_routing(candidate.bank.channel_chains[0], candidate.effect_routing[0]));

    // Fresh empty chain: the first effect takes the output and the input.
    build_routing_fixture(&source, 0, 0);
    TEST(Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::delay, &candidate));
    TEST(candidate.bank.channel_chains[0].num_effects == 1);
    {
        const uint8_t expected[Synth::max_chain_effects] = { Sculptor::effect_out_output,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected,
                                                             Sculptor::effect_out_disconnected };
        check_routing(candidate, 0, 1, expected);
    }
    check_chain_contiguous(candidate.bank.channel_chains[0], candidate.effect_routing[0]);

    // A full chain refuses untouched.
    build_routing_fixture(&source, 0, 4);
    const std::unique_ptr<Synth::InstrumentEditorBank> before_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       before         = *before_storage;
    candidate                                                         = source;
    memcpy(&before, &candidate, sizeof(before));
    TEST(! Sculptor::fx_add_effect_candidate(&source, 0, Synth::EffectType::delay, &candidate));
    TEST(memcmp(&candidate, &before, sizeof(candidate)) == 0);
}

// The oscillator-only slot-disable stamp: a five-parameter effect
// projects 53 slots, well past osc_slot_count = 18, so a slot-count
// guard alone cannot exclude effect nodes.  The disable function must
// refuse non-oscillator nodes outright instead of reading oscillator
// slot indices out of them.
static void fx_slot_disable_stamp_regressions()
{
    const std::unique_ptr<Synth::InstrumentEditorBank> source_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       source         = *source_storage;
    Synth::Instrument                                  instrument     = {};
    const std::unique_ptr<Sculptor::Graph>             graph_storage  = std::make_unique<Sculptor::Graph>();
    Sculptor::Graph&                                   graph          = *graph_storage;
    Sculptor::EffectGraphMapping                       fx_mapping     = {};
    build_zone_fixture(&source, &instrument);
    Synth::EffectChainBinding& chain = add_cc_chain_fixture(&source, 0);
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::compressor);
    chain.effects[0].enabled = true;
    TEST(Sculptor::project_effect_chain_to_graph(source, 0, &graph, &fx_mapping));

    const uint32_t fx_node = fx_mapping.effect_nodes[0];
    TEST(fx_node != Sculptor::pool_no_slot);
    const Sculptor::Node& fx = graph.node(fx_node);
    TEST(fx.slots.num_allocated == 3 + 5 * Sculptor::fx_param_stride);
    // The disable function reads waveform list values at the oscillator
    // contract indices (slot 1 and 28).  The fx rows there are
    // value-initialized zeros: slot 1 is the Out dot, whose zero value
    // reads as sine (the wave a list starts at sine), and slot 28 reads
    // as off (the wave b list starts at off).  The write below pins slot
    // 28 to list 0 explicitly.
    Sculptor::PropertyValue sine = {};
    sine.list_index              = 0;
    graph.set_slot_value(fx_node, 28, sine); // waveform b contract index
    for (uint32_t slot = 0; slot < fx.slots.num_allocated; ++slot) {
        TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(fx, slot));
    }

    // Small non-oscillator nodes stay unstamped, so the disable function
    // returns false for every slot regardless of slot count.
    const uint32_t small_nodes[] = { fx_mapping.input_node, fx_mapping.output_node, fx_mapping.midi_node };
    for (uint32_t node_idx : small_nodes) {
        TEST(node_idx != Sculptor::pool_no_slot);
        const Sculptor::Node& small = graph.node(node_idx);
        TEST(small.slots.num_allocated < Sculptor::osc_slot_count);
        for (uint32_t slot = 0; slot < small.slots.num_allocated; ++slot) {
            TEST(! Sculptor::oscillator_slot_waveform_mode_disabled(small, slot));
        }
    }
}

// ---- Distortion: output gain + first-order ADAA ----
// distortion_adaa_reference is a float32 CPU mirror of apply_distortion in
// synth_effect.comp.glsl, in the same order of operations: raw previous input
// x0, u = x * drive, F(u) = |u| + log(1 + exp(-2|u|)) - ln2,
// wet = (F(u1) - F(u0)) / (u1 - u0), with tanh(0.5 * (u0 + u1)) when
// |u1 - u0| < 1e-2, result = mix(x, wet, mix) * gain. The CPU mirror does
// not establish GPU equivalence; listening to production audio covers that.
// distortion_tanh_reference is the older stateless formula.
static float distortion_log_cosh(float u)
{
    const float a = fabsf(u);
    return a + logf(1.0f + expf(-2.0f * a)) - 0.69314718f;
}

static void distortion_adaa_reference(const float* in,
                                      uint32_t     count,
                                      float        drive,
                                      float        mix,
                                      float        gain,
                                      float*       prev,
                                      float*       out)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float u0 = *prev * drive;
        const float u1 = in[i] * drive;
        const float du = u1 - u0;
        float       wet;
        if (fabsf(du) < 1e-2f) {
            wet = tanhf(0.5f * (u0 + u1));
        }
        else {
            wet = (distortion_log_cosh(u1) - distortion_log_cosh(u0)) / du;
        }
        out[i] = (in[i] * (1.0f - mix) + wet * mix) * gain;
        *prev  = in[i];
    }
}

static void distortion_tanh_reference(const float* in, uint32_t count, float drive, float mix, float* out)
{
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = in[i] * (1.0f - mix) + tanhf(in[i] * drive) * mix;
    }
}

// Bin-centered sine: `bin` whole cycles per `count` samples, so the DFT below
// has no leakage. Element -1 (the carried previous input) is *first_prev.
static void distortion_make_sine(float* out, uint32_t count, uint32_t bin, float amplitude, float* first_prev)
{
    constexpr double two_pi = 6.283185307179586;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = static_cast<float>(static_cast<double>(amplitude) * sin(two_pi * bin * i / count));
    }
    *first_prev = static_cast<float>(static_cast<double>(amplitude) * sin(-two_pi * bin / count));
}

// 10*log10(non-harmonic / total) over DFT bins 1..count/2-1; harmonic bins are
// multiples of `bin`. Lower is cleaner.
static double distortion_non_harmonic_db(const float* signal, uint32_t count, uint32_t bin)
{
    const std::unique_ptr<FixedArray<double, 4096>> cos_storage = std::make_unique<FixedArray<double, 4096>>();
    const std::unique_ptr<FixedArray<double, 4096>> sin_storage = std::make_unique<FixedArray<double, 4096>>();
    TEST(count == 4096);
    constexpr double two_pi = 6.283185307179586;
    for (uint32_t i = 0; i < count; ++i) {
        cos_storage->elements[i] = cos(two_pi * i / count);
        sin_storage->elements[i] = sin(two_pi * i / count);
    }
    double harmonic     = 0.0;
    double non_harmonic = 0.0;
    for (uint32_t k = 1; k < count / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(k) * i) % count);
            re += static_cast<double>(signal[i]) * cos_storage->elements[phase];
            im -= static_cast<double>(signal[i]) * sin_storage->elements[phase];
        }
        (k % bin == 0 ? harmonic : non_harmonic) += re * re + im * im;
    }
    return 10.0 * log10(non_harmonic / (harmonic + non_harmonic) + 1e-30);
}

static double distortion_rms(const float* signal, uint32_t count)
{
    double sum = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        sum += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);
    }
    return sqrt(sum / count);
}

// Param and state layout the distortion shader and editor depend on.
static void check_distortion_param_layout()
{
    TEST(Synth::get_effect_param_floats(Synth::EffectType::distortion) == 3);
    TEST(Synth::get_effect_state_floats(Synth::EffectType::distortion) == 2); // previous raw input, left and right
    const Sculptor::EffectTypeInfo& info = Sculptor::effect_type_info(Synth::EffectType::distortion);
    TEST(info.num_params == 3);
    TEST(strcmp(info.params[2].name, "Output Gain") == 0);
    TEST(info.params[2].min_value == 0.0f);
    TEST(info.params[2].max_value == 4.0f);
    TEST(! info.params[2].logarithmic);
    TEST(Sculptor::effect_param_default(Synth::EffectType::distortion, 0) == 1.0f);
    TEST(Sculptor::effect_param_default(Synth::EffectType::distortion, 1) == 1.0f);
    TEST(Sculptor::effect_param_default(Synth::EffectType::distortion, 2) == 1.0f);

    Synth::EffectChainBinding chain = {};
    chain.num_effects               = 1;
    Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
    TEST(chain.effects[0].bindings[0].base_value == 1.0f);
    TEST(chain.effects[0].bindings[1].base_value == 1.0f);
    TEST(chain.effects[0].bindings[2].base_value == 1.0f);
}

// C4: output level follows the output gain exactly (post-mix multiply).
static void check_distortion_output_gain_level()
{
    constexpr uint32_t                              count        = 1024;
    const std::unique_ptr<FixedArray<float, count>> in_storage   = std::make_unique<FixedArray<float, count>>();
    const std::unique_ptr<FixedArray<float, count>> full_storage = std::make_unique<FixedArray<float, count>>();
    const std::unique_ptr<FixedArray<float, count>> half_storage = std::make_unique<FixedArray<float, count>>();
    const std::unique_ptr<FixedArray<float, count>> zero_storage = std::make_unique<FixedArray<float, count>>();
    float                                           first_prev   = 0.0f;
    distortion_make_sine(in_storage->elements, count, 20, 0.5f, &first_prev);

    float prev = first_prev;
    distortion_adaa_reference(in_storage->elements, count, 8.0f, 1.0f, 1.0f, &prev, full_storage->elements);
    prev = first_prev;
    distortion_adaa_reference(in_storage->elements, count, 8.0f, 1.0f, 0.5f, &prev, half_storage->elements);
    prev = first_prev;
    distortion_adaa_reference(in_storage->elements, count, 8.0f, 1.0f, 0.0f, &prev, zero_storage->elements);

    const double full_rms = distortion_rms(full_storage->elements, count);
    const double half_rms = distortion_rms(half_storage->elements, count);
    TEST(full_rms > 0.1);
    TEST(fabs(20.0 * log10(half_rms / full_rms) - 20.0 * log10(0.5)) < 1e-3);
    TEST(distortion_rms(zero_storage->elements, count) == 0.0);
    for (uint32_t i = 0; i < count; ++i) {
        TEST(half_storage->elements[i] == 0.5f * full_storage->elements[i]); // power of two: exact
    }
}

// C5: ADAA1 measures less non-harmonic energy than the pinned tanh baseline.
static void check_distortion_adaa_reduces_aliasing()
{
    constexpr uint32_t                              count       = 4096;
    const std::unique_ptr<FixedArray<float, count>> in_storage  = std::make_unique<FixedArray<float, count>>();
    const std::unique_ptr<FixedArray<float, count>> out_storage = std::make_unique<FixedArray<float, count>>();

    struct Case {
        uint32_t bin; // 186 = 2 kHz, 372 = 4 kHz at 44.1 kHz
        float    drive;
        double   tanh_db;  // tanh baseline at this bin and drive, amplitude 0.8
        double   adaa1_db; // ADAA1 at the same bin and drive
    };

    const Case cases[] = { { 186, 16.0f, -22.4, -30.4 }, { 372, 4.0f, -29.0, -37.2 } };
    for (const Case& test_case : cases) {
        float first_prev = 0.0f;
        distortion_make_sine(in_storage->elements, count, test_case.bin, 0.8f, &first_prev);

        // The metric must reproduce the pinned baseline before it can judge ADAA.
        distortion_tanh_reference(in_storage->elements, count, test_case.drive, 1.0f, out_storage->elements);
        const double tanh_db = distortion_non_harmonic_db(out_storage->elements, count, test_case.bin);
        TEST(fabs(tanh_db - test_case.tanh_db) < 0.5);

        float prev = first_prev;
        distortion_adaa_reference(in_storage->elements,
                                  count,
                                  test_case.drive,
                                  1.0f,
                                  1.0f,
                                  &prev,
                                  out_storage->elements);
        const double adaa_db = distortion_non_harmonic_db(out_storage->elements, count, test_case.bin);
        TEST(adaa_db <= test_case.tanh_db - 6.0);
        TEST(fabs(adaa_db - test_case.adaa1_db) < 1.0);
    }
}

// Two 256-sample blocks with the carried previous input equal one 512 pass.
static void check_distortion_block_continuity()
{
    constexpr uint32_t block = 256;
    float              in[2 * block];
    float              whole[2 * block];
    float              split[2 * block];
    float              first_prev = 0.0f;
    distortion_make_sine(in, 2 * block, 7, 0.8f, &first_prev);

    float prev = first_prev;
    distortion_adaa_reference(in, 2 * block, 12.0f, 0.75f, 0.9f, &prev, whole);
    prev = first_prev;
    distortion_adaa_reference(in, block, 12.0f, 0.75f, 0.9f, &prev, split);
    distortion_adaa_reference(in + block, block, 12.0f, 0.75f, 0.9f, &prev, split + block);
    TEST(memcmp(whole, split, sizeof(whole)) == 0);
    TEST(prev == in[2 * block - 1]);
}

// Missing-trailing-params rule: decode_effect default-fills params omitted
// from a lenient bank slot (all of them when the key is absent) for any
// effect type; distortion's later-added output gain and a cut delay slot are
// the cases here. A full array round-trips, extra entries are refused, and
// the strict effects document still requires the exact count.
static void check_effect_missing_params_compat()
{
    const std::unique_ptr<Synth::EffectsDocument> doc_storage = std::make_unique<Synth::EffectsDocument>();
    Synth::EffectsDocument&                       decoded     = *doc_storage;

    // Strict documents keep the exact count: short arrays are refused.
    const char two_entry[] = "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\","
                             "\"params\":[{\"base_value\":4},{\"base_value\":0.75}]}],\"graph_layout\":[]}";
    TEST(! Synth::decode_effects_json(two_entry, static_cast<uint32_t>(sizeof(two_entry) - 1), &decoded));

    const char one_entry[] = "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\","
                             "\"params\":[{\"base_value\":4}]}],\"graph_layout\":[]}";
    TEST(! Synth::decode_effects_json(one_entry, static_cast<uint32_t>(sizeof(one_entry) - 1), &decoded));

    // Three entries round-trip through the strict effects document.
    memset(static_cast<void*>(&decoded), 0, sizeof(decoded));
    decoded.chain.num_effects                                   = 1;
    decoded.chain.effects[0].type                               = Synth::EffectType::distortion;
    decoded.chain.effects[0].enabled                            = true;
    decoded.chain.effects[0].bindings[0].base_value             = 4.5f;
    decoded.chain.effects[0].bindings[1].base_value             = 0.75f;
    decoded.chain.effects[0].bindings[2].base_value             = 0.25f;
    const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
    char (&text)[65536]                                         = text_storage->elements;
    const uint32_t length = Synth::encode_effects_json(&decoded, text, sizeof(text));
    TEST(length > 0);
    TEST(strstr(text, "{\"base_value\":0.25}") != nullptr);
    Synth::EffectsDocument round_trip = {};
    TEST(Synth::decode_effects_json(text, length, &round_trip));
    TEST(round_trip.chain.effects[0].bindings[0].base_value == 4.5f);
    TEST(round_trip.chain.effects[0].bindings[1].base_value == 0.75f);
    TEST(round_trip.chain.effects[0].bindings[2].base_value == 0.25f);

    // The editor bank carrier: a saved 3-entry slot round-trips and validates,
    // and the same slot cut back to the old 2-entry shape loads with gain 1.0.
    const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       bank         = *bank_storage;
    Synth::init_default_bank(&bank.bank);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    Synth::EffectSlotBinding* const slot = enabled_effect(bank.bank, 0, 0, Synth::EffectType::distortion);
    slot->bindings[0].base_value         = 4.5f;
    slot->bindings[1].base_value         = 0.75f;
    slot->bindings[2].base_value         = 0.25f;
    TEST(Synth::validate_instrument_bank(&bank.bank));
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> payload_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&payload)[Synth::library_payload_max] = payload_storage->elements;
    const uint32_t payload_length               = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(payload_length > 0);
    const std::unique_ptr<Synth::InstrumentEditorBank> loaded_storage = std::make_unique<Synth::InstrumentEditorBank>();
    Synth::InstrumentEditorBank&                       loaded         = *loaded_storage;
    memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
    TEST(Synth::decode_editor_bank_json(payload, payload_length, &loaded));
    TEST(loaded.bank.channel_chains[0].effects[0].bindings[2].base_value == 0.25f);
    TEST(Synth::validate_instrument_bank(&loaded.bank));

    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> cut_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&cut)[Synth::library_payload_max] = cut_storage->elements;
    const uint32_t cut_length               = splice_single_occurrence(payload,
                                                                       payload_length,
                                                                       "{\"base_value\":0.75},{\"base_value\":0.25}]",
                                                                       "{\"base_value\":0.75}]",
                                                                       cut,
                                                                       sizeof(cut));
    TEST(cut_length > 0);
    if (cut_length > 0) {
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(Synth::decode_editor_bank_json(cut, cut_length, &loaded));
        TEST(loaded.bank.channel_chains[0].effects[0].bindings[0].base_value == 4.5f);
        TEST(loaded.bank.channel_chains[0].effects[0].bindings[1].base_value == 0.75f);
        TEST(loaded.bank.channel_chains[0].effects[0].bindings[2].base_value == 1.0f);
        TEST(Synth::validate_instrument_bank(&loaded.bank));
    }
    // A lenient bank slot with no params key still decodes, gain 1.0.
    const std::unique_ptr<FixedArray<char, Synth::library_payload_max>> bare_storage =
        std::make_unique<FixedArray<char, Synth::library_payload_max>>();
    char (&bare)[Synth::library_payload_max] = bare_storage->elements;
    const uint32_t bare_length =
        splice_single_occurrence(payload,
                                 payload_length,
                                 ",\"params\":[{\"base_value\":4.5},{\"base_value\":0.75},{\"base_value\":0.25}]",
                                 "",
                                 bare,
                                 sizeof(bare));
    TEST(bare_length > 0);
    if (bare_length > 0) {
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(Synth::decode_editor_bank_json(bare, bare_length, &loaded));
        TEST(loaded.bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(loaded.bank.channel_chains[0].effects[0].bindings[2].base_value == 1.0f);
        TEST(Synth::validate_instrument_bank(&loaded.bank));
    }

    // Four entries are refused.
    const uint32_t four_length = splice_single_occurrence(payload,
                                                          payload_length,
                                                          "{\"base_value\":0.25}]",
                                                          "{\"base_value\":0.25},{\"base_value\":0.5}]",
                                                          cut,
                                                          sizeof(cut));
    TEST(four_length > 0);
    if (four_length > 0) {
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(! Synth::decode_editor_bank_json(cut, four_length, &loaded));
    }
    // Another type follows the same rule: a delay slot cut to one entry takes
    // the delay defaults for the rest.
    Synth::init_default_bank(&bank.bank);
    Sculptor::set_identity_osc_sources(&bank, 0, 0);
    Synth::EffectSlotBinding* const delay = enabled_effect(bank.bank, 0, 0, Synth::EffectType::delay);
    delay->bindings[0].base_value         = 3000.0f;
    delay->bindings[1].base_value         = 0.5f;
    delay->bindings[2].base_value         = 0.25f;
    const uint32_t delay_length           = Synth::encode_editor_bank_json(&bank, payload, sizeof(payload));
    TEST(delay_length > 0);
    const uint32_t delay_cut_length = splice_single_occurrence(payload,
                                                               delay_length,
                                                               ",{\"base_value\":0.5},{\"base_value\":0.25}]",
                                                               "]",
                                                               cut,
                                                               sizeof(cut));
    TEST(delay_cut_length > 0);
    if (delay_cut_length > 0) {
        memset(static_cast<void*>(&loaded), 0, sizeof(loaded));
        TEST(Synth::decode_editor_bank_json(cut, delay_cut_length, &loaded));
        const Synth::EffectSlotBinding& cut_delay = loaded.bank.channel_chains[0].effects[0];
        TEST(cut_delay.type == Synth::EffectType::delay);
        TEST(cut_delay.bindings[0].base_value == 3000.0f);
        TEST(cut_delay.bindings[1].base_value == Sculptor::effect_param_default(Synth::EffectType::delay, 1));
        TEST(cut_delay.bindings[2].base_value == Sculptor::effect_param_default(Synth::EffectType::delay, 2));
        TEST(Synth::validate_instrument_bank(&loaded.bank));
    }

    // The strict effects document still requires params on a typed slot, and
    // the exact count (the generic rule is for lenient bank slots only).
    const char strict_missing[] =
        "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"distortion\"}],\"graph_layout\":[]}";
    TEST(! Synth::decode_effects_json(strict_missing, static_cast<uint32_t>(sizeof(strict_missing) - 1), &decoded));
    const char strict_short[] = "{\"format\":\"synth-effects-v1\",\"lfos\":[],\"effects\":[{\"type\":\"delay\","
                                "\"params\":[{\"base_value\":3000}]}],\"graph_layout\":[]}";
    TEST(! Synth::decode_effects_json(strict_short, static_cast<uint32_t>(sizeof(strict_short) - 1), &decoded));
}

static void distortion_m1_regressions()
{
    check_distortion_param_layout();
    check_distortion_output_gain_level();
    check_distortion_adaa_reduces_aliasing();
    check_distortion_block_continuity();

    check_effect_missing_params_compat();
}

static void check_effects_inline_0()
{

    // A stateful chain expands onto pool nodes with state at the region base, one clear range.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].base_value         = 300.0f;
        delay->bindings[0].lfo_desc_id        = 1;
        delay->bindings[0].lfo_depth          = 50.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.slots[0][0].allocated_for == Synth::EffectType::delay);
        TEST(plan.num_nodes == 2); // dest + LFO leaf
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[0].type == Synth::EffectType::delay);
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].params[0] == 300.0f);
        TEST(chains[0].effects[0].state_offs == fake_region_base);
        TEST(chains[0].effects[0].src_param_id[0] == 50); // dest node allocated first
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        TEST(writer.last_dest_lfo_node == 51); // LFO leaf allocated second

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        TEST(Synth::take_effect_clear_ranges().count == 0); // consumed once
    } // a modulated FIR slope binding expands like any other
    // parameter - the preflight consumes one mod-param entry and the
    // commit fills params[2] from the bank's base value.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const fir = enabled_effect(bank, 0, 0, Synth::EffectType::fir);
        fir->bindings[2].base_value         = 37.0f;
        fir->bindings[2].lfo_desc_id        = 1;
        fir->bindings[2].lfo_depth          = 0.5f;
        TEST(Synth::validate_instrument_bank(&bank));
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 50, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[0].type == Synth::EffectType::fir);
        TEST(chains[0].effects[0].params[2] == 37.0f);
        TEST(writer.dest_count == 1 && writer.lfo_count == 1);
        (void)Synth::take_effect_clear_ranges();
    }

    // An unchanged slot keeps its state offset and emits no clear; a type change re-allocates.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        // Same effect: offset preserved, nothing to clear, no new consumption.
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(! plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::delay));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(Synth::take_effect_clear_ranges().count == 0);

        // Type change releases the old range and clears its replacement before first use.
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::chorus));
    }
    // Over-budget and over-pool preflights fail without touching committed state.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;

        // Master: 4 delays - exactly the worst chain, fits the budget.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; slot++) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();
        const uint32_t committed_consumed = plan.consumed_bytes;

        // A 5th delay anywhere exceeds the budget: preflight fails, nothing mutates.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed);    // plan untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // no clears recorded

        // The committed configuration (5th delay dropped again) still re-preflights
        // cleanly via the preservation path.
        bank.channel_chains[0].num_effects = 0;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == committed_consumed);

        // 33 modulated params exceed the modulation pool: rejected wherever they sit.
        Synth::init_effect_state_region(fake_region_base);
        const std::unique_ptr<Synth::InstrumentBank> pool_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       pool_bank         = *pool_bank_storage;
        make_valid_bank(pool_bank);
        uint32_t num_modulated = 0;
        for (uint32_t channel = 0; channel < Synth::max_channels && num_modulated < 33; channel++) {
            Synth::EffectChainBinding& chain = pool_bank.channel_chains[channel];
            chain.num_effects                = 4;
            for (uint32_t slot = 0; slot < 4 && num_modulated < 33; slot++) {
                chain.effects[slot].type = Synth::EffectType::distortion; // 3 params; disabled, so it claims no state
                for (uint32_t param = 0; param < 2 && num_modulated < 33; param++) {
                    chain.effects[slot].bindings[param].lfo_desc_id = 1;
                    num_modulated++;
                }
            }
        }
        TEST(num_modulated == 33);
        TEST(! Synth::preflight_effect_expansion(pool_bank, &plan, &error));

        // An input-only binding (no LFO) costs one node, not two.
        Synth::init_effect_state_region(fake_region_base);
        const std::unique_ptr<Synth::InstrumentBank> input_bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       input_bank         = *input_bank_storage;
        make_valid_bank(input_bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(input_bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs         = 1;
        delay->bindings[0].inputs[0].source   = Synth::ModSource::mod_wheel;
        TEST(Synth::preflight_effect_expansion(input_bank, &plan, &error));
        TEST(plan.num_nodes == 1);
    }
    // A shrinking chain leaves no stale instances behind.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 0, 1, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 2);

        bank.channel_chains[0].num_effects = 1;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].num_effects == 1);
        TEST(chains[0].effects[1].type == Synth::EffectType::none);
        TEST(! chains[0].effects[1].enabled);
        TEST(chains[0].effects[1].state_offs == 0);
        TEST(chains[0].effects[1].src_param_id[0] == 0);
    }

    // Distortion claims its two-float state: an aligned region, an offset and a clear range.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const distortion = enabled_effect(bank, 0, 0, Synth::EffectType::distortion);
        distortion->bindings[0].base_value         = 5.0f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[0][0].state_offs == fake_region_base);
        TEST(plan.slots[0][0].needs_clear);
        TEST(plan.consumed_bytes == Synth::get_effect_state_bytes(Synth::EffectType::distortion));

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[0].effects[0].enabled);
        TEST(chains[0].effects[0].state_offs == fake_region_base);
        TEST(Synth::take_effect_clear_ranges().count == 1);
    }
    // A preflight that fails validation leaves the caller's plan byte-identical.  The commit
    // is only reachable after a passing preflight (set_current_bank returns early on
    // failure), so a rejected bank can never reach the writer or the chains.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const delay = enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        delay->bindings[0].num_inputs         = Synth::max_mod_inputs + 1; // over the input bound

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan = {};
        plan.consumed_bytes             = 0xABCD;
        const char* error               = nullptr;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 0xABCD);                // untouched by the failed run
        TEST(Synth::take_effect_clear_ranges().count == 0); // nothing recorded either
    }
    // Each commit-safety rule the writer relies on is checked by the preflight.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        const char*                error = nullptr;
        Synth::EffectExpansionPlan plan  = {};

        Synth::init_effect_state_region(fake_region_base);

        // Dangling LFO descriptor.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 9;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Non-finite LFO depth.
        bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id = 1;
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth   = NAN;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // Per-note MIDI source on a channel effect.
        bank.channel_chains[0].effects[0].bindings[0].lfo_depth        = 1.0f;
        bank.channel_chains[0].effects[0].bindings[1].num_inputs       = 1;
        bank.channel_chains[0].effects[0].bindings[1].inputs[0].source = Synth::ModSource::velocity;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));

        // A per-note MIDI source on the master chain (here: via the LFO depth
        // source) stays refused, like on every channel chain.
        Synth::EffectSlotBinding* const reverb =
            enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        reverb->bindings[0].lfo_desc_id      = 1;
        reverb->bindings[0].lfo_depth_source = Synth::ModSource::velocity;
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
    }
    // Only final-plan allocations require clearing after several undrained publishes.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        enabled_effect(bank, 1, 0, Synth::EffectType::chorus);

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);

        // Re-type both slots: the drained second publish allocates fresh state and clears it.
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        enabled_effect(bank, 1, 0, Synth::EffectType::delay);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 2);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == delay_bytes);
        TEST(clears.ranges[1].offset == fake_region_base + delay_bytes);
        TEST(clears.ranges[1].bytes == delay_bytes);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // Channel 15 and the master chain expand like any other chain.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const ch15 =
            enabled_effect(bank, Synth::max_channels - 1, 0, Synth::EffectType::delay);
        ch15->bindings[0].base_value        = 100.0f;
        Synth::EffectSlotBinding* const rev = enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        rev->bindings[0].base_value         = 0.7f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[Synth::max_channels - 1][0].state_offs == fake_region_base);
        const uint32_t delay_bytes = Synth::get_effect_state_bytes(Synth::EffectType::delay);
        TEST(plan.slots[Synth::max_channels][0].state_offs == fake_region_base + delay_bytes);

        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(chains[Synth::max_channels - 1].effects[0].params[0] == 100.0f);
        TEST(master.effects[0].type == Synth::EffectType::reverb);
        TEST(master.effects[0].params[0] == 0.7f);
        TEST(master.effects[0].state_offs == fake_region_base + delay_bytes);
    }
    // Init and two drained publishes clear only the last live allocation.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectSlotBinding* const chorus = enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        chorus->bindings[0].base_value         = 1.5f;

        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };

        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        for (uint32_t republish = 0; republish < 2; republish++) {
            // Each drained publish re-types the slot, so it allocates and clears fresh state.
            enabled_effect(bank, 0, 0, (republish % 2) ? Synth::EffectType::chorus : Synth::EffectType::delay);
            TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        }

        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(clears.ranges[0].bytes == Synth::get_effect_state_bytes(Synth::EffectType::chorus));
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }

    // Live ranges are reserved before newly introduced earlier slots are placed.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, Synth::max_channels, 0, Synth::EffectType::delay);
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.slots[Synth::max_channels][0].state_offs == fake_region_base);
        TEST(! plan.slots[Synth::max_channels][0].needs_clear);
        TEST(plan.slots[0][0].state_offs == fake_region_base + Synth::get_effect_state_bytes(Synth::EffectType::delay));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == plan.slots[0][0].state_offs);
    }
    // Unrendered lineage survives compatible publishes exactly once.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };
        for (uint32_t publish = 0; publish < 3; ++publish) {
            TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
            TEST(plan.slots[0][0].needs_clear);
            Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        }
        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 1);
        TEST(clears.ranges[0].offset == fake_region_base);
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(! plan.slots[0][0].needs_clear);
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // Repeated full bypass, enable churn and type churn have bounded live state.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };
        uint32_t                   successful_transitions      = 0;
        for (uint32_t cycle = 0; cycle < 100; ++cycle) {
            for (uint32_t phase = 0; phase < 4; ++phase) {
                bank.master_chain = {};
                if (phase == 0 || phase == 2) {
                    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
                        enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay)
                            ->bindings[0]
                            .base_value = 71.0f + float(slot);
                    }
                }
                else if (phase == 3) {
                    for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
                        enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::chorus);
                    }
                }
                if (! Synth::preflight_effect_expansion(bank, &plan, &error)) {
                    break;
                }
                ++successful_transitions;
                const uint32_t bytes =
                    Synth::get_effect_state_bytes(phase == 3 ? Synth::EffectType::chorus : Synth::EffectType::delay);
                TEST(plan.consumed_bytes == (phase == 1 ? 0 : 4 * bytes));
                for (uint32_t slot = 0; slot < bank.master_chain.num_effects; ++slot) {
                    TEST(plan.slots[Synth::max_channels][slot].state_offs == fake_region_base + slot * bytes);
                    TEST(plan.slots[Synth::max_channels][slot].needs_clear);
                }
                Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
                const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
                TEST(clears.count == bank.master_chain.num_effects);
            }
            if (successful_transitions != 4 * (cycle + 1)) {
                break;
            }
        }
        TEST(successful_transitions == 400);
    }
    // Fragmented within-budget refusal preserves the caller, writer, live chains and pending clears.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        for (uint32_t i = 0; i < 14; ++i) {
            enabled_effect(bank, i / 4, i % 4, Synth::EffectType::reverb);
        }
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();
        for (uint32_t i = 0; i < 14; i += 2) {
            bank.channel_chains[i / 4].effects[i % 4].enabled = false;
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 7 * Synth::get_effect_state_bytes(Synth::EffectType::reverb));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        TEST(Synth::take_effect_clear_ranges().count == 0);
        bank.channel_chains[0].effects[0].type    = Synth::EffectType::delay;
        bank.channel_chains[0].effects[0].enabled = true;
        TEST(7 * Synth::get_effect_state_bytes(Synth::EffectType::reverb) +
                 Synth::get_effect_state_bytes(Synth::EffectType::delay) <
             Synth::effect_state_budget);
        const Synth::EffectExpansionPlan before                             = plan;
        const FakeWriter                 writer_before                      = writer;
        Synth::EffectChain               chains_before[Synth::max_channels] = {};
        memcpy(chains_before, chains, sizeof(chains));
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(error && strcmp(error, "effect state has no contiguous free range") == 0);
        TEST(memcmp(&plan, &before, sizeof(plan)) == 0);
        TEST(memcmp(&writer, &writer_before, sizeof(writer)) == 0);
        TEST(memcmp(chains, chains_before, sizeof(chains)) == 0);
        TEST(Synth::take_effect_clear_ranges().count == 0);
    }
    // Authoring slots stay stable; only complete topology is compiled for playback.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> restored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank&                       restored = *restored_storage;
        const std::unique_ptr<Synth::InstrumentEditorBank> authored_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& authored = *authored_storage;
        memset(static_cast<void*>(&authored), 0, sizeof(authored));
        make_valid_bank(authored.bank);
        Sculptor::set_identity_osc_sources(&authored, 0, 0);
        enabled_effect(authored.bank, 0, 0, Synth::EffectType::delay)->bindings[0].base_value  = 217.0f;
        enabled_effect(authored.bank, 0, 1, Synth::EffectType::chorus)->bindings[0].base_value = 3.25f;
        authored.bank.channel_chains[0].effects[0].bindings[0].lfo_desc_id                     = 1;
        authored.bank.channel_chains[0].effects[1].bindings[0].lfo_desc_id                     = 1;
        const std::unique_ptr<Synth::InstrumentBank> playback_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       playback         = *playback_storage;
        TEST(Sculptor::compile_editor_playback_bank(authored, &playback));
        TEST(playback.channel_chains[0].num_effects == 0); // zeroed routing: no path, compiles dry
        const Synth::EffectChainBinding    retained = authored.bank.channel_chains[0];
        const Sculptor::EffectChainRouting valid[]  = {
            { { Sculptor::effect_out_next,
                Sculptor::effect_out_output,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // I -> A -> B -> O
            { { Sculptor::effect_out_output,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // I -> A -> O, single effect
            { { Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // input wired to nothing: dry
            { { Sculptor::effect_out_next,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // incomplete prefix
            { { Sculptor::effect_out_next,
                Sculptor::effect_out_output,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              0 }, // detached fragment
            { { Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              0 }, // all detached
        };
        for (uint32_t example = 0; example < 6; ++example) {
            authored.effect_routing[0] = valid[example];
            TEST(Sculptor::validate_effect_routing(retained, valid[example]));
            memset(static_cast<void*>(&playback), 0, sizeof(playback));
            TEST(Sculptor::compile_editor_playback_bank(authored, &playback));
            {
                TEST(Synth::validate_instrument_bank(&playback));
                TEST(playback.channel_chains[0].num_effects == (example == 0 ? 2u : example == 1 ? 1u : 0u));
                if (example < 2) {
                    TEST(memcmp(&playback.channel_chains[0].effects[0],
                                &retained.effects[0],
                                sizeof(retained.effects[0])) == 0);
                }
                if (example == 0) {
                    TEST(memcmp(&playback.channel_chains[0].effects[1],
                                &retained.effects[1],
                                sizeof(retained.effects[0])) == 0);
                }
            }
            TEST(memcmp(&authored.bank.channel_chains[0], &retained, sizeof(retained)) == 0);
        }
        const Sculptor::EffectChainRouting invalid[] = {
            { { Sculptor::effect_out_output,
                Sculptor::effect_out_output,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // two output holders
            { { Sculptor::effect_out_disconnected,
                Sculptor::effect_out_next,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // next on the last occupied slot
            { { Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_output },
              1 }, // a flag on an empty slot
            { { 3,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected,
                Sculptor::effect_out_disconnected },
              1 }, // out-of-range flag value
        };
        for (const auto& routing : invalid) {
            authored.effect_routing[0] = routing;
            TEST(! Sculptor::validate_effect_routing(retained, routing));
            memset(static_cast<void*>(&playback), 0xA5, sizeof(playback));
            const std::unique_ptr<Synth::InstrumentBank> before_storage = std::make_unique<Synth::InstrumentBank>();
            Synth::InstrumentBank&                       before         = *before_storage;
            memcpy(&before, &playback, sizeof(before));
            TEST(! Sculptor::compile_editor_playback_bank(authored, &playback));
            TEST(memcmp(&playback, &before, sizeof(playback)) == 0);
        }
        authored.effect_routing[0]                           = valid[1];
        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(Sculptor::project_effect_chain_to_graph(authored, 0, &graph, &mapping));
        TEST(graph.node_occupied(mapping.effect_nodes[0]));
        TEST(graph.node_occupied(mapping.effect_nodes[1]));
        TEST(graph.slot_is_connected(mapping.effect_nodes[0], 0));   // A carries the input wire (I -> A -> O)
        TEST(graph.slot_is_connected(mapping.effect_nodes[0], 1));   // and feeds the output
        TEST(! graph.slot_is_connected(mapping.effect_nodes[1], 0)); // B stays parked
        Synth::EffectsDocument document = {};
        TEST(Sculptor::extract_effect_chain_document(&authored, 0, nullptr, nullptr, &document));
        TEST(memcmp(&document.routing, &valid[1], sizeof(document.routing)) == 0);
        TEST(document.chain.num_effects == 2);
        TEST(document.chain.effects[0].bindings[0].lfo_desc_id == document.chain.effects[1].bindings[0].lfo_desc_id);
        const std::unique_ptr<FixedArray<char, 65536>> text_storage = std::make_unique<FixedArray<char, 65536>>();
        char (&text)[65536]                                         = text_storage->elements;
        const uint32_t length = Synth::encode_effects_json(&document, text, sizeof(text));
        TEST(length != 0);
        TEST(strstr(text, "synth-effects-v3") != nullptr);
        Synth::EffectsDocument decoded = {};
        TEST(Synth::decode_effects_json(text, length, &decoded));
        TEST(memcmp(&decoded.routing, &document.routing, sizeof(document.routing)) == 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> pasted_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& pasted = *pasted_storage;
        TEST(Sculptor::replace_effect_chain_candidate(&authored, 1, &decoded, &pasted));
        TEST(memcmp(&pasted.effect_routing[1], &valid[1], sizeof(document.routing)) == 0);
        const std::unique_ptr<FixedArray<char, 1024 * 1024>> bank_text_storage =
            std::make_unique<FixedArray<char, 1024 * 1024>>();
        char (&bank_text)[1024 * 1024] = bank_text_storage->elements;
        const uint32_t bank_length     = Synth::encode_editor_bank_json(&authored, bank_text, sizeof(bank_text));
        TEST(bank_length != 0);
        TEST(strncmp(bank_text, "{\"instrument_editor_bank\":{", strlen("{\"instrument_editor_bank\":{")) == 0);
        TEST(strstr(bank_text, "\"effect_routing\"") != nullptr);
        TEST(Synth::decode_editor_bank_json(bank_text, bank_length, &restored));
        TEST(memcmp(restored.effect_routing, authored.effect_routing, sizeof(authored.effect_routing)) == 0);
        // Detached authoring topology rides the bank codec, effects transfer and reduced library owner.
        const char* library_path = "synth_disconnected_effects_library.tmp";
        remove(library_path);
        TEST(Synth::save_library_record(library_path, "Effects", "Detached", &authored, 0) == 0);
        Synth::LibraryEntry entry = {};
        TEST(Synth::read_library_index(library_path, &entry, 1) == 1);
        memset(static_cast<void*>(&restored), 0, sizeof(restored));
        Synth::init_default_bank(&restored.bank);
        Sculptor::set_identity_osc_sources(&restored, 0, 0);
        const Synth::EffectChainBinding other      = restored.bank.master_chain;
        uint16_t                        first_slot = 0;
        TEST(Synth::load_library_instrument(library_path, &entry, &restored, 1, &first_slot));
        TEST(memcmp(&restored.effect_routing[1], &valid[1], sizeof(valid[1])) == 0);
        TEST(restored.bank.channel_chains[1].num_effects == 2);
        TEST(restored.bank.channel_chains[1].effects[0].bindings[0].lfo_desc_id ==
             restored.bank.channel_chains[1].effects[1].bindings[0].lfo_desc_id);
        TEST(memcmp(&restored.bank.master_chain, &other, sizeof(other)) == 0);
        remove(library_path);
        // Explicit routing is never silently accepted in an old effects envelope.
        const char* invalid_documents[] = {
            "{\"format\":\"synth-effects-v1\",\"effects\":[],\"audio_next\":[0,0,0,0,5]}",
            "{\"format\":\"synth-effects-v2\",\"effects\":[]}",
            "{\"format\":\"synth-effects-v2\",\"effects\":[],\"audio_next\":[0,0,0,0,6]}",
            "{\"format\":\"synth-effects-v2\",\"effects\":[],\"audio_next\":[0,0,0,0,5],\"audio_next\":[0,0,0,0,5]}",
            "{\"format\":\"synth-effects-v3\",\"effects\":[]}",
        };
        for (const char* invalid_document : invalid_documents) {
            memset(static_cast<void*>(&decoded), 0xA5, sizeof(decoded));
            Synth::EffectsDocument unchanged;
            memcpy(&unchanged, &decoded, sizeof(decoded));
            TEST(! Synth::decode_effects_json(invalid_document,
                                              static_cast<uint32_t>(strlen(invalid_document)),
                                              &decoded));
            TEST(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
        }
        const char* envelopes[] = {
            "{\"instrument_editor_bank\":null,\"instrument_editor_bank_v2\":{}}",
            "{\"instrument_editor_bank_v2\":{},\"instrument_editor_bank\":null}",
        };
        for (const char* envelope : envelopes) {
            // A v2 payload without the required owner metadata must refuse, not default serial.
            memset(static_cast<void*>(&restored), 0xA5, sizeof(restored));
            const std::unique_ptr<Synth::InstrumentEditorBank> unchanged_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& unchanged = *unchanged_storage;
            memcpy(&unchanged, &restored, sizeof(restored));
            TEST(! Synth::decode_editor_bank_json(envelope, static_cast<uint32_t>(strlen(envelope)), &restored));
            TEST(memcmp(&restored, &unchanged, sizeof(restored)) == 0);
        }
        FILE* stream = tmpfile();
        TEST(stream != nullptr);
        if (stream) {
            uint32_t written = 0;
            TEST(Synth::write_editor_bank_json(stream, &authored, sizeof(bank_text), &written) == 0);
            rewind(stream);
            TEST(Synth::read_editor_bank_json(stream, written, &restored));
            TEST(memcmp(restored.effect_routing, authored.effect_routing, sizeof(authored.effect_routing)) == 0);
            fclose(stream);
        }
    }

    // The queued image is playable, never the retained authoring collection.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> source_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& source = *source_storage;
        memset(static_cast<void*>(&source), 0, sizeof(source));
        make_valid_bank(source.bank);
        enabled_effect(source.bank, 0, 0, Synth::EffectType::chorus)->bindings[0].base_value = 4.5f;
        enabled_effect(source.bank, 0, 1, Synth::EffectType::delay)->bindings[0].base_value  = 231.0f;
        source.effect_routing[0].in                                                          = 1;
        source.effect_routing[0].out[0] = Sculptor::effect_out_output; // I -> A -> O
        enabled_effect(source.bank, Synth::max_channels, 0, Synth::EffectType::reverb);
        source.effect_routing[Synth::max_channels].in                  = 1; // incomplete: input feeds nothing
        const std::unique_ptr<Synth::InstrumentBank> packet_storage    = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       packet            = *packet_storage;
        const std::unique_ptr<Synth::InstrumentBank> installed_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       installed         = *installed_storage;
        memset(static_cast<void*>(&packet), 0, sizeof(packet));
        installed           = source.bank;
        const bool compiled = Sculptor::compile_editor_playback_bank(source, &packet);
        TEST(compiled);
        if (compiled) {
            TEST(packet.channel_chains[0].num_effects == 1);
            TEST(packet.channel_chains[0].effects[0].type == Synth::EffectType::chorus);
            TEST(packet.master_chain.num_effects == 0);
            const std::unique_ptr<Synth::BankUpdateQueue> queue_storage = std::make_unique<Synth::BankUpdateQueue>();
            Synth::BankUpdateQueue&                       queue         = *queue_storage;
            queue.head.store(0);
            queue.tail.store(0);
            TEST(Synth::push_bank_update(&queue, packet));
            const Synth::InstrumentBank* queued = Synth::peek_bank_update(&queue);
            TEST(queued != nullptr);
            if (queued) {
                TEST(queued->channel_chains[0].effects[0].bindings[0].base_value == 4.5f);
                Synth::init_effect_state_region(fake_region_base);
                Synth::EffectExpansionPlan plan  = {};
                const char*                error = nullptr;
                TEST(Synth::preflight_effect_expansion(*queued, &plan, &error));
                Synth::EffectChain chains[Synth::max_channels] = {};
                Synth::EffectChain master                      = {};
                FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
                Synth::commit_effect_expansion(*queued, plan, chains, &master, fake_writer_binding(writer));
                installed = *queued;
                Synth::consume_bank_update(&queue);
                TEST(chains[0].num_effects == 1);
                TEST(chains[0].effects[0].params[0] == 4.5f);
                TEST(installed.channel_chains[0].num_effects == 1);
                TEST(source.bank.channel_chains[0].num_effects == 2);
            }
        }
        // A dormant gap slot makes the routing invalid: every decode path
        // compacts gaps away, so compile refuses the hand-authored shape
        // without touching the destination.
        source.effect_routing[0]                                     = {};
        source.effect_routing[Synth::max_channels]                   = {};
        source.bank.channel_chains[0].effects[0]                     = {};
        const std::unique_ptr<Synth::InstrumentBank> refused_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       refused         = *refused_storage;
        memset(static_cast<void*>(&packet), 0xA5, sizeof(packet));
        memcpy(&refused, &packet, sizeof(refused));
        const bool legacy = Sculptor::compile_editor_playback_bank(source, &packet);
        TEST(! legacy);
        TEST(memcmp(&packet, &refused, sizeof(packet)) == 0); // unchanged destination on refusal
    }
    // True exhaustion leaves prior live chains and still-pending initialization untouched.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        for (uint32_t slot = 0; slot < 4; ++slot) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan                        = {};
        const char*                error                       = nullptr;
        Synth::EffectChain         chains[Synth::max_channels] = {};
        Synth::EffectChain         master                      = {};
        FakeWriter                 writer                      = { 1, 0, 0, 0, 0 };
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        const Synth::EffectExpansionPlan before        = plan;
        const Synth::EffectChain         master_before = master;
        enabled_effect(bank, 0, 0, Synth::EffectType::delay);
        TEST(! Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(error && strcmp(error, "effect state budget exceeded") == 0);
        TEST(memcmp(&plan, &before, sizeof(plan)) == 0);
        TEST(memcmp(&master, &master_before, sizeof(master)) == 0);
        const Synth::EffectClearList clears = Synth::take_effect_clear_ranges();
        TEST(clears.count == 4);
        for (uint32_t slot = 0; slot < 4 && slot < clears.count; ++slot) {
            TEST(clears.ranges[slot].offset ==
                 fake_region_base + slot * Synth::get_effect_state_bytes(Synth::EffectType::delay));
        }
    }

    // Queue mechanics: room condition, wraparound, ordering, two-phase consume.
    // Packets are whole banks; instruments.entries[0].layer_count marks each one.
    {
        const std::unique_ptr<Synth::BankUpdateQueue> queue_storage = std::make_unique<Synth::BankUpdateQueue>();
        Synth::BankUpdateQueue&                       queue         = *queue_storage;
        memset(static_cast<void*>(&queue), 0, sizeof(queue));

        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        bank.instruments.allocate();

        // Fill the queue (capacity 2).
        for (uint32_t k = 0; k < Synth::bank_queue_capacity; k++) {
            bank.instruments.entries[0].layer_count = k + 1;
            set_default_dispersion_sections(bank.instruments.entries[0]);
            TEST(Synth::push_bank_update(&queue, bank));
        }
        // Third push fails at tail=2, head=0.
        bank.instruments.entries[0].layer_count = 99;
        set_default_dispersion_sections(bank.instruments.entries[0]);
        TEST(! Synth::push_bank_update(&queue, bank));

        // Consume one: the third push now succeeds (wraparound: slot 0 reused).
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 1);
        Synth::consume_bank_update(&queue);
        TEST(Synth::push_bank_update(&queue, bank));

        // Drain in order: 2, then 99.
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 2);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue)->instruments.entries[0].layer_count == 99);
        Synth::consume_bank_update(&queue);
        TEST(Synth::peek_bank_update(&queue) == nullptr);
    }
    // get_zone_name: named instrument and the "Zone X" fallback.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank);
        memcpy(bank.instrument_names[0], "Lead", 5);
        char name[Synth::max_name_len];
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Lead") == 0);

        bank.instrument_names[0][0] = 0;
        Synth::get_zone_name(&bank, 0, 0, name, sizeof(name));
        TEST(strcmp(name, "Zone 0") == 0);

        bank.bank.channel_zones[0][1] = { 50, 0 };
        Synth::get_zone_name(&bank, 0, 1, name, sizeof(name));
        TEST(strcmp(name, "Zone 1") == 0);
    }

    // Capture semantics: a configured generator node holds its descriptor BY VALUE; mutating
    // (or replacing) the bank afterwards cannot change the node's sound.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);

        Synth::ParamDescriptor env_node              = {};
        env_node.kind                                = Synth::ParamKind::envelope;
        env_node.envelope                            = bank.envelopes.entries[0];
        bank.envelopes.entries[0].num_points         = 7; // edit the bank entry
        bank.envelopes.entries[0].points[1].position = 12345;
        TEST(env_node.envelope.num_points == 2); // node kept its own copy
        TEST(env_node.envelope.points[1].position == 100);

        Synth::ParamDescriptor lfo_node = {};
        Synth::configure_lfo(&lfo_node, bank.lfos.entries[0], Synth::SourceOp::add, 0.5f, 0, 0, 0.0f);
        bank.lfos.entries[0].period_ms = 9999;
        TEST(lfo_node.lfo.lfo.period_ms == 50); // node kept its own copy
    }

    // Full-bank publish flow: two complete banks queued (coalesced upstream), drained in
    // order; the runtime bank ends at the newest and the queue is empty.
    {
        const std::unique_ptr<FixedArray<Synth::InstrumentBank, 2>> banks_storage =
            std::make_unique<FixedArray<Synth::InstrumentBank, 2>>();
        Synth::InstrumentBank(&banks)[2]                              = banks_storage->elements;
        const std::unique_ptr<Synth::InstrumentBank>  runtime_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                        runtime         = *runtime_storage;
        const std::unique_ptr<Synth::BankUpdateQueue> queue_storage   = std::make_unique<Synth::BankUpdateQueue>();
        Synth::BankUpdateQueue&                       queue           = *queue_storage;
        memset(static_cast<void*>(&queue), 0, sizeof(queue));
        make_valid_bank(banks[0]);
        make_valid_bank(banks[1]);
        banks[0].instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value = 0.25f;
        banks[1].instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value = 0.75f;

        TEST(Synth::validate_instrument_bank(&banks[0]));
        TEST(Synth::validate_instrument_bank(&banks[1]));
        TEST(Synth::push_bank_update(&queue, banks[0]));
        TEST(Synth::push_bank_update(&queue, banks[1]));

        // Audio-side drain: copy each peeked bank, then consume (as the editor app's audio-step hook does).
        while (const Synth::InstrumentBank* packet = Synth::peek_bank_update(&queue)) {
            runtime = *packet;
            Synth::consume_bank_update(&queue);
        }
        TEST(approx(runtime.instruments.entries[0].layers[0].targets[Synth::mod_volume].base_value, 0.75f, 0.001f));
        TEST(runtime.instruments.num_allocated == 1);
    }
}

static void check_effects_inline_1()
{

    // Capacity: the reshaped projection fits the decided slot budget.
    {
        TEST(Sculptor::max_node_slots >= 53); // 5-param effects carrying the full modulation grammar
    }
}

static void check_effects_inline_2()
{

    // A widget-style edit sequence on a real bank stays valid and survives
    // the JSON round trip byte for byte.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> bank_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& bank = *bank_storage;
        make_valid_bank(bank.bank); // instrument 0 (env 1, lfo 1) on ch0
        Sculptor::set_identity_osc_sources(&bank, 0, 0);

        Synth::EnvelopeDescriptor env = bank.bank.envelopes.entries[0];
        TEST(Sculptor::env_insert_point(env, 1, true, &env));
        TEST(Sculptor::env_move_point(env, 2, 250, 0xC000, &env));
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(Sculptor::env_set_sustain_last(env, 1, &env));
        // Delete down to a single point, then grow again from it.
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(env.num_points == 1);
        TEST(Sculptor::env_insert_point(env, 0, true, &env));
        TEST(env.num_points == 2);
        // Deleting the nonzero final point leaves a valid single-point
        // descriptor.
        TEST(Sculptor::env_remove_point(env, 1, &env));
        TEST(env.num_points == 1 && env.points[0].position == 0);
        bank.bank.envelopes.entries[0] = env;
        TEST(Synth::validate_instrument_bank(&bank.bank));

        const std::unique_ptr<FixedArray<char, 128 * 1024>> doc_storage =
            std::make_unique<FixedArray<char, 128 * 1024>>();
        char (&doc)[128 * 1024] = doc_storage->elements;
        const uint32_t len      = Synth::encode_editor_bank_json(&bank, doc, sizeof(doc));
        TEST(len > 0);
        const std::unique_ptr<Synth::InstrumentEditorBank> decoded_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& decoded = *decoded_storage;
        TEST(Synth::decode_editor_bank_json(doc, len, &decoded));
        TEST(decoded.bank.envelopes.num_allocated == bank.bank.envelopes.num_allocated);
        TEST(memcmp(&decoded.bank.envelopes.entries[0], &bank.bank.envelopes.entries[0], sizeof(env)) == 0);
        const std::unique_ptr<FixedArray<char, 128 * 1024>> doc2_storage =
            std::make_unique<FixedArray<char, 128 * 1024>>();
        char (&doc2)[128 * 1024] = doc2_storage->elements;
        const uint32_t len2      = Synth::encode_editor_bank_json(&decoded, doc2, sizeof(doc2));
        TEST(len2 == len && memcmp(doc, doc2, len) == 0);
    }

    // ---- Effects graph projection and edit paths (sculptor_effect_graph) ----

    // Projection mirrors one channel chain; value edits, structural deletes,
    // move/retype and fresh-LFO semantics all hold on a validated bank.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain = bank.channel_chains[0];
        chain.num_effects                = 2;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        chain.effects[0].enabled                 = true;
        chain.effects[0].bindings[1].lfo_desc_id = 1;
        chain.effects[0].bindings[1].lfo_depth   = 0.5f;
        // The same descriptor referenced from another chain: shared content.
        Synth::EffectChainBinding& other = bank.channel_chains[1];
        other.num_effects                = 1;
        Sculptor::fx_init_slot(&other, 0, Synth::EffectType::chorus);
        other.effects[0].bindings[0].lfo_desc_id = 1;
        other.effects[0].bindings[0].lfo_depth   = 0.5f;
        // A MIDI source rides the Mix row (channel chains only).
        chain.effects[0].bindings[1].num_inputs       = 1;
        chain.effects[0].bindings[1].inputs[0].source = Synth::ModSource::mod_wheel;
        chain.effects[0].bindings[1].inputs[0].op     = Synth::SourceOp::multiply;
        chain.effects[0].bindings[1].inputs[0].scale  = 0.5f;
        TEST(Synth::validate_instrument_bank(&bank));

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(project_legacy_effects(bank, 0, &graph, &mapping));
        TEST(mapping.chain == 0);
        TEST(mapping.effect_nodes[0] != Sculptor::pool_no_slot);
        TEST(mapping.effect_nodes[1] != Sculptor::pool_no_slot);
        TEST(mapping.effect_nodes[2] == Sculptor::pool_no_slot);
        TEST(mapping.lfo_count == 1 && mapping.lfo_nodes[0] != Sculptor::pool_no_slot);
        TEST(mapping.lfo_nodes[1] == Sculptor::pool_no_slot);
        TEST(graph.node(mapping.effect_nodes[0]).slots.num_allocated == 3 + Sculptor::fx_param_stride * 3);
        TEST(graph.node(mapping.lfo_nodes[0]).slots.num_allocated == 7);
        TEST(mapping.midi_node != Sculptor::pool_no_slot); // channel chain: MIDI routes in
        TEST(graph.node(mapping.midi_node).slots.num_allocated == Sculptor::fx_num_input_sources);

        // Serial wires plus one read-only reference wire onto the Mix row.
        TEST(find_fx_graph_connection(graph, { mapping.input_node, 0 }, { mapping.effect_nodes[0], 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[0], 1 }, { mapping.effect_nodes[1], 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph, { mapping.effect_nodes[1], 1 }, { mapping.output_node, 0 }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph,
                                      { mapping.lfo_nodes[0], 0 },
                                      { mapping.effect_nodes[0], Sculptor::fx_param_lfo_dot(1) }) !=
             Sculptor::pool_no_slot);
        TEST(find_fx_graph_connection(graph,
                                      { mapping.midi_node, mapping.midi_source_slots[1] },
                                      { mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) }) !=
             Sculptor::pool_no_slot);

        // Projected values mirror the chain and the descriptor.
        TEST(graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_enabled_row].value.toggle == true);
        TEST(graph.node(mapping.effect_nodes[0]).slots.entries[Sculptor::fx_param_row(0)].value.real ==
             chain.effects[0].bindings[0].base_value);
        TEST(graph.node(mapping.lfo_nodes[0]).slots.entries[3].value.integer == bank.lfos.entries[0].period_ms);

        // Unwired modulation knobs stay inert; the wired rows' knobs are live.
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(0) + 2));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(0) + 3));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 2));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_row(1) + 3));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 1));
        TEST(graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(0, 0) + 2));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) + 1));
        TEST(! graph.slot_edit_disabled(mapping.effect_nodes[0], Sculptor::fx_param_src_dot(1, 0) + 2));

        Sculptor::GraphChange change = {};
        change.kind                  = Sculptor::ChangeKind::value_changed;

        // Enabled row: the checkbox toggles the saved state.
        change.node_idx                = mapping.effect_nodes[0];
        change.slot_idx                = Sculptor::fx_enabled_row;
        Sculptor::PropertyValue toggle = {};
        toggle.toggle                  = false;
        graph.set_slot_value(mapping.effect_nodes[0], Sculptor::fx_enabled_row, toggle);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(! chain.effects[0].enabled);
        toggle.toggle = true;
        graph.set_slot_value(mapping.effect_nodes[0], Sculptor::fx_enabled_row, toggle);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].enabled);

        // Parameter row: the live slot value becomes the base value.
        change.slot_idx             = Sculptor::fx_param_row(1);
        Sculptor::PropertyValue mix = {};
        mix.real                    = 0.75f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, mix);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].base_value == 0.75f);

        // The Op and Depth rows ride the modulation line under the base slider.
        change.slot_idx            = Sculptor::fx_param_row(0) + 2;
        Sculptor::PropertyValue op = {};
        op.list_index              = 1;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, op);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_op == Synth::SourceOp::multiply);
        op.list_index = 0;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, op);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_op == Synth::SourceOp::add);

        change.slot_idx               = Sculptor::fx_param_row(0) + 3;
        Sculptor::PropertyValue depth = {};
        depth.real                    = 1.25f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, depth);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[0].lfo_depth == 1.25f);

        // A MIDI source row edit compiles the row back into the binding's
        // packed input list.
        change.slot_idx                = Sculptor::fx_param_src_dot(1, 0) + 2;
        Sculptor::PropertyValue amount = {};
        amount.real                    = 0.25f;
        graph.set_slot_value(mapping.effect_nodes[0], change.slot_idx, amount);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].num_inputs == 1);
        TEST(chain.effects[0].bindings[1].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(chain.effects[0].bindings[1].inputs[0].op == Synth::SourceOp::multiply);
        TEST(chain.effects[0].bindings[1].inputs[0].scale == 0.25f);

        // LFO descriptor edits reach every sharing chain at once.
        change.node_idx              = mapping.lfo_nodes[0];
        change.slot_idx              = 1;
        Sculptor::PropertyValue wave = {};
        wave.list_index              = 1;
        graph.set_slot_value(mapping.lfo_nodes[0], 1, wave);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].wave == Synth::WaveType::sawtooth_wave);
        wave.list_index = 0;
        graph.set_slot_value(mapping.lfo_nodes[0], 1, wave);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].wave == Synth::WaveType::sine_wave);

        change.slot_idx              = 2;
        Sculptor::PropertyValue duty = {};
        duty.real                    = 0.25f;
        graph.set_slot_value(mapping.lfo_nodes[0], 2, duty);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].duty == 64);

        change.slot_idx                = 3;
        Sculptor::PropertyValue period = {};
        period.integer                 = 500;
        graph.set_slot_value(mapping.lfo_nodes[0], 3, period);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].period_ms == 500);
        // A zero period would freeze the LFO: rejected, value kept.
        period.integer = 0;
        graph.set_slot_value(mapping.lfo_nodes[0], 3, period);
        TEST(! Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].period_ms == 500);

        // Slots 4/5/6 are Pulse/Sync/Start: pinned here so a future slot
        // insertion cannot silently shift the fx apply dispatch indices.
        change.slot_idx               = 4;
        Sculptor::PropertyValue pulse = {};
        pulse.real                    = 1.0f;
        graph.set_slot_value(mapping.lfo_nodes[0], 4, pulse);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].pulse_mix == 255);
        change.slot_idx              = 5;
        Sculptor::PropertyValue sync = {};
        sync.list_index              = 2;
        graph.set_slot_value(mapping.lfo_nodes[0], 5, sync);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].beat_div == 2);
        change.slot_idx               = 6;
        Sculptor::PropertyValue start = {};
        start.list_index              = 2;
        graph.set_slot_value(mapping.lfo_nodes[0], 6, start);
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(bank.lfos.entries[0].start_point == 2);

        // Deleting the LFO node clears only this chain's references.
        change.kind = Sculptor::ChangeKind::node_deleted;
        TEST(Sculptor::apply_fx_graph_change(&bank, graph, mapping, change));
        TEST(chain.effects[0].bindings[1].lfo_desc_id == 0);
        TEST(other.effects[0].bindings[0].lfo_desc_id == 1);

        // Deleting an effect node removes the slot, shifts later slots down, and
        // heals the routing flags; structural deletes ride the batch seam.
        {
            const std::unique_ptr<Synth::InstrumentEditorBank> del_editor_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& del_editor = *del_editor_storage;
            memset(static_cast<void*>(&del_editor), 0, sizeof(del_editor));
            del_editor.bank = bank;
            implicit_serial_routing(del_editor, 0);
            const std::unique_ptr<Sculptor::Graph> del_graph_storage = std::make_unique<Sculptor::Graph>();
            Sculptor::Graph&                       del_graph         = *del_graph_storage;
            Sculptor::EffectGraphMapping           del_mapping       = {};
            TEST(Sculptor::project_effect_chain_to_graph(del_editor, 0, &del_graph, &del_mapping));
            Sculptor::GraphChange del_changes[Sculptor::max_pending_changes];
            (void)del_graph.take_changes(del_changes, Sculptor::max_pending_changes); // re-arm the ring
            del_graph.delete_node(del_mapping.effect_nodes[1]);
            const uint32_t del_count = del_graph.take_changes(del_changes, Sculptor::max_pending_changes);
            const std::unique_ptr<Synth::InstrumentEditorBank> del_candidate_storage =
                std::make_unique<Synth::InstrumentEditorBank>();
            Synth::InstrumentEditorBank& del_candidate = *del_candidate_storage;
            TEST(Sculptor::apply_effect_graph_batch(&del_editor,
                                                    &del_graph,
                                                    &del_mapping,
                                                    del_changes,
                                                    del_count,
                                                    nullptr,
                                                    &del_candidate));
            bank.channel_chains[0] = del_candidate.bank.channel_chains[0];
            TEST(chain.num_effects == 1);
            TEST(chain.effects[0].type == Synth::EffectType::distortion);
            TEST(chain.effects[1].type == Synth::EffectType::none);
            TEST(chain.effects[1].bindings[0].lfo_desc_id == 0);
        }

        // Retype: neutral defaults, no dormant modulation of the old type.
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::chorus);
        TEST(chain.effects[0].type == Synth::EffectType::chorus);
        TEST(chain.effects[0].bindings[0].base_value == Sculptor::effect_param_default(Synth::EffectType::chorus, 0));
        TEST(chain.effects[0].bindings[0].lfo_desc_id == 0);

        // New LFO: a fresh descriptor, unbound until a wire lands on a row.
        uint16_t desc_id = 0;
        TEST(Sculptor::allocate_default_lfo(&bank, &desc_id));
        TEST(desc_id == 2);
        TEST(bank.lfos.num_allocated == 2);
        TEST(chain.effects[0].bindings[0].lfo_desc_id == 0);
        TEST(bank.lfos.entries[1].wave == Synth::WaveType::sine_wave);
        TEST(bank.lfos.entries[1].period_ms == 250);

        // The modulation count spans every chain, disabled effects included.
        uint32_t num_modulated = 0;
        uint32_t state_bytes   = 0;
        Sculptor::count_effect_budgets(bank, &num_modulated, &state_bytes);
        TEST(num_modulated == 1); // the other chain's chorus; the fresh LFO is unwired
        TEST(state_bytes == 0);   // fx_init_slot leaves effects disabled

        // Full descriptor pool: refusal leaves the bank unmodified.
        while (bank.lfos.num_allocated < Synth::max_lfos) {
            bank.lfos.allocate();
        }
        uint16_t refused = 0;
        TEST(! Sculptor::allocate_default_lfo(&bank, &refused));
        TEST(refused == 0);
    }

    // Deleting a node through the widget API drops its projected wires
    // first, each drop reporting a connection event with a dead endpoint; the
    // apply path no-ops those byproducts (the freed node carried the wire) and
    // the node event alone removes the slot.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::distortion);
        enabled_effect(bank, 0, 1, Synth::EffectType::delay);
        TEST(Synth::validate_instrument_bank(&bank));

        const std::unique_ptr<Sculptor::Graph>             widget_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                                   widget_graph         = *widget_graph_storage;
        Sculptor::EffectGraphMapping                       widget_mapping       = {};
        const std::unique_ptr<Synth::InstrumentEditorBank> widget_editor_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& widget_editor = *widget_editor_storage;
        memset(static_cast<void*>(&widget_editor), 0, sizeof(widget_editor));
        widget_editor.bank = bank;
        implicit_serial_routing(widget_editor, 0);
        TEST(Sculptor::project_effect_chain_to_graph(widget_editor, 0, &widget_graph, &widget_mapping));
        // Projection quiets event reporting until the first drain re-arms
        // the ring; drain once first, as the per-frame GUI flow does.
        Sculptor::GraphChange widget_changes[Sculptor::max_pending_changes];
        (void)widget_graph.take_changes(widget_changes, Sculptor::max_pending_changes);
        widget_graph.delete_node(widget_mapping.effect_nodes[1]);

        const uint32_t widget_count     = widget_graph.take_changes(widget_changes, Sculptor::max_pending_changes);
        bool           seen_node_delete = false;
        bool           seen_wire_drop   = false;
        for (uint32_t i = 0; i < widget_count; ++i) {
            seen_node_delete = seen_node_delete || widget_changes[i].kind == Sculptor::ChangeKind::node_deleted;
            seen_wire_drop   = seen_wire_drop || widget_changes[i].kind == Sculptor::ChangeKind::connection_deleted;
        }
        TEST(seen_node_delete);
        TEST(seen_wire_drop);

        // The batch seam drops the wire byproducts (freed endpoints) and lets
        // the node event alone remove the slot and heal the flags.
        const std::unique_ptr<Synth::InstrumentEditorBank> widget_candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& widget_candidate = *widget_candidate_storage;
        TEST(Sculptor::apply_effect_graph_batch(&widget_editor,
                                                &widget_graph,
                                                &widget_mapping,
                                                widget_changes,
                                                widget_count,
                                                nullptr,
                                                &widget_candidate));
        bank = widget_candidate.bank;
        TEST(bank.channel_chains[0].num_effects == 1);
        TEST(bank.channel_chains[0].effects[0].type == Synth::EffectType::distortion);
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Wire gestures: an LFO Value wire onto a parameter dot binds the
    // descriptor, retargeting moves the binding, dragging a serial wire
    // reorders the chain, and a serial-wire disconnect clears the earlier
    // endpoint's flag while the slot order stays untouched.
    {
        const std::unique_ptr<Synth::InstrumentEditorBank> wire_editor_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& wire_editor = *wire_editor_storage;
        memset(static_cast<void*>(&wire_editor), 0, sizeof(wire_editor));
        Synth::InstrumentBank& bank = wire_editor.bank;
        make_valid_bank(bank);
        enabled_effect(bank, 0, 0, Synth::EffectType::chorus);
        enabled_effect(bank, 0, 1, Synth::EffectType::distortion);
        enabled_effect(bank, 0, 2, Synth::EffectType::delay);
        TEST(Synth::validate_instrument_bank(&bank));
        implicit_serial_routing(wire_editor, 0);

        // The projection shows descriptor 1 only through the pin: nothing
        // references it until the first wire lands.
        bool pinned[Synth::max_lfos] = {};
        pinned[0]                    = true;

        const std::unique_ptr<Sculptor::Graph> wire_graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       wire_graph         = *wire_graph_storage;
        Sculptor::EffectGraphMapping           wire_mapping       = {};
        TEST(Sculptor::project_effect_chain_to_graph(wire_editor, 0, &wire_graph, &wire_mapping, pinned));
        TEST(wire_mapping.lfo_count == 1);
        Sculptor::GraphChange                              wire_changes[Sculptor::max_pending_changes];
        const std::unique_ptr<Synth::InstrumentEditorBank> wire_candidate_storage =
            std::make_unique<Synth::InstrumentEditorBank>();
        Synth::InstrumentEditorBank& wire_candidate = *wire_candidate_storage;
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes); // re-arm the ring

        Synth::EffectChainBinding& wire_chain = bank.channel_chains[0];
        const uint32_t             chorus     = wire_mapping.effect_nodes[0];

        // Bind: LFO Value -> chorus rate row.
        TEST(
            wire_graph.attempt_connection({ wire_mapping.lfo_nodes[0], 0 }, { chorus, Sculptor::fx_param_lfo_dot(0) }));
        const uint32_t bind_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(bind_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_added);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].lfo_desc_id == 1);
        TEST(wire_chain.effects[0].bindings[0].lfo_depth == 0.5f); // depth-0 fresh binding reads as broken
        TEST(Synth::validate_instrument_bank(&bank));

        // Retarget the wire to the depth row: the old binding clears.
        const uint32_t bind_wire = find_fx_graph_connection(wire_graph,
                                                            { wire_mapping.lfo_nodes[0], 0 },
                                                            { chorus, Sculptor::fx_param_lfo_dot(0) });
        TEST(bind_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(bind_wire, false, { chorus, Sculptor::fx_param_lfo_dot(1) }));
        const uint32_t retarget_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(retarget_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].lfo_desc_id == 0);
        TEST(wire_chain.effects[0].bindings[1].lfo_desc_id == 1);

        // Unbind: pulling the wire off the row clears the binding.
        wire_graph.delete_connection(bind_wire);
        const uint32_t unbind_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(unbind_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[1].lfo_desc_id == 0);

        // MIDI route: the routing node's mod-wheel dot onto the chorus rate
        // Source A row packs the row into the binding's input list.
        TEST(wire_graph.attempt_connection({ wire_mapping.midi_node, wire_mapping.midi_source_slots[1] },
                                           { chorus, Sculptor::fx_param_src_dot(0, 0) }));
        const uint32_t midi_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(midi_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_added);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::mod_wheel);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].op == Synth::SourceOp::add);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].scale == 1.0f);
        TEST(Synth::validate_instrument_bank(&bank));

        // A second source on Source B keeps both inputs, packed in row order.
        TEST(wire_graph.attempt_connection({ wire_mapping.midi_node, wire_mapping.midi_source_slots[0] },
                                           { chorus, Sculptor::fx_param_src_dot(0, 1) }));
        const uint32_t midi2_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(midi2_count == 1);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 2);
        TEST(wire_chain.effects[0].bindings[0].inputs[1].source == Synth::ModSource::pitch_bend);
        TEST(Synth::validate_instrument_bank(&bank));

        // Unroute: pulling a source wire drops that row's input; the
        // remaining input compacts to the front of the list.
        const uint32_t midi_wire =
            find_fx_graph_connection(wire_graph,
                                     { wire_mapping.midi_node, wire_mapping.midi_source_slots[1] },
                                     { chorus, Sculptor::fx_param_src_dot(0, 0) });
        TEST(midi_wire != Sculptor::pool_no_slot);
        wire_graph.delete_connection(midi_wire);
        const uint32_t unroute_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(unroute_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_deleted);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[0].inputs[0].source == Synth::ModSource::pitch_bend);

        // Cross-parameter retarget: pulling the Source B wire off param 0 and
        // dropping it on param 1's Source A moves the routing.  The row the wire
        // left must forget the source, or the move would become a copy.
        const uint32_t pitch_bend_wire =
            find_fx_graph_connection(wire_graph,
                                     { wire_mapping.midi_node, wire_mapping.midi_source_slots[0] },
                                     { chorus, Sculptor::fx_param_src_dot(0, 1) });
        TEST(pitch_bend_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(pitch_bend_wire, false, { chorus, Sculptor::fx_param_src_dot(1, 0) }));
        const uint32_t reroute_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(reroute_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_fx_graph_change(&bank, wire_graph, wire_mapping, wire_changes[0]));
        TEST(wire_chain.effects[0].bindings[0].num_inputs == 0);
        TEST(wire_chain.effects[0].bindings[1].num_inputs == 1);
        TEST(wire_chain.effects[0].bindings[1].inputs[0].source == Synth::ModSource::pitch_bend);
        TEST(Synth::validate_instrument_bank(&bank));

        // Reorder: retarget the distortion Out end of the delay's feed wire
        // onto the chorus Out dot, so the delay follows the chorus directly.
        const uint32_t delay_in_wire = find_fx_graph_connection(wire_graph,
                                                                { wire_mapping.effect_nodes[1], 1 },
                                                                { wire_mapping.effect_nodes[2], 0 });
        TEST(delay_in_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(delay_in_wire, true, { wire_mapping.effect_nodes[0], 1 }));
        const uint32_t reorder_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(reorder_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_effect_graph_batch(&wire_editor,
                                                &wire_graph,
                                                &wire_mapping,
                                                &wire_changes[0],
                                                1,
                                                nullptr,
                                                &wire_candidate));
        wire_editor = wire_candidate; // structural commits land through the batch seam
        TEST(wire_chain.effects[0].type == Synth::EffectType::chorus);
        TEST(wire_chain.effects[1].type == Synth::EffectType::delay);
        TEST(wire_chain.effects[2].type == Synth::EffectType::distortion);
        TEST(Synth::validate_instrument_bank(&bank));

        // The editor re-projects after every structural commit: the mapping
        // is the projection's slot-to-node table, so a splice invalidates
        // it.  Re-project before the next gesture, exactly like the drain.
        TEST(Sculptor::project_effect_chain_to_graph(wire_editor, 0, &wire_graph, &wire_mapping, pinned));
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);

        // To the front: retarget the input wire's chorus end onto the parked
        // distortion's In dot; the distortion run moves to the front.
        // The chorus end is the wire's input end, so the drag moves it the way
        // the canvas retarget does (render seam: retarget_output_end = false).
        const uint32_t input_chorus_wire =
            find_fx_graph_connection(wire_graph, { wire_mapping.input_node, 0 }, { wire_mapping.effect_nodes[0], 0 });
        TEST(input_chorus_wire != Sculptor::pool_no_slot);
        TEST(wire_graph.move_connection_end(input_chorus_wire, false, { wire_mapping.effect_nodes[2], 0 }));
        const uint32_t front_count = wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(front_count == 1 && wire_changes[0].kind == Sculptor::ChangeKind::connection_changed);
        TEST(Sculptor::apply_effect_graph_batch(&wire_editor,
                                                &wire_graph,
                                                &wire_mapping,
                                                &wire_changes[0],
                                                1,
                                                nullptr,
                                                &wire_candidate));
        wire_editor = wire_candidate;
        TEST(wire_chain.effects[0].type == Synth::EffectType::distortion);

        TEST(Sculptor::project_effect_chain_to_graph(wire_editor, 0, &wire_graph, &wire_mapping, pinned));
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);

        // Disconnect: deleting a serial audio wire clears the earlier
        // endpoint's flag and keeps the slot order untouched. The bank order
        // is distortion, chorus, delay; the chorus parks between the input-fed
        // distortion and the delay that still feeds the output.
        const uint32_t serial_wire = find_fx_graph_connection(wire_graph,
                                                              { wire_mapping.effect_nodes[1], 1 },
                                                              { wire_mapping.effect_nodes[2], 0 });
        TEST(serial_wire != Sculptor::pool_no_slot);
        wire_graph.delete_connection(serial_wire);
        (void)wire_graph.take_changes(wire_changes, Sculptor::max_pending_changes);
        TEST(Sculptor::apply_effect_graph_batch(&wire_editor,
                                                &wire_graph,
                                                &wire_mapping,
                                                &wire_changes[0],
                                                1,
                                                nullptr,
                                                &wire_candidate));
        wire_editor = wire_candidate;
        TEST(wire_chain.num_effects == 3);
        TEST(wire_chain.effects[0].type == Synth::EffectType::distortion);
        TEST(wire_chain.effects[1].type == Synth::EffectType::chorus);
        TEST(wire_chain.effects[2].type == Synth::EffectType::delay);
        TEST(wire_editor.effect_routing[0].out[1] == Sculptor::effect_out_disconnected); // the cleared flag
        TEST(wire_editor.effect_routing[0].out[2] == Sculptor::effect_out_output); // the delay still feeds the output
        TEST(wire_editor.effect_routing[0].in ==
             Sculptor::effect_in_first_slot); // the input still feeds the distortion
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Master chain: LFO modulation and channel-wide MIDI sources are legal;
    // they resolve from channel index 0.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectChainBinding& master = bank.master_chain;
        master.num_effects                = 1;
        Sculptor::fx_init_slot(&master, 0, Synth::EffectType::reverb);
        master.effects[0].enabled                 = true;
        master.effects[0].bindings[0].lfo_desc_id = 1;
        master.effects[0].bindings[0].lfo_depth   = 0.25f;
        TEST(Synth::validate_instrument_bank(&bank));

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(project_legacy_effects(bank, Sculptor::fx_master_chain, &graph, &mapping));
        TEST(mapping.chain == Sculptor::fx_master_chain);
        TEST(mapping.lfo_count == 1);
        TEST(mapping.midi_node != Sculptor::pool_no_slot);

        master.effects[0].bindings[0].lfo_depth_source = Synth::ModSource::pitch_bend;
        TEST(Synth::validate_instrument_bank(&bank));
        master.effects[0].bindings[0].num_inputs       = 1;
        master.effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        TEST(Synth::validate_instrument_bank(&bank));

        // The same direct input is channel-wide state on a channel chain.

        Synth::EffectChainBinding& channel = bank.channel_chains[0];
        channel.num_effects                = 1;
        Sculptor::fx_init_slot(&channel, 0, Synth::EffectType::reverb);
        channel.effects[0].enabled                      = true;
        channel.effects[0].bindings[0].num_inputs       = 1;
        channel.effects[0].bindings[0].inputs[0].source = Synth::ModSource::mod_wheel;
        TEST(Synth::validate_instrument_bank(&bank));
    }

    // Widest projection: 4 effects, every parameter modulated by a distinct
    // descriptor; 21 nodes stay far under the widget's pools.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        Synth::EffectChainBinding& chain = bank.channel_chains[0];
        chain.num_effects                = 4;
        Sculptor::fx_init_slot(&chain, 0, Synth::EffectType::distortion);
        Sculptor::fx_init_slot(&chain, 1, Synth::EffectType::delay);
        Sculptor::fx_init_slot(&chain, 2, Synth::EffectType::compressor);
        Sculptor::fx_init_slot(&chain, 3, Synth::EffectType::fir);
        uint16_t       next_desc = 1;
        const uint32_t max_desc  = 3 + 3 + 5 + 3;
        while (bank.lfos.num_allocated < max_desc) {
            const uint32_t slot               = bank.lfos.allocate();
            bank.lfos.entries[slot].wave      = Synth::WaveType::sine_wave;
            bank.lfos.entries[slot].period_ms = 250;
        }
        for (uint32_t slot = 0; slot < 4; ++slot) {
            const uint32_t num_params = Synth::get_effect_param_floats(chain.effects[slot].type);
            for (uint32_t param = 0; param < num_params; ++param) {
                chain.effects[slot].bindings[param].lfo_desc_id = next_desc++;
                chain.effects[slot].bindings[param].lfo_depth   = 0.1f;
            }
        }
        TEST(Synth::validate_instrument_bank(&bank));

        const std::unique_ptr<Sculptor::Graph> graph_storage = std::make_unique<Sculptor::Graph>();
        Sculptor::Graph&                       graph         = *graph_storage;
        Sculptor::EffectGraphMapping           mapping       = {};
        TEST(project_legacy_effects(bank, 0, &graph, &mapping));
        TEST(mapping.lfo_count == max_desc);

        // The compressor node carries the full modulation grammar: 5 params
        // x 10 slots + In/Out/Enabled = 53, under the widget's per-node cap.
        TEST(graph.node(mapping.effect_nodes[2]).slots.num_allocated == 3 + 5 * Sculptor::fx_param_stride);
        uint32_t node_count = 0;
        for (uint32_t idx = 0; idx < Sculptor::max_nodes; ++idx) {
            if (graph.node_occupied(idx)) {
                ++node_count;
            }
        }
        TEST(node_count == 21); // input, output, MIDI, 4 effects, 14 LFOs

        uint32_t num_modulated = 0;
        uint32_t state_bytes   = 0;
        Sculptor::count_effect_budgets(bank, &num_modulated, &state_bytes);
        TEST(num_modulated == max_desc);
    }

    // Disabled allocations release reservations; re-enable starts fresh DSP history.
    {
        const std::unique_ptr<Synth::InstrumentBank> bank_storage = std::make_unique<Synth::InstrumentBank>();
        Synth::InstrumentBank&                       bank         = *bank_storage;
        make_valid_bank(bank);
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            enabled_effect(bank, Synth::max_channels, slot, Synth::EffectType::delay);
        }
        Synth::init_effect_state_region(fake_region_base);
        Synth::EffectExpansionPlan plan  = {};
        const char*                error = nullptr;
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::EffectChain chains[Synth::max_channels] = {};
        Synth::EffectChain master                      = {};
        FakeWriter         writer                      = { 1, 0, 0, 0, 0 };
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));
        (void)Synth::take_effect_clear_ranges();

        // Disabled slots carry no state and reserve no live bytes.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            bank.master_chain.effects[slot].enabled = false;
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        Synth::commit_effect_expansion(bank, plan, chains, &master, fake_writer_binding(writer));

        TEST(plan.consumed_bytes == 0);
        // Re-enabling reuses the released space, with fresh initialization.
        for (uint32_t slot = 0; slot < Synth::max_chain_effects; ++slot) {
            bank.master_chain.effects[slot].enabled = true;
        }
        TEST(Synth::preflight_effect_expansion(bank, &plan, &error));
        TEST(plan.consumed_bytes == 4 * Synth::get_effect_state_bytes(Synth::EffectType::delay));

        Synth::init_effect_state_region(fake_region_base);
    }
}

int main()
{
    test_silence_stdout();
    effects_paste_pin_regressions();
    effects_layout_classification_regressions();
    effects_structural_regressions();
    effects_clipboard_regressions();
    effects_add_cc_regressions();
    effects_enable_regressions();
    effects_inline_modulator_regressions();
    fx_inputs_move_reprojection_regressions();
    fx_auto_lane_spacing_regressions();
    fx_output_node_move_regressions();
    fx_drag_session_reset_regressions();
    fx_drop_anchor_flag_regressions();
    fx_add_lfo_modeled_regressions();
    fx_lfo_binding_seed_regressions();
    fx_adjacent_projected_slots_regressions();
    fx_exchange_effect_slots_regressions();
    fx_routing_derivation_regressions();
    fx_routing_gesture_regressions();
    fx_routing_cycle_refusal_regressions();
    fx_routing_delete_regressions();
    fx_routing_contiguity_regressions();
    fx_swap_layout_exchange_regressions();
    fx_effective_enablement_regressions();
    fx_input_bypass_regressions();
    fx_connector_red_regressions();
    fx_routing_migration_regressions();
    fx_add_effect_default_regressions();
    fx_slot_disable_stamp_regressions();
    distortion_m1_regressions();
    check_effects_inline_0();
    check_effects_inline_1();
    check_effects_inline_2();
    return test_exit_code();
}
