// A D3D12 heap exported to Vulkan as alias memory: resources placed at one offset on both APIs see the same bytes, and a
// later resource placed over an earlier one (an aliasing occupant switch) does too, in both directions. This is what an alias
// pool shared between a D3D12 and a Vulkan device needs.
#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"
#include "rhi_interop_vulkan.h"

#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
bool Check(rhi::Result result, const char* operation) {
	if (rhi::IsOk(result)) return true;
	std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
	return false;
}

void BufferBarrier(rhi::CommandList list, rhi::Resource resource, rhi::ResourceAccessType before, rhi::ResourceAccessType after) {
	rhi::BufferBarrier barrier{ .buffer = resource.GetHandle(), .beforeSync = rhi::ResourceSyncState::Copy,
		.afterSync = rhi::ResourceSyncState::Copy, .beforeAccess = before, .afterAccess = after };
	list.Barriers({ .buffers = { &barrier, 1 } });
}

uint32_t g_mips = 1; // the current case's mip count: barriers cover every mip

void TextureBarrier(rhi::CommandList list, rhi::Resource resource, rhi::ResourceAccessType beforeAccess,
	rhi::ResourceAccessType afterAccess, rhi::ResourceLayout beforeLayout, rhi::ResourceLayout afterLayout,
	rhi::TextureBarrier::ExternalOwnership ownership = rhi::TextureBarrier::ExternalOwnership::None, bool discard = false) {
	rhi::TextureBarrier barrier{ .texture = resource.GetHandle(), .range = { .baseMip = 0, .mipCount = g_mips, .baseLayer = 0, .layerCount = 1 },
		.beforeSync = rhi::ResourceSyncState::Copy, .afterSync = rhi::ResourceSyncState::Copy, .beforeAccess = beforeAccess,
		.afterAccess = afterAccess, .beforeLayout = beforeLayout, .afterLayout = afterLayout, .discard = discard,
		.externalOwnership = ownership };
	list.Barriers({ .textures = { &barrier, 1 } });
}

struct List {
	rhi::CommandAllocatorPtr allocator;
	rhi::CommandListPtr list;
};
bool MakeList(rhi::Device& device, List& out) {
	return Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, out.allocator), "CreateCommandAllocator") &&
		Check(device.CreateCommandList(rhi::QueueKind::Graphics, out.allocator.Get(), out.list), "CreateCommandList");
}

struct Case {
	const char* name;
	rhi::HeapFlags heapFlags;
	rhi::Format format;
	uint32_t bytesPerPixel;
	rhi::ResourceFlags resourceFlags;
	uint32_t width = 64, height = 64;
	uint32_t mips = 1;
};
}

