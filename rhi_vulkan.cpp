#include "rhi_vulkan.h"

#include <memory>

namespace rhi {
	namespace {
		template <typename... TArgs>
		constexpr void VkIgnoreUnused(TArgs&&...) noexcept {}

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
			VkIgnoreUnused(queue, lists, submit);
			return Result::Unsupported;
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
			VkIgnoreUnused(resource, offset, size);
			if (data) {
				*data = nullptr;
			}
		}

		static void buf_unmap(Resource* resource, uint64_t writeOffset, uint64_t writeSize) noexcept {
			VkIgnoreUnused(resource, writeOffset, writeSize);
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

		static void cl_end(CommandList* commandList) noexcept {
			VkIgnoreUnused(commandList);
		}

		static void cl_reset(CommandList* commandList, const CommandAllocator& allocator) noexcept {
			VkIgnoreUnused(commandList, allocator);
		}

		static void cl_beginPass(CommandList* commandList, const PassBeginInfo& passInfo) noexcept {
			VkIgnoreUnused(commandList, passInfo);
		}

		static void cl_endPass(CommandList* commandList) noexcept {
			VkIgnoreUnused(commandList);
		}

		static void cl_barrier(CommandList* commandList, const BarrierBatch& barriers) noexcept {
			VkIgnoreUnused(commandList, barriers);
		}

		static void cl_bindLayout(CommandList* commandList, PipelineLayoutHandle layout) noexcept {
			VkIgnoreUnused(commandList, layout);
		}

		static void cl_bindPipeline(CommandList* commandList, PipelineHandle pipeline) noexcept {
			VkIgnoreUnused(commandList, pipeline);
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
			VkIgnoreUnused(commandList, x, y, z);
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
			VkIgnoreUnused(commandList, cbvSrvUav, sampler);
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
			VkIgnoreUnused(commandList, stages, set, binding, dstOffset32, num32, data);
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
			VkIgnoreUnused(allocator);
		}

		static uint32_t sc_count(Swapchain* swapchain) noexcept {
			VkIgnoreUnused(swapchain);
			return 0;
		}

		static uint32_t sc_curr(Swapchain* swapchain) noexcept {
			VkIgnoreUnused(swapchain);
			return 0;
		}

		static ResourceHandle sc_img(Swapchain* swapchain, uint32_t imageIndex) noexcept {
			VkIgnoreUnused(swapchain, imageIndex);
			return {};
		}

		static Result sc_present(Swapchain* swapchain, bool vsync) noexcept {
			VkIgnoreUnused(swapchain, vsync);
			return Result::Unsupported;
		}

		static Result sc_resizeBuffers(Swapchain* swapchain, uint32_t bufferCount, uint32_t width, uint32_t height, Format newFormat, uint32_t flags) noexcept {
			VkIgnoreUnused(swapchain, bufferCount, width, height, newFormat, flags);
			return Result::Unsupported;
		}

		static void sc_setName(Swapchain* swapchain, const char* name) noexcept {
			VkIgnoreUnused(swapchain, name);
		}

		static Result d_createPipelineFromStream(Device* device, const PipelineStreamItem* items, uint32_t count, PipelinePtr& out) noexcept {
			VkIgnoreUnused(device, items, count);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyBuffer(DeviceDeletionContext* context, ResourceHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_destroyTexture(DeviceDeletionContext* context, ResourceHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_destroySampler(DeviceDeletionContext* context, SamplerHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static void d_destroyPipeline(DeviceDeletionContext* context, PipelineHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
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
			VkIgnoreUnused(context, commandList);
		}

		static Queue d_getQueue(Device* device, QueueKind kind) noexcept {
			VkIgnoreUnused(device);
			return Queue(kind);
		}

		static Result d_createQueue(Device* device, QueueKind kind, const char* name, Queue& out) noexcept {
			VkIgnoreUnused(device, kind, name);
			out = Queue(kind);
			return Result::Unsupported;
		}

		static void d_destroyQueue(DeviceDeletionContext* context, QueueHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_waitIdle(Device* device) noexcept {
			VkIgnoreUnused(device);
			return Result::Unsupported;
		}

		static void d_flushDeletionQueue(Device* device) noexcept {
			VkIgnoreUnused(device);
		}

		static Result d_createSwapchain(Device* device, void* windowHandle, uint32_t width, uint32_t height, Format format, uint32_t bufferCount, bool allowTearing, SwapchainPtr& out) noexcept {
			VkIgnoreUnused(device, windowHandle, width, height, format, bufferCount, allowTearing);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroySwapchain(DeviceDeletionContext* context, Swapchain* swapchain) noexcept {
			VkIgnoreUnused(context, swapchain);
		}

		static void d_destroyDevice(Device* device) noexcept {
			auto* impl = device ? static_cast<VulkanDevice*>(device->impl) : nullptr;
			if (impl) {
				impl->Shutdown();
			}
		}

		static Result d_createPipelineLayout(Device* device, const PipelineLayoutDesc& desc, PipelineLayoutPtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static Result d_queryFeatureInfo(const Device* device, FeatureInfoHeader* chain) noexcept {
			VkIgnoreUnused(device, chain);
			return Result::Unsupported;
		}

		static Result d_queryVideoMemoryInfo(const Device* device, uint32_t nodeIndex, MemorySegmentGroup segmentGroup, VideoMemoryInfo& out) noexcept {
			VkIgnoreUnused(device, nodeIndex, segmentGroup);
			out = {};
			return Result::Unsupported;
		}

		static void d_destroyPipelineLayout(DeviceDeletionContext* context, PipelineLayoutHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
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
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyDescriptorHeap(DeviceDeletionContext* context, DescriptorHeapHandle handle) noexcept {
			VkIgnoreUnused(context, handle);
		}

		static Result d_createShaderResourceView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const SrvDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, resource, desc);
			return Result::Unsupported;
		}

		static Result d_createUnorderedAccessView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const UavDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, resource, desc);
			return Result::Unsupported;
		}

		static Result d_createConstantBufferView(Device* device, DescriptorSlot slot, const ResourceHandle& resource, const CbvDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, resource, desc);
			return Result::Unsupported;
		}

		static Result d_createSampler(Device* device, DescriptorSlot slot, const SamplerDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, desc);
			return Result::Unsupported;
		}

		static Result d_createRenderTargetView(Device* device, DescriptorSlot slot, const ResourceHandle& texture, const RtvDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, texture, desc);
			return Result::Unsupported;
		}

		static Result d_createDepthStencilView(Device* device, DescriptorSlot slot, const ResourceHandle& texture, const DsvDesc& desc) noexcept {
			VkIgnoreUnused(device, slot, texture, desc);
			return Result::Unsupported;
		}

		static Result d_createCommandAllocator(Device* device, QueueKind kind, CommandAllocatorPtr& out) noexcept {
			VkIgnoreUnused(device, kind);
			out.Reset();
			return Result::Unsupported;
		}

		static void d_destroyCommandAllocator(DeviceDeletionContext* context, CommandAllocator* allocator) noexcept {
			VkIgnoreUnused(context, allocator);
		}

		static Result d_createCommandList(Device* device, QueueKind kind, CommandAllocator allocator, CommandListPtr& out) noexcept {
			VkIgnoreUnused(device, kind, allocator);
			out.Reset();
			return Result::Unsupported;
		}

		static Result d_createCommittedBuffer(Device* device, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
		}

		static Result d_createCommittedTexture(Device* device, const ResourceDesc& desc, ResourcePtr& out) noexcept {
			VkIgnoreUnused(device, desc);
			out.Reset();
			return Result::Unsupported;
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
			VkIgnoreUnused(device, type);
			return 0;
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
			VkIgnoreUnused(device, kind);
			return {};
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
		VkIgnoreUnused(ci);

		auto impl = std::make_shared<VulkanDevice>();
		impl->selfWeak = impl;
		impl->self = Device{ impl.get(), &g_vkdevvt };

		outPtr = MakeDevicePtr(&impl->self, impl);
		return Result::Ok;
	}

} // namespace rhi