#pragma once

#include <cstdint>

// The Streamline C API stubs (StreamlineStubs.cpp). By default every call fails, so BasicRHI runs without Streamline. In
// pass-through mode a D3D12 device made with DeviceCreateInfo::streamlineHost sees Streamline accept it. The proxies are
// the native objects themselves, except a swapchain's native interface, which is a wrapper that counts its GetBuffer calls.
namespace basicrhi_test {
	void SetStreamlinePassThrough(bool on) noexcept;
	// GetBuffer calls on native swapchain interfaces handed out since the last reset.
	uint32_t NativeSwapchainGetBufferCalls() noexcept;
	void ResetNativeSwapchainGetBufferCalls() noexcept;
}
