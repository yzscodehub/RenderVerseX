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
    };

    /** @brief Schema for the immutable CPU particle packet carried to Render. */
    inline constexpr uint32 RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION = 1;

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
                   cpuPacket.contentHash == 0 && cpuPacket.particles.empty();
        }

        /** @brief Cheap sealed-packet shape check for intermediate boundaries. */
        [[nodiscard]] bool IsCpuPacketReady() const noexcept
        {
            return payloadStatus == ParticleRenderSnapshotPayloadStatus::CpuPacketReady &&
                   simulationBackend == ParticleRenderSnapshotSimulationBackend::CPU &&
                   cpuPacket.schemaVersion == RVX_PARTICLE_CPU_PACKET_SCHEMA_VERSION &&
                   aliveParticleCount == cpuPacket.particles.size() &&
                   cpuPacket.particles.size() <= maxParticleCount;
        }

        /** @brief Full content validation for the producer and final Render consumer. */
        [[nodiscard]] bool HasValidCpuPacket() const noexcept
        {
            return IsCpuPacketReady() &&
                   cpuPacket.IsStructurallyValid(maxParticleCount);
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
