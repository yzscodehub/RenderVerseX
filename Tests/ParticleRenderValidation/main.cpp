#include "Core/MathTypes.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "Render/Passes/ParticleFeaturePass.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] RVX::ParticleRenderParticleData MakeParticle(
        RVX::uint32 stableParticleId,
        float zPosition)
    {
        return {
            .stableParticleId = stableParticleId,
            .position = {0.0f, 0.0f, zPosition},
            .lifetime = 2.0f,
            .velocity = {0.0f, 0.0f, 0.0f},
            .age = 0.25f,
            .color = {1.0f, 0.5f, 0.25f, 0.75f},
            .size = {1.0f, 1.0f},
            .rotation = 0.0f,
        };
    }

    [[nodiscard]] RVX::ParticleRenderSnapshotItem MakeReadyItem(
        RVX::uint64 systemId,
        RVX::uint64 instanceId,
        RVX::uint32 entitySlot,
        RVX::ParticleRenderSnapshotBlendMode blendMode,
        std::vector<RVX::ParticleRenderParticleData> particles,
        RVX::uint64 deterministicInstanceKey = 0u)
    {
        RVX::ParticleCpuPacket packet;
        packet.simulationTick = 33u;
        packet.deterministicSeed = 0x13579BDFu;
        packet.deterministicInstanceKey = deterministicInstanceKey != 0u
            ? deterministicInstanceKey
            : ((static_cast<RVX::uint64>(entitySlot) << 32u) | 1u);
        packet.effectiveSimulationSeed =
            packet.deterministicSeed ^ static_cast<RVX::uint32>(
                packet.deterministicInstanceKey);
        packet.particles = std::move(particles);
        packet.contentHash = packet.ComputeContentHash();

        RVX::ParticleRenderSnapshotItem item;
        item.systemId = systemId;
        item.instanceId = instanceId;
        item.header = {
            .channelId = RVX::RenderFeatureChannelId::Particle,
            .schemaVersion = RVX::ParticleRenderFeatureChannelDescriptor::SchemaVersion,
            .captureSequence = 33u,
            .configurationRevision = 7u,
            .source = {
                .sceneRuntimeId = 0x51A9u,
                .entitySlot = entitySlot,
                .entityGeneration = 2u,
            },
        };
        item.worldMatrix = RVX::MakeTranslation({0.0f, 0.0f, -50.0f});
        item.renderMode = RVX::ParticleRenderSnapshotMode::Billboard;
        item.blendMode = blendMode;
        item.simulationBackend =
            RVX::ParticleRenderSnapshotSimulationBackend::CPU;
        item.payloadStatus =
            RVX::ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
        item.aliveParticleCount = static_cast<RVX::uint32>(packet.particles.size());
        item.maxParticleCount = 32u;
        item.visible = true;
        item.simulationSupported = true;
        item.sortingSupported = true;
        item.cpuPacket = std::move(packet);
        return item;
    }

    [[nodiscard]] RVX::ParticleRenderSnapshot MakeCompleteSnapshot(
        std::vector<RVX::ParticleRenderSnapshotItem> items)
    {
        RVX::ParticleRenderSnapshot snapshot;
        snapshot.BeginBuild(33u);
        snapshot.items = std::move(items);
        snapshot.MarkComplete();
        return snapshot;
    }

    [[nodiscard]] RVX::ViewData MakeView()
    {
        RVX::ViewData view;
        view.cameraPosition = {0.0f, 0.0f, 0.0f};
        view.cameraForward = {0.0f, 0.0f, -1.0f};
        return view;
    }
} // namespace

TEST(ParticleRenderValidation,
     UsesWorldSpacePacketPositionsForStableViewSortingAndContiguousBatches)
{
    RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
        MakeReadyItem(20u, 2u, 20u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(4u, -10.0f)}),
        MakeReadyItem(10u, 1u, 10u,
                      RVX::ParticleRenderSnapshotBlendMode::Premultiplied,
                      {MakeParticle(3u, -10.0f), MakeParticle(8u, -10.0f)}),
    });

    const RVX::ParticleBillboardDrawPlan plan =
        RVX::BuildParticleBillboardDrawPlan(snapshot, MakeView());

    ASSERT_TRUE(plan.valid) << plan.diagnostic;
    ASSERT_EQ(plan.items.size(), 3u);
    // Same view depth is ordered by packet-sealed owner identity, then the
    // immutable particle slot; process RenderScene IDs are diagnostics only.
    EXPECT_EQ(plan.items[0].systemId, 10u);
    EXPECT_EQ(plan.items[0].particle.stableParticleId, 3u);
    EXPECT_EQ(plan.items[1].systemId, 10u);
    EXPECT_EQ(plan.items[1].particle.stableParticleId, 8u);
    EXPECT_EQ(plan.items[2].systemId, 20u);
    EXPECT_EQ(plan.items[2].particle.stableParticleId, 4u);
    // The item matrix intentionally translates by -50m. Packet positions are
    // already world-space and must not be transformed a second time.
    EXPECT_FLOAT_EQ(plan.items[0].particle.position.z, -10.0f);

    ASSERT_EQ(plan.batches.size(), 2u);
    EXPECT_EQ(plan.batches[0].blendMode,
              RVX::ParticleRenderSnapshotBlendMode::Premultiplied);
    EXPECT_EQ(plan.batches[0].firstItem, 0u);
    EXPECT_EQ(plan.batches[0].itemCount, 2u);
    EXPECT_EQ(plan.batches[1].blendMode,
              RVX::ParticleRenderSnapshotBlendMode::AlphaBlend);
    EXPECT_EQ(plan.batches[1].firstItem, 2u);
    EXPECT_EQ(plan.batches[1].itemCount, 1u);
}

