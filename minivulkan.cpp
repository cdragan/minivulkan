// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "minivulkan.h"
#include "dprintf.h"
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
            dprintf("%s:%d: %s in %s\n", file, line, desc, call_str);
        else
            dprintf("%s:%d: error %d in %s\n", file, line, static_cast<int>(res), call_str);
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
    vulkan_lib = LoadLibrary("vulkan-1.dll");
#elif defined(__APPLE__)
    vulkan_lib = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
#elif defined(__linux__)
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
        const uint32_t len = mstd::strlen(names);

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
            dprintf("Required extension %s not found\n", supported_extensions);
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

    VkResult res = CHK(vkEnumerateInstanceExtensionProperties(nullptr, &num_ext_props, ext_props));
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

#ifndef NDEBUG
    if (num_ext_props)
        dprintf("instance extensions:\n");
    for (uint32_t i = 0; i < num_ext_props; i++)
        dprintf("    %s\n", ext_props[i].extensionName);
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
        dprintf("layer: %s\n", layer_props[i].layerName);

        num_ext_props = mstd::array_size(ext_props);

        const char* validation_features_str = nullptr;

        res = vkEnumerateInstanceExtensionProperties(layer_props[i].layerName,
                                                     &num_ext_props,
                                                     ext_props);
        if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
            for (uint32_t j = 0; j < num_ext_props; j++) {
                dprintf("    %s\n", ext_props[j].extensionName);

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

static VkPhysicalDeviceProperties2 phys_props = {
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
                dprintf("found surface format %s\n", format_string(pref_format));
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
        dprintf("Found 0 physical devices\n");
        return false;
    }

    const VkPhysicalDeviceType seek_types[] = {
        VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    };

    for (uint32_t i_type = 0; i_type < mstd::array_size(seek_types); i_type++) {

        const VkPhysicalDeviceType type = seek_types[i_type];

        for (uint32_t i_dev = 0; i_dev < count; i_dev++) {
            const VkPhysicalDevice phys_dev = phys_devices[i_dev];

            vkGetPhysicalDeviceProperties2(phys_dev, &phys_props);

            if (phys_props.properties.deviceType != type)
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
            dprintf("Selected device %u: %s\n", i_dev, phys_props.properties.deviceName);
            return true;
        }
    }

    dprintf("Could not find any usable GPUs\n");
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
        dprintf("    %s\n", extensions[i].extensionName);
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

static VkPhysicalDeviceFeatures2 vk_features = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
};

static bool create_device()
{
    if ( ! find_gpu())
        return false;

    if ( ! get_device_extensions())
        return false;

    vkGetPhysicalDeviceFeatures2(vk_phys_dev, &vk_features);

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
static constexpr uint32_t coherent_heap_size = 1u * 1024u * 1024u;

static DeviceMemoryHeap vk_device_heap;
static DeviceMemoryHeap vk_coherent_heap{DeviceMemoryHeap::coherent_memory};

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

    dprintf("Memory heaps\n");
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
        dprintf("    type %d, heap %u, flags 0x%x (%s)\n",
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
        dprintf("Could not find coherent and cached host memory type\n");
        return false;
    }

    device_memory_type   = (uint32_t)device_type_index;
    host_memory_type     = (uint32_t)host_type_index;
    coherent_memory_type = (uint32_t)coherent_type_index;

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

    size = mstd::align_up(size, VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));

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
    dprintf("Allocated heap size 0x%" PRIx64 " bytes with memory type %u\n",
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
                                                  VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));
    const VkDeviceSize aligned_offs = mstd::align_up(next_free_offs, alignment);
    assert(next_free_offs || ! aligned_offs);
    assert(aligned_offs >= next_free_offs);
    assert(aligned_offs % alignment == 0);

    if (aligned_offs + requirements.size > heap_size) {
        dprintf("Not enough device memory\n");
        dprintf("Surface size 0x%" PRIx64 ", used heap size 0x%" PRIx64 ", max heap size 0x%" PRIx64 "\n",
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
    assert((offset % phys_props.properties.limits.minMemoryMapAlignment) == 0);

    const VkDeviceSize aligned_size = mstd::align_up(size,
            VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));

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
        dprintf("Device memory does not support requested image type\n");
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
        dprintf("Device memory does not support requested buffer type\n");
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
static Image            vk_swapchain_images[max_swapchain_size];
static Image            vk_depth_buffers[max_swapchain_size];
static DeviceMemoryHeap depth_buffer_heap;
static VkFormat         vk_depth_format = VK_FORMAT_UNDEFINED;

static bool allocate_depth_buffers(uint32_t num_depth_buffers)
{
    for (uint32_t i = 0; i < mstd::array_size(vk_depth_buffers); i++)
        vk_depth_buffers[i].destroy();

    depth_buffer_heap.free_heap();

    const uint32_t width  = vk_surface_caps.currentExtent.width;
    const uint32_t height = vk_surface_caps.currentExtent.height;

    static const VkFormat depth_formats[] = {
        VK_FORMAT_D24_UNORM_S8_UINT
    };
    if ( ! find_optimal_tiling_format(depth_formats,
                                      mstd::array_size(depth_formats),
                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      &vk_depth_format)) {
        dprintf("error: could not find any of the required depth formats\n");
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
                                    VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));
    }

    if ( ! depth_buffer_heap.allocate_heap(heap_size))
        return false;

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

    dprintf("create_swapchain %ux%u\n", vk_surface_caps.currentExtent.width, vk_surface_caps.currentExtent.height);

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

    vk_num_swapchain_images = num_images;

    if (res != VK_SUCCESS)
        return false;

    if (num_images > mstd::array_size(vk_swapchain_images))
        num_images = mstd::array_size(vk_swapchain_images);

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
        dprintf("error: surface format does not support color attachments\n");
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

