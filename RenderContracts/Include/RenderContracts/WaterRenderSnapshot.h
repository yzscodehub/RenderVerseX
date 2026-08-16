#pragma once

/**
 * @file WaterRenderSnapshot.h
 * @brief Render-facing water data contract without Feature, Render, or RHI types.
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
    inline constexpr uint32 RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION = 2;

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
        Simple = 0,
        Gerstner,
        FFT
    };

    struct WaterRenderSnapshotItem
    {
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
        uint32 resolution = 0;

        WaterRenderSnapshotSurfaceType surfaceType = WaterRenderSnapshotSurfaceType::Ocean;
        WaterRenderSnapshotSimulationType simulationType = WaterRenderSnapshotSimulationType::Gerstner;

        Vec3 shallowColor{0.0f, 0.4f, 0.5f};
        Vec3 deepColor{0.0f, 0.1f, 0.2f};
        Vec3 foamColor{1.0f, 1.0f, 1.0f};
        float transparency = 0.8f;
        float reflectionStrength = 0.5f;
        float refractionStrength = 0.1f;
        float roughness = 0.1f;
        float foamIntensity = 1.0f;

        bool reflectionEnabled = false;
        bool refractionEnabled = false;
        bool causticsEnabled = false;
        bool underwaterEffectsEnabled = false;
        bool foamEnabled = false;
        bool gpuInitialized = false;
        bool cpuSimulationAvailable = false;
        bool renderGpuPathAvailable = false;
        std::string gpuInitializationReason;
        std::string simulationFallbackReason;
        std::string renderPathReason;
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
