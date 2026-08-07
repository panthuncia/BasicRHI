#include "rhi.h"

namespace rhi {
    Result CreateD3D12Device(const DeviceCreateInfo&, DevicePtr& out, bool) noexcept {
        out.Reset();
        return Result::Unsupported;
    }
}
