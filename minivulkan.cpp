// SPDX-License-Identifier: MIT
// Copyright (c) 2021 Chris Dragan

#include "minivulkan.h"
#include "vulkan_extensions.h"
#include "mstdc.h"
#include <assert.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <dlfcn.h>

#ifdef NDEBUG
#   define dprintf(...)
#else
#   include <stdio.h>
#   include <string.h>
#   define dprintf printf
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
    for (uint32_t i = 0; i < num_ext_props; i++)
        dprintf("instance extension %s\n", ext_props[i].extensionName);
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
        (PFN_vkEnumerateInstanceLayerProperties)
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties");

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

static const float queue_priorities[] = { 1 };

static constexpr uint32_t no_queue_family = ~0u;

static VkDeviceQueueCreateInfo queue_create_info = {
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    nullptr,
    0,
    no_queue_family, // queueFamilyIndex
    1,               // queueCount
    queue_priorities
};

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
    VkSurfaceFormatKHR formats[32];

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

                queue_create_info.queueFamilyIndex = i_queue;
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

static VkDevice    vk_dev                   = VK_NULL_HANDLE;
static VkQueue     vk_queue                 = VK_NULL_HANDLE;
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

static bool create_device()
{
    if ( ! find_gpu())
        return false;

    if ( ! get_device_extensions())
        return false;

    static VkDeviceCreateInfo dev_create_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        nullptr,
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

    vkGetDeviceQueue(vk_dev, queue_create_info.queueFamilyIndex, 0, &vk_queue);

    return true;
}

class DeviceMemoryHeap {
    public:
        enum Location {
            device_memory,
            host_memory
        };

        DeviceMemoryHeap() = default;
        DeviceMemoryHeap(Location loc) : host_visible(loc == host_memory) { }
        DeviceMemoryHeap(const DeviceMemoryHeap&) = delete;
        DeviceMemoryHeap& operator=(const DeviceMemoryHeap&) = delete;

        bool allocate_heap_if_empty(const VkMemoryRequirements& requirements);
        bool allocate_heap(VkDeviceSize size);
        void free_heap();
        bool allocate_memory(const VkMemoryRequirements& requirements, VkDeviceSize* offset);

        VkDeviceMemory  get_memory() const { return memory; }
        bool            is_host_memory() const { return host_visible; }
        static uint32_t get_memory_type() { return device_memory_type; }

    private:
        static bool init_heap_info();

        static uint32_t device_memory_type;
        static uint32_t host_memory_type;

        VkDeviceMemory  memory         = VK_NULL_HANDLE;
        VkDeviceSize    next_free_offs = 0;
        VkDeviceSize    heap_size      = 0;
        bool            host_visible   = false;
        bool            mapped         = false;

        friend class MapBase;
};

uint32_t DeviceMemoryHeap::device_memory_type = ~0u;
uint32_t DeviceMemoryHeap::host_memory_type   = ~0u;

static constexpr uint32_t alloc_heap_size = 64u * 1024u * 1024u;
static DeviceMemoryHeap vk_device_heap;

