#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_vulkan.h"

#include "ThirdParty/FFX/ffx_api_loader.h"
#include "ThirdParty/FFX/ffx_upscale.h"
#include "ThirdParty/FFX/host/backends/vk/ffx_vk.h"
#include "ThirdParty/FFX/vk/ffx_api_vk.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>

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

	if (!ValidateFidelityFXVulkanContext(device.Get())) {
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