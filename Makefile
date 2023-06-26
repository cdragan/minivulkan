# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2022 Chris Dragan

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)
ARCH ?= $(shell uname -m)

ifneq (,$(filter CYGWIN% MINGW% MSYS%, $(UNAME)))
    # Note: Still use cl.exe on Windows
    UNAME = Windows
endif

##############################################################################
# Default build flags

# Debug vs release
debug ?= 0

# Enable spirv shuffling, disable for debugging purposes
spirv_shuffle ?= 1

# Enable spirv optimization, disable for debugging purposes
spirv_opt ?= 1

# Windows only: 1 links against MSVCRT, 0 doesn't use MSVCRT
ifeq ($(UNAME), Windows)
    stdlib ?= 0
else
    override stdlib := 1
endif

# Enable address sanitizer in debug builds
ifneq ($(UNAME), Windows)
    # Disable sanitizers for nsight
    ifeq ($(spirv_opt), 0)
        sanitize =
    endif
    # Use address sanitizer only in debug builds
    ifneq ($(debug), 0)
        sanitize ?= address
    endif
    sanitize ?=
endif

##############################################################################
# Declare default target

default:

##############################################################################
# Directory where generated files are stored

out_dir_base ?= Out

ifeq ($(UNAME), Windows)
    ifneq ($(stdlib), 0)
        out_dir_suffix = _stdlib
    endif
else
    ifneq ($(sanitize),)
        out_dir_suffix = _$(sanitize)
    endif
endif

out_dir_suffix ?=

ifeq ($(debug), 0)
    out_dir_config = release
else
    out_dir_config = debug
endif

out_dir = $(out_dir_base)/$(out_dir_config)$(out_dir_suffix)

##############################################################################
# Function for converting source path to object file path

ifeq ($(UNAME), Windows)
    o_suffix = obj
else
    o_suffix = o
endif

asm_suffix = S

OBJ_FROM_SRC = $(addsuffix .$(o_suffix), $(addprefix $(out_dir)/,$(basename $(notdir $1))))

ASM_FROM_SRC = $(addsuffix .$(asm_suffix), $(addprefix $(out_dir)/,$(notdir $1)))

##############################################################################
# Sources

lib_src_files += mstdc.cpp
lib_src_files += rng.cpp
lib_src_files += vmath.cpp

threed_src_files += host_filler.cpp
threed_src_files += memory_heap.cpp
threed_src_files += minivulkan.cpp
threed_src_files += resource.cpp
threed_src_files += shaders.cpp
threed_src_files += sound.cpp

ifeq ($(UNAME), Linux)
    threed_src_files       += main_linux.cpp
    threed_gui_src_files   += gui_linux.cpp
    threed_nogui_src_files += nogui_linux.cpp
endif

ifeq ($(UNAME), Darwin)
    threed_src_files       += main_macos.mm
    threed_gui_src_files   += gui_macos.mm
    imgui_src_files        += imgui/backends/imgui_impl_osx.mm
    threed_nogui_src_files += nogui_macos.mm
endif

ifeq ($(UNAME), Windows)
    threed_src_files       += main_windows.cpp
    imgui_src_files        += imgui/backends/imgui_impl_win32.cpp
    threed_gui_src_files   += gui_windows.cpp
    threed_nogui_src_files += nogui_windows.cpp

    ifeq ($(stdlib), 0)
        lib_src_files += mstdc_windows.cpp
    endif
endif

vmath_unit_src_files += vmath_unit.cpp

imgui_src_files += imgui/imgui.cpp
imgui_src_files += imgui/imgui_draw.cpp
imgui_src_files += imgui/imgui_tables.cpp
imgui_src_files += imgui/imgui_widgets.cpp
imgui_src_files += imgui/backends/imgui_impl_vulkan.cpp
ifneq ($(debug), 0)
    imgui_src_files += imgui/imgui_demo.cpp
endif

threed_gui_src_files += $(imgui_src_files)
threed_gui_src_files += gui.cpp

threed_nogui_src_files += nogui.cpp

