#include "RenderExtraction/ECS/EcsFrameExtractor.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "Scene/ECS/SceneFeatureSnapshotStore.h"

#include "ECS/Registry.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

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
    input.worldRevision = sequence;
    input.temporalEpoch = 1;
    input.viewFamilyId = 1;
    input.surfaceGeneration = 1;
    input.configurationRevision = 1;
    input.formatRevision = 1;
    input.simulationTick = sequence - 1;
    input.deterministicSeed =
        (0x94d049bbU ^ static_cast<uint32>(sequence)) | 1U;
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

[[nodiscard]] ParticleRenderSnapshotItem MakeParticle(float positionX = 1.0f)
{
    ParticleRenderSnapshotItem item;
    item.instanceId = 17;
    item.systemId = 23;
    item.systemAssetId = {.value = 101};
    item.systemName = "Transient legacy identity must not escape";
    item.simulationBackend = ParticleRenderSnapshotSimulationBackend::CPU;
    item.payloadStatus = ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
    item.aliveParticleCount = 1;
    item.maxParticleCount = 64;
    ParticleCpuPacket packet;
    packet.simulationTick = 7;
    packet.deterministicSeed = 11;
    packet.deterministicInstanceKey = 13;
    packet.effectiveSimulationSeed = 17;
    packet.particles.push_back({
        .stableParticleId = 3,
        .position = {positionX, 2.0f, 3.0f},
        .lifetime = 5.0f,
        .velocity = {4.0f, 5.0f, 6.0f},
        .age = 0.5f,
    });
    packet.contentHash = packet.ComputeContentHash();
    item.cpuPacket = std::move(packet);
    return item;
}

