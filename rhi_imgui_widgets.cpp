#include "rhi_imgui_widgets.h"

#include <imgui.h>

#if BASICRHI_ENABLE_RESHAPE
#include <Backend/Resource/ResourceToken.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rhi_debug.h"
#include "rhi_debug_internal.h"

namespace rhi::debug {

    namespace {

        const char* SeverityLabel(DebugInstrumentationDiagnosticSeverity severity) noexcept {
            switch (severity) {
            case DebugInstrumentationDiagnosticSeverity::Info:
                return "Info";
            case DebugInstrumentationDiagnosticSeverity::Warning:
                return "Warning";
            case DebugInstrumentationDiagnosticSeverity::Error:
                return "Error";
            default:
                return "Unknown";
            }
        }

        ImVec4 SeverityColor(DebugInstrumentationDiagnosticSeverity severity) noexcept {
            switch (severity) {
            case DebugInstrumentationDiagnosticSeverity::Info:
                return ImVec4(0.60f, 0.75f, 0.95f, 1.0f);
            case DebugInstrumentationDiagnosticSeverity::Warning:
                return ImVec4(0.95f, 0.78f, 0.33f, 1.0f);
            case DebugInstrumentationDiagnosticSeverity::Error:
                return ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
            default:
                return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        const char* BoolLabel(bool value) noexcept {
            return value ? "Yes" : "No";
        }

        std::string FormatActionResult(const char* action, Result result) {
            std::string message = action;
            message += ": ";
            message += ResultName(result);
            return message;
        }

        int SeverityRank(DebugInstrumentationDiagnosticSeverity severity) noexcept {
            switch (severity) {
            case DebugInstrumentationDiagnosticSeverity::Error:
                return 2;
            case DebugInstrumentationDiagnosticSeverity::Warning:
                return 1;
            case DebugInstrumentationDiagnosticSeverity::Info:
            default:
                return 0;
            }
        }

        DebugInstrumentationDiagnosticSeverity MaxSeverity(
            DebugInstrumentationDiagnosticSeverity lhs,
            DebugInstrumentationDiagnosticSeverity rhs) noexcept {
            return SeverityRank(lhs) >= SeverityRank(rhs) ? lhs : rhs;
        }

        std::string_view ShaderFilename(std::string_view path) noexcept {
            const size_t split = path.find_last_of("\\/");
            if (split == std::string_view::npos) {
                return path;
            }
            return path.substr(split + 1);
        }

        std::string DefaultPipelineLabel(uint64_t pipelineUid) {
            char label[64] = {};
            std::snprintf(label, sizeof(label), "Pipeline %llu", static_cast<unsigned long long>(pipelineUid));
            return label;
        }

        const char* PipelineKindLabel(DebugInstrumentationPipelineKind kind) noexcept {
            switch (kind) {
            case DebugInstrumentationPipelineKind::Graphics:
                return "Graphics";
            case DebugInstrumentationPipelineKind::Compute:
                return "Compute";
            case DebugInstrumentationPipelineKind::StateObject:
                return "StateObject";
            default:
                return "Unknown";
            }
        }

        std::string PipelineLabel(const DebugInstrumentationPipeline& pipeline) {
            return pipeline.label[0] != '\0'
                ? std::string(pipeline.label)
                : DefaultPipelineLabel(pipeline.pipelineUid);
        }

        std::string NormalizeIssueMessageForDeduplication(std::string_view input) {
            std::string normalized(input);
            constexpr std::string_view kExecutionToken = "execution ";
            size_t searchOffset = 0;
            while ((searchOffset = normalized.find(kExecutionToken, searchOffset)) != std::string::npos) {
                size_t digitOffset = searchOffset + kExecutionToken.size();
                size_t digitEnd = digitOffset;
                while (digitEnd < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[digitEnd])) != 0) {
                    ++digitEnd;
                }
                if (digitEnd > digitOffset) {
                    normalized.replace(digitOffset, digitEnd - digitOffset, "#");
                    searchOffset = digitOffset + 1;
                } else {
                    searchOffset = digitOffset;
                }
            }
            return normalized;
        }

        std::string BuildIssueIdentityKey(const DebugInstrumentationIssue& issue) {
            std::string key;
            key.reserve(512);
            key += std::to_string(static_cast<uint32_t>(issue.severity));
            key += '|';
            key += std::to_string(static_cast<uint32_t>(issue.type));
            key += '|';
            key += std::to_string(issue.objectUid);
            key += '|';
            key += issue.label;
            key += '|';
            key += issue.path;
            key += '|';
            key += NormalizeIssueMessageForDeduplication(issue.message);
            return key;
        }

        const DebugInstrumentationIssue* FindIssueByIdentityKey(const std::vector<DebugInstrumentationIssue>& issues, const std::string& selectedIssueKey) {
            auto it = std::find_if(
                issues.begin(),
                issues.end(),
                [&](const DebugInstrumentationIssue& issue) {
                    return BuildIssueIdentityKey(issue) == selectedIssueKey;
                });
            return it != issues.end() ? &(*it) : nullptr;
        }
        const char* ExecutionKindLabel(InstrumentationExecutionDetailKind kind) noexcept {
            switch (kind) {
            case InstrumentationExecutionDetailKind::DescriptorMismatch:
                return "Descriptor mismatch";
            case InstrumentationExecutionDetailKind::ResourceIndexOutOfBounds:
                return "Resource bounds";
            default:
                return "Unknown";
            }
        }

