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

    /** @brief Perspective clip range derived from the current orbit distance. */
    struct ModelCameraClipRange
    {
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        bool valid = false;
    };

    /**
     * @brief Fit a perspective camera to a world-space AABB.
     * @param bounds Finite world-space model bounds.
     * @param aspect Viewport width divided by height.
     * @param verticalFovRadians Vertical field of view in radians.
     * @param fitMargin Scale-aware margin applied after the view-basis corner fit.
     * @return Valid framing parameters, or an invalid result for malformed inputs.
     */
    ModelCameraFrame BuildModelCameraFrame(const AABB& bounds,
                                           float aspect,
                                           float verticalFovRadians,
                                           float fitMargin = 1.10f);

    /**
     * @brief Derive scale-aware clip planes for an interactive orbit camera.
     * @param distance Current camera distance from the orbit target.
     * @param boundsRadius Radius of the scene bounds around the orbit target.
     * @param nearRadiusMargin Extra radius kept in front of the nearest bound.
     * @param farRadiusMargin Extra radii kept behind the target.
     */
    ModelCameraClipRange BuildModelCameraClipRange(
        float distance,
        float boundsRadius,
        float nearRadiusMargin = 1.10f,
        float farRadiusMargin = 2.0f);
} // namespace RVX
