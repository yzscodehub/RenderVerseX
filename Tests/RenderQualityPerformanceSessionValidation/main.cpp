#include "Render/Qualification/RenderQualityPerformanceSession.h"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>

namespace
{
    RVX::RenderQualityPerformanceReceiptBinding MakeBinding(
        RVX::uint64 frameSequence,
        RVX::uint64 configurationRevision = 17)
    {
        RVX::RenderQualityPerformanceReceiptBinding binding;
        binding.frameIdentity = {
            frameSequence,
            1000 + frameSequence,
            5,
            configurationRevision};
        binding.viewIdentity = {
            41,
            0x0000000200000003ULL,
            0,
            RVX::ViewRole::Main};
        binding.receiptSchemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        binding.receiptDigest.front() = static_cast<RVX::uint8>(
            1U + frameSequence % 251U);
        binding.sourceCommit = std::string(40, 'a');
        binding.preset = "win_x64_gfx24_debug";
        binding.backend = "dx12";
        binding.recipe = RVX::RenderPipelineRecipe::DeferredQuality;
        binding.quality = RVX::RenderQualityTier::High;
        binding.profileHash = RVX::ComputeRenderPipelineProfileHash(
            binding.recipe, binding.quality);
        return binding;
    }

    RVX::RenderQualityPerformanceCpuTiming MakeCpuTiming(
        bool extractionAvailable = true)
    {
        using Duration = RVX::RenderQualityPerformanceDuration;
        RVX::RenderQualityPerformanceCpuTiming timing;
        timing.extraction = extractionAvailable
            ? Duration::Available(100)
            : Duration::Unavailable();
        timing.visibilityAndDrawPlanning = Duration::Available(200);
        timing.frameApply = Duration::Available(300);
        timing.frameBeginAcquire = Duration::Available(400);
        timing.renderPrepare = Duration::Available(500);
        timing.policy = Duration::Available(600);
        timing.graphBuild = Duration::Available(700);
        timing.graphCompile = Duration::Available(800);
        timing.graphRealizeRecord = Duration::Available(900);
        timing.frameEndSubmit = Duration::Available(1000);
        timing.present = Duration::Available(1100);
        timing.acceptedFrameTotal = Duration::Available(6500);
        return timing;
    }

    RVX::RenderQualityPerformanceFrameTelemetry MakeTelemetry(
        RVX::uint64 frameSequence,
        RVX::uint64 cumulativeTrackedUploadBytes = 0,
        RVX::uint64 frameTrackedUploadBytes = 0,
        RVX::uint64 configurationRevision = 17)
    {
        RVX::RenderQualityPerformanceFrameTelemetry telemetry;
        telemetry.frameIdentity = {
            frameSequence,
            1000 + frameSequence,
            5,
            configurationRevision};
        telemetry.trackedUploadCoverageMask =
            RVX::RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE;
        telemetry.cumulativeTrackedUploadBytes = cumulativeTrackedUploadBytes;
        telemetry.frameTrackedUploadBytes = frameTrackedUploadBytes;
        telemetry.cumulativeTrackedUploadBytesAvailable = true;
        telemetry.trackedEstimatedFootprint.coverageMask =
            RVX::RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE;
        telemetry.trackedEstimatedFootprint
            .transientPoolPhysicalEstimatedBytes = 4096;
        telemetry.trackedEstimatedFootprint
            .importedHistoryNamedEstimatedBytes = 2048;
        telemetry.trackedEstimatedFootprint
            .resourceRegistryResidentEstimatedBytes = 8192;
        telemetry.trackedEstimatedFootprint.available = true;
        telemetry.resourceCounts.transientTextureCount = 3;
        telemetry.resourceCounts.transientBufferCount = 2;
        telemetry.resourceCounts.importedTextureCount = 7;
        telemetry.resourceCounts.importedBufferCount = 1;
        telemetry.resourceCounts.residentMeshCount = 4;
        telemetry.resourceCounts.residentTextureCount = 5;
        telemetry.resourceCounts.residentMaterialCount = 6;
        telemetry.resourceCounts.available = true;
        return telemetry;
    }

    std::array<RVX::RenderGraphPassTimingSample, 2> MakePassTimings(
        RVX::uint64 timestampBase = 1000,
        const char* secondPass = "Lighting")
    {
        return {{
            {0, "Depth", timestampBase, timestampBase + 100, 100, 0.1},
            {1,
             secondPass,
             timestampBase + 150,
             timestampBase + 350,
             200,
             0.2},
        }};
    }

