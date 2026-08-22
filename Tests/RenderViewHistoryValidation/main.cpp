#include "Runtime/RenderViewHistoryCoordinator.h"

#include <gtest/gtest.h>

namespace RVX
{
namespace
{
    [[nodiscard]] RenderExtractionDiagnostics CompleteDiagnostics()
    {
        RenderExtractionDiagnostics diagnostics;
        diagnostics.complete = true;
        return diagnostics;
    }

    struct FrameOptions
    {
        uint64 sequence = 1;
        uint64 cameraCutRevision = 1;
        uint64 temporalEpoch = 1;
        uint64 surfaceGeneration = 1;
        uint64 configurationRevision = 1;
        uint64 familyId = 1;
        uint64 cameraEntityPacked = 0x0000000300000002ULL;
        uint32 width = 320;
        uint32 height = 180;
    };

    [[nodiscard]] std::unique_ptr<const RenderFramePacketV5> MakeFrame(
        const FrameOptions& options)
    {
        RenderFrameHeaderV5 header;
        header.sequence = options.sequence;
        header.requiredSceneRevision = 1;
        header.worldRevision = 1;
        header.temporalEpoch = options.temporalEpoch;
        header.viewFamilyId = options.familyId;
        header.surfaceGeneration = options.surfaceGeneration;
        header.configurationRevision = options.configurationRevision;
        header.formatRevision = 1;
        header.simulationTick = options.sequence - 1;
        header.deterministicSeed =
            0x243f6a88U ^ static_cast<uint32>(options.sequence);
        header.outputWidth = options.width;
        header.outputHeight = options.height;

        RenderViewSnapshot view;
        view.identity = {
            .sceneRuntimeId = 11,
            .cameraEntityPacked = options.cameraEntityPacked,
            .subViewIndex = 0,
            .role = ViewRole::Main,
        };
        view.cameraCutRevision = options.cameraCutRevision;
        view.viewportWidth = header.outputWidth;
        view.viewportHeight = header.outputHeight;
        view.absoluteTime = static_cast<float64>(options.sequence - 1) / 60.0;
        view.deltaTime = 1.0f / 60.0f;

        return RenderFramePacketV5::Create(
            header,
            view,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            CompleteDiagnostics());
    }

    [[nodiscard]] FrameOptions MakeOptions(uint64 sequence)
    {
        FrameOptions options;
        options.sequence = sequence;
        return options;
    }

    TEST(RenderViewHistoryValidation,
         OnlyMatchingPresentedCandidateAdvancesCommittedHistory)
    {
        RenderViewHistoryCoordinator coordinator;
        const auto first = MakeFrame(MakeOptions(1));
        ASSERT_NE(first, nullptr);

        const RenderHistoryAvailability firstDecision =
            coordinator.Evaluate(*first, true);
        EXPECT_FALSE(firstDecision.available);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            firstDecision.resetReasons, RenderHistoryResetReason::FirstFrame));
        EXPECT_TRUE(coordinator.CommitPresented(first->GetHistoryIdentity()));

        FrameOptions cutOptions = MakeOptions(2);
        cutOptions.cameraCutRevision = 2;
        cutOptions.temporalEpoch = 2;
        const auto cutCandidate = MakeFrame(cutOptions);
        ASSERT_NE(cutCandidate, nullptr);
        const RenderHistoryAvailability cutDecision =
            coordinator.Evaluate(*cutCandidate, true);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            cutDecision.resetReasons, RenderHistoryResetReason::CameraCut));
        coordinator.Abandon(cutCandidate->GetHistoryIdentity());

        const auto compatible = MakeFrame(MakeOptions(3));
        ASSERT_NE(compatible, nullptr);
        const RenderHistoryAvailability compatibleDecision =
            coordinator.Evaluate(*compatible, true);
        EXPECT_TRUE(compatibleDecision.available);
        EXPECT_EQ(coordinator.GetLastPresentedIdentity()->previousFrame.frameSequence,
                  1U);
        EXPECT_TRUE(
            coordinator.CommitPresented(compatible->GetHistoryIdentity()));
        EXPECT_EQ(coordinator.GetLastPresentedIdentity()->previousFrame.frameSequence,
                  3U);
    }

