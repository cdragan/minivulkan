Minimal Vulkan app example

The goal is to minimize exe size.

As part of minimizing exe size, there are dependencies on standard library and
there are no heap allocations.  Some objects may need to be static in order to
allocate them in the data segment, to reduce amount of code for filling up
structures.

On exit nothing is closed or freed, let the OS and the Vulkan lib to tidy up.
