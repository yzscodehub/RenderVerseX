#pragma once

/**
 * @file RenderFrameValidation.h
 * @brief Shared validation for update-owned immutable frame values.
 */

#include "RenderContracts/RenderFrameTypes.h"

#include <cmath>
#include <limits>

namespace RVX
{
    [[nodiscard]] inline bool IsFiniteRenderValue(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] inline bool IsFiniteRenderValue(float64 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] inline bool IsFiniteRenderValue(const Vec3& value) noexcept
    {
        return IsFiniteRenderValue(value.x) && IsFiniteRenderValue(value.y) &&
               IsFiniteRenderValue(value.z);
    }

    [[nodiscard]] inline bool IsFiniteRenderValue(const Vec4& value) noexcept
    {
        return IsFiniteRenderValue(value.x) && IsFiniteRenderValue(value.y) &&
               IsFiniteRenderValue(value.z) && IsFiniteRenderValue(value.w);
    }

    [[nodiscard]] inline bool IsFiniteRenderValue(const Mat4& value) noexcept
    {
        return IsFiniteRenderValue(value[0]) &&
               IsFiniteRenderValue(value[1]) &&
               IsFiniteRenderValue(value[2]) &&
               IsFiniteRenderValue(value[3]);
    }

    [[nodiscard]] inline bool IsValidRenderFrameStamp(
        const RenderFrameStamp& stamp) noexcept
    {
        return stamp.sequence != 0 &&
               IsFiniteRenderValue(stamp.absoluteTimeSeconds) &&
               stamp.absoluteTimeSeconds >= 0.0 &&
               IsFiniteRenderValue(stamp.deltaTimeSeconds) &&
               stamp.deltaTimeSeconds >= 0.0f;
    }

    [[nodiscard]] inline bool IsValidEffectFrameTime(
        const EffectFrameTime& time) noexcept
    {
        return IsFiniteRenderValue(time.fixedDeltaSeconds) &&
               time.fixedDeltaSeconds >= 0.0f &&
               IsFiniteRenderValue(time.simulationSeconds) &&
               time.simulationSeconds >= 0.0 && time.deterministicSeed != 0;
    }

    /** @brief Validate one backend-neutral view clear policy and value-owned color. */
    [[nodiscard]] inline bool IsValidRenderViewClearValues(
        const RenderViewSnapshot& view) noexcept
    {
        return IsValidRenderViewClearPolicy(view.clearPolicy) &&
               std::isfinite(view.clearColor.x) &&
               std::isfinite(view.clearColor.y) &&
               std::isfinite(view.clearColor.z) &&
               std::isfinite(view.clearColor.w);
    }

    /** @brief Validate all backend-neutral values of one exact render view. */
    [[nodiscard]] inline bool IsValidRenderViewSnapshot(
        const RenderViewSnapshot& view) noexcept
    {
        return view.identity.IsValid() && view.cameraCutRevision != 0 &&
               IsFiniteRenderValue(view.viewMatrix) &&
               IsFiniteRenderValue(view.projectionMatrix) &&
               IsFiniteRenderValue(view.viewProjectionMatrix) &&
               IsFiniteRenderValue(view.inverseViewProjectionMatrix) &&
               IsFiniteRenderValue(view.cameraPosition) &&
               IsFiniteRenderValue(view.cameraDirection) &&
               IsFiniteRenderValue(view.cameraUp) &&
               glm::dot(view.cameraDirection, view.cameraDirection) > 0.0f &&
               glm::dot(view.cameraUp, view.cameraUp) > 0.0f &&
               view.viewportWidth != 0 && view.viewportHeight != 0 &&
               IsFiniteRenderValue(view.nearPlane) && view.nearPlane > 0.0f &&
               IsFiniteRenderValue(view.farPlane) &&
               view.farPlane > view.nearPlane &&
               IsFiniteRenderValue(view.absoluteTime) &&
               view.absoluteTime >= 0.0 &&
               view.absoluteTime <= static_cast<float64>(
                   std::numeric_limits<float32>::max()) &&
               IsFiniteRenderValue(view.deltaTime) && view.deltaTime >= 0.0f &&
               IsFiniteRenderValue(view.exposure) && view.exposure >= 0.0f &&
               IsValidRenderViewClearValues(view);
    }

