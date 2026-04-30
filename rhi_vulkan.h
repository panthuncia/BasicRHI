#pragma once

#include "rhi.h"

#include "volk.h"

#include <array>
#include <deque>
#include <memory>

#include <vector>

namespace rhi {
	template<class Obj> struct VulkanHandleFor;

	struct VulkanDescriptorHeap;
	struct VulkanResource;
	struct VulkanSwapchain;
	struct VulkanCommandAllocator;
	struct VulkanCommandList;

	template<> struct VulkanHandleFor<VulkanDescriptorHeap> { using type = DescriptorHeapHandle; };
	template<> struct VulkanHandleFor<VulkanResource> { using type = ResourceHandle; };
	template<> struct VulkanHandleFor<VulkanSwapchain> { using type = SwapChainHandle; };
	template<> struct VulkanHandleFor<VulkanCommandAllocator> { using type = CommandAllocatorHandle; };
	template<> struct VulkanHandleFor<VulkanCommandList> { using type = CommandListHandle; };

	template<typename T>
	struct VulkanSlot {
		T obj{};
		uint32_t generation{ 1 };
		bool alive{ false };
	};

	template<typename T>
	struct VulkanRegistry {
		using HandleT = typename VulkanHandleFor<T>::type;

		std::deque<VulkanSlot<T>> slots;
		std::vector<uint32_t> freelist;

		HandleT alloc(const T& value) {
			if (!freelist.empty()) {
				const uint32_t index = freelist.back();
				freelist.pop_back();
				auto& slot = slots[index];
				slot.obj = value;
				slot.alive = true;
				++slot.generation;
				return HandleT{ index, slot.generation };
			}

			const uint32_t index = static_cast<uint32_t>(slots.size());
			slots.push_back({ value, 1u, true });
			return HandleT{ index, 1u };
		}

		void free(HandleT handle) {
			const uint32_t index = handle.index;
			if (index >= slots.size()) {
				return;
			}

			auto& slot = slots[index];
			if (!slot.alive || slot.generation != handle.generation) {
				return;
			}

			slot.alive = false;
			slot.obj = T{};
			freelist.push_back(index);
		}

		T* get(HandleT handle) {
			const uint32_t index = handle.index;
			if (index >= slots.size()) {
				return nullptr;
			}

			auto& slot = slots[index];
			if (!slot.alive || slot.generation != handle.generation) {
				return nullptr;
			}

			return &slot.obj;
		}

		const T* get(HandleT handle) const {
			const uint32_t index = handle.index;
			if (index >= slots.size()) {
				return nullptr;
			}

			const auto& slot = slots[index];
			if (!slot.alive || slot.generation != handle.generation) {
				return nullptr;
			}

			return &slot.obj;
		}

		void clear() {
			slots.clear();
			freelist.clear();
		}
	};

	struct VulkanQueueState {
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t familyIndex = 0xFFFFFFFFu;
		uint32_t queueIndex = 0;
	};

	struct VulkanResource {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mappedData = nullptr;
		VkDeviceAddress deviceAddress = 0;
		uint64_t bufferSize = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		ResourceType type = ResourceType::Unknown;
		ResourceLayout currentLayout = ResourceLayout::Undefined;
		uint32_t width = 0;
		uint32_t height = 0;
		uint16_t depthOrLayers = 1;
		uint16_t mipLevels = 1;
		bool hostVisible = false;
		bool isSwapchainImage = false;
		bool ownsBuffer = false;
		bool ownsImage = false;
		bool ownsMemory = false;
	};

	struct VulkanImageViewSlot {
		enum class Kind : uint8_t {
			None,
			ImageView,
			BufferView,
			ConstantBuffer,
			Sampler,
		};

