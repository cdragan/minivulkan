
#include <vulkan/vulkan.h>

#define VK_LIB_FUNCTIONS \
    X(vkGetInstanceProcAddr) \
    X(vkGetDeviceProcAddr)

#define VK_GLOBAL_FUNCTIONS \
    X(vkCreateInstance)

#define VK_INSTANCE_FUNCTIONS \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkCreateDevice)

#define VK_DEVICE_FUNCTIONS \
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

#define vkGetInstanceProcAddr           ((PFN_vkGetInstanceProcAddr)          vk_lib_functions[      id_vkGetInstanceProcAddr])
#define vkGetDeviceProcAddr             ((PFN_vkGetDeviceProcAddr)            vk_lib_functions[      id_vkGetDeviceProcAddr])

#define vkCreateInstance                ((PFN_vkCreateInstance)               vk_global_functions[   id_vkCreateInstance])

#define vkEnumeratePhysicalDevices      ((PFN_vkEnumeratePhysicalDevices)     vk_instance_functions[ id_vkEnumeratePhysicalDevices])
#define vkGetPhysicalDeviceProperties2  ((PFN_vkGetPhysicalDeviceProperties2) vk_instance_functions[ id_vkGetPhysicalDeviceProperties2])
#define vkCreateDevice                  ((PFN_vkCreateDevice)                 vk_instance_functions[ id_vkCreateDevice])

#define vkCreateCommandPool             ((PFN_vkCreateCommandPool)            vk_device_functions[   id_vkCreateCommandPool])
