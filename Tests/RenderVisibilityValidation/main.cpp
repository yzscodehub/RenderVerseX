#include "Render/Visibility/RenderVisibility.h"

#include "RenderContracts/RenderFrameTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace RVX;

namespace
{
    Mat4 TestViewProjection()
    {
        return glm::perspective(glm::radians(70.0f), 1.0f, 0.1f, 100.0f) *
               glm::lookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f),
                           Vec3(0.0f, 1.0f, 0.0f));
    }

    RenderVisibilityRequest TestRequest(bool distance = false,
                                        float maxDistance = 0.0f)
    {
        RenderVisibilityRequest request;
        request.viewProjection = TestViewProjection();
        request.cameraPosition = Vec3(0.0f);
        request.enableDistanceCulling = distance;
        request.maxDrawDistance = maxDistance;
        return request;
    }

    RenderVisibilityCandidate MakeCandidate(uint32 objectIndex,
                                            RenderPassKind pass,
                                            uint32 sourcePacketIndex,
                                            const AABB& bounds)
    {
        RenderVisibilityCandidate candidate;
        candidate.objectIndex = objectIndex;
        candidate.pass = pass;
        candidate.sourcePacketIndex = sourcePacketIndex;
        candidate.sourceOrdinal = 7; // Deliberately repeated compatibility data.
        candidate.objectVisible = true;
        candidate.drawable = true;
        candidate.worldBounds = bounds;
        return candidate;
    }
} // namespace

TEST(RenderVisibilityValidation, CanonicalAABBFrustumKeepsBoundaryAndRejectsBehindCamera)
{
    const RenderVisibilityFrustum frustum =
        RenderVisibilityFrustum::FromViewProjection(TestViewProjection());
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(-0.01f, -0.01f, -0.1f), Vec3(0.01f, 0.01f, -0.1f)),
        frustum));
    EXPECT_FALSE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(-0.1f, -0.1f, 1.0f), Vec3(0.1f, 0.1f, 1.1f)), frustum));
    EXPECT_FALSE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(-0.1f, -0.1f, -101.0f), Vec3(0.1f, 0.1f, -100.5f)),
        frustum));
}

TEST(RenderVisibilityValidation, ReverseZAndZeroExtentUseSameClipInequalities)
{
    const Mat4 reverseZ = glm::perspective(
        glm::radians(70.0f), 1.0f, 100.0f, 0.1f);
    const RenderVisibilityFrustum frustum =
        RenderVisibilityFrustum::FromViewProjection(reverseZ);
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(0.0f, 0.0f, -0.1f), Vec3(0.0f, 0.0f, -0.1f)), frustum));
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(0.0f, 0.0f, -100.0f), Vec3(0.0f, 0.0f, -100.0f)), frustum));
}

TEST(RenderVisibilityValidation, InvalidBoundsAreConservativeAndSanitizedForGPU)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const AABB invalid(Vec3(nan, 0.0f, -1.0f), Vec3(1.0f, 1.0f, -1.0f));
    bool reportedInvalid = false;
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        invalid,
        RenderVisibilityFrustum::FromViewProjection(TestViewProjection()),
        &reportedInvalid));
    EXPECT_TRUE(reportedInvalid);
    const RenderVisibilityGPUInput gpuInput = MakeRenderVisibilityGPUInput(invalid);
    EXPECT_EQ(1u, gpuInput.forceVisible);
    EXPECT_TRUE(std::isfinite(gpuInput.aabbMin.x));
    EXPECT_TRUE(std::isfinite(gpuInput.aabbMax.x));

    const float infinity = std::numeric_limits<float>::infinity();
    const AABB infinite(Vec3(-1.0f, -1.0f, -2.0f),
                         Vec3(infinity, 1.0f, -1.0f));
    reportedInvalid = false;
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        infinite,
        RenderVisibilityFrustum::FromViewProjection(TestViewProjection()),
        &reportedInvalid));
    EXPECT_TRUE(reportedInvalid);
    const RenderVisibilityGPUInput infiniteGPUInput =
        MakeRenderVisibilityGPUInput(infinite);
    EXPECT_EQ(1u, infiniteGPUInput.forceVisible);
    EXPECT_TRUE(std::isfinite(infiniteGPUInput.aabbMin.x));
    EXPECT_TRUE(std::isfinite(infiniteGPUInput.aabbMax.x));
}

