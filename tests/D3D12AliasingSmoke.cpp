#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"

#include <cstdio>
#include <vector>

namespace {
	bool Check(rhi::Result result, const char* operation) {
		if (rhi::IsOk(result)) {
			return true;
		}
		std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
		return false;
	}

	bool SubmitAndWait(rhi::Device& device, rhi::Queue queue, rhi::CommandList commandList) {
		const rhi::CommandList lists[] = { commandList };
		return Check(queue.Submit(lists, {}), "Queue::Submit")
			&& Check(device.WaitIdle(), "Device::WaitIdle");
	}

	void TextureBarrier(
		rhi::CommandList& commandList,
		rhi::ResourceHandle texture,
		rhi::TextureSubresourceRange range,
		rhi::ResourceAccessType beforeAccess,
		rhi::ResourceAccessType afterAccess,
		rhi::ResourceLayout beforeLayout,
		rhi::ResourceLayout afterLayout,
		bool discard) {
		rhi::TextureBarrier barrier{
			.texture = texture,
			.range = range,
			.beforeSync = discard ? rhi::ResourceSyncState::None : rhi::ResourceSyncState::All,
			.afterSync = rhi::ResourceSyncState::All,
			.beforeAccess = beforeAccess,
			.afterAccess = afterAccess,
			.beforeLayout = beforeLayout,
			.afterLayout = afterLayout,
			.discard = discard,
		};
		commandList.Barriers(rhi::BarrierBatch{ .textures = { &barrier, 1 } });
	}

	bool HasDebugLayerErrors(ID3D12Device* nativeDevice, UINT64 firstMessage) {
		ID3D12InfoQueue* infoQueue = nullptr;
		if (!nativeDevice || FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
			std::fprintf(stderr, "D3D12 InfoQueue unavailable\n");
			return true;
		}

		bool hasErrors = false;
		const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		for (UINT64 index = firstMessage; index < messageCount; ++index) {
			SIZE_T bytes = 0;
			if (FAILED(infoQueue->GetMessage(index, nullptr, &bytes)) || bytes == 0) {
				continue;
			}
			std::vector<std::byte> storage(bytes);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if (FAILED(infoQueue->GetMessage(index, message, &bytes))) {
				continue;
			}
			if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR
				|| message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
				hasErrors = true;
				std::fprintf(stderr, "D3D12 validation error %u: %s\n", message->ID, message->pDescription);
			}
		}
		infoQueue->Release();
		return hasErrors;
	}
}

int main() {
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::D3D12;
	createInfo.enableDebug = true;

	rhi::DevicePtr device;
	if (!Check(rhi::CreateD3D12Device(createInfo, device), "CreateD3D12Device")) {
		return 1;
	}

	ID3D12Device* nativeDevice = rhi::dx12::get_device(device.Get());
	ID3D12InfoQueue* infoQueue = nullptr;
	UINT64 firstMessage = 0;
	if (nativeDevice && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
		firstMessage = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		infoQueue->Release();
	}

	const auto textureDesc = rhi::helpers::ResourceDesc::Tex2D(
		rhi::Format::R16G16B16A16_Float,
		rhi::HeapType::DeviceLocal,
		256,
		256,
		4,
		1,
		1,
		rhi::ResourceLayout::Undefined,
		nullptr,
		rhi::RF_AllowUnorderedAccess,
		"D3D12 alias smoke texture");
	rhi::HeapPtr heap;
	if (!Check(device->CreateHeap(rhi::HeapDesc{
		.sizeBytes = 4ull * 1024ull * 1024ull,
		.alignment = 0,
		.memory = rhi::HeapType::DeviceLocal,
		.flags = rhi::HeapFlags::AllowOnlyNonRtDsTextures,
		.debugName = "D3D12 alias smoke heap",
	}, heap), "CreateHeap")) {
		return 1;
	}

	rhi::ResourcePtr textureA;
	rhi::ResourcePtr textureB;
	if (!Check(device->CreatePlacedResource(heap->GetHandle(), 0, textureDesc, textureA), "CreatePlacedResource A")
		|| !Check(device->CreatePlacedResource(heap->GetHandle(), 0, textureDesc, textureB), "CreatePlacedResource B")) {
		return 1;
	}

	auto graphicsQueue = device->GetQueue(rhi::QueueKind::Graphics);
	rhi::CommandAllocatorPtr allocator;
	rhi::CommandListPtr commandList;
	if (!graphicsQueue
		|| !Check(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator")
		|| !Check(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), commandList), "CreateCommandList")) {
		return 1;
	}

	const rhi::TextureSubresourceRange whole{ 0, 4, 0, 1, 0, 1 };
	TextureBarrier(commandList.Get(), textureA->GetHandle(), whole,
		rhi::ResourceAccessType::None, rhi::ResourceAccessType::UnorderedAccess,
		rhi::ResourceLayout::Undefined, rhi::ResourceLayout::UnorderedAccess, true);
	TextureBarrier(commandList.Get(), textureA->GetHandle(), { 0, 1, 0, 1, 0, 1 },
		rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::ShaderResource,
		rhi::ResourceLayout::UnorderedAccess, rhi::ResourceLayout::ShaderResource, false);
	TextureBarrier(commandList.Get(), textureA->GetHandle(), { 1, 1, 0, 1, 0, 1 },
		rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::CopySource,
		rhi::ResourceLayout::UnorderedAccess, rhi::ResourceLayout::CopySource, false);
	TextureBarrier(commandList.Get(), textureB->GetHandle(), whole,
		rhi::ResourceAccessType::None, rhi::ResourceAccessType::CopyDest,
		rhi::ResourceLayout::Undefined, rhi::ResourceLayout::CopyDest, true);
	commandList->End();
	if (!SubmitAndWait(device.Get(), graphicsQueue, commandList.Get())) {
		return 1;
	}

	commandList->Recycle(allocator.Get());
	TextureBarrier(commandList.Get(), textureA->GetHandle(), whole,
		rhi::ResourceAccessType::None, rhi::ResourceAccessType::UnorderedAccess,
		rhi::ResourceLayout::Undefined, rhi::ResourceLayout::UnorderedAccess, true);
	commandList->End();
	if (!SubmitAndWait(device.Get(), graphicsQueue, commandList.Get())) {
		return 1;
	}

	if (HasDebugLayerErrors(nativeDevice, firstMessage)) {
		return 1;
	}
	std::puts("BasicRHID3D12AliasingSmoke: success");
	return 0;
}
