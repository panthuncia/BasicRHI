#pragma once

#include <cstdint>
#include <type_traits>

namespace rhi {

	enum ResourceAccessType {
		None = 0,
		Common = 1,
		VertexBuffer = 1 << 1,
		ConstantBuffer = 1 << 2,
		IndexBuffer = 1 << 3,
		RenderTarget = 1 << 4,
		UnorderedAccess = 1 << 5,
		DepthReadWrite = 1 << 6,
		DepthRead = 1 << 7,
		ShaderResource = 1 << 8,
		IndirectArgument = 1 << 9,
		CopyDest = 1 << 10,
		CopySource = 1 << 11,
		RaytracingAccelerationStructureRead = 1 << 12,
		RaytracingAccelerationStructureWrite = 1 << 13,
	};

	inline ResourceAccessType operator|(ResourceAccessType a, ResourceAccessType b)
	{
		return static_cast<ResourceAccessType>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	}

	enum class ResourceLayout {
		Undefined,
		Common,
		Present,
		GenericRead,
		RenderTarget,
		UnorderedAccess,
		DepthReadWrite,
		DepthRead,
		ShaderResource,
		CopySource,
		CopyDest,

		ResolveSource,
		ResolveDest,
		ShadingRateSource,

		DirectCommon,
		DirectGenericRead,
		DirectUnorderedAccess,
		DirectShaderResource,
		DirectCopySource,
		DirectCopyDest,

		ComputeCommon,
		ComputeGenericRead,
		ComputeUnorderedAccess,
		ComputeShaderResource,
		ComputeCopySource,
		ComputeCopyDest
	};

	inline bool ResourceLayoutIsUnorderedAccess(ResourceLayout layout) {
		return layout == ResourceLayout::UnorderedAccess ||
			layout == ResourceLayout::DirectUnorderedAccess ||
			layout == ResourceLayout::ComputeUnorderedAccess;
	}

	enum class ResourceSyncState : uint32_t {
		None = 0x0,
		All = 0x1,
		Draw = 0x2,
		IndexInput = 0x4,
		VertexShading = 0x8,
		PixelShading = 0x10,
		DepthStencil = 0x20,
		RenderTarget = 0x40,
		ComputeShading = 0x80,
		Raytracing = 0x100,
		Copy = 0x200,
		Resolve = 0x400,
		ExecuteIndirect = 0x800,
		Predication = 0x800,
		AllShading = 0x1000,
		NonPixelShading = 0x2000,
		EmitRaytracingAccelerationStructurePostbuildInfo = 0x4000,
		ClearUnorderedAccessView = 0x8000,
		VideoDecode = 0x100000,
		VideoProcess = 0x200000,
		VideoEncode = 0x400000,
		BuildRaytracingAccelerationStructure = 0x800000,
		CopyRatracingAccelerationStructure = 0x1000000,
		SyncSplit = 0x80000000u,
	};

	inline constexpr ResourceSyncState operator|(ResourceSyncState a, ResourceSyncState b) noexcept
	{
		using U = std::underlying_type_t<ResourceSyncState>;
		return static_cast<ResourceSyncState>(static_cast<U>(a) | static_cast<U>(b));
	}

	inline constexpr ResourceSyncState operator&(ResourceSyncState a, ResourceSyncState b) noexcept
	{
		using U = std::underlying_type_t<ResourceSyncState>;
		return static_cast<ResourceSyncState>(static_cast<U>(a) & static_cast<U>(b));
	}

	inline constexpr ResourceSyncState operator~(ResourceSyncState a) noexcept
	{
		using U = std::underlying_type_t<ResourceSyncState>;
		return static_cast<ResourceSyncState>(~static_cast<U>(a));
	}

	inline constexpr ResourceSyncState& operator|=(ResourceSyncState& a, ResourceSyncState b) noexcept
	{
		a = a | b;
		return a;
	}

	inline constexpr bool ResourceSyncStateHasAny(ResourceSyncState value, ResourceSyncState bits) noexcept
	{
		using U = std::underlying_type_t<ResourceSyncState>;
		return (static_cast<U>(value) & static_cast<U>(bits)) != 0;
	}

	inline constexpr bool ResourceSyncStateHasOnly(ResourceSyncState value, ResourceSyncState allowed) noexcept
	{
		using U = std::underlying_type_t<ResourceSyncState>;
		return (static_cast<U>(value) & ~static_cast<U>(allowed)) == 0;
	}

