#include "Scenes/RenderQualityLabSample.h"

#include "Samples/SampleCLI.h"
#include "Samples/SampleInfo.h"
#include "Samples/SampleRenderQualityDiagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace RVX
{
    /** @brief Narrow value-only test access; no runtime renderer object leaks. */
    class RenderQualityLabSampleValidationAccess final
    {
    public:
        static bool BuildDeterministicModelSlots(
            std::size_t modelCount,
            std::vector<Vec3>& outSlots) noexcept
        {
            return RenderQualityLabSample::BuildDeterministicModelSlots(
                modelCount, outSlots);
        }

        static bool GetDeterministicParticleInstanceKeys(
            uint64& outFireKey,
            uint64& outSmokeKey) noexcept
        {
            return RenderQualityLabSample::GetDeterministicParticleInstanceKeys(
                outFireKey, outSmokeKey);
        }
    };
} // namespace RVX

namespace
{
    constexpr std::array<std::string_view, 3> ExpectedAdditionalModels{
        "transparent-materials-v1",
        "water-surface-reference-fixture-v1",
        "particle-vfx-ground-caster-v1"};
    constexpr std::array<std::string_view, 7> ExpectedRequiredAssets{
        "post-processing-interior-v1",
        "pbr-reference-environment-cooked-v1",
        "transparent-materials-v1",
        "water-surface-reference-fixture-v1",
        "particle-vfx-ground-caster-v1",
        "particle-fire-v1",
        "particle-smoke-v1"};
    constexpr std::array<std::string_view, 2> ExpectedEffects{
        "particle-fire-v1",
        "particle-smoke-v1"};
    constexpr std::array<std::string_view, 6> ExpectedCapabilities{
        "RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING",
        "RENDER.CAPABILITY.TEMPORAL_AA",
        "RENDER.CAPABILITY.DIRECTIONAL_CSM",
        "RENDER.CAPABILITY.SORTED_TRANSLUCENCY",
        "RENDER.CAPABILITY.CPU_PARTICLE_BILLBOARD",
        "RENDER.CAPABILITY.GERSTNER_WATER_SURFACE"};
}

TEST(RenderQualityLabSampleValidation, TemporalTranslucencyRequiresExactExecutedChain)
{
    using namespace RVX;
    SampleRenderDiagnostics diagnostics;
    diagnostics.transparentAlphaBlendDrawCount = 1;
    diagnostics.transparentPremultipliedDrawCount = 1;
    diagnostics.transparentAdditiveDrawCount = 1;
    diagnostics.transparentThinTransmissionDrawCount = 1;
    diagnostics.transparentOpaqueSceneColorPyramidReady = true;
    diagnostics.transparentOpaqueSceneColorPyramidBuilt = true;
    diagnostics.transparentReactiveMaskAttachmentBound = true;
    diagnostics.transparentReactiveMaskWriteDrawCount = 1;
    diagnostics.transparentPriorityBucketCount = 4;
    EXPECT_TRUE(HasSampleRenderQualityTranslucencyExecution(diagnostics));
    diagnostics.postProcessActualExecution.taaRequested = true;
    diagnostics.postProcessActualExecution.receiptFrameSequence = 17;
    EXPECT_FALSE(HasSampleRenderQualityTranslucencyExecution(diagnostics));
    RenderExecutionReceipt receipt;
    receipt.available = receipt.graphComposed = receipt.executionSucceeded = true;
    receipt.presented = receipt.succeeded = true;
    receipt.frameIdentity = {17, 1, 1, 1};
    RenderFeatureExecutionReceipt feature;
    feature.featureId = BuiltinRenderFeatureIds::Translucency;
    feature.selected = true;
    feature.readiness.compiled = feature.readiness.hardwareSupported = true;
    feature.readiness.implementationSupported = feature.readiness.pipelineReady = true;
    feature.readiness.inputsReady = feature.readiness.executed = true;
    feature.graphPassNames = {"TransparentMeshReactiveClear", "TransparentTemporalCandidates", "TransparentTemporalResolve"};
    feature.graphPassCount = feature.executedGraphPassCount = 3;
    receipt.features = {feature};
    diagnostics.transparentTemporalExecutionQualified = HasSampleTransparentTemporalExecution(receipt, 17);
    EXPECT_TRUE(HasSampleRenderQualityTranslucencyExecution(diagnostics));
    const auto reject = [&](auto mutate)
    {
        auto changed = diagnostics;
        auto changedReceipt = receipt;
        mutate(changedReceipt);
        changed.transparentTemporalExecutionQualified = HasSampleTransparentTemporalExecution(changedReceipt, 17);
        EXPECT_FALSE(HasSampleRenderQualityTranslucencyExecution(changed));
    };
    reject([](auto& r){++r.frameIdentity.frameSequence;});
    reject([](auto& r){r.features[0].executedGraphPassCount = 2;});
    reject([](auto& r){r.features[0].fallbackUsed = true;});
    reject([](auto& r){r.features[0].graphPassNames[1] = "TransparentTemporalResolve";});
    reject([](auto& r){r.features.push_back(r.features[0]);});
    reject([](auto& r){r.presented = false;});
}

