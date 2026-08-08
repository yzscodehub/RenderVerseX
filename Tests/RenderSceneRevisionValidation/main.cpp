#include "Render/Renderer/RenderSceneDatabase.h"
#include "RenderContracts/RenderFramePacketV5.h"

#include <gtest/gtest.h>

namespace
{
    RVX::RenderPrimitiveSnapshot MakePrimitive(RVX::uint64 id,
                                                RVX::uint64 sortKey = 0)
    {
        RVX::RenderPrimitiveSnapshot primitive;
        primitive.objectId = id;
        primitive.sortKey = sortKey;
        return primitive;
    }

    RVX::RenderSceneUpdateBatch MakeFullReset(RVX::uint64 revision)
    {
        RVX::RenderSceneMutationAccumulator accumulator;
        accumulator.Begin(0, true);
        EXPECT_TRUE(accumulator.UpsertPrimitive(MakePrimitive(11), true));
        return accumulator.Build(revision);
    }

    constexpr RVX::uint64 PackHandle(RVX::uint32 index,
                                     RVX::uint32 generation)
    {
        return (static_cast<RVX::uint64>(generation) << 32u) |
               (static_cast<RVX::uint64>(index) + 1u);
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

TEST(RenderSceneRevisionValidation, ExposesAuthoritativeSceneAndV5FrameState)
{
    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(22, 2), true));
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(11, 1), true));

    RVX::RenderLightSnapshot light;
    light.lightId = 7;
    light.intensity = 4.0f;
    ASSERT_TRUE(reset.UpsertLight(light, true));

    RVX::ParticleRenderSnapshotItem particle;
    particle.instanceId = 31;
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
    view.viewportWidth = 1280;
    view.viewportHeight = 720;

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
    RVX::RenderViewSnapshot view;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
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
    const RVX::uint64 oldId = PackHandle(7, 3);
    const RVX::uint64 staleId = PackHandle(7, 2);
    const RVX::uint64 replacementId = PackHandle(7, 4);

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(MakePrimitive(oldId), true));
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());

    RVX::RenderSceneMutationAccumulator stale;
    stale.Begin(1);
    ASSERT_TRUE(stale.UpsertPrimitive(MakePrimitive(staleId), true));
    EXPECT_EQ(database.Apply(stale.Build(2)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 1u);
    EXPECT_NE(database.FindPrimitive(oldId), nullptr);

    RVX::RenderSceneMutationAccumulator missingRemove;
    missingRemove.Begin(1);
    ASSERT_TRUE(missingRemove.UpsertPrimitive(
        MakePrimitive(replacementId), true));
    EXPECT_EQ(database.Apply(missingRemove.Build(2)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 1u);

    RVX::RenderSceneMutationAccumulator replace;
    replace.Begin(1);
    ASSERT_TRUE(replace.RemovePrimitive(oldId));
    ASSERT_TRUE(replace.UpsertPrimitive(MakePrimitive(replacementId), true));
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
    ASSERT_TRUE(resurrect.UpsertPrimitive(MakePrimitive(replacementId), true));
    EXPECT_EQ(database.Apply(resurrect.Build(4)).code,
              RVX::RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(database.GetRevision(), 3u);

    const RVX::uint64 nextGenerationId = PackHandle(7, 5);
    RVX::RenderSceneMutationAccumulator nextGeneration;
    nextGeneration.Begin(3);
    ASSERT_TRUE(nextGeneration.UpsertPrimitive(
        MakePrimitive(nextGenerationId), true));
    ASSERT_TRUE(database.Apply(nextGeneration.Build(4)).IsApplied());
    EXPECT_NE(database.FindPrimitive(nextGenerationId), nullptr);
}

TEST(RenderSceneRevisionValidation, DecalAndProbeMutationsArePersistentAndGenerationSafe)
{
    const RVX::uint64 decalId = PackHandle(12, 1);
    const RVX::uint64 probeId = PackHandle(18, 1);

    RVX::RenderDecalSnapshot decal;
    decal.decalId = decalId;
    decal.opacity = 0.75f;
    decal.sortOrder = 4;

    RVX::RenderProbeSnapshot probe;
    probe.probeId = probeId;
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

    const RVX::uint64 nextProbeId = PackHandle(18, 2);
    probe.probeId = nextProbeId;
    RVX::RenderSceneMutationAccumulator replacement;
    replacement.Begin(2);
    ASSERT_TRUE(replacement.UpsertProbe(probe, true));
    ASSERT_TRUE(database.Apply(replacement.Build(3)).IsApplied());
    EXPECT_NE(database.FindProbe(nextProbeId), nullptr);
}
