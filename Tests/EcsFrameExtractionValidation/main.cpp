#include "RenderExtraction/ECS/EcsFrameExtractor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <utility>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;

struct TestAssetResolver
{
    std::unordered_map<uint64, RenderResourceHandle> handles;
    uint64 rejectedAssetId = 0;
};

[[nodiscard]] RenderResourceHandle ResolveAsset(
    void* context,
    AssetId assetId,
    RenderResourceKind) noexcept
{
    const auto* resolver = static_cast<const TestAssetResolver*>(context);
    if (resolver == nullptr || assetId.value == resolver->rejectedAssetId)
        return {};
    const auto found = resolver->handles.find(assetId.value);
    return found != resolver->handles.end() ? found->second
                                            : RenderResourceHandle{};
}

[[nodiscard]] Mat4 MakeTranslation(float32 x, float32 y = 0.0f, float32 z = 0.0f)
{
    Mat4 value{1.0f};
    value[3] = Vec4(x, y, z, 1.0f);
    return value;
}

[[nodiscard]] FrozenSceneObjectId MakeId(
    ECS::SceneRuntimeId runtimeId,
    ECS::EntityHandle entity,
    FrozenSceneObjectType type,
    uint32 slot = 0)
{
    return MakeFrozenSceneObjectId(runtimeId, entity, type, slot);
}

[[nodiscard]] FrozenSceneMesh MakeMesh(
    ECS::SceneRuntimeId runtimeId,
    uint32 entityIndex,
    uint64 meshAssetId,
    uint64 firstMaterialAssetId,
    float32 transformX)
{
    const ECS::EntityHandle entity = ECS::EntityHandle::Create(entityIndex, 1);
    FrozenSceneMesh mesh;
    mesh.id = MakeId(runtimeId, entity, FrozenSceneObjectType::Mesh);
    mesh.source.meshAssetId = {.value = meshAssetId};
    mesh.source.submeshCount = 2;
    mesh.visibility.layerMask = 0x01U;
    mesh.visibility.visible = true;
    mesh.visibility.castsShadow = true;
    mesh.visibility.receivesShadow = true;
    mesh.bounds.center = {0.0f, 0.0f, 0.0f};
    mesh.bounds.extents = {1.0f, 1.0f, 1.0f};
    mesh.worldTransform = MakeTranslation(transformX);
    mesh.previousSimulationWorldTransform = MakeTranslation(transformX - 1.0f);
    mesh.transformSourceRevision = 5;
    mesh.previousSimulationSourceRevision = 4;
    mesh.materialSlotsSource.count = 2;
    mesh.materialSlotsSource.values[0] = {
        .materialAssetId = {.value = firstMaterialAssetId},
        .materialMode = RenderMaterialMode::Masked,
    };
    mesh.materialSlotsSource.values[1] = {
        .materialAssetId = {.value = firstMaterialAssetId + 1},
        .materialMode = RenderMaterialMode::Transparent,
    };
    mesh.materialSlots = {
        {
            .id = MakeId(runtimeId, entity, FrozenSceneObjectType::MaterialSlot, 0),
            .source = mesh.materialSlotsSource.values[0],
        },
        {
            .id = MakeId(runtimeId, entity, FrozenSceneObjectType::MaterialSlot, 1),
            .source = mesh.materialSlotsSource.values[1],
        },
    };
    return mesh;
}

[[nodiscard]] FrozenSceneSnapshot MakeSnapshot(
    ECS::SceneRuntimeId runtimeId,
    uint64 revision,
    bool includeSecondMesh = false,
    bool includeSkybox = false)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = runtimeId;
    snapshot.revision = revision;
    snapshot.structuralJournalSequence = revision;
    snapshot.renderWorldTransformWriteVersion = revision;

    const ECS::EntityHandle cameraEntity = ECS::EntityHandle::Create(1, 1);
    FrozenSceneCamera camera;
    camera.id = MakeId(runtimeId, cameraEntity, FrozenSceneObjectType::Camera);
    camera.source.nearPlane = 0.1f;
    camera.source.farPlane = 100.0f;
    camera.source.aspectRatio = 4.0f / 3.0f;
    camera.source.normalizedViewport = {0.0f, 0.0f, 1.0f, 1.0f};
    camera.source.cullingMask = 0x02U;
    camera.worldTransform = MakeTranslation(3.0f, 4.0f, 5.0f);
    snapshot.cameras.push_back(camera);
    snapshot.selectedCamera = camera.id;

    // Reverse source order deliberately: accumulator output must remain ordered by ID.
    if (includeSecondMesh)
    {
        snapshot.meshes.push_back(MakeMesh(runtimeId, 12, 102, 203, 20.0f));
    }
    snapshot.meshes.push_back(MakeMesh(runtimeId, 11, 101, 201, 10.0f));

    FrozenSceneLight light;
    light.id = MakeId(runtimeId, ECS::EntityHandle::Create(21, 1),
                      FrozenSceneObjectType::Light);
    light.source.type = LightType::Point;
    light.source.intensity = 3.0f;
    light.source.range = 25.0f;
    light.visibility.layerMask = 0x04U;
    light.visibility.visible = true;
    light.worldTransform = MakeTranslation(6.0f, 7.0f, 8.0f);
    snapshot.lights.push_back(light);

    if (includeSkybox)
    {
        FrozenSceneSkybox skybox;
        skybox.id = MakeId(runtimeId, ECS::EntityHandle::Create(31, 1),
                           FrozenSceneObjectType::Skybox);
        skybox.source.mode = SkyboxMode::SolidColor;
        skybox.source.contributesToLighting = false;
        snapshot.skyboxes.push_back(skybox);
    }
    return snapshot;
}

