#include "Samples/SampleCLI.h"
#include "RenderContracts/RenderExposure.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    [[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] std::filesystem::path GetSourceRoot()
    {
        const std::filesystem::path sourceFile{__FILE__};
        return sourceFile.parent_path().parent_path().parent_path();
    }

    RVX::SampleParticleVfxExecutionDiagnostics MakeQualifiedParticle(
        const bool smoke)
    {
        RVX::SampleParticleVfxExecutionDiagnostics diagnostics;
        diagnostics.available = true;
        diagnostics.executionReceiptSchemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
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
        diagnostics.qualifiedBatchReceiptCount = 1;
        diagnostics.distinctEffectDescriptorCount = 1;
        diagnostics.distinctTextureBindingCount = 1;
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

    TEST(ParticleVfxSampleValidation,
         CombinedFireAndSmokeRequiresTwoDistinctCompletionBoundEffects)
    {
        RVX::SampleParticleVfxExecutionDiagnostics diagnostics =
            MakeQualifiedParticle(false);
        diagnostics.itemCount = 2;
        diagnostics.cpuPacketReadyItemCount = 2;
        diagnostics.totalAliveParticles = 32;
        diagnostics.instanceCount = 32;
        diagnostics.drawCallCount = 2;
        diagnostics.batchReceiptCount = 2;
        diagnostics.textureResolvedBatchCount = 2;
        diagnostics.qualifiedBatchReceiptCount = 2;
        diagnostics.distinctEffectDescriptorCount = 2;
        diagnostics.distinctTextureBindingCount = 2;
        diagnostics.additiveBatchCount = 1;
        diagnostics.premultipliedBatchCount = 1;
        diagnostics.softDepthRequested = true;
        diagnostics.softDepthRequired = true;
        diagnostics.softDepthSnapshotReady = true;
        diagnostics.softDepthLinearizationReady = true;
        diagnostics.softDepthEnabled = true;
        EXPECT_TRUE(diagnostics.HasFireAndSmokeExecution());

        diagnostics.distinctEffectDescriptorCount = 1;
        EXPECT_FALSE(diagnostics.HasFireAndSmokeExecution());
        diagnostics.distinctEffectDescriptorCount = 2;
        diagnostics.distortionPassExecuted = false;
        EXPECT_FALSE(diagnostics.HasFireAndSmokeExecution());
        diagnostics.distortionPassExecuted = true;
        diagnostics.reactiveMaskWriteEnabled = false;
        EXPECT_FALSE(diagnostics.HasFireAndSmokeExecution());
    }

    TEST(ParticlePreExposureValidation,
         DirectAndWeightedParticleRadianceReconstructAtEV0AndEV85)
    {
        constexpr float authoredParticleRadiance = 6.0f;
        constexpr float froxelTransmittance = 0.4f;
        constexpr float premultipliedAlpha = 0.35f;
        for (const float ev100 : std::array<float, 2>{0.0f, 8.5f})
        {
            float producerPreExposure = 0.0f;
            ASSERT_TRUE(RVX::TryCalculateRenderExposureScale(
                ev100, 0.0f, producerPreExposure));
            ASSERT_GT(producerPreExposure, 0.0f);

            const float directPreExposed = authoredParticleRadiance *
                froxelTransmittance * producerPreExposure;
            const float premultipliedPreExposed = authoredParticleRadiance *
                premultipliedAlpha * froxelTransmittance * producerPreExposure;
            const float weightedPreExposed = authoredParticleRadiance *
                froxelTransmittance * producerPreExposure;
            EXPECT_NEAR(directPreExposed / producerPreExposure,
                        authoredParticleRadiance * froxelTransmittance,
                        1.0e-5f);
            EXPECT_NEAR(premultipliedPreExposed / producerPreExposure,
                        authoredParticleRadiance * premultipliedAlpha *
                            froxelTransmittance,
                        1.0e-5f);
            EXPECT_NEAR(weightedPreExposed / producerPreExposure,
                        authoredParticleRadiance * froxelTransmittance,
                        1.0e-5f);
        }
    }

    TEST(ParticlePreExposureValidation,
         GraphRecordAndDescriptorBindTheExactCommittedExposureIdentity)
    {
        const std::filesystem::path root = GetSourceRoot();
        const std::string recordContext = ReadTextFile(
            root / "Source/Runtime/Render/Include/Render/Passes/RenderPassRecordContext.h");
        const std::string deferredAdapter = ReadTextFile(
            root / "Source/Runtime/Render/Private/Features/DeferredRecipeFeatureAdapter.cpp");
        const std::string forwardAdapter = ReadTextFile(
            root / "Source/Runtime/Render/Private/Features/LegacyForwardFeatureAdapter.cpp");
        const std::string translucencyAdapter = ReadTextFile(
            root / "Source/Runtime/Render/Private/Features/TranslucencyFeatureAdapter.cpp");
        const std::string particle = ReadTextFile(
            root / "Source/Runtime/Render/Private/Passes/ParticleFeaturePass.cpp");
        const std::string shader = ReadTextFile(
            root / "Source/Runtime/Render/Shaders/Particle/ParticleBillboard.hlsl");

        EXPECT_NE(recordContext.find("struct ParticleVfxRecordInput"),
                  std::string::npos);
        EXPECT_NE(recordContext.find(
                      "SceneDepthSnapshotRecordInput softDepthSnapshot"),
                  std::string::npos);
        EXPECT_NE(recordContext.find("RGBufferHandle exposureState{};"),
                  std::string::npos);
        EXPECT_NE(recordContext.find("exposureStateContentHash != 0"),
                  std::string::npos);
        EXPECT_NE(translucencyAdapter.find(
                      "outInput.exposureState = context.view.exposureState;"),
                  std::string::npos);
        EXPECT_NE(translucencyAdapter.find(
                      "outInput.exposureStateContentHash = context.view.exposureStateContentHash;"),
                  std::string::npos);
        const std::string legacyParticleDepthCopyName =
            "ParticleSoftDepth" "SnapshotCopy";
        EXPECT_EQ(forwardAdapter.find(legacyParticleDepthCopyName),
                  std::string::npos);
        EXPECT_EQ(deferredAdapter.find(legacyParticleDepthCopyName),
                  std::string::npos);
        EXPECT_EQ(translucencyAdapter.find(legacyParticleDepthCopyName),
                  std::string::npos);
        EXPECT_NE(particle.find("RVX_PARTICLE_EXPOSURE_STATE_BINDING = 6u"),
                  std::string::npos);
        EXPECT_NE(particle.find("data.exposureStateReadHandle = builder.Read("),
                  std::string::npos);
        EXPECT_NE(particle.find(
                      "data.particleVfx.softDepthSnapshot.texture"),
                  std::string::npos);
        EXPECT_NE(particle.find("softDepthSnapshot.receipt->copied.load"),
                  std::string::npos);
        EXPECT_NE(particle.find("data.softDepthHandle = builder.Read("),
                  std::string::npos);
        EXPECT_NE(particle.find(
                      "Particle VFX committed exposure-state handle is stale"),
                  std::string::npos);
        EXPECT_NE(particle.find(
                      ".BindBuffer(RVX_PARTICLE_EXPOSURE_STATE_BINDING, exposureState)"),
                  std::string::npos);
        EXPECT_NE(shader.find("StructuredBuffer<RVXExposureState> gExposureState : register(t6, space0)"),
                  std::string::npos);
    }

    TEST(ParticlePreExposureValidation,
         HlslScalesAuthoredFireSmokeAndOitWithoutRescalingGraphInputs)
    {
        const std::filesystem::path root = GetSourceRoot();
        const std::string shader = ReadTextFile(
            root / "Source/Runtime/Render/Shaders/Particle/ParticleBillboard.hlsl");

        EXPECT_NE(shader.find("#include \"../Include/Exposure.hlsli\""),
                  std::string::npos);
        EXPECT_NE(shader.find("TryGetParticleProducerPreExposure"),
                  std::string::npos);
        EXPECT_NE(shader.find("GetParticleVolumetricTransmittance(input.Position) *\n        producerPreExposure"),
                  std::string::npos);
        EXPECT_NE(shader.find("RVXEncodeWeightedBlendedOIT("),
                  std::string::npos);
        EXPECT_EQ(shader.find("gFroxelVolumetricLighting.SampleLevel(\n        gLinearClamp, float3(uv, canonicalDepth), 0.0f) *"),
                  std::string::npos);
        EXPECT_NE(shader.find("discard;"), std::string::npos);
    }
} // namespace
