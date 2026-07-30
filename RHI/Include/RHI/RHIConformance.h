#pragma once

/**
 * @file RHIConformance.h
 * @brief Backend-neutral RHI conformance case catalog and report contract.
 */

#include "RHI/RHICapabilities.h"
#include "RHI/RHIValidationMessageSink.h"

#include <string>
#include <vector>

namespace RVX
{
    inline constexpr const char* RVX_RHI_CONFORMANCE_REPORT_SCHEMA_ID =
        "RVX.RHI.ConformanceReport";
    inline constexpr uint32 RVX_RHI_CONFORMANCE_REPORT_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_RHI_BASE_CONFORMANCE_CATALOG_ID =
        "RVX.RHI.BaseConformance";
    inline constexpr uint32 RVX_RHI_BASE_CONFORMANCE_CATALOG_VERSION = 1;
    enum class RHIConformanceCaseId : uint8
    {
        ResourceAndViews = 0,
        DescriptorSnapshot = 1,
        GraphicsPipelineInput = 2,
        ComputeUAVToSRV = 3,
        CopyAndReadback = 4,
        ScopedDependency = 5,
        IndirectArguments = 6,
        QueueAndFence = 7,
        Query = 8,
        SurfaceLifecycle = 9,
        DeferredDestruction = 10,
        Count = 11,
    };

    enum class RHIConformanceRequirement : uint8
    {
        Required = 0,
        CapabilityDependent = 1,
        Optional = 2,
    };

    enum class RHIConformanceOutcome : uint8
    {
        NotRun = 0,
        Passed = 1,
        Failed = 2,
        Unsupported = 3,
        EnvironmentUnavailable = 4,
    };

    enum class RHIConformanceReasonCode : uint8
    {
        None = 0,
        NotExecuted = 1,
        CapabilityUnsupported = 2,
        EnvironmentUnavailable = 3,
        RequestedBackendNotRealized = 4,
        CapabilityReportMismatch = 5,
        RequiredCaseUnsupported = 6,
        ContractViolation = 7,
        NativeValidationMessage = 8,
        Timeout = 9,
        NativeFailure = 10,
    };

    enum class RHIConformanceAdapterType : uint8
    {
        Unknown = 0,
        Hardware = 1,
        Software = 2,
    };

    struct RHIConformanceCaseDefinition
    {
        RHIConformanceCaseId id = RHIConformanceCaseId::ResourceAndViews;
        RHIConformanceRequirement requirement = RHIConformanceRequirement::Required;
        bool hasCapabilityGate = false;
        RHICapabilityFeature capabilityFeature = RHICapabilityFeature::ComputePipeline;
    };

    struct RHIConformanceCaseResult
    {
        RHIConformanceCaseId id = RHIConformanceCaseId::ResourceAndViews;
        RHIConformanceOutcome outcome = RHIConformanceOutcome::NotRun;
        uint64 durationMicroseconds = 0;
        RHIConformanceReasonCode reasonCode = RHIConformanceReasonCode::NotExecuted;
        std::string diagnosticMessage;
    };

    struct RHIConformanceReportDesc
    {
        std::string sourceCommit;
        std::string platform;
        RHIBackendType requestedBackend = RHIBackendType::None;
        RHIBackendType realizedBackend = RHIBackendType::None;
        RHIConformanceAdapterType adapterType =
            RHIConformanceAdapterType::Unknown;
        bool environmentAvailable = true;
        std::string environmentReason;
        bool validationRequested = false;
        bool validationEnabled = false;
        bool surfaceRequested = false;
        bool surfaceRealized = false;
        int32 processExitCode = 0;
        RHICapabilityReport capabilityReport;
        std::vector<RHIConformanceCaseResult> caseResults;
        RHIConformanceValidationSnapshot validationSnapshot;
    };

    struct RHIConformanceReport
    {
        uint32 schemaVersion = RVX_RHI_CONFORMANCE_REPORT_SCHEMA_VERSION;
        std::string catalogId = RVX_RHI_BASE_CONFORMANCE_CATALOG_ID;
        uint32 catalogVersion = RVX_RHI_BASE_CONFORMANCE_CATALOG_VERSION;
        std::string sourceCommit;
        std::string platform;
        RHIBackendType requestedBackend = RHIBackendType::None;
        RHIBackendType realizedBackend = RHIBackendType::None;
        RHIConformanceAdapterType adapterType =
            RHIConformanceAdapterType::Unknown;
        bool environmentAvailable = true;
        std::string environmentReason;
        bool validationRequested = false;
        bool validationEnabled = false;
        bool surfaceRequested = false;
        bool surfaceRealized = false;
        int32 processExitCode = 0;
        RHICapabilityReport capabilityReport;
        bool validationPassed = true;
        std::vector<std::string> validationDiagnostics;
        RHIConformanceOutcome outcome = RHIConformanceOutcome::NotRun;
        std::vector<RHIConformanceCaseResult> caseResults;
        std::string validationEvaluationDate;
        std::vector<RHIConformanceValidationMessage> validationMessages;
        uint32 passedCount = 0;
        uint32 failedCount = 0;
        uint32 unsupportedCount = 0;
        uint32 environmentUnavailableCount = 0;
        uint32 notRunCount = 0;
        uint32 validationInfoCount = 0;
        uint32 validationWarningCount = 0;
        uint32 validationErrorCount = 0;
        uint32 unexpectedValidationWarningCount = 0;
        uint32 unexpectedValidationErrorCount = 0;
        uint32 droppedValidationMessageCount = 0;
    };

    const std::vector<RHIConformanceCaseDefinition>& GetRHIConformanceCaseCatalog();
    const RHIConformanceCaseDefinition* FindRHIConformanceCaseDefinition(
        RHIConformanceCaseId id);

    const char* GetRHIConformanceCaseIdName(RHIConformanceCaseId id);
    const char* GetRHIConformanceRequirementName(RHIConformanceRequirement requirement);
    const char* GetRHIConformanceOutcomeName(RHIConformanceOutcome outcome);
    const char* GetRHIConformanceReasonCodeName(RHIConformanceReasonCode reasonCode);
    const char* GetRHIConformanceAdapterTypeName(
        RHIConformanceAdapterType adapterType);

    /**
     * @brief Canonicalize case/message order and classify one conformance run.
     */
    RHIConformanceReport BuildRHIConformanceReport(
        const RHIConformanceReportDesc& desc);

    /**
     * @brief Export a deterministic, versioned machine-readable report.
     */
    std::string ExportRHIConformanceReportJson(
        const RHIConformanceReport& report);
} // namespace RVX
