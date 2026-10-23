#include "Render/Renderer/RenderSceneDatabase.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"

#include <gtest/gtest.h>

namespace
{
    RVX::RenderSceneObjectSourceIdentity MakeSource(
        RVX::uint64 sceneRuntimeId,
        RVX::uint32 entitySlot,
        RVX::uint32 entityGeneration = 0)
    {
        return {
            .sceneRuntimeId = sceneRuntimeId,
            .entitySlot = entitySlot,
            .entityGeneration = entityGeneration,
        };
    }

    RVX::uint64 DeriveRenderId(RVX::uint64 sceneRuntimeId,
                               RVX::uint32 entitySlot,
                               RVX::uint32 entityGeneration,
                               RVX::SceneECS::FrozenSceneObjectType type)
    {
        return RVX::EcsFrozenSceneBridge::DeriveRenderId({
            .sceneRuntimeId = RVX::ECS::SceneRuntimeId{sceneRuntimeId},
            .entity = RVX::ECS::EntityHandle::Create(entitySlot,
                                                       entityGeneration),
            .type = type,
        });
    }

    RVX::RenderPrimitiveSnapshot MakePrimitive(RVX::uint64 id,
                                                RVX::uint64 sortKey = 0)
    {
        RVX::RenderPrimitiveSnapshot primitive;
        primitive.objectId = id;
        primitive.source = MakeSource(1, static_cast<RVX::uint32>(id));
        primitive.sortKey = sortKey;
        return primitive;
    }

    RVX::RenderPrimitiveSnapshot MakePrimitive(
        RVX::uint64 id,
        const RVX::RenderSceneObjectSourceIdentity& source,
        RVX::uint64 sortKey = 0)
    {
        RVX::RenderPrimitiveSnapshot primitive = MakePrimitive(id, sortKey);
        primitive.source = source;
        return primitive;
    }

    RVX::RenderSceneUpdateBatch MakeFullReset(RVX::uint64 revision)
    {
        RVX::RenderSceneMutationAccumulator accumulator;
        accumulator.Begin(0, true);
        EXPECT_TRUE(accumulator.UpsertPrimitive(MakePrimitive(11), true));
        return accumulator.Build(revision);
    }

    RVX::ParticleRenderSnapshotItem MakeParticle(
        RVX::uint64 renderId,
        RVX::uint64 sceneRuntimeId,
        RVX::uint32 entitySlot,
        RVX::uint32 entityGeneration,
        RVX::uint64 captureSequence)
    {
        RVX::ParticleRenderSnapshotItem particle;
        particle.instanceId = renderId;
        particle.systemId = renderId;
        particle.header.captureSequence = captureSequence;
        particle.header.configurationRevision = 1;
        particle.header.source = {
            .sceneRuntimeId = sceneRuntimeId,
            .entitySlot = entitySlot,
            .entityGeneration = entityGeneration,
        };
        return particle;
    }

    void CompleteFrameIdentity(RVX::RenderFrameHeaderV5& header,
                               RVX::RenderViewSnapshot& view,
                               RVX::uint32 width,
                               RVX::uint32 height)
    {
        header.viewFamilyId = 1;
        header.surfaceGeneration = 1;
        header.configurationRevision = 1;
        header.formatRevision = 1;
        header.simulationTick = header.sequence;
        header.deterministicSeed = 1;
        header.outputWidth = width;
        header.outputHeight = height;
        view.identity = {
            .sceneRuntimeId = header.worldRevision,
            .cameraEntityPacked = 1,
        };
        view.cameraCutRevision = 1;
        view.viewportWidth = width;
        view.viewportHeight = height;
        view.deltaTime = 1.0F / 60.0F;
    }
}

TEST(RenderSceneRevisionValidation, FullResetCreatesPersistentDatabase)
{
    RVX::RenderSceneDatabase database;
    const auto result = database.Apply(MakeFullReset(1));

    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(database.GetRevision(), 1U);
    ASSERT_NE(database.FindPrimitive(11), nullptr);
}

