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

// Buffer view refs (Device::GetBufferViewRef): how a shader reaches a buffer without a pointer, the same on D3D12 and Vulkan.
// A ref is two words, the buffer's view slot and a byte offset into the buffer. The view slot is a heap index in
// ResourceDescriptorHeap: the buffer's raw SRV (ByteAddressBuffer) is at the slot and its raw UAV (RWByteAddressBuffer) at the
// slot after it. A buffer without UAV access has a null UAV there: a store through it is dropped.
// - The pairs live in a reserved region of every shader-visible CBV/SRV/UAV heap of the device's full capacity
//   (ShaderFeatureInfo::maxShaderVisibleResourceDescriptors), past the slots the client indexes: slots
//   [capacity, capacity + BUFFER_VIEW_REF_SLOTS). A buffer gets its pair the first time its ref is asked for and gives it back
//   when it is destroyed (with the device's deferred destruction, so after the frames that may read it).
// - View slot 0 is never a pair: a ref with view slot 0 names no buffer. Nothing makes a load through it safe; check first.
// - On Vulkan the pair is two storage buffer descriptors over the buffer's device address range, in either binding model
//   (VK_EXT_descriptor_heap's heap or the descriptor-buffer model's universal set), so the device reports
//   BUFFER_VIEW_REF_SLOTS fewer descriptors than it holds.
// Common/DeviceRef.hlsli is the shader side.
inline constexpr uint32_t BUFFER_VIEW_REF_PAIRS = 16384;
inline constexpr uint32_t BUFFER_VIEW_REF_SLOTS = 2 * BUFFER_VIEW_REF_PAIRS;

// The DXC arguments every SPIR-V shader consumed by the Vulkan backend must be compiled with:
// DX buffer layout, DX SV_Position.w, and ResourceDescriptorHeap/SamplerDescriptorHeap mapped onto the
// VK_EXT_descriptor_heap bindings above. Runtime compilers append these; build-time
// compilation uses BASICRHI_VULKAN_DXC_FLAGS from cmake/BasicRHIShaderFlags.cmake,
// which parses the constants from this header.
inline void AppendVulkanDxcSpirvArguments(std::vector<std::wstring>& args) {
	const auto set = std::to_wstring(VULKAN_DESCRIPTOR_HEAP_SET);
	args.insert(args.end(), {
		L"-spirv",
		L"-fvk-use-dx-layout",
		L"-fvk-use-dx-position-w", // SV_Position.w reads as clip W in a pixel shader, not its reciprocal (SPIR-V FragCoord.w), as on D3D12.
		L"-fspv-target-env=vulkan1.3",
		L"-fvk-bind-resource-heap", std::to_wstring(VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING), set,
		L"-fvk-bind-sampler-heap", std::to_wstring(VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING), set,
		L"-fvk-bind-counter-heap", std::to_wstring(VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING), set,
	});
}

}
