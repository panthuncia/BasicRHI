// Windowed present smoke test: a swapchain on a small window, cleared and presented for a few frames per phase. Phases:
// vsync off, vsync on, HDR10 and scRGB where the output has HDR on, and back to SDR. Each frame waits on the frame
// latency, and presents with a PresentSyncDesc on the timeline its clear signalled (the GPU-side relay). It closes on
// its own. Arguments: "vulkan", "d3d12", or nothing for both. Exit 77 means no backend could be created.

#include "rhi.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
	constexpr int kSkip = 77;
	constexpr uint32_t kFramesInFlight = 3;

	bool Ok(rhi::Result result, const char* what) {
		if (result != rhi::Result::Ok) {
			std::fprintf(stderr, "  %s failed: %s\n", what, rhi::ResultName(result));
			return false;
		}
		return true;
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	void PumpMessages() {
		MSG message{};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}

	struct Frame {
		rhi::CommandAllocatorPtr allocator;
		rhi::CommandListPtr list;
		uint64_t done = 0;
	};

	struct Context {
		rhi::Device device;
		rhi::Queue queue;
		rhi::Swapchain swapchain;
		rhi::DescriptorHeapPtr rtvHeap;
		rhi::TimelinePtr timeline;
		Frame frames[kFramesInFlight];
		uint64_t frameNumber = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	// Clears the current image (it starts in Present, on both backends), presents it after the clear's timeline point.
	bool RenderFrame(Context& c, bool vsync, float r, float g, float b) {
		Frame& frame = c.frames[c.frameNumber % kFramesInFlight];
		if (frame.done != 0 && !Ok(c.timeline->HostWait(frame.done, 5000), "Timeline::HostWait")) return false;
		if (!Ok(c.swapchain.WaitForFrameLatency(1'000'000'000ull), "WaitForFrameLatency")) return false;

		const rhi::ResourceHandle image = c.swapchain.Image(c.swapchain.CurrentImageIndex());
		if (!Ok(c.device.CreateRenderTargetView({ c.rtvHeap->GetHandle(), 0 }, image, {}), "CreateRenderTargetView")) return false;

		frame.allocator->Recycle();
		frame.list->Recycle(frame.allocator.Get());
		rhi::TextureBarrier toTarget{};
		toTarget.texture = image;
		toTarget.beforeSync = rhi::ResourceSyncState::All;
		toTarget.afterSync = rhi::ResourceSyncState::RenderTarget;
		toTarget.beforeAccess = rhi::ResourceAccessType::Present;
		toTarget.afterAccess = rhi::ResourceAccessType::RenderTarget;
		toTarget.beforeLayout = rhi::ResourceLayout::Present;
		toTarget.afterLayout = rhi::ResourceLayout::RenderTarget;
		frame.list->Barriers(rhi::BarrierBatch{ .textures = { &toTarget, 1 } });

		rhi::ColorAttachment color{};
		color.rtv = { c.rtvHeap->GetHandle(), 0 };
		color.loadOp = rhi::LoadOp::Clear;
		color.storeOp = rhi::StoreOp::Store;
		color.resource = image;
		color.clear.rgba[0] = r;
		color.clear.rgba[1] = g;
		color.clear.rgba[2] = b;
		color.clear.rgba[3] = 1.0f;
		rhi::PassBeginInfo pass{};
		pass.colors = { &color, 1 };
		pass.width = c.width;
		pass.height = c.height;
		pass.debugName = "PresentSmokeClear";
		frame.list->BeginPass(pass);
		frame.list->EndPass();

		rhi::TextureBarrier toPresent = toTarget;
		toPresent.beforeSync = rhi::ResourceSyncState::RenderTarget;
		toPresent.afterSync = rhi::ResourceSyncState::All;
		toPresent.beforeAccess = rhi::ResourceAccessType::RenderTarget;
		toPresent.afterAccess = rhi::ResourceAccessType::Present;
		toPresent.beforeLayout = rhi::ResourceLayout::RenderTarget;
		toPresent.afterLayout = rhi::ResourceLayout::Present;
		frame.list->Barriers(rhi::BarrierBatch{ .textures = { &toPresent, 1 } });
		frame.list->End();

		const rhi::TimelinePoint signal{ c.timeline->GetHandle(), ++c.frameNumber };
		const rhi::CommandList lists[] = { frame.list.Get() };
		rhi::SubmitDesc submit{};
		submit.signals = { &signal, 1 };
		// The image's acquire point: already ordered on the queue that presented last, but what work on another queue
		// waits on. Waiting on it here checks that the point is valid.
		if (const rhi::TimelinePoint acquired = c.swapchain.AcquirePoint(); acquired.value != 0) {
			if (!Ok(c.queue.Wait(acquired), "Queue::Wait(AcquirePoint)")) return false;
		}
		if (!Ok(c.queue.Submit(lists, submit), "Queue::Submit")) return false;
		frame.done = signal.value;

		rhi::PresentSyncDesc sync{};
		sync.queue = c.queue;
		sync.wait = signal;
		const rhi::Result present = c.swapchain.Present(vsync, sync);
		c.queue.CheckDebugMessages();
		PumpMessages();
		// ModeChanged: the swapchain was rebuilt (a vsync switch without in-place mode changes, or a suboptimal surface).
		return present == rhi::Result::Ok || present == rhi::Result::ModeChanged || Ok(present, "Swapchain::Present");
	}

	bool RunPhase(Context& c, const char* name, bool vsync, uint32_t frameCount, float r, float g, float b) {
		const ULONGLONG start = GetTickCount64();
		for (uint32_t frame = 0; frame < frameCount; ++frame) {
			if (!RenderFrame(c, vsync, r, g, b)) {
				std::fprintf(stderr, "  phase %s failed at frame %u\n", name, frame);
				return false;
			}
		}
		const double ms = static_cast<double>(GetTickCount64() - start);
		std::printf("  %-10s %3u frames, %.1f ms/frame, presents=%llu\n", name, frameCount, ms / frameCount,
			static_cast<unsigned long long>(c.swapchain.PresentCount()));
		return true;
	}

	bool RunHdrPhase(Context& c, rhi::SwapchainColorSpace colorSpace, const char* name) {
		if (!c.swapchain.IsColorSpaceSupported(colorSpace)) {
			std::printf("  %-10s not supported by the output, skipped\n", name);
			return true;
		}
		if (!Ok(c.swapchain.SetColorSpace(colorSpace), "SetColorSpace")) return false;
		if (c.swapchain.ColorSpace() != colorSpace) {
			std::fprintf(stderr, "  ColorSpace() did not report %s\n", name);
			return false;
		}
		const rhi::Result metadata = c.swapchain.SetHdrMetadata(rhi::HdrMetadata{});
		if (metadata != rhi::Result::Ok && metadata != rhi::Result::Unsupported) return Ok(metadata, "SetHdrMetadata");
		return RunPhase(c, name, true, 20, 0.5f, 0.5f, 0.5f);
	}

	int RunBackend(rhi::Backend backend) {
		const char* label = backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12";
		std::printf("BasicRHIPresentSmoke [%s]\n", label);
		rhi::DeviceCreateInfo createInfo{};
		createInfo.backend = backend;
		createInfo.enableDebug = true;
		rhi::DevicePtr device;
		const rhi::Result created = backend == rhi::Backend::Vulkan ? rhi::CreateVulkanDevice(createInfo, device)
			: rhi::CreateD3D12Device(createInfo, device);
		if (created != rhi::Result::Ok) {
			std::printf("  no %s device (%s), skipped\n", label, rhi::ResultName(created));
			return kSkip;
		}

		const wchar_t* className = L"BasicRHIPresentSmokeWindow";
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = className;
		RegisterClassW(&windowClass);
		HWND window = CreateWindowExW(0, className, L"BasicRHIPresentSmoke", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
			nullptr, nullptr, instance, nullptr);
		if (!window) {
			std::fprintf(stderr, "  CreateWindowExW failed\n");
			return 1;
		}
		ShowWindow(window, SW_SHOWNOACTIVATE);
		RECT client{};
		GetClientRect(window, &client);

		int status = 0;
		{
			Context c{};
			c.device = device.Get();
			c.queue = device->GetQueue(rhi::QueueKind::Graphics);
			c.width = static_cast<uint32_t>(client.right - client.left);
			c.height = static_cast<uint32_t>(client.bottom - client.top);
			rhi::SwapchainPtr swapchain;
			rhi::DescriptorHeapDesc rtvDesc{};
			rtvDesc.type = rhi::DescriptorHeapType::RTV;
			rtvDesc.capacity = 1;
			rtvDesc.shaderVisible = false;
			rtvDesc.debugName = "PresentSmokeRTV";
			bool ready = c.queue && Ok(device->CreateDescriptorHeap(rtvDesc, c.rtvHeap), "CreateDescriptorHeap") &&
				Ok(device->CreateTimeline(c.timeline, 0, "PresentSmokeFrames"), "CreateTimeline") &&
				Ok(device->CreateSwapchain(window, c.width, c.height, rhi::Format::R8G8B8A8_UNorm, 3, true, swapchain), "CreateSwapchain");
			for (Frame& frame : c.frames) {
				ready = ready && Ok(device->CreateCommandAllocator(rhi::QueueKind::Graphics, frame.allocator), "CreateCommandAllocator") &&
					Ok(device->CreateCommandList(rhi::QueueKind::Graphics, frame.allocator.Get(), frame.list), "CreateCommandList");
				if (ready) frame.list->End();
			}
			if (ready) {
				c.swapchain = swapchain.Get();
				const rhi::SwapchainCaps caps = c.swapchain.Caps();
				std::printf("  caps: tearing=%d presentModeSwitch=%d presentWait=%d hdrMetadata=%d hdr10=%d scrgb=%d waitable=%p\n",
					caps.tearing, caps.presentModeSwitch, caps.presentWait, caps.hdrMetadata,
					c.swapchain.IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084),
					c.swapchain.IsColorSpaceSupported(rhi::SwapchainColorSpace::ScRgbLinear), c.swapchain.FrameLatencyWaitableObject());
				ready = Ok(c.swapchain.SetMaximumFrameLatency(2), "SetMaximumFrameLatency");
			}
			ready = ready && RunPhase(c, "vsync-off", false, 60, 0.1f, 0.2f, 0.35f) &&
				RunPhase(c, "vsync-on", true, 30, 0.35f, 0.2f, 0.1f) &&
				RunHdrPhase(c, rhi::SwapchainColorSpace::Hdr10St2084, "hdr10") &&
				RunHdrPhase(c, rhi::SwapchainColorSpace::ScRgbLinear, "scrgb") &&
				Ok(c.swapchain.SetColorSpace(rhi::SwapchainColorSpace::SrgbNonlinear), "SetColorSpace(SDR)") &&
				RunPhase(c, "sdr-again", false, 20, 0.1f, 0.35f, 0.2f);
			if (ready && c.swapchain.Caps().presentWait) {
				ready = Ok(c.swapchain.WaitForPresent(c.swapchain.PresentCount(), 1'000'000'000ull), "WaitForPresent");
			}
			if (!ready) status = 1;
			if (!Ok(device->WaitIdle(), "Device::WaitIdle")) status = 1;
		}
		DestroyWindow(window);
		UnregisterClassW(className, instance);
		std::printf("BasicRHIPresentSmoke [%s]: %s\n", label, status == 0 ? "success" : "FAILED");
		return status;
	}
}

int main(int argc, char** argv) {
	const bool vulkan = argc < 2 || std::strcmp(argv[1], "vulkan") == 0;
	const bool d3d12 = argc < 2 || std::strcmp(argv[1], "d3d12") == 0;
	int ran = 0;
	int status = 0;
	if (vulkan) {
		const int result = RunBackend(rhi::Backend::Vulkan);
		if (result != kSkip) { ++ran; status |= result; }
	}
	if (d3d12) {
		const int result = RunBackend(rhi::Backend::D3D12);
		if (result != kSkip) { ++ran; status |= result; }
	}
	return ran == 0 ? kSkip : status;
}