spirv_encode_src_files += tools/spirv_encode.cpp

all_src_files += $(lib_src_files)
all_src_files += $(threed_src_files)
all_src_files += $(threed_gui_src_files)
all_src_files += $(threed_nogui_src_files)
all_src_files += $(vmath_unit_src_files)
all_src_files += $(spirv_encode_src_files)

all_gui_src_files += $(threed_gui_src_files)

all_vmath_unit_src_files += $(lib_src_files)
all_vmath_unit_src_files += $(vmath_unit_src_files)

##############################################################################
# Shaders

shader_files += shaders/simple.vert.glsl
shader_files += shaders/phong.frag.glsl
shader_files += shaders/pass_through.vert.glsl
shader_files += shaders/rounded_cube.vert.glsl
shader_files += shaders/bezier_line_cubic_sculptor.tesc.glsl
shader_files += shaders/bezier_line_cubic_sculptor.tese.glsl
shader_files += shaders/bezier_surface_quadratic.tesc.glsl
shader_files += shaders/bezier_surface_quadratic.tese.glsl
shader_files += shaders/bezier_surface_cubic.tesc.glsl
shader_files += shaders/bezier_surface_cubic.tese.glsl
shader_files += shaders/bezier_surface_cubic_sculptor.tesc.glsl
shader_files += shaders/bezier_surface_cubic_sculptor.tese.glsl
shader_files += shaders/sculptor_edge.frag.glsl
shader_files += shaders/sculptor_object.frag.glsl
shader_files += shaders/synth.comp.glsl
shader_files += shaders/mono_to_stereo.comp.glsl

##############################################################################
# Sub-project handling

define include_project

project_path = $1
gui_project_name =
nogui_project_name =
src_files =
gui_src_files =
nogui_src_files =
use_threed = 1

include $1/makefile.mk

project_$1_src_files       := $$(src_files)
project_$1_gui_src_files   := $$(gui_src_files)
project_$1_nogui_src_files := $$(nogui_src_files)

all_src_files += $$(addprefix $1/,$$(project_$1_src_files))

ifneq ($$(gui_project_name),)
    all_$$(gui_project_name)_src_files += $$(addprefix $1/,$$(project_$1_src_files))
    all_$$(gui_project_name)_src_files += $$(addprefix $1/,$$(project_$1_gui_src_files))
    all_$$(gui_project_name)_src_files += $$(lib_src_files)
    all_$$(gui_project_name)_src_files += $$(threed_src_files)
    all_$$(gui_project_name)_src_files += $$(threed_gui_src_files)

    all_gui_src_files += $$(addprefix $1/,$$(project_$1_src_files))
    all_gui_src_files += $$(addprefix $1/,$$(project_$1_gui_src_files))
    all_src_files     += $$(addprefix $1/,$$(project_$1_gui_src_files))

    project_$1_gui_name := $$(gui_project_name)
    gui_targets         += $$(project_$1_gui_name)
endif

ifneq ($$(nogui_project_name),)
    all_$$(nogui_project_name)_src_files += $$(addprefix $1/,$$(project_$1_src_files))
    all_$$(nogui_project_name)_src_files += $$(lib_src_files)

    ifeq ($$(use_threed), 1)
        all_$$(nogui_project_name)_src_files += $$(addprefix $1/,$$(project_$1_nogui_src_files))
        all_src_files                        += $$(addprefix $1/,$$(project_$1_nogui_src_files))
        all_$$(nogui_project_name)_src_files += $$(threed_src_files)
        all_$$(nogui_project_name)_src_files += $$(threed_nogui_src_files)
    endif

    project_$1_nogui_name := $$(nogui_project_name)
    nogui_targets         += $$(project_$1_nogui_name)
endif

endef

##############################################################################
# Sub-projects

projects += example
projects += matedit
projects += sculptor
projects += sleek
projects += synth

$(foreach project,$(projects),$(eval $(call include_project,$(project))))

##############################################################################
# Compiler flags

