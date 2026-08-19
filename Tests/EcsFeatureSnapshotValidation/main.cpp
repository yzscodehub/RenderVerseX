#include "RenderExtraction/ECS/EcsFrameExtractor.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "Scene/ECS/SceneFeatureSnapshotStore.h"

#include "ECS/Registry.h"

#include <gtest/gtest.h>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;

[[nodiscard]] RenderResourceHandle ResolveUnusedAsset(
    void*, AssetId, RenderResourceKind) noexcept
{
    return {};
}

[[nodiscard]] EcsFrameExtractionInput MakeInput(uint64 sequence)
{
    EcsFrameExtractionInput input;
    input.sequence = sequence;
    input.outputWidth = 1280;
    input.outputHeight = 720;
    input.absoluteTime = 1.0f;
    input.deltaTime = 1.0f / 60.0f;
    input.assetResolver = {.context = nullptr, .resolve = &ResolveUnusedAsset};
    return input;
}

[[nodiscard]] FrozenSceneSnapshot MakeSnapshot(
    ECS::SceneRuntimeId sceneRuntimeId,
    uint64 revision)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = sceneRuntimeId;
    snapshot.revision = revision;
    snapshot.structuralJournalSequence = revision;

    FrozenSceneCamera camera;
    camera.id = MakeFrozenSceneObjectId(
        sceneRuntimeId, ECS::EntityHandle::Create(997, 0),
        FrozenSceneObjectType::Camera);
    snapshot.cameras.push_back(camera);
    snapshot.selectedCamera = camera.id;
    return snapshot;
}

[[nodiscard]] ParticleRenderSnapshotItem MakeParticle()
{
    ParticleRenderSnapshotItem item;
    item.instanceId = 17;
    item.systemId = 23;
    item.systemAssetId = {.value = 101};
    item.systemName = "Transient legacy identity must not escape";
    item.simulationBackend = ParticleRenderSnapshotSimulationBackend::CPU;
    item.payloadStatus = ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady;
    item.aliveParticleCount = 1;
    item.maxParticleCount = 64;
    item.renderPayloadAvailable = true;
    item.particles.push_back({
        .position = {1.0f, 2.0f, 3.0f},
        .lifetime = 5.0f,
        .velocity = {4.0f, 5.0f, 6.0f},
        .age = 0.5f,
    });
    return item;
}

[[nodiscard]] WaterRenderSnapshotItem MakeWater()
{
    WaterRenderSnapshotItem item;
    item.componentId = 31;
    item.surfaceAssetId = {.value = 201};
    item.materialAssetId = {.value = 202};
    item.size = {10.0f, 20.0f};
    item.depth = 4.0f;
    item.resolution = 128;
    item.cpuSimulationAvailable = true;
    return item;
}

[[nodiscard]] TerrainRenderSnapshotItem MakeTerrain()
{
    TerrainRenderSnapshotItem item;
    item.componentId = 37;
    item.heightmapAssetId = {.value = 301};
    item.materialAssetId = {.value = 302};
    item.size = {100.0f, 10.0f, 100.0f};
    item.patchSize = 16;
    item.maxLODLevels = 6;
    item.cpuDataAvailable = true;
    return item;
}

[[nodiscard]] EcsFrozenSceneBridgeOutput BuildBridgeOutput(
    const FrozenSceneSnapshot& snapshot)
{
    EcsFrozenSceneBridgeOutput output;
    EXPECT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output));
    return output;
}
} // namespace

