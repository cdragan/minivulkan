// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#pragma once

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

#define GET_VK_DEV_FUNCTION(name, dev) \
    const PFN_##name name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr((dev), #name))

#define VK_LIB_FUNCTIONS \
    X(vkGetInstanceProcAddr) \
    X(vkGetDeviceProcAddr)

#define VK_GLOBAL_FUNCTIONS_BASE \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkCreateInstance)

#ifdef NDEBUG
#   define VK_GLOBAL_FUNCTIONS VK_GLOBAL_FUNCTIONS_BASE
#else
#   define VK_GLOBAL_FUNCTIONS VK_GLOBAL_FUNCTIONS_BASE \
    X(vkEnumerateInstanceVersion)
#endif

#define VK_INSTANCE_FUNCTIONS_BASE \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures2) \
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
    X(vkAllocateMemory) \
    X(vkMapMemory) \
    X(vkFlushMappedMemoryRanges) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetImageSubresourceLayout) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateBuffer) \
    X(vkCreateBufferView) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateSampler) \
    X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) \
    X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRenderingKHR) \
    X(vkCmdEndRenderingKHR) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyImage) \
    X(vkCmdDispatch)

extern PFN_vkVoidFunction vk_lib_functions[];
extern PFN_vkVoidFunction vk_global_functions[];
extern PFN_vkVoidFunction vk_instance_functions[];
extern PFN_vkVoidFunction vk_device_functions[];