TEST(RenderQualityLabSampleValidation,
     DeclaresFixedCookedCompositionAndNoSourceFallback)
{
    const RVX::RenderQualityLabSample sample;
    const RVX::SampleInfo& info = sample.GetInfo();

    EXPECT_EQ(info.id, "render-quality-lab");
    EXPECT_EQ(info.defaultAssetId, "post-processing-interior-v1");
    EXPECT_EQ(info.defaultEnvironmentId,
              "pbr-reference-environment-cooked-v1");
    EXPECT_EQ(info.defaultEffectAssetId, "particle-fire-v1");
    EXPECT_EQ(info.assetPolicy, RVX::SampleAssetPolicy::Fixed);
    EXPECT_EQ(info.environmentPolicy, RVX::SampleEnvironmentPolicy::Required);
    EXPECT_TRUE(info.supportsRenderPathSelection);
    EXPECT_EQ(info.defaultRenderPath, RVX::SampleRenderPath::Auto);
    EXPECT_TRUE(info.deterministicQualification);
    EXPECT_EQ(info.visualContractId, "render-quality-lab-v3");

    ASSERT_EQ(info.requiredEffectAssetIds.size(), ExpectedEffects.size());
    for (std::size_t index = 0; index < ExpectedEffects.size(); ++index)
    {
        EXPECT_EQ(info.requiredEffectAssetIds[index], ExpectedEffects[index]);
    }

    ASSERT_EQ(info.additionalDefaultAssetIds.size(),
              ExpectedAdditionalModels.size());
    for (std::size_t index = 0; index < ExpectedAdditionalModels.size(); ++index)
    {
        EXPECT_EQ(info.additionalDefaultAssetIds[index],
                  ExpectedAdditionalModels[index]);
    }

    ASSERT_EQ(info.requiredCookedAssetIds.size(), ExpectedRequiredAssets.size());
    for (std::size_t index = 0; index < ExpectedRequiredAssets.size(); ++index)
    {
        EXPECT_EQ(info.requiredCookedAssetIds[index], ExpectedRequiredAssets[index]);
    }
}

TEST(RenderQualityLabSampleValidation,
     AssessmentContractGatesEveryComposedGraphicsCapability)
{
    const RVX::RenderQualityLabSample sample;
    const RVX::SampleAssessmentContract contract =
        sample.GetAssessmentContract();

    ASSERT_TRUE(contract.IsValid());
    EXPECT_EQ(contract.code.GetValue(), "SAMPLE.RENDER_QUALITY_LAB");
    EXPECT_EQ(contract.revision, "1");
    ASSERT_EQ(contract.capabilities.size(), ExpectedCapabilities.size());
    for (std::size_t index = 0; index < ExpectedCapabilities.size(); ++index)
    {
        EXPECT_EQ(contract.capabilities[index].code.GetValue(),
                  ExpectedCapabilities[index]);
        EXPECT_TRUE(contract.capabilities[index].gating);
        EXPECT_FALSE(contract.capabilities[index].blockingReason.empty());
    }
}

TEST(RenderQualityLabSampleValidation,
     DeterministicModelSlotsAreStableAndBounded)
{
    std::vector<RVX::Vec3> first;
    std::vector<RVX::Vec3> second;
    ASSERT_TRUE(
        RVX::RenderQualityLabSampleValidationAccess::BuildDeterministicModelSlots(
            4u, first));
    ASSERT_TRUE(
        RVX::RenderQualityLabSampleValidationAccess::BuildDeterministicModelSlots(
            4u, second));
    ASSERT_EQ(first.size(), 4u);
    ASSERT_EQ(second.size(), first.size());

    constexpr std::array<RVX::Vec3, 4> ExpectedSlots{
        RVX::Vec3(0.0f, 0.0f, -5.0f),
        RVX::Vec3(0.0f, 0.0f, -1.0f),
        RVX::Vec3(2.7f, 0.0f, -2.3f),
        RVX::Vec3(-2.7f, 0.0f, -2.3f)};
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        EXPECT_EQ(first[index], ExpectedSlots[index]);
        EXPECT_FLOAT_EQ(first[index].x, second[index].x);
        EXPECT_FLOAT_EQ(first[index].y, second[index].y);
        EXPECT_FLOAT_EQ(first[index].z, second[index].z);
    }

    EXPECT_FALSE(
        RVX::RenderQualityLabSampleValidationAccess::BuildDeterministicModelSlots(
            0u, first));
    EXPECT_TRUE(first.empty());
    EXPECT_FALSE(
        RVX::RenderQualityLabSampleValidationAccess::BuildDeterministicModelSlots(
            5u, first));
    EXPECT_TRUE(first.empty());
}

TEST(RenderQualityLabSampleValidation,
     FireAndSmokeUseDistinctNonzeroDeterministicInstanceKeys)
{
    RVX::uint64 fireKey = 0;
    RVX::uint64 smokeKey = 0;
    EXPECT_TRUE(
        RVX::RenderQualityLabSampleValidationAccess::GetDeterministicParticleInstanceKeys(
            fireKey, smokeKey));
    EXPECT_NE(fireKey, 0u);
    EXPECT_NE(smokeKey, 0u);
    EXPECT_NE(fireKey, smokeKey);
}

TEST(RenderQualityLabSampleValidation,
     CapabilityContractContainsNoFallbackOrDuplicateDeclarations)
{
    const RVX::RenderQualityLabSample sample;
    const RVX::SampleAssessmentContract contract =
        sample.GetAssessmentContract();
    std::vector<std::string_view> codes;
    for (const RVX::AssessmentCapability& capability : contract.capabilities)
    {
        EXPECT_TRUE(capability.code.IsValid());
        EXPECT_TRUE(std::find(codes.begin(), codes.end(),
                              capability.code.GetValue()) == codes.end());
        codes.push_back(capability.code.GetValue());
    }
    EXPECT_TRUE(contract.actions.empty());
    EXPECT_FALSE(contract.invariants.empty());
    EXPECT_FALSE(contract.metrics.empty());
}
