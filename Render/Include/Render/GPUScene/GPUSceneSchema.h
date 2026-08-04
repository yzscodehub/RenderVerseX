#pragma once

/**
 * @file GPUSceneSchema.h
 * @brief Backend-neutral, value-only GPU scene row schema.
 *
 * This header is intentionally limited to fixed-width scalar values and typed
 * scene-table references. A reference slot indexes only its corresponding CPU
 * scene table; it never represents an object id, descriptor, GPU virtual
 * address, or backend object. Task 11B defines a persistent CPU shadow only:
 * it does not establish GPU residency, upload any row, or change rendering.
 */

#include "Core/Types.h"

#include <cstddef>
#include <type_traits>

namespace RVX
{
    inline constexpr uint32 RVX_GPU_SCENE_SCHEMA_VERSION = 1;

    /** @brief Flags stored in every GPU-scene row header. */
    enum class GPUSceneRowFlags : uint32
    {
        None = 0,
        Live = 1U << 0U,
        Tombstone = 1U << 1U,
    };

    /** @brief Flags describing transform-row payload validity. */
    enum class GPUSceneTransformFlags : uint32
    {
        None = 0,
        PreviousWorldFromLocalValid = 1U << 0U,
        NormalFromLocalValid = 1U << 1U,
    };

    /** @brief Stable conservative-culling states for bounds rows. */
    enum class GPUSceneBoundsFlags : uint32
    {
        None = 0,
        ForceVisible = 1U << 0U,
        Invalid = 1U << 1U,
    };

    /** @brief Render semantics stored in GPUScenePrimitiveRow::primitiveFlags. */
    enum class GPUScenePrimitiveFlags : uint32
    {
        None = 0,
        ReceivesShadow = 1U << 2U,
    };

    static_assert(static_cast<uint32>(GPUScenePrimitiveFlags::ReceivesShadow) ==
                  (1U << 2U));

    /** @brief Backend-neutral material metadata flags for one draw-local row. */
    enum class GPUSceneMaterialFlags : uint32
    {
        None = 0,
        DefaultMaterial = 1U << 0U,
        MissingMaterial = 1U << 1U,
        MetadataInvalid = 1U << 2U,
        Masked = 1U << 3U,
        Transparent = 1U << 4U,
        DoubleSided = 1U << 5U,
        HasTextureBindings = 1U << 6U,
    };

    /** @brief Backend-neutral geometry metadata flags for one draw-local row. */
    enum class GPUSceneGeometryFlags : uint32
    {
        None = 0,
        IndexUInt16 = 1U << 0U,
        IndexUInt32 = 1U << 1U,
        Skinned = 1U << 2U,
    };