#define X(func) id_##func,
enum e_lib_functions {
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

#define SELECT_VK_FUNCTION(type, name) ((PFN_##name) vk_##type##_functions[id_##name])

#define vkGetInstanceProcAddr                     SELECT_VK_FUNCTION(lib,      vkGetInstanceProcAddr)
#define vkGetDeviceProcAddr                       SELECT_VK_FUNCTION(lib,      vkGetDeviceProcAddr)

#define vkEnumerateInstanceVersion                SELECT_VK_FUNCTION(global,   vkEnumerateInstanceVersion)
#define vkEnumerateInstanceExtensionProperties    SELECT_VK_FUNCTION(global,   vkEnumerateInstanceExtensionProperties)
#define vkCreateInstance                          SELECT_VK_FUNCTION(global,   vkCreateInstance)

#define vkEnumeratePhysicalDevices                SELECT_VK_FUNCTION(instance, vkEnumeratePhysicalDevices)
#define vkGetPhysicalDeviceProperties2            SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceProperties2)
#define vkGetPhysicalDeviceQueueFamilyProperties  SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceQueueFamilyProperties)
#define vkGetPhysicalDeviceSurfaceSupportKHR      SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceSurfaceSupportKHR)
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
#define vkGetPhysicalDeviceSurfaceFormatsKHR      SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceSurfaceFormatsKHR)
#define vkGetPhysicalDeviceFormatProperties       SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceFormatProperties)
#define vkGetPhysicalDeviceMemoryProperties       SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceMemoryProperties)
#define vkGetPhysicalDeviceFeatures2              SELECT_VK_FUNCTION(instance, vkGetPhysicalDeviceFeatures2)
#define vkEnumerateDeviceExtensionProperties      SELECT_VK_FUNCTION(instance, vkEnumerateDeviceExtensionProperties)
#define vkCreateMetalSurfaceEXT                   SELECT_VK_FUNCTION(instance, vkCreateMetalSurfaceEXT)
#define vkCreateXcbSurfaceKHR                     SELECT_VK_FUNCTION(instance, vkCreateXcbSurfaceKHR)
#define vkCreateWin32SurfaceKHR                   SELECT_VK_FUNCTION(instance, vkCreateWin32SurfaceKHR)
#define vkCreateDevice                            SELECT_VK_FUNCTION(instance, vkCreateDevice)

#define vkGetDeviceQueue                          SELECT_VK_FUNCTION(device,   vkGetDeviceQueue)
#define vkCreateSwapchainKHR                      SELECT_VK_FUNCTION(device,   vkCreateSwapchainKHR)
#define vkDestroySwapchainKHR                     SELECT_VK_FUNCTION(device,   vkDestroySwapchainKHR)
#define vkGetSwapchainImagesKHR                   SELECT_VK_FUNCTION(device,   vkGetSwapchainImagesKHR)
#define vkAcquireNextImageKHR                     SELECT_VK_FUNCTION(device,   vkAcquireNextImageKHR)
#define vkQueuePresentKHR                         SELECT_VK_FUNCTION(device,   vkQueuePresentKHR)
#define vkCreateFence                             SELECT_VK_FUNCTION(device,   vkCreateFence)
#define vkWaitForFences                           SELECT_VK_FUNCTION(device,   vkWaitForFences)
#define vkResetFences                             SELECT_VK_FUNCTION(device,   vkResetFences)
#define vkCreateSemaphore                         SELECT_VK_FUNCTION(device,   vkCreateSemaphore)
#define vkAllocateMemory                          SELECT_VK_FUNCTION(device,   vkAllocateMemory)
#define vkMapMemory                               SELECT_VK_FUNCTION(device,   vkMapMemory)
#define vkFlushMappedMemoryRanges                 SELECT_VK_FUNCTION(device,   vkFlushMappedMemoryRanges)
#define vkCreateImage                             SELECT_VK_FUNCTION(device,   vkCreateImage)
#define vkDestroyImage                            SELECT_VK_FUNCTION(device,   vkDestroyImage)
#define vkGetImageMemoryRequirements              SELECT_VK_FUNCTION(device,   vkGetImageMemoryRequirements)
#define vkGetImageSubresourceLayout               SELECT_VK_FUNCTION(device,   vkGetImageSubresourceLayout)
#define vkBindImageMemory                         SELECT_VK_FUNCTION(device,   vkBindImageMemory)
#define vkCreateImageView                         SELECT_VK_FUNCTION(device,   vkCreateImageView)
#define vkDestroyImageView                        SELECT_VK_FUNCTION(device,   vkDestroyImageView)
#define vkCreateBuffer                            SELECT_VK_FUNCTION(device,   vkCreateBuffer)
#define vkCreateBufferView                        SELECT_VK_FUNCTION(device,   vkCreateBufferView)
#define vkGetBufferMemoryRequirements             SELECT_VK_FUNCTION(device,   vkGetBufferMemoryRequirements)
#define vkBindBufferMemory                        SELECT_VK_FUNCTION(device,   vkBindBufferMemory)
#define vkCreateSampler                           SELECT_VK_FUNCTION(device,   vkCreateSampler)
#define vkCreateShaderModule                      SELECT_VK_FUNCTION(device,   vkCreateShaderModule)
#define vkCreateDescriptorSetLayout               SELECT_VK_FUNCTION(device,   vkCreateDescriptorSetLayout)
#define vkCreatePipelineLayout                    SELECT_VK_FUNCTION(device,   vkCreatePipelineLayout)
#define vkCreateGraphicsPipelines                 SELECT_VK_FUNCTION(device,   vkCreateGraphicsPipelines)
#define vkCreateComputePipelines                  SELECT_VK_FUNCTION(device,   vkCreateComputePipelines)
#define vkCreateCommandPool                       SELECT_VK_FUNCTION(device,   vkCreateCommandPool)
#define vkAllocateCommandBuffers                  SELECT_VK_FUNCTION(device,   vkAllocateCommandBuffers)
#define vkResetCommandBuffer                      SELECT_VK_FUNCTION(device,   vkResetCommandBuffer)
#define vkBeginCommandBuffer                      SELECT_VK_FUNCTION(device,   vkBeginCommandBuffer)
#define vkEndCommandBuffer                        SELECT_VK_FUNCTION(device,   vkEndCommandBuffer)
#define vkQueueSubmit                             SELECT_VK_FUNCTION(device,   vkQueueSubmit)
#define vkQueueWaitIdle                           SELECT_VK_FUNCTION(device,   vkQueueWaitIdle)
#define vkCreateDescriptorPool                    SELECT_VK_FUNCTION(device,   vkCreateDescriptorPool)
#define vkAllocateDescriptorSets                  SELECT_VK_FUNCTION(device,   vkAllocateDescriptorSets)
#define vkUpdateDescriptorSets                    SELECT_VK_FUNCTION(device,   vkUpdateDescriptorSets)
#define vkCmdBeginRenderingKHR                    SELECT_VK_FUNCTION(device,   vkCmdBeginRenderingKHR)
#define vkCmdEndRenderingKHR                      SELECT_VK_FUNCTION(device,   vkCmdEndRenderingKHR)
#define vkCmdBindPipeline                         SELECT_VK_FUNCTION(device,   vkCmdBindPipeline)
#define vkCmdBindVertexBuffers                    SELECT_VK_FUNCTION(device,   vkCmdBindVertexBuffers)
#define vkCmdBindIndexBuffer                      SELECT_VK_FUNCTION(device,   vkCmdBindIndexBuffer)
#define vkCmdBindDescriptorSets                   SELECT_VK_FUNCTION(device,   vkCmdBindDescriptorSets)
#define vkCmdSetViewport                          SELECT_VK_FUNCTION(device,   vkCmdSetViewport)
#define vkCmdSetScissor                           SELECT_VK_FUNCTION(device,   vkCmdSetScissor)
#define vkCmdDraw                                 SELECT_VK_FUNCTION(device,   vkCmdDraw)
#define vkCmdDrawIndexed                          SELECT_VK_FUNCTION(device,   vkCmdDrawIndexed)
#define vkCmdPipelineBarrier                      SELECT_VK_FUNCTION(device,   vkCmdPipelineBarrier)
#define vkCmdCopyBuffer                           SELECT_VK_FUNCTION(device,   vkCmdCopyBuffer)
#define vkCmdCopyImage                            SELECT_VK_FUNCTION(device,   vkCmdCopyImage)
#define vkCmdDispatch                             SELECT_VK_FUNCTION(device,   vkCmdDispatch)
