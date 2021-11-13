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

#ifdef NDEBUG
#   define CHK(call) call
#   define CHK_RES(call_str, res)
#else
#   define CHK(call) check_vk_call(#call, __FILE__, __LINE__, call)
#   define CHK_RES(call_str, res) check_vk_call(call_str, __FILE__, __LINE__, res)
VkResult check_vk_call(const char* call_str, const char* file, int line, VkResult res);
#endif