SUBSYSTEMFLAGS =
LDFLAGS_gui    =

ifeq ($(UNAME), Windows)
    WFLAGS += -W3

    ifeq ($(debug), 0)
        CFLAGS  += -O1 -Oi -DNDEBUG -GL -MT
        LDFLAGS += -ltcg
    else
        CFLAGS  += -D_DEBUG -Zi -FS -MTd
        LDFLAGS += -debug
    endif

    ifeq ($(stdlib), 0)
        CFLAGS += -DNOSTDLIB -D_NO_CRT_STDIO_INLINE -Zc:threadSafeInit- -GS- -Gs9999999
        LDFLAGS_NODEFAULTLIB += -nodefaultlib -stack:0x100000,0x100000
    else
        LDFLAGS_NODEFAULTLIB =
    endif

    CFLAGS += -nologo
    CFLAGS += -GR-
    CFLAGS += -TP -EHa-
    CFLAGS += -FS
    CFLAGS += -std:c++17 -Zc:__cplusplus

    win_libs += kernel32.lib
    win_libs += ole32.lib
    win_libs += user32.lib

    LDFLAGS += -nologo
    LDFLAGS += $(win_libs)

    CXX  = cl.exe
    LINK = link.exe

    COMPILER_OUTPUT = -Fo:$1
    LINKER_OUTPUT   = -out:$1

    ifeq ($(debug), 0)
        $(call OBJ_FROM_SRC, mstdc_windows.cpp): CFLAGS += -GL-
    endif
else
    WFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused -Wno-missing-field-initializers
    WFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion

    CFLAGS += -fvisibility=hidden
    CFLAGS += -fPIC
    CFLAGS += -MD
    ifneq ($(ARCH), aarch64)
        CFLAGS += -msse4.1
    endif

    ifeq ($(debug), 0)
        CFLAGS += -DNDEBUG -Os
        CFLAGS += -fomit-frame-pointer
        CFLAGS += -fno-stack-check -fno-stack-protector -fno-threadsafe-statics

        CFLAGS  += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
    else
        ifneq ($(sanitize),)
            CFLAGS  += -fsanitize=$(sanitize)
            LDFLAGS += -fsanitize=$(sanitize)
        endif

        CFLAGS += -O0 -g
    endif

    CXXFLAGS += -x c++ -std=c++17 -fno-rtti -fno-exceptions

    OBJCXXFLAGS += -x objective-c++ -std=c++17

    # For compatibility with MSVC
    LDFLAGS_NODEFAULTLIB =

    LINK = $(CXX)

    COMPILER_OUTPUT = -o $1
    LINKER_OUTPUT   = -o $1
endif

ifeq ($(UNAME), Linux)
    LDFLAGS += -lxcb -lxcb-xfixes -ldl

    ifeq ($(debug), 0)
        STRIP = strip

        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed

        LTO_CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS    += -flto=auto -fuse-linker-plugin
    endif
endif

ifeq ($(UNAME), Darwin)
    frameworks += AVFoundation
    frameworks += Cocoa
    frameworks += CoreVideo
    frameworks += Quartz

    gui_frameworks += GameController

    LDFLAGS     += $(addprefix -framework ,$(frameworks))
    LDFLAGS_gui += $(addprefix -framework ,$(gui_frameworks))

    ifeq ($(debug), 0)
        STRIP = strip -x

        LDFLAGS += -Wl,-dead_strip

        LTO_CFLAGS += -flto
        LDFLAGS    += -flto
    else
        export MallocNanoZone=0
    endif
endif

time_stats ?= 0
ifeq ($(time_stats), 1)
    CFLAGS += -DTIME_STATS=1
endif

ifdef VULKAN_SDK
    CFLAGS += -I$(VULKAN_SDK)/include
endif

GLSL_FLAGS = --target-env vulkan1.1
GLSL_OPT_FLAGS =
GLSL_STRIP_FLAGS =
GLSL_ENCODE_FLAGS =
ifndef GLSL_NO_OPTIMIZER
    GLSL_OPT_FLAGS += -Os
    GLSL_STRIP_FLAGS += --strip all --dce all
