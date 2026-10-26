// The descriptor-buffer lowering tests (rhi_vulkan_spirv_lowering.h): HLSL fixtures (tests/lowering, compiled at build time with
// the shader ABI's DXC flags) lowered with hand-built binding plans (as VkBuildBindingPlan builds them), validated, and checked
// structurally on the disassembly. Negative cases must fail with a clear message. No GPU needed.

#include "rhi_vulkan_spirv_lowering.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "spirv-tools/libspirv.hpp"

#ifndef BASICRHI_LOWERING_FIXTURE_DIR
#error "BASICRHI_LOWERING_FIXTURE_DIR must name the directory of the compiled fixtures (tests/lowering/CMakeLists.txt)"
#endif

namespace {
	using namespace rhi::vulkan_binding;

	// VkBuildBindingPlan's masks (rhi_vulkan.cpp).
	constexpr VkFlags kResourceMask =
		VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT |
		VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
	constexpr VkFlags kSamplerMask = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;

	// ---- Plan building (the shapes VkBuildBindingPlan produces) ----------------------------------------------------------

	void AddAbiHeaps(BindingPlan& a_plan) {
		a_plan.mappings.push_back(BindingMapping{ .set = 0, .firstBinding = 1000000, .resourceMask = kResourceMask, .source = BindingSource::Heap });
		a_plan.mappings.push_back(BindingMapping{ .set = 0, .firstBinding = 1000001, .resourceMask = kSamplerMask, .source = BindingSource::Heap });
		a_plan.mappings.push_back(BindingMapping{ .set = 0, .firstBinding = 1000002, .resourceMask = kResourceMask, .source = BindingSource::Heap });
	}

