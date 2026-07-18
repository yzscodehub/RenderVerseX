#pragma once

/**
 * @file RenderProxy.h
 * @brief Render-only scene proxy data consumed by RenderScene.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"

#include <cstddef>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION = 1;

    struct RenderProxyId
    {
        uint64 value = 0;

        bool IsValid() const { return value != 0; }
    };

    enum class RenderProxySnapshotStatus : uint8
    {
        Empty = 0,
        Complete,
        Incomplete,
    };

    struct RenderProxySnapshotMetadata
    {
        uint32 schemaVersion = RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION;
        uint64 sequence = 0;
        RenderProxySnapshotStatus status = RenderProxySnapshotStatus::Empty;
        bool complete = false;
        size_t primitiveCount = 0;
        size_t lightCount = 0;
    };

    struct RenderPrimitiveProxy
    {
        RenderProxyId id;
        uint64 ownerId = 0;

        Mat4 worldMatrix = Mat4Identity();
        Mat4 normalMatrix = Mat4Identity();
        AABB bounds;

        AssetId meshAssetId;
        std::vector<AssetId> materialAssetIds;
        std::vector<RenderMaterialMode> materialModes;

        std::vector<Mat4> skinningMatrices;

        uint64 sortKey = 0;
        uint32 layerMask = ~0u;
        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;

        bool HasSkinningData() const { return !skinningMatrices.empty(); }
    };

    struct RenderLightProxy
    {
        enum class Type : uint8
        {
            Directional,
            Point,
            Spot
        };

        RenderProxyId id;
        uint64 ownerId = 0;
        Type type = Type::Directional;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 direction{0.0f, 0.0f, -1.0f};
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;
        float innerConeAngle = 0.0f;
        float outerConeAngle = 0.7854f;
        bool castsShadow = false;
    };

    struct RenderProxyCommand
    {
        enum class Type : uint8
        {
            AddOrUpdatePrimitive,
            RemovePrimitive,
            AddOrUpdateLight,
            RemoveLight
        };

        Type type = Type::AddOrUpdatePrimitive;
        RenderProxyId id;
        RenderPrimitiveProxy primitive;
        RenderLightProxy light;
    };

    struct RenderProxySnapshot
    {
        RenderProxySnapshotMetadata metadata;
        std::vector<RenderPrimitiveProxy> primitives;
        std::vector<RenderLightProxy> lights;

        void Clear()
        {
            metadata = {};
            primitives.clear();
            lights.clear();
        }

        void BeginBuild(uint64 sequence)
        {
            Clear();
            metadata.sequence = sequence;
            metadata.status = RenderProxySnapshotStatus::Incomplete;
        }

        void MarkComplete()
        {
            metadata.status = RenderProxySnapshotStatus::Complete;
            metadata.complete = true;
            metadata.primitiveCount = primitives.size();
            metadata.lightCount = lights.size();
        }

        void MarkIncomplete()
        {
            primitives.clear();
            lights.clear();
            metadata.status = RenderProxySnapshotStatus::Incomplete;
            metadata.complete = false;
            metadata.primitiveCount = 0;
            metadata.lightCount = 0;
        }

        RenderProxySnapshotMetadata GetMetadata() const
        {
            RenderProxySnapshotMetadata snapshotMetadata = metadata;
            snapshotMetadata.primitiveCount = primitives.size();
            snapshotMetadata.lightCount = lights.size();
            return snapshotMetadata;
        }
    };

} // namespace RVX
