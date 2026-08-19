#pragma once

/**
 * @file MeshBatch.h
 * @brief Immutable CPU draw-batch values derived from render scene metadata.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"
#include "RenderContracts/ResourceUploadRequest.h"

#include <vector>

namespace RVX
{
    using RenderObjectId = uint64;
    using PrimitiveDataIndex = uint32;
    inline constexpr PrimitiveDataIndex RVX_INVALID_PRIMITIVE_DATA_INDEX =
        ~PrimitiveDataIndex{0};

    enum class RenderBatchFlags : uint32
    {
        None = 0,
        Skinned = 1U << 0U,
        Masked = 1U << 1U,
        Transparent = 1U << 2U,
        CastsShadow = 1U << 3U,
        ReceivesShadow = 1U << 4U,
        MissingMaterial = 1U << 5U,
    };

    constexpr RenderBatchFlags operator|(RenderBatchFlags lhs,
                                         RenderBatchFlags rhs) noexcept
    {
        return static_cast<RenderBatchFlags>(
            static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    constexpr RenderBatchFlags& operator|=(RenderBatchFlags& lhs,
                                           RenderBatchFlags rhs) noexcept
    {
        lhs = lhs | rhs;
        return lhs;
    }

    constexpr bool HasRenderBatchFlag(RenderBatchFlags flags,
                                      RenderBatchFlags flag) noexcept
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    /** @brief A value-only batch. It deliberately owns no transform or RHI state. */
    struct MeshBatch
    {
        RenderObjectId objectId = 0;
        RenderResourceHandle mesh;
        RenderResourceHandle material;
        uint32 submeshIndex = 0;
        PrimitiveDataIndex primitiveData = RVX_INVALID_PRIMITIVE_DATA_INDEX;
        MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;
        MeshUploadSubmesh geometry;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
        RenderBatchFlags flags = RenderBatchFlags::None;
        /** Revision of the complete retained object state that produced this batch. */
        uint64 objectRevision = 0;
    };

    struct MeshBatchSourceSubmesh
    {
        uint32 submeshIndex = 0;
        MeshUploadSubmesh geometry;
        RenderResourceHandle material;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
    };

    /** @brief Pure source values collected from a render object and mesh metadata. */
    struct MeshBatchBuildInput
    {
        RenderObjectId objectId = 0;
        RenderResourceHandle mesh;
        PrimitiveDataIndex primitiveData = RVX_INVALID_PRIMITIVE_DATA_INDEX;
        MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;
        Vec3 boundsMin{0.0f};
        Vec3 boundsMax{0.0f};
        RenderBatchFlags flags = RenderBatchFlags::None;
        std::vector<MeshBatchSourceSubmesh> submeshes;
        uint64 objectRevision = 0;
    };

    enum class MeshBatchBuildCode : uint8
    {
        Success = 0,
        InvalidObject,
        InvalidMesh,
        InvalidPrimitiveData,
        MalformedBounds,
        MissingSubmesh,
        InvalidSubmesh,
    };

    struct MeshBatchBuildResult
    {
        MeshBatchBuildCode code = MeshBatchBuildCode::InvalidObject;
        std::vector<MeshBatch> batches;

        [[nodiscard]] bool IsSuccess() const noexcept
        {
            return code == MeshBatchBuildCode::Success;
        }
    };

    /** @brief Deterministically create one MeshBatch for each source submesh. */
    [[nodiscard]] MeshBatchBuildResult BuildMeshBatches(
        const MeshBatchBuildInput& input);
} // namespace RVX