endif
ifeq ($(spirv_opt), 0)
    GLSL_FLAGS += -g
else
    GLSL_ENCODE_FLAGS += --remove-unused
endif
ifeq ($(spirv_shuffle), 0)
    GLSL_ENCODE_FLAGS += --no-shuffle
    CFLAGS += -DNO_SPIRV_SHUFFLE
endif

ASM_SYNTAX =
ifneq ($(ARCH), aarch64)
    ASM_SYNTAX = -masm=intel
endif

##############################################################################
# Executable

ifeq ($(UNAME), Windows)
    exe_suffix = .exe
else
    exe_suffix =
endif

CMDLINE_PATH = $(out_dir)/$1$(exe_suffix)

ifeq ($(UNAME), Darwin)
    GUI_PATH = $(out_dir)/$1.app/Contents/MacOS/$1

    define GUI_LINK_RULE
      $(call LINK_RULE,$(call GUI_PATH,$1),$2)

      $$(out_dir)/$1.app/Contents/MacOS: | $$(out_dir)
	mkdir -p $$@

      $(call GUI_PATH,$1): | $$(out_dir)/$1.app/Contents/MacOS/Info.plist

      $$(out_dir)/$1.app/Contents/MacOS/Info.plist: Info.plist | $$(out_dir)/$1.app/Contents/MacOS
	cp $$< $$@
    endef
else
    GUI_PATH = $(out_dir)/$1$(exe_suffix)
    GUI_LINK_RULE = $(call LINK_RULE,$(call GUI_PATH,$1),$2)
endif

##############################################################################
# Rules

# imgui requires std C library
ifeq ($(stdlib), 0)
    gui_targets :=
endif

default: $(gui_targets) $(nogui_targets)

define make_target_rule
.PHONY: $1
$1: $$(call GUI_PATH,$1)
endef

$(foreach target,$(gui_targets) $(nogui_targets),$(eval $(call make_target_rule,$(target))))

clean:
	rm -rf $(out_dir)

asm: $(addprefix $(out_dir)/,$(addsuffix .$(asm_suffix),$(notdir $(filter %.cpp,$(all_example_src_files)))))

$(out_dir_base):
	mkdir -p $@

$(out_dir): | $(out_dir_base)
	mkdir -p $@

define LINK_RULE
$1: $$(call OBJ_FROM_SRC, $2)
	$$(LINK) $$(call LINKER_OUTPUT,$$@) $$^ $$(LDFLAGS) $$(SUBSYSTEMFLAGS) $$(LDFLAGS_NODEFAULTLIB)
ifdef STRIP
	$$(STRIP) $$@
endif
endef

ifeq ($(UNAME), Windows)
$(foreach target,$(gui_targets) $(nogui_targets),$(call GUI_PATH,$(target))): SUBSYSTEMFLAGS = -subsystem:windows
endif

ifeq ($(UNAME), Darwin)
$(foreach target,$(gui_targets),$(call GUI_PATH,$(target))): SUBSYSTEMFLAGS = $(LDFLAGS_gui)
endif

$(foreach target,$(gui_targets) $(nogui_targets),$(eval $(call GUI_LINK_RULE,$(target),$(all_$(target)_src_files))))

define MM_RULE
$$(call OBJ_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CC) $$(CFLAGS) $$(LTO_CFLAGS) $$(OBJCXXFLAGS) -c $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(foreach file, $(filter %.mm, $(all_src_files)), $(eval $(call MM_RULE,$(file))))

define CPP_RULE
$$(call OBJ_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CXX) $$(CFLAGS) $$(LTO_CFLAGS) $$(CXXFLAGS) -c $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(foreach file, $(filter %.cpp, $(all_src_files)), $(eval $(call CPP_RULE,$(file))))

define ASM_RULE
$$(call ASM_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CXX) $$(CFLAGS) $$(CXXFLAGS) $$(ASM_SYNTAX) -S $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(foreach file, $(filter %.cpp, $(all_src_files)), $(eval $(call ASM_RULE,$(file))))

