// VRAM attribution and placement in rhi::ma::Allocator, on the backend named by the first argument (vulkan or d3d12).
// Owner labels from a thread scope, an AllocationDesc and a relabel, and the per-owner report. Spillable allocations
// stay in video memory within budget and land in system memory over a tiny budget cap; the placement overrides force
// either way. A spilled buffer must still work as a copy target.
#include "rhi.h"
#include "rhi_allocator.h"
#include "rhi_helpers.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
	using rhi::ma::AllocationPtr;

	constexpr uint64_t kBufferBytes = 1ull << 20;
	constexpr uint32_t kTextureSize = 512; // RGBA8: 1 MiB

	bool Check(rhi::Result result, const char* operation) {
		if (rhi::IsOk(result)) {
			return true;
		}
		std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
		return false;
	}

	bool Expect(bool condition, const char* what) {
		if (!condition) {
			std::fprintf(stderr, "expectation failed: %s\n", what);
		}
		return condition;
	}

	rhi::Result CreateBuffer(rhi::ma::Allocator& allocator, rhi::ma::AllocationFlags flags, const char* owner, rhi::HeapType heapType,
		AllocationPtr& out, rhi::ResourceFlags resourceFlags = rhi::RF_None) {
		rhi::ma::AllocationDesc desc{};
		desc.flags = flags;
		desc.heapType = heapType;
		desc.owner = owner;
		const auto resourceDesc = rhi::helpers::ResourceDesc::Buffer(kBufferBytes, heapType, resourceFlags, "AllocatorPlacementSmoke buffer");
		return allocator.CreateResource(&desc, &resourceDesc, 0, nullptr, out);
	}

	rhi::Result CreateTexture(rhi::ma::Allocator& allocator, rhi::ma::AllocationFlags flags, const char* owner, AllocationPtr& out) {
		rhi::ma::AllocationDesc desc{};
		desc.flags = flags;
		desc.owner = owner;
		const auto resourceDesc = rhi::helpers::ResourceDesc::Tex2D(rhi::Format::R8G8B8A8_UNorm, rhi::HeapType::DeviceLocal, kTextureSize, kTextureSize,
			1, 1, 1, rhi::ResourceLayout::Undefined, nullptr, rhi::RF_None, "AllocatorPlacementSmoke texture");
		return allocator.CreateResource(&desc, &resourceDesc, 0, nullptr, out);
	}

	const rhi::ma::OwnerMemoryReport* FindRow(const std::vector<rhi::ma::OwnerMemoryReport>& rows, const char* owner) {
		for (const auto& row : rows) {
			if (std::strcmp(row.owner, owner) == 0) return &row;
		}
		return nullptr;
	}

	std::vector<rhi::ma::OwnerMemoryReport> Report(const rhi::ma::Allocator& allocator) {
		std::vector<rhi::ma::OwnerMemoryReport> rows(allocator.GetOwnerReport(nullptr, 0));
		rows.resize(allocator.GetOwnerReport(rows.data(), static_cast<uint32_t>(rows.size())));
		for (const auto& row : rows) {
			std::printf("  owner '%s': %u allocations, %llu bytes (video %llu, system %llu), spilled %u (%llu bytes)\n", row.owner,
				row.allocationCount, static_cast<unsigned long long>(row.allocationBytes),
				static_cast<unsigned long long>(row.bytesBySegment[0]), static_cast<unsigned long long>(row.bytesBySegment[1]),
				row.spilledCount, static_cast<unsigned long long>(row.spilledBytes));
		}
		return rows;
	}

	bool InVideoMemory(const AllocationPtr& allocation) {
		const auto placement = allocation->GetPlacement();
		return placement.heapType == rhi::HeapType::DeviceLocal && placement.segment == rhi::MemorySegmentGroup::Local && !placement.spilled;
	}

	bool InSystemMemory(const AllocationPtr& allocation) {
		const auto placement = allocation->GetPlacement();
		return placement.heapType == rhi::HeapType::SystemMemory && placement.spilled && placement.segment == rhi::MemorySegmentGroup::NonLocal;
	}

	// Upload -> `target` -> readback: the words must come back.
	bool CopyThrough(rhi::Device& device, rhi::ma::Allocator& allocator, rhi::Resource& target) {
		AllocationPtr upload, readback;
		if (!Check(CreateBuffer(allocator, rhi::ma::AllocationFlagNone, "Smoke::Staging", rhi::HeapType::Upload, upload), "create upload buffer") ||
			!Check(CreateBuffer(allocator, rhi::ma::AllocationFlagNone, "Smoke::Staging", rhi::HeapType::Readback, readback), "create readback buffer")) {
			return false;
		}
		void* mapped = nullptr;
		upload->GetResource().Map(&mapped, 0, kBufferBytes);
		if (!mapped) return Expect(false, "upload buffer maps");
		for (uint32_t word = 0; word < kBufferBytes / sizeof(uint32_t); ++word) {
			static_cast<uint32_t*>(mapped)[word] = word * 2654435761u;
		}
		upload->GetResource().Unmap(0, kBufferBytes);

		auto queue = device.GetQueue(rhi::QueueKind::Graphics);
		rhi::CommandAllocatorPtr commandAllocator;
		rhi::CommandListPtr commandList;
		if (!Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, commandAllocator), "CreateCommandAllocator") ||
			!Check(device.CreateCommandList(rhi::QueueKind::Graphics, commandAllocator.Get(), commandList), "CreateCommandList")) {
			return false;
		}
		rhi::BufferBarrier toCopyDest{
			.buffer = target.GetHandle(),
			.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::Copy,
			.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::CopyDest,
		};
		commandList->Barriers(rhi::BarrierBatch{ .buffers = { &toCopyDest, 1 } });
		commandList->CopyBufferRegion(target.GetHandle(), 0, upload->GetResource().GetHandle(), 0, kBufferBytes);
		rhi::BufferBarrier toCopySource{
			.buffer = target.GetHandle(),
			.beforeSync = rhi::ResourceSyncState::Copy, .afterSync = rhi::ResourceSyncState::Copy,
			.beforeAccess = rhi::ResourceAccessType::CopyDest, .afterAccess = rhi::ResourceAccessType::CopySource,
		};
		commandList->Barriers(rhi::BarrierBatch{ .buffers = { &toCopySource, 1 } });
		commandList->CopyBufferRegion(readback->GetResource().GetHandle(), 0, target.GetHandle(), 0, kBufferBytes);
		commandList->End();
		const rhi::CommandList lists[] = { commandList.Get() };
		if (!Check(queue.Submit(lists, {}), "Queue::Submit") || !Check(device.WaitIdle(), "WaitIdle")) {
			return false;
		}
		readback->GetResource().Map(&mapped, 0, kBufferBytes);
		if (!mapped) return Expect(false, "readback buffer maps");
		bool same = true;
		for (uint32_t word = 0; word < kBufferBytes / sizeof(uint32_t) && same; ++word) {
			same = static_cast<const uint32_t*>(mapped)[word] == word * 2654435761u;
		}
		readback->GetResource().Unmap(0, 0);
		return Expect(same, "the words copied through the spilled buffer come back");
	}

	bool Run(rhi::Device& device, rhi::ma::Allocator& allocator) {
		using rhi::ma::AllocationFlagNone;
		using rhi::ma::AllocationFlagSpillable;

		// Owner labels: a thread scope, an explicit owner, and none.
		AllocationPtr scoped, nested, explicitOwner, unlabelled;
		{
			rhi::ma::AllocationOwnerScope scope("Smoke::Scope");
			if (!Check(CreateBuffer(allocator, AllocationFlagNone, nullptr, rhi::HeapType::DeviceLocal, scoped), "scoped buffer")) return false;
			{
				rhi::ma::AllocationOwnerScope inner("Smoke::Inner");
				if (!Check(CreateTexture(allocator, AllocationFlagNone, nullptr, nested), "nested texture")) return false;
			}
			if (!Check(CreateTexture(allocator, AllocationFlagNone, "Smoke::Explicit", explicitOwner), "explicit texture")) return false;
		}
		if (!Check(CreateBuffer(allocator, AllocationFlagNone, nullptr, rhi::HeapType::DeviceLocal, unlabelled), "unlabelled buffer")) return false;
		if (!Expect(std::strcmp(scoped->GetOwner(), "Smoke::Scope") == 0 && std::strcmp(nested->GetOwner(), "Smoke::Inner") == 0 &&
				std::strcmp(explicitOwner->GetOwner(), "Smoke::Explicit") == 0 && unlabelled->GetOwner()[0] == '\0', "owners as labelled")) {
			return false;
		}
		allocator.SetOwner(unlabelled.Get(), "Smoke::Relabelled", false);
		allocator.SetOwner(scoped.Get(), "Smoke::Ignored", false);
		if (!Expect(std::strcmp(unlabelled->GetOwner(), "Smoke::Relabelled") == 0 && std::strcmp(scoped->GetOwner(), "Smoke::Scope") == 0,
				"relabel without overwrite only labels the unlabelled")) {
			return false;
		}
		std::puts(" labels:");
		{
			const auto rows = Report(allocator);
			const auto* scopeRow = FindRow(rows, "Smoke::Scope");
			const auto* explicitRow = FindRow(rows, "Smoke::Explicit");
			if (!Expect(scopeRow && scopeRow->allocationCount == 1 && scopeRow->allocationBytes >= kBufferBytes &&
					scopeRow->countByHeapType[static_cast<uint32_t>(rhi::HeapType::DeviceLocal)] == 1, "scope row") ||
				!Expect(explicitRow && explicitRow->allocationCount == 1 && explicitRow->bytesBySegment[0] >= kBufferBytes, "explicit row") ||
				!Expect(FindRow(rows, "Smoke::Relabelled") && !FindRow(rows, "") && !FindRow(rows, "Smoke::Ignored"), "relabelled row")) {
				return false;
			}
		}

		// A one-byte budget: spillable allocations go to system memory, the others stay. (Spillable allocations that fit in
		// a spill-pool block already made take no new memory, so they would stay; this runs before there is one.)
		allocator.SetBudgetLimit(rhi::MemorySegmentGroup::Local, 1);
		AllocationPtr spilledTexture, spilledBuffer, pinned;
		if (!Check(CreateTexture(allocator, AllocationFlagSpillable, "Smoke::Spillable", spilledTexture), "spillable texture over budget") ||
			!Check(CreateBuffer(allocator, AllocationFlagSpillable, "Smoke::Spillable", rhi::HeapType::DeviceLocal, spilledBuffer), "spillable buffer over budget") ||
			!Check(CreateTexture(allocator, AllocationFlagNone, "Smoke::Pinned", pinned), "plain texture over budget")) {
			return false;
		}
		if (!Expect(InSystemMemory(spilledTexture) && InSystemMemory(spilledBuffer), "spillable allocations over budget are in system memory") ||
			!Expect(InVideoMemory(pinned), "a plain allocation over budget stays in video memory")) {
			return false;
		}
		if (!CopyThrough(device, allocator, spilledBuffer->GetResource())) return false;

		// Forced placement: DeviceLocal keeps a spillable one in video memory over budget; SystemMemory moves a plain one.
		allocator.SetPlacementOverride("Smoke::Spillable", rhi::ma::PlacementOverride::DeviceLocal);
		allocator.SetPlacementOverride("Smoke::Forced", rhi::ma::PlacementOverride::SystemMemory);
		AllocationPtr keptLocal, forcedSystem;
		if (!Check(CreateTexture(allocator, AllocationFlagSpillable, "Smoke::Spillable", keptLocal), "spillable texture forced local") ||
			!Check(CreateTexture(allocator, AllocationFlagNone, "Smoke::Forced", forcedSystem), "plain texture forced to system memory")) {
			return false;
		}
		if (!Expect(InVideoMemory(keptLocal), "the DeviceLocal override keeps a spillable allocation in video memory") ||
			!Expect(InSystemMemory(forcedSystem), "the SystemMemory override moves a plain allocation")) {
			return false;
		}
		allocator.SetPlacementOverride("Smoke::Spillable", rhi::ma::PlacementOverride::None);
		allocator.SetPlacementOverride("Smoke::Forced", rhi::ma::PlacementOverride::None);
		allocator.SetBudgetLimit(rhi::MemorySegmentGroup::Local, 0);

		// Spillable, within budget: video memory, in the spill pool.
		AllocationPtr spillableFits;
		if (!Check(CreateTexture(allocator, AllocationFlagSpillable, "Smoke::Spillable", spillableFits), "spillable texture within budget") ||
			!Expect(InVideoMemory(spillableFits) && spillableFits->GetPlacement().spillable, "a spillable texture within budget stays in video memory")) {
			return false;
		}

		std::puts(" placement:");
		{
			const auto rows = Report(allocator);
			const auto* spillRow = FindRow(rows, "Smoke::Spillable");
			const auto* forcedRow = FindRow(rows, "Smoke::Forced");
			if (!Expect(spillRow && spillRow->allocationCount == 4 && spillRow->spilledCount == 2 &&
					spillRow->countByHeapType[static_cast<uint32_t>(rhi::HeapType::SystemMemory)] == 2, "spillable row") ||
				!Expect(forcedRow && forcedRow->spilledCount == 1 && forcedRow->bytesBySegment[1] >= kBufferBytes, "forced row")) {
				return false;
			}
		}

		// The statistics see the spill and system-memory pools.
		rhi::ma::TotalStatistics stats{};
		allocator.CalculateStatistics(&stats);
		if (!Expect(stats.heapType[static_cast<uint32_t>(rhi::HeapType::SystemMemory)].stats.allocationCount == 3, "statistics count the system-memory pool")) {
			return false;
		}

		// Freed allocations leave the report.
		scoped.Reset(); nested.Reset(); explicitOwner.Reset(); unlabelled.Reset();
		spillableFits.Reset(); spilledTexture.Reset(); spilledBuffer.Reset(); pinned.Reset(); keptLocal.Reset(); forcedSystem.Reset();
		const uint32_t remaining = allocator.GetOwnerReport(nullptr, 0);
		return Expect(remaining == 0, "the report is empty once everything is freed");
	}
}

int main(int argc, char** argv) {
	const std::string backendName = argc > 1 ? argv[1] : "vulkan";
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = backendName == "d3d12" ? rhi::Backend::D3D12 : rhi::Backend::Vulkan;
	createInfo.enableDebug = true;
	rhi::DevicePtr device;
	if (!Check(createInfo.backend == rhi::Backend::Vulkan ? rhi::CreateVulkanDevice(createInfo, device) : rhi::CreateD3D12Device(createInfo, device),
			"create device")) {
		return 1;
	}

	rhi::ma::AllocatorDesc allocatorDesc{};
	allocatorDesc.device = device.Get();
	rhi::ma::Allocator* allocator = nullptr;
	if (!Check(rhi::ma::CreateAllocator(&allocatorDesc, &allocator), "CreateAllocator")) {
		return 1;
	}
	const bool ok = Run(device.Get(), *allocator);
	allocator->ReleaseThis();
	device->CheckDebugMessages();
	if (!ok) {
		return 1;
	}
	std::printf("BasicRHIAllocatorPlacementSmoke (%s): success\n", backendName.c_str());
	return 0;
}
