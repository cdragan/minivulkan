// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "minivulkan.h"

#include "d_printf.h"
#include "gui.h"
#include "memory_heap.h"
#include "mstdc.h"
#include "resource.h"
#include "vmath.h"
#include "vulkan_extensions.h"

#ifndef NDEBUG
#   include <stdlib.h>
#endif
#if defined(NDEBUG) && defined(TIME_STATS)
#   if !defined(_WIN32) || !defined(NOSTDLIB)
#       include <stdio.h>
#   else
#       define printf wsprintfA
#   endif
#endif

#ifdef _WIN32
#   define dlsym GetProcAddress
#else
#   include <dlfcn.h>
#endif

// Workaround Windows headers
#ifdef OPTIONAL
#   undef OPTIONAL
#endif

#ifndef NDEBUG
VkResult check_vk_call(const char* call_str, const char* file, int line, VkResult res)
{
    if (res != VK_SUCCESS) {
        const char* desc = nullptr;

        switch (res) {
#           define MAKE_ERR_STR(val) case val: desc = #val; break;
            MAKE_ERR_STR(VK_NOT_READY)
            MAKE_ERR_STR(VK_TIMEOUT)
            MAKE_ERR_STR(VK_EVENT_SET)
            MAKE_ERR_STR(VK_EVENT_RESET)
            MAKE_ERR_STR(VK_INCOMPLETE)
            MAKE_ERR_STR(VK_ERROR_OUT_OF_HOST_MEMORY)
            MAKE_ERR_STR(VK_ERROR_OUT_OF_DEVICE_MEMORY)
            MAKE_ERR_STR(VK_ERROR_INITIALIZATION_FAILED)
            MAKE_ERR_STR(VK_ERROR_DEVICE_LOST)
            MAKE_ERR_STR(VK_ERROR_MEMORY_MAP_FAILED)
            MAKE_ERR_STR(VK_ERROR_LAYER_NOT_PRESENT)
            MAKE_ERR_STR(VK_ERROR_EXTENSION_NOT_PRESENT)
            MAKE_ERR_STR(VK_ERROR_FEATURE_NOT_PRESENT)
            MAKE_ERR_STR(VK_ERROR_INCOMPATIBLE_DRIVER)
            MAKE_ERR_STR(VK_ERROR_TOO_MANY_OBJECTS)
            MAKE_ERR_STR(VK_ERROR_FORMAT_NOT_SUPPORTED)
            MAKE_ERR_STR(VK_ERROR_FRAGMENTED_POOL)
            MAKE_ERR_STR(VK_ERROR_UNKNOWN)
            MAKE_ERR_STR(VK_ERROR_OUT_OF_POOL_MEMORY)
            MAKE_ERR_STR(VK_ERROR_INVALID_EXTERNAL_HANDLE)
            MAKE_ERR_STR(VK_ERROR_FRAGMENTATION)
            MAKE_ERR_STR(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
            MAKE_ERR_STR(VK_ERROR_SURFACE_LOST_KHR)
            MAKE_ERR_STR(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
            MAKE_ERR_STR(VK_ERROR_OUT_OF_DATE_KHR)
#           undef MAKE_ERR_STR
            case VK_SUBOPTIMAL_KHR: return res;
            default: break;
        }

        if (desc)
            d_printf("%s:%d: %s in %s\n", file, line, desc, call_str);
        else
            d_printf("%s:%d: error %d in %s\n", file, line, static_cast<int>(res), call_str);
    }

    return res;
}

const char* format_string(VkFormat format)
{
    switch (format) {
#define MAKE_STR(fmt) case fmt: return #fmt ;
        MAKE_STR(VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        MAKE_STR(VK_FORMAT_A2R10G10B10_UNORM_PACK32)
        MAKE_STR(VK_FORMAT_R16G16B16A16_SFLOAT)
        MAKE_STR(VK_FORMAT_R16G16B16A16_UNORM)
        MAKE_STR(VK_FORMAT_A8B8G8R8_UNORM_PACK32)
        MAKE_STR(VK_FORMAT_B8G8R8A8_UNORM)
        MAKE_STR(VK_FORMAT_R8G8B8A8_UNORM)
        MAKE_STR(VK_FORMAT_B8G8R8_UNORM)
        MAKE_STR(VK_FORMAT_R8G8B8_UNORM)
        MAKE_STR(VK_FORMAT_B8G8R8A8_SRGB)
        default: break;
    }
    return "unrecognized";
}
#endif

#ifdef _WIN32
static HMODULE vulkan_lib = nullptr;
#else
static void* vulkan_lib = nullptr;
#endif

static bool load_vulkan()
{
#ifdef _WIN32
    static const char lib_name[] = "vulkan-1.dll";
#elif defined(__APPLE__)
    static const char lib_name[] = "/usr/local/lib/libvulkan.1.dylib";
#elif defined(__linux__)
    static const char lib_name[] = "libvulkan.so.1";
#endif

#ifdef _WIN32
    vulkan_lib = LoadLibraryEx(lib_name, nullptr, 0);
#elif defined(__APPLE__) || defined(__linux__)
    vulkan_lib = dlopen(lib_name, RTLD_NOW | RTLD_LOCAL);
#endif

    d_printf("%s %s\n", vulkan_lib ? "Loaded" : "Failed to load", lib_name);

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
        if ( ! *names)
            break;

        const PFN_vkVoidFunction fn = load(names);

        if ( ! fn) {
            d_printf("Failed to load %s\n", names);
            return false;
        }

        *fn_ptrs = fn;

        ++fn_ptrs;
        names += mstd::strlen(names) + 1;
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

static bool enable_extensions(const char*                  supported_extensions,
                              const VkExtensionProperties* found_extensions,
                              uint32_t                     num_found_extensions,
                              const char**                 enabled_extensions,
                              uint32_t*                    num_enabled_extensions)
{
    for (;;) {
        const uint32_t len = mstd::strlen(supported_extensions);

        if ( ! len)
            break;

        const char req = *(supported_extensions++);

        bool found = false;
        for (uint32_t i = 0; i < num_found_extensions; i++) {
            if (mstd::strcmp(supported_extensions, found_extensions[i].extensionName) == 0) {
                found = true;
                break;
            }
        }

        if (found) {
            enabled_extensions[*num_enabled_extensions] = supported_extensions;
            d_printf("Enable extension %s\n", supported_extensions);
            ++*num_enabled_extensions;
        }
        else if (req == '1') {
            d_printf("Required extension %s not found\n", supported_extensions);
            return false;
        }

        supported_extensions += len;
    }

    return true;
}

VkInstance vk_instance = VK_NULL_HANDLE;

static bool load_instance_functions()
{
    return load_functions(vk_instance_function_names, vk_instance_functions,
            [](const char* name) -> PFN_vkVoidFunction
            {
                return vkGetInstanceProcAddr(vk_instance, name);
            });
}

#ifndef NDEBUG
static bool print_extensions()
{
#ifdef _WIN32
    return GetEnvironmentVariable("EXTENSIONS", nullptr, 0) != 0;
#else
    const char *ext = getenv("EXTENSIONS");
    return ext != nullptr;
#endif
}
#endif

static bool init_instance()
{
    VkResult res;

#ifndef NDEBUG
    uint32_t api_version = 0;

    res = CHK(vkEnumerateInstanceVersion(&api_version));
    if (res != VK_SUCCESS)
        return false;

    d_printf("Vulkan version %u.%u.%u\n",
             VK_VERSION_MAJOR(api_version),
             VK_VERSION_MINOR(api_version),
             VK_VERSION_PATCH(api_version));
#endif

    static const VkApplicationInfo app_info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
        nullptr,
        nullptr,
        0,
        nullptr,
        0,
        VK_API_VERSION_1_3
    };

    static const char* enabled_instance_extensions[16];

    static VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
#if defined(__APPLE__) && defined(VK_KHR_portability_enumeration)
        VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#else
        0,
#endif
        &app_info,
        0,
        nullptr,
        0,
        enabled_instance_extensions
    };

    VkExtensionProperties ext_props[32];
    uint32_t              num_ext_props = mstd::array_size(ext_props);

    res = CHK(vkEnumerateInstanceExtensionProperties(nullptr, &num_ext_props, ext_props));
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

#ifndef NDEBUG
    const bool do_print = print_extensions();
    if (do_print) {
        if (num_ext_props)
            d_printf("Instance extensions:\n");
        for (uint32_t i = 0; i < num_ext_props; i++)
            d_printf("    %s\n", ext_props[i].extensionName);
    }
    else
        d_printf("Tip: Set EXTENSIONS env var to print all available extensions and layers\n");
#endif

    // List of extensions declared in vulkan_extensions.h
#define REQUIRED "1"
#define OPTIONAL "0"
#define X(ext, req) req #ext "\0"

    static const char supported_instance_extensions[] = SUPPORTED_INSTANCE_EXTENSIONS;

#undef REQUIRED
#undef OPTIONAL
#undef X

    if ( ! enable_extensions(supported_instance_extensions,
                             ext_props,
                             num_ext_props,
                             enabled_instance_extensions,
                             &instance_info.enabledExtensionCount))
        return false;

#ifndef NDEBUG
    {
        const PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties =
            reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
                vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));

        static VkLayerProperties layer_props[8];

        uint32_t num_layer_props = 0;

        if (vkEnumerateInstanceLayerProperties) {
            num_layer_props = mstd::array_size(layer_props);

            res = vkEnumerateInstanceLayerProperties(&num_layer_props, layer_props);
            if (res != VK_SUCCESS && res != VK_INCOMPLETE)
                num_layer_props = 0;
        }

        const char* validation_str = nullptr;

        static VkValidationFeatureEnableEXT enabled_features[] = {
            VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
        };

        static VkValidationFeaturesEXT validation_features = {
            VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
            nullptr,
            mstd::array_size(enabled_features),
            enabled_features
        };

        for (uint32_t i = 0; i < num_layer_props; i++) {
            if (do_print)
                d_printf("Layer: %s\n", layer_props[i].layerName);

            num_ext_props = mstd::array_size(ext_props);

            const char* validation_features_str = nullptr;

            res = vkEnumerateInstanceExtensionProperties(layer_props[i].layerName,
                                                         &num_ext_props,
                                                         ext_props);
            if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
                for (uint32_t j = 0; j < num_ext_props; j++) {
                    if (do_print)
                        d_printf("    %s\n", ext_props[j].extensionName);

                    static const char validation_features_ext[] = "VK_EXT_validation_features";
                    if (mstd::strcmp(ext_props[j].extensionName, validation_features_ext) == 0)
                        validation_features_str = validation_features_ext;
                }
            }

            static const char validation[] = "VK_LAYER_KHRONOS_validation";
            if (mstd::strcmp(layer_props[i].layerName, validation) == 0) {
                validation_str = validation;
                instance_info.ppEnabledLayerNames = &validation_str;
                instance_info.enabledLayerCount   = 1;

                if (validation_features_str &&
                    instance_info.enabledExtensionCount < mstd::array_size(enabled_instance_extensions)) {

                    enabled_instance_extensions[instance_info.enabledExtensionCount] = validation_features_str;
                    ++instance_info.enabledExtensionCount;
                    d_printf("Enable extension %s\n", validation_features_str);

                    instance_info.pNext = &validation_features;

                    d_printf("Enable layer %s\n", layer_props[i].layerName);
                }
            }
        }
    }
