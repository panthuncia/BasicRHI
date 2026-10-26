#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_feature_info.h"
#include "rhi_interop_vulkan.h"

// FidelityFX validation is optional: the smoke still exercises the RHI when
// the FFX SDK is not part of the build tree.
#if __has_include("ThirdParty/FFX/ffx_api_loader.h") && __has_include(<FidelityFX/host/ffx_fsr3upscaler.h>)
#define BASICRHI_SMOKE_HAS_FFX 1
#include "ThirdParty/FFX/ffx_api_loader.h"
#include "ThirdParty/FFX/ffx_upscale.h"
#include "ThirdParty/FFX/host/backends/vk/ffx_vk.h"
#include "ThirdParty/FFX/vk/ffx_api_vk.h"
#else
#define BASICRHI_SMOKE_HAS_FFX 0
#endif

#if BASICRHI_SMOKE_HAS_FFX
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
	template<typename Function>
	bool LoadFfxBackendFunction(HMODULE module, Function& target, const char* name) {
		target = reinterpret_cast<Function>(GetProcAddress(module, name));
		if (target == nullptr) {
			std::fprintf(stderr, "FidelityFX Vulkan backend module is missing export %s\n", name);
			return false;
		}
		return true;
	}

#if BASICRHI_SMOKE_HAS_FFX
	bool ValidateFidelityFXVulkanBackend(
		HMODULE ffxApiModule,
		VkDevice vkDevice,
		VkPhysicalDevice vkPhysicalDevice,
		PFN_vkGetDeviceProcAddr vkDeviceProcAddr) {
		using PfnGetScratchMemorySizeVK = decltype(&ffxGetScratchMemorySizeVK);
		using PfnGetDeviceVK = decltype(&ffxGetDeviceVK);
		using PfnGetInterfaceVK = decltype(&ffxGetInterfaceVK);
		using PfnFsr3UpscalerContextCreate = decltype(&ffxFsr3UpscalerContextCreate);
		using PfnFsr3UpscalerContextDestroy = decltype(&ffxFsr3UpscalerContextDestroy);
		using PfnFsr3UpscalerGetSharedResourceDescriptions = decltype(&ffxFsr3UpscalerGetSharedResourceDescriptions);

		HMODULE ffxBackendModule = LoadLibraryW(L"ffx_backend_vk_x64drel.dll");
		if (ffxBackendModule == nullptr) {
			std::fprintf(stderr, "LoadLibraryW(ffx_backend_vk_x64drel.dll) failed with error %lu\n", GetLastError());
			return false;
		}

		PfnGetScratchMemorySizeVK getScratchMemorySizeVK = nullptr;
		PfnGetDeviceVK getDeviceVK = nullptr;
		PfnGetInterfaceVK getInterfaceVK = nullptr;
		if (!LoadFfxBackendFunction(ffxBackendModule, getScratchMemorySizeVK, "ffxGetScratchMemorySizeVK") ||
			!LoadFfxBackendFunction(ffxBackendModule, getDeviceVK, "ffxGetDeviceVK") ||
			!LoadFfxBackendFunction(ffxBackendModule, getInterfaceVK, "ffxGetInterfaceVK")) {
			FreeLibrary(ffxBackendModule);
			return false;
		}

		VkDeviceContext deviceContext{};
		deviceContext.vkDevice = vkDevice;
		deviceContext.vkPhysicalDevice = vkPhysicalDevice;
		deviceContext.vkDeviceProcAddr = vkDeviceProcAddr;

		std::fprintf(stderr, "FFX Vulkan smoke: calling ffxGetDeviceVK\n");
		std::fflush(stderr);
		const FfxDevice ffxDevice = getDeviceVK(&deviceContext);

		std::fprintf(stderr, "FFX Vulkan smoke: calling ffxGetScratchMemorySizeVK\n");
		std::fflush(stderr);
		const size_t scratchMemorySize = getScratchMemorySizeVK(vkPhysicalDevice, 1);
		std::fprintf(stderr, "FFX Vulkan smoke: scratchMemorySize=%zu\n", scratchMemorySize);
		std::fflush(stderr);
		if (scratchMemorySize == 0) {
			FreeLibrary(ffxBackendModule);
			return false;
		}

		void* scratchMemory = std::malloc(scratchMemorySize);
		if (scratchMemory == nullptr) {
			std::fprintf(stderr, "FFX Vulkan smoke: malloc(%zu) failed\n", scratchMemorySize);
			FreeLibrary(ffxBackendModule);
			return false;
		}
		std::memset(scratchMemory, 0, scratchMemorySize);

		FfxInterface backendInterface{};
		std::fprintf(stderr, "FFX Vulkan smoke: calling ffxGetInterfaceVK\n");
		std::fflush(stderr);
		const FfxErrorCode interfaceResult = getInterfaceVK(&backendInterface, ffxDevice, scratchMemory, scratchMemorySize, 1);
		std::fprintf(stderr, "FFX Vulkan smoke: ffxGetInterfaceVK returned %d\n", static_cast<int>(interfaceResult));
		std::fflush(stderr);
		if (interfaceResult != FFX_OK) {
			std::free(scratchMemory);
			FreeLibrary(ffxBackendModule);
			return false;
		}

		PfnFsr3UpscalerContextCreate fsr3Create = nullptr;
		PfnFsr3UpscalerContextDestroy fsr3Destroy = nullptr;
		PfnFsr3UpscalerGetSharedResourceDescriptions fsr3GetSharedResourceDescriptions = nullptr;
		if (LoadFfxBackendFunction(ffxApiModule, fsr3Create, "ffxFsr3UpscalerContextCreate") &&
			LoadFfxBackendFunction(ffxApiModule, fsr3Destroy, "ffxFsr3UpscalerContextDestroy") &&
			LoadFfxBackendFunction(ffxApiModule, fsr3GetSharedResourceDescriptions, "ffxFsr3UpscalerGetSharedResourceDescriptions")) {
			FfxFsr3UpscalerContextDescription fsr3Desc{};
			fsr3Desc.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE | FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
			fsr3Desc.maxRenderSize = { 320u, 240u };
			fsr3Desc.maxUpscaleSize = { 320u, 240u };
			fsr3Desc.backendInterface = backendInterface;

			FfxFsr3UpscalerContext fsr3Context{};
			std::fprintf(stderr, "FFX Vulkan smoke: calling ffxFsr3UpscalerContextCreate directly\n");
			std::fflush(stderr);
			const FfxErrorCode fsr3CreateResult = fsr3Create(&fsr3Context, &fsr3Desc);
			std::fprintf(stderr, "FFX Vulkan smoke: ffxFsr3UpscalerContextCreate returned %d\n", static_cast<int>(fsr3CreateResult));
			std::fflush(stderr);

			if (fsr3CreateResult == FFX_OK) {
				FfxFsr3UpscalerSharedResourceDescriptions sharedDescriptions{};
				std::fprintf(stderr, "FFX Vulkan smoke: calling ffxFsr3UpscalerGetSharedResourceDescriptions directly\n");
				std::fflush(stderr);
				const FfxErrorCode sharedDescriptionsResult = fsr3GetSharedResourceDescriptions(&fsr3Context, &sharedDescriptions);
				std::fprintf(stderr, "FFX Vulkan smoke: ffxFsr3UpscalerGetSharedResourceDescriptions returned %d\n", static_cast<int>(sharedDescriptionsResult));
				std::fflush(stderr);

				std::fprintf(stderr, "FFX Vulkan smoke: calling ffxFsr3UpscalerContextDestroy directly\n");
				std::fflush(stderr);
				const FfxErrorCode fsr3DestroyResult = fsr3Destroy(&fsr3Context);
				std::fprintf(stderr, "FFX Vulkan smoke: ffxFsr3UpscalerContextDestroy returned %d\n", static_cast<int>(fsr3DestroyResult));
				std::fflush(stderr);
			}
		}

		std::free(scratchMemory);
		FreeLibrary(ffxBackendModule);
		return true;
	}

	bool ValidateFidelityFXVulkanContext(rhi::Device& device) {
		HMODULE ffxApiModule = LoadLibraryW(L"amd_fidelityfx_vk.dll");
		if (ffxApiModule == nullptr) {
			std::fprintf(stderr, "LoadLibraryW(amd_fidelityfx_vk.dll) failed with error %lu\n", GetLastError());
			return false;
		}

		ffxFunctions ffxApi{};
		ffxLoadFunctions(&ffxApi, ffxApiModule);
		if (ffxApi.CreateContext == nullptr || ffxApi.DestroyContext == nullptr) {
			std::fprintf(stderr, "FidelityFX Vulkan API module is missing context exports\n");
			FreeLibrary(ffxApiModule);
			return false;
		}

		const VkDevice vkDevice = rhi::vulkan::get_device(device);
		const VkPhysicalDevice vkPhysicalDevice = rhi::vulkan::get_physical_device(device);
		const PFN_vkGetDeviceProcAddr vkDeviceProcAddr = rhi::vulkan::get_device_proc_addr();
		const PFN_vkVoidFunction createDescriptorPoolProc = vkDeviceProcAddr && vkDevice != VK_NULL_HANDLE
			? vkDeviceProcAddr(vkDevice, "vkCreateDescriptorPool")
			: nullptr;

		std::fprintf(
			stderr,
			"FFX Vulkan smoke handles: device=%p physicalDevice=%p vkGetDeviceProcAddr=%p vkCreateDescriptorPool=%p\n",
			reinterpret_cast<void*>(vkDevice),
			reinterpret_cast<void*>(vkPhysicalDevice),
			reinterpret_cast<void*>(vkDeviceProcAddr),
			reinterpret_cast<void*>(createDescriptorPoolProc));

		if (vkDevice == VK_NULL_HANDLE || vkPhysicalDevice == VK_NULL_HANDLE || vkDeviceProcAddr == nullptr || createDescriptorPoolProc == nullptr) {
			std::fprintf(stderr, "FidelityFX Vulkan smoke skipped because Vulkan native handles or proc loader are invalid\n");
			FreeLibrary(ffxApiModule);
			return false;
		}

		if (!ValidateFidelityFXVulkanBackend(ffxApiModule, vkDevice, vkPhysicalDevice, vkDeviceProcAddr)) {
			std::fprintf(stderr, "FidelityFX Vulkan backend probe failed before API CreateContext\n");
			FreeLibrary(ffxApiModule);
			return false;
		}

		ffxCreateBackendVKDesc backendDesc{};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
		backendDesc.vkDevice = vkDevice;
		backendDesc.vkPhysicalDevice = vkPhysicalDevice;
		backendDesc.vkDeviceProcAddr = vkDeviceProcAddr;

		ffxCreateContextDescUpscale createUpscale{};
		createUpscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		createUpscale.header.pNext = &backendDesc.header;
		createUpscale.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
		createUpscale.maxRenderSize = { 320u, 240u };
		createUpscale.maxUpscaleSize = { 320u, 240u };

		ffxContext context = nullptr;
		std::fprintf(stderr, "FFX Vulkan smoke: calling ffxCreateContext\n");
		std::fflush(stderr);
		const ffxReturnCode_t createResult = ffxApi.CreateContext(&context, &createUpscale.header, nullptr);
		if (createResult != FFX_API_RETURN_OK) {
			std::fprintf(stderr, "ffxCreateContext Vulkan upscale failed with code %u\n", static_cast<unsigned>(createResult));
			FreeLibrary(ffxApiModule);
			return false;
		}
		std::fprintf(stderr, "FFX Vulkan smoke: ffxCreateContext returned OK\n");
		std::fflush(stderr);

		const ffxReturnCode_t destroyResult = ffxApi.DestroyContext(&context, nullptr);
		FreeLibrary(ffxApiModule);
		if (destroyResult != FFX_API_RETURN_OK) {
			std::fprintf(stderr, "ffxDestroyContext Vulkan upscale failed with code %u\n", static_cast<unsigned>(destroyResult));
			return false;
		}
		std::fprintf(stderr, "FFX Vulkan smoke: ffxDestroyContext returned OK\n");
		std::fflush(stderr);

		return true;
	}

