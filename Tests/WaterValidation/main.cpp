#include "Water/GerstnerSurface.h"

#include "RenderContracts/WaterGerstnerMath.h"

#include <gtest/gtest.h>

#include <bit>
#include <filesystem>
#include <limits>

namespace
{
    [[nodiscard]] RVX::WaterRenderSnapshotPayload MakePacket()
    {
        RVX::WaterRenderSnapshotPayload packet;
        packet.payloadStatus = RVX::WaterRenderSnapshotPayloadStatus::Ready;
        packet.componentId = 91;
        packet.surfaceAssetId = {.value = 101};
        packet.materialAssetId = {.value = 102};
        packet.worldBounds = RVX::AABB({-5.8f, -4.8f, -5.8f}, {5.8f, 0.8f, 5.8f});
        packet.size = {10.0f, 10.0f};
        packet.depth = 4.0f;
        packet.gerstnerWaves.waveCount = 2;
        packet.gerstnerWaves.waves[0] = {
            .direction = {1.0f, 0.0f}, .amplitude = 0.5f,
            .wavelength = 8.0f, .steepness = 0.4f,
            .speed = 1.0f, .phaseOffsetRadians = 0.0f};
        packet.gerstnerWaves.waves[1] = {
            .direction = {0.0f, 1.0f}, .amplitude = 0.3f,
            .wavelength = 5.0f, .steepness = 0.3f,
            .speed = 0.8f, .phaseOffsetRadians = 0.2f};
        EXPECT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(packet.gerstnerWaves));
        packet.shallowColor = {0.02f, 0.24f, 0.30f};
        packet.deepColor = {0.005f, 0.04f, 0.10f};
        packet.foamColor = {0.92f, 0.96f, 1.0f};
        packet.absorptionCoefficient = {0.15f, 0.06f, 0.02f};
        packet.transparency = 0.72f;
        packet.reflectionStrength = 0.65f;
        packet.refractionStrength = 0.08f;
        packet.roughness = 0.08f;
        packet.normalScale = 1.0f;
        packet.foamIntensity = 1.0f;
        packet.foamCrestThreshold = 0.62f;
        packet.reflectionEnabled = true;
        packet.refractionEnabled = true;
        packet.foamEnabled = true;
        packet.contentHash = packet.ComputeContentHash();
        return packet;
    }
}

TEST(WaterValidation, SealedWaveSetRejectsHashTrailingAndChoppinessCorruption)
{
    RVX::WaterGerstnerWaveSet waveSet;
    waveSet.waveCount = 1;
    waveSet.waves[0] = {.direction = {2.0f, 0.0f}, .amplitude = 0.5f,
                        .wavelength = 6.0f, .steepness = 0.5f,
                        .speed = 1.0f, .phaseOffsetRadians = 0.0f};
    ASSERT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(waveSet));
    EXPECT_NEAR(RVX::length(waveSet.waves[0].direction), 1.0f, 0.0001f);

    RVX::WaterGerstnerWaveSet trailing = waveSet;
    trailing.waves[1].amplitude = 0.1f;
    EXPECT_FALSE(trailing.IsStructurallyValid());

    RVX::WaterGerstnerWaveSet corruptHash = waveSet;
    ++corruptHash.contentHash;
    EXPECT_FALSE(corruptHash.IsStructurallyValid());

    RVX::WaterGerstnerWaveSet choppy = waveSet;
    choppy.waveCount = 2;
    choppy.waves[1] = choppy.waves[0];
    choppy.waves[0].steepness = 0.75f;
    choppy.waves[1].steepness = 0.75f;
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(choppy));
    EXPECT_FALSE(choppy.IsStructurallyValid());
}

