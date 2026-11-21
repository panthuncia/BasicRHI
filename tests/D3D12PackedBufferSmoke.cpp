// Packed D3D12 buffers: small placed buffers are byte ranges of a buffer over their heap (rhi_dx12.cpp, Dx12Packs). Checks the
// reported placement, addresses, maps, copies (one of them between two buffers of one heap), buffer-texture copies, views and
// barriers, with the debug layer on.
#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
	int g_failures = 0;

	void Expect(bool a_ok, const char* a_what) {
		if (!a_ok) {
			std::fprintf(stderr, "FAILED: %s\n", a_what);
			++g_failures;
		}
	}

	bool Check(rhi::Result result, const char* operation) {
		if (rhi::IsOk(result))
			return true;
		std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
		++g_failures;
		return false;
	}

	UINT64 MessageCount(ID3D12Device* a_device) {
		ID3D12InfoQueue* queue = nullptr;
		if (!a_device || FAILED(a_device->QueryInterface(IID_PPV_ARGS(&queue))))
			return 0;
		const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
		queue->Release();
		return count;
	}

	bool HasDebugLayerErrors(ID3D12Device* a_device, UINT64 a_first) {
		ID3D12InfoQueue* queue = nullptr;
		if (!a_device || FAILED(a_device->QueryInterface(IID_PPV_ARGS(&queue)))) {
			std::fprintf(stderr, "D3D12 InfoQueue unavailable\n");
			return true;
		}
		bool errors = false;
		const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
		for (UINT64 i = a_first; i < count; ++i) {
			SIZE_T bytes = 0;
			if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes == 0)
				continue;
			std::vector<std::byte> storage(bytes);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if (FAILED(queue->GetMessage(i, message, &bytes)))
				continue;
			if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
				errors = true;
				std::fprintf(stderr, "D3D12 validation error %u: %s\n", message->ID, message->pDescription);
			}
		}
		queue->Release();
		return errors;
	}

	void BufferBarrier(rhi::CommandList& a_list, rhi::ResourceHandle a_buffer, rhi::ResourceAccessType a_before, rhi::ResourceAccessType a_after) {
		rhi::BufferBarrier barrier{};
		barrier.buffer = a_buffer;
		barrier.beforeSync = rhi::ResourceSyncState::All;
		barrier.afterSync = rhi::ResourceSyncState::All;
		barrier.beforeAccess = a_before;
		barrier.afterAccess = a_after;
		a_list.Barriers(rhi::BarrierBatch{ .buffers = { &barrier, 1 } });
	}

	void TextureBarrier(rhi::CommandList& a_list, rhi::ResourceHandle a_texture, rhi::ResourceAccessType a_beforeAccess, rhi::ResourceAccessType a_afterAccess,
		rhi::ResourceLayout a_beforeLayout, rhi::ResourceLayout a_afterLayout) {
		rhi::TextureBarrier barrier{
			.texture = a_texture,
			.range = { 0, 1, 0, 1, 0, 1 },
			.beforeSync = rhi::ResourceSyncState::All,
			.afterSync = rhi::ResourceSyncState::All,
			.beforeAccess = a_beforeAccess,
			.afterAccess = a_afterAccess,
			.beforeLayout = a_beforeLayout,
			.afterLayout = a_afterLayout,
			.discard = a_beforeLayout == rhi::ResourceLayout::Undefined,
		};
		a_list.Barriers(rhi::BarrierBatch{ .textures = { &barrier, 1 } });
	}

	uint8_t Pattern(uint32_t a_seed, uint32_t a_index) { return static_cast<uint8_t>((a_index * 131u + a_seed * 17u) & 0xFF); }
}

