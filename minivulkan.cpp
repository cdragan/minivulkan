// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "minivulkan.h"
#include "d_printf.h"
#include "gui.h"
#include "mstdc.h"
#include "vmath.h"
#include "vulkan_extensions.h"

#ifdef _WIN32
#   define dlsym GetProcAddress
#else
#   include <dlfcn.h>
#endif

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

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
            MAKE_ERR_STR(VK_SUBOPTIMAL_KHR)
            MAKE_ERR_STR(VK_ERROR_OUT_OF_DATE_KHR)
#           undef MAKE_ERR_STR
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
        MAKE_STR(VK_FORMAT_A8B8G8R8_UNORM_PACK32)
        MAKE_STR(VK_FORMAT_B8G8R8A8_UNORM)
        MAKE_STR(VK_FORMAT_R8G8B8A8_UNORM)
        MAKE_STR(VK_FORMAT_B8G8R8_UNORM)
        MAKE_STR(VK_FORMAT_R8G8B8_UNORM)
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
    static const char lib_name[] = "libvulkan.dylib";
#elif defined(__linux__)
    static const char lib_name[] = "libvulkan.so.1";
#endif

#ifdef _WIN32
    vulkan_lib = LoadLibrary(lib_name);
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
        const uint32_t len = mstd::strlen(names);

        if ( ! len)
            break;

        const PFN_vkVoidFunction fn = load(names);

        if ( ! fn) {
            d_printf("Failed to load %s\n", names);
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
        VK_API_VERSION_1_2
    };

    static const char* enabled_instance_extensions[16];

    static VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        nullptr,
        0,
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
    if (num_ext_props)
        d_printf("Instance extensions:\n");
    for (uint32_t i = 0; i < num_ext_props; i++)
        d_printf("    %s\n", ext_props[i].extensionName);
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

    VkValidationFeaturesEXT validation_features = {
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        nullptr,
    };

    for (uint32_t i = 0; i < num_layer_props; i++) {
        d_printf("Layer: %s\n", layer_props[i].layerName);

        num_ext_props = mstd::array_size(ext_props);

        const char* validation_features_str = nullptr;

        res = vkEnumerateInstanceExtensionProperties(layer_props[i].layerName,
                                                     &num_ext_props,
                                                     ext_props);
        if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
            for (uint32_t j = 0; j < num_ext_props; j++) {
                d_printf("    %s\n", ext_props[j].extensionName);

                static const char validation_features_ext[] = "VK_EXT_validation_features";
                if (mstd::strcmp(ext_props[i].extensionName, validation_features_ext) == 0)
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

                instance_info.pNext = &validation_features;
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

static VkSwapchainCreateInfoKHR swapchain_create_info = {
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

    static const VkFormat preferred_output_formats[] = {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM
    };

    for (uint32_t i_pref = 0; i_pref < mstd::array_size(preferred_output_formats); i_pref++) {

        const VkFormat pref_format = preferred_output_formats[i_pref];

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

static bool find_optimal_tiling_format(const VkFormat* preferred_formats,
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
            d_printf("Selected device %u: %s\n", i_dev, vk_phys_props.properties.deviceName);
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
    for (uint32_t i = 0; i < num_extensions; i++)
        d_printf("    %s\n", extensions[i].extensionName);
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

uint32_t DeviceMemoryHeap::device_memory_type   = ~0u;
uint32_t DeviceMemoryHeap::host_memory_type     = ~0u;
uint32_t DeviceMemoryHeap::coherent_memory_type = ~0u;

static constexpr uint32_t alloc_heap_size    = 64u * 1024u * 1024u;

static DeviceMemoryHeap vk_device_heap;

#ifndef NDEBUG
static void str_append(char* buf, const char* str)
{
    while (*buf)
        ++buf;

    while (*str)
        *(buf++) = *(str++);

    *buf = 0;
}
#endif

bool DeviceMemoryHeap::init_heap_info()
{
    if (device_memory_type != ~0u)
        return true;

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_phys_dev, &mem_props);

    int device_type_index   = -1;
    int host_type_index     = -1;
    int coherent_type_index = -1;

    VkDeviceSize device_heap_size = 0;

    d_printf("Memory heaps\n");
    for (int i = 0; i < static_cast<int>(mem_props.memoryTypeCount); i++) {

        const VkMemoryType& memory_type    = mem_props.memoryTypes[i];
        const uint32_t      property_flags = memory_type.propertyFlags;
        const VkDeviceSize  heap_size      = mem_props.memoryHeaps[memory_type.heapIndex].size;

#ifndef NDEBUG
        static char info[64];
        info[0] = 0;
        if (property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            str_append(info, "device,");
        if (property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            str_append(info, "host_visible,");
        if (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            str_append(info, "host_coherent,");
        if (property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            str_append(info, "host_cached,");
        d_printf("    type %d, heap %u, flags 0x%x (%s)\n",
                i,
                memory_type.heapIndex,
                property_flags,
                info);
#endif

        if ((property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            (heap_size > device_heap_size)) {

            device_type_index = i;
            device_heap_size  = heap_size;
        }

        constexpr uint32_t host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if ((property_flags & host_flags) == host_flags) {
            if ((property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
                (host_type_index == -1))

                host_type_index = i;
        }

        constexpr uint32_t coherent_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((property_flags & coherent_flags) == coherent_flags) {
            if (property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                if (heap_size >= device_heap_size) {
                    device_type_index = i;
                    device_heap_size  = heap_size;
                }

                coherent_type_index = i;
            }
            else if (coherent_type_index == -1)
                coherent_type_index = i;
        }
    }

    if (host_type_index == -1) {
        if (coherent_type_index != -1)
            host_type_index = coherent_type_index;
        else {
            d_printf("Could not find coherent and cached host memory type\n");
            return false;
        }
    }

    device_memory_type   = static_cast<uint32_t>(device_type_index);
    host_memory_type     = static_cast<uint32_t>(host_type_index);
    coherent_memory_type = static_cast<uint32_t>(coherent_type_index);

    return true;
}

bool DeviceMemoryHeap::allocate_heap_once(const VkMemoryRequirements& requirements)
{
    if (memory != VK_NULL_HANDLE)
        return true;

    return allocate_heap(mstd::align_up(requirements.size, requirements.alignment));
}

bool DeviceMemoryHeap::allocate_heap(VkDeviceSize size)
{
    assert(memory         == VK_NULL_HANDLE);
    assert(next_free_offs == 0);
    assert(heap_size      == 0);

    if ( ! init_heap_info())
        return false;

    const uint32_t memory_type = (memory_location == device_memory) ? device_memory_type :
                                 (memory_location == host_memory)   ? host_memory_type   :
                                                                      coherent_memory_type;

    size = mstd::align_up(size, VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));

    static VkMemoryAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        0,  // allocationSize
        0   // memoryTypeIndex
    };
    alloc_info.allocationSize  = size;
    alloc_info.memoryTypeIndex = memory_type;

    const VkResult res = CHK(vkAllocateMemory(vk_dev, &alloc_info, nullptr, &memory));
    if (res != VK_SUCCESS)
        return false;

    heap_size = size;
    d_printf("Allocated heap size 0x%" PRIx64 " bytes with memory type %u\n",
            static_cast<uint64_t>(size), memory_type);

    return true;
}

void DeviceMemoryHeap::free_heap()
{
    if (memory) {
        vkFreeMemory(vk_dev, memory, nullptr);

        memory         = VK_NULL_HANDLE;
        next_free_offs = 0;
        heap_size      = 0;
    }
}

bool DeviceMemoryHeap::allocate_memory(const VkMemoryRequirements& requirements,
                                       VkDeviceSize*               offset)
{
    const VkDeviceSize alignment = mstd::align_up(requirements.alignment,
                                                  VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));
    const VkDeviceSize aligned_offs = mstd::align_up(next_free_offs, alignment);
    assert(next_free_offs || ! aligned_offs);
    assert(aligned_offs >= next_free_offs);
    assert(aligned_offs % alignment == 0);

    if (aligned_offs + requirements.size > heap_size) {
        d_printf("Not enough device memory\n");
        d_printf("Surface size 0x%" PRIx64 ", used heap size 0x%" PRIx64 ", max heap size 0x%" PRIx64 "\n",
                static_cast<uint64_t>(requirements.size),
                static_cast<uint64_t>(aligned_offs),
                static_cast<uint64_t>(heap_size));
        return false;
    }

    *offset        = aligned_offs;
    next_free_offs = aligned_offs + requirements.size;

    return true;
}

MapBase::MapBase(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size)
{
    assert( ! heap->mapped);
    assert((offset % vk_phys_props.properties.limits.minMemoryMapAlignment) == 0);
    assert((offset % vk_phys_props.properties.limits.nonCoherentAtomSize) == 0);

    const VkDeviceSize aligned_size = mstd::align_up(size,
            VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));

    void* ptr;
    const VkResult res = CHK(vkMapMemory(vk_dev, heap->get_memory(), offset, aligned_size, 0, &ptr));
    if (res == VK_SUCCESS) {
        mapped_heap   = heap;
        mapped_ptr    = ptr;
        mapped_offset = offset;
        mapped_size   = size;
        heap->mapped  = true;
    }
}

void MapBase::unmap()
{
    if (mapped_heap) {
        assert(mapped_ptr);
        assert(mapped_size);
        assert(mapped_heap->mapped);

        vkUnmapMemory(vk_dev, mapped_heap->get_memory());
        mapped_heap->mapped = false;

        mapped_heap   = nullptr;
        mapped_ptr    = nullptr;
        mapped_offset = 0;
        mapped_size   = 0;
    }
    else {
        assert( ! mapped_ptr);
        assert( ! mapped_offset);
        assert( ! mapped_size);
    }
}

bool MapBase::flush(uint32_t offset, uint32_t size)
{
    assert(mapped_heap);
    if ( ! mapped_heap)
        return false;

    assert(offset + size <= mapped_size);
    assert((size % vk_phys_props.properties.limits.nonCoherentAtomSize) == 0);

    static VkMappedMemoryRange range = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        VK_NULL_HANDLE,     // memory
        0,                  // offset
        0                   // size
    };
    range.memory = mapped_heap->get_memory();
    range.offset = mapped_offset + offset;
    range.size   = size;

    const VkResult res = CHK(vkFlushMappedMemoryRanges(vk_dev, 1, &range));
    return res == VK_SUCCESS;
}

void MapBase::move_from(MapBase& map)
{
    unmap();

    mapped_heap = map.mapped_heap;
    mapped_ptr  = map.mapped_ptr;
    mapped_size = map.mapped_size;

    map.mapped_heap = nullptr;
    map.mapped_ptr  = nullptr;
    map.mapped_size = 0;
}

bool Image::create(const ImageInfo& image_info, VkImageTiling tiling)
{
    static VkImageCreateInfo create_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        nullptr,
        0,                  // flags
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_UNDEFINED,
        { 0, 0, 1 },        // extent
        1,                  // mipLevels
        1,                  // arrayLayers
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        0,                  // usage
        VK_SHARING_MODE_EXCLUSIVE,
        1,                  // queueFamilyIndexCount
        &vk_queue_family_index,
        Image::initial_layout
    };
    create_info.format        = image_info.format;
    create_info.extent.width  = image_info.width;
    create_info.extent.height = image_info.height;
    create_info.mipLevels     = image_info.mip_levels;
    create_info.tiling        = tiling;
    create_info.usage         = image_info.usage;

    const VkResult res = CHK(vkCreateImage(vk_dev, &create_info, nullptr, &image));
    if (res != VK_SUCCESS)
        return false;

    layout     = Image::initial_layout;
    format     = image_info.format;
    aspect     = image_info.aspect;
    mip_levels = image_info.mip_levels;

    vkGetImageMemoryRequirements(vk_dev, image, &memory_reqs);

    return true;
}

bool Image::allocate(DeviceMemoryHeap& heap)
{
#ifndef NDEBUG
    if ( ! (memory_reqs.memoryTypeBits & (1u << heap.get_memory_type()))) {
        d_printf("Device memory does not support requested image type\n");
        return false;
    }
#endif

    if ( ! heap.allocate_heap_once(memory_reqs))
        return false;

    VkDeviceSize offset;
    if ( ! heap.allocate_memory(memory_reqs, &offset))
        return false;

    VkResult res = CHK(vkBindImageMemory(vk_dev, image, heap.get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    owning_heap = &heap;
    heap_offset = offset;

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
            0, // aspectMask
            0, // baseMipLevel
            0, // levelCount
            0, // baseArrayLayer
            1  // layerCount
        }
    };
    view_create_info.image                       = image;
    view_create_info.format                      = format;
    view_create_info.subresourceRange.aspectMask = aspect;
    view_create_info.subresourceRange.levelCount = mip_levels;

    res = CHK(vkCreateImageView(vk_dev, &view_create_info, nullptr, &view));
    return res == VK_SUCCESS;
}

bool Image::allocate(DeviceMemoryHeap& heap, const ImageInfo& image_info)
{
    const VkImageTiling tiling = heap.is_host_memory() ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    return create(image_info, tiling) && allocate(heap);
}

void Image::destroy()
{
    if (view)
        vkDestroyImageView(vk_dev, view, nullptr);
    if (image)
        vkDestroyImage(vk_dev, image, nullptr);
    view        = VK_NULL_HANDLE;
    image       = VK_NULL_HANDLE;
    owning_heap = nullptr;
    heap_offset = 0;
}

void Image::set_image_layout(VkCommandBuffer buf, const Transition& transition)
{
    static VkImageMemoryBarrier img_barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED
    };

    img_barrier.oldLayout     = layout;
    img_barrier.newLayout     = transition.new_layout;
    img_barrier.srcAccessMask = transition.src_access;
    img_barrier.dstAccessMask = transition.dest_access;

    layout = transition.new_layout;

    img_barrier.srcQueueFamilyIndex         = vk_queue_family_index;
    img_barrier.dstQueueFamilyIndex         = vk_queue_family_index;
    img_barrier.image                       = image;
    img_barrier.subresourceRange.aspectMask = aspect;
    img_barrier.subresourceRange.levelCount = 1;
    img_barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(buf,
                         transition.src_stage,
                         transition.dest_stage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &img_barrier);
}

bool Buffer::allocate(DeviceMemoryHeap&  heap,
                      uint32_t           alloc_size,
                      VkBufferUsageFlags usage)
{
    static VkBufferCreateInfo create_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,          // flags
        0,          // size
        0,          // usage
        VK_SHARING_MODE_EXCLUSIVE,
        1,          // queueFamilyIndexCount
        &vk_queue_family_index
    };
    create_info.size  = alloc_size;
    create_info.usage = usage;

    VkResult res = CHK(vkCreateBuffer(vk_dev, &create_info, nullptr, &buffer));
    if (res != VK_SUCCESS)
        return false;

    vkGetBufferMemoryRequirements(vk_dev, buffer, &memory_reqs);

#ifndef NDEBUG
    if ( ! (memory_reqs.memoryTypeBits & (1u << heap.get_memory_type()))) {
        d_printf("Device memory does not support requested buffer type\n");
        return false;
    }
#endif

    if ( ! heap.allocate_heap_once(memory_reqs))
        return false;

    VkDeviceSize offset;
    if ( ! heap.allocate_memory(memory_reqs, &offset))
        return false;

    res = CHK(vkBindBufferMemory(vk_dev, buffer, heap.get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    owning_heap = &heap;
    heap_offset = offset;

    return true;
}

bool Buffer::create_view(VkFormat format)
{
    static VkBufferViewCreateInfo view_create_info = {
        VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        nullptr,
        0,                      // flags
        VK_NULL_HANDLE,         // buffer
        VK_FORMAT_UNDEFINED,    // format
        0,                      // offset
        VK_WHOLE_SIZE           // range
    };
    view_create_info.buffer = buffer;
    view_create_info.format = format;
    view_create_info.offset = heap_offset;

    const VkResult res = CHK(vkCreateBufferView(vk_dev, &view_create_info, nullptr, &view));
    return res == VK_SUCCESS;
}

void Buffer::destroy()
{
    if (view)
        vkDestroyBufferView(vk_dev, view, nullptr);
    if (buffer)
        vkDestroyBuffer(vk_dev, buffer, nullptr);
    buffer      = VK_NULL_HANDLE;
    view        = VK_NULL_HANDLE;
    owning_heap = nullptr;
    heap_offset = 0;
}

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

uint32_t                vk_num_swapchain_images = 0;
Image                   vk_swapchain_images[max_swapchain_size];
Image                   vk_depth_buffers[max_swapchain_size];
static DeviceMemoryHeap depth_buffer_heap;
static VkFormat         vk_depth_format = VK_FORMAT_UNDEFINED;

static bool allocate_depth_buffers(uint32_t num_depth_buffers)
{
    for (uint32_t i = 0; i < mstd::array_size(vk_depth_buffers); i++)
        vk_depth_buffers[i].destroy();

    const uint32_t width  = vk_surface_caps.currentExtent.width;
    const uint32_t height = vk_surface_caps.currentExtent.height;

    static const VkFormat depth_formats[] = {
        VK_FORMAT_D24_UNORM_S8_UINT
    };
    if ( ! find_optimal_tiling_format(depth_formats,
                                      mstd::array_size(depth_formats),
                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      &vk_depth_format)) {
        d_printf("Error: could not find any of the required depth formats\n");
        return false;
    }

    VkDeviceSize heap_size = 0;

    for (uint32_t i = 0; i < num_depth_buffers; i++) {

        static ImageInfo image_info = {
            0, // width
            0, // height
            VK_FORMAT_UNDEFINED,
            1, // mip_levels
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        };
        image_info.width  = width;
        image_info.height = height;
        image_info.format = vk_depth_format;

        if ( ! vk_depth_buffers[i].create(image_info, VK_IMAGE_TILING_OPTIMAL))
            return false;

        heap_size += mstd::align_up(vk_depth_buffers[i].size(),
                                    VkDeviceSize(vk_phys_props.properties.limits.minMemoryMapAlignment));
    }

    if (heap_size > depth_buffer_heap.get_heap_size()) {

        depth_buffer_heap.free_heap();

        if ( ! depth_buffer_heap.allocate_heap(heap_size))
            return false;
    }
    else
        depth_buffer_heap.reset_heap();

    for (uint32_t i = 0; i < num_depth_buffers; i++) {
        if ( ! vk_depth_buffers[i].allocate(depth_buffer_heap))
            return false;
    }
    return true;
}

static bool create_swapchain()
{
    VkResult res = CHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys_dev,
                                                                 vk_surface,
                                                                 &vk_surface_caps));
    if (res != VK_SUCCESS)
        return false;

    d_printf("Create swapchain %ux%u\n", vk_surface_caps.currentExtent.width, vk_surface_caps.currentExtent.height);

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

    return allocate_depth_buffers(num_images);
}

VkRenderPass vk_render_pass = VK_NULL_HANDLE;

static bool create_render_pass()
{
    static VkAttachmentDescription attachments[] = {
        {
            0, // flags
            VK_FORMAT_UNDEFINED,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        },
        {
            0, // flags
            VK_FORMAT_UNDEFINED,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        }
    };

#ifdef NDEBUG
    attachments[0].format = swapchain_create_info.imageFormat;
#else
    if ( ! find_optimal_tiling_format(&swapchain_create_info.imageFormat,
                                      1,
                                      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
                                      &attachments[0].format)) {
        d_printf("Error: surface format does not support color attachments\n");
        return false;
    }
#endif

    attachments[1].format = vk_depth_format;

    static const VkAttachmentReference color_att_ref = {
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    static const VkAttachmentReference depth_att_ref = {
        1,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    static const VkSubpassDescription subpass = {
        0,          // flags
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        0,          // inputAttachmentCount
        nullptr,    // pInputAttachments
        1,
        &color_att_ref,
        nullptr,    // pResolveAttachments
        &depth_att_ref,
        0,          // preserveAttachmentCount
        nullptr     // pPreserveAttachments
    };
    static const VkRenderPassCreateInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        nullptr,    // pNext
        0,          // flags
        mstd::array_size(attachments),
        attachments,
        1,          // subpassCount
        &subpass
    };

    const VkResult res = CHK(vkCreateRenderPass(vk_dev, &render_pass_info, nullptr, &vk_render_pass));
    return res == VK_SUCCESS;
}

VkFramebuffer vk_frame_buffers[max_swapchain_size];

static bool create_frame_buffer()
{
    for (uint32_t i = 0; i < mstd::array_size(vk_frame_buffers); i++) {

        if ( ! vk_swapchain_images[i].get_image())
            break;

        static VkImageView attachments[2];

        static VkFramebufferCreateInfo frame_buffer_info = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            nullptr,
            0,                  // flags
            VK_NULL_HANDLE,     // renderPass
            mstd::array_size(attachments),
            attachments,
            0,                  // width
            0,                  // height
            1                   // layers
        };
        frame_buffer_info.renderPass = vk_render_pass;
        frame_buffer_info.width      = vk_surface_caps.currentExtent.width;
        frame_buffer_info.height     = vk_surface_caps.currentExtent.height;

        attachments[0] = vk_swapchain_images[i].get_view();
        attachments[1] = vk_depth_buffers[i].get_view();

        const VkResult res = CHK(vkCreateFramebuffer(vk_dev, &frame_buffer_info, nullptr, &vk_frame_buffers[i]));
        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

void idle_queue()
{
    if (vk_queue) {
        d_printf("Idling queue\n");
        CHK(vkQueueWaitIdle(vk_queue));
    }
}

static bool update_resolution()
{
    const VkResult res = CHK(vkQueueWaitIdle(vk_queue));
    if (res != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < mstd::array_size(vk_frame_buffers); i++) {
        if (vk_frame_buffers[i])
            vkDestroyFramebuffer(vk_dev, vk_frame_buffers[i], nullptr);
        vk_frame_buffers[i] = VK_NULL_HANDLE;
    }

    if ( ! create_swapchain())
        return false;

    if ( ! create_frame_buffer())
        return false;

    if ( ! create_pipelines())
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

bool HostFiller::init(VkDeviceSize heap_size)
{
    if ( ! host_heap.allocate_heap(heap_size))
        return false;

    if ( ! allocate_command_buffers(&cmd_buf))
        return false;

    return reset_and_begin_command_buffer(cmd_buf.bufs[0]);
}

bool HostFiller::fill_buffer(Buffer*            buffer,
                             VkBufferUsageFlags usage,
                             const void*        data,
                             uint32_t           size)
{
    assert(num_buffers < max_buffers);

    if (num_buffers == max_buffers)
        return false;

    Buffer& host_buffer = buffers[num_buffers++];

    if ( ! buffer->allocate(vk_device_heap,
                            size,
                            usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    if ( ! host_buffer.allocate(host_heap, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    {
        Map<uint8_t> map = host_buffer.map<uint8_t>();
        if ( ! map.mapped())
            return false;

        mstd::mem_copy(map.data(), data, size);
    }

    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    copy_region.size = size;

    vkCmdCopyBuffer(cmd_buf.bufs[0], host_buffer.get_buffer(), buffer->get_buffer(), 1, &copy_region);

    return true;
}

bool HostFiller::send_to_gpu()
{
    VkResult res = CHK(vkEndCommandBuffer(cmd_buf.bufs[0]));
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
        cmd_buf.bufs,
        0,                  // signalSemaphoreCount
        nullptr             // pSignalSemaphores
    };

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, vk_fens[fen_copy_to_dev]));
    return res == VK_SUCCESS;
}

bool HostFiller::wait_until_done()
{
    if ( ! wait_and_reset_fence(fen_copy_to_dev))
        return false;

    while (num_buffers--)
        buffers[num_buffers].destroy();

    host_heap.free_heap();

    return true;
}

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

    if ( ! vk_device_heap.allocate_heap(alloc_heap_size))
        return false;

    if ( ! create_additional_heaps())
        return false;

    if ( ! create_semaphores())
        return false;

    if ( ! create_fences())
        return false;

    if ( ! create_swapchain())
        return false;

    if ( ! create_render_pass())
        return false;

    if ( ! create_frame_buffer())
        return false;

    if ( ! create_pipeline_layouts())
        return false;

    if ( ! create_pipelines())
        return false;

    if ( ! init_gui())
        return false;

    if ( ! init_sound())
        return false;

    return true;
}

bool draw_frame()
{
    uint32_t image_idx;
    VkResult res;

    static bool playing = false;
    if ( ! playing) {
        playing = true;
        if ( ! play_sound(0))
            return false;
    }

    for (;;) {

        res = CHK(vkAcquireNextImageKHR(vk_dev,
                                        vk_swapchain,
                                        1'000'000'000,
                                        vk_sems[sem_acquire],
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

    const uint64_t  cur_abs_time  = get_current_time_ms();
    static uint64_t base_abs_time = 0;
    if ( ! base_abs_time)
        base_abs_time = cur_abs_time;

    if ( ! draw_frame(image_idx, cur_abs_time - base_abs_time, vk_fens[fen_queue]))
        return false;
    fence_set[image_idx] = true;

    static VkPresentInfoKHR present_info = {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        nullptr,
        1,
        &vk_sems[sem_acquire],
        1,
        &vk_swapchain,
        nullptr,
        nullptr
    };

    present_info.pImageIndices = &image_idx;

    res = CHK(vkQueuePresentKHR(vk_queue, &present_info));

    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
        return update_resolution();

    return res == VK_SUCCESS;
}
