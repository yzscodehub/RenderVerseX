#pragma once

/**
 * @file ParticleRenderSnapshot.h
 * @brief Render-facing particle data contract without Feature, Render, or RHI types.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION = 1;

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

    struct ParticleRenderSnapshotItem
    {
        uint64 instanceId = 0;
        uint64 systemId = 0;
        std::string systemName;

        Mat4 worldMatrix = Mat4Identity();
        AABB worldBounds;
        Vec3 position{0.0f, 0.0f, 0.0f};

        ParticleRenderSnapshotMode renderMode = ParticleRenderSnapshotMode::Billboard;
        ParticleRenderSnapshotBlendMode blendMode = ParticleRenderSnapshotBlendMode::AlphaBlend;
        ParticleRenderSnapshotSimulationBackend simulationBackend = ParticleRenderSnapshotSimulationBackend::None;

        uint32 aliveParticleCount = 0;
        uint32 maxParticleCount = 0;
        uint32 lodLevel = 0;
        float normalizedTime = 0.0f;

        bool visible = true;
        bool simulationSupported = false;
        bool softParticlesEnabled = false;
        float softParticleFadeDistance = 0.0f;
        std::string unsupportedReason;
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
            metadata.status = ParticleRenderSnapshotStatus::Complete;
            metadata.complete = true;
            metadata.itemCount = items.size();
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
            snapshotMetadata.skippedInstanceCount = static_cast<uint32>(skippedReasons.size());
            return snapshotMetadata;
        }
    };

} // namespace RVX