[[nodiscard]] EcsFrozenSceneBridgeOutput MakeBridgeOutput(
    ECS::SceneRuntimeId runtimeId,
    uint64 revision,
    bool includeSecondMesh = false,
    bool includeSkybox = false)
{
    EcsFrozenSceneBridgeOutput output;
    const FrozenSceneSnapshot snapshot =
        MakeSnapshot(runtimeId, revision, includeSecondMesh, includeSkybox);
    EXPECT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output));
    return output;
}

[[nodiscard]] TestAssetResolver MakeResolver()
{
    TestAssetResolver resolver;
    resolver.handles = {
        {101, {1, 1}}, {102, {2, 1}},
        {201, {3, 1}}, {202, {4, 1}},
        {203, {5, 1}}, {204, {6, 1}},
    };
    return resolver;
}

[[nodiscard]] EcsFrameExtractionInput MakeInput(
    TestAssetResolver& resolver,
    uint64 sequence)
{
    EcsFrameExtractionInput input;
    input.sequence = sequence;
    input.outputWidth = 800;
    input.outputHeight = 600;
    input.absoluteTime = 2.0f;
    input.deltaTime = 1.0f / 60.0f;
    input.assetResolver = {.context = &resolver, .resolve = &ResolveAsset};
    return input;
}

void RemoveMesh(EcsFrozenSceneBridgeOutput& output, uint64 meshAssetId)
{
    const auto mesh = std::find_if(
        output.renderProxies.primitives.begin(), output.renderProxies.primitives.end(),
        [meshAssetId](const RenderPrimitiveProxy& primitive)
        {
            return primitive.meshAssetId == AssetId{meshAssetId};
        });
    ASSERT_NE(mesh, output.renderProxies.primitives.end());
    const uint64 primitiveId = mesh->id.value;
    output.renderProxies.primitives.erase(mesh);
    std::erase_if(output.primitiveTemporalValues,
                  [primitiveId](const EcsFrozenScenePrimitiveTemporalValue& value)
                  {
                      return value.primitiveId.value == primitiveId;
                  });
    std::erase_if(output.materialBindings,
                  [primitiveId](const EcsFrozenSceneMaterialBindingValue& value)
                  {
                      return value.primitiveId.value == primitiveId;
                  });
}

void RemoveLight(EcsFrozenSceneBridgeOutput& output)
{
    ASSERT_EQ(output.renderProxies.lights.size(), 1U);
    output.renderProxies.lights.clear();
    output.lightValues.clear();
}

[[nodiscard]] EcsRenderSceneRetirementBarrier MakeBarrier(
    ECS::SceneRuntimeId runtimeId,
    uint64 correlation,
    std::initializer_list<EcsRenderSceneRetirementMember> members)
{
    EcsRenderSceneRetirementBarrier barrier;
    barrier.correlation = {.value = correlation};
    barrier.sceneRuntimeId = runtimeId;
    barrier.members = members;
    return barrier;
}
} // namespace

