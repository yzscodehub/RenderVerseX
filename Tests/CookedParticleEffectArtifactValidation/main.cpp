#include "Resource/Cooked/CookedParticleEffectArtifact.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

namespace
{
    RVX::Resource::CookedParticleEffectArtifact MakeArtifact()
    {
        RVX::Resource::CookedParticleEffectArtifact artifact;
        artifact.logicalAssetId = "Effects/Fire";
        artifact.blendMode = RVX::Resource::CookedParticleEffectBlendMode::Additive;
        artifact.emitter.maxParticles = 512u;
        artifact.emitter.looping = true;
        artifact.emitter.durationSeconds = 5.0f;
        artifact.emitter.emissionRate = 42.0f;
        artifact.emitter.position = {0.0f, 1.0f, 0.0f};
        artifact.emitter.speedMin = 0.5f;
        artifact.emitter.speedMax = 1.5f;
        artifact.emitter.lifetimeMin = 0.4f;
        artifact.emitter.lifetimeMax = 1.2f;
        artifact.emitter.initialColor = {4.0f, 1.2f, 0.1f, 1.0f};
        artifact.emitter.sizeMin = 0.1f;
        artifact.emitter.sizeMax = 0.4f;
        artifact.flipbook.textureArtifactPath = "Textures/Fire.rva";
        artifact.flipbook.expectedContentHash[0] = 0xF1u;
        artifact.flipbook.tilesX = 4u;
        artifact.flipbook.tilesY = 4u;
        artifact.flipbook.firstFrame = 0u;
        artifact.flipbook.frameCount = 16u;
        artifact.flipbook.framesPerSecond = 24.0f;
        artifact.flipbook.uvOffset = {0.0f, 0.0f};
        artifact.flipbook.uvScale = {1.0f, 1.0f};
        artifact.softDepth.policy =
            RVX::Resource::CookedParticleEffectSoftDepthPolicy::Optional;
        artifact.softDepth.fadeDistanceMeters = 0.3f;
        artifact.softDepth.contrastPower = 1.5f;
        artifact.distortion.distortionStrengthPixels = 3.0f;
        artifact.reactiveMask.reactiveContribution = 0.75f;
        artifact.lightHint.enabled = true;
        artifact.lightHint.color = {1.0f, 0.4f, 0.1f};
        artifact.lightHint.intensityCandela = 120.0f;
        artifact.lightHint.rangeMeters = 4.0f;
        artifact.lightHint.flickerFrequencyHz = 2.0f;
        return artifact;
    }
}

TEST(CookedParticleEffectArtifactValidation,
     RoundTripIsDeterministicAndPreservesRendererNeutralValues)
{
    const RVX::Resource::CookedParticleEffectArtifact artifact = MakeArtifact();
    std::vector<RVX::uint8> first;
    std::vector<RVX::uint8> second;
    std::string error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedParticleEffectArtifact(
        artifact, first, error)) << error;
    ASSERT_TRUE(RVX::Resource::SerializeCookedParticleEffectArtifact(
        artifact, second, error)) << error;
    EXPECT_EQ(first, second);

    RVX::Resource::CookedParticleEffectArtifact parsed;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedParticleEffectArtifact(
        first, parsed, error)) << error;
    EXPECT_EQ(parsed.logicalAssetId, artifact.logicalAssetId);
    EXPECT_EQ(parsed.flipbook.textureArtifactPath, artifact.flipbook.textureArtifactPath);
    EXPECT_EQ(parsed.flipbook.expectedContentHash,
              artifact.flipbook.expectedContentHash);
    EXPECT_EQ(parsed.flipbook.tilesX, 4u);
    EXPECT_EQ(parsed.flipbook.frameCount, 16u);
    EXPECT_EQ(parsed.emitter.initialColor, artifact.emitter.initialColor);
    EXPECT_FLOAT_EQ(parsed.emitter.sizeMin, artifact.emitter.sizeMin);
    EXPECT_FLOAT_EQ(parsed.emitter.sizeMax, artifact.emitter.sizeMax);
    EXPECT_FLOAT_EQ(parsed.distortion.distortionStrengthPixels, 3.0f);
    EXPECT_FLOAT_EQ(parsed.reactiveMask.reactiveContribution, 0.75f);
    EXPECT_TRUE(parsed.lightHint.enabled);
    EXPECT_NE(parsed.payloadHash, RVX::Hash::SHA256Digest{});
}

TEST(CookedParticleEffectArtifactValidation,
     RejectsTamperingPathEscapeInvalidPolicyAndNonFiniteValues)
{
    RVX::Resource::CookedParticleEffectArtifact artifact = MakeArtifact();
    std::string error;
    artifact.flipbook.textureArtifactPath = "../Fire.rva";
    EXPECT_FALSE(artifact.IsValid(error));
    EXPECT_NE(error.find("canonical"), std::string::npos);

    artifact = MakeArtifact();
    artifact.softDepth.policy =
        RVX::Resource::CookedParticleEffectSoftDepthPolicy::Disabled;
    artifact.softDepth.fadeDistanceMeters = 0.0f;
    artifact.softDepth.contrastPower = 0.0f;
    EXPECT_FALSE(artifact.IsValid(error));

    artifact = MakeArtifact();
    artifact.flipbook.expectedContentHash = {};
    EXPECT_FALSE(artifact.IsValid(error));

    artifact = MakeArtifact();
    artifact.emitter.emissionRate = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(artifact.IsValid(error));

    artifact = MakeArtifact();
    std::vector<RVX::uint8> bytes;
    ASSERT_TRUE(RVX::Resource::SerializeCookedParticleEffectArtifact(
        artifact, bytes, error)) << error;
    bytes.back() ^= 0x80u;
    RVX::Resource::CookedParticleEffectArtifact parsed;
    EXPECT_FALSE(RVX::Resource::DeserializeCookedParticleEffectArtifact(
        bytes, parsed, error));
    EXPECT_NE(error.find("hash"), std::string::npos);
}