        const char* DescriptorTypeLabel(uint32_t type) noexcept {
            static constexpr const char* kTypeNames[] = { "Texture", "Buffer", "CBuffer", "Sampler" };
            return type < std::size(kTypeNames) ? kTypeNames[type] : "Unknown";
        }

        #if BASICRHI_ENABLE_RESHAPE
        const char* ResourceTokenTypeLabel(::Backend::IL::ResourceTokenType type) noexcept {
            switch (type) {
            case ::Backend::IL::ResourceTokenType::Texture:
                return "Texture";
            case ::Backend::IL::ResourceTokenType::Buffer:
                return "Buffer";
            case ::Backend::IL::ResourceTokenType::CBuffer:
                return "CBuffer";
            case ::Backend::IL::ResourceTokenType::Sampler:
                return "Sampler";
            default:
                return "Unknown";
            }
        }
        #endif

        std::string FormatPackedToken(uint32_t packedToken) {
        #if BASICRHI_ENABLE_RESHAPE
            ResourceToken token{};
            token.packedToken = packedToken;

            char buffer[128] = {};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "0x%08X (%s, PUID %u)",
                packedToken,
                ResourceTokenTypeLabel(token.GetType()),
                token.puid);
            return buffer;
        #else
            char buffer[32] = {};
            std::snprintf(buffer, sizeof(buffer), "0x%08X", packedToken);
            return buffer;
        #endif
        }

        struct ShaderIssueTreeNode {
            std::string key;
            std::string displayName;
            std::string fullPath;
            DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
            std::vector<const DebugInstrumentationIssue*> issues;
        };

        struct PipelineIssueTreeNode {
            uint64_t pipelineUid = 0;
            std::string label;
            DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
            std::vector<const DebugInstrumentationIssue*> pipelineIssues;
            std::vector<ShaderIssueTreeNode> shaderIssues;
        };

        struct TechniquePipelineTreeNode {
            std::string label;
            std::string fullPath;
            std::vector<TechniquePipelineTreeNode> children;
            std::vector<size_t> pipelineIndices;
        };

        TechniquePipelineTreeNode& EnsureTechniquePipelineNode(TechniquePipelineTreeNode& root, std::string_view techniquePath) {
            TechniquePipelineTreeNode* node = &root;
            size_t offset = 0;
            while (offset <= techniquePath.size()) {
                const size_t separator = techniquePath.find("::", offset);
                const size_t tokenEnd = separator == std::string_view::npos ? techniquePath.size() : separator;
                const std::string_view token = techniquePath.substr(offset, tokenEnd - offset);
                if (!token.empty()) {
                    auto childIt = std::find_if(
                        node->children.begin(),
                        node->children.end(),
                        [&](const TechniquePipelineTreeNode& child) {
                            return child.label == token;
                        });
                    if (childIt == node->children.end()) {
                        TechniquePipelineTreeNode child{};
                        child.label.assign(token.data(), token.size());
                        child.fullPath = node->fullPath.empty()
                            ? child.label
                            : node->fullPath + "::" + child.label;
                        node->children.push_back(std::move(child));
                        childIt = std::prev(node->children.end());
                    }
                    node = &(*childIt);
                }

                if (separator == std::string_view::npos) {
                    break;
                }
                offset = separator + 2;
            }
            return *node;
        }

        void SortTechniquePipelineTree(TechniquePipelineTreeNode& node, const std::vector<DebugInstrumentationPipeline>& pipelines) {
            std::sort(
                node.children.begin(),
                node.children.end(),
                [](const TechniquePipelineTreeNode& lhs, const TechniquePipelineTreeNode& rhs) {
                    return lhs.label < rhs.label;
                });
            std::sort(
                node.pipelineIndices.begin(),
                node.pipelineIndices.end(),
                [&](size_t lhs, size_t rhs) {
                    return PipelineLabel(pipelines[lhs]) < PipelineLabel(pipelines[rhs]);
                });
            for (TechniquePipelineTreeNode& child : node.children) {
                SortTechniquePipelineTree(child, pipelines);
            }
        }

