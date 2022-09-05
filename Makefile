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
# Directory where generated files are stored

out_dir = Out

##############################################################################
# Sources

lib_src_files += mstdc.cpp
lib_src_files += rng.cpp
lib_src_files += vmath.cpp

threed_src_files += minivulkan.cpp
threed_src_files += shaders.cpp
threed_src_files += sound.cpp

ifeq ($(UNAME), Linux)
    threed_src_files += main_linux.cpp
    imgui_src_files  += gui_linux.cpp
    ifndef imgui
        threed_src_files += nogui_linux.cpp
    endif
endif

ifeq ($(UNAME), Darwin)
    threed_src_files += main_macos.mm
    imgui_src_files  += gui_macos.mm
    imgui_src_files  += imgui/backends/imgui_impl_osx.mm
    ifndef imgui
        threed_src_files += nogui_macos.mm
    endif
endif

ifeq ($(UNAME), Windows)
    threed_src_files += main_windows.cpp
    imgui_src_files  += imgui/backends/imgui_impl_win32.cpp
    ifdef imgui
        stdlib = 1
    endif
    ifndef stdlib
        lib_src_files += mstdc_windows.cpp
    endif
endif

vmath_unit_src_files += vmath_unit.cpp

imgui_src_files += imgui/imgui.cpp
imgui_src_files += imgui/imgui_draw.cpp
imgui_src_files += imgui/imgui_tables.cpp
imgui_src_files += imgui/imgui_widgets.cpp
imgui_src_files += imgui/backends/imgui_impl_vulkan.cpp
imgui_src_files += gui.cpp
ifdef imgui
    threed_src_files += $(imgui_src_files)
else
    threed_src_files += nogui.cpp
endif

spirv_encode_src_files += tools/spirv_encode.cpp

example_src_files += example/example.cpp

synth_src_files += synth/synth.cpp

all_src_files += $(lib_src_files)
all_src_files += $(threed_src_files)
all_src_files += $(vmath_unit_src_files)
all_src_files += $(spirv_encode_src_files)
all_src_files += $(example_src_files)
all_src_files += $(synth_src_files)

all_example_src_files += $(example_src_files)
all_example_src_files += $(lib_src_files)
all_example_src_files += $(threed_src_files)

all_synth_src_files += $(synth_src_files)
all_synth_src_files += $(lib_src_files)
all_synth_src_files += $(threed_src_files)

all_vmath_unit_src_files += $(lib_src_files)
all_vmath_unit_src_files += $(vmath_unit_src_files)

##############################################################################
# Shaders

shader_files += shaders/simple.vert.glsl
shader_files += shaders/phong.frag.glsl
shader_files += shaders/pass_through.vert.glsl
shader_files += shaders/rounded_cube.vert.glsl
shader_files += shaders/bezier_surface_quadratic.tesc.glsl
shader_files += shaders/bezier_surface_quadratic.tese.glsl
shader_files += shaders/bezier_surface_cubic.tesc.glsl
shader_files += shaders/bezier_surface_cubic.tese.glsl
shader_files += shaders/synth.comp.glsl
shader_files += shaders/mono_to_stereo.comp.glsl

##############################################################################
# Compiler flags

SUBSYSTEMFLAGS =

ifeq ($(UNAME), Windows)
    WFLAGS += -W3

    ifdef debug
        CFLAGS  += -D_DEBUG -Zi -MTd
        LDFLAGS += -debug
    else
        CFLAGS  += -O1 -Oi -DNDEBUG -GL -MT
        LDFLAGS += -ltcg
    endif

    ifdef stdlib
        LDFLAGS_NODEFAULTLIB =
    else
        CFLAGS += -DNOSTDLIB -D_NO_CRT_STDIO_INLINE -Zc:threadSafeInit- -GS- -Gs9999999
        LDFLAGS_NODEFAULTLIB += -nodefaultlib -stack:0x100000,0x100000
    endif

    CFLAGS += -nologo
    CFLAGS += -GR-
    CFLAGS += -TP -EHa-
    CFLAGS += -FS
    CFLAGS += -std:c++17 -Zc:__cplusplus

    LDFLAGS += -nologo
    LDFLAGS += user32.lib kernel32.lib ole32.lib

    CXX  = cl.exe
    LINK = link.exe

    COMPILER_OUTPUT = -Fo:$1
    LINKER_OUTPUT   = -out:$1

    ifndef debug
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

    ifdef debug
        CFLAGS  += -fsanitize=address
        LDFLAGS += -fsanitize=address

        CFLAGS += -O0 -g
    else
        CFLAGS += -DNDEBUG -Os
        CFLAGS += -fomit-frame-pointer
        CFLAGS += -fno-stack-check -fno-stack-protector -fno-threadsafe-statics

        CFLAGS  += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
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

    ifndef debug
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

    ifdef imgui
        frameworks += GameController
    endif

    LDFLAGS += $(addprefix -framework ,$(frameworks))

    ifndef debug
        STRIP = strip -x

        LDFLAGS += -Wl,-dead_strip

        LTO_CFLAGS += -flto
        LDFLAGS    += -flto
    endif
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
ifdef debug
    GLSL_FLAGS += -g