TEST(RenderViewHistoryValidation,
     PendingResetAndMissingResourceRemainFailClosedUntilPresentation)
    {
        RenderViewHistoryCoordinator coordinator;
        const auto first = MakeFrame(MakeOptions(1));
        ASSERT_NE(first, nullptr);
        EXPECT_FALSE(coordinator.Evaluate(*first, true).available);
        EXPECT_TRUE(coordinator.CommitPresented(first->GetHistoryIdentity()));

        coordinator.RequestReset(RenderHistoryResetReason::Resize);
        const auto abandoned = MakeFrame(MakeOptions(2));
        ASSERT_NE(abandoned, nullptr);
        RenderHistoryAvailability decision = coordinator.Evaluate(*abandoned, true);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::Resize));
        coordinator.Abandon(abandoned->GetHistoryIdentity());

        const auto presented = MakeFrame(MakeOptions(3));
        ASSERT_NE(presented, nullptr);
        decision = coordinator.Evaluate(*presented, true);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons, RenderHistoryResetReason::Resize));
        EXPECT_TRUE(
            coordinator.CommitPresented(presented->GetHistoryIdentity()));

        const auto noResource = MakeFrame(MakeOptions(4));
        ASSERT_NE(noResource, nullptr);
        decision = coordinator.Evaluate(*noResource, false);
        EXPECT_FALSE(decision.available);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            decision.resetReasons,
            RenderHistoryResetReason::HistoryUnavailable));
        EXPECT_FALSE(coordinator.CommitPresented(first->GetHistoryIdentity()));
}