bool DeviceMemoryHeap::init_heap_info()
{
    if (device_memory_type != ~0u)
        return true;

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_phys_dev, &mem_props);

    int device_type_index = -1;
    int host_type_index   = -1;

    dprintf("Memory heaps\n");
    for (int i = 0; i < static_cast<int>(mem_props.memoryTypeCount); i++) {
#ifndef NDEBUG
        static char info[64];
        info[0] = 0;
        if (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            strcat(info, "device,");
        if (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            strcat(info, "host_visible,");
        if (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            strcat(info, "host_coherent,");
        if (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            strcat(info, "host_cached,");
        dprintf("    type %d, heap %u, flags 0x%x (%s)\n",
                i,
                mem_props.memoryTypes[i].heapIndex,
                mem_props.memoryTypes[i].propertyFlags,
                info);
#endif

        if (device_type_index == -1 &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            device_type_index = i;

        const uint32_t host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                  | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                  | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if ((mem_props.memoryTypes[i].propertyFlags & host_flags) == host_flags) {
            if (host_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                device_type_index = i;
                host_type_index   = i;
            }
            else if (host_type_index == -1)
                host_type_index = i;
        }
    }

    if (host_type_index == -1) {
        dprintf("Could not find coherent and cached host memory type\n");
        return false;
    }

    device_memory_type = (uint32_t)device_type_index;
    host_memory_type   = (uint32_t)host_type_index;

    return true;
}

bool DeviceMemoryHeap::allocate_heap_if_empty(const VkMemoryRequirements& requirements)
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

    size = mstd::align_up(size, VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));

    static VkMemoryAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        0,  // allocationSize
        0   // memoryTypeIndex
    };
    alloc_info.allocationSize  = size;
    alloc_info.memoryTypeIndex = host_visible ? host_memory_type : device_memory_type;

    const VkResult res = CHK(vkAllocateMemory(vk_dev, &alloc_info, nullptr, &memory));
    if (res != VK_SUCCESS)
        return false;

    heap_size = size;
    dprintf("Allocated %s heap size 0x%" PRIx64 " bytes\n",
            host_visible ? "host" : "device",
            static_cast<uint64_t>(size));

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

static constexpr VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

class MapBase {
    public:
        MapBase(const MapBase&) = delete;
        MapBase& operator=(const MapBase&) = delete;

        bool mapped() const { return mapped_ptr != nullptr; }
        void unmap();

    protected:
        MapBase() = default;
        MapBase(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size);
        ~MapBase() { unmap(); }

        void move_from(MapBase& map);
        void* get_ptr() { return mapped_ptr; }
        const void* get_ptr() const { return mapped_ptr; }
        void* get_end_ptr() { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mapped_ptr) + mapped_size); }
        const void* get_end_ptr() const { return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mapped_ptr) + mapped_size); }

    private:
        DeviceMemoryHeap* mapped_heap = nullptr;
        void*             mapped_ptr  = nullptr;
        VkDeviceSize      mapped_size = 0;
};