TEST(EcsFeatureSnapshotValidation,
     RejectsForeignAndStaleReferencesWhileRecordedGenerationRemainsRemovable)
{
    ECS::Registry registry;
    ECS::Registry foreignRegistry;
    SceneFeatureSnapshotStore store(registry.GetSceneRuntimeId());

    FrozenSceneSnapshot foreignSnapshot =
        MakeSnapshot(foreignRegistry.GetSceneRuntimeId(), 1);
    EXPECT_FALSE(store.FreezeInto(foreignSnapshot));

    const ECS::EntityHandle entity = registry.CreateEntity();
    const ECS::EntityRef source = registry.GetRef(entity);
    const std::optional<SceneFeatureSnapshotSource> recordedSource =
        store.MakeSourceIdentity(source);
    ASSERT_TRUE(recordedSource.has_value());
    EXPECT_TRUE(store.PublishParticle(source, MakeParticle(), 1));
    EXPECT_FALSE(store.PublishParticle(
        foreignRegistry.GetRef(foreignRegistry.CreateEntity()), MakeParticle(), 1));

    ASSERT_TRUE(registry.DestroyEntity(entity));
    EXPECT_FALSE(source.IsValid());
    EXPECT_FALSE(store.PublishParticle(source, MakeParticle(), 2));

    const ECS::EntityHandle replacement = registry.CreateEntity();
    ASSERT_NE(replacement, entity);
    EXPECT_TRUE(store.PublishParticle(registry.GetRef(replacement), MakeParticle(), 3));
    EXPECT_EQ(store.GetCounts().particleCount, 2U);
    EXPECT_TRUE(store.Remove(*recordedSource));
    EXPECT_TRUE(store.Remove(*recordedSource));
    EXPECT_EQ(store.GetCounts().particleCount, 1U);
}

