// LayoutRangeSource::PushIndex on D3D12: constant buffers bound through root descriptor tables whose base is a pushed heap slot.
// Eight CBVs sit in heap slots 0-7, CBV i holding uint4(10 * i + 1). FrameA (b0, space9) reads slot (S + 2) and FrameB (b1, space9)
// slot (S + 5), S being dword 1 of the root constants. The top half is drawn with S = 1, then the heaps and the layout are set
// again (each sets the tables again from the pushed slot); the bottom half with S = 0 pushed on its own (PushConstants sets the
// tables). The pixel shader writes (A.x, B.x, dword 0) / 255 into red, green and blue. Also checks that a command signature may not write S and that a sampler range is refused.
#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
	constexpr uint32_t kSize = 64;
	constexpr uint32_t kCbvCount = 8;
	constexpr uint32_t kCbvStride = 256;

	constexpr const char* kShaders = R"(
cbuffer Root : register(b0) { uint drawFlags; uint frameSlot; uint2 unused; };
cbuffer FrameA : register(b0, space9) { uint4 a; };
cbuffer FrameB : register(b1, space9) { uint4 b; };
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 PSMain() : SV_Target { return float4(a.x / 255.0, b.x / 255.0, drawFlags / 255.0, 1); }
)";

	bool Check(rhi::Result result, const char* operation) {
		if (rhi::IsOk(result)) {
			return true;
		}
		std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
		return false;
	}

	bool Compile(const char* entry, const char* target, ID3DBlob** out) {
		ID3DBlob* errors = nullptr;
		const HRESULT hr = D3DCompile(kShaders, std::strlen(kShaders), "D3D12PushIndexSmoke", nullptr, nullptr, entry, target, 0, 0, out, &errors);
		if (FAILED(hr)) {
			std::fprintf(stderr, "D3DCompile %s failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
		}
		if (errors) errors->Release();
		return SUCCEEDED(hr);
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
			if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
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
	rhi::DevicePtr devicePtr;
	if (!Check(rhi::CreateD3D12Device(createInfo, devicePtr), "CreateD3D12Device")) {
		return 1;
	}
	rhi::Device& device = devicePtr.Get();
	ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
	UINT64 firstMessage = 0;
	if (ID3D12InfoQueue* infoQueue = nullptr; nativeDevice && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
		firstMessage = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
		infoQueue->Release();
	}

	ID3DBlob* vsBlob = nullptr;
	ID3DBlob* psBlob = nullptr;
	if (!Compile("VSMain", "vs_5_1", &vsBlob) || !Compile("PSMain", "ps_5_1", &psBlob)) {
		return 1;
	}

	rhi::PushConstantRangeDesc rootConstants{ .visibility = rhi::ShaderStage::All, .num32BitValues = 4, .set = 0, .binding = 0 };
	rhi::LayoutBindingRange ranges[2]{};
	ranges[0] = { .set = 9, .binding = 0, .count = 1, .visibility = rhi::ShaderStage::Pixel, .source = rhi::LayoutRangeSource::PushIndex,
		.addressRootIndex = 0, .addressOffset32 = 1, .recordOffset = 2 };
	ranges[1] = ranges[0];
	ranges[1].binding = 1;
	ranges[1].recordOffset = 5;
	rhi::PipelineLayoutPtr layout;
	if (!Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = { ranges, 2 }, .pushConstants = { &rootConstants, 1 }, .staticSamplers = {},
		.flags = rhi::PF_None }, layout), "CreatePipelineLayout")) {
		return 1;
	}
	{
		// Samplers are not supported through a push index.
		rhi::LayoutBindingRange samplers = ranges[0];
		samplers.samplers = true;
		rhi::PipelineLayoutPtr refused;
		if (device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = { &samplers, 1 }, .pushConstants = { &rootConstants, 1 }, .staticSamplers = {},
			.flags = rhi::PF_None }, refused) != rhi::Result::InvalidArgument) {
			std::fprintf(stderr, "CreatePipelineLayout accepted a sampler push index range\n");
			return 1;
		}
	}
	{
		// An indirect command may set dword 0 but not the pass-wide slot in dword 1.
		rhi::IndirectArg args[2]{};
		args[0].kind = rhi::IndirectArgKind::Constant;
		args[0].u.rootConstants = { 0, 0, 1 };
		args[1].kind = rhi::IndirectArgKind::Draw;
		rhi::CommandSignaturePtr signature;
		if (!Check(device.CreateCommandSignature(rhi::CommandSignatureDesc{ .args = { args, 2 }, .byteStride = 32 }, layout->GetHandle(), signature),
				"CreateCommandSignature (dword 0)")) {
			return 1;
		}
		args[0].u.rootConstants = { 0, 0, 2 };
		rhi::CommandSignaturePtr refused;
		if (device.CreateCommandSignature(rhi::CommandSignatureDesc{ .args = { args, 2 }, .byteStride = 32 }, layout->GetHandle(), refused) !=
			rhi::Result::InvalidArgument) {
			std::fprintf(stderr, "CreateCommandSignature accepted a write of the push index slot\n");
			return 1;
		}
	}

	constexpr rhi::Format kColorFormat = rhi::Format::R8G8B8A8_UNorm;
	rhi::SubobjLayout subobjLayout{ layout->GetHandle() };
	rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { vsBlob->GetBufferPointer(), static_cast<uint32_t>(vsBlob->GetBufferSize()) }, "VSMain" };
	rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { psBlob->GetBufferPointer(), static_cast<uint32_t>(psBlob->GetBufferSize()) }, "PSMain" };
	rhi::SubobjRaster raster{};
	raster.rs.cull = rhi::CullMode::None;
	rhi::SubobjDepth noDepth{};
	noDepth.ds.depthEnable = false;
	noDepth.ds.depthWrite = false;
	rhi::SubobjRTVs targets{};
	targets.rt.count = 1;
	targets.rt.formats[0] = kColorFormat;
	rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
	rhi::PipelinePtr pipeline;
	{
		const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(noDepth),
			rhi::Make(targets), rhi::Make(topology) };
		if (!Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), pipeline), "CreatePipeline")) return 1;
	}
	vsBlob->Release();
	psBlob->Release();

	// The constant buffers: CBV i at heap slot i holds uint4(10 * i + 1).
	rhi::ResourcePtr constants;
	if (!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kCbvCount * kCbvStride, rhi::HeapType::Upload, {}, "PushIndexSmokeConstants"),
			constants), "CreateCommittedResource constants")) {
		return 1;
	}
	void* mappedConstants = nullptr;
	constants->Map(&mappedConstants, 0, kCbvCount * kCbvStride);
	for (uint32_t i = 0; i < kCbvCount; ++i) {
		uint32_t value[4] = { 10 * i + 1, 0, 0, 0 };
		std::memcpy(static_cast<uint8_t*>(mappedConstants) + i * kCbvStride, value, sizeof(value));
	}
	constants->Unmap(0, 0);

	rhi::ClearValue colorClear{};
	colorClear.format = kColorFormat;
	rhi::ResourcePtr color, readback;
	if (!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(kColorFormat, rhi::HeapType::DeviceLocal, kSize, kSize, 1, 1, 1,
				rhi::ResourceLayout::Undefined, &colorClear, rhi::RF_AllowRenderTarget, "PushIndexSmokeColor"), color), "CreateCommittedResource color")) {
		return 1;
	}
	rhi::DescriptorHeapPtr rtvHeap, resourceHeap, samplerHeap;
	rhi::DescriptorHeapDesc heapDesc{};
	heapDesc.capacity = 1;
	heapDesc.shaderVisible = false;
	heapDesc.type = rhi::DescriptorHeapType::RTV;
	heapDesc.debugName = "PushIndexSmokeRTV";
	if (!Check(device.CreateDescriptorHeap(heapDesc, rtvHeap), "CreateDescriptorHeap RTV")) return 1;
	heapDesc.capacity = kCbvCount;
	heapDesc.shaderVisible = true;
	heapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
	heapDesc.debugName = "PushIndexSmokeResources";
	if (!Check(device.CreateDescriptorHeap(heapDesc, resourceHeap), "CreateDescriptorHeap CbvSrvUav")) return 1;
	heapDesc.capacity = 1;
	heapDesc.type = rhi::DescriptorHeapType::Sampler;
	heapDesc.debugName = "PushIndexSmokeSamplers";
	if (!Check(device.CreateDescriptorHeap(heapDesc, samplerHeap), "CreateDescriptorHeap Sampler")) return 1;
	for (uint32_t i = 0; i < kCbvCount; ++i) {
		if (!Check(device.CreateConstantBufferView({ resourceHeap->GetHandle(), i }, constants->GetHandle(), rhi::CbvDesc{ i * kCbvStride, kCbvStride }),
				"CreateConstantBufferView")) {
			return 1;
		}
	}
	if (!Check(device.CreateRenderTargetView({ rtvHeap->GetHandle(), 0 }, color->GetHandle(), {}), "CreateRenderTargetView")) {
		return 1;
	}
	rhi::CopyableFootprint footprint{};
	const rhi::CopyableFootprintsInfo footprintInfo = device.GetCopyableFootprints(rhi::FootprintRangeDesc{ .texture = color->GetHandle() }, &footprint, 1);
	if (footprintInfo.count != 1 ||
		!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(footprintInfo.totalBytes, rhi::HeapType::Readback, {}, "PushIndexSmokeReadback"), readback),
			"CreateCommittedResource readback")) {
		return 1;
	}

	auto queue = device.GetQueue(rhi::QueueKind::Graphics);
	rhi::CommandAllocatorPtr allocator;
	rhi::CommandListPtr listPtr;
	if (!queue || !Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator") ||
		!Check(device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), listPtr), "CreateCommandList")) {
		return 1;
	}
	rhi::CommandList& list = listPtr.Get();

	const rhi::TextureBarrier toTarget{ .texture = color->GetHandle(), .range = {}, .beforeSync = rhi::ResourceSyncState::None,
		.afterSync = rhi::ResourceSyncState::RenderTarget, .beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::RenderTarget,
		.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::RenderTarget, .discard = true };
	list.Barriers(rhi::BarrierBatch{ .textures = { &toTarget, 1 } });
	rhi::ColorAttachment colorAttachment{};
	colorAttachment.rtv = { rtvHeap->GetHandle(), 0 };
	colorAttachment.loadOp = rhi::LoadOp::Clear;
	colorAttachment.clear = colorClear;
	colorAttachment.resource = color->GetHandle();

	// Top half: S = 1. Setting the heaps again invalidates the tables, and setting them sets the tables again; so does
	// binding the same layout again.
	list.SetDescriptorHeaps(resourceHeap->GetHandle(), samplerHeap->GetHandle());
	list.BindLayout(layout->GetHandle());
	const uint32_t top[2] = { 3, 1 };
	list.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, 2, top);
	list.SetDescriptorHeaps(resourceHeap->GetHandle(), samplerHeap->GetHandle());
	list.BindLayout(layout->GetHandle());
	list.BeginPass(rhi::PassBeginInfo{ .colors = { &colorAttachment, 1 }, .width = kSize, .height = kSize, .debugName = "PushIndexSmoke" });
	list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
	list.BindPipeline(pipeline->GetHandle());
	const rhi::ScissorRect whole{ 0, 0, kSize, kSize };
	list.SetScissors({ &whole, 1 });
	const rhi::Viewport topHalf{ 0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize / 2), 0.0f, 1.0f };
	list.SetViewports({ &topHalf, 1 });
	list.Draw(3, 1, 0, 0);

	// Bottom half: S = 0 pushed on its own; PushConstants sets the tables.
	const uint32_t bottom = 0;
	list.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 1, 1, &bottom);
	const rhi::Viewport bottomHalf{ 0.0f, static_cast<float>(kSize / 2), static_cast<float>(kSize), static_cast<float>(kSize / 2), 0.0f, 1.0f };
	list.SetViewports({ &bottomHalf, 1 });
	list.Draw(3, 1, 0, 0);
	list.EndPass();

	const rhi::TextureBarrier toCopy{ .texture = color->GetHandle(), .range = {}, .beforeSync = rhi::ResourceSyncState::RenderTarget, .afterSync = rhi::ResourceSyncState::Copy,
		.beforeAccess = rhi::ResourceAccessType::RenderTarget, .afterAccess = rhi::ResourceAccessType::CopySource,
		.beforeLayout = rhi::ResourceLayout::RenderTarget, .afterLayout = rhi::ResourceLayout::CopySource, .discard = false };
	list.Barriers(rhi::BarrierBatch{ .textures = { &toCopy, 1 } });
	list.CopyTextureToBuffer(rhi::BufferTextureCopyFootprint{ .texture = color->GetHandle(), .buffer = readback->GetHandle(), .mip = 0, .arraySlice = 0, .footprint = footprint });
	if (!Check(list.EndChecked(), "CommandList::End")) return 1;
	const rhi::CommandList lists[] = { list };
	if (!Check(queue.Submit(lists, {}), "Queue::Submit") || !Check(device.WaitIdle(), "Device::WaitIdle")) return 1;

	void* mapped = nullptr;
	readback->Map(&mapped, 0, footprintInfo.totalBytes);
	const auto* bytes = static_cast<const uint8_t*>(mapped);
	// RGBA8, little-endian: top = slots 3 and 6 (31, 61), bottom = slots 2 and 5 (21, 51); blue is dword 0 (3).
	const uint32_t topTexel = 0xFF000000u | (3u << 16) | (61u << 8) | 31u;
	const uint32_t bottomTexel = 0xFF000000u | (3u << 16) | (51u << 8) | 21u;
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < kSize; ++y) {
		const auto* row = reinterpret_cast<const uint32_t*>(bytes + footprint.offset + static_cast<uint64_t>(y) * footprint.rowPitch);
		for (uint32_t x = 0; x < kSize; ++x) {
			const uint32_t expected = y < kSize / 2 ? topTexel : bottomTexel;
			if (row[x] != expected && wrong++ < 4) {
				std::fprintf(stderr, "texel (%u, %u) = 0x%08X, expected 0x%08X\n", x, y, row[x], expected);
			}
		}
	}
	readback->Unmap(0, 0);
	if (wrong != 0) {
		std::fprintf(stderr, "%u of %u texels wrong\n", wrong, kSize * kSize);
		return 1;
	}
	if (HasDebugLayerErrors(nativeDevice, firstMessage)) {
		return 1;
	}
	std::puts("BasicRHID3D12PushIndexSmoke: success");
	return 0;
}