		Kind kind = Kind::None;
		VkImageView view = VK_NULL_HANDLE;
		VkBufferView bufferView = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		ResourceHandle resource{};
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageAspectFlags aspectMask = 0;
		TextureSubresourceRange range{};
		uint64_t bufferOffset = 0;
		uint64_t bufferSize = 0;
		uint32_t bufferStride = 0;
		BufferViewKind bufferKind = BufferViewKind::Raw;
		ComponentMapping componentMapping = 0;
		CbvDesc cbv{};
		SamplerDesc samplerDesc{};
	};

	struct VulkanDescriptorHeap {
		DescriptorHeapType type = DescriptorHeapType::CbvSrvUav;
		uint32_t capacity = 0;
		bool shaderVisible = false;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mappedData = nullptr;
		VkDeviceAddress deviceAddress = 0;
		uint64_t descriptorStride = 0;
		uint64_t descriptorBytes = 0;
		uint64_t reservedRangeOffset = 0;
		uint64_t reservedRangeSize = 0;
		std::vector<VulkanImageViewSlot> imageViewSlots;
	};

	struct VulkanSwapchain {
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		Format rhiFormat = Format::Unknown;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t imageCount = 0;
		uint32_t currentImageIndex = 0;
		bool allowTearing = false;
		std::vector<VkImage> images;
		std::vector<ResourceHandle> imageHandles;
		VkFence acquireFence = VK_NULL_HANDLE;
	};

	struct VulkanCommandAllocator {
		VkCommandPool pool = VK_NULL_HANDLE;
		QueueKind kind = QueueKind::Graphics;
		uint32_t familyIndex = 0xFFFFFFFFu;
	};

	struct VulkanCommandList {
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		CommandAllocatorHandle allocatorHandle{};
		QueueKind kind = QueueKind::Graphics;
		bool isRecording = false;
		bool passActive = false;
		DescriptorHeapHandle boundCbvSrvUavHeap{};
		DescriptorHeapHandle boundSamplerHeap{};
		std::vector<ResourceHandle> passColorResources;
		ResourceHandle passDepthResource{};
	};

	struct VulkanDevice {
		~VulkanDevice() = default;
		void Shutdown() noexcept;

		Device self{};
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		VkPhysicalDeviceFeatures supportedFeatures{};
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		uint32_t loaderApiVersion = VK_API_VERSION_1_0;
		uint32_t instanceApiVersion = VK_API_VERSION_1_0;
		bool swapchainExtensionEnabled = false;
		bool bufferDeviceAddressEnabled = false;
		bool descriptorHeapEnabled = false;
		bool dynamicRenderingEnabled = false;
		std::vector<VkQueueFamilyProperties> queueFamilyProperties;
		VulkanRegistry<VulkanDescriptorHeap> descriptorHeaps;
		VulkanRegistry<VulkanResource> resources;
		VulkanRegistry<VulkanSwapchain> swapchains;
		VulkanRegistry<VulkanCommandAllocator> allocators;
		VulkanRegistry<VulkanCommandList> commandLists;
		std::array<VulkanQueueState, 3> queues{};
		QueueHandle gfxHandle{ 0u, 1u };
		QueueHandle compHandle{ 1u, 1u };
		QueueHandle copyHandle{ 2u, 1u };
		std::weak_ptr<VulkanDevice> selfWeak;
	};

	extern const DeviceVTable g_vkdevvt;
	extern const QueueVTable g_vkqvt;
	extern const CommandListVTable g_vkclvt;
	extern const SwapchainVTable g_vkscvt;
	extern const CommandAllocatorVTable g_vkcalvt;
	extern const ResourceVTable g_vkbuf_rvt;
	extern const ResourceVTable g_vktex_rvt;
	extern const QueryPoolVTable g_vkqpvt;
	extern const PipelineVTable g_vkpsovt;
	extern const WorkGraphVTable g_vkwgvt;
	extern const PipelineLayoutVTable g_vkplvt;
	extern const CommandSignatureVTable g_vkcsvt;
	extern const DescriptorHeapVTable g_vkdhvt;
	extern const SamplerVTable g_vksvt;
	extern const TimelineVTable g_vktlvt;
	extern const HeapVTable g_vkhevt;

} // namespace rhi