#include "RHI/RHIConformance.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RVX::Tests
{
    static_assert(
        static_cast<uint8>(RHIConformanceOutcome::EnvironmentUnavailable) == 4);
    static_assert(
        static_cast<uint8>(RHIConformanceReasonCode::NativeFailure) == 10);
    static_assert(
        static_cast<uint8>(RHIConformanceAdapterType::Software) == 2);

    namespace
    {
        RHICapabilities MakeValidCapabilities(RHIBackendType backend,
                                              bool supportsQueries = false)
        {
            RHICapabilities capabilities;
            capabilities.backendType = backend;
            capabilities.adapterName =
                std::string(ToString(backend)) + " Conformance Adapter";
            capabilities.driverVersion = "ConformanceDriver.1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;
            capabilities.supportsTimestampQueries = supportsQueries;
            capabilities.timestampFrequency = supportsQueries ? 1000000 : 0;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
            };
            capabilities.queueTopology.activeDomainCount = 1;

            if (backend == RHIBackendType::DX11 ||
                backend == RHIBackendType::OpenGL)
            {
                capabilities.queueTopology.completionMode =
                    RHIQueueCompletionMode::CompatibilityWaitIdle;
                capabilities.emulatesQueueFences = true;
            }
            else
            {
                capabilities.queueTopology.completionMode =
                    RHIQueueCompletionMode::NativeTimeline;
            }

            if (backend == RHIBackendType::DX12)
            {
                capabilities.dx12.resourceBindingTier = 2;
                capabilities.supportsAsyncCompute = true;
                capabilities.queueTopology.logicalQueueDomains = {
                    GPUQueueDomain::Graphics,
                    GPUQueueDomain::Compute,
                    GPUQueueDomain::Copy,
                };
                capabilities.queueTopology.activeDomainCount = 3;
            }
            else if (backend == RHIBackendType::Vulkan)
            {
                capabilities.vulkan.apiVersion = 1;
            }
            else if (backend == RHIBackendType::OpenGL)
            {
                capabilities.opengl.majorVersion = 4;
                capabilities.opengl.minorVersion = 5;
            }

            return capabilities;
        }

        RHIConformanceCaseResult MakePassedResult(
            RHIConformanceCaseId id,
            uint64 durationMicroseconds = 100)
        {
            RHIConformanceCaseResult result;
            result.id = id;
            result.outcome = RHIConformanceOutcome::Passed;
            result.durationMicroseconds = durationMicroseconds;
            result.reasonCode = RHIConformanceReasonCode::None;
            return result;
        }

        RHIConformanceCaseResult MakeUnsupportedResult(
            RHIConformanceCaseId id,
            RHIConformanceReasonCode reasonCode =
                RHIConformanceReasonCode::CapabilityUnsupported)
        {
            RHIConformanceCaseResult result;
            result.id = id;
            result.outcome = RHIConformanceOutcome::Unsupported;
            result.reasonCode = reasonCode;
            result.diagnosticMessage = "The reported capability is unavailable.";
            return result;
        }

        std::vector<RHIConformanceCaseResult> MakeCompleteResults(
            bool supportsQueries)
        {
            std::vector<RHIConformanceCaseResult> results;
            for (const RHIConformanceCaseDefinition& definition :
                 GetRHIConformanceCaseCatalog())
            {
                if (definition.id == RHIConformanceCaseId::Query &&
                    !supportsQueries)
                {
                    results.push_back(
                        MakeUnsupportedResult(definition.id));
                }
                else
                {
                    results.push_back(MakePassedResult(
                        definition.id,
                        100 + static_cast<uint64>(results.size())));
                }
            }
            return results;
        }

        RHIConformanceReportDesc MakeCompleteReportDesc(
            RHIBackendType backend,
            bool supportsQueries = false)
        {
            RHIConformanceReportDesc desc;
            desc.sourceCommit =
                "0123456789abcdef0123456789abcdef01234567";
            desc.platform = "Windows-x64";
            desc.requestedBackend = backend;
            desc.realizedBackend = backend;
            desc.adapterType = RHIConformanceAdapterType::Hardware;
            desc.validationRequested = true;
            desc.validationEnabled = true;
            RHIConformanceValidationMessageSink validationSink(
                backend,
                "2026-07-30");
            desc.validationSnapshot = validationSink.GetSnapshot();
            desc.surfaceRequested = true;
            desc.surfaceRealized = true;
            desc.capabilityReport = BuildRHICapabilityReport(
                MakeValidCapabilities(backend, supportsQueries));
            desc.caseResults = MakeCompleteResults(supportsQueries);
            return desc;
        }

        RHIConformanceCaseResult* FindResult(
            std::vector<RHIConformanceCaseResult>& results,
            RHIConformanceCaseId id)
        {
            const auto it = std::find_if(
                results.begin(),
                results.end(),
                [id](const RHIConformanceCaseResult& result)
                {
                    return result.id == id;
                });
            return it != results.end() ? &(*it) : nullptr;
        }

        bool HasDiagnostic(const RHIConformanceReport& report,
                           const std::string& text)
        {
            return std::any_of(
                report.validationDiagnostics.begin(),
                report.validationDiagnostics.end(),
                [&text](const std::string& diagnostic)
                {
                    return diagnostic.find(text) != std::string::npos;
                });
        }

        RHIConformanceValidationSnapshot MakeValidationSnapshot(
            RHIBackendType backend,
            const std::vector<RHIConformanceValidationMessage>& messages,
            std::vector<RHIConformanceValidationAllowlistEntry> allowlist = {})
        {
            RHIConformanceValidationMessageSink sink(
                backend,
                "2026-07-30",
                std::move(allowlist));
            for (const RHIConformanceValidationMessage& message : messages)
            {
                sink.Record(message);
            }
            return sink.GetSnapshot();
        }
    } // namespace

    TEST(RHIConformanceValidation, BaseCatalogIdsAndOrderAreFrozen)
    {
        const auto& catalog = GetRHIConformanceCaseCatalog();
        ASSERT_EQ(
            catalog.size(),
            static_cast<size_t>(RHIConformanceCaseId::Count));

        const std::vector<std::string> expectedIds = {
            "rhi.resource-and-views",
            "rhi.descriptor-snapshot",
            "rhi.graphics-pipeline-input",
            "rhi.compute-uav-to-srv",
            "rhi.copy-and-readback",
            "rhi.scoped-dependency",
            "rhi.indirect-arguments",
            "rhi.queue-and-fence",
            "rhi.query",
            "rhi.surface-lifecycle",
            "rhi.deferred-destruction",
        };

        std::set<std::string> uniqueIds;
        for (size_t i = 0; i < catalog.size(); ++i)
        {
            EXPECT_EQ(static_cast<size_t>(catalog[i].id), i);
            EXPECT_EQ(GetRHIConformanceCaseIdName(catalog[i].id),
                      expectedIds[i]);
            uniqueIds.emplace(expectedIds[i]);
        }
        EXPECT_EQ(uniqueIds.size(), catalog.size());

        const RHIConformanceCaseDefinition* query =
            FindRHIConformanceCaseDefinition(RHIConformanceCaseId::Query);
        ASSERT_NE(query, nullptr);
        EXPECT_EQ(query->requirement,
                  RHIConformanceRequirement::CapabilityDependent);
        EXPECT_TRUE(query->hasCapabilityGate);
        EXPECT_EQ(query->capabilityFeature,
                  RHICapabilityFeature::QuerySupport);
    }

    TEST(RHIConformanceValidation,
         ReversedInputsProduceTheSameCanonicalJson)
    {
        RHIConformanceReportDesc ordered =
            MakeCompleteReportDesc(RHIBackendType::DX12, true);
        ordered.validationSnapshot = MakeValidationSnapshot(
            RHIBackendType::DX12,
            {
            {RHIConformanceValidationSeverity::Info,
             "Device",
             "DX12-2",
             "Second deterministic message."},
            {RHIConformanceValidationSeverity::Info,
             "Device",
             "DX12-1",
             "First deterministic message."},
            });

        RHIConformanceReportDesc reversed = ordered;
        std::reverse(reversed.caseResults.begin(),
                     reversed.caseResults.end());
        std::reverse(reversed.capabilityReport.entries.begin(),
                     reversed.capabilityReport.entries.end());
        std::reverse(reversed.validationSnapshot.messages.begin(),
                     reversed.validationSnapshot.messages.end());

        const RHIConformanceReport orderedReport =
            BuildRHIConformanceReport(ordered);
        const RHIConformanceReport reversedReport =
            BuildRHIConformanceReport(reversed);

        ASSERT_TRUE(orderedReport.validationPassed);
        ASSERT_TRUE(reversedReport.validationPassed);
        EXPECT_EQ(orderedReport.outcome, RHIConformanceOutcome::Passed);
        EXPECT_EQ(reversedReport.outcome, RHIConformanceOutcome::Passed);
        ASSERT_EQ(orderedReport.caseResults.size(),
                  GetRHIConformanceCaseCatalog().size());
        EXPECT_EQ(orderedReport.caseResults.front().id,
                  RHIConformanceCaseId::ResourceAndViews);
        EXPECT_EQ(orderedReport.caseResults.back().id,
                  RHIConformanceCaseId::DeferredDestruction);

        const std::string orderedJson =
            ExportRHIConformanceReportJson(orderedReport);
        const std::string reversedJson =
            ExportRHIConformanceReportJson(reversedReport);
        EXPECT_EQ(orderedJson, reversedJson);
        EXPECT_NE(orderedJson.find(
                      "\"schemaId\": \"RVX.RHI.ConformanceReport\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"sourceCommit\": \"0123456789abcdef0123456789abcdef01234567\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"platform\": \"Windows-x64\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"adapterType\": \"Hardware\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"validationEnabled\": true"),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"evaluationDate\": \"2026-07-30\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"id\": \"RVX.RHI.BaseConformance\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"renderGraphBaseline\": {"),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"capabilityGate\": \"QuerySupport\""),
                  std::string::npos);
        EXPECT_NE(orderedJson.find(
                      "\"capabilityGate\": null"),
                  std::string::npos);
        EXPECT_LT(orderedJson.find("\"id\": \"rhi.resource-and-views\""),
                  orderedJson.find("\"id\": \"rhi.surface-lifecycle\""));
    }

    TEST(RHIConformanceValidation, RequestedBackendFallbackFailsClosed)
    {
        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::Vulkan);
        desc.requestedBackend = RHIBackendType::DX12;

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report, "was not realized"));
        EXPECT_NE(ExportRHIConformanceReportJson(report).find(
                      "\"requestedBackend\": \"DirectX 12\""),
                  std::string::npos);
        EXPECT_NE(ExportRHIConformanceReportJson(report).find(
                      "\"realizedBackend\": \"Vulkan\""),
                  std::string::npos);
    }

    TEST(RHIConformanceValidation,
         EnvironmentUnavailableDoesNotPublishAFalseBackendPass)
    {
        RHIConformanceReportDesc desc;
        desc.sourceCommit =
            "0123456789abcdef0123456789abcdef01234567";
        desc.platform = "Linux-x64";
        desc.requestedBackend = RHIBackendType::Vulkan;
        desc.environmentAvailable = false;
        desc.environmentReason = "Vulkan loader or ICD is unavailable.";
        desc.validationRequested = true;
        desc.surfaceRequested = true;
        desc.processExitCode = 2;

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.outcome,
                  RHIConformanceOutcome::EnvironmentUnavailable);
        EXPECT_EQ(report.realizedBackend, RHIBackendType::None);
        EXPECT_EQ(report.environmentUnavailableCount,
                  static_cast<uint32>(RHIConformanceCaseId::Count));
        EXPECT_EQ(report.passedCount, 0u);
        EXPECT_NE(ExportRHIConformanceReportJson(report).find(
                      "\"outcome\": \"EnvironmentUnavailable\""),
                  std::string::npos);
    }

    TEST(RHIConformanceValidation,
         EnvironmentUnavailableRejectsExecutedCaseResults)
    {
        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::Vulkan);
        desc.environmentAvailable = false;
        desc.environmentReason = "The Vulkan ICD is unavailable.";
        desc.realizedBackend = RHIBackendType::None;
        desc.adapterType = RHIConformanceAdapterType::Unknown;
        desc.validationEnabled = false;
        desc.surfaceRealized = false;
        desc.processExitCode = 2;

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(
            report,
            "must not publish executed case results"));
        EXPECT_EQ(report.passedCount, 0u);
        EXPECT_EQ(report.environmentUnavailableCount,
                  static_cast<uint32>(RHIConformanceCaseId::Count));
    }

    TEST(RHIConformanceValidation,
         AvailableRunFailsClosedOnProcessAndIdentityContradictions)
    {
        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        desc.sourceCommit.clear();
        desc.platform.clear();
        desc.adapterType = RHIConformanceAdapterType::Unknown;
        desc.validationEnabled = false;
        desc.processExitCode = 3;
        desc.capabilityReport.driverVersion.clear();

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report, "exact source commit"));
        EXPECT_TRUE(HasDiagnostic(report, "platform identity"));
        EXPECT_TRUE(HasDiagnostic(report, "non-zero process exit code"));
        EXPECT_TRUE(HasDiagnostic(report, "hardware or software"));
        EXPECT_TRUE(HasDiagnostic(report, "was requested but was not enabled"));
        EXPECT_TRUE(HasDiagnostic(report, "driver identity"));
    }

    TEST(RHIConformanceValidation,
         CapabilityDependentOutcomeMustAgreeWithCapabilityReport)
    {
        RHIConformanceReportDesc unsupported =
            MakeCompleteReportDesc(RHIBackendType::DX12, false);
        RHIConformanceCaseResult* unsupportedQuery =
            FindResult(unsupported.caseResults,
                       RHIConformanceCaseId::Query);
        ASSERT_NE(unsupportedQuery, nullptr);
        *unsupportedQuery =
            MakePassedResult(RHIConformanceCaseId::Query);

        RHIConformanceReport report =
            BuildRHIConformanceReport(unsupported);
        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report,
                                  "capability report says Unsupported"));

        RHIConformanceReportDesc supported =
            MakeCompleteReportDesc(RHIBackendType::DX12, true);
        RHIConformanceCaseResult* supportedQuery =
            FindResult(supported.caseResults,
                       RHIConformanceCaseId::Query);
        ASSERT_NE(supportedQuery, nullptr);
        *supportedQuery =
            MakeUnsupportedResult(RHIConformanceCaseId::Query);

        report = BuildRHIConformanceReport(supported);
        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report,
                                  "capability report says Supported"));

        const RHIConformanceReport honestUnsupported =
            BuildRHIConformanceReport(
                MakeCompleteReportDesc(RHIBackendType::DX12, false));
        EXPECT_TRUE(honestUnsupported.validationPassed);
        EXPECT_EQ(honestUnsupported.outcome,
                  RHIConformanceOutcome::Passed);
    }

    TEST(RHIConformanceValidation,
         RequiredUnsupportedFailsTier1ButRemainsHonestForCompatibility)
    {
        RHIConformanceReportDesc tier1 =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        RHIConformanceCaseResult* tier1Resource =
            FindResult(tier1.caseResults,
                       RHIConformanceCaseId::ResourceAndViews);
        ASSERT_NE(tier1Resource, nullptr);
        *tier1Resource = MakeUnsupportedResult(
            RHIConformanceCaseId::ResourceAndViews,
            RHIConformanceReasonCode::RequiredCaseUnsupported);

        RHIConformanceReport report =
            BuildRHIConformanceReport(tier1);
        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);

        RHIConformanceReportDesc compatibility =
            MakeCompleteReportDesc(RHIBackendType::OpenGL);
        RHIConformanceCaseResult* compatibilityResource =
            FindResult(compatibility.caseResults,
                       RHIConformanceCaseId::ResourceAndViews);
        ASSERT_NE(compatibilityResource, nullptr);
        *compatibilityResource = MakeUnsupportedResult(
            RHIConformanceCaseId::ResourceAndViews,
            RHIConformanceReasonCode::RequiredCaseUnsupported);

        report = BuildRHIConformanceReport(compatibility);
        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.outcome,
                  RHIConformanceOutcome::Unsupported);
    }

    TEST(RHIConformanceValidation,
         SurfaceVerdictRequiresRequestedAndRealizedSurface)
    {
        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        desc.surfaceRealized = false;

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(
            report,
            "without a requested and realized surface"));
    }

    TEST(RHIConformanceValidation,
         DuplicateCasesAndUnexpectedNativeMessagesFailClosed)
    {
        RHIConformanceReportDesc duplicate =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        duplicate.caseResults.push_back(
            duplicate.caseResults.front());

        RHIConformanceReport report =
            BuildRHIConformanceReport(duplicate);
        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report, "duplicate ID"));

        RHIConformanceReportDesc warning =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        warning.validationSnapshot = MakeValidationSnapshot(
            RHIBackendType::DX12,
            {
            {RHIConformanceValidationSeverity::Warning,
             "Descriptor",
             "D3D12-1001",
             "Unexpected descriptor warning."},
            });

        report = BuildRHIConformanceReport(warning);
        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.unexpectedValidationWarningCount, 1u);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);

        warning.validationSnapshot = MakeValidationSnapshot(
            RHIBackendType::DX12,
            {
                {RHIConformanceValidationSeverity::Warning,
                 "Descriptor",
                 "D3D12-1001",
                 "Unexpected descriptor warning."},
            },
            {
                {"dx12-reviewed-1001",
                 RHIBackendType::DX12,
                 "D3D12-1001",
                 "Reviewed driver warning with no correctness impact.",
                 "rendering-owner",
                 "2026-08-30"},
            });
        report = BuildRHIConformanceReport(warning);
        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.unexpectedValidationWarningCount, 0u);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Passed);
    }

    TEST(RHIConformanceValidation,
         ValidationAllowlistMatchesOnlyBackendAndExactNativeId)
    {
        const RHIConformanceValidationAllowlistEntry reviewedWarning = {
            "dx12-reviewed-1001",
            RHIBackendType::DX12,
            "D3D12-1001",
            "Reviewed driver warning with no correctness impact.",
            "rendering-owner",
            "2026-08-30",
        };

        RHIConformanceValidationMessageSink exactSink(
            RHIBackendType::DX12,
            "2026-07-30",
            {reviewedWarning});
        exactSink.Record(
            RHIConformanceValidationSeverity::Warning,
            "Descriptor",
            "D3D12-1001",
            "Text may change without becoming the suppression identity.");
        RHIConformanceValidationSnapshot snapshot =
            exactSink.GetSnapshot();
        ASSERT_TRUE(snapshot.configurationValid);
        ASSERT_EQ(snapshot.messages.size(), 1u);
        EXPECT_TRUE(snapshot.messages.front().allowlisted);
        EXPECT_EQ(snapshot.messages.front().allowlistEntryId,
                  "dx12-reviewed-1001");
        EXPECT_EQ(snapshot.unexpectedWarningCount, 0u);

        RHIConformanceValidationMessageSink wrongBackendSink(
            RHIBackendType::Vulkan,
            "2026-07-30",
            {reviewedWarning});
        wrongBackendSink.Record(
            RHIConformanceValidationSeverity::Warning,
            "Descriptor",
            "D3D12-1001",
            "Same native ID on a different backend.");
        snapshot = wrongBackendSink.GetSnapshot();
        EXPECT_FALSE(snapshot.messages.front().allowlisted);
        EXPECT_EQ(snapshot.unexpectedWarningCount, 1u);

        RHIConformanceValidationMessageSink wrongIdSink(
            RHIBackendType::DX12,
            "2026-07-30",
            {reviewedWarning});
        wrongIdSink.Record(
            RHIConformanceValidationSeverity::Warning,
            "Descriptor",
            "D3D12-9999",
            "Same text cannot wildcard a different native ID.");
        snapshot = wrongIdSink.GetSnapshot();
        EXPECT_FALSE(snapshot.messages.front().allowlisted);
        EXPECT_EQ(snapshot.unexpectedWarningCount, 1u);

        RHIConformanceValidationMessageSink expiredSink(
            RHIBackendType::DX12,
            "2026-09-01",
            {reviewedWarning});
        expiredSink.Record(
            RHIConformanceValidationSeverity::Warning,
            "Descriptor",
            "D3D12-1001",
            "Expired review.");
        snapshot = expiredSink.GetSnapshot();
        EXPECT_FALSE(snapshot.messages.front().allowlisted);
        EXPECT_EQ(snapshot.unexpectedWarningCount, 1u);

        RHIConformanceValidationMessageSink errorSink(
            RHIBackendType::DX12,
            "2026-07-30",
            {reviewedWarning});
        errorSink.Record(
            RHIConformanceValidationSeverity::Error,
            "Descriptor",
            "D3D12-1001",
            "Errors are never allowlisted.");
        snapshot = errorSink.GetSnapshot();
        EXPECT_FALSE(snapshot.messages.front().allowlisted);
        EXPECT_EQ(snapshot.unexpectedErrorCount, 1u);
    }

    TEST(RHIConformanceValidation,
         ValidationAllowlistRequiresReviewedMetadata)
    {
        const RHIConformanceValidationAllowlistEntry incomplete = {
            "",
            RHIBackendType::Auto,
            "",
            "",
            "",
            "2026-02-30",
        };
        RHIConformanceValidationMessageSink sink(
            RHIBackendType::DX12,
            "2026-07-30",
            {incomplete});

        const RHIConformanceValidationSnapshot snapshot =
            sink.GetSnapshot();

        EXPECT_FALSE(snapshot.configurationValid);
        EXPECT_GE(snapshot.configurationDiagnostics.size(), 5u);

        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        desc.validationSnapshot = snapshot;
        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);
        EXPECT_FALSE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Failed);
        EXPECT_TRUE(HasDiagnostic(report, "sink configuration is invalid"));
    }

    TEST(RHIConformanceValidation,
         ValidationSinkIsThreadSafeAndBoundsDeterministicEvidence)
    {
        RHIConformanceValidationMessageSink sink(
            RHIBackendType::DX12,
            "2026-07-30");
        std::vector<std::thread> producers;
        for (uint32 producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back(
                [&sink, producer]()
                {
                    for (uint32 messageIndex = 0;
                         messageIndex < 40;
                         ++messageIndex)
                    {
                        sink.Record(
                            RHIConformanceValidationSeverity::Info,
                            "Threaded",
                            "INFO-" + std::to_string(producer) + "-" +
                                std::to_string(1000 + messageIndex),
                            "Concurrent native validation message.");
                    }
                });
        }
        for (std::thread& producer : producers)
        {
            producer.join();
        }

        const RHIConformanceValidationSnapshot snapshot =
            sink.GetSnapshot();

        EXPECT_TRUE(snapshot.configurationValid);
        EXPECT_EQ(snapshot.infoCount, 160u);
        EXPECT_EQ(snapshot.messages.size(),
                  RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES);
        EXPECT_EQ(snapshot.droppedMessageCount, 96u);
        EXPECT_TRUE(std::is_sorted(
            snapshot.messages.begin(),
            snapshot.messages.end(),
            [](const RHIConformanceValidationMessage& left,
               const RHIConformanceValidationMessage& right)
            {
                return left.nativeId < right.nativeId;
            }));
    }

    TEST(RHIConformanceValidation,
         ValidationSinkBoundsNativeMessageText)
    {
        RHIConformanceValidationMessageSink sink(
            RHIBackendType::DX12,
            "2026-07-30");
        sink.Record(
            RHIConformanceValidationSeverity::Info,
            "Device",
            "INFO-LONG",
            std::string(
                RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES + 128,
                'x'));

        const RHIConformanceValidationSnapshot snapshot =
            sink.GetSnapshot();

        ASSERT_EQ(snapshot.messages.size(), 1u);
        EXPECT_TRUE(snapshot.messages.front().textTruncated);
        EXPECT_EQ(snapshot.messages.front().text.size(),
                  RVX_RHI_CONFORMANCE_MAX_VALIDATION_TEXT_BYTES);
    }

    TEST(RHIConformanceValidation,
         ValidationMessagesAreBoundedWithoutLosingCounts)
    {
        RHIConformanceReportDesc desc =
            MakeCompleteReportDesc(RHIBackendType::DX12);
        RHIConformanceValidationMessageSink sink(
            RHIBackendType::DX12,
            "2026-07-30");
        for (uint32 i = 0;
             i < RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES + 6;
             ++i)
        {
            RHIConformanceValidationMessage message;
            message.severity = RHIConformanceValidationSeverity::Info;
            message.category = "Info";
            message.nativeId = "INFO-" + std::to_string(i);
            message.text = "Message " + std::to_string(i);
            sink.Record(message);
        }
        desc.validationSnapshot = sink.GetSnapshot();

        const RHIConformanceReport report =
            BuildRHIConformanceReport(desc);

        EXPECT_TRUE(report.validationPassed);
        EXPECT_EQ(report.outcome, RHIConformanceOutcome::Passed);
        EXPECT_EQ(report.validationInfoCount,
                  RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES + 6);
        EXPECT_EQ(report.validationMessages.size(),
                  RVX_RHI_CONFORMANCE_MAX_VALIDATION_MESSAGES);
        EXPECT_EQ(report.droppedValidationMessageCount, 6u);
    }
} // namespace RVX::Tests
