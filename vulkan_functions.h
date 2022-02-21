// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

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
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
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
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkCreateRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkCreateBufferView) \
    X(vkDestroyBufferView) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBuffer)

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
#define vkGetPhysicalDeviceFormatProperties       ((PFN_vkGetPhysicalDeviceFormatProperties)       vk_instance_functions[ id_vkGetPhysicalDeviceFormatProperties])
#define vkGetPhysicalDeviceMemoryProperties       ((PFN_vkGetPhysicalDeviceMemoryProperties)       vk_instance_functions[ id_vkGetPhysicalDeviceMemoryProperties])
#define vkEnumerateDeviceExtensionProperties      ((PFN_vkEnumerateDeviceExtensionProperties)      vk_instance_functions[ id_vkEnumerateDeviceExtensionProperties])
#define vkCreateMetalSurfaceEXT                   ((PFN_vkCreateMetalSurfaceEXT)                   vk_instance_functions[ id_vkCreateMetalSurfaceEXT])
#define vkCreateXcbSurfaceKHR                     ((PFN_vkCreateXcbSurfaceKHR)                     vk_instance_functions[ id_vkCreateXcbSurfaceKHR])
#define vkCreateWin32SurfaceKHR                   ((PFN_vkCreateWin32SurfaceKHR)                   vk_instance_functions[ id_vkCreateWin32SurfaceKHR])
#define vkCreateDevice                            ((PFN_vkCreateDevice)                            vk_instance_functions[ id_vkCreateDevice])

#define vkGetDeviceQueue                          ((PFN_vkGetDeviceQueue)                          vk_device_functions[   id_vkGetDeviceQueue])
#define vkCreateSwapchainKHR                      ((PFN_vkCreateSwapchainKHR)                      vk_device_functions[   id_vkCreateSwapchainKHR])
#define vkDestroySwapchainKHR                     ((PFN_vkDestroySwapchainKHR)                     vk_device_functions[   id_vkDestroySwapchainKHR])
#define vkGetSwapchainImagesKHR                   ((PFN_vkGetSwapchainImagesKHR)                   vk_device_functions[   id_vkGetSwapchainImagesKHR])
#define vkAcquireNextImageKHR                     ((PFN_vkAcquireNextImageKHR)                     vk_device_functions[   id_vkAcquireNextImageKHR])
#define vkQueuePresentKHR                         ((PFN_vkQueuePresentKHR)                         vk_device_functions[   id_vkQueuePresentKHR])
#define vkCreateFence                             ((PFN_vkCreateFence)                             vk_device_functions[   id_vkCreateFence])
#define vkWaitForFences                           ((PFN_vkWaitForFences)                           vk_device_functions[   id_vkWaitForFences])
#define vkResetFences                             ((PFN_vkResetFences)                             vk_device_functions[   id_vkResetFences])
#define vkCreateSemaphore                         ((PFN_vkCreateSemaphore)                         vk_device_functions[   id_vkCreateSemaphore])
#define vkCreateRenderPass                        ((PFN_vkCreateRenderPass)                        vk_device_functions[   id_vkCreateRenderPass])
#define vkCreateFramebuffer                       ((PFN_vkCreateFramebuffer)                       vk_device_functions[   id_vkCreateFramebuffer])
#define vkDestroyFramebuffer                      ((PFN_vkDestroyFramebuffer)                      vk_device_functions[   id_vkDestroyFramebuffer])
#define vkAllocateMemory                          ((PFN_vkAllocateMemory)                          vk_device_functions[   id_vkAllocateMemory])
#define vkFreeMemory                              ((PFN_vkFreeMemory)                              vk_device_functions[   id_vkFreeMemory])
#define vkMapMemory                               ((PFN_vkMapMemory)                               vk_device_functions[   id_vkMapMemory])
#define vkUnmapMemory                             ((PFN_vkUnmapMemory)                             vk_device_functions[   id_vkUnmapMemory])
#define vkCreateImage                             ((PFN_vkCreateImage)                             vk_device_functions[   id_vkCreateImage])
#define vkDestroyImage                            ((PFN_vkDestroyImage)                            vk_device_functions[   id_vkDestroyImage])
#define vkGetImageMemoryRequirements              ((PFN_vkGetImageMemoryRequirements)              vk_device_functions[   id_vkGetImageMemoryRequirements])
#define vkBindImageMemory                         ((PFN_vkBindImageMemory)                         vk_device_functions[   id_vkBindImageMemory])
#define vkCreateImageView                         ((PFN_vkCreateImageView)                         vk_device_functions[   id_vkCreateImageView])
#define vkDestroyImageView                        ((PFN_vkDestroyImageView)                        vk_device_functions[   id_vkDestroyImageView])
#define vkCreateBuffer                            ((PFN_vkCreateBuffer)                            vk_device_functions[   id_vkCreateBuffer])
#define vkDestroyBuffer                           ((PFN_vkDestroyBuffer)                           vk_device_functions[   id_vkDestroyBuffer])
#define vkCreateBufferView                        ((PFN_vkCreateBufferView)                        vk_device_functions[   id_vkCreateBufferView])
#define vkDestroyBufferView                       ((PFN_vkDestroyBufferView)                       vk_device_functions[   id_vkDestroyBufferView])
#define vkGetBufferMemoryRequirements             ((PFN_vkGetBufferMemoryRequirements)             vk_device_functions[   id_vkGetBufferMemoryRequirements])
#define vkBindBufferMemory                        ((PFN_vkBindBufferMemory)                        vk_device_functions[   id_vkBindBufferMemory])
#define vkCreateShaderModule                      ((PFN_vkCreateShaderModule)                      vk_device_functions[   id_vkCreateShaderModule])
#define vkCreateDescriptorSetLayout               ((PFN_vkCreateDescriptorSetLayout)               vk_device_functions[   id_vkCreateDescriptorSetLayout])
#define vkCreatePipelineLayout                    ((PFN_vkCreatePipelineLayout)                    vk_device_functions[   id_vkCreatePipelineLayout])
#define vkCreateGraphicsPipelines                 ((PFN_vkCreateGraphicsPipelines)                 vk_device_functions[   id_vkCreateGraphicsPipelines])
#define vkDestroyPipeline                         ((PFN_vkDestroyPipeline)                         vk_device_functions[   id_vkDestroyPipeline])
#define vkCreateCommandPool                       ((PFN_vkCreateCommandPool)                       vk_device_functions[   id_vkCreateCommandPool])
#define vkAllocateCommandBuffers                  ((PFN_vkAllocateCommandBuffers)                  vk_device_functions[   id_vkAllocateCommandBuffers])
#define vkResetCommandBuffer                      ((PFN_vkResetCommandBuffer)                      vk_device_functions[   id_vkResetCommandBuffer])
#define vkBeginCommandBuffer                      ((PFN_vkBeginCommandBuffer)                      vk_device_functions[   id_vkBeginCommandBuffer])
#define vkEndCommandBuffer                        ((PFN_vkEndCommandBuffer)                        vk_device_functions[   id_vkEndCommandBuffer])
#define vkQueueSubmit                             ((PFN_vkQueueSubmit)                             vk_device_functions[   id_vkQueueSubmit])
#define vkQueueWaitIdle                           ((PFN_vkQueueWaitIdle)                           vk_device_functions[   id_vkQueueWaitIdle])
#define vkCmdPipelineBarrier                      ((PFN_vkCmdPipelineBarrier)                      vk_device_functions[   id_vkCmdPipelineBarrier])
#define vkCmdCopyBuffer                           ((PFN_vkCmdCopyBuffer)                           vk_device_functions[   id_vkCmdCopyBuffer])