    /**
     * @brief Validate the deliberately single-view GFX01 family contract.
     *
     * Multiple views fail closed until the renderer owns per-view resource and
     * history execution. This prevents a vector-shaped API from overstating
     * actual stereo or reflection-view support.
     */
    [[nodiscard]] inline bool IsValidSingleRenderViewFamily(
        const RenderViewFamilySnapshot& family) noexcept
    {
        if (!IsValidRenderFrameStamp(family.frame) ||
            !family.identity.IsValid() ||
            !IsValidEffectFrameTime(family.effectTime) ||
            !family.extent.IsValid() || family.familyId == 0 ||
            family.primaryViewIndex != 0 ||
            family.views.size() != 1 ||
            !IsValidRenderViewSnapshot(family.views.front()))
        {
            return false;
        }

        const RenderViewSnapshot& view = family.views.front();
        const uint64 viewportRight = static_cast<uint64>(view.viewportX) +
                                     static_cast<uint64>(view.viewportWidth);
        const uint64 viewportBottom = static_cast<uint64>(view.viewportY) +
                                      static_cast<uint64>(view.viewportHeight);
        return family.frame.sequence == family.identity.frameSequence &&
               viewportRight <= family.extent.width &&
               viewportBottom <= family.extent.height &&
               family.frame.absoluteTimeSeconds == view.absoluteTime &&
               family.frame.deltaTimeSeconds == view.deltaTime;
    }

    /** @brief Build the exact temporal-resource key for a validated mono family. */
    [[nodiscard]] inline RenderViewHistoryIdentity MakeRenderViewHistoryIdentity(
        const RenderViewFamilySnapshot& family,
        uint64 worldRevision,
        uint64 temporalEpoch,
        uint64 formatRevision) noexcept
    {
        RenderViewHistoryIdentity identity;
        if (!IsValidSingleRenderViewFamily(family))
            return identity;

        const RenderViewSnapshot& view = family.views.front();
        identity.previousFrame = family.identity;
        identity.view = view.identity;
        identity.extent = family.extent;
        identity.familyId = family.familyId;
        identity.worldRevision = worldRevision;
        identity.temporalEpoch = temporalEpoch;
        identity.formatRevision = formatRevision;
        identity.surfaceGeneration = family.identity.surfaceGeneration;
        identity.cameraCutRevision = view.cameraCutRevision;
        return identity;
    }

    /** @brief Compare one incoming view with the last committed history identity. */
    [[nodiscard]] inline RenderHistoryAvailability EvaluateRenderHistoryAvailability(
        const RenderViewHistoryIdentity* lastCommitted,
        const RenderViewHistoryIdentity& incoming,
        bool explicitDiscontinuity,
        bool committedResourceAvailable = true) noexcept
    {
        RenderHistoryAvailability result;
        result.identity = incoming;
        result.resetReasons = RenderHistoryResetReason::None;
        if (!incoming.IsValid())
        {
            result.resetReasons = RenderHistoryResetReason::HistoryUnavailable;
            return result;
        }
        if (lastCommitted == nullptr || !lastCommitted->IsValid())
        {
            result.resetReasons |= RenderHistoryResetReason::FirstFrame;
            result.resetReasons |= RenderHistoryResetReason::HistoryUnavailable;
        }
        else
        {
            if (lastCommitted->view != incoming.view)
                result.resetReasons |= RenderHistoryResetReason::ViewChanged;
            if (lastCommitted->familyId != incoming.familyId)
                result.resetReasons |= RenderHistoryResetReason::ViewFamilyChanged;
            if (lastCommitted->worldRevision != incoming.worldRevision)
                result.resetReasons |=
                    RenderHistoryResetReason::SceneDiscontinuity;
            if (lastCommitted->temporalEpoch != incoming.temporalEpoch)
                result.resetReasons |=
                    RenderHistoryResetReason::TemporalEpochChanged;
            if (lastCommitted->cameraCutRevision != incoming.cameraCutRevision)
                result.resetReasons |= RenderHistoryResetReason::CameraCut;
            if (lastCommitted->surfaceGeneration != incoming.surfaceGeneration)
                result.resetReasons |= RenderHistoryResetReason::SurfaceChanged;
            if (lastCommitted->extent != incoming.extent)
                result.resetReasons |= RenderHistoryResetReason::Resize;
            if (lastCommitted->formatRevision != incoming.formatRevision)
                result.resetReasons |= RenderHistoryResetReason::FormatChanged;
            if (lastCommitted->previousFrame.configurationRevision !=
                incoming.previousFrame.configurationRevision)
            {
                result.resetReasons |=
                    RenderHistoryResetReason::ConfigurationChanged;
            }
        }
        if (!committedResourceAvailable)
            result.resetReasons |= RenderHistoryResetReason::HistoryUnavailable;
        if (explicitDiscontinuity)
            result.resetReasons |= RenderHistoryResetReason::ExplicitDiscontinuity;

        result.available = result.resetReasons == RenderHistoryResetReason::None;
        return result;
    }

