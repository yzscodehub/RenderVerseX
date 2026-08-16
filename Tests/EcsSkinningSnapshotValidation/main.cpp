#include "AnimationSceneAdapters/ECS/AnimationEcsBridge.h"
#include "Core/Diagnostics/SkinningPaletteHash.h"
#include "RenderExtraction/ECS/EcsFrameExtractor.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"
#include "Scene/ECS/SceneSkinningSnapshotStore.h"

#include <gtest/gtest.h>

#include <limits>
#include <unordered_map>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;

[[nodiscard]] SceneSkinningPaletteSnapshot MakePalette(
    ECS::EntityHandle poseEntity,
    uint64 sourceModelAssetValue = 700,
    int32 sourceSkinIndex = 3)
{
    SceneSkinningPaletteSnapshot palette;
    palette.poseEntity = poseEntity;
    palette.sourceModelAssetValue = sourceModelAssetValue;
    palette.sourceSkinIndex = sourceSkinIndex;
    palette.poseSequence = 11;
    palette.paletteRevision = 17;
    palette.paletteBoneCount = 2;
    palette.matrices.assign(palette.paletteBoneCount, Mat4(1.0f));
    return palette;
}

[[nodiscard]] FrozenSceneSnapshot MakeSkinnedSnapshot(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle meshEntity,
    ECS::EntityHandle poseEntity,
    uint64 revision = 1)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = sceneRuntimeId;
    snapshot.revision = revision;
    FrozenSceneMesh mesh;
    mesh.id = MakeFrozenSceneObjectId(
        sceneRuntimeId, meshEntity, FrozenSceneObjectType::Mesh);
    mesh.sourceEntity = meshEntity;
    mesh.skinningBinding = {
        .poseEntity = poseEntity,
        .sourceModelAssetValue = 700,
        .sourceSkinIndex = 3,
    };
    snapshot.meshes.push_back(std::move(mesh));
    return snapshot;
}

void AddSkinnedMesh(SceneEcsRuntime& runtime, ECS::EntityHandle entity)
{
    Mesh mesh;
    mesh.meshAssetId = {.value = 101};
    mesh.submeshCount = 1;
    MaterialSlots materials;
    materials.count = 1;
    materials.values[0].materialAssetId = {.value = 201};
    AnimationSkeletonBinding skeleton;
    skeleton.animationAssetValue = 501;
    skeleton.sourceModelAssetValue = 700;
    skeleton.sourceSkinIndex = 3;
    skeleton.boneCount = 2;
    SkinnedMeshBinding binding;
    binding.poseEntity = entity;
    binding.sourceModelAssetValue = skeleton.sourceModelAssetValue;
    binding.sourceSkinIndex = skeleton.sourceSkinIndex;

    ASSERT_TRUE(runtime.AddFragment<Mesh>(entity, mesh));
    ASSERT_TRUE(runtime.AddFragment<MaterialSlots>(entity, materials));
    ASSERT_TRUE(runtime.AddFragment<Visibility>(entity));
    ASSERT_TRUE(runtime.AddFragment<Camera>(entity));
    ASSERT_TRUE(runtime.AddFragment<AnimationSkeletonBinding>(entity, skeleton));
    ASSERT_TRUE(runtime.AddFragment<Animator>(entity));
    ASSERT_TRUE(runtime.AddFragment<AnimationPoseState>(entity));
    ASSERT_TRUE(runtime.AddFragment<SkinnedMeshBinding>(entity, binding));
}

struct Resolver
{
    std::unordered_map<uint64, RenderResourceHandle> handles;
};

[[nodiscard]] RenderResourceHandle ResolveAsset(
    void* context,
    AssetId assetId,
    RenderResourceKind) noexcept
{
    const auto* resolver = static_cast<const Resolver*>(context);
    if (resolver == nullptr)
    {
        return {};
    }
    const auto found = resolver->handles.find(assetId.value);
    return found != resolver->handles.end() ? found->second : RenderResourceHandle{};
}
} // namespace

