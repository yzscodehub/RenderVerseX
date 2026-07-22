#pragma once

/**
 * @file RenderFrameValidation.h
 * @brief Shared validation for update-owned immutable frame values.
 */

#include "RenderContracts/RenderFramePacket.h"

#include <cmath>

namespace RVX
{
    /** @brief Validate all cross-thread render settings invariants. */
    [[nodiscard]] inline bool IsValidRenderFrameSettings(
        const RenderFrameSettings& settings) noexcept
    {
        if (!std::isfinite(settings.renderScale) ||
            settings.renderScale <= 0.0f ||
            !std::isfinite(settings.postProcess.bloomThreshold) ||
            settings.postProcess.bloomThreshold < 0.0f ||
            !std::isfinite(settings.postProcess.bloomIntensity) ||
            settings.postProcess.bloomIntensity < 0.0f ||
            !std::isfinite(settings.shadows.maxDistance))
        {
            return false;
        }
        if (settings.shadows.enabled &&
            (settings.shadows.atlasResolution == 0 ||
             settings.shadows.cascadeCount == 0 ||
             settings.shadows.maxDistance <= 0.0f))
        {
            return false;
        }
        if (settings.gpuCulling.enabled &&
            settings.gpuCulling.maxVisibleObjects == 0)
        {
            return false;
        }
        if (settings.rayTracing.enabled)
        {
            return settings.rayTracing.maxInstances != 0 &&
                   settings.rayTracing.maxRaysPerPixel != 0;
        }
        return !settings.rayTracing.enableShadows &&
               !settings.rayTracing.enableReflections;
    }

    /** @brief Validate an empty or complete one-shot capture request value. */
    [[nodiscard]] inline bool IsValidRenderFrameCaptureRequest(
        const RenderFrameCaptureRequest& request) noexcept
    {
        if (request.kind == RenderFrameCaptureKind::None)
        {
            return request.requestId == 0 && request.width == 0 &&
                   request.height == 0 && !request.includeAlpha;
        }
        const bool declared = request.kind == RenderFrameCaptureKind::Color ||
                              request.kind == RenderFrameCaptureKind::Depth ||
                              request.kind == RenderFrameCaptureKind::ObjectId;
        return declared && request.requestId != 0 && request.width != 0 &&
               request.height != 0;
    }
} // namespace RVX
