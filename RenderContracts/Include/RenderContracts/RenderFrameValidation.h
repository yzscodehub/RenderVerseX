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
            !std::isfinite(settings.postProcess.bloomRadius) ||
            settings.postProcess.bloomRadius < 0.0f ||
            !std::isfinite(settings.postProcess.exposure) ||
            settings.postProcess.exposure < 0.0f ||
            !std::isfinite(settings.postProcess.cameraEV100) ||
            !std::isfinite(settings.postProcess.exposureCompensationEV) ||
            !std::isfinite(settings.postProcess.gamma) ||
            settings.postProcess.gamma <= 0.0f ||
            !std::isfinite(settings.shadows.maxDistance))
        {
            return false;
        }
        if (settings.shadows.enabled &&
            (settings.shadows.atlasResolution == 0 ||
             settings.shadows.cascadeCount == 0 ||
             settings.shadows.maxDistance <= 0.0f ||
             !std::isfinite(settings.shadows.cascadeSplitLambda) ||
             settings.shadows.cascadeSplitLambda < 0.0f ||
             settings.shadows.cascadeSplitLambda > 1.0f ||
             !std::isfinite(settings.shadows.filterRadiusTexels) ||
             settings.shadows.filterRadiusTexels < 0.0f ||
             !std::isfinite(settings.shadows.shadowBias) ||
             !std::isfinite(settings.shadows.normalBias) ||
             !std::isfinite(settings.shadows.cascadeBlendRatio) ||
             settings.shadows.cascadeBlendRatio < 0.0f ||
             settings.shadows.cascadeBlendRatio > 1.0f))
        {
            return false;
        }
        if (settings.gpuCulling.enabled &&
            (settings.gpuCulling.maxVisibleObjects == 0 ||
             !std::isfinite(settings.gpuCulling.maxDrawDistance) ||
             settings.gpuCulling.maxDrawDistance <= 0.0f))
        {
            return false;
        }
        if (settings.rayTracing.enabled)
        {
            return settings.rayTracing.maxInstances != 0 &&
                   settings.rayTracing.maxRaysPerPixel != 0 &&
                   std::isfinite(settings.rayTracing.maxMeasuredGpuMs) &&
                   settings.rayTracing.maxMeasuredGpuMs >= 0.0f &&
                   std::isfinite(
                       settings.rayTracing.maxShadowMeasuredGpuMs) &&
                   settings.rayTracing.maxShadowMeasuredGpuMs >= 0.0f &&
                   std::isfinite(
                       settings.rayTracing.maxReflectionMeasuredGpuMs) &&
                   settings.rayTracing.maxReflectionMeasuredGpuMs >= 0.0f &&
                   settings.rayTracing.gpuTimingAdjustmentFrameCount != 0;
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
