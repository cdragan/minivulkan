#include "vulkan_functions.h"

extern VkInstance   vk_instance;
extern VkSurfaceKHR vk_surface;

struct Window;

#if defined(__APPLE__) && defined(__cplusplus)
#   define PORTABLE extern "C"
#else
#   define PORTABLE
#endif

PORTABLE bool init_vulkan(struct Window* w);
PORTABLE bool create_surface(struct Window* w);
PORTABLE bool draw_frame();
