/**
 * @file ModelCameraFraming.cpp
 * @brief Perspective camera framing implementation for model inspection tools.
 */

#include "Samples/ModelCameraFraming.h"

#include "Runtime/Camera/OrbitCameraRig.h"

#include <algorithm>
#include <cmath>
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

        float GetFiniteVectorLength(const Vec3& value)
        {
            if (!IsFiniteVector(value))
            {
                return 0.0f;
            }

            const float largestComponent = std::max(
                std::abs(value.x),
                std::max(std::abs(value.y), std::abs(value.z)));
            if (largestComponent <= 0.0f)
            {
                return 0.0f;
            }

            const Vec3 scaled = value / largestComponent;
            const float length = largestComponent * std::sqrt(
                dot(scaled, scaled));
            return std::isfinite(length) ? length : 0.0f;
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
        const float radius = GetFiniteVectorLength(extent);
        if (!IsFiniteVector(extent) || !std::isfinite(radius) ||
            radius <= std::numeric_limits<float>::epsilon())
        {
            return frame;
        }

        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.verticalFovRadians = verticalFovRadians;
        settings.aspectRatio = aspect;
        settings.fitMargin = fitMargin;
        settings.maxDistance = std::numeric_limits<float>::max();

        OrbitCameraRig rig;
        if (!rig.Initialize(settings) || !rig.Fit())
        {
            return frame;
        }

        const OrbitCameraRigPose pose = rig.GetPose();
        frame.target = pose.pivot;
        frame.distance = pose.distance;
        frame.nearPlane = pose.nearPlane;
        frame.farPlane = pose.farPlane;
        frame.valid = pose.valid;
        return frame;
    }

    ModelCameraClipRange BuildModelCameraClipRange(
        float distance,
        float boundsRadius,
        float nearRadiusMargin,
        float farRadiusMargin)
    {
        const OrbitCameraClipRange rigRange = OrbitCameraRig::BuildClipRange(
            distance,
            boundsRadius,
            nearRadiusMargin,
            farRadiusMargin);
        ModelCameraClipRange range;
        range.nearPlane = rigRange.nearPlane;
        range.farPlane = rigRange.farPlane;
        range.valid = rigRange.valid;
        return range;
    }
} // namespace RVX
