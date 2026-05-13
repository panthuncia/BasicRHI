#pragma once

#include "rhi.h"

#include "volk.h"

#include <array>
#include <deque>
#include <memory>

#include <vector>

#if BASICRHI_ENABLE_RESHAPE
namespace Backend { class Environment; }
#endif

namespace rhi {
	template<class Obj> struct VulkanHandleFor;

	struct VulkanDescriptorHeap;
	struct VulkanResource;
	struct VulkanSwapchain;
	struct VulkanCommandAllocator;
	struct VulkanCommandList;
	struct VulkanPipeline;
	struct VulkanPipelineLayout;
	struct VulkanCommandSignature;
	struct VulkanTimeline;
	struct VulkanHeap;
	struct VulkanQueryPool;

	template<> struct VulkanHandleFor<VulkanDescriptorHeap> { using type = DescriptorHeapHandle; };
	template<> struct VulkanHandleFor<VulkanResource> { using type = ResourceHandle; };
	template<> struct VulkanHandleFor<VulkanSwapchain> { using type = SwapChainHandle; };
	template<> struct VulkanHandleFor<VulkanCommandAllocator> { using type = CommandAllocatorHandle; };
	template<> struct VulkanHandleFor<VulkanCommandList> { using type = CommandListHandle; };
	template<> struct VulkanHandleFor<VulkanPipeline> { using type = PipelineHandle; };
	template<> struct VulkanHandleFor<VulkanPipelineLayout> { using type = PipelineLayoutHandle; };
	template<> struct VulkanHandleFor<VulkanCommandSignature> { using type = CommandSignatureHandle; };
	template<> struct VulkanHandleFor<VulkanTimeline> { using type = TimelineHandle; };
	template<> struct VulkanHandleFor<VulkanHeap> { using type = HeapHandle; };
	template<> struct VulkanHandleFor<VulkanQueryPool> { using type = QueryPoolHandle; };

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
		ResourceAccessType currentAccess = ResourceAccessType::Common;
		ResourceLayout currentLayout = ResourceLayout::Undefined;
		ResourceSyncState currentSync = ResourceSyncState::All;
		ResourceAccessType submittedAccess = ResourceAccessType::Common;
		ResourceLayout submittedLayout = ResourceLayout::Undefined;
		ResourceSyncState submittedSync = ResourceSyncState::All;
		uint32_t width = 0;
		uint32_t height = 0;
		uint16_t depthOrLayers = 1;
		uint16_t mipLevels = 1;
		VkImageCreateFlags imageCreateFlags = 0;
		VkImageUsageFlags imageUsage = 0;
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
		std::vector<VkSemaphore> presentWaitSemaphores;
		VkFence acquireFence = VK_NULL_HANDLE;
	};

	struct VulkanCommandAllocator {
		VkCommandPool pool = VK_NULL_HANDLE;
		QueueKind kind = QueueKind::Graphics;
		uint32_t familyIndex = 0xFFFFFFFFu;
	};

	struct VulkanPushConstantRange {
		PushConstantRangeDesc desc{};
		uint32_t byteOffset = 0;
		uint32_t byteSize = 0;
		uint32_t dataByteSize = 0;
	};

	struct VulkanPipelineLayout {
		PipelineLayoutFlags flags = PF_None;
		std::vector<LayoutBindingRange> ranges;
		std::vector<PushConstantRangeDesc> pushConstants;
		std::vector<StaticSamplerDesc> staticSamplers;
		std::vector<VulkanPushConstantRange> pushConstantRanges;
		uint32_t totalPushDataBytes = 0;
		bool usesDescriptorHeap = false;
	};

	struct VulkanPipeline {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
		PipelineLayoutHandle rhiLayout{};
		bool isCompute = false;
	};

	struct VulkanCommandSignature {
		std::vector<IndirectArg> args;
		uint32_t byteStride = 0;
		VkIndirectCommandsLayoutEXT indirectLayout = VK_NULL_HANDLE;
		bool usesExecutionSet = false;
		VkIndirectExecutionSetEXT executionSet = VK_NULL_HANDLE;
		VkPipeline executionSetPipeline = VK_NULL_HANDLE;
		VkBuffer preprocessBuffer = VK_NULL_HANDLE;
		VkDeviceMemory preprocessMemory = VK_NULL_HANDLE;
		VkDeviceAddress preprocessAddress = 0;
		VkDeviceSize preprocessSize = 0;
		uint32_t preprocessMaxSequenceCount = 0;
	};

	struct VulkanTimeline {
		VkSemaphore semaphore = VK_NULL_HANDLE;
	};

	struct VulkanHeap {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t memoryTypeIndex = 0;
		HeapType heapType = HeapType::DeviceLocal;
	};

	struct VulkanQueryPool {
		VkQueryPool pool = VK_NULL_HANDLE;
		QueryType type = QueryType::Timestamp;
		uint32_t count = 0;
		PipelineStatsMask statsMask = 0;
		VkQueryPipelineStatisticFlags vkStats = 0;
	};

	struct VulkanCommandList {
		struct RecordedTextureBarrier {
			ResourceHandle texture{};
			ResourceAccessType beforeAccess = ResourceAccessType::Common;
			ResourceAccessType afterAccess = ResourceAccessType::Common;
			ResourceLayout beforeLayout = ResourceLayout::Common;
			ResourceLayout afterLayout = ResourceLayout::Common;
			ResourceSyncState beforeSync = ResourceSyncState::All;
			ResourceSyncState afterSync = ResourceSyncState::All;
			bool discard = false;
		};

		struct RecordedBufferBarrier {
			ResourceHandle buffer{};
			ResourceAccessType beforeAccess = ResourceAccessType::Common;
			ResourceAccessType afterAccess = ResourceAccessType::Common;
			ResourceSyncState beforeSync = ResourceSyncState::All;
			ResourceSyncState afterSync = ResourceSyncState::All;
			bool discard = false;
		};

		struct RecordedBarrierBatch {
			std::vector<RecordedTextureBarrier> textures;
			std::vector<RecordedBufferBarrier> buffers;
		};

		struct RecordingTextureState {
			ResourceHandle texture{};
			ResourceAccessType access = ResourceAccessType::Common;
			ResourceLayout layout = ResourceLayout::Undefined;
			ResourceSyncState sync = ResourceSyncState::All;
		};

		struct RecordingBufferState {
			ResourceHandle buffer{};
			ResourceAccessType access = ResourceAccessType::Common;
			ResourceSyncState sync = ResourceSyncState::All;
		};

		struct EmulatedRootConstantScratchPage {
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void* mappedData = nullptr;
			VkDeviceAddress deviceAddress = 0;
			uint32_t capacity = 0;
			uint32_t cursor = 0;
		};

		struct EmulatedRootConstantShadowState {
			uint32_t set = 0;
			uint32_t binding = 0;
			std::vector<uint32_t> values;
		};

		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		CommandAllocatorHandle allocatorHandle{};
		QueueKind kind = QueueKind::Graphics;
		Result pendingError = Result::Ok;
		bool isRecording = false;
		bool passActive = false;
		PipelineLayoutHandle boundLayout{};
		PipelineHandle boundPipeline{};
		DescriptorHeapHandle boundCbvSrvUavHeap{};
		DescriptorHeapHandle boundSamplerHeap{};
		VkRect2D passRenderArea{};
		std::vector<ResourceHandle> passColorResources;
		ResourceHandle passDepthResource{};
		std::vector<RecordedBarrierBatch> recordedBarrierBatches;
		std::vector<RecordingTextureState> recordingTextureStates;
		std::vector<RecordingBufferState> recordingBufferStates;
		std::vector<EmulatedRootConstantScratchPage> emulatedRootConstantScratchPages;
		std::vector<EmulatedRootConstantShadowState> emulatedRootConstantShadowStates;
	};

	struct VulkanDevice {
		~VulkanDevice();
		void Shutdown() noexcept;

		Device self{};
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		VkPhysicalDeviceFeatures supportedFeatures{};
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
		VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR computeShaderDerivativesProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_PROPERTIES_KHR };
		uint32_t loaderApiVersion = VK_API_VERSION_1_0;
		uint32_t instanceApiVersion = VK_API_VERSION_1_0;
		bool swapchainExtensionEnabled = false;
		bool bufferDeviceAddressEnabled = false;
		bool bufferDeviceAddressCaptureReplayEnabled = false;
		bool timelineSemaphoreEnabled = false;
		bool descriptorIndexingEnabled = false;
		bool runtimeDescriptorArrayEnabled = false;
		bool scalarBlockLayoutEnabled = false;
		bool descriptorHeapEnabled = false;
		bool descriptorHeapCaptureReplayEnabled = false;
		bool meshShaderEnabled = false;
		bool taskShaderEnabled = false;
		bool meshShaderPipelineStatsEnabled = false;
		bool deviceGeneratedCommandsEnabled = false;
		bool dynamicGeneratedPipelineLayoutEnabled = false;
		bool dynamicRenderingEnabled = false;
		bool shaderDemoteToHelperInvocationEnabled = false;
		bool computeDerivativeGroupQuadsEnabled = false;
		bool computeDerivativeGroupLinearEnabled = false;
		bool shaderImageInt64AtomicsEnabled = false;
		bool shaderSubgroupPartitionedEnabled = false;
		bool validateBarrierTransitions = false;
		bool streamlineInitialized = false;
		std::vector<VkQueueFamilyProperties> queueFamilyProperties;
		VulkanRegistry<VulkanDescriptorHeap> descriptorHeaps;
		VulkanRegistry<VulkanResource> resources;
		VulkanRegistry<VulkanSwapchain> swapchains;
		VulkanRegistry<VulkanCommandAllocator> allocators;
		VulkanRegistry<VulkanPipeline> pipelines;
		VulkanRegistry<VulkanPipelineLayout> pipelineLayouts;
		VulkanRegistry<VulkanCommandSignature> commandSignatures;
		VulkanRegistry<VulkanTimeline> timelines;
		VulkanRegistry<VulkanHeap> heaps;
		VulkanRegistry<VulkanQueryPool> queryPools;
		VulkanRegistry<VulkanCommandList> commandLists;
		std::array<VulkanQueueState, 3> queues{};
#if BASICRHI_ENABLE_RESHAPE
		std::unique_ptr<::Backend::Environment> reshapeEnvironment;
#endif
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