static VkFramebuffer vk_frame_buffers[max_swapchain_size];

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

enum WhatGeometry {
    geom_cube,
    geom_cubic_patch,
    geom_quadratic_patch
};
static constexpr WhatGeometry what_geometry = geom_quadratic_patch;

static VkPipelineLayout vk_gr_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline       vk_gr_pipeline        = VK_NULL_HANDLE;

static
#include "simple.vert.h"

static
#include "phong.frag.h"

static
#include "pass_through.vert.h"

static
#include "rounded_cube.vert.h"

static
#include "bezier_surface_quadratic.tesc.h"

static
#include "bezier_surface_quadratic.tese.h"

static
#include "bezier_surface_cubic.tesc.h"

static
#include "bezier_surface_cubic.tese.h"

#define DEFINE_SHADERS \
    X(simple_vert) \
    X(phong_frag) \
    X(pass_through_vert) \
    X(rounded_cube_vert) \
    X(bezier_surface_quadratic_tesc) \
    X(bezier_surface_quadratic_tese) \
    X(bezier_surface_cubic_tesc) \
    X(bezier_surface_cubic_tese)

static const struct {
    const uint32_t* code;
    uint32_t        size;
} spirv[] =
{
#define X(shader) { shader##_glsl, mstd::array_size(shader##_glsl) },
    DEFINE_SHADERS
#undef X
};

enum ShaderIds {
    no_shader,
#define X(shader) shader_##shader,
    DEFINE_SHADERS
#undef X
};

static VkShaderModule shaders[mstd::array_size(spirv)];

static bool load_shaders()
{
    for (uint32_t i = 0; i < mstd::array_size(spirv); i++) {
        static VkShaderModuleCreateInfo create_info = {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            nullptr,
            0,
            0,
            nullptr
        };

        create_info.codeSize = spirv[i].size * sizeof(uint32_t);
        create_info.pCode    = spirv[i].code;

        const VkResult res = CHK(vkCreateShaderModule(vk_dev, &create_info, nullptr, &shaders[i]));
        if (res != VK_SUCCESS)
            return false;
    }

    return true;
}

static VkDescriptorSetLayout vk_desc_set_layout = VK_NULL_HANDLE;

static bool create_pipeline_layouts()
{
    static const VkDescriptorSetLayoutBinding create_binding = {
        0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        1,
        VK_SHADER_STAGE_VERTEX_BIT
            | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
            | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
            | VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
    };

    static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0, // flags
        1,
        &create_binding
    };

    VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_desc_set_layout, nullptr, &vk_desc_set_layout));
    if (res != VK_SUCCESS)
        return false;

    static VkPipelineLayoutCreateInfo layout_create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,      // flags
        1,
        &vk_desc_set_layout,
        0,      // pushConstantRangeCount
        nullptr // pPushConstantRanges
    };

    res = CHK(vkCreatePipelineLayout(vk_dev, &layout_create_info, nullptr, &vk_gr_pipeline_layout));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

struct Vertex {
    int8_t pos[3];
    int8_t normal[3];
    int8_t alignment[2]; // VkPhysicalDevicePortabilitySubsetPropertiesKHR::minVertexInputBindingStrideAlignment
};

