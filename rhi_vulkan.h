#pragma once

#include "rhi.h"

#include <memory>

namespace rhi {

	struct VulkanDevice {
		~VulkanDevice() = default;
		void Shutdown() noexcept;

		Device self{};
		std::weak_ptr<VulkanDevice> selfWeak;
	};

	extern const DeviceVTable g_vkdevvt;
	extern const QueueVTable g_vkqvt;
	extern const CommandListVTable g_vkclvt;
	extern const SwapchainVTable g_vkscvt;
	extern const CommandAllocatorVTable g_vkcalvt;
	extern const ResourceVTable g_vkbuf_rvt;
	extern const ResourceVTable g_vktex_rvt;
	extern const QueryPoolVTable g_vkqpvt;
	extern const PipelineVTable g_vkpsovt;
	extern const WorkGraphVTable g_vkwgvt;
	extern const PipelineLayoutVTable g_vkplvt;
	extern const CommandSignatureVTable g_vkcsvt;
	extern const DescriptorHeapVTable g_vkdhvt;
	extern const SamplerVTable g_vksvt;
	extern const TimelineVTable g_vktlvt;
	extern const HeapVTable g_vkhevt;

} // namespace rhi