TEST(RenderSceneRevisionValidation,
     LightStateHashIsOrderIndependentAndChangesWithRetainedValues)
{
    RVX::RenderLightSnapshot first;
    first.lightId = 10;
    first.source = MakeSource(2, 10);
    first.color = RVX::Vec3(1.0f, 0.5f, 0.25f);
    first.intensity = 2.0f;
    first.intensityUnit = RVX::RenderLightIntensityUnit::Lux;
    RVX::RenderLightSnapshot second;
    second.lightId = 20;
    second.source = MakeSource(2, 20);
    second.type = RVX::RenderLightType::Point;
    second.position = RVX::Vec3(1.0f, 2.0f, 3.0f);
    second.intensityUnit = RVX::RenderLightIntensityUnit::Candela;
    second.range = 10.0f;

    RVX::RenderSceneMutationAccumulator firstOrder;
    firstOrder.Begin(0, true);
    ASSERT_TRUE(firstOrder.UpsertLight(first, true));
    ASSERT_TRUE(firstOrder.UpsertLight(second, true));
    RVX::RenderSceneDatabase firstDatabase;
    ASSERT_TRUE(firstDatabase.Apply(firstOrder.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator reverseOrder;
    reverseOrder.Begin(0, true);
    ASSERT_TRUE(reverseOrder.UpsertLight(second, true));
    ASSERT_TRUE(reverseOrder.UpsertLight(first, true));
    RVX::RenderSceneDatabase secondDatabase;
    ASSERT_TRUE(secondDatabase.Apply(reverseOrder.Build(1)).IsApplied());
    EXPECT_EQ(firstDatabase.ComputeLightStateHash(),
              secondDatabase.ComputeLightStateHash());

    first.intensity = 3.0f;
    RVX::RenderSceneMutationAccumulator mutation;
    mutation.Begin(1);
    ASSERT_TRUE(mutation.UpsertLight(first));
    ASSERT_TRUE(firstDatabase.Apply(mutation.Build(2)).IsApplied());
    EXPECT_NE(firstDatabase.ComputeLightStateHash(),
              secondDatabase.ComputeLightStateHash());
}

TEST(RenderSceneRevisionValidation,
     LightLayerMaskChangesRetainedStateHash)
{
    RVX::RenderLightSnapshot light;
    light.lightId = 9;
    light.source = MakeSource(2, 9);
    light.intensityUnit = RVX::RenderLightIntensityUnit::Lux;
    light.layerMask = 0x00000001U;

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertLight(light, true));
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
    const RVX::uint64 before = database.ComputeLightStateHash();

    light.layerMask = 0x00000002U;
    RVX::RenderSceneMutationAccumulator update;
    update.Begin(1);
    ASSERT_TRUE(update.UpsertLight(light));
    ASSERT_TRUE(database.Apply(update.Build(2)).IsApplied());

    EXPECT_NE(before, database.ComputeLightStateHash());
    ASSERT_NE(database.FindLight(9), nullptr);
    EXPECT_EQ(0x00000002U, database.FindLight(9)->layerMask);
}

TEST(RenderSceneRevisionValidation,
     LightPhotometricMetadataChangesProduceDistinctIncrementalMutations)
{
    RVX::RenderLightSnapshot light;
    light.lightId = 42;
    light.source = MakeSource(2, 42);
    light.type = RVX::RenderLightType::Point;
    light.intensity = 20.0f;
    light.intensityUnit = RVX::RenderLightIntensityUnit::Candela;
    light.range = 15.0f;

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertLight(light, true));

    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
    const RVX::uint64 initialHash = database.ComputeLightStateHash();
    const RVX::RenderSceneDatabaseChanges initialChanges =
        database.CollectChangesSince(0);
    ASSERT_TRUE(initialChanges.available);

    light.intensityUnit = RVX::RenderLightIntensityUnit::Lumen;
    RVX::RenderSceneMutationAccumulator unitUpdate;
    unitUpdate.Begin(1);
    ASSERT_TRUE(unitUpdate.UpsertLight(light));
    ASSERT_TRUE(database.Apply(unitUpdate.Build(2)).IsApplied());
    EXPECT_NE(initialHash, database.ComputeLightStateHash());
    ASSERT_NE(database.FindLight(42), nullptr);
    EXPECT_EQ(database.FindLight(42)->intensityUnit,
              RVX::RenderLightIntensityUnit::Lumen);
    const RVX::RenderSceneDatabaseChanges unitChanges =
        database.CollectChangesSince(1);
    ASSERT_TRUE(unitChanges.available);
    ASSERT_EQ(unitChanges.lights.size(), 1U);
    EXPECT_EQ(unitChanges.lights.front(), 42U);
    EXPECT_NE(initialChanges.canonicalMutationDigest,
              unitChanges.canonicalMutationDigest);

    const RVX::uint64 unitHash = database.ComputeLightStateHash();
    light.colorTemperatureKelvin = 4500.0f;
    RVX::RenderSceneMutationAccumulator temperatureUpdate;
    temperatureUpdate.Begin(2);
    ASSERT_TRUE(temperatureUpdate.UpsertLight(light));
    ASSERT_TRUE(database.Apply(temperatureUpdate.Build(3)).IsApplied());
    EXPECT_NE(unitHash, database.ComputeLightStateHash());
    ASSERT_NE(database.FindLight(42), nullptr);
    ASSERT_TRUE(database.FindLight(42)->colorTemperatureKelvin.has_value());
    EXPECT_FLOAT_EQ(*database.FindLight(42)->colorTemperatureKelvin, 4500.0f);
    const RVX::RenderSceneDatabaseChanges temperatureChanges =
        database.CollectChangesSince(2);
    ASSERT_TRUE(temperatureChanges.available);
    ASSERT_EQ(temperatureChanges.lights.size(), 1U);
    EXPECT_EQ(temperatureChanges.lights.front(), 42U);
    EXPECT_NE(unitChanges.canonicalMutationDigest,
              temperatureChanges.canonicalMutationDigest);
}

TEST(RenderSceneRevisionValidation, AppliesIncrementalUpsertAndRemoveAtomically)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator accumulator;
    accumulator.Begin(1);
    ASSERT_TRUE(accumulator.UpsertPrimitive(MakePrimitive(11, 9)));
    ASSERT_TRUE(accumulator.UpsertPrimitive(MakePrimitive(12, 3), true));
    ASSERT_TRUE(accumulator.RemovePrimitive(11));

    const auto result = database.Apply(accumulator.Build(2));
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(database.GetRevision(), 2U);
    EXPECT_EQ(database.FindPrimitive(11), nullptr);
    ASSERT_NE(database.FindPrimitive(12), nullptr);
    EXPECT_EQ(database.FindPrimitive(12)->sortKey, 3U);
}