TEST(WaterValidation, WaveSetRejectsInvalidCountsDirectionsAndFiniteValues)
{
    RVX::WaterGerstnerWaveSet waveSet;
    waveSet.waveCount = 1;
    waveSet.waves[0] = {.direction = {1.0f, 0.0f}, .amplitude = 0.5f,
                        .wavelength = 6.0f, .steepness = 0.5f,
                        .speed = 1.0f, .phaseOffsetRadians = 0.0f};

    RVX::WaterGerstnerWaveSet zeroCount = waveSet;
    zeroCount.waveCount = 0;
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(zeroCount));

    RVX::WaterGerstnerWaveSet overCapacity = waveSet;
    overCapacity.waveCount = RVX::RVX_WATER_MAX_GERSTNER_WAVES + 1;
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(overCapacity));

    RVX::WaterGerstnerWaveSet zeroDirection = waveSet;
    zeroDirection.waves[0].direction = {0.0f, 0.0f};
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(zeroDirection));

    RVX::WaterGerstnerWaveSet nanAmplitude = waveSet;
    nanAmplitude.waves[0].amplitude = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(nanAmplitude));

    RVX::WaterGerstnerWaveSet infiniteSpeed = waveSet;
    infiniteSpeed.waves[0].speed = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(infiniteSpeed));

    RVX::WaterGerstnerWaveSet zeroWavelength = waveSet;
    zeroWavelength.waves[0].wavelength = 0.0f;
    EXPECT_FALSE(RVX::CanonicalizeWaterGerstnerWaveSet(zeroWavelength));
}

TEST(WaterValidation, WaveSetCanonicalizesSignedZeroAndFullTurnPhaseIdentity)
{
    RVX::WaterGerstnerWaveSet canonical;
    canonical.waveCount = 1;
    canonical.waves[0] = {.direction = {1.0f, 0.0f}, .amplitude = 0.5f,
                          .wavelength = 6.0f, .steepness = 0.0f,
                          .speed = 0.0f, .phaseOffsetRadians = 0.0f};
    RVX::WaterGerstnerWaveSet equivalent = canonical;
    equivalent.waves[0].direction.y = -0.0f;
    equivalent.waves[0].steepness = -0.0f;
    equivalent.waves[0].speed = -0.0f;
    equivalent.waves[0].phaseOffsetRadians = 6.28318530717958647692f;
    RVX::WaterGerstnerWaveSet negativeEquivalent = canonical;
    negativeEquivalent.waves[0].phaseOffsetRadians = -6.28318530717958647692f;

    ASSERT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(canonical));
    ASSERT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(equivalent));
    ASSERT_TRUE(RVX::CanonicalizeWaterGerstnerWaveSet(negativeEquivalent));
    EXPECT_EQ(canonical.contentHash, equivalent.contentHash);
    EXPECT_EQ(canonical.contentHash, negativeEquivalent.contentHash);
    EXPECT_EQ(std::bit_cast<RVX::uint32>(equivalent.waves[0].direction.y), 0U);
    EXPECT_EQ(std::bit_cast<RVX::uint32>(equivalent.waves[0].steepness), 0U);
    EXPECT_EQ(std::bit_cast<RVX::uint32>(equivalent.waves[0].speed), 0U);
    EXPECT_EQ(std::bit_cast<RVX::uint32>(equivalent.waves[0].phaseOffsetRadians), 0U);
}

TEST(WaterValidation, PacketRequiresConservativeBoundsAndContentIdentity)
{
    RVX::WaterRenderSnapshotPayload packet = MakePacket();
    ASSERT_TRUE(packet.IsStructurallyValid());

    RVX::WaterRenderSnapshotPayload clipped = packet;
    clipped.worldBounds = RVX::AABB({-5.0f, -4.0f, -5.0f}, {5.0f, 0.4f, 5.0f});
    clipped.contentHash = clipped.ComputeContentHash();
    EXPECT_FALSE(clipped.IsStructurallyValid());

    RVX::WaterRenderSnapshotPayload mutated = packet;
    mutated.transparency = 0.5f;
    EXPECT_FALSE(mutated.IsStructurallyValid());

    RVX::WaterRenderSnapshotPayload disabled;
    EXPECT_TRUE(disabled.IsCanonicalDisabled());
    disabled.componentId = 77;
    EXPECT_FALSE(disabled.IsCanonicalDisabled());
}