TEST(EcsFrameExtractionValidation,
     FullIncrementalAndNoOpPacketsPreserveEcsSidecarsWithoutViewFiltering)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    EcsFrozenSceneBridgeOutput first =
        MakeBridgeOutput(ECS::SceneRuntimeId{81}, 1, true);

    EcsFrameExtractionResult full = extractor.Extract(MakeInput(resolver, 1), first);
    ASSERT_TRUE(full.IsComplete());
    ASSERT_NE(full.sceneUpdate, nullptr);
    EXPECT_TRUE(full.sceneUpdate->fullReset);
    EXPECT_EQ(full.sceneUpdate->baseSceneRevision, 0U);
    EXPECT_EQ(full.sceneUpdate->targetSceneRevision, 1U);
    ASSERT_TRUE(full.sceneUpdate->IsStructurallyValid());
    ASSERT_EQ(full.sceneUpdate->primitives.size(), 2U);
    EXPECT_LT(full.sceneUpdate->primitives[0].objectId,
              full.sceneUpdate->primitives[1].objectId);

    const uint64 firstMeshId = std::find_if(
        first.renderProxies.primitives.begin(),
        first.renderProxies.primitives.end(),
        [](const RenderPrimitiveProxy& proxy)
        {
            return proxy.meshAssetId == AssetId{101};
        })->id.value;
    const auto primitiveMutation = std::find_if(
        full.sceneUpdate->primitives.begin(), full.sceneUpdate->primitives.end(),
        [firstMeshId](const RenderPrimitiveMutation& mutation)
        {
            return mutation.objectId == firstMeshId;
        });
    ASSERT_NE(primitiveMutation, full.sceneUpdate->primitives.end());
    const RenderPrimitiveSnapshot& primitive = primitiveMutation->state;
    EXPECT_EQ(primitive.layerMask, 0x01U);
    ASSERT_EQ(primitive.submeshes.size(), 2U);
    EXPECT_EQ(primitive.submeshes[0].submeshIndex, 0U);
    EXPECT_EQ(primitive.submeshes[0].material, (RenderResourceHandle{3, 1}));
    EXPECT_EQ(primitive.submeshes[0].materialMode, RenderMaterialMode::Masked);
    EXPECT_EQ(primitive.submeshes[1].submeshIndex, 1U);
    EXPECT_EQ(primitive.submeshes[1].material, (RenderResourceHandle{4, 1}));
    EXPECT_EQ(primitive.submeshes[1].materialMode,
              RenderMaterialMode::Transparent);
    EXPECT_FLOAT_EQ(primitive.previousWorldTransform[3].x,
                    primitive.worldTransform[3].x - 1.0f);
    ASSERT_EQ(full.sceneUpdate->lights.size(), 1U);
    EXPECT_EQ(full.sceneUpdate->lights[0].state.layerMask, 0x04U);
    ASSERT_NE(full.frameV5, nullptr);
    EXPECT_EQ(full.frameV5->GetView().cullingMask, 0x02U);
    EXPECT_EQ(full.frameV5->GetView().viewportWidth, 800U);
    // Neither scene layer overlaps the selected camera mask; both are still
    // retained in the scene update for shared Direct/GPU-driven filtering.
    EXPECT_FALSE(IsRenderLayerVisible(primitive.layerMask,
                                      full.frameV5->GetView().cullingMask));
    EXPECT_FALSE(IsRenderLayerVisible(full.sceneUpdate->lights[0].state.layerMask,
                                      full.frameV5->GetView().cullingMask));
    EXPECT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    EcsFrozenSceneBridgeOutput incremental =
        MakeBridgeOutput(ECS::SceneRuntimeId{81}, 2, true);
    incremental.renderProxies.primitives.front().worldMatrix[3].x += 3.0f;
    EcsFrameExtractionResult changed =
        extractor.Extract(MakeInput(resolver, 2), incremental);
    ASSERT_TRUE(changed.IsComplete());
    EXPECT_FALSE(changed.sceneUpdate->fullReset);
    EXPECT_EQ(changed.sceneUpdate->baseSceneRevision, 1U);
    EXPECT_EQ(changed.sceneUpdate->targetSceneRevision, 2U);
    ASSERT_EQ(changed.sceneUpdate->primitives.size(), 1U);
    EXPECT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    EcsFrozenSceneBridgeOutput unchanged =
        MakeBridgeOutput(ECS::SceneRuntimeId{81}, 3, true);
    unchanged.renderProxies.primitives.front().worldMatrix[3].x += 3.0f;
    EcsFrameExtractionResult noOp = extractor.Extract(MakeInput(resolver, 3), unchanged);
    ASSERT_TRUE(noOp.IsComplete());
    EXPECT_TRUE(noOp.IsFrameOnly());
    EXPECT_EQ(noOp.sceneUpdate, nullptr);
    EXPECT_EQ(noOp.targetRenderSceneRevision, 2U);
    EXPECT_EQ(noOp.frameV5->GetHeader().requiredSceneRevision, 2U);
    EXPECT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 2U);
    EXPECT_EQ(extractor.GetAcceptedFrameSequence(), 3U);

    EcsFrozenSceneBridgeOutput laterMutation =
        MakeBridgeOutput(ECS::SceneRuntimeId{81}, 4, true);
    laterMutation.renderProxies.primitives.front().worldMatrix[3].x += 6.0f;
    EcsFrameExtractionResult incrementalAfterFrameOnly =
        extractor.Extract(MakeInput(resolver, 4), laterMutation);
    ASSERT_TRUE(incrementalAfterFrameOnly.IsComplete());
    EXPECT_FALSE(incrementalAfterFrameOnly.IsFrameOnly());
    ASSERT_NE(incrementalAfterFrameOnly.sceneUpdate, nullptr);
    EXPECT_FALSE(incrementalAfterFrameOnly.sceneUpdate->fullReset);
    EXPECT_EQ(incrementalAfterFrameOnly.sceneUpdate->baseSceneRevision, 2U);
    EXPECT_EQ(incrementalAfterFrameOnly.sceneUpdate->targetSceneRevision, 3U);
}

