#include "Render/Translucency/TranslucencyFramePlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace
{
    RVX::WeightedBlendedOITCapabilityReadiness MakeReadyCapability()
    {
        return {
            .requested = true,
            .compiled = true,
            .hardwareSupported = true,
            .independentBlendSupported = true,
            .accumulationFormatBlendSupported = true,
            .revealageFormatBlendSupported = true,
            .implementationSupported = true,
            .pipelineReady = true,
            .inputsReady = true};
    }

    RVX::TranslucencyMeshCandidate MakeMesh(
        RVX::uint32 ordinal,
        RVX::uint64 identity,
        RVX::MaterialShadingModel shadingModel,
        RVX::MaterialBlendMode blendMode)
    {
        return {
            .sourceOrdinal = ordinal,
            .sourceIdentityHash = identity,
            .shadingModel = shadingModel,
            .blendMode = blendMode,
            .surfacePathEligibility =
                RVX::SurfacePathEligibility::ForwardTranslucency,
            .materialPolicyValid = true};
    }

    RVX::TranslucencyParticleCandidate MakeParticle(
        RVX::uint32 ordinal,
        RVX::uint64 identity,
        RVX::uint32 firstItem,
        RVX::uint32 itemCount,
        RVX::ParticleRenderSnapshotBlendMode blendMode)
    {
        return {
            .sourceOrdinal = ordinal,
            .sourceIdentityHash = identity,
            .firstItem = firstItem,
            .itemCount = itemCount,
            .blendMode = blendMode};
    }

    RVX::TranslucencyFramePlanRequest MakeRequest()
    {
        RVX::TranslucencyFramePlanRequest request;
        request.frameIdentity = {
            .frameSequence = 11,
            .sceneRevision = 13,
            .surfaceGeneration = 17,
            .configurationRevision = 19};
        request.viewIdentity = {
            .sceneRuntimeId = 23,
            .cameraEntityPacked = 29,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        request.profileHash = 31;
        request.meshSourceIdentityHash = 37;
        request.particleSourceIdentityHash = 41;
        request.particleItemCount = 9;
        request.weightedBlendedRequirement =
            RVX::TranslucencyCapabilityRequirement::Optional;
        request.weightedBlendedReadiness = MakeReadyCapability();
        request.meshCandidates = {
            MakeMesh(0, 101, RVX::MaterialShadingModel::StandardLit,
                     RVX::MaterialBlendMode::AlphaBlend),
            MakeMesh(1, 103, RVX::MaterialShadingModel::Unlit,
                     RVX::MaterialBlendMode::Premultiplied),
            MakeMesh(2, 107, RVX::MaterialShadingModel::ThinTransmission,
                     RVX::MaterialBlendMode::AlphaBlend),
            MakeMesh(3, 109, RVX::MaterialShadingModel::Unlit,
                     RVX::MaterialBlendMode::Additive)};
        request.particleCandidates = {
            MakeParticle(
                0, 201, 0, 2,
                RVX::ParticleRenderSnapshotBlendMode::AlphaBlend),
            MakeParticle(
                1, 211, 2, 3,
                RVX::ParticleRenderSnapshotBlendMode::Premultiplied),
            MakeParticle(
                2, 223, 5, 4,
                RVX::ParticleRenderSnapshotBlendMode::Additive)};
        return request;
    }

    RVX::WeightedBlendedOITReferenceSample MakeSample(
        RVX::Vec3 color,
        RVX::float32 alpha,
        RVX::float32 depth,
        bool premultiplied = false)
    {
        return {
            .color = color,
            .alpha = alpha,
            .normalizedDepth = depth,
            .premultiplied = premultiplied};
    }

    void ExpectReferenceEqual(
        const RVX::WeightedBlendedOITReferenceResult& left,
        const RVX::WeightedBlendedOITReferenceResult& right)
    {
        EXPECT_EQ(left.code, right.code);
        EXPECT_EQ(left.inputSampleCount, right.inputSampleCount);
        EXPECT_EQ(left.contributingSampleCount, right.contributingSampleCount);
        EXPECT_EQ(left.weightedPremultipliedColor,
                  right.weightedPremultipliedColor);
        EXPECT_EQ(left.weightedAlpha, right.weightedAlpha);
        EXPECT_EQ(left.revealage, right.revealage);
        EXPECT_EQ(left.resolvedPremultiplied.x,
                  right.resolvedPremultiplied.x);
        EXPECT_EQ(left.resolvedPremultiplied.y,
                  right.resolvedPremultiplied.y);
        EXPECT_EQ(left.resolvedPremultiplied.z,
                  right.resolvedPremultiplied.z);
        EXPECT_EQ(left.resolvedPremultiplied.w,
                  right.resolvedPremultiplied.w);
    }
} // namespace

TEST(TranslucencyFramePlanValidation,
     RoutesEveryMeshAndParticleExactlyOnceWhenWeightedBlendedIsReady)
{
    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(MakeRequest());
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::Ready, result.code);
    EXPECT_EQ(
        RVX::WeightedBlendedOITCapabilityResolution::Enabled,
        result.plan.GetWeightedBlendedResolution());
    ASSERT_TRUE(result.plan.IsStructurallyValid());
    EXPECT_TRUE(result.plan.IsExecutable());

    const RVX::TranslucencyRouteSummary& summary = result.plan.GetSummary();
    EXPECT_TRUE(summary.IsExactlyOnce());
    EXPECT_EQ(4U, summary.meshCandidateCount);
    EXPECT_EQ(3U, summary.particleBatchCandidateCount);
    EXPECT_EQ(9U, summary.particleItemCount);
    EXPECT_EQ(4U,
              summary.GetEntryCount(
                  RVX::TranslucencyRoute::WeightedBlended));
    EXPECT_EQ(7U,
              summary.GetItemCount(
                  RVX::TranslucencyRoute::WeightedBlended));
    EXPECT_EQ(1U,
              summary.GetEntryCount(
                  RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(1U,
              summary.GetItemCount(
                  RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(2U,
              summary.GetEntryCount(
                  RVX::TranslucencyRoute::DirectAdditive));
    EXPECT_EQ(5U,
              summary.GetItemCount(
                  RVX::TranslucencyRoute::DirectAdditive));
    EXPECT_EQ(0U,
              summary.GetItemCount(
                  RVX::TranslucencyRoute::Unsupported));

    const RVX::TranslucencyPlanEntry* const mesh =
        result.plan.FindEntry(RVX::TranslucencySourceKind::Mesh, 2);
    ASSERT_NE(nullptr, mesh);
    EXPECT_EQ(RVX::TranslucencyRoute::SortedRefraction, mesh->route);
    const RVX::TranslucencyPlanEntry* const particles =
        result.plan.FindEntry(RVX::TranslucencySourceKind::Particle, 1);
    ASSERT_NE(nullptr, particles);
    EXPECT_EQ(RVX::TranslucencyRoute::WeightedBlended, particles->route);
    EXPECT_EQ(2U, particles->firstItem);
    EXPECT_EQ(3U, particles->itemCount);
    EXPECT_EQ(nullptr,
              result.plan.FindEntry(
                  RVX::TranslucencySourceKind::Particle, 7));
}

TEST(TranslucencyFramePlanValidation,
     OptionalUnavailableCapabilityRejectsMixedSourceSortedFallback)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.weightedBlendedReadiness.pipelineReady = false;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::MixedSourceSortedOrderingUnsupported,
              result.code);
    EXPECT_EQ(
        RVX::WeightedBlendedOITCapabilityResolution::OptionalSortedFallback,
        result.plan.GetWeightedBlendedResolution());
    EXPECT_TRUE(result.plan.IsStructurallyValid());
    EXPECT_TRUE(result.plan.HasMixedSourceSortedOrderingConflict());
    EXPECT_FALSE(result.plan.IsExecutable());
    EXPECT_EQ(0U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::WeightedBlended));
    EXPECT_EQ(8U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(5U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::DirectAdditive));
}

TEST(TranslucencyFramePlanValidation,
     OptionalUnavailableCapabilityUsesDeclaredSortedFallbackForMeshOnly)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.weightedBlendedReadiness.pipelineReady = false;
    request.particleCandidates.clear();
    request.particleItemCount = 0;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::ReadyWithOptionalFallback,
              result.code);
    EXPECT_FALSE(result.plan.HasMixedSourceSortedOrderingConflict());
    EXPECT_EQ(3U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(1U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::DirectAdditive));
}

