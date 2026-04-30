#include "rhi.h"
#include "rhi_helpers.h"

#include <cstdio>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
	rhi::Result Check(rhi::Result result, const char* what) {
		if (result != rhi::Result::Ok) {
			std::fprintf(stderr, "%s failed with result %u\n", what, static_cast<unsigned>(result));
		}
		return result;
	}

	rhi::Result RecordAndSubmitClearPass(
		rhi::Device& device,
		rhi::Queue& graphicsQueue,
		rhi::CommandAllocator& allocator,
		rhi::CommandList& commandList,
		rhi::Swapchain& swapchain,
		rhi::DescriptorHeap& rtvHeap,
		const rhi::DepthAttachment* depthAttachment,
		uint32_t width,
		uint32_t height) {
		const uint32_t imageIndex = swapchain.CurrentImageIndex();
		const rhi::ResourceHandle imageHandle = swapchain.Image(imageIndex);
		if (!imageHandle.valid()) {
			std::fprintf(stderr, "Swapchain image handle was invalid for render pass\n");
			return rhi::Result::InvalidArgument;
		}

		if (Check(device.CreateRenderTargetView({ rtvHeap.GetHandle(), 0 }, imageHandle, {}), "CreateRenderTargetView") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		commandList.Recycle(allocator);

		rhi::ColorAttachment color{};
		color.rtv = { rtvHeap.GetHandle(), 0 };
		color.loadOp = rhi::LoadOp::Clear;
		color.storeOp = rhi::StoreOp::Store;
		color.resource = imageHandle;
		color.clear.rgba[0] = 0.10f;
		color.clear.rgba[1] = 0.20f;
		color.clear.rgba[2] = 0.35f;
		color.clear.rgba[3] = 1.00f;

		const rhi::PassBeginInfo passInfo{
			.colors = { &color, 1 },
			.depth = depthAttachment,
			.width = width,
			.height = height,
			.debugName = "VulkanSmokeClear"
		};

		commandList.BeginPass(passInfo);
		commandList.EndPass();
		commandList.End();

		const rhi::CommandList submitLists[] = { commandList };
		return Check(graphicsQueue.Submit(submitLists, {}), "Queue::Submit clear pass");
	}

	rhi::Result ValidateUploadBuffer(rhi::Device& device) {
		rhi::ResourcePtr uploadBuffer;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(sizeof(uint32_t), rhi::HeapType::Upload, {}, "VulkanSmokeUploadBuffer"), uploadBuffer), "CreateCommittedResource upload buffer") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		void* mappedData = nullptr;
		uploadBuffer->Map(&mappedData, 0, sizeof(uint32_t));
		if (mappedData == nullptr) {
			std::fprintf(stderr, "Upload buffer map returned null\n");
			return rhi::Result::InvalidArgument;
		}

		*static_cast<uint32_t*>(mappedData) = 0x1234ABCDu;
		uploadBuffer->Unmap(0, sizeof(uint32_t));
		return rhi::Result::Ok;
	}

	rhi::Result ValidateDescriptors(rhi::Device& device, rhi::CommandList& commandList) {
		rhi::DescriptorHeapPtr shaderVisibleHeap;
		rhi::DescriptorHeapPtr cpuVisibleHeap;
		rhi::DescriptorHeapPtr samplerHeap;

		rhi::DescriptorHeapDesc shaderHeapDesc{};
		shaderHeapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
		shaderHeapDesc.capacity = 4;
		shaderHeapDesc.shaderVisible = true;
		shaderHeapDesc.debugName = "VulkanSmokeShaderVisibleHeap";
		if (Check(device.CreateDescriptorHeap(shaderHeapDesc, shaderVisibleHeap), "CreateDescriptorHeap shader-visible") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::DescriptorHeapDesc cpuHeapDesc{};
		cpuHeapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
		cpuHeapDesc.capacity = 1;
		cpuHeapDesc.shaderVisible = false;
		cpuHeapDesc.debugName = "VulkanSmokeCpuVisibleHeap";
		if (Check(device.CreateDescriptorHeap(cpuHeapDesc, cpuVisibleHeap), "CreateDescriptorHeap cpu-visible") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::DescriptorHeapDesc samplerHeapDesc{};
		samplerHeapDesc.type = rhi::DescriptorHeapType::Sampler;
		samplerHeapDesc.capacity = 1;
		samplerHeapDesc.shaderVisible = true;
		samplerHeapDesc.debugName = "VulkanSmokeSamplerHeap";
		if (Check(device.CreateDescriptorHeap(samplerHeapDesc, samplerHeap), "CreateDescriptorHeap sampler") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::ResourcePtr constantBuffer;
		if (Check(device.CreateCommittedResource(
			rhi::helpers::ResourceDesc::Buffer(256, rhi::HeapType::Upload, {}, "VulkanSmokeConstantBuffer"),
			constantBuffer), "CreateCommittedResource constant buffer") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::ResourcePtr sampledStorageTexture;
		if (Check(device.CreateCommittedResource(
			rhi::helpers::ResourceDesc::Tex2D(
				rhi::Format::R8G8B8A8_UNorm,
				rhi::HeapType::DeviceLocal,
				64,
				64,
				1,
				1,
				1,
				rhi::ResourceLayout::Undefined,
				nullptr,
				rhi::RF_AllowUnorderedAccess,
				"VulkanSmokeSampledStorageTexture"),
			sampledStorageTexture), "CreateCommittedResource sampled storage texture") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::CbvDesc cbvDesc{};
		cbvDesc.byteOffset = 0;
		cbvDesc.byteSize = 256;
		if (Check(device.CreateConstantBufferView({ shaderVisibleHeap->GetHandle(), 0 }, constantBuffer->GetHandle(), cbvDesc), "CreateConstantBufferView") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::SrvDesc srvDesc{};
		srvDesc.dimension = rhi::SrvDim::Texture2D;
		srvDesc.tex2D.mostDetailedMip = 0;
		srvDesc.tex2D.mipLevels = 1;
		if (Check(device.CreateShaderResourceView({ shaderVisibleHeap->GetHandle(), 1 }, sampledStorageTexture->GetHandle(), srvDesc), "CreateShaderResourceView") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::UavDesc shaderVisibleUavDesc{};
		shaderVisibleUavDesc.dimension = rhi::UavDim::Texture2D;
		shaderVisibleUavDesc.texture2D.mipSlice = 0;
		if (Check(device.CreateUnorderedAccessView({ shaderVisibleHeap->GetHandle(), 2 }, sampledStorageTexture->GetHandle(), shaderVisibleUavDesc), "CreateUnorderedAccessView shader-visible") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::UavDesc cpuVisibleUavDesc{};
		cpuVisibleUavDesc.dimension = rhi::UavDim::Texture2D;
		cpuVisibleUavDesc.texture2D.mipSlice = 0;
		if (Check(device.CreateUnorderedAccessView({ cpuVisibleHeap->GetHandle(), 0 }, sampledStorageTexture->GetHandle(), cpuVisibleUavDesc), "CreateUnorderedAccessView cpu-visible") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::SamplerDesc samplerDesc{};
		samplerDesc.minFilter = rhi::Filter::Linear;
		samplerDesc.magFilter = rhi::Filter::Linear;
		samplerDesc.mipFilter = rhi::MipFilter::Linear;
		samplerDesc.addressU = rhi::AddressMode::Clamp;
		samplerDesc.addressV = rhi::AddressMode::Clamp;
		samplerDesc.addressW = rhi::AddressMode::Clamp;
		if (Check(device.CreateSampler({ samplerHeap->GetHandle(), 0 }, samplerDesc), "CreateSampler") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		commandList.SetDescriptorHeaps(shaderVisibleHeap->GetHandle(), samplerHeap->GetHandle());
		return rhi::Result::Ok;
	}

#ifdef _WIN32
	LRESULT CALLBACK SmokeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	HWND CreateSmokeWindow(HINSTANCE instance, const wchar_t* className) {
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = SmokeWindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = className;
		RegisterClassW(&windowClass);

		return CreateWindowExW(
			0,
			className,
			L"BasicRHIVulkanSmoke",
			WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			320,
			240,
			nullptr,
			nullptr,
			instance,
			nullptr);
	}

	void GetWindowClientExtent(HWND window, uint32_t& width, uint32_t& height) {
		RECT clientRect{};
		if (GetClientRect(window, &clientRect)) {
			width = static_cast<uint32_t>(clientRect.right - clientRect.left);
			height = static_cast<uint32_t>(clientRect.bottom - clientRect.top);
		}
	}
#endif
}

int main() {
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::Vulkan;
	createInfo.enableDebug = true;

	rhi::DevicePtr device;
	if (Check(rhi::CreateVulkanDevice(createInfo, device), "CreateVulkanDevice") != rhi::Result::Ok) {
		return 1;
	}

	auto graphicsQueue = device->GetQueue(rhi::QueueKind::Graphics);
	if (!graphicsQueue) {
		std::fprintf(stderr, "Graphics queue acquisition failed\n");
		return 1;
	}

	if (ValidateUploadBuffer(device.Get()) != rhi::Result::Ok) {
		return 1;
	}

	rhi::CommandAllocatorPtr allocator;
	if (Check(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator") != rhi::Result::Ok) {
		return 1;
	}

	rhi::CommandListPtr commandList;
	if (Check(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), commandList), "CreateCommandList") != rhi::Result::Ok) {
		return 1;
	}

	if (ValidateDescriptors(device.Get(), commandList.Get()) != rhi::Result::Ok) {
		return 1;
	}

	commandList->End();
	const rhi::CommandList submitLists[] = { commandList.Get() };
	if (Check(graphicsQueue.Submit(submitLists, {}), "Queue::Submit") != rhi::Result::Ok) {
		return 1;
	}

	if (Check(device->WaitIdle(), "Device::WaitIdle") != rhi::Result::Ok) {
		return 1;
	}

#ifdef _WIN32
	const wchar_t* windowClassName = L"BasicRHIVulkanSmokeWindow";
	HINSTANCE instance = GetModuleHandleW(nullptr);
	HWND window = CreateSmokeWindow(instance, windowClassName);
	if (!window) {
		std::fprintf(stderr, "CreateWindowExW failed\n");
		return 1;
	}

	{
		rhi::SwapchainPtr swapchain;
		if (Check(device->CreateSwapchain(window, 320, 240, rhi::Format::R8G8B8A8_UNorm, 2, true, swapchain), "CreateSwapchain") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (swapchain->ImageCount() == 0) {
			std::fprintf(stderr, "Swapchain image count was zero\n");
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (!swapchain->Image(swapchain->CurrentImageIndex()).valid()) {
			std::fprintf(stderr, "Current swapchain image handle was invalid\n");
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		uint32_t renderWidth = 320;
		uint32_t renderHeight = 240;
		GetWindowClientExtent(window, renderWidth, renderHeight);

		rhi::DescriptorHeapPtr rtvHeap;
		rhi::DescriptorHeapPtr dsvHeap;
		rhi::DescriptorHeapDesc rtvHeapDesc{};
		rtvHeapDesc.type = rhi::DescriptorHeapType::RTV;
		rtvHeapDesc.capacity = 1;
		rtvHeapDesc.shaderVisible = false;
		rtvHeapDesc.debugName = "VulkanSmokeRTVHeap";
		if (Check(device->CreateDescriptorHeap(rtvHeapDesc, rtvHeap), "CreateDescriptorHeap") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		rhi::DescriptorHeapDesc dsvHeapDesc{};
		dsvHeapDesc.type = rhi::DescriptorHeapType::DSV;
		dsvHeapDesc.capacity = 1;
		dsvHeapDesc.shaderVisible = false;
		dsvHeapDesc.debugName = "VulkanSmokeDSVHeap";
		if (Check(device->CreateDescriptorHeap(dsvHeapDesc, dsvHeap), "CreateDescriptorHeap DSV") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		rhi::ClearValue depthClear{};
		depthClear.type = rhi::ClearValueType::DepthStencil;
		depthClear.format = rhi::Format::D32_Float;
		depthClear.depthStencil.depth = 1.0f;
		depthClear.depthStencil.stencil = 0;

		rhi::ResourcePtr depthTexture;
		if (Check(device->CreateCommittedResource(
			rhi::helpers::ResourceDesc::Tex2D(
				rhi::Format::D32_Float,
				rhi::HeapType::DeviceLocal,
				renderWidth,
				renderHeight,
				1,
				1,
				1,
				rhi::ResourceLayout::Undefined,
				&depthClear,
				rhi::RF_AllowDepthStencil,
				"VulkanSmokeDepth"),
			depthTexture), "CreateCommittedResource depth texture") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		rhi::DsvDesc dsvDesc{};
		dsvDesc.dimension = rhi::DsvDim::Texture2D;
		dsvDesc.formatOverride = rhi::Format::D32_Float;
		if (Check(device->CreateDepthStencilView({ dsvHeap->GetHandle(), 0 }, depthTexture->GetHandle(), dsvDesc), "CreateDepthStencilView") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		rhi::DepthAttachment depthAttachment{};
		depthAttachment.dsv = { dsvHeap->GetHandle(), 0 };
		depthAttachment.depthLoad = rhi::LoadOp::Clear;
		depthAttachment.depthStore = rhi::StoreOp::Store;
		depthAttachment.stencilLoad = rhi::LoadOp::DontCare;
		depthAttachment.stencilStore = rhi::StoreOp::DontCare;
		depthAttachment.clear = depthClear;
		depthAttachment.readOnly = false;
		depthAttachment.resource = depthTexture->GetHandle();

		if (RecordAndSubmitClearPass(device.Get(), graphicsQueue, allocator.Get(), commandList.Get(), swapchain.Get(), rtvHeap.Get(), &depthAttachment, renderWidth, renderHeight) != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (Check(device->WaitIdle(), "Device::WaitIdle after clear pass") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (Check(swapchain->Present(false), "Swapchain::Present") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (Check(device->WaitIdle(), "Device::WaitIdle after present") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (Check(swapchain->ResizeBuffers(2, 256, 192, rhi::Format::R8G8B8A8_UNorm, 0), "Swapchain::ResizeBuffers") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (swapchain->ImageCount() == 0 || !swapchain->Image(swapchain->CurrentImageIndex()).valid()) {
			std::fprintf(stderr, "Resized swapchain image handle was invalid\n");
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		renderWidth = 256;
		renderHeight = 192;
		GetWindowClientExtent(window, renderWidth, renderHeight);

		if (RecordAndSubmitClearPass(device.Get(), graphicsQueue, allocator.Get(), commandList.Get(), swapchain.Get(), rtvHeap.Get(), nullptr, renderWidth, renderHeight) != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}

		if (Check(swapchain->Present(false), "Swapchain::Present after resize") != rhi::Result::Ok) {
			DestroyWindow(window);
			UnregisterClassW(windowClassName, instance);
			return 1;
		}
	}

	DestroyWindow(window);
	UnregisterClassW(windowClassName, instance);

	if (Check(device->WaitIdle(), "Device::WaitIdle after swapchain") != rhi::Result::Ok) {
		return 1;
	}
#endif

	std::puts("BasicRHIVulkanSmoke: success");
	return 0;
}