#include "rhi.h"

namespace rhi {
    Result CreateVulkanDevice(const DeviceCreateInfo&, DevicePtr& out, bool) noexcept {
        out.Reset();
        return Result::Unsupported;
    }
}