[[nodiscard]] WaterRenderSnapshotItem MakeWater()
{
    WaterRenderSnapshotItem item;
    item.payloadStatus = WaterRenderSnapshotPayloadStatus::Ready;
    item.componentId = 31;
    item.surfaceAssetId = {.value = 201};
    item.materialAssetId = {.value = 202};
    item.worldBounds = AABB({-5.8f, -4.5f, -10.8f}, {5.8f, 0.5f, 10.8f});
    item.size = {10.0f, 20.0f};
    item.depth = 4.0f;
    item.gerstnerWaves.waveCount = 1;
    item.gerstnerWaves.waves[0] = {
        .direction = {1.0f, 0.0f}, .amplitude = 0.5f,
        .wavelength = 8.0f, .steepness = 0.5f, .speed = 1.0f,
        .phaseOffsetRadians = 0.0f,
    };
    static_cast<void>(CanonicalizeWaterGerstnerWaveSet(item.gerstnerWaves));
    item.shallowColor = {0.02f, 0.24f, 0.30f};
    item.deepColor = {0.005f, 0.04f, 0.10f};
    item.foamColor = {0.92f, 0.96f, 1.0f};
    item.absorptionCoefficient = {0.15f, 0.06f, 0.02f};
    item.transparency = 0.72f;
    item.reflectionStrength = 0.65f;
    item.refractionStrength = 0.08f;
    item.roughness = 0.08f;
    item.normalScale = 1.0f;
    item.foamIntensity = 1.0f;
    item.foamCrestThreshold = 0.62f;
    item.reflectionEnabled = true;
    item.refractionEnabled = true;
    item.foamEnabled = true;
    item.contentHash = item.ComputeContentHash();
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
     TypedParticleWaterAndTerrainChannelsFailClosedSymmetrically)
{
    ECS::Registry registry;
    SceneFeatureSnapshotStore store(registry.GetSceneRuntimeId());

    const auto exercise = [&]<typename Item>(
                              const ECS::EntityRef& source,
                              auto&& publish)
    {
        const std::optional<SceneFeatureSnapshotSource> recorded =
            store.MakeSourceIdentity(source);
        ASSERT_TRUE(recorded.has_value());
        const RenderFeatureSourceIdentity expected = recorded->ToRenderIdentity();

        Item item;
        item.header.captureSequence = 4;
        item.header.configurationRevision = 9;
        item.header.source = expected;
        EXPECT_TRUE(publish(item));

        EXPECT_FALSE(publish(item));

        Item invalid = item;
        invalid.header.captureSequence = 5;
        invalid.header.channelId = static_cast<RenderFeatureChannelId>(999U);
        EXPECT_FALSE(publish(invalid));

        invalid = item;
        invalid.header.captureSequence = 5;
        invalid.header.schemaVersion = Item::SchemaVersion + 1U;
        EXPECT_FALSE(publish(invalid));

        invalid = item;
        invalid.header.captureSequence = 0;
        EXPECT_FALSE(publish(invalid));

        invalid = item;
        invalid.header.captureSequence = 5;
        invalid.header.source.sceneRuntimeId += 1;
        EXPECT_FALSE(publish(invalid));

        item.header.captureSequence = 5;
        item.header.configurationRevision = 10;
        EXPECT_TRUE(publish(item));
    };

    const ECS::EntityRef particleSource = registry.GetRef(registry.CreateEntity());
    const ECS::EntityRef waterSource = registry.GetRef(registry.CreateEntity());
    const ECS::EntityRef terrainSource = registry.GetRef(registry.CreateEntity());

    exercise.template operator()<ParticleRenderSnapshotItem>(
        particleSource,
        [&](ParticleRenderSnapshotItem item)
        {
            return store.PublishParticleChannel(particleSource, std::move(item));
        });
    exercise.template operator()<WaterRenderSnapshotItem>(
        waterSource,
        [&](WaterRenderSnapshotItem item)
        {
            return store.PublishWaterChannel(waterSource, std::move(item));
        });
    exercise.template operator()<TerrainRenderSnapshotItem>(
        terrainSource,
        [&](TerrainRenderSnapshotItem item)
        {
            return store.PublishTerrainChannel(terrainSource, std::move(item));
        });

    FrozenSceneSnapshot snapshot = MakeSnapshot(registry.GetSceneRuntimeId(), 1);
    ASSERT_TRUE(store.FreezeInto(snapshot));
    ASSERT_EQ(snapshot.particles.size(), 1U);
    ASSERT_EQ(snapshot.water.size(), 1U);
    ASSERT_EQ(snapshot.terrain.size(), 1U);
    EXPECT_EQ(snapshot.particles.front().state.header.captureSequence, 5U);
    EXPECT_EQ(snapshot.water.front().state.header.captureSequence, 5U);
    EXPECT_EQ(snapshot.terrain.front().state.header.captureSequence, 5U);
    EXPECT_EQ(snapshot.particles.front().payloadRevision, 10U);
    EXPECT_EQ(snapshot.water.front().payloadRevision, 10U);
    EXPECT_EQ(snapshot.terrain.front().payloadRevision, 10U);
}

TEST(EcsFeatureSnapshotValidation,
     StoreRequiresASealedFeatureCompositionForPublishAndFreeze)
{
    ECS::Registry registry;
    RenderFeatureChannelRegistry unsealedRegistry;
    SceneFeatureSnapshotStore store(
        registry.GetSceneRuntimeId(), std::move(unsealedRegistry));
    EXPECT_FALSE(store.GetChannelRegistry().IsSealed());

    const ECS::EntityRef source = registry.GetRef(registry.CreateEntity());
    EXPECT_FALSE(store.PublishParticle(source, MakeParticle(), 1));

    ParticleRenderSnapshotItem typed = MakeParticle();
    const std::optional<SceneFeatureSnapshotSource> identity =
        store.MakeSourceIdentity(source);
    ASSERT_TRUE(identity.has_value());
    typed.header.captureSequence = 1;
    typed.header.source = identity->ToRenderIdentity();
    EXPECT_FALSE(store.PublishParticleChannel(source, std::move(typed)));

    FrozenSceneSnapshot snapshot = MakeSnapshot(registry.GetSceneRuntimeId(), 1);
    snapshot.particles.push_back({});
    EXPECT_FALSE(store.FreezeInto(snapshot));
    EXPECT_EQ(snapshot.particles.size(), 1U);

    SceneFeatureSnapshotStore validStore(registry.GetSceneRuntimeId());
    EXPECT_TRUE(validStore.GetChannelRegistry().IsSealed());
    EXPECT_EQ(validStore.GetChannelRegistry().GetRegistrationCount(),
              RVX_RENDER_FEATURE_CHANNEL_COUNT);
    EXPECT_TRUE(validStore.PublishParticle(source, MakeParticle(), 1));
    EXPECT_TRUE(validStore.FreezeInto(snapshot));
    EXPECT_EQ(snapshot.particles.size(), 1U);
}