TEST(RenderViewHistoryValidation,
     ResetRequestedAfterEvaluationIsNotConsumedByThatPresentation)
{
    RenderViewHistoryCoordinator coordinator;
    const auto first = MakeFrame({.sequence = 1});
    ASSERT_NE(first, nullptr);
    const RenderHistoryAvailability firstDecision =
        coordinator.Evaluate(*first, false);
    ASSERT_TRUE(coordinator.CommitPresented(firstDecision.identity));

    const auto second = MakeFrame({.sequence = 2});
    ASSERT_NE(second, nullptr);
    const RenderHistoryAvailability secondDecision =
        coordinator.Evaluate(*second, true);
    ASSERT_TRUE(secondDecision.available);

    coordinator.RequestReset(RenderHistoryResetReason::CameraCut);
    ASSERT_TRUE(coordinator.CommitPresented(secondDecision.identity));
    EXPECT_TRUE(coordinator.HasPendingReset());

    const auto third = MakeFrame({.sequence = 3});
    ASSERT_NE(third, nullptr);
    const RenderHistoryAvailability thirdDecision =
        coordinator.Evaluate(*third, true);
    EXPECT_FALSE(thirdDecision.available);
    EXPECT_TRUE(HasRenderHistoryResetReason(
        thirdDecision.resetReasons, RenderHistoryResetReason::CameraCut));
    ASSERT_TRUE(coordinator.CommitPresented(thirdDecision.identity));
    EXPECT_FALSE(coordinator.HasPendingReset());
}

    TEST(RenderViewHistoryValidation,
         SubmissionWithoutPresentationAndAbandonedCandidatesDoNotAdvanceHistory)
    {
        RenderViewHistoryCoordinator coordinator;
        const auto submittedOnly = MakeFrame(MakeOptions(1));
        ASSERT_NE(submittedOnly, nullptr);
        EXPECT_FALSE(coordinator.Evaluate(*submittedOnly, true).available);

        // Submission does not have a coordinator API. Until the presentation
        // boundary calls CommitPresented(), the next candidate remains a first
        // frame and cannot borrow the submitted-only candidate's identity.
        const auto replacement = MakeFrame(MakeOptions(2));
        ASSERT_NE(replacement, nullptr);
        const RenderHistoryAvailability replacementDecision =
            coordinator.Evaluate(*replacement, true);
        EXPECT_FALSE(replacementDecision.available);
        EXPECT_TRUE(HasRenderHistoryResetReason(
            replacementDecision.resetReasons, RenderHistoryResetReason::FirstFrame));
        EXPECT_FALSE(
            coordinator.CommitPresented(submittedOnly->GetHistoryIdentity()));
        EXPECT_TRUE(
            coordinator.CommitPresented(replacement->GetHistoryIdentity()));

        const auto abandoned = MakeFrame(MakeOptions(3));
        ASSERT_NE(abandoned, nullptr);
        EXPECT_TRUE(coordinator.Evaluate(*abandoned, true).available);
        coordinator.Abandon(abandoned->GetHistoryIdentity());

        const auto successor = MakeFrame(MakeOptions(4));
        ASSERT_NE(successor, nullptr);
        EXPECT_TRUE(coordinator.Evaluate(*successor, true).available);
        EXPECT_TRUE(
            coordinator.CommitPresented(successor->GetHistoryIdentity()));
    }

    TEST(RenderViewHistoryValidation,
         PresentedHistoryRejectsCameraSurfaceResizeAndConfigurationChanges)
    {
        const auto assertReason = [](FrameOptions candidateOptions,
                                     RenderHistoryResetReason expectedReason)
        {
            RenderViewHistoryCoordinator coordinator;
            const auto baseline = MakeFrame(MakeOptions(1));
            ASSERT_NE(baseline, nullptr);
            ASSERT_TRUE(coordinator.CommitPresented(
                coordinator.Evaluate(*baseline, true).identity));

            const auto candidate = MakeFrame(candidateOptions);
            ASSERT_NE(candidate, nullptr);
            const RenderHistoryAvailability decision =
                coordinator.Evaluate(*candidate, true);
            EXPECT_FALSE(decision.available);
            EXPECT_TRUE(HasRenderHistoryResetReason(
                decision.resetReasons, expectedReason));
        };

        FrameOptions cameraCut = MakeOptions(2);
        cameraCut.cameraCutRevision = 2;
        assertReason(cameraCut, RenderHistoryResetReason::CameraCut);

        FrameOptions resized = MakeOptions(2);
        resized.width = 640;
        assertReason(resized, RenderHistoryResetReason::Resize);

        FrameOptions surfaceChanged = MakeOptions(2);
        surfaceChanged.surfaceGeneration = 2;
        assertReason(surfaceChanged, RenderHistoryResetReason::SurfaceChanged);

        FrameOptions configurationChanged = MakeOptions(2);
        configurationChanged.configurationRevision = 2;
        assertReason(configurationChanged,
                     RenderHistoryResetReason::ConfigurationChanged);
    }

    TEST(RenderViewHistoryValidation,
         HistoryCannotCrossViewOrViewFamilyBoundaries)
    {
        const auto assertReason = [](FrameOptions candidateOptions,
                                     RenderHistoryResetReason expectedReason)
        {
            RenderViewHistoryCoordinator coordinator;
            const auto baseline = MakeFrame(MakeOptions(1));
            ASSERT_NE(baseline, nullptr);
            ASSERT_TRUE(coordinator.CommitPresented(
                coordinator.Evaluate(*baseline, true).identity));

            const auto candidate = MakeFrame(candidateOptions);
            ASSERT_NE(candidate, nullptr);
            const RenderHistoryAvailability decision =
                coordinator.Evaluate(*candidate, true);
            EXPECT_FALSE(decision.available);
            EXPECT_TRUE(HasRenderHistoryResetReason(
                decision.resetReasons, expectedReason));
        };

        FrameOptions differentView = MakeOptions(2);
        differentView.cameraEntityPacked = 0x0000000500000004ULL;
        assertReason(differentView, RenderHistoryResetReason::ViewChanged);

        FrameOptions differentFamily = MakeOptions(2);
        differentFamily.familyId = 2;
        assertReason(differentFamily,
                     RenderHistoryResetReason::ViewFamilyChanged);
    }
} // namespace
} // namespace RVX