TEST(TranslucencyFramePlanValidation,
     OptionalUnrequestedCapabilityRejectsMixedSourceSortedConfiguration)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.weightedBlendedReadiness.requested = false;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::MixedSourceSortedOrderingUnsupported,
        result.code);
    EXPECT_EQ(RVX::WeightedBlendedOITCapabilityResolution::Disabled,
              result.plan.GetWeightedBlendedResolution());
    EXPECT_TRUE(result.plan.HasMixedSourceSortedOrderingConflict());
    EXPECT_EQ(8U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::SortedRefraction));
}

TEST(TranslucencyFramePlanValidation,
     RequiredUnavailableCapabilityFailsClosedButPreservesAuditPlan)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.weightedBlendedRequirement =
        RVX::TranslucencyCapabilityRequirement::Required;
    request.weightedBlendedReadiness.hardwareSupported = false;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::RequiredCapabilityUnavailable,
              result.code);
    EXPECT_TRUE(result.plan.IsStructurallyValid());
    EXPECT_FALSE(result.plan.IsExecutable());
    EXPECT_TRUE(result.plan.HasRequiredCapabilityFailure());
    EXPECT_EQ(7U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::Unsupported));
    EXPECT_EQ(1U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(5U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::DirectAdditive));
}

TEST(TranslucencyFramePlanValidation,
     UnsupportedMaterialAndParticlePoliciesNeverAcquireACompatibleRoute)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.meshCandidates[0].shadingModel =
        RVX::MaterialShadingModel::ToonLit;
    request.particleCandidates[1].blendMode =
        RVX::ParticleRenderSnapshotBlendMode::Multiply;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::UnsupportedCandidate,
              result.code);
    EXPECT_TRUE(result.plan.IsStructurallyValid());
    EXPECT_FALSE(result.plan.IsExecutable());
    EXPECT_EQ(4U,
              result.plan.GetSummary().GetItemCount(
                  RVX::TranslucencyRoute::Unsupported));
}

