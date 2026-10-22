#pragma once

/**
 * @file TransmissionDepthPyramid.h
 * @brief Conservative graph-owned depth hierarchy for ThinTransmission.
 */

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <memory>
#include <string>

namespace RVX
{
    class PipelineCache;

    /**
     * @brief Immutable R32 depth hierarchy created for the current graph.
     *
     * The completion receipt means every mip was recorded. It deliberately
     * does not claim physical GPU completion.
     */
    struct TransmissionDepthPyramidOutput
    {
        RGTextureHandle texture{};
        RenderOutputExtent baseExtent{};
        uint32 mipCount = 0;
        std::shared_ptr<TransmissionDepthPyramidBuildReceipt> receipt{};
    };

    /**
     * @brief Builds a capped conservative depth chain from the exact D32
     *        scene-depth snapshot for the current frame and view.
     *
     * High and lower tiers use a ceil-half base; Ultra uses a full-resolution
     * base. Later levels are floor-half and use the opaque color pyramid's
     * cap so every legal refraction color mip has matching depth coverage.
     */
    [[nodiscard]] bool BuildTransmissionDepthPyramid(
        RenderFeatureGraphBuilder& builder,
        PipelineCache& pipelineCache,
        const SceneDepthSnapshotRecordInput& source,
        RenderQualityTier quality,
        bool reverseZ,
        TransmissionDepthPyramidOutput& outPyramid,
        std::string& outFailure);
} // namespace RVX
