#pragma once

/**
 * @file CookedParticleEffectArtifact.h
 * @brief Strict, renderer-neutral cooked CPU billboard particle effect product.
 *
 * This contract deliberately contains values only.  It does not expose Particle
 * runtime types, RHI state, shader programs, or authoring JSON.  The Particle
 * module is responsible for translating a valid product into its runtime model.
 */

#include "Core/Hash/SHA256.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr char RVX_PARTICLE_EFFECT_ARTIFACT_MAGIC[] =
        "RVX_PARTICLE_EFFECT_ARTIFACT_V1";
    inline constexpr uint32 RVX_PARTICLE_EFFECT_ARTIFACT_SCHEMA_VERSION = 1;

    enum class CookedParticleEffectBlendMode : uint8
    {
        AlphaBlend = 0,
        Premultiplied,
        Additive,
    };

    enum class CookedParticleEffectSoftDepthPolicy : uint8
    {
        Disabled = 0,
        Optional,
        Required,
    };

    /** @brief Deterministic source-independent CPU billboard emitter settings. */
    struct CookedParticleEffectEmitter
    {
        uint32 maxParticles = 0;
        bool looping = true;
        float32 durationSeconds = 0.0f;
        float32 emissionRate = 0.0f;
        Vec3 position{};
        float32 speedMin = 0.0f;
        float32 speedMax = 0.0f;
        float32 lifetimeMin = 0.0f;
        float32 lifetimeMax = 0.0f;
        /** @brief Constant spawn color. Instance seeds belong to ECS, not the product. */
        Vec4 initialColor{};
        /** @brief Uniform billboard-size range in meters. */
        float32 sizeMin = 0.0f;
        float32 sizeMax = 0.0f;
        float32 rotationDegreesMin = 0.0f;
        float32 rotationDegreesMax = 0.0f;
        float32 rotationSpeedDegreesMin = 0.0f;
        float32 rotationSpeedDegreesMax = 0.0f;
    };

    struct CookedParticleEffectFlipbook
    {
        std::string textureArtifactPath;
        /**
         * @brief SHA-256 of the complete contained .rva product.
         *
         * The path is relative to the effect product package directory. The
         * effect cannot silently bind another atlas with the same relative name.
         * This is a product dependency identity, not a shader or RHI handle,
         * so the artifact remains renderer neutral.
         */
        Hash::SHA256Digest expectedContentHash{};
        uint32 tilesX = 0;
        uint32 tilesY = 0;
        uint32 firstFrame = 0;
        uint32 frameCount = 0;
        float32 framesPerSecond = 0.0f;
        Vec2 uvOffset{};
        Vec2 uvScale{};
    };

    struct CookedParticleEffectSoftDepth
    {
        CookedParticleEffectSoftDepthPolicy policy =
            CookedParticleEffectSoftDepthPolicy::Disabled;
        float32 fadeDistanceMeters = 0.0f;
        float32 contrastPower = 1.0f;
    };

    struct CookedParticleEffectDistortion
    {
        float32 distortionStrengthPixels = 0.0f;
    };

    struct CookedParticleEffectReactiveMask
    {
        float32 reactiveContribution = 0.0f;
    };

    /** @brief At most one non-shadowing, low-frequency dynamic-light hint. */
    struct CookedParticleEffectLightHint
    {
        bool enabled = false;
        Vec3 color{};
        float32 intensityCandela = 0.0f;
        float32 rangeMeters = 0.0f;
        float32 flickerFrequencyHz = 0.0f;
    };

    struct CookedParticleEffectArtifact
    {
        std::string logicalAssetId;
        CookedParticleEffectBlendMode blendMode =
            CookedParticleEffectBlendMode::AlphaBlend;
        CookedParticleEffectEmitter emitter;
        CookedParticleEffectFlipbook flipbook;
        CookedParticleEffectSoftDepth softDepth;
        CookedParticleEffectDistortion distortion;
        CookedParticleEffectReactiveMask reactiveMask;
        CookedParticleEffectLightHint lightHint;
        Hash::SHA256Digest payloadHash{};

        [[nodiscard]] bool IsValid(std::string& outError) const;
    };

    [[nodiscard]] bool SerializeCookedParticleEffectArtifact(
        const CookedParticleEffectArtifact& artifact,
        std::vector<uint8>& outBytes,
        std::string& outError);

    [[nodiscard]] bool DeserializeCookedParticleEffectArtifact(
        std::span<const uint8> bytes,
        CookedParticleEffectArtifact& outArtifact,
        std::string& outError);
} // namespace RVX::Resource