TEST(TranslucencyFramePlanValidation,
     RejectsInvalidMeshPolicyWithoutSilentlyChangingTheRoute)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    request.meshCandidates[0].materialPolicyValid = false;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::UnsupportedCandidate,
              result.code);
    const RVX::TranslucencyPlanEntry* const entry =
        result.plan.FindEntry(RVX::TranslucencySourceKind::Mesh, 0);
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(RVX::TranslucencyRoute::Unsupported, entry->route);
}

TEST(TranslucencyFramePlanValidation,
     RejectsDuplicateFrozenSourceIdentityWithinEachSourceKind)
{
    RVX::TranslucencyFramePlanRequest meshRequest = MakeRequest();
    meshRequest.meshCandidates[1].sourceIdentityHash =
        meshRequest.meshCandidates[0].sourceIdentityHash;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::DuplicateSourceIdentity,
        RVX::TranslucencyFramePlanBuilder{}.Build(meshRequest).code);

    RVX::TranslucencyFramePlanRequest particleRequest = MakeRequest();
    particleRequest.particleCandidates[1].sourceIdentityHash =
        particleRequest.particleCandidates[0].sourceIdentityHash;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::DuplicateSourceIdentity,
        RVX::TranslucencyFramePlanBuilder{}.Build(particleRequest).code);
}

TEST(TranslucencyFramePlanValidation,
     RejectsSparseOrdinalsAndParticleGapsOverlapsAndMissingTail)
{
    RVX::TranslucencyFramePlanRequest sparseMesh = MakeRequest();
    sparseMesh.meshCandidates[2].sourceOrdinal = 8;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::InvalidSourceInventory,
        RVX::TranslucencyFramePlanBuilder{}.Build(sparseMesh).code);

    RVX::TranslucencyFramePlanRequest sparseParticle = MakeRequest();
    sparseParticle.particleCandidates[1].sourceOrdinal = 8;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::InvalidSourceInventory,
        RVX::TranslucencyFramePlanBuilder{}.Build(sparseParticle).code);

    RVX::TranslucencyFramePlanRequest gap = MakeRequest();
    gap.particleCandidates[1].firstItem = 3;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::ParticleCoverageMismatch,
        RVX::TranslucencyFramePlanBuilder{}.Build(gap).code);

    RVX::TranslucencyFramePlanRequest overlap = MakeRequest();
    overlap.particleCandidates[1].firstItem = 1;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::ParticleCoverageMismatch,
        RVX::TranslucencyFramePlanBuilder{}.Build(overlap).code);

    RVX::TranslucencyFramePlanRequest missingTail = MakeRequest();
    missingTail.particleItemCount = 10;
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::ParticleCoverageMismatch,
        RVX::TranslucencyFramePlanBuilder{}.Build(missingTail).code);
}

