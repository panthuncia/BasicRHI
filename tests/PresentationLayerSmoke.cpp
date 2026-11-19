// Swapchains a presentation layer may wrap (SwapchainDesc::presentationLayer). Each backend makes a swapchain on a small
// window, rebuilds it (resize, vsync switch, colour space) and replaces it (destroy, then create on the same window), and
// clears and presents a few frames after every step, so a stale image fails the frame.
// Vulkan: this executable plays the host's Streamline interposer (DeviceCreateInfo::streamlineHost). Its exports hand
// BasicRHI swapchain functions that record every create and destroy. With the flag, a create must find no other swapchain
// alive, pass no oldSwapchain and no pNext chain, and be followed by a read of the new swapchain's images. Without it, a
// resize overlaps the old and new swapchains, which shows the recording sees the difference.
// D3D12: StreamlineStubs in pass-through mode, whose native swapchain counts GetBuffer. With the flag, the images come from
// the proxy, so the native swapchain is never asked, and the waitable object is still valid after every rebuild.
// Arguments: "vulkan", "d3d12", or nothing for both. Exit 77 means no backend could be created.

#include "rhi.h"
#include "rhi_interop_vulkan.h"
#include "sl.h"

#include "StreamlineStubs.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>

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

	bool Check(bool condition, const char* what) {
		if (!condition) std::fprintf(stderr, "  check failed: %s\n", what);
		return condition;
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

	// ---- The fake Streamline interposer's Vulkan swapchain functions ----
	namespace fake {
		struct Event {
			enum class Kind { Create, Destroy, Images } kind;
			VkSwapchainKHR handle = VK_NULL_HANDLE;
			VkSwapchainKHR old = VK_NULL_HANDLE;
			bool pNext = false;
			size_t alive = 0;
		};
		std::vector<Event> events;
		std::set<VkSwapchainKHR> alive;
		PFN_vkGetInstanceProcAddr realGetInstanceProcAddr = nullptr;
		PFN_vkGetDeviceProcAddr realGetDeviceProcAddr = nullptr;
		PFN_vkCreateSwapchainKHR realCreate = nullptr;
		PFN_vkDestroySwapchainKHR realDestroy = nullptr;
		PFN_vkGetSwapchainImagesKHR realImages = nullptr;

		VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
			Event event{ Event::Kind::Create, VK_NULL_HANDLE, info->oldSwapchain, info->pNext != nullptr, alive.size() };
			const VkResult result = realCreate(device, info, allocator, out);
			if (result == VK_SUCCESS) {
				event.handle = *out;
				alive.insert(*out);
			}
			events.push_back(event);
			return result;
		}

		void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
			if (swapchain != VK_NULL_HANDLE) {
				events.push_back({ Event::Kind::Destroy, swapchain, VK_NULL_HANDLE, false, alive.size() });
				alive.erase(swapchain);
			}
			realDestroy(device, swapchain, allocator);
		}

		VkResult VKAPI_CALL GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images) {
			if (images) events.push_back({ Event::Kind::Images, swapchain, VK_NULL_HANDLE, false, alive.size() });
			return realImages(device, swapchain, count, images);
		}

		PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
			const PFN_vkVoidFunction real = realGetDeviceProcAddr(device, name);
			if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
				realCreate = reinterpret_cast<PFN_vkCreateSwapchainKHR>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&CreateSwapchain);
			}
			if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
				realDestroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&DestroySwapchain);
			}
			if (std::strcmp(name, "vkGetSwapchainImagesKHR") == 0) {
				realImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&GetSwapchainImages);
			}
			return real;
		}

		// Checks the events since `from`: with a presentation layer every create comes alone and without a chain, and its
		// images are read after it. Without, at least one create must overlap (proving the recording sees it).
		bool CheckEvents(size_t from, bool presentationLayer, const char* step) {
			bool ok = true;
			bool sawCreate = false;
			bool sawOverlap = false;
			for (size_t i = from; i < events.size(); ++i) {
				const Event& event = events[i];
				if (event.kind != Event::Kind::Create) continue;
				sawCreate = true;
				if (event.alive != 0 || event.old != VK_NULL_HANDLE) sawOverlap = true;
				if (!presentationLayer) continue;
				if (event.alive != 0 || event.old != VK_NULL_HANDLE) {
					std::fprintf(stderr, "  %s: a create found %zu swapchain(s) alive (oldSwapchain %s)\n", step, event.alive,
						event.old != VK_NULL_HANDLE ? "set" : "null");
					ok = false;
				}
				if (event.pNext) {
					std::fprintf(stderr, "  %s: a create passed a pNext chain\n", step);
					ok = false;
				}
				bool imagesRead = false;
				for (size_t j = i + 1; j < events.size(); ++j) {
					if (events[j].kind == Event::Kind::Images && events[j].handle == event.handle) imagesRead = true;
				}
				if (!imagesRead) {
					std::fprintf(stderr, "  %s: the new swapchain's images were not read\n", step);
					ok = false;
				}
			}
			if (!sawCreate) {
				std::fprintf(stderr, "  %s: no swapchain was created\n", step);
				return false;
			}
			if (!presentationLayer && !sawOverlap) {
				std::fprintf(stderr, "  %s: without a presentation layer the rebuild did not overlap swapchains\n", step);
				return false;
			}
			return ok;
		}
	}
}