TEST(EcsFeatureSnapshotValidation,
     DeepCopiesDynamicParticlePayloadAndExtractsFeatureFullIncrementalAndResetDeltas)
{
    ECS::Registry registry;
    SceneFeatureSnapshotStore store(registry.GetSceneRuntimeId());

    const ECS::EntityRef particleSource = registry.GetRef(registry.CreateEntity());
    const ECS::EntityRef waterSource = registry.GetRef(registry.CreateEntity());
    const ECS::EntityRef terrainSource = registry.GetRef(registry.CreateEntity());
    const std::optional<SceneFeatureSnapshotSource> particleIdentity =
        store.MakeSourceIdentity(particleSource);
    ASSERT_TRUE(particleIdentity.has_value());

    ParticleRenderSnapshotItem particle = MakeParticle();
    ASSERT_TRUE(store.PublishParticle(particleSource, particle, 11));
    ASSERT_TRUE(store.PublishWater(waterSource, MakeWater(), 12));
    ASSERT_TRUE(store.PublishTerrain(terrainSource, MakeTerrain(), 13));

    particle.particles[0].position.x = 999.0f;
    FrozenSceneSnapshot firstSnapshot = MakeSnapshot(registry.GetSceneRuntimeId(), 1);
    ASSERT_TRUE(store.FreezeInto(firstSnapshot));
    ASSERT_EQ(firstSnapshot.particles.size(), 1U);
    ASSERT_EQ(firstSnapshot.water.size(), 1U);
    ASSERT_EQ(firstSnapshot.terrain.size(), 1U);
    EXPECT_FLOAT_EQ(firstSnapshot.particles[0].state.particles[0].position.x, 1.0f);
    EXPECT_EQ(firstSnapshot.particles[0].payloadRevision, 11U);
    EXPECT_EQ(firstSnapshot.particles[0].state.systemAssetId,
              (AssetId{101}));
    EXPECT_EQ(firstSnapshot.water[0].state.surfaceAssetId, (AssetId{201}));
    EXPECT_EQ(firstSnapshot.water[0].state.materialAssetId, (AssetId{202}));
    EXPECT_EQ(firstSnapshot.terrain[0].state.heightmapAssetId,
              (AssetId{301}));
    EXPECT_EQ(firstSnapshot.terrain[0].state.materialAssetId,
              (AssetId{302}));

    EcsFrozenSceneBridgeOutput firstBridge = BuildBridgeOutput(firstSnapshot);
    ASSERT_EQ(firstBridge.particles.size(), 1U);
    ASSERT_EQ(firstBridge.water.size(), 1U);
    ASSERT_EQ(firstBridge.terrain.size(), 1U);
    const uint64 particleRenderId = EcsFrozenSceneBridge::DeriveRenderId(
        firstSnapshot.particles[0].id);
    const uint64 waterRenderId = EcsFrozenSceneBridge::DeriveRenderId(
        firstSnapshot.water[0].id);
    const uint64 terrainRenderId = EcsFrozenSceneBridge::DeriveRenderId(
        firstSnapshot.terrain[0].id);
    EXPECT_EQ(firstBridge.particles[0].id, particleRenderId);
    EXPECT_EQ(firstBridge.particles[0].state.instanceId, particleRenderId);
    EXPECT_EQ(firstBridge.particles[0].state.systemId, particleRenderId);
    EXPECT_EQ(firstBridge.particles[0].state.systemAssetId, (AssetId{101}));
    EXPECT_EQ(firstBridge.water[0].state.componentId, waterRenderId);
    EXPECT_EQ(firstBridge.water[0].state.surfaceAssetId, (AssetId{201}));
    EXPECT_EQ(firstBridge.water[0].state.materialAssetId, (AssetId{202}));
    EXPECT_EQ(firstBridge.terrain[0].state.componentId, terrainRenderId);
    EXPECT_EQ(firstBridge.terrain[0].state.heightmapAssetId, (AssetId{301}));
    EXPECT_EQ(firstBridge.terrain[0].state.materialAssetId, (AssetId{302}));

    // Re-publishing cannot mutate an already frozen snapshot.
    ParticleRenderSnapshotItem replacementParticle = MakeParticle();
    replacementParticle.particles[0].position.x = 42.0f;
    ASSERT_TRUE(store.PublishParticle(particleSource, replacementParticle, 14));
    EXPECT_FLOAT_EQ(firstSnapshot.particles[0].state.particles[0].position.x, 1.0f);

    EcsFrameExtractor extractor;
    EcsFrameExtractionResult first = extractor.Extract(MakeInput(1), firstBridge);
    ASSERT_TRUE(first.IsComplete());
    EXPECT_TRUE(first.sceneUpdate->fullReset);
    ASSERT_EQ(first.sceneUpdate->particles.size(), 1U);
    ASSERT_EQ(first.sceneUpdate->water.size(), 1U);
    ASSERT_EQ(first.sceneUpdate->terrain.size(), 1U);
    EXPECT_EQ(first.sceneUpdate->particles[0].operation,
              RenderSceneMutationOperation::Upsert);
    EXPECT_EQ(first.sceneUpdate->particles[0].state.instanceId, particleRenderId);
    EXPECT_EQ(first.sceneUpdate->particles[0].state.systemId, particleRenderId);
    EXPECT_EQ(first.sceneUpdate->particles[0].state.systemAssetId,
              (AssetId{101}));
    EXPECT_EQ(first.sceneUpdate->water[0].state.componentId, waterRenderId);
    EXPECT_EQ(first.sceneUpdate->water[0].state.surfaceAssetId, (AssetId{201}));
    EXPECT_EQ(first.sceneUpdate->water[0].state.materialAssetId, (AssetId{202}));
    EXPECT_EQ(first.sceneUpdate->terrain[0].state.componentId, terrainRenderId);
    EXPECT_EQ(first.sceneUpdate->terrain[0].state.heightmapAssetId,
              (AssetId{301}));
    EXPECT_EQ(first.sceneUpdate->terrain[0].state.materialAssetId,
              (AssetId{302}));
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    ASSERT_TRUE(store.RemoveParticle(*particleIdentity));
    WaterRenderSnapshotItem updatedWater = MakeWater();
    updatedWater.depth = 9.0f;
    updatedWater.materialAssetId = {.value = 222};
    ASSERT_TRUE(store.PublishWater(waterSource, updatedWater, 15));
    FrozenSceneSnapshot secondSnapshot = MakeSnapshot(registry.GetSceneRuntimeId(), 2);
    ASSERT_TRUE(store.FreezeInto(secondSnapshot));
    EcsFrozenSceneBridgeOutput secondBridge = BuildBridgeOutput(secondSnapshot);
    EcsFrameExtractionResult second = extractor.Extract(MakeInput(2), secondBridge);
    ASSERT_TRUE(second.IsComplete());
    EXPECT_FALSE(second.sceneUpdate->fullReset);
    ASSERT_EQ(second.sceneUpdate->particles.size(), 1U);
    EXPECT_EQ(second.sceneUpdate->particles[0].operation,
              RenderSceneMutationOperation::Remove);
    ASSERT_EQ(second.sceneUpdate->water.size(), 1U);
    EXPECT_EQ(second.sceneUpdate->water[0].operation,
              RenderSceneMutationOperation::Upsert);
    EXPECT_FLOAT_EQ(second.sceneUpdate->water[0].state.depth, 9.0f);
    EXPECT_EQ(second.sceneUpdate->water[0].state.surfaceAssetId,
              (AssetId{201}));
    EXPECT_EQ(second.sceneUpdate->water[0].state.materialAssetId,
              (AssetId{222}));
    EXPECT_TRUE(second.sceneUpdate->terrain.empty());
    ASSERT_TRUE(extractor.ResolveLastPublication(
        EcsFramePublicationDisposition::Accepted));

    // A source revision discontinuity forces an authoritative full feature reset.
    FrozenSceneSnapshot discontinuousSnapshot =
        MakeSnapshot(registry.GetSceneRuntimeId(), 4);
    ASSERT_TRUE(store.FreezeInto(discontinuousSnapshot));
    EcsFrameExtractionResult discontinuous = extractor.Extract(
        MakeInput(3), BuildBridgeOutput(discontinuousSnapshot));
    ASSERT_TRUE(discontinuous.IsComplete());
    EXPECT_TRUE(discontinuous.sceneUpdate->fullReset);
    EXPECT_TRUE(discontinuous.sceneUpdate->particles.empty());
    ASSERT_EQ(discontinuous.sceneUpdate->water.size(), 1U);
    ASSERT_EQ(discontinuous.sceneUpdate->terrain.size(), 1U);
    EXPECT_EQ(discontinuous.sceneUpdate->water[0].state.componentId,
              waterRenderId);
    EXPECT_EQ(discontinuous.sceneUpdate->water[0].state.surfaceAssetId,
              (AssetId{201}));
    EXPECT_EQ(discontinuous.sceneUpdate->water[0].state.materialAssetId,
              (AssetId{222}));
    EXPECT_EQ(discontinuous.sceneUpdate->terrain[0].state.componentId,
              terrainRenderId);
    EXPECT_EQ(discontinuous.sceneUpdate->terrain[0].state.heightmapAssetId,
              (AssetId{301}));
    EXPECT_EQ(discontinuous.sceneUpdate->terrain[0].state.materialAssetId,
              (AssetId{302}));
}

