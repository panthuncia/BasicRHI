#include "rhi_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <spdlog/spdlog.h>

namespace rhi {
	namespace {
		static constexpr uint32_t kVkInvalidQueueFamily = 0xFFFFFFFFu;

		template <typename... TArgs>
		constexpr void VkIgnoreUnused(TArgs&&...) noexcept {}

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
			case Format::R16G16B16A16_Float:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::R16G16_Float:
				return VK_FORMAT_R16G16_SFLOAT;
			case Format::R8G8B8A8_UNorm:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8G8B8A8_UNorm_sRGB:
				return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::B8G8R8A8_UNorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8_UNorm_sRGB:
				return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R32_Float:
				return VK_FORMAT_R32_SFLOAT;
			case Format::D32_Float:
				return VK_FORMAT_D32_SFLOAT;
			default:
				return VK_FORMAT_UNDEFINED;
			}
		}

		static Format FromVkFormat(VkFormat format) noexcept {
			switch (format) {
			case VK_FORMAT_R8G8B8A8_UNORM:
				return Format::R8G8B8A8_UNorm;
			case VK_FORMAT_R8G8B8A8_SRGB:
				return Format::R8G8B8A8_UNorm_sRGB;
			case VK_FORMAT_B8G8R8A8_UNORM:
				return Format::B8G8R8A8_UNorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return Format::B8G8R8A8_UNorm_sRGB;
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

			return ToRHI(vkAllocateCommandBuffers(device, &allocateInfo, &outCommandBuffer));
		}

		static Result VkBeginCommandRecording(VkCommandBuffer commandBuffer) noexcept {
			VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			return ToRHI(vkBeginCommandBuffer(commandBuffer, &beginInfo));
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
				return Result::InvalidArgument;
			}

			VkResetDescriptorSlot(impl, *descriptorSlot);

			if (impl->descriptorHeapEnabled && heap->buffer != VK_NULL_HANDLE && resource->deviceAddress != 0) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					return Result::InvalidArgument;
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
			case ResourceLayout::Present:
				return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case ResourceLayout::RenderTarget:
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ResourceLayout::DepthReadWrite:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case ResourceLayout::DepthRead:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			case ResourceLayout::ShaderResource:
			case ResourceLayout::GenericRead:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ResourceLayout::CopyDest:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceLayout::CopySource:
				return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
				accessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				stageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
				accessMask = VK_ACCESS_TRANSFER_READ_BIT;
				break;
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
				return Result::InvalidArgument;
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

		static Result VkAcquireNextSwapchainImage(VulkanDevice* impl, VulkanSwapchain& swapchain) noexcept {
			if (!impl || impl->device == VK_NULL_HANDLE || swapchain.swapchain == VK_NULL_HANDLE || swapchain.acquireFence == VK_NULL_HANDLE) {
				return Result::InvalidArgument;
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
				return Result::Unsupported;
			}

			VkBool32 graphicsPresentSupported = VK_FALSE;
			VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(impl->physicalDevice, impl->queues[0].familyIndex, swapchain.surface, &graphicsPresentSupported);
			if (result != VK_SUCCESS) {
				return ToRHI(result);
			}
			if (graphicsPresentSupported != VK_TRUE) {
				return Result::Unsupported;
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
				return Result::Unsupported;
			}

			const VkExtent2D extent = VkChooseSwapchainExtent(capabilities, width, height);
			const uint32_t imageCount = VkClampSwapchainImageCount(capabilities, bufferCount);
			const VkPresentModeKHR presentMode = VkChoosePresentMode(presentModes, allowTearing);

			std::vector<ResourceHandle> oldImageHandles = std::move(swapchain.imageHandles);
			std::vector<VkImage> oldImages = std::move(swapchain.images);
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
					return ToRHI(result);
				}
			}

			const Result populateResult = VkPopulateSwapchainImages(impl, swapchain);
			if (populateResult != Result::Ok) {
				VkReleaseSwapchainImageHandles(impl, swapchain);
				vkDestroySwapchainKHR(impl->device, newSwapchain, nullptr);
				swapchain.swapchain = oldSwapchain;
				swapchain.imageHandles = std::move(oldImageHandles);
				swapchain.images = std::move(oldImages);
				return populateResult;
			}

			for (const ResourceHandle handle : oldImageHandles) {
				if (VulkanResource* resource = VkResourceState(impl, handle)) {
					VkDestroyResource(impl, *resource);
				}
				impl->resources.free(handle);
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
			VkIgnoreUnused(qp);
			return {};
		}

		static PipelineStatsLayout qp_getPipelineStatsLayout(QueryPool* qp, PipelineStatsFieldDesc* outBuf, uint32_t outCap) noexcept {
			VkIgnoreUnused(qp, outBuf, outCap);
			return {};
		}

		static void qp_setName(QueryPool* qp, const char* name) noexcept {
			VkIgnoreUnused(qp, name);
		}

		static void pso_setName(Pipeline* pipeline, const char* name) noexcept {
			VkIgnoreUnused(pipeline, name);
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
			VkIgnoreUnused(heap, name);
		}

		static void s_setName(Sampler* sampler, const char* name) noexcept {
			VkIgnoreUnused(sampler, name);
		}

		static uint64_t tl_timelineCompletedValue(Timeline* timeline) noexcept {
			VkIgnoreUnused(timeline);
			return 0;
		}

		static Result tl_timelineHostWait(Timeline* timeline, const uint64_t value, uint32_t timeoutMs) noexcept {
			VkIgnoreUnused(timeline, value, timeoutMs);
			return Result::Unsupported;
		}

		static void tl_setName(Timeline* timeline, const char* name) noexcept {
			VkIgnoreUnused(timeline, name);
		}

		static void h_setName(Heap* heap, const char* name) noexcept {
			VkIgnoreUnused(heap, name);
		}

		static Result q_submit(Queue* queue, Span<CommandList> lists, const SubmitDesc& submit) noexcept {
			auto* impl = queue ? static_cast<VulkanDevice*>(queue->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				return Result::InvalidArgument;
			}

			if (submit.waits.size != 0 || submit.signals.size != 0) {
				return Result::Unsupported;
			}

			VulkanQueueState* queueState = VkQueueStateForHandle(impl, queue->GetQueueHandle());
			if (!queueState || queueState->queue == VK_NULL_HANDLE) {
				return Result::InvalidArgument;
			}

			if (lists.size == 0) {
				return Result::Ok;
			}

			std::vector<VkCommandBuffer> commandBuffers;
			commandBuffers.reserve(lists.size);
			for (uint32_t index = 0; index < lists.size; ++index) {
				VulkanCommandList* commandListState = VkCommandListState(impl, lists.data[index].GetHandle());
				if (!commandListState || commandListState->commandBuffer == VK_NULL_HANDLE) {
					return Result::InvalidArgument;
				}

				if (commandListState->isRecording) {
					spdlog::error("Vulkan queue submit rejected a command list that is still recording");
					return Result::InvalidArgument;
				}

				commandBuffers.push_back(commandListState->commandBuffer);
			}

			VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
			submitInfo.pCommandBuffers = commandBuffers.data();

			return ToRHI(vkQueueSubmit(queueState->queue, 1, &submitInfo, VK_NULL_HANDLE));
		}

		static Result q_signal(Queue* queue, const TimelinePoint& point) noexcept {
			VkIgnoreUnused(queue, point);
			return Result::Unsupported;
		}

		static Result q_wait(Queue* queue, const TimelinePoint& point) noexcept {
			VkIgnoreUnused(queue, point);
			return Result::Unsupported;
		}

		static void q_checkDebugMessages(Queue* queue) noexcept {
			VkIgnoreUnused(queue);
		}

		static void q_setName(Queue* queue, const char* name) noexcept {
			VkIgnoreUnused(queue, name);
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
			VkIgnoreUnused(resource, name);
		}

		static void tex_map(Resource* resource, void** data, uint64_t offset, uint64_t size) noexcept {
			VkIgnoreUnused(resource, offset, size);
			if (data) {
				*data = nullptr;
			}
		}

		static void tex_unmap(Resource* resource, uint64_t writeOffset, uint64_t writeSize) noexcept {
			VkIgnoreUnused(resource, writeOffset, writeSize);
		}

		static void tex_setName(Resource* resource, const char* name) noexcept {
			VkIgnoreUnused(resource, name);
		}

		static void cl_endPass(CommandList* commandList) noexcept;

		static void cl_end(CommandList* commandList) noexcept {
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!commandListState || commandListState->commandBuffer == VK_NULL_HANDLE || !commandListState->isRecording) {
				return;
			}

			if (commandListState->passActive) {
				cl_endPass(commandList);
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
			commandListState->passColorResources.clear();
			commandListState->passDepthResource = {};
			commandListState->isRecording = true;
		}

		static void cl_beginPass(CommandList* commandList, const PassBeginInfo& passInfo) noexcept {
			auto* impl = commandList ? static_cast<VulkanDevice*>(commandList->impl) : nullptr;
			VulkanCommandList* commandListState = VkCommandListState(commandList);
			if (!impl || !impl->dynamicRenderingEnabled || !commandListState || !commandListState->isRecording || commandListState->commandBuffer == VK_NULL_HANDLE || commandListState->passActive) {
				return;
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

				VkTransitionResourceLayout(commandListState->commandBuffer, *resource, viewSlot->aspectMask, ResourceLayout::RenderTarget);

				VkRenderingAttachmentInfo attachmentInfo{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				attachmentInfo.imageView = viewSlot->view;
				attachmentInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				attachmentInfo.storeOp = color.storeOp == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
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

				const ResourceLayout depthLayout = passInfo.depth->readOnly ? ResourceLayout::DepthRead : ResourceLayout::DepthReadWrite;
				VkTransitionResourceLayout(commandListState->commandBuffer, *resource, viewSlot->aspectMask, depthLayout);

				depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				depthAttachment.imageView = viewSlot->view;
				depthAttachment.imageLayout = passInfo.depth->readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depthAttachment.loadOp = passInfo.depth->depthLoad == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : (passInfo.depth->depthLoad == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
				depthAttachment.storeOp = passInfo.depth->depthStore == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
				depthAttachment.clearValue.depthStencil.depth = passInfo.depth->clear.depthStencil.depth;
				depthAttachment.clearValue.depthStencil.stencil = passInfo.depth->clear.depthStencil.stencil;
				depthAttachmentPtr = (viewSlot->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? &depthAttachment : nullptr;

				if ((viewSlot->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
					stencilAttachment = depthAttachment;
					stencilAttachment.loadOp = passInfo.depth->stencilLoad == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : (passInfo.depth->stencilLoad == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD);
					stencilAttachment.storeOp = passInfo.depth->stencilStore == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
					stencilAttachmentPtr = &stencilAttachment;
				}

				commandListState->passDepthResource = viewSlot->resource;
			}

			VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
			renderingInfo.renderArea.offset = { 0, 0 };
			renderingInfo.renderArea.extent = { passInfo.width, passInfo.height };
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

			for (ResourceHandle handle : commandListState->passColorResources) {
				VulkanResource* resource = VkResourceState(impl, handle);
				if (resource && resource->isSwapchainImage) {
					VkTransitionResourceLayout(commandListState->commandBuffer, *resource, VK_IMAGE_ASPECT_COLOR_BIT, ResourceLayout::Present);
				}
			}

			commandListState->passColorResources.clear();
			commandListState->passDepthResource = {};
			commandListState->passActive = false;
		}

		static void cl_barrier(CommandList* commandList, const BarrierBatch& barriers) noexcept {
			VkIgnoreUnused(commandList, barriers);
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
			VkIgnoreUnused(commandList, startSlot, numViews, views);
		}

		static void cl_setIB(CommandList* commandList, const IndexBufferView& view) noexcept {
			VkIgnoreUnused(commandList, view);
		}

		static void cl_draw(CommandList* commandList, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
			VkIgnoreUnused(commandList, vertexCount, instanceCount, firstVertex, firstInstance);
		}

		static void cl_drawIndexed(CommandList* commandList, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
			VkIgnoreUnused(commandList, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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
			VkIgnoreUnused(commandList, slot, clearValue);
		}

		static void cl_clearDSV_slot(CommandList* commandList, DescriptorSlot slot, bool clearDepth, bool clearStencil, float depth, uint8_t stencil) noexcept {
			VkIgnoreUnused(commandList, slot, clearDepth, clearStencil, depth, stencil);
		}

		static void cl_executeIndirect(CommandList* commandList,
			CommandSignatureHandle signature,
			ResourceHandle argumentBuffer,
			uint64_t argumentOffset,
			ResourceHandle countBuffer,
			uint64_t countOffset,
			uint32_t maxCommandCount) noexcept {
			VkIgnoreUnused(commandList, signature, argumentBuffer, argumentOffset, countBuffer, countOffset, maxCommandCount);
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
			VkIgnoreUnused(commandList, clearInfo, value);
		}

		static void cl_clearUavFloat(CommandList* commandList, const UavClearInfo& clearInfo, const UavClearFloat& value) noexcept {
			VkIgnoreUnused(commandList, clearInfo, value);
		}

		static void cl_copyTextureToBuffer(CommandList* commandList, const BufferTextureCopyFootprint& footprint) noexcept {
			VkIgnoreUnused(commandList, footprint);
		}

		static void cl_copyBufferToTexture(CommandList* commandList, const BufferTextureCopyFootprint& footprint) noexcept {
			VkIgnoreUnused(commandList, footprint);
		}

		static void cl_copyTextureRegion(CommandList* commandList, const TextureCopyRegion& dst, const TextureCopyRegion& src) noexcept {
			VkIgnoreUnused(commandList, dst, src);
		}

		static void cl_copyBufferRegion(CommandList* commandList, ResourceHandle dst, uint64_t dstOffset, ResourceHandle src, uint64_t srcOffset, uint64_t numBytes) noexcept {
			VkIgnoreUnused(commandList, dst, dstOffset, src, srcOffset, numBytes);
		}

		static void cl_writeTimestamp(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index, Stage stageHint) noexcept {
			VkIgnoreUnused(commandList, queryPool, index, stageHint);
		}

		static void cl_beginQuery(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index) noexcept {
			VkIgnoreUnused(commandList, queryPool, index);
		}

		static void cl_endQuery(CommandList* commandList, QueryPoolHandle queryPool, uint32_t index) noexcept {
			VkIgnoreUnused(commandList, queryPool, index);
		}

		static void cl_resolveQueryData(CommandList* commandList, QueryPoolHandle queryPool, uint32_t firstQuery, uint32_t queryCount, ResourceHandle dstBuffer, uint64_t dstOffsetBytes) noexcept {
			VkIgnoreUnused(commandList, queryPool, firstQuery, queryCount, dstBuffer, dstOffsetBytes);
		}

		static void cl_resetQueries(CommandList* commandList, QueryPoolHandle queryPool, uint32_t firstQuery, uint32_t queryCount) noexcept {
			VkIgnoreUnused(commandList, queryPool, firstQuery, queryCount);
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

			const uint32_t byteOffset = pushRange->byteOffset + dstOffset32 * 4u;
			const uint32_t byteSize = num32 * 4u;
			if (byteOffset + byteSize > pushRange->byteOffset + pushRange->byteSize ||
				byteOffset + byteSize > layoutState->totalPushDataBytes ||
				byteOffset + byteSize > impl->descriptorHeapProperties.maxPushDataSize) {
				return;
			}

			VkPushDataInfoEXT pushInfo{ VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT };
			pushInfo.offset = byteOffset;
			pushInfo.data.address = data;
			pushInfo.data.size = byteSize;
			vkCmdPushDataEXT(commandListState->commandBuffer, &pushInfo);
		}

		static void cl_setPrimitiveTopology(CommandList* commandList, PrimitiveTopology topology) noexcept {
			VkIgnoreUnused(commandList, topology);
		}

		static void cl_dispatchMesh(CommandList* commandList, uint32_t x, uint32_t y, uint32_t z) noexcept {
			VkIgnoreUnused(commandList, x, y, z);
		}

		static void cl_setWorkGraph(CommandList* commandList, const WorkGraphHandle& workGraph, const ResourceHandle& backingMemory, bool resetBackingMemory) noexcept {
			VkIgnoreUnused(commandList, workGraph, backingMemory, resetBackingMemory);
		}

		static void cl_dispatchWorkGraph(CommandList* commandList, const WorkGraphDispatchDesc& desc) noexcept {
			VkIgnoreUnused(commandList, desc);
		}

		static void cl_setName(CommandList* commandList, const char* name) noexcept {
			VkIgnoreUnused(commandList, name);
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

		static Result sc_present(Swapchain* swapchain, bool vsync) noexcept {
			VkIgnoreUnused(vsync);
			auto* impl = swapchain ? static_cast<VulkanDevice*>(swapchain->impl) : nullptr;
			VulkanSwapchain* swapchainState = VkSwapchainState(swapchain);
			if (!impl || !swapchainState || swapchainState->swapchain == VK_NULL_HANDLE) {
				return Result::InvalidArgument;
			}

			VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &swapchainState->swapchain;
			presentInfo.pImageIndices = &swapchainState->currentImageIndex;

			const VkResult presentResult = vkQueuePresentKHR(impl->queues[0].queue, &presentInfo);
			if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
				return ToRHI(presentResult);
			}

			const Result acquireResult = VkAcquireNextSwapchainImage(impl, *swapchainState);
			if (presentResult == VK_SUBOPTIMAL_KHR && acquireResult == Result::Ok) {
				return Result::ModeChanged;
			}
			return acquireResult;
		}

		static Result sc_resizeBuffers(Swapchain* swapchain, uint32_t bufferCount, uint32_t width, uint32_t height, Format newFormat, uint32_t flags) noexcept {
			VkIgnoreUnused(flags);
			auto* impl = swapchain ? static_cast<VulkanDevice*>(swapchain->impl) : nullptr;
			VulkanSwapchain* swapchainState = VkSwapchainState(swapchain);
			if (!impl || !swapchainState) {
				return Result::InvalidArgument;
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
				return Result::InvalidArgument;
			}

			const SubobjShader* computeShader = nullptr;
			const VulkanPipelineLayout* layoutState = nullptr;
			PipelineLayoutHandle layoutHandle{};
			bool sawGraphicsState = false;

			for (uint32_t index = 0; index < count; ++index) {
				switch (items[index].type) {
				case PsoSubobj::Layout: {
					const auto& layout = *static_cast<const SubobjLayout*>(items[index].data);
					layoutHandle = layout.layout;
					layoutState = VkPipelineLayoutState(impl, layout.layout);
					if (!layoutState) {
						spdlog::error("Vulkan pipeline creation: invalid pipeline layout handle");
						return Result::InvalidArgument;
					}
					break;
				}
				case PsoSubobj::Shader: {
					const auto& shader = *static_cast<const SubobjShader*>(items[index].data);
					switch (shader.stage) {
					case ShaderStage::Compute:
						if (computeShader) {
							spdlog::error("Vulkan pipeline creation: multiple compute shader stages are not supported");
							return Result::InvalidArgument;
						}
						computeShader = &shader;
						break;
					case ShaderStage::Vertex:
					case ShaderStage::Pixel:
					case ShaderStage::Task:
					case ShaderStage::Mesh:
						sawGraphicsState = true;
						break;
					default:
						spdlog::error("Vulkan pipeline creation: unsupported shader stage {}", static_cast<uint32_t>(shader.stage));
						return Result::InvalidArgument;
					}
					break;
				}
				case PsoSubobj::Rasterizer:
				case PsoSubobj::Blend:
				case PsoSubobj::DepthStencil:
				case PsoSubobj::RTVFormats:
				case PsoSubobj::DSVFormat:
				case PsoSubobj::Sample:
				case PsoSubobj::InputLayout:
				case PsoSubobj::PrimitiveTopology:
					sawGraphicsState = true;
					break;
				case PsoSubobj::Flags:
				default:
					break;
				}
			}

			if (!computeShader) {
				return sawGraphicsState ? Result::Unsupported : Result::InvalidArgument;
			}
			if (sawGraphicsState) {
				spdlog::warn("Vulkan pipeline creation: graphics pipeline creation is not implemented yet");
				return Result::Unsupported;
			}
			if (!layoutState) {
				spdlog::error("Vulkan pipeline creation: a pipeline layout subobject is required");
				return Result::InvalidArgument;
			}
			if (!layoutState->ranges.empty() || !layoutState->staticSamplers.empty()) {
				spdlog::warn("Vulkan compute pipeline creation: descriptor mappings are not implemented yet");
				return Result::Unsupported;
			}
			if (!VkIsSpirvBytecode(computeShader->bytecode)) {
				spdlog::warn("Vulkan compute pipeline creation: shader bytecode was not SPIR-V");
				return Result::Unsupported;
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

			VkPipelineLayoutCreateInfo nativeLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			VkPipelineLayout nativeLayout = VK_NULL_HANDLE;
			vkResult = vkCreatePipelineLayout(impl->device, &nativeLayoutInfo, nullptr, &nativeLayout);
			if (vkResult != VK_SUCCESS) {
				vkDestroyShaderModule(impl->device, shaderModule, nullptr);
				spdlog::error("Vulkan compute pipeline creation: vkCreatePipelineLayout failed with VkResult {}", static_cast<int>(vkResult));
				return ToRHI(vkResult);
			}

			const char* entryPoint = computeShader->entryPoint.empty() ? "main" : computeShader->entryPoint.c_str();
			VkPipelineShaderStageCreateInfo stageInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageInfo.module = shaderModule;
			stageInfo.pName = entryPoint;

			VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			pipelineInfo.stage = stageInfo;
			pipelineInfo.layout = nativeLayout;

			VkPipeline nativePipeline = VK_NULL_HANDLE;
			vkResult = vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &nativePipeline);
			vkDestroyShaderModule(impl->device, shaderModule, nullptr);
			if (vkResult != VK_SUCCESS) {
				vkDestroyPipelineLayout(impl->device, nativeLayout, nullptr);
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
			return Result::Unsupported;
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
			return out ? Result::Ok : Result::Failed;
		}

		static void d_destroyQueue(DeviceDeletionContext* context, QueueHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_waitIdle(Device* device) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE) {
				return Result::Failed;
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
				return Result::InvalidArgument;
			}

			if (!impl->swapchainExtensionEnabled) {
				out.Reset();
				return Result::Unsupported;
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
			return Result::Unsupported;
		#endif

			const Result createResult = VkCreateOrResizeSwapchain(impl, swapchainState, width, height, format, bufferCount, allowTearing);
			if (createResult != Result::Ok) {
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
				return Result::Unsupported;
			}

			VulkanPipelineLayout layoutState{};
			layoutState.flags = desc.flags;
			layoutState.ranges.assign(desc.ranges.data, desc.ranges.data + desc.ranges.size);
			layoutState.pushConstants.assign(desc.pushConstants.data, desc.pushConstants.data + desc.pushConstants.size);
			layoutState.staticSamplers.assign(desc.staticSamplers.data, desc.staticSamplers.data + desc.staticSamplers.size);

			uint32_t pushDataOffset = 0;
			layoutState.pushConstantRanges.reserve(layoutState.pushConstants.size());
			for (const PushConstantRangeDesc& pushConstant : layoutState.pushConstants) {
				VulkanPushConstantRange range{};
				range.desc = pushConstant;
				range.byteOffset = pushDataOffset;
				range.byteSize = pushConstant.num32BitValues * 4u;
				layoutState.pushConstantRanges.push_back(range);
				pushDataOffset += range.byteSize;
			}
			layoutState.totalPushDataBytes = pushDataOffset;

			if (layoutState.totalPushDataBytes > impl->descriptorHeapProperties.maxPushDataSize) {
				out.Reset();
				return Result::Unsupported;
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
				return Result::InvalidArgument;
			}

			auto* impl = static_cast<const VulkanDevice*>(device->impl);
			if (!impl || impl->physicalDevice == VK_NULL_HANDLE) {
				return Result::Failed;
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
					return Result::InvalidArgument;
				}

				switch (header->sType) {
				case FeatureInfoStructType::AdapterInfo: {
					if (header->structSize < sizeof(AdapterFeatureInfo)) {
						return Result::InvalidArgument;
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
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<ArchitectureFeatureInfo*>(header);
					out->uma = hostVisibleDeviceLocal || !hasDeviceLocalHeap;
					out->cacheCoherentUMA = hostCoherentDeviceLocal;
					out->isolatedMMU = false;
					out->tileBasedRenderer = false;
				} break;

				case FeatureInfoStructType::Features: {
					if (header->structSize < sizeof(ShaderFeatureInfo)) {
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<ShaderFeatureInfo*>(header);
					out->maxShaderModel = ShaderModel::Unknown;
					out->unifiedResourceHeaps = false;
					out->unboundedDescriptorTables = false;
					out->waveOps = false;
					out->int64ShaderOps = impl->supportedFeatures.shaderInt64 == VK_TRUE;
					out->barycentrics = false;
					out->derivativesInMeshAndTaskShaders = false;
					out->atomicInt64OnGroupShared = false;
					out->atomicInt64OnTypedResource = false;
					out->atomicInt64OnDescriptorHeapResources = false;
				} break;

				case FeatureInfoStructType::MeshShaders: {
					if (header->structSize < sizeof(MeshShaderFeatureInfo)) {
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<MeshShaderFeatureInfo*>(header);
					out->meshShader = false;
					out->taskShader = false;
					out->derivatives = false;
				} break;

				case FeatureInfoStructType::RayTracing: {
					if (header->structSize < sizeof(RayTracingFeatureInfo)) {
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<RayTracingFeatureInfo*>(header);
					out->pipeline = false;
					out->rayQuery = false;
					out->indirect = false;
				} break;

				case FeatureInfoStructType::ShadingRate: {
					if (header->structSize < sizeof(ShadingRateFeatureInfo)) {
						return Result::InvalidArgument;
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
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<EnhancedBarriersFeatureInfo*>(header);
					out->enhancedBarriers = false;
					out->relaxedFormatCasting = false;
				} break;

				case FeatureInfoStructType::ResourceAllocation: {
					if (header->structSize < sizeof(ResourceAllocationFeatureInfo)) {
						return Result::InvalidArgument;
					}

					auto* out = reinterpret_cast<ResourceAllocationFeatureInfo*>(header);
					out->gpuUploadHeapSupported = hostVisibleDeviceLocal;
					out->tightAlignmentSupported = false;
					out->createNotZeroedHeapSupported = false;
				} break;

				case FeatureInfoStructType::WorkGraphs: {
					if (header->structSize < sizeof(WorkGraphFeatureInfo)) {
						return Result::InvalidArgument;
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
				return Result::Failed;
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
				return Result::InvalidArgument;
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
			VkIgnoreUnused(device, desc, layout);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyCommandSignature(DeviceDeletionContext* context, CommandSignatureHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_createDescriptorHeap(Device* device, const DescriptorHeapDesc& desc, DescriptorHeapPtr& out) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (!impl || impl->device == VK_NULL_HANDLE || desc.capacity == 0) {
				out.Reset();
				return Result::InvalidArgument;
			}

			VulkanDescriptorHeap heap{};
			heap.type = desc.type;
			heap.capacity = desc.capacity;
			heap.shaderVisible = desc.shaderVisible;
			heap.imageViewSlots.resize(desc.capacity);

			if (impl->descriptorHeapEnabled && (desc.type == DescriptorHeapType::CbvSrvUav || desc.type == DescriptorHeapType::Sampler)) {
				heap.descriptorStride = VkDescriptorHeapStride(impl, desc.type);
				heap.descriptorBytes = static_cast<uint64_t>(desc.capacity) * heap.descriptorStride;
				heap.reservedRangeSize = desc.type == DescriptorHeapType::Sampler
					? static_cast<uint64_t>(impl->descriptorHeapProperties.minSamplerHeapReservedRange)
					: static_cast<uint64_t>(impl->descriptorHeapProperties.minResourceHeapReservedRange);
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
					return Result::Unsupported;
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
				return Result::InvalidArgument;
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
					return Result::Unsupported;
				}

				const uint64_t elementSize = desc.buffer.kind == BufferViewKind::Structured
					? strideBytes
					: (desc.buffer.kind == BufferViewKind::Typed ? static_cast<uint64_t>(FormatByteSize(desc.formatOverride)) : 4ull);
				if (elementSize == 0) {
					return Result::InvalidArgument;
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
			const VkFormat viewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resourceState->format;
			const VkImageAspectFlags aspectMask = VkAspectMaskForFormat(viewFormat);
			if (viewType == VK_IMAGE_VIEW_TYPE_MAX_ENUM || viewFormat == VK_FORMAT_UNDEFINED) {
				return Result::Unsupported;
			}
			const VkImageSubresourceRange subresourceRange = VkSrvSubresourceRange(*resourceState, desc, aspectMask);

			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					return Result::InvalidArgument;
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

				const VkImageLayout imageLayout = resourceState->currentLayout != ResourceLayout::Undefined
					? VkToImageLayout(resourceState->currentLayout, aspectMask)
					: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

				VkImageDescriptorInfoEXT imageInfo{ VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT };
				imageInfo.pView = &viewCreateInfo;
				imageInfo.layout = imageLayout;

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
					return Result::InvalidArgument;
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
				return Result::InvalidArgument;
			}

			if (desc.dimension == UavDim::Texture2DMS || desc.dimension == UavDim::Texture2DMSArray) {
				return Result::Unsupported;
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
					return Result::Unsupported;
				}

				const uint64_t elementSize = desc.buffer.kind == BufferViewKind::Structured
					? strideBytes
					: (desc.buffer.kind == BufferViewKind::Typed ? static_cast<uint64_t>(FormatByteSize(desc.formatOverride)) : 4ull);
				if (elementSize == 0) {
					return Result::InvalidArgument;
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
				return Result::Unsupported;
			}

			const VkImageSubresourceRange subresourceRange = VkUavSubresourceRange(*resourceState, desc, aspectMask);
			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					return Result::InvalidArgument;
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
					return Result::InvalidArgument;
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
				return Result::InvalidArgument;
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
				return Result::InvalidArgument;
			}

			if (desc.reduction == ReductionMode::Min || desc.reduction == ReductionMode::Max) {
				return Result::Unsupported;
			}
			if (desc.borderPreset == BorderPreset::Custom) {
				return Result::Unsupported;
			}

			VkResetDescriptorSlot(impl, *descriptorSlot);

			VkSamplerCreateInfo createInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			createInfo.magFilter = VkToFilter(desc.magFilter);
			createInfo.minFilter = VkToFilter(desc.minFilter);
			createInfo.mipmapMode = VkToMipFilter(desc.mipFilter);
			createInfo.addressModeU = VkToAddressMode(desc.addressU);
			createInfo.addressModeV = VkToAddressMode(desc.addressV);
			createInfo.addressModeW = VkToAddressMode(desc.addressW);
			createInfo.mipLodBias = desc.mipLodBias;
			createInfo.anisotropyEnable = desc.maxAnisotropy > 1;
			createInfo.maxAnisotropy = static_cast<float>((std::min)(desc.maxAnisotropy, 16u));
			createInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
			createInfo.compareOp = VkToCompareOp(desc.compareOp);
			createInfo.minLod = desc.minLod;
			createInfo.maxLod = desc.maxLod;
			createInfo.borderColor = VkToBorderColor(desc);
			createInfo.unnormalizedCoordinates = desc.unnormalizedCoordinates ? VK_TRUE : VK_FALSE;

			if (impl->descriptorHeapEnabled && heap && heap->buffer != VK_NULL_HANDLE) {
				void* slotAddress = VkDescriptorHeapSlotAddress(heap, slot.index);
				if (!slotAddress) {
					return Result::InvalidArgument;
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
				return Result::InvalidArgument;
			}

			const VkFormat viewFormat = desc.formatOverride != Format::Unknown ? ToVkFormat(desc.formatOverride) : resource->format;
			const VkImageViewType viewType = VkImageViewTypeForRtv(desc.dimension);
			return VkCreateImageViewSlot(impl, slot, DescriptorHeapType::RTV, texture, viewFormat, VK_IMAGE_ASPECT_COLOR_BIT, viewType, desc.range);
		}

		static Result d_createDepthStencilView(Device* device, DescriptorSlot slot, const ResourceHandle& texture, const DsvDesc& desc) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			VulkanResource* resource = VkResourceState(impl, texture);
			if (!impl || !resource) {
				return Result::InvalidArgument;
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
				return Result::InvalidArgument;
			}

			const uint32_t queueSlot = VkPrimaryQueueSlotForKind(kind);
			if (queueSlot >= impl->queues.size() || impl->queues[queueSlot].familyIndex == kVkInvalidQueueFamily) {
				out.Reset();
				return Result::Unsupported;
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
				return Result::InvalidArgument;
			}

			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			Result result = VkAllocatePrimaryCommandBuffer(impl->device, allocatorState->pool, commandBuffer);
			if (result != Result::Ok) {
				out.Reset();
				return result;
			}

			result = VkBeginCommandRecording(commandBuffer);
			if (result != Result::Ok) {
				vkFreeCommandBuffers(impl->device, allocatorState->pool, 1, &commandBuffer);
				out.Reset();
				return result;
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
				return Result::InvalidArgument;
			}
			if (desc.heapType == HeapType::Custom) {
				out.Reset();
				return Result::Unsupported;
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
				return Result::Unsupported;
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
				return Result::InvalidArgument;
			}
			if (desc.type != ResourceType::Texture1D && desc.type != ResourceType::Texture2D && desc.type != ResourceType::Texture3D) {
				out.Reset();
				return Result::InvalidArgument;
			}
			if (desc.texture.width == 0 || desc.texture.height == 0 || desc.texture.mipLevels == 0 || desc.texture.format == Format::Unknown) {
				out.Reset();
				return Result::InvalidArgument;
			}
			if (desc.texture.initialLayout != ResourceLayout::Undefined) {
				out.Reset();
				return Result::Unsupported;
			}
			if (desc.heapType != HeapType::DeviceLocal) {
				out.Reset();
				return Result::Unsupported;
			}

			const VkFormat format = ToVkFormat(desc.texture.format);
			const VkImageType imageType = VkImageTypeForResourceType(desc.type);
			const VkSampleCountFlagBits samples = VkSampleCountForDesc(desc.texture.sampleCount);
			if (format == VK_FORMAT_UNDEFINED || imageType == VK_IMAGE_TYPE_MAX_ENUM || samples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) {
				out.Reset();
				return Result::Unsupported;
			}

			VkImageCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
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
				return Result::Unsupported;
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
				return Result::Unsupported;
			}
		}

		static uint32_t d_getDescriptorHandleIncrementSize(Device* device, DescriptorHeapType type) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			return static_cast<uint32_t>(VkDescriptorHeapStride(impl, type));
		}

		static Result d_createTimeline(Device* device, uint64_t initialValue, const char* debugName, TimelinePtr& out) noexcept {
			VkIgnoreUnused(device, initialValue, debugName);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyTimeline(DeviceDeletionContext* context, TimelineHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_createHeap(const Device* device, const HeapDesc& desc, HeapPtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyHeap(DeviceDeletionContext* context, HeapHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_setNameBuffer(Device* device, ResourceHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameTexture(Device* device, ResourceHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameSampler(Device* device, SamplerHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNamePipelineLayout(Device* device, PipelineLayoutHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNamePipeline(Device* device, PipelineHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameCommandSignature(Device* device, CommandSignatureHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameDescriptorHeap(Device* device, DescriptorHeapHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameTimeline(Device* device, TimelineHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static void d_setNameHeap(Device* device, HeapHandle handle, const char* name) noexcept {
			VkIgnoreUnused(device, handle, name);
		}

		static Result d_createPlacedTexture(Device* device, HeapHandle heap, uint64_t offset, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			VkIgnoreUnused(device, heap, offset, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static Result d_createPlacedBuffer(Device* device, HeapHandle heap, uint64_t offset, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			VkIgnoreUnused(device, heap, offset, desc);
			out.Reset();
			return Result::Unsupported;
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
				return Result::Unsupported;
			}
		}

		static Result d_createQueryPool(Device* device, const QueryPoolDesc& desc, QueryPoolPtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyQueryPool(DeviceDeletionContext* context, QueryPoolHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
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
			VkIgnoreUnused(device, range, out, outCap);
			return {};
		}

		static Result d_getResourceAllocationInfo(const Device* device, const ResourceDesc* resources, uint32_t resourceCount, ResourceAllocationInfo* outInfos) noexcept {
			VkIgnoreUnused(device, resources, resourceCount, outInfos);
			return Result::Unsupported;
		}

		static Result d_setResidencyPriority(const Device* device, Span<PageableRef> objects, ResidencyPriority priority) noexcept {
			VkIgnoreUnused(device, objects, priority);
			return Result::Unsupported;
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
			return Result::Unsupported;
		}

		static Result d_setDebugPipelineInstrumentationMask(Device* device, uint64_t pipelineUid, uint64_t featureMask) noexcept {
			VkIgnoreUnused(device, pipelineUid, featureMask);
			return Result::Unsupported;
		}

		static Result d_setDebugSynchronousRecording(Device* device, bool enabled) noexcept {
			VkIgnoreUnused(device, enabled);
			return Result::Unsupported;
		}

		static Result d_setDebugTexelAddressing(Device* device, bool enabled) noexcept {
			VkIgnoreUnused(device, enabled);
			return Result::Unsupported;
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
			return Result::InvalidArgument;
		}

		const VkResult volkInit = volkInitialize();
		if (volkInit != VK_SUCCESS) {
			spdlog::error("CreateVulkanDevice: volkInitialize failed with VkResult {}", static_cast<int>(volkInit));
			return Result::SdkComponentMissing;
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
		VkPhysicalDeviceFeatures2 supportedFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		supportedFeatures2.pNext = &supportedVulkan12Features;
		supportedVulkan12Features.pNext = &supportedVulkan13Features;
		supportedVulkan13Features.pNext = &supportedDescriptorHeapFeatures;
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		properties2.pNext = &descriptorHeapProperties;
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
			return Result::NotFound;
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
		const bool hasDescriptorHeapExtension = VkHasDeviceExtension(physicalDevice, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		std::vector<const char*> enabledDeviceExtensions;
		if (enableSwapchainExtension) {
			enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		}
		if (hasDescriptorHeapExtension) {
			enabledDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		}
		vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures2);
		vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
		supportedFeatures = supportedFeatures2.features;

		VkPhysicalDeviceVulkan12Features enabledVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features enabledVulkan13Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorHeapFeaturesEXT enabledDescriptorHeapFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
		enabledVulkan12Features.pNext = &enabledVulkan13Features;
		enabledVulkan13Features.pNext = &enabledDescriptorHeapFeatures;
		if (supportedVulkan12Features.bufferDeviceAddress == VK_TRUE) {
			enabledVulkan12Features.bufferDeviceAddress = VK_TRUE;
		}
		if (supportedVulkan13Features.dynamicRendering == VK_TRUE) {
			enabledVulkan13Features.dynamicRendering = VK_TRUE;
		}
		if (hasDescriptorHeapExtension &&
			supportedDescriptorHeapFeatures.descriptorHeap == VK_TRUE &&
			enabledVulkan12Features.bufferDeviceAddress == VK_TRUE) {
			enabledDescriptorHeapFeatures.descriptorHeap = VK_TRUE;
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
		impl->bufferDeviceAddressEnabled = enabledVulkan12Features.bufferDeviceAddress == VK_TRUE;
		impl->descriptorHeapEnabled = enabledDescriptorHeapFeatures.descriptorHeap == VK_TRUE;
		impl->dynamicRenderingEnabled = enabledVulkan13Features.dynamicRendering == VK_TRUE;
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
			"CreateVulkanDevice: selected device '{}' api {}.{}.{} queueFamilies[gfx={}, compute={}, copy={}] dynamicRendering={} bufferDeviceAddress={} descriptorHeap={}",
			impl->physicalDeviceProperties.deviceName,
			VK_API_VERSION_MAJOR(impl->physicalDeviceProperties.apiVersion),
			VK_API_VERSION_MINOR(impl->physicalDeviceProperties.apiVersion),
			VK_API_VERSION_PATCH(impl->physicalDeviceProperties.apiVersion),
			impl->queues[0].familyIndex,
			impl->queues[1].familyIndex,
			impl->queues[2].familyIndex,
			impl->dynamicRenderingEnabled,
			impl->bufferDeviceAddressEnabled,
			impl->descriptorHeapEnabled);

		outPtr = MakeDevicePtr(&impl->self, impl);
		return Result::Ok;
	}

} // namespace rhi