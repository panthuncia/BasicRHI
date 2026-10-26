#pragma once

// The descriptor-buffer model's shader lowering: rewrites a stage's SPIR-V so that every set/binding the binding plan maps
// (rhi_vulkan_binding_plan.h) reads its descriptor or data the way VK_EXT_descriptor_heap's mappings would, through the
// descriptor-buffer model's one universal pipeline layout:
//   set resourceSet, binding resourceBinding: the resource heap, a MUTABLE_EXT array (DXC's aliased typed arrays bind to it);
//   set samplerSet, binding samplerBinding: the sampler heap, a SAMPLER array;
//   one push constant range [0, pushConstantBytes): the pushed prefix of the push data image, and the spill address.
// Built on SPIRV-Tools' optimizer (a custom spvtools::opt::Pass between InlineExhaustive and the usual cleanup passes).
// Thread-safe: no state outside the call.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rhi_vulkan_binding_plan.h"

namespace rhi::vulkan_binding {

	inline constexpr uint32_t kNoSpill = 0xFFFFFFFFu;

	struct LoweringTarget {
		uint32_t resourceSet = 0, resourceBinding = 0;
		uint32_t samplerSet = 1, samplerBinding = 0;
		// Push data image bytes [0, pushedBytes) are push constants at the same offsets. Bytes [pushedBytes, plan.pushDataBytes)
		// are spilled: the spill image holds them at (offset - pushedBytes) from the uint64 device address that is push constant
		// data at spillAddressOffset (8-byte aligned, past pushedBytes). spillAddressOffset is kNoSpill when nothing is spilled.
		uint32_t pushedBytes = 0;
		uint32_t spillAddressOffset = kNoSpill;
		uint32_t pushConstantBytes = 128;  // the universal layout's push constant range
		// Guaranteed alignments of the device addresses the shader reads, for the PhysicalStorageBuffer loads' Aligned operands.
		// Only what the heap model's contract guarantees: a record address (the base an Indirect* source's recordOffset is added
		// to) is 8-byte aligned and its index entries 4-byte aligned; a uniform block's address is aligned to the device's
		// minUniformBufferOffsetAlignment; the spill image is BasicRHI's own scratch, aligned to spillAlignment.
		uint32_t recordAlignment = 8;
		uint32_t uniformBlockAlignment = 4;
		uint32_t spillAlignment = 16;
		// Sampler heap element of each layout static sampler's first element (BindingMapping::staticSampler indexes this).
		std::span<const uint32_t> staticSamplerElements;
	};

	// Lowers one stage's module. False (and a message in a_error) when the module cannot be lowered: a binding the plan does not
	// map, a pointer the rewrite cannot follow, an optimizer failure. a_out is the lowered module on success.
	// a_entryPoint names the entry point to lower when the module has several (a ray tracing library): the others are removed.
	// Empty: the module's only entry point.
	bool LowerForDescriptorBuffer(std::span<const uint32_t> a_spirv, const BindingPlan& a_plan, VkShaderStageFlagBits a_stage,
		const LoweringTarget& a_target, std::vector<uint32_t>& a_out, std::string& a_error, std::string_view a_entryPoint = {});

	// SPIRV-Tools' validator for a lowered module (Vulkan 1.3, scalar block layout), for the diagnostic mode
	// (BASICRHI_VK_VALIDATE_LOWERING=1). False and the validator's messages in a_error when invalid.
	bool ValidateLoweredModule(std::span<const uint32_t> a_spirv, std::string& a_error);

}