TEST(WaterValidation, EvaluatorUsesExplicitTimeAndProvidesAnalyticalQueries)
{
    RVX::WaterRenderSnapshotPayload packet = MakePacket();
    const RVX::Water::GerstnerSurfaceEvaluation atZero =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f}, 0.0);
    const RVX::Water::GerstnerSurfaceEvaluation atLater =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f}, 1.0);
    ASSERT_TRUE(atZero.valid);
    ASSERT_TRUE(atLater.valid);
    EXPECT_NE(atZero.displacement, atLater.displacement);
    EXPECT_NEAR(RVX::length(atZero.normal), 1.0f, 0.0001f);
    EXPECT_NEAR(RVX::length(atZero.tangentX), 1.0f, 0.0001f);
    EXPECT_NEAR(RVX::length(atZero.tangentZ), 1.0f, 0.0001f);
    EXPECT_GT(RVX::length(atZero.horizontalVelocity), 0.0f);

    constexpr float h = 0.001f;
    const RVX::Water::GerstnerSurfaceEvaluation xMinus =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f - h, 2.0f}, 0.0);
    const RVX::Water::GerstnerSurfaceEvaluation xPlus =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f + h, 2.0f}, 0.0);
    const RVX::Water::GerstnerSurfaceEvaluation zMinus =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f - h}, 0.0);
    const RVX::Water::GerstnerSurfaceEvaluation zPlus =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f + h}, 0.0);
    ASSERT_TRUE(xMinus.valid);
    ASSERT_TRUE(xPlus.valid);
    ASSERT_TRUE(zMinus.valid);
    ASSERT_TRUE(zPlus.valid);
    const RVX::Vec3 finiteTangentX = RVX::normalize(
        (xPlus.surfacePosition - xMinus.surfacePosition) / (2.0f * h));
    const RVX::Vec3 finiteTangentZ = RVX::normalize(
        (zPlus.surfacePosition - zMinus.surfacePosition) / (2.0f * h));
    const RVX::Vec3 finiteNormal = RVX::normalize(RVX::cross(finiteTangentZ, finiteTangentX));
    EXPECT_GT(RVX::dot(atZero.tangentX, finiteTangentX), 0.999f);
    EXPECT_GT(RVX::dot(atZero.tangentZ, finiteTangentZ), 0.999f);
    EXPECT_GT(RVX::dot(atZero.normal, finiteNormal), 0.999f);

    const RVX::Water::GerstnerSignedVerticalDistance distance =
        RVX::Water::EvaluateGerstnerSignedVerticalDistance(
            packet, {1.0f, atZero.surfacePosition.y + 2.0f, 2.0f}, 0.0);
    ASSERT_TRUE(distance.valid);
    EXPECT_NEAR(distance.signedVerticalDistance, 2.0f, 0.0001f);

    const RVX::Water::GerstnerSurfaceEvaluation hugeTimeFirst =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f}, 1.0e200);
    const RVX::Water::GerstnerSurfaceEvaluation hugeTimeSecond =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f}, 1.0e200);
    const RVX::Water::GerstnerSurfaceEvaluation unrelated =
        RVX::Water::EvaluateGerstnerSurface(packet, {-5.0f, 9.0f}, 37.25);
    const RVX::Water::GerstnerSurfaceEvaluation hugeTimeAfterUnrelated =
        RVX::Water::EvaluateGerstnerSurface(packet, {1.0f, 2.0f}, 1.0e200);
    ASSERT_TRUE(hugeTimeFirst.valid);
    ASSERT_TRUE(hugeTimeSecond.valid);
    ASSERT_TRUE(unrelated.valid);
    ASSERT_TRUE(hugeTimeAfterUnrelated.valid);
    EXPECT_EQ(hugeTimeFirst.displacement, hugeTimeSecond.displacement);
    EXPECT_EQ(hugeTimeFirst.displacement, hugeTimeAfterUnrelated.displacement);
}