else
    GLSL_ENCODE_FLAGS += --remove-unused
endif
ifdef no_spirv_shuffle
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

gui_targets = example synth

default: $(foreach target,$(gui_targets),$(call GUI_PATH,$(target)))

clean:
	rm -rf $(out_dir)

asm: $(addprefix $(out_dir)/,$(addsuffix .$(asm_suffix),$(notdir $(filter %.cpp,$(all_example_src_files)))))

$(out_dir):
	mkdir -p $@

define LINK_RULE
$1: $$(call OBJ_FROM_SRC, $2)
	$$(LINK) $$(call LINKER_OUTPUT,$$@) $$^ $$(LDFLAGS) $$(SUBSYSTEMFLAGS) $$(LDFLAGS_NODEFAULTLIB)
ifdef STRIP
	$$(STRIP) $$@
endif
endef

ifeq ($(UNAME), Windows)
$(foreach target,$(gui_targets),$(call GUI_PATH,$(target))): SUBSYSTEMFLAGS = -subsystem:windows
endif

$(foreach target,$(gui_targets),$(eval $(call GUI_LINK_RULE,$(target),$(all_$(target)_src_files))))

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

ifdef imgui
    CFLAGS += -Iimgui -DENABLE_GUI=1
endif

shaders_out_dir = $(out_dir)/shaders

$(shaders_out_dir): | $(out_dir)
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
$(shaders_out_dir)/%.$1.h: shaders/%.$1.glsl $(spirv_encode) | $(shaders_out_dir) $(addprefix $(shaders_out_dir)/,$(shader_dirs))
	$(GLSL_VALIDATOR_PREFIX)glslangValidator $(GLSL_FLAGS) -o $$(call shader_stage,default,$$<) $$<
	$(GLSL_VALIDATOR_PREFIX)spirv-opt $(GLSL_OPT_FLAGS) $$(call shader_stage,default,$$<) -o $$(call shader_stage,opt,$$<)
	$(GLSL_VALIDATOR_PREFIX)spirv-remap $(GLSL_STRIP_FLAGS) --input $$(call shader_stage,opt,$$<) --output $(shaders_out_dir)/strip
	$(spirv_encode) $(GLSL_ENCODE_FLAGS) shader_$$(subst .,_,$$(basename $$(notdir $$<))) $$(call shader_stage,strip,$$<) $$@
	$(spirv_encode) $(GLSL_ENCODE_FLAGS) --binary shader_$$(subst .,_,$$(basename $$(notdir $$<))) $$(call shader_stage,strip,$$<) $$(basename $$@).bin
endef

$(foreach ext, vert tesc tese geom frag comp, $(eval $(call GLSL_EXT,$(ext))))

$(call OBJ_FROM_SRC, shaders.cpp) $(out_dir)/shaders.cpp.$(asm_suffix): $(addprefix $(out_dir)/,$(addsuffix .h,$(basename $(shader_files))))
$(call OBJ_FROM_SRC, shaders.cpp) $(out_dir)/shaders.cpp.$(asm_suffix): CFLAGS += -I$(shaders_out_dir)

$(eval $(call LINK_RULE,$(call CMDLINE_PATH,vmath_unit),$(all_vmath_unit_src_files)))

test: $(call CMDLINE_PATH,vmath_unit)
	$<

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))

-include $(dep_files)
