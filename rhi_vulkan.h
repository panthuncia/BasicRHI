#pragma once

#include "rhi.h"

#include "volk.h"

#ifdef VOLK_NAMESPACE
// BasicRHI historically uses Vulkan entry points unqualified.  Volk's
// namespace mode is required when DX12 and Vulkan coexist in an executable;
// make its dispatch table visible without changing the public RHI surface.
using namespace volk;
#endif

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
	struct VulkanAccelerationStructure;
	struct VulkanQueueState;

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
	template<> struct VulkanHandleFor<VulkanAccelerationStructure> { using type = AccelerationStructureHandle; };
	template<> struct VulkanHandleFor<VulkanQueueState> { using type = QueueHandle; };

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
		// Graph compilation and command recording can lazily create backend
		// objects on worker threads.  In particular, concurrent deque growth
		// corrupts its block map even though references to existing elements are
		// otherwise stable.  Serialize registry metadata access; callers retain
		// the normal RHI lifetime responsibility for an object returned by get().
		mutable std::mutex mutex;

		HandleT alloc(const T& value) {
			const std::scoped_lock lock(mutex);
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
			const std::scoped_lock lock(mutex);
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
			const std::scoped_lock lock(mutex);
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
			const std::scoped_lock lock(mutex);
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
			const std::scoped_lock lock(mutex);
			slots.clear();
			freelist.clear();
		}
	};

	struct VulkanQueueState {
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t familyIndex = 0xFFFFFFFFu;
		uint32_t queueIndex = 0;
		void* tracyGpuContext = nullptr;
	};

	struct VulkanResource {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize memoryOffset = 0;
		void* mappedData = nullptr;
		uint32_t mapRefCount = 0;
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
			AccelerationStructure,
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
		AccelerationStructureHandle accelerationStructure{};
		VkDeviceAddress accelerationStructureDeviceAddress = 0;
		bool partitionedAccelerationStructure = false;
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
		VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
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
		bool isRayTracing = false;
		bool isRayTracingLibrary = false;
		uint32_t shaderGroupCount = 0;
		uint32_t rayTracingStackSize = 0;
	};

	struct VulkanAccelerationStructure {
		VkAccelerationStructureKHR accelerationStructure = VK_NULL_HANDLE;
		ResourceHandle storage{};
		uint64_t storageOffset = 0;
		uint64_t sizeBytes = 0;
		RayTracingAccelerationStructureType type = RayTracingAccelerationStructureType::BottomLevel;
		VkDeviceAddress deviceAddress = 0;
	};

	struct VulkanCommandSignature {
		std::vector<IndirectArg> args;
		uint32_t byteStride = 0;
		VkIndirectCommandsLayoutEXT indirectLayout = VK_NULL_HANDLE;
	};

	struct VulkanTimeline {
		VkSemaphore semaphore = VK_NULL_HANDLE;
		uint64_t lastSubmittedSignalValue = 0;
		uint64_t integrityCookie = 0x564B54494D454C4Eull;
		bool importedD3D12Fence = false;
		uint32_t activeHostWaits = 0;
	};

	struct VulkanHeap {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t memoryTypeIndex = 0;
		HeapType heapType = HeapType::DeviceLocal;
		bool externalD3D12 = false;
	};

	struct VulkanQueryPool {
		VkQueryPool pool = VK_NULL_HANDLE;
		QueryType type = QueryType::Timestamp;
		uint32_t count = 0;
		PipelineStatsMask statsMask = 0;
		VkQueryPipelineStatisticFlags vkStats = 0;
	};

	struct VulkanCommandList {
		struct GeneratedCommandsPreprocessPage {
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkDeviceAddress deviceAddress = 0;
			VkDeviceSize capacity = 0;
			VkDeviceSize cursor = 0;
			uint32_t memoryTypeIndex = UINT32_MAX;
		};

		struct TracyGpuZoneEvent {
			void* context = nullptr;
			uint32_t queryId = 0;
			bool begin = false;
			std::string name;
		};
		struct TracyGpuOpenZone {
			void* context = nullptr;
			uint32_t beginQueryId = 0;
		};

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
		// VK_EXT_device_generated_commands preprocess memory is writable scratch.
		// Keep unique ranges with the command list so overlapping recorded lists
		// (and distinct commands in one list) never alias the same scratch bytes.
		std::vector<GeneratedCommandsPreprocessPage> generatedCommandsPreprocessPages;
		std::vector<EmulatedRootConstantShadowState> emulatedRootConstantShadowStates;
		std::vector<VkQueryPool> transientQueryPools;
		std::vector<TracyGpuOpenZone> tracyGpuZoneStack;
		std::vector<TracyGpuZoneEvent> tracyGpuZoneEvents;
		bool tracyGpuZoneEventsSubmitted = false;
	};

	struct VulkanDevice {
		~VulkanDevice();
		void Shutdown() noexcept;

		Device self{};
		VkInstance instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
		struct ValidationMessage {
			VkDebugUtilsMessageSeverityFlagBitsEXT severity{};
			std::string text;
		};
		mutable std::mutex validationMessagesMutex;
		std::deque<ValidationMessage> validationMessages;
		uint64_t droppedValidationMessageCount = 0;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		VkPhysicalDeviceFeatures supportedFeatures{};
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
		VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR computeShaderDerivativesProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_PROPERTIES_KHR };
		VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
		VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
		VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
		VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
		VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipelineProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
