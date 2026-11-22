// CopyRetain defragmentation in rhi::ma::Allocator, on the backend named by the first argument (vulkan or d3d12).
// A pool of 4 MiB blocks is filled with 1 MiB buffers and then thinned out. Full passes with CopyRetain move the
// survivors into one block. The moved buffers keep their owner records and contents. The old buffers come back as
// retained allocations and stay readable. The emptied blocks are freed only once the retained allocations are dropped,
// and then by the block vector's usual rule (it may keep one empty block).
#include "rhi.h"
#include "rhi_allocator.h"
#include "rhi_helpers.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
	using rhi::ma::AllocationPtr;

	constexpr uint64_t kBufferBytes = 1ull << 20;
	constexpr uint64_t kBlockBytes = 4ull << 20;
	constexpr uint32_t kBuffersPerBlock = 4;
	constexpr uint32_t kBlockCount = 3;
	constexpr uint32_t kBufferCount = kBuffersPerBlock * kBlockCount;
	constexpr uint32_t kWords = static_cast<uint32_t>(kBufferBytes / sizeof(uint32_t));

	// What ORG would keep in an allocation's private data. Here it only names the buffer's pattern.
	struct OwnerRecord {
		uint32_t seed = 0;
	};

	uint32_t Pattern(uint32_t seed, uint32_t word) {
		return ((seed + 1u) * 2654435761u) ^ (word * 40503u);
	}

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

	bool SameHeap(rhi::HeapHandle a, rhi::HeapHandle b) {
		return a.index == b.index && a.generation == b.generation;
	}

	uint32_t BlockCount(rhi::ma::Pool& pool) {
		rhi::ma::Statistics stats{};
		pool.GetStatistics(&stats);
		return stats.blockCount;
	}

	const auto kPoolBufferDesc = rhi::helpers::ResourceDesc::Buffer(kBufferBytes, rhi::HeapType::DeviceLocal, rhi::RF_None,
		"AllocatorDefragmentSmoke buffer");

	void CopyBarrier(rhi::CommandListPtr& list, rhi::ResourceHandle buffer, rhi::ResourceAccessType after) {
		rhi::BufferBarrier barrier{
			.buffer = buffer,
			.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::Copy,
			.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = after,
		};
		list->Barriers(rhi::BarrierBatch{ .buffers = { &barrier, 1 } });
	}

	// Records with `record`, submits on the graphics queue and waits for the device.
	template <class TRecord>
	bool RunOnGpu(rhi::Device& device, TRecord&& record) {
		auto queue = device.GetQueue(rhi::QueueKind::Graphics);
		rhi::CommandAllocatorPtr commandAllocator;
		rhi::CommandListPtr commandList;
		if (!Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, commandAllocator), "CreateCommandAllocator") ||
			!Check(device.CreateCommandList(rhi::QueueKind::Graphics, commandAllocator.Get(), commandList), "CreateCommandList")) {
			return false;
		}
		record(commandList);
		commandList->End();
		const rhi::CommandList lists[] = { commandList.Get() };
		return Check(queue.Submit(lists, {}), "Queue::Submit") && Check(device.WaitIdle(), "WaitIdle");
	}

	rhi::Result CreateStaging(rhi::ma::Allocator& allocator, rhi::HeapType heapType, uint64_t bytes, AllocationPtr& out) {
		rhi::ma::AllocationDesc desc{};
		desc.heapType = heapType;
		desc.owner = "Smoke::Staging";
		const auto resourceDesc = rhi::helpers::ResourceDesc::Buffer(bytes, heapType, rhi::RF_None, "AllocatorDefragmentSmoke staging");
		return allocator.CreateResource(&desc, &resourceDesc, 0, nullptr, out);
	}

	// Writes each buffer's pattern through one upload buffer.
	bool Fill(rhi::Device& device, rhi::ma::Allocator& allocator, const std::vector<AllocationPtr>& buffers) {
		AllocationPtr upload;
		if (!Check(CreateStaging(allocator, rhi::HeapType::Upload, kBufferBytes * buffers.size(), upload), "create upload buffer")) return false;
		void* mapped = nullptr;
		upload->GetResource().Map(&mapped, 0, kBufferBytes * buffers.size());
		if (!mapped) return Expect(false, "upload buffer maps");
		for (size_t i = 0; i < buffers.size(); ++i) {
			const uint32_t seed = buffers[i]->GetOwnerRecord<OwnerRecord>()->seed;
			uint32_t* words = static_cast<uint32_t*>(mapped) + i * kWords;
			for (uint32_t word = 0; word < kWords; ++word) words[word] = Pattern(seed, word);
		}
		upload->GetResource().Unmap(0, kBufferBytes * buffers.size());
		return RunOnGpu(device, [&](rhi::CommandListPtr& list) {
			for (size_t i = 0; i < buffers.size(); ++i) {
				const rhi::ResourceHandle target = buffers[i]->GetResource().GetHandle();
				CopyBarrier(list, target, rhi::ResourceAccessType::CopyDest);
				list->CopyBufferRegion(target, 0, upload->GetResource().GetHandle(), i * kBufferBytes, kBufferBytes);
			}
		});
	}

	struct Readable {
		rhi::Resource* resource;
		uint32_t seed;
	};

	// Copies each resource into one readback buffer and checks its pattern.
	bool Verify(rhi::Device& device, rhi::ma::Allocator& allocator, const std::vector<Readable>& items, const char* what) {
		if (items.empty()) return true;
		AllocationPtr readback;
		if (!Check(CreateStaging(allocator, rhi::HeapType::Readback, kBufferBytes * items.size(), readback), "create readback buffer")) return false;
		const bool copied = RunOnGpu(device, [&](rhi::CommandListPtr& list) {
			for (size_t i = 0; i < items.size(); ++i) {
				const rhi::ResourceHandle source = items[i].resource->GetHandle();
				CopyBarrier(list, source, rhi::ResourceAccessType::CopySource);
				list->CopyBufferRegion(readback->GetResource().GetHandle(), i * kBufferBytes, source, 0, kBufferBytes);
			}
		});
		if (!copied) return false;
		void* mapped = nullptr;
		readback->GetResource().Map(&mapped, 0, kBufferBytes * items.size());
		if (!mapped) return Expect(false, "readback buffer maps");
		bool same = true;
		for (size_t i = 0; i < items.size() && same; ++i) {
			const uint32_t* words = static_cast<const uint32_t*>(mapped) + i * kWords;
			for (uint32_t word = 0; word < kWords && same; ++word) same = words[word] == Pattern(items[i].seed, word);
		}
		readback->GetResource().Unmap(0, 0);
		return Expect(same, what);
	}

	struct PendingMove {
		rhi::HeapHandle srcHeap;
		uint64_t srcOffset;
		rhi::HeapHandle dstHeap;
		uint64_t dstOffset;
		uint32_t seed;
	};

	bool Run(rhi::Device& device, rhi::ma::Allocator& allocator, rhi::ma::Pool& pool) {
		// Fill three blocks with four buffers each.
		std::vector<OwnerRecord> records(kBufferCount);
		std::vector<AllocationPtr> buffers(kBufferCount);
		for (uint32_t i = 0; i < kBufferCount; ++i) {
			rhi::ma::AllocationDesc desc{};
			desc.customPool = &pool;
			desc.owner = "Smoke::Defragment";
			if (!Check(allocator.CreateResource(&desc, &kPoolBufferDesc, 0, nullptr, buffers[i]), "create pool buffer")) return false;
			records[i].seed = i;
			buffers[i]->SetOwnerRecord(&records[i]);
		}
		if (!Expect(BlockCount(pool) == kBlockCount, "twelve 1 MiB buffers fill three 4 MiB blocks")) return false;
		if (!Fill(device, allocator, buffers)) return false;

		// Keep two buffers in the first block and one in each other block; free the rest.
		std::vector<rhi::HeapHandle> heaps;
		std::vector<uint32_t> kept(kBlockCount, 0);
		std::vector<AllocationPtr> live;
		for (auto& buffer : buffers) {
			size_t heapIndex = 0;
			while (heapIndex < heaps.size() && !SameHeap(heaps[heapIndex], buffer->GetHeap())) ++heapIndex;
			if (heapIndex == heaps.size()) heaps.push_back(buffer->GetHeap());
			if (!Expect(heapIndex < kBlockCount, "the buffers use three heaps")) return false;
			const uint32_t keep = heapIndex == 0 ? 2u : 1u;
			if (kept[heapIndex] < keep) {
				++kept[heapIndex];
				live.push_back(std::move(buffer));
			} else {
				buffer.Reset();
			}
		}
		if (!Expect(live.size() == 4 && BlockCount(pool) == kBlockCount, "four buffers left over three blocks")) return false;

		// Full passes with CopyRetain until nothing moves.
		rhi::ma::DefragmentationDesc defragDesc{};
		defragDesc.flags = rhi::ma::DefragmentationFlagsAlgorithmFull;
		rhi::ma::DefragmentationContext* context = nullptr;
		if (!Check(pool.BeginDefragmentation(&defragDesc, &context), "Pool::BeginDefragmentation")) return false;

		std::vector<AllocationPtr> retained;
		std::vector<uint32_t> retainedSeeds;
		uint32_t moved = 0;
		bool ok = true;
		for (uint32_t pass = 0; ok && pass < 8; ++pass) {
			rhi::ma::DefragmentationPassMoveInfo info{};
			const rhi::Result begin = context->BeginPass(&info);
			if (begin == rhi::Result::Ok) break;
			if (!Expect(begin == rhi::Result::False && info.moveCount > 0, "BeginPass reports moves")) { ok = false; break; }

			// Make each destination resource and record the copies.
			std::vector<PendingMove> pending(info.moveCount);
			for (uint32_t i = 0; ok && i < info.moveCount; ++i) {
				rhi::ma::DefragmentationMove& move = info.pMoves[i];
				const OwnerRecord* record = move.pSrcAllocation->GetOwnerRecord<OwnerRecord>();
				ok = Expect(record != nullptr && !move.pSrcAllocation->IsDefragmentationRetained(),
					"every move's source is a live buffer with its owner record");
				if (!ok) break;
				pending[i] = { move.pSrcAllocation->GetHeap(), move.pSrcAllocation->GetOffset(), move.pDstTmpAllocation->GetHeap(),
					move.pDstTmpAllocation->GetOffset(), record->seed };
				rhi::ResourcePtr destination;
				ok = Check(allocator.CreateAliasingResource(move.pDstTmpAllocation, 0, &kPoolBufferDesc, 0, nullptr, destination),
					"CreateAliasingResource at the destination");
				if (!ok) break;
				move.pDstTmpAllocation->SetResource(std::move(destination));
				move.operation = rhi::ma::DefragmentationMoveOperation::CopyRetain;
			}
			if (!ok) break;
			ok = RunOnGpu(device, [&](rhi::CommandListPtr& list) {
				for (uint32_t i = 0; i < info.moveCount; ++i) {
					const rhi::ResourceHandle source = info.pMoves[i].pSrcAllocation->GetResource().GetHandle();
					const rhi::ResourceHandle destination = info.pMoves[i].pDstTmpAllocation->GetResource().GetHandle();
					CopyBarrier(list, source, rhi::ResourceAccessType::CopySource);
					CopyBarrier(list, destination, rhi::ResourceAccessType::CopyDest);
					list->CopyBufferRegion(destination, 0, source, 0, kBufferBytes);
				}
			});
			if (!ok) break;

			// Without somewhere to put the old ranges, EndPass refuses and the moves stay pending.
			const uint32_t moveCount = info.moveCount;
			ok = Expect(context->EndPass(&info) == rhi::Result::InvalidArgument && info.moveCount == moveCount,
				"EndPass without pRetained refuses CopyRetain and changes nothing");
			if (!ok) break;

			std::vector<AllocationPtr> passRetained(moveCount);
			std::vector<rhi::ma::Allocation*> sources(moveCount);
			for (uint32_t i = 0; i < moveCount; ++i) sources[i] = info.pMoves[i].pSrcAllocation;
			info.pRetained = passRetained.data();
			context->EndPass(&info);

			for (uint32_t i = 0; ok && i < moveCount; ++i) {
				const AllocationPtr& old = passRetained[i];
				ok = Expect(old && old->IsDefragmentationRetained() && old->GetPrivateData() == nullptr, "a CopyRetain move hands its old range back") &&
					Expect(SameHeap(old->GetHeap(), pending[i].srcHeap) && old->GetOffset() == pending[i].srcOffset,
						"the retained allocation holds the old range") &&
					Expect(SameHeap(sources[i]->GetHeap(), pending[i].dstHeap) && sources[i]->GetOffset() == pending[i].dstOffset,
						"the moved allocation names the new range") &&
					Expect(sources[i]->GetOwnerRecord<OwnerRecord>()->seed == pending[i].seed, "the moved allocation keeps its owner record");
			}
			for (uint32_t i = 0; i < moveCount; ++i) {
				retained.push_back(std::move(passRetained[i]));
				retainedSeeds.push_back(pending[i].seed);
			}
			moved += moveCount;
			if (ok) ok = Expect(BlockCount(pool) == kBlockCount, "no block is freed while retained allocations hold it");
		}

		rhi::ma::DefragmentationStats stats{};
		context->GetStats(&stats);
		context->ReleaseThis();
		if (!ok) return false;
		std::printf("  %u moves, %llu bytes moved, %u heaps freed during the passes\n", stats.allocationsMoved,
			static_cast<unsigned long long>(stats.bytesMoved), stats.heapsFreed);
		if (!Expect(moved > 0 && stats.allocationsMoved == moved && stats.heapsFreed == 0, "the passes moved buffers and freed no heap")) {
			return false;
		}

		// The live buffers share one block and read back through their new resources. The old resources still hold the data.
		for (const auto& buffer : live) {
			if (!Expect(SameHeap(buffer->GetHeap(), live.front()->GetHeap()), "the live buffers are compacted into one block")) return false;
		}
		std::vector<Readable> liveItems, retainedItems;
		for (const auto& buffer : live) liveItems.push_back({ &buffer->GetResource(), buffer->GetOwnerRecord<OwnerRecord>()->seed });
		for (size_t i = 0; i < retained.size(); ++i) retainedItems.push_back({ &retained[i]->GetResource(), retainedSeeds[i] });
		if (!Verify(device, allocator, liveItems, "the moved buffers read back their contents") ||
			!Verify(device, allocator, retainedItems, "the retained old buffers still read back their contents")) {
			return false;
		}

		// Dropping the retained allocations frees the emptied blocks. The vector may keep one empty block.
		retained.clear();
		const uint32_t afterRetained = BlockCount(pool);
		std::printf("  blocks: %u while retained, %u after release\n", kBlockCount, afterRetained);
		if (!Expect(afterRetained < kBlockCount && afterRetained >= 1 && afterRetained <= 2, "the emptied blocks go with the retained allocations") ||
			!Verify(device, allocator, liveItems, "the moved buffers are intact after the release")) {
			return false;
		}

		live.clear();
		return Expect(BlockCount(pool) <= 1, "at most one empty block stays once everything is freed");
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
	rhi::ma::PoolDesc poolDesc{};
	poolDesc.heapType = rhi::HeapType::DeviceLocal;
	poolDesc.blockSize = kBlockBytes;
	poolDesc.debugName = "AllocatorDefragmentSmoke pool";
	rhi::ma::Pool* pool = nullptr;
	bool ok = Check(allocator->CreatePool(&poolDesc, &pool), "CreatePool");
	if (ok) {
		ok = Run(device.Get(), *allocator, *pool);
		pool->ReleaseThis();
	}
	allocator->ReleaseThis();
	device->CheckDebugMessages();
	if (!ok) {
		return 1;
	}
	std::printf("BasicRHIAllocatorDefragmentSmoke (%s): success\n", backendName.c_str());
	return 0;
}
