# SPDX-License-Identifier: MIT
# Copyright (c) 2021-2022 Chris Dragan

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)

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

##############################################################################
# Directory where generated files are stored

out_dir = Out

##############################################################################
# Sources

lib_src_files += mstdc.cpp
lib_src_files += vmath.cpp

threed_src_files += minivulkan.cpp

ifeq ($(UNAME), Linux)
    threed_src_files += main_linux.m
endif

ifeq ($(UNAME), Darwin)
    threed_src_files += main_macos.mm
    imgui_src_files  += imgui/backends/imgui_impl_osx.mm
endif

ifeq ($(UNAME), Windows)
    threed_src_files += main_windows.cpp
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

all_src_files += $(lib_src_files)
all_src_files += $(threed_src_files)
all_src_files += $(vmath_unit_src_files)
all_src_files += $(imgui_src_files)

all_threed_src_files += $(lib_src_files)
all_threed_src_files += $(threed_src_files)
ifdef imgui
    all_threed_src_files += $(imgui_src_files)
endif

all_vmath_unit_src_files += $(lib_src_files)
all_vmath_unit_src_files += $(vmath_unit_src_files)

##############################################################################
# Shaders

shader_files += shaders/simple.vert
shader_files += shaders/phong.frag

##############################################################################
# Compiler flags

ifeq ($(UNAME), Windows)
    WFLAGS += -W3

    ifdef debug
        CFLAGS  += -D_DEBUG -Zi -MTd
        LDFLAGS += -debug
    else
        CFLAGS  += -O1 -Oi -DNDEBUG -GL -MT
        LDFLAGS += -ltcg
    endif

    ifndef stdlib
        CFLAGS  += -DNOSTDLIB -D_NO_CRT_STDIO_INLINE -Zc:threadSafeInit- -GS- -Gs9999999
        LDFLAGS += -nodefaultlib -stack:0x100000,0x100000
    endif

    CFLAGS += -nologo
    CFLAGS += -GR-
    CFLAGS += -TP -EHa-
    CFLAGS += -FS
    CFLAGS += -std:c++17 -Zc:__cplusplus

    LDFLAGS += -nologo
    LDFLAGS += user32.lib kernel32.lib

    CXX   = cl.exe
    LINK  = link.exe
    STRIP = true

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
    CFLAGS += -msse4.1

    ifdef debug
        CFLAGS  += -fsanitize=address
        LDFLAGS += -fsanitize=address

        STRIP = true
        CFLAGS += -O0 -g
    else
        CFLAGS += -DNDEBUG -Os
        CFLAGS += -fomit-frame-pointer
        CFLAGS += -fno-stack-check -fno-stack-protector -fno-threadsafe-statics

        CFLAGS  += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
    endif

    CXXFLAGS += -x c++ -std=c++17 -fno-rtti -fno-exceptions

    OBJCXXFLAGS += -x objective-c++ -std=c++17 -fno-objc-arc

    LINK = $(CXX)

    COMPILER_OUTPUT = -o $1
    LINKER_OUTPUT   = -o $1
endif

ifeq ($(UNAME), Linux)
    LDFLAGS += -lxcb -ldl

    ifndef debug
        STRIP = strip

        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed

        LTO_CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS    += -flto=auto -fuse-linker-plugin
    endif
endif

ifeq ($(UNAME), Darwin)
    frameworks += Cocoa
    frameworks += CoreVideo
    frameworks += Quartz

    ifdef imgui
        frameworks += GameController
    endif

    LDFLAGS += -fno-objc-arc $(addprefix -framework ,$(frameworks))

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
ifndef GLSL_NO_OPTIMIZER
    GLSL_FLAGS += -Os
endif
ifdef debug
    GLSL_FLAGS += -g
endif

##############################################################################
# Executable

exe_name = minivulkan

ifeq ($(UNAME), Windows)
    exe_suffix = .exe
else
    exe_suffix =
endif

exe = $(out_dir)/$(exe_name)$(exe_suffix)

ifeq ($(UNAME), Darwin)
    macos_app_dir = $(out_dir)/$(exe_name).app/Contents/MacOS

    exe = $(macos_app_dir)/$(exe_name)
endif

ifeq ($(UNAME), Windows)
$(exe): LDFLAGS += -subsystem:windows
$(out_dir)/vmath_unit.$(exe_suffix): LDFLAGS += -subsystem:console
endif

##############################################################################
# Rules

default: $(exe)

clean:
	rm -rf $(out_dir)

asm: $(addprefix $(out_dir)/,$(addsuffix .$(asm_suffix),$(notdir $(filter %.cpp,$(all_threed_src_files)))))

$(out_dir):
	mkdir -p $@

define LINK_RULE
$1: $$(call OBJ_FROM_SRC, $2)
	$$(LINK) $$(call LINKER_OUTPUT,$$@) $$^ $$(LDFLAGS)
	$$(STRIP) $$@
endef

$(eval $(call LINK_RULE,$(exe),$(all_threed_src_files)))

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

define C_RULE
$$(call OBJ_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CXX) $$(CFLAGS) $$(CXXFLAGS) -masm=intel -S $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(foreach file, $(filter %.c, $(all_src_files)), $(eval $(call C_RULE,$(file))))

define ASM_RULE
$$(call OBJ_FROM_SRC,$1): $1 | $$(out_dir)
	$$(CXX) $$(CFLAGS) $$(CXXFLAGS) -masm=intel -S $$(call COMPILER_OUTPUT,$$@) $$<
endef

$(foreach file, $(filter %.c, $(all_src_files)), $(eval $(call ASM_RULE,$(file))))

ifeq ($(UNAME), Darwin)
$(macos_app_dir): | $(out_dir)
	mkdir -p $(macos_app_dir)

$(exe): | $(macos_app_dir)

default: $(macos_app_dir)/Info.plist

$(macos_app_dir)/Info.plist: Info.plist | $(macos_app_dir)
	cp $< $@
endif

$(foreach file, $(filter-out $(imgui_src_files), $(all_src_files)), $(call OBJ_FROM_SRC, $(file))): CFLAGS += $(WFLAGS)

$(foreach file, $(imgui_src_files), $(call OBJ_FROM_SRC, $(file))): CFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES

ifdef imgui
    CFLAGS += -Iimgui -DENABLE_GUI=1
endif

$(out_dir)/shaders: | $(out_dir)
	mkdir -p $@

ifdef VULKAN_SDK_BIN
    GLSL_VALIDATOR = $(VULKAN_SDK_BIN)/glslangValidator
else
    GLSL_VALIDATOR = glslangValidator
endif

define GLSL_EXT
$(out_dir)/shaders/%.$1.h: shaders/%.$1 | $(out_dir)/shaders
	$(GLSL_VALIDATOR) $(GLSL_FLAGS) --variable-name $$(subst .,_,$$(notdir $$<)) -o $$@ $$<
endef

$(foreach ext, vert frag, $(eval $(call GLSL_EXT,$(ext))))

$(call OBJ_FROM_SRC, minivulkan.cpp) $(out_dir)/minivulkan.cpp.$(asm_suffix): $(addprefix $(out_dir)/,$(addsuffix .h,$(shader_files)))
$(call OBJ_FROM_SRC, minivulkan.cpp) $(out_dir)/minivulkan.cpp.$(asm_suffix): CFLAGS += -I$(out_dir)/shaders

$(eval $(call LINK_RULE,$(out_dir)/vmath_unit,$(all_vmath_unit_src_files)))

test: $(out_dir)/vmath_unit
	$(out_dir)/vmath_unit

##############################################################################
# Dependency files

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(all_src_files)))))

-include $(dep_files)