	inline unsigned int ResourceAccessGetNumReadStates(ResourceAccessType access) {
		int num = 0;
		if (access & ResourceAccessType::ShaderResource) num++;
		if (access & ResourceAccessType::DepthRead) num++;
		if (access & ResourceAccessType::RenderTarget) num++;
		if (access & ResourceAccessType::CopySource) num++;
		if (access & ResourceAccessType::IndexBuffer) num++;
		if (access & ResourceAccessType::VertexBuffer) num++;
		if (access & ResourceAccessType::ConstantBuffer) num++;
		if (access & ResourceAccessType::IndirectArgument) num++;

		return num;
	}

	inline ResourceLayout AccessToLayout(ResourceAccessType access, bool directQueue) {
		// most-specific first:
		if (access & ResourceAccessType::Common)
			return ResourceLayout::Common;
		if (access & ResourceAccessType::UnorderedAccess)
			return ResourceLayout::UnorderedAccess;
		if (access & ResourceAccessType::RenderTarget)
			return ResourceLayout::RenderTarget;
		if (access & ResourceAccessType::DepthReadWrite)
			return ResourceLayout::DepthReadWrite;
		if (access & ResourceAccessType::CopySource)
			return ResourceLayout::CopySource;
		if (access & ResourceAccessType::CopyDest)
			return ResourceLayout::CopyDest;

		auto num = ResourceAccessGetNumReadStates(access);
		if (num > 1) {
			if (directQueue) {
				return ResourceLayout::DirectGenericRead;
			}
			else {
				return ResourceLayout::ComputeGenericRead;
			}
		}
		else {
			if (access & ResourceAccessType::ShaderResource) {
				return ResourceLayout::ShaderResource;
			}
			if (access & ResourceAccessType::DepthRead) {
				return ResourceLayout::DepthRead;
			}
			if (access & ResourceAccessType::IndexBuffer) {
				return ResourceLayout::GenericRead;
			}
			if (access & ResourceAccessType::VertexBuffer) {
				return ResourceLayout::GenericRead;
			}
			if (access & ResourceAccessType::ConstantBuffer) {
				return ResourceLayout::GenericRead;
			}
			if (access & ResourceAccessType::IndirectArgument) {
				return ResourceLayout::GenericRead;
			}
		}

		return ResourceLayout::Common;
	}


	inline ResourceSyncState ComputeSyncFromAccess(ResourceAccessType access) {
		bool needsIndirect = (access & ResourceAccessType::IndirectArgument) != 0;

		if (needsIndirect) {
			return ResourceSyncState::ExecuteIndirect;
		}

		return ResourceSyncState::ComputeShading;
	}

	inline ResourceSyncState RenderSyncFromAccess(ResourceAccessType access)
	{
		// pick out each distinct sync category
		bool needsCommon = (access & ResourceAccessType::Common) != 0;
		bool needsShading = (access & (ResourceAccessType::VertexBuffer
			| ResourceAccessType::ConstantBuffer
			| ResourceAccessType::ShaderResource
			| ResourceAccessType::UnorderedAccess)) != 0;
		bool needsIndexInput = (access & ResourceAccessType::IndexBuffer) != 0;
		bool needsRenderTarget = (access & ResourceAccessType::RenderTarget) != 0;
		bool needsDepthStencil = (access & (ResourceAccessType::DepthRead
			| ResourceAccessType::DepthReadWrite)) != 0;
		bool needsCopy = (access & (ResourceAccessType::CopySource
			| ResourceAccessType::CopyDest)) != 0;
		bool needsIndirect = (access & ResourceAccessType::IndirectArgument) != 0;
		bool needsRayTracing = (access & ResourceAccessType::RaytracingAccelerationStructureRead) != 0;
		bool needsBuildAS = (access & ResourceAccessType::RaytracingAccelerationStructureWrite) != 0;

		// count how many distinct categories are requested
		int categoryCount =
			(int)needsCommon
			+ (int)needsShading
			+ (int)needsIndexInput
			+ (int)needsRenderTarget
			+ (int)needsDepthStencil
			+ (int)needsCopy
			+ (int)needsIndirect
			+ (int)needsRayTracing
			+ (int)needsBuildAS;

		// zero categories = no sync
		if (categoryCount == 0)
			return ResourceSyncState::None;
		// Mixed graphics-pipeline categories can use the DRAW aggregate scope.
		if (categoryCount > 1
			&& !needsCommon
			&& !needsCopy
			&& !needsIndirect
			&& !needsRayTracing
			&& !needsBuildAS)
			return ResourceSyncState::Draw;
		// Other mixed categories still need the conservative full-pipeline fallback.
		if (categoryCount > 1)
			return ResourceSyncState::All;

		// exactly one category = pick it
		if (needsCommon)        return ResourceSyncState::All;
		if (needsShading)       return ResourceSyncState::AllShading;
		if (needsIndexInput)    return ResourceSyncState::IndexInput;
		if (needsRenderTarget)  return ResourceSyncState::RenderTarget;
		if (needsDepthStencil)  return ResourceSyncState::DepthStencil;
		if (needsCopy)          return ResourceSyncState::Copy;
		if (needsIndirect)      return ResourceSyncState::ExecuteIndirect;
		if (needsBuildAS)       return ResourceSyncState::BuildRaytracingAccelerationStructure;
		if (needsRayTracing)    return ResourceSyncState::Raytracing;

		// (should never get here)
		return ResourceSyncState::All;
	}

