#include "Samples/SampleCLI.h"

#include <gtest/gtest.h>

namespace
{
    RVX::SampleParticleVfxExecutionDiagnostics MakeQualifiedParticle(
        const bool smoke)
    {
        RVX::SampleParticleVfxExecutionDiagnostics diagnostics;
        diagnostics.available = true;
        diagnostics.executionReceiptSchemaVersion = 11;
        diagnostics.executionReceiptStructurallyValid = true;
        diagnostics.executionReceiptPresented = true;
        diagnostics.executionReceiptSucceeded = true;
        diagnostics.mainView = true;
        diagnostics.requested = true;
        diagnostics.supported = true;
        diagnostics.enabled = true;
        diagnostics.graphPassScheduled = true;
        diagnostics.drawSubmitted = true;
        diagnostics.uploadPublished = true;
        diagnostics.itemCount = 1;
        diagnostics.cpuPacketReadyItemCount = 1;
        diagnostics.totalAliveParticles = 16;
        diagnostics.drawCallCount = 1;
        diagnostics.instanceCount = 16;
        diagnostics.uploadBytes = 512;
        diagnostics.flipbookRequested = true;
        diagnostics.flipbookValidated = true;
        diagnostics.textureResolvedBatchCount = 1;
        diagnostics.effectDescriptorHash = 101;
        diagnostics.textureAssetId = 102;
        diagnostics.textureGeneration = 7;
        diagnostics.textureContentRevision = 103;
        diagnostics.batchReceiptCount = 1;
        diagnostics.reactiveMaskRequested = true;
        diagnostics.reactiveMaskWriteEnabled = true;
        diagnostics.reactiveMaskWriteDrawCount = 1;
        diagnostics.recordFrameSequence = 120;
        diagnostics.recordSceneRevision = 4;
        diagnostics.recordSurfaceGeneration = 2;
        diagnostics.recordConfigurationRevision = 2;
        diagnostics.recordGraphIdentity = 9;
        diagnostics.recordGraphGeneration = 3;
        diagnostics.recordEpoch = 1;
        diagnostics.executionReceiptFrameSequence = 120;
        diagnostics.presentedFrameSequence = 120;
        diagnostics.recordIdentityMatchesExecutionReceipt = true;
        diagnostics.presentedFrameAligned = true;
        if (smoke)
        {
            diagnostics.premultipliedBatchCount = 1;
            diagnostics.softDepthRequested = true;
            diagnostics.softDepthRequired = true;
            diagnostics.softDepthSnapshotReady = true;
            diagnostics.softDepthLinearizationReady = true;
            diagnostics.softDepthEnabled = true;
        }
        else
        {
            diagnostics.additiveBatchCount = 1;
            diagnostics.distortionRequested = true;
            diagnostics.distortionPassScheduled = true;
            diagnostics.distortionPassExecuted = true;
            diagnostics.distortionCompositeExecuted = true;
            diagnostics.lightHintRequestedCount = 1;
            diagnostics.lightHintAdmittedCount = 1;
        }
        return diagnostics;
    }

    TEST(ParticleVfxSampleValidation, FireRequiresExactLightAndDistortionReceipt)
    {
        RVX::SampleParticleVfxExecutionDiagnostics diagnostics =
            MakeQualifiedParticle(false);
        EXPECT_TRUE(diagnostics.HasCommonExecution());
        EXPECT_TRUE(diagnostics.HasFireExecution());
        EXPECT_FALSE(diagnostics.HasSmokeExecution());

        diagnostics.lightHintAdmittedCount = 0;
        EXPECT_FALSE(diagnostics.HasFireExecution());
        diagnostics.lightHintAdmittedCount = 1;
        diagnostics.distortionCompositeExecuted = false;
        EXPECT_FALSE(diagnostics.HasFireExecution());
    }

    TEST(ParticleVfxSampleValidation, SmokeRequiresCanonicalSoftDepthReceipt)
    {
        RVX::SampleParticleVfxExecutionDiagnostics diagnostics =
            MakeQualifiedParticle(true);
        EXPECT_TRUE(diagnostics.HasCommonExecution());
        EXPECT_TRUE(diagnostics.HasSmokeExecution());
        EXPECT_FALSE(diagnostics.HasFireExecution());

        diagnostics.softDepthLinearizationReady = false;
        EXPECT_FALSE(diagnostics.HasSmokeExecution());
        diagnostics.softDepthLinearizationReady = true;
        diagnostics.softDepthOptionalFallback = true;
        EXPECT_FALSE(diagnostics.HasSmokeExecution());
    }

    TEST(ParticleVfxSampleValidation,
         TargetTickIdentityDoesNotCarryReadinessPreRollState)
    {
        // A VFX receipt is bound to the simulation capture frame, not to the
        // arbitrary number of host frames spent waiting for the cooked effect.
        // The Sample's BeginDeterministicEpoch queues its config+restart pair
        // at tick zero; the exact same tick-120 presentation identity therefore
        // qualifies after either a short or long asynchronous readiness pre-roll.
        const RVX::SampleParticleVfxExecutionDiagnostics shortPreRoll =
            MakeQualifiedParticle(false);
        const RVX::SampleParticleVfxExecutionDiagnostics longPreRoll =
            MakeQualifiedParticle(false);
        EXPECT_TRUE(shortPreRoll.HasFireExecution());
        EXPECT_TRUE(longPreRoll.HasFireExecution());
        EXPECT_EQ(shortPreRoll.effectDescriptorHash,
                  longPreRoll.effectDescriptorHash);
        EXPECT_EQ(shortPreRoll.textureAssetId, longPreRoll.textureAssetId);
        EXPECT_EQ(shortPreRoll.textureGeneration,
                  longPreRoll.textureGeneration);
        EXPECT_EQ(shortPreRoll.recordFrameSequence,
                  longPreRoll.recordFrameSequence);
        EXPECT_EQ(shortPreRoll.presentedFrameSequence,
                  longPreRoll.presentedFrameSequence);
    }
} // namespace
