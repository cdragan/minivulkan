#ifdef __APPLE__
#   define VK_USE_PLATFORM_METAL_EXT
#endif

#ifdef __linux__
#   define VK_USE_PLATFORM_XCB_KHR
#endif

#ifdef _WIN32
#   define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>

#define VK_LIB_FUNCTIONS \
    X(vkGetInstanceProcAddr) \
    X(vkGetDeviceProcAddr)

#define VK_GLOBAL_FUNCTIONS \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkCreateInstance)

#define VK_INSTANCE_FUNCTIONS_BASE \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice)

#ifdef __APPLE__
#   define VK_INSTANCE_FUNCTIONS VK_INSTANCE_FUNCTIONS_BASE \
    X(vkCreateMetalSurfaceEXT)
#endif

#ifdef __linux__
#   define VK_INSTANCE_FUNCTIONS VK_INSTANCE_FUNCTIONS_BASE \
    X(vkCreateXcbSurfaceKHR)
#endif

#ifdef _WIN32
#   define VK_INSTANCE_FUNCTIONS VK_INSTANCE_FUNCTIONS_BASE \
    X(vkCreateWin32SurfaceKHR)
#endif

#define VK_DEVICE_FUNCTIONS \
    X(vkGetDeviceQueue) \
    X(vkCreateSwapchainKHR) \
    X(vkCreateCommandPool)

extern PFN_vkVoidFunction vk_lib_functions[];
extern PFN_vkVoidFunction vk_global_functions[];
extern PFN_vkVoidFunction vk_instance_functions[];
extern PFN_vkVoidFunction vk_device_functions[];

#define X(func) id_##func,
enum eLibFunctions {
    VK_LIB_FUNCTIONS
    id_lib_num
};

enum e_global_funcgtions {
    VK_GLOBAL_FUNCTIONS
    id_global_num
};

enum e_instance_funcgtions {
    VK_INSTANCE_FUNCTIONS
    id_instance_num
};

enum e_device_functions {
    VK_DEVICE_FUNCTIONS
    id_device_num
};
#undef X

#define vkGetInstanceProcAddr                     ((PFN_vkGetInstanceProcAddr)                     vk_lib_functions[      id_vkGetInstanceProcAddr])
#define vkGetDeviceProcAddr                       ((PFN_vkGetDeviceProcAddr)                       vk_lib_functions[      id_vkGetDeviceProcAddr])

#define vkEnumerateInstanceExtensionProperties    ((PFN_vkEnumerateInstanceExtensionProperties)    vk_global_functions[   id_vkEnumerateInstanceExtensionProperties])
#define vkCreateInstance                          ((PFN_vkCreateInstance)                          vk_global_functions[   id_vkCreateInstance])

#define vkEnumeratePhysicalDevices                ((PFN_vkEnumeratePhysicalDevices)                vk_instance_functions[ id_vkEnumeratePhysicalDevices])
#define vkGetPhysicalDeviceProperties2            ((PFN_vkGetPhysicalDeviceProperties2)            vk_instance_functions[ id_vkGetPhysicalDeviceProperties2])
#define vkGetPhysicalDeviceQueueFamilyProperties  ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)  vk_instance_functions[ id_vkGetPhysicalDeviceQueueFamilyProperties])
#define vkGetPhysicalDeviceSurfaceSupportKHR      ((PFN_vkGetPhysicalDeviceSurfaceSupportKHR)      vk_instance_functions[ id_vkGetPhysicalDeviceSurfaceSupportKHR])
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR ((PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) vk_instance_functions[ id_vkGetPhysicalDeviceSurfaceCapabilitiesKHR])
#define vkGetPhysicalDeviceSurfaceFormatsKHR      ((PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)      vk_instance_functions[ id_vkGetPhysicalDeviceSurfaceFormatsKHR])
#define vkEnumerateDeviceExtensionProperties      ((PFN_vkEnumerateDeviceExtensionProperties)      vk_instance_functions[ id_vkEnumerateDeviceExtensionProperties])
#define vkCreateMetalSurfaceEXT                   ((PFN_vkCreateMetalSurfaceEXT)                   vk_instance_functions[ id_vkCreateMetalSurfaceEXT])
#define vkCreateXcbSurfaceKHR                     ((PFN_vkCreateXcbSurfaceKHR)                     vk_instance_functions[ id_vkCreateXcbSurfaceKHR])
#define vkCreateWin32SurfaceKHR                   ((PFN_vkCreateWin32SurfaceKHR)                   vk_instance_functions[ id_vkCreateWin32SurfaceKHR])
#define vkCreateDevice                            ((PFN_vkCreateDevice)                            vk_instance_functions[ id_vkCreateDevice])

#define vkGetDeviceQueue                          ((PFN_vkGetDeviceQueue)                          vk_device_functions[   id_vkGetDeviceQueue])
#define vkCreateSwapchainKHR                      ((PFN_vkCreateSwapchainKHR)                      vk_instance_functions[ id_vkCreateSwapchainKHR])
#define vkCreateCommandPool                       ((PFN_vkCreateCommandPool)                       vk_device_functions[   id_vkCreateCommandPool])
