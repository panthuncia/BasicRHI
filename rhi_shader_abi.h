#pragma once

// The SPIR-V ABI of BasicRHI's Vulkan backend, kept free of platform headers so shader compilers
// (which include DXC's own Win32 adapter on Linux) can use it without the rest of rhi.h.

#include <cstdint>
#include <string>
#include <vector>

namespace rhi {

inline constexpr uint32_t VULKAN_DESCRIPTOR_HEAP_SET = 0;
inline constexpr uint32_t VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING = 1000000;
inline constexpr uint32_t VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING = 1000001;
inline constexpr uint32_t VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING = 1000002;

// The DXC arguments every SPIR-V shader consumed by the Vulkan backend must be compiled with:
// DX buffer layout and ResourceDescriptorHeap/SamplerDescriptorHeap mapped onto the
// VK_EXT_descriptor_heap bindings above. Runtime compilers append these; build-time
// compilation uses BASICRHI_VULKAN_DXC_FLAGS from cmake/BasicRHIShaderFlags.cmake,
// which parses the constants from this header.
inline void AppendVulkanDxcSpirvArguments(std::vector<std::wstring>& args) {
	const auto set = std::to_wstring(VULKAN_DESCRIPTOR_HEAP_SET);
	args.insert(args.end(), {
		L"-spirv",
		L"-fvk-use-dx-layout",
		L"-fspv-target-env=vulkan1.3",
		L"-fvk-bind-resource-heap", std::to_wstring(VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING), set,
		L"-fvk-bind-sampler-heap", std::to_wstring(VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING), set,
		L"-fvk-bind-counter-heap", std::to_wstring(VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING), set,
	});
}

}