MapBase::MapBase(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size)
{
    assert( ! heap->mapped);
    assert((offset % phys_props.properties.limits.minMemoryMapAlignment) == 0);

    const VkDeviceSize aligned_size = mstd::align_up(size,
            VkDeviceSize(phys_props.properties.limits.minMemoryMapAlignment));

    void* ptr;
    const VkResult res = CHK(vkMapMemory(vk_dev, heap->get_memory(), offset, aligned_size, 0, &ptr));
    if (res == VK_SUCCESS) {
        mapped_heap  = heap;
        mapped_ptr   = ptr;
        mapped_size  = size;
        heap->mapped = true;
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

        mapped_heap = nullptr;
        mapped_ptr  = nullptr;
        mapped_size = 0;
    }
    else {
        assert( ! mapped_ptr);
        assert( ! mapped_size);
    }
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

template<typename T>
class Map: public MapBase {
    public:
        Map() = default;
        Map(DeviceMemoryHeap* heap, VkDeviceSize offset, VkDeviceSize size)
            : MapBase(heap, offset, size) { }

        Map(Map&& map) { move_from(map); }
        Map& operator=(Map&& map) {
            move_from(map);
            return *this;
        }

        uint32_t size() const { return end() - begin(); }

        T* data() { return static_cast<T*>(get_ptr()); }
        const T* data() const { return static_cast<const T*>(get_ptr()); }

        operator T*() { return static_cast<T*>(get_ptr()); }
        operator const T*() const { return static_cast<const T*>(get_ptr()); }

        T& operator[](uintptr_t i) { return static_cast<T*>(get_ptr())[i]; }
        const T& operator[](uintptr_t i) const { return static_cast<const T*>(get_ptr())[i]; }

        T* begin() { return static_cast<T*>(get_ptr()); }
        T* end()   { return static_cast<T*>(get_end_ptr()); }

        const T* begin() const { return static_cast<const T*>(get_ptr()); }
        const T* end()   const { return static_cast<const T*>(get_end_ptr()); }

        const T* cbegin() const { return static_cast<const T*>(get_ptr()); }
        const T* cend()   const { return static_cast<const T*>(get_end_ptr()); }
};

class Resource {
    public:
        Resource() = default;
        Resource(const Resource&) = delete;
        Resource& operator=(const Resource&) = delete;

        template<typename T>
        Map<T> map() const { return Map<T>(owning_heap, heap_offset, bytes); }

        bool allocated() const { return !! bytes; }

    protected:
        DeviceMemoryHeap* owning_heap = nullptr;
        VkDeviceSize      heap_offset = 0;
        VkDeviceSize      bytes       = 0;
};

struct ImageInfo {
    uint32_t           width;
    uint32_t           height;
    VkFormat           format;
    uint32_t           mip_levels;
    VkImageAspectFlags aspect;
    VkImageUsageFlags  usage;
};

class Image: public Resource {
    public:
        VkImageLayout layout = initial_layout;

        Image() = default;

        operator VkImage() const { return image; }
        operator VkImageView() const { return view; }

        bool allocate(DeviceMemoryHeap& heap, const ImageInfo& image_info);
        void destroy();

        // Used with swapchains
        void set(VkImage new_image) {
            assert(image == VK_NULL_HANDLE);
            image = new_image;
        }
        void set(VkImageView new_view) {
            assert(view == VK_NULL_HANDLE);
            view = new_view;
        }

    private:
        VkImage     image = VK_NULL_HANDLE;
        VkImageView view  = VK_NULL_HANDLE;
};

bool Image::allocate(DeviceMemoryHeap& heap, const ImageInfo& image_info)
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
        &queue_create_info.queueFamilyIndex,
        initial_layout
    };
    create_info.format        = image_info.format;
    create_info.extent.width  = image_info.width;
    create_info.extent.height = image_info.height;
    create_info.mipLevels     = image_info.mip_levels;
    create_info.tiling        = heap.is_host_memory() ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    create_info.usage         = image_info.usage;

    VkResult res = CHK(vkCreateImage(vk_dev, &create_info, nullptr, &image));
    if (res != VK_SUCCESS)
        return false;

    layout = initial_layout;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk_dev, image, &requirements);

#ifndef NDEBUG
    if ( ! (requirements.memoryTypeBits & (1u << heap.get_memory_type()))) {
        dprintf("Device memory does not support requested image type\n");
        return false;
    }
#endif

    if ( ! heap.allocate_heap_if_empty(requirements))
        return false;

    VkDeviceSize offset;
    if ( ! heap.allocate_memory(requirements, &offset))
        return false;

    res = CHK(vkBindImageMemory(vk_dev, image, heap.get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    owning_heap = &heap;
    heap_offset = offset;
    bytes       = requirements.size;

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
            1, // levelCount
            0, // baseArrayLayer
            1  // layerCount
        }
    };
    view_create_info.image                       = image;
    view_create_info.format                      = image_info.format;
    view_create_info.subresourceRange.aspectMask = image_info.aspect;
    view_create_info.subresourceRange.levelCount = image_info.mip_levels;

    res = CHK(vkCreateImageView(vk_dev, &view_create_info, nullptr, &view));
    return res == VK_SUCCESS;
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
    bytes       = 0;
}

class TempHostImage: public Image {
    public:
        TempHostImage() = default;
        ~TempHostImage();

        bool allocate(const ImageInfo& image_info) {
            return Image::allocate(heap, image_info);
        }

    private:
        DeviceMemoryHeap heap{DeviceMemoryHeap::host_memory};
};

TempHostImage::~TempHostImage()
{
    destroy();
    heap.free_heap();
}