#endif

    res = CHK(vkCreateInstance(&instance_info, nullptr, &vk_instance));

    if (res != VK_SUCCESS)
        return false;

    return load_instance_functions();
}

VkSurfaceKHR vk_surface = VK_NULL_HANDLE;

VkPhysicalDevice vk_phys_dev = VK_NULL_HANDLE;

static VkPhysicalDeviceVulkan12Properties vk12_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
    nullptr
};

static VkPhysicalDeviceVulkan11Properties vk11_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
    &vk12_props
};

VkPhysicalDeviceProperties2 vk_phys_props = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    &vk11_props
};

static const float queue_priorities[] = { 1 };

static constexpr uint32_t no_queue_family = ~0u;

uint32_t vk_queue_family_index = no_queue_family;

VkSwapchainCreateInfoKHR swapchain_create_info = {
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    nullptr,
    0,
    VK_NULL_HANDLE,
    0,
    VK_FORMAT_UNDEFINED,
    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    {},
    1,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    VK_SHARING_MODE_EXCLUSIVE,
    0,
    nullptr,
    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    VK_PRESENT_MODE_FIFO_KHR,
    VK_FALSE,
    VK_NULL_HANDLE
};

static bool find_surface_format(VkPhysicalDevice phys_dev)
{
    VkSurfaceFormatKHR formats[64];

    uint32_t num_formats = mstd::array_size(formats);

    const VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev,
                                                              vk_surface,
                                                              &num_formats,
                                                              formats);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

    static const uint8_t preferred_output_formats[] = {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM
    };

    for (uint32_t i_pref = 0; i_pref < mstd::array_size(preferred_output_formats); i_pref++) {

        const VkFormat pref_format = static_cast<VkFormat>(preferred_output_formats[i_pref]);

        for (uint32_t i_cur = 0; i_cur < num_formats; i_cur++) {
            if (formats[i_cur].format == pref_format) {
                swapchain_create_info.surface         = vk_surface;
                swapchain_create_info.imageFormat     = pref_format;
                swapchain_create_info.imageColorSpace = formats[i_cur].colorSpace;
                d_printf("Found surface format %s\n", format_string(pref_format));
                return true;
            }
        }
    }

    return false;
}

