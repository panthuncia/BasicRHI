#include "rhi_vulkan_spirv_lowering.h"

// The descriptor-buffer lowering (rhi_vulkan_spirv_lowering.h): one SPIRV-Tools optimizer pass that rewrites every resource
// variable of a stage to what the binding plan maps it to.
//
// Per variable (found by DescriptorSet/Binding and its declared type, VK_SPIRV_RESOURCE_TYPE_*):
//   Heap, StaticSampler at heap element 0 (the shader ABI's heap bindings): re-decorated to the universal layout's heap binding.
//     DXC's aliased typed runtime arrays stay aliased: they are valid against a MUTABLE_EXT binding.
//   Heap, StaticSampler at element e != 0, HeapIndirect: every use becomes an access chain into a typed alias of the heap binding
//     (one alias variable per descriptor type), at e plus the variable's own array index. HeapIndirect's e is a uint32 loaded once per
//     function, in its entry block, through a PhysicalStorageBuffer pointer at the record address (push data) + recordOffset + 4i.
//   PushData inside the pushed prefix: a member of the one push constant block (see BuildPushBlock); access chains get the member
//     index prepended and are retyped Uniform -> PushConstant.
//   PushData past the pushed prefix (spilled), BufferIndirectAddress, BufferPushAddress: the uniform block becomes a
//     PhysicalStorageBuffer pointer to the same Block type, bitcast from a uvec2 address recomputed at each use (a
//     BufferIndirectAddress's read from the record once per function, in its entry block, as HeapIndirect's index is); access chains
//     and loads are retyped through a worklist and every load carries Aligned (see Walk).
// Address arithmetic stays 32-bit (uvec2 + OpIAddCarry), so no Int64 capability is needed.