TEST(RenderVisibilityValidation, PlaneDistanceEqualityAndZeroCandidatesRemainVisibleAndStable)
{
    RenderVisibilityFrustum frustum{};
    frustum.planes[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    EXPECT_TRUE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(-1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 0.0f)), frustum));
    EXPECT_FALSE(IsRenderVisibilityAABBVisible(
        AABB(Vec3(-1.1f, 0.0f, 0.0f), Vec3(-0.1f, 0.0f, 0.0f)), frustum));

    RenderCandidateSet emptyCandidates;
    CPUVisibilityProvider provider;
    RenderVisibilityResult result;
    RenderVisibilityRequest emptyRequest;
    provider.Evaluate(emptyCandidates, emptyRequest, result);
    EXPECT_EQ(0u, result.diagnostics.sceneObjectCandidateCount);
    EXPECT_TRUE(result.cpuVisibleObjectIndices.empty());
}

TEST(RenderVisibilityValidation, ProvidersPreservePassAwareSourceIdentityWithoutReadback)
{
    RenderCandidateSet candidates;
    (void)candidates.Add(MakeCandidate(0, RenderPassKind::None, RVX_INVALID_INDEX,
                                 AABB(Vec3(-1.0f, -1.0f, -6.0f),
                                      Vec3(1.0f, 1.0f, -4.0f))));
    (void)candidates.Add(MakeCandidate(1, RenderPassKind::None, RVX_INVALID_INDEX,
                                 AABB(Vec3(-1.0f, -1.0f, 2.0f),
                                      Vec3(1.0f, 1.0f, 3.0f))));
    (void)candidates.Add(MakeCandidate(0, RenderPassKind::Depth, 3,
                                 AABB(Vec3(-1.0f, -1.0f, -6.0f),
                                      Vec3(1.0f, 1.0f, -4.0f))));
    (void)candidates.Add(MakeCandidate(1, RenderPassKind::Depth, 4,
                                 AABB(Vec3(-1.0f, -1.0f, 2.0f),
                                      Vec3(1.0f, 1.0f, 3.0f))));
    (void)candidates.Add(MakeCandidate(0, RenderPassKind::Opaque, 9,
                                 AABB(Vec3(-1.0f, -1.0f, -6.0f),
                                      Vec3(1.0f, 1.0f, -4.0f))));

    CPUVisibilityProvider cpu;
    GPUVisibilityProvider gpu;
    RenderVisibilityResult cpuResult;
    RenderVisibilityResult gpuResult;
    cpu.Evaluate(candidates, TestRequest(), cpuResult);
    gpu.Evaluate(candidates, TestRequest(), gpuResult);

    ASSERT_EQ(1u, cpuResult.cpuVisibleObjectIndices.size());
    EXPECT_EQ(0u, cpuResult.cpuVisibleObjectIndices[0]);
    EXPECT_TRUE(cpuResult.IsDirectPacketVisible(RenderPassKind::Depth, 3));
    EXPECT_FALSE(cpuResult.IsDirectPacketVisible(RenderPassKind::Depth, 4));
    EXPECT_TRUE(cpuResult.IsDirectPacketVisible(RenderPassKind::Opaque, 9));
    EXPECT_EQ(cpuResult.cpuVisibleObjectIndices, gpuResult.cpuVisibleObjectIndices);
    EXPECT_EQ(cpuResult.passes[static_cast<size_t>(RenderPassKind::Depth)].cpuVisibleBySourcePacket,
              gpuResult.passes[static_cast<size_t>(RenderPassKind::Depth)].cpuVisibleBySourcePacket);
    EXPECT_FALSE(gpuResult.diagnostics.gpuReadbackPerformed);
    EXPECT_EQ(3u, gpuResult.diagnostics.gpuDeferredCandidateCount);
    EXPECT_EQ(2u,
              cpuResult.passes[static_cast<size_t>(RenderPassKind::Depth)]
                  .coarseCandidateIndices.size());
    EXPECT_LT(1u,
              cpuResult.passes[static_cast<size_t>(RenderPassKind::Depth)]
                  .coarseCandidateIndices.size());
    const RenderVisibilityCandidate* mapped =
        candidates.Find(RenderPassKind::Depth, 3);
    ASSERT_NE(nullptr, mapped);
    EXPECT_EQ(3u, mapped->sourcePacketIndex);
}