TEST(RenderSceneRevisionValidation, RevisionGapAndDuplicateFailClosed)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(5)).IsApplied());

    RVX::RenderSceneMutationAccumulator gapAccumulator;
    gapAccumulator.Begin(4);
    ASSERT_TRUE(gapAccumulator.UpsertPrimitive(MakePrimitive(22), true));
    const auto gap = database.Apply(gapAccumulator.Build(6));
    EXPECT_EQ(gap.code, RVX::RenderSceneUpdateApplyCode::RevisionGap);
    EXPECT_EQ(database.GetRevision(), 5U);
    EXPECT_EQ(database.FindPrimitive(22), nullptr);

    const auto duplicate = database.Apply(MakeFullReset(5));
    EXPECT_EQ(duplicate.code, RVX::RenderSceneUpdateApplyCode::OutOfOrder);
    EXPECT_EQ(database.GetRevision(), 5U);
}

TEST(RenderSceneRevisionValidation, InvalidBatchDoesNotPartiallyMutate)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(1)).IsApplied());

    RVX::RenderSceneUpdateBatch invalid;
    invalid.baseSceneRevision = 1;
    invalid.targetSceneRevision = 2;
    RVX::RenderPrimitiveMutation first;
    first.objectId = 11;
    first.state = MakePrimitive(11, 100);
    RVX::RenderPrimitiveMutation duplicate = first;
    duplicate.state.sortKey = 200;
    invalid.primitives = {first, duplicate};

    const auto result = database.Apply(invalid);
    EXPECT_EQ(result.code, RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 1U);
    ASSERT_NE(database.FindPrimitive(11), nullptr);
    EXPECT_EQ(database.FindPrimitive(11)->sortKey, 0U);
}

TEST(RenderSceneRevisionValidation, NewerFullResetRecoversFromDesynchronization)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(2)).IsApplied());

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(99), true));
    const auto result = database.Apply(reset.Build(10));

    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(database.GetRevision(), 10U);
    EXPECT_EQ(database.FindPrimitive(11), nullptr);
    EXPECT_NE(database.FindPrimitive(99), nullptr);
}

TEST(RenderSceneRevisionValidation,
     ChangeJournalMergesExactIdsAndExpiresOnlyAcknowledgedRanges)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(1)).IsApplied());
    EXPECT_EQ(database.GetPrimitiveRevision(11), 1U);

    RVX::RenderSceneMutationAccumulator second;
    second.Begin(1);
    ASSERT_TRUE(second.UpsertPrimitive(MakePrimitive(11, 9)));
    RVX::RenderLightSnapshot light;
    light.lightId = 7;
    light.source = MakeSource(2, 7);
    light.intensityUnit = RVX::RenderLightIntensityUnit::Lux;
    ASSERT_TRUE(second.UpsertLight(light, true));
    ASSERT_TRUE(database.Apply(second.Build(2)).IsApplied());
    EXPECT_EQ(database.GetPrimitiveRevision(11), 2U);

    RVX::RenderSceneMutationAccumulator third;
    third.Begin(2);
    ASSERT_TRUE(third.UpsertPrimitive(MakePrimitive(11, 12)));
    RVX::RenderSkySnapshot sky;
    sky.intensity = 2.0F;
    third.UpsertSky(sky);
    ASSERT_TRUE(database.Apply(third.Build(3)).IsApplied());

    const RVX::RenderSceneDatabaseChanges merged =
        database.CollectChangesSince(1);
    ASSERT_TRUE(merged.available);
    EXPECT_FALSE(merged.fullReset);
    EXPECT_EQ(merged.baseRevision, 1U);
    EXPECT_EQ(merged.targetRevision, 3U);
    EXPECT_EQ(merged.primitives, std::vector<RVX::uint64>({11U}));
    EXPECT_EQ(merged.lights, std::vector<RVX::uint64>({7U}));
    EXPECT_TRUE(merged.skyChanged);
    EXPECT_FALSE(merged.environmentChanged);

    database.AcknowledgeChangesThrough(2);
    const RVX::RenderSceneDatabaseChanges current =
        database.CollectChangesSince(2);
    ASSERT_TRUE(current.available);
    EXPECT_EQ(current.primitives, std::vector<RVX::uint64>({11U}));
    EXPECT_TRUE(current.skyChanged);

    const RVX::RenderSceneDatabaseChanges expired =
        database.CollectChangesSince(1);
    EXPECT_FALSE(expired.available);
    const RVX::RenderSceneDatabaseChanges noChanges =
        database.CollectChangesSince(3);
    EXPECT_TRUE(noChanges.available);
    EXPECT_TRUE(noChanges.Empty());
}

