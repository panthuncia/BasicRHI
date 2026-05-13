#pragma once

#include "rhi.h"
#include "rhi_interop.h"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#if __has_include("volk.h")
#include "volk.h"
#elif __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif

#if defined(VK_VERSION_1_0)

#define BASICRHI_HAS_VULKAN_HEADERS 1

namespace rhi::vulkan {

    inline bool spirv_instruction_string_equals(const uint32_t* words, uint32_t wordCount, const char* expected) noexcept {
        const char* bytes = reinterpret_cast<const char*>(words);
        const size_t byteCount = static_cast<size_t>(wordCount) * sizeof(uint32_t);
        const size_t expectedLength = std::strlen(expected);
        return expectedLength < byteCount && std::memcmp(bytes, expected, expectedLength) == 0 && bytes[expectedLength] == '\0';
    }

    // FidelityFX FSR3's upscaling shader has typos that annoy the Vulkan validation layers
    inline bool patch_fsr3_luma_history_image_format(std::vector<uint32_t>& spirvWords) {
        constexpr uint16_t OpName = 5;
        constexpr uint16_t OpTypeImage = 25;
        constexpr uint16_t OpTypePointer = 32;
        constexpr uint16_t OpVariable = 59;
        constexpr uint32_t SpvImageFormatRgba16f = 2;
        constexpr uint32_t SpvImageFormatRgba8 = 4;

        uint32_t lumaHistoryVariableId = 0;
        uint32_t lumaHistoryPointerTypeId = 0;
        uint32_t lumaHistoryImageTypeId = 0;

        for (size_t offset = 5; offset < spirvWords.size();) {
            const uint32_t instruction = spirvWords[offset];
            const uint16_t wordCount = static_cast<uint16_t>(instruction >> 16);
            const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffffu);
            if (wordCount == 0 || offset + wordCount > spirvWords.size()) {
                return false;
            }

            if (opcode == OpName && wordCount >= 3 && spirv_instruction_string_equals(&spirvWords[offset + 2], wordCount - 2, "rw_luma_history")) {
                lumaHistoryVariableId = spirvWords[offset + 1];
                break;
            }

            offset += wordCount;
        }

        if (lumaHistoryVariableId == 0) {
            return false;
        }

        for (size_t offset = 5; offset < spirvWords.size();) {
            const uint32_t instruction = spirvWords[offset];
            const uint16_t wordCount = static_cast<uint16_t>(instruction >> 16);
            const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffffu);
            if (wordCount == 0 || offset + wordCount > spirvWords.size()) {
                return false;
            }

            if (opcode == OpVariable && wordCount >= 4 && spirvWords[offset + 2] == lumaHistoryVariableId) {
                lumaHistoryPointerTypeId = spirvWords[offset + 1];
                break;
            }

