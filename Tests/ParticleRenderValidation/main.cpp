#include "Core/MathTypes.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "Render/Passes/ParticleFeaturePass.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
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

    void ResealEffectDescriptor(RVX::ParticleRenderSnapshotItem& item)
    {
        item.effectDescriptor.contentHash =
            item.effectDescriptor.ComputeContentHash();
        item.cpuPacket.effectDescriptorHash = item.effectDescriptor.contentHash;
        item.cpuPacket.contentHash = item.cpuPacket.ComputeContentHash();
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

TEST(ParticleRenderValidation, AcceptsSealedGfx17SoftDepthDescriptor)
{
    RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                      {MakeParticle(1u, -2.0f)}),
    });
    RVX::ParticleRenderSnapshotItem& item = snapshot.items.front();
    item.effectDescriptor.softDepthMode =
        RVX::ParticleRenderSoftDepthMode::Optional;
    item.effectDescriptor.softDepthFadeMeters = 1.5f;
    item.effectDescriptor.softDepthContrast = 2.0f;
    ResealEffectDescriptor(item);

    const RVX::ParticleBillboardDrawPlan plan =
        RVX::BuildParticleBillboardDrawPlan(snapshot, MakeView());
    ASSERT_TRUE(plan.valid) << plan.diagnostic;
    ASSERT_EQ(plan.items.size(), 1u);
    EXPECT_EQ(plan.items.front().effectDescriptor.softDepthMode,
              RVX::ParticleRenderSoftDepthMode::Optional);
    EXPECT_EQ(plan.items.front().effectDescriptorHash,
              item.effectDescriptor.contentHash);
}

TEST(ParticleRenderValidation,
     SplitsContiguousBatchesForExactEffectAndFlipbookGenerations)
{
    RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::Additive,
                      {MakeParticle(1u, -2.0f)}),
        MakeReadyItem(2u, 2u, 2u,
                      RVX::ParticleRenderSnapshotBlendMode::Additive,
                      {MakeParticle(2u, -2.0f)}),
    });

    for (RVX::uint32 index = 0u; index < snapshot.items.size(); ++index)
    {
        RVX::ParticleRenderSnapshotItem& item = snapshot.items[index];
        item.effectDescriptor.flipbook = {
            .textureAssetId = RVX::AssetId{100u + index},
            .texture = RVX::RenderResourceHandle{10u, 5u + index},
            .committedContentRevision = 20u + index,
            .tiles = {1u, 1u},
            .firstFrame = 0u,
            .frameCount = 1u,
            .framesPerSecond = 0.0f,
            .uvOffset = {0.0f, 0.0f},
            .uvScale = {1.0f, 1.0f},
            .required = false,
        };
        ResealEffectDescriptor(item);
    }

    const RVX::ParticleBillboardDrawPlan plan =
        RVX::BuildParticleBillboardDrawPlan(snapshot, MakeView());
    ASSERT_TRUE(plan.valid) << plan.diagnostic;
    ASSERT_EQ(plan.batches.size(), 2u);
    EXPECT_NE(plan.batches[0].effectDescriptorHash,
              plan.batches[1].effectDescriptorHash);
    EXPECT_NE(plan.batches[0].flipbookTexture,
              plan.batches[1].flipbookTexture);
    EXPECT_NE(plan.batches[0].flipbookContentRevision,
              plan.batches[1].flipbookContentRevision);
}

TEST(ParticleRenderValidation,
     RejectsLightHintsThatDoNotMatchTheirSealedSourceAndEffectPolicy)
{
    RVX::ParticleRenderSnapshot snapshot = MakeCompleteSnapshot({
        MakeReadyItem(1u, 1u, 1u,
                      RVX::ParticleRenderSnapshotBlendMode::Additive,
                      {MakeParticle(7u, -2.0f)}),
    });
    RVX::ParticleRenderSnapshotItem& item = snapshot.items.front();
    const RVX::ParticleRenderParticleData& particle =
        item.cpuPacket.particles.front();
    item.effectDescriptor.lightHint = {
        .enabled = true,
        .color = {2.0f, 4.0f, 6.0f},
        .intensityCandela = 12.0f,
        .rangeMeters = 5.0f,
        .flickerFrequencyHz = 0.0f,
    };
    item.effectDescriptor.contentHash = item.effectDescriptor.ComputeContentHash();
    item.cpuPacket.effectDescriptorHash = item.effectDescriptor.contentHash;
    item.cpuPacket.lightHint = {
        .enabled = true,
        .stableLightId = 0xCAFEu,
        .sampleTick = item.cpuPacket.simulationTick,
        .sourceParticleId = particle.stableParticleId,
        .position = particle.position,
        .color = {item.effectDescriptor.lightHint.color.x * particle.color.x,
                  item.effectDescriptor.lightHint.color.y * particle.color.y,
                  item.effectDescriptor.lightHint.color.z * particle.color.z},
        .intensityCandela = 6.0f,
        .rangeMeters = item.effectDescriptor.lightHint.rangeMeters,
        .modulation = 0.5f,
    };
    item.cpuPacket.contentHash = item.cpuPacket.ComputeContentHash();

    RVX::ParticleFeaturePass pass;
    pass.SetSnapshot(&snapshot);
    std::vector<RVX::ParticleRenderLightHint> acceptedHints;
    std::string diagnostic;
    ASSERT_TRUE(pass.CollectLightingHints(acceptedHints, diagnostic)) << diagnostic;
    ASSERT_EQ(acceptedHints.size(), 1u);
    EXPECT_EQ(acceptedHints.front().stableLightId, 0xCAFEu);

    RVX::ParticleRenderSnapshot altered = snapshot;
    altered.items.front().cpuPacket.lightHint.rangeMeters += 1.0f;
    altered.items.front().cpuPacket.contentHash =
        altered.items.front().cpuPacket.ComputeContentHash();
    pass.SetSnapshot(&altered);
    EXPECT_FALSE(pass.CollectLightingHints(acceptedHints, diagnostic));
    EXPECT_NE(diagnostic.find("sealed effect policy"), std::string::npos);
}

