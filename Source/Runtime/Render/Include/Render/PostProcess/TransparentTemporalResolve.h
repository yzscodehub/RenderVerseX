#pragma once

#include "Render/Graph/RenderGraph.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <atomic>
#include <memory>
#include <string>

namespace RVX
{
    class PipelineCache;
    class RenderFeatureGraphBuilder;

    /** Immutable graph-local surface export; flags require producer-side history validation. */
    struct TransparentTemporalSurfaceInputs
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderOutputExtent extent{};
        /** RGBA16F: NDC motion.xy, composited contribution, positive view depth. */
        RGTextureHandle motionCoverageDepth{};
        /** R32_UINT: present=1, sorted=2, qualified history=4, mixed=8, refractive=16. */
        RGTextureHandle flags{};
        /** R32F linear depth at opaque velocity's surface, not device depth. */
        RGTextureHandle opaqueLinearDepth{};
        /** Global view history gate; per-surface flags cannot override a reset. */
        bool historyAvailable = false;
        /** Production raw-candidate mode: exact device depth and contributing layer count. */
        bool rawCandidates = false;
        RGTextureHandle opaqueDeviceDepth{};
        RGTextureHandle layerCount{};
        RGTextureHandle meshReactive{};
        Mat4 inverseProjectionMatrix = Mat4Identity();
    };

    struct TransparentTemporalResolveReceipt
    {
        std::atomic_bool recorded{false};
        std::atomic_bool failed{false};
    };

    struct TransparentTemporalResolveOutputs
    {
        RGTextureHandle velocity{};
        RGTextureHandle reactive{};
        RGTextureHandle linearDepth{};
        std::shared_ptr<TransparentTemporalResolveReceipt> receipt;
    };

    [[nodiscard]] bool BuildTransparentTemporalResolve(
        RenderFeatureGraphBuilder& builder, PipelineCache& pipelines,
        const RenderFrameIdentity& frame, const RenderViewIdentity& view,
        RGTextureHandle opaqueVelocity, RGTextureHandle reactive,
        const TransparentTemporalSurfaceInputs& surface,
        TransparentTemporalResolveOutputs& output, std::string& failure);
}