TEST(EcsFeatureSnapshotValidation,
     RejectsMalformedWaterPacketBeforeItCanBecomeFrozenSceneState)
{
    ECS::Registry registry;
    SceneFeatureSnapshotStore store(registry.GetSceneRuntimeId());
    const ECS::EntityRef source = registry.GetRef(registry.CreateEntity());

    WaterRenderSnapshotItem invalidHash = MakeWater();
    ++invalidHash.contentHash;
    EXPECT_FALSE(store.PublishWater(source, invalidHash, 1));

    WaterRenderSnapshotItem clippedBounds = MakeWater();
    clippedBounds.worldBounds = AABB({-5.0f, -4.0f, -10.0f}, {5.0f, 0.4f, 10.0f});
    clippedBounds.contentHash = clippedBounds.ComputeContentHash();
    EXPECT_FALSE(store.PublishWater(source, clippedBounds, 2));
    EXPECT_EQ(store.GetCounts().waterCount, 0U);
}

TEST(EcsFeatureSnapshotValidation,
     SharesImmutableParticlePacketAndExtractsFeatureFullIncrementalAndResetDeltas)
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

    FrozenSceneSnapshot firstSnapshot = MakeSnapshot(registry.GetSceneRuntimeId(), 1);
    ASSERT_TRUE(store.FreezeInto(firstSnapshot));
    ASSERT_EQ(firstSnapshot.particles.size(), 1U);
    ASSERT_EQ(firstSnapshot.water.size(), 1U);
    ASSERT_EQ(firstSnapshot.terrain.size(), 1U);
    EXPECT_TRUE(firstSnapshot.particles[0].state.HasValidCpuPacket());
    EXPECT_NE(&firstSnapshot.particles[0].state.cpuPacket, &particle.cpuPacket);
    EXPECT_EQ(firstSnapshot.particles[0].state.cpuPacket.contentHash,
              particle.cpuPacket.contentHash);
    EXPECT_FLOAT_EQ(firstSnapshot.particles[0].state.GetParticles()[0].position.x, 1.0f);
    EXPECT_EQ(firstSnapshot.particles[0].payloadRevision, 11U);
    EXPECT_EQ(firstSnapshot.particles[0].state.header.channelId,
              RenderFeatureChannelId::Particle);
    EXPECT_EQ(firstSnapshot.particles[0].state.header.captureSequence, 1U);
    EXPECT_EQ(firstSnapshot.particles[0].state.header.configurationRevision, 11U);
    EXPECT_EQ(firstSnapshot.water[0].state.header.channelId,
              RenderFeatureChannelId::Water);
    EXPECT_EQ(firstSnapshot.water[0].state.header.captureSequence, 1U);
    EXPECT_EQ(firstSnapshot.terrain[0].state.header.channelId,
              RenderFeatureChannelId::Terrain);
    EXPECT_EQ(firstSnapshot.terrain[0].state.header.captureSequence, 1U);
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
    ParticleRenderSnapshotItem replacementParticle = MakeParticle(42.0f);
    ASSERT_TRUE(store.PublishParticle(particleSource, replacementParticle, 14));
    EXPECT_NE(firstSnapshot.particles[0].state.cpuPacket.contentHash,
              replacementParticle.cpuPacket.contentHash);
    EXPECT_FLOAT_EQ(firstSnapshot.particles[0].state.GetParticles()[0].position.x, 1.0f);

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
    updatedWater.worldBounds = AABB({-5.8f, -9.5f, -10.8f}, {5.8f, 0.5f, 10.8f});
    updatedWater.contentHash = updatedWater.ComputeContentHash();
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