    RVX::RenderGpuFrameTimingSample MakeGraphicsTerminalCommandSpan(
        RVX::uint64 frameSequence,
        RVX::uint64 completionValue,
        RVX::uint64 timestampFrequency = 1'000'000)
    {
        return {
            frameSequence,
            completionValue,
            10'000 + completionValue * 10,
            10'000 + completionValue * 10 + 400,
            400,
            timestampFrequency,
            64,
            0.4};
    }

    bool Commit(
        RVX::RenderQualityPerformanceSession& session,
        RVX::uint32 ordinal,
        const RVX::RenderQualityPerformanceReceiptBinding& binding,
        const RVX::RenderQualityPerformanceCpuTiming& cpu,
        std::span<const RVX::RenderGraphPassTimingSample> timings)
    {
        return RVX::RenderQualityPerformanceSessionTestAccess::
            CommitCompletedSample(
                session,
                ordinal,
                binding,
                cpu,
                MakeTelemetry(
                    binding.frameIdentity.frameSequence,
                    1024 + ordinal * 64,
                    48,
                    binding.frameIdentity.configurationRevision),
                ordinal + 1,
                ordinal + 1,
                100 + ordinal,
                1'000'000,
                timings);
    }
} // namespace

TEST(RenderQualityPerformanceSessionValidation,
     PublishesOnlyAfterExactlySixtyWarmupAndThreeHundredMeasuredCompletions)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));

    for (RVX::uint32 ordinal = 0;
         ordinal < RVX::RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT +
                       RVX::RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT;
         ++ordinal)
    {
        const auto timings = MakePassTimings(1000 + ordinal * 1000);
        ASSERT_TRUE(Commit(
            session,
            ordinal,
            MakeBinding(ordinal + 1),
            MakeCpuTiming(),
            timings));
    }

    const auto& diagnostics = session.GetDiagnostics();
    EXPECT_EQ(diagnostics.state,
              RVX::RenderQualityPerformanceSessionState::Completed);
    EXPECT_EQ(diagnostics.failure,
              RVX::RenderQualityPerformanceFailure::None);
    EXPECT_EQ(diagnostics.completedWarmupSamples, 60u);
    EXPECT_EQ(diagnostics.completedMeasuredSamples, 300u);

    const auto evidence = session.GetCompletedEvidence();
    ASSERT_NE(evidence, nullptr);
    ASSERT_TRUE(evidence->IsComplete());
    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
                    PublishCompletedEvidence(session));
    EXPECT_EQ(evidence, session.GetCompletedEvidence());
    ASSERT_EQ(evidence->samples.size(), 300u);
    EXPECT_EQ(evidence->samples.front().sampleIndex, 0u);
    EXPECT_EQ(evidence->samples.front().binding.frameIdentity.frameSequence,
              61u);
    EXPECT_EQ(evidence->samples.back().sampleIndex, 299u);
    EXPECT_EQ(evidence->samples.back().binding.frameIdentity.frameSequence,
              360u);
    EXPECT_EQ(evidence->samples.front().passSchemaHash,
              evidence->passSchemaHash);
    EXPECT_EQ(evidence->hardware.backend, "dx12");
    EXPECT_EQ(evidence->hardware.capabilityReportSha256,
              std::string(64, 'a'));
    EXPECT_EQ(evidence->samples.front().trackedUploadCoverageMask,
              RVX::RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE);
    EXPECT_EQ(evidence->samples.front().trackedUploadBytes, 112U);
    EXPECT_TRUE(evidence->samples.front()
                    .trackedEstimatedFootprint.IsValid());
    EXPECT_NE(evidence->samples.front()
                  .graphicsTerminalCommandSpanStartTimestamp,
              0u);
    EXPECT_EQ(evidence->samples.front()
                  .graphicsTerminalCommandSpanElapsedTimestampTicks,
              400u);
    EXPECT_DOUBLE_EQ(evidence->samples.front()
                         .graphicsTerminalCommandSpanMilliseconds,
                     0.4);
}

TEST(RenderQualityPerformanceSessionValidation,
     MissingUpdateOwnedExtractionTimingRejectsRequiredSession)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();

    EXPECT_FALSE(Commit(
        session, 0, MakeBinding(1), MakeCpuTiming(false), timings));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ExtractionTimingUnavailable);
    EXPECT_EQ(session.GetCompletedEvidence(), nullptr);
}

TEST(RenderQualityPerformanceSessionValidation,
     NonZeroEqualTimestampPairIsAValidSubResolutionPass)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    auto timings = MakePassTimings();
    timings[0].endTimestamp = timings[0].startTimestamp;
    timings[0].elapsedTimestampTicks = 0;
    timings[0].durationMilliseconds = 0.0;

    EXPECT_TRUE(Commit(
        session, 0, MakeBinding(1), MakeCpuTiming(), timings));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Collecting);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::None);
}

