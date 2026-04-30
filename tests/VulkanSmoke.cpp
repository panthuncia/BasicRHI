#include "rhi.h"

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
			.depth = nullptr,
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

	rhi::CommandAllocatorPtr allocator;
	if (Check(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator") != rhi::Result::Ok) {
		return 1;
	}

	rhi::CommandListPtr commandList;
	if (Check(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), commandList), "CreateCommandList") != rhi::Result::Ok) {
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

		if (RecordAndSubmitClearPass(device.Get(), graphicsQueue, allocator.Get(), commandList.Get(), swapchain.Get(), rtvHeap.Get(), renderWidth, renderHeight) != rhi::Result::Ok) {
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

		if (RecordAndSubmitClearPass(device.Get(), graphicsQueue, allocator.Get(), commandList.Get(), swapchain.Get(), rtvHeap.Get(), renderWidth, renderHeight) != rhi::Result::Ok) {
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