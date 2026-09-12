#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <cmath>

namespace RVX
{
    enum class TransparentTemporalMode : uint8
    {
        SortedSurface,
        WeightedBlended,
    };

    enum class TransparentTemporalVelocitySource : uint8
    {
        Opaque,
        Transparent,
    };

    /** Values describe the current rendered surface, not its simulation velocity. */
    struct TransparentTemporalSample
    {
        TransparentTemporalMode mode = TransparentTemporalMode::SortedSurface;
        /** Current-NDC minus previous-presented-NDC, matching opaque velocity. */
        Vec2 motion{0.0f, 0.0f};
        /** Composited color contribution, not geometric/MSAA sample coverage. */
        float32 coverage = 0.0f;
        float32 viewDepth = 0.0f;
        uint64 instanceId = 0;
        uint64 generation = 0;
        uint64 previousInstanceId = 0;
        uint64 previousGeneration = 0;
        uint64 previousSubmittedFrame = 0;
        bool previousShapeAvailable = false;
        bool overlappingLayers = false;
        /** Refracted background motion is not represented by surface motion. */
        bool refractive = false;
        bool discontinuity = false;
    };

    struct TransparentTemporalDecision
    {
        TransparentTemporalVelocitySource source = TransparentTemporalVelocitySource::Opaque;
        Vec2 motion{0.0f, 0.0f};
        float32 reactive = 0.0f;
        bool valid = false;
    };

    /**
     * CPU reference for the first transparent temporal resolve contract.
     * A single, fully covered sorted surface may replace the selected motion;
     * ambiguous mixtures retain opaque motion and reject history by coverage.
     * The opaque velocity attachment itself is never overwritten.
     */
    [[nodiscard]] inline TransparentTemporalDecision ResolveTransparentTemporalSample(
        const TransparentTemporalSample& sample,
        const Vec2& opaqueMotion,
        float32 opaqueViewDepth,
        uint64 currentFrame,
        uint64 lastSubmittedFrame) noexcept
    {
        TransparentTemporalDecision result;
        if (!std::isfinite(opaqueMotion.x) || !std::isfinite(opaqueMotion.y) ||
            !std::isfinite(opaqueViewDepth) || opaqueViewDepth <= 0.0f ||
            !std::isfinite(sample.motion.x) || !std::isfinite(sample.motion.y) ||
            !std::isfinite(sample.coverage) || sample.coverage < 0.0f || sample.coverage > 1.0f ||
            !std::isfinite(sample.viewDepth) || sample.viewDepth <= 0.0f ||
            currentFrame == 0 || lastSubmittedFrame >= currentFrame ||
            (sample.mode != TransparentTemporalMode::SortedSurface &&
             sample.mode != TransparentTemporalMode::WeightedBlended))
            return result;
        result.valid = true;
        result.motion = opaqueMotion;
        if (sample.coverage == 0.0f || sample.viewDepth >= opaqueViewDepth)
            return result;

        const bool exactHistory = sample.instanceId != 0 && sample.generation != 0 &&
            sample.instanceId == sample.previousInstanceId &&
            sample.generation == sample.previousGeneration &&
            lastSubmittedFrame != 0 && sample.previousSubmittedFrame == lastSubmittedFrame &&
            sample.previousShapeAvailable && !sample.discontinuity;
        if (sample.mode == TransparentTemporalMode::SortedSurface &&
            !sample.overlappingLayers && !sample.refractive &&
            sample.coverage == 1.0f && exactHistory)
        {
            result.source = TransparentTemporalVelocitySource::Transparent;
            result.motion = sample.motion;
        }
        else
        {
            result.reactive = sample.coverage;
        }
        return result;
    }
}