TEST(TranslucencyFramePlanValidation,
     IdentityIsStableAndBindsSourcesCapabilityAndExpectedIdentity)
{
    RVX::TranslucencyFramePlanRequest request = MakeRequest();
    const RVX::TranslucencyFramePlanBuildResult first =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    const RVX::TranslucencyFramePlanBuildResult repeat =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(first.Succeeded());
    ASSERT_TRUE(repeat.Succeeded());
    EXPECT_EQ(first.plan.GetIdentityHash(), repeat.plan.GetIdentityHash());

    request.meshSourceIdentityHash += 1;
    const RVX::TranslucencyFramePlanBuildResult changedSource =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(changedSource.Succeeded());
    EXPECT_NE(first.plan.GetIdentityHash(),
              changedSource.plan.GetIdentityHash());

    request = MakeRequest();
    request.weightedBlendedReadiness.pipelineReady = false;
    const RVX::TranslucencyFramePlanBuildResult changedCapability =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    ASSERT_FALSE(changedCapability.Succeeded());
    EXPECT_EQ(
        RVX::TranslucencyFramePlanCode::MixedSourceSortedOrderingUnsupported,
        changedCapability.code);
    EXPECT_TRUE(changedCapability.plan.IsStructurallyValid());
    EXPECT_TRUE(
        changedCapability.plan.HasMixedSourceSortedOrderingConflict());
    EXPECT_NE(first.plan.GetIdentityHash(),
              changedCapability.plan.GetIdentityHash());

    request = MakeRequest();
    request.expectedIdentityHash = first.plan.GetIdentityHash();
    EXPECT_TRUE(RVX::TranslucencyFramePlanBuilder{}.Build(request).Succeeded());
    request.expectedIdentityHash += 1;
    const RVX::TranslucencyFramePlanBuildResult stale =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::IdentityMismatch, stale.code);
    EXPECT_TRUE(stale.plan.IsStructurallyValid());
}

TEST(TranslucencyFramePlanValidation,
     EmptyReferenceAndTransparentBoundarySamplesHaveBoundedResults)
{
    const std::vector<RVX::WeightedBlendedOITReferenceSample> empty;
    const RVX::WeightedBlendedOITReferenceResult emptyResult =
        RVX::EvaluateWeightedBlendedOITReference(empty);
    ASSERT_TRUE(emptyResult.Succeeded());
    EXPECT_TRUE(emptyResult.IsEmpty());
    EXPECT_EQ(1.0, emptyResult.revealage);
    EXPECT_EQ(RVX::Vec4(0.0f), emptyResult.resolvedPremultiplied);

    const std::array samples{
        MakeSample(RVX::Vec3(16.0f, 0.0f, 4.0f), 0.0f, 0.0f),
        MakeSample(RVX::Vec3(2.0f, 4.0f, 8.0f), 1.0f, 1.0f)};
    const RVX::WeightedBlendedOITReferenceResult result =
        RVX::EvaluateWeightedBlendedOITReference(samples);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(1U, result.contributingSampleCount);
    EXPECT_EQ(0.0, result.revealage);
    EXPECT_EQ(RVX::Vec4(2.0f, 4.0f, 8.0f, 1.0f),
              result.resolvedPremultiplied);

    const std::array maximumHdr{
        MakeSample(RVX::Vec3(16.0f), 1.0f, 0.0f)};
    const auto maximumHdrResult =
        RVX::EvaluateWeightedBlendedOITReference(maximumHdr);
    ASSERT_TRUE(maximumHdrResult.Succeeded());
    EXPECT_EQ(RVX::Vec4(16.0f, 16.0f, 16.0f, 1.0f),
              maximumHdrResult.resolvedPremultiplied);
}

