#pragma once

#include "rhi.h"
#include "rhi_interop.h"

#include <cstdint>
#include <type_traits>

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>

#define BASICRHI_HAS_VULKAN_HEADERS 1

namespace rhi::vulkan {
    template <typename THandle>
    inline THandle from_native_void(void* ptr) noexcept {
        if constexpr (std::is_pointer_v<THandle>) {
            return static_cast<THandle>(ptr);
        }
        else {
            return static_cast<THandle>(reinterpret_cast<uintptr_t>(ptr));
        }
    }

    inline VkInstance get_instance(rhi::Device device) {
        VulkanDeviceInfo info{};
        if (!QueryNativeDevice(device, RHI_IID_VK_DEVICE, &info, sizeof(info))) return VK_NULL_HANDLE;
        return from_native_void<VkInstance>(info.instance);
    }

    inline VkPhysicalDevice get_physical_device(rhi::Device device) {
        VulkanDeviceInfo info{};
        if (!QueryNativeDevice(device, RHI_IID_VK_DEVICE, &info, sizeof(info))) return VK_NULL_HANDLE;
		return from_native_void<VkPhysicalDevice>(info.physicalDevice);
    }

    inline VkDevice get_device(rhi::Device device) {
        VulkanDeviceInfo info{};
        if (!QueryNativeDevice(device, RHI_IID_VK_DEVICE, &info, sizeof(info))) return VK_NULL_HANDLE;
		return from_native_void<VkDevice>(info.device);
    }

    inline VkQueue get_queue(rhi::Queue queue) {
        VulkanQueueInfo info{};
        if (!QueryNativeQueue(queue, RHI_IID_VK_QUEUE, &info, sizeof(info))) return VK_NULL_HANDLE;
		return from_native_void<VkQueue>(info.queue);
    }

    inline uint32_t get_queue_family_index(rhi::Queue queue) {
        VulkanQueueInfo info{};
        if (!QueryNativeQueue(queue, RHI_IID_VK_QUEUE, &info, sizeof(info))) return 0u;
        return info.familyIndex;
    }

    inline VkCommandBuffer get_cmd_list(rhi::CommandList commandList) {
        VulkanCmdBufInfo info{};
        if (!QueryNativeCmdList(commandList, RHI_IID_VK_COMMAND_BUFFER, &info, sizeof(info))) return VK_NULL_HANDLE;
		return from_native_void<VkCommandBuffer>(info.commandBuffer);
    }

    inline VkSwapchainKHR get_swapchain(rhi::Swapchain swapchain) {
        VulkanSwapchainInfo info{};
        if (!QueryNativeSwapchain(swapchain, RHI_IID_VK_SWAPCHAIN, &info, sizeof(info))) return VK_NULL_HANDLE;
		return from_native_void<VkSwapchainKHR>(info.swapchain);
    }

    inline VkImage get_resource(rhi::Resource resource) {
        VulkanResourceInfo info{};
        if (!QueryNativeResource(resource, RHI_IID_VK_RESOURCE, &info, sizeof(info))) return VK_NULL_HANDLE;
        return from_native_void<VkImage>(info.resource);
    }

    inline uint64_t get_buffer_device_address(rhi::Resource resource) {
        VulkanResourceInfo info{};
        if (!QueryNativeResource(resource, RHI_IID_VK_RESOURCE, &info, sizeof(info))) return 0u;
        return info.deviceAddress;
    }
} // namespace rhi::vulkan

#else

#define BASICRHI_HAS_VULKAN_HEADERS 0

#endif