TEST(EcsFrameExtractionValidation,
     CompleteCandidatesCarryUniqueExtractorQualifiedIdentityIntoResolution)
{
    TestAssetResolver resolver = MakeResolver();
    const ECS::SceneRuntimeId runtime{86};
    EcsFrameExtractor extractor;
    EcsFrameExtractor foreignExtractor;

    const EcsFrozenSceneBridgeOutput source = MakeBridgeOutput(runtime, 1);
    EcsFrameExtractionResult candidate =
        extractor.Extract(MakeInput(resolver, 1), source);
    ASSERT_TRUE(candidate.IsComplete());
    EXPECT_TRUE(candidate.candidateIdentity.IsValid());
    EXPECT_EQ(candidate.sourceSceneRuntimeId, runtime);
    EXPECT_EQ(candidate.sourceSnapshotRevision, 1U);
    EXPECT_EQ(extractor.GetPendingCandidateIdentity(), candidate.candidateIdentity);
    EXPECT_TRUE(extractor.MatchesPendingCandidate(
        candidate.candidateIdentity,
        candidate.sourceSceneRuntimeId,
        candidate.sourceSnapshotRevision,
        candidate.sceneUpdate->targetSceneRevision,
        candidate.frameV5->GetHeader().sequence));

    EcsFrameExtractionResult foreign =
        foreignExtractor.Extract(MakeInput(resolver, 1), source);
    ASSERT_TRUE(foreign.IsComplete());
    EXPECT_NE(candidate.candidateIdentity, foreign.candidateIdentity);

    const EcsRenderSceneRetirementResolution resolution =
        extractor.ResolveLastPublication(
            EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    EXPECT_TRUE(resolution.resolvedCandidate);
    EXPECT_EQ(resolution.candidateIdentity, candidate.candidateIdentity);
    EXPECT_EQ(resolution.disposition,
              EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    EXPECT_FALSE(extractor.GetPendingCandidateIdentity().IsValid());
}

TEST(EcsFrameExtractionValidation,
     RuntimeIdentitySwapForcesAFullResetWithContiguousRenderRevision)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(ECS::SceneRuntimeId{91}, 40))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    EcsFrameExtractionResult swapped = extractor.Extract(
        MakeInput(resolver, 2), MakeBridgeOutput(ECS::SceneRuntimeId{92}, 1));
    ASSERT_TRUE(swapped.IsComplete());
    EXPECT_TRUE(swapped.sceneUpdate->fullReset);
    EXPECT_EQ(swapped.sceneUpdate->baseSceneRevision, 0U);
    EXPECT_EQ(swapped.sceneUpdate->targetSceneRevision, 2U);
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    EXPECT_EQ(extractor.GetAcceptedSceneRuntimeId(), ECS::SceneRuntimeId{92});
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 2U);
}

