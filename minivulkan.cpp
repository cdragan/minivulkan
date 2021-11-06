#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include "window.h"

static void* load_vulkan()
{
#ifdef _WIN32
    void* const vulkan_lib = LoadLibrary("vulkan-1.dll");
#elif defined(__APPLE__)
    void* const vulkan_lib = dlopen("libvulkan.dylib", RTLD_LAZY | RTLD_LOCAL);
#else
    void* const vulkan_lib = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
#endif

    return vulkan_lib;
}

int main()
{
    Window w;

    if (create_window(&w))
        return EXIT_FAILURE;

    if ( ! load_vulkan())
        return EXIT_FAILURE;

    return event_loop(&w);
}
