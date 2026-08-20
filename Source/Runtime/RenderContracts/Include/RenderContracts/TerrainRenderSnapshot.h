#pragma once

/**
 * @file TerrainRenderSnapshot.h
 * @brief Render-facing terrain data contract without Feature, Render, or RHI types.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION = 2;

    enum class TerrainRenderSnapshotStatus : uint8
    {
        Empty = 0,
        Complete,
        Incomplete
    };

    struct TerrainRenderSnapshotItem
    {
        /** @brief Generation-qualified RenderScene object identity. */
        uint64 componentId = 0;
        /** @brief Stable authored terrain-heightmap source asset. */
        AssetId heightmapAssetId{};
        /** @brief Stable authored terrain-material source asset. */
        AssetId materialAssetId{};
        Vec3 worldPosition{0.0f, 0.0f, 0.0f};
        AABB worldBounds;

        Vec3 size{0.0f, 0.0f, 0.0f};
        float lodBias = 0.0f;
        uint32 patchSize = 0;
        uint32 maxLODLevels = 0;

        bool castsShadow = true;
        bool receivesShadow = true;
        bool collisionEnabled = false;
        bool hasHeightmap = false;
        bool heightmapValid = false;
        bool hasMaterial = false;
        bool gpuInitialized = false;
        bool cpuDataAvailable = false;
        bool renderGpuPathAvailable = false;
        std::string renderPathReason;
        std::string heightmapDiagnostic;
        std::string materialDiagnostic;
        std::string lodDiagnostic;
    };

    struct TerrainRenderSnapshotMetadata
    {
        uint32 schemaVersion = RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION;
        uint64 sequence = 0;
        TerrainRenderSnapshotStatus status = TerrainRenderSnapshotStatus::Empty;
        bool complete = false;
        size_t itemCount = 0;
    };

    struct TerrainRenderSnapshot
    {
        TerrainRenderSnapshotMetadata metadata;
        std::vector<TerrainRenderSnapshotItem> items;

        void Clear()
        {
            metadata = {};
            items.clear();
        }

        void BeginBuild(uint64 sequence)
        {
            Clear();
            metadata.sequence = sequence;
            metadata.status = TerrainRenderSnapshotStatus::Incomplete;
        }

        void MarkComplete()
        {
            metadata.status = TerrainRenderSnapshotStatus::Complete;
            metadata.complete = true;
            metadata.itemCount = items.size();
        }

        void MarkIncomplete()
        {
            items.clear();
            metadata.status = TerrainRenderSnapshotStatus::Incomplete;
            metadata.complete = false;
            metadata.itemCount = 0;
        }

        TerrainRenderSnapshotMetadata GetMetadata() const
        {
            TerrainRenderSnapshotMetadata snapshotMetadata = metadata;
            snapshotMetadata.itemCount = items.size();
            return snapshotMetadata;
        }
    };

} // namespace RVX