TEST(RenderVisibilityValidation, PassProjectionReusesCanonicalObjectVisibility)
{
    RenderCandidateSet candidates;
    (void)candidates.Add(MakeCandidate(
        0, RenderPassKind::None, RVX_INVALID_INDEX,
        AABB(Vec3(-1.0f, -1.0f, -6.0f), Vec3(1.0f, 1.0f, -4.0f))));
    (void)candidates.Add(MakeCandidate(
        1, RenderPassKind::None, RVX_INVALID_INDEX,
        AABB(Vec3(-1.0f, -1.0f, 2.0f), Vec3(1.0f, 1.0f, 3.0f))));

    GPUVisibilityProvider provider;
    RenderVisibilityResult incremental;
    provider.Evaluate(candidates, TestRequest(), incremental);
    const uint32 firstPassCandidate = static_cast<uint32>(
        candidates.GetCandidates().size());
    (void)candidates.Add(MakeCandidate(
        0, RenderPassKind::Depth, 2,
        AABB(Vec3(-1.0f, -1.0f, -6.0f), Vec3(1.0f, 1.0f, -4.0f))));
    (void)candidates.Add(MakeCandidate(
        1, RenderPassKind::Depth, 5,
        AABB(Vec3(-1.0f, -1.0f, 2.0f), Vec3(1.0f, 1.0f, 3.0f))));
    (void)candidates.Add(MakeCandidate(
        0, RenderPassKind::Opaque, 7,
        AABB(Vec3(-1.0f, -1.0f, -6.0f), Vec3(1.0f, 1.0f, -4.0f))));
    provider.AppendPassCandidates(candidates, firstPassCandidate, incremental);

    CPUVisibilityProvider fullProvider;
    RenderVisibilityResult full;
    fullProvider.Evaluate(candidates, TestRequest(), full);
    EXPECT_TRUE(incremental.structurallyValid);
    EXPECT_EQ(full.cpuVisibleObjectIndices,
              incremental.cpuVisibleObjectIndices);
    EXPECT_EQ(full.passes[static_cast<size_t>(RenderPassKind::Depth)]
                  .cpuVisibleBySourcePacket,
              incremental.passes[static_cast<size_t>(RenderPassKind::Depth)]
                  .cpuVisibleBySourcePacket);
    EXPECT_EQ(full.passes[static_cast<size_t>(RenderPassKind::Opaque)]
                  .cpuVisibleBySourcePacket,
              incremental.passes[static_cast<size_t>(RenderPassKind::Opaque)]
                  .cpuVisibleBySourcePacket);
    EXPECT_EQ(2u, incremental.diagnostics.sceneObjectCandidateCount);
    EXPECT_EQ(3u, incremental.diagnostics.passCandidateCount);
    EXPECT_EQ(3u, incremental.diagnostics.gpuDeferredCandidateCount);
    EXPECT_FALSE(incremental.diagnostics.gpuReadbackPerformed);
}

TEST(RenderVisibilityValidation,
     CommonLayerCandidateExcludesDirectAndGPUDrivenLanesIdentically)
{
    constexpr uint32 objectLayerMask = 0x00000002U;
    constexpr uint32 viewCullingMask = 0x00000001U;
    ASSERT_FALSE(IsRenderLayerVisible(objectLayerMask, viewCullingMask));

    const AABB bounds(Vec3(-0.1f, -0.1f, -5.0f),
                      Vec3(0.1f, 0.1f, -4.9f));
    RenderCandidateSet candidates;
    RenderVisibilityCandidate sceneCandidate = MakeCandidate(
        0, RenderPassKind::None, RVX_INVALID_INDEX, bounds);
    sceneCandidate.objectVisible = IsRenderLayerVisible(
        objectLayerMask, viewCullingMask);
    ASSERT_NE(RVX_INVALID_INDEX, candidates.Add(sceneCandidate));

    GPUVisibilityProvider gpuProvider;
    RenderVisibilityResult gpuResult;
    gpuProvider.Evaluate(candidates, TestRequest(), gpuResult);

    const uint32 firstPassCandidate = static_cast<uint32>(
        candidates.GetCandidates().size());
    RenderVisibilityCandidate directCandidate = MakeCandidate(
        0, RenderPassKind::Opaque, 0, bounds);
    directCandidate.objectVisible = sceneCandidate.objectVisible;
    ASSERT_NE(RVX_INVALID_INDEX, candidates.Add(directCandidate));
    gpuProvider.AppendPassCandidates(candidates, firstPassCandidate, gpuResult);

    CPUVisibilityProvider directProvider;
    RenderVisibilityResult directResult;
    directProvider.Evaluate(candidates, TestRequest(), directResult);

    EXPECT_TRUE(directResult.cpuVisibleObjectIndices.empty());
    EXPECT_TRUE(gpuResult.cpuVisibleObjectIndices.empty());
    EXPECT_FALSE(directResult.IsDirectPacketVisible(RenderPassKind::Opaque, 0));
    EXPECT_FALSE(gpuResult.IsDirectPacketVisible(RenderPassKind::Opaque, 0));
    EXPECT_EQ(0U, gpuResult.diagnostics.gpuDeferredCandidateCount);
}

