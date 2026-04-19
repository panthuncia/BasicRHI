#pragma once
#include <cstdint>
#include "rhi.h"
#include "rhi_interop.h" // for QueryNative* + interop structs
#include "rhi_interop_dx12.h" // Used when building with D3D12
#include "rhi_colors.h"

// Optional feature switches (define them in your build if needed):
// - RHI_ENABLE_PIX            -> enable PIX markers on D3D12 (requires <pix3.h>)
// - RHI_ENABLE_VULKAN_MARKERS -> enable VK_EXT_debug_utils markers on Vulkan

#if !defined(RHI_ENABLE_PIX)
#  if defined(BASICRHI_ENABLE_PIX) && BASICRHI_ENABLE_PIX
#    define RHI_ENABLE_PIX 1
#  else
#    define RHI_ENABLE_PIX 0
#  endif
#endif

#if RHI_ENABLE_PIX
#  ifndef USE_PIX
#    define USE_PIX 1
#  endif
#  define PIX_ENABLE_BLOCK_ARGUMENT_COPY 0
#  include <pix3.h>
#endif

#if defined(RHI_ENABLE_VULKAN_MARKERS)
#include <vulkan/vulkan.h>
#endif

namespace rhi::debug {

    // Colors are 0xAARRGGBB
    using Color = rhi::colors::RGBA8;

#if RHI_ENABLE_PIX
    inline std::uint64_t to_pix(Color c) noexcept {
        using namespace rhi::colors;
        return PIX_COLOR(r(c), g(c), b(c));
    }
#endif