TEST(ParticleRenderValidation, RejectsDefaultOrDuplicateChannelEnvelope)
{
    RVX::ParticleRenderSnapshot invalidHeader = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
    });
    invalidHeader.items.front().header = {};
    const RVX::ParticleBillboardDrawPlan invalidHeaderPlan =
        RVX::BuildParticleBillboardDrawPlan(invalidHeader, MakeView());
    EXPECT_FALSE(invalidHeaderPlan.valid);
    EXPECT_EQ(invalidHeaderPlan.failure,
              RVX::ParticleBillboardDrawPlanFailure::InvalidChannelEnvelope);

    RVX::ParticleRenderSnapshot duplicateSource = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
        MakeReadyItem(2u, 2u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::Additive,
                      {MakeParticle(2u, -3.0f)}),
    });
    const RVX::ParticleBillboardDrawPlan duplicateSourcePlan =
        RVX::BuildParticleBillboardDrawPlan(duplicateSource, MakeView());
    EXPECT_FALSE(duplicateSourcePlan.valid);
    EXPECT_EQ(duplicateSourcePlan.failure,
              RVX::ParticleBillboardDrawPlanFailure::DuplicateSourceIdentity);
}

TEST(ParticleRenderValidation, RejectsPacketCorruptionAndInvisibleAggregateMismatch)
{
    RVX::ParticleRenderSnapshot corruptedPacket = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
    });
    RVX::ParticleCpuPacket& packet = corruptedPacket.items.front().cpuPacket;
    packet.particles.front().age = 0.75f; // Hash is intentionally not resealed.
    const RVX::ParticleBillboardDrawPlan corruptedPacketPlan =
        RVX::BuildParticleBillboardDrawPlan(corruptedPacket, MakeView());
    EXPECT_FALSE(corruptedPacketPlan.valid);
    EXPECT_EQ(corruptedPacketPlan.failure,
              RVX::ParticleBillboardDrawPlanFailure::InvalidCpuPacket);

    RVX::ParticleRenderSnapshot hiddenCorruptedPacket = MakeCompleteSnapshot({
        MakeReadyItem(2u, 2u, 2u,
                      RVX::ParticleRenderSnapshotBlendMode::Premultiplied,
                      {MakeParticle(2u, -3.0f)}),
    });
    hiddenCorruptedPacket.items.front().visible = false;
    RVX::ParticleCpuPacket& hiddenPacket =
        hiddenCorruptedPacket.items.front().cpuPacket;
    hiddenPacket.particles.front().size.x = 0.0f;
    const RVX::ParticleBillboardDrawPlan hiddenCorruptedPacketPlan =
        RVX::BuildParticleBillboardDrawPlan(hiddenCorruptedPacket, MakeView());
    EXPECT_FALSE(hiddenCorruptedPacketPlan.valid);
    EXPECT_EQ(hiddenCorruptedPacketPlan.failure,
              RVX::ParticleBillboardDrawPlanFailure::InvalidCpuPacket);

    RVX::ParticleRenderSnapshot aggregateMismatch = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
        MakeReadyItem(2u, 2u, 2u,
                      RVX::ParticleRenderSnapshotBlendMode::Additive,
                      {MakeParticle(2u, -3.0f)}),
    });
    aggregateMismatch.items.back().visible = false;
    aggregateMismatch.metadata.totalAliveParticles = 1u;
    const RVX::ParticleBillboardDrawPlan aggregateMismatchPlan =
        RVX::BuildParticleBillboardDrawPlan(aggregateMismatch, MakeView());
    EXPECT_FALSE(aggregateMismatchPlan.valid);
    EXPECT_EQ(aggregateMismatchPlan.failure,
              RVX::ParticleBillboardDrawPlanFailure::InvalidParticleCount);
}

