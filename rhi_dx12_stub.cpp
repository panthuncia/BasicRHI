#include "rhi.h"
#include "rhi_interop_dx12.h"

namespace rhi {
    Result CreateD3D12Device(const DeviceCreateInfo&, DevicePtr& out, bool) noexcept {
        out.Reset();
        return Result::Unsupported;
    }
    void PrewarmD3D12Streamline() noexcept {}
}

namespace rhi::dx12 {
    Result import_resource(rhi::Device, ID3D12Resource*, rhi::ResourcePtr& out) noexcept {
        out.Reset();
        return Result::Unsupported;
    }
    Result export_shared_resource(rhi::Device, rhi::Resource, ExternalHandle&) noexcept { return Result::Unsupported; }
    Result export_shared_heap(rhi::Device, rhi::Heap, ExternalHandle&) noexcept { return Result::Unsupported; }
    Result export_shared_timeline(rhi::Device, rhi::Timeline, ExternalHandle&) noexcept { return Result::Unsupported; }
}

namespace rhi::dx12 {
    void EnableCaptureTables(rhi::Device) noexcept {}
    uint32_t DumpTimelineHistories(rhi::Device) noexcept { return 0; }
    bool CaptureTablesEnabled(rhi::Device) noexcept { return false; }
    bool DescribeDescriptorSlot(rhi::Device, rhi::DescriptorHeapHandle, uint32_t, DescriptorSlotInfo& out) noexcept {
        out = {};
        return false;
    }
    bool DescribePipeline(rhi::Device, rhi::PipelineHandle, PipelineCaptureInfo&) noexcept { return false; }
    void EnumerateBufferResources(rhi::Device, std::vector<BufferResourceInfo>& out) noexcept { out.clear(); }
}