        TechniquePipelineTreeNode BuildTechniquePipelineTree(
            const std::vector<DebugInstrumentationPipeline>& pipelines,
            const std::vector<DebugInstrumentationPipelineUsage>& usages,
            std::vector<size_t>& unobservedPipelineIndices) {
            TechniquePipelineTreeNode root{};
            std::unordered_map<uint64_t, size_t> pipelineIndexByUid;
            for (size_t pipelineIndex = 0; pipelineIndex < pipelines.size(); ++pipelineIndex) {
                pipelineIndexByUid.emplace(pipelines[pipelineIndex].pipelineUid, pipelineIndex);
            }

            std::vector<bool> observed(pipelines.size(), false);
            for (const DebugInstrumentationPipelineUsage& usage : usages) {
                const auto pipelineIt = pipelineIndexByUid.find(usage.pipelineUid);
                if (pipelineIt == pipelineIndexByUid.end()) {
                    continue;
                }

                const size_t pipelineIndex = pipelineIt->second;
                observed[pipelineIndex] = true;

                TechniquePipelineTreeNode& node = EnsureTechniquePipelineNode(root, usage.techniquePath);
                if (std::find(node.pipelineIndices.begin(), node.pipelineIndices.end(), pipelineIndex) == node.pipelineIndices.end()) {
                    node.pipelineIndices.push_back(pipelineIndex);
                }
            }

            for (size_t pipelineIndex = 0; pipelineIndex < observed.size(); ++pipelineIndex) {
                if (!observed[pipelineIndex]) {
                    unobservedPipelineIndices.push_back(pipelineIndex);
                }
            }

            SortTechniquePipelineTree(root, pipelines);
            std::sort(
                unobservedPipelineIndices.begin(),
                unobservedPipelineIndices.end(),
                [&](size_t lhs, size_t rhs) {
                    return PipelineLabel(pipelines[lhs]) < PipelineLabel(pipelines[rhs]);
                });
            return root;
        }

        bool ApplyPipelineMask(
            Device device,
            DebugInstrumentationPipeline& pipeline,
            uint64_t featureMask,
            std::string& lastStatus,
            bool& lastStatusWasError) {
            const Result result = SetPipelineInstrumentationMask(device, pipeline.pipelineUid, featureMask);
            lastStatus = FormatActionResult(PipelineLabel(pipeline).c_str(), result);
            lastStatusWasError = !IsOk(result);
            if (!IsOk(result)) {
                return false;
            }

            pipeline.explicitlyInstrumented = featureMask != 0;
            pipeline.explicitFeatureMask = featureMask;
            if (featureMask != 0) {
                pipeline.instrumented = true;
            }
            return true;
        }