TEST(ParticleRenderValidation, RejectsSoftParticleItemsUntilGfx17)
{
    RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
    });
    snapshot.items.front().softParticlesEnabled = true;

    const RVX::ParticleBillboardDrawPlan plan =
        RVX::BuildParticleBillboardDrawPlan(snapshot, MakeView());
    EXPECT_FALSE(plan.valid);
    EXPECT_EQ(plan.failure,
              RVX::ParticleBillboardDrawPlanFailure::UnsupportedSoftParticles);
}

TEST(ParticleRenderValidation,
     StablePacketKeysMakeEqualDepthOrderingIndependentOfProcessInstanceIds)
{
    const auto buildPlan = [](RVX::uint64 firstSystem,
                              RVX::uint64 firstInstance,
                              RVX::uint64 secondSystem,
                              RVX::uint64 secondInstance)
    {
        const RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
            MakeReadyItem(firstSystem, firstInstance, 1u,
                          RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                          {MakeParticle(1u, -4.0f)}, 0xF0u),
            MakeReadyItem(secondSystem, secondInstance, 2u,
                          RVX::ParticleRenderSnapshotBlendMode::Premultiplied,
                          {MakeParticle(2u, -4.0f)}, 0x0Fu),
        });
        return RVX::BuildParticleBillboardDrawPlan(snapshot, MakeView());
    };

    const RVX::ParticleBillboardDrawPlan first =
        buildPlan(700u, 900u, 40u, 50u);
    const RVX::ParticleBillboardDrawPlan second =
        buildPlan(2u, 3u, 4000u, 8000u);

    ASSERT_TRUE(first.valid) << first.diagnostic;
    ASSERT_TRUE(second.valid) << second.diagnostic;
    ASSERT_EQ(first.items.size(), 2u);
    ASSERT_EQ(second.items.size(), 2u);
    EXPECT_EQ(first.items[0].deterministicInstanceKey, 0x0Fu);
    EXPECT_EQ(first.items[1].deterministicInstanceKey, 0xF0u);
    EXPECT_EQ(second.items[0].deterministicInstanceKey,
              first.items[0].deterministicInstanceKey);
    EXPECT_EQ(second.items[1].deterministicInstanceKey,
              first.items[1].deterministicInstanceKey);
    EXPECT_EQ(second.items[0].particle.stableParticleId,
              first.items[0].particle.stableParticleId);
    EXPECT_EQ(second.items[1].particle.stableParticleId,
              first.items[1].particle.stableParticleId);
}

TEST(ParticleRenderValidation,
     SealedZeroAliveAndInactiveMetadataSnapshotsAreSupportedNoOps)
{
    const RVX::ParticleRenderSnapshot zeroAlive = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {}),
    });
    const RVX::ParticleBillboardDrawPlan zeroAlivePlan =
        RVX::BuildParticleBillboardDrawPlan(zeroAlive, MakeView());
    ASSERT_TRUE(zeroAlivePlan.valid) << zeroAlivePlan.diagnostic;
    EXPECT_TRUE(zeroAlivePlan.items.empty());

    RVX::ParticleFeaturePass zeroAlivePass;
    zeroAlivePass.SetSnapshot(&zeroAlive);
    EXPECT_TRUE(zeroAlivePass.IsRequestedEnabled());
    EXPECT_TRUE(zeroAlivePass.IsSupported());
    EXPECT_FALSE(zeroAlivePass.IsEnabled());

    RVX::ParticleRenderSnapshotItem inactive = MakeReadyItem(
        2u, 2u, 2u, RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
        {MakeParticle(2u, -2.0f)});
    inactive.visible = false;
    inactive.simulationBackend =
        RVX::ParticleRenderSnapshotSimulationBackend::None;
    inactive.payloadStatus =
        RVX::ParticleRenderSnapshotPayloadStatus::MetadataOnly;
    inactive.aliveParticleCount = 0u;
    inactive.cpuPacket = {};
    const RVX::ParticleRenderSnapshot inactiveSnapshot =
        MakeCompleteSnapshot({std::move(inactive)});
    const RVX::ParticleBillboardDrawPlan inactivePlan =
        RVX::BuildParticleBillboardDrawPlan(inactiveSnapshot, MakeView());
    ASSERT_TRUE(inactivePlan.valid) << inactivePlan.diagnostic;
    EXPECT_TRUE(inactivePlan.items.empty());

    RVX::ParticleFeaturePass inactivePass;
    inactivePass.SetSnapshot(&inactiveSnapshot);
    EXPECT_TRUE(inactivePass.IsRequestedEnabled());
    EXPECT_TRUE(inactivePass.IsSupported());
    EXPECT_FALSE(inactivePass.IsEnabled());
}
