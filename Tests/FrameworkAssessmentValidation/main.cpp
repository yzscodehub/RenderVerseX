/** @file main.cpp @brief Validation for the framework assessment report contract. */

#include "Samples/FrameworkAssessment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace RVX
{
    namespace
    {
        AssessmentFingerprint MakeFingerprint()
        {
            AssessmentFingerprint fingerprint;
            fingerprint.sampleId = "model-viewer";
            fingerprint.sampleRevision = "scene-r1";
            fingerprint.engineBuild = "build-123";
            fingerprint.backend = "vulkan";
            fingerprint.device = "vendor-device-driver";
            fingerprint.renderConfiguration = "1280x720-default";
            fingerprint.assetSet = "fixture-r1";
            fingerprint.platform = "windows";
            return fingerprint;
        }

        SampleAssessmentContract MakeContract()
        {
            SampleAssessmentContract contract;
            contract.code = AssessmentCode("SAMPLE.MODEL_VIEWER");
            contract.revision = "1";
            contract.checkpoints = {
                {AssessmentCode("STARTUP.READY"), "Sample reached ready state."},
                {AssessmentCode("SCENE.LOADED"), "Scene assets are available."}};
            contract.actions = {
                {AssessmentCode("CAMERA.ORBIT"), "Orbit the active camera."}};
            contract.invariants = {
                {AssessmentCode("SCENE.HIERARCHY.AUTHORITY"),
                 "Scene hierarchy retains one authority."}};
            contract.metrics = {
                {AssessmentCode("FRAME.DRAW_COUNT"), "draws", "Submitted draws."}};
            contract.capabilities = {
                {AssessmentCode("RENDER.GPU_DRIVEN"),
                 "GPU-driven rendering can be assessed."}};
            return contract;
        }

        SampleAssessmentContract MakeEmptyContract()
        {
            SampleAssessmentContract contract;
            contract.code = AssessmentCode("SAMPLE.EMPTY");
            contract.revision = "1";
            return contract;
        }

        AssessmentMetadata MakeMetadata()
        {
            AssessmentMetadata metadata;
            metadata.fingerprint = MakeFingerprint();
            metadata.values.emplace("runMode", "smoke");
            return metadata;
        }

        AssessmentMetricValue MakeMetric(uint64 value)
        {
            AssessmentMetricValue metric;
            metric.metric = {AssessmentCode("FRAME.DRAW_COUNT"), "draws",
                             "Submitted draws."};
            metric.value =
                DiagnosticValue<AssessmentScalar>::Available(AssessmentScalar{value});
            return metric;
        }

        AssessmentCapability MakeGpuDrivenCapability(bool gating = false)
        {
            AssessmentCapability capability;
            capability.code = AssessmentCode("RENDER.GPU_DRIVEN");
            capability.description = "GPU-driven rendering can be assessed.";
            capability.gating = gating;
            if (gating)
            {
                capability.blockingReason =
                    "GPU-driven rendering support is required by this contract.";
            }
            return capability;
        }

        AssessmentCapabilityObservation MakeGpuDrivenObservation(
            bool supported,
            AssessmentCheckpoint checkpoint =
                {AssessmentCode("SCENE.LOADED"),
                 "Scene assets are available."})
        {
            AssessmentCapabilityObservation observation;
            observation.capability = MakeGpuDrivenCapability();
            observation.checkpoint = std::move(checkpoint);
            observation.value = DiagnosticValue<bool>::Available(supported);
            observation.reason = supported
                                     ? "GPU-driven path was evaluated and is supported."
                                     : "GPU-driven path was evaluated and is unsupported.";
            return observation;
        }

        Finding MakeFinding(FindingSeverity severity,
                            bool gating = true,
                            FindingConfidence confidence =
                                FindingConfidence::Confirmed,
                            FindingClass classification =
                                FindingClass::ContractViolation)
        {
            Finding finding;
            finding.code = AssessmentCode("SCENE.HIERARCHY.AUTHORITY_MISMATCH");
            finding.subsystemCode = AssessmentCode("SCENE.HIERARCHY");
            finding.invariantCode =
                AssessmentCode("SCENE.HIERARCHY.AUTHORITY");
            finding.checkpoint = AssessmentCheckpoints::ScenarioStable;
            finding.classification = classification;
            finding.severity = severity;
            finding.confidence = confidence;
            finding.summary = "Scene authority mismatch.";
            finding.expected = "One hierarchy authority.";
            finding.observed = "Multiple hierarchy authorities.";
            finding.frameBegin = DiagnosticValue<uint64>::Available(120);
            finding.frameEnd = DiagnosticValue<uint64>::Available(121);
            finding.sceneRevision = DiagnosticValue<uint64>::Available(9);
            finding.resourceGeneration = DiagnosticValue<uint64>::Available(4);
            finding.requestId = DiagnosticValue<uint64>::Available(40);
            finding.completionToken = DiagnosticValue<uint64>::Available(41);
            finding.traceCorrelationId = "trace-42";
            finding.artifactPaths = {"artifacts/authority.json"};
            finding.gating = gating;
            if (gating)
            {
                finding.blockingReason = "Hierarchy authority is required.";
            }
            return finding;
        }

        bool RecordCompleteContract(FrameworkAssessmentSession& session)
        {
            return session.RecordCheckpoint(
                       {AssessmentCode("STARTUP.READY"),
                        "Sample reached ready state."}) &&
                   session.RecordCheckpoint(
                       {AssessmentCode("SCENE.LOADED"),
                        "Scene assets are available."}) &&
                   session.RecordMetric(
                       {AssessmentCode("STARTUP.READY"),
                        "Sample reached ready state."},
                       MakeMetric(2)) &&
                   session.RecordAction(
                       {AssessmentCode("CAMERA.ORBIT"),
                        "Orbit the active camera."}) &&
                   session.RecordCapabilityObservation(
                       MakeGpuDrivenObservation(true)) &&
                   session.RecordInvariantObservation(
                       {{AssessmentCode("SCENE.HIERARCHY.AUTHORITY"),
                         "Scene hierarchy retains one authority."},
                        {AssessmentCode("SCENE.LOADED"),
                         "Scene assets are available."}});
        }

        const Finding* FindFindingByCode(const SampleAssessmentReport& report,
                                         std::string_view code,
                                         std::string_view observedFragment = {})
        {
            const auto finding = std::find_if(
                report.findings.begin(), report.findings.end(),
                [code, observedFragment](const Finding& candidate)
                {
                    return candidate.code.GetValue() == code &&
                           (observedFragment.empty() ||
                            candidate.observed.find(observedFragment) !=
                                std::string::npos);
                });
            return finding == report.findings.end() ? nullptr : &*finding;
        }
    } // namespace

    TEST(FrameworkAssessmentValidation, StableCodesRejectUnstableText)
    {
        EXPECT_TRUE(AssessmentCode::IsStable(
            "SCENE.HIERARCHY.AUTHORITY_MISMATCH"));
        EXPECT_TRUE(AssessmentCode("FRAME.DRAW_COUNT").IsValid());
        EXPECT_FALSE(AssessmentCode::IsStable("scene.draw_count"));
        EXPECT_FALSE(AssessmentCode::IsStable("SCENE..DRAW_COUNT"));
        EXPECT_FALSE(AssessmentCode::IsStable("SCENE.1DRAW"));
        EXPECT_STREQ(GetFindingSeverityCode(FindingSeverity::Fatal), "FATAL");
        EXPECT_STREQ(GetFindingClassCode(FindingClass::InstrumentationGap),
                     "INSTRUMENTATION_GAP");
        EXPECT_STREQ(GetFindingConfidenceCode(FindingConfidence::Confirmed),
                     "CONFIRMED");
    }

    TEST(FrameworkAssessmentValidation, FixedCheckpointsHaveStableCodes)
    {
        EXPECT_EQ(AssessmentCheckpoints::EngineBaseline.code.GetValue(),
                  "ENGINE.BASELINE");
        EXPECT_EQ(AssessmentCheckpoints::ScenarioSetup.code.GetValue(),
                  "SCENARIO.SETUP");
        EXPECT_EQ(AssessmentCheckpoints::ActionRequested.code.GetValue(),
                  "ACTION.REQUESTED");
        EXPECT_EQ(AssessmentCheckpoints::ActionApplied.code.GetValue(),
                  "ACTION.APPLIED");
        EXPECT_EQ(AssessmentCheckpoints::ScenarioStable.code.GetValue(),
                  "SCENARIO.STABLE");
        EXPECT_EQ(AssessmentCheckpoints::TeardownBefore.code.GetValue(),
                  "TEARDOWN.BEFORE");
        EXPECT_EQ(AssessmentCheckpoints::TeardownSceneComplete.code.GetValue(),
                  "TEARDOWN.SCENE_COMPLETE");
        EXPECT_EQ(AssessmentCheckpoints::TeardownRenderDrained.code.GetValue(),
                  "TEARDOWN.RENDER_DRAINED");
        EXPECT_EQ(AssessmentCheckpoints::EngineShutdownComplete.code.GetValue(),
                  "ENGINE.SHUTDOWN_COMPLETE");
    }

    TEST(FrameworkAssessmentValidation, UnavailableMetricDoesNotBecomeZero)
    {
        AssessmentMetricValue metric;
        metric.metric = {AssessmentCode("FRAME.DRAW_COUNT"), "draws",
                         "Submitted draws."};
        metric.value = DiagnosticValue<AssessmentScalar>::Unavailable(
            "Renderer diagnostics were not captured.");

        EXPECT_FALSE(metric.value.IsAvailable());
        EXPECT_FALSE(metric.value.GetValue().has_value());
        EXPECT_EQ(metric.value.GetReason(), "Renderer diagnostics were not captured.");

        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        ASSERT_TRUE(session.RecordCheckpoint(
            {AssessmentCode("STARTUP.READY"), "Sample reached ready state."}));
        ASSERT_TRUE(session.RecordCheckpoint(
            {AssessmentCode("SCENE.LOADED"), "Scene assets are available."}));
        ASSERT_TRUE(session.RecordInvariantObservation(
            {{AssessmentCode("SCENE.HIERARCHY.AUTHORITY"),
              "Scene hierarchy retains one authority."},
             {AssessmentCode("SCENE.LOADED"), "Scene assets are available."}}));
        ASSERT_TRUE(session.RecordMetric(
            {AssessmentCode("STARTUP.READY"), "Sample reached ready state."},
            std::move(metric)));
        const SampleAssessmentReport report = session.Finalize();
        const std::string json = FrameworkAssessmentJsonWriter::ToJson(report);
        EXPECT_NE(json.find("\"available\":false"), std::string::npos);
        EXPECT_EQ(json.find("\"value\":0"), std::string::npos);
        const Finding* finding = FindFindingByCode(
            report, "INSTRUMENTATION.METRIC.UNAVAILABLE", "FRAME.DRAW_COUNT");
        ASSERT_NE(finding, nullptr);
        EXPECT_EQ(finding->classification, FindingClass::InstrumentationGap);
        EXPECT_EQ(finding->confidence, FindingConfidence::Confirmed);
        EXPECT_EQ(finding->severity, FindingSeverity::Error);
        EXPECT_TRUE(finding->gating);
        EXPECT_TRUE(IsBlockingGrade(report.blockGrade));
    }

    TEST(FrameworkAssessmentValidation, UnavailableMetricRequiresAReason)
    {
        AssessmentMetricValue metric;
        metric.metric = {AssessmentCode("FRAME.GPU_TIME"), "ms",
                         "Completion-owned GPU frame time."};
        metric.value =
            DiagnosticValue<AssessmentScalar>::Unavailable(std::string{});

        EXPECT_FALSE(metric.IsValid());

        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        EXPECT_FALSE(session.RecordMetric(
            {AssessmentCode("STARTUP.READY"), "Sample reached ready state."},
            std::move(metric)));
    }

    TEST(FrameworkAssessmentValidation,
         LaterCompletionOwnedMetricSatisfiesRequiredCoverage)
    {
        SampleAssessmentContract contract = MakeEmptyContract();
        contract.metrics = {
            {AssessmentCode("RENDER.GPU.TERMINAL_CRITICAL_PATH_MS"),
             "ms",
             "Completion-owned Graphics frame time."}};
        FrameworkAssessmentSession session(std::move(contract), MakeMetadata());

        AssessmentMetricValue pending;
        pending.metric = {
            AssessmentCode("RENDER.GPU.TERMINAL_CRITICAL_PATH_MS"),
            "ms",
            "Completion-owned Graphics frame time."};
        pending.value = DiagnosticValue<AssessmentScalar>::Unavailable(
            "The submitted Graphics fence has not completed yet.");
        ASSERT_TRUE(session.RecordMetric(
            AssessmentCheckpoints::ScenarioStable, std::move(pending)));

        AssessmentMetricValue completed;
        completed.metric = {
            AssessmentCode("RENDER.GPU.TERMINAL_CRITICAL_PATH_MS"),
            "ms",
            "Completion-owned Graphics frame time."};
        completed.value = DiagnosticValue<AssessmentScalar>::Available(
            AssessmentScalar{float64{0.0}});
        ASSERT_TRUE(session.RecordMetric(
            AssessmentCheckpoints::TeardownRenderDrained,
            std::move(completed)));

        const SampleAssessmentReport report = session.Finalize();
        EXPECT_EQ(FindFindingByCode(
                      report,
                      "INSTRUMENTATION.METRIC.UNAVAILABLE",
                      "RENDER.GPU.TERMINAL_CRITICAL_PATH_MS"),
                  nullptr);
        EXPECT_FALSE(IsBlockingGrade(report.blockGrade));
    }

    TEST(FrameworkAssessmentValidation, FinalizeDetectsMissingRequiredCoverage)
    {
        SampleAssessmentContract contract = MakeEmptyContract();
        contract.checkpoints = {
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable};
        contract.actions = {
            {AssessmentCode("CAMERA.ORBIT"), "Orbit the active camera."}};
        contract.metrics = {
            {AssessmentCode("FRAME.DRAW_COUNT"), "draws", "Submitted draws."}};
        contract.invariants = {
            {AssessmentCode("SCENE.HIERARCHY.AUTHORITY"),
             "Scene hierarchy retains one authority."}};

        FrameworkAssessmentSession session(std::move(contract), MakeMetadata());
        const SampleAssessmentReport report = session.Finalize();

        const Finding* actionApplied = FindFindingByCode(
            report, "INSTRUMENTATION.CHECKPOINT.MISSING", "ACTION.APPLIED");
        const Finding* scenarioStable = FindFindingByCode(
            report, "INSTRUMENTATION.CHECKPOINT.MISSING", "SCENARIO.STABLE");
        const Finding* missingAction = FindFindingByCode(
            report, "INSTRUMENTATION.ACTION.MISSING", "CAMERA.ORBIT");
        const Finding* missingMetric = FindFindingByCode(
            report, "INSTRUMENTATION.METRIC.MISSING", "FRAME.DRAW_COUNT");
        const Finding* missingInvariant = FindFindingByCode(
            report, "INSTRUMENTATION.INVARIANT.MISSING",
            "SCENE.HIERARCHY.AUTHORITY");

        ASSERT_NE(actionApplied, nullptr);
        ASSERT_NE(scenarioStable, nullptr);
        ASSERT_NE(missingAction, nullptr);
        ASSERT_NE(missingMetric, nullptr);
        ASSERT_NE(missingInvariant, nullptr);
        for (const Finding* finding :
             {actionApplied, scenarioStable, missingAction, missingMetric,
              missingInvariant})
        {
            EXPECT_EQ(finding->classification, FindingClass::InstrumentationGap);
            EXPECT_EQ(finding->confidence, FindingConfidence::Confirmed);
            EXPECT_EQ(finding->severity, FindingSeverity::Error);
            EXPECT_TRUE(finding->gating);
        }
        EXPECT_EQ(report.blockGrade, AssessmentBlockGrade::Blocked);
    }

    TEST(FrameworkAssessmentValidation,
         ExplicitInvariantObservationCompletesContractWithoutFalseFinding)
    {
        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(session));

        const SampleAssessmentReport first = session.Finalize();
        EXPECT_TRUE(first.findings.empty());
        EXPECT_EQ(first.blockGrade, AssessmentBlockGrade::Pass);
        ASSERT_EQ(first.invariantObservations.size(), 1u);

        const SampleAssessmentReport second = session.Finalize();
        EXPECT_TRUE(second.findings.empty());
        EXPECT_EQ(second.blockGrade, AssessmentBlockGrade::Pass);
        EXPECT_FALSE(session.RecordCheckpoint(AssessmentCheckpoints::ActionApplied));
    }

    TEST(FrameworkAssessmentValidation,
         CapabilityObservationsDistinguishCoverageGapsAndInstrumentation)
    {
        SampleAssessmentContract advisoryContract = MakeEmptyContract();
        advisoryContract.capabilities = {MakeGpuDrivenCapability()};

        FrameworkAssessmentSession coveredSession(advisoryContract,
                                                  MakeMetadata());
        ASSERT_TRUE(coveredSession.RecordCapabilityObservation(
            MakeGpuDrivenObservation(true)));
        const SampleAssessmentReport coveredReport = coveredSession.Finalize();
        EXPECT_TRUE(coveredReport.findings.empty());
        EXPECT_EQ(coveredReport.blockGrade, AssessmentBlockGrade::Pass);
        ASSERT_EQ(coveredReport.capabilityObservations.size(), 1u);

        FrameworkAssessmentSession unsupportedSession(advisoryContract,
                                                      MakeMetadata());
        ASSERT_TRUE(unsupportedSession.RecordCapabilityObservation(
            MakeGpuDrivenObservation(false)));
        const SampleAssessmentReport unsupportedReport =
            unsupportedSession.Finalize();
        const Finding* unsupported = FindFindingByCode(
            unsupportedReport, "CAPABILITY.UNAVAILABLE", "RENDER.GPU_DRIVEN");
        ASSERT_NE(unsupported, nullptr);
        EXPECT_EQ(unsupported->classification, FindingClass::CapabilityGap);
        EXPECT_EQ(unsupported->confidence, FindingConfidence::Confirmed);
        EXPECT_EQ(unsupported->severity, FindingSeverity::Warning);
        EXPECT_FALSE(unsupported->gating);
        EXPECT_FALSE(unsupported->capabilityBlocking);
        EXPECT_EQ(unsupportedReport.blockGrade, AssessmentBlockGrade::Advisory);
        const std::string unsupportedJson =
            FrameworkAssessmentJsonWriter::ToJson(unsupportedReport);
        EXPECT_NE(unsupportedJson.find(
                      "\"capability\":{\"code\":\"RENDER.GPU_DRIVEN\""),
                  std::string::npos);
        EXPECT_NE(unsupportedJson.find(
                      "\"reason\":\"GPU-driven path was evaluated and is unsupported.\""),
                  std::string::npos);

        FrameworkAssessmentSession missingSession(advisoryContract, MakeMetadata());
        const SampleAssessmentReport missingReport = missingSession.Finalize();
        const Finding* missing = FindFindingByCode(
            missingReport, "INSTRUMENTATION.CAPABILITY.MISSING",
            "RENDER.GPU_DRIVEN");
        ASSERT_NE(missing, nullptr);
        EXPECT_EQ(missing->classification, FindingClass::InstrumentationGap);
        EXPECT_EQ(missing->confidence, FindingConfidence::Confirmed);
        EXPECT_EQ(missing->severity, FindingSeverity::Error);
        EXPECT_TRUE(missing->gating);
        EXPECT_EQ(missingReport.blockGrade, AssessmentBlockGrade::Blocked);

        FrameworkAssessmentSession unavailableSession(advisoryContract,
                                                      MakeMetadata());
        AssessmentCapabilityObservation unavailable = MakeGpuDrivenObservation(false);
        unavailable.value = DiagnosticValue<bool>::Unavailable(
            "The renderer did not expose capability diagnostics.");
        unavailable.reason = "Capability probe was not available.";
        ASSERT_TRUE(unavailableSession.RecordCapabilityObservation(
            std::move(unavailable)));
        const SampleAssessmentReport unavailableReport =
            unavailableSession.Finalize();
        const Finding* unavailableFinding = FindFindingByCode(
            unavailableReport, "INSTRUMENTATION.CAPABILITY.UNAVAILABLE",
            "RENDER.GPU_DRIVEN");
        ASSERT_NE(unavailableFinding, nullptr);
        EXPECT_EQ(unavailableFinding->classification,
                  FindingClass::InstrumentationGap);
        EXPECT_EQ(unavailableReport.blockGrade, AssessmentBlockGrade::Blocked);
    }

    TEST(FrameworkAssessmentValidation,
         GatedCapabilityGapBlocksOnlyWhenDeclaredByContract)
    {
        SampleAssessmentContract contract = MakeEmptyContract();
        contract.capabilities = {MakeGpuDrivenCapability(true)};

        FrameworkAssessmentSession session(std::move(contract), MakeMetadata());
        ASSERT_TRUE(session.RecordCapabilityObservation(MakeGpuDrivenObservation(
            false)));
        const SampleAssessmentReport report = session.Finalize();
        const Finding* finding = FindFindingByCode(
            report, "CAPABILITY.UNAVAILABLE", "RENDER.GPU_DRIVEN");
        ASSERT_NE(finding, nullptr);
        EXPECT_EQ(finding->classification, FindingClass::CapabilityGap);
        EXPECT_EQ(finding->severity, FindingSeverity::Error);
        EXPECT_TRUE(finding->gating);
        EXPECT_TRUE(finding->capabilityBlocking);
        EXPECT_EQ(report.blockGrade, AssessmentBlockGrade::Blocked);
        const std::string json = FrameworkAssessmentJsonWriter::ToJson(report);
        EXPECT_NE(json.find("\"capabilityBlocking\":true"),
                  std::string::npos);
    }

    TEST(FrameworkAssessmentValidation,
         FinalReportNormalizesFindingArtifactsBeforeStorage)
    {
        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(session));
        Finding finding = MakeFinding(FindingSeverity::Warning, false);
        finding.artifactPaths = {
            "artifacts/zeta.json", "artifacts/alpha.json", "artifacts/zeta.json"};
        ASSERT_TRUE(session.RecordFinding(std::move(finding)));

        const SampleAssessmentReport report = session.Finalize();
        const Finding* normalized = FindFindingByCode(
            report, "SCENE.HIERARCHY.AUTHORITY_MISMATCH");
        ASSERT_NE(normalized, nullptr);
        ASSERT_EQ(normalized->artifactPaths.size(), 2u);
        EXPECT_EQ(normalized->artifactPaths[0].generic_string(),
                  "artifacts/alpha.json");
        EXPECT_EQ(normalized->artifactPaths[1].generic_string(),
                  "artifacts/zeta.json");

        const std::optional<SampleAssessmentReport> stored =
            session.GetFinalReport();
        ASSERT_TRUE(stored.has_value());
        ASSERT_EQ(stored->findings.size(), report.findings.size());
        EXPECT_EQ(stored->findings.front().artifactPaths,
                  report.findings.front().artifactPaths);
    }

    TEST(FrameworkAssessmentValidation, FindingsProduceGradedBlocking)
    {
        FrameworkAssessmentSession warningSession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(warningSession));
        ASSERT_TRUE(warningSession.RecordFinding(
            MakeFinding(FindingSeverity::Warning, false)));
        const SampleAssessmentReport warningReport = warningSession.Finalize();
        EXPECT_EQ(warningReport.blockGrade, AssessmentBlockGrade::Advisory);
        EXPECT_FALSE(IsBlockingGrade(warningReport.blockGrade));

        FrameworkAssessmentSession errorSession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(errorSession));
        ASSERT_TRUE(errorSession.RecordFinding(MakeFinding(FindingSeverity::Error)));
        const SampleAssessmentReport errorReport = errorSession.Finalize();
        EXPECT_EQ(errorReport.blockGrade, AssessmentBlockGrade::Blocked);
        EXPECT_TRUE(IsBlockingGrade(errorReport.blockGrade));

        FrameworkAssessmentSession criticalSession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(criticalSession));
        ASSERT_TRUE(criticalSession.RecordFinding(MakeFinding(FindingSeverity::Fatal)));
        const SampleAssessmentReport criticalReport = criticalSession.Finalize();
        EXPECT_EQ(criticalReport.blockGrade, AssessmentBlockGrade::Fatal);

        FrameworkAssessmentSession ungatedSession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(ungatedSession));
        ASSERT_TRUE(ungatedSession.RecordFinding(
            MakeFinding(FindingSeverity::Error, false)));
        EXPECT_EQ(ungatedSession.Finalize().blockGrade,
                  AssessmentBlockGrade::Advisory);

        FrameworkAssessmentSession suspectedSession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(suspectedSession));
        ASSERT_TRUE(suspectedSession.RecordFinding(MakeFinding(
            FindingSeverity::Fatal, true, FindingConfidence::Suspected)));
        EXPECT_EQ(suspectedSession.Finalize().blockGrade,
                  AssessmentBlockGrade::Advisory);

        FrameworkAssessmentSession capabilitySession(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(capabilitySession));
        ASSERT_TRUE(capabilitySession.RecordFinding(MakeFinding(
            FindingSeverity::Error, true, FindingConfidence::Confirmed,
            FindingClass::CapabilityGap)));
        EXPECT_EQ(capabilitySession.Finalize().blockGrade,
                  AssessmentBlockGrade::Advisory);
    }

    TEST(FrameworkAssessmentValidation, ChannelIsBoundedAndPreservesAcceptedEvents)
    {
        SampleAssessmentChannel channel(2);
        EXPECT_TRUE(channel.MarkAction(
            AssessmentAction{AssessmentCode("CAMERA.ORBIT"), "Orbit."}));
        EXPECT_TRUE(channel.MarkCheckpoint(
            AssessmentCheckpoint{AssessmentCode("STARTUP.READY"), "Ready."}));
        EXPECT_FALSE(channel.MarkCheckpoint(
            AssessmentCheckpoint{AssessmentCode("SCENE.LOADED"), "Loaded."}));
        EXPECT_EQ(channel.GetPendingEventCount(), 2u);
        EXPECT_EQ(channel.GetDroppedEventCount(), 1u);

        const std::vector<SampleAssessmentEvent> events = channel.Drain();
        ASSERT_EQ(events.size(), 2u);
        EXPECT_TRUE(std::holds_alternative<AssessmentAction>(events.front()));
        EXPECT_TRUE(std::holds_alternative<AssessmentCheckpoint>(events.back()));
        EXPECT_EQ(channel.GetPendingEventCount(), 0u);

        SampleAssessmentChannel capabilityChannel(1);
        EXPECT_TRUE(capabilityChannel.MarkCapabilityObservation(
            MakeGpuDrivenObservation(true)));
        const std::vector<SampleAssessmentEvent> capabilityEvents =
            capabilityChannel.Drain();
        ASSERT_EQ(capabilityEvents.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<AssessmentCapabilityObservation>(
            capabilityEvents.front()));

        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        SampleAssessmentChannel& sampleChannel = session.GetChannel();
        ASSERT_TRUE(sampleChannel.MarkAction(
            AssessmentCode("CAMERA.ORBIT"), "Orbit the active camera."));
        const SampleAssessmentReport report = session.Finalize();
        ASSERT_EQ(report.actions.size(), 1u);
        EXPECT_EQ(report.actions.front().code.GetValue(), "CAMERA.ORBIT");
    }

    TEST(FrameworkAssessmentValidation, DroppedEventsProduceGatingFinding)
    {
        FrameworkAssessmentSession session(MakeEmptyContract(), MakeMetadata(), 1);
        ASSERT_TRUE(session.RecordCheckpoint(AssessmentCheckpoints::EngineBaseline));
        EXPECT_FALSE(session.RecordCheckpoint(AssessmentCheckpoints::ScenarioSetup));

        const SampleAssessmentReport report = session.Finalize();
        ASSERT_EQ(report.droppedEventCount, 1u);
        ASSERT_EQ(report.findings.size(), 1u);
        const Finding& finding = report.findings.front();
        EXPECT_EQ(finding.code.GetValue(), "INSTRUMENTATION.EVENTS_DROPPED");
        EXPECT_EQ(finding.classification, FindingClass::InstrumentationGap);
        EXPECT_TRUE(finding.gating);
        EXPECT_EQ(finding.confidence, FindingConfidence::Confirmed);
        EXPECT_EQ(finding.severity, FindingSeverity::Error);
        EXPECT_EQ(report.blockGrade, AssessmentBlockGrade::Blocked);
    }

    TEST(FrameworkAssessmentValidation, JsonIsDeterministicAfterOutOfOrderRecords)
    {
        FrameworkAssessmentSession first(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(first));
        ASSERT_TRUE(first.RecordMetadata("zeta", "last"));
        ASSERT_TRUE(first.RecordMetadata("alpha", "first"));
        ASSERT_TRUE(first.RecordCheckpoint(
            {AssessmentCode("SCENE.LOADED"), "Scene assets are available."}));
        ASSERT_TRUE(first.RecordAction(
            {AssessmentCode("CAMERA.ORBIT"), "Orbit the active camera."}));
        ASSERT_TRUE(first.RecordCapabilityObservation(MakeGpuDrivenObservation(
            false, {AssessmentCode("ACTION.APPLIED"),
                    "Qualified action applied."})));
        ASSERT_TRUE(first.RecordMetric(
            {AssessmentCode("STARTUP.READY"), "Sample reached ready state."},
            MakeMetric(2)));
        ASSERT_TRUE(first.RecordFinding(MakeFinding(FindingSeverity::Error)));

        FrameworkAssessmentSession second(MakeContract(), MakeMetadata());
        ASSERT_TRUE(RecordCompleteContract(second));
        ASSERT_TRUE(second.RecordFinding(MakeFinding(FindingSeverity::Error)));
        ASSERT_TRUE(second.RecordCapabilityObservation(MakeGpuDrivenObservation(
            false, {AssessmentCode("ACTION.APPLIED"),
                    "Qualified action applied."})));
        ASSERT_TRUE(second.RecordMetric(
            {AssessmentCode("STARTUP.READY"), "Sample reached ready state."},
            MakeMetric(2)));
        ASSERT_TRUE(second.RecordCheckpoint(
            {AssessmentCode("SCENE.LOADED"), "Scene assets are available."}));
        ASSERT_TRUE(second.RecordAction(
            {AssessmentCode("CAMERA.ORBIT"), "Orbit the active camera."}));
        ASSERT_TRUE(second.RecordMetadata("alpha", "first"));
        ASSERT_TRUE(second.RecordMetadata("zeta", "last"));

        const std::string firstJson = FrameworkAssessmentJsonWriter::ToJson(
            first.Finalize());
        const std::string secondJson = FrameworkAssessmentJsonWriter::ToJson(
            second.Finalize());
        EXPECT_EQ(firstJson, secondJson);
        EXPECT_NE(firstJson.find("\"schema\":\"RVX.FrameworkAssessmentReport\""),
                  std::string::npos);
        EXPECT_NE(firstJson.find("\"schemaVersion\":2"), std::string::npos);
        EXPECT_NE(firstJson.find("\"subsystemCode\":\"SCENE.HIERARCHY\""),
                  std::string::npos);
        EXPECT_NE(firstJson.find("\"artifactPaths\""), std::string::npos);
        EXPECT_NE(firstJson.find("\"capabilityObservations\""),
                  std::string::npos);
    }

    TEST(FrameworkAssessmentValidation,
         BaselineRequiresCompleteV2FingerprintAndIgnoresBuildProvenance)
    {
        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());

        AssessmentFingerprint rebuild = MakeFingerprint();
        rebuild.engineBuild = "build-456";
        EXPECT_TRUE(session.CompareBaseline(rebuild).IsComparable());

        AssessmentFingerprint mismatched = MakeFingerprint();
        mismatched.backend = "dx12";
        const BaselineComparisonResult mismatch = session.CompareBaseline(mismatched);
        EXPECT_EQ(mismatch.status,
                  BaselineComparisonStatus::IncompatibleFingerprint);
        EXPECT_FALSE(mismatch.IsComparable());

        AssessmentFingerprint incomplete = MakeFingerprint();
        incomplete.device.clear();
        const BaselineComparisonResult partial = session.CompareBaseline(incomplete);
        EXPECT_EQ(partial.status,
                  BaselineComparisonStatus::IncompatibleFingerprint);

        const BaselineComparisonResult matching =
            session.CompareBaseline(MakeFingerprint());
        EXPECT_EQ(matching.status,
                  BaselineComparisonStatus::CompatibleNoThresholds);
        EXPECT_TRUE(matching.IsComparable());
    }

    TEST(FrameworkAssessmentValidation,
         V2FingerprintRejectsAssetDeviceAndRenderConfigurationMismatch)
    {
        FrameworkAssessmentSession session(MakeContract(), MakeMetadata());
        AssessmentFingerprint assetMismatch = MakeFingerprint();
        assetMismatch.assetSet =
            "rvx-asset-set-v1|model:r7@source:self-contained-artifact:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:bytes=4:files=1";
        EXPECT_FALSE(session.CompareBaseline(assetMismatch).IsComparable());

        AssessmentFingerprint deviceMismatch = MakeFingerprint();
        deviceMismatch.device = "other-device-driver";
        EXPECT_FALSE(session.CompareBaseline(deviceMismatch).IsComparable());

        AssessmentFingerprint configurationMismatch = MakeFingerprint();
        configurationMismatch.renderConfiguration = "1920x1080-default";
        EXPECT_FALSE(
            session.CompareBaseline(configurationMismatch).IsComparable());
    }

    TEST(FrameworkAssessmentValidation, VerifiedAssetSetBindsOnceBeforeFinalize)
    {
        AssessmentMetadata metadata = MakeMetadata();
        metadata.fingerprint.assetSet.clear();
        FrameworkAssessmentSession session(MakeContract(), std::move(metadata));

        const std::string assetSet =
            "rvx-asset-set-v1|model:r7-triangle@source:self-contained-artifact:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:bytes=4:files=1";
        EXPECT_TRUE(session.BindVerifiedAssetSet(assetSet));
        EXPECT_TRUE(session.BindVerifiedAssetSet(assetSet));
        EXPECT_FALSE(session.BindVerifiedAssetSet("rvx-asset-set-v1|other"));
        EXPECT_FALSE(session.BindVerifiedAssetSet({}));
        static_cast<void>(session.Finalize());
        EXPECT_FALSE(session.BindVerifiedAssetSet(assetSet));
    }

    TEST(FrameworkAssessmentValidation,
         FingerprintRejectsUnavailableSentinelsWithoutRejectingRealValues)
    {
        const AssessmentFingerprint complete = MakeFingerprint();
        EXPECT_TRUE(complete.IsComplete());

        AssessmentFingerprint unavailable = complete;
        unavailable.backend = "unavailable";
        EXPECT_FALSE(unavailable.IsComplete());

        AssessmentFingerprint unavailableContent = complete;
        unavailableContent.assetSet = "fixture-r1#content=unavailable";
        EXPECT_FALSE(unavailableContent.IsComplete());

        AssessmentFingerprint realValue = complete;
        realValue.device = "unavailable-but-real-device-name";
        EXPECT_TRUE(realValue.IsComplete());

        AssessmentFingerprint missingDriver = complete;
        missingDriver.device = "adapter|driver-unavailable";
        EXPECT_FALSE(missingDriver.IsComplete());
    }
} // namespace RVX