TEST(RenderQualityPerformanceSessionValidation,
     MissingGraphicsTerminalCommandSpanRejectsCompletedSample)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto binding = MakeBinding(1);
    const RVX::RenderGpuFrameTimingSample missingSpan;

    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSampleWithGraphicsTerminal(
            session,
            0,
            binding,
            MakeCpuTiming(),
            MakeTelemetry(1, 1024),
            1,
            1,
            100,
            1'000'000,
            missingSpan,
            MakePassTimings()));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::SampleCountMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     WrappedGraphicsTerminalCommandSpanUsesItsDeclaredTimestampDomain)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto binding = MakeBinding(1);
    auto terminalSpan = MakeGraphicsTerminalCommandSpan(1, 100);
    terminalSpan.startTimestamp = 65'530;
    terminalSpan.endTimestamp = 5;
    terminalSpan.elapsedTimestampTicks = 11;
    terminalSpan.timestampValidBits = 16;
    terminalSpan.terminalCriticalPathMilliseconds = 0.011;

    EXPECT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSampleWithGraphicsTerminal(
            session,
            0,
            binding,
            MakeCpuTiming(),
            MakeTelemetry(1, 1024),
            1,
            1,
            100,
            1'000'000,
            terminalSpan,
            MakePassTimings()));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Collecting);
}

TEST(RenderQualityPerformanceSessionValidation,
     WrongGraphicsTerminalSourceFrameRejectsCompletedSample)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto binding = MakeBinding(1);
    auto terminalSpan = MakeGraphicsTerminalCommandSpan(2, 100);

    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSampleWithGraphicsTerminal(
            session,
            0,
            binding,
            MakeCpuTiming(),
            MakeTelemetry(1, 1024),
            1,
            1,
            100,
            1'000'000,
            terminalSpan,
            MakePassTimings()));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::SampleCountMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     DuplicateOrWrongGraphicsTerminalBatchSpanRejectsSession)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    RVX::RenderQualityPerformanceSessionTestAccess::StageSyntheticSubmittedSlot(
        session, 77, 900);

    RVX::RenderGpuFrameTimingSampleBatch batch;
    batch.count = 1;
    batch.samples[0] = MakeGraphicsTerminalCommandSpan(77, 900);
    ASSERT_TRUE(session.BindGraphicsTerminalCommandSpans(batch));
    EXPECT_FALSE(session.BindGraphicsTerminalCommandSpans(batch));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptMismatch);

    RVX::RenderQualityPerformanceSession wrongCompletion;
    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(
        wrongCompletion));
    RVX::RenderQualityPerformanceSessionTestAccess::StageSyntheticSubmittedSlot(
        wrongCompletion, 78, 901);
    batch.samples[0] = MakeGraphicsTerminalCommandSpan(78, 902);
    EXPECT_FALSE(wrongCompletion.BindGraphicsTerminalCommandSpans(batch));
    EXPECT_EQ(wrongCompletion.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     DelayedGraphicsTerminalSpanKeepsCompletedPassTimingCompletionOwned)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    RVX::RenderQualityPerformanceSessionTestAccess::
        StageSyntheticCompletedPassTimingSlot(session, 77, 900);

    // The pass query owner has completed, but RenderContext has not exposed
    // the matching source-bound terminal span in this polling round yet.
    RVX::RenderQualityPerformanceSessionTestAccess::PollSyntheticCompletion(
        session);
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Collecting);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::None);
    EXPECT_TRUE(session.HasPendingCompletion());
    EXPECT_EQ(session.GetDiagnostics().completedWarmupSamples, 0u);

    RVX::RenderGpuFrameTimingSampleBatch batch;
    batch.count = 1;
    batch.samples[0] = MakeGraphicsTerminalCommandSpan(77, 900);
    ASSERT_TRUE(session.BindGraphicsTerminalCommandSpans(batch));
    RVX::RenderQualityPerformanceSessionTestAccess::PollSyntheticCompletion(
        session);

    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Collecting);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::None);
    EXPECT_EQ(session.GetDiagnostics().completedWarmupSamples, 1u);
    EXPECT_FALSE(session.HasPendingCompletion());
}

