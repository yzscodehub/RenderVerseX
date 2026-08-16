#pragma once

/**
 * @file FeatureRenderSnapshot.h
 * @brief Aggregated feature snapshots extracted from Scene/World for Render.
 */

#include "Core/Types.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include "RenderContracts/TerrainRenderSnapshot.h"
#include "RenderContracts/WaterRenderSnapshot.h"

#include <cstddef>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION = 2;

    enum class RenderFeatureSnapshotStatus : uint8
    {
        Empty = 0,
        Complete,
        Incomplete
    };

    struct RenderFeatureSnapshotMetadata
    {
        uint32 schemaVersion = RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION;
        uint64 sequence = 0;
        RenderFeatureSnapshotStatus status = RenderFeatureSnapshotStatus::Empty;
        bool complete = false;
        size_t providerCount = 0;
        size_t particleItemCount = 0;
        size_t waterItemCount = 0;
        size_t terrainItemCount = 0;
        size_t skippedProviderCount = 0;
    };

    struct RenderFeatureSnapshot
    {
        RenderFeatureSnapshotMetadata metadata;
        ParticleRenderSnapshot particles;
        WaterRenderSnapshot water;
        TerrainRenderSnapshot terrain;

        void Clear()
        {
            metadata = {};
            particles.Clear();
            water.Clear();
            terrain.Clear();
        }

        void BeginBuild(uint64 sequence)
        {
            Clear();
            metadata.sequence = sequence;
            metadata.status = RenderFeatureSnapshotStatus::Incomplete;
            particles.BeginBuild(sequence);
            water.BeginBuild(sequence);
            terrain.BeginBuild(sequence);
        }

        void MarkComplete()
        {
            metadata.status = RenderFeatureSnapshotStatus::Complete;
            metadata.complete = true;
            metadata.particleItemCount = particles.items.size();
            metadata.waterItemCount = water.items.size();
            metadata.terrainItemCount = terrain.items.size();
            particles.MarkComplete();
            water.MarkComplete();
            terrain.MarkComplete();
        }

        void MarkIncomplete()
        {
            metadata.status = RenderFeatureSnapshotStatus::Incomplete;
            metadata.complete = false;
            particles.MarkIncomplete();
            water.MarkIncomplete();
            terrain.MarkIncomplete();
        }

        RenderFeatureSnapshotMetadata GetMetadata() const
        {
            RenderFeatureSnapshotMetadata snapshotMetadata = metadata;
            snapshotMetadata.particleItemCount = particles.items.size();
            snapshotMetadata.waterItemCount = water.items.size();
            snapshotMetadata.terrainItemCount = terrain.items.size();
            return snapshotMetadata;
        }
    };

    class IRenderFeatureSnapshotProvider
    {
    public:
        virtual ~IRenderFeatureSnapshotProvider() = default;
        virtual bool AppendRenderFeatureSnapshot(RenderFeatureSnapshot& outSnapshot) const = 0;
    };

} // namespace RVX
