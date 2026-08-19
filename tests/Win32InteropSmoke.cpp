#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"
#include "rhi_interop_vulkan.h"

#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
bool Check(rhi::Result result, const char* operation) {
    if (rhi::IsOk(result)) return true;
    std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
    return false;
}

void BufferBarrier(rhi::CommandList commandList, rhi::Resource resource,
    rhi::ResourceAccessType before, rhi::ResourceAccessType after,
    rhi::BufferBarrier::ExternalOwnership ownership = rhi::BufferBarrier::ExternalOwnership::None) {
    rhi::BufferBarrier barrier{
        .buffer = resource.GetHandle(),
        .beforeSync = rhi::ResourceSyncState::Copy,
        .afterSync = rhi::ResourceSyncState::Copy,
        .beforeAccess = before,
        .afterAccess = after,
        .externalOwnership = ownership,
    };
    commandList.Barriers({ .buffers = { &barrier, 1 } });
}

void TextureBarrier(rhi::CommandList commandList, rhi::Resource resource,
    rhi::ResourceAccessType beforeAccess, rhi::ResourceAccessType afterAccess,
    rhi::ResourceLayout beforeLayout, rhi::ResourceLayout afterLayout,
    rhi::TextureBarrier::ExternalOwnership ownership = rhi::TextureBarrier::ExternalOwnership::None) {
    rhi::TextureBarrier barrier{
        .texture = resource.GetHandle(),
        .range = { .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 },
        .beforeSync = rhi::ResourceSyncState::Copy,
        .afterSync = rhi::ResourceSyncState::Copy,
        .beforeAccess = beforeAccess,
        .afterAccess = afterAccess,
        .beforeLayout = beforeLayout,
        .afterLayout = afterLayout,
        .externalOwnership = ownership,
    };
    commandList.Barriers({ .textures = { &barrier, 1 } });
}

bool MakeList(rhi::Device& device, rhi::CommandAllocatorPtr& allocator, rhi::CommandListPtr& list) {
    return Check(device.CreateCommandAllocator(rhi::QueueKind::Copy, allocator), "CreateCommandAllocator") &&
        Check(device.CreateCommandList(rhi::QueueKind::Copy, allocator.Get(), list), "CreateCommandList");
}
}