TEST(WaterGerstnerCanonicalContract, RuntimeAndPackedGpuMirrorHaveBoundedExplicitPrecisionError)
{
    // The common CPU path keeps k/omega/phase in float64. The GPU boundary
    // deliberately narrows only packed k and the bounded temporal phase to
    // float32. At the maximum representative local coordinate this permits a
    // small trigonometric error; bit parity across CPU libm and GPU sin/cos is
    // neither expected nor required.
    const RVX::WaterRenderSnapshotPayload packet = MakePacket();
    constexpr double longRealisticTimeSeconds = 6.0 * 60.0 * 60.0 + 0.125;
    for (const RVX::Vec2 localPosition : {RVX::Vec2{1.25f, -2.5f},
                                          RVX::Vec2{8191.25f, -8190.75f}})
    {
        const RVX::Water::GerstnerSurfaceEvaluation runtime =
            RVX::Water::EvaluateGerstnerSurface(
                packet, localPosition, longRealisticTimeSeconds);
        RVX::WaterGerstnerSurfaceMathSample canonical;
        RVX::WaterGerstnerSurfaceMathSample gpuMirror;
        ASSERT_TRUE(runtime.valid);
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerSurfaceMath(
            packet.gerstnerWaves, localPosition, longRealisticTimeSeconds, canonical));
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerGpuFloatMirror(
            packet.gerstnerWaves, localPosition, longRealisticTimeSeconds, gpuMirror));

        EXPECT_EQ(runtime.displacement, canonical.displacement);
        EXPECT_EQ(runtime.tangentX, RVX::normalize(canonical.tangentX));
        EXPECT_EQ(runtime.tangentZ, RVX::normalize(canonical.tangentZ));
        EXPECT_NEAR(runtime.displacement.x, gpuMirror.displacement.x, 0.05f);
        EXPECT_NEAR(runtime.displacement.y, gpuMirror.displacement.y, 0.05f);
        EXPECT_NEAR(runtime.displacement.z, gpuMirror.displacement.z, 0.05f);
        EXPECT_GT(RVX::dot(runtime.normal,
                           RVX::normalize(RVX::cross(gpuMirror.tangentZ,
                                                     gpuMirror.tangentX))),
                  0.995f);
    }
}

TEST(WaterGerstnerCanonicalContract, SurfaceTranslationDoesNotChangeLocalWavePhase)
{
    const RVX::WaterRenderSnapshotPayload packet = MakePacket();
    RVX::WaterRenderSnapshotPayload translated = packet;
    translated.worldPosition = {1450.0f, -37.0f, 712.0f};
    const RVX::Vec3 translation = translated.worldPosition - packet.worldPosition;
    translated.worldBounds = RVX::AABB(
        packet.worldBounds.GetMin() + translation,
        packet.worldBounds.GetMax() + translation);
    translated.contentHash = translated.ComputeContentHash();
    ASSERT_TRUE(translated.IsStructurallyValid());

    const RVX::Vec2 localPosition{6789.5f, -5432.25f};
    const RVX::Water::GerstnerSurfaceEvaluation original =
        RVX::Water::EvaluateGerstnerSurface(packet, localPosition, 18342.25);
    const RVX::Water::GerstnerSurfaceEvaluation moved =
        RVX::Water::EvaluateGerstnerSurface(translated, localPosition, 18342.25);
    ASSERT_TRUE(original.valid);
    ASSERT_TRUE(moved.valid);
    EXPECT_EQ(original.displacement, moved.displacement);
    EXPECT_EQ(original.normal, moved.normal);
    EXPECT_EQ(original.horizontalVelocity, moved.horizontalVelocity);
}

TEST(WaterGerstnerCanonicalContract, PerWavePeriodLeavesCanonicalTemporalPhaseInvariant)
{
    const RVX::WaterRenderSnapshotPayload packet = MakePacket();
    constexpr double timeSeconds = 137.625;
    for (RVX::uint32 index = 0; index < packet.gerstnerWaves.waveCount; ++index)
    {
        RVX::WaterGerstnerWaveFrameTerms atTime;
        RVX::WaterGerstnerWaveFrameTerms atWholePeriodsLater;
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerWaveFrameTerms(
            packet.gerstnerWaves.waves[index], timeSeconds, atTime));
        ASSERT_GT(atTime.angularFrequency, 0.0);
        const double period = RVX::RVX_WATER_GERSTNER_TWO_PI_DOUBLE /
            atTime.angularFrequency;
        ASSERT_TRUE(RVX::EvaluateWaterGerstnerWaveFrameTerms(
            packet.gerstnerWaves.waves[index],
            timeSeconds + period * 4096.0, atWholePeriodsLater));
        EXPECT_NEAR(atTime.temporalPhase, atWholePeriodsLater.temporalPhase, 1.0e-9);
    }
}

TEST(WaterValidation, LegacyPlaceholderWaterSourcesAreAbsent)
{
    const std::filesystem::path root{RVX_SOURCE_DIR};
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Private/WaterSimulation.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Private/WaterSurface.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Private/Caustics.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Private/Underwater.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Shaders/WaterSurface.hlsl"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Shaders/Caustics.hlsl"));
    EXPECT_FALSE(std::filesystem::exists(root / "Source/Runtime/Water/Shaders/Underwater.hlsl"));
}