#endif // BASICRHI_SMOKE_HAS_FFX

	rhi::Result Check(rhi::Result result, const char* what) {
		if (result != rhi::Result::Ok) {
			std::fprintf(stderr, "%s failed with result %u\n", what, static_cast<unsigned>(result));
		}
		return result;
	}

	// The binding model the device runs on (rhi::vulkan::BindingModelBits): set once in main, named in failure messages and
	// used by the cases whose expectations differ between the models.
	uint32_t g_bindingModel = 0;

	const char* BindingModelName(uint32_t model) {
		switch (model) {
		case rhi::vulkan::BindingModel_DescriptorHeap: return "descriptor heap";
		case rhi::vulkan::BindingModel_DescriptorBuffer: return "descriptor buffer";
		default: return "unknown model";
		}
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

		rhi::TextureBarrier beginBarriers[2]{};
		uint32_t beginBarrierCount = 0;
		beginBarriers[beginBarrierCount++] = rhi::TextureBarrier{
			.texture = imageHandle,
			.range = {},
			.beforeSync = rhi::ResourceSyncState::None,
			.afterSync = rhi::ResourceSyncState::RenderTarget,
			.beforeAccess = rhi::ResourceAccessType::None,
			.afterAccess = rhi::ResourceAccessType::RenderTarget,
			.beforeLayout = rhi::ResourceLayout::Undefined,
			.afterLayout = rhi::ResourceLayout::RenderTarget,
			.discard = true,
		};
		if (depthAttachment != nullptr && depthAttachment->resource.valid()) {
			beginBarriers[beginBarrierCount++] = rhi::TextureBarrier{
				.texture = depthAttachment->resource,
				.range = {},
				.beforeSync = rhi::ResourceSyncState::None,
				.afterSync = rhi::ResourceSyncState::DepthStencil,
				.beforeAccess = rhi::ResourceAccessType::None,
				.afterAccess = rhi::ResourceAccessType::DepthReadWrite,
				.beforeLayout = rhi::ResourceLayout::Undefined,
				.afterLayout = rhi::ResourceLayout::DepthReadWrite,
				.discard = true,
			};
		}
		commandList.Barriers(rhi::BarrierBatch{ .textures = { beginBarriers, beginBarrierCount } });

		const rhi::PassBeginInfo passInfo{
			.colors = { &color, 1 },
			.depth = depthAttachment,
			.width = width,
			.height = height,
			.debugName = "VulkanSmokeClear"
		};

		commandList.BeginPass(passInfo);
		commandList.EndPass();

		rhi::TextureBarrier presentBarrier{
			.texture = imageHandle,
			.range = {},
			.beforeSync = rhi::ResourceSyncState::RenderTarget,
			.afterSync = rhi::ResourceSyncState::All,
			.beforeAccess = rhi::ResourceAccessType::RenderTarget,
			.afterAccess = rhi::ResourceAccessType::Present,
			.beforeLayout = rhi::ResourceLayout::RenderTarget,
			.afterLayout = rhi::ResourceLayout::Present,
			.discard = false,
		};
		commandList.Barriers(rhi::BarrierBatch{ .textures = { &presentBarrier, 1 } });
		commandList.End();

		const rhi::CommandList submitLists[] = { commandList };
		return Check(graphicsQueue.Submit(submitLists, {}), "Queue::Submit clear pass");
	}

	// Cross-queue ownership: a device-local buffer is written on the copy
	// queue and read back on the graphics queue, ordered by a timeline. With
	// QueueSharing::Concurrent the ownership barriers collapse; with Exclusive
	// they become a VK_SHARING_MODE_EXCLUSIVE release/acquire pair. Both must
	// read back the written pattern under validation without family errors.
	rhi::Result ValidateCrossQueueOwnership(rhi::Device& device, rhi::Queue graphicsQueue, rhi::QueueSharing sharing, const char* label) {
		auto copyQueue = device.GetQueue(rhi::QueueKind::Copy);
		if (!copyQueue) {
			std::printf("Cross-queue ownership (%s): no copy queue, skipped\n", label);
			return rhi::Result::Ok;
		}
		constexpr uint64_t kBytes = 256;
		auto deviceDesc = rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, {}, "VulkanSmokeOwnershipDevice");
		deviceDesc.queueSharing = sharing;
		rhi::ResourcePtr deviceBuffer;
		if (Check(device.CreateCommittedResource(deviceDesc, deviceBuffer), "CreateCommittedResource ownership device buffer") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		rhi::ResourcePtr uploadBuffer;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::Upload, {}, "VulkanSmokeOwnershipUpload"), uploadBuffer), "CreateCommittedResource ownership upload") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		rhi::ResourcePtr readbackBuffer;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::Readback, {}, "VulkanSmokeOwnershipReadback"), readbackBuffer), "CreateCommittedResource ownership readback") != rhi::Result::Ok) return rhi::Result::InvalidArgument;

		std::vector<uint32_t> pattern(kBytes / sizeof(uint32_t));
		for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = 0xA5000000u ^ static_cast<uint32_t>(i * 2654435761u);
		void* mapped = nullptr;
		uploadBuffer->Map(&mapped, 0, kBytes);
		if (!mapped) { std::fprintf(stderr, "ownership upload map failed\n"); return rhi::Result::InvalidArgument; }
		std::memcpy(mapped, pattern.data(), kBytes);
		uploadBuffer->Unmap(0, kBytes);

		rhi::TimelinePtr timeline;
		if (Check(device.CreateTimeline(timeline, 0, "VulkanSmokeOwnershipTimeline"), "CreateTimeline ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;

		// Copy queue: write, then release to the graphics family.
		rhi::CommandAllocatorPtr copyAllocator;
		rhi::CommandListPtr copyList;
		if (Check(device.CreateCommandAllocator(rhi::QueueKind::Copy, copyAllocator), "CreateCommandAllocator copy") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		if (Check(device.CreateCommandList(rhi::QueueKind::Copy, copyAllocator.Get(), copyList), "CreateCommandList copy") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		{
			rhi::BufferBarrier toCopyDest{
				.buffer = deviceBuffer->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::All,
				.afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::Common,
				.afterAccess = rhi::ResourceAccessType::CopyDest,
			};
			copyList->Barriers(rhi::BarrierBatch{ .buffers = { &toCopyDest, 1 } });
			copyList->CopyBufferRegion(deviceBuffer->GetHandle(), 0, uploadBuffer->GetHandle(), 0, kBytes);
			rhi::BufferBarrier release{
				.buffer = deviceBuffer->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::Copy,
				.afterSync = rhi::ResourceSyncState::All,
				.beforeAccess = rhi::ResourceAccessType::CopyDest,
				.afterAccess = rhi::ResourceAccessType::Common,
				.queueOwnership = rhi::QueueOwnership::Release,
				.ownershipPeer = rhi::QueueKind::Graphics,
			};
			copyList->Barriers(rhi::BarrierBatch{ .buffers = { &release, 1 } });
			copyList->End();
			const rhi::CommandList lists[] = { copyList.Get() };
			const rhi::TimelinePoint signal{ timeline->GetHandle(), 1 };
			if (Check(copyQueue.Submit(lists, { .signals = { &signal, 1 } }), "Queue::Submit copy ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		}

		// Graphics queue: acquire from the copy family, read back.
		rhi::CommandAllocatorPtr graphicsAllocator;
		rhi::CommandListPtr graphicsList;
		if (Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, graphicsAllocator), "CreateCommandAllocator graphics ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		if (Check(device.CreateCommandList(rhi::QueueKind::Graphics, graphicsAllocator.Get(), graphicsList), "CreateCommandList graphics ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		{
			rhi::BufferBarrier acquire{
				.buffer = deviceBuffer->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::All,
				.afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::Common,
				.afterAccess = rhi::ResourceAccessType::CopySource,
				.queueOwnership = rhi::QueueOwnership::Acquire,
				.ownershipPeer = rhi::QueueKind::Copy,
			};
			graphicsList->Barriers(rhi::BarrierBatch{ .buffers = { &acquire, 1 } });
			graphicsList->CopyBufferRegion(readbackBuffer->GetHandle(), 0, deviceBuffer->GetHandle(), 0, kBytes);
			graphicsList->End();
			const rhi::CommandList lists[] = { graphicsList.Get() };
			const rhi::TimelinePoint wait{ timeline->GetHandle(), 1 };
			if (Check(graphicsQueue.Submit(lists, { .waits = { &wait, 1 } }), "Queue::Submit graphics ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;
		}
		if (Check(device.WaitIdle(), "Device::WaitIdle ownership") != rhi::Result::Ok) return rhi::Result::InvalidArgument;

		void* readback = nullptr;
		readbackBuffer->Map(&readback, 0, kBytes);
		if (!readback) { std::fprintf(stderr, "ownership readback map failed\n"); return rhi::Result::InvalidArgument; }
		const bool match = std::memcmp(readback, pattern.data(), kBytes) == 0;
		readbackBuffer->Unmap(0, 0);
		if (!match) {
			std::fprintf(stderr, "Cross-queue ownership (%s): readback mismatch\n", label);
			return rhi::Result::InvalidArgument;
		}
		std::printf("Cross-queue ownership (%s): ok\n", label);
		return rhi::Result::Ok;
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

	rhi::Result ValidateDescriptors(
		rhi::Device& device,
		rhi::CommandList& commandList,
		rhi::DescriptorHeapPtr& shaderVisibleHeap,
		rhi::DescriptorHeapPtr& samplerHeap) {
		rhi::DescriptorHeapPtr cpuVisibleHeap;

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

	// Device-generated commands with an indirect pipeline set: one ExecuteIndirect draws kCommands
	// quads into columns of an R32_UINT target. Each command selects a pixel shader (writing 11, 22 or
	// 33) from the set, sets its column through a root constant, binds the index buffer through an
	// IndexBuffer argument in D3D12 layout, and draws. The count buffer stops one command early.
	rhi::Result ValidateIndirectPipelineSet(
		rhi::Device& device,
		rhi::Queue& graphicsQueue,
		rhi::CommandAllocator& allocator,
		rhi::CommandList& commandList,
		rhi::DescriptorHeap& shaderVisibleHeap,
		rhi::DescriptorHeap& samplerHeap) {
		static constexpr uint32_t kDgcVertexSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000027,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0008000f,0x00000000,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,
			0x00030003,0x00000005,0x00000258,0x00060005,0x00000005,0x65707974,
			0x6172442e,0x74614477,0x00000061,0x00050006,0x00000005,0x00000000,
			0x77617264,0x00006449,0x00050005,0x00000004,0x77617244,0x61746144,
			0x00000000,0x00040005,0x00000001,0x6e69616d,0x00000000,0x00040047,
			0x00000002,0x0000000b,0x0000002a,0x00040047,0x00000003,0x0000000b,
			0x00000000,0x00040047,0x00000004,0x00000022,0x00000000,0x00040047,
			0x00000004,0x00000021,0x00000000,0x00050048,0x00000005,0x00000000,
			0x00000023,0x00000000,0x00030047,0x00000005,0x00000002,0x00040015,
			0x00000006,0x00000020,0x00000000,0x0004002b,0x00000006,0x00000007,
			0x00000001,0x00030016,0x00000008,0x00000020,0x0004002b,0x00000008,
			0x00000009,0xbf800000,0x00040015,0x0000000a,0x00000020,0x00000001,
			0x0004002b,0x0000000a,0x0000000b,0x00000000,0x0004002b,0x00000008,
			0x0000000c,0x3e800000,0x0004002b,0x00000008,0x0000000d,0x40000000,
			0x0004002b,0x00000008,0x0000000e,0x00000000,0x0004002b,0x00000008,
			0x0000000f,0x3f800000,0x0003001e,0x00000005,0x00000006,0x00040020,
			0x00000010,0x00000002,0x00000005,0x00040020,0x00000011,0x00000001,
			0x00000006,0x00040017,0x00000012,0x00000008,0x00000004,0x00040020,
			0x00000013,0x00000003,0x00000012,0x00020013,0x00000014,0x00030021,
			0x00000015,0x00000014,0x00040020,0x00000016,0x00000002,0x00000006,
			0x0004003b,0x00000010,0x00000004,0x00000002,0x0004003b,0x00000011,
			0x00000002,0x00000001,0x0004003b,0x00000013,0x00000003,0x00000003,
			0x00050036,0x00000014,0x00000001,0x00000000,0x00000015,0x000200f8,
			0x00000017,0x0004003d,0x00000006,0x00000018,0x00000002,0x000500c7,
			0x00000006,0x00000019,0x00000018,0x00000007,0x00040070,0x00000008,
			0x0000001a,0x00000019,0x000500c2,0x00000006,0x0000001b,0x00000018,
			0x00000007,0x00040070,0x00000008,0x0000001c,0x0000001b,0x00050041,
			0x00000016,0x0000001d,0x00000004,0x0000000b,0x0004003d,0x00000006,
			0x0000001e,0x0000001d,0x00040070,0x00000008,0x0000001f,0x0000001e,
			0x00050085,0x00000008,0x00000020,0x0000001f,0x0000000c,0x00050081,
			0x00000008,0x00000021,0x00000009,0x00000020,0x00050085,0x00000008,
			0x00000022,0x0000001a,0x0000000c,0x00050081,0x00000008,0x00000023,
			0x00000021,0x00000022,0x00050085,0x00000008,0x00000024,0x0000001c,
			0x0000000d,0x00050081,0x00000008,0x00000025,0x00000009,0x00000024,
			0x00070050,0x00000012,0x00000026,0x00000023,0x00000025,0x0000000e,
			0x0000000f,0x0003003e,0x00000003,0x00000026,0x000100fd,0x00010038,
		};
		static constexpr uint32_t kDgcPixel11Spirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000009,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000004,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00030010,0x00000001,
			0x00000007,0x00030003,0x00000005,0x00000258,0x00070005,0x00000002,
			0x2e74756f,0x2e726176,0x545f5653,0x65677261,0x00000074,0x00040005,
			0x00000001,0x6e69616d,0x00000000,0x00040047,0x00000002,0x0000001e,
			0x00000000,0x00040015,0x00000003,0x00000020,0x00000000,0x0004002b,
			0x00000003,0x00000004,0x0000000b,0x00040020,0x00000005,0x00000003,
			0x00000003,0x00020013,0x00000006,0x00030021,0x00000007,0x00000006,
			0x0004003b,0x00000005,0x00000002,0x00000003,0x00050036,0x00000006,
			0x00000001,0x00000000,0x00000007,0x000200f8,0x00000008,0x0003003e,
			0x00000002,0x00000004,0x000100fd,0x00010038,
		};
		static constexpr uint32_t kDgcPixel22Spirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000009,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000004,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00030010,0x00000001,
			0x00000007,0x00030003,0x00000005,0x00000258,0x00070005,0x00000002,
			0x2e74756f,0x2e726176,0x545f5653,0x65677261,0x00000074,0x00040005,
			0x00000001,0x6e69616d,0x00000000,0x00040047,0x00000002,0x0000001e,
			0x00000000,0x00040015,0x00000003,0x00000020,0x00000000,0x0004002b,
			0x00000003,0x00000004,0x00000016,0x00040020,0x00000005,0x00000003,
			0x00000003,0x00020013,0x00000006,0x00030021,0x00000007,0x00000006,
			0x0004003b,0x00000005,0x00000002,0x00000003,0x00050036,0x00000006,
			0x00000001,0x00000000,0x00000007,0x000200f8,0x00000008,0x0003003e,
			0x00000002,0x00000004,0x000100fd,0x00010038,
		};
		static constexpr uint32_t kDgcPixel33Spirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000009,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000004,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00030010,0x00000001,
			0x00000007,0x00030003,0x00000005,0x00000258,0x00070005,0x00000002,
			0x2e74756f,0x2e726176,0x545f5653,0x65677261,0x00000074,0x00040005,
			0x00000001,0x6e69616d,0x00000000,0x00040047,0x00000002,0x0000001e,
			0x00000000,0x00040015,0x00000003,0x00000020,0x00000000,0x0004002b,
			0x00000003,0x00000004,0x00000021,0x00040020,0x00000005,0x00000003,
			0x00000003,0x00020013,0x00000006,0x00030021,0x00000007,0x00000006,
			0x0004003b,0x00000005,0x00000002,0x00000003,0x00050036,0x00000006,
			0x00000001,0x00000000,0x00000007,0x000200f8,0x00000008,0x0003003e,
			0x00000002,0x00000004,0x000100fd,0x00010038,
		};

		IndirectCommandsFeatureInfo indirect{};
		if (Check(device.QueryFeatureInfo(&indirect.header), "QueryFeatureInfo IndirectCommands") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		if (!indirect.pipelineSets || !indirect.indexBufferArguments) {
			std::puts("BasicRHIVulkanSmoke: indirect pipeline set validation skipped (not supported by this device)");
			return rhi::Result::Ok;
		}

		constexpr uint32_t kWidth = 64;
		constexpr uint32_t kHeight = 16;
		constexpr uint32_t kCommands = 8;         // one 8-texel column each
		constexpr uint32_t kExecutedCommands = 7; // from the count buffer
		constexpr uint32_t kColumnValues[3] = { 11u, 22u, 33u };

		rhi::PushConstantRangeDesc drawData{};
		drawData.visibility = rhi::ShaderStage::Vertex;
		drawData.num32BitValues = 1;
		drawData.set = 0;
		drawData.binding = 0;
		rhi::PipelineLayoutPtr layout;
		if (Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { &drawData, 1 }, .staticSamplers = {}, .flags = rhi::PF_None }, layout),
				"CreatePipelineLayout indirect set") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		const uint32_t* pixelShaders[3] = { kDgcPixel11Spirv, kDgcPixel22Spirv, kDgcPixel33Spirv };
		const uint32_t pixelSizes[3] = { sizeof(kDgcPixel11Spirv), sizeof(kDgcPixel22Spirv), sizeof(kDgcPixel33Spirv) };
		rhi::PipelinePtr pipelines[3];
		for (uint32_t i = 0; i < 3; ++i) {
			rhi::SubobjLayout subobjLayout{ layout->GetHandle() };
			rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { kDgcVertexSpirv, static_cast<uint32_t>(sizeof(kDgcVertexSpirv)) }, "main" };
			rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { pixelShaders[i], pixelSizes[i] }, "main" };
			rhi::SubobjRaster raster{};
			raster.rs.cull = rhi::CullMode::None;
			rhi::SubobjDepth depth{};
			depth.ds.depthEnable = false;
			depth.ds.depthWrite = false;
			rhi::SubobjRTVs targets{};
			targets.rt.count = 1;
			targets.rt.formats[0] = rhi::Format::R32_UInt;
			rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			rhi::SubobjFlags flags{ rhi::PipelineFlags_IndirectBindable };
			const rhi::PipelineStreamItem items[] = {
				rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(depth),
				rhi::Make(targets), rhi::Make(topology), rhi::Make(flags),
			};
			if (Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), pipelines[i]), "CreatePipeline indirect-bindable") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// Index 0 at creation, 1 and 2 filled in afterwards (the on-demand compilation pattern).
		rhi::IndirectPipelineSetPtr pipelineSet;
		if (Check(device.CreateIndirectPipelineSet({ pipelines[0]->GetHandle(), 4 }, pipelineSet), "CreateIndirectPipelineSet") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		const rhi::PipelineHandle laterPipelines[] = { pipelines[1]->GetHandle(), pipelines[2]->GetHandle() };
		if (Check(device.UpdateIndirectPipelineSet(pipelineSet->GetHandle(), 1, { laterPipelines, 2 }), "UpdateIndirectPipelineSet") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::IndirectArg args[4]{};
		args[0].kind = rhi::IndirectArgKind::PipelineIndex;
		args[1].kind = rhi::IndirectArgKind::Constant;
		args[1].u.rootConstants = { 0, 0, 1 };
		args[2].kind = rhi::IndirectArgKind::IndexBuffer;
		args[3].kind = rhi::IndirectArgKind::DrawIndexed;
		struct Sequence {
			uint32_t pipelineIndex;
			uint32_t drawId;
			uint64_t indexAddress; // D3D12_INDEX_BUFFER_VIEW
			uint32_t indexSize;
			uint32_t indexFormat;  // DXGI_FORMAT
			uint32_t indexCount;
			uint32_t instanceCount;
			uint32_t firstIndex;
			int32_t vertexOffset;
			uint32_t firstInstance;
			uint32_t pad;
		};
		static_assert(sizeof(Sequence) == 48);
		rhi::CommandSignaturePtr signature;
		rhi::CommandSignatureDesc signatureDesc{};
		signatureDesc.args = { args, 4 };
		signatureDesc.byteStride = sizeof(Sequence);
		signatureDesc.pipelineSet = pipelineSet->GetHandle();
		if (Check(device.CreateCommandSignature(signatureDesc, layout->GetHandle(), signature), "CreateCommandSignature pipeline set") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		auto makeUpload = [&](uint64_t size, const char* name, rhi::ResourcePtr& out) {
			return Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(size, rhi::HeapType::Upload, {}, name), out), name);
		};
		rhi::ResourcePtr indexBuffer, argumentBuffer, countBuffer, readback;
		if (makeUpload(16, "VulkanSmokeDgcIndices", indexBuffer) != rhi::Result::Ok ||
			makeUpload(sizeof(Sequence) * kCommands + 16 /* the offset read below */, "VulkanSmokeDgcArguments", argumentBuffer) != rhi::Result::Ok ||
			makeUpload(sizeof(uint32_t), "VulkanSmokeDgcCount", countBuffer) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		void* mapped = nullptr;
		indexBuffer->Map(&mapped, 0, 12);
		const uint16_t indices[6] = { 0, 1, 2, 2, 1, 3 };
		std::memcpy(mapped, indices, sizeof(indices));
		indexBuffer->Unmap(0, sizeof(indices));

		const uint64_t indexAddress = device.GetBufferDeviceAddress({ indexBuffer->GetHandle(), 0 });
		if (indexAddress == 0) {
			std::fprintf(stderr, "Index buffer device address was zero\n");
			return rhi::Result::InvalidArgument;
		}
		argumentBuffer->Map(&mapped, 0, sizeof(Sequence) * kCommands);
		auto* sequences = static_cast<Sequence*>(mapped);
		for (uint32_t i = 0; i < kCommands; ++i) {
			sequences[i] = Sequence{ i % 3, i, indexAddress, 12, 57 /* DXGI_FORMAT_R16_UINT */, 6, 1, 0, 0, 0, 0 };
		}
		argumentBuffer->Unmap(0, sizeof(Sequence) * kCommands);
		countBuffer->Map(&mapped, 0, sizeof(uint32_t));
		*static_cast<uint32_t*>(mapped) = kExecutedCommands;
		countBuffer->Unmap(0, sizeof(uint32_t));

		rhi::ClearValue clear{};
		clear.format = rhi::Format::R32_UInt;
		clear.rgba[0] = clear.rgba[1] = clear.rgba[2] = clear.rgba[3] = 0.0f;
		rhi::ResourcePtr target;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(rhi::Format::R32_UInt, rhi::HeapType::DeviceLocal, kWidth, kHeight, 1, 1, 1,
				rhi::ResourceLayout::Undefined, &clear, rhi::RF_AllowRenderTarget, "VulkanSmokeDgcTarget"), target), "CreateCommittedResource DGC target") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		rhi::DescriptorHeapPtr rtvHeap;
		rhi::DescriptorHeapDesc rtvHeapDesc{};
		rtvHeapDesc.type = rhi::DescriptorHeapType::RTV;
		rtvHeapDesc.capacity = 1;
		rtvHeapDesc.shaderVisible = false;
		rtvHeapDesc.debugName = "VulkanSmokeDgcRTVHeap";
		if (Check(device.CreateDescriptorHeap(rtvHeapDesc, rtvHeap), "CreateDescriptorHeap DGC RTV") != rhi::Result::Ok ||
			Check(device.CreateRenderTargetView({ rtvHeap->GetHandle(), 0 }, target->GetHandle(), {}), "CreateRenderTargetView DGC") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::CopyableFootprint footprint{};
		const rhi::CopyableFootprintsInfo footprintInfo = device.GetCopyableFootprints(rhi::FootprintRangeDesc{ .texture = target->GetHandle() }, &footprint, 1);
		if (footprintInfo.count != 1 || Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(footprintInfo.totalBytes, rhi::HeapType::Readback, {}, "VulkanSmokeDgcReadback"), readback),
				"CreateCommittedResource DGC readback") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// Records one ExecuteIndirect into the cleared target, reads the target back and compares every
		// texel with expected(column).
		auto runAndCheck = [&](const char* what, rhi::CommandSignatureHandle runSignature, rhi::ResourceHandle arguments, uint64_t argumentOffset, rhi::ResourceHandle count,
							   rhi::PipelineHandle boundPipeline, rhi::PipelineLayoutHandle runLayout, rhi::DescriptorHeapHandle runHeap, auto beforePass,
							   auto expectedForColumn) -> rhi::Result {
			commandList.Recycle(allocator);
			commandList.SetDescriptorHeaps(runHeap, samplerHeap.GetHandle());
			commandList.BindLayout(runLayout);
			beforePass();
			rhi::TextureBarrier toTarget{
				.texture = target->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::RenderTarget,
				.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::RenderTarget,
				.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::RenderTarget, .discard = true,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toTarget, 1 } });
			rhi::ColorAttachment color{};
			color.rtv = { rtvHeap->GetHandle(), 0 };
			color.loadOp = rhi::LoadOp::Clear;
			color.storeOp = rhi::StoreOp::Store;
			color.resource = target->GetHandle();
			color.clear = clear;
			commandList.BeginPass(rhi::PassBeginInfo{ .colors = { &color, 1 }, .depth = nullptr, .width = kWidth, .height = kHeight, .debugName = "VulkanSmokeDgc" });
			if (boundPipeline.valid()) {
				commandList.BindPipeline(boundPipeline);
			}
			commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commandList.ExecuteIndirect(runSignature, arguments, argumentOffset, count, 0, kCommands);
			commandList.EndPass();
			rhi::TextureBarrier toCopy{
				.texture = target->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::RenderTarget, .afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::RenderTarget, .afterAccess = rhi::ResourceAccessType::CopySource,
				.beforeLayout = rhi::ResourceLayout::RenderTarget, .afterLayout = rhi::ResourceLayout::CopySource, .discard = false,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toCopy, 1 } });
			commandList.CopyTextureToBuffer(rhi::BufferTextureCopyFootprint{ .texture = target->GetHandle(), .buffer = readback->GetHandle(), .mip = 0, .arraySlice = 0, .footprint = footprint });
			commandList.End();
			const rhi::CommandList submitLists[] = { commandList };
			if (Check(graphicsQueue.Submit(submitLists, {}), "Queue::Submit DGC") != rhi::Result::Ok || Check(device.WaitIdle(), "Device::WaitIdle DGC") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}

			void* readMapped = nullptr;
			readback->Map(&readMapped, 0, footprintInfo.totalBytes);
			const auto* bytes = static_cast<const uint8_t*>(readMapped);
			uint32_t wrong = 0;
			for (uint32_t y = 0; y < kHeight; ++y) {
				const auto* row = reinterpret_cast<const uint32_t*>(bytes + footprint.offset + static_cast<uint64_t>(y) * footprint.rowPitch);
				for (uint32_t x = 0; x < kWidth; ++x) {
					const uint32_t expected = expectedForColumn(x / (kWidth / kCommands));
					if (row[x] != expected && wrong++ < 4) {
						std::fprintf(stderr, "%s: texel (%u, %u) = %u, expected %u\n", what, x, y, row[x], expected);
					}
				}
			}
			readback->Unmap(0, 0);
			if (wrong != 0) {
				std::fprintf(stderr, "%s: %u of %u texels wrong\n", what, wrong, kWidth * kHeight);
				return rhi::Result::InvalidArgument;
			}
			std::printf("BasicRHIVulkanSmoke: %s OK\n", what);
			return rhi::Result::Ok;
		};

		// 1. Pipeline set: each command picks its pixel shader; the count buffer leaves the last column empty.
		if (runAndCheck("indirect pipeline set", signature->GetHandle(), argumentBuffer->GetHandle(), 0, countBuffer->GetHandle(), {},
				layout->GetHandle(), shaderVisibleHeap.GetHandle(), [] {}, [&](uint32_t column) { return column < kExecutedCommands ? kColumnValues[column % 3] : 0u; }) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// 2. No set: root constant, index buffer and draw with the bound pipeline (the same argument
		// layout minus the pipeline index), every command, no count buffer.
		rhi::CommandSignaturePtr plainSignature;
		rhi::CommandSignatureDesc plainDesc{};
		plainDesc.args = { args + 1, 3 };
		plainDesc.byteStride = sizeof(Sequence);
		if (Check(device.CreateCommandSignature(plainDesc, layout->GetHandle(), plainSignature), "CreateCommandSignature without set") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		// Same buffer, read from byte 4: each command starts at its drawId, so the index buffer view
		// stays 8-byte aligned; the next command's pipeline index falls in the unread tail.
		if (runAndCheck("indirect draws with the bound pipeline", plainSignature->GetHandle(), argumentBuffer->GetHandle(), 4, rhi::ResourceHandle{}, pipelines[1]->GetHandle(),
				layout->GetHandle(), shaderVisibleHeap.GetHandle(), [] {}, [&](uint32_t) { return kColumnValues[1]; }) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// 2b. Push spill: a 96 B per-draw range, written per command by the Constant argument, then a 128 B per-pass range set
		// once with PushConstants (DCLF's split). 224 B is over the descriptor-buffer model's 128 B push budget, so there the
		// per-draw range is pushed and the per-pass one spilled to memory. Texel = draw value + two per-pass words. A Constant
		// argument aimed into the per-pass range must be refused there (past constantArgumentBytes) and accepted on heaps.
		{
			// dxc -T ps_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
			//   [[vk::binding(0, 0)]] cbuffer DrawData { uint drawId; uint drawValue; };  // 96 B per-draw range (pushed): Constant argument
			//   [[vk::binding(1, 0)]] cbuffer PassData { uint4 passWords[8]; };           // 128 B per-pass range (spilled): PushConstants
			//
			//   uint main() : SV_Target {
			//       return drawValue + passWords[drawId >> 2][drawId & 3] + passWords[7].w;
			//   }
			static constexpr uint32_t kDgcSpillPixelSpirv[] = {
				0x07230203,0x00010600,0x000e0000,0x00000025,0x00000000,0x00020011,
				0x00000001,0x0003000e,0x00000000,0x00000001,0x0008000f,0x00000004,
				0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,
				0x00030010,0x00000001,0x00000007,0x00030003,0x00000005,0x00000294,
				0x00060005,0x00000005,0x65707974,0x6172442e,0x74614477,0x00000061,
				0x00050006,0x00000005,0x00000000,0x77617264,0x00006449,0x00060006,
				0x00000005,0x00000001,0x77617264,0x756c6156,0x00000065,0x00050005,
				0x00000003,0x77617244,0x61746144,0x00000000,0x00060005,0x00000006,
				0x65707974,0x7361502e,0x74614473,0x00000061,0x00060006,0x00000006,
				0x00000000,0x73736170,0x64726f57,0x00000073,0x00050005,0x00000004,
				0x73736150,0x61746144,0x00000000,0x00070005,0x00000002,0x2e74756f,
				0x2e726176,0x545f5653,0x65677261,0x00000074,0x00040005,0x00000001,
				0x6e69616d,0x00000000,0x00040047,0x00000002,0x0000001e,0x00000000,
				0x00040047,0x00000003,0x00000022,0x00000000,0x00040047,0x00000003,
				0x00000021,0x00000000,0x00040047,0x00000004,0x00000022,0x00000000,
				0x00040047,0x00000004,0x00000021,0x00000001,0x00050048,0x00000005,
				0x00000000,0x00000023,0x00000000,0x00050048,0x00000005,0x00000001,
				0x00000023,0x00000004,0x00030047,0x00000005,0x00000002,0x00040047,
				0x00000007,0x00000006,0x00000010,0x00050048,0x00000006,0x00000000,
				0x00000023,0x00000000,0x00030047,0x00000006,0x00000002,0x00040015,
				0x00000008,0x00000020,0x00000001,0x0004002b,0x00000008,0x00000009,
				0x00000001,0x0004002b,0x00000008,0x0000000a,0x00000000,0x00040015,
				0x0000000b,0x00000020,0x00000000,0x0004002b,0x0000000b,0x0000000c,
				0x00000002,0x0004002b,0x0000000b,0x0000000d,0x00000003,0x0004002b,
				0x00000008,0x0000000e,0x00000007,0x0004002b,0x00000008,0x0000000f,
				0x00000003,0x0004001e,0x00000005,0x0000000b,0x0000000b,0x00040020,
				0x00000010,0x00000002,0x00000005,0x0004002b,0x0000000b,0x00000011,
				0x00000008,0x00040017,0x00000012,0x0000000b,0x00000004,0x0004001c,
				0x00000007,0x00000012,0x00000011,0x0003001e,0x00000006,0x00000007,
				0x00040020,0x00000013,0x00000002,0x00000006,0x00040020,0x00000014,
				0x00000003,0x0000000b,0x00020013,0x00000015,0x00030021,0x00000016,
				0x00000015,0x00040020,0x00000017,0x00000002,0x0000000b,0x0004003b,
				0x00000010,0x00000003,0x00000002,0x0004003b,0x00000013,0x00000004,
				0x00000002,0x0004003b,0x00000014,0x00000002,0x00000003,0x00050036,
				0x00000015,0x00000001,0x00000000,0x00000016,0x000200f8,0x00000018,
				0x00050041,0x00000017,0x00000019,0x00000003,0x00000009,0x0004003d,
				0x0000000b,0x0000001a,0x00000019,0x00050041,0x00000017,0x0000001b,
				0x00000003,0x0000000a,0x0004003d,0x0000000b,0x0000001c,0x0000001b,
				0x000500c2,0x0000000b,0x0000001d,0x0000001c,0x0000000c,0x000500c7,
				0x0000000b,0x0000001e,0x0000001c,0x0000000d,0x00070041,0x00000017,
				0x0000001f,0x00000004,0x0000000a,0x0000001d,0x0000001e,0x0004003d,
				0x0000000b,0x00000020,0x0000001f,0x00050080,0x0000000b,0x00000021,
				0x0000001a,0x00000020,0x00070041,0x00000017,0x00000022,0x00000004,
				0x0000000a,0x0000000e,0x0000000f,0x0004003d,0x0000000b,0x00000023,
				0x00000022,0x00050080,0x0000000b,0x00000024,0x00000021,0x00000023,
				0x0003003e,0x00000002,0x00000024,0x000100fd,0x00010038,
			};
			constexpr uint32_t kDrawWords = 24;  // 96 B
			constexpr uint32_t kPassWords = 32;  // 128 B
			const uint32_t model = g_bindingModel;
			std::printf("BasicRHIVulkanSmoke [%s]: constantArgumentBytes = %u\n", BindingModelName(model), indirect.constantArgumentBytes);
			if (model == rhi::vulkan::BindingModel_DescriptorBuffer &&
				(indirect.constantArgumentBytes < kDrawWords * 4u || indirect.constantArgumentBytes > 128u)) {
				std::fprintf(stderr, "[%s] DGC push spill: constantArgumentBytes = %u, expected %u..128 (the pushed prefix of the 128 B budget)\n",
					BindingModelName(model), indirect.constantArgumentBytes, kDrawWords * 4u);
				return rhi::Result::InvalidArgument;
			}

			rhi::PushConstantRangeDesc spillRanges[2]{};
			spillRanges[0].visibility = rhi::ShaderStage::AllGraphics;
			spillRanges[0].num32BitValues = kDrawWords;
			spillRanges[0].binding = 0;
			spillRanges[1].visibility = rhi::ShaderStage::AllGraphics;
			spillRanges[1].num32BitValues = kPassWords;
			spillRanges[1].binding = 1;
			rhi::PipelineLayoutPtr spillLayout;
			if (Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { spillRanges, 2 }, .staticSamplers = {}, .flags = rhi::PF_None },
					spillLayout), "CreatePipelineLayout DGC push spill") != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] DGC push spill: a 224 B root-constant layout was refused\n", BindingModelName(model));
				return rhi::Result::InvalidArgument;
			}
			rhi::PipelinePtr spillPipeline;
			{
				rhi::SubobjLayout subobjLayout{ spillLayout->GetHandle() };
				rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { kDgcVertexSpirv, static_cast<uint32_t>(sizeof(kDgcVertexSpirv)) }, "main" };
				rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { kDgcSpillPixelSpirv, static_cast<uint32_t>(sizeof(kDgcSpillPixelSpirv)) }, "main" };
				rhi::SubobjRaster raster{};
				raster.rs.cull = rhi::CullMode::None;
				rhi::SubobjDepth depth{};
				depth.ds.depthEnable = false;
				depth.ds.depthWrite = false;
				rhi::SubobjRTVs targets{};
				targets.rt.count = 1;
				targets.rt.formats[0] = rhi::Format::R32_UInt;
				rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(depth), rhi::Make(targets), rhi::Make(topology),
				};
				if (Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), spillPipeline), "CreatePipeline DGC push spill") != rhi::Result::Ok) {
					return rhi::Result::InvalidArgument;
				}
			}

			rhi::IndirectArg spillArgs[3]{};
			spillArgs[0].kind = rhi::IndirectArgKind::Constant;
			spillArgs[0].u.rootConstants = { 0, 0, 2 };  // drawId, drawValue: the per-draw (pushed) range only
			spillArgs[1].kind = rhi::IndirectArgKind::IndexBuffer;
			spillArgs[2].kind = rhi::IndirectArgKind::DrawIndexed;
			struct SpillSequence {
				uint32_t drawId;
				uint32_t drawValue;
				uint64_t indexAddress; // D3D12_INDEX_BUFFER_VIEW
				uint32_t indexSize;
				uint32_t indexFormat;
				uint32_t indexCount;
				uint32_t instanceCount;
				uint32_t firstIndex;
				int32_t vertexOffset;
				uint32_t firstInstance;
				uint32_t pad;
			};
			static_assert(sizeof(SpillSequence) == 48);
			rhi::CommandSignaturePtr spillSignature;
			rhi::CommandSignatureDesc spillDesc{};
			spillDesc.args = { spillArgs, 3 };
			spillDesc.byteStride = sizeof(SpillSequence);
			if (Check(device.CreateCommandSignature(spillDesc, spillLayout->GetHandle(), spillSignature), "CreateCommandSignature DGC push spill") != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] DGC push spill: a Constant argument into the first (pushed) range was refused\n", BindingModelName(model));
				return rhi::Result::InvalidArgument;
			}

			// The same stream writing the per-pass range's last word: past the pushed prefix.
			rhi::IndirectArg intoPassArgs[3] = { spillArgs[0], spillArgs[1], spillArgs[2] };
			intoPassArgs[0].u.rootConstants = { 1, kPassWords - 1, 1 };
			rhi::CommandSignatureDesc intoPassDesc = spillDesc;
			intoPassDesc.args = { intoPassArgs, 3 };
			rhi::CommandSignaturePtr intoPassSignature;
			const rhi::Result intoPassResult = device.CreateCommandSignature(intoPassDesc, spillLayout->GetHandle(), intoPassSignature);
			const bool expectRefusal = model == rhi::vulkan::BindingModel_DescriptorBuffer;
			if ((intoPassResult == rhi::Result::Ok) == expectRefusal) {
				std::fprintf(stderr, "[%s] DGC push spill: a Constant argument at byte %u of a 224 B layout (constantArgumentBytes %u) returned result %u, expected %s\n",
					BindingModelName(model), (kDrawWords + kPassWords - 1) * 4u, indirect.constantArgumentBytes, static_cast<unsigned>(intoPassResult),
					expectRefusal ? "a refusal" : "Ok");
				return rhi::Result::InvalidArgument;
			}
			std::printf("BasicRHIVulkanSmoke [%s]: Constant argument into the per-pass range %s as expected\n", BindingModelName(model),
				expectRefusal ? "refused" : "accepted");

			rhi::ResourcePtr spillArguments;
			if (makeUpload(sizeof(SpillSequence) * kCommands, "VulkanSmokeDgcSpillArguments", spillArguments) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			spillArguments->Map(&mapped, 0, sizeof(SpillSequence) * kCommands);
			auto* spillSequences = static_cast<SpillSequence*>(mapped);
			for (uint32_t i = 0; i < kCommands; ++i) {
				spillSequences[i] = SpillSequence{ i, 100u * (i + 1u), indexAddress, 12, 57, 6, 1, 0, 0, 0, 0 };
			}
			spillArguments->Unmap(0, sizeof(SpillSequence) * kCommands);
			uint32_t passWords[kPassWords];
			for (uint32_t i = 0; i < kPassWords; ++i) {
				passWords[i] = 1000u + 10u * i;
			}
			if (runAndCheck("indirect draws with spilled per-pass constants", spillSignature->GetHandle(), spillArguments->GetHandle(), 0, rhi::ResourceHandle{},
					spillPipeline->GetHandle(), spillLayout->GetHandle(), shaderVisibleHeap.GetHandle(),
					[&] { commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0, 1, 0, kPassWords, passWords); },
					[&](uint32_t column) { return 100u * (column + 1u) + passWords[column] + passWords[kPassWords - 1]; }) != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] DGC push spill: per-draw words come from the Constant argument, per-pass words from PushConstants\n", BindingModelName(model));
				return rhi::Result::InvalidArgument;
			}
		}

		// 3. Indirect bindings: each command pushes the address of its record, from which the pixel shader's
		// constant buffer (b1, IndirectAddress) and texture (t0, IndirectIndex) are read, and binds its
		// own vertex buffer (VertexBuffer argument, dynamic stride). The vertex shader's b1 is a vertex-only
		// range reading another record entry (per-stage mappings). Texel = texture value + pixel b1 + vertex b1.
		if (!indirect.indirectBindings || !indirect.vertexBufferArguments) {
			std::puts("BasicRHIVulkanSmoke: indirect binding validation skipped (not supported by this device)");
			return rhi::Result::Ok;
		}
		static constexpr uint32_t kDgcBoundVertexSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000001d,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000000,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,
			0x00000005,0x00030003,0x00000005,0x00000294,0x00070005,0x00000006,
			0x65707974,0x7265562e,0x56786574,0x65756c61,0x00000000,0x00060006,
			0x00000006,0x00000000,0x74726576,0x61567865,0x0065756c,0x00050005,
			0x00000005,0x74726556,0x61567865,0x0065756c,0x00060005,0x00000002,
			0x762e6e69,0x502e7261,0x5449534f,0x004e4f49,0x00060005,0x00000004,
			0x2e74756f,0x2e726176,0x554c4156,0x00000045,0x00040005,0x00000001,
			0x6e69616d,0x00000000,0x00040047,0x00000003,0x0000000b,0x00000000,
			0x00030047,0x00000004,0x0000000e,0x00040047,0x00000002,0x0000001e,
			0x00000000,0x00040047,0x00000004,0x0000001e,0x00000000,0x00040047,
			0x00000005,0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,
			0x00000001,0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,
			0x00030047,0x00000006,0x00000002,0x00040015,0x00000007,0x00000020,
			0x00000001,0x0004002b,0x00000007,0x00000008,0x00000000,0x00030016,
			0x00000009,0x00000020,0x0004002b,0x00000009,0x0000000a,0x00000000,
			0x0004002b,0x00000009,0x0000000b,0x3f800000,0x00040015,0x0000000c,
			0x00000020,0x00000000,0x0003001e,0x00000006,0x0000000c,0x00040020,
			0x0000000d,0x00000002,0x00000006,0x00040017,0x0000000e,0x00000009,
			0x00000002,0x00040020,0x0000000f,0x00000001,0x0000000e,0x00040017,
			0x00000010,0x00000009,0x00000004,0x00040020,0x00000011,0x00000003,
			0x00000010,0x00040020,0x00000012,0x00000003,0x0000000c,0x00020013,
			0x00000013,0x00030021,0x00000014,0x00000013,0x00040020,0x00000015,
			0x00000002,0x0000000c,0x0004003b,0x0000000d,0x00000005,0x00000002,
			0x0004003b,0x0000000f,0x00000002,0x00000001,0x0004003b,0x00000011,
			0x00000003,0x00000003,0x0004003b,0x00000012,0x00000004,0x00000003,
			0x00050036,0x00000013,0x00000001,0x00000000,0x00000014,0x000200f8,
			0x00000016,0x0004003d,0x0000000e,0x00000017,0x00000002,0x00050051,
			0x00000009,0x00000018,0x00000017,0x00000000,0x00050051,0x00000009,
			0x00000019,0x00000017,0x00000001,0x00070050,0x00000010,0x0000001a,
			0x00000018,0x00000019,0x0000000a,0x0000000b,0x00050041,0x00000015,
			0x0000001b,0x00000005,0x00000008,0x0004003d,0x0000000c,0x0000001c,
			0x0000001b,0x0003003e,0x00000003,0x0000001a,0x0003003e,0x00000004,
			0x0000001c,0x000100fd,0x00010038,
		};
		static constexpr uint32_t kDgcBoundPixelSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000001e,0x00000000,0x00020011,
			0x00000001,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,
			0x00000001,0x6e69616d,0x00000000,0x00000002,0x00000003,0x00000004,
			0x00000005,0x00030010,0x00000001,0x00000007,0x00030003,0x00000005,
			0x00000294,0x00060005,0x00000006,0x65707974,0x6172442e,0x6c615677,
			0x00006575,0x00050006,0x00000006,0x00000000,0x756c6176,0x00000065,
			0x00050005,0x00000004,0x77617244,0x756c6156,0x00000065,0x00060005,
			0x00000007,0x65707974,0x2e64322e,0x67616d69,0x00000065,0x00050005,
			0x00000005,0x65736142,0x74786554,0x00657275,0x00060005,0x00000002,
			0x762e6e69,0x562e7261,0x45554c41,0x00000000,0x00070005,0x00000003,
			0x2e74756f,0x2e726176,0x545f5653,0x65677261,0x00000074,0x00040005,
			0x00000001,0x6e69616d,0x00000000,0x00030047,0x00000002,0x0000000e,
			0x00040047,0x00000002,0x0000001e,0x00000000,0x00040047,0x00000003,
			0x0000001e,0x00000000,0x00040047,0x00000004,0x00000022,0x00000000,
			0x00040047,0x00000004,0x00000021,0x00000001,0x00040047,0x00000005,
			0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,0x00000064,
			0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,0x00030047,
			0x00000006,0x00000002,0x00040015,0x00000008,0x00000020,0x00000001,
			0x0004002b,0x00000008,0x00000009,0x00000000,0x00040015,0x0000000a,
			0x00000020,0x00000000,0x0003001e,0x00000006,0x0000000a,0x00040020,
			0x0000000b,0x00000002,0x00000006,0x00090019,0x00000007,0x0000000a,
			0x00000001,0x00000002,0x00000000,0x00000000,0x00000001,0x00000000,
			0x00040020,0x0000000c,0x00000000,0x00000007,0x00040020,0x0000000d,
			0x00000001,0x0000000a,0x00040020,0x0000000e,0x00000003,0x0000000a,
			0x00020013,0x0000000f,0x00030021,0x00000010,0x0000000f,0x00040017,
			0x00000011,0x00000008,0x00000002,0x00040017,0x00000012,0x0000000a,
			0x00000004,0x00040020,0x00000013,0x00000002,0x0000000a,0x0004003b,
			0x0000000b,0x00000004,0x00000002,0x0004003b,0x0000000c,0x00000005,
			0x00000000,0x0004003b,0x0000000d,0x00000002,0x00000001,0x0004003b,
			0x0000000e,0x00000003,0x00000003,0x0005002c,0x00000011,0x00000014,
			0x00000009,0x00000009,0x00050036,0x0000000f,0x00000001,0x00000000,
			0x00000010,0x000200f8,0x00000015,0x0004003d,0x0000000a,0x00000016,
			0x00000002,0x0004003d,0x00000007,0x00000017,0x00000005,0x0007005f,
			0x00000012,0x00000018,0x00000017,0x00000014,0x00000002,0x00000009,
			0x00050051,0x0000000a,0x00000019,0x00000018,0x00000000,0x00050041,
			0x00000013,0x0000001a,0x00000004,0x00000009,0x0004003d,0x0000000a,
			0x0000001b,0x0000001a,0x00050080,0x0000000a,0x0000001c,0x00000019,
			0x0000001b,0x00050080,0x0000000a,0x0000001d,0x0000001c,0x00000016,
			0x0003003e,0x00000003,0x0000001d,0x000100fd,0x00010038,
		};
		constexpr uint32_t kTextureValue = 1000u;
		constexpr uint32_t kTextureRegisterBinding = 100;  // -fvk-t-shift 100
		rhi::PushConstantRangeDesc recordAddress{};
		recordAddress.visibility = rhi::ShaderStage::AllGraphics;
		recordAddress.num32BitValues = 2;
		recordAddress.set = 0;
		recordAddress.binding = 60;  // no shader reads the push data directly
		rhi::LayoutBindingRange boundRanges[3]{};
		boundRanges[0].set = 0;
		boundRanges[0].binding = 1;
		boundRanges[0].visibility = rhi::ShaderStage::Pixel;
		boundRanges[0].source = rhi::LayoutRangeSource::IndirectAddress;
		boundRanges[0].recordOffset = 0;
		boundRanges[1].set = 0;
		boundRanges[1].binding = kTextureRegisterBinding;
		boundRanges[1].visibility = rhi::ShaderStage::Pixel;
		boundRanges[1].source = rhi::LayoutRangeSource::IndirectIndex;
		boundRanges[1].recordOffset = 8;
		boundRanges[2].set = 0;
		boundRanges[2].binding = 1;
		boundRanges[2].visibility = rhi::ShaderStage::Vertex;
		boundRanges[2].source = rhi::LayoutRangeSource::IndirectAddress;
		boundRanges[2].recordOffset = 16;
		rhi::PipelineLayoutPtr boundLayout;
		if (Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = { boundRanges, 3 }, .pushConstants = { &recordAddress, 1 }, .staticSamplers = {},
				.flags = rhi::PF_AllowInputAssembler }, boundLayout), "CreatePipelineLayout indirect bindings") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::PipelinePtr boundPipeline;
		{
			rhi::SubobjLayout subobjLayout{ boundLayout->GetHandle() };
			rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { kDgcBoundVertexSpirv, static_cast<uint32_t>(sizeof(kDgcBoundVertexSpirv)) }, "main" };
			rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { kDgcBoundPixelSpirv, static_cast<uint32_t>(sizeof(kDgcBoundPixelSpirv)) }, "main" };
			rhi::SubobjRaster raster{};
			raster.rs.cull = rhi::CullMode::None;
			rhi::SubobjDepth depth{};
			depth.ds.depthEnable = false;
			depth.ds.depthWrite = false;
			rhi::SubobjRTVs targets{};
			targets.rt.count = 1;
			targets.rt.formats[0] = rhi::Format::R32_UInt;
			rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			rhi::SubobjInputLayout input{};
			input.il.bindings.push_back(rhi::InputBindingDesc{ 0, 8, rhi::InputRate::PerVertex, 1 });
			input.il.attributes.push_back(rhi::InputAttributeDesc{ 0, 0, rhi::Format::R32G32_Float, "POSITION", 0, 0 });
			rhi::SubobjFlags flags{ rhi::PipelineFlags_IndirectBindable };
			const rhi::PipelineStreamItem items[] = {
				rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(depth),
				rhi::Make(targets), rhi::Make(topology), rhi::Make(input), rhi::Make(flags),
			};
			if (Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), boundPipeline), "CreatePipeline indirect bindings") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// One 1x1 texture holding kTextureValue, in slot 0 of a heap of our own.
		rhi::ResourcePtr texture, textureUpload;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(rhi::Format::R32_UInt, rhi::HeapType::DeviceLocal, 1, 1, 1, 1, 1,
				rhi::ResourceLayout::Undefined, nullptr, {}, "VulkanSmokeDgcTexture"), texture), "CreateCommittedResource DGC texture") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		rhi::CopyableFootprint textureFootprint{};
		const rhi::CopyableFootprintsInfo textureFootprintInfo = device.GetCopyableFootprints(rhi::FootprintRangeDesc{ .texture = texture->GetHandle() }, &textureFootprint, 1);
		if (makeUpload(textureFootprintInfo.totalBytes, "VulkanSmokeDgcTextureUpload", textureUpload) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		textureUpload->Map(&mapped, 0, textureFootprintInfo.totalBytes);
		std::memcpy(static_cast<uint8_t*>(mapped) + textureFootprint.offset, &kTextureValue, sizeof(kTextureValue));
		textureUpload->Unmap(0, textureFootprintInfo.totalBytes);
		rhi::DescriptorHeapPtr boundHeap;
		rhi::DescriptorHeapDesc boundHeapDesc{};
		boundHeapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
		boundHeapDesc.capacity = 4;
		boundHeapDesc.shaderVisible = true;
		boundHeapDesc.debugName = "VulkanSmokeDgcHeap";
		rhi::SrvDesc textureSrv{};
		textureSrv.dimension = rhi::SrvDim::Texture2D;
		textureSrv.formatOverride = rhi::Format::R32_UInt;
		textureSrv.tex2D.mipLevels = 1;
		constexpr uint32_t kTextureHeapIndex = 2;
		if (Check(device.CreateDescriptorHeap(boundHeapDesc, boundHeap), "CreateDescriptorHeap DGC") != rhi::Result::Ok ||
			Check(device.CreateShaderResourceView({ boundHeap->GetHandle(), kTextureHeapIndex }, texture->GetHandle(), textureSrv), "CreateShaderResourceView DGC") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// Per command: a pixel constant block (value 100 + i) and a vertex one (value 10000 + i), a record
		// { pixel cb address, texture index, vertex cb address }, and four vertices of its column.
		struct BoundSequence {
			uint64_t recordAddress;
			uint64_t vertexAddress; // D3D12_VERTEX_BUFFER_VIEW
			uint32_t vertexSize;
			uint32_t vertexStride;
			uint64_t indexAddress;
			uint32_t indexSize;
			uint32_t indexFormat;
			uint32_t indexCount;
			uint32_t instanceCount;
			uint32_t firstIndex;
			int32_t vertexOffset;
			uint32_t firstInstance;
			uint32_t pad;
		};
		static_assert(sizeof(BoundSequence) == 64);
		constexpr uint64_t kBlockStride = 256;
		rhi::ResourcePtr blocks, records, vertices, boundArguments;
		constexpr uint64_t kRecordStride = 32;
		if (makeUpload(kBlockStride * kCommands * 2, "VulkanSmokeDgcBlocks", blocks) != rhi::Result::Ok ||
			makeUpload(kRecordStride * kCommands, "VulkanSmokeDgcRecords", records) != rhi::Result::Ok ||
			makeUpload(32 * kCommands, "VulkanSmokeDgcVertices", vertices) != rhi::Result::Ok ||
			makeUpload(sizeof(BoundSequence) * kCommands, "VulkanSmokeDgcBoundArguments", boundArguments) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		const uint64_t blocksAddress = device.GetBufferDeviceAddress({ blocks->GetHandle(), 0 });
		const uint64_t recordsAddress = device.GetBufferDeviceAddress({ records->GetHandle(), 0 });
		const uint64_t verticesAddress = device.GetBufferDeviceAddress({ vertices->GetHandle(), 0 });
		blocks->Map(&mapped, 0, kBlockStride * kCommands * 2);
		for (uint32_t i = 0; i < kCommands; ++i) {
			*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + kBlockStride * i) = 100u + i;
			*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped) + kBlockStride * (kCommands + i)) = 10000u + i;
		}
		blocks->Unmap(0, kBlockStride * kCommands * 2);
		records->Map(&mapped, 0, kRecordStride * kCommands);
		for (uint32_t i = 0; i < kCommands; ++i) {
			auto* record = static_cast<uint8_t*>(mapped) + kRecordStride * i;
			const uint64_t pixelBlockAddress = blocksAddress + kBlockStride * i;
			const uint64_t vertexBlockAddress = blocksAddress + kBlockStride * (kCommands + i);
			std::memcpy(record, &pixelBlockAddress, 8);
			std::memcpy(record + 8, &kTextureHeapIndex, 4);
			std::memcpy(record + 16, &vertexBlockAddress, 8);
		}
		records->Unmap(0, kRecordStride * kCommands);
		vertices->Map(&mapped, 0, 32 * kCommands);
		for (uint32_t i = 0; i < kCommands; ++i) {
			auto* corner = reinterpret_cast<float*>(static_cast<uint8_t*>(mapped) + 32 * i);
			const float left = -1.0f + 0.25f * static_cast<float>(i);
			const float quad[8] = { left, -1.0f, left + 0.25f, -1.0f, left, 1.0f, left + 0.25f, 1.0f };
			std::memcpy(corner, quad, sizeof(quad));
		}
		vertices->Unmap(0, 32 * kCommands);
		boundArguments->Map(&mapped, 0, sizeof(BoundSequence) * kCommands);
		auto* boundSequences = static_cast<BoundSequence*>(mapped);
		for (uint32_t i = 0; i < kCommands; ++i) {
			boundSequences[i] = BoundSequence{ recordsAddress + kRecordStride * i, verticesAddress + 32 * i, 32, 8, indexAddress, 12, 57, 6, 1, 0, 0, 0, 0 };
		}
		boundArguments->Unmap(0, sizeof(BoundSequence) * kCommands);

		rhi::IndirectArg boundArgs[4]{};
		boundArgs[0].kind = rhi::IndirectArgKind::Constant;
		boundArgs[0].u.rootConstants = { 0, 0, 2 };
		boundArgs[1].kind = rhi::IndirectArgKind::VertexBuffer;
		boundArgs[1].u.vertexBuffer.slot = 0;
		boundArgs[2].kind = rhi::IndirectArgKind::IndexBuffer;
		boundArgs[3].kind = rhi::IndirectArgKind::DrawIndexed;
		rhi::CommandSignaturePtr boundSignature;
		rhi::CommandSignatureDesc boundDesc{};
		boundDesc.args = { boundArgs, 4 };
		boundDesc.byteStride = sizeof(BoundSequence);
		if (Check(device.CreateCommandSignature(boundDesc, boundLayout->GetHandle(), boundSignature), "CreateCommandSignature indirect bindings") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// The texture's upload happens in the same command list as the draws.
		auto uploadTexture = [&]() {
			rhi::TextureBarrier toCopyDest{
				.texture = texture->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::CopyDest,
				.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::CopyDest, .discard = true,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toCopyDest, 1 } });
			commandList.CopyBufferToTexture(rhi::BufferTextureCopyFootprint{ .texture = texture->GetHandle(), .buffer = textureUpload->GetHandle(), .mip = 0, .arraySlice = 0, .footprint = textureFootprint });
			rhi::TextureBarrier toRead{
				.texture = texture->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::Copy, .afterSync = rhi::ResourceSyncState::PixelShading,
				.beforeAccess = rhi::ResourceAccessType::CopyDest, .afterAccess = rhi::ResourceAccessType::ShaderResource,
				.beforeLayout = rhi::ResourceLayout::CopyDest, .afterLayout = rhi::ResourceLayout::ShaderResource, .discard = false,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toRead, 1 } });
		};
		if (runAndCheck("indirect bindings and vertex buffers", boundSignature->GetHandle(), boundArguments->GetHandle(), 0, rhi::ResourceHandle{}, boundPipeline->GetHandle(),
				boundLayout->GetHandle(), boundHeap->GetHandle(), uploadTexture, [&](uint32_t column) { return kTextureValue + 100u + column + 10000u + column; }) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		return rhi::Result::Ok;
	}

	rhi::Result ValidatePipelineLayoutPushData(
		rhi::Device& device,
		rhi::CommandList& commandList,
		rhi::PipelineLayoutPtr& pipelineLayout,
		rhi::PipelinePtr& pipeline) {
		static constexpr uint32_t kEmptyComputeSpirv[] = {
			0x07230203,0x00010000,0x000d000a,0x0000000a,
			0x00000000,0x00020011,0x00000001,0x0006000b,
			0x00000001,0x4c534c47,0x6474732e,0x3035342e,
			0x00000000,0x0003000e,0x00000000,0x00000001,
			0x0005000f,0x00000005,0x00000004,0x6e69616d,
			0x00000000,0x00060010,0x00000004,0x00000011,
			0x00000001,0x00000001,0x00000001,0x00030003,
			0x00000002,0x000001b8,0x000a0004,0x475f4c47,
			0x4c474f4f,0x70635f45,0x74735f70,0x5f656c79,
			0x656e696c,0x7269645f,0x69746365,0x00006576,
			0x00080004,0x475f4c47,0x4c474f4f,0x6e695f45,
			0x64756c63,0x69645f65,0x74636572,0x00657669,
			0x00040005,0x00000004,0x6e69616d,0x00000000,
			0x00040047,0x00000009,0x0000000b,0x00000019,
			0x00020013,0x00000002,0x00030021,0x00000003,
			0x00000002,0x00040015,0x00000006,0x00000020,
			0x00000000,0x00040017,0x00000007,0x00000006,
			0x00000003,0x0004002b,0x00000006,0x00000008,
			0x00000001,0x0006002c,0x00000007,0x00000009,
			0x00000008,0x00000008,0x00000008,0x00050036,
			0x00000002,0x00000004,0x00000000,0x00000003,
			0x000200f8,0x00000005,0x000100fd,0x00010038,
		};

		rhi::PushConstantRangeDesc pushConstantRange{};
		pushConstantRange.visibility = rhi::ShaderStage::Compute;
		pushConstantRange.num32BitValues = 4;
		pushConstantRange.set = 0;
		pushConstantRange.binding = 7;

		if (Check(device.CreatePipelineLayout(
			rhi::PipelineLayoutDesc{
				.ranges = {},
				.pushConstants = { &pushConstantRange, 1 },
				.staticSamplers = {},
				.flags = rhi::PipelineLayoutFlags::PF_None,
			},
			pipelineLayout), "CreatePipelineLayout") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::SubobjLayout subobjLayout{ pipelineLayout->GetHandle() };
		rhi::SubobjShader subobjCompute{
			rhi::ShaderStage::Compute,
			{ kEmptyComputeSpirv, static_cast<uint32_t>(sizeof(kEmptyComputeSpirv)) },
			"main"
		};
		const rhi::PipelineStreamItem items[] = {
			rhi::Make(subobjLayout),
			rhi::Make(subobjCompute),
		};
		std::atomic_uint32_t concurrentFailures{ 0 };
		std::vector<std::thread> pipelineThreads;
		for (uint32_t threadIndex = 0; threadIndex < 8; ++threadIndex) {
			pipelineThreads.emplace_back([&] {
				for (uint32_t iteration = 0; iteration < 8; ++iteration) {
					rhi::PipelinePtr concurrentPipeline;
					if (device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), concurrentPipeline) != rhi::Result::Ok ||
						!concurrentPipeline) {
						concurrentFailures.fetch_add(1, std::memory_order_relaxed);
					}
				}
			});
		}
		for (auto& thread : pipelineThreads) thread.join();
		if (concurrentFailures.load(std::memory_order_relaxed) != 0) {
			std::fprintf(stderr, "Concurrent Vulkan pipeline creation failed %u times\n",
				concurrentFailures.load(std::memory_order_relaxed));
			return rhi::Result::InvalidArgument;
		}

		if (Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), pipeline), "CreatePipeline compute") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		const uint32_t pushValues[4] = { 11u, 22u, 33u, 44u };
		commandList.BindLayout(pipelineLayout->GetHandle());
		commandList.BindPipeline(pipeline->GetHandle());
		commandList.PushConstants(rhi::ShaderStage::Compute, 0, 7, 0, 4, pushValues);
		commandList.Dispatch(1, 1, 1);
		return rhi::Result::Ok;
	}

	// The output side of the binding-model compute cases: a raw buffer whose UAV a case puts in slot 0 of its resource heap
	// (WriteOutputView). Run resets it to kSentinel, records the case, reads it back and compares every word: the expected
	// ones, and the sentinel everywhere else, so that a missing or stray write shows.
	struct ComputeCheck {
		static constexpr uint32_t kOutputWords = 128;
		static constexpr uint64_t kOutputBytes = kOutputWords * sizeof(uint32_t);
		static constexpr uint32_t kSentinel = 0xCDCDCDCDu;

		rhi::Device& device;
		rhi::Queue& queue;
		rhi::CommandAllocator& allocator;
		rhi::CommandList& commandList;
		rhi::ResourcePtr output, sentinel, readback;

		rhi::Result Init() {
			if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kOutputBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "VulkanSmokeCaseOutput"),
					output), "CreateCommittedResource case output") != rhi::Result::Ok ||
				Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kOutputBytes, rhi::HeapType::Upload, {}, "VulkanSmokeCaseSentinel"),
					sentinel), "CreateCommittedResource case sentinel") != rhi::Result::Ok ||
				Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kOutputBytes, rhi::HeapType::Readback, {}, "VulkanSmokeCaseReadback"),
					readback), "CreateCommittedResource case readback") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			void* mapped = nullptr;
			sentinel->Map(&mapped, 0, kOutputBytes);
			if (!mapped) {
				std::fprintf(stderr, "Case sentinel map failed\n");
				return rhi::Result::InvalidArgument;
			}
			for (uint32_t i = 0; i < kOutputWords; ++i) {
				static_cast<uint32_t*>(mapped)[i] = kSentinel;
			}
			sentinel->Unmap(0, kOutputBytes);
			return rhi::Result::Ok;
		}

		// A raw UAV of output words [firstWord, firstWord + wordCount) in slot 0 of `heap`.
		rhi::Result WriteOutputView(rhi::DescriptorHeapHandle heap, uint32_t firstWord, uint32_t wordCount) {
			rhi::UavDesc uav{};
			uav.dimension = rhi::UavDim::Buffer;
			uav.buffer.kind = rhi::BufferViewKind::Raw;
			uav.buffer.firstElement = firstWord;
			uav.buffer.numElements = wordCount;
			return Check(device.CreateUnorderedAccessView({ heap, 0 }, output->GetHandle(), uav), "CreateUnorderedAccessView case output");
		}

		rhi::Result CreatePipeline(const char* what, rhi::PipelineLayoutHandle layout, const uint32_t* spirv, uint32_t bytes, rhi::PipelinePtr& out) {
			rhi::SubobjLayout subobjLayout{ layout };
			rhi::SubobjShader compute{ rhi::ShaderStage::Compute, { spirv, bytes }, "main" };
			const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(compute) };
			const rhi::Result result = device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), out);
			if (result != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: CreatePipeline failed with result %u\n", BindingModelName(g_bindingModel), what, static_cast<unsigned>(result));
				return rhi::Result::InvalidArgument;
			}
			return rhi::Result::Ok;
		}

		// Binds the heaps, layout and pipeline, then `record(commandList)` pushes its data and dispatches. Words [0,
		// expected.size()) must read back as expected, the rest as the sentinel.
		template<typename Record>
		rhi::Result Run(const char* what, rhi::DescriptorHeapHandle heap, rhi::DescriptorHeapHandle samplerHeap, rhi::PipelineLayoutHandle layout,
			rhi::PipelineHandle pipeline, Record&& record, const std::vector<uint32_t>& expected) {
			const char* model = BindingModelName(g_bindingModel);
			commandList.Recycle(allocator);
			rhi::BufferBarrier toCopyDest{
				.buffer = output->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::CopyDest,
			};
			commandList.Barriers(rhi::BarrierBatch{ .buffers = { &toCopyDest, 1 } });
			commandList.CopyBufferRegion(output->GetHandle(), 0, sentinel->GetHandle(), 0, kOutputBytes);
			rhi::BufferBarrier toWrite{
				.buffer = output->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::Copy, .afterSync = rhi::ResourceSyncState::ComputeShading,
				.beforeAccess = rhi::ResourceAccessType::CopyDest, .afterAccess = rhi::ResourceAccessType::UnorderedAccess,
			};
			commandList.Barriers(rhi::BarrierBatch{ .buffers = { &toWrite, 1 } });
			commandList.SetDescriptorHeaps(heap, samplerHeap);
			commandList.BindLayout(layout);
			commandList.BindPipeline(pipeline);
			record(commandList);
			rhi::BufferBarrier toCopySource{
				.buffer = output->GetHandle(),
				.beforeSync = rhi::ResourceSyncState::ComputeShading, .afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::UnorderedAccess, .afterAccess = rhi::ResourceAccessType::CopySource,
			};
			commandList.Barriers(rhi::BarrierBatch{ .buffers = { &toCopySource, 1 } });
			commandList.CopyBufferRegion(readback->GetHandle(), 0, output->GetHandle(), 0, kOutputBytes);
			if (commandList.SupportsCheckedEnd()) {
				const rhi::Result endResult = commandList.EndChecked();
				if (endResult != rhi::Result::Ok) {
					std::fprintf(stderr, "[%s] %s: recording failed (EndChecked result %u)\n", model, what, static_cast<unsigned>(endResult));
					return rhi::Result::InvalidArgument;
				}
			}
			else {
				commandList.End();
			}
			const rhi::CommandList submitLists[] = { commandList };
			if (Check(queue.Submit(submitLists, {}), "Queue::Submit case") != rhi::Result::Ok || Check(device.WaitIdle(), "Device::WaitIdle case") != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: submission failed\n", model, what);
				return rhi::Result::InvalidArgument;
			}

			void* mapped = nullptr;
			readback->Map(&mapped, 0, kOutputBytes);
			if (!mapped) {
				std::fprintf(stderr, "[%s] %s: readback map failed\n", model, what);
				return rhi::Result::InvalidArgument;
			}
			const auto* words = static_cast<const uint32_t*>(mapped);
			uint32_t wrong = 0;
			for (uint32_t i = 0; i < kOutputWords; ++i) {
				const uint32_t want = i < expected.size() ? expected[i] : kSentinel;
				if (words[i] != want && wrong++ < 8) {
					std::fprintf(stderr, "[%s] %s: word %u = 0x%08x (%u), expected 0x%08x (%u)%s\n", model, what, i, words[i], words[i], want, want,
						words[i] == kSentinel ? " - never written" : "");
				}
			}
			readback->Unmap(0, 0);
			if (wrong != 0) {
				std::fprintf(stderr, "[%s] %s: %u of %u words wrong\n", model, what, wrong, kOutputWords);
				return rhi::Result::InvalidArgument;
			}
			std::printf("BasicRHIVulkanSmoke [%s]: %s OK\n", model, what);
			return rhi::Result::Ok;
		}
	};

	// Bindings whose path differs between the binding models, each read by a compute shader that writes what it read:
	// static samplers, the sampler and resource heaps (a sampler, a CBV, null views), emulated and multiple root-constant
	// ranges, push data over the 128 B budget (spilled ranges holding indirect addresses), and secondaries binding heaps.
	rhi::Result ValidateBindingModelCoverage(rhi::Device& device, rhi::Queue& queue, rhi::CommandAllocator& allocator, rhi::CommandList& commandList) {
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   SamplerState LinearClamp : register(s0);  // the layout's static sampler (linear, clamp)
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       Texture2D<float4> ramp = ResourceDescriptorHeap[1];
		//       output.Store(0, uint(ramp.SampleLevel(LinearClamp, float2(0.5, 0.5), 0).r * 1000.0 + 0.5));
		//       output.Store(4, uint(ramp.SampleLevel(LinearClamp, float2(-0.25, 0.5), 0).r * 1000.0 + 0.5));
		//       output.Store(8, uint(ramp.SampleLevel(LinearClamp, float2(1.25, 0.5), 0).r * 1000.0 + 0.5));
		//   }
		static constexpr uint32_t kStaticSamplerSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000003d,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0008000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00060010,0x00000001,
			0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,
			0x00000294,0x00060005,0x00000005,0x65707974,0x6d61732e,0x72656c70,
			0x00000000,0x00050005,0x00000002,0x656e694c,0x6c437261,0x00706d61,
			0x00090005,0x00000006,0x65707974,0x4257522e,0x41657479,0x65726464,
			0x75427373,0x72656666,0x00000000,0x00080005,0x00000003,0x6f736552,
			0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,0x00060005,
			0x00000007,0x65707974,0x2e64322e,0x67616d69,0x00000065,0x00080005,
			0x00000004,0x6f736552,0x65637275,0x63736544,0x74706972,0x6548726f,
			0x00007061,0x00040005,0x00000001,0x6e69616d,0x00000000,0x00070005,
			0x00000008,0x65707974,0x6d61732e,0x64656c70,0x616d692e,0x00006567,
			0x00040047,0x00000002,0x00000022,0x00000000,0x00040047,0x00000002,
			0x00000021,0x00000000,0x00040047,0x00000003,0x00000022,0x00000000,
			0x00040047,0x00000003,0x00000021,0x000f4240,0x00040047,0x00000004,
			0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,0x000f4240,
			0x00040047,0x00000009,0x00000006,0x00000004,0x00050048,0x00000006,
			0x00000000,0x00000023,0x00000000,0x00030047,0x00000006,0x00000002,
			0x00040015,0x0000000a,0x00000020,0x00000000,0x0004002b,0x0000000a,
			0x0000000b,0x00000000,0x0004002b,0x0000000a,0x0000000c,0x00000001,
			0x0004002b,0x0000000a,0x0000000d,0x00000002,0x00030016,0x0000000e,
			0x00000020,0x0004002b,0x0000000e,0x0000000f,0x3f000000,0x00040017,
			0x00000010,0x0000000e,0x00000002,0x0005002c,0x00000010,0x00000011,
			0x0000000f,0x0000000f,0x0004002b,0x0000000e,0x00000012,0x00000000,
			0x0004002b,0x0000000e,0x00000013,0x447a0000,0x0004002b,0x0000000e,
			0x00000014,0xbe800000,0x0005002c,0x00000010,0x00000015,0x00000014,
			0x0000000f,0x0004002b,0x0000000e,0x00000016,0x3fa00000,0x0005002c,
			0x00000010,0x00000017,0x00000016,0x0000000f,0x0002001a,0x00000005,
			0x00040020,0x00000018,0x00000000,0x00000005,0x0003001d,0x00000009,
			0x0000000a,0x0003001e,0x00000006,0x00000009,0x0003001d,0x00000019,
			0x00000006,0x00040020,0x0000001a,0x0000000c,0x00000019,0x00090019,
			0x00000007,0x0000000e,0x00000001,0x00000002,0x00000000,0x00000000,
			0x00000001,0x00000000,0x0003001d,0x0000001b,0x00000007,0x00040020,
			0x0000001c,0x00000000,0x0000001b,0x00020013,0x0000001d,0x00030021,
			0x0000001e,0x0000001d,0x00040020,0x0000001f,0x00000000,0x00000007,
			0x0003001b,0x00000008,0x00000007,0x00040017,0x00000020,0x0000000e,
			0x00000004,0x00040020,0x00000021,0x0000000c,0x0000000a,0x0004003b,
			0x00000018,0x00000002,0x00000000,0x0004003b,0x0000001a,0x00000003,
			0x0000000c,0x0004003b,0x0000001c,0x00000004,0x00000000,0x00050036,
			0x0000001d,0x00000001,0x00000000,0x0000001e,0x000200f8,0x00000022,
			0x00050041,0x0000001f,0x00000023,0x00000004,0x0000000c,0x0004003d,
			0x00000007,0x00000024,0x00000023,0x0004003d,0x00000005,0x00000025,
			0x00000002,0x00050056,0x00000008,0x00000026,0x00000024,0x00000025,
			0x00070058,0x00000020,0x00000027,0x00000026,0x00000011,0x00000002,
			0x00000012,0x00050051,0x0000000e,0x00000028,0x00000027,0x00000000,
			0x00050085,0x0000000e,0x00000029,0x00000028,0x00000013,0x00050081,
			0x0000000e,0x0000002a,0x00000029,0x0000000f,0x0004006d,0x0000000a,
			0x0000002b,0x0000002a,0x00070041,0x00000021,0x0000002c,0x00000003,
			0x0000000b,0x0000000b,0x0000000b,0x0003003e,0x0000002c,0x0000002b,
			0x0004003d,0x00000005,0x0000002d,0x00000002,0x00050056,0x00000008,
			0x0000002e,0x00000024,0x0000002d,0x00070058,0x00000020,0x0000002f,
			0x0000002e,0x00000015,0x00000002,0x00000012,0x00050051,0x0000000e,
			0x00000030,0x0000002f,0x00000000,0x00050085,0x0000000e,0x00000031,
			0x00000030,0x00000013,0x00050081,0x0000000e,0x00000032,0x00000031,
			0x0000000f,0x0004006d,0x0000000a,0x00000033,0x00000032,0x00070041,
			0x00000021,0x00000034,0x00000003,0x0000000b,0x0000000b,0x0000000c,
			0x0003003e,0x00000034,0x00000033,0x0004003d,0x00000005,0x00000035,
			0x00000002,0x00050056,0x00000008,0x00000036,0x00000024,0x00000035,
			0x00070058,0x00000020,0x00000037,0x00000036,0x00000017,0x00000002,
			0x00000012,0x00050051,0x0000000e,0x00000038,0x00000037,0x00000000,
			0x00050085,0x0000000e,0x00000039,0x00000038,0x00000013,0x00050081,
			0x0000000e,0x0000003a,0x00000039,0x0000000f,0x0004006d,0x0000000a,
			0x0000003b,0x0000003a,0x00070041,0x00000021,0x0000003c,0x00000003,
			0x0000000b,0x0000000b,0x0000000d,0x0003003e,0x0000003c,0x0000003b,
			0x000100fd,0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       Texture2D<float4> ramp = ResourceDescriptorHeap[1];
		//       SamplerState pointWrap = SamplerDescriptorHeap[1];
		//       output.Store(0, uint(ramp.SampleLevel(pointWrap, float2(0.45, 0.5), 0).r * 1000.0 + 0.5));
		//       output.Store(4, uint(ramp.SampleLevel(pointWrap, float2(-0.25, 0.5), 0).r * 1000.0 + 0.5));
		//       output.Store(8, uint(ramp.SampleLevel(pointWrap, float2(0.75, 0.5), 0).r * 1000.0 + 0.5));
		//   }
		static constexpr uint32_t kSamplerHeapSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000003f,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0008000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00060010,0x00000001,
			0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,
			0x00000294,0x00090005,0x00000005,0x65707974,0x4257522e,0x41657479,
			0x65726464,0x75427373,0x72656666,0x00000000,0x00080005,0x00000002,
			0x6f736552,0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,
			0x00060005,0x00000006,0x65707974,0x2e64322e,0x67616d69,0x00000065,
			0x00080005,0x00000003,0x6f736552,0x65637275,0x63736544,0x74706972,
			0x6548726f,0x00007061,0x00060005,0x00000007,0x65707974,0x6d61732e,
			0x72656c70,0x00000000,0x00080005,0x00000004,0x706d6153,0x4472656c,
			0x72637365,0x6f747069,0x61654872,0x00000070,0x00040005,0x00000001,
			0x6e69616d,0x00000000,0x00070005,0x00000008,0x65707974,0x6d61732e,
			0x64656c70,0x616d692e,0x00006567,0x00040047,0x00000002,0x00000022,
			0x00000000,0x00040047,0x00000002,0x00000021,0x000f4240,0x00040047,
			0x00000003,0x00000022,0x00000000,0x00040047,0x00000003,0x00000021,
			0x000f4240,0x00040047,0x00000004,0x00000022,0x00000000,0x00040047,
			0x00000004,0x00000021,0x000f4241,0x00040047,0x00000009,0x00000006,
			0x00000004,0x00050048,0x00000005,0x00000000,0x00000023,0x00000000,
			0x00030047,0x00000005,0x00000002,0x00040015,0x0000000a,0x00000020,
			0x00000000,0x0004002b,0x0000000a,0x0000000b,0x00000000,0x0004002b,
			0x0000000a,0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000d,
			0x00000002,0x00030016,0x0000000e,0x00000020,0x0004002b,0x0000000e,
			0x0000000f,0x3ee66666,0x0004002b,0x0000000e,0x00000010,0x3f000000,
			0x00040017,0x00000011,0x0000000e,0x00000002,0x0005002c,0x00000011,
			0x00000012,0x0000000f,0x00000010,0x0004002b,0x0000000e,0x00000013,
			0x00000000,0x0004002b,0x0000000e,0x00000014,0x447a0000,0x0004002b,
			0x0000000e,0x00000015,0xbe800000,0x0005002c,0x00000011,0x00000016,
			0x00000015,0x00000010,0x0004002b,0x0000000e,0x00000017,0x3f400000,
			0x0005002c,0x00000011,0x00000018,0x00000017,0x00000010,0x0003001d,
			0x00000009,0x0000000a,0x0003001e,0x00000005,0x00000009,0x0003001d,
			0x00000019,0x00000005,0x00040020,0x0000001a,0x0000000c,0x00000019,
			0x00090019,0x00000006,0x0000000e,0x00000001,0x00000002,0x00000000,
			0x00000000,0x00000001,0x00000000,0x0003001d,0x0000001b,0x00000006,
			0x00040020,0x0000001c,0x00000000,0x0000001b,0x0002001a,0x00000007,
			0x0003001d,0x0000001d,0x00000007,0x00040020,0x0000001e,0x00000000,
			0x0000001d,0x00020013,0x0000001f,0x00030021,0x00000020,0x0000001f,
			0x00040020,0x00000021,0x00000000,0x00000006,0x00040020,0x00000022,
			0x00000000,0x00000007,0x0003001b,0x00000008,0x00000006,0x00040017,
			0x00000023,0x0000000e,0x00000004,0x00040020,0x00000024,0x0000000c,
			0x0000000a,0x0004003b,0x0000001a,0x00000002,0x0000000c,0x0004003b,
			0x0000001c,0x00000003,0x00000000,0x0004003b,0x0000001e,0x00000004,
			0x00000000,0x00050036,0x0000001f,0x00000001,0x00000000,0x00000020,
			0x000200f8,0x00000025,0x00050041,0x00000021,0x00000026,0x00000003,
			0x0000000c,0x0004003d,0x00000006,0x00000027,0x00000026,0x00050041,
			0x00000022,0x00000028,0x00000004,0x0000000c,0x0004003d,0x00000007,
			0x00000029,0x00000028,0x00050056,0x00000008,0x0000002a,0x00000027,
			0x00000029,0x00070058,0x00000023,0x0000002b,0x0000002a,0x00000012,
			0x00000002,0x00000013,0x00050051,0x0000000e,0x0000002c,0x0000002b,
			0x00000000,0x00050085,0x0000000e,0x0000002d,0x0000002c,0x00000014,
			0x00050081,0x0000000e,0x0000002e,0x0000002d,0x00000010,0x0004006d,
			0x0000000a,0x0000002f,0x0000002e,0x00070041,0x00000024,0x00000030,
			0x00000002,0x0000000b,0x0000000b,0x0000000b,0x0003003e,0x00000030,
			0x0000002f,0x00050056,0x00000008,0x00000031,0x00000027,0x00000029,
			0x00070058,0x00000023,0x00000032,0x00000031,0x00000016,0x00000002,
			0x00000013,0x00050051,0x0000000e,0x00000033,0x00000032,0x00000000,
			0x00050085,0x0000000e,0x00000034,0x00000033,0x00000014,0x00050081,
			0x0000000e,0x00000035,0x00000034,0x00000010,0x0004006d,0x0000000a,
			0x00000036,0x00000035,0x00070041,0x00000024,0x00000037,0x00000002,
			0x0000000b,0x0000000b,0x0000000c,0x0003003e,0x00000037,0x00000036,
			0x00050056,0x00000008,0x00000038,0x00000027,0x00000029,0x00070058,
			0x00000023,0x00000039,0x00000038,0x00000018,0x00000002,0x00000013,
			0x00050051,0x0000000e,0x0000003a,0x00000039,0x00000000,0x00050085,
			0x0000000e,0x0000003b,0x0000003a,0x00000014,0x00050081,0x0000000e,
			0x0000003c,0x0000003b,0x00000010,0x0004006d,0x0000000a,0x0000003d,
			0x0000003c,0x00070041,0x00000024,0x0000003e,0x00000002,0x0000000b,
			0x0000000b,0x0000000d,0x0003003e,0x0000003e,0x0000003d,0x000100fd,
			0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   struct Values { uint4 first; uint4 second; };
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       ConstantBuffer<Values> values = ResourceDescriptorHeap[2];
		//       output.Store4(0, values.first);
		//       output.Store4(16, values.second);
		//   }
		static constexpr uint32_t kHeapCbvSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000030,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0007000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00060010,0x00000001,0x00000011,
			0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,0x00000294,
			0x00090005,0x00000004,0x65707974,0x4257522e,0x41657479,0x65726464,
			0x75427373,0x72656666,0x00000000,0x00080005,0x00000002,0x6f736552,
			0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,0x00090005,
			0x00000005,0x65707974,0x6e6f432e,0x6e617473,0x66754274,0x2e726566,
			0x756c6156,0x00007365,0x00050006,0x00000005,0x00000000,0x73726966,
			0x00000074,0x00050006,0x00000005,0x00000001,0x6f636573,0x0000646e,
			0x00080005,0x00000003,0x6f736552,0x65637275,0x63736544,0x74706972,
			0x6548726f,0x00007061,0x00040005,0x00000001,0x6e69616d,0x00000000,
			0x00040047,0x00000002,0x00000022,0x00000000,0x00040047,0x00000002,
			0x00000021,0x000f4240,0x00040047,0x00000003,0x00000022,0x00000000,
			0x00040047,0x00000003,0x00000021,0x000f4240,0x00040047,0x00000006,
			0x00000006,0x00000004,0x00050048,0x00000004,0x00000000,0x00000023,
			0x00000000,0x00030047,0x00000004,0x00000002,0x00050048,0x00000005,
			0x00000000,0x00000023,0x00000000,0x00050048,0x00000005,0x00000001,
			0x00000023,0x00000010,0x00030047,0x00000005,0x00000002,0x00040015,
			0x00000007,0x00000020,0x00000000,0x0004002b,0x00000007,0x00000008,
			0x00000000,0x0004002b,0x00000007,0x00000009,0x00000002,0x0004002b,
			0x00000007,0x0000000a,0x00000001,0x0004002b,0x00000007,0x0000000b,
			0x00000003,0x0003001d,0x00000006,0x00000007,0x0003001e,0x00000004,
			0x00000006,0x0003001d,0x0000000c,0x00000004,0x00040020,0x0000000d,
			0x0000000c,0x0000000c,0x00040017,0x0000000e,0x00000007,0x00000004,
			0x0004001e,0x00000005,0x0000000e,0x0000000e,0x0003001d,0x0000000f,
			0x00000005,0x00040020,0x00000010,0x00000002,0x0000000f,0x00020013,
			0x00000011,0x00030021,0x00000012,0x00000011,0x00040020,0x00000013,
			0x00000002,0x00000005,0x00040020,0x00000014,0x0000000c,0x00000007,
			0x0004003b,0x0000000d,0x00000002,0x0000000c,0x0004003b,0x00000010,
			0x00000003,0x00000002,0x0004002b,0x00000007,0x00000015,0x00000004,
			0x0004002b,0x00000007,0x00000016,0x00000005,0x0004002b,0x00000007,
			0x00000017,0x00000006,0x0004002b,0x00000007,0x00000018,0x00000007,
			0x00040020,0x00000019,0x00000002,0x0000000e,0x00050036,0x00000011,
			0x00000001,0x00000000,0x00000012,0x000200f8,0x0000001a,0x00050041,
			0x00000013,0x0000001b,0x00000003,0x00000009,0x00050041,0x00000019,
			0x0000001c,0x0000001b,0x00000008,0x0004003d,0x0000000e,0x0000001d,
			0x0000001c,0x00050041,0x00000019,0x0000001e,0x0000001b,0x0000000a,
			0x0004003d,0x0000000e,0x0000001f,0x0000001e,0x00050051,0x00000007,
			0x00000020,0x0000001d,0x00000000,0x00070041,0x00000014,0x00000021,
			0x00000002,0x00000008,0x00000008,0x00000008,0x0003003e,0x00000021,
			0x00000020,0x00050051,0x00000007,0x00000022,0x0000001d,0x00000001,
			0x00070041,0x00000014,0x00000023,0x00000002,0x00000008,0x00000008,
			0x0000000a,0x0003003e,0x00000023,0x00000022,0x00050051,0x00000007,
			0x00000024,0x0000001d,0x00000002,0x00070041,0x00000014,0x00000025,
			0x00000002,0x00000008,0x00000008,0x00000009,0x0003003e,0x00000025,
			0x00000024,0x00050051,0x00000007,0x00000026,0x0000001d,0x00000003,
			0x00070041,0x00000014,0x00000027,0x00000002,0x00000008,0x00000008,
			0x0000000b,0x0003003e,0x00000027,0x00000026,0x00050051,0x00000007,
			0x00000028,0x0000001f,0x00000000,0x00070041,0x00000014,0x00000029,
			0x00000002,0x00000008,0x00000008,0x00000015,0x0003003e,0x00000029,
			0x00000028,0x00050051,0x00000007,0x0000002a,0x0000001f,0x00000001,
			0x00070041,0x00000014,0x0000002b,0x00000002,0x00000008,0x00000008,
			0x00000016,0x0003003e,0x0000002b,0x0000002a,0x00050051,0x00000007,
			0x0000002c,0x0000001f,0x00000002,0x00070041,0x00000014,0x0000002d,
			0x00000002,0x00000008,0x00000008,0x00000017,0x0003003e,0x0000002d,
			0x0000002c,0x00050051,0x00000007,0x0000002e,0x0000001f,0x00000003,
			0x00070041,0x00000014,0x0000002f,0x00000002,0x00000008,0x00000008,
			0x00000018,0x0003003e,0x0000002f,0x0000002e,0x000100fd,0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       Texture2D<float4> nullTexture = ResourceDescriptorHeap[1];
		//       ByteAddressBuffer nullBuffer = ResourceDescriptorHeap[2];
		//       const float4 texel = nullTexture.Load(int3(0, 0, 0));
		//       output.Store3(0, uint3(asuint(texel.x), asuint(texel.y), nullBuffer.Load(0)));
		//   }
		static constexpr uint32_t kNullSrvSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000002a,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0008000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00060010,0x00000001,
			0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,
			0x00000294,0x00090005,0x00000005,0x65707974,0x4257522e,0x41657479,
			0x65726464,0x75427373,0x72656666,0x00000000,0x00080005,0x00000002,
			0x6f736552,0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,
			0x00060005,0x00000006,0x65707974,0x2e64322e,0x67616d69,0x00000065,
			0x00080005,0x00000003,0x6f736552,0x65637275,0x63736544,0x74706972,
			0x6548726f,0x00007061,0x00080005,0x00000007,0x65707974,0x7479422e,
			0x64644165,0x73736572,0x66667542,0x00007265,0x00080005,0x00000004,
			0x6f736552,0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,
			0x00040005,0x00000001,0x6e69616d,0x00000000,0x00040047,0x00000002,
			0x00000022,0x00000000,0x00040047,0x00000002,0x00000021,0x000f4240,
			0x00040047,0x00000003,0x00000022,0x00000000,0x00040047,0x00000003,
			0x00000021,0x000f4240,0x00040047,0x00000004,0x00000022,0x00000000,
			0x00040047,0x00000004,0x00000021,0x000f4240,0x00040047,0x00000008,
			0x00000006,0x00000004,0x00050048,0x00000005,0x00000000,0x00000023,
			0x00000000,0x00030047,0x00000005,0x00000002,0x00050048,0x00000007,
			0x00000000,0x00000023,0x00000000,0x00040048,0x00000007,0x00000000,
			0x00000018,0x00030047,0x00000007,0x00000002,0x00040015,0x00000009,
			0x00000020,0x00000000,0x0004002b,0x00000009,0x0000000a,0x00000000,
			0x0004002b,0x00000009,0x0000000b,0x00000001,0x0004002b,0x00000009,
			0x0000000c,0x00000002,0x00040015,0x0000000d,0x00000020,0x00000001,
			0x0004002b,0x0000000d,0x0000000e,0x00000000,0x0003001d,0x00000008,
			0x00000009,0x0003001e,0x00000005,0x00000008,0x0003001d,0x0000000f,
			0x00000005,0x00040020,0x00000010,0x0000000c,0x0000000f,0x00030016,
			0x00000011,0x00000020,0x00090019,0x00000006,0x00000011,0x00000001,
			0x00000002,0x00000000,0x00000000,0x00000001,0x00000000,0x0003001d,
			0x00000012,0x00000006,0x00040020,0x00000013,0x00000000,0x00000012,
			0x0003001e,0x00000007,0x00000008,0x0003001d,0x00000014,0x00000007,
			0x00040020,0x00000015,0x0000000c,0x00000014,0x00020013,0x00000016,
			0x00030021,0x00000017,0x00000016,0x00040017,0x00000018,0x00000011,
			0x00000004,0x00040020,0x00000019,0x00000000,0x00000006,0x00040017,
			0x0000001a,0x0000000d,0x00000002,0x00040020,0x0000001b,0x0000000c,
			0x00000009,0x0004003b,0x00000010,0x00000002,0x0000000c,0x0004003b,
			0x00000013,0x00000003,0x00000000,0x0004003b,0x00000015,0x00000004,
			0x0000000c,0x0005002c,0x0000001a,0x0000001c,0x0000000e,0x0000000e,
			0x00050036,0x00000016,0x00000001,0x00000000,0x00000017,0x000200f8,
			0x0000001d,0x00050041,0x00000019,0x0000001e,0x00000003,0x0000000b,
			0x0004003d,0x00000006,0x0000001f,0x0000001e,0x0007005f,0x00000018,
			0x00000020,0x0000001f,0x0000001c,0x00000002,0x0000000e,0x00050051,
			0x00000011,0x00000021,0x00000020,0x00000000,0x0004007c,0x00000009,
			0x00000022,0x00000021,0x00050051,0x00000011,0x00000023,0x00000020,
			0x00000001,0x0004007c,0x00000009,0x00000024,0x00000023,0x00070041,
			0x0000001b,0x00000025,0x00000004,0x0000000c,0x0000000a,0x0000000a,
			0x0004003d,0x00000009,0x00000026,0x00000025,0x00070041,0x0000001b,
			0x00000027,0x00000002,0x0000000a,0x0000000a,0x0000000a,0x0003003e,
			0x00000027,0x00000022,0x00070041,0x0000001b,0x00000028,0x00000002,
			0x0000000a,0x0000000a,0x0000000b,0x0003003e,0x00000028,0x00000024,
			0x00070041,0x0000001b,0x00000029,0x00000002,0x0000000a,0x0000000a,
			0x0000000c,0x0003003e,0x00000029,0x00000026,0x000100fd,0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   [[vk::binding(0, 0)]] cbuffer Emulated { uint4 emulatedFirst; uint4 emulatedSecond; };  // EmulatedRootConstants, 8 dwords
		//   [[vk::binding(1, 0)]] cbuffer DispatchData { uint outputBase; };                       // RootConstants32, 1 dword
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       output.Store4(outputBase, emulatedFirst);
		//       output.Store4(outputBase + 16, emulatedSecond);
		//   }
		static constexpr uint32_t kEmulatedRootConstantsSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000003c,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0008000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00060010,0x00000001,
			0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,
			0x00000294,0x00060005,0x00000005,0x65707974,0x756d452e,0x6574616c,
			0x00000064,0x00070006,0x00000005,0x00000000,0x6c756d65,0x64657461,
			0x73726946,0x00000074,0x00070006,0x00000005,0x00000001,0x6c756d65,
			0x64657461,0x6f636553,0x0000646e,0x00050005,0x00000002,0x6c756d45,
			0x64657461,0x00000000,0x00070005,0x00000006,0x65707974,0x7369442e,
			0x63746170,0x74614468,0x00000061,0x00060006,0x00000006,0x00000000,
			0x7074756f,0x61427475,0x00006573,0x00060005,0x00000003,0x70736944,
			0x68637461,0x61746144,0x00000000,0x00090005,0x00000007,0x65707974,
			0x4257522e,0x41657479,0x65726464,0x75427373,0x72656666,0x00000000,
			0x00080005,0x00000004,0x6f736552,0x65637275,0x63736544,0x74706972,
			0x6548726f,0x00007061,0x00040005,0x00000001,0x6e69616d,0x00000000,
			0x00040047,0x00000002,0x00000022,0x00000000,0x00040047,0x00000002,
			0x00000021,0x00000000,0x00040047,0x00000003,0x00000022,0x00000000,
			0x00040047,0x00000003,0x00000021,0x00000001,0x00040047,0x00000004,
			0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,0x000f4240,
			0x00050048,0x00000005,0x00000000,0x00000023,0x00000000,0x00050048,
			0x00000005,0x00000001,0x00000023,0x00000010,0x00030047,0x00000005,
			0x00000002,0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,
			0x00030047,0x00000006,0x00000002,0x00040047,0x00000008,0x00000006,
			0x00000004,0x00050048,0x00000007,0x00000000,0x00000023,0x00000000,
			0x00030047,0x00000007,0x00000002,0x00040015,0x00000009,0x00000020,
			0x00000000,0x0004002b,0x00000009,0x0000000a,0x00000000,0x00040015,
			0x0000000b,0x00000020,0x00000001,0x0004002b,0x0000000b,0x0000000c,
			0x00000000,0x0004002b,0x00000009,0x0000000d,0x00000002,0x0004002b,
			0x00000009,0x0000000e,0x00000001,0x0004002b,0x00000009,0x0000000f,
			0x00000003,0x0004002b,0x00000009,0x00000010,0x00000010,0x0004002b,
			0x0000000b,0x00000011,0x00000001,0x00040017,0x00000012,0x00000009,
			0x00000004,0x0004001e,0x00000005,0x00000012,0x00000012,0x00040020,
			0x00000013,0x00000002,0x00000005,0x0003001e,0x00000006,0x00000009,
			0x00040020,0x00000014,0x00000002,0x00000006,0x0003001d,0x00000008,
			0x00000009,0x0003001e,0x00000007,0x00000008,0x0003001d,0x00000015,
			0x00000007,0x00040020,0x00000016,0x0000000c,0x00000015,0x00020013,
			0x00000017,0x00030021,0x00000018,0x00000017,0x00040020,0x00000019,
			0x00000002,0x00000009,0x00040020,0x0000001a,0x00000002,0x00000012,
			0x00040020,0x0000001b,0x0000000c,0x00000009,0x0004003b,0x00000013,
			0x00000002,0x00000002,0x0004003b,0x00000014,0x00000003,0x00000002,
			0x0004003b,0x00000016,0x00000004,0x0000000c,0x00050036,0x00000017,
			0x00000001,0x00000000,0x00000018,0x000200f8,0x0000001c,0x00050041,
			0x00000019,0x0000001d,0x00000003,0x0000000c,0x0004003d,0x00000009,
			0x0000001e,0x0000001d,0x000500c2,0x00000009,0x0000001f,0x0000001e,
			0x0000000d,0x00050041,0x0000001a,0x00000020,0x00000002,0x0000000c,
			0x0004003d,0x00000012,0x00000021,0x00000020,0x00050051,0x00000009,
			0x00000022,0x00000021,0x00000000,0x00070041,0x0000001b,0x00000023,
			0x00000004,0x0000000a,0x0000000a,0x0000001f,0x0003003e,0x00000023,
			0x00000022,0x00050051,0x00000009,0x00000024,0x00000021,0x00000001,
			0x00050080,0x00000009,0x00000025,0x0000001f,0x0000000e,0x00070041,
			0x0000001b,0x00000026,0x00000004,0x0000000a,0x0000000a,0x00000025,
			0x0003003e,0x00000026,0x00000024,0x00050051,0x00000009,0x00000027,
			0x00000021,0x00000002,0x00050080,0x00000009,0x00000028,0x0000001f,
			0x0000000d,0x00070041,0x0000001b,0x00000029,0x00000004,0x0000000a,
			0x0000000a,0x00000028,0x0003003e,0x00000029,0x00000027,0x00050051,
			0x00000009,0x0000002a,0x00000021,0x00000003,0x00050080,0x00000009,
			0x0000002b,0x0000001f,0x0000000f,0x00070041,0x0000001b,0x0000002c,
			0x00000004,0x0000000a,0x0000000a,0x0000002b,0x0003003e,0x0000002c,
			0x0000002a,0x00050080,0x00000009,0x0000002d,0x0000001e,0x00000010,
			0x000500c2,0x00000009,0x0000002e,0x0000002d,0x0000000d,0x00050041,
			0x0000001a,0x0000002f,0x00000002,0x00000011,0x0004003d,0x00000012,
			0x00000030,0x0000002f,0x00050051,0x00000009,0x00000031,0x00000030,
			0x00000000,0x00070041,0x0000001b,0x00000032,0x00000004,0x0000000a,
			0x0000000a,0x0000002e,0x0003003e,0x00000032,0x00000031,0x00050051,
			0x00000009,0x00000033,0x00000030,0x00000001,0x00050080,0x00000009,
			0x00000034,0x0000002e,0x0000000e,0x00070041,0x0000001b,0x00000035,
			0x00000004,0x0000000a,0x0000000a,0x00000034,0x0003003e,0x00000035,
			0x00000033,0x00050051,0x00000009,0x00000036,0x00000030,0x00000002,
			0x00050080,0x00000009,0x00000037,0x0000002e,0x0000000d,0x00070041,
			0x0000001b,0x00000038,0x00000004,0x0000000a,0x0000000a,0x00000037,
			0x0003003e,0x00000038,0x00000036,0x00050051,0x00000009,0x00000039,
			0x00000030,0x00000003,0x00050080,0x00000009,0x0000003a,0x0000002e,
			0x0000000f,0x00070041,0x0000001b,0x0000003b,0x00000004,0x0000000a,
			0x0000000a,0x0000003a,0x0003003e,0x0000003b,0x00000039,0x000100fd,
			0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   [[vk::binding(0, 0)]] cbuffer RangeA { uint4 a; };
		//   [[vk::binding(1, 0)]] cbuffer RangeB { uint2 b; };
		//   [[vk::binding(2, 0)]] cbuffer RangeC { uint3 c; };
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       output.Store4(0, a);
		//       output.Store2(16, b);
		//       output.Store3(24, c);
		//   }
		static constexpr uint32_t kRootConstantRangesSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x0000003e,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0009000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00000005,0x00060010,
			0x00000001,0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,
			0x00000005,0x00000294,0x00050005,0x00000006,0x65707974,0x6e61522e,
			0x00416567,0x00040006,0x00000006,0x00000000,0x00000061,0x00040005,
			0x00000002,0x676e6152,0x00004165,0x00050005,0x00000007,0x65707974,
			0x6e61522e,0x00426567,0x00040006,0x00000007,0x00000000,0x00000062,
			0x00040005,0x00000003,0x676e6152,0x00004265,0x00050005,0x00000008,
			0x65707974,0x6e61522e,0x00436567,0x00040006,0x00000008,0x00000000,
			0x00000063,0x00040005,0x00000004,0x676e6152,0x00004365,0x00090005,
			0x00000009,0x65707974,0x4257522e,0x41657479,0x65726464,0x75427373,
			0x72656666,0x00000000,0x00080005,0x00000005,0x6f736552,0x65637275,
			0x63736544,0x74706972,0x6548726f,0x00007061,0x00040005,0x00000001,
			0x6e69616d,0x00000000,0x00040047,0x00000002,0x00000022,0x00000000,
			0x00040047,0x00000002,0x00000021,0x00000000,0x00040047,0x00000003,
			0x00000022,0x00000000,0x00040047,0x00000003,0x00000021,0x00000001,
			0x00040047,0x00000004,0x00000022,0x00000000,0x00040047,0x00000004,
			0x00000021,0x00000002,0x00040047,0x00000005,0x00000022,0x00000000,
			0x00040047,0x00000005,0x00000021,0x000f4240,0x00050048,0x00000006,
			0x00000000,0x00000023,0x00000000,0x00030047,0x00000006,0x00000002,
			0x00050048,0x00000007,0x00000000,0x00000023,0x00000000,0x00030047,
			0x00000007,0x00000002,0x00050048,0x00000008,0x00000000,0x00000023,
			0x00000000,0x00030047,0x00000008,0x00000002,0x00040047,0x0000000a,
			0x00000006,0x00000004,0x00050048,0x00000009,0x00000000,0x00000023,
			0x00000000,0x00030047,0x00000009,0x00000002,0x00040015,0x0000000b,
			0x00000020,0x00000000,0x0004002b,0x0000000b,0x0000000c,0x00000000,
			0x0004002b,0x0000000b,0x0000000d,0x00000002,0x00040015,0x0000000e,
			0x00000020,0x00000001,0x0004002b,0x0000000e,0x0000000f,0x00000000,
			0x0004002b,0x0000000b,0x00000010,0x00000001,0x0004002b,0x0000000b,
			0x00000011,0x00000003,0x00040017,0x00000012,0x0000000b,0x00000004,
			0x0003001e,0x00000006,0x00000012,0x00040020,0x00000013,0x00000002,
			0x00000006,0x00040017,0x00000014,0x0000000b,0x00000002,0x0003001e,
			0x00000007,0x00000014,0x00040020,0x00000015,0x00000002,0x00000007,
			0x00040017,0x00000016,0x0000000b,0x00000003,0x0003001e,0x00000008,
			0x00000016,0x00040020,0x00000017,0x00000002,0x00000008,0x0003001d,
			0x0000000a,0x0000000b,0x0003001e,0x00000009,0x0000000a,0x0003001d,
			0x00000018,0x00000009,0x00040020,0x00000019,0x0000000c,0x00000018,
			0x00020013,0x0000001a,0x00030021,0x0000001b,0x0000001a,0x00040020,
			0x0000001c,0x00000002,0x00000012,0x00040020,0x0000001d,0x0000000c,
			0x0000000b,0x00040020,0x0000001e,0x00000002,0x00000014,0x00040020,
			0x0000001f,0x00000002,0x00000016,0x0004003b,0x00000013,0x00000002,
			0x00000002,0x0004003b,0x00000015,0x00000003,0x00000002,0x0004003b,
			0x00000017,0x00000004,0x00000002,0x0004003b,0x00000019,0x00000005,
			0x0000000c,0x0004002b,0x0000000b,0x00000020,0x00000004,0x0004002b,
			0x0000000b,0x00000021,0x00000005,0x0004002b,0x0000000b,0x00000022,
			0x00000006,0x0004002b,0x0000000b,0x00000023,0x00000007,0x0004002b,
			0x0000000b,0x00000024,0x00000008,0x00050036,0x0000001a,0x00000001,
			0x00000000,0x0000001b,0x000200f8,0x00000025,0x00050041,0x0000001c,
			0x00000026,0x00000002,0x0000000f,0x0004003d,0x00000012,0x00000027,
			0x00000026,0x00050051,0x0000000b,0x00000028,0x00000027,0x00000000,
			0x00070041,0x0000001d,0x00000029,0x00000005,0x0000000c,0x0000000c,
			0x0000000c,0x0003003e,0x00000029,0x00000028,0x00050051,0x0000000b,
			0x0000002a,0x00000027,0x00000001,0x00070041,0x0000001d,0x0000002b,
			0x00000005,0x0000000c,0x0000000c,0x00000010,0x0003003e,0x0000002b,
			0x0000002a,0x00050051,0x0000000b,0x0000002c,0x00000027,0x00000002,
			0x00070041,0x0000001d,0x0000002d,0x00000005,0x0000000c,0x0000000c,
			0x0000000d,0x0003003e,0x0000002d,0x0000002c,0x00050051,0x0000000b,
			0x0000002e,0x00000027,0x00000003,0x00070041,0x0000001d,0x0000002f,
			0x00000005,0x0000000c,0x0000000c,0x00000011,0x0003003e,0x0000002f,
			0x0000002e,0x00050041,0x0000001e,0x00000030,0x00000003,0x0000000f,
			0x0004003d,0x00000014,0x00000031,0x00000030,0x00050051,0x0000000b,
			0x00000032,0x00000031,0x00000000,0x00070041,0x0000001d,0x00000033,
			0x00000005,0x0000000c,0x0000000c,0x00000020,0x0003003e,0x00000033,
			0x00000032,0x00050051,0x0000000b,0x00000034,0x00000031,0x00000001,
			0x00070041,0x0000001d,0x00000035,0x00000005,0x0000000c,0x0000000c,
			0x00000021,0x0003003e,0x00000035,0x00000034,0x00050041,0x0000001f,
			0x00000036,0x00000004,0x0000000f,0x0004003d,0x00000016,0x00000037,
			0x00000036,0x00050051,0x0000000b,0x00000038,0x00000037,0x00000000,
			0x00070041,0x0000001d,0x00000039,0x00000005,0x0000000c,0x0000000c,
			0x00000022,0x0003003e,0x00000039,0x00000038,0x00050051,0x0000000b,
			0x0000003a,0x00000037,0x00000001,0x00070041,0x0000001d,0x0000003b,
			0x00000005,0x0000000c,0x0000000c,0x00000023,0x0003003e,0x0000003b,
			0x0000003a,0x00050051,0x0000000b,0x0000003c,0x00000037,0x00000002,
			0x00070041,0x0000001d,0x0000003d,0x00000005,0x0000000c,0x0000000c,
			0x00000024,0x0003003e,0x0000003d,0x0000003c,0x000100fd,0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   // 64 B pushed, then 96 B that cannot be pushed (64 + 96 > 120), then 8 B more.
		//   [[vk::binding(0, 0)]] cbuffer Pushed { uint4 pushedWords[4]; };
		//   [[vk::binding(1, 0)]] cbuffer Spilled { uint4 spilledWords[5]; uint2 blockAddress; uint2 recordAddress; };
		//   [[vk::binding(2, 0)]] cbuffer Tail { uint outputBase; uint tailValue; };
		//   [[vk::binding(3, 0)]] cbuffer AddressedBlock { uint4 blockWords; };  // PushAddress: Spilled dwords 20-21
		//   [[vk::binding(4, 0)]] ByteAddressBuffer IndexedBuffer;                 // IndirectIndex: record address in Spilled dwords 22-23
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       uint at = outputBase;
		//       [unroll] for (uint i = 0; i < 4; ++i) { output.Store4(at, pushedWords[i]); at += 16; }
		//       [unroll] for (uint j = 0; j < 5; ++j) { output.Store4(at, spilledWords[j]); at += 16; }
		//       output.Store4(at, uint4(blockAddress, recordAddress)); at += 16;
		//       output.Store2(at, uint2(outputBase, tailValue)); at += 8;
		//       output.Store4(at, blockWords); at += 16;
		//       output.Store4(at, IndexedBuffer.Load4(0));
		//   }
		static constexpr uint32_t kPushSpillSpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x000000f8,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x000b000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00000005,0x00000006,
			0x00000007,0x00060010,0x00000001,0x00000011,0x00000001,0x00000001,
			0x00000001,0x00030003,0x00000005,0x00000294,0x00050005,0x00000008,
			0x65707974,0x7375502e,0x00646568,0x00060006,0x00000008,0x00000000,
			0x68737570,0x6f576465,0x00736472,0x00040005,0x00000002,0x68737550,
			0x00006465,0x00060005,0x00000009,0x65707974,0x6970532e,0x64656c6c,
			0x00000000,0x00070006,0x00000009,0x00000000,0x6c697073,0x5764656c,
			0x7364726f,0x00000000,0x00070006,0x00000009,0x00000001,0x636f6c62,
			0x6464416b,0x73736572,0x00000000,0x00070006,0x00000009,0x00000002,
			0x6f636572,0x64416472,0x73657264,0x00000073,0x00040005,0x00000003,
			0x6c697053,0x0064656c,0x00050005,0x0000000a,0x65707974,0x6961542e,
			0x0000006c,0x00060006,0x0000000a,0x00000000,0x7074756f,0x61427475,
			0x00006573,0x00060006,0x0000000a,0x00000001,0x6c696174,0x756c6156,
			0x00000065,0x00040005,0x00000004,0x6c696154,0x00000000,0x00070005,
			0x0000000b,0x65707974,0x6464412e,0x73736572,0x6c426465,0x006b636f,
			0x00060006,0x0000000b,0x00000000,0x636f6c62,0x726f576b,0x00007364,
			0x00060005,0x00000005,0x72646441,0x65737365,0x6f6c4264,0x00006b63,
			0x00080005,0x0000000c,0x65707974,0x7479422e,0x64644165,0x73736572,
			0x66667542,0x00007265,0x00060005,0x00000006,0x65646e49,0x42646578,
			0x65666675,0x00000072,0x00090005,0x0000000d,0x65707974,0x4257522e,
			0x41657479,0x65726464,0x75427373,0x72656666,0x00000000,0x00080005,
			0x00000007,0x6f736552,0x65637275,0x63736544,0x74706972,0x6548726f,
			0x00007061,0x00040005,0x00000001,0x6e69616d,0x00000000,0x00040047,
			0x00000002,0x00000022,0x00000000,0x00040047,0x00000002,0x00000021,
			0x00000000,0x00040047,0x00000003,0x00000022,0x00000000,0x00040047,
			0x00000003,0x00000021,0x00000001,0x00040047,0x00000004,0x00000022,
			0x00000000,0x00040047,0x00000004,0x00000021,0x00000002,0x00040047,
			0x00000005,0x00000022,0x00000000,0x00040047,0x00000005,0x00000021,
			0x00000003,0x00040047,0x00000006,0x00000022,0x00000000,0x00040047,
			0x00000006,0x00000021,0x00000004,0x00040047,0x00000007,0x00000022,
			0x00000000,0x00040047,0x00000007,0x00000021,0x000f4240,0x00040047,
			0x0000000e,0x00000006,0x00000010,0x00050048,0x00000008,0x00000000,
			0x00000023,0x00000000,0x00030047,0x00000008,0x00000002,0x00040047,
			0x0000000f,0x00000006,0x00000010,0x00050048,0x00000009,0x00000000,
			0x00000023,0x00000000,0x00050048,0x00000009,0x00000001,0x00000023,
			0x00000050,0x00050048,0x00000009,0x00000002,0x00000023,0x00000058,
			0x00030047,0x00000009,0x00000002,0x00050048,0x0000000a,0x00000000,
			0x00000023,0x00000000,0x00050048,0x0000000a,0x00000001,0x00000023,
			0x00000004,0x00030047,0x0000000a,0x00000002,0x00050048,0x0000000b,
			0x00000000,0x00000023,0x00000000,0x00030047,0x0000000b,0x00000002,
			0x00040047,0x00000010,0x00000006,0x00000004,0x00050048,0x0000000c,
			0x00000000,0x00000023,0x00000000,0x00040048,0x0000000c,0x00000000,
			0x00000018,0x00030047,0x0000000c,0x00000002,0x00050048,0x0000000d,
			0x00000000,0x00000023,0x00000000,0x00030047,0x0000000d,0x00000002,
			0x00040015,0x00000011,0x00000020,0x00000000,0x0004002b,0x00000011,
			0x00000012,0x00000000,0x00040015,0x00000013,0x00000020,0x00000001,
			0x0004002b,0x00000013,0x00000014,0x00000000,0x0004002b,0x00000011,
			0x00000015,0x00000004,0x0004002b,0x00000011,0x00000016,0x00000002,
			0x0004002b,0x00000011,0x00000017,0x00000001,0x0004002b,0x00000011,
			0x00000018,0x00000003,0x0004002b,0x00000011,0x00000019,0x00000010,
			0x0004002b,0x00000011,0x0000001a,0x00000005,0x0004002b,0x00000013,
			0x0000001b,0x00000001,0x0004002b,0x00000013,0x0000001c,0x00000002,
			0x00040017,0x0000001d,0x00000011,0x00000004,0x0004001c,0x0000000e,
			0x0000001d,0x00000015,0x0003001e,0x00000008,0x0000000e,0x00040020,
			0x0000001e,0x00000002,0x00000008,0x0004001c,0x0000000f,0x0000001d,
			0x0000001a,0x00040017,0x0000001f,0x00000011,0x00000002,0x0005001e,
			0x00000009,0x0000000f,0x0000001f,0x0000001f,0x00040020,0x00000020,
			0x00000002,0x00000009,0x0004001e,0x0000000a,0x00000011,0x00000011,
			0x00040020,0x00000021,0x00000002,0x0000000a,0x0003001e,0x0000000b,
			0x0000001d,0x00040020,0x00000022,0x00000002,0x0000000b,0x0003001d,
			0x00000010,0x00000011,0x0003001e,0x0000000c,0x00000010,0x00040020,
			0x00000023,0x0000000c,0x0000000c,0x0003001e,0x0000000d,0x00000010,
			0x0003001d,0x00000024,0x0000000d,0x00040020,0x00000025,0x0000000c,
			0x00000024,0x00020013,0x00000026,0x00030021,0x00000027,0x00000026,
			0x00040020,0x00000028,0x00000002,0x00000011,0x00040020,0x00000029,
			0x00000002,0x0000001d,0x00040020,0x0000002a,0x0000000c,0x00000011,
			0x00040020,0x0000002b,0x00000002,0x0000001f,0x0004003b,0x0000001e,
			0x00000002,0x00000002,0x0004003b,0x00000020,0x00000003,0x00000002,
			0x0004003b,0x00000021,0x00000004,0x00000002,0x0004003b,0x00000022,
			0x00000005,0x00000002,0x0004003b,0x00000023,0x00000006,0x0000000c,
			0x0004003b,0x00000025,0x00000007,0x0000000c,0x0004002b,0x00000011,
			0x0000002c,0x00000020,0x0004002b,0x00000011,0x0000002d,0x00000030,
			0x0004002b,0x00000011,0x0000002e,0x00000040,0x0004002b,0x00000011,
			0x0000002f,0x00000050,0x0004002b,0x00000011,0x00000030,0x00000060,
			0x0004002b,0x00000011,0x00000031,0x00000070,0x0004002b,0x00000011,
			0x00000032,0x00000080,0x0004002b,0x00000011,0x00000033,0x00000090,
			0x0004002b,0x00000011,0x00000034,0x000000a0,0x0004002b,0x00000011,
			0x00000035,0x000000a8,0x0004002b,0x00000011,0x00000036,0x000000b8,
			0x00050036,0x00000026,0x00000001,0x00000000,0x00000027,0x000200f8,
			0x00000037,0x00050041,0x00000028,0x00000038,0x00000004,0x00000014,
			0x0004003d,0x00000011,0x00000039,0x00000038,0x000500c2,0x00000011,
			0x0000003a,0x00000039,0x00000016,0x00060041,0x00000029,0x0000003b,
			0x00000002,0x00000014,0x00000012,0x0004003d,0x0000001d,0x0000003c,
			0x0000003b,0x00050051,0x00000011,0x0000003d,0x0000003c,0x00000000,
			0x00070041,0x0000002a,0x0000003e,0x00000007,0x00000012,0x00000012,
			0x0000003a,0x0003003e,0x0000003e,0x0000003d,0x00050051,0x00000011,
			0x0000003f,0x0000003c,0x00000001,0x00050080,0x00000011,0x00000040,
			0x0000003a,0x00000017,0x00070041,0x0000002a,0x00000041,0x00000007,
			0x00000012,0x00000012,0x00000040,0x0003003e,0x00000041,0x0000003f,
			0x00050051,0x00000011,0x00000042,0x0000003c,0x00000002,0x00050080,
			0x00000011,0x00000043,0x0000003a,0x00000016,0x00070041,0x0000002a,
			0x00000044,0x00000007,0x00000012,0x00000012,0x00000043,0x0003003e,
			0x00000044,0x00000042,0x00050051,0x00000011,0x00000045,0x0000003c,
			0x00000003,0x00050080,0x00000011,0x00000046,0x0000003a,0x00000018,
			0x00070041,0x0000002a,0x00000047,0x00000007,0x00000012,0x00000012,
			0x00000046,0x0003003e,0x00000047,0x00000045,0x00050080,0x00000011,
			0x00000048,0x00000039,0x00000019,0x000500c2,0x00000011,0x00000049,
			0x00000048,0x00000016,0x00060041,0x00000029,0x0000004a,0x00000002,
			0x00000014,0x00000017,0x0004003d,0x0000001d,0x0000004b,0x0000004a,
			0x00050051,0x00000011,0x0000004c,0x0000004b,0x00000000,0x00070041,
			0x0000002a,0x0000004d,0x00000007,0x00000012,0x00000012,0x00000049,
			0x0003003e,0x0000004d,0x0000004c,0x00050051,0x00000011,0x0000004e,
			0x0000004b,0x00000001,0x00050080,0x00000011,0x0000004f,0x00000049,
			0x00000017,0x00070041,0x0000002a,0x00000050,0x00000007,0x00000012,
			0x00000012,0x0000004f,0x0003003e,0x00000050,0x0000004e,0x00050051,
			0x00000011,0x00000051,0x0000004b,0x00000002,0x00050080,0x00000011,
			0x00000052,0x00000049,0x00000016,0x00070041,0x0000002a,0x00000053,
			0x00000007,0x00000012,0x00000012,0x00000052,0x0003003e,0x00000053,
			0x00000051,0x00050051,0x00000011,0x00000054,0x0000004b,0x00000003,
			0x00050080,0x00000011,0x00000055,0x00000049,0x00000018,0x00070041,
			0x0000002a,0x00000056,0x00000007,0x00000012,0x00000012,0x00000055,
			0x0003003e,0x00000056,0x00000054,0x00050080,0x00000011,0x00000057,
			0x00000039,0x0000002c,0x000500c2,0x00000011,0x00000058,0x00000057,
			0x00000016,0x00060041,0x00000029,0x00000059,0x00000002,0x00000014,
			0x00000016,0x0004003d,0x0000001d,0x0000005a,0x00000059,0x00050051,
			0x00000011,0x0000005b,0x0000005a,0x00000000,0x00070041,0x0000002a,
			0x0000005c,0x00000007,0x00000012,0x00000012,0x00000058,0x0003003e,
			0x0000005c,0x0000005b,0x00050051,0x00000011,0x0000005d,0x0000005a,
			0x00000001,0x00050080,0x00000011,0x0000005e,0x00000058,0x00000017,
			0x00070041,0x0000002a,0x0000005f,0x00000007,0x00000012,0x00000012,
			0x0000005e,0x0003003e,0x0000005f,0x0000005d,0x00050051,0x00000011,
			0x00000060,0x0000005a,0x00000002,0x00050080,0x00000011,0x00000061,
			0x00000058,0x00000016,0x00070041,0x0000002a,0x00000062,0x00000007,
			0x00000012,0x00000012,0x00000061,0x0003003e,0x00000062,0x00000060,
			0x00050051,0x00000011,0x00000063,0x0000005a,0x00000003,0x00050080,
			0x00000011,0x00000064,0x00000058,0x00000018,0x00070041,0x0000002a,
			0x00000065,0x00000007,0x00000012,0x00000012,0x00000064,0x0003003e,
			0x00000065,0x00000063,0x00050080,0x00000011,0x00000066,0x00000039,
			0x0000002d,0x000500c2,0x00000011,0x00000067,0x00000066,0x00000016,
			0x00060041,0x00000029,0x00000068,0x00000002,0x00000014,0x00000018,
			0x0004003d,0x0000001d,0x00000069,0x00000068,0x00050051,0x00000011,
			0x0000006a,0x00000069,0x00000000,0x00070041,0x0000002a,0x0000006b,
			0x00000007,0x00000012,0x00000012,0x00000067,0x0003003e,0x0000006b,
			0x0000006a,0x00050051,0x00000011,0x0000006c,0x00000069,0x00000001,
			0x00050080,0x00000011,0x0000006d,0x00000067,0x00000017,0x00070041,
			0x0000002a,0x0000006e,0x00000007,0x00000012,0x00000012,0x0000006d,
			0x0003003e,0x0000006e,0x0000006c,0x00050051,0x00000011,0x0000006f,
			0x00000069,0x00000002,0x00050080,0x00000011,0x00000070,0x00000067,
			0x00000016,0x00070041,0x0000002a,0x00000071,0x00000007,0x00000012,
			0x00000012,0x00000070,0x0003003e,0x00000071,0x0000006f,0x00050051,
			0x00000011,0x00000072,0x00000069,0x00000003,0x00050080,0x00000011,
			0x00000073,0x00000067,0x00000018,0x00070041,0x0000002a,0x00000074,
			0x00000007,0x00000012,0x00000012,0x00000073,0x0003003e,0x00000074,
			0x00000072,0x00050080,0x00000011,0x00000075,0x00000039,0x0000002e,
			0x000500c2,0x00000011,0x00000076,0x00000075,0x00000016,0x00060041,
			0x00000029,0x00000077,0x00000003,0x00000014,0x00000012,0x0004003d,
			0x0000001d,0x00000078,0x00000077,0x00050051,0x00000011,0x00000079,
			0x00000078,0x00000000,0x00070041,0x0000002a,0x0000007a,0x00000007,
			0x00000012,0x00000012,0x00000076,0x0003003e,0x0000007a,0x00000079,
			0x00050051,0x00000011,0x0000007b,0x00000078,0x00000001,0x00050080,
			0x00000011,0x0000007c,0x00000076,0x00000017,0x00070041,0x0000002a,
			0x0000007d,0x00000007,0x00000012,0x00000012,0x0000007c,0x0003003e,
			0x0000007d,0x0000007b,0x00050051,0x00000011,0x0000007e,0x00000078,
			0x00000002,0x00050080,0x00000011,0x0000007f,0x00000076,0x00000016,
			0x00070041,0x0000002a,0x00000080,0x00000007,0x00000012,0x00000012,
			0x0000007f,0x0003003e,0x00000080,0x0000007e,0x00050051,0x00000011,
			0x00000081,0x00000078,0x00000003,0x00050080,0x00000011,0x00000082,
			0x00000076,0x00000018,0x00070041,0x0000002a,0x00000083,0x00000007,
			0x00000012,0x00000012,0x00000082,0x0003003e,0x00000083,0x00000081,
			0x00050080,0x00000011,0x00000084,0x00000039,0x0000002f,0x000500c2,
			0x00000011,0x00000085,0x00000084,0x00000016,0x00060041,0x00000029,
			0x00000086,0x00000003,0x00000014,0x00000017,0x0004003d,0x0000001d,
			0x00000087,0x00000086,0x00050051,0x00000011,0x00000088,0x00000087,
			0x00000000,0x00070041,0x0000002a,0x00000089,0x00000007,0x00000012,
			0x00000012,0x00000085,0x0003003e,0x00000089,0x00000088,0x00050051,
			0x00000011,0x0000008a,0x00000087,0x00000001,0x00050080,0x00000011,
			0x0000008b,0x00000085,0x00000017,0x00070041,0x0000002a,0x0000008c,
			0x00000007,0x00000012,0x00000012,0x0000008b,0x0003003e,0x0000008c,
			0x0000008a,0x00050051,0x00000011,0x0000008d,0x00000087,0x00000002,
			0x00050080,0x00000011,0x0000008e,0x00000085,0x00000016,0x00070041,
			0x0000002a,0x0000008f,0x00000007,0x00000012,0x00000012,0x0000008e,
			0x0003003e,0x0000008f,0x0000008d,0x00050051,0x00000011,0x00000090,
			0x00000087,0x00000003,0x00050080,0x00000011,0x00000091,0x00000085,
			0x00000018,0x00070041,0x0000002a,0x00000092,0x00000007,0x00000012,
			0x00000012,0x00000091,0x0003003e,0x00000092,0x00000090,0x00050080,
			0x00000011,0x00000093,0x00000039,0x00000030,0x000500c2,0x00000011,
			0x00000094,0x00000093,0x00000016,0x00060041,0x00000029,0x00000095,
			0x00000003,0x00000014,0x00000016,0x0004003d,0x0000001d,0x00000096,
			0x00000095,0x00050051,0x00000011,0x00000097,0x00000096,0x00000000,
			0x00070041,0x0000002a,0x00000098,0x00000007,0x00000012,0x00000012,
			0x00000094,0x0003003e,0x00000098,0x00000097,0x00050051,0x00000011,
			0x00000099,0x00000096,0x00000001,0x00050080,0x00000011,0x0000009a,
			0x00000094,0x00000017,0x00070041,0x0000002a,0x0000009b,0x00000007,
			0x00000012,0x00000012,0x0000009a,0x0003003e,0x0000009b,0x00000099,
			0x00050051,0x00000011,0x0000009c,0x00000096,0x00000002,0x00050080,
			0x00000011,0x0000009d,0x00000094,0x00000016,0x00070041,0x0000002a,
			0x0000009e,0x00000007,0x00000012,0x00000012,0x0000009d,0x0003003e,
			0x0000009e,0x0000009c,0x00050051,0x00000011,0x0000009f,0x00000096,
			0x00000003,0x00050080,0x00000011,0x000000a0,0x00000094,0x00000018,
			0x00070041,0x0000002a,0x000000a1,0x00000007,0x00000012,0x00000012,
			0x000000a0,0x0003003e,0x000000a1,0x0000009f,0x00050080,0x00000011,
			0x000000a2,0x00000039,0x00000031,0x000500c2,0x00000011,0x000000a3,
			0x000000a2,0x00000016,0x00060041,0x00000029,0x000000a4,0x00000003,
			0x00000014,0x00000018,0x0004003d,0x0000001d,0x000000a5,0x000000a4,
			0x00050051,0x00000011,0x000000a6,0x000000a5,0x00000000,0x00070041,
			0x0000002a,0x000000a7,0x00000007,0x00000012,0x00000012,0x000000a3,
			0x0003003e,0x000000a7,0x000000a6,0x00050051,0x00000011,0x000000a8,
			0x000000a5,0x00000001,0x00050080,0x00000011,0x000000a9,0x000000a3,
			0x00000017,0x00070041,0x0000002a,0x000000aa,0x00000007,0x00000012,
			0x00000012,0x000000a9,0x0003003e,0x000000aa,0x000000a8,0x00050051,
			0x00000011,0x000000ab,0x000000a5,0x00000002,0x00050080,0x00000011,
			0x000000ac,0x000000a3,0x00000016,0x00070041,0x0000002a,0x000000ad,
			0x00000007,0x00000012,0x00000012,0x000000ac,0x0003003e,0x000000ad,
			0x000000ab,0x00050051,0x00000011,0x000000ae,0x000000a5,0x00000003,
			0x00050080,0x00000011,0x000000af,0x000000a3,0x00000018,0x00070041,
			0x0000002a,0x000000b0,0x00000007,0x00000012,0x00000012,0x000000af,
			0x0003003e,0x000000b0,0x000000ae,0x00050080,0x00000011,0x000000b1,
			0x00000039,0x00000032,0x000500c2,0x00000011,0x000000b2,0x000000b1,
			0x00000016,0x00060041,0x00000029,0x000000b3,0x00000003,0x00000014,
			0x00000015,0x0004003d,0x0000001d,0x000000b4,0x000000b3,0x00050051,
			0x00000011,0x000000b5,0x000000b4,0x00000000,0x00070041,0x0000002a,
			0x000000b6,0x00000007,0x00000012,0x00000012,0x000000b2,0x0003003e,
			0x000000b6,0x000000b5,0x00050051,0x00000011,0x000000b7,0x000000b4,
			0x00000001,0x00050080,0x00000011,0x000000b8,0x000000b2,0x00000017,
			0x00070041,0x0000002a,0x000000b9,0x00000007,0x00000012,0x00000012,
			0x000000b8,0x0003003e,0x000000b9,0x000000b7,0x00050051,0x00000011,
			0x000000ba,0x000000b4,0x00000002,0x00050080,0x00000011,0x000000bb,
			0x000000b2,0x00000016,0x00070041,0x0000002a,0x000000bc,0x00000007,
			0x00000012,0x00000012,0x000000bb,0x0003003e,0x000000bc,0x000000ba,
			0x00050051,0x00000011,0x000000bd,0x000000b4,0x00000003,0x00050080,
			0x00000011,0x000000be,0x000000b2,0x00000018,0x00070041,0x0000002a,
			0x000000bf,0x00000007,0x00000012,0x00000012,0x000000be,0x0003003e,
			0x000000bf,0x000000bd,0x00050080,0x00000011,0x000000c0,0x00000039,
			0x00000033,0x000500c2,0x00000011,0x000000c1,0x000000c0,0x00000016,
			0x00050041,0x0000002b,0x000000c2,0x00000003,0x0000001b,0x0004003d,
			0x0000001f,0x000000c3,0x000000c2,0x00050041,0x0000002b,0x000000c4,
			0x00000003,0x0000001c,0x0004003d,0x0000001f,0x000000c5,0x000000c4,
			0x00050051,0x00000011,0x000000c6,0x000000c3,0x00000000,0x00050051,
			0x00000011,0x000000c7,0x000000c3,0x00000001,0x00050051,0x00000011,
			0x000000c8,0x000000c5,0x00000000,0x00050051,0x00000011,0x000000c9,
			0x000000c5,0x00000001,0x00070041,0x0000002a,0x000000ca,0x00000007,
			0x00000012,0x00000012,0x000000c1,0x0003003e,0x000000ca,0x000000c6,
			0x00050080,0x00000011,0x000000cb,0x000000c1,0x00000017,0x00070041,
			0x0000002a,0x000000cc,0x00000007,0x00000012,0x00000012,0x000000cb,
			0x0003003e,0x000000cc,0x000000c7,0x00050080,0x00000011,0x000000cd,
			0x000000c1,0x00000016,0x00070041,0x0000002a,0x000000ce,0x00000007,
			0x00000012,0x00000012,0x000000cd,0x0003003e,0x000000ce,0x000000c8,
			0x00050080,0x00000011,0x000000cf,0x000000c1,0x00000018,0x00070041,
			0x0000002a,0x000000d0,0x00000007,0x00000012,0x00000012,0x000000cf,
			0x0003003e,0x000000d0,0x000000c9,0x00050080,0x00000011,0x000000d1,
			0x00000039,0x00000034,0x000500c2,0x00000011,0x000000d2,0x000000d1,
			0x00000016,0x00050041,0x00000028,0x000000d3,0x00000004,0x0000001b,
			0x0004003d,0x00000011,0x000000d4,0x000000d3,0x00070041,0x0000002a,
			0x000000d5,0x00000007,0x00000012,0x00000012,0x000000d2,0x0003003e,
			0x000000d5,0x00000039,0x00050080,0x00000011,0x000000d6,0x000000d2,
			0x00000017,0x00070041,0x0000002a,0x000000d7,0x00000007,0x00000012,
			0x00000012,0x000000d6,0x0003003e,0x000000d7,0x000000d4,0x00050080,
			0x00000011,0x000000d8,0x00000039,0x00000035,0x000500c2,0x00000011,
			0x000000d9,0x000000d8,0x00000016,0x00050041,0x00000029,0x000000da,
			0x00000005,0x00000014,0x0004003d,0x0000001d,0x000000db,0x000000da,
			0x00050051,0x00000011,0x000000dc,0x000000db,0x00000000,0x00070041,
			0x0000002a,0x000000dd,0x00000007,0x00000012,0x00000012,0x000000d9,
			0x0003003e,0x000000dd,0x000000dc,0x00050051,0x00000011,0x000000de,
			0x000000db,0x00000001,0x00050080,0x00000011,0x000000df,0x000000d9,
			0x00000017,0x00070041,0x0000002a,0x000000e0,0x00000007,0x00000012,
			0x00000012,0x000000df,0x0003003e,0x000000e0,0x000000de,0x00050051,
			0x00000011,0x000000e1,0x000000db,0x00000002,0x00050080,0x00000011,
			0x000000e2,0x000000d9,0x00000016,0x00070041,0x0000002a,0x000000e3,
			0x00000007,0x00000012,0x00000012,0x000000e2,0x0003003e,0x000000e3,
			0x000000e1,0x00050051,0x00000011,0x000000e4,0x000000db,0x00000003,
			0x00050080,0x00000011,0x000000e5,0x000000d9,0x00000018,0x00070041,
			0x0000002a,0x000000e6,0x00000007,0x00000012,0x00000012,0x000000e5,
			0x0003003e,0x000000e6,0x000000e4,0x00050080,0x00000011,0x000000e7,
			0x00000039,0x00000036,0x000500c2,0x00000011,0x000000e8,0x000000e7,
			0x00000016,0x00060041,0x0000002a,0x000000e9,0x00000006,0x00000012,
			0x00000012,0x0004003d,0x00000011,0x000000ea,0x000000e9,0x00060041,
			0x0000002a,0x000000eb,0x00000006,0x00000012,0x00000017,0x0004003d,
			0x00000011,0x000000ec,0x000000eb,0x00060041,0x0000002a,0x000000ed,
			0x00000006,0x00000012,0x00000016,0x0004003d,0x00000011,0x000000ee,
			0x000000ed,0x00060041,0x0000002a,0x000000ef,0x00000006,0x00000012,
			0x00000018,0x0004003d,0x00000011,0x000000f0,0x000000ef,0x00070041,
			0x0000002a,0x000000f1,0x00000007,0x00000012,0x00000012,0x000000e8,
			0x0003003e,0x000000f1,0x000000ea,0x00050080,0x00000011,0x000000f2,
			0x000000e8,0x00000017,0x00070041,0x0000002a,0x000000f3,0x00000007,
			0x00000012,0x00000012,0x000000f2,0x0003003e,0x000000f3,0x000000ec,
			0x00050080,0x00000011,0x000000f4,0x000000e8,0x00000016,0x00070041,
			0x0000002a,0x000000f5,0x00000007,0x00000012,0x00000012,0x000000f4,
			0x0003003e,0x000000f5,0x000000ee,0x00050080,0x00000011,0x000000f6,
			0x000000e8,0x00000018,0x00070041,0x0000002a,0x000000f7,0x00000007,
			0x00000012,0x00000012,0x000000f6,0x0003003e,0x000000f7,0x000000f0,
			0x000100fd,0x00010038,
		};
		// dxc -T cs_6_6 -E main -HV 2021 + the shader ABI's flags (rhi_shader_abi.h):
		//   [[vk::binding(0, 0)]] cbuffer DispatchData { uint outputBase; uint tag; };
		//
		//   [numthreads(1, 1, 1)]
		//   void main() {
		//       RWByteAddressBuffer output = ResourceDescriptorHeap[0];
		//       ByteAddressBuffer source = ResourceDescriptorHeap[4];
		//       output.Store2(outputBase, uint2(source.Load(0), tag));
		//   }
		static constexpr uint32_t kSecondarySpirv[] = {
			0x07230203,0x00010600,0x000e0000,0x00000025,0x00000000,0x00020011,
			0x00000001,0x00020011,0x000014b6,0x0008000a,0x5f565053,0x5f545845,
			0x63736564,0x74706972,0x695f726f,0x7865646e,0x00676e69,0x0003000e,
			0x00000000,0x00000001,0x0008000f,0x00000005,0x00000001,0x6e69616d,
			0x00000000,0x00000002,0x00000003,0x00000004,0x00060010,0x00000001,
			0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000005,
			0x00000294,0x00070005,0x00000005,0x65707974,0x7369442e,0x63746170,
			0x74614468,0x00000061,0x00060006,0x00000005,0x00000000,0x7074756f,
			0x61427475,0x00006573,0x00040006,0x00000005,0x00000001,0x00676174,
			0x00060005,0x00000002,0x70736944,0x68637461,0x61746144,0x00000000,
			0x00090005,0x00000006,0x65707974,0x4257522e,0x41657479,0x65726464,
			0x75427373,0x72656666,0x00000000,0x00080005,0x00000003,0x6f736552,
			0x65637275,0x63736544,0x74706972,0x6548726f,0x00007061,0x00080005,
			0x00000007,0x65707974,0x7479422e,0x64644165,0x73736572,0x66667542,
			0x00007265,0x00080005,0x00000004,0x6f736552,0x65637275,0x63736544,
			0x74706972,0x6548726f,0x00007061,0x00040005,0x00000001,0x6e69616d,
			0x00000000,0x00040047,0x00000002,0x00000022,0x00000000,0x00040047,
			0x00000002,0x00000021,0x00000000,0x00040047,0x00000003,0x00000022,
			0x00000000,0x00040047,0x00000003,0x00000021,0x000f4240,0x00040047,
			0x00000004,0x00000022,0x00000000,0x00040047,0x00000004,0x00000021,
			0x000f4240,0x00050048,0x00000005,0x00000000,0x00000023,0x00000000,
			0x00050048,0x00000005,0x00000001,0x00000023,0x00000004,0x00030047,
			0x00000005,0x00000002,0x00040047,0x00000008,0x00000006,0x00000004,
			0x00050048,0x00000006,0x00000000,0x00000023,0x00000000,0x00030047,
			0x00000006,0x00000002,0x00050048,0x00000007,0x00000000,0x00000023,
			0x00000000,0x00040048,0x00000007,0x00000000,0x00000018,0x00030047,
			0x00000007,0x00000002,0x00040015,0x00000009,0x00000020,0x00000000,
			0x0004002b,0x00000009,0x0000000a,0x00000000,0x0004002b,0x00000009,
			0x0000000b,0x00000004,0x00040015,0x0000000c,0x00000020,0x00000001,
			0x0004002b,0x0000000c,0x0000000d,0x00000000,0x0004002b,0x00000009,
			0x0000000e,0x00000002,0x0004002b,0x0000000c,0x0000000f,0x00000001,
			0x0004002b,0x00000009,0x00000010,0x00000001,0x0004001e,0x00000005,
			0x00000009,0x00000009,0x00040020,0x00000011,0x00000002,0x00000005,
			0x0003001d,0x00000008,0x00000009,0x0003001e,0x00000006,0x00000008,
			0x0003001d,0x00000012,0x00000006,0x00040020,0x00000013,0x0000000c,
			0x00000012,0x0003001e,0x00000007,0x00000008,0x0003001d,0x00000014,
			0x00000007,0x00040020,0x00000015,0x0000000c,0x00000014,0x00020013,
			0x00000016,0x00030021,0x00000017,0x00000016,0x00040020,0x00000018,
			0x00000002,0x00000009,0x00040020,0x00000019,0x0000000c,0x00000009,
			0x0004003b,0x00000011,0x00000002,0x00000002,0x0004003b,0x00000013,
			0x00000003,0x0000000c,0x0004003b,0x00000015,0x00000004,0x0000000c,
			0x00050036,0x00000016,0x00000001,0x00000000,0x00000017,0x000200f8,
			0x0000001a,0x00050041,0x00000018,0x0000001b,0x00000002,0x0000000d,
			0x0004003d,0x00000009,0x0000001c,0x0000001b,0x000500c2,0x00000009,
			0x0000001d,0x0000001c,0x0000000e,0x00070041,0x00000019,0x0000001e,
			0x00000004,0x0000000b,0x0000000a,0x0000000a,0x0004003d,0x00000009,
			0x0000001f,0x0000001e,0x00050041,0x00000018,0x00000020,0x00000002,
			0x0000000f,0x0004003d,0x00000009,0x00000021,0x00000020,0x00070041,
			0x00000019,0x00000022,0x00000003,0x0000000a,0x0000000a,0x0000001d,
			0x0003003e,0x00000022,0x0000001f,0x00050080,0x00000009,0x00000023,
			0x0000001d,0x00000010,0x00070041,0x00000019,0x00000024,0x00000003,
			0x0000000a,0x0000000a,0x00000023,0x0003003e,0x00000024,0x00000021,
			0x000100fd,0x00010038,
		};

		const char* model = BindingModelName(g_bindingModel);
		ComputeCheck check{ device, queue, allocator, commandList };
		if (check.Init() != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		IndirectCommandsFeatureInfo indirect{};
		if (Check(device.QueryFeatureInfo(&indirect.header), "QueryFeatureInfo IndirectCommands") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// Resource heap: 0 output, 1 ramp texture, 2 CBV, 3 the push-spill case's indexed buffer, 4 the secondary case's source.
		// Sampler heap: 0 linear clamp, 1 point wrap.
		constexpr uint32_t kRampSlot = 1, kCbvSlot = 2, kIndexedSlot = 3, kSourceSlot = 4;
		rhi::DescriptorHeapPtr caseHeap, samplerHeap;
		rhi::DescriptorHeapDesc caseHeapDesc{};
		caseHeapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
		caseHeapDesc.capacity = 8;
		caseHeapDesc.shaderVisible = true;
		caseHeapDesc.debugName = "VulkanSmokeCaseHeap";
		rhi::DescriptorHeapDesc samplerHeapDesc{};
		samplerHeapDesc.type = rhi::DescriptorHeapType::Sampler;
		samplerHeapDesc.capacity = 2;
		samplerHeapDesc.shaderVisible = true;
		samplerHeapDesc.debugName = "VulkanSmokeCaseSamplerHeap";
		if (Check(device.CreateDescriptorHeap(caseHeapDesc, caseHeap), "CreateDescriptorHeap case") != rhi::Result::Ok ||
			Check(device.CreateDescriptorHeap(samplerHeapDesc, samplerHeap), "CreateDescriptorHeap case sampler") != rhi::Result::Ok ||
			check.WriteOutputView(caseHeap->GetHandle(), 0, ComputeCheck::kOutputWords) != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// Case data, one upload buffer: the CBV's values at 256, the indexed buffer at 512, the secondary sources at 768 and
		// 1024, the PushAddress block at 1280, the IndirectIndex record (a heap index) at 1536.
		constexpr uint64_t kCbvOffset = 256, kIndexedOffset = 512, kSourceAOffset = 768, kSourceBOffset = 1024, kBlockOffset = 1280, kRecordOffset = 1536;
		constexpr uint64_t kCaseDataBytes = 2048;
		constexpr uint32_t kSourceA = 0xA0000001u, kSourceB = 0xB0000002u;
		rhi::ResourcePtr caseData;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kCaseDataBytes, rhi::HeapType::Upload, {}, "VulkanSmokeCaseData"), caseData),
				"CreateCommittedResource case data") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		std::vector<uint32_t> cbvValues(8), indexedValues(4), blockValues(4);
		for (uint32_t i = 0; i < 8; ++i) cbvValues[i] = 0xC0000000u + i;
		for (uint32_t i = 0; i < 4; ++i) indexedValues[i] = 0x50000000u + i;
		for (uint32_t i = 0; i < 4; ++i) blockValues[i] = 0x40000000u + i;
		{
			void* mapped = nullptr;
			caseData->Map(&mapped, 0, kCaseDataBytes);
			if (!mapped) {
				std::fprintf(stderr, "Case data map failed\n");
				return rhi::Result::InvalidArgument;
			}
			auto* bytes = static_cast<uint8_t*>(mapped);
			std::memset(bytes, 0, kCaseDataBytes);
			std::memcpy(bytes + kCbvOffset, cbvValues.data(), cbvValues.size() * 4);
			std::memcpy(bytes + kIndexedOffset, indexedValues.data(), indexedValues.size() * 4);
			std::memcpy(bytes + kSourceAOffset, &kSourceA, 4);
			std::memcpy(bytes + kSourceBOffset, &kSourceB, 4);
			std::memcpy(bytes + kBlockOffset, blockValues.data(), blockValues.size() * 4);
			std::memcpy(bytes + kRecordOffset, &kIndexedSlot, 4);
			caseData->Unmap(0, kCaseDataBytes);
		}
		const uint64_t caseDataAddress = device.GetBufferDeviceAddress({ caseData->GetHandle(), 0 });
		if (caseDataAddress == 0) {
			std::fprintf(stderr, "[%s] case data device address was zero\n", model);
			return rhi::Result::InvalidArgument;
		}
		auto rawView = [&](rhi::DescriptorHeapHandle heap, uint32_t slot, uint64_t byteOffset, const char* what) {
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.buffer.kind = rhi::BufferViewKind::Raw;
			srv.buffer.firstElement = byteOffset / 4;
			srv.buffer.numElements = 4;
			return Check(device.CreateShaderResourceView({ heap, slot }, caseData->GetHandle(), srv), what);
		};
		rhi::CbvDesc cbv{};
		cbv.byteOffset = kCbvOffset;
		cbv.byteSize = 256;
		if (Check(device.CreateConstantBufferView({ caseHeap->GetHandle(), kCbvSlot }, caseData->GetHandle(), cbv), "CreateConstantBufferView case") != rhi::Result::Ok ||
			rawView(caseHeap->GetHandle(), kIndexedSlot, kIndexedOffset, "CreateShaderResourceView case indexed buffer") != rhi::Result::Ok ||
			rawView(caseHeap->GetHandle(), kSourceSlot, kSourceAOffset, "CreateShaderResourceView case source A") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		rhi::SamplerDesc linearClamp{};
		linearClamp.minFilter = rhi::Filter::Linear;
		linearClamp.magFilter = rhi::Filter::Linear;
		linearClamp.mipFilter = rhi::MipFilter::Linear;
		linearClamp.addressU = linearClamp.addressV = linearClamp.addressW = rhi::AddressMode::Clamp;
		rhi::SamplerDesc pointWrap{};
		pointWrap.minFilter = rhi::Filter::Nearest;
		pointWrap.magFilter = rhi::Filter::Nearest;
		pointWrap.mipFilter = rhi::MipFilter::Nearest;
		pointWrap.addressU = pointWrap.addressV = pointWrap.addressW = rhi::AddressMode::Wrap;
		if (Check(device.CreateSampler({ samplerHeap->GetHandle(), 0 }, linearClamp), "CreateSampler case linear clamp") != rhi::Result::Ok ||
			Check(device.CreateSampler({ samplerHeap->GetHandle(), 1 }, pointWrap), "CreateSampler case point wrap") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		// The ramp: a 2x1 RGBA8 texture whose red channel is 0 then 1, uploaded once and left in ShaderResource. Sampled at
		// u = 0.5 a linear filter gives 0.5; outside [0, 1] clamp and wrap pick different texels.
		rhi::ResourcePtr ramp, rampUpload;
		if (Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Tex2D(rhi::Format::R8G8B8A8_UNorm, rhi::HeapType::DeviceLocal, 2, 1, 1, 1, 1,
				rhi::ResourceLayout::Undefined, nullptr, {}, "VulkanSmokeCaseRamp"), ramp), "CreateCommittedResource case ramp") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		rhi::CopyableFootprint rampFootprint{};
		const rhi::CopyableFootprintsInfo rampFootprintInfo = device.GetCopyableFootprints(rhi::FootprintRangeDesc{ .texture = ramp->GetHandle() }, &rampFootprint, 1);
		if (rampFootprintInfo.count != 1 || Check(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(rampFootprintInfo.totalBytes, rhi::HeapType::Upload, {},
				"VulkanSmokeCaseRampUpload"), rampUpload), "CreateCommittedResource case ramp upload") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		{
			void* mapped = nullptr;
			rampUpload->Map(&mapped, 0, rampFootprintInfo.totalBytes);
			if (!mapped) {
				std::fprintf(stderr, "Case ramp upload map failed\n");
				return rhi::Result::InvalidArgument;
			}
			const uint8_t texels[8] = { 0, 0, 0, 255, 255, 0, 0, 255 };
			std::memcpy(static_cast<uint8_t*>(mapped) + rampFootprint.offset, texels, sizeof(texels));
			rampUpload->Unmap(0, rampFootprintInfo.totalBytes);
		}
		{
			commandList.Recycle(allocator);
			rhi::TextureBarrier toCopyDest{
				.texture = ramp->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::None, .afterSync = rhi::ResourceSyncState::Copy,
				.beforeAccess = rhi::ResourceAccessType::None, .afterAccess = rhi::ResourceAccessType::CopyDest,
				.beforeLayout = rhi::ResourceLayout::Undefined, .afterLayout = rhi::ResourceLayout::CopyDest, .discard = true,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toCopyDest, 1 } });
			commandList.CopyBufferToTexture(rhi::BufferTextureCopyFootprint{ .texture = ramp->GetHandle(), .buffer = rampUpload->GetHandle(), .mip = 0, .arraySlice = 0, .footprint = rampFootprint });
			rhi::TextureBarrier toRead{
				.texture = ramp->GetHandle(), .range = {},
				.beforeSync = rhi::ResourceSyncState::Copy, .afterSync = rhi::ResourceSyncState::ComputeShading,
				.beforeAccess = rhi::ResourceAccessType::CopyDest, .afterAccess = rhi::ResourceAccessType::ShaderResource,
				.beforeLayout = rhi::ResourceLayout::CopyDest, .afterLayout = rhi::ResourceLayout::ShaderResource, .discard = false,
			};
			commandList.Barriers(rhi::BarrierBatch{ .textures = { &toRead, 1 } });
			commandList.End();
			const rhi::CommandList submitLists[] = { commandList };
			if (Check(queue.Submit(submitLists, {}), "Queue::Submit case ramp upload") != rhi::Result::Ok || Check(device.WaitIdle(), "Device::WaitIdle case ramp upload") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}
		rhi::SrvDesc rampSrv{};
		rampSrv.dimension = rhi::SrvDim::Texture2D;
		rampSrv.tex2D.mipLevels = 1;
		if (Check(device.CreateShaderResourceView({ caseHeap->GetHandle(), kRampSlot }, ramp->GetHandle(), rampSrv), "CreateShaderResourceView case ramp") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}

		auto makeLayout = [&](const char* what, const rhi::PipelineLayoutDesc& desc, rhi::PipelineLayoutPtr& out) {
			const rhi::Result result = device.CreatePipelineLayout(desc, out);
			if (result != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: CreatePipelineLayout failed with result %u\n", model, what, static_cast<unsigned>(result));
				return rhi::Result::InvalidArgument;
			}
			return rhi::Result::Ok;
		};
		auto noPushData = [](rhi::CommandList& list) { list.Dispatch(1, 1, 1); };
		const rhi::DescriptorHeapHandle heap = caseHeap->GetHandle();
		const rhi::DescriptorHeapHandle samplers = samplerHeap->GetHandle();

		// 1. A static sampler (linear, clamp) in the layout, sampling the ramp: 0.5 at its middle, the edge texels outside.
		{
			const char* what = "static sampler";
			rhi::StaticSamplerDesc staticSampler{};
			staticSampler.sampler = linearClamp;
			staticSampler.visibility = rhi::ShaderStage::Compute;
			staticSampler.set = 0;
			staticSampler.binding = 0;
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = { &staticSampler, 1 }, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kStaticSamplerSpirv, static_cast<uint32_t>(sizeof(kStaticSamplerSpirv)), pipeline) != rhi::Result::Ok ||
				check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), noPushData, { 500u, 0u, 1000u }) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// 2. A sampler from the sampler heap (SamplerDescriptorHeap[1], point wrap; slot 0's linear clamp would read 400, 0, 1000).
		{
			const char* what = "sampler heap read";
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kSamplerHeapSpirv, static_cast<uint32_t>(sizeof(kSamplerHeapSpirv)), pipeline) != rhi::Result::Ok ||
				check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), noPushData, { 0u, 1000u, 1000u }) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// 3. A CBV from the resource heap (ConstantBuffer<T> = ResourceDescriptorHeap[2]), at a non-zero offset in its buffer.
		{
			const char* what = "resource heap CBV read";
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kHeapCbvSpirv, static_cast<uint32_t>(sizeof(kHeapCbvSpirv)), pipeline) != rhi::Result::Ok ||
				check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), noPushData, cbvValues) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// 4. Null views (a texture and a raw buffer): reads return zero. Needs nullDescriptor, which a device BasicRHI creates
		// may not have enabled (an adopted DXVK device does).
		{
			const char* what = "null SRV read";
			rhi::DescriptorHeapPtr nullHeap;
			rhi::DescriptorHeapDesc nullHeapDesc = caseHeapDesc;
			nullHeapDesc.capacity = 3;
			nullHeapDesc.debugName = "VulkanSmokeNullHeap";
			if (Check(device.CreateDescriptorHeap(nullHeapDesc, nullHeap), "CreateDescriptorHeap null views") != rhi::Result::Ok ||
				check.WriteOutputView(nullHeap->GetHandle(), 0, ComputeCheck::kOutputWords) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			rhi::SrvDesc nullTexture{};
			nullTexture.dimension = rhi::SrvDim::Texture2D;
			nullTexture.formatOverride = rhi::Format::R8G8B8A8_UNorm;
			nullTexture.tex2D.mipLevels = 1;
			rhi::SrvDesc nullBuffer{};
			nullBuffer.dimension = rhi::SrvDim::Buffer;
			nullBuffer.buffer.kind = rhi::BufferViewKind::Raw;
			nullBuffer.buffer.numElements = 4;
			const rhi::Result textureResult = device.CreateShaderResourceView({ nullHeap->GetHandle(), 1 }, rhi::ResourceHandle{}, nullTexture);
			const rhi::Result bufferResult = device.CreateShaderResourceView({ nullHeap->GetHandle(), 2 }, rhi::ResourceHandle{}, nullBuffer);
			if (textureResult == rhi::Result::Unsupported || bufferResult == rhi::Result::Unsupported) {
				std::printf("BasicRHIVulkanSmoke [%s]: null SRV read skipped (nullDescriptor not enabled on this device)\n", model);
			}
			else if (textureResult != rhi::Result::Ok || bufferResult != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: null views failed with results %u (texture), %u (buffer)\n", model, what,
					static_cast<unsigned>(textureResult), static_cast<unsigned>(bufferResult));
				return rhi::Result::InvalidArgument;
			}
			else {
				rhi::PipelineLayoutPtr layout;
				rhi::PipelinePtr pipeline;
				if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
					check.CreatePipeline(what, layout->GetHandle(), kNullSrvSpirv, static_cast<uint32_t>(sizeof(kNullSrvSpirv)), pipeline) != rhi::Result::Ok ||
					check.Run(what, nullHeap->GetHandle(), samplers, layout->GetHandle(), pipeline->GetHandle(), noPushData, { 0u, 0u, 0u }) != rhi::Result::Ok) {
					return rhi::Result::InvalidArgument;
				}
			}
		}

		// 5. EmulatedRootConstants (8 dwords, read through a pushed address) beside a RootConstants32 output offset. The second
		// dispatch changes two words: each dispatch must see its own snapshot, with the other words kept.
		{
			const char* what = "emulated root constants";
			rhi::PushConstantRangeDesc ranges[2]{};
			ranges[0].visibility = rhi::ShaderStage::Compute;
			ranges[0].num32BitValues = 8;
			ranges[0].binding = 0;
			ranges[0].type = rhi::PushConstantRangeType::EmulatedRootConstants;
			ranges[1].visibility = rhi::ShaderStage::Compute;
			ranges[1].num32BitValues = 1;
			ranges[1].binding = 1;
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { ranges, 2 }, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kEmulatedRootConstantsSpirv, static_cast<uint32_t>(sizeof(kEmulatedRootConstantsSpirv)), pipeline) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			uint32_t first[8];
			for (uint32_t i = 0; i < 8; ++i) first[i] = 0xE0000000u + i;
			const uint32_t changed[2] = { 0xE1000004u, 0xE1000005u };
			std::vector<uint32_t> expected(first, first + 8);
			expected.insert(expected.end(), first, first + 8);
			expected[8 + 4] = changed[0];
			expected[8 + 5] = changed[1];
			if (check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), [&](rhi::CommandList& list) {
					const uint32_t firstBase = 0, secondBase = 32;
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 8, first);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 1, 0, 1, &firstBase);
					list.Dispatch(1, 1, 1);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 4, 2, changed);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 1, 0, 1, &secondBase);
					list.Dispatch(1, 1, 1);
				}, expected) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// 6. Three RootConstants32 ranges (4, 2 and 3 dwords, packed back to back), pushed out of order and partially updated.
		{
			const char* what = "multiple root-constant ranges";
			rhi::PushConstantRangeDesc ranges[3]{};
			const uint32_t sizes[3] = { 4, 2, 3 };
			for (uint32_t r = 0; r < 3; ++r) {
				ranges[r].visibility = rhi::ShaderStage::Compute;
				ranges[r].num32BitValues = sizes[r];
				ranges[r].binding = r;
			}
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { ranges, 3 }, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kRootConstantRangesSpirv, static_cast<uint32_t>(sizeof(kRootConstantRangesSpirv)), pipeline) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			const uint32_t a[4] = { 0xA1u, 0xA2u, 0xA3u, 0xA4u };
			const uint32_t b[2] = { 0xB1u, 0xB2u };
			const uint32_t c[3] = { 0xC1u, 0xC2u, 0xC3u };
			const uint32_t a2 = 0xAAu;
			if (check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), [&](rhi::CommandList& list) {
					list.PushConstants(rhi::ShaderStage::Compute, 0, 2, 0, 3, c);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 4, a);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 1, 0, 2, b);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 2, 1, &a2);
					list.Dispatch(1, 1, 1);
				}, { a[0], a[1], a2, a[3], b[0], b[1], c[0], c[1], c[2] }) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
		}

		// 7. Push spill: 64 B + 96 B + 8 B of root constants, over the descriptor-buffer model's 128 B budget, so there the 96 B
		// range (at least) lives in memory. Its words 20-21 are a PushAddress (a constant block) and 22-23 an IndirectIndex
		// record address (a heap index, then the buffer it names), so both take the extra load through the spill pointer. The
		// second dispatch changes a pushed word, a spilled word and the tail: the spilled bytes must be re-sent when dirty.
		if (!indirect.indirectBindings) {
			std::printf("BasicRHIVulkanSmoke [%s]: push spill validation skipped (indirect bindings not supported by this device)\n", model);
		}
		else {
			const char* what = "push spill (pushed, spilled, spilled addresses)";
			constexpr uint32_t kPushedWords = 16, kSpilledWords = 24, kTailWords = 2;
			rhi::PushConstantRangeDesc ranges[3]{};
			const uint32_t sizes[3] = { kPushedWords, kSpilledWords, kTailWords };
			for (uint32_t r = 0; r < 3; ++r) {
				ranges[r].visibility = rhi::ShaderStage::Compute;
				ranges[r].num32BitValues = sizes[r];
				ranges[r].binding = r;
			}
			rhi::LayoutBindingRange bindings[2]{};
			bindings[0].set = 0;
			bindings[0].binding = 3;
			bindings[0].visibility = rhi::ShaderStage::Compute;
			bindings[0].source = rhi::LayoutRangeSource::PushAddress;
			bindings[0].addressRootIndex = 1;
			bindings[0].addressOffset32 = 20;
			bindings[1].set = 0;
			bindings[1].binding = 4;
			bindings[1].visibility = rhi::ShaderStage::Compute;
			bindings[1].source = rhi::LayoutRangeSource::IndirectIndex;
			bindings[1].addressRootIndex = 1;
			bindings[1].addressOffset32 = 22;
			bindings[1].recordOffset = 0;
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = { bindings, 2 }, .pushConstants = { ranges, 3 }, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kPushSpillSpirv, static_cast<uint32_t>(sizeof(kPushSpillSpirv)), pipeline) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}

			uint32_t pushed[kPushedWords], spilled[kSpilledWords];
			for (uint32_t i = 0; i < kPushedWords; ++i) pushed[i] = 0x10000000u + i;
			for (uint32_t i = 0; i < 20; ++i) spilled[i] = 0x20000000u + i;
			const uint64_t blockAddress = caseDataAddress + kBlockOffset;
			const uint64_t recordAddress = caseDataAddress + kRecordOffset;
			std::memcpy(&spilled[20], &blockAddress, 8);
			std::memcpy(&spilled[22], &recordAddress, 8);
			const uint32_t firstTail[kTailWords] = { 0u, 0x30000000u };
			const uint32_t secondTail[kTailWords] = { 256u, 0x3F000000u };  // output byte 256: word 64
			const uint32_t changedPushed = 0x1F000000u, changedSpilled = 0x2F000003u;

			// What the shader writes at word `at`: the pushed words, the spilled ones (addresses included), the tail, the
			// PushAddress block, the indexed buffer.
			std::vector<uint32_t> expected(64 + 50, ComputeCheck::kSentinel);
			auto expect = [&](uint32_t at, const uint32_t* pushedWords, const uint32_t* spilledWords, const uint32_t* tail) {
				for (uint32_t i = 0; i < kPushedWords; ++i) expected[at + i] = pushedWords[i];
				for (uint32_t i = 0; i < kSpilledWords; ++i) expected[at + 16 + i] = spilledWords[i];
				expected[at + 40] = tail[0];
				expected[at + 41] = tail[1];
				for (uint32_t i = 0; i < 4; ++i) expected[at + 42 + i] = blockValues[i];
				for (uint32_t i = 0; i < 4; ++i) expected[at + 46 + i] = indexedValues[i];
			};
			expect(0, pushed, spilled, firstTail);
			uint32_t pushedAfter[kPushedWords], spilledAfter[kSpilledWords];
			std::memcpy(pushedAfter, pushed, sizeof(pushed));
			std::memcpy(spilledAfter, spilled, sizeof(spilled));
			pushedAfter[0] = changedPushed;
			spilledAfter[3] = changedSpilled;
			expect(64, pushedAfter, spilledAfter, secondTail);

			if (check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), [&](rhi::CommandList& list) {
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kPushedWords, pushed);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 1, 0, kSpilledWords, spilled);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 2, 0, kTailWords, firstTail);
					list.Dispatch(1, 1, 1);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 1, &changedPushed);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 1, 3, 1, &changedSpilled);
					list.PushConstants(rhi::ShaderStage::Compute, 0, 2, 0, kTailWords, secondTail);
					list.Dispatch(1, 1, 1);
				}, expected) != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: words 0-15 pushed, 16-35 spilled data, 36-39 spilled addresses, 40-41 tail, 42-45 PushAddress block, "
					"46-49 IndirectIndex buffer; the second dispatch's copy starts at word 64\n", model, what);
				return rhi::Result::InvalidArgument;
			}
		}

		// 8. Secondaries: a secondary (recorded outside any pass) binds its own heap, whose output view is words 64-127 and
		// whose source holds kSourceB, and dispatches. The primary dispatches before it and, after ExecuteCommands, binds the
		// same heap as before again (the backend's bind dedupe must not skip it: ExecuteCommands leaves bindings undefined)
		// and dispatches once more, which must see its own heap.
		if (!commandList.SupportsSecondaryLists()) {
			std::printf("BasicRHIVulkanSmoke [%s]: secondary list validation skipped (not supported by this backend)\n", model);
		}
		else {
			const char* what = "secondary SetDescriptorHeaps then primary dispatch";
			rhi::PushConstantRangeDesc range{};
			range.visibility = rhi::ShaderStage::Compute;
			range.num32BitValues = 2;
			range.binding = 0;
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			if (makeLayout(what, rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { &range, 1 }, .staticSamplers = {}, .flags = rhi::PF_None }, layout) != rhi::Result::Ok ||
				check.CreatePipeline(what, layout->GetHandle(), kSecondarySpirv, static_cast<uint32_t>(sizeof(kSecondarySpirv)), pipeline) != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			rhi::DescriptorHeapPtr secondaryHeap;
			rhi::DescriptorHeapDesc secondaryHeapDesc = caseHeapDesc;
			secondaryHeapDesc.capacity = kSourceSlot + 1;
			secondaryHeapDesc.debugName = "VulkanSmokeSecondaryHeap";
			if (Check(device.CreateDescriptorHeap(secondaryHeapDesc, secondaryHeap), "CreateDescriptorHeap secondary") != rhi::Result::Ok ||
				check.WriteOutputView(secondaryHeap->GetHandle(), 64, 64) != rhi::Result::Ok ||
				rawView(secondaryHeap->GetHandle(), kSourceSlot, kSourceBOffset, "CreateShaderResourceView secondary source B") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}

			rhi::CommandAllocatorPtr secondaryAllocator;
			rhi::CommandListPtr secondary;
			if (Check(device.CreateCommandAllocator(rhi::QueueKind::Graphics, secondaryAllocator), "CreateCommandAllocator secondary") != rhi::Result::Ok) {
				return rhi::Result::InvalidArgument;
			}
			const rhi::Result createResult = device.CreateSecondaryCommandList(rhi::QueueKind::Graphics, secondaryAllocator.Get(), secondary);
			if (createResult != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: CreateSecondaryCommandList failed with result %u\n", model, what, static_cast<unsigned>(createResult));
				return rhi::Result::InvalidArgument;
			}
			secondary->Recycle(secondaryAllocator.Get());
			rhi::SecondaryInheritance inheritance{};
			inheritance.insideRendering = false;
			inheritance.debugName = "VulkanSmokeSecondary";
			const rhi::Result beginResult = secondary->BeginSecondary(inheritance);
			if (beginResult != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: BeginSecondary failed with result %u\n", model, what, static_cast<unsigned>(beginResult));
				return rhi::Result::InvalidArgument;
			}
			const uint32_t secondaryData[2] = { 0u, 2u };
			secondary->SetDescriptorHeaps(secondaryHeap->GetHandle(), samplers);
			secondary->BindLayout(layout->GetHandle());
			secondary->BindPipeline(pipeline->GetHandle());
			secondary->PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 2, secondaryData);
			secondary->Dispatch(1, 1, 1);
			if (secondary->SupportsCheckedEnd()) {
				const rhi::Result endResult = secondary->EndChecked();
				if (endResult != rhi::Result::Ok) {
					std::fprintf(stderr, "[%s] %s: secondary recording failed (EndChecked result %u)\n", model, what, static_cast<unsigned>(endResult));
					return rhi::Result::InvalidArgument;
				}
			}
			else {
				secondary->End();
			}

			std::vector<uint32_t> expected(66, ComputeCheck::kSentinel);
			expected[0] = kSourceA;
			expected[1] = 1u;
			expected[2] = kSourceA;
			expected[3] = 3u;
			expected[64] = kSourceB;
			expected[65] = 2u;
			rhi::CommandList* const secondaries[] = { &secondary.Get() };
			if (check.Run(what, heap, samplers, layout->GetHandle(), pipeline->GetHandle(), [&](rhi::CommandList& list) {
					const uint32_t before[2] = { 0u, 1u }, after[2] = { 8u, 3u };
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 2, before);
					list.Dispatch(1, 1, 1);
					list.ExecuteCommands(secondaries);
					list.SetDescriptorHeaps(heap, samplers);
					list.BindLayout(layout->GetHandle());
					list.BindPipeline(pipeline->GetHandle());
					list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 2, after);
					list.Dispatch(1, 1, 1);
				}, expected) != rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] %s: words 0-1 primary before, 2-3 primary after ExecuteCommands (source A), 64-65 the secondary (source B)\n", model, what);
				return rhi::Result::InvalidArgument;
			}
		}
		return rhi::Result::Ok;
	}

