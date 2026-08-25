#pragma once

/**
 * @file RenderAmbientOcclusion.h
 * @brief Backend-neutral GTAO policy and deterministic CPU reference helpers.
 *
 * Ambient occlusion is an indirect-light visibility field.  This contract
 * deliberately makes its consumer set closed so implementations cannot darken
 * direct lighting, emissive surfaces, the sky, or translucent composition as
 * a convenient post-process shortcut.
 */

#include "Core/Types.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace RVX
{
    /**
     * @brief The only lighting terms permitted to consume the AO visibility field.
     */
    enum class RenderAmbientOcclusionConsumer : uint32
    {
        IndirectDiffuse = 1U << 0U,
        IndirectSpecular = 1U << 1U,
        IBL = 1U << 2U,
    };

    using RenderAmbientOcclusionConsumerMask = uint32;

    [[nodiscard]] constexpr RenderAmbientOcclusionConsumerMask ToMask(
        RenderAmbientOcclusionConsumer consumer) noexcept
    {
        return static_cast<RenderAmbientOcclusionConsumerMask>(consumer);
    }

    inline constexpr RenderAmbientOcclusionConsumerMask
        RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK =
            ToMask(RenderAmbientOcclusionConsumer::IndirectDiffuse) |
            ToMask(RenderAmbientOcclusionConsumer::IndirectSpecular) |
            ToMask(RenderAmbientOcclusionConsumer::IBL);

    [[nodiscard]] constexpr bool IsValidRenderAmbientOcclusionConsumerMask(
        RenderAmbientOcclusionConsumerMask mask) noexcept
    {
        return mask == RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK;
    }

    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MIN_RADIUS_METERS =
        0.01f;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_RADIUS_METERS =
        32.0f;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_FALLOFF_METERS =
        64.0f;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_THICKNESS_METERS =
        8.0f;
    inline constexpr uint32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_DIRECTION_COUNT =
        64;
    inline constexpr uint32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_STEP_COUNT = 32;
    inline constexpr uint32
        RVX_RENDER_AMBIENT_OCCLUSION_MAX_HORIZON_SAMPLE_COUNT = 256;
    inline constexpr uint32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_SPATIAL_RADIUS = 8;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_DEPTH_SIGMA =
        64.0f;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_TEMPORAL_BLEND =
        0.98f;
    inline constexpr float32 RVX_RENDER_AMBIENT_OCCLUSION_MAX_TEMPORAL_DEPTH_THRESHOLD_METERS =
        4.0f;

    /**
     * @brief Immutable GTAO authoring policy for one frame/view.
     *
     * Every field is validated even when disabled: an invalid dormant setting
     * must not become a latent source of backend-specific behavior when it is
     * enabled on a later frame.
     */
    struct RenderAmbientOcclusionSettings
    {
        bool enabled = false;
        float32 radiusMeters = 1.0f;
        float32 falloffStartMeters = 0.1f;
        float32 falloffEndMeters = 3.0f;
        float32 thicknessMeters = 0.15f;
        uint32 directionCount = 8;
        uint32 stepCount = 4;
        uint32 spatialRadius = 2;
        float32 bilateralDepthSigma = 1.5f;
        float32 bilateralNormalThreshold = 0.8f;
        float32 temporalBlend = 0.9f;
        float32 temporalDepthThresholdMeters = 0.05f;
        float32 temporalNormalThreshold = 0.85f;
        RenderAmbientOcclusionConsumerMask consumerMask =
            RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK;
    };

    [[nodiscard]] inline bool IsValidRenderAmbientOcclusionSettings(
        const RenderAmbientOcclusionSettings& settings) noexcept
    {
        return std::isfinite(settings.radiusMeters) &&
               settings.radiusMeters >=
                   RVX_RENDER_AMBIENT_OCCLUSION_MIN_RADIUS_METERS &&
               settings.radiusMeters <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_RADIUS_METERS &&
               std::isfinite(settings.falloffStartMeters) &&
               settings.falloffStartMeters >= 0.0f &&
               settings.falloffStartMeters < settings.falloffEndMeters &&
               std::isfinite(settings.falloffEndMeters) &&
               settings.falloffEndMeters <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_FALLOFF_METERS &&
               std::isfinite(settings.thicknessMeters) &&
               settings.thicknessMeters >= 0.0f &&
               settings.thicknessMeters <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_THICKNESS_METERS &&
               settings.directionCount != 0 &&
               settings.directionCount <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_DIRECTION_COUNT &&
               settings.stepCount != 0 &&
               settings.stepCount <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_STEP_COUNT &&
               static_cast<uint64>(settings.directionCount) *
                       static_cast<uint64>(settings.stepCount) <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_HORIZON_SAMPLE_COUNT &&
               settings.spatialRadius != 0 &&
               settings.spatialRadius <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_SPATIAL_RADIUS &&
               std::isfinite(settings.bilateralDepthSigma) &&
               settings.bilateralDepthSigma > 0.0f &&
               settings.bilateralDepthSigma <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_DEPTH_SIGMA &&
               std::isfinite(settings.bilateralNormalThreshold) &&
               settings.bilateralNormalThreshold >= 0.0f &&
               settings.bilateralNormalThreshold < 1.0f &&
               std::isfinite(settings.temporalBlend) &&
               settings.temporalBlend >= 0.0f &&
               settings.temporalBlend <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_TEMPORAL_BLEND &&
               std::isfinite(settings.temporalDepthThresholdMeters) &&
               settings.temporalDepthThresholdMeters > 0.0f &&
               settings.temporalDepthThresholdMeters <=
                   RVX_RENDER_AMBIENT_OCCLUSION_MAX_TEMPORAL_DEPTH_THRESHOLD_METERS &&
               std::isfinite(settings.temporalNormalThreshold) &&
               settings.temporalNormalThreshold >= 0.0f &&
               settings.temporalNormalThreshold <= 1.0f &&
               IsValidRenderAmbientOcclusionConsumerMask(settings.consumerMask);
    }

    /** @brief Stable settings identity used to reject incompatible AO histories. */
    [[nodiscard]] inline uint64 CalculateRenderAmbientOcclusionSettingsHash(
        const RenderAmbientOcclusionSettings& settings) noexcept
    {
        if (!IsValidRenderAmbientOcclusionSettings(settings))
            return 0;

        constexpr uint64 offsetBasis = 14695981039346656037ULL;
        constexpr uint64 prime = 1099511628211ULL;
        uint64 hash = offsetBasis;
        const auto append = [&hash](uint32 value) noexcept
        {
            hash ^= static_cast<uint64>(value);
            hash *= prime;
        };
        append(settings.enabled ? 1U : 0U);
        append(std::bit_cast<uint32>(settings.radiusMeters));
        append(std::bit_cast<uint32>(settings.falloffStartMeters));
        append(std::bit_cast<uint32>(settings.falloffEndMeters));
        append(std::bit_cast<uint32>(settings.thicknessMeters));
        append(settings.directionCount);
        append(settings.stepCount);
        append(settings.spatialRadius);
        append(std::bit_cast<uint32>(settings.bilateralDepthSigma));
        append(std::bit_cast<uint32>(settings.bilateralNormalThreshold));
        append(std::bit_cast<uint32>(settings.temporalBlend));
        append(std::bit_cast<uint32>(settings.temporalDepthThresholdMeters));
        append(std::bit_cast<uint32>(settings.temporalNormalThreshold));
        append(settings.consumerMask);
        return hash;
    }

    /**
     * @brief Linearize a perspective [0, 1] device-depth value.
     *
     * The reference uses forward Z with `0 = near` and `1 = far`; reversed-Z
     * support must be normalized before calling it. Renderer GTAO reconstructs
     * world positions from inverse projection instead so perspective and
     * orthographic views share one metric implementation. Invalid numeric or
     * projection inputs are rejected.
     */
    [[nodiscard]] inline bool TryLinearizeRenderAmbientOcclusionDepth(
        float32 deviceDepth,
        float32 nearPlane,
        float32 farPlane,
        float32& outLinearDepth) noexcept
    {
        if (!std::isfinite(deviceDepth) || deviceDepth < 0.0f ||
            deviceDepth > 1.0f || !std::isfinite(nearPlane) ||
            !std::isfinite(farPlane) || nearPlane <= 0.0f ||
            farPlane <= nearPlane)
        {
            return false;
        }

        const float32 denominator = farPlane -
            deviceDepth * (farPlane - nearPlane);
        if (!std::isfinite(denominator) || denominator <= 0.0f)
            return false;

        const float32 linearDepth = nearPlane * farPlane / denominator;
        const float32 tolerance = std::max(nearPlane, farPlane) * 1.0e-4f;
        if (!std::isfinite(linearDepth) || linearDepth < nearPlane - tolerance ||
            linearDepth > farPlane + tolerance)
        {
            return false;
        }
        outLinearDepth = std::clamp(linearDepth, nearPlane, farPlane);
        return true;
    }

    /**
     * @brief Evaluate one deterministic GTAO horizon visibility sample.
     *
     * Coplanar/open samples yield one.  A closer sample with a positive
     * thickness-adjusted horizon decreases visibility, then smoothly fades to
     * one outside the configured falloff interval.
     */
    [[nodiscard]] inline bool TryCalculateRenderAmbientOcclusionHorizonVisibility(
        float32 centerLinearDepth,
        float32 sampleLinearDepth,
        float32 sampleDistanceMeters,
        const RenderAmbientOcclusionSettings& settings,
        float32& outVisibility) noexcept
    {
        if (!IsValidRenderAmbientOcclusionSettings(settings) ||
            !std::isfinite(centerLinearDepth) || centerLinearDepth <= 0.0f ||
            !std::isfinite(sampleLinearDepth) || sampleLinearDepth <= 0.0f ||
            !std::isfinite(sampleDistanceMeters) ||
            sampleDistanceMeters <= 0.0f)
        {
            return false;
        }

        const float32 interval = settings.falloffEndMeters -
                                 settings.falloffStartMeters;
        const float32 normalizedDistance = std::clamp(
            (sampleDistanceMeters - settings.falloffStartMeters) / interval,
            0.0f, 1.0f);
        const float32 smoothDistance = normalizedDistance * normalizedDistance *
            (3.0f - 2.0f * normalizedDistance);
        const float32 falloff = 1.0f - smoothDistance;
        const float32 occluderHeight = centerLinearDepth - sampleLinearDepth -
            settings.thicknessMeters;
        const float32 horizon = std::clamp(
            occluderHeight / sampleDistanceMeters, 0.0f, 1.0f);
        outVisibility = std::clamp(1.0f - horizon * falloff, 0.0f, 1.0f);
        return true;
    }

    /** @brief Bilateral spatial weight shared by the CPU and shader references. */
    [[nodiscard]] inline bool TryCalculateRenderAmbientOcclusionBilateralWeight(
        float32 centerLinearDepth,
        float32 sampleLinearDepth,
        float32 normalDot,
        float32 sampleDistancePixels,
        const RenderAmbientOcclusionSettings& settings,
        float32& outWeight) noexcept
    {
        if (!IsValidRenderAmbientOcclusionSettings(settings) ||
            !std::isfinite(centerLinearDepth) || centerLinearDepth <= 0.0f ||
            !std::isfinite(sampleLinearDepth) || sampleLinearDepth <= 0.0f ||
            !std::isfinite(normalDot) || normalDot < -1.0f || normalDot > 1.0f ||
            !std::isfinite(sampleDistancePixels) || sampleDistancePixels < 0.0f)
        {
            return false;
        }

        if (sampleDistancePixels > static_cast<float32>(settings.spatialRadius) ||
            normalDot < settings.bilateralNormalThreshold)
        {
            outWeight = 0.0f;
            return true;
        }

        const float32 radius = static_cast<float32>(settings.spatialRadius);
        const float32 spatialWeight = std::exp(
            -0.5f * sampleDistancePixels * sampleDistancePixels /
            (radius * radius));
        const float32 depthWeight = std::exp(
            -std::abs(sampleLinearDepth - centerLinearDepth) /
            settings.bilateralDepthSigma);
        const float32 normalWeight = std::clamp(
            (normalDot - settings.bilateralNormalThreshold) /
                (1.0f - settings.bilateralNormalThreshold),
            0.0f, 1.0f);
        outWeight = std::clamp(
            spatialWeight * depthWeight * normalWeight, 0.0f, 1.0f);
        return true;
    }

    /**
     * @brief Reject history across a reprojected world-position or normal discontinuity.
     *
     * The positions are the current pixel and its velocity-reprojected history
     * sample. Comparing all three metric axes keeps the CPU contract equivalent
     * to the shader and rejects lateral disocclusion at equal view depth.
     */
    [[nodiscard]] inline bool IsRenderAmbientOcclusionTemporalHistoryAccepted(
        const Vec3& currentWorldPosition,
        const Vec3& historyWorldPosition,
        float32 normalDot,
        const RenderAmbientOcclusionSettings& settings) noexcept
    {
        const bool positionsFinite =
            std::isfinite(currentWorldPosition.x) &&
            std::isfinite(currentWorldPosition.y) &&
            std::isfinite(currentWorldPosition.z) &&
            std::isfinite(historyWorldPosition.x) &&
            std::isfinite(historyWorldPosition.y) &&
            std::isfinite(historyWorldPosition.z);
        const float32 deltaX =
            currentWorldPosition.x - historyWorldPosition.x;
        const float32 deltaY =
            currentWorldPosition.y - historyWorldPosition.y;
        const float32 deltaZ =
            currentWorldPosition.z - historyWorldPosition.z;
        const float32 distanceSquared =
            deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        const float32 thresholdSquared =
            settings.temporalDepthThresholdMeters *
            settings.temporalDepthThresholdMeters;
        return IsValidRenderAmbientOcclusionSettings(settings) &&
               positionsFinite && std::isfinite(distanceSquared) &&
               std::isfinite(normalDot) && normalDot >= -1.0f &&
               normalDot <= 1.0f &&
               distanceSquared <= thresholdSquared &&
               normalDot >= settings.temporalNormalThreshold;
    }

    /** @brief Clamp a valid temporal sample to the current neighborhood bounds. */
    [[nodiscard]] inline bool TryClampRenderAmbientOcclusionTemporalHistory(
        float32 historyVisibility,
        float32 neighborhoodMinimum,
        float32 neighborhoodMaximum,
        float32& outClampedVisibility) noexcept
    {
        if (!std::isfinite(historyVisibility) || historyVisibility < 0.0f ||
            historyVisibility > 1.0f || !std::isfinite(neighborhoodMinimum) ||
            !std::isfinite(neighborhoodMaximum) || neighborhoodMinimum < 0.0f ||
            neighborhoodMaximum > 1.0f ||
            neighborhoodMinimum > neighborhoodMaximum)
        {
            return false;
        }
        outClampedVisibility = std::clamp(
            historyVisibility, neighborhoodMinimum, neighborhoodMaximum);
        return true;
    }

    /** @brief Lighting-energy partition used to prove indirect-only AO application. */
    struct RenderAmbientOcclusionIndirectLighting
    {
        float32 directDiffuse = 0.0f;
        float32 directSpecular = 0.0f;
        float32 indirectDiffuse = 0.0f;
        float32 indirectSpecular = 0.0f;
        float32 iblDiffuse = 0.0f;
        float32 iblSpecular = 0.0f;
        float32 emissive = 0.0f;
        float32 sky = 0.0f;
    };

    [[nodiscard]] inline bool TryComposeRenderAmbientOcclusionIndirectLighting(
        const RenderAmbientOcclusionIndirectLighting& input,
        float32 visibility,
        RenderAmbientOcclusionConsumerMask consumerMask,
        RenderAmbientOcclusionIndirectLighting& outLighting) noexcept
    {
        const auto finiteNonNegative = [](float32 value) noexcept
        {
            return std::isfinite(value) && value >= 0.0f;
        };
        if (!finiteNonNegative(input.directDiffuse) ||
            !finiteNonNegative(input.directSpecular) ||
            !finiteNonNegative(input.indirectDiffuse) ||
            !finiteNonNegative(input.indirectSpecular) ||
            !finiteNonNegative(input.iblDiffuse) ||
            !finiteNonNegative(input.iblSpecular) ||
            !finiteNonNegative(input.emissive) || !finiteNonNegative(input.sky) ||
            !std::isfinite(visibility) || visibility < 0.0f ||
            visibility > 1.0f ||
            !IsValidRenderAmbientOcclusionConsumerMask(consumerMask))
        {
            return false;
        }

        outLighting = input;
        outLighting.indirectDiffuse *= visibility;
        outLighting.indirectSpecular *= visibility;
        outLighting.iblDiffuse *= visibility;
        outLighting.iblSpecular *= visibility;
        return true;
    }
} // namespace RVX
