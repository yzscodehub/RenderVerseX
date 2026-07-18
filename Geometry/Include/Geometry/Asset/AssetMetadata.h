#pragma once

/**
 * @file AssetMetadata.h
 * @brief Render-neutral metadata exposed by update-owned assets.
 */

#include "Core/Math/AABB.h"
#include "Core/Types.h"

#include <cstddef>

namespace RVX
{
    enum class AssetMaterialMode : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    /** @brief Update-side mesh metadata without upload or RHI concepts. */
    class IMeshAssetMetadata
    {
    public:
        virtual ~IMeshAssetMetadata() = default;

        [[nodiscard]] virtual AABB GetAssetMeshBounds() const = 0;
        [[nodiscard]] virtual size_t GetAssetMeshSubmeshCount() const = 0;
    };

    /** @brief Update-side material routing metadata. */
    class IMaterialAssetMetadata
    {
    public:
        virtual ~IMaterialAssetMetadata() = default;

        [[nodiscard]] virtual AssetMaterialMode GetAssetMaterialMode() const = 0;
    };

    static_assert(static_cast<uint8>(AssetMaterialMode::Opaque) == 0);
    static_assert(static_cast<uint8>(AssetMaterialMode::Masked) == 1);
    static_assert(static_cast<uint8>(AssetMaterialMode::Transparent) == 2);
} // namespace RVX