TEST(EcsFrameExtractionValidation,
     CameraClearPolicyAndColorMapExactlyIntoTheV5ViewContract)
{
    TestAssetResolver resolver = MakeResolver();
    const ECS::SceneRuntimeId runtime{96};
    const std::array<std::pair<CameraClearPolicy, RenderViewClearPolicy>, 4>
        policies = {{
            {CameraClearPolicy::Skybox, RenderViewClearPolicy::Skybox},
            {CameraClearPolicy::SolidColor, RenderViewClearPolicy::SolidColor},
            {CameraClearPolicy::DepthOnly, RenderViewClearPolicy::DepthOnly},
            {CameraClearPolicy::Nothing, RenderViewClearPolicy::Nothing},
        }};

    uint64 sequence = 1;
    for (const auto& [sourcePolicy, expectedPolicy] : policies)
    {
        EcsFrameExtractor extractor;
        EcsFrozenSceneBridgeOutput output =
            MakeBridgeOutput(runtime, sequence);
        ASSERT_EQ(output.cameras.size(), 1U);
        output.cameras[0].clearPolicy = sourcePolicy;
        output.cameras[0].clearColor = {0.2f, 0.4f, 0.6f, 0.8f};

        EcsFrameExtractionResult result =
            extractor.Extract(MakeInput(resolver, sequence), output);
        ASSERT_TRUE(result.IsComplete());
        ASSERT_NE(result.frameV5, nullptr);
        EXPECT_EQ(result.frameV5->GetView().clearPolicy, expectedPolicy);
        EXPECT_FLOAT_EQ(result.frameV5->GetView().clearColor.x, 0.2f);
        EXPECT_FLOAT_EQ(result.frameV5->GetView().clearColor.w, 0.8f);
        ++sequence;
    }
}

TEST(EcsFrameExtractionValidation,
     RejectedCandidateDoesNotAdvanceAcceptedBaselineAndSourceGapResets)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{101};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 8))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    EcsFrameExtractionResult rejected = extractor.Extract(
        MakeInput(resolver, 2), MakeBridgeOutput(runtime, 9));
    ASSERT_TRUE(rejected.IsComplete());
    EXPECT_TRUE(rejected.IsFrameOnly());
    EXPECT_EQ(rejected.sceneUpdate, nullptr);
    EXPECT_EQ(rejected.targetRenderSceneRevision, 1U);
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::NotAccepted));
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 1U);
    EXPECT_FALSE(extractor.HasPendingCandidate());

    EcsFrameExtractionResult gap = extractor.Extract(
        MakeInput(resolver, 3), MakeBridgeOutput(runtime, 10));
    ASSERT_TRUE(gap.IsComplete());
    EXPECT_TRUE(gap.sceneUpdate->fullReset);
    EXPECT_EQ(gap.sceneUpdate->baseSceneRevision, 0U);
    EXPECT_EQ(gap.sceneUpdate->targetSceneRevision, 2U);
}

TEST(EcsFrameExtractionValidation,
     FailedAssetResolutionFailsClosedAndLeavesTheAcceptedBaselineUntouched)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{111};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    resolver.rejectedAssetId = 101;
    EcsFrameExtractionResult failed = extractor.Extract(
        MakeInput(resolver, 2), MakeBridgeOutput(runtime, 2));
    EXPECT_EQ(failed.code, EcsFrameExtractionResultCode::RequiredAssetUnresolved);
    EXPECT_FALSE(failed.IsComplete());
    EXPECT_FALSE(extractor.HasPendingCandidate());
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 1U);

    resolver.rejectedAssetId = 0;
    EcsFrameExtractionResult retry = extractor.Extract(
        MakeInput(resolver, 3), MakeBridgeOutput(runtime, 2));
    ASSERT_TRUE(retry.IsComplete());
    EXPECT_TRUE(retry.IsFrameOnly());
    EXPECT_EQ(retry.sceneUpdate, nullptr);
    EXPECT_EQ(retry.targetRenderSceneRevision, 1U);
}

