// Adopting a host-created VkDevice (the DXVK embedding model): the test plays the
// host, creates its own instance/device/buffer, hands them to BasicRHI, and checks
// that BasicRHI (1) writes through a non-owning buffer import, (2) brackets every
// queue submission with the host's lock, and (3) leaves the device, queue and
// buffer alive and untouched when it shuts down.

#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_vulkan.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
	PFN_vkCmdPipelineBarrier2 originalBarrier2 = nullptr;
	std::vector<VkBufferMemoryBarrier2> capturedBufferBarriers;
	uint32_t capturedGlobalBarriers = 0;
	VKAPI_ATTR void VKAPI_CALL CaptureBarrier2(VkCommandBuffer commands, const VkDependencyInfo* dependency) {
		capturedGlobalBarriers += dependency->memoryBarrierCount;
		for (uint32_t i = 0; i < dependency->bufferMemoryBarrierCount; ++i)
			capturedBufferBarriers.push_back(dependency->pBufferMemoryBarriers[i]);
		originalBarrier2(commands, dependency);
	}
	struct HostDevice {
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = 0;
		std::vector<const char*> extensions;
		VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		VkPhysicalDeviceVulkan12Features vulkan12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features vulkan13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
	};

	struct LockCounters {
		int locks = 0;
		int unlocks = 0;
		int depth = 0;
		bool unbalanced = false;
	};

	void Lock(void* user, VkQueue) {
		auto* c = static_cast<LockCounters*>(user);
		++c->locks;
		if (++c->depth != 1) c->unbalanced = true;
	}
	void Unlock(void* user, VkQueue) {
		auto* c = static_cast<LockCounters*>(user);
		++c->unlocks;
		if (--c->depth != 0) c->unbalanced = true;
	}

	// Host-side submission hook (the DXVK command-stream model): copies each batch and
	// submits it later, from the host's own point in its stream.
	struct DeferredSubmissions {
		std::vector<rhi::vulkan::NativeResourceAccess> accesses;
		bool completeAccesses = false;
		struct Batch {
			std::vector<VkSemaphoreSubmitInfo> waits;
			std::vector<VkCommandBufferSubmitInfo> commandBuffers;
			std::vector<VkSemaphoreSubmitInfo> signals;
		};
		std::vector<Batch> pending;
		int handed = 0;
	};

	VkResult DeferSubmit(void* user, VkQueue, const VkSubmitInfo2& submit) {
		auto* deferred = static_cast<DeferredSubmissions*>(user);
		DeferredSubmissions::Batch batch;
		batch.waits.assign(submit.pWaitSemaphoreInfos, submit.pWaitSemaphoreInfos + submit.waitSemaphoreInfoCount);
		batch.commandBuffers.assign(submit.pCommandBufferInfos, submit.pCommandBufferInfos + submit.commandBufferInfoCount);
		batch.signals.assign(submit.pSignalSemaphoreInfos, submit.pSignalSemaphoreInfos + submit.signalSemaphoreInfoCount);
		deferred->pending.push_back(std::move(batch));
		++deferred->handed;
		return VK_SUCCESS;
	}

	VkResult DeferSubmitResources(void* user, VkQueue queue, const VkSubmitInfo2& submit,
		rhi::Span<rhi::vulkan::CommandBufferResourceAccesses> manifests) {
		auto& deferred = *static_cast<DeferredSubmissions*>(user);
		if (manifests.size != submit.commandBufferInfoCount) return VK_ERROR_UNKNOWN;
		deferred.completeAccesses = true;
		for (uint32_t i = 0; i < manifests.size; ++i) {
			if (manifests.data[i].commandBuffer != submit.pCommandBufferInfos[i].commandBuffer) return VK_ERROR_UNKNOWN;
			deferred.completeAccesses &= manifests.data[i].complete;
			for (const auto& access : manifests.data[i].accesses) deferred.accesses.push_back(access);
		}
		return DeferSubmit(user, queue, submit);
	}

	VkResult FlushDeferred(DeferredSubmissions& deferred, VkQueue queue) {
		for (const auto& batch : deferred.pending) {
			VkSubmitInfo2 info{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			info.waitSemaphoreInfoCount = static_cast<uint32_t>(batch.waits.size());
			info.pWaitSemaphoreInfos = batch.waits.data();
			info.commandBufferInfoCount = static_cast<uint32_t>(batch.commandBuffers.size());
			info.pCommandBufferInfos = batch.commandBuffers.data();
			info.signalSemaphoreInfoCount = static_cast<uint32_t>(batch.signals.size());
			info.pSignalSemaphoreInfos = batch.signals.data();
			if (const VkResult result = vkQueueSubmit2(queue, 1, &info, VK_NULL_HANDLE); result != VK_SUCCESS) return result;
		}
		deferred.pending.clear();
		return VK_SUCCESS;
	}

	bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> props(count);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
		for (const auto& p : props) if (std::strcmp(p.extensionName, name) == 0) return true;
		return false;
	}

	// Returns 0 on success, 77 when no suitable device exists (skip), 1 on failure.
	int CreateHostDevice(HostDevice& host) {
		if (volkInitialize() != VK_SUCCESS) return 77;
		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "VulkanAdoptionSmokeHost";
		app.apiVersion = VK_API_VERSION_1_3;
		VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &app;
		if (vkCreateInstance(&ici, nullptr, &host.instance) != VK_SUCCESS) return 77;
		volkLoadInstanceOnly(host.instance);

		uint32_t count = 0;
		vkEnumeratePhysicalDevices(host.instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(host.instance, &count, devices.data());
		for (VkPhysicalDevice candidate : devices) {
			if (HasDeviceExtension(candidate, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) &&
				HasDeviceExtension(candidate, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
				host.physicalDevice = candidate;
				break;
			}
		}
		if (!host.physicalDevice) return 77;

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(host.physicalDevice, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(host.physicalDevice, &familyCount, families.data());
		for (uint32_t i = 0; i < familyCount; ++i) {
			if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
				host.family = i;
				break;
			}
		}

		host.extensions = { VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME };
		host.vulkan12.bufferDeviceAddress = VK_TRUE;
		host.vulkan12.timelineSemaphore = VK_TRUE;
		host.vulkan12.descriptorIndexing = VK_TRUE;
		host.vulkan12.runtimeDescriptorArray = VK_TRUE;
		host.vulkan12.scalarBlockLayout = VK_TRUE;
		host.vulkan13.dynamicRendering = VK_TRUE;
		host.vulkan13.synchronization2 = VK_TRUE;
		host.vulkan13.maintenance4 = VK_TRUE;
		host.descriptorHeap.descriptorHeap = VK_TRUE;
		host.features.pNext = &host.vulkan12;
		host.vulkan12.pNext = &host.vulkan13;
		host.vulkan13.pNext = &host.descriptorHeap;

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		qci.queueFamilyIndex = host.family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		dci.pNext = &host.features;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = static_cast<uint32_t>(host.extensions.size());
		dci.ppEnabledExtensionNames = host.extensions.data();
		if (vkCreateDevice(host.physicalDevice, &dci, nullptr, &host.device) != VK_SUCCESS) return 1;
		volkLoadDevice(host.device);
		vkGetDeviceQueue(host.device, host.family, 0, &host.queue);
		return 0;
	}

	uint32_t FindHostVisibleMemory(VkPhysicalDevice device, uint32_t typeBits) {
		VkPhysicalDeviceMemoryProperties props{};
		vkGetPhysicalDeviceMemoryProperties(device, &props);
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
			if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
		return UINT32_MAX;
	}

#define REQUIRE(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)
}

int main(int argc, char** argv) {
	// "submit": BasicRHI hands submissions to the host (QueueSubmissionHooks::submit)
	// instead of submitting under the host's lock.
	const bool resourceSubmits = argc > 1 && std::strcmp(argv[1], "resources") == 0;
	const bool hostSubmits = resourceSubmits || (argc > 1 && std::strcmp(argv[1], "submit") == 0);
	HostDevice host;
	if (const int created = CreateHostDevice(host); created != 0) {
		std::printf(created == 77 ? "SKIP: no Vulkan device with VK_EXT_descriptor_heap\n" : "FAIL: host device creation\n");
		return created == 77 ? 0 : 1;
	}

	// Host-owned destination buffer: 512 bytes, BasicRHI may write only [128, 384).
	constexpr VkDeviceSize kBufferBytes = 512;
	constexpr uint64_t kWriteOffset = 128;
	constexpr uint64_t kWriteBytes = 256;
	constexpr uint8_t kSentinel = 0xCD;
	VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bci.size = kBufferBytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	VkBuffer hostBuffer = VK_NULL_HANDLE;
	REQUIRE(vkCreateBuffer(host.device, &bci, nullptr, &hostBuffer) == VK_SUCCESS, "vkCreateBuffer");
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(host.device, hostBuffer, &req);
	VkMemoryAllocateFlagsInfo flags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	mai.pNext = &flags;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = FindHostVisibleMemory(host.physicalDevice, req.memoryTypeBits);
	REQUIRE(mai.memoryTypeIndex != UINT32_MAX, "host-visible memory type");
	VkDeviceMemory hostMemory = VK_NULL_HANDLE;
	REQUIRE(vkAllocateMemory(host.device, &mai, nullptr, &hostMemory) == VK_SUCCESS, "vkAllocateMemory");
	REQUIRE(vkBindBufferMemory(host.device, hostBuffer, hostMemory, 0) == VK_SUCCESS, "vkBindBufferMemory");
	void* hostMapped = nullptr;
	REQUIRE(vkMapMemory(host.device, hostMemory, 0, kBufferBytes, 0, &hostMapped) == VK_SUCCESS, "vkMapMemory");
	std::memset(hostMapped, kSentinel, kBufferBytes);

	LockCounters counters;
	DeferredSubmissions deferred;
	{
		rhi::vulkan::AdoptedVulkanDeviceInfo info{};
		info.getInstanceProcAddr = vkGetInstanceProcAddr;
		info.instance = host.instance;
		info.instanceApiVersion = VK_API_VERSION_1_3;
		info.physicalDevice = host.physicalDevice;
		info.device = host.device;
		info.enabledDeviceExtensions = host.extensions.data();
		info.enabledDeviceExtensionCount = static_cast<uint32_t>(host.extensions.size());
		info.enabledFeatureChain = &host.features;
		info.queues[0] = { host.queue, host.family, 0 };
		if (hostSubmits)
			info.submissionHooks = { &deferred, nullptr, nullptr, &DeferSubmit };
		else
			info.submissionHooks = { &counters, &Lock, &Unlock };
		if (resourceSubmits) {
			info.submissionHooks.submit = nullptr;
			info.submissionHooks.submitResources = &DeferSubmitResources;
		}
		rhi::DevicePtr device;
		REQUIRE(rhi::vulkan::AdoptVulkanDevice(info, device) == rhi::Result::Ok, "AdoptVulkanDevice");
		REQUIRE(rhi::vulkan::get_device(device.Get()) == host.device, "adopted device handle");

		auto graphics = device->GetQueue(rhi::QueueKind::Graphics);
		auto compute = device->GetQueue(rhi::QueueKind::Compute);
		auto copy = device->GetQueue(rhi::QueueKind::Copy);
		REQUIRE(graphics && compute && copy, "queues for every kind");
		REQUIRE(rhi::vulkan::get_queue(compute) == host.queue && rhi::vulkan::get_queue(copy) == host.queue, "unset kinds alias the graphics queue");
		rhi::SwapchainPtr swapchain;
		REQUIRE(device->CreateSwapchain(reinterpret_cast<void*>(1), 16, 16, rhi::Format::R8G8B8A8_UNorm, 2, false, swapchain) == rhi::Result::Unsupported,
			"adopted devices present through their host");

		rhi::ResourcePtr imported;
		rhi::vulkan::ImportedBufferDesc importDesc{};
		importDesc.buffer = hostBuffer;
		importDesc.size = kBufferBytes;
		importDesc.usage = bci.usage;
		importDesc.debugName = "AdoptionSmokeHostBuffer";
		REQUIRE(rhi::vulkan::import_buffer(device.Get(), importDesc, imported) == rhi::Result::Ok, "import_buffer");
		REQUIRE(rhi::vulkan::get_buffer_device_address(imported.Get()) != 0, "imported buffer device address");

		rhi::ResourcePtr upload;
		REQUIRE(device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kWriteBytes, rhi::HeapType::Upload, {}, "AdoptionSmokeUpload"), upload) == rhi::Result::Ok,
			"upload buffer");
		void* uploadMapped = nullptr;
		upload->Map(&uploadMapped, 0, kWriteBytes);
		REQUIRE(uploadMapped, "upload map");
		for (uint64_t i = 0; i < kWriteBytes; ++i) static_cast<uint8_t*>(uploadMapped)[i] = static_cast<uint8_t>(i);
		upload->Unmap(0, kWriteBytes);

		rhi::TimelinePtr timeline;
		REQUIRE(device->CreateTimeline(timeline, 0, "AdoptionSmokeTimeline") == rhi::Result::Ok, "timeline");
		rhi::CommandAllocatorPtr allocator;
		rhi::CommandListPtr list;
		REQUIRE(device->CreateCommandAllocator(rhi::QueueKind::Graphics, allocator) == rhi::Result::Ok, "allocator");
		REQUIRE(device->CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list) == rhi::Result::Ok, "command list");
		if (resourceSubmits) {
			std::array<rhi::vulkan::ResourceAccessDeclaration, 2> accesses{};
			accesses[0].resource = upload->GetHandle();
			accesses[0].sync = rhi::ResourceSyncState::Copy;
			accesses[0].access = rhi::ResourceAccessType::CopySource;
			accesses[1].resource = imported->GetHandle();
			accesses[1].offset = kWriteOffset;
			accesses[1].size = kWriteBytes;
			accesses[1].sync = rhi::ResourceSyncState::Copy;
			accesses[1].access = rhi::ResourceAccessType::CopyDest;
			accesses[1].write = true;
			REQUIRE(rhi::vulkan::set_command_list_resource_accesses(list.Get(), {accesses.data(), 2}) == rhi::Result::Ok,
				"attach native access manifest");
			accesses[1].size = kBufferBytes;
			REQUIRE(rhi::vulkan::set_command_list_resource_accesses(list.Get(), {accesses.data(), 2}) == rhi::Result::InvalidArgument,
				"reject invalid buffer range without replacing the valid manifest");
			accesses[1].size = kWriteBytes;
			accesses[1].write = false;
			REQUIRE(rhi::vulkan::set_command_list_resource_accesses(list.Get(), {accesses.data(), 2}) == rhi::Result::InvalidArgument,
				"reject writes mislabeled as reads");
		}
		if (hostSubmits) {
			std::array<rhi::BufferBarrier, 2> scoped{};
			for (auto& barrier : scoped) {
				barrier.buffer = imported->GetHandle();
				barrier.size = 64;
				barrier.afterAccess = rhi::ResourceAccessType::ShaderResource;
			}
			scoped[0].beforeSync = rhi::ResourceSyncState::Copy;
			scoped[0].beforeAccess = rhi::ResourceAccessType::CopyDest;
			scoped[0].afterSync = rhi::ResourceSyncState::VertexShading;
			scoped[1].offset = 64;
			scoped[1].beforeSync = rhi::ResourceSyncState::ComputeShading;
			scoped[1].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
			scoped[1].afterSync = rhi::ResourceSyncState::PixelShading;
			originalBarrier2 = vkCmdPipelineBarrier2;
			vkCmdPipelineBarrier2 = &CaptureBarrier2;
			list->Barriers(rhi::BarrierBatch{ .buffers = { scoped.data(), static_cast<uint32_t>(scoped.size()) } });
			vkCmdPipelineBarrier2 = originalBarrier2;
			REQUIRE(capturedGlobalBarriers == 0 && capturedBufferBarriers.size() == 2, "resource barriers remain resource scoped");
			REQUIRE(capturedBufferBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
				capturedBufferBarriers[1].srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, "independent producer stages are not unioned");
			REQUIRE((capturedBufferBarriers[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) == 0 &&
				capturedBufferBarriers[1].dstStageMask == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, "independent consumer stages are not unioned");
			REQUIRE(capturedBufferBarriers[0].offset == 0 && capturedBufferBarriers[1].offset == 64 &&
				capturedBufferBarriers[0].size == 64 && capturedBufferBarriers[1].size == 64, "byte ranges survive lowering");
		}
		auto full = rhi::FullMemoryBarrier();
		list->Barriers(rhi::BarrierBatch{ .globals = { &full, 1 } });
		list->CopyBufferRegion(imported->GetHandle(), kWriteOffset, upload->GetHandle(), 0, kWriteBytes);
		list->Barriers(rhi::BarrierBatch{ .globals = { &full, 1 } });
		list->End();
		const rhi::CommandList lists[] = { list.Get() };
		const rhi::TimelinePoint signal{ timeline->GetHandle(), 1 };
		REQUIRE(graphics.Submit(lists, { .signals = { &signal, 1 } }) == rhi::Result::Ok, "submit");
		if (hostSubmits) {
			REQUIRE(deferred.handed == 1 && counters.locks == 0, "submission handed to the host, not submitted under its lock");
			if (resourceSubmits) {
				REQUIRE(deferred.completeAccesses && deferred.accesses.size() == 2, "complete manifest delivered with its command buffer");
				REQUIRE(deferred.accesses[0].size == kWriteBytes && !deferred.accesses[0].write, "whole upload range resolved");
				const auto& output = deferred.accesses[1];
				REQUIRE(output.buffer == hostBuffer && output.offset == kWriteOffset && output.size == kWriteBytes && output.write,
					"native backing and output interval preserved");
				REQUIRE(output.stages == VK_PIPELINE_STAGE_2_TRANSFER_BIT && output.access == VK_ACCESS_2_TRANSFER_WRITE_BIT,
					"native access scopes preserved");
			}
			REQUIRE(timeline->GetCompletedValue() == 0, "nothing ran before the host submitted");
			REQUIRE(FlushDeferred(deferred, host.queue) == VK_SUCCESS, "host submits the handed batch");
		} else {
			REQUIRE(counters.locks >= 1 && counters.locks == counters.unlocks && !counters.unbalanced, "submission bracketed by the host lock");
		}
		REQUIRE(timeline->HostWait(1) == rhi::Result::Ok, "timeline host wait");
		REQUIRE(device->WaitIdle() == rhi::Result::Ok, "WaitIdle waits on BasicRHI timelines only");

		const auto* bytes = static_cast<const uint8_t*>(hostMapped);
		for (uint64_t i = 0; i < kBufferBytes; ++i) {
			const bool inside = i >= kWriteOffset && i < kWriteOffset + kWriteBytes;
			const uint8_t expected = inside ? static_cast<uint8_t>(i - kWriteOffset) : kSentinel;
			if (bytes[i] != expected) {
				std::fprintf(stderr, "FAIL: byte %llu = 0x%02X, expected 0x%02X\n", static_cast<unsigned long long>(i), bytes[i], expected);
				return 1;
			}
		}
		// Destruction order: resources, then the device. Nothing the host owns may be destroyed.
	}

	REQUIRE(vkDeviceWaitIdle(host.device) == VK_SUCCESS, "host device survives BasicRHI shutdown");
	std::memset(hostMapped, 0, kBufferBytes); // host buffer memory still mapped and valid
	vkUnmapMemory(host.device, hostMemory);
	vkDestroyBuffer(host.device, hostBuffer, nullptr);
	vkFreeMemory(host.device, hostMemory, nullptr);
	vkDestroyDevice(host.device, nullptr);
	vkDestroyInstance(host.instance, nullptr);
	std::printf("VulkanAdoptionSmoke: ok (%s: locks=%d, handed=%d)\n", hostSubmits ? "host submit" : "lock", counters.locks, deferred.handed);
	return 0;
}
