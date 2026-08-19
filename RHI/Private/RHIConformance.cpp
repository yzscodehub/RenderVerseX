/**
 * @file RHIConformance.cpp
 * @brief Backend-neutral RHI conformance case catalog and report contract.
 */

#include "RHI/RHIConformance.h"
#include "Core/Diagnostics/JsonWriter.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <tuple>
#include <utility>

namespace RVX
{
    namespace
    {
        using Diagnostics::JsonBool;
        using Diagnostics::JsonString;

        const std::vector<RHIConformanceCaseDefinition> kCaseCatalog = {
            {RHIConformanceCaseId::ResourceAndViews,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::DescriptorSnapshot,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::GraphicsPipelineInput,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::ComputeUAVToSRV,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::CopyAndReadback,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::ScopedDependency,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::IndirectArguments,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::QueueAndFence,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::Query,
             RHIConformanceRequirement::CapabilityDependent,
             true,
             RHICapabilityFeature::QuerySupport},
            {RHIConformanceCaseId::SurfaceLifecycle,
             RHIConformanceRequirement::Required},
            {RHIConformanceCaseId::DeferredDestruction,
             RHIConformanceRequirement::Required},
        };

        constexpr bool IsConcreteBackend(RHIBackendType backend)
        {
            switch (backend)
            {
                case RHIBackendType::DX11:
                case RHIBackendType::DX12:
                case RHIBackendType::Vulkan:
                case RHIBackendType::Metal:
                case RHIBackendType::OpenGL:
                    return true;
                case RHIBackendType::None:
                case RHIBackendType::Auto:
                default:
                    return false;
            }
        }

        constexpr bool IsTier1Backend(RHIBackendType backend)
        {
            return backend == RHIBackendType::DX12 ||
                   backend == RHIBackendType::Vulkan ||
                   backend == RHIBackendType::Metal;
        }

        size_t GetCaseIndex(RHIConformanceCaseId id)
        {
            return static_cast<size_t>(id);
        }

        bool IsValidCaseId(RHIConformanceCaseId id)
        {
            return GetCaseIndex(id) <
                   static_cast<size_t>(RHIConformanceCaseId::Count);
        }

        const RHICapabilityReportEntry* FindCapabilityEntry(
            const RHICapabilityReport& report,
            RHICapabilityFeature feature)
        {
            const auto it = std::find_if(
                report.entries.begin(),
                report.entries.end(),
                [feature](const RHICapabilityReportEntry& entry)
                {
                    return entry.feature == feature;
                });
            return it != report.entries.end() ? &(*it) : nullptr;
        }

        void AddValidationDiagnostic(RHIConformanceReport& report,
                                     std::string diagnostic)
        {
            report.validationPassed = false;
            report.validationDiagnostics.push_back(std::move(diagnostic));
        }

