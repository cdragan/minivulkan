# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#gui_project_name = synth

lib_src_files += synth/midi_decode.cpp
lib_src_files += synth/synth_parameters.cpp
lib_src_files += synth/synth_effects.cpp
lib_src_files += synth/synth_effect_expansion.cpp
lib_src_files += synth/synth_midi_cc.cpp
lib_src_files += synth/synth_instrument.cpp
lib_src_files += synth/synth_soundtrack.cpp

synth_unit_common_src_files += synth/synth_test_common.cpp
synth_core_unit_src_files += synth/synth_core_unit.cpp
synth_runtime_unit_src_files += synth/synth_runtime_unit.cpp
synth_effects_unit_src_files += synth/synth_effects_unit.cpp
synth_osc_unit_src_files += synth/synth_osc_unit.cpp
synth_fir_unit_src_files += synth/synth_fir_unit.cpp
synth_string_unit_src_files += synth/synth_string_unit.cpp
synth_bank_unit_src_files += synth/synth_bank_unit.cpp
synth_clipboard_unit_src_files += synth/synth_clipboard_unit.cpp

synth_unit_common_src_files += sculptor/sculptor_undo.cpp
synth_unit_common_src_files += synth/synth_effect_expansion.cpp
synth_unit_common_src_files += sculptor/sculptor_atomic_file.cpp
synth_unit_common_src_files += sculptor/sculptor_instr_bank.cpp
synth_unit_common_src_files += sculptor/sculptor_instr_library.cpp
synth_unit_common_src_files += sculptor/sculptor_bank_json.cpp
synth_unit_common_src_files += sculptor/sculptor_graph.cpp
synth_unit_common_src_files += sculptor/sculptor_osc_graph.cpp
synth_unit_common_src_files += sculptor/sculptor_effect_graph.cpp
synth_unit_common_src_files += synth/midi_file.cpp
synth_unit_common_src_files += synth/synth_soundtrack_codec.cpp
synth_unit_common_src_files += synth/midi_input.cpp

midi_decode_unit_src_files += synth/midi_decode_unit.cpp

threed_src_files += synth/realtime_synth.cpp
threed_src_files += synth/empty_midi_soundtrack.cpp
threed_gui_src_files += synth/midi_input.cpp

threed_nogui_src_files += synth/midi_input_none.cpp

shader_files += synth_fir_coeff.comp.glsl
shader_files += synth_oscillator.comp.glsl
shader_files += synth_chan_combine.comp.glsl
shader_files += synth_master_mix.comp.glsl
shader_files += synth_effect.comp.glsl
shader_files += synth_output_16_interlv.comp.glsl
shader_files += synth_output_f32_interlv.comp.glsl
shader_files += synth_output_f32_separate.comp.glsl

# Make sure synth shaders are available in all projects
$(call OBJ_FROM_SRC, realtime_synth.cpp): $(gen_headers_dir)/synth_shaders.h
threed_src_files += $(gen_headers_dir)/synth_shaders.cpp