int main() {
	rhi::DeviceCreateInfo d3dInfo{};
	d3dInfo.backend = rhi::Backend::D3D12;
	d3dInfo.enableDebug = true;
	d3dInfo.enableExternalInterop = true;
	rhi::DevicePtr d3d;
	if (!Check(rhi::CreateD3D12Device(d3dInfo, d3d, false), "CreateD3D12Device")) return 1;
	rhi::DeviceCreateInfo vkInfo{};
	vkInfo.backend = rhi::Backend::Vulkan;
	vkInfo.enableDebug = true;
	vkInfo.enableExternalInterop = true;
	vkInfo.adapterLuid = rhi::dx12::get_adapter_luid(d3d.Get());
	vkInfo.requireAdapterLuid = true;
	rhi::DevicePtr vk;
	if (!Check(rhi::CreateVulkanDevice(vkInfo, vk, false), "CreateVulkanDevice")) return 1;

	rhi::TimelinePtr d3dBridge, vkBridge, done;
	if (!Check(d3d->CreateTimeline(d3dBridge, 0, "SharedHeapBridge", true), "Create shared fence")) return 1;
	rhi::ExternalHandle fenceHandle{};
	if (!Check(rhi::dx12::export_shared_timeline(d3d.Get(), d3dBridge.Get(), fenceHandle), "Export shared fence") ||
		!Check(rhi::vulkan::import_d3d12_timeline(vk.Get(), fenceHandle, 0, "SharedHeapBridgeVk", vkBridge), "Import shared fence") ||
		!Check(d3d->CreateTimeline(done, 0, "SharedHeapDone"), "Create completion fence")) return 1;
	uint64_t point = 0;
	auto d3dQueue = d3d->GetQueue(rhi::QueueKind::Graphics);
	auto vkQueue = vk->GetQueue(rhi::QueueKind::Graphics);
	std::vector<List> keep;

	const Case cases[] = {
		// Render target and depth textures are left out: D3D12 requires a placed one to be initialized by D3D12 after each
		// occupant switch, which a first use on Vulkan does not do.
		{ "RGBA16F UAV, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R16G16B16A16_Float, 8,
			rhi::ResourceFlags::RF_AllowUnorderedAccess },
		{ "R8 uint UAV, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R8_UInt, 1,
			rhi::ResourceFlags::RF_AllowUnorderedAccess },
		{ "RGBA8 UAV, heap for all resources", rhi::HeapFlags::None, rhi::Format::R8G8B8A8_UNorm, 4,
			rhi::ResourceFlags::RF_AllowUnorderedAccess },
		{ "RGBA8 UAV, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R8G8B8A8_UNorm, 4,
			rhi::ResourceFlags::RF_AllowUnorderedAccess },
		// Screen-sized, as the renderer's targets are: a driver may lay out large surfaces differently from small ones.
		{ "R8 uint UAV 3200x1800, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R8_UInt, 1,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 3200, 1800 },
		{ "R8 uint UAV 2560x1440, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R8_UInt, 1,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 2560, 1440 },
		{ "RGBA16F UAV 3200x1800, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R16G16B16A16_Float, 8,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 3200, 1800 },
		// Mip chains: every mip must be at the same place on both APIs (GTAO's working depths are R32F with 5 mips).
		{ "R32F UAV 3200x1800 5 mips, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R32_Float, 4,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 3200, 1800, 5 },
		{ "R32F UAV 2560x1440 5 mips, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R32_Float, 4,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 2560, 1440, 5 },
		{ "RGBA8 UAV 256x256 9 mips, non-RT/DS heap", rhi::HeapFlags::AllowOnlyNonRtDsTextures, rhi::Format::R8G8B8A8_UNorm, 4,
			rhi::ResourceFlags::RF_AllowUnorderedAccess, 256, 256, 9 },
	};
	int failures = 0;
	for (const auto& testCase : cases) {
		const uint32_t width = testCase.width, height = testCase.height;
		g_mips = testCase.mips;
		// One footprint per mip, packed into a staging region per round.
		std::vector<rhi::CopyableFootprint> footprints;
		uint64_t stagingBytes = 0;
		for (uint32_t mip = 0; mip < testCase.mips; ++mip) {
			const uint32_t w = (std::max)(1u, width >> mip), h = (std::max)(1u, height >> mip);
			const uint32_t pitch = (w * testCase.bytesPerPixel + 255u) & ~255u;
			footprints.push_back({ .offset = stagingBytes, .rowPitch = pitch, .width = w, .height = h, .depth = 1 });
			stagingBytes = (stagingBytes + uint64_t{ pitch } * h + 511u) & ~uint64_t{ 511 };
		}
		rhi::ResourceDesc desc{};
		desc.type = rhi::ResourceType::Texture2D;
		desc.heapType = rhi::HeapType::DeviceLocal;
		desc.resourceFlags = testCase.resourceFlags;
		desc.texture.format = testCase.format;
		desc.texture.width = width;
		desc.texture.height = height;
		desc.texture.depthOrLayers = desc.texture.sampleCount = 1;
		desc.texture.mipLevels = static_cast<uint16_t>(testCase.mips);
		desc.texture.initialLayout = rhi::ResourceLayout::Undefined;
		if (!rhi::vulkan::query_d3d12_heap_texture_support(vk.Get(), desc).supported) {
			std::fprintf(stderr, "[%s] query_d3d12_heap_texture_support reports the texture cannot be placed in an imported heap\n", testCase.name);
			++failures;
			continue;
		}
		rhi::ResourceAllocationInfo d3dInfoSize{}, vkInfoSize{};
		d3d->GetResourceAllocationInfo(&desc, 1, &d3dInfoSize);
		vk->GetResourceAllocationInfo(&desc, 1, &vkInfoSize);
		const uint64_t alignment = (std::max)({ d3dInfoSize.alignment, vkInfoSize.alignment, uint64_t{ 64 * 1024 } });
		const uint64_t slot = (((std::max)(d3dInfoSize.sizeInBytes, vkInfoSize.sizeInBytes) + alignment - 1) / alignment) * alignment;
		std::fprintf(stderr, "[%s] D3D12 size=%llu align=%llu, Vulkan size=%llu align=%llu\n", testCase.name,
			static_cast<unsigned long long>(d3dInfoSize.sizeInBytes), static_cast<unsigned long long>(d3dInfoSize.alignment),
			static_cast<unsigned long long>(vkInfoSize.sizeInBytes), static_cast<unsigned long long>(vkInfoSize.alignment));

		rhi::HeapDesc heapDesc{};
		heapDesc.sizeBytes = slot * 2;
		heapDesc.alignment = 64 * 1024;
		heapDesc.memory = rhi::HeapType::DeviceLocal;
		heapDesc.flags = testCase.heapFlags | rhi::HeapFlags::Shared;
		heapDesc.debugName = "Shared alias heap";
		rhi::HeapPtr d3Heap, vkHeap;
		rhi::ExternalHandle heapHandle{};
		if (!Check(d3d->CreateHeap(heapDesc, d3Heap), "Create shared heap") ||
			!Check(rhi::dx12::export_shared_heap(d3d.Get(), d3Heap.Get(), heapHandle), "Export shared heap") ||
			!Check(rhi::vulkan::import_d3d12_heap(vk.Get(), heapHandle, heapDesc, vkHeap), "Import shared heap")) {
			++failures;
			continue;
		}
		// Slot 0: the first occupant A, then an aliasing occupant C. Slot 1: B, which Vulkan fills from slot 0.
		rhi::ResourcePtr d3A, vkA, d3B, vkB, d3C, vkC;
		if (!Check(d3d->CreatePlacedResource(d3Heap->GetHandle(), 0, desc, d3A), "Place D3D12 A") ||
			!Check(vk->CreatePlacedResource(vkHeap->GetHandle(), 0, desc, vkA), "Place Vulkan A") ||
			!Check(d3d->CreatePlacedResource(d3Heap->GetHandle(), slot, desc, d3B), "Place D3D12 B") ||
			!Check(vk->CreatePlacedResource(vkHeap->GetHandle(), slot, desc, vkB), "Place Vulkan B") ||
			!Check(d3d->CreatePlacedResource(d3Heap->GetHandle(), 0, desc, d3C), "Place D3D12 C") ||
			!Check(vk->CreatePlacedResource(vkHeap->GetHandle(), 0, desc, vkC), "Place Vulkan C")) {
			++failures;
			continue;
		}
		rhi::ResourcePtr upload, readback;
		if (!Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(stagingBytes * 2, rhi::HeapType::Upload), upload), "Create upload") ||
			!Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(stagingBytes * 2, rhi::HeapType::Readback), readback), "Create readback")) return 1;
		void* mapped = nullptr;
		upload->Map(&mapped, 0, stagingBytes * 2);
		auto* bytes = static_cast<std::byte*>(mapped);
		for (uint64_t i = 0; i < stagingBytes * 2; ++i) bytes[i] = static_cast<std::byte>((i * 37u + (i / stagingBytes) * 101u + 11u) & 0x7fu);
		upload->Unmap(0, stagingBytes * 2);

		// Round 1: D3D12 writes A; Vulkan copies A into B; D3D12 reads B. Round 2 (the occupant switch): D3D12 writes C over A;
		// Vulkan copies C into B; D3D12 reads B.
		bool ok = true;
		for (int round = 0; round < 2 && ok; ++round) {
			auto& d3Source = round == 0 ? d3A : d3C;
			auto& vkSource = round == 0 ? vkA : vkC;
			List seed, copy, read;
			if (!MakeList(d3d.Get(), seed) || !MakeList(vk.Get(), copy) || !MakeList(d3d.Get(), read)) return 1;
			BufferBarrier(seed.list.Get(), upload.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource);
			// B is only ever written by Vulkan: give its D3D12 view a layout before D3D12 first reads it.
			if (round == 0)
				TextureBarrier(seed.list.Get(), d3B.Get(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::Common,
					rhi::ResourceLayout::Undefined, rhi::ResourceLayout::Common);
			TextureBarrier(seed.list.Get(), d3Source.Get(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::CopyDest,
				rhi::ResourceLayout::Undefined, rhi::ResourceLayout::CopyDest, rhi::TextureBarrier::ExternalOwnership::None, true);
			for (uint32_t mip = 0; mip < testCase.mips; ++mip) {
				auto sourceFootprint = footprints[mip];
				sourceFootprint.offset += round * stagingBytes;
				seed.list->CopyBufferToTexture({ .texture = d3Source->GetHandle(), .buffer = upload->GetHandle(), .mip = mip, .arraySlice = 0,
					.footprint = sourceFootprint });
			}
			TextureBarrier(seed.list.Get(), d3Source.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common,
				rhi::ResourceLayout::CopyDest, rhi::ResourceLayout::Common);
			seed.list->End();
			const rhi::CommandList seedLists[] = { seed.list.Get() };
			const rhi::TimelinePoint seeded{ d3dBridge->GetHandle(), ++point };
			if (!Check(d3dQueue.Submit(seedLists, { .signals = { &seeded, 1 } }), "Submit D3D12 seed")) return 1;

			TextureBarrier(copy.list.Get(), vkSource.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource,
				rhi::ResourceLayout::Common, rhi::ResourceLayout::CopySource, rhi::TextureBarrier::ExternalOwnership::Acquire);
			TextureBarrier(copy.list.Get(), vkB.Get(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::CopyDest,
				rhi::ResourceLayout::Undefined, rhi::ResourceLayout::CopyDest, rhi::TextureBarrier::ExternalOwnership::Acquire);
			for (uint32_t mip = 0; mip < testCase.mips; ++mip)
				copy.list->CopyTextureRegion({ .texture = vkB->GetHandle(), .mip = mip, .width = footprints[mip].width, .height = footprints[mip].height },
					{ .texture = vkSource->GetHandle(), .mip = mip, .width = footprints[mip].width, .height = footprints[mip].height });
			TextureBarrier(copy.list.Get(), vkSource.Get(), rhi::ResourceAccessType::CopySource, rhi::ResourceAccessType::Common,
				rhi::ResourceLayout::CopySource, rhi::ResourceLayout::Common, rhi::TextureBarrier::ExternalOwnership::Release);
			TextureBarrier(copy.list.Get(), vkB.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common,
				rhi::ResourceLayout::CopyDest, rhi::ResourceLayout::Common, rhi::TextureBarrier::ExternalOwnership::Release);
			copy.list->End();
			const rhi::CommandList copyLists[] = { copy.list.Get() };
			const rhi::TimelinePoint copyWait{ vkBridge->GetHandle(), point };
			const rhi::TimelinePoint copied{ vkBridge->GetHandle(), ++point };
			if (!Check(vkQueue.Submit(copyLists, { .waits = { &copyWait, 1 }, .signals = { &copied, 1 } }), "Submit Vulkan copy")) return 1;

			if (!Check(d3dQueue.Wait({ d3dBridge->GetHandle(), point }), "D3D12 wait for Vulkan")) return 1;
			TextureBarrier(read.list.Get(), d3B.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource,
				rhi::ResourceLayout::Common, rhi::ResourceLayout::CopySource);
			BufferBarrier(read.list.Get(), readback.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
			for (uint32_t mip = 0; mip < testCase.mips; ++mip) {
				auto readFootprint = footprints[mip];
				readFootprint.offset += round * stagingBytes;
				read.list->CopyTextureToBuffer({ .texture = d3B->GetHandle(), .buffer = readback->GetHandle(), .mip = mip, .arraySlice = 0,
					.footprint = readFootprint });
			}
			TextureBarrier(read.list.Get(), d3B.Get(), rhi::ResourceAccessType::CopySource, rhi::ResourceAccessType::Common,
				rhi::ResourceLayout::CopySource, rhi::ResourceLayout::Common);
			read.list->End();
			const rhi::CommandList readLists[] = { read.list.Get() };
			const uint64_t finished = round + 1 + (&testCase - cases) * 2;
			const rhi::TimelinePoint doneSignal{ done->GetHandle(), finished };
			if (!Check(d3dQueue.Submit(readLists, { .signals = { &doneSignal, 1 } }), "Submit D3D12 readback") ||
				!Check(done->HostWait(finished, 30000), "Wait for readback")) return 1;
			keep.push_back(std::move(seed)); keep.push_back(std::move(copy)); keep.push_back(std::move(read));

			void* actual = nullptr;
			void* expected = nullptr;
			readback->Map(&actual, 0, stagingBytes * 2);
			upload->Map(&expected, 0, stagingBytes * 2);
			for (uint32_t mip = 0; mip < testCase.mips; ++mip) {
				const auto& fp = footprints[mip];
				bool mipOk = true;
				for (uint32_t y = 0; mipOk && y < fp.height; ++y) {
					const uint64_t offset = round * stagingBytes + fp.offset + uint64_t{ y } * fp.rowPitch;
					mipOk = std::memcmp(static_cast<std::byte*>(actual) + offset, static_cast<std::byte*>(expected) + offset,
						fp.width * testCase.bytesPerPixel) == 0;
				}
				if (!mipOk) std::fprintf(stderr, "[%s] round %d: mip %u differs\n", testCase.name, round, mip);
				ok = ok && mipOk;
			}
			readback->Unmap(0, 0);
			upload->Unmap(0, 0);
			std::fprintf(stderr, "[%s] round %d (%s): %s\n", testCase.name, round, round == 0 ? "first occupant" : "aliasing occupant",
				ok ? "match" : "MISMATCH");
		}
		if (!ok) ++failures;
		if (!Check(vk->WaitIdle(), "Vulkan idle") || !Check(d3d->WaitIdle(), "D3D12 idle")) return 1;
	}
	if (failures) {
		std::fprintf(stderr, "BasicRHIWin32SharedHeapAliasSmoke: %d case(s) failed\n", failures);
		return 1;
	}
	std::puts("BasicRHIWin32SharedHeapAliasSmoke: success");
	return 0;
}