TEST(EcsFeatureSnapshotValidation,
     RuntimePublishesOnlyDuringFeatureAndCleanupRemovalReachesTheNextFreeze)
{
    SceneEcsRuntime runtime;
    const ECS::EntityHandle source = runtime.CreateEntity();
    ASSERT_TRUE(source.IsValid());

    EXPECT_FALSE(runtime.PublishParticleFeatureSnapshot(source, MakeParticle(), 1));
    EXPECT_FALSE(runtime.RemoveFeatureSnapshots(source));

    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "publish-particle-feature-value",
        .phase = ECS::ProcessorPhase::Feature,
        .run = [&runtime, source](ECS::Registry&)
        {
            EXPECT_TRUE(runtime.PublishParticleFeatureSnapshot(source, MakeParticle(), 7));
        },
    }));

    uint64 observedPayloadRevision = 0;
    ASSERT_TRUE(runtime.RegisterRenderExtractionProcessor(
        "observe-particle-feature-value",
        0,
        [&observedPayloadRevision](const FrozenSceneSnapshot& snapshot)
        {
            ASSERT_EQ(snapshot.particles.size(), 1U);
            observedPayloadRevision = snapshot.particles.front().payloadRevision;
        }));

    const SceneEcsTickResult published = runtime.Tick();
    ASSERT_TRUE(published.succeeded);
    EXPECT_EQ(observedPayloadRevision, 7U);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    ASSERT_EQ(runtime.GetLatestFrozenSnapshot()->particles.size(), 1U);
    EXPECT_EQ(runtime.GetFeatureSnapshotCounts().particleCount, 1U);

    runtime.ClearProcessors();
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "remove-particle-feature-value",
        .phase = ECS::ProcessorPhase::EndFrameCleanup,
        .run = [&runtime, source](ECS::Registry&)
        {
            EXPECT_TRUE(runtime.RemoveFeatureSnapshots(source));
        },
    }));
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    ASSERT_EQ(runtime.GetLatestFrozenSnapshot()->particles.size(), 1U);
    EXPECT_EQ(runtime.GetFeatureSnapshotCounts().particleCount, 0U);

    // EndFrameCleanup intentionally follows the presentation freeze. The
    // idempotent removal is reflected by the next immutable snapshot.
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    EXPECT_TRUE(runtime.GetLatestFrozenSnapshot()->particles.empty());
    EXPECT_EQ(runtime.GetFeatureSnapshotCounts().particleCount, 0U);
}
