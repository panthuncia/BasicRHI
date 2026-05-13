#include "rhi_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <cstddef>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <spdlog/spdlog.h>

namespace rhi {
	namespace {
		static constexpr uint32_t kVkInvalidQueueFamily = 0xFFFFFFFFu;

		template <typename... TArgs>
		constexpr void VkIgnoreUnused(TArgs&&...) noexcept {}

		static void VkMarkCommandListError(VulkanCommandList* commandListState, Result result) noexcept {
			if (commandListState && commandListState->pendingError == Result::Ok) {
				commandListState->pendingError = result;
			}
			BreakIfDebugging();
		}

		static Result ToRHI(VkResult result) noexcept {
			switch (result) {
			case VK_SUCCESS:
				return Result::Ok;
			case VK_TIMEOUT:
				return Result::WaitTimeout;
			case VK_NOT_READY:
				return Result::StillDrawing;
			case VK_ERROR_OUT_OF_HOST_MEMORY:
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				return Result::OutOfMemory;
			case VK_ERROR_INITIALIZATION_FAILED:
				return Result::Failed;
			case VK_ERROR_DEVICE_LOST:
				return Result::DeviceLost;
			case VK_ERROR_LAYER_NOT_PRESENT:
			case VK_ERROR_EXTENSION_NOT_PRESENT:
			case VK_ERROR_FEATURE_NOT_PRESENT:
			case VK_ERROR_FORMAT_NOT_SUPPORTED:
				return Result::Unsupported;
			case VK_SUBOPTIMAL_KHR:
			case VK_ERROR_OUT_OF_DATE_KHR:
				return Result::ModeChanged;
			case VK_ERROR_SURFACE_LOST_KHR:
				return Result::PresentationLost;
			case VK_ERROR_INCOMPATIBLE_DRIVER:
				return Result::DriverVersionMismatch;
			case VK_ERROR_UNKNOWN:
			default:
				return Result::Failed;
			}
		}

		static VkFormat ToVkFormat(Format format) noexcept {
			switch (format) {
			case Format::R32G32B32A32_UInt:
				return VK_FORMAT_R32G32B32A32_UINT;
			case Format::R32G32B32A32_SInt:
				return VK_FORMAT_R32G32B32A32_SINT;
			case Format::R32G32B32A32_Float:
				return VK_FORMAT_R32G32B32A32_SFLOAT;
			case Format::R32G32B32_UInt:
				return VK_FORMAT_R32G32B32_UINT;
			case Format::R32G32B32_SInt:
				return VK_FORMAT_R32G32B32_SINT;
			case Format::R32G32B32_Float:
				return VK_FORMAT_R32G32B32_SFLOAT;
			case Format::R32G32_UInt:
				return VK_FORMAT_R32G32_UINT;
			case Format::R32G32_SInt:
				return VK_FORMAT_R32G32_SINT;
			case Format::R32G32_Float:
				return VK_FORMAT_R32G32_SFLOAT;
			case Format::R16G16B16A16_UNorm:
				return VK_FORMAT_R16G16B16A16_UNORM;
			case Format::R16G16B16A16_SNorm:
				return VK_FORMAT_R16G16B16A16_SNORM;
			case Format::R16G16B16A16_UInt:
				return VK_FORMAT_R16G16B16A16_UINT;
			case Format::R16G16B16A16_SInt:
				return VK_FORMAT_R16G16B16A16_SINT;
			case Format::R16G16B16A16_Float:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::R16G16_UNorm:
				return VK_FORMAT_R16G16_UNORM;
			case Format::R16G16_SNorm:
				return VK_FORMAT_R16G16_SNORM;
			case Format::R16G16_UInt:
				return VK_FORMAT_R16G16_UINT;
			case Format::R16G16_SInt:
				return VK_FORMAT_R16G16_SINT;
			case Format::R16G16_Float:
				return VK_FORMAT_R16G16_SFLOAT;
			case Format::R8G8B8A8_UNorm:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8G8B8A8_UNorm_sRGB:
				return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::R8G8B8A8_UInt:
				return VK_FORMAT_R8G8B8A8_UINT;
			case Format::R8G8B8A8_SNorm:
				return VK_FORMAT_R8G8B8A8_SNORM;
			case Format::R8G8B8A8_SInt:
				return VK_FORMAT_R8G8B8A8_SINT;
			case Format::B8G8R8A8_UNorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8_UNorm_sRGB:
				return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R32_Typeless:
				return VK_FORMAT_D32_SFLOAT;
			case Format::R32_Float:
				return VK_FORMAT_R32_SFLOAT;
			case Format::R32_UInt:
				return VK_FORMAT_R32_UINT;
			case Format::R32_SInt:
				return VK_FORMAT_R32_SINT;
			case Format::R16_Float:
				return VK_FORMAT_R16_SFLOAT;
			case Format::R16_UNorm:
				return VK_FORMAT_R16_UNORM;
			case Format::R16_UInt:
				return VK_FORMAT_R16_UINT;
			case Format::R16_SNorm:
				return VK_FORMAT_R16_SNORM;
			case Format::R16_SInt:
				return VK_FORMAT_R16_SINT;
			case Format::R8G8_UNorm:
				return VK_FORMAT_R8G8_UNORM;
			case Format::R8G8_UInt:
				return VK_FORMAT_R8G8_UINT;
			case Format::R8G8_SNorm:
				return VK_FORMAT_R8G8_SNORM;
			case Format::R8G8_SInt:
				return VK_FORMAT_R8G8_SINT;
			case Format::R8_UNorm:
				return VK_FORMAT_R8_UNORM;
			case Format::R8_UInt:
				return VK_FORMAT_R8_UINT;
			case Format::R8_SNorm:
				return VK_FORMAT_R8_SNORM;
			case Format::R8_SInt:
				return VK_FORMAT_R8_SINT;
			case Format::BC1_UNorm:
				return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case Format::BC1_UNorm_sRGB:
				return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
			case Format::BC2_UNorm:
				return VK_FORMAT_BC2_UNORM_BLOCK;
			case Format::BC2_UNorm_sRGB:
				return VK_FORMAT_BC2_SRGB_BLOCK;
			case Format::BC3_UNorm:
				return VK_FORMAT_BC3_UNORM_BLOCK;
			case Format::BC3_UNorm_sRGB:
				return VK_FORMAT_BC3_SRGB_BLOCK;
			case Format::BC4_UNorm:
				return VK_FORMAT_BC4_UNORM_BLOCK;
			case Format::BC4_SNorm:
				return VK_FORMAT_BC4_SNORM_BLOCK;
			case Format::BC5_UNorm:
				return VK_FORMAT_BC5_UNORM_BLOCK;
			case Format::BC5_SNorm:
				return VK_FORMAT_BC5_SNORM_BLOCK;
			case Format::BC6H_UF16:
				return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			case Format::BC6H_SF16:
				return VK_FORMAT_BC6H_SFLOAT_BLOCK;
			case Format::BC7_UNorm:
				return VK_FORMAT_BC7_UNORM_BLOCK;
			case Format::BC7_UNorm_sRGB:
				return VK_FORMAT_BC7_SRGB_BLOCK;
			case Format::D32_Float:
				return VK_FORMAT_D32_SFLOAT;
			default:
				return VK_FORMAT_UNDEFINED;
			}
		}

		static Format FromVkFormat(VkFormat format) noexcept {
			switch (format) {
			case VK_FORMAT_R32G32B32A32_UINT:
				return Format::R32G32B32A32_UInt;
			case VK_FORMAT_R32G32B32A32_SINT:
				return Format::R32G32B32A32_SInt;
			case VK_FORMAT_R32G32B32A32_SFLOAT:
				return Format::R32G32B32A32_Float;
			case VK_FORMAT_R32G32B32_UINT:
				return Format::R32G32B32_UInt;
			case VK_FORMAT_R32G32B32_SINT:
				return Format::R32G32B32_SInt;
			case VK_FORMAT_R32G32B32_SFLOAT:
				return Format::R32G32B32_Float;
			case VK_FORMAT_R32G32_UINT:
				return Format::R32G32_UInt;
			case VK_FORMAT_R32G32_SINT:
				return Format::R32G32_SInt;
			case VK_FORMAT_R32G32_SFLOAT:
				return Format::R32G32_Float;
			case VK_FORMAT_R16G16B16A16_UNORM:
				return Format::R16G16B16A16_UNorm;
			case VK_FORMAT_R16G16B16A16_SNORM:
				return Format::R16G16B16A16_SNorm;
			case VK_FORMAT_R16G16B16A16_UINT:
				return Format::R16G16B16A16_UInt;
			case VK_FORMAT_R16G16B16A16_SINT:
				return Format::R16G16B16A16_SInt;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return Format::R16G16B16A16_Float;
			case VK_FORMAT_R16G16_UNORM:
				return Format::R16G16_UNorm;
			case VK_FORMAT_R16G16_SNORM:
				return Format::R16G16_SNorm;
			case VK_FORMAT_R16G16_UINT:
				return Format::R16G16_UInt;
			case VK_FORMAT_R16G16_SINT:
				return Format::R16G16_SInt;
			case VK_FORMAT_R16G16_SFLOAT:
				return Format::R16G16_Float;
			case VK_FORMAT_R8G8B8A8_UNORM:
				return Format::R8G8B8A8_UNorm;
			case VK_FORMAT_R8G8B8A8_SRGB:
				return Format::R8G8B8A8_UNorm_sRGB;
			case VK_FORMAT_R8G8B8A8_UINT:
				return Format::R8G8B8A8_UInt;
			case VK_FORMAT_R8G8B8A8_SNORM:
				return Format::R8G8B8A8_SNorm;
			case VK_FORMAT_R8G8B8A8_SINT:
				return Format::R8G8B8A8_SInt;
			case VK_FORMAT_B8G8R8A8_UNORM:
				return Format::B8G8R8A8_UNorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return Format::B8G8R8A8_UNorm_sRGB;
			case VK_FORMAT_R32_SFLOAT:
				return Format::R32_Float;
			case VK_FORMAT_R32_UINT:
				return Format::R32_UInt;
			case VK_FORMAT_R32_SINT:
				return Format::R32_SInt;
			case VK_FORMAT_R16_SFLOAT:
				return Format::R16_Float;
			case VK_FORMAT_R16_UNORM:
				return Format::R16_UNorm;
			case VK_FORMAT_R16_UINT:
				return Format::R16_UInt;
			case VK_FORMAT_R16_SNORM:
				return Format::R16_SNorm;
			case VK_FORMAT_R16_SINT:
				return Format::R16_SInt;
			case VK_FORMAT_R8G8_UNORM:
				return Format::R8G8_UNorm;
			case VK_FORMAT_R8G8_UINT:
				return Format::R8G8_UInt;
			case VK_FORMAT_R8G8_SNORM:
				return Format::R8G8_SNorm;
			case VK_FORMAT_R8G8_SINT:
				return Format::R8G8_SInt;
			case VK_FORMAT_R8_UNORM:
				return Format::R8_UNorm;
			case VK_FORMAT_R8_UINT:
				return Format::R8_UInt;
			case VK_FORMAT_R8_SNORM:
				return Format::R8_SNorm;
			case VK_FORMAT_R8_SINT:
				return Format::R8_SInt;
			case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
				return Format::BC1_UNorm;
			case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
				return Format::BC1_UNorm_sRGB;
			case VK_FORMAT_BC2_UNORM_BLOCK:
				return Format::BC2_UNorm;
			case VK_FORMAT_BC2_SRGB_BLOCK:
				return Format::BC2_UNorm_sRGB;
			case VK_FORMAT_BC3_UNORM_BLOCK:
				return Format::BC3_UNorm;
			case VK_FORMAT_BC3_SRGB_BLOCK:
				return Format::BC3_UNorm_sRGB;
			case VK_FORMAT_BC4_UNORM_BLOCK:
				return Format::BC4_UNorm;
			case VK_FORMAT_BC4_SNORM_BLOCK:
				return Format::BC4_SNorm;
			case VK_FORMAT_BC5_UNORM_BLOCK:
				return Format::BC5_UNorm;
			case VK_FORMAT_BC5_SNORM_BLOCK:
				return Format::BC5_SNorm;
			case VK_FORMAT_BC6H_UFLOAT_BLOCK:
				return Format::BC6H_UF16;
			case VK_FORMAT_BC6H_SFLOAT_BLOCK:
				return Format::BC6H_SF16;
			case VK_FORMAT_BC7_UNORM_BLOCK:
				return Format::BC7_UNorm;
			case VK_FORMAT_BC7_SRGB_BLOCK:
				return Format::BC7_UNorm_sRGB;
			case VK_FORMAT_D32_SFLOAT:
				return Format::D32_Float;
			default:
				return Format::Unknown;
			}
		}

		static bool VkHasNamedProperty(std::string_view target, const char* name) noexcept {
			return name && target == name;
		}

		static bool VkHasInstanceLayer(std::string_view target) {
			uint32_t count = 0;
			if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
				return false;
			}

			std::vector<VkLayerProperties> layers(count);
			if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
				return false;
			}

			for (uint32_t index = 0; index < count; ++index) {
				if (VkHasNamedProperty(target, layers[index].layerName)) {
					return true;
				}
			}

