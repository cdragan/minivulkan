
##############################################################################
# Sources

src_files += minivulkan.cpp

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
    CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wunused
    CFLAGS += -Wshadow -Wformat=2 -Wconversion -Wdouble-promotion

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

##############################################################################
# Directory where generated files are stored

out_dir = Out

##############################################################################
# Function for converting source path to object file path

o_suffix = o

asm_suffix = S

OBJ_FROM_SRC = $(addsuffix .$(o_suffix), $(addprefix $(out_dir)/,$(basename $(notdir $1))))

##############################################################################
# Rules

default: $(out_dir)/minivulkan

ifeq ($(UNAME), Darwin)

    macos_app_dir = $(out_dir)/minivulkan.app/Contents/MacOS

    macos_install_files = Info.plist $(out_dir)/minivulkan

    macos_target_files = $(addprefix $(macos_app_dir)/, $(notdir $(macos_install_files)))

$(macos_app_dir):
	mkdir -p $@

$(macos_target_files): $(macos_install_files) | $(macos_app_dir)
	cp $^ $(macos_app_dir)

default: $(macos_target_files)
endif

clean:
	rm -rf $(out_dir)

$(out_dir):
	mkdir -p $(out_dir)

$(out_dir)/minivulkan: $(call OBJ_FROM_SRC, $(src_files))
	$(LINK) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

$(out_dir)/$(notdir %.$(o_suffix)): %.c | $(out_dir)
	$(CC) $(CFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(o_suffix)): %.m | $(out_dir)
	$(CC) $(CFLAGS) $(OBJCFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(o_suffix)): %.cpp | $(out_dir)
	$(CXX) $(CFLAGS) $(CXXFLAGS) -c -o $@ $<

$(out_dir)/$(notdir %.$(asm_suffix)): %.cpp | $(out_dir)
	$(CXX) $(CFLAGS) $(CXXFLAGS) -S -masm=intel -o $@ $<