    constexpr Color RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) noexcept {
        return (Color(a) << 24) | (Color(r) << 16) | (Color(g) << 8) | Color(b);
    }

    inline void toRGBAf(Color c, float out[4]) noexcept {
        const float inv = 1.0f / 255.0f;
        out[0] = ((c >> 16) & 0xFF) * inv; // R
        out[1] = ((c >> 8) & 0xFF) * inv; // G
        out[2] = ((c >> 0) & 0xFF) * inv; // B
        out[3] = ((c >> 24) & 0xFF) * inv; // A
    }

    // Optional one-time init for backends that need function pointers (Vulkan).
    // D3D12 + PIX requires no init; Vulkan will query vk* proc addrs here.
    bool Init(Device d) noexcept;
    void Shutdown(Device /*d*/) noexcept; // currently a no-op

    inline Result GetInstrumentationCapabilities(Device d, DebugInstrumentationCapabilities& out) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationCapabilities) {
            return Result::Unsupported;
        }
        return d.vt->getDebugInstrumentationCapabilities(&d, out);
    }

    inline Result GetInstrumentationState(Device d, DebugInstrumentationState& out) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationState) {
            return Result::Unsupported;
        }
        return d.vt->getDebugInstrumentationState(&d, out);
    }

    inline uint32_t GetInstrumentationFeatureCount(Device d) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationFeatureCount) {
            return 0;
        }
        return d.vt->getDebugInstrumentationFeatureCount(&d);
    }

    inline Result CopyInstrumentationFeatures(Device d,
        uint32_t first,
        DebugInstrumentationFeature* outFeatures,
        uint32_t capacity,
        uint32_t* copied = nullptr) noexcept {
        if (!d || !d.vt || !d.vt->copyDebugInstrumentationFeatures) {
            if (copied) {
                *copied = 0;
            }
            return Result::Unsupported;
        }
        return d.vt->copyDebugInstrumentationFeatures(&d, first, outFeatures, capacity, copied);
    }

    inline std::vector<DebugInstrumentationFeature> GetInstrumentationFeatures(Device d) {
        std::vector<DebugInstrumentationFeature> features(GetInstrumentationFeatureCount(d));
        if (features.empty()) {
            return features;
        }

        uint32_t copied = 0;
        if (!IsOk(CopyInstrumentationFeatures(d, 0, features.data(), static_cast<uint32_t>(features.size()), &copied))) {
            features.clear();
            return features;
        }

        features.resize(copied);
        return features;
    }

    inline uint32_t GetInstrumentationPipelineCount(Device d) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationPipelineCount) {
            return 0;
        }
        return d.vt->getDebugInstrumentationPipelineCount(&d);
    }

    inline Result CopyInstrumentationPipelines(Device d,
        uint32_t first,
        DebugInstrumentationPipeline* outPipelines,
        uint32_t capacity,
        uint32_t* copied = nullptr) noexcept {
        if (!d || !d.vt || !d.vt->copyDebugInstrumentationPipelines) {
            if (copied) {
                *copied = 0;
            }
            return Result::Unsupported;
        }
        return d.vt->copyDebugInstrumentationPipelines(&d, first, outPipelines, capacity, copied);
    }

    inline std::vector<DebugInstrumentationPipeline> GetInstrumentationPipelines(Device d) {
        std::vector<DebugInstrumentationPipeline> pipelines(GetInstrumentationPipelineCount(d));
        if (pipelines.empty()) {
            return pipelines;
        }

        uint32_t copied = 0;
        if (!IsOk(CopyInstrumentationPipelines(d, 0, pipelines.data(), static_cast<uint32_t>(pipelines.size()), &copied))) {
            pipelines.clear();
            return pipelines;
        }

        pipelines.resize(copied);
        return pipelines;
    }

    inline uint32_t GetInstrumentationPipelineUsageCount(Device d) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationPipelineUsageCount) {
            return 0;
        }
        return d.vt->getDebugInstrumentationPipelineUsageCount(&d);
    }

    inline Result CopyInstrumentationPipelineUsages(Device d,
        uint32_t first,
        DebugInstrumentationPipelineUsage* outUsages,
        uint32_t capacity,
        uint32_t* copied = nullptr) noexcept {
        if (!d || !d.vt || !d.vt->copyDebugInstrumentationPipelineUsages) {
            if (copied) {
                *copied = 0;
            }
            return Result::Unsupported;
        }
        return d.vt->copyDebugInstrumentationPipelineUsages(&d, first, outUsages, capacity, copied);
    }

    inline std::vector<DebugInstrumentationPipelineUsage> GetInstrumentationPipelineUsages(Device d) {
        std::vector<DebugInstrumentationPipelineUsage> usages(GetInstrumentationPipelineUsageCount(d));
        if (usages.empty()) {
            return usages;
        }

        uint32_t copied = 0;
        if (!IsOk(CopyInstrumentationPipelineUsages(d, 0, usages.data(), static_cast<uint32_t>(usages.size()), &copied))) {
            usages.clear();
            return usages;
        }

        usages.resize(copied);
        return usages;
    }

    inline uint32_t GetInstrumentationDiagnosticCount(Device d) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationDiagnosticCount) {
            return 0;
        }
        return d.vt->getDebugInstrumentationDiagnosticCount(&d);
    }

    inline Result CopyInstrumentationDiagnostics(Device d,
        uint32_t first,
        DebugInstrumentationDiagnostic* outDiagnostics,
        uint32_t capacity,
        uint32_t* copied = nullptr) noexcept {
        if (!d || !d.vt || !d.vt->copyDebugInstrumentationDiagnostics) {
            if (copied) {
                *copied = 0;
            }
            return Result::Unsupported;
        }
        return d.vt->copyDebugInstrumentationDiagnostics(&d, first, outDiagnostics, capacity, copied);
    }

    inline std::vector<DebugInstrumentationDiagnostic> GetInstrumentationDiagnostics(Device d) {
        std::vector<DebugInstrumentationDiagnostic> diagnostics(GetInstrumentationDiagnosticCount(d));
        if (diagnostics.empty()) {
            return diagnostics;
        }

        uint32_t copied = 0;
        if (!IsOk(CopyInstrumentationDiagnostics(d, 0, diagnostics.data(), static_cast<uint32_t>(diagnostics.size()), &copied))) {
            diagnostics.clear();
            return diagnostics;
        }

        diagnostics.resize(copied);
        return diagnostics;
    }

    inline uint32_t GetInstrumentationIssueCount(Device d) noexcept {
        if (!d || !d.vt || !d.vt->getDebugInstrumentationIssueCount) {
            return 0;
        }
        return d.vt->getDebugInstrumentationIssueCount(&d);
    }

    inline Result CopyInstrumentationIssues(Device d,
        uint32_t first,
        DebugInstrumentationIssue* outIssues,
        uint32_t capacity,
        uint32_t* copied = nullptr) noexcept {
        if (!d || !d.vt || !d.vt->copyDebugInstrumentationIssues) {
            if (copied) {
                *copied = 0;
            }
            return Result::Unsupported;
        }
        return d.vt->copyDebugInstrumentationIssues(&d, first, outIssues, capacity, copied);
    }

    inline std::vector<DebugInstrumentationIssue> GetInstrumentationIssues(Device d) {
        std::vector<DebugInstrumentationIssue> issues(GetInstrumentationIssueCount(d));
        if (issues.empty()) {
            return issues;
        }

        uint32_t copied = 0;
        if (!IsOk(CopyInstrumentationIssues(d, 0, issues.data(), static_cast<uint32_t>(issues.size()), &copied))) {
            issues.clear();
            return issues;
        }

        issues.resize(copied);
        return issues;
    }

    inline Result SetGlobalInstrumentationMask(Device d, uint64_t featureMask) noexcept {
        if (!d || !d.vt || !d.vt->setDebugGlobalInstrumentationMask) {
            return Result::Unsupported;
        }
        return d.vt->setDebugGlobalInstrumentationMask(&d, featureMask);
    }

    inline Result SetPipelineInstrumentationMask(Device d, uint64_t pipelineUid, uint64_t featureMask) noexcept {
        if (!d || !d.vt || !d.vt->setDebugPipelineInstrumentationMask) {
            return Result::Unsupported;
        }
        return d.vt->setDebugPipelineInstrumentationMask(&d, pipelineUid, featureMask);
    }

    inline bool IsInstrumentationFeatureEnabled(uint64_t featureMask, const DebugInstrumentationFeature& feature) noexcept {
        return feature.featureBit != 0 && (featureMask & feature.featureBit) == feature.featureBit;
    }

    inline bool IsInstrumentationFeatureEnabled(const DebugInstrumentationState& state, const DebugInstrumentationFeature& feature) noexcept {
        return IsInstrumentationFeatureEnabled(state.globalFeatureMask, feature);
    }

    inline uint64_t UpdateInstrumentationFeatureMask(uint64_t featureMask, const DebugInstrumentationFeature& feature, bool enabled) noexcept {
        if (feature.featureBit == 0) {
            return featureMask;
        }

        return enabled ? (featureMask | feature.featureBit) : (featureMask & ~feature.featureBit);
    }

    inline Result SetInstrumentationFeatureEnabled(Device d, uint64_t featureMask, const DebugInstrumentationFeature& feature, bool enabled) noexcept {
        return SetGlobalInstrumentationMask(d, UpdateInstrumentationFeatureMask(featureMask, feature, enabled));
    }

    inline Result SetInstrumentationFeatureEnabled(Device d, const DebugInstrumentationState& state, const DebugInstrumentationFeature& feature, bool enabled) noexcept {
        return SetInstrumentationFeatureEnabled(d, state.globalFeatureMask, feature, enabled);
    }

    inline Result SetSynchronousRecording(Device d, bool enabled) noexcept {
        if (!d || !d.vt || !d.vt->setDebugSynchronousRecording) {
            return Result::Unsupported;
        }
        return d.vt->setDebugSynchronousRecording(&d, enabled);
    }

    inline Result SetTexelAddressing(Device d, bool enabled) noexcept {
        if (!d || !d.vt || !d.vt->setDebugTexelAddressing) {
            return Result::Unsupported;
        }
        return d.vt->setDebugTexelAddressing(&d, enabled);
    }

    inline Result SetInstrumentationContext(CommandList cmd, const char* passName, const char* techniquePath) noexcept {
        if (!cmd || !cmd.vt || !cmd.vt->setDebugInstrumentationContext) {
            return Result::Unsupported;
        }
        cmd.vt->setDebugInstrumentationContext(&cmd, passName, techniquePath);
        return Result::Ok;
    }

    // Command list markers
    void Begin(CommandList cmd, Color color, const char* name) noexcept;
    void End(CommandList cmd) noexcept;
    void Marker(CommandList cmd, Color color, const char* name) noexcept;

    // Queue markers
    void Begin(Queue q, Color color, const char* name) noexcept;
    void End(Queue q) noexcept;
    void Marker(Queue q, Color color, const char* name) noexcept;

    // RAII scopes
    struct Scope {
        Scope(CommandList cmd, Color color, const char* name) noexcept : cmd_(cmd), active_(true) {
            Begin(cmd_, color, name);
        }
        ~Scope() { if (active_) End(cmd_); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& o) noexcept : cmd_(o.cmd_), active_(o.active_) { o.active_ = false; }
        Scope& operator=(Scope&& o) noexcept {
            if (this != &o) { if (active_) End(cmd_); cmd_ = o.cmd_; active_ = o.active_; o.active_ = false; }
            return *this;
        }
    private:
        CommandList cmd_{};
        bool active_{ false };
    };

    struct QueueScope {
        QueueScope(Queue q, Color color, const char* name) noexcept : q_(q), active_(true) {
            Begin(q_, color, name);
        }
        ~QueueScope() { if (active_) End(q_); }
        QueueScope(const QueueScope&) = delete;
        QueueScope& operator=(const QueueScope&) = delete;
        QueueScope(QueueScope&& o) noexcept : q_(o.q_), active_(o.active_) { o.active_ = false; }
        QueueScope& operator=(QueueScope&& o) noexcept {
            if (this != &o) { if (active_) End(q_); q_ = o.q_; active_ = o.active_; o.active_ = false; }
            return *this;
        }
    private:
        Queue q_{};
        bool active_{ false };
    };

    // -------------------- Implementation --------------------

    namespace detail {

#if defined(RHI_ENABLE_VULKAN_MARKERS)
        // Cached proc addrs; populated by debug::Init(...)
        inline PFN_vkCmdBeginDebugUtilsLabelEXT  vkCmdBeginDebugUtilsLabelEXT_ = nullptr;
        inline PFN_vkCmdEndDebugUtilsLabelEXT    vkCmdEndDebugUtilsLabelEXT_ = nullptr;
        inline PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT_ = nullptr;
        inline bool vk_ready_ = false;
#endif

    } // namespace detail

    inline bool Init(Device d) noexcept {
        (void)d;
#if defined(RHI_ENABLE_VULKAN_MARKERS)
        // Try to fetch Vulkan proc addrs if we have a Vulkan device behind this RHI device.
        VulkanDeviceInfo vinfo{};
        if (QueryNativeDevice(d, RHI_IID_VK_DEVICE, &vinfo, sizeof(vinfo)) && vinfo.instance && vinfo.device) {
            auto inst = reinterpret_cast<VkInstance>(vinfo.instance);
            auto dev = reinterpret_cast<VkDevice>(vinfo.device);

            auto vkGetInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                vkGetInstanceProcAddr);
            auto vkGetDeviceProcAddr_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                vkGetDeviceProcAddr);

            if (vkGetInstanceProcAddr_ && vkGetDeviceProcAddr_) {
                detail::vkCmdBeginDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr_(inst, "vkCmdBeginDebugUtilsLabelEXT"));
                detail::vkCmdEndDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr_(inst, "vkCmdEndDebugUtilsLabelEXT"));
                detail::vkCmdInsertDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
                        vkGetInstanceProcAddr_(inst, "vkCmdInsertDebugUtilsLabelEXT"));

                // Some loaders require device proc addrs; try those as fallback
                if (!detail::vkCmdBeginDebugUtilsLabelEXT_)
                    detail::vkCmdBeginDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                        vkGetDeviceProcAddr_(dev, "vkCmdBeginDebugUtilsLabelEXT"));
                if (!detail::vkCmdEndDebugUtilsLabelEXT_)
                    detail::vkCmdEndDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                        vkGetDeviceProcAddr_(dev, "vkCmdEndDebugUtilsLabelEXT"));
                if (!detail::vkCmdInsertDebugUtilsLabelEXT_)
                    detail::vkCmdInsertDebugUtilsLabelEXT_ =
                    reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
                        vkGetDeviceProcAddr_(dev, "vkCmdInsertDebugUtilsLabelEXT"));

                detail::vk_ready_ =
                    detail::vkCmdBeginDebugUtilsLabelEXT_ &&
                    detail::vkCmdEndDebugUtilsLabelEXT_ &&
                    detail::vkCmdInsertDebugUtilsLabelEXT_;
            }
        }