	inline bool AccessTypeIsWriteType(ResourceAccessType access) {
		if (access & ResourceAccessType::RenderTarget) return true;
		if (access & ResourceAccessType::DepthReadWrite) return true;
		if (access & ResourceAccessType::CopyDest) return true;
		if (access & ResourceAccessType::UnorderedAccess) return true;
		if (access & ResourceAccessType::RaytracingAccelerationStructureWrite) return true;
		return false;
	}

	inline bool ValidateResourceLayoutAndAccessType(ResourceLayout layout, ResourceAccessType access) {
		if (access & ResourceAccessType::DepthRead && access & ResourceAccessType::DepthReadWrite) {
			return false;
		}
		switch (layout) {
		case ResourceLayout::Common:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopyDest | ResourceAccessType::CopySource)) != 0)
				return false;
			break;
		case ResourceLayout::DirectCommon:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopyDest | ResourceAccessType::CopySource | ResourceAccessType::UnorderedAccess)) != 0)
				return false;
			break;
		case ResourceLayout::ComputeCommon:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopyDest | ResourceAccessType::CopySource | ResourceAccessType::UnorderedAccess)) != 0)
				return false;
			break;
		case ResourceLayout::GenericRead:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopySource)) != 0)
				return false;
			break;
		case ResourceLayout::DirectGenericRead:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopySource | ResourceAccessType::DepthRead /*| ResourceAccessType::SHADING_RATE_SOURCE | ResourceAccessType::RESOLVE_SOURCE*/)) != 0)
				return false;
			break;
		case ResourceLayout::ComputeGenericRead:
			if ((access & ~(ResourceAccessType::ShaderResource | ResourceAccessType::IndirectArgument | ResourceAccessType::CopySource)) != 0)
				return false;
			break;
		case ResourceLayout::RenderTarget:
			if ((access & ~(ResourceAccessType::RenderTarget)) != 0)
				return false;
			break;
		case ResourceLayout::UnorderedAccess:
		case ResourceLayout::DirectUnorderedAccess:
		case ResourceLayout::ComputeUnorderedAccess:
			if ((access & ~(ResourceAccessType::UnorderedAccess)) != 0)
				return false;
			break;
		case ResourceLayout::DepthReadWrite:
			if ((access & ~(ResourceAccessType::DepthReadWrite | ResourceAccessType::DepthRead)) != 0)
				return false;
			break;
		case ResourceLayout::DepthRead:
			if ((access & ~(ResourceAccessType::DepthRead)) != 0)
				return false;
			break;
		case ResourceLayout::ShaderResource:
		case ResourceLayout::DirectShaderResource:
		case ResourceLayout::ComputeShaderResource:
			if ((access & ~(ResourceAccessType::ShaderResource)) != 0)
				return false;
			break;
		case ResourceLayout::CopySource:
		case ResourceLayout::DirectCopySource:
		case ResourceLayout::ComputeCopySource:
			if ((access & ~(ResourceAccessType::CopySource)) != 0)
				return false;
			break;
		case ResourceLayout::CopyDest:
			if ((access & ~(ResourceAccessType::CopyDest)) != 0)
				return false;
			break;
		default:
			break;
		}
		//TODO: other types
		return true;
	}

	inline bool ResourceSyncStateIsNotComputeSyncState(ResourceSyncState state) {
		constexpr ResourceSyncState kComputeSupported =
			ResourceSyncState::None
			| ResourceSyncState::All
			| ResourceSyncState::ComputeShading
			| ResourceSyncState::Copy
			| ResourceSyncState::Resolve
			| ResourceSyncState::ExecuteIndirect
			| ResourceSyncState::Predication
			| ResourceSyncState::AllShading
			| ResourceSyncState::NonPixelShading
			| ResourceSyncState::Raytracing
			| ResourceSyncState::BuildRaytracingAccelerationStructure
			| ResourceSyncState::CopyRatracingAccelerationStructure
			| ResourceSyncState::EmitRaytracingAccelerationStructurePostbuildInfo
			| ResourceSyncState::ClearUnorderedAccessView
			| ResourceSyncState::SyncSplit;
		return !ResourceSyncStateHasOnly(state, kComputeSupported);
	}
}