	BindingMapping HeapSlot(uint32_t a_set, uint32_t a_binding, uint32_t a_count) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .bindingCount = a_count, .resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT,
			.source = BindingSource::Heap, .heapElement = a_binding };
	}

	BindingMapping IndirectIndex(uint32_t a_set, uint32_t a_binding, uint32_t a_count, uint32_t a_pushOffset, uint32_t a_recordOffset, bool a_samplers) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .bindingCount = a_count, .resourceMask = a_samplers ? kSamplerMask : kResourceMask,
			.source = BindingSource::HeapIndirect, .pushOffset = a_pushOffset, .recordOffset = a_recordOffset };
	}

	BindingMapping IndirectAddress(uint32_t a_set, uint32_t a_binding, uint32_t a_count, uint32_t a_pushOffset, uint32_t a_recordOffset) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .bindingCount = a_count, .resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
			.source = BindingSource::BufferIndirectAddress, .pushOffset = a_pushOffset, .recordOffset = a_recordOffset };
	}

	BindingMapping PushAddress(uint32_t a_set, uint32_t a_binding, uint32_t a_count, uint32_t a_pushOffset) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .bindingCount = a_count, .resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
			.source = BindingSource::BufferPushAddress, .pushOffset = a_pushOffset };
	}

	BindingMapping PushData(uint32_t a_set, uint32_t a_binding, uint32_t a_pushOffset, uint32_t a_bytes) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
			.source = BindingSource::PushData, .pushOffset = a_pushOffset, .pushDataBytes = a_bytes };
	}

	BindingMapping StaticSampler(uint32_t a_set, uint32_t a_binding, uint32_t a_count, uint32_t a_staticSampler) {
		return BindingMapping{ .set = a_set, .firstBinding = a_binding, .bindingCount = a_count, .resourceMask = kSamplerMask,
			.source = BindingSource::StaticSampler, .staticSampler = a_staticSampler };
	}

	// ---- Fixtures and disassembly ----------------------------------------------------------------------------------------

	bool LoadFixture(const std::string& a_name, std::vector<uint32_t>& a_spirv) {
		const std::string path = std::string(BASICRHI_LOWERING_FIXTURE_DIR) + "/" + a_name + ".spv";
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			std::printf("  cannot open fixture %s\n", path.c_str());
			return false;
		}
		const std::streamsize bytes = file.tellg();
		file.seekg(0);
		a_spirv.assign(static_cast<size_t>(bytes) / 4, 0u);
		return bytes % 4 == 0 && file.read(reinterpret_cast<char*>(a_spirv.data()), bytes).good();
	}

	std::string Disassemble(const std::vector<uint32_t>& a_spirv) {
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
		std::string text;
		tools.Disassemble(a_spirv, &text);
		return text;
	}

	// Lines of a disassembly matching a pattern.
	size_t CountLines(const std::string& a_text, const char* a_pattern) {
		const std::regex pattern(a_pattern);
		std::istringstream lines(a_text);
		size_t count = 0;
		for (std::string line; std::getline(lines, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			count += std::regex_search(line, pattern) ? 1u : 0u;
		}
		return count;
	}

	struct Checks {
		std::vector<std::string> failures;
		void That(bool a_condition, const std::string& a_what) {
			if (!a_condition) {
				failures.push_back(a_what);
			}
		}
		void Has(const std::string& a_text, const char* a_pattern, const char* a_what) { That(CountLines(a_text, a_pattern) > 0, a_what); }
		void HasNo(const std::string& a_text, const char* a_pattern, const char* a_what) { That(CountLines(a_text, a_pattern) == 0, a_what); }
		void Count(const std::string& a_text, const char* a_pattern, size_t a_expected, const char* a_what) {
			const size_t count = CountLines(a_text, a_pattern);
			That(count == a_expected, std::string(a_what) + " (found " + std::to_string(count) + ", expected " + std::to_string(a_expected) + ")");
		}
	};

	struct Case {
		const char* name = "";
		const char* fixture = "";
		VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
		BindingPlan plan;
		LoweringTarget target;
		std::vector<uint32_t> staticSamplerElements;
		const char* entryPoint = "";                               // LowerForDescriptorBuffer's a_entryPoint
		std::function<void(Checks&, const std::string&)> expect;  // successful cases
		const char* errorContains = nullptr;                       // negative cases: the error must contain this
	};

	// Checks every lowered module must pass.
	void CommonChecks(Checks& a_checks, const std::string& a_text) {
		a_checks.HasNo(a_text, "Binding 100000[012]$", "a shader ABI heap binding (1000000-1000002) is left");
		a_checks.That(CountLines(a_text, "= OpVariable %\\S+ PushConstant$") <= 1, "more than one PushConstant variable");
		if (CountLines(a_text, "OpTypePointer PhysicalStorageBuffer") > 0) {
			a_checks.Has(a_text, "OpMemoryModel PhysicalStorageBuffer64", "PhysicalStorageBuffer pointers without the PhysicalStorageBuffer64 addressing model");
			a_checks.Has(a_text, "OpLoad .* Aligned [0-9]+", "PhysicalStorageBuffer pointers but no Aligned load");
		}
	}

	bool RunCase(Case& a_case) {
		std::vector<uint32_t> spirv;
		if (!LoadFixture(a_case.fixture, spirv)) {
			std::printf("FAIL %s: fixture %s missing\n", a_case.name, a_case.fixture);
			return false;
		}
		a_case.target.staticSamplerElements = a_case.staticSamplerElements;
		std::vector<uint32_t> lowered;
		std::string error;
		const bool ok = LowerForDescriptorBuffer(spirv, a_case.plan, a_case.stage, a_case.target, lowered, error, a_case.entryPoint);

		if (a_case.errorContains) {
			if (ok) {
				std::printf("FAIL %s: lowering succeeded, expected an error containing \"%s\"\n", a_case.name, a_case.errorContains);
				return false;
			}
			if (error.find(a_case.errorContains) == std::string::npos) {
				std::printf("FAIL %s: error \"%s\" does not contain \"%s\"\n", a_case.name, error.c_str(), a_case.errorContains);
				return false;
			}
			std::printf("ok   %s: %s\n", a_case.name, error.c_str());
			return true;
		}

		if (!ok) {
			std::printf("FAIL %s: %s\n", a_case.name, error.c_str());
			return false;
		}
		const std::string text = Disassemble(lowered);
		Checks checks;
		std::string validation;
		checks.That(ValidateLoweredModule(lowered, validation), "validation: " + validation);
		CommonChecks(checks, text);
		if (a_case.expect) {
			a_case.expect(checks, text);
		}
		if (!checks.failures.empty()) {
			std::printf("FAIL %s\n", a_case.name);
			for (const std::string& failure : checks.failures) {
				std::printf("  %s\n", failure.c_str());
			}
			std::printf("---- lowered module ----\n%s------------------------\n", text.c_str());
			return false;
		}
		std::printf("ok   %s (%zu words)\n", a_case.name, lowered.size());
		return true;
	}

	// ---- Cases -----------------------------------------------------------------------------------------------------------

	std::vector<Case> Cases() {
		std::vector<Case> cases;

		{
			// Every resource type through the ABI heaps: decorations only.
			Case c{ .name = "heap resource types", .fixture = "heap_types", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Has(a_text, "OpDecorate %SamplerDescriptorHeap DescriptorSet 1$", "sampler heap not at set 1");
				a_checks.Has(a_text, "OpDecorate %ResourceDescriptorHeap Binding 0$", "resource heap not at binding 0");
				a_checks.Has(a_text, "OpDecorate %counter_var_ResourceDescriptorHeap Binding 0$", "counter heap not on the resource heap");
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 0, "push constant variables");
				a_checks.HasNo(a_text, "PhysicalStorageBuffer", "unexpected PhysicalStorageBuffer use");
			};
			cases.push_back(std::move(c));
		}
		{
			// Three RootConstants32 ranges, 128 bytes, all pushed.
			Case c{ .name = "root constants (all pushed)", .fixture = "root_constants", .stage = VK_SHADER_STAGE_VERTEX_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(PushData(0, 0, 0, 16));
			c.plan.mappings.push_back(PushData(0, 1, 16, 48));
			c.plan.mappings.push_back(PushData(0, 2, 64, 64));
			c.plan.pushDataBytes = 128;
			c.target.pushedBytes = 128;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform block variables left");
				a_checks.Has(a_text, "OpMemberDecorate %BasicRHIPushData 0 Offset 0$", "DrawConstants not at push offset 0");
				a_checks.Has(a_text, "OpMemberDecorate %BasicRHIPushData 1 Offset 16$", "Lights not at push offset 16");
				a_checks.Has(a_text, "OpMemberDecorate %BasicRHIPushData 2 Offset 64$", "Transform not at push offset 64");
				a_checks.HasNo(a_text, "PhysicalStorageBuffer", "unexpected PhysicalStorageBuffer use");
			};
			cases.push_back(std::move(c));
		}
		{
			// 176 bytes of push data: b0 [0, 64) and b1 [64, 112) pushed, b2 [112, 176) spilled; the spill address at 120.
			Case c{ .name = "push spill", .fixture = "spill", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectIndex(1, 0, 2, 0, 0, false));   // textures: record address = PassConstants.textureRecord
			c.plan.mappings.push_back(IndirectIndex(2, 0, 1, 0, 8, true));    // samplers
			c.plan.mappings.push_back(IndirectAddress(0, 3, 1, 160, 0));      // Material: record address in the spilled b2
			c.plan.mappings.push_back(PushAddress(0, 4, 1, 72));              // Object: address in the undeclared b1
			c.plan.mappings.push_back(HeapSlot(3, 0, 1));                     // output: heap slot 0
			c.plan.mappings.push_back(PushData(0, 0, 0, 64));
			c.plan.mappings.push_back(PushData(0, 1, 64, 48));
			c.plan.mappings.push_back(PushData(0, 2, 112, 64));
			c.plan.pushDataBytes = 176;
			c.target.pushedBytes = 112;
			c.target.spillAddressOffset = 120;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform block variables left");
				a_checks.Has(a_text, "OpMemoryModel PhysicalStorageBuffer64", "no PhysicalStorageBuffer64 addressing");
				a_checks.Has(a_text, "OpIAddCarry", "no address arithmetic");
				a_checks.Has(a_text, "OpDecorate %BasicRHIResourceHeap DescriptorSet 0$", "no resource heap alias");
				a_checks.Has(a_text, "OpDecorate %BasicRHISamplerHeap DescriptorSet 1$", "no sampler heap alias");
				a_checks.Has(a_text, "OpDecorate %output DescriptorSet 0$", "heap slot 0 not re-decorated");
				a_checks.Has(a_text, "Offset 120$", "no push member for the spill address");
				a_checks.Has(a_text, "Offset 72$", "no push member for the undeclared b1 address word");
			};
			cases.push_back(std::move(c));
		}
		{
			// IndirectIndex textures and samplers, IndirectAddress and PushAddress cbuffers; the addresses live in the declared Root.
			Case c{ .name = "indirect bindings", .fixture = "indirect", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectIndex(1, 0, 3, 0, 0, false));
			c.plan.mappings.push_back(IndirectIndex(2, 0, 2, 0, 16, true));
			c.plan.mappings.push_back(IndirectAddress(0, 1, 2, 0, 32));
			c.plan.mappings.push_back(PushAddress(0, 3, 1, 8));
			c.plan.mappings.push_back(PushData(0, 0, 0, 20));
			c.plan.pushDataBytes = 20;
			c.target.pushedBytes = 20;
			c.target.uniformBlockAlignment = 16;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform block variables left");
				a_checks.Count(a_text, "OpMemberDecorate %BasicRHIPushData [0-9]+ Offset", 1, "push block members (the addresses must be read through Root)");
				a_checks.Has(a_text, "OpMemoryModel PhysicalStorageBuffer64", "no PhysicalStorageBuffer64 addressing");
				a_checks.Has(a_text, "Aligned 16$", "no 16-byte aligned block load");
				a_checks.Has(a_text, "OpDecorate %BasicRHISamplerHeap DescriptorSet 1$", "no sampler heap alias");
			};
			cases.push_back(std::move(c));
		}
		{
			// HeapSlot ranges (elements 10 + i, 0 and 5) and static samplers in the sampler heap's tail (2048, 2049 + i).
			Case c{ .name = "heap slots and static samplers", .fixture = "heap_slots", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(1, 10, 4));
			c.plan.mappings.push_back(HeapSlot(3, 0, 1));
			c.plan.mappings.push_back(HeapSlot(3, 5, 1));
			c.plan.mappings.push_back(StaticSampler(2, 0, 1, 0));
			c.plan.mappings.push_back(StaticSampler(2, 1, 2, 1));
			c.staticSamplerElements = { 2048, 2049 };
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 0, "push constant variables");
				a_checks.HasNo(a_text, "PhysicalStorageBuffer", "unexpected PhysicalStorageBuffer use");
				a_checks.Has(a_text, "OpDecorate %output Binding 0$", "heap slot 0 not re-decorated");
				a_checks.Has(a_text, "OpDecorate %BasicRHISamplerHeap DescriptorSet 1$", "no sampler heap alias");
				a_checks.Has(a_text, "OpConstant %uint 2048$", "static sampler element 2048 missing");
				a_checks.Has(a_text, "OpConstant %uint 10$", "heap slot element 10 missing");
				a_checks.Has(a_text, "OpConstant %uint 5$", "heap slot element 5 missing");
			};
			cases.push_back(std::move(c));
		}
		{
			// DCLF-style engine shader: per-draw b190 (96 B) pushed, per-pass b191 (128 B) spilled, the spill address at 120.
			Case c{ .name = "DCLF engine shader", .fixture = "dclf_engine", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectAddress(0, 0, 1, 0, 0));        // b0: the pipeline row (draw words 0-1)
			c.plan.mappings.push_back(PushAddress(0, 2, 1, 16));              // b2: the geometry block address (draw words 4-5)
			c.plan.mappings.push_back(IndirectAddress(0, 4, 1, 96, 32));      // b4: from the frame record, whose address is per-pass: spilled
			c.plan.mappings.push_back(IndirectIndex(0, 200, 4, 8, 16, false)); // t0-t3: the material row (draw words 2-3)
			c.plan.mappings.push_back(IndirectIndex(0, 400, 4, 8, 48, true));  // s0-s3
			c.plan.mappings.push_back(PushData(0, 190, 0, 96));
			c.plan.mappings.push_back(PushData(0, 191, 96, 128));
			c.plan.pushDataBytes = 224;
			c.target.pushedBytes = 96;
			c.target.spillAddressOffset = 120;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform block variables left");
				a_checks.Count(a_text, "= OpVariable %\\S+ UniformConstant$", 2, "descriptor variables (one resource and one sampler heap alias)");
				a_checks.Has(a_text, "OpMemoryModel PhysicalStorageBuffer64", "no PhysicalStorageBuffer64 addressing");
				a_checks.Has(a_text, "OpMemberDecorate %BasicRHIPushData [0-9]+ Offset 120$", "no push member for the spill address");
			};
			cases.push_back(std::move(c));
		}

		{
			// The same bindings with minUniformBufferOffsetAlignment 4: block loads may claim no more than 4 (a vec4 at offset 16, 64,
			// or dynamically indexed), the record reads recordAlignment 8 at offset 0 and 4 past it.
			Case c{ .name = "uniform block alignment 4", .fixture = "indirect", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectIndex(1, 0, 3, 0, 0, false));
			c.plan.mappings.push_back(IndirectIndex(2, 0, 2, 0, 16, true));
			c.plan.mappings.push_back(IndirectAddress(0, 1, 2, 0, 32));
			c.plan.mappings.push_back(PushAddress(0, 3, 1, 8));
			c.plan.mappings.push_back(PushData(0, 0, 0, 20));
			c.plan.pushDataBytes = 20;
			c.target.pushedBytes = 20;
			c.target.uniformBlockAlignment = 4;
			c.target.recordAlignment = 8;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Has(a_text, "OpLoad %v4float %\\S+ Aligned 4$", "no 4-byte aligned vec4 block load");
				a_checks.HasNo(a_text, "OpLoad %(v4float|mat4v4float|mat2v3float|uint) %\\S+ Aligned 16$", "a load claims 16 bytes");
				a_checks.HasNo(a_text, "OpLoad %(v4float|mat4v4float|mat2v3float) %\\S+ Aligned 8$", "a block load claims 8 bytes");
				a_checks.Has(a_text, "OpLoad %uint %\\S+ Aligned 8$", "no 8-byte aligned record index load (record offset 0)");
				a_checks.Has(a_text, "OpLoad %uint %\\S+ Aligned 4$", "no 4-byte aligned record index load (record offset 4)");
			};
			cases.push_back(std::move(c));
		}
		{
			// -fspv-debug=vulkan-with-source: DebugGlobalVariable references every resource variable; buffer mode strips the debug info.
			Case c{ .name = "NonSemantic debug info", .fixture = "indirect_debug", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectIndex(1, 0, 3, 0, 0, false));
			c.plan.mappings.push_back(IndirectIndex(2, 0, 2, 0, 16, true));
			c.plan.mappings.push_back(IndirectAddress(0, 1, 2, 0, 32));
			c.plan.mappings.push_back(PushAddress(0, 3, 1, 8));
			c.plan.mappings.push_back(PushData(0, 0, 0, 20));
			c.plan.pushDataBytes = 20;
			c.target.pushedBytes = 20;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform block variables left");
				a_checks.HasNo(a_text, "NonSemantic", "NonSemantic instructions left");
			};
			cases.push_back(std::move(c));
		}
		{
			// A library with two compute entry points: only "second" (and its execution mode) is kept.
			Case c{ .name = "entry point selection", .fixture = "two_entries", .stage = VK_SHADER_STAGE_COMPUTE_BIT, .entryPoint = "second" };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(0, 0, 2));
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.Count(a_text, "OpEntryPoint ", 1, "entry points");
				a_checks.Has(a_text, "OpEntryPoint GLCompute %\\S+ \"second\"", "entry point 'second' missing");
				a_checks.Has(a_text, "OpExecutionMode %\\S+ LocalSize 2 1 1$", "the kept entry point's execution mode missing");
				a_checks.HasNo(a_text, "LocalSize 1 1 1$", "the dropped entry point's execution mode left");
				a_checks.HasNo(a_text, "first_output", "the dropped entry point's buffer left");
			};
			cases.push_back(std::move(c));
		}

		// Negative cases.
		{
			Case c{ .name = "several entry points without a name fail", .fixture = "two_entries", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(0, 0, 2));
			c.errorContains = "2 entry points";
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "unknown entry point fails", .fixture = "two_entries", .stage = VK_SHADER_STAGE_COMPUTE_BIT, .entryPoint = "third" };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(0, 0, 2));
			c.errorContains = "no entry point named 'third'";
			cases.push_back(std::move(c));
		}
		{
			// Lights declares 48 bytes; its range has 32.
			Case c{ .name = "cbuffer larger than its range fails", .fixture = "root_constants", .stage = VK_SHADER_STAGE_VERTEX_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(PushData(0, 0, 0, 16));
			c.plan.mappings.push_back(PushData(0, 1, 16, 32));
			c.plan.mappings.push_back(PushData(0, 2, 48, 64));
			c.plan.pushDataBytes = 112;
			c.target.pushedBytes = 112;
			c.errorContains = "more than its root-constant range";
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "unmapped binding fails", .fixture = "root_constants", .stage = VK_SHADER_STAGE_VERTEX_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(PushData(0, 0, 0, 16));
			c.plan.mappings.push_back(PushData(0, 1, 16, 48));
			c.plan.pushDataBytes = 64;
			c.target.pushedBytes = 64;
			c.errorContains = "binding 2, uniform buffer) is not mapped";
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "combined image sampler fails", .fixture = "combined_sampler", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(0, 0, 1));
			c.errorContains = "combined image samplers are not supported";
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "spilled data without a spill address fails", .fixture = "root_constants", .stage = VK_SHADER_STAGE_VERTEX_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(PushData(0, 0, 0, 16));
			c.plan.mappings.push_back(PushData(0, 1, 16, 48));
			c.plan.mappings.push_back(PushData(0, 2, 64, 64));
			c.plan.pushDataBytes = 128;
			c.target.pushedBytes = 64;
			c.errorContains = "spills nothing";
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "wrong stage fails", .fixture = "heap_types", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.errorContains = "is not for the stage";
			cases.push_back(std::move(c));
		}
		return cases;
	}
}

int main() {
	std::vector<Case> cases = Cases();
	int failures = 0;
	for (Case& c : cases) {
		failures += RunCase(c) ? 0 : 1;
	}
	std::printf("%d of %zu lowering cases failed\n", failures, cases.size());
	return failures == 0 ? 0 : 1;
}