class Buffer: public Resource {
    public:
        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        operator VkBuffer() const { return buffer; }
        operator VkBufferView() const { return view; }

        bool allocate(DeviceMemoryHeap&  heap,
                      uint32_t           alloc_size,
                      VkFormat           format,
                      VkBufferUsageFlags usage);
        void destroy();

    private:
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkBufferView view   = VK_NULL_HANDLE;
};

bool Buffer::allocate(DeviceMemoryHeap&  heap,
                      uint32_t           alloc_size,
                      VkFormat           format,
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
        &queue_create_info.queueFamilyIndex
    };
    create_info.size  = alloc_size;
    create_info.usage = usage;

    VkResult res = CHK(vkCreateBuffer(vk_dev, &create_info, nullptr, &buffer));
    if (res != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk_dev, buffer, &requirements);

#ifndef NDEBUG
    if ( ! (requirements.memoryTypeBits & (1u << heap.get_memory_type()))) {
        dprintf("Device memory does not support requested buffer type\n");
        return false;
    }
#endif

    if ( ! heap.allocate_heap_if_empty(requirements))
        return false;

    VkDeviceSize offset;
    if ( ! heap.allocate_memory(requirements, &offset))
        return false;

    res = CHK(vkBindBufferMemory(vk_dev, buffer, heap.get_memory(), offset));
    if (res != VK_SUCCESS)
        return false;

    owning_heap = &heap;
    heap_offset = offset;
    bytes       = requirements.size;

    if (format == VK_FORMAT_UNDEFINED)
        return true;

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
    view_create_info.offset = offset;

    res = CHK(vkCreateBufferView(vk_dev, &view_create_info, nullptr, &view));
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
    bytes       = 0;
}

class TempHostBuffer: public Buffer {
    public:
        TempHostBuffer() = default;
        ~TempHostBuffer();

        bool allocate(uint32_t           alloc_size,
                      VkFormat           format,
                      VkBufferUsageFlags usage) {
            return Buffer::allocate(heap, alloc_size, format, usage);
        }

    private:
        DeviceMemoryHeap heap{DeviceMemoryHeap::host_memory};
};

TempHostBuffer::~TempHostBuffer()
{
    destroy();
    heap.free_heap();
}

enum eSemId {
    sem_acquire,

    num_semaphores
};

static VkSemaphore vk_sems[num_semaphores];

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

enum eFenceId {
    fen_acquire,
    fen_copy_to_dev,

    num_fences
};

static VkFence vk_fens[num_fences];

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

static VkSurfaceCapabilitiesKHR vk_surface_caps;

static VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;

