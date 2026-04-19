#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "rhi_debug.h"

namespace rhi::debug {

    enum class InstrumentationExecutionDetailKind : uint8_t {
        Unknown,
        DescriptorMismatch,
        ResourceIndexOutOfBounds,
    };

    struct InstrumentationExecutionTracebackSnapshot {
        bool valid = false;
        uint32_t executionFlag = 0;
        uint32_t rollingExecutionUid = 0;
        uint32_t queueUid = 0;
        std::array<uint32_t, 5> markerHashes32{};
        std::array<uint32_t, 3> kernelLaunch{};
        std::array<uint32_t, 3> thread{};
        std::string hostStackTrace;
    };

    struct InstrumentationDescriptorMismatchSnapshot {
        bool hasDetail = false;
        uint32_t token = 0;
        uint32_t compileType = 0;
        uint32_t runtimeType = 0;
        bool isUndefined = false;
        bool isOutOfBounds = false;
        bool isTableNotBound = false;
    };

    struct InstrumentationResourceBoundsSnapshot {
        bool hasDetail = false;
        uint32_t token = 0;
        std::array<uint32_t, 3> coordinate{};
        bool isTexture = false;
        bool isWrite = false;
    };

    struct InstrumentationExecutionDetailSnapshot {
        uint64_t detailId = 0;
        uint64_t pipelineUid = 0;
        uint64_t shaderUid = 0;
        uint64_t sguid = 0;
        DebugInstrumentationDiagnosticSeverity severity = DebugInstrumentationDiagnosticSeverity::Info;
        InstrumentationExecutionDetailKind kind = InstrumentationExecutionDetailKind::Unknown;
        std::string pipelineLabel;
        std::string sourcePath;
        uint32_t sourceLine = 0;
        uint32_t sourceColumn = 0;
        std::string sourceText;
        std::string sourceContents;
        std::string message;
        InstrumentationExecutionTracebackSnapshot traceback;
        InstrumentationDescriptorMismatchSnapshot descriptorMismatch;
        InstrumentationResourceBoundsSnapshot resourceBounds;
    };

    std::vector<InstrumentationExecutionDetailSnapshot> GetInstrumentationExecutionDetails(
        Device device,
        const DebugInstrumentationIssue& issue);

    bool RetainInstrumentationExecutionDetail(
        Device device,
        uint64_t detailId);

}