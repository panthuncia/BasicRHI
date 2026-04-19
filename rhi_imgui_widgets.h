#pragma once

#include <string>

#include "rhi.h"

namespace rhi::debug {

    class InstrumentationWidget {
    public:
        void Draw(Device device, bool* pOpen = nullptr, const char* title = "GPU Instrumentation");

    private:
        std::string lastStatus_;
        bool lastStatusWasError_ = false;
        bool showIssues_ = false;
        bool showDiagnostics_ = false;
        std::string selectedIssueKey_;
        uint64_t selectedExecutionDetailId_ = 0;
    };

}