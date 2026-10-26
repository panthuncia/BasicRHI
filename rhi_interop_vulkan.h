#pragma once

#include "rhi.h"
#include "rhi_interop.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#if __has_include("volk.h")
#include "volk.h"
#ifdef VOLK_NAMESPACE
using namespace volk;
#endif
#elif __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif

#if defined(VK_VERSION_1_0)

#define BASICRHI_HAS_VULKAN_HEADERS 1

namespace rhi::vulkan {

    struct Win32ExternalInteropCapabilities {
        bool memory = false;
        bool timeline = false;
        bool d3d12ResourceBuffers = false;
		bool d3d12ResourceImages = false;
		bool d3d12Heaps = false;
    };

	struct ExternalImageSupport {
		bool supported = false;
		bool importable = false;
		bool dedicatedOnly = false;
		uint64_t allocationSize = 0;
		uint64_t alignment = 0;
		uint32_t memoryTypeBits = 0;
	};

    Win32ExternalInteropCapabilities query_win32_external_interop(rhi::Device device) noexcept;
    Result import_d3d12_buffer(
        rhi::Device device,
        const ExternalHandle& sharedHandle,
        const rhi::ResourceDesc& desc,
        rhi::ResourcePtr& out) noexcept;
	ExternalImageSupport query_d3d12_texture_support(
		rhi::Device device,
		const rhi::ResourceDesc& desc) noexcept;
	Result import_d3d12_texture(
		rhi::Device device,
		const ExternalHandle& sharedHandle,
		const rhi::ResourceDesc& desc,
		rhi::ResourcePtr& out) noexcept;
	Result import_d3d12_heap(
		rhi::Device device,
		const ExternalHandle& sharedHandle,
		const rhi::HeapDesc& desc,
		rhi::HeapPtr& out) noexcept;
    Result import_d3d12_timeline(
        rhi::Device device,
        const ExternalHandle& sharedHandle,
        uint64_t initialValue,
        const char* debugName,
        rhi::TimelinePtr& out) noexcept;

    // ---------------------------------------------------------------------------
    // Adopting a host-created device (e.g. DXVK's). BasicRHI never destroys the
    // instance/device/queues it adopts, never calls vkDeviceWaitIdle/vkQueueWaitIdle
    // on them, and brackets every vkQueueSubmit with the host's submission lock.

    // Called around every host access to an adopted VkQueue. DXVK: IDXGIVkInteropDevice
    // LockSubmissionQueue / ReleaseSubmissionQueue. Both or neither must be set.
    struct QueueSubmissionHooks {
        void* user = nullptr;
        void (*lock)(void* user, VkQueue queue) = nullptr;
        void (*unlock)(void* user, VkQueue queue) = nullptr;
        // Optional. When set, queue submissions are handed to the host instead of calling
        // vkQueueSubmit, so it can order them within its own stream (e.g. DXVK's command
        // stream). The host must submit batches to `queue` in call order; everything the
        // submit info points at is only valid during the call. lock/unlock are then not
        // used for submissions.
        VkResult (*submit)(void* user, VkQueue queue, const VkSubmitInfo2& submitInfo) = nullptr;
    };

    struct AdoptedQueue {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t familyIndex = 0;
        uint32_t queueIndex = 0;
    };

    // The binding models BasicRHI's Vulkan backend can run on. The model is fixed for a device: an adopted device uses the one
    // whose features the host enabled (descriptorHeap; or descriptorBuffer + mutableDescriptorType), and a host that records
    // BasicRHI work into its own command buffers must use the same model itself.
    enum BindingModelBits : uint32_t {
        BindingModel_DescriptorHeap = 1u << 0,    // VK_EXT_descriptor_heap
        BindingModel_DescriptorBuffer = 1u << 1,  // VK_EXT_descriptor_buffer + VK_EXT_mutable_descriptor_type, shaders lowered
    };

    // What the client will create on the device, for the descriptor-buffer model's limit checks.
    struct BindingRequirements {
        // The shader-visible heap capacities the client wants (preferred) and can live with (min): a model is supported with
        // min(preferred, the device's limit) when that reaches min, and that capacity is reported per model.
        DescriptorHeapCapacities heapCapacities{};
        // What the host itself takes from the device's shared budgets (DXVK: its 2048-sampler pool and its descriptor
        // buffers), so that the two together fit.
        uint32_t hostSamplerObjects = 0;
        VkDeviceSize hostResourceDescriptorBufferBytes = 0;
        VkDeviceSize hostSamplerDescriptorBufferBytes = 0;
    };

    struct BindingModelReport {
        uint32_t supportedModels = 0;  // BindingModelBits
        // The capacities each supported model would set the device up with (0 for an unsupported model).
        uint32_t heapModelResources = 0, heapModelSamplers = 0;
        uint32_t bufferModelResources = 0, bufferModelSamplers = 0;
        // One line per check, required against the device's value, for the log.
        std::vector<std::string> checks;
    };