TEST(TranslucencyFramePlanValidation,
     ForwardAndReverseDeviceDepthMapToTheSameCanonicalWeightDepth)
{
    for (const RVX::float32 canonicalDepth :
         {0.0f, 0.125f, 0.5f, 0.875f, 1.0f})
    {
        EXPECT_FLOAT_EQ(
            canonicalDepth,
            RVX::CanonicalizeWeightedBlendedOITDepth(
                canonicalDepth, false));
        EXPECT_FLOAT_EQ(
            canonicalDepth,
            RVX::CanonicalizeWeightedBlendedOITDepth(
                1.0f - canonicalDepth, true));
    }
    EXPECT_FLOAT_EQ(
        0.0f,
        RVX::CanonicalizeWeightedBlendedOITDepth(-1.0f, false));
    EXPECT_FLOAT_EQ(
        1.0f,
        RVX::CanonicalizeWeightedBlendedOITDepth(-1.0f, true));
}

TEST(TranslucencyFramePlanValidation,
     PublishedOverlapBudgetKeepsRgba16fAccumulationFinite)
{
    std::vector<RVX::WeightedBlendedOITReferenceSample> samples(
        RVX::RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT,
        MakeSample(
            RVX::Vec3(RVX::RVX_WBOIT_REFERENCE_MAX_HDR_CHANNEL),
            1.0f,
            0.0f));
    const RVX::WeightedBlendedOITReferenceResult result =
        RVX::EvaluateWeightedBlendedOITReference(samples);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(RVX::RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT,
              result.contributingSampleCount);
    for (const RVX::float64 channel : result.weightedPremultipliedColor)
    {
        EXPECT_TRUE(std::isfinite(channel));
        EXPECT_LE(
            channel,
            static_cast<RVX::float64>(
                RVX::RVX_WBOIT_REFERENCE_MAX_ACCUMULATED_CHANNEL));
    }
    EXPECT_TRUE(std::isfinite(result.weightedAlpha));
    EXPECT_EQ(0.0, result.revealage);
}

TEST(TranslucencyFramePlanValidation,
     ReferenceIsBitIdenticalForEveryInputPermutation)
{
    std::vector<RVX::WeightedBlendedOITReferenceSample> samples{
        MakeSample(RVX::Vec3(4.0f, 0.5f, 1.0f), 0.25f, 0.75f),
        MakeSample(RVX::Vec3(0.1f, 1.5f, 8.0f), 0.9f, 0.05f),
        MakeSample(RVX::Vec3(2.0f, 3.0f, 0.25f), 0.5f, 0.4f),
        MakeSample(RVX::Vec3(0.0f), 0.0f, 1.0f, true)};
    const RVX::WeightedBlendedOITReferenceResult expected =
        RVX::EvaluateWeightedBlendedOITReference(samples);
    ASSERT_TRUE(expected.Succeeded());

    std::sort(samples.begin(), samples.end(),
              [](const auto& left, const auto& right)
              {
                  return left.color.x < right.color.x;
              });
    do
    {
        ExpectReferenceEqual(
            expected,
            RVX::EvaluateWeightedBlendedOITReference(samples));
    } while (std::next_permutation(
        samples.begin(), samples.end(),
        [](const auto& left, const auto& right)
        {
            return left.color.x < right.color.x;
        }));
}

TEST(TranslucencyFramePlanValidation,
     StraightAndPremultipliedRepresentationsProduceTheSameReference)
{
    const std::array straight{
        MakeSample(RVX::Vec3(4.0f, 2.0f, 1.0f), 0.25f, 0.3f)};
    const std::array premultiplied{
        MakeSample(RVX::Vec3(1.0f, 0.5f, 0.25f),
                   0.25f, 0.3f, true)};
    const auto straightResult =
        RVX::EvaluateWeightedBlendedOITReference(straight);
    const auto premultipliedResult =
        RVX::EvaluateWeightedBlendedOITReference(premultiplied);
    ASSERT_TRUE(straightResult.Succeeded());
    ASSERT_TRUE(premultipliedResult.Succeeded());
    EXPECT_EQ(straightResult.weightedPremultipliedColor,
              premultipliedResult.weightedPremultipliedColor);
    EXPECT_EQ(straightResult.weightedAlpha,
              premultipliedResult.weightedAlpha);
    EXPECT_EQ(straightResult.revealage, premultipliedResult.revealage);
    EXPECT_EQ(straightResult.resolvedPremultiplied.x,
              premultipliedResult.resolvedPremultiplied.x);
    EXPECT_EQ(straightResult.resolvedPremultiplied.y,
              premultipliedResult.resolvedPremultiplied.y);
    EXPECT_EQ(straightResult.resolvedPremultiplied.z,
              premultipliedResult.resolvedPremultiplied.z);
    EXPECT_EQ(straightResult.resolvedPremultiplied.w,
              premultipliedResult.resolvedPremultiplied.w);
}

