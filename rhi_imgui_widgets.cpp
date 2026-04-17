#include "rhi_imgui_widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rhi_debug.h"

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

        void DrawIssueMessageLine(const DebugInstrumentationIssue& issue) {
            ImGui::TextColored(SeverityColor(issue.severity), "[%s]", SeverityLabel(issue.severity));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", issue.message[0] != '\0' ? issue.message : "Issue reported.");

            if (issue.path[0] != '\0') {
                ImGui::Indent();
                ImGui::TextDisabled("%s", issue.path);
                ImGui::Unindent();
            }
        }

        void DrawIssueTree(const std::vector<DebugInstrumentationIssue>& issues) {
            const std::vector<PipelineIssueTreeNode> tree = BuildIssueTree(issues);
            if (tree.empty()) {
                ImGui::TextDisabled("No pipeline warnings or shader issues were reported.");
                return;
            }

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
                    DrawIssueMessageLine(*pipelineIssue);
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
                        DrawIssueMessageLine(*shaderIssue);
                    }
                    ImGui::TreePop();
                }

                if (pipelineNode.pipelineIssues.empty() && pipelineNode.shaderIssues.empty()) {
                    ImGui::TextDisabled("No detailed issues available.");
                }

                ImGui::TreePop();
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
        const std::vector<DebugInstrumentationIssue> issues = GetInstrumentationIssues(device);
        const std::vector<DebugInstrumentationDiagnostic> diagnostics = GetInstrumentationDiagnostics(device);

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

        if (!state.active) {
            ImGui::TextDisabled("Runtime instrumentation is inactive. Enable the ReShape-backed debug mode at device creation to use feature toggles.");
        }

        ImGui::Separator();

        uint64_t workingMask = state.globalFeatureMask;
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
                    DrawIssueTree(issues);
                }
                ImGui::EndChild();
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

}