TEST(EcsSkinningSnapshotValidation,
     StoreCopiesExactValuesAndFailsClosedForMissingMismatchedAndNonFinitePalettes)
{
    ECS::Registry registry;
    SceneSkinningSnapshotStore store(registry.GetSceneRuntimeId());
    const ECS::EntityHandle mesh = registry.CreateEntity();
    const ECS::EntityHandle pose = registry.CreateEntity();
    const ECS::EntityRef meshSource = registry.GetRef(mesh);

    FrozenSceneSnapshot missing = MakeSkinnedSnapshot(
        registry.GetSceneRuntimeId(), mesh, pose);
    EXPECT_FALSE(store.FreezeInto(missing));

    ASSERT_TRUE(store.Publish(meshSource, MakePalette(pose)));
    FrozenSceneSnapshot complete = MakeSkinnedSnapshot(
        registry.GetSceneRuntimeId(), mesh, pose, 2);
    ASSERT_TRUE(store.FreezeInto(complete));
    ASSERT_TRUE(complete.meshes.front().skinningPalette.has_value());
    const FrozenSceneSkinningPalette& frozen =
        *complete.meshes.front().skinningPalette;
    EXPECT_EQ(frozen.id.type, FrozenSceneObjectType::SkinningPalette);
    EXPECT_EQ(frozen.id.entity, mesh);
    EXPECT_EQ(frozen.poseEntity, pose);
    EXPECT_EQ(frozen.paletteBoneCount, 2u);
    EXPECT_EQ(store.GetCounts().paletteCount, 1u);

    FrozenSceneSnapshot mismatch = MakeSkinnedSnapshot(
        registry.GetSceneRuntimeId(), mesh, pose, 3);
    mismatch.meshes.front().skinningBinding->sourceSkinIndex = 4;
    EXPECT_FALSE(store.FreezeInto(mismatch));

    SceneSkinningPaletteSnapshot nonFinite = MakePalette(pose);
    nonFinite.matrices.front()[0][0] = std::numeric_limits<float32>::quiet_NaN();
    EXPECT_FALSE(store.Publish(meshSource, std::move(nonFinite)));
    EXPECT_EQ(store.GetCounts().paletteCount, 1u);

    EXPECT_TRUE(store.Remove(pose));
    EXPECT_EQ(store.GetCounts().paletteCount, 0u);
}

TEST(EcsSkinningSnapshotValidation,
     AnimationFeaturePublicationFreezesCanonicalPaletteAndFrameExtractionConsumesIt)
{
    SceneEcsRuntime runtime;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            AnimationSceneAdapters::AnimationEcsEvaluatedPose pose;
            pose.skinningPalette.assign(request.skeleton.boneCount, Mat4(1.0f));
            pose.skinningPalette.front()[3].x = 2.0f;
            return pose;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    AddSkinnedMesh(runtime, entity);
    EXPECT_FALSE(runtime.PublishSkinningPaletteSnapshot(entity, MakePalette(entity)));

    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    const FrozenSceneSnapshot& snapshot = *runtime.GetLatestFrozenSnapshot();
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    const FrozenSceneMesh& mesh = snapshot.meshes.front();
    ASSERT_TRUE(mesh.skinningBinding.has_value());
    ASSERT_TRUE(mesh.skinningPalette.has_value());
    EXPECT_EQ(mesh.skinningPalette->poseEntity, entity);
    EXPECT_EQ(mesh.skinningPalette->sourceModelAssetValue, 700u);
    EXPECT_EQ(mesh.skinningPalette->sourceSkinIndex, 3);
    EXPECT_EQ(mesh.skinningPalette->paletteBoneCount, 2u);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 1u);

    EcsFrozenSceneBridgeOutput bridgeOutput;
    ASSERT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, bridgeOutput));
    ASSERT_EQ(bridgeOutput.renderProxies.primitives.size(), 1u);
    const RenderPrimitiveProxy& proxy = bridgeOutput.renderProxies.primitives.front();
    EXPECT_TRUE(proxy.hasSkinningPaletteProvider);
    ASSERT_TRUE(proxy.skinningPalette.IsValidFor(proxy.skinningMatrices));
    EXPECT_EQ(proxy.skinningPalette.providerComponentId,
              EcsFrozenSceneBridge::DeriveRenderId(mesh.skinningPalette->id));
    EXPECT_EQ(proxy.skinningPalette.sourceModelResourceId, 700u);
    EXPECT_EQ(proxy.skinningPalette.poseSequence, mesh.skinningPalette->poseSequence);
    EXPECT_EQ(proxy.skinningPalette.paletteHash,
              ComputeSkinningPaletteHash(proxy.skinningMatrices).value);

    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    EcsFrameExtractionInput input;
    input.sequence = 1;
    input.outputWidth = 640;
    input.outputHeight = 480;
    input.assetResolver = {.context = &resolver, .resolve = &ResolveAsset};
    EcsFrameExtractor extractor;
    const EcsFrameExtractionResult extracted = extractor.Extract(input, bridgeOutput);
    ASSERT_TRUE(extracted.IsComplete());
    ASSERT_EQ(extracted.sceneUpdate->primitives.size(), 1u);

    ASSERT_EQ(runtime.RequestDestroy(entity), SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 0u);
}
