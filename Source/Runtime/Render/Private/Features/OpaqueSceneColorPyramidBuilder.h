#pragma once

/**
 * @file OpaqueSceneColorPyramidBuilder.h
 * @brief Late graph-owned opaque HDR refraction pyramid construction.
 */

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <string>

namespace RVX
{
    class PipelineCache;
    class OpaqueSceneColorHistory;

    /**
     * @brief Builds one bounded opaque-scene mip chain after reflection/Sky.
     *
     * High and below start at half source resolution; Ultra keeps a full
     * source-resolution base. Every level remains in one RGBA16F texture so
     * refraction consumers select a mip explicitly without changing screen UV
     * interpretation.
     */
    [[nodiscard]] bool BuildOpaqueSceneColorPyramid(
        RenderFeatureGraphBuilder& builder,
        PipelineCache& pipelineCache,
        const RenderPassRecordContext& context,
        RenderFrameIdentity frameIdentity,
        RenderViewIdentity viewIdentity,
        RenderOutputExtent sourceExtent,
        RenderQualityTier quality,
        OpaqueSceneColorPyramid& outPyramid,
        OpaqueSceneColorPyramidRecordInput& outRecordInput,
        std::string& outFailure, OpaqueSceneColorHistory* temporalHistory = nullptr);
} // namespace RVX