bool find_optimal_tiling_format(const VkFormat* preferred_formats,
                                uint32_t        num_preferred_formats,
                                uint32_t        format_feature_flags,
                                VkFormat*       out_format)
{
    for (uint32_t i = 0; i < num_preferred_formats; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(vk_phys_dev, preferred_formats[i], &props);

        if ((props.optimalTilingFeatures & format_feature_flags) == format_feature_flags) {
            *out_format = preferred_formats[i];
            return true;
        }
    }

    return false;
}

static bool find_gpu()
{
    VkPhysicalDevice        phys_devices[8];
    VkQueueFamilyProperties queues[8];

    uint32_t count = mstd::array_size(phys_devices);

    VkResult res = CHK(vkEnumeratePhysicalDevices(vk_instance, &count, phys_devices));

    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

    if ( ! count) {
        d_printf("Found 0 physical devices\n");
        return false;
    }

    static const VkPhysicalDeviceType seek_types[] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    };

    for (uint32_t i_type = 0; i_type < mstd::array_size(seek_types); i_type++) {

        const VkPhysicalDeviceType type = seek_types[i_type];

        for (uint32_t i_dev = 0; i_dev < count; i_dev++) {
            const VkPhysicalDevice phys_dev = phys_devices[i_dev];

            vkGetPhysicalDeviceProperties2(phys_dev, &vk_phys_props);

            if (vk_phys_props.properties.deviceType != type)
                continue;

            if ( ! find_surface_format(phys_dev))
                continue;

            uint32_t num_queues = mstd::array_size(queues);
            vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &num_queues, queues);

            uint32_t i_queue;

            for (i_queue = 0; i_queue < num_queues; i_queue++) {

                if ( ! (queues[i_queue].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    continue;

                VkBool32 supported = VK_FALSE;

                res = vkGetPhysicalDeviceSurfaceSupportKHR(phys_dev, i_queue, vk_surface, &supported);

                if (res != VK_SUCCESS || ! supported)
                    continue;

                vk_queue_family_index = i_queue;
                break;
            }

            if (i_queue == num_queues)
                continue;

            vk_phys_dev = phys_devices[i_dev];
            d_printf("Selected device %u: %s, supports Vulkan %u.%u\n",
                     i_dev,
                     vk_phys_props.properties.deviceName,
                     VK_VERSION_MAJOR(vk_phys_props.properties.apiVersion),
                     VK_VERSION_MINOR(vk_phys_props.properties.apiVersion));
            return true;
        }
    }

    d_printf("Could not find any usable GPUs\n");
    return false;
}