struct ShaderInfo {
    uint8_t                                  shader_ids[4];
    uint8_t                                  vertex_stride;
    uint8_t                                  topology;
    uint8_t                                  patch_control_points;
    uint8_t                                  num_vertex_attributes;
    const VkVertexInputAttributeDescription* vertex_attributes;
};

static bool create_graphics_pipeline(const ShaderInfo& shader_info)
{
    if (vk_gr_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_dev, vk_gr_pipeline, nullptr);
        vk_gr_pipeline = VK_NULL_HANDLE;
    }

    static VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_VERTEX_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,              // flags
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_NULL_HANDLE, // module
            "main",         // pName
            nullptr         // pSpecializationInfo
        }
    };

    uint32_t num_stages = 0;
    for (uint32_t i = 0; i < mstd::array_size(shader_info.shader_ids); i++) {
        const uint32_t id = shader_info.shader_ids[i];
        if (id == no_shader)
            break;
        shader_stages[i].module = shaders[id - 1];
        ++num_stages;
    }

    static VkVertexInputBindingDescription vertex_bindings[] = {
        {
            0, // binding
            0, // stride
            VK_VERTEX_INPUT_RATE_VERTEX
        }
    };
    vertex_bindings[0].stride = shader_info.vertex_stride;

    static VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,      // flags
        mstd::array_size(vertex_bindings),
        vertex_bindings,
        0,      // vertexAttributeDescriptionCount
        nullptr // pVertexAttributeDescriptions
    };
    vertex_input_state.vertexAttributeDescriptionCount = shader_info.num_vertex_attributes;
    vertex_input_state.pVertexAttributeDescriptions    = shader_info.vertex_attributes;

    static VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_FALSE
    };
    input_assembly_state.topology = static_cast<VkPrimitiveTopology>(shader_info.topology);

    static VkPipelineTessellationStateCreateInfo tessellation_state = {
        VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        0   // patchControlPoints
    };
    tessellation_state.patchControlPoints = shader_info.patch_control_points;

    static VkViewport viewport = {
        0,      // x
        0,      // y
        0,      // width
        0,      // height
        0,      // minDepth
        1       // maxDepth
    };

    // Flip Y coordinate.  The world coordinate system assumes Y going from bottom to top,
    // but in Vulkan screen-space Y coordinate goes from top to bottom.
    viewport.y      = static_cast<float>(vk_surface_caps.currentExtent.height);
    viewport.width  = static_cast<float>(vk_surface_caps.currentExtent.width);
    viewport.height = -static_cast<float>(vk_surface_caps.currentExtent.height);

    static VkRect2D scissor = {
        { 0, 0 },   // offset
        { 0, 0 }    // extent
    };

    scissor.extent = vk_surface_caps.currentExtent;

    static VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        1,
        &viewport,
        1,
        &scissor
    };

    static VkPipelineRasterizationStateCreateInfo rasterization_state = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_FALSE,   // depthClampEnable
        VK_FALSE,   // rasterizerDiscardEnable
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        VK_FRONT_FACE_COUNTER_CLOCKWISE,
        VK_FALSE,   // depthBiasEnable
        0,          // depthBiasConstantFactor
        0,          // depthBiasClamp
        0,          // depthBiasSlopeFactor
        1           // lineWidth
    };

    static VkPipelineMultisampleStateCreateInfo multisample_state = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,   // sampleShadingEnable
        0,          // minSampleShading
        nullptr,    // pSampleMask
        VK_FALSE,   // alphaToCoverageEnable
        VK_FALSE    // alphaToOneEnable
    };

    static VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_TRUE,    // depthTestEnable
        VK_TRUE,    // depthWriteEnable
        VK_COMPARE_OP_GREATER_OR_EQUAL,
        VK_FALSE,   // depthBoundsTestEnable
        VK_FALSE,   // stencilTestEnable
        { },        // front
        { },        // back
        0,          // minDepthBounds
        0           // maxDepthBounds
    };

    static VkPipelineColorBlendAttachmentState color_blend_att = {
        VK_FALSE,               // blendEnable
        VK_BLEND_FACTOR_ZERO,   // srcColorBlendFactor
        VK_BLEND_FACTOR_ZERO,   // dstColorBlendFactor
        VK_BLEND_OP_ADD,        // colorBlendOp
        VK_BLEND_FACTOR_ZERO,   // srcAlphaBlendFactor
        VK_BLEND_FACTOR_ZERO,   // dstAlphaBlendFactor
        VK_BLEND_OP_ADD,        // alphaBlendOp
                                // colorWriteMask
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
    };

    static VkPipelineColorBlendStateCreateInfo color_blend_state = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        nullptr,
        0,          // flags
        VK_FALSE,   // logicOpEnable
        VK_LOGIC_OP_CLEAR,
        1,          // attachmentCount
        &color_blend_att,
        { }         // blendConstants
    };

    static VkGraphicsPipelineCreateInfo pipeline_create_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        nullptr,
        0,              // flags
        num_stages,
        shader_stages,
        &vertex_input_state,
        &input_assembly_state,
        &tessellation_state,
        &viewport_state,
        &rasterization_state,
        &multisample_state,
        &depth_stencil_state,
        &color_blend_state,
        nullptr,        // pDynamicState
        VK_NULL_HANDLE, // layout
        VK_NULL_HANDLE, // renderPass
        0,              // subpass
        VK_NULL_HANDLE, // basePipelineHandle
        -1              // basePipelineIndex
    };

    pipeline_create_info.layout     = vk_gr_pipeline_layout;
    pipeline_create_info.renderPass = vk_render_pass;

    const VkResult res = CHK(vkCreateGraphicsPipelines(vk_dev,
                                                       VK_NULL_HANDLE,
                                                       1,
                                                       &pipeline_create_info,
                                                       nullptr,
                                                       &vk_gr_pipeline));
    return res == VK_SUCCESS;
}

