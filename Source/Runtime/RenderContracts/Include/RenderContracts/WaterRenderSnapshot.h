#pragma once

/**
 * @file WaterRenderSnapshot.h
 * @brief Render-facing water data contract without Feature, Render, or RHI types.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureChannelContracts.h"
#include "RenderContracts/RenderIdentity.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace RVX
{
    enum class WaterRenderSnapshotStatus : uint8
    {
        Empty = 0,
        Complete,
        Incomplete
    };

    enum class WaterRenderSnapshotSurfaceType : uint8
    {
        Ocean = 0,
        Lake,
        River,
        Pool
    };

    enum class WaterRenderSnapshotSimulationType : uint8
    {
        Gerstner = 0,
    };

    /** @brief A packet either supplies a complete renderable surface or is a canonical disable marker. */
    enum class WaterRenderSnapshotPayloadStatus : uint8
    {
        Disabled = 0,
        Ready,
    };

    inline constexpr uint32 RVX_WATER_MAX_GERSTNER_WAVES = 8;
    /** Included in the wave-set hash so a semantic formula change is explicit. */
    inline constexpr uint32 RVX_WATER_GERSTNER_WAVE_SET_SCHEMA_VERSION = 2;
    inline constexpr float32 RVX_WATER_MAX_SURFACE_EXTENT_METERS = 16384.0f;
    inline constexpr float32 RVX_WATER_MAX_DEPTH_METERS = 8192.0f;
    /** Canonical constants used by CPU evaluation, GPU packing, and bounds. */
    inline constexpr float64 RVX_WATER_GERSTNER_TWO_PI_DOUBLE =
        6.28318530717958647692528676655900577;
    inline constexpr float32 RVX_WATER_GERSTNER_TWO_PI_FLOAT =
        6.28318530717958647692f;
    inline constexpr float64 RVX_WATER_GERSTNER_GRAVITY_METERS_PER_SECOND_SQUARED =
        9.81;

    /**
     * @brief One canonical Gerstner component in a horizontal, right-handed Y-up plane.
     *
     * Direction must already be unit length in published packets. The phase is
     * evaluated against the renderer's frozen EffectFrameTime; packets never
     * carry an accumulated clock.
     */
    struct WaterGerstnerWave
    {
        Vec2 direction{0.0f, 0.0f};
        float32 amplitude = 0.0f;
        float32 wavelength = 0.0f;
        float32 steepness = 0.0f;
        float32 speed = 0.0f;
        float32 phaseOffsetRadians = 0.0f;

        [[nodiscard]] bool IsCanonicalUnused() const noexcept
        {
            return direction == Vec2{0.0f, 0.0f} && amplitude == 0.0f &&
                   wavelength == 0.0f && steepness == 0.0f && speed == 0.0f &&
                   phaseOffsetRadians == 0.0f;
        }
    };

    /**
     * @brief Fixed-capacity immutable Gerstner data shared by gameplay and Render.
     *
     * The content hash is FNV-1a over the versioned canonical scalar
     * representation. It deliberately includes every fixed slot, which makes
     * non-zero trailing data an observable contract violation.
     */
    struct WaterGerstnerWaveSet
    {
        std::array<WaterGerstnerWave, RVX_WATER_MAX_GERSTNER_WAVES> waves{};
        uint32 waveCount = 0;
        uint64 contentHash = 0;

        [[nodiscard]] uint64 ComputeContentHash() const noexcept
        {
            constexpr uint64 offsetBasis = 14695981039346656037ull;
            constexpr uint64 prime = 1099511628211ull;
            uint64 value = offsetBasis;
            const auto mixUnsigned = [&value](uint64 source, uint32 byteCount)
            {
                for (uint32 byte = 0; byte < byteCount; ++byte)
                {
                    value ^= static_cast<uint8>(source & 0xffU);
                    value *= prime;
                    source >>= 8U;
                }
            };
            const auto mixFloat = [&mixUnsigned](float32 source)
            {
                mixUnsigned(std::bit_cast<uint32>(source), sizeof(uint32));
            };

            mixUnsigned(RVX_WATER_GERSTNER_WAVE_SET_SCHEMA_VERSION,
                        sizeof(RVX_WATER_GERSTNER_WAVE_SET_SCHEMA_VERSION));
            mixUnsigned(waveCount, sizeof(waveCount));
            for (const WaterGerstnerWave& wave : waves)
            {
                mixFloat(wave.direction.x);
                mixFloat(wave.direction.y);
                mixFloat(wave.amplitude);
                mixFloat(wave.wavelength);
                mixFloat(wave.steepness);
                mixFloat(wave.speed);
                mixFloat(wave.phaseOffsetRadians);
            }
            return value;
        }

        [[nodiscard]] bool IsCanonicalDisabled() const noexcept
        {
            if (waveCount != 0 || contentHash != 0)
            {
                return false;
            }
            for (const WaterGerstnerWave& wave : waves)
            {
                if (!wave.IsCanonicalUnused())
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (waveCount == 0 || waveCount > RVX_WATER_MAX_GERSTNER_WAVES ||
                contentHash == 0)
            {
                return false;
            }

            constexpr float32 directionTolerance = 0.0005f;
            constexpr float32 minimumWavelength = 0.25f;
            constexpr float32 maximumWavelength = 10000.0f;
            constexpr float32 maximumAmplitude = 32.0f;
            constexpr float32 maximumSpeed = 16.0f;
            constexpr float32 pi = 3.14159265358979323846f;
            float32 aggregateSteepness = 0.0f;
            for (uint32 index = 0; index < RVX_WATER_MAX_GERSTNER_WAVES; ++index)
            {
                const WaterGerstnerWave& wave = waves[index];
                if (index >= waveCount)
                {
                    if (!wave.IsCanonicalUnused())
                    {
                        return false;
                    }
                    continue;
                }

                if (!std::isfinite(wave.direction.x) || !std::isfinite(wave.direction.y) ||
                    !std::isfinite(wave.amplitude) || !std::isfinite(wave.wavelength) ||
                    !std::isfinite(wave.steepness) || !std::isfinite(wave.speed) ||
                    !std::isfinite(wave.phaseOffsetRadians))
                {
                    return false;
                }
                const float32 directionLengthSquared =
                    wave.direction.x * wave.direction.x + wave.direction.y * wave.direction.y;
                if (std::abs(directionLengthSquared - 1.0f) > directionTolerance ||
                    wave.amplitude <= 0.0f || wave.amplitude > maximumAmplitude ||
                    wave.wavelength < minimumWavelength || wave.wavelength > maximumWavelength ||
                    wave.steepness < 0.0f || wave.steepness > 1.0f ||
                    wave.speed < 0.0f || wave.speed > maximumSpeed ||
                    wave.phaseOffsetRadians < -pi ||
                    wave.phaseOffsetRadians >= pi)
                {
                    return false;
                }
                aggregateSteepness += wave.steepness;
            }
            // Standard Gerstner horizontal displacement becomes self-
            // intersecting when the aggregate choppiness exceeds one.
            return aggregateSteepness <= 1.0f && contentHash == ComputeContentHash();
        }
    };

    /**
     * @brief Converts authoring values to the sole packet form accepted by the channel.
     *
     * This is intentionally a one-way seal operation: it normalizes active
     * directions, clears all inactive storage and recomputes the exact hash.
     */
    [[nodiscard]] inline bool CanonicalizeWaterGerstnerWaveSet(
        WaterGerstnerWaveSet& value) noexcept
    {
        if (value.waveCount == 0 || value.waveCount > RVX_WATER_MAX_GERSTNER_WAVES)
        {
            return false;
        }
        for (uint32 index = 0; index < value.waveCount; ++index)
        {
            WaterGerstnerWave& wave = value.waves[index];
            if (!std::isfinite(wave.direction.x) || !std::isfinite(wave.direction.y) ||
                !std::isfinite(wave.amplitude) || !std::isfinite(wave.wavelength) ||
                !std::isfinite(wave.steepness) || !std::isfinite(wave.speed) ||
                !std::isfinite(wave.phaseOffsetRadians))
            {
                return false;
            }
            const float32 lengthSquared = wave.direction.x * wave.direction.x +
                                          wave.direction.y * wave.direction.y;
            if (lengthSquared <= std::numeric_limits<float32>::epsilon())
            {
                return false;
            }
            const float32 inverseLength = 1.0f / std::sqrt(lengthSquared);
            wave.direction *= inverseLength;

            // The packet hash is an identity, not a record of incidental
            // floating-point spelling. Canonicalize every active zero to
            // +0.0 and phase to [-pi, pi), including a small snap around
            // exact full-turn authoring constants represented as float32.
            const auto canonicalZero = [](float32 scalar) noexcept
            {
                return scalar == 0.0f ? 0.0f : scalar;
            };
            constexpr float64 pi = 3.14159265358979323846264338327950288;
            constexpr float64 twoPi = pi * 2.0;
            constexpr float64 phaseZeroTolerance = 1.0e-6;
            float64 phase = std::remainder(
                static_cast<float64>(wave.phaseOffsetRadians), twoPi);
            if (phase >= pi)
            {
                phase -= twoPi;
            }
            if (std::abs(phase) <= phaseZeroTolerance)
            {
                phase = 0.0;
            }
            wave.direction.x = canonicalZero(wave.direction.x);
            wave.direction.y = canonicalZero(wave.direction.y);
            wave.amplitude = canonicalZero(wave.amplitude);
            wave.wavelength = canonicalZero(wave.wavelength);
            wave.steepness = canonicalZero(wave.steepness);
            wave.speed = canonicalZero(wave.speed);
            wave.phaseOffsetRadians = canonicalZero(static_cast<float32>(phase));
        }
        for (uint32 index = value.waveCount; index < RVX_WATER_MAX_GERSTNER_WAVES; ++index)
        {
            value.waves[index] = {};
        }
        value.contentHash = value.ComputeContentHash();
        return value.IsStructurallyValid();
    }

    struct WaterRenderSnapshotPayload
    {
        WaterRenderSnapshotPayloadStatus payloadStatus =
            WaterRenderSnapshotPayloadStatus::Disabled;
        /** @brief Generation-qualified RenderScene object identity. */
        uint64 componentId = 0;
        /** @brief Stable authored water-surface source asset. */
        AssetId surfaceAssetId{};
        /** @brief Stable authored water-material source asset. */
        AssetId materialAssetId{};
        Vec3 worldPosition{0.0f, 0.0f, 0.0f};
        AABB worldBounds;

        Vec2 size{0.0f, 0.0f};
        float depth = 0.0f;

        WaterRenderSnapshotSurfaceType surfaceType = WaterRenderSnapshotSurfaceType::Ocean;
        WaterRenderSnapshotSimulationType simulationType = WaterRenderSnapshotSimulationType::Gerstner;
        WaterGerstnerWaveSet gerstnerWaves;

        Vec3 shallowColor{0.0f, 0.0f, 0.0f};
        Vec3 deepColor{0.0f, 0.0f, 0.0f};
        Vec3 foamColor{0.0f, 0.0f, 0.0f};
        Vec3 absorptionCoefficient{0.0f, 0.0f, 0.0f};
        float transparency = 0.0f;
        float reflectionStrength = 0.0f;
        float refractionStrength = 0.0f;
        float roughness = 0.0f;
        float normalScale = 0.0f;
        float foamIntensity = 0.0f;
        float foamCrestThreshold = 0.0f;

        bool reflectionEnabled = false;
        bool refractionEnabled = false;
        bool foamEnabled = false;
        uint64 contentHash = 0;

        [[nodiscard]] uint64 ComputeContentHash() const noexcept
        {
            constexpr uint64 offsetBasis = 14695981039346656037ull;
            constexpr uint64 prime = 1099511628211ull;
            uint64 value = offsetBasis;
            const auto mixUnsigned = [&value](uint64 source, uint32 byteCount)
            {
                for (uint32 byte = 0; byte < byteCount; ++byte)
                {
                    value ^= static_cast<uint8>(source & 0xffU);
                    value *= prime;
                    source >>= 8U;
                }
            };
            const auto mixFloat = [&mixUnsigned](float32 source)
            {
                mixUnsigned(std::bit_cast<uint32>(source), sizeof(uint32));
            };
            const auto mixVec2 = [&mixFloat](const Vec2& source)
            {
                mixFloat(source.x);
                mixFloat(source.y);
            };
            const auto mixVec3 = [&mixFloat](const Vec3& source)
            {
                mixFloat(source.x);
                mixFloat(source.y);
                mixFloat(source.z);
            };

            // `componentId` is intentionally excluded: EcsFrozenSceneBridge
            // replaces it with the generation-qualified RenderScene object
            // identity after the producer sealed this payload. Header/source
            // provenance and the explicit non-zero validation protect that
            // transport rewrite without invalidating the authored packet hash.
            mixUnsigned(static_cast<uint8>(payloadStatus), 1U);
            mixUnsigned(surfaceAssetId.value, sizeof(surfaceAssetId.value));
            mixUnsigned(materialAssetId.value, sizeof(materialAssetId.value));
            mixVec3(worldPosition);
            mixVec3(worldBounds.GetMin());
            mixVec3(worldBounds.GetMax());
            mixVec2(size);
            mixFloat(depth);
            mixUnsigned(static_cast<uint8>(surfaceType), 1U);
            mixUnsigned(static_cast<uint8>(simulationType), 1U);
            mixUnsigned(gerstnerWaves.contentHash, sizeof(gerstnerWaves.contentHash));
            mixVec3(shallowColor);
            mixVec3(deepColor);
            mixVec3(foamColor);
            mixVec3(absorptionCoefficient);
            mixFloat(transparency);
            mixFloat(reflectionStrength);
            mixFloat(refractionStrength);
            mixFloat(roughness);
            mixFloat(normalScale);
            mixFloat(foamIntensity);
            mixFloat(foamCrestThreshold);
            mixUnsigned(reflectionEnabled ? 1U : 0U, 1U);
            mixUnsigned(refractionEnabled ? 1U : 0U, 1U);
            mixUnsigned(foamEnabled ? 1U : 0U, 1U);
            return value;
        }

        [[nodiscard]] bool IsCanonicalDisabled() const noexcept
        {
            return payloadStatus == WaterRenderSnapshotPayloadStatus::Disabled &&
                   // A disabled packet is an absence marker, never a retained
                   // RenderScene object. Its identity must therefore remain
                   // zero until the bridge suppresses it.
                   componentId == 0 &&
                   !surfaceAssetId.IsValid() &&
                   !materialAssetId.IsValid() && worldPosition == Vec3{0.0f} &&
                   !worldBounds.IsValid() && size == Vec2{0.0f} && depth == 0.0f &&
                   surfaceType == WaterRenderSnapshotSurfaceType::Ocean &&
                   simulationType == WaterRenderSnapshotSimulationType::Gerstner &&
                   gerstnerWaves.IsCanonicalDisabled() && shallowColor == Vec3{0.0f} &&
                   deepColor == Vec3{0.0f} && foamColor == Vec3{0.0f} &&
                   absorptionCoefficient == Vec3{0.0f} && transparency == 0.0f &&
                   reflectionStrength == 0.0f && refractionStrength == 0.0f &&
                   roughness == 0.0f && normalScale == 0.0f && foamIntensity == 0.0f &&
                   foamCrestThreshold == 0.0f && !reflectionEnabled &&
                   !refractionEnabled && !foamEnabled && contentHash == 0;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (payloadStatus == WaterRenderSnapshotPayloadStatus::Disabled)
            {
                return IsCanonicalDisabled();
            }
            if (payloadStatus != WaterRenderSnapshotPayloadStatus::Ready ||
                componentId == 0 || !surfaceAssetId.IsValid() || !materialAssetId.IsValid() ||
                !worldBounds.IsValid() || simulationType !=
                    WaterRenderSnapshotSimulationType::Gerstner ||
                !gerstnerWaves.IsStructurallyValid() || contentHash == 0 ||
                !std::isfinite(worldPosition.x) || !std::isfinite(worldPosition.y) ||
                !std::isfinite(worldPosition.z) || !std::isfinite(size.x) ||
                !std::isfinite(size.y) || !std::isfinite(depth) ||
                !std::isfinite(shallowColor.x) || !std::isfinite(shallowColor.y) ||
                !std::isfinite(shallowColor.z) || !std::isfinite(deepColor.x) ||
                !std::isfinite(deepColor.y) || !std::isfinite(deepColor.z) ||
                !std::isfinite(foamColor.x) || !std::isfinite(foamColor.y) ||
                !std::isfinite(foamColor.z) || !std::isfinite(absorptionCoefficient.x) ||
                !std::isfinite(absorptionCoefficient.y) ||
                !std::isfinite(absorptionCoefficient.z) || !std::isfinite(transparency) ||
                !std::isfinite(reflectionStrength) || !std::isfinite(refractionStrength) ||
                !std::isfinite(roughness) || !std::isfinite(normalScale) ||
                !std::isfinite(foamIntensity) || !std::isfinite(foamCrestThreshold))
            {
                return false;
            }

            const Vec3 boundsMin = worldBounds.GetMin();
            const Vec3 boundsMax = worldBounds.GetMax();
            const auto finiteBounds = [](const Vec3& value) noexcept
            {
                return std::isfinite(value.x) && std::isfinite(value.y) &&
                       std::isfinite(value.z);
            };
            float32 verticalEnvelope = 0.0f;
            float32 horizontalEnvelope = 0.0f;
            for (uint32 index = 0; index < gerstnerWaves.waveCount; ++index)
            {
                const WaterGerstnerWave& wave = gerstnerWaves.waves[index];
                verticalEnvelope += wave.amplitude;
                horizontalEnvelope += wave.steepness /
                    (RVX_WATER_GERSTNER_TWO_PI_FLOAT / wave.wavelength);
            }
            constexpr float32 boundsTolerance = 0.001f;
            const float32 halfWidth = size.x * 0.5f + horizontalEnvelope;
            const float32 halfLength = size.y * 0.5f + horizontalEnvelope;
            if (!finiteBounds(boundsMin) || !finiteBounds(boundsMax) ||
                size.x <= 0.0f || size.y <= 0.0f ||
                size.x > RVX_WATER_MAX_SURFACE_EXTENT_METERS ||
                size.y > RVX_WATER_MAX_SURFACE_EXTENT_METERS || depth < 0.0f ||
                depth > RVX_WATER_MAX_DEPTH_METERS ||
                transparency < 0.0f || transparency > 1.0f ||
                reflectionStrength < 0.0f || reflectionStrength > 1.0f ||
                refractionStrength < 0.0f || refractionStrength > 1.0f ||
                roughness < 0.02f || roughness > 1.0f || normalScale < 0.0f ||
                normalScale > 8.0f || foamIntensity < 0.0f || foamIntensity > 8.0f ||
                foamCrestThreshold < 0.0f || foamCrestThreshold > 1.0f ||
                shallowColor.x < 0.0f || shallowColor.y < 0.0f || shallowColor.z < 0.0f ||
                deepColor.x < 0.0f || deepColor.y < 0.0f || deepColor.z < 0.0f ||
                foamColor.x < 0.0f || foamColor.y < 0.0f || foamColor.z < 0.0f ||
                absorptionCoefficient.x < 0.0f || absorptionCoefficient.y < 0.0f ||
                absorptionCoefficient.z < 0.0f || shallowColor.x > 64.0f ||
                shallowColor.y > 64.0f || shallowColor.z > 64.0f || deepColor.x > 64.0f ||
                deepColor.y > 64.0f || deepColor.z > 64.0f || foamColor.x > 64.0f ||
                foamColor.y > 64.0f || foamColor.z > 64.0f ||
                absorptionCoefficient.x > 64.0f || absorptionCoefficient.y > 64.0f ||
                absorptionCoefficient.z > 64.0f ||
                boundsMin.x > worldPosition.x - halfWidth + boundsTolerance ||
                boundsMax.x < worldPosition.x + halfWidth - boundsTolerance ||
                boundsMin.z > worldPosition.z - halfLength + boundsTolerance ||
                boundsMax.z < worldPosition.z + halfLength - boundsTolerance ||
                boundsMin.y > worldPosition.y - depth - verticalEnvelope + boundsTolerance ||
                boundsMax.y < worldPosition.y + verticalEnvelope - boundsTolerance)
            {
                return false;
            }
            return contentHash == ComputeContentHash();
        }
    };

    /** @brief Water payload plus versioned producer/source provenance. */
    struct WaterRenderSnapshotItem final
        : RenderFeatureChannelEnvelope<
              WaterRenderFeatureChannelDescriptor,
              WaterRenderSnapshotPayload>
    {
    };

    struct WaterRenderSnapshotMetadata
    {
        uint32 schemaVersion = RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION;
        uint64 sequence = 0;
        WaterRenderSnapshotStatus status = WaterRenderSnapshotStatus::Empty;
        bool complete = false;
        size_t itemCount = 0;
    };

    struct WaterRenderSnapshot
    {
        WaterRenderSnapshotMetadata metadata;
        std::vector<WaterRenderSnapshotItem> items;

        void Clear()
        {
            metadata = {};
            items.clear();
        }

        void BeginBuild(uint64 sequence)
        {
            Clear();
            metadata.sequence = sequence;
            metadata.status = WaterRenderSnapshotStatus::Incomplete;
        }

        void MarkComplete()
        {
            metadata.status = WaterRenderSnapshotStatus::Complete;
            metadata.complete = true;
            metadata.itemCount = items.size();
        }

        void MarkIncomplete()
        {
            items.clear();
            metadata.status = WaterRenderSnapshotStatus::Incomplete;
            metadata.complete = false;
            metadata.itemCount = 0;
        }

        WaterRenderSnapshotMetadata GetMetadata() const
        {
            WaterRenderSnapshotMetadata snapshotMetadata = metadata;
            snapshotMetadata.itemCount = items.size();
            return snapshotMetadata;
        }
    };

} // namespace RVX