VkDevice           vk_dev                   = VK_NULL_HANDLE;
VkQueue            vk_queue                 = VK_NULL_HANDLE;
static const char* vk_device_extensions[16];
static uint32_t    vk_num_device_extensions = 0;

static bool get_device_extensions()
{
    VkExtensionProperties extensions[256];
    uint32_t              num_extensions = mstd::array_size(extensions);

    const VkResult res = CHK(vkEnumerateDeviceExtensionProperties(vk_phys_dev,
                                                                  nullptr,
                                                                  &num_extensions,
                                                                  extensions));
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

#ifndef NDEBUG
    if (print_extensions()) {
        for (uint32_t i = 0; i < num_extensions; i++)
            d_printf("    %s\n", extensions[i].extensionName);
    }
#endif

    // List of extensions declared in vulkan_extensions.h
#define REQUIRED "1"
#define OPTIONAL "0"
#define X(ext, req) req #ext "\0"

    static const char supported_device_extensions[] = SUPPORTED_DEVICE_EXTENSIONS;

#undef REQUIRED
#undef OPTIONAL
#undef X

    return enable_extensions(supported_device_extensions,
                             extensions,
                             num_extensions,
                             vk_device_extensions,
                             &vk_num_device_extensions);
}

static bool load_device_functions()
{
    return load_functions(vk_device_function_names, vk_device_functions,
            [](const char* name) -> PFN_vkVoidFunction
            {
                return vkGetDeviceProcAddr(vk_dev, name);
            });
}

#define X(set, prev, type, tag) type vk##set = { tag, prev };
FEATURE_SETS
#undef X

#ifdef NDEBUG
uint32_t check_feature(const VkBool32* feature)
{
    return ! *feature;
}

static bool check_device_features_internal()
{
    return ! check_device_features();
}

#else

#define X(set, prev, type, tag) static type init##set = { tag };
FEATURE_SETS
#undef X

static void set_feature(const VkBool32* feature, const void* device_set, void* init_set, size_t set_size)
{
    const uintptr_t feature_ptr = reinterpret_cast<uintptr_t>(feature);
    const uintptr_t set_begin   = reinterpret_cast<uintptr_t>(device_set);
    const uintptr_t set_end     = set_begin + set_size;

    if (feature_ptr > set_begin && feature_ptr < set_end) {
        const uintptr_t offset = feature_ptr - set_begin;
        const uintptr_t target = reinterpret_cast<uintptr_t>(init_set) + offset;
        VkBool32* const init_feature = reinterpret_cast<VkBool32*>(target);

        *init_feature = VK_TRUE;
    }
}

// In debug builds, check_feature() is a macro, which unfolds to check_feature_str(),
// so that we can print feature name if it isn't supported
uint32_t check_feature_str(const char* name, const VkBool32* feature)
{
    const uint32_t missing_features = ! *feature;

    if (missing_features)
        d_printf("Feature %s is not present\n", name);

    // Set the feature in the init set.  See check_device_features_internal() for details.
    #define X(set, prev, type, tag) set_feature(feature, &vk##set, &init##set, sizeof(init##set));
    FEATURE_SETS
    #undef X

    return missing_features;
}