TEST(TranslucencyFramePlanValidation,
     ReferenceRejectsNonFiniteAndOutOfRangeInputs)
{
    using Code = RVX::WeightedBlendedOITReferenceCode;
    const RVX::float32 nan = std::numeric_limits<RVX::float32>::quiet_NaN();
    const RVX::float32 infinity =
        std::numeric_limits<RVX::float32>::infinity();

    const std::array nonFiniteColor{
        MakeSample(RVX::Vec3(nan, 0.0f, 0.0f), 0.5f, 0.5f)};
    EXPECT_EQ(Code::NonFiniteInput,
              RVX::EvaluateWeightedBlendedOITReference(nonFiniteColor).code);
    const std::array nonFiniteAlpha{
        MakeSample(RVX::Vec3(1.0f), infinity, 0.5f)};
    EXPECT_EQ(Code::NonFiniteInput,
              RVX::EvaluateWeightedBlendedOITReference(nonFiniteAlpha).code);
    const std::array highColor{
        MakeSample(RVX::Vec3(16.01f, 0.0f, 0.0f), 0.5f, 0.5f)};
    EXPECT_EQ(Code::ColorOutOfRange,
              RVX::EvaluateWeightedBlendedOITReference(highColor).code);
    const std::array negativeColor{
        MakeSample(RVX::Vec3(-0.01f, 0.0f, 0.0f), 0.5f, 0.5f)};
    EXPECT_EQ(Code::ColorOutOfRange,
              RVX::EvaluateWeightedBlendedOITReference(negativeColor).code);
    const std::array highAlpha{
        MakeSample(RVX::Vec3(1.0f), 1.01f, 0.5f)};
    EXPECT_EQ(Code::AlphaOutOfRange,
              RVX::EvaluateWeightedBlendedOITReference(highAlpha).code);
    const std::array lowDepth{
        MakeSample(RVX::Vec3(1.0f), 0.5f, -0.01f)};
    EXPECT_EQ(Code::DepthOutOfRange,
              RVX::EvaluateWeightedBlendedOITReference(lowDepth).code);
}

TEST(TranslucencyFramePlanValidation,
     ReferenceRejectsPremultipliedColorOutsideThePublishedHdrBound)
{
    const std::array zeroAlpha{
        MakeSample(RVX::Vec3(0.01f, 0.0f, 0.0f), 0.0f, 0.5f, true)};
    EXPECT_EQ(
        RVX::WeightedBlendedOITReferenceCode::InvalidPremultipliedColor,
        RVX::EvaluateWeightedBlendedOITReference(zeroAlpha).code);

    const std::array exceedsStraightHdrBound{
        MakeSample(RVX::Vec3(8.01f, 0.0f, 0.0f),
                   0.5f, 0.5f, true)};
    EXPECT_EQ(
        RVX::WeightedBlendedOITReferenceCode::InvalidPremultipliedColor,
        RVX::EvaluateWeightedBlendedOITReference(
            exceedsStraightHdrBound).code);
}

TEST(TranslucencyFramePlanValidation,
     ReferenceEnforcesThePublishedSampleBound)
{
    std::vector<RVX::WeightedBlendedOITReferenceSample> samples(
        RVX::RVX_WBOIT_REFERENCE_MAX_SAMPLE_COUNT,
        MakeSample(RVX::Vec3(0.0f), 0.0f, 0.0f));
    EXPECT_TRUE(
        RVX::EvaluateWeightedBlendedOITReference(samples).Succeeded());
    samples.push_back(MakeSample(RVX::Vec3(0.0f), 0.0f, 0.0f));
    EXPECT_EQ(
        RVX::WeightedBlendedOITReferenceCode::SampleLimitExceeded,
        RVX::EvaluateWeightedBlendedOITReference(samples).code);
}