static bool create_simple_graphics_pipeline()
{
    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0,  // location
            0,  // binding
            VK_FORMAT_R8G8B8_SNORM,
            offsetof(Vertex, pos)
        },
        {
            1,  // location
            0,  // binding
            VK_FORMAT_R8G8B8_SNORM,
            offsetof(Vertex, normal)
        }
    };

    static const ShaderInfo shader_info = {
        {
            shader_simple_vert,
            shader_phong_frag
        },
        sizeof(Vertex),
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        0,
        mstd::array_size(vertex_attributes),
        vertex_attributes
    };

    return create_graphics_pipeline(shader_info);
}

static bool create_patch_graphics_pipeline()
{
    static const VkVertexInputAttributeDescription vertex_attributes[] = {
        {
            0,  // location
            0,  // binding
            VK_FORMAT_R8G8B8_SNORM,
            offsetof(Vertex, pos)
        }
    };

    static ShaderInfo shader_info = {
        {
            shader_pass_through_vert,
            shader_phong_frag,
            shader_bezier_surface_cubic_tesc,
            shader_bezier_surface_cubic_tese
        },
        sizeof(Vertex),
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        16,
        mstd::array_size(vertex_attributes),
        vertex_attributes
    };
    if (what_geometry == geom_quadratic_patch) {
        shader_info.patch_control_points = 9;
        shader_info.shader_ids[0]        = shader_rounded_cube_vert;
        shader_info.shader_ids[2]        = shader_bezier_surface_quadratic_tesc;
        shader_info.shader_ids[3]        = shader_bezier_surface_quadratic_tese;
    }

    return create_graphics_pipeline(shader_info);
}

static bool create_graphics_pipelines()
{
    // TODO unify common parts
    if (what_geometry == geom_cube)
        return create_simple_graphics_pipeline();
    else
        return create_patch_graphics_pipeline();
}

void idle_queue()
{
    if (vk_queue) {
        dprintf("idling queue\n");
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

    if ( ! create_graphics_pipelines())
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

static bool create_cube(Buffer* vertex_buffer, Buffer* index_buffer)
{
    HostFiller filler;
    if ( ! filler.init(0x10000))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127, -127 }, {    0,    0, -127 }, {} },
        { {  127,  127, -127 }, {    0,    0, -127 }, {} },
        { { -127, -127, -127 }, {    0,    0, -127 }, {} },
        { {  127, -127, -127 }, {    0,    0, -127 }, {} },
        { { -127, -127,  127 }, {    0,    0,  127 }, {} },
        { {  127, -127,  127 }, {    0,    0,  127 }, {} },
        { { -127,  127,  127 }, {    0,    0,  127 }, {} },
        { {  127,  127,  127 }, {    0,    0,  127 }, {} },
        { { -127,  127,  127 }, {    0,  127,    0 }, {} },
        { {  127,  127,  127 }, {    0,  127,    0 }, {} },
        { { -127,  127, -127 }, {    0,  127,    0 }, {} },
        { {  127,  127, -127 }, {    0,  127,    0 }, {} },
        { { -127, -127, -127 }, {    0, -127,    0 }, {} },
        { {  127, -127, -127 }, {    0, -127,    0 }, {} },
        { { -127, -127,  127 }, {    0, -127,    0 }, {} },
        { {  127, -127,  127 }, {    0, -127,    0 }, {} },
        { {  127,  127, -127 }, {  127,    0,    0 }, {} },
        { {  127,  127,  127 }, {  127,    0,    0 }, {} },
        { {  127, -127, -127 }, {  127,    0,    0 }, {} },
        { {  127, -127,  127 }, {  127,    0,    0 }, {} },
        { { -127,  127,  127 }, { -127,    0,    0 }, {} },
        { { -127,  127, -127 }, { -127,    0,    0 }, {} },
        { { -127, -127,  127 }, { -127,    0,    0 }, {} },
        { { -127, -127, -127 }, { -127,    0,    0 }, {} },
    };

    if ( ! filler.fill_buffer(vertex_buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices, sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        2, 3, 0,
        0, 3, 1,
        6, 7, 4,
        4, 7, 5,
        10, 11, 8,
        8, 11, 9,
        14, 15, 12,
        12, 15, 13,
        18, 19, 16,
        16, 19, 17,
        22, 23, 20,
        20, 23, 21,
    };

    if ( ! filler.fill_buffer(index_buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices, sizeof(indices)))
        return false;

    filler.send_to_gpu();
    return filler.wait_until_done();
}