static Image            vk_swapchain_images[8];
static Image            vk_depth_buffers[mstd::array_size(vk_swapchain_images)];
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
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT
    };
    if ( ! find_optimal_tiling_format(depth_formats,
                                      mstd::array_size(depth_formats),
                                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                      &vk_depth_format)) {
        dprintf("error: could not find any of the required depth formats\n");
        return false;
    }

    const uint32_t aligned_pitch = mstd::align_up(width * 4, 4u * 1024u);
    const uint32_t heap_size     = mstd::align_up(aligned_pitch * height, 64u * 1024u) * num_depth_buffers;

    if ( ! depth_buffer_heap.allocate_heap(heap_size))
        return false;

    for (uint32_t i = 0; i < num_depth_buffers; i++) {

        static ImageInfo image_info = {
            0, // width
            0, // height
            VK_FORMAT_UNDEFINED,
            1, // mip_levels
            VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        };
        image_info.width  = width;
        image_info.height = height;
        image_info.format = vk_depth_format;

        if ( ! vk_depth_buffers[i].allocate(depth_buffer_heap, image_info))
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

    res = CHK(vkCreateSwapchainKHR(vk_dev, &swapchain_create_info, nullptr, &vk_swapchain));

    if (res != VK_SUCCESS)
        return false;

    if (old_swapchain != VK_NULL_HANDLE) {
        for (const Image& image : vk_swapchain_images) {
            const VkImageView view = image;
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

    res = CHK(vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &num_images, images));

    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;

    for (uint32_t i = 0; i < num_images; i++) {
        Image& image = vk_swapchain_images[i];
        image.set(images[i]);

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
        view_create_info.image  = image;
        view_create_info.format = swapchain_create_info.imageFormat;

        VkImageView view;
        res = CHK(vkCreateImageView(vk_dev, &view_create_info, nullptr, &view));
        if (res != VK_SUCCESS)
            return false;

        vk_swapchain_images[i].set(view);
    }

    return allocate_depth_buffers(num_images);
}

static VkRenderPass vk_render_pass = VK_NULL_HANDLE;

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
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        },
        {
            0, // flags
            VK_FORMAT_UNDEFINED,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL
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

static VkFramebuffer vk_frame_buffer = VK_NULL_HANDLE;

static bool create_frame_buffer()
{
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

    attachments[0] = vk_swapchain_images[0];
    attachments[1] = vk_depth_buffers[0];

    const VkResult res = CHK(vkCreateFramebuffer(vk_dev, &frame_buffer_info, nullptr, &vk_frame_buffer));
    return res == VK_SUCCESS;
}

static VkPipelineLayout vk_gr_pipeline_layout = VK_NULL_HANDLE;
static VkPipeline       vk_gr_pipeline        = VK_NULL_HANDLE;

static
#include "simple.vert.h"

static
#include "phong.frag.h"

#define DEFINE_SHADERS \
    X(simple_vert) \
    X(phong_frag)

static const struct {
    const uint32_t* code;
    uint32_t        size;
} spirv[] =
{
#define X(shader) { shader, mstd::array_size(shader) },
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

static bool create_pipeline_layouts()
{
    static const VkDescriptorSetLayoutBinding create_binding = {
        0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr
    };

    static const VkDescriptorSetLayoutCreateInfo create_desc_set_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        nullptr,
        0, // flags
        1,
        &create_binding
    };

    static VkDescriptorSetLayout desc_set_layout;
    VkResult res = CHK(vkCreateDescriptorSetLayout(vk_dev, &create_desc_set_layout, nullptr, &desc_set_layout));
    if (res != VK_SUCCESS)
        return false;

    static VkPipelineLayoutCreateInfo layout_create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        nullptr,
        0,      // flags
        1,
        &desc_set_layout,
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

static bool create_graphics_pipelines()
{
    if (vk_gr_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_dev, vk_gr_pipeline, nullptr);
        vk_gr_pipeline = VK_NULL_HANDLE;
    }

    static VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,          // flags
            VK_SHADER_STAGE_VERTEX_BIT,
            nullptr,    // module
            "main",     // pName
            nullptr     // pSpecializationInfo
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0,          // flags
            VK_SHADER_STAGE_FRAGMENT_BIT,
            nullptr,    // module
            "main",     // pName
            nullptr     // pSpecializationInfo
        }
    };
    shader_stages[0].module = shaders[0];
    shader_stages[1].module = shaders[1];

    static VkVertexInputBindingDescription vertex_bindings[] = {
        {
            0,
            sizeof(Vertex),
            VK_VERTEX_INPUT_RATE_VERTEX
        }
    };

    static VkVertexInputAttributeDescription vertex_attributes[] = {
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

    static VkPipelineVertexInputStateCreateInfo vertex_input_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        mstd::array_size(vertex_bindings),
        vertex_bindings,
        mstd::array_size(vertex_attributes),
        vertex_attributes
    };

    static VkPipelineInputAssemblyStateCreateInfo input_assembly_state = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,
        0,  // flags
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE
    };

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
        VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_FALSE,   // depthBoundsTestEnable
        VK_FALSE,   // stencilTestEnable
        { },        // front
        { },        // back
        0,          // minDepthBounds
        0           // maxDepthBounds
    };

    static VkPipelineColorBlendAttachmentState color_blend_att = {
        VK_FALSE,   // blendEnable
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
        mstd::array_size(shader_stages),
        shader_stages,
        &vertex_input_state,
        &input_assembly_state,
        nullptr,        // pTessellationState
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

static bool update_resolution()
{
    vkDestroyFramebuffer(vk_dev, vk_frame_buffer, nullptr);
    vk_frame_buffer = VK_NULL_HANDLE;

    if ( ! create_swapchain())
        return false;

    if ( ! create_frame_buffer())
        return false;

    if ( ! create_graphics_pipelines())
        return false;

    return true;
}

struct CommandBuffersBase {
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer bufs[1];
};

static bool allocate_command_buffers(CommandBuffersBase* bufs, uint32_t num_buffers)
{
    assert(bufs->pool == VK_NULL_HANDLE);

    static VkCommandPoolCreateInfo create_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };

    create_info.queueFamilyIndex = queue_create_info.queueFamilyIndex;

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

template<uint32_t num_buffers>
struct CommandBuffers: public CommandBuffersBase {
    static uint32_t size() { return num_buffers; }

    private:
        VkCommandBuffer tail[num_buffers - 1];
};

template<>
struct CommandBuffers<1>: public CommandBuffersBase {
    static uint32_t size() { return 1; }
};

template<uint32_t num_buffers>
static bool allocate_command_buffers(CommandBuffers<num_buffers>* bufs)
{
    return allocate_command_buffers(bufs, num_buffers);
}

template<uint32_t num_buffers>
static bool allocate_command_buffers_if_empty(CommandBuffers<num_buffers>* bufs)
{
    if (bufs->pool)
        return true;

    return allocate_command_buffers(bufs, num_buffers);
}

static bool fill_buffer(Buffer*            buffer,
                        VkFormat           format,
                        VkBufferUsageFlags usage,
                        const void*        data,
                        uint32_t           size)
{
    if ( ! buffer->allocate(vk_device_heap,
                            size,
                            format,
                            usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    TempHostBuffer host_buffer;
    if ( ! host_buffer.allocate(size, format, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;

    {
        Map<uint8_t> map = host_buffer.map<uint8_t>();
        if ( ! map.mapped())
            return false;

        mstd::mem_copy(map.data(), data, size);
    }

    static CommandBuffers<1> aux_cmd_buf;

    if ( ! allocate_command_buffers_if_empty(&aux_cmd_buf))
        return false;

    VkResult res = CHK(vkResetCommandBuffer(aux_cmd_buf.bufs[0], 0));
    if (res != VK_SUCCESS)
        return false;

    static VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };

    res = CHK(vkBeginCommandBuffer(aux_cmd_buf.bufs[0], &begin_info));
    if (res != VK_SUCCESS)
        return false;

    static VkBufferCopy copy_region = {
        0, // srcOffset
        0, // dstOffset
        0  // size
    };

    copy_region.size = size;

    vkCmdCopyBuffer(aux_cmd_buf.bufs[0], host_buffer, *buffer, 1, &copy_region);

    res = CHK(vkEndCommandBuffer(aux_cmd_buf.bufs[0]));
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
        aux_cmd_buf.bufs,
        0,                  // signalSemaphoreCount
        nullptr             // pSignalSemaphores
    };

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, vk_fens[fen_copy_to_dev]));
    if (res != VK_SUCCESS)
        return false;

    res = CHK(vkWaitForFences(vk_dev, 1, &vk_fens[fen_copy_to_dev], VK_TRUE, 1'000'000'000));
    if (res != VK_SUCCESS)
        return false;

    res = CHK(vkResetFences(vk_dev, 1, &vk_fens[fen_copy_to_dev]));
    if (res != VK_SUCCESS)
        return false;

    return true;
}

static bool create_cube(Buffer* vertex_buffer, Buffer* index_buffer)
{
    static const Vertex vertices[] = {
        { { -127,  127, 0 }, { 0, 0, -127 }, {} },
        { {  127,  127, 0 }, { 0, 0, -127 }, {} },
        { { -127, -127, 0 }, { 0, 0, -127 }, {} },
        { {  127, -127, 0 }, { 0, 0, -127 }, {} },
    };

    if ( ! fill_buffer(vertex_buffer, VK_FORMAT_UNDEFINED, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       vertices, sizeof(vertices)))
        return false;

    static const uint16_t indices[] = {
        2, 3, 0,
        0, 3, 1
    };

    return fill_buffer(index_buffer, VK_FORMAT_UNDEFINED, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       indices, sizeof(indices));
}

static bool dummy_draw(uint32_t image_idx)
{
    static Buffer vertex_buffer;
    static Buffer index_buffer;

    if ( ! vertex_buffer.allocated()) {
        if ( ! create_cube(&vertex_buffer, &index_buffer))
            return false;
    }

    if (vk_swapchain_images[image_idx].layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        return true;

    dprintf("dummy_draw for image %u\n", image_idx);

    const VkImage image = vk_swapchain_images[image_idx];

    VkResult res;

    static CommandBuffers<2 * mstd::array_size(vk_swapchain_images)> bufs;

    if ( ! allocate_command_buffers_if_empty(&bufs))
        return false;

    static uint32_t         cmd_buf_idx = 0;
    static VkCommandBuffer* cur_buf_ptr = nullptr;
    cur_buf_ptr = &bufs.bufs[cmd_buf_idx];

    const VkCommandBuffer buf = *cur_buf_ptr;
    cmd_buf_idx = (cmd_buf_idx + 1) % bufs.size();

    res = CHK(vkResetCommandBuffer(buf, 0));
    if (res != VK_SUCCESS)
        return false;

    static VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr
    };

    res = CHK(vkBeginCommandBuffer(buf, &begin_info));
    if (res != VK_SUCCESS)
        return false;

    static VkImageMemoryBarrier img_barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    img_barrier.oldLayout = vk_swapchain_images[image_idx].layout;
    vk_swapchain_images[image_idx].layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    img_barrier.srcQueueFamilyIndex         = queue_create_info.queueFamilyIndex;
    img_barrier.dstQueueFamilyIndex         = queue_create_info.queueFamilyIndex;
    img_barrier.image                       = image;
    img_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    img_barrier.subresourceRange.levelCount = 1;
    img_barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(buf,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &img_barrier);

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

    submit_info.pCommandBuffers = cur_buf_ptr;

    res = CHK(vkQueueSubmit(vk_queue, 1, &submit_info, VK_NULL_HANDLE));
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

    return true;
}

bool draw_frame()
{
    static uint64_t frame_idx = 0;
    uint32_t        image_idx;
    VkResult        res;

    if (frame_idx++) {
        res = CHK(vkWaitForFences(vk_dev, 1, &vk_fens[fen_acquire], VK_TRUE, 1'000'000'000));
        if (res != VK_SUCCESS)
            return false;

        res = CHK(vkResetFences(vk_dev, 1, &vk_fens[fen_acquire]));
        if (res != VK_SUCCESS)
            return false;
    }

    for (;;) {

        res = CHK(vkAcquireNextImageKHR(vk_dev,
                                        vk_swapchain,
                                        1'000'000'000,
                                        vk_sems[sem_acquire],
                                        vk_fens[fen_acquire],
                                        &image_idx));
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)
            break;

        if (res != VK_ERROR_OUT_OF_DATE_KHR)
            return false;

        if ( ! update_resolution())
            return false;
    }

    // TODO draw
    if ( ! dummy_draw(image_idx))
        return false;

    dprintf("present frame %" PRIu64 " image %u\n", frame_idx - 1, image_idx);

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

    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR) {
        if ( ! update_resolution())
            return false;
        res = VK_SUCCESS;
    }

    if (res != VK_SUCCESS)
        return false;

    return true;
}
