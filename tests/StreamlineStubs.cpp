#include "sl.h"

#include "StreamlineStubs.h"

#include <atomic>

#ifdef _WIN32
#include <Windows.h>
#include <dxgi1_6.h>
#endif

namespace {
	std::atomic<bool> g_passThrough{ false };
	std::atomic<uint32_t> g_nativeGetBufferCalls{ 0 };

#ifdef _WIN32
	// A native swapchain interface distinct from the proxy (the real swapchain), so a test can tell which one BasicRHI
	// reads its images from.
	class NativeSwapchain final : public IDXGISwapChain3 {
	public:
		explicit NativeSwapchain(IDXGISwapChain3* inner) noexcept : inner_(inner) { inner_->AddRef(); }

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
			if (!out) return E_POINTER;
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
				riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
				riid == __uuidof(IDXGISwapChain3)) {
				AddRef();
				*out = static_cast<IDXGISwapChain3*>(this);
				return S_OK;
			}
			return inner_->QueryInterface(riid, out);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
		ULONG STDMETHODCALLTYPE Release() override {
			const ULONG left = --refs_;
			if (left == 0) {
				inner_->Release();
				delete this;
			}
			return left;
		}

		HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { return inner_->SetPrivateData(g, s, d); }
		HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return inner_->SetPrivateDataInterface(g, u); }
		HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { return inner_->GetPrivateData(g, s, d); }
		HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return inner_->GetParent(r, p); }
		HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** d) override { return inner_->GetDevice(r, d); }

		HRESULT STDMETHODCALLTYPE Present(UINT i, UINT f) override { return inner_->Present(i, f); }
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID r, void** s) override {
			++g_nativeGetBufferCalls;
			return inner_->GetBuffer(b, r, s);
		}
		HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* t) override { return inner_->SetFullscreenState(f, t); }
		HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** t) override { return inner_->GetFullscreenState(f, t); }
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return inner_->GetDesc(d); }
		HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override { return inner_->ResizeBuffers(c, w, h, f, fl); }
		HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* d) override { return inner_->ResizeTarget(d); }
		HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return inner_->GetContainingOutput(o); }
		HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return inner_->GetFrameStatistics(s); }
		HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return inner_->GetLastPresentCount(c); }

		HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return inner_->GetDesc1(d); }
		HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return inner_->GetFullscreenDesc(d); }
		HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return inner_->GetHwnd(h); }
		HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** u) override { return inner_->GetCoreWindow(r, u); }
		HRESULT STDMETHODCALLTYPE Present1(UINT i, UINT f, const DXGI_PRESENT_PARAMETERS* p) override { return inner_->Present1(i, f, p); }
		BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return inner_->IsTemporaryMonoSupported(); }
		HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return inner_->GetRestrictToOutput(o); }
		HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return inner_->SetBackgroundColor(c); }
		HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return inner_->GetBackgroundColor(c); }
		HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return inner_->SetRotation(r); }
		HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return inner_->GetRotation(r); }

		HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return inner_->SetSourceSize(w, h); }
		HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return inner_->GetSourceSize(w, h); }
		HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override { return inner_->SetMaximumFrameLatency(m); }
		HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override { return inner_->GetMaximumFrameLatency(m); }
		HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return inner_->GetFrameLatencyWaitableObject(); }
		HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return inner_->SetMatrixTransform(m); }
		HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return inner_->GetMatrixTransform(m); }

		UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return inner_->GetCurrentBackBufferIndex(); }
		HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return inner_->CheckColorSpaceSupport(c, s); }
		HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return inner_->SetColorSpace1(c); }
		HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* m, IUnknown* const* q) override {
			return inner_->ResizeBuffers1(c, w, h, f, fl, m, q);
		}

	private:
		~NativeSwapchain() = default;
		IDXGISwapChain3* inner_;
		std::atomic<ULONG> refs_{ 1 };
	};
#endif
}

namespace basicrhi_test {
	void SetStreamlinePassThrough(bool on) noexcept { g_passThrough = on; }
	uint32_t NativeSwapchainGetBufferCalls() noexcept { return g_nativeGetBufferCalls; }
	void ResetNativeSwapchainGetBufferCalls() noexcept { g_nativeGetBufferCalls = 0; }
}

extern "C" sl::Result slInit(const sl::Preferences&, uint64_t) { return sl::Result::eErrorNotInitialized; }
extern "C" sl::Result slShutdown() { return sl::Result::eOk; }

// Pass-through: the upgraded interface is the object itself, with the reference the caller attaches.
extern "C" sl::Result slUpgradeInterface(void** object) {
#ifdef _WIN32
	if (g_passThrough && object && *object) {
		static_cast<IUnknown*>(*object)->AddRef();
		return sl::Result::eOk;
	}
#endif
	return sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slGetNativeInterface(void* proxy, void** native) {
#ifdef _WIN32
	if (g_passThrough && proxy && native) {
		auto* unknown = static_cast<IUnknown*>(proxy);
		IDXGISwapChain3* swapchain = nullptr;
		if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapchain)))) {
			*native = static_cast<IDXGISwapChain3*>(new NativeSwapchain(swapchain));
			swapchain->Release();
			return sl::Result::eOk;
		}
		unknown->AddRef();
		*native = proxy;
		return sl::Result::eOk;
	}
#endif
	return sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slSetD3DDevice(void*) { return g_passThrough ? sl::Result::eOk : sl::Result::eErrorNotInitialized; }