#ifdef _WIN32
	// The environment variable's value; `present` is false (and the value empty) when it is not set.
	std::string ReadEnvironment(const char* name, bool& present) {
		char* value = nullptr;
		size_t size = 0;
		present = _dupenv_s(&value, &size, name) == 0 && value != nullptr;
		std::string result = present ? value : "";
		std::free(value);
		return result;
	}

	void WriteEnvironment(const char* name, const char* value) {
		_putenv_s(name, value ? value : "");  // an empty value removes it
	}
#else
	std::string ReadEnvironment(const char* name, bool& present) {
		const char* value = std::getenv(name);
		present = value != nullptr;
		return present ? value : "";
	}

	void WriteEnvironment(const char* name, const char* value) {
		if (value) setenv(name, value, 1);
		else unsetenv(name);
	}
#endif

	// The startup support check (rhi::vulkan::EvaluateBindingModels) on the device's physical device: its own limits must admit
	// the model the device runs on, and BASICRHI_VK_LIMITS=base (every limit at the Vulkan base-spec minimum, set in-process
	// for the call) must reject the descriptor-buffer model, with a report line per check.
	rhi::Result ValidateBindingModelLimits(rhi::Device& device) {
		const char* model = BindingModelName(g_bindingModel);
		const VkInstance instance = rhi::vulkan::get_instance(device);
		const VkPhysicalDevice physicalDevice = rhi::vulkan::get_physical_device(device);
		const PFN_vkGetInstanceProcAddr getInstanceProcAddr = vkGetInstanceProcAddr;
		if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || getInstanceProcAddr == nullptr) {
			std::fprintf(stderr, "[%s] binding model limits: no instance (%p), physical device (%p) or vkGetInstanceProcAddr (%p)\n", model,
				reinterpret_cast<void*>(instance), reinterpret_cast<void*>(physicalDevice), reinterpret_cast<void*>(getInstanceProcAddr));
			return rhi::Result::InvalidArgument;
		}
		const rhi::vulkan::BindingRequirements requirements{};
		auto print = [&](const char* label, const rhi::vulkan::BindingModelReport& report) {
			std::printf("BasicRHIVulkanSmoke [%s]: EvaluateBindingModels (%s): heap %s, descriptor buffer %s\n", model, label,
				(report.supportedModels & rhi::vulkan::BindingModel_DescriptorHeap) != 0 ? "supported" : "unsupported",
				(report.supportedModels & rhi::vulkan::BindingModel_DescriptorBuffer) != 0 ? "supported" : "unsupported");
			std::printf("    capacities: heap model %u resources / %u samplers, descriptor-buffer model %u resources / %u samplers\n",
				report.heapModelResources, report.heapModelSamplers, report.bufferModelResources, report.bufferModelSamplers);
			for (const std::string& line : report.checks) {
				std::printf("    %s\n", line.c_str());
			}
		};

		const rhi::vulkan::BindingModelReport deviceReport = rhi::vulkan::EvaluateBindingModels(getInstanceProcAddr, instance, physicalDevice, requirements);
		print("device limits", deviceReport);
		if ((deviceReport.supportedModels & g_bindingModel) == 0) {
			std::fprintf(stderr, "[%s] binding model limits: the device runs on a model its own evaluation reports unsupported (supported 0x%x)\n",
				model, deviceReport.supportedModels);
			return rhi::Result::InvalidArgument;
		}

		bool hadLimits = false;
		const std::string previousLimits = ReadEnvironment("BASICRHI_VK_LIMITS", hadLimits);
		WriteEnvironment("BASICRHI_VK_LIMITS", "base");
		const rhi::vulkan::BindingModelReport baseReport = rhi::vulkan::EvaluateBindingModels(getInstanceProcAddr, instance, physicalDevice, requirements);
		WriteEnvironment("BASICRHI_VK_LIMITS", hadLimits ? previousLimits.c_str() : nullptr);
		print("BASICRHI_VK_LIMITS=base", baseReport);
		if ((baseReport.supportedModels & rhi::vulkan::BindingModel_DescriptorBuffer) != 0) {
			std::fprintf(stderr, "[%s] binding model limits: BASICRHI_VK_LIMITS=base still reports the descriptor-buffer model supported "
				"(expected rejected: base-spec per-stage limits cannot hold a %u-descriptor heap)\n", model, requirements.heapCapacities.minResources);
			return rhi::Result::InvalidArgument;
		}
		if (baseReport.checks.empty()) {
			std::fprintf(stderr, "[%s] binding model limits: BASICRHI_VK_LIMITS=base produced no report lines (expected one per check)\n", model);
			return rhi::Result::InvalidArgument;
		}

		// BASICRHI_VK_MAX_DESCRIPTORS=n (set in-process for the call): a device whose descriptor-count limits are all n. The
		// device's own model must then be supported at capacity n while n reaches the minimum, and rejected below it.
		bool hadMaxDescriptors = false;
		const std::string previousMaxDescriptors = ReadEnvironment("BASICRHI_VK_MAX_DESCRIPTORS", hadMaxDescriptors);
		auto evaluateClamped = [&](uint32_t maxDescriptors) {
			const std::string value = std::to_string(maxDescriptors);
			WriteEnvironment("BASICRHI_VK_MAX_DESCRIPTORS", value.c_str());
			rhi::vulkan::BindingModelReport report = rhi::vulkan::EvaluateBindingModels(getInstanceProcAddr, instance, physicalDevice, requirements);
			WriteEnvironment("BASICRHI_VK_MAX_DESCRIPTORS", hadMaxDescriptors ? previousMaxDescriptors.c_str() : nullptr);
			const std::string label = "BASICRHI_VK_MAX_DESCRIPTORS=" + value;
			print(label.c_str(), report);
			return report;
		};
		auto ownModelResources = [&](const rhi::vulkan::BindingModelReport& report) {
			return g_bindingModel == rhi::vulkan::BindingModel_DescriptorBuffer ? report.bufferModelResources : report.heapModelResources;
		};
		{
			constexpr uint32_t kSmaller = 500000;
			const rhi::vulkan::BindingModelReport report = evaluateClamped(kSmaller);
			if ((report.supportedModels & g_bindingModel) == 0 || ownModelResources(report) != kSmaller) {
				std::fprintf(stderr, "[%s] binding model limits: BASICRHI_VK_MAX_DESCRIPTORS=%u gave supported 0x%x with %u resources for the device's model, "
					"expected supported with %u\n", model, kSmaller, report.supportedModels, ownModelResources(report), kSmaller);
				return rhi::Result::InvalidArgument;
			}
			const std::string capacityText = std::to_string(kSmaller);
			bool lineNamesCapacity = false;
			for (const std::string& line : report.checks) {
				lineNamesCapacity = lineNamesCapacity || line.find(capacityText) != std::string::npos;
			}
			if (!lineNamesCapacity) {
				std::fprintf(stderr, "[%s] binding model limits: BASICRHI_VK_MAX_DESCRIPTORS=%u produced no report line naming the %u capacity\n",
					model, kSmaller, kSmaller);
				return rhi::Result::InvalidArgument;
			}
		}
		{
			constexpr uint32_t kTooSmall = 1000;
			const rhi::vulkan::BindingModelReport report = evaluateClamped(kTooSmall);
			if ((report.supportedModels & g_bindingModel) != 0) {
				std::fprintf(stderr, "[%s] binding model limits: BASICRHI_VK_MAX_DESCRIPTORS=%u still reports the device's model supported "
					"(%u resources; expected rejected below the %u minimum)\n", model, kTooSmall, ownModelResources(report), requirements.heapCapacities.minResources);
				return rhi::Result::InvalidArgument;
			}
		}
		std::printf("BasicRHIVulkanSmoke [%s]: binding model limits OK\n", model);
		return rhi::Result::Ok;
	}

	// The shader-visible heap capacities the device was set up with (ShaderFeatureInfo, structVersion 2): within
	// DescriptorHeapCapacities' [min, preferred], a heap of exactly that size created, one larger refused. Nothing assumes
	// the preferred 1,000,000: under BASICRHI_VK_MAX_DESCRIPTORS the device is set up smaller.
	rhi::Result ValidateHeapCapacities(rhi::Device& device) {
		const char* model = BindingModelName(g_bindingModel);
		ShaderFeatureInfo shader{};
		if (Check(device.QueryFeatureInfo(&shader.header), "QueryFeatureInfo Features") != rhi::Result::Ok) {
			return rhi::Result::InvalidArgument;
		}
		const rhi::DescriptorHeapCapacities wanted{};  // what main's DeviceCreateInfo asked for
		std::printf("BasicRHIVulkanSmoke [%s]: shader-visible heap capacities %u resources, %u samplers\n", model,
			shader.maxShaderVisibleResourceDescriptors, shader.maxShaderVisibleSamplerDescriptors);
		struct Kind { const char* name; rhi::DescriptorHeapType type; uint32_t max, min, preferred; };
		const Kind kinds[2] = {
			{ "resource", rhi::DescriptorHeapType::CbvSrvUav, shader.maxShaderVisibleResourceDescriptors, wanted.minResources, wanted.preferredResources },
			{ "sampler", rhi::DescriptorHeapType::Sampler, shader.maxShaderVisibleSamplerDescriptors, wanted.minSamplers, wanted.preferredSamplers },
		};
		for (const Kind& kind : kinds) {
			if (kind.max == 0 || kind.max < kind.min || kind.max > kind.preferred) {
				std::fprintf(stderr, "[%s] heap capacities: %s capacity %u, expected %u..%u\n", model, kind.name, kind.max, kind.min, kind.preferred);
				return rhi::Result::InvalidArgument;
			}
			rhi::DescriptorHeapDesc desc{};
			desc.type = kind.type;
			desc.shaderVisible = true;
			desc.capacity = kind.max;
			desc.debugName = "VulkanSmokeHeapAtCapacity";
			{
				rhi::DescriptorHeapPtr heap;
				const rhi::Result result = device.CreateDescriptorHeap(desc, heap);
				if (result != rhi::Result::Ok) {
					std::fprintf(stderr, "[%s] heap capacities: a %s heap of exactly the capacity (%u) failed with result %u\n", model, kind.name, kind.max,
						static_cast<unsigned>(result));
					return rhi::Result::InvalidArgument;
				}
			}
			desc.capacity = kind.max + 1;
			desc.debugName = "VulkanSmokeHeapOverCapacity";
			rhi::DescriptorHeapPtr heap;
			if (device.CreateDescriptorHeap(desc, heap) == rhi::Result::Ok) {
				std::fprintf(stderr, "[%s] heap capacities: a %s heap one over the capacity (%u) was created, expected a refusal\n", model, kind.name, kind.max + 1);
				return rhi::Result::InvalidArgument;
			}
		}
		std::printf("BasicRHIVulkanSmoke [%s]: heap capacities OK\n", model);
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

	// BASICRHI_VK_BINDING=heap|buffer forces the binding model of a device BasicRHI creates (the CTest variants): the device
	// must come up on exactly that model, or the run fails rather than passing on the other one.
	bool bindingRequested = false;
	const std::string requestedBinding = ReadEnvironment("BASICRHI_VK_BINDING", bindingRequested);
	const char* requestedLabel = bindingRequested ? requestedBinding.c_str() : "<unset>";

	rhi::DevicePtr device;
	if (Check(rhi::CreateVulkanDevice(createInfo, device), "CreateVulkanDevice") != rhi::Result::Ok) {
		std::fprintf(stderr, "BasicRHIVulkanSmoke: no Vulkan device for BASICRHI_VK_BINDING=%s (the requested binding model is unsupported here, or device creation failed)\n",
			requestedLabel);
		return 1;
	}

	g_bindingModel = rhi::vulkan::GetBindingModel(device.Get());
	std::printf("BasicRHIVulkanSmoke: binding model %s (BASICRHI_VK_BINDING=%s)\n", BindingModelName(g_bindingModel), requestedLabel);
	if (g_bindingModel != rhi::vulkan::BindingModel_DescriptorHeap && g_bindingModel != rhi::vulkan::BindingModel_DescriptorBuffer) {
		std::fprintf(stderr, "BasicRHIVulkanSmoke: GetBindingModel returned 0x%x, expected exactly one binding model\n", g_bindingModel);
		return 1;
	}
	if (bindingRequested) {
		const uint32_t requestedModel = requestedBinding == "buffer" ? rhi::vulkan::BindingModel_DescriptorBuffer
			: requestedBinding == "heap" ? rhi::vulkan::BindingModel_DescriptorHeap : 0u;
		if (requestedModel == 0) {
			std::fprintf(stderr, "BasicRHIVulkanSmoke: BASICRHI_VK_BINDING=%s is neither heap nor buffer\n", requestedLabel);
			return 1;
		}
		if (g_bindingModel != requestedModel) {
			std::fprintf(stderr, "BasicRHIVulkanSmoke: BASICRHI_VK_BINDING=%s requested the %s model, but the device came up on the %s model\n",
				requestedLabel, BindingModelName(requestedModel), BindingModelName(g_bindingModel));
			return 1;
		}
	}

#if BASICRHI_SMOKE_HAS_FFX
	if (!ValidateFidelityFXVulkanContext(device.Get())) {
		return 1;
	}
#else
	std::puts("BasicRHIVulkanSmoke: FidelityFX validation skipped (SDK not in build tree)");
#endif

	auto graphicsQueue = device->GetQueue(rhi::QueueKind::Graphics);
	if (!graphicsQueue) {
		std::fprintf(stderr, "Graphics queue acquisition failed\n");
		return 1;
	}

	if (ValidateUploadBuffer(device.Get()) != rhi::Result::Ok) {
		return 1;
	}
	if (ValidateCrossQueueOwnership(device.Get(), graphicsQueue, rhi::QueueSharing::Concurrent, "concurrent") != rhi::Result::Ok) {
		return 1;
	}
	if (ValidateCrossQueueOwnership(device.Get(), graphicsQueue, rhi::QueueSharing::Exclusive, "exclusive") != rhi::Result::Ok) {
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

	rhi::DescriptorHeapPtr shaderVisibleHeap;
	rhi::DescriptorHeapPtr samplerHeap;
	if (ValidateDescriptors(device.Get(), commandList.Get(), shaderVisibleHeap, samplerHeap) != rhi::Result::Ok) {
		return 1;
	}

	rhi::PipelineLayoutPtr pipelineLayout;
	rhi::PipelinePtr pipeline;
	if (ValidatePipelineLayoutPushData(device.Get(), commandList.Get(), pipelineLayout, pipeline) != rhi::Result::Ok) {
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

	if (ValidateIndirectPipelineSet(device.Get(), graphicsQueue, allocator.Get(), commandList.Get(), shaderVisibleHeap.Get(), samplerHeap.Get()) != rhi::Result::Ok) {
		return 1;
	}

	if (ValidateBindingModelCoverage(device.Get(), graphicsQueue, allocator.Get(), commandList.Get()) != rhi::Result::Ok) {
		return 1;
	}

	if (ValidateBindingModelLimits(device.Get()) != rhi::Result::Ok) {
		return 1;
	}

	if (ValidateHeapCapacities(device.Get()) != rhi::Result::Ok) {
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