$(foreach file, $(filter-out $(imgui_src_files), $(all_src_files)), $(call OBJ_FROM_SRC, $(file))): CFLAGS += $(WFLAGS)

$(foreach file, $(imgui_src_files), $(call OBJ_FROM_SRC, $(file))): CFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES

$(foreach file, $(all_gui_src_files), $(call OBJ_FROM_SRC, $(file))): CFLAGS += -Iimgui

shaders_out_dir := $(out_dir_base)/shaders

ifeq ($(spirv_opt), 0)
    shaders_out_dir := $(shaders_out_dir)_noopt
endif
ifeq ($(spirv_shuffle), 0)
    shaders_out_dir := $(shaders_out_dir)_noshuffle
endif

$(shaders_out_dir): | $(out_dir_base)
	mkdir -p $@

shader_dirs = default opt strip bin

$(addprefix $(shaders_out_dir)/,$(shader_dirs)): | $(shaders_out_dir)
	mkdir -p $@

shader_stage = $(addprefix $(shaders_out_dir)/$1/,$(subst .glsl,.spv,$(notdir $2)))

ifdef VULKAN_SDK_BIN
    GLSL_VALIDATOR_PREFIX = $(VULKAN_SDK_BIN)/
else
    GLSL_VALIDATOR_PREFIX =
endif

spirv_encode = $(call CMDLINE_PATH,spirv_encode)

ifeq ($(UNAME), Windows)
$(spirv_encode): LDFLAGS_NODEFAULTLIB =
$(spirv_encode): SUBSYSTEMFLAGS = -subsystem:console
endif

$(eval $(call LINK_RULE,$(spirv_encode),$(spirv_encode_src_files)))

define GLSL_EXT
$(shaders_out_dir)/%.$1.h: shaders/%.$1.glsl | $(spirv_encode) $(shaders_out_dir) $(addprefix $(shaders_out_dir)/,$(shader_dirs))
	$(GLSL_VALIDATOR_PREFIX)glslangValidator $(GLSL_FLAGS) -o $$(call shader_stage,default,$$<) $$<
	$(GLSL_VALIDATOR_PREFIX)spirv-opt $(GLSL_OPT_FLAGS) $$(call shader_stage,default,$$<) -o $$(call shader_stage,opt,$$<)
	cd $(shaders_out_dir)/opt && $(GLSL_VALIDATOR_PREFIX)spirv-remap $(GLSL_STRIP_FLAGS) --input $$(subst .glsl,.spv,$$(notdir $$<)) --output ../../../$(shaders_out_dir)/strip
	$(spirv_encode) $(GLSL_ENCODE_FLAGS) shader_$$(subst .,_,$$(basename $$(notdir $$<))) $$(call shader_stage,strip,$$<) $$@
	$(spirv_encode) $(GLSL_ENCODE_FLAGS) --binary shader_$$(subst .,_,$$(basename $$(notdir $$<))) $$(call shader_stage,strip,$$<) $$(basename $$@).bin
	$(GLSL_VALIDATOR_PREFIX)spirv-dis -o $$(basename $$@).disasm $$(call shader_stage,strip,$$<)
endef

$(foreach ext, vert tesc tese geom frag comp, $(eval $(call GLSL_EXT,$(ext))))

$(call OBJ_FROM_SRC, shaders.cpp) $(out_dir)/shaders.cpp.$(asm_suffix): $(addprefix $(shaders_out_dir)/,$(addsuffix .h,$(basename $(notdir $(shader_files)))))
$(call OBJ_FROM_SRC, shaders.cpp) $(out_dir)/shaders.cpp.$(asm_suffix): CFLAGS += -I$(shaders_out_dir)

$(eval $(call LINK_RULE,$(call CMDLINE_PATH,vmath_unit),$(all_vmath_unit_src_files)))

test: $(call CMDLINE_PATH,vmath_unit)
	$<

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))

-include $(dep_files)