static bool create_cubic_patch(Buffer* vertex_buffer, Buffer* index_buffer)
{
    HostFiller filler;
    if ( ! filler.init(0x10000))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127,  127 }, { }, {} },
        { {  127,  127,  127 }, { }, {} },
        { {  127, -127,  127 }, { }, {} },
        { { -127, -127,  127 }, { }, {} },
        { { -127,  127, -127 }, { }, {} },
        { {  127,  127, -127 }, { }, {} },
        { {  127, -127, -127 }, { }, {} },
        { { -127, -127, -127 }, { }, {} },
    };

    if ( ! filler.fill_buffer(vertex_buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices, sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        0, 0, 3, 3,
        0, 1, 2, 3,
        4, 5, 6, 7,
        4, 4, 7, 7,
    };

    if ( ! filler.fill_buffer(index_buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices, sizeof(indices)))
        return false;

    filler.send_to_gpu();
    return filler.wait_until_done();
}

static bool create_quadratic_patch(Buffer* vertex_buffer, Buffer* index_buffer)
{
    HostFiller filler;
    if ( ! filler.init(0x10000))
        return false;

    static const Vertex vertices[] = {
        { { -127,  127, -127 }, {}, {} },
        { { -111,  127, -127 }, {}, {} },
        { {  111,  127, -127 }, {}, {} },
        { {  127,  127, -127 }, {}, {} },
        { { -127,  111, -127 }, {}, {} },
        { { -111,  111, -127 }, {}, {} },
        { {  111,  111, -127 }, {}, {} },
        { {  127,  111, -127 }, {}, {} },
        { { -127, -111, -127 }, {}, {} },
        { { -111, -111, -127 }, {}, {} },
        { {  111, -111, -127 }, {}, {} },
        { {  127, -111, -127 }, {}, {} },
        { { -127, -127, -127 }, {}, {} },
        { { -111, -127, -127 }, {}, {} },
        { {  111, -127, -127 }, {}, {} },
        { {  127, -127, -127 }, {}, {} },

        { { -127,  127, -111 }, {}, {} },
        { { -111,  127, -111 }, {}, {} },
        { {  111,  127, -111 }, {}, {} },
        { {  127,  127, -111 }, {}, {} },
        { { -127,  111, -111 }, {}, {} },
        { {                  }, {}, {} }, // unneeded
        { {                  }, {}, {} }, // unneeded
        { {  127,  111, -111 }, {}, {} },
        { { -127, -111, -111 }, {}, {} },
        { {                  }, {}, {} }, // unneeded
        { {                  }, {}, {} }, // unneeded
        { {  127, -111, -111 }, {}, {} },
        { { -127, -127, -111 }, {}, {} },
        { { -111, -127, -111 }, {}, {} },
        { {  111, -127, -111 }, {}, {} },
        { {  127, -127, -111 }, {}, {} },

        { { -127,  127,  111 }, {}, {} },
        { { -111,  127,  111 }, {}, {} },
        { {  111,  127,  111 }, {}, {} },
        { {  127,  127,  111 }, {}, {} },
        { { -127,  111,  111 }, {}, {} },
        { {                  }, {}, {} }, // unneeded
        { {                  }, {}, {} }, // unneeded
        { {  127,  111,  111 }, {}, {} },
        { { -127, -111,  111 }, {}, {} },
        { {                  }, {}, {} }, // unneeded
        { {                  }, {}, {} }, // unneeded
        { {  127, -111,  111 }, {}, {} },
        { { -127, -127,  111 }, {}, {} },
        { { -111, -127,  111 }, {}, {} },
        { {  111, -127,  111 }, {}, {} },
        { {  127, -127,  111 }, {}, {} },

        { { -127,  127,  127 }, {}, {} },
        { { -111,  127,  127 }, {}, {} },
        { {  111,  127,  127 }, {}, {} },
        { {  127,  127,  127 }, {}, {} },
        { { -127,  111,  127 }, {}, {} },
        { { -111,  111,  127 }, {}, {} },
        { {  111,  111,  127 }, {}, {} },
        { {  127,  111,  127 }, {}, {} },
        { { -127, -111,  127 }, {}, {} },
        { { -111, -111,  127 }, {}, {} },
        { {  111, -111,  127 }, {}, {} },
        { {  127, -111,  127 }, {}, {} },
        { { -127, -127,  127 }, {}, {} },
        { { -111, -127,  127 }, {}, {} },
        { {  111, -127,  127 }, {}, {} },
        { {  127, -127,  127 }, {}, {} },
    };

    if ( ! filler.fill_buffer(vertex_buffer, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              vertices, sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        17, 17, 17, // front left-top
        16,  0,  1,
        20,  4,  5,
        17, 17, 18, // front middle-top
         1,  1,  2,
         5,  5,  6,
        18, 18, 18, // front right-top
         2,  3, 19,
         6,  7, 23,
        20,  4,  5, // front left-middle
        20,  4,  5,
        24,  8,  9,
         5,  5,  6, // front middle
         5,  5,  6,
         9,  9, 10,
         6,  7, 23, // front right-mddile
         6,  7, 23,
        10, 11, 27,
        24,  8,  9, // front left-bottom
        28, 12, 13,
        29, 29, 29,
         9,  9, 10, // front middle-bottom
        13, 13, 14,
        29, 29, 30,
        10, 11, 27, // front right-bottom
        14, 15, 31,
        30, 30, 30,
        36, 36, 20, // left side middle
        36, 36, 20,
        40, 40, 24,
        33, 33, 17, // left side top
        32, 32, 16,
        36, 36, 20,
        40, 40, 24, // left side bottom
        44, 44, 28,
        45, 45, 29,
        33, 33, 34, // top middle
        33, 33, 34,
        17, 17, 18,
        18, 18, 34, // right side top
        19, 19, 35,
        23, 23, 39,
        23, 23, 39, // right side middle
        23, 23, 39,
        27, 27, 43,
        27, 27, 43, // right side bottom
        31, 31, 47,
        30, 30, 46,
        29, 29, 30, // bottom middle
        45, 45, 46,
        45, 45, 46,
        33, 33, 33, // back left-top
        49, 48, 32,
        53, 52, 36,
        34, 34, 33, // back middle-top
        50, 50, 49,
        54, 54, 53,
        34, 34, 34, // back right-top
        35, 51, 50,
        39, 55, 54,
        39, 55, 54, // back right-middle
        39, 55, 54,
        43, 59, 58,
        43, 59, 58, // back right-bottom
        47, 63, 62,
        46, 46, 46,
        45, 45, 46, // back middle-bottom
        61, 61, 62,
        57, 57, 58,
        40, 40, 40, // back left-bottom
        56, 60, 44,
        57, 61, 45,
        57, 57, 53, // back left-middle
        56, 56, 52,
        40, 40, 36,
        54, 54, 53, // back middle
        54, 54, 53,
        58, 58, 57,
    };

    if ( ! filler.fill_buffer(index_buffer, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              indices, sizeof(indices)))
        return false;

    filler.send_to_gpu();
    return filler.wait_until_done();
}

static constexpr VkClearValue make_clear_color(float r, float g, float b, float a)
{
    VkClearValue value = { };
    value.color.float32[0] = r;
    value.color.float32[1] = g;
    value.color.float32[2] = b;
    value.color.float32[3] = a;
    return value;
}

static constexpr VkClearValue make_clear_depth(float depth, uint32_t stencil)
{
    VkClearValue value = { };
    value.depthStencil.depth   = depth;
    value.depthStencil.stencil = stencil;
    return value;
}

float user_roundedness = 111.0f / 127.0f;

static bool dummy_draw(uint32_t image_idx, uint64_t time_ms, VkFence queue_fence)
{
    static Buffer vertex_buffer;
    static Buffer index_buffer;
    VkResult      res;

    if ( ! create_gui_frame())
        return false;

    if ( ! vertex_buffer.allocated()) {
        if (what_geometry == geom_cube) {
            if ( ! create_cube(&vertex_buffer, &index_buffer))
                return false;
        }
        else if (what_geometry == geom_cubic_patch) {
            if ( ! create_cubic_patch(&vertex_buffer, &index_buffer))
                return false;
        }
        else if (what_geometry == geom_quadratic_patch) {
            if ( ! create_quadratic_patch(&vertex_buffer, &index_buffer))
                return false;
        }
    }

    // Allocate descriptor set
    static VkDescriptorSet desc_set[max_swapchain_size] = { VK_NULL_HANDLE };
    if ( ! desc_set[0]) {
        static VkDescriptorPoolSize pool_sizes[] = {
            {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,      // type
                mstd::array_size(desc_set)              // descriptorCount
            }
        };

        static VkDescriptorPoolCreateInfo pool_create_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            nullptr,
            0,                                      // flags
            mstd::array_size(desc_set),             // maxSets
            mstd::array_size(pool_sizes),
            pool_sizes
        };

        VkDescriptorPool desc_set_pool;

        res = CHK(vkCreateDescriptorPool(vk_dev, &pool_create_info, nullptr, &desc_set_pool));
        if (res != VK_SUCCESS)
            return false;

        static VkDescriptorSetLayout       layouts[max_swapchain_size];
        static VkDescriptorSetAllocateInfo alloc_info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            nullptr,
            VK_NULL_HANDLE,             // descriptorPool
            mstd::array_size(layouts),  // descriptorSetCount
            layouts                     // pSetLayouts
        };

        alloc_info.descriptorPool = desc_set_pool;

        for (uint32_t i = 0; i < mstd::array_size(layouts); i++)
            layouts[i] = vk_desc_set_layout;

        res = CHK(vkAllocateDescriptorSets(vk_dev, &alloc_info, desc_set));
        if (res != VK_SUCCESS)
            return false;
    }

    // Create shader data
    static Buffer shader_data;
    struct UniformBuffer {
        vmath::mat4 model_view_proj;  // transforms to camera space for rasterization
        vmath::mat4 model;            // transforms to world space for lighting
        vmath::mat3 model_normal;     // inverse transpose for transforming normals to world space
        vmath::vec4 color;            // object color
        vmath::vec4 lights[1];        // light positions in world space
    };
    static uint32_t     slot_size;
    static Map<uint8_t> host_shader_data;
    if ( ! shader_data.allocated()) {
        slot_size = mstd::align_up(static_cast<uint32_t>(sizeof(UniformBuffer)),
                                   static_cast<uint32_t>(phys_props.properties.limits.minUniformBufferOffsetAlignment));
        const uint32_t total_size = slot_size * mstd::array_size(desc_set);
        if ( ! shader_data.allocate(vk_coherent_heap, total_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return false;

        host_shader_data = Map<uint8_t>(&vk_coherent_heap, 0, total_size);
        if ( ! host_shader_data.mapped())
            return false;
    }

    // Calculate matrices
    const auto uniform_data = reinterpret_cast<UniformBuffer*>(&host_shader_data[slot_size * image_idx]);
    const float angle = vmath::radians(static_cast<float>(time_ms) * 15.0f / 1000.0f);
    const vmath::mat4 model_view = vmath::mat4(vmath::quat(vmath::vec3(0.70710678f, 0.70710678f, 0), angle))
                                 * vmath::translate(0.0f, 0.0f, 7.0f);
    const vmath::mat4 proj = vmath::projection(
            static_cast<float>(vk_surface_caps.currentExtent.width)     // aspect
                / static_cast<float>(vk_surface_caps.currentExtent.height),
            vmath::radians(30.0f),  // fov
            0.01f,                  // near_plane
            100.0f,                 // far_plane
            0.0f);                  // depth_bias
    uniform_data->model_view_proj = model_view * proj;
    uniform_data->model           = model_view;
    uniform_data->model_normal    = vmath::transpose(vmath::inverse(vmath::mat3(model_view)));
    uniform_data->color           = vmath::vec<4>(0.4f, 0.6f, 0.1f, user_roundedness);
    uniform_data->lights[0]       = vmath::vec<4>(5.0f, 5.0f, -5.0f, 1.0f);

    // Send matrices to GPU
    if ( ! host_shader_data.flush(slot_size * image_idx, slot_size))
        return false;

    // Update descriptor set
    static VkDescriptorBufferInfo buffer_info = {
        VK_NULL_HANDLE,     // buffer
        0,                  // offset
        0                   // range
    };
    buffer_info.buffer = shader_data.get_buffer();
    buffer_info.offset = slot_size * image_idx;
    buffer_info.range  = slot_size;
    static VkWriteDescriptorSet write_desc_sets[] = {
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            VK_NULL_HANDLE,                     // dstSet
            0,                                  // dstBinding
            0,                                  // dstArrayElement
            1,                                  // descriptorCount
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  // descriptorType
            nullptr,                            // pImageInfo
            &buffer_info,                       // pBufferInfo
            nullptr                             // pTexelBufferView
        }
    };
    write_desc_sets[0].dstSet = desc_set[image_idx];

    vkUpdateDescriptorSets(vk_dev,
                           mstd::array_size(write_desc_sets),
                           write_desc_sets,
                           0,           // descriptorCopyCount
                           nullptr);    // pDescriptorCopies

    // Render image
    Image& image = vk_swapchain_images[image_idx];

    static CommandBuffers<2 * mstd::array_size(vk_swapchain_images)> bufs;

    if ( ! allocate_command_buffers_once(&bufs))
        return false;

    const VkCommandBuffer buf = bufs.bufs[image_idx];

    if ( ! reset_and_begin_command_buffer(buf))
        return false;

    static const Image::Transition color_att_init = {
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    image.set_image_layout(buf, color_att_init);

    if (vk_depth_buffers[image_idx].layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        static const Image::Transition depth_init = {
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            0,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };

        vk_depth_buffers[image_idx].set_image_layout(buf, depth_init);
    }

    static const VkClearValue clear_values[2] = {
        make_clear_color(0, 0, 0, 0),
        make_clear_depth(0, 0)
    };

    static VkRenderPassBeginInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        nullptr,
        VK_NULL_HANDLE,     // renderPass
        VK_NULL_HANDLE,     // framebuffer
        { },
        mstd::array_size(clear_values),
        clear_values
    };

    render_pass_info.renderPass        = vk_render_pass;
    render_pass_info.framebuffer       = vk_frame_buffers[image_idx];
    render_pass_info.renderArea.extent = vk_surface_caps.currentExtent;

    vkCmdBeginRenderPass(buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_gr_pipeline);

    static const VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(buf,
                           0,   // firstBinding
                           1,   // bindingCount
                           &vertex_buffer.get_buffer(),
                           &vb_offset);

    vkCmdBindIndexBuffer(buf,
                         index_buffer.get_buffer(),
                         0,     // offset
                         VK_INDEX_TYPE_UINT16);

    vkCmdBindDescriptorSets(buf,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_gr_pipeline_layout,
                            0,          // firstSet
                            1,          // descriptorSetCount
                            &desc_set[image_idx],
                            0,          // dynamicOffsetCount
                            nullptr);   // pDynamicOffsets

    constexpr uint32_t index_count =
        (what_geometry == geom_cube)            ? 36 :
        (what_geometry == geom_cubic_patch)     ? 16 :
        (what_geometry == geom_quadratic_patch) ? 78 * 3 :
        0;

    vkCmdDrawIndexed(buf,
                     index_count,
                     1,     // instanceCount
                     0,     // firstVertex
                     0,     // vertexOffset
                     0);    // firstInstance

    if ( ! send_gui_to_gpu(buf))
        return false;

    vkCmdEndRenderPass(buf);

    static const Image::Transition color_att_present = {
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    image.set_image_layout(buf, color_att_present);

    res = CHK(vkEndCommandBuffer(buf));
    if (res != VK_SUCCESS)
        return false;

    static const VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    static VkSubmitInfo submit_info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        1,                      // waitSemaphoreCount
        &vk_sems[sem_acquire],  // pWaitSemaphores
        &dst_stage,             // pWaitDstStageMask
        1,                      // commandBufferCount
        nullptr,                // pCommandBuffers
        1,                      // signalSemaphoreCount
        &vk_sems[sem_acquire]   // pSignalSemaphores
    };

    submit_info.pCommandBuffers = &buf;

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, queue_fence));
    if (res != VK_SUCCESS)
        return false;

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

    if ( ! vk_coherent_heap.allocate_heap(coherent_heap_size))
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

    if ( ! load_shaders())
        return false;

    if ( ! create_pipeline_layouts())
        return false;

    if ( ! create_graphics_pipelines())
        return false;

    if ( ! init_gui())
        return false;

    return true;
}

bool draw_frame()
{
    uint32_t image_idx;
    VkResult res;

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

    if ( ! dummy_draw(image_idx, cur_abs_time - base_abs_time, vk_fens[fen_queue]))
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