TEST(EcsFrameExtractionValidation,
     SceneOnlyAcceptanceAdvancesSceneButDefersRetirementUntilNextAcceptedFrame)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{121};
    const EcsRenderSceneRetirementMember meshMember{
        ECS::EntityHandle::Create(11, 1),
        EcsRenderSceneRetainedMemberType::Mesh};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    EXPECT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    EXPECT_EQ(extractor.GetAcceptedFrameSequence(), 1U);

    ASSERT_TRUE(extractor.SubmitRetirementBarrier(
                    MakeBarrier(runtime, 701, {meshMember}))
                    .IsAccepted());
    EcsFrozenSceneBridgeOutput removed = MakeBridgeOutput(runtime, 2);
    RemoveMesh(removed, 101);
    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 2), removed).IsComplete());
    const EcsRenderSceneRetirementResolution sceneOnly =
        extractor.ResolveLastPublication(
            EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    EXPECT_TRUE(sceneOnly);
    EXPECT_TRUE(sceneOnly.Empty());
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 2U);
    EXPECT_EQ(extractor.GetAcceptedFrameSequence(), 1U);

    EcsFrozenSceneBridgeOutput recovery = removed;
    recovery.snapshotRevision = 3;
    recovery.renderProxies.metadata.sequence = 3;
    EcsFrameExtractionResult recoveryFull =
        extractor.Extract(MakeInput(resolver, 3), recovery);
    ASSERT_TRUE(recoveryFull.IsComplete());
    EXPECT_TRUE(recoveryFull.sceneUpdate->fullReset);
    EXPECT_TRUE(extractor.HasPendingCandidate());
    const EcsFrameExtractionResult recoveryCandidate = extractor.Extract(
        MakeInput(resolver, 4), recovery);
    EXPECT_EQ(recoveryCandidate.code,
              EcsFrameExtractionResultCode::CandidatePublicationPending);

    const EcsRenderSceneRetirementResolution recovered =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    ASSERT_TRUE(recovered);
    ASSERT_EQ(recovered.bindingCount, 1U);
    EXPECT_EQ(recovered.bindings[0].correlation.value, 701U);
    EXPECT_EQ(recovered.bindings[0].targetSceneRevision, 2U);
    EXPECT_EQ(recovered.bindings[0].minimumFrameSequence, 3U);
    EXPECT_EQ(extractor.GetAcceptedFrameSequence(), 3U);
}

TEST(EcsFrameExtractionValidation,
     SceneOnlyRemovalReappearingBeforePresentationRearmsTheExactBarrier)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{126};
    const EcsRenderSceneRetirementMember meshMember{
        ECS::EntityHandle::Create(11, 1),
        EcsRenderSceneRetainedMemberType::Mesh};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    ASSERT_TRUE(extractor.SubmitRetirementBarrier(
                    MakeBarrier(runtime, 706, {meshMember}))
                    .IsAccepted());

    EcsFrozenSceneBridgeOutput removed = MakeBridgeOutput(runtime, 2);
    RemoveMesh(removed, 101);
    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 2), removed).IsComplete());
    const EcsRenderSceneRetirementResolution sceneOnly =
        extractor.ResolveLastPublication(
            EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    ASSERT_TRUE(sceneOnly.resolvedCandidate);
    EXPECT_TRUE(sceneOnly.Empty());

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 3),
                                  MakeBridgeOutput(runtime, 3))
                    .IsComplete());
    const EcsRenderSceneRetirementResolution reintroduced =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    EXPECT_TRUE(reintroduced.resolvedCandidate);
    EXPECT_TRUE(reintroduced.Empty());

    EcsFrozenSceneBridgeOutput removedAgain = MakeBridgeOutput(runtime, 4);
    RemoveMesh(removedAgain, 101);
    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 4), removedAgain).IsComplete());
    const EcsRenderSceneRetirementResolution released =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    ASSERT_EQ(released.bindingCount, 1U);
    EXPECT_EQ(released.bindings[0].correlation.value, 706U);
    EXPECT_EQ(released.bindings[0].targetSceneRevision, 4U);
    EXPECT_EQ(released.bindings[0].minimumFrameSequence, 4U);
}

TEST(EcsFrameExtractionValidation,
     RejectedCandidatesConsumeTheirCompletedSequenceWithoutAdvancingScene)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{131};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 2),
                                  MakeBridgeOutput(runtime, 2))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::NotAccepted));
    EXPECT_EQ(extractor.GetAcceptedSceneRevision(), 1U);

    EcsFrameExtractionResult repeated = extractor.Extract(
        MakeInput(resolver, 2), MakeBridgeOutput(runtime, 3));
    EXPECT_EQ(repeated.code,
              EcsFrameExtractionResultCode::NonMonotonicFrameSequence);
    EXPECT_FALSE(repeated.IsComplete());

    EcsFrameExtractionResult next = extractor.Extract(
        MakeInput(resolver, 3), MakeBridgeOutput(runtime, 3));
    ASSERT_TRUE(next.IsComplete());
    EXPECT_TRUE(next.sceneUpdate->fullReset);
}