        void ValidateCapabilityReport(RHIConformanceReport& report)
        {
            const RHICapabilityReport& capabilities = report.capabilityReport;
            if (capabilities.schemaVersion !=
                RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION)
            {
                AddValidationDiagnostic(
                    report,
                    "Capability report schema version does not match the public RHI contract.");
            }
            if (capabilities.backendType != report.realizedBackend)
            {
                AddValidationDiagnostic(
                    report,
                    "Capability report backend does not match the realized backend.");
            }
            if (!capabilities.validationPassed)
            {
                AddValidationDiagnostic(
                    report,
                    "Capability report failed its public consistency validation.");
            }
            if (capabilities.adapterName.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "A realized backend must publish a non-empty adapter identity.");
            }
            if (capabilities.driverVersion.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "A realized backend must publish a non-empty driver identity.");
            }
            if (capabilities.renderGraphBaselineSupported !=
                capabilities.renderGraphBaselineMissingRequirements.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "Capability report RenderGraph baseline verdict is inconsistent.");
            }

            uint32 supportedCount = 0;
            uint32 emulatedCount = 0;
            uint32 unsupportedCount = 0;
            std::array<bool, 256> seenFeatures{};
            for (const RHICapabilityReportEntry& entry : capabilities.entries)
            {
                const size_t featureIndex = static_cast<size_t>(entry.feature);
                if (seenFeatures[featureIndex])
                {
                    AddValidationDiagnostic(
                        report,
                        std::string("Capability report contains duplicate feature ") +
                            GetRHICapabilityFeatureName(entry.feature) + ".");
                }
                seenFeatures[featureIndex] = true;

                switch (entry.status)
                {
                    case RHICapabilityStatus::Supported:
                        ++supportedCount;
                        if (!entry.supported || entry.emulated)
                        {
                            AddValidationDiagnostic(
                                report,
                                "Supported capability entry has inconsistent flags.");
                        }
                        break;
                    case RHICapabilityStatus::Emulated:
                        ++emulatedCount;
                        if (!entry.supported || !entry.emulated)
                        {
                            AddValidationDiagnostic(
                                report,
                                "Emulated capability entry has inconsistent flags.");
                        }
                        break;
                    case RHICapabilityStatus::Unsupported:
                    default:
                        ++unsupportedCount;
                        if (entry.supported || entry.emulated)
                        {
                            AddValidationDiagnostic(
                                report,
                                "Unsupported capability entry has inconsistent flags.");
                        }
                        break;
                }
            }

            if (supportedCount != capabilities.supportedCount ||
                emulatedCount != capabilities.emulatedCount ||
                unsupportedCount != capabilities.unsupportedCount)
            {
                AddValidationDiagnostic(
                    report,
                    "Capability report summary counts do not match its entries.");
            }
        }

        void ValidateCaseResultShape(RHIConformanceReport& report,
                                     const RHIConformanceCaseResult& result)
        {
            const char* caseName = GetRHIConformanceCaseIdName(result.id);
            if (result.outcome == RHIConformanceOutcome::Passed)
            {
                if (result.reasonCode != RHIConformanceReasonCode::None)
                {
                    AddValidationDiagnostic(
                        report,
                        std::string(caseName) +
                            " passed but published a non-empty reason code.");
                }
                return;
            }

            if (result.reasonCode == RHIConformanceReasonCode::None)
            {
                AddValidationDiagnostic(
                    report,
                    std::string(caseName) +
                        " did not pass and must publish a stable reason code.");
            }
            if (result.diagnosticMessage.empty())
            {
                AddValidationDiagnostic(
                    report,
                    std::string(caseName) +
                        " did not pass and must publish a diagnostic message.");
            }
        }

        bool CapabilityGateIsSupported(
            RHIConformanceReport& report,
            const RHIConformanceCaseDefinition& definition)
        {
            const RHICapabilityReportEntry* entry =
                FindCapabilityEntry(report.capabilityReport,
                                    definition.capabilityFeature);
            if (!entry)
            {
                AddValidationDiagnostic(
                    report,
                    std::string(GetRHIConformanceCaseIdName(definition.id)) +
                        " is capability-dependent but its capability entry is missing.");
                return false;
            }
            return entry->supported;
        }

        void CountCaseOutcome(RHIConformanceReport& report,
                              RHIConformanceOutcome outcome)
        {
            switch (outcome)
            {
                case RHIConformanceOutcome::Passed:
                    ++report.passedCount;
                    break;
                case RHIConformanceOutcome::Failed:
                    ++report.failedCount;
                    break;
                case RHIConformanceOutcome::Unsupported:
                    ++report.unsupportedCount;
                    break;
                case RHIConformanceOutcome::EnvironmentUnavailable:
                    ++report.environmentUnavailableCount;
                    break;
                case RHIConformanceOutcome::NotRun:
                default:
                    ++report.notRunCount;
                    break;
            }
        }

        void ValidateValidationMessage(
            RHIConformanceReport& report,
            const RHIConformanceValidationMessage& message)
        {
            if (message.severity !=
                    RHIConformanceValidationSeverity::Info &&
                message.severity !=
                    RHIConformanceValidationSeverity::Warning &&
                message.severity !=
                    RHIConformanceValidationSeverity::Error)
            {
                AddValidationDiagnostic(
                    report,
                    "Validation message has an invalid severity.");
            }

            if (message.allowlisted && message.allowlistEntryId.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "Allowlisted validation message is missing its reviewed entry ID.");
            }
            if (message.allowlisted &&
                message.severity !=
                    RHIConformanceValidationSeverity::Warning)
            {
                AddValidationDiagnostic(
                    report,
                    "Only native validation warnings can be allowlisted.");
            }
            if (!message.allowlisted &&
                !message.allowlistEntryId.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "A non-allowlisted validation message cannot publish an allowlist entry ID.");
            }
            if (message.severity != RHIConformanceValidationSeverity::Info &&
                message.nativeId.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "Native validation warning/error is missing its native ID.");
            }
            if (message.category.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "Native validation message is missing its normalized category.");
            }
            if (message.text.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "Native validation message is missing diagnostic text.");
            }
            if (message.text.size() >
                RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES)
            {
                AddValidationDiagnostic(
                    report,
                    "Native validation message text exceeds the bounded contract.");
            }
            if (message.textTruncated &&
                message.text.size() !=
                    RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES)
            {
                AddValidationDiagnostic(
                    report,
                    "A truncated validation message must retain the full bounded prefix.");
            }
        }

        void ExportCapabilityEntriesJson(std::ostringstream& ss,
                                         const RHICapabilityReport& capabilities)
        {
            ss << "    \"schemaVersion\": " << capabilities.schemaVersion << ",\n";
            ss << "    \"backend\": "
               << JsonString(ToString(capabilities.backendType)) << ",\n";
            ss << "    \"adapterName\": "
               << JsonString(capabilities.adapterName) << ",\n";
            ss << "    \"driverVersion\": "
               << JsonString(capabilities.driverVersion) << ",\n";
            ss << "    \"validationPassed\": "
               << JsonBool(capabilities.validationPassed) << ",\n";
            ss << "    \"validationMessage\": "
               << JsonString(capabilities.validationMessage) << ",\n";
            ss << "    \"queueTopology\": {\n";
            ss << "      \"completionMode\": "
               << JsonString(GetRHIQueueCompletionModeName(
                      capabilities.queueTopology.completionMode))
               << ",\n";
            ss << "      \"logicalQueueDomains\": ["
               << JsonString(GetGPUQueueDomainName(
                      capabilities.queueTopology.logicalQueueDomains[0]))
               << ", "
               << JsonString(GetGPUQueueDomainName(
                      capabilities.queueTopology.logicalQueueDomains[1]))
               << ", "
               << JsonString(GetGPUQueueDomainName(
                      capabilities.queueTopology.logicalQueueDomains[2]))
               << "],\n";
            ss << "      \"activeDomainCount\": "
               << static_cast<uint32>(
                      capabilities.queueTopology.activeDomainCount)
               << "\n";
            ss << "    },\n";
            ss << "    \"summary\": {\n";
            ss << "      \"supportedCount\": "
               << capabilities.supportedCount << ",\n";
            ss << "      \"emulatedCount\": "
               << capabilities.emulatedCount << ",\n";
            ss << "      \"unsupportedCount\": "
               << capabilities.unsupportedCount << "\n";
            ss << "    },\n";
            ss << "    \"renderGraphBaseline\": {\n";
            ss << "      \"supported\": "
               << JsonBool(capabilities.renderGraphBaselineSupported)
               << ",\n";
            ss << "      \"missingRequirements\": [";
            for (size_t i = 0;
                 i <
                 capabilities.renderGraphBaselineMissingRequirements.size();
                 ++i)
            {
                ss << (i == 0 ? "" : ", ")
                   << JsonString(
                          capabilities
                              .renderGraphBaselineMissingRequirements[i]);
            }
            ss << "]\n";
            ss << "    },\n";
            ss << "    \"entries\": [\n";
            for (size_t i = 0; i < capabilities.entries.size(); ++i)
            {
                const RHICapabilityReportEntry& entry = capabilities.entries[i];
                ss << "      {\n";
                ss << "        \"feature\": "
                   << JsonString(GetRHICapabilityFeatureName(entry.feature))
                   << ",\n";
                ss << "        \"status\": "
                   << JsonString(GetRHICapabilityStatusName(entry.status))
                   << ",\n";
                ss << "        \"supported\": "
                   << JsonBool(entry.supported) << ",\n";
                ss << "        \"emulated\": "
                   << JsonBool(entry.emulated) << ",\n";
                ss << "        \"requiredCapability\": "
                   << JsonString(entry.requiredCapability) << ",\n";
                ss << "        \"diagnosticMessage\": "
                   << JsonString(entry.diagnosticMessage) << "\n";
                ss << "      }"
                   << (i + 1 < capabilities.entries.size() ? "," : "")
                   << "\n";
            }
            ss << "    ]\n";
        }
    } // namespace

    const std::vector<RHIConformanceCaseDefinition>&
    GetRHIConformanceCaseCatalog()
    {
        return kCaseCatalog;
    }

    const RHIConformanceCaseDefinition* FindRHIConformanceCaseDefinition(
        RHIConformanceCaseId id)
    {
        const auto it = std::find_if(
            kCaseCatalog.begin(),
            kCaseCatalog.end(),
            [id](const RHIConformanceCaseDefinition& definition)
            {
                return definition.id == id;
            });
        return it != kCaseCatalog.end() ? &(*it) : nullptr;
    }

    const char* GetRHIConformanceCaseIdName(RHIConformanceCaseId id)
    {
        switch (id)
        {
            case RHIConformanceCaseId::ResourceAndViews:
                return "rhi.resource-and-views";
            case RHIConformanceCaseId::DescriptorSnapshot:
                return "rhi.descriptor-snapshot";
            case RHIConformanceCaseId::GraphicsPipelineInput:
                return "rhi.graphics-pipeline-input";
            case RHIConformanceCaseId::ComputeUAVToSRV:
                return "rhi.compute-uav-to-srv";
            case RHIConformanceCaseId::CopyAndReadback:
                return "rhi.copy-and-readback";
            case RHIConformanceCaseId::ScopedDependency:
                return "rhi.scoped-dependency";
            case RHIConformanceCaseId::IndirectArguments:
                return "rhi.indirect-arguments";
            case RHIConformanceCaseId::QueueAndFence:
                return "rhi.queue-and-fence";
            case RHIConformanceCaseId::Query:
                return "rhi.query";
            case RHIConformanceCaseId::SurfaceLifecycle:
                return "rhi.surface-lifecycle";
            case RHIConformanceCaseId::DeferredDestruction:
                return "rhi.deferred-destruction";
            case RHIConformanceCaseId::Count:
            default:
                return "rhi.invalid";
        }
    }

    const char* GetRHIConformanceRequirementName(
        RHIConformanceRequirement requirement)
    {
        switch (requirement)
        {
            case RHIConformanceRequirement::Required:
                return "Required";
            case RHIConformanceRequirement::CapabilityDependent:
                return "CapabilityDependent";
            case RHIConformanceRequirement::Optional:
                return "Optional";
            default:
                return "Unknown";
        }
    }

    const char* GetRHIConformanceOutcomeName(RHIConformanceOutcome outcome)
    {
        switch (outcome)
        {
            case RHIConformanceOutcome::NotRun:
                return "NotRun";
            case RHIConformanceOutcome::Passed:
                return "Passed";
            case RHIConformanceOutcome::Failed:
                return "Failed";
            case RHIConformanceOutcome::Unsupported:
                return "Unsupported";
            case RHIConformanceOutcome::EnvironmentUnavailable:
                return "EnvironmentUnavailable";
            default:
                return "Unknown";
        }
    }

    const char* GetRHIConformanceReasonCodeName(
        RHIConformanceReasonCode reasonCode)
    {
        switch (reasonCode)
        {
            case RHIConformanceReasonCode::None:
                return "None";
            case RHIConformanceReasonCode::NotExecuted:
                return "NotExecuted";
            case RHIConformanceReasonCode::CapabilityUnsupported:
                return "CapabilityUnsupported";
            case RHIConformanceReasonCode::EnvironmentUnavailable:
                return "EnvironmentUnavailable";
            case RHIConformanceReasonCode::RequestedBackendNotRealized:
                return "RequestedBackendNotRealized";
            case RHIConformanceReasonCode::CapabilityReportMismatch:
                return "CapabilityReportMismatch";
            case RHIConformanceReasonCode::RequiredCaseUnsupported:
                return "RequiredCaseUnsupported";
            case RHIConformanceReasonCode::ContractViolation:
                return "ContractViolation";
            case RHIConformanceReasonCode::NativeValidationMessage:
                return "NativeValidationMessage";
            case RHIConformanceReasonCode::Timeout:
                return "Timeout";
            case RHIConformanceReasonCode::NativeFailure:
                return "NativeFailure";
            default:
                return "Unknown";
        }
    }

    const char* GetRHIConformanceAdapterTypeName(
        RHIConformanceAdapterType adapterType)
    {
        switch (adapterType)
        {
            case RHIConformanceAdapterType::Hardware:
                return "Hardware";
            case RHIConformanceAdapterType::Software:
                return "Software";
            case RHIConformanceAdapterType::Unknown:
            default:
                return "Unknown";
        }
    }

    RHIConformanceReport BuildRHIConformanceReport(
        const RHIConformanceReportDesc& desc)
    {
        RHIConformanceReport report;
        report.sourceCommit = desc.sourceCommit;
        report.platform = desc.platform;
        report.requestedBackend = desc.requestedBackend;
        report.realizedBackend = desc.realizedBackend;
        report.adapterType = desc.adapterType;
        report.environmentAvailable = desc.environmentAvailable;
        report.environmentReason = desc.environmentReason;
        report.validationRequested = desc.validationRequested;
        report.validationEnabled = desc.validationEnabled;
        report.surfaceRequested = desc.surfaceRequested;
        report.surfaceRealized = desc.surfaceRealized;
        report.processExitCode = desc.processExitCode;
        report.capabilityReport = desc.capabilityReport;

        std::sort(
            report.capabilityReport.entries.begin(),
            report.capabilityReport.entries.end(),
            [](const RHICapabilityReportEntry& left,
               const RHICapabilityReportEntry& right)
            {
                return static_cast<uint8>(left.feature) <
                       static_cast<uint8>(right.feature);
            });

        report.validationPassed = true;
        if (desc.sourceCommit.empty())
        {
            AddValidationDiagnostic(
                report,
                "Conformance reports require the exact source commit.");
        }
        if (desc.platform.empty())
        {
            AddValidationDiagnostic(
                report,
                "Conformance reports require a platform identity.");
        }
        if (!IsConcreteBackend(desc.requestedBackend))
        {
            AddValidationDiagnostic(
                report,
                "Conformance reports require a concrete requested backend.");
        }
        if (desc.surfaceRealized && !desc.surfaceRequested)
        {
            AddValidationDiagnostic(
                report,
                "A surface cannot be realized when surface execution was not requested.");
        }

        report.caseResults.reserve(kCaseCatalog.size());
        for (const RHIConformanceCaseDefinition& definition : kCaseCatalog)
        {
            RHIConformanceCaseResult result;
            result.id = definition.id;
            result.outcome = desc.environmentAvailable
                                 ? RHIConformanceOutcome::NotRun
                                 : RHIConformanceOutcome::EnvironmentUnavailable;
            result.reasonCode = desc.environmentAvailable
                                    ? RHIConformanceReasonCode::NotExecuted
                                    : RHIConformanceReasonCode::EnvironmentUnavailable;
            result.diagnosticMessage =
                desc.environmentAvailable
                    ? "Case was not executed."
                    : desc.environmentReason;
            report.caseResults.push_back(std::move(result));
        }

        std::array<bool,
                   static_cast<size_t>(RHIConformanceCaseId::Count)>
            seenCases{};
        if (desc.environmentAvailable)
        {
            for (const RHIConformanceCaseResult& sourceResult :
                 desc.caseResults)
            {
                if (!IsValidCaseId(sourceResult.id))
                {
                    AddValidationDiagnostic(
                        report,
                        "Case results contain an invalid case ID.");
                    continue;
                }

                const size_t caseIndex = GetCaseIndex(sourceResult.id);
                if (seenCases[caseIndex])
                {
                    AddValidationDiagnostic(
                        report,
                        std::string("Case results contain duplicate ID ") +
                            GetRHIConformanceCaseIdName(sourceResult.id) + ".");
                    continue;
                }
                seenCases[caseIndex] = true;
                report.caseResults[caseIndex] = sourceResult;
            }
        }

        if (!desc.environmentAvailable)
        {
            if (!desc.caseResults.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "EnvironmentUnavailable reports must not publish executed case results.");
            }
            if (desc.environmentReason.empty())
            {
                AddValidationDiagnostic(
                    report,
                    "EnvironmentUnavailable reports require a reason.");
            }
            if (desc.realizedBackend != RHIBackendType::None)
            {
                AddValidationDiagnostic(
                    report,
                    "An unavailable backend environment cannot publish a realized backend.");
            }
        }
        else
        {
            if (desc.processExitCode != 0)
            {
                AddValidationDiagnostic(
                    report,
                    "An available conformance run cannot pass with a non-zero process exit code.");
            }
            if (desc.adapterType == RHIConformanceAdapterType::Unknown)
            {
                AddValidationDiagnostic(
                    report,
                    "An available conformance run must classify its adapter as hardware or software.");
            }
            if (desc.validationRequested && !desc.validationEnabled)
            {
                AddValidationDiagnostic(
                    report,
                    "Native validation was requested but was not enabled.");
            }
            if (!IsConcreteBackend(desc.realizedBackend))
            {
                AddValidationDiagnostic(
                    report,
                    "An available environment must publish a concrete realized backend.");
            }
            else if (desc.realizedBackend != desc.requestedBackend)
            {
                AddValidationDiagnostic(
                    report,
                    std::string("Requested backend ") +
                        ToString(desc.requestedBackend) +
                        " was not realized; actual backend is " +
                        ToString(desc.realizedBackend) + ".");
            }
            ValidateCapabilityReport(report);
        }

        const RHIConformanceValidationSnapshot& validationSnapshot =
            desc.validationSnapshot;
        if (desc.validationEnabled &&
            validationSnapshot.evaluationDate.empty())
        {
            AddValidationDiagnostic(
                report,
                "Enabled native validation requires an allowlist evaluation date.");
        }
        if (!validationSnapshot.configurationValid)
        {
            AddValidationDiagnostic(
                report,
                "Native validation message sink configuration is invalid.");
        }
        for (const std::string& diagnostic :
             validationSnapshot.configurationDiagnostics)
        {
            AddValidationDiagnostic(
                report,
                "Native validation sink: " + diagnostic);
        }

        report.validationEvaluationDate =
            validationSnapshot.evaluationDate;
        report.validationMessages = validationSnapshot.messages;
        std::sort(
            report.validationMessages.begin(),
            report.validationMessages.end(),
            [](const RHIConformanceValidationMessage& left,
               const RHIConformanceValidationMessage& right)
            {
                return std::tie(left.severity,
                                left.category,
                                left.nativeId,
                                left.text,
                                left.textTruncated,
                                left.allowlisted,
                                left.allowlistEntryId) <
                       std::tie(right.severity,
                                right.category,
                                right.nativeId,
                                right.text,
                                right.textTruncated,
                                right.allowlisted,
                                right.allowlistEntryId);
            });
        report.validationInfoCount = validationSnapshot.infoCount;
        report.validationWarningCount = validationSnapshot.warningCount;
        report.validationErrorCount = validationSnapshot.errorCount;
        report.unexpectedValidationWarningCount =
            validationSnapshot.unexpectedWarningCount;
        report.unexpectedValidationErrorCount =
            validationSnapshot.unexpectedErrorCount;
        report.droppedValidationMessageCount =
            validationSnapshot.droppedMessageCount;

        const uint64 validationMessageCount =
            static_cast<uint64>(report.validationInfoCount) +
            static_cast<uint64>(report.validationWarningCount) +
            static_cast<uint64>(report.validationErrorCount);
        if (report.validationMessages.size() >
            RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES)
        {
            AddValidationDiagnostic(
                report,
                "Native validation snapshot exceeds the bounded message count.");
            report.validationMessages.resize(
                RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES);
        }
        if (validationMessageCount < report.validationMessages.size() ||
            validationMessageCount -
                    static_cast<uint64>(report.validationMessages.size()) !=
                report.droppedValidationMessageCount)
        {
            AddValidationDiagnostic(
                report,
                "Native validation snapshot counts do not match its bounded messages.");
        }
        if (report.unexpectedValidationWarningCount >
                report.validationWarningCount ||
            report.unexpectedValidationErrorCount >
                report.validationErrorCount)
        {
            AddValidationDiagnostic(
                report,
                "Native validation unexpected-message counts exceed their totals.");
        }
        for (const RHIConformanceValidationMessage& message :
             report.validationMessages)
        {
            ValidateValidationMessage(report, message);
        }

        bool hasFailedCase = false;
        bool hasEnvironmentUnavailableCase = false;
        bool hasCompatibilityRequiredUnsupported = false;
        for (size_t i = 0; i < kCaseCatalog.size(); ++i)
        {
            const RHIConformanceCaseDefinition& definition =
                kCaseCatalog[i];
            const RHIConformanceCaseResult& result =
                report.caseResults[i];
            ValidateCaseResultShape(report, result);
            CountCaseOutcome(report, result.outcome);

            if (result.outcome ==
                RHIConformanceOutcome::EnvironmentUnavailable)
            {
                hasEnvironmentUnavailableCase = true;
            }
            if (result.outcome == RHIConformanceOutcome::Failed ||
                result.outcome == RHIConformanceOutcome::NotRun)
            {
                hasFailedCase = true;
            }

            if (definition.id ==
                    RHIConformanceCaseId::SurfaceLifecycle &&
                result.outcome == RHIConformanceOutcome::Passed &&
                (!desc.surfaceRequested || !desc.surfaceRealized))
            {
                AddValidationDiagnostic(
                    report,
                    "The surface lifecycle case passed without a requested and realized surface.");
                hasFailedCase = true;
            }

            if (definition.requirement ==
                RHIConformanceRequirement::Required)
            {
                if (result.outcome == RHIConformanceOutcome::Unsupported)
                {
                    if (IsTier1Backend(desc.requestedBackend))
                    {
                        hasFailedCase = true;
                    }
                    else
                    {
                        hasCompatibilityRequiredUnsupported = true;
                    }
                }
                continue;
            }

            if (definition.requirement ==
                    RHIConformanceRequirement::CapabilityDependent &&
                desc.environmentAvailable)
            {
                const bool capabilitySupported =
                    CapabilityGateIsSupported(report, definition);
                if (capabilitySupported &&
                    result.outcome == RHIConformanceOutcome::Unsupported)
                {
                    AddValidationDiagnostic(
                        report,
                        std::string(GetRHIConformanceCaseIdName(definition.id)) +
                            " reported Unsupported while the capability report says Supported.");
                    hasFailedCase = true;
                }
                else if (!capabilitySupported &&
                         result.outcome == RHIConformanceOutcome::Passed)
                {
                    AddValidationDiagnostic(
                        report,
                        std::string(GetRHIConformanceCaseIdName(definition.id)) +
                            " passed while the capability report says Unsupported.");
                    hasFailedCase = true;
                }
                else if (!capabilitySupported &&
                         result.outcome != RHIConformanceOutcome::Unsupported)
                {
                    hasFailedCase = true;
                }
            }

        }

        if (report.unexpectedValidationWarningCount > 0 ||
            report.unexpectedValidationErrorCount > 0)
        {
            hasFailedCase = true;
        }

        if (!report.validationPassed || hasFailedCase)
        {
            report.outcome = RHIConformanceOutcome::Failed;
        }
        else if (hasEnvironmentUnavailableCase)
        {
            report.outcome =
                RHIConformanceOutcome::EnvironmentUnavailable;
        }
        else if (hasCompatibilityRequiredUnsupported)
        {
            report.outcome = RHIConformanceOutcome::Unsupported;
        }
        else
        {
            report.outcome = RHIConformanceOutcome::Passed;
        }

        return report;
    }

    std::string ExportRHIConformanceReportJson(
        const RHIConformanceReport& report)
    {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"schemaVersion\": " << report.schemaVersion << ",\n";
        ss << "  \"schemaId\": "
           << JsonString(RVX_RHI_CONFORMANCE_REPORT_SCHEMA_ID) << ",\n";
        ss << "  \"catalog\": {\n";
        ss << "    \"id\": " << JsonString(report.catalogId) << ",\n";
        ss << "    \"version\": " << report.catalogVersion << "\n";
        ss << "  },\n";
        ss << "  \"sourceCommit\": "
           << JsonString(report.sourceCommit) << ",\n";
        ss << "  \"platform\": "
           << JsonString(report.platform) << ",\n";
        ss << "  \"requestedBackend\": "
           << JsonString(ToString(report.requestedBackend)) << ",\n";
        ss << "  \"realizedBackend\": "
           << JsonString(ToString(report.realizedBackend)) << ",\n";
        ss << "  \"adapterType\": "
           << JsonString(GetRHIConformanceAdapterTypeName(
                  report.adapterType))
           << ",\n";
        ss << "  \"environmentAvailable\": "
           << JsonBool(report.environmentAvailable) << ",\n";
        ss << "  \"environmentReason\": "
           << JsonString(report.environmentReason) << ",\n";
        ss << "  \"validationRequested\": "
           << JsonBool(report.validationRequested) << ",\n";
        ss << "  \"validationEnabled\": "
           << JsonBool(report.validationEnabled) << ",\n";
        ss << "  \"surfaceRequested\": "
           << JsonBool(report.surfaceRequested) << ",\n";
        ss << "  \"surfaceRealized\": "
           << JsonBool(report.surfaceRealized) << ",\n";
        ss << "  \"processExitCode\": " << report.processExitCode << ",\n";
        ss << "  \"validationPassed\": "
           << JsonBool(report.validationPassed) << ",\n";
        ss << "  \"validationDiagnostics\": [";
        for (size_t i = 0; i < report.validationDiagnostics.size(); ++i)
        {
            ss << (i == 0 ? "" : ", ")
               << JsonString(report.validationDiagnostics[i]);
        }
        ss << "],\n";
        ss << "  \"outcome\": "
           << JsonString(GetRHIConformanceOutcomeName(report.outcome))
           << ",\n";
        ss << "  \"summary\": {\n";
        ss << "    \"passed\": " << report.passedCount << ",\n";
        ss << "    \"failed\": " << report.failedCount << ",\n";
        ss << "    \"unsupported\": " << report.unsupportedCount << ",\n";
        ss << "    \"environmentUnavailable\": "
           << report.environmentUnavailableCount << ",\n";
        ss << "    \"notRun\": " << report.notRunCount << "\n";
        ss << "  },\n";
        ss << "  \"nativeValidation\": {\n";
        ss << "    \"evaluationDate\": "
           << JsonString(report.validationEvaluationDate) << ",\n";
        ss << "    \"infoCount\": " << report.validationInfoCount << ",\n";
        ss << "    \"warningCount\": " << report.validationWarningCount << ",\n";
        ss << "    \"errorCount\": " << report.validationErrorCount << ",\n";
        ss << "    \"unexpectedWarningCount\": "
           << report.unexpectedValidationWarningCount << ",\n";
        ss << "    \"unexpectedErrorCount\": "
           << report.unexpectedValidationErrorCount << ",\n";
        ss << "    \"droppedMessageCount\": "
           << report.droppedValidationMessageCount << ",\n";
        ss << "    \"messages\": [\n";
        for (size_t i = 0; i < report.validationMessages.size(); ++i)
        {
            const RHIConformanceValidationMessage& message =
                report.validationMessages[i];
            ss << "      {\n";
            ss << "        \"severity\": "
               << JsonString(GetRHIConformanceValidationSeverityName(
                      message.severity))
               << ",\n";
            ss << "        \"category\": "
               << JsonString(message.category) << ",\n";
            ss << "        \"nativeId\": "
               << JsonString(message.nativeId) << ",\n";
            ss << "        \"text\": "
               << JsonString(message.text) << ",\n";
            ss << "        \"textTruncated\": "
               << JsonBool(message.textTruncated) << ",\n";
            ss << "        \"allowlisted\": "
               << JsonBool(message.allowlisted) << ",\n";
            ss << "        \"allowlistEntryId\": "
               << JsonString(message.allowlistEntryId) << "\n";
            ss << "      }"
               << (i + 1 < report.validationMessages.size() ? "," : "")
               << "\n";
        }
        ss << "    ]\n";
        ss << "  },\n";
        ss << "  \"capabilities\": {\n";
        ExportCapabilityEntriesJson(ss, report.capabilityReport);
        ss << "  },\n";
        ss << "  \"cases\": [\n";
        for (size_t i = 0; i < report.caseResults.size(); ++i)
        {
            const RHIConformanceCaseResult& result =
                report.caseResults[i];
            const RHIConformanceCaseDefinition* definition =
                FindRHIConformanceCaseDefinition(result.id);
            ss << "    {\n";
            ss << "      \"id\": "
               << JsonString(GetRHIConformanceCaseIdName(result.id))
               << ",\n";
            ss << "      \"requirement\": "
               << JsonString(
                      definition
                          ? GetRHIConformanceRequirementName(
                                definition->requirement)
                          : "Unknown")
               << ",\n";
            ss << "      \"capabilityGate\": ";
            if (definition && definition->hasCapabilityGate)
            {
                ss << JsonString(GetRHICapabilityFeatureName(
                    definition->capabilityFeature));
            }
            else
            {
                ss << "null";
            }
            ss << ",\n";
            ss << "      \"outcome\": "
               << JsonString(GetRHIConformanceOutcomeName(result.outcome))
               << ",\n";
            ss << "      \"durationMicroseconds\": "
               << result.durationMicroseconds << ",\n";
            ss << "      \"reasonCode\": "
               << JsonString(GetRHIConformanceReasonCodeName(
                      result.reasonCode))
               << ",\n";
            ss << "      \"diagnosticMessage\": "
               << JsonString(result.diagnosticMessage) << "\n";
            ss << "    }"
               << (i + 1 < report.caseResults.size() ? "," : "")
               << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";
        return ss.str();
    }
} // namespace RVX