    /** @brief Validate all cross-thread render settings invariants. */
    [[nodiscard]] inline bool IsValidRenderFrameSettings(
        const RenderFrameSettings& settings) noexcept
    {
        if (!IsValidRenderPipelineRequest(settings.pipelineRequest) ||
            !IsValidRenderAntiAliasingMode(
                settings.postProcess.antiAliasingMode) ||
            !std::isfinite(settings.renderScale) ||
            settings.renderScale <= 0.0f ||
            !std::isfinite(settings.postProcess.bloomThreshold) ||
            settings.postProcess.bloomThreshold < 0.0f ||
            !std::isfinite(settings.postProcess.bloomIntensity) ||
            settings.postProcess.bloomIntensity < 0.0f ||
            !std::isfinite(settings.postProcess.bloomRadius) ||
            settings.postProcess.bloomRadius < 0.0f ||
            !std::isfinite(settings.postProcess.colorGradingContrast) ||
            settings.postProcess.colorGradingContrast < 0.0f ||
            !std::isfinite(settings.postProcess.colorGradingSaturation) ||
            settings.postProcess.colorGradingSaturation < 0.0f ||
            !std::isfinite(settings.postProcess.colorGradingBrightness) ||
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
        if (settings.postProcess.antiAliasingMode ==
                RenderAntiAliasingMode::TAA &&
            !settings.postProcess.enableVelocityOutput)
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
        switch (settings.gpuCulling.mode)
        {
            case RenderGPUDrivenMode::Auto:
            case RenderGPUDrivenMode::ForceEnabled:
            case RenderGPUDrivenMode::ForceDisabled:
                break;
            default:
                return false;
        }
        switch (settings.instancingMode)
        {
            case RenderInstancingMode::Disabled:
            case RenderInstancingMode::Auto:
                break;
            default:
                return false;
        }
        if (settings.gpuCulling.mode != RenderGPUDrivenMode::ForceDisabled &&
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
                   request.height == 0 && !request.includeAlpha &&
                   !request.pixelProbeEnabled && request.pixelProbeX == 0 &&
                   request.pixelProbeY == 0;
        }
        const bool declared = request.kind == RenderFrameCaptureKind::Color ||
                              request.kind == RenderFrameCaptureKind::Depth ||
                              request.kind == RenderFrameCaptureKind::ObjectId;
        if (!declared || request.requestId == 0 || request.width == 0 ||
            request.height == 0)
        {
            return false;
        }
        if (!request.pixelProbeEnabled)
        {
            return request.pixelProbeX == 0 && request.pixelProbeY == 0;
        }
        return request.kind == RenderFrameCaptureKind::Color &&
               request.pixelProbeX < request.width &&
               request.pixelProbeY < request.height;
    }
} // namespace RVX