    /** @brief Backend-neutral pass eligibility bits for a draw-local row. */
    enum class GPUScenePassMask : uint32
    {
        None = 0,
        Depth = 1U << 0U,
        Opaque = 1U << 1U,
        Shadow = 1U << 2U,
        Transparent = 1U << 3U,
    };

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUSceneTransformFlag(uint32 value, GPUSceneTransformFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUSceneBoundsFlag(uint32 value, GPUSceneBoundsFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUScenePrimitiveFlag(uint32 value, GPUScenePrimitiveFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUSceneRowFlag(uint32 value, GPUSceneRowFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUSceneMaterialFlag(uint32 value, GPUSceneMaterialFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief True when every bit in @p flags is present in @p value. */
    constexpr bool HasGPUSceneGeometryFlag(uint32 value, GPUSceneGeometryFlags flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /**
     * @brief True when every bit in @p flags is present in @p value.
     *
     * Material mode selects only material flags and materialVariant. A masked
     * draw is Depth|Opaque (plus Shadow when it casts); transparent is only
     * Transparent (plus Shadow when it casts). There is intentionally no
     * Masked pass bit.
     */
    constexpr bool HasGPUScenePassMask(uint32 value, GPUScenePassMask flags)
    {
        return (value & static_cast<uint32>(flags)) == static_cast<uint32>(flags);
    }

    /** @brief Shader-portable uint2 with std430-compatible eight-byte alignment. */
    struct alignas(8) GPUSceneUint64
    {
        uint32 low = 0;
        uint32 high = 0;

        constexpr bool operator==(const GPUSceneUint64&) const = default;
    };

    /** @brief Pack a CPU uint64 into shader-portable low/high words. */
    constexpr GPUSceneUint64 PackGPUSceneUint64(uint64 value)
    {
        return {
            static_cast<uint32>(value),
            static_cast<uint32>(value >> 32U)};
    }

    /** @brief Recover a CPU uint64 from shader-portable low/high words. */
    constexpr uint64 UnpackGPUSceneUint64(GPUSceneUint64 value)
    {
        return static_cast<uint64>(value.low) |
               (static_cast<uint64>(value.high) << 32U);
    }

    /** @brief Explicit std430/HLSL-compatible float4 value. */
    struct alignas(16) GPUSceneFloat4
    {
        float32 x = 0.0F;
        float32 y = 0.0F;
        float32 z = 0.0F;
        float32 w = 0.0F;

        constexpr bool operator==(const GPUSceneFloat4&) const = default;
    };

    /** @brief Row-major 4x4 matrix input/output used by explicit schema packing. */
    struct alignas(16) GPUSceneMatrix4x4
    {
        GPUSceneFloat4 rows[4];

        constexpr bool operator==(const GPUSceneMatrix4x4&) const = default;
    };

    /** @brief Row-major 3x4 affine matrix stored by GPU-scene transform rows. */
    struct alignas(16) GPUSceneAffineMatrix3x4
    {
        GPUSceneFloat4 rows[3];

        constexpr bool operator==(const GPUSceneAffineMatrix3x4&) const = default;
    };

    /** @brief Pack the first three rows of an affine row-major matrix. */
    constexpr GPUSceneAffineMatrix3x4 PackGPUSceneAffineMatrix(
        const GPUSceneMatrix4x4& matrix)
    {
        GPUSceneAffineMatrix3x4 packed;
        for (uint32 row = 0; row < 3; ++row)
        {
            packed.rows[row] = matrix.rows[row];
        }
        return packed;
    }

    /** @brief Expand a packed affine matrix with the canonical [0, 0, 0, 1] row. */
    constexpr GPUSceneMatrix4x4 UnpackGPUSceneAffineMatrix(
        const GPUSceneAffineMatrix3x4& packed)
    {
        GPUSceneMatrix4x4 matrix;
        for (uint32 row = 0; row < 3; ++row)
        {
            matrix.rows[row] = packed.rows[row];
        }
        matrix.rows[3] = {0.0F, 0.0F, 0.0F, 1.0F};
        return matrix;
    }

    /** @brief Stable handle into the primitive table. Zero is always invalid. */
    struct alignas(8) GPUScenePrimitiveRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUScenePrimitiveRef&) const = default;
    };

    /** @brief Stable handle into the bounds table. Zero is always invalid. */
    struct alignas(8) GPUSceneBoundsRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUSceneBoundsRef&) const = default;
    };

    /** @brief Stable handle into the transform table. Zero is always invalid. */
    struct alignas(8) GPUSceneTransformRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUSceneTransformRef&) const = default;
    };

