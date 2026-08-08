/**
 * @file ModelCameraFraming.cpp
 * @brief Perspective camera framing implementation for model inspection tools.
 */

#include "Samples/ModelCameraFraming.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <limits>

namespace RVX
{
    namespace
    {
        bool IsFiniteVector(const Vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        }
    } // namespace

    ModelCameraFrame BuildModelCameraFrame(const AABB& bounds,
                                           float aspect,
                                           float verticalFovRadians,
                                           float fitMargin)
    {
        ModelCameraFrame frame;
        if (!bounds.IsValid() || !IsFiniteVector(bounds.GetMin()) ||
            !IsFiniteVector(bounds.GetMax()) || !std::isfinite(aspect) ||
            aspect <= 0.0f || !std::isfinite(verticalFovRadians) ||
            verticalFovRadians <= 0.0f ||
            verticalFovRadians >= 3.14159265f || !std::isfinite(fitMargin) ||
            fitMargin < 1.0f)
        {
            return frame;
        }

        const Vec3 extent = bounds.GetExtent();
        const float radius = glm::length(extent);
        if (!IsFiniteVector(extent) || !std::isfinite(radius) ||
            radius <= std::numeric_limits<float>::epsilon())
        {
            return frame;
        }

        const float halfVerticalFov = verticalFovRadians * 0.5f;
        const float halfHorizontalFov =
            std::atan(std::tan(halfVerticalFov) * aspect);
        const float limitingHalfFov =
            std::max(0.01f, std::min(halfVerticalFov, halfHorizontalFov));

        frame.target = bounds.GetCenter();
        frame.distance =
            (radius / std::sin(limitingHalfFov)) * fitMargin;
        const ModelCameraClipRange clipRange =
            BuildModelCameraClipRange(frame.distance, radius);
        frame.nearPlane = clipRange.nearPlane;
        frame.farPlane = clipRange.farPlane;
        frame.valid = IsFiniteVector(frame.target) &&
                      std::isfinite(frame.distance) && frame.distance > 0.0f &&
                      clipRange.valid;
        return frame;
    }

    ModelCameraClipRange BuildModelCameraClipRange(
        float distance,
        float boundsRadius,
        float nearRadiusMargin,
        float farRadiusMargin)
    {
        ModelCameraClipRange range;
        if (!std::isfinite(distance) || distance <= 0.0f ||
            !std::isfinite(boundsRadius) || boundsRadius <= 0.0f ||
            !std::isfinite(nearRadiusMargin) || nearRadiusMargin < 1.0f ||
            !std::isfinite(farRadiusMargin) || farRadiusMargin <= 0.0f)
        {
            return range;
        }

        const float scaleAwareNearFloor = std::max(
            boundsRadius * 0.001f,
            std::numeric_limits<float>::epsilon());
        range.nearPlane = std::max(
            scaleAwareNearFloor,
            distance - boundsRadius * nearRadiusMargin);
        range.farPlane = std::max(
            range.nearPlane + boundsRadius,
            distance + boundsRadius * farRadiusMargin);
        range.valid = std::isfinite(range.nearPlane) &&
                      std::isfinite(range.farPlane) &&
                      range.nearPlane > 0.0f &&
                      range.farPlane > range.nearPlane;
        return range;
    }
} // namespace RVX