// The interposer exports BasicRHI resolves from DeviceCreateInfo::StreamlineHost::interposer (this executable).
extern "C" sl::Result FakeSlGetFeatureRequirements(int, void*) { return sl::Result::eErrorNotInitialized; }
extern "C" sl::Result FakeSlSetVulkanInfo(const void*) { return sl::Result::eOk; }
extern "C" PFN_vkVoidFunction VKAPI_CALL FakeVkGetInstanceProcAddr(VkInstance instance, const char* name) {
	if (!fake::realGetInstanceProcAddr) {
		fake::realGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(LoadLibraryW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
	}
	if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
		fake::realGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fake::realGetInstanceProcAddr(instance, name));
		return reinterpret_cast<PFN_vkVoidFunction>(&fake::GetDeviceProcAddr);
	}
	return fake::realGetInstanceProcAddr(instance, name);
}
#pragma comment(linker, "/EXPORT:slShutdown")
#pragma comment(linker, "/EXPORT:slGetFeatureRequirements=FakeSlGetFeatureRequirements")
#pragma comment(linker, "/EXPORT:slSetVulkanInfo=FakeSlSetVulkanInfo")
#pragma comment(linker, "/EXPORT:vkGetInstanceProcAddr=FakeVkGetInstanceProcAddr")

namespace {
	struct Frame {
		rhi::CommandAllocatorPtr allocator;
		rhi::CommandListPtr list;
		uint64_t done = 0;
	};