#if defined(VK_NV_cluster_acceleration_structure)
		VkPhysicalDeviceClusterAccelerationStructureFeaturesNV clusterAccelerationStructureFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV };
		VkPhysicalDeviceClusterAccelerationStructurePropertiesNV clusterAccelerationStructureProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV };
#endif
#if defined(VK_NV_partitioned_acceleration_structure)
		VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV partitionedAccelerationStructureFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV };
		VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV partitionedAccelerationStructureProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_PROPERTIES_NV };
#endif
		uint32_t loaderApiVersion = VK_API_VERSION_1_0;
		uint32_t instanceApiVersion = VK_API_VERSION_1_0;
		bool swapchainExtensionEnabled = false;
		bool bufferDeviceAddressEnabled = false;
		bool bufferDeviceAddressCaptureReplayEnabled = false;
		bool timelineSemaphoreEnabled = false;
		bool externalMemoryWin32Enabled = false;
		bool externalSemaphoreWin32Enabled = false;
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
		bool deferredHostOperationsEnabled = false;
		bool accelerationStructureEnabled = false;
		bool rayTracingPipelineEnabled = false;
		bool rayQueryEnabled = false;
		bool rayTracingPipelineLibraryEnabled = false;
		bool clusterAccelerationStructureEnabled = false;
		bool partitionedAccelerationStructureEnabled = false;
		bool validateBarrierTransitions = false;
		bool streamlineInitialized = false;
		std::vector<VkQueueFamilyProperties> queueFamilyProperties;
		VulkanRegistry<VulkanDescriptorHeap> descriptorHeaps;
		// Image-view slots are updated by parallel graph materialization and swept
		// when resources retire. Registry lookup alone does not protect the nested
		// per-heap slot arrays or vkCreate/vkDestroyImageView pairs.
		mutable std::recursive_mutex descriptorViewsMutex;
		VulkanRegistry<VulkanResource> resources;
		// Vulkan requires host access to each VkDeviceMemory object to be
		// externally synchronized. Resource setup and upload mapping can occur on
		// parallel graph/setup threads, so serialize memory bind/map operations.
		// Vulkan memory-object creation, binding, mapping, and destruction can be
		// reached concurrently by async backing construction and deferred retirement.
		// Keep each compound lifetime operation atomic. Recursive locking lets the
		// compound helpers reuse the synchronized bind/map helpers safely.
		mutable std::recursive_mutex deviceMemoryMutex;
		struct HostMemoryMapping {
			void* base = nullptr;
			uint32_t refCount = 0;
		};
		std::unordered_map<VkDeviceMemory, HostMemoryMapping> hostMemoryMappings;
		VulkanRegistry<VulkanSwapchain> swapchains;
		VulkanRegistry<VulkanCommandAllocator> allocators;
		VulkanRegistry<VulkanPipeline> pipelines;
		VulkanRegistry<VulkanPipelineLayout> pipelineLayouts;
		VulkanRegistry<VulkanCommandSignature> commandSignatures;
		VulkanRegistry<VulkanTimeline> timelines;
		// Timeline completion is polled from retirement/streaming threads while the
		// render threads submit and deferred deletion can recycle registry slots.
		// Keep registry lookup and the Vulkan operation using the returned object in
		// one critical section so a slot cannot be destroyed or reused underneath it.
		mutable std::mutex timelinesMutex;
		std::condition_variable timelinesCondition;
		VulkanRegistry<VulkanHeap> heaps;
		VulkanRegistry<VulkanQueryPool> queryPools;
		// Host query-pool reset is externally synchronized by Vulkan, and graph
		// command lists are recorded concurrently.
		mutable std::mutex queryResetMutex;
		VulkanRegistry<VulkanAccelerationStructure> accelerationStructures;
		VulkanRegistry<VulkanCommandList> commandLists;
		VulkanRegistry<VulkanQueueState> queues;
		std::vector<uint32_t> queueFamilyNextQueueIndex;
		std::vector<std::vector<uint32_t>> queueFamilyFreeQueueIndices;
#if BASICRHI_ENABLE_RESHAPE
		std::unique_ptr<::Backend::Environment> reshapeEnvironment;
#endif
		QueueHandle gfxHandle{};
		QueueHandle compHandle{};
		QueueHandle copyHandle{};
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
	extern const AccelerationStructureVTable g_vkasvt;
	extern const PipelineVTable g_vkpsovt;
	extern const WorkGraphVTable g_vkwgvt;
	extern const PipelineLayoutVTable g_vkplvt;
	extern const CommandSignatureVTable g_vkcsvt;
	extern const DescriptorHeapVTable g_vkdhvt;
	extern const SamplerVTable g_vksvt;
	extern const TimelineVTable g_vktlvt;
	extern const HeapVTable g_vkhevt;

} // namespace rhi
