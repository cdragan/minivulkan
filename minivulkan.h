#include "vulkan_functions.h"

extern VkInstance   vk_instance;
extern VkSurfaceKHR vk_surface;

struct Window;

#if defined(__APPLE__) && defined(__cplusplus)
extern "C"
#endif
bool init_vulkan(struct Window* w);

#if defined(__APPLE__) && defined(__cplusplus)
extern "C"
#endif
bool create_surface(struct Window* w);