template<typename T>
static void copy_feature_set(T* dest, const T* src, size_t)
{
    const VkStructureType s_type = dest->sType;
    void* const           p_next = dest->pNext;

    *dest = *src;

    dest->sType = s_type;
    dest->pNext = p_next;
}

static bool check_device_features_internal()
{
    if (check_device_features())
        return false;

    // vk_features and chained structures are initialized by obtaining features from the device.
    // Then check_device_features() calls check_feature_str() for each feature, which then
    // sets the feature bit in the init set.  We copy each feature set back, so we clear
    // all the unused feature bits and set all the used one.  Then the updated vk_features
    // gets used to create the device.  This way validation layers will tell us if we missed
    // to check any feature bits.
    // In release builds we just enable all feature bits supported by the device to reduce exe size.
    #define X(set, prev, type, tag) copy_feature_set(&vk##set, &init##set, sizeof(init##set));
    FEATURE_SETS
    #undef X

    return true;
}
#endif

static bool create_device()
{
    if ( ! find_gpu())
        return false;

    if ( ! get_device_extensions())
        return false;

    vkGetPhysicalDeviceFeatures2(vk_phys_dev, &vk_features);

    if ( ! check_device_features_internal())
        return false;

    static VkDeviceQueueCreateInfo queue_create_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        nullptr,
        0,
        no_queue_family, // queueFamilyIndex
        1,               // queueCount
        queue_priorities
    };

    queue_create_info.queueFamilyIndex = vk_queue_family_index;

    static VkDeviceCreateInfo dev_create_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        &vk_features,
        0,
        1,
        &queue_create_info,
        0,
        nullptr,
        0,       // enabledExtensionCount
        nullptr, // ppEnabledExtensionNames
        nullptr  // pEnabledFeatures
    };

    dev_create_info.enabledExtensionCount   = vk_num_device_extensions;
    dev_create_info.ppEnabledExtensionNames = vk_device_extensions;

    const VkResult res = CHK(vkCreateDevice(vk_phys_dev,
                                            &dev_create_info,
                                            nullptr,
                                            &vk_dev));

    if (res != VK_SUCCESS)
        return false;

    if ( ! load_device_functions())
        return false;

    vkGetDeviceQueue(vk_dev, vk_queue_family_index, 0, &vk_queue);

    return true;
}

#ifndef NDEBUG
void set_vk_object_name(VkObjectType type, uint64_t handle, Description desc)
{
    static char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "%s %u", desc.name, desc.idx);

    static VkDebugUtilsObjectNameInfoEXT object_name_info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        nullptr,
        VK_OBJECT_TYPE_UNKNOWN,
        0,
        nullptr
    };

    object_name_info.objectType   = type;
    object_name_info.objectHandle = handle;
    object_name_info.pObjectName  = (desc.idx == ~0U) ? desc.name : name_buf;

    static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT;

    if ( ! vkSetDebugUtilsObjectNameEXT) {
        vkSetDebugUtilsObjectNameEXT =
            reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(vk_dev, "vkSetDebugUtilsObjectNameEXT"));
    }

    if (vkSetDebugUtilsObjectNameEXT)
        CHK(vkSetDebugUtilsObjectNameEXT(vk_dev, &object_name_info));
}
#endif

VkSemaphore vk_sems[num_semaphores];