TEST(RenderSceneRevisionValidation,
     FullResetJournalForcesCheckpointAndDatabaseLineageIsUnique)
{
    RVX::RenderSceneDatabase first;
    RVX::RenderSceneDatabase second;
    ASSERT_NE(first.GetInstanceId(), 0U);
    ASSERT_NE(second.GetInstanceId(), 0U);
    EXPECT_NE(first.GetInstanceId(), second.GetInstanceId());

    ASSERT_TRUE(first.Apply(MakeFullReset(4)).IsApplied());
    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(99), true));
    reset.UpsertEnvironment(RVX::RenderEnvironmentSnapshot{});
    ASSERT_TRUE(first.Apply(reset.Build(9)).IsApplied());

    const RVX::RenderSceneDatabaseChanges changes =
        first.CollectChangesSince(4);
    ASSERT_TRUE(changes.available);
    EXPECT_TRUE(changes.fullReset);
    EXPECT_TRUE(changes.skyChanged);
    EXPECT_TRUE(changes.environmentChanged);
    EXPECT_EQ(changes.targetRevision, 9U);
}

TEST(RenderSceneRevisionValidation,
     MutationSummaryAndDigestAreCanonicalAcrossInputOrder)
{
    RVX::RenderLightSnapshot light;
    light.lightId = 17;
    light.source = MakeSource(2, 17);
    light.intensityUnit = RVX::RenderLightIntensityUnit::Lux;

    RVX::RenderSceneMutationAccumulator firstOrder;
    firstOrder.Begin(0, true);
    ASSERT_TRUE(firstOrder.UpsertPrimitive(MakePrimitive(9), true));
    ASSERT_TRUE(firstOrder.UpsertPrimitive(MakePrimitive(3), true));
    ASSERT_TRUE(firstOrder.UpsertLight(light, true));
    ASSERT_TRUE(firstOrder.UpsertParticle(
        MakeParticle(41, 7, 3, 0, 1), true));
    firstOrder.UpsertSky({});

    RVX::RenderSceneMutationAccumulator reverseOrder;
    reverseOrder.Begin(0, true);
    reverseOrder.UpsertSky({});
    ASSERT_TRUE(reverseOrder.UpsertParticle(
        MakeParticle(41, 7, 3, 0, 1), true));
    ASSERT_TRUE(reverseOrder.UpsertLight(light, true));
    ASSERT_TRUE(reverseOrder.UpsertPrimitive(MakePrimitive(3), true));
    ASSERT_TRUE(reverseOrder.UpsertPrimitive(MakePrimitive(9), true));

    RVX::RenderSceneDatabase first;
    RVX::RenderSceneDatabase second;
    ASSERT_TRUE(first.Apply(firstOrder.Build(1)).IsApplied());
    ASSERT_TRUE(second.Apply(reverseOrder.Build(1)).IsApplied());

    const RVX::RenderSceneDatabaseChanges firstChanges =
        first.CollectChangesSince(0);
    const RVX::RenderSceneDatabaseChanges secondChanges =
        second.CollectChangesSince(0);
    ASSERT_TRUE(firstChanges.available);
    ASSERT_TRUE(secondChanges.available);
    EXPECT_TRUE(firstChanges.fullReset);
    EXPECT_EQ(2U, firstChanges.mutationSummary.primitives.upsertCount);
    EXPECT_EQ(0U, firstChanges.mutationSummary.primitives.removeCount);
    EXPECT_EQ(1U, firstChanges.mutationSummary.lights.upsertCount);
    EXPECT_EQ(1U, firstChanges.mutationSummary.particles.upsertCount);
    EXPECT_EQ(1U, firstChanges.mutationSummary.sky.upsertCount);
    EXPECT_NE(0U, firstChanges.canonicalMutationDigest);
    EXPECT_EQ(firstChanges.canonicalMutationDigest,
              secondChanges.canonicalMutationDigest);

    RVX::RenderSceneMutationAccumulator update;
    update.Begin(1);
    ASSERT_TRUE(update.RemovePrimitive(3));
    ASSERT_TRUE(update.UpsertPrimitive(MakePrimitive(9, 4)));
    ASSERT_TRUE(first.Apply(update.Build(2)).IsApplied());
    const RVX::RenderSceneDatabaseChanges incremental =
        first.CollectChangesSince(1);
    ASSERT_TRUE(incremental.available);
    EXPECT_FALSE(incremental.fullReset);
    EXPECT_EQ(1U, incremental.mutationSummary.primitives.upsertCount);
    EXPECT_EQ(1U, incremental.mutationSummary.primitives.removeCount);
    EXPECT_NE(firstChanges.canonicalMutationDigest,
              incremental.canonicalMutationDigest);

    RVX::RenderSceneMutationAccumulator changedPayload;
    changedPayload.Begin(0, true);
    ASSERT_TRUE(changedPayload.UpsertPrimitive(MakePrimitive(9, 99), true));
    ASSERT_TRUE(changedPayload.UpsertPrimitive(MakePrimitive(3), true));
    ASSERT_TRUE(changedPayload.UpsertLight(light, true));
    ASSERT_TRUE(changedPayload.UpsertParticle(
        MakeParticle(41, 7, 3, 0, 1), true));
    changedPayload.UpsertSky({});
    RVX::RenderSceneDatabase payloadDatabase;
    ASSERT_TRUE(payloadDatabase.Apply(changedPayload.Build(1)).IsApplied());
    const RVX::RenderSceneDatabaseChanges payloadChanges =
        payloadDatabase.CollectChangesSince(0);
    ASSERT_TRUE(payloadChanges.available);
    EXPECT_NE(firstChanges.canonicalMutationDigest,
              payloadChanges.canonicalMutationDigest);

    RVX::ParticleRenderSnapshotItem provenance =
        MakeParticle(41, 7, 3, 0, 1);
    provenance.header.configurationRevision = 2;
    RVX::RenderSceneMutationAccumulator changedProvenance;
    changedProvenance.Begin(0, true);
    ASSERT_TRUE(changedProvenance.UpsertPrimitive(MakePrimitive(9), true));
    ASSERT_TRUE(changedProvenance.UpsertPrimitive(MakePrimitive(3), true));
    ASSERT_TRUE(changedProvenance.UpsertLight(light, true));
    ASSERT_TRUE(changedProvenance.UpsertParticle(
        std::move(provenance), true));
    changedProvenance.UpsertSky({});
    RVX::RenderSceneDatabase provenanceDatabase;
    ASSERT_TRUE(
        provenanceDatabase.Apply(changedProvenance.Build(1)).IsApplied());
    const RVX::RenderSceneDatabaseChanges provenanceChanges =
        provenanceDatabase.CollectChangesSince(0);
    ASSERT_TRUE(provenanceChanges.available);
    EXPECT_NE(firstChanges.canonicalMutationDigest,
              provenanceChanges.canonicalMutationDigest);
}

