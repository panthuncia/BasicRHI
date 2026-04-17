#include "rhi_imgui_widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>

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
            if (ImGui::BeginChild("InstrumentationDiagnostics", ImVec2(0.0f, 180.0f), true)) {
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

        ImGui::End();
    }

}