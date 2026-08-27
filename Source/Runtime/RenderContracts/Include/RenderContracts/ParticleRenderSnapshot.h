#pragma once

/**
 * @file ParticleRenderSnapshot.h
 * @brief Render-facing particle data contract without Feature, Render, or RHI types.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/FeatureChannelContracts.h"
#include "RenderContracts/RenderIdentity.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace RVX
{
    enum class ParticleRenderSnapshotStatus : uint8
    {
        Empty = 0,
        Complete,
        Incomplete
    };

    enum class ParticleRenderSnapshotMode : uint8
    {
        Billboard = 0,
        StretchedBillboard,
        HorizontalBillboard,
        VerticalBillboard,
        Mesh,
        Trail
    };

    enum class ParticleRenderSnapshotBlendMode : uint8
    {
        Additive = 0,
        AlphaBlend,
        Multiply,
        Premultiplied
    };

    enum class ParticleRenderSnapshotSimulationBackend : uint8
    {
        None = 0,
        CPU,
        GPU,
        External
    };

    enum class ParticleRenderSnapshotPayloadStatus : uint8
    {
        MetadataOnly = 0,
        /** @brief CPU-owned immutable packet ready for Render-owned upload. */
        CpuPacketReady
    };

    /** @brief Render-facing soft-depth policy. Render resolves optional inputs explicitly. */
    enum class ParticleRenderSoftDepthMode : uint8
    {
        Disabled = 0,
        Optional,
        Required
    };

    inline constexpr uint32 RVX_PARTICLE_RENDER_EFFECT_SCHEMA_VERSION = 1;

    /**
     * @brief Immutable atlas identity and sampling layout for one particle effect.
     *
     * `texture` is populated only by the update-owner resource-publication
     * view. It is never synthesized by Particle from a resource pointer.
     */
    struct ParticleRenderFlipbookDescriptor
    {
        AssetId textureAssetId{};
        RenderResourceHandle texture{};
        uint64 committedContentRevision = 0;
        UVec2 tiles{1U, 1U};
        uint32 firstFrame = 0;
        uint32 frameCount = 1;
        float framesPerSecond = 0.0f;
        Vec2 uvOffset{0.0f, 0.0f};
        Vec2 uvScale{1.0f, 1.0f};
        /** @brief A missing/stale publication must prevent packet publication. */
        bool required = false;

        [[nodiscard]] bool IsBound() const noexcept
        {
            return textureAssetId.IsValid() && texture.IsValid() &&
                   committedContentRevision != 0;
        }

        [[nodiscard]] bool IsAtlasRangeValid() const noexcept
        {
            if (tiles.x == 0U || tiles.y == 0U || tiles.x > 65535U ||
                tiles.y > 65535U || frameCount == 0U ||
                !std::isfinite(framesPerSecond) || framesPerSecond < 0.0f ||
                !std::isfinite(uvOffset.x) || !std::isfinite(uvOffset.y) ||
                !std::isfinite(uvScale.x) || !std::isfinite(uvScale.y) ||
                uvOffset.x < 0.0f || uvOffset.y < 0.0f ||
                uvScale.x <= 0.0f || uvScale.y <= 0.0f ||
                uvOffset.x + uvScale.x > 1.0f ||
                uvOffset.y + uvScale.y > 1.0f)
            {
                return false;
            }

            const uint64 totalTiles = static_cast<uint64>(tiles.x) *
                                      static_cast<uint64>(tiles.y);
            const uint64 endFrame = static_cast<uint64>(firstFrame) +
                                    static_cast<uint64>(frameCount);
            return totalTiles <= std::numeric_limits<uint32>::max() &&
                   static_cast<uint64>(firstFrame) < totalTiles &&
                   endFrame <= totalTiles;
        }

        [[nodiscard]] bool IsCanonicalUntextured() const noexcept
        {
            return !textureAssetId.IsValid() && !texture.IsValid() &&
                   committedContentRevision == 0 && tiles == UVec2{1U, 1U} &&
                   firstFrame == 0 && frameCount == 1 &&
                   framesPerSecond == 0.0f && uvOffset == Vec2{0.0f, 0.0f} &&
                   uvScale == Vec2{1.0f, 1.0f} && !required;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!IsAtlasRangeValid())
            {
                return false;
            }

            if (required && !textureAssetId.IsValid())
            {
                return false;
            }

            if (texture.IsValid() != (committedContentRevision != 0))
            {
                return false;
            }

            return !texture.IsValid() || textureAssetId.IsValid();
        }
    };

    /** @brief One bounded, no-shadow light policy carried by an effect descriptor. */
    struct ParticleRenderLightHintSettings
    {
        bool enabled = false;
        Vec3 color{0.0f, 0.0f, 0.0f};
        float intensityCandela = 0.0f;
        float rangeMeters = 0.0f;
        float flickerFrequencyHz = 0.0f;
    };

    /**
     * @brief Immutable, backend-neutral rendering parameters for one effect.
     *
     * The descriptor is value-owned by every snapshot item. Its content hash
     * covers the exact atlas generation so a packet cannot be consumed against
     * a replacement resource generation.
     */
    struct ParticleRenderEffectDescriptor
    {
        uint32 schemaVersion = RVX_PARTICLE_RENDER_EFFECT_SCHEMA_VERSION;
        ParticleRenderFlipbookDescriptor flipbook;
        ParticleRenderSoftDepthMode softDepthMode =
            ParticleRenderSoftDepthMode::Disabled;
        float softDepthFadeMeters = 0.0f;
        float softDepthContrast = 1.0f;
        float distortionStrengthPixels = 0.0f;
        float reactiveContribution = 0.0f;
        ParticleRenderLightHintSettings lightHint;
        uint64 contentHash = 0;

        [[nodiscard]] uint64 ComputeContentHash() const noexcept
        {
            constexpr uint64 offsetBasis = 14695981039346656037ull;
            uint64 value = offsetBasis;
            const auto mixByte = [&value](uint8 byte)
            {
                value ^= byte;
                value *= 1099511628211ull;
            };
            const auto mixUnsigned = [&mixByte](uint64 source, uint32 byteCount)
            {
                for (uint32 byte = 0; byte < byteCount; ++byte)
                {
                    mixByte(static_cast<uint8>(source & 0xffU));
                    source >>= 8U;
                }
            };
            const auto mixFloat = [&mixUnsigned](float source)
            {
                mixUnsigned(std::bit_cast<uint32>(source), sizeof(uint32));
            };

            mixUnsigned(schemaVersion, sizeof(schemaVersion));
            mixUnsigned(flipbook.textureAssetId.value,
                        sizeof(flipbook.textureAssetId.value));
            mixUnsigned(flipbook.texture.slot, sizeof(flipbook.texture.slot));
            mixUnsigned(flipbook.texture.generation,
                        sizeof(flipbook.texture.generation));
            mixUnsigned(flipbook.committedContentRevision,
                        sizeof(flipbook.committedContentRevision));
            mixUnsigned(flipbook.tiles.x, sizeof(flipbook.tiles.x));
            mixUnsigned(flipbook.tiles.y, sizeof(flipbook.tiles.y));
            mixUnsigned(flipbook.firstFrame, sizeof(flipbook.firstFrame));
            mixUnsigned(flipbook.frameCount, sizeof(flipbook.frameCount));
            mixFloat(flipbook.framesPerSecond);
            mixFloat(flipbook.uvOffset.x);
            mixFloat(flipbook.uvOffset.y);
            mixFloat(flipbook.uvScale.x);
            mixFloat(flipbook.uvScale.y);
            mixUnsigned(flipbook.required ? 1U : 0U, 1U);
            mixUnsigned(static_cast<uint8>(softDepthMode), 1U);
            mixFloat(softDepthFadeMeters);
            mixFloat(softDepthContrast);
            mixFloat(distortionStrengthPixels);
            mixFloat(reactiveContribution);
            mixUnsigned(lightHint.enabled ? 1U : 0U, 1U);
            mixFloat(lightHint.color.x);
            mixFloat(lightHint.color.y);
            mixFloat(lightHint.color.z);
            mixFloat(lightHint.intensityCandela);
            mixFloat(lightHint.rangeMeters);
            mixFloat(lightHint.flickerFrequencyHz);
            return value;
        }

        [[nodiscard]] bool IsCanonicalDisabled() const noexcept
        {
            return schemaVersion == RVX_PARTICLE_RENDER_EFFECT_SCHEMA_VERSION &&
                   flipbook.IsCanonicalUntextured() &&
                   softDepthMode == ParticleRenderSoftDepthMode::Disabled &&
                   softDepthFadeMeters == 0.0f && softDepthContrast == 1.0f &&
                   distortionStrengthPixels == 0.0f &&
                   reactiveContribution == 0.0f && !lightHint.enabled &&
                   lightHint.color == Vec3{0.0f, 0.0f, 0.0f} &&
                   lightHint.intensityCandela == 0.0f &&
                   lightHint.rangeMeters == 0.0f &&
                   lightHint.flickerFrequencyHz == 0.0f && contentHash == 0;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (schemaVersion != RVX_PARTICLE_RENDER_EFFECT_SCHEMA_VERSION ||
                !flipbook.IsStructurallyValid() ||
                !std::isfinite(softDepthFadeMeters) ||
                !std::isfinite(softDepthContrast) ||
                !std::isfinite(distortionStrengthPixels) ||
                !std::isfinite(reactiveContribution) ||
                !std::isfinite(lightHint.color.x) ||
                !std::isfinite(lightHint.color.y) ||
                !std::isfinite(lightHint.color.z) ||
                !std::isfinite(lightHint.intensityCandela) ||
                !std::isfinite(lightHint.rangeMeters) ||
                !std::isfinite(lightHint.flickerFrequencyHz) ||
                distortionStrengthPixels < 0.0f ||
                distortionStrengthPixels > 64.0f || reactiveContribution < 0.0f ||
                reactiveContribution > 1.0f)
            {
                return false;
            }

            if (softDepthMode == ParticleRenderSoftDepthMode::Disabled)
            {
                if (softDepthFadeMeters != 0.0f || softDepthContrast != 1.0f)
                {
                    return false;
                }
            }
            else if (softDepthMode != ParticleRenderSoftDepthMode::Optional &&
                     softDepthMode != ParticleRenderSoftDepthMode::Required)
            {
                return false;
            }
            else if (softDepthFadeMeters <= 0.0f ||
                     softDepthFadeMeters > 1000.0f || softDepthContrast <= 0.0f ||
                     softDepthContrast > 16.0f)
            {
                return false;
            }

            if (lightHint.enabled)
            {
                if (lightHint.color.x < 0.0f || lightHint.color.x > 65504.0f ||
                    lightHint.color.y < 0.0f || lightHint.color.y > 65504.0f ||
                    lightHint.color.z < 0.0f || lightHint.color.z > 65504.0f ||
                    lightHint.intensityCandela <= 0.0f ||
                    lightHint.intensityCandela > 100000.0f ||
                    lightHint.rangeMeters <= 0.0f || lightHint.rangeMeters > 1000.0f ||
                    lightHint.flickerFrequencyHz < 0.0f ||
                    lightHint.flickerFrequencyHz > 10.0f)
                {
                    return false;
                }
            }
            else if (lightHint.color != Vec3{0.0f, 0.0f, 0.0f} ||
                     lightHint.intensityCandela != 0.0f ||
                     lightHint.rangeMeters != 0.0f ||
                     lightHint.flickerFrequencyHz != 0.0f)
            {
                return false;
            }

            return IsCanonicalDisabled() || contentHash == ComputeContentHash();
        }
    };

    /** @brief One deterministic, bounded light hint for a CPU particle packet. */
    struct ParticleRenderLightHint
    {
        bool enabled = false;
        /** @brief Stable per-system identity used for deterministic light admission. */
        uint64 stableLightId = 0;
        /** @brief Fixed simulation tick at which this low-frequency sample began. */
        uint64 sampleTick = 0;
        uint32 sourceParticleId = RVX_INVALID_INDEX;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 color{0.0f, 0.0f, 0.0f};
        float intensityCandela = 0.0f;
        float rangeMeters = 0.0f;
        float modulation = 0.0f;

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            const bool finite = std::isfinite(position.x) && std::isfinite(position.y) &&
                                std::isfinite(position.z) && std::isfinite(color.x) &&
                                std::isfinite(color.y) && std::isfinite(color.z) &&
                                std::isfinite(intensityCandela) &&
                                std::isfinite(rangeMeters) && std::isfinite(modulation);
            if (!finite)
            {
                return false;
            }
            if (!enabled)
            {
                return sourceParticleId == RVX_INVALID_INDEX &&
                       stableLightId == 0 && sampleTick == 0 &&
                       position == Vec3{0.0f, 0.0f, 0.0f} &&
                       color == Vec3{0.0f, 0.0f, 0.0f} &&
                       intensityCandela == 0.0f && rangeMeters == 0.0f &&
                       modulation == 0.0f;
            }
            return stableLightId != 0 && sourceParticleId != RVX_INVALID_INDEX &&
                   color.x >= 0.0f && color.x <= 65504.0f &&
                   color.y >= 0.0f && color.y <= 65504.0f &&
                   color.z >= 0.0f && color.z <= 65504.0f &&
                   intensityCandela > 0.0f &&
                   intensityCandela <= 100000.0f && rangeMeters > 0.0f &&
                   rangeMeters <= 1000.0f && modulation >= 0.0f && modulation <= 1.0f;
        }
    };

    struct ParticleRenderParticleData
    {
        /**
         * @brief Stable CPU particle-slot identity.
         *
         * It is immutable for the packet lifetime and is the deterministic
         * tie-breaker for render-owned per-view sorting.  It is not a render
         * resource handle and it is never generated from a pointer.
         */
        uint32 stableParticleId = RVX_INVALID_INDEX;
        /**
         * @brief Canonical world-space position produced after the emitter
         * transform has been applied. Render must not multiply it by the
         * system `worldMatrix` again.
         */
        Vec3 position{0.0f, 0.0f, 0.0f};
        float lifetime = 0.0f;
        /** @brief Canonical world-space velocity paired with `position`. */
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        float age = 0.0f;
        Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        Vec2 size{1.0f, 1.0f};
        float rotation = 0.0f;
        uint32 flags = 0;
        /** @brief Final atlas frame resolved by the deterministic CPU policy. */
        uint32 flipbookFrameIndex = 0;
    };

    /** @brief Schema for the immutable CPU particle packet carried to Render. */
    inline constexpr uint32 RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION = 2;

    /**
     * @brief One immutable CPU particle payload captured for one simulation tick.
     *
     * Producers seal this value before publication. Each snapshot boundary
     * owns its packet value, so later source mutations cannot alias or alter
     * already-frozen Scene or Render values.
     */
    struct ParticleCpuPacket final
    {
        uint32 schemaVersion = RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION;
        uint64 simulationTick = 0;
        /** @brief Source-configured seed; zero is a valid explicit value. */
        uint32 deterministicSeed = 0;
        /** @brief Stable owner identity used with `deterministicSeed`. */
        uint64 deterministicInstanceKey = 0;
        /** @brief Exact CPU simulator RNG seed after key derivation. */
        uint32 effectiveSimulationSeed = 0;
        /** @brief Exact caller-owned fixed delta consumed by this packet's tick. */
        float fixedDeltaSeconds = 0.0f;
        /** @brief Must match the immutable payload effect descriptor hash. */
        uint64 effectDescriptorHash = 0;
        /** @brief At most one deterministic, bounded, no-shadow light hint. */
        ParticleRenderLightHint lightHint;
        uint64 contentHash = 0;
        std::vector<ParticleRenderParticleData> particles;

        /** @brief Stable FNV-1a content identity with scalar fields encoded explicitly. */
        [[nodiscard]] uint64 ComputeContentHash() const noexcept
        {
            constexpr uint64 offsetBasis = 14695981039346656037ull;
            uint64 value = offsetBasis;
            const auto mixByte = [&value](uint8 byte)
            {
                value ^= byte;
                value *= 1099511628211ull;
            };
            const auto mixUnsigned = [&mixByte](uint64 source, uint32 byteCount)
            {
                for (uint32 byte = 0; byte < byteCount; ++byte)
                {
                    mixByte(static_cast<uint8>(source & 0xffu));
                    source >>= 8u;
                }
            };
            const auto mixFloat = [&mixUnsigned](float source)
            {
                mixUnsigned(std::bit_cast<uint32>(source), sizeof(uint32));
            };

            mixUnsigned(schemaVersion, sizeof(schemaVersion));
            mixUnsigned(simulationTick, sizeof(simulationTick));
            mixUnsigned(deterministicSeed, sizeof(deterministicSeed));
            mixUnsigned(deterministicInstanceKey, sizeof(deterministicInstanceKey));
            mixUnsigned(effectiveSimulationSeed, sizeof(effectiveSimulationSeed));
            mixFloat(fixedDeltaSeconds);
            mixUnsigned(effectDescriptorHash, sizeof(effectDescriptorHash));
            mixUnsigned(lightHint.enabled ? 1U : 0U, 1U);
            mixUnsigned(lightHint.stableLightId, sizeof(lightHint.stableLightId));
            mixUnsigned(lightHint.sampleTick, sizeof(lightHint.sampleTick));
            mixUnsigned(lightHint.sourceParticleId, sizeof(lightHint.sourceParticleId));
            mixFloat(lightHint.position.x);
            mixFloat(lightHint.position.y);
            mixFloat(lightHint.position.z);
            mixFloat(lightHint.color.x);
            mixFloat(lightHint.color.y);
            mixFloat(lightHint.color.z);
            mixFloat(lightHint.intensityCandela);
            mixFloat(lightHint.rangeMeters);
            mixFloat(lightHint.modulation);
            mixUnsigned(static_cast<uint64>(particles.size()), sizeof(uint64));
            for (const ParticleRenderParticleData& particle : particles)
            {
                mixUnsigned(particle.stableParticleId, sizeof(particle.stableParticleId));
                mixFloat(particle.position.x);
                mixFloat(particle.position.y);
                mixFloat(particle.position.z);
                mixFloat(particle.lifetime);
                mixFloat(particle.velocity.x);
                mixFloat(particle.velocity.y);
                mixFloat(particle.velocity.z);
                mixFloat(particle.age);
                mixFloat(particle.color.x);
                mixFloat(particle.color.y);
                mixFloat(particle.color.z);
                mixFloat(particle.color.w);
                mixFloat(particle.size.x);
                mixFloat(particle.size.y);
                mixFloat(particle.rotation);
                mixUnsigned(particle.flags, sizeof(particle.flags));
                mixUnsigned(particle.flipbookFrameIndex,
                            sizeof(particle.flipbookFrameIndex));
            }
            return value;
        }

        /**
         * @brief Validates every serialised field without reading padding bytes.
         *
         * Particle data is emitted in ascending CPU-slot order, so strict
         * monotonic IDs prove there are no duplicate tie-break values without
         * allocating a second validation container.
         */
        [[nodiscard]] bool IsStructurallyValid(uint32 maxParticleCount) const noexcept
        {
            if (schemaVersion != RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION ||
                deterministicInstanceKey == 0 ||
                particles.size() > maxParticleCount ||
                !std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds < 0.0f ||
                (lightHint.enabled && lightHint.sampleTick > simulationTick) ||
                !lightHint.IsStructurallyValid() ||
                contentHash != ComputeContentHash())
            {
                return false;
            }

            uint32 previousStableId = RVX_INVALID_INDEX;
            for (const ParticleRenderParticleData& particle : particles)
            {
                const bool finite = std::isfinite(particle.position.x) &&
                                    std::isfinite(particle.position.y) &&
                                    std::isfinite(particle.position.z) &&
                                    std::isfinite(particle.lifetime) &&
                                    std::isfinite(particle.velocity.x) &&
                                    std::isfinite(particle.velocity.y) &&
                                    std::isfinite(particle.velocity.z) &&
                                    std::isfinite(particle.age) &&
                                    std::isfinite(particle.color.x) &&
                                    std::isfinite(particle.color.y) &&
                                    std::isfinite(particle.color.z) &&
                                    std::isfinite(particle.color.w) &&
                                    std::isfinite(particle.size.x) &&
                                    std::isfinite(particle.size.y) &&
                                    std::isfinite(particle.rotation);
                if (!finite || particle.stableParticleId >= maxParticleCount ||
                    particle.stableParticleId == RVX_INVALID_INDEX ||
                    (previousStableId != RVX_INVALID_INDEX &&
                     particle.stableParticleId <= previousStableId) ||
                    particle.lifetime <= 0.0f || particle.age < 0.0f ||
                    particle.age > particle.lifetime || particle.size.x < 0.0f ||
                    particle.size.y < 0.0f)
                {
                    return false;
                }
                previousStableId = particle.stableParticleId;
            }
            return true;
        }
    };

    struct ParticleRenderSnapshotPayload
    {
        /** @brief Generation-qualified RenderScene object identity. */
        uint64 instanceId = 0;
        /** @brief Generation-qualified RenderScene system identity. */
        uint64 systemId = 0;
        /** @brief Stable source asset identity; never replaced by a render identity. */
        AssetId systemAssetId{};
        std::string systemName;

        /**
         * @brief System transform retained for provenance, bounds and
         * diagnostics only; CPU packet positions are already world-space.
         */
        Mat4 worldMatrix = Mat4Identity();
        AABB worldBounds;
        Vec3 position{0.0f, 0.0f, 0.0f};

        ParticleRenderSnapshotMode renderMode = ParticleRenderSnapshotMode::Billboard;
        ParticleRenderSnapshotBlendMode blendMode = ParticleRenderSnapshotBlendMode::AlphaBlend;
        ParticleRenderSnapshotSimulationBackend simulationBackend = ParticleRenderSnapshotSimulationBackend::None;
        ParticleRenderSnapshotPayloadStatus payloadStatus = ParticleRenderSnapshotPayloadStatus::MetadataOnly;

        /** @brief Immutable effect state resolved from the owning particle asset. */
        ParticleRenderEffectDescriptor effectDescriptor;

        uint32 aliveParticleCount = 0;
        uint32 maxParticleCount = 0;
        uint32 lodLevel = 0;
        float normalizedTime = 0.0f;

        bool visible = true;
        bool simulationSupported = false;
        bool sortingSupported = false;
        bool softParticlesEnabled = false;
        float softParticleFadeDistance = 0.0f;
        /**
         * @brief Sealed CPU packet owned by this payload value.
         *
         * It is meaningful only when payloadStatus is CpuPacketReady. A
         * metadata-only payload must retain the canonical default packet.
         */
        ParticleCpuPacket cpuPacket{};
        std::string unsupportedReason;
        std::string renderPayloadReason;
        std::string sortingReason;

        [[nodiscard]] size_t GetParticleCount() const noexcept
        {
            return payloadStatus == ParticleRenderSnapshotPayloadStatus::CpuPacketReady
                       ? cpuPacket.particles.size()
                       : 0;
        }

        [[nodiscard]] const std::vector<ParticleRenderParticleData>&
        GetParticles() const noexcept
        {
            static const std::vector<ParticleRenderParticleData> empty;
            return payloadStatus == ParticleRenderSnapshotPayloadStatus::CpuPacketReady
                       ? cpuPacket.particles
                       : empty;
        }

        /** @brief Checks the canonical no-packet state for metadata-only payloads. */
        [[nodiscard]] bool HasNoCpuPacket() const noexcept
        {
            return cpuPacket.schemaVersion == RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION &&
                   cpuPacket.simulationTick == 0 &&
                   cpuPacket.deterministicSeed == 0 &&
                   cpuPacket.deterministicInstanceKey == 0 &&
                   cpuPacket.effectiveSimulationSeed == 0 &&
                   cpuPacket.fixedDeltaSeconds == 0.0f &&
                   cpuPacket.effectDescriptorHash == 0 &&
                   cpuPacket.lightHint.IsStructurallyValid() &&
                   cpuPacket.contentHash == 0 && cpuPacket.particles.empty();
        }

        /** @brief Cheap sealed-packet shape check for intermediate boundaries. */
        [[nodiscard]] bool IsCpuPacketReady() const noexcept
        {
            return payloadStatus == ParticleRenderSnapshotPayloadStatus::CpuPacketReady &&
                   simulationBackend == ParticleRenderSnapshotSimulationBackend::CPU &&
                   cpuPacket.schemaVersion == RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION &&
                   effectDescriptor.IsStructurallyValid() &&
                   cpuPacket.effectDescriptorHash == effectDescriptor.contentHash &&
                   aliveParticleCount == cpuPacket.particles.size() &&
                   cpuPacket.particles.size() <= maxParticleCount;
        }

        /** @brief Full content validation for the producer and final Render consumer. */
        [[nodiscard]] bool HasValidCpuPacket() const noexcept
        {
            return IsCpuPacketReady() &&
                   cpuPacket.IsStructurallyValid(maxParticleCount) &&
                   (effectDescriptor.flipbook.IsCanonicalUntextured() ||
                    (effectDescriptor.flipbook.IsBound() &&
                     std::all_of(cpuPacket.particles.begin(),
                                 cpuPacket.particles.end(),
                                 [this](const ParticleRenderParticleData& particle)
                                 {
                                     const uint64 first =
                                         effectDescriptor.flipbook.firstFrame;
                                     const uint64 end = first +
                                         effectDescriptor.flipbook.frameCount;
                                     return static_cast<uint64>(particle.flipbookFrameIndex) >=
                                                first &&
                                            static_cast<uint64>(particle.flipbookFrameIndex) < end;
                                 })));
        }
    };

    /** @brief Particle payload plus versioned producer/source provenance. */
    struct ParticleRenderSnapshotItem final
        : RenderFeatureChannelEnvelope<
              ParticleRenderFeatureChannelDescriptor,
              ParticleRenderSnapshotPayload>
    {
    };

    struct ParticleRenderSnapshotMetadata
    {
        uint32 schemaVersion = RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
        uint64 sequence = 0;
        ParticleRenderSnapshotStatus status = ParticleRenderSnapshotStatus::Empty;
        bool complete = false;
        size_t itemCount = 0;
        uint32 totalAliveParticles = 0;
        uint32 skippedInstanceCount = 0;
    };

    struct ParticleRenderSnapshot
    {
        ParticleRenderSnapshotMetadata metadata;
        std::vector<ParticleRenderSnapshotItem> items;
        std::vector<std::string> skippedReasons;

        void Clear()
        {
            metadata = {};
            items.clear();
            skippedReasons.clear();
        }

        void BeginBuild(uint64 sequence)
        {
            Clear();
            metadata.sequence = sequence;
            metadata.status = ParticleRenderSnapshotStatus::Incomplete;
        }

        void MarkComplete()
        {
            uint64 totalAliveParticles = 0;
            for (const ParticleRenderSnapshotItem& item : items)
            {
                const bool ready = item.payloadStatus ==
                    ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
                if (item.aliveParticleCount > item.maxParticleCount ||
                    (ready && !item.IsCpuPacketReady()) ||
                    (!ready && !item.HasNoCpuPacket()))
                {
                    skippedReasons.push_back(
                        "Particle snapshot contains an invalid packet/status relationship.");
                    MarkIncomplete();
                    return;
                }
                totalAliveParticles += item.aliveParticleCount;
                if (totalAliveParticles > std::numeric_limits<uint32>::max())
                {
                    skippedReasons.push_back(
                        "Particle snapshot alive-particle count overflowed uint32 diagnostics.");
                    MarkIncomplete();
                    return;
                }
            }
            metadata.status = ParticleRenderSnapshotStatus::Complete;
            metadata.complete = true;
            metadata.itemCount = items.size();
            metadata.totalAliveParticles = static_cast<uint32>(totalAliveParticles);
            metadata.skippedInstanceCount = static_cast<uint32>(skippedReasons.size());
        }

        void MarkIncomplete()
        {
            items.clear();
            metadata.status = ParticleRenderSnapshotStatus::Incomplete;
            metadata.complete = false;
            metadata.itemCount = 0;
            metadata.totalAliveParticles = 0;
            metadata.skippedInstanceCount = static_cast<uint32>(skippedReasons.size());
        }

        ParticleRenderSnapshotMetadata GetMetadata() const
        {
            ParticleRenderSnapshotMetadata snapshotMetadata = metadata;
            snapshotMetadata.itemCount = items.size();
            uint64 totalAliveParticles = 0;
            for (const ParticleRenderSnapshotItem& item : items)
            {
                const bool ready = item.payloadStatus ==
                    ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
                if (item.aliveParticleCount > item.maxParticleCount ||
                    (ready && !item.IsCpuPacketReady()) ||
                    (!ready && !item.HasNoCpuPacket()))
                {
                    snapshotMetadata.status = ParticleRenderSnapshotStatus::Incomplete;
                    snapshotMetadata.complete = false;
                    snapshotMetadata.totalAliveParticles = 0;
                    snapshotMetadata.skippedInstanceCount =
                        static_cast<uint32>(skippedReasons.size());
                    return snapshotMetadata;
                }
                totalAliveParticles += item.aliveParticleCount;
                if (totalAliveParticles > std::numeric_limits<uint32>::max())
                {
                    snapshotMetadata.status = ParticleRenderSnapshotStatus::Incomplete;
                    snapshotMetadata.complete = false;
                    snapshotMetadata.totalAliveParticles = 0;
                    snapshotMetadata.skippedInstanceCount =
                        static_cast<uint32>(skippedReasons.size());
                    return snapshotMetadata;
                }
            }
            snapshotMetadata.totalAliveParticles =
                static_cast<uint32>(totalAliveParticles);
            snapshotMetadata.skippedInstanceCount = static_cast<uint32>(skippedReasons.size());
            return snapshotMetadata;
        }
    };

} // namespace RVX