    struct AdoptedVulkanDeviceInfo {
        // Optional, backend-neutral marker registration. The caller owns marker storage
        // until all submitted command buffers retire; BasicRHI only records opaque IDs.
        struct Diagnostics {
            void* user = nullptr;
            const void* (*registerMarker)(void* user, const char* name) noexcept = nullptr;
            bool detailedCommands = false;
        } diagnostics{};
        // The loader entry point the host used (it may be an interposer's).
        PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        uint32_t instanceApiVersion = 0;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        const char* const* enabledDeviceExtensions = nullptr;
        uint32_t enabledDeviceExtensionCount = 0;
        // The instance's enabled extensions. Only VK_EXT_debug_utils is consulted: object names and
        // debugger labels are issued only when the host enabled it.
        const char* const* enabledInstanceExtensions = nullptr;
        uint32_t enabledInstanceExtensionCount = 0;
        // The pNext chain given to vkCreateDevice (VkPhysicalDeviceFeatures2 and/or
        // Vulkan 1.1-1.3 / extension feature structs). Every capability BasicRHI
        // reports is derived from it, never assumed.
        const void* enabledFeatureChain = nullptr;
        // VkDeviceCreateInfo::pEnabledFeatures, if the host used it instead of Features2.
        const VkPhysicalDeviceFeatures* enabledCoreFeatures = nullptr;
        // Indexed by rhi::QueueKind (Graphics, Compute, Copy). queues[0] is required;
        // unset kinds alias it.
        AdoptedQueue queues[3]{};
        // Queues the host created and never uses itself: Device::CreateQueue hands them out (one whose
        // family supports the kind, each once; a destroyed one is handed out again). Their families join
        // every Concurrent resource's sharing list. Submissions to them go through submissionHooks too.
        const AdoptedQueue* spareQueues = nullptr;
        uint32_t spareQueueCount = 0;
        QueueSubmissionHooks submissionHooks{};
        bool validateBarrierTransitions = false;
        // What the host passed EvaluateBindingModels for this device (heap capacities, its own sampler objects and descriptor
        // buffer bytes): adoption sets the device up the same way the evaluation said it would.
        BindingRequirements bindingRequirements{};
    };

    Result AdoptVulkanDevice(const AdoptedVulkanDeviceInfo& info, rhi::DevicePtr& out) noexcept;

    // Which binding models a physical device supports for BasicRHI, from instance-level queries only, so that a host can ask
    // before it creates its device. BASICRHI_VK_LIMITS=base clamps every limit to the Vulkan base-spec minimum (to exercise the
    // fallbacks).
    BindingModelReport EvaluateBindingModels(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance,
        VkPhysicalDevice physicalDevice, const BindingRequirements& requirements) noexcept;

    // The binding model a Vulkan device runs on (one BindingModelBits value), 0 for a device of another backend.
    uint32_t GetBindingModel(rhi::Device device) noexcept;

    // Non-owning import of a host image. currentLayout is the layout the image is in
    // when BasicRHI first uses it; the host keeps ownership of image and memory.
    struct ImportedImageDesc {
        VkImage image = VK_NULL_HANDLE;
        VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The host keeps the image in VK_IMAGE_LAYOUT_GENERAL and may use it between BasicRHI's commands
        // (D3D12's simultaneous-access contract): BasicRHI never moves it out of GENERAL, and its views
        // and attachments are used in GENERAL. Requires currentLayout GENERAL; pair with barriers whose
        // layouts are Common (ORG: ExternalTextureResource with commonLayoutOnly).
        bool simultaneousAccess = false;
        const char* debugName = nullptr;
    };
    Result import_image(rhi::Device device, const ImportedImageDesc& desc, rhi::ResourcePtr& out) noexcept;

    // Non-owning import of a whole host buffer. Bindless descriptors need
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT in usage.
    struct ImportedBufferDesc {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkBufferUsageFlags usage = 0;
        bool concurrentSharing = false;
        const char* debugName = nullptr;
    };
    Result import_buffer(rhi::Device device, const ImportedBufferDesc& desc, rhi::ResourcePtr& out) noexcept;

    // Non-owning wrap of a VkCommandBuffer recorded by the host (an adopted device's), already begun: every recording call
    // works on it, starting with nothing bound (no heaps, layout, pipeline or pass), and records on one thread at a time. End
    // and Recycle are no-ops (End closes only a pass this wrapper began), the RHI never submits it, and destroying the
    // wrapper leaves the VkCommandBuffer alone. Barriers recorded through it are not reflected in BasicRHI's resource layout
    // tracking. GPU scratch its recording allocates - generated-commands preprocess memory, emulated root constants, transient
    // query pools - lives with the wrapper: keep it until the host's submission of the buffer completes if any was used.
    CommandListPtr AdoptCommandBuffer(Device& device, VkCommandBuffer commandBuffer, QueueKind kind) noexcept;

    // The host destroyed an adopted VkDevice before BasicRHI was shut down: release
    // BasicRHI's bookkeeping without touching any Vulkan handle.
    void abandon_device(rhi::Device device) noexcept;

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

    uint64_t get_adapter_luid(rhi::Device device) noexcept;

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