TEST(RenderSceneRevisionValidation,
     IncrementalSingletonRemovalCommitsAsAnEngagedEmptyValue)
{
    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    reset.UpsertSky(RVX::RenderSkySnapshot{});
    reset.UpsertEnvironment(RVX::RenderEnvironmentSnapshot{});
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
    ASSERT_TRUE(database.GetSky().has_value());
    ASSERT_TRUE(database.GetEnvironment().has_value());

    RVX::RenderSceneMutationAccumulator remove;
    remove.Begin(1);
    remove.RemoveSky();
    remove.RemoveEnvironment();
    ASSERT_TRUE(database.Apply(remove.Build(2)).IsApplied());
    EXPECT_FALSE(database.GetSky().has_value());
    EXPECT_FALSE(database.GetEnvironment().has_value());
    const RVX::RenderSceneDatabaseChanges changes =
        database.CollectChangesSince(1);
    ASSERT_TRUE(changes.available);
    EXPECT_TRUE(changes.skyChanged);
    EXPECT_TRUE(changes.environmentChanged);
}

TEST(RenderSceneRevisionValidation, ExposesAuthoritativeSceneAndV5FrameState)
{
    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(22, 2), true));
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(11, 1), true));

    RVX::RenderLightSnapshot light;
    light.lightId = 7;
    light.source = MakeSource(2, 7);
    light.intensity = 4.0f;
    light.intensityUnit = RVX::RenderLightIntensityUnit::Lux;
    ASSERT_TRUE(reset.UpsertLight(light, true));

    RVX::ParticleRenderSnapshotItem particle;
    particle = MakeParticle(31, 5, 9, 0, 1);
    particle.systemAssetId = {.value = 8};
    particle.aliveParticleCount = 8;
    ASSERT_TRUE(reset.UpsertParticle(particle, true));

    RVX::RenderSkySnapshot sky;
    sky.mode = RVX::RenderSkyMode::Procedural;
    sky.intensity = 1.5f;
    reset.UpsertSky(sky);

    RVX::RenderEnvironmentSnapshot environment;
    environment.intensity = 0.75f;
    reset.UpsertEnvironment(environment);

    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(7)).IsApplied());

    RVX::RenderFrameHeaderV5 header;
    header.sequence = 41;
    header.requiredSceneRevision = 7;
    header.worldRevision = 6;
    header.temporalEpoch = 3;

    RVX::RenderViewSnapshot view;
    CompleteFrameIdentity(header, view, 1280, 720);

    RVX::RenderExtractionDiagnostics diagnostics;
    diagnostics.complete = true;
    const auto frameV5 = RVX::RenderFramePacketV5::Create(
        header,
        view,
        RVX::RenderFrameSettings{},
        RVX::RenderFrameCaptureRequest{},
        diagnostics);
    ASSERT_NE(frameV5, nullptr);

    EXPECT_EQ(frameV5->GetHeader().sequence, 41U);
    EXPECT_EQ(frameV5->GetHeader().worldRevision, 6U);
    EXPECT_EQ(frameV5->GetHeader().temporalEpoch, 3U);
    EXPECT_EQ(frameV5->GetHeader().requiredSceneRevision,
              database.GetRevision());
    ASSERT_EQ(database.GetPrimitives().size(), 2U);
    EXPECT_NE(database.FindPrimitive(11), nullptr);
    EXPECT_NE(database.FindPrimitive(22), nullptr);
    ASSERT_EQ(database.GetLights().size(), 1U);
    EXPECT_NE(database.FindLight(7), nullptr);
    ASSERT_EQ(database.GetParticles().size(), 1U);
    EXPECT_TRUE(database.GetParticles().contains(31));
    ASSERT_TRUE(database.GetSky().has_value());
    EXPECT_EQ(database.GetSky()->mode, RVX::RenderSkyMode::Procedural);
    ASSERT_TRUE(database.GetEnvironment().has_value());
    EXPECT_FLOAT_EQ(database.GetEnvironment()->intensity, 0.75f);
}