TEST(RenderQualityPerformanceSessionValidation,
     DeviceLostShutdownRejectsActiveSessionButPreservesCompletedEvidence)
{
    RVX::RenderQualityPerformanceSession active;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(active));
    active.Shutdown(true);
    EXPECT_EQ(active.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(active.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::DeviceLost);

    RVX::RenderQualityPerformanceSession completed;
    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(
        completed));
    for (RVX::uint32 ordinal = 0;
         ordinal < RVX::RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT +
                       RVX::RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT;
         ++ordinal)
    {
        ASSERT_TRUE(Commit(
            completed,
            ordinal,
            MakeBinding(ordinal + 1),
            MakeCpuTiming(),
            MakePassTimings(1000 + ordinal * 1000)));
    }
    const auto evidence = completed.GetCompletedEvidence();
    ASSERT_NE(evidence, nullptr);
    completed.Shutdown(true);
    EXPECT_EQ(completed.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Completed);
    EXPECT_EQ(completed.GetCompletedEvidence(), evidence);
}

TEST(RenderQualityPerformanceSessionValidation,
     PassNameOrOrderChangeRejectsWholeSession)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto stable = MakePassTimings();
    ASSERT_TRUE(Commit(
        session, 0, MakeBinding(1), MakeCpuTiming(), stable));

    const auto changed = MakePassTimings(2000, "Reflections");
    EXPECT_FALSE(Commit(
        session, 1, MakeBinding(2), MakeCpuTiming(), changed));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::PassSchemaChanged);
}

TEST(RenderQualityPerformanceSessionValidation,
     ConfigurationChangeRejectsCrossConfigurationAggregation)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();
    ASSERT_TRUE(Commit(
        session, 0, MakeBinding(1, 17), MakeCpuTiming(), timings));

    EXPECT_FALSE(Commit(
        session, 1, MakeBinding(2, 18), MakeCpuTiming(), timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     BuildOrBackendChangeRejectsCrossSessionAggregation)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();
    ASSERT_TRUE(Commit(session, 0, MakeBinding(1), MakeCpuTiming(), timings));

    auto changed = MakeBinding(2);
    changed.backend = "vulkan";
    EXPECT_FALSE(Commit(session, 1, changed, MakeCpuTiming(), timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     DuplicateCompletionOrdinalFailsClosed)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();
    ASSERT_TRUE(Commit(
        session, 0, MakeBinding(1), MakeCpuTiming(), timings));

    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            MakeBinding(2),
            MakeCpuTiming(),
            MakeTelemetry(2, 1088),
            2,
            2,
            101,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::SampleCountMismatch);
}

TEST(RenderQualityPerformanceSessionValidation,
     PreallocatedSlotSaturationRejectsInsteadOfDroppingSample)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    RVX::RenderQualityPerformanceSessionTestAccess::SaturateSlots(session);

    EXPECT_FALSE(
        RVX::RenderQualityPerformanceSessionTestAccess::RejectSaturatedFrame(
            session));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::SlotSaturated);
    EXPECT_EQ(session.GetDiagnostics().inFlightSlots,
              RVX::RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT);
    EXPECT_EQ(session.GetCompletedEvidence(), nullptr);
}

TEST(RenderQualityPerformanceSessionValidation,
     MissingSameFrameTelemetryRejectsRequiredSession)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto binding = MakeBinding(1);
    const auto timings = MakePassTimings();
    RVX::RenderQualityPerformanceFrameTelemetry telemetry;

    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            binding,
            MakeCpuTiming(),
            telemetry,
            1,
            1,
            100,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::TelemetryUnavailable);
}

TEST(RenderQualityPerformanceSessionValidation,
     ResourceCountChangeAfterWarmupRejectsMeasurementEvidence)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();
    const auto firstBinding = MakeBinding(61);
    const auto secondBinding = MakeBinding(62);
    const auto stableTelemetry = MakeTelemetry(61, 1024);
    auto changedTelemetry = MakeTelemetry(62, 1088);
    ++changedTelemetry.resourceCounts.residentTextureCount;

    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            60,
            firstBinding,
            MakeCpuTiming(),
            stableTelemetry,
            61,
            61,
            160,
            1'000'000,
            timings));
    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            61,
            secondBinding,
            MakeCpuTiming(),
            changedTelemetry,
            62,
            62,
            161,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ResourceCountsChanged);
}

TEST(RenderQualityPerformanceSessionValidation,
     DuplicateOrRetrogradeCompletionFrameRejectsEvidence)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();
    const auto firstBinding = MakeBinding(1);
    const auto secondBinding = MakeBinding(2);

    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            firstBinding,
            MakeCpuTiming(),
            MakeTelemetry(1, 1024),
            1,
            1,
            100,
            1'000'000,
            timings));
    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            1,
            secondBinding,
            MakeCpuTiming(),
            MakeTelemetry(2, 1088),
            1,
            1,
            101,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::FrameOrdinalInvalid);
}

