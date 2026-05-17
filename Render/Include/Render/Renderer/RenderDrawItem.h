#pragma once

/**
 * @file RenderDrawItem.h
 * @brief Per-submesh draw item used by material-aware render passes
 */

#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"

namespace RVX
{
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

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item);
    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item);

} // namespace RVX
