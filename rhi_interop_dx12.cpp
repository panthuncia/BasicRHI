#include "rhi_dx12.h"
#include "rhi_interop.h"
#include "rhi_dx12_casting.h"
#include "rhi_vulkan.h"

#include <cstdint>
#include <type_traits>

// Returns non-owning raw pointers. Caller must not store them long-term without AddRef/Release.

namespace rhi {
    namespace {
        template <typename THandle>
        void* NativeHandleToVoid(THandle handle) noexcept {
            if constexpr (std::is_pointer_v<THandle>) {
                return static_cast<void*>(handle);
            }
            else {
                return reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
            }
        }

        bool IsDx12Device(const Device& device) noexcept { return device.vt == &g_devvt; }
        bool IsVulkanDevice(const Device& device) noexcept { return device.vt == &g_vkdevvt; }
        bool IsDx12Queue(const Queue& queue) noexcept { return queue.vt == &g_qvt; }
        bool IsVulkanQueue(const Queue& queue) noexcept { return queue.vt == &g_vkqvt; }
        bool IsDx12CommandList(const CommandList& commandList) noexcept { return commandList.vt == &g_clvt; }
        bool IsVulkanCommandList(const CommandList& commandList) noexcept { return commandList.vt == &g_vkclvt; }
        bool IsDx12Swapchain(const Swapchain& swapchain) noexcept { return swapchain.vt == &g_scvt; }
        bool IsVulkanSwapchain(const Swapchain& swapchain) noexcept { return swapchain.vt == &g_vkscvt; }
        bool IsDx12Resource(const Resource& resource) noexcept { return resource.vt == &g_buf_rvt || resource.vt == &g_tex_rvt; }
        bool IsVulkanResource(const Resource& resource) noexcept { return resource.vt == &g_vkbuf_rvt || resource.vt == &g_vktex_rvt; }
    }

    bool QueryNativeDevice(Device d, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!d.IsValid() || !outStruct) return false;

