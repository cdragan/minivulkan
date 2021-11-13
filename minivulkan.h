#include "vulkan_functions.h"

#define APPNAME "minivulkan"

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

#ifdef NDEBUG
#   define CHK(call) call
#else
#   define CHK(call) check_vk_call(#call, __FILE__, __LINE__, call)
VkResult check_vk_call(const char* call_str, const char* file, int line, VkResult res);
#endif
