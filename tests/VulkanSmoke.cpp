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