			return false;
		}

		static bool VkHasInstanceExtension(std::string_view target) {
			uint32_t count = 0;
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
				return false;
			}

			std::vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) {
				return false;
			}

			for (uint32_t index = 0; index < count; ++index) {
				if (VkHasNamedProperty(target, extensions[index].extensionName)) {
					return true;
				}
			}

			return false;
		}

		static bool VkHasDeviceExtension(VkPhysicalDevice physicalDevice, std::string_view target) {
			uint32_t count = 0;
			if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
				return false;
			}

			std::vector<VkExtensionProperties> extensions(count);
			if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS) {
				return false;
			}

			for (uint32_t index = 0; index < count; ++index) {
				if (VkHasNamedProperty(target, extensions[index].extensionName)) {
					return true;
				}
			}

			return false;
		}

		static int VkDeviceTypeRank(VkPhysicalDeviceType type) noexcept {
			switch (type) {
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
				return 0;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
				return 1;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
				return 2;
			case VK_PHYSICAL_DEVICE_TYPE_CPU:
				return 3;
			case VK_PHYSICAL_DEVICE_TYPE_OTHER:
			default:
				return 4;
			}
		}

		static uint32_t VkFindQueueFamily(const std::vector<VkQueueFamilyProperties>& families, VkQueueFlags required, VkQueueFlags preferredAbsent) noexcept {
			for (uint32_t index = 0; index < static_cast<uint32_t>(families.size()); ++index) {
				const VkQueueFamilyProperties& family = families[index];
				if ((family.queueFlags & required) != required || family.queueCount == 0) {
					continue;
				}

				if ((family.queueFlags & preferredAbsent) == 0) {
					return index;
				}
			}

			for (uint32_t index = 0; index < static_cast<uint32_t>(families.size()); ++index) {
				const VkQueueFamilyProperties& family = families[index];
				if ((family.queueFlags & required) == required && family.queueCount > 0) {
					return index;
				}
			}

			return kVkInvalidQueueFamily;
		}

		static bool VkSelectPhysicalDeviceAndQueues(
			VkInstance instance,
			VkPhysicalDevice& selectedDevice,
			VkPhysicalDeviceProperties& selectedProperties,
			VkPhysicalDeviceMemoryProperties& selectedMemoryProperties,
			VkPhysicalDeviceFeatures& selectedFeatures,
			std::vector<VkQueueFamilyProperties>& selectedFamilies,
			std::array<uint32_t, 3>& selectedQueueFamilies) noexcept {
			selectedDevice = VK_NULL_HANDLE;
			selectedProperties = {};
			selectedMemoryProperties = {};
			selectedFeatures = {};
			selectedFamilies.clear();
			selectedQueueFamilies.fill(kVkInvalidQueueFamily);

			uint32_t physicalDeviceCount = 0;
			if (vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0) {
				return false;
			}

			std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
			if (vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data()) != VK_SUCCESS) {
				return false;
			}

			int bestRank = (std::numeric_limits<int>::max)();
			for (uint32_t deviceIndex = 0; deviceIndex < physicalDeviceCount; ++deviceIndex) {
				VkPhysicalDevice candidate = physicalDevices[deviceIndex];
				VkPhysicalDeviceProperties properties{};
				vkGetPhysicalDeviceProperties(candidate, &properties);

				uint32_t familyCount = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
				if (familyCount == 0) {
					continue;
				}

				std::vector<VkQueueFamilyProperties> families(familyCount);
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

				const uint32_t graphicsFamily = VkFindQueueFamily(families, VK_QUEUE_GRAPHICS_BIT, 0);
				if (graphicsFamily == kVkInvalidQueueFamily) {
					continue;
				}

				uint32_t computeFamily = VkFindQueueFamily(families, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
				if (computeFamily == kVkInvalidQueueFamily) {
					computeFamily = graphicsFamily;
				}

				uint32_t copyFamily = VkFindQueueFamily(families, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
				if (copyFamily == kVkInvalidQueueFamily) {
					copyFamily = VkFindQueueFamily(families, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
				}
				if (copyFamily == kVkInvalidQueueFamily) {
					copyFamily = computeFamily;
				}
				if (copyFamily == kVkInvalidQueueFamily) {
					copyFamily = graphicsFamily;
				}

				const int rank = VkDeviceTypeRank(properties.deviceType);
				if (rank >= bestRank) {
					continue;
				}

				bestRank = rank;
				selectedDevice = candidate;
				selectedProperties = properties;
				vkGetPhysicalDeviceMemoryProperties(candidate, &selectedMemoryProperties);
				vkGetPhysicalDeviceFeatures(candidate, &selectedFeatures);
				selectedFamilies = std::move(families);
				selectedQueueFamilies = { graphicsFamily, computeFamily, copyFamily };
			}

			return selectedDevice != VK_NULL_HANDLE;
		}

		static QueueHandle VkPrimaryQueueHandleForKind(const VulkanDevice* impl, QueueKind kind) noexcept {
			switch (kind) {
			case QueueKind::Graphics:
				return impl->gfxHandle;
			case QueueKind::Compute:
				return impl->compHandle;
			case QueueKind::Copy:
				return impl->copyHandle;
			default:
				return {};
			}
		}

		static uint32_t VkPrimaryQueueSlotForKind(QueueKind kind) noexcept {
			switch (kind) {
			case QueueKind::Graphics:
				return 0;
			case QueueKind::Compute:
				return 1;
			case QueueKind::Copy:
				return 2;
			default:
				return 0;
			}
		}

		static VulkanQueueState* VkQueueStateForHandle(VulkanDevice* impl, QueueHandle handle) noexcept {
			if (!impl || handle.generation != 1u || handle.index >= impl->queues.size()) {
				return nullptr;
			}

			return &impl->queues[handle.index];
		}

		static VulkanCommandAllocator* VkAllocatorState(VulkanDevice* impl, CommandAllocatorHandle handle) noexcept {
			return impl ? impl->allocators.get(handle) : nullptr;
		}

		static VulkanPipeline* VkPipelineState(VulkanDevice* impl, PipelineHandle handle) noexcept {
			return impl ? impl->pipelines.get(handle) : nullptr;
		}

		static VulkanPipelineLayout* VkPipelineLayoutState(VulkanDevice* impl, PipelineLayoutHandle handle) noexcept {
			return impl ? impl->pipelineLayouts.get(handle) : nullptr;
		}

		static VulkanCommandSignature* VkCommandSignatureState(VulkanDevice* impl, CommandSignatureHandle handle) noexcept {
			return impl ? impl->commandSignatures.get(handle) : nullptr;
		}

		static VulkanTimeline* VkTimelineState(VulkanDevice* impl, TimelineHandle handle) noexcept {
			return impl ? impl->timelines.get(handle) : nullptr;
		}

		static VulkanHeap* VkHeapState(VulkanDevice* impl, HeapHandle handle) noexcept {
			return impl ? impl->heaps.get(handle) : nullptr;
		}

		static VulkanQueryPool* VkQueryPoolState(VulkanDevice* impl, QueryPoolHandle handle) noexcept {
			return impl ? impl->queryPools.get(handle) : nullptr;
		}

		static VulkanResource* VkResourceState(VulkanDevice* impl, ResourceHandle handle) noexcept {
			return impl ? impl->resources.get(handle) : nullptr;
		}

		static VulkanDescriptorHeap* VkDescriptorHeapState(VulkanDevice* impl, DescriptorHeapHandle handle) noexcept {
			return impl ? impl->descriptorHeaps.get(handle) : nullptr;
		}

		static VulkanSwapchain* VkSwapchainState(VulkanDevice* impl, SwapChainHandle handle) noexcept {
			return impl ? impl->swapchains.get(handle) : nullptr;
		}

		static VulkanSwapchain* VkSwapchainState(Swapchain* swapchain) noexcept {
			auto* impl = swapchain ? static_cast<VulkanDevice*>(swapchain->impl) : nullptr;
			return VkSwapchainState(impl, swapchain ? swapchain->GetHandle() : SwapChainHandle{});
		}

		static VulkanImageViewSlot* VkImageViewSlotState(VulkanDevice* impl, DescriptorSlot slot, DescriptorHeapType expectedType) noexcept {
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			if (!heap || heap->type != expectedType || slot.index >= heap->imageViewSlots.size()) {
				return nullptr;
			}
			return &heap->imageViewSlots[slot.index];
		}

		static void VkResetDescriptorSlot(VulkanDevice* impl, VulkanImageViewSlot& slot) noexcept {
			if (impl && impl->device != VK_NULL_HANDLE) {
				if (slot.view != VK_NULL_HANDLE) {
					vkDestroyImageView(impl->device, slot.view, nullptr);
				}
				if (slot.bufferView != VK_NULL_HANDLE) {
					vkDestroyBufferView(impl->device, slot.bufferView, nullptr);
				}
				if (slot.sampler != VK_NULL_HANDLE) {
					vkDestroySampler(impl->device, slot.sampler, nullptr);
				}
			}
			slot = {};
		}

		static uint64_t VkAlignUp(uint64_t value, uint64_t alignment) noexcept {
			if (alignment == 0) {
				return value;
			}
			const uint64_t mask = alignment - 1u;
			return (value + mask) & ~mask;
		}

		static bool VkFindMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memoryProperties,
			uint32_t memoryTypeBits,
			VkMemoryPropertyFlags preferredFlags,
			VkMemoryPropertyFlags requiredFlags,
			uint32_t& outMemoryTypeIndex) noexcept;

		static uint32_t VkPushConstantStorageBytes(const PushConstantRangeDesc& desc) noexcept {
			if (desc.type == PushConstantRangeType::EmulatedRootConstants) {
				return static_cast<uint32_t>(sizeof(VkDeviceAddress));
			}
			return desc.num32BitValues * 4u;
		}

		static uint64_t VkEmulatedRootConstantAlignment(const VulkanDevice* impl) noexcept {
			if (!impl) {
				return 16u;
			}

			return (std::max)(16ull, static_cast<uint64_t>(impl->physicalDeviceProperties.limits.minUniformBufferOffsetAlignment));
		}

		static void VkDestroyEmulatedRootConstantScratchPage(VulkanDevice* impl, VulkanCommandList::EmulatedRootConstantScratchPage& page) noexcept {
			if (!impl) {
				page = {};
				return;
			}

			if (page.mappedData != nullptr && page.memory != VK_NULL_HANDLE) {
				vkUnmapMemory(impl->device, page.memory);
			}
			if (page.buffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(impl->device, page.buffer, nullptr);
			}
			if (page.memory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, page.memory, nullptr);
			}
			page = {};
		}

		static bool VkEnsureEmulatedRootConstantScratchPage(VulkanDevice* impl, VulkanCommandList& commandListState, uint32_t minSize) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || !impl->bufferDeviceAddressEnabled || minSize == 0) {
				return false;
			}

			const uint64_t alignment = VkEmulatedRootConstantAlignment(impl);
			const uint64_t capacity64 = (std::max<uint64_t>)(VkAlignUp(minSize, alignment), 64ull * 1024ull);
			if (capacity64 > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
				return false;
			}

			VkBufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			createInfo.size = capacity64;
			createInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
			createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkBuffer buffer = VK_NULL_HANDLE;
			if (vkCreateBuffer(impl->device, &createInfo, nullptr, &buffer) != VK_SUCCESS) {
				return false;
			}

			VkMemoryRequirements memoryRequirements{};
			vkGetBufferMemoryRequirements(impl->device, buffer, &memoryRequirements);

			uint32_t memoryTypeIndex = 0;
			if (!VkFindMemoryTypeIndex(
				impl->memoryProperties,
				memoryRequirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				memoryTypeIndex)) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				return false;
			}

			VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			allocateInfo.allocationSize = memoryRequirements.size;
			allocateInfo.memoryTypeIndex = memoryTypeIndex;
			VkMemoryAllocateFlagsInfo allocateFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
			allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
			allocateInfo.pNext = &allocateFlagsInfo;

			VkDeviceMemory memory = VK_NULL_HANDLE;
			if (vkAllocateMemory(impl->device, &allocateInfo, nullptr, &memory) != VK_SUCCESS) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				return false;
			}

			if (vkBindBufferMemory(impl->device, buffer, memory, 0) != VK_SUCCESS) {
				vkFreeMemory(impl->device, memory, nullptr);
				vkDestroyBuffer(impl->device, buffer, nullptr);
				return false;
			}

			void* mappedData = nullptr;
			if (vkMapMemory(impl->device, memory, 0, capacity64, 0, &mappedData) != VK_SUCCESS || mappedData == nullptr) {
				vkFreeMemory(impl->device, memory, nullptr);
				vkDestroyBuffer(impl->device, buffer, nullptr);
				return false;
			}

			VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
			addressInfo.buffer = buffer;
			const VkDeviceAddress deviceAddress = vkGetBufferDeviceAddress(impl->device, &addressInfo);
			if (deviceAddress == 0) {
				vkUnmapMemory(impl->device, memory);
				vkFreeMemory(impl->device, memory, nullptr);
				vkDestroyBuffer(impl->device, buffer, nullptr);
				return false;
			}

			VulkanCommandList::EmulatedRootConstantScratchPage page{};
			page.buffer = buffer;
			page.memory = memory;
			page.mappedData = mappedData;
			page.deviceAddress = deviceAddress;
			page.capacity = static_cast<uint32_t>(capacity64);
			page.cursor = 0;
			commandListState.emulatedRootConstantScratchPages.push_back(std::move(page));
			return true;
		}

		static bool VkAllocateEmulatedRootConstantScratch(
			VulkanDevice* impl,
			VulkanCommandList& commandListState,
			uint32_t size,
			VkDeviceAddress& outDeviceAddress,
			void*& outMappedData) noexcept {
			if (!impl || size == 0) {
				return false;
			}

			const uint64_t alignment = VkEmulatedRootConstantAlignment(impl);
			for (auto& page : commandListState.emulatedRootConstantScratchPages) {
				const uint64_t alignedCursor = VkAlignUp(page.cursor, alignment);
				if (alignedCursor + size > page.capacity) {
					continue;
				}

				outDeviceAddress = page.deviceAddress + alignedCursor;
				outMappedData = static_cast<std::byte*>(page.mappedData) + alignedCursor;
				page.cursor = static_cast<uint32_t>(alignedCursor + size);
				return true;
			}

			if (!VkEnsureEmulatedRootConstantScratchPage(impl, commandListState, size)) {
				return false;
			}

			auto& page = commandListState.emulatedRootConstantScratchPages.back();
			outDeviceAddress = page.deviceAddress;
			outMappedData = page.mappedData;
			page.cursor = size;
			return true;
		}

		static VulkanCommandList::EmulatedRootConstantShadowState* VkGetEmulatedRootConstantShadowState(
			VulkanCommandList& commandListState,
			const VulkanPushConstantRange& range) noexcept {
			for (auto& state : commandListState.emulatedRootConstantShadowStates) {
				if (state.set == range.desc.set && state.binding == range.desc.binding) {
					if (state.values.size() != range.desc.num32BitValues) {
						state.values.assign(range.desc.num32BitValues, 0u);
					}
					return &state;
				}
			}

			VulkanCommandList::EmulatedRootConstantShadowState state{};
			state.set = range.desc.set;
			state.binding = range.desc.binding;
			state.values.assign(range.desc.num32BitValues, 0u);
			commandListState.emulatedRootConstantShadowStates.push_back(std::move(state));
			return &commandListState.emulatedRootConstantShadowStates.back();
		}

		static uint64_t VkDescriptorHeapStride(const VulkanDevice* impl, DescriptorHeapType type) noexcept {
			if (!impl || !impl->descriptorHeapEnabled) {
				return 1;
			}

			switch (type) {
			case DescriptorHeapType::CbvSrvUav:
				return (std::max)(static_cast<uint64_t>(impl->descriptorHeapProperties.imageDescriptorSize),
					static_cast<uint64_t>(impl->descriptorHeapProperties.bufferDescriptorSize));
			case DescriptorHeapType::Sampler:
				return static_cast<uint64_t>(impl->descriptorHeapProperties.samplerDescriptorSize);
			case DescriptorHeapType::RTV:
			case DescriptorHeapType::DSV:
			default:
				return 1;
			}
		}

		static void* VkDescriptorHeapSlotAddress(VulkanDescriptorHeap* heap, uint32_t index) noexcept {
			if (!heap || !heap->mappedData || heap->descriptorStride == 0) {
				return nullptr;
			}

			auto* base = static_cast<std::byte*>(heap->mappedData);
			return base + static_cast<size_t>(index * heap->descriptorStride);
		}

		static bool VkIsSpirvBytecode(const ShaderBinary& bytecode) noexcept {
			static constexpr uint32_t kSpirvMagic = 0x07230203u;
			if (!bytecode.data || bytecode.size < sizeof(uint32_t) || (bytecode.size % sizeof(uint32_t)) != 0) {
				return false;
			}

			uint32_t magic = 0;
			std::memcpy(&magic, bytecode.data, sizeof(magic));
			return magic == kSpirvMagic;
		}

		static void VkSetObjectName(VulkanDevice* impl, uint64_t objectHandle, VkObjectType objectType, const char* name) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || objectHandle == 0 || !name || !*name || !vkSetDebugUtilsObjectNameEXT) {
				return;
			}

			VkDebugUtilsObjectNameInfoEXT nameInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
			nameInfo.objectType = objectType;
			nameInfo.objectHandle = objectHandle;
			nameInfo.pObjectName = name;
			(void)vkSetDebugUtilsObjectNameEXT(impl->device, &nameInfo);
		}

		static VkImageAspectFlags VkAspectMaskForFormat(VkFormat format) noexcept;

		static VkPipelineStageFlagBits VkPipelineStageForStage(Stage stage) noexcept {
			switch (stage) {
			case Stage::Top: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			case Stage::Draw: return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
			case Stage::Pixel: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			case Stage::Compute: return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			case Stage::Copy: return VK_PIPELINE_STAGE_TRANSFER_BIT;
			case Stage::Bottom: return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			default: return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			}
		}

		static VkPipelineStageFlags VkStageMaskForSync(ResourceSyncState sync) noexcept {
			const uint32_t bits = static_cast<uint32_t>(sync);
			if (bits == 0) {
				return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			}
			if ((bits & static_cast<uint32_t>(ResourceSyncState::All)) != 0) {
				return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			}

			VkPipelineStageFlags stages = 0;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::Draw)) != 0) stages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::IndexInput)) != 0) stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::VertexShading)) != 0) stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::PixelShading)) != 0) stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::DepthStencil)) != 0) stages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::RenderTarget)) != 0) stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::ComputeShading)) != 0) stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::Raytracing)) != 0) {
#ifdef VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
				stages |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
#else
				stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
#endif
			}
			if ((bits & static_cast<uint32_t>(ResourceSyncState::Copy)) != 0) stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::Resolve)) != 0) stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::ExecuteIndirect)) != 0) stages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::AllShading)) != 0) stages |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::NonPixelShading)) != 0) stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::EmitRaytracingAccelerationStructurePostbuildInfo)) != 0) stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::ClearUnorderedAccessView)) != 0) stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::VideoDecode)) != 0) stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::VideoProcess)) != 0) stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::VideoEncode)) != 0) stages |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			if ((bits & static_cast<uint32_t>(ResourceSyncState::BuildRaytracingAccelerationStructure)) != 0) {
#ifdef VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
				stages |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
#else
				stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
#endif
			}
			if ((bits & static_cast<uint32_t>(ResourceSyncState::CopyRatracingAccelerationStructure)) != 0) {
#ifdef VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
				stages |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
#else
				stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
			}
			return stages != 0 ? stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		}

		static VkAccessFlags VkAccessMaskForAccess(ResourceAccessType access) noexcept {
			const uint32_t bits = static_cast<uint32_t>(access);
			if (bits == 0 || access == ResourceAccessType::Common) {
				return 0;
			}

			VkAccessFlags flags = 0;
			if ((bits & ResourceAccessType::VertexBuffer) != 0) flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
			if ((bits & ResourceAccessType::ConstantBuffer) != 0) flags |= VK_ACCESS_UNIFORM_READ_BIT;
			if ((bits & ResourceAccessType::IndexBuffer) != 0) flags |= VK_ACCESS_INDEX_READ_BIT;
			if ((bits & ResourceAccessType::RenderTarget) != 0) flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			if ((bits & ResourceAccessType::RenderTargetClear) != 0) flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
			if ((bits & ResourceAccessType::UnorderedAccess) != 0) flags |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			if ((bits & ResourceAccessType::UnorderedAccessClear) != 0) flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
			if ((bits & ResourceAccessType::DepthReadWrite) != 0) flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			if ((bits & ResourceAccessType::DepthStencilClear) != 0) flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
			if ((bits & ResourceAccessType::DepthRead) != 0) flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			if ((bits & ResourceAccessType::ShaderResource) != 0) flags |= VK_ACCESS_SHADER_READ_BIT;
			if ((bits & ResourceAccessType::IndirectArgument) != 0) flags |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
			if ((bits & ResourceAccessType::CopyDest) != 0) flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
			if ((bits & ResourceAccessType::CopySource) != 0) flags |= VK_ACCESS_TRANSFER_READ_BIT;
			if ((bits & ResourceAccessType::RaytracingAccelerationStructureRead) != 0) flags |= VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
			if ((bits & ResourceAccessType::RaytracingAccelerationStructureWrite) != 0) flags |= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
			return flags;
		}

		static VkAttachmentStoreOp VkStoreOpForAttachment(StoreOp storeOp, bool readOnly = false) noexcept {
			if (readOnly) {
#if defined(VK_VERSION_1_3)
				return VK_ATTACHMENT_STORE_OP_NONE;
#elif defined(VK_KHR_dynamic_rendering)
				return VK_ATTACHMENT_STORE_OP_NONE_KHR;
#else
				return VK_ATTACHMENT_STORE_OP_DONT_CARE;
#endif
			}
			return storeOp == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
		}

		static VkIndexType VkIndexTypeForFormat(Format format) noexcept {
			switch (format) {
			case Format::R16_UInt: return VK_INDEX_TYPE_UINT16;
			case Format::R32_UInt: return VK_INDEX_TYPE_UINT32;
			default: return VK_INDEX_TYPE_UINT32;
			}
		}

		static VkPrimitiveTopology VkPrimitiveTopologyForRHI(PrimitiveTopology topology) noexcept {
			switch (topology) {
			case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			case PrimitiveTopology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			case PrimitiveTopology::Unknown:
			default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			}
		}

		static VkShaderStageFlagBits VkShaderStageForRHI(ShaderStage stage) noexcept {
			switch (stage) {
			case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStage::Pixel: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
			case ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
			case ShaderStage::Task: return VK_SHADER_STAGE_TASK_BIT_EXT;
			default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
			}
		}

		static VkShaderStageFlags VkShaderStageMaskForRHI(ShaderStage stage) noexcept {
			const uint32_t bits = static_cast<uint32_t>(stage);
			if (stage == ShaderStage::All) {
				return VK_SHADER_STAGE_ALL;
			}
			VkShaderStageFlags flags = 0;
			if ((bits & static_cast<uint32_t>(ShaderStage::Vertex)) != 0) flags |= VK_SHADER_STAGE_VERTEX_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Pixel)) != 0) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Compute)) != 0) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Mesh)) != 0) flags |= VK_SHADER_STAGE_MESH_BIT_EXT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Task)) != 0) flags |= VK_SHADER_STAGE_TASK_BIT_EXT;
			return flags;
		}

		static VkFilter VkToFilter(Filter filter) noexcept;
		static VkSamplerMipmapMode VkToMipFilter(MipFilter filter) noexcept;
		static VkSamplerAddressMode VkToAddressMode(AddressMode addressMode) noexcept;
		static VkCompareOp VkToCompareOp(CompareOp compareOp) noexcept;
		static VkBorderColor VkToBorderColor(const SamplerDesc& desc) noexcept;

		static VkSamplerCreateInfo VkBuildSamplerCreateInfo(const SamplerDesc& desc) noexcept {
			VkSamplerCreateInfo createInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			createInfo.magFilter = VkToFilter(desc.magFilter);
			createInfo.minFilter = VkToFilter(desc.minFilter);
			createInfo.mipmapMode = VkToMipFilter(desc.mipFilter);
			createInfo.addressModeU = VkToAddressMode(desc.addressU);
			createInfo.addressModeV = VkToAddressMode(desc.addressV);
			createInfo.addressModeW = VkToAddressMode(desc.addressW);
			createInfo.mipLodBias = desc.mipLodBias;
			createInfo.anisotropyEnable = desc.maxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
			createInfo.maxAnisotropy = static_cast<float>((std::min)(desc.maxAnisotropy, 16u));
			createInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
			createInfo.compareOp = VkToCompareOp(desc.compareOp);
			createInfo.minLod = desc.minLod;
			createInfo.maxLod = desc.maxLod;
			createInfo.borderColor = VkToBorderColor(desc);
			createInfo.unnormalizedCoordinates = desc.unnormalizedCoordinates ? VK_TRUE : VK_FALSE;
			return createInfo;
		}

		static void VkAppendDescriptorHeapMappings(
			const VulkanDevice* impl,
			const VulkanPipelineLayout& layout,
			std::vector<VkDescriptorSetAndBindingMappingEXT>& mappings,
			std::vector<VkSamplerCreateInfo>& embeddedSamplers) noexcept {
			const uint32_t resourceStride = static_cast<uint32_t>(VkDescriptorHeapStride(impl, DescriptorHeapType::CbvSrvUav));
			const uint32_t samplerStride = static_cast<uint32_t>(VkDescriptorHeapStride(impl, DescriptorHeapType::Sampler));
			constexpr uint32_t kDescriptorHeapSet = VULKAN_DESCRIPTOR_HEAP_SET;
			constexpr uint32_t kResourceDescriptorHeapBinding = VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING;
			constexpr uint32_t kSamplerDescriptorHeapBinding = VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING;
			constexpr uint32_t kCounterDescriptorHeapBinding = VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING;
			constexpr VkFlags kResourceDescriptorHeapMask =
				VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
			constexpr VkFlags kSamplerDescriptorHeapMask =
				VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT |
				VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;

			mappings.reserve(layout.ranges.size() + layout.pushConstantRanges.size() + layout.staticSamplers.size() + 3);
			embeddedSamplers.reserve(layout.staticSamplers.size());

			auto hasMapping = [&](uint32_t set, uint32_t binding, VkFlags resourceMask) noexcept {
				for (const VkDescriptorSetAndBindingMappingEXT& mapping : mappings) {
					const uint32_t first = mapping.firstBinding;
					const uint32_t last = first + (std::max)(1u, mapping.bindingCount);
					if (mapping.descriptorSet == set && binding >= first && binding < last && (mapping.resourceMask & resourceMask) != 0) {
						return true;
					}
				}
				return false;
			};

			auto appendHeapMapping = [&](uint32_t binding, VkFlags resourceMask) noexcept {
				if (hasMapping(kDescriptorHeapSet, binding, resourceMask)) {
					return;
				}

				VkDescriptorSetAndBindingMappingEXT mapping{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT };
				mapping.descriptorSet = kDescriptorHeapSet;
				mapping.firstBinding = binding;
				mapping.bindingCount = 1;
				mapping.resourceMask = resourceMask;
				mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
				mapping.sourceData.constantOffset.heapOffset = 0;
				mapping.sourceData.constantOffset.heapArrayStride = resourceStride;
				mapping.sourceData.constantOffset.samplerHeapOffset = 0;
				mapping.sourceData.constantOffset.samplerHeapArrayStride = samplerStride;
				mappings.push_back(mapping);
			};

			appendHeapMapping(kResourceDescriptorHeapBinding, kResourceDescriptorHeapMask);
			appendHeapMapping(kSamplerDescriptorHeapBinding, kSamplerDescriptorHeapMask);
			appendHeapMapping(kCounterDescriptorHeapBinding, kResourceDescriptorHeapMask);

			for (const LayoutBindingRange& range : layout.ranges) {
				VkDescriptorSetAndBindingMappingEXT mapping{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT };
				mapping.descriptorSet = range.set;
				mapping.firstBinding = range.binding;
				mapping.bindingCount = (std::max)(1u, range.count);
				mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
				mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
				mapping.sourceData.constantOffset.heapOffset = range.binding * resourceStride;
				mapping.sourceData.constantOffset.heapArrayStride = resourceStride;
				mapping.sourceData.constantOffset.samplerHeapOffset = range.binding * samplerStride;
				mapping.sourceData.constantOffset.samplerHeapArrayStride = samplerStride;
				mappings.push_back(mapping);
			}

			for (const VulkanPushConstantRange& range : layout.pushConstantRanges) {
				VkDescriptorSetAndBindingMappingEXT mapping{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT };
				mapping.descriptorSet = range.desc.set;
				mapping.firstBinding = range.desc.binding;
				mapping.bindingCount = 1;
				mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
				if (range.desc.type == PushConstantRangeType::EmulatedRootConstants) {
					mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
					mapping.sourceData.pushAddressOffset = range.byteOffset;
				}
				else {
					mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT;
					mapping.sourceData.pushDataOffset = range.byteOffset;
				}
				mappings.push_back(mapping);
			}

			for (const StaticSamplerDesc& sampler : layout.staticSamplers) {
				embeddedSamplers.push_back(VkBuildSamplerCreateInfo(sampler.sampler));
				VkDescriptorSetAndBindingMappingEXT mapping{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT };
				mapping.descriptorSet = sampler.set;
				mapping.firstBinding = sampler.binding;
				mapping.bindingCount = (std::max)(1u, sampler.arrayCount);
				mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
				mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
				mapping.sourceData.constantOffset.heapArrayStride = samplerStride;
				mapping.sourceData.constantOffset.pEmbeddedSampler = &embeddedSamplers.back();
				mappings.push_back(mapping);
			}
		}

		static VkCullModeFlags VkCullModeForRHI(CullMode mode) noexcept {
			switch (mode) {
			case CullMode::None: return VK_CULL_MODE_NONE;
			case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
			case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
			default: return VK_CULL_MODE_BACK_BIT;
			}
		}

		static VkPolygonMode VkPolygonModeForRHI(FillMode mode) noexcept {
			switch (mode) {
			case FillMode::Wireframe: return VK_POLYGON_MODE_LINE;
			case FillMode::Solid:
			default: return VK_POLYGON_MODE_FILL;
			}
		}

		static VkBlendFactor VkBlendFactorForRHI(BlendFactor factor) noexcept {
			switch (factor) {
			case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
			case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
			case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
			case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
			case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
			case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
			case BlendFactor::InvDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			default: return VK_BLEND_FACTOR_ONE;
			}
		}

		static VkBlendOp VkBlendOpForRHI(BlendOp op) noexcept {
			switch (op) {
			case BlendOp::Add: return VK_BLEND_OP_ADD;
			case BlendOp::Sub: return VK_BLEND_OP_SUBTRACT;
			case BlendOp::RevSub: return VK_BLEND_OP_REVERSE_SUBTRACT;
			case BlendOp::Min: return VK_BLEND_OP_MIN;
			case BlendOp::Max: return VK_BLEND_OP_MAX;
			default: return VK_BLEND_OP_ADD;
			}
		}

		static VkColorComponentFlags VkColorMaskForRHI(ColorWriteEnable mask) noexcept {
			const uint32_t bits = static_cast<uint32_t>(mask);
			VkColorComponentFlags flags = 0;
			if ((bits & ColorWriteEnable::R) != 0) flags |= VK_COLOR_COMPONENT_R_BIT;
			if ((bits & ColorWriteEnable::G) != 0) flags |= VK_COLOR_COMPONENT_G_BIT;
			if ((bits & ColorWriteEnable::B) != 0) flags |= VK_COLOR_COMPONENT_B_BIT;
			if ((bits & ColorWriteEnable::A) != 0) flags |= VK_COLOR_COMPONENT_A_BIT;
			return flags;
		}

		static uint32_t VkMipDim(uint32_t base, uint32_t mip) noexcept {
			return (std::max)(1u, base >> mip);
		}

		static uint32_t VkTextureLayerCount(const VulkanResource& texture) noexcept {
			return texture.type == ResourceType::Texture3D ? 1u : texture.depthOrLayers;
		}

		static uint32_t VkSubresourceIndex(const VulkanResource& texture, uint32_t mip, uint32_t arraySlice) noexcept {
			return mip + arraySlice * texture.mipLevels;
		}

		static bool VkBuildBufferImageCopy(const VulkanResource& texture, const BufferTextureCopyFootprint& footprint, VkBufferImageCopy& out) noexcept {
			if (footprint.mip >= texture.mipLevels || footprint.arraySlice >= VkTextureLayerCount(texture)) {
				return false;
			}

			out = {};
			out.bufferOffset = footprint.footprint.offset;
			out.bufferRowLength = footprint.footprint.rowPitch && FormatByteSize(FromVkFormat(texture.format))
				? footprint.footprint.rowPitch / FormatByteSize(FromVkFormat(texture.format))
				: 0;
			out.bufferImageHeight = 0;
			out.imageSubresource.aspectMask = VkAspectMaskForFormat(texture.format);
			out.imageSubresource.mipLevel = footprint.mip;
			out.imageSubresource.baseArrayLayer = texture.type == ResourceType::Texture3D ? 0u : footprint.arraySlice;
			out.imageSubresource.layerCount = 1;
			out.imageOffset = { static_cast<int32_t>(footprint.x), static_cast<int32_t>(footprint.y), static_cast<int32_t>(footprint.z) };
			out.imageExtent.width = footprint.footprint.width ? footprint.footprint.width : VkMipDim(texture.width, footprint.mip);
			out.imageExtent.height = footprint.footprint.height ? footprint.footprint.height : VkMipDim(texture.height, footprint.mip);
			out.imageExtent.depth = footprint.footprint.depth ? footprint.footprint.depth : (texture.type == ResourceType::Texture3D ? VkMipDim(texture.depthOrLayers, footprint.mip) : 1u);
			return true;
		}

		static VkQueryPipelineStatisticFlags VkPipelineStatsToVk(PipelineStatsMask mask) noexcept {
			VkQueryPipelineStatisticFlags flags = 0;
			if ((mask & PS_IAVertices) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT;
			if ((mask & PS_IAPrimitives) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
			if ((mask & PS_VSInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT;
			if ((mask & PS_GSInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT;
			if ((mask & PS_GSPrimitives) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT;
			if ((mask & PS_CInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT;
			if ((mask & PS_CPrimitives) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT;
			if ((mask & PS_PSInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
			if ((mask & PS_CSInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
			if ((mask & PS_TaskInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT;
			if ((mask & PS_MeshInvocations) != 0) flags |= VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
			return flags;
		}

		static PipelineStatsMask VkSupportedPipelineStatsMask(const VulkanDevice* impl) noexcept {
			PipelineStatsMask mask = PS_IAVertices | PS_IAPrimitives | PS_VSInvocations | PS_GSInvocations | PS_GSPrimitives |
				PS_CInvocations | PS_CPrimitives | PS_PSInvocations | PS_CSInvocations;
			if (impl && impl->meshShaderPipelineStatsEnabled) {
				mask |= PS_TaskInvocations | PS_MeshInvocations;
			}
			return mask;
		}

		static uint32_t VkPipelineStatsFieldCount(VkQueryPipelineStatisticFlags flags) noexcept {
			uint32_t count = 0;
			for (uint32_t bit = 0; bit < 32; ++bit) {
				count += (flags & (1u << bit)) != 0 ? 1u : 0u;
			}
			return count;
		}

		static VkQueryType VkQueryTypeForRHI(QueryType type) noexcept {
			switch (type) {
			case QueryType::Timestamp: return VK_QUERY_TYPE_TIMESTAMP;
			case QueryType::Occlusion: return VK_QUERY_TYPE_OCCLUSION;
			case QueryType::PipelineStatistics: return VK_QUERY_TYPE_PIPELINE_STATISTICS;
			default: return VK_QUERY_TYPE_MAX_ENUM;
			}
		}

		static void VkDestroyPipeline(VulkanDevice* impl, VulkanPipeline& pipeline) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE) {
				pipeline = {};
				return;
			}

			if (pipeline.pipeline != VK_NULL_HANDLE) {
				vkDestroyPipeline(impl->device, pipeline.pipeline, nullptr);
			}
			if (pipeline.layout != VK_NULL_HANDLE) {
				vkDestroyPipelineLayout(impl->device, pipeline.layout, nullptr);
			}

			pipeline = {};
		}

		static void VkDestroyDescriptorHeapBacking(VulkanDevice* impl, VulkanDescriptorHeap& heap) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE) {
				heap.buffer = VK_NULL_HANDLE;
				heap.memory = VK_NULL_HANDLE;
				heap.mappedData = nullptr;
				heap.deviceAddress = 0;
				return;
			}

			if (heap.mappedData && heap.memory != VK_NULL_HANDLE) {
				vkUnmapMemory(impl->device, heap.memory);
			}
			if (heap.buffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(impl->device, heap.buffer, nullptr);
			}
			if (heap.memory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, heap.memory, nullptr);
			}

			heap.buffer = VK_NULL_HANDLE;
			heap.memory = VK_NULL_HANDLE;
			heap.mappedData = nullptr;
			heap.deviceAddress = 0;
			heap.descriptorStride = 0;
			heap.descriptorBytes = 0;
			heap.reservedRangeOffset = 0;
			heap.reservedRangeSize = 0;
		}

		static VulkanCommandList* VkCommandListState(VulkanDevice* impl, CommandListHandle handle) noexcept {
			return impl ? impl->commandLists.get(handle) : nullptr;
		}

		static VulkanCommandList* VkCommandListState(CommandList* commandList) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			return VkCommandListState(impl, commandList ? commandList->GetHandle() : CommandListHandle{});
		}

		static Result VkAllocatePrimaryCommandBuffer(VkDevice device, VkCommandPool pool, VkCommandBuffer& outCommandBuffer) noexcept {
			VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocateInfo.commandPool = pool;
			allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocateInfo.commandBufferCount = 1;

			const VkResult result = vkAllocateCommandBuffers(device, &allocateInfo, &outCommandBuffer);
			if (result != VK_SUCCESS) {
				RHI_FAIL(ToRHI(result));
			}
			return Result::Ok;
		}

		static Result VkBeginCommandRecording(VkCommandBuffer commandBuffer) noexcept {
			VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			const VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
			if (result != VK_SUCCESS) {
				RHI_FAIL(ToRHI(result));
			}
			return Result::Ok;
		}

		static VkImageAspectFlags VkAspectMaskForFormat(VkFormat format) noexcept {
			switch (format) {
			case VK_FORMAT_D32_SFLOAT:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}

		static VkImageAspectFlags VkSampledImageAspectMaskForFormat(VkFormat format) noexcept {
			switch (format) {
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			default:
				return VK_IMAGE_ASPECT_COLOR_BIT;
			}
		}

		static bool VkIsDepthStencilFormat(VkFormat format) noexcept {
			const VkImageAspectFlags aspectMask = VkAspectMaskForFormat(format);
			return (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
		}

		static VkImageViewType VkImageViewTypeForRtv(RtvDim dimension) noexcept {
			switch (dimension) {
			case RtvDim::Texture1D:
				return VK_IMAGE_VIEW_TYPE_1D;
			case RtvDim::Texture1DArray:
				return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case RtvDim::Texture2D:
			case RtvDim::Texture2DMS:
				return VK_IMAGE_VIEW_TYPE_2D;
			case RtvDim::Texture2DArray:
			case RtvDim::Texture2DMSArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case RtvDim::Texture3D:
				return VK_IMAGE_VIEW_TYPE_3D;
			default:
				return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
			}
		}

		static VkImageViewType VkImageViewTypeForDsv(DsvDim dimension) noexcept {
			switch (dimension) {
			case DsvDim::Texture1D:
				return VK_IMAGE_VIEW_TYPE_1D;
			case DsvDim::Texture1DArray:
				return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case DsvDim::Texture2D:
			case DsvDim::Texture2DMS:
				return VK_IMAGE_VIEW_TYPE_2D;
			case DsvDim::Texture2DArray:
			case DsvDim::Texture2DMSArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			default:
				return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
			}
		}

		static VkImageViewType VkImageViewTypeForSrv(SrvDim dimension) noexcept {
			switch (dimension) {
			case SrvDim::Texture1D:
				return VK_IMAGE_VIEW_TYPE_1D;
			case SrvDim::Texture1DArray:
				return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case SrvDim::Texture2D:
			case SrvDim::Texture2DMS:
				return VK_IMAGE_VIEW_TYPE_2D;
			case SrvDim::Texture2DArray:
			case SrvDim::Texture2DMSArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case SrvDim::Texture3D:
				return VK_IMAGE_VIEW_TYPE_3D;
			case SrvDim::TextureCube:
				return VK_IMAGE_VIEW_TYPE_CUBE;
			case SrvDim::TextureCubeArray:
				return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
			default:
				return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
			}
		}

		static VkImageViewType VkImageViewTypeForUav(UavDim dimension) noexcept {
			switch (dimension) {
			case UavDim::Texture1D:
				return VK_IMAGE_VIEW_TYPE_1D;
			case UavDim::Texture1DArray:
				return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case UavDim::Texture2D:
			case UavDim::Texture2DMS:
				return VK_IMAGE_VIEW_TYPE_2D;
			case UavDim::Texture2DArray:
			case UavDim::Texture2DMSArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case UavDim::Texture3D:
				return VK_IMAGE_VIEW_TYPE_3D;
			default:
				return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
			}
		}

		static VkImageSubresourceRange VkMakeImageSubresourceRange(VulkanResource& resource, const TextureSubresourceRange& range, VkImageAspectFlags aspectMask) noexcept;

		static VkImageSubresourceRange VkSrvSubresourceRange(VulkanResource& resource, const SrvDesc& desc, VkImageAspectFlags aspectMask) noexcept {
			TextureSubresourceRange range{};
			switch (desc.dimension) {
			case SrvDim::Texture1D:
				range.baseMip = desc.tex1D.mostDetailedMip;
				range.mipCount = desc.tex1D.mipLevels != 0 ? desc.tex1D.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.tex1D.mostDetailedMip);
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			case SrvDim::Texture1DArray:
				range.baseMip = desc.tex1DArray.mostDetailedMip;
				range.mipCount = desc.tex1DArray.mipLevels != 0 ? desc.tex1DArray.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.tex1DArray.mostDetailedMip);
				range.baseLayer = desc.tex1DArray.firstArraySlice;
				range.layerCount = desc.tex1DArray.arraySize != 0 ? desc.tex1DArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.tex1DArray.firstArraySlice);
				break;
			case SrvDim::Texture2D:
			case SrvDim::Texture2DMS:
				range.baseMip = desc.tex2D.mostDetailedMip;
				range.mipCount = desc.dimension == SrvDim::Texture2DMS ? 1u : (desc.tex2D.mipLevels != 0 ? desc.tex2D.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.tex2D.mostDetailedMip));
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			case SrvDim::Texture2DArray:
			case SrvDim::Texture2DMSArray:
				range.baseMip = desc.tex2DArray.mostDetailedMip;
				range.mipCount = desc.dimension == SrvDim::Texture2DMSArray ? 1u : (desc.tex2DArray.mipLevels != 0 ? desc.tex2DArray.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.tex2DArray.mostDetailedMip));
				range.baseLayer = desc.dimension == SrvDim::Texture2DMSArray ? desc.tex2DMSArray.firstArraySlice : desc.tex2DArray.firstArraySlice;
				range.layerCount = desc.dimension == SrvDim::Texture2DMSArray
					? (desc.tex2DMSArray.arraySize != 0 ? desc.tex2DMSArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.tex2DMSArray.firstArraySlice))
					: (desc.tex2DArray.arraySize != 0 ? desc.tex2DArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.tex2DArray.firstArraySlice));
				break;
			case SrvDim::Texture3D:
				range.baseMip = desc.tex3D.mostDetailedMip;
				range.mipCount = desc.tex3D.mipLevels != 0 ? desc.tex3D.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.tex3D.mostDetailedMip);
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			case SrvDim::TextureCube:
				range.baseMip = desc.cube.mostDetailedMip;
				range.mipCount = desc.cube.mipLevels != 0 ? desc.cube.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.cube.mostDetailedMip);
				range.baseLayer = 0;
				range.layerCount = 6;
				break;
			case SrvDim::TextureCubeArray:
				range.baseMip = desc.cubeArray.mostDetailedMip;
				range.mipCount = desc.cubeArray.mipLevels != 0 ? desc.cubeArray.mipLevels : static_cast<uint32_t>(resource.mipLevels - desc.cubeArray.mostDetailedMip);
				range.baseLayer = desc.cubeArray.first2DArrayFace;
				range.layerCount = desc.cubeArray.numCubes != 0 ? desc.cubeArray.numCubes * 6u : static_cast<uint32_t>(resource.depthOrLayers - desc.cubeArray.first2DArrayFace);
				break;
			default:
				break;
			}
			return VkMakeImageSubresourceRange(resource, range, aspectMask);
		}

		static VkImageSubresourceRange VkUavSubresourceRange(VulkanResource& resource, const UavDesc& desc, VkImageAspectFlags aspectMask) noexcept {
			TextureSubresourceRange range{};
			switch (desc.dimension) {
			case UavDim::Texture1D:
				range.baseMip = desc.texture1D.mipSlice;
				range.mipCount = 1;
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			case UavDim::Texture1DArray:
				range.baseMip = desc.texture1DArray.mipSlice;
				range.mipCount = 1;
				range.baseLayer = desc.texture1DArray.firstArraySlice;
				range.layerCount = desc.texture1DArray.arraySize != 0 ? desc.texture1DArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.texture1DArray.firstArraySlice);
				break;
			case UavDim::Texture2D:
			case UavDim::Texture2DMS:
				range.baseMip = desc.texture2D.mipSlice;
				range.mipCount = 1;
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			case UavDim::Texture2DArray:
			case UavDim::Texture2DMSArray:
				range.baseMip = desc.texture2DArray.mipSlice;
				range.mipCount = 1;
				range.baseLayer = desc.dimension == UavDim::Texture2DMSArray ? desc.texture2DMSArray.firstArraySlice : desc.texture2DArray.firstArraySlice;
				range.layerCount = desc.dimension == UavDim::Texture2DMSArray
					? (desc.texture2DMSArray.arraySize != 0 ? desc.texture2DMSArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.texture2DMSArray.firstArraySlice))
					: (desc.texture2DArray.arraySize != 0 ? desc.texture2DArray.arraySize : static_cast<uint32_t>(resource.depthOrLayers - desc.texture2DArray.firstArraySlice));
				break;
			case UavDim::Texture3D:
				range.baseMip = desc.texture3D.mipSlice;
				range.mipCount = 1;
				range.baseLayer = 0;
				range.layerCount = 1;
				break;
			default:
				break;
			}
			return VkMakeImageSubresourceRange(resource, range, aspectMask);
		}

		static VkFilter VkToFilter(Filter filter) noexcept {
			switch (filter) {
			case Filter::Nearest:
				return VK_FILTER_NEAREST;
			case Filter::Linear:
			default:
				return VK_FILTER_LINEAR;
			}
		}

		static VkSamplerMipmapMode VkToMipFilter(MipFilter filter) noexcept {
			switch (filter) {
			case MipFilter::Nearest:
				return VK_SAMPLER_MIPMAP_MODE_NEAREST;
			case MipFilter::Linear:
			default:
				return VK_SAMPLER_MIPMAP_MODE_LINEAR;
			}
		}

		static VkSamplerAddressMode VkToAddressMode(AddressMode addressMode) noexcept {
			switch (addressMode) {
			case AddressMode::Wrap:
				return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case AddressMode::Mirror:
				return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case AddressMode::Clamp:
				return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case AddressMode::Border:
				return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			case AddressMode::MirrorOnce:
				return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
			default:
				return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			}
		}

		static VkCompareOp VkToCompareOp(CompareOp compareOp) noexcept {
			switch (compareOp) {
			case CompareOp::Never: return VK_COMPARE_OP_NEVER;
			case CompareOp::Less: return VK_COMPARE_OP_LESS;
			case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always:
			default:
				return VK_COMPARE_OP_ALWAYS;
			}
		}

		static VkBorderColor VkToBorderColor(const SamplerDesc& desc) noexcept {
			switch (desc.borderPreset) {
			case BorderPreset::TransparentBlack:
				return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			case BorderPreset::OpaqueBlack:
				return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			case BorderPreset::OpaqueWhite:
				return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			case BorderPreset::Custom:
			default:
				if (desc.borderColor[3] == 0.0f) {
					return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				}
				if (desc.borderColor[0] == 1.0f && desc.borderColor[1] == 1.0f && desc.borderColor[2] == 1.0f && desc.borderColor[3] == 1.0f) {
					return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				}
				return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			}
		}

		static Result VkCreateBufferDescriptorSlot(VulkanDevice* impl,
			DescriptorSlot slot,
			ResourceHandle resourceHandle,
			VkFormat format,
			VkDescriptorType descriptorType,
			BufferViewKind bufferKind,
			uint64_t offsetBytes,
			uint64_t sizeBytes,
			uint32_t strideBytes,
			const CbvDesc* cbvDesc) noexcept {
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			VulkanImageViewSlot* descriptorSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::CbvSrvUav);
			VulkanResource* resource = VkResourceState(impl, resourceHandle);
			if (!impl || !heap || !descriptorSlot || !resource || resource->type != ResourceType::Buffer || resource->buffer == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkResetDescriptorSlot(impl, *descriptorSlot);

			if (impl->descriptorHeapEnabled && heap->buffer != VK_NULL_HANDLE && resource->deviceAddress != 0) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					RHI_FAIL(Result::InvalidArgument);
				}

				std::memset(slotAddress, 0, static_cast<size_t>(heap->descriptorStride));

				VkHostAddressRangeEXT descriptorRange{};
				descriptorRange.address = slotAddress;
				descriptorRange.size = static_cast<size_t>(heap->descriptorStride);

				VkResult result = VK_SUCCESS;
				if (descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
					VkTexelBufferDescriptorInfoEXT texelInfo{ VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT };
					texelInfo.format = format;
					texelInfo.addressRange.address = resource->deviceAddress + offsetBytes;
					texelInfo.addressRange.size = sizeBytes;

					VkResourceDescriptorInfoEXT descriptorInfo{ VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT };
					descriptorInfo.type = descriptorType;
					descriptorInfo.data.pTexelBuffer = &texelInfo;
					result = vkWriteResourceDescriptorsEXT(impl->device, 1, &descriptorInfo, &descriptorRange);
				}
				else {
					VkDeviceAddressRangeEXT addressRange{};
					addressRange.address = resource->deviceAddress + offsetBytes;
					addressRange.size = sizeBytes;

					VkResourceDescriptorInfoEXT descriptorInfo{ VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT };
					descriptorInfo.type = descriptorType;
					descriptorInfo.data.pAddressRange = &addressRange;
					result = vkWriteResourceDescriptorsEXT(impl->device, 1, &descriptorInfo, &descriptorRange);
				}

				if (result != VK_SUCCESS) {
					return ToRHI(result);
				}
			}
			else if (format != VK_FORMAT_UNDEFINED) {
				VkBufferViewCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
				createInfo.buffer = resource->buffer;
				createInfo.format = format;
				createInfo.offset = offsetBytes;
				createInfo.range = sizeBytes;
				const VkResult result = vkCreateBufferView(impl->device, &createInfo, nullptr, &descriptorSlot->bufferView);
				if (result != VK_SUCCESS) {
					return ToRHI(result);
				}
			}

			descriptorSlot->kind = cbvDesc ? VulkanImageViewSlot::Kind::ConstantBuffer : VulkanImageViewSlot::Kind::BufferView;
			descriptorSlot->resource = resourceHandle;
			descriptorSlot->format = format;
			descriptorSlot->bufferKind = bufferKind;
			descriptorSlot->bufferOffset = offsetBytes;
			descriptorSlot->bufferSize = sizeBytes;
			descriptorSlot->bufferStride = strideBytes;
			if (cbvDesc) {
				descriptorSlot->cbv = *cbvDesc;
			}
			return Result::Ok;
		}

		static VkBufferUsageFlags VkBufferUsageForDesc(const ResourceDesc& desc) noexcept {
			VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
				VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			return usage;
		}

		static VkImageType VkImageTypeForResourceType(ResourceType type) noexcept {
			switch (type) {
			case ResourceType::Texture1D:
				return VK_IMAGE_TYPE_1D;
			case ResourceType::Texture2D:
				return VK_IMAGE_TYPE_2D;
			case ResourceType::Texture3D:
				return VK_IMAGE_TYPE_3D;
			default:
				return VK_IMAGE_TYPE_MAX_ENUM;
			}
		}

		static VkSampleCountFlagBits VkSampleCountForDesc(uint32_t sampleCount) noexcept {
			switch (sampleCount) {
			case 1: return VK_SAMPLE_COUNT_1_BIT;
			case 2: return VK_SAMPLE_COUNT_2_BIT;
			case 4: return VK_SAMPLE_COUNT_4_BIT;
			case 8: return VK_SAMPLE_COUNT_8_BIT;
			case 16: return VK_SAMPLE_COUNT_16_BIT;
			case 32: return VK_SAMPLE_COUNT_32_BIT;
			case 64: return VK_SAMPLE_COUNT_64_BIT;
			default: return VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
			}
		}

		static bool VkImageCreateFlagsForDesc(const ResourceDesc& desc, VkImageCreateFlags& flags) noexcept {
			flags = 0;
			const uint32_t resourceFlags = static_cast<uint32_t>(desc.resourceFlags);
			if ((resourceFlags & static_cast<uint32_t>(RF_TextureCubeCompatible)) != 0) {
				if (desc.type != ResourceType::Texture2D ||
					desc.texture.width != desc.texture.height ||
					desc.texture.depthOrLayers < 6 ||
					(desc.texture.depthOrLayers % 6) != 0) {
					return false;
				}
				flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			}
			return true;
		}

		static VkImageUsageFlags VkImageUsageForDesc(const ResourceDesc& desc, VkFormat format) noexcept {
			VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			const VkImageAspectFlags aspectMask = VkAspectMaskForFormat(format);
			const uint32_t resourceFlags = static_cast<uint32_t>(desc.resourceFlags);
			if ((resourceFlags & static_cast<uint32_t>(RF_AllowRenderTarget)) != 0) {
				usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			}
			if ((resourceFlags & static_cast<uint32_t>(RF_AllowDepthStencil)) != 0 || (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
				usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
			if ((resourceFlags & static_cast<uint32_t>(RF_AllowUnorderedAccess)) != 0) {
				usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			}
			if ((resourceFlags & static_cast<uint32_t>(RF_DenyShaderResource)) == 0) {
				usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			}
			return usage;
		}

		static VkMemoryPropertyFlags VkPreferredMemoryPropertyFlags(HeapType heapType) noexcept {
			switch (heapType) {
			case HeapType::DeviceLocal:
				return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			case HeapType::Upload:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			case HeapType::Readback:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			case HeapType::HostCached:
				return VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			case HeapType::GPUUpload:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			case HeapType::Custom:
			default:
				return 0;
			}
		}

		static VkMemoryPropertyFlags VkRequiredMemoryPropertyFlags(HeapType heapType) noexcept {
			switch (heapType) {
			case HeapType::DeviceLocal:
				return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			case HeapType::Upload:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			case HeapType::Readback:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			case HeapType::HostCached:
				return VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			case HeapType::GPUUpload:
				return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
			case HeapType::Custom:
			default:
				return 0;
			}
		}

		static bool VkFindMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memoryProperties,
			uint32_t memoryTypeBits,
			VkMemoryPropertyFlags preferredFlags,
			VkMemoryPropertyFlags requiredFlags,
			uint32_t& memoryTypeIndex) noexcept {
			for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
				const uint32_t bit = (1u << index);
				if ((memoryTypeBits & bit) == 0) {
					continue;
				}

				const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[index].propertyFlags;
				if ((flags & preferredFlags) == preferredFlags) {
					memoryTypeIndex = index;
					return true;
				}
			}

			for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
				const uint32_t bit = (1u << index);
				if ((memoryTypeBits & bit) == 0) {
					continue;
				}

				const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[index].propertyFlags;
				if ((flags & requiredFlags) == requiredFlags) {
					memoryTypeIndex = index;
					return true;
				}
			}

			return false;
		}

		static VkShaderStageFlags VkShaderStageFlagsForRHI(ShaderStage stages) noexcept {
			VkShaderStageFlags flags = 0;
			const uint32_t bits = static_cast<uint32_t>(stages);
			if ((bits & static_cast<uint32_t>(ShaderStage::Vertex)) != 0) flags |= VK_SHADER_STAGE_VERTEX_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Pixel)) != 0) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Compute)) != 0) flags |= VK_SHADER_STAGE_COMPUTE_BIT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Mesh)) != 0) flags |= VK_SHADER_STAGE_MESH_BIT_EXT;
			if ((bits & static_cast<uint32_t>(ShaderStage::Task)) != 0) flags |= VK_SHADER_STAGE_TASK_BIT_EXT;
			return flags != 0 ? flags : VK_SHADER_STAGE_ALL;
		}

		static uint32_t VkIndirectArgumentByteSize(const IndirectArg& arg) noexcept {
			switch (arg.kind) {
			case IndirectArgKind::Constant:
				return arg.u.rootConstants.num32 * 4u;
			case IndirectArgKind::Draw:
				return sizeof(VkDrawIndirectCommand);
			case IndirectArgKind::DrawIndexed:
				return sizeof(VkDrawIndexedIndirectCommand);
			case IndirectArgKind::Dispatch:
				return sizeof(VkDispatchIndirectCommand);
			case IndirectArgKind::DispatchMesh:
				return sizeof(VkDrawMeshTasksIndirectCommandEXT);
			default:
				return 0;
			}
		}

		static VkIndirectCommandsTokenTypeEXT VkIndirectCommandsTokenTypeForRHI(IndirectArgKind kind) noexcept {
			switch (kind) {
			case IndirectArgKind::Draw:
				return VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
			case IndirectArgKind::DrawIndexed:
				return VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT;
			case IndirectArgKind::Dispatch:
				return VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT;
			case IndirectArgKind::DispatchMesh:
				return VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT;
			default:
				return VK_INDIRECT_COMMANDS_TOKEN_TYPE_MAX_ENUM_EXT;
			}
		}

		static VkShaderStageFlags VkIndirectExecutionDomainStages(IndirectArgKind kind) noexcept {
			switch (kind) {
			case IndirectArgKind::Dispatch:
				return VK_SHADER_STAGE_COMPUTE_BIT;
			case IndirectArgKind::DispatchMesh:
				return VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
			case IndirectArgKind::Draw:
			case IndirectArgKind::DrawIndexed:
				return VK_SHADER_STAGE_ALL_GRAPHICS;
			default:
				return 0;
			}
		}

		static void VkDestroyCommandSignature(VulkanDevice* impl, VulkanCommandSignature& signature) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE) {
				signature = {};
				return;
			}

			if (signature.preprocessBuffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(impl->device, signature.preprocessBuffer, nullptr);
			}
			if (signature.preprocessMemory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, signature.preprocessMemory, nullptr);
			}
			if (signature.executionSet != VK_NULL_HANDLE && vkDestroyIndirectExecutionSetEXT) {
				vkDestroyIndirectExecutionSetEXT(impl->device, signature.executionSet, nullptr);
			}
			if (signature.indirectLayout != VK_NULL_HANDLE && vkDestroyIndirectCommandsLayoutEXT) {
				vkDestroyIndirectCommandsLayoutEXT(impl->device, signature.indirectLayout, nullptr);
			}

			signature = {};
		}

		static Result VkEnsureGeneratedCommandsExecutionSet(VulkanDevice* impl, VulkanCommandSignature& signature, VkPipeline pipeline) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE || !vkCreateIndirectExecutionSetEXT) {
				RHI_FAIL(Result::Unsupported);
			}
			if (signature.executionSet != VK_NULL_HANDLE) {
				if (signature.executionSetPipeline != pipeline) {
					RHI_FAIL(Result::Unsupported);
				}
				return Result::Ok;
			}

			VkIndirectExecutionSetPipelineInfoEXT pipelineInfo{ VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT };
			pipelineInfo.initialPipeline = pipeline;
			pipelineInfo.maxPipelineCount = 1;

			VkIndirectExecutionSetCreateInfoEXT createInfo{ VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT };
			createInfo.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
			createInfo.info.pPipelineInfo = &pipelineInfo;

			const VkResult result = vkCreateIndirectExecutionSetEXT(impl->device, &createInfo, nullptr, &signature.executionSet);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}
			signature.executionSetPipeline = pipeline;
			return Result::Ok;
		}

		static Result VkEnsureGeneratedCommandsPreprocessBuffer(VulkanDevice* impl, VulkanCommandSignature& signature, VkPipeline pipeline, uint32_t maxSequenceCount) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || !impl->bufferDeviceAddressEnabled || signature.indirectLayout == VK_NULL_HANDLE || !vkGetGeneratedCommandsMemoryRequirementsEXT) {
				RHI_FAIL(Result::Unsupported);
			}
			if (signature.usesExecutionSet && signature.executionSet == VK_NULL_HANDLE) {
				RHI_FAIL(Result::Unsupported);
			}
			if (!signature.usesExecutionSet && pipeline == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkGeneratedCommandsMemoryRequirementsInfoEXT requirementsInfo{ VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT };
			VkGeneratedCommandsPipelineInfoEXT pipelineInfo{ VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT };
			if (!signature.usesExecutionSet) {
				pipelineInfo.pipeline = pipeline;
				requirementsInfo.pNext = &pipelineInfo;
			}
			requirementsInfo.indirectExecutionSet = signature.usesExecutionSet ? signature.executionSet : VK_NULL_HANDLE;
			requirementsInfo.indirectCommandsLayout = signature.indirectLayout;
			requirementsInfo.maxSequenceCount = maxSequenceCount;
			requirementsInfo.maxDrawCount = maxSequenceCount;

			VkMemoryRequirements2 requirements{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
			vkGetGeneratedCommandsMemoryRequirementsEXT(impl->device, &requirementsInfo, &requirements);
			if (requirements.memoryRequirements.size == 0) {
				RHI_FAIL(Result::Unsupported);
			}

			if (signature.preprocessBuffer != VK_NULL_HANDLE &&
				signature.preprocessSize >= requirements.memoryRequirements.size &&
				signature.preprocessMaxSequenceCount >= maxSequenceCount) {
				return Result::Ok;
			}

			if (signature.preprocessBuffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(impl->device, signature.preprocessBuffer, nullptr);
				signature.preprocessBuffer = VK_NULL_HANDLE;
			}
			if (signature.preprocessMemory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, signature.preprocessMemory, nullptr);
				signature.preprocessMemory = VK_NULL_HANDLE;
			}
			signature.preprocessAddress = 0;
			signature.preprocessSize = 0;
			signature.preprocessMaxSequenceCount = 0;

			VkBufferUsageFlags2CreateInfo usage2{ VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO };
			usage2.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
			VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufferInfo.pNext = &usage2;
			bufferInfo.size = requirements.memoryRequirements.size;
			bufferInfo.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkResult result = vkCreateBuffer(impl->device, &bufferInfo, nullptr, &signature.preprocessBuffer);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			VkMemoryRequirements bufferRequirements{};
			vkGetBufferMemoryRequirements(impl->device, signature.preprocessBuffer, &bufferRequirements);
			uint32_t memoryTypeIndex = 0;
			if (!VkFindMemoryTypeIndex(impl->memoryProperties, bufferRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, memoryTypeIndex)) {
				vkDestroyBuffer(impl->device, signature.preprocessBuffer, nullptr);
				signature.preprocessBuffer = VK_NULL_HANDLE;
				RHI_FAIL(Result::OutOfMemory);
			}

			VkMemoryAllocateFlagsInfo allocateFlags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
			allocateFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
			VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			allocateInfo.pNext = &allocateFlags;
			allocateInfo.allocationSize = bufferRequirements.size;
			allocateInfo.memoryTypeIndex = memoryTypeIndex;
			result = vkAllocateMemory(impl->device, &allocateInfo, nullptr, &signature.preprocessMemory);
			if (result != VK_SUCCESS) {
				vkDestroyBuffer(impl->device, signature.preprocessBuffer, nullptr);
				signature.preprocessBuffer = VK_NULL_HANDLE;
				return ToRHI(result);
			}

			result = vkBindBufferMemory(impl->device, signature.preprocessBuffer, signature.preprocessMemory, 0);
			if (result != VK_SUCCESS) {
				vkDestroyBuffer(impl->device, signature.preprocessBuffer, nullptr);
				vkFreeMemory(impl->device, signature.preprocessMemory, nullptr);
				signature.preprocessBuffer = VK_NULL_HANDLE;
				signature.preprocessMemory = VK_NULL_HANDLE;
				return ToRHI(result);
			}

			VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
			addressInfo.buffer = signature.preprocessBuffer;
			signature.preprocessAddress = vkGetBufferDeviceAddress(impl->device, &addressInfo);
			signature.preprocessSize = requirements.memoryRequirements.size;
			signature.preprocessMaxSequenceCount = maxSequenceCount;
			if (signature.preprocessAddress == 0) {
				RHI_FAIL(Result::Unsupported);
			}
			return Result::Ok;
		}

		static VkImageSubresourceRange VkMakeImageSubresourceRange(VulkanResource& resource, const TextureSubresourceRange& range, VkImageAspectFlags aspectMask) noexcept {
			VkImageSubresourceRange subresourceRange{};
			subresourceRange.aspectMask = aspectMask;
			subresourceRange.baseMipLevel = range.baseMip;
			subresourceRange.levelCount = range.mipCount != 0 ? range.mipCount : resource.mipLevels;
			subresourceRange.baseArrayLayer = range.baseLayer;
			subresourceRange.layerCount = range.layerCount != 0 ? range.layerCount : resource.depthOrLayers;
			return subresourceRange;
		}

		static VkImageLayout VkToImageLayout(ResourceLayout layout, VkImageAspectFlags aspectMask) noexcept {
			switch (layout) {
			case ResourceLayout::Common:
			case ResourceLayout::DirectCommon:
			case ResourceLayout::ComputeCommon:
				return VK_IMAGE_LAYOUT_GENERAL;
			case ResourceLayout::Present:
				return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case ResourceLayout::GenericRead:
			case ResourceLayout::DirectGenericRead:
			case ResourceLayout::ComputeGenericRead:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ResourceLayout::RenderTarget:
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ResourceLayout::RenderTargetClear:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceLayout::UnorderedAccess:
			case ResourceLayout::DirectUnorderedAccess:
			case ResourceLayout::ComputeUnorderedAccess:
				return VK_IMAGE_LAYOUT_GENERAL;
			case ResourceLayout::DepthReadWrite:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case ResourceLayout::DepthStencilClear:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceLayout::DepthRead:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			case ResourceLayout::ShaderResource:
			case ResourceLayout::DirectShaderResource:
			case ResourceLayout::ComputeShaderResource:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ResourceLayout::CopyDest:
			case ResourceLayout::DirectCopyDest:
			case ResourceLayout::ComputeCopyDest:
			case ResourceLayout::ResolveDest:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceLayout::CopySource:
			case ResourceLayout::DirectCopySource:
			case ResourceLayout::ComputeCopySource:
			case ResourceLayout::ResolveSource:
				return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case ResourceLayout::ShadingRateSource:
#ifdef VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR
				return VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
#else
				return VK_IMAGE_LAYOUT_GENERAL;
#endif
			case ResourceLayout::Undefined:
			default:
				return VK_IMAGE_LAYOUT_UNDEFINED;
			}
		}

		static void VkGetBarrierMasksForLayout(VkImageLayout layout, VkPipelineStageFlags& stageMask, VkAccessFlags& accessMask) noexcept {
			switch (layout) {
			case VK_IMAGE_LAYOUT_UNDEFINED:
				stageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				accessMask = 0;
				break;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				accessMask = 0;
				break;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				accessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				break;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				accessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				break;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
				accessMask = VK_ACCESS_SHADER_READ_BIT;
				break;
			case VK_IMAGE_LAYOUT_GENERAL:
				stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				accessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
				accessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
				accessMask = VK_ACCESS_TRANSFER_READ_BIT;
				break;
#ifdef VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR
			case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
				stageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
				accessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
				break;
#endif
			default:
				stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				accessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				break;
			}
		}

		static void VkTransitionResourceLayout(VkCommandBuffer commandBuffer, VulkanResource& resource, VkImageAspectFlags aspectMask, ResourceLayout newLayout) noexcept {
			if (resource.image == VK_NULL_HANDLE && !resource.isSwapchainImage) {
				return;
			}

			const VkImageLayout oldLayout = VkToImageLayout(resource.currentLayout, aspectMask);
			const VkImageLayout targetLayout = VkToImageLayout(newLayout, aspectMask);
			if (oldLayout == targetLayout) {
				resource.currentLayout = newLayout;
				return;
			}

			VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			VkAccessFlags srcAccessMask = 0;
			VkAccessFlags dstAccessMask = 0;
			VkGetBarrierMasksForLayout(oldLayout, srcStageMask, srcAccessMask);
			VkGetBarrierMasksForLayout(targetLayout, dstStageMask, dstAccessMask);
			if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
				srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				srcAccessMask = 0;
			}
			if (targetLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
				dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				dstAccessMask = 0;
			}

			VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.srcAccessMask = srcAccessMask;
			barrier.dstAccessMask = dstAccessMask;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = targetLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = resource.image;
			barrier.subresourceRange.aspectMask = aspectMask;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = resource.mipLevels;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = resource.depthOrLayers;

			vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			resource.currentLayout = newLayout;
		}

		static Result VkCreateImageViewSlot(VulkanDevice* impl,
			DescriptorSlot slot,
			DescriptorHeapType expectedType,
			ResourceHandle resourceHandle,
			VkFormat viewFormat,
			VkImageAspectFlags aspectMask,
			VkImageViewType viewType,
			const TextureSubresourceRange& range) noexcept {
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, slot, expectedType);
			VulkanResource* resource = VkResourceState(impl, resourceHandle);
			if (!heap || !viewSlot || !resource || resource->image == VK_NULL_HANDLE || viewFormat == VK_FORMAT_UNDEFINED || viewType == VK_IMAGE_VIEW_TYPE_MAX_ENUM) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkResetDescriptorSlot(impl, *viewSlot);

			VkImageViewCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			createInfo.image = resource->image;
			createInfo.viewType = viewType;
			createInfo.format = viewFormat;
			createInfo.subresourceRange = VkMakeImageSubresourceRange(*resource, range, aspectMask);

			const VkResult result = vkCreateImageView(impl->device, &createInfo, nullptr, &viewSlot->view);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			viewSlot->kind = VulkanImageViewSlot::Kind::ImageView;
			viewSlot->resource = resourceHandle;
			viewSlot->format = viewFormat;
			viewSlot->aspectMask = aspectMask;
			viewSlot->range = range;
			return Result::Ok;
		}

		static void VkReleaseViewsForResource(VulkanDevice* impl, ResourceHandle resourceHandle) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || !resourceHandle.valid()) {
				return;
			}

			for (auto& heapSlot : impl->descriptorHeaps.slots) {
				if (!heapSlot.alive) {
					continue;
				}

				for (VulkanImageViewSlot& viewSlot : heapSlot.obj.imageViewSlots) {
					if (viewSlot.resource.index != resourceHandle.index || viewSlot.resource.generation != resourceHandle.generation) {
						continue;
					}

					VkResetDescriptorSlot(impl, viewSlot);
				}
			}
		}

		static void VkDestroyResource(VulkanDevice* impl, VulkanResource& resource) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE) {
				resource = {};
				return;
			}

			if (resource.mappedData != nullptr && resource.memory != VK_NULL_HANDLE) {
				vkUnmapMemory(impl->device, resource.memory);
				resource.mappedData = nullptr;
			}

			if (resource.ownsBuffer && resource.buffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(impl->device, resource.buffer, nullptr);
			}

			if (resource.ownsImage && resource.image != VK_NULL_HANDLE) {
				vkDestroyImage(impl->device, resource.image, nullptr);
			}

			if (resource.ownsMemory && resource.memory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, resource.memory, nullptr);
			}

			resource = {};
		}

		static void VkReleaseSwapchainImageHandles(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			if (!impl) {
				swapchain.imageHandles.clear();
				swapchain.images.clear();
				return;
			}

			for (const ResourceHandle handle : swapchain.imageHandles) {
				VkReleaseViewsForResource(impl, handle);
				if (VulkanResource* resource = VkResourceState(impl, handle)) {
					VkDestroyResource(impl, *resource);
				}
				impl->resources.free(handle);
			}

			swapchain.imageHandles.clear();
			swapchain.images.clear();
		}

		static void VkReleaseSwapchainPresentSemaphores(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE) {
				swapchain.presentWaitSemaphores.clear();
				return;
			}

			for (VkSemaphore semaphore : swapchain.presentWaitSemaphores) {
				if (semaphore != VK_NULL_HANDLE) {
					vkDestroySemaphore(impl->device, semaphore, nullptr);
				}
			}
			swapchain.presentWaitSemaphores.clear();
		}

		static Result VkCreateSwapchainPresentSemaphores(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || swapchain.imageCount == 0) {
				RHI_FAIL(Result::InvalidArgument);
			}

			swapchain.presentWaitSemaphores.assign(swapchain.imageCount, VK_NULL_HANDLE);
			VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			for (uint32_t imageIndex = 0; imageIndex < swapchain.imageCount; ++imageIndex) {
				VkSemaphore semaphore = VK_NULL_HANDLE;
				const VkResult result = vkCreateSemaphore(impl->device, &semaphoreInfo, nullptr, &semaphore);
				if (result != VK_SUCCESS) {
					VkReleaseSwapchainPresentSemaphores(impl, swapchain);
					return ToRHI(result);
				}
				swapchain.presentWaitSemaphores[imageIndex] = semaphore;
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(semaphore), VK_OBJECT_TYPE_SEMAPHORE, std::format("Swapchain Present Wait Semaphore {}", imageIndex).c_str());
			}

			return Result::Ok;
		}

		static Result VkAcquireNextSwapchainImage(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || swapchain.swapchain == VK_NULL_HANDLE || swapchain.acquireFence == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkResult result = vkResetFences(impl->device, 1, &swapchain.acquireFence);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			uint32_t imageIndex = 0;
			result = vkAcquireNextImageKHR(impl->device, swapchain.swapchain, UINT64_MAX, VK_NULL_HANDLE, swapchain.acquireFence, &imageIndex);
			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
				return ToRHI(result);
			}

			const VkResult waitResult = vkWaitForFences(impl->device, 1, &swapchain.acquireFence, VK_TRUE, UINT64_MAX);
			if (waitResult != VK_SUCCESS) {
				return ToRHI(waitResult);
			}

			swapchain.currentImageIndex = imageIndex;
			return ToRHI(result);
		}

		static VkSurfaceFormatKHR VkChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& surfaceFormats, Format requestedFormat) noexcept {
			const VkFormat requestedVkFormat = ToVkFormat(requestedFormat);
			if (requestedVkFormat != VK_FORMAT_UNDEFINED) {
				for (const VkSurfaceFormatKHR& format : surfaceFormats) {
					if (format.format == requestedVkFormat) {
						return format;
					}
				}
			}

			for (const VkSurfaceFormatKHR& format : surfaceFormats) {
				if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
					return format;
				}
			}

			return surfaceFormats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : surfaceFormats.front();
		}

		static VkPresentModeKHR VkChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes, bool allowTearing) noexcept {
			if (allowTearing) {
				for (VkPresentModeKHR mode : presentModes) {
					if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
						return mode;
					}
				}
				for (VkPresentModeKHR mode : presentModes) {
					if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
						return mode;
					}
				}
			}

			return VK_PRESENT_MODE_FIFO_KHR;
		}

		static uint32_t VkClampSwapchainImageCount(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t requestedCount) noexcept {
			uint32_t imageCount = requestedCount != 0 ? requestedCount : (capabilities.minImageCount + 1u);
			imageCount = (std::max)(imageCount, capabilities.minImageCount);
			if (capabilities.maxImageCount != 0) {
				imageCount = (std::min)(imageCount, capabilities.maxImageCount);
			}
			return imageCount;
		}

		static VkExtent2D VkChooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) noexcept {
			if (capabilities.currentExtent.width != UINT32_MAX) {
				return capabilities.currentExtent;
			}

			VkExtent2D extent{};
			extent.width = (std::min)((std::max)(width, capabilities.minImageExtent.width), capabilities.maxImageExtent.width);
			extent.height = (std::min)((std::max)(height, capabilities.minImageExtent.height), capabilities.maxImageExtent.height);
			return extent;
		}

		static Result VkPopulateSwapchainImages(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			uint32_t imageCount = 0;
			VkResult result = vkGetSwapchainImagesKHR(impl->device, swapchain.swapchain, &imageCount, nullptr);
			if (result != VK_SUCCESS || imageCount == 0) {
				return ToRHI(result);
			}

			swapchain.images.resize(imageCount);
			result = vkGetSwapchainImagesKHR(impl->device, swapchain.swapchain, &imageCount, swapchain.images.data());
			if (result != VK_SUCCESS) {
				swapchain.images.clear();
				return ToRHI(result);
			}

			swapchain.imageHandles.clear();
			swapchain.imageHandles.reserve(imageCount);
			for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
				VulkanResource resource{};
				resource.image = swapchain.images[imageIndex];
				resource.format = swapchain.format;
				resource.type = ResourceType::Texture2D;
				resource.width = swapchain.width;
				resource.height = swapchain.height;
				resource.depthOrLayers = 1;
				resource.mipLevels = 1;
				resource.currentLayout = ResourceLayout::Undefined;
				resource.isSwapchainImage = true;
				swapchain.imageHandles.push_back(impl->resources.alloc(resource));
			}

			swapchain.imageCount = imageCount;
			return Result::Ok;
		}

		static Result VkCreateOrResizeSwapchain(VulkanDevice* impl, VulkanSwapchain& swapchain, uint32_t width, uint32_t height, Format requestedFormat, uint32_t bufferCount, bool allowTearing) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || impl->physicalDevice == VK_NULL_HANDLE || swapchain.surface == VK_NULL_HANDLE || !impl->swapchainExtensionEnabled) {
				RHI_FAIL(Result::Unsupported);
			}

			VkBool32 graphicsPresentSupported = VK_FALSE;
			VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(impl->physicalDevice, impl->queues[0].familyIndex, swapchain.surface, &graphicsPresentSupported);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}
			if (graphicsPresentSupported != VK_TRUE) {
				RHI_FAIL(Result::Unsupported);
			}

			VkSurfaceCapabilitiesKHR capabilities{};
			result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl->physicalDevice, swapchain.surface, &capabilities);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			uint32_t formatCount = 0;
			result = vkGetPhysicalDeviceSurfaceFormatsKHR(impl->physicalDevice, swapchain.surface, &formatCount, nullptr);
			if (result != VK_SUCCESS || formatCount == 0) {
				return ToRHI(result);
			}

			std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
			result = vkGetPhysicalDeviceSurfaceFormatsKHR(impl->physicalDevice, swapchain.surface, &formatCount, surfaceFormats.data());
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			uint32_t presentModeCount = 0;
			result = vkGetPhysicalDeviceSurfacePresentModesKHR(impl->physicalDevice, swapchain.surface, &presentModeCount, nullptr);
			if (result != VK_SUCCESS || presentModeCount == 0) {
				return ToRHI(result);
			}

			std::vector<VkPresentModeKHR> presentModes(presentModeCount);
			result = vkGetPhysicalDeviceSurfacePresentModesKHR(impl->physicalDevice, swapchain.surface, &presentModeCount, presentModes.data());
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			const VkSurfaceFormatKHR surfaceFormat = VkChooseSurfaceFormat(surfaceFormats, requestedFormat);
			if (surfaceFormat.format == VK_FORMAT_UNDEFINED) {
				RHI_FAIL(Result::Unsupported);
			}

			const VkExtent2D extent = VkChooseSwapchainExtent(capabilities, width, height);
			const uint32_t imageCount = VkClampSwapchainImageCount(capabilities, bufferCount);
			const VkPresentModeKHR presentMode = VkChoosePresentMode(presentModes, allowTearing);

			std::vector<ResourceHandle> oldImageHandles = std::move(swapchain.imageHandles);
			std::vector<VkImage> oldImages = std::move(swapchain.images);
			std::vector<VkSemaphore> oldPresentWaitSemaphores = std::move(swapchain.presentWaitSemaphores);
			VkSwapchainKHR oldSwapchain = swapchain.swapchain;

			VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
			createInfo.surface = swapchain.surface;
			createInfo.minImageCount = imageCount;
			createInfo.imageFormat = surfaceFormat.format;
			createInfo.imageColorSpace = surfaceFormat.colorSpace;
			createInfo.imageExtent = extent;
			createInfo.imageArrayLayers = 1;
			createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			createInfo.preTransform = capabilities.currentTransform;
			createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			createInfo.presentMode = presentMode;
			createInfo.clipped = VK_TRUE;
			createInfo.oldSwapchain = oldSwapchain;

			VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
			result = vkCreateSwapchainKHR(impl->device, &createInfo, nullptr, &newSwapchain);
			if (result != VK_SUCCESS) {
				swapchain.imageHandles = std::move(oldImageHandles);
				swapchain.images = std::move(oldImages);
				swapchain.presentWaitSemaphores = std::move(oldPresentWaitSemaphores);
				swapchain.swapchain = oldSwapchain;
				return ToRHI(result);
			}

			swapchain.swapchain = newSwapchain;
			swapchain.format = surfaceFormat.format;
			swapchain.rhiFormat = FromVkFormat(surfaceFormat.format);
			swapchain.width = extent.width;
			swapchain.height = extent.height;
			swapchain.allowTearing = allowTearing;

			if (swapchain.acquireFence == VK_NULL_HANDLE) {
				VkFenceCreateInfo fenceCreateInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				result = vkCreateFence(impl->device, &fenceCreateInfo, nullptr, &swapchain.acquireFence);
				if (result != VK_SUCCESS) {
					vkDestroySwapchainKHR(impl->device, newSwapchain, nullptr);
					swapchain.swapchain = oldSwapchain;
					swapchain.imageHandles = std::move(oldImageHandles);
					swapchain.images = std::move(oldImages);
					swapchain.presentWaitSemaphores = std::move(oldPresentWaitSemaphores);
					return ToRHI(result);
				}
			}

			const Result populateResult = VkPopulateSwapchainImages(impl, swapchain);
			if (populateResult != Result::Ok) {
				VkReleaseSwapchainImageHandles(impl, swapchain);
				VkReleaseSwapchainPresentSemaphores(impl, swapchain);
				vkDestroySwapchainKHR(impl->device, newSwapchain, nullptr);
				swapchain.swapchain = oldSwapchain;
				swapchain.imageHandles = std::move(oldImageHandles);
				swapchain.images = std::move(oldImages);
				swapchain.presentWaitSemaphores = std::move(oldPresentWaitSemaphores);
				return populateResult;
			}

			const Result semaphoreResult = VkCreateSwapchainPresentSemaphores(impl, swapchain);
			if (semaphoreResult != Result::Ok) {
				VkReleaseSwapchainImageHandles(impl, swapchain);
				VkReleaseSwapchainPresentSemaphores(impl, swapchain);
				vkDestroySwapchainKHR(impl->device, newSwapchain, nullptr);
				swapchain.swapchain = oldSwapchain;
				swapchain.imageHandles = std::move(oldImageHandles);
				swapchain.images = std::move(oldImages);
				swapchain.presentWaitSemaphores = std::move(oldPresentWaitSemaphores);
				return semaphoreResult;
			}

			for (const ResourceHandle handle : oldImageHandles) {
				if (VulkanResource* resource = VkResourceState(impl, handle)) {
					VkDestroyResource(impl, *resource);
				}
				impl->resources.free(handle);
			}
			for (VkSemaphore semaphore : oldPresentWaitSemaphores) {
				if (semaphore != VK_NULL_HANDLE) {
					vkDestroySemaphore(impl->device, semaphore, nullptr);
				}
			}

			if (oldSwapchain != VK_NULL_HANDLE) {
				vkDestroySwapchainKHR(impl->device, oldSwapchain, nullptr);
			}

			return VkAcquireNextSwapchainImage(impl, swapchain);
		}

		static void VkCopyAdapterName(char* dst, size_t dstSize, const char* src) noexcept {
			if (!dst || dstSize == 0) {
				return;
			}

			std::snprintf(dst, dstSize, "%s", src ? src : "");
		}

		static uint64_t VkSumHeapBudget(const VkPhysicalDeviceMemoryProperties& memoryProperties, bool deviceLocal) noexcept {
			uint64_t total = 0;
			for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex) {
				const VkMemoryHeap& heap = memoryProperties.memoryHeaps[heapIndex];
				const bool heapDeviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
				if (heapDeviceLocal == deviceLocal) {
					total += heap.size;
				}
			}
			return total;
		}

		static QueryResultInfo qp_getQueryResultInfo(QueryPool* qp) noexcept {
			auto* impl = qp ? static_cast<VulkanDevice*>(qp->impl) : nullptr;
			VulkanQueryPool* queryPool = VkQueryPoolState(impl, qp ? qp->GetHandle() : QueryPoolHandle{});
			QueryResultInfo out{};
			if (!queryPool) {
				return out;
			}
			out.type = queryPool->type;
			out.count = queryPool->count;
			out.elementAlignment = 8;
			switch (queryPool->type) {
			case QueryType::Timestamp:
			case QueryType::Occlusion:
				out.elementSize = sizeof(uint64_t);
				break;
			case QueryType::PipelineStatistics:
				out.elementSize = VkPipelineStatsFieldCount(queryPool->vkStats) * sizeof(uint64_t);
				break;
			}
			return out;
		}

		static PipelineStatsLayout qp_getPipelineStatsLayout(QueryPool* qp, PipelineStatsFieldDesc* outBuf, uint32_t outCap) noexcept {
			auto* impl = qp ? static_cast<VulkanDevice*>(qp->impl) : nullptr;
			VulkanQueryPool* queryPool = VkQueryPoolState(impl, qp ? qp->GetHandle() : QueryPoolHandle{});
			PipelineStatsLayout layout{};
			layout.info = qp_getQueryResultInfo(qp);
			if (!queryPool || queryPool->type != QueryType::PipelineStatistics || !outBuf || outCap == 0) {
				return layout;
			}

			uint32_t offset = 0;
			uint32_t written = 0;
			auto pushField = [&](PipelineStatsMask mask, PipelineStatTypes field) {
				if ((queryPool->statsMask & mask) == 0 || written >= outCap) {
					return;
				}
				outBuf[written++] = { field, offset, static_cast<uint32_t>(sizeof(uint64_t)), true };
				offset += static_cast<uint32_t>(sizeof(uint64_t));
			};

			pushField(PS_IAVertices, PipelineStatTypes::IAVertices);
			pushField(PS_IAPrimitives, PipelineStatTypes::IAPrimitives);
			pushField(PS_VSInvocations, PipelineStatTypes::VSInvocations);
			pushField(PS_GSInvocations, PipelineStatTypes::GSInvocations);
			pushField(PS_GSPrimitives, PipelineStatTypes::GSPrimitives);
			pushField(PS_CInvocations, PipelineStatTypes::TSControlInvocations);
			pushField(PS_CPrimitives, PipelineStatTypes::TSEvaluationInvocations);
			pushField(PS_PSInvocations, PipelineStatTypes::PSInvocations);
			pushField(PS_CSInvocations, PipelineStatTypes::CSInvocations);
			pushField(PS_TaskInvocations, PipelineStatTypes::TaskInvocations);
			pushField(PS_MeshInvocations, PipelineStatTypes::MeshInvocations);
			layout.fields = { outBuf, written };
			return layout;
		}

		static void qp_setName(QueryPool* qp, const char* name) noexcept {
			auto* impl = qp ? static_cast<VulkanDevice*>(qp->impl) : nullptr;
			VulkanQueryPool* queryPool = VkQueryPoolState(impl, qp ? qp->GetHandle() : QueryPoolHandle{});
			if (queryPool) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(queryPool->pool), VK_OBJECT_TYPE_QUERY_POOL, name);
			}
		}

		static void pso_setName(Pipeline* pipeline, const char* name) noexcept {
			auto* impl = pipeline ? static_cast<VulkanDevice*>(pipeline->impl) : nullptr;
			VulkanPipeline* pipelineState = VkPipelineState(impl, pipeline ? pipeline->GetHandle() : PipelineHandle{});
			if (pipelineState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(pipelineState->pipeline), VK_OBJECT_TYPE_PIPELINE, name);
			}
		}

		static void wg_setName(WorkGraph* workGraph, const char* name) noexcept {
			VkIgnoreUnused(workGraph, name);
		}

		static uint64_t wg_getRequiredScratchMemorySize(WorkGraph* workGraph) noexcept {
			VkIgnoreUnused(workGraph);
			return 0;
		}

		static void pl_setName(PipelineLayout* layout, const char* name) noexcept {
			VkIgnoreUnused(layout, name);
		}

		static void cs_setName(CommandSignature* signature, const char* name) noexcept {
			VkIgnoreUnused(signature, name);
		}

		static void dh_setName(DescriptorHeap* heap, const char* name) noexcept {
			auto* impl = heap ? static_cast<VulkanDevice*>(heap->impl) : nullptr;
			VulkanDescriptorHeap* heapState = VkDescriptorHeapState(impl, heap ? heap->GetHandle() : DescriptorHeapHandle{});
			if (heapState && heapState->buffer != VK_NULL_HANDLE) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(heapState->buffer), VK_OBJECT_TYPE_BUFFER, name);
			}
		}

		static void s_setName(Sampler* sampler, const char* name) noexcept {
			VkIgnoreUnused(sampler, name);
		}

		static uint64_t tl_timelineCompletedValue(Timeline* timeline) noexcept {
			auto* impl = timeline ? static_cast<VulkanDevice*>(timeline->impl) : nullptr;
			VulkanTimeline* timelineState = VkTimelineState(impl, timeline ? timeline->GetHandle() : TimelineHandle{});
			if (!impl || !timelineState || timelineState->semaphore == VK_NULL_HANDLE) {
				return 0;
			}

			uint64_t value = 0;
			return vkGetSemaphoreCounterValue(impl->device, timelineState->semaphore, &value) == VK_SUCCESS ? value : 0;
		}

		static Result tl_timelineHostWait(Timeline* timeline, const uint64_t value, uint32_t timeoutMs) noexcept {
			auto* impl = timeline ? static_cast<VulkanDevice*>(timeline->impl) : nullptr;
			VulkanTimeline* timelineState = VkTimelineState(impl, timeline ? timeline->GetHandle() : TimelineHandle{});
			if (!impl || !timelineState || timelineState->semaphore == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
			waitInfo.semaphoreCount = 1;
			waitInfo.pSemaphores = &timelineState->semaphore;
			waitInfo.pValues = &value;
			const uint64_t timeoutNs = timeoutMs == UINT32_MAX
				? UINT64_MAX
				: static_cast<uint64_t>(timeoutMs) * 1000000ull;
			return ToRHI(vkWaitSemaphores(impl->device, &waitInfo, timeoutNs));
		}

		static void tl_setName(Timeline* timeline, const char* name) noexcept {
			auto* impl = timeline ? static_cast<VulkanDevice*>(timeline->impl) : nullptr;
			VulkanTimeline* timelineState = VkTimelineState(impl, timeline ? timeline->GetHandle() : TimelineHandle{});
			if (timelineState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(timelineState->semaphore), VK_OBJECT_TYPE_SEMAPHORE, name);
			}
		}

		static void h_setName(Heap* heap, const char* name) noexcept {
			auto* impl = heap ? static_cast<VulkanDevice*>(heap->impl) : nullptr;
			VulkanHeap* heapState = VkHeapState(impl, heap ? heap->GetHandle() : HeapHandle{});
			if (heapState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(heapState->memory), VK_OBJECT_TYPE_DEVICE_MEMORY, name);
			}
		}

		static Result VkValidateAndApplySubmittedBarriers(VulkanDevice* impl, Span<CommandList> lists) noexcept {
			struct TextureStateSnapshot {
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceLayout layout = ResourceLayout::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};
			struct TextureStateUpdate {
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceLayout layout = ResourceLayout::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};
			struct BufferStateSnapshot {
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};
			struct BufferStateUpdate {
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};

			for (uint32_t listIndex = 0; listIndex < lists.size; ++listIndex) {
				VulkanCommandList* commandListState = VkCommandListState(impl, lists.data[listIndex].GetHandle());
				if (!commandListState) {
					RHI_FAIL(Result::InvalidArgument);
				}

				for (const VulkanCommandList::RecordedBarrierBatch& batch : commandListState->recordedBarrierBatches) {
					std::vector<TextureStateSnapshot> textureSnapshots;
					std::vector<TextureStateUpdate> textureUpdates;
					std::vector<BufferStateSnapshot> bufferSnapshots;
					std::vector<BufferStateUpdate> bufferUpdates;

					auto getTextureSnapshot = [&textureSnapshots](VulkanResource* resource) -> TextureStateSnapshot {
						for (const TextureStateSnapshot& snapshot : textureSnapshots) {
							if (snapshot.resource == resource) {
								return snapshot;
							}
						}
						TextureStateSnapshot snapshot{};
						snapshot.resource = resource;
						snapshot.access = resource->submittedAccess;
						snapshot.layout = resource->submittedLayout;
						snapshot.sync = resource->submittedSync;
						textureSnapshots.push_back(snapshot);
						return snapshot;
					};
					auto getBufferSnapshot = [&bufferSnapshots](VulkanResource* resource) -> BufferStateSnapshot {
						for (const BufferStateSnapshot& snapshot : bufferSnapshots) {
							if (snapshot.resource == resource) {
								return snapshot;
							}
						}
						BufferStateSnapshot snapshot{};
						snapshot.resource = resource;
						snapshot.access = resource->submittedAccess;
						snapshot.sync = resource->submittedSync;
						bufferSnapshots.push_back(snapshot);
						return snapshot;
					};

					for (const VulkanCommandList::RecordedTextureBarrier& barrier : batch.textures) {
						VulkanResource* resource = VkResourceState(impl, barrier.texture);
						if (!resource || resource->image == VK_NULL_HANDLE) {
							continue;
						}
						const TextureStateSnapshot snapshot = getTextureSnapshot(resource);
						const bool firstUseFromUndefined = !barrier.discard && snapshot.layout == ResourceLayout::Undefined;
						if (!barrier.discard && !firstUseFromUndefined && snapshot.layout != barrier.beforeLayout) {
							spdlog::error(
								"Vulkan barrier state mismatch for texture handle {}: backend layout={} graph beforeLayout={} afterLayout={}",
								barrier.texture.index,
								static_cast<uint32_t>(snapshot.layout),
								static_cast<uint32_t>(barrier.beforeLayout),
								static_cast<uint32_t>(barrier.afterLayout));
							RHI_FAIL(Result::InvalidCall);
						}
						if (!barrier.discard && snapshot.access != barrier.beforeAccess) {
							spdlog::error(
								"Vulkan barrier state mismatch for texture handle {}: backend access={} graph beforeAccess={} afterAccess={}",
								barrier.texture.index,
								static_cast<uint32_t>(snapshot.access),
								static_cast<uint32_t>(barrier.beforeAccess),
								static_cast<uint32_t>(barrier.afterAccess));
							RHI_FAIL(Result::InvalidCall);
						}
						textureUpdates.push_back(TextureStateUpdate{ resource, barrier.afterAccess, barrier.afterLayout, barrier.afterSync });
					}

					for (const VulkanCommandList::RecordedBufferBarrier& barrier : batch.buffers) {
						VulkanResource* resource = VkResourceState(impl, barrier.buffer);
						if (!resource || resource->buffer == VK_NULL_HANDLE) {
							continue;
						}
						const BufferStateSnapshot snapshot = getBufferSnapshot(resource);
						if (!barrier.discard && snapshot.access != barrier.beforeAccess) {
							spdlog::error(
								"Vulkan barrier state mismatch for buffer handle {}: backend access={} graph beforeAccess={} afterAccess={}",
								barrier.buffer.index,
								static_cast<uint32_t>(snapshot.access),
								static_cast<uint32_t>(barrier.beforeAccess),
								static_cast<uint32_t>(barrier.afterAccess));
							RHI_FAIL(Result::InvalidCall);
						}
						bufferUpdates.push_back(BufferStateUpdate{ resource, barrier.afterAccess, barrier.afterSync });
					}

					for (const TextureStateUpdate& update : textureUpdates) {
						update.resource->submittedAccess = update.access;
						update.resource->submittedLayout = update.layout;
						update.resource->submittedSync = update.sync;
						update.resource->currentAccess = update.access;
						update.resource->currentLayout = update.layout;
						update.resource->currentSync = update.sync;
					}
					for (const BufferStateUpdate& update : bufferUpdates) {
						update.resource->submittedAccess = update.access;
						update.resource->submittedSync = update.sync;
						update.resource->currentAccess = update.access;
						update.resource->currentSync = update.sync;
					}
				}
			}

			return Result::Ok;
		}

		static Result q_submit(Queue* queue, Span<CommandList> lists, const SubmitDesc& submit) noexcept {
			auto* impl = queue ? static_cast<VulkanDevice*>(queue->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if ((submit.waits.size != 0 || submit.signals.size != 0) && !impl->timelineSemaphoreEnabled) {
				RHI_FAIL(Result::Unsupported);
			}

			VulkanQueueState* queueState = VkQueueStateForHandle(impl, queue->GetQueueHandle());
			if (!queueState || queueState->queue == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (lists.size == 0 && submit.waits.size == 0 && submit.signals.size == 0) {
				return Result::Ok;
			}

			std::vector<VkCommandBuffer> commandBuffers;
			commandBuffers.reserve(lists.size);
			for (uint32_t index = 0; index < lists.size; ++index) {
				VulkanCommandList* commandListState = VkCommandListState(impl, lists.data[index].GetHandle());
				if (!commandListState || commandListState->commandBuffer == VK_NULL_HANDLE) {
					RHI_FAIL(Result::InvalidArgument);
				}

				if (commandListState->pendingError != Result::Ok) {
					RHI_FAIL(commandListState->pendingError);
				}

				if (commandListState->isRecording) {
					spdlog::error("Vulkan queue submit rejected a command list that is still recording");
					RHI_FAIL(Result::InvalidArgument);
				}

				commandBuffers.push_back(commandListState->commandBuffer);
			}

			if (impl->validateBarrierTransitions) {
				const Result barrierValidationResult = VkValidateAndApplySubmittedBarriers(impl, lists);
				if (barrierValidationResult != Result::Ok) {
					return barrierValidationResult;
				}
			}

			std::vector<VkSemaphore> waitSemaphores;
			std::vector<uint64_t> waitValues;
			std::vector<VkPipelineStageFlags> waitStages;
			std::vector<VkSemaphore> signalSemaphores;
			std::vector<uint64_t> signalValues;
			waitSemaphores.reserve(submit.waits.size);
			waitValues.reserve(submit.waits.size);
			waitStages.resize(submit.waits.size, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			signalSemaphores.reserve(submit.signals.size);
			signalValues.reserve(submit.signals.size);

			for (const TimelinePoint& wait : submit.waits) {
				VulkanTimeline* timeline = VkTimelineState(impl, wait.t);
				if (!timeline || timeline->semaphore == VK_NULL_HANDLE) {
					RHI_FAIL(Result::InvalidArgument);
				}
				waitSemaphores.push_back(timeline->semaphore);
				waitValues.push_back(wait.value);
			}
			for (const TimelinePoint& signal : submit.signals) {
				VulkanTimeline* timeline = VkTimelineState(impl, signal.t);
				if (!timeline || timeline->semaphore == VK_NULL_HANDLE || signal.value == 0) {
					RHI_FAIL(Result::InvalidArgument);
				}
				signalSemaphores.push_back(timeline->semaphore);
				signalValues.push_back(signal.value);
			}

			VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
			timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
			timelineInfo.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
			timelineInfo.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
			timelineInfo.pSignalSemaphoreValues = signalValues.empty() ? nullptr : signalValues.data();

			VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submitInfo.pNext = (waitValues.empty() && signalValues.empty()) ? nullptr : &timelineInfo;
			submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
			submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
			submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
			submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
			submitInfo.pCommandBuffers = commandBuffers.empty() ? nullptr : commandBuffers.data();
			submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
			submitInfo.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

			const Result submitResult = ToRHI(vkQueueSubmit(queueState->queue, 1, &submitInfo, VK_NULL_HANDLE));
			if (submitResult == Result::Ok) {
				for (uint32_t index = 0; index < lists.size; ++index) {
					if (VulkanCommandList* commandListState = VkCommandListState(impl, lists.data[index].GetHandle())) {
						commandListState->recordedBarrierBatches.clear();
						commandListState->recordingTextureStates.clear();
						commandListState->recordingBufferStates.clear();
					}
				}
			}
			return submitResult;
		}

		static Result q_signal(Queue* queue, const TimelinePoint& point) noexcept {
			CommandList* noLists = nullptr;
			SubmitDesc submit{};
			submit.signals = { &point, 1 };
			return q_submit(queue, { noLists, 0 }, submit);
		}

		static Result q_wait(Queue* queue, const TimelinePoint& point) noexcept {
			CommandList* noLists = nullptr;
			SubmitDesc submit{};
			submit.waits = { &point, 1 };
			return q_submit(queue, { noLists, 0 }, submit);
		}

		static void q_checkDebugMessages(Queue* queue) noexcept {
			VkIgnoreUnused(queue);
		}

		static void q_setName(Queue* queue, const char* name) noexcept {
			auto* impl = queue ? static_cast<VulkanDevice*>(queue->impl) : nullptr;
			VulkanQueueState* queueState = VkQueueStateForHandle(impl, queue ? queue->GetQueueHandle() : QueueHandle{});
			if (queueState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(queueState->queue), VK_OBJECT_TYPE_QUEUE, name);
			}
		}

		static void buf_map(Resource* resource, void** data, uint64_t offset, uint64_t size) noexcept {
			auto* impl = resource ? static_cast<VulkanDevice*>(resource->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource ? resource->GetHandle() : ResourceHandle{});
			if (data) {
				*data = nullptr;
			}
			if (!impl || !resourceState || !data || resourceState->memory == VK_NULL_HANDLE || !resourceState->hostVisible || offset > resourceState->bufferSize) {
				return;
			}

			if (resourceState->mappedData == nullptr) {
				const VkDeviceSize mapSize = size == ~0ull ? VK_WHOLE_SIZE : static_cast<VkDeviceSize>(size);
				void* mappedData = nullptr;
				if (vkMapMemory(impl->device, resourceState->memory, static_cast<VkDeviceSize>(offset), mapSize, 0, &mappedData) != VK_SUCCESS) {
					return;
				}
				resourceState->mappedData = mappedData;
			}

			*data = resourceState->mappedData;
		}

		static void buf_unmap(Resource* resource, uint64_t writeOffset, uint64_t writeSize) noexcept {
			VkIgnoreUnused(writeOffset, writeSize);
			auto* impl = resource ? static_cast<VulkanDevice*>(resource->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource ? resource->GetHandle() : ResourceHandle{});
			if (!impl || !resourceState || resourceState->memory == VK_NULL_HANDLE || resourceState->mappedData == nullptr) {
				return;
			}

			vkUnmapMemory(impl->device, resourceState->memory);
			resourceState->mappedData = nullptr;
		}

		static void buf_setName(Resource* resource, const char* name) noexcept {
			auto* impl = resource ? static_cast<VulkanDevice*>(resource->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource ? resource->GetHandle() : ResourceHandle{});
			if (resourceState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(resourceState->buffer), VK_OBJECT_TYPE_BUFFER, name);
			}
		}

		static void tex_map(Resource* resource, void** data, uint64_t offset, uint64_t size) noexcept {
			if (data) {
				*data = nullptr;
			}
			buf_map(resource, data, offset, size);
		}

		static void tex_unmap(Resource* resource, uint64_t writeOffset, uint64_t writeSize) noexcept {
			buf_unmap(resource, writeOffset, writeSize);
		}

		static void tex_setName(Resource* resource, const char* name) noexcept {
			auto* impl = resource ? static_cast<VulkanDevice*>(resource->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource ? resource->GetHandle() : ResourceHandle{});
			if (resourceState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(resourceState->image), VK_OBJECT_TYPE_IMAGE, name);
			}
		}

		static void cl_endPass(CommandList* commandList) noexcept;

		static void cl_end(CommandList* commandList) noexcept {
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!commandListState || commandListState->commandBuffer == VK_NULL_HANDLE || !commandListState->isRecording) {
				return;
			}

			if (commandListState->pendingError != Result::Ok) {
				BreakIfDebugging();
				return;
			}

			if (commandListState->passActive) {
				cl_endPass(commandList);
			}

			if (commandListState->pendingError != Result::Ok) {
				BreakIfDebugging();
				return;
			}

			const VkResult result = vkEndCommandBuffer(commandListState->commandBuffer);
			if (result != VK_SUCCESS) {
				spdlog::error("Vulkan command list end failed with VkResult {}", static_cast<int>(result));
				return;
			}

			commandListState->isRecording = false;
		}

		static void cl_reset(CommandList* commandList, const CommandAllocator& allocator) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanCommandAllocator* allocatorState = VkAllocatorState(impl, allocator.GetHandle());
			if (!impl || impl->device == VK_NULL_HANDLE || !commandListState || !allocatorState || allocatorState->pool == VK_NULL_HANDLE) {
				spdlog::error("Vulkan command list reset received invalid state");
				return;
			}

			if (commandListState->commandBuffer == VK_NULL_HANDLE || commandListState->allocatorHandle.index != allocator.GetHandle().index
				|| commandListState->allocatorHandle.generation != allocator.GetHandle().generation) {
				if (commandListState->commandBuffer != VK_NULL_HANDLE) {
					if (VulkanCommandAllocator* oldAllocator = VkAllocatorState(impl, commandListState->allocatorHandle);
						oldAllocator && oldAllocator->pool != VK_NULL_HANDLE) {
						vkFreeCommandBuffers(impl->device, oldAllocator->pool, 1, &commandListState->commandBuffer);
					}
					commandListState->commandBuffer = VK_NULL_HANDLE;
				}

				VkCommandBuffer newCommandBuffer = VK_NULL_HANDLE;
				if (VkAllocatePrimaryCommandBuffer(impl->device, allocatorState->pool, newCommandBuffer) != Result::Ok) {
					spdlog::error("Vulkan command list reset failed to allocate a command buffer");
					return;
				}
				commandListState->commandBuffer = newCommandBuffer;
			}
			else {
				const VkResult resetResult = vkResetCommandBuffer(commandListState->commandBuffer, 0);
				if (resetResult != VK_SUCCESS) {
					spdlog::error("Vulkan command list reset failed with VkResult {}", static_cast<int>(resetResult));
					return;
				}
			}

			commandListState->allocatorHandle = allocator.GetHandle();
			commandListState->kind = allocatorState->kind;
			if (VkBeginCommandRecording(commandListState->commandBuffer) != Result::Ok) {
				spdlog::error("Vulkan command list reset failed to begin recording");
				return;
			}

			commandListState->passActive = false;
			commandListState->boundLayout = {};
			commandListState->boundPipeline = {};
			commandListState->boundCbvSrvUavHeap = {};
			commandListState->boundSamplerHeap = {};
			commandListState->pendingError = Result::Ok;
			commandListState->recordedBarrierBatches.clear();
			commandListState->recordingTextureStates.clear();
			commandListState->recordingBufferStates.clear();
			for (auto& page : commandListState->emulatedRootConstantScratchPages) {
				page.cursor = 0;
			}
			commandListState->emulatedRootConstantShadowStates.clear();
			commandListState->passRenderArea = {};
			commandListState->passColorResources.clear();
			commandListState->passDepthResource = {};
			commandListState->isRecording = true;
		}

		static void cl_beginPass(CommandList* commandList, const PassBeginInfo& passInfo) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !impl->dynamicRenderingEnabled || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}

			std::vector<VkRenderingAttachmentInfo> colorAttachments;
			colorAttachments.reserve(passInfo.colors.size);
			commandListState->passColorResources.clear();

			for (const ColorAttachment& color : passInfo.colors) {
				VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, color.rtv, DescriptorHeapType::RTV);
				VulkanResource* resource = VkResourceState(impl, viewSlot ? viewSlot->resource : ResourceHandle{});
				if (!viewSlot || viewSlot->view == VK_NULL_HANDLE || !resource) {
					spdlog::error("Vulkan BeginPass: invalid RTV slot {}", color.rtv.index);
					return;
				}

				VkRenderingAttachmentInfo attachmentInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				attachmentInfo.imageView = viewSlot->view;
				attachmentInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				attachmentInfo.storeOp = VkStoreOpForAttachment(color.storeOp);
				switch (color.loadOp) {
				case LoadOp::Clear:
					attachmentInfo.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
					attachmentInfo.clearValue.color.float32[0] = color.clear.rgba[0];
					attachmentInfo.clearValue.color.float32[1] = color.clear.rgba[1];
					attachmentInfo.clearValue.color.float32[2] = color.clear.rgba[2];
					attachmentInfo.clearValue.color.float32[3] = color.clear.rgba[3];
					break;
				case LoadOp::DontCare:
					attachmentInfo.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
					break;
				case LoadOp::Load:
				default:
					attachmentInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
					break;
				}

				colorAttachments.push_back(attachmentInfo);
				commandListState->passColorResources.push_back(viewSlot->resource);
			}

			VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			VkRenderingAttachmentInfo stencilAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			const VkRenderingAttachmentInfo* depthAttachmentPtr = nullptr;
			const VkRenderingAttachmentInfo* stencilAttachmentPtr = nullptr;
			if (passInfo.depth) {
				VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, passInfo.depth->dsv, DescriptorHeapType::DSV);
				VulkanResource* resource = VkResourceState(impl, viewSlot ? viewSlot->resource : ResourceHandle{});
				if (!viewSlot || viewSlot->view == VK_NULL_HANDLE || !resource) {
					spdlog::error("Vulkan BeginPass: invalid DSV slot {}", passInfo.depth->dsv.index);
					return;
				}

				depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				depthAttachment.imageView = viewSlot->view;
				depthAttachment.imageLayout = passInfo.depth->readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depthAttachment.loadOp = passInfo.depth->depthLoad == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : (passInfo.depth->depthLoad == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
				depthAttachment.storeOp = VkStoreOpForAttachment(passInfo.depth->depthStore, passInfo.depth->readOnly);
				depthAttachment.clearValue.depthStencil.depth = passInfo.depth->clear.depthStencil.depth;
				depthAttachment.clearValue.depthStencil.stencil = passInfo.depth->clear.depthStencil.stencil;
				depthAttachmentPtr = (viewSlot->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? &depthAttachment : nullptr;

				if ((viewSlot->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
					stencilAttachment = depthAttachment;
					stencilAttachment.loadOp = passInfo.depth->stencilLoad == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : (passInfo.depth->stencilLoad == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
					stencilAttachment.storeOp = VkStoreOpForAttachment(passInfo.depth->stencilStore, passInfo.depth->readOnly);
					stencilAttachmentPtr = &stencilAttachment;
				}

				commandListState->passDepthResource = viewSlot->resource;
			}

			VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
			renderingInfo.renderArea.offset = { 0, 0 };
			renderingInfo.renderArea.extent = { passInfo.width, passInfo.height };
			commandListState->passRenderArea = renderingInfo.renderArea;
			renderingInfo.layerCount = 1;
			renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
			renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
			renderingInfo.pDepthAttachment = depthAttachmentPtr;
			renderingInfo.pStencilAttachment = stencilAttachmentPtr;

			vkCmdBeginRendering(commandListState->commandBuffer, &renderingInfo);

			VkViewport viewport{};
			viewport.width = static_cast<float>(passInfo.width);
			viewport.height = static_cast<float>(passInfo.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport(commandListState->commandBuffer, 0, 1, &viewport);

			VkRect2D scissor{};
			scissor.extent = { passInfo.width, passInfo.height };
			vkCmdSetScissor(commandListState->commandBuffer, 0, 1, &scissor);

			commandListState->passActive = true;
		}

		static void cl_endPass(CommandList* commandList) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->passActive || commandListState->commandBuffer == VK_NULL_HANDLE) {
				return;
			}

			vkCmdEndRendering(commandListState->commandBuffer);

			commandListState->passColorResources.clear();
			commandListState->passDepthResource = {};
			commandListState->passRenderArea = {};
			commandListState->passActive = false;
		}

		static void cl_barrier(CommandList* commandList, const BarrierBatch& barriers) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}

			std::vector<VkImageMemoryBarrier> imageBarriers;
			std::vector<VkBufferMemoryBarrier> bufferBarriers;
			std::vector<VkMemoryBarrier> memoryBarriers;
			struct TextureStateUpdate {
				ResourceHandle handle{};
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceLayout layout = ResourceLayout::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};
			struct BufferStateUpdate {
				ResourceHandle handle{};
				VulkanResource* resource = nullptr;
				ResourceAccessType access = ResourceAccessType::Common;
				ResourceSyncState sync = ResourceSyncState::All;
			};
			std::vector<TextureStateUpdate> textureUpdates;
			std::vector<BufferStateUpdate> bufferUpdates;
			VulkanCommandList::RecordedBarrierBatch recordedBatch{};
			if (impl->validateBarrierTransitions) {
				recordedBatch.textures.reserve(barriers.textures.size);
				recordedBatch.buffers.reserve(barriers.buffers.size);
			}
			VkPipelineStageFlags srcStages = 0;
			VkPipelineStageFlags dstStages = 0;

			auto getRecordingTextureState = [&](ResourceHandle texture, const VulkanResource& resource) -> VulkanCommandList::RecordingTextureState {
				for (const auto& state : commandListState->recordingTextureStates) {
					if (state.texture.index == texture.index && state.texture.generation == texture.generation) {
						return state;
					}
				}
				return VulkanCommandList::RecordingTextureState{ texture, resource.submittedAccess, resource.submittedLayout, resource.submittedSync };
			};
			auto upsertRecordingTextureState = [&](ResourceHandle texture, ResourceAccessType access, ResourceLayout layout, ResourceSyncState sync) {
				for (auto& state : commandListState->recordingTextureStates) {
					if (state.texture.index == texture.index && state.texture.generation == texture.generation) {
						state.access = access;
						state.layout = layout;
						state.sync = sync;
						return;
					}
				}
				commandListState->recordingTextureStates.push_back(VulkanCommandList::RecordingTextureState{ texture, access, layout, sync });
			};
			auto upsertRecordingBufferState = [&](ResourceHandle buffer, ResourceAccessType access, ResourceSyncState sync) {
				for (auto& state : commandListState->recordingBufferStates) {
					if (state.buffer.index == buffer.index && state.buffer.generation == buffer.generation) {
						state.access = access;
						state.sync = sync;
						return;
					}
				}
				commandListState->recordingBufferStates.push_back(VulkanCommandList::RecordingBufferState{ buffer, access, sync });
			};

			for (const TextureBarrier& barrier : barriers.textures) {
				VulkanResource* resource = VkResourceState(impl, barrier.texture);
				if (!resource || resource->image == VK_NULL_HANDLE) {
					continue;
				}
				const VkImageAspectFlags aspect = VkAspectMaskForFormat(resource->format);
				const auto recordingState = getRecordingTextureState(barrier.texture, *resource);
				const bool firstUseFromUndefined = !barrier.discard && recordingState.layout == ResourceLayout::Undefined && barrier.beforeLayout == ResourceLayout::Undefined;
				VkImageMemoryBarrier vkBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				const bool beforePresent = barrier.beforeLayout == ResourceLayout::Present;
				const bool afterPresent = barrier.afterLayout == ResourceLayout::Present;
				vkBarrier.srcAccessMask = (barrier.discard || firstUseFromUndefined || beforePresent) ? 0 : VkAccessMaskForAccess(barrier.beforeAccess);
				vkBarrier.dstAccessMask = afterPresent ? 0 : VkAccessMaskForAccess(barrier.afterAccess);
				vkBarrier.oldLayout = (barrier.discard || firstUseFromUndefined) ? VK_IMAGE_LAYOUT_UNDEFINED : VkToImageLayout(barrier.beforeLayout, aspect);
				vkBarrier.newLayout = VkToImageLayout(barrier.afterLayout, aspect);
				vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				vkBarrier.image = resource->image;
				vkBarrier.subresourceRange = VkMakeImageSubresourceRange(*resource, barrier.range, aspect);
				imageBarriers.push_back(vkBarrier);
				const bool beforeTransferClear = (barrier.beforeAccess & (ResourceAccessType::DepthStencilClear | ResourceAccessType::RenderTargetClear | ResourceAccessType::UnorderedAccessClear)) != 0;
				const bool afterTransferClear = (barrier.afterAccess & (ResourceAccessType::DepthStencilClear | ResourceAccessType::RenderTargetClear | ResourceAccessType::UnorderedAccessClear)) != 0;
				srcStages |= (barrier.discard || firstUseFromUndefined || beforePresent) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : (beforeTransferClear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VkStageMaskForSync(barrier.beforeSync));
				dstStages |= afterPresent ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : (afterTransferClear ? VK_PIPELINE_STAGE_TRANSFER_BIT : VkStageMaskForSync(barrier.afterSync));
				if (impl->validateBarrierTransitions) {
					recordedBatch.textures.push_back(VulkanCommandList::RecordedTextureBarrier{
						barrier.texture,
						barrier.beforeAccess,
						barrier.afterAccess,
						barrier.beforeLayout,
						barrier.afterLayout,
						barrier.beforeSync,
						barrier.afterSync,
						barrier.discard });
				}
				textureUpdates.push_back(TextureStateUpdate{ barrier.texture, resource, barrier.afterAccess, barrier.afterLayout, barrier.afterSync });
			}

			for (const BufferBarrier& barrier : barriers.buffers) {
				VulkanResource* resource = VkResourceState(impl, barrier.buffer);
				if (!resource || resource->buffer == VK_NULL_HANDLE) {
					continue;
				}
				VkBufferMemoryBarrier vkBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
				vkBarrier.srcAccessMask = barrier.discard ? 0 : VkAccessMaskForAccess(barrier.beforeAccess);
				vkBarrier.dstAccessMask = VkAccessMaskForAccess(barrier.afterAccess);
				vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				vkBarrier.buffer = resource->buffer;
				vkBarrier.offset = barrier.offset;
				vkBarrier.size = barrier.size == ~0ull ? VK_WHOLE_SIZE : barrier.size;
				bufferBarriers.push_back(vkBarrier);
				srcStages |= barrier.discard ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VkStageMaskForSync(barrier.beforeSync);
				dstStages |= VkStageMaskForSync(barrier.afterSync);
				if (impl->validateBarrierTransitions) {
					recordedBatch.buffers.push_back(VulkanCommandList::RecordedBufferBarrier{
						barrier.buffer,
						barrier.beforeAccess,
						barrier.afterAccess,
						barrier.beforeSync,
						barrier.afterSync,
						barrier.discard });
				}
				bufferUpdates.push_back(BufferStateUpdate{ barrier.buffer, resource, barrier.afterAccess, barrier.afterSync });
			}

			for (const GlobalBarrier& barrier : barriers.globals) {
				VkMemoryBarrier vkBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
				vkBarrier.srcAccessMask = VkAccessMaskForAccess(barrier.beforeAccess);
				vkBarrier.dstAccessMask = VkAccessMaskForAccess(barrier.afterAccess);
				memoryBarriers.push_back(vkBarrier);
				srcStages |= VkStageMaskForSync(barrier.beforeSync);
				dstStages |= VkStageMaskForSync(barrier.afterSync);
			}

			if (!imageBarriers.empty() || !bufferBarriers.empty() || !memoryBarriers.empty()) {
				vkCmdPipelineBarrier(commandListState->commandBuffer,
					srcStages != 0 ? srcStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					dstStages != 0 ? dstStages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
					0,
					static_cast<uint32_t>(memoryBarriers.size()), memoryBarriers.empty() ? nullptr : memoryBarriers.data(),
					static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.empty() ? nullptr : bufferBarriers.data(),
					static_cast<uint32_t>(imageBarriers.size()), imageBarriers.empty() ? nullptr : imageBarriers.data());
			}

			for (const TextureStateUpdate& update : textureUpdates) {
				upsertRecordingTextureState(update.handle, update.access, update.layout, update.sync);
			}
			for (const BufferStateUpdate& update : bufferUpdates) {
				upsertRecordingBufferState(update.handle, update.access, update.sync);
			}
			if (impl->validateBarrierTransitions && (!recordedBatch.textures.empty() || !recordedBatch.buffers.empty())) {
				commandListState->recordedBarrierBatches.push_back(std::move(recordedBatch));
			}
		}

		static void cl_bindLayout(CommandList* commandList, PipelineLayoutHandle layout) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState) {
				return;
			}

			commandListState->boundLayout = VkPipelineLayoutState(impl, layout) ? layout : PipelineLayoutHandle{};
		}

		static void cl_bindPipeline(CommandList* commandList, PipelineHandle pipeline) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE) {
				return;
			}

			VulkanPipeline* pipelineState = VkPipelineState(impl, pipeline);
			if (!pipelineState || pipelineState->pipeline == VK_NULL_HANDLE) {
				return;
			}

			vkCmdBindPipeline(commandListState->commandBuffer, pipelineState->bindPoint, pipelineState->pipeline);
			commandListState->boundPipeline = pipeline;
		}

		static void cl_setVB(CommandList* commandList, uint32_t startSlot, uint32_t numViews, VertexBufferView* views) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->isRecording || !views || numViews == 0) {
				return;
			}
			std::vector<VkBuffer> buffers(numViews, VK_NULL_HANDLE);
			std::vector<VkDeviceSize> offsets(numViews, 0);
			for (uint32_t i = 0; i < numViews; ++i) {
				VulkanResource* resource = VkResourceState(impl, views[i].buffer);
				if (resource) {
					buffers[i] = resource->buffer;
					offsets[i] = views[i].offset;
				}
			}
			vkCmdBindVertexBuffers(commandListState->commandBuffer, startSlot, numViews, buffers.data(), offsets.data());
		}

		static void cl_setIB(CommandList* commandList, const IndexBufferView& view) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* resource = VkResourceState(impl, view.buffer);
			if (!commandListState || !commandListState->isRecording || !resource || resource->buffer == VK_NULL_HANDLE) {
				return;
			}
			vkCmdBindIndexBuffer(commandListState->commandBuffer, resource->buffer, view.offset, VkIndexTypeForFormat(view.format));
		}

		static void cl_draw(CommandList* commandList, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (commandListState && commandListState->isRecording) {
				vkCmdDraw(commandListState->commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
			}
		}

		static void cl_drawIndexed(CommandList* commandList, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (commandListState && commandListState->isRecording) {
				vkCmdDrawIndexed(commandListState->commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
			}
		}

		static void cl_dispatch(CommandList* commandList, uint32_t x, uint32_t y, uint32_t z) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE || commandListState->passActive) {
				return;
			}

			VulkanPipeline* pipelineState = VkPipelineState(impl, commandListState->boundPipeline);
			if (!pipelineState || pipelineState->bindPoint != VK_PIPELINE_BIND_POINT_COMPUTE || pipelineState->pipeline == VK_NULL_HANDLE) {
				return;
			}

			vkCmdDispatch(commandListState->commandBuffer, x, y, z);
		}

		static void cl_clearRTV_slot(CommandList* commandList, DescriptorSlot slot, const ClearValue& clearValue) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::RTV);
			VulkanResource* resource = VkResourceState(impl, viewSlot ? viewSlot->resource : ResourceHandle{});
			if (!commandListState || !commandListState->isRecording || !viewSlot || !resource || resource->image == VK_NULL_HANDLE) {
				return;
			}
			VkClearColorValue value{};
			std::memcpy(value.float32, clearValue.rgba, sizeof(value.float32));
			if (commandListState->passActive) {
				for (uint32_t index = 0; index < commandListState->passColorResources.size(); ++index) {
					if (commandListState->passColorResources[index].index == viewSlot->resource.index &&
						commandListState->passColorResources[index].generation == viewSlot->resource.generation) {
						VkClearAttachment attachment{};
						attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
						attachment.colorAttachment = index;
						attachment.clearValue.color = value;
						VkClearRect rect{};
						rect.rect = commandListState->passRenderArea;
						rect.baseArrayLayer = 0;
						rect.layerCount = 1;
						vkCmdClearAttachments(commandListState->commandBuffer, 1, &attachment, 1, &rect);
						return;
					}
				}
				cl_endPass(commandList);
			}
			VkImageSubresourceRange range = VkMakeImageSubresourceRange(*resource, viewSlot->range, viewSlot->aspectMask);
			vkCmdClearColorImage(commandListState->commandBuffer, resource->image, VkToImageLayout(ResourceLayout::RenderTargetClear, viewSlot->aspectMask), &value, 1, &range);
		}

		static void cl_clearDSV_slot(CommandList* commandList, DescriptorSlot slot, bool clearDepth, bool clearStencil, float depth, uint8_t stencil) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::DSV);
			VulkanResource* resource = VkResourceState(impl, viewSlot ? viewSlot->resource : ResourceHandle{});
			if (!commandListState || !commandListState->isRecording || !viewSlot || !resource || resource->image == VK_NULL_HANDLE) {
				return;
			}
			VkClearDepthStencilValue value{ depth, stencil };
			VkImageSubresourceRange range = VkMakeImageSubresourceRange(*resource, viewSlot->range, 0);
			if (clearDepth) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
			if (clearStencil) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			if (commandListState->passActive) {
				if (commandListState->passDepthResource.index == viewSlot->resource.index &&
					commandListState->passDepthResource.generation == viewSlot->resource.generation &&
					range.aspectMask != 0) {
					VkClearAttachment attachment{};
					attachment.aspectMask = range.aspectMask & viewSlot->aspectMask;
					attachment.clearValue.depthStencil = value;
					if (attachment.aspectMask != 0) {
						VkClearRect rect{};
						rect.rect = commandListState->passRenderArea;
						rect.baseArrayLayer = 0;
						rect.layerCount = 1;
						vkCmdClearAttachments(commandListState->commandBuffer, 1, &attachment, 1, &rect);
					}
					return;
				}
				cl_endPass(commandList);
			}
			if (range.aspectMask != 0) {
				vkCmdClearDepthStencilImage(commandListState->commandBuffer, resource->image, VkToImageLayout(ResourceLayout::DepthStencilClear, viewSlot->aspectMask), &value, 1, &range);
			}
		}

		static void cl_executeIndirect(CommandList* commandList,
			CommandSignatureHandle signature,
			ResourceHandle argumentBuffer,
			uint64_t argumentOffset,
			ResourceHandle countBuffer,
			uint64_t countOffset,
			uint32_t maxCommandCount) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanCommandSignature* signatureState = VkCommandSignatureState(impl, signature);
			VulkanResource* argumentResource = VkResourceState(impl, argumentBuffer);
			VulkanResource* countResource = VkResourceState(impl, countBuffer);
			if (!commandListState || !signatureState || signatureState->args.empty() || !argumentResource || argumentResource->buffer == VK_NULL_HANDLE || maxCommandCount == 0) {
				return;
			}

			const uint32_t stride = signatureState->byteStride;
			if (signatureState->indirectLayout != VK_NULL_HANDLE) {
				VulkanPipeline* pipelineState = VkPipelineState(impl, commandListState->boundPipeline);
				if (!impl || !pipelineState || pipelineState->pipeline == VK_NULL_HANDLE || argumentResource->deviceAddress == 0 || !vkCmdExecuteGeneratedCommandsEXT) {
					return;
				}

				if (signatureState->usesExecutionSet &&
					VkEnsureGeneratedCommandsExecutionSet(impl, *signatureState, pipelineState->pipeline) != Result::Ok) {
					return;
				}
				if (VkEnsureGeneratedCommandsPreprocessBuffer(impl, *signatureState, pipelineState->pipeline, maxCommandCount) != Result::Ok) {
					return;
				}

				VkGeneratedCommandsInfoEXT generatedInfo{ VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT };
				VkGeneratedCommandsPipelineInfoEXT pipelineInfo{ VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT };
				if (!signatureState->usesExecutionSet) {
					pipelineInfo.pipeline = pipelineState->pipeline;
					generatedInfo.pNext = &pipelineInfo;
				}
				generatedInfo.shaderStages = pipelineState->bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;
				generatedInfo.indirectExecutionSet = signatureState->usesExecutionSet ? signatureState->executionSet : VK_NULL_HANDLE;
				generatedInfo.indirectCommandsLayout = signatureState->indirectLayout;
				generatedInfo.indirectAddress = argumentResource->deviceAddress + argumentOffset;
				generatedInfo.indirectAddressSize = static_cast<VkDeviceSize>(stride) * maxCommandCount;
				generatedInfo.preprocessAddress = signatureState->preprocessAddress;
				generatedInfo.preprocessSize = signatureState->preprocessSize;
				generatedInfo.maxSequenceCount = maxCommandCount;
				generatedInfo.sequenceCountAddress = countResource && countResource->deviceAddress != 0 ? countResource->deviceAddress + countOffset : 0;
				generatedInfo.maxDrawCount = maxCommandCount;
				vkCmdExecuteGeneratedCommandsEXT(commandListState->commandBuffer, VK_FALSE, &generatedInfo);
				return;
			}

			const IndirectArg* executableArg = nullptr;
			for (const IndirectArg& arg : signatureState->args) {
				if (arg.kind != IndirectArgKind::Constant) {
					executableArg = &arg;
					break;
				}
			}
			if (!executableArg) {
				return;
			}

			switch (executableArg->kind) {
			case IndirectArgKind::Draw:
				if (countResource && countResource->buffer != VK_NULL_HANDLE && vkCmdDrawIndirectCount) {
					vkCmdDrawIndirectCount(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, countResource->buffer, countOffset, maxCommandCount, stride);
				}
				else {
					vkCmdDrawIndirect(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, maxCommandCount, stride);
				}
				break;
			case IndirectArgKind::DrawIndexed:
				if (countResource && countResource->buffer != VK_NULL_HANDLE && vkCmdDrawIndexedIndirectCount) {
					vkCmdDrawIndexedIndirectCount(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, countResource->buffer, countOffset, maxCommandCount, stride);
				}
				else {
					vkCmdDrawIndexedIndirect(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, maxCommandCount, stride);
				}
				break;
			case IndirectArgKind::Dispatch:
				vkCmdDispatchIndirect(commandListState->commandBuffer, argumentResource->buffer, argumentOffset);
				break;
			case IndirectArgKind::DispatchMesh:
				if (impl && impl->meshShaderEnabled && vkCmdDrawMeshTasksIndirectEXT) {
					if (countResource && countResource->buffer != VK_NULL_HANDLE && vkCmdDrawMeshTasksIndirectCountEXT) {
						vkCmdDrawMeshTasksIndirectCountEXT(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, countResource->buffer, countOffset, maxCommandCount, stride);
					}
					else {
						vkCmdDrawMeshTasksIndirectEXT(commandListState->commandBuffer, argumentResource->buffer, argumentOffset, maxCommandCount, stride);
					}
				}
				break;
			default:
				break;
			}
		}

		static void cl_setDescriptorHeaps(CommandList* commandList, DescriptorHeapHandle cbvSrvUav, std::optional<DescriptorHeapHandle> sampler) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState) {
				return;
			}

			if (VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, cbvSrvUav);
				heap && heap->type == DescriptorHeapType::CbvSrvUav && heap->shaderVisible) {
				commandListState->boundCbvSrvUavHeap = cbvSrvUav;
				if (impl->descriptorHeapEnabled && heap->buffer != VK_NULL_HANDLE && heap->deviceAddress != 0) {
					VkBindHeapInfoEXT bindInfo{ VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT };
					bindInfo.heapRange.address = heap->deviceAddress;
					bindInfo.heapRange.size = heap->descriptorBytes + heap->reservedRangeSize;
					bindInfo.reservedRangeOffset = heap->reservedRangeOffset;
					bindInfo.reservedRangeSize = heap->reservedRangeSize;
					vkCmdBindResourceHeapEXT(commandListState->commandBuffer, &bindInfo);
				}
			}
			else {
				commandListState->boundCbvSrvUavHeap = {};
			}

			if (sampler.has_value()) {
				if (VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, sampler.value());
					heap && heap->type == DescriptorHeapType::Sampler && heap->shaderVisible) {
					commandListState->boundSamplerHeap = sampler.value();
					if (impl->descriptorHeapEnabled && heap->buffer != VK_NULL_HANDLE && heap->deviceAddress != 0) {
						VkBindHeapInfoEXT bindInfo{ VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT };
						bindInfo.heapRange.address = heap->deviceAddress;
						bindInfo.heapRange.size = heap->descriptorBytes + heap->reservedRangeSize;
						bindInfo.reservedRangeOffset = heap->reservedRangeOffset;
						bindInfo.reservedRangeSize = heap->reservedRangeSize;
						vkCmdBindSamplerHeapEXT(commandListState->commandBuffer, &bindInfo);
					}
				}
				else {
					commandListState->boundSamplerHeap = {};
				}
			}
			else {
				commandListState->boundSamplerHeap = {};
			}
		}

		static void cl_clearUavUint(CommandList* commandList, const UavClearInfo& clearInfo, const UavClearUint& value) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* resource = VkResourceState(impl, clearInfo.resource.GetHandle());
			if (!commandListState || !resource) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}
			if (clearInfo.resource.IsTexture() && resource->image != VK_NULL_HANDLE) {
				VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, clearInfo.cpuVisible, DescriptorHeapType::CbvSrvUav);
				if (!viewSlot) {
					viewSlot = VkImageViewSlotState(impl, clearInfo.shaderVisible, DescriptorHeapType::CbvSrvUav);
				}
				VkClearColorValue clearValue{};
				std::memcpy(clearValue.uint32, value.v, sizeof(clearValue.uint32));
				VkImageSubresourceRange range = viewSlot
					? VkMakeImageSubresourceRange(*resource, viewSlot->range, viewSlot->aspectMask)
					: VkMakeImageSubresourceRange(*resource, {}, VkAspectMaskForFormat(resource->format));
				vkCmdClearColorImage(commandListState->commandBuffer, resource->image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &range);
			}
			else if (resource->buffer != VK_NULL_HANDLE) {
				vkCmdFillBuffer(commandListState->commandBuffer, resource->buffer, 0, VK_WHOLE_SIZE, value.v[0]);
			}
		}

		static void cl_clearUavFloat(CommandList* commandList, const UavClearInfo& clearInfo, const UavClearFloat& value) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* resource = VkResourceState(impl, clearInfo.resource.GetHandle());
			if (!commandListState || !resource) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}
			if (clearInfo.resource.IsTexture() && resource->image != VK_NULL_HANDLE) {
				VulkanImageViewSlot* viewSlot = VkImageViewSlotState(impl, clearInfo.cpuVisible, DescriptorHeapType::CbvSrvUav);
				if (!viewSlot) {
					viewSlot = VkImageViewSlotState(impl, clearInfo.shaderVisible, DescriptorHeapType::CbvSrvUav);
				}
				VkClearColorValue clearValue{};
				std::memcpy(clearValue.float32, value.v, sizeof(clearValue.float32));
				VkImageSubresourceRange range = viewSlot
					? VkMakeImageSubresourceRange(*resource, viewSlot->range, viewSlot->aspectMask)
					: VkMakeImageSubresourceRange(*resource, {}, VkAspectMaskForFormat(resource->format));
				vkCmdClearColorImage(commandListState->commandBuffer, resource->image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &range);
			}
		}

		static void cl_copyTextureToBuffer(CommandList* commandList, const BufferTextureCopyFootprint& footprint) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* texture = VkResourceState(impl, footprint.texture);
			VulkanResource* buffer = VkResourceState(impl, footprint.buffer);
			VkBufferImageCopy region{};
			if (!commandListState || !texture || !buffer || !VkBuildBufferImageCopy(*texture, footprint, region)) {
				return;
			}
			vkCmdCopyImageToBuffer(commandListState->commandBuffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
		}

		static void cl_copyBufferToTexture(CommandList* commandList, const BufferTextureCopyFootprint& footprint) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* texture = VkResourceState(impl, footprint.texture);
			VulkanResource* buffer = VkResourceState(impl, footprint.buffer);
			VkBufferImageCopy region{};
			if (!commandListState || !texture || !buffer || !VkBuildBufferImageCopy(*texture, footprint, region)) {
				return;
			}
			vkCmdCopyBufferToImage(commandListState->commandBuffer, buffer->buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		static void cl_copyTextureRegion(CommandList* commandList, const TextureCopyRegion& dst, const TextureCopyRegion& src) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* dstTexture = VkResourceState(impl, dst.texture);
			VulkanResource* srcTexture = VkResourceState(impl, src.texture);
			if (!commandListState || !dstTexture || !srcTexture || dstTexture->image == VK_NULL_HANDLE || srcTexture->image == VK_NULL_HANDLE) {
				return;
			}
			VkImageCopy region{};
			region.srcSubresource.aspectMask = VkAspectMaskForFormat(srcTexture->format);
			region.srcSubresource.mipLevel = src.mip;
			region.srcSubresource.baseArrayLayer = srcTexture->type == ResourceType::Texture3D ? 0u : src.arraySlice;
			region.srcSubresource.layerCount = 1;
			region.srcOffset = { static_cast<int32_t>(src.x), static_cast<int32_t>(src.y), static_cast<int32_t>(src.z) };
			region.dstSubresource.aspectMask = VkAspectMaskForFormat(dstTexture->format);
			region.dstSubresource.mipLevel = dst.mip;
			region.dstSubresource.baseArrayLayer = dstTexture->type == ResourceType::Texture3D ? 0u : dst.arraySlice;
			region.dstSubresource.layerCount = 1;
			region.dstOffset = { static_cast<int32_t>(dst.x), static_cast<int32_t>(dst.y), static_cast<int32_t>(dst.z) };
			region.extent.width = src.width ? src.width : VkMipDim(srcTexture->width, src.mip);
			region.extent.height = src.height ? src.height : VkMipDim(srcTexture->height, src.mip);
			region.extent.depth = src.depth ? src.depth : (srcTexture->type == ResourceType::Texture3D ? VkMipDim(srcTexture->depthOrLayers, src.mip) : 1u);
			vkCmdCopyImage(commandListState->commandBuffer, srcTexture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTexture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		static void cl_copyBufferRegion(CommandList* commandList, ResourceHandle dst, uint64_t dstOffset, ResourceHandle src, uint64_t srcOffset, uint64_t numBytes) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanResource* dstBuffer = VkResourceState(impl, dst);
			VulkanResource* srcBuffer = VkResourceState(impl, src);
			if (!commandListState || !dstBuffer || !srcBuffer || numBytes == 0) {
				return;
			}
			VkBufferCopy region{ srcOffset, dstOffset, numBytes };
			vkCmdCopyBuffer(commandListState->commandBuffer, srcBuffer->buffer, dstBuffer->buffer, 1, &region);
		}

		static void cl_writeTimestamp(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index, Stage stageHint) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanQueryPool* queryPoolState = VkQueryPoolState(impl, queryPool);
			if (!commandListState || !queryPoolState || queryPoolState->type != QueryType::Timestamp || index >= queryPoolState->count) {
				return;
			}
			vkCmdWriteTimestamp(commandListState->commandBuffer, VkPipelineStageForStage(stageHint), queryPoolState->pool, index);
		}

		static void cl_beginQuery(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanQueryPool* queryPoolState = VkQueryPoolState(impl, queryPool);
			if (!commandListState || !queryPoolState || index >= queryPoolState->count || queryPoolState->type == QueryType::Timestamp) {
				return;
			}
			vkCmdBeginQuery(commandListState->commandBuffer, queryPoolState->pool, index, 0);
		}

		static void cl_endQuery(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanQueryPool* queryPoolState = VkQueryPoolState(impl, queryPool);
			if (!commandListState || !queryPoolState || index >= queryPoolState->count || queryPoolState->type == QueryType::Timestamp) {
				return;
			}
			vkCmdEndQuery(commandListState->commandBuffer, queryPoolState->pool, index);
		}

		static void cl_resolveQueryData(CommandList* commandList, QueryPoolHandle queryPool, uint32_t firstQuery, uint32_t queryCount, ResourceHandle dstBuffer, uint64_t dstOffsetBytes) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanQueryPool* queryPoolState = VkQueryPoolState(impl, queryPool);
			VulkanResource* dst = VkResourceState(impl, dstBuffer);
			if (!commandListState || !queryPoolState || !dst || dst->buffer == VK_NULL_HANDLE || queryCount == 0 || firstQuery + queryCount > queryPoolState->count) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}
			VkDeviceSize stride = sizeof(uint64_t);
			if (queryPoolState->type == QueryType::PipelineStatistics) {
				stride = (std::max)(1u, VkPipelineStatsFieldCount(queryPoolState->vkStats)) * sizeof(uint64_t);
			}
			vkCmdCopyQueryPoolResults(commandListState->commandBuffer,
				queryPoolState->pool,
				firstQuery,
				queryCount,
				dst->buffer,
				dstOffsetBytes,
				stride,
				VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
		}

		static void cl_resetQueries(CommandList* commandList, QueryPoolHandle queryPool, uint32_t firstQuery, uint32_t queryCount) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			VulkanQueryPool* queryPoolState = VkQueryPoolState(impl, queryPool);
			if (!commandListState || !queryPoolState || queryCount == 0 || firstQuery + queryCount > queryPoolState->count) {
				return;
			}
			if (commandListState->passActive) {
				cl_endPass(commandList);
			}
			vkCmdResetQueryPool(commandListState->commandBuffer, queryPoolState->pool, firstQuery, queryCount);
		}

		static void cl_pushConstants(CommandList* commandList,
			ShaderStage stages,
			uint32_t set,
			uint32_t binding,
			uint32_t dstOffset32,
			uint32_t num32,
			const void* data) noexcept {
			VkIgnoreUnused(stages);
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE || num32 == 0 || !data || !impl->descriptorHeapEnabled) {
				return;
			}

			VulkanPipelineLayout* layoutState = VkPipelineLayoutState(impl, commandListState->boundLayout);
			if (!layoutState) {
				return;
			}

			const VulkanPushConstantRange* pushRange = nullptr;
			for (const VulkanPushConstantRange& candidate : layoutState->pushConstantRanges) {
				if (candidate.desc.set == set && candidate.desc.binding == binding) {
					pushRange = &candidate;
					break;
				}
			}
			if (!pushRange) {
				return;
			}

			const uint32_t dataByteOffset = dstOffset32 * 4u;
			const uint32_t dataByteSize = num32 * 4u;
			if (dataByteOffset + dataByteSize > pushRange->dataByteSize) {
				return;
			}

			uint32_t pushByteOffset = pushRange->byteOffset;
			uint32_t pushByteSize = pushRange->byteSize;
			const void* pushData = data;

			VkDeviceAddress emulatedDeviceAddress = 0;
			if (pushRange->desc.type == PushConstantRangeType::EmulatedRootConstants) {
				auto* shadowState = VkGetEmulatedRootConstantShadowState(*commandListState, *pushRange);
				if (!shadowState || shadowState->values.size() != pushRange->desc.num32BitValues) {
					return;
				}

				std::memcpy(
					shadowState->values.data() + dstOffset32,
					data,
					static_cast<size_t>(dataByteSize));

				void* mappedScratch = nullptr;
				if (!VkAllocateEmulatedRootConstantScratch(
					impl,
					*commandListState,
					pushRange->dataByteSize,
					emulatedDeviceAddress,
					mappedScratch) || mappedScratch == nullptr || emulatedDeviceAddress == 0) {
					return;
				}

				std::memcpy(mappedScratch, shadowState->values.data(), pushRange->dataByteSize);
				pushData = &emulatedDeviceAddress;
			}

			if (pushByteOffset + pushByteSize > layoutState->totalPushDataBytes ||
				pushByteOffset + pushByteSize > impl->descriptorHeapProperties.maxPushDataSize) {
				return;
			}

			VkPushDataInfoEXT pushInfo{ VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT };
			pushInfo.offset = pushByteOffset;
			pushInfo.data.address = pushData;
			pushInfo.data.size = pushByteSize;
			vkCmdPushDataEXT(commandListState->commandBuffer, &pushInfo);
		}

		static void cl_setPrimitiveTopology(CommandList* commandList, PrimitiveTopology topology) noexcept {
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (commandListState && commandListState->isRecording && vkCmdSetPrimitiveTopology) {
				vkCmdSetPrimitiveTopology(commandListState->commandBuffer, VkPrimitiveTopologyForRHI(topology));
			}
		}

		static void cl_dispatchMesh(CommandList* commandList, uint32_t x, uint32_t y, uint32_t z) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !impl->meshShaderEnabled || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE || !vkCmdDrawMeshTasksEXT) {
				return;
			}
			vkCmdDrawMeshTasksEXT(commandListState->commandBuffer, x, y, z);
		}

		static void cl_setWorkGraph(CommandList* commandList, const WorkGraphHandle& workGraph, const ResourceHandle& backingMemory, bool resetBackingMemory) noexcept {
			VkIgnoreUnused(commandList, workGraph, backingMemory, resetBackingMemory);
		}

		static void cl_dispatchWorkGraph(CommandList* commandList, const WorkGraphDispatchDesc& desc) noexcept {
			VkIgnoreUnused(commandList, desc);
		}

		static void cl_setName(CommandList* commandList, const char* name) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (commandListState) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(commandListState->commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, name);
			}
		}

		static void cl_setDebugInstrumentationContext(CommandList* commandList, const char* passName, const char* techniquePath) noexcept {
			VkIgnoreUnused(commandList, passName, techniquePath);
		}

		static void ca_reset(CommandAllocator* allocator) noexcept {
			auto* impl = allocator ? static_cast<VulkanDevice*>(allocator->impl) : nullptr;
			VulkanCommandAllocator* allocatorState = VkAllocatorState(impl, allocator ? allocator->GetHandle() : CommandAllocatorHandle{});
			if (!impl || impl->device == VK_NULL_HANDLE || !allocatorState || allocatorState->pool == VK_NULL_HANDLE) {
				return;
			}

			const VkResult result = vkResetCommandPool(impl->device, allocatorState->pool, 0);
			if (result != VK_SUCCESS) {
				spdlog::error("Vulkan command allocator reset failed with VkResult {}", static_cast<int>(result));
			}
		}

		static uint32_t sc_count(Swapchain* swapchain) noexcept {
			if (VulkanSwapchain* swapchainState = VkSwapchainState(swapchain)) {
				return swapchainState->imageCount;
			}
			return 0;
		}

		static uint32_t sc_curr(Swapchain* swapchain) noexcept {
			if (VulkanSwapchain* swapchainState = VkSwapchainState(swapchain)) {
				return swapchainState->currentImageIndex;
			}
			return 0;
		}

		static ResourceHandle sc_img(Swapchain* swapchain, uint32_t imageIndex) noexcept {
			if (VulkanSwapchain* swapchainState = VkSwapchainState(swapchain);
				swapchainState && imageIndex < swapchainState->imageHandles.size()) {
				return swapchainState->imageHandles[imageIndex];
			}
			return {};
		}

		static Result sc_present(Swapchain* swapchain, bool vsync, const PresentSyncDesc* sync) noexcept {
			VkIgnoreUnused(vsync);
			auto* impl = swapchain ? static_cast<VulkanDevice*>(swapchain->impl) : nullptr;
			VulkanSwapchain* swapchainState = VkSwapchainState(swapchain);
			if (!impl || !swapchainState || swapchainState->swapchain == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}
			if (swapchainState->currentImageIndex >= swapchainState->presentWaitSemaphores.size()) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VkSemaphore presentWaitSemaphore = swapchainState->presentWaitSemaphores[swapchainState->currentImageIndex];
			if (presentWaitSemaphore == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			VulkanQueueState* signalQueueState = nullptr;
			VulkanTimeline* waitTimeline = nullptr;
			if (sync && sync->queue && sync->wait.value != 0) {
				signalQueueState = VkQueueStateForHandle(impl, sync->queue.GetQueueHandle());
				waitTimeline = VkTimelineState(impl, sync->wait.t);
				if (!signalQueueState || signalQueueState->queue == VK_NULL_HANDLE || !waitTimeline || waitTimeline->semaphore == VK_NULL_HANDLE) {
					RHI_FAIL(Result::InvalidArgument);
				}
			} else {
				signalQueueState = &impl->queues[0];
			}

			VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
			VkSemaphore waitSemaphore = VK_NULL_HANDLE;
			uint64_t waitValue = 0;
			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkSubmitInfo signalPresentInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			if (waitTimeline) {
				waitSemaphore = waitTimeline->semaphore;
				waitValue = sync->wait.value;
				timelineInfo.waitSemaphoreValueCount = 1;
				timelineInfo.pWaitSemaphoreValues = &waitValue;
				signalPresentInfo.pNext = &timelineInfo;
				signalPresentInfo.waitSemaphoreCount = 1;
				signalPresentInfo.pWaitSemaphores = &waitSemaphore;
				signalPresentInfo.pWaitDstStageMask = &waitStage;
			}
			signalPresentInfo.signalSemaphoreCount = 1;
			signalPresentInfo.pSignalSemaphores = &presentWaitSemaphore;
			VkResult signalResult = vkQueueSubmit(signalQueueState->queue, 1, &signalPresentInfo, VK_NULL_HANDLE);
			if (signalResult != VK_SUCCESS) {
				return ToRHI(signalResult);
			}

			VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &presentWaitSemaphore;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &swapchainState->swapchain;
			presentInfo.pImageIndices = &swapchainState->currentImageIndex;

			const VkResult presentResult = vkQueuePresentKHR(impl->queues[0].queue, &presentInfo);
			if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
				return ToRHI(presentResult);
			}

			const Result acquireResult = VkAcquireNextSwapchainImage(impl, *swapchainState);
			if (presentResult == VK_SUBOPTIMAL_KHR && acquireResult == Result::Ok) {
				RHI_FAIL(Result::ModeChanged);
			}
			return acquireResult;
		}

		static Result sc_resizeBuffers(Swapchain* swapchain, uint32_t bufferCount, uint32_t width, uint32_t height, Format newFormat, uint32_t flags) noexcept {
			VkIgnoreUnused(flags);
			auto* impl = swapchain ? static_cast<VulkanDevice*>(swapchain->impl) : nullptr;
			VulkanSwapchain* swapchainState = VkSwapchainState(swapchain);
			if (!impl || !swapchainState) {
				RHI_FAIL(Result::InvalidArgument);
			}

			const Result idleResult = ToRHI(vkDeviceWaitIdle(impl->device));
			if (idleResult != Result::Ok) {
				return idleResult;
			}

			const Format targetFormat = newFormat != Format::Unknown ? newFormat : swapchainState->rhiFormat;
			const uint32_t targetWidth = width != 0 ? width : swapchainState->width;
			const uint32_t targetHeight = height != 0 ? height : swapchainState->height;
			const uint32_t targetBufferCount = bufferCount != 0 ? bufferCount : swapchainState->imageCount;
			return VkCreateOrResizeSwapchain(impl, *swapchainState, targetWidth, targetHeight, targetFormat, targetBufferCount, swapchainState->allowTearing);
		}

		static void sc_setName(Swapchain* swapchain, const char* name) noexcept {
			VkIgnoreUnused(swapchain, name);
		}

		static Result d_createPipelineFromStream(Device* device, const PipelineStreamItem* items, uint32_t count, PipelinePtr& out) noexcept {
			out.Reset();
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || !items || count == 0 || impl->device == VK_NULL_HANDLE) {
				RHI_FAIL(Result::InvalidArgument);
			}

			const SubobjShader* computeShader = nullptr;
			const SubobjShader* vertexShader = nullptr;
			const SubobjShader* pixelShader = nullptr;
			const SubobjShader* taskShader = nullptr;
			const SubobjShader* meshShader = nullptr;
			const VulkanPipelineLayout* layoutState = nullptr;
			PipelineLayoutHandle layoutHandle{};
			RasterState rasterState{};
			BlendState blendState{};
			DepthStencilState depthState{};
			RenderTargets renderTargets{};
			Format depthFormat = Format::Unknown;
			SampleDesc sampleDesc{};
			FinalizedInputLayout inputLayout{};
			PrimitiveTopology primitiveTopology = PrimitiveTopology::TriangleList;
			bool sawGraphicsState = false;

			for (uint32_t index = 0; index < count; ++index) {
				switch (items[index].type) {
				case PsoSubobj::Layout: {
					const auto& layout = *static_cast<const SubobjLayout*>(items[index].data);
					layoutHandle = layout.layout;
					layoutState = VkPipelineLayoutState(impl, layout.layout);
					if (!layoutState) {
						spdlog::error("Vulkan pipeline creation: invalid pipeline layout handle");
						RHI_FAIL(Result::InvalidArgument);
					}
					break;
				}
				case PsoSubobj::Shader: {
					const auto& shader = *static_cast<const SubobjShader*>(items[index].data);
					switch (shader.stage) {
					case ShaderStage::Compute:
						if (computeShader) {
							spdlog::error("Vulkan pipeline creation: multiple compute shader stages are not supported");
							RHI_FAIL(Result::InvalidArgument);
						}
						computeShader = &shader;
						break;
					case ShaderStage::Vertex:
						vertexShader = &shader;
						sawGraphicsState = true;
						break;
					case ShaderStage::Pixel:
						pixelShader = &shader;
						sawGraphicsState = true;
						break;
					case ShaderStage::Task:
						if (taskShader) {
							spdlog::error("Vulkan pipeline creation: multiple task shader stages are not supported");
							RHI_FAIL(Result::InvalidArgument);
						}
						taskShader = &shader;
						sawGraphicsState = true;
						break;
					case ShaderStage::Mesh:
						if (meshShader) {
							spdlog::error("Vulkan pipeline creation: multiple mesh shader stages are not supported");
							RHI_FAIL(Result::InvalidArgument);
						}
						meshShader = &shader;
						sawGraphicsState = true;
						break;
					default:
						spdlog::error("Vulkan pipeline creation: unsupported shader stage {}", static_cast<uint32_t>(shader.stage));
						RHI_FAIL(Result::InvalidArgument);
					}
					break;
				}
				case PsoSubobj::Rasterizer:
					rasterState = static_cast<const SubobjRaster*>(items[index].data)->rs;
					sawGraphicsState = true;
					break;
				case PsoSubobj::Blend:
					blendState = static_cast<const SubobjBlend*>(items[index].data)->bs;
					sawGraphicsState = true;
					break;
				case PsoSubobj::DepthStencil:
					depthState = static_cast<const SubobjDepth*>(items[index].data)->ds;
					sawGraphicsState = true;
					break;
				case PsoSubobj::RTVFormats:
					renderTargets = static_cast<const SubobjRTVs*>(items[index].data)->rt;
					sawGraphicsState = true;
					break;
				case PsoSubobj::DSVFormat:
					depthFormat = static_cast<const SubobjDSV*>(items[index].data)->dsv;
					sawGraphicsState = true;
					break;
				case PsoSubobj::Sample:
					sampleDesc = static_cast<const SubobjSample*>(items[index].data)->sd;
					sawGraphicsState = true;
					break;
				case PsoSubobj::InputLayout:
					inputLayout = static_cast<const SubobjInputLayout*>(items[index].data)->il;
					sawGraphicsState = true;
					break;
				case PsoSubobj::PrimitiveTopology:
					primitiveTopology = static_cast<const SubobjPrimitiveTopology*>(items[index].data)->pt;
					sawGraphicsState = true;
					break;
				case PsoSubobj::Flags:
				default:
					break;
				}
			}

			if (sawGraphicsState) {
				const bool hasMeshShader = meshShader != nullptr;
				if (computeShader || !layoutState) {
					RHI_FAIL(Result::InvalidArgument);
				}
				if (hasMeshShader) {
					if (!impl->meshShaderEnabled || vertexShader || !VkIsSpirvBytecode(meshShader->bytecode) || (taskShader && (!impl->taskShaderEnabled || !VkIsSpirvBytecode(taskShader->bytecode)))) {
						RHI_FAIL(Result::Unsupported);
					}
				}
				else if (!vertexShader || taskShader) {
					RHI_FAIL(Result::InvalidArgument);
				}
				if (!impl->dynamicRenderingEnabled) {
					RHI_FAIL(Result::Unsupported);
				}
				if (!layoutState->usesDescriptorHeap || !impl->descriptorHeapEnabled) {
					RHI_FAIL(Result::Unsupported);
				}
				if ((!hasMeshShader && !VkIsSpirvBytecode(vertexShader->bytecode)) || (pixelShader && !VkIsSpirvBytecode(pixelShader->bytecode))) {
					spdlog::warn("Vulkan graphics pipeline creation: shader bytecode was not SPIR-V");
					RHI_FAIL(Result::Unsupported);
				}

				std::vector<VkShaderModule> modules;
				std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
				std::vector<VkDescriptorSetAndBindingMappingEXT> descriptorMappings;
				std::vector<VkSamplerCreateInfo> embeddedSamplers;
				VkAppendDescriptorHeapMappings(impl, *layoutState, descriptorMappings, embeddedSamplers);
				VkShaderDescriptorSetAndBindingMappingInfoEXT shaderMappingInfo{ VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT };
				shaderMappingInfo.mappingCount = static_cast<uint32_t>(descriptorMappings.size());
				shaderMappingInfo.pMappings = descriptorMappings.empty() ? nullptr : descriptorMappings.data();
				auto addShader = [&](const SubobjShader* shader) -> Result {
					VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
					moduleInfo.codeSize = shader->bytecode.size;
					moduleInfo.pCode = static_cast<const uint32_t*>(shader->bytecode.data);
					VkShaderModule module = VK_NULL_HANDLE;
					VkResult result = vkCreateShaderModule(impl->device, &moduleInfo, nullptr, &module);
					if (result != VK_SUCCESS) {
						return ToRHI(result);
					}
					modules.push_back(module);
					VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
					stage.stage = VkShaderStageForRHI(shader->stage);
					stage.module = module;
					stage.pName = shader->entryPoint.empty() ? "main" : shader->entryPoint.c_str();
					stage.pNext = shaderMappingInfo.mappingCount != 0 ? &shaderMappingInfo : nullptr;
					shaderStages.push_back(stage);
					return Result::Ok;
				};
				Result addResult = Result::Ok;
				if (taskShader) {
					addResult = addShader(taskShader);
				}
				if (addResult == Result::Ok) {
					addResult = addShader(hasMeshShader ? meshShader : vertexShader);
				}
				if (addResult == Result::Ok && pixelShader) {
					addResult = addShader(pixelShader);
				}
				if (addResult != Result::Ok) {
					for (VkShaderModule module : modules) vkDestroyShaderModule(impl->device, module, nullptr);
					return addResult;
				}

				VkPipelineLayout nativeLayout = VK_NULL_HANDLE;

				std::vector<VkVertexInputBindingDescription> bindings;
				bindings.reserve(inputLayout.bindings.size());
				for (const InputBindingDesc& binding : inputLayout.bindings) {
					bindings.push_back({ binding.binding, binding.stride, binding.rate == InputRate::PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
				}
				std::vector<VkVertexInputAttributeDescription> attributes;
				attributes.reserve(inputLayout.attributes.size());
				for (const InputAttributeDesc& attribute : inputLayout.attributes) {
					const VkFormat format = ToVkFormat(attribute.format);
					if (format == VK_FORMAT_UNDEFINED) {
						for (VkShaderModule module : modules) vkDestroyShaderModule(impl->device, module, nullptr);
						vkDestroyPipelineLayout(impl->device, nativeLayout, nullptr);
						RHI_FAIL(Result::Unsupported);
					}
					attributes.push_back({ attribute.location, attribute.binding, format, attribute.offset });
				}

				VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
				vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
				vertexInput.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
				vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
				vertexInput.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();
				VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
				inputAssembly.topology = VkPrimitiveTopologyForRHI(primitiveTopology);
				VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
				viewportState.viewportCount = 1;
				viewportState.scissorCount = 1;
				VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
				raster.polygonMode = VkPolygonModeForRHI(rasterState.fill);
				raster.cullMode = VkCullModeForRHI(rasterState.cull);
				raster.frontFace = rasterState.frontCCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
				raster.lineWidth = 1.0f;
				raster.depthBiasEnable = (rasterState.depthBias != 0.0f || rasterState.slopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
				raster.depthBiasConstantFactor = rasterState.depthBias;
				raster.depthBiasClamp = rasterState.depthBiasClamp;
				raster.depthBiasSlopeFactor = rasterState.slopeScaledDepthBias;
				VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
				multisample.rasterizationSamples = VkSampleCountForDesc(sampleDesc.count);
				if (multisample.rasterizationSamples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) {
					multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
				}
				multisample.alphaToCoverageEnable = blendState.alphaToCoverage ? VK_TRUE : VK_FALSE;
				VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
				depthStencil.depthTestEnable = depthState.depthEnable ? VK_TRUE : VK_FALSE;
				depthStencil.depthWriteEnable = depthState.depthWrite ? VK_TRUE : VK_FALSE;
				depthStencil.depthCompareOp = VkToCompareOp(depthState.depthFunc);
				std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
				const uint32_t attachmentCount = (std::max)(renderTargets.count, blendState.numAttachments);
				blendAttachments.resize(attachmentCount);
				for (uint32_t i = 0; i < attachmentCount; ++i) {
					const BlendAttachment& src = blendState.attachments[(std::min)(i, blendState.numAttachments ? blendState.numAttachments - 1 : 0)];
					auto& dst = blendAttachments[i];
					dst.blendEnable = src.enable ? VK_TRUE : VK_FALSE;
					dst.srcColorBlendFactor = VkBlendFactorForRHI(src.srcColor);
					dst.dstColorBlendFactor = VkBlendFactorForRHI(src.dstColor);
					dst.colorBlendOp = VkBlendOpForRHI(src.colorOp);
					dst.srcAlphaBlendFactor = VkBlendFactorForRHI(src.srcAlpha);
					dst.dstAlphaBlendFactor = VkBlendFactorForRHI(src.dstAlpha);
					dst.alphaBlendOp = VkBlendOpForRHI(src.alphaOp);
					dst.colorWriteMask = VkColorMaskForRHI(src.writeMask);
				}
				VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
				colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
				colorBlend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();
				std::array<VkDynamicState, 3> dynamicStates = {
					VK_DYNAMIC_STATE_VIEWPORT,
					VK_DYNAMIC_STATE_SCISSOR,
					VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
				};
				uint32_t dynamicStateCount = hasMeshShader ? 2u : static_cast<uint32_t>(dynamicStates.size());
				VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
				dynamicState.dynamicStateCount = dynamicStateCount;
				dynamicState.pDynamicStates = dynamicStates.data();
				std::vector<VkFormat> colorFormats;
				colorFormats.reserve(renderTargets.count);
				for (uint32_t i = 0; i < renderTargets.count; ++i) {
					colorFormats.push_back(ToVkFormat(renderTargets.formats[i]));
				}
				VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
				rendering.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
				rendering.pColorAttachmentFormats = colorFormats.empty() ? nullptr : colorFormats.data();
				rendering.depthAttachmentFormat = depthFormat != Format::Unknown ? ToVkFormat(depthFormat) : VK_FORMAT_UNDEFINED;
				VkPipelineCreateFlags2CreateInfo createFlags{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
				createFlags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
				if (impl->deviceGeneratedCommandsEnabled) {
					createFlags.flags |= VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
				}
				createFlags.pNext = &rendering;
				VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
				pipelineInfo.pNext = &createFlags;
				pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
				pipelineInfo.pStages = shaderStages.data();
				pipelineInfo.pVertexInputState = hasMeshShader ? nullptr : &vertexInput;
				pipelineInfo.pInputAssemblyState = hasMeshShader ? nullptr : &inputAssembly;
				pipelineInfo.pViewportState = &viewportState;
				pipelineInfo.pRasterizationState = &raster;
				pipelineInfo.pMultisampleState = &multisample;
				pipelineInfo.pDepthStencilState = &depthStencil;
				pipelineInfo.pColorBlendState = &colorBlend;
				pipelineInfo.pDynamicState = &dynamicState;
				pipelineInfo.layout = nativeLayout;

				VkPipeline nativePipeline = VK_NULL_HANDLE;
				VkResult vkResult = vkCreateGraphicsPipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &nativePipeline);
				for (VkShaderModule module : modules) vkDestroyShaderModule(impl->device, module, nullptr);
				if (vkResult != VK_SUCCESS) {
					vkDestroyPipelineLayout(impl->device, nativeLayout, nullptr);
					return ToRHI(vkResult);
				}

				const PipelineHandle handle = impl->pipelines.alloc(VulkanPipeline{ nativePipeline, nativeLayout, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutHandle, false });
				Pipeline pipelineObject(handle);
				pipelineObject.impl = impl;
				pipelineObject.vt = &g_vkpsovt;
				out = MakePipelinePtr(device, pipelineObject, impl->selfWeak.lock());
				return Result::Ok;
			}

			if (!computeShader) {
				RHI_FAIL(Result::InvalidArgument);
			}
			if (!layoutState) {
				spdlog::error("Vulkan pipeline creation: a pipeline layout subobject is required");
				RHI_FAIL(Result::InvalidArgument);
			}
			if (!layoutState->usesDescriptorHeap || !impl->descriptorHeapEnabled) {
				RHI_FAIL(Result::Unsupported);
			}
			if (!VkIsSpirvBytecode(computeShader->bytecode)) {
				spdlog::warn("Vulkan compute pipeline creation: shader bytecode was not SPIR-V");
				RHI_FAIL(Result::Unsupported);
			}

			VkShaderModuleCreateInfo shaderModuleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			shaderModuleInfo.codeSize = computeShader->bytecode.size;
			shaderModuleInfo.pCode = static_cast<const uint32_t*>(computeShader->bytecode.data);

			VkShaderModule shaderModule = VK_NULL_HANDLE;
			VkResult vkResult = vkCreateShaderModule(impl->device, &shaderModuleInfo, nullptr, &shaderModule);
			if (vkResult != VK_SUCCESS) {
				spdlog::error("Vulkan compute pipeline creation: vkCreateShaderModule failed with VkResult {}", static_cast<int>(vkResult));
				return ToRHI(vkResult);
			}

			VkPipelineLayout nativeLayout = VK_NULL_HANDLE;

			std::vector<VkDescriptorSetAndBindingMappingEXT> descriptorMappings;
			std::vector<VkSamplerCreateInfo> embeddedSamplers;
			VkAppendDescriptorHeapMappings(impl, *layoutState, descriptorMappings, embeddedSamplers);
			VkShaderDescriptorSetAndBindingMappingInfoEXT shaderMappingInfo{ VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT };
			shaderMappingInfo.mappingCount = static_cast<uint32_t>(descriptorMappings.size());
			shaderMappingInfo.pMappings = descriptorMappings.empty() ? nullptr : descriptorMappings.data();

			const char* entryPoint = computeShader->entryPoint.empty() ? "main" : computeShader->entryPoint.c_str();
			VkPipelineShaderStageCreateInfo stageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageInfo.module = shaderModule;
			stageInfo.pName = entryPoint;
			stageInfo.pNext = shaderMappingInfo.mappingCount != 0 ? &shaderMappingInfo : nullptr;

			VkPipelineCreateFlags2CreateInfo createFlags{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
			createFlags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
			if (impl->deviceGeneratedCommandsEnabled) {
				createFlags.flags |= VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT;
			}
			VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			pipelineInfo.pNext = &createFlags;
			pipelineInfo.stage = stageInfo;
			pipelineInfo.layout = nativeLayout;

			VkPipeline nativePipeline = VK_NULL_HANDLE;
			vkResult = vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &nativePipeline);
			vkDestroyShaderModule(impl->device, shaderModule, nullptr);
			if (vkResult != VK_SUCCESS) {
				spdlog::error("Vulkan compute pipeline creation: vkCreateComputePipelines failed with VkResult {}", static_cast<int>(vkResult));
				return ToRHI(vkResult);
			}

			const PipelineHandle handle = impl->pipelines.alloc(VulkanPipeline{
				.pipeline = nativePipeline,
				.layout = nativeLayout,
				.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
				.rhiLayout = layoutHandle,
				.isCompute = true,
			});

			Pipeline pipelineObject(handle);
			pipelineObject.impl = impl;
			pipelineObject.vt = &g_vkpsovt;
			out = MakePipelinePtr(device, pipelineObject, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyBuffer(DeviceDeletionContext* context, ResourceHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl) {
				return;
			}

			VkReleaseViewsForResource(impl, handle);
			if (VulkanResource* resource = VkResourceState(impl, handle)) {
				VkDestroyResource(impl, *resource);
			}
			impl->resources.free(handle);
		}

		static void d_destroyTexture(DeviceDeletionContext* context, ResourceHandle handle) noexcept {
			d_destroyBuffer(context, handle);
		}

		static void d_destroySampler(DeviceDeletionContext* context, SamplerHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_destroyPipeline(DeviceDeletionContext* context, PipelineHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl) {
				return;
			}

			if (VulkanPipeline* pipeline = VkPipelineState(impl, handle)) {
				VkDestroyPipeline(impl, *pipeline);
			}
			impl->pipelines.free(handle);
		}

		static Result d_createWorkGraph(Device* device, const WorkGraphDesc& desc, WorkGraphPtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			RHI_FAIL(Result::Unsupported);
		}

		static void d_destroyWorkGraph(DeviceDeletionContext* context, WorkGraphHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_destroyCommandList(DeviceDeletionContext* context, CommandList* commandList) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl || !commandList) {
				return;
			}

			VulkanCommandList* commandListState = impl->commandLists.get(commandList->GetHandle());
			if (!commandListState) {
				commandList->Reset();
				return;
			}

			if (commandListState->commandBuffer != VK_NULL_HANDLE) {
				if (VulkanCommandAllocator* allocatorState = VkAllocatorState(impl, commandListState->allocatorHandle);
					allocatorState && allocatorState->pool != VK_NULL_HANDLE && impl->device != VK_NULL_HANDLE) {
					vkFreeCommandBuffers(impl->device, allocatorState->pool, 1, &commandListState->commandBuffer);
				}
			}

			for (auto& page : commandListState->emulatedRootConstantScratchPages) {
				VkDestroyEmulatedRootConstantScratchPage(impl, page);
			}

			impl->commandLists.free(commandList->GetHandle());
			commandList->Reset();
		}

		static Queue d_getQueue(Device* device, QueueKind kind) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl) {
				return Queue(kind);
			}

			Queue out{ kind, VkPrimaryQueueHandleForKind(impl, kind) };
			out.vt = &g_vkqvt;
			out.impl = impl;
			return out;
		}

		static Result d_createQueue(Device* device, QueueKind kind, const char* name, Queue& out) noexcept {
			VkIgnoreUnused(name);
			out = d_getQueue(device, kind);
			if (!out) {
				RHI_FAIL(Result::Failed);
			}
			return Result::Ok;
		}

		static void d_destroyQueue(DeviceDeletionContext* context, QueueHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_waitIdle(Device* device) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				RHI_FAIL(Result::Failed);
			}

			return ToRHI(vkDeviceWaitIdle(impl->device));
		}

		static void d_flushDeletionQueue(Device* device) noexcept {
			VkIgnoreUnused(device);
		}

		static Result d_createSwapchain(Device* device, void* windowHandle, uint32_t width, uint32_t height, Format format, uint32_t bufferCount, bool allowTearing, SwapchainPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->instance == VK_NULL_HANDLE || impl->device == VK_NULL_HANDLE || !windowHandle || width == 0 || height == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			if (!impl->swapchainExtensionEnabled) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VulkanSwapchain swapchainState{};
		#ifdef _WIN32
			VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
			surfaceCreateInfo.hwnd = static_cast<HWND>(windowHandle);
			surfaceCreateInfo.hinstance = GetModuleHandleW(nullptr);
			const VkResult surfaceResult = vkCreateWin32SurfaceKHR(impl->instance, &surfaceCreateInfo, nullptr, &swapchainState.surface);
			if (surfaceResult != VK_SUCCESS) {
				out.Reset();
				return ToRHI(surfaceResult);
			}
		#else
			VkIgnoreUnused(windowHandle);
			out.Reset();
			RHI_FAIL(Result::Unsupported);
		#endif

			const Result createResult = VkCreateOrResizeSwapchain(impl, swapchainState, width, height, format, bufferCount, allowTearing);
			if (createResult != Result::Ok) {
				VkReleaseSwapchainPresentSemaphores(impl, swapchainState);
				if (swapchainState.acquireFence != VK_NULL_HANDLE) {
					vkDestroyFence(impl->device, swapchainState.acquireFence, nullptr);
				}
				if (swapchainState.surface != VK_NULL_HANDLE) {
					vkDestroySurfaceKHR(impl->instance, swapchainState.surface, nullptr);
				}
				out.Reset();
				return createResult;
			}

			const SwapChainHandle handle = impl->swapchains.alloc(swapchainState);
			Swapchain swapchain{ handle };
			swapchain.impl = impl;
			swapchain.vt = &g_vkscvt;
			out = MakeSwapchainPtr(device, swapchain, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroySwapchain(DeviceDeletionContext* context, Swapchain* swapchain) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl || !swapchain) {
				return;
			}

			VulkanSwapchain* swapchainState = VkSwapchainState(impl, swapchain->GetHandle());
			if (!swapchainState) {
				swapchain->reset();
				return;
			}

			if (impl->device != VK_NULL_HANDLE) {
				vkDeviceWaitIdle(impl->device);
				VkReleaseSwapchainImageHandles(impl, *swapchainState);
				VkReleaseSwapchainPresentSemaphores(impl, *swapchainState);
				if (swapchainState->acquireFence != VK_NULL_HANDLE) {
					vkDestroyFence(impl->device, swapchainState->acquireFence, nullptr);
					swapchainState->acquireFence = VK_NULL_HANDLE;
				}
				if (swapchainState->swapchain != VK_NULL_HANDLE) {
					vkDestroySwapchainKHR(impl->device, swapchainState->swapchain, nullptr);
					swapchainState->swapchain = VK_NULL_HANDLE;
				}
			}

			if (impl->instance != VK_NULL_HANDLE && swapchainState->surface != VK_NULL_HANDLE) {
				vkDestroySurfaceKHR(impl->instance, swapchainState->surface, nullptr);
				swapchainState->surface = VK_NULL_HANDLE;
			}

			impl->swapchains.free(swapchain->GetHandle());
			swapchain->reset();
		}

		static void d_destroyDevice(Device* device) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (impl) {
				impl->Shutdown();
			}
		}

		static Result d_createPipelineLayout(Device* device, const PipelineLayoutDesc& desc, PipelineLayoutPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || !impl->descriptorHeapEnabled) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VulkanPipelineLayout layoutState{};
			layoutState.flags = desc.flags;
			layoutState.usesDescriptorHeap = true;
			layoutState.ranges.assign(desc.ranges.data, desc.ranges.data + desc.ranges.size);
			layoutState.pushConstants.assign(desc.pushConstants.data, desc.pushConstants.data + desc.pushConstants.size);
			layoutState.staticSamplers.assign(desc.staticSamplers.data, desc.staticSamplers.data + desc.staticSamplers.size);

			uint32_t pushDataOffset = 0;
			layoutState.pushConstantRanges.reserve(layoutState.pushConstants.size());
			for (const PushConstantRangeDesc& pushConstant : layoutState.pushConstants) {
				VulkanPushConstantRange range{};
				range.desc = pushConstant;
				range.byteOffset = pushDataOffset;
				range.dataByteSize = pushConstant.num32BitValues * 4u;
				range.byteSize = VkPushConstantStorageBytes(pushConstant);
				layoutState.pushConstantRanges.push_back(range);
				pushDataOffset += range.byteSize;
			}
			layoutState.totalPushDataBytes = pushDataOffset;

			if (layoutState.totalPushDataBytes > impl->descriptorHeapProperties.maxPushDataSize) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			const PipelineLayoutHandle handle = impl->pipelineLayouts.alloc(layoutState);
			PipelineLayout layout{ handle };
			layout.impl = impl;
			layout.vt = &g_vkplvt;
			out = MakePipelineLayoutPtr(device, layout, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_queryFeatureInfo(const Device* device, FeatureInfoHeader* chain) noexcept {
			if (!device || !chain) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<const VulkanDevice*>(device->impl);
			if (!impl || impl->physicalDevice == VK_NULL_HANDLE) {
				RHI_FAIL(Result::Failed);
			}

			bool hasDeviceLocalHeap = false;
			bool hostVisibleDeviceLocal = false;
			bool hostCoherentDeviceLocal = false;
			for (uint32_t heapIndex = 0; heapIndex < impl->memoryProperties.memoryHeapCount; ++heapIndex) {
				if ((impl->memoryProperties.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
					hasDeviceLocalHeap = true;
				}
			}

			for (uint32_t typeIndex = 0; typeIndex < impl->memoryProperties.memoryTypeCount; ++typeIndex) {
				const VkMemoryType& type = impl->memoryProperties.memoryTypes[typeIndex];
				const uint32_t heapIndex = type.heapIndex;
				if (heapIndex >= impl->memoryProperties.memoryHeapCount) {
					continue;
				}

				const bool deviceLocal = (impl->memoryProperties.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
				if (!deviceLocal) {
					continue;
				}

				if ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
					hostVisibleDeviceLocal = true;
				}
				if ((type.propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
					== (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
					hostCoherentDeviceLocal = true;
				}
			}

			for (FeatureInfoHeader* header = chain; header; header = header->pNext) {
				if (header->structSize < sizeof(FeatureInfoHeader)) {
					RHI_FAIL(Result::InvalidArgument);
				}

				switch (header->sType) {
				case FeatureInfoStructType::AdapterInfo: {
					if (header->structSize < sizeof(AdapterFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<AdapterFeatureInfo*>(header);
					VkCopyAdapterName(out->name, sizeof(out->name), impl->physicalDeviceProperties.deviceName);
					out->vendorId = impl->physicalDeviceProperties.vendorID;
					out->deviceId = impl->physicalDeviceProperties.deviceID;
					out->dedicatedVideoMemory = VkSumHeapBudget(impl->memoryProperties, true);
					out->dedicatedSystemMemory = 0;
					out->sharedSystemMemory = VkSumHeapBudget(impl->memoryProperties, false);
				} break;

				case FeatureInfoStructType::Architecture: {
					if (header->structSize < sizeof(ArchitectureFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<ArchitectureFeatureInfo*>(header);
					out->uma = hostVisibleDeviceLocal || !hasDeviceLocalHeap;
					out->cacheCoherentUMA = hostCoherentDeviceLocal;
					out->isolatedMMU = false;
					out->tileBasedRenderer = false;
				} break;

				case FeatureInfoStructType::Features: {
					if (header->structSize < sizeof(ShaderFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<ShaderFeatureInfo*>(header);
					out->maxShaderModel = ShaderModel::Unknown;
					out->unifiedResourceHeaps = impl->descriptorHeapEnabled;
					out->unboundedDescriptorTables = impl->descriptorHeapEnabled;
					out->waveOps = false;
					out->int64ShaderOps = impl->supportedFeatures.shaderInt64 == VK_TRUE;
					out->barycentrics = false;
					out->derivativesInMeshAndTaskShaders =
						impl->computeDerivativeGroupQuadsEnabled &&
						impl->computeShaderDerivativesProperties.meshAndTaskShaderDerivatives == VK_TRUE;
					out->atomicInt64OnGroupShared = false;
					out->atomicInt64OnTypedResource = impl->shaderImageInt64AtomicsEnabled;
					out->atomicInt64OnDescriptorHeapResources = impl->shaderImageInt64AtomicsEnabled && impl->descriptorHeapEnabled;
				} break;

				case FeatureInfoStructType::MeshShaders: {
					if (header->structSize < sizeof(MeshShaderFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<MeshShaderFeatureInfo*>(header);
					out->meshShader = impl->meshShaderEnabled;
					out->taskShader = impl->taskShaderEnabled;
					out->derivatives =
						impl->computeDerivativeGroupQuadsEnabled &&
						impl->computeShaderDerivativesProperties.meshAndTaskShaderDerivatives == VK_TRUE;
				} break;

				case FeatureInfoStructType::RayTracing: {
					if (header->structSize < sizeof(RayTracingFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<RayTracingFeatureInfo*>(header);
					out->pipeline = false;
					out->rayQuery = false;
					out->indirect = false;
				} break;

				case FeatureInfoStructType::ShadingRate: {
					if (header->structSize < sizeof(ShadingRateFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<ShadingRateFeatureInfo*>(header);
					out->perDrawRate = false;
					out->attachmentRate = false;
					out->perPrimitiveRate = false;
					out->additionalRates = false;
					out->tileSize = 0;
				} break;

				case FeatureInfoStructType::EnhancedBarriers: {
					if (header->structSize < sizeof(EnhancedBarriersFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<EnhancedBarriersFeatureInfo*>(header);
					out->enhancedBarriers = false;
					out->relaxedFormatCasting = false;
				} break;

				case FeatureInfoStructType::ResourceAllocation: {
					if (header->structSize < sizeof(ResourceAllocationFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<ResourceAllocationFeatureInfo*>(header);
					out->gpuUploadHeapSupported = hostVisibleDeviceLocal;
					out->tightAlignmentSupported = false;
					out->createNotZeroedHeapSupported = false;
				} break;

				case FeatureInfoStructType::WorkGraphs: {
					if (header->structSize < sizeof(WorkGraphFeatureInfo)) {
						RHI_FAIL(Result::InvalidArgument);
					}

					auto* out = reinterpret_cast<WorkGraphFeatureInfo*>(header);
					out->computeNodes = false;
					out->meshNodes = false;
				} break;

				default:
					break;
				}
			}

			return Result::Ok;
		}

		static Result d_queryVideoMemoryInfo(const Device* device, uint32_t nodeIndex, MemorySegmentGroup segmentGroup, VideoMemoryInfo& out) noexcept {
			VkIgnoreUnused(nodeIndex);
			auto* impl = device ? static_cast<const VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->physicalDevice == VK_NULL_HANDLE) {
				out = {};
				RHI_FAIL(Result::Failed);
			}

			out = {};
			switch (segmentGroup) {
			case MemorySegmentGroup::Local:
				out.budgetBytes = VkSumHeapBudget(impl->memoryProperties, true);
				break;
			case MemorySegmentGroup::NonLocal:
				out.budgetBytes = VkSumHeapBudget(impl->memoryProperties, false);
				break;
			default:
				RHI_FAIL(Result::InvalidArgument);
			}

			return Result::Ok;
		}

		static void d_destroyPipelineLayout(DeviceDeletionContext* context, PipelineLayoutHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl) {
				return;
			}
			impl->pipelineLayouts.free(handle);
		}

		static Result d_createCommandSignature(Device* device, const CommandSignatureDesc& desc, PipelineLayoutHandle layout, CommandSignaturePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || desc.args.size == 0 || desc.byteStride == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			bool needsGeneratedCommands = false;
			bool hasExecutableToken = false;
			uint32_t runningOffset = 0;
			for (const IndirectArg& arg : desc.args) {
				const uint32_t argByteSize = VkIndirectArgumentByteSize(arg);
				if (argByteSize == 0 || runningOffset + argByteSize > desc.byteStride) {
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}

				switch (arg.kind) {
				case IndirectArgKind::Draw:
				case IndirectArgKind::DrawIndexed:
				case IndirectArgKind::Dispatch:
				case IndirectArgKind::DispatchMesh:
					hasExecutableToken = true;
					break;
				case IndirectArgKind::Constant:
					needsGeneratedCommands = true;
					break;
				default:
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}
				runningOffset += argByteSize;
			}
			if (!hasExecutableToken) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkIndirectCommandsLayoutEXT indirectLayout = VK_NULL_HANDLE;
			if (needsGeneratedCommands || desc.args.size != 1) {
				VulkanPipelineLayout* layoutState = VkPipelineLayoutState(impl, layout);
				if (!layoutState ||
					!impl->deviceGeneratedCommandsEnabled ||
					!impl->dynamicGeneratedPipelineLayoutEnabled ||
					!impl->bufferDeviceAddressEnabled ||
					!vkCreateIndirectCommandsLayoutEXT) {
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}

				std::vector<VkIndirectCommandsLayoutTokenEXT> tokens;
				std::vector<VkIndirectCommandsPushConstantTokenEXT> pushTokens;
				tokens.reserve(desc.args.size);
				pushTokens.reserve(desc.args.size);

				VkShaderStageFlags executionDomainStages = 0;
				for (const IndirectArg& arg : desc.args) {
					if (arg.kind == IndirectArgKind::Constant) {
						continue;
					}

					const VkShaderStageFlags argStages = VkIndirectExecutionDomainStages(arg.kind);
					if (argStages == 0) {
						out.Reset();
						RHI_FAIL(Result::Unsupported);
					}
					if (executionDomainStages != 0 && executionDomainStages != argStages) {
						out.Reset();
						RHI_FAIL(Result::Unsupported);
					}
					executionDomainStages = argStages;
				}

				runningOffset = 0;
				VkShaderStageFlags shaderStages = executionDomainStages;
				for (const IndirectArg& arg : desc.args) {
					VkIndirectCommandsLayoutTokenEXT token{ VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT };
					token.offset = runningOffset;

					if (arg.kind == IndirectArgKind::Constant) {
						const uint32_t rootIndex = arg.u.rootConstants.rootIndex;
						if (rootIndex >= layoutState->pushConstantRanges.size()) {
							out.Reset();
							RHI_FAIL(Result::InvalidArgument);
						}

						const VulkanPushConstantRange* pushRange = &layoutState->pushConstantRanges[rootIndex];
						if (pushRange->desc.type != PushConstantRangeType::RootConstants32) {
							out.Reset();
							RHI_FAIL(Result::Unsupported);
						}

						VkIndirectCommandsPushConstantTokenEXT pushToken{};
						VkShaderStageFlags pushStages = VkShaderStageFlagsForRHI(pushRange->desc.visibility);
						if (executionDomainStages != 0) {
							pushStages &= executionDomainStages;
						}
						if (pushStages == 0) {
							out.Reset();
							RHI_FAIL(Result::Unsupported);
						}
						pushToken.updateRange.stageFlags = pushStages;
						pushToken.updateRange.offset = pushRange->byteOffset + arg.u.rootConstants.destOffset32 * 4u;
						pushToken.updateRange.size = arg.u.rootConstants.num32 * 4u;
						if (pushToken.updateRange.offset + pushToken.updateRange.size > pushRange->byteOffset + pushRange->byteSize) {
							out.Reset();
							RHI_FAIL(Result::Unsupported);
						}

						pushTokens.push_back(pushToken);
						token.type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT;
						token.data.pPushConstant = &pushTokens.back();
					}
					else {
						token.type = VkIndirectCommandsTokenTypeForRHI(arg.kind);
						if (token.type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_MAX_ENUM_EXT) {
							out.Reset();
							RHI_FAIL(Result::Unsupported);
						}
					}

					tokens.push_back(token);
					runningOffset += VkIndirectArgumentByteSize(arg);
				}

				const VkShaderStageFlags layoutShaderStages = shaderStages != 0 ? shaderStages : VK_SHADER_STAGE_ALL;
				VkPushConstantRange pipelineLayoutPushRange{};
				pipelineLayoutPushRange.stageFlags = layoutShaderStages;
				pipelineLayoutPushRange.offset = 0;
				pipelineLayoutPushRange.size = layoutState->totalPushDataBytes;
				VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
				pipelineLayoutInfo.setLayoutCount = 0;
				pipelineLayoutInfo.pSetLayouts = nullptr;
				pipelineLayoutInfo.pushConstantRangeCount = pipelineLayoutPushRange.size != 0 ? 1u : 0u;
				pipelineLayoutInfo.pPushConstantRanges = pipelineLayoutPushRange.size != 0 ? &pipelineLayoutPushRange : nullptr;
				VkIndirectCommandsLayoutCreateInfoEXT layoutCreateInfo{ VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT };
				layoutCreateInfo.pNext = &pipelineLayoutInfo;
				layoutCreateInfo.shaderStages = layoutShaderStages;
				layoutCreateInfo.indirectStride = desc.byteStride;
				layoutCreateInfo.pipelineLayout = VK_NULL_HANDLE;
				layoutCreateInfo.tokenCount = static_cast<uint32_t>(tokens.size());
				layoutCreateInfo.pTokens = tokens.data();
				const VkResult createResult = vkCreateIndirectCommandsLayoutEXT(impl->device, &layoutCreateInfo, nullptr, &indirectLayout);
				if (createResult != VK_SUCCESS) {
					out.Reset();
					return ToRHI(createResult);
				}
			}

			VulkanCommandSignature signature{};
			signature.args.assign(desc.args.data, desc.args.data + desc.args.size);
			signature.byteStride = desc.byteStride;
			signature.indirectLayout = indirectLayout;
			signature.usesExecutionSet = false;
			const CommandSignatureHandle handle = impl->commandSignatures.alloc(signature);
			CommandSignature object{ handle };
			object.impl = impl;
			object.vt = &g_vkcsvt;
			out = MakeCommandSignaturePtr(device, object, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyCommandSignature(DeviceDeletionContext* context, CommandSignatureHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (impl) {
				if (VulkanCommandSignature* signature = VkCommandSignatureState(impl, handle)) {
					VkDestroyCommandSignature(impl, *signature);
				}
				impl->commandSignatures.free(handle);
			}
		}

		static Result d_createDescriptorHeap(Device* device, const DescriptorHeapDesc& desc, DescriptorHeapPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE || desc.capacity == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VulkanDescriptorHeap heap{};
			heap.type = desc.type;
			heap.capacity = desc.capacity;
			heap.shaderVisible = desc.shaderVisible;
			heap.imageViewSlots.resize(desc.capacity);

			if (impl->descriptorHeapEnabled && (desc.type == DescriptorHeapType::CbvSrvUav || desc.type == DescriptorHeapType::Sampler)) {
				heap.descriptorStride = VkDescriptorHeapStride(impl, desc.type);
				heap.descriptorBytes = static_cast<uint64_t>(desc.capacity) * heap.descriptorStride;
				if (desc.type == DescriptorHeapType::Sampler) {
					heap.reservedRangeSize = (std::max)(
						static_cast<uint64_t>(impl->descriptorHeapProperties.minSamplerHeapReservedRange),
						static_cast<uint64_t>(impl->descriptorHeapProperties.minSamplerHeapReservedRangeWithEmbedded));
				}
				else {
					heap.reservedRangeSize = static_cast<uint64_t>(impl->descriptorHeapProperties.minResourceHeapReservedRange);
				}
				heap.reservedRangeOffset = heap.descriptorBytes;
				const uint64_t totalBytes = heap.descriptorBytes + heap.reservedRangeSize;

				VkBufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
				createInfo.size = totalBytes;
				createInfo.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
				createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				const VkResult createResult = vkCreateBuffer(impl->device, &createInfo, nullptr, &heap.buffer);
				if (createResult != VK_SUCCESS) {
					out.Reset();
					return ToRHI(createResult);
				}

				VkMemoryRequirements memoryRequirements{};
				vkGetBufferMemoryRequirements(impl->device, heap.buffer, &memoryRequirements);

				uint32_t memoryTypeIndex = 0;
				if (!VkFindMemoryTypeIndex(
					impl->memoryProperties,
					memoryRequirements.memoryTypeBits,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					memoryTypeIndex)) {
					vkDestroyBuffer(impl->device, heap.buffer, nullptr);
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}

				VkMemoryAllocateFlagsInfo allocateFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
				allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

				VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				allocateInfo.pNext = &allocateFlagsInfo;
				allocateInfo.allocationSize = memoryRequirements.size;
				allocateInfo.memoryTypeIndex = memoryTypeIndex;

				const VkResult allocateResult = vkAllocateMemory(impl->device, &allocateInfo, nullptr, &heap.memory);
				if (allocateResult != VK_SUCCESS) {
					vkDestroyBuffer(impl->device, heap.buffer, nullptr);
					out.Reset();
					return ToRHI(allocateResult);
				}

				const VkResult bindResult = vkBindBufferMemory(impl->device, heap.buffer, heap.memory, 0);
				if (bindResult != VK_SUCCESS) {
					vkFreeMemory(impl->device, heap.memory, nullptr);
					vkDestroyBuffer(impl->device, heap.buffer, nullptr);
					out.Reset();
					return ToRHI(bindResult);
				}

				const VkResult mapResult = vkMapMemory(impl->device, heap.memory, 0, totalBytes, 0, &heap.mappedData);
				if (mapResult != VK_SUCCESS) {
					vkFreeMemory(impl->device, heap.memory, nullptr);
					vkDestroyBuffer(impl->device, heap.buffer, nullptr);
					out.Reset();
					return ToRHI(mapResult);
				}

				std::memset(heap.mappedData, 0, static_cast<size_t>(totalBytes));

				VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
				addressInfo.buffer = heap.buffer;
				heap.deviceAddress = vkGetBufferDeviceAddress(impl->device, &addressInfo);
			}

			const DescriptorHeapHandle handle = impl->descriptorHeaps.alloc(heap);
			DescriptorHeap descriptorHeap{ handle };
			descriptorHeap.impl = impl;
			descriptorHeap.vt = &g_vkdhvt;
			out = MakeDescriptorHeapPtr(device, descriptorHeap, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyDescriptorHeap(DeviceDeletionContext* context, DescriptorHeapHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, handle);
			if (!impl || !heap) {
				return;
			}

			for (VulkanImageViewSlot& slot : heap->imageViewSlots) {
				VkResetDescriptorSlot(impl, slot);
			}
			VkDestroyDescriptorHeapBacking(impl, *heap);

			impl->descriptorHeaps.free(handle);
		}

		static Result d_createShaderResourceView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const SrvDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource);
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			if (!impl || !resourceState) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (desc.dimension == SrvDim::Buffer) {
				VkFormat viewFormat = VK_FORMAT_UNDEFINED;
				uint32_t strideBytes = 0;
				VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				switch (desc.buffer.kind) {
				case BufferViewKind::Raw:
					viewFormat = VK_FORMAT_R32_UINT;
					break;
				case BufferViewKind::Structured:
					strideBytes = desc.buffer.structureByteStride;
					break;
				case BufferViewKind::Typed:
					viewFormat = ToVkFormat(desc.formatOverride);
					descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
					break;
				}

				if (desc.buffer.kind == BufferViewKind::Typed && viewFormat == VK_FORMAT_UNDEFINED) {
					RHI_FAIL(Result::Unsupported);
				}

				const uint64_t elementSize = desc.buffer.kind == BufferViewKind::Structured
					? strideBytes
					: (desc.buffer.kind == BufferViewKind::Typed ? static_cast<uint64_t>(FormatByteSize(desc.formatOverride)) : 4ull);
				if (elementSize == 0) {
					RHI_FAIL(Result::InvalidArgument);
				}

				return VkCreateBufferDescriptorSlot(impl,
					slot,
					resource,
					viewFormat,
					descriptorType,
					desc.buffer.kind,
					desc.buffer.firstElement * elementSize,
					static_cast<uint64_t>(desc.buffer.numElements) * elementSize,
					strideBytes,
					nullptr);
			}

			const VkImageViewType viewType = VkImageViewTypeForSrv(desc.dimension);
			const bool isDepthStencilResource = VkIsDepthStencilFormat(resourceState->format);
			const VkFormat requestedViewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resourceState->format;
			const VkFormat viewFormat = isDepthStencilResource ? resourceState->format : requestedViewFormat;
			const VkImageAspectFlags aspectMask = isDepthStencilResource ? VkSampledImageAspectMaskForFormat(resourceState->format) : VkAspectMaskForFormat(viewFormat);
			if (viewType == VK_IMAGE_VIEW_TYPE_MAX_ENUM || viewFormat == VK_FORMAT_UNDEFINED) {
				RHI_FAIL(Result::Unsupported);
			}
			const VkImageSubresourceRange subresourceRange = VkSrvSubresourceRange(*resourceState, desc, aspectMask);

			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					RHI_FAIL(Result::InvalidArgument);
				}

				std::memset(slotAddress, 0, static_cast<size_t>(heap->descriptorStride));

				VkImageViewCreateInfo viewCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				viewCreateInfo.image = resourceState->image;
				viewCreateInfo.viewType = viewType;
				viewCreateInfo.format = viewFormat;
				viewCreateInfo.components = {
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
				};
				viewCreateInfo.subresourceRange = subresourceRange;

				VkImageDescriptorInfoEXT imageInfo{ VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT };
				imageInfo.pView = &viewCreateInfo;
				imageInfo.layout = VkToImageLayout(ResourceLayout::ShaderResource, aspectMask);

				VkResourceDescriptorInfoEXT descriptorInfo{ VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT };
				descriptorInfo.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				descriptorInfo.data.pImage = &imageInfo;

				VkHostAddressRangeEXT descriptorRange{};
				descriptorRange.address = slotAddress;
				descriptorRange.size = static_cast<size_t>(heap->descriptorStride);

				const VkResult result = vkWriteResourceDescriptorsEXT(impl->device, 1, &descriptorInfo, &descriptorRange);
				if (result != VK_SUCCESS) {
					return ToRHI(result);
				}

				VulkanImageViewSlot* descriptorSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::CbvSrvUav);
				if (!descriptorSlot) {
					RHI_FAIL(Result::InvalidArgument);
				}
				VkResetDescriptorSlot(impl, *descriptorSlot);
				descriptorSlot->kind = VulkanImageViewSlot::Kind::ImageView;
				descriptorSlot->resource = resource;
				descriptorSlot->format = viewFormat;
				descriptorSlot->aspectMask = aspectMask;
				descriptorSlot->range = { subresourceRange.baseMipLevel, subresourceRange.levelCount, subresourceRange.baseArrayLayer, subresourceRange.layerCount };
				return Result::Ok;
			}

			return VkCreateImageViewSlot(impl,
				slot,
				DescriptorHeapType::CbvSrvUav,
				resource,
				viewFormat,
				aspectMask,
				viewType,
				{ subresourceRange.baseMipLevel, subresourceRange.levelCount, subresourceRange.baseArrayLayer, subresourceRange.layerCount });
		}

		static Result d_createUnorderedAccessView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const UavDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource);
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			if (!impl || !resourceState) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (desc.dimension == UavDim::Texture2DMS || desc.dimension == UavDim::Texture2DMSArray) {
				RHI_FAIL(Result::Unsupported);
			}

			if (desc.dimension == UavDim::Buffer) {
				VkFormat viewFormat = VK_FORMAT_UNDEFINED;
				uint32_t strideBytes = 0;
				VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				switch (desc.buffer.kind) {
				case BufferViewKind::Raw:
					viewFormat = VK_FORMAT_R32_UINT;
					break;
				case BufferViewKind::Structured:
					strideBytes = desc.buffer.structureByteStride;
					break;
				case BufferViewKind::Typed:
					viewFormat = ToVkFormat(desc.formatOverride);
					descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
					break;
				}

				if (desc.buffer.kind == BufferViewKind::Typed && viewFormat == VK_FORMAT_UNDEFINED) {
					RHI_FAIL(Result::Unsupported);
				}

				const uint64_t elementSize = desc.buffer.kind == BufferViewKind::Structured
					? strideBytes
					: (desc.buffer.kind == BufferViewKind::Typed ? static_cast<uint64_t>(FormatByteSize(desc.formatOverride)) : 4ull);
				if (elementSize == 0) {
					RHI_FAIL(Result::InvalidArgument);
				}

				return VkCreateBufferDescriptorSlot(impl,
					slot,
					resource,
					viewFormat,
					descriptorType,
					desc.buffer.kind,
					desc.buffer.firstElement * elementSize,
					static_cast<uint64_t>(desc.buffer.numElements) * elementSize,
					strideBytes,
					nullptr);
			}

			const VkImageViewType viewType = VkImageViewTypeForUav(desc.dimension);
			const VkFormat viewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resourceState->format;
			const VkImageAspectFlags aspectMask = VkAspectMaskForFormat(viewFormat);
			if (viewType == VK_IMAGE_VIEW_TYPE_MAX_ENUM || viewFormat == VK_FORMAT_UNDEFINED) {
				RHI_FAIL(Result::Unsupported);
			}

			const VkImageSubresourceRange subresourceRange = VkUavSubresourceRange(*resourceState, desc, aspectMask);
			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					RHI_FAIL(Result::InvalidArgument);
				}

				std::memset(slotAddress, 0, static_cast<size_t>(heap->descriptorStride));

				VkImageViewCreateInfo viewCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				viewCreateInfo.image = resourceState->image;
				viewCreateInfo.viewType = viewType;
				viewCreateInfo.format = viewFormat;
				viewCreateInfo.components = {
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY,
				};
				viewCreateInfo.subresourceRange = subresourceRange;

				VkImageDescriptorInfoEXT imageInfo{ VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT };
				imageInfo.pView = &viewCreateInfo;
				imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;

				VkResourceDescriptorInfoEXT descriptorInfo{ VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT };
				descriptorInfo.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				descriptorInfo.data.pImage = &imageInfo;

				VkHostAddressRangeEXT descriptorRange{};
				descriptorRange.address = slotAddress;
				descriptorRange.size = static_cast<size_t>(heap->descriptorStride);

				const VkResult result = vkWriteResourceDescriptorsEXT(impl->device, 1, &descriptorInfo, &descriptorRange);
				if (result != VK_SUCCESS) {
					return ToRHI(result);
				}

				VulkanImageViewSlot* descriptorSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::CbvSrvUav);
				if (!descriptorSlot) {
					RHI_FAIL(Result::InvalidArgument);
				}
				VkResetDescriptorSlot(impl, *descriptorSlot);
				descriptorSlot->kind = VulkanImageViewSlot::Kind::ImageView;
				descriptorSlot->resource = resource;
				descriptorSlot->format = viewFormat;
				descriptorSlot->aspectMask = aspectMask;
				descriptorSlot->range = { subresourceRange.baseMipLevel, subresourceRange.levelCount, subresourceRange.baseArrayLayer, subresourceRange.layerCount };
				return Result::Ok;
			}

			return VkCreateImageViewSlot(impl,
				slot,
				DescriptorHeapType::CbvSrvUav,
				resource,
				viewFormat,
				aspectMask,
				viewType,
				{ subresourceRange.baseMipLevel, subresourceRange.levelCount, subresourceRange.baseArrayLayer, subresourceRange.layerCount });
		}

		static Result d_createConstantBufferView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const CbvDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resourceState = VkResourceState(impl, resource);
			if (!impl || !resourceState || resourceState->type != ResourceType::Buffer || desc.byteSize == 0) {
				RHI_FAIL(Result::InvalidArgument);
			}

			const uint32_t alignedSize = (desc.byteSize + 255u) & ~255u;
			return VkCreateBufferDescriptorSlot(impl,
				slot,
				resource,
				VK_FORMAT_UNDEFINED,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				BufferViewKind::Structured,
				desc.byteOffset,
				alignedSize,
				0,
				&desc);
		}

		static Result d_createSampler(Device* device, DescriptorSlot slot, const SamplerDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, slot.heap);
			VulkanImageViewSlot* descriptorSlot = VkImageViewSlotState(impl, slot, DescriptorHeapType::Sampler);
			if (!impl || !descriptorSlot) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (desc.reduction == ReductionMode::Min || desc.reduction == ReductionMode::Max) {
				RHI_FAIL(Result::Unsupported);
			}
			if (desc.borderPreset == BorderPreset::Custom) {
				RHI_FAIL(Result::Unsupported);
			}

			VkResetDescriptorSlot(impl, *descriptorSlot);

			VkSamplerCreateInfo createInfo = VkBuildSamplerCreateInfo(desc);

			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					RHI_FAIL(Result::InvalidArgument);
				}

				std::memset(slotAddress, 0, static_cast<size_t>(heap->descriptorStride));

				VkHostAddressRangeEXT descriptorRange{};
				descriptorRange.address = slotAddress;
				descriptorRange.size = static_cast<size_t>(heap->descriptorStride);

				const VkResult result = vkWriteSamplerDescriptorsEXT(impl->device, 1, &createInfo, &descriptorRange);
				if (result != VK_SUCCESS) {
					return ToRHI(result);
				}

				descriptorSlot->kind = VulkanImageViewSlot::Kind::Sampler;
				descriptorSlot->samplerDesc = desc;
				return Result::Ok;
			}

			const VkResult result = vkCreateSampler(impl->device, &createInfo, nullptr, &descriptorSlot->sampler);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}

			descriptorSlot->kind = VulkanImageViewSlot::Kind::Sampler;
			descriptorSlot->samplerDesc = desc;
			return Result::Ok;
		}

		static Result d_createRenderTargetView(Device* device, DescriptorSlot slot, const ResourceHandle& texture, const RtvDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resource = VkResourceState(impl, texture);
			if (!impl || !resource) {
				RHI_FAIL(Result::InvalidArgument);
			}

			const VkFormat viewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resource->format;
			const VkImageViewType viewType = VkImageViewTypeForRtv(desc.dimension);
			return VkCreateImageViewSlot(impl, slot, DescriptorHeapType::RTV, texture, viewFormat, VK_IMAGE_ASPECT_COLOR_BIT, viewType, desc.range);
		}

		static Result d_createDepthStencilView(Device* device, DescriptorSlot slot, const ResourceHandle& texture, const DsvDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resource = VkResourceState(impl, texture);
			if (!impl || !resource) {
				RHI_FAIL(Result::InvalidArgument);
			}

			const VkFormat viewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resource->format;
			const VkImageAspectFlags aspectMask = VkAspectMaskForFormat(viewFormat);
			const VkImageViewType viewType = VkImageViewTypeForDsv(desc.dimension);
			return VkCreateImageViewSlot(impl, slot, DescriptorHeapType::DSV, texture, viewFormat, aspectMask, viewType, desc.range);
		}

		static Result d_createCommandAllocator(Device* device, QueueKind kind, CommandAllocatorPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			const uint32_t queueSlot = VkPrimaryQueueSlotForKind(kind);
			if (queueSlot >= impl->queues.size() || impl->queues[queueSlot].familyIndex == kVkInvalidQueueFamily) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkCommandPoolCreateInfo createInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			createInfo.queueFamilyIndex = impl->queues[queueSlot].familyIndex;

			VkCommandPool pool = VK_NULL_HANDLE;
			const VkResult result = vkCreateCommandPool(impl->device, &createInfo, nullptr, &pool);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			const CommandAllocatorHandle handle = impl->allocators.alloc(VulkanCommandAllocator{ pool, kind, createInfo.queueFamilyIndex });
			CommandAllocator allocator{ handle };
			allocator.impl = impl;
			allocator.vt = &g_vkcalvt;
			out = MakeCommandAllocatorPtr(device, allocator, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyCommandAllocator(DeviceDeletionContext* context, CommandAllocator* allocator) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			if (!impl || !allocator) {
				return;
			}

			VulkanCommandAllocator* allocatorState = impl->allocators.get(allocator->GetHandle());
			if (!allocatorState) {
				allocator->Reset();
				return;
			}

			if (allocatorState->pool != VK_NULL_HANDLE && impl->device != VK_NULL_HANDLE) {
				vkDestroyCommandPool(impl->device, allocatorState->pool, nullptr);
			}

			impl->allocators.free(allocator->GetHandle());
			allocator->Reset();
		}

		static Result d_createCommandList(Device* device, QueueKind kind, CommandAllocator allocator, CommandListPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanCommandAllocator* allocatorState = VkAllocatorState(impl, allocator.GetHandle());
			if (!impl || impl->device == VK_NULL_HANDLE || !allocatorState || allocatorState->pool == VK_NULL_HANDLE) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			Result result = VkAllocatePrimaryCommandBuffer(impl->device, allocatorState->pool, commandBuffer);
			if (result != Result::Ok) {
				out.Reset();
				RHI_FAIL(result);
			}

			result = VkBeginCommandRecording(commandBuffer);
			if (result != Result::Ok) {
				vkFreeCommandBuffers(impl->device, allocatorState->pool, 1, &commandBuffer);
				out.Reset();
				RHI_FAIL(result);
			}

			VulkanCommandList commandListState{};
			commandListState.commandBuffer = commandBuffer;
			commandListState.allocatorHandle = allocator.GetHandle();
			commandListState.kind = kind;
			commandListState.isRecording = true;
			const CommandListHandle handle = impl->commandLists.alloc(commandListState);
			CommandList commandList{ handle };
			commandList.impl = impl;
			commandList.vt = &g_vkclvt;
			out = MakeCommandListPtr(device, commandList, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedBuffer(Device* device, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE || desc.type != ResourceType::Buffer || desc.buffer.sizeBytes == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}
			if (desc.heapType == HeapType::Custom) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkBufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			createInfo.size = desc.buffer.sizeBytes;
			createInfo.usage = VkBufferUsageForDesc(desc);
			if (impl->bufferDeviceAddressEnabled) {
				createInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
			}
			createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkBuffer buffer = VK_NULL_HANDLE;
			VkResult result = vkCreateBuffer(impl->device, &createInfo, nullptr, &buffer);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			VkMemoryRequirements memoryRequirements{};
			vkGetBufferMemoryRequirements(impl->device, buffer, &memoryRequirements);

			const VkMemoryPropertyFlags preferredFlags = VkPreferredMemoryPropertyFlags(desc.heapType);
			const VkMemoryPropertyFlags requiredFlags = VkRequiredMemoryPropertyFlags(desc.heapType);
			uint32_t memoryTypeIndex = 0;
			if (!VkFindMemoryTypeIndex(impl->memoryProperties, memoryRequirements.memoryTypeBits, preferredFlags, requiredFlags, memoryTypeIndex)) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			allocateInfo.allocationSize = memoryRequirements.size;
			allocateInfo.memoryTypeIndex = memoryTypeIndex;
			VkMemoryAllocateFlagsInfo allocateFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
			if (impl->bufferDeviceAddressEnabled) {
				allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
				allocateInfo.pNext = &allocateFlagsInfo;
			}

			VkDeviceMemory memory = VK_NULL_HANDLE;
			result = vkAllocateMemory(impl->device, &allocateInfo, nullptr, &memory);
			if (result != VK_SUCCESS) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			result = vkBindBufferMemory(impl->device, buffer, memory, 0);
			if (result != VK_SUCCESS) {
				vkFreeMemory(impl->device, memory, nullptr);
				vkDestroyBuffer(impl->device, buffer, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			const VkMemoryPropertyFlags actualFlags = impl->memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags;
			VulkanResource resourceState{};
			resourceState.buffer = buffer;
			resourceState.memory = memory;
			resourceState.bufferSize = desc.buffer.sizeBytes;
			resourceState.type = ResourceType::Buffer;
			resourceState.hostVisible = (actualFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
			resourceState.ownsBuffer = true;
			resourceState.ownsMemory = true;
			resourceState.currentLayout = ResourceLayout::Common;
			resourceState.submittedLayout = ResourceLayout::Common;
			if (impl->bufferDeviceAddressEnabled) {
				VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
				addressInfo.buffer = buffer;
				resourceState.deviceAddress = vkGetBufferDeviceAddress(impl->device, &addressInfo);
			}

			const ResourceHandle handle = impl->resources.alloc(resourceState);
			Resource resource{ handle, false };
			resource.impl = impl;
			resource.vt = &g_vkbuf_rvt;
			out = MakeBufferPtr(device, resource, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedTexture(Device* device, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}
			if (desc.type != ResourceType::Texture1D && desc.type != ResourceType::Texture2D && desc.type != ResourceType::Texture3D) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}
			if (desc.texture.width == 0 || desc.texture.height == 0 || desc.texture.mipLevels == 0 || desc.texture.format == Format::Unknown) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}
			if (desc.texture.initialLayout != ResourceLayout::Undefined && desc.texture.initialLayout != ResourceLayout::Common) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}
			if (desc.heapType != HeapType::DeviceLocal) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			const VkFormat format = ToVkFormat(desc.texture.format);
			const VkImageType imageType = VkImageTypeForResourceType(desc.type);
			const VkSampleCountFlagBits samples = VkSampleCountForDesc(desc.texture.sampleCount);
			if (format == VK_FORMAT_UNDEFINED || imageType == VK_IMAGE_TYPE_MAX_ENUM || samples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}
			VkImageCreateFlags imageCreateFlags = 0;
			if (!VkImageCreateFlagsForDesc(desc, imageCreateFlags)) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			createInfo.flags = imageCreateFlags;
			createInfo.imageType = imageType;
			createInfo.format = format;
			createInfo.extent.width = desc.texture.width;
			createInfo.extent.height = desc.type == ResourceType::Texture1D ? 1u : desc.texture.height;
			createInfo.extent.depth = desc.type == ResourceType::Texture3D ? desc.texture.depthOrLayers : 1u;
			createInfo.mipLevels = desc.texture.mipLevels;
			createInfo.arrayLayers = desc.type == ResourceType::Texture3D ? 1u : desc.texture.depthOrLayers;
			createInfo.samples = samples;
			createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			createInfo.usage = VkImageUsageForDesc(desc, format);
			createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkImage image = VK_NULL_HANDLE;
			VkResult result = vkCreateImage(impl->device, &createInfo, nullptr, &image);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			VkMemoryRequirements memoryRequirements{};
			vkGetImageMemoryRequirements(impl->device, image, &memoryRequirements);

			uint32_t memoryTypeIndex = 0;
			if (!VkFindMemoryTypeIndex(impl->memoryProperties,
				memoryRequirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				memoryTypeIndex)) {
				vkDestroyImage(impl->device, image, nullptr);
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			allocateInfo.allocationSize = memoryRequirements.size;
			allocateInfo.memoryTypeIndex = memoryTypeIndex;

			VkDeviceMemory memory = VK_NULL_HANDLE;
			result = vkAllocateMemory(impl->device, &allocateInfo, nullptr, &memory);
			if (result != VK_SUCCESS) {
				vkDestroyImage(impl->device, image, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			result = vkBindImageMemory(impl->device, image, memory, 0);
			if (result != VK_SUCCESS) {
				vkFreeMemory(impl->device, memory, nullptr);
				vkDestroyImage(impl->device, image, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			VulkanResource resourceState{};
			resourceState.image = image;
			resourceState.memory = memory;
			resourceState.format = format;
			resourceState.type = desc.type;
			resourceState.currentLayout = ResourceLayout::Undefined;
			resourceState.width = desc.texture.width;
			resourceState.height = desc.type == ResourceType::Texture1D ? 1u : desc.texture.height;
			resourceState.depthOrLayers = desc.type == ResourceType::Texture3D ? 1u : desc.texture.depthOrLayers;
			resourceState.mipLevels = desc.texture.mipLevels;
			resourceState.ownsImage = true;
			resourceState.ownsMemory = true;

			const ResourceHandle handle = impl->resources.alloc(resourceState);
			Resource texture{ handle, true };
			texture.impl = impl;
			texture.vt = &g_vktex_rvt;
			out = MakeTexturePtr(device, texture, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedResource(Device* device, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			switch (desc.type) {
			case ResourceType::Buffer:
				return d_createCommittedBuffer(device, desc, out);
			case ResourceType::Texture1D:
			case ResourceType::Texture2D:
			case ResourceType::Texture3D:
				return d_createCommittedTexture(device, desc, out);
			case ResourceType::Unknown:
			default:
				VkIgnoreUnused(device);
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}
		}

		static uint32_t d_getDescriptorHandleIncrementSize(Device* device, DescriptorHeapType type) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			return static_cast<uint32_t>(VkDescriptorHeapStride(impl, type));
		}

		static Result d_createTimeline(Device* device, uint64_t initialValue, const char* debugName, TimelinePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || !impl->timelineSemaphoreEnabled) {
				out.Reset();
				RHI_FAIL(impl ? Result::Unsupported : Result::InvalidArgument);
			}

			VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
			typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			typeInfo.initialValue = initialValue;
			VkSemaphoreCreateInfo createInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			createInfo.pNext = &typeInfo;
			VkSemaphore semaphore = VK_NULL_HANDLE;
			const VkResult result = vkCreateSemaphore(impl->device, &createInfo, nullptr, &semaphore);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			const TimelineHandle handle = impl->timelines.alloc(VulkanTimeline{ semaphore });
			Timeline timeline{ handle };
			timeline.impl = impl;
			timeline.vt = &g_vktlvt;
			out = MakeTimelinePtr(device, timeline, impl->selfWeak.lock());
			if (debugName) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(semaphore), VK_OBJECT_TYPE_SEMAPHORE, debugName);
			}
			return Result::Ok;
		}

		static void d_destroyTimeline(DeviceDeletionContext* context, TimelineHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			VulkanTimeline* timeline = VkTimelineState(impl, handle);
			if (!impl || !timeline) {
				return;
			}
			if (timeline->semaphore != VK_NULL_HANDLE) {
				vkDestroySemaphore(impl->device, timeline->semaphore, nullptr);
			}
			impl->timelines.free(handle);
		}

		static Result d_createHeap(const Device* device, const HeapDesc& desc, HeapPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE || desc.sizeBytes == 0 || desc.memory == HeapType::Custom) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VkMemoryPropertyFlags preferredFlags = VkPreferredMemoryPropertyFlags(desc.memory);
			VkMemoryPropertyFlags requiredFlags = VkRequiredMemoryPropertyFlags(desc.memory);
			uint32_t memoryTypeBits = 0;
			for (uint32_t index = 0; index < impl->memoryProperties.memoryTypeCount; ++index) {
				memoryTypeBits |= (1u << index);
			}

			uint32_t memoryTypeIndex = 0;
			if (!VkFindMemoryTypeIndex(impl->memoryProperties, memoryTypeBits, preferredFlags, requiredFlags, memoryTypeIndex)) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			VkMemoryAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			allocateInfo.allocationSize = desc.sizeBytes;
			allocateInfo.memoryTypeIndex = memoryTypeIndex;
			VkMemoryAllocateFlagsInfo allocateFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
			if (impl->bufferDeviceAddressEnabled) {
				allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
				allocateInfo.pNext = &allocateFlagsInfo;
			}
			VkDeviceMemory memory = VK_NULL_HANDLE;
			const VkResult result = vkAllocateMemory(impl->device, &allocateInfo, nullptr, &memory);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			const HeapHandle handle = impl->heaps.alloc(VulkanHeap{ memory, desc.sizeBytes, memoryTypeIndex, desc.memory });
			Heap heap{ handle };
			heap.impl = impl;
			heap.vt = &g_vkhevt;
			out = MakeHeapPtr(&impl->self, heap, impl->selfWeak.lock());
			if (desc.debugName) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(memory), VK_OBJECT_TYPE_DEVICE_MEMORY, desc.debugName);
			}
			return Result::Ok;
		}

		static void d_destroyHeap(DeviceDeletionContext* context, HeapHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			VulkanHeap* heap = VkHeapState(impl, handle);
			if (!impl || !heap) {
				return;
			}
			if (heap->memory != VK_NULL_HANDLE) {
				vkFreeMemory(impl->device, heap->memory, nullptr);
			}
			impl->heaps.free(handle);
		}

		static void d_setNameBuffer(Device* device, ResourceHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resource = VkResourceState(impl, handle);
			if (resource) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(resource->buffer), VK_OBJECT_TYPE_BUFFER, name);
			}
		}

		static void d_setNameTexture(Device* device, ResourceHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resource = VkResourceState(impl, handle);
			if (resource) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(resource->image), VK_OBJECT_TYPE_IMAGE, name);
			}
		}

		static void d_setNameSampler(Device* device, SamplerHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNamePipelineLayout(Device* device, PipelineLayoutHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNamePipeline(Device* device, PipelineHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanPipeline* pipeline = VkPipelineState(impl, handle);
			if (pipeline) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(pipeline->pipeline), VK_OBJECT_TYPE_PIPELINE, name);
			}
		}

		static void d_setNameCommandSignature(Device* device, CommandSignatureHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameDescriptorHeap(Device* device, DescriptorHeapHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanDescriptorHeap* heap = VkDescriptorHeapState(impl, handle);
			if (heap && heap->buffer != VK_NULL_HANDLE) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(heap->buffer), VK_OBJECT_TYPE_BUFFER, name);
			}
		}

		static void d_setNameTimeline(Device* device, TimelineHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanTimeline* timeline = VkTimelineState(impl, handle);
			if (timeline) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(timeline->semaphore), VK_OBJECT_TYPE_SEMAPHORE, name);
			}
		}

		static void d_setNameHeap(Device* device, HeapHandle handle, const char* name) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanHeap* heap = VkHeapState(impl, handle);
			if (heap) {
				VkSetObjectName(impl, reinterpret_cast<uint64_t>(heap->memory), VK_OBJECT_TYPE_DEVICE_MEMORY, name);
			}
		}

		static Result d_createPlacedTexture(Device* device, HeapHandle heap, uint64_t offset, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanHeap* heapState = VkHeapState(impl, heap);
			if (!impl || !heapState ||
				(desc.type != ResourceType::Texture1D && desc.type != ResourceType::Texture2D && desc.type != ResourceType::Texture3D) ||
				desc.texture.format == Format::Unknown || desc.texture.width == 0 || desc.texture.height == 0 || desc.texture.mipLevels == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}
			if (desc.texture.initialLayout != ResourceLayout::Undefined && desc.texture.initialLayout != ResourceLayout::Common) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}

			const VkFormat format = ToVkFormat(desc.texture.format);
			const VkImageType imageType = VkImageTypeForResourceType(desc.type);
			const VkSampleCountFlagBits samples = VkSampleCountForDesc(desc.texture.sampleCount);
			if (format == VK_FORMAT_UNDEFINED || imageType == VK_IMAGE_TYPE_MAX_ENUM || samples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) {
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}
			VkImageCreateFlags imageCreateFlags = 0;
			if (!VkImageCreateFlagsForDesc(desc, imageCreateFlags)) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			createInfo.flags = imageCreateFlags;
			createInfo.imageType = imageType;
			createInfo.format = format;
			createInfo.extent.width = desc.texture.width;
			createInfo.extent.height = desc.type == ResourceType::Texture1D ? 1u : desc.texture.height;
			createInfo.extent.depth = desc.type == ResourceType::Texture3D ? desc.texture.depthOrLayers : 1u;
			createInfo.mipLevels = desc.texture.mipLevels;
			createInfo.arrayLayers = desc.type == ResourceType::Texture3D ? 1u : desc.texture.depthOrLayers;
			createInfo.samples = samples;
			createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			createInfo.usage = VkImageUsageForDesc(desc, format);
			createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkImage image = VK_NULL_HANDLE;
			VkResult result = vkCreateImage(impl->device, &createInfo, nullptr, &image);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			VkMemoryRequirements requirements{};
			vkGetImageMemoryRequirements(impl->device, image, &requirements);
			if ((requirements.memoryTypeBits & (1u << heapState->memoryTypeIndex)) == 0 ||
				(offset % requirements.alignment) != 0 ||
				offset + requirements.size > heapState->size) {
				vkDestroyImage(impl->device, image, nullptr);
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			result = vkBindImageMemory(impl->device, image, heapState->memory, offset);
			if (result != VK_SUCCESS) {
				vkDestroyImage(impl->device, image, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			VulkanResource resourceState{};
			resourceState.image = image;
			resourceState.memory = heapState->memory;
			resourceState.format = format;
			resourceState.type = desc.type;
			resourceState.currentLayout = ResourceLayout::Undefined;
			resourceState.width = desc.texture.width;
			resourceState.height = desc.type == ResourceType::Texture1D ? 1u : desc.texture.height;
			resourceState.depthOrLayers = desc.type == ResourceType::Texture3D ? 1u : desc.texture.depthOrLayers;
			resourceState.mipLevels = desc.texture.mipLevels;
			resourceState.ownsImage = true;
			resourceState.ownsMemory = false;

			const ResourceHandle handle = impl->resources.alloc(resourceState);
			Resource texture{ handle, true };
			texture.impl = impl;
			texture.vt = &g_vktex_rvt;
			out = MakeTexturePtr(device, texture, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createPlacedBuffer(Device* device, HeapHandle heap, uint64_t offset, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanHeap* heapState = VkHeapState(impl, heap);
			if (!impl || !heapState || desc.type != ResourceType::Buffer || desc.buffer.sizeBytes == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			VkBufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			createInfo.size = desc.buffer.sizeBytes;
			createInfo.usage = VkBufferUsageForDesc(desc);
			if (impl->bufferDeviceAddressEnabled) {
				createInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
			}
			createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkBuffer buffer = VK_NULL_HANDLE;
			VkResult result = vkCreateBuffer(impl->device, &createInfo, nullptr, &buffer);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			VkMemoryRequirements requirements{};
			vkGetBufferMemoryRequirements(impl->device, buffer, &requirements);
			if ((requirements.memoryTypeBits & (1u << heapState->memoryTypeIndex)) == 0 ||
				(offset % requirements.alignment) != 0 ||
				offset + requirements.size > heapState->size) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			result = vkBindBufferMemory(impl->device, buffer, heapState->memory, offset);
			if (result != VK_SUCCESS) {
				vkDestroyBuffer(impl->device, buffer, nullptr);
				out.Reset();
				return ToRHI(result);
			}

			const VkMemoryPropertyFlags actualFlags = impl->memoryProperties.memoryTypes[heapState->memoryTypeIndex].propertyFlags;
			VulkanResource resourceState{};
			resourceState.buffer = buffer;
			resourceState.memory = heapState->memory;
			resourceState.bufferSize = desc.buffer.sizeBytes;
			resourceState.type = ResourceType::Buffer;
			resourceState.hostVisible = (actualFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
			resourceState.ownsBuffer = true;
			resourceState.ownsMemory = false;
			resourceState.currentLayout = ResourceLayout::Common;
			resourceState.submittedLayout = ResourceLayout::Common;
			if (impl->bufferDeviceAddressEnabled) {
				VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
				addressInfo.buffer = buffer;
				resourceState.deviceAddress = vkGetBufferDeviceAddress(impl->device, &addressInfo);
			}

			const ResourceHandle handle = impl->resources.alloc(resourceState);
			Resource resource{ handle, false };
			resource.impl = impl;
			resource.vt = &g_vkbuf_rvt;
			out = MakeBufferPtr(device, resource, impl->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createPlacedResource(Device* device, HeapHandle heap, uint64_t offset, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			switch (desc.type) {
			case ResourceType::Buffer:
				return d_createPlacedBuffer(device, heap, offset, desc, out);
			case ResourceType::Texture1D:
			case ResourceType::Texture2D:
			case ResourceType::Texture3D:
				return d_createPlacedTexture(device, heap, offset, desc, out);
			case ResourceType::Unknown:
			default:
				VkIgnoreUnused(device, heap, offset);
				out.Reset();
				RHI_FAIL(Result::Unsupported);
			}
		}

		static Result d_createQueryPool(Device* device, const QueryPoolDesc& desc, QueryPoolPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || desc.count == 0) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			const VkQueryType vkType = VkQueryTypeForRHI(desc.type);
			if (vkType == VK_QUERY_TYPE_MAX_ENUM) {
				out.Reset();
				RHI_FAIL(Result::InvalidArgument);
			}

			PipelineStatsMask requestedStats = desc.statsMask;
			VkQueryPipelineStatisticFlags vkStats = 0;
			if (desc.type == QueryType::PipelineStatistics) {
				if (requestedStats == 0 || requestedStats == PS_All) {
					requestedStats = VkSupportedPipelineStatsMask(impl);
				}
				const PipelineStatsMask unsupported = requestedStats & ~VkSupportedPipelineStatsMask(impl);
				if (unsupported != 0 && desc.requireAllStats) {
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}
				requestedStats &= VkSupportedPipelineStatsMask(impl);
				vkStats = VkPipelineStatsToVk(requestedStats);
				if (vkStats == 0) {
					out.Reset();
					RHI_FAIL(Result::Unsupported);
				}
			}

			VkQueryPoolCreateInfo createInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			createInfo.queryType = vkType;
			createInfo.queryCount = desc.count;
			createInfo.pipelineStatistics = vkStats;
			VkQueryPool pool = VK_NULL_HANDLE;
			const VkResult result = vkCreateQueryPool(impl->device, &createInfo, nullptr, &pool);
			if (result != VK_SUCCESS) {
				out.Reset();
				return ToRHI(result);
			}

			const QueryPoolHandle handle = impl->queryPools.alloc(VulkanQueryPool{ pool, desc.type, desc.count, requestedStats, vkStats });
			QueryPool queryPool{ handle };
			queryPool.impl = impl;
			queryPool.vt = &g_vkqpvt;
			out = MakeQueryPoolPtr(device, queryPool, impl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyQueryPool(DeviceDeletionContext* context, QueryPoolHandle handle) noexcept {
			auto* impl = context ? static_cast<VulkanDevice*>(context->impl) : nullptr;
			VulkanQueryPool* queryPool = VkQueryPoolState(impl, handle);
			if (!impl || !queryPool) {
				return;
			}
			if (queryPool->pool != VK_NULL_HANDLE) {
				vkDestroyQueryPool(impl->device, queryPool->pool, nullptr);
			}
			impl->queryPools.free(handle);
		}

		static TimestampCalibration d_getTimestampCalibration(Device* device, QueueKind kind) noexcept {
			VkIgnoreUnused(kind);
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->physicalDevice == VK_NULL_HANDLE || impl->physicalDeviceProperties.limits.timestampPeriod <= 0.0f) {
				return {};
			}

			const double ticksPerSecond = 1000000000.0 / static_cast<double>(impl->physicalDeviceProperties.limits.timestampPeriod);
			TimestampCalibration calibration{};
			calibration.ticksPerSecond = ticksPerSecond > 0.0
				? static_cast<uint64_t>(std::llround(ticksPerSecond))
				: 0ull;
			return calibration;
		}

		static CopyableFootprintsInfo d_getCopyableFootprints(Device* device, const FootprintRangeDesc& range, CopyableFootprint* out, uint32_t outCap) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* texture = VkResourceState(impl, range.texture);
			if (!texture || texture->image == VK_NULL_HANDLE || !out || outCap == 0) {
				return {};
			}

			const uint32_t layers = VkTextureLayerCount(*texture);
			const uint32_t firstMip = (std::min)(range.firstMip, static_cast<uint32_t>(texture->mipLevels - 1));
			const uint32_t mipCount = (std::min)(range.mipCount, static_cast<uint32_t>(texture->mipLevels - firstMip));
			const uint32_t firstLayer = (std::min)(range.firstArraySlice, layers - 1u);
			const uint32_t layerCount = (std::min)(range.arraySize, layers - firstLayer);
			const uint32_t count = mipCount * layerCount;
			if (mipCount == 0 || layerCount == 0 || outCap < count) {
				return {};
			}

			const uint32_t bytesPerPixel = FormatByteSize(FromVkFormat(texture->format));
			if (bytesPerPixel == 0) {
				return {};
			}

			uint64_t offset = range.baseOffset;
			uint32_t written = 0;
			for (uint32_t layer = 0; layer < layerCount; ++layer) {
				for (uint32_t mip = 0; mip < mipCount; ++mip) {
					const uint32_t actualMip = firstMip + mip;
					const uint32_t width = VkMipDim(texture->width, actualMip);
					const uint32_t height = VkMipDim(texture->height, actualMip);
					const uint32_t depth = texture->type == ResourceType::Texture3D ? VkMipDim(texture->depthOrLayers, actualMip) : 1u;
					const uint32_t rowPitch = static_cast<uint32_t>(VkAlignUp(static_cast<uint64_t>(width) * bytesPerPixel, 256));
					offset = VkAlignUp(offset, 512);
					out[written++] = { offset, rowPitch, width, height, depth };
					offset += static_cast<uint64_t>(rowPitch) * height * depth;
				}
			}

			return { written, offset - range.baseOffset };
		}

		static Result d_getResourceAllocationInfo(const Device* device, const ResourceDesc* resources, uint32_t resourceCount, ResourceAllocationInfo* outInfos) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || !resources || resourceCount == 0 || !outInfos) {
				RHI_FAIL(Result::InvalidArgument);
			}

			uint64_t runningOffset = 0;
			for (uint32_t index = 0; index < resourceCount; ++index) {
				const ResourceDesc& desc = resources[index];
				VkMemoryRequirements requirements{};
				if (desc.type == ResourceType::Buffer) {
					VkBufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
					createInfo.size = desc.buffer.sizeBytes;
					createInfo.usage = VkBufferUsageForDesc(desc);
					createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
					VkBuffer buffer = VK_NULL_HANDLE;
					VkResult result = vkCreateBuffer(impl->device, &createInfo, nullptr, &buffer);
					if (result != VK_SUCCESS) {
						return ToRHI(result);
					}
					vkGetBufferMemoryRequirements(impl->device, buffer, &requirements);
					vkDestroyBuffer(impl->device, buffer, nullptr);
				}
				else if (desc.type == ResourceType::Texture1D || desc.type == ResourceType::Texture2D || desc.type == ResourceType::Texture3D) {
					const VkFormat format = ToVkFormat(desc.texture.format);
					const VkImageType imageType = VkImageTypeForResourceType(desc.type);
					const VkSampleCountFlagBits samples = VkSampleCountForDesc(desc.texture.sampleCount);
					if (format == VK_FORMAT_UNDEFINED || imageType == VK_IMAGE_TYPE_MAX_ENUM || samples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) {
						RHI_FAIL(Result::Unsupported);
					}
					VkImageCreateFlags imageCreateFlags = 0;
					if (!VkImageCreateFlagsForDesc(desc, imageCreateFlags)) {
						RHI_FAIL(Result::InvalidArgument);
					}
					VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
					createInfo.flags = imageCreateFlags;
					createInfo.imageType = imageType;
					createInfo.format = format;
					createInfo.extent.width = desc.texture.width;
					createInfo.extent.height = desc.type == ResourceType::Texture1D ? 1u : desc.texture.height;
					createInfo.extent.depth = desc.type == ResourceType::Texture3D ? desc.texture.depthOrLayers : 1u;
					createInfo.mipLevels = desc.texture.mipLevels;
					createInfo.arrayLayers = desc.type == ResourceType::Texture3D ? 1u : desc.texture.depthOrLayers;
					createInfo.samples = samples;
					createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
					createInfo.usage = VkImageUsageForDesc(desc, format);
					createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
					createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					VkImage image = VK_NULL_HANDLE;
					VkResult result = vkCreateImage(impl->device, &createInfo, nullptr, &image);
					if (result != VK_SUCCESS) {
						return ToRHI(result);
					}
					vkGetImageMemoryRequirements(impl->device, image, &requirements);
					vkDestroyImage(impl->device, image, nullptr);
				}
				else {
					RHI_FAIL(Result::InvalidArgument);
				}

				runningOffset = VkAlignUp(runningOffset, requirements.alignment);
				outInfos[index] = { runningOffset, requirements.alignment, requirements.size };
				runningOffset += requirements.size;
			}
			return Result::Ok;
		}

		static Result d_setResidencyPriority(const Device* device, Span<PageableRef> objects, ResidencyPriority priority) noexcept {
			VkIgnoreUnused(priority);
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}
			for (const PageableRef& object : objects) {
				switch (object.kind) {
				case PageableKind::Resource:
					if (!VkResourceState(impl, object.resource)) RHI_FAIL(Result::InvalidArgument);
					break;
				case PageableKind::Heap:
					if (!VkHeapState(impl, object.heap)) RHI_FAIL(Result::InvalidArgument);
					break;
				case PageableKind::DescriptorHeap:
					if (!VkDescriptorHeapState(impl, object.descHeap)) RHI_FAIL(Result::InvalidArgument);
					break;
				case PageableKind::QueryPool:
					if (!VkQueryPoolState(impl, object.queryPool)) RHI_FAIL(Result::InvalidArgument);
					break;
				case PageableKind::Pipeline:
					if (!VkPipelineState(impl, object.pipeline)) RHI_FAIL(Result::InvalidArgument);
					break;
				default:
					RHI_FAIL(Result::InvalidArgument);
				}
			}
			return Result::Ok;
		}

		static void d_checkDebugMessages(Device* device) noexcept {
			VkIgnoreUnused(device);
		}

		static Result d_getDebugInstrumentationCapabilities(const Device* device, DebugInstrumentationCapabilities& out) noexcept {
			VkIgnoreUnused(device);
			out = {};
			return Result::Ok;
		}

		static Result d_getDebugInstrumentationState(const Device* device, DebugInstrumentationState& out) noexcept {
			VkIgnoreUnused(device);
			out = {};
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationFeatureCount(const Device* device) noexcept {
			VkIgnoreUnused(device);
			return 0;
		}

		static Result d_copyDebugInstrumentationFeatures(const Device* device, uint32_t first, DebugInstrumentationFeature* features, uint32_t capacity, uint32_t* copied) noexcept {
			VkIgnoreUnused(device, first, features, capacity);
			if (copied) {
				*copied = 0;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationPipelineCount(const Device* device) noexcept {
			VkIgnoreUnused(device);
			return 0;
		}

		static Result d_copyDebugInstrumentationPipelines(const Device* device, uint32_t first, DebugInstrumentationPipeline* pipelines, uint32_t capacity, uint32_t* copied) noexcept {
			VkIgnoreUnused(device, first, pipelines, capacity);
			if (copied) {
				*copied = 0;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationPipelineUsageCount(const Device* device) noexcept {
			VkIgnoreUnused(device);
			return 0;
		}

		static Result d_copyDebugInstrumentationPipelineUsages(const Device* device, uint32_t first, DebugInstrumentationPipelineUsage* usages, uint32_t capacity, uint32_t* copied) noexcept {
			VkIgnoreUnused(device, first, usages, capacity);
			if (copied) {
				*copied = 0;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationDiagnosticCount(const Device* device) noexcept {
			VkIgnoreUnused(device);
			return 0;
		}

		static Result d_copyDebugInstrumentationDiagnostics(const Device* device, uint32_t first, DebugInstrumentationDiagnostic* diagnostics, uint32_t capacity, uint32_t* copied) noexcept {
			VkIgnoreUnused(device, first, diagnostics, capacity);
			if (copied) {
				*copied = 0;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationIssueCount(const Device* device) noexcept {
			VkIgnoreUnused(device);
			return 0;
		}

		static Result d_copyDebugInstrumentationIssues(const Device* device, uint32_t first, DebugInstrumentationIssue* issues, uint32_t capacity, uint32_t* copied) noexcept {
			VkIgnoreUnused(device, first, issues, capacity);
			if (copied) {
				*copied = 0;
			}
			return Result::Ok;
		}

		static Result d_setDebugGlobalInstrumentationMask(Device* device, uint64_t featureMask) noexcept {
			VkIgnoreUnused(device, featureMask);
			RHI_FAIL(Result::Unsupported);
		}

		static Result d_setDebugPipelineInstrumentationMask(Device* device, uint64_t pipelineUid, uint64_t featureMask) noexcept {
			VkIgnoreUnused(device, pipelineUid, featureMask);
			RHI_FAIL(Result::Unsupported);
		}

		static Result d_setDebugSynchronousRecording(Device* device, bool enabled) noexcept {
			VkIgnoreUnused(device, enabled);
			RHI_FAIL(Result::Unsupported);
		}

		static Result d_setDebugTexelAddressing(Device* device, bool enabled) noexcept {
			VkIgnoreUnused(device, enabled);
			RHI_FAIL(Result::Unsupported);
		}
	} // namespace

	void VulkanDevice::Shutdown() noexcept {
		if (device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(device);
		}

		for (auto& slot : descriptorHeaps.slots) {
			if (!slot.alive) {
				continue;
			}

			for (VulkanImageViewSlot& descriptorSlot : slot.obj.imageViewSlots) {
				VkResetDescriptorSlot(this, descriptorSlot);
			}
			VkDestroyDescriptorHeapBacking(this, slot.obj);
			slot.obj = VulkanDescriptorHeap{};
			slot.alive = false;
		}
		descriptorHeaps.clear();

		for (auto& slot : swapchains.slots) {
			if (!slot.alive) {
				continue;
			}

			VkReleaseSwapchainImageHandles(this, slot.obj);
			VkReleaseSwapchainPresentSemaphores(this, slot.obj);
			if (device != VK_NULL_HANDLE && slot.obj.acquireFence != VK_NULL_HANDLE) {
				vkDestroyFence(device, slot.obj.acquireFence, nullptr);
			}
			if (device != VK_NULL_HANDLE && slot.obj.swapchain != VK_NULL_HANDLE) {
				vkDestroySwapchainKHR(device, slot.obj.swapchain, nullptr);
			}
			if (instance != VK_NULL_HANDLE && slot.obj.surface != VK_NULL_HANDLE) {
				vkDestroySurfaceKHR(instance, slot.obj.surface, nullptr);
			}
			slot.obj = VulkanSwapchain{};
			slot.alive = false;
		}
		swapchains.clear();

		for (auto& slot : resources.slots) {
			if (!slot.alive) {
				continue;
			}
			VkDestroyResource(this, slot.obj);
			slot.alive = false;
		}
		resources.clear();

		for (auto& slot : pipelines.slots) {
			if (!slot.alive) {
				continue;
			}
			VkDestroyPipeline(this, slot.obj);
			slot.alive = false;
		}
		pipelines.clear();

		for (auto& slot : pipelineLayouts.slots) {
			if (!slot.alive) {
				continue;
			}
			slot.obj = VulkanPipelineLayout{};
			slot.alive = false;
		}
		pipelineLayouts.clear();

		for (auto& slot : commandSignatures.slots) {
			if (!slot.alive) {
				continue;
			}
			VkDestroyCommandSignature(this, slot.obj);
			slot.alive = false;
		}
		commandSignatures.clear();

		for (auto& slot : commandLists.slots) {
			if (slot.alive) {
				slot.obj = VulkanCommandList{};
				slot.alive = false;
			}
		}
		commandLists.clear();

		if (device != VK_NULL_HANDLE) {
			for (auto& slot : allocators.slots) {
				if (slot.alive && slot.obj.pool != VK_NULL_HANDLE) {
					vkDestroyCommandPool(device, slot.obj.pool, nullptr);
					slot.obj.pool = VK_NULL_HANDLE;
					slot.alive = false;
				}
			}
		}
		allocators.clear();

		if (device != VK_NULL_HANDLE) {
			vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}

		if (instance != VK_NULL_HANDLE) {
			vkDestroyInstance(instance, nullptr);
			instance = VK_NULL_HANDLE;
		}

		physicalDevice = VK_NULL_HANDLE;
		physicalDeviceProperties = {};
		memoryProperties = {};
		supportedFeatures = {};
		queueFamilyProperties.clear();
		queues = {};
		self = {};
	}

	const DeviceVTable g_vkdevvt = {
		&d_createPipelineFromStream,
		&d_createWorkGraph,
		&d_createPipelineLayout,
		&d_createCommandSignature,
		&d_createCommandAllocator,
		&d_createCommandList,
		&d_createSwapchain,
		&d_createDescriptorHeap,
		&d_createConstantBufferView,
		&d_createShaderResourceView,
		&d_createUnorderedAccessView,
		&d_createRenderTargetView,
		&d_createDepthStencilView,
		&d_createSampler,
		&d_createCommittedResource,
		&d_createTimeline,
		&d_createHeap,
		&d_createPlacedResource,
		&d_createQueryPool,

		&d_destroySampler,
		&d_destroyPipelineLayout,
		&d_destroyPipeline,
		&d_destroyWorkGraph,
		&d_destroyCommandSignature,
		&d_destroyCommandAllocator,
		&d_destroyCommandList,
		&d_destroySwapchain,
		&d_destroyDescriptorHeap,
		&d_destroyBuffer,
		&d_destroyTexture,
		&d_destroyTimeline,
		&d_destroyHeap,
		&d_destroyQueryPool,

		&d_getQueue,
		&d_createQueue,
		&d_destroyQueue,
		&d_waitIdle,
		&d_flushDeletionQueue,
		&d_getDescriptorHandleIncrementSize,
		&d_getTimestampCalibration,
		&d_getCopyableFootprints,
		&d_getResourceAllocationInfo,
		&d_queryFeatureInfo,
		&d_setResidencyPriority,
		&d_queryVideoMemoryInfo,

		&d_setNameBuffer,
		&d_setNameTexture,
		&d_setNameSampler,
		&d_setNamePipelineLayout,
		&d_setNamePipeline,
		&d_setNameCommandSignature,
		&d_setNameDescriptorHeap,
		&d_setNameTimeline,
		&d_setNameHeap,
		&d_checkDebugMessages,
		&d_getDebugInstrumentationCapabilities,
		&d_getDebugInstrumentationState,
		&d_getDebugInstrumentationFeatureCount,
		&d_copyDebugInstrumentationFeatures,
		&d_getDebugInstrumentationPipelineCount,
		&d_copyDebugInstrumentationPipelines,
		&d_getDebugInstrumentationPipelineUsageCount,
		&d_copyDebugInstrumentationPipelineUsages,
		&d_getDebugInstrumentationDiagnosticCount,
		&d_copyDebugInstrumentationDiagnostics,
		&d_getDebugInstrumentationIssueCount,
		&d_copyDebugInstrumentationIssues,
		&d_setDebugGlobalInstrumentationMask,
		&d_setDebugPipelineInstrumentationMask,
		&d_setDebugSynchronousRecording,
		&d_setDebugTexelAddressing,
		&d_destroyDevice,
		9u
	};

	const QueueVTable g_vkqvt = {
		&q_submit,
		&q_signal,
		&q_wait,
		&q_checkDebugMessages,
		&q_setName,
		2u
	};

	const CommandAllocatorVTable g_vkcalvt = {
		&ca_reset,
		1u
	};

	const CommandListVTable g_vkclvt = {
		&cl_end,
		&cl_reset,
		&cl_beginPass,
		&cl_endPass,
		&cl_barrier,
		&cl_bindLayout,
		&cl_bindPipeline,
		&cl_setVB,
		&cl_setIB,
		&cl_draw,
		&cl_drawIndexed,
		&cl_dispatch,
		&cl_clearRTV_slot,
		&cl_clearDSV_slot,
		&cl_executeIndirect,
		&cl_setDescriptorHeaps,
		&cl_clearUavUint,
		&cl_clearUavFloat,
		&cl_copyTextureToBuffer,
		&cl_copyBufferToTexture,
		&cl_copyTextureRegion,
		&cl_copyBufferRegion,
		&cl_writeTimestamp,
		&cl_beginQuery,
		&cl_endQuery,
		&cl_resolveQueryData,
		&cl_resetQueries,
		&cl_pushConstants,
		&cl_setPrimitiveTopology,
		&cl_dispatchMesh,
		&cl_setWorkGraph,
		&cl_dispatchWorkGraph,
		&cl_setName,
		&cl_setDebugInstrumentationContext,
		2u
	};

	const SwapchainVTable g_vkscvt = {
		&sc_count,
		&sc_curr,
		&sc_img,
		&sc_present,
		&sc_resizeBuffers,
		&sc_setName,
		1u
	};

	const ResourceVTable g_vkbuf_rvt = {
		&buf_map,
		&buf_unmap,
		&buf_setName,
		1u
	};

	const ResourceVTable g_vktex_rvt = {
		&tex_map,
		&tex_unmap,
		&tex_setName,
		1u
	};

	const QueryPoolVTable g_vkqpvt = {
		&qp_getQueryResultInfo,
		&qp_getPipelineStatsLayout,
		&qp_setName,
		1u
	};

	const PipelineVTable g_vkpsovt = {
		&pso_setName,
		1u
	};

	const WorkGraphVTable g_vkwgvt = {
		&wg_setName,
		&wg_getRequiredScratchMemorySize,
		1u
	};

	const PipelineLayoutVTable g_vkplvt = {
		&pl_setName,
		1u
	};

	const CommandSignatureVTable g_vkcsvt = {
		&cs_setName,
		1u
	};

	const DescriptorHeapVTable g_vkdhvt = {
		&dh_setName,
		1u
	};

	const SamplerVTable g_vksvt = {
		&s_setName,
		1u
	};

	const TimelineVTable g_vktlvt = {
		&tl_timelineCompletedValue,
		&tl_timelineHostWait,
		&tl_setName,
		1u
	};

	const HeapVTable g_vkhevt = {
		&h_setName,
		1u
	};

	rhi::Result CreateVulkanDevice(const DeviceCreateInfo& ci, DevicePtr& outPtr) noexcept {
		outPtr = {};
		if (ci.backend != Backend::Vulkan) {
			RHI_FAIL(Result::InvalidArgument);
		}

		const VkResult volkInit = volkInitialize();
		if (volkInit != VK_SUCCESS) {
			spdlog::error("CreateVulkanDevice: volkInitialize failed with VkResult {}", static_cast<int>(volkInit));
			RHI_FAIL(Result::SdkComponentMissing);
		}

		const uint32_t loaderApiVersion = (std::max)(volkGetInstanceVersion(), VK_API_VERSION_1_0);
		const uint32_t requestedApiVersion = (std::min)(loaderApiVersion, VK_API_VERSION_1_3);

		std::vector<const char*> enabledLayers;
		std::vector<const char*> enabledExtensions;
		if (VkHasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)) {
			enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		}
	#ifdef _WIN32
		if (VkHasInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
			enabledExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
		}
	#endif
		if (ci.enableDebug) {
			if (VkHasInstanceLayer("VK_LAYER_KHRONOS_validation")) {
				enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
			}
			else {
				spdlog::warn("CreateVulkanDevice: validation layer VK_LAYER_KHRONOS_validation not available.");
			}

			if (VkHasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
				enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			}
		}

		VkApplicationInfo applicationInfo{};
		applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		applicationInfo.pApplicationName = "BasicRenderer";
		applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
		applicationInfo.pEngineName = "BasicRHI";
		applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
		applicationInfo.apiVersion = requestedApiVersion;

		VkInstanceCreateInfo instanceCreateInfo{};
		instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceCreateInfo.pApplicationInfo = &applicationInfo;
		instanceCreateInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
		instanceCreateInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
		instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
		instanceCreateInfo.ppEnabledExtensionNames = enabledExtensions.empty() ? nullptr : enabledExtensions.data();

		VkInstance instance = VK_NULL_HANDLE;
		VkResult result = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);
		if (result != VK_SUCCESS) {
			spdlog::error("CreateVulkanDevice: vkCreateInstance failed with VkResult {}", static_cast<int>(result));
			return ToRHI(result);
		}

		volkLoadInstance(instance);

		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		VkPhysicalDeviceFeatures supportedFeatures{};
		VkPhysicalDeviceVulkan12Features supportedVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features supportedVulkan13Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorHeapFeaturesEXT supportedDescriptorHeapFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
		VkPhysicalDeviceMeshShaderFeaturesEXT supportedMeshShaderFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
		VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT supportedDeviceGeneratedCommandsFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
		VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR supportedComputeShaderDerivativesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR };
		VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT supportedShaderImageAtomicInt64Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };
		VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT supportedShaderSubgroupPartitionedFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_PARTITIONED_FEATURES_EXT };
		VkPhysicalDeviceFeatures2 supportedFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		supportedFeatures2.pNext = &supportedVulkan12Features;
		supportedVulkan12Features.pNext = &supportedVulkan13Features;
		supportedVulkan13Features.pNext = &supportedDescriptorHeapFeatures;
		supportedDescriptorHeapFeatures.pNext = &supportedMeshShaderFeatures;
		supportedMeshShaderFeatures.pNext = &supportedDeviceGeneratedCommandsFeatures;
		supportedDeviceGeneratedCommandsFeatures.pNext = &supportedComputeShaderDerivativesFeatures;
		supportedComputeShaderDerivativesFeatures.pNext = &supportedShaderImageAtomicInt64Features;
		supportedShaderImageAtomicInt64Features.pNext = &supportedShaderSubgroupPartitionedFeatures;
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
		VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR computeShaderDerivativesProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_PROPERTIES_KHR };
		VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		properties2.pNext = &descriptorHeapProperties;
		descriptorHeapProperties.pNext = &meshShaderProperties;
		meshShaderProperties.pNext = &computeShaderDerivativesProperties;
		std::vector<VkQueueFamilyProperties> queueFamilyProperties;
		std::array<uint32_t, 3> selectedQueueFamilies{};
		if (!VkSelectPhysicalDeviceAndQueues(
			instance,
			physicalDevice,
			physicalDeviceProperties,
			memoryProperties,
			supportedFeatures,
			queueFamilyProperties,
			selectedQueueFamilies)) {
			vkDestroyInstance(instance, nullptr);
			RHI_FAIL(Result::NotFound);
		}

		std::vector<uint32_t> uniqueQueueFamilies;
		for (uint32_t familyIndex : selectedQueueFamilies) {
			if (familyIndex == kVkInvalidQueueFamily) {
				continue;
			}
			if (std::find(uniqueQueueFamilies.begin(), uniqueQueueFamilies.end(), familyIndex) == uniqueQueueFamilies.end()) {
				uniqueQueueFamilies.push_back(familyIndex);
			}
		}

		const float queuePriority = 1.0f;
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		queueCreateInfos.reserve(uniqueQueueFamilies.size());
		for (uint32_t familyIndex : uniqueQueueFamilies) {
			VkDeviceQueueCreateInfo queueCreateInfo{};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = familyIndex;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueCreateInfo);
		}

		const bool enableSwapchainExtension = VkHasDeviceExtension(physicalDevice, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		const bool hasMaintenance5Extension = VkHasDeviceExtension(physicalDevice, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
		const bool hasDescriptorHeapExtension = VkHasDeviceExtension(physicalDevice, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		const bool hasMeshShaderExtension = VkHasDeviceExtension(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME);
		const bool hasDeviceGeneratedCommandsExtension = VkHasDeviceExtension(physicalDevice, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
		const bool hasComputeShaderDerivativesExtension = VkHasDeviceExtension(physicalDevice, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
		const bool hasNvComputeShaderDerivativesExtension = !hasComputeShaderDerivativesExtension && VkHasDeviceExtension(physicalDevice, VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
		const bool hasShaderImageAtomicInt64Extension = VkHasDeviceExtension(physicalDevice, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
		const bool hasShaderSubgroupPartitionedExtension = VkHasDeviceExtension(physicalDevice, VK_EXT_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME);
		const bool hasNvShaderSubgroupPartitionedExtension = !hasShaderSubgroupPartitionedExtension && VkHasDeviceExtension(physicalDevice, VK_NV_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME);
		std::vector<const char*> enabledDeviceExtensions;
		if (enableSwapchainExtension) {
			enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		}
		if (hasMaintenance5Extension && (hasDescriptorHeapExtension || hasDeviceGeneratedCommandsExtension)) {
			enabledDeviceExtensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
		}
		if (hasDescriptorHeapExtension && hasMaintenance5Extension) {
			enabledDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		}
		if (hasMeshShaderExtension) {
			enabledDeviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
		}
		if (hasComputeShaderDerivativesExtension) {
			enabledDeviceExtensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
		}
		else if (hasNvComputeShaderDerivativesExtension) {
			enabledDeviceExtensions.push_back(VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
		}
		vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures2);
		vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
		supportedFeatures = supportedFeatures2.features;

		VkPhysicalDeviceVulkan12Features enabledVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features enabledVulkan13Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorHeapFeaturesEXT enabledDescriptorHeapFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
		VkPhysicalDeviceMeshShaderFeaturesEXT enabledMeshShaderFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
		VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT enabledDeviceGeneratedCommandsFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
		VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR enabledComputeShaderDerivativesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR };
		VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT enabledShaderImageAtomicInt64Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };
		VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT enabledShaderSubgroupPartitionedFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_PARTITIONED_FEATURES_EXT };
		enabledVulkan12Features.pNext = &enabledVulkan13Features;
		enabledVulkan13Features.pNext = &enabledDescriptorHeapFeatures;
		enabledDescriptorHeapFeatures.pNext = &enabledMeshShaderFeatures;
		enabledMeshShaderFeatures.pNext = &enabledDeviceGeneratedCommandsFeatures;
		enabledDeviceGeneratedCommandsFeatures.pNext = &enabledComputeShaderDerivativesFeatures;
		enabledComputeShaderDerivativesFeatures.pNext = &enabledShaderImageAtomicInt64Features;
		enabledShaderImageAtomicInt64Features.pNext = &enabledShaderSubgroupPartitionedFeatures;
		if (supportedVulkan12Features.bufferDeviceAddress == VK_TRUE) {
			enabledVulkan12Features.bufferDeviceAddress = VK_TRUE;
		}
		if (supportedVulkan13Features.dynamicRendering == VK_TRUE) {
			enabledVulkan13Features.dynamicRendering = VK_TRUE;
		}
		if (supportedVulkan13Features.shaderDemoteToHelperInvocation == VK_TRUE) {
			enabledVulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;
		}
		if (supportedVulkan12Features.timelineSemaphore == VK_TRUE) {
			enabledVulkan12Features.timelineSemaphore = VK_TRUE;
		}
		if (supportedVulkan12Features.descriptorIndexing == VK_TRUE) {
			enabledVulkan12Features.descriptorIndexing = VK_TRUE;
		}
		if (supportedVulkan12Features.shaderUniformBufferArrayNonUniformIndexing == VK_TRUE) {
			enabledVulkan12Features.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
		}
		if (supportedVulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE) {
			enabledVulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		}
		if (supportedVulkan12Features.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE) {
			enabledVulkan12Features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
		}
		if (supportedVulkan12Features.shaderStorageImageArrayNonUniformIndexing == VK_TRUE) {
			enabledVulkan12Features.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
		}
		if (supportedVulkan12Features.descriptorBindingPartiallyBound == VK_TRUE) {
			enabledVulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
		}
		if (supportedVulkan12Features.runtimeDescriptorArray == VK_TRUE) {
			enabledVulkan12Features.runtimeDescriptorArray = VK_TRUE;
		}
		if (supportedVulkan12Features.scalarBlockLayout == VK_TRUE) {
			enabledVulkan12Features.scalarBlockLayout = VK_TRUE;
		}
		if (hasDescriptorHeapExtension &&
			hasMaintenance5Extension &&
			supportedDescriptorHeapFeatures.descriptorHeap == VK_TRUE &&
			enabledVulkan12Features.bufferDeviceAddress == VK_TRUE &&
			enabledVulkan12Features.runtimeDescriptorArray == VK_TRUE &&
			enabledVulkan12Features.scalarBlockLayout == VK_TRUE) {
			enabledDescriptorHeapFeatures.descriptorHeap = VK_TRUE;
		}
		if (hasMeshShaderExtension && supportedMeshShaderFeatures.meshShader == VK_TRUE) {
			enabledMeshShaderFeatures.meshShader = VK_TRUE;
			enabledMeshShaderFeatures.taskShader = supportedMeshShaderFeatures.taskShader;
			enabledMeshShaderFeatures.meshShaderQueries = supportedMeshShaderFeatures.meshShaderQueries;
		}
		if (hasDeviceGeneratedCommandsExtension && hasMaintenance5Extension && supportedDeviceGeneratedCommandsFeatures.deviceGeneratedCommands == VK_TRUE) {
			enabledDeviceExtensions.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
			enabledDeviceGeneratedCommandsFeatures.deviceGeneratedCommands = VK_TRUE;
			enabledDeviceGeneratedCommandsFeatures.dynamicGeneratedPipelineLayout = supportedDeviceGeneratedCommandsFeatures.dynamicGeneratedPipelineLayout;
		}
		if ((hasComputeShaderDerivativesExtension || hasNvComputeShaderDerivativesExtension) &&
			supportedComputeShaderDerivativesFeatures.computeDerivativeGroupQuads == VK_TRUE) {
			enabledComputeShaderDerivativesFeatures.computeDerivativeGroupQuads = VK_TRUE;
			enabledComputeShaderDerivativesFeatures.computeDerivativeGroupLinear = supportedComputeShaderDerivativesFeatures.computeDerivativeGroupLinear;
		}
		if (hasShaderImageAtomicInt64Extension && supportedShaderImageAtomicInt64Features.shaderImageInt64Atomics == VK_TRUE) {
			enabledDeviceExtensions.push_back(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
			enabledShaderImageAtomicInt64Features.shaderImageInt64Atomics = VK_TRUE;
		}
		if (hasShaderSubgroupPartitionedExtension && supportedShaderSubgroupPartitionedFeatures.shaderSubgroupPartitioned == VK_TRUE) {
			enabledDeviceExtensions.push_back(VK_EXT_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME);
			enabledShaderSubgroupPartitionedFeatures.shaderSubgroupPartitioned = VK_TRUE;
		}
		else if (hasNvShaderSubgroupPartitionedExtension) {
			enabledDeviceExtensions.push_back(VK_NV_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME);
		}

		VkDeviceCreateInfo deviceCreateInfo{};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
		deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
		deviceCreateInfo.pEnabledFeatures = &supportedFeatures;
		deviceCreateInfo.pNext = &enabledVulkan12Features;
		deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
		deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.empty() ? nullptr : enabledDeviceExtensions.data();

		VkDevice device = VK_NULL_HANDLE;
		result = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device);
		if (result != VK_SUCCESS) {
			spdlog::error("CreateVulkanDevice: vkCreateDevice failed with VkResult {}", static_cast<int>(result));
			vkDestroyInstance(instance, nullptr);
			return ToRHI(result);
		}

		volkLoadDevice(device);

		auto impl = std::make_shared<VulkanDevice>();
		impl->selfWeak = impl;
		impl->instance = instance;
		impl->physicalDevice = physicalDevice;
		impl->device = device;
		impl->physicalDeviceProperties = physicalDeviceProperties;
		impl->memoryProperties = memoryProperties;
		impl->supportedFeatures = supportedFeatures;
		impl->descriptorHeapProperties = descriptorHeapProperties;
		impl->meshShaderProperties = meshShaderProperties;
		impl->computeShaderDerivativesProperties = computeShaderDerivativesProperties;
		impl->bufferDeviceAddressEnabled = enabledVulkan12Features.bufferDeviceAddress == VK_TRUE;
		impl->timelineSemaphoreEnabled = enabledVulkan12Features.timelineSemaphore == VK_TRUE;
		impl->descriptorIndexingEnabled = enabledVulkan12Features.descriptorIndexing == VK_TRUE;
		impl->runtimeDescriptorArrayEnabled = enabledVulkan12Features.runtimeDescriptorArray == VK_TRUE;
		impl->scalarBlockLayoutEnabled = enabledVulkan12Features.scalarBlockLayout == VK_TRUE;
		impl->descriptorHeapEnabled = enabledDescriptorHeapFeatures.descriptorHeap == VK_TRUE;
		impl->meshShaderEnabled = enabledMeshShaderFeatures.meshShader == VK_TRUE;
		impl->taskShaderEnabled = enabledMeshShaderFeatures.taskShader == VK_TRUE;
		impl->meshShaderPipelineStatsEnabled = enabledMeshShaderFeatures.meshShaderQueries == VK_TRUE;
		impl->deviceGeneratedCommandsEnabled = enabledDeviceGeneratedCommandsFeatures.deviceGeneratedCommands == VK_TRUE;
		impl->dynamicGeneratedPipelineLayoutEnabled = enabledDeviceGeneratedCommandsFeatures.dynamicGeneratedPipelineLayout == VK_TRUE;
		impl->dynamicRenderingEnabled = enabledVulkan13Features.dynamicRendering == VK_TRUE;
		impl->shaderDemoteToHelperInvocationEnabled = enabledVulkan13Features.shaderDemoteToHelperInvocation == VK_TRUE;
		impl->computeDerivativeGroupQuadsEnabled = enabledComputeShaderDerivativesFeatures.computeDerivativeGroupQuads == VK_TRUE;
		impl->computeDerivativeGroupLinearEnabled = enabledComputeShaderDerivativesFeatures.computeDerivativeGroupLinear == VK_TRUE;
		impl->shaderImageInt64AtomicsEnabled = enabledShaderImageAtomicInt64Features.shaderImageInt64Atomics == VK_TRUE;
		impl->shaderSubgroupPartitionedEnabled = enabledShaderSubgroupPartitionedFeatures.shaderSubgroupPartitioned == VK_TRUE || hasNvShaderSubgroupPartitionedExtension;
		impl->validateBarrierTransitions = ci.validateBarrierTransitions;
		impl->loaderApiVersion = loaderApiVersion;
		impl->instanceApiVersion = requestedApiVersion;
		impl->swapchainExtensionEnabled = enableSwapchainExtension;
		impl->queueFamilyProperties = std::move(queueFamilyProperties);
		for (uint32_t queueSlot = 0; queueSlot < 3; ++queueSlot) {
			const uint32_t familyIndex = selectedQueueFamilies[queueSlot];
			if (familyIndex == kVkInvalidQueueFamily) {
				continue;
			}

			vkGetDeviceQueue(device, familyIndex, 0, &impl->queues[queueSlot].queue);
			impl->queues[queueSlot].familyIndex = familyIndex;
			impl->queues[queueSlot].queueIndex = 0;
		}
		impl->self = Device{ impl.get(), &g_vkdevvt };

		spdlog::info(
			"CreateVulkanDevice: selected device '{}' api {}.{}.{} queueFamilies[gfx={}, compute={}, copy={}] dynamicRendering={} bufferDeviceAddress={} descriptorIndexing={} runtimeDescriptorArray={} descriptorHeap={} meshShader={} taskShader={} deviceGeneratedCommands={} dynamicGeneratedPipelineLayout={} computeDerivativeGroupQuads={} computeDerivativeGroupLinear={} validateBarrierTransitions={}",
			impl->physicalDeviceProperties.deviceName,
			VK_API_VERSION_MAJOR(impl->physicalDeviceProperties.apiVersion),
			VK_API_VERSION_MINOR(impl->physicalDeviceProperties.apiVersion),
			VK_API_VERSION_PATCH(impl->physicalDeviceProperties.apiVersion),
			impl->queues[0].familyIndex,
			impl->queues[1].familyIndex,
			impl->queues[2].familyIndex,
			impl->dynamicRenderingEnabled,
			impl->bufferDeviceAddressEnabled,
			impl->descriptorIndexingEnabled,
			impl->runtimeDescriptorArrayEnabled,
			impl->descriptorHeapEnabled,
			impl->meshShaderEnabled,
			impl->taskShaderEnabled,
			impl->deviceGeneratedCommandsEnabled,
			impl->dynamicGeneratedPipelineLayoutEnabled,
			impl->computeDerivativeGroupQuadsEnabled,
			impl->computeDerivativeGroupLinearEnabled,
			impl->validateBarrierTransitions);

		outPtr = MakeDevicePtr(&impl->self, impl);
		return Result::Ok;
	}

} // namespace rhi
