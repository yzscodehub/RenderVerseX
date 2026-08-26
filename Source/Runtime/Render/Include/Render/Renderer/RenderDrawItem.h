#pragma once

/**
 * @file RenderDrawItem.h
 * @brief Per-submesh draw item used by material-aware render passes
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "RenderContracts/RenderIdentity.h"

#include <vector>

namespace RVX
{
    class RenderScene;

    struct RenderDrawItem
    {
        /// Stable index into the frame's view-independent scene classification.
        uint32 classificationIndex = RVX_INVALID_INDEX;
        uint32 objectIndex = 0;
        uint32 submeshIndex = 0;
        RenderResourceHandle mesh;
        RenderResourceHandle material;
        MaterialRenderMode renderMode = MaterialRenderMode::Opaque;
        /** Positive projection on the normalized view forward axis. */
        float viewSpaceDepth = 0.0f;
        /** Author-controlled coarse translucency layer; lower layers draw first. */
        int32 translucencySortPriority = 0;
        uint64 sortKey = 0;
        RenderDrawPacket packet;
    };

    /** @brief Value-only validation result for one transparent draw-list build. */
    struct TransparentDrawListDiagnostics
    {
        /** Non-finite view depths are rejected before graph recording. */
        uint32 rejectedNonFiniteDepthCount = 0;
        /** True only when the retained list has the canonical strict order. */
        bool orderValid = true;
        /** Stable hash of the retained back-to-front order. */
        uint64 orderHash = 0;
    };

    void BuildMaterialDrawLists(const RenderScene& scene,
                                const std::vector<uint32_t>& visibleObjectIndices,
                                const Vec3& cameraPosition,
                                std::vector<RenderDrawItem>& outOpaqueDrawItems,
                                std::vector<RenderDrawItem>& outMaskedDrawItems,
                                std::vector<RenderDrawItem>& outTransparentDrawItems,
                                TransparentDrawListDiagnostics*
                                    outTransparentDiagnostics = nullptr);

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item);
    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item);
    [[nodiscard]] bool IsTransparentDrawListStrictlyOrdered(
        const std::vector<RenderDrawItem>& drawItems) noexcept;
    [[nodiscard]] uint64 BuildTransparentDrawOrderHash(
        const std::vector<RenderDrawItem>& drawItems) noexcept;

} // namespace RVX
