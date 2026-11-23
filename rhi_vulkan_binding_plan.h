#pragma once

// The binding plan: where each set/binding a shader stage declares reads its descriptor or data from, for one pipeline layout.
// It is the backend-neutral form of the VK_EXT_descriptor_heap set/binding mappings (VkDescriptorSetAndBindingMappingEXT):
// the descriptor-heap model translates it into those mappings, and the descriptor-buffer model lowers the shaders to it
// (rhi_vulkan_spirv_lowering.h). Built by VkBuildBindingPlan (rhi_vulkan.cpp) from a VulkanPipelineLayout and the stages.
//
// Offsets into "push data" are offsets into the layout's push data image: its push constant ranges packed back to back in
// declaration order (VulkanPushConstantRange::byteOffset). How that image reaches the shader (all of it in push constants, or a
// pushed prefix and a spilled rest) is the lowering target's business, not the plan's.

#include <cstdint>
#include <vector>

#include <volk.h>

namespace rhi::vulkan_binding {

	// Heap sources read the sampler heap for a variable declared as a sampler and the resource heap for anything else (as
	// VK_EXT_descriptor_heap does: the declared type picks the heap; resourceMask limits which declared types the mapping covers).
	enum class BindingSource : uint32_t {
		// A heap descriptor at a constant element: element (heapElement + i) for binding (firstBinding + i), the variable's own
		// array index added. The three heap bindings of the shader ABI (rhi_shader_abi.h) are this with heapElement 0 and a
		// runtime array; LayoutRangeSource::HeapSlot ranges are this with heapElement = the range's binding.
		Heap,
		// A heap descriptor whose element index is a uint32 in memory: at (the uint64 record address in push data at pushOffset)
		// + recordOffset + 4 * i (LayoutRangeSource::IndirectIndex).
		HeapIndirect,
		// A uniform block whose uint64 device address is in memory: at (the record address in push data at pushOffset)
		// + recordOffset + 8 * i (LayoutRangeSource::IndirectAddress).
		BufferIndirectAddress,
		// A uniform block whose uint64 device address is push data at pushOffset + 8 * i (LayoutRangeSource::PushAddress, and
		// PushConstantRangeType::EmulatedRootConstants with one binding).
		BufferPushAddress,
		// A uniform block whose bytes are push data starting at pushOffset (PushConstantRangeType::RootConstants32).
		PushData,
		// A static sampler: element i of layout static sampler staticSampler (StaticSamplerDesc, consecutive elements).
		StaticSampler,
		// A heap descriptor whose element index is (the uint32 in push data at pushOffset) + recordOffset + i
		// (LayoutRangeSource::PushIndex; recordOffset is a slot count here). Appended last: tools read sources by number.
		HeapPush,
		// A uniform buffer bound at descriptor set boundSet, binding (heapElement + i): a push descriptor set the command list fills
		// from heap slots (LayoutRangeSource::PushIndex in the descriptor-sets model with bound uniform buffers). The variable keeps
		// its type and is only re-decorated. Appended last, like HeapPush.
		BoundUniform,
	};

	struct BindingMapping {
		uint32_t set = 0;
		uint32_t firstBinding = 0;
		// Bindings (firstBinding + i), i < bindingCount, each mapped the same way with its own i. The heap ABI bindings use 1 (one
		// runtime array binding; the shader indexes it).
		uint32_t bindingCount = 1;
		// VK_SPIRV_RESOURCE_TYPE_*_BIT_EXT: the declared resource types this mapping applies to.
		VkFlags resourceMask = 0;
		BindingSource source = BindingSource::Heap;
		uint32_t heapElement = 0;   // Heap; BoundUniform: the binding of element 0 in boundSet
		uint32_t pushOffset = 0;    // bytes into push data: the record address (*Indirect*), the address (BufferPushAddress), the data (PushData),
		                            // the heap index (HeapPush)
		uint32_t recordOffset = 0;  // bytes, *Indirect* sources; HeapPush: slots added to the pushed index
		uint32_t pushDataBytes = 0; // PushData: the range's size in bytes
		uint32_t staticSampler = 0; // StaticSampler: index into the layout's static samplers
		uint32_t boundSet = 0;      // BoundUniform: the push descriptor set
	};

	struct BindingPlan {
		std::vector<BindingMapping> mappings;
		uint32_t pushDataBytes = 0;  // the push data image's size (VulkanPipelineLayout::totalPushDataBytes)
	};

}