#endif
        return true;
    }

    inline void Shutdown(Device /*d*/) noexcept {
        // Nothing to do; debuggers hook globally and Vulkan function pointers can stay cached.
    }

    // ---------------- D3D12 (PIX) command list ----------------
    inline void Begin(CommandList cmd, Color color, const char* name) noexcept {
#if RHI_ENABLE_PIX
        if (auto* cl = rhi::dx12::get_cmd_list(cmd)) { PIXBeginEvent(cl, color, name ? name : ""); return; }
#endif
#if defined(RHI_ENABLE_VULKAN_MARKERS)
        if (detail::vk_ready_) {
            VulkanCmdBufInfo info{};
            if (QueryNativeCmdList(cmd, RHI_IID_VK_COMMAND_BUFFER, &info, sizeof(info)) && info.commandBuffer) {
                VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
                label.pLabelName = name ? name : "";
                float c[4]; toRGBAf(color, c); label.color[0] = c[0]; label.color[1] = c[1]; label.color[2] = c[2]; label.color[3] = c[3];
                detail::vkCmdBeginDebugUtilsLabelEXT_(reinterpret_cast<VkCommandBuffer>(info.commandBuffer), &label);
                return;
            }
        }
#endif
        (void)cmd; (void)color; (void)name; // no-op
    }

    inline void End(CommandList cmd) noexcept {
#if RHI_ENABLE_PIX
        if (auto* cl = rhi::dx12::get_cmd_list(cmd)) { PIXEndEvent(cl); return; }
#endif
#if defined(RHI_ENABLE_VULKAN_MARKERS)
        if (detail::vk_ready_) {
            VulkanCmdBufInfo info{};
            if (QueryNativeCmdList(cmd, RHI_IID_VK_COMMAND_BUFFER, &info, sizeof(info)) && info.commandBuffer) {
                detail::vkCmdEndDebugUtilsLabelEXT_(reinterpret_cast<VkCommandBuffer>(info.commandBuffer));
                return;
            }
        }
#endif
        (void)cmd; // no-op
    }

    inline void Marker(CommandList cmd, Color color, const char* name) noexcept {
#if RHI_ENABLE_PIX
        if (auto* cl = rhi::dx12::get_cmd_list(cmd)) { PIXSetMarker(cl, color, name ? name : ""); return; }
#endif
#if defined(RHI_ENABLE_VULKAN_MARKERS)
        if (detail::vk_ready_) {
            VulkanCmdBufInfo info{};
            if (QueryNativeCmdList(cmd, RHI_IID_VK_COMMAND_BUFFER, &info, sizeof(info)) && info.commandBuffer) {
                VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
                label.pLabelName = name ? name : "";
                float c[4]; toRGBAf(color, c); label.color[0] = c[0]; label.color[1] = c[1]; label.color[2] = c[2]; label.color[3] = c[3];
                detail::vkCmdInsertDebugUtilsLabelEXT_(reinterpret_cast<VkCommandBuffer>(info.commandBuffer), &label);
                return;
            }
        }
#endif
        (void)cmd; (void)color; (void)name; // no-op
    }

    // ---------------- D3D12 (PIX) queue ----------------
    inline void Begin(Queue q, Color color, const char* name) noexcept {
#if RHI_ENABLE_PIX
        if (auto* dq = rhi::dx12::get_queue(q)) { PIXBeginEvent(dq, color, name ? name : ""); return; }
#endif
		(void)q; (void)color; (void)name; // TODO: Vulkan queue markers?
    }

    inline void End(Queue q) noexcept {
#if RHI_ENABLE_PIX
        if (auto* dq = rhi::dx12::get_queue(q)) { PIXEndEvent(dq); return; }
#endif
        (void)q;
    }

    inline void Marker(Queue q, Color color, const char* name) noexcept {
#if RHI_ENABLE_PIX
        if (auto* dq = rhi::dx12::get_queue(q)) { PIXSetMarker(dq, color, name ? name : ""); return; }
#endif
        (void)q; (void)color; (void)name;
    }

} // namespace rhi::debug