TEST(EcsFrameExtractionValidation,
     RetirementBarrierCapturesExactEcsMembersAndReleasesAfterContiguousRemoval)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{141};
    const EcsRenderSceneRetirementMember meshMember{
        ECS::EntityHandle::Create(11, 1),
        EcsRenderSceneRetainedMemberType::Mesh};
    const EcsRenderSceneRetirementMember lightMember{
        ECS::EntityHandle::Create(21, 1),
        EcsRenderSceneRetainedMemberType::Light};
    const EcsRenderSceneRetirementMember skyMember{
        ECS::EntityHandle::Create(31, 1),
        EcsRenderSceneRetainedMemberType::Skybox};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1, false, true))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    const EcsRenderSceneRetirementBarrierSubmitResult submitted =
        extractor.SubmitRetirementBarrier(
            MakeBarrier(runtime, 711, {meshMember, lightMember, skyMember}));
    ASSERT_TRUE(submitted.IsAccepted());
    EXPECT_EQ(submitted.publishedMemberCount, 3U);
    EXPECT_EQ(submitted.ignoredNeverPublishedMemberCount, 0U);

    EcsFrozenSceneBridgeOutput removed =
        MakeBridgeOutput(runtime, 2, false, false);
    RemoveMesh(removed, 101);
    RemoveLight(removed);
    EcsFrameExtractionResult candidate = extractor.Extract(MakeInput(resolver, 2), removed);
    ASSERT_TRUE(candidate.IsComplete());
    EXPECT_FALSE(candidate.sceneUpdate->fullReset);
    ASSERT_EQ(candidate.sceneUpdate->primitives.size(), 1U);
    EXPECT_EQ(candidate.sceneUpdate->primitives[0].operation,
              RenderSceneMutationOperation::Remove);
    ASSERT_EQ(candidate.sceneUpdate->lights.size(), 1U);
    EXPECT_EQ(candidate.sceneUpdate->lights[0].operation,
              RenderSceneMutationOperation::Remove);
    ASSERT_TRUE(candidate.sceneUpdate->sky.has_value());
    EXPECT_EQ(candidate.sceneUpdate->sky->operation,
              RenderSceneMutationOperation::Remove);

    const EcsRenderSceneRetirementResolution resolution =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    ASSERT_EQ(resolution.bindingCount, 1U);
    EXPECT_EQ(resolution.bindings[0].correlation.value, 711U);
    EXPECT_EQ(resolution.bindings[0].targetSceneRevision, 2U);
    EXPECT_EQ(resolution.bindings[0].minimumFrameSequence, 2U);
}

TEST(EcsFrameExtractionValidation,
     HiddenMeshDoesNotResolveGpuAssetsAndRemovesAnAcceptedPrimitive)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{146};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    FrozenSceneSnapshot hiddenSnapshot = MakeSnapshot(runtime, 2);
    ASSERT_EQ(hiddenSnapshot.meshes.size(), 1u);
    hiddenSnapshot.meshes.front().visibility.visible = false;
    EcsFrozenSceneBridgeOutput hiddenOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(hiddenSnapshot, hiddenOutput));
    ASSERT_TRUE(hiddenOutput.renderProxies.primitives.empty());

    // If extraction attempted to resolve the hidden mesh this candidate would
    // fail. Its absence must instead produce the reliable retained-state Remove.
    resolver.rejectedAssetId = 101;
    EcsFrameExtractionResult hidden =
        extractor.Extract(MakeInput(resolver, 2), hiddenOutput);
    ASSERT_TRUE(hidden.IsComplete());
    ASSERT_NE(hidden.sceneUpdate, nullptr);
    ASSERT_EQ(hidden.sceneUpdate->primitives.size(), 1u);
    EXPECT_EQ(hidden.sceneUpdate->primitives.front().operation,
              RenderSceneMutationOperation::Remove);
}

TEST(EcsFrameExtractionValidation,
     FullResetOmissionSatisfiesAConfirmedEcsRetirementBarrier)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId firstRuntime{151};
    const ECS::SceneRuntimeId secondRuntime{152};
    const EcsRenderSceneRetirementMember meshMember{
        ECS::EntityHandle::Create(11, 1),
        EcsRenderSceneRetainedMemberType::Mesh};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(firstRuntime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    ASSERT_TRUE(extractor.SubmitRetirementBarrier(
                    MakeBarrier(firstRuntime, 721, {meshMember}))
                    .IsAccepted());

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 2),
                                  MakeBridgeOutput(secondRuntime, 1))
                    .IsComplete());
    const EcsRenderSceneRetirementResolution resolution =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    ASSERT_EQ(resolution.bindingCount, 1U);
    EXPECT_EQ(resolution.bindings[0].correlation.value, 721U);
    EXPECT_EQ(resolution.bindings[0].targetSceneRevision, 2U);
    EXPECT_EQ(resolution.bindings[0].minimumFrameSequence, 2U);
}