static bool create_semaphores()
{
    for (uint32_t i = 0; i < mstd::array_size(vk_sems); i++) {

        static const VkSemaphoreCreateInfo sem_create_info = {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
        };

        const VkResult res = CHK(vkCreateSemaphore(vk_dev, &sem_create_info, nullptr, &vk_sems[i]));

        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

VkFence vk_fens[num_fences];

static bool create_fences()
{
    for (uint32_t i = 0; i < mstd::array_size(vk_fens); i++) {

        static const VkFenceCreateInfo fence_create_info = {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
        };

        const VkResult res = CHK(vkCreateFence(vk_dev, &fence_create_info, nullptr, &vk_fens[i]));

        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

bool wait_and_reset_fence(eFenceId fence)
{
    VkResult res = CHK(vkWaitForFences(vk_dev, 1, &vk_fens[fence], VK_TRUE, 1'000'000'000));
    if (res != VK_SUCCESS)
        return false;

    res = CHK(vkResetFences(vk_dev, 1, &vk_fens[fence]));
    return res == VK_SUCCESS;
}

VkSurfaceCapabilitiesKHR vk_surface_caps;

static VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;

uint32_t vk_num_swapchain_images = 0;
Image    vk_swapchain_images[max_swapchain_size];
Image    vk_depth_buffers[max_swapchain_size];
VkFormat vk_depth_format = VK_FORMAT_UNDEFINED;

static VkDeviceSize heap_low_checkpoint;
static VkDeviceSize heap_high_checkpoint;

bool allocate_depth_buffers(Image (&depth_buffers)[max_swapchain_size], uint32_t num_depth_buffers)
{
    if (heap_low_checkpoint != heap_high_checkpoint) {
        for (uint32_t i = 0; i < mstd::array_size(depth_buffers); i++)
            depth_buffers[i].destroy();

        mem_mgr.restore_heap_checkpoint(heap_low_checkpoint, heap_high_checkpoint);
        heap_high_checkpoint = 0;
    }

    heap_low_checkpoint = mem_mgr.get_heap_checkpoint();

    const uint32_t width  = vk_surface_caps.currentExtent.width;
    const uint32_t height = vk_surface_caps.currentExtent.height;

    static const VkFormat depth_formats[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        #ifdef __APPLE__
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        #endif
    };
    if ( ! find_optimal_tiling_format(depth_formats,
                                      mstd::array_size(depth_formats),
                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      &vk_depth_format)) {
        d_printf("Error: could not find any of the required depth formats\n");
        return false;
    }

    for (uint32_t i = 0; i < num_depth_buffers; i++) {

        static ImageInfo image_info = {
            0, // width
            0, // height
            VK_FORMAT_UNDEFINED,
            1, // mip_levels
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            Usage::device_temporary
        };
        image_info.width  = width;
        image_info.height = height;
        image_info.format = vk_depth_format;

        if ( ! depth_buffers[i].allocate(image_info, {"depth buffer", i}))
            return false;
    }

    heap_high_checkpoint = mem_mgr.get_heap_checkpoint();

    return true;
}

static bool create_swapchain()
{
    VkResult res = CHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys_dev,
                                                                 vk_surface,
                                                                 &vk_surface_caps));
    if (res != VK_SUCCESS)
        return false;

    d_printf("Create swapchain %u x %u\n", vk_surface_caps.currentExtent.width, vk_surface_caps.currentExtent.height);

#ifndef NDEBUG
    {
        VkFormat found_tiling_format = VK_FORMAT_UNDEFINED;
        if ( ! find_optimal_tiling_format(&swapchain_create_info.imageFormat,
                                          1,
                                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
                                          &found_tiling_format)) {
            d_printf("Error: surface format does not support color attachments\n");
            return false;
        }
    }
#endif

    VkSwapchainKHR old_swapchain = vk_swapchain;

    swapchain_create_info.minImageCount = mstd::max(vk_surface_caps.minImageCount, 2u);
    swapchain_create_info.imageExtent   = vk_surface_caps.currentExtent;
    swapchain_create_info.oldSwapchain  = old_swapchain;

#ifdef _WIN32
    static VkSurfaceFullScreenExclusiveInfoEXT fullscreen_exclusive_info = {
        VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
        nullptr,
        VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
    };

    swapchain_create_info.pNext = &fullscreen_exclusive_info;
#endif

    res = CHK(vkCreateSwapchainKHR(vk_dev, &swapchain_create_info, nullptr, &vk_swapchain));

    if (res != VK_SUCCESS)
        return false;

    if (old_swapchain != VK_NULL_HANDLE) {
        for (const Image& image : vk_swapchain_images) {
            const VkImageView view = image.get_view();
            if (view)
                vkDestroyImageView(vk_dev, view, nullptr);
        }

        vkDestroySwapchainKHR(vk_dev, old_swapchain, nullptr);
    }

    mstd::mem_zero(&vk_swapchain_images, sizeof(vk_swapchain_images));

    VkImage  images[mstd::array_size(vk_swapchain_images)];
    uint32_t num_images = 0;

    res = CHK(vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &num_images, nullptr));

    if (res != VK_SUCCESS)
        return false;

    if (num_images > mstd::array_size(vk_swapchain_images))
        num_images = mstd::array_size(vk_swapchain_images);

    vk_num_swapchain_images = num_images;

    res = CHK(vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &num_images, images));

    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

    for (uint32_t i = 0; i < num_images; i++) {
        Image& image = vk_swapchain_images[i];
        image.set_image(images[i]);

        static VkImageViewCreateInfo view_create_info = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0,                  // flags
            VK_NULL_HANDLE,     // image
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_UNDEFINED,
            {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY
            },
            {
                VK_IMAGE_ASPECT_COLOR_BIT,
                0,              // baseMipLevel
                1,              // levelCount
                0,              // baseArrayLayer
                1               // layerCount
            }
        };
        view_create_info.image  = image.get_image();
        view_create_info.format = swapchain_create_info.imageFormat;

        VkImageView view;
        res = CHK(vkCreateImageView(vk_dev, &view_create_info, nullptr, &view));
        if (res != VK_SUCCESS)
            return false;

        vk_swapchain_images[i].set_view(view);
    }

    return allocate_depth_buffers(vk_depth_buffers, num_images);
}

bool idle_queue()
{
    VkResult res = VK_SUCCESS;

    if (vk_queue) {
        d_printf("Idling queue\n");
        res = CHK(vkQueueWaitIdle(vk_queue));
    }

    return res == VK_SUCCESS;
}

static bool update_resolution()
{
    if ( ! idle_queue())
        return false;

    resize_gui();

    if ( ! create_swapchain())
        return false;

    return true;
}

bool allocate_command_buffers(CommandBuffersBase* bufs, uint32_t num_buffers)
{
    assert(bufs->pool == VK_NULL_HANDLE);

    static VkCommandPoolCreateInfo create_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };

    create_info.queueFamilyIndex = vk_queue_family_index;

    VkResult res = CHK(vkCreateCommandPool(vk_dev,
                                           &create_info,
                                           nullptr,
                                           &bufs->pool));
    if (res != VK_SUCCESS)
        return false;

    static VkCommandBufferAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        VK_NULL_HANDLE,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY
    };

    alloc_info.commandPool        = bufs->pool;
    alloc_info.commandBufferCount = num_buffers;

    res = CHK(vkAllocateCommandBuffers(vk_dev, &alloc_info, bufs->bufs));
    return res == VK_SUCCESS;
}