    /** @brief Stable handle into the material table. Zero is always invalid. */
    struct alignas(8) GPUSceneMaterialRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUSceneMaterialRef&) const = default;
    };

    /** @brief Stable handle into the geometry table. Zero is always invalid. */
    struct alignas(8) GPUSceneGeometryRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUSceneGeometryRef&) const = default;
    };

    /** @brief Stable handle into the draw-metadata table. Zero is always invalid. */
    struct alignas(8) GPUSceneDrawRef
    {
        uint32 slot = 0;
        uint32 generation = 0;

        [[nodiscard]] constexpr bool IsValid() const
        {
            return slot != 0 && generation != 0;
        }

        constexpr bool operator==(const GPUSceneDrawRef&) const = default;
    };

    /** @brief Common prefix for every mirrored GPU-scene row. */
    struct alignas(16) GPUSceneRowHeader
    {
        GPUSceneUint64 objectId;
        uint32 schemaVersion = RVX_GPU_SCENE_SCHEMA_VERSION;
        uint32 generation = 0;
        uint32 flags = static_cast<uint32>(GPUSceneRowFlags::None);
        uint32 padding[3] = {};

        constexpr bool operator==(const GPUSceneRowHeader&) const = default;
    };

    /**
     * @brief Per-object linkage to bounds, transforms, and a contiguous draw range.
     *
     * When @c drawCount is nonzero, @c firstDraw identifies the first element of
     * one contiguous draw-table allocation and its generation applies to every
     * draw in that allocation. Every row in the range must carry exactly that
     * generation and back-reference this primitive. Future reclamation must
     * retire or reuse the entire block with one strictly advanced generation,
     * permanently retiring it rather than wrapping at UINT32_MAX.
     */
    struct alignas(16) GPUScenePrimitiveRow
    {
        GPUSceneRowHeader header;
        GPUSceneBoundsRef bounds;
        GPUSceneTransformRef transform;
        GPUSceneDrawRef firstDraw;
        uint32 drawCount = 0;
        uint32 primitiveFlags = 0;
        uint32 layerMask = 0;
        uint32 padding0 = 0;
        GPUSceneUint64 sortKey;

        constexpr bool operator==(const GPUScenePrimitiveRow&) const = default;
    };

    /** @brief Bounds as three float4-compatible vectors: minimum, maximum, sphere. */
    struct alignas(16) GPUSceneBoundsRow
    {
        GPUSceneRowHeader header;
        GPUSceneFloat4 minimum;
        GPUSceneFloat4 maximum;
        GPUSceneFloat4 sphere;
        uint32 boundsFlags = 0;
        uint32 padding[3] = {};

        constexpr bool operator==(const GPUSceneBoundsRow&) const = default;
    };

    /**
     * @brief Current, previous, and normal affine transforms.
     *
     * Every array is a 3x4 matrix with row-major element order:
     * @c element = row * 4 + column. This schema explicitly owns that order
     * rather than inheriting a math-library convention.
     */
    struct alignas(16) GPUSceneTransformRow
    {
        GPUSceneRowHeader header;
        GPUSceneAffineMatrix3x4 worldFromLocal;
        GPUSceneAffineMatrix3x4 previousWorldFromLocal;
        GPUSceneAffineMatrix3x4 normalFromLocal;
        uint32 transformFlags = 0;
        uint32 padding[3] = {};

        constexpr bool operator==(const GPUSceneTransformRow&) const = default;
    };

    /** @brief Value-only material data and exact resource-registry identity. */
    struct alignas(16) GPUSceneMaterialRow
    {
        GPUSceneRowHeader header;
        uint32 resourceSlot = 0;
        uint32 resourceGeneration = 0;
        GPUSceneUint64 materialId;
        uint32 materialFlags = 0;
        uint32 shadingModel = 0;
        uint32 padding0[2] = {};
        GPUSceneFloat4 baseColor;
        float32 metallic = 0.0F;
        float32 roughness = 0.0F;
        float32 emissiveIntensity = 0.0F;
        float32 opacity = 0.0F;

        constexpr bool operator==(const GPUSceneMaterialRow&) const = default;
    };

    /** @brief Geometry metadata and exact mesh-resource/submesh/index identity. */
    struct alignas(16) GPUSceneGeometryRow
    {
        GPUSceneRowHeader header;
        uint32 resourceSlot = 0;
        uint32 resourceGeneration = 0;
        GPUSceneUint64 geometryId;
        uint32 submeshIndex = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 indexCount = 0;
        uint32 topology = 0;
        uint32 geometryFlags = 0;
        uint32 padding[2] = {};

        constexpr bool operator==(const GPUSceneGeometryRow&) const = default;
    };

    /** @brief Draw metadata and typed geometry/material ownership for one batch. */
    struct alignas(16) GPUSceneDrawMetadataRow
    {
        GPUSceneRowHeader header;
        GPUScenePrimitiveRef primitive;
        GPUSceneMaterialRef material;
        GPUSceneGeometryRef geometry;
        uint32 indexCount = 0;
        uint32 instanceCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
        uint32 firstInstance = 0;
        uint32 passMask = 0;
        uint32 materialVariant = 0;
        uint32 padding0 = 0;
        GPUSceneUint64 pipelineKey;
        GPUSceneUint64 sortKey;
        uint32 padding[2] = {};

        constexpr bool operator==(const GPUSceneDrawMetadataRow&) const = default;
    };

    static_assert(std::is_standard_layout_v<GPUScenePrimitiveRef>);
    static_assert(std::is_standard_layout_v<GPUSceneUint64>);
    static_assert(std::is_standard_layout_v<GPUSceneFloat4>);
    static_assert(std::is_standard_layout_v<GPUSceneMatrix4x4>);
    static_assert(std::is_standard_layout_v<GPUSceneAffineMatrix3x4>);
    static_assert(std::is_standard_layout_v<GPUSceneBoundsRef>);
    static_assert(std::is_standard_layout_v<GPUSceneTransformRef>);
    static_assert(std::is_standard_layout_v<GPUSceneMaterialRef>);
    static_assert(std::is_standard_layout_v<GPUSceneGeometryRef>);
    static_assert(std::is_standard_layout_v<GPUSceneDrawRef>);
    static_assert(std::is_trivially_copyable_v<GPUScenePrimitiveRef>);
    static_assert(std::is_trivially_copyable_v<GPUSceneUint64>);
    static_assert(std::is_trivially_copyable_v<GPUSceneFloat4>);
    static_assert(std::is_trivially_copyable_v<GPUSceneMatrix4x4>);
    static_assert(std::is_trivially_copyable_v<GPUSceneAffineMatrix3x4>);
    static_assert(std::is_trivially_copyable_v<GPUSceneBoundsRef>);
    static_assert(std::is_trivially_copyable_v<GPUSceneTransformRef>);
    static_assert(std::is_trivially_copyable_v<GPUSceneMaterialRef>);
    static_assert(std::is_trivially_copyable_v<GPUSceneGeometryRef>);
    static_assert(std::is_trivially_copyable_v<GPUSceneDrawRef>);

    static_assert(std::is_standard_layout_v<GPUSceneRowHeader>);
    static_assert(std::is_standard_layout_v<GPUScenePrimitiveRow>);
    static_assert(std::is_standard_layout_v<GPUSceneBoundsRow>);
    static_assert(std::is_standard_layout_v<GPUSceneTransformRow>);
    static_assert(std::is_standard_layout_v<GPUSceneMaterialRow>);
    static_assert(std::is_standard_layout_v<GPUSceneGeometryRow>);
    static_assert(std::is_standard_layout_v<GPUSceneDrawMetadataRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneRowHeader>);
    static_assert(std::is_trivially_copyable_v<GPUScenePrimitiveRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneBoundsRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneTransformRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneMaterialRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneGeometryRow>);
    static_assert(std::is_trivially_copyable_v<GPUSceneDrawMetadataRow>);

    static_assert(sizeof(GPUScenePrimitiveRef) == 8);
    static_assert(sizeof(GPUSceneUint64) == 8);
    static_assert(sizeof(GPUSceneFloat4) == 16);
    static_assert(sizeof(GPUSceneMatrix4x4) == 64);
    static_assert(sizeof(GPUSceneAffineMatrix3x4) == 48);
    static_assert(sizeof(GPUSceneBoundsRef) == 8);
    static_assert(sizeof(GPUSceneTransformRef) == 8);
    static_assert(sizeof(GPUSceneMaterialRef) == 8);
    static_assert(sizeof(GPUSceneGeometryRef) == 8);
    static_assert(sizeof(GPUSceneDrawRef) == 8);
    static_assert(sizeof(GPUSceneRowHeader) == 32);
    static_assert(sizeof(GPUScenePrimitiveRow) == 80);
    static_assert(sizeof(GPUSceneBoundsRow) == 96);
    static_assert(sizeof(GPUSceneTransformRow) == 192);
    static_assert(sizeof(GPUSceneMaterialRow) == 96);
    static_assert(sizeof(GPUSceneGeometryRow) == 80);
    static_assert(sizeof(GPUSceneDrawMetadataRow) == 112);

    static_assert(static_cast<uint32>(GPUSceneRowFlags::None) == 0U);
    static_assert(static_cast<uint32>(GPUSceneRowFlags::Live) == 1U);
    static_assert(static_cast<uint32>(GPUSceneRowFlags::Tombstone) == 2U);
    static_assert(static_cast<uint32>(GPUSceneTransformFlags::None) == 0U);
    static_assert(
        static_cast<uint32>(GPUSceneTransformFlags::PreviousWorldFromLocalValid) == 1U);
    static_assert(static_cast<uint32>(GPUSceneTransformFlags::NormalFromLocalValid) == 2U);
    static_assert(static_cast<uint32>(GPUSceneBoundsFlags::None) == 0U);
    static_assert(static_cast<uint32>(GPUSceneBoundsFlags::ForceVisible) == 1U);
    static_assert(static_cast<uint32>(GPUSceneBoundsFlags::Invalid) == 2U);
    static_assert(static_cast<uint32>(GPUSceneMaterialFlags::DefaultMaterial) == 1U);
    static_assert(static_cast<uint32>(GPUSceneMaterialFlags::MissingMaterial) == 2U);
    static_assert(static_cast<uint32>(GPUSceneMaterialFlags::MetadataInvalid) == 4U);
    static_assert(static_cast<uint32>(GPUSceneGeometryFlags::IndexUInt16) == 1U);
    static_assert(static_cast<uint32>(GPUSceneGeometryFlags::IndexUInt32) == 2U);
    static_assert(static_cast<uint32>(GPUSceneGeometryFlags::Skinned) == 4U);
    static_assert(static_cast<uint32>(GPUScenePassMask::Depth) == 1U);
    static_assert(static_cast<uint32>(GPUScenePassMask::Opaque) == 2U);
    static_assert(static_cast<uint32>(GPUScenePassMask::Shadow) == 4U);
    static_assert(static_cast<uint32>(GPUScenePassMask::Transparent) == 8U);

    static_assert(alignof(GPUSceneUint64) == 8);
    static_assert(alignof(GPUScenePrimitiveRef) == 8);
    static_assert(alignof(GPUSceneBoundsRef) == 8);
    static_assert(alignof(GPUSceneTransformRef) == 8);
    static_assert(alignof(GPUSceneMaterialRef) == 8);
    static_assert(alignof(GPUSceneGeometryRef) == 8);
    static_assert(alignof(GPUSceneDrawRef) == 8);
    static_assert(alignof(GPUSceneFloat4) == 16);
    static_assert(alignof(GPUSceneMatrix4x4) == 16);
    static_assert(alignof(GPUSceneAffineMatrix3x4) == 16);
    static_assert(alignof(GPUSceneRowHeader) == 16);
    static_assert(alignof(GPUScenePrimitiveRow) == 16);
    static_assert(alignof(GPUSceneBoundsRow) == 16);
    static_assert(alignof(GPUSceneTransformRow) == 16);
    static_assert(alignof(GPUSceneMaterialRow) == 16);
    static_assert(alignof(GPUSceneGeometryRow) == 16);
    static_assert(alignof(GPUSceneDrawMetadataRow) == 16);

    static_assert(offsetof(GPUSceneRowHeader, objectId) == 0);
    static_assert(offsetof(GPUSceneRowHeader, schemaVersion) == 8);
    static_assert(offsetof(GPUSceneRowHeader, generation) == 12);
    static_assert(offsetof(GPUSceneRowHeader, flags) == 16);
    static_assert(offsetof(GPUScenePrimitiveRow, bounds) == 32);
    static_assert(offsetof(GPUScenePrimitiveRow, firstDraw) == 48);
    static_assert(offsetof(GPUScenePrimitiveRow, layerMask) == 64);
    static_assert(offsetof(GPUScenePrimitiveRow, sortKey) == 72);
    static_assert(offsetof(GPUSceneBoundsRow, minimum) == 32);
    static_assert(offsetof(GPUSceneBoundsRow, maximum) == 48);
    static_assert(offsetof(GPUSceneBoundsRow, sphere) == 64);
    static_assert(offsetof(GPUSceneTransformRow, worldFromLocal) == 32);
    static_assert(offsetof(GPUSceneTransformRow, previousWorldFromLocal) == 80);
    static_assert(offsetof(GPUSceneTransformRow, normalFromLocal) == 128);
    static_assert(offsetof(GPUSceneMaterialRow, resourceSlot) == 32);
    static_assert(offsetof(GPUSceneMaterialRow, materialId) == 40);
    static_assert(offsetof(GPUSceneMaterialRow, baseColor) == 64);
    static_assert(offsetof(GPUSceneGeometryRow, resourceSlot) == 32);
    static_assert(offsetof(GPUSceneGeometryRow, submeshIndex) == 48);
    static_assert(offsetof(GPUSceneDrawMetadataRow, primitive) == 32);
    static_assert(offsetof(GPUSceneDrawMetadataRow, material) == 40);
    static_assert(offsetof(GPUSceneDrawMetadataRow, geometry) == 48);
    static_assert(offsetof(GPUSceneDrawMetadataRow, passMask) == 76);
    static_assert(offsetof(GPUSceneDrawMetadataRow, pipelineKey) == 88);
    static_assert(offsetof(GPUSceneDrawMetadataRow, sortKey) == 96);
} // namespace RVX