TEST(EcsFrameExtractionValidation,
     RetirementBarrierRejectsNeverPublishedMismatchedPendingDuplicateAndCapacity)
{
    TestAssetResolver resolver = MakeResolver();
    EcsFrameExtractor extractor;
    const ECS::SceneRuntimeId runtime{161};
    const EcsRenderSceneRetirementMember meshMember{
        ECS::EntityHandle::Create(11, 1),
        EcsRenderSceneRetainedMemberType::Mesh};
    const EcsRenderSceneRetirementMember neverPublishedMember{
        ECS::EntityHandle::Create(99, 1),
        EcsRenderSceneRetainedMemberType::Mesh};

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 1),
                                  MakeBridgeOutput(runtime, 1))
                    .IsComplete());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));
    EXPECT_EQ(extractor.SubmitRetirementBarrier(
                  MakeBarrier(runtime, 730, {neverPublishedMember}))
                  .code,
              EcsRenderSceneRetirementBarrierSubmitCode::NeverPublishedIdentity);
    EXPECT_EQ(extractor.SubmitRetirementBarrier(
                  MakeBarrier(ECS::SceneRuntimeId{162}, 731, {meshMember}))
                  .code,
              EcsRenderSceneRetirementBarrierSubmitCode::SceneRuntimeMismatch);

    ASSERT_TRUE(extractor.SubmitRetirementBarrier(
                    MakeBarrier(runtime, 732, {meshMember}))
                    .IsAccepted());
    EXPECT_EQ(extractor.SubmitRetirementBarrier(
                  MakeBarrier(runtime, 732, {meshMember}))
                  .code,
              EcsRenderSceneRetirementBarrierSubmitCode::DuplicateCorrelation);
    for (uint64 correlation = 733;
         correlation < 733 + RVX_ECS_RENDER_SCENE_RETIREMENT_MAX_BINDINGS - 1;
         ++correlation)
    {
        ASSERT_TRUE(extractor.SubmitRetirementBarrier(
                        MakeBarrier(runtime, correlation, {meshMember}))
                        .IsAccepted());
    }
    EXPECT_EQ(extractor.SubmitRetirementBarrier(
                  MakeBarrier(runtime, 900, {meshMember}))
                  .code,
              EcsRenderSceneRetirementBarrierSubmitCode::CapacityExceeded);

    ASSERT_TRUE(extractor.Extract(MakeInput(resolver, 2),
                                  MakeBridgeOutput(runtime, 2))
                    .IsComplete());
    EXPECT_EQ(extractor.SubmitRetirementBarrier(
                  MakeBarrier(runtime, 901, {meshMember}))
                  .code,
              EcsRenderSceneRetirementBarrierSubmitCode::PublicationPending);
}

TEST(EcsFrameExtractionValidation, NumericPoisoningFailsClosedForEveryEcsSourceKind)
{
    TestAssetResolver resolver = MakeResolver();
    const ECS::SceneRuntimeId runtime{171};
    const float32 nan = std::numeric_limits<float32>::quiet_NaN();
    const float32 infinity = std::numeric_limits<float32>::infinity();

    EcsFrozenSceneBridgeOutput poisonedCamera = MakeBridgeOutput(runtime, 1);
    poisonedCamera.cameras[0].view.viewMatrix[0].x = nan;
    EcsFrameExtractor cameraExtractor;
    EXPECT_FALSE(cameraExtractor.Extract(MakeInput(resolver, 1), poisonedCamera)
                     .IsComplete());

    EcsFrozenSceneBridgeOutput poisonedPrimitive = MakeBridgeOutput(runtime, 1);
    poisonedPrimitive.renderProxies.primitives[0].bounds.GetMax().x = nan;
    EcsFrameExtractor primitiveExtractor;
    EXPECT_FALSE(primitiveExtractor.Extract(MakeInput(resolver, 1), poisonedPrimitive)
                     .IsComplete());

    EcsFrozenSceneBridgeOutput poisonedLight = MakeBridgeOutput(runtime, 1);
    poisonedLight.lightValues[0].proxy.intensity = infinity;
    EcsFrameExtractor lightExtractor;
    EXPECT_FALSE(lightExtractor.Extract(MakeInput(resolver, 1), poisonedLight)
                     .IsComplete());

    EcsFrozenSceneBridgeOutput poisonedSky =
        MakeBridgeOutput(runtime, 1, false, true);
    poisonedSky.skyboxes[0].exposure = nan;
    EcsFrameExtractor skyAndEnvironmentExtractor;
    EXPECT_FALSE(skyAndEnvironmentExtractor
                     .Extract(MakeInput(resolver, 1), poisonedSky)
                     .IsComplete());
}
