/**
 * @file ModelCameraFraming.h
 * @brief Aspect-aware perspective camera framing for model inspection tools.
 */

#pragma once

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"

namespace RVX
{
    /** @brief Deterministic camera parameters that enclose a world-space model bound. */
    struct ModelCameraFrame
    {
        Vec3 target{0.0f};
        float distance = 0.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        bool valid = false;
    };

    /**
     * @brief Fit a perspective camera to a world-space AABB.
     * @param bounds Finite world-space model bounds.
     * @param aspect Viewport width divided by height.
     * @param verticalFovRadians Vertical field of view in radians.
     * @param fitMargin Multiplicative margin applied to the enclosing sphere distance.
     * @return Valid framing parameters, or an invalid result for malformed inputs.
     */
    ModelCameraFrame BuildModelCameraFrame(const AABB& bounds,
                                           float aspect,
                                           float verticalFovRadians,
                                           float fitMargin = 1.10f);
} // namespace RVX
