# Minimal Vulkan app example

The goal of this project is to minimize exe size.

Ideas which contribute to reduced executable size:

* Do not perform heap allocations.  Use static buffers.  Managing memory increases
  code size and encourages writing sloppy code.  In many cases it is possible to
  write perfectly good programs without using heap allocations.
* Do not free resources on exit.  The OS and the Vulkan lib tidy up all resources
  anyway.  (Some resources need to be freed to avoid running out of them, such
  as reallocated swapchain images during window resize, etc.)
* Allocate structures and arrays in static store (data segment) when possible.
  Typically Vulkan structures take advantage of this approach, which avoids
  generating extra code for filling out these structures if they were on the stack.
* Rearrange SPIR-V bytecode to make it more friendly to executable compressors.
  This increases the executable size in the first place, but the SPIR-V bytecode
  is laid out in such a way that the executable becomes even smaller after compression.
* (Windows) Do not depend on MS C runtime.  C library on Windows is linked statically
  into the executable and causes bloat.

## Building from sources

### Prerequisites

* On Windows, use e.g. MSYS2 or Cygwin to get GNU make.  The Makefile still uses MSVC,
  so make sure you run `vcvarsall.bat x64` (or `x86`) before entering MSYS2 environment.
* Install Vulkan SDK.

### Compiling

Use GNU make to compile the sources.

Additional variables which can be passed to `make` as arguments:

* `debug=1` Enables debug build.
* `VULKAN_SDK=path` Path to Vulkan SDK, in case it's not installed where compiler can find it.
  This is used for includes.
* `VULKAN_SDK_BIN=path` Path to Vulkan SDK bin dir, where `glslangValidator` is.  This is
  necessary if Vulkan SDK is unpacked in a directory and not fully installed in the system.
* `stdlib=1` (Windows-only) Enables linking against MSVCRT, required for GUI apps.
