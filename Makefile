# SPDX-License-Identifier: MIT
# Copyright (c) 2021 Chris Dragan

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)

ifneq (,$(filter CYGWIN% MINGW% MSYS%, $(UNAME)))
    # Note: Still use cl.exe on Windows
    UNAME = Windows
endif

##############################################################################
# Sources

src_files += minivulkan.cpp
src_files += mstdc.cpp
src_files += vmath.cpp

ifeq ($(UNAME), Linux)
    src_files += main_linux.m
endif

ifeq ($(UNAME), Darwin)
    src_files += main_macos.m
endif

ifeq ($(UNAME), Windows)
    src_files += main_windows.cpp
endif

##############################################################################
# Shaders

shader_files += shaders/simple.vert
shader_files += shaders/phong.frag

##############################################################################
# Compiler flags

ifeq ($(UNAME), Windows)
    ifdef debug
        CFLAGS  += -D_DEBUG -Z7 -MTd
        LDFLAGS += -debug
    else
        CFLAGS  += -O1 -DNDEBUG -Gs4096 -GL -MT
        LDFLAGS += -ltcg
    endif

    CFLAGS += -W3
    CFLAGS += -TP -EHsc
    CFLAGS += -std:c++17 -Zc:__cplusplus
    # -Zi -nologo -Gm- -GR- -EHa- -Oi -GS- -Gs9999999 -stack:0x100000,0x100000 kernel32.lib

    LDFLAGS += -nodefaultlib
    LDFLAGS += -subsystem:windows
else
    CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused -Wno-missing-field-initializers
    CFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion

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

    OBJCFLAGS += -x objective-c -fno-objc-arc

    LINK = $(CXX)
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

GLSL_FLAGS = -Os --target-env vulkan1.1
ifdef debug
    GLSL_FLAGS += -g
endif

##############################################################################
# Directory where generated files are stored

out_dir = Out

##############################################################################
# Function for converting source path to object file path

o_suffix = o

asm_suffix = S

OBJ_FROM_SRC = $(addsuffix .$(o_suffix), $(addprefix $(out_dir)/,$(basename $(notdir $1))))

##############################################################################
# Executable

exe_name = minivulkan

exe = $(out_dir)/$(exe_name)

ifeq ($(UNAME), Darwin)
    macos_app_dir = $(out_dir)/$(exe_name).app/Contents/MacOS

    exe = $(macos_app_dir)/$(exe_name)
endif

##############################################################################
# Rules

default: $(exe)

clean:
	rm -rf $(out_dir)

asm: $(addprefix $(out_dir)/,$(addsuffix .$(asm_suffix),$(notdir $(filter %.cpp,$(src_files)))))

$(out_dir):
	mkdir -p $@

$(exe): $(call OBJ_FROM_SRC, $(src_files))
	$(LINK) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

ifeq ($(UNAME), Darwin)
$(macos_app_dir): | $(out_dir)
	mkdir -p $(macos_app_dir)

$(exe): | $(macos_app_dir)

default: $(macos_app_dir)/Info.plist

$(macos_app_dir)/Info.plist: Info.plist | $(macos_app_dir)
	cp $< $@
endif

$(out_dir)/%.$(o_suffix): %.m | $(out_dir)
	$(CC) $(CFLAGS) $(LTO_CFLAGS) $(OBJCFLAGS) -c -o $@ $<

$(out_dir)/%.$(o_suffix): %.cpp | $(out_dir)
	$(CXX) $(CFLAGS) $(LTO_CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(out_dir)/%.$(asm_suffix): % | $(out_dir)
	$(CXX) $(CFLAGS) $(CXXFLAGS) -masm=intel -S -o $@ $<

$(out_dir)/shaders: | $(out_dir)
	mkdir -p $@

define GLSL_EXT
$(out_dir)/shaders/%.$1.h: shaders/%.$1 | $(out_dir)/shaders
	glslangValidator $(GLSL_FLAGS) --variable-name $$(subst .,_,$$(notdir $$<)) -o $$@ $$<
endef

$(foreach ext, vert frag, $(eval $(call GLSL_EXT,$(ext))))

$(call OBJ_FROM_SRC, minivulkan.cpp) $(out_dir)/minivulkan.cpp.$(asm_suffix): $(addprefix $(out_dir)/,$(addsuffix .h,$(shader_files)))
$(call OBJ_FROM_SRC, minivulkan.cpp) $(out_dir)/minivulkan.cpp.$(asm_suffix): CFLAGS += -I$(out_dir)/shaders

dep_files = $(addprefix $(out_dir)/, $(addsuffix .d, $(basename $(notdir $(src_files)))))

-include $(dep_files)
