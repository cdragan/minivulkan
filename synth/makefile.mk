# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2025 Chris Dragan

gui_project_name = sculptor

shader_files += synth_oscillator.comp.glsl
shader_files += synth_fir.comp.glsl
shader_files += synth_chan_combine.comp.glsl
shader_files += synth_output_16_interlv.comp.glsl
shader_files += synth_output_f32_separate.comp.glsl

# Make sure synth shaders are available in all projects
$(call OBJ_FROM_SRC, realtime_synth.cpp): $(gen_headers_dir)/synth_shaders.h
threed_src_files += $(gen_headers_dir)/synth_shaders.cpp