        if (IsVulkanDevice(d)) {
            if (iid != RHI_IID_VK_DEVICE || outSize < sizeof(VulkanDeviceInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(d.impl);
            if (!impl) return false;
            auto* out = reinterpret_cast<VulkanDeviceInfo*>(outStruct);
            out->instance = NativeHandleToVoid(impl->instance);
            out->physicalDevice = NativeHandleToVoid(impl->physicalDevice);
            out->device = NativeHandleToVoid(impl->device);
            out->version = 1;
            return true;
        }

        if (!IsDx12Device(d)) return false;

        auto* impl = static_cast<Dx12Device*>(d.impl);
        if (!impl) return false;

        switch (iid) {
        case RHI_IID_D3D12_DEVICE: {
            if (outSize < sizeof(D3D12DeviceInfo)) return false;

            // Ensure we hand out an ID3D12Device* (not Device10)
            Microsoft::WRL::ComPtr<ID3D12Device> devBase;
            // If you also stored devBase at creation time, you can skip As() and use that ComPtr.
            (void)impl->pNativeDevice.As(&devBase);

            auto* out = reinterpret_cast<D3D12DeviceInfo*>(outStruct);
            out->device = devBase.Get();            // ID3D12Device*
            out->factory = impl->pNativeFactory.Get();      // IDXGIFactory7*
            out->adapter = impl->adapter.Get();      // IDXGIAdapter4* (may be null if not stored)
            out->version = 1;
            return true;
        }
        default:
            return false;
        }
    }

    bool QueryNativeQueue(Queue q, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!q.IsValid() || !outStruct) return false;
        if (IsVulkanQueue(q)) {
            if (iid != RHI_IID_VK_QUEUE || outSize < sizeof(VulkanQueueInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(q.impl);
            if (!impl) return false;
            const uint32_t queueSlot = q.GetQueueHandle().index;
            if (queueSlot >= impl->queues.size()) return false;
            const VulkanQueueState& queueState = impl->queues[queueSlot];
            if (queueState.queue == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanQueueInfo*>(outStruct);
            out->queue = NativeHandleToVoid(queueState.queue);
            out->familyIndex = queueState.familyIndex;
            out->version = 1;
            return true;
        }
        if (!IsDx12Queue(q)) return false;
        if (iid != RHI_IID_D3D12_QUEUE) return false;
        if (outSize < sizeof(D3D12QueueInfo)) return false;

        auto* s = dx12_detail::QState(&q);
        if (!s || !s->pNativeQueue) return false;

        auto* out = reinterpret_cast<D3D12QueueInfo*>(outStruct);
        out->queue = s->pNativeQueue.Get();   // ID3D12CommandQueue*
        out->version = 1;
        return true;
    }

    bool QueryNativeCmdList(CommandList cl, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!cl.IsValid() || !outStruct) return false;
        if (IsVulkanCommandList(cl)) {
            if (iid != RHI_IID_VK_COMMAND_BUFFER || outSize < sizeof(VulkanCmdBufInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(cl.impl);
            if (!impl) return false;
            VulkanCommandList* rec = impl->commandLists.get(cl.GetHandle());
            if (!rec || rec->commandBuffer == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanCmdBufInfo*>(outStruct);
            out->commandBuffer = NativeHandleToVoid(rec->commandBuffer);
            out->version = 1;
            return true;
        }
        if (!IsDx12CommandList(cl)) return false;
        if (iid != RHI_IID_D3D12_CMD_LIST) return false;
        if (outSize < sizeof(D3D12CmdListInfo)) return false;

        auto* rec = dx12_detail::CL(&cl);
        if (!rec || !rec->cl) return false;

        // Hand out ID3D12GraphicsCommandList* (QI from v7 to base)
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> baseCl;
        (void)rec->cl.As(&baseCl);

        auto* out = reinterpret_cast<D3D12CmdListInfo*>(outStruct);
        out->cmdList = baseCl.Get();              // ID3D12GraphicsCommandList*
        out->allocator = rec->alloc.Get();          // ID3D12CommandAllocator* (may be null if not tracked)
        out->version = 1;
        return true;
    }

    bool QueryNativeSwapchain(Swapchain sc, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!sc.IsValid() || !outStruct) return false;
        if (IsVulkanSwapchain(sc)) {
            if (iid != RHI_IID_VK_SWAPCHAIN || outSize < sizeof(VulkanSwapchainInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(sc.impl);
            if (!impl) return false;
            VulkanSwapchain* rec = impl->swapchains.get(sc.GetHandle());
            if (!rec || rec->swapchain == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanSwapchainInfo*>(outStruct);
            out->swapchain = NativeHandleToVoid(rec->swapchain);
            out->version = 1;
            return true;
        }
        if (!IsDx12Swapchain(sc)) return false;
        if (iid != RHI_IID_D3D12_SWAPCHAIN) return false;
        if (outSize < sizeof(D3D12SwapchainInfo)) return false;

        auto* s = dx12_detail::SC(&sc);
        if (!s || !s->pNativeSC) return false;

        auto* out = reinterpret_cast<D3D12SwapchainInfo*>(outStruct);
        out->swapchain = s->pNativeSC.Get(); // IDXGISwapChain3*
        out->version = 1;
        return true;
    }

    bool QueryNativeResource(Resource h, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!h.IsValid() || !outStruct) return false;
        if (IsVulkanResource(h)) {
            if (iid != RHI_IID_VK_RESOURCE || outSize < sizeof(VulkanResourceInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(h.impl);
            if (!impl) return false;
            VulkanResource* rec = impl->resources.get(h.GetHandle());
            if (!rec || (rec->buffer == VK_NULL_HANDLE && rec->image == VK_NULL_HANDLE)) return false;
            auto* out = reinterpret_cast<VulkanResourceInfo*>(outStruct);
            out->resource = rec->image != VK_NULL_HANDLE ? NativeHandleToVoid(rec->image) : NativeHandleToVoid(rec->buffer);
            out->deviceAddress = rec->deviceAddress;
            out->version = 2;
            return true;
        }
        if (!IsDx12Resource(h)) return false;
        if (iid != RHI_IID_D3D12_RESOURCE) return false;
        if (outSize < sizeof(D3D12ResourceInfo)) return false;
		// Cast to Dx12Buffer or Dx12Texture based on h.IsTexture()
		Dx12Resource* resRec = nullptr;
        resRec = dx12_detail::Res(&h);
        if (!resRec || !resRec->res) {
            return false;
        }
        auto* out = reinterpret_cast<D3D12ResourceInfo*>(outStruct);
        out->resource = resRec->res.Get(); // ID3D12Resource*
        out->version = 1;
        return true;
	}

    bool QueryNativeHeap(Heap h, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!h.IsValid() || !outStruct) return false;
        if (h.vt == &g_vkhevt) {
            if (iid != RHI_IID_VK_HEAP || outSize < sizeof(VulkanHeapInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(h.impl);
            if (!impl) return false;
            VulkanHeap* rec = impl->heaps.get(h.GetHandle());
            if (!rec || rec->memory == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanHeapInfo*>(outStruct);
            out->heap = NativeHandleToVoid(rec->memory);
            out->version = 1;
            return true;
        }
        if (h.vt != &g_hevt) return false;
        if (iid != RHI_IID_D3D12_HEAP) return false;
        if (outSize < sizeof(D3D12HeapInfo)) return false;
        auto* rec = dx12_detail::Hp(&h);
        if (!rec || !rec->heap) return false;
        auto* out = reinterpret_cast<D3D12HeapInfo*>(outStruct);
        out->heap = rec->heap.Get(); // ID3D12Heap*
        out->version = 1;
		return true;
	}

    bool QueryNativeQueryPool(QueryPool qp, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
		if (!qp.IsValid() || !outStruct) return false;
        if (qp.vt == &g_vkqpvt) {
            if (iid != RHI_IID_VK_QUERY_POOL || outSize < sizeof(VulkanQueryPoolInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(qp.impl);
            if (!impl) return false;
            VulkanQueryPool* rec = impl->queryPools.get(qp.GetHandle());
            if (!rec || rec->pool == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanQueryPoolInfo*>(outStruct);
            out->queryPool = NativeHandleToVoid(rec->pool);
            out->version = 1;
            return true;
        }
        if (qp.vt != &g_qpvt) return false;
		if (iid != RHI_IID_D3D12_QUERY_POOL) return false;
		if (outSize < sizeof(D3D12QueryPoolInfo)) return false;
        auto* rec = dx12_detail::QP(&qp);
		if (!rec || !rec->heap) return false;
		auto* out = reinterpret_cast<D3D12QueryPoolInfo*>(outStruct);
		out->queryPool = rec->heap.Get(); // ID3D12QueryHeap*
		out->version = 1;
		return true;
	}

    bool QueryNativePipeline(Pipeline p, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!p.IsValid() || !outStruct) return false;
        if (p.vt == &g_vkpsovt) {
            if (iid != RHI_IID_VK_PIPELINE || outSize < sizeof(VulkanPipelineInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(p.impl);
            if (!impl) return false;
            VulkanPipeline* rec = impl->pipelines.get(p.GetHandle());
            if (!rec || rec->pipeline == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanPipelineInfo*>(outStruct);
            out->pipeline = NativeHandleToVoid(rec->pipeline);
            out->version = 1;
            return true;
        }
        if (p.vt != &g_psovt) return false;
        if (iid != RHI_IID_D3D12_PIPELINE) return false;
        if (outSize < sizeof(D3D12PipelineInfo)) return false;
        auto* rec = dx12_detail::Pso(&p);
        if (!rec || !rec->pso) return false;
        auto* out = reinterpret_cast<D3D12PipelineInfo*>(outStruct);
        out->pipeline = rec->pso.Get(); // ID3D12PipelineState*
        out->version = 1;
        return true;
	}

    bool QueryNativePipelineLayout(PipelineLayout pl, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!pl.IsValid() || !outStruct) return false;
		if (pl.vt == &g_vkplvt) return false;
		if (pl.vt != &g_plvt) return false;
        if (iid != RHI_IID_D3D12_PIPELINE_LAYOUT) return false;
        if (outSize < sizeof(D3D12PipelineLayoutInfo)) return false;
        auto* rec = dx12_detail::PL(&pl);
        if (!rec || !rec->root) return false;
        auto* out = reinterpret_cast<D3D12PipelineLayoutInfo*>(outStruct);
        out->layout = rec->root.Get(); // ID3D12RootSignature*
        out->version = 1;
        return true;
    }

    bool QueryNativeDescriptorHeap(DescriptorHeap dh, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!dh.IsValid() || !outStruct) return false;
        if (dh.vt == &g_vkdhvt) {
            if (iid != RHI_IID_VK_DESCRIPTOR_HEAP || outSize < sizeof(VulkanDescriptorHeapInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(dh.impl);
            if (!impl) return false;
            VulkanDescriptorHeap* rec = impl->descriptorHeaps.get(dh.GetHandle());
            if (!rec || rec->buffer == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanDescriptorHeapInfo*>(outStruct);
            out->descHeap = NativeHandleToVoid(rec->buffer);
            out->version = 1;
            return true;
        }
        if (dh.vt != &g_dhvt) return false;
        if (iid != RHI_IID_D3D12_DESCRIPTOR_HEAP) return false;
        if (outSize < sizeof(D3D12DescriptorHeapInfo)) return false;
        auto* rec = dx12_detail::DH(&dh);
        if (!rec || !rec->heap) return false;
        auto* out = reinterpret_cast<D3D12DescriptorHeapInfo*>(outStruct);
        out->descHeap = rec->heap.Get(); // ID3D12DescriptorHeap*
        out->version = 1;
        return true;
	}

    bool QueryNativeCommandSignature(CommandSignature cs, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!cs.IsValid() || !outStruct) return false;
		if (cs.vt == &g_vkcsvt) return false;
		if (cs.vt != &g_csvt) return false;
        if (iid != RHI_IID_D3D12_COMMAND_SIGNATURE) return false;
        if (outSize < sizeof(D3D12CommandSignatureInfo)) return false;
        auto* rec = dx12_detail::CSig(&cs);
        if (!rec || !rec->sig) return false;
        auto* out = reinterpret_cast<D3D12CommandSignatureInfo*>(outStruct);
        out->cmdSig = rec->sig.Get(); // ID3D12CommandSignature*
        out->version = 1;
        return true;
    }

    bool QueryNativeTimeline(Timeline t, uint32_t iid, void* outStruct, uint32_t outSize) noexcept {
        if (!t.IsValid() || !outStruct) return false;
        if (t.vt == &g_vktlvt) {
            if (iid != RHI_IID_VK_TIMELINE || outSize < sizeof(VulkanTimelineInfo)) return false;
            auto* impl = static_cast<VulkanDevice*>(t.impl);
            if (!impl) return false;
            VulkanTimeline* rec = impl->timelines.get(t.GetHandle());
            if (!rec || rec->semaphore == VK_NULL_HANDLE) return false;
            auto* out = reinterpret_cast<VulkanTimelineInfo*>(outStruct);
            out->timeline = NativeHandleToVoid(rec->semaphore);
            out->version = 1;
            return true;
        }
        if (t.vt != &g_tlvt) return false;
        if (iid != RHI_IID_D3D12_TIMELINE) return false;
        if (outSize < sizeof(D3D12TimelineInfo)) return false;
        auto* rec = dx12_detail::TL(&t);
        if (!rec || !rec->fence) return false;
        auto* out = reinterpret_cast<D3D12TimelineInfo*>(outStruct);
        out->timeline = rec->fence.Get(); // ID3D12Fence*
        out->version = 1;
        return true;
	}

    namespace dx12 {

        
    }

}