#include <algorithm>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "source/opcode.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/pass.h"
#include "source/util/string_utils.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace rhi::vulkan_binding {

	namespace {

		namespace opt = spvtools::opt;
		using opt::Instruction;
		using opt::IRContext;
		using opt::Operand;

		template<class T>
		void AppendText(std::string& a_text, const T& a_value) {
			if constexpr (std::is_integral_v<T>) {
				a_text += std::to_string(a_value);
			}
			else {
				a_text += std::string_view(a_value);
			}
		}

		template<class... Args>
		std::string Cat(const Args&... a_args) {
			std::string text;
			(AppendText(text, a_args), ...);
			return text;
		}

		struct LoweringFailure : std::runtime_error {
			using std::runtime_error::runtime_error;
		};

		template<class... Args>
		[[noreturn]] void Fail(const Args&... a_args) {
			throw LoweringFailure(Cat(a_args...));
		}

		uint32_t LowBit(uint32_t a_value) noexcept { return a_value & (~a_value + 1u); }

		// The alignment a byte offset from an a_base-aligned address keeps.
		uint32_t AlignmentAt(uint32_t a_base, uint32_t a_offset) noexcept {
			return a_offset == 0 ? a_base : (std::min)(a_base, LowBit(a_offset));
		}

		const char* KindName(VkFlags a_kind) noexcept {
			switch (a_kind) {
			case VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT: return "sampler";
			case VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT: return "sampled image";
			case VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT: return "read-only storage image";
			case VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT: return "read-write storage image";
			case VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT: return "combined image sampler";
			case VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT: return "uniform buffer";
			case VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT: return "read-only storage buffer";
			case VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT: return "read-write storage buffer";
			case VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT: return "acceleration structure";
			default: return "resource";
			}
		}

		const char* SourceName(BindingSource a_source) noexcept {
			switch (a_source) {
			case BindingSource::Heap: return "Heap";
			case BindingSource::HeapIndirect: return "HeapIndirect";
			case BindingSource::BufferIndirectAddress: return "BufferIndirectAddress";
			case BindingSource::BufferPushAddress: return "BufferPushAddress";
			case BindingSource::PushData: return "PushData";
			case BindingSource::StaticSampler: return "StaticSampler";
			default: return "?";
			}
		}

		std::optional<spv::ExecutionModel> ExecutionModelFor(VkShaderStageFlagBits a_stage) noexcept {
			switch (a_stage) {
			case VK_SHADER_STAGE_VERTEX_BIT: return spv::ExecutionModel::Vertex;
			case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return spv::ExecutionModel::TessellationControl;
			case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return spv::ExecutionModel::TessellationEvaluation;
			case VK_SHADER_STAGE_GEOMETRY_BIT: return spv::ExecutionModel::Geometry;
			case VK_SHADER_STAGE_FRAGMENT_BIT: return spv::ExecutionModel::Fragment;
			case VK_SHADER_STAGE_COMPUTE_BIT: return spv::ExecutionModel::GLCompute;
			case VK_SHADER_STAGE_TASK_BIT_EXT: return spv::ExecutionModel::TaskEXT;
			case VK_SHADER_STAGE_MESH_BIT_EXT: return spv::ExecutionModel::MeshEXT;
			case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return spv::ExecutionModel::RayGenerationKHR;
			case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return spv::ExecutionModel::AnyHitKHR;
			case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return spv::ExecutionModel::ClosestHitKHR;
			case VK_SHADER_STAGE_MISS_BIT_KHR: return spv::ExecutionModel::MissKHR;
			case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return spv::ExecutionModel::IntersectionKHR;
			case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return spv::ExecutionModel::CallableKHR;
			default: return std::nullopt;
			}
		}

		bool IsBookkeeping(const Instruction* a_inst) noexcept {
			switch (a_inst->opcode()) {
			case spv::Op::OpName:
			case spv::Op::OpMemberName:
			case spv::Op::OpDecorate:
			case spv::Op::OpDecorateId:
			case spv::Op::OpDecorateString:
			case spv::Op::OpMemberDecorate:
			case spv::Op::OpEntryPoint:
				return true;
			default:
				return false;
			}
		}

		bool IsAccessChain(spv::Op a_op) noexcept { return a_op == spv::Op::OpAccessChain || a_op == spv::Op::OpInBoundsAccessChain; }

		// A block member's matrix layout (MatrixStride, RowMajor), carried down through arrays of matrices.
		struct MatrixLayout {
			uint32_t stride = 0;
			bool rowMajor = false;
		};

		// The static part of a PhysicalStorageBuffer pointer: the guaranteed alignment of its address and the constant byte offset
		// accumulated since (a load's Aligned is AlignmentAt(align, offset), so the gcd of the base alignment, the static offset and
		// the stride of every dynamic index on the way).
		struct Walk {
			uint32_t align = 0;
			uint32_t offset = 0;
			MatrixLayout layout;
			uint32_t componentStride = 0;  // a vector's component step: a row-major matrix's column steps by MatrixStride
		};

		// How a rewritten block's pointers are retyped: into `storage`, with pointee `from` (the original block struct) becoming
		// `to` (its push constant member clone), if set.
		struct Retarget {
			spv::StorageClass storage = spv::StorageClass::PhysicalStorageBuffer;
			uint32_t from = 0, to = 0;
			uint32_t Remap(uint32_t a_type) const noexcept { return to != 0 && a_type == from ? to : a_type; }
		};

		struct Resource {
			Instruction* var = nullptr;
			std::string description;
			uint32_t set = 0, binding = 0;
			spv::StorageClass storage = spv::StorageClass::UniformConstant;
			uint32_t pointee = 0;   // the variable's type
			uint32_t element = 0;   // the descriptor type: the pointee without its array
			bool arrayed = false;
			VkFlags kind = 0;       // VK_SPIRV_RESOURCE_TYPE_*_BIT_EXT
			const BindingMapping* mapping = nullptr;
			uint32_t index = 0;     // binding - mapping->firstBinding
			bool used = false;
			bool keep = false;      // re-decorated in place
			int clone = -1;         // PushData in the pushed prefix: its push block member (clones)
		};

		// A pushed root-constant cbuffer's member of the push block: a clone of its struct without Block, at its push data offset.
		struct Clone {
			Resource* resource = nullptr;
			uint32_t offset = 0, extent = 0;
			std::vector<uint32_t> padding;  // uint members appended at these struct offsets, for address dwords in padding
			uint32_t type = 0, member = 0;
		};

		// Where a push data dword the lowering reads (an address word half) lives in the push block.
		struct DwordSource {
			int clone = -1;              // -1: its own top-level uint member
			std::vector<uint32_t> path;  // indices below the member
			int padding = -1;            // the clone's padding member
			uint32_t scalar = 0;         // the 32-bit scalar type read
			uint32_t member = 0;         // resolved by BuildPushBlock
		};

		class DescriptorBufferLoweringPass final : public opt::Pass {
		public:
			DescriptorBufferLoweringPass(const BindingPlan& a_plan, VkShaderStageFlagBits a_stage, const LoweringTarget& a_target, std::string& a_error) :
				plan(a_plan), stage(a_stage), target(a_target), error(a_error) {}

			const char* name() const override { return "basicrhi-lower-descriptor-buffer"; }

			Status Process() override {
				try {
					Lower();
					return Status::SuccessWithChange;
				}
				catch (const LoweringFailure& failure) {
					error = failure.what();
				}
				catch (const std::exception& e) {
					error = Cat("descriptor-buffer lowering failed: ", e.what());
				}
				return Status::Failure;
			}

		private:
			const BindingPlan& plan;
			VkShaderStageFlagBits stage;
			const LoweringTarget& target;
			std::string& error;

			Instruction* entry = nullptr;
			std::vector<Resource> resources;
			std::vector<Clone> clones;
			std::set<uint32_t> neededDwords;
			std::map<uint32_t, DwordSource> dwords;
			uint32_t pushVar = 0;
			bool usesPhysicalPointers = false;
			bool usesHeapAliases = false;

			// Type and constant caches (the type manager is not used: the pass adds raw types it would not know).
			uint32_t uintType = 0, uvec2Type = 0, carryType = 0;
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> pointerTypes;
			std::map<uint32_t, uint32_t> uintConstants;
			std::map<std::vector<uint32_t>, uint32_t> heapAliases;
			// RecordValue's: by function, type, push offset and record offset.
			std::map<std::tuple<opt::Function*, uint32_t, uint32_t, uint32_t>, uint32_t> recordValues;

			static IRContext::Analysis Preserved() { return IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping; }

			void Lower() {
				context()->InvalidateAnalyses(IRContext::kAnalysisTypes | IRContext::kAnalysisConstants);
				CheckModule();
				CollectResources();
				PlanPushData();
				BuildPushBlock();
				for (Resource& resource : resources) {
					if (resource.used) {
						Rewrite(resource);
					}
				}
				Finish();
			}

			// ---- Module queries -------------------------------------------------------------------------------------------

			Instruction* Def(uint32_t a_id) const {
				Instruction* def = get_def_use_mgr()->GetDef(a_id);
				if (!def) {
					Fail("the module references undefined id %", a_id);
				}
				return def;
			}

			static uint32_t InOp(const Instruction* a_inst, uint32_t a_index) { return a_inst->GetSingleWordInOperand(a_index); }

			uint32_t PointeeOf(uint32_t a_pointerType) const {
				const Instruction* type = Def(a_pointerType);
				if (type->opcode() != spv::Op::OpTypePointer) {
					Fail("expected a pointer type, found %", a_pointerType);
				}
				return InOp(type, 1);
			}

			std::string NameOf(uint32_t a_id) const {
				for (Instruction& debug : get_module()->debugs2()) {
					if (debug.opcode() == spv::Op::OpName && InOp(&debug, 0) == a_id) {
						return debug.GetInOperand(1).AsString();
					}
				}
				return Cat("%", a_id);
			}

			std::optional<uint32_t> Decoration(uint32_t a_id, spv::Decoration a_decoration) const {
				for (Instruction& annotation : get_module()->annotations()) {
					if (annotation.opcode() == spv::Op::OpDecorate && InOp(&annotation, 0) == a_id && InOp(&annotation, 1) == uint32_t(a_decoration)) {
						return annotation.NumInOperands() > 2 ? InOp(&annotation, 2) : 0u;
					}
				}
				return std::nullopt;
			}

			std::optional<uint32_t> MemberDecoration(uint32_t a_struct, uint32_t a_member, spv::Decoration a_decoration) const {
				for (Instruction& annotation : get_module()->annotations()) {
					if (annotation.opcode() == spv::Op::OpMemberDecorate && InOp(&annotation, 0) == a_struct && InOp(&annotation, 1) == a_member &&
						InOp(&annotation, 2) == uint32_t(a_decoration)) {
						return annotation.NumInOperands() > 3 ? InOp(&annotation, 3) : 0u;
					}
				}
				return std::nullopt;
			}

			std::optional<uint32_t> ConstantValue(uint32_t a_id) const {
				const Instruction* constant = Def(a_id);
				if (constant->opcode() == spv::Op::OpConstant) {
					return constant->GetInOperand(0).words[0];
				}
				if (constant->opcode() == spv::Op::OpConstantNull) {
					return 0u;
				}
				return std::nullopt;
			}

			std::vector<Instruction*> Users(uint32_t a_id) const {
				std::vector<Instruction*> users;
				get_def_use_mgr()->ForEachUser(a_id, [&](Instruction* a_user) { users.push_back(a_user); });
				return users;
			}

			bool HasRealUses(uint32_t a_id) const {
				bool used = false;
				get_def_use_mgr()->ForEachUser(a_id, [&](Instruction* a_user) { used = used || !IsBookkeeping(a_user); });
				return used;
			}

			void Update(Instruction* a_inst) { get_def_use_mgr()->AnalyzeInstUse(a_inst); }

			// ---- Types, constants, decorations ----------------------------------------------------------------------------

			uint32_t TakeId() {
				const uint32_t id = context()->TakeNextId();
				if (id == 0) {
					Fail("the module ran out of ids");
				}
				return id;
			}

			uint32_t FindType(spv::Op a_op, const std::vector<uint32_t>& a_words) const {
				for (Instruction& inst : get_module()->types_values()) {
					if (inst.opcode() != a_op || inst.NumInOperands() != a_words.size()) {
						continue;
					}
					bool same = true;
					for (uint32_t i = 0; i < a_words.size() && same; ++i) {
						same = InOp(&inst, i) == a_words[i];
					}
					if (same) {
						return inst.result_id();
					}
				}
				return 0;
			}

			uint32_t AddType(spv::Op a_op, opt::Instruction::OperandList a_operands) {
				const uint32_t id = TakeId();
				context()->AddType(std::make_unique<Instruction>(context(), a_op, 0u, id, a_operands));
				return id;
			}

			uint32_t Uint() {
				if (uintType == 0) {
					uintType = FindType(spv::Op::OpTypeInt, { 32, 0 });
					if (uintType == 0) {
						uintType = AddType(spv::Op::OpTypeInt, { { SPV_OPERAND_TYPE_LITERAL_INTEGER, { 32 } }, { SPV_OPERAND_TYPE_LITERAL_INTEGER, { 0 } } });
					}
				}
				return uintType;
			}

			uint32_t Uvec2() {
				if (uvec2Type == 0) {
					const uint32_t component = Uint();
					uvec2Type = FindType(spv::Op::OpTypeVector, { component, 2 });
					if (uvec2Type == 0) {
						uvec2Type = AddType(spv::Op::OpTypeVector, { { SPV_OPERAND_TYPE_ID, { component } }, { SPV_OPERAND_TYPE_LITERAL_INTEGER, { 2 } } });
					}
				}
				return uvec2Type;
			}

			// OpIAddCarry's result: an undecorated { uint, uint } (its own type, so no existing struct's decorations apply).
			uint32_t CarryStruct() {
				if (carryType == 0) {
					const uint32_t component = Uint();
					carryType = AddType(spv::Op::OpTypeStruct, { { SPV_OPERAND_TYPE_ID, { component } }, { SPV_OPERAND_TYPE_ID, { component } } });
				}
				return carryType;
			}

			uint32_t Pointer(uint32_t a_pointee, spv::StorageClass a_storage) {
				if (a_storage == spv::StorageClass::PhysicalStorageBuffer) {
					usesPhysicalPointers = true;
				}
				const auto key = std::make_pair(a_pointee, uint32_t(a_storage));
				if (const auto it = pointerTypes.find(key); it != pointerTypes.end()) {
					return it->second;
				}
				uint32_t id = FindType(spv::Op::OpTypePointer, { uint32_t(a_storage), a_pointee });
				if (id == 0) {
					id = AddType(spv::Op::OpTypePointer, { { SPV_OPERAND_TYPE_STORAGE_CLASS, { uint32_t(a_storage) } }, { SPV_OPERAND_TYPE_ID, { a_pointee } } });
				}
				pointerTypes.emplace(key, id);
				return id;
			}

			// A runtime array of descriptors (no ArrayStride: arrays of descriptors and blocks carry none).
			uint32_t RuntimeArray(uint32_t a_element) {
				for (Instruction& inst : get_module()->types_values()) {
					if (inst.opcode() == spv::Op::OpTypeRuntimeArray && InOp(&inst, 0) == a_element && !Decoration(inst.result_id(), spv::Decoration::ArrayStride)) {
						return inst.result_id();
					}
				}
				return AddType(spv::Op::OpTypeRuntimeArray, { { SPV_OPERAND_TYPE_ID, { a_element } } });
			}

			uint32_t UintConstant(uint32_t a_value) {
				if (const auto it = uintConstants.find(a_value); it != uintConstants.end()) {
					return it->second;
				}
				const uint32_t type = Uint();
				uint32_t id = 0;
				for (Instruction& inst : get_module()->types_values()) {
					if (inst.opcode() == spv::Op::OpConstant && inst.type_id() == type && InOp(&inst, 0) == a_value) {
						id = inst.result_id();
						break;
					}
				}
				if (id == 0) {
					id = TakeId();
					context()->AddGlobalValue(std::make_unique<Instruction>(context(), spv::Op::OpConstant, type, id,
						opt::Instruction::OperandList{ { SPV_OPERAND_TYPE_TYPED_LITERAL_NUMBER, { a_value } } }));
				}
				uintConstants.emplace(a_value, id);
				return id;
			}

			void Decorate(uint32_t a_id, spv::Decoration a_decoration, std::optional<uint32_t> a_value = std::nullopt) {
				opt::Instruction::OperandList operands{ { SPV_OPERAND_TYPE_ID, { a_id } }, { SPV_OPERAND_TYPE_DECORATION, { uint32_t(a_decoration) } } };
				if (a_value) {
					operands.push_back({ SPV_OPERAND_TYPE_LITERAL_INTEGER, { *a_value } });
				}
				context()->AddAnnotationInst(std::make_unique<Instruction>(context(), spv::Op::OpDecorate, 0u, 0u, operands));
			}

			void DecorateMember(uint32_t a_struct, uint32_t a_member, spv::Decoration a_decoration, uint32_t a_value) {
				context()->AddAnnotationInst(std::make_unique<Instruction>(context(), spv::Op::OpMemberDecorate, 0u, 0u,
					opt::Instruction::OperandList{ { SPV_OPERAND_TYPE_ID, { a_struct } }, { SPV_OPERAND_TYPE_LITERAL_INTEGER, { a_member } },
						{ SPV_OPERAND_TYPE_DECORATION, { uint32_t(a_decoration) } }, { SPV_OPERAND_TYPE_LITERAL_INTEGER, { a_value } } }));
			}

			void Name(uint32_t a_id, const std::string& a_name) {
				context()->AddDebug2Inst(std::make_unique<Instruction>(context(), spv::Op::OpName, 0u, 0u,
					opt::Instruction::OperandList{ { SPV_OPERAND_TYPE_ID, { a_id } }, { SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(a_name) } }));
			}

			uint32_t AddVariable(uint32_t a_pointerType, spv::StorageClass a_storage, const std::string& a_name) {
				const uint32_t id = TakeId();
				context()->AddGlobalValue(std::make_unique<Instruction>(context(), spv::Op::OpVariable, a_pointerType, id,
					opt::Instruction::OperandList{ { SPV_OPERAND_TYPE_STORAGE_CLASS, { uint32_t(a_storage) } } }));
				Name(id, a_name);
				// SPIR-V 1.4 and newer list every global the entry point uses; older versions only Input and Output.
				if (get_module()->version() >= 0x00010400u) {
					entry->AddOperand(Operand(SPV_OPERAND_TYPE_ID, { id }));
					Update(entry);
				}
				return id;
			}

			void RemoveFromInterface(uint32_t a_id) {
				for (uint32_t i = entry->NumInOperands(); i-- > 3;) {
					if (InOp(entry, i) == a_id) {
						entry->RemoveInOperand(i);
					}
				}
				Update(entry);
			}

			// ---- Block layout ---------------------------------------------------------------------------------------------

			uint32_t ScalarBytes(uint32_t a_type) const {
				const Instruction* type = Def(a_type);
				if (type->opcode() == spv::Op::OpTypeInt || type->opcode() == spv::Op::OpTypeFloat) {
					return InOp(type, 0) / 8u;
				}
				Fail("a constant buffer member has a non-numeric scalar type (", spvOpcodeString(type->opcode()), ")");
			}

			uint32_t MemberOffset(uint32_t a_struct, uint32_t a_member) const {
				const std::optional<uint32_t> offset = MemberDecoration(a_struct, a_member, spv::Decoration::Offset);
				if (!offset) {
					Fail("member ", a_member, " of block struct '", NameOf(a_struct), "' has no Offset");
				}
				return *offset;
			}

			MatrixLayout MemberLayout(uint32_t a_struct, uint32_t a_member) const {
				return { MemberDecoration(a_struct, a_member, spv::Decoration::MatrixStride).value_or(0u),
					MemberDecoration(a_struct, a_member, spv::Decoration::RowMajor).has_value() };
			}

			uint32_t ArrayStride(uint32_t a_array) const {
				const std::optional<uint32_t> stride = Decoration(a_array, spv::Decoration::ArrayStride);
				if (!stride || *stride == 0) {
					Fail("an array in a constant buffer has no ArrayStride");
				}
				return *stride;
			}

			uint32_t ArrayLength(const Instruction* a_array) const {
				const std::optional<uint32_t> length = ConstantValue(InOp(a_array, 1));
				if (!length) {
					Fail("an array in a constant buffer has a specialization-constant length");
				}
				return *length;
			}

			uint32_t MatrixStrideOf(const MatrixLayout& a_layout) const {
				if (a_layout.stride == 0) {
					Fail("a matrix in a constant buffer has no MatrixStride");
				}
				return a_layout.stride;
			}

			// Bytes from a value's offset to the end of its last byte (struct padding at the end excluded).
			uint32_t Extent(uint32_t a_type, MatrixLayout a_layout = {}) const {
				const Instruction* type = Def(a_type);
				switch (type->opcode()) {
				case spv::Op::OpTypeInt:
				case spv::Op::OpTypeFloat:
					return ScalarBytes(a_type);
				case spv::Op::OpTypeVector:
					return InOp(type, 1) * ScalarBytes(InOp(type, 0));
				case spv::Op::OpTypeMatrix: {
					const Instruction* column = Def(InOp(type, 0));
					const uint32_t columns = InOp(type, 1), rows = InOp(column, 1), component = ScalarBytes(InOp(column, 0));
					const uint32_t stride = MatrixStrideOf(a_layout);
					return a_layout.rowMajor ? (rows - 1) * stride + columns * component : (columns - 1) * stride + rows * component;
				}
				case spv::Op::OpTypeArray: {
					const uint32_t length = ArrayLength(type);
					return length == 0 ? 0 : (length - 1) * ArrayStride(a_type) + Extent(InOp(type, 0), a_layout);
				}
				case spv::Op::OpTypeStruct: {
					uint32_t extent = 0;
					for (uint32_t m = 0; m < type->NumInOperands(); ++m) {
						extent = (std::max)(extent, MemberOffset(a_type, m) + Extent(InOp(type, m), MemberLayout(a_type, m)));
					}
					return extent;
				}
				default:
					Fail("a constant buffer member has a type the lowering cannot lay out (", spvOpcodeString(type->opcode()), ")");
				}
			}

			// The access path to the 32-bit scalar that starts exactly at byte a_offset of a value of a_type; false if no such scalar
			// (padding, or a 16/64-bit member).
			bool DwordPath(uint32_t a_type, uint32_t a_offset, MatrixLayout a_layout, std::vector<uint32_t>& a_path, uint32_t& a_scalar) const {
				const Instruction* type = Def(a_type);
				switch (type->opcode()) {
				case spv::Op::OpTypeInt:
				case spv::Op::OpTypeFloat:
					if (a_offset != 0 || InOp(type, 0) != 32) {
						return false;
					}
					a_scalar = a_type;
					return true;
				case spv::Op::OpTypeVector: {
					const uint32_t component = InOp(type, 0);
					if (ScalarBytes(component) != 4 || a_offset % 4 != 0 || a_offset / 4 >= InOp(type, 1)) {
						return false;
					}
					a_path.push_back(a_offset / 4);
					a_scalar = component;
					return true;
				}
				case spv::Op::OpTypeMatrix: {
					const Instruction* column = Def(InOp(type, 0));
					const uint32_t columns = InOp(type, 1), rows = InOp(column, 1), component = InOp(column, 0);
					const uint32_t stride = MatrixStrideOf(a_layout);
					if (ScalarBytes(component) != 4) {
						return false;
					}
					const uint32_t major = a_offset / stride, within = a_offset % stride;
					if (within % 4 != 0) {
						return false;
					}
					// Row-major: MatrixStride steps rows and a column's components are rows; column-major the opposite.
					const uint32_t c = a_layout.rowMajor ? within / 4 : major, r = a_layout.rowMajor ? major : within / 4;
					if (c >= columns || r >= rows) {
						return false;
					}
					a_path.push_back(c);
					a_path.push_back(r);
					a_scalar = component;
					return true;
				}
				case spv::Op::OpTypeArray: {
					const uint32_t stride = ArrayStride(a_type), element = a_offset / stride;
					if (element >= ArrayLength(type)) {
						return false;
					}
					a_path.push_back(element);
					return DwordPath(InOp(type, 0), a_offset % stride, a_layout, a_path, a_scalar);
				}
				case spv::Op::OpTypeStruct:
					for (uint32_t m = 0; m < type->NumInOperands(); ++m) {
						const uint32_t offset = MemberOffset(a_type, m);
						const MatrixLayout layout = MemberLayout(a_type, m);
						if (a_offset >= offset && a_offset < offset + Extent(InOp(type, m), layout)) {
							a_path.push_back(m);
							return DwordPath(InOp(type, m), a_offset - offset, layout, a_path, a_scalar);
						}
					}
					return false;
				default:
					return false;
				}
			}

			// True if no top-level member of the struct overlaps bytes [a_offset, a_offset + 4).
			bool TopLevelPadding(uint32_t a_struct, uint32_t a_offset) const {
				const Instruction* type = Def(a_struct);
				for (uint32_t m = 0; m < type->NumInOperands(); ++m) {
					const uint32_t offset = MemberOffset(a_struct, m);
					if (offset < a_offset + 4 && offset + Extent(InOp(type, m), MemberLayout(a_struct, m)) > a_offset) {
						return false;
					}
				}
				return true;
			}

			// ---- Module checks and resource collection --------------------------------------------------------------------

			void CheckModule() {
				uint32_t entryPoints = 0;
				for (Instruction& point : get_module()->entry_points()) {
					entry = &point;
					++entryPoints;
				}
				if (entryPoints != 1) {
					Fail("the module has ", entryPoints, " entry points; the lowering takes one stage's module with exactly one");
				}
				if (const std::optional<spv::ExecutionModel> model = ExecutionModelFor(stage); model && InOp(entry, 0) != uint32_t(*model)) {
					Fail("the module's entry point '", entry->GetInOperand(2).AsString(), "' is not for the stage it is lowered for (VkShaderStageFlagBits ",
						uint32_t(stage), ")");
				}
				const auto checkAlignment = [](const char* a_name, uint32_t a_alignment) {
					if ((a_alignment & (a_alignment - 1)) != 0 || a_alignment < 4) {
						Fail("LoweringTarget::", a_name, " (", a_alignment, ") must be a power of two of at least 4");
					}
				};
				checkAlignment("recordAlignment", target.recordAlignment);
				checkAlignment("uniformBlockAlignment", target.uniformBlockAlignment);
				checkAlignment("spillAlignment", target.spillAlignment);
				if (target.pushedBytes % 4 != 0 || target.pushedBytes > target.pushConstantBytes) {
					Fail("LoweringTarget::pushedBytes (", target.pushedBytes, ") must be a multiple of 4 within the ", target.pushConstantBytes,
						" push constant bytes");
				}
				if (target.spillAddressOffset != kNoSpill &&
					(target.spillAddressOffset % 4 != 0 || target.spillAddressOffset + 8 > target.pushConstantBytes)) {
					Fail("LoweringTarget::spillAddressOffset (", target.spillAddressOffset, ") must be dword-aligned with 8 bytes within the ",
						target.pushConstantBytes, " push constant bytes");
				}
			}

			// The declared-type class of a resource variable (VK_EXT_descriptor_heap's VkSpirvResourceTypeFlagBitsEXT rules).
			VkFlags Classify(const Resource& a_resource) const {
				const Instruction* element = Def(a_resource.element);
				const uint32_t var = a_resource.var->result_id();
				switch (element->opcode()) {
				case spv::Op::OpTypeSampler:
					return VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
				case spv::Op::OpTypeSampledImage:
					return VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
				case spv::Op::OpTypeImage:
					// Sampled 2: a storage image, read-only when the variable is NonWritable. Sampled 1 (and 0): a sampled image.
					if (InOp(element, 5) == 2) {
						return Decoration(var, spv::Decoration::NonWritable) ? VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT : VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT;
					}
					return VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
				case spv::Op::OpTypeAccelerationStructureKHR:
					return VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
				case spv::Op::OpTypeStruct: {
					const bool block = Decoration(a_resource.element, spv::Decoration::Block).has_value();
					const bool bufferBlock = Decoration(a_resource.element, spv::Decoration::BufferBlock).has_value();
					if (a_resource.storage == spv::StorageClass::Uniform && block) {
						return VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
					}
					if ((a_resource.storage == spv::StorageClass::StorageBuffer && block) || (a_resource.storage == spv::StorageClass::Uniform && bufferBlock)) {
						// NonWritable on the variable, or on every member (DXC marks read-only buffers' members).
						bool readOnly = Decoration(var, spv::Decoration::NonWritable).has_value();
						if (!readOnly) {
							readOnly = element->NumInOperands() > 0;
							for (uint32_t m = 0; m < element->NumInOperands() && readOnly; ++m) {
								readOnly = MemberDecoration(a_resource.element, m, spv::Decoration::NonWritable).has_value();
							}
						}
						return readOnly ? VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT : VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT;
					}
					return 0;
				}
				default:
					return 0;
				}
			}

			void CollectResources() {
				std::vector<Instruction*> variables;
				for (Instruction& inst : get_module()->types_values()) {
					if (inst.opcode() != spv::Op::OpVariable) {
						continue;
					}
					switch (spv::StorageClass(InOp(&inst, 0))) {
					case spv::StorageClass::UniformConstant:
					case spv::StorageClass::Uniform:
					case spv::StorageClass::StorageBuffer:
						variables.push_back(&inst);
						break;
					case spv::StorageClass::PushConstant:
						if (HasRealUses(inst.result_id())) {
							Fail("'", NameOf(inst.result_id()), "' is a push constant block ([[vk::push_constant]]); BasicRHI shaders read push data through "
								"cbuffers the pipeline layout maps (PushConstantRangeDesc)");
						}
						variables.push_back(&inst);
						break;
					default:
						break;
					}
				}

				resources.reserve(variables.size());
				for (Instruction* var : variables) {
					Resource resource;
					resource.var = var;
					resource.storage = spv::StorageClass(InOp(var, 0));
					resource.used = HasRealUses(var->result_id());
					resource.pointee = PointeeOf(var->type_id());
					resource.element = resource.pointee;
					const Instruction* pointee = Def(resource.pointee);
					if (pointee->opcode() == spv::Op::OpTypeArray || pointee->opcode() == spv::Op::OpTypeRuntimeArray) {
						resource.element = InOp(pointee, 0);
						resource.arrayed = true;
					}
					const std::string name = NameOf(var->result_id());
					if (!resource.used) {
						resources.push_back(std::move(resource));
						continue;  // removed: an unused declaration needs no mapping
					}
					const std::optional<uint32_t> set = Decoration(var->result_id(), spv::Decoration::DescriptorSet);
					const std::optional<uint32_t> binding = Decoration(var->result_id(), spv::Decoration::Binding);
					if (!set || !binding) {
						Fail("resource variable '", name, "' has no DescriptorSet/Binding decoration");
					}
					resource.set = *set;
					resource.binding = *binding;
					resource.kind = Classify(resource);
					resource.description = Cat("'", name, "' (set ", resource.set, ", binding ", resource.binding, ", ", KindName(resource.kind), ")");
					if (resource.kind == 0) {
						Fail("resource variable '", name, "' (set ", resource.set, ", binding ", resource.binding, ") has a type the lowering does not support (",
							spvOpcodeString(Def(resource.element)->opcode()), ")");
					}
					if (resource.kind == VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT) {
						Fail(resource.description, ": combined image samplers are not supported by the descriptor-buffer binding model (the sampler and "
							"resource heaps are separate); declare a separate Texture and SamplerState");
					}
					if (Def(resource.element)->opcode() == spv::Op::OpTypeArray || Def(resource.element)->opcode() == spv::Op::OpTypeRuntimeArray) {
						Fail(resource.description, ": arrays of arrays of descriptors are not supported");
					}
					for (const BindingMapping& mapping : plan.mappings) {
						if (mapping.set == resource.set && resource.binding >= mapping.firstBinding && resource.binding - mapping.firstBinding < mapping.bindingCount &&
							(mapping.resourceMask & resource.kind) != 0) {
							resource.mapping = &mapping;
							resource.index = resource.binding - mapping.firstBinding;
							break;
						}
					}
					if (!resource.mapping) {
						Fail(resource.description, " is not mapped by the pipeline layout (no binding plan mapping covers its set, binding and type)");
					}
					switch (resource.mapping->source) {
					case BindingSource::BufferIndirectAddress:
					case BindingSource::BufferPushAddress:
					case BindingSource::PushData:
						if (resource.kind != VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT || resource.arrayed) {
							Fail(resource.description, ": a ", SourceName(resource.mapping->source), " mapping needs a single (non-arrayed) uniform block");
						}
						break;
					case BindingSource::StaticSampler:
						if (resource.kind != VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT) {
							Fail(resource.description, ": a static sampler mapping needs a sampler");
						}
						if (resource.mapping->staticSampler >= target.staticSamplerElements.size()) {
							Fail(resource.description, ": static sampler ", resource.mapping->staticSampler, " has no sampler heap element (LoweringTarget::staticSamplerElements has ",
								target.staticSamplerElements.size(), ")");
						}
						break;
					default:
						break;
					}
					resources.push_back(std::move(resource));
				}
			}

			// ---- Push data ------------------------------------------------------------------------------------------------

			void NeedDword(uint32_t a_offset) {
				if (a_offset % 4 != 0 || a_offset + 4 > target.pushConstantBytes) {
					Fail("push constant dword at byte ", a_offset, " is not dword-aligned within the ", target.pushConstantBytes, " push constant bytes");
				}
				neededDwords.insert(a_offset);
			}

			void NeedSpillAddress(const Resource& a_resource, uint32_t a_offset) {
				if (target.spillAddressOffset == kNoSpill) {
					Fail(a_resource.description, " reads push data at byte ", a_offset, ", past the ", target.pushedBytes,
						" pushed bytes, but the target spills nothing (LoweringTarget::spillAddressOffset is kNoSpill)");
				}
				NeedDword(target.spillAddressOffset);
				NeedDword(target.spillAddressOffset + 4);
			}

			// The planning half of ReadPushAddress: the push constant dwords it reads.
			void NeedPushAddress(const Resource& a_resource, uint32_t a_offset) {
				if (a_offset + 8 > plan.pushDataBytes) {
					Fail(a_resource.description, " reads a push data address at byte ", a_offset, ", past the layout's ", plan.pushDataBytes, " push data bytes");
				}
				if (a_offset + 8 <= target.pushedBytes) {
					NeedDword(a_offset);
					NeedDword(a_offset + 4);
				}
				else if (a_offset >= target.pushedBytes) {
					NeedSpillAddress(a_resource, a_offset);
				}
				else {
					Fail(a_resource.description, " reads a push data address at byte ", a_offset, " that straddles the pushed prefix (", target.pushedBytes, " bytes)");
				}
			}

			// Which push block members the stage needs: a clone per pushed root-constant cbuffer, the address dwords the lowering reads.
			void PlanPushData() {
				for (Resource& resource : resources) {
					if (!resource.used) {
						continue;
					}
					const BindingMapping& mapping = *resource.mapping;
					switch (mapping.source) {
					case BindingSource::HeapIndirect:
					case BindingSource::BufferIndirectAddress:
						NeedPushAddress(resource, mapping.pushOffset);
						break;
					case BindingSource::BufferPushAddress:
						NeedPushAddress(resource, mapping.pushOffset + 8 * resource.index);
						break;
					case BindingSource::PushData: {
						const uint32_t extent = Extent(resource.pointee);
						// Heap mode would read the neighbouring push data bytes past the range; refuse instead of reading stale data.
						if (extent > mapping.pushDataBytes) {
							Fail(resource.description, " declares ", extent, " bytes, more than its root-constant range's ", mapping.pushDataBytes, " bytes");
						}
						if (mapping.pushOffset % 4 != 0) {
							Fail(resource.description, ": push data offset ", mapping.pushOffset, " is not dword-aligned");
						}
						if (mapping.pushOffset + extent <= target.pushedBytes) {
							resource.clone = int(clones.size());
							clones.push_back(Clone{ .resource = &resource, .offset = mapping.pushOffset, .extent = extent });
						}
						else if (mapping.pushOffset >= target.pushedBytes) {
							NeedSpillAddress(resource, mapping.pushOffset);
						}
						else {
							Fail(resource.description, ": its ", extent, " bytes at push data offset ", mapping.pushOffset, " straddle the pushed prefix (",
								target.pushedBytes, " bytes)");
						}
					} break;
					default:
						break;
					}
				}

				// Overlapping members are invalid: a dword inside a pushed cbuffer is read through the cbuffer's clone (the 32-bit
				// scalar there, or a uint member added to the clone in its top-level padding), never as a second member.
				for (const uint32_t offset : neededDwords) {
					DwordSource source;
					for (uint32_t c = 0; c < clones.size(); ++c) {
						Clone& clone = clones[c];
						if (offset < clone.offset || offset >= clone.offset + clone.extent) {
							continue;
						}
						const uint32_t within = offset - clone.offset;
						source.clone = int(c);
						if (!DwordPath(clone.resource->pointee, within, {}, source.path, source.scalar)) {
							source.path.clear();
							if (!TopLevelPadding(clone.resource->pointee, within)) {
								Fail("push data bytes [", offset, ", ", offset + 4, ") hold half of an address the layout reads, but lie inside ", clone.resource->description,
									" in a member that is not a 32-bit scalar (nor top-level padding)");
							}
							source.padding = int(clone.padding.size());
							source.scalar = Uint();
							clone.padding.push_back(within);
						}
						break;
					}
					if (source.clone < 0) {
						source.scalar = Uint();
					}
					dwords.emplace(offset, std::move(source));
				}
			}

			// Clones a block struct for the push block: the same members (and member decorations) without Block, plus uint padding
			// members.
			uint32_t CloneStruct(const Clone& a_clone) {
				const uint32_t original = a_clone.resource->pointee;
				const Instruction* type = Def(original);
				const uint32_t members = type->NumInOperands();
				opt::Instruction::OperandList operands;
				for (uint32_t m = 0; m < members; ++m) {
					operands.push_back({ SPV_OPERAND_TYPE_ID, { InOp(type, m) } });
				}
				for (size_t p = 0; p < a_clone.padding.size(); ++p) {
					operands.push_back({ SPV_OPERAND_TYPE_ID, { Uint() } });
				}
				const uint32_t clone = AddType(spv::Op::OpTypeStruct, operands);

				std::vector<Instruction*> decorations;
				for (Instruction& annotation : get_module()->annotations()) {
					if ((annotation.opcode() == spv::Op::OpMemberDecorate || annotation.opcode() == spv::Op::OpDecorate) && InOp(&annotation, 0) == original) {
						decorations.push_back(&annotation);
					}
				}
				for (const Instruction* decoration : decorations) {
					if (decoration->opcode() == spv::Op::OpDecorate) {
						const spv::Decoration kind = spv::Decoration(InOp(decoration, 1));
						if (kind == spv::Decoration::Block || kind == spv::Decoration::BufferBlock) {
							continue;
						}
					}
					std::unique_ptr<Instruction> copy(decoration->Clone(context()));
					copy->SetInOperand(0, { clone });
					context()->AddAnnotationInst(std::move(copy));
				}
				for (uint32_t p = 0; p < a_clone.padding.size(); ++p) {
					DecorateMember(clone, members + p, spv::Decoration::Offset, a_clone.padding[p]);
				}
				Name(clone, Cat(NameOf(original), ".push"));
				return clone;
			}

			// The one push constant block: the pushed root-constant cbuffers' clones at their push data offsets and a uint member per
			// address dword no cbuffer covers (the spill address among them). Scalar block layout makes dword offsets valid.
			void BuildPushBlock() {
				struct Entry {
					uint32_t offset = 0, extent = 0, type = 0;
					int clone = -1;
					uint32_t dword = 0;
				};
				std::vector<Entry> entries;
				for (uint32_t c = 0; c < clones.size(); ++c) {
					clones[c].type = CloneStruct(clones[c]);
					entries.push_back(Entry{ .offset = clones[c].offset, .extent = clones[c].extent, .type = clones[c].type, .clone = int(c) });
				}
				for (const auto& [offset, source] : dwords) {
					if (source.clone < 0) {
						entries.push_back(Entry{ .offset = offset, .extent = 4, .type = Uint(), .dword = offset });
					}
				}
				if (entries.empty()) {
					return;
				}
				std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.offset < b.offset; });

				opt::Instruction::OperandList members;
				for (uint32_t k = 0; k < entries.size(); ++k) {
					const Entry& item = entries[k];
					if (k + 1 < entries.size() && item.offset + item.extent > entries[k + 1].offset) {
						Fail("push data members overlap: bytes [", item.offset, ", ", item.offset + item.extent, ") and the member at byte ", entries[k + 1].offset,
							" (a root-constant cbuffer larger than its range?)");
					}
					if (item.offset + item.extent > target.pushConstantBytes) {
						Fail("push data bytes [", item.offset, ", ", item.offset + item.extent, ") lie past the ", target.pushConstantBytes, " push constant bytes");
					}
					members.push_back({ SPV_OPERAND_TYPE_ID, { item.type } });
					if (item.clone >= 0) {
						clones[item.clone].member = k;
					}
					else {
						dwords.at(item.dword).member = k;
					}
				}
				for (auto& [offset, source] : dwords) {
					if (source.clone >= 0) {
						const Clone& clone = clones[source.clone];
						source.member = clone.member;
						if (source.padding >= 0) {
							source.path = { Def(clone.resource->pointee)->NumInOperands() + uint32_t(source.padding) };
						}
					}
				}

				const uint32_t block = AddType(spv::Op::OpTypeStruct, members);
				Decorate(block, spv::Decoration::Block);
				for (uint32_t k = 0; k < entries.size(); ++k) {
					DecorateMember(block, k, spv::Decoration::Offset, entries[k].offset);
				}
				Name(block, "BasicRHIPushData");
				pushVar = AddVariable(Pointer(block, spv::StorageClass::PushConstant), spv::StorageClass::PushConstant, "BasicRHIPushConstants");
			}

			// ---- Code emission helpers (instructions go before a_where) ----------------------------------------------------

			opt::InstructionBuilder Builder(Instruction* a_where) {
				if (a_where->opcode() == spv::Op::OpPhi) {
					Fail("the lowering cannot insert code before an OpPhi");
				}
				return opt::InstructionBuilder(context(), a_where, Preserved());
			}

			// A push constant dword as a uint.
			uint32_t ReadPushDword(Instruction* a_where, uint32_t a_offset) {
				const DwordSource& source = dwords.at(a_offset);
				std::vector<uint32_t> indices{ UintConstant(source.member) };
				for (const uint32_t index : source.path) {
					indices.push_back(UintConstant(index));
				}
				opt::InstructionBuilder builder = Builder(a_where);
				const uint32_t pointer = builder.AddAccessChain(Pointer(source.scalar, spv::StorageClass::PushConstant), pushVar, indices)->result_id();
				uint32_t value = builder.AddLoad(source.scalar, pointer)->result_id();
				if (source.scalar != Uint()) {
					value = builder.AddUnaryOp(Uint(), spv::Op::OpBitcast, value)->result_id();
				}
				return value;
			}

			// a_address (uvec2) + a_offset, with the carry into the high word.
			uint32_t AddressPlus(Instruction* a_where, uint32_t a_address, uint32_t a_offset) {
				if (a_offset == 0) {
					return a_address;
				}
				opt::InstructionBuilder builder = Builder(a_where);
				const uint32_t low = builder.AddCompositeExtract(Uint(), a_address, { 0 })->result_id();
				const uint32_t high = builder.AddCompositeExtract(Uint(), a_address, { 1 })->result_id();
				const uint32_t sum = builder.AddBinaryOp(CarryStruct(), spv::Op::OpIAddCarry, low, UintConstant(a_offset))->result_id();
				const uint32_t sumLow = builder.AddCompositeExtract(Uint(), sum, { 0 })->result_id();
				const uint32_t carry = builder.AddCompositeExtract(Uint(), sum, { 1 })->result_id();
				const uint32_t sumHigh = builder.AddIAdd(Uint(), high, carry)->result_id();
				return builder.AddCompositeConstruct(Uvec2(), { sumLow, sumHigh })->result_id();
			}

			uint32_t PhysicalPointer(Instruction* a_where, uint32_t a_pointee, uint32_t a_address) {
				return Builder(a_where).AddUnaryOp(Pointer(a_pointee, spv::StorageClass::PhysicalStorageBuffer), spv::Op::OpBitcast, a_address)->result_id();
			}

			uint32_t LoadPhysical(Instruction* a_where, uint32_t a_type, uint32_t a_address, uint32_t a_alignment) {
				const uint32_t pointer = PhysicalPointer(a_where, a_type, a_address);
				return Builder(a_where).AddLoad(a_type, pointer, a_alignment)->result_id();
			}

			// Where a value read once per invocation goes: a_where's function's entry block, past its variables. Every use in the
			// function is dominated by it (after InlineExhaustive, every use is in the entry point).
			Instruction* EntryInsertionPoint(Instruction* a_where) {
				opt::BasicBlock& entry = *context()->get_instr_block(a_where)->GetParent()->begin();
				auto it = entry.begin();
				while (it != entry.end() && (it->opcode() == spv::Op::OpVariable || it->opcode() == spv::Op::OpLine || it->opcode() == spv::Op::OpNoLine)) {
					++it;
				}
				return &*it;
			}

			// A record's a_type value (uint: a heap index; uvec2: a block's address) at the record address in push data at a_pushOffset
			// plus a_offset, loaded once per function in its entry block. The record is the frame's, the same for every invocation of a
			// draw: per use, the load and its address arithmetic were repeated at each descriptor and block access (hundreds in a
			// Lighting pixel stage), which the driver cannot merge (the pointer's memory may be written, for all it knows).
			uint32_t RecordValue(Instruction* a_where, uint32_t a_type, uint32_t a_pushOffset, uint32_t a_offset) {
				opt::Function* function = context()->get_instr_block(a_where)->GetParent();
				const auto key = std::tuple{ function, a_type, a_pushOffset, a_offset };
				if (const auto found = recordValues.find(key); found != recordValues.end()) {
					return found->second;
				}
				Instruction* at = EntryInsertionPoint(a_where);
				const uint32_t address = AddressPlus(at, ReadPushAddress(at, a_pushOffset), a_offset);
				const uint32_t value = LoadPhysical(at, a_type, address, AlignmentAt(target.recordAlignment, a_offset));
				recordValues.emplace(key, value);
				return value;
			}

			uint32_t ReadSpillAddress(Instruction* a_where) {
				const uint32_t low = ReadPushDword(a_where, target.spillAddressOffset);
				const uint32_t high = ReadPushDword(a_where, target.spillAddressOffset + 4);
				return Builder(a_where).AddCompositeConstruct(Uvec2(), { low, high })->result_id();
			}

			// The uint64 (as uvec2) at push data image byte a_offset: push constants in the pushed prefix, else read from the spill
			// image (NeedPushAddress planned the dwords).
			uint32_t ReadPushAddress(Instruction* a_where, uint32_t a_offset) {
				if (a_offset + 8 <= target.pushedBytes) {
					const uint32_t low = ReadPushDword(a_where, a_offset);
					const uint32_t high = ReadPushDword(a_where, a_offset + 4);
					return Builder(a_where).AddCompositeConstruct(Uvec2(), { low, high })->result_id();
				}
				const uint32_t spilled = a_offset - target.pushedBytes;
				const uint32_t address = AddressPlus(a_where, ReadSpillAddress(a_where), spilled);
				return LoadPhysical(a_where, Uvec2(), address, AlignmentAt(target.spillAlignment, spilled));
			}

			// ---- Descriptors ----------------------------------------------------------------------------------------------

			void Redecorate(Resource& a_resource, uint32_t a_set, uint32_t a_binding) {
				for (Instruction& annotation : get_module()->annotations()) {
					if (annotation.opcode() != spv::Op::OpDecorate || InOp(&annotation, 0) != a_resource.var->result_id()) {
						continue;
					}
					if (InOp(&annotation, 1) == uint32_t(spv::Decoration::DescriptorSet)) {
						annotation.SetInOperand(2, { a_set });
					}
					else if (InOp(&annotation, 1) == uint32_t(spv::Decoration::Binding)) {
						annotation.SetInOperand(2, { a_binding });
					}
				}
				a_resource.keep = true;
			}

			// The typed alias of a heap binding for a descriptor type: a runtime array variable at the heap binding, with the variable's
			// own decorations (NonWritable, Coherent, ...) other than its set and binding.
			uint32_t HeapAlias(const Resource& a_resource, bool a_sampler) {
				const uint32_t set = a_sampler ? target.samplerSet : target.resourceSet;
				const uint32_t binding = a_sampler ? target.samplerBinding : target.resourceBinding;
				std::vector<uint32_t> key{ a_resource.element, uint32_t(a_resource.storage), set, binding };
				std::vector<Instruction*> decorations;
				for (Instruction& annotation : get_module()->annotations()) {
					if (annotation.opcode() != spv::Op::OpDecorate || InOp(&annotation, 0) != a_resource.var->result_id()) {
						continue;
					}
					const spv::Decoration kind = spv::Decoration(InOp(&annotation, 1));
					if (kind == spv::Decoration::DescriptorSet || kind == spv::Decoration::Binding) {
						continue;
					}
					decorations.push_back(&annotation);
					key.push_back(~0u);
					for (uint32_t i = 1; i < annotation.NumInOperands(); ++i) {
						key.push_back(InOp(&annotation, i));
					}
				}
				if (const auto it = heapAliases.find(key); it != heapAliases.end()) {
					return it->second;
				}
				const uint32_t array = RuntimeArray(a_resource.element);
				const uint32_t alias = AddVariable(Pointer(array, a_resource.storage), a_resource.storage, a_sampler ? "BasicRHISamplerHeap" : "BasicRHIResourceHeap");
				Decorate(alias, spv::Decoration::DescriptorSet, set);
				Decorate(alias, spv::Decoration::Binding, binding);
				for (const Instruction* decoration : decorations) {
					std::unique_ptr<Instruction> copy(decoration->Clone(context()));
					copy->SetInOperand(0, { alias });
					context()->AddAnnotationInst(std::move(copy));
				}
				heapAliases.emplace(std::move(key), alias);
				usesHeapAliases = true;
				return alias;
			}

			// The heap element of the variable's element 0, as a uint id computed before a_where.
			uint32_t HeapElement(Instruction* a_where, const Resource& a_resource) {
				const BindingMapping& mapping = *a_resource.mapping;
				switch (mapping.source) {
				case BindingSource::Heap:
					return UintConstant(mapping.heapElement + a_resource.index);
				case BindingSource::StaticSampler:
					return UintConstant(target.staticSamplerElements[mapping.staticSampler] + a_resource.index);
				case BindingSource::HeapIndirect:
					// The record base is recordAlignment-aligned; its index entries at least 4 (recordOffset + 4i).
					return RecordValue(a_where, Uint(), mapping.pushOffset, mapping.recordOffset + 4 * a_resource.index);
				default:
					Fail(a_resource.description, ": not a descriptor mapping");
				}
			}

			void CheckDescriptorUse(const Resource& a_resource, const Instruction* a_user) const {
				switch (a_user->opcode()) {
				case spv::Op::OpPhi:
				case spv::Op::OpSelect:
					Fail(a_resource.description, " reaches ", spvOpcodeString(a_user->opcode()), " directly: the lowering recomputes descriptor pointers at each use "
						"and cannot follow one through control flow");
				case spv::Op::OpFunctionCall:
					Fail(a_resource.description, " is passed to a function the inliner kept; the lowering needs every use inlined");
				case spv::Op::OpExtInst:
					Fail(a_resource.description, " is referenced by an extended instruction (debug info?); the lowering cannot rewrite it");
				default:
					break;
				}
			}

			// Every use of the variable becomes an access chain into the heap alias at (element + the variable's own index).
			void RewriteDescriptor(Resource& a_resource) {
				const bool sampler = a_resource.kind == VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
				const uint32_t alias = HeapAlias(a_resource, sampler);
				const uint32_t var = a_resource.var->result_id();
				for (Instruction* user : Users(var)) {
					if (IsBookkeeping(user)) {
						continue;
					}
					CheckDescriptorUse(a_resource, user);
					if (a_resource.arrayed) {
						if (!IsAccessChain(user->opcode()) || InOp(user, 0) != var || user->NumInOperands() < 2) {
							Fail(a_resource.description, ": the descriptor array is used by ", spvOpcodeString(user->opcode()), "; only access chains into it can be lowered");
						}
						const uint32_t element = HeapElement(user, a_resource);
						const uint32_t arrayIndex = InOp(user, 1);
						uint32_t index = arrayIndex;
						if (ConstantValue(element).value_or(1u) != 0) {
							index = Builder(user).AddIAdd(Uint(), element, arrayIndex)->result_id();
							if (Decoration(arrayIndex, spv::Decoration::NonUniform)) {
								Decorate(index, spv::Decoration::NonUniform);
							}
						}
						user->SetInOperand(0, { alias });
						user->SetInOperand(1, { index });
					}
					else {
						const uint32_t element = HeapElement(user, a_resource);
						const uint32_t pointer = Builder(user).AddAccessChain(Pointer(a_resource.element, a_resource.storage), alias, { element })->result_id();
						user->ForEachInId([&](uint32_t* a_id) {
							if (*a_id == var) {
								*a_id = pointer;
							}
						});
					}
					Update(user);
				}
			}

			// ---- Blocks: push constants and PhysicalStorageBuffer pointers -------------------------------------------------

			void Step(Walk& a_walk, std::optional<uint32_t> a_index, uint32_t a_stride) const {
				if (a_index) {
					a_walk.offset += *a_index * a_stride;
				}
				else {
					a_walk.align = (std::min)(a_walk.align, LowBit(a_stride));
				}
			}

			// Advances a walk through an access chain's indices from in-operand a_first, starting at type a_type.
			void WalkIndices(const Instruction* a_chain, uint32_t a_first, uint32_t a_type, Walk& a_walk) const {
				for (uint32_t i = a_first; i < a_chain->NumInOperands(); ++i) {
					const std::optional<uint32_t> index = ConstantValue(InOp(a_chain, i));
					const Instruction* type = Def(a_type);
					switch (type->opcode()) {
					case spv::Op::OpTypeStruct:
						if (!index) {
							Fail("a struct is indexed with a non-constant index");
						}
						a_walk.offset += MemberOffset(a_type, *index);
						a_walk.layout = MemberLayout(a_type, *index);
						a_walk.componentStride = 0;
						a_type = InOp(type, *index);
						break;
					case spv::Op::OpTypeArray:
					case spv::Op::OpTypeRuntimeArray:
						Step(a_walk, index, ArrayStride(a_type));
						a_type = InOp(type, 0);
						break;
					case spv::Op::OpTypeMatrix: {
						const uint32_t column = InOp(type, 0);
						const uint32_t component = ScalarBytes(InOp(Def(column), 0));
						const uint32_t stride = MatrixStrideOf(a_walk.layout);
						Step(a_walk, index, a_walk.layout.rowMajor ? component : stride);
						a_walk.componentStride = a_walk.layout.rowMajor ? stride : component;
						a_type = column;
					} break;
					case spv::Op::OpTypeVector: {
						const uint32_t component = ScalarBytes(InOp(type, 0));
						Step(a_walk, index, a_walk.componentStride != 0 ? a_walk.componentStride : component);
						a_type = InOp(type, 0);
					} break;
					default:
						Fail("an access chain indexes into a ", spvOpcodeString(type->opcode()));
					}
				}
			}

			void SetAligned(Instruction* a_load, uint32_t a_alignment) {
				const uint32_t aligned = uint32_t(spv::MemoryAccessMask::Aligned);
				if (a_load->NumInOperands() < 2) {
					a_load->AddOperand(Operand(SPV_OPERAND_TYPE_MEMORY_ACCESS, { aligned }));
					a_load->AddOperand(Operand(SPV_OPERAND_TYPE_LITERAL_INTEGER, { a_alignment }));
				}
				else if ((InOp(a_load, 1) & aligned) != 0) {
					a_load->SetInOperand(2, { a_alignment });
				}
				else {
					a_load->SetInOperand(1, { InOp(a_load, 1) | aligned });
					// Aligned is the lowest mask bit with an operand: its literal comes first.
					a_load->InsertOperand((a_load->NumOperands() - a_load->NumInOperands()) + 2, Operand(SPV_OPERAND_TYPE_LITERAL_INTEGER, { a_alignment }));
				}
				Update(a_load);
			}

			// A load whose pointee was remapped to a clone: it loads the clone, and the original struct is rebuilt from its members for
			// the users (not OpCopyLogical: padding members make the clone a different logical type).
			void LoadAsOriginal(Instruction* a_load, uint32_t a_clone) {
				const uint32_t original = a_load->type_id();
				const Instruction* originalType = Def(original);
				a_load->SetResultType(a_clone);
				Update(a_load);
				const uint32_t loaded = a_load->result_id();
				opt::InstructionBuilder builder(context(), a_load->NextNode(), Preserved());
				std::vector<uint32_t> members;
				for (uint32_t m = 0; m < originalType->NumInOperands(); ++m) {
					members.push_back(builder.AddCompositeExtract(InOp(originalType, m), loaded, { m })->result_id());
				}
				Instruction* rebuilt = builder.AddCompositeConstruct(original, members);
				context()->ReplaceAllUsesWithPredicate(loaded, rebuilt->result_id(), [&](Instruction* a_user) {
					return a_user->opcode() != spv::Op::OpCompositeExtract || std::find(members.begin(), members.end(), a_user->result_id()) == members.end();
				});
			}

			void FinishLoad(Instruction* a_load, uint32_t a_pointee, const Walk& a_walk, const Retarget& a_retarget) {
				if (a_retarget.storage == spv::StorageClass::PhysicalStorageBuffer) {
					SetAligned(a_load, AlignmentAt(a_walk.align, a_walk.offset));
				}
				if (a_retarget.Remap(a_pointee) != a_pointee) {
					LoadAsOriginal(a_load, a_retarget.Remap(a_pointee));
				}
			}

			// Retypes the users of a pointer whose pointee is a_pointee (original type) and that now lives in a_retarget.storage.
			void FollowPointer(const Resource& a_resource, Instruction* a_pointer, uint32_t a_pointee, const Walk& a_walk, const Retarget& a_retarget) {
				const uint32_t id = a_pointer->result_id();
				for (Instruction* user : Users(id)) {
					if (IsBookkeeping(user)) {
						continue;
					}
					switch (user->opcode()) {
					case spv::Op::OpLoad:
						FinishLoad(user, a_pointee, a_walk, a_retarget);
						break;
					case spv::Op::OpAccessChain:
					case spv::Op::OpInBoundsAccessChain:
						if (InOp(user, 0) != id) {
							Fail(a_resource.description, ": a pointer into it is used as an access chain index");
						}
						RetypeChain(a_resource, user, 1, a_pointee, a_walk, a_retarget);
						break;
					case spv::Op::OpCopyObject:
						user->SetResultType(Pointer(a_retarget.Remap(a_pointee), a_retarget.storage));
						Update(user);
						FollowPointer(a_resource, user, a_pointee, a_walk, a_retarget);
						break;
					case spv::Op::OpPhi:
					case spv::Op::OpSelect:
						Fail(a_resource.description, ": a pointer into it reaches ", spvOpcodeString(user->opcode()), "; the lowering recomputes pointers at each use "
							"and cannot follow one through control flow");
					case spv::Op::OpFunctionCall:
						Fail(a_resource.description, ": a pointer into it is passed to a function the inliner kept");
					default:
						Fail(a_resource.description, ": a pointer into it is used by ", spvOpcodeString(user->opcode()), ", which the lowering cannot rewrite");
					}
				}
			}

			void RetypeChain(const Resource& a_resource, Instruction* a_chain, uint32_t a_first, uint32_t a_basePointee, Walk a_walk, const Retarget& a_retarget) {
				if (a_retarget.storage == spv::StorageClass::PhysicalStorageBuffer) {
					WalkIndices(a_chain, a_first, a_basePointee, a_walk);
				}
				const uint32_t pointee = PointeeOf(a_chain->type_id());
				a_chain->SetResultType(Pointer(a_retarget.Remap(pointee), a_retarget.storage));
				Update(a_chain);
				FollowPointer(a_resource, a_chain, pointee, a_walk, a_retarget);
			}

			// Rewrites a uniform block variable's uses onto a pointer a_base makes before each use (a fresh PhysicalStorageBuffer
			// bitcast, or the push block variable with member index a_prefix).
			void RewriteBlock(Resource& a_resource, const Retarget& a_retarget, const Walk& a_walk, const std::function<uint32_t(Instruction*)>& a_base, uint32_t a_prefix) {
				const uint32_t var = a_resource.var->result_id();
				const uint32_t blockPointer = Pointer(a_retarget.Remap(a_resource.pointee), a_retarget.storage);
				for (Instruction* user : Users(var)) {
					if (IsBookkeeping(user)) {
						continue;
					}
					switch (user->opcode()) {
					case spv::Op::OpAccessChain:
					case spv::Op::OpInBoundsAccessChain: {
						if (InOp(user, 0) != var) {
							Fail(a_resource.description, " is used as an access chain index");
						}
						user->SetInOperand(0, { a_base(user) });
						uint32_t first = 1;
						if (a_prefix != 0) {
							user->InsertOperand((user->NumOperands() - user->NumInOperands()) + 1, Operand(SPV_OPERAND_TYPE_ID, { a_prefix }));
							first = 2;
						}
						Update(user);
						RetypeChain(a_resource, user, first, a_resource.pointee, a_walk, a_retarget);
					} break;
					case spv::Op::OpLoad:
					case spv::Op::OpCopyObject: {
						uint32_t pointer = a_base(user);
						if (a_prefix != 0) {
							pointer = Builder(user).AddAccessChain(blockPointer, pointer, { a_prefix })->result_id();
						}
						user->SetInOperand(0, { pointer });
						Update(user);
						if (user->opcode() == spv::Op::OpLoad) {
							FinishLoad(user, a_resource.pointee, a_walk, a_retarget);
						}
						else {
							user->SetResultType(blockPointer);
							Update(user);
							FollowPointer(a_resource, user, a_resource.pointee, a_walk, a_retarget);
						}
					} break;
					case spv::Op::OpPhi:
					case spv::Op::OpSelect:
					case spv::Op::OpFunctionCall:
					case spv::Op::OpExtInst:
						CheckDescriptorUse(a_resource, user);
						[[fallthrough]];
					default:
						Fail(a_resource.description, " is used by ", spvOpcodeString(user->opcode()), ", which the lowering cannot rewrite");
					}
				}
			}

			void RewritePushed(Resource& a_resource) {
				const Clone& clone = clones[a_resource.clone];
				const Retarget retarget{ .storage = spv::StorageClass::PushConstant, .from = a_resource.pointee, .to = clone.type };
				RewriteBlock(a_resource, retarget, Walk{}, [this](Instruction*) { return pushVar; }, UintConstant(clone.member));
			}

			void RewritePhysical(Resource& a_resource) {
				const BindingMapping& mapping = *a_resource.mapping;
				// A uniform block's address is only guaranteed minUniformBufferOffsetAlignment (uniformBlockAlignment); member offsets
				// and strides lower it further (Walk).
				Walk walk{ .align = target.uniformBlockAlignment };
				std::function<uint32_t(Instruction*)> address;
				switch (mapping.source) {
				case BindingSource::BufferPushAddress:
					address = [this, &mapping, &a_resource](Instruction* a_where) { return ReadPushAddress(a_where, mapping.pushOffset + 8 * a_resource.index); };
					break;
				case BindingSource::BufferIndirectAddress:
					address = [this, &mapping, &a_resource](Instruction* a_where) {
						return RecordValue(a_where, Uvec2(), mapping.pushOffset, mapping.recordOffset + 8 * a_resource.index);
					};
					break;
				default: {
					// PushData past the pushed prefix: the spill image holds it at (offset - pushedBytes).
					const uint32_t spilled = mapping.pushOffset - target.pushedBytes;
					walk.align = AlignmentAt(target.spillAlignment, spilled);
					address = [this, spilled](Instruction* a_where) { return AddressPlus(a_where, ReadSpillAddress(a_where), spilled); };
				} break;
				}
				const uint32_t block = a_resource.pointee;
				RewriteBlock(a_resource, Retarget{}, walk, [&](Instruction* a_where) { return PhysicalPointer(a_where, block, address(a_where)); }, 0);
			}

			void Rewrite(Resource& a_resource) {
				const BindingMapping& mapping = *a_resource.mapping;
				const bool sampler = a_resource.kind == VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
				switch (mapping.source) {
				case BindingSource::Heap:
				case BindingSource::StaticSampler: {
					const uint32_t element = mapping.source == BindingSource::Heap ? mapping.heapElement + a_resource.index
					                                                               : target.staticSamplerElements[mapping.staticSampler] + a_resource.index;
					if (element == 0) {
						Redecorate(a_resource, sampler ? target.samplerSet : target.resourceSet, sampler ? target.samplerBinding : target.resourceBinding);
					}
					else {
						RewriteDescriptor(a_resource);
					}
				} break;
				case BindingSource::HeapIndirect:
					RewriteDescriptor(a_resource);
					break;
				case BindingSource::PushData:
					if (a_resource.clone >= 0) {
						RewritePushed(a_resource);
					}
					else {
						RewritePhysical(a_resource);
					}
					break;
				case BindingSource::BufferIndirectAddress:
				case BindingSource::BufferPushAddress:
					RewritePhysical(a_resource);
					break;
				}
			}

			void Finish() {
				for (Resource& resource : resources) {
					if (resource.keep) {
						continue;
					}
					const uint32_t var = resource.var->result_id();
					if (HasRealUses(var)) {
						Fail(resource.description, " still has uses after the lowering (internal error)");
					}
					RemoveFromInterface(var);
					context()->KillInst(resource.var);
					resource.var = nullptr;
				}
				if (usesPhysicalPointers) {
					// OpBitcast from a uvec2 to a pointer needs SPIR-V 1.5.
					if (get_module()->version() < 0x00010500u) {
						Fail("the lowering reads memory through PhysicalStorageBuffer pointers, which needs SPIR-V 1.5 or newer (compile for vulkan1.2+)");
					}
					context()->AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
					get_module()->GetMemoryModel()->SetInOperand(0, { uint32_t(spv::AddressingModel::PhysicalStorageBuffer64) });
				}
				if (usesHeapAliases) {
					context()->AddCapability(spv::Capability::RuntimeDescriptorArray);
					// Core in SPIR-V 1.5; older modules need the extension for the capability.
					if (get_module()->version() < 0x00010500u) {
						bool declared = false;
						for (Instruction& extension : get_module()->extensions()) {
							declared = declared || extension.GetInOperand(0).AsString() == "SPV_EXT_descriptor_indexing";
						}
						if (!declared) {
							context()->AddExtension("SPV_EXT_descriptor_indexing");
						}
					}
				}
			}
		};

		// Keeps only the named entry point (of the stage's execution model, when known) of a module with several, with its
		// execution modes; EliminateDeadFunctions then drops the other entry points' functions.
		class SelectEntryPointPass final : public opt::Pass {
		public:
			SelectEntryPointPass(std::string_view a_name, VkShaderStageFlagBits a_stage, std::string& a_error) :
				entryName(a_name), stage(a_stage), error(a_error) {}

			const char* name() const override { return "basicrhi-select-entry-point"; }

			Status Process() override {
				const std::optional<spv::ExecutionModel> model = ExecutionModelFor(stage);
				Instruction* keep = nullptr;
				std::vector<Instruction*> drop;
				for (Instruction& point : get_module()->entry_points()) {
					const bool match = point.GetInOperand(2).AsString() == entryName && (!model || point.GetSingleWordInOperand(0) == uint32_t(*model));
					if (match && keep) {
						error = Cat("the module has more than one entry point named '", entryName, "' for this stage");
						return Status::Failure;
					}
					if (match) {
						keep = &point;
					}
					else {
						drop.push_back(&point);
					}
				}
				if (!keep) {
					error = Cat("the module has no entry point named '", entryName, "' for this stage");
					return Status::Failure;
				}
				if (drop.empty()) {
					return Status::SuccessWithoutChange;
				}
				const uint32_t kept = keep->GetSingleWordInOperand(1);
				std::set<uint32_t> functions;
				for (Instruction* point : drop) {
					if (point->GetSingleWordInOperand(1) != kept) {
						functions.insert(point->GetSingleWordInOperand(1));
					}
					context()->KillInst(point);
				}
				std::vector<Instruction*> modes;
				for (Instruction& mode : get_module()->execution_modes()) {
					if (functions.count(mode.GetSingleWordInOperand(0)) != 0) {
						modes.push_back(&mode);
					}
				}
				for (Instruction* mode : modes) {
					context()->KillInst(mode);
				}
				return Status::SuccessWithChange;
			}

		private:
			std::string entryName;
			VkShaderStageFlagBits stage;
			std::string& error;
		};

		// True if the module imports a NonSemantic.Shader.DebugInfo set (DXC's -fspv-debug=vulkan*).
		bool ImportsShaderDebugInfo(std::span<const uint32_t> a_spirv) {
			constexpr std::string_view kPrefix = "NonSemantic.Shader.DebugInfo";
			for (size_t word = 5; word < a_spirv.size();) {
				const uint32_t count = a_spirv[word] >> 16, opcode = a_spirv[word] & 0xFFFFu;
				if (count == 0 || word + count > a_spirv.size()) {
					break;
				}
				if (opcode == uint32_t(spv::Op::OpFunction)) {
					break;  // imports precede the functions
				}
				if (opcode == uint32_t(spv::Op::OpExtInstImport) && count > 2) {
					const char* text = reinterpret_cast<const char*>(&a_spirv[word + 2]);
					const size_t bytes = (count - 2) * sizeof(uint32_t);
					const std::string_view name(text, strnlen(text, bytes));
					if (name.substr(0, kPrefix.size()) == kPrefix) {
						return true;
					}
				}
				word += count;
			}
			return false;
		}

		spvtools::MessageConsumer CollectMessages(std::string& a_messages) {
			return [&a_messages](spv_message_level_t a_level, const char*, const spv_position_t& a_position, const char* a_message) {
				if (a_level > SPV_MSG_WARNING) {
					return;
				}
				if (!a_messages.empty()) {
					a_messages += '\n';
				}
				a_messages += Cat("[", a_position.index, "] ", a_message ? a_message : "");
			};
		}

	}

	bool LowerForDescriptorBuffer(std::span<const uint32_t> a_spirv, const BindingPlan& a_plan, VkShaderStageFlagBits a_stage,
		const LoweringTarget& a_target, std::vector<uint32_t>& a_out, std::string& a_error, std::string_view a_entryPoint) {
		a_out.clear();
		a_error.clear();
		try {
			if (a_spirv.size() < 5 || a_spirv[0] != spv::MagicNumber) {
				a_error = "descriptor-buffer lowering: not a SPIR-V module";
				return false;
			}
			std::string passError, messages;
			spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
			optimizer.SetMessageConsumer(CollectMessages(messages));
			if (!a_entryPoint.empty()) {
				optimizer.RegisterPass(spvtools::Optimizer::PassToken(
					std::unique_ptr<spvtools::opt::Pass>(new SelectEntryPointPass(a_entryPoint, a_stage, passError))));
			}
			// Source-level debug info (DebugGlobalVariable and friends) references the resource variables the lowering replaces.
			// Buffer mode strips it rather than rewriting it: such modules lose their NonSemantic info (debug info, and with it any
			// other NonSemantic instructions such as DebugPrintf) in buffer mode only; heap mode keeps it.
			if (ImportsShaderDebugInfo(a_spirv)) {
				optimizer.RegisterPass(spvtools::CreateStripNonSemanticInfoPass());
			}
			optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
			optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
			optimizer.RegisterPass(spvtools::Optimizer::PassToken(
				std::unique_ptr<spvtools::opt::Pass>(new DescriptorBufferLoweringPass(a_plan, a_stage, a_target, passError))));
			// Cleanup: fold the constant element arithmetic and drop what the rewrite left dead (types, constants, loads). The
			// interface is preserved: inputs and outputs must keep matching the neighbouring stages.
			optimizer.RegisterPass(spvtools::CreateScalarReplacementPass());
			optimizer.RegisterPass(spvtools::CreateSSARewritePass());
			optimizer.RegisterPass(spvtools::CreateCCPPass());
			optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));

			spvtools::OptimizerOptions options;
			options.set_run_validator(false);  // DXC validated its output; ValidateLoweredModule checks ours in diagnostic mode
			std::vector<uint32_t> lowered;
			if (!optimizer.Run(a_spirv.data(), a_spirv.size(), &lowered, options)) {
				if (!passError.empty()) {
					a_error = "descriptor-buffer lowering: " + passError;
				}
				else {
					a_error = "descriptor-buffer lowering: the SPIR-V optimizer failed" + (messages.empty() ? std::string() : ": " + messages);
				}
				return false;
			}
			a_out = std::move(lowered);
			return true;
		}
		catch (const std::exception& e) {
			a_error = Cat("descriptor-buffer lowering: ", e.what());
		}
		catch (...) {
			a_error = "descriptor-buffer lowering: unknown exception";
		}
		a_out.clear();
		return false;
	}

	bool ValidateLoweredModule(std::span<const uint32_t> a_spirv, std::string& a_error) {
		a_error.clear();
		try {
			std::string messages;
			spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
			tools.SetMessageConsumer(CollectMessages(messages));
			spvtools::ValidatorOptions options;
			options.SetScalarBlockLayout(true);
			if (tools.Validate(a_spirv.data(), a_spirv.size(), options)) {
				return true;
			}
			a_error = messages.empty() ? "SPIR-V validation failed" : messages;
		}
		catch (const std::exception& e) {
			a_error = Cat("SPIR-V validation: ", e.what());
		}
		catch (...) {
			a_error = "SPIR-V validation: unknown exception";
		}
		return false;
	}

}