TEST(RenderQualityPerformanceSessionValidation,
     CompletionFrameMustNotPrecedeSubmittedFrame)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();

    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            MakeBinding(1),
            MakeCpuTiming(),
            MakeTelemetry(1, 1024),
            2,
            1,
            100,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::FrameOrdinalInvalid);
}

TEST(RenderQualityPerformanceSessionValidation,
     FirstFrameUsesArmedBaselineAndCombinesTrackedFrameUploads)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(
        session, 4096));
    const auto timings = MakePassTimings();

    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            MakeBinding(1),
            MakeCpuTiming(),
            MakeTelemetry(1, 4096, 96),
            1,
            1,
            100,
            1'000'000,
            timings));
    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            1,
            MakeBinding(2),
            MakeCpuTiming(),
            MakeTelemetry(2, 4128, 64),
            2,
            2,
            101,
            1'000'000,
            timings));

    // The first two slots are warmup, but their synthetic path still verifies
    // the exact baseline/delta arithmetic used before completion ownership.
    EXPECT_EQ(session.GetDiagnostics().completedWarmupSamples, 2U);
}

TEST(RenderQualityPerformanceSessionValidation,
     TrackedCumulativeUploadCounterDecreaseRejectsImmediately)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    const auto timings = MakePassTimings();

    ASSERT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            0,
            MakeBinding(1),
            MakeCpuTiming(),
            MakeTelemetry(1, 1000, 20),
            1,
            1,
            100,
            1'000'000,
            timings));
    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::
        CommitCompletedSample(
            session,
            1,
            MakeBinding(2),
            MakeCpuTiming(),
            MakeTelemetry(2, 999, 20),
            2,
            2,
            101,
            1'000'000,
            timings));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::UploadCounterDecreased);
}

TEST(RenderQualityPerformanceSessionValidation,
     AdmissionFailureIsTerminalInsteadOfLeavingAnIdleSession)
{
    RVX::RenderQualityPerformanceSession session;

    session.RejectAdmission(
        RVX::RenderQualityPerformanceFailure::TelemetryUnavailable,
        "synthetic admission telemetry was unavailable");

    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::TelemetryUnavailable);
    EXPECT_EQ(session.GetCompletedEvidence(), nullptr);
    EXPECT_FALSE(RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(
        session));
}

TEST(RenderQualityPerformanceSessionValidation,
     ExecutionReceiptFailureRemainsTheTerminalDiagnosticAfterCpuBinding)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    RVX::RenderQualityPerformanceSessionTestAccess::StageSyntheticRecordingSlot(
        session);

    // The default receipt intentionally lacks a successful graph execution.
    // RenderSubsystem still binds CPU timing after the corresponding normal
    // Present path, so the earlier receipt error must stay observable.
    const RVX::RenderExecutionReceipt invalidReceipt;
    EXPECT_FALSE(session.BindExecutionReceipt(invalidReceipt));
    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptUnavailable);
    EXPECT_EQ(session.GetDiagnostics().diagnostic,
              "The sampled graph did not complete execution successfully.");

    EXPECT_FALSE(session.RecordCpuTiming(
        {1, 1, 1, 1}, MakeCpuTiming()));
    EXPECT_EQ(session.GetDiagnostics().failure,
              RVX::RenderQualityPerformanceFailure::ReceiptUnavailable);
    EXPECT_EQ(session.GetDiagnostics().diagnostic,
              "The sampled graph did not complete execution successfully.");
}

TEST(RenderQualityPerformanceSessionValidation,
     RejectedUnsubmittedSlotReleasesItsPerFrameBindingsForRingReuse)
{
    RVX::RenderQualityPerformanceSession session;
    ASSERT_TRUE(
        RVX::RenderQualityPerformanceSessionTestAccess::ArmSynthetic(session));
    RVX::RenderQualityPerformanceSessionTestAccess::StageSyntheticRecordingSlot(
        session);

    session.AbortCurrentFrame(
        RVX::RenderQualityPerformanceFailure::SubmissionRejected,
        "synthetic graph submission was rejected");

    EXPECT_EQ(session.GetDiagnostics().state,
              RVX::RenderQualityPerformanceSessionState::Rejected);
    EXPECT_EQ(session.GetDiagnostics().inFlightSlots, 0U);
    EXPECT_TRUE(RVX::RenderQualityPerformanceSessionTestAccess::
        IsSyntheticSlotReusable(session));
}
