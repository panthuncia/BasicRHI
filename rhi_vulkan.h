#pragma once

#include "rhi.h"
#include "rhi_pipeline_cache.h"

#include "volk.h"

#ifdef VOLK_NAMESPACE
// BasicRHI historically uses Vulkan entry points unqualified.  Volk's
// namespace mode is required when DX12 and Vulkan coexist in an executable;
// make its dispatch table visible without changing the public RHI surface.
using namespace volk;
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
	struct VulkanIndirectPipelineSet;
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
	template<> struct VulkanHandleFor<VulkanIndirectPipelineSet> { using type = IndirectPipelineSetHandle; };
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
		// Registry mutations still serialize deque metadata. Readers retain the
		// normal RHI lifetime responsibility for an object returned by get().
		// Backend teardown occurs after releasing this lock. Parking blocked readers
		// avoids priority inversion if a short writer is preempted.
		mutable std::shared_mutex mutex;

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
			T retired{};
			{
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
				using std::swap;
				swap(retired, slot.obj);
				freelist.push_back(index);
			}
			// Vector/string teardown and allocator callbacks stay outside the
			// registry's exclusive critical section.
		}

		T* get(HandleT handle) {
			const std::shared_lock lock(mutex);
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
			const std::shared_lock lock(mutex);
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
		// Owned by the host of an adopted device: never idled or reused by BasicRHI.
		bool external = false;
		// One of the host's spare queues (AdoptedVulkanDeviceInfo::spareQueues): offered again when destroyed.
		bool spare = false;
		// Queue::Progress: a timeline every submission signals, made at the first; the last value submitted. Under timelinesMutex.
		TimelineHandle progress{};
		bool hasProgress = false;
		uint64_t progressSubmitted = 0;
	};

	struct VulkanImageViewSlot;
	struct VulkanResource {
		// Intrusive reverse list of descriptor views. The device's descriptorViewsMutex
		// protects both this head and the links in VulkanImageViewSlot.
		VulkanImageViewSlot* firstDescriptorView = nullptr;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize memoryOffset = 0;
		void* mappedData = nullptr;
		uint32_t mapRefCount = 0;
		VkDeviceAddress deviceAddress = 0;
		uint64_t bufferSize = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		// The format the resource was created with, when its image is stored in another (VulkanDevice::formatRemaps); Unknown for
		// an imported image.
		Format rhiFormat = Format::Unknown;
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
		// VK_SHARING_MODE_CONCURRENT: queue-family ownership barriers are ignored.
		bool concurrentSharing = false;
		// Simultaneous access (D3D12 RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, always COMMON): the image stays in
		// VK_IMAGE_LAYOUT_GENERAL, so views and attachments of it are used in GENERAL.
		bool generalLayoutOnly = false;
	};

	struct VulkanImageViewSlot {
		VulkanResource* descriptorResource = nullptr;
		VulkanImageViewSlot* previousResourceView = nullptr;
		VulkanImageViewSlot* nextResourceView = nullptr;
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
		// Descriptor-buffer model: element 0's byte offset (the universal set layout's binding offset), the buffer's usage
		// (vkCmdBindDescriptorBuffersEXT names it), and a CPU-only heap's descriptors, kept in host memory (mappedData points
		// into it) rather than in a descriptor buffer.
		uint64_t descriptorBaseOffset = 0;
		VkBufferUsageFlags descriptorBufferUsage = 0;
		std::vector<std::byte> hostDescriptors;
		std::vector<VulkanImageViewSlot> imageViewSlots;
		// A full-capacity shader-visible resource heap's buffer view ref slots, past `capacity` (VulkanDevice::ViewRefRegion).
		uint32_t viewRefSlots = 0;
	};

	struct VulkanSwapchain {
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		Format rhiFormat = Format::Unknown;
		// The format asked for at creation or resize. SrgbNonlinear presents in it; the HDR colour spaces fix their own.
		Format sdrFormat = Format::Unknown;
		SwapchainColorSpace colorSpace = SwapchainColorSpace::SrgbNonlinear;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t imageCount = 0;
		uint32_t currentImageIndex = 0;
		bool allowTearing = false;
		// The mode the swapchain was created with, or the last one a present switched to.
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
		// The surface's modes, and those a present can switch to without recreation (VK_EXT_swapchain_maintenance1;
		// empty without it).
		std::vector<VkPresentModeKHR> surfacePresentModes;
		std::vector<VkPresentModeKHR> switchablePresentModes;
		std::vector<VkImage> images;
		std::vector<ResourceHandle> imageHandles;
		std::vector<VkSemaphore> presentWaitSemaphores;
		// Acquire slots, one per acquire in flight. A slot's wait batch signals its fence, and the slot (its semaphore and
		// command buffer) is reused once that fence has signalled.
		std::vector<VkSemaphore> acquireSemaphores;
		std::vector<VkFence> acquireFences;
		std::vector<uint8_t> acquireFencePending;
		std::vector<VkCommandBuffer> acquireCommandBuffers;
		uint32_t nextAcquireSlot = 0;
		// Images still in VK_IMAGE_LAYOUT_UNDEFINED. Their first acquire batch moves them to PRESENT_SRC, the layout every
		// later use starts from (as D3D12 back buffers start in PRESENT).
		std::vector<uint8_t> imageNeedsInitialLayout;
		VkCommandPool acquireCommandPool = VK_NULL_HANDLE;
		uint32_t acquireCommandPoolFamily = 0xFFFFFFFFu;
		// The queue that presented last. The next acquire's wait goes on it, which orders the acquire before every later
		// submission there.
		QueueHandle presentQueue{};
		// The timeline the acquire batch signals, at acquireValue for the current image (Swapchain::AcquirePoint): other
		// queues wait on it. Created at the first acquire; none without timeline semaphores.
		TimelineHandle acquireTimeline{};
		bool hasAcquireTimeline = false;
		uint64_t acquireValue = 0;
		// Presented frames not yet known to have finished: each one's point on its queue's progress timeline.
		struct PendingFrame {
			TimelineHandle timeline{};
			uint64_t value = 0;
			uint64_t presentId = 0;
		};
		std::deque<PendingFrame> pendingFrames;
		uint32_t maxFrameLatency = 2;
		uint64_t presentCount = 0;
		bool hasHdrMetadata = false;
		HdrMetadata hdrMetadata{};
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
		// Descriptor-buffer model (VulkanBindingModel::DescriptorBuffer): push data bytes [0, pushedBytes) are push constants of
		// the universal pipeline layout, the rest are spilled to memory whose address is the push constant at
		// spillAddressOffset (0xFFFFFFFF: nothing spilled); and the sampler heap element of each static sampler's first
		// element (the static-sampler tail, VulkanDevice::descriptorBuffer).
		bool usesDescriptorBuffer = false;
		uint32_t pushedBytes = 0;
		uint32_t spillAddressOffset = 0xFFFFFFFFu;
		std::vector<uint32_t> staticSamplerElements;
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
		bool indirectBindable = false;       // created with PipelineFlags_IndirectBindable
		bool dynamicVertexStride = false;    // vertex input strides are dynamic state
		bool dynamicStencilReference = false; // the stencil reference is dynamic state (stencil test on)
		bool dynamicBlendConstants = false;  // the blend constants are dynamic state (a blend reads them)
		VkShaderStageFlags shaderStages = 0; // stages the pipeline contains
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
		IndirectPipelineSetHandle pipelineSet{};  // set a PipelineIndex argument selects from
		bool explicitPreprocess = false;  // the layout has EXPLICIT_PREPROCESS: every execution is preprocessed first
	};

	struct VulkanIndirectPipelineSet {
		VkIndirectExecutionSetEXT executionSet = VK_NULL_HANDLE;
		PipelineHandle initialPipeline{};
		uint32_t maxPipelineCount = 0;
		VkShaderStageFlags shaderStages = 0;
		VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	};

	struct VulkanTimeline {
		VkSemaphore semaphore = VK_NULL_HANDLE;
		uint64_t lastSubmittedSignalValue = 0;
		uint64_t integrityCookie = 0x564B54494D454C4Eull;
		bool importedD3D12Fence = false;
		bool poisonReported = false;  // the UINT64_MAX read is logged once per timeline (a lost device reads it on every poll)
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
		// Native crash markers (VulkanCrashMarkers): the list's slot pair, given at its first marker, and its open debug
		// labels' marker names.
		uint32_t crashMarkerSlot = UINT32_MAX;
		std::vector<const char*> crashLabelStack;
		// A secondary (VK_COMMAND_BUFFER_LEVEL_SECONDARY, Device::CreateSecondaryCommandList).
		bool secondary = false;
		// This recording of a secondary continues the executing primary's rendering (BeginSecondary with insideRendering):
		// passActive is set by BeginSecondary and the rendering is never ended here. Cleared by Recycle.
		bool continuesRendering = false;
		// A foreign VkCommandBuffer wrapped by AdoptCommandBuffer: begun, ended, reset, submitted and freed by its owner.
		bool adopted = false;
		bool passActive = false;
		PipelineLayoutHandle boundLayout{};
		PipelineHandle boundPipeline{};
		// The stencil reference set with SetStencilRef (0 until then). Applied: the buffer holds it. A bound pipeline with
		// the state static, or executed secondaries, lose it, and the next pipeline that has it dynamic sets it again.
		uint32_t stencilReference = 0;
		bool stencilReferenceApplied = false;
		// The same for the blend constants (SetBlendFactor; {1, 1, 1, 1} until then).
		float blendConstants[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool blendConstantsApplied = false;
		DescriptorHeapHandle boundCbvSrvUavHeap{};
		DescriptorHeapHandle boundSamplerHeap{};
		// Diagnostics (DiagnosticDedupeHeapBinds): the heaps the list's last bind emitted; cleared where the binding may be lost.
		DescriptorHeapHandle emittedCbvSrvUavHeap{};
		DescriptorHeapHandle emittedSamplerHeap{};
		VkRect2D passRenderArea{};
		// The scissors as last set (SetScissors, or the render area by BeginPass and BeginSecondary), and a bit per viewport
		// last set with no area. Vulkan takes no such viewport: it is set as 1x1 with an empty scissor, and the stored
		// scissor comes back once the viewport has an area again.
		std::array<VkRect2D, MAX_VIEWPORTS> scissors{};
		uint32_t emptyViewportMask = 0;
		// Secondaries were executed inside this list's pass, where only vkCmdExecuteCommands may be recorded: EndPass binds
		// the placeholder vertex and index buffers.
		bool placeholderInputBindPending = false;
		std::vector<ResourceHandle> passColorResources;
		ResourceHandle passDepthResource{};
		std::vector<RecordedBarrierBatch> recordedBarrierBatches;
		std::vector<RecordingTextureState> recordingTextureStates;
		std::vector<RecordingBufferState> recordingBufferStates;
		std::vector<EmulatedRootConstantScratchPage> emulatedRootConstantScratchPages;
		// VK_EXT_device_generated_commands preprocess memory is writable scratch.
		// Keep unique ranges with the command list so overlapping recorded lists
		// (and distinct commands in one list) never alias the same scratch bytes.
		// Held until the list's reset, which gives them back to the device's pool (VulkanDevice::preprocessPool).
		std::vector<GeneratedCommandsPreprocessPage> generatedCommandsPreprocessPages;
		// Explicit preprocesses recorded in this list and not yet executed, each consumed by the one execution it matches.
		struct PreprocessedIndirect {
			CommandSignatureHandle signature{};
			ResourceHandle argumentBuffer{};
			uint64_t argumentOffset = 0;
			ResourceHandle countBuffer{};
			uint64_t countOffset = 0;
			uint32_t maxCommandCount = 0;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkDeviceAddress preprocessAddress = 0;
			VkDeviceSize preprocessSize = 0;
			// Descriptor-buffer model: the universal layout's whole push constant range (VulkanDescriptorBufferModel::
			// pushConstantBytes of it) as the preprocess saw it - the pushed prefix and the spill address, whose scratch is this
			// list's - pushed again, unchanged, before the execution (VUID-vkCmdExecuteGeneratedCommandsEXT-isPreprocessed-11048).
			std::array<uint8_t, 256> pushConstants{};
			bool pushConstantsRecorded = false;
		};
		std::vector<PreprocessedIndirect> preprocessedIndirect;
		// A preprocess was recorded since the last barrier making preprocess output visible to executions.
		bool preprocessBarrierPending = false;
		std::vector<EmulatedRootConstantShadowState> emulatedRootConstantShadowStates;
		// Descriptor-buffer model: the bound layout's push data image as last written (PushConstants). Its pushed prefix is
		// recorded as push constants when written; pushedDataDirty asks for it again (after ExecuteCommands or generated
		// commands left push constants undefined). Its spilled rest is uploaded to emulated-root-constant scratch and its
		// address pushed before the next draw, dispatch, trace or ExecuteIndirect when spilledDataDirty. Descriptor-heap model:
		// the push data as written, pushed again when pushedDataDirty (after generated commands left it undefined).
		std::vector<uint8_t> pushDataShadow;
		bool pushedDataDirty = false;
		bool spilledDataDirty = false;
		std::vector<VkQueryPool> transientQueryPools;
		std::vector<TracyGpuOpenZone> tracyGpuZoneStack;
		std::vector<TracyGpuZoneEvent> tracyGpuZoneEvents;
		bool tracyGpuZoneEventsSubmitted = false;
	};

	// How shaders reach descriptor heaps; fixed for a device (rhi_interop_vulkan.h BindingModelBits).
	enum class VulkanBindingModel : uint8_t {
		None,              // neither: pipelines and pipeline layouts are unsupported
		DescriptorHeap,    // VK_EXT_descriptor_heap: heaps are bound as they are, shaders keep DXC's bindings (set/binding mappings)
		DescriptorBuffer,  // VK_EXT_descriptor_buffer + VK_EXT_mutable_descriptor_type: one universal pipeline layout, shaders lowered
	};

	// The descriptor-buffer model's device state (VulkanBindingModel::DescriptorBuffer), set up once the VkDevice exists.
	struct VulkanDescriptorBufferModel {
		// Static samplers follow the caller's samplers, at elements [samplerCapacity, samplerCapacity + staticSamplerCapacity) of
		// every shader-visible sampler heap, so that a lowered shader's constant element is the same whichever sampler heap is
		// bound.
		static constexpr uint32_t kMaxStaticSamplers = 256;
		// The universal pipeline layout: set 0 the resource heap (one MUTABLE_EXT variable-count binding), set 1 the sampler
		// heap (one SAMPLER variable-count binding), push constants [0, pushConstantBytes) for every stage: the device's
		// maxPushConstantsSize, at least the base spec's 128 and at most kMaxPushConstantBytes. Spilled push data is read through
		// pointers, which some drivers load per lane (AMD's: never scalar loads), so a device's whole push range is used.
		static constexpr uint32_t kMinPushConstantBytes = 128;
		static constexpr uint32_t kMaxPushConstantBytes = 256;
		uint32_t pushConstantBytes = kMinPushConstantBytes;
		// A layout whose push data exceeds pushConstantBytes pushes the ranges within pushedPrefixBytes and spills the rest;
		// the spill image's address is the push constant at spillAddressOffset (the range's last 8 bytes).
		uint32_t pushedPrefixBytes = kMinPushConstantBytes - 8;
		uint32_t spillAddressOffset = kMinPushConstantBytes - 8;
		VkDescriptorSetLayout resourceSetLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		// The resource heap's mutable type list includes uniform buffers (else CBVs are written as storage buffers) and
		// acceleration structures.
		bool uniformBuffers = false;
		bool accelerationStructures = false;
		uint32_t resourceCapacity = 0;        // descriptors of set 0: the largest shader-visible resource heap (the chosen capacity)
		uint32_t samplerCapacity = 0;         // the largest shader-visible sampler heap's caller samplers (the chosen capacity)
		uint32_t staticSamplerCapacity = 0;   // the static-sampler tail past samplerCapacity
		uint64_t resourceStride = 0;          // bytes between consecutive elements (vkGetDescriptorSetLayoutSizeEXT)
		uint64_t samplerStride = 0;
		uint64_t resourceBindingOffset = 0;   // vkGetDescriptorSetLayoutBindingOffsetEXT of binding 0
		uint64_t samplerBindingOffset = 0;
		// vkGetDescriptorEXT data sizes, robust ones where robustBufferAccess is enabled.
		size_t samplerSize = 0, sampledImageSize = 0, storageImageSize = 0, uniformTexelSize = 0, storageTexelSize = 0;
		size_t uniformBufferSize = 0, storageBufferSize = 0, accelerationStructureSize = 0;
		// Distinct VkSamplers BasicRHI may create: maxSamplerAllocationCount less the host's (AdoptedVulkanDeviceInfo::
		// hostSamplerObjects).
		uint32_t samplerBudget = 0;
		// One VkSampler per distinct sampler description, shared by every slot (and static sampler) that describes it.
		struct CachedSampler {
			VkSampler sampler = VK_NULL_HANDLE;
			uint32_t references = 0;
		};
		std::mutex samplerCacheMutex;
		std::unordered_map<std::string, CachedSampler> samplerCache;
		std::unordered_map<VkSampler, std::string> samplerCacheKeys;
		// Static samplers registered by pipeline layouts (deduplicated), each at tail elements [element, element + count) of
		// every shader-visible sampler heap. Guarded by the device's descriptorViewsMutex.
		struct StaticSampler {
			std::string key;
			uint32_t count = 0;
			uint32_t element = 0;
			VkSampler sampler = VK_NULL_HANDLE;
		};
		std::vector<StaticSampler> staticSamplers;
		uint32_t staticSamplersUsed = 0;
	};

	// Native crash diagnostics on a device BasicRHI created (DeviceCreateInfo::enableCrashMarkers or
	// BASICRHI_VK_CRASH_MARKERS=1). Markers are written at command list begin and end and at every debug label, with
	// VK_AMD_buffer_marker and VK_NV_device_diagnostic_checkpoints when present. VK_EXT_device_fault is queried on device
	// loss. An adopted device uses its host's checkpoint callbacks instead (VulkanDevice::registerCheckpoint).
	struct VulkanCrashMarkers {
		// Command lists that hold a marker slot pair: the last marker the GPU began (top of pipe) and the last it finished
		// (bottom of pipe). Lists past this many share slots.
		static constexpr uint32_t kSlots = 1024;
		// Marker ids remembered with their names; older ids show as overwritten.
		static constexpr uint32_t kHistory = 8192;
		bool enabled = false;
		bool deviceFault = false;
		bool deviceFaultVendorBinary = false;
		bool bufferMarkers = false;
		bool checkpoints = false;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		volatile uint32_t* mapped = nullptr;  // kSlots pairs of { begun, finished } marker ids
		std::atomic<uint32_t> nextSlot{ 0 };
		std::atomic<bool> lossReported{ false };
		std::atomic<bool> lossNotified{ false };  // the host's loss callback ran (VkReportDeviceLoss)
		std::mutex mutex;  // guards everything below
		uint32_t nextId = 1;
		std::array<uint32_t, kHistory> historyIds{};
		std::array<const char*, kHistory> historyNames{};
		std::array<const char*, kSlots> slotNames{};  // the command list's name (SetName) per slot
		std::unordered_set<std::string> names;  // interned, so the pointers above stay valid
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
		// The formats this device stores in another (FormatSupport), the rules DXVK's format table uses: VK_FORMAT_D24_UNORM_S8_UINT
		// as D32_SFLOAT_S8_UINT where it is no depth attachment (AMD), VK_FORMAT_A8_UNORM_KHR as R8 with its red read as alpha where
		// A8 cannot be sampled, stored and rendered to. Set once at creation (VkApplyEnabledDeviceState); every conversion reads it.
		struct FormatRemap {
			VkFormat from = VK_FORMAT_UNDEFINED;
			VkFormat to = VK_FORMAT_UNDEFINED;
			// Makes the format's channels from the stored ones (identity: none).
			VkComponentMapping swizzle{};
		};
		std::vector<FormatRemap> formatRemaps;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		VkPhysicalDeviceFeatures supportedFeatures{};
		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
		VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorBufferProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT };
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
		// Present extensions, each enabled when the device has it (VkEnablePresentExtensions). VK_KHR_present_id and
		// VK_KHR_present_wait are left off under Streamline, whose interposer may not pass a present ID through.
		bool swapchainColorSpaceEnabled = false;      // instance VK_EXT_swapchain_colorspace
		bool surfaceMaintenance1Enabled = false;      // instance VK_EXT_surface_maintenance1 + VK_KHR_get_surface_capabilities2
		bool swapchainMaintenance1Enabled = false;    // VK_EXT_swapchain_maintenance1 (or the KHR promotion)
		bool presentIdEnabled = false;                // VK_KHR_present_id
		bool presentWaitEnabled = false;              // VK_KHR_present_wait
		bool hdrMetadataEnabled = false;              // VK_EXT_hdr_metadata
		bool bufferDeviceAddressEnabled = false;
		bool bufferDeviceAddressCaptureReplayEnabled = false;
		bool timelineSemaphoreEnabled = false;
		bool externalMemoryWin32Enabled = false;
		bool externalSemaphoreWin32Enabled = false;
		bool descriptorIndexingEnabled = false;
		bool runtimeDescriptorArrayEnabled = false;
		bool scalarBlockLayoutEnabled = false;
		bool descriptorHeapEnabled = false;
		bool descriptorBufferEnabled = false;          // VK_EXT_descriptor_buffer descriptorBuffer
		bool mutableDescriptorTypeEnabled = false;     // VK_EXT_mutable_descriptor_type (or the VALVE alias)
		bool variableDescriptorCountEnabled = false;   // descriptorBindingVariableDescriptorCount
		VulkanBindingModel bindingModel = VulkanBindingModel::None;
		// The shader-visible heap capacities the device was set up with (min(DescriptorHeapCapacities preferred, the device's
		// limit) for its binding model; ShaderFeatureInfo reports them). Larger shader-visible heaps fail creation.
		uint32_t shaderVisibleResourceCapacity = 0;
		uint32_t shaderVisibleSamplerCapacity = 0;
		VulkanDescriptorBufferModel descriptorBuffer;
		// Buffer view refs (rhi_shader_abi.h, BUFFER_VIEW_REF_*): BUFFER_VIEW_REF_SLOTS slots past shaderVisibleResourceCapacity in
		// every shader-visible resource heap of that capacity. A pair is two storage buffer descriptors over the buffer's address
		// range, written once into `staging` and copied into each heap. Heaps and staging under descriptorViewsMutex; the map
		// also under `lock`, so a ref already made is found without the views mutex.
		struct ViewRefRegion {
			std::shared_mutex lock;
			bool ready = false;
			uint64_t stride = 0;      // the heaps' descriptor stride
			uint64_t descriptorSize = 0;  // a storage buffer descriptor's bytes
			std::vector<std::byte> staging;  // BUFFER_VIEW_REF_SLOTS descriptors, `stride` bytes each
			std::vector<DescriptorHeapHandle> heaps;
			std::unordered_map<uint64_t, uint32_t> pairByResource;  // ResourceHandle index and generation -> pair
			std::vector<uint32_t> freePairs;
			uint32_t nextPair = 0;
			bool loggedFull = false;
		} viewRefs;
		bool nullDescriptorEnabled = false;  // VK_EXT_robustness2 nullDescriptor: views with no resource
		bool descriptorHeapCaptureReplayEnabled = false;
		bool meshShaderEnabled = false;
		bool taskShaderEnabled = false;
		bool meshShaderPipelineStatsEnabled = false;
		bool deviceGeneratedCommandsEnabled = false;
		bool drawIndirectCountEnabled = false;  // Vulkan 1.2 drawIndirectCount: vkCmdDraw*IndirectCount
		bool dynamicGeneratedPipelineLayoutEnabled = false;
		bool dynamicRenderingEnabled = false;
		// VK_KHR_maintenance5: pipeline create flags go in VkPipelineCreateFlags2CreateInfo; without it (AMD's legacy Polaris driver)
		// in the legacy flags (VkChainPipelineCreateFlags).
		bool maintenance5Enabled = false;
		// VK_KHR_pipeline_executable_properties' pipelineExecutableInfo: the debugging switches' (DXVK_PIPELINE_STATS, BASICRHI_VK_DUMP_ISA).
		bool pipelineExecutableInfoEnabled = false;
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
		// VK_EXT_debug_utils was enabled on the instance: object names and command buffer labels may
		// be issued. volk resolves the entry points whether or not it was, so a null check is not enough.
		bool debugUtilsEnabled = false;
		// VK_EXT_memory_budget: per-heap budgets and usage for QueryVideoMemoryInfo and QueryMemoryHeapBudgets.
		bool memoryBudgetEnabled = false;
		VulkanCrashMarkers crashMarkers;
		// A small vertex and index buffer that ExecuteCommands binds after secondaries (VkBindPlaceholderInputBuffers).
		// Created at the first ExecuteCommands; never read.
		std::once_flag placeholderInputBufferOnce;
		VkBuffer placeholderInputBuffer = VK_NULL_HANDLE;
		VkDeviceMemory placeholderInputMemory = VK_NULL_HANDLE;
		PFN_vkCmdSetCheckpointNV setCheckpoint = nullptr;
		void* checkpointUser = nullptr;
		const void* (*registerCheckpoint)(void*, const char*) noexcept = nullptr;
		bool detailedCheckpoints = false;
		// AdoptVulkanDevice: the instance and device belong to the host. Every VkQueue
		// access is bracketed by the host's submission lock, and whole-device waits are
		// replaced by waits on BasicRHI's own timelines.
		bool ownsInstance = true;
		bool ownsDevice = true;
		// The host destroyed the VkDevice before BasicRHI shut down: skip every vkDestroy*.
		bool abandoned = false;
		// ShareVulkanDevice: a device BasicRHI created that a guest API also submits to. Queue access takes the submission hooks
		// and whole-device waits become waits on BasicRHI's timelines, as on an adopted device; BasicRHI still destroys it.
		bool sharedQueues = false;
		// The queue hooks (vulkan::QueueSubmissionHooks), or null. A published set is never changed: ShareVulkanDevice
		// publishes a new one while other threads may still be inside the old one's lock, so each access reads the pointer
		// once, and every set ever published lives until the device is destroyed.
		struct SubmissionHooks {
			void* user = nullptr;
			void (*lock)(void* user, VkQueue queue) = nullptr;
			void (*unlock)(void* user, VkQueue queue) = nullptr;
			VkResult (*submit)(void* user, VkQueue queue, const VkSubmitInfo2& submitInfo) = nullptr;
		};
		std::atomic<const SubmissionHooks*> submissionHooks{ nullptr };
		std::mutex submissionHooksMutex;
		std::vector<std::unique_ptr<const SubmissionHooks>> publishedSubmissionHooks;
		std::vector<VkQueueFamilyProperties> queueFamilyProperties;
		// AdoptVulkanDevice's spare queues (AdoptedVulkanDeviceInfo::spareQueues) not handed out, and the families of all of
		// them, which Concurrent resources share with besides the primary queues'.
		std::mutex spareQueuesMutex;
		std::vector<VulkanQueueState> spareQueues;
		std::vector<uint32_t> spareQueueFamilies;
		VulkanRegistry<VulkanDescriptorHeap> descriptorHeaps;
		// Image-view slots are updated by parallel graph materialization. The
		// intrusive resource-to-view lists and vkCreate/vkDestroy pairs are
		// protected here; retiring a resource never scans descriptor heaps.
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
		// VK_EXT_device_generated_commands preprocess pages no command list holds. A list gives its pages back at its reset (its
		// buffer is not pending then, so the GPU is done with them) and takes ranges from them before allocating, so the memory
		// is what the lists not yet reset hold, not every page a pooled list ever needed. A page left here for
		// kPreprocessPoolIdleResets resets (a size the lists outgrew) is freed.
		// The pool also holds at most kPreprocessPoolIdleBytes: past that, the pages given back longest ago are freed first.
		static constexpr uint64_t kPreprocessPoolIdleResets = 4096;
		static constexpr uint64_t kPreprocessPoolIdleBytes = 64ull * 1024ull * 1024ull;
		struct PooledPreprocessPage {
			VulkanCommandList::GeneratedCommandsPreprocessPage page;
			uint64_t returnedAt = 0;  // preprocessPoolResets when it came back
		};
		std::mutex preprocessPoolMutex;
		std::vector<PooledPreprocessPage> preprocessPool;
		uint64_t preprocessPoolResets = 0;
		uint64_t preprocessPoolBytes = 0;
		// Emulated root constants' scratch pages no command list holds, kept the same way: a list gives its pages back at its reset
		// and takes them from here before allocating. So a list that never spills its constants holds no page, and the memory is
		// what the lists not yet reset hold. Bounded by resets and by bytes like the preprocess pool.
		static constexpr uint64_t kScratchPoolIdleResets = 4096;
		static constexpr uint64_t kScratchPoolIdleBytes = 16ull * 1024ull * 1024ull;
		struct PooledScratchPage {
			VulkanCommandList::EmulatedRootConstantScratchPage page;
			uint64_t returnedAt = 0;  // scratchPoolResets when it came back
		};
		std::mutex scratchPoolMutex;
		std::vector<PooledScratchPage> scratchPool;
		uint64_t scratchPoolResets = 0;
		uint64_t scratchPoolBytes = 0;
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
		VulkanRegistry<VulkanIndirectPipelineSet> indirectPipelineSets;
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
		// The on-disk pipeline cache (Device::OpenPipelineCache). Pipeline creation reads `cache` under a shared lock, so
		// threads create pipelines in parallel; opening and closing it take the lock exclusively.
		struct PipelineCacheState {
			mutable std::shared_mutex mutex;
			VkPipelineCache cache = VK_NULL_HANDLE;
			std::string path;
			detail::PipelineCacheIdentity identity{};
			// VK_EXT_pipeline_creation_feedback (core in 1.3) tells hits from misses.
			bool creationFeedback = false;
			std::mutex saveMutex;
			detail::PipelineCacheCounters counters;
		} pipelineCache;
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
	extern const IndirectPipelineSetVTable g_vkipsvt;
	extern const DescriptorHeapVTable g_vkdhvt;
	extern const SamplerVTable g_vksvt;
	extern const TimelineVTable g_vktlvt;
	extern const HeapVTable g_vkhevt;

} // namespace rhi