TEST(RenderSceneRevisionValidation, PersistentRevisionDoesNotSatisfyAheadV5Frame)
{
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(MakeFullReset(2)).IsApplied());

    RVX::RenderFrameHeaderV5 header;
    header.sequence = 9;
    header.requiredSceneRevision = 3;
    header.worldRevision = 1;
    header.temporalEpoch = 1;
    RVX::RenderViewSnapshot view;
    CompleteFrameIdentity(header, view, 64, 64);
    RVX::RenderExtractionDiagnostics diagnostics;
    diagnostics.complete = true;
    const auto frameV5 = RVX::RenderFramePacketV5::Create(
        header,
        view,
        RVX::RenderFrameSettings{},
        RVX::RenderFrameCaptureRequest{},
        diagnostics);
    ASSERT_NE(frameV5, nullptr);

    EXPECT_LT(database.GetRevision(),
              frameV5->GetHeader().requiredSceneRevision);
}

TEST(RenderSceneRevisionValidation, RecycledObjectSlotRequiresOrderedGenerationReplacement)
{
    const RVX::RenderSceneObjectSourceIdentity generationZero =
        MakeSource(33, 7, 0);
    const RVX::RenderSceneObjectSourceIdentity generationOne =
        MakeSource(33, 7, 1);
    const RVX::RenderSceneObjectSourceIdentity generationTwo =
        MakeSource(33, 7, 2);
    const RVX::uint64 oldId = DeriveRenderId(
        33, 7, 0, RVX::SceneECS::FrozenSceneObjectType::Mesh);
    const RVX::uint64 replacementId = DeriveRenderId(
        33, 7, 1, RVX::SceneECS::FrozenSceneObjectType::Mesh);
    const RVX::uint64 nextGenerationId = DeriveRenderId(
        33, 7, 2, RVX::SceneECS::FrozenSceneObjectType::Mesh);

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(oldId, generationZero), true));
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator missingRemove;
    missingRemove.Begin(1);
    ASSERT_TRUE(missingRemove.UpsertPrimitive(
        MakePrimitive(replacementId, generationOne), true));
    EXPECT_EQ(database.Apply(missingRemove.Build(2)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 1u);

    RVX::RenderSceneMutationAccumulator replace;
    replace.Begin(1);
    ASSERT_TRUE(replace.RemovePrimitive(oldId));
    ASSERT_TRUE(replace.UpsertPrimitive(
        MakePrimitive(replacementId, generationOne), true));
    ASSERT_TRUE(database.Apply(replace.Build(2)).IsApplied());
    EXPECT_EQ(database.FindPrimitive(oldId), nullptr);
    EXPECT_NE(database.FindPrimitive(replacementId), nullptr);

    RVX::RenderSceneMutationAccumulator expiredRemove;
    expiredRemove.Begin(2);
    ASSERT_TRUE(expiredRemove.RemovePrimitive(oldId));
    EXPECT_EQ(database.Apply(expiredRemove.Build(3)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 2u);
    EXPECT_NE(database.FindPrimitive(replacementId), nullptr);

    RVX::RenderSceneMutationAccumulator removeReplacement;
    removeReplacement.Begin(2);
    ASSERT_TRUE(removeReplacement.RemovePrimitive(replacementId));
    ASSERT_TRUE(database.Apply(removeReplacement.Build(3)).IsApplied());
    EXPECT_EQ(database.FindPrimitive(replacementId), nullptr);

    RVX::RenderSceneMutationAccumulator resurrect;
    resurrect.Begin(3);
    ASSERT_TRUE(resurrect.UpsertPrimitive(MakePrimitive(oldId, generationZero), true));
    EXPECT_EQ(database.Apply(resurrect.Build(4)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 3u);

    RVX::RenderSceneMutationAccumulator nextGeneration;
    nextGeneration.Begin(3);
    ASSERT_TRUE(nextGeneration.UpsertPrimitive(
        MakePrimitive(nextGenerationId, generationTwo), true));
    ASSERT_TRUE(database.Apply(nextGeneration.Build(4)).IsApplied());
    EXPECT_NE(database.FindPrimitive(nextGenerationId), nullptr);
}

TEST(RenderSceneRevisionValidation,
     SceneObjectGenerationReplacementCannotReuseOpaqueRenderId)
{
    constexpr RVX::uint64 opaqueRenderId = 0xA11CE00010000001ull;
    const RVX::RenderSceneObjectSourceIdentity generationZero =
        MakeSource(84, 6, 0);
    const RVX::RenderSceneObjectSourceIdentity generationOne =
        MakeSource(84, 6, 1);

    RVX::RenderSceneDatabase database;
    RVX::RenderSceneMutationAccumulator first;
    first.Begin(0, true);
    ASSERT_TRUE(first.UpsertPrimitive(
        MakePrimitive(opaqueRenderId, generationZero), true));
    ASSERT_TRUE(database.Apply(first.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator remove;
    remove.Begin(1);
    ASSERT_TRUE(remove.RemovePrimitive(opaqueRenderId));
    ASSERT_TRUE(database.Apply(remove.Build(2)).IsApplied());

    RVX::RenderSceneMutationAccumulator reusedId;
    reusedId.Begin(2);
    ASSERT_TRUE(reusedId.UpsertPrimitive(
        MakePrimitive(opaqueRenderId, generationOne), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(reusedId.Build(3)).code);
    EXPECT_EQ(database.GetRevision(), 2U);
    EXPECT_EQ(database.FindPrimitive(opaqueRenderId), nullptr);
}

TEST(RenderSceneRevisionValidation,
     ParticleGenerationRestartsCaptureSequenceButRejectsStaleSources)
{
    constexpr RVX::uint64 oldRenderId = 0xF123456789ABCDEFull;
    constexpr RVX::uint64 replacementRenderId = 0x0000000100000001ull;
    constexpr RVX::uint64 nextRenderId = 0xABCDEF0102030405ull;

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertParticle(
        MakeParticle(oldRenderId, 77, 4, 0, 1), true));
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator staleCapture;
    staleCapture.Begin(1);
    ASSERT_TRUE(staleCapture.UpsertParticle(
        MakeParticle(oldRenderId, 77, 4, 0, 1)));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(staleCapture.Build(2)).code);

    RVX::RenderSceneMutationAccumulator sameGeneration;
    sameGeneration.Begin(1);
    ASSERT_TRUE(sameGeneration.RemoveParticle(oldRenderId));
    ASSERT_TRUE(sameGeneration.UpsertParticle(
        MakeParticle(replacementRenderId, 77, 4, 0, 2), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(sameGeneration.Build(2)).code);

    RVX::RenderSceneMutationAccumulator missingRemoval;
    missingRemoval.Begin(1);
    ASSERT_TRUE(missingRemoval.UpsertParticle(
        MakeParticle(replacementRenderId, 77, 4, 1, 2), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(missingRemoval.Build(2)).code);

    RVX::RenderSceneMutationAccumulator replace;
    replace.Begin(1);
    ASSERT_TRUE(replace.RemoveParticle(oldRenderId));
    ASSERT_TRUE(replace.UpsertParticle(
        MakeParticle(replacementRenderId, 77, 4, 1, 1), true));
    ASSERT_TRUE(database.Apply(replace.Build(2)).IsApplied());
    EXPECT_FALSE(database.GetParticles().contains(oldRenderId));
    EXPECT_TRUE(database.GetParticles().contains(replacementRenderId));

    RVX::RenderSceneMutationAccumulator staleGeneration;
    staleGeneration.Begin(2);
    ASSERT_TRUE(staleGeneration.UpsertParticle(
        MakeParticle(oldRenderId, 77, 4, 0, 2), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(staleGeneration.Build(3)).code);

    RVX::RenderSceneMutationAccumulator staleReplacementCapture;
    staleReplacementCapture.Begin(2);
    ASSERT_TRUE(staleReplacementCapture.UpsertParticle(
        MakeParticle(replacementRenderId, 77, 4, 1, 1), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(staleReplacementCapture.Build(3)).code);

    RVX::RenderSceneMutationAccumulator remove;
    remove.Begin(2);
    ASSERT_TRUE(remove.RemoveParticle(replacementRenderId));
    ASSERT_TRUE(database.Apply(remove.Build(3)).IsApplied());

    RVX::RenderSceneMutationAccumulator resurrect;
    resurrect.Begin(3);
    ASSERT_TRUE(resurrect.UpsertParticle(
        MakeParticle(replacementRenderId, 77, 4, 1, 3), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(resurrect.Build(4)).code);

    RVX::RenderSceneMutationAccumulator nextGeneration;
    nextGeneration.Begin(3);
    ASSERT_TRUE(nextGeneration.UpsertParticle(
        MakeParticle(nextRenderId, 77, 4, 2, 4), true));
    ASSERT_TRUE(database.Apply(nextGeneration.Build(4)).IsApplied());
    EXPECT_TRUE(database.GetParticles().contains(nextRenderId));
}

TEST(RenderSceneRevisionValidation,
     ParticleGenerationReplacementCannotReuseOpaqueRenderId)
{
    constexpr RVX::uint64 opaqueRenderId = 0xBEE7000100000001ull;

    RVX::RenderSceneDatabase database;
    RVX::RenderSceneMutationAccumulator first;
    first.Begin(0, true);
    ASSERT_TRUE(first.UpsertParticle(
        MakeParticle(opaqueRenderId, 85, 7, 0, 1), true));
    ASSERT_TRUE(database.Apply(first.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator remove;
    remove.Begin(1);
    ASSERT_TRUE(remove.RemoveParticle(opaqueRenderId));
    ASSERT_TRUE(database.Apply(remove.Build(2)).IsApplied());

    RVX::RenderSceneMutationAccumulator reusedId;
    reusedId.Begin(2);
    ASSERT_TRUE(reusedId.UpsertParticle(
        MakeParticle(opaqueRenderId, 85, 7, 1, 1), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(reusedId.Build(3)).code);
    EXPECT_EQ(database.GetRevision(), 2U);
    EXPECT_FALSE(database.GetParticles().contains(opaqueRenderId));
}

TEST(RenderSceneRevisionValidation,
     FeatureRenderIdRetainsExclusiveSourceOwnershipAcrossRemoval)
{
    constexpr RVX::uint64 sharedRenderId = 0x9A5E000100000001ull;

    RVX::RenderSceneDatabase database;
    RVX::RenderSceneMutationAccumulator first;
    first.Begin(0, true);
    ASSERT_TRUE(first.UpsertParticle(
        MakeParticle(sharedRenderId, 71, 3, 0, 1), true));
    ASSERT_TRUE(database.Apply(first.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator remove;
    remove.Begin(1);
    ASSERT_TRUE(remove.RemoveParticle(sharedRenderId));
    ASSERT_TRUE(database.Apply(remove.Build(2)).IsApplied());
    EXPECT_FALSE(database.GetParticles().contains(sharedRenderId));

    RVX::RenderSceneMutationAccumulator historicalCollision;
    historicalCollision.Begin(2);
    ASSERT_TRUE(historicalCollision.UpsertParticle(
        MakeParticle(sharedRenderId, 72, 9, 0, 1), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              database.Apply(historicalCollision.Build(3)).code);
    EXPECT_EQ(database.GetRevision(), 2U);

    RVX::RenderSceneDatabase currentOwnerDatabase;
    RVX::RenderSceneMutationAccumulator currentOwner;
    currentOwner.Begin(0, true);
    ASSERT_TRUE(currentOwner.UpsertParticle(
        MakeParticle(sharedRenderId, 71, 3, 0, 1), true));
    ASSERT_TRUE(currentOwnerDatabase.Apply(currentOwner.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator currentCollision;
    currentCollision.Begin(1);
    ASSERT_TRUE(currentCollision.UpsertParticle(
        MakeParticle(sharedRenderId, 72, 9, 0, 1), true));
    EXPECT_EQ(RVX::RenderSceneUpdateApplyCode::InvalidMutation,
              currentOwnerDatabase.Apply(currentCollision.Build(2)).code);
    EXPECT_EQ(currentOwnerDatabase.GetRevision(), 1U);
    ASSERT_NE(currentOwnerDatabase.GetParticles().find(sharedRenderId),
              currentOwnerDatabase.GetParticles().end());
    EXPECT_EQ(currentOwnerDatabase.GetParticles().at(sharedRenderId)
                  .header.source.sceneRuntimeId,
              71U);
}

TEST(RenderSceneRevisionValidation, DecalAndProbeMutationsArePersistentAndGenerationSafe)
{
    const RVX::RenderSceneObjectSourceIdentity decalSource =
        MakeSource(51, 12, 0);
    const RVX::RenderSceneObjectSourceIdentity probeSource =
        MakeSource(51, 18, 0);
    const RVX::uint64 decalId = DeriveRenderId(
        51, 12, 0, RVX::SceneECS::FrozenSceneObjectType::Decal);
    const RVX::uint64 probeId = DeriveRenderId(
        51, 18, 0, RVX::SceneECS::FrozenSceneObjectType::Probe);

    RVX::RenderDecalSnapshot decal;
    decal.decalId = decalId;
    decal.source = decalSource;
    decal.opacity = 0.75f;
    decal.sortOrder = 4;

    RVX::RenderProbeSnapshot probe;
    probe.probeId = probeId;
    probe.source = probeSource;
    probe.kind = RVX::RenderProbeKind::Reflection;
    probe.shape = RVX::RenderProbeShape::Box;
    probe.priority = 3;

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertDecal(decal, true));
    ASSERT_TRUE(reset.UpsertProbe(probe, true));

    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
    ASSERT_EQ(database.GetDecalCount(), 1u);
    ASSERT_EQ(database.GetProbeCount(), 1u);
    ASSERT_NE(database.FindDecal(decalId), nullptr);
    ASSERT_NE(database.FindProbe(probeId), nullptr);
    EXPECT_FLOAT_EQ(database.FindDecal(decalId)->opacity, 0.75f);
    EXPECT_EQ(database.FindProbe(probeId)->priority, 3);

    decal.opacity = 0.25f;
    RVX::RenderSceneMutationAccumulator update;
    update.Begin(1);
    ASSERT_TRUE(update.UpsertDecal(decal));
    ASSERT_TRUE(update.RemoveProbe(probeId));
    ASSERT_TRUE(database.Apply(update.Build(2)).IsApplied());
    ASSERT_NE(database.FindDecal(decalId), nullptr);
    EXPECT_FLOAT_EQ(database.FindDecal(decalId)->opacity, 0.25f);
    EXPECT_EQ(database.FindProbe(probeId), nullptr);

    RVX::RenderProbeSnapshot staleProbe = probe;
    staleProbe.probeId = probeId;
    RVX::RenderSceneMutationAccumulator stale;
    stale.Begin(2);
    ASSERT_TRUE(stale.UpsertProbe(staleProbe, true));
    EXPECT_EQ(database.Apply(stale.Build(3)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 2u);

    const RVX::uint64 nextProbeId = DeriveRenderId(
        51, 18, 1, RVX::SceneECS::FrozenSceneObjectType::Probe);
    probe.probeId = nextProbeId;
    probe.source = MakeSource(51, 18, 1);
    RVX::RenderSceneMutationAccumulator replacement;
    replacement.Begin(2);
    ASSERT_TRUE(replacement.UpsertProbe(probe, true));
    ASSERT_TRUE(database.Apply(replacement.Build(3)).IsApplied());
    EXPECT_NE(database.FindProbe(nextProbeId), nullptr);
}