bool reset_and_begin_command_buffer(VkCommandBuffer cmd_buf)
{
    VkResult res = CHK(vkResetCommandBuffer(cmd_buf, 0));
    if (res != VK_SUCCESS)
        return false;

    static VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };

    res = CHK(vkBeginCommandBuffer(cmd_buf, &begin_info));
    return res == VK_SUCCESS;
}

bool send_to_device_and_wait(VkCommandBuffer cmd_buf)
{
    VkResult res = CHK(vkEndCommandBuffer(cmd_buf));
    if (res != VK_SUCCESS)
        return false;

    static const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0,                  // waitSemaphoreCount
        nullptr,            // pWaitSemaphored
        &dst_stage,         // pWaitDstStageMask
        1,                  // commandBufferCount
        &cmd_buf,
        0,                  // signalSemaphoreCount
        nullptr             // pSignalSemaphores
    };

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, vk_fens[fen_copy_to_dev]));
    if (res != VK_SUCCESS)
        return false;

    return wait_and_reset_fence(fen_copy_to_dev);
}

void configure_viewport_and_scissor(VkViewport* viewport,
                                    VkRect2D*   scissor,
                                    float       image_ratio,
                                    uint32_t    viewport_width,
                                    uint32_t    viewport_height)
{
    // Note: Flip Y coordinate.  The world coordinate system assumes Y going from
    // bottom to top, but in Vulkan screen-space Y coordinate goes from top to bottom.
    if (image_ratio != 0) {
        const float cur_ratio = static_cast<float>(viewport_width) /
                                static_cast<float>(viewport_height);

        if (cur_ratio > image_ratio) {
            const uint32_t height = viewport_height;
            const uint32_t width  = static_cast<uint32_t>(static_cast<float>(height) * image_ratio);
            const uint32_t x      = (viewport_width - width) / 2;

            scissor->offset.x      = static_cast<int32_t>(x);
            scissor->extent.width  = width;
            scissor->extent.height = height;

            viewport->x      = static_cast<float>(x);
            viewport->y      = static_cast<float>(height);
            viewport->width  = static_cast<float>(width);
            viewport->height = -static_cast<float>(height);
        }
        else {
            const uint32_t width  = viewport_width;
            const uint32_t height = static_cast<uint32_t>(static_cast<float>(width) / image_ratio);
            const uint32_t y      = (viewport_height - height) / 2;

            scissor->offset.y      = static_cast<int32_t>(y);
            scissor->extent.width  = width;
            scissor->extent.height = height;

            viewport->y      = static_cast<float>((viewport_height + height) / 2);
            viewport->width  = static_cast<float>(width);
            viewport->height = -static_cast<float>(height);
        }
    }
    else {
        viewport->y      = static_cast<float>(viewport_height);
        viewport->width  = static_cast<float>(viewport_width);
        viewport->height = -static_cast<float>(viewport_height);

        scissor->extent.width  = viewport_width;
        scissor->extent.height = viewport_height;
    }

}

void configure_viewport_and_scissor(VkViewport* viewport,
                                    VkRect2D*   scissor,
                                    uint32_t    viewport_width,
                                    uint32_t    viewport_height)
{
    configure_viewport_and_scissor(viewport,
                                   scissor,
                                   static_cast<float>(viewport_width) / static_cast<float>(viewport_height),
                                   viewport_width,
                                   viewport_height);
}

void send_viewport_and_scissor(VkCommandBuffer cmd_buf,
                               float           image_ratio,
                               uint32_t        viewport_width,
                               uint32_t        viewport_height)
{
    static VkViewport viewport = {
        0,      // x
        0,      // y
        0,      // width
        0,      // height
        0,      // minDepth
        1       // maxDepth
    };

    static VkRect2D scissor = {
        { 0, 0 },   // offset
        { 0, 0 }    // extent
    };

    configure_viewport_and_scissor(&viewport, &scissor, image_ratio, viewport_width, viewport_height);

    vkCmdSetViewport(cmd_buf, 0, 1, &viewport);

    vkCmdSetScissor(cmd_buf, 0, 1, &scissor);
}

