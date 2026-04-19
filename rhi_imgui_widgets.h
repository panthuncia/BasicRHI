#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "ImGuiColorTextEdit/TextEditor.h"
#include "rhi.h"

namespace rhi::debug {

    class InstrumentationWidget {
    public:
        struct ShaderSourceViewerState {
            TextEditor editor;
            bool configured = false;
            std::string loadedPath;
            std::string loadedSourceContents;
            uint64_t loadedDetailId = 0;
            uint32_t highlightedLine = 0;
            std::string loadError;
        };

        void Draw(Device device, bool* pOpen = nullptr, const char* title = "GPU Instrumentation");

    private:
        std::string lastStatus_;
        bool lastStatusWasError_ = false;
        bool showIssues_ = false;
        bool showDiagnostics_ = false;
        std::string selectedIssueKey_;
        uint64_t selectedExecutionDetailId_ = 0;
        uint64_t selectedFeatureMask_ = 0;
        bool selectedFeatureMaskInitialized_ = false;
        std::unordered_set<uint64_t> selectedPipelineUids_;
        ShaderSourceViewerState shaderSourceViewer_;
    };

}