int main() {
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::D3D12;
	createInfo.enableDebug = true;
	rhi::DevicePtr device;
	if (!Check(rhi::CreateD3D12Device(createInfo, device), "CreateD3D12Device"))
		return 1;
	ID3D12Device* native = rhi::dx12::get_device(device.Get());
	const UINT64 firstMessage = MessageCount(native);

	constexpr uint64_t kBytes = 4096;

	// The reported placement: a small buffer at 512 bytes, a large one and an own-resource one at 64 KiB.
	{
		const auto smallDesc = rhi::helpers::ResourceDesc::Buffer(1000, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess);
		const auto ownDesc = rhi::helpers::ResourceDesc::Buffer(1000, rhi::HeapType::DeviceLocal, rhi::RF_OwnResource);
		const auto large = rhi::helpers::ResourceDesc::Buffer(1024 * 1024, rhi::HeapType::DeviceLocal);
		rhi::ResourceAllocationInfo info[3]{};
		device->GetResourceAllocationInfo(&smallDesc, 1, &info[0]);
		device->GetResourceAllocationInfo(&ownDesc, 1, &info[1]);
		device->GetResourceAllocationInfo(&large, 1, &info[2]);
		Expect(info[0].alignment == 512 && info[0].sizeInBytes == 1024, "a small buffer is placed at 512 bytes and takes 1024");
		Expect(info[1].alignment == 65536 && info[1].sizeInBytes == 65536, "an own-resource buffer keeps 64 KiB");
		Expect(info[2].alignment == 65536, "a large buffer keeps 64 KiB alignment");
		const rhi::ResourceDesc pair[2] = { smallDesc, smallDesc };
		rhi::ResourceAllocationInfo pairInfo[2]{};
		device->GetResourceAllocationInfo(pair, 2, pairInfo);
		Expect(pairInfo[0].offset == 0 && pairInfo[1].offset == 1024, "a set of two small buffers lies one after the other");
	}

	rhi::HeapPtr deviceHeap, uploadHeap, readbackHeap;
	if (!Check(device->CreateHeap({ .sizeBytes = 1ull << 20, .memory = rhi::HeapType::DeviceLocal, .flags = rhi::HeapFlags::AllowOnlyBuffers, .debugName = "packed smoke device heap" }, deviceHeap), "CreateHeap device") ||
		!Check(device->CreateHeap({ .sizeBytes = 1ull << 20, .memory = rhi::HeapType::Upload, .flags = rhi::HeapFlags::AllowOnlyBuffers, .debugName = "packed smoke upload heap" }, uploadHeap), "CreateHeap upload") ||
		!Check(device->CreateHeap({ .sizeBytes = 1ull << 20, .memory = rhi::HeapType::Readback, .flags = rhi::HeapFlags::AllowOnlyBuffers, .debugName = "packed smoke readback heap" }, readbackHeap), "CreateHeap readback"))
		return 1;

	// Two device buffers in one heap at 512-byte starts, an upload and a readback buffer, and an own-resource buffer at 64 KiB.
	rhi::ResourcePtr a, b, upload, readback, own;
	if (!Check(device->CreatePlacedResource(deviceHeap->GetHandle(), 512, rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "packed A"), a), "place A") ||
		!Check(device->CreatePlacedResource(deviceHeap->GetHandle(), 1024 + kBytes, rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, {}, "packed B"), b), "place B") ||
		!Check(device->CreatePlacedResource(uploadHeap->GetHandle(), 1024, rhi::helpers::ResourceDesc::Buffer(2 * kBytes, rhi::HeapType::Upload, {}, "packed upload"), upload), "place upload") ||
		!Check(device->CreatePlacedResource(readbackHeap->GetHandle(), 1536, rhi::helpers::ResourceDesc::Buffer(2 * kBytes, rhi::HeapType::Readback, {}, "packed readback"), readback), "place readback") ||
		!Check(device->CreatePlacedResource(deviceHeap->GetHandle(), 65536, rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, rhi::RF_OwnResource, "own"), own), "place own"))
		return 1;

	// Addresses: the buffers of one heap are their offsets apart.
	const uint64_t addressA = device->GetBufferDeviceAddress({ a->GetHandle(), 0 });
	const uint64_t addressB = device->GetBufferDeviceAddress({ b->GetHandle(), 0 });
	const uint64_t addressOwn = device->GetBufferDeviceAddress({ own->GetHandle(), 0 });
	Expect(addressA != 0 && addressB == addressA + kBytes + 512, "B's address is A's plus their distance in the heap");
	Expect(addressOwn != 0 && addressOwn % 65536 == 0, "the own-resource buffer has a 64 KiB address");
	Expect(rhi::dx12::get_resource(a.Get()) == nullptr, "a packed buffer hands out no native resource");
	Expect(rhi::dx12::get_resource(own.Get()) != nullptr, "an own-resource buffer hands out its native resource");
	uint64_t rangeA = 0, rangeB = 0, rangeOwn = 1;
	ID3D12Resource* sharedA = rhi::dx12::get_buffer_range(a.Get(), rangeA);
	ID3D12Resource* sharedB = rhi::dx12::get_buffer_range(b.Get(), rangeB);
	ID3D12Resource* ownResource = rhi::dx12::get_buffer_range(own.Get(), rangeOwn);
	Expect(sharedA && sharedA == sharedB && rangeA == 512 && rangeB == 1024 + kBytes, "A and B are ranges of one resource at their heap offsets");
	Expect(ownResource && ownResource != sharedA && rangeOwn == 0, "the own-resource buffer is its own resource from 0");

	// Map: two buffers of one upload heap, written through their own pointers.
	void* mapped = nullptr;
	upload->Map(&mapped);
	Expect(mapped != nullptr, "the upload buffer maps");
	if (!mapped)
		return 1;
	auto* bytes = static_cast<uint8_t*>(mapped);
	for (uint32_t i = 0; i < 2 * kBytes; ++i)
		bytes[i] = Pattern(1, i);
	upload->Unmap(0, 2 * kBytes);

	// Views: raw, a dividing structured stride and a CBV work; a stride of 12 at B's start (5120) is refused.
	rhi::DescriptorHeapPtr views;
	if (!Check(device->CreateDescriptorHeap({ rhi::DescriptorHeapType::CbvSrvUav, 8, false, "packed smoke views" }, views), "CreateDescriptorHeap"))
		return 1;
	rhi::SrvDesc raw{};
	raw.dimension = rhi::SrvDim::Buffer;
	raw.buffer.kind = rhi::BufferViewKind::Raw;
	raw.buffer.numElements = kBytes / 4;
	Expect(rhi::IsOk(device->CreateShaderResourceView({ views->GetHandle(), 0 }, b->GetHandle(), raw)), "a raw SRV of B");
	rhi::SrvDesc structured = raw;
	structured.buffer.kind = rhi::BufferViewKind::Structured;
	structured.buffer.structureByteStride = 16;
	structured.buffer.numElements = kBytes / 16;
	Expect(rhi::IsOk(device->CreateShaderResourceView({ views->GetHandle(), 1 }, b->GetHandle(), structured)), "a 16-byte structured SRV of B");
	structured.buffer.structureByteStride = 12;
	structured.buffer.numElements = kBytes / 12;
	Expect(!rhi::IsOk(device->CreateShaderResourceView({ views->GetHandle(), 2 }, b->GetHandle(), structured)), "a 12-byte structured SRV of B is refused");
	rhi::UavDesc uav{};
	uav.dimension = rhi::UavDim::Buffer;
	uav.buffer.kind = rhi::BufferViewKind::Raw;
	uav.buffer.numElements = kBytes / 4;
	Expect(rhi::IsOk(device->CreateUnorderedAccessView({ views->GetHandle(), 3 }, a->GetHandle(), uav)), "a raw UAV of A");
	Expect(!rhi::IsOk(device->CreateUnorderedAccessView({ views->GetHandle(), 4 }, b->GetHandle(), uav)), "a UAV of B (made without unordered access) is refused");
	rhi::CbvDesc cbv{};
	cbv.byteOffset = 256;
	cbv.byteSize = 256;
	Expect(rhi::IsOk(device->CreateConstantBufferView({ views->GetHandle(), 5 }, a->GetHandle(), cbv)), "a CBV of A");
	const auto viewRef = device->GetBufferViewRef({ b->GetHandle(), 0 });
	(void)viewRef;

	// A texture to copy through, from a packed upload buffer and back into a packed readback buffer.
	rhi::ResourcePtr texture;
	if (!Check(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(rhi::Format::R8G8B8A8_UNorm, rhi::HeapType::DeviceLocal, 16, 16, 1, 1, 1,
		rhi::ResourceLayout::Undefined, nullptr, {}, "packed smoke texture"), texture), "CreateCommittedResource texture"))
		return 1;
	rhi::CopyableFootprint footprint{};
	const auto footprints = device->GetCopyableFootprints({ .texture = texture->GetHandle() }, &footprint, 1);
	Expect(footprints.count == 1 && footprint.rowPitch * 16 <= kBytes, "the texture's footprint fits 4 KiB");

	auto queue = device->GetQueue(rhi::QueueKind::Graphics);
	rhi::CommandAllocatorPtr allocator;
	rhi::CommandListPtr list;
	if (!queue || !Check(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator") ||
		!Check(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list), "CreateCommandList"))
		return 1;

	// upload[0..4K) -> A -> B (two buffers of one heap) -> readback[0..4K); upload[4K..8K) -> texture -> readback[4K..8K).
	BufferBarrier(list.Get(), a->GetHandle(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
	list->CopyBufferRegion(a->GetHandle(), 0, upload->GetHandle(), 0, kBytes);
	BufferBarrier(list.Get(), a->GetHandle(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::CopySource);
	BufferBarrier(list.Get(), b->GetHandle(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
	list->CopyBufferRegion(b->GetHandle(), 0, a->GetHandle(), 0, kBytes);
	BufferBarrier(list.Get(), b->GetHandle(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::CopySource);
	list->CopyBufferRegion(readback->GetHandle(), 0, b->GetHandle(), 0, kBytes);
	TextureBarrier(list.Get(), texture->GetHandle(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::CopyDest, rhi::ResourceLayout::Undefined,
		rhi::ResourceLayout::CopyDest);
	rhi::BufferTextureCopyFootprint copy{};
	copy.texture = texture->GetHandle();
	copy.buffer = upload->GetHandle();
	copy.footprint = footprint;
	copy.footprint.offset = kBytes;
	list->CopyBufferToTexture(copy);
	TextureBarrier(list.Get(), texture->GetHandle(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::CopySource, rhi::ResourceLayout::CopyDest,
		rhi::ResourceLayout::CopySource);
	copy.buffer = readback->GetHandle();
	list->CopyTextureToBuffer(copy);
	list->End();
	const rhi::CommandList lists[] = { list.Get() };
	if (!Check(queue.Submit(lists, {}), "Submit") || !Check(device->WaitIdle(), "WaitIdle"))
		return 1;

	void* back = nullptr;
	readback->Map(&back, 0, 2 * kBytes);
	Expect(back != nullptr, "the readback buffer maps");
	if (back) {
		const auto* read = static_cast<const uint8_t*>(back);
		bool sameBuffer = true;
		for (uint32_t i = 0; i < kBytes; ++i)
			sameBuffer = sameBuffer && read[i] == Pattern(1, i);
		Expect(sameBuffer, "the bytes came back through A and B");
		bool sameTexture = true;
		for (uint32_t row = 0; row < 16; ++row)
			for (uint32_t i = 0; i < 16 * 4; ++i)
				sameTexture = sameTexture && read[kBytes + row * footprint.rowPitch + i] == Pattern(1, kBytes + row * footprint.rowPitch + i);
		Expect(sameTexture, "the bytes came back through the texture");
		readback->Unmap(0, 0);
	}

	Expect(!HasDebugLayerErrors(native, firstMessage), "no debug layer errors");
	if (g_failures) {
		std::fprintf(stderr, "BasicRHID3D12PackedBufferSmoke: %d failure(s)\n", g_failures);
		return 1;
	}
	std::puts("BasicRHID3D12PackedBufferSmoke: success");
	return 0;
}
