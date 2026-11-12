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

// D3D12's emulated buffer device addresses (rhi_dx12.cpp, Dx12AddressTable). DXIL has no buffer pointers, so a shader turns
// an address from Device::GetBufferDeviceAddress into a raw view and an offset through two tables BasicRHI keeps. Their raw
// views sit in the reserved region of every shader-visible CBV/SRV/UAV heap of the device's full capacity: that region starts
// at D3D12_DEVICE_ADDRESS_TABLE_SLOT, past the capacity the client indexes.
// - Slot + 0: the entries, D3D12_DEVICE_ADDRESS_ENTRY_BYTES each: base address (2 words), size (2 words), the raw SRV's heap
//   slot, the raw UAV's heap slot (a read-only buffer's: the null UAV), 2 unused words.
// - Slot + 1: the page table, D3D12_DEVICE_ADDRESS_PAGE_SLOTS slots of 16 bytes, open addressing on the address's
//   D3D12_DEVICE_ADDRESS_PAGE_BITS-bit page: entry index (0xFFFFFFFF: removed), unused, page (2 words; 0: empty). A page may
//   name several buffers; the shader checks the address against each entry it finds.
// - Slot + 2 + 2 * entry and + 3 + 2 * entry: an entry's raw SRV and UAV.
// - Slot + D3D12_DEVICE_ADDRESS_NULL_SLOT and the slot after it: a null raw SRV and UAV, for an address in no buffer. A load
//   through one reads zeros and a store is dropped (heap slot 0 may hold any kind of view: reading it as a buffer can fault).
// Common/DeviceAddress.hlsli is the shader side.
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_TABLE_SLOT = 1000000;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_ENTRIES = 16384;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_ENTRY_BYTES = 32;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_PAGE_BITS = 16;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_PAGE_SLOTS = 1u << 18;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_PROBES = 64;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_NULL_SLOT = 2 + 2 * D3D12_DEVICE_ADDRESS_ENTRIES;
inline constexpr uint32_t D3D12_DEVICE_ADDRESS_RESERVED_SLOTS = D3D12_DEVICE_ADDRESS_NULL_SLOT + 2;

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
