#pragma once

/**
 * @file RenderDrawItem.h
 * @brief Per-submesh draw item used by material-aware render passes
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"

#include <vector>

namespace RVX
{
    class RenderScene;

    namespace Resource
    {
        class MaterialResource;
    } // namespace Resource

    struct RenderDrawItem
    {
        uint32 objectIndex = 0;
        uint32 submeshIndex = 0;
        uint64 meshId = 0;
        uint64 materialId = 0;
        Resource::MaterialResource* materialResource = nullptr;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;
        float depthFromCamera = 0.0f;
        uint64 sortKey = 0;
    };

    void BuildMaterialDrawLists(const RenderScene& scene,
                                const std::vector<uint32_t>& visibleObjectIndices,
                                const Vec3& cameraPosition,
                                std::vector<RenderDrawItem>& outOpaqueDrawItems,
                                std::vector<RenderDrawItem>& outMaskedDrawItems,
                                std::vector<RenderDrawItem>& outTransparentDrawItems);

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item);
    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item);

} // namespace RVX
