#include <assert.h>
#include <dlfcn.h>
#include "window.h"
#include "vulkan_functions.h"
#include "stdc.h"

#ifdef NDEBUG
#   define dprintf(...)
#else
#   include <stdio.h>
#   define dprintf printf
#endif

static void* vulkan_lib = nullptr;

static bool load_vulkan()
{
#ifdef _WIN32
    vulkan_lib = LoadLibrary("vulkan-1.dll");
#elif defined(__APPLE__)
    vulkan_lib = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif

    return !! vulkan_lib;
}

PFN_vkVoidFunction vk_lib_functions[      id_lib_num];
PFN_vkVoidFunction vk_global_functions[   id_global_num];
PFN_vkVoidFunction vk_instance_functions[ id_instance_num];
PFN_vkVoidFunction vk_device_functions[   id_device_num];

#define X(func) #func "\0"
static const char vk_lib_function_names[]      = VK_LIB_FUNCTIONS;
static const char vk_global_function_names[]   = VK_GLOBAL_FUNCTIONS;
static const char vk_instance_function_names[] = VK_INSTANCE_FUNCTIONS;
static const char vk_device_function_names[]   = VK_DEVICE_FUNCTIONS;
#undef X

typedef PFN_vkVoidFunction (*LOAD_FUNCTION)(const char* name);

static bool load_functions(const char* names, PFN_vkVoidFunction* fn_ptrs, LOAD_FUNCTION load)
{
    for (;;) {
        const uint32_t len = strlen(names);

        if ( ! len)
            break;

        const PFN_vkVoidFunction fn = load(names);

        if ( ! fn) {
            dprintf("Failed to load %s\n", names);
            return false;
        }

        *fn_ptrs = fn;

        ++fn_ptrs;
        names += len + 1;
    }

    return true;
}

static bool load_lib_functions()
{
    return load_functions(vk_lib_function_names, vk_lib_functions,
            [](const char* name) -> PFN_vkVoidFunction
            {
                return reinterpret_cast<PFN_vkVoidFunction>(dlsym(vulkan_lib, name));
            });
}

static bool load_global_functions()
{
    return load_functions(vk_global_function_names, vk_global_functions,
            [](const char* name) -> PFN_vkVoidFunction
            {
                return vkGetInstanceProcAddr(nullptr, name);
            });
}

static VkInstance vk_instance = VK_NULL_HANDLE;

static bool load_instance_functions()
{
    return load_functions(vk_instance_function_names, vk_instance_functions,
            [](const char* name) -> PFN_vkVoidFunction
            {
                return vkGetInstanceProcAddr(vk_instance, name);
            });
}

static bool init_instance()
{
    static const VkApplicationInfo app_info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        nullptr,
        0,
        nullptr,
        0,
        VK_API_VERSION_1_2
    };

    static VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        0,
        &app_info
    };

#ifndef NDEBUG
    const PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties");

    static VkLayerProperties layer_props[8];

    uint32_t num_layer_props = 0;

    if (vkEnumerateInstanceLayerProperties) {
        num_layer_props = sizeof(layer_props) / sizeof(layer_props[0]);

        const VkResult res = vkEnumerateInstanceLayerProperties(&num_layer_props, layer_props);
        if (res != VK_SUCCESS && res != VK_INCOMPLETE)
            num_layer_props = 0;
    }

    const PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties");

    const char* validation_str          = nullptr;
    const char* validation_features_str = nullptr;

    VkValidationFeaturesEXT validation_features = {
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        nullptr,
    };

    if (vkEnumerateInstanceExtensionProperties) {

        for (uint32_t i = 0; i < num_layer_props; i++) {
            dprintf("layer: %s\n", layer_props[i].layerName);

            static VkExtensionProperties ext_props[32];

            uint32_t num_ext_props = sizeof(ext_props) / sizeof(ext_props[0]);

            const VkResult res = vkEnumerateInstanceExtensionProperties(layer_props[i].layerName,
                                                                        &num_ext_props,
                                                                        ext_props);
            if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
                for (uint32_t j = 0; j < num_ext_props; j++) {
                    dprintf("    %s\n", ext_props[j].extensionName);

                    static const char validation_features_ext[] = "VK_EXT_validation_features";
                    if (strcmp(ext_props[i].extensionName, validation_features_ext) == 0)
                        validation_features_str = validation_features_ext;
                }
            }

            static const char validation[] = "VK_LAYER_KHRONOS_validation";
            if (strcmp(layer_props[i].layerName, validation) == 0) {
                validation_str = validation;
                instance_info.ppEnabledLayerNames = &validation_str;
                instance_info.enabledLayerCount   = 1;

                if (validation_features_str) {
                    instance_info.ppEnabledExtensionNames = &validation_features_str;
                    instance_info.enabledExtensionCount   = 1;
                    instance_info.pNext                   = &validation_features;
                }
            }
        }
    }
#endif

    const VkResult res = vkCreateInstance(&instance_info, nullptr, &vk_instance);

    if (res != VK_SUCCESS)
        return false;

    return load_instance_functions();
}

static VkPhysicalDevice vk_phys_dev = VK_NULL_HANDLE;

static VkPhysicalDeviceVulkan12Properties vk12_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
    nullptr
};

static VkPhysicalDeviceVulkan11Properties vk11_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
    &vk12_props
};

static VkPhysicalDeviceProperties2 phys_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    &vk11_props
};

static bool find_gpu()
{
    VkPhysicalDevice phys_devices[16];

    uint32_t count = sizeof(phys_devices) / sizeof(phys_devices[0]);

    const VkResult res = vkEnumeratePhysicalDevices(vk_instance, &count, phys_devices);

    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

    if ( ! count)
        return false;

    const VkPhysicalDeviceType seek_types[] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    };

    for (uint32_t i_type = 0; i_type < sizeof(seek_types) / sizeof(seek_types[0]); i_type++) {

        const VkPhysicalDeviceType type = seek_types[i_type];

        for (uint32_t i_dev = 0; i_dev < count; i_dev++) {
            vkGetPhysicalDeviceProperties2(phys_devices[i_dev],
                                           &phys_props);

            if (phys_props.properties.deviceType == type) {
                vk_phys_dev = phys_devices[i_dev];
                dprintf("Selected device %u: %s\n", i_dev, phys_props.properties.deviceName);
                return true;
            }
        }
    }

    return false;
}

static bool create_device()
{
    if ( ! find_gpu())
        return false;

    return true;
}

int main()
{
    Window w;

    if ( ! create_window(&w))
        return 1;

    if ( ! load_vulkan())
        return 1;

    if ( ! load_lib_functions())
        return 1;

    if ( ! load_global_functions())
        return 1;

    if ( ! init_instance())
        return 1;

    if ( ! create_device())
        return 1;

    return event_loop(&w);
}
