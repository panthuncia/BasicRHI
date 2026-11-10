// Draws with stencil, the constant blend factor and dynamic viewports and scissors, and reads the target back.
// Pass: a stencil-writing draw scissored to the left half writes reference 7 there, then a draw that tests EQUAL 7,
// with its viewport on the top half, blends the shader's white by the constant factor. Only the top-left quarter
// gets the factor's colour; the rest stays black.
#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
	constexpr uint32_t kSize = 64;

	constexpr const char* kShaders = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
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
		const HRESULT hr = D3DCompile(kShaders, std::strlen(kShaders), "D3D12FixedFunctionSmoke", nullptr, nullptr, entry, target, 0, 0, out, &errors);
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

	FixedFunctionFeatureInfo caps{};
	if (!Check(device.QueryFeatureInfo(&caps.header), "QueryFeatureInfo") || caps.maxViewports < 1 || !caps.dualSourceBlend || !caps.depthClipDisable
		|| caps.secondaryViewportScissor) {
		std::fprintf(stderr, "FixedFunctionFeatureInfo: unexpected D3D12 caps\n");
		return 1;
	}

	ID3DBlob* vsBlob = nullptr;
	ID3DBlob* psBlob = nullptr;
	if (!Compile("VSMain", "vs_5_1", &vsBlob) || !Compile("PSMain", "ps_5_1", &psBlob)) {
		return 1;
	}

	rhi::PipelineLayoutPtr layout;
	if (!Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = {}, .flags = rhi::PF_None }, layout),
			"CreatePipelineLayout")) {
		return 1;
	}

	constexpr rhi::Format kColorFormat = rhi::Format::R8G8B8A8_UNorm;
	constexpr rhi::Format kDepthFormat = rhi::Format::D24_UNorm_S8_UInt;
	rhi::SubobjLayout subobjLayout{ layout->GetHandle() };
	rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { vsBlob->GetBufferPointer(), static_cast<uint32_t>(vsBlob->GetBufferSize()) }, "VSMain" };
	rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { psBlob->GetBufferPointer(), static_cast<uint32_t>(psBlob->GetBufferSize()) }, "PSMain" };
	rhi::SubobjRaster raster{};
	raster.rs.cull = rhi::CullMode::None;
	raster.rs.depthClip = false;
	rhi::SubobjRTVs targets{};
	targets.rt.count = 1;
	targets.rt.formats[0] = kColorFormat;
	rhi::SubobjDSV dsvFormat{ kDepthFormat };
	rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };

	// Writes the reference into the stencil, nothing into colour.
	rhi::SubobjDepth writeStencil{};
	writeStencil.ds.depthEnable = false;
	writeStencil.ds.depthWrite = false;
	writeStencil.ds.stencilEnable = true;
	writeStencil.ds.front.passOp = rhi::StencilOp::Replace;
	writeStencil.ds.back = writeStencil.ds.front;
	rhi::SubobjBlend noColor{};
	noColor.bs.attachments[0].writeMask = static_cast<rhi::ColorWriteEnable>(0);

	// Draws where the stencil equals the reference, colour = white * blend factor.
	rhi::SubobjDepth testStencil{};
	testStencil.ds.depthEnable = false;
	testStencil.ds.depthWrite = false;
	testStencil.ds.stencilEnable = true;
	testStencil.ds.stencilWriteMask = 0;
	testStencil.ds.front.func = rhi::CompareOp::Equal;
	testStencil.ds.back = testStencil.ds.front;
	rhi::SubobjBlend byFactor{};
	byFactor.bs.attachments[0].enable = true;
	byFactor.bs.attachments[0].srcColor = rhi::BlendFactor::BlendFactor;
	byFactor.bs.attachments[0].dstColor = rhi::BlendFactor::Zero;
	byFactor.bs.attachments[0].srcAlpha = rhi::BlendFactor::BlendFactor;
	byFactor.bs.attachments[0].dstAlpha = rhi::BlendFactor::Zero;

	rhi::PipelinePtr writePipeline, testPipeline;
	{
		const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(noColor),
			rhi::Make(writeStencil), rhi::Make(targets), rhi::Make(dsvFormat), rhi::Make(topology) };
		if (!Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), writePipeline), "CreatePipeline stencil write")) return 1;
	}
	{
		const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(byFactor),
			rhi::Make(testStencil), rhi::Make(targets), rhi::Make(dsvFormat), rhi::Make(topology) };
		if (!Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), testPipeline), "CreatePipeline stencil test")) return 1;
	}
	vsBlob->Release();
	psBlob->Release();

	rhi::ClearValue colorClear{};
	colorClear.format = kColorFormat;
	colorClear.rgba[0] = colorClear.rgba[1] = colorClear.rgba[2] = colorClear.rgba[3] = 0.0f;
	rhi::ClearValue depthClear{};
	depthClear.type = rhi::ClearValueType::DepthStencil;
	depthClear.format = kDepthFormat;
	depthClear.depthStencil = { 1.0f, 0 };
	rhi::ResourcePtr color, depth, readback;
	if (!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(kColorFormat, rhi::HeapType::DeviceLocal, kSize, kSize, 1, 1, 1,
				rhi::ResourceLayout::Undefined, &colorClear, rhi::RF_AllowRenderTarget, "FixedFunctionSmokeColor"), color), "CreateCommittedResource color") ||
		!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(kDepthFormat, rhi::HeapType::DeviceLocal, kSize, kSize, 1, 1, 1,
				rhi::ResourceLayout::Undefined, &depthClear, rhi::RF_AllowDepthStencil, "FixedFunctionSmokeDepth"), depth), "CreateCommittedResource depth")) {
		return 1;
	}
	rhi::DescriptorHeapPtr rtvHeap, dsvHeap;
	rhi::DescriptorHeapDesc heapDesc{};
	heapDesc.capacity = 1;
	heapDesc.shaderVisible = false;
	heapDesc.type = rhi::DescriptorHeapType::RTV;
	heapDesc.debugName = "FixedFunctionSmokeRTV";
	if (!Check(device.CreateDescriptorHeap(heapDesc, rtvHeap), "CreateDescriptorHeap RTV")) return 1;
	heapDesc.type = rhi::DescriptorHeapType::DSV;
	heapDesc.debugName = "FixedFunctionSmokeDSV";
	if (!Check(device.CreateDescriptorHeap(heapDesc, dsvHeap), "CreateDescriptorHeap DSV")) return 1;
	// The layout's root signature indexes the heaps directly, so they are bound even though nothing is read.
	rhi::DescriptorHeapPtr resourceHeap, samplerHeap;
	heapDesc.shaderVisible = true;
	heapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
	heapDesc.debugName = "FixedFunctionSmokeResources";
	if (!Check(device.CreateDescriptorHeap(heapDesc, resourceHeap), "CreateDescriptorHeap CbvSrvUav")) return 1;
	heapDesc.type = rhi::DescriptorHeapType::Sampler;
	heapDesc.debugName = "FixedFunctionSmokeSamplers";
	if (!Check(device.CreateDescriptorHeap(heapDesc, samplerHeap), "CreateDescriptorHeap Sampler")) return 1;
	if (!Check(device.CreateRenderTargetView({ rtvHeap->GetHandle(), 0 }, color->GetHandle(), {}), "CreateRenderTargetView") ||
		!Check(device.CreateDepthStencilView({ dsvHeap->GetHandle(), 0 }, depth->GetHandle(), rhi::DsvDesc{ .formatOverride = kDepthFormat }), "CreateDepthStencilView")) {
		return 1;
	}
	rhi::CopyableFootprint footprint{};
	const rhi::CopyableFootprintsInfo footprintInfo = device.GetCopyableFootprints(rhi::FootprintRangeDesc{ .texture = color->GetHandle() }, &footprint, 1);
	if (footprintInfo.count != 1 ||
		!Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(footprintInfo.totalBytes, rhi::HeapType::Readback, {}, "FixedFunctionSmokeReadback"), readback),
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

	const rhi::TextureBarrier toTargets[] = {
		{ .texture = color->GetHandle(), .range = {}, .beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::RenderTarget,
			.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::RenderTarget,
			.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::RenderTarget, .discard = true },
		{ .texture = depth->GetHandle(), .range = { 0, 1, 0, 1, 0, 2 }, .beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::DepthStencil,
			.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::DepthReadWrite,
			.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::DepthReadWrite, .discard = true },
	};
	list.Barriers(rhi::BarrierBatch{ .textures = { toTargets, 2 } });
	rhi::ColorAttachment colorAttachment{};
	colorAttachment.rtv = { rtvHeap->GetHandle(), 0 };
	colorAttachment.loadOp = rhi::LoadOp::Clear;
	colorAttachment.clear = colorClear;
	colorAttachment.resource = color->GetHandle();
	rhi::DepthAttachment depthAttachment{};
	depthAttachment.dsv = { dsvHeap->GetHandle(), 0 };
	depthAttachment.depthLoad = rhi::LoadOp::Clear;
	depthAttachment.stencilLoad = rhi::LoadOp::Clear;
	depthAttachment.stencilStore = rhi::StoreOp::Store;
	depthAttachment.clear = depthClear;
	depthAttachment.resource = depth->GetHandle();
	list.SetDescriptorHeaps(resourceHeap->GetHandle(), samplerHeap->GetHandle());
	list.BindLayout(layout->GetHandle());
	list.BeginPass(rhi::PassBeginInfo{ .colors = { &colorAttachment, 1 }, .depth = &depthAttachment, .width = kSize, .height = kSize, .debugName = "FixedFunctionSmoke" });
	list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);

	const rhi::ScissorRect leftHalf{ 0, 0, kSize / 2, kSize };
	list.SetScissors({ &leftHalf, 1 });
	list.SetStencilRef(7);
	list.BindPipeline(writePipeline->GetHandle());
	list.Draw(3, 1, 0, 0);

	const rhi::ScissorRect whole{ 0, 0, kSize, kSize };
	const rhi::Viewport topHalf{ 0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize / 2), 0.0f, 1.0f };
	const float factor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	list.SetScissors({ &whole, 1 });
	list.SetViewports({ &topHalf, 1 });
	list.SetBlendFactor(factor);
	list.BindPipeline(testPipeline->GetHandle());
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
	const uint32_t inside = 0xFF000000u | (191u << 16) | (128u << 8) | 64u;  // RGBA8 of the factor, little-endian
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < kSize; ++y) {
		const auto* row = reinterpret_cast<const uint32_t*>(bytes + footprint.offset + static_cast<uint64_t>(y) * footprint.rowPitch);
		for (uint32_t x = 0; x < kSize; ++x) {
			const uint32_t expected = (x < kSize / 2 && y < kSize / 2) ? inside : 0u;
			const uint32_t texel = row[x];
			// UNorm rounding may differ by one step per channel.
			bool close = true;
			for (uint32_t c = 0; c < 4; ++c) {
				const int a = static_cast<int>((texel >> (8 * c)) & 0xFF), b = static_cast<int>((expected >> (8 * c)) & 0xFF);
				close = close && (a - b <= 1 && b - a <= 1);
			}
			if (!close && wrong++ < 4) {
				std::fprintf(stderr, "texel (%u, %u) = 0x%08X, expected 0x%08X\n", x, y, texel, expected);
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
	std::puts("BasicRHID3D12FixedFunctionSmoke: success");
	return 0;
}