void send_viewport_and_scissor(VkCommandBuffer cmd_buf,
                               uint32_t        viewport_width,
                               uint32_t        viewport_height)
{
    send_viewport_and_scissor(cmd_buf,
                              static_cast<float>(viewport_width) / static_cast<float>(viewport_height),
                              viewport_width,
                              viewport_height);
}

#if !defined(NDEBUG) || defined(TIME_STATS)
static void update_time_stats(uint64_t draw_start_time_ms)
{
    const uint64_t  draw_end_time_ms = get_current_time_ms();
    static uint64_t last_draw_end_time_ms;
    static uint64_t stat_start_time_ms;

    if (last_draw_end_time_ms) {
        const uint64_t draw_time_ms = draw_end_time_ms - draw_start_time_ms;

        static uint64_t total_draw_time_ms;
        static uint32_t num_frames;

        total_draw_time_ms += draw_time_ms;
        ++num_frames;

        const uint64_t stat_time = draw_end_time_ms - stat_start_time_ms;
        if (stat_time > 1000) {
            const double   fps              = 1000.0 * num_frames / static_cast<double>(stat_time);
            const unsigned avg_draw_time_ms = static_cast<unsigned>(total_draw_time_ms / num_frames);
            const unsigned load             = static_cast<unsigned>(100 * total_draw_time_ms / stat_time);

            printf("FPS: %.1f, avg draw %u ms, load %u%%\n", fps, avg_draw_time_ms, load);

            stat_start_time_ms = draw_end_time_ms;
            total_draw_time_ms = 0;
            num_frames         = 0;
        }
    }
    else
        stat_start_time_ms = draw_end_time_ms;

    last_draw_end_time_ms = draw_end_time_ms;
}
#else
#define update_time_stats(time) (void)0
#endif

bool init_vulkan(Window* w)
{
    if ( ! load_vulkan())
        return false;

    if ( ! load_lib_functions())
        return false;

    if ( ! load_global_functions())
        return false;

    if ( ! init_instance())
        return false;

    if ( ! create_surface(w))
        return false;

    if ( ! create_device())
        return false;

    if ( ! mem_mgr.init_heaps(256u * 1024u * 1024u,
                              128u * 1024u * 1024u,
                              16u * 1024u * 1024u))
        return false;

    if ( ! create_semaphores())
        return false;

    if ( ! create_fences())
        return false;

    if ( ! create_swapchain())
        return false;

    if ( ! init_assets())
        return false;

    if ( ! init_sound())
        return false;

    return true;
}

static uint32_t get_next_sem_id()
{
    static uint32_t next_sem_id = 0;
    const uint32_t sem_id = next_sem_id;
    next_sem_id = (next_sem_id + num_semaphore_types) % num_semaphores;
    return sem_id;
}

bool draw_frame()
{
    uint32_t image_idx;
    VkResult res;

    static bool playing = false;
    if ( ! playing) {
        playing = true;
        if ( ! play_sound_track())
            return false;
    }

    const uint32_t sem_id = get_next_sem_id();

    for (;;) {

        res = CHK(vkAcquireNextImageKHR(vk_dev,
                                        vk_swapchain,
                                        1'000'000'000,
                                        vk_sems[sem_id + sem_acquire],
                                        VK_NULL_HANDLE,
                                        &image_idx));
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
            assert(image_idx < max_swapchain_size);
            break;
        }

        if (res != VK_ERROR_OUT_OF_DATE_KHR)
            return false;

        if ( ! update_resolution())
            return false;
    }

    const eFenceId fen_queue = static_cast<eFenceId>(fen_submit + image_idx);
    static bool    fence_set[max_swapchain_size];
    if (fence_set[image_idx]) {
        if ( ! wait_and_reset_fence(fen_queue))
            return false;
    }

    const uint64_t  cur_abs_time_ms  = get_current_time_ms();
    static uint64_t base_abs_time_ms = 0;
    if ( ! base_abs_time_ms)
        base_abs_time_ms = cur_abs_time_ms;

    if ( ! draw_frame(image_idx, cur_abs_time_ms - base_abs_time_ms, vk_fens[fen_queue], sem_id))
        return false;
    fence_set[image_idx] = true;

    update_time_stats(cur_abs_time_ms);

    static VkPresentInfoKHR present_info = {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        nullptr,
        1,
        nullptr,
        1,
        &vk_swapchain,
        nullptr,
        nullptr
    };

    present_info.pImageIndices   = &image_idx;
    present_info.pWaitSemaphores = &vk_sems[sem_id + sem_present];

    res = CHK(vkQueuePresentKHR(vk_queue, &present_info));

    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
        return update_resolution();

    return res == VK_SUCCESS;
}
