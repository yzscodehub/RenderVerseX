#pragma once

/** @file RenderPassClearValues.h @brief Stable render-pass clear-value policy */

#include "RenderContracts/RenderFrameTypes.h"
#include "RHI/RHIResources.h"

namespace RVX
{
    inline constexpr RHIClearColor RVX_SCENE_COLOR_CLEAR_VALUE = {
        0.1f,
        0.1f,
        0.15f,
        1.0f};

    /**
     * @brief Known initialization for a newly-created scene-color transient.
     *
     * DepthOnly and Nothing normally request a color Load. The current
     * single-view post-process graph has no previous scene-color image to
     * import, so it initializes a fresh transient to this deterministic value
     * before the later Load. The raster policy itself is not changed.
     */
    inline constexpr RHIClearColor
        RVX_SCENE_COLOR_TRANSIENT_LOAD_FALLBACK_VALUE =
            RVX_SCENE_COLOR_CLEAR_VALUE;

    struct RenderViewClearActions
    {
        RHILoadOp colorLoadOp = RHILoadOp::Clear;
        RHILoadOp depthLoadOp = RHILoadOp::Clear;
        RHIClearColor colorClearValue = RVX_SCENE_COLOR_CLEAR_VALUE;
        bool permitsSkybox = true;
        bool requiresDeterministicTransientColorInitialization = false;
    };

    [[nodiscard]] inline RHIClearColor MakeRHIClearColor(
        const Vec4& color) noexcept
    {
        return {color.x, color.y, color.z, color.w};
    }

    /**
     * @brief Resolves the backend-neutral camera clear policy for all raster
     * passes sharing a ViewData snapshot.
     */
    [[nodiscard]] inline RenderViewClearActions ResolveRenderViewClearActions(
        RenderViewClearPolicy policy,
        const Vec4& clearColor) noexcept
    {
        const RHIClearColor requestedClearColor =
            MakeRHIClearColor(clearColor);
        switch (policy)
        {
        case RenderViewClearPolicy::Skybox:
            return {RHILoadOp::Clear,
                    RHILoadOp::Clear,
                    RVX_SCENE_COLOR_CLEAR_VALUE,
                    true,
                    false};

        case RenderViewClearPolicy::SolidColor:
            return {RHILoadOp::Clear,
                    RHILoadOp::Clear,
                    requestedClearColor,
                    false,
                    false};

        case RenderViewClearPolicy::DepthOnly:
            return {RHILoadOp::Load,
                    RHILoadOp::Clear,
                    requestedClearColor,
                    false,
                    true};

        case RenderViewClearPolicy::Nothing:
            return {RHILoadOp::Load,
                    RHILoadOp::Load,
                    requestedClearColor,
                    false,
                    true};
        }

        // Invalid values are rejected at frame validation. Keep an explicit
        // fail-closed fallback for defensive call sites.
        return {RHILoadOp::Clear,
                RHILoadOp::Clear,
                RVX_SCENE_COLOR_CLEAR_VALUE,
                false,
                false};
    }
} // namespace RVX
