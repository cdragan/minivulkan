
##############################################################################
# Sources

src_files += minivulkan.cpp
src_files += stdc.cpp

##############################################################################
# Determine target OS

UNAME = $(shell uname -s)

ifneq (,$(filter CYGWIN% MINGW% MSYS%, $(UNAME)))
    # Note: Still use cl.exe on Windows
    UNAME = Windows
endif

##############################################################################
# Compiler flags

ifneq ($(UNAME), Windows)
    CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused -Wno-missing-field-initializers
    CFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion

    CFLAGS += -fvisibility=hidden
    CFLAGS += -fPIC

    CXXFLAGS += -x c++ -std=c++17 -fno-rtti -fno-exceptions

    OBJCFLAGS += -x objective-c

    LINK = $(CXX)
endif

ifeq ($(UNAME), Linux)
    src_files += window_linux.m

    LDFLAGS += -lxcb -ldl

    ifdef debug
        STRIP = true
        CFLAGS += -Og -g
    else
        STRIP = strip
        CFLAGS += -DNDEBUG -Os
        CFLAGS += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
        LDFLAGS += -Wl,--gc-sections -Wl,--as-needed
        CFLAGS += -flto -fno-fat-lto-objects
        LDFLAGS += -flto=auto -fuse-linker-plugin
    endif
endif

ifeq ($(UNAME), Darwin)
    src_files += window_macos.m

    frameworks += Cocoa
    frameworks += CoreVideo
    frameworks += Quartz

    LDFLAGS += -fobjc-arc $(addprefix -framework ,$(frameworks))

    ifdef debug
        STRIP = true
        CFLAGS += -Og -g
    else
        STRIP = strip -x
        CFLAGS += -DNDEBUG -Os
        CFLAGS += -ffunction-sections -fdata-sections
        LDFLAGS += -ffunction-sections -fdata-sections
        LDFLAGS += -Wl,-dead_strip
        CFLAGS += -flto
        LDFLAGS += -flto
    endif
endif

ifdef VULKAN_SDK
    CFLAGS += -I$(VULKAN_SDK)/include
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

$(out_dir):
	mkdir -p $(out_dir)

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

$(out_dir)/$(notdir %.$(o_suffix)): %.c | $(out_dir)
	$(CC) $(CFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(o_suffix)): %.m | $(out_dir)
	$(CC) $(CFLAGS) $(OBJCFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(o_suffix)): %.cpp | $(out_dir)
	$(CXX) $(CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(asm_suffix)): %.cpp | $(out_dir)
	$(CXX) $(CFLAGS) $(CXXFLAGS) -S -masm=intel -o $@ $<
