#pragma once

/**
 * @file RenderDrawPacket.h
 * @brief Stable value keys and arguments for material draw submission.
 */

#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Material/MaterialGPUData.h"
#include "Render/Policy/RenderPolicyTypes.h"
#include "Render/Renderer/MeshBatch.h"

#include <cstddef>

namespace RVX
{
    struct PipelineKey
    {
        MaterialPipelineVariant materialVariant =
            MaterialPipelineVariant::Opaque;
        MeshUploadPrimitiveTopology topology =
            MeshUploadPrimitiveTopology::Triangles;
        bool skinned = false;

        [[nodiscard]] bool operator==(const PipelineKey& other) const noexcept;
    };

    struct GeometryBindingKey
    {
        RenderResourceHandle mesh;
        uint32 submeshIndex = 0;
        MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;

        [[nodiscard]] bool operator==(const GeometryBindingKey& other) const noexcept;
    };

    struct MaterialBindingKey
    {
        RenderResourceHandle material;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;

        [[nodiscard]] bool operator==(const MaterialBindingKey& other) const noexcept;
    };

    struct RenderDrawArguments
    {
        uint32 indexCount = 0;
        uint32 instanceCount = 1;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;

        [[nodiscard]] bool operator==(const RenderDrawArguments& other) const noexcept = default;
    };

    enum class RenderDrawFlags : uint32
    {
        None = 0,
        Skinned = 1U << 0U,
        Masked = 1U << 1U,
        Transparent = 1U << 2U,
        CastsShadow = 1U << 3U,
        ReceivesShadow = 1U << 4U,
        MissingMaterial = 1U << 5U,
    };

    constexpr RenderDrawFlags operator|(RenderDrawFlags lhs,
                                        RenderDrawFlags rhs) noexcept
    {
        return static_cast<RenderDrawFlags>(
            static_cast<uint32>(lhs) | static_cast<uint32>(rhs));
    }

    struct RenderDrawPacket
    {
        RenderObjectId objectId = 0;
        PrimitiveDataIndex primitiveData = RVX_INVALID_PRIMITIVE_DATA_INDEX;
        uint32 submeshIndex = 0;
        RenderPassKind pass = RenderPassKind::None;
        PipelineKey pipelineKey;
        GeometryBindingKey geometryKey;
        MaterialBindingKey materialKey;
        MaterialInstanceBindingKey materialInstanceKey;
        RenderDrawArguments arguments;
        RenderDrawFlags flags = RenderDrawFlags::None;

        [[nodiscard]] bool operator==(const RenderDrawPacket& other) const noexcept = default;
    };

    [[nodiscard]] uint64 GetStableHash(const PipelineKey& key) noexcept;
    [[nodiscard]] uint64 GetStableHash(const GeometryBindingKey& key) noexcept;
    [[nodiscard]] uint64 GetStableHash(const MaterialBindingKey& key) noexcept;

    struct PipelineKeyHasher
    {
        [[nodiscard]] size_t operator()(const PipelineKey& key) const noexcept;
    };

    struct GeometryBindingKeyHasher
    {
        [[nodiscard]] size_t operator()(const GeometryBindingKey& key) const noexcept;
    };

    struct MaterialBindingKeyHasher
    {
        [[nodiscard]] size_t operator()(const MaterialBindingKey& key) const noexcept;
    };

    /** @brief Adapt one MeshBatch into its legacy material-pass packet. */
    [[nodiscard]] RenderDrawPacket BuildLegacyMaterialDrawPacket(
        const MeshBatch& batch) noexcept;
} // namespace RVX