int main() {
    constexpr uint64_t byteCount = 4096;
    std::array<std::byte, byteCount> expected{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::byte>((i * 37u + 11u) & 0xffu);

    rhi::DeviceCreateInfo d3dInfo{};
    d3dInfo.backend = rhi::Backend::D3D12;
    d3dInfo.enableDebug = true;
    d3dInfo.enableExternalInterop = true;
    rhi::DevicePtr d3d;
    if (!Check(rhi::CreateD3D12Device(d3dInfo, d3d, false), "CreateD3D12Device")) return 1;
    const uint64_t luid = rhi::dx12::get_adapter_luid(d3d.Get());
    if (!luid) {
        std::fprintf(stderr, "D3D12 adapter did not expose a LUID\n");
        return 1;
    }

    rhi::DeviceCreateInfo vkInfo{};
    vkInfo.backend = rhi::Backend::Vulkan;
    vkInfo.enableDebug = true;
    vkInfo.enableExternalInterop = true;
    vkInfo.adapterLuid = luid;
    vkInfo.requireAdapterLuid = true;
    rhi::DevicePtr vk;
    std::fprintf(stderr, "Creating Vulkan peer for LUID 0x%llx\n", static_cast<unsigned long long>(luid));
    std::fflush(stderr);
    if (!Check(rhi::CreateVulkanDevice(vkInfo, vk, false), "CreateVulkanDevice")) return 1;
    std::fprintf(stderr, "Vulkan peer created\n");
    if (rhi::vulkan::get_adapter_luid(vk.Get()) != luid) {
        std::fprintf(stderr, "Vulkan physical device LUID did not match D3D12\n");
        return 1;
    }

	for (const auto format : { rhi::Format::R8G8B8A8_UNorm, rhi::Format::R16G16B16A16_Float }) {
		rhi::ResourceDesc textureDesc{};
		textureDesc.type = rhi::ResourceType::Texture2D;
		textureDesc.heapType = rhi::HeapType::DeviceLocal;
		textureDesc.heapFlags = rhi::HeapFlags::Shared;
		textureDesc.texture.format = format;
		textureDesc.texture.width = textureDesc.texture.height = 64;
		textureDesc.texture.depthOrLayers = textureDesc.texture.mipLevels = textureDesc.texture.sampleCount = 1;
		textureDesc.texture.initialLayout = rhi::ResourceLayout::Undefined;
		const auto support = rhi::vulkan::query_d3d12_texture_support(vk.Get(), textureDesc);
		if (!support.supported || !support.importable || !support.dedicatedOnly) {
			std::fprintf(stderr, "Required shared texture format is unsupported\n");
			return 1;
		}
		rhi::ResourcePtr d3Texture, vkTexture;
		if (!Check(d3d->CreateCommittedResource(textureDesc, d3Texture), "Create shared texture")) return 1;
		rhi::dx12::SharedHandle textureHandle{};
		if (!Check(rhi::dx12::export_shared_resource(d3d.Get(), d3Texture.Get(), textureHandle), "Export shared texture")) return 1;
		const auto textureImport = rhi::vulkan::import_d3d12_texture(vk.Get(), textureHandle.value, textureDesc, vkTexture);
		CloseHandle(static_cast<HANDLE>(textureHandle.value));
		if (!Check(textureImport, "Import shared texture")) return 1;
	}
	rhi::ResourceDesc unsupportedTexture{};
	unsupportedTexture.type = rhi::ResourceType::Texture2D;
	unsupportedTexture.heapType = rhi::HeapType::DeviceLocal;
	unsupportedTexture.texture.format = rhi::Format::R8G8B8A8_UNorm;
	unsupportedTexture.texture.width = unsupportedTexture.texture.height = 16;
	unsupportedTexture.texture.depthOrLayers = unsupportedTexture.texture.mipLevels = 1;
	unsupportedTexture.texture.sampleCount = 4;
	if (rhi::vulkan::query_d3d12_texture_support(vk.Get(), unsupportedTexture).supported) {
		std::fprintf(stderr, "MSAA external texture query was expected to be rejected\n");
		return 1;
	}

	rhi::HeapDesc sharedHeapDesc{};
	sharedHeapDesc.sizeBytes = 256u * 1024u;
	sharedHeapDesc.alignment = 64u * 1024u;
	sharedHeapDesc.memory = rhi::HeapType::DeviceLocal;
	sharedHeapDesc.flags = rhi::HeapFlags::Shared | rhi::HeapFlags::AllowOnlyBuffers;
	sharedHeapDesc.debugName = "Interop shared alias heap";
	rhi::HeapPtr d3Heap, vkHeap;
	if (!Check(d3d->CreateHeap(sharedHeapDesc, d3Heap), "Create shared heap")) return 1;
	rhi::dx12::SharedHandle heapHandle{};
	if (!Check(rhi::dx12::export_shared_heap(d3d.Get(), d3Heap.Get(), heapHandle), "Export shared heap")) return 1;
	const auto heapImport = rhi::vulkan::import_d3d12_heap(vk.Get(), heapHandle.value, sharedHeapDesc, vkHeap);
	CloseHandle(static_cast<HANDLE>(heapHandle.value));
	if (!Check(heapImport, "Import shared heap")) return 1;
	auto aliasDesc = rhi::helpers::ResourceDesc::Buffer(4096, rhi::HeapType::DeviceLocal);
	rhi::ResourcePtr d3AliasA, d3AliasB, vkAliasA, vkAliasB;
	if (!Check(d3d->CreatePlacedResource(d3Heap->GetHandle(), 0, aliasDesc, d3AliasA), "Create D3D alias A") ||
		!Check(d3d->CreatePlacedResource(d3Heap->GetHandle(), 0, aliasDesc, d3AliasB), "Create D3D alias B") ||
		!Check(vk->CreatePlacedResource(vkHeap->GetHandle(), 0, aliasDesc, vkAliasA), "Create Vulkan alias A") ||
		!Check(vk->CreatePlacedResource(vkHeap->GetHandle(), 0, aliasDesc, vkAliasB), "Create Vulkan alias B")) return 1;

    auto sharedDesc = rhi::helpers::ResourceDesc::Buffer(byteCount, rhi::HeapType::DeviceLocal, {}, "InteropSharedBuffer");
    sharedDesc.heapFlags = rhi::HeapFlags::Shared;
    rhi::ResourcePtr d3dA, d3dB, vkA, vkB;
    if (!Check(d3d->CreateCommittedResource(sharedDesc, d3dA), "Create shared buffer A") ||
        !Check(d3d->CreateCommittedResource(sharedDesc, d3dB), "Create shared buffer B")) return 1;

    for (auto pair : { std::pair{ &d3dA, &vkA }, std::pair{ &d3dB, &vkB } }) {
        rhi::dx12::SharedHandle handle{};
        if (!Check(rhi::dx12::export_shared_resource(d3d.Get(), pair.first->Get(), handle), "Export shared buffer")) return 1;
        const auto importResult = rhi::vulkan::import_d3d12_buffer(vk.Get(), handle.value, sharedDesc, *pair.second);
        CloseHandle(static_cast<HANDLE>(handle.value));
        if (!Check(importResult, "Import shared buffer")) return 1;
    }

    rhi::ResourcePtr upload, readback;
    if (!Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(byteCount, rhi::HeapType::Upload), upload), "Create upload") ||
        !Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(byteCount, rhi::HeapType::Readback), readback), "Create readback")) return 1;
    void* mapped = nullptr;
    upload->Map(&mapped, 0, byteCount);
    if (!mapped) return 1;
    std::memcpy(mapped, expected.data(), expected.size());
    upload->Unmap(0, byteCount);

    rhi::TimelinePtr d3dBridge, vkBridge, done;
    if (!Check(d3d->CreateTimeline(d3dBridge, 0, "InteropBridge", true), "Create shared fence")) return 1;
    rhi::dx12::SharedHandle fenceHandle{};
    if (!Check(rhi::dx12::export_shared_timeline(d3d.Get(), d3dBridge.Get(), fenceHandle), "Export shared fence")) return 1;
    const auto timelineImport = rhi::vulkan::import_d3d12_timeline(vk.Get(), fenceHandle.value, 0, "InteropBridgeVk", vkBridge);
    CloseHandle(static_cast<HANDLE>(fenceHandle.value));
    if (!Check(timelineImport, "Import shared fence")) return 1;
    if (!Check(d3d->CreateTimeline(done, 0, "InteropDone"), "Create completion fence")) return 1;

    rhi::CommandAllocatorPtr d3dAllocA, d3dAllocB, vkAlloc;
    rhi::CommandListPtr d3dSeed, d3dRead, vkCopy;
    if (!MakeList(d3d.Get(), d3dAllocA, d3dSeed) || !MakeList(d3d.Get(), d3dAllocB, d3dRead) || !MakeList(vk.Get(), vkAlloc, vkCopy)) return 1;

    BufferBarrier(d3dSeed.Get(), upload.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource);
    BufferBarrier(d3dSeed.Get(), d3dA.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
    d3dSeed->CopyBufferRegion(d3dA->GetHandle(), 0, upload->GetHandle(), 0, byteCount);
    BufferBarrier(d3dSeed.Get(), d3dA.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common);
    d3dSeed->End();
    const rhi::CommandList seedLists[] = { d3dSeed.Get() };
    const rhi::TimelinePoint seedSignal{ d3dBridge->GetHandle(), 1 };
    if (!Check(d3d->GetQueue(rhi::QueueKind::Copy).Submit(seedLists, { .signals = { &seedSignal, 1 } }), "Submit D3D seed")) return 1;

    BufferBarrier(vkCopy.Get(), vkA.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource, rhi::BufferBarrier::ExternalOwnership::Acquire);
    BufferBarrier(vkCopy.Get(), vkB.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest, rhi::BufferBarrier::ExternalOwnership::Acquire);
    vkCopy->CopyBufferRegion(vkB->GetHandle(), 0, vkA->GetHandle(), 0, byteCount);
    BufferBarrier(vkCopy.Get(), vkA.Get(), rhi::ResourceAccessType::CopySource, rhi::ResourceAccessType::Common, rhi::BufferBarrier::ExternalOwnership::Release);
    BufferBarrier(vkCopy.Get(), vkB.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common, rhi::BufferBarrier::ExternalOwnership::Release);
    vkCopy->End();
    const rhi::CommandList vkLists[] = { vkCopy.Get() };
    const rhi::TimelinePoint vkWait{ vkBridge->GetHandle(), 1 };
    const rhi::TimelinePoint vkSignal{ vkBridge->GetHandle(), 2 };
    if (!Check(vk->GetQueue(rhi::QueueKind::Copy).Submit(vkLists, { .waits = { &vkWait, 1 }, .signals = { &vkSignal, 1 } }), "Submit Vulkan copy")) return 1;

    auto d3dQueue = d3d->GetQueue(rhi::QueueKind::Copy);
    if (!Check(d3dQueue.Wait({ d3dBridge->GetHandle(), 2 }), "D3D wait for Vulkan")) return 1;
    BufferBarrier(d3dRead.Get(), d3dB.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource);
    BufferBarrier(d3dRead.Get(), readback.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
    d3dRead->CopyBufferRegion(readback->GetHandle(), 0, d3dB->GetHandle(), 0, byteCount);
    d3dRead->End();
    const rhi::CommandList readLists[] = { d3dRead.Get() };
    const rhi::TimelinePoint doneSignal{ done->GetHandle(), 1 };
    if (!Check(d3dQueue.Submit(readLists, { .signals = { &doneSignal, 1 } }), "Submit D3D readback") ||
        !Check(done->HostWait(1, 30000), "Wait for readback")) return 1;
    mapped = nullptr;
    readback->Map(&mapped, 0, byteCount);
    const bool matches = mapped && std::memcmp(mapped, expected.data(), expected.size()) == 0;
    readback->Unmap(0, 0);
    if (!matches) {
        std::fprintf(stderr, "Round-trip buffer contents did not match\n");
        return 1;
    }

    constexpr uint32_t textureExtent = 16;
    constexpr uint32_t textureRowPitch = 256;
    rhi::ResourceDesc transferTextureDesc{};
    transferTextureDesc.type = rhi::ResourceType::Texture2D;
    transferTextureDesc.heapType = rhi::HeapType::DeviceLocal;
    transferTextureDesc.heapFlags = rhi::HeapFlags::Shared;
    transferTextureDesc.texture.format = rhi::Format::R8G8B8A8_UNorm;
    transferTextureDesc.texture.width = transferTextureDesc.texture.height = textureExtent;
    transferTextureDesc.texture.depthOrLayers = transferTextureDesc.texture.mipLevels = transferTextureDesc.texture.sampleCount = 1;
    transferTextureDesc.texture.initialLayout = rhi::ResourceLayout::Undefined;
    rhi::ResourcePtr d3TextureA, d3TextureB, vkTextureA, vkTextureB;
    if (!Check(d3d->CreateCommittedResource(transferTextureDesc, d3TextureA), "Create transfer texture A") ||
        !Check(d3d->CreateCommittedResource(transferTextureDesc, d3TextureB), "Create transfer texture B")) return 1;
    for (auto pair : { std::pair{ &d3TextureA, &vkTextureA }, std::pair{ &d3TextureB, &vkTextureB } }) {
        rhi::dx12::SharedHandle handle{};
        if (!Check(rhi::dx12::export_shared_resource(d3d.Get(), pair.first->Get(), handle), "Export transfer texture")) return 1;
        const auto import = rhi::vulkan::import_d3d12_texture(vk.Get(), handle.value, transferTextureDesc, *pair.second);
        CloseHandle(static_cast<HANDLE>(handle.value));
        if (!Check(import, "Import transfer texture")) return 1;
    }
    rhi::ResourcePtr textureUpload, textureReadback;
    if (!Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(byteCount, rhi::HeapType::Upload), textureUpload), "Create texture upload") ||
        !Check(d3d->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(byteCount, rhi::HeapType::Readback), textureReadback), "Create texture readback")) return 1;
    textureUpload->Map(&mapped, 0, byteCount);
    std::memset(mapped, 0, byteCount);
    for (uint32_t y = 0; y < textureExtent; ++y) {
        auto* row = static_cast<std::byte*>(mapped) + y * textureRowPitch;
        for (uint32_t x = 0; x < textureExtent * 4; ++x) row[x] = static_cast<std::byte>((x * 13u + y * 29u + 7u) & 0xffu);
    }
    textureUpload->Unmap(0, byteCount);
    rhi::CommandAllocatorPtr d3TextureSeedAlloc, vkTextureCopyAlloc, d3TextureReadAlloc;
    rhi::CommandListPtr d3TextureSeed, vkTextureCopy, d3TextureRead;
    if (!MakeList(d3d.Get(), d3TextureSeedAlloc, d3TextureSeed) ||
        !MakeList(vk.Get(), vkTextureCopyAlloc, vkTextureCopy) ||
        !MakeList(d3d.Get(), d3TextureReadAlloc, d3TextureRead)) return 1;
    BufferBarrier(d3TextureSeed.Get(), textureUpload.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource);
    TextureBarrier(d3TextureSeed.Get(), d3TextureA.Get(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::CopyDest,
        rhi::ResourceLayout::Undefined, rhi::ResourceLayout::CopyDest);
    TextureBarrier(d3TextureSeed.Get(), d3TextureB.Get(), rhi::ResourceAccessType::None, rhi::ResourceAccessType::Common,
        rhi::ResourceLayout::Undefined, rhi::ResourceLayout::Common);
    d3TextureSeed->CopyBufferToTexture({ .texture = d3TextureA->GetHandle(), .buffer = textureUpload->GetHandle(),
        .mip = 0, .arraySlice = 0, .footprint = { .offset = 0, .rowPitch = textureRowPitch,
            .width = textureExtent, .height = textureExtent, .depth = 1 } });
    TextureBarrier(d3TextureSeed.Get(), d3TextureA.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common,
        rhi::ResourceLayout::CopyDest, rhi::ResourceLayout::Common);
    d3TextureSeed->End();
    const rhi::CommandList d3TextureSeedLists[] = { d3TextureSeed.Get() };
    const rhi::TimelinePoint textureSeedSignal{ d3dBridge->GetHandle(), 3 };
    if (!Check(d3dQueue.Submit(d3TextureSeedLists, { .signals = { &textureSeedSignal, 1 } }), "Submit D3D texture seed")) return 1;

    TextureBarrier(vkTextureCopy.Get(), vkTextureA.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource,
        rhi::ResourceLayout::Common, rhi::ResourceLayout::CopySource, rhi::TextureBarrier::ExternalOwnership::Acquire);
    TextureBarrier(vkTextureCopy.Get(), vkTextureB.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest,
        rhi::ResourceLayout::Common, rhi::ResourceLayout::CopyDest, rhi::TextureBarrier::ExternalOwnership::Acquire);
    vkTextureCopy->CopyTextureRegion(
        { .texture = vkTextureB->GetHandle(), .width = textureExtent, .height = textureExtent },
        { .texture = vkTextureA->GetHandle(), .width = textureExtent, .height = textureExtent });
    TextureBarrier(vkTextureCopy.Get(), vkTextureA.Get(), rhi::ResourceAccessType::CopySource, rhi::ResourceAccessType::Common,
        rhi::ResourceLayout::CopySource, rhi::ResourceLayout::Common, rhi::TextureBarrier::ExternalOwnership::Release);
    TextureBarrier(vkTextureCopy.Get(), vkTextureB.Get(), rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::Common,
        rhi::ResourceLayout::CopyDest, rhi::ResourceLayout::Common, rhi::TextureBarrier::ExternalOwnership::Release);
    vkTextureCopy->End();
    const rhi::CommandList vkTextureLists[] = { vkTextureCopy.Get() };
    const rhi::TimelinePoint textureVkWait{ vkBridge->GetHandle(), 3 };
    const rhi::TimelinePoint textureVkSignal{ vkBridge->GetHandle(), 4 };
    if (!Check(vk->GetQueue(rhi::QueueKind::Copy).Submit(vkTextureLists,
        { .waits = { &textureVkWait, 1 }, .signals = { &textureVkSignal, 1 } }), "Submit Vulkan texture copy")) return 1;
    if (!Check(d3dQueue.Wait({ d3dBridge->GetHandle(), 4 }), "D3D wait for Vulkan texture")) return 1;
    TextureBarrier(d3TextureRead.Get(), d3TextureB.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopySource,
        rhi::ResourceLayout::Common, rhi::ResourceLayout::CopySource);
    BufferBarrier(d3TextureRead.Get(), textureReadback.Get(), rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
    d3TextureRead->CopyTextureToBuffer({ .texture = d3TextureB->GetHandle(), .buffer = textureReadback->GetHandle(),
        .mip = 0, .arraySlice = 0, .footprint = { .offset = 0, .rowPitch = textureRowPitch,
            .width = textureExtent, .height = textureExtent, .depth = 1 } });
    d3TextureRead->End();
    const rhi::CommandList d3TextureReadLists[] = { d3TextureRead.Get() };
    const rhi::TimelinePoint textureDoneSignal{ done->GetHandle(), 2 };
    if (!Check(d3dQueue.Submit(d3TextureReadLists, { .signals = { &textureDoneSignal, 1 } }), "Submit D3D texture readback") ||
        !Check(done->HostWait(2, 30000), "Wait for texture readback")) return 1;
    void* textureActual = nullptr;
    void* textureExpected = nullptr;
    textureReadback->Map(&textureActual, 0, byteCount);
    textureUpload->Map(&textureExpected, 0, byteCount);
    bool textureMatches = textureActual && textureExpected;
    for (uint32_t y = 0; textureMatches && y < textureExtent; ++y) {
        textureMatches = std::memcmp(static_cast<std::byte*>(textureActual) + y * textureRowPitch,
            static_cast<std::byte*>(textureExpected) + y * textureRowPitch, textureExtent * 4) == 0;
    }
    textureReadback->Unmap(0, 0);
    textureUpload->Unmap(0, 0);
    if (!textureMatches) {
        std::fprintf(stderr, "Round-trip texture contents did not match\n");
        return 1;
    }
    if (!Check(vk->WaitIdle(), "Wait for Vulkan idle") || !Check(d3d->WaitIdle(), "Wait for D3D12 idle")) return 1;
    std::puts("BasicRHIWin32InteropSmoke: success");
    return 0;
}