            offset += wordCount;
        }

        if (lumaHistoryPointerTypeId == 0) {
            return false;
        }

        for (size_t offset = 5; offset < spirvWords.size();) {
            const uint32_t instruction = spirvWords[offset];
            const uint16_t wordCount = static_cast<uint16_t>(instruction >> 16);
            const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffffu);
            if (wordCount == 0 || offset + wordCount > spirvWords.size()) {
                return false;
            }

            if (opcode == OpTypePointer && wordCount >= 4 && spirvWords[offset + 1] == lumaHistoryPointerTypeId) {
                lumaHistoryImageTypeId = spirvWords[offset + 3];
                break;
            }

            offset += wordCount;
        }

        if (lumaHistoryImageTypeId == 0) {
            return false;
        }

        for (size_t offset = 5; offset < spirvWords.size();) {
            const uint32_t instruction = spirvWords[offset];
            const uint16_t wordCount = static_cast<uint16_t>(instruction >> 16);
            const uint16_t opcode = static_cast<uint16_t>(instruction & 0xffffu);
            if (wordCount == 0 || offset + wordCount > spirvWords.size()) {
                return false;
            }

            if (opcode == OpTypeImage && wordCount >= 9 && spirvWords[offset + 1] == lumaHistoryImageTypeId && spirvWords[offset + 8] == SpvImageFormatRgba8) {
                spirvWords[offset + 8] = SpvImageFormatRgba16f;
                return true;
            }

            offset += wordCount;
        }

        return false;
    }

    inline VkResult VKAPI_CALL create_shader_module_with_fsr3_luma_history_format_fix(
        VkDevice device,
        const VkShaderModuleCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkShaderModule* shaderModule) {
        if (createInfo && createInfo->pCode && createInfo->codeSize >= 5u * sizeof(uint32_t) && createInfo->codeSize % sizeof(uint32_t) == 0u) {
            VkShaderModuleCreateInfo patchedCreateInfo = *createInfo;
            std::vector<uint32_t> patchedWords(createInfo->pCode, createInfo->pCode + createInfo->codeSize / sizeof(uint32_t));
            if (patch_fsr3_luma_history_image_format(patchedWords)) {
                patchedCreateInfo.pCode = patchedWords.data();
                return vkCreateShaderModule(device, &patchedCreateInfo, allocator, shaderModule);
            }
        }

        return vkCreateShaderModule(device, createInfo, allocator, shaderModule);
    }

    // Patch for drivers that don't support storage buffers in descriptor pools without an explicit storage buffer descriptor
    inline VkResult VKAPI_CALL create_descriptor_pool_with_storage_buffer_fallback(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDescriptorPool* descriptorPool) noexcept {
        if (createInfo && createInfo->pPoolSizes && createInfo->poolSizeCount > 0u) {
            bool hasStorageBuffer = false;
            uint32_t storageBufferDescriptorCount = 0;
            for (uint32_t index = 0; index < createInfo->poolSizeCount; ++index) {
                const VkDescriptorPoolSize& poolSize = createInfo->pPoolSizes[index];
                hasStorageBuffer = hasStorageBuffer || poolSize.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                if (poolSize.descriptorCount > storageBufferDescriptorCount) {
                    storageBufferDescriptorCount = poolSize.descriptorCount;
                }
            }

            if (!hasStorageBuffer && createInfo->poolSizeCount < 16u && storageBufferDescriptorCount > 0u) {
                VkDescriptorPoolSize poolSizes[16]{};
                for (uint32_t index = 0; index < createInfo->poolSizeCount; ++index) {
                    poolSizes[index] = createInfo->pPoolSizes[index];
                }
                poolSizes[createInfo->poolSizeCount] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBufferDescriptorCount };

                VkDescriptorPoolCreateInfo patchedCreateInfo = *createInfo;
                patchedCreateInfo.poolSizeCount = createInfo->poolSizeCount + 1u;
                patchedCreateInfo.pPoolSizes = poolSizes;
                return vkCreateDescriptorPool(device, &patchedCreateInfo, allocator, descriptorPool);
            }
        }

        return vkCreateDescriptorPool(device, createInfo, allocator, descriptorPool);
    }

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

    // FidelityFX internally tries to use KHR variants of certain functions, but they may not be exported by all VK1.1+ loaders
    // If that happens, FFX will crash in CreateContext, so we must wrap the device proc addr to return core versions of those functions when the KHR variants are not found
    inline PFN_vkVoidFunction VKAPI_CALL get_device_proc_addr_with_core_alias_fallback(VkDevice device, const char* name) noexcept {
        PFN_vkVoidFunction proc = vkGetDeviceProcAddr(device, name);
        if (name && std::strcmp(name, "vkCreateDescriptorPool") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(&create_descriptor_pool_with_storage_buffer_fallback);
        }
        if (name && std::strcmp(name, "vkCreateShaderModule") == 0) {
            return reinterpret_cast<PFN_vkVoidFunction>(&create_shader_module_with_fsr3_luma_history_format_fix);
        }
        if (!proc && name) {
#if defined(VK_VERSION_1_1)
            if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0) {
                proc = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
            }
            else if (std::strcmp(name, "vkGetImageMemoryRequirements2KHR") == 0) {
                proc = vkGetDeviceProcAddr(device, "vkGetImageMemoryRequirements2");
            }
#endif
        }
        return proc;
    }

    inline PFN_vkGetDeviceProcAddr get_device_proc_addr() noexcept {
        return get_device_proc_addr_with_core_alias_fallback;
    }

    inline uint32_t get_api_version(rhi::Device device) {
        VulkanDeviceInfo info{};
        if (!QueryNativeDevice(device, RHI_IID_VK_DEVICE, &info, sizeof(info))) return VK_API_VERSION_1_0;
        return info.apiVersion ? info.apiVersion : VK_API_VERSION_1_0;
    }

    inline uint32_t get_device_api_version(rhi::Device device) {
        VulkanDeviceInfo info{};
        if (!QueryNativeDevice(device, RHI_IID_VK_DEVICE, &info, sizeof(info))) return VK_API_VERSION_1_0;
        return info.deviceApiVersion ? info.deviceApiVersion : VK_API_VERSION_1_0;
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

    inline bool get_resource_info(rhi::Resource resource, VulkanResourceInfo& info) {
        info = {};
        return QueryNativeResource(resource, RHI_IID_VK_RESOURCE, &info, sizeof(info));
    }

    inline bool get_descriptor_slot_info(rhi::Device device, rhi::DescriptorSlot slot, VulkanDescriptorSlotInfo& info) {
        info = {};
        return QueryNativeDescriptorSlot(device, slot, RHI_IID_VK_DESCRIPTOR_SLOT, &info, sizeof(info));
    }

    inline VkImageView get_image_view(rhi::Device device, rhi::DescriptorSlot slot) {
        VulkanDescriptorSlotInfo info{};
        if (!get_descriptor_slot_info(device, slot, info)) return VK_NULL_HANDLE;
        return from_native_void<VkImageView>(info.imageView);
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