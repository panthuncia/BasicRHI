#pragma once

#include <cstdint>
#include <vector>

#include "rhi.h"

namespace rhi {
	enum class NativeBackend : uint32_t { Unknown, D3D12, Vulkan };
	enum class ExternalHandleType : uint8_t {
		Unknown,
		D3D12Resource,
		D3D12Heap,
		D3D12Fence
	};
	enum class ExternalHandleImportOwnership : uint8_t {
		RetainedByCaller,
		ConsumedOnSuccessfulImport
	};

	class ExternalHandle {
	public:
		using CloseFunction = void(*)(void*) noexcept;

		ExternalHandle() = default;
		ExternalHandle(void* value, ExternalHandleType type, ExternalHandleImportOwnership importOwnership, CloseFunction close) noexcept
			: m_value(value), m_type(type), m_importOwnership(importOwnership), m_close(close) {}
		~ExternalHandle() { Reset(); }
		ExternalHandle(const ExternalHandle&) = delete;
		ExternalHandle& operator=(const ExternalHandle&) = delete;
		ExternalHandle(ExternalHandle&& other) noexcept { MoveFrom(other); }
		ExternalHandle& operator=(ExternalHandle&& other) noexcept {
			if (this != &other) {
				Reset();
				MoveFrom(other);
			}
			return *this;
		}

		explicit operator bool() const noexcept { return m_value != nullptr; }
		void* Get() const noexcept { return m_value; }
		ExternalHandleType GetType() const noexcept { return m_type; }
		ExternalHandleImportOwnership GetImportOwnership() const noexcept { return m_importOwnership; }
		void Reset() noexcept {
			if (m_value && m_close) m_close(m_value);
			m_value = nullptr;
			m_type = ExternalHandleType::Unknown;
			m_importOwnership = ExternalHandleImportOwnership::RetainedByCaller;
			m_close = nullptr;
		}
		void MarkConsumed() noexcept {
			m_value = nullptr;
			m_type = ExternalHandleType::Unknown;
			m_close = nullptr;
		}

	private:
		void MoveFrom(ExternalHandle& other) noexcept {
			m_value = other.m_value;
			m_type = other.m_type;
			m_importOwnership = other.m_importOwnership;
			m_close = other.m_close;
			other.m_value = nullptr;
			other.m_type = ExternalHandleType::Unknown;
			other.m_close = nullptr;
		}

		void* m_value = nullptr;
		ExternalHandleType m_type = ExternalHandleType::Unknown;
		ExternalHandleImportOwnership m_importOwnership = ExternalHandleImportOwnership::RetainedByCaller;
		CloseFunction m_close = nullptr;
	};

	// --- Generic native handle (opaque)
	struct NativeHandle { void* ptr{}; uint32_t tag{}; uint32_t version{}; };

	// --- Stable, versioned IDs for interop structs
	constexpr uint32_t RHI_IID_D3D12_DEVICE = 0x1001;
	constexpr uint32_t RHI_IID_D3D12_QUEUE = 0x1002;
	constexpr uint32_t RHI_IID_D3D12_CMD_LIST = 0x1003;
	constexpr uint32_t RHI_IID_D3D12_SWAPCHAIN = 0x1004;
	constexpr uint32_t RHI_IID_D3D12_RESOURCE = 0x1005;
	constexpr uint32_t RHI_IID_D3D12_HEAP = 0x1006;
	constexpr uint32_t RHI_IID_D3D12_QUERY_POOL = 0x1007;
	constexpr uint32_t RHI_IID_D3D12_PIPELINE = 0x1008;
	constexpr uint32_t RHI_IID_D3D12_PIPELINE_LAYOUT = 0x1009;
	constexpr uint32_t RHI_IID_D3D12_COMMAND_SIGNATURE = 0x100A;
	constexpr uint32_t RHI_IID_D3D12_DESCRIPTOR_HEAP = 0x100B;
	constexpr uint32_t RHI_IID_D3D12_TIMELINE = 0x100C;
	constexpr uint32_t RHI_IID_D3D12_WORK_GRAPH = 0x100D;
	// Streamline manual-hooking proxy. Engine and other native interop callers must
	// continue to request RHI_IID_D3D12_CMD_LIST.
	constexpr uint32_t RHI_IID_D3D12_STREAMLINE_CMD_LIST = 0x100E;
	// A buffer as a range of an ID3D12Resource (D3D12BufferRangeInfo). A small D3D12 buffer may be packed: a range of a resource it
	// shares with other buffers, which RHI_IID_D3D12_RESOURCE refuses to hand out. Callers that honour the offset ask for this.
	constexpr uint32_t RHI_IID_D3D12_BUFFER_RANGE = 0x100F;

	constexpr uint32_t RHI_IID_VK_DEVICE = 0x2001;
	constexpr uint32_t RHI_IID_VK_QUEUE = 0x2002;
	constexpr uint32_t RHI_IID_VK_COMMAND_BUFFER = 0x2003;
	constexpr uint32_t RHI_IID_VK_SWAPCHAIN = 0x2004;
	constexpr uint32_t RHI_IID_VK_RESOURCE = 0x2005;
	constexpr uint32_t RHI_IID_VK_HEAP = 0x2006;
	constexpr uint32_t RHI_IID_VK_QUERY_POOL = 0x2007;
	constexpr uint32_t RHI_IID_VK_PIPELINE = 0x2008;
	constexpr uint32_t RHI_IID_VK_PIPELINE_LAYOUT = 0x2009;
	constexpr uint32_t RHI_IID_VK_COMMAND_SIGNATURE = 0x200A;
	constexpr uint32_t RHI_IID_VK_DESCRIPTOR_HEAP = 0x200B;
	constexpr uint32_t RHI_IID_VK_TIMELINE = 0x200C;
	constexpr uint32_t RHI_IID_VK_WORK_GRAPH = 0x200D;
	constexpr uint32_t RHI_IID_VK_DESCRIPTOR_SLOT = 0x200E;

	// --- Narrow, typed query surface (no native types)
	struct D3D12DeviceInfo { void* device; void* factory; void* adapter; uint32_t version; };
	struct D3D12QueueInfo { void* queue;  uint32_t version; };
	struct D3D12CmdListInfo { void* cmdList; void* allocator; uint32_t version; };
	struct D3D12SwapchainInfo { void* swapchain; uint32_t version; };
	struct D3D12ResourceInfo { void* resource; uint32_t version; };
	// The buffer is bytes [offset, offset + size) of resource. offset is 0 for a buffer with a resource of its own.
	struct D3D12BufferRangeInfo { void* resource; uint64_t offset; uint64_t size; uint32_t version; };
	struct D3D12HeapInfo { void* heap; uint32_t version; };
	struct D3D12QueryPoolInfo { void* queryPool; uint32_t version; };
	struct D3D12PipelineInfo { void* pipeline; uint32_t version; };
	struct D3D12PipelineLayoutInfo { void* layout; uint32_t version; };
	struct D3D12CommandSignatureInfo { void* cmdSig; uint32_t version; };
	struct D3D12DescriptorHeapInfo { void* descHeap; uint32_t version; };
	struct D3D12TimelineInfo { void* timeline; uint32_t version; };
	struct D3D12WorkGraphInfo { void* workGraph; uint32_t version; };

	struct VulkanDeviceInfo { void* instance; void* physicalDevice; void* device; uint32_t version; uint32_t apiVersion; uint32_t deviceApiVersion; };
	struct VulkanQueueInfo { void* queue; uint32_t familyIndex; uint32_t version; };
	struct VulkanCmdBufInfo { void* commandBuffer; uint32_t version; };
	struct VulkanSwapchainInfo { void* swapchain; uint32_t version; };
	struct VulkanResourceInfo {
		void* resource;
		uint64_t deviceAddress;
		uint32_t version;
		uint32_t nativeFormat;
		uint32_t width;
		uint32_t height;
		uint32_t mipLevels;
		uint32_t arrayLayers;
		uint32_t flags;
		uint32_t usage;
	};
	struct VulkanDescriptorSlotInfo {
		void* imageView;
		void* bufferView;
		uint32_t version;
		uint32_t nativeFormat;
		uint32_t aspectMask;
		uint32_t baseMipLevel;
		uint32_t levelCount;
		uint32_t baseArrayLayer;
		uint32_t layerCount;
	};
	struct VulkanHeapInfo { void* heap; uint32_t version; };
	struct VulkanQueryPoolInfo { void* queryPool; uint32_t version; };
	struct VulkanPipelineInfo { void* pipeline; uint32_t version; };
	struct VulkanPipelineLayoutInfo { void* layout; uint32_t version; };
	struct VulkanCommandSignatureInfo { void* cmdSig; uint32_t version; };
	struct VulkanDescriptorHeapInfo { void* descHeap; uint32_t version; };
	struct VulkanTimelineInfo { void* timeline; uint32_t version; };
	struct VulkanWorkGraphInfo { void* workGraph; uint32_t version; };

	// --- Query interface entry points (implemented by each backend)
	bool QueryNativeDevice(Device, uint32_t iid /*RHI_IID_...*/, void* outStruct /*typed*/, uint32_t outSize) noexcept;
	bool QueryNativeQueue(Queue, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeCmdList(CommandList, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeSwapchain(Swapchain, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeResource(Resource, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeHeap(Heap, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeQueryPool(QueryPool, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativePipeline(Pipeline, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativePipelineLayout(PipelineLayout, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeCommandSignature(CommandSignature, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeDescriptorHeap(DescriptorHeap, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeTimeline(Timeline, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeWorkGraph(WorkGraph, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
	bool QueryNativeDescriptorSlot(Device, DescriptorSlot, uint32_t iid, void* outStruct, uint32_t outSize) noexcept;
} // namespace rhi

namespace rhi::dx12 {
	/**
	 * Capture tables, a debug aid for tools that dump a frame. Off until EnableCaptureTables. Once on, every view and sampler
	 * BasicRHI writes is recorded by its descriptor, and every graphics pipeline keeps its shader bytecode and state.
	 */
	enum class DescriptorSlotKind : uint32_t { None, Srv, Uav, Cbv, Sampler };
	/** @brief What a descriptor slot holds. desc is the D3D12 view or sampler desc as written (D3D12_*_VIEW_DESC, D3D12_SAMPLER_DESC). */
	struct DescriptorSlotInfo {
		DescriptorSlotKind kind = DescriptorSlotKind::None;
		void* resource = nullptr;  // ID3D12Resource*, with a reference added for the caller. Null for a null view or a sampler.
		alignas(8) uint8_t desc[64]{};
	};
	/** @brief A graphics pipeline as BasicRHI handed it to CreatePipelineState. The state blobs are the D3D12 structs. */
	struct PipelineCaptureInfo {
		std::vector<uint8_t> vs, ps;  // empty when the stage is absent
		alignas(8) uint8_t rasterizer[64]{};    // D3D12_RASTERIZER_DESC
		alignas(8) uint8_t depthStencil[64]{};  // D3D12_DEPTH_STENCIL_DESC
		alignas(8) uint8_t blend[336]{};        // D3D12_BLEND_DESC
		uint32_t rtvCount = 0;
		uint32_t rtvFormats[8]{};
		uint32_t dsvFormat = 0;
		uint32_t sampleCount = 1, sampleQuality = 0;
		uint32_t sampleMask = 0xFFFFFFFFu;
		uint32_t topologyType = 0;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE
	};
	/** @brief Turns the capture tables on for a D3D12 device. Views and pipelines made before the call are not recorded. */
	void EnableCaptureTables(Device device) noexcept;
	/**
	 * @brief For a stall report: logs every D3D12 timeline's completed value and its recent signals and waits, with the queue of each.
	 * A queue stuck on a GPU wait shows as a wait whose value is past its timeline's completed one. Reads the histories racily.
	 * @return The number of timelines logged; 0 on another backend.
	 */
	uint32_t DumpTimelineHistories(Device device) noexcept;
	/** @brief Whether the capture tables are on. */
	bool CaptureTablesEnabled(Device device) noexcept;
	/**
	 * @brief The view or sampler at a_slot of a_heap. A slot of the buffer view refs' region (past a full-size heap's capacity)
	 * reads the region's staging copy. False when nothing was recorded there.
	 */
	bool DescribeDescriptorSlot(Device device, DescriptorHeapHandle heap, uint32_t slot, DescriptorSlotInfo& out) noexcept;
	/** @brief A graphics pipeline's bytecode and state. False when it was not recorded. */
	bool DescribePipeline(Device device, PipelineHandle pipeline, PipelineCaptureInfo& out) noexcept;
	/** @brief A live buffer resource and its GPU address range. */
	struct BufferResourceInfo {
		void* resource = nullptr;  // ID3D12Resource*, with a reference added for the caller
		uint64_t address = 0, bytes = 0;
	};
	/** @brief Every live buffer resource of the device, once each (a packed buffer's shared resource once). A debug aid; it walks the registry. */
	void EnumerateBufferResources(Device device, std::vector<BufferResourceInfo>& out) noexcept;
}
