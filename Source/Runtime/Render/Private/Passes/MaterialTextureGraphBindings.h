#pragma once

/**
 * @file MaterialTextureGraphBindings.h
 * @brief RenderGraph declarations for registry-owned material textures.
 */

#include "RenderContracts/RenderIdentity.h"

namespace RVX
{
    class RenderGraphBuilder;
    class RenderResourceRegistry;
    struct RenderPassRecordResults;

    /**
     * @brief Declare exact-generation material textures as graph shader reads.
     *
     * Missing/unready texture generations remain MaterialSystem fallback
     * decisions. A resident exact generation is imported once per graph and
     * shared by every pass that samples it.
     */
    [[nodiscard]] bool DeclareMaterialTextureGraphReads(
        RenderGraphBuilder& builder,
        const RenderResourceRegistry* registry,
        RenderResourceHandle material,
        RenderPassRecordResults& results);
} // namespace RVX