TEST(ParticleRenderValidation,
     SoftDepthReferenceUsesViewLinearMetersAcrossDepthEncodings)
{
    const auto softFadeMeters = [](float sceneMeters,
                                   float particleMeters,
                                   float fadeMeters)
    {
        return std::clamp((sceneMeters - particleMeters) / fadeMeters,
                          0.0f, 1.0f);
    };
    const auto reverseDeviceDepth = [](float meters, float nearPlane)
    {
        return nearPlane / meters;
    };
    const auto reverseLinearDepth = [](float deviceDepth, float nearPlane)
    {
        return nearPlane / deviceDepth;
    };
    const auto forwardDeviceDepth = [](float meters,
                                       float nearPlane,
                                       float farPlane)
    {
        return farPlane / (farPlane - nearPlane) -
            (nearPlane * farPlane) / ((farPlane - nearPlane) * meters);
    };
    const auto forwardLinearDepth = [](float deviceDepth,
                                       float nearPlane,
                                       float farPlane)
    {
        return (nearPlane * farPlane) /
            (farPlane - deviceDepth * (farPlane - nearPlane));
    };

    constexpr float particleMeters = 4.0f;
    constexpr float sceneMeters = 4.5f;
    constexpr float fadeMeters = 1.0f;
    const float reverseNear = softFadeMeters(
        reverseLinearDepth(reverseDeviceDepth(sceneMeters, 0.1f), 0.1f),
        reverseLinearDepth(reverseDeviceDepth(particleMeters, 0.1f), 0.1f),
        fadeMeters);
    const float reverseFar = softFadeMeters(
        reverseLinearDepth(reverseDeviceDepth(sceneMeters, 1.0f), 1.0f),
        reverseLinearDepth(reverseDeviceDepth(particleMeters, 1.0f), 1.0f),
        fadeMeters);
    const float forwardShortRange = softFadeMeters(
        forwardLinearDepth(forwardDeviceDepth(sceneMeters, 0.1f, 25.0f),
                           0.1f, 25.0f),
        forwardLinearDepth(forwardDeviceDepth(particleMeters, 0.1f, 25.0f),
                           0.1f, 25.0f),
        fadeMeters);
    const float forwardLongRange = softFadeMeters(
        forwardLinearDepth(forwardDeviceDepth(sceneMeters, 0.5f, 5000.0f),
                           0.5f, 5000.0f),
        forwardLinearDepth(forwardDeviceDepth(particleMeters, 0.5f, 5000.0f),
                           0.5f, 5000.0f),
        fadeMeters);

    EXPECT_NEAR(reverseNear, 0.5f, 1.0e-5f);
    EXPECT_NEAR(reverseFar, 0.5f, 1.0e-5f);
    EXPECT_NEAR(forwardShortRange, 0.5f, 2.0e-5f);
    EXPECT_NEAR(forwardLongRange, 0.5f, 2.0e-5f);
}

TEST(ParticleRenderValidation,
     SoftDepthDiscontinuityReferenceUsesTheCoveredIntegerTexel)
{
    // A linear D32 sample on this edge would blend foreground and background
    // depth, making the fade depend on sub-pixel position. The renderer's
    // shader uses Texture.Load() so the covered depth texel is deterministic.
    constexpr std::array<float, 2> deviceDepth = {0.20f, 0.80f};
    const auto integerLoad = [&deviceDepth](float fragmentX)
    {
        const size_t pixel = std::min<size_t>(
            static_cast<size_t>(std::floor(std::max(fragmentX, 0.0f))),
            deviceDepth.size() - 1u);
        return deviceDepth[pixel];
    };
    const auto linearSample = [&deviceDepth](float normalizedX)
    {
        return deviceDepth[0] * (1.0f - normalizedX) +
            deviceDepth[1] * normalizedX;
    };

    EXPECT_FLOAT_EQ(integerLoad(0.99f), 0.20f);
    EXPECT_FLOAT_EQ(integerLoad(1.01f), 0.80f);
    EXPECT_NEAR(linearSample(0.51f), 0.506f, 1.0e-6f);
    EXPECT_NE(integerLoad(1.01f), linearSample(0.51f));
}

TEST(ParticleRenderValidation,
     StraightAlphaReferenceSuppressesPremultipliedAndAdditiveTransparentTexels)
{
    const auto finalAlpha = [](float particleAlpha,
                               float textureAlpha,
                               float attenuation)
    {
        return std::clamp(particleAlpha * textureAlpha * attenuation,
                          0.0f, 1.0f);
    };
    constexpr float sourceRgb = 8.0f;
    const float transparentAlpha = finalAlpha(0.9f, 0.0f, 1.0f);
    const float translucentAlpha = finalAlpha(0.8f, 0.5f, 0.75f);
    const float premultipliedRgb = sourceRgb * translucentAlpha;
    const float additiveRgb = sourceRgb * translucentAlpha;

    EXPECT_FLOAT_EQ(sourceRgb * transparentAlpha, 0.0f);
    EXPECT_FLOAT_EQ(sourceRgb * transparentAlpha, 0.0f);
    EXPECT_LE(premultipliedRgb, sourceRgb);
    EXPECT_LE(additiveRgb, sourceRgb);
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
