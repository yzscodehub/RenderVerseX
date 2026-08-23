#pragma once

/**
 * @file TemporalJitter.h
 * @brief Deterministic, pixel-space temporal sampling jitter contracts.
 */

#include "RenderContracts/RenderFrameTypes.h"

#include <cmath>
#include <limits>

namespace RVX
{
    /** @brief Fixed number of Halton(2, 3) samples in the temporal sequence. */
    inline constexpr uint32 RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT = 8U;

    /**
     * @brief Exact immutable jitter used to rasterize one render view.
     *
     * `pixelOffset` is expressed in pixel units about the pixel center.  The
     * matching `ndcOffset` is supplied so every caller applies the same
     * backend-neutral projection perturbation.
     */
    struct RenderTemporalJitter
    {
        uint32 phaseIndex = std::numeric_limits<uint32>::max();
        RenderOutputExtent extent;
        Vec2 pixelOffset{0.0f};
        Vec2 ndcOffset{0.0f};

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return phaseIndex < RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT &&
                   extent.IsValid() && std::isfinite(pixelOffset.x) &&
                   std::isfinite(pixelOffset.y) && std::isfinite(ndcOffset.x) &&
                   std::isfinite(ndcOffset.y);
        }

        friend bool operator==(const RenderTemporalJitter&,
                               const RenderTemporalJitter&) = default;
    };

    /** @brief Evaluate the zero-based Halton radical inverse for a valid base. */
    [[nodiscard]] constexpr float32 CalculateRenderHalton(
        uint32 index,
        uint32 base) noexcept
    {
        if (base < 2U)
            return 0.0f;

        float32 value = 0.0f;
        float32 scale = 1.0f / static_cast<float32>(base);
        while (index != 0U)
        {
            value += scale * static_cast<float32>(index % base);
            index /= base;
            scale /= static_cast<float32>(base);
        }
        return value;
    }

    /** @brief Build one fixed Halton(2, 3) jitter sample from a phase index. */
    [[nodiscard]] inline RenderTemporalJitter MakeRenderTemporalJitterForPhase(
        uint32 phaseIndex,
        const RenderOutputExtent& extent) noexcept
    {
        RenderTemporalJitter jitter;
        if (phaseIndex >= RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT ||
            !extent.IsValid())
        {
            return jitter;
        }

        const uint32 sampleIndex = phaseIndex + 1U;
        jitter.phaseIndex = phaseIndex;
        jitter.extent = extent;
        jitter.pixelOffset = {
            CalculateRenderHalton(sampleIndex, 2U) - 0.5f,
            CalculateRenderHalton(sampleIndex, 3U) - 0.5f,
        };
        jitter.ndcOffset = {
            2.0f * jitter.pixelOffset.x / static_cast<float32>(extent.width),
            2.0f * jitter.pixelOffset.y / static_cast<float32>(extent.height),
        };
        return jitter;
    }

    /**
     * @brief Select the deterministic phase for a one-based render sequence.
     *
     * Frame one maps to phase zero, which makes capture and offline replay
     * agree without a hidden warm-up frame.
     */
    [[nodiscard]] inline RenderTemporalJitter MakeRenderTemporalJitter(
        uint64 frameSequence,
        const RenderOutputExtent& extent) noexcept
    {
        if (frameSequence == 0U)
            return {};
        return MakeRenderTemporalJitterForPhase(
            static_cast<uint32>((frameSequence - 1U) %
                                RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT),
            extent);
    }

    /** @brief Reject malformed or non-canonical temporal jitter values. */
    [[nodiscard]] inline bool IsValidRenderTemporalJitter(
        const RenderTemporalJitter& jitter) noexcept
    {
        if (!jitter.IsValid())
            return false;

        return jitter == MakeRenderTemporalJitterForPhase(
                             jitter.phaseIndex, jitter.extent);
    }

    /**
     * @brief Apply a pixel-space jitter to a canonical unjittered projection.
     *
     * The matrix layout follows the existing GLM projection convention used by
     * the renderer: the clip translation belongs in column two, x/y rows.
     */
    [[nodiscard]] inline Mat4 ApplyRenderTemporalJitter(
        const Mat4& unjitteredProjection,
        const RenderTemporalJitter& jitter) noexcept
    {
        if (!IsValidRenderTemporalJitter(jitter))
            return unjitteredProjection;

        Mat4 jitteredProjection = unjitteredProjection;
        jitteredProjection[2][0] += jitter.ndcOffset.x;
        jitteredProjection[2][1] += jitter.ndcOffset.y;
        return jitteredProjection;
    }
} // namespace RVX
