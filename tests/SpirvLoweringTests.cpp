// The descriptor-buffer lowering tests (rhi_vulkan_spirv_lowering.h): HLSL fixtures (tests/lowering, compiled at build time with
// the shader ABI's DXC flags) lowered with hand-built binding plans (as VkBuildBindingPlan builds them), validated, and checked
// structurally on the disassembly. Negative cases must fail with a clear message. No GPU needed.

#include "rhi_vulkan_spirv_lowering.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

	// Every descriptor variable's (set, binding), keyed by its descriptor type's name (the variable's pointee without its array).
	std::map<std::string, std::set<std::pair<uint32_t, uint32_t>>> BindingsByType(const std::string& a_text) {
		const std::regex pointer(R"(^\s*(%\S+) = OpTypePointer \S+ (%\S+)$)");
		const std::regex array(R"(^\s*(%\S+) = OpType(?:Runtime)?Array (%\S+))");
		const std::regex variable(R"(^\s*(%\S+) = OpVariable (%\S+) (UniformConstant|Uniform|StorageBuffer)$)");
		const std::regex decoration(R"(^\s*OpDecorate (%\S+) (DescriptorSet|Binding) ([0-9]+)$)");
		std::map<std::string, std::string> pointees, elements, variables;
		std::map<std::string, uint32_t> sets, bindings;
		std::istringstream lines(a_text);
		for (std::string line; std::getline(lines, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			std::smatch match;
			if (std::regex_search(line, match, pointer)) {
				pointees[match[1]] = match[2];
			}
			else if (std::regex_search(line, match, array)) {
				elements[match[1]] = match[2];
			}
			else if (std::regex_search(line, match, variable)) {
				variables[match[1]] = match[2];
			}
			else if (std::regex_search(line, match, decoration)) {
				(match[2] == "Binding" ? bindings : sets)[match[1]] = uint32_t(std::stoul(match[3]));
			}
		}
		std::map<std::string, std::set<std::pair<uint32_t, uint32_t>>> result;
		for (const auto& [name, pointerType] : variables) {
			if (!bindings.count(name)) {
				continue;
			}
			std::string type = pointees[pointerType];
			if (const auto element = elements.find(type); element != elements.end()) {
				type = element->second;
			}
			result[type.substr(1)].insert({ sets[name], bindings[name] });
		}
		return result;
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
		// Every variable of descriptor type a_type is at (a_set, a_binding).
		void BoundAt(const std::string& a_text, const std::string& a_type, uint32_t a_set, uint32_t a_binding) {
			const auto all = BindingsByType(a_text);
			const auto found = all.find(a_type);
			std::string where;
			if (found != all.end()) {
				for (const auto& [set, binding] : found->second) {
					where += " (" + std::to_string(set) + ", " + std::to_string(binding) + ")";
				}
			}
			That(found != all.end() && found->second == std::set<std::pair<uint32_t, uint32_t>>{ { a_set, a_binding } },
				a_type + " not only at (" + std::to_string(a_set) + ", " + std::to_string(a_binding) + "); found at" + (where.empty() ? " nothing" : where));
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

	const char* g_dumpDir = nullptr;  // main's --dump

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
		if (g_dumpDir) {
			std::string file = a_case.name;
			for (char& ch : file) {
				ch = std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
			}
			std::ofstream(std::string(g_dumpDir) + "/" + file + ".spvasm", std::ios::binary) << text;
		}
		std::printf("ok   %s (%zu words)\n", a_case.name, lowered.size());
		return true;
	}

	// perType with every kind on resourceBinding (cbvAsStorageBuffer off) must lower to the same words as the default target:
	// the per-type routing only picks bindings.
	bool RunMutableIdentity(Case& a_case) {
		std::vector<uint32_t> spirv;
		if (!LoadFixture(a_case.fixture, spirv)) {
			std::printf("FAIL %s (mutable identity): fixture %s missing\n", a_case.name, a_case.fixture);
			return false;
		}
		a_case.target.staticSamplerElements = a_case.staticSamplerElements;
		LoweringTarget perType = a_case.target;
		perType.perType = true;
		for (uint32_t& binding : perType.kindBindings) {
			binding = perType.resourceBinding;
		}
		std::vector<uint32_t> expected, lowered;
		std::string error;
		if (!LowerForDescriptorBuffer(spirv, a_case.plan, a_case.stage, a_case.target, expected, error, a_case.entryPoint) ||
			!LowerForDescriptorBuffer(spirv, a_case.plan, a_case.stage, perType, lowered, error, a_case.entryPoint)) {
			std::printf("FAIL %s (mutable identity): %s\n", a_case.name, error.c_str());
			return false;
		}
		if (lowered != expected) {
			std::printf("FAIL %s (mutable identity): per-type lowering on one binding differs from the default\n", a_case.name);
			return false;
		}
		std::printf("ok   %s (mutable identity)\n", a_case.name);
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

		// Per-type heaps and constant buffers as storage buffers (LoweringTarget::perType, cbvAsStorageBuffer).
		const auto perTypeTarget = [](LoweringTarget& a_target) {
			a_target.perType = true;
			for (uint32_t k = 0; k < uint32_t(LoweringTarget::Kind::Count); ++k) {
				a_target.kindBindings[k] = 10 + k;  // 10 sampled image ... 14 storage buffer, 15 uniform buffer, 16 acceleration structure
			}
		};
		{
			// Every resource type through the ABI heaps onto its kind's binding. Typed buffers are texel buffers; the counter heap's
			// buffer is a storage buffer.
			Case c{ .name = "per-type heap resource types", .fixture = "heap_types", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			perTypeTarget(c.target);
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_2d_image", 0, 10);
				a_checks.BoundAt(a_text, "type_2d_image_0", 0, 11);         // RWTexture2D
				a_checks.BoundAt(a_text, "type_buffer_image", 0, 12);       // Buffer<float4>
				a_checks.BoundAt(a_text, "type_buffer_image_0", 0, 13);     // RWBuffer<float4>
				a_checks.BoundAt(a_text, "type_ByteAddressBuffer", 0, 14);
				a_checks.BoundAt(a_text, "type_RWStructuredBuffer_Item", 0, 14);
				a_checks.BoundAt(a_text, "type_ACSBuffer_counter", 0, 14);  // the counter heap
				a_checks.BoundAt(a_text, "type_RWByteAddressBuffer", 0, 14);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Params", 0, 15);
				a_checks.BoundAt(a_text, "accelerationStructureNV", 0, 16);
				a_checks.BoundAt(a_text, "type_sampler", 1, 0);
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 1, "uniform variables (the constant buffer)");
			};
			cases.push_back(std::move(c));
		}
		{
			// The same with cbvAsStorageBuffer: the ConstantBuffer joins the storage buffers, read-only.
			Case c{ .name = "per-type heap with constant buffers as storage buffers", .fixture = "heap_types", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			perTypeTarget(c.target);
			c.target.cbvAsStorageBuffer = true;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Params", 0, 14);
				a_checks.BoundAt(a_text, "type_buffer_image", 0, 12);
				a_checks.BoundAt(a_text, "type_buffer_image_0", 0, 13);
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform variables");
				a_checks.HasNo(a_text, "OpTypePointer Uniform ", "Uniform pointers left");
				a_checks.HasNo(a_text, "Binding 15$", "something on the uniform buffer binding");
				a_checks.Count(a_text, "OpDecorate %\\S+ NonWritable$", 1, "NonWritable variables (the constant buffer)");
			};
			cases.push_back(std::move(c));
		}
		{
			// Heap slot aliases (elements 10 + i and 5) and a re-decorated element 0 on their kinds' bindings; samplers unchanged.
			Case c{ .name = "per-type heap slots", .fixture = "heap_slots", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(HeapSlot(1, 10, 4));
			c.plan.mappings.push_back(HeapSlot(3, 0, 1));
			c.plan.mappings.push_back(HeapSlot(3, 5, 1));
			c.plan.mappings.push_back(StaticSampler(2, 0, 1, 0));
			c.plan.mappings.push_back(StaticSampler(2, 1, 2, 1));
			c.staticSamplerElements = { 2048, 2049 };
			perTypeTarget(c.target);
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_2d_image", 0, 10);
				a_checks.Has(a_text, "OpDecorate %output Binding 11$", "heap slot 0 (a storage image) not on the storage image binding");
				a_checks.BoundAt(a_text, "type_RWByteAddressBuffer", 0, 14);
				a_checks.BoundAt(a_text, "type_sampler", 1, 0);
				a_checks.Has(a_text, "OpConstant %uint 2048$", "static sampler element 2048 missing");
			};
			cases.push_back(std::move(c));
		}
		{
			// IndirectIndex textures: the alias is on the sampled image binding. Register-bound cbuffers on the push and address
			// paths are unchanged by cbvAsStorageBuffer.
			Case c{ .name = "per-type indirect bindings", .fixture = "indirect", .stage = VK_SHADER_STAGE_FRAGMENT_BIT };
			AddAbiHeaps(c.plan);
			c.plan.mappings.push_back(IndirectIndex(1, 0, 3, 0, 0, false));
			c.plan.mappings.push_back(IndirectIndex(2, 0, 2, 0, 16, true));
			c.plan.mappings.push_back(IndirectAddress(0, 1, 2, 0, 32));
			c.plan.mappings.push_back(PushAddress(0, 3, 1, 8));
			c.plan.mappings.push_back(PushData(0, 0, 0, 20));
			c.plan.pushDataBytes = 20;
			c.target.pushedBytes = 20;
			perTypeTarget(c.target);
			c.target.cbvAsStorageBuffer = true;
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_2d_image", 0, 10);
				a_checks.BoundAt(a_text, "type_sampler", 1, 0);
				a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ StorageBuffer$", 0, "storage buffer variables (no heap constant buffer here)");
				a_checks.Has(a_text, "OpMemoryModel PhysicalStorageBuffer64", "no PhysicalStorageBuffer64 addressing");
			};
			cases.push_back(std::move(c));
		}

		// cbv_heap's plan: Root pushed, Material at heap slot 4 (an alias), indirect through the record at push byte 16.
		const auto cbvPlan = [](Case& a_case) {
			AddAbiHeaps(a_case.plan);
			a_case.plan.mappings.push_back(PushData(0, 0, 0, 24));
			a_case.plan.mappings.push_back(BindingMapping{ .set = 1, .firstBinding = 1, .bindingCount = 1, .resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT,
				.source = BindingSource::Heap, .heapElement = 4 });
			a_case.plan.mappings.push_back(IndirectIndex(2, 0, 1, 16, 0, false));
			a_case.plan.pushDataBytes = 24;
			a_case.target.pushedBytes = 24;
		};
		const auto cbvLayout = [](Checks& a_checks, const std::string& a_text) {
			a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 0, "uniform variables");
			a_checks.HasNo(a_text, "OpTypePointer Uniform ", "Uniform pointers left");
			a_checks.Count(a_text, "= OpVariable %\\S+ PushConstant$", 1, "push constant variables (Root)");
			a_checks.Has(a_text, "OpDecorate %OrgConstantBuffers_lens NonWritable$", "ORG's constant buffer array is not NonWritable");
			a_checks.Has(a_text, "OpDecorate %ResourceDescriptorHeap NonWritable$", "the static ConstantBuffer's heap variable is not NonWritable");
			a_checks.Count(a_text, "OpDecorate %BasicRHIResourceHeap(_[0-9]+)? NonWritable$", 2, "NonWritable heap aliases (Material, indirect)");
			// The layout decorations stay: dx-layout offsets, array strides, matrix strides and majorness.
			a_checks.Has(a_text, "OpDecorate %type_ConstantBuffer_Lens Block$", "Lens lost Block");
			a_checks.Has(a_text, "OpMemberDecorate %type_ConstantBuffer_Lens 1 RowMajor$", "Lens.skew lost RowMajor");
			a_checks.Has(a_text, "OpMemberDecorate %type_ConstantBuffer_Lens 1 MatrixStride 16$", "Lens.skew lost MatrixStride");
			a_checks.Has(a_text, "OpMemberDecorate %type_ConstantBuffer_Lens 2 Offset 188$", "Lens.flags lost its dx-layout offset");
			a_checks.Has(a_text, "OpDecorate %_arr_v4float_uint_8 ArrayStride 16$", "Lens.weights lost ArrayStride");
			a_checks.Has(a_text, "OpMemberDecorate %type_ConstantBuffer_Frame 0 ColMajor$", "Frame.viewProjection lost its majorness");
			a_checks.Has(a_text, "OpMemberDecorate %type_Material 1 MatrixStride 16$", "Material.rotation lost MatrixStride");
			// Dynamic indexing into an array inside the buffer reads it in place.
			a_checks.Has(a_text, "OpAccessChain %_ptr_StorageBuffer_v4float\\S* %OrgConstantBuffers_lens %\\S+ %\\S+ %\\S+$",
				"no in-place dynamic access chain into Lens.weights");
			a_checks.HasNo(a_text, "OpVariable %_ptr_Function_type_ConstantBuffer", "a constant buffer copied into function memory");
		};
		{
			Case c{ .name = "constant buffers as storage buffers", .fixture = "cbv_heap", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			cbvPlan(c);
			c.target.cbvAsStorageBuffer = true;
			c.expect = [cbvLayout](Checks& a_checks, const std::string& a_text) {
				cbvLayout(a_checks, a_text);
				// Without perType they stay on the mutable binding, typed as storage buffers.
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Lens", 0, 0);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Frame", 0, 0);
				a_checks.BoundAt(a_text, "type_Material", 0, 0);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Indirect", 0, 0);
				a_checks.Count(a_text, "= OpVariable %\\S+ StorageBuffer$", 5, "storage buffer variables (four constant buffers and the output)");
			};
			cases.push_back(std::move(c));
		}
		{
			Case c{ .name = "per-type constant buffers as storage buffers", .fixture = "cbv_heap", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			cbvPlan(c);
			perTypeTarget(c.target);
			c.target.cbvAsStorageBuffer = true;
			c.expect = [cbvLayout](Checks& a_checks, const std::string& a_text) {
				cbvLayout(a_checks, a_text);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Lens", 0, 14);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Frame", 0, 14);
				a_checks.BoundAt(a_text, "type_Material", 0, 14);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Indirect", 0, 14);
				a_checks.BoundAt(a_text, "type_RWByteAddressBuffer", 0, 14);
				a_checks.HasNo(a_text, "Binding 15$", "something on the uniform buffer binding");
			};
			cases.push_back(std::move(c));
		}
		{
			// perType alone: heap constant buffers stay uniform buffers, on the uniform buffer binding.
			Case c{ .name = "per-type uniform buffers", .fixture = "cbv_heap", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			cbvPlan(c);
			perTypeTarget(c.target);
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Lens", 0, 15);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Frame", 0, 15);
				a_checks.BoundAt(a_text, "type_Material", 0, 15);
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Indirect", 0, 15);
				a_checks.BoundAt(a_text, "type_RWByteAddressBuffer", 0, 14);
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 4, "uniform variables");
				a_checks.HasNo(a_text, "NonWritable$", "NonWritable without cbvAsStorageBuffer");
			};
			cases.push_back(std::move(c));
		}
		{
			// The default target: heap constant buffers stay uniform buffers on the mutable binding.
			Case c{ .name = "heap constant buffers (default target)", .fixture = "cbv_heap", .stage = VK_SHADER_STAGE_COMPUTE_BIT };
			cbvPlan(c);
			c.expect = [](Checks& a_checks, const std::string& a_text) {
				a_checks.BoundAt(a_text, "type_ConstantBuffer_Lens", 0, 0);
				a_checks.BoundAt(a_text, "type_Material", 0, 0);
				a_checks.Count(a_text, "= OpVariable %\\S+ Uniform$", 4, "uniform variables");
				a_checks.Count(a_text, "= OpVariable %\\S+ StorageBuffer$", 1, "storage buffer variables (the output)");
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

int main(int a_argc, char** a_argv) {
	// --dump DIR writes each successful case's lowered disassembly to DIR, to compare two builds of the lowering.
	if (a_argc == 3 && std::string(a_argv[1]) == "--dump") {
		g_dumpDir = a_argv[2];
	}
	std::vector<Case> cases = Cases();
	int failures = 0;
	size_t identityCases = 0;
	for (Case& c : cases) {
		failures += RunCase(c) ? 0 : 1;
	}
	for (Case& c : cases) {
		if (!c.errorContains && !c.target.perType && !c.target.cbvAsStorageBuffer) {
			failures += RunMutableIdentity(c) ? 0 : 1;
			++identityCases;
		}
	}
	std::printf("%d of %zu lowering cases failed\n", failures, cases.size() + identityCases);
	return failures == 0 ? 0 : 1;
}