        void DrawPipelineToggleLine(
            Device device,
            DebugInstrumentationPipeline& pipeline,
            uint64_t enableMask,
            std::string& lastStatus,
            bool& lastStatusWasError) {
            ImGui::PushID(static_cast<int>(pipeline.pipelineUid));
            bool explicitlyEnabled = pipeline.explicitlyInstrumented;
            if (ImGui::Checkbox("##explicit", &explicitlyEnabled)) {
                (void)ApplyPipelineMask(device, pipeline, explicitlyEnabled ? enableMask : 0, lastStatus, lastStatusWasError);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(PipelineLabel(pipeline).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("[%s | active: %s | instrumented: %s]",
                PipelineKindLabel(pipeline.kind),
                BoolLabel(pipeline.active),
                BoolLabel(pipeline.instrumented));
            if (pipeline.lastPassName[0] != '\0') {
                ImGui::Indent();
                ImGui::TextDisabled("Last pass: %s", pipeline.lastPassName);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }

        void DrawTechniquePipelineNode(
            Device device,
            TechniquePipelineTreeNode& node,
            std::vector<DebugInstrumentationPipeline>& pipelines,
            uint64_t enableMask,
            std::string& lastStatus,
            bool& lastStatusWasError) {
            std::vector<size_t> subtreePipelineIndices = node.pipelineIndices;
            std::function<void(const TechniquePipelineTreeNode&)> collectPipelineIndices = [&](const TechniquePipelineTreeNode& current) {
                for (const TechniquePipelineTreeNode& child : current.children) {
                    subtreePipelineIndices.insert(subtreePipelineIndices.end(), child.pipelineIndices.begin(), child.pipelineIndices.end());
                    collectPipelineIndices(child);
                }
            };
            collectPipelineIndices(node);

            std::sort(subtreePipelineIndices.begin(), subtreePipelineIndices.end());
            subtreePipelineIndices.erase(std::unique(subtreePipelineIndices.begin(), subtreePipelineIndices.end()), subtreePipelineIndices.end());

            ImGui::PushID(node.fullPath.c_str());
            ImGui::SetNextItemAllowOverlap();
            const bool open = ImGui::TreeNodeEx(node.label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap);
            if (!subtreePipelineIndices.empty()) {
                size_t enabledPipelineCount = 0;
                for (size_t pipelineIndex : subtreePipelineIndices) {
                    if (pipelines[pipelineIndex].explicitlyInstrumented) {
                        ++enabledPipelineCount;
                    }
                }

                bool subtreeEnabled = enabledPipelineCount == subtreePipelineIndices.size();
                ImGui::SameLine();
                if (ImGui::Checkbox("##technique_toggle", &subtreeEnabled)) {
                    const uint64_t subtreeMask = subtreeEnabled ? enableMask : 0;
                    for (size_t pipelineIndex : subtreePipelineIndices) {
                        (void)ApplyPipelineMask(device, pipelines[pipelineIndex], subtreeMask, lastStatus, lastStatusWasError);
                    }
                }

                ImGui::SameLine();
                ImGui::TextDisabled("%u pipeline(s)", static_cast<unsigned>(subtreePipelineIndices.size()));
            }

            if (open) {
                for (TechniquePipelineTreeNode& child : node.children) {
                    DrawTechniquePipelineNode(device, child, pipelines, enableMask, lastStatus, lastStatusWasError);
                }
                for (size_t pipelineIndex : node.pipelineIndices) {
                    DrawPipelineToggleLine(device, pipelines[pipelineIndex], enableMask, lastStatus, lastStatusWasError);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        std::vector<PipelineIssueTreeNode> BuildIssueTree(const std::vector<DebugInstrumentationIssue>& issues) {
            static constexpr uint64_t kUnattributedPipelineUid = (std::numeric_limits<uint64_t>::max)();

            std::vector<PipelineIssueTreeNode> tree;
            std::unordered_map<uint64_t, size_t> pipelineLookup;

            auto ensurePipelineNode = [&](uint64_t pipelineUid, const char* label) -> PipelineIssueTreeNode& {
                const auto [it, inserted] = pipelineLookup.emplace(pipelineUid, tree.size());
                if (inserted) {
                    PipelineIssueTreeNode node{};
                    node.pipelineUid = pipelineUid;
                    if (pipelineUid == kUnattributedPipelineUid) {
                        node.label = "Unattributed Shader Issues";
                    } else if (label && label[0] != '\0') {
                        node.label = label;
                    } else {
                        node.label = DefaultPipelineLabel(pipelineUid);
                    }
                    tree.push_back(std::move(node));
                } else if (label && label[0] != '\0') {
                    PipelineIssueTreeNode& node = tree[it->second];
                    if (node.label.empty() || node.label == DefaultPipelineLabel(pipelineUid)) {
                        node.label = label;
                    }
                }
                return tree[it->second];
            };

            for (const DebugInstrumentationIssue& issue : issues) {
                if (issue.type == DebugInstrumentationIssueType::Pipeline) {
                    PipelineIssueTreeNode& node = ensurePipelineNode(issue.objectUid, issue.label);
                    node.pipelineIssues.push_back(&issue);
                    node.severity = MaxSeverity(node.severity, issue.severity);
                    continue;
                }

                if (issue.type != DebugInstrumentationIssueType::ShaderFile) {
                    continue;
                }

                const uint64_t pipelineUid = issue.parentPipelineUid != 0 ? issue.parentPipelineUid : kUnattributedPipelineUid;
                PipelineIssueTreeNode& node = ensurePipelineNode(pipelineUid, nullptr);
                node.severity = MaxSeverity(node.severity, issue.severity);

                std::string shaderKey;
                std::string shaderDisplayName;
                std::string shaderFullPath;
                if (issue.path[0] != '\0') {
                    shaderFullPath = issue.path;
                    shaderDisplayName = std::string(ShaderFilename(shaderFullPath));
                    shaderKey = shaderFullPath;
                } else if (issue.label[0] != '\0') {
                    shaderDisplayName = issue.label;
                    shaderKey = issue.label;
                } else {
                    char fallback[64] = {};
                    std::snprintf(fallback, sizeof(fallback), "Shader %llu", static_cast<unsigned long long>(issue.objectUid));
                    shaderDisplayName = fallback;
                    shaderKey = fallback;
                }

                auto shaderIt = std::find_if(
                    node.shaderIssues.begin(),
                    node.shaderIssues.end(),
                    [&](const ShaderIssueTreeNode& shaderNode) {
                        return shaderNode.key == shaderKey;
                    });
                if (shaderIt == node.shaderIssues.end()) {
                    ShaderIssueTreeNode shaderNode{};
                    shaderNode.key = shaderKey;
                    shaderNode.displayName = shaderDisplayName;
                    shaderNode.fullPath = shaderFullPath;
                    shaderNode.severity = issue.severity;
                    shaderNode.issues.push_back(&issue);
                    node.shaderIssues.push_back(std::move(shaderNode));
                } else {
                    shaderIt->severity = MaxSeverity(shaderIt->severity, issue.severity);
                    shaderIt->issues.push_back(&issue);
                }
            }

            return tree;
        }

        bool DrawIssueMessageLine(const DebugInstrumentationIssue& issue, std::string& selectedIssueKey) {
            const std::string issueKey = BuildIssueIdentityKey(issue);
            const bool selected = selectedIssueKey == issueKey;
            std::string label = "[";
            label += SeverityLabel(issue.severity);
            label += "] ";
            label += issue.message[0] != '\0' ? issue.message : "Issue reported.";

            ImGui::PushID(issueKey.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(issue.severity));
            const bool changed = ImGui::Selectable(label.c_str(), selected);
            ImGui::PopStyleColor();
            if (changed) {
                selectedIssueKey = issueKey;
            }

            if (issue.path[0] != '\0') {
                ImGui::Indent();
                ImGui::TextDisabled("%s", issue.path);
                ImGui::Unindent();
            }
            ImGui::PopID();
            return changed;
        }

        bool DrawIssueTree(const std::vector<DebugInstrumentationIssue>& issues, std::string& selectedIssueKey) {
            const std::vector<PipelineIssueTreeNode> tree = BuildIssueTree(issues);
            if (tree.empty()) {
                ImGui::TextDisabled("No pipeline warnings or shader issues were reported.");
                return false;
            }

            bool selectionChanged = false;

            for (const PipelineIssueTreeNode& pipelineNode : tree) {
                const std::string pipelineId = std::string("##pipeline_") + std::to_string(pipelineNode.pipelineUid);
                const ImGuiTreeNodeFlags pipelineFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
                ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(pipelineNode.severity));
                const bool open = ImGui::TreeNodeEx(
                    pipelineId.c_str(),
                    pipelineFlags,
                    "%s",
                    pipelineNode.label.c_str());
                ImGui::PopStyleColor();
                if (!open) {
                    continue;
                }

                for (const DebugInstrumentationIssue* pipelineIssue : pipelineNode.pipelineIssues) {
                    if (!pipelineIssue) {
                        continue;
                    }
                    selectionChanged = DrawIssueMessageLine(*pipelineIssue, selectedIssueKey) || selectionChanged;
                }

                for (size_t shaderIndex = 0; shaderIndex < pipelineNode.shaderIssues.size(); ++shaderIndex) {
                    const ShaderIssueTreeNode& shaderNode = pipelineNode.shaderIssues[shaderIndex];
                    const std::string shaderId = std::string("##shader_")
                        + std::to_string(pipelineNode.pipelineUid)
                        + "_"
                        + std::to_string(shaderIndex);
                    ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(shaderNode.severity));
                    const bool shaderOpen = ImGui::TreeNodeEx(
                        shaderId.c_str(),
                        ImGuiTreeNodeFlags_SpanAvailWidth,
                        "%s",
                        shaderNode.displayName.c_str());
                    ImGui::PopStyleColor();
                    if (!shaderOpen) {
                        continue;
                    }

                    if (!shaderNode.fullPath.empty()) {
                        ImGui::TextDisabled("%s", shaderNode.fullPath.c_str());
                    }
                    for (const DebugInstrumentationIssue* shaderIssue : shaderNode.issues) {
                        if (!shaderIssue) {
                            continue;
                        }
                        selectionChanged = DrawIssueMessageLine(*shaderIssue, selectedIssueKey) || selectionChanged;
                    }
                    ImGui::TreePop();
                }

                if (pipelineNode.pipelineIssues.empty() && pipelineNode.shaderIssues.empty()) {
                    ImGui::TextDisabled("No detailed issues available.");
                }

                ImGui::TreePop();
            }

            return selectionChanged;
        }

        void DrawExecutionDetailPane(
            Device device,
            const std::vector<DebugInstrumentationIssue>& issues,
            const std::string& selectedIssueKey,
            uint64_t& selectedExecutionDetailId) {
            const DebugInstrumentationIssue* selectedIssue = FindIssueByIdentityKey(issues, selectedIssueKey);
            if (!selectedIssue) {
                ImGui::TextDisabled("Select an issue to inspect captured executions.");
                return;
            }

            std::vector<InstrumentationExecutionDetailSnapshot> matchingDetails = GetInstrumentationExecutionDetails(device, *selectedIssue);

            ImGui::TextColored(SeverityColor(selectedIssue->severity), "[%s] %s", SeverityLabel(selectedIssue->severity), selectedIssue->message);
            if (selectedIssue->path[0] != '\0') {
                ImGui::TextDisabled("%s", selectedIssue->path);
            }
            ImGui::Separator();

            if (matchingDetails.empty()) {
                selectedExecutionDetailId = 0;
                ImGui::TextDisabled("No per-execution values were captured for this issue.");
                return;
            }

            auto selectedIt = std::find_if(
                matchingDetails.begin(),
                matchingDetails.end(),
                [&](const InstrumentationExecutionDetailSnapshot& detail) {
                    return detail.detailId == selectedExecutionDetailId;
                });
            if (selectedIt == matchingDetails.end()) {
                selectedExecutionDetailId = matchingDetails.front().detailId;
                selectedIt = matchingDetails.begin();
            }

            ImGui::Text("Executions: %u", static_cast<unsigned>(matchingDetails.size()));
            if (ImGui::BeginChild("InstrumentationExecutionList", ImVec2(0.0f, 140.0f), true)) {
                for (const InstrumentationExecutionDetailSnapshot& detail : matchingDetails) {
                    char label[256] = {};
                    if (detail.traceback.valid && detail.traceback.rollingExecutionUid != 0) {
                        std::snprintf(
                            label,
                            sizeof(label),
                            "Execution %u | Event %llu | T(%u,%u,%u) | %s | %s",
                            detail.traceback.rollingExecutionUid,
                            static_cast<unsigned long long>(detail.detailId),
                            detail.traceback.thread[0],
                            detail.traceback.thread[1],
                            detail.traceback.thread[2],
                            ExecutionKindLabel(detail.kind),
                            detail.pipelineLabel.c_str());
                    } else {
                        std::snprintf(
                            label,
                            sizeof(label),
                            "Event %llu | %s | %s",
                            static_cast<unsigned long long>(detail.detailId),
                            ExecutionKindLabel(detail.kind),
                            detail.pipelineLabel.c_str());
                    }

                    ImGui::PushID(static_cast<int>(detail.detailId));
                    if (ImGui::Selectable(label, selectedExecutionDetailId == detail.detailId)) {
                        selectedExecutionDetailId = detail.detailId;
                        (void)RetainInstrumentationExecutionDetail(device, detail.detailId);
                        selectedIt = std::find_if(
                            matchingDetails.begin(),
                            matchingDetails.end(),
                            [&](const InstrumentationExecutionDetailSnapshot& candidate) {
                                return candidate.detailId == detail.detailId;
                            });
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            if (selectedIt == matchingDetails.end()) {
                return;
            }

            const InstrumentationExecutionDetailSnapshot& detail = *selectedIt;

            ImGui::Separator();
            if (ImGui::BeginTable("InstrumentationExecutionDetails", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                auto row = [](const char* key, const std::string& value) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(key);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", value.c_str());
                };

                row("Severity", SeverityLabel(detail.severity));
                row("Operation", ExecutionKindLabel(detail.kind));
                row("Pipeline", detail.pipelineLabel.empty() ? DefaultPipelineLabel(detail.pipelineUid) : detail.pipelineLabel);
                if (!detail.sourcePath.empty()) {
                    std::string sourceLocation = detail.sourcePath;
                    char lineColumn[48] = {};
                    std::snprintf(lineColumn, sizeof(lineColumn), " (%u:%u)", detail.sourceLine + 1, detail.sourceColumn + 1);
                    sourceLocation += lineColumn;
                    row("Source", sourceLocation);
                }
                if (detail.traceback.valid) {
                    row("Execution UID", std::to_string(detail.traceback.rollingExecutionUid));
                    row("Execution Flag", std::to_string(detail.traceback.executionFlag));
                    row(
                        "Kernel Launch",
                        std::to_string(detail.traceback.kernelLaunch[0]) + ", "
                        + std::to_string(detail.traceback.kernelLaunch[1]) + ", "
                        + std::to_string(detail.traceback.kernelLaunch[2]));
                    row(
                        "Thread",
                        std::to_string(detail.traceback.thread[0]) + ", "
                        + std::to_string(detail.traceback.thread[1]) + ", "
                        + std::to_string(detail.traceback.thread[2]));
                    row("Queue UID", std::to_string(detail.traceback.queueUid));

                    char markerBuffer[128] = {};
                    std::snprintf(
                        markerBuffer,
                        sizeof(markerBuffer),
                        "%08X %08X %08X %08X %08X",
                        detail.traceback.markerHashes32[0],
                        detail.traceback.markerHashes32[1],
                        detail.traceback.markerHashes32[2],
                        detail.traceback.markerHashes32[3],
                        detail.traceback.markerHashes32[4]);
                    row("Marker Hashes", markerBuffer);
                }

                switch (detail.kind) {
                case InstrumentationExecutionDetailKind::DescriptorMismatch:
                    row("Expected Descriptor", DescriptorTypeLabel(detail.descriptorMismatch.compileType & 0x3u));
                    if (!detail.descriptorMismatch.isUndefined
                        && !detail.descriptorMismatch.isOutOfBounds
                        && !detail.descriptorMismatch.isTableNotBound) {
                        row("Runtime Descriptor", DescriptorTypeLabel(detail.descriptorMismatch.runtimeType & 0x3u));
                    }
                    row("Undefined", detail.descriptorMismatch.isUndefined ? "Yes" : "No");
                    row("Index Out Of Bounds", detail.descriptorMismatch.isOutOfBounds ? "Yes" : "No");
                    row("Table Not Bound", detail.descriptorMismatch.isTableNotBound ? "Yes" : "No");
                    if (detail.descriptorMismatch.hasDetail) {
                        row("Resource Token", FormatPackedToken(detail.descriptorMismatch.token));
                    }
                    break;
                case InstrumentationExecutionDetailKind::ResourceIndexOutOfBounds:
                    row("Resource Kind", detail.resourceBounds.isTexture ? "Texture" : "Buffer");
                    row("Access", detail.resourceBounds.isWrite ? "Write" : "Read");
                    if (detail.resourceBounds.hasDetail) {
                        row("Resource Token", FormatPackedToken(detail.resourceBounds.token));
                        row(
                            "Coordinates",
                            std::to_string(detail.resourceBounds.coordinate[0]) + ", "
                            + std::to_string(detail.resourceBounds.coordinate[1]) + ", "
                            + std::to_string(detail.resourceBounds.coordinate[2]));
                    }
                    break;
                default:
                    break;
                }

                ImGui::EndTable();
            }

            if (!detail.sourceText.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Source Line");
                ImGui::TextWrapped("%s", detail.sourceText.c_str());
            }

            if (!detail.traceback.hostStackTrace.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Host Stack Trace");
                if (ImGui::BeginChild("InstrumentationExecutionStack", ImVec2(0.0f, 120.0f), true)) {
                    ImGui::TextWrapped("%s", detail.traceback.hostStackTrace.c_str());
                }
                ImGui::EndChild();
            }
        }

    }

    void InstrumentationWidget::Draw(Device device, bool* pOpen, const char* title) {
        if (!ImGui::Begin(title, pOpen)) {
            ImGui::End();
            return;
        }

        if (!device) {
            ImGui::TextDisabled("No RHI device available.");
            ImGui::End();
            return;
        }

        device.CheckDebugMessages();

        DebugInstrumentationCapabilities capabilities{};
        DebugInstrumentationState state{};
        const Result capabilitiesResult = GetInstrumentationCapabilities(device, capabilities);
        const Result stateResult = GetInstrumentationState(device, state);

        if (!IsOk(capabilitiesResult) || !IsOk(stateResult)) {
            ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f),
                "Instrumentation state unavailable: caps=%s state=%s",
                ResultName(capabilitiesResult),
                ResultName(stateResult));
            ImGui::End();
            return;
        }

        const std::vector<DebugInstrumentationFeature> features = GetInstrumentationFeatures(device);
        std::vector<DebugInstrumentationPipeline> pipelines = GetInstrumentationPipelines(device);
        const std::vector<DebugInstrumentationPipelineUsage> pipelineUsages = GetInstrumentationPipelineUsages(device);
        std::vector<DebugInstrumentationIssue> issues;
        if (showIssues_ || !selectedIssueKey_.empty() || selectedExecutionDetailId_ != 0) {
            issues = GetInstrumentationIssues(device);
        }
        std::vector<DebugInstrumentationDiagnostic> diagnostics;
        if (showDiagnostics_) {
            diagnostics = GetInstrumentationDiagnostics(device);
        }

        std::vector<size_t> unobservedPipelineIndices;
        TechniquePipelineTreeNode techniquePipelineTree = BuildTechniquePipelineTree(pipelines, pipelineUsages, unobservedPipelineIndices);

        if (!selectedIssueKey_.empty() && !FindIssueByIdentityKey(issues, selectedIssueKey_)) {
            selectedIssueKey_.clear();
            selectedExecutionDetailId_ = 0;
        }

        if (!lastStatus_.empty()) {
            const ImVec4 statusColor = lastStatusWasError_
                ? ImVec4(0.95f, 0.40f, 0.40f, 1.0f)
                : ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
            ImGui::TextColored(statusColor, "%s", lastStatus_.c_str());
            ImGui::Separator();
        }

        ImGui::Text("Requested: %s", BoolLabel(state.requested));
        ImGui::Text("Active: %s", BoolLabel(state.active));
        ImGui::Text("Backend Build Enabled: %s", BoolLabel(capabilities.backendBuildEnabled));
        ImGui::Text("Global Instrumentation Supported: %s", BoolLabel(capabilities.globalInstrumentationSupported));
        ImGui::Text("Feature Count: %u", static_cast<unsigned>(features.size()));

        bool synchronousRecording = state.synchronousRecording;
        const bool allowSynchronousToggle = state.active && capabilities.synchronousRecordingSupported;
        if (!allowSynchronousToggle) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Synchronous Recording", &synchronousRecording)) {
            const Result result = SetSynchronousRecording(device, synchronousRecording);
            lastStatus_ = FormatActionResult("Set synchronous recording", result);
            lastStatusWasError_ = !IsOk(result);
            if (IsOk(result)) {
                state.synchronousRecording = synchronousRecording;
            }
        }
        if (!allowSynchronousToggle) {
            ImGui::EndDisabled();
        }

        bool texelAddressingEnabled = state.texelAddressingEnabled;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Texel Addressing (requires device recreation)", &texelAddressingEnabled);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Texel addressing improves initialization/concurrency accuracy, but adds heavy memory and runtime overhead.");
        ImGui::TextDisabled("This selects the GPU-Reshape initialization/concurrency backend at device creation, so it cannot be changed live from this widget.");

        if (!state.active) {
            ImGui::TextDisabled("Runtime instrumentation is inactive. Enable the ReShape-backed debug mode at device creation to use feature toggles.");
        }

        ImGui::Separator();

        uint64_t workingMask = state.globalFeatureMask;
        const uint64_t pipelineEnableMask = workingMask;
        const bool allowFeatureEditing = state.active && capabilities.globalInstrumentationSupported && !features.empty();

        if (ImGui::BeginTable("InstrumentationLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextColumn();

            if (!allowFeatureEditing) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Enable All Features")) {
                uint64_t fullMask = 0;
                for (const DebugInstrumentationFeature& feature : features) {
                    fullMask |= feature.featureBit;
                }

                const Result result = SetGlobalInstrumentationMask(device, fullMask);
                lastStatus_ = FormatActionResult("Enable all features", result);
                lastStatusWasError_ = !IsOk(result);
                if (IsOk(result)) {
                    workingMask = fullMask;
                    state.globalFeatureMask = fullMask;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable All Features")) {
                const Result result = SetGlobalInstrumentationMask(device, 0);
                lastStatus_ = FormatActionResult("Disable all features", result);
                lastStatusWasError_ = !IsOk(result);
                if (IsOk(result)) {
                    workingMask = 0;
                    state.globalFeatureMask = 0;
                }
            }
            if (!allowFeatureEditing) {
                ImGui::EndDisabled();
            }

            int enabledFeatureCount = 0;
            for (const DebugInstrumentationFeature& feature : features) {
                if (IsInstrumentationFeatureEnabled(workingMask, feature)) {
                    ++enabledFeatureCount;
                }
            }

            ImGui::Text("Enabled Features: %d / %u", enabledFeatureCount, static_cast<unsigned>(features.size()));

            if (ImGui::BeginChild("InstrumentationFeatures", ImVec2(0.0f, 220.0f), true)) {
                if (features.empty()) {
                    ImGui::TextDisabled("No backend features were discovered.");
                }

                for (size_t index = 0; index < features.size(); ++index) {
                    const DebugInstrumentationFeature& feature = features[index];
                    bool enabled = IsInstrumentationFeatureEnabled(workingMask, feature);

                    if (!allowFeatureEditing) {
                        ImGui::BeginDisabled();
                    }

                    std::array<char, 96> checkboxLabel{};
                    std::snprintf(checkboxLabel.data(), checkboxLabel.size(), "%s##feature_%zu", feature.name, index);
                    if (ImGui::Checkbox(checkboxLabel.data(), &enabled)) {
                        const uint64_t updatedMask = UpdateInstrumentationFeatureMask(workingMask, feature, enabled);
                        const Result result = SetGlobalInstrumentationMask(device, updatedMask);
                        lastStatus_ = FormatActionResult(feature.name, result);
                        lastStatusWasError_ = !IsOk(result);
                        if (IsOk(result)) {
                            workingMask = updatedMask;
                            state.globalFeatureMask = updatedMask;
                        }
                    }

                    if (!allowFeatureEditing) {
                        ImGui::EndDisabled();
                    }

                    if (feature.description[0] != '\0') {
                        ImGui::Indent();
                        ImGui::TextWrapped("%s", feature.description);
                        ImGui::Unindent();
                    }
                }
            }
            ImGui::EndChild();

            if (ImGui::CollapsingHeader("Currently Enabled", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool haveEnabledFeature = false;
                for (const DebugInstrumentationFeature& feature : features) {
                    if (!IsInstrumentationFeatureEnabled(workingMask, feature)) {
                        continue;
                    }

                    haveEnabledFeature = true;
                    ImGui::BulletText("%s", feature.name);
                }

                if (!haveEnabledFeature) {
                    ImGui::TextDisabled("No instrumentation features are currently enabled.");
                }
            }

            if (ImGui::CollapsingHeader("Technique And Pipeline Targeting", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Pipelines: %u", static_cast<unsigned>(pipelines.size()));
                ImGui::Text("Technique Bindings: %u", static_cast<unsigned>(pipelineUsages.size()));
                if (pipelineEnableMask == 0) {
                    ImGui::TextDisabled("No instrumentation feature mask is available. Enable at least one feature globally first.");
                }

                if (ImGui::BeginChild("InstrumentationPipelines", ImVec2(0.0f, 260.0f), true)) {
                    if (pipelines.empty()) {
                        ImGui::TextDisabled("No pipeline inventory is available yet.");
                    }

                    for (TechniquePipelineTreeNode& node : techniquePipelineTree.children) {
                        DrawTechniquePipelineNode(device, node, pipelines, pipelineEnableMask, lastStatus_, lastStatusWasError_);
                    }

                    if (!unobservedPipelineIndices.empty()) {
                        if (ImGui::TreeNodeEx("Unobserved Pipelines", ImGuiTreeNodeFlags_SpanAvailWidth)) {
                            for (size_t pipelineIndex : unobservedPipelineIndices) {
                                DrawPipelineToggleLine(device, pipelines[pipelineIndex], pipelineEnableMask, lastStatus_, lastStatusWasError_);
                            }
                            ImGui::TreePop();
                        }
                    }
                }
                ImGui::EndChild();
            }

            ImGui::Separator();
            ImGui::Checkbox("Show Diagnostics", &showDiagnostics_);
            if (showDiagnostics_) {
                if (ImGui::BeginChild("InstrumentationDiagnostics", ImVec2(0.0f, 200.0f), true)) {
                    if (diagnostics.empty()) {
                        ImGui::TextDisabled("No diagnostics available.");
                    }

                    for (const DebugInstrumentationDiagnostic& diagnostic : diagnostics) {
                        ImGui::TextColored(SeverityColor(diagnostic.severity), "[%s]", SeverityLabel(diagnostic.severity));
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", diagnostic.message);
                    }
                }
                ImGui::EndChild();
            }

            ImGui::TableNextColumn();
            ImGui::Checkbox("Show Issues", &showIssues_);
            if (showIssues_) {
                if (ImGui::BeginChild("InstrumentationIssues", ImVec2(0.0f, 0.0f), true)) {
                    if (ImGui::BeginTable("InstrumentationIssueLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableNextColumn();
                        if (ImGui::BeginChild("InstrumentationIssueTree", ImVec2(0.0f, 0.0f), false)) {
                            if (DrawIssueTree(issues, selectedIssueKey_)) {
                                selectedExecutionDetailId_ = 0;
                            }
                        }
                        ImGui::EndChild();

                        ImGui::TableNextColumn();
                        if (ImGui::BeginChild("InstrumentationIssueDetails", ImVec2(0.0f, 0.0f), false)) {
                            DrawExecutionDetailPane(device, issues, selectedIssueKey_, selectedExecutionDetailId_);
                        }
                        ImGui::EndChild();

                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

}