	struct Context {
		rhi::Device device;
		rhi::Queue queue;
		rhi::Backend backend{};
		HWND window = nullptr;
		rhi::SwapchainPtr swapchain;
		rhi::DescriptorHeapPtr rtvHeap;
		rhi::TimelinePtr timeline;
		Frame frames[kFramesInFlight];
		uint64_t frameNumber = 0;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	bool RenderFrame(Context& c, bool vsync) {
		rhi::Swapchain& sc = c.swapchain.Get();
		Frame& frame = c.frames[c.frameNumber % kFramesInFlight];
		if (frame.done != 0 && !Ok(c.timeline->HostWait(frame.done, 5000), "Timeline::HostWait")) return false;
		const rhi::Result paced = sc.WaitForFrameLatency(1'000'000'000ull);
		if (paced != rhi::Result::Ok && !Ok(paced, "WaitForFrameLatency")) return false;
		if (!Check(sc.CurrentImageIndex() < sc.ImageCount(), "the current image index is within the image count")) return false;

		const rhi::ResourceHandle image = sc.Image(sc.CurrentImageIndex());
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
		color.clear.rgba[0] = 0.2f;
		color.clear.rgba[1] = 0.3f;
		color.clear.rgba[2] = 0.4f;
		color.clear.rgba[3] = 1.0f;
		rhi::PassBeginInfo pass{};
		pass.colors = { &color, 1 };
		pass.width = c.width;
		pass.height = c.height;
		pass.debugName = "PresentationLayerSmokeClear";
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
		if (const rhi::TimelinePoint acquired = sc.AcquirePoint(); acquired.value != 0) {
			if (!Ok(c.queue.Wait(acquired), "Queue::Wait(AcquirePoint)")) return false;
		}
		if (!Ok(c.queue.Submit(lists, submit), "Queue::Submit")) return false;
		frame.done = signal.value;

		rhi::PresentSyncDesc sync{};
		sync.queue = c.queue;
		sync.wait = signal;
		const rhi::Result present = sc.Present(vsync, sync);
		c.queue.CheckDebugMessages();
		PumpMessages();
		return present == rhi::Result::Ok || present == rhi::Result::ModeChanged || Ok(present, "Swapchain::Present");
	}

	bool RenderFrames(Context& c, bool vsync, uint32_t count) {
		for (uint32_t i = 0; i < count; ++i) {
			if (!RenderFrame(c, vsync)) return false;
		}
		return true;
	}

	rhi::SwapchainDesc Desc(const Context& c, bool presentationLayer, rhi::SwapchainColorSpace colorSpace) {
		rhi::SwapchainDesc desc{};
		desc.hwnd = c.window;
		desc.width = c.width;
		desc.height = c.height;
		desc.format = rhi::Format::R8G8B8A8_UNorm;
		desc.bufferCount = 3;
		desc.allowTearing = true;
		desc.colorSpace = colorSpace;
		desc.presentationLayer = presentationLayer;
		return desc;
	}

	// The present queue must be idle before a swapchain goes, as a host's replacement waits for its presents.
	bool Replace(Context& c, const rhi::SwapchainDesc& desc) {
		if (!Ok(c.device.WaitIdle(), "Device::WaitIdle")) return false;
		for (Frame& frame : c.frames) frame.done = 0;
		c.swapchain.Reset();
		return Ok(c.device.CreateSwapchain(desc, c.swapchain), "CreateSwapchain(desc)") && Check(static_cast<bool>(c.swapchain), "a swapchain was made") &&
			Ok(c.swapchain->SetMaximumFrameLatency(2), "SetMaximumFrameLatency");
	}

	bool WaitableValid(Context& c) {
		void* const waitable = c.swapchain->FrameLatencyWaitableObject();
		// Not waited on: a wait takes one of the frame latency counts.
		DWORD flags = 0;
		return Check(waitable && GetHandleInformation(static_cast<HANDLE>(waitable), &flags), "the frame latency waitable object is a valid handle");
	}

	bool RunVulkanChecks(Context& c, bool hdr10) {
		using fake::events;
		// Without the flag: a resize overlaps the swapchains (the recording is not vacuous).
		size_t from = events.size();
		bool ok = Replace(c, Desc(c, false, rhi::SwapchainColorSpace::SrgbNonlinear)) && RenderFrames(c, false, 3) &&
			Ok(c.swapchain->ResizeBuffers(0, c.width, c.height, rhi::Format::Unknown, 0), "ResizeBuffers") && RenderFrames(c, false, 3) &&
			fake::CheckEvents(from, false, "no presentation layer");
		if (!ok) return false;

		// With the flag: every rebuild destroys first, without chains, and reads the images again.
		from = events.size();
		ok = Replace(c, Desc(c, true, rhi::SwapchainColorSpace::SrgbNonlinear)) &&
			Check(c.swapchain->Caps().presentationLayer, "Caps().presentationLayer") &&
			Check(!c.swapchain->Caps().presentModeSwitch, "no present mode switch under a presentation layer") &&
			RenderFrames(c, false, 3) && fake::CheckEvents(from, true, "create");
		if (!ok) return false;
		from = events.size();
		ok = Ok(c.swapchain->ResizeBuffers(0, c.width, c.height, rhi::Format::Unknown, 0), "ResizeBuffers") && RenderFrames(c, false, 3) &&
			fake::CheckEvents(from, true, "resize");
		if (!ok) return false;
		from = events.size();
		// A vsync change recreates the swapchain inside Present.
		ok = RenderFrames(c, true, 3) && fake::CheckEvents(from, true, "vsync switch");
		if (!ok) return false;
		if (hdr10) {
			from = events.size();
			ok = Ok(c.swapchain->SetColorSpace(rhi::SwapchainColorSpace::Hdr10St2084), "SetColorSpace(HDR10)") && RenderFrames(c, true, 3) &&
				Ok(c.swapchain->SetColorSpace(rhi::SwapchainColorSpace::SrgbNonlinear), "SetColorSpace(SDR)") && RenderFrames(c, true, 3) &&
				fake::CheckEvents(from, true, "colour space");
			if (!ok) return false;
		}
		// The replacement a host makes: destroy, then create on the same window, in the other colour space.
		from = events.size();
		const auto colorSpace = hdr10 ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
		ok = Replace(c, Desc(c, true, colorSpace)) && Check(c.swapchain->ColorSpace() == colorSpace, "the colour space given at create") &&
			RenderFrames(c, true, 3) && fake::CheckEvents(from, true, "replacement");
		std::printf("  vulkan: %zu swapchain events recorded\n", events.size());
		return ok;
	}

	bool RunD3D12Checks(Context& c, bool hdr10) {
		using basicrhi_test::NativeSwapchainGetBufferCalls;
		basicrhi_test::ResetNativeSwapchainGetBufferCalls();
		bool ok = Replace(c, Desc(c, false, rhi::SwapchainColorSpace::SrgbNonlinear)) && RenderFrames(c, false, 3) &&
			Check(NativeSwapchainGetBufferCalls() > 0, "without a presentation layer the images come from the native swapchain");
		if (!ok) return false;

		basicrhi_test::ResetNativeSwapchainGetBufferCalls();
		ok = Replace(c, Desc(c, true, rhi::SwapchainColorSpace::SrgbNonlinear)) &&
			Check(c.swapchain->Caps().presentationLayer, "Caps().presentationLayer") && WaitableValid(c) && RenderFrames(c, false, 3) &&
			Ok(c.swapchain->ResizeBuffers(0, c.width, c.height, rhi::Format::Unknown, 0), "ResizeBuffers") && WaitableValid(c) &&
			RenderFrames(c, true, 3);
		if (ok && hdr10) {
			ok = Ok(c.swapchain->SetColorSpace(rhi::SwapchainColorSpace::Hdr10St2084), "SetColorSpace(HDR10)") && WaitableValid(c) &&
				RenderFrames(c, true, 3) && Ok(c.swapchain->SetColorSpace(rhi::SwapchainColorSpace::SrgbNonlinear), "SetColorSpace(SDR)") &&
				RenderFrames(c, true, 3);
		}
		const auto colorSpace = hdr10 ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
		ok = ok && Replace(c, Desc(c, true, colorSpace)) && Check(c.swapchain->ColorSpace() == colorSpace, "the colour space given at create") &&
			WaitableValid(c) && RenderFrames(c, true, 3) &&
			Check(NativeSwapchainGetBufferCalls() == 0, "with a presentation layer the images come from the proxy swapchain");
		std::printf("  d3d12: native swapchain GetBuffer calls under the presentation layer: %u\n", NativeSwapchainGetBufferCalls());
		return ok;
	}

	int RunBackend(rhi::Backend backend) {
		const bool vulkan = backend == rhi::Backend::Vulkan;
		const char* label = vulkan ? "vulkan" : "d3d12";
		std::printf("BasicRHIPresentationLayerSmoke [%s]\n", label);
		basicrhi_test::SetStreamlinePassThrough(!vulkan);
		rhi::DeviceCreateInfo::StreamlineHost host{};
		host.interposer = GetModuleHandleW(nullptr);
		rhi::DeviceCreateInfo createInfo{};
		createInfo.backend = backend;
		createInfo.enableDebug = true;
		createInfo.streamlineHost = &host;
		rhi::DevicePtr device;
		const rhi::Result created = vulkan ? rhi::CreateVulkanDevice(createInfo, device) : rhi::CreateD3D12Device(createInfo, device);
		if (created != rhi::Result::Ok) {
			std::printf("  no %s device (%s), skipped\n", label, rhi::ResultName(created));
			basicrhi_test::SetStreamlinePassThrough(false);
			return kSkip;
		}

		const wchar_t* className = L"BasicRHIPresentationLayerSmokeWindow";
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = WindowProc;
		windowClass.hInstance = instance;
		windowClass.lpszClassName = className;
		RegisterClassW(&windowClass);
		HWND window = CreateWindowExW(0, className, L"BasicRHIPresentationLayerSmoke", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
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
			c.backend = backend;
			c.window = window;
			c.queue = device->GetQueue(rhi::QueueKind::Graphics);
			c.width = static_cast<uint32_t>(client.right - client.left);
			c.height = static_cast<uint32_t>(client.bottom - client.top);
			rhi::DescriptorHeapDesc rtvDesc{};
			rtvDesc.type = rhi::DescriptorHeapType::RTV;
			rtvDesc.capacity = 1;
			rtvDesc.shaderVisible = false;
			rtvDesc.debugName = "PresentationLayerSmokeRTV";
			bool ready = c.queue && Ok(device->CreateDescriptorHeap(rtvDesc, c.rtvHeap), "CreateDescriptorHeap") &&
				Ok(device->CreateTimeline(c.timeline, 0, "PresentationLayerSmokeFrames"), "CreateTimeline");
			for (Frame& frame : c.frames) {
				ready = ready && Ok(device->CreateCommandAllocator(rhi::QueueKind::Graphics, frame.allocator), "CreateCommandAllocator") &&
					Ok(device->CreateCommandList(rhi::QueueKind::Graphics, frame.allocator.Get(), frame.list), "CreateCommandList");
				if (ready) frame.list->End();
			}
			// HDR10 is tried only where the output shows it.
			bool hdr10 = false;
			if (ready) {
				ready = Replace(c, Desc(c, false, rhi::SwapchainColorSpace::SrgbNonlinear));
				hdr10 = ready && c.swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084);
				std::printf("  HDR10 %s\n", hdr10 ? "supported: colour space steps run" : "not supported by the output: colour space steps skipped");
			}
			ready = ready && (vulkan ? RunVulkanChecks(c, hdr10) : RunD3D12Checks(c, hdr10));
			if (!ready) status = 1;
			if (!Ok(device->WaitIdle(), "Device::WaitIdle")) status = 1;
		}
		DestroyWindow(window);
		UnregisterClassW(className, instance);
		basicrhi_test::SetStreamlinePassThrough(false);
		std::printf("BasicRHIPresentationLayerSmoke [%s]: %s\n", label, status == 0 ? "success" : "FAILED");
		return status;
	}
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
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