TEST(RenderVisibilityValidation, DenseSourceMappingStaysLinearForLargeCandidateSets)
{
    RenderCandidateSet candidates;
    constexpr uint32 candidateCount = 16384;
    for (uint32 index = 0; index < candidateCount; ++index)
    {
        (void)candidates.Add(MakeCandidate(index, RenderPassKind::Opaque, index,
                                     AABB(Vec3(-0.1f, -0.1f, -5.0f),
                                          Vec3(0.1f, 0.1f, -4.9f))));
    }
    CPUVisibilityProvider provider;
    RenderVisibilityResult result;
    provider.Evaluate(candidates, TestRequest(), result);
    EXPECT_EQ(0u, result.diagnostics.sceneObjectCandidateCount);
    EXPECT_EQ(candidateCount, result.diagnostics.passCandidateCount);
    EXPECT_EQ(candidateCount,
              result.passes[static_cast<size_t>(RenderPassKind::Opaque)]
                  .coarseCandidateIndices.size());
    EXPECT_TRUE(result.IsDirectPacketVisible(RenderPassKind::Opaque,
                                             candidateCount - 1));
}

TEST(RenderVisibilityValidation, DirectDistanceVisibilityUsesRequestAndDuplicateSourcesFailClosed)
{
    RenderCandidateSet candidates;
    (void)candidates.Add(MakeCandidate(0, RenderPassKind::Opaque, 4,
                                       AABB(Vec3(0.0f, 0.0f, -5.0f),
                                            Vec3(0.0f, 0.0f, -5.0f))));
    CPUVisibilityProvider provider;
    RenderVisibilityResult equalityResult;
    provider.Evaluate(candidates, TestRequest(true, 5.0f), equalityResult);
    EXPECT_TRUE(equalityResult.IsDirectPacketVisible(RenderPassKind::Opaque, 4));

    RenderVisibilityResult culledResult;
    provider.Evaluate(candidates, TestRequest(true, 4.99f), culledResult);
    EXPECT_FALSE(culledResult.IsDirectPacketVisible(RenderPassKind::Opaque, 4));
    ASSERT_EQ(1u, culledResult.passes[static_cast<size_t>(RenderPassKind::Opaque)]
                       .coarseCandidateIndices.size());

    EXPECT_EQ(RVX_INVALID_INDEX,
              candidates.Add(MakeCandidate(0, RenderPassKind::Opaque, 4,
                                           AABB(Vec3(0.0f, 0.0f, -5.0f),
                                                Vec3(0.0f, 0.0f, -5.0f)))));
    EXPECT_FALSE(candidates.IsValid());
    RenderVisibilityResult invalidResult;
    provider.Evaluate(candidates, TestRequest(), invalidResult);
    EXPECT_FALSE(invalidResult.structurallyValid);
    EXPECT_FALSE(invalidResult.HasDirectPacketSource(RenderPassKind::Opaque, 4));
}

TEST(RenderVisibilityValidation, CandidateIdentityStructureFailsClosed)
{
    const AABB bounds(Vec3(-0.1f, -0.1f, -5.0f),
                      Vec3(0.1f, 0.1f, -4.9f));

    RenderCandidateSet missingSceneObject;
    EXPECT_EQ(RVX_INVALID_INDEX,
              missingSceneObject.Add(MakeCandidate(
                  RVX_INVALID_INDEX, RenderPassKind::None,
                  RVX_INVALID_INDEX, bounds)));
    EXPECT_FALSE(missingSceneObject.IsValid());

    RenderCandidateSet sceneWithPacketIdentity;
    EXPECT_EQ(RVX_INVALID_INDEX,
              sceneWithPacketIdentity.Add(MakeCandidate(
                  0, RenderPassKind::None, 0, bounds)));
    EXPECT_FALSE(sceneWithPacketIdentity.IsValid());

    RenderCandidateSet passWithoutPacketIdentity;
    EXPECT_EQ(RVX_INVALID_INDEX,
              passWithoutPacketIdentity.Add(MakeCandidate(
                  0, RenderPassKind::Depth, RVX_INVALID_INDEX, bounds)));
    EXPECT_FALSE(passWithoutPacketIdentity.IsValid());

    RenderCandidateSet invalidPass;
    EXPECT_EQ(RVX_INVALID_INDEX,
              invalidPass.Add(MakeCandidate(
                  0, static_cast<RenderPassKind>(255), 0, bounds)));
    EXPECT_FALSE(invalidPass.